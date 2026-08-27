# Quisk configuration for the Dev-iCE DDC SDR firmware.

from __future__ import absolute_import, division, print_function

import os
import time

import serial
import serial.tools.list_ports
from quisk_hardware_model import Hardware as BaseHardware


name_of_sound_capt = "alsa:DDC SDR 2026 (hw:D2026,0)"
name_of_sound_play = "pulse"
sample_rate = 48000
openradio_lower = 0
openradio_upper = 30_000_000


class Hardware(BaseHardware):
    def open(self):
        port = self._find_port()
        if port is None:
            raise serial.serialutil.SerialException(
                "Dev-iCE DDC SDR not found (VID:PID 1209:b1c0)")

        self.or_serial = serial.Serial(port, 115200, timeout=3)
        self.or_serial.write(b"\x03\x04")
        self.or_serial.reset_input_buffer()
        self.or_serial.timeout = 0.2
        deadline = time.time() + 6.0
        while time.time() < deadline:
            if b"SDR ready" in self.or_serial.readline():
                break
        
        # Flush any trailing prompt/banner data sent after "SDR ready"
        time.sleep(0.05)
        self.or_serial.reset_input_buffer()
        self.or_serial.timeout = 3

        version = self._get_parameter("VER")
        xtal_val = self._get_parameter("XTAL")
        try:
            self._crystal_freq = float(xtal_val)
        except ValueError:
            # Fallback if XTAL is not reported or invalid
            self._crystal_freq = 122880000.0

        mode = self._get_parameter("MODE")
        if str(mode).strip().upper() != "DDC":
            raise serial.serialutil.SerialException(
                f"Connected device is not a Dev-iCE DDC SDR (mode reported: {mode})")

        self._set_parameter("RATE", str(sample_rate))
        self._last_tune = None
        self._last_lo = None
        self._pending_vfo_update = False
        self._golden_status = "DDC: ready"
        return "%s. Capture from %s at %d Hz." % (
            version, self.conf.name_of_sound_capt, sample_rate)

    def close(self):
        self.or_serial.close()

    def _find_port(self):
        for info in serial.tools.list_ports.comports():
            if (info.vid, info.pid) == (0x1209, 0xB1C0):
                return info.device
        for device in ("/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2"):
            if os.path.exists(device):
                return device
        return None

    def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
        frequency = max(openradio_lower, min(openradio_upper, int(round(tune))))
        self._send("FREQ,%d" % frequency)
        self._get_argument()
        self._last_tune = frequency
        self._last_lo = frequency
        self._pending_vfo_update = True
        return frequency, frequency

    def ReturnFrequency(self):
        if self._pending_vfo_update and self._last_tune is not None:
            self._pending_vfo_update = False
            return self._last_tune, self._last_lo
        return None, None

    def HeartBeat(self):
        try:
            self.application.StatusScreen(self._golden_status)
        except Exception:
            pass

    def _send(self, line):
        # Clear any unread stale response bytes before sending a new command
        self.or_serial.reset_input_buffer()
        self.or_serial.write((line + "\r\n").encode())

    def _readline(self):
        return self.or_serial.readline()

    def _get_parameter(self, command):
        self._send(command)
        for _ in range(10):
            line = self._readline().decode(errors='replace').strip()
            if not line or line == "OK":
                continue
            if line.startswith(command + ","):
                return line.split(",", 1)[1]
            if "," in line:
                return line.split(",", 1)[1]
        return -1

    def _set_parameter(self, command, value):
        self._send("%s,%s" % (command, value))
        self._get_argument()
        return True

    def _get_argument(self):
        for _ in range(10):
            line = self._readline().decode(errors='replace').strip()
            if not line:
                continue
            if line in ("OK", "ERR"):
                return line
            if "," in line:
                return line.split(",", 1)[1]
        return -1
