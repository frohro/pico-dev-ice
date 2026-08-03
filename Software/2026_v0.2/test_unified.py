#!/usr/bin/env python3
"""
test_unified.py -- Test script for Unified 2026 v0.2 SDR Firmware.
Verifies all sample rates (48/96/192 kHz) and ADC selection (PCM1808/CJC5430).
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
    if not ok: failures += 1
    return ok

def find_serial_port():
    # Fallback for environments where pyserial list_ports is restricted
    if os.path.exists("/dev/ttyACM0"):
        return "/dev/ttyACM0"
    
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if p.vid == 0xcafe and p.pid == 0x4080:
            return p.device
    return None

def test_adc_rate(ser, adc_name, rate_hz):
    print(f"\n--- Testing Mode: {adc_name} @ {rate_hz} Hz ---")
    
    # 1. Select ADC
    ser.write(f"ADC,{adc_name}\r\n".encode())
    res = ser.read_until(b"OK\r\n").decode()
    check(f"Select {adc_name}", "OK" in res, True)
    
    # 2. Select Rate
    ser.write(f"RATE,{rate_hz}\r\n".encode())
    res = ser.read_until(b"OK\r\n").decode()
    check(f"Select Rate {rate_hz}", "OK" in res, True)

    # 3. Check Audio via arecord -l
    # Note: It might take a moment for the USB interface to re-negotiate
    time.sleep(1.0)
    out = subprocess.check_output(["arecord", "-l"]).decode()
    check("Audio Device Found", "WWU SDR" in out, True)

    # 4. Verify Sample Rate via arecord --duration=1 (simplified test)
    fmt = "S24_3LE" if rate_hz < 192000 else "S16_LE"
    cmd = ["arecord", "-D", "hw:CARD=SDR,DEV=0", "-r", str(rate_hz), "-f", fmt, "--duration=1", "/dev/null"]
    print(f"  Running: {' '.join(cmd)}")
    try:
        subprocess.check_call(cmd, stderr=subprocess.STDOUT)
        print(f"  {PASS} Audio Captured successfully")
    except subprocess.CalledProcessError as e:
        print(f"  {FAIL} Audio Capture failed")
        global failures
        failures += 1

def main():
    print("=" * 60)
    print("Testing Unified SDR Firmware v0.2")
    print("=" * 60)

    port = find_serial_port()
    if not port:
        print(f"{FAIL} Could not find SDR CDC device (0xcafe:0x4080)")
        sys.exit(1)
    
    print(f"Found SDR on {port}")
    ser = serial.Serial(port, 115200, timeout=1.0)
    
    # Basic identification
    ser.write(b"VER\r\n")
    ver = ser.read_until(b"OK\r\n").decode()
    check("Firmware Version", "SDR Multi-ADC 2.0" in ver, True)

    # Test Matrix
    modes = [
        ("PCM1808", 48000),
        ("PCM1808", 96000),
        ("CJC5430", 48000),
        ("CJC5430", 96000),
        ("CJC5430", 192000),
    ]

    for adc, rate in modes:
        test_adc_rate(ser, adc, rate)

    ser.close()
    
    print("\n" + "=" * 60)
    if failures == 0:
        print(f"ALL TESTS {PASS}")
    else:
        print(f"TESTS {FAIL} with {failures} failures")
    print("=" * 60)

if __name__ == "__main__":
    main()
