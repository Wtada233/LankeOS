---
title: Quick Start
---

# Quick Start

## What is LankeOS?

**LankeOS** is a Linux distribution built from scratch based on Linux From Scratch (LFS). It is not a derivative of any existing distribution — all packages are manually configured, compiled, and packaged.

Core features:

- **Custom package manager** `lpkg` (C++20)
- **Native Wayland desktop** (Sway tiling window manager)
- **Extremely fast boot** (~4 seconds to desktop as of v0.12)
- **Built from source** complete toolchain
- **Chinese-first support** (Fcitx5 + Noto CJK out of the box)

## Quick Experience

### 1. Download the ISO

Get the latest Live ISO image from the [Download page](/en/download).

### 2. Write to USB Drive

```bash
sudo dd if=lankeos-0.09-live.iso of=/dev/sdX bs=4M status=progress
```

### 3. Boot

Insert the USB drive, restart, and boot from it (select the USB drive in the BIOS/UEFI boot menu).

On first boot you'll see:

```
[    0.0] LankeOS 7.1.1-lanke kernel booting
[    1.2] initramfs: found LankeOS boot media
[    1.5] initramfs: mounting rootfs.sfs
[    1.8] initramfs: setting up OverlayFS
[    2.1] switching to systemd
[    2.8] Sway desktop ready
```

### 4. Login

Usernames and passwords (automatic login in Live mode):

| Account | Live Mode | Persistence Mode |
|---------|-----------|------------------|
| `root` | Auto-login | Password is empty |
| `LankeOS` | Auto-login | Password is empty |

### 5. Explore the System

```bash
# View system information
fastfetch

# Check the package manager
sudo lpkg --help

# Query installed packages
lpkg query

# Check network status
nmcli device status
```

## Install to Hard Disk

To install LankeOS to a hard disk:

1. Boot into the live environment
2. Run the installer:

```bash
sudo lanke_install
```

3. Follow the prompts to complete partitioning, installation, and GRUB configuration
4. Reboot and remove the USB drive, then boot from the hard disk

## Package Management Basics

LankeOS uses the custom `lpkg` package manager. Common commands:

```bash
# Search and install a package
sudo lpkg install <package-name>

# Upgrade all installed packages
sudo lpkg upgrade

# Remove a package
sudo lpkg remove <package-name>

# Query file ownership
lpkg query /usr/bin/bash

# Scan for files not managed by any package
sudo lpkg scan
```

## Next Steps

- Read the [Installation Guide](/en/guide/install) — detailed step-by-step installation tutorial
- Explore the [lpkg Package Manager](/en/lpkg/) — LankeOS's core component
- Read [Build from Source](/en/guide/build) — if you want to build LankeOS yourself
- Check the [Release History](/en/releases) — learn about the project's development journey
