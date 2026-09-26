---
title: Installation Guide
---

# Installation Guide

## Prerequisites

- Basic Linux command-line skills
- An **x86_64-v3** UEFI system (packages are built with `-march=x86-64-v3`; legacy BIOS/MBR boot is not supported)
- A target disk that will be **completely erased**

## Installation

Run the built-in installer from the live environment:

```bash
sudo lanke_install
```

The installer walks you through the whole process:

1. **Select Disk** — lists all available disks (from `lsblk`); enter the target device name (e.g. `sda`)
2. **Confirm Erase** — requires an explicit `y`, then `wipefs` clears the old partition table
3. **Automatic Partitioning** (GPT, via `sfdisk`):
   - `LANKE_BASE` — **4 GiB**, FAT32. The boot partition, holding the kernel, initramfs, GRUB and `live/rootfs.sfs`
   - `LANKE_DATA` — remaining space, ext4. Persistent storage
4. **Format and Copy** — copies the entire system from the live medium onto `LANKE_BASE`
5. **Write GRUB Configuration** — generates the boot entry (without live-specific kernel parameters)

Reboot afterwards, remove the boot medium, and start from the hard disk.

First login: user `LankeOS`, password `LankeOS`. **Change it right after logging in.**

`root` cannot log in directly — in live mode its password field is cleared (`su` accepts no password at all, a PAM safety mechanism), and on an installed system it holds a value set during development that is not published. Use `sudo` for root privileges, or run `sudo passwd root` first to set a password.

> The installer does **not** run `grub-install`; it reuses the GRUB EFI files already present on the live medium, because `grub-install` fails inside an OverlayFS environment. This is also why the boot partition needs 4 GiB: `live/rootfs.sfs` (~1.5 GiB) has to sit on the same partition as the kernel.

### One hard constraint if you install manually

If you partition and copy things yourself instead of using the installer, remember this: **`live/rootfs.sfs` must live on the partition labelled `LANKE_BASE`.** initramfs locates the boot medium with `findfs LABEL=LANKE_BASE` and then looks for `live/rootfs.sfs` on that partition; if it is missing the system drops straight to a rescue shell. `LANKE_DATA` only serves as the OverlayFS persistence upper directory and does not hold rootfs.sfs.

## Persistent Storage (Live USB)

To keep changes on a live USB without installing to a hard disk, create a partition labelled `LANKE_DATA` and format it as ext4 — initramfs will automatically mount it as the OverlayFS upper directory at boot:

```bash
mkfs.ext4 -L LANKE_DATA /dev/sda2
```

## Troubleshooting

### Cannot enter graphical environment after boot

Check whether the graphics driver loaded:

```bash
# Look for DRM drivers in the kernel log
journalctl -b | grep -i "drm\|i915\|amdgpu\|nouveau"

# Check session type
loginctl show-session <SESSION_ID> -p Type
```

LankeOS uses open-source driver stacks for all three GPU vendors: `iris` for Intel, `radeonsi` for AMD, and `nouveau`/NVK for NVIDIA (including GSP firmware on Ada and newer), so no proprietary driver is needed.

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
