#!/usr/bin/env python3
"""Real kernel -> staging -> trampoline -> kernel, using scratch disks only."""
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time


def main():
    out = Path(sys.argv[1]).resolve(strict=True)
    before = sys.argv[2] == "before"
    work = Path(os.environ["OSITO_TEST_DISK_DIR"]).resolve(strict=True)
    serial = out / "serial.log"
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as port_socket:
        port_socket.bind(("127.0.0.1", 0))
        echo_port = port_socket.getsockname()[1]
    storage = os.environ.get("KEXEC_TEST_STORAGE", "nvme")
    assert storage in ("nvme", "usb")
    ram = os.environ.get("KEXEC_TEST_RAM", "2G")
    args = ["qemu-system-x86_64", "-name", "osito-kexec-test", "-machine", "q35,memory-backend=ram",
            "-object", f"memory-backend-ram,id=ram,size={ram},reserve=off",
            "-accel", "kvm", "-cpu", "host", "-m", ram,
            "-smp", "4", "-display", "none", "-vga", "std", "-monitor", "none",
            "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
            "-drive", f"if=pflash,format=raw,file={work / 'vars.fd'}",
            "-drive", f"if=ide,format=raw,file={work / 'esp.img'}",
            "-drive", f"if=none,format=raw,id=data,file={work / 'data.img'}",
            "-device", "qemu-xhci", "-device", "usb-kbd", "-device", "usb-tablet",
            "-device", "usb-storage,drive=data" if storage == "usb" else "nvme,drive=data,serial=kexec-test",
            "-netdev", f"user,id=net0,hostfwd=udp:127.0.0.1:{echo_port}-:7777",
            "-device", "virtio-net-pci,netdev=net0,disable-legacy=on",
            "-chardev", f"socket,id=uart,path={work / 'serial.sock'},server=on,wait=off,logfile={serial}",
            "-serial", "chardev:uart", "-qmp", f"unix:{work / 'qmp.sock'},server=on,wait=off",
            "-no-reboot", "-no-shutdown"]
    (out / "command.json").write_text(json.dumps(args, indent=2))
    results = {"kernel_sha256": hashlib.sha256((out / "kernel.elf").read_bytes()).hexdigest()}
    stop = threading.Event()
    uart = reader = None
    with (out / "qemu.log").open("wb") as log:
        process = subprocess.Popen(args, stdout=log, stderr=log)

    def qmp(command):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(5)
            s.connect(str(work / "qmp.sock"))
            f = s.makefile("rwb", buffering=0)
            f.readline()
            for request in ({"execute": "qmp_capabilities"}, command):
                f.write(json.dumps(request).encode() + b"\n")
                while True:
                    reply = json.loads(f.readline())
                    if "return" in reply: break
                    if "error" in reply: raise RuntimeError(reply)
            return reply["return"]

    def prompt(start=0, timeout=150):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            assert process.poll() is None, "QEMU exited"
            data = serial.read_bytes()[start:] if serial.exists() else b""
            if b"osito> " in data: return data
            time.sleep(.1)
        raise TimeoutError("kernel did not return to shell")

    def command(line, timeout=150):
        start = serial.stat().st_size
        for byte in (line + "\r").encode():
            uart.sendall(bytes([byte])); time.sleep(.05)
        return prompt(start, timeout)

    def drain():
        while not stop.is_set():
            try:
                if not uart.recv(65536): return
            except socket.timeout:
                pass
            except OSError:
                return

    def network():
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(.5)
            token = b"kexec-network-check"
            for _ in range(20):
                s.sendto(token, ("127.0.0.1", echo_port))
                for _ in range(2):
                    try:
                        if s.recv(2048) == token: return
                    except socket.timeout:
                        break
            raise AssertionError("UDP echo did not recover")

    def keyboard():
        start = serial.stat().st_size
        for key in "echo usbinput\n":
            key = {" ": "spc", "\n": "ret"}.get(key, key)
            for down in (True, False):
                qmp({"execute": "input-send-event", "arguments": {"events": [
                    {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": key}}}]}})
                time.sleep(.04)
        output = prompt(start).replace(b"\r", b"")
        assert b"\nusbinput\n" in output, output

    try:
        assert b"KexecTest" in prompt()
        uart = socket.socket(socket.AF_UNIX); uart.settimeout(.2)
        uart.connect(str(work / "serial.sock"))
        reader = threading.Thread(target=drain); reader.start()
        command("echo KEXEC-FILE > marker.txt")
        assert b"KEXEC-FILE" in command("cat marker.txt")
        if not before:
            for bad in ("missing.elf", "marker.txt", "bad.elf"):
                rejected = command(f"kexec {bad}")
                assert b"kexec:" in rejected and b"jumping to new kernel" not in rejected
                assert b"KEXEC-FILE" in command("cat marker.txt")
            network()
            keyboard()
            results["reject_invalid_keep_shell_io"] = True
        for iteration in range(1 if before else 2):
            try:
                data = command("kexec next.elf", 25 if before else 150)
            except TimeoutError:
                if not before: raise
                results["reproduced_old_hang"] = True
                print("CONFIRMED old kexec hangs before the next shell", flush=True)
                break
            assert not before, "old failure did not reproduce"
            assert b"[KEXEC-PATH] kernel_entry" in data and b"KexecTest" in data, data[-2000:]
            assert b"DOUBLE PAGE-FREE" not in data and b"#PF" not in data and b"SYSTEM HALTED" not in data
            assert b"KEXEC-FILE" in command("cat marker.txt")
            assert b"4 total CPU(s)" in data
            network()
            keyboard()
            results[f"reload_{iteration + 1}"] = True
            print(f"PASS reload {iteration + 1}: shell, {storage}, SMP, UDP, USB keyboard, no faults", flush=True)
    finally:
        try:
            regs = qmp({"execute": "human-monitor-command", "arguments": {"command-line": "info registers -a"}})
            (out / "registers.txt").write_text(regs)
        except Exception:
            pass
        stop.set()
        if uart: uart.close()
        if reader: reader.join(timeout=2)
        if process.poll() is None:
            try: qmp({"execute": "quit"})
            except Exception: process.terminate()
        try: process.wait(timeout=10)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
