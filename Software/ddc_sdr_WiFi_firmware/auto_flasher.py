#!/usr/bin/env python3
"""
auto_flasher.py - Background daemon that automatically flashes the Pico W
the instant it appears in BOOTSEL mode, then tests the streaming connection.
"""

import os
import sys
import time
import shutil
import subprocess

FIRMWARE_DIR = os.path.dirname(os.path.abspath(__file__))
UF2_PATH = os.path.join(FIRMWARE_DIR, "build-picow", "ddc_sdr.uf2")
LOG_PATH = "/tmp/auto_flash.log"

def log(msg):
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{ts}] {msg}"
    print(line, flush=True)
    with open(LOG_PATH, "a") as f:
        f.write(line + "\n")

def find_rp2_mount():
    # 1. Check existing mounts
    user = os.environ.get("USER", "frohro")
    candidates = [
        f"/run/media/{user}/RPI-RP2",
        f"/media/{user}/RPI-RP2",
        "/run/media/RPI-RP2",
        "/media/RPI-RP2",
        "/mnt/RPI-RP2",
    ]
    for c in candidates:
        if os.path.exists(os.path.join(c, "INFO_UF2.TXT")):
            return c

    # 2. Check all dirs in /run/media and /media
    for base in [f"/run/media/{user}", f"/media/{user}", "/run/media", "/media"]:
        if os.path.isdir(base):
            for entry in os.listdir(base):
                p = os.path.join(base, entry)
                if os.path.exists(os.path.join(p, "INFO_UF2.TXT")):
                    return p

    # 3. Try mounting /dev/disk/by-label/RPI-RP2 if device exists
    dev = "/dev/disk/by-label/RPI-RP2"
    if os.path.exists(dev):
        res = subprocess.run(["udisksctl", "mount", "-b", dev], capture_output=True, text=True)
        if "at " in res.stdout:
            mount_path = res.stdout.split("at ")[-1].strip().rstrip(".")
            if os.path.exists(os.path.join(mount_path, "INFO_UF2.TXT")):
                return mount_path
        time.sleep(0.5)
        for c in candidates:
            if os.path.exists(os.path.join(c, "INFO_UF2.TXT")):
                return c

    return None

def main():
    log(f"Auto-flasher started. Watching for RPI-RP2 to flash {UF2_PATH}...")
    while True:
        mount = find_rp2_mount()
        if mount:
            log(f"Found RPI-RP2 at {mount}!")
            target = os.path.join(mount, os.path.basename(UF2_PATH))
            log(f"Copying {UF2_PATH} -> {target}...")
            try:
                shutil.copyfile(UF2_PATH, target)
                os.sync()
                log("UF2 copied successfully! Board rebooting...")
            except Exception as e:
                log(f"Copy finished (or auto-unmounted): {e}")

            # Wait for board to boot and associate
            log("Waiting 10s for board to initialize and connect to Wi-Fi...")
            time.sleep(10)

            # Run test
            log("Running test_openhpsdr.py...")
            test_script = os.path.join(FIRMWARE_DIR, "test_openhpsdr.py")
            res = subprocess.run([sys.executable, test_script], capture_output=True, text=True)
            log("--- Test Output ---")
            for l in res.stdout.splitlines():
                log("  " + l)
            if res.stderr:
                for l in res.stderr.splitlines():
                    log("  [err] " + l)
            log("--- End Test Output ---")
            log("Auto-flasher continuing to monitor for future BOOTSEL mounts...")

        time.sleep(0.5)

if __name__ == "__main__":
    main()
