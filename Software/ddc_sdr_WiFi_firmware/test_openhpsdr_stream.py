#!/usr/bin/env python3
"""
test_openhpsdr_stream.py — OpenHPSDR Protocol 1 Wi-Fi Diagnostic and Stream Tester
Used to verify discovery, control, and 24-bit I/Q UDP streaming from the Pico W SDR.
"""

import socket
import struct
import time
import sys
import numpy as np

DEFAULT_IP = "192.168.1.191"
DEFAULT_PORT = 1024
HOST_PORT = 1024

def test_openhpsdr(pico_ip=DEFAULT_IP, duration=5.0, freq_hz=7050000, rate_hz=48000):
    print("=" * 65)
    print(f"  OpenHPSDR Protocol 1 Wi-Fi Diagnostic Tool")
    print(f"  Target: {pico_ip}:{DEFAULT_PORT} | Tuning: {freq_hz/1e6:.3f} MHz | Rate: {rate_hz} Hz")
    print("=" * 65)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(('', HOST_PORT))
    except Exception as e:
        print(f"[!] Could not bind to local port {HOST_PORT}: {e}")
        print("    Retrying with OS-assigned local port...")
        sock.bind(('', 0))

    sock.settimeout(2.0)

    # 1. OpenHPSDR Discovery Broadcast / Directed probe
    print("\n[1/4] Sending Discovery Probe...")
    disc = bytearray(63)
    disc[0], disc[1], disc[2] = 0xEF, 0xFE, 0x02
    sock.sendto(disc, (pico_ip, DEFAULT_PORT))

    try:
        resp, addr = sock.recvfrom(1024)
        if len(resp) >= 11 and resp[0] == 0xEF and resp[1] == 0xFE:
            mac_str = ":".join(f"{b:02X}" for b in resp[3:9])
            fw_ver = f"{resp[9] >> 4}.{resp[9] & 0x0F}"
            board_id = resp[10]
            board_name = {0x01: "Hermes", 0x02: "Metis", 0x06: "Hermes-Lite"}.get(board_id, f"Unknown(0x{board_id:02X})")
            print(f"  ✓ Found Device: {board_name} at {addr[0]}")
            print(f"    MAC: {mac_str} | Firmware Version: v{fw_ver}")
        else:
            print(f"  ⚠ Received unknown discovery response: {resp[:16].hex()}")
    except socket.timeout:
        print(f"  ✗ Discovery timed out! Device at {pico_ip} did not respond.")
        print("    Check that Pico W is connected to Wi-Fi and pingable.")
        sock.close()
        return False

    # 2. Start Streaming Command
    print("\n[2/4] Sending Start Streaming Command (0xEFFE 0x04 0x01)...")
    start_pkt = bytearray(64)
    start_pkt[0], start_pkt[1], start_pkt[2], start_pkt[3] = 0xEF, 0xFE, 0x04, 0x01
    sock.sendto(start_pkt, (pico_ip, DEFAULT_PORT))

    # 3. Send Frequency & Rate C&C Packet
    print(f"[3/4] Sending C&C Packet: Freq={freq_hz} Hz ({freq_hz/1e6:.3f} MHz), Rate={rate_hz} Hz...")
    cc_pkt = bytearray(1032)
    cc_pkt[0], cc_pkt[1], cc_pkt[2], cc_pkt[3] = 0xEF, 0xFE, 0x01, 0x02
    cc_pkt[8], cc_pkt[9], cc_pkt[10] = 0x7F, 0x7F, 0x7F  # C&C Sync Pattern
    cc_pkt[11] = 0x02  # RX0 / VFO Frequency command
    cc_pkt[12] = (freq_hz >> 24) & 0xFF
    cc_pkt[13] = (freq_hz >> 16) & 0xFF
    cc_pkt[14] = (freq_hz >> 8) & 0xFF
    cc_pkt[15] = freq_hz & 0xFF
    sock.sendto(cc_pkt, (pico_ip, DEFAULT_PORT))

    # 4. Stream Capture & Verification
    print(f"\n[4/4] Capturing 24-bit I/Q stream for {duration:.1f} seconds...")
    packets_received = 0
    seq_errors = 0
    last_seq = None
    all_i = []
    all_q = []

    start_time = time.time()
    sock.settimeout(1.0)

    while time.time() - start_time < duration:
        try:
            data, addr = sock.recvfrom(2048)
            if len(data) >= 1032 and data[0] == 0xEF and data[1] == 0xFE:
                ep = data[3]
                seq = struct.unpack('>I', data[4:8])[0]
                packets_received += 1

                if last_seq is not None and seq != (last_seq + 1) & 0xFFFFFFFF:
                    seq_errors += 1
                last_seq = seq

                # Each 1032-byte packet contains 2 subframes of 512 bytes each:
                # Subframe 1: offset 8..519 (Sync 8..10, C0..C4 11..15, 63 samples of 6 bytes: 16..393)
                # Subframe 2: offset 520..1031 (Sync 520..522, C0..C4 523..527, 63 samples of 6 bytes: 528..905)
                for sf_offset in (8, 520):
                    sample_start = sf_offset + 8
                    for s_idx in range(63):
                        off = sample_start + s_idx * 6
                        # 24-bit signed big-endian
                        raw_i = int.from_bytes(data[off:off+3], byteorder='big', signed=True)
                        raw_q = int.from_bytes(data[off+3:off+6], byteorder='big', signed=True)
                        all_i.append(raw_i)
                        all_q.append(raw_q)
        except socket.timeout:
            pass

    elapsed = time.time() - start_time

    # 5. Send Stop Streaming
    print("\n[-] Stopping stream (0xEFFE 0x04 0x00)...")
    stop_pkt = bytearray(64)
    stop_pkt[0], stop_pkt[1], stop_pkt[2], stop_pkt[3] = 0xEF, 0xFE, 0x04, 0x00
    sock.sendto(stop_pkt, (pico_ip, DEFAULT_PORT))
    sock.close()

    # 6. Report Statistics
    total_samples = len(all_i)
    actual_sample_rate = (total_samples / elapsed) if elapsed > 0 else 0
    packet_rate = (packets_received / elapsed) if elapsed > 0 else 0

    print("\n" + "=" * 65)
    print("  CAPTURE RESULTS & STREAM HEALTH")
    print("=" * 65)
    print(f"  Duration:           {elapsed:.2f} s")
    print(f"  Packets Received:   {packets_received} ({packet_rate:.1f} pkts/sec)")
    print(f"  Sequence Errors:    {seq_errors}")
    print(f"  Total I/Q Samples:  {total_samples} ({actual_sample_rate:.1f} samples/sec)")

    if total_samples > 0:
        arr_i = np.array(all_i, dtype=np.float64)
        arr_q = np.array(all_q, dtype=np.float64)
        rms_i = np.sqrt(np.mean(arr_i**2))
        rms_q = np.sqrt(np.mean(arr_q**2))
        iq_ratio = (rms_i / rms_q) if rms_q > 0 else 0.0

        print(f"  I Channel RMS:      {rms_i:.1f} (min: {np.min(arr_i):.0f}, max: {np.max(arr_i):.0f})")
        print(f"  Q Channel RMS:      {rms_q:.1f} (min: {np.min(arr_q):.0f}, max: {np.max(arr_q):.0f})")
        print(f"  I/Q Balance Ratio:  {iq_ratio:.3f}")

        # FFT Analysis
        iq_complex = arr_i + 1j * arr_q
        fft = np.abs(np.fft.fftshift(np.fft.fft(iq_complex[:4096])))
        peak_bin = np.argmax(fft)
        peak_freq_offset = (peak_bin - 2048) * (rate_hz / 4096)
        print(f"  Spectral Peak:      {peak_freq_offset/1e3:+.2f} kHz relative to center")

        if rms_i > 10 and rms_q > 10 and 0.5 <= iq_ratio <= 2.0:
            print("\n  [✓ PASS] OpenHPSDR Wi-Fi streaming is active, valid, and healthy!")
            return True
        else:
            print("\n  [!] Warning: Signal levels or I/Q balance appear anomalous.")
            return False
    else:
        print("\n  [✗ FAIL] No I/Q audio packets were received from the device.")
        return False

if __name__ == "__main__":
    ip = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_IP
    test_openhpsdr(ip)
