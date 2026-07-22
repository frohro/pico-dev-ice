
# Educational Mixed-Signal SDR & DSP Laboratory
**A unified DDC/DUC Software Defined Radio, Vector Network Analyzer, and Arbitrary Waveform Generator designed for university-level Digital Design and DSP classes.**

## 📌 Overview
This board is a low-cost, high-performance "Lab-in-a-Box." It combines the high-speed programmable logic of a **Lattice iCE40UP5K FPGA** with the flexible C/MicroPython ecosystem of a **Raspberry Pi Pico (RP2040/RP2350)**. 

Designed specifically as an educational platform, it abandons "black box" RF chips in favor of discrete, observable analog blocks. Students can physically probe the RF signal path, design their own LC filters on pluggable sandboxes, and write Verilog to perform Direct Digital Conversion (DDC), digital decimation, and hardware I2S audio routing.

## 🚀 Key Capabilities
* **HF Software Defined Radio (RX/TX):** Full-duplex DDC/DUC transceiver covering 0 - 15.36 MHz (1st Nyquist) with Super-Nyquist capabilities up to 30 MHz.
* **Vector Network Analyzer (VNA):** Built-in Return Loss Bridge (RLB) for S11 (reflection) antenna tuning, and Port-to-Port S21 (transmission) filter characterization.
* **Arbitrary Waveform Generator (AWG):** Dedicated DC-coupled and AC-coupled SMA outputs for 30 MSPS generic signal generation.
* **Real-Time Spectrum Analyzer:** Visualize the entire 15 MHz HF spectrum simultaneously via FPGA-accelerated FFTs.
* **Standalone Operation:** Onboard Audio DAC/Headphone amp and rotary encoder support allows the board to operate as a standalone radio without a PC.

---

## 🧠 Hardware Architecture

### Processing Core
* **Microcontroller:** Raspberry Pi Pico / Pico 2 / YD-RP2040 socket. Handles AGC math, user interface, USB communications, and slow-state hardware toggles.
* **FPGA:** Lattice iCE40UP5K (SG48). Handles high-speed 30.72 MHz DSP, NCO generation, CIC decimation/interpolation, and I2S master clocking.
* **Control Bus:** The Pico uses its high-speed `SPI0` bus to dual-role: it flashes the FPGA bitstream on boot, and then seamlessly transitions to sending runtime DSP/Gain commands via a dedicated `RUNTIME_CS` pin.

### Clocking & DSP Math
* **Master Clock:** A clean 30.720 MHz CMOS oscillator drives the FPGA.
* **The Magic of 30.72 MHz:** The FPGA decimates the 30.72 MSPS ADC data by exactly $R=640$ to yield a mathematically perfect **48.000 kHz** baseband I/Q audio stream over I2S to the Pico. 
* **Process Gain:** The 640x decimation provides ~ 28 dB of digital processing gain, turning the raw 8-bit ADC into a highly sensitive 12.6-bit effective receiver!

### The Receive (RX) Path
* **Topology:** `SMA` ➔ `Ethernet Isolation/CMC` ➔ `Filter Sandbox` ➔ `T/R Switch` ➔ `LNA 1` ➔ `PGA (5/10/20dB)` ➔ `LNA 2` ➔ `ADC`.
* **Amplifiers:** Uses discrete **TPH2501** high-speed op-amps wired as AC-coupled inverting amplifiers. 
* **Programmable Gain:** A 3-bit Digital Step Attenuator (DSA) built from **74LVC1G3157** CMOS switches and precision T-networks, providing 0 to 55 dB of gain control in 5 dB steps.
* **ADC (MS9280):** 8-bit, 32 MSPS. Driven by an RF balun into True Differential Mode with a 4.5V analog supply, yielding a massive 4.0V peak-to-peak input span for maximum dynamic range.

### The Transmit (TX) Path
* **Topology:** `DAC` ➔ `Passive I-V` ➔ `Ethernet Transformer` ➔ `TX Switch` ➔ `Recon Filter Sandbox` ➔ `TX Op-Amp (+12dB)` ➔ `T/R Switch` ➔ `SMA`.
* **DAC (MS9708):** 8-bit, 32 MSPS Current-Steering DAC.
* Passive 50Ω resistors and an Ethernet transformer gracefully map the DAC's strict output compliance limits into a clean 1.0V p-p RF signal, which is then amplified to drive a 50-ohm antenna.

### Power Domains (Strict Isolation)
To achieve commercial-grade noise floors, the noisy Switch-Mode Power Supply (SMPS) of standard Picos is bypassed/isolated. 
* **`+4.5V AVDD`:** Clean LDO powering the RF op-amps, analog switches, and ADC/DAC analog cores.
* **`+3.3V Digital`:** LDO powering the FPGA I/O, Pico Flash, and IC logic.
* **`+3.3V Clean`:** Ferrite bead + LC filtered rail strictly for the 30.72 MHz oscillator to eliminate phase-noise/jitter.

---

## 🎛️ Port & Jumper Reference

