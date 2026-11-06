# USB audio debugging: 3-5 November 2026

## Where we left off

The four-slot I2S capture ring and occupancy-based USB packet sizing are in
place, but **the intermittent USB failures and audio pops are not fixed**.
One definite source of lost samples was removed: TinyUSB's audio IN flow
control is now disabled so it does not compete with the firmware's own
packet-size decisions. The remaining symptoms need more investigation.

At the end of the 3 November session, the device under test (DUT) and a
second Pico acting as a CMSIS-DAP debug probe were connected to the second
Linux computer through a USB 2.0 hub. The DUT was restored to the earlier
symbol-enabled **normal capture firmware with the sawtooth FPGA image**, not the
Pico-generated USB-only diagnostic. Flash contents were verified during
programming, and CDC responded to `VER` afterward. The image is useful for
testing, but it is not known to be reliable.

On 4 November the YD-RP2040 was replaced with a Pico W for remote tests
without a debug probe. On 5 November a separate school test used a
Chinese Pico clone. The present physical configuration should be confirmed
before resuming; do not assume the probe is still connected.

## Background

The desired capture is FPGA I2S to Pico to USB host: 48 kHz, stereo,
24-bit packed little-endian (`S24_3LE`), or six bytes per stereo frame.
The FPGA supplies the I2S clocks. USB capture is asynchronous: the host's
millisecond frames and FPGA's sample clock are not necessarily identical.
The firmware also retains a 96 kHz alternate setting.

Commit `6092583` replaced the old rigid ping-pong buffers with four
one-millisecond DMA slots. At 48 kHz, the firmware targets 96 frames of
occupancy and chooses 47, 48, or 49 frames per USB packet. It selects 48
frames in the 86-106 frame deadband; below that it selects 47 and above it
49. The ring implementation and its recovery paths still need end-to-end
validation.

Symptoms included:

* `arecord` sometimes fails while installing hardware parameters; other
  attempts start and later end with an input/output error.
* CDC commands may stop responding after capture is attempted, although they
  can work again after reset.
* Successful sawtooth recordings contain audible pops and missing samples.
  Both the older firmware and the ring firmware showed intermittent setup
  failures, so the ring alone cannot explain every symptom.

## What we found

### Two packet-size controllers were fighting

In the TinyUSB revision used for these tests, the default audio IN flow
control adjusts packet length based on its FIFO. The application already
chooses packet lengths from capture-ring occupancy. TinyUSB could therefore
send fewer bytes than the application prepared, and the application's FIFO
clearing discarded the remainder.

One earlier ten-second sawtooth recording had 927 discontinuities, mostly
single-sample skips; many were spaced exactly 527 frames apart. After
setting `CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL` to `0` in
[`tusb_config.h`](tusb_config.h), a ten-second recording had 10
discontinuities and the regular single-sample pattern disappeared. These
were separate recordings, not a controlled statistical comparison, but
the code explains the competing-controller loss mechanism. The remaining
gaps were mostly about 48 frames, so disabling flow control is **not** a
complete fix.

The tested TinyUSB checkout was `53fef2833`, described as
`0.21.0-185-g53fef2833`, with a pre-existing local modification to
`hw/bsp/family_support.cmake`. Preserve that modification and use a
TinyUSB checkout with the UAC1 support required by this firmware.

### Some pops correspond to failed USB packets

After a clean reset, six four-second captures on the first host contained
23 waveform discontinuities, with 23 corresponding isochronous packet
errors reported by the host as `-71` (`EPROTO`). Some hub traces also
showed partial packets. These observations locate those losses at the USB
transport boundary; they do not prove why the packets failed, or explain
every possible capture-ring loss.

The DUT's CPU was observed executing firmware after one failure, rather than
remaining in a hard fault. That snapshot does not establish the failure's
origin or prove that no other lockup is possible.

### Isolating the capture path and the FPGA

We made a **temporary diagnostic**, separate from the repository firmware:
the Pico generated its own 48-frame sawtooth in the USB packet-fill path
and did not start I2S capture PIO/DMA. The FPGA was still programmed
normally. SWD confirmed capture DMA was inactive.

