# -*- coding: utf-8 -*-
"""Phase 2: COM11 boot + LVGL task + SD while UI task alive."""
import serial
import subprocess
import time
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PY = r"D:\prj\soft\Espressif\python_env\idf5.1_py3.11_env\Scripts\python.exe"
ESPTOOL = r"D:\prj\soft\Espressif\frameworks\esp-idf-v5.1.2\components\esptool_py\esptool\esptool.py"
BUILD = r"D:\prj\prj\esp32s3_project\01_led\build"


def flash_com9():
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
        raise RuntimeError("flash fail %s" % r.returncode)


def safe(s):
    return s.encode("utf-8", errors="replace").decode("utf-8", errors="replace")


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
    print(safe(text) if text.strip() else "(no rx)", flush=True)
    return text


def has_crash(text):
    low = text.lower()
    keys = ("guru meditation", "abort()", "stack overflow", "storeprohibited",
            "loadprohibited", "panic", "task_wdt: task watchdog")
    return [k for k in keys if k in low]


def main():
    all_log = []
    try:
        p = serial.Serial("COM11", 115200, timeout=0.2, write_timeout=2)
        p.dtr = False
        p.rts = False
    except Exception as e:
        print("COM11 OPEN_FAIL:", e, flush=True)
        return 2

    boot = ""
    if "--flash" in sys.argv:
        flash_com9()
        boot = read_for(p, 8.0)
    elif "--reset" in sys.argv:
        print("==== RESET via COM9 ====", flush=True)
        try:
            subprocess.run(
                [PY, ESPTOOL, "--chip", "esp32s3", "-p", "COM9",
                 "--before", "default_reset", "--after", "hard_reset", "chip_id"],
                timeout=20, check=False,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
        except Exception as e:
            print("reset warn:", e, flush=True)
        boot = read_for(p, 8.0)
    else:
        boot = read_for(p, 0.5)
    all_log.append(boot)
    print("==== BOOT ====", flush=True)
    print(safe(boot) if boot.strip() else "(no boot rx)", flush=True)

    ver = send(p, "version", 1.0)
    tasks1 = send(p, "exe=dbg_print_task()", 1.2)
    sd_q = send(p, "exe=dbg_msg_send(msg:task_sd_msg,dat:sd?)", 0.8)
    setup = send(p, "exe=dbg_msg_send(msg:task_sd_msg,dat:setup)", 6.0)
    a1 = send(p, "exe=dbg_msg_send(msg:task_sd_msg,dat:append:[P2 T=1 R=1])", 1.5)
    a2 = send(p, "exe=dbg_msg_send(msg:task_sd_msg,dat:append:[P2 T=2 R=2])", 1.5)
    a3 = send(p, "exe=dbg_msg_send(msg:task_sd_msg,dat:append:[P2 T=3 R=3])", 1.5)
    # SPI share: LCD DMA wait + SD write in the same cooperative loop
    pix = send(p, "exe=dbg_msg_send(msg:task_lcd_msg,dat:pixel:x=319,y=239,color=0xffff)", 0.6)
    done = send(p, "exe=dbg_msg_send(msg:task_lcd_msg,dat:setup_done)", 1.2)
    a4 = send(p, "exe=dbg_msg_send(msg:task_sd_msg,dat:append:[P2 T=4 R=4])", 1.5)
    got = send(p, "exe=dbg_msg_send(msg:task_sd_msg,dat:get)", 2.0)
    deleted = send(p, "exe=dbg_msg_send(msg:task_sd_msg,dat:del)", 2.0)
    sd_q2 = send(p, "exe=dbg_msg_send(msg:task_sd_msg,dat:sd?)", 0.8)
    tasks2 = send(p, "exe=dbg_print_task()", 1.2)
    alive = send(p, "version", 1.0)
    extra = read_for(p, 1.0)
    p.close()

    blob = "".join([
        boot, ver, tasks1, sd_q, setup, a1, a2, a3, pix, done, a4,
        got, deleted, sd_q2, tasks2, alive, extra,
    ])
    all_log.append(blob)
    crash = has_crash(blob)

    lvgl_boot = "lvgl home ready" in blob
    lvgl_task = "task_lvgl" in blob
    lvgl_after = tasks2.count("task_lvgl") > 0
    sd_ok = ("fat32 mount ok" in blob) or ("sd task ready" in blob)
    sd_fail = ("sd mount fail" in blob) or ("sdspi mount err" in blob)
    append_ok = ("append" in blob.lower()) or ("new file" in blob) or ("P2 T=" in blob)
    get_ok = ("cache:ready" in blob) or ("get " in blob) or ("cache ready" in blob)
    still_alive = "version" in alive.lower() or "esp-idf" in alive.lower() or "00_basic" in alive or len(alive.strip()) > 0

    print("==== JUDGE ====", flush=True)
    print("lvgl_boot=%s lvgl_task=%s lvgl_after_sd=%s" % (lvgl_boot, lvgl_task, lvgl_after), flush=True)
    print("sd_ok=%s sd_fail=%s append=%s get=%s" % (sd_ok, sd_fail, append_ok, get_ok), flush=True)
    print("alive=%s crash=%s" % (still_alive, crash if crash else "none"), flush=True)

    ok = lvgl_task and lvgl_after and sd_ok and (not sd_fail) and (not crash) and still_alive
    print("PHASE2=%s" % ("PASS" if ok else "FAIL"), flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
