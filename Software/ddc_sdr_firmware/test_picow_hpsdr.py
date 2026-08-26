#!/usr/bin/env python3
"""
Test script for Pico-Dev-iCE OpenHPSDR Protocol 1 over Wi-Fi.

Tests:
  1. Discovery broadcast (UDP 1024) -> verifies reply with Hermes ID 0x01
  2. Start RX streaming (UDP 1024 -> 0xEFFE 0x04 0x01)
  3. C&C Frequency tuning (UDP 1024 -> 0xEFFE 0x01 0x02 <freq>)
  4. Receive and parse EP6 I/Q packets (1032 bytes)
  5. TCP Control Server (TCP Port 5000)
"""

import socket
import struct
import time
import sys
import argparse

HPSDR_PORT = 1024
TCP_PORT = 5000

def discover_pico_w(timeout=3.0):
    print("[*] Sending OpenHPSDR Discovery Broadcast on UDP port 1024...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(timeout)
    
    # 0xEFFE 0x02 + 60 bytes of 0x00
    disc_packet = bytearray(63)
    disc_packet[0] = 0xEF
    disc_packet[1] = 0xFE
    disc_packet[2] = 0x02
    
    sock.sendto(disc_packet, ('<broadcast>', HPSDR_PORT))
    
    start_time = time.time()
    devices = []
    while time.time() - start_time < timeout:
        try:
            data, addr = sock.recvfrom(256)
            if len(data) >= 14 and data[0] == 0xEF and data[1] == 0xFE and data[2] == 0x02:
                mac_str = ":".join(f"{b:02X}" for b in data[3:9])
                fw_ver = f"{data[9] >> 4}.{data[9] & 0x0F}"
                board_id = data[10]
                board_names = {0x00: "Metis", 0x01: "Hermes", 0x02: "Griffin", 0x04: "Angelia", 0x05: "Orion", 0x06: "Hermes-Lite"}
                bname = board_names.get(board_id, f"Unknown (0x{board_id:02X})")
                print(f"[+] Found OpenHPSDR Device at {addr[0]}:{addr[1]}")
                print(f"    Board: {bname} | Firmware: v{fw_ver} | MAC: {mac_str}")
                devices.append((addr[0], addr[1], mac_str))
        except socket.timeout:
            break
    sock.close()
    return devices

def test_streaming(ip, duration=3.0, freq_hz=7050000):
    print(f"\n[*] Connecting to Pico W OpenHPSDR at {ip}:{HPSDR_PORT}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('', 0))
    sock.settimeout(1.0)
    
    # Send Start Command (0xEFFE 0x04 0x01)
    start_cmd = bytearray(64)
    start_cmd[0] = 0xEF
    start_cmd[1] = 0xFE
    start_cmd[2] = 0x04
    start_cmd[3] = 0x01 # Run RX
    sock.sendto(start_cmd, (ip, HPSDR_PORT))
    print(f"[+] Sent START command to {ip}:{HPSDR_PORT}")
    
    # Send C&C Frequency Tune Command (RX0 Frequency = cmd 0x02)
    cc_pkt = bytearray(1032)
    cc_pkt[0] = 0xEF
    cc_pkt[1] = 0xFE
    cc_pkt[2] = 0x01
    cc_pkt[3] = 0x02 # EP2 C&C
    # Subframe 1 sync: 0x7F 0x7F 0x7F
    cc_pkt[8] = 0x7F
    cc_pkt[9] = 0x7F
    cc_pkt[10] = 0x7F
    cc_pkt[11] = (0x02 << 1) # Command 0x02 (RX0 NCO)
    cc_pkt[12] = (freq_hz >> 24) & 0xFF
    cc_pkt[13] = (freq_hz >> 16) & 0xFF
    cc_pkt[14] = (freq_hz >> 8) & 0xFF
    cc_pkt[15] = freq_hz & 0xFF
    
    # Subframe 2 sync
    cc_pkt[520] = 0x7F
    cc_pkt[521] = 0x7F
    cc_pkt[522] = 0x7F
    
    sock.sendto(cc_pkt, (ip, HPSDR_PORT))
    print(f"[+] Sent C&C Tune Command: {freq_hz} Hz ({freq_hz/1e6:.4f} MHz)")
    
    # Capture EP6 Packets
    print(f"[*] Capturing streaming I/Q packets for {duration} seconds...")
    start_t = time.time()
    packet_count = 0
    total_bytes = 0
    last_seq = None
    seq_errors = 0
    
    while time.time() - start_t < duration:
        try:
            data, _ = sock.recvfrom(2048)
            if len(data) == 1032 and data[0] == 0xEF and data[1] == 0xFE and data[2] == 0x01 and data[3] == 0x06:
                seq = struct.unpack(">I", data[4:8])[0]
                if last_seq is not None and seq != (last_seq + 1):
                    seq_errors += 1
                last_seq = seq
                packet_count += 1
                total_bytes += len(data)
        except socket.timeout:
            pass
            
    # Send Stop Command
    stop_cmd = bytearray(64)
    stop_cmd[0] = 0xEF
    stop_cmd[1] = 0xFE
    stop_cmd[2] = 0x04
    stop_cmd[3] = 0x00 # Stop
    sock.sendto(stop_cmd, (ip, HPSDR_PORT))
    sock.close()
    
    rate_kbps = (total_bytes * 8) / (duration * 1000)
    pps = packet_count / duration
    print(f"\n[+] Stream Test Completed:")
    print(f"    Packets received: {packet_count} ({pps:.1f} pkts/sec)")
    print(f"    Data rate:        {rate_kbps:.1f} kbps")
    print(f"    Sequence drops:   {seq_errors}")
    return packet_count > 0

def test_tcp_control(ip):
    print(f"\n[*] Testing TCP Control Server on {ip}:{TCP_PORT}...")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect((ip, TCP_PORT))
        
        # Read greeting
        greeting = s.recv(128).decode('ascii', errors='ignore')
        print(f"    Greeting: {greeting.strip()}")
        
        # Send VER
        s.sendall(b"VER\r\n")
        ver = s.recv(128).decode('ascii', errors='ignore')
        print(f"    VER response: {ver.strip()}")
        
        # Send MODE
        s.sendall(b"MODE\r\n")
        mode = s.recv(128).decode('ascii', errors='ignore')
        print(f"    MODE response: {mode.strip()}")
        
        # Send WIFI?
        s.sendall(b"WIFI?\r\n")
        wifi_st = s.recv(128).decode('ascii', errors='ignore')
        print(f"    WIFI response: {wifi_st.strip()}")
        
        s.close()
        print("[+] TCP Control Server test SUCCESS!")
        return True
    except Exception as e:
        print(f"[-] TCP Control Server error: {e}")
        return False

def main():
    parser = argparse.ArgumentParser(description="Pico-Dev-iCE OpenHPSDR Protocol 1 Wi-Fi Tester")
    parser.add_argument("--ip", default="192.168.1.186", help="IP address of Pico W (default: 192.168.1.186)")
    parser.add_argument("--discover", action="store_true", help="Perform discovery broadcast first")
    parser.add_argument("--freq", type=int, default=7050000, help="Test frequency in Hz (default: 7050000)")
    args = parser.parse_args()
    
    target_ip = args.ip
    if args.discover:
        devs = discover_pico_w()
        if devs:
            target_ip = devs[0][0]
    
    test_tcp_control(target_ip)
    test_streaming(target_ip, duration=3.0, freq_hz=args.freq)

if __name__ == "__main__":
    main()