| Experiment | Observation | What it tells us |
| --- | --- | --- |
| Normal capture, replacement cable and USB hub | Errors and gaps still occurred. | Neither change alone eliminated the problem. |
| Pico-generated USB sawtooth, sawtooth FPGA running, second Linux host | Six four-second captures had 44 `-71` packet errors and 40 observed 48-frame gaps. | Capture PIO/DMA and the capture ring are not necessary for those USB errors. |
| Same USB-only diagnostic, FPGA held in reset | Six four-second captures had no waveform discontinuities and no `-71` packet errors; CDC responded. | Running the FPGA is strongly associated with the failures in this comparison. |
| Pico-generated USB sawtooth, `flat_response.bin` FPGA running | Six four-second captures had no waveform discontinuities and no `-71` packet errors. | The flat-response image behaved differently in this limited test; the Pico, not this FPGA image, generated the test waveform. |
| Sawtooth FPGA running; GPIO0 rising-edge interrupt disabled from startup | The first capture setup still stalled with `-32` (`EPIPE`), and CDC did not respond. | The GPIO0 interrupt is not required for this setup/CDC failure. This does not rule it out for every other symptom. |

Holding the FPGA in reset changes configuration, output levels, switching
activity, and power at the same time. It does **not** establish whether
the cause is an FPGA pin, electrical interference, power, firmware state,
or something else. Likewise, the flat-response comparison does not prove
which difference between the bitstreams matters.

In particular, the USB-only diagnostic did not consume I2S via capture
PIO/DMA, but the FPGA was still connected to the Pico's pins. Consequently
the results are consistent with an effect from active FPGA outputs such as
BCK, WS, or data, even when software is not receiving them. We did not
isolate or measure those individual signals, so this is a hypothesis, not a
demonstrated cause. FPGA reset also changes other signals and switching.

The user noted that `flat_response.bin` is an SDR bitstream, does not
produce the sawtooth, and could drive OTR. That is compatible with the
USB-only experiment because its sawtooth was generated on the Pico. The
user-supplied sawtooth source assigns `fpga_int` low, but its comments and
the checked-in FPGA pin constraints differ; do not assume the actual
programmed bitstream's pin behavior without verifying its build and pins.
GPIO0 was low in individual SWD snapshots; a snapshot cannot exclude
intermittent changes.

### Interface stalls and CDC need separate investigation

After reloading the sawtooth FPGA via `FPGA,LOAD,RX`, capture setup
stalled and CDC stopped responding. Holding the FPGA in reset afterward
did not immediately restore capture setup. A later trace with the GPIO
interrupt disabled from startup showed `SET_INTERFACE`, interface 3,
alternate setting 1, completing with `-32`. At a subsequent SWD snapshot,
`fpga_ready` was true, the sample rate was 48000, and the CPU was still
executing the main loop. These are observations after the failure, not
proof of the exact point where the request was rejected.

The `-32` control stall and the `-71` packet errors may be related, but
that has not been demonstrated. Trace the first failure in TinyUSB's
interface/endpoint setup and application callback, and examine CDC
progress, before treating them as one cause.

## Subsequent board and power comparisons

### Pico W: remote tests on 4 November

The same normal symbol-enabled sawtooth firmware used in the earlier tests
was programmed and flash-verified on the Pico W. No debug probe was
connected. Six four-second captures and two 30-second captures all
completed: 84 seconds total. Their sawtooth checks found zero skipped or
repeated samples after the expected initial silence. USB traces showed
84,000 successful nonempty audio packets and no `-71` protocol errors.
Fourteen of those packets had 49 frames, consistent with rate modulation.

We then ran a 60-second capture while issuing 300 CDC commands (`VER`,
`AUDIO,STATS`, and `FPGA,STATUS`) over approximately 46 seconds. Every
command received the expected response prefix and `OK`. Fifty further `VER` commands
after capture also succeeded. That recording had no sawtooth discontinuities,
the USB trace had 60,000 successful nonempty audio packets and no `-71`
errors, and capture statistics reported no underruns or overruns.

These are encouraging observations for this Pico W and setup, not proof
that every Pico W or every longer run will be error-free. They do not
isolate which board-level difference matters.

### User measurements and school test

