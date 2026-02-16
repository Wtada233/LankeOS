# lpkg - 一个简单的 C++ 包管理器

`lpkg` 是一个为 LankeOS 设计的轻量级、高性能、基于命令行的包管理器。它使用 C++20 编写，旨在为 LFS (Linux From Scratch) 环境提供原子化、可追溯的软件包管理方案。

## 功能特性

-   **全生命周期管理**：安装、卸载、升级、重装软件包。
-   **智能版本解析**：支持多位修订号（如 `1.0.0.1`），内置严谨的比较算法，确保 `6.16.1 > 6.6.1`。
-   **聚合索引优化**：采用 `index.txt` 聚合格式，一行即可记录包的所有版本及其哈希，极大减少网络请求。
-   **零冗余存储**：取消了 `latest.txt` 和单独的 `hash.txt`。软件包以 `包名/版本号.lpkg` 形式扁平化存储。
-   **自动化运维**：提供 `lrepo-mgr.py` 工具，支持一键推送到腾讯云 COS (S3) 或 SCP 远程服务器。
-   **高兼容性静态构建**：内置系统 CA 证书路径自动探测，确保静态编译版本在不同 Linux 发行版上都能正常联网。
-   **安全性**：强制 SHA256 哈希校验、文件冲突检测、恶意路径过滤。
-   **系统钩子与触发器**：支持包级 `postinst/prerm` 脚本及系统级触发器（如 `ldconfig`）。

## 用户指南

### 依赖

编译 `lpkg` 需要以下库：
- `libcurl`: 用于文件下载。
- `libarchive`: 用于解压包文件。
- `libcrypto` (OpenSSL): 用于计算哈希。
- `libfmt`: 用于字符串格式化（Header-only）。

在 Arch Linux 上安装：
```bash
sudo pacman -S curl libarchive openssl fmt
```

### 编译与安装

1.  **动态编译**:
    ```bash
    make && sudo make install
    ```
2.  **静态编译 (推荐用于 LFS)**:
    项目中包含 `Dockerfile.build`，可生成完全静态链接的二进制文件：
    ```bash
    sudo docker build -t lpkg-builder -f Dockerfile.build .
    sudo docker run -it --rm -v $(pwd):/app lpkg-builder
    ```

### 使用方法

`lpkg` 的大部分操作需要 `root` 权限。

**通用语法:**
```bash
lpkg [选项] <命令> [参数]
```

#### 常用命令

-   **`install <包名>[:版本]`**: 安装包。缺省版本时自动安装最新版。
-   **`upgrade`**: 自动从索引中查找所有已安装包的更新。
-   **`remove <包名> [--force]`**: 移除包。使用 `--force` 可强制删除被依赖的包。
-   **`query [-p] <包名|文件名>`**: 查询文件归属或列出包内文件。
-   **`scan [目录]`**: 扫描系统中不属于任何包的“孤儿”文件。
-   **`pack -o <输出文件> --source <源目录>`**: 构建 `.lpkg` 格式的软件包。

## 仓库管理 (运维指南)

项目在 `main/scripts/lrepo-mgr.py` 提供了一个强大的运维脚本。

### 1. 配置仓库连接
支持 S3 (腾讯云 COS, AWS) 或 SCP：
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
该命令会自动提取依赖信息、更新聚合索引并上传：
```bash
./main/scripts/lrepo-mgr.py push ./pkgs/*.lpkg
```

### 3. 清理历史版本
从存储端删除所有不在 `index.txt` 记录中的旧版本文件：
```bash
./main/scripts/lrepo-mgr.py cleanup
```

## 仓库规范

### 目录结构
```text
/amd64
  ├── index.txt           # 核心索引：包名|版本1:哈希1,版本2:哈希2|依赖|提供
  └── bash/
      ├── 5.2.lpkg        # 实际的 tar.zst 压缩包
      └── 5.3.lpkg
```

### 索引行示例
```text
acl|2.3.1:sha...,2.3.2:sha...|attr,coreutils|libacl.so
```

## 源码架构

-   **`Repository`**: 负责解析聚合索引，实现版本智能排序。
-   **`InstallationTask`**: 原子事务模型，支持失败自动回滚。
-   **`Downloader`**: 封装了 `libcurl` 并集成了多路径证书探测逻辑。
-   **`Cache`**: 本地状态数据库，存储于 `/var/lib/lpkg/`。

## 贡献

欢迎提交 PR 或报告 Bug。对于新的功能建议，请优先考虑保持二进制文件的轻量化。

## 许可证

基于 GPL3 许可证开源。
