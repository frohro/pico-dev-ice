Here is the complete **Lab 8 Document** for your students. 

This is the "Boss Fight" of the semester. Cascaded Integrator-Comb (CIC) filters are notoriously difficult for AI to generate correctly because AI inherently wants to "protect" math from overflowing, whereas a CIC filter *relies* on overflow to function! 

This lab forces the students to take absolute control of the bit-width math. You can save this as `Lab_08_CIC_Decimator.md` in your repository!

***

# Lab 8: The CIC Decimation Filter

**Objective:** This week is the mathematical peak of the course. You will build a Cascaded Integrator-Comb (CIC) filter. This module will take the 30.72 MSPS, 16-bit high-speed signals from your Mixer and "average" them down to a clean, highly sensitive 48 kSPS audio stream. You will learn multi-rate signal processing, decimation, and how to harness intentional integer overflow.

## Part 1: The DSP Math & "Wrap-Around" Magic
A CIC filter consists of three parts: Integrators, a Decimator, and Combs. 
We are building a 3-stage ($N=3$) filter with a decimation ratio of $R=640$.

**1. The Bit-Growth Calculation (CRITICAL)**
In a CIC filter, the DC gain is $R^N$. 
*   Our Gain = $640^3 = \mathbf{262,144,000}$. 
*   To calculate how many extra bits are required to hold this massive number without losing data, we take $\log_2(640^3)$, which is exactly **28 bits of growth**. 
*   Your input from the mixer is **16 bits**. 
*   $16 + 28 = \mathbf{44 \text{ bits}}$. 
*   *Rule:* Every internal integrator, comb delay, and comb result register inside your CIC filter must be exactly 44 bits wide. The decimation counter and 24-bit interface output are exceptions.

**2. The Hogenauer Rule (Intentional Overflow)**
Because the integrators are constantly adding data to themselves, they will inevitably exceed the 44-bit limit and overflow. 
*   *In normal DSP:* Overflow is a disaster. You usually write "saturation" logic to prevent it. 
*   *In a CIC Filter:* **You MUST let it overflow naturally.** As long as you use Two's Complement signed arithmetic, and the bit-width (44 bits) is strictly enforced, the Comb stages at the end will mathematically "un-wrap" the overflow and restore the perfect signal! 

## Part 2: Python Golden Vectors
To verify this filter, you cannot use a complex sine wave. You must use an **Impulse** or a **Step** function to trace the math.

**Your Task:** Write a Python script (`cic_model.py`) that simulates the CIC math.
1.  Create an input array of 2000 zeros, but make the very first value `1` (an Impulse).
2.  Pass it through 3 discrete Integrator stages.
3.  Decimate the array by keeping only every 640th sample.
4.  Pass the decimated array through 3 discrete Comb stages (where $Out = Current - Previous$).
5.  Print the expected 44-bit comb values and scaled 24-bit output values to a text file for your testbench.

---

## Part 3: The AI Prompt (The Blueprint)

AI coding models (like Claude or Gemini) will almost always mess up CIC filters. They will try to add saturation protection, or they will make the combs the wrong bit-width. You must be extremely strict in your prompt.

