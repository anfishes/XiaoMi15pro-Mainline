#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "Usage: $0 <path-to-linux-sm8750-piano-tree>"
  exit 1
fi

KDIR="$1"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

for p in "$REPO_ROOT"/patches/*.patch; do
  echo "==> Applying $(basename "$p")"
  patch -d "$KDIR" -p1 < "$p"
done

echo "All patches applied cleanly."
