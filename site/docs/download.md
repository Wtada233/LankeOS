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
sudo dd if=lankeos-0.09-live.iso of=/dev/sdX bs=4M status=progress
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
| CPU | x86_64, 单核 | Intel Core i3 或同等 |
| 内存 | 300 MiB | 4 GiB+ |
| 存储 | 1 GiB | 10 GiB+ |
| 显卡 | 支持 KMS/DRM | Intel / AMD GPU |
| UEFI | 64位 UEFI | 64位 UEFI |

> 更多版本信息请查看 [发布历史](/releases)。

## 软件包仓库

LankeOS 使用腾讯云 COS 作为软件包分发后端：

```
Repository: https://lankerepo.wtada233.top/x86_64
```