The user measured approximately 3.19 V on the YD-RP2040 and 3.30 V on the
Pico W. The next day i measured 3.3V on another YD-RP2040 unit in a different Pico iCE-Device.  Connecting a second YD-RP2040's 3.3 V output raised the reading to
3.29 V. That direct parallel-regulator arrangement is **not a recommended
test setup**: independently powered regulator outputs can backfeed or
contend unless their design explicitly supports it. A steady DC reading
does not rule out transients, and no controlled packet test of that
parallel arrangement was performed.

On 5 November, the user powered the SDR circuitry from a separate SGM2205
3.3 V regulator while the YD-RP2040 continued to use its own LDO. The
problem persisted. The user then substituted a Chinese Pico clone and
reported that audio had no missing packets and CDC and UAC1 stayed
responsive, without making any change to GPIO23. These school observations
were reported by the user; no USB trace or capture duration was supplied.

The user also measured 160 mA at the Pico Dev-iCE's 5 V USB input. The
earlier 225 mA reading included the USB hub and debug probe. The 160 mA is
the total current at that 5 V input under the measured conditions, not
the current through the YD's 3.3 V regulator or an instantaneous peak.
The separate-SDR-supply result weakens the idea that the SDR load alone
overloaded the YD regulator, but it does not rule out YD-side voltage
ripple, transient behavior, grounding, or another board difference.

### Schematic comparison and testable hypotheses

The supplied [YD-RP2040 2022-V1.1 schematic][yd-schematic] and
[original Raspberry Pi Pico schematic][pico-schematic] show:

* **Regulation:** the YD schematic shows an ME6215C33M5G linear regulator;
  the original Pico schematic shows an RT6150B-33GQW switching regulator.
  Their response to load changes and noise may differ, but the schematic
  does not measure either rail in the assembled SDR.
* **USB:** both schematics show 27-ohm series resistors in D+ and D-. This
  shared nominal value does not eliminate differences in actual components,
  connector, PCB routing, grounding, USB signal integrity, or clock accuracy.
* **Clock:** both show a 12 MHz crystal and a 1-kilohm series resistor.
  The YD schematic shows 10 pF crystal capacitors; the original Pico
  schematic shows 15 pF. Crystal specifications and board parasitics also
  matter, so the capacitor difference alone does not establish a frequency
  error. The earlier approximately 48 MHz USB PLL measurement was relative
  to the Pico's own reference and did not verify absolute clock accuracy.
* **GPIO23 and the module header:** on the YD schematic GPIO23 drives an
  onboard WS2812B-style RGB LED circuit and is brought to header pin 37.
  On the original Pico, RP2040 GPIO23 controls regulator power-save mode,
  whereas header pin 37 is `3V3_EN`. These are not equivalent connections.
  The checked-in [carrier schematic](../../Hardware/Pico-Dev-iCE.pdf)
  marks pin 37 unconnected and notes a USB/VSYS jumper setting for the YD;
  verify the actual assembled carrier and jumper position rather than
  assuming they match that revision. The Pico W has its own board design,
  and neither the supplied original-Pico schematic nor the YD schematic
  establishes its internal wiring. GPIO23 has not been proven causal.

Neither schematic establishes the manufactured board's exact components,
layout, soldering, or power integrity. To distinguish these hypotheses,
repeat the USB-only YD test with a controlled FPGA diagnostic image that
keeps the FPGA configured while independently holding or toggling the
I2S outputs, comparing identical captures and CDC traffic. Change one
signal at a time where practical; also account for the FPGA's own
switching activity. Verify actual pin mappings before modifying gateware.
Separately, measure the YD's 3.3 V rail and relevant ground points under
load with suitable instrumentation, and compare the USB clock against an
independent reference. Avoid direct parallel regulator outputs.

[yd-schematic]: https://github.com/initdc/YD-RP2040/blob/master/YD-2040-2022-V1.1-SCH.pdf
[pico-schematic]: https://deepbluembedded.com/wp-content/uploads/2023/10/Raspberry-Pi-Pico-Schematic-Diagram.png

## Remote debugging notes

Remote access via SSH worked well: the device host ran `arecord`,
`tcpdump` on `usbmon`, `picotool`, and OpenOCD, while recordings and USB
traces were analyzed from the other machine. The debug probe provided SWD
register and state inspection. These methods did not require moving the
hardware between computers.

Useful precautions when resuming:

