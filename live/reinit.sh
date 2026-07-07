#!/bin/bash
# LankeOS Reinit Script (Run from Host System)
# 目标：清理所有使用痕迹，将系统恢复至“纯净固化”状态

TARGET="/mnt/lfs"

if [ "$EUID" -ne 0 ]; then
  echo "错误：请以 root 权限运行。"
  exit 1
fi

if [ ! -d "$TARGET/etc" ] || [ ! -d "$TARGET/usr" ]; then
  echo "错误：在 $TARGET 未发现有效的 LankeOS 根目录。"
  exit 1
fi

echo "=> 正在为 LankeOS ($TARGET) 进行深度重置..."

# 1. 处理身份标识 (Machine ID)
echo "   [1/5] 重置 Machine ID..."
rm -f $TARGET/etc/machine-id
rm -f $TARGET/var/lib/dbus/machine-id
# 创建一个空的 /etc/machine-id 占位符，这是 systemd 触发首次启动逻辑的关键
touch $TARGET/etc/machine-id
# 重新建立规范的 D-Bus 软链接
mkdir -p $TARGET/var/lib/dbus
ln -sf /etc/machine-id $TARGET/var/lib/dbus/machine-id

# 2. 清理运行时碎屑与日志
echo "   [2/5] 清理运行时数据与日志..."
rm -rf $TARGET/var/log/*
rm -rf $TARGET/var/cache/ldconfig/*
rm -rf $TARGET/var/lib/systemd/random-seed
rm -rf $TARGET/var/lib/systemd/timers/stamp-*
rm -rf $TARGET/var/tmp/*
rm -rf $TARGET/tmp/*
rm -f $TARGET/etc/.updated $TARGET/var/.updated

# 3. 清理账户备份与锁文件
echo "   [3/5] 清理账户备份与安全锁..."
rm -f $TARGET/etc/*-
rm -f $TARGET/etc/*.lock
rm -f $TARGET/etc/group- $TARGET/etc/passwd- $TARGET/etc/shadow- $TARGET/etc/gshadow-
# 物理删除所有 SSH 主机密钥，确保固化镜像不含私钥
rm -f $TARGET/etc/ssh/ssh_host_*

# 4. 修正目录权限与状态
echo "   [4/5] 修正关键目录状态..."
# 确保 resolv.conf 链接到 systemd-resolved (如果你用了的话) 或者清空
# > $TARGET/etc/resolv.conf 
# 确保 mtab 是软链接
ln -sf /proc/self/mounts $TARGET/etc/mtab

# 5. 清理 lpkg 扫描残留 (可选)
echo "   [5/5] 清理 lpkg 缓存锁..."
rm -f $TARGET/var/lpkg/db.lck
rm -f $TARGET/etc/udev/hwdb.bin

echo "------------------------------------------------"
echo "=> 重置成功！LankeOS 现在处于“出厂纯净”状态。"
echo "   现在可以安全地执行: mksquashfs $TARGET lankeos.sqsh ..."
