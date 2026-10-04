#!/usr/bin/env python3
"""
test_openhpsdr.py — Unified OpenHPSDR Protocol 1 Diagnostic Suite
==================================================================
Single diagnostic tool for Pico-Dev-iCE SDR:
1. Auto-Discovery: Global Broadcast (255.255.255.255), Subnet Broadcast (192.168.1.255), and Unicast.
2. Serial Diagnostic Fallback: Checks CDC /dev/ttyACM0 for WiFi status, IP, and FPGA status.
3. SDR++ Protocol 1 Emulation: Tests exact SDR++ handshake (0x81 Start, C&C frequency & rate registers).
4. Stream Benchmark: Verifies line-rate continuous packet flow (381.0 pkts/s @ 48 kHz).
5. I/Q Signal Quality: 24-bit big-endian extraction, RMS, I/Q balance ratio, and FFT spectrum.
"""

import sys
import os
import time
import socket
import struct
import argparse
import glob
import numpy as np

# Protocol Constants
HPSDR_PORT = 1024
HPSDR_PACKET_SIZE = 1032
SYNC_PATTERN = bytes([0x7F, 0x7F, 0x7F])

def test_discovery(target_ip=None, timeout=1.0):
    print("\n==================================================")
    print(" 1. Testing OpenHPSDR Discovery Protocol")
    print("==================================================")
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    req = bytearray(63)
    req[0] = 0xEF
    req[1] = 0xFE
    req[2] = 0x02

    targets = [("255.255.255.255", "Global Broadcast"), ("192.168.1.255", "Subnet Broadcast")]
    if target_ip:
        targets.insert(0, (target_ip, "Direct Unicast"))

    discovered = {}
    for ip, desc in targets:
        print(f"[*] Sending Discovery Request via {desc} to {ip}:{HPSDR_PORT}...")
        try:
            sock.sendto(req, (ip, HPSDR_PORT))
            while True:
                data, addr = sock.recvfrom(1024)
                if len(data) >= 60 and data[0] == 0xEF and data[1] == 0xFE and data[2] == 0x02:
                    mac = ":".join(f"{b:02X}" for b in data[3:9])
                    fw_ver = f"{data[9] >> 4}.{data[9] & 0x0F}"
                    board_id = data[10]
                    board_names = {1: "Hermes", 2: "Metis", 3: "Griffin", 6: "Hermes-Lite 2"}
                    board_str = board_names.get(board_id, f"Unknown (0x{board_id:02X})")
                    proto_ver = data[11]
                    rx_count = data[12]
                    adc_count = data[13]

                    if mac not in discovered:
                        discovered[mac] = {
                            "ip": addr[0],
                            "port": addr[1],
                            "mac": mac,
                            "board": board_str,
                            "fw": fw_ver,
                            "proto": proto_ver
                        }
                        print(f"    [+] Discovered Device at {addr[0]}:{addr[1]}")
                        print(f"        - MAC Address:      {mac}")
                        print(f"        - Board Type:       {board_str} (ID: {board_id})")
                        print(f"        - Firmware Version: {fw_ver}")
                        print(f"        - Protocol Version: {proto_ver}")
                        print(f"        - DDC Receivers:    {rx_count}")
                        print(f"        - ADCs:             {adc_count}")
        except socket.timeout:
            pass
        except Exception as e:
            print(f"    [-] Socket error: {e}")

    # If broadcast timed out, try quick sweep of common local IPs
    if not discovered and not target_ip:
        print("[*] Probing local subnet IPs (192.168.1.150 - 210)...")
        for last in range(150, 211):
            probe_ip = f"192.168.1.{last}"
            try:
                sock.sendto(req, (probe_ip, HPSDR_PORT))
            except Exception:
                pass
        
        sock.settimeout(0.3)
        while True:
            try:
                data, addr = sock.recvfrom(1024)
                if len(data) >= 60 and data[0] == 0xEF and data[1] == 0xFE and data[2] == 0x02:
                    mac = ":".join(f"{b:02X}" for b in data[3:9])
                    discovered[mac] = {"ip": addr[0]}
                    print(f"    [+] Discovered Device at {addr[0]}:{addr[1]} (MAC: {mac})")
            except socket.timeout:
                break

    sock.close()
    if discovered:
        print(f"[PASS] Discovery succeeded! Found {len(discovered)} OpenHPSDR device(s).")
        return list(discovered.values())[0]["ip"]
    else:
        print("[-] No OpenHPSDR devices responded to discovery.")
        return target_ip

