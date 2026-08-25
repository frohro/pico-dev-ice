
# Lab 7: The Digital Mixer (Quadrature Demodulation)

**Objective:** This week, you will build the mathematical heart of the radio. You will take the raw RF signals from the ADC and multiply them by the Sine and Cosine waves from your NCO. This process (Heterodyning) shifts the high-frequency radio station down to 0 Hz (Baseband). You will learn how to handle bit-growth, Signed vs. Unsigned arithmetic, and pipelined multiplication.

## Part 1: The DSP Math & The "Signed" Trap
To tune into a radio station, we multiply the incoming antenna signal by the NCO signal. 

*   **The NCO:** Your NCO from Lab 6 outputs an 8-bit **Signed Two's Complement** number (ranging from -127 to +127). 
*   **The ADC:** The MS9280 ADC outputs an 8-bit **Unsigned Offset Binary** number (ranging from 0 to 255), where 128 represents 0 Volts.

**THE FATAL TRAP:** In SystemVerilog, if you multiply an Unsigned number by a Signed number, the synthesizer will usually default to Unsigned math, completely destroying your negative NCO values and ruining the radio signal!

**The Fix (MSB Inversion):**
Before you multiply them, you must convert the ADC data into a Signed Two's Complement number. Just like we inverted the MSB to send signed data to the DAC last week, you must invert the MSB of the ADC data as it comes *in*.
*   `adc_signed[7:0] = { ~adc_raw[7], adc_raw[6:0] };`
*   This instantly converts the ADC's 0 to 255 range into a -128 to +127 signed range!

**Bit Growth:**
When you multiply an 8-bit number by an 8-bit number, the result requires **16 bits** to store without overflowing. Your mixer outputs will be 16-bit signed integers. 

## Part 2: Python Golden Vectors
To verify your mixer, you need to know what a mixed signal looks like. 

According to trigonometric identities: $\sin(A) \times \sin(B) = \frac{1}{2} \cos(A - B) - \frac{1}{2} \cos(A + B)$. 
If you mix a 7 MHz antenna signal with a 7 MHz NCO, you get a Difference frequency (0 Hz / DC Baseband) and a Sum frequency (14 MHz). 

**Your Task:** Write a Python script (`mixer_model.py`) that:
1. Generates an array of an 8-bit 7 MHz sine wave (representing the ADC).
2. Generates an array of an 8-bit 7 MHz sine wave (representing the NCO).
3. Multiplies them together. 
4. Plots the output. You should clearly see a high-frequency 14 MHz wave riding on top of a shifted DC baseline!
5. Print the first 20 expected 16-bit integer results and write a complete golden-vector file for your testbench.


## Part 3: The AI Prompt (The Blueprint)

This is a pure Datapath module. Data flows continuously on every clock edge. Use this template:

> **SystemVerilog Digital Mixer Request**
> Act as a Senior DSP Engineer. Write a SystemVerilog Quadrature Mixer. Do not use an FSM. The module must be fully pipelined.
> 
> **Inputs:** 
> *   `clk_30m`, `reset_n`
> *   `adc_data[7:0]` (Unsigned 8-bit)
> *   `nco_sin[7:0]` (Signed 8-bit)
> *   `nco_cos[7:0]` (Signed 8-bit)
> 
> **Outputs:** 
> *   `mixer_i[15:0]` (Signed 16-bit)
> *   `mixer_q[15:0]` (Signed 16-bit)
> 
> **Pipeline Stage 1 (Registration & Conversion):**
> *   On the clock edge, register all inputs. 
> *   Convert `adc_data` to a signed 8-bit number by inverting its MSB. Store this in a signed register `adc_signed`.
> 
> **Pipeline Stage 2 (Multiplication):**
> *   Multiply `adc_signed` by `nco_sin`. Store in a signed 16-bit register `mix_i_reg`.
> *   Multiply `adc_signed` by `nco_cos`. Store in a signed 16-bit register `mix_q_reg`.
> *   *Ensure you use the `$signed()` system task to force signed arithmetic!*
> 
> **Pipeline Stage 3 (Output):**
> *   Register `mix_i_reg` and `mix_q_reg` to the outputs `mixer_i` and `mixer_q`.


## Part 4: The Self-Checking Testbench

Your testbench (`mixer_tb.sv`) must prove that the AI successfully commanded the synthesizer to use Signed multipliers.

1.  **Feed the Data:** Create arrays in your testbench loaded with the 7 MHz fake ADC data and the 7 MHz NCO data from your Python script.
2.  **The Self-Check:** On every clock cycle, feed the values into the Mixer, wait for the pipeline delay, and compare the 16-bit outputs against your Python golden vectors.
3.  **The Edge Case:** Manually inject the maximum possible negative numbers (`-128` from ADC $\times$ `-128` from NCO). Ensure the output successfully equals `+16384` without overflowing or losing its sign bit!
4.  **Inject a Bug:** Purposely remove the `$signed()` cast or the MSB inversion in the AI's RTL. Run the testbench and observe how the multiplication results turn into complete garbage.


## Part 5: Hardware Payoff & Plotting

Because the Mixer outputs 16-bit data at 30.72 MHz, we cannot easily send this directly to the Pico yet (that requires the CIC Decimation filter next week). 

For this week's hardware proof, your simulation is your primary deliverable. 
1.  Use `$fdisplay` in your testbench to save 500 clock cycles of `mixer_i` and `mixer_q` to `mixer_capture.txt`.
2.  Open that text file, `mixer_plot.csv`, or the generated `mixer_plot.png` in Python or Excel and plot it.
3.  For equal-frequency sine inputs, `mixer_i` contains the DC difference component plus the 14 MHz sum component. `mixer_q` contains the corresponding 14 MHz product component.

> [!TIP]
> **Real-World SDR Insight: ADC Resting DC Offset & NCO Spur Cancellation**
> In real hardware, physical ADC comparator offsets and op-amp bias voltages cause the ADC's resting code at idle to sit slightly off mid-scale (e.g. Code `129` $\implies +1\text{ LSB}$ DC bias).
> Multiplying a static $+1$ DC offset by the NCO directly feeds the raw NCO waveform and its discrete phase-truncation spurs into the baseband channel ($0\text{ Hz}$ LO feedthrough).
> In production DDC SDRs, this is eliminated by adding a digital **DC auto-zero tracking loop** (a simple 24-bit leaky integrator with $f_c \approx 1\text{ kHz}$) before the multiplier:
> $$\text{dc\_acc} \leftarrow \text{dc\_acc} + \text{adc\_signed} - (\text{dc\_acc} \gg 12)$$
> $$\text{adc\_clean} = \text{adc\_signed} - \text{dc\_acc}[23:12]$$
> This wipes out static DC offset down to $0.000$ without attenuating high-frequency RF signals ($> 500\text{ kHz}$).


## 📝 Deliverables & Oral Defense
At your weekly 15-minute team meeting, you must provide:
1. **The Blueprint:** Your 3-stage pipeline diagram.
2. **The Python Script:** Show your Python plot comparing the input waves to the mixed wave. 
3. **The Simulation Proof:** Demonstrate your testbench safely handling the `-128 * -128` edge case. Show the testbench failing when the MSB inversion is removed.
4. **The AI Critique:** Did the AI understand how to use `$signed()`? Did it try to infer DSP blocks (like MACs), or did it just use the `*` operator? 
5. **The Defense:** Explain to the instructor why $\sin(A) \times \sin(B)$ results in two new frequencies, and point to them on your simulation graph.