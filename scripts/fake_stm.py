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
        "pH": round(random.uniform(6.8, 7.6), 2), "turb_ntu": round(random.uniform(20, 60), 1),
        "tds_ppm": random.randint(250, 350), "do_mgl": round(random.uniform(6, 8), 2),
        "ec_uscm": round(random.uniform(500, 700), 1), "orp_mv": random.randint(180, 240),
        "temp_w1": round(random.uniform(18, 24), 2), "temp_w2": round(random.uniform(18, 24), 2),
        "depth_cm": round(random.uniform(20, 50), 1), "temp_air": round(random.uniform(20, 32), 1),
        "humidity": round(random.uniform(40, 80), 1), "pressure_hpa": round(random.uniform(1005, 1020), 2),
    }
    link.write((json.dumps(packet, separators=(",", ":")) + "\n").encode())
    print(f"-> packet {n + 1}/{count}")
    deadline = time.monotonic() + interval
    while time.monotonic() < deadline:
        line = link.readline().decode(errors="replace").rstrip()
        if line:
            print("   " + line)
link.close()
