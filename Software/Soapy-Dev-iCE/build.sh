#!/bin/bash
# build.sh — Build and install the SoapySDR driver for Pico-Dev-iCE & WWU 2026 SDR on Linux/macOS.
# Usage: bash build.sh [--clean] [--no-install]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

CLEAN=0; NO_INSTALL=0
for arg in "$@"; do
    [[ $arg == --clean ]]      && CLEAN=1
    [[ $arg == --no-install ]] && NO_INSTALL=1
done

if [[ $CLEAN -eq 1 ]]; then
    echo "== Cleaning build directory =="
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "== Configuring =="
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local

echo "== Building =="
make -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"

if [[ $NO_INSTALL -eq 0 ]]; then
    echo "== Installing (may prompt for sudo) =="
    sudo make install
    sudo ldconfig 2>/dev/null || true
    echo ""
    echo "== Verifying installation =="
    SoapySDRUtil --find="driver=2026sdr" 2>/dev/null \
        && echo "Board found!" \
        || echo "(Board not plugged in — that is OK, driver was installed successfully)"
else
    echo "Skipping install (--no-install)."
    echo "Module is at: $BUILD_DIR/lib2026sdrSupport.so"
fi
