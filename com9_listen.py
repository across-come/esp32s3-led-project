# -*- coding: utf-8 -*-
import serial
import time
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

p = serial.Serial("COM9", 115200, timeout=0.2, write_timeout=1)
p.dtr = False
p.rts = False
print("COM9 listen 12s", flush=True)
t0 = time.time()
buf = b""
while time.time() - t0 < 12:
    chunk = p.read(4096)
    if chunk:
        buf += chunk
        sys.stdout.write(chunk.decode("utf-8", errors="replace"))
        sys.stdout.flush()
p.close()
print("\n==== total bytes %d ====" % len(buf), flush=True)
