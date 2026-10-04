#!/usr/bin/env python3

import argparse
import pathlib
import subprocess
import sys
import time

import serial
import serial.tools.list_ports

VID = 0x1209
PID = 0xB1C0


def find_control_port():
    for port in serial.tools.list_ports.comports():
        if port.vid == VID and port.pid == PID:
            return port.device
    return None


def prepare_device(timeout):
    device = find_control_port()
    if device is None:
        raise RuntimeError("DDC SDR control CDC device was not found")

    with serial.Serial(device, 115200, timeout=timeout) as control:
        control.write(b"DFU,PREPARE\r\n")
        response = control.read_until(b"OK\r\n").decode(errors="replace")
    if "DFU,READY" not in response or "OK" not in response:
        raise RuntimeError("Pico did not acknowledge DFU preparation: " + response.strip())


def main():
    parser = argparse.ArgumentParser(description="Load a packed iCE40 bitstream into DDC SDR CRAM")
    parser.add_argument("bitstream", type=pathlib.Path)
    parser.add_argument("--dfu-util", default="dfu-util")
    parser.add_argument("--skip-prepare", action="store_true")
    args = parser.parse_args()

    if not args.bitstream.is_file():
        parser.error(f"bitstream does not exist: {args.bitstream}")
    if not args.skip_prepare:
        prepare_device(timeout=3.0)
        time.sleep(0.05)

    command = [
        args.dfu_util,
        "-d", f"{VID:04x}:{PID:04x}",
        "-a", "0",
        "-D", str(args.bitstream),
    ]
    return subprocess.run(command).returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"dfu_fpga.py: {error}", file=sys.stderr)
        sys.exit(1)
