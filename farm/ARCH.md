# LankeOS Build Farm 架构

> 本文档由实际代码编写（`farm/src/`），描述**当前实现**的真实行为，而非设计意图。若与代码冲突，以代码为准。

## 1. 总览

LankeOS build farm 是一个 **ABI 驱动的增量包构建系统**：基于 `needed_so`/`provides` 的链接依赖图，检测上游 ABI 断裂，只重建受影响的最小集合。

- **增量**：只构建配方版本与本地 repo 不一致的包，或 ABI 断裂的受害者
- **容器隔离**：所有构建在 fresh docker 容器内进行（禁止主机构建，`--image` 必填）
- **ABI 精确**：用旧索引的 needed_so/provides 算 removed SONAME → 直连受害者，不做树状闭包

```
farm build <pkg>|--all   → 增量选择 → 拓扑排序 → 逐包容器构建 → scan → verify 三分支 → repack/传播 → 进 repo
farm track <pkg> --run   → 探测上游版本 → 更新 LankeBUILD.json
farm gen-trackers        → LLM 批量生成 tracker yaml
farm seed --remote       → 冷启动播种远程 repo（并行下载 + 剥离 needed_so + 建 abidb）
farm serve               → 本地 repo 静态 HTTP 服务器
```

## 2. 模块结构与分层

```
src/
  main.rs          薄入口（cli::run()）
  cli/             CLI 层
    mod.rs         Cli/Command/Args + dispatch + localize_help + track/gen-trackers 逻辑
    build.rs       cmd_build
    serve.rs       cmd_serve
    seed.rs        cmd_seed
  build/           调度层
    mod.rs         run_build 核心（增量选择/队列循环/ABI 传播编排）+ 全部测试
    sched.rs       topo_order（needed_so 拓扑）/ find_cycle_edge / reorder_queue
    prompt.rs      BLOCKED/源缺失的交互接管（开 shell/跳过/结束）
    sources.rs     源预下载（§8.6）
    repo.rs        版本判定/漂移 repack/上传/index 更新/配方读写
  abi.rs           removed_sonames / detect_abi_breaks / propagate
  abidb.rs         farm 自己的 SONAME 数据库（传播基线）
  graph.rs         index.txt 解析 + Index/RevMap + link_deps
  scan.rs          .lpkg 解包 + ELF needed_so/provides 扫描
  repack.rs        metadata.json 漂移修正 + 重打
  seed.rs          冷启动播种
  serve.rs         静态 HTTP 服务器
  state.rs         SQLite 状态库（job 状态/续跑）
  track/           tracker 模板（github/gitlab/sourceforge/gnome/gcs/html-index/script）
  net.rs           HTTP 下载
  lpkg_binding.rs  唯一碰 lpkg 的接缝（docker 编排）
  i18n.rs          l10n（tr! 宏 + zh/en 目录 + LANG 切换）
  ux.rs            ANSI 颜色（非 TTY 自动降级）
  llm.rs           gen-trackers 的 LLM 客户端
  verify.rs        构建产物 vs 期望 metadata 的三分支判定
```

分层：`cli`（参数/命令）→ `build`（调度编排）→ `abi`/`graph`/`abidb`/`scan`/`repack`（逻辑）→ `lpkg_binding`（容器接口）。逻辑层不碰 lpkg，所有 lpkg 交互收敛在 `LpkgBinding` trait（`lpkg_binding.rs`，唯一碰 lpkg 的接缝）。

## 3. 三个依赖字段

每个包的 LankeBUILD.json / repo index 有三类依赖元数据（语义对齐 gen_deps）：

- **`needed_so`**：DT_NEEDED 的 SONAME 列表（运行时链接库，如 `libc.so.6`）
- **`provides`**：本包提供的 SONAME/能力（如 `libmagic.so.1`）
- **`deps`**：包级运行时依赖（由 gen_deps/deprules 规则生成，**farm 不扫不比**）

farm 只扫/比 `needed_so` + `provides`（`build/repo.rs` 规则 3：`deps` 不读不改）。

## 4. build 调度（run_build）

`build/mod.rs::run_build` 主流程：

