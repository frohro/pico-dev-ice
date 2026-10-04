# DDC FPGA handoff

The [`ddc_sdr.pcf`](ddc_sdr.pcf) file is the Dev-iCE SG48 package map for the
DDC top level. It intentionally contains signal names rather than a generated
bitstream; the HDL team can choose the internal module structure while keeping
the board contract stable.

The minimum first-pass top level must:

- sample `adc_data[7:0]` with `adc_clk` from the 30.720 MHz `clk` domain;
- provide the runtime SPI slave on `spi_*` while `spi_cs` is asserted;
- emit standard I2S on `i2s_rx_data`, `i2s_bck`, and `i2s_ws` to Pico GPIOs
  14, 15, and 16;
- accept the versioned `D5 01 command 04 value` frames described by the
  parent README;
- expose a deterministic 48 kHz path first, then a tested 96 kHz path.

The 96 kHz path needs explicit clocking design. A 30.720 MHz source provides
320 master-clock cycles per 96 kHz frame, but a 64fs I2S bit clock would be
6.144 MHz. Do not implement that mode using an integer divide of the raw
30.720 MHz clock; use a suitable PLL/derived clock or a formally verified
fractional edge scheduler and validate BCK/WS on hardware.

Example build flow:

```sh
yosys -p 'synth_ice40 -top ddc_sdr -json ddc_sdr.json' *.v
nextpnr-ice40 --up5k --package sg48 --freq 30.72 \
    --top ddc_sdr --pcf fpga/ddc_sdr.pcf \
    --json ddc_sdr.json --asc ddc_sdr.asc
icepack ddc_sdr.asc ddc_sdr.bin
```