# Dev-iCE DDC SDR firmware

This application is the Dev-iCE successor to the earlier Si5351a/Tayloe/
PCM1808 implementation. The FPGA replaces the Si5351a LO, Tayloe detector, and PCM1808 audio ADC path. It
receives the FPGA's I/Q stream as PCM1808-compatible I2S and exposes the same
UAC1 stereo 24-bit capture format to SDR++ or Quisk.

## Dev-iCE Pico pins

| Signal | GPIO | Direction | Notes |
| --- | ---: | --- | --- |
| FPGA SPI0 MISO | 4 | Pico input | Runtime telemetry / SPI readback |
| FPGA SPI0 CS | 5 | Pico output | Shared CS for boot CRAM and runtime SPI |
| FPGA SPI0 SCK | 6 | Pico output | 10 MHz runtime SPI clock |
| FPGA SPI0 MOSI | 7 | Pico output | Runtime command frames to FPGA |
| FPGA I2S RX_DATA | 14 | Pico input | Baseband SDR I/Q audio from FPGA (Pin 11) |
| FPGA I2S BCK | 15 | Pico input | 3.072 MHz Bit Clock from FPGA (Pin 12) |
| FPGA I2S WS | 16 | Pico input | 48 kHz Word Select / LRCLK from FPGA (Pin 9) |
| FPGA I2S TX_DATA | 13 | Pico output | Transmit audio to FPGA (Pin 10) |
| FPGA interrupt | 0 | Pico input | Active-high OTR clipping notification |
| PGA control mask | 8..11 | Pico outputs | PGA0..PGA3 digital step attenuators |
| FPGA CDONE | 21 | Pico input | HIGH when FPGA CRAM boot is complete |
| FPGA CRESET | 22 | Pico output | Active-low FPGA hardware reset |
| REF Multiplexer | 26 | Pico output | **0 = SDR RF Antenna RX**, 1 = VNA Input |
| T/R Switch | 28 | Pico output | **1 = RX Mode**, 0 = TX Mode |
| Onboard Pico LED | 25 | Pico output | Toggles on FREQ and RATE tuning activity |
| 30.720 MHz clock | FPGA pin 37 | External oscillator | Master DSP clock reference |

The FPGA is the I2S master. The firmware supports 48 kHz (and 96 kHz)
UAC1 streaming, with standard Philips I2S framing, two channels (Left = I, Right = Q),
24 valid bits, and little-endian three-byte (`S24_3LE`) USB samples.

## FPGA SPI protocol

Runtime SPI uses the same CS as CRAM configuration, GPIO 5. The Pico sends
this versioned frame after configuration:

```text
D5 01 command 04 value[31:0] little-endian
```

Commands are:

| Command | Value |
| ---: | --- |
| `01` | Set center frequency; value is a 32-bit FCW |
| `02` | Set output sample rate, 48000 or 96000 |
| `03` | Reserved for status reads |
| `04` | Clear the FPGA's sticky OTR event when value bit 0 is set |

The four PGA GPIOs form a hardware control mask: `PGA0` is the least-
significant bit and `PGA3` is the most-significant bit. This is not a linear
attenuation number. The verified gain states are:

| Mask | Hardware action | Nominal gain |
| ---: | --- | ---: |
| `0x0` | All MOSFETs off; straight path | `+40 dB` |
| `0x1` | 5 dB pad engaged | `+35 dB` |
| `0x3` | 5 dB and 10 dB pads engaged | `+25 dB` |
| `0xF` | All pads engaged; both LNAs bypassed | `-15 dB` |

The automatic overload path uses only the monotonic, verified sequence
`0x0 -> 0x1 -> 0x3 -> 0xF`. It starts at `0x0`, advances after each FPGA
OTR interrupt, and remains at `0xF` after the final step. Other masks remain
available for manual calibration, but are not selected automatically until
their gain has been measured.

The FPGA latches an OTR event, holds `fpga_int` high, and keeps it high
until it receives the `DDC_FPGA_CMD_CLEAR_OTR` command.

## CDC Serial Command Reference

The USB CDC interface (`/dev/ttyACM0`) supports interactive terminals (`picocom`, `minicom`, PuTTY)
with support for both `\r` (CR) and `\n` (LF) line endings, backspace (`0x08` / `0x7F`), and Ctrl+C (`0x03`).

| Command | Response | Description |
| :--- | :--- | :--- |
| `VER` | `VER,DDC SDR 0.1` / `OK` | Reports firmware version |
| `MODE` | `MODE,DDC` / `OK` | Reports SDR architecture mode (`DDC`) |
| `XTAL` | `XTAL,30720000` / `OK` | Reports master clock frequency in Hz |
| `FPGA,STATUS` | `FPGA,RX` / `OK` | Reports currently active FPGA image (`RX`, `TX`, or `DFU`) |
| `FPGA,LOAD,RX` | `FPGA,RX` / `OK` | Reconfigures FPGA CRAM with stored RX image |
| `FPGA,LOAD,TX` | `FPGA,TX` / `OK` | Reconfigures FPGA CRAM with stored TX image |
| `FREQ,<hz>` | `<hz>` / `OK` | Sets NCO tuning frequency in Hz (calculates 32-bit FCW) |
| `RATE,<hz>` | `RATE,<hz> OK` | Sets baseband audio sample rate in Hz (e.g. 48000) |
| `REF` | `REF,<0\|1>` / `OK` | Queries front-end RF multiplexer (`0` = SDR RF RX, `1` = VNA) |
| `REF,<0\|1>` | `REF,<0\|1>` / `OK` | Sets front-end RF multiplexer (`0` = SDR RF RX, `1` = VNA) |
| `PGA` | `PGA,<code>` / `OK` | Queries current PGA digital attenuator code (`0` = max $+40\text{ dB}$ gain) |
| `PGA,<code>` | `PGA,<code>` / `OK` | Sets PGA digital attenuator code (`0..15`) |
| `DEBUG` | `DEBUG: ready=...` / `OK` | Reports real-time DMA, I2S toggle counts, and GPIO states |
| `BOOTSEL` | `REBOOTING_BOOTSEL` | Soft-reboots the RP2040 into USB BOOTSEL flash mode |
| `HELP` / `?` | Command list / `OK` | Lists all available interactive CDC commands |

## Build

Use the checked-in Pico SDK through the SDK port. Without a bitstream, the
firmware builds as a USB/DFU development image and waits for FPGA CRAM to be
loaded:

```sh
cmake -S Software/ddc_sdr_firmware -B Software/ddc_sdr_firmware/build \
    -DPICO_BOARD=pico_dev_ice -DPICO_NO_PICOTOOL=1
cmake --build Software/ddc_sdr_firmware/build -j2
```

For a production UF2 that configures the FPGA at every Pico boot, pass the
packed `icepack` output. The legacy `FPGA_BITSTREAM_BIN` variable is treated as
the stored RX image:

```sh
cmake -S Software/ddc_sdr_firmware -B Software/ddc_sdr_firmware/build-embedded \
    -DPICO_BOARD=pico_dev_ice -DPICO_NO_PICOTOOL=1 \
    -DFPGA_BOOT_MODE=STORED \
    -DFPGA_BITSTREAM_BIN=/path/to/ddc_sdr_rx.bin
cmake --build Software/ddc_sdr_firmware/build-embedded -j2
```

To store separate RX and TX images in the Pico firmware and boot into TX by
default, use:

```sh
cmake -S Software/ddc_sdr_firmware -B Software/ddc_sdr_firmware/build-stored \
    -DPICO_BOARD=pico_dev_ice -DPICO_NO_PICOTOOL=1 \
    -DFPGA_BOOT_MODE=STORED -DFPGA_DEFAULT_IMAGE=TX \
    -DFPGA_RX_BITSTREAM_BIN=/path/to/ddc_sdr_rx.bin \
    -DFPGA_TX_BITSTREAM_BIN=/path/to/lab10_tx.bin
cmake --build Software/ddc_sdr_firmware/build-stored -j2
```

`FPGA_BOOT_MODE=DFU` builds the development behavior: the Pico does not load
an image at reset and waits for `dfu-util`. `FPGA_BOOT_MODE=STORED` loads the
selected embedded image at every reset. `FPGA_DEFAULT_IMAGE` chooses RX or TX
when both images are present. The stored images are generated as separate C
arrays, so the same firmware can switch between them without reflashing the
Pico.

The `.uf2` flashes the RP2040. Dev-iCE has no FPGA configuration flash, so
FPGA CRAM is volatile and must be loaded again after reset or power loss.

While the firmware is running, the CDC control port accepts:

```text
FPGA,STATUS
FPGA,LOAD,RX
FPGA,LOAD,TX
```

`FPGA,LOAD,RX` and `FPGA,LOAD,TX` stop audio DMA, force GPIO28 high for
receive, release runtime SPI, reload the selected stored CRAM image, restore
the sample-rate and audio streams, and then allow the normal TX start path to
assert GPIO28 low. A missing image returns an error. `FPGA,STATUS` reports the
active image or `DFU`. The existing `DFU,PREPARE` and `dfu-util` workflow is
unchanged and remains useful for development images that are not embedded.

These are deployment and transceiver-profile choices. Use DFU while developing
or testing a new image, and use STORED when the Pico should boot a known image
from its own flash. If both stored images are embedded, RX and TX can be
selected at boot or between transceiver sessions with the `FPGA_DEFAULT_IMAGE`
setting and the `FPGA,LOAD,*` commands.

Do not use RX/TX CRAM switching as the ordinary VNA mode switch. A VNA sweep
needs its stimulus, reference, receive channel, and measurement processing to
remain clock-coherent for the entire sweep. VNA support therefore needs one
persistent VNA-capable FPGA design, either as its own stored image selected
before the measurement or as part of an integrated design; reconfiguration is
only a between-session operation.

## Development bitstream update

Stop or close the SDR++/Quisk audio stream before updating. Then run:

```sh
python3 Software/ddc_sdr_firmware/tools/dfu_fpga.py /path/to/ddc_sdr.bin
```

The helper sends `DFU,PREPARE`, which stops I2S/DMA and releases runtime SPI,
then invokes:

```sh
dfu-util -d 1209:b1c0 -a 0 -D /path/to/ddc_sdr.bin
```

The SDK's DFU callback streams the raw packed bitstream into FPGA CRAM and
checks CDONE. The Pico remains running; after a successful transfer it
reinitializes runtime SPI, restores the selected rate, and restarts I2S. The
helper deliberately does not pass `-R`, because a reset would clear volatile
CRAM and would also discard a bitstream that was only loaded through DFU.

The SDK also prepares automatically when the first DFU block arrives, so
`--skip-prepare` is available for manual testing:

```sh
python3 Software/ddc_sdr_firmware/tools/dfu_fpga.py --skip-prepare /path/to/ddc_sdr.bin
```

## Raspberry Pi Pico W & OpenHPSDR Protocol 1 over Wi-Fi

When running on a **Raspberry Pi Pico W**, the firmware operates simultaneously over:
1. **Wi-Fi OpenHPSDR Protocol 1 (UDP Port 1024)**: Compatible with **SDR++**, **Quisk**, **PowerSDR**, **Thetis**, and **SparkSDR** using the Hermes board identity.
2. **Wi-Fi TCP Control Server (TCP Port 5000)**: Interactive command port for frequency/PGA control, Wi-Fi status, and debugging.
3. **USB Audio Class 1.0 (UAC1)**: 24-bit stereo I/Q streaming over USB.
4. **USB CDC Serial Port (`/dev/ttyACM0`)**: Full interactive command prompt.

### Building for Pico W

Run the dedicated build script:

```bash
cd Software/ddc_sdr_firmware
bash build_picow.sh
```

This compiles the firmware and automatically embeds the Lab 09 DDC FPGA bitstream (`ddc_sdr_top.bin`). The resulting file is generated at:
```text
Software/ddc_sdr_firmware/build-picow/ddc_sdr.uf2
```

### Flashing

1. Put the Raspberry Pi Pico W in BOOTSEL mode (hold BOOTSEL button while plugging into USB).
2. Copy `build-picow/ddc_sdr.uf2` to the `RPI-RP2` drive:
   ```bash
   cp Software/ddc_sdr_firmware/build-picow/ddc_sdr.uf2 /media/$USER/RPI-RP2/
   ```
3. The board will reboot, automatically boot the FPGA CRAM from the embedded bitstream, connect to Wi-Fi, and turn on the onboard Wi-Fi LED.

### Wi-Fi Configuration

The firmware is pre-configured to connect to:
- Primary AP: `Frohro-2.4GHz` (bench)
- Fallback AP: `Frohro-Shop-2.4GHz` (shop with antenna)
- Default IP: `192.168.1.186` (via DHCP or static fallback)

To query or dynamically connect to a different AP over USB CDC or TCP port 5000:
```text
WIFI?
WIFI,MyNetworkSSID,MyPassword
```

### Using with SDR Software

#### 1. SDR++ (OpenHPSDR Source)
* **Source**: Choose **OpenHPSDR** (or Metis / Hermes).
* **Sample Rate**: `48000` (or `96000`).
* Press **Play (▶)** and tune across 0 – 30 MHz.

#### 2. Quisk (OpenHPSDR Wi-Fi)
Launch Quisk with the provided configuration:
```bash
quisk -c Software/ddc_sdr_firmware/quisk_conf_openhpsdr.py
```

#### 3. Command-Line Test & Validation
To test discovery, TCP control, and verify active UDP packet streaming:
```bash
python3 Software/ddc_sdr_firmware/test_picow_hpsdr.py --ip 192.168.1.186
```

## Current gateware dependency

This directory defines the Pico-side contract but does not invent the DDC HDL
or its `.pcf`. Add the FPGA top level and Dev-iCE constraints separately, then
synthesize with `yosys`, place and route with `nextpnr-ice40`, pack with
`icepack`, and pass the resulting `.bin` to the build or DFU helper.