1. **旧索引基线**：`load_old_index` 读 `abidb::load_index`（`out/<arch>/.abi.json`）。缺失/为空 → 报错（**禁止无基线构建**，`farm seed` 是唯一入口）。
2. **增量选择**：`--all` 时用 `needs_build`（配方 effective_version vs 旧索引）跳过一致的包；指定 `pkg` 强制重建。
3. **拓扑排序**：`sched::topo_order` 只用 needed_so 链接边（build_deps/deps 排除，避免伪环），环切割。
4. **逐包循环**（队列，受害者带 `is_victim` 标记）：
   - 受害者先 `bump_release`（release+1，用户规则 1）
   - 源预下载 → 容器构建（见 §5）
   - `scan` 产物 → `verify::decide` 三分支（见 §6）
   - 漂移 → `repack` 双写（metadata.json + LankeBUILD.json + abidb）
   - 进 repo（`place_in_repo` 命名 `<version>.lpkg`）+ 更新 index（**剥掉 needed_so**，写剥离后哈希）
   - ABI 传播（见 §7）→ 受害者入队 → `sched::reorder_queue` 去重重排

### 排序与 ABI 受害者重排（build/sched.rs）

- `topo_order`：needed_so → provider 的链接图，Kahn 拓扑 + 三色 DFS 环切割。**确定性**（名字升序弹出）。
- `reorder_queue`：受害者入队后按依赖算法重排，**先去重**（同一受害者被多个断裂重复入队 → `rev` 被污染导致顺序错乱）+ victim 标记取 OR。保证"被依赖者先建"（如 librsvg 先于 appstream）且叶子（chromium）**维持队尾、只构建一次**。

## 5. 构建执行（lpkg_binding）

`RealBinding::docker_build`（fresh 容器，`docker create --network=host` + exec）：

1. 常驻容器 `tail -f /dev/null` 保活，`/work` 预建（docker cp 对不存在目录是"铺内容"而非建子目录）
2. 写 `/etc/lpkg/mirror.conf` → `http://127.0.0.1:<repo_port>/`（容器经 host 网络访问内嵌 repo 服务器）
3. `docker cp` 配方（含预下载源）→ `/work/<pkg>/`
4. 容器内脚本：
   ```
   rm -rf /var/lib/lpkg/{deps,needed_so,provides.db}   # 清安装库，SONAME 检查失效
   lpkg install lpkg -y &&                              # 自更新到带 force-solve-conflict 的 lpkg
   ( lpkg upgrade -y || { echo 'I understand...' | lpkg force-solve-conflict -y && lpkg upgrade -y; } ) &&
   lpkg build -y
   ```
5. `docker cp` 产物回宿主 staging

`ContainerGuard` RAII：所有失败路径自动 `docker rm -f`（命名容器 `lankefarm-build`，启动前清残留）。

## 6. 验证三分支（verify.rs）

`verify::decide(actual_scan, expected_meta)`：

- **Unchanged**：needed_so/provides/deps 全一致 → 直接进 repo
- **Repack**（needed_so/deps 漂移）：二进制未变只元数据错 → repack（不 rebuild）
- **AbiBreak**（provides 漂移）：ABI 面变化 → repack 修正 + 传播重建依赖者

provides 漂移优先（ABI 面是最高信号）。

### 交互接管（build/prompt.rs）

BLOCKED 或源预下载失败 → **进程内交互提示，不退出**：
- `1) 开 shell 修复`（宿主 shell 改配方，exit 后重试）
- `2) 跳过此包`（仅 operator 明确选择才跳过）
- `3) 结束构建`

非交互（无 TTY）→ 标记 Blocked 继续，不静默丢弃。

## 7. ABI 检测与传播（abi.rs + abidb.rs）

- **`removed_sonames(old, pkg, new_provides)`**：旧索引 provides − 新扫描 versioned provides → 被移除的 SONAME（ABI 断裂信号）
- **`RevMap`**（graph.rs）：soname → 需要它的包（旧索引 needed_so 反图）
- **`direct_victims(revmap, removed)`**：直接链接被移除 SONAME 的包
- **传播**：只有 SONAME 变化才触发；受害者 release bump + 入队重建；受害者自身的 provides 变化再级联（固定点）

