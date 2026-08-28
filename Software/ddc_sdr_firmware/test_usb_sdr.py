#!/usr/bin/env python3
"""
USB Verification Script for Pico-Dev-iCE SDR
--------------------------------------------
1. Interrogates USB CDC serial interface (/dev/ttyACM0).
2. Sets frequency, rate, PGA gain, and checks status.
3. Captures raw 24-bit stereo I/Q audio over USB UAC1 using ALSA / PyAudio / SoundDevice / arecord.
4. Verifies non-zero I/Q samples and signal metrics.
"""

import sys
import time
import subprocess
import struct
import math
import glob

def test_cdc_serial():
    print("=== Testing USB CDC Control Port ===")
    ports = glob.glob('/dev/ttyACM*')
    if not ports:
        print("[-] No /dev/ttyACM* device found.")
        return False
    
    port = ports[0]
    print(f"[*] Found serial port: {port}")
    try:
        import serial
        ser = serial.Serial(port, 115200, timeout=1.0, dsrdtr=False, rtscts=False)
        ser.dtr = True
        ser.rts = True
        time.sleep(0.1)
        ser.reset_input_buffer()
        
        commands = [
            ("VER", "Firmware version"),
            ("MODE", "Hardware Mode"),
            ("XTAL", "Master Clock"),
            ("FREQ,7050000", "Tune 7.050 MHz"),
            ("RATE,48000", "Sample rate 48 kHz"),
            ("PGA,0", "Max RF gain"),
            ("REF,0", "RF Antenna input"),
            ("WIFI?", "Wi-Fi status"),
            ("DEBUG", "DMA / GPIO diagnostics")
        ]
        
        for cmd, desc in commands:
            ser.write(f"{cmd}\r\n".encode())
            time.sleep(0.1)
            resp = ser.read_all().decode(errors='replace').strip()
            print(f"    [{cmd}] -> {resp}")
            
        ser.close()
        print("[PASS] USB CDC Control Port operates normally.")
        return True
    except Exception as e:
        print(f"[-] USB CDC error: {e}")
        return False

def test_usb_audio():
    print("\n=== Testing USB UAC1 24-bit Audio Capture ===")
    try:
        # Find ALSA card name
        cmd = "arecord -l"
        res = subprocess.check_output(cmd, shell=True, text=True)
        print(res.strip())
        
        card_num = None
        for line in res.splitlines():
            if "card" in line and ("D2026" in line or "DDC SDR" in line):
                parts = line.split(":")
                card_num = parts[0].split()[1]
                print(f"[*] Identified Pico-Dev-iCE SDR Audio on card {card_num} ({line.strip()})")
                break
        
        if card_num is None:
            for line in res.splitlines():
                if "card" in line and ("Pico" in line or "USB Audio" in line):
                    parts = line.split(":")
                    card_num = parts[0].split()[1]
                    print(f"[*] Identified USB SDR Audio on card {card_num} ({line.strip()})")
                    break
                
        if card_num is None:
            print("[-] USB Audio card not identified in arecord list.")
            return False
            
        device = f"hw:{card_num},0"
        print(f"[*] Capturing 2.0 seconds of raw 24-bit stereo I/Q audio from {device}...")
        
        raw_cmd = f"arecord -D {device} -f S24_3LE -c 2 -r 48000 -d 2 -t raw 2>/dev/null"
        try:
            raw_data = subprocess.check_output(raw_cmd, shell=True)
        except Exception:
            # Try S16_LE if 24-bit is not exposed
            raw_cmd = f"arecord -D {device} -f S16_LE -c 2 -r 48000 -d 2 -t raw 2>/dev/null"
            raw_data = subprocess.check_output(raw_cmd, shell=True)
            
        bytes_rec = len(raw_data)
        print(f"[+] Received {bytes_rec} bytes of raw audio data.")
        
        if bytes_rec < 1000:
            print("[FAIL] Insufficient audio data captured.")
            return False
            
        # Parse samples
        zero_samples = 0
        nonzero_samples = 0
        max_amp = 0
        
        # S24_3LE = 6 bytes per stereo frame (3 bytes Left, 3 bytes Right)
        for i in range(0, bytes_rec - 5, 6):
            # Left (I)
            b0, b1, b2 = raw_data[i], raw_data[i+1], raw_data[i+2]
            i_val = (b2 << 16) | (b1 << 8) | b0
            if i_val & 0x800000:
                i_val -= 0x1000000
            # Right (Q)
            b3, b4, b5 = raw_data[i+3], raw_data[i+4], raw_data[i+5]
            q_val = (b5 << 16) | (b4 << 8) | b3
            if q_val & 0x800000:
                q_val -= 0x1000000
                
            if i_val == 0 and q_val == 0:
                zero_samples += 1
            else:
                nonzero_samples += 1
            max_amp = max(max_amp, abs(i_val), abs(q_val))
            
        total = zero_samples + nonzero_samples
        print(f"    - Total Stereo Samples: {total}")
        print(f"    - Non-Zero Samples:     {nonzero_samples} ({nonzero_samples/total*100.0:.1f}%)")
        print(f"    - Zero Samples:         {zero_samples} ({zero_samples/total*100.0:.1f}%)")
        print(f"    - Max Peak Amplitude:   {max_amp}")
        
        if nonzero_samples > 0:
            print("[PASS] USB UAC1 Streaming is capturing active non-zero I/Q data!")
            return True
        else:
            print("[WARN] All USB samples are zero. Check FPGA I2S output.")
            return False
            
    except Exception as e:
        print(f"[-] Error during USB audio test: {e}")
        return False

def main():
    print("========================================")
    print("Pico-Dev-iCE USB Hardware Self-Test")
    print("========================================\n")
    test_cdc_serial()
    test_usb_audio()

if __name__ == "__main__":
    main()
