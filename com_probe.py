# -*- coding: utf-8 -*-
import serial
import time
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

for name in ("COM11", "COM8", "COM9"):
    print("==== %s ====" % name, flush=True)
    try:
        p = serial.Serial(name, 115200, timeout=0.25, write_timeout=1)
        p.dtr = False
        p.rts = False
        time.sleep(0.25)
        boot = p.read(16384)
        print("open_ok bytes=%d" % len(boot), flush=True)
        print(boot.decode("utf-8", errors="replace") if boot else "(empty)", flush=True)
        p.close()
    except Exception as e:
        print("OPEN_FAIL: %s %s" % (type(e).__name__, e), flush=True)
