#!/usr/bin/env bash
# drop_tests.sh - Flash the Pico (Philips I2S firmware) + FPGA test bitstream,
# then run repeated sawtooth captures and count dropped USB packets.
#
# Run from the repo root (or set REPO=/path/to/repo).
#
# Usage: ./drop_tests.sh [options]
#   --no-build        skip ./build.sh, use the existing build/ddc_sdr.uf2
#   --no-flash        skip BOOTSEL/UF2/DFU, just run the capture tests
#   --runs N          number of captures (default 3)
#   --dur SECONDS     length of each capture (default 20)
#   --uf2 FILE        UF2 to flash (default: build/ddc_sdr.uf2)
#   --bin FILE        FPGA bitstream (default: ENGR433-Solutions/Lab_03/i2s_test.bin)
#   --sof-wd 0|1      patch main.c so the SOF watchdog (usb_sof_watchdog) is off (0) or on (1)
#                     before building; also adds its recovery count to the DEBUG output
#
# Env overrides: REPO, VENV, MOUNT, CARD (e.g. hw:2,0)
#
# A dropout = one lost 288 B USB packet = 48 stereo frames, which shows up in
# the sawtooth as a step of 49 * 0x10000 instead of 0x10000 (see debugging_drops.md).

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -z "${REPO:-}" ]; then
    # Works from the repo root or from inside the firmware directory.
    REPO="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$SCRIPT_DIR")"
fi
VENV="${VENV:-/home/frohro/Classes/Digital_Design/pico-dev-ice/.venv}"
if [ -f "$SCRIPT_DIR/build.sh" ] && [ -f "$SCRIPT_DIR/tools/dfu_fpga.py" ]; then
    FW_DIR="$SCRIPT_DIR"
else
    FW_DIR="$REPO/Software/ddc_sdr_firmware_usb_working_philips"
fi
UF2="$FW_DIR/build/ddc_sdr.uf2"
BIN="$REPO/ENGR433-Solutions/Lab_03/i2s_test.bin"
MOUNT="${MOUNT:-/run/media/${USER:-frohro}/RPI-RP2}"
WAV=/tmp/saw10.wav
RUNS=3
DUR=20
DO_BUILD=1
DO_FLASH=1
SOF_WD=""

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) DO_BUILD=0 ;;
        --no-flash) DO_FLASH=0 ;;
        --runs) RUNS="$2"; shift ;;
        --dur)  DUR="$2"; shift ;;
        --uf2)  UF2="$2"; shift ;;
        --bin)  BIN="$2"; shift ;;
        --sof-wd) SOF_WD="$2"; shift ;;
        -h|--help) sed -n '2,24p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
    shift
done

if [ -f "$VENV/bin/activate" ]; then
    # shellcheck disable=SC1091
    source "$VENV/bin/activate"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

