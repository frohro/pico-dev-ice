#!/usr/bin/env python3

import unittest


SYNC = 0xD5
VERSION = 0x01
SET_FREQUENCY = 0x01
SET_SAMPLE_RATE = 0x02


def make_command(command, value):
    return bytes((SYNC, VERSION, command, 4)) + value.to_bytes(4, "little")


class DdcProtocolTests(unittest.TestCase):
    def test_frequency_frame(self):
        self.assertEqual(
            make_command(SET_FREQUENCY, 7_050_000),
            bytes.fromhex("d5 01 01 04 10 93 6b 00"),
        )

    def test_rate_frame(self):
        self.assertEqual(
            make_command(SET_SAMPLE_RATE, 96_000),
            bytes.fromhex("d5 01 02 04 00 77 01 00"),
        )


if __name__ == "__main__":
    unittest.main()