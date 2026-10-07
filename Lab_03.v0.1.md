
# Lab 3: The I2S Audio Transmitter

**Objective:** This week, you will bridge the FPGA to the Raspberry Pi Pico. You will learn clock division, shift registers, and the serial timing used by the board's I2S receiver. You will use provided golden vectors, use chat AI to generate bounded SystemVerilog and a self-checking testbench, and prove the bit alignment in simulation and on the hardware.

**Schedule:** Lab 3 is assigned Wednesday, October 7. The solution is discussed in lecture Friday, October 9. On Wednesday, October 14, you must be ready to continue Lab 3 and explain your progress after the October 12-13 Senior Trip. Lab 3 must be complete, tested, and ready for teaching and hardware validation at the Lab 4 meeting on Tuesday, October 20.

## AI Use Policy
Chat-based AI is permitted for this lab. You may ask a chat AI for explanations, design suggestions, or code generated from your written blueprint. Agentic AI is not permitted: do not use coding agents, autonomous IDE modes, workspace agents, or tools that inspect or modify your files, run commands, or execute tests on your behalf. You must make the edits, run the simulations, generate the golden vectors, and verify the results yourself, and you must be able to explain all submitted work.

## Part 1: The I2S Hardware Contract
The Pico is expecting to receive audio from the FPGA. The FPGA is the **I2S Master**, meaning it generates the clocks. The Pico's PIO state machine (which you used with the PCM1808 last spring quarter) is the **I2S Slave**, meaning it listens.

For this lab, use the following Dev-iCE contract. The signal names match
`Software/ddc_sdr_firmware/fpga/ddc_sdr.pcf` and the receiver in
`Software/ddc_sdr_firmware/i2s_rx.pio`:
1.  **Bit Clock (`BCK`):** Runs at exactly **3.072 MHz**.
2.  **Word Select (`WS`):** Its complete low-to-high-to-low cycle runs at exactly **48 kHz**. It changes level every 32 BCK periods.
    *   `WS = 0` indicates the **Left Channel**.
    *   `WS = 1` indicates the **Right Channel**.
3.  **Frame Size:** 64 bits total per `WS` cycle (32 bits per channel). 
4.  **Data Alignment:** We transmit a 24-bit sample in the most-significant 24 bit times of a 32-bit slot. The final 8 bit times are zero, so the slot word is `{sample[23:0], 8'b0}`. Data is MSB-first.
5.  **Philips alignment rule:** WS changes on the channel-boundary BCK rising edge, which completes the previous slot. The FPGA presents the new slot's MSB on the following BCK falling edge, and the Pico's PIO samples it on the next BCK rising edge. Each slot therefore has 32 sampled bits, with the WS transition occurring one BCK period before the new slot's MSB.
6.  **Verification requirement:** Do not infer the protocol from reconstructed words alone. Prove in the waveform that WS changes first, data changes on a falling BCK edge, and the new MSB is sampled on the following rising edge.

### The Clock Budget
Your master clock is **30.720 MHz**.
*   `BCK` has a full period of 10 master-clock cycles, so its output toggles every 5 master-clock cycles: `30.720 MHz / 10 = 3.072 MHz`.
*   A complete I2S frame is 64 BCK periods, so `3.072 MHz / 64 = 48 kHz`.
*   `WS` changes level after 32 BCK periods, because each channel occupies one 32-bit slot.

*The Sawtooth Generator:* To test this, you don't need real radio data yet. Create a 24-bit counter that increments by a small fixed amount every 48 kHz cycle. This will generate a mathematically perfect digital sawtooth wave.

---

## Part 2: Golden Vectors (The Verification)
Before you ask AI to write the SystemVerilog, you must know exactly what the output is supposed to look like. Python programming is not the goal of this lab, so the reference generator and its output are provided in `ENGR433-Solutions/Lab_03` and in the October 9 lecture notebook.

**Your Task:** Use the provided `generate_golden_vectors.py`, `expected_i2s.txt`, and `expected_words.mem` as the reference for your design. You must be able to explain how the vectors were generated and use them in your self-checking testbench. You may modify the sawtooth start value or step, but if you do, regenerate the expected files and explain the change.

---

