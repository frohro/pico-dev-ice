#!/usr/bin/env python3
"""
runner.py - Pico W SDR build, flash, discovery, and diagnostic tool.
Usage:
  python3 runner.py          # Auto-discover, flash if USB attached, and test stream
  python3 runner.py flash    # Rebuild and flash to Pico W
  python3 runner.py [ip]     # Run stream test on specific IP
"""
import sys
import os
import time
import subprocess
import shutil
import socket
import re

CWD = os.path.dirname(os.path.abspath(__file__))
UF2_PATH = os.path.join(CWD, "build-picow", "ddc_sdr.uf2")
SERIAL_PORT = '/dev/ttyACM0'
BAUD = 115200


def build_firmware():
    """Build firmware using build_picow.sh."""
    print("[*] Building pico_w firmware...")
    res = subprocess.run(["bash", "build_picow.sh"], cwd=CWD)
    if res.returncode != 0:
        print("[!] Build failed!")
        return False
    print(f"[✓] Built: {UF2_PATH}")
    return True


def flash_firmware():
    """Reset Pico W to BOOTSEL and copy UF2."""
    if not os.path.exists(UF2_PATH):
        if not build_firmware():
            return False

    print("[*] Checking for Pico W USB connection...")
    if os.path.exists(SERIAL_PORT):
        try:
            import serial
            print(f"[*] Resetting {SERIAL_PORT} to BOOTSEL...")
            s = serial.Serial(SERIAL_PORT, BAUD, timeout=1)
            s.write(b'BOOTSEL\r\n')
            time.sleep(0.5)
            s.close()
        except Exception as e:
            print(f"[*] Serial reset note: {e}")

    print("[*] Waiting for RPI-RP2 drive to appear...")
    mount_pt = None
    for _ in range(15):
        time.sleep(1)
        for p in ['/media/frohro/RPI-RP2', '/run/media/frohro/RPI-RP2']:
            if os.path.exists(p):
                mount_pt = p
                break
        if mount_pt:
            break
        out = subprocess.getoutput('lsblk -o NAME,MOUNTPOINT,LABEL')
        if 'RPI-RP2' in out:
            for line in out.splitlines():
                if 'RPI-RP2' in line:
                    parts = line.split()
                    if len(parts) >= 2 and parts[1].startswith('/'):
                        mount_pt = parts[1]
                        break

    if mount_pt:
        print(f"[*] Found RPI-RP2 mount point: {mount_pt}")
        print(f"[*] Copying {UF2_PATH} -> {mount_pt}...")
        shutil.copy(UF2_PATH, mount_pt)
        print("[✓] Firmware successfully flashed to Pico W!")
        time.sleep(2)
        return True
    else:
        print("[!] Could not find RPI-RP2 drive (Pico may not be connected to computer data port).")
        return False


def probe_lan_for_pico():
    """Send OpenHPSDR discovery probe to find Pico W IP on LAN."""
    print("[*] Probing LAN for OpenHPSDR device on port 1024...")
    disc = bytearray(64)
    disc[0], disc[1], disc[2] = 0xEF, 0xFE, 0x02

    # 1. Try Broadcast
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(1.0)
        sock.sendto(disc, ('255.255.255.255', 1024))
        resp, addr = sock.recvfrom(1024)
        sock.close()
        if len(resp) >= 60 and resp[0] == 0xEF and resp[1] == 0xFE:
            mac = ':'.join(f'{b:02X}' for b in resp[3:9])
            print(f"[✓] Discovered Pico W at {addr[0]} (MAC: {mac}) via broadcast")
            return addr[0]
    except Exception:
        pass

    # 2. Try Known IPs (Static Fallback 192.168.1.191, DHCP 192.168.1.192, etc.)
    candidate_ips = ['192.168.1.191', '192.168.1.192', '192.168.1.193', '192.168.1.203']
    for ip in candidate_ips:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.settimeout(0.4)
            sock.sendto(disc, (ip, 1024))
            resp, addr = sock.recvfrom(1024)
            sock.close()
            if len(resp) >= 60:
                mac = ':'.join(f'{b:02X}' for b in resp[3:9])
                print(f"[✓] Discovered Pico W at {ip} (MAC: {mac}) via direct probe")
                return ip
        except Exception:
            pass

    return None


def wait_for_wifi_serial(max_wait=20):
    """Poll serial port if attached."""
def wait_for_wifi_serial(max_wait=30):
    """Wait for serial port to appear and poll for WIFI,UP."""
    print(f"[*] Waiting for {SERIAL_PORT} and polling for WIFI,UP (timeout {max_wait}s)...")
    deadline = time.time() + max_wait
    s = None

    while time.time() < deadline:
        if not s:
            if os.path.exists(SERIAL_PORT):
                try:
                    import serial
                    s = serial.Serial(SERIAL_PORT, BAUD, timeout=0.5)
                    s.dtr = True
                except Exception:
                    s = None
            if not s:
                time.sleep(0.5)
                continue

        try:
            s.write(b'WIFI\r\n')
            time.sleep(0.15)
            raw = s.read_all().decode(errors='ignore')
            for line in raw.replace('\r', '').split('\n'):
                line = line.strip()
                if line:
                    print(f"  Serial: {line}")
                    m = re.search(r'WIFI,UP,IP,(\d+\.\d+\.\d+\.\d+)', line)
                    if m:
                        s.close()
                        return m.group(1)
        except Exception:
            try:
                s.close()
            except Exception:
                pass
            s = None

        time.sleep(1.0)

    if s:
        try:
            s.close()
        except Exception:
            pass
    return None


def run_stream_test(ip, duration=10.0):
    """Run OpenHPSDR stream test against target IP."""
    sys.path.insert(0, CWD)
    from test_openhpsdr_stream import test_openhpsdr
    test_openhpsdr(ip, duration=duration)


if __name__ == '__main__':
    args = sys.argv[1:]

    # Check if flash was explicitly requested
    if 'flash' in args:
        build_firmware()
        flash_firmware()
        sys.exit(0)

    # Check if an explicit IP was passed
    target_ip = None
    for a in args:
        if re.match(r'^\d+\.\d+\.\d+\.\d+$', a):
            target_ip = a
            break

    if not target_ip:
        # Check if USB is plugged in, flash the latest code if available
        if os.path.exists(SERIAL_PORT):
            print("[*] Detected Pico W on USB serial! Building and flashing latest firmware...")
            if build_firmware():
                flash_firmware()
                target_ip = wait_for_wifi_serial(max_wait=30)

        # Probe LAN
        if not target_ip:
            target_ip = probe_lan_for_pico()

        # Serial wait fallback
        if not target_ip:
            target_ip = wait_for_wifi_serial(max_wait=15)

    if not target_ip:
        print("\n[!] Could not locate Pico W on network or serial.")
        print("[!] Please plug the Pico W into the computer with a USB data cable.")
        sys.exit(1)

    print(f"\n[✓] Target Pico W IP: {target_ip}")
    run_stream_test(target_ip, duration=10.0)
