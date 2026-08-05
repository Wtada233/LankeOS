#!/bin/sh
# 从 chromium 源码的 DEPS 读取 GCS 组件并下载解压。
# 用法: fetch_gcs.sh <deps_key> <object_prefix> <dest_dir> <marker_path>
#   在 chromium 源码根目录运行（DEPS 必须在当前目录）。
#   幂等：marker 已存在则跳过（源码树保留时避免重复下载）。
set -e

_g_key="$1"; _g_pat="$2"; _g_dest="$3"; _g_marker="$4"
[ -n "$_g_key" ] || { echo "用法: fetch_gcs.sh <deps_key> <object_prefix> <dest_dir> <marker_path>"; exit 1; }

# key 含字面 +（如 llvm-build/Release+Asserts），必须 grep -F（fixed string）
_g_bucket=$(grep -F -A15 "'$_g_key'" DEPS \
    | grep -oE "'bucket': '[^']*'" | sed "s/.*'bucket': '//;s/'//" | head -1)
[ -n "$_g_bucket" ] || _g_bucket=chromium-browser-clang
_g_obj=$(grep -F -A30 "'$_g_key'" DEPS \
    | grep -oE "'object_name': '${_g_pat}[^']*'" | sed "s/.*'object_name': '//;s/'//" | head -1)
_g_sha=$(grep -F -A30 "'$_g_key'" DEPS \
    | grep -oE "'sha256sum': '[^']*'" | sed "s/.*'sha256sum': '//;s/'//" | head -1)

if [ -n "$_g_obj" ] && [ ! -e "$_g_marker" ]; then
    echo "下载 GCS 组件: $_g_obj"
    curl -fsSL "https://commondatastorage.googleapis.com/$_g_bucket/$_g_obj" \
        -o gcs-fetch.tar.xz
    if [ -n "$_g_sha" ]; then
        echo "$_g_sha  gcs-fetch.tar.xz" | sha256sum -c - \
            || { echo "GCS 组件 sha256 校验失败"; exit 1; }
    fi
    mkdir -p "$_g_dest"
    tar -xJf gcs-fetch.tar.xz -C "$_g_dest"
    rm -f gcs-fetch.tar.xz
fi
