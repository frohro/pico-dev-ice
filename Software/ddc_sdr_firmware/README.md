# Dev-iCE DDC SDR Firmware

A high-performance Direct Down-Conversion (DDC) Software Defined Radio (SDR) firmware for the **WWU Pico-Dev-iCE** platform, powered by a Raspberry Pi RP2040 / Pico W and an iCE40UP5K FPGA.

The FPGA implements the digital down-converter (mixer, 32-bit NCO, CIC decimators, half-band filters, and 24-bit stereo I2S master transmitter). The RP2040 receives the baseband I/Q stream over I2S DMA and simultaneously streams it to host software over **USB Audio (UAC1)** and **Wi-Fi (OpenHPSDR Protocol 1)**.

---

## Architecture Overview

```
                      +---------------------------------------+
                      |       iCE40UP5K FPGA (DDC SDR)        |
                      |  NCO + Mixer + CIC/FIR Decimators     |
                      +---------------------------------------+
                                  | I2S Master (BCK, WS, DATA)
                                  v
                      +---------------------------------------+
                      |      RP2040 PIO I2S Receiver SM       |
                      +---------------------------------------+
                                  | Ping-Pong DMA
                                  v
             +---------------------------------------------------------+
             |         Lock-Free Multi-Consumer SPSC Ring Buffer       |
             |       audio_ring[32][252] (756 bytes / 126 samples)     |
             |                   write_idx (DMA IRQ)                   |
             +---------------------------------------------------------+
                         |                                 |
                         v                                 v
         +-------------------------------+ +-------------------------------+
         |    CORE 0: Real-Time Audio    | |     CORE 1: Dedicated Wi-Fi   |
         |-------------------------------| |-------------------------------|
         | - TinyUSB UAC1 24-bit Audio   | | - CYW43439 Wi-Fi Driver       |
         | - USB CDC Serial Console      | | - lwIP 2.1.3 Network Stack    |
         | - FPGA SPI Tuning / Control   | | - OpenHPSDR Protocol 1 (UDP)  |
         | - Monotonic Hardware AGC      | | - SDPCM Credit Pacing Engine  |
         | - Multi-Core Mailbox Dispatch | | - LED Status & Link Watchdog  |
         +-------------------------------+ +-------------------------------+
                         |                                 |
                         v                                 v
                 USB Host Audio & CDC              Wi-Fi UDP Port 1024
               (SDR++, Quisk, Gqrx)              (SDR++, Thetis, Quisk)
```

### Dual-Core Thread Isolation
To ensure smooth, sputter-free SDR audio, all execution is strictly partitioned between the two RP2040 cores:

1. **Core 0 — Deterministic Real-Time Audio & Hardware**:
   - Executes `tud_task()`, `cdc_task()`, `audio_task()`, `agc_task()`, and `handle_fpga_interrupt()`.
   - Dedicated exclusively to hardware operations with microsecond-level determinism.
   - Consumes samples from `audio_ring` using `ring_usb_read_idx` and pushes them to the TinyUSB endpoint buffer.
   - Dispatches FPGA frequency tuning and PGA gain commands over SPI.
   - **Zero Wi-Fi or network code runs on Core 0**, guaranteeing that USB audio never suffers from network stack jitter or mutex contention.

2. **Core 1 — Dedicated Wi-Fi & OpenHPSDR Protocol 1**:
   - Executes `core1_entry()`, dedicated 100% to CYW43 Wi-Fi and lwIP networking.
   - Initializes the CYW43439 driver, associates with the AP, and enables `GMODE_PERFORMANCE` (54 Mbps OFDM only).
   - Runs the OpenHPSDR Protocol 1 server on UDP port 1024 (Hermes board profile).
   - Consumes samples from `audio_ring` using `ring_wifi_read_idx`, formats standard 1032-byte EP6 dual-subframe frames, and dispatches them via `udp_sendto()`.
   - Never blocks Core 0 or shares mutexes with Core 0.

