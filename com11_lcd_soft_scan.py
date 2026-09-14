# -*- coding: utf-8 -*-
"""COM11: software-SPI LCD timing. scan -> STA only, no SD, no TCP."""
import re
import serial
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SSID = "1111"
PWD = "1234567890"


def open_com11():
    p = serial.Serial("COM11", 115200, timeout=0.2, write_timeout=2)
    p.dtr = False
    p.rts = False
    return p


class Log:
    def __init__(self):
        self.parts = []

    def add(self, t):
        self.parts.append(t)
        sys.stdout.write(t)
        sys.stdout.flush()

    def text(self):
        return "".join(self.parts)


def pump(port, log, sec):
    t0 = time.time()
    while time.time() - t0 < sec:
        c = port.read(4096)
        if c:
            log.add(c.decode("utf-8", errors="replace"))
        else:
            time.sleep(0.03)


def send(port, log, cmd, wait=0.6):
    print(">>>", cmd, flush=True)
    port.write((cmd + "\r\n").encode("ascii"))
    port.flush()
    pump(port, log, wait)


def wait_any(port, log, needles, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        c = port.read(4096)
        if c:
            log.add(c.decode("utf-8", errors="replace"))
            t = log.text()
            for n in needles:
                if n in t:
                    return n
        else:
            time.sleep(0.03)
    return None


def wait_new(port, log, needle, timeout, start_len):
    if needle in log.text()[start_len:]:
        return True
    t0 = time.time()
    while time.time() - t0 < timeout:
        c = port.read(4096)
        if c:
            log.add(c.decode("utf-8", errors="replace"))
            if needle in log.text()[start_len:]:
                return True
        else:
            time.sleep(0.03)
    return False


def parse_lcd_ms(text):
    starts, ends = [], []
    for m in re.finditer(
            r"task:task_lcd first_run tmr=(\d+)|task:task_lcd run_quit tmr=(\d+)",
            text):
        if m.group(1):
            starts.append(int(m.group(1)))
        else:
            ends.append(int(m.group(2)))
    starts.sort()
    ends.sort()
    out = []
    j = 0
    for s in starts:
        while j < len(ends) and ends[j] < s:
            j += 1
        if j < len(ends):
            out.append(ends[j] - s)
            j += 1
    return out


def stats(name, samples):
    if not samples:
        print("====%s==== none" % name, flush=True)
        return
    samples = [x for x in samples if x >= 0]
    print("====%s==== n=%u min=%u max=%u avg=%.1f ms  samples=%s" % (
        name, len(samples), min(samples), max(samples),
        sum(samples) / float(len(samples)), samples), flush=True)


def main():
    p = open_com11()
    log = Log()
    print("====WAIT BOOT (soft SPI LCD, no TCP/SD)====", flush=True)
    wait_any(p, log, ["wifi_init ok", "spi soft init ok", "data_gen_init ok"], 20.0)
    pump(p, log, 1.0)

    mark = len(log.text())
    send(p, log, "exe=dbg_msg_send(msg:task_wifi_msg,dat:wifi_scan)", 0.4)
    send(p, log, "exe=dbg_msg_send(msg:task_wifi_msg,dat:setup)", 0.3)
    scan_ok = wait_new(p, log, "wifi scan display done", 20.0, mark)
    lcd1 = wait_new(p, log, "task:task_lcd run_quit", 90.0, mark)
    pump(p, log, 0.5)
    print("====SCAN==== ok=%s lcd_quit=%s has_1111=%s" % (
        scan_ok, lcd1, SSID in log.text()[mark:]), flush=True)

    mark = len(log.text())
    send(p, log, "exe=dbg_msg_send(msg:task_wifi_msg,dat:wifi_sta:ssid=%s,pwd=%s)" % (SSID, PWD), 0.4)
    send(p, log, "exe=dbg_msg_send(msg:task_wifi_msg,dat:setup)", 0.3)
    wifi_ok = wait_new(p, log, "wifi sta ok", 40.0, mark)
    wait_new(p, log, "task:task_lcd run_quit", 90.0, mark)
    pump(p, log, 8.0)
    print("====WIFI==== ok=%s" % wifi_ok, flush=True)

    p.close()
    lcd = parse_lcd_ms(log.text())
    stats("LCD_SOFT_MS", lcd)
    print("====JUDGE==== scan=%s wifi=%s tcp=skipped sd=skipped" % (scan_ok, wifi_ok), flush=True)
    print("PASS=%s" % (scan_ok and wifi_ok), flush=True)
    return 0 if (scan_ok and wifi_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
