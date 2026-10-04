# quisk_hardware_picow.py - Hardware Driver for Intro-to-CAD-2026 Pico W SDR
# ==============================================================================
# Customized Quisk hardware driver for Raspberry Pi Pico W OpenHPSDR Receiver.
# Eliminates Hermes-Lite 2 PA, Alex Filter, and Step-Attenuator controls.
# Provides Sample Rate switching (48 kHz / 96 kHz) and Si5351 LO controls.
# ==============================================================================

from __future__ import print_function, absolute_import, division
import os, sys

try:
    from hermes.quisk_hardware import Hardware as BaseHardware
except ImportError:
    try:
        from quisk_hardware_hermes import Hardware as BaseHardware
    except ImportError:
        from quisk_hardware_model import Hardware as BaseHardware


class Hardware(BaseHardware):
    """
    Dedicated Hardware driver for the Intro-to-CAD-2026 Pico W SDR.
    Overrides and suppresses HL2 / Hermes transmitter and PA controls.
    """
    def __init__(self, app, conf):
        self.app = app
        self.conf = conf
        self.bandEdge1 = 0
        self.bandEdge2 = 0
        self._current_sample_rate = getattr(conf, 'sample_rate', 48000)
        
        try:
            super(Hardware, self).__init__(app, conf)
            print(f"[*] Pico W Hardware Driver initialized at {self._current_sample_rate} SPS.")
        except Exception as e:
            print(f"[!] BaseHardware init exception (non-fatal): {e}")

    def open(self):
        try:
            return super(Hardware, self).open()
        except Exception as e:
            print(f"[!] Hardware open exception: {e}")
            return "Pico W Open"

    def close(self):
        try:
            return super(Hardware, self).close()
        except Exception:
            return None

    def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
        try:
            return super(Hardware, self).ChangeFrequency(tune, vfo, source=source, band=band, event=event)
        except TypeError:
            try:
                return super(Hardware, self).ChangeFrequency(tune, vfo)
            except Exception as e:
                return tune, vfo
        except Exception as e:
            return tune, vfo

    def ChangeMode(self, mode):
        if hasattr(super(Hardware, self), 'ChangeMode'):
            super(Hardware, self).ChangeMode(mode)

    def ChangeBand(self, band):
        if hasattr(super(Hardware, self), 'ChangeBand'):
            super(Hardware, self).ChangeBand(band)

    def HeartBeat(self):
        try:
            if hasattr(super(Hardware, self), 'HeartBeat'):
                return super(Hardware, self).HeartBeat()
            elif hasattr(super(Hardware, self), 'heartbeat'):
                return super(Hardware, self).heartbeat()
        except Exception:
            pass
        return None

    def heartbeat(self):
        return self.HeartBeat()

    # --------------------------------------------------------------------------
    # Hermes-Lite 2 & Peripheral Stubs (Suppress unwanted hardware options)
    # --------------------------------------------------------------------------
    def set_attenuation(self, att):
        """Pico W receiver has no relay step attenuator."""
        pass

    def set_preamp(self, preamp):
        """Pico W receiver front-end gain is fixed."""
        pass

    def set_filter(self, filter_num):
        """Pico W uses QSD Tayloe mixer with fixed low-pass anti-alias."""
        pass

    def set_alex(self, val):
        """Suppress Alex filter bank switching."""
        pass

    def set_tx_power(self, power):
        """RX Only receiver - disable TX power."""
        pass

    def set_cw_key(self, state):
        """Disable CW keying."""
        pass

    def get_tx_power(self):
        return 0.0

    def get_swr(self):
        return 1.0

    # --------------------------------------------------------------------------
    # Sample Rate Controls
    # --------------------------------------------------------------------------
    def SetSampleRate(self, new_rate):
        """
        Switches the SDR receiver sample rate between 48,000 and 96,000 SPS.
        """
        if new_rate not in (48000, 96000):
            print(f"[!] Unsupported sample rate requested: {new_rate}")
            return False
        
        print(f"[*] Setting Pico W Sample Rate to {new_rate} SPS...")
        self._current_sample_rate = new_rate
        self.conf.sample_rate = new_rate

        # If OpenHPSDR C backend has SetControlBit / SendControlPacket
        if hasattr(self, 'SetControlBit'):
            # Bit 0 of Control Byte 1 in OpenHPSDR Protocol 1 specifies sample rate:
            # 00 = 48k, 01 = 96k, 10 = 192k
            speed_code = 0 if new_rate == 48000 else 1
            try:
                # Update control byte
                self.SetControlBit(0x00, 0, speed_code & 1)
                self.SetControlBit(0x00, 1, (speed_code >> 1) & 1)
            except Exception as e:
                print(f"[!] SetControlBit error: {e}")

        return True

    def GetSampleRate(self):
        return self._current_sample_rate
