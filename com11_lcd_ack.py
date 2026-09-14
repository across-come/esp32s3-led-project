# -*- coding: utf-8 -*-
"""COM11 only (avoid COM9 USB-JTAG reset). Huawei STA + TCP, judge LCD/ack text."""
import serial
import time
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def main():
    p = serial.Serial("COM11", 115200, timeout=0.2, write_timeout=2)
    p.dtr = False
    p.rts = False
    buf = []

    def dump():
        return "".join(buf)

    def pump(sec):
        t0 = time.time()
        while time.time() - t0 < sec:
            c = p.read(4096)
            if c:
                t = c.decode("utf-8", errors="replace")
                buf.append(t)
                sys.stdout.write(t)
                sys.stdout.flush()
            else:
                time.sleep(0.03)

    def send(cmd, wait=0.4):
        print(">>>", cmd, flush=True)
        p.write((cmd + "\r\n").encode("ascii"))
        p.flush()
        pump(wait)

    def wait_for(needles, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            log = dump()
            for n in needles:
                if n in log:
                    return True
            pump(0.2)
        return False

    print("====WAIT BOOT====", flush=True)
    pump(5.0)
    boot = dump()
    idle_ok = (
        ("wifi sta connecting" not in boot)
        and ("tcp connecting" not in boot)
        and ("task:task_wifi first_run" not in boot)
        and ("task:task_tcp first_run" not in boot)
    )
    print("====IDLE==== %s" % idle_ok, flush=True)

    send("exe=dbg_msg_send(msg:task_wifi_msg,dat:wifi_sta:ssid=HUAWEI Mate60 Pro,pwd=12345678)", 0.5)
    send("exe=dbg_msg_send(msg:task_wifi_msg,dat:setup)", 0.3)
    wifi_ok = wait_for(["wifi sta ok"], 30.0)
    print("====WIFI==== %s" % wifi_ok, flush=True)

    mark = len(dump())
    send("exe=dbg_msg_send(msg:task_tcp_msg,dat:setup)", 0.3)
    tcp_ok = wait_for(["hello sent", "tcp ok "], 20.0)
    print("====TCP HELLO==== %s" % tcp_ok, flush=True)

    send("exe=dbg_msg_send(msg:task_data_msg,dat:setup)", 0.3)
    pump(22.0)

    log = dump()
    after = log[mark:] if mark < len(log) else log
    lcd_nk = "task_lcd is running, cmd NK" in after
    ack_serial = "tcp ack timeout" in after
    ack_lcd = "txt=tcp ack timeout" in after
    fail_lcd = "txt=tcp connect fail" in after
    hello_n = after.count("hello sent")
    print("====JUDGE====", flush=True)
    print("idle=%s" % idle_ok, flush=True)
    print("wifi_ok=%s" % wifi_ok, flush=True)
    print("tcp_hello=%s" % tcp_ok, flush=True)
    print("hello_n=%u" % hello_n, flush=True)
    print("lcd_cmd_nk=%s" % lcd_nk, flush=True)
    print("ack_serial=%s" % ack_serial, flush=True)
    print("ack_lcd=%s" % ack_lcd, flush=True)
    print("connect_fail_lcd=%s" % fail_lcd, flush=True)
    ok = idle_ok and wifi_ok and tcp_ok and (not lcd_nk) and ack_serial and ack_lcd and (not fail_lcd)
    print("PASS=%s" % ok, flush=True)
    p.close()
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
