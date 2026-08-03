# Quisk configuration for the Intro-to-CAD-2026 Pico SDR board
#
# Student Lab 5 copy: place this next to your lab files and use it as the
# host-side Quisk configuration for the CDC control protocol.

from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

import os

name_of_sound_capt = "alsa:SDR PCM1808 2026 (hw:S2026,0)"
name_of_sound_play = "pulse"

sample_rate = 48000

openradio_lower = 3_800_000
openradio_upper = 30_000_000

widgets_file_name = os.path.join(os.path.dirname(__file__), "quisk_widgets_2026.py")

import math
import fractions
import serial
import serial.tools.list_ports
import time
from quisk_hardware_model import Hardware as BaseHardware


class Hardware(BaseHardware):
    def open(self):
        """Open the serial connection to the Pico and configure it."""
        baud = 115200
        # Prefer VID:PID detection so the correct port is found even when the
        # Pico Debug Stack (picoprobe) occupies a lower-numbered ACM device.
        port_device = None
        for info in serial.tools.list_ports.comports():
            if (info.vid, info.pid) in ((0xcafe, 0x4011), (0xcafe, 0x4010)):
                port_device = info.device
                break
        if port_device is None:
            for p in ("/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2", "/dev/ttyACM3"):
                if os.path.exists(p):
                    port_device = p
                    break
        if port_device is None:
            raise serial.serialutil.SerialException(
                "Pico not found (VID:PID cafe:4011 or /dev/ttyACM0-3)")
        self.or_serial = serial.Serial(port_device, baud, timeout=3)
        print("Opened", port_device)

        self.or_serial.write(b'\x03')
        time.sleep(0.1)
        self.or_serial.write(b'\x04')
        self.or_serial.reset_input_buffer()

        self.or_serial.timeout = 0.2
        deadline = time.time() + 6.0
        while time.time() < deadline:
            line = self.or_serial.readline()
            if b'SDR ready' in line:
                break
        self.or_serial.timeout = 3

        version = str(self._get_parameter("VER"))
        print("Pico firmware:", version)

        xtal_raw = self._get_parameter("XTAL")
        try:
            self._crystal_freq = float(xtal_raw)
            if self._crystal_freq <= 0:
                raise ValueError("invalid")
        except (ValueError, TypeError):
            self._crystal_freq = 24_576_000.0
        print("Crystal freq: %.3f Hz" % self._crystal_freq)

        mode_raw = self._get_parameter("MODE")
        if mode_raw == -1:
            self._johnson_counter = False
        else:
            self._johnson_counter = (str(mode_raw).strip().upper() == "JOHNSON")
        print("Mixer mode: %s" % ("JOHNSON counter (÷4)" if self._johnson_counter else "DIRECT"))

        self._set_parameter("RATE", str(sample_rate))

        self._golden_status = "PLL: ready"
        self._last_lo = None
        self._last_tune = None
        self._active_candidate = None
        self._pending_vfo_update = True  # Force UI sync after open

        self._comparison_use_worst = False
        self._comparison_use_middle = False
        self._comparison_use_fract = False
        self._comparison_pair = None
        self._comparison_candidates = []

        return version + ". Capture from %s at %d Hz." % (
            self.conf.name_of_sound_capt, sample_rate)

    def close(self):
        self.or_serial.close()

    def _candidate_from_values(self, tune, lo_hz, n_value, a_value, b_value, c_value):
        tune_hz = int(round(tune))
        lo_hz = int(round(lo_hz))
        floor_term = math.floor(128 * b_value / c_value)
        p1 = int(128 * a_value + floor_term - 512)
        p2 = int(128 * b_value - c_value * floor_term)
        p3 = int(c_value)
        return {
            "tune": tune_hz,
            "lo": lo_hz,
            "signed_offset": lo_hz - tune_hz,
            "n": int(n_value),
            "a": int(a_value),
            "b": int(b_value),
            "c": int(c_value),
            "p1": p1,
            "p2": p2,
            "p3": p3,
            "ptype": "G" if int(b_value) == 0 else "F",
        }

    def _candidate_for_lo(self, tune, lo_hz):
        lo_hz = int(round(lo_hz))
        crystal = self._crystal_freq

        # Si5351a CLK0 must output 4× lo_hz in Johnson mode
        si5351_hz = lo_hz * 4 if self._johnson_counter else lo_hz
        n_step = 1 if self._johnson_counter else 2
        n_min = max(4 if self._johnson_counter else 6,
                    int(math.ceil(600_000_000.0 / si5351_hz)))
        n_max = min(127 if self._johnson_counter else 126,
                    int(900_000_000.0 / si5351_hz))
        if not self._johnson_counter and n_min % 2 != 0:
            n_min += 1

        best_n = None
        exact_m = None
        for n_cand in range(n_min, n_max + 1, n_step):
            m_float = si5351_hz * n_cand / crystal
            if not (14 < m_float < 91):
                continue
            m_int = round(m_float)
            if abs(m_float - m_int) < 1e-4:
                best_n = n_cand
                exact_m = m_int
                break

        if best_n is not None:
            return self._candidate_from_values(tune, lo_hz, best_n, exact_m, 0, 1)

        n_value = n_min
        frac = fractions.Fraction(int(si5351_hz * n_value), int(crystal)).limit_denominator(1048575)
        a_value = int(frac)
        b_value = frac.numerator - a_value * frac.denominator
        c_value = frac.denominator
        return self._candidate_from_values(tune, lo_hz, n_value, a_value, b_value, c_value)

    def _candidate_rank_key(self, candidate):
        # Primary: integer quality = reduced denominator of M/N.
        # When M/N reduces to p/q (lowest terms), the q-th LO harmonic couples
        # to the p-th crystal harmonic.  Larger q → higher-order coupling →
        # weaker spur → BETTER quality.  Use -q so sorted() puts best first.
        n = candidate["n"]
        m = candidate["a"]          # b==0 for all integer candidates
        g = math.gcd(m, n)
        reduced_denom = n // g      # q in the reduced fraction M/N = p/q
        return (
            -reduced_denom,                   # best (largest q) first
            abs(candidate["signed_offset"]),  # then closest LO to tune
            n,                                # then smallest N (lower phase noise)
        )

    def _candidate_worst_fract(self, tune):
        half_bw = sample_rate // 2
        crystal = self._crystal_freq
        multiplier = 4 if self._johnson_counter else 1
        n_step = 1 if self._johnson_counter else 2
        si5351_target = tune * multiplier
        n_min = max(4, int(math.ceil(600_000_000.0 / si5351_target)))
        n_max = min(127, int(900_000_000.0 / si5351_target))
        if not self._johnson_counter and n_min % 2 != 0:
            n_min += 1
            
        best_cand = None
        best_offset = float('inf')
        
        for n_cand in range(n_min, n_max + 1, n_step):
            ideal_m = si5351_target * n_cand / crystal
            if not (14 < ideal_m < 91): continue
            
            # Target an audibly annoying spur around 2.5 kHz right in the passband
            c_target = crystal / (n_cand * 2500.0)
            c_int = int(round(c_target))
            if c_int < 2: c_int = 2
            
            a_int = int(math.floor(ideal_m))
            frac = ideal_m - a_int
            b_int = int(round(frac * c_int))
            
            # FORCE it to be non-integer to guarantee spurs!
            if b_int <= 0: b_int = 1
            if b_int >= c_int: b_int = c_int - 1
            
            lo_logical = (a_int + b_int / c_int) * crystal / (n_cand * multiplier)
            signed = lo_logical - tune
            
            if abs(signed) < half_bw:
                if abs(signed) < best_offset:
                    best_offset = abs(signed)
                    best_cand = self._candidate_from_values(tune, lo_logical, n_cand, a_int, b_int, c_int)
                        
        if best_cand:
            return best_cand
                    
        # Fallback if no appropriate fraction fits in the passband
        return self._candidate_for_lo(tune, int(round(tune - sample_rate // 4)))

    def _comparison_label(self):
        if getattr(self, '_comparison_use_fract', False):
            return "FRACT"
        if getattr(self, '_comparison_use_worst', False):
            return "WORST"
        if getattr(self, '_comparison_use_middle', False):
            return "MIDDLE"
        return "BEST"

    def get_lo_choices(self):
        """Returns a list of (label, metadata) for the UI dropdown."""
        choices = []
        pair = getattr(self, '_comparison_pair', None)
        
        # Determine available integer options
        count = pair["integer_count"] if pair else 0
        
        if count >= 1:
            choices.append(("BEST Integer (Green)", {"type": "best"}))
        if count >= 3:
            choices.append(("MIDDLE Integer (Yellow)", {"type": "middle"}))
        if count >= 2:
            choices.append(("WORST Integer (Yellow)", {"type": "worst"}))
            
        # Always include fractional and custom options
        choices.append(("Worst LO (Red)", {"type": "fract"}))
        choices.append(("Custom LO... (Black)", {"type": "custom"}))
        
        return choices

    def _format_candidate(self, label, candidate):
        return (
            "%s LO=%dHz off=%+dHz N=%d M=%d+%d/%d P1=%d P2=%d P3=%d" % (
                label,
                candidate["lo"],
                candidate["signed_offset"],
                candidate["n"],
                candidate["a"],
                candidate["b"],
                candidate["c"],
                candidate["p1"],
                candidate["p2"],
                candidate["p3"],
            )
        )

    def _widget_summary(self):
        if not getattr(self, '_comparison_pair', None):
            return "Best/Worst integers not ready"

        cand = getattr(self, '_active_candidate', None) or self.active_comparison_candidate()
        if not cand:
            return "No active candidate"

        # Show the actual LO type, not just the comparison mode label.
        if cand.get("b", 0) == 0:
            mode = self._comparison_label()
        else:
            mode = "FRAC"

        return "%s LO=%dHz off=%+dHz N=%d M=%d+%d/%d" % (
            mode,
            cand["lo"],
            cand["signed_offset"],
            cand["n"],
            cand["a"],
            cand["b"],
            cand["c"],
        )

    def _refresh_widget_status(self):
        bottom = getattr(self.application, "bottom_widgets", None)
        if bottom and hasattr(bottom, "UpdateStatus"):
            bottom.UpdateStatus()

    def _log_comparison_pair(self):
        if not self._comparison_pair:
            return
        print(self._format_candidate("BEST", self._comparison_pair["best"]))
        if self._comparison_pair["integer_count"] > 1:
            print(self._format_candidate("WORST", self._comparison_pair["worst"]))
        elif self._comparison_pair["integer_count"] == 1:
            print("WORST same as BEST: only one valid integer LO in the overlapping 48 kHz windows")
        else:
            print("No valid integer LO pair; using fractional fallback")

    def _select_comparison_pair(self, tune):
        candidates = self._find_integer_candidates(tune)
        self._comparison_candidates = candidates
        if candidates:
            ordered = sorted(candidates, key=self._candidate_rank_key)
            best = ordered[0]
            middle = ordered[len(ordered) // 2]
            worst = ordered[-1]
            pair = {
                "best": best,
                "middle": middle,
                "worst": worst,
                "integer_count": len(ordered),
            }
        else:
            fallback = self._candidate_for_lo(tune, int(round(tune - sample_rate // 4)))
            pair = {
                "best": fallback,
                "middle": fallback,
                "worst": fallback,
                "integer_count": 0,
            }
        pair["fract"] = self._candidate_worst_fract(tune)
        self._comparison_pair = pair
        return pair

    def active_comparison_candidate(self):
        if not self._comparison_pair:
            return None
        if getattr(self, '_comparison_use_fract', False):
            return self._comparison_pair.get("fract")
        if getattr(self, '_comparison_use_worst', False):
            return self._comparison_pair["worst"]
        if getattr(self, '_comparison_use_middle', False):
            return self._comparison_pair["middle"]
        return self._comparison_pair["best"]

    def set_comparison_mode(self, use_worst=False, use_middle=False, use_fract=False):
        self._comparison_use_worst = bool(use_worst)
        self._comparison_use_middle = bool(use_middle)
        self._comparison_use_fract = bool(use_fract)
        candidate = self.active_comparison_candidate()
        if candidate is not None:
            self._program_candidate(candidate, self._comparison_label())
            self._pending_vfo_update = True
        self._refresh_widget_status()

    def set_custom_lo(self, lo_hz):
        """Program the Si5351a to an arbitrary LO frequency chosen by the user."""
        lo_hz = int(round(lo_hz))
        tune = self._last_tune if self._last_tune is not None else lo_hz
        
        # Validation: The requested LO must be within 48% of the sample rate
        # to ensure it can actually receive the current tune frequency.
        # We use 0.48 instead of 0.5 as a small guard band.
        half_bw = sample_rate * 0.48
        if abs(lo_hz - tune) > half_bw:
            import wx
            wx.MessageBox(
                f"Cannot set LO to {lo_hz:,} Hz.\n\n"
                f"The current receive frequency ({tune:,} Hz) is outside the "
                f"audible passband for this LO.\n\n"
                f"Maximum allowed offset is ±{int(half_bw):,} Hz.",
                "LO Frequency Error",
                wx.OK | wx.ICON_ERROR
            )
            return

        candidate = self._candidate_for_lo(tune, lo_hz)
        self._program_candidate(candidate, "CUST")
        self._pending_vfo_update = True
        self._refresh_widget_status()

    def ReturnFrequency(self):
        if getattr(self, '_pending_vfo_update', False):
            candidate = getattr(self, '_active_candidate', None) or self.active_comparison_candidate()
            if candidate is not None and self._last_tune is not None:
                self._pending_vfo_update = False
                return self._last_tune, candidate["lo"]
        return None, None

    def _find_integer_candidates(self, tune):
        half_bw = sample_rate // 2
        crystal = self._crystal_freq
        multiplier = 4 if self._johnson_counter else 1
        n_step = 1 if self._johnson_counter else 2
        si5351_target = tune * multiplier
        n_min = max(4, int(math.ceil(600_000_000.0 / si5351_target)))
        n_max = min(127, int(900_000_000.0 / si5351_target))
        if not self._johnson_counter and n_min % 2 != 0:
            n_min += 1
        results = []
        for n_cand in range(n_min, n_max + 1, n_step):
            m_float = si5351_target * n_cand / crystal
            if not (14 < m_float < 91):
                continue
            m_int = round(m_float)
            lo_si5351 = crystal * m_int / n_cand
            lo_logical = lo_si5351 / multiplier
            signed = lo_logical - tune
            if abs(signed) < half_bw:
                candidate = self._candidate_from_values(tune, lo_logical, n_cand, m_int, 0, 1)
                results.append(candidate)
        return results

    def _find_golden_los(self, tune):
        ordered = sorted(self._find_integer_candidates(tune), key=self._candidate_rank_key)
        return [(candidate["lo"], candidate["signed_offset"]) for candidate in ordered]

    def _program_lo(self, lo_hz):
        candidate = self._candidate_for_lo(lo_hz, lo_hz)
        return self._program_candidate(candidate, None)

    def _program_candidate(self, candidate, label):
        # In JOHNSON mode the Si5351a must output 4× the logical LO frequency
        multiplier = 4 if self._johnson_counter else 1
        si5351_hz = int(round(candidate['lo'] * multiplier))
        self._send(
            f"FREQ,{si5351_hz},{candidate['n']},{candidate['a']},{candidate['b']},{candidate['c']},{candidate['p1']},{candidate['p2']},{candidate['p3']}"
        )
        self._readline()
        ok_line = self._readline().decode(errors='replace').strip()
        self._last_lo = candidate["lo"]
        self._active_candidate = dict(candidate)
        parts = ok_line.split(",")
        signed_offset = 0
        if len(parts) >= 3 and parts[0] == "OK":
            ptype = parts[1]
            try:
                signed_offset = int(parts[2])
            except ValueError:
                signed_offset = 0
            if ptype == "G":
                prefix = label or "PLL"
                self._golden_status = "%s  {:+d} Hz  N=%d M=%d+%d/%d".format(signed_offset) % (
                    prefix,
                    candidate["n"],
                    candidate["a"],
                    candidate["b"],
                    candidate["c"],
                )
            elif ptype == "F":
                self._golden_status = "frac  (exact freq)"
            else:
                self._golden_status = "fallback  (VCO out of spec)"
        self._refresh_widget_status()
        return signed_offset

    def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
        tune = max(openradio_lower, min(openradio_upper, tune))
        half_bw = sample_rate // 2
        quarter = sample_rate // 4
        self._last_tune = int(round(tune))

        pair = self._select_comparison_pair(tune)
        golden = [(candidate["lo"], candidate["signed_offset"]) for candidate in self._comparison_candidates]

        is_arrow = (
            self._last_lo is not None and
            abs(vfo - self._last_lo) > 100 and
            abs(vfo - self._last_lo) < half_bw
        )

        if is_arrow:
            direction = 1 if vfo > self._last_lo else -1
            candidates = [
                (abs(lo - self._last_lo), lo, signed)
                for (lo, signed) in golden
                if (lo - self._last_lo) * direction > 0
            ]
            if candidates:
                candidates.sort()
                chosen_lo = candidates[0][1]
            else:
                shift = direction * quarter
                new_lo = self._last_lo + shift
                guard = 2000
                new_lo = max(tune - half_bw + guard, min(tune + half_bw - guard, new_lo))
                chosen_lo = new_lo
            candidate = self._candidate_for_lo(tune, chosen_lo)
            label = "ARROW"
        else:
            candidate = self.active_comparison_candidate()
            if candidate is None:
                candidate = pair["best"]
            chosen_lo = candidate["lo"]
            label = self._comparison_label()
            self._log_comparison_pair()

        chosen_lo = int(max(openradio_lower, min(openradio_upper, chosen_lo)))
        if candidate["lo"] != chosen_lo:
            candidate = self._candidate_for_lo(tune, chosen_lo)
        self._program_candidate(candidate, label)
        return int(tune), int(chosen_lo)

    def HeartBeat(self):
        try:
            self.application.StatusScreen(self._golden_status)
            self._refresh_widget_status()
        except Exception:
            pass

    def _send(self, line):
        self.or_serial.write((line + "\n").encode())

    def _readline(self):
        return self.or_serial.readline()

    def _get_parameter(self, cmd):
        self._send(cmd)
        return self._get_argument()

    def _set_parameter(self, cmd, arg):
        self._send(cmd + "," + arg)
        self._get_argument()
        return True

    def _get_argument(self):
        """Read lines from the serial port until we find a value or a terminal.

        Protocol lines are either:
          • "<CMD>,<value>\\r\\n"  – return the value; trailing OK is left
            in the buffer and will be discarded as a terminal on the *next*
            _get_argument call.  Do NOT consume it here: doing so risks eating
            the first line of the next command's reply.
          • "OK\\r\\n" or "ERR\\r\\n" – terminal, return as-is.
        """
        for _ in range(10):
            data = self._readline()
            if len(data) == 0:
                return -1
            data_str = data.decode(errors='replace').strip()
            if not data_str:          # blank line – skip and keep reading
                continue
            if data_str == 'OK' or data_str == 'ERR':
                return data_str       # terminal with no payload
            if ',' in data_str:
                return data_str.split(',')[1]   # value payload; trailing OK stays
        return -1
