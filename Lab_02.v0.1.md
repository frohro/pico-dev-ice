
# Lab 2: Control Logic, AI Verification, and the UK Traffic Light

**Objective:** This week, you will learn the "Edit-Simulate-Explain" loop. You will design Control Logic (a Finite State Machine), translate it into a strict AI prompt, and use AI to generate the SystemVerilog RTL. Before touching the hardware, you must write a self-checking testbench to mathematically prove the AI did not hallucinate the timing or the reset states.

## Part 1: The British Traffic Light FSM
In the United States, traffic lights transition from Red, to Green, to Yellow. In the United Kingdom, traffic lights use an intermediate "Get Ready" state to alert drivers with manual transmissions to put their cars in gear. 

The UK Sequence is:
1. **Red** (Stop)
2. **Red + Amber** (Stop, but prepare to go)
3. **Green** (Go)
4. **Amber** (Stop if safe to do so)
5. **Red** (Stop)

*(Note: On our Pico Dev-iCE board, we will use the Yellow LED for Amber).*

### 1. The Hardware Constraints
Look at your `ddc_sdr.pcf` file. You have three RGB LED pins:
*   `RED` (Pin 39)
*   `YELLOW` (Pin 40)
*   `GREEN` (Pin 41)

**CRITICAL PHYSICS NOTE:** These pins are **Open-Drain**. They connect the LED cathode to Ground. Therefore:
*   Outputting `1'b0` turns the LED **ON**.
*   Outputting `1'b1` turns the LED **OFF** (High-Z).

### 2. The Parameterized Clock
The Pico Dev-iCE master clock runs at **30.720 MHz**. If you simulate 5 seconds of real time at 30.72 MHz, your computer will simulate 153.6 million clock ticks, and your waveform viewer will likely crash.
*   Your module **must** use a `parameter TICKS_PER_SEC`.
*   In your testbench, you will override this parameter to a small number (e.g., `10`) so the simulation runs instantly. 
*   When compiling for the real hardware, it will default to `30720000`.

### 3. Your Task: The State Diagram
Before typing anything, draw a strict Finite State Machine (FSM) diagram on paper. 
*   Define the states (e.g., `ST_RED`, `ST_RED_YEL`, etc.).
*   Define the duration of each state (e.g., Red = 3 seconds, Red+Yel = 1 second, Green = 3 seconds, Yel = 1 second).
*   Define the exact 3-bit LED output for each state. 

---

## Part 2: The AI Prompt (The Blueprint)

Large Language Models (like Claude, Gemini, or Windsurf) are terrible at inventing hardware architecture, but they are flawless typists. You must give the AI your exact FSM blueprint.

Use the following template to prompt your AI. **Fill in the bracketed information with the data from your paper diagram.**

> **SystemVerilog FSM Generation Request**
> Act as a Senior ASIC Verification Engineer. Write a SystemVerilog module for a UK Traffic Light controller. Use a standard multi-always-block FSM architecture with `typedef enum logic`.
> 
> **Parameters:** 
> * `TICKS_PER_SEC` (default 30720000)
> 
> **Inputs:** `clk`, `reset_n` (Active Low)
> **Outputs:** `led_red`, `led_yel`, `led_grn` (Active Low: 0 = ON, 1 = OFF).
> 
> **Internal Timer:** Include a counter that increments every clock cycle to track seconds based on `TICKS_PER_SEC`. 
> 
> **States:**
> 1. `ST_RED`:
>     * Outputs: [Your LED bits]
>     * Wait for [X] seconds, then go to `ST_RED_YEL`.
> 2. `ST_RED_YEL`:
>     * Outputs: [Your LED bits]
>     * Wait for [X] seconds, then go to `ST_GRN`.
> 3. `ST_GRN`:
>    ... [Continue defining the rest of your states]
> 
> **Reset Behavior:** Asynchronous active-low reset. The system must default to `ST_RED` and clear the timer.

---

## Part 3: The Self-Checking Testbench

You must not trust the AI. You must prove it works mathematically. Write a testbench (`traffic_tb.v`) that does the following:

1.  **Override the Parameter:** Instantiate your module, overriding `TICKS_PER_SEC` to `10`. 
2.  **Generate the Clock:** Write an `always` block to toggle the clock every 5 time units.
3.  **The Self-Checking Monitor:** Write code that counts the clock ticks and uses `assert` or `if/else` statements to verify the outputs.
    *   *Example Check:* "If the reset is released, and we wait exactly 10 clock ticks, does the state transition from RED to RED_YEL? If not, `$display("FAIL")` and `$stop`."
4.  **Inject a Bug:** Purposely modify the AI's RTL (e.g., change the RED_YEL timer from 1 second to 2 seconds, or change an LED output to active-high). Run the testbench and prove that your testbench catches the error and halts.

---

## Part 4: Synthesis & Hardware Bring-Up

Once your testbench prints a clean "PASS" across all states, it is time to build the real hardware.

1.  Use the OSS CAD Suite tools (`yosys`, `nextpnr-ice40`, `icepack`) to synthesize your `.v` file against the `ddc_sdr.pcf` constraints file. 
2.  Generate your `traffic.bin` bitstream.
3.  Load the bitstream onto the Pico using the MicroPython script from Lab 1.
4.  Watch the LEDs on the Pico Dev-iCE board. If they sequence properly, your physical timing matches your simulated timing!

---

## 📝 Deliverables & Oral Defense

At your weekly 15-minute team meeting with the instructor, you must provide:

1. **The Blueprint:** Your hand-drawn FSM State Diagram, including timers and output vectors.
2. **The Prompt Log:** The exact prompt you fed to the AI.
3. **The AI Critique:** Identify one thing the AI did poorly (e.g., did it use an inefficient single-always block? Did it mess up the active-low logic? Did it have an off-by-one error on the counter?).
4. **The Simulation Proof:** Demonstrate your self-checking testbench running in the terminal. Show the output catching your injected bug, and then show it passing the corrected RTL.
5. **The Hardware Demo:** Show the physical board successfully running the UK traffic light sequence.