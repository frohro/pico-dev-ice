# Dev-iCE DDC SDR firmware

This application is the Dev-iCE successor to `Software/2026_v0.2`. The FPGA
replaces the Si5351a LO, Tayloe detector, and PCM1808 audio ADC path. It
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
| FPGA interrupt | 0 | Pico input, reserved |
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
| `01` | Set center frequency in Hz |
| `02` | Set output sample rate, 48000 or 96000 |
| `03` | Reserved for status reads |

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

`FREQ,<hz>` replaces the old Si5351 parameter list. `XTAL` reports the FPGA
clock reference as `30720000`, and `MODE` reports `DDC` for host discovery.

## Build

Use the checked-in Pico SDK through the SDK port. Without a bitstream, the
firmware builds as a USB/DFU development image and waits for FPGA CRAM to be
loaded:

```sh
cmake -S Software/ddc_sdr -B Software/ddc_sdr/build \
    -DPICO_BOARD=pico_dev_ice -DPICO_NO_PICOTOOL=1
cmake --build Software/ddc_sdr/build -j2
```

For a production UF2 that configures the FPGA at every Pico boot, pass the
packed `icepack` output:

```sh
cmake -S Software/ddc_sdr -B Software/ddc_sdr/build-embedded \
    -DPICO_BOARD=pico_dev_ice -DPICO_NO_PICOTOOL=1 \
    -DFPGA_BITSTREAM_BIN=/path/to/ddc_sdr.bin
cmake --build Software/ddc_sdr/build-embedded -j2
```

The `.uf2` flashes the RP2040. Dev-iCE has no FPGA configuration flash, so
FPGA CRAM is volatile and must be loaded again after reset or power loss.

## Development bitstream update

Stop or close the SDR++/Quisk audio stream before updating. Then run:

```sh
python3 Software/ddc_sdr/tools/dfu_fpga.py /path/to/ddc_sdr.bin
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
python3 Software/ddc_sdr/tools/dfu_fpga.py --skip-prepare /path/to/ddc_sdr.bin
```

## Current gateware dependency

This directory defines the Pico-side contract but does not invent the DDC HDL
or its `.pcf`. Add the FPGA top level and Dev-iCE constraints separately, then
synthesize with `yosys`, place and route with `nextpnr-ice40`, pack with
`icepack`, and pass the resulting `.bin` to the build or DFU helper.
