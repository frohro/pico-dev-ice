# USB audio debugging: evening of 3 November 2026

## Where we left off

The four-slot I2S capture ring and occupancy-based USB packet sizing are in
place, but **the intermittent USB failures and audio pops are not fixed**.
One definite source of lost samples was removed: TinyUSB's audio IN flow
control is now disabled so it does not compete with the firmware's own
packet-size decisions. The remaining symptoms need more investigation.

The device under test (DUT) and a second Pico acting as a CMSIS-DAP debug
probe are connected to the second Linux computer through a USB 2.0 hub. At
the end of the session, the DUT was restored to the earlier symbol-enabled
**normal capture firmware with the sawtooth FPGA image**, not the
Pico-generated USB-only diagnostic. Flash contents were verified during
programming, and CDC responded to `VER` afterward. The image is useful for
testing, but it is not known to be reliable.

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

The separate user-supplied `flat_response.bin` was not committed. Its
SHA-256 was `499cce45de23d502deab5d74b35c37395fdf68a8bd2c7de5fc17272d8659a963`.
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
