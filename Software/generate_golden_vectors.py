from pathlib import Path


FRAME_COUNT = 3
SAW_START = 0x800000
SAW_STEP = 0x010000
SAMPLE_MASK = (1 << 24) - 1


def slot_word(sample):
    return (sample & SAMPLE_MASK) << 8


def frame_lines(frame_index, sample):
    word = slot_word(sample)
    bits = f"{word:032b}"
    return [
        f"frame={frame_index} sample=0x{sample:06x} ws=0 bits={bits}",
        f"frame={frame_index} sample=0x{sample:06x} ws=1 bits={bits}",
    ]


def generate_vectors():
    text_lines = [
        "# Each frame contains one WS=0 left slot followed by one WS=1 right slot.",
        "# Each slot is the exact 32 bits sampled on BCK rising edges.",
    ]
    words = []
    sample = SAW_START
    for frame_index in range(FRAME_COUNT):
        word = slot_word(sample)
        text_lines.extend(frame_lines(frame_index, sample))
        words.extend((f"{word:08x}", f"{word:08x}"))
        sample = (sample + SAW_STEP) & SAMPLE_MASK
    return text_lines, words


def main():
    text_lines, words = generate_vectors()
    output_dir = Path(__file__).resolve().parent
    (output_dir / "expected_i2s.txt").write_text("\n".join(text_lines) + "\n")
    (output_dir / "expected_words.mem").write_text("\n".join(words) + "\n")
    print("\n".join(text_lines))


if __name__ == "__main__":
    main()