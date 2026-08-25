
# Lab 9: System Integration & Top-Level Verification

**Objective:** You have successfully built and verified every individual component of a Digital Down-Converter (DDC) radio. This week, you will act as a Systems Integrator. You will write a Top-Level wrapper to wire all your modules together: `ADC ➔ Mixer ➔ CIC ➔ I2S`. You will write a massive, full-system testbench to prove the entire radio works mathematically, and then flash the complete design to your Pico Dev-iCE board.

## Part 1: The System Architecture (Structural Verilog)
In this lab, you are not writing new math; you are wiring boxes together. This is called Structural Verilog.

Before writing code, map out the exact data flow:
1.  **Control:** The `spi_cmd_parser` receives commands from the Pico. Its `cmd_freq_val` output is the 32-bit FCW consumed by the NCO, and `cmd_clear_otr` goes to the ADC capture module.
2.  **Data Intake:** The `ADC_Capture` module reads the 8-bit MS9280 data and the OTR pin. It outputs safe, registered 8-bit data and the `fpga_int` signal.
3.  **Local Oscillator:** The `NCO` uses the `tuning_word` to generate 8-bit Signed Sine and Cosine waves.
4.  **Demodulation:** The `mixer` multiplies the 8-bit ADC data by the NCO Sine (I) and Cosine (Q), outputting two 16-bit signed channels. The checked-in mixer performs the MS9280 offset-binary MSB inversion internally; do not invert `adc_data` in the top level.
5.  **Decimation:** You must instantiate **TWO** `CIC_Decimator` modules—one for the I channel, and one for the Q channel. They take the 16-bit 30.72 MSPS data and output 24-bit 48 kSPS data.
6.  **Output:** The `i2s_transmitter` takes the 24-bit I and Q data from the CIC filters and serializes it to the Pico when both CIC `valid_out` strobes agree.

## Part 2: The AI Prompt (The Blueprint)
AI is fantastic at typing out tedious structural port-maps, provided you give it the exact module names and wire names.

> **SystemVerilog Top-Level Integration Request**
> Act as a Senior Systems Integrator. Write a SystemVerilog top-level module named `ddc_sdr_top`. 
> 
> **External Ports:**
> *   Match the ordinary top-level pin names defined in `ENGR433-Solutions/Lab_09/lab09.pcf`: `clk`, `adc_clk`, `dac_clk`, `adc_data`, `adc_otr`, `dac_data`, `spi_miso`, `spi_mosi`, `spi_sck`, `spi_cs`, `i2s_bck`, `i2s_ws`, `i2s_rx_data`, `i2s_tx_data`, `fpga_int`, `pmod`, `led_red`, `led_yellow`, `led_green`.
> 
> **Hardware Quirks & Output Indicators:**
> *   **RGB LEDs (Pins 39, 40, 41):** Dedicated open-drain hard-IP drivers. They are **active-low** (`0 = LED ON`, `1 = LED OFF`).
> *   **Pulse Stretching:** Single-cycle events (such as SPI commands or 32ns ADC OTR spikes) are invisible to human eyes. Implement a ~24-bit down-counter (~550 ms hold) so:
>     *   `led_green`: Steady ON (`0`) when un-reset.
>     *   `led_yellow`: Pulses ON for ~550 ms whenever a frequency or rate SPI command arrives.
>     *   `led_red`: Pulses ON for ~550 ms whenever the ADC asserts Out-Of-Range clipping (`adc_otr`).
> 
> **Internal Wires:**
> *   Declare all necessary internal wires to connect the sub-modules (e.g., `wire [31:0] cmd_freq_val;`, `wire signed [23:0] cic_i, cic_q;`).
> 
> **Instantiations:**
> *   Instantiate the following modules and connect their ports using named port mapping (`.port(wire)`):
>     1. `spi_cmd_parser`
>     2. `adc_capture_otr`
>     3. `nco`
>     4. `mixer` (Pass the raw offset-binary ADC bus; the checked-in mixer performs the one required MSB inversion.)
>     5. `cic_decimator` (one instance for I and one for Q)
>     6. `i2s_transmitter`
> 
> *(Note to AI: Do not rewrite the sub-modules. Assume they already exist in my workspace. Only write the top-level structural wrapper).*

