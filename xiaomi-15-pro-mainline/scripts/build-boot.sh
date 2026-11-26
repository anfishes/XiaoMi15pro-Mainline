#!/bin/sh
# Package haotian boot images from a built kernel.
#
# Usage: build-boot.sh IMAGE [--dtb DTB] [--ramdisk RD] [--out DIR]
#
#   IMAGE     arch/arm64/boot/Image from the kernel tree
#   --dtb     device tree to package (default: the known-good 107992-byte tree)
#   --ramdisk initramfs (cpio, gzip or xz compressed)
#   --out     output directory (default: boot/out)
#
# This wraps the two host tools already used for this device and applies no
# firmware of its own: the BootShim/UEFI firmware (SILICIUM_UEFI.fd) comes from
# the SunUEFI tree.
#
# It refuses two things that cost hours here:
#   - a ramdisk above the observed live limit, rather than a silent truncation
#   - overwriting an existing output directory
set -eu

here=$(cd "$(dirname "$0")" && pwd)
device=$(cd "$here/.." && pwd)
port=$(cd "$device/../.." && pwd)
sunuefi=${SUNUEFI:-$port/repos/Project-SunUEFI-main}

image=
dtb=$port/artifacts-dtb/haotian-working-107992-no-role.dtb
ramdisk=
out=$here/out

while [ $# -gt 0 ]; do
	case $1 in
	--dtb) dtb=$2; shift 2 ;;
	--ramdisk) ramdisk=$2; shift 2 ;;
	--out) out=$2; shift 2 ;;
	-*) echo "unknown option $1" >&2; exit 2 ;;
	*) image=$1; shift ;;
	esac
done

[ -n "$image" ] || { echo "usage: build-boot.sh IMAGE [--dtb DTB] [--ramdisk RD] [--out DIR]" >&2; exit 2; }
[ -f "$image" ] || { echo "no such image: $image" >&2; exit 1; }
[ -f "$dtb" ] || { echo "no such dtb: $dtb" >&2; exit 1; }
[ -d "$sunuefi" ] || { echo "SunUEFI tree not found at $sunuefi (set SUNUEFI=)" >&2; exit 1; }

# The live initrd ceiling observed on this device.
limit=30291000
if [ -n "$ramdisk" ]; then
	[ -f "$ramdisk" ] || { echo "no such ramdisk: $ramdisk" >&2; exit 1; }
	size=$(wc -c < "$ramdisk")
	[ "$size" -le "$limit" ] ||
		{ echo "ramdisk is $size bytes, above the observed $limit limit" >&2; exit 1; }
fi

# Directory names are derived from --out; refuse to reuse one.
label=$(basename "$out")
payload=$sunuefi/artifacts/$label-payload
imgdir=$sunuefi/artifacts/$label
[ ! -e "$payload" ] || { echo "$payload already exists; choose a new --out name" >&2; exit 1; }
[ ! -e "$imgdir" ] || { echo "$imgdir already exists; choose a new --out name" >&2; exit 1; }

echo "kernel : $image"
echo "dtb    : $dtb ($(sha256sum "$dtb" | cut -d' ' -f1))"
[ -z "$ramdisk" ] || echo "ramdisk: $ramdisk"
echo "payload: $payload"

# Build the argument list explicitly, one shell word per option. Using $@
# here previously dropped --dtb, and the packager fell back to its pinned
# haotian.dtb, which is the variant that goes black on this device.
set -- --raw --image "$image" --dtb "$dtb" --output-dir "$payload"
if [ -n "$ramdisk" ]; then
	set -- "$@" --ramdisk "$ramdisk"
fi

echo "== payload"
python3 "$sunuefi/tools/package_haotian_linux_payload.py" "$@"

echo "== boot image"
python3 "$sunuefi/tools/package_haotian_linux_validate_image.py" --raw-linux \
	--payload-dir "$payload" --output-dir "$imgdir"

# Verify the packaged tree really is the one that was asked for, by reading
# the bytes out of the payload. A silent substitution here is what produced a
# black screen earlier. The manifest's `inputs` block records the packager's
# default DTB rather than an override, so it is not evidence; the payload is.
want=$(sha256sum "$dtb" | cut -d' ' -f1)
packaged=$(python3 - "$payload/linux-payload.bin" <<'PY'
import hashlib, struct, sys
p = open(sys.argv[1], 'rb').read()
klen, ilen = struct.unpack_from('<QQ', p, 24)
print(hashlib.sha256(p[192 + klen + ilen:]).hexdigest())
PY
)
[ "$packaged" = "$want" ] || {
	echo "packaged DTB is $packaged, expected $want" >&2
	echo "refusing to keep an image built with the wrong device tree" >&2
	exit 1
}
echo "dtb verified in payload: $packaged"

mkdir -p "$out"
cp "$imgdir/haotian-linux-raw-v3.img" "$out/boot.img"
echo
echo "boot.img: $(sha256sum "$out/boot.img" | cut -d' ' -f1)"
echo "RAM-boot it (writes nothing): fastboot boot $out/boot.img"