# -*- coding: utf-8 -*-
"""COM11 hardware SPI: data_gen 30s offline -> scan -> STA -> TCP 24701, record times."""
import re
import serial
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SSID = "1111"
PWD = "1234567890"
TCP_IP = "115.120.239.161"
TCP_PORT = 24701
OFFLINE_SEC = 30.0


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


def parse_ticks(text):
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

    return pair(lcd_s, lcd_e), pair(sd_s, sd_ready)


def stats(name, samples):
    if not samples:
        print("====%s==== none" % name, flush=True)
        return
    samples = [x for x in samples if x >= 0]
    print("====%s==== n=%u min=%u max=%u avg=%.1f ms  samples=%s" % (
        name, len(samples), min(samples), max(samples),
        sum(samples) / float(len(samples)), samples), flush=True)


def count_seq(text):
    return len(re.findall(r"\[SEQ=", text))


def main():
    wall = {}
    p = open_com11()
    log = Log()
    t_all = time.time()
    print("====WAIT BOOT (hw SPI LCD+SD)====", flush=True)
    boot = wait_any(p, log, ["wifi_init ok", "spi2 bus init ok", "data_gen_init ok"], 15.0)
    pump(p, log, 1.0)
    print("====BOOT==== hit=%s" % boot, flush=True)

    mark = len(log.text())
    t0 = time.time()
    send(p, log, "exe=dbg_msg_send(msg:task_sd_msg,dat:setup)", 0.3)
    sd_ok = wait_new(p, log, "sd task ready", 15.0, mark) or (
        "sd fat32 mount ok" in log.text()[mark:])
    wall["sd_setup_s"] = time.time() - t0
    print("====SD==== ok=%s t=%.1fs" % (sd_ok, wall["sd_setup_s"]), flush=True)

    mark = len(log.text())
    t0 = time.time()
    send(p, log, "exe=dbg_msg_send(msg:task_data_msg,dat:setup)", 0.4)
    data_start_ok = wait_new(p, log, "task:task_data first_run", 5.0, mark) or (
        "task_data is running" in log.text()[mark:])
    print("====DATA_GEN start==== ok=%s (no TCP yet)" % data_start_ok, flush=True)

    print("====WAIT %ds offline (data_gen -> SD, no TCP)====" % int(OFFLINE_SEC), flush=True)
    t0 = time.time()
    pump(p, log, OFFLINE_SEC)
    wall["offline_s"] = time.time() - t0
    seq_offline = count_seq(log.text()[mark:])
    print("====OFFLINE==== wait=%.1fs seq_seen=%u" % (wall["offline_s"], seq_offline), flush=True)

    mark = len(log.text())
    t0 = time.time()
    send(p, log, "exe=dbg_msg_send(msg:task_wifi_msg,dat:wifi_scan)", 0.4)
    send(p, log, "exe=dbg_msg_send(msg:task_wifi_msg,dat:setup)", 0.3)
    scan_ok = wait_new(p, log, "wifi scan display done", 20.0, mark)
    wait_new(p, log, "task:task_lcd run_quit", 8.0, mark)
    wall["scan_s"] = time.time() - t0
    print("====SCAN==== ok=%s has_1111=%s t=%.1fs" % (
        scan_ok, SSID in log.text()[mark:], wall["scan_s"]), flush=True)

    mark = len(log.text())
    t0 = time.time()
    send(p, log, "exe=dbg_msg_send(msg:task_wifi_msg,dat:wifi_sta:ssid=%s,pwd=%s)" % (SSID, PWD), 0.4)
    send(p, log, "exe=dbg_msg_send(msg:task_wifi_msg,dat:setup)", 0.3)
    wifi_ok = wait_new(p, log, "wifi sta ok", 40.0, mark)
    wait_new(p, log, "task:task_lcd run_quit", 8.0, mark)
    wall["wifi_s"] = time.time() - t0
    print("====WIFI==== ok=%s t=%.1fs" % (wifi_ok, wall["wifi_s"]), flush=True)
    if not wifi_ok:
        p.close()
        lcd, sd = parse_ticks(log.text())
        stats("LCD_MS", lcd)
        stats("SD_MOUNT_MS", sd)
        print("====FAIL wifi====", flush=True)
        return 2

    mark = len(log.text())
    t0 = time.time()
    send(p, log, "exe=dbg_msg_send(msg:task_tcp_msg,dat:tcp:ip=%s,port=%u)" % (TCP_IP, TCP_PORT), 0.5)
    send(p, log, "exe=dbg_msg_send(msg:task_tcp_msg,dat:setup)", 0.3)
    tcp_ok = wait_new(p, log, "hello sent", 25.0, mark) or (
        "tcp ok " in log.text()[mark:])
    wait_new(p, log, "task:task_lcd run_quit", 8.0, mark)
    wall["tcp_hello_s"] = time.time() - t0
    print("====TCP==== ok=%s dst=%s:%u hello_t=%.1fs" % (
        tcp_ok, TCP_IP, TCP_PORT, wall["tcp_hello_s"]), flush=True)
    if not tcp_ok:
        p.close()
        lcd, sd = parse_ticks(log.text())
        stats("LCD_MS", lcd)
        stats("SD_MOUNT_MS", sd)
        print("====FAIL tcp====", flush=True)
        return 3

    t1 = time.time()
    up_ok = wait_new(p, log, "tcp upload cache ok", 25.0, mark) or (
        "tcp up fifo" in log.text()[mark:])
    pump(p, log, 12.0)
    wall["tcp_up_s"] = time.time() - t1
    wall["total_s"] = time.time() - t_all
    p.close()

    allj = log.text()
    m_pclk = re.search(r"lcd spi2 hw[^\r\n]+", allj)
    if not m_pclk:
        m_pclk = re.search(r"lcd spi pclk[^\r\n]+", allj)
    print("====PCLK==== %s" % (m_pclk.group(0) if m_pclk else "not_in_log"), flush=True)
    lcd, sd = parse_ticks(allj)
    stats("LCD_MS", lcd)
    stats("SD_MOUNT_MS", sd)
    print("====WALL==== sd=%.1fs offline=%.1fs scan=%.1fs wifi=%.1fs tcp_hello=%.1fs tcp_up=%.1fs total=%.1fs" % (
        wall.get("sd_setup_s", 0), wall.get("offline_s", 0), wall.get("scan_s", 0),
        wall.get("wifi_s", 0), wall.get("tcp_hello_s", 0), wall.get("tcp_up_s", 0),
        wall.get("total_s", 0)), flush=True)
    print("====JUDGE==== scan=%s wifi=%s tcp=%s sd=%s data_start=%s seq_offline=%u up=%s" % (
        scan_ok, wifi_ok, tcp_ok, sd_ok, data_start_ok, seq_offline, up_ok), flush=True)
    print("rx=%s fifo=%s cache_up=%s ack_to=%s pending=%s" % (
        "tcp rx len=" in allj,
        "tcp up fifo" in allj,
        "tcp upload cache ok" in allj,
        "tcp ack timeout" in allj,
        "cache:pending" in allj or "sd notify" in allj), flush=True)
    ok = scan_ok and wifi_ok and tcp_ok and sd_ok
    print("PASS=%s" % ok, flush=True)
    return 0 if ok else 4


if __name__ == "__main__":
    sys.exit(main())
