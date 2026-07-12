---
title: 快速开始
---

# 快速开始

## 什么是 LankeOS？

**LankeOS** 是一个基于 Linux From Scratch (LFS) 从零构建的 Linux 发行版。它不是某个现有发行版的衍生版——所有软件包都是手动配置、编译和打包的。

它的核心特色：

- **自研包管理器** `lpkg`（C++20）
- **原生 Wayland 桌面**（Sway 平铺窗口管理器）
- **极致启动速度**（截至0.12 ~4 秒进桌面）
- **中文优先**（Fcitx5 + Noto CJK 开箱即用）
- **从源码构建**的完整工具链

## 快速体验

### 1. 下载 ISO

从 [下载页面](/download) 获取最新的 Live ISO 镜像。

### 2. 写入 U 盘

```bash
sudo dd if=lankeos-0.09-live.iso of=/dev/sdX bs=4M status=progress
```

### 3. 启动

将 U 盘插入电脑，重启并从 U 盘启动（在 BIOS/UEFI 启动菜单中选择 U 盘）。

首次启动你会看到：

```
[    0.0] LankeOS 7.1.1-lanke 内核启动
[    1.2] initramfs: 找到 LankeOS 启动介质
[    1.5] initramfs: 挂载 rootfs.sfs
[    1.8] initramfs: 设置 OverlayFS
[    2.1] 切换到 systemd
[    2.8] Sway 桌面就绪
```

### 4. 登录

用户名和密码（Live 模式下无需密码或自动登录）：

| 账户 | Live 模式 | 持久化模式 |
|------|----------|-----------|
| `root` | 自动登录 | 密码为空 |
| `LankeOS` | 自动登录 | 密码为空 |

### 5. 探索系统

```bash
# 查看系统信息
fastfetch

# 检查包管理器
sudo lpkg --help

# 查询已安装的包
lpkg query

# 查看网络状态
nmcli device status
```

## 安装到硬盘

如果你想将 LankeOS 安装到硬盘：

1. 启动 Live 环境
2. 运行安装器：

```bash
sudo lanke_install
```

3. 按照提示完成分区、安装和 GRUB 配置
4. 重启后拔掉 U 盘，从硬盘启动

## 包管理基础

LankeOS 使用自研的 `lpkg` 包管理器。常用命令：

```bash
# 搜索并安装软件包
sudo lpkg install <包名>

# 升级所有已安装的包
sudo lpkg upgrade

# 移除软件包
sudo lpkg remove <包名>

# 查询文件归属
lpkg query /usr/bin/bash

# 扫描未被包管理的文件
sudo lpkg scan
```

详细用法请参考 [lpkg 使用指南](/lpkg/)。

## 下一步

- 了解 [安装指南](/guide/install) — 详细的分步安装教程
- 探索 [lpkg 包管理器](/lpkg/) — LankeOS 的核心组件
- 阅读 [从源码构建](/guide/build) — 如果你想自己构建 LankeOS
- 查看 [发布历史](/releases) — 了解项目的发展历程