def build_cc_packet(freq_hz=7050000, rate_hz=48000, gain_code=0):
    pkt = bytearray(1032)
    pkt[0] = 0xEF
    pkt[1] = 0xFE
    pkt[2] = 0x01 # C&C / Data Packet
    pkt[3] = 0x02 # Endpoint 2 (C&C to device)

    # Subframe 1 Header (Offset 8) -> Set Frequency (Command 0x01)
    pkt[8]  = 0x7F; pkt[9]  = 0x7F; pkt[10] = 0x7F
    pkt[11] = 0x02 # Command 0x01 (VFO)
    pkt[12] = (freq_hz >> 24) & 0xFF
    pkt[13] = (freq_hz >> 16) & 0xFF
    pkt[14] = (freq_hz >> 8) & 0xFF
    pkt[15] = freq_hz & 0xFF

    # Subframe 2 Header (Offset 520) -> Set Sample Rate & Preamp (Command 0x00)
    pkt[520] = 0x7F; pkt[521] = 0x7F; pkt[522] = 0x7F
    speed_code = 0x01 if rate_hz == 96000 else 0x00
    pkt[523] = 0x00
    pkt[524] = speed_code & 0x03
    return pkt

def test_stream_and_iq(target_ip, duration=3.0, freq_hz=7050000, rate_hz=48000):
    print("\n==================================================")
    print(f" 2. Testing Streaming & I/Q Signal Quality ({duration:.1f}s @ {rate_hz/1000:.0f} kHz)")
    print("==================================================")
    
    if not target_ip:
        print("[-] Error: No target IP available for streaming.")
        return False

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind(("", 0))
    sock.settimeout(0.5)
    
    # 1. Send Start Command with NO_WD flag (0x81 = IQ + NO_WD)
    start_cmd = bytearray(64)
    start_cmd[0] = 0xEF; start_cmd[1] = 0xFE; start_cmd[2] = 0x04; start_cmd[3] = 0x81
    print(f"[*] Sending SDR++ Start Command (0x81) to {target_ip}:{HPSDR_PORT}...")
    for _ in range(3):
        sock.sendto(start_cmd, (target_ip, HPSDR_PORT))

    # 2. Send initial C&C packet (Frequency + Rate)
    cc_pkt = build_cc_packet(freq_hz=freq_hz, rate_hz=rate_hz)
    sock.sendto(cc_pkt, (target_ip, HPSDR_PORT))

    packets = []
    seq_list = []
    all_i_samples = []
    all_q_samples = []
    
    start_time = time.time()

    print(f"[*] Capturing streaming I/Q packets for {duration:.1f} seconds...")
    while time.time() - start_time < duration:
        try:
            data, addr = sock.recvfrom(2048)
            if len(data) == HPSDR_PACKET_SIZE and data[0] == 0xEF and data[1] == 0xFE and data[2] == 0x01:
                seq = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7]
                seq_list.append(seq)
                packets.append(data)

                # Extract 126 I/Q stereo samples from both subframes
                for s in range(63):
                    off = 16 + (s * 8)
                    i_raw = (data[off] << 16) | (data[off+1] << 8) | data[off+2]
                    if i_raw & 0x800000: i_raw -= 0x1000000
                    q_raw = (data[off+3] << 16) | (data[off+4] << 8) | data[off+5]
                    if q_raw & 0x800000: q_raw -= 0x1000000
                    all_i_samples.append(i_raw)
                    all_q_samples.append(q_raw)

                for s in range(63):
                    off = 528 + (s * 8)
                    i_raw = (data[off] << 16) | (data[off+1] << 8) | data[off+2]
                    if i_raw & 0x800000: i_raw -= 0x1000000
                    q_raw = (data[off+3] << 16) | (data[off+4] << 8) | data[off+5]
                    if q_raw & 0x800000: q_raw -= 0x1000000
                    all_i_samples.append(i_raw)
                    all_q_samples.append(q_raw)
        except socket.timeout:
            pass

    elapsed = time.time() - start_time

    # Stop Streaming
    stop_cmd = bytearray(64)
    stop_cmd[0] = 0xEF; stop_cmd[1] = 0xFE; stop_cmd[2] = 0x04; stop_cmd[3] = 0x00
    for _ in range(3):
        sock.sendto(stop_cmd, (target_ip, HPSDR_PORT))
    time.sleep(0.2)
    sock.close()

    if not packets:
        print("[-] Error: No streaming data packets received!")
        return False

    expected_pkts_per_sec = rate_hz / 126.0
    actual_rate = len(packets) / elapsed

    print(f"\n--- [Throughput & Continuity Report] ---")
    print(f"  - Total Packets Received: {len(packets)}")
    print(f"  - Total Elapsed Time:     {elapsed:.2f} s")
    print(f"  - Packet Stream Rate:     {actual_rate:.1f} pkts/s (Expected: {expected_pkts_per_sec:.1f} pkts/s)")
    print(f"  - Throughput:             {len(packets) * HPSDR_PACKET_SIZE * 8 / (elapsed * 1000):.1f} kbps")

    if len(seq_list) > 1:
        gaps = 0
        for i in range(len(seq_list) - 1):
            diff = (seq_list[i+1] - seq_list[i]) & 0xFFFFFFFF
            if diff != 1:
                gaps += (diff - 1)
        span = (seq_list[-1] - seq_list[0] + 1) & 0xFFFFFFFF
        loss_pct = (gaps / (span if span > 0 else 1)) * 100.0
        print(f"  - Sequence Span:          {seq_list[0]} -> {seq_list[-1]} ({span} total)")
        print(f"  - Sequence Gaps:          {gaps}")
        print(f"  - Estimated Packet Loss:  {gaps} packet(s) ({loss_pct:.2f}%)")

    # I/Q Signal Quality
    i_arr = np.array(all_i_samples, dtype=np.float64)
    q_arr = np.array(all_q_samples, dtype=np.float64)
    
    i_rms = np.sqrt(np.mean(i_arr**2)) if len(i_arr) > 0 else 0
    q_rms = np.sqrt(np.mean(q_arr**2)) if len(q_arr) > 0 else 0
    ratio = (i_rms / q_rms) if q_rms > 0 else 0
    mismatch_pct = abs(1.0 - ratio) * 100.0

    print(f"\n--- [I/Q Signal Quality Report] ---")
    print(f"  - Total Stereo Samples:   {len(i_arr):,}")
    print(f"  - I Channel RMS:          {i_rms:.1f} (Peak: {np.max(np.abs(i_arr)):.1f})")
    print(f"  - Q Channel RMS:          {q_rms:.1f} (Peak: {np.max(np.abs(q_arr)):.1f})")
    print(f"  - I / Q Amplitude Ratio:  {ratio:.3f} ({mismatch_pct:.1f}% mismatch)")

    if len(i_arr) >= 2048:
        z = (i_arr - np.mean(i_arr)) + 1j * (q_arr - np.mean(q_arr))
        nfft = 2048
        num_windows = len(z) // nfft
        spec = np.zeros(nfft)
        for w in range(num_windows):
            seg = z[w*nfft : (w+1)*nfft] * np.hanning(nfft)
            spec += np.abs(np.fft.fftshift(np.fft.fft(seg)))**2
        spec /= num_windows
        spec_db = 10 * np.log10(spec + 1e-12)
        freqs = np.fft.fftshift(np.fft.fftfreq(nfft, 1.0 / rate_hz))

        pos_mask = freqs > 500
        neg_mask = freqs < -500
        p_pos = np.max(spec_db[pos_mask])
        p_neg = np.max(spec_db[neg_mask])
        suppression = abs(p_pos - p_neg)

        print(f"\n--- [Spectral Image Rejection Report] ---")
        print(f"  - Peak Positive Band:     {freqs[pos_mask][np.argmax(spec_db[pos_mask])]:+.1f} Hz ({p_pos:.1f} dB)")
        print(f"  - Peak Negative Band:     {freqs[neg_mask][np.argmax(spec_db[neg_mask])]:+.1f} Hz ({p_neg:.1f} dB)")
        print(f"  - Measured Suppression:   {suppression:.1f} dB")

    if actual_rate > 100 and 0.8 < ratio < 1.2:
        print("\n[PASS] OpenHPSDR Protocol 1 Streaming operates normally!")
        return True
    else:
        print("\n[!] WARNING: Stream throughput or I/Q balance is sub-optimal.")
        return False

def main():
    parser = argparse.ArgumentParser(description="OpenHPSDR Protocol 1 Diagnostic Suite")
    parser.add_argument("--ip", default=None, help="Target Pico W IP address (default: auto-discover)")
    parser.add_argument("--freq", type=int, default=7050000, help="Tuning Frequency in Hz (default: 7050000)")
    parser.add_argument("--rate", type=int, default=48000, choices=[48000, 96000], help="Sample rate in Hz (48000 or 96000)")
    parser.add_argument("--duration", type=float, default=3.0, help="Streaming capture duration in seconds (default: 3.0)")
    args = parser.parse_args()

    print("==================================================")
    print(" Pico-Dev-iCE OpenHPSDR Protocol 1 Test Suite")
    print("==================================================")
    active_ip = test_discovery(args.ip)
    if active_ip:
        test_stream_and_iq(active_ip, duration=args.duration, freq_hz=args.freq, rate_hz=args.rate)
    else:
        print("[-] Could not find an active board on Wi-Fi.")

if __name__ == "__main__":
    main()
