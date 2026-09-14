# -*- coding: utf-8 -*-
import serial
import time
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

p = serial.Serial("COM11", 115200, timeout=0.2, write_timeout=2)
p.dtr = False
p.rts = False
log = []


def pump(sec):
    t0 = time.time()
    while time.time() - t0 < sec:
        c = p.read(4096)
        if c:
            t = c.decode("utf-8", errors="replace")
            log.append(t)
            sys.stdout.write(t)
            sys.stdout.flush()
        else:
            time.sleep(0.03)


def send(cmd, wait=0.8):
    print(">>>", cmd, flush=True)
    p.write((cmd + "\r\n").encode("ascii"))
    p.flush()
    pump(wait)


send("exe=dbg_msg_send(msg:task_sd_msg,dat:sd?)", 0.8)
send("exe=dbg_print_task", 0.8)
send("exe=dbg_msg_send(msg:task_tcp_msg,dat:tcp?)", 0.5)

text = "".join(log)
need_sd = ("sd task=idle" in text) or ("task_sd" not in text)
print("====NEED_SD_SETUP==== %s" % need_sd, flush=True)

if need_sd or "sd task=idle" in text:
    send("exe=dbg_msg_send(msg:task_sd_msg,dat:setup)", 0.4)
    pump(8.0)

send("exe=dbg_msg_send(msg:task_sd_msg,dat:sd?)", 1.0)
pump(25.0)

allj = "".join(log)
print("====JUDGE====", flush=True)
print("sd_idle_first=%s" % ("sd task=idle" in text), flush=True)
print("mount=%s" % ("fat32 mount ok" in allj), flush=True)
print("cache_init=%s" % ("cache init ok" in allj), flush=True)
print("pending=%s" % ("cache:pending" in allj or "sd get " in allj), flush=True)
print("cache_ready=%s" % ("cache ready" in allj or "sd get " in allj), flush=True)
print("upload_ok=%s" % ("upload cache ok" in allj), flush=True)
print("cache_del=%s" % ("cache del " in allj or "del 0:/CACHE" in allj), flush=True)
print("fifo=%s" % ("tcp up fifo" in allj), flush=True)
print("ack_to=%s" % ("tcp ack timeout" in allj), flush=True)
print("qfull=%s" % ("sd append q full" in allj), flush=True)
p.close()
