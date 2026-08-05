

# Lab 6: The Numerically Controlled Oscillator (NCO)

**Objective:** This week, you leave State Machines behind and enter the world of **Datapath Logic**. You will design a 32-bit Numerically Controlled Oscillator (NCO) using a Phase Accumulator and a Sine/Cosine Look-Up Table (LUT). You will use Python to generate your mathematical golden vectors, use AI to write your registered datapath, and finally route your digital sine wave physically out to the MS9708 DAC to create a real RF signal generator.

## Part 1: The DSP Math (The Phase Accumulator)
An NCO generates frequencies purely through math. It consists of a 32-bit register (the Phase Accumulator) that simply adds a Tuning Word to itself on every single clock tick.

*   **The Clock:** Our master clock is $F_{clk} = 30.720 \text{ MHz}$.
*   **The Register:** A 32-bit register wraps around at $2^{32} = 4,294,967,296$.
*   **The Tuning Word (FCW):** The formula to calculate the Frequency Control Word (Tuning Word) for a desired output frequency ($F_{out}$) is:
    $$FCW = \left( \frac{F_{out}}{F_{clk}} \right) \times 2^{32}$$

*Example:* To approximate 1.000 MHz, the Pico calculates `round((1,000,000 / 30,720,000) * 2^32) = 139,810,133`. It sends `0x08555555` over the SPI bus to the FPGA. The realized frequency is approximately `999999.9976 Hz` because the 32-bit tuning word is quantized.

## Part 2: Python LUT & Golden Vectors
Before writing Verilog, you must know what a sine wave looks like in binary. 
The NCO takes the top 8 bits of the 32-bit accumulator (bits `[31:24]`) and uses them as an "address" to look up the amplitude of a sine and cosine wave in a Read-Only Memory (ROM) table. An 8-bit address means your table needs $2^8 = 256$ entries.

**Your Task:** Write a Python script (`nco_model.py`) that does two things:
1.  **Generate the LUT:** Calculate 256 samples of one full cycle of a Sine wave and a Cosine wave. Scale the amplitudes to fit inside an **8-bit signed two's complement** integer (-127 to +127). Have Python print out the SystemVerilog array initialization code (or a `.hex` file for `$readmemh`).
2.  **Generate the Golden Vectors:** Simulate the 32-bit accumulator in Python for a 1 MHz tuning word over 50 clock cycles. Print the exact Sine and Cosine integer values you expect to see on each clock tick. 

---

## Part 3: The AI Prompt (The Blueprint)

Datapath logic has **NO STATES**. Data must flow continuously on every positive clock edge. Use this template to prompt your AI:

> **SystemVerilog Pipelined NCO Request**
> Act as a Senior DSP Engineer. Write a SystemVerilog Numerically Controlled Oscillator (NCO). Do not use an FSM. The module must be fully pipelined.
> 
> **Inputs:** 
> *   `clk_30m`, `reset_n`
> *   `tuning_word[31:0]` (From the SPI Parser)
> 
> **Outputs:** 
> *   `sine_out[7:0]` (Signed 8-bit)
> *   `cosine_out[7:0]` (Signed 8-bit)
> 
> **Pipeline Stage 1 (Phase Accumulator):**
> *   Create a 32-bit register `phase_acc`. On every clock edge, add the `tuning_word` to `phase_acc`. Allow it to naturally wrap around on overflow.
> 
> **Pipeline Stage 2 (Look-Up Table):**
> *   Create a 256-entry, 8-bit signed ROM for Sine, and another for Cosine. 
> *   Use the top 8 bits of `phase_acc` (`phase_acc[31:24]`) as the address to index into both ROMs. 
> *   Register the outputs of the ROMs into `sine_out` and `cosine_out` to complete the pipeline. 
> 
> *(Note to AI: Leave the ROM initialization arrays blank or filled with zeros; I will paste in my own Python-generated sine/cosine tables).*

---

## Part 4: The Self-Checking Testbench
DSP math is extremely difficult to verify just by staring at a terminal output. 

Write a self-checking testbench (`nco_tb.sv`) that:
1.  Instantiates your NCO and drives the 30.72 MHz clock.
2.  Applies the 1.0 MHz tuning word you used in your Python script.
3.  Simulates 100 clock cycles.
4.  Compares the registered `sine_out` and `cosine_out` values against the Python-generated golden vectors in `nco_golden_1mhz.txt`.
5.  **The Proof:** Plot the captured or golden data in Python, Excel, or MATLAB. The quantized outputs should show two 90-degree offset sine waves.
6.  **Inject a Bug:** Change the LUT address slice from `[31:24]` to `[23:16]` in the RTL. Plot the result and observe how truncation ruins the frequency. 

---

## Part 5: Hardware Bring-Up & The "Signed to Unsigned" Trick

Once your simulation plot looks beautiful, we will route the NCO physically out of the FPGA to the MS9708 DAC.

**The Hardware Trap:**
Your NCO outputs **Signed** data (-127 to +127). 
The physical DAC expects **Unsigned** data (0 to 255), where `128` represents the dead-center 0-Volt baseline. If you send signed data straight to the DAC, the negative numbers will wrap around to maximum brightness, creating horrific distortion!

**The "MSB Inversion" Trick:**
To convert signed two's complement into an unsigned offset binary (which the DAC requires), you simply invert the Most Significant Bit (MSB)!
In your top-level Verilog file, assign the DAC pins like this:
`assign DAC_D[7:0] = { ~sine_out[7], sine_out[6:0] };`

**The Hardware Demo:**
1.  Wire the output of Lab 4 (SPI `cmd_freq_val`) to the `tuning_word` input of your NCO. In this Lab 06 wrapper, that value is the 32-bit FCW; the Pico firmware converts the host's frequency in Hz before sending it.
2.  Route the NCO to the DAC pins. 
3.  Ensure the DAC Clock is being output through an iCE40 `SB_IO` DDR output configured with constant zero/one data. This produces a full-rate 30.720 MHz clock from the existing master clock; `SB_ODDR` is not an available primitive in the installed iCE40 library.
4.  Synthesize and flash your bitstream.
5.  Hook an oscilloscope to the **`AWG_OUT` (DC-Coupled)** SMA port. 
6.  Use the Pico's serial terminal to send `FREQ,1000000`. The firmware converts the request to `0x08555555`, and the scope should show the resulting approximately 1 MHz sine wave.

---

## 📝 Deliverables & Oral Defense
At your weekly 15-minute team meeting, you must provide:
1. **The Blueprint:** Your pipeline diagram showing exactly what happens on Clock Edge 1, Edge 2, etc. 
2. **The Python Script:** Show the code you used to generate the Sine/Cosine ROMs.
3. **The Simulation Proof:** Present the plotted graph of your simulated sine and cosine waves. Show what happened to the graph when you injected the truncation bug. 
4. **The AI Critique:** How did the AI handle the ROM instantiation? Did it try to use an asynchronous read (combinational logic), or did it correctly register the ROM output? Why is a synchronous ROM required for high-speed DSP?
5. **The Hardware Demo:** Send a frequency command from the Pico and show the physical sine wave changing on the lab oscilloscope. Explain to the instructor why the MSB had to be inverted to drive the DAC.