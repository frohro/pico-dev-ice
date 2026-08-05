# DDC SDR HDL Lab Plan

This is a ten-week, hands-on introduction to FPGA design using the Pico
Dev-iCE DDC receiver. Students already know the QSD SDR signal path and some
DSP math. The new material is HDL: clocks, registers, serial protocols,
simulation, and how to test code generated with AI.

The sequence is deliberately gentle. Students see a real board in Week 1,
learn to test simple logic in Week 2, learn two useful protocols, and only then
take on the NCO, mixer, and CIC filter.

## Course Shape

- Students work in pairs. Each pair owns an independent implementation of the
  common baseline.
- Every week has four parts: a short explanation, a small lab, a testbench, and
   a hardware demonstration when the board supports it.
- The common project is a 48 kHz, receive-only DDC path that is simulated,
   self-checking, and synthesizable for the Dev-iCE iCE40UP5K.
- Boards should be in students' hands from Week 1. Hardware demonstrations
   motivate the work, but a passing result never depends on a particular RF
   signal, antenna, or board revision.
- Students may use AI for RTL, testbench scaffolding, scripts, and debugging,
  but the pair is responsible for the specification, expected results, test
  intent, and explanation of every submitted line.

The board and software contracts are documented in the root
[`README.md`](README.md), the FPGA handoff
[`Software/ddc_sdr_firmware/fpga/README.md`](Software/ddc_sdr_firmware/fpga/README.md),
and the checked-in constraints
[`ddc_sdr.pcf`](Software/ddc_sdr_firmware/fpga/ddc_sdr.pcf). Resolve conflicts
between those documents, the schematic, and the netlist in Week 1 before
writing datapath RTL.

## How Students Work

For every lab, students answer these questions before asking AI for RTL:

1. What should the circuit do on each clock edge?
2. What should happen during reset and for bad input?
3. What simple example proves that it works?
4. What believable bug should the testbench catch?

The weekly submission then contains:

1. A short specification, including widths, signedness, timing, and reset.
2. A self-checking SystemVerilog testbench with useful failure messages.
3. An independent reference calculation or golden vector when numbers are
   involved.
4. RTL, the command used to run it, and the test result.
5. One intentionally broken version that the testbench catches.
6. A short AI-use note naming one generated mistake the students corrected.

A waveform screenshot can explain a failure, but it is not evidence by itself.
A test passes only when the checker would fail for a plausible implementation
error. For DSP numbers, students should use a Python calculation or
hand-derived vectors rather than copying the RTL algorithm into the checker.

## Weeks 1-2: Start Here

### Week 1: Board, Schematic, and Clock Budget

**Goal:** understand the physical board and make the toolchain work.

Students install the OSS CAD Suite, inspect the KiCad schematic, identify the
SPI, I2S, ADC, DAC, interrupt, and PGA pins, and trace one sample through the
planned signal path. They calculate:

- 30.720 MHz / 640 = 48 kHz;
- 48 kHz * 64 = 3.072 MHz I2S bit clock;
- two 32-bit I2S slots with 24 valid bits each.

**Hardware payoff:** connect the board, verify that the Pico boots, identify
the FPGA status LED, and run the smallest known-good FPGA bitstream. This is a
power and bring-up check, not an RF experiment.

**Evidence:** a one-page annotated block diagram, pin table, clock budget,
and a successful toolchain/board bring-up log.

### Week 2: Traffic Light and AI Verification

**Goal:** learn the edit-simulate-explain loop with very small HDL.

AI writes a traffic-light state machine for the RGB LEDs. Students write the
testbench first, then ask AI for the RTL. The testbench must catch an incorrect
reset state and an off-by-one timer.

The timer must be parameterized. The testbench uses a tiny divider so it runs
quickly; the hardware build uses a divider that gives an approximately
one-second interval. This teaches students not to simulate a full second for
every test.

**Hardware payoff:** flash the FPGA and watch the physical LEDs change.

**Evidence:** a specification, self-checking testbench, waveform showing the
state transitions, a caught broken version, and an AI-use note.

## Weeks 3-10: DDC Project

### Week 3: I2S Audio Transmitter

**Goal:** learn clock division, shift registers, and serial timing.

AI generates a simple FPGA-master I2S transmitter. Start with two fixed sample
values, then try a repeating sawtooth sequence. The testbench checks the exact
relationship between BCK, WS, and data: 48 kHz WS, 3.072 MHz BCK, two 32-bit
slots, 24 valid MSB-first bits, and the standard one-bit I2S data delay.

Do not begin by trying to make the sound impressive. First prove that a logic
analyzer or testbench can reconstruct the two known samples from the bitstream.

**Hardware payoff:** connect the FPGA I2S output to the existing Pico firmware
and observe captured samples on the host. Hearing a repeating test pattern is a
good bonus, but a logic-analyzer capture is the required hardware evidence.

**Evidence:** a timing diagram, self-checking serializer testbench, and a
captured or simulated reconstruction of both channels.

### Week 4: SPI Command Parser

**Goal:** receive serial commands and update a register safely.

AI generates an SPI slave and command parser for the versioned frame documented
in [`ddc_protocol.h`](Software/ddc_sdr_firmware/ddc_protocol.h):
`D5 01 command 04 value[31:0]` in little-endian order. Begin with one harmless
test register. Then add frequency, sample-rate, and OTR-clear commands.

The first version works in one clock domain. Only after that test passes should
students add the CDC mailbox or handshake that transfers a complete command to
the 30.720 MHz domain. Tests cover bad sync/version/length fields, aborted chip
select, back-to-back frames, and randomized clock phase.

**Hardware payoff:** send commands from the Pico and observe a test register,
LED, or logic-analyzer output change. The current public protocol does not
directly command the PGA pins; those pins are controlled by the Pico's OTR/AGC
path. Do not promise a PGA toggle for an SPI-parser lab until that command is
explicitly added to the protocol.

**Evidence:** parser tests that reject malformed frames, commit valid frames
exactly once, and never expose a half-updated multi-bit value.

### Week 5: ADC Capture and OTR Latch

**Goal:** capture the fast ADC bus and make clipping visible to the Pico.

Students capture `adc_data[7:0]` in the 30.720 MHz domain and implement the OTR
event contract. The testbench uses OTR pulses aligned to the ADC clock, then
checks that the FPGA interrupt remains high until a valid clear command. It
covers zero, full-scale, alternating, ramp, and PRBS ADC values; reset during
an event; repeated events; and clear/retrigger behavior.

The OTR input is described as a roughly 32 ns event because it is one sample
clock period. A synchronous design cannot guarantee to see an arbitrary pulse
that occurs entirely between clock edges. If the real signal is asynchronous,
students must state and test an asynchronous capture strategy instead.

The interrupt is deliberately sticky rather than a fixed one-millisecond
pulse. This gives the Pico time to respond and matches the existing firmware
contract: `fpga_int` stays high until `DDC_FPGA_CMD_CLEAR_OTR` is accepted.

**Hardware payoff:** use a safe, current-limited signal source or an instructor
test fixture to exercise the OTR path. Never touch the ADC input to create
static; that can damage the analog front end. The Pico should report the
interrupt and step through the verified PGA masks `0x0 -> 0x1 -> 0x3 -> 0xf`.

**Evidence:** ADC capture tests, OTR timing evidence, and a hardware interrupt
log. A complete level-regulating AGC is an extension.

### Week 6: NCO

**Goal:** introduce DSP inside a familiar clocked module.

AI generates a 32-bit phase accumulator and a sine/cosine lookup or equivalent
approximation. Students choose a tuning word, predict the frequency, save the
simulated output, and plot it in Python or another approved tool. The testbench
checks phase wraparound, zero and positive/negative frequencies, lookup-table
quantization, and signed extremes.

**Hardware payoff:** after the digital simulation passes, route the NCO to the
FPGA DAC interface and observe a safe test signal at `AWG_OUT` with an
oscilloscope. Begin with a low-risk configuration and verify the DAC clock,
data coding, termination, and analog output path before interpreting the
waveform.

**Evidence:** independent frequency calculation, plot, simulation test, and
optional oscilloscope capture.

### Week 7: Digital Mixer

**Goal:** learn signed fixed-point multiplication and complex mixing.

Students mix a known ADC tone with the NCO output. Use a frequency choice that
is easy to predict, such as a 7 MHz input and a 7 MHz NCO, and verify that the
desired product produces a DC or near-DC baseband component. Also test a
nonzero difference frequency and the unwanted sum component.

The reference must define signed widths, multiplication width, truncation or
rounding, and I/Q sign convention. Tests must include positive and negative
values, zero, maximum positive, and maximum negative values.

**Hardware payoff:** a plotted simulation result is the main demonstration.
Students should be able to point to the DC result and explain why it appears;
an RF hardware demonstration is optional.

**Evidence:** signed-arithmetic reference calculations, mixer testbench, and
plots showing the expected difference and sum frequencies.

### Week 8: CIC Decimator

**Goal:** handle multirate DSP, bit growth, and output-valid timing.

Students design a three-stage CIC decimator with `R=640`. Before writing RTL,
they calculate the maximum gain, register growth, scaling, latency, and
decimation phase. For a three-stage filter, the gain is $R^3=640^3$ and the
decimation gain requires about 28 additional bits because
$\lceil\log_2(640^3)\rceil=28$; the final width also depends on the input and
mixer widths. The testbench, not an AI guess, decides whether the chosen width
is sufficient.

