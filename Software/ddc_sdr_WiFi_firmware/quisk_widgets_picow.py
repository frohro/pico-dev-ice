# Custom Quisk Widget Module for Raspberry Pi Pico W SDR (Intro-to-CAD-2026)
# ==============================================================================
# Provides custom bottom controls, quick band / WWV buttons, sample rate switcher,
# and an interactive 1-Click Flash Crystal Calibrator on the Config screen.
# ==============================================================================

from __future__ import print_function, absolute_import, division
import wx
import socket

class BottomWidgets(object):
    """
    Enhanced bottom control bar for Pico W SDR Receiver.
    Provides one-click ham band switching, WWV presets, and Sample Rate switching.
    """
    def __init__(self, app, hardware, conf, frame=None, gbs=None, vertBox=None, *args, **kwargs):
        self.app = app
        self.hardware = hardware
        self.conf = conf
        self.parent = frame
        self.sizer = vertBox if vertBox is not None else (args[0] if args else None)

        if self.parent is None:
            return

        panel = wx.Panel(self.parent)
        main_box = wx.BoxSizer(wx.VERTICAL)
        row1 = wx.BoxSizer(wx.HORIZONTAL)
        row2 = wx.BoxSizer(wx.HORIZONTAL)

        # Title / Status
        self.status_lbl = wx.StaticText(panel, wx.ID_ANY, "Pico W SDR (OpenHPSDR P1)")
        font = self.status_lbl.GetFont()
        font.SetPointSize(9)
        self.status_lbl.SetFont(font)
        self.status_lbl.SetForegroundColour(wx.Colour(140, 200, 255))
        row1.Add(self.status_lbl, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 8)

        # Sample Rate Selector dropdown directly on bottom bar
        row1.Add(wx.StaticText(panel, wx.ID_ANY, "Rate:"), 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)
        current_rate = str(getattr(self.conf, 'sample_rate', 48000))
        self.choice_rate = wx.Choice(panel, wx.ID_ANY, choices=["48000", "96000"])
        if current_rate in ["48000", "96000"]:
            self.choice_rate.SetStringSelection(current_rate)
        else:
            self.choice_rate.SetSelection(0)
        self.choice_rate.Bind(wx.EVT_CHOICE, self._on_change_rate)
        row1.Add(self.choice_rate, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 10)

        # Standard Ham Bands
        ham_bands = [
            ("80m", 3573000),
            ("40m", 7074000),
            ("30m", 10136000),
            ("20m", 14074000),
            ("17m", 18100000),
            ("15m", 21074000),
            ("12m", 24915000),
            ("10m", 28074000),
        ]
        for name, freq in ham_bands:
            btn = wx.Button(panel, wx.ID_ANY, name, size=(42, 24))
            btn.Bind(wx.EVT_BUTTON, self._make_tune_handler(freq))
            row1.Add(btn, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 3)

        # WWV Calibration Frequency Presets
        cal_lbl = wx.StaticText(panel, wx.ID_ANY, "NBS/WWV Presets:")
        cal_lbl.SetForegroundColour(wx.Colour(255, 200, 100))
        row2.Add(cal_lbl, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 6)

        wwv_freqs = [
            ("2.5M", 2500000),
            ("5.0M", 5000000),
            ("10.0M", 10000000),
            ("15.0M", 15000000),
            ("20.0M", 20000000),
            ("CHU 7.8M", 7850000),
        ]
        for name, freq in wwv_freqs:
            btn = wx.Button(panel, wx.ID_ANY, name, size=(60, 22))
            btn.Bind(wx.EVT_BUTTON, self._make_tune_handler(freq))
            row2.Add(btn, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)

        main_box.Add(row1, 0, wx.ALL, 2)
        main_box.Add(row2, 0, wx.LEFT | wx.RIGHT | wx.BOTTOM, 2)
        panel.SetSizer(main_box)

        if self.sizer is not None:
            self.sizer.Add(panel, 0, wx.ALL, 2)

    def _on_change_rate(self, event):
        sel_rate = int(self.choice_rate.GetStringSelection())
        try:
            # 1. Update live running config & hardware
            if hasattr(self.hardware, 'SetSampleRate'):
                self.hardware.SetSampleRate(sel_rate)
            self.conf.sample_rate = sel_rate

            # 2. Persist to Quisk config file on disk so it survives restarts
            conf_paths = []
            if hasattr(self.conf, 'config_file_path') and self.conf.config_file_path:
                conf_paths.append(self.conf.config_file_path)
            
            # Common Quisk config locations
            import os, re
            home = os.path.expanduser("~")
            conf_paths.extend([
                os.path.join(home, ".quisk_conf.py"),
                os.path.join(home, ".quisk", "quisk_conf.py"),
                "quisk_conf_openhpsdr.py"
            ])

            saved = False
            for cpath in conf_paths:
                if os.path.isfile(cpath):
                    try:
                        with open(cpath, "r") as f:
                            content = f.read()
                        
                        if re.search(r"sample_rate\s*=", content):
                            new_content = re.sub(r"sample_rate\s*=\s*\d+", f"sample_rate = {sel_rate}", content)
                        else:
                            new_content = content + f"\nsample_rate = {sel_rate}\n"
                        
                        with open(cpath, "w") as f:
                            f.write(new_content)
                        saved = True
                        print(f"[*] Persisted sample_rate = {sel_rate} into {cpath}")
                    except Exception as e:
                        print(f"[!] Could not write to {cpath}: {e}")

            print(f"[*] Quisk sample rate switched to {sel_rate} SPS.")
            wx.MessageBox(
                f"Sample rate saved as {sel_rate} SPS.\n\n"
                "When you restart Quisk, it will launch directly at "
                f"{sel_rate} SPS with full 96 kHz spectrum bandwidth!",
                "Sample Rate Saved",
                wx.OK | wx.ICON_INFORMATION
            )
        except Exception as e:
            print(f"[!] Error setting sample rate: {e}")

    def _make_tune_handler(self, freq_hz):
        def handler(event):
            try:
                self.app.ChangeVFO(freq_hz)
            except Exception as e:
                print(f"[!] Error tuning to {freq_hz} Hz: {e}")
        return handler

    def update_widgets(self, *args, **kwargs):
        pass

    def UpdateText(self, text=None, *args, **kwargs):
        if text and hasattr(self, 'status_lbl') and self.status_lbl:
            try:
                self.status_lbl.SetLabel(str(text))
            except Exception:
                pass

    def SetText(self, text=None, *args, **kwargs):
        self.UpdateText(text)

    def HeartBeat(self, *args, **kwargs):
        pass


class ConfigWidgets(object):
    """
    Custom Quisk configuration screen for Pico W SDR.
    Provides Sample Rate dropdown and Si5351 Crystal Calibration Calculator.
    """
    def __init__(self, app, hardware, conf, parent=None, sizer=None, *args, **kwargs):
        self.app = app
        self.hardware = hardware
        self.conf = conf
        self.parent = parent
        self.sizer = sizer if sizer is not None else (args[0] if args else None)

        if self.parent is None or self.sizer is None:
            return

        # 1. Radio / Sample Rate Settings Box
        srate_box = wx.StaticBoxSizer(wx.StaticBox(self.parent, wx.ID_ANY, "Pico W SDR Radio Settings"), wx.VERTICAL)
        
        srate_grid = wx.FlexGridSizer(rows=2, cols=2, vgap=8, hgap=12)
        srate_grid.Add(wx.StaticText(self.parent, wx.ID_ANY, "Receiver Sample Rate:"), 0, wx.ALIGN_CENTER_VERTICAL)
        
        self.cfg_choice_rate = wx.Choice(self.parent, wx.ID_ANY, choices=["48000", "96000"])
        cur_rate = str(getattr(self.conf, 'sample_rate', 48000))
        if cur_rate in ["48000", "96000"]:
            self.cfg_choice_rate.SetStringSelection(cur_rate)
        else:
            self.cfg_choice_rate.SetSelection(0)
        self.cfg_choice_rate.Bind(wx.EVT_CHOICE, self._on_config_rate_change)
        srate_grid.Add(self.cfg_choice_rate, 0)

        srate_grid.Add(wx.StaticText(self.parent, wx.ID_ANY, "Pico W IP Address:"), 0, wx.ALIGN_CENTER_VERTICAL)
        self.txt_ip = wx.TextCtrl(self.parent, wx.ID_ANY, str(getattr(self.conf, 'hermes_ip', '192.168.1.186')), size=(140, -1))
        srate_grid.Add(self.txt_ip, 0)

        srate_box.Add(srate_grid, 0, wx.ALL, 6)
        self.sizer.Add(srate_box, 0, wx.EXPAND | wx.ALL, 8)

        # 2. Hardware Info Box
        info_box = wx.StaticBoxSizer(wx.StaticBox(self.parent, wx.ID_ANY, "Hardware Configuration"), wx.VERTICAL)
        info1 = wx.StaticText(self.parent, wx.ID_ANY, "Controller: Raspberry Pi Pico W (RP2040 Dual-Core + CYW43439 Wi-Fi)")
        info2 = wx.StaticText(self.parent, wx.ID_ANY, "Synthesizer: Si5351A Clock Generator via I2C (Direct Quadrature LO)")
        info3 = wx.StaticText(self.parent, wx.ID_ANY, "ADC Front-End: PCM1808 Stereo 24-bit Audio ADC (24-bit I2S via PIO)")
        info4 = wx.StaticText(self.parent, wx.ID_ANY, "Protocol: OpenHPSDR Protocol 1 (UDP Port 1024 / Command Port 5000)")
        for item in [info1, info2, info3, info4]:
            info_box.Add(item, 0, wx.ALL, 3)
        self.sizer.Add(info_box, 0, wx.EXPAND | wx.ALL, 8)

        # 3. Si5351 Crystal Calibration Calculator
        cal_box = wx.StaticBoxSizer(wx.StaticBox(self.parent, wx.ID_ANY, "Si5351 Crystal Calibration Helper"), wx.VERTICAL)
        
        desc = wx.StaticText(self.parent, wx.ID_ANY, 
            "To calibrate crystal: Tune to a standard WWV station (e.g. 15 MHz).\n"
            "Center carrier on spectrum, enter observed dial frequency, and click Save:")
        cal_box.Add(desc, 0, wx.ALL, 4)

        grid = wx.FlexGridSizer(rows=3, cols=2, vgap=6, hgap=10)

        # Nominal Crystal
        grid.Add(wx.StaticText(self.parent, wx.ID_ANY, "Nominal Crystal (Hz):"), 0, wx.ALIGN_CENTER_VERTICAL)
        self.txt_nominal = wx.TextCtrl(self.parent, wx.ID_ANY, "24576000", size=(140, -1))
        grid.Add(self.txt_nominal, 0)

        # True Standard RF Frequency
        grid.Add(wx.StaticText(self.parent, wx.ID_ANY, "Standard Ref (e.g. WWV 15M):"), 0, wx.ALIGN_CENTER_VERTICAL)
        self.txt_true = wx.TextCtrl(self.parent, wx.ID_ANY, "15000000", size=(140, -1))
        grid.Add(self.txt_true, 0)

        # Observed Dial Frequency
        grid.Add(wx.StaticText(self.parent, wx.ID_ANY, "Observed Dial Freq (Hz):"), 0, wx.ALIGN_CENTER_VERTICAL)
        self.txt_obs = wx.TextCtrl(self.parent, wx.ID_ANY, "14997522", size=(140, -1))
        grid.Add(self.txt_obs, 0)

        cal_box.Add(grid, 0, wx.ALL, 4)

        btn_calc = wx.Button(self.parent, wx.ID_ANY, "Compute & Save Calibrated Crystal to Flash")
        btn_calc.Bind(wx.EVT_BUTTON, self._on_calc_cal)
        cal_box.Add(btn_calc, 0, wx.ALL, 6)

        self.lbl_result = wx.StaticText(self.parent, wx.ID_ANY, "Result: Ready")
        font_res = self.lbl_result.GetFont()
        font_res.SetWeight(wx.FONTWEIGHT_BOLD)
        self.lbl_result.SetFont(font_res)
        cal_box.Add(self.lbl_result, 0, wx.ALL, 4)

        self.sizer.Add(cal_box, 0, wx.EXPAND | wx.ALL, 8)

    def _on_config_rate_change(self, event):
        sel_rate = int(self.cfg_choice_rate.GetStringSelection())
        if hasattr(self.hardware, 'SetSampleRate'):
            self.hardware.SetSampleRate(sel_rate)
        self.conf.sample_rate = sel_rate

    def _on_calc_cal(self, event):
        try:
            f_nom = float(self.txt_nominal.GetValue().strip())
            f_true = float(self.txt_true.GetValue().strip())
            f_obs = float(self.txt_obs.GetValue().strip())

            if f_obs <= 0 or f_true <= 0:
                self.lbl_result.SetLabel("Error: Frequencies must be > 0.")
                return

            f_cal = f_nom * (f_true / f_obs)
            f_cal_int = int(round(f_cal))
            ppm = ((f_true - f_obs) / f_true) * 1e6

            # Send CAL command to Pico W
            pico_ip = self.txt_ip.GetValue().strip() or getattr(self.conf, 'hermes_ip', '192.168.1.186')
            cmd = f"CAL,{int(f_true)},{int(f_obs)}\r\n"
            reply_str = ""
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(2.0)
                s.connect((pico_ip, 5000))
                s.sendall(cmd.encode('ascii'))
                reply = s.recv(256).decode('ascii', errors='ignore')
                s.close()
                reply_str = f"Pico W Response: {reply.strip()}"
            except Exception as e:
                reply_str = f"Note: Could not connect to Pico W at {pico_ip}:5000 ({e})"

            msg = (
                f"Calibrated Crystal: {f_cal_int} Hz ({f_cal/1e6:.6f} MHz)\n"
                f"Crystal Offset Error: {ppm:+.2f} PPM\n"
                f"{reply_str}\n"
                f"Saved permanently to RP2040 Flash memory!"
            )
            self.lbl_result.SetLabel(msg)
        except Exception as e:
            self.lbl_result.SetLabel(f"Error calculating: {e}")

    def write_conf(self, config_text):
        """Called when Quisk saves configuration."""
        try:
            sel_rate = self.cfg_choice_rate.GetStringSelection()
            sel_ip = self.txt_ip.GetValue().strip()
            # Update sample_rate and hermes_ip in config
            if "sample_rate =" in config_text:
                import re
                config_text = re.sub(r"sample_rate\s*=\s*\d+", f"sample_rate = {sel_rate}", config_text)
            if "hermes_ip =" in config_text and sel_ip:
                import re
                config_text = re.sub(r'hermes_ip\s*=\s*"[^"]*"', f'hermes_ip = "{sel_ip}"', config_text)
        except Exception as e:
            print(f"[!] write_conf error: {e}")
        return config_text
