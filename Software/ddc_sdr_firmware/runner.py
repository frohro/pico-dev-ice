#!/usr/bin/env python3
"""
runner.py - Pico W SDR diagnostic helper.
Polls serial until WIFI,UP then runs the OpenHPSDR stream test.
Usage: python3 runner.py [pico_ip]
"""
import sys
import serial
import time
import re

# Override IP from command line if provided
OVERRIDE_IP = sys.argv[1] if len(sys.argv) > 1 else None
SERIAL_PORT = '/dev/ttyACM0'
BAUD = 115200
MAX_WAIT_SEC = 30

def wait_for_wifi_up():
    """Poll serial for WIFI,UP and return the IP address, or None on timeout."""
    print(f"[runner] Opening {SERIAL_PORT} @ {BAUD}...")
    try:
        s = serial.Serial(SERIAL_PORT, BAUD, timeout=0.5)
    except Exception as e:
        print(f"[runner] Cannot open serial port: {e}")
        return None

    s.dtr = True
    deadline = time.time() + MAX_WAIT_SEC
    ip = None

    while time.time() < deadline:
        s.write(b'WIFI\r\n')
        time.sleep(0.1)
        raw = s.read_all().decode(errors='ignore')
        lines = raw.replace('\r', '').split('\n')
        for line in lines:
            line = line.strip()
            if not line:
                continue
            print(f"  Serial: {line}")
            m = re.search(r'WIFI,UP,IP,(\d+\.\d+\.\d+\.\d+)', line)
            if m:
                ip = m.group(1)
                s.close()
                return ip
        elapsed = MAX_WAIT_SEC - (deadline - time.time())
        print(f"[runner] Waiting for WIFI,UP... ({elapsed:.0f}s elapsed)")
        time.sleep(1.0)

    s.close()
    print(f"[runner] Timed out after {MAX_WAIT_SEC}s waiting for WIFI,UP")
    return None


def run_stream_test(ip, duration=10.0):
    """Run the OpenHPSDR stream test against the given IP."""
    import sys, os
    sys.path.insert(0, os.path.dirname(__file__))
    from test_openhpsdr_stream import test_openhpsdr
    test_openhpsdr(ip, duration=duration)


if __name__ == '__main__':
    if OVERRIDE_IP:
        print(f"[runner] Using override IP: {OVERRIDE_IP}")
        ip = OVERRIDE_IP
    else:
        ip = wait_for_wifi_up()
        if not ip:
            print("[runner] Failed to get IP. Exiting.")
            sys.exit(1)
        print(f"\n[runner] Got WIFI,UP with IP={ip}")

    run_stream_test(ip, duration=10.0)
