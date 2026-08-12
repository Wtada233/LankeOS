[English](README.en.md) | 中文

<h1 align="center">lpkg</h1>

<p align="center">
  <strong>为 LankeOS 设计的轻量级、基于命令行的 C++20 包管理器。</strong>
  <br />
  <em>原子 WAL 事务回滚 · needed_so ABI 校验 · 聚合索引 · 静态构建</em>
</p>

<p align="center">
  <a href="#使用方法"><img src="https://img.shields.io/badge/Quick_Start-4CAF50?style=for-the-badge" alt="Quick Start" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv3-2d8cf0?style=for-the-badge" alt="License" /></a>
</p>

<p align="center">
  <img src="https://img.shields.io/github/actions/workflow/status/Wtada233/LankeOS/lpkg-build.yml?style=flat&logo=github-actions&logoColor=white" alt="Build status" />
  <img src="https://img.shields.io/badge/C++20-00599C?style=flat&logo=cplusplus&logoColor=white" alt="C++20" />
  <img src="https://img.shields.io/github/license/Wtada233/LankeOS?style=flat" alt="License" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Docker-2496ED?style=flat&logo=docker&logoColor=white" alt="Docker" />
  <img src="https://img.shields.io/badge/SQLite-003B57?style=flat&logo=sqlite&logoColor=white" alt="SQLite" />
  <img src="https://img.shields.io/badge/Bash-4EAA25?style=flat&logo=gnubash&logoColor=white" alt="Bash" />
  <img src="https://img.shields.io/badge/zstd-1E5B8C?style=flat&logo=zstandard&logoColor=white" alt="zstd" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Claude_Code-D97757?style=flat&logo=claude&logoColor=white" alt="Claude Code" />
</p>

`lpkg` 是一个为 LankeOS 设计的轻量级、基于命令行的包管理器。它使用 C++20 编写，旨在为 LFS (Linux From Scratch) 环境提供原子化、可追溯的软件包管理方案。

## 功能特性

-   **全生命周期管理**：安装、卸载、升级、重装软件包。
-   **needed_so 依赖校验**：安装前自动验证每个 ELF DT_NEEDED 声明的 SONAME 在仓库中有对应的提供者包，无提供者则拒绝安装，杜绝"空 provides 还能装"的漏洞。
-   **SIGINT 优雅退出**：Ctrl+C 设置优雅退出标志，当前操作（含必要回滚）完整执行完毕后退出，防止事务中断导致系统不一致。不提供强制终止——确保回滚永不被打断。
-   **智能版本解析**：支持多位修订号（如 `1.0.0.1`），内置严谨的比较算法，确保 `6.16.1 > 6.6.1`。支持复合区间约束（如 `>= 2.0.0 < 3.0.0`）。
-   **聚合索引**：采用 `index.txt` 聚合格式，一行即可记录包的所有版本及各自的哈希、依赖、provides、needed_so。
-   **包内嵌元数据**：采用 `metadata.json` 嵌入包内，统一存储名称、版本、依赖、needed_so、虚拟提供、手册页等元数据。
-   **自动依赖推导**：提供 `gen_deps.py` 工具，基于 ELF 文件扫描自动生成 `needed_so`（DT_NEEDED SONAME 列表）和 `deps`（提供者包名），无需手动维护版本约束。
-   **内容即文件布局**：包的 `content/` 目录结构直接对应根目录文件布局。
-   **自动化运维**：提供 `lrepo-mgr.py` 工具，支持一键推送到腾讯云 COS (S3) 或 SCP 远程服务器。新增 `--path` 参数支持本地文件系统仓库，便于离线测试。
-   **高兼容性静态构建**：内置系统 CA 证书路径自动探测，确保静态编译版本在不同 Linux 发行版上都能正常联网。
-   **安全性**：强制 SHA256 哈希校验、文件冲突检测、恶意路径过滤。
-   **系统钩子与触发器**：支持包级 `postinst/prerm` 脚本及系统级触发器（如 `ldconfig`）。

## 使用方法

`lpkg` 的大部分操作需要 `root` 权限。

**通用语法:**
```bash
lpkg [选项] <命令> [参数]
```

### 常用命令

-   **`install <包名>[:版本]`**: 安装包。缺省版本时自动安装最新版。
-   **`upgrade`**: 自动从索引中查找所有已安装包的更新。
-   **`remove <包名> [--force]`**: 移除包。使用 `--force` 可强制删除被依赖的包。
-   **`query [-p] <包名|文件名>`**: 查询文件归属或列出包内文件。
-   **`scan [目录]`**: 扫描系统中不属于任何包的"孤儿"文件。
-   **`pack -o <输出文件> -d <目录>`**: 构建 `.lpkg` 格式的软件包，从 `<目录>/metadata.json` 读取包元数据。
-   **`build [目录]`**: 自动编译并打包软件包。

## 快速开始

### 依赖

以下库仅在主机上直接编译（动态/静态链接）时需要；推荐用 Docker 构建，主机无需安装任何依赖:
- `libcurl`: 用于文件下载。
- `libarchive`: 用于解压包文件。
- `libcrypto` (OpenSSL): 用于计算哈希。
- `libfmt`: 用于字符串格式化(Header-only)。

在 Arch Linux 上安装:
```bash
sudo pacman -S curl libarchive openssl fmt
```

### 编译与安装

1.  **Docker 构建 (推荐)**: 完整构建与测试只需要 Docker。所有依赖（含 libgit2、libsolv 等静态库）都会在 `alpine:3.19` 容器内自动安装，产出纯静态二进制:
    ```bash
    make docker          # 纯静态二进制: build/lpkg-docker
    sudo make docker-install
    make test            # 在同一容器内运行完整测试套件
    ```
