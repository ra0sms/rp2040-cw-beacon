#!/usr/bin/env bash
# Copy the freshly built firmware into the tracked firmware/ directory
# (the prebuilt binary that is committed to the repository).
# Usage: ./scripts/update_firmware.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/code/Beacon/build/Beacon.uf2"
# File name follows the program version set in CMakeLists.txt
VERSION="$(sed -n 's/^set(BEACON_VERSION "\(.*\)")/\1/p' "$ROOT/code/Beacon/CMakeLists.txt")"
DST="$ROOT/firmware/Beacon-v$VERSION.uf2"

if [ ! -f "$SRC" ]; then
    echo "ERROR: $SRC not found. Run ./scripts/build.sh first." >&2
    exit 1
fi

# Keep only the current version in firmware/
for old in "$ROOT"/firmware/Beacon-v*.uf2; do
    [ "$old" != "$DST" ] && [ -e "$old" ] && git -C "$ROOT" rm -q "$old" 2>/dev/null || true
done
mkdir -p "$(dirname "$DST")"
cp "$SRC" "$DST"
echo "==> Updated: $DST"