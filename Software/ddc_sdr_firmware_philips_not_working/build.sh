#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

export PICO_SDK_PATH="${PICO_SDK_PATH:-/home/frohro/Projects/pico-sdk}"
export PICO_ICE_SDK_PATH="${PICO_ICE_SDK_PATH:-${SCRIPT_DIR}/../pico-ice-sdk}"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Default: DFU-only firmware (no FPGA image embedded). Students load their own
# .bin with tools/dfu_fpga.py. To embed an image instead, set FPGA_BOOT_MODE=STORED
# and FPGA_RX_BITSTREAM_BIN (and optionally FPGA_TX_BITSTREAM_BIN).
BOOT_MODE="${FPGA_BOOT_MODE:-DFU}"
RX_BITSTREAM="${FPGA_RX_BITSTREAM_BIN:-}"
TX_BITSTREAM="${FPGA_TX_BITSTREAM_BIN:-}"
DEFAULT_IMAGE="${FPGA_DEFAULT_IMAGE:-RX}"

TINYUSB_ARG=""
if [ -d "${HOME}/tinyusb/src" ]; then
    TINYUSB_ARG="-DPICO_TINYUSB_PATH=${HOME}/tinyusb"
fi

PICO_ICE_SDK_ARG=""
if [ -n "${PICO_ICE_SDK_PATH:-}" ]; then
    PICO_ICE_SDK_ARG="-DPICO_ICE_SDK_PATH=${PICO_ICE_SDK_PATH}"
fi

PICOTOOL_ARG=""
if [ -f "${SCRIPT_DIR}/../ddc_sdr_firmware/build-picow/_deps/picotool/picotool" ]; then
    PICOTOOL_ARG="-DPICOTOOL_EXECUTABLE=${SCRIPT_DIR}/../ddc_sdr_firmware/build-picow/_deps/picotool/picotool"
fi

cmake .. \
    -DPICO_BOARD=pico_dev_ice \
    -DPICO_DEV_ICE=1 \
    -DCMAKE_BUILD_TYPE=Release \
    -DFPGA_BOOT_MODE="${BOOT_MODE}" \
    -DFPGA_DEFAULT_IMAGE="${DEFAULT_IMAGE}" \
    -DFPGA_RX_BITSTREAM_BIN="${RX_BITSTREAM}" \
    -DFPGA_TX_BITSTREAM_BIN="${TX_BITSTREAM}" \
    ${TINYUSB_ARG} \
    ${PICO_ICE_SDK_ARG} \
    ${PICOTOOL_ARG}

make -j$(nproc)

echo ""
echo "=== Build Complete! ==="
echo "Firmware UF2: ${BUILD_DIR}/ddc_sdr.uf2"
ls -lh "${BUILD_DIR}/ddc_sdr.uf2"
