#!/usr/bin/env bash
# Copy the freshly built firmware into the tracked firmware/ directory
# (the prebuilt binary that is committed to the repository).
# Usage: ./scripts/update_firmware.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/code/Beacon/build/Beacon.uf2"
DST="$ROOT/firmware/Beacon-v0.1.uf2"

if [ ! -f "$SRC" ]; then
    echo "ERROR: $SRC not found. Run ./scripts/build.sh first." >&2
    exit 1
fi

cp "$SRC" "$DST"
echo "==> Updated: $DST"