### SMA Connectors
1. **`SDR_ANT`:** Main Transceiver Port (Transformer isolated, AC-Coupled).
2. **`DUT` (VNA_TEST):** Device Under Test port for S11/VSWR Antenna measurements. 
3. **`AWG_OUT_DC`:** Direct DC-coupled output (0V to +4V) from the DAC.
4. **`AWG_OUT_AC`:** AC-coupled output for S21 Filter sweeps and RF signal generation.

### Filter Sandboxes (Plug-and-Play)
The board features `IN-GND-GND-OUT` 0.1" sockets for inserting custom filter daughterboards. 
* **Main Sandbox:** Shared by both RX and TX (post-T/R switch). Students calculate and test bandpass filters here.
* **TX Pre-Sandbox:** Placed before the TX amplifier. Used for mild Nyquist reconstruction filters or super-Nyquist image-selection filters.

---

### 🛠️ Software & HDL Notes
* **Level Shifters:** The 4.5V analog switches are controlled by 3.3V GPIOs via N-Channel MOSFETs. **Note:** This results in a logic inversion. Writing `0` to the pin in MicroPython = Switch HIGH (Max Gain / Default Path). Writing `1` = Switch LOW (Attenuated / Alt Path).
* **FPGA SPI Roles (The CRAM Paradox):** 
    *   **During Boot:** The `pico-ice` SDK uses software bit-banging/PIO to program the FPGA's CRAM, sending the bitstream into the FPGA's `ICE_SI` pin (as required by Lattice hardware).
    *   **During Runtime:** To communicate at high speeds, the Pico relies on its internal Hardware `SPI0` block. Due to how the SPI Flash chip is wired, the Pico's Hardware `TX` pin is permanently bound to the **`ICE_SO`** net, and its `RX` pin is bound to the **`ICE_SI`** net.
    *   **Verilog Configuration:** Because the Pico is the master transmitting on `ICE_SO`, your Verilog `SPI_Slave` module must configure the `ICE_SO` pin as **MOSI (Input)**, and the `ICE_SI` pin as **MISO (Output)**.
* **ADC OTR:** The MS9280 "Out of Range" (Clipping) pin pulses for only 32ns. The FPGA catches this, stretches the pulse, and triggers a hardware interrupt on the Pico to engage the AGC.

---

Here is the complete, master pinout table generated directly from your final SPICE netlist. 

This is formatted in Markdown so you can copy and paste it directly into your `README.md` file. I have separated it into two tables: **The Software Map (Pico)** and **The HDL Map (FPGA)**. This will make it incredibly easy for the software students to write their Python/C code, and the digital design students to write their `.pcf` constraint files without stepping on each other's toes!

***

## 📍 Hardware Pinout Reference

### 1. Microcontroller (Raspberry Pi Pico) Pinout
*This map defines the interface for the software team handling AGC, VNA sweeps, user interface, and I2S audio routing.* (I checked this.)

| Pico GPIO | Net Name | Function | Direction | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **GPIO 0** | `FPGA_INT` | FPGA Hardware Interrupt | Input | Catches stretched ADC Out-Of-Range (OTR) pulses |
| **GPIO 1** | `SPI_RUN_CS` | FPGA Runtime Chip Select | Output | Pull LOW to send runtime SPI commands to the FPGA |
| **GPIO 2** | `SDA` | I2C Data | In/Out | I2C bus for the OLED Display |
| **GPIO 3** | `SCL` | I2C Clock | Output | I2C bus for the OLED Display |
| **GPIO 4** | `ICE_SI` | SPI0 RX (MISO) | Input | Data entering Pico from FPGA/Flash |
| **GPIO 5** | `ICE_SSN` | SPI0 CSn (Flash CS) | Output | Used ONLY during boot to flash the bitstream |
| **GPIO 6** | `ICE_SCK` | SPI0 SCK | Output | SPI Clock for Boot and Runtime |
| **GPIO 7** | `ICE_SO` | SPI0 TX (MOSI) | Output | Data leaving Pico to FPGA/Flash |
| **GPIO 8** | `PGA0` | 5 dB Attenuator | Output | Logic 1 = Attenuator Active (LOW at switch) |
| **GPIO 9** | `PGA1` | 10 dB Attenuator | Output | Logic 1 = Attenuator Active (LOW at switch) |
| **GPIO 10** | `PGA2` | Removes 20 dB LNA2 | Output | Logic 1 = Attenuator Active (LOW at switch) |
| **GPIO 11** | `PGA3` | LNA 1 Bypass | Output | Logic 1 = LNA Bypassed (LOW at switch) |
| **GPIO 12** | `BAND_SW` | Band Filter Sandbox | Output | Toggles sandbox selection switch |
| **GPIO 13** | `TX_DATA` | I2S TX Data | Output | Full-Duplex Baseband Audio TO the FPGA |
| **GPIO 14** | `RX_DATA` | I2S RX Data | Input | Full-Duplex Baseband Audio FROM the FPGA |
| **GPIO 15** | `BCK` | I2S Bit Clock | Input | Clocks generated by FPGA (I2S Master) |
| **GPIO 16** | `WS` | I2S Word Select (L/R) | Input | Clocks generated by FPGA (I2S Master) |
| **GPIO 17** | `AF_DAC_DATA` | Audio DAC Data | Output | I2S out to standalone headphone amp |
| **GPIO 18** | `AF_DAC_MCLK` | Audio DAC Master | Output | I2S out to standalone headphone amp |
| **GPIO 19** | `AF_DAC_SCLK` | Audio DAC Clock | Output | I2S out to standalone headphone amp |
| **GPIO 20** | `AF_DAC_LRCK` | Audio DAC L/R | Output | I2S out to standalone headphone amp |
| **GPIO 21** | `ICE_DONE` | FPGA Boot Status | Input | Goes HIGH when FPGA is running (Pulled up) |
| **GPIO 22** | `~ICE_RST` | FPGA Reset | Output | Pull LOW to hold FPGA in reset |
| **GPIO 26** | `~T/R` | Transmit/Receive Sw | Output | Toggles main antenna between ADC and DAC |
| **GPIO 27** | `~REF` | VNA Reflection Sw | Output | Toggles paths to form the closed-loop VNA |


