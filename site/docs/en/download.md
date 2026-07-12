---
title: Download LankeOS
editLink: false
---

# Download LankeOS

### Download Image

- [**Latest Release**](https://github.com/Wtada233/LankeOS/releases/latest)

### The link above also provides workdir snapshots (used by the build guide) and binary snapshots of all packages.

## Quick Start

### Write to USB Drive (Recommended)

```bash
# Write the ISO to a USB drive (/dev/sdX is your USB device)
sudo dd if=lankeos-0.09-live.iso of=/dev/sdX bs=4M status=progress
sync
```

### Boot Modes

- **Live Mode**: Boot normally to enter the live desktop environment. All changes are lost after reboot.
- **Persistence Mode**: Create a partition with label `LANKE_DATA`. The system will automatically mount it as the OverlayFS upper directory, enabling persistent changes.
- **Toram Mode**: Add `toram` to the kernel parameters to load the entire system into RAM. You can remove the boot media after booting.

### Install to Hard Disk

After booting into the live environment, run the built-in installer:

```bash
sudo lanke_install
```

The installer will guide you through partitioning (GPT + EFI), formatting, data copy, and GRUB bootloader configuration.

## System Requirements

| Hardware | Minimum | Recommended |
|----------|---------|-------------|
| CPU | x86_64, single core | Intel Core i3 or equivalent |
| Memory | 300 MiB | 4 GiB+ |
| Storage | 1 GiB | 10 GiB+ |
| Graphics | KMS/DRM support | Intel / AMD GPU |
| UEFI | 64-bit UEFI | 64-bit UEFI |

> Check the [Release History](/en/releases) for more version information.

## Package Repository

LankeOS uses Tencent Cloud COS as its package distribution backend:

```
Repository: https://lankerepo.wtada233.top/x86_64
```
