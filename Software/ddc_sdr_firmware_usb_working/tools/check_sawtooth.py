#!/usr/bin/env python3
"""Check a 24-bit stereo WAV recorded from the i2s_tx_demo sawtooth source.

Each sample advances by 0x010000 (mod 2^24) and L == R. Initial zero-filled
packets are expected while the capture ring primes and are excluded from the
sawtooth continuity check.
"""
import sys
import wave

STEP = 0x010000
MASK = 0xFFFFFF


def main(path):
    with wave.open(path, "rb") as wav:
        if wav.getsampwidth() != 3 or wav.getnchannels() != 2:
            sys.exit("need 24-bit stereo WAV")
        raw = wav.readframes(wav.getnframes())

    frames = len(raw) // 6
    left = [int.from_bytes(raw[6 * i:6 * i + 3], "little") for i in range(frames)]
    right = [int.from_bytes(raw[6 * i + 3:6 * i + 6], "little") for i in range(frames)]

    swaps = sum(1 for a, b in zip(left, right) if a != b)
    active_start = next((i for i, sample in enumerate(left) if sample != 0), frames)
    startup_silence = active_start
    skipped = repeated = events = 0
    for i in range(active_start + 1, frames):
        delta = ((left[i] - left[i - 1]) & MASK) // STEP
        if delta != 1:
            events += 1
            if delta == 0:
                repeated += 1
            # 256 steps per wrap; treat delta > 128 as a repeat/backwards slip.
            elif delta > 128:
                repeated += 256 - delta + 1
            else:
                skipped += delta - 1
    print(f"frames={frames} startup_silence={startup_silence} "
          f"discontinuities={events} "
          f"samples_skipped={skipped} samples_repeated={repeated} "
          f"L!=R frames={swaps}")
    return 0 if events == 0 and swaps == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
