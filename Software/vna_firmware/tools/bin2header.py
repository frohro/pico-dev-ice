#!/usr/bin/env python3
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write(f"Usage: {sys.argv[0]} <input.bin> <output.h> <symbol>\n")
        return 1

    in_path = pathlib.Path(sys.argv[1])
    out_path = pathlib.Path(sys.argv[2])
    symbol = sys.argv[3]

    data = in_path.read_bytes()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    guard = f"{symbol.upper()}_H"
    lines = [
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        f"static const uint8_t {symbol}[] = {{",
    ]

    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        hex_bytes = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hex_bytes},")

    lines.extend([
        "};",
        f"static const size_t {symbol}_len = sizeof({symbol});",
        "",
        f"#endif /* {guard} */",
        "",
    ])

    out_path.write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
