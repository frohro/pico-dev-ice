Here is the complete **Lab 4 Document** for your students. It perfectly implements the `ddc_protocol.h` C-header you provided and introduces them to one of the most critical concepts in modern digital design: **Clock Domain Crossing (CDC)**.

This lab ensures they write bulletproof verification that catches corrupted frames and aborted communications *before* those errors can crash the radio's DSP math.

You can save this as `Lab_04_SPI_Parser.md` in your repository!

***

# Lab 4: The SPI Command Parser & Clock Domain Crossing (CDC)

**Objective:** This week, you will build the Control Interface for your radio. The Raspberry Pi Pico will act as the SPI Master, sending tuning and control commands to the FPGA. You will learn how to design a serial parsing FSM, how to handle Little-Endian byte ordering, and how to safely transfer data between two different clock speeds using Clock Domain Crossing (CDC) synchronization.

## Part 1: The Hardware Contract & The CRAM Paradox
As discussed in Lab 1, the Pico uses its hardware `SPI0` block to blast the bitstream into the FPGA's CRAM during boot, and reuses those exact same wires during runtime. 

Because the Pico is driving the bus, you must write your Verilog from the perspective of an **SPI Slave**:
*   **`spi_sck` / `ICE_SCK` (FPGA Pin 15):** The SPI Clock, driven by the Pico.
*   **`spi_csn` / `ICE_SSN` (FPGA Pin 16):** Active-low Chip Select, driven by the Pico.
*   **`spi_mosi` / `SPI0_TX` / `ICE_SO` (FPGA Pin 17):** Master Out, Slave In. Configure this as an **INPUT**.
*   **`spi_miso` / `SPI0_RX` / `ICE_SI` (FPGA Pin 14):** Master In, Slave Out. Configure this as an **OUTPUT**.

The current SDK runtime transfer is nominally 33 MHz, uses SPI mode 0, and
sends bits MSB-first. The FPGA samples MOSI on each rising edge of `spi_sck`;
the Pico changes MOSI on the falling edge.

## Part 2: The `ddc_protocol.h` Frame Format
To prevent the FPGA from acting on garbage data or static, the Pico wraps every command in a strict 8-byte (64-bit) protocol frame. 

Every time the Pico pulls `ICE_SSN` LOW, it will transmit exactly 8 bytes:
1.  **Byte 0 (Sync):** `0xD5`
2.  **Byte 1 (Version):** `0x01`
3.  **Byte 2 (Command):** e.g., `0x01` (Set Freq), `0x02` (Set Rate), `0x04` (Clear OTR).
4.  **Byte 3 (Length):** `0x04` (Indicating a 4-byte payload follows).
5.  **Bytes 4, 5, 6, 7 (Payload):** A 32-bit value in **Little-Endian** format. (Byte 4 is the Least Significant Byte; Byte 7 is the Most Significant Byte).

*Rule: If `ICE_SSN` goes HIGH before all 64 bits are received, or if the Sync/Version/Length bytes do not match the expected values exactly, the FPGA must discard the frame and do nothing.*

## Part 3: The Danger of Clock Domain Crossing (CDC)
The SPI Clock (`ICE_SCK`) comes in bursts from the Pico and the current SDK runtime clock is nominally 33 MHz. Your DSP radio logic runs continuously on a completely separate 30.72 MHz master clock.

If you wire the output of an SPI shift register directly to your DSP logic, the 30.72 MHz clock might try to read the 32-bit value while the SPI clock is changing it. You can read a mixture of the old and new frequency, tearing the data and corrupting the radio control state.

**The Mailbox / Handshake Solution:**
1.  **The SPI Domain (`always @(posedge spi_sck)`):** Shift the incoming bits into a temporary register. If all 8 bytes are valid and `spi_csn` stays LOW through the eighth byte, copy the command and value into a held shadow register and toggle a `spi_commit_flag`. A rising `spi_csn` asynchronously abandons a partial frame.
2.  **The Master Domain (`always @(posedge clk_30m)`):** Pass the commit toggle through a **2-Flop Synchronizer** and detect a change. The shadow bus remains unchanged while the toggle crosses, so the 30.72 MHz domain copies a complete command only after the bundled data has settled. A synchronizer reduces metastability risk for the toggle; it does not make arbitrary unsynchronized buses safe.

---

## Part 4: The AI Prompt (The Blueprint)

Before prompting the AI, draw the FSM for the SPI parser on a whiteboard. Map out the `WAIT_SYNC` ➔ `CHECK_VER` ➔ `GET_CMD` states. Once you understand the flow, use this template:

