#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 2 ]; then
  echo "Usage: $0 <path-to-audioreach-topology> <path-to-piano-audio-dir>"
  echo "Example: $0 ./audioreach-topology ./xiaomipad-8pro-mainline/device/audio"
  exit 1
fi

AR_TPLG="$1"
PIANO_AUDIO="$2"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TDIR="$REPO_ROOT/audio/topology"

m4 -I "$TDIR" -I "$PIANO_AUDIO" -I "$AR_TPLG" \
  "$TDIR/Xiaomi-15-Pro.m4" > "$TDIR/Xiaomi-15-Pro.conf"

alsatplg -c "$TDIR/Xiaomi-15-Pro.conf" -o "$TDIR/Xiaomi-15-Pro-tplg.bin"
echo "==> Built $TDIR/Xiaomi-15-Pro-tplg.bin"
