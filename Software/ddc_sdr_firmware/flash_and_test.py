#!/usr/bin/env python3
"""
Pico-Dev-iCE Unified Flash, Test, and Diagnostics Utility
---------------------------------------------------------
Automates building, 1200-baud touch reset, flashing, Wi-Fi configuration,
USB ALSA audio verification, and OpenHPSDR Protocol 1 UDP streaming verification.
"""

import os
import sys
import time
import socket
import struct
import subprocess
import argparse
import glob

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "../.."))
BUILD_DIR = os.path.join(SCRIPT_DIR, "build-picow")
UF2_PATH = os.path.join(BUILD_DIR, "ddc_sdr.uf2")
DEFAULT_CDC_PORT = "/dev/ttyACM0"


def find_serial_port():
    if serial:
        for p in serial.tools.list_ports.comports():
            if (p.vid, p.pid) == (0x1209, 0xB1C0) or (p.vid, p.pid) == (0x2E8A, 0x000A):
                return p.device
    for dev in ["/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2", "/dev/ttyACM3"]:
        if os.path.exists(dev):
            return dev
    return DEFAULT_CDC_PORT


def run_build():
    print(f"[*] Building firmware in {BUILD_DIR}...")
    if not os.path.exists(BUILD_DIR):
        os.makedirs(BUILD_DIR, exist_ok=True)
        res = subprocess.run(["cmake", "-B", BUILD_DIR, "-S", SCRIPT_DIR, "-DPICO_BOARD=pico_w"], cwd=SCRIPT_DIR)
        if res.returncode != 0:
            print("[!] CMake configure failed.")
            sys.exit(1)

    res = subprocess.run(["cmake", "--build", BUILD_DIR, "-j"], cwd=SCRIPT_DIR)
    if res.returncode != 0:
        print("[!] Build failed.")
        sys.exit(1)
    print("[+] Build successful.")


def reboot_to_bootsel(port):
    port = find_serial_port()
    if not os.path.exists(port):
        print(f"[*] Port {port} not found; checking if already in BOOTSEL mode...")
        return

    print(f"[*] Sending BOOTSEL command to {port}...")
    if serial:
        try:
            s = serial.Serial(port, 115200, timeout=0.5)
            s.write(b"\r\nBOOTSEL\r\n")
            s.flush()
            s.close()
            time.sleep(1.5)
        except Exception as e:
            print(f"[-] Serial touch warning: {e}")


def find_bootsel_mount(timeout=8.0):
    print("[*] Waiting for RPI-RP2 bootloader partition...")
    user = os.environ.get("USER", "frohro")
    candidates = [
        f"/media/{user}/RPI-RP2",
        f"/media/{user}/RPI-RP21",
        "/run/media/RPI-RP2",
        "/mnt/RPI-RP2"
    ]
    start = time.time()
    while time.time() - start < timeout:
        for c in candidates:
            if os.path.ismount(c) or (os.path.exists(c) and os.path.isdir(c)):
                return c
        if os.path.exists("/dev/disk/by-label/RPI-RP2"):
            try:
                subprocess.run(["udisksctl", "mount", "-b", "/dev/disk/by-label/RPI-RP2"], capture_output=True)
            except Exception:
                pass
        time.sleep(0.5)

    # Try picotool if available
    res = subprocess.run(["which", "picotool"], capture_output=True, text=True)
    if res.returncode == 0:
        print("[*] picotool found. Attempting picotool load...")
        p_res = subprocess.run(["picotool", "load", "-fx", UF2_PATH], capture_output=True, text=True)
        if p_res.returncode == 0:
            print("[+] picotool load and reboot succeeded!")
            return "PICOTOOL"

    return None


def flash_uf2(mount_point, uf2_file):
    if mount_point == "PICOTOOL":
        return
    import shutil
    target = os.path.join(mount_point, os.path.basename(uf2_file))
    print(f"[*] Copying {uf2_file} -> {target} ...")
    try:
        shutil.copyfile(uf2_file, target)
    except Exception as e:
        # Ignore disconnection error as Pico immediately reboots upon completion
        pass
    print("[+] Flash complete. Pico W is rebooting...")


def send_cdc_command(cmd, port=None, wait_time=0.3):
    port = port or find_serial_port()
    if not serial or not os.path.exists(port):
        return None
    try:
        ser = serial.Serial(port, 115200, timeout=1.0)
        ser.reset_input_buffer()
        ser.write((cmd + "\r\n").encode())
        time.sleep(wait_time)
        resp = ser.read_all().decode("latin1", errors="replace").strip()
        ser.close()
        return resp
    except Exception as e:
        return f"ERROR: {e}"


