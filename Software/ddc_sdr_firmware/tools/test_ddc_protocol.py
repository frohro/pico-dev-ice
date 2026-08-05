#!/usr/bin/env python3

import unittest


SYNC = 0xD5
VERSION = 0x01
SET_FREQUENCY = 0x01
SET_SAMPLE_RATE = 0x02
CLEAR_OTR = 0x04
FPGA_CLOCK_HZ = 30_720_000


def make_command(command, value):
    return bytes((SYNC, VERSION, command, 4)) + value.to_bytes(4, "little")


def frequency_to_fcw(frequency_hz):
    return (frequency_hz * (1 << 32) + FPGA_CLOCK_HZ // 2) // FPGA_CLOCK_HZ


class DdcProtocolTests(unittest.TestCase):
    def test_frequency_frame(self):
        self.assertEqual(
            make_command(SET_FREQUENCY, frequency_to_fcw(7_050_000)),
            bytes.fromhex("d5 01 01 04 00 00 c0 3a"),
        )

    def test_rate_frame(self):
        self.assertEqual(
            make_command(SET_SAMPLE_RATE, 96_000),
            bytes.fromhex("d5 01 02 04 00 77 01 00"),
        )

    def test_clear_otr_frame(self):
        self.assertEqual(
            make_command(CLEAR_OTR, 1),
            bytes.fromhex("d5 01 04 04 01 00 00 00"),
        )


if __name__ == "__main__":
    unittest.main()