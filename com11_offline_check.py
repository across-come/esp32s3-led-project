# -*- coding: utf-8 -*-
import serial
import time
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def send(p, cmd, wait=1.2):
    print(">>>", cmd, flush=True)
    p.write((cmd + "\r\n").encode("ascii"))
    p.flush()
    t0 = time.time()
    rx = b""
    while time.time() - t0 < wait:
        c = p.read(4096)
        if c:
            rx += c
        else:
            time.sleep(0.03)
    t = rx.decode("utf-8", errors="replace")
    print(t if t.strip() else "(no rx)", flush=True)
    return t


p = serial.Serial("COM11", 115200, timeout=0.2, write_timeout=2)
p.dtr = False
p.rts = False
time.sleep(0.3)
boot = p.read(16384)
print("====LIVE====", flush=True)
print(boot.decode("utf-8", errors="replace") if boot else "(empty)", flush=True)

send(p, "exe=dbg_print_task_name", 0.8)
send(p, "exe=dbg_msg_send(msg:task_tcp_msg,dat:tcp?)", 0.6)
send(p, "exe=dbg_msg_send(msg:task_sd_msg,dat:sd?)", 0.8)
send(p, "exe=dbg_msg_send(msg:task_sd_msg,dat:sd?)", 6.5)
p.close()
