[English](README.en.md) | 中文

<h1 align="center">lankefarm</h1>

<p align="center">
  <strong>LankeOS 的 ABI 驱动增量包构建系统（Rust 编写）。</strong>
  <br />
  <em>ABI 断裂检测 · 增量重建 · 容器隔离 · 确定性拓扑排序</em>
</p>

<p align="center">
  <a href="#使用方法"><img src="https://img.shields.io/badge/Quick_Start-4CAF50?style=for-the-badge" alt="Quick Start" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv3-2d8cf0?style=for-the-badge" alt="License" /></a>
</p>

<p align="center">
  <img src="https://img.shields.io/github/actions/workflow/status/Wtada233/LankeOS/farm-build.yml?style=flat&logo=github-actions&logoColor=white" alt="Build status" />
  <img src="https://img.shields.io/badge/Rust-000000?style=flat&logo=rust&logoColor=white" alt="Rust" />
  <img src="https://img.shields.io/github/license/Wtada233/LankeOS?style=flat" alt="License" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Docker-2496ED?style=flat&logo=docker&logoColor=white" alt="Docker" />
  <img src="https://img.shields.io/badge/SQLite-003B57?style=flat&logo=sqlite&logoColor=white" alt="SQLite" />
  <img src="https://img.shields.io/badge/zstd-1E5B8C?style=flat&logo=zstandard&logoColor=white" alt="zstd" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Claude_Code-D97757?style=flat&logo=claude&logoColor=white" alt="Claude Code" />
</p>

`lankefarm` 是 LankeOS 的 **ABI 驱动增量包构建系统**（Rust 编写）。它基于每个软件包的 `needed_so`/`provides` 构建链接依赖图，检测上游 ABI 断裂，只重建受影响的最小集合，并在隔离的 Docker 容器内完成所有构建。

> 详细架构规范见 [ARCH.md](ARCH.md)。该文档由实际代码编写，描述当前实现的真实行为，若与代码冲突以代码为准。

## 功能特性

- **ABI 驱动增量构建** — 只构建配方版本与本地仓库不一致的包，或 ABI 断裂的受害者。用旧索引的 `needed_so`/`provides` 计算 removed SONAME，直连受害者，不做树状闭包。
- **容器隔离构建** — 所有构建在 fresh Docker 容器内进行，`--image` 必填，禁止主机构建污染宿主环境。
- **确定性构建序** — 拓扑排序，同级按包名升序固定顺序，绝无随机（有回归测试保证）。
- **构建计划确认** — 开始前列出 topo 顺序供 operator 确认；源预下载只针对"确认集"。
- **ABI 过渡备份** — 检测到 SONAME 断裂时把旧 `.so` 备份到 `out/backups/`，在每个构建容器内恢复并 `ldconfig`，让旧二进制在过渡期存活，整个 build 完成后清理。
- **上游版本追踪** — `track` 探测上游版本并更新 `LankeBUILD.json`（默认只读出提案，`--run` 应用）；`gen-trackers` 批量调用 LLM 生成 tracker yaml。
- **冷启动播种** — `seed --remote` 并行下载远程仓库并做 SHA-256 校验，index/`.lpkg` 原样完整落地。
- **双语 CLI** — 完整的 zh/en l10n，ANSI 颜色在非 TTY 下自动降级。

## 使用方法

> **运行要求**：操作命令（`build`/`validate`/`abifix`/`export`/`seed`）**必须以 root 运行**——
> `.lpkg` 解包/重打包需读写 root 属主文件与 SUID/SGID（farm 不再经 `sudo` 降级）。用 root 用户或
> `sudo farm …` 执行。`track`/`serve` 不需要 root。

### 增量构建全部待重建的包

```bash
lankefarm build --all --image wtada233/lankeos:latest
```

### 强制重建指定包

```bash
lankefarm build bash curl --image wtada233/lankeos:latest
```

### 校验缺失构建标记的包

