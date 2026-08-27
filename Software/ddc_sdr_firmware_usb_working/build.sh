#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

export PICO_SDK_PATH="${PICO_SDK_PATH:-/home/frohro/Projects/pico-sdk}"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

BITSTREAM="${SCRIPT_DIR}/bitstreams/ddc_sdr_rx.bin"

TINYUSB_ARG=""
if [ -d "${HOME}/tinyusb/src" ]; then
    TINYUSB_ARG="-DPICO_TINYUSB_PATH=${HOME}/tinyusb"
fi

PICOTOOL_ARG=""
if [ -f "${SCRIPT_DIR}/../ddc_sdr_firmware/build-picow/_deps/picotool/picotool" ]; then
    PICOTOOL_ARG="-DPICOTOOL_EXECUTABLE=${SCRIPT_DIR}/../ddc_sdr_firmware/build-picow/_deps/picotool/picotool"
fi

cmake .. \
    -DPICO_BOARD=pico_dev_ice \
    -DPICO_DEV_ICE=1 \
    -DCMAKE_BUILD_TYPE=Release \
    -DFPGA_BOOT_MODE=STORED \
    -DFPGA_RX_BITSTREAM_BIN="${BITSTREAM}" \
    ${TINYUSB_ARG} \
    ${PICOTOOL_ARG}

make -j$(nproc)

echo ""
echo "=== Build Complete! ==="
echo "Firmware UF2: ${BUILD_DIR}/ddc_sdr.uf2"
ls -lh "${BUILD_DIR}/ddc_sdr.uf2"
