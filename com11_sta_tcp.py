# -*- coding: utf-8 -*-
"""PC-side serial test: WiFi STA connect -> TCP upload -> data generation."""
import re
import serial
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SSID = "<YOUR_WIFI_SSID>"
PWD = "<YOUR_WIFI_PASSWORD>"
TCP_IP = "<SERVER_IP>"
TCP_PORT = <SERVER_PORT>


def main():
    p = serial.Serial("COM11", 115200, timeout=0.2, write_timeout=2)
    p.dtr = False
    p.rts = False
    log = []

    def add(t):
        log.append(t)
        sys.stdout.write(t)
        sys.stdout.flush()

    def pump(sec):
        t0 = time.time()
        while time.time() - t0 < sec:
            c = p.read(4096)
            if c:
                add(c.decode("utf-8", errors="replace"))
            else:
                time.sleep(0.03)

    def send(cmd, wait=0.5):
        print(">>>", cmd, flush=True)
        p.write((cmd + "\r\n").encode("ascii"))
        p.flush()
        pump(wait)

    def wait_new(needle, timeout, start):
        t0 = time.time()
        while time.time() - t0 < timeout:
            c = p.read(4096)
            if c:
                add(c.decode("utf-8", errors="replace"))
                if needle in "".join(log)[start:]:
                    return True
            else:
                time.sleep(0.03)
        return False

    pump(0.8)
    send("exe=dbg_msg_send(msg:task_wifi_msg,dat:stop)", 0.6)
    mark = len("".join(log))
    send("exe=dbg_msg_send(msg:task_wifi_msg,dat:wifi_sta:ssid=%s,pwd=%s)" % (SSID, PWD), 0.5)
    send("exe=dbg_msg_send(msg:task_wifi_msg,dat:setup)", 0.3)
    wifi_ok = wait_new("wifi sta ok", 35.0, mark)
    wait_new("task:task_lcd run_quit", 6.0, mark)
    print("====WIFI==== ok=%s" % wifi_ok, flush=True)
    if not wifi_ok:
        p.close()
        dump("".join(log))
        return 2

    mark = len("".join(log))
    send("exe=dbg_msg_send(msg:task_tcp_msg,dat:tcp:ip=%s,port=%u)" % (TCP_IP, TCP_PORT), 0.5)
    send("exe=dbg_msg_send(msg:task_tcp_msg,dat:setup)", 0.3)
    tcp_ok = wait_new("hello sent", 25.0, mark)
    wait_new("task:task_lcd run_quit", 6.0, mark)
    print("====TCP==== ok=%s" % tcp_ok, flush=True)
    if not tcp_ok:
        p.close()
        dump("".join(log))
        return 3

    mark = len("".join(log))
    send("exe=dbg_msg_send(msg:task_data_msg,dat:setup)", 0.4)
    data_ok = False
    t0 = time.time()
    while time.time() - t0 < 20.0:
        c = p.read(4096)
        if c:
            add(c.decode("utf-8", errors="replace"))
            chunk = "".join(log)[mark:]
            if ("[SEQ=" in chunk) or ("tcp up fifo" in chunk) or ("tcp upload cache ok" in chunk):
                data_ok = True
                break
        else:
            time.sleep(0.03)
    pump(12.0)
    p.close()
    dump("".join(log))
    print("====JUDGE==== wifi=%s tcp=%s data=%s" % (wifi_ok, tcp_ok, data_ok), flush=True)
    print("PASS=%s" % (wifi_ok and tcp_ok), flush=True)
    return 0 if wifi_ok and tcp_ok else 4


def dump(text):
    lcd = []
    sd = []
    pending = {}
    sd_start = None
    for m in re.finditer(
            r"task:(\S+) first_run_quit tmr=(\d+)|task:(\S+) run_quit tmr=(\d+)|(\d+) sd task ready",
            text):
        if m.group(1):
            pending[m.group(1)] = int(m.group(2))
            if m.group(1) == "task_sd":
                sd_start = int(m.group(2))
        elif m.group(3):
            name = m.group(3)
            end = int(m.group(4))
            if name == "task_lcd" and name in pending:
                lcd.append(end - pending[name])
                del pending[name]
        elif m.group(5) and sd_start is not None:
            sd.append(int(m.group(5)) - sd_start)
            sd_start = None

    def stats(name, samples):
        if not samples:
            print("====%s==== none" % name, flush=True)
            return
        avg = sum(samples) / float(len(samples))
        print("====%s==== n=%u min=%u max=%u avg=%.1f ms samples=%s" % (
            name, len(samples), min(samples), max(samples), avg, samples), flush=True)

    stats("LCD_MS", lcd)
    stats("SD_MS", sd)
    print("rx=%s fifo=%s cache_up=%s ack_to=%s" % (
        "tcp rx len=" in text,
        "tcp up fifo" in text,
        "tcp upload cache ok" in text,
        "tcp ack timeout" in text), flush=True)


if __name__ == "__main__":
    sys.exit(main())
