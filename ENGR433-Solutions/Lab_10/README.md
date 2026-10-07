# Lab 10 Solution: Automatic Gain Control

This solution implements the Pico-side AGC contract for the verified FPGA OTR
interrupt. It uses the existing C firmware rather than a blocking script:

- `agc_control.h` contains the host-testable AGC state machine.
- `main.c` keeps the GPIO interrupt callback short by setting a pending flag.
- The foreground loop advances the PGA state and sends the clear-OTR command.
- A nonblocking millisecond timer decays gain after 2 seconds of quiet.
- Runtime FPGA SPI is explicitly configured to 10 MHz.

The only automatic PGA sequence is:

```text
0x0 -> 0x1 -> 0x3 -> 0xF
```

Each OTR event advances one state and sends exactly:

```text
D5 01 04 04 01 00 00 00
```

At `0xF`, further OTR events remain saturated. When `FPGA_INT` is low for
2,000 ms, the state steps backward through `0x3`, `0x1`, and `0x0`. The timer
is restarted by a new OTR event and never blocks USB, audio, or CDC handling.

## Host tests

Run the AGC state-machine test:

```sh
gcc -std=c11 -Wall -Wextra -Werror \
  -I Software/ddc_sdr_firmware \
  -o /tmp/test_agc_control \
  Software/ddc_sdr_firmware/tools/test_agc_control.c
/tmp/test_agc_control
```

Run the protocol tests, including the exact clear-OTR frame:

```sh
python3 -m unittest \
  Software/ddc_sdr_firmware/tools/test_ddc_protocol.py
```

## Pico firmware build

### Build the Lab 10 FPGA TX image

From the repository root, build the Philips-I2S TX bitstream with:

```sh
ENGR433-Solutions/Lab_10/build_tx.sh
```

The default output directory is `ENGR433-Solutions/Lab_10/build-tx/`. The
script produces `lab10_tx_top.asc`, `lab10_tx_top.bin`, and the nextpnr timing
report. The packed `.bin` can be embedded in the Pico firmware build.

The generated image uses the following contract:

- BCK: 3.072 MHz at 48 kHz, or 6.144 MHz at 96 kHz.
- WS: 48 kHz or 96 kHz, with 32-bit left and right slots.
- WS changes on the rising edge sampling the previous slot's final bit.
- Data changes on BCK falling edges and is sampled on rising edges.
- 24-bit I/Q samples occupy bits `[31:8]`; bits `[7:0]` are zero.

### Build the USB-working Pico firmware with the TX image

After building the FPGA image, embed it in the USB-working firmware:

```sh
FPGA_TX_BITSTREAM_BIN="$PWD/ENGR433-Solutions/Lab_10/build-tx/lab10_tx_top.bin" \
FPGA_DEFAULT_IMAGE=TX \
  Software/ddc_sdr_firmware_usb_working/build.sh
```

The resulting UF2 is
`Software/ddc_sdr_firmware_usb_working/build/ddc_sdr.uf2`. Flash this UF2 to
the Pico and use the firmware's stored-image or DFU workflow to load the
matching FPGA image.

The checked-in SDK build can be used without hardware:

```sh
cmake --build Software/ddc_sdr_firmware/build-current-validation -j2
```

Or configure a fresh build as described in
`Software/ddc_sdr_firmware/README.md`. The resulting firmware includes the
nonblocking AGC loop and the 10 MHz runtime SPI setting.

## Hardware acceptance

1. Load the Lab 09 FPGA bitstream and the rebuilt Pico firmware.
2. Confirm the initial PGA code is `0x0`.
3. Assert `FPGA_INT` and confirm the code advances to `0x1`, then the FPGA OTR
   latch clears through the 8-byte SPI command.
4. Repeat until `0xF`; confirm additional OTR events do not wrap the state.
5. Keep `FPGA_INT` low for at least 2 seconds and confirm one-step decay.
6. Confirm USB audio and CDC command handling continue during the decay wait.

Live RF reception is optional hardware evidence. The host AGC and protocol
regressions are the required reproducible proof when no board or antenna is
available.

## Persistent VNA prototype

The directory also contains a carrier-only VNA image. Unlike the transceiver
profiles, this image remains loaded for the complete measurement sweep:

- `vna_top.sv` generates a single DAC carrier from a shared phase accumulator;
- `vna_correlator.sv` multiplies the ADC stream by the same sine/cosine
  references and accumulates a 1024-sample complex snapshot;
- `vna_spi_slave.sv` controls frequency/acquisition and reads the snapshot;
- `vna.pcf` preserves the production SG48 pin contract; and
- `vna_top_tb.sv` verifies coherent carrier feedback and SPI readback.

Build and pack it from the Lab 10 directory:

```sh
cd ENGR433-Solutions/Lab_10
iverilog -g2012 -s vna_top_tb -o /tmp/vna_top_tb.vvp \
    vna_top.sv vna_nco.sv vna_correlator.sv vna_spi_slave.sv vna_top_tb.sv
vvp /tmp/vna_top_tb.vvp
yosys -p 'read_verilog -sv vna_top.sv vna_nco.sv vna_correlator.sv vna_spi_slave.sv; synth_ice40 -top vna_top -json vna_top.json'
nextpnr-ice40 --up5k --package sg48 --freq 30.72 --top vna_top \
    --pcf vna.pcf --json vna_top.json --asc vna_top.asc
icepack vna_top.asc vna_top.bin
```

The measured route closes at 39.56 MHz on the 30.72 MHz system clock, using
551 of 5280 logic cells and one 4 Kbit block RAM. The raw I and Q results are
signed 32-bit correlation sums; a host should divide by the window length and
form DUT/reference complex ratios for magnitude and phase.

The VNA SPI additions retain the existing 8-byte command frame:

| Command | Value | Meaning |
| ---: | ---: | --- |
| `01` | FCW | Set the carrier frequency control word. |
| `05` | bit 0 | Clear the previous result and start a 1024-sample window. |
| `06` | `0..3` | Select the following read register: I, Q, status, or FCW. |

After a read-select command, assert CS for a separate four-byte SPI read. The
FPGA shifts the selected register most-significant bit first. `FPGA_INT` stays
high when the window is complete and returns low on the next clear command.
The Pico's GPIO26 `REF` control remains an external analog-switch control; the
FPGA does not drive it. A sweep should acquire a reference window, switch the
analog path, acquire the DUT window, and compute the complex ratio without
reconfiguring the FPGA.