def query_diagnostics(port=None, timeout=10.0, wait_for_wifi=True):
    port = port or find_serial_port()
    print(f"[*] Waiting for Pico W CDC port ({port}) to re-enumerate...")
    start = time.time()
    while time.time() - start < timeout:
        if os.path.exists(port):
            break
        time.sleep(0.5)

    if not os.path.exists(port):
        print(f"[-] Serial port {port} did not appear.")
        return None

    time.sleep(0.5)
    print(f"[*] Querying status over {port}...")

    discovered_ip = None
    for cmd in ["VER", "MODE", "FPGA,STATUS", "FREQ", "RATE"]:
        resp = send_cdc_command(cmd, port)
        print(f"    [{cmd}] -> {resp}")

    # Poll WIFI? until connected or timeout
    wifi_start = time.time()
    while True:
        resp = send_cdc_command("WIFI?", port, wait_time=0.2)
        print(f"    [WIFI?] -> {resp}")
        if resp and "IP:" in resp:
            for part in resp.split(","):
                if part.startswith("IP:"):
                    discovered_ip = part.split("IP:")[1].strip()
            if "CONNECTED" in resp or not wait_for_wifi:
                break
        if time.time() - wifi_start >= 10.0:
            break
        time.sleep(1.0)

    debug_resp = send_cdc_command("DEBUG", port)
    print(f"    [DEBUG] -> {debug_resp}")

    return discovered_ip


def set_wifi(ssid, password="", port=None):
    port = port or find_serial_port()
    cmd = f"WIFI,{ssid},{password}"
    print(f"[*] Sending '{cmd}' to {port}...")
    resp = send_cdc_command(cmd, port, wait_time=0.5)
    print(f"    Response: {resp}")
    print("[*] Waiting for connection...")
    for _ in range(10):
        time.sleep(1.0)
        status = send_cdc_command("WIFI?", port, wait_time=0.2)
        print(f"    [WIFI?] -> {status}")
        if "CONNECTED" in str(status):
            print("[+] Successfully connected to Wi-Fi!")
            return True
    return False


