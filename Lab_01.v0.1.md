



# Lab 1: Hardware Design Review & Board Bring-Up

**Objective:** Before you write a single line of SystemVerilog, you must understand the physical constraints of the hardware you are targeting. This week, you will perform a rigorous design review of the **Pico Dev-iCE** schematic, justify the engineers' analog design choices, and perform the initial smoke-test and power-on bring-up using MicroPython.

## Part 1: The Hardware Design Review
Open the provided KiCad schematic PDF for the Pico Dev-iCE board. Trace the signal paths and answer the following architectural questions. You will defend these answers during your oral review.

### 1. Power Domains and Noise Isolation
*   **The Dilemma:** The Raspberry Pi Pico contains a Switch-Mode Power Supply (SMPS) that is highly efficient but generates aggressive high-frequency switching noise. 
*   **The Review:** 
    1. Look at the Pico socket on the schematic. How is the board physically preventing the Pico's 3.3V switching noise from reaching the radio?
    2. Trace the `VBUS` (5V) line. Identify the three linear regulators (LDOs). What voltages do they output, and what specific components do they power?
    3. Look at the `+3.3V_Clean` net for the 30.72 MHz oscillator. Explain the purpose of the Ferrite Bead (`FB5`) separating it from the main digital 3.3V rail.

### 2. The Analog Front End (AFE)
Follow the RF signal path from the `SDR_ANT` SMA connector to the MS9280 ADC. 
*   **The Ethernet Transformer:** Why is an Ethernet LAN transformer (`TR1`) placed immediately after the antenna? What two specific types of RF noise does this filter out?
*   **The Programmable Gain Amplifier (PGA):** Look at the 5 dB, 10 dB, and 20 dB T-networks. 
    *   Why are the shunt resistors connected to ground through 0.1µF capacitors instead of being grounded directly?
    *   The `74LVC1G3157` analog switches are powered by 4.5V (`AVDD`). How does the schematic safely bias the RF signal to float at 2.25V (`VA/2`) so it doesn't clip against the Ground diodes?
*   **LNA Bypassing:** Why did the designers choose to bypass the op-amps (LNA 1 and LNA 2) using RF switches instead of just changing the feedback resistors to lower their gain?
*   **The ADC Balun (`TR2`):** Explain how this transformer converts the single-ended RF signal into a differential signal for the ADC. How does it apply the 2.25V DC bias to the ADC inputs?

### 3. Digital Control & Clocking
*   **The Clock:** Find the 30.72 MHz CMOS Oscillator. Why is there a 22 Ω resistor (`R34`) placed in series with the clock output trace before it reaches the FPGA?
*   **The SPI CRAM Paradox:** Look at the SPI0 bus connecting the Pico to the FPGA. The Pico's `SPI0_TX` (MOSI) pin is connected to a net named `ICE_SO` (Slave Out). Why is the Pico's Transmit pin connected to the FPGA's Output net? *(Hint: Think about what happens when the Pico writes to the FPGA's CRAM during boot).*
*   **The OTR Pin:** The ADC's Out-of-Range (`OTR`) pin pulses high when the analog signal clips. Why is this pin routed to the FPGA instead of directly to a Pico GPIO pin? 

---

## Part 2: Board Bring-Up (The Smoke Test)

Once you understand the hardware, it is time to turn it on. We will use the Pico's MicroPython environment to perform the initial bring-up. *(Note: In Week 3, we will transition to a high-performance C++ `dfu_util` environment for full SDR operation).*

### Step 1: The Visual & Smoke Test
1. Do not plug the board in yet. Visually inspect your PCB for solder bridges, particularly around the pins of the iCE40UP5K and the Pico socket.
2. Plug a USB cable into the Pico. 
3. **The Smoke Test:** Gently touch the top of the LDO voltage regulators and the FPGA. If any component is burning hot to the touch, unplug the board immediately and alert the instructor.

### Step 2: MicroPython Setup
1. Open your IDE (e.g., Thonny or VS Code with MicroPython extensions). 
2. Ensure the Pico is flashed with the standard MicroPython UF2 firmware.
3. Install the provided `ice.py` library to your Pico's filesystem. This library contains the routines necessary to reset the FPGA and blast a bitstream into its CRAM over the SPI0 bus.

### Step 3: The "Hello World" Bitstream
We need to prove the FPGA is alive, the 30.72 MHz clock is running, and the Pico can successfully program it. 

You are provided with a tiny pre-compiled bitstream (`led_test.bin`). This bitstream contains a simple hardware module that turns on the Green LED (Pin 41), turns off the Red and Yellow LEDs (Pins 39 and 40), and asserts `CDONE`. 

*(Hardware Note: The RGB LED pins on the iCE40UP5K are Open-Drain. They sink current to Ground to turn the LED on. To turn the LED off, the pin must be set to High-Impedance/1).*

### Step 4: Flashing the FPGA
Write a short MicroPython script (`main.py`) on the Pico to load the bitstream:

```python
import ice
import time

# 1. Initialize the FPGA SPI programming interface
# (This utilizes the Pico's SPI0 block connected to ICE_SI, ICE_SO, ICE_SCK)
fpga = ice.FPGA()

print("Holding FPGA in Reset...")
fpga.reset()
time.sleep(0.1)

print("Flashing led_test.bin to FPGA CRAM...")
with open("led_test.bin", "rb") as f:
    bitstream = f.read()
    fpga.program_cram(bitstream)

print("Boot complete. Checking CDONE status...")
# CDONE is wired to Pico GPIO 21. 
# Check your physical board: Is the White Done LED lit? Is the Green RGB LED lit?
```

Run the script. If the White `ICE_DONE` LED turns on, and the Green diagnostic LED turns on, your hardware is healthy and your toolchain is ready for Week 2!

---

## 📝 Deliverables & Oral Defense

At your weekly 15-minute team meeting with the instructor, you must provide:
1. **The Block Diagram:** A hand-drawn or cleanly digitized 1-page block diagram of the Pico Dev-iCE board. You must clearly label the Power Domains, the RX Path, the TX Path, and the SPI/I2S Control busses.
2. **The Defense:** Be prepared to answer any of the Design Review questions from Part 1. You must be able to point to the physical components on your PCB while answering.
3. **The Hardware Demo:** Demonstrate your Pico successfully flashing the `led_test.bin` file and lighting the Green LED.