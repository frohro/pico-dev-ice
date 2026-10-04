# Dev-iCE DDC SDR Wi-Fi Firmware (OpenHPSDR Protocol 1)

This application runs on the **Raspberry Pi Pico W** on the Dev-iCE board. It interfaces with the iCE40 FPGA digital down-converter (DDC), receives 24-bit stereo I/Q samples over I2S via PIO, and streams them wirelessly over Wi-Fi using the standard **OpenHPSDR Protocol 1 (UDP port 1024)**.

All radio tuning, sample rate switching, and step-attenuator gain control are handled natively via OpenHPSDR Protocol 1 Command & Control (C&C) packets. All USB protocols (CDC and UAC1) have been removed, eliminating resource contention and timing conflicts.

## Compatible SDR Software

The firmware identifies as an **OpenHPSDR Hermes** device and works seamlessly with:
- **SDR++** (Select source: *Hermes*)
- **Quisk** (OpenHPSDR / Hermes hardware driver)
- **PowerSDR / Thetis**
- **SparkSDR**
- **linHPSDR**

## Dev-iCE Pico W Hardware Pinout

| Signal | GPIO | Direction | Notes |
| --- | ---: | --- | --- |
| FPGA SPI0 MISO | 4 | Pico input | Runtime SPI readback |
| FPGA SPI0 CS | 5 | Pico output | Shared CS for boot CRAM and runtime SPI |
| FPGA SPI0 SCK | 6 | Pico output | 10 MHz runtime SPI clock |
| FPGA SPI0 MOSI | 7 | Pico output | Runtime command frames to FPGA |
| FPGA I2S RX_DATA | 14 | Pico input | Baseband SDR I/Q audio from FPGA (Pin 11) |
| FPGA I2S BCK | 15 | Pico input | 3.072 MHz Bit Clock from FPGA (Pin 12) |
| FPGA I2S WS | 16 | Pico input | 48 kHz / 96 kHz Word Select from FPGA (Pin 9) |
| FPGA interrupt | 0 | Pico input | Active-high OTR clipping notification |
| PGA control mask | 8..11 | Pico outputs | PGA0..PGA3 digital step attenuators |
| FPGA CDONE | 21 | Pico input | HIGH when FPGA CRAM boot is complete |
| FPGA CRESET | 22 | Pico output | Active-low FPGA hardware reset |
| REF Multiplexer | 26 | Pico output | **0 = SDR RF Antenna RX**, 1 = VNA Input |
| T/R Switch | 28 | Pico output | **1 = RX Mode**, 0 = TX Mode |
| 30.720 MHz clock | FPGA pin 37 | External oscillator | Master DSP clock reference |

The FPGA is the I2S master. The firmware supports **48 kHz** and **96 kHz** sample rates, with Philips I2S framing, two channels (Left = I, Right = Q), 24 valid bits, and packages them into standard 1032-byte OpenHPSDR Protocol 1 EP6 packets (126 stereo samples / 252 words per packet).

## Automatic Gain Control (AGC) & Step Attenuators

The four PGA GPIOs (8..11) control the RF front-end step attenuators:
- `0x0`: Straight path, max gain (+40 dB)
- `0x1`: 5 dB pad engaged (+35 dB)
- `0x3`: 5 dB and 10 dB pads engaged (+25 dB)
- `0xF`: All pads engaged (-15 dB)

When an ADC overload occurs, the FPGA triggers an interrupt on GPIO 0. The firmware steps down the gain and clears the OTR condition. If the signal remains below threshold, the AGC decays back towards max sensitivity.

## Building for Pico W

Run the build script:

```bash
cd Software/ddc_sdr_WiFi_firmware
bash build_picow.sh
```

This compiles the firmware and embeds the DDC FPGA bitstream (`bitstreams/ddc_sdr_rx.bin`). The resulting binary is generated at:
```text
Software/ddc_sdr_WiFi_firmware/build-picow/ddc_sdr.uf2
```

## Flashing the Pico W

1. Put the Raspberry Pi Pico W in BOOTSEL mode: hold the BOOTSEL button while plugging the USB cable into your computer.
2. The `RPI-RP2` mass storage drive will appear.
3. Copy the firmware UF2:
   ```bash
   cp build-picow/ddc_sdr.uf2 /media/$USER/RPI-RP2/
   ```
4. The Pico W will reboot automatically, program the FPGA CRAM from flash, and connect to Wi-Fi.

## Wi-Fi Configuration

Settings are configured in [`wifi_config.h`](wifi_config.h):
- Primary SSID: `Frohne-Shop-2.4GHz`
- Secondary SSID: `Frohne-2.4GHz`
- Fallback Static IP: `192.168.1.191` (used if DHCP lease times out)
- Subnet Gateway: `192.168.1.1` | Netmask: `255.255.255.0`

## Pico W Status LED

The onboard Wi-Fi LED indicates connection and streaming state:

| Blink Pattern | Rate | Meaning |
| :--- | :--- | :--- |
| **Solid ON** | Constant | **Active SDR Streaming**: SDR client is actively receiving I/Q data. |
| **Steady Heartbeat** | 1 Hz | **Wi-Fi Connected (`CYW43_LINK_UP`)**: Listening for OpenHPSDR discovery. |
| **Medium Blink** | 2 Hz | **Associating / Acquiring IP** (`CYW43_LINK_JOIN` / `NO_IP`). |
| **Slow Blink** | 0.5 Hz | **Scanning / Reconnecting** (`CYW43_LINK_DOWN`). |

## Diagnostic & Verification Tools

### 1. Unified OpenHPSDR Test Suite
To verify network discovery, frequency tuning, streaming continuity, packet arrival delta, and I/Q balance:
```bash
python3 test_openhpsdr.py
```

### 2. Stream & FFT Spectrum Verification
To capture live packets and analyze the RF spectrum:
```bash
python3 test_openhpsdr_stream.py 192.168.1.191
```

### 3. Comprehensive Protocol 1 Compliance Test
```bash
python3 test_pico_w_openhpsdr.py --ip 192.168.1.191 --seconds 5.0
```
