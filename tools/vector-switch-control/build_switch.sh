#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP_DIR="$ROOT/tools/vector-switch-control"
OUT_DIR="$ROOT/artifacts/dist"

mkdir -p "$OUT_DIR"

echo "=== Building Nintendo Switch Vector Controller ==="

if [[ -n "${DEVKITPRO:-}" ]] && command -v aarch64-none-elf-g++ >/dev/null 2>&1; then
  echo "Found local devkitPro toolchain. Compiling locally..."
  make -C "$APP_DIR" clean >/dev/null
  make -C "$APP_DIR" -j4
else
  echo "devkitPro not found locally. Attempting Docker-based build..."
  command -v docker >/dev/null 2>&1 || {
    echo "error: docker is required when DEVKITPRO is not installed locally" >&2
    exit 1
  }
  docker info >/dev/null 2>&1 || {
    echo "error: docker daemon is not running" >&2
    exit 1
  }
  
  # Run docker to build the switch app
  docker run --rm \
    -v "$ROOT":/workspace \
    -w /workspace/tools/vector-switch-control \
    devkitpro/devkita64:latest \
    bash -lc 'make clean >/dev/null && make -j4'
fi

cp "$APP_DIR/vector-switch-control.nro" "$OUT_DIR/"

echo ""
echo "=== Success! Compiled Switch NRO is located in: ==="
echo "  $OUT_DIR/vector-switch-control.nro"
echo ""
