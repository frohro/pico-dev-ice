#!/usr/bin/env python3
"""
manage_sdr.py - SDR Firmware Management, Flashing, and Telemetry Tool
"""

import sys
import os
import time
import glob
import subprocess
import serial

FIRMWARE_DIR = "/home/frohro/Projects/pico-dev-ice/Software/ddc_sdr_firmware"
DEFAULT_IP = "192.168.1.191"

def find_serial_port():
    ports = glob.glob('/dev/ttyACM*')
    return ports[0] if ports else None

def send_cdc_command(cmd, wait_time=0.2):
    port = find_serial_port()
    if not port:
        return "ERROR: No /dev/ttyACM* found"
    try:
        s = serial.Serial(port, 115200, timeout=1.5)
        time.sleep(0.05)
        s.write((cmd + "\r\n").encode())
        time.sleep(wait_time)
        resp = s.read_all().decode(errors='replace').strip()
        s.close()
        return resp
    except Exception as e:
        return f"ERROR: {e}"

def build_firmware():
    print("[*] Building firmware...")
    res = subprocess.run(["./build_picow.sh"], cwd=FIRMWARE_DIR, capture_output=True, text=True)
    if res.returncode != 0:
        print("[-] Build failed:")
        print(res.stderr)
        return False
    print("[+] Build succeeded!")
    return True

def flash_firmware():
    if not build_firmware():
        return False
    print("[*] Flashing firmware...")
    res = subprocess.run(["python3", "flash_picow.py"], cwd=FIRMWARE_DIR)
    return res.returncode == 0

def wait_for_wifi(timeout=25):
    print("[*] Waiting for board to boot and connect to Wi-Fi...")
    time.sleep(8)
    start = time.time()
    while time.time() - start < timeout:
        resp = send_cdc_command("WIFI")
        if "WIFI,UP" in resp:
            print(f"[+] Connected: {resp}")
            return True
        elif "WIFI" in resp:
            print(f"[*] Status: {resp}")
        time.sleep(1.0)
    print("[-] Timeout waiting for Wi-Fi")
    return False

def show_stats():
    print(send_cdc_command("HPSDR"))

def show_prof():
    print(send_cdc_command("PROF"))

def run_test():
    res = subprocess.run(["python3", "test_openhpsdr.py", "--ip", DEFAULT_IP], cwd=FIRMWARE_DIR)
    return res.returncode == 0

def full_cycle():
    if not flash_firmware():
        return False
    if not wait_for_wifi():
        return False
    show_stats()
    run_test()
    show_stats()
    show_prof()
    return True

if __name__ == "__main__":
    action = sys.argv[1] if len(sys.argv) > 1 else "help"
    if action == "flash":
        flash_firmware()
    elif action == "wifi":
        wait_for_wifi()
    elif action == "stats":
        show_stats()
    elif action == "prof":
        show_prof()
    elif action == "test":
        run_test()
    elif action == "cycle":
        full_cycle()
    elif action == "cmd":
        cmd = sys.argv[2] if len(sys.argv) > 2 else "HPSDR"
        print(send_cdc_command(cmd))
    else:
        print("Usage: python3 manage_sdr.py [flash|wifi|stats|prof|test|cycle|cmd <CMD>]")
