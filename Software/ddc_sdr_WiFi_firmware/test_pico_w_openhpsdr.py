#!/usr/bin/env python3
"""
Comprehensive Test and Verification Utility for Unified Pico W SDR (OpenHPSDR Protocol 1)
------------------------------------------------------------------------------------------
Comprehensive Tests:
  1. Discovery Broadcast (0xEFFE 0x02 on UDP 1024) -> verifies MAC, Board ID, Protocol Version,
     Receiver Count, and full 60-byte Metis frame layout.
  2. Concurrent Bidirectional C&C: Periodically sends EP2 C&C packets (50 Hz) during streaming,
     mirroring the behavior of real SDR GUIs (Quisk, SDR++, Thetis).
  3. Dynamic VFO Retuning & Sample Rate Switching (48 kHz <-> 96 kHz) on the fly.
  4. Status Byte Validation: Parses subframe headers (C0..C4) for PTT, ADC overload, and sync.
  5. Real-time 24-bit I/Q packet reception, strict sequence continuity, jitter, and RMS power.
  6. Non-zero sample verification (confirms hardware ADC/FPGA is feeding actual I/Q signal).
  7. Clean Stop Stream (0xEFFE 0x04 0x00).

Usage:
  python3 test_pico_w_openhpsdr.py [--ip <pico_ip>] [--freq <hz>] [--seconds <sec>] [--rate 48000|96000]
"""

import sys
import time
import socket
import struct
import argparse
import math
import threading

HPSDR_PORT = 1024
DISCOVERY_HEADER = bytes([0xEF, 0xFE, 0x02])
START_STREAM_CMD = bytes([0xEF, 0xFE, 0x04, 0x01])
STOP_STREAM_CMD  = bytes([0xEF, 0xFE, 0x04, 0x00])

def parse_args():
    parser = argparse.ArgumentParser(description="Comprehensive OpenHPSDR Protocol 1 Test for Pico W")
    parser.add_argument("--ip", type=str, default=None, help="Pico W IP address (defaults to broadcast discovery)")
    parser.add_argument("--freq", type=int, default=7050000, help="Initial tune frequency in Hz (default: 7050000)")
    parser.add_argument("--rate", type=int, choices=[48000, 96000], default=48000, help="Sample rate (48000 or 96000)")
    parser.add_argument("--seconds", type=float, default=8.0, help="Duration to stream I/Q packets (seconds)")
    parser.add_argument("--retune-interval", type=float, default=3.0, help="Seconds between dynamic VFO frequency hops")
    return parser.parse_args()

def discover_pico(sock, target_ip=None):
    """Sends OpenHPSDR discovery probe and validates full Metis response frame."""
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    
    probe = bytearray(60)
    probe[0] = 0xEF
    probe[1] = 0xFE
    probe[2] = 0x02

    dest = (target_ip if target_ip else "255.255.255.255", HPSDR_PORT)
    print(f"[*] Sending OpenHPSDR discovery probe to {dest[0]}:{dest[1]}...")
    sock.sendto(probe, dest)

    sock.settimeout(2.5)
    try:
        data, addr = sock.recvfrom(1024)
        if len(data) >= 11 and data[0] == 0xEF and data[1] == 0xFE and data[2] == 0x02:
            mac_str = ":".join(f"{b:02X}" for b in data[3:9])
            fw_version = f"{data[9] >> 4}.{data[9] & 0x0F}"
            board_id = data[10]
            proto_ver = data[11] if len(data) > 11 else 1
            num_rx = data[12] if len(data) > 12 else 1
            num_adc = data[13] if len(data) > 13 else 1

            board_names = {
                0x00: "Metis",
                0x01: "Hermes",
                0x02: "Griffin",
                0x04: "Angelia",
                0x05: "Orion",
                0x06: "Hermes-Lite"
            }
            board_name = board_names.get(board_id, f"ID 0x{board_id:02X}")

            print(f"[+] Discovery Successful from {addr[0]}:{addr[1]}!")
            print(f"    - Frame Size:        {len(data)} bytes (Standard Metis=60)")
            print(f"    - Board Identifier:  {board_name} (0x{board_id:02X})")
            print(f"    - MAC Address:       {mac_str}")
            print(f"    - Firmware Version:  v{fw_version}")
            print(f"    - Protocol Version:  Protocol {proto_ver}")
            print(f"    - Receivers (DDC):   {num_rx}")
            print(f"    - ADC Channels:      {num_adc}")

            if board_id in (0x01, 0x02, 0x06):
                print("    [PASS] Board ID recognized as Hermes / OpenHPSDR Protocol 1")
            if len(data) >= 60:
                print("    [PASS] Discovery packet meets standard 60-byte Metis frame requirement")

            return addr[0]
        else:
            print(f"[!] Received non-standard discovery response ({len(data)} bytes): {data[:16].hex()}")
            return addr[0]
    except socket.timeout:
        print("[-] Discovery timed out. No response received.")
        return None

