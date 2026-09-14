# -*- coding: utf-8 -*-
"""Boot closed loop: SD mount -> WiFi STA -> TCP HELLO -> cache replay -> FIFO."""
import serial
import threading
import time
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


class Tap(threading.Thread):
    def __init__(self, name):
        super().__init__(daemon=True)
        self.name = name
        self.buf = []
        self.lock = threading.Lock()
        self.stop_f = False

    def run(self):
        try:
            p = serial.Serial(self.name, 115200, timeout=0.2, write_timeout=1)
            p.dtr = False
            p.rts = False
        except Exception as e:
            print("%s OPEN_FAIL: %s" % (self.name, e), flush=True)
            return
        while not self.stop_f:
            chunk = p.read(4096)
            if chunk:
                text = chunk.decode("utf-8", errors="replace")
                with self.lock:
                    self.buf.append(text)
                sys.stdout.write(text)
                sys.stdout.flush()
        p.close()

    def dump(self):
        with self.lock:
            return "".join(self.buf)

    def close(self):
        self.stop_f = True
        self.join(timeout=2)


def main():
    t9 = Tap("COM9")
    t11 = Tap("COM11")
    t9.start()
    t11.start()
    t0 = time.time()
    timeout = 45.0
    log = ""
    while time.time() - t0 < timeout:
        time.sleep(0.4)
        log = t11.dump() + "\n" + t9.dump()
        if ("tcp ok " in log) or ("hello sent" in log):
            if ("upload cache ok" in log) or ("tcp up fifo" in log) or ("cache ready" in log):
                time.sleep(6.0)
                break
            # connected; wait a bit more for first data
            if time.time() - t0 > 25:
                time.sleep(6.0)
                break
    log = t11.dump() + "\n" + t9.dump()
    t9.close()
    t11.close()

    print("\n====CLOSED LOOP====", flush=True)
    print("overflow=%s" % ("stack overflow" in log.lower()), flush=True)
    print("sd_mount=%s" % ("fat32 mount ok" in log), flush=True)
    print("sd_ready=%s" % ("task ready" in log), flush=True)
    print("wifi_ok=%s" % ("wifi sta ok" in log), flush=True)
    print("tcp_ok=%s" % (("tcp ok " in log) or ("hello sent" in log)), flush=True)
    print("cache_replay=%s" % (("upload cache ok" in log) or ("cache ready" in log)), flush=True)
    print("fifo_live=%s" % ("tcp up fifo" in log), flush=True)
    print("cache_del=%s" % (("del 0:/CACHE" in log) or ("cache del" in log)), flush=True)


if __name__ == "__main__":
    main()
