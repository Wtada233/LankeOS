---
title: Installation Guide
---

# Installation Guide

## Prerequisites

Installing LankeOS requires:
- Basic Linux command-line operations
- Understanding of disk partitioning concepts (GPT, EFI system partition, etc.)
- x86_64 UEFI system

## Method 1: Using the Built-in Installer (Recommended)

Boot into the live environment and run:

```bash
sudo lanke_install
```

Installer workflow:

1. **Select Disk** — Lists all available disks
2. **Partition Scheme** — Automatically creates GPT partitions:
   - EFI System Partition (512 MiB, Label set to `LANKE_BASE`)
   - Root partition (remaining space, Label optionally set to `LANKE_DATA`)
3. **Format and Copy** — Formats partitions and copies live system data to the hard disk
4. **Finish** — Reboot into the installed system

## Method 2: Manual Installation

### 1. Partitioning

Use `gdisk` or `fdisk` to create the following partitions:

```bash
# Example: /dev/nvme0n1
gdisk /dev/nvme0n1
```

| Partition | Size | Type | Label | Description |
|-----------|------|------|-------|-------------|
| `/dev/nvme0n1p1` | 512 MiB | EF00 (EFI System) | `LANKE_BASE` | **Required** — initramfs uses this label to locate the boot media |
| `/dev/nvme0n1p2` | Remaining | 8300 (Linux) | `LANKE_DATA` | Optional, for persistent storage |

### 2. Formatting

```bash
# EFI partition — Label MUST be set to LANKE_BASE
mkfs.fat -F 32 -n LANKE_BASE /dev/nvme0n1p1

# Root partition (if persistence is needed)
mkfs.ext4 -L LANKE_DATA /dev/nvme0n1p2
```

> The `-n LANKE_BASE` flag sets the FAT partition label. The initramfs uses `findfs LABEL=LANKE_BASE` to locate the boot partition.

### 3. Copy Data

After booting into the live environment, copy the boot files and rootfs from the ISO to the hard disk:

```bash
# Locate the boot media path (mounted by initramfs in live mode)
LIVE_MNT="/mnt/lanke_live"

# Mount the target partition
mount /dev/nvme0n1p1 /mnt

# Copy kernel, initramfs, and GRUB EFI bootloader
cp -a "$LIVE_MNT/boot" /mnt/
cp -a "$LIVE_MNT/EFI" /mnt/

# If using a persistence partition, copy rootfs.sfs as well
mount /dev/nvme0n1p2 /data
cp -a "$LIVE_MNT/live" /data/
```

> Note: Running `grub-install` within an OverlayFS environment will fail, so we directly copy the pre-installed GRUB EFI files from the Live ISO.

### 4. Configure GRUB

The installed system does not need the `live` or `toram` kernel parameters. Write a new GRUB configuration:

```bash
cat > /mnt/boot/grub/grub.cfg << 'EOF'
# ---------------------------------------------------------
# LankeOS GRUB Configuration
# ---------------------------------------------------------

# Load basic modules
insmod part_gpt
insmod part_msdos
insmod fat
insmod iso9660
insmod ext2

# Video output
insmod all_video
insmod video_bochs
insmod video_cirrus
insmod gfxterm

# Basic display settings
terminal_input console
terminal_output console

# Timeout and defaults
set timeout=10
set default=0
set menu_color_normal=white/black
set menu_color_highlight=black/light-gray

# ---------------------------------------------------------
# Boot Entries
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

> Unlike live mode, the installed system runs **without** the `live` parameter. This tells initramfs to skip live-specific steps like password clearing, auto-login injection, and installer execution. If a `LANKE_DATA` partition is present, initramfs will automatically mount it as an OverlayFS upper directory for persistence.

## Persistent Storage

To use persistent storage on a Live USB (retaining changes after reboot):

1. Create a second partition on the USB drive with label `LANKE_DATA`
2. Format as ext4
3. The system will automatically detect it and mount it as the OverlayFS upper directory on boot

```bash
# Example: create a persistence partition on /dev/sda
# First create the second partition
gdisk /dev/sda
# Then format it
mkfs.ext4 -L LANKE_DATA /dev/sda2
```

## Troubleshooting

### Cannot enter graphical environment after boot

Check graphics driver support:

```bash
# View system logs
journalctl -b | grep -i "drm\|i915\|amdgpu\|nouveau"

# Check session type
loginctl show-session <SESSION_ID> -p Type
```

### Wi-Fi not connecting

Check if wpa_supplicant is running:

```bash
systemctl status wpa_supplicant
nmcli device wifi list
nmcli device wifi connect <SSID> password <password>
```

### Repository connection failure

Check network connectivity and mirror configuration:

```bash
lpkg --version
cat /etc/lpkg/mirror.conf
curl -I https://lankerepo.wtada233.top/x86_64/index.txt
```
