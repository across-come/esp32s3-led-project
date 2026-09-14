# -*- coding: utf-8 -*-
"""Phase 3: COM11 key-msg UI nav + scan smoke."""
import serial
import subprocess
import time
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PY = r"D:\prj\soft\Espressif\python_env\idf5.1_py3.11_env\Scripts\python.exe"
ESPTOOL = r"D:\prj\soft\Espressif\frameworks\esp-idf-v5.1.2\components\esptool_py\esptool\esptool.py"
BUILD = r"D:\prj\prj\esp32s3_project\01_led\build"


def read_for(port, sec):
    rx = b""
    t0 = time.time()
    while time.time() - t0 < sec:
        chunk = port.read(4096)
        if chunk:
            rx += chunk
        else:
            time.sleep(0.03)
    return rx.decode("utf-8", errors="replace")


def send(port, cmd, wait):
    print(">>>", cmd, flush=True)
    port.reset_input_buffer()
    port.write((cmd + "\r\n").encode("ascii"))
    port.flush()
    text = read_for(port, wait)
    print(text if text.strip() else "(no rx)", flush=True)
    return text


def main():
    p = serial.Serial("COM11", 115200, timeout=0.2, write_timeout=2)
    p.dtr = False
    p.rts = False

    print("==== FLASH COM9 ====", flush=True)
    cmd = [
        PY, ESPTOOL, "--chip", "esp32s3", "-p", "COM9", "-b", "460800",
        "--before=default_reset", "--after=hard_reset", "write_flash",
        "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
        "0x0", BUILD + r"\bootloader\bootloader.bin",
        "0x10000", BUILD + r"\00_basic.bin",
        "0x8000", BUILD + r"\partition_table\partition-table.bin",
    ]
    r = subprocess.run(cmd, timeout=120)
    if r.returncode != 0:
        print("FLASH FAIL", flush=True)
        return 2

    boot = read_for(p, 8.0)
    print("==== BOOT ====", flush=True)
    print(boot, flush=True)

    blob = [boot]
    blob.append(send(p, "version", 1.0))
    blob.append(send(p, "exe=dbg_print_task()", 1.0))
    blob.append(send(p, "exe=dbg_msg_send(msg:task_key_msg,dat:short:2)", 0.6))
    blob.append(send(p, "exe=dbg_msg_send(msg:task_key_msg,dat:short:2)", 0.6))
    blob.append(send(p, "exe=dbg_msg_send(msg:task_key_msg,dat:short:1)", 0.6))
    blob.append(send(p, "exe=dbg_msg_send(msg:task_key_msg,dat:short:0)", 0.8))
    blob.append(send(p, "exe=dbg_msg_send(msg:task_key_msg,dat:long:1)", 12.0))
    blob.append(send(p, "exe=dbg_msg_send(msg:task_key_msg,dat:short:3)", 0.8))
    blob.append(send(p, "exe=dbg_msg_send(msg:task_key_msg,dat:long:3)", 0.8))
    blob.append(send(p, "exe=dbg_print_task()", 1.0))
    blob.append(send(p, "version", 1.0))
    p.close()

    text = "".join(blob)
    low = text.lower()
    crash = any(k in low for k in ("guru meditation", "abort()", "stack overflow", "panic"))
    ok = (
        ("lvgl ui ready" in text)
        and ("task_lvgl" in text)
        and ("key1 long" in text or "key wifi scan" in text)
        and ("wifi scan" in text)
        and (not crash)
    )
    print("==== JUDGE ====", flush=True)
    print("ui_ready=%s lvgl_task=%s scan=%s crash=%s" % (
        "lvgl ui ready" in text, "task_lvgl" in text,
        "wifi scan done" in text or "wifi scan start" in text, crash), flush=True)
    print("PHASE3=%s" % ("PASS" if ok else "FAIL"), flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