> **SystemVerilog CIC Decimator Request**
> Act as a Senior DSP Engineer. Write a fully pipelined, 3-stage Cascaded Integrator-Comb (CIC) filter. Do not use an FSM. 
> 
> **Parameters:**
> *   `R = 640` (Decimation Ratio)
> *   `WIDTH = 44` (Internal register width)
> 
> **Inputs:** 
> *   `clk_30m`, `reset_n`
> *   `data_in[15:0]` (Signed 16-bit)
> 
> **Outputs:** 
> *   `data_out[23:0]` (Signed 24-bit)
> *   `valid_out` (Pulses HIGH for 1 clock cycle every time a new 48 kHz sample is ready)
> 
> **Architecture Rules:**
> 1.  **Input Sign Extension:** Sign-extend `data_in` from 16 bits to 44 bits.
> 2.  **3 Integrator Stages:** Running continuously on `clk_30m`. Each stage is simply `acc = acc + previous_acc`. *Allow natural two's complement overflow. Do NOT use saturation logic.*
> 3.  **The Decimator:** Use a counter from 0 to 639. When it rolls over, pulse a `decimation_strobe` signal HIGH for one clock cycle.
> 4.  **3 Comb Stages:** The combs run on `clk_30m`, but they MUST use a **Clock Enable**. They only update their registers when `decimation_strobe` is HIGH. Each stage is `comb = current_val - delayed_val`. 
> 5.  **Output Truncation & Scaling:** For a full-scale pure sine input, the maximum peak output is $128 \times 127 \times 640^3 \approx 4.26 \times 10^{12} < 2^{42}$, so bits `[43:42]` are redundant sign-extension bits. Output bits `[41:18]` (`comb_out[WIDTH-3 -: 24]`) as the signed 24-bit `data_out` value. This yields a **+12 dB (4×)** dynamic range boost over `[43:20]` while guaranteeing zero clipping on full-scale signals.

---

## Part 4: The Self-Checking Testbench

Because CIC filters have a massive pipeline delay, your testbench (`cic_tb.sv`) must be highly intelligent.

1.  **The Impulse Test:** Feed the filter an input of `1`, followed by thousands of `0`s. The first valid output occurs after input sample 639, because the decimation counter keeps samples 0 through 639 as its first block.
2.  **The Valid Monitor:** Write a block in your testbench that *only* reads the CIC output when `valid_out` is HIGH. 
3.  **The Self-Check:** Compare the outputs against your Python Golden Vectors. Run the impulse, step, tone, maximum-positive, and maximum-negative scenarios with reset between independent scenarios.
4.  **The DC Gain Check:** Feed the filter a constant DC value of `1`. Because the gain of the filter is $640^3$, the final 44-bit output inside the filter should eventually settle at exactly `262,144,000`. Assert that this is true!
5.  **Inject a Bug:** Purposely change the internal register width in the RTL from 44 bits to 40 bits. Run the simulation and watch the massive data corruption that occurs when the Hogenauer rule is violated.

---

## Part 5: Hardware Payoff & Synthesis

There is no hardware flashing requirement this week! This is a pure Verification and DSP week. Your primary deliverable is the successful simulation and the `cic_capture.txt` comparison against the independent Python model.

However, you must run your CIC filter through the Synthesis tool (Yosys) to check your **Resource Utilization**. 
*   A 44-bit adder takes up a lot of logic cells (LUTs). 
*   Verify that your CIC filter easily fits inside the iCE40UP5K without consuming more than 15-20% of the chip. 
*   Ensure that the synthesizer doesn't throw any structural errors on the 30.72 MHz clocked design due to the long carry-chains of the 44-bit adders. The standalone Lab 08 check is synthesis-only; routed timing is covered when this block is integrated in Lab 09.

---

## 📝 Deliverables & Oral Defense
At your weekly 15-minute team meeting, you must provide:
1. **The Blueprint:** Your pipeline diagram showing the Integrators, the Clock Enable barrier, and the Combs. 
2. **The Bit Growth Math:** Walk the instructor through exactly why $16 + 28 = 44$ bits on the whiteboard. 
3. **The Simulation Proof:** Demonstrate your testbench feeding an impulse into the CIC filter. Show the GTKWave trace where the integrators wildly overflow, and explain how the comb stages successfully recovered the data. 
4. **The AI Critique:** Did the AI try to implement the Comb stages on a separate slow clock (which causes horrible Clock Domain Crossing issues), or did it correctly use a Clock Enable (`ce`) on the fast 30 MHz clock? Did it try to add overflow protection?
5. **The Synthesis Log:** Show the instructor your Yosys synthesis report. How many LUTs did your filter consume?