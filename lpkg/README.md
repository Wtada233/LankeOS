# lpkg - 一个简单的 C++ 包管理器

`lpkg` 是一个为 LankeOS 设计的轻量级、基于命令行的包管理器。它使用 C++20 编写，旨在提供一个简单而高效的方式来管理系统上的软件包。

## ✨ 功能特性

-   **安装、卸载、升级** 软件包。
-   **自动处理依赖关系**，包括循环依赖检测。
-   通过 **SHA256 哈希校验** 保证包的完整性。
-   **文件冲突检测**，防止不同包覆盖同一文件。
-   防止删除被其他包共享的文件。
-   **自动移除**不再需要的孤立包。
-   支持**多语言** (英语, 中文)。
-   通过 `man` 命令查看软件包的文档。
-   使用文件锁防止多个实例同时修改数据库。

## 🔧 用户指南

### 依赖

编译 `lpkg` 需要以下库：
- `libcurl`: 用于文件下载。
- `libarchive`: 用于解压包文件。
- `libfmt`: 用于格式化字符串。
- `libcrypto` (OpenSSL): 用于计算文件哈希值。

在基于 Debian/Ubuntu 的系统上，你可以通过以下命令安装它们：
```bash
sudo apt-get install build-essential libcurl4-openssl-dev libarchive-dev libfmt-dev libssl-dev
```

### 编译

在项目根目录 (`lpkg/`) 下，运行 `make`：
```bash
make
```
可执行文件 `lpkg` 将会生成在 `build/` 目录下。

### 安装

运行 `make install` 将 `lpkg` 安装到系统中。
```bash
sudo make install
```
默认安装路径如下：
- 可执行文件: `/usr/bin/lpkg`
- 配置文件: `/etc/lpkg/`
- 共享文件 (语言、文档): `/usr/share/lpkg/`
- 锁文件: `/var/lpkg/db.lck`

你可以通过 `PREFIX` 变量来自定义安装基准目录：
```bash
sudo make PREFIX=/usr/local install
```

### 使用方法

`lpkg` 的所有操作都需要 `root` 权限 (除了 `man` 命令)。

**通用语法:**
```
lpkg [选项] <命令> [参数]
```

#### 命令

-   **`install <包名>[:版本]...`**: 安装一个或多个包。如果不指定版本，则安装最新版。
    ```bash
    # 安装最新版的 a-package 和 b-package
    lpkg install a-package b-package

    # 安装指定版本的 a-package
    lpkg install a-package:1.2.3
    ```

-   **`remove <包名>... [--force]`**: 移除一个或多个包。
    -   `--force`: 强制移除，即使它被其他包依赖（不推荐）。
    ```bash
    lpkg remove a-package
    ```

-   **`autoremove`**: 自动移除作为依赖安装且不再被任何手动安装的包所需要的软件包。
    ```bash
    lpkg autoremove
    ```

-   **`upgrade`**: 升级所有已安装的软件包到它们的最新版本。
    ```bash
    lpkg upgrade
    ```

-   **`man <包名>`**: 显示一个软件包的 `man.txt` 内容。
    ```bash
    lpkg man a-package
    ```

#### 选项

-   `-h, --help`: 显示帮助信息。
-   `--non-interactive [yes|no]`: 以非交互模式运行。如果只提供 `--non-interactive`，则默认为 `yes`。
-   `--force`: 与 `remove` 命令一同使用，强制删除。

### 配置

`lpkg` 的镜像源在 `/etc/lpkg/mirror.conf` 文件中配置。文件内容应该是一个指向仓库根目录的 URL，以 `/` 结尾。
例如:
```
http://your-mirror.com/lpkg-repo/
```

## ⚙️ 代码实现

### 项目结构

```
lpkg/
├── Makefile              # 编译和安装脚本
├── conf/
│   └── mirror.conf       # 默认镜像配置文件
├── l10n/
│   ├── en.txt            # 英文本地化字符串
│   └── zh.txt            # 中文本地化字符串
├── src/                  # C++ 源代码
│   ├── main.cpp          # 程序入口，命令解析和分发
│   ├── package_manager.hpp/cpp # 核心逻辑，处理包的增删改查
│   ├── downloader.hpp/cpp    # 使用 libcurl 下载文件
│   ├── archive.hpp/cpp       # 使用 libarchive 解压文件
│   ├── hash.hpp/cpp          # 使用 OpenSSL 计算 SHA256
│   ├── config.hpp/cpp        # 管理配置和路径
│   ├── version.hpp/cpp       # 版本号比较
│   ├── localization.hpp/cpp  # 加载和管理本地化字符串
│   ├── utils.hpp/cpp         # 工具函数 (日志、锁、权限检查)
│   ├── exception.hpp       # 异常类定义
│   └── ...
└── third_party/          # 第三方库
    └── include/
        └── cxxopts.hpp   # 命令行参数解析库
```

### 核心组件

-   **`main.cpp`**: 使用 `cxxopts` 解析命令行参数，根据用户输入的命令调用 `PackageManager` 中对应的功能。在执行需要修改系统的操作前，会进行 `root` 权限检查并创建数据库锁。

-   **`PackageManager`**: 这是 `lpkg` 的大脑。它管理着软件包的整个生命周期。
    -   **`InstallationTask` 类**: 封装了单个包的完整安装流程，包括：
        1.  **下载与校验**: 从镜像源下载包的 `app.tar.zst` 和 `hash.txt`，并校验文件哈希值。
        2.  **解压与验证**: 将包解压到临时目录，并检查 `deps.txt`, `files.txt` 等元数据文件是否存在。
        3.  **依赖解析**: 读取 `deps.txt`，递归地安装所有尚未安装的依赖。内置了循环依赖检测。
        4.  **文件冲突检查**: 读取 `files.txt`，检查将要安装的文件是否已存在且被其他包拥有。
        5.  **文件复制**: 将包内容从临时目录复制到系统目标位置。
        6.  **注册包**: 更新包数据库，记录新安装的包、其版本、文件列表和依赖关系。
    -   **移除逻辑**: 在移除包时，会检查是否有其他包依赖于它。同时，它会检查包内的文件是否被其他包共享，以避免破坏系统。
    -   **缓存机制**: 为了提高性能，`PackageManager` 在内存中缓存了已安装包列表和文件数据库。在操作结束后，`write_cache()` 会将变动写回磁盘。

-   **`Downloader`**: 一个简单的 `libcurl` 封装，支持带进度条的文件下载和下载重试。

-   **`Archive`**: 使用 `libarchive` 来处理 `.tar.zst` 格式的压缩包解压。

-   **`DBLock`**: 一个基于 `flock` 的 RAII 锁。当 `PackageManager` 实例创建时，它会自动锁定数据库文件 (`/var/lpkg/db.lck`)，并在实例销毁时自动解锁，确保了任何时候只有一个 `lpkg` 进程在修改系统状态。

### 软件包格式

`lpkg` 期望在镜像服务器上的每个软件包都遵循以下目录结构：
```
<mirror_url>/<arch>/<pkg_name>/<version>/
├── app.tar.zst   # 包内容，使用 zstd 压缩的 tar 包
├── hash.txt      # app.tar.zst 的 SHA256 哈希值
```
此外，还有一个 `latest.txt` 文件位于包的根目录，用于 `upgrade` 和 `install` 最新版：
```
<mirror_url>/<arch>/<pkg_name>/latest.txt
```
该文件只包含最新版本的版本号字符串。

## 📦 打包指南

本节将介绍如何为 `lpkg` 创建软件包。

### 自动打包 (占位符)

> ⚠️ **注意**: `lbp` (LankeOS Build & Pack) 工具目前正在开发中，尚未发布。一旦可用，它将成为创建和上传软件包的首选方式，能自动处理大部分打包流程。

### 手动打包

手动打包需要您手动创建包的元数据文件和压缩包。以下是详细步骤和规范。

