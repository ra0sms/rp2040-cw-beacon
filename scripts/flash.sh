#!/usr/bin/env bash
# Flash the built firmware to the RP2040-Zero via picotool (no BOOTSEL button).
# Usage: ./scripts/flash.sh
# Optional: PICOTOOL=/path/to/picotool ./scripts/flash.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UF2="$ROOT/code/Beacon/build/Beacon.uf2"

if [ ! -f "$UF2" ]; then
    echo "ERROR: $UF2 not found. Run ./scripts/build.sh first." >&2
    exit 1
fi

# Locate picotool: explicit env var, then the Pico VS Code extension copy,
# then PATH
PICOTOOL="${PICOTOOL:-}"
if [ -z "$PICOTOOL" ]; then
    for c in "$HOME"/.pico-sdk/picotool/*/picotool/picotool \
             "$HOME"/.pico-sdk/picotool/*/picotool; do
        if [ -x "$c" ]; then
            PICOTOOL="$c"
            break
        fi
    done
fi
PICOTOOL="${PICOTOOL:-picotool}"

echo "==> Flashing $UF2 with $PICOTOOL"
# The first attempt often gives up before the board has re-enumerated in
# BOOTSEL mode after the USB reboot request - retry once
if ! "$PICOTOOL" load "$UF2" -fx; then
    echo "==> Retrying..."
    sleep 2
    "$PICOTOOL" load "$UF2" -fx
fi
echo "==> Done"