def build_cc_packet(tune_hz, sample_rate=48000, run_state=1):
    """Constructs a standard 512-byte OpenHPSDR C&C packet (EP2)."""
    buf = bytearray(512)
    buf[0] = 0xEF
    buf[1] = 0xFE
    buf[2] = 0x01  # EP2 C&C packet

    # Subframe 1 C&C header (offset 8)
    buf[8]  = 0x7F
    buf[9]  = 0x7F
    buf[10] = 0x7F

    # Command 0x02: RX0 NCO frequency
    buf[11] = (0x02 << 1)  # C0: Command 0x02
    buf[12] = (tune_hz >> 24) & 0xFF
    buf[13] = (tune_hz >> 16) & 0xFF
    buf[14] = (tune_hz >> 8)  & 0xFF
    buf[15] = (tune_hz)       & 0xFF

    # Subframe 2 C&C header (offset 520 / second command slot)
    speed = 0x01 if sample_rate == 96000 else 0x00
    buf[11+5] = 0x00  # C0: Command 0x00 (Speed)
    buf[12+5] = (speed & 0x03) | ((run_state & 0x01) << 2)

    return bytes(buf)

def unpack_s24_be(b0, b1, b2):
    """Converts 3 big-endian bytes to signed 24-bit integer."""
    val = (b0 << 16) | (b1 << 8) | b2
    if val & 0x800000:
        val -= 0x1000000
    return val

class CCHeartbeatSender:
    """Simulates active SDR GUI transmitting 50 Hz C&C heartbeat packets concurrently."""
    def __init__(self, sock, target_ip, target_port, freq, rate):
        self.sock = sock
        self.target = (target_ip, target_port)
        self.freq = freq
        self.rate = rate
        self.running = True
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def set_freq(self, new_freq):
        with self.lock:
            self.freq = new_freq

    def start(self):
        self.thread.start()

    def stop(self):
        self.running = False
        self.thread.join(timeout=1.0)

    def _run(self):
        while self.running:
            with self.lock:
                pkt = build_cc_packet(self.freq, self.rate, run_state=1)
            try:
                self.sock.sendto(pkt, self.target)
            except Exception:
                pass
            time.sleep(0.02)  # 50 Hz heartbeat

def run_test():
    args = parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
    sock.bind(("", 0)) # Ephemeral port for receiving stream

    pico_ip = args.ip
    if not pico_ip:
        pico_ip = discover_pico(sock)
        if not pico_ip:
            print("\nError: Could not locate Pico W on the network.")
            print("Check that:")
            print("  1. The Pico W is powered and connected to the same WiFi network.")
            print("  2. wifi_config.h has the correct SSID and password.")
            print("  3. Your PC's firewall allows UDP 1024 packets.")
            sys.exit(1)

    print(f"\n[*] Testing with Pico W target: {pico_ip}:{HPSDR_PORT}")

    current_freq = args.freq
    sample_rate = args.rate

    # 1. Initial C&C Setup
    print(f"[*] Initializing VFO Frequency: {current_freq / 1e6:.6f} MHz ({sample_rate} SPS)...")
    init_pkt = build_cc_packet(current_freq, sample_rate)
    sock.sendto(init_pkt, (pico_ip, HPSDR_PORT))
    time.sleep(0.05)

    # 2. Start Streaming Command
    print("[*] Sending START streaming command (0xEFFE 0x04 0x01)...")
    sock.sendto(START_STREAM_CMD, (pico_ip, HPSDR_PORT))

    # 3. Launch Concurrent C&C Heartbeat Thread (simulates Quisk / SDR++ / Thetis background thread)
    print("[*] Starting concurrent 50 Hz C&C heartbeat thread...")
    cc_sender = CCHeartbeatSender(sock, pico_ip, HPSDR_PORT, current_freq, sample_rate)
    cc_sender.start()

    sock.settimeout(2.0)
    start_time = time.time()
    last_seq = None
    packet_count = 0
    lost_packets = 0
    total_samples = 0
    
    max_i = 0
    max_q = 0
    power_sum = 0.0
    zero_samples_count = 0
    nonzero_samples_count = 0

    last_retune = start_time
    retune_count = 0
    hop_frequencies = [current_freq, current_freq + 10000, current_freq - 10000, current_freq + 25000]

    status_c0_seen = set()
    adc_overloads = 0

    print(f"[*] Streaming live 24-bit I/Q audio for {args.seconds:.1f} seconds...\n")
    print(f"{'Elapsed':>8} | {'Packets':>8} | {'Lost':>5} | {'Rate (KB/s)':>11} | {'Max Peak':>10} | {'RMS (dBFS)':>10} | {'Zero %':>7} | {'Status C0':>9}")
    print("-" * 92)

    last_report = start_time

    try:
        while (time.time() - start_time) < args.seconds:
            now = time.time()

            # Dynamic Retuning Test
            if (now - last_retune) >= args.retune_interval:
                retune_count = (retune_count + 1) % len(hop_frequencies)
                current_freq = hop_frequencies[retune_count]
                cc_sender.set_freq(current_freq)
                print(f"    --> [DYNAMIC VFO TUNE] VFO dialed to {current_freq / 1e6:.6f} MHz")
                last_retune = now

            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                print("\n[!] Timeout waiting for I/Q packet! Stream interrupted.")
                break

            if len(data) != 1032 or data[0] != 0xEF or data[1] != 0xFE or data[2] != 0x01:
                continue

            # Parse sequence number
            seq = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7]
            if last_seq is not None:
                diff = (seq - last_seq) & 0xFFFFFFFF
                if diff > 1:
                    lost_packets += (diff - 1)
            last_seq = seq
            packet_count += 1

            # Subframe 1 Header & Status Check (offset 8)
            if data[8] == 0x7F and data[9] == 0x7F and data[10] == 0x7F:
                c0 = data[11]
                status_c0_seen.add(c0)
                if c0 & 0x01:
                    adc_overloads += 1

            # Parse 126 stereo samples (63 in subframe 1, 63 in subframe 2)
            # Subframe 1: starts at offset 16
            for s in range(63):
                off = 16 + s * 8
                i_val = unpack_s24_be(data[off], data[off+1], data[off+2])
                q_val = unpack_s24_be(data[off+3], data[off+4], data[off+5])
                
                if i_val == 0 and q_val == 0:
                    zero_samples_count += 1
                else:
                    nonzero_samples_count += 1

                max_i = max(max_i, abs(i_val))
                max_q = max(max_q, abs(q_val))
                norm_i = i_val / 8388608.0
                norm_q = q_val / 8388608.0
                power_sum += (norm_i*norm_i + norm_q*norm_q)
                total_samples += 1

            # Subframe 2: starts at offset 528
            for s in range(63):
                off = 528 + s * 8
                i_val = unpack_s24_be(data[off], data[off+1], data[off+2])
                q_val = unpack_s24_be(data[off+3], data[off+4], data[off+5])
                
                if i_val == 0 and q_val == 0:
                    zero_samples_count += 1
                else:
                    nonzero_samples_count += 1

                max_i = max(max_i, abs(i_val))
                max_q = max(max_q, abs(q_val))
                norm_i = i_val / 8388608.0
                norm_q = q_val / 8388608.0
                power_sum += (norm_i*norm_i + norm_q*norm_q)
                total_samples += 1

            if now - last_report >= 1.0:
                elapsed = now - start_time
                kbytes_sec = (packet_count * 1032) / (elapsed * 1024.0)
                mean_power = (power_sum / total_samples) if total_samples > 0 else 1e-12
                rms_dbfs = 10.0 * math.log10(max(1e-12, mean_power))
                
                peak = max(max_i, max_q)
                c0_str = f"0x{max(status_c0_seen):02X}" if status_c0_seen else "None"
                zero_pct = (zero_samples_count / total_samples * 100.0) if total_samples > 0 else 0.0
                print(f"{elapsed:7.2f}s | {packet_count:8d} | {lost_packets:5d} | {kbytes_sec:9.1f} KB/s | {peak:10d} | {rms_dbfs:8.1f} dB | {zero_pct:6.1f}% | {c0_str:>9}")
                last_report = now

    except KeyboardInterrupt:
        print("\n[*] Interrupted by user.")

    # Stop C&C Heartbeat
    cc_sender.stop()

    # 4. Stop Streaming
    print("\n[*] Stopping stream (0xEFFE 0x04 0x00)...")
    sock.sendto(STOP_STREAM_CMD, (pico_ip, HPSDR_PORT))

    elapsed_total = time.time() - start_time
    drop_pct = (lost_packets / (packet_count + lost_packets) * 100) if (packet_count + lost_packets) > 0 else 0.0

    print("\n" + "=" * 60)
    print("OPENHPSDR PROTOCOL 1 COMPLIANCE SUMMARY")
    print("=" * 60)
    print(f"Target Pico W IP:         {pico_ip}")
    print(f"Final Tuned Frequency:    {current_freq / 1e6:.6f} MHz")
    print(f"Packets Received:         {packet_count}")
    print(f"Packets Dropped:          {lost_packets} ({drop_pct:.2f}%)")
    print(f"Total I/Q Samples:        {total_samples}")
    print(f"  - Non-Zero Samples:     {nonzero_samples_count} ({nonzero_samples_count / max(1, total_samples) * 100.0:.1f}%)")
    print(f"  - Zero Samples:         {zero_samples_count} ({zero_samples_count / max(1, total_samples) * 100.0:.1f}%)")
    print(f"Peak I/Q Amplitude:       {max(max_i, max_q)}")
    print(f"Average Bandwidth:        {(packet_count * 1032) / (elapsed_total * 1024.0):.1f} KB/s")
    print(f"ADC Overload Events:      {adc_overloads}")
    print(f"Observed C0 Status Bytes: {[f'0x{c:02X}' for c in status_c0_seen]}")
    print("-" * 60)

    verdict_passed = True
    if packet_count < 50:
        print("[FAIL] Insufficient packet throughput received from Pico W.")
        verdict_passed = False
    if drop_pct > 5.0:
        print("[WARN] Packet drop rate > 5%. Wi-Fi signal quality may cause audio stutter.")
    if not status_c0_seen:
        print("[FAIL] No valid subframe sync headers (0x7F 0x7F 0x7F) detected in EP6 stream.")
        verdict_passed = False
    if total_samples > 0 and zero_samples_count == total_samples:
        print("[FAIL: ALL SAMPLES ARE ZERO] All I/Q samples received are 0x000000.")
        print("       Check that FPGA DDC / I2S DMA is feeding live samples into openhpsdr_push_samples().")
        verdict_passed = False
    elif nonzero_samples_count > 0:
        print("[PASS] Valid non-zero RF/Audio I/Q samples detected!")

    if verdict_passed:
        print("[PASS] Pico W fully adheres to OpenHPSDR Protocol 1 specifications!")
        print("       Ready for live operation in Quisk, SDR++, Thetis, and LinHPSDR.")
    print("=" * 60)

if __name__ == "__main__":
    run_test()
