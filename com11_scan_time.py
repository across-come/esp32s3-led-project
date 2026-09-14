# -*- coding: utf-8 -*-
"""COM11: scan -> STA 1111 -> TCP 115.120.239.161:27278 -> data_gen.
Parse dbgtx tick timestamps for LCD refresh and SD write."""
import re
import serial
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SSID = "1111"
PWD = "1234567890"
TCP_IP = "115.120.239.161"
TCP_PORT = 27278


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


def parse_ticks(text):
    """Pair by tmr value (UART may print run_quit before first_run)."""
    lcd_s, lcd_e = [], []
    sd_s, sd_ready = [], []
    for m in re.finditer(
            r"task:(\S+) first_run tmr=(\d+)|task:(\S+) run_quit tmr=(\d+)|(\d+) sd task ready",
            text):
        if m.group(1):
            if m.group(1) == "task_lcd":
                lcd_s.append(int(m.group(2)))
            elif m.group(1) == "task_sd":
                sd_s.append(int(m.group(2)))
        elif m.group(3):
            if m.group(3) == "task_lcd":
                lcd_e.append(int(m.group(4)))
        elif m.group(5):
            sd_ready.append(int(m.group(5)))

    def pair(starts, ends):
        starts = sorted(starts)
        ends = sorted(ends)
        out = []
        j = 0
        for s in starts:
            while j < len(ends) and ends[j] < s:
                j += 1
            if j < len(ends):
                out.append(ends[j] - s)
                j += 1
        return out

    sd = pair(sd_s, sd_ready)
    return pair(lcd_s, lcd_e), sd


def stats(name, samples):
    if not samples:
        print("====%s==== none" % name, flush=True)
        return
    samples = [x for x in samples if x >= 0]
    avg = sum(samples) / float(len(samples))
    print("====%s==== n=%u min=%u max=%u avg=%.1f ms  samples=%s" % (
        name, len(samples), min(samples), max(samples), avg, samples), flush=True)


def main():
    p = open_com11()
    log = Log()
    print("====WAIT BOOT====", flush=True)
    wait_any(p, log, ["wifi_init ok", "data_gen_init ok", "tcp_init ok"], 12.0)
    pump(p, log, 1.5)

    send(p, log, "exe=dbg_print_task", 0.8)

    mark = len(log.text())
    send(p, log, "exe=dbg_msg_send(msg:task_wifi_msg,dat:wifi_scan)", 0.4)
    send(p, log, "exe=dbg_msg_send(msg:task_wifi_msg,dat:setup)", 0.3)
    scan_ok = wait_new(p, log, "wifi scan display done", 15.0, mark)
    wait_new(p, log, "task:task_lcd run_quit", 8.0, mark)
    pump(p, log, 0.8)
    print("====SCAN==== ok=%s has_1111=%s" % (
        scan_ok, SSID in log.text()[mark:]), flush=True)

    mark = len(log.text())
    send(p, log, "exe=dbg_msg_send(msg:task_sd_msg,dat:setup)", 0.3)
    sd_ok = wait_new(p, log, "sd task ready", 15.0, mark) or (
        "sd fat32 mount ok" in log.text()[mark:])
    print("====SD==== ok=%s" % sd_ok, flush=True)

    mark = len(log.text())
    send(p, log, "exe=dbg_msg_send(msg:task_wifi_msg,dat:wifi_sta:ssid=%s,pwd=%s)" % (SSID, PWD), 0.4)
    send(p, log, "exe=dbg_msg_send(msg:task_wifi_msg,dat:setup)", 0.3)
    wifi_ok = wait_new(p, log, "wifi sta ok", 35.0, mark)
    wait_new(p, log, "task:task_lcd run_quit", 8.0, mark)
    print("====WIFI==== ok=%s" % wifi_ok, flush=True)
    if not wifi_ok:
        p.close()
        lcd, sd = parse_ticks(log.text())
        stats("LCD_MS", lcd)
        stats("SD_WRITE_MS", sd)
        print("====FAIL wifi====", flush=True)
        return 2

    mark = len(log.text())
    send(p, log, "exe=dbg_msg_send(msg:task_tcp_msg,dat:tcp:ip=%s,port=%u)" % (TCP_IP, TCP_PORT), 0.5)
    send(p, log, "exe=dbg_msg_send(msg:task_tcp_msg,dat:setup)", 0.3)
    tcp_ok = wait_new(p, log, "hello sent", 25.0, mark) or (
        "tcp ok " in log.text()[mark:])
    wait_new(p, log, "task:task_lcd run_quit", 8.0, mark)
    print("====TCP==== ok=%s dst=%s:%u" % (tcp_ok, TCP_IP, TCP_PORT), flush=True)
    if not tcp_ok:
        p.close()
        lcd, sd = parse_ticks(log.text())
        stats("LCD_MS", lcd)
        stats("SD_WRITE_MS", sd)
        print("====FAIL tcp====", flush=True)
        return 3

    mark = len(log.text())
    send(p, log, "exe=dbg_msg_send(msg:task_data_msg,dat:setup)", 0.4)
    t0 = time.time()
    data_ok = False
    while time.time() - t0 < 20.0:
        c = p.read(4096)
        if c:
            log.add(c.decode("utf-8", errors="replace"))
            chunk = log.text()[mark:]
            if ("[SEQ=" in chunk) or ("tcp up fifo" in chunk):
                data_ok = True
                break
        else:
            time.sleep(0.03)
    pump(p, log, 12.0)
    p.close()

    allj = log.text()
    m_pclk = re.search(r"lcd spi2 hw[^\r\n]+", allj)
    if not m_pclk:
        m_pclk = re.search(r"lcd spi pclk[^\r\n]+", allj)
    print("====PCLK==== %s" % (m_pclk.group(0) if m_pclk else "not_in_log"), flush=True)
    lcd, sd = parse_ticks(allj)
    stats("LCD_MS", lcd)
    stats("SD_WRITE_MS", sd)
    print("====JUDGE====", flush=True)
    print("scan=%s wifi=%s tcp=%s data=%s" % (scan_ok, wifi_ok, tcp_ok, data_ok), flush=True)
    print("rx=%s fifo=%s cache_up=%s ack_to=%s" % (
        "tcp rx len=" in allj,
        "tcp up fifo" in allj,
        "tcp upload cache ok" in allj,
        "tcp ack timeout" in allj), flush=True)
    ok = scan_ok and wifi_ok and tcp_ok
    print("PASS=%s" % ok, flush=True)
    return 0 if ok else 4


if __name__ == "__main__":
    sys.exit(main())
