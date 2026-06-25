---
title: 安装指南
---

# 安装指南

## 预备知识

安装 LankeOS 需要以下基础：
- 基本的 Linux 命令行操作能力
- 了解磁盘分区概念（GPT、EFI 系统分区等）
- x86_64 UEFI 系统

## 方式一：使用内置安装器（推荐）

启动 Live 环境后，运行：

```bash
sudo lanke_install
```

安装器交互流程：

1. **选择磁盘** — 列出所有可用磁盘
2. **分区方案** — 自动创建 GPT 分区：
   - EFI 系统分区（512 MiB，Label 设为 `LANKE_BASE`）
   - Root 分区（剩余全部空间，Label 可选设为 `LANKE_DATA`）
3. **格式化和拷贝** — 格式化分区并将 Live 系统数据拷贝到硬盘
4. **完成** — 重启进入已安装系统

## 方式二：手动安装

如果你想手动控制安装过程：

### 1. 分区

使用 `gdisk` 或 `fdisk` 创建以下分区：

```bash
# 示例：/dev/nvme0n1
gdisk /dev/nvme0n1
```

| 分区 | 大小 | 类型 | Label | 说明 |
|------|------|------|-------|------|
| `/dev/nvme0n1p1` | 512 MiB | EF00 (EFI System) | `LANKE_BASE` | **必需** — initramfs 通过此 Label 定位启动介质 |
| `/dev/nvme0n1p2` | 剩余 | 8300 (Linux) | `LANKE_DATA` | 可选，设为持久化存储分区 |

### 2. 格式化

```bash
# EFI 分区 — Label 必须设为 LANKE_BASE
mkfs.fat -F 32 -n LANKE_BASE /dev/nvme0n1p1

# Root 分区（如需要持久化）
mkfs.ext4 -L LANKE_DATA /dev/nvme0n1p2
```

> `-n LANKE_BASE` 给 FAT 分区设置卷标，initramfs 中的 `findfs LABEL=LANKE_BASE` 靠它找到启动分区。

### 3. 拷贝数据

启动 Live 环境后，将 ISO 中的引导文件和 rootfs 复制到硬盘：

```bash
# 确定启动介质路径（Live 模式下 initramfs 挂载在此）
LIVE_MNT="/mnt/lanke_live"

# 挂载目标分区
mount /dev/nvme0n1p1 /mnt

# 复制内核、initramfs 和 GRUB EFI 引导
cp -a "$LIVE_MNT/boot" /mnt/
cp -a "$LIVE_MNT/EFI" /mnt/

# 如果使用了持久化分区，将 rootfs.sfs 也复制过去
mount /dev/nvme0n1p2 /data
cp -a "$LIVE_MNT/live" /data/
```

> 注意：在 OverlayFS 环境下运行 `grub-install` 会失败，因此直接复制 Live ISO 中预装好的 GRUB EFI 文件即可。

### 4. 配置 GRUB

安装后的系统不需要 `live` 和 `toram` 这些 Live 专用内核参数，需要写入一份新的 GRUB 配置：

```bash
cat > /mnt/boot/grub/grub.cfg << 'EOF'
# ---------------------------------------------------------
# LankeOS GRUB Configuration
# ---------------------------------------------------------

# 加载基础模块
insmod part_gpt
insmod part_msdos
insmod fat
insmod iso9660
insmod ext2

# 视频输出
insmod all_video
insmod video_bochs
insmod video_cirrus
insmod gfxterm

# 基础显示设置
terminal_input console
terminal_output console

# 设置超时与默认选项
set timeout=10
set default=0
set menu_color_normal=white/black
set menu_color_highlight=black/light-gray

# ---------------------------------------------------------
# 引导条目
# ---------------------------------------------------------

menuentry "LankeOS" --class lankeos --class gnu-linux {
    echo "Loading LankeOS Kernel..."
    linux /boot/vmlinuz-lanke rw loglevel=3 console=ttyS0 console=tty1

    echo "Loading LankeOS Initramfs..."
    initrd /boot/initrd.img
}

menuentry "Reboot System" {
    reboot
}

menuentry "Power Off" {
    halt
}
EOF
```

> 与 Live 模式不同，安装后的系统**不加** `live` 参数，这样 initramfs 会跳过密码清除、自动登录注入和安装程序写入等 Live 专用步骤。如果挂了 `LANKE_DATA` 分区，initramfs 会自动将其挂载为 OverlayFS upperdir 实现持久化。

## 持久化存储

如果你想在 Live USB 上使用持久化存储（重启后保留更改）：

1. 在 U 盘上创建第二个分区，标签设为 `LANKE_DATA`
2. 格式化为 ext4
3. 系统启动时会自动检测并将其挂载为 OverlayFS 的上层目录

```bash
# 示例：在 /dev/sda 上创建持久化分区
# 先创建第二个分区
gdisk /dev/sda
# 然后格式化
mkfs.ext4 -L LANKE_DATA /dev/sda2
```

## 常见问题

### 启动后无法进入图形界面

检查显卡驱动支持：

```bash
# 查看系统日志
journalctl -b | grep -i "drm\|i915\|amdgpu\|nouveau"

# 确认图形会话状态
loginctl show-session <SESSION_ID> -p Type
```

### Wi-Fi 无法连接

确认 wpa_supplicant 是否运行：

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
