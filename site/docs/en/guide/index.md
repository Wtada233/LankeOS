---
title: Quick Start
---

# Quick Start

## What is LankeOS?

**LankeOS** is a Linux distribution built from scratch based on Linux From Scratch (LFS). It is not a derivative of any existing distribution — all packages are manually configured, compiled, and packaged.

Its core strength is **automating how the distribution is built and maintained**:

- **ABI-driven build farm** `lankefarm` — incremental rebuilds keyed on link-level SONAME changes, rebuilding only what must be rebuilt
- **Custom package manager** `lpkg` (C++20) — atomic transactions and rollback, SONAME provider validation before install, dependencies derived from the artifacts' ELF rather than written by hand
- **Declarative upstream tracking** — one readable YAML per package declaring its source and filtering rules, following upstream automatically
- **Native Wayland desktop** — the KDE Plasma and GNOME core stacks, with open-source drivers across all three GPU vendors
- **Low barrier to entry** — download the ISO, boot it, run the installer; no need to compile the whole system yourself
- **Chinese-friendly support** (Fcitx5 + Noto CJK out of the box)
- **Built from source** complete toolchain

## Quick Experience

### 1. Download the ISO

Get the latest Live ISO image from the [Download page](/en/download).

### 2. Write to USB Drive

```bash
sudo dd if=lankeos-live.iso of=/dev/sdX bs=4M status=progress
```

### 3. Boot

Insert the USB drive, restart, and boot from it (select the USB drive in the BIOS/UEFI boot menu).

On first boot you'll see:

```
[    0.0] LankeOS x.x.x-lanke kernel booting
[    1.2] initramfs: found LankeOS boot media
[    1.5] initramfs: mounting rootfs.sfs
[    1.8] initramfs: setting up OverlayFS
[    2.1] switching to systemd
[    2.8] desktop ready
```

### 4. Login

The default user is `LankeOS` with password `LankeOS`. Live mode additionally does two things: it clears `root`'s password field and auto-logs in `LankeOS` (on tty1 and serial). Persistence mode and the installed system do neither, so you log in manually:

| Account | Live Mode | Persistence Mode / Installed System |
|---------|-----------|-------------------------------------|
| `LankeOS` | Auto-login; password `LankeOS` (for sudo and graphical login) | Password `LankeOS` |
| `root` | Password field cleared → **`su` accepts no password at all** (a PAM safety mechanism), so root cannot log in directly | Password was set during development and is not published; for a root shell, reset it with `sudo passwd root` |

> Get root privileges through `sudo` (as `LankeOS`). If you do want a working root password, run `sudo passwd root` first.

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
