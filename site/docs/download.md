---
title: 下载 LankeOS
editLink: false
---

# 下载 LankeOS

### 下载镜像

- [**最新Release**](https://github.com/Wtada233/LankeOS/releases/latest)

### 上面的链接也提供工作目录快照（构建指南使用的）和所有包的二进制快照

## 快速开始

### U 盘写入（推荐）

```bash
# 将 ISO 写入 U 盘（/dev/sdX 为你的 U 盘设备）
sudo dd if=lankeos-live.iso of=/dev/sdX bs=4M status=progress
sync
```

### 启动方式

- **Live 模式**：正常启动即可进入 Live 桌面环境，重启后所有更改丢失
- **持久化模式**：创建一个 LABEL=`LANKE_DATA` 的分区，系统会自动将其挂载为 OverlayFS 上层目录，实现更改持久化
- **Toram 模式**：在内核参数中添加 `toram`，将整个系统加载到内存中运行，可拔出启动介质

### 安装到硬盘

启动 Live 环境后，运行内置安装器：

```bash
sudo lanke_install
```

安装器会引导你完成分区（GPT + EFI）、格式化、数据拷贝和 GRUB 引导配置。

## 系统要求

| 硬件 | 最低配置 | 推荐配置 |
|------|---------|---------|
| CPU | x86_64-**v3** | Intel Core i3 或同等 |
| 内存 | 128 MiB（仅基础系统） | 2 GiB+（KDE 桌面） |
| 存储 | 4 GiB | 10 GiB+ |
| 显卡 | 支持 KMS/DRM | Intel / AMD / NVIDIA |
| UEFI | 64 位 UEFI | 64 位 UEFI |

> **关于这几项要求**
>
> - **CPU 需要支持 x86-64-v3**（AVX2 / FMA / BMI 等）：所有软件包都以 `-march=x86-64-v3` 构建，覆盖 2013 年后的绝大多数 x86-64 机器（Intel Haswell 及以后、AMD Excavator 及以后）。不支持 v3 的老 CPU 不保证能运行。
> - **内存取决于你跑什么桌面**：仅基础系统（无图形）约 **128 MiB** 即可启动；niri 等轻量 Wayland 合成器 + 基本 GUI 需要 **300 MiB 以上**；KDE Plasma 完整桌面建议 **2 GiB 以上**。上表"推荐"一列按 KDE 桌面给出。
> - **存储的最低 4 GiB** 来自安装器固定的启动分区大小——`live/rootfs.sfs`（约 1.5 GB）必须与内核放在同一分区上；如需持久化存储还需额外空间。
> - **显卡三厂商全部走开源驱动栈**：Intel（`iris`）、AMD（`radeonsi`）、NVIDIA（`nouveau` / NVK，含 Ada 及以后的 GSP 固件），无需闭源驱动。

> 更多版本信息请查看 [发布历史](/releases)。

## 软件包仓库

仓库地址：

```
https://lankerepo.wtada233.top/x86_64
```
