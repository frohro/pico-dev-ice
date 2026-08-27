# Quisk Configuration for Pico-Dev-iCE OpenHPSDR Protocol 1 over Wi-Fi
# ==============================================================================
# Connects to the Raspberry Pi Pico W running OpenHPSDR Protocol 1.
# ==============================================================================

import os, sys

# 1. Hardware Module Selection (Use OpenHPSDR / Hermes protocol)
try:
    import hermes.quisk_hardware as quisk_hardware
except ImportError:
    try:
        import quisk_hardware_hermes as quisk_hardware
    except ImportError:
        import quisk_hardware_model as quisk_hardware

# 2. Network Configuration
hermes_ip = "192.168.1.191" # Pico W IP address
hermes_card_name = "Hermes"

# 3. Sample Rate & Audio Options
sample_rate = 48000
playback_rate = 48000
name_of_sound_capt = "None"
name_of_sound_play = "default"
channel_i = 0
channel_q = 1

# 4. Display and Tuning Defaults
default_screen = "Graph"
band_edges = [
    (1800000, 2000000),     # 160m
    (3500000, 4000000),     # 80m
    (7000000, 7300000),     # 40m
    (10100000, 10150000),   # 30m
    (14000000, 14350000),   # 20m
    (18068000, 18168000),   # 17m
    (21000000, 21450000),   # 15m
    (24890000, 24990000),   # 12m
    (28000000, 29700000),   # 10m
]

rx_min_freq = 100000
rx_max_freq = 30000000

print(f"[*] Quisk configured for Pico-Dev-iCE OpenHPSDR (Wi-Fi: {hermes_ip})")
