//! 原 initramfs/modules/*/content 的数据，直接编进二进制。

pub const AUTOLOGIN: &str = r###"[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin LankeOS --noclear %I $TERM
"###;

pub const LIVE_NOTICE: &str = r###"================================================================
 LankeOS Live mode.
 Default Password is LankeOS
================================================================
"###;

pub const UPDATE_NOTICE: &str = r###"================================================================
 LankeOS system update detected.
 The current system image version is different from the previous
 persistent installation.
 The system is running in LIVE upgrade mode.
 You can:
 1. Remove:
       LANKE_DATA/upper/etc/lanke-release-curr
    Then reboot to accept the new base system.
 2. Reset your modifications under /usr if required.
 3. Keep your personal data under /home.
 A future upgrade mechanism can migrate system files while
 preserving user data.
================================================================
"###;

pub const INSTALLER: &str = r###"#!/bin/bash
set -e
[ "$EUID" -ne 0 ] && { echo "Error: Please run as root"; exit 1; }
echo "=== LankeOS Installer (Fixed Partitioning) ==="
DISKS=$(lsblk -dno NAME,SIZE,MODEL | grep -v "loop")
echo "Available disks:"
echo "$DISKS"
read -p "Enter target disk (e.g., sda): " TARGET_DISK
TARGET_DEV="/dev/$TARGET_DISK"
[ -b "$TARGET_DEV" ] || { echo "Error: $TARGET_DEV not found"; exit 1; }
read -p "WARNING: Data on $TARGET_DEV will be wiped. Continue? (y/N): " CONFIRM
[[ "$CONFIRM" =~ ^[yY]$ ]] || exit 1
echo "Partitioning with sfdisk..."
wipefs -a "$TARGET_DEV"
# P1: 8G (EFI + BASE), P2: Remainder (DATA)
sfdisk "$TARGET_DEV" << EOF
label: gpt
size=4G, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="LANKE_BASE"
type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name="LANKE_DATA"
EOF
PART_PREFIX=""
[[ "$TARGET_DEV" =~ [0-9]$ ]] && PART_PREFIX="p"
echo "Formatting partitions..."
mkfs.fat -F32 -n LANKE_BASE "${TARGET_DEV}${PART_PREFIX}1"
mkfs.ext4 -F -L LANKE_DATA "${TARGET_DEV}${PART_PREFIX}2"
echo "Copying LiveCD data to LANKE_BASE..."
mkdir -p /mnt/inst_base
mount "${TARGET_DEV}${PART_PREFIX}1" /mnt/inst_base
cp -a --no-preserve=mode,ownership /mnt/lanke_live/* /mnt/inst_base/
echo "Configuring GRUB..."
cat > /mnt/inst_base/boot/grub/grub.cfg << 'EGRUBCFG'
# ---------------------------------------------------------
# LankeOS GRUB Configuration
# ---------------------------------------------------------
insmod part_gpt
insmod part_msdos
insmod fat
insmod iso9660
insmod ext2
insmod all_video
insmod video_bochs
insmod video_cirrus
insmod gfxterm
terminal_input console
terminal_output console
set timeout=10
set default=0
set menu_color_normal=white/black
set menu_color_highlight=black/light-gray

menuentry "LankeOS" --class lankeos --class gnu-linux {
    echo "Loading LankeOS Kernel..."
    linux /boot/vmlinuz-lanke loglevel=3 console=ttyS0 console=tty1   
    echo "Loading LankeOS Initramfs..."
    initrd /boot/initrd.img
}
menuentry "Reboot System" {
    reboot
}
menuentry "Power Off" {
    halt
}
EGRUBCFG
# 清理
umount /mnt/inst_base
echo "LankeOS Installed Successfully! Default User: LankeOS. Default Password: LankeOS"
"###;

pub const MEDIUM_SHUTDOWN: &str = r###"[Unit]
Description=Prepare LankeOS boot medium for shutdown
After=mnt-lanke_live.mount mnt-base.mount

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/bin/true
ExecStop=-/usr/bin/sh -c 'if mountpoint -q /mnt/lanke_live; then mount -o remount,ro /mnt/lanke_live; umount -l /mnt/lanke_live; fi; if mountpoint -q /mnt/base; then mount -o remount,ro /mnt/base; umount -l /mnt/base; fi'

[Install]
WantedBy=multi-user.target
"###;

pub const MEDIUM_LAZY: &str = r###"[Mount]
LazyUnmount=yes
"###;
