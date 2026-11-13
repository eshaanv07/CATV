#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
BR_DIR="${BR2_DIR:-/opt/os-track/src/bases/buildroot-2025.02}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/build}"
JOBS="${JOBS:-2}"

if [ ! -f "$BR_DIR/Makefile" ]; then
    echo "ERROR: Buildroot source not found: $BR_DIR"
    exit 1
fi

mkdir -p "$OUT_DIR"
OUT_DIR="$(CDPATH= cd -- "$OUT_DIR" && pwd)"

echo "Project:  $ROOT_DIR"
echo "Buildroot: $BR_DIR"
echo "Output:    $OUT_DIR"

cp "$ROOT_DIR/buildroot.config" "$OUT_DIR/.config"

sed -i \
    "s|^BR2_ROOTFS_OVERLAY=.*|BR2_ROOTFS_OVERLAY=\"$ROOT_DIR/overlay\"|" \
    "$OUT_DIR/.config"

sed -i \
    "s|^BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE=.*|BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE=\"$ROOT_DIR/kernel.config\"|" \
    "$OUT_DIR/.config"

make -C "$BR_DIR" O="$OUT_DIR" olddefconfig

make -C "$BR_DIR" O="$OUT_DIR" -j"$JOBS"

cp "$OUT_DIR/images/rootfs.ext2" "$OUT_DIR/system.img"

echo "Build completed successfully."
echo "Kernel:     $OUT_DIR/images/bzImage"
echo "Rootfs:     $OUT_DIR/images/rootfs.ext2"
echo "System:     $OUT_DIR/system.img"
