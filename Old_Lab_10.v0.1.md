


# Lab 10: Automatic Gain Control (AGC) & Live Radio

**Objective:** The Verilog is finished. The hardware is locked. This week, you cross the boundary into Software Engineering. You will write the Automatic Gain Control (AGC) loop on the Raspberry Pi Pico to manage the radio's analog front-end. Finally, you will connect a real antenna, stream the I2S data to your PC over USB, and tune into a live shortwave broadcast station using your Pico Dev-iCE!

## Part 1: The Hardware/Software Contract
Your FPGA is currently outputting a sticky interrupt on `FPGA_INT` (Pico GPIO 0) whenever the ADC clips. To protect the radio, the Pico software must catch this interrupt and turn down the analog volume using the `PGA0` through `PGA3` pins. 

**The Logic Inversion Reminder:**
Because we used N-Channel MOSFETs as level shifters to protect the 4.5V analog switches, your software logic is inverted!
*   Writing `0` to a PGA pin turns the MOSFET OFF, allowing the 10k resistor to pull the switch HIGH to 4.5V (The straight-through, Maximum Gain path).
*   Writing `1` to a PGA pin turns the MOSFET ON, pulling the switch to Ground (The Attenuator/Bypass path). 

**The Verified Gain States:**
You will not use a simple `+1` counter for the volume. To ensure the radio volume drops smoothly and monotonically, your software must step through this specific sequence defined in `ddc_protocol.h`:
1.  **State `0x0`:** All MOSFETs OFF. Max sensitivity (+40 dB). 
2.  **State `0x1`:** 5 dB pad engaged (+35 dB).
3.  **State `0x3`:** 5 dB and 10 dB pads engaged (+25 dB).
4.  **State `0xF`:** All pads engaged, BOTH LNAs bypassed. Maximum protection (-15 dB).

## Part 2: The AGC Algorithm
The checked-in implementation uses the Pico C firmware. The GPIO interrupt
callback only records a pending event; PGA writes, SPI transactions, and timer
work run in the foreground loop so USB and audio handling are not blocked.

1.  **Listen:** Monitor `FPGA_INT` (GPIO 0).
2.  **React:** If `FPGA_INT` is HIGH, look at your current Gain State. Advance to the *next* state in the sequence (`0x0 ➔ 0x1 ➔ 0x3 ➔ 0xF`). 
3.  **Apply:** Write the new 4-bit state to the `PGA0..PGA3` GPIO pins.
4.  **Clear the Latch:** The FPGA is still stuck in the interrupt state! Use the Pico's `SPI0` bus to send exactly eight bytes for Command `0x04` (`DDC_FPGA_CMD_CLEAR_OTR`) with a value of `1`: `D5 01 04 04 01 00 00 00`. The runtime FPGA SPI clock is configured to 10 MHz.
5.  **Recover (Decay):** If `FPGA_INT` stays LOW for 2 seconds, the signal is quiet. Drop the state back down by one level to increase the radio's sensitivity. Implement this with a timestamp check, not `sleep_ms(2000)` or another blocking delay.

---

## Part 3: The AI Prompt (The Blueprint)

AI is exceptionally good at writing Python and C hardware control loops, provided you give it the exact memory map and pinout.

> **Raspberry Pi Pico AGC Software Request**
> Act as a Senior Embedded Software Engineer. Write a [MicroPython / C++] script for a Raspberry Pi Pico that implements an Automatic Gain Control (AGC) loop.
> 
> **Hardware Pins:**
> *   `FPGA_INT`: GPIO 0 (Input, Pulled Down).
> *   `PGA_PINS`: GPIOs 8, 9, 10, 11 (Outputs).
> *   `SPI0`: SCK on GPIO 6, MOSI on GPIO 7, MISO on GPIO 4.
> *   `SPI_CSn`: GPIO 5 (Output, active low).
> 
> **The State Machine:**
> Create an array of valid states: `[0x0, 0x1, 0x3, 0x0F]`. Track the current index (start at 0).
> 
> **The AGC Loop:**
> 1. If `FPGA_INT` goes HIGH:
>    *   If index < 3, increment the index. 
>    *   Apply the new state from the array to the 4 `PGA_PINS`.
>    *   Call a function `clear_fpga_otr()`.
> 2. If `FPGA_INT` remains LOW for 2000 milliseconds:
>    *   If index > 0, decrement the index.
>    *   Apply the new state. 
> 
> **The SPI Clear Function:**
> Write `clear_fpga_otr()`. It must assert `SPI_CSn` LOW, transmit exactly 8 bytes (`0xD5, 0x01, 0x04, 0x04, 0x01, 0x00, 0x00, 0x00`), and de-assert `SPI_CSn` HIGH. Use a standard 10 MHz SPI baud rate.

---

## Part 4: Live Radio! (The Hardware Payoff)

Once your AGC loop is running on the Pico, your radio is fully functional and protected from overload. It is time to listen to the world.

1.  Connect the Pico Dev-iCE board to your PC via USB.
2.  Ensure the Pico is running the UAC1 USB-Audio firmware, routing the FPGA's I2S data to the PC.
3.  Connect a wire antenna (or a tuned dipole) to the **`SDR_ANT`** BNC connector.
4.  Open your SDR software (SDR++ or Quisk) on your PC. Select the Pico as your audio input device.
5.  Use the Pico terminal/software to send SPI Command `0x01` to set the NCO frequency to **10.000 MHz** (WWV Time Station) or a known local AM broadcast frequency.
6.  **Observe the Waterfall!** You should see signals appearing on your screen. 
7.  **Test the AGC:** Touch the antenna center pin with your finger to inject massive 60Hz noise. Watch the Pico serial terminal—it should step through the verified sequence toward `0xF` attenuation to protect the ADC, and the waterfall should dim. Repeated clipping events may be coalesced while `FPGA_INT` is already high.

---

## 📝 Deliverables & Oral Defense (The Final Review)

At your final team meeting, you must provide:
1. **The Blueprint:** Draw the Hardware Abstraction Layer (HAL) showing how your software talks to the physical pins. 
2. **The Software Proof:** Walk the instructor through `agc_control.h` and the firmware integration. Explain how the Little-Endian bytes for the `0x04` clear command are formatted in memory before being sent over the 10 MHz SPI bus. The host tests must cover escalation, saturation, decay, timer reset, and the exact clear frame.
3. **The AI Critique:** Did the AI use a blocking `sleep()` function for the 2-second decay timer, which would freeze the whole microcontroller? Explain how the implementation uses a nonblocking timestamp check instead.
4. **The Live Demo:** Open SDR++ and show the instructor a live radio station streaming from your board. Demonstrate your software successfully catching an `FPGA_INT` clipping event and clearing it. 

### 🎉 Congratulations!
You have successfully designed a mixed-signal PCB, written rigorous DSP and Control logic in SystemVerilog, verified it mathematically, and written an embedded software driver to control it. You are now officially a Radio Systems Engineer.