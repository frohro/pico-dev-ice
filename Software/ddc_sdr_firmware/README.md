# Dev-iCE DDC SDR firmware

This application is the Dev-iCE successor to the earlier Si5351a/Tayloe/
PCM1808 implementation. The FPGA replaces the Si5351a LO, Tayloe detector, and PCM1808 audio ADC path. It
receives the FPGA's I/Q stream as PCM1808-compatible I2S and exposes the same
UAC1 stereo 24-bit capture format to SDR++ or Quisk.

## Dev-iCE Pico pins

| Signal | GPIO | Direction |
| --- | ---: | --- |
| FPGA SPI0 MISO | 4 | Pico input |
| FPGA SPI0 CS | 5 | Pico output |
| FPGA SPI0 SCK | 6 | Pico output |
| FPGA SPI0 MOSI | 7 | Pico output |
| FPGA I2S RX_DATA | 14 | Pico input |
| FPGA I2S BCK | 15 | Pico input |
| FPGA I2S WS | 16 | Pico input |
| FPGA interrupt | 0 | Pico input, active-high OTR notification |
| PGA control mask | 8..11 | Pico outputs, PGA0..PGA3 |
| FPGA CDONE | 21 | Pico input |
| FPGA CRESET | 22 | Pico output |
| 30.720 MHz clock | FPGA pin 37 | External oscillator |

The FPGA is the I2S master. The initial firmware supports 48 kHz and 96 kHz
UAC1 alternatives, with standard I2S, two channels, 24 valid bits, and
little-endian three-byte USB samples. The FPGA gateware must implement the
corresponding clocking and decimation modes.

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

The FPGA should latch an OTR event, hold `fpga_int` high, and keep it high
until it receives the `DDC_FPGA_CMD_CLEAR_OTR` command. Repeated OTR events
may be coalesced while the interrupt is high; a future status response can
add an event counter without changing this interrupt contract.

The checked-in AGC implementation is in `agc_control.h` and `main.c`. The
interrupt callback only records a pending event. The foreground loop advances
the PGA state, sends the exact clear frame `D5 01 04 04 01 00 00 00`, and uses
a nonblocking 2-second timestamp for decay. Runtime FPGA SPI is configured to
10 MHz after the shared SDK initializes the peripheral.

The protocol is intentionally small and versioned so it can be implemented in
the FPGA alongside the first DDC datapath. The FPGA should latch a complete
frame only while CS is asserted and ignore malformed lengths or versions.

CDC commands are:

```text
FREQ,<hz>
RATE,<hz>
DFU,PREPARE
DFU,CANCEL
DFU,STATUS
```

`FREQ,<hz>` replaces the old Si5351 parameter list. The Pico converts the
requested Hz value to an FCW using the 30.720 MHz FPGA clock before sending
command `01`. `XTAL` reports the FPGA clock reference as `30720000`, and
`MODE` reports `DDC` for host discovery.

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

## Current gateware dependency

This directory defines the Pico-side contract but does not invent the DDC HDL
or its `.pcf`. Add the FPGA top level and Dev-iCE constraints separately, then
synthesize with `yosys`, place and route with `nextpnr-ice40`, pack with
`icepack`, and pass the resulting `.bin` to the build or DFU helper.