log() { printf '\n=== %s ===\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------- helpers ----
# CDC command helper: cdc.py CMD [wait_seconds]; prints reply, exit 1 if empty.
cat > "$TMP/cdc.py" <<'PY'
import glob, sys, time
import serial

cmd = sys.argv[1]
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 1.5
ports = glob.glob('/dev/serial/by-id/*DDC_SDR*')
if not ports:
    sys.exit(2)
try:
    s = serial.Serial(ports[0], 115200, timeout=0.3)
except Exception as e:
    print("open failed:", e)
    sys.exit(2)
s.reset_input_buffer()
s.write((cmd + "\r").encode())
end = time.time() + wait
buf = b""
while time.time() < end:
    chunk = s.read(512)
    if chunk:
        buf += chunk
        end = max(min(end, time.time() + 0.5), time.time() + 0.1) if b"OK" in buf else end
s.close()
out = buf.decode(errors="replace").strip()
print(out)
sys.exit(0 if out else 1)
PY

# WAV analyzer: counts sawtooth discontinuities (dropped packets).
cat > "$TMP/analyze.py" <<'PY'
import struct, sys
from collections import Counter

STEP = 0x010000
MASK = 0xFFFFFF
FRAMES_PER_PKT = 48          # 288 B / 6 B per stereo frame
RATE = 48000

b = open(sys.argv[1], 'rb').read()
i, data = 12, None
while i + 8 <= len(b):
    cid = b[i:i+4]
    sz = struct.unpack('<I', b[i+4:i+8])[0]
    if cid == b'data':
        data = b[i+8:i+8+sz] if sz != 0xFFFFFFFF else b[i+8:]
        break
    i += 8 + sz + (sz & 1)
if data is None:
    print("RESULT drops=-1 frames=0 lr_mismatch=0 note=no_data_chunk")
    sys.exit(1)

n = len(data) // 6
data = data[:n*6]
prev = None
drops = []
lost = Counter()
odd = 0
lr_bad = 0
for f in range(n):
    o = f * 6
    L = data[o] | (data[o+1] << 8) | (data[o+2] << 16)
    R = data[o+3] | (data[o+4] << 8) | (data[o+5] << 16)
    if L != R:
        lr_bad += 1
    if prev is not None:
        d = (L - prev) & MASK
        if d != STEP:
            drops.append(f)
            if d % STEP == 0:
                lost[d // STEP - 1] += 1       # frames lost
            else:
                odd += 1
    prev = L

secs = n / RATE
print("frames=%d (%.2f s)" % (n, secs))
print("discontinuities=%d  (non-multiple-of-step: %d)" % (len(drops), odd))
if lost:
    print("frames lost per event (n x frames): " +
          ", ".join("%dx%d" % (c, k) for k, c in sorted(lost.items())))
    print("  (48 frames = exactly one 288 B packet)")
if drops:
    print("first drops at (s): " + ", ".join("%.3f" % (f / RATE) for f in drops[:8]))
    if len(drops) > 1:
        iv = [(drops[k+1] - drops[k]) / RATE for k in range(len(drops) - 1)]
        print("mean interval between drops: %.3f s" % (sum(iv) / len(iv)))
if secs > 0:
    print("drop rate: %.2f /s" % (len(drops) / secs))
print("L!=R frames: %d" % lr_bad)
print("RESULT drops=%d frames=%d lr_mismatch=%d" % (len(drops), n, lr_bad))
PY

wait_for_cdc() {   # wait up to $1 s for the CDC port to answer VER
    local t=$(( $(date +%s) + $1 ))
    while [ "$(date +%s)" -lt "$t" ]; do
        if python3 "$TMP/cdc.py" VER 1.5 >/dev/null 2>&1; then return 0; fi
        sleep 1
    done
    return 1
}

# ------------------------------------------------------------------ build ----
if [ "$DO_FLASH" = 1 ]; then
    if [ "$DO_BUILD" = 1 ] && [ -n "$SOF_WD" ]; then
        log "Setting SOF watchdog = $SOF_WD in main.c"
        python3 - "$FW_DIR/main.c" "$SOF_WD" <<'PY' || die "could not patch main.c"
import re, shutil, sys
path, val = sys.argv[1], sys.argv[2]
if val not in ("0", "1"):
    sys.exit("--sof-wd must be 0 or 1")
t = open(path).read()
if "ENABLE_SOF_WD" not in t:
    shutil.copy(path, path + ".orig")
    edits = [
        ("#define ENABLE_SAMPLER 0\n",
         "#define ENABLE_SAMPLER 0\n#define ENABLE_SOF_WD 1\nextern volatile uint32_t dbg_sof_recoveries;\n"),
        ("        usb_sof_watchdog();\n",
         "        if (ENABLE_SOF_WD) usb_sof_watchdog();\n"),
        (r'"DBG4: gap<500=%lu gap>1500=%lu gap>2500=%lu gapmax=%lu\r\nOK\r\n",',
         r'"DBG4: gap<500=%lu gap>1500=%lu gap>2500=%lu gapmax=%lu sofrec=%lu\r\nOK\r\n",'),
        ("(unsigned long)dbg_gap_gt2500, (unsigned long)dbg_gap_max);",
         "(unsigned long)dbg_gap_gt2500, (unsigned long)dbg_gap_max,\n"
         "                 (unsigned long)dbg_sof_recoveries);"),
    ]
    for old, new in edits:
        if t.count(old) != 1:
            sys.exit("main.c does not match what I expected near: " + old.strip()[:50])
        t = t.replace(old, new)
t = re.sub(r"#define ENABLE_SOF_WD \d", "#define ENABLE_SOF_WD " + val, t)
open(path, "w").write(t)
print("main.c: ENABLE_SOF_WD =", val, "(original saved as main.c.orig)")
PY
    fi
    if [ "$DO_BUILD" = 1 ]; then
        log "Building firmware"
        ( cd "$FW_DIR" && ./build.sh ) || die "firmware build failed"
    fi
    [ -f "$UF2" ] || die "UF2 not found: $UF2"
    [ -f "$BIN" ] || die "bitstream not found: $BIN"

    # ------------------------------------------------------------- BOOTSEL ----
    log "Rebooting Pico into BOOTSEL"
    if [ ! -d "$MOUNT" ]; then
        if python3 "$TMP/cdc.py" BOOTSEL 2 >/dev/null 2>&1; then
            echo "Sent BOOTSEL over CDC"
        elif command -v picotool >/dev/null 2>&1 && picotool reboot -f -u >/dev/null 2>&1; then
            echo "Rebooted via picotool"
        else
            echo "CDC and picotool BOOTSEL failed (firmware wedged?)."
            echo "Use SWD: /tmp/bootsel.tcl via OpenOCD, or hold BOOTSEL and replug."
        fi
        for _ in $(seq 1 30); do
            [ -d "$MOUNT" ] && break
            sleep 1
        done
    fi
    [ -d "$MOUNT" ] || die "RPI-RP2 not mounted at $MOUNT (set MOUNT=... if different)"

    # ---------------------------------------------------------------- UF2 ----
    log "Copying UF2"
    cp "$UF2" "$MOUNT/" && sync || die "UF2 copy failed"
    sleep 8
    wait_for_cdc 20 || die "Pico CDC did not come back after UF2 flash"
    echo "Firmware: $(python3 "$TMP/cdc.py" VER 1.5 | head -1)"

    # ---------------------------------------------------------------- DFU ----
    log "Loading FPGA bitstream via DFU"
    python3 "$FW_DIR/tools/dfu_fpga.py" "$BIN" || die "DFU load failed"
    sleep 10
    if ! wait_for_cdc 15; then
        echo "WEDGE: CDC not responding after DFU (Open Problem B)."
        journalctl -k -n 15 --no-pager 2>/dev/null | grep -i -E 'usb|set_interface' || true
        exit 3
    fi
    echo "CDC alive after DFU: OK"
fi

# --------------------------------------------------------------- capture ----
devnum() {   # USB device number of the Pico; changes every time it re-enumerates
    lsusb -d 1209:b1c0 2>/dev/null | sed -n 's/.*[Dd]evice \([0-9]*\).*/\1/p' | head -1
}
log "Checking USB stability (10 s)"
last="$(devnum)"; changes=0
for _ in $(seq 1 10); do
    sleep 1
    cur="$(devnum)"
    if [ "$cur" != "$last" ]; then changes=$((changes + 1)); last="$cur"; fi
done
if [ "$changes" -gt 0 ]; then
    echo "UNSTABLE: the Pico re-enumerated $changes time(s) in 10 s, before any capture started."
    echo "That is a disconnect loop, not packet drops. Recent kernel USB log:"
    journalctl -k -n 40 --no-pager 2>/dev/null | grep -i -E 'usb 1-|usb [0-9]+-[0-9.]+:' | tail -n 20 || true
    echo "--- device counters ---"
    python3 "$TMP/cdc.py" DEBUG 2.5 || echo "(DEBUG gave no reply)"
    exit 4
fi
echo "Stable (device number $last)."

find_card() {
    arecord -l 2>/dev/null |
        sed -n 's/^card \([0-9]*\):.*DDC SDR.*device \([0-9]*\):.*/hw:\1,\2/p' | head -1
}
USER_CARD="${CARD:-}"
lsusb -d 1209:b1c0 || echo "(lsusb: device not listed?)"

SUM_DROPS=()
SUM_OK=1
for run in $(seq 1 "$RUNS"); do
    log "Capture $run/$RUNS (${DUR}s)"
    # The card number changes if the device re-enumerates, so look it up each run.
    CARD="$USER_CARD"
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -n "$CARD" ] || CARD="$(find_card)"
        [ -n "$CARD" ] && break
        sleep 1
    done
    if [ -z "$CARD" ]; then
        echo "DDC SDR capture device not found in 'arecord -l' (device gone from the bus?)"
        journalctl -k -n 25 --no-pager 2>/dev/null | grep -i -E 'usb|xhci|snd' || true
        SUM_DROPS+=("no-device"); SUM_OK=0
        continue
    fi
    echo "Capture device: $CARD"
    T0="$(date '+%Y-%m-%d %H:%M:%S')"
    rm -f "$WAV"
    if ! arecord -D "$CARD" -f S24_3LE -r 48000 -c 2 -d "$DUR" "$WAV" 2>&1 | tail -n 6; then :; fi
    if [ "${PIPESTATUS[0]}" -ne 0 ]; then
        echo "arecord failed; kernel USB log since start of this capture:"
        journalctl -k --since "$T0" --no-pager 2>/dev/null | grep -i -E 'usb|xhci|snd|dwc|hub' | tail -n 30 || true
        SUM_DROPS+=("arecord-fail")
        SUM_OK=0
        continue
    fi

    OUT="$(python3 "$TMP/analyze.py" "$WAV")"
    echo "$OUT" | grep -v '^RESULT'
    D="$(echo "$OUT" | sed -n 's/^RESULT drops=\(-\?[0-9]*\).*/\1/p')"
    SUM_DROPS+=("${D:-?}")
    [ "${D:-1}" = "0" ] || SUM_OK=0

    echo "--- device counters ---"
    python3 "$TMP/cdc.py" DEBUG 2.5 || echo "(DEBUG gave no reply)"
done

# --------------------------------------------------------------- summary ----
log "Summary"
i=1
for d in "${SUM_DROPS[@]}"; do
    echo "run $i: drops=$d"
    i=$((i + 1))
done
if [ "$SUM_OK" = 1 ]; then
    echo "PASS: no dropouts in $RUNS x ${DUR}s"
    exit 0
else
    echo "FAIL: dropouts (or capture errors) detected"
    exit 1
fi
