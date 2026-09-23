# LankeOS Build Farm 架构

> 本文档由实际代码编写（`farm/src/`），描述**当前实现**的真实行为，而非设计意图。若与代码冲突，以代码为准。

## 1. 总览

LankeOS build farm 是一个 **ABI 驱动的增量包构建系统**：基于 `needed_so`/`provides` 的链接依赖图，检测上游 ABI 断裂，只重建受影响的最小集合。

- **增量**：只构建配方版本与本地 repo 不一致的包，或 ABI 断裂的受害者
- **容器隔离**：所有构建在 fresh docker 容器内进行（禁止主机构建，`--image` 必填）
- **ABI 精确**：用旧索引的 needed_so/provides 算 removed SONAME → 直连受害者，不做树状闭包
- **单一真源**：`out/<arch>/index.txt` 含**完整 needed_so**，同时供容器可见索引与 farm 的 ABI 传播
- **ABI 过渡备份**：检测到 SONAME 断裂时把旧 SONAME 的 .so 备份到 `out/backups/`（**扁平**，不按包分子目录——同一 SONAME 文件被两个包同时提供本就冲突，跨包同名覆盖无害），每个构建容器内恢复 + ldconfig，让旧二进制在过渡期存活；**整个 build 完成后**清理
- **确定性构建序**：拓扑排序同级包按名字升序固定顺序，绝无随机（有回归测试）
- **构建计划确认**：开始前列出 topo 顺序供 operator 确认；预下载只给"确认集"，ABI 受害者构建时由 lpkg build 自己下载

```
farm build <pkg>|--all   → 增量选择 → 拓扑排序 → 计划预览+确认 → bulk 预下载(确认集)
                          → 逐包容器构建 → scan → verify 三分支 → repack/传播/备份 → 进 repo → 备份清理
farm track <pkg> --run   → 探测上游版本 → 更新 LankeBUILD.json
farm gen-trackers        → LLM 批量生成 tracker yaml
farm seed --remote       → 冷启动播种远程 repo（并行下载 + SHA256 校验；index/.lpkg 原样完整）
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
    mod.rs         run_build 核心（增量选择/计划预览/队列循环/ABI 传播/备份清理编排）+ 全部测试
    sched.rs       topo_order（**needed_so 链接边 + build_deps 依赖边 + 声明式组边** 拓扑，确定性）/ find_cycle_edge / reorder_queue
    groups.rs      data/build/*.yaml 声明式 ABI 重建组（python 生态等不链但 ABI 敏感）
    prompt.rs      BLOCKED/源缺失的交互接管（开 shell/跳过/结束）+ 构建计划预览/确认
    sources.rs     源预下载
    repo.rs        版本判定/漂移 repack/上传/index 更新/备份清理/配方读写
  abi.rs           removed_sonames / detect_abi_breaks / propagate
  graph.rs         index.txt 解析 + Index/RevMap + link_deps
  scan.rs          .lpkg 解包 + ELF needed_so/provides 扫描
  repack.rs        metadata.json 漂移修正 + 重打
  seed.rs          冷启动播种
  serve.rs         静态 HTTP 服务器
  state.rs         SQLite 状态库（job 状态记录；自动 requeue 未实现，见 §11 附注）
  track/           tracker 模板（github/gitlab/sourceforge/gnome/gcs/html-index/multi-level-html-index/script）
  net.rs           HTTP 下载
  lpkg_binding.rs  唯一碰 lpkg 的接缝（docker 编排 + ABI 过渡备份注入）
  i18n.rs          l10n（tr! 宏 + zh/en 目录 + LANG 切换）
  ux.rs            ANSI 颜色（非 TTY 自动降级）
  llm.rs           gen-trackers 的 LLM 客户端
  verify.rs        构建产物 vs 期望 metadata 的三分支判定
```

分层：`cli`（参数/命令）→ `build`（调度编排）→ `abi`/`graph`/`scan`/`repack`（逻辑）→ `lpkg_binding`（容器接口）。逻辑层不碰 lpkg，所有 lpkg 交互收敛在 `LpkgBinding` trait（`lpkg_binding.rs`，唯一碰 lpkg 的接缝）。

## 3. 三个依赖字段

每个包的 LankeBUILD.json / repo index 有三类依赖元数据（语义对齐 gen_deps）：

- **`needed_so`**：DT_NEEDED 的 SONAME 列表（运行时链接库，如 `libc.so.6`）
- **`provides`**：本包提供的 SONAME/能力（如 `libmagic.so.1`）
- **`deps`**：包级运行时依赖（由 gen_deps/deprules 规则生成，**farm 不扫不比**）

farm 只扫/比 `needed_so` + `provides`（`build/repo.rs` 规则 3：`deps` 不读不改；`deps` 不参与构建序、`build_deps` **无条件参与**（仅限本轮 targets 内）——见 §4）。

## 4. build 调度（run_build）

**运行要求**：操作命令（build/validate/abifix/export/seed）**必须以 root 运行**（CLI 层
`ensure_root` 强制；非 root 直接报错）。`.lpkg` 解包/重打包需读写 root 属主文件与 SUID/SGID，
farm **不再 spawn `sudo`/`tar`/`zstd` CLI**——解包走 `zstd`+`tar` Rust crate（`scan.rs::extract_lpkg`，
保留 mode/uid/gid），打包走 `tar::Builder`（`repack.rs::pack_dir_tar`，uid/gid=0、mtime=0、排序遍历
字节可复现）。库层 archive 函数对 root 可选（所有权保留仅 root 时启用），单测非 root 可跑。

`build/mod.rs::run_build` 主流程：

1. **旧索引基线**：`load_old_index` 读 `out/<arch>/index.txt`（**完整 needed_so**，单一真源）。缺失/为空 → 报错（**禁止无基线构建**，`farm seed` 是唯一入口）；旧索引全零 needed_so → 警告重新 seed，否则 ABI 传播失明。
2. **增量选择**：`--all` 时用 `needs_build`（配方 effective_version vs 旧索引）跳过一致的包；指定 `pkg` 强制重建。
3. **拓扑排序**：`sched::topo_order` 按 **needed_so 链接边 ∪ 声明式重建组边（victim → on）∪ build_deps 边** 做 Kahn 拓扑 + 三色 DFS 切环。**确定性**：就绪队列用 `BinaryHeap<Reverse<String>>` 弹名字最小者 → **同级包固定按名字升序**，两次运行逐位一致。`deps` 不参与排序；`build_deps` **无条件进边（仅限本轮 targets 内）**——构建期需要另一个包先产出时必须等它先建（如 python-bar 要 python-foo 本轮重建的产物、gjs 要同轮首建的 sysprof）；指向本轮不重建的包 → 边丢弃。组边保证"不链 libpython 的 python-* 包"也排在 python 之后（见 §4 声明式组）。**切环偏好**：出现环时按 **build_deps → 组边 → needed_so 链接边** 挑边切断——无条件进边会引入"构建工具伪环"（如 glibc ← python/cmake 的 build_deps），这些边正是**该被切**的那类，链接序不会因此被破坏。
4. **计划预览 + 确认**（2.5）：交互模式（stdin 是 tty）列出 topo 顺序（包 + 版本）并让 operator 确认（回车继续 / n 取消）；非交互（CI/测试/脚本）直接开始。
5. **预下载拆分**：确认后**只给确认集** bulk 预下载全部源；ABI 受害者动态入队**不预下载**（构建时由 lpkg build 自己下载）。批量预下载失败不阻塞——循环里每个确认集包会再走一次源就绪门（带交互接管）。
6. **逐包循环**（队列，受害者带 `is_victim` 标记）：
   - 受害者先 `bump_release`（release+1，用户规则 1）
   - 确认集包走源就绪门（已 bulk 预取，幂等）→ 容器构建（见 §5）
   - `scan` 产物 → `verify::decide` 三分支（见 §6）
   - **无条件归一化重打**（level 22 + mtime 1970，复用 scan 解包目录）：漂移与否都重打 → 进 repo 的
     .lpkg 一律发行级/字节可复现；漂移才额外改 metadata.json + 双写 LankeBUILD.json
   - 进 repo（`place_in_repo` 命名 `<version>.lpkg`，取代旧版本前先备份旧 so，见 §7）+ 更新 index（**写回完整 needed_so**）
   - ABI 传播（见 §7）→ 受害者入队 → `sched::reorder_queue` 去重重排
7. **备份清理**：**整个 build 完成后**（而非单包完成）调用 `cleanup_backups`——备份的旧 SONAME 已不再被任何包 needed_so 引用 → 删除（含空根目录）；仍有包被跳过 / BLOCKED 未重建 → 保留，留待下次 build 完成后再清。

### 排序与 ABI 受害者重排（build/sched.rs）

- `topo_order`：**needed_so 链接边 + build_deps 依赖边 + 声明式组边** Kahn + 三色 DFS 环切割。**确定性**：就绪队列弹名字最小者 → 同级按名字升序；`find_cycle_edge` 节点与邻接都排序 → 切环也确定。**切环偏好**（`EdgeKind`）：按 **build_deps(0) → 组边(1) → needed_so 链接边(2)** 挑后向边切断；同一对 (P→D) 同时是多种边时取**最高**优先级（不把链接边降级成可切的 build_deps 边）。理由：链接边是构建序的真相（切了消费者会先于库重建、按旧 ABI 白跑），build_deps 环多是构建工具互赖（gtk4 ↔ sysprof）或无条件进边引入的伪环。**有回归测试锁死（同级升序 + 两次运行一致 + 输入乱序不影响 + 组受害者排触发包之后 + 切环优先挑 build_deps/组边、只有链接边时才切链接边）**。
- `build_deps` 边（`src/build/sched.rs`）：读配方 LankeBUILD.json，`build_deps` 里某依赖 D 当且仅当 **D 在本轮 targets** 时作为边 P→D 入图——**无条件**（原 `BUILD_AFTER_BUILD_DEPS` flag 已删除，该行为已成默认；配方里再写它会得到"未知 farm flag"告警）。与链接/组边同规则，只对 targets 内生效。**有回归测试锁死（gjs→sysprof 先建，无论 sysprof 在不在旧索引；D 不在 targets 时不加边、维持名字升序）**。

### 声明式 ABI 重建组（build/groups.rs，data/build/*.yaml）

构建序只看 needed_so → "不链 libpython 但 ABI 敏感"的包（python-cairo/gobject/blueman/meson…）没有链接边、不会自动成为 ABI 受害者。用**声明式 YAML** 声明强制重建。**组受害者必须排在触发包之后**：`topo_order` 把组边（victim → on）与 needed_so 链接边同等入图（`groups.trigger_edges_in`），否则 `--all` 初始队列里 python-cairo 会在 python 重建前构建（容器 `lpkg upgrade` 时本地 repo 还是旧 python，构建基于旧 ABI 白跑）。

```yaml
# data/build/python.yaml
rebuild-on-abichange: python
packages: python-* meson gobject-introspection blueman   # 空格分隔的 `*` glob
```

`rebuild-on-abichange` 包触发时，`groups.victims_for` 把匹配 `packages` glob 的配方包并入重建受害者集（与 `direct_victims` 并集、去重、排序入队，release bump + 重建）。

**触发语义（用户规则）**：
- 有版本化 SONAME 的包（python…）→ **只在 SONAME 断裂时**触发（removed_sonames 非空）
- 无版本化 SONAME 的纯脚本解释器（perl…）→ **任何重建**都算运行时变化 → 触发（ABI 信号不存在，靠这个补）

目录缺失/为空 → 空组（无害）。与 data/trackers 同一套 YAML 模式。
- `reorder_queue`：受害者入队后按依赖算法重排，**先去重**（同一受害者被多个断裂重复入队 → `rev` 被污染导致顺序错乱）+ victim 标记取 OR。保证"被依赖者先建"（如 librsvg 先于 appstream）且叶子（chromium）**维持队尾、只构建一次**。

**version-change 组动态受害者（Qt 私有 API 场景）**：version-change 与 ABI breaking **不同**——ABI 断裂
不可预知（要等某包重建、SONAME 变了才知道受害者），只能运行时动态入队；version-change 在 **run_build
开工前即定**（on 包旧索引版本 vs 配方版本一比较就知会触发），所以受害者在**构建概览/确认阶段就算好并
并入初始队列**：进概览、bulk 预下载、release bump，不是边建边冒（`--manual-sort` 严格手工顺序时不并入）。
run_build 给脚本传 `pkgs_dir/out_dir/arch` 并 prepend 两个 farm 导出**纯 bash 函数**——`farm_pkg_list`
（全部配方包名）、`farm_pkg_extract <pkg> <dir>`（解该包当前 .lpkg，用宿主 `zstd|tar`）；脚本可**自己
扫**出受害者打到 stdout（farm 并入静态 glob 并集，排除 on 自身）。脚本运行时刻即私有 ABI 断裂时刻，
无需时间戳。例 `data/build/qt.yaml`：`rebuild-on-version-change: qt6-base` + 脚本对每个非 qt6-* 包
extract 后 `grep -rla "Qt_6_PRIVATE_API"` 命中即 echo 包名。这覆盖 **Qt 私有 API 不随 patch 保 ABI**
（6.11.1 头编的 consumer 跑 6.11.2 私有布局会崩）这类 needed_so/SONAME/符号版本扫描都看不见的漂移。

## 5. 构建执行（lpkg_binding）

`RealBinding::docker_build`（fresh 容器，`docker create --network=host` + DooD socket 挂载 + exec）：

