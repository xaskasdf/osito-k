#!/usr/bin/env python3
"""Exercise the production kdownload over SLIRP, with controlled UDP loss."""

import hashlib
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import threading
import time


def main():
    out = Path(sys.argv[1]).resolve(strict=True)
    work = Path(os.environ["OSITO_TEST_DISK_DIR"]).resolve(strict=True)
    serial_log = out / "serial.log"
    serial_path = work / "serial.sock"
    qmp_path = work / "qmp.sock"
    disk = work / "net-test.img"
    payload = bytes((i * 37 + i // 251 + 11) % 256 for i in range(4097))
    (out / "expected.bin").write_bytes(payload)
    events, server_errors = [], []
    stop = threading.Event()
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind(("127.0.0.1", 0))
    server.settimeout(0.2)
    port = server.getsockname()[1]

    def chunk(addr, offset, count, total=len(payload)):
        header = b"OFTD" + struct.pack(">III", total, offset, count)
        server.sendto(header + payload[offset:offset + count], addr)
        time.sleep(0.005)

    def stream(addr, start, size=700):
        for offset in range(start, len(payload), size):
            chunk(addr, offset, min(size, len(payload) - offset))
        chunk(addr, len(payload), 0)

    def serve():
        active = {}
        requests = {}
        try:
            while not stop.is_set():
                try:
                    data, addr = server.recvfrom(2048)
                except socket.timeout:
                    continue
                if data[:4] == b"OFTQ":
                    name = data[4:].rstrip(b"\0").decode("ascii")
                    active[addr] = name
                    requests[name] = requests.get(name, 0) + 1
                    events.append({"request": name, "attempt": requests[name]})
                    if name == "lost-request.bin" and requests[name] == 1:
                        continue
                    if name == "loss.bin":
                        chunk(addr, 0, 1400)
                        chunk(addr, 2800, len(payload) - 2800)
                        chunk(addr, len(payload), 0)
                    elif name == "eof-first.bin":
                        chunk(addr, len(payload), 0)
                        chunk(addr, 2800, len(payload) - 2800)
                    elif name == "changed-size.bin":
                        chunk(addr, 0, 1400)
                        chunk(addr, 1400, 1400, len(payload) + 1)
                    else:
                        stream(addr, 0)
                elif data[:4] == b"OFTN" and len(data) == 8:
                    offset = struct.unpack(">I", data[4:])[0]
                    events.append({"resume": active.get(addr), "offset": offset})
                    if offset <= len(payload):
                        stream(addr, offset)
        except Exception as error:
            server_errors.append(repr(error))

    thread = threading.Thread(target=serve)
    thread.start()
    argv = [
        "qemu-system-x86_64", "-name", "osito-oftp-regression",
        "-machine", "q35", "-accel", "kvm", "-cpu", "host", "-m", "2G", "-smp", "4",
        "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
        "-drive", f"if=pflash,format=raw,file={work / 'vars.fd'}",
        "-drive", f"format=raw,if=ide,file={work / 'esp.img'}",
        "-drive", f"format=raw,if=none,id=data,file={disk}",
        "-device", "nvme,serial=oftp-test,drive=data",
        "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0,disable-legacy=on",
        "-display", "none", "-vga", "std", "-monitor", "none",
        "-chardev", f"socket,id=uart,path={serial_path},server=on,wait=off,logfile={serial_log}",
        "-serial", "chardev:uart", "-qmp", f"unix:{qmp_path},server=on,wait=off",
        "-no-reboot", "-no-shutdown",
    ]
    (out / "command.json").write_text(json.dumps(argv, indent=2))
    results = {"kernel_sha256": hashlib.sha256((out / "kernel.elf").read_bytes()).hexdigest()}
    process = None
    serial_channel = None
    serial_reader = None
    serial_errors = []

    def drain_uart():
        try:
            with (out / "uart-wire.bin").open("xb") as wire:
                while not stop.is_set():
                    try:
                        data = serial_channel.recv(65536)
                    except socket.timeout:
                        continue
                    if not data:
                        break
                    wire.write(data)
        except Exception as error:
            serial_errors.append(repr(error))

    def wait_prompt(start=0, timeout=180):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before prompt")
            raw = serial_log.read_bytes()[start:] if serial_log.exists() else b""
            if b"KERNEL PANIC" in raw or b"SYSTEM HALTED" in raw:
                raise RuntimeError("kernel halted")
            # UART may duplicate a character. Only prompt recognition is
            # tolerant; persisted payloads and diagnostic files stay exact.
            if re.search(rb"(?:^|[\r\n])o+s+i+t+o+> ", raw):
                return raw
            if server_errors:
                raise RuntimeError(server_errors)
            if serial_errors:
                raise RuntimeError(serial_errors)
            time.sleep(0.1)
        diagnostics = {}
        for cmd in ("info registers -a", "info irq"):
            try:
                diagnostics[cmd] = qmp_command({"execute": "human-monitor-command",
                                                "arguments": {"command-line": cmd}})
            except Exception as error:
                diagnostics[cmd] = repr(error)
        (out / "timeout-qmp.json").write_text(json.dumps(diagnostics, indent=2))
        raise TimeoutError("no prompt")

    def command(line):
        start = serial_log.stat().st_size
        for byte in (line + "\r").encode():
            serial_channel.sendall(bytes([byte]))
            time.sleep(0.08)
        return wait_prompt(start, 90)

    def qmp_command(request):
        with socket.socket(socket.AF_UNIX) as channel:
            channel.settimeout(5)
            channel.connect(str(qmp_path))
            stream_file = channel.makefile("rwb", buffering=0)
            json.loads(stream_file.readline())
            for cmd in ({"execute": "qmp_capabilities"}, request):
                stream_file.write(json.dumps(cmd).encode() + b"\n")
                while True:
                    reply = json.loads(stream_file.readline())
                    if "return" in reply:
                        break
                    if "error" in reply:
                        raise RuntimeError(reply)
            return reply["return"]

    def quit_qemu():
        qmp_command({"execute": "quit"})

    good = ("normal.bin", "loss.bin", "eof-first.bin", "lost-request.bin")
    try:
        with (out / "qemu.log").open("xb") as log:
            process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
        raw = wait_prompt()
        assert b"NetTest" in raw and b"Auto-launch disabled" in raw
        assert b"[NIC] backend: virtio-net" in raw
        results["boot"] = True
        print("PASS boot, OsitoFS3 and virtio-net", flush=True)
        # QEMU's chardev logfile is not a consumer of the socket. Keep one
        # peer and drain it continuously, or guest TX can stall on backpressure.
        serial_channel = socket.socket(socket.AF_UNIX)
        serial_channel.settimeout(0.2)
        serial_channel.connect(str(serial_path))
        serial_reader = threading.Thread(target=drain_uart)
        serial_reader.start()
        raw = command("ifconfig")
        assert b"eth0: 10.0.2.15" in raw and b"Driver: virtio-net; link up" in raw
        raw = command("route")
        assert b"10.0.2.0/255.255.255.0" in raw and b"10.0.2.2" in raw
        assert b"(gateway)" not in raw
        results["network_diagnostics"] = True
        raw = command("wlan selftest")
        (out / "wlan-selftest-console.log").write_bytes(raw)
        assert b"WPA2 selftest: PASS (6/6)" in raw and b": FAIL" not in raw
        command("wlan selftest > wlan-selftest.log")
        if (out / "firmware.ucode").exists():
            for line in ("wlan", "wlan probe"):
                raw = command(line)
                assert b"no supported AX200 (8086:2723) detected" in raw, raw[-4096:]
            raw = command("wlan firmware /iwlwifi-cc-a0-77.ucode")
            (out / "wlan-firmware.log").write_bytes(raw)
            assert b"API-77 TLV layout valid; firmware NOT loaded" in raw, raw[-4096:]
            assert b"14/15/19; paging bytes: 585728; command versions: 192" in raw
            raw = command("wlan firmware /invalid-fw.bin")
            assert b"not an Intel TLV .ucode image" in raw, raw[-4096:]
            raw = command("wlan firmware /missing.ucode")
            assert b"firmware file not found" in raw, raw[-4096:]
            raw = command("wlan fwboot /iwlwifi-cc-a0-77.ucode")
            (out / "wlan-fwboot.log").write_bytes(raw)
            assert b"no supported AX200 (8086:2723)" in raw, raw[-4096:]
            assert b"ALIVE ABI: 6; RX packets: 0" in raw, raw[-4096:]
            assert b"Firmware ALIVE notification validated" not in raw
            raw = command("wlan fwboot /invalid-fw.bin")
            assert b"not an Intel TLV .ucode image" in raw, raw[-4096:]
            for mode in ("nvm", "nvmtrace", "start", "starttrace"):
                command(f"wlan {mode} /iwlwifi-cc-a0-77.ucode > wlan-{mode}.log")
            command("wlan status > wlan-runtime-status.log")
            command("wlan start /invalid-fw.bin > wlan-start-invalid.log")
            command("wlan start /missing.ucode > wlan-start-missing.log")
            raw = command("wlan stop")
            assert b"no pending WLAN DMA" in raw and b"DMA allocations released" in raw
            raw = command("kexec /missing.elf")
            assert b"File not found" in raw and b"WLAN busy" not in raw
            raw = command("wlan stop")
            assert b"no pending WLAN DMA" in raw  # Aborted kexec released the lock.
            raw = command("wlan connect")
            assert b"Wi-Fi is not the selected NIC" in raw and b"Wi-Fi>" not in raw
            raw = command("wlan connect wpa2 bad")
            assert b"Usage: wlan connect" in raw and b"WPA2 password" not in raw
            assert b"wlan connect wpa2 <BSSID>" in raw and b"wlan connect open <BSSID>" in raw
            assert b"open|wpa2" not in raw
            raw = command("wlan connect wpa2 02:00:00:00:00:01")
            assert b"Wi-Fi is not the selected NIC" in raw and b"WPA2 password" not in raw
            command("wlan scan > wlan-scan-absent.log")
            command("wlan networks > wlan-networks-absent.log")
            command("wlan abort > wlan-abort-absent.log")
            raw = command("wlan associate open bad")
            assert b"Usage: wlan associate wpa2 <BSSID>" in raw
            assert b"wlan associate open <BSSID>" in raw and b"open|wpa2" not in raw
            command("wlan associate wpa2 02:00:00:00:00:01 > wlan-assoc-absent.log")
            results["wlan_absence_and_firmware"] = True
            print("PASS WLAN absence, firmware read/validation and honest unsupported commands", flush=True)
        for name in good:
            raw = command(f"kdownload 10.0.2.2 {port} {name} {name}")
            (out / (name + ".log")).write_bytes(raw)
            assert b"kdownload: saved -> " + name.encode() in raw, raw[-4096:]
            assert b"kdownload: FAILED" not in raw
            results[name] = True
            print("PASS kdownload " + name, flush=True)
        raw = command(f"kdownload 10.0.2.2 {port} changed-size.bin changed-size.bin")
        (out / "changed-size.log").write_bytes(raw)
        assert b"kdownload: FAILED" in raw and b"saved ->" not in raw
        results["changed_size_rejected"] = True
        assert {"resume": "loss.bin", "offset": 1400} in events
        assert {"resume": "eof-first.bin", "offset": 0} in events
        assert {"request": "lost-request.bin", "attempt": 2} in events
        quit_qemu()
        process.wait(timeout=10)
        subprocess.run(["tools/ositofs/ositofs-read3", str(disk), "--output-dir",
                        str(out / "readback")], check=True, timeout=60)
        for name in good:
            actual = (out / "readback" / name).read_bytes()
            assert actual == payload, name
        assert not (out / "readback" / "changed-size.bin").exists()
        raw = (out / "readback" / "wlan-selftest.log").read_bytes()
        assert b"WPA2 selftest: PASS (6/6)" in raw and raw.count(b": PASS") == 7
        assert b": FAIL" not in raw
        results["wpa2_native_selftest"] = True
        if (out / "firmware.ucode").exists():
            for mode in ("nvm", "nvmtrace", "start", "starttrace"):
                raw = (out / "readback" / f"wlan-{mode}.log").read_bytes()
                assert b"Error: no supported AX200 (8086:2723)" in raw
                assert b"NVM: not validated" in raw and b"NVM commands sent/completed: 0/0" in raw
                assert b"Firmware ALIVE notification validated" not in raw
            raw = (out / "readback" / "wlan-runtime-status.log").read_bytes()
            assert b"WLAN transport: failed" in raw and b"RX worker: not started" in raw
            assert b"Owned DMA bytes: 0; runtime polls: 0" in raw
            raw = (out / "readback" / "wlan-start-invalid.log").read_bytes()
            assert b"not an Intel TLV .ucode image" in raw
            raw = (out / "readback" / "wlan-start-missing.log").read_bytes()
            assert b"firmware file not found" in raw
            raw = (out / "readback" / "wlan-scan-absent.log").read_bytes()
            assert b"run wlan start <firmware> before scanning" in raw
            assert b"WLAN scan: idle; generation: 0; networks: 0" in raw
            raw = (out / "readback" / "wlan-networks-absent.log").read_bytes()
            assert b"WLAN scan: idle; generation: 0; networks: 0" in raw
            assert b"Radio not configured; regulatory profile not current" in raw
            raw = (out / "readback" / "wlan-abort-absent.log").read_bytes()
            assert b"no scan in progress" in raw
            raw = (out / "readback" / "wlan-assoc-absent.log").read_bytes()
            assert b"run wlan start and wlan scan before association" in raw
            assert b"WLAN link: idle" in raw and b"Link TX sent/completed/failed: 0/0/0" in raw
            assert b"associated (data port closed)" not in raw
            results["wlan_association_absence_readback"] = True
            results["wlan_scan_absence_readback"] = True
            results["wlan_runtime_absent_readback"] = True
            print("PASS NVM/runtime absence, invalid/missing firmware and unbound NIC", flush=True)
        results["readback_sha256"] = hashlib.sha256(payload).hexdigest()
        results["byte_exact_readback"] = True
        print("PASS byte-exact disk readback; incomplete transfer not saved", flush=True)
    finally:
        stop.set()
        thread.join(timeout=5)
        if serial_reader is not None:
            serial_reader.join(timeout=5)
        if serial_channel is not None:
            serial_channel.close()
        server.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
        results["events"] = events
        results["server_errors"] = server_errors
        results["serial_errors"] = serial_errors
        (out / "result.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