### Lock-Free Multi-Consumer SPSC Ring Buffer
Audio streaming between the hardware DMA and both consumers is completely lock-free:
- `audio_ring[32][256]`: 32 buffers of 252 words (126 stereo samples each, matching exactly 1 OpenHPSDR UDP packet).
- **Producer**: The DMA IRQ advances `ring_write_idx` every 2.625 ms (at 48 kHz).
- **Consumer 1 (USB)**: `ring_usb_read_idx` tracks USB endpoint availability independently.
- **Consumer 2 (Wi-Fi)**: `ring_wifi_read_idx` tracks Wi-Fi transmission credits independently.
- Both streams operate simultaneously without mutexes, spinlocks, or priority inversion.

### CYW43 SDPCM Credit Pacing
The CYW43439 Wi-Fi chip uses a credit-based flow control system. Under heavy UDP transmission, transmitting without verifying credits causes the driver's internal loop to block for 1.0 second per packet (`CYW43_SDPCM_SEND_TIMEOUT`).
- The firmware implements `cyw43_wait_credit()` on Core 1, polling credits with a bounded 1.2 ms timeout.
- If the Wi-Fi chip is temporarily busy servicing beacons or channel jitter, Core 1 yields cleanly rather than blocking.
- If network congestion causes lag to exceed 16 buffers, older frames are dropped to keep the live stream strictly real-time.

---

## Hardware Pinout (WWU Pico-Dev-iCE)

| Signal | GPIO | Direction | Notes |
| :--- | :---: | :--- | :--- |
| **FPGA SPI MISO** | 4 | Pico input | Runtime telemetry / SPI readback |
| **FPGA SPI CS** | 5 | Pico output | Shared CS for boot CRAM and runtime SPI |
| **FPGA SPI SCK** | 6 | Pico output | 10 MHz runtime SPI clock |
| **FPGA SPI MOSI** | 7 | Pico output | Runtime command frames to FPGA |
| **FPGA I2S RX_DATA** | 14 | Pico input | Baseband SDR I/Q audio from FPGA (Pin 11) |
| **FPGA I2S BCK** | 15 | Pico input | 3.072 MHz Bit Clock from FPGA (Pin 12) |
| **FPGA I2S WS** | 16 | Pico input | 48 kHz Word Select / LRCLK from FPGA (Pin 9) |
| **FPGA I2S TX_DATA** | 13 | Pico output | Transmit audio to FPGA (Pin 10) |
| **FPGA Interrupt** | 0 | Pico input | Active-high OTR clipping notification |
| **PGA Attenuator Mask** | 8..11 | Pico outputs | PGA0..PGA3 digital step attenuators |
| **FPGA CDONE** | 21 | Pico input | HIGH when FPGA CRAM configuration completes |
| **FPGA CRESET_B** | 22 | Pico output | Active-low FPGA hardware reset |
| **REF Multiplexer** | 26 | Pico output | **0 = SDR RF Antenna RX**, 1 = VNA Input |
| **T/R Switch** | 28 | Pico output | **1 = RX Mode**, 0 = TX Mode |
| **Pico W Status LED** | WL_GPIO0 | Pico W output | Wi-Fi link status & streaming indicator |
| **Master Clock** | FPGA pin 37 | Oscillator | 30.720 MHz ultra-low-jitter clock reference |

---

## Pico W Status LED Indicators

The onboard LED on the CYW43 Wi-Fi module indicates connection and streaming state:

| Pattern | Frequency | Description |
| :--- | :---: | :--- |
| **Solid ON** | Constant | **Active SDR Streaming**: SDR++ (Hermes), Quisk, or Thetis is streaming I/Q audio over UDP. |
| **Steady Blink** | **1.0 Hz** (500 ms on / off) | **Wi-Fi Connected (`LINK_UP`)**: Assigned IP via DHCP/Static; ready for SDR connection. |
| **Medium Blink** | **2.0 Hz** (250 ms on / off) | **Associating (`LINK_JOIN` / `NO_IP`)**: Connecting to AP or acquiring DHCP lease. |
| **Slow Blink** | **0.5 Hz** (1000 ms on / off) | **Disconnected (`LINK_DOWN` / `FAIL`)**: Retrying connection every 4 seconds. |

---

## CDC Serial Command Reference

The USB CDC interface (`/dev/ttyACM0`) provides an interactive command shell:

| Command | Example Response | Description |
| :--- | :--- | :--- |
| `VER` | `VER,DDC SDR 0.2` | Firmware version |
| `MODE` | `MODE,DDC` | SDR architecture mode |
| `XTAL` | `XTAL,30720000` | Master oscillator frequency in Hz |
| `FREQ,<hz>` | `14074000 OK` | Tunes NCO center frequency (calculates 32-bit FCW) |
| `RATE,<hz>` | `RATE,48000 OK` | Sets sample rate (clamped safely to 48000) |
| `PGA` | `PGA,0 OK` | Queries PGA attenuator code (0 = +40 dB, 15 = -15 dB) |
| `PGA,<code>` | `PGA,3 OK` | Sets PGA attenuator code (0..15) |
| `REF` | `REF,0 OK` | Queries RF switch (0 = SDR Antenna, 1 = VNA Input) |
| `REF,<0\|1>` | `REF,0 OK` | Sets RF switch |
| `WIFI` | `WIFI,UP,IP,192.168.1.191,SSID,...` | Reports live Wi-Fi link state, IP, and SSID |
| `JOIN,<ssid>` | `JOIN_START,0,SSID,...` | Connects to a new Wi-Fi network |
| `HPSDR` | `HPSDR: act=1, push=1142, sent=...` | OpenHPSDR stream diagnostics, throughput, and packet counters |
| `PROF` | `PROF: loops=9606390 tud=461 ...` | Microsecond execution profiling across all Core 0 tasks |
| `BOOTSEL` | `REBOOTING_BOOTSEL` | Soft-reboots RP2040 into USB BOOTSEL flash mode |

---

## Management & Automation Tool (`manage_sdr.py`)

A unified management script is provided for one-step building, flashing, and verification:

```bash
# Full build, flash, Wi-Fi wait, and automated stream test:
python3 manage_sdr.py cycle

# Flash firmware UF2 to board:
python3 manage_sdr.py flash

# Wait for and check Wi-Fi connection:
python3 manage_sdr.py wifi

# Run OpenHPSDR Protocol 1 test suite:
python3 manage_sdr.py test

# Display live HPSDR streaming statistics:
python3 manage_sdr.py stats

# Display Core 0 execution profiler:
python3 manage_sdr.py prof

# Send custom CDC command:
python3 manage_sdr.py cmd "FREQ,7074000"
```

---

## Using with Host SDR Software

### SDR++ (over Wi-Fi)
1. In the **Source** panel, select **Hermes Source**.
2. Click **Refresh**.
3. In the device dropdown, select your board (e.g. `28:CD:C1:0F:EA:08 (192.168.1.191)`).
4. Select Sample Rate: **48 kHz**.
5. Click **Play (▶)**.
6. The spectrum and waterfall will begin streaming immediately with zero packet loss.

### SDR++ (over USB)
1. In the **Source** panel, select **SoapySDR**.
2. Select driver `driver=2026_sdr` or ALSA audio card `D2026`.
3. Click **Play (▶)**.

---

## Building from Source

Prerequisites: `pico-sdk` (v1.5+), `arm-none-eabi-gcc`, `cmake`, and `python3`.

```bash
cd Software/ddc_sdr_firmware
./build_picow.sh
```

The output UF2 will be created at:
```
Software/ddc_sdr_firmware/build-picow/ddc_sdr.uf2
```