---

## Part 3: The Full-System Testbench
This is the ultimate verification environment (`ddc_top_tb.v`). If this testbench passes, your radio works.

1.  **The Simulated ADC:** Drive a deterministic offset-binary ADC value so the mixer and CIC steady-state result can be checked exactly.
2.  **The Simulated Pico (SPI):** Send valid and invalid versioned frames, abort a frame with reset, clear OTR, and update the FCW near a CIC valid boundary.
3.  **The Simulated Pico (I2S):** Monitor `i2s_rx_data` on each BCK rising edge, shifting 32-bit slots exactly as the checked-in Pico receiver contract requires.
4.  **The Proof:** Wait for the CIC pipeline, assert that I and Q valid strobes are simultaneous every 640 master-clock cycles, and verify the steady-state 24-bit I2S samples and zero padding.
5.  **Inject a Bug:** Compile with `LAB09_SWAP_IQ` to swap the I and Q wires. Prove the testbench reports channel-value failures and exits nonzero.

---

## Part 4: Synthesis & Hardware Smoke Test
It is time to put your radio onto the actual silicon.

1.  **Physical Pin Constraints (`lab09.pcf`):**
    *   `clk 37` (30.720 MHz Master Oscillator)
    *   `i2s_bck 12` $\rightarrow$ Pico GPIO 15 (3.072 MHz Bit Clock)
    *   `i2s_ws 9` $\rightarrow$ Pico GPIO 16 (48 kHz LRCLK / Word Select)
    *   `i2s_rx_data 11` $\rightarrow$ Pico GPIO 14 (Baseband I/Q data to Pico)
    *   `i2s_tx_data 10` $\rightarrow$ Pico GPIO 13 (Audio from Pico)
    *   `led_red 39`, `led_yellow 40`, `led_green 41` (Active-Low RGB LEDs)
2.  **Firmware Front-End Note (`REF` pin):**
    *   The board's front-end RF multiplexer is controlled by Pico **GPIO 26 (`REF`)**. The Pico firmware must drive `GPIO 26` **LOW (`0`)** to connect the antenna SMA to the ADC. (Driving it HIGH connects the VNA test path instead).
3.  Synthesize the design using Yosys/NextPNR. Check the logs for **no latch warnings** and verify timing closure ($F_{\text{MAX}} \ge 30.72\text{ MHz}$).
4.  Flash your compiled `ddc_sdr.bin` bitstream into the FPGA.
5.  **The Smoke Test:** 
    *   **Visual LEDs:** Green LED should illuminate. Yellow LED should pulse whenever you change frequency in Quisk / SDR++ (or via CDC serial command `FREQ,7074000`).
    *   **I2S Clocks:** Hook an oscilloscope to Pin 12 (`BCK`) and Pin 9 (`WS`). Are they running cleanly at 3.072 MHz and 48 kHz?
    *   **Audio Spectrum:** Open Quisk or SDR++. You should see live RF spectrum and waterfall from the antenna!

---

## 📝 Deliverables & Oral Defense
At your weekly 15-minute team meeting, you must provide:
1. **The Blueprint:** A complete schematic of your internal FPGA routing, showing exactly how the data bit-widths change from 8-bit (ADC) to 16-bit (Mixer) to 44-bit (CIC internal) to 24-bit (I2S).
2. **The Synthesis Log:** Show the instructor your final Yosys resource utilization. How many Logic Cells (LUTs) and DSP Blocks did your complete radio use? 
3. **The Simulation Proof:** Demonstrate your massive top-level testbench. Show the instructor the exact moment the SPI command is received, and trace that resulting frequency change all the way through the NCO and out the I2S port in GTKWave.
4. **The AI Critique:** Integration is where AI usually fails. Did the AI mess up the port widths? Did it forget to connect the `valid_out` strobe from the CIC filter to the I2S transmitter? How did you fix the wiring? 
5. **The Hardware Demo:** Show the physical I2S clocks running on the board using a lab instrument.