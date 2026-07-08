#!/bin/bash
sudo chown -R root:root *
pushd initramfs
sudo rm -rf ../ISO/boot/initrd.img
sudo touch ../ISO/boot/initrd.img
sudo chmod 777 ../ISO/boot/initrd.img
sudo find . | cpio -o -H newc | xz --check=crc32 --lzma2=dict=1MiB > ../ISO/boot/initrd.img
sudo chmod 644 ../ISO/boot/initrd.img
popd
pushd ISO/live/
sudo rm -rf rootfs.sfs
sudo gensquashfs -D /mnt/lfs/ -c xz -b 1M -X dictsize=1M,level=9,x86,extreme -f rootfs.sfs
popd

sudo grub-mkrescue --directory=/usr/lib/grub/x86_64-efi -o lankeos-live.iso ISO -- -volid "LANKE_BASE"
sudo rm ISO/live/rootfs.sfs ISO/boot/initrd.img
#qemu-system-x86_64 \
#              -m 4G \
#              -cdrom lankeos-live.iso \
#              -vga std \
#              -serial stdio \
#              -net nic -net user \
#              -bios /usr/share/OVMF/x64/OVMF.4m.fd
sudo chown -R $UID:$(id -g) *
