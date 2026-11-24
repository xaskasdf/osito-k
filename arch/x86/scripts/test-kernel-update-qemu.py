#!/usr/bin/env python3
"""Install, cold boot and hotreload using one disposable GPT USB disk."""
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
    work = Path(os.environ["OSITO_TEST_DISK_DIR"]).resolve(strict=True)
    serial = out / "serial.log"
    args = ["qemu-system-x86_64", "-name", "osito-kupdate-test", "-machine", "q35",
            "-accel", "kvm", "-cpu", "host", "-m", "2G", "-smp", "4",
            "-display", "none", "-vga", "std", "-monitor", "none",
            "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
            "-drive", f"if=pflash,format=raw,file={work / 'vars.fd'}",
            "-drive", f"if=none,format=raw,id=usb,file={work / 'usb.img'}",
            "-device", "qemu-xhci", "-device", "usb-storage,drive=usb,bootindex=1",
            "-device", "usb-kbd", "-device", "usb-tablet",
            "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0,disable-legacy=on",
            "-chardev", f"socket,id=uart,path={work / 'serial.sock'},server=on,wait=off,logfile={serial}",
            "-serial", "chardev:uart", "-qmp", f"unix:{work / 'qmp.sock'},server=on,wait=off",
            "-no-shutdown"]
    (out / "command.json").write_text(json.dumps(args, indent=2))
    results = {"kernel_sha256": hashlib.sha256((out / "kernel.elf").read_bytes()).hexdigest(),
               "updated_sha256": hashlib.sha256((out / "next.elf").read_bytes()).hexdigest()}
    stop = threading.Event()
    uart = reader = None
    with (out / "qemu.log").open("wb") as log:
        process = subprocess.Popen(args, stdout=log, stderr=log)

    def qmp(command):
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(10)
            sock.connect(str(work / "qmp.sock"))
            stream = sock.makefile("rwb", buffering=0)
            stream.readline()
            for request in ({"execute": "qmp_capabilities"}, {"execute": command}):
                stream.write(json.dumps(request).encode() + b"\n")
                while True:
                    reply = json.loads(stream.readline())
                    if "return" in reply:
                        break
                    if "error" in reply:
                        raise RuntimeError(reply)

    def prompt(start=0, timeout=180):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            assert process.poll() is None, "QEMU exited"
            data = serial.read_bytes()[start:] if serial.exists() else b""
            if b"osito> " in data:
                return data
            time.sleep(.1)
        raise TimeoutError("kernel did not return to shell")

    def command(line):
        start = serial.stat().st_size
        for byte in (line + "\r").encode():
            uart.sendall(bytes([byte]))
            time.sleep(.03)
        data = prompt(start)
        print(data.decode(errors="replace")[-1800:], flush=True)
        return data

    def drain():
        while not stop.is_set():
            try:
                if not uart.recv(65536):
                    return
            except socket.timeout:
                pass
            except OSError:
                return

    try:
        assert b"KupdateTest" in prompt()
        uart = socket.socket(socket.AF_UNIX)
        uart.settimeout(.2)
        uart.connect(str(work / "serial.sock"))
        reader = threading.Thread(target=drain)
        reader.start()
        assert b"no writes performed" in command("kupdate --status")
        assert b"kexec cancelled" in command("kupdate --file /bad.elf")
        assert b"kexec cancelled" in command("kupdate --file /missing.elf")
        installed = command("kupdate --file /next.elf --no-kexec")
        assert b"EFI kernel verified; cold boot updated" in installed
        assert b"persistent update complete" in installed
        results["install_verified"] = True
        start = serial.stat().st_size
        qmp("system_reset")
        assert b"KupdateTest" in prompt(start)
        assert b"no writes performed" in command("kupdate --status")
        results["cold_boot"] = True
        reloaded = command("kupdate --file /next.elf")
        assert b"EFI kernel already current" in reloaded
        assert b"[KEXEC-PATH] kernel_entry" in reloaded and b"KupdateTest" in reloaded
        assert b"no writes performed" in command("kupdate --status")
        results["hotreload"] = True
        assert b"#PF" not in serial.read_bytes() and b"SYSTEM HALTED" not in serial.read_bytes()
        print("PASS update, reboot from USB, repeated update and kexec", flush=True)
    finally:
        stop.set()
        if uart:
            uart.close()
        if reader:
            reader.join(timeout=2)
        if process.poll() is None:
            try:
                qmp("quit")
            except Exception:
                process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
