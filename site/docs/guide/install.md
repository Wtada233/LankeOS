---
title: 安装指南
---

# 安装指南

## 预备知识

- 基本的 Linux 命令行操作能力
- **x86_64-v3** UEFI 系统（软件包以 `-march=x86-64-v3` 构建；不支持传统 BIOS/MBR 启动）
- 一块将被**完全擦除**的目标磁盘

## 安装

从 Live 环境运行内置安装器：

```bash
sudo lanke_install
```

安装器会引导你完成整个流程：

1. **选择磁盘** — 列出所有可用磁盘（`lsblk` 输出），输入目标设备名（如 `sda`）
2. **确认擦除** — 需要显式输入 `y`，随后 `wipefs` 抹掉旧分区表
3. **自动分区**（GPT，`sfdisk`）：
   - `LANKE_BASE` — **4 GiB**，FAT32。启动分区，存放内核、initramfs、GRUB 与 `live/rootfs.sfs`
   - `LANKE_DATA` — 剩余空间，ext4。持久化存储
4. **格式化并拷贝** — 把 Live 介质上的系统数据完整拷贝到 `LANKE_BASE`
5. **写入 GRUB 配置** — 生成引导条目（不含 Live 专用内核参数）

完成后重启、拔掉启动介质，从硬盘启动即可。

首次登录：用户 `LankeOS`，密码 `LankeOS`。**建议登录后立即修改。**

`root` 无法直接登录——Live 模式下它的密码字段被清空（`su` 不接受任何密码，PAM 的安全机制），安装后的系统则是开发阶段所设的未公开值。需要 root shell 时用 `sudo`，或先 `sudo passwd root` 重设密码。

> 安装器**不运行 `grub-install`**，而是直接复用 Live 介质上预装好的 GRUB EFI 文件——在 OverlayFS 环境下 `grub-install` 会失败。这也是启动分区需要有 4 GiB 的原因：`live/rootfs.sfs`（约 1.5 GiB）必须与内核放在同一分区上。

### 手工安装时的一条硬性约束

如果你不用安装器、自己分区和拷贝，只要记住一件事：**`live/rootfs.sfs` 必须位于卷标为 `LANKE_BASE` 的分区上**。initramfs 用 `findfs LABEL=LANKE_BASE` 定位启动介质，然后在该分区上查找 `live/rootfs.sfs`；找不到会直接进入救援 shell。`LANKE_DATA` 只作为 OverlayFS 的持久化上层目录，不存放 rootfs.sfs。

## 持久化存储（Live USB）

如果想在 Live USB 上保留更改、而不安装到硬盘，创建一个卷标为 `LANKE_DATA` 的分区并格式化为 ext4 即可，initramfs 启动时会自动把它挂载为 OverlayFS 的上层目录：

```bash
mkfs.ext4 -L LANKE_DATA /dev/sda2
```

## 常见问题

### 启动后无法进入图形界面

检查显卡驱动是否加载：

```bash
# 查看内核日志中的 DRM 驱动
journalctl -b | grep -i "drm\|i915\|amdgpu\|nouveau"

# 确认图形会话状态
loginctl show-session <SESSION_ID> -p Type
```

LankeOS 对三大厂商的 GPU 都走开源驱动栈：Intel 用 `iris`、AMD 用 `radeonsi`、NVIDIA 用 `nouveau`/NVK（含 Ada 及以后的 GSP 固件），因此不需要安装任何闭源驱动。

### Wi-Fi 无法连接

确认 `wpa_supplicant` 是否运行：

```bash
systemctl status wpa_supplicant
nmcli device wifi list
nmcli device wifi connect <SSID> password <密码>
```

### 软件源连接失败

检查网络连通性并确认仓库配置：

```bash
lpkg --version
cat /etc/lpkg/mirror.conf
curl -I https://lankerepo.wtada233.top/x86_64/index.txt
```
