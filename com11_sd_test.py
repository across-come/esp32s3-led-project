# -*- coding: utf-8 -*-
"""COM9 看 IDF 日志, COM11 发 exe 测 SD."""
import serial
import threading
import time
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def safe(s):
    return s.encode("utf-8", errors="replace").decode("utf-8", errors="replace")


class Com9Tap(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.buf = []
        self.lock = threading.Lock()
        self.stop_f = False
        self.port = None

    def run(self):
        try:
            self.port = serial.Serial("COM9", 115200, timeout=0.2, write_timeout=1)
            self.port.dtr = False
            self.port.rts = False
        except Exception as e:
            print("COM9 OPEN_FAIL: %s" % e, flush=True)
            return
        while not self.stop_f:
            chunk = self.port.read(4096)
            if chunk:
                text = chunk.decode("utf-8", errors="replace")
                with self.lock:
                    self.buf.append(text)
                sys.stdout.write(text)
                sys.stdout.flush()
        self.port.close()

    def dump(self):
        with self.lock:
            return "".join(self.buf)

    def close(self):
        self.stop_f = True
        self.join(timeout=2)


def send(port, cmd, wait=1.2):
    print(">>>", cmd, flush=True)
    port.write((cmd + "\r\n").encode("ascii"))
    port.flush()
    rx = b""
    t0 = time.time()
    while time.time() - t0 < wait:
        chunk = port.read(4096)
        if chunk:
            rx += chunk
        else:
            time.sleep(0.03)
    text = rx.decode("utf-8", errors="replace")
    print("COM11:", safe(text) if text.strip() else "(no rx)", flush=True)
    return text


def main():
    tap = Com9Tap()
    tap.start()
    time.sleep(6)
    log = tap.dump()
    print("====JUDGE COM9==== overflow=%s mount=%s ready=%s fail=%s" % (
        "stack overflow" in log.lower(),
        ("fat32 mount ok" in log) or ("sd fat32 mount ok" in log),
        "task ready" in log,
        ("mount fail" in log) or ("sdspi mount err" in log),
    ), flush=True)

    try:
        p11 = serial.Serial("COM11", 115200, timeout=0.2, write_timeout=2)
        p11.dtr = False
        p11.rts = False
    except Exception as e:
        print("COM11 OPEN_FAIL: %s" % e, flush=True)
        tap.close()
        return

    send(p11, "exe=dbg_msg_send(msg:task_tcp_msg,dat:stop)", 0.8)
    send(p11, "exe=dbg_print_task_name", 0.8)
    send(p11, "exe=dbg_msg_send(msg:task_sd_msg,dat:sd?)", 0.8)
    send(p11, "exe=dbg_msg_send(msg:task_sd_msg,dat:append:[SDTEST T=1 R=1])", 1.5)
    send(p11, "exe=dbg_msg_send(msg:task_sd_msg,dat:append:[SDTEST T=2 R=2])", 1.5)
    send(p11, "exe=dbg_msg_send(msg:task_sd_msg,dat:sd?)", 0.8)
    send(p11, "exe=dbg_msg_send(msg:task_sd_msg,dat:get)", 2.0)
    send(p11, "exe=dbg_msg_send(msg:task_sd_msg,dat:del)", 2.0)
    send(p11, "exe=dbg_msg_send(msg:task_sd_msg,dat:sd?)", 0.8)
    p11.close()

    time.sleep(1.0)
    log2 = tap.dump()
    print("====JUDGE AFTER EXE==== get=%s del=%s append=%s overflow=%s" % (
        "get " in log2 or "cache ready" in log2,
        "del " in log2 or "left=" in log2,
        "append new file" in log2,
        "stack overflow" in log2.lower(),
    ), flush=True)
    tap.close()


if __name__ == "__main__":
    main()
