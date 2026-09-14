# -*- coding: utf-8 -*-
"""Boot idle, then COM11 exe: SD -> WiFi -> TCP -> data_gen."""
import serial
import threading
import time
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


class Tap(threading.Thread):
    def __init__(self, name):
        super().__init__(daemon=True)
        self.port_name = name
        self.buf = []
        self.lock = threading.Lock()
        self.stop_f = False
        self.tx = None
        self.ok = False

    def run(self):
        try:
            self.tx = serial.Serial(self.port_name, 115200, timeout=0.2, write_timeout=2)
            self.tx.dtr = False
            self.tx.rts = False
            self.ok = True
        except Exception as e:
            print("%s OPEN_FAIL: %s" % (self.port_name, e), flush=True)
            return
        while not self.stop_f:
            chunk = self.tx.read(4096)
            if chunk:
                text = chunk.decode("utf-8", errors="replace")
                with self.lock:
                    self.buf.append(text)
                sys.stdout.write(text)
                sys.stdout.flush()
        self.tx.close()

    def dump(self):
        with self.lock:
            return "".join(self.buf)

    def send(self, cmd):
        print(">>>", cmd, flush=True)
        self.tx.write((cmd + "\r\n").encode("ascii"))
        self.tx.flush()

    def close(self):
        self.stop_f = True
        self.join(timeout=2)


def wait_log(taps, needles, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        log = "".join(t.dump() for t in taps)
        for n in needles:
            if n in log:
                return True, log
        time.sleep(0.2)
    return False, "".join(t.dump() for t in taps)


def main():
    t9 = Tap("COM9")
    t11 = Tap("COM11")
    t9.start()
    t11.start()
    time.sleep(0.8)
    if not t11.ok:
        t9.close()
        t11.close()
        print("====FAIL==== COM11 not open", flush=True)
        return 1

    taps = [t9, t11]
    print("====WAIT BOOT====", flush=True)
    time.sleep(6.0)
    t11.send("exe=dbg_print_task_name")
    time.sleep(0.8)
    boot = "".join(t.dump() for t in taps)
    idle_ok = (
        ("wifi sta connecting" not in boot)
        and ("wifi sta ok" not in boot)
        and ("tcp connecting" not in boot)
        and ("tcp connected" not in boot)
        and ("tcp ok " not in boot)
        and ("sd fat32 mount ok" not in boot)
        and ("task:task_wifi first_run" not in boot)
        and ("task:task_tcp first_run" not in boot)
        and ("task:task_sd first_run" not in boot)
        and ("task:task_data first_run" not in boot)
    )
    print("====IDLE==== ok=%s" % idle_ok, flush=True)

    t11.send("exe=dbg_msg_send(msg:task_sd_msg,dat:setup)")
    sd_ok, _ = wait_log(taps, ["sd fat32 mount ok", "sd task ready"], 12.0)
    print("====SD==== ok=%s" % sd_ok, flush=True)

    t11.send("exe=dbg_msg_send(msg:task_wifi_msg,dat:wifi_sta:ssid=test_8266,pwd=12345678)")
    time.sleep(0.4)
    t11.send("exe=dbg_msg_send(msg:task_wifi_msg,dat:setup)")
    wifi_ok, _ = wait_log(taps, ["wifi sta ok"], 30.0)
    print("====WIFI==== ok=%s" % wifi_ok, flush=True)

    t11.send("exe=dbg_msg_send(msg:task_tcp_msg,dat:setup)")
    tcp_ok, _ = wait_log(taps, ["tcp ok ", "hello sent"], 25.0)
    print("====TCP==== ok=%s" % tcp_ok, flush=True)

    t11.send("exe=dbg_msg_send(msg:task_data_msg,dat:setup)")
    data_ok, log = wait_log(taps, ["tcp up fifo", "tcp upload cache ok", "[SEQ="], 20.0)
    time.sleep(8.0)
    log = "".join(t.dump() for t in taps)
    t9.close()
    t11.close()

    fifo = "tcp up fifo" in log
    cache = ("tcp upload cache ok" in log) or ("cache ready" in log)
    print("====CLOSED LOOP====", flush=True)
    print("idle=%s" % idle_ok, flush=True)
    print("overflow=%s" % ("stack overflow" in log.lower()), flush=True)
    print("sd_mount=%s" % sd_ok, flush=True)
    print("wifi_ok=%s" % wifi_ok, flush=True)
    print("tcp_ok=%s" % tcp_ok, flush=True)
    print("data_push=%s" % data_ok, flush=True)
    print("cache_replay=%s" % cache, flush=True)
    print("fifo_live=%s" % fifo, flush=True)
    ok = idle_ok and sd_ok and wifi_ok and tcp_ok and data_ok
    print("PASS=%s" % ok, flush=True)
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
