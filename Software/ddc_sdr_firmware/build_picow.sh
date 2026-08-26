#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-picow"

if [ ! -f "${SCRIPT_DIR}/../pico-ice-sdk/CMakeLists.txt" ]; then
    echo "[*] Initializing pico-ice-sdk..."
    git -C "${SCRIPT_DIR}/../.." submodule sync Software/pico-ice-sdk || true
    git -C "${SCRIPT_DIR}/../.." submodule update --init --recursive Software/pico-ice-sdk || true
    if [ ! -f "${SCRIPT_DIR}/../pico-ice-sdk/CMakeLists.txt" ]; then
        echo "[*] Cloning pico-ice-sdk directly from GitHub..."
        rm -rf "${SCRIPT_DIR}/../pico-ice-sdk"
        git clone https://github.com/frohro/pico-ice-sdk.git "${SCRIPT_DIR}/../pico-ice-sdk"
    fi
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake .. \
    -DPICO_BOARD=pico_w \
    -DPICO_DEV_ICE=1 \
    -DCMAKE_BUILD_TYPE=Release \
    -DFPGA_BOOT_MODE=STORED \
    -DFPGA_RX_BITSTREAM_BIN="${SCRIPT_DIR}/../../ENGR433-Solutions/Lab_09/ddc_sdr_top.bin"

make -j$(nproc)

echo ""
echo "=== Build Complete! ==="
echo "Firmware UF2: ${BUILD_DIR}/ddc_sdr.uf2"
ls -lh "${BUILD_DIR}/ddc_sdr.uf2"