1. 常驻容器 `tail -f /dev/null` 保活，`/work` 预建（docker cp 对不存在目录是"铺内容"而非建子目录）
2. 写 `/etc/lpkg/mirror.conf` → `http://127.0.0.1:<repo_port>/`（容器经 host 网络访问内嵌 repo 服务器）
3. `docker cp` 配方（含预下载源）→ `/work/<pkg>/`
4. ABI 过渡备份注入：`docker cp out/backups`（扁平，文件直接是 `<soname>.so.*`）→ 容器 `/backups`（若有）；容器内 `cp -a /backups/. /usr/lib/ && ldconfig`
5. 容器内脚本（**无任何 rm -rf 状态清空 hack**；SONAME 检查真实运行，过渡期由 flag 显式容忍）：
   ```
   cd /work/<pkg> && \
   lpkg install lpkg -y && \
   ( lpkg upgrade -y --missing-so-no-error || { echo 'I understand that this may break my system.' | lpkg force-solve-conflict && lpkg upgrade -y --missing-so-no-error; } ) || exit 1 ; \
   [ -d /backups ] && cp -a /backups/. /usr/lib/ && ldconfig ; \
   lpkg build -y --use-system-soname
   ```
   - `upgrade` 失败（含 force-solve-conflict 重试）→ `|| exit 1` 致命，不继续 build
   - `--missing-so-no-error`：bootstrap/过渡期容忍缺失 SONAME（lpkg 前向 `check_forward_soname_integrity` 与后向 `check_needed_so_consistency` **都纳入该 flag**；真实系统不带 flag 仍硬抛，不变量保留）
   - 备份恢复后 `--use-system-soname`：build 的 needed_so 检测命中 /usr/lib 里备份的旧 .so
   - **dev symlink（`xxx.so`）指向新 so → 新构建完美链接新 so；旧二进制链旧 SONAME 在过渡期加载备份的旧 so**——新旧两条 ABI 并行存活到全部重建完
6. `docker cp` 产物回宿主 staging

`ContainerGuard` RAII：所有失败路径自动 `docker rm -f`。容器名唯一（`lankefarm-build-<pid>-<pkg>`，
> 并发 build 进程互不踩踏）；启动前只清理"创建者 PID 已死"的孤儿容器（SIGKILL/断电，RAII 未执行时）。

## 6. 验证三分支（verify.rs）

`verify::decide(actual_scan, expected_meta)`（`build/repo.rs::repack_if_drift` 复用，单一判定源）：

- **Unchanged**：needed_so/provides 全一致 → 直接进 repo
- **Repack**（needed_so 漂移）：二进制未变只元数据错 → repack（不 rebuild）
- **AbiBreak**（provides 漂移）：ABI 面变化 → repack 修正 + 传播重建依赖者

provides 漂移优先（ABI 面是最高信号）。

**`deps` 不参与判定**：deps 由 gen_deps/deprules 规则生成，farm 不扫不比（`BuildOutcome.deps` 恒空，
`ScanResult.deps` 保留但 `decide` 不读）。

**xattr 保留：做**（原"明确不做"的决策已作废——它假定 tar 的 Builder 不能写 PAX xattr，而
`Builder::append_pax_extensions` 就在 `tar::pax` 里且无 feature 门控）。两侧对称：打包
（`repack.rs`）把每个条目的 xattr 写成 PAX `SCHILY.xattr.<name>`，解包（`scan.rs::extract_lpkg`）
开 `set_unpack_xattrs(true)` 还原。**丢 `security.capability` 是功能性损坏**（systemd native 二进制、
ping 这类靠文件能力提权的程序会失效），SUID/SGID 是另一套机制、覆盖不到它。
细节：值按**原始字节**走（capability 是二进制结构）；xattr 按名排序后才写（保"同一内容两次打包
字节一致"）；悬空符号链接上 `llistxattr` 返回 `ENOENT`、不支持 xattr 的文件系统返回 `EOPNOTSUPP`，
这两类是"本来就没有 xattr"（不是读失败），其余错误致命。

### 交互接管（build/prompt.rs）

BLOCKED 或源预下载失败 → **进程内交互提示，不退出**：
- `1) 开 shell 修复`（宿主 shell 改配方，exit 后重试）
- `2) 跳过此包`（仅 operator 明确选择才跳过）
- `3) 结束构建`

非交互（`interactive=false`，由 CLI 入口 `stdin().is_terminal()` 决定）→ 标记 Blocked 继续，不静默丢弃。

## 7. ABI 检测与传播（abi.rs）