def test_openhpsdr_stream(target_ip, target_port=1024, duration=4.0):
    print(f"\n[*] Testing OpenHPSDR Protocol 1 streaming to {target_ip}:{target_port} for {duration}s...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
    sock.settimeout(1.5)
    sock.bind(("", 0))

    # Discovery Probe (0xEFFE 0x02)
    disc_cmd = bytes([0xEF, 0xFE, 0x02])
    sock.sendto(disc_cmd, (target_ip, target_port))
    try:
        resp, addr = sock.recvfrom(1024)
        print(f"[+] Discovery response received ({len(resp)} bytes):")
        mac = ":".join(f"{b:02X}" for b in resp[3:9])
        print(f"    - MAC: {mac}, Firmware: 0x{resp[9]:02X}, Board ID: 0x{resp[10]:02X}")
    except Exception as e:
        print(f"[!] Discovery response warning: {e}")

    # Send START_STREAM (0xEFFE 0x04 0x01)
    start_cmd = bytes([0xEF, 0xFE, 0x04, 0x01])
    sock.sendto(start_cmd, (target_ip, target_port))

    rx_packets = 0
    total_bytes = 0
    non_zero = 0
    packet_times = []
    cc_frame = bytearray(512)
    cc_frame[0:4] = bytes([0xEF, 0xFE, 0x01, 0x02])
    cc_frame[8:11] = bytes([0x7F, 0x7F, 0x7F])
    cc_frame[11] = 0x04 # Cmd 0x02 (RX0 frequency)
    cc_frame[12:16] = (7050000).to_bytes(4, 'big')

    last_cc = 0.0
    start_time = time.time()
    while time.time() - start_time < duration:
        now = time.time()
        if now - last_cc >= 0.02: # 50 Hz keepalive
            sock.sendto(cc_frame, (target_ip, target_port))
            last_cc = now
        try:
            data, addr = sock.recvfrom(2048)
            t = time.perf_counter()
            packet_times.append(t)
            rx_packets += 1
            total_bytes += len(data)
            if len(data) >= 16:
                if any(b != 0 for b in data[16:1032]):
                    non_zero += 1
        except socket.timeout:
            pass

    # Send STOP_STREAM (0xEFFE 0x04 0x00)
    stop_cmd = bytes([0xEF, 0xFE, 0x04, 0x00])
    sock.sendto(stop_cmd, (target_ip, target_port))
    sock.close()

    print(f"[+] Streaming Results: {rx_packets} packets received ({total_bytes} bytes), {non_zero} with non-zero IQ.")
    if len(packet_times) > 1:
        deltas = [packet_times[i] - packet_times[i - 1] for i in range(1, len(packet_times))]
        mean_ms = sum(deltas) / len(deltas) * 1000.0
        min_ms = min(deltas) * 1000.0
        max_ms = max(deltas) * 1000.0
        print(f"    - Packet Arrival Delta: Mean={mean_ms:.2f} ms (expected: 2.63 ms), Min={min_ms:.2f} ms, Max={max_ms:.2f} ms")

    return rx_packets > 0


def test_usb_audio(seconds=3.0):
    print(f"\n[*] Testing USB UAC1 24-bit Audio capture for {seconds}s...")
    # Find ALSA card ID for DDC SDR
    res = subprocess.run(["arecord", "-l"], capture_output=True, text=True)
    card_idx = None
    # Prioritize D2026 or DDC SDR
    for line in res.stdout.splitlines():
        if "D2026" in line or "DDC SDR" in line:
            if "card" in line:
                card_idx = line.split("card")[1].split(":")[0].strip()
                break
    if card_idx is None:
        for line in res.stdout.splitlines():
            if "USB Audio" in line and "card" in line:
                card_idx = line.split("card")[1].split(":")[0].strip()
                break

    if card_idx is None:
        print("[-] Could not locate Pico-Dev-iCE USB Audio card in arecord -l.")
        return False

    device = f"hw:{card_idx},0"
    wav_path = "/tmp/flash_and_test_audio.wav"
    cmd = ["arecord", "-D", device, "-f", "S24_3LE", "-c", "2", "-r", "48000", "-d", str(int(seconds)), wav_path]
    print(f"[*] Running: {' '.join(cmd)}")
    rec_res = subprocess.run(cmd, capture_output=True, text=True)
    if rec_res.returncode != 0:
        print(f"[!] arecord failed: {rec_res.stderr}")
        return False

    file_size = os.path.getsize(wav_path)
    total_frames = (file_size - 44) // 6  # 6 bytes per 24-bit stereo frame
    expected_frames = int(seconds * 48000)
    print(f"[+] Audio Capture Results:")
    print(f"    - Captured Frames: {total_frames} / {expected_frames} expected ({total_frames / expected_frames * 100:.1f}%)")
    print(f"    - WAV Size:        {file_size} bytes")
    if abs(total_frames - expected_frames) < 200:
        print("    [PASS] USB Audio streaming is continuous and smooth!")
        return True
    else:
        print("    [FAIL] Frame count mismatch.")
        return False


def main():
    parser = argparse.ArgumentParser(description="Pico-Dev-iCE Flash, Test & Diagnostics Utility")
    parser.add_argument("--build", action="store_true", help="Recompile firmware before flashing")
    parser.add_argument("--flash", action="store_true", help="Flash firmware to connected Pico W")
    parser.add_argument("--diag", action="store_true", help="Query serial status & diagnostics")
    parser.add_argument("--wifi-connect", nargs="+", metavar=("SSID", "[PASS]"), help="Connect Pico W to Wi-Fi SSID [Password]")
    parser.add_argument("--test-stream", action="store_true", help="Test OpenHPSDR UDP streaming over Wi-Fi")
    parser.add_argument("--test-audio", action="store_true", help="Test USB UAC1 24-bit audio capture over ALSA")
    parser.add_argument("--ip", default=None, help="Target IP for OpenHPSDR streaming test")
    parser.add_argument("--port", default=None, help="Serial CDC device path")
    args = parser.parse_args()

    port = args.port or find_serial_port()

    # Default action if no flag is provided: flash & run full diagnostics
    should_flash = args.flash or args.build or (not args.diag and not args.wifi_connect and not args.test_stream and not args.test_audio)

    if args.build or (should_flash and not os.path.exists(UF2_PATH)):
        run_build()

    if should_flash:
        reboot_to_bootsel(port)
        mount = find_bootsel_mount()
        if not mount:
            print("[!] Could not find RPI-RP2 bootloader mount.")
            sys.exit(1)
        flash_uf2(mount, UF2_PATH)
        time.sleep(1.0)

    if args.wifi_connect:
        ssid = args.wifi_connect[0]
        passw = args.wifi_connect[1] if len(args.wifi_connect) > 1 else ""
        set_wifi(ssid, passw, port)

    discovered_ip = None
    if args.diag or should_flash or args.test_stream:
        discovered_ip = query_diagnostics(port, wait_for_wifi=args.test_stream)

    if args.test_audio or should_flash:
        test_usb_audio(seconds=3.0)

    if args.test_stream or should_flash:
        target_ip = args.ip if args.ip else (discovered_ip or "192.168.1.191")
        if target_ip and target_ip != "0.0.0.0":
            test_openhpsdr_stream(target_ip, duration=3.0)


if __name__ == "__main__":
    main()
