"""Stand-in for the STM32: sends sensor JSON lines to the gateway and shows replies.

Usage: python scripts/fake_stm.py COM5 [count] [interval_seconds]
COM5 is the ESP32 USB port (the gateway also accepts packets over USB), or a
USB-to-serial adapter wired to GPIO25/26 to exercise the real STM32 UART.
Readings are synthetic. Close the PlatformIO serial monitor first.
"""
import json
import random
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
count = int(sys.argv[2]) if len(sys.argv) > 2 else 5
interval = float(sys.argv[3]) if len(sys.argv) > 3 else 10

link = serial.Serial(port, 115200, timeout=0.2)
time.sleep(2)  # opening the port can reset the ESP32
start = time.monotonic()
for n in range(count):
    packet = {
        "id": "WQM-001", "ts": int((time.monotonic() - start) * 1000),
        "pH": round(random.uniform(6.8, 7.6), 2),
        "tds_ppm": round(random.uniform(250, 350), 1),
        "do_mgl": round(random.uniform(6.5, 8.9), 2),
        "orp_mv": round(random.uniform(80, 260), 1),
        "temp_w1": round(random.uniform(18, 25), 2),
        "temp_air": round(random.uniform(20, 30), 2),
        "pressure_hpa": round(random.uniform(1005, 1020), 2),
        "lat": 0.0, "lon": 0.0, "alt_m": 0.0,
        "sog_knots": 0.0, "cog_deg": 0.0, "gps_fix": 0, "sats": 0,
        "heading": round(random.uniform(0, 359.9), 1),
        "pitch": round(random.uniform(-2, 2), 1),
        "roll": round(random.uniform(-2, 2), 1),
    }
    link.write((json.dumps(packet, separators=(",", ":")) + "\n").encode())
    print(f"-> packet {n + 1}/{count}")
    deadline = time.monotonic() + interval
    while time.monotonic() < deadline:
        line = link.readline().decode(errors="replace").rstrip()
        if line:
            print("   " + line)
link.close()
