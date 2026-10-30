#!/usr/bin/env python3
"""Boot a supplied kernel on fresh scratch disks and exercise desktop input.

Run under qemu-test-disks.sh. Captures are real GOP output; no P1 connection.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import socket
import subprocess
import sys
import threading
import time

boot = Path(sys.argv[1]).resolve(strict=True)
kernel = Path(sys.argv[2]).resolve(strict=True)
out = Path(sys.argv[3]).resolve()
out.mkdir(exist_ok=False)
work = Path(os.environ["OSITO_TEST_DISK_DIR"]).resolve(strict=True)
for name, size in (("esp.img", 128 << 20), ("data.img", 128 << 20)):
    with (work / name).open("xb") as disk:
        disk.truncate(size)
subprocess.run(["mformat", "-i", str(work / "esp.img"), "-F", "::"], check=True)
subprocess.run(["mmd", "-i", str(work / "esp.img"), "::/EFI", "::/EFI/BOOT"], check=True)
subprocess.run(["mcopy", "-i", str(work / "esp.img"), str(boot), "::/EFI/BOOT/BOOTX64.EFI"], check=True)
subprocess.run(["mcopy", "-i", str(work / "esp.img"), str(kernel), "::/EFI/BOOT/kernel.elf"], check=True)
subprocess.run(["tools/ositofs/mkfs.ositofs3", str(work / "data.img"), "--label", "GuiTest", "--inodes", "4096"], check=True)
(work / "noautoload").touch()
subprocess.run(["tools/ositofs/ositofs-write3", str(work / "data.img"), str(work / "noautoload"), "--dest", "/.noautoload"], check=True)
fixture = Path(sys.argv[4]).resolve(strict=True) if len(sys.argv) > 4 else None
if fixture:
    subprocess.run(["tools/ositofs/ositofs-write3", str(work / "data.img"), str(fixture), "--dest", "/guiwm.exe"], check=True)
shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", work / "vars.fd")
serial = out / "serial.log"
command = ["qemu-system-x86_64", "-name", "osito-gui-aero-test", "-machine", "q35,memory-backend=ram",
    "-object", "memory-backend-ram,id=ram,size=2G,reserve=off", "-accel", "kvm", "-cpu", "host", "-m", "2G",
    "-smp", "4", "-display", "none", "-vga", "std", "-monitor", "none", "-nic", "none",
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", f"if=pflash,format=raw,file={work / 'vars.fd'}",
    "-drive", f"if=ide,format=raw,file={work / 'esp.img'}",
    "-drive", f"if=none,format=raw,id=data,file={work / 'data.img'}",
    "-device", "nvme,drive=data,serial=gui-aero-test", "-device", "qemu-xhci", "-device", "usb-kbd", "-device", "usb-tablet",
    "-chardev", f"socket,id=uart,path={work / 'serial.sock'},server=on,wait=off,logfile={serial}", "-serial", "chardev:uart",
    "-qmp", f"unix:{work / 'qmp.sock'},server=on,wait=off", "-no-reboot", "-no-shutdown"]
(out / "command.json").write_text(json.dumps(command, indent=2) + "\n")
record = {"kernel_sha256": hashlib.sha256(kernel.read_bytes()).hexdigest(), "captures": [], "passed": []}
stop = threading.Event()
uart = reader = None
with (out / "qemu.log").open("wb") as log:
    process = subprocess.Popen(command, stdout=log, stderr=log)


def gui_qmp(command, arguments=None):
    with socket.socket(socket.AF_UNIX) as channel:
        channel.settimeout(10)
        channel.connect(str(work / "qmp.sock"))
        stream = channel.makefile("rwb", buffering=0)
        json.loads(stream.readline())
        for request in ({"execute": "qmp_capabilities"},
                        {"execute": command, "arguments": arguments or {}}):
            stream.write(json.dumps(request).encode() + b"\n")
            while True:
                reply = json.loads(stream.readline())
                if "error" in reply:
                    raise RuntimeError(reply["error"])
                if "return" in reply:
                    break
        return reply["return"]


def gui_wait_serial(marker, start=0, timeout=150):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        assert process.poll() is None, "QEMU exited"
        data = serial.read_bytes()[start:].replace(b"\r", b"") if serial.exists() else b""
        assert b"!!! EXCEPTION" not in data and b"SYSTEM HALTED" not in data, data[-1500:]
        if marker in data:
            return data
        time.sleep(.1)
    raise TimeoutError(f"missing marker {marker!r}")


def gui_uart_drain():
    while not stop.is_set():
        try:
            if not uart.recv(65536):
                return
        except socket.timeout:
            pass
        except OSError:
            return


def gui_qemu_key(code, held=None):
    if held:
        gui_qmp("input-send-event", {"events": [{"type": "key", "data": {
            "down": True, "key": {"type": "qcode", "data": held}}}]})
        time.sleep(.08)
    for down in (True, False):
        gui_qmp("input-send-event", {"events": [{"type": "key", "data": {
            "down": down, "key": {"type": "qcode", "data": code}}}]})
        time.sleep(.12)
    if held:
        gui_qmp("input-send-event", {"events": [{"type": "key", "data": {
            "down": False, "key": {"type": "qcode", "data": held}}}]})
        time.sleep(.08)


def gui_qemu_click(x, y):
    gui_qmp("input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": round(x * 32767 / (width - 1))}},
        {"type": "abs", "data": {"axis": "y", "value": round(y * 32767 / (height - 1))}}]})
    time.sleep(.25)
    for down in (True, False):
        gui_qmp("input-send-event", {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]})
        time.sleep(.2)


def gui_qemu_capture(name):
    time.sleep(1.2)
    target = out / (name + ".ppm")
    gui_qmp("screendump", {"filename": str(target)})
    data = target.read_bytes()
    match = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    assert match, "expected PPM screenshot"
    size = tuple(int(part) for part in match.groups())
    record["captures"].append({"name": name, "size": size, "sha256": hashlib.sha256(data).hexdigest()})
    print(json.dumps({"capture": name, "size": size}), flush=True)
    return size


try:
    gui_wait_serial(b"osito> ")
    uart = socket.socket(socket.AF_UNIX)
    uart.settimeout(.2); uart.connect(str(work / "serial.sock"))
    reader = threading.Thread(target=gui_uart_drain); reader.start()
    if os.environ.get("OSITO_GUI_SELFTEST") == "1":
        for command, marker in ((b"win32-wm-test", b"USER32 window-model test: PASS"),
                                (b"win32-input-test", b"USER32 SendInput test: PASS")):
            start = serial.stat().st_size
            for byte in command + b"\r":
                uart.sendall(bytes([byte])); time.sleep(.03)
            gui_wait_serial(marker, start)
            gui_wait_serial(b"osito> ", start)
            record["passed"].append(marker.decode())
    start = serial.stat().st_size
    for byte in b"desktop\r":
        uart.sendall(bytes([byte])); time.sleep(.05)
    gui_wait_serial(b"Desktop launched. Compositor running.", start)
    width, height = gui_qemu_capture("01-desktop")
    assert (width, height) == (1280, 800), "scripted pointer positions target the standard 1280x800 GOP mode"
    if fixture:
        record["fixture_sha256"] = hashlib.sha256(fixture.read_bytes()).hexdigest()
        driver = os.environ.get("OSITO_GUI_FIXTURE_DRIVER",
                                "arch/x86/scripts/gui-window-integration-driver.py")
        record["fixture_driver"] = driver
        runpy.run_path(driver, init_globals=globals())
        raise SystemExit(0)
    start = serial.stat().st_size
    gui_qemu_click(width / 2 + 24, height - 39)
    gui_wait_serial(b"[COMP] Dock icon=1", start, 15)
    gui_qemu_capture("02-system-open")
    record["passed"].append("open System through dock pointer input")
    if os.environ.get("OSITO_GUI_RESPONSIVE_TEST") == "1":
        runpy.run_path("arch/x86/scripts/gui-responsive-qemu-driver.py", init_globals=globals())
        raise SystemExit(0)
    system_y = 30 + (height - 30 - 76 - 426) // 2
    if system_y < 42:
        system_y = 42
    gui_qemu_click(width - 47, system_y + 17)
    gui_qemu_capture("03-system-closed")
    gui_qemu_click(width / 2 + 24, height - 39)
    gui_qemu_capture("04-system-restored")
    gui_qemu_click(width - 115, system_y + 17)
    gui_qemu_capture("05-system-minimized")
    gui_qemu_click(width / 2 + 24, height - 39)
    gui_qemu_capture("06-system-restored")
    gui_qemu_click(width - 84, system_y + 17)
    gui_qemu_capture("06a-system-maximized")
    gui_qemu_click(width - 64, 47)
    gui_qemu_capture("06b-system-unmaximized")
    gui_qemu_key("right")
    gui_qemu_capture("07-taskbar")
    gui_qemu_key("tab"); gui_qemu_key("ret")
    gui_qemu_capture("08-scale-menu")
    gui_qemu_key("down"); gui_qemu_key("down"); gui_qemu_key("ret")
    gui_qemu_capture("09-scale-125")
    gui_qemu_key("tab", "shift"); gui_qemu_key("right")
    gui_qemu_capture("10-dock-125")
    gui_qemu_key("tab", "alt")
    start = serial.stat().st_size
    for char in "echo guilive\n":
        gui_qemu_key({" ": "spc", "\n": "ret"}.get(char, char))
    gui_wait_serial(b"\nguilive\n", start, 15)
    record["passed"].append("USB keyboard returns to terminal after appearance changes")
    gui_qemu_capture("11-terminal-keyboard")
    data = serial.read_bytes()
    assert b"!!! EXCEPTION" not in data and b"SYSTEM HALTED" not in data and b"DOUBLE PAGE-FREE" not in data
    record["passed"].append("desktop session completed without a fatal diagnostic")
finally:
    stop.set()
    if uart:
        uart.close()
    if reader:
        reader.join(timeout=2)
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill(); process.wait(timeout=5)
    (out / "results.json").write_text(json.dumps(record, indent=2) + "\n")
