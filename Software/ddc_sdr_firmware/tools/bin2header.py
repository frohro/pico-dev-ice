#!/usr/bin/env python3

import pathlib
import sys


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: bin2header.py BITSTREAM.bin OUTPUT.h")

    data = pathlib.Path(sys.argv[1]).read_bytes()
    if not data:
        raise SystemExit("bitstream is empty")

    output = "#ifndef DDC_FPGA_BITSTREAM_H_\n"
    output += "#define DDC_FPGA_BITSTREAM_H_\n\n"
    output += "#include <stddef.h>\n#include <stdint.h>\n\n"
    output += "static const uint8_t ddc_fpga_bitstream[] = {\n"
    for offset in range(0, len(data), 12):
        values = ", ".join(f"0x{value:02x}" for value in data[offset:offset + 12])
        output += "    " + values + ",\n"
    output += "};\n\n"
    output += "#define DDC_FPGA_BITSTREAM_SIZE (sizeof(ddc_fpga_bitstream))\n\n"
    output += "#endif\n"
    pathlib.Path(sys.argv[2]).write_text(output, encoding="ascii")


if __name__ == "__main__":
    main()