> **SystemVerilog SPI Parser & CDC Request**
> Act as a Senior ASIC Designer. Write a SystemVerilog SPI Slave that parses a specific 64-bit command frame and safely crosses the data into a 30.72 MHz clock domain.
> 
> **Inputs:** `clk_30m`, `reset_n`, `spi_sck`, `spi_csn`, `spi_mosi`
> **Outputs:** `spi_miso`, `cmd_freq_valid`, `cmd_freq_val[31:0]`, `cmd_rate_valid`, `cmd_rate_val[31:0]`, `cmd_clear_otr`
> 
> **Clock Domain 1: SPI Parser (`posedge spi_sck`)**
> *   When `spi_csn` is LOW, shift data in from `spi_mosi` (MSB-first per byte). 
> *   The frame is 8 bytes: `Sync (0xD5)`, `Version (0x01)`, `Command`, `Length (0x04)`, and a 32-bit `Value` (Little-Endian).
> *   Use an FSM or separate bit/byte counters. If the Sync, Version, or Length do not match exactly, consume and discard the frame. If `spi_csn` rises before byte 7 completes, discard the frame and reset the parser for the next transaction.
> *   If the frame is valid and complete, store the parsed Command and 32-bit Value in a shadow register, and toggle a `spi_commit_flag`.
> 
> **Clock Domain 2: The Master Domain (`posedge clk_30m`)**
> *   Pass the `spi_commit_flag` through a 2-stage flip-flop synchronizer and detect a change.
> *   When an edge is detected, read the held shadow register. Do not read a shadow bus while the SPI parser is still updating it.
> *   If Command == `0x01`, output the 32-bit value to `cmd_freq_val` and pulse `cmd_freq_valid` for one clock cycle.
> *   If Command == `0x02`, output the value to `cmd_rate_val` and pulse `cmd_rate_valid` for one clock cycle.
> *   If Command == `0x04`, pulse the `cmd_clear_otr` output for one clock cycle. 

---

## Part 5: The Self-Checking Testbench (Negative Testing)

This is the most critical verification week of the course. You must write a testbench (`spi_parser_tb.v`) that acts as the Pico and deliberately tries to break your FPGA.

Your testbench must simulate the 30.72 MHz clock and the asynchronous SPI clock. Write a `task send_spi_byte(input [7:0] data)` to make generating frames easier.

**Your testbench MUST check for the following failures:**
1.  **The Happy Path:** Send `D5 01 01 04 00 00 00 00` (Set Frequency to 0). Verify that `cmd_freq_valid` pulses on the 30 MHz clock. 
2.  **The Bad Sync:** Send `AA 01 01 04...`. Verify the module completely ignores it.
3.  **Bad Header Fields:** Send a bad version and a bad length. Verify neither produces a command pulse.
4.  **The Aborted Frame (Torn Data):** Send a valid sync and command, but pull `spi_csn` HIGH after only 6 bytes. Verify the module drops the data and does **not** update the output registers.
5.  **Endianness Check:** Send a frequency command with payload `0x78 0x56 0x34 0x12`. Assert that the 30 MHz domain outputs exactly `32'h12345678`.
6.  **Other Commands:** Verify the sample-rate command and the clear-OTR command produce their own one-cycle outputs.
7.  **Back-to-Back Frames:** Send two valid frames in consecutive chip-select transactions and verify each commits exactly once.

---

## Part 6: Hardware Bring-Up

Once your testbench prints "PASS" for all edge cases, synthesize the code. 
We will test it on the board using the provided Pico firmware.

1.  Flash your generated bitstream (`spi_test.bin`) into the FPGA CRAM.
2.  Flash the current C SDR firmware from `Software/ddc_sdr_firmware` onto your Pico.
3.  *Temporary Debug Code:* Route `cmd_freq_val[0]` to `GREEN`, bit 1 to `YELLOW`, and bit 2 to `RED`. The standalone solution exposes these logical LED outputs.
4.  Open your serial terminal to the Pico and send the CDC text command `FREQ,1`. The Green LED should light up. Send `FREQ,7`. All three LEDs should light up. 

If the LEDs match your commands, your SPI FSM and Clock Domain Crossing are flawlessly passing data at hardware speeds!

---

## 📝 Deliverables & Oral Defense
At your weekly 15-minute team meeting, you must provide:
1. **The Blueprint:** Your FSM diagram or Shift-Register architecture drawing.
2. **The Endianness Math:** Explain manually how `0x78 0x56 0x34 0x12` becomes `32'h12345678`.
3. **The Simulation Proof:** Demonstrate your testbench safely ignoring the aborted CS frame, and show the exact clock cycle where the 2-flop synchronizer passes the valid data.
4. **The Hardware Demo:** Send frequency commands via the Pico's serial terminal and show the FPGA LEDs reacting instantly to the payload.