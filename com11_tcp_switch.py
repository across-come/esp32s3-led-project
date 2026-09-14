# -*- coding: utf-8 -*-
import serial
import socket
import time
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

DST = ("115.120.239.161", 27362)


def probe():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(6)
    try:
        s.connect(DST)
        print("PC_CONNECT_OK", s.getsockname(), "->", s.getpeername(), flush=True)
        s.sendall(b"[PC HELLO]\r\n")
        s.settimeout(3)
        try:
            print("PC_RX", s.recv(256), flush=True)
        except socket.timeout:
            print("PC_RX_TIMEOUT", flush=True)
        s.close()
    except Exception as e:
        print("PC_CONNECT", type(e).__name__, e, flush=True)


def main():
    probe()
    p = serial.Serial("COM11", 115200, timeout=0.2, write_timeout=2)
    p.dtr = False
    p.rts = False

    def pump(sec):
        t0 = time.time()
        while time.time() - t0 < sec:
            c = p.read(4096)
            if c:
                sys.stdout.write(c.decode("utf-8", errors="replace"))
                sys.stdout.flush()
            else:
                time.sleep(0.03)

    def send(cmd, wait=0.7):
        print(">>>", cmd, flush=True)
        p.write((cmd + "\r\n").encode("ascii"))
        p.flush()
        pump(wait)

    send("exe=dbg_msg_send(msg:task_tcp_msg,dat:stop)", 0.9)
    send("exe=dbg_msg_send(msg:task_tcp_msg,dat:tcp:ip=115.120.239.161,port=27362)", 0.5)
    send("exe=dbg_msg_send(msg:task_tcp_msg,dat:setup)", 0.4)

    log = []
    hello = fail = ack = rx = False
    t0 = time.time()
    while time.time() - t0 < 20:
        c = p.read(4096)
        if c:
            t = c.decode("utf-8", errors="replace")
            log.append(t)
            sys.stdout.write(t)
            sys.stdout.flush()
            allj = "".join(log)
            if ("hello sent" in allj) and ("27362" in allj):
                hello = True
            if ("tcp connect timeout" in allj) or ("tcp connect err" in allj):
                fail = True
            if "tcp ack timeout" in allj:
                ack = True
            if "tcp rx len=" in allj:
                rx = True
                pump(1.5)
                break
            if hello and (time.time() - t0 > 14):
                break
        else:
            time.sleep(0.03)
    p.close()
    print("====JUDGE==== hello=%s fail=%s rx=%s ack_to=%s" % (hello, fail, rx, ack), flush=True)
    return 0 if hello else 2


if __name__ == "__main__":
    sys.exit(main())