```bash
lankefarm validate --image wtada233/lankeos:latest
```

### 追踪上游版本

```bash
lankefarm track bash --run          # 只读提案: 去掉 --run
lankefarm track --all --run         # 批量应用
```

### 冷启动播种远程仓库

```bash
lankefarm seed --remote https://lankerepo.wtada233.top
```

### 本地仓库 HTTP 服务

```bash
lankefarm serve --root out --port 8000
```

## Tracker 配置（data/trackers/\<pkg\>.yaml）

`farm track` 用 `data/trackers/<pkg>.yaml` 定义包的上游版本来源。tracker 是 **sources / work_sources 的完整清单**：逐条声明式探测，探测成功且版本变新时，LankeBUILD.json 的 `sources`/`work_sources` 被**原子全量替换**（空列表也写键；任一条失败整包不更新）。

```yaml
# 单源包：github tags 探测
pkg-name: systemd
version-source: sources[0]        # 显式声明版本来自哪条
sources:
  - tracker-template: github
    repo: systemd/systemd
    mode: tags
    tag-prefix: v
    template: https://github.com/{repo}/archive/refs/tags/{tag}.tar.gz
```

模板：`github` `gitlab` `html-index` `gnome` `gcs` `sourceforge` `pypi` `same-version`（直接锁定另一包版本）`script`（内嵌 bash）。每模板只接受自己的字段——设置不支持的（如 github + `max-version`）或拼错字段名（如 `tag-prefx`）都报错，不静默忽略。

`script` 是**条目级**逃生舱（与其他模板平级，不是包级类型）：一个脚本产一个 source/work_source 槽位，stdout **恰好一行** `<版本>|URL`。模板覆盖不了时才用——脚本不可复用、无法统一校验：

```yaml
pkg-name: ant
version-source: sources[0]
sources:
  - tracker-template: script
    script: |
      page=$(curl -fsSL "https://www.apache.org/dist/ant/source/")
      ver=$(printf '%s\n' "$page" | grep -oE 'apache-ant-[0-9.]+-src\.tar\.bz2' | sed 's/apache-ant-//; s/-src.tar.bz2//' | sort -V | tail -n1)
      test -n "$ver" || exit 1
      echo "$ver|https://www.apache.org/dist/ant/source/apache-ant-$ver-src.tar.bz2"
```

上游**动态枚举**（目录里有几个文件不确定）时加 `expand: true`：stdout 每行一个 `<版本>|URL`，逐行对应一个连续槽位（如 libreoffice-i18n 的 123 个 langpack，数量随上游变）。**默认多行会报错**——这是故意的，能立刻暴露"忘了改格式"的漏迁移。

一个条目的内容**派生自**另一条目时用 `version-var`，把上游已解析的版本注入成脚本变量。不这样做、各探各的，两次探测之间上游发新版就会产出**版本不一致的清单**（主源 A 版 + vendor B 版 → 构建必坏）：

```yaml
work_sources:
  - tracker-template: script
    expand: true
    version-var: {main: sources[0]}   # 脚本里 $main = sources[0] 本轮解析出的版本
    script: |
      echo "$main|https://x/vendor-$main.tar.gz"
```

只能引用**位于它之前**的槽位（`sources` 先于 `work_sources` 探测，列表内从左到右）；前向/自引用在探测时报错并说明可用范围。

work_sources-only 包（字体、jar 等非归档源必须放 work_sources）只写 work_sources 列表 + `version-source: work_sources[0]`，无 sources 条目。

## 快速开始

### 环境要求

- Rust 工具链（stable）
- Docker（构建运行时必需）

### 编译

```bash
cd farm
cargo build --release        # 产物: target/release/lankefarm
```

### 运行

```bash
./target/release/lankefarm --help
```

## CLI 命令

