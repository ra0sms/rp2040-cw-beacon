#!/usr/bin/env bash
# Build the beacon firmware.
# Usage: ./scripts/build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/code/Beacon/build"

# Configure once (if not configured yet)
if [ ! -f "$BUILD_DIR/build.ninja" ] && [ ! -f "$BUILD_DIR/Makefile" ]; then
    echo "==> Configuring CMake..."
    cmake -B "$BUILD_DIR" -S "$ROOT/code/Beacon"
fi

echo "==> Building..."
cmake --build "$BUILD_DIR"

echo "==> Done: $BUILD_DIR/Beacon.uf2"