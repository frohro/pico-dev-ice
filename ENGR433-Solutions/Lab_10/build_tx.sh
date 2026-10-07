#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${1:-${SCRIPT_DIR}/build-tx}"
mkdir -p "${BUILD_DIR}"

RTL=(
    "${SCRIPT_DIR}/lab10_tx_top.sv"
    "${SCRIPT_DIR}/i2s_clock_master.sv"
    "${SCRIPT_DIR}/i2s_receiver.sv"
    "${SCRIPT_DIR}/i2s_serializer.sv"
    "${SCRIPT_DIR}/duc_modulator.sv"
    "${ROOT_DIR}/ENGR433-Solutions/Lab_04/spi_cmd_parser.sv"
    "${ROOT_DIR}/ENGR433-Solutions/Lab_05/adc_capture_otr.sv"
    "${ROOT_DIR}/ENGR433-Solutions/Lab_06/nco.sv"
    "${ROOT_DIR}/ENGR433-Solutions/Lab_07/mixer.sv"
    "${ROOT_DIR}/ENGR433-Solutions/Lab_08/cic_decimator.sv"
)

JSON="${BUILD_DIR}/lab10_tx_top.json"
ASC="${BUILD_DIR}/lab10_tx_top.asc"
BIN="${BUILD_DIR}/lab10_tx_top.bin"

printf 'Synthesizing Lab 10 TX top...\n'
yosys -p "read_verilog -sv ${RTL[*]}; synth_ice40 -top lab10_tx_top -json ${JSON}"

printf 'Placing and routing Lab 10 TX top...\n'
nextpnr-ice40 \
    --up5k \
    --package sg48 \
    --freq 30.72 \
    --top lab10_tx_top \
    --pcf "${SCRIPT_DIR}/lab10.pcf" \
    --json "${JSON}" \
    --asc "${ASC}"

icepack "${ASC}" "${BIN}"

printf '\nBuild complete:\n  ASC: %s\n  BIN: %s\n' "${ASC}" "${BIN}"
