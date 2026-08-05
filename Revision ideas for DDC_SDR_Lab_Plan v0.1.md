Here is the completely revised, jargon-free `DDC_SDR_Lab_Plan.md`. 

I have structured it to explicitly build upon the MIT *Computation Structures* lectures your students just finished. It clearly separates Control Logic (FSMs) from Datapath Logic (Pipelining), gives them strict templates for how to control the AI, and lays out the smoothed 10-week difficulty curve we designed. 

You can copy and paste everything below the line directly into your repository.

***

# DDC SDR HDL Lab Plan

This is a ten-week, hands-on introduction to FPGA design and Digital Signal Processing (DSP) using the **Pico Dev-iCE** receiver. 

You already know the analog QSD SDR signal path. In this course, we are moving the radio into the digital domain. You will learn hardware description languages (SystemVerilog), clock domains, serial protocols, and rigorous simulation. 

This course is **Verification-First and AI-Assisted**. You will use AI (Google Gemini, Anthropic Claude, or Codeium Windsurf) to generate your SystemVerilog syntax. However, AI is notoriously bad at hardware architecture. You are not a typist in this class; you are a **Senior Systems Architect and Verification Engineer**. Your job is to provide the AI with strict blueprints, write testbenches to catch the AI's hallucinations, and mathematically prove your radio works.

---

## 🧠 The AI Workflow: Control vs. Datapath

Hardware design requires two distinct paradigms. Before you ask an AI to write RTL (Register-Transfer Level) code, you must determine which paradigm your module falls into and write the blueprint yourself based on your *MIT Computation Structures (L01-L06)* lectures.

### 1. Control Logic (Use State Machines)
Modules that parse protocols, wait for events, or route data (like SPI and I2S) are Control Logic. 
*   **The Blueprint:** You must draw a Finite State Machine (FSM). 
*   **The AI Prompt:** Do not ask the AI to "invent" the module. Give the AI your exact state transition table. 
    *   *Example Prompt:* "Write a SystemVerilog module using a 2-always-block FSM. The inputs are X and Y. State 1 is IDLE: if X=1, go to ACTIVE. State 2 is ACTIVE: wait 10 clocks, set output Y=1, and go to DONE."

### 2. Datapath Logic (Use Pipelining)
Modules that do heavy, continuous math (like the NCO, Mixer, and CIC Filter) are Datapath Logic. **There are no states here.** Data flows continuously on every single clock tick.
*   **The Blueprint:** You must draw a pipeline diagram.
*   **The AI Prompt:** Tell the AI exactly what happens on each clock edge and what the bit-widths are. 
    *   *Example Prompt:* "Write a fully pipelined SystemVerilog datapath. Stage 1: Register inputs A and B. Stage 2: Multiply A and B using signed two's complement math into a 16-bit register. Stage 3: Truncate the bottom 8 bits and output."

---

## 📝 Weekly Lab Deliverables

Every week, your team must submit the following before your oral defense:
1. **The Blueprint:** Your FSM State Transition Table OR your Pipeline diagram.
2. **The Reference Math:** A short Python script or hand-calculation showing what the correct DSP numbers should be. 
3. **The Testbench:** A self-checking SystemVerilog testbench (`_tb.v`) that feeds your reference math into the module and prints "PASS" or "FAIL".
4. **The RTL:** The AI-generated SystemVerilog module.
5. **The AI Log:** The exact prompts you used, and a note detailing **one mistake or hallucination** the AI made that your testbench caught.

*(Note: A screenshot of a GTKWave graph is helpful for explaining a bug to the instructor, but it is not proof of success. Your testbench must be self-checking!)*

---

## 📅 The 10-Week Schedule

### Week 1: Hardware Design Review & Clock Budgets
**Goal:** Understand the physical Pico Dev-iCE board and setup the toolchain.
*   **Lab:** Install OSS CAD Suite (Icarus Verilog + GTKWave). Inspect the KiCad schematic and the `ddc_sdr.pcf` constraints file. Identify the SPI, I2S, ADC, and DAC pins. 
*   **The Math:** Calculate the clock budget. If the master clock is 30.720 MHz, what decimation factor yields exactly 48 kHz? How do you divide 30.72 MHz to get a 3.072 MHz I2S bit-clock?
*   **Hardware Payoff:** Plug the board in and verify the Pico boots.

### Week 2: Traffic Light and AI Verification
**Goal:** Learn the edit-simulate-explain loop using Control Logic (FSM).
*   **Lab:** Provide the AI with an FSM table to generate a Traffic Light controller for the RGB LEDs on the board. You must implement a clock divider to slow 30.72 MHz down to 1-second intervals. 
*   **Testbench:** Your testbench must catch an incorrect reset state and an off-by-one timer.
*   **Hardware Payoff:** Flash the FPGA and watch the physical LEDs change.

### Week 3: I2S Audio Transmitter
**Goal:** Shift registers and serial timing.
*   **Lab:** Design the I2S Master module. The FPGA will output `BCK` (3.072 MHz) and `WS` (48 kHz) to the Pico. To test it, generate a fake digital "Sawtooth" wave inside the FPGA.
*   **Testbench:** Prove the data shifts out on the correct edge of the Bit Clock.
*   **Hardware Payoff:** The Pico routes the FPGA's I2S data to the PC over USB. You will hear the fake sawtooth tone in your headphones!

### Week 4: SPI Command Parser
**Goal:** Cross-Clock Domains (CDC) and serial receiving.
*   **Lab:** Design an SPI Slave FSM that receives volume and band-switch commands from the Pico's hardware `SPI0` bus. 
*   **Testbench:** Simulate the Pico sending asynchronous SPI bytes. Prove the internal FPGA registers update correctly without causing metastability.
*   **Hardware Payoff:** Use the Pico to send SPI commands and watch the physical Programmable Gain Amplifier (`PGA0-PGA3`) pins toggle.

### Week 5: ADC Capture and OTR Latch
**Goal:** High-speed I/O and asynchronous event catching.
*   **Lab:** Write the interface to capture the 8-bit ADC data at 30.72 MSPS. Design a "Pulse Stretcher" FSM for the Out-Of-Range (OTR) pin.
*   **Testbench:** Inject a microscopic 32ns clipping pulse on the simulated OTR pin. Prove your code stretches it to a full millisecond so the slow Pico can read it via the `FPGA_INT` pin.

### Week 6: The NCO (Numerically Controlled Oscillator)
**Goal:** Introduction to Datapaths, Phase Accumulators, and Sine LUTs.
*   **Lab:** Design a 32-bit NCO. This is pure pipelined datapath logic.
*   **Testbench:** Feed the module a tuning word. Save the simulated output to a text file (`$writememh`), plot it in Python/Excel, and mathematically prove the sine wave frequency is correct.
*   **Hardware Payoff:** Route the NCO directly to the MS9708 DAC. Use an oscilloscope to watch your digital math become a physical RF signal on the `AWG_OUT` port!

### Week 7: The Digital Mixer (Quadrature Demodulation)
**Goal:** Signed math and two's complement multiplication.
*   **Lab:** Instantiate DSP multiplier blocks. Multiply the incoming ADC data by the NCO Sine (I) and Cosine (Q) to shift the target radio station down to 0 Hz (Baseband).
*   **Testbench:** Feed a simulated high-frequency digital sine wave into the mixer. Prove the output contains the mathematically correct sum and difference frequencies.

### Week 8: The CIC Decimation Filter
**Goal:** Multi-rate DSP, bit-growth, and integrator wrapping. *(This is the hardest math week).*
*   **Lab:** Design a Cascaded Integrator-Comb (CIC) filter that decimates the 30.72 MHz I and Q data down by a factor of 640. AI will almost certainly mess up the bit-widths here. You must calculate the massive register growth manually to prevent data corruption.
*   **Testbench:** Feed a massive step-function into the filter. Prove in GTKWave that the integrators wrap around (two's complement overflow) correctly without losing data. 

### Week 9: System Integration
**Goal:** Top-level wiring.
*   **Lab:** The heavy lifting is done. Write a Top-Level wrapper that connects: `ADC -> Mixer -> CIC -> I2S`. 
*   **Testbench:** Run a full-system simulation. Feed fake radio static into the ADC and prove that clean audio comes out the I2S port. 
*   **Hardware Payoff:** Flash the full bitstream. The Digital Down-Converter (DDC) is alive!

### Week 10: AGC & Live Radio Demonstration
**Goal:** The Software/Hardware handshake.
*   **Lab:** The FPGA hardware is frozen. Spend this week writing the MicroPython Automatic Gain Control (AGC) loop on the Pico. It must monitor the I2S volume, listen to the OTR interrupt, and send SPI commands to the FPGA to toggle the PGA switches.
*   **Hardware Payoff:** Plug in an antenna, open SDR++ on your PC, and tune into a real shortwave broadcast station using the radio you built from scratch.

---

## 🗣️ Assessment & The Oral Defense

Because AI is generating your syntax, you will be graded on your ability to act as a **Senior Reviewer**. Every week, your team will have a 15-minute oral defense with the instructor. 

**Grade 1: The AI & Verification Product (40%)**
*   Does the code synthesize? 
*   Is the testbench self-checking, or does it only test the "happy path"?
*   Did your testbench successfully catch an edge-case or AI hallucination?

**Grade 2: Student Understanding (60%)**
During the oral defense, you must be prepared to answer:
1.  **"Show me the Waveform:"** Open GTKWave. *"Why did this signal go high exactly here? Trace it back to the RTL."*
2.  **The "What If" Simulation:** *"What happens in your simulation if the Pico tries to write to the SPI register at the exact same time the CIC filter updates?"*
3.  **AI Critique:** *"What was the dumbest mistake the AI made when generating this module, and how did you fix it?"*

If you blindly copy-pasted the AI's work and cannot explain the physics, you will fail the week (with one opportunity to study the code and try the defense again). 

### 🎓 The Final Exam
For the final exam, the radio will be broken down into three interconnected systems:
1.  **Analog Front End & Control** (Op-amps, T-networks, VNA bridge, and Pico SPI/AGC).
2.  **High-Speed DSP** (ADC kickback, NCO phase accumulator, and IQ Mixer math).
3.  **Multi-Rate DSP & Baseband** (CIC filter math, Decimation, Process Gain, and I2S formatting).

Your team will be assigned one section. You will have 20 minutes to present it to the rest of the class, defending your architectural choices as if you are pitching a commercial product to a tech company.