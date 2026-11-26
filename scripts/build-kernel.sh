#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "Usage: $0 <path-to-patched-linux-sm8750-piano-tree>"
  exit 1
fi

KDIR="$1"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cp "$REPO_ROOT/configs/haotian.config" "$KDIR/.config"
make -C "$KDIR" ARCH=arm64 LLVM=1 olddefconfig
make -C "$KDIR" ARCH=arm64 LLVM=1 -j"$(nproc)" Image qcom/sm8750-xiaomi-haotian.dtb modules

IMG_SIZE=$(stat -c%s "$KDIR/arch/arm64/boot/Image")
MAX_SAFE=$((41 * 1024 * 1024))
echo "==> Built Image size: $IMG_SIZE bytes"
if [ "$IMG_SIZE" -gt "$MAX_SAFE" ]; then
  echo "WARNING: Uncompressed Image exceeds ~41 MiB! ABL/SunUEFI may fail to boot."
  echo "         Keep Wi-Fi (ath12k), Bluetooth, and ASoC drivers built as modules (=m)."
fi

echo "==> Done:"
echo "    Image: $KDIR/arch/arm64/boot/Image"
echo "    DTB:   $KDIR/arch/arm64/boot/dts/qcom/sm8750-xiaomi-haotian.dtb"
