#!/usr/bin/env python3

import pathlib
import sys


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(
            "usage: bin2header.py BITSTREAM.bin OUTPUT.h [SYMBOL]"
        )

    data = pathlib.Path(sys.argv[1]).read_bytes()
    if not data:
        raise SystemExit("bitstream is empty")

    symbol = sys.argv[3] if len(sys.argv) == 4 else "ddc_fpga_bitstream"
    guard = "_".join((part.upper() for part in symbol.split("_"))) + "_H_"
    output = f"#ifndef {guard}\n"
    output += f"#define {guard}\n\n"
    output += "#include <stddef.h>\n#include <stdint.h>\n\n"
    output += f"static const uint8_t {symbol}[] = {{\n"
    for offset in range(0, len(data), 12):
        values = ", ".join(f"0x{value:02x}" for value in data[offset:offset + 12])
        output += "    " + values + ",\n"
    output += "};\n\n"
    output += f"#define {symbol.upper()}_SIZE (sizeof({symbol}))\n\n"
    output += "#endif\n"
    pathlib.Path(sys.argv[2]).write_text(output, encoding="ascii")


if __name__ == "__main__":
    main()