1. Check the current audio card and USB device numbers with `arecord -l`
   and `lsusb`; they can change after re-enumeration. Verify which
   `/dev/ttyACM*` is the DUT and which is the probe before opening either.
2. Resetting or reflashing can recreate `/dev/snd/*` nodes and remove
   temporary ACLs. If necessary, grant the remote user access to the
   *currently identified* control and capture nodes; do not assume card 1.
   USB monitor and probe nodes also require appropriate permissions.
3. Use a matching ELF for SWD symbol addresses. A debug image and its
   corresponding UF2/ELF must come from the same build. The session-only
   USB diagnostic was rebuilt multiple times, so an older UF2 must not be
   paired with the latest diagnostic ELF.
4. `picotool load -v -x` successfully programmed and verified the DUT
   through BOOTSEL remotely. A CDC `BOOTSEL` command can enter that mode
   while CDC is responsive. If CDC is unresponsive, a debug-probe reset
   may restore it; otherwise use the physical BOOTSEL procedure. Verify
   the target before programming and restore normal firmware after a
   diagnostic.
5. USB captures with `tcpdump -i usbmon1 -U -w -` worked when live
   `tshark` capture had permission problems. Analyze completed isochronous
   URBs, not submission placeholders; `-104` at capture shutdown can be
   cancellation rather than corruption. USB traces and WAVs may contain
   captured data; handle them appropriately.

The earlier OpenOCD could attach but did not recognize the DUT's flash
for SWD programming. A newer local OpenOCD was built but its flash support
was not verified. BOOTSEL programming with `picotool` was verified instead.

## Reproducing and continuing

The normal build uses [`build.sh`](build.sh), which prefers `~/tinyusb`
when present and embeds [`bitstreams/ddc_sdr_rx.bin`](bitstreams/ddc_sdr_rx.bin).
At the time of this investigation that tracked bitstream had SHA-256:

```text
95cedcd9a762345bd8e417cf24860de4094820f35e7e1e2f235b296cf184ad68
```

The separate user-supplied [`flat_response.bin`](flat_response.bin) is now
tracked. Its SHA-256 was
`499cce45de23d502deab5d74b35c37395fdf68a8bd2c7de5fc17272d8659a963`.
Do not silently replace the tracked bitstream when comparing images.

For a normal sawtooth capture, substituting the current ALSA card number:

```sh
arecord -D hw:CARD,0 -f S24_3LE -r 48000 -c 2 -d 10 /tmp/sawtooth.wav
python3 tools/check_sawtooth.py /tmp/sawtooth.wav
```

`CARD` is a placeholder, not a literal device name. The checker expects
the particular incrementing sawtooth test waveform; it is not meaningful
for the flat-response SDR output. It excludes initial silence when
counting discontinuities.

The USB-only diagnostic sources, builds, traces and recordings were
session-local, not committed. To recreate the isolation experiment, use a
separate diagnostic build that produces a known stereo sawtooth directly
in the USB packet-fill path and does not start capture PIO/DMA; verify
those conditions with SWD. Keep it separate from normal SDR firmware.

Suggested next steps:

1. Instrument or debug the `SET_INTERFACE` failure to identify the exact
   failing check or endpoint operation, without assuming the error seen
   after the fact is its root cause. Capture the matching USB trace and
   firmware state, and check CDC servicing.
2. Repeat controlled FPGA-running versus reset and bitstream comparisons,
   with repeated resets and longer captures. Establish actual GPIO/OTR
   behavior and inspect FPGA pin mappings, power and USB signals as
   evidence warrants. Do not equate correlation with a proven electrical
   cause.
3. Separately review and test DMA stop/abort and ISR ordering, USB
   completion callbacks, ring head/tail accounting, startup and recovery.
   RP2040 DMA abort can produce a spurious completion interrupt; whether
   that affects this firmware has not been demonstrated.
4. Validate any fix with repeated capture start/stop cycles, concurrent
   CDC commands, continuous sawtooth checks, USB packet statuses, and the
   supported sample rates. Keep normal and diagnostic results distinct.

The packet-rate boundary test can be run on the host:

```sh
gcc -std=c11 -Wall -Wextra -Werror tools/test_audio_rate_ctrl.c -o /tmp/ddc-audio-rate-test
/tmp/ddc-audio-rate-test
```
