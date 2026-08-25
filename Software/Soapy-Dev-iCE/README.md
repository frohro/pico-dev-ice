# SoapySDR Driver for Pico-Dev-iCE DDC SDR & WWU 2026 SDR

A native [SoapySDR](https://github.com/pothosware/SoapySDR) module and hardware driver for the **Pico-Dev-iCE DDC SDR** (RP2040 + iCE40UP5K FPGA Software Defined Radio board) and the **WWU 2026 SDR** board.

This driver exposes the board as a standard SoapySDR receiver device, making it compatible with SDR software such as **SDR++**, **GQRX**, **GNU Radio**, and **CubicSDR**.

---

## Hardware Architecture & How It Works

```
                       ┌────────────────────────────────────────┐
                       │          Pico-Dev-iCE Board            │
                       │                                        │
[ Antenna ] ──> [ Front End / PGA ] <── [ MS9280 High-Speed ADC ]
                        │                         │             │
                    RF Signal                     │ 30.72 MSPS  │
                        ▼                         ▼             │
                [ iCE40UP5K FPGA ] ── NCO / Mixer / CIC DDC     │
                        │                                       │
                     I2S I/Q (24-bit stereo)                    │
                        ▼                                       │
                 [ RP2040 MCU ]                                 │
                        │                                       │
                        └────────────┬────────────┘             │
                                     │ USB                      │
                       ┌─────────────┴────────────┐             │
                       │  USB Audio   │  USB CDC  │             │
                       └──────┬───────┴─────┬─────┘             │
                              │             │                   │
══════════════════════════════╪═════════════╪═══════════════════╪══════════════════
 HOST COMPUTER                │             │
                              ▼             ▼
                       [ miniaudio ]   [ SerialPort ]
                        (Direct ALSA)   (115200 baud)
                              │             │
                              └──────┬──────┘
                                     ▼
                              [ Soapy2026SDR ]
                                     │
                                     ▼
                              [ SDR++ / GQRX ]
```

### 1. Control Protocol (USB CDC Serial)
* **Discovery:** Scans available serial ports for USB Vendor/Product IDs `0x1209:0xB1C0` (Pico-Dev-iCE DDC SDR) as well as `0xCAFE:0x4011` / `0xCAFE:0x4010` (WWU 2026 SDR).
* **Direct NCO Frequency Tuning (DDC):** When tuning frequency, the driver sends `FREQ,<hz>\r\n`. The RP2040 firmware computes the 32-bit frequency tuning word (FCW) and programs the FPGA NCO over SPI.
* **PGA Gain Control:** Digital step attenuator codes (0x0 = +40 dB, 0x1 = +35 dB, 0x3 = +25 dB, 0xF = -15 dB) via `PGA,<code>\r\n`.
* **Antenna Switching:** Front-end RF switch via `REF,<0|1>\r\n` (0 = Antenna RX, 1 = VNA Input).

### 2. Audio & I/Q Baseband Capture (USB Audio)
* **ADC / DDC:** The FPGA streams I2S baseband samples (Left = I, Right = Q) at 24-bit resolution (`S24_3LE`) into the RP2040.
* **ALSA Direct Bypass:** Binds directly to the ALSA hardware device `hw:CARD=D2026,DEV=0` (or `hw:CARD=SDR,DEV=0`) using `miniaudio` with `noAutoResample = true` to prevent any OS audio resampling.
* **Buffering & Format Conversion:** Converts 24-bit PCM samples to normalized float32 complex samples (`CF32` or `CS16`) in an internal lock-protected ring buffer.

---

## Dependencies & Prerequisites

### Linux (Ubuntu / Debian / Raspberry Pi OS)
```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    libsoapysdr-dev \
    soapysdr-tools \
    libasound2-dev \
    libudev-dev \
    python3-soapysdr \
    python3-numpy
```

### Serial Port Permissions
Make sure your user has permissions to access USB serial ports (`/dev/ttyACM*`):
```bash
sudo usermod -a -G dialout $USER
```
*(Log out and back in if this was just added).*

---

## Building and Installing

1. **Build and install using the build script:**
   ```bash
   cd Software/Soapy-Dev-iCE
   bash build.sh
   ```

2. **Or build manually with CMake:**
   ```bash
   cd Software/Soapy-Dev-iCE
   mkdir -p build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
   make -j$(nproc)
   sudo make install
   sudo ldconfig
   ```

3. **Verify the installation:**
   Ensure the board is plugged into USB, then run:
   ```bash
   SoapySDRUtil --find="driver=2026sdr"
   ```
   Or probe full details:
   ```bash
   SoapySDRUtil --probe="driver=2026sdr"
   ```

4. **Run the functional test (optional):**
   ```bash
   python3 test_driver.py --freq 7100000 --rate 48000
   ```

---

## Using with SDR++

1. **Launch SDR++:**
   ```bash
   sdrpp
   ```
2. **Select Source:**
   * In the top-left **Source** panel, select **SoapySDR**.
3. **Select Device:**
   * In the device drop-down, select **WWU Dev-iCE DDC SDR** (or **DDC SDR 2026**).
   * If it doesn't appear, click the **Refresh** button next to the device list.
4. **Configure Parameters:**
   * **Sample Rate:** Select `48000` (48 kHz) or `96000` (96 kHz).
   * **Antenna:** Select `RX` (for HF Antenna input) or `VNA` (for VNA input).
   * **Gain:** Adjust between `-15 dB` and `+40 dB` (maps to FPGA PGA attenuation states).
5. **Start Receiving:**
   * Click the **Play (▶)** button in the top-left.
   * Tune to your desired HF frequency (e.g. `7.100 MHz` for 40m amateur band, `14.200 MHz` for 20m amateur band).
   * Choose demodulation (AM, LSB, USB, CW, etc.) in the Radio panel.

---

## Troubleshooting

* **Device not detected by `SoapySDRUtil --find`:**
  * Check USB connection: `lsusb` should show `1209:b1c0` (or `cafe:4011`/`cafe:4010`).
  * Check serial port exists: `ls -la /dev/ttyACM*`.
  * Ensure the module was installed to the SoapySDR search path (inspect paths with `SoapySDRUtil --info`).
* **Permission denied on `/dev/ttyACM*`:**
  * Run `sudo usermod -a -G dialout $USER` and log back in.
* **Audio device not found / no sound in SDR++:**
  * Check `cat /proc/asound/cards` to verify the USB audio device is enumerated.
