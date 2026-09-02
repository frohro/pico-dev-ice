#!/usr/bin/env python3
"""
flash_picow.py — Flash Raspberry Pi Pico / Pico W for Pico-Dev-iCE SDR

Reboots the Pico into BOOTSEL mode via:
1. 1200-baud touch reset on /dev/ttyACM* (USB CDC)
2. 'BOOTSEL' command over USB CDC serial
3. 'BOOTSEL' command over Wi-Fi TCP Control Server (port 5000)

Then detects the mounted RPI-RP2 mass storage drive and copies the UF2 firmware.
"""

import argparse
import glob
import os
import shutil
import socket
import sys
import time

DEFAULT_IP = "192.168.1.191"
DEFAULT_UF2 = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "build-picow", "ddc_sdr.uf2"
)


def find_rp2_mount():
    """Look for mounted RPI-RP2 volumes."""
    user = os.environ.get("USER", "")
    candidate_paths = [
        f"/media/{user}/RPI-RP2",
        f"/run/media/{user}/RPI-RP2",
        "/media/RPI-RP2",
        "/mnt/RPI-RP2",
    ]
    for path in candidate_paths:
        if os.path.exists(os.path.join(path, "INFO_UF2.TXT")):
            return path

    # Check /media/$USER/* and /run/media/$USER/*
    for base in [f"/media/{user}", f"/run/media/{user}"]:
        if os.path.isdir(base):
            for entry in os.listdir(base):
                full = os.path.join(base, entry)
                if os.path.exists(os.path.join(full, "INFO_UF2.TXT")):
                    return full
    return None


def trigger_bootsel_serial(port):
    """Trigger BOOTSEL mode via USB CDC command or 1200 baud touch reset."""
    print(f"[*] Trying serial reset on {port}...")
    try:
        import serial
        s = serial.Serial(port, 115200, timeout=0.5)
        s.write(b"BOOTSEL\r\n")
        time.sleep(0.1)
        s.close()
        time.sleep(0.5)
        return True
    except Exception as e:
        pass
    try:
        import termios
        fd = os.open(port, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
        attrs = termios.tcgetattr(fd)
        attrs[4] = termios.B1200
        attrs[5] = termios.B1200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        os.close(fd)
        time.sleep(0.5)
        return True
    except Exception as e:
        print(f"    Serial reset failed: {e}")
        return False


def trigger_bootsel_wifi(ip, port=5000):
    """Trigger BOOTSEL mode via Wi-Fi TCP Control Server."""
    print(f"[*] Trying Wi-Fi TCP reset on {ip}:{port}...")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect((ip, port))
        # Read greeting
        try:
            s.recv(256)
        except Exception:
            pass
        s.sendall(b"BOOTSEL\r\n")
        time.sleep(0.2)
        s.close()
        print("    [+] Sent BOOTSEL command over Wi-Fi TCP!")
        return True
    except Exception as e:
        print(f"    [-] Wi-Fi TCP reset failed: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(description="Flash Pico-Dev-iCE SDR Firmware")
    parser.add_argument("--uf2", default=DEFAULT_UF2, help="Path to UF2 firmware file")
    parser.add_argument("--ip", default=DEFAULT_IP, help="Pico W IP address for Wi-Fi reset")
    parser.add_argument("--serial", default=None, help="Serial port (e.g. /dev/ttyACM0)")
    parser.add_argument("--skip-reset", action="store_true", help="Skip reset and just look for drive")
    args = parser.parse_args()

    if not os.path.exists(args.uf2):
        print(f"[-] Error: UF2 file not found: {args.uf2}")
        print("    Run 'bash build_picow.sh' first.")
        sys.exit(1)

    print(f"[*] Target UF2: {args.uf2} ({os.path.getsize(args.uf2):,} bytes)")

    # 1. Check if already in BOOTSEL mode
    mount = find_rp2_mount()
    if mount:
        print(f"[+] Found RPI-RP2 drive at: {mount}")
    elif not args.skip_reset:
        # 2. Try Serial reset
        serial_ports = [args.serial] if args.serial else glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")
        reset_sent = False
        for p in serial_ports:
            if p and os.path.exists(p):
                if trigger_bootsel_serial(p):
                    reset_sent = True
                    break

        # 3. If serial didn't trigger, try Wi-Fi TCP
        if not reset_sent and args.ip:
            reset_sent = trigger_bootsel_wifi(args.ip)

        # 4. Wait for RPI-RP2 mount to appear
        print("[*] Waiting for RPI-RP2 volume to mount...")
        for i in range(20):
            mount = find_rp2_mount()
            if mount:
                break
            time.sleep(0.5)

    if not mount:
        print("\n[-] Error: RPI-RP2 drive not found.")
        print("    Please ensure the board is connected via USB and:")
        print("    1. Hold the BOOTSEL button while plugging into USB, OR")
        print("    2. Check that the USB cable is capable of data transfer.")
        sys.exit(1)

    # 5. Flash the UF2 file
    dest = os.path.join(mount, os.path.basename(args.uf2))
    print(f"[*] Copying {args.uf2} -> {dest}...")
    try:
        shutil.copyfile(args.uf2, dest)
        os.sync()
        print("[+] Firmware copied successfully! The board will now reboot.")
    except Exception as e:
        # On Linux, the drive unmounts immediately when the copy completes, which can trigger an I/O error
        if not os.path.exists(mount):
            print("[+] Flashing completed (drive rebooted automatically).")
        else:
            print(f"[-] Flash error: {e}")
            sys.exit(1)


if __name__ == "__main__":
    main()
