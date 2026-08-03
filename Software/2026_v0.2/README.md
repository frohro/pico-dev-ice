# Research: 48/96 kHz PCM1808 UAC1 SDR (v0.2)

Firmware for the **Intro-to-CAD-2026 v0.2** board (YD-RP2040) that streams
stereo 24-bit I2S audio from the **PCM1808** ADC over USB Audio Class 1.0 at
either **48 kHz** or **96 kHz**.  Quisk selects the sample rate at runtime via
a CDC command — no jumper changes required.

## Key Features

- **Single ADC**: PCM1808, driven by the Si5351a CLK0 master clock at
  **24.576 MHz** (= 512 × 48 kHz = 256 × 96 kHz).
- **Dynamic Rate Switching**: 48 kHz ↔ 96 kHz at runtime, controlled by Quisk
  over USB CDC (`RATE,<hz>`).
- **USB Audio Class 1.0**: 24-bit stereo S24_3LE at both rates.
- **Software Mode Control**: M0 is held LOW (I2S format, also enforced by
  firmware GPIO).  M1 is toggled by firmware to select 48 kHz (M1=0) or
  96 kHz (M1=1).

## Hardware Configuration (v0.2 Board)

### PCM1808 Mode Pins
| Signal | GPIO | Logic Level | Function |
| :--- | :--- | :--- | :--- |
| **M0 (MD0)** | 22 | Always LOW | Selects I2S standard format |
| **M1 (MD1)** | 26 | 0 = 48 kHz, 1 = 96 kHz | Sample rate selection |

> **Note**: The PCM1808 **FMT** pin is held LOW by a hardware jumper on the
> board; no GPIO is assigned to it.

### PCM1808 I2S Pins (PIO slave receiver)
| Signal | GPIO |
| :--- | :--- |
| **DATA (SDOUT)** | GPIO 9 |
| **BCK** | GPIO 10 |
| **WS (LRCK)** | GPIO 11 |

### Si5351a (I2C + clock generation)
| Signal | GPIO |
| :--- | :--- |
| **SDA** | GPIO 12 |
| **SCL** | GPIO 13 |
| **CLK0** → PCM1808 SCKI | 24.576 MHz fixed |
| **CLK1/CLK2** → QSD LO | Tunable via `FREQ` command |

## CDC Control Protocol

Quisk controls the hardware over the CDC serial port using a line-oriented
ASCII protocol:

| Command | Description |
| :--- | :--- |
| `VER` | Returns firmware version string |
| `XTAL` | Returns Si5351a reference frequency (24576000) |
| `MODE` | Returns `DIRECT` or `JOHNSON` (LO topology) |
| `RATE,<hz>` | Set sample rate: **48000** or **96000**.  Immediately reconfigures M1, PIO, and DMA. |
| `FREQ,<hz>,<N>,<a>,<b>,<c>,<P1>,<P2>,<P3>` | Program the Si5351a LO registers. |

All commands return `OK\r\n` on success or `ERROR,...\r\n` on failure.

## USB Alternate Settings

| Alt | Sample Rate | Format | Max EP size |
| :--- | :--- | :--- | :--- |
| 0 | — | zero bandwidth | — |
| 1 | 48,000 Hz | 2ch 24-bit S24_3LE | 294 B |
| 2 | 96,000 Hz | 2ch 24-bit S24_3LE | 582 B |

## Building the Firmware

This project requires a standalone **TinyUSB** checkout (version ≥ 0.19) as the
version bundled with the Pico SDK does not include the necessary UAC1 headers.

```bash
# 1. Clone TinyUSB if you haven't already
git clone https://github.com/hathach/tinyusb ~/tinyusb
export PICO_TINYUSB_PATH=$HOME/tinyusb

# 2. Build the project
mkdir -p build && cd build
cmake ..
make -j4
```

Flash the resulting `sdr_2026_v0_2.uf2` to your YD-RP2040.

## Testing

```bash
python3 test_pcm1808.py
```

The script finds the CDC device automatically (`0xcafe:0x4080` or
`/dev/ttyACM0`), verifies the firmware version and XTAL frequency, tests both
sample rates, and captures 1 second of audio at each rate to confirm the full
USB audio path is working.

The ALSA card name assigned by Linux is `S2026` (short name derived from the
USB iProduct string `SDR PCM1808 2026`).  To capture manually:

```bash
# List capture devices — look for [SDR PCM1808 2026]
arecord -l

# Capture 5 seconds at 48 kHz
arecord -D plughw:CARD=S2026,DEV=0 -r 48000 -f S24_3LE -c 2 --duration=5 out.wav

# Capture 5 seconds at 96 kHz
arecord -D plughw:CARD=S2026,DEV=0 -r 96000 -f S24_3LE -c 2 --duration=5 out.wav
```
