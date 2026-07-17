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
cd lpkg

# 静态链接（要求docker）
sudo make docker-install

# 或直接默认
make
sudo make install
```

## 构建软件包

使用 lpkg 的 build 命令构建单个软件包：

```bash
# 构建 bash
sudo lpkg build pkgs/bash

# 构建 coreutils
sudo lpkg build pkgs/coreutils

# 构建整个系统需要按依赖顺序构建所有软件包
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
    cd pkg-dir
    # 补丁、配置生成
    patch -p1 < "{WORK_DIR}/fix.patch"
}

lankebuild_build() {
    cd pkg-dir
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
7. 后处理：ELF 剥离、SONAME 链接、移除 .la 文件
8. 打包为 .lpkg（tar.zst + metadata.json）
```

## 构建整个发行版

完整构建可以遵循以下步骤：

### 1. 设置工作区：运行LankeOS的一台机器，实体机或虚拟机均可，作为构建宿主

- 安装所有包，比如默认没有安装的wlroots（目前LankeBUILD系统没有build_deps声明，这是一大缺陷，我改天会改善）

### 2. 开始构建

- 依旧运行`git clone https://github.com/Wtada233/LankeOS --depth=1`

- 运行pkgs/*下的hacks.sh（每个包需要的特殊环境，比如python3=python的软链接，特殊python库等LankeOS默认不提供的）

- 在pkgs目录下运行`sudo python3 ../lpkg/main/scripts/lankeos-world-rebuild-helper.py`

- 按照指引进行半自动的重新构建。通常情况下，只需要在每一步按下Ctrl+D，这里半自动是为了方便debug，如果你需要全自动重新构建，请使用yes exit来快速退出shell。

- 运行`sudo python3 ../lpkg/main/scripts/gen_deps.py .`生成provider map并自动标记deps和needed_so等（记得提前安装pyelftools用于快速扫描）

- 在所有打包完成之后，创建新的LankeOS根目录，基本usr merge软链接，基本系统配置文件（passwd等）

- 手动把lpkg cp到rootfs/usr/bin，创建rootfs/etc/resolv.conf和rootfs/etc/ssl的所有必要证书（可直接cp主机的）然后cp所有包到新的rootfs，chroot到新rootfs，使用lpkg安装所有包，使用no-deps直接拒绝解析依赖，完成安装。

## Live ISO 打包

```bash
# 1. 重置系统状态（清除机器 ID、日志、临时文件等）
sudo bash live/reinit.sh

# 2. 打包 ISO（需 root 权限）
sudo bash live/pack.sh
```

生成的 ISO 文件位于 `live/lankeos-live.iso`。

## 依赖生成

使用 `gen_deps.py` 自动分析并补充软件包依赖：

```bash
python3 lpkg/main/scripts/gen_deps.py /path/to/packages
```

该脚本通过分析 ELF 文件的 NEEDED 和 SONAME，自动更新每个包的 `metadata.json` 中的依赖列表。