2.  **动态编译 (主机)**: 需要主机上的 `libcurl`、`libarchive`、`libelf`、`libgit2`、`libsolv`、`libopenssl`:
    ```bash
    make && sudo make install
    ```

## 仓库管理 (运维指南)

项目在 `main/scripts/lrepo-mgr.py` 提供了自动化运维脚本。

### 1. 配置仓库连接
支持 S3 (腾讯云 COS, AWS) 或 SCP:
```bash
# 配置 S3/COS
./main/scripts/lrepo-mgr.py config --set \
    storage.type=s3 \
    storage.endpoint=https://cos.ap-hongkong.myqcloud.com \
    storage.bucket=your-bucket-id \
    storage.access_key=AK... \
    storage.secret_key=SK...
```

### 2. 发布软件包
该命令会自动更新聚合索引并上传:
```bash
./main/scripts/lrepo-mgr.py push ./pkgs/*.lpkg
```

### 3. 本地仓库（离线测试）
```bash
# 初始化本地 repo 并推送包
./main/scripts/lrepo-mgr.py --path /tmp/repo push ./pkgs/*.lpkg

# 查看生成的索引
cat /tmp/repo/x86_64/index.txt
```

### 4. 清理历史版本
从存储端删除所有不在 `index.txt` 记录中的旧版本文件:
```bash
./main/scripts/lrepo-mgr.py cleanup
```

## 包格式规范

每个 `.lpkg` 文件是一个 tar.zst 压缩包，包含以下内容：

```text
metadata.json         # 包元数据（名称、版本、依赖、提供、手册页等）
content/              # 文件内容（直接对应根目录结构）
hooks/                # 钩子脚本（可选）
```

### metadata.json 示例
```json
{
  "name": "curl",
  "version": "8.11.1",
  "deps": ["glibc", "openssl", "zlib", "zstd", "bash"],
  "provides": ["libcurl.so.4"],
  "needed_so": ["libc.so.6", "libssl.so.3", "libcrypto.so.3", "libz.so.1", "libzstd.so.1"],
  "man": "curl(1) - transfer a URL\n..."
}
```

| 字段 | 说明 |
|------|------|
| `name` | 包名 |
| `version` | 版本号 |
| `deps` | 依赖包名列表（无版本约束，由 gen_deps 自动解析 needed_so 生成） |
| `provides` | 本包提供的 SONAME 及虚拟能力列表 |
| `needed_so` | 本包 ELF 文件声明的 DT_NEEDED SONAME 列表（运行时依赖的原始真相） |
| `man` | 内联手册页内容（可选） |

`content/` 目录下的文件布局直接对应目标根目录（`/`）。

## 构建标志配置（build.conf）

`lpkg build` 默认使用 **Arch Linux 编译/链接标志，ISA 基线 x86-64-v3**（绝不用
`-march=native`，避免把构建宿主超出 v3 的 CPU 特性烙进发行版包；v3 = AVX2/FMA 等，
覆盖 2013 年后绝大多数主流 x86-64 机器）。默认值见
`main/src/base/build_defaults.hpp`（fallback），系统级配置见
`/etc/lpkg/build.conf`（makepkg.conf 风格，由 `make install` 从
`main/conf/build.conf` 安装）：

```text
CFLAGS="-march=x86-64-v3 -mtune=generic -O2 -pipe -fno-plt -fexceptions ..."
CXXFLAGS="$CFLAGS -Wp,-D_GLIBCXX_ASSERTIONS"   # 写法上为完整展开
LDFLAGS="-Wl,-O1 -Wl,--sort-common -Wl,--as-needed -Wl,-z,relro -Wl,-z,now ..."
LTOFLAGS="-flto=auto"
MAKEFLAGS="-j$(nproc)"                          # $(nproc) 自动展开为逻辑核心数
```

`build` 的每个阶段（`lankebuild_prepare/build/package`）执行前会把上述标志
`export` 到构建环境，configure/make/cmake 自动继承；LankeBUILD 脚本也可引用
模板变量 `{CFLAGS}`、`{CXXFLAGS}`、`{LDFLAGS}`、`{MAKEFLAGS}`。

**逐包覆盖**（优先级最高）：`LankeBUILD.json` 增加可选字段，留空 = 用配置默认：

```json
{
  "name": "foo",
  "version": "1.0",
  "cflags": "-O2 -march=x86-64-v3",
  "ldflags": "-Wl,--as-needed",
  "makeflags": "-j2",
  "lto": true
}
```

`"lto": true` 会把 `LTOFLAGS`（默认 `-flto=auto`）追加到编译与链接标志。

## 仓库规范

### 目录结构
```text
/x86_64
  ├── index.txt           # 核心索引：包名|版本:哈希:依赖:提供:needed_so;...|
  └── bash/
      ├── 5.3.lpkg        # 实际的 tar.zst 压缩包
      └── 5.4.lpkg
```

### 索引行示例
```text
# provides/needed_so 在版本块内（第 4、5 字段），每个版本独立
curl|8.11.1:hash:glibc,openssl,zlib,zstd,bash:libcurl.so.4:libc.so.6,libssl.so.3,libz.so.1,libzstd.so.1|
```

## 源码架构

-   **`Repository`**: 解析聚合索引，实现版本智能排序。
-   **`InstallationTask`**: 原子事务模型，支持失败自动回滚。
-   **`Downloader`**: 封装了 `libcurl` 并集成了多路径证书探测逻辑。
-   **`Cache`**: 本地状态数据库，存储于 `/var/lib/lpkg/`。
-   **`metadata.json`**: 包内嵌元数据（名称、版本、依赖、提供、手册页）。

## 贡献

欢迎提交 PR 或报告 Bug。对于新的功能建议，请优先考虑保持二进制文件的轻量化。

## 许可证

基于 GPL3 许可证开源。
