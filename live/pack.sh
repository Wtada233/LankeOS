#!/bin/bash
# 非 root 时用 sudo 重以 root 执行自身，后面全程 root，不再逐条 sudo
if [ "$EUID" -ne 0 ]; then
  exec sudo bash "$0" "$@"
fi
set -e

chown -R root:root *
pushd initramfs
rm -rf ../ISO/boot/initrd.img
touch ../ISO/boot/initrd.img
chmod 777 ../ISO/boot/initrd.img
find . | cpio -o -H newc | xz --check=crc32 --lzma2=dict=1MiB > ../ISO/boot/initrd.img
chmod 644 ../ISO/boot/initrd.img
popd
pushd ISO/live/
#rm -rf rootfs.sfs
#gensquashfs -D /mnt/lfs/ -c xz -b 1M -X dictsize=1M,level=9,x86,extreme -f rootfs.sfs
popd
pushd ISO/boot/
cp -a ../../../../../tools/krnl/{config-lanke,System.map-lanke,vmlinuz-lanke} ./
popd

# ============================================================
# 手工构建混合 ISO（替代 grub-mkrescue）
#
# 布局：
#   - efiboot.img : FAT12 loopback，strip 后只留 EFI/BOOT/BOOTX64.EFI，
#                   UEFI 固件通过 El Torito 加载它（CDROM 启动）
#   - temp/       : ISO 根目录 = loopback 全部内容(EFI/ + boot/grub/)
#                   + live 内容(kernel/initrd/grub.cfg/live/)，
#                   写进硬盘/U 盘后也能从 boot/grub 启动
#
# 步骤：
#   1) 建 FAT12 loopback 镜像并挂载（挂载成真实块设备，
#      绕过 grub-install 在 tmpfs/overlay 上无法工作的限制），
#      装 grub（--removable）。运行所需模块全部内置进核心
#      （--modules），故 --install-modules/--fonts/--locales/--themes
#      置空，boot/grub 只留 grub.cfg 和少量元数据
#   2) loopback 全部内容 cp 到 temp/
#   3) 卸载再重挂：grub-install 把 VFAT 的分配提示(hint)留在高簇，
#      直接删/拷回会让 EFI 又分到高簇；重挂后 hint 归零，清空
#      loopback、从 temp/EFI 拷回，EFI 便落到最低簇
#   4) 卸载后用 strip 脚本解析 FAT 表，把镜像截到实际使用区
#      （删掉无数据部分，FAT 删除文件不清数据区，不能按最后
#        一个非零字节截）
#   5) xorriso 把 efiboot.img 与 temp/ 打包成 lankeos-live.iso
# ============================================================

MOUNT="$(pwd)/.efi-mnt"
IMG="$(pwd)/efiboot.img"

rm -rf "$MOUNT" temp
mkdir -p "$MOUNT"

cleanup() { umount "$MOUNT" 2>/dev/null || true; }
trap cleanup EXIT

# 1) FAT12 loopback（32M：FAT12 在此规模用 16K 大簇，装得下
#    boot/grub 全量模块，且不像 FAT32 那样报集群数警告）
truncate -s 32M "$IMG"
mkfs.vfat -F 12 -n EFI "$IMG" >/dev/null
mount -o loop "$IMG" "$MOUNT"

grub-install \
  --target=x86_64-efi \
  --efi-directory="$MOUNT" \
  --boot-directory="$MOUNT/boot" \
  --modules="iso9660 udf fat part_gpt part_msdos ext2 search search_fs_uuid search_fs_file linux normal configfile echo cat all_video video_bochs video_cirrus gfxterm font serial terminal reboot halt" \
  --install-modules= \
  --fonts= \
  --locales= \
  --themes= \
  --no-nvram \
  --removable

# 2) loopback 全量内容 -> temp/，再合入 live 内容
mkdir -p temp
cp -a "$MOUNT"/. temp/
cp -a ISO/. temp/

# 3) 卸载再重挂（VFAT 分配提示归零），清空 loopback，
#    从 temp/EFI 拷回，EFI 落到最低簇，strip 才能切到最小
umount "$MOUNT"
mount -o loop "$IMG" "$MOUNT"
rm -rf "$MOUNT/boot" "$MOUNT/EFI"
cp -a temp/EFI "$MOUNT/EFI"

umount "$MOUNT"
trap - EXIT

# strip：解析 FAT 表找最高已用簇，把镜像截到那里，
# 删掉无数据（已释放但未清零）的部分。
# 注意：FAT 删除文件不清数据区，所以不能按"最后一个非零字节"截。
python3 - "$IMG" <<'EOF'
import struct, sys

def main(path):
    with open(path, 'rb') as f:
        img = f.read()
    bps = struct.unpack_from('<H', img, 11)[0]
    spc = img[13]
    reserved = struct.unpack_from('<H', img, 14)[0]
    nfat = img[16]
    root_entries = struct.unpack_from('<H', img, 17)[0]
    tot16 = struct.unpack_from('<H', img, 19)[0]
    fat16 = struct.unpack_from('<H', img, 22)[0]
    tot32 = struct.unpack_from('<I', img, 32)[0]
    fat32 = struct.unpack_from('<I', img, 36)[0]
    total = tot16 if tot16 else tot32
    fat_size = fat16 if fat16 else fat32
    root_dir = ((root_entries * 32) + (bps - 1)) // bps
    data_start = reserved + nfat * fat_size + root_dir
    total_clusters = (total - data_start) // spc
    ftype = 12 if total_clusters < 4085 else (16 if total_clusters < 65525 else 32)
    fat_off = reserved * bps
    max_used = 0
    for c in range(2, total_clusters):
        if ftype == 12:
            off = c + (c >> 1)
            b0 = img[fat_off + off]
            if c & 1:
                val = (b0 >> 4) | (img[fat_off + off + 1] << 4)
            else:
                val = b0 | ((img[fat_off + off + 1] & 0x0f) << 8)
        elif ftype == 16:
            val = struct.unpack_from('<H', img, fat_off + c * 2)[0]
        else:
            val = struct.unpack_from('<I', img, fat_off + c * 4)[0] & 0x0fffffff
        # 已分配 = 非空闲(0) 且非坏簇(0x0ff7)
        if 0x0002 <= val and val != 0x0ff7:
            max_used = c
    if max_used < 2:
        sys.exit('strip: FAT 里没有已用簇')
    keep = (data_start + (max_used - 1) * spc + spc) * bps
    if keep > len(img):
        keep = len(img)
    with open(path, 'r+b') as f:
        f.truncate(keep)
    print('strip: %d -> %d bytes (%.2f MiB), FAT%d' % (len(img), keep, keep / 1048576, ftype))

main(sys.argv[1])
EOF

cp "$IMG" temp/efiboot.img

# 5) xorriso 打包
xorriso \
  -as mkisofs \
  -o lankeos-live.iso \
  -iso-level 3 \
  -full-iso9660-filenames \
  -volid "LANKE_BASE" \
  -eltorito-alt-boot \
  -e efiboot.img \
  -no-emul-boot \
  -isohybrid-gpt-basdat \
  temp

rm -rf "$MOUNT" temp efiboot.img
rm -f ISO/live/rootfs.sfs ISO/boot/initrd.img ISO/boot/{config-lanke,System.map-lanke,vmlinuz-lanke}
#qemu-system-x86_64 \
#              -m 4G \
#              -cdrom lankeos-live.iso \
#              -vga std \
#              -serial stdio \
#              -net nic -net user \
#              -bios /usr/share/OVMF/x64/OVMF.4m.fd
chown -R "$SUDO_USER:$(id -g "$SUDO_USER")" *
