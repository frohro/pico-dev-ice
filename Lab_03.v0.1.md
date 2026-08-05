
# Lab 3: The I2S Audio Transmitter

**Objective:** This week, you will bridge the FPGA to the Raspberry Pi Pico. You will learn clock division, shift registers, and the serial timing used by the board's I2S receiver. You will use Python to generate golden vectors, use AI to generate bounded SystemVerilog, and use a self-checking testbench to prove the bit alignment. The hardware demonstration is optional until boards and a configured Pico are available.

## Part 1: The I2S Hardware Contract
The Pico is expecting to receive audio from the FPGA. The FPGA is the **I2S Master**, meaning it generates the clocks. The Pico's PIO state machine (which you used with the PCM1808 last semester) is the **I2S Slave**, meaning it listens.

For this lab, use the following Dev-iCE contract. The signal names match
`Software/ddc_sdr_firmware/fpga/ddc_sdr.pcf` and the receiver in
`Software/ddc_sdr_firmware/i2s_rx.pio`:
1.  **Bit Clock (`BCK`):** Runs at exactly **3.072 MHz**.
2.  **Word Select (`WS`):** Its complete low-to-high-to-low cycle runs at exactly **48 kHz**. It changes level every 32 BCK periods.
    *   `WS = 0` indicates the **Left Channel**.
    *   `WS = 1` indicates the **Right Channel**.
3.  **Frame Size:** 64 bits total per `WS` cycle (32 bits per channel). 
4.  **Data Alignment:** We transmit a 24-bit sample in the most-significant 24 bit times of a 32-bit slot. The final 8 bit times are zero, so the slot word is `{sample[23:0], 8'b0}`. Data is MSB-first.
5.  **Local alignment rule:** The FPGA changes `i2s_rx_data` and `WS` on a BCK falling edge. The Pico's PIO samples `i2s_rx_data` on the next BCK rising edge and collects 32 bits for the channel selected by `WS`. Therefore the first bit of each slot must already be valid before that first rising edge. Do not add an extra dummy bit to this lab.
6.  **Important terminology:** Generic I2S documents often describe a one-bit delay between a WS transition and the first data bit, but that phrase is easy to interpret incorrectly at the signal edges. The checked-in Pico PIO is the receiver contract for this board; prove the exact edge relationship in your waveform.

### The Clock Budget
Your master clock is **30.720 MHz**.
*   `BCK` has a full period of 10 master-clock cycles, so its output toggles every 5 master-clock cycles: `30.720 MHz / 10 = 3.072 MHz`.
*   A complete I2S frame is 64 BCK periods, so `3.072 MHz / 64 = 48 kHz`.
*   `WS` changes level after 32 BCK periods, because each channel occupies one 32-bit slot.

*The Sawtooth Generator:* To test this, you don't need real radio data yet. Create a 24-bit counter that increments by a small fixed amount every 48 kHz cycle. This will generate a mathematically perfect digital sawtooth wave.

---

## Part 2: Python Golden Vectors (The Verification)
Before you ask AI to write the SystemVerilog, you must know exactly what the output is supposed to look like. 

**Your Task:** Write a short Python script that simulates 3 complete I2S frames (Left and Right channels) of your Sawtooth wave. 
*   The script must print out the exact sequence of 64 sampled data bits (1s and 0s) expected for each frame, including the channel boundary and zero-padding. It must also state the WS level for each 32-bit slot.
*   Save this output to a text file (e.g., `expected_i2s.txt`).

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
> * Update `i2s_rx_data` and `i2s_ws` on the master-clock event that creates a falling edge of `i2s_bck`. The first data bit must be valid for the following BCK rising edge, matching the Pico PIO.
> * `i2s_ws` = 0 is Left and `i2s_ws` = 1 is Right. Send `{sawtooth_val, 8'b0}` MSB-first. Send the same sample in both slots and increment it once per complete 64-bit frame.

---

## Part 4: The Self-Checking Testbench
Do not trust the AI's shift register logic. "Off-by-one" bit shifts are the most common AI hallucination in serial protocols.

Write a SystemVerilog testbench (`i2s_tb.v`) that:
1. Instantiates your module and drives the 30.72 MHz clock.
2. Uses an `always @(posedge i2s_bck)` block to act exactly like the Pico's PIO state machine.
3. It must observe each BCK rising edge, verify the WS level, collect the next 32 sampled bits into a shift register, and compare that word against the values in the file generated by Python.
4. **Inject a Bug:** Purposely shift the first bit of one slot or change the WS boundary by one BCK edge. Prove that your testbench throws a `$display("FAIL")` error.

---

## Part 5: Hardware Bring-Up & Audacity Payoff
Once your testbench passes perfectly, it is time to deploy it to the Pico Dev-iCE.

1. Synthesize your code and generate your bitstream (`i2s_test.bin`).
2. Flash your bitstream to the FPGA.
3. Build and load the current Dev-iCE firmware in `Software/ddc_sdr_firmware`. It provides the Pico-side I2S receiver and UAC1 audio path.
4. Plug the Pico USB into your PC. Your computer will recognize it as a USB Microphone/Soundcard.
If your HDL is aligned with the Pico's PIO contract and the firmware is configured for the FPGA, Audacity should receive the sawtooth samples on both channels. A simulation pass is the required result; hardware and Audacity are an additional demonstration when the board is available.

---

## 📝 Deliverables & Oral Defense
At your weekly 15-minute team meeting, you must provide:
1. **The Blueprint:** Your clock-tree diagram and a drawing showing the I2S bit-alignment.
2. **The Golden Vectors:** Your Python script and the resulting text file.
3. **The Simulation Proof:** Demonstrate your testbench catching an injected bit-shift error, and then passing the clean RTL.
4. **The AI Critique:** What did the AI mess up? Did it struggle with the falling-edge updates? Did it forget the 1-bit delay? 
5. **The Hardware Demo:** Open Audacity and show the instructor the recorded sawtooth wave from your physical board.