#### 1. 包源文件结构

首先，准备一个用于打包的源目录，其结构如下：

```
my-package-1.0.0/
├── content/
│   └── binary.bin
├── deps.txt
├── files.txt
├── man.txt
└── hooks/
    ├── postinst.sh
    └── prerm.sh
```

-   **`content/`**: 这个目录包含了包的所有文件。`files.txt` 中列出的源路径是相对于这个目录的。

#### 2. 创建元数据文件

##### `deps.txt`

一个纯文本文件，每行列出一个依赖包的名称。如果您的包没有依赖项，这个文件可以为空。

**示例:**
```
glibc
openssl
```

##### `files.txt`

这是包中最重要的元数据文件之一，它告诉 `lpkg` 如何安装 `content/` 目录中的文件。

-   **格式**: `源文件 目标目录`
-   **源文件**: 位于 `content/` 目录中的文件名。
-   **目标目录**: 文件在目标系统上安装的**基准目录** (推荐以 `/` 结尾)。最终的安装路径是 `目标目录` + `源文件`。

**示例 `files.txt`:**
```
binary.bin /usr/bin/
config.json /etc/my-app/
```
-   `binary.bin /usr/bin/`: 将 `content/binary.bin` 安装到 `/usr/bin/binary.bin`。
-   `config.json /etc/my-app/`: 将 `content/config.json` 安装到 `/etc/my-app/config.json`。

**注意**: 您必须列出 `content/` 目录中的 **每一个文件**。目录会自动创建，无需列出。

'''##### `man.txt`

纯文本文件，包含软件包的 `man` 手册页内容。用户可以通过 `lpkg man <包名>` 查看。

##### `hooks/` (可选)

您可以创建一个 `hooks/` 目录来存放安装和卸载钩子脚本。这些脚本将在特定时间点被 `lpkg` 自动执行。

-   **`postinst.sh`**: 在软件包成功安装并注册 **之后** 执行。通常用于执行服务启动、用户创建或其他配置任务。
-   **`prerm.sh`**: 在软件包从系统中移除文件 **之前** 执行。通常用于停止服务、清理用户数据等。

如果这些脚本不存在，`lpkg` 会默认跳过。

#### 3. 创建包压缩文件

##### `app.tar.zst`

这是包的核心压缩文件。它应包含打包源目录（例如 `my-package-1.0.0/`）内 **所有** 的内容，包括：
-   `content/` 目录
-   `deps.txt`
-   `files.txt`
-   `man.txt`
-   `hooks/` (如果存在)

在您的包源目录中运行以下命令来创建它：

```bash
tar -I zstd -cf ../app.tar.zst ./
```

#### 4. 生成哈希值

##### `hash.txt`

为了保证包的完整性，需要为 `app.tar.zst` 生成一个 SHA256 哈希值。`hash.txt` 文件 **不包含在** `app.tar.zst` 内，而是与 `app.tar.zst` 一起存放在镜像服务器的同一个目录中。

在 `app.tar.zst` 所在的目录中运行：
```bash
sha256sum app.tar.zst | cut -d' ' -f1 > hash.txt
```
这会计算哈希值并将其保存到 `hash.txt` 文件中。

#### 5. 上传到镜像

完成以上步骤后，您的包最终目录中应该有以下文件：
-   `app.tar.zst`
-   `hash.txt`

将这些文件上传到您的 `lpkg` 镜像服务器。路径结构应遵循：
`<mirror_url>/<arch>/<pkg_name>/<version>/`

例如: `http://my-mirror.com/repo/amd64/my-package/1.0.0/`

最后，如果这是最新版本，不要忘记更新或创建 `latest.txt` 文件：
`<mirror_url>/<arch>/<pkg_name>/latest.txt`

该文件应只包含版本号字符串，例如 `1.0.0`。

## 🤝 贡献

欢迎提交 Pull Requests 或 Issues 来改进 `lpkg`。

## 📄 许可证

请参考项目根目录下的 `LICENSE` 文件。
