
# Lab 5: ADC Capture & The OTR Sticky Status Register

**Objective:** This week, you will register the high-speed ADC bus in the
30.720 MHz FPGA domain and build a clocked sticky OTR status register. The
sticky status is a hardware-to-software handshake between the FPGA and the
Pico.

## Part 1: The Hardware Contract
The MS9280 is a 32 MSPS, 8-bit Analog-to-Digital Converter.
*   **The Clock (`adc_clk`, FPGA pin 23):** The FPGA forwards the 30.720 MHz master clock to the ADC.
*   **The Data (`adc_data[7:0]`, FPGA pins 25, 26, 27, 28, 31, 32, 34, 35):** The ADC outputs 8 bits of data.
*   **The Out-of-Range Pin (`adc_otr`, FPGA pin 36):** If the analog radio signal exceeds the ADC's voltage limits (clipping), the ADC asserts the OTR input HIGH.
*   **The Pico interrupt (`fpga_int`, FPGA pin 13):** The FPGA holds this active-high output until a valid clear command arrives.

**The Timing Trap:**
Because the ADC sits away from the FPGA, the ADC clock, output delay, board
skew, and FPGA input setup/hold time all matter. Register `adc_data[7:0]` and
`adc_otr` on the 30.720 MHz capture edge before using them in DSP logic. This
lab models the inputs as stable around that edge; it does not replace timing
constraints or hardware timing validation.

## Part 2: The Sticky Status Protocol (System Architecture)
The interface uses a **Deterministic Handshake** rather than a fixed-duration
pulse stretcher.
According to `ddc_protocol.h`, the system handles clipping like this:
1.  A massive radio signal causes the ADC to assert `OTR` HIGH for one sample-clock period, approximately 32.55 ns at 30.720 MHz.
2.  The FPGA catches this synchronously and sets a **sticky status register**. It drives the `FPGA_INT` pin HIGH and keeps it HIGH until cleared.
3.  The Pico sees `FPGA_INT` go HIGH. It knows the radio is clipping.
4.  The Pico steps up the PGA attenuation to turn down the analog volume. 
5.  The Pico sends SPI Command `0x04` (`DDC_FPGA_CMD_CLEAR_OTR`) to the FPGA.
6.  Your SPI Parser (from Lab 4) generates a `cmd_clear_otr` pulse. 
7.  The FPGA sees this clear pulse and forces `FPGA_INT` back LOW, unless `OTR` is also HIGH on that same edge. OTR has priority so an active clipping event cannot be cleared accidentally.

---

## Part 3: The AI Prompt (The Blueprint)

Before prompting the AI, map out your module. It has two simple jobs: an
8-bit capture register for the data, and a clocked sticky status register for
the `OTR` interrupt. This is not a level-sensitive SR latch; all state changes
occur on `clk_30m` edges, with asynchronous reset only.

Use this template to generate your RTL:

> **SystemVerilog ADC Capture & OTR Sticky-Register Request**
> Act as a Senior ASIC Designer. Write a SystemVerilog module that captures a high-speed ADC bus and implements a sticky interrupt register. Normal state changes must occur synchronously to `clk_30m`; use an active-low asynchronous reset.
> 
> **Inputs:** 
> *   `clk_30m`, `reset_n`
> *   `adc_data_in[7:0]` (Raw pins from the ADC)
> *   `adc_otr_in` (Raw pin from the ADC)
> *   `cmd_clear_otr` (1-clock-cycle pulse from the SPI Parser)
> 
> **Outputs:** 
> *   `adc_clk` (30.720 MHz clock forwarded to the ADC)
> *   `adc_data_out[7:0]` (Registered, safe data for the DSP mixer)
> *   `fpga_int` (The sticky interrupt status to the Pico)
> 
> **Architecture:**
> 1.  **Data Capture:** On every positive edge of `clk_30m`, register `adc_data_in` into `adc_data_out`.
> 2.  **The Sticky Status Register (`fpga_int`):**
>     *   If `adc_otr_in` is HIGH, set `fpga_int` HIGH.
>     *   Else if `cmd_clear_otr` is HIGH, set `fpga_int` LOW.
>     *   Otherwise, `fpga_int` holds its current state.
>     *   *Priority Note:* If `adc_otr_in` and `cmd_clear_otr` happen on the exact same clock cycle, `adc_otr_in` wins (the status stays HIGH because clipping is still occurring).

---

## Part 4: The Self-Checking Testbench (Edge Cases)

An interrupt status register that drops events or clears incorrectly will break
the radio's Automatic Gain Control (AGC). Your testbench
(`adc_capture_tb.sv`) must test the edge cases.

**Your testbench MUST check and print "PASS" for the following:**
1.  **The 1-Cycle Catch:** Assert `adc_otr_in` for exactly ONE 30.72 MHz clock cycle, then pull it LOW. Assert that `fpga_int` goes HIGH and stays HIGH for at least 50 clock cycles.
2.  **The Clear:** Assert `cmd_clear_otr` for one clock cycle. Assert that `fpga_int` drops LOW on the next clock. 
3.  **The Collision (Priority Check):** Assert both `adc_otr_in` AND `cmd_clear_otr` on the exact same clock edge. Assert that `fpga_int` evaluates to HIGH. (If it goes LOW, your radio will ignore active clipping!)
4.  **Data Alignment:** Feed a ramp sequence into `adc_data_in` (`0x01`, `0x02`, `0x03`). Assert that `adc_data_out` outputs the exact same ramp, delayed by exactly one clock cycle.

---

## Part 5: Hardware Bring-Up & The AGC Handshake

This week, you will wire Lab 4 (the SPI parser) and Lab 5 (the ADC capture)
together in a top-level wrapper.

1.  Connect your SPI Parser's `cmd_clear_otr` output to this new module's clear input. 
2.  Synthesize your bitstream (`adc_test.bin`) and flash the FPGA.
3.  Flash the current C SDR firmware from `Software/ddc_sdr_firmware` to the Pico.
4.  Open your serial terminal to the Pico to view the debug logs.

**The Hardware Test:**
*   *Warning: Do not short ADC or analog-input pins to power or ground.*
*   Do not use body contact, a screwdriver, or an unspecified 3.0 V peak-to-peak source as a trigger. If a physical test is approved, use a current-limited, documented signal-generator level within the ADC and analog-front-end limits, and verify it with an oscilloscope before connecting it.
*   Probe `fpga_int` with a logic analyzer or oscilloscope. When the ADC clips, the Pico should observe the active-high interrupt, step the PGA hardware mask through its verified states (`0x0 -> 0x1 -> 0x3 -> 0xF`), and send command `0x04` to clear the sticky status.

If the Pico climbs to `0xF` (Maximum Attenuation), your hardware handshake is functioning perfectly!

---

## 📝 Deliverables & Oral Defense
At your weekly 15-minute team meeting, you must provide:
1. **The Blueprint:** Your block diagram showing the data path and the clocked sticky-register logic.
2. **The Timing Diagram:** Draw the exact clock-by-clock timing of the Collision edge-case. 
3. **The Simulation Proof:** Demonstrate your testbench catching a broken priority state, and then show it passing the corrected RTL.
4. **The AI Critique:** How did the AI handle the priority collision between Set and Reset? Did it use an `if/else` block, or did it try to use combinational logic? Why is an `always_ff` block required here?
5. **The Hardware Demo:** Show `fpga_int` asserting on a controlled overload and the Pico clearing it after the PGA response. A serial log alone is not sufficient evidence of the FPGA pin timing.