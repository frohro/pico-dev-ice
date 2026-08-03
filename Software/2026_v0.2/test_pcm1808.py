#!/usr/bin/env python3
"""
test_pcm1808.py -- Test script for PCM1808 48/96 kHz SDR Firmware (v0.2).

Tests both sample rates using the CDC control interface and verifies
audio capture via arecord.
"""

import os
import re
import subprocess
import sys
import time
import serial
import serial.tools.list_ports

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

failures = 0

def check(label, got, expected):
    global failures
    ok = (got == expected)
    status = PASS if ok else FAIL
    print(f"  {status}  {label}: got {got!r}, expected {expected!r}")
    if not ok:
        failures += 1
    return ok

def find_serial_port():
    """Find the SDR CDC device by VID:PID, falling back to /dev/ttyACM0."""
    if os.path.exists("/dev/ttyACM0"):
        return "/dev/ttyACM0"
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if p.vid == 0xcafe and p.pid == 0x4080:
            return p.device
    return None

def find_alsa_card():
    """Return the ALSA short card name for our SDR device, or None."""
    try:
        out = subprocess.check_output(["arecord", "-l"], stderr=subprocess.STDOUT).decode()
    except subprocess.CalledProcessError:
        return None
    # Each card line looks like:  card N: SHORTNAME [LONGNAME], device D: ...
    for line in out.splitlines():
        m = re.match(r'card\s+\d+:\s+(\S+)\s+\[([^\]]+)\]', line)
        if m and 'SDR PCM1808' in m.group(2):
            return m.group(1)   # e.g. "S2026"
    return None

def test_rate(ser, rate_hz):
    """Send RATE command and verify audio capture at the given sample rate."""
    print(f"\n--- Testing PCM1808 @ {rate_hz} Hz ---")

    # 1. Set sample rate via CDC
    ser.write(f"RATE,{rate_hz}\r\n".encode())
    res = ser.read_until(b"OK\r\n").decode()
    check(f"RATE,{rate_hz} acknowledged", "OK" in res, True)

    # Allow time for the PCM1808 mode pin and PIO/DMA to settle
    time.sleep(0.5)

    # 2. Find the ALSA card name and verify it is present
    card_name = find_alsa_card()
    check("Audio device present", card_name is not None, True)
    if card_name is None:
        return

    # 3. Capture 1 second of audio and verify no errors
    cmd = [
        "arecord",
        "-D", f"plughw:CARD={card_name},DEV=0",
        "-r", str(rate_hz),
        "-f", "S24_3LE",
        "-c", "2",
        "--duration=1",
        "/dev/null",
    ]
    print(f"  Running: {' '.join(cmd)}")
    try:
        subprocess.check_call(cmd, stderr=subprocess.STDOUT)
        print(f"  {PASS}  Audio captured successfully at {rate_hz} Hz")
    except subprocess.CalledProcessError:
        print(f"  {FAIL}  Audio capture failed at {rate_hz} Hz")
        global failures
        failures += 1

def main():
    print("=" * 60)
    print("Testing SDR Firmware v0.2 — PCM1808 48/96 kHz")
    print("=" * 60)

    port = find_serial_port()
    if not port:
        print(f"{FAIL} Could not find SDR CDC device (0xcafe:0x4080)")
        sys.exit(1)

    print(f"Found SDR on {port}")
    ser = serial.Serial(port, 115200, timeout=2.0)
    time.sleep(0.2)   # allow DTR to settle

    # --- Basic identification -------------------------------------------
    ser.write(b"VER\r\n")
    ver = ser.read_until(b"OK\r\n").decode()
    check("Firmware version", "SDR PCM1808 2.1" in ver, True)

    ser.write(b"XTAL\r\n")
    xtal = ser.read_until(b"OK\r\n").decode()
    check("XTAL = 24576000 Hz", "24576000" in xtal, True)

    # --- Reject invalid rate -------------------------------------------
    ser.write(b"RATE,192000\r\n")
    res = ser.read_until(b"\r\n").decode()
    check("RATE,192000 rejected", "ERROR" in res, True)
    # Drain any remaining response
    ser.read(ser.in_waiting or 1)

    # --- Rate tests ----------------------------------------------------
    for rate in [48000, 96000]:
        test_rate(ser, rate)

    # --- Return to 48 kHz (tidy state) ---------------------------------
    ser.write(b"RATE,48000\r\n")
    ser.read_until(b"OK\r\n")

    ser.close()

    print("\n" + "=" * 60)
    if failures == 0:
        print(f"ALL TESTS {PASS}")
    else:
        print(f"TESTS {FAIL} — {failures} failure(s)")
    print("=" * 60)
    sys.exit(0 if failures == 0 else 1)

if __name__ == "__main__":
    main()