- **`removed_sonames(old, pkg, new_provides)`**：旧索引 provides − 新扫描 provides（ABI 面 = 版本化 `.so.*` **+ 无 SONAME 实体库**如 tcl 的 `libtcl8.6.so`/expect 的 `libexpect5.45.4.so`；dev symlink 与虚拟提供排除）→ 被移除的 SONAME（ABI 断裂信号）
- **`RevMap`**（graph.rs）：soname → 需要它的包（旧索引 needed_so 反图）
- **`direct_victims(revmap, removed)`**：直接链接被移除 SONAME 的包
- **`groups.victims_for(on, all_pkgs)`**：data/build/*.yaml 声明式重建组（不链但 ABI 敏感，见 §4）
- **传播**：只有 SONAME 变化才触发；受害者 = `direct_victims` ∪ `groups.victims_for`（并集、去重、排序）；受害者 release bump + 入队重建；受害者自身的 provides 变化再级联（固定点）

**index.txt 是单一真源**：完整 needed_so/provides 同时供容器可见索引与 farm 传播（removed_sonames / revmap / link_deps / 备份清理）。lpkg 的 SONAME 检查在容器里真实运行，过渡期由 `--missing-so-no-error` / `--use-system-soname` 显式容忍（见 §5）。

### ABI 过渡备份机制

- **触发（`backup_removed_sonames`）**：`place_in_repo` 取代旧 .lpkg 时，计算 `removed = 旧 provides − 新 provides`（**只备份旧提供、新打包消失的 SONAME**），从旧包中把属于这些 SONAME 的文件备份到 `out/backups/`：版本化 `.so.*`（SONAME 本体 + 实体；精确 `r.` 前缀匹配，不误吞 `libfoo.so.20`）+ **无 SONAME 的运行时库**（如 tcl 的 `libtcl8.6.so`、expect 的 `libexpect5.45.4.so`，文件名即身份）。**符号链接保留本身**（ldconfig 要求版本化 SONAME 是符号链接，否则报 dirty），并**复刻目录树**备份其指向的实体：`/usr/lib/xxx.so.x → xxx/xxx.so.x.x` ⇒ `out/backups/xxx.so.x`（symlink）+ `out/backups/xxx/xxx.so.x.x`（实体）。**绝对目标容错**：指向 `/usr/lib/xxx`（或 lib/usr/lib64/lib64）时在 archive 里定位（content/ → /），符号链接转为相对路径（相对备份树根 /usr/lib）。dev symlink（`xxx.so` 指向版本化文件）归新包，不备份。扫全部系统库目录（usr/lib、lib、usr/lib64、lib64），同名覆盖去重。
- **注入（`lpkg_binding`）**：每个构建容器启动后把备份 cp 进 `/usr/lib` 并 `ldconfig` 刷新缓存——dev symlink 仍指向新 so，新构建链新 so，旧二进制链旧 so 且能按 SONAME 命中 ld.so.cache。
- **清理（`cleanup_backups`）**：**整个 build 完成**后扫描 `out/backups/`（只清扁平 `<soname>.so.*` 文件），某备份的 SONAME 已无任何包 needed_so 引用 → 删除（含空根）；仍有引用（有包跳过/BLOCKED）→ 保留。index.txt 不可读/为空/全零 needed_so → 保守保留，绝不误删。

## 8. seed（冷启动播种）

`seed.rs`：
1. 下载远端 `index.txt` → 解析（含**完整 needed_so**）
2. 并行（`-j`，默认 CPU 核数）逐包：下载 + **SHA256 校验** + 清旧版本（本地已有该版本 → 增量跳过下载）
3. 本地 `index.txt` **原样保留**（不重写哈希），`.lpkg` **不重打**——单一真源，容器与 farm 共用

## 9. track 系统

`track/mod.rs`：从 LankeBUILD.json 的 source URL 匹配 tracker（`data/trackers/*.yaml`），探测上游最新版本。

- **模板**：`github` / `gitlab` / `sourceforge` / `gnome` / `gcs` / `html-index` / `multi-level-html-index` / `script`
- **`multi-level-html-index`（N 级目录逐级进，版本藏在路径里）**：`levels` 每级 `{name, url, pattern}`。
  **级名即占位符**（`{series}` 只能在**后续级**的 url 与 template 里引用）；**必须且只能有一级叫 `version`**
  ——它的捕获即包版本（**按名字定，不按位置**）。位置隐式的 `{v1}..{vN}` **已废弃**：引用它会得到
  "未知占位符"报错（原先"靠级序号猜语义"的坑）。校验在探测前全部做完：级名必填/非空/唯一、不得取保留名
  `name`（上游名占位符）、前向引用与未知占位符（**含 `levels[i].url`**，原先只查最终 URL）一律报错。
  例（KDE frameworks，73 个 kf-* tracker 同形）：
  ```yaml
  levels:
  - name: series      # {series} = 6.11
    url: https://download.kde.org/stable/frameworks/
    pattern: href="([0-9][0-9.]*)/"
  - name: version     # 保留名：= 包版本 6.11.0
    url: https://download.kde.org/stable/frameworks/{series}/
    pattern: ki18n-([0-9][0-9.]*)\.tar\.xz
  template: https://download.kde.org/stable/frameworks/{series}/ki18n-{version}.tar.xz
  ```
- **`script`（条目级逃生舱，与其他模板平级）**：stdout 每行 `<版本>|URL`，**默认恰好一行**
  （多行报错——故意的，能让"忘了改格式"的漏迁移立刻炸出来而不是被静默截断）；声明 `expand: true`
  时才允许多行，每行 = **同一列表里的一个连续槽位**（上游动态枚举，如 libreoffice-i18n 的 123 个
  langpack，数量随上游变）。条目声明在哪个列表就填哪个列表。只认 `script`/`expand`/`version-var`
  三个字段（连 `major-of`/`max-version` 都不认——脚本自己过滤版本）。
- **`version-var`（条目间的版本派生）**：`{变量名: 选择器}`，把**已探测**槽位的版本注入为脚本
  环境变量。`version-var: {main: sources[0]}` → 脚本里 `$main` 即 `sources[0]` 本轮解析出的版本。
  存在的理由：一个条目的**内容派生自**另一条目时（libreoffice 的 vendor 文件名来自主源里的
  `download.lst`），若各自重探上游，两次探测之间上游发新版就会产出**版本不一致的清单**
  （主源 26.8.0.3 + vendor 26.8.0.4 → 构建必坏；该条目要下载 300MB tarball，窗口是分钟级）。
  **只能引用位于它之前的槽位**（`sources` 先于 `work_sources` 探测，列表内从左到右；与
  multi-level「只能引用前面的级」同规则），前向/自引用在探测时报错并说明可用范围。
- **包级字段**（`TrackerConfig`）：`pkg-name`（必填）、`version-source`（`sources[i]` /
  `work_sources[i]`，缺省 `sources[0]`、空则 `work_sources[0]`）、`after` / `last`（依赖排序）、
  `sources` / `work_sources`。**没有包级 `type`**——`type: script` / `script-content` 已废弃；
  字段仍被解析但只用于给出**明确的迁移错误**（否则 `deny_unknown_fields` 只吐 `unknown field`）。
- **版本筛选：各模板共享的单一汇点**（`templates::VersionFilter`）。所有探测模板的候选版本一律
  先过这一层，再谈"稳定版优先 → 取最大"：
  - `major-of` / `major-version-lock`（主版本）、`max-version`（数值封顶）；
  - **`exclude`（版本黑名单，正则）**——作用于**提取出的版本字符串**，命中即整条候选丢弃。
    上游混着历史异常 tag 时用：uasm 的 `v213`（旧命名法，按版本比较 `213 > 2.57` 会被误选）、
    cython 的 `3.3.0b1`（PEP 440 预发布，`is_stable` 只认 rc/beta 这类**单词**，认不出裸 `bN`）、
    intel-media-driver 的 `600`。
  - **`stable-minor: even`**——只保留 minor（第二段）为偶数的候选，**全被滤掉时退回全部**
    （与 GNOME 同款兜底：上游偶尔没有偶数 minor 的稳定分支时不该直接探测失败）。GNOME 惯例
    （pango/vala/perl 等开发分支与稳定分支同号段并存）。
  - **加约束请加在这一层**：历史上 `max-version` 只有部分模板支持，正是"各写各的"造成的漂移。
    `script` / `same-version` 不参与（前者自带逻辑、后者不探测）。
- **条目字段**（`SourceConfig`）：`tracker-template`（必填）、`script` + `expand` + `version-var`（仅 script）、
  `source-name`（覆盖上游目录名）、`repo` / `host` / `project` / `url` / `pattern` / `levels` /
  `template`、`tag-prefix` / `mode`、`same-version-of`、`major-of` / `major-version-lock` /
  `max-version` / `exclude` / `stable-minor`。白名单逐模板校验，越界即报错。
- **same-version**：读被锁包的已解析版本，`{version}`/`{tag}`/`{name}` 占位符替换（如 SPIRV-Tools/vulkan-loader 锁 vulkan-headers）
- `farm track <pkg> --run` 单包应用；`--all -j N` 并行探测（依赖序门控）

## 10. gen-trackers（LLM 批量）

`cli/mod.rs::cmd_gen_trackers`：LLM 每批 12 个包生成 tracker yaml，`parse_batch_blocks` 解析多个 yaml doc，写 `data/trackers/<pkg>.yaml`。

## 11. 状态模型（state.rs）

SQLite（`out/farm-state.db`，可选 `--state`）：
- `jobs` 表：每包状态（`Building`/`Done`/`Blocked`/`Skipped`）+ 失败阶段 + 配方 hash
- `build_history`：版本 + 成功/失败

> ⚠️ 现状：**只有写端**（`set_job`/`record_build`），`job_recipe_hash`/`list_by_status` 等读端
> 尚无调用方。**"配方 hash 变化自动 requeue"尚未实现**——BLOCKED 包需 operator 手动
> `farm build <pkg>` 重跑。失败路径（source 缺失 / repack / repo / index 失败）也会
> `set_job(Blocked)` 落库，job 不会永久停在 `Building`。若将来实现差分 requeue，读端已就绪。

## 12. serve（本地 repo HTTP）

`serve.rs`：静态文件服务器，serve `out/` 根（`farm serve --root out --port 8000`）。build 的 docker 模式内嵌一个（`--repo-port`，默认 80）。只接受 GET/HEAD（其余 400）；**HEAD 仅回头、无 body**（RFC 7231，`Content-Length` 仍为 body 长度）；路径穿越由组件级 `..` 拒绝 + `canonicalize` 双重兜底。

## 13. i18n / ux

- **i18n.rs**：`tr!("key")` / `tr!("key", args)` 宏；中文默认，生效 locale 按 gettext/POSIX 优先级 **`LC_ALL` > `LC_MESSAGES` > `LANG`**（空值视为未设），其值以 `en` 开头切英文；`{}` 运行时替换（`format!` 需字面量，farm 用 `i18n::fmt` 手填）；键缺失回退键名；中英目录键一致性有测试。clap 帮助 `LANG=en` 时覆盖为英文。
- **ux.rs**：ANSI 颜色（成功绿/信息灰/警告黄/错误红），非 TTY 或 `NO_COLOR` 降级纯文本，每个颜色带 `\x1b[0m` reset。

## 14. 数据流图

```
seed ──> out/<arch>/
           index.txt   (完整 needed_so/provides/deps，单一真源)
           <pkg>/<ver>.lpkg  (完整 metadata，含 needed_so)
           backups/    (ABI 断裂备份的旧 .so，扁平 <soname>.so.*，整个 build 后清理)

build --all ──> run_build
  needs_build 过滤 → topo_order(确定性，三类边) → 计划预览+确认 → bulk 预下载(确认集)
  └─ 逐包: 源就绪门(非victim) → docker build(upgrade --missing-so-no-error + 备份恢复 + build --use-system-soname)
           → scan → verify::decide
       ├─ Unchanged → 进 repo → index 更新(完整 needed_so)
       ├─ Repack    → repack metadata + LankeBUILD → 进 repo → index 更新
       └─ AbiBreak  → 备份旧 so + repack + 传播(removed SONAME → direct_victims → 入队重排)
  完成后: cleanup_backups(无 needed_so 引用则删)
```

## 15. 测试

- **src 内单元测试**（`#[cfg(test)]`）：内部函数（topo/reorder/scan/i18n/ux/repack/seed/repo/chk）
- **tests/**：公共 API 集成——`integration.rs`（ABI 传播/track 排序/real index）、
  `custom_checks_integration.rs`（qml/pkgconf/pkg-err/hook + 缓存 + hook 注释回归）、
  `architecture_guards.rs`（分层守护：docker 只在 binding 叶 spawn、net 必设读写超时）、
  `docker_binding_sequence.rs`（假 docker 影子脚本锁定 docker 子命令序列，拆步重构的行为不变证据）

**231 个测试全绿**（204 lib + 11 bin + 16 integration——`cargo test` 实测；编译 0 告警）。关键回归：ABI 中链包排序、叶子维持队尾、多断裂去重、坏 symlink repack、**同级构建顺序确定（名字升序、两次运行一致、输入乱序不影响）**、**ABI 受害者跳过预下载（确认集 bulk 预取）**、**备份清理（无引用删 / 有引用留）**、**声明式重建组（python ABI 断裂 → 不链 libpython 的 python 生态包被重建；perl 无 SONAME → 任何重建都触发 xml-parser 重建）**、index 写回完整 needed_so（单一真源）、**seed 半文件/损坏包不被接受**、**依赖环 track 不崩溃**、**repack 失败不静默发布**、**vercmp alpha 后缀（`1.0beta > 1.0`）**、**注释掉的 hook 调用 / QML import 不误判**、**docker 拆步后子命令序列不变**、**HTTP 读超时（无应答连接秒级失败）**。

## 16. ABI 符号/版本审计（`custom_checks/abi`，`farm chk abi`）

`src/custom_checks/abi.rs`（原顶层 `abi_fullchk.rs` / `manual-abi-fullchk`）镜像 `/tmp/scan_elf_ver.py` 的两段式审计，但原生/缓存/单趟：
- **provider/cache 来源 = `--source`（默认 out）下全部包**：每个 .lpkg 解包一次到临时目录，逐 ELF 用
  goblin 读 `.gnu.version_d/.gnu.version_r/.gnu.version`（verdef/verneed/versym）。**整包缓存**
  `~/.cache/lankefarm/abi/<pkg>.json`（key = 当前 `.lpkg` 文件 sha256 + 本检則 `schema`；内容为该包
  每个 ELF 的 soname/needed/defined/undef）。sha 命中 → 整包不重扫、不解包；不匹配 → 原生重扫并覆写。
  **无 readelf/objdump、无新 Cargo 依赖**。
- **`--pkgs` 只收窄审计范围**：consumer（可执行/未命中缓存的库）引用 `name@version` 无任何 DT_NEEDED
  候选库（provider 目录内）提供 → 按包报告。候选为空跳过（无法判断，同 python）。
- 确定性：包/文件排序遍历；provider 冲突（同 SONAME 多份）最后一次胜。consumer 判缺失在 provider
  目录建全后统一做。

## 17. `farm chk`：LankeOS 维护检則工具集（qml/pkgconf/pkg-err/introspection/vapi/build-deps/pycache/hook/abi/full）

`src/custom_checks/` 一族的策略/打包检测（非 farm 核心 ABI），参考 abichk（§16）的架构与缓存思路，
由 `farm chk full` 一并跑（或 `farm chk <kind>` 单跑；ABI 审计 §16 同属此工具集）。逐包一次解包；
**整包缓存 key = `.lpkg` 文件 sha256 + 本检則的 `schema` 常量**（不是名字/版本），命中即复用分析。
缓存根 `~/.cache/lankefarm/<chk>`（无 HOME 回落 `--source/.abi-cache`）——**单跑与 `farm chk full` 共用同一根**
（曾两处路径不一致、同一检則互不命中）；`schema` 由每个检則模块自持，改一类分析只失效该类缓存。判定：
- qmlchk/pkgconfchk：`import`/`Requires` 模块的**归属包** ∈ 本包 binpkg `deps`（仓库 index 记录的运行时
  依赖，`binpkg_deps`；**不读 LankeBUILD.json**）∪ `needed_so` 推导链接依赖（`graph::link_deps`）即满足。
  三段判定：闭包内命中 → 通过；仓库内其它包提供但不在闭包 → Warning（少依赖）；**仓库内无任何 provider
  → Critical（真缺口）**——引擎/进程内注册型 QML 模块（仓库无 qmldir 属正常）用配方 `QML_CHK_IGN_LST`
  显式豁免。qmlchk 扫描前**剥离 QML 注释**（`//` 与 `/* */`，跳过字符串字面量）——注释掉的 `import`
  不再误报。
- pkg-errchk：`usr/etc`|`usr/var` 错位、`.la`、`.a`（静态库一般应删；.cmake 引用的包用
  `IGNORE_CHK_PKGERR` 豁免）。
- introspectionchk/vapichk：装 `.gir` 的包，配方须声明 `gobject-introspection` 构建依赖；装 `.vapi`
  的须声明 `vala`——两类文件都由**构建期**工具（`g-ir-scanner` / `valac`）生成。**读配方 `build_deps`**
  （构建期依赖，区别于上面读 index 运行时 deps 的两条）；缺失 → **Critical**。`pkg == 工具包`自满足
  （`vala`/`gobject-introspection` 自带自己的产物）。
- build-deps chk：`needed_so` 的每个 SONAME，其提供者必须**直接**写在本包 `build_deps`——
  **被别的 build_dep 传递满足也不算**（`needed_so` 是直接链接依赖，build_deps 要写全）；
  `base`/`base-devel` 的**直接** `deps` 并集覆盖的提供者视为满足（铁律：这些包不该写进 build_deps）。
  自提供、仓库内无 provider 的 SONAME 跳过（后者归 `farm abifix`）。**只读配方 `LankeBUILD.json`**
  （SONAME→provider 关系取自各包 `provides` 字段，不扫 `.lpkg`），因此**不占缓存**。
- pycachechk：包内不得含 Python 字节码——① `__pycache__` 目录（**按目录报**：`file` = 该目录，附条目数；
  一个目录里动辄上百 `.pyc`，逐文件报会刷屏，且删除单位就是目录；**遍历目录而非只扫文件**，空的
  `__pycache__` 也算，tar 保留空目录）；② `__pycache__` 之外的**散落 `.pyc`/`.pyo`**（旧式布局，
  **逐文件报**）。两类都是构建期生成、含绝对路径、不可复现。
- hookchk：建了 `usr/lib/sysusers.d`/`tmpfiles.d` 的包，其 `hooks/postinst.sh` 必须调
  `systemd-sysusers`/`systemd-tmpfiles --create`；**只认非注释行**（行首可选空白后 `#` 视为注释）——
  注释里出现字样不算已调用。
配方 `farm_flags` 支持 `IGNORE_CHK_ABI/QML/PKGCONF/PKGERR/INTROSPECTION/VAPI/BUILDDEPS/PYCACHE/HOOK`（大写）整包豁免；列表值 flag
（如 `QML_CHK_IGN_LST`）**只认 JSON 数组对象成员** `{"QML_CHK_IGN_LST": ["a","b"]}`（`NAME=a,b` 裸串
已废弃，会告警）。这些 flag 已在 `build/farm_flags.rs` 注册（topo 解析不告警）。检則都非 root、只读。