### 2. FPGA (Lattice iCE40UP5K-SG48) Pinout
*This map defines the interface for the digital design team writing Verilog and the `.pcf` constraints file.* (I only checked the first few.)

| Pin # | Lattice Name | Net Name | Function / Connection |
| :---: | :--- | :--- | :--- |
| **37** | `IOT_45a_G1` | `CLK` | **30.72 MHz Master Clock IN** (From Oscillator) |
| **35** | `IOT_46b_G0` | `ADC_CLK` | ADC Clock OUT (To MS9280) |
| **6** | `IOB_13b` | `DAC_CLK` | DAC Clock OUT (To MS9708) |
| **23** | `IOT_37a` | `D0` | ADC Data Bit 0 (LSB) |
| **25** | `IOT_36b` | `D1` | ADC Data Bit 1 |
| **26** | `IOT_39a` | `D2` | ADC Data Bit 2 |
| **27** | `IOT_38b` | `D3` | ADC Data Bit 3 |
| **28** | `IOT_41a` | `D4` | ADC Data Bit 4 |
| **31** | `IOT_42b` | `D5` | ADC Data Bit 5 |
| **32** | `IOT_43a` | `D6` | ADC Data Bit 6 |
| **34** | `IOT_44b` | `D7` | ADC Data Bit 7 (MSB) |
| **36** | `IOT_48b` | `OTR` | ADC Out of Range (Clipping Flag) |
| **44** | `IOB_3b_G6` | `DB0` | DAC Data Bit 0 (LSB) |
| **45** | `IOB_5b` | `DB1` | DAC Data Bit 1 |
| **46** | `IOB_0a` | `DB2` | DAC Data Bit 2 |
| **47** | `IOB_2a` | `DB3` | DAC Data Bit 3 |
| **48** | `IOB_4a` | `DB4` | DAC Data Bit 4 |
| **2** | `IOB_6a` | `DB5` | DAC Data Bit 5 |
| **3** | `IOB_9b` | `DB6` | DAC Data Bit 6 |
| **4** | `IOB_8a` | `DB7` | DAC Data Bit 7 (MSB) |
| **14** | `IOB_32a_SPI_SO` | `ICE_SO` | SPI MOSI (Data *from* Pico during runtime) |
| **17** | `IOB_33b_SPI_SI` | `ICE_SI` | SPI MISO (Data *to* Pico during runtime) |
| **15** | `IOB_34a_SPI_SCK`| `ICE_SCK` | SPI Clock IN |
| **18** | `IOB_31b` | `SPI_RUN_CS` | SPI Runtime Chip Select IN |
| **11** | `IOB_20a` | `BCK` | I2S Bit Clock OUT (To Pico) |
| **21** | `IOB_23b` | `WS` | I2S Word Select OUT (To Pico) |
| **10** | `IOB_18a` | `RX_DATA` | I2S Data OUT (SDR audio to Pico) |
| **9** | `IOB_16a` | `TX_DATA` | I2S Data IN (Transmit audio from Pico) |
| **43** | `IOT_49a` | `FPGA_INT` | Hardware Interrupt OUT (To Pico) |
| **19** | `IOB_29b` | `PMOD_0` | PMOD Header Pin 1 |
| **42** | `IOT_51a` | `PMOD_1` | PMOD Header Pin 2 |
| **38** | `IOT_50b` | `PMOD_2` | PMOD Header Pin 3 |
| **20** | `IOB_25b_G3` | `PMOD_3` | PMOD Header Pin 4 |
| **39** | `RGB0` | `RED` | Open-Drain LED Driver (Red) |
| **40** | `RGB1` | `YELLOW` | Open-Drain LED Driver (Yellow) |
| **41** | `RGB2` | `GREEN` | Open-Drain LED Driver (Green) |
| **16** | `IOB_35b_SPI_SS` | `ICE_SSN` | *Hardware Boot Only - Ignore in User Verilog* |
| **7** | `CDONE` | `ICE_DONE` | *Hardware Boot Only - Ignore in User Verilog* |
| **8** | `~CRESET` | `~ICE_RST` | *Hardware Boot Only - Ignore in User Verilog* |