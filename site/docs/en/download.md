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
sudo dd if=lankeos-live.iso of=/dev/sdX bs=4M status=progress
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
| CPU | x86_64-**v3** | Intel Core i3 or equivalent |
| Memory | 128 MiB (base system only) | 2 GiB+ (KDE desktop) |
| Storage | 4 GiB | 10 GiB+ |
| Graphics | KMS/DRM support | Intel / AMD / NVIDIA |
| UEFI | 64-bit UEFI | 64-bit UEFI |

> **About these requirements**
>
> - **The CPU must support x86-64-v3** (AVX2 / FMA / BMI and friends): every package is built with `-march=x86-64-v3`, which covers the vast majority of x86-64 machines made since 2013 (Intel Haswell and later, AMD Excavator and later). Older CPUs without v3 support are not guaranteed to run.
> - **Memory depends on the desktop you run**: the base system (no graphics) boots in about **128 MiB**; a lightweight Wayland compositor such as niri plus a basic GUI needs **300 MiB or more**; the full KDE Plasma desktop is best with **2 GiB or more**. The table's "Recommended" column assumes KDE.
> - **The 4 GiB storage minimum** comes from the installer's fixed boot-partition size — `live/rootfs.sfs` (~1.5 GB) has to sit on the same partition as the kernel. Persistent storage needs additional space.
> - **All three GPU vendors run open-source driver stacks**: Intel (`iris`), AMD (`radeonsi`) and NVIDIA (`nouveau` / NVK, including GSP firmware on Ada and newer). No proprietary driver is required.

> Check the [Release History](/en/releases) for more version information.

## Package Repository

Repository URL:

```
https://lankerepo.wtada233.top/x86_64
```