## Part 3: The AI Prompt (The Blueprint)
Now that you know the math, use this template to prompt your AI (Claude, Gemini, or Windsurf). Fill in the brackets with your specific architectural decisions.

> **SystemVerilog I2S Transmitter Request**
> Act as a Senior ASIC Designer. Write a SystemVerilog module that acts as an I2S Audio Master and generates a Sawtooth test wave. 
> 
> **Inputs:** `clk_30m` (30.72 MHz), `reset_n`
> **Outputs:** `i2s_bck`, `i2s_ws`, `i2s_rx_data`
> 
> **Clock Generation:**
> * Use a clock-enable counter in the `clk_30m` domain. Toggle `i2s_bck` every 5 master-clock cycles to produce 3.072 MHz. Do not create a second procedural clock domain just for this exercise.
> * Change `i2s_ws` after every 32 BCK periods. Its complete cycle is 48 kHz.
> 
> **Data Generation:**
> * Create a 24-bit register `sawtooth_val` that increments by [X] every time `i2s_ws` completes a full cycle. 
> 
> **I2S Shift Register:**
> * Change `i2s_ws` on the channel-boundary rising edge. Present the new slot's first data bit on the following BCK falling edge so it is valid for the next BCK rising-edge sample, matching standard Philips I2S.
> * `i2s_ws` = 0 is Left and `i2s_ws` = 1 is Right. Send `{sawtooth_val, 8'b0}` MSB-first. Send the same sample in both slots and increment it once per complete 64-bit frame.

---

## Part 4: The Self-Checking Testbench
Do not trust the AI's shift register logic. "Off-by-one" bit shifts are the most common AI hallucination in serial protocols.

Use the provided `i2s_tb.sv` as a starting point, and use chat AI to help generate or explain the checks. You must understand and be able to defend every check. Your testbench must:
1. Instantiates your module and drives the 30.72 MHz clock.
2. Uses an `always @(posedge i2s_bck)` block to act exactly like the Pico's PIO state machine.
3. It must observe each BCK rising edge, verify the WS level, collect the next 32 sampled bits into a shift register, and compare that word against the values in the file generated by Python.
4. **Inject a Bug:** Purposely shift the first bit of one slot or change the WS boundary by one BCK edge. Prove that your testbench throws a `$display("FAIL")` error.

The provided solution checks six channel slots in simulation: three left slots and three right slots. Your submission must pass the corrected design and fail the intentionally broken design before the testbench timeout. Have this simulation evidence ready before the October 20 teach-back.

---

## Part 5: Hardware Validation and Teach-Back
Once your testbench passes perfectly, deploy the standalone bring-up design to the Pico Dev-iCE. Hardware validation is required at the Lab 4 meeting; it is not an optional bonus for this lab.

1. Synthesize the standalone bring-up top from `ENGR433-Solutions/Lab_03` and generate `i2s_test.bin`.
2. Flash the bitstream to the FPGA.
3. Verify that BCK, WS, and I2S data are present on the assigned pins. Use an oscilloscope or logic analyzer to check the clock rates and edge relationship.
4. Confirm the LED heartbeat or another documented power-on indicator.
5. At the October 20 meeting, teach the design only after it is working: explain the clock divider, frame counter, shift-register alignment, and the testbench's injected failure, then demonstrate the working hardware. Lab 3 is not complete until the simulation passes, the intentionally broken design fails, and the hardware validation succeeds.

Audacity and the full USB audio path are optional extensions. The required hardware evidence is a working I2S clock/data bring-up and a clear explanation of the measured signals.

---

## 📝 Deliverables & Oral Defense
At your weekly 15-minute team meeting, you must provide:
1. **The Blueprint:** Your clock-tree diagram and a drawing showing the I2S bit-alignment.
2. **The Golden Vectors:** The provided reference files, with a brief explanation of the Python generator and any changes you made.
3. **The Simulation Proof:** Demonstrate your testbench catching an injected bit-shift error, and then passing the clean RTL.
4. **The AI Critique:** What did the AI mess up? Did it struggle with the falling-edge updates? Did it forget the 1-bit delay? 
5. **The Hardware Teach-Back:** At the Lab 4 meeting, demonstrate the I2S bring-up on the physical board and explain the measured BCK, WS, and data timing. Audacity is optional.