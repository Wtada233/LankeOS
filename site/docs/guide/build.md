---
title: 从源码构建
---

# 从源码构建 LankeOS

> 构建 LankeOS 需要一台运行 Linux 的主机和高性能的 CPU（建议 8 核以上）以及至少 64 GB 的可用磁盘空间。

## 构建环境要求

| 组件 | 要求 |
|------|------|
| 操作系统 | 任意现代 Linux 发行版（Arch/Ubuntu/Fedora 等） |
| CPU | x86_64，建议 8 核以上 |
| 内存 | 建议 8 GiB+ |
| 磁盘 | 至少 64 GiB 可用空间 |
| 工具链 | GCC、Make、pkg-config |

## 获取源码

```bash
git clone https://github.com/Wtada233/LankeOS.git
cd LankeOS
```

## 构建 lpkg 包管理器

lpkg 是整个构建流程的核心工具。推荐使用静态编译：

```bash
cd files/LankeOS/lpkg

# 静态编译（Docker 方式）
make docker
sudo make docker-install

# 或直接动态编译
make
sudo make install
```

## 构建软件包

使用 lpkg 的 build 命令构建单个软件包：

```bash
# 构建 bash
sudo lpkg build files/LankeOS/pkgs/bash

# 构建 coreutils
sudo lpkg build files/LankeOS/pkgs/coreutils

# 构建整个系统需要按依赖顺序构建所有 189 个软件包
```

## 构建系统架构

LankeOS 的构建系统基于以下概念：

### LankeBUILD 格式

每个软件包目录包含两个文件：

```text
pkgs/<package>/
├── LankeBUILD          # 构建脚本（Shell 函数）
└── LankeBUILD.json     # 构建元数据（JSON）
```

### LankeBUILD.json

```json
{
  "name": "package-name",
  "version": "1.2.3",
  "sources": ["https://upstream.example.com/pkg-1.2.3.tar.gz"],
  "work_sources": ["https://example.com/patches/fix.patch"],
  "no_strip": false,
  "deps": ["dependency >= 1.0"]
}
```

### LankeBUILD 脚本

定义三个阶段：

```bash
lankebuild_prepare() {
    # 补丁、配置生成
    patch -Np1 -i {WORK_DIR}/fix.patch
}

lankebuild_build() {
    # 配置、编译、安装到 {STAGING_ROOT}
    ./configure --prefix=/usr
    make -j$(nproc)
    make DESTDIR={STAGING_ROOT} install
}

lankebuild_package() {
    # 打包前的最终调整（可选）
    :
}
```

### 生命周期

```text
1. 解析 LankeBUILD.json → 获取名称、版本、源码 URL
2. 下载并解压源码
3. 替换变量（{PKG_NAME}、{PKG_VER}、{WORK_DIR} 等）
4. 执行 lankebuild_prepare()
5. 执行 lankebuild_build()   → 安装到 STAGING_ROOT
6. 执行 lankebuild_package()
7. 后处理：ELP 剥离、SONAME 链接、移除 .la 文件
8. 打包为 .lpkg（tar.zst + metadata.json）
```

## 构建整个发行版

完整构建通常需要以下步骤：

1. **构建 lpkg**（已完成）
2. **构建基础工具链** — glibc、gcc、binutils、coreutils 等
3. **构建系统基础设施** — systemd、dbus、util-linux、PAM 等
4. **构建网络栈** — NetworkManager、wpa_supplicant、openssh、curl
5. **构建图形栈** — Mesa、Wayland、wlroots、Sway
6. **构建开发工具** — LLVM、Rust、Python、Ruby
7. **构建桌面应用** — GTK4、WebKitGTK、Alacritty、Fcitx5
8. **构建内核和固件**
9. **打包 Live ISO**

> 注：完整构建指南仍在完善中。目前推荐直接使用 [预构建 ISO](/download)。

## Live ISO 打包

```bash
# 1. 重置系统状态（清除机器 ID、日志、临时文件等）
sudo bash files/live/reinit.sh

# 2. 打包 ISO（需 root 权限）
sudo bash files/live/pack.sh
```

生成的 ISO 文件位于 `files/live/ISO/../lankeos-live.iso`。

## 依赖生成

使用 `gen_deps.py` 自动分析并补充软件包依赖：

```bash
python3 files/LankeOS/lpkg/main/scripts/gen_deps.py /path/to/packages
```

该脚本通过分析 ELF 文件的 NEEDED 和 SONAME，自动更新每个包的 `metadata.json` 中的依赖列表。