| 命令 | 说明 |
|---|---|
| `build <pkg>...\|--all --image <img>` | 增量、ABI 感知的构建，带计划预览（仅容器构建，`--image` 必填） |
| `validate --image <img>` | 重建所有缺少 `.build_ok` 标记的包 |
| `export --output <dir>` | 将构建仓库扁平化复制为发行格式 `<pkg>-<ver>.lpkg`（不重打包） |
| `chk <kind> --source <repo> [--pkgs …]` | 维护期实用检則工具集（**非稳定接口**，可能因技术变迁移除）：`qml`（QML import 依赖）/ `pkgconf`（.pc Requires 依赖）/ `pkg-err`（usr/etc|usr/var、.la、.a）/ `hook`（sysusers/tmpfiles → postinst 自动跑）/ `abi`（全 ABI 符号@版本审计）/ `full`（一键全跑）。共享 `--source/--arch/--pkgs-dir/--cache/--pkg/--full-rescan`；非 root、只读 |
| `track <pkg>\|--all [--run]` | 探测上游版本（默认只读提案，`--run` 应用） |
| `gen-trackers` | 批量调用 LLM 生成 tracker yaml |
| `seed --remote <url>` | 冷启动播种远程仓库（并行下载 + SHA-256 校验） |
| `serve [--root out] [--port 8000]` | 本地仓库静态 HTTP 服务器 |

## 构建流水线

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'fontSize': '14px'}}}%%
flowchart LR
    A[增量选择<br/>版本对比 + ABI 断裂] --> B[拓扑排序<br/>确定性顺序]
    B --> C[计划预览 + 确认]
    C --> D[源预下载<br/>确认集]
    D --> E[逐包容器构建]
    E --> F{ABI 检测<br/>removed SONAME?}
    F -->|断裂| G[repack / 传播 / 备份]
    F -->|正常| H[进仓库 / 备份清理]
    G --> H

    classDef start fill:#3B82F6,stroke:#2563EB,color:#fff,stroke-width:2px
    classDef process fill:#10B981,stroke:#059669,color:#fff,stroke-width:2px
    classDef decision fill:#F59E0B,stroke:#D97706,color:#fff,stroke-width:2px
    classDef end fill:#8B5CF6,stroke:#7C3AED,color:#fff,stroke-width:2px

    class A start
    class B,C,D,E process
    class F decision
    class G,H end
```

## 源码结构

```
farm/
├── src/
│   ├── main.rs          # 入口
│   ├── cli/             # build / validate / export / track / gen-trackers / seed / serve
│   ├── build/           # 调度层: 增量选择、拓扑排序、计划预览、源预下载
│   ├── abi.rs           # removed_sonames / detect_abi_breaks / propagate
│   ├── graph.rs         # index.txt 解析 + 链接依赖图
│   ├── scan.rs          # .lpkg 解包 + ELF needed_so/provides 扫描
│   ├── repack.rs        # metadata.json 漂移修正 + 重打
│   ├── verify.rs        # 构建产物 vs 期望 metadata 的三分支判定
│   ├── lpkg_binding.rs  # 唯一碰 lpkg 的接缝（docker 编排 + ABI 过渡备份注入）
│   └── ...
├── data/
│   ├── trackers/        # 各包的上游版本追踪器（github/gitlab/gnome/...）
│   └── build/           # 声明式 ABI 重建组（如 Python 生态）
└── tests/               # 回归测试
```

## 技术栈

| 技术 | 用途 |
|---|---|
| Rust | 实现语言（serde / clap / rusqlite / ureq / goblin / zstd / sha2） |
| SQLite | 任务状态库（`out/farm-state.db`） |
| Docker | 容器隔离构建（`--image` 指定基础镜像） |
| zstd · tar（Rust crate） | `.lpkg` 解包 / 重打（纯 Rust，无需系统 `zstd`/`tar`/`sudo`） |

## 贡献

欢迎提交 PR 或报告 Bug。所有交互逻辑收敛在 `LpkgBinding` trait（`lpkg_binding.rs`），新增 lpkg 相关功能请通过该接缝实现。

## 许可证

基于 GPL-3.0 开源。
