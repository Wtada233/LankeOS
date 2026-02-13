# lpkg - 一个简单的 C++ 包管理器

`lpkg` 是一个为 LankeOS 设计的轻量级、基于命令行的包管理器。它使用 C++20 编写，旨在提供一个简单而高效的方式来管理系统上的软件包。

## 功能特性

-   **安装、卸载、升级、重装** 软件包。
-   **自动处理依赖关系**，包括版本约束解析和循环依赖检测。
-   **虚拟包支持**：通过 `provides` 机制实现功能映射（如 `sh` 可由 `bash` 提供）。
-   **安全性**：SHA256 哈希校验、文件冲突检测、恶意路径过滤。
-   **系统钩子与触发器**：支持包级别的 `postinst/prerm` 脚本及系统级的 `ldconfig` 等触发器。
-   **孤儿文件扫描**：扫描文件系统中不属于任何包的冗余文件。
-   **多语言支持** (英语, 中文)。
-   **数据库保护**：使用文件锁防止并发冲突。

## 用户指南

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

-   **`install <包名>[:版本]...`**: 安装一个或多个包。如果不指定版本，则安装最新版。支持指定本地 `.lpkg` 或 `.tar.zst` 文件路径。
    ```bash
    # 安装远程包
    lpkg install a-package:1.2.3
    # 安装本地文件
    lpkg install ./my-package-1.0.0.lpkg
    ```

-   **`remove <包名>... [--force]`**: 移除一个或多个包。

-   **`upgrade`**: 升级所有已安装的软件包到最新版本。

-   **`reinstall <包名>...`**: 重新安装指定的软件包，常用于修复损坏的安装。

-   **`query [-p] <包名|文件名>`**: 
    -   默认查询文件属于哪个包。
    -   使用 `-p` 选项列出指定包包含的所有文件。

-   **`scan [目录]`**: 扫描指定目录（默认系统根目录）下不属于任何软件包的孤立文件。

-   **`pack -o <输出文件> --source <源目录>`**: 将源目录打包为 `lpkg` 格式（详见打包指南）。

-   **`man <包名>`**: 显示软件包的手册页。

-   **`autoremove`**: 自动移除不再需要的依赖包。

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

## 源码架构

项目采用模块化设计，核心逻辑分层如下：

### 1. 核心逻辑层 (Core Logic)
-   **`PackageManager` (`package_manager.cpp`)**: 
    -   管理包的生命周期。
    -   **`InstallationTask`**: 核心事务类，采用准备(Prepare) -> 提交(Commit) 模式，确保安装过程的原子性和可回滚性。
-   **`Cache` (`cache.cpp`)**: 
    -   单例模式，管理本地数据库（`/var/lib/lpkg/`）。
    -   维护文件所有权映射 (`files.db`)、Provider 映射 (`provides.db`) 和依赖图。
-   **`Repository` (`repository.cpp`)**: 
    -   解析远程或本地镜像的 `index.txt`。
    -   处理版本约束并寻找最佳匹配版本。

### 2. 系统交互层 (System Interaction)
-   **`TriggerManager` (`trigger.cpp`)**: 处理系统级触发器（如 `ldconfig`）。
-   **`Scanner` (`scanner.cpp`)**: 实现文件系统遍历与数据库交叉对比。
-   **`Packer` (`packer.cpp`)**: 实现软件包构建流程。

### 3. 基础组件层 (Infrastructure)
-   **`Archive`, `Downloader`, `Hash`**: 分别对 `libarchive`, `libcurl` 和 `OpenSSL` 的封装。
-   **`Localization`**: 基于 Key-Value 的多语言管理。

## SDK / 内部接口说明

如果你希望在其他 C++ 项目中集成 `lpkg` 的功能，可以参考以下核心类：

### `InstallationTask`
用于执行复杂的安装事务。
```cpp
InstallationTask task(
    "pkg_name",      // 包名
    "1.0.0",         // 目标版本
    true,            // 是否为显式安装
    "old_version",   // 旧版本（如果是升级）
    "local_path"     // 如果是本地安装，提供文件路径
);

task.prepare();      // 下载、解压、文件冲突预检
task.commit();       // 复制文件、运行钩子、更新数据库
```

### `Cache` (本地数据库)
```cpp
auto& cache = Cache::instance();
if (cache.is_installed("bash")) {
    std::string ver = cache.get_installed_version("bash");
}
// 查询文件归属
auto owners = cache.get_file_owners("/usr/bin/ls");
```

## 打包指南

`lpkg` 提供了内置的 `pack` 命令来简化打包流程。

### 1. 准备源目录
创建一个目录（如 `myapp_src/`），结构如下：

```text
myapp_src/
├── root/              # 必填：安装后位于系统根目录的内容
│   ├── usr/bin/myapp
│   └── etc/myapp.conf
├── deps.txt           # 可选：依赖列表 (如 libc>=2.35)
├── provides.txt       # 可选：提供的虚拟功能 (如 sh)
├── man.txt            # 可选：软件包的手册页
└── hooks/             # 可选：安装/卸载脚本
    ├── postinst.sh
    └── prerm.sh
```

### 2. 执行打包
使用 `lpkg pack` 命令生成软件包。它会自动扫描 `root/` 目录并生成内部元数据。

```bash
lpkg pack -o myapp-1.0.0.lpkg --source ./myapp_src
```

### 3. 发布到镜像
1.  **生成哈希**:
    ```bash
    sha256sum myapp-1.0.0.lpkg | cut -d' ' -f1 > hash.txt
    ```
2.  **上传路径**:
    将 `myapp-1.0.0.lpkg` (重命名为 `app.tar.zst`) 和 `hash.txt` 放入镜像服务器：
    `<mirror_url>/<arch>/myapp/1.0.0/`
3.  **更新版本**:
    将 `<mirror_url>/<arch>/myapp/latest.txt` 内容设为 `1.0.0`。
4.  **更新索引**:
    更新镜像根目录下的 `index.txt`，加入新包的信息。

### 附：软件包内部结构规范
一个标准的 `lpkg` 包（`.tar.zst`）内部包含：
-   `content/`: 实际安装到系统的文件（由 `root/` 映射）。
-   `files.txt`: 核心元数据，定义文件到系统的映射。
-   `deps.txt`, `provides.txt`, `man.txt`: 包属性定义。
-   `hooks/`: 生命周期脚本。

## 贡献

欢迎提交 Pull Requests 或 Issues 来改进 `lpkg`。

## 许可证

请参考项目根目录下的 `LICENSE` 文件。