**abidb（`out/<arch>/.abi.json`）**：farm 自己的 SONAME 数据库，存完整 provides + needed_so。**容器可见的 index.txt 剥掉 needed_so**（只留 provides/deps）→ lpkg 的一致性检查失效、构建不被 bootstrap 环卡死；farm 的传播从 abidb 读，两不干扰。每次 repack 三处同步：LankeBUILD.json + metadata.json + abidb。

## 8. seed（冷启动播种）

`seed.rs`：
1. 下载远端 `index.txt` → 解析
2. **abidb 全量写入**（完整 provides + needed_so）
3. 并行（`-j`，默认 CPU 核数）逐包：下载 + SHA256 校验 + **剥离 .lpkg metadata 的 needed_so**（保留 provides）+ 清旧版本
4. index.txt 写剥离版（needed_so 空 + **剥离后 .lpkg 的哈希**）

增量：本地已有该版本且 metadata 已剥 → 跳过下载（省流量）。解包目录随用随清。

## 9. track 系统

`track/mod.rs`：从 LankeBUILD.json 的 source URL 匹配 tracker（`data/trackers/*.yaml`），探测上游最新版本。

- **模板**：`github` / `gitlab` / `sourceforge` / `gnome` / `gcs` / `html-index` / `script`
- **TrackerConfig 字段**：`pkg-name`（必填）、`tracker-template`（必填）、`source-name`（覆盖上游目录名）、`tag-prefix`、`same-version`（锁定某包版本）、`major-version-lock`、`max-version`、`stable-minor`（gnome 的 even/odd）、`order`（after/last 依赖排序）
- **same-version**：读被锁包的已解析版本，`{version}`/`{tag}`/`{name}` 占位符替换（如 SPIRV-Tools/vulkan-loader 锁 vulkan-headers）
- `farm track <pkg> --run` 单包应用；`--all -j N` 并行探测（依赖序门控）

## 10. gen-trackers（LLM 批量）

`cli/mod.rs::cmd_gen_trackers`：LLM 每批 12 个包生成 tracker yaml，`parse_batch_blocks` 解析多个 yaml doc，写 `data/trackers/<pkg>.yaml`。

## 11. 状态模型（state.rs）

SQLite（`out/farm-state.db`，可选 `--state`）：
- `jobs` 表：每包状态（`Building`/`Done`/`Blocked`/`Skipped`）+ 失败阶段 + 配方 hash
- `build_history`：版本 + 成功/失败

支持续跑/差分（配方 hash 变化 → 重新入队）。

## 12. serve（本地 repo HTTP）

`serve.rs`：静态文件服务器，serve `out/` 根（`farm serve --root out --port 8000`）。build 的 docker 模式内嵌一个（`--repo-port`，默认 80）。

## 13. i18n / ux

- **i18n.rs**：`tr!("key")` / `tr!("key", args)` 宏；中文默认，`LANG`/`LC_ALL` 以 `en` 开头切英文；`{}` 运行时替换（`format!` 需字面量，farm 用 `i18n::fmt` 手填）；键缺失回退键名；中英目录键一致性有测试。clap 帮助 `LANG=en` 时覆盖为英文。
- **ux.rs**：ANSI 颜色（成功绿/信息灰/警告黄/错误红），非 TTY 或 `NO_COLOR` 降级纯文本，每个颜色带 `\x1b[0m` reset。

## 14. 数据流图

```
seed ──> out/<arch>/
           index.txt   (needed_so 剥离 + 剥离后哈希，容器可见)
           .abi.json   (完整 provides/needed_so，farm 传播基线)
           <pkg>/<ver>.lpkg  (metadata 剥离 needed_so)

build --all ──> run_build
  needs_build 过滤 → topo_order → 队列
  └─ 逐包: 预下载 → docker build → scan → verify::decide
       ├─ Unchanged → 进 repo
       ├─ Repack    → repack metadata + LankeBUILD + abidb → 进 repo
       └─ AbiBreak  → repack + 传播（removed SONAME → direct_victims → 入队重排）
```

## 15. 测试

- **src 内单元测试**（`#[cfg(test)]`）：内部函数（topo/reorder/scan/i18n/ux/repack/seed）
- **tests/integration.rs**：公共 API 集成（ABI 传播、track 排序、real index）

107 个测试全绿、clippy 0 警告。关键回归：ABI 中链包排序、叶子维持队尾、多断裂去重、坏 symlink repack、index 剥离 + 哈希替换。