Tests cover impulse, step, constant input, tones inside and outside the
passband, reset, signed extremes, wraparound behavior, and output-valid
alignment. Students may use a deliberately broken width calculation to learn
from the failure, but they should not rely on uncontrolled overflow as the
lesson.

**Hardware payoff:** none is required. This is the difficult verification week
and should remain focused.

**Evidence:** bit-growth calculation, independent model, self-checking tests,
and a written explanation of the chosen arithmetic behavior.

### Week 9: System Integration

**Goal:** connect blocks that have already been tested separately.

Students integrate ADC capture, NCO, mixer, CIC, I2S, SPI control, reset, and
OTR notification. The top-level testbench drives deterministic ADC tones and
impulses, changes frequency through the SPI model, and checks the resulting
I/Q audio samples and I2S framing.

The integration test must include at least one negative test for each control
boundary: invalid command, reset during traffic, OTR during a sample transfer,
and a frequency update near a decimator output.

**Hardware payoff:** load the complete bitstream and verify clocks, I2S, and
status signals. Use a signal generator or a known test fixture before trying an
antenna. The board is alive when the interface evidence is good; an unknown RF
environment is not a valid test failure.

**Evidence:** top-level regression, packed bitstream, and a short block-by-block
debugging record for any integration failure.

### Week 10: AGC and Radio Demonstration

**Goal:** finish the hardware/software handshake and demonstrate the receiver.

The required part is a controlled OTR/AGC demonstration using the existing Pico
firmware: the Pico receives the FPGA interrupt, advances through the verified
PGA masks, sends the OTR-clear command, and keeps the audio path running. The
current implementation is C firmware; a MicroPython AGC loop can be an
extension after the C path is understood and tested.

**Hardware payoff:** with a safe signal source, students observe the gain
state changes and clipping recovery. If the analog chain, antenna, and local
conditions are ready, they may try SDR++ or a shortwave broadcast. Live radio
reception is a celebration and extension, not the passing criterion.

**Evidence:** AGC state trace, OTR-clear transaction, final regression, and a
short hardware demonstration or simulation substitute.

The 96 kHz mode, transmit path, and other clocking extensions remain optional.
The handoff notes that 30.720 MHz does not directly provide an integer-divided
6.144 MHz I2S bit clock, so 96 kHz requires its own explicit clocking design.

## Weekly Teach-Back

Each pair has a short weekly discussion with the instructor. Both students
must participate. Each student answers:

- a signal-tracing question: what happens on a named edge or transaction;
- a prediction question: what will the next output be and why;
- a verification question: which test would catch a plausible bug;
- a reflection question: what did the AI get wrong or leave unjustified?

The pair brings its specification, evidence, one failure analysis, and current
risk list. A failed checkpoint can be redone once. The original failure and
the diagnosis remain part of the record.

## Assessment

- 20% weekly artifacts, progress, teach-backs, and quality of any redo.
- 30% verification quality: independence of the model, edge cases, assertions,
  failure messages, and regression discipline.
- 40% individual understanding and oral defense.
- 10% engineering process: version control, documentation, reproducibility,
  and honest AI-use log.

A passing project requires a working simulated 48 kHz baseline, self-checking
verification, and successful synthesis. Hardware evidence increases
confidence but does not replace simulation or penalize students while boards
are unavailable.

## Extensions

Extensions should be proposed with a written specification and a verification
plan before implementation. Suitable topics include:

- validated 96 kHz output;
- a measured level-regulating AGC loop;
- transmit DUC and DAC/I2S input;
- VNA or AWG support;
- stronger status reporting and counters;
- formal properties for the SPI parser, CDC handshake, or I2S serializer;
- real-radio or filter-sandbox hardware experiments.

TX, VNA, AWG, full AGC, and real RF reception are intentionally extensions:
they are valuable demonstrations but depend more heavily on analog hardware,
clocking, and board availability than the common HDL learning objectives.
DFU and stored FPGA images are interchangeable deployment choices for
development and transceiver profiles, but a VNA proposal must keep one
coherent FPGA design loaded for the whole sweep rather than switching RX/TX
CRAM images between points.

## Implementation Order for This Repository

The next practical build-out should follow the weekly sequence:

1. Add a traffic-light starter design, broken variant, and self-checking
   SystemVerilog testbench.
2. Add a small simulator command or Makefile target that students can run
   from the repository root.
3. Add one lab directory at a time under
   `Software/ddc_sdr_firmware/fpga/labs/`, keeping each lab's specification,
   RTL, testbench, and reference data together.
4. Keep the top-level names in `ddc_sdr.pcf` and the SPI frames in
   `ddc_protocol.h` stable while the internal RTL evolves.
5. Add hardware smoke tests alongside the simulation regression, and never use
   an unexplained hardware failure as a substitute for a missing testbench.

This order gives the class a usable verification exercise immediately and
keeps unfinished student HDL out of the production firmware build.