#!/usr/bin/env python3
"""Production SSH -> TCP -> shell -> OsitoFS3, isolated scratch disks only."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import threading
import time


def main():
    out = Path(sys.argv[1]).resolve(strict=True)
    work = Path(os.environ["OSITO_TEST_DISK_DIR"]).resolve(strict=True)
    if os.environ.get("OSITO_SSH_TEST_REMOTE_IO") == "1":
        source = work / "io-input.bin"
        source.write_bytes(bytes(range(256)) * (256 * 1024))
        subprocess.run(["tools/ositofs/ositofs-write3", str(work / "ssh-test.img"),
                        str(source), "--dest", "/io-input.bin"], check=True)
    serial = out / "serial.log"
    storage = os.environ.get("OSITO_SSH_TEST_STORAGE", "nvme")
    assert storage in ("nvme", "usb"), "unsupported test storage"
    storage_args = (["-device", "qemu-xhci,id=storage-xhci",
                     "-device", "usb-storage,bus=storage-xhci.0,drive=data"]
                    if storage == "usb" else ["-device", "nvme,serial=ssh-test,drive=data"])
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
    trace_sessions = os.environ.get("OSITO_SSH_TEST_SESSION_REUSE") == "1"
    trace_enabled = trace_sessions or os.environ.get("OSITO_SSH_TEST_TRACE") == "1"
    gdb_port = None
    restricted_test = os.environ.get("OSITO_SSH_TEST_RESTRICTED_TOKEN") == "1"
    policy_test = os.environ.get("OSITO_SSH_TEST_TOKEN_POLICY") == "1"
    integrity_test = os.environ.get("OSITO_SSH_TEST_TOKEN_INTEGRITY") == "1"
    privileges_test = os.environ.get("OSITO_SSH_TEST_TOKEN_PRIVILEGES") == "1"
    process_token_test = os.environ.get("OSITO_SSH_TEST_PROCESS_TOKEN") == "1"
    native_token_test = os.environ.get("OSITO_SSH_TEST_NATIVE_TOKEN") == "1"
    thread_info_test = os.environ.get("OSITO_SSH_TEST_THREAD_INFO") == "1"
    job_iocp_test = os.environ.get("OSITO_SSH_TEST_JOB_IOCP") == "1"
    job_atoms_test = os.environ.get("OSITO_SSH_TEST_JOB_UI_ATOMS") == "1"
    job_ui_access_test = os.environ.get("OSITO_SSH_TEST_JOB_UI_ACCESS") == "1"
    job_ui_handles_test = os.environ.get("OSITO_SSH_TEST_JOB_UI_HANDLES") == "1"
    job_ui_system_test = os.environ.get("OSITO_SSH_TEST_JOB_UI_SYSTEM") == "1"
    if trace_enabled or restricted_test or policy_test or integrity_test or privileges_test or process_token_test or native_token_test or thread_info_test or job_iocp_test or job_atoms_test or job_ui_access_test or job_ui_handles_test or job_ui_system_test or os.environ.get("OSITO_SSH_TEST_JOB_OBJECT") == "1":
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0)); gdb_port = s.getsockname()[1]
    args = ["qemu-system-x86_64", "-name", "osito-ssh-regression",
            "-machine", "q35", "-accel", "kvm", "-cpu", "host", "-m", "2G", "-smp", "4",
            "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
            "-drive", f"if=pflash,format=raw,file={work / 'vars.fd'}",
            "-drive", f"format=raw,if=ide,file={work / 'esp.img'}",
            "-drive", f"format=raw,if=none,id=data,file={work / 'ssh-test.img'}",
            *storage_args,
            "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{port}-:22",
            "-device", "virtio-net-pci,netdev=net0,disable-legacy=on",
            "-display", "none", "-vga", "std", "-monitor", "none",
            "-chardev", f"socket,id=uart,path={work / 'serial.sock'},server=on,wait=off,logfile={serial}",
            "-serial", "chardev:uart", "-qmp", f"unix:{work / 'qmp.sock'},server=on,wait=off",
            "-no-reboot", "-no-shutdown"]
    if gdb_port:
        args += ["-gdb", f"tcp:127.0.0.1:{gdb_port}"]
    if trace_enabled:
        args += ["-object", f"filter-dump,id=trace,netdev=net0,file={out / 'packets.pcap'}"]
    if os.environ.get("OSITO_SSH_TEST_REBOOT_ONLY"):
        args.remove("-no-reboot")
    (out / "command.json").write_text(json.dumps(args, indent=2))
    results = {"kernel_sha256": hashlib.sha256((out / "kernel.elf").read_bytes()).hexdigest(),
               "storage": storage}
    stop = threading.Event()
    uart = None
    reader = None
    with (out / "qemu.log").open("wb") as log:
        process = subprocess.Popen(args, stdout=log, stderr=log)

    def qmp(command):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(5); s.connect(str(work / "qmp.sock"))
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
            assert process.poll() is None, "QEMU terminated"
            data = serial.read_bytes()[start:] if serial.exists() else b""
            assert b"KERNEL PANIC" not in data and b"SYSTEM HALTED" not in data, data[-1000:]
            if b"osito> " in data: return data
            time.sleep(.1)
        (out / "timeout-qmp.json").write_text(json.dumps(qmp({"execute": "human-monitor-command", "arguments": {"command-line": "info registers -a"}}), indent=2))
        raise TimeoutError("no shell prompt")

    def local(command):
        start = serial.stat().st_size
        for c in (command + "\r").encode():
            uart.sendall(bytes([c])); time.sleep(.05)
        return prompt(start)

    def remote(command=None, key="client", data=None, extra=()):
        argv = ["ssh", "-F", "/dev/null", "-p", str(port), "-i", str(work / key),
                "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
                "-o", "StrictHostKeyChecking=accept-new", "-o", f"UserKnownHostsFile={out / 'known_hosts'}",
                *extra, "osito@127.0.0.1"]
        if command is not None: argv.append(command)
        began = time.monotonic()
        result = subprocess.run(argv, input=data, capture_output=True, timeout=90)
        with (out / "client.log").open("ab") as f:
            f.write((str(command) + f" [{time.monotonic() - began:.3f}s status={result.returncode}]\n").encode() + result.stdout + result.stderr + b"\n")
        if trace_enabled and result.returncode == 255 and key == "client" and process.poll() is None:
            with (out / "session-failure-gdb.log").open("ab") as f:
                subprocess.run(["gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                    "-ex", f"target remote 127.0.0.1:{gdb_port}",
                    "-ex", "thread apply all bt",
                    "-ex", "p *(int*)&connection",
                    "-ex", "p *(unsigned int*)&service_state",
                    "-ex", "p *(int*)&binary",
                    "-ex", "p *(int*)&done",
                    "-ex", "p *(unsigned int*)&output_len",
                    "-ex", "p *(unsigned long long*)&deadline",
                    "-ex", "p *(unsigned long long*)&tick_count",
                    "-ex", "p *(unsigned int*)&ready_count",
                    "-ex", "p *(unsigned int*)&sched_preempt_depth",
                    "-ex", "detach"], stdout=f, stderr=f, timeout=30)
        return result

    def passed(name):
        results[name] = True; print("PASS " + name, flush=True)

    def drain():
        while not stop.is_set():
            try:
                if not uart.recv(65536): return
            except socket.timeout:
                pass
            except OSError:
                return

    try:
        assert b"SSHTest" in prompt()
        uart = socket.socket(socket.AF_UNIX); uart.settimeout(.2)
        uart.connect(str(work / "serial.sock"))
        reader = threading.Thread(target=drain); reader.start()
        assert b"sshd: stopped" in local("sshd status")
        passed("opt_in_boot")
        raw = local("sshd start")
        assert b"listening on TCP 22" in raw, raw
        passed("persistent_keygen_and_listener")
        r = remote("echo REMOTE-ONLY-2491")
        assert r.returncode == 0 and r.stdout.replace(b"\r", b"") == b"REMOTE-ONLY-2491\n", (r.returncode, r.stdout, r.stderr)
        assert b"REMOTE-ONLY-2491" not in serial.read_bytes()
        passed("encrypted_exec_and_console_isolation")
        r = remote("echo MUST-NOT-RUN", key="wrong")
        assert r.returncode == 255 and b"MUST-NOT-RUN" not in r.stdout
        passed("reject_wrong_key")
        for cmd in ("mem", "ps", "ifconfig", "dmesg | grep SSHD"):
            r = remote(cmd)
            assert r.returncode == 0 and len(r.stdout) > 10, (cmd, r.stdout, r.stderr)
        passed("diagnostics_and_serial_capture")
        if os.environ.get("OSITO_SSH_TEST_SERIAL") == "1":
            spec = importlib.util.spec_from_file_location(
                "serial_check", Path("arch/x86/scripts/check-serial-test.py").resolve())
            serial_check = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(serial_check)
            raw = local("serial-test")
            (out / "serial-contract.bin").write_bytes(raw)
            serial_check.check_frames(raw)
            r = remote("echo SERIAL-SSH-ALIVE")
            assert r.returncode == 0 and b"SERIAL-SSH-ALIVE" in r.stdout, (r.stdout, r.stderr)
            passed("serial_uart_contract_and_ssh_recovery")
        r = remote("grep sshd- diag/boot0.log")
        assert r.returncode == 0, (r.stdout, r.stderr)
        stages = (b"[BDIAG] sshd-start", b"[BDIAG] sshd-keys-ready", b"[BDIAG] sshd-ready")
        positions = [r.stdout.find(stage) for stage in stages]
        assert all(n >= 0 for n in positions) and positions == sorted(positions), r.stdout[-2000:]
        passed("persistent_ssh_startup_checkpoints")
        if os.environ.get("OSITO_SSH_TEST_GDI_SURFACES") == "1":
            local("desktop")
            for bits in (32, 64):
                for suite in ("gdi-paint", "gdi-dib", "gdi-text", "user32-paint"):
                    data = local(f"winexec {suite}{bits}.exe")
                    (out / f"{suite}{bits}.log").write_bytes(data)
                    marker = ("[" + suite.upper() + "]").encode()
                    lines = [line for line in data.splitlines() if marker in line]
                    assert lines and any(b" PASS" in line for line in lines), data[-6000:]
                    assert b"FAIL" not in data and b"!!! EXCEPTION" not in data, data[-6000:]
                    passed(f"pe{bits}_{suite.replace('-', '_')}")
            data = local("win32-wm-test")
            (out / "window-model.log").write_bytes(data)
            assert b"USER32 window-model test: PASS" in data, data[-6000:]
            passed("window_model_after_gdi_regressions")
            r = remote("echo GDI-SSH-ALIVE")
            assert r.returncode == 0 and b"GDI-SSH-ALIVE" in r.stdout, (r.stdout, r.stderr)
            passed("gdi_desktop_ssh_alive")
            return
        if trace_sessions:
            spec = importlib.util.spec_from_file_location("sync_reuse", Path("tools/osito-sync.py").resolve())
            transfer = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(transfer)
            binary_argv = ["ssh", "-F", "/dev/null", "-p", str(port), "-i", str(work / "client"),
                "-oIdentitiesOnly=yes", "-oBatchMode=yes", "-oConnectTimeout=8",
                "-oStrictHostKeyChecking=yes", "-oRekeyLimit=64K",
                "-o", f"UserKnownHostsFile={out / 'known_hosts'}",
                "-T", "-s", "osito@127.0.0.1", "osito-sync-v1"]
            payload = bytes(range(256)) * 5000 + b"REUSE-END"
            source = out / "reuse-source.bin"
            source.write_bytes(payload)
            for index in range(8):
                client = transfer.Client(binary_argv)
                began = time.monotonic()
                try:
                    task = transfer.Sync(client, quiet=True)
                    if not index:
                        task.push_file(source, "/reuse.bin")
                    else:
                        destination = out / f"reuse-pull-{index}.bin"
                        task.pull("/reuse.bin", destination)
                        assert destination.read_bytes() == payload
                    client.close()
                except BaseException:
                    client.close(graceful=False)
                    raise
                print(f"transfer {index} closed after {time.monotonic() - began:.3f}s", flush=True)
                r = remote(f"echo REUSE-{index}")
                assert r.returncode == 0 and f"REUSE-{index}".encode() in r.stdout, (r.stdout, r.stderr)
                passed(f"binary_rekey_close_then_exec_{index}")
            return
        if os.environ.get("OSITO_SSH_TEST_IPHLP_DNS") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_IPHLP_DNS_NEGATIVE") == "1"
            servers = ("10.0.2.3",) if negative else ("10.0.2.3", "192.0.2.53", "0.0.0.0")
            for index, server in enumerate(servers):
                assert b"ipconf: applied" in local(f"ipconf 10.0.2.15 10.0.2.2 255.255.255.0 {server}")
                for bits in (32, 64):
                    raw = local(f"winexec dns{bits}.exe {server}")
                    (out / f"dns{bits}-{index}.log").write_bytes(raw)
                    assert b"!!! EXCEPTION" not in raw, raw[-5000:]
                    expected = b"IPHLP-DNS FAIL configured DNS list" if negative else b"IPHLP-DNS PASS"
                    assert expected in raw, raw[-5000:]
                    if not negative:
                        assert b"IPHLP-DNS FAIL" not in raw, raw[-5000:]
                    passed(f"pe{bits}_dns_{index}_{'negative' if negative else 'contract'}")
            assert b"ipconf: applied" in local("ipconf 10.0.2.15 10.0.2.2 255.255.255.0 10.0.2.3")
            assert b"DNS-SSH-ALIVE" in remote("echo DNS-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_REGISTRY_CONCURRENT") == "1":
            for bits in (32, 64):
                raw = local(f"winexec registry{bits}.exe")
                (out / f"registry{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-5000:]
                assert b"REGISTRY-CONCURRENT PASS" in raw, raw[-5000:]
                assert b"REGISTRY-CONCURRENT FAIL" not in raw, raw[-5000:]
                passed(f"pe{bits}_registry_concurrent_flush")
            assert b"REGISTRY-SSH-ALIVE" in remote("echo REGISTRY-SSH-ALIVE").stdout
            stop.set()
            uart.close()
            reader.join(timeout=2)
            qmp({"execute": "quit"})
            process.wait(timeout=10)
            serial.rename(out / "serial-first-boot.log")
            stop.clear()
            with (out / "qemu.log").open("ab") as log:
                process = subprocess.Popen(args, stdout=log, stderr=log)
            assert b"SSHTest" in prompt()
            uart = socket.socket(socket.AF_UNIX); uart.settimeout(.2)
            uart.connect(str(work / "serial.sock"))
            reader = threading.Thread(target=drain); reader.start()
            for bits in (32, 64):
                raw = local(f"winexec registry{bits}.exe verify")
                (out / f"registry{bits}-reloaded.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-5000:]
                assert b"REGISTRY-CONCURRENT PERSISTED" in raw, raw[-5000:]
                assert b"REGISTRY-CONCURRENT FAIL" not in raw, raw[-5000:]
                passed(f"pe{bits}_registry_persisted_after_reset")
            return
        if os.environ.get("OSITO_SSH_TEST_OPEN_PROCESS") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_OPEN_PROCESS_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec openprocess{bits}.exe")
                (out / f"openprocess{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"OPEN-PROCESS MISSING NtOpenProcess" in raw, raw[-15000:]
                else:
                    assert b"OPEN-PROCESS PASS" in raw and b"OPEN-PROCESS FAIL" not in raw, raw[-15000:]
                    assert raw.count(b"OPEN-PROCESS CHILD PASS") == 2, raw[-15000:]
                passed(f"pe{bits}_open_process_{'negative' if negative else 'contract'}")
            assert b"OPEN-PROCESS-SSH-ALIVE" in remote("echo OPEN-PROCESS-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_OPEN_THREAD") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_OPEN_THREAD_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec openthread{bits}.exe")
                (out / f"openthread{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"OPEN-THREAD MISSING NtOpenThread" in raw, raw[-15000:]
                else:
                    assert b"OPEN-THREAD PASS" in raw and b"OPEN-THREAD FAIL" not in raw, raw[-15000:]
                passed(f"pe{bits}_open_thread_{'negative' if negative else 'contract'}")
            assert b"OPEN-THREAD-SSH-ALIVE" in remote("echo OPEN-THREAD-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_REMOTE_VM") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_REMOTE_VM_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec remotevm{bits}.exe")
                (out / f"remotevm{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"REMOTE-VM FAIL: remote reserve" in raw, raw[-15000:]
                else:
                    assert b"REMOTE-VM PASS" in raw and b"REMOTE-VM FAIL" not in raw, raw[-15000:]
                    assert raw.count(b"REMOTE-VM CHILD PASS") == 4, raw[-15000:]
                passed(f"pe{bits}_remote_vm_{'negative' if negative else 'contract'}")
            assert b"REMOTE-VM-SSH-ALIVE" in remote("echo REMOTE-VM-SSH-ALIVE").stdout
            if not negative:
                raw = local("win32-vm-test")
                (out / "vm-regression.log").write_bytes(raw)
                assert b"NT virtual-memory test: PASS" in raw and b"FAIL" not in raw, raw[-12000:]
                passed("nt_virtual_memory_regression")
            return
        if os.environ.get("OSITO_SSH_TEST_SUSPENDED_IMAGE") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_SUSPENDED_IMAGE_NEGATIVE") == "1"
            ntdll_negative = os.environ.get("OSITO_SSH_TEST_SUSPENDED_NTDLL_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec suspended{bits}.exe")
                (out / f"suspended{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"SUSPENDED-IMAGE FAIL: mapped image exists before resume" in raw, raw[-15000:]
                elif ntdll_negative:
                    assert b"SUSPENDED-IMAGE FAIL: NTDLL mapped at common ABI base before resume" in raw, raw[-15000:]
                else:
                    assert b"SUSPENDED-IMAGE PASS" in raw and b"SUSPENDED-IMAGE FAIL" not in raw, raw[-15000:]
                    assert raw.count(b"SUSPENDED-IMAGE CHILD PASS") == 4, raw[-15000:]
                passed(f"pe{bits}_suspended_image_{'negative' if negative or ntdll_negative else 'contract'}")
            assert b"SUSPENDED-SSH-ALIVE" in remote("echo SUSPENDED-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_NT64_SERVICE") == "1":
            def check_elf(stage):
                elf = local("exec nt64-elf")
                (out / f"nt64-elf-{stage}.log").write_bytes(elf)
                assert b"NT64-ELF PASS" in elf and b"NT64-ELF FAIL" not in elf, elf[-15000:]
                assert b"!!! EXCEPTION" not in elf, elf[-15000:]
                passed(f"nt64_elf_namespace_{stage}")

            check_elf("before")
            raw = local("winexec nt64-service.exe")
            (out / "nt64-service.log").write_bytes(raw)
            assert b"!!! EXCEPTION" not in raw, raw[-15000:]
            if os.environ.get("OSITO_SSH_TEST_NT64_SERVICE_NEGATIVE") == "1":
                assert b"NT64-SERVICE FAIL: position-independent NT service instruction contract" in raw, raw[-15000:]
                passed("nt64_service_negative")
            else:
                assert b"NT64-SERVICE PASS" in raw and b"NT64-SERVICE FAIL" not in raw, raw[-15000:]
                assert b"NT64-SERVICE CHILD PASS" in raw, raw[-15000:]
                passed("nt64_service_copy_interception_and_abi")
            check_elf("after")
            assert b"NT64-SSH-ALIVE" in remote("echo NT64-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_SHIM_EXPORTS") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_SHIM_EXPORTS_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec shimexports{bits}.exe")
                (out / f"shimexports{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-10000:]
                if negative:
                    assert b"SHIM-EXPORTS FAIL: PE export directory" in raw, raw[-10000:]
                else:
                    assert b"SHIM-EXPORTS PASS" in raw and b"SHIM-EXPORTS FAIL" not in raw, raw[-15000:]
                    assert raw.count(b"SHIM-EXPORTS CHILD PASS") == 4, raw[-15000:]
                passed(f"pe{bits}_shim_exports_{'negative' if negative else 'contract_and_isolation'}")
            if not negative:
                raw = local("win32-module-test")
                (out / "module-image-regression.log").write_bytes(raw)
                assert b"Win32 module-image test: PASS" in raw and b"FAIL" not in raw, raw[-10000:]
                passed("module_image_and_abi_regression")
            assert b"EXPORTS-SSH-ALIVE" in remote("echo EXPORTS-SSH-ALIVE").stdout
            return
        if job_iocp_test:
            negative = os.environ.get("OSITO_SSH_TEST_JOB_IOCP_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec jobiocp{bits}.exe")
                (out / f"jobiocp{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"JOB-IOCP FAIL: associate empty job" in raw, raw[-15000:]
                else:
                    assert b"JOB-IOCP PASS" in raw and b"JOB-IOCP FAIL" not in raw, raw[-15000:]
                    result = subprocess.run([
                        "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                        "-ex", f"target remote 127.0.0.1:{gdb_port}",
                        "-ex", 'printf "JOB-LIVE=%u\\n", *(unsigned int *)&g_job_live_objects',
                        "-ex", 'printf "JOB-PROCESSES=%llu\\n", *(unsigned long long *)&job_processes',
                        "-ex", 'printf "IOCP-REFERENCES=%u\\n", *(unsigned int *)&g_iocp_job_references',
                        "-ex", 'printf "IOCP-LIVE=%u\\n", *(unsigned int *)&g_iocp_live_ports',
                        "-ex", "detach"], capture_output=True, timeout=30)
                    (out / f"jobiocp{bits}-lifetime.log").write_bytes(result.stdout + result.stderr)
                    assert result.returncode == 0, result
                    for marker in (b"JOB-LIVE", b"JOB-PROCESSES", b"IOCP-REFERENCES", b"IOCP-LIVE"):
                        assert marker + b"=0\n" in result.stdout, result
                passed(f"pe{bits}_job_iocp_{'negative' if negative else 'contract_and_lifetime'}")
            assert b"JOB-IOCP-SSH-ALIVE" in remote("echo JOB-IOCP-SSH-ALIVE").stdout
            return
        if thread_info_test:
            negative = os.environ.get("OSITO_SSH_TEST_THREAD_INFO_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec threadinfo{bits}.exe")
                (out / f"threadinfo{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"THREAD-INFO MISSING NtSetInformationThread" in raw, raw[-15000:]
                else:
                    assert b"THREAD-INFO PASS" in raw and b"THREAD-INFO FAIL" not in raw, raw[-15000:]
                    result = subprocess.run([
                        "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                        "-ex", f"target remote 127.0.0.1:{gdb_port}",
                        "-ex", 'printf "TOKEN-LIVE=%u\\n", *(unsigned int *)&g_token_live_objects',
                        "-ex", 'printf "POLICY-LIVE=%u\\n", *(unsigned int *)&g_token_live_policies',
                        "-ex", "detach"], capture_output=True, timeout=30)
                    (out / f"threadinfo{bits}-lifetime.log").write_bytes(result.stdout + result.stderr)
                    assert result.returncode == 0 and b"TOKEN-LIVE=0\n" in result.stdout, result
                    assert b"POLICY-LIVE=0\n" in result.stdout, result
                passed(f"pe{bits}_thread_info_{'negative' if negative else 'contract_and_lifetime'}")
            assert b"THREAD-INFO-SSH-ALIVE" in remote("echo THREAD-INFO-SSH-ALIVE").stdout
            return
        if native_token_test:
            negative = os.environ.get("OSITO_SSH_TEST_NATIVE_TOKEN_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec nativetoken{bits}.exe")
                (out / f"nativetoken{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"NATIVE-TOKEN MISSING NtOpenProcessToken" in raw, raw[-15000:]
                else:
                    assert b"NATIVE-TOKEN PASS" in raw and b"NATIVE-TOKEN FAIL" not in raw, raw[-15000:]
                    result = subprocess.run([
                        "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                        "-ex", f"target remote 127.0.0.1:{gdb_port}",
                        "-ex", 'printf "TOKEN-LIVE=%u\\n", *(unsigned int *)&g_token_live_objects',
                        "-ex", 'printf "POLICY-LIVE=%u\\n", *(unsigned int *)&g_token_live_policies',
                        "-ex", "detach"], capture_output=True, timeout=30)
                    (out / f"nativetoken{bits}-lifetime.log").write_bytes(result.stdout + result.stderr)
                    assert result.returncode == 0 and b"TOKEN-LIVE=0\n" in result.stdout, result
                    assert b"POLICY-LIVE=0\n" in result.stdout, result
                passed(f"pe{bits}_native_token_{'negative' if negative else 'contract_and_lifetime'}")
            assert b"NATIVE-TOKEN-SSH-ALIVE" in remote("echo NATIVE-TOKEN-SSH-ALIVE").stdout
            return
        if process_token_test:
            negative = os.environ.get("OSITO_SSH_TEST_PROCESS_TOKEN_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec processtoken{bits}.exe")
                (out / f"processtoken{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"PROCESS-TOKEN FAIL: create process with selected token" in raw, raw[-15000:]
                else:
                    assert b"PROCESS-TOKEN PASS" in raw and b"PROCESS-TOKEN FAIL" not in raw, raw[-15000:]
                    result = subprocess.run([
                        "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                        "-ex", f"target remote 127.0.0.1:{gdb_port}",
                        "-ex", 'printf "TOKEN-LIVE=%u\\n", *(unsigned int *)&g_token_live_objects',
                        "-ex", 'printf "POLICY-LIVE=%u\\n", *(unsigned int *)&g_token_live_policies',
                        "-ex", "detach"], capture_output=True, timeout=30)
                    (out / f"processtoken{bits}-lifetime.log").write_bytes(result.stdout + result.stderr)
                    assert result.returncode == 0 and b"TOKEN-LIVE=0\n" in result.stdout, result
                    assert b"POLICY-LIVE=0\n" in result.stdout, result
                passed(f"pe{bits}_selected_process_token_{'negative' if negative else 'contract_and_lifetime'}")
            assert b"PROCESS-TOKEN-SSH-ALIVE" in remote("echo PROCESS-TOKEN-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_WINSOCK_STREAM") == "1":
            server_log = out / "stream-server.log"
            corrupt = int(os.environ.get("OSITO_SSH_TEST_STREAM_CORRUPT", "-1"))
            assert -1 <= corrupt < 2097152
            server_args = [sys.executable, "arch/x86/scripts/winsock-stream-server.py"]
            if corrupt >= 0:
                server_args += ["--corrupt-at", str(corrupt)]
            with server_log.open("wb") as stream_log:
                stream_server = subprocess.Popen(
                    server_args,
                    stdout=stream_log, stderr=stream_log)
                try:
                    deadline = time.monotonic() + 10
                    while not server_log.stat().st_size and time.monotonic() < deadline:
                        assert stream_server.poll() is None, "stream server exited"
                        time.sleep(.05)
                    stream_port = json.loads(server_log.read_text().splitlines()[0])["listening"][1]
                    for bits in (32, 64):
                        for mode in (0, 1):
                            raw = local(f"winexec stream{bits}.exe 10.0.2.2 {stream_port} {mode} 2")
                            (out / f"stream{bits}-mode{mode}.log").write_bytes(raw)
                            if corrupt >= 0:
                                assert f"STREAM MISMATCH offset={corrupt} ".encode() in raw, raw[-5000:]
                                assert b"STREAM PRIVATE PASS" not in raw, raw[-5000:]
                            else:
                                assert b"STREAM PRIVATE PASS" in raw and b"STREAM FAIL" not in raw, raw[-5000:]
                            assert not any(s in raw for s in (b"!!! EXCEPTION", b"SCHED CORRUPT")), raw[-5000:]
                            passed(f"pe{bits}_stream_mode{mode}_private_{'corruption_detected' if corrupt >= 0 else 'integrity'}")
                            r = remote("echo STREAM-SSH-ALIVE")
                            assert r.returncode == 0 and b"STREAM-SSH-ALIVE" in r.stdout
                    assert stream_server.wait(timeout=10) == (1 if corrupt >= 0 else 0), server_log.read_text()
                    entries = [json.loads(line) for line in server_log.read_text().splitlines()[1:]]
                    assert len(entries) == 4
                    if corrupt >= 0:
                        assert all(not e["ok"] for e in entries)
                        passed("corruption_aborts_all_four_transfers")
                    else:
                        assert all(e["ok"] and e["bytes_each_direction"] == 2097152 for e in entries)
                        passed("host_independently_verified_stream_tx_all_four_cases")
                finally:
                    if stream_server.poll() is None:
                        stream_server.terminate()
                    stream_server.wait(timeout=10)
            return
        if os.environ.get("OSITO_SSH_TEST_DISCONNECT") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_DISCONNECT_NEGATIVE") == "1"
            churn = os.environ.get("OSITO_SSH_TEST_DISCONNECT_CHURN") == "1"
            abort = os.environ.get("OSITO_SSH_TEST_DISCONNECT_ABORT") == "1"
            private = os.environ.get("OSITO_SSH_TEST_DISCONNECT_PRIVATE") == "1"
            lifetime = os.environ.get("OSITO_SSH_TEST_DISCONNECT_LIFETIME") == "1"
            orphans = os.environ.get("OSITO_SSH_TEST_DISCONNECT_ORPHANS") == "1"
            tokens = os.environ.get("OSITO_SSH_TEST_DISCONNECT_TOKENS") == "1"
            assert sum((churn, abort, lifetime, orphans, tokens)) <= 1
            assert not (private and (lifetime or orphans or tokens))
            selected = os.environ.get("OSITO_SSH_TEST_DISCONNECT_BITS", "32,64").split(",")
            assert selected and all(bits in ("32", "64") for bits in selected)
            for bits in selected:
                suffix = (" --churn" if churn else " --abort" if abort else
                          " --lifetime" if lifetime else " --orphans" if orphans else
                          " --tokens" if tokens else "")
                if private:
                    suffix += " --private"
                raw = local(f"winexec disconnect{bits}.exe{suffix}")
                (out / f"disconnect{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    failure = (b"connect check=" if churn or tokens else
                               b"exit-pending connect check=" if lifetime else
                               b"exit-open connect check=" if orphans else
                               b"disconnect posts completion")
                    assert b"DISCONNECT FAIL: " + failure in raw, raw[-15000:]
                    assert b"DISCONNECT PASS" not in raw, raw[-15000:]
                else:
                    marker = b"DISCONNECT PRIVATE PASS" if private else b"DISCONNECT PASS"
                    assert marker in raw and b"DISCONNECT FAIL" not in raw, raw[-15000:]
                mode = ("churn" if churn else "abort" if abort else
                        "lifetime" if lifetime else "orphans" if orphans else
                        "tokens" if tokens else "completion")
                if private:
                    mode += "_private"
                passed(f"pe{bits}_disconnect_{mode}_{'negative' if negative else 'contract'}")
            if not (negative and (churn or lifetime or orphans or tokens)):
                assert b"DISCONNECT-SSH-ALIVE" in remote("echo DISCONNECT-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_PROCESS_HANDLES") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_PROCESS_HANDLES_NEGATIVE") == "1"
            for bits in (32, 64):
                suffix = "" if negative else " --scratch"
                raw = local(f"winexec prochandles{bits}.exe{suffix}")
                (out / f"prochandles{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"JOB-OBJECT FAIL: ProcessHandleTable succeeds" in raw, raw[-15000:]
                    assert b"PROCESS-HANDLES PASS" not in raw, raw[-15000:]
                else:
                    assert b"PROCESS-HANDLES PASS" in raw and b"JOB-OBJECT FAIL" not in raw, raw[-15000:]
                passed(f"pe{bits}_process_handles_{'negative' if negative else 'contract'}")
            assert b"PROCESS-HANDLES-SSH-ALIVE" in remote("echo PROCESS-HANDLES-SSH-ALIVE").stdout
            return
        if job_ui_system_test:
            negative = os.environ.get("OSITO_SSH_TEST_JOB_UI_SYSTEM_NEGATIVE") == "1"
            for bits in (32, 64):
                for profile in (("native",) if negative else ("native", "scratch")):
                    suffix = " --scratch" if profile == "scratch" else ""
                    raw = local(f"winexec jobuisystem{bits}.exe{suffix}")
                    (out / f"jobuisystem{bits}-{profile}.log").write_bytes(raw)
                    assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                    if negative:
                        assert b"JOB-UI-SYSTEM MISSING SwitchDesktop" in raw, raw[-15000:]
                        assert b"JOB-UI-SYSTEM PASS" not in raw, raw[-15000:]
                    else:
                        assert b"JOB-UI-SYSTEM PASS" in raw and b"JOB-OBJECT FAIL" not in raw, raw[-15000:]
                        result = subprocess.run([
                            "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                            "-ex", f"target remote 127.0.0.1:{gdb_port}",
                            "-ex", 'printf "JOB-LIVE=%u\\n", *(unsigned int *)&g_job_live_objects',
                            "-ex", 'printf "JOB-PROCESSES=%llu\\n", *(unsigned long long *)&job_processes',
                            "-ex", 'printf "JOB-GRANTS=%u\\n", *(unsigned int *)&g_job_user_grants',
                            "-ex", 'printf "USER-WINDOWS=%d\\n", *(int *)&window_count',
                            "-ex", "detach"], capture_output=True, timeout=30)
                        (out / f"jobuisystem{bits}-{profile}-lifetime.log").write_bytes(result.stdout + result.stderr)
                        assert result.returncode == 0, result
                        for marker in (b"JOB-LIVE=0\n", b"JOB-PROCESSES=0\n", b"JOB-GRANTS=0\n", b"USER-WINDOWS=0\n"):
                            assert marker in result.stdout, result
                    passed(f"pe{bits}_job_ui_system_{profile}_{'negative' if negative else 'contract_and_lifetime'}")
            assert b"JOB-UI-SYSTEM-SSH-ALIVE" in remote("echo JOB-UI-SYSTEM-SSH-ALIVE").stdout
            return
        if job_ui_handles_test:
            negative = os.environ.get("OSITO_SSH_TEST_JOB_UI_HANDLES_NEGATIVE") == "1"
            if not negative:
                symbols = subprocess.check_output(["nm", "-S", "--defined-only", str(out / "kernel.elf")], text=True)
                imm_symbol = next(line.split() for line in symbols.splitlines() if line.endswith(" imm_contexts"))
                imm_address, imm_bytes = (int(value, 16) for value in imm_symbol[:2])
                imm_stride = imm_bytes // 32
                assert imm_bytes % 32 == 0 and imm_stride == 32, imm_symbol
            for bits in (32, 64):
                for profile in (("native",) if negative else ("native", "scratch")):
                    suffix = " --scratch" if profile == "scratch" else ""
                    raw = local(f"winexec jobuihandles{bits}.exe{suffix}")
                    (out / f"jobuihandles{bits}-{profile}.log").write_bytes(raw)
                    assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                    if negative:
                        assert (b"JOB-UI-HANDLES MISSING UserHandleGrantAccess" in raw or
                                b"JOB-UI-HANDLES FAIL: configure USER handle restriction" in raw), raw[-15000:]
                        assert b"JOB-UI-HANDLES PASS" not in raw, raw[-15000:]
                    else:
                        assert b"JOB-UI-HANDLES PASS" in raw and b"JOB-UI-HANDLES FAIL" not in raw, raw[-15000:]
                        imm_dump = out / f"jobuihandles{bits}-{profile}-imm.bin"
                        result = subprocess.run([
                            "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                            "-ex", f"target remote 127.0.0.1:{gdb_port}",
                            "-ex", 'printf "JOB-LIVE=%u\\n", *(unsigned int *)&g_job_live_objects',
                            "-ex", 'printf "JOB-PROCESSES=%llu\\n", *(unsigned long long *)&job_processes',
                            "-ex", 'printf "JOB-GRANTS=%u\\n", *(unsigned int *)&g_job_user_grants',
                            "-ex", 'printf "USER-WINDOWS=%d\\n", *(int *)&window_count',
                            "-ex", f"dump binary memory {imm_dump} 0x{imm_address:x} 0x{imm_address + imm_bytes:x}",
                            "-ex", "detach"], capture_output=True, timeout=30)
                        (out / f"jobuihandles{bits}-{profile}-lifetime.log").write_bytes(result.stdout + result.stderr)
                        assert result.returncode == 0, result
                        for marker in (b"JOB-LIVE=0\n", b"JOB-PROCESSES=0\n", b"JOB-GRANTS=0\n", b"USER-WINDOWS=0\n"):
                            assert marker in result.stdout, result
                        contexts = imm_dump.read_bytes()
                        assert len(contexts) == imm_bytes
                        assert all(contexts[offset + 28:offset + 32] == bytes(4)
                                   for offset in range(0, imm_bytes, imm_stride)), "input context leak"
                    passed(f"pe{bits}_job_ui_handles_{profile}_{'negative' if negative else 'contract_and_lifetime'}")
            assert b"JOB-UI-HANDLES-SSH-ALIVE" in remote("echo JOB-UI-HANDLES-SSH-ALIVE").stdout
            return
        if job_ui_access_test:
            negative = os.environ.get("OSITO_SSH_TEST_JOB_UI_ACCESS_NEGATIVE") == "1"
            for bits in (32, 64):
                for profile in (("native",) if negative else ("native", "scratch")):
                    suffix = " --scratch" if profile == "scratch" else ""
                    raw = local(f"winexec jobuiaccess{bits}.exe{suffix}")
                    (out / f"jobuiaccess{bits}-{profile}.log").write_bytes(raw)
                    assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                    if negative:
                        assert b"JOB-UI-ACCESS FAIL: configure clipboard/display UI limits" in raw, raw[-15000:]
                        assert b"JOB-UI-ACCESS PASS" not in raw, raw[-15000:]
                    else:
                        assert b"JOB-UI-ACCESS PASS" in raw and b"JOB-UI-ACCESS FAIL" not in raw, raw[-15000:]
                    result = subprocess.run([
                        "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                        "-ex", f"target remote 127.0.0.1:{gdb_port}",
                        "-ex", 'printf "JOB-LIVE=%u\\n", *(unsigned int *)&g_job_live_objects',
                        "-ex", 'printf "JOB-PROCESSES=%llu\\n", *(unsigned long long *)&job_processes',
                        "-ex", 'printf "USER-WINDOWS=%d\\n", *(int *)&window_count',
                        "-ex", "detach"], capture_output=True, timeout=30)
                    (out / f"jobuiaccess{bits}-{profile}-lifetime.log").write_bytes(result.stdout + result.stderr)
                    assert result.returncode == 0 and b"JOB-LIVE=0\n" in result.stdout, result
                    assert b"JOB-PROCESSES=0\n" in result.stdout, result
                    assert b"USER-WINDOWS=0\n" in result.stdout, result
                    passed(f"pe{bits}_job_ui_access_{profile}_{'negative' if negative else 'contract_and_lifetime'}")
            assert b"JOB-UI-ACCESS-SSH-ALIVE" in remote("echo JOB-UI-ACCESS-SSH-ALIVE").stdout
            return
        if job_atoms_test:
            negative = os.environ.get("OSITO_SSH_TEST_JOB_UI_ATOMS_NEGATIVE") == "1"
            symbols = subprocess.check_output(["nm", "-S", "--defined-only", str(out / "kernel.elf")], text=True)
            atom_symbol = next(line.split() for line in symbols.splitlines() if line.endswith(" k32_atom_table"))
            atom_address, atom_bytes = (int(n, 16) for n in atom_symbol[:2])
            atom_stride = atom_bytes // 512
            assert atom_bytes % 512 == 0 and atom_stride >= 520
            for bits in (32, 64):
                suffix = " u:00000001" if os.environ.get("OSITO_SSH_TEST_JOB_UI_ATOMS_PARTIAL") == "1" else ""
                raw = local(f"winexec jobuiatoms{bits}.exe{suffix}")
                (out / f"jobuiatoms{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"JOB-UI-ATOM FAIL: configure global atom UI limit" in raw, raw[-15000:]
                    assert b"JOB-UI-ATOM PASS" not in raw, raw[-15000:]
                else:
                    assert b"JOB-UI-ATOM PASS" in raw and b"JOB-UI-ATOM FAIL" not in raw, raw[-15000:]
                atom_dump = out / f"jobuiatoms{bits}-table.bin"
                result = subprocess.run([
                    "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                    "-ex", f"target remote 127.0.0.1:{gdb_port}",
                    "-ex", 'printf "JOB-LIVE=%u\\n", *(unsigned int *)&g_job_live_objects',
                    "-ex", 'printf "JOB-PROCESSES=%llu\\n", *(unsigned long long *)&job_processes',
                    "-ex", f"dump binary memory {atom_dump} {atom_address:#x} {atom_address + atom_bytes:#x}",
                    "-ex", "detach"], capture_output=True, timeout=30)
                (out / f"jobuiatoms{bits}-lifetime.log").write_bytes(result.stdout + result.stderr)
                assert result.returncode == 0 and b"JOB-LIVE=0\n" in result.stdout, result
                assert b"JOB-PROCESSES=0\n" in result.stdout, result
                atoms = atom_dump.read_bytes()
                assert len(atoms) == atom_bytes
                assert all(atoms[i:i+4] == b"\0" * 4 for i in range(0, atom_bytes, atom_stride)), "atom entries leaked after job/process destruction"
                passed(f"pe{bits}_job_ui_atoms_{'negative' if negative else 'contract_and_lifetime'}")
            assert b"JOB-UI-ATOMS-SSH-ALIVE" in remote("echo JOB-UI-ATOMS-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_JOB_OBJECT") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_JOB_DIE_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec jobobject{bits}.exe")
                (out / f"jobobject{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"JOB-OBJECT FAIL: configure die-on-unhandled limit" in raw, raw[-15000:]
                else:
                    assert b"JOB-OBJECT PASS" in raw and b"JOB-OBJECT FAIL" not in raw, raw[-15000:]
                result = subprocess.run([
                    "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                    "-ex", f"target remote 127.0.0.1:{gdb_port}",
                    "-ex", 'printf "JOB-LIVE=%u\\n", *(unsigned int *)&g_job_live_objects',
                    "-ex", 'printf "JOB-PROCESSES=%llu\\n", *(unsigned long long *)&job_processes',
                    "-ex", "detach"], capture_output=True, timeout=30)
                (out / f"jobobject{bits}-lifetime.log").write_bytes(result.stdout + result.stderr)
                assert result.returncode == 0 and b"JOB-LIVE=0\n" in result.stdout, result
                assert b"JOB-PROCESSES=0\n" in result.stdout, result
                passed(f"pe{bits}_job_{'die_negative' if negative else 'contract_and_lifetime'}")
            assert b"JOB-SSH-ALIVE" in remote("echo JOB-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_REGISTRY_CACHE") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_REGISTRY_CACHE_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec regcache{bits}.exe")
                (out / f"regcache{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    if bits == 32:
                        assert b"[ABI-MISS] PE32 import advapi32.dll!RegDisablePredefinedCache has no thunk contract" in raw, raw[-15000:]
                        assert b"[WINEXEC] compat32 IAT patch failed" in raw, raw[-15000:]
                    else:
                        assert b"REGISTRY-CACHE FAIL: missing cache-disable export" in raw, raw[-15000:]
                else:
                    assert b"REGISTRY-CACHE PASS" in raw and b"REGISTRY-CACHE FAIL" not in raw, raw[-15000:]
                passed(f"pe{bits}_registry_cache_{'negative' if negative else 'contract'}")
            assert b"REGISTRY-CACHE-SSH-ALIVE" in remote("echo REGISTRY-CACHE-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_MANDATORY_ACE") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_MANDATORY_ACE_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec mandatoryace{bits}.exe")
                (out / f"mandatoryace{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    if bits == 32:
                        assert b"PE32 import advapi32.dll!AddMandatoryAce has no thunk contract" in raw, raw[-15000:]
                        assert b"compat32 IAT patch failed" in raw, raw[-15000:]
                        assert b"MANDATORY-ACE PASS" not in raw, raw[-15000:]
                    else:
                        assert b"MANDATORY-ACE FAIL: missing export" in raw, raw[-15000:]
                else:
                    assert b"MANDATORY-ACE PASS" in raw and b"MANDATORY-ACE FAIL" not in raw, raw[-15000:]
                passed(f"pe{bits}_mandatory_ace_{'negative' if negative else 'contract'}")
            assert b"MANDATORY-ACE-SSH-ALIVE" in remote("echo MANDATORY-ACE-SSH-ALIVE").stdout
            return
        if privileges_test:
            negative = os.environ.get("OSITO_SSH_TEST_TOKEN_PRIVILEGES_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec tokenprivileges{bits}.exe")
                (out / f"tokenprivileges{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"TOKEN-PRIVILEGES FAIL: empty query requires TOKEN_PRIVILEGES header" in raw, raw[-15000:]
                else:
                    assert b"TOKEN-PRIVILEGES PASS" in raw and b"TOKEN-PRIVILEGES FAIL" not in raw, raw[-15000:]
                result = subprocess.run([
                    "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                    "-ex", f"target remote 127.0.0.1:{gdb_port}",
                    "-ex", 'printf "TOKEN-LIVE=%u\\n", *(unsigned int *)&g_token_live_objects',
                    "-ex", 'printf "POLICY-LIVE=%u\\n", *(unsigned int *)&g_token_live_policies',
                    "-ex", "detach"], capture_output=True, timeout=30)
                (out / f"tokenprivileges{bits}-lifetime.log").write_bytes(result.stdout + result.stderr)
                assert result.returncode == 0 and b"TOKEN-LIVE=0\n" in result.stdout, result
                assert b"POLICY-LIVE=0\n" in result.stdout, result
                passed(f"pe{bits}_token_privileges_{'negative' if negative else 'contract_and_lifetime'}")
            assert b"TOKEN-PRIVILEGES-SSH-ALIVE" in remote("echo TOKEN-PRIVILEGES-SSH-ALIVE").stdout
            return
        if integrity_test:
            negative = os.environ.get("OSITO_SSH_TEST_TOKEN_INTEGRITY_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec tokenintegrity{bits}.exe")
                (out / f"tokenintegrity{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-15000:]
                if negative:
                    assert b"TOKEN-INTEGRITY FAIL: missing label state" in raw, raw[-15000:]
                else:
                    assert b"TOKEN-INTEGRITY PASS" in raw and b"TOKEN-INTEGRITY FAIL" not in raw, raw[-15000:]
                    result = subprocess.run([
                        "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                        "-ex", f"target remote 127.0.0.1:{gdb_port}",
                        "-ex", 'printf "TOKEN-LIVE=%u\\n", *(unsigned int *)&g_token_live_objects',
                        "-ex", 'printf "POLICY-LIVE=%u\\n", *(unsigned int *)&g_token_live_policies',
                        "-ex", "detach"], capture_output=True, timeout=30)
                    (out / f"tokenintegrity{bits}-lifetime.log").write_bytes(result.stdout + result.stderr)
                    assert result.returncode == 0 and b"TOKEN-LIVE=0\n" in result.stdout, result
                    assert b"POLICY-LIVE=0\n" in result.stdout, result
                passed(f"pe{bits}_token_integrity_{'negative' if negative else 'contract_and_lifetime'}")
            assert b"TOKEN-INTEGRITY-SSH-ALIVE" in remote("echo TOKEN-INTEGRITY-SSH-ALIVE").stdout
            return
        if policy_test:
            negative = os.environ.get("OSITO_SSH_TEST_TOKEN_POLICY_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec tokenpolicy{bits}.exe")
                (out / f"tokenpolicy{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-10000:]
                if negative:
                    assert b"TOKEN-POLICY FAILED" in raw, raw[-10000:]
                    assert b"TOKEN-POLICY FAIL: round-trip DACL" in raw, raw[-10000:]
                    assert b"TOKEN-POLICY FAIL: restricted token DACL can change" in raw, raw[-10000:]
                else:
                    assert b"TOKEN-POLICY PASS" in raw and b"TOKEN-POLICY FAIL" not in raw, raw[-10000:]
                    result = subprocess.run([
                        "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                        "-ex", f"target remote 127.0.0.1:{gdb_port}",
                        "-ex", 'printf "TOKEN-LIVE=%u\\n", *(unsigned int *)&g_token_live_objects',
                        "-ex", 'printf "POLICY-LIVE=%u\\n", *(unsigned int *)&g_token_live_policies',
                        "-ex", "detach"], capture_output=True, timeout=30)
                    (out / f"tokenpolicy{bits}-lifetime.log").write_bytes(result.stdout + result.stderr)
                    assert result.returncode == 0 and b"TOKEN-LIVE=0\n" in result.stdout, result
                    assert b"POLICY-LIVE=0\n" in result.stdout, result
                passed(f"pe{bits}_token_policy_{'negative' if negative else 'contract_and_lifetime'}")
            assert b"TOKEN-POLICY-SSH-ALIVE" in remote("echo TOKEN-POLICY-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_RESTRICTED_TOKEN") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_RESTRICTED_TOKEN_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec restricted{bits}.exe")
                (out / f"restricted{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-10000:]
                expected = b"RESTRICTED-TOKEN MISSING EXPORT" if negative else b"RESTRICTED-TOKEN PASS"
                assert expected in raw, raw[-10000:]
                assert b"RESTRICTED-TOKEN FAIL" not in raw, raw[-10000:]
                passed(f"pe{bits}_restricted_token_{'negative' if negative else 'contract'}")
                if not negative:
                    result = subprocess.run([
                        "gdb", "-q", "-nx", "-batch", str(out / "kernel.elf"),
                        "-ex", f"target remote 127.0.0.1:{gdb_port}",
                        "-ex", 'printf "TOKEN-LIVE=%u\\n", *(unsigned int *)&g_token_live_objects',
                        "-ex", "detach"], capture_output=True, timeout=30)
                    (out / f"restricted{bits}-lifetime.log").write_bytes(result.stdout + result.stderr)
                    assert result.returncode == 0 and b"TOKEN-LIVE=0\n" in result.stdout, result
                    passed(f"pe{bits}_restricted_token_no_live_allocations")
            assert b"RESTRICTED-TOKEN-SSH-ALIVE" in remote("echo RESTRICTED-TOKEN-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_ACCESS_CHECK") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_ACCESS_CHECK_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec accesscheck{bits}.exe")
                (out / f"accesscheck{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-8000:]
                expected = b"ACCESS-CHECK MISSING EXPORT" if negative else b"ACCESS-CHECK PASS"
                assert expected in raw, raw[-8000:]
                assert b"ACCESS-CHECK FAIL" not in raw, raw[-8000:]
                passed(f"pe{bits}_access_check_{'negative' if negative else 'contract'}")
            assert b"ACCESS-CHECK-SSH-ALIVE" in remote("echo ACCESS-CHECK-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_USER_OBJECT") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_USER_OBJECT_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec userobject{bits}.exe")
                (out / f"userobject{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-5000:]
                expected = b"USER-OBJECT FAILED" if negative else b"USER-OBJECT PASS"
                assert expected in raw, raw[-5000:]
                if negative:
                    assert b"FAIL: name is nonempty UTF-16" in raw, raw[-5000:]
                else:
                    assert b"USER-OBJECT FAIL" not in raw, raw[-5000:]
                passed(f"pe{bits}_user_object_{'negative' if negative else 'contract'}")
            assert b"USER-OBJECT-SSH-ALIVE" in remote("echo USER-OBJECT-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_USER32_CASE") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_USER32_CASE_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec case{bits}.exe")
                (out / f"case{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-5000:]
                expected = b"USER32-CASE MISSING EXPORT" if negative else b"USER32-CASE PASS"
                assert expected in raw, raw[-5000:]
                passed(f"pe{bits}_user32_case_{'negative' if negative else 'contract'}")
            assert b"CASE-SSH-ALIVE" in remote("echo CASE-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_VNET_PRIVATE") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_VNET_PRIVATE_NEGATIVE") == "1"
            echo_stop = threading.Event()
            echoed = []

            def echo_worker(listener):
                while not echo_stop.is_set():
                    try:
                        conn, _ = listener.accept()
                    except socket.timeout:
                        continue
                    total = 0
                    with conn:
                        conn.settimeout(.2)
                        while not echo_stop.is_set():
                            try:
                                data = conn.recv(4096)
                                if not data:
                                    break
                                conn.sendall(data)
                                total += len(data)
                            except socket.timeout:
                                continue
                            except OSError:
                                break
                    echoed.append(total)

            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen(2)
                listener.settimeout(.2)
                echo_thread = threading.Thread(target=echo_worker, args=(listener,))
                echo_thread.start()
                try:
                    for bits in (32, 64):
                        raw = local(f"winexec vnet{bits}.exe {listener.getsockname()[1]}")
                        (out / f"vnet{bits}.log").write_bytes(raw)
                        assert b"VNET-PRIVATE CHILD CONNECT" in raw, raw[-5000:]
                        if negative:
                            assert b"!!! EXCEPTION: #PF" in raw, raw[-5000:]
                            assert b"VNET-PRIVATE PASS" not in raw, raw[-5000:]
                        else:
                            assert b"!!! EXCEPTION" not in raw, raw[-5000:]
                            assert b"VNET-PRIVATE CHILD 65536 BYTES OK" in raw, raw[-5000:]
                            assert b"VNET-PRIVATE PASS" in raw, raw[-5000:]
                            assert b"VNET-PRIVATE FAIL" not in raw, raw[-5000:]
                        passed(f"pe{bits}_private_cr3_network_{'negative' if negative else 'contract'}")
                finally:
                    echo_stop.set()
                    echo_thread.join(timeout=5)
                    assert not echo_thread.is_alive(), "echo worker did not exit"
            if not negative:
                assert echoed == [65536, 65536], echoed
            assert b"VNET-SSH-ALIVE" in remote("echo VNET-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_DWRITE_NUMBER") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_DWRITE_NUMBER_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec d3d9{bits}.exe")
                (out / f"d3d9{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-5000:]
                expected = b"D3D9-BACKEND MISSING EXPORT" if negative else b"D3D9-BACKEND PASS"
                assert expected in raw, raw[-5000:]
                passed(f"pe{bits}_d3d9_backend_{'negative' if negative else 'contract'}")
            raw = local("winexec dwrite-number.exe")
            (out / "dwrite-number.log").write_bytes(raw)
            assert b"!!! EXCEPTION" not in raw, raw[-5000:]
            expected = b"DWRITE-NUMBER CREATE FAILED" if negative else b"DWRITE-NUMBER PASS"
            assert expected in raw, raw[-5000:]
            passed("pe64_number_substitution_" + ("negative" if negative else "contract"))
            assert b"GRAPHICS-SSH-ALIVE" in remote("echo GRAPHICS-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_CONSOLE_ATTACH") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_CONSOLE_ATTACH_NEGATIVE") == "1"
            for bits in (32, 64):
                raw = local(f"winexec conattach{bits}.exe")
                (out / f"conattach{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-5000:]
                if negative:
                    assert b"CONSOLE-ATTACH MISSING EXPORT" in raw, raw[-5000:]
                    passed(f"negative_pe{bits}_attach_console_missing")
                else:
                    for check in ("PARENT-AND-EXPLICIT", "REDIRECT-AND-LIFETIME"):
                        assert f"CONSOLE-ATTACH {check} PASS".encode() in raw, raw[-5000:]
                    assert b"CONSOLE-ATTACH PASS" in raw and b"CONSOLE-ATTACH FAIL" not in raw, raw[-5000:]
                    passed(f"pe{bits}_attach_console_contract")
            assert b"CONSOLE-SSH-ALIVE" in remote("echo CONSOLE-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_SEH_DIAGNOSTIC") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_SEH_DIAGNOSTIC_NEGATIVE") == "1"
            for bits in (32, 64):
                for case in range(8):
                    data = local(f"winexec seh-launch.exe seh{bits}-{case}.exe")
                    (out / f"seh{bits}-{case}.log").write_bytes(data)
                    assert b"SEH-DIAGNOSTIC BEGIN" in data, data[-5000:]
                    fault = negative and (bits == 64 or case >= 3)
                    if fault:
                        expected = b"#GP General Protection" if bits == 64 and case in (0, 5) else b"#PF Page Fault"
                        assert expected in data and b"SEH-DIAGNOSTIC PASS" not in data, data[-5000:]
                    else:
                        assert b"SEH-DIAGNOSTIC PASS" in data and b"!!! EXCEPTION" not in data, data[-5000:]
                        if not negative:
                            assert b"SEH64-LOWIO" not in data, data[-5000:]
                            if case in (3, 4, 6, 7):
                                assert b"unreadable" in data, data[-5000:]
                            if case == 5 or (bits == 64 and case in (1, 2)):
                                assert b"unmapped" in data, data[-5000:]
                    assert b"[WINEXEC] PE64 process exited, code=0" in data, data[-5000:]
                    passed(f"pe{bits}_seh_diagnostic_case{case}_{'negative' if fault else 'safe'}")
            assert b"SEH-SSH-ALIVE" in remote("echo SEH-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_FAULT_READ") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_FAULT_READ_NEGATIVE") == "1"
            for bits in (32, 64):
                for boundary in (0, 1):
                    # A child has private page tables, unlike the root PE32
                    # task which shares the low identity-mapped kernel CR3.
                    raw = local(f"winexec fault-launch.exe fault{bits}-{boundary}.exe")
                    (out / f"fault{bits}-{boundary}.log").write_bytes(raw)
                    expected = b"#BP Breakpoint" if boundary else b"#PF Page Fault"
                    assert b"!!! EXCEPTION: " + expected in raw, raw[-5000:]
                    if negative:
                        assert raw.count(b"!!! EXCEPTION:") > 1, raw[-5000:]
                        passed(f"negative_pe{bits}_nested_fault_boundary{boundary}")
                    else:
                        assert raw.count(b"!!! EXCEPTION:") == 1, raw[-5000:]
                        assert b"Code @ RIP: <unmapped>" in raw, raw[-5000:]
                        assert b"[WINEXEC-CHILD] exit pid=" in raw, raw[-5000:]
                        assert b"[WINEXEC] PE64 process exited, code=0" in raw, raw[-5000:]
                        passed(f"pe{bits}_unmapped_ip_diagnostic_boundary{boundary}")
            assert b"FAULT-SSH-ALIVE" in remote("echo FAULT-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_WSASEND_QUEUE") == "1":
            spec = importlib.util.spec_from_file_location(
                "wsasend_queue_peer", Path("arch/x86/scripts/wsasend-queue-peer.py").resolve())
            peer_module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(peer_module)
            modes = [int(n) for n in os.environ.get(
                "OSITO_SSH_TEST_WSASEND_QUEUE_MODES", "0,1,2,3,4,5,0").split(",")]
            assert modes and all(n in range(6) for n in modes)
            bits_list = [int(n) for n in os.environ.get(
                "OSITO_SSH_TEST_WSASEND_QUEUE_BITS", "32,64").split(",")]
            assert bits_list and all(n in (32, 64) for n in bits_list)
            private = os.environ.get("OSITO_SSH_TEST_WSASEND_QUEUE_PRIVATE") == "1"
            peer = peer_module.Peer(out / "wsasend-queue-peer.json",
                                   expected=len(modes) * len(bits_list)).start()
            try:
                suites = []
                for bits in bits_list:
                    for sequence, mode in enumerate(modes):
                        raw = local(f"winexec queue{bits}.exe 10.0.2.2 {peer.port} {mode}" +
                                    (" --private" if private else ""))
                        (out / f"queue{bits}-{sequence}-mode{mode}.log").write_bytes(raw)
                        marker = (b"SENDQUEUE PRIVATE PASS" if private else
                                  b"SENDQUEUE EXIT-PENDING PASS" if mode == 5 else b"SENDQUEUE PASS")
                        assert marker in raw and b"SENDQUEUE FAIL" not in raw, raw[-8000:]
                        assert not any(s in raw for s in (b"!!! EXCEPTION", b"[SCHED] CORRUPT")), raw[-8000:]
                        suites.append((bits, mode))
                        passed(f"pe{bits}_sendqueue_{sequence}_mode{mode}" +
                               ("_private" if private else ""))
                peer.thread.join(timeout=40)
                assert len(peer.results) == len(suites), peer.results
                assert [(r["bits"], r["mode"]) for r in peer.results] == suites
                assert all(r.get("matches") for r in peer.results), peer.results
                for command, marker in (
                        ("win32-iocp-test", b"Win32 IOCP test: PASS"),
                        ("win32-wsock-test", b"Winsock contract test: PASS")):
                    raw = local(command)
                    (out / f"{command}.log").write_bytes(raw)
                    assert marker in raw and b"failures=0" in raw, raw[-8000:]
                    passed(command.replace("-", "_"))
                assert b"SENDQUEUE-SSH-ALIVE" in remote("echo SENDQUEUE-SSH-ALIVE").stdout
                passed("sendqueue_wire_integrity_and_ssh_alive")
            finally:
                peer.close()
            return
        if os.environ.get("OSITO_SSH_TEST_WSASEND_FULL") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_WSASEND_FULL_NEGATIVE") == "1"
            spec = importlib.util.spec_from_file_location(
                "wsasend_full_peer", Path("arch/x86/scripts/wsasend-completion-peer.py").resolve())
            peer_module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(peer_module)
            peer = peer_module.Peer(out / "wsasend-peer.json").start()
            try:
                for bits in (32, 64):
                    for nonblocking in (0, 1):
                        raw = local(f"winexec send{bits}.exe 10.0.2.2 {peer.port} {nonblocking}")
                        (out / f"send{bits}-{nonblocking}.log").write_bytes(raw)
                        match = re.search(rb"FULLSEND RESULT requested=(\d+) completed=(\d+) nonblocking=(\d+) call_ms=(\d+)", raw)
                        assert match and int(match[1]) == 2097152 and int(match[3]) == nonblocking, raw[-5000:]
                        assert b"!!! EXCEPTION" not in raw and b"[SCHED] CORRUPT" not in raw
                        if negative:
                            assert 0 < int(match[2]) < 2097152 and b"FULLSEND FAIL complete scatter/gather count" in raw
                            assert b"FULLSEND PASS" not in raw
                        else:
                            assert int(match[2]) == 2097152 and b"FULLSEND PASS" in raw
                            assert b"FULLSEND FAIL" not in raw
                        passed(f"pe{bits}_wsasend_full_{nonblocking}_{'negative' if negative else 'contract'}")
                peer.thread.join(timeout=40)
                assert len(peer.results) == 4, peer.results
                expected_suites = [(bits, mode) for bits in (32, 64) for mode in (0, 1)]
                assert [(record["bits"], record["nonblocking"]) for record in peer.results] == expected_suites
                if negative:
                    assert all(record["received"] < 2097152 for record in peer.results), peer.results
                else:
                    assert all(record.get("matches") for record in peer.results), peer.results
                assert b"FULLSEND-SSH-ALIVE" in remote("echo FULLSEND-SSH-ALIVE").stdout
                passed("wsasend_full_peer_integrity_and_ssh_alive")
            finally:
                peer.close()
            return
        if os.environ.get("OSITO_SSH_TEST_HWBP_TRACE") == "1":
            profiles = json.loads((out / "hwbp-trace/probes.json").read_text())
            for profile in profiles:
                bits = profile["bits"]
                addr, signature = profile["address"], profile["signature"]
                assert hashlib.sha256((out / f"hwbp-trace/trace{bits}.exe").read_bytes()).hexdigest() == profile["sha256"]
                for case in ("limit", "mismatch", "scope", "clear", "watch", "caller"):
                    assert remote("hwbp clear 0").returncode == 0
                    if case == "watch":
                        local(f"watch {addr} x 1")
                    else:
                        key = "00" * 16 if case == "mismatch" else signature
                        caller_option = f" caller{bits}:{profile['caller_offset']}" if case == "caller" else ""
                        r = remote(f"hwbp trace 0 1 {addr} edi32 2 {key}{caller_option}")
                        assert r.returncode == 0 and b"armed slot=0" in r.stdout, (r.stdout, r.stderr)
                        if case == "clear":
                            assert remote("hwbp clear 0").returncode == 0
                    start = serial.stat().st_size
                    suffix = " --scope" if case == "scope" else ""
                    r = remote(f"console launch winexec trace{bits}.exe{suffix}")
                    assert r.returncode == 0 and b"queued request" in r.stdout, (r.stdout, r.stderr)
                    deadline = time.monotonic() + 30
                    while b"HWBP-FIXTURE READY" not in serial.read_bytes()[start:]:
                        assert time.monotonic() < deadline, "fixture did not enter guest code"
                        time.sleep(.05)
                    r = remote("hwbp status")
                    assert r.returncode == 0, (r.stdout, r.stderr)
                    if case == "scope":
                        assert b"HWBP-FIXTURE CHILD DONE" in serial.read_bytes()[start:]
                        assert b"hits=0 limit=2" in r.stdout and b"active=1" in r.stdout, r.stdout
                    r = remote("console status")
                    assert r.returncode == 0 and b"running" in r.stdout, r.stdout
                    data = prompt(start, 30)
                    (out / f"hwbp{bits}-{case}.log").write_bytes(data)
                    assert b"HWBP-FIXTURE DONE" in data and b"HWBP-FIXTURE FAIL" not in data
                    assert not any(marker in data for marker in
                        (b"!!! EXCEPTION", b"[SCHED] CORRUPT", b"[SCHED-OWNER-MISMATCH]")), data[-4000:]
                    hits = re.findall(rb"\[HWBP-TRACE\] slot=0 pid=(\d+) .*? edi32=(?:0x)?([0-9a-fA-F]+) hits=(\d+)", data)
                    if case in ("limit", "scope", "caller"):
                        assert len(hits) == 2 and all(pid == b"1" and int(value, 16) == 0x140000a5 for pid, value, count in hits), hits
                        assert [int(count) for pid, value, count in hits] == [1, 2], hits
                        assert b"disabled hit limit slot=0" in data
                        if case == "caller":
                            callers = re.findall(rb"\[HWBP-TRACE\] slot=0 .*? caller=(?:0x)?([0-9a-fA-F]+)", data)
                            expected_callers = {int(value, 16) for value in profile["caller_returns"]}
                            assert len(callers) == 2 and all(int(value, 16) in expected_callers for value in callers), (callers, expected_callers)
                        else:
                            trace_lines = re.findall(rb"^\[HWBP-TRACE\] slot=0[^\r\n]*", data, re.MULTILINE)
                            assert all(b"caller=" not in line for line in trace_lines)
                    elif case == "mismatch":
                        assert not hits and data.count(b"disabled signature mismatch slot=0") == 1
                    elif case == "clear":
                        assert not hits and b"[HWBP] slot=" not in data
                    else:
                        assert not hits and data.count(b"[HWBP] slot=0") == 3
                    assert remote("hwbp clear 0").returncode == 0
                    passed(f"pe{bits}_hwbp_{case}_returns_and_ssh_busy")
            for command in ("hwbp trace 0 1 0 edi32 2 " + "90" * 16,
                            "hwbp trace 0 1 140002ff1 edi32 2 " + "90" * 16,
                            "hwbp trace 0 1 140002000 edi32 33 " + "90" * 16,
                            "hwbp trace 0 2147483647 140002000 edi32 2 " + "90" * 16,
                            "hwbp trace 0 1 140002000 edi32 2 " + "90" * 16 + " caller64:513",
                            "hwbp trace 0 1 140002000 edi32 2 " + "90" * 16 + " caller64:1",
                            "hwbp trace 0 1 140002000 edi32 2 " + "90" * 16 + " extra",
                            "hwbp clear 4"):
                assert remote(command).returncode != 0, command
            assert b"HWBP-SSH-ALIVE" in remote("echo HWBP-SSH-ALIVE").stdout
            passed("hwbp_invalid_requests_rejected_and_ssh_alive")
            return
        if os.environ.get("OSITO_SSH_TEST_CHILD_CAPACITY") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_CHILD_CAPACITY_NEGATIVE") == "1"
            abis = (int(os.environ.get("OSITO_SSH_TEST_CHILD_CAPACITY_BITS", "32")),) if negative else (32, 64)
            assert all(bits in (32, 64) for bits in abis), "unsupported child-capacity ABI"
            for bits in abis:
                if negative:
                    start = serial.stat().st_size
                    r = remote(f"console launch winexec capacity{bits}.exe --limit")
                    assert r.returncode == 0 and b"queued request" in r.stdout, r.stdout
                    deadline = time.monotonic() + 90
                    data = b""
                    while time.monotonic() < deadline:
                        data = serial.read_bytes()[start:]
                        if b"[SCHED] CORRUPT PID" in data:
                            break
                        time.sleep(.1)
                    (out / f"capacity{bits}-negative.log").write_bytes(data)
                    normalized = data.replace(b"\r", b"")
                    assert b"CHILD-CAPACITY CREATED 16\n" in normalized, data[-6000:]
                    assert b"CHILD-CAPACITY CREATED 17\n" not in normalized, data[-6000:]
                    assert b"[SCHED] CORRUPT PID" in data, data[-6000:]
                    assert b"RIP=0x0x0000000000000000" in data, data[-6000:]
                    assert b"CHILD-CAPACITY PASS" not in data, data[-6000:]
                    passed(f"pe{bits}_child_slot_17_null_entry_reproduced")
                    # The parent is stranded in CreateProcess; dispose this VM.
                    return
                data = local(f"winexec capacity{bits}.exe --limit")
                (out / f"capacity{bits}.log").write_bytes(data)
                for marker in (b"CHILD-CAPACITY LIMIT PASS", b"CHILD-CAPACITY CREATED 31"):
                    assert data.count(marker) == 2, data[-6000:]
                for marker in (b"CHILD-CAPACITY ROUND PASS 1", b"CHILD-CAPACITY ROUND PASS 2",
                               b"CHILD-CAPACITY PASS"):
                    assert marker in data, data[-6000:]
                assert not any(marker in data for marker in
                    (b"CHILD-CAPACITY FAIL", b"[SCHED] CORRUPT", b"[SCHED-OWNER-MISMATCH]",
                     b"!!! EXCEPTION", b"!!! #PF", b"!!! #BP")), data[-6000:]
                passed(f"pe{bits}_31_live_children_limit_and_reuse")
            assert b"CAPACITY-SSH-ALIVE" in remote("echo CAPACITY-SSH-ALIVE").stdout
            passed("child_capacity_ssh_after_exit")
            return
        if os.environ.get("OSITO_SSH_TEST_CHILD_EXIT") == "1":
            if os.environ.get("OSITO_SSH_TEST_CHILD_EXIT_NEGATIVE") == "1":
                # The old kernel strands its global heap lock; do not wait for
                # normal teardown or reuse this VM after observing the failure.
                start = serial.stat().st_size
                r = remote("console launch winexec exit64.exe")
                assert r.returncode == 0 and b"queued request" in r.stdout, r.stdout
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    data = serial.read_bytes()[start:]
                    if b"CHILD-EXIT FAIL heap probe timed out" in data:
                        break
                    time.sleep(.1)
                (out / "exit64-negative.log").write_bytes(data)
                assert b"CHILD-EXIT FAIL heap probe timed out" in data, data[-5000:]
                assert b"CHILD-EXIT PASS" not in data, data[-5000:]
                passed("negative_child_termination_strands_heap")
            else:
                for bits in (32, 64):
                    data = local(f"winexec exit{bits}.exe")
                    (out / f"exit{bits}.log").write_bytes(data)
                    assert b"!!! EXCEPTION" not in data, data[-5000:]
                    for marker in (b"CHILD-EXIT EXTERNAL PASS", b"CHILD-EXIT WORKER PASS",
                                   b"CHILD-EXIT WORKER-LOCK PASS", b"CHILD-EXIT PASS"):
                        assert marker in data, data[-5000:]
                    assert b"CHILD-EXIT FAIL" not in data, data[-5000:]
                    passed(f"pe{bits}_child_exit_does_not_abandon_heap")
            assert b"CHILD-SSH-ALIVE" in remote("echo CHILD-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_FILE_POSITION") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_FILE_POSITION_NEGATIVE") == "1"
            for bits in (32, 64):
                data = local(f"winexec position{bits}.exe")
                (out / f"position{bits}.log").write_bytes(data)
                assert b"!!! EXCEPTION" not in data, data[-5000:]
                if negative:
                    assert b"FILE-POSITION FAIL seek overlapped handle" in data, data[-5000:]
                    assert b"FILE-POSITION FAIL extended length" in data, data[-5000:]
                    assert b"FILE-POSITION PASS" not in data, data[-5000:]
                else:
                    assert b"FILE-POSITION PASS" in data and b"FILE-POSITION FAIL" not in data, data[-5000:]
                passed(f"pe{bits}_file_position_{'negative' if negative else 'contract'}")
            if not negative:
                data = local("win32-file-test")
                (out / "nt-file.log").write_bytes(data)
                assert b"NT file contract test: PASS" in data, data[-5000:]
                assert b"failures=0" in data, data[-5000:]
                passed("nt_file_explicit_cursor_and_io_offsets")
            assert b"POSITION-SSH-ALIVE" in remote("echo POSITION-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_FILE_APPEND") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_FILE_APPEND_NEGATIVE") == "1"
            for bits in (32, 64):
                data = local(f"winexec append{bits}.exe")
                (out / f"append{bits}.log").write_bytes(data)
                assert b"!!! EXCEPTION" not in data, data[-5000:]
                if negative:
                    assert b"APPEND FAIL append contents" in data, data[-5000:]
                    assert b"APPEND FAIL append size" in data, data[-5000:]
                    assert b"APPEND PASS" not in data, data[-5000:]
                else:
                    assert b"APPEND PASS" in data and b"APPEND FAIL" not in data, data[-5000:]
                passed(f"pe{bits}_file_append_{'negative' if negative else 'contract'}")
            if not negative:
                data = local("win32-file-test")
                (out / "nt-file.log").write_bytes(data)
                assert b"NT file contract test: PASS" in data, data[-5000:]
                assert b"failures=0" in data, data[-5000:]
                passed("nt_file_offsets_locks_and_append_retry")
            assert b"APPEND-SSH-ALIVE" in remote("echo APPEND-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_IOCP_DEADLINE") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_IOCP_DEADLINE_NEGATIVE") == "1"
            r = remote("console launch desktop")
            assert r.returncode == 0 and b"queued request" in r.stdout, r.stdout
            for _ in range(50):
                r = remote("console status")
                if b"console: idle" in r.stdout:
                    break
                time.sleep(.1)
            assert b"console: idle" in r.stdout, r.stdout
            for bits in (32, 64):
                data = local(f"winexec deadline{bits}.exe")
                (out / f"deadline{bits}.log").write_bytes(data)
                assert b"!!! EXCEPTION" not in data, data[-5000:]
                if negative:
                    assert b"IOCP-DEADLINE EARLY base" in data, data[-5000:]
                    assert b"IOCP-DEADLINE EARLY ex" in data, data[-5000:]
                    assert b"IOCP-DEADLINE PASS" not in data, data[-5000:]
                else:
                    assert b"IOCP-DEADLINE PASS" in data, data[-5000:]
                    assert b"IOCP-DEADLINE FAIL" not in data, data[-5000:]
                passed(f"pe{bits}_iocp_deadline_{'negative' if negative else 'contract'}")
            for command, marker in (
                    ("win32-iocp-test", b"Win32 IOCP test: PASS"),
                    ("win32-wsock-test", b"Winsock contract test: PASS")):
                data = local(command)
                (out / f"{command}.log").write_bytes(data)
                assert marker in data and b"!!! EXCEPTION" not in data, data[-5000:]
                assert b"failures=0" in data, data[-5000:]
                passed(command.replace("-", "_"))
            assert b"IOCP-SSH-ALIVE" in remote("echo IOCP-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_CPU_CYCLES") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_CPU_CYCLES_NEGATIVE") == "1"
            # The idle task starts with desktop. Without it NtDelayExecution
            # may use a real busy-wait fallback, which correctly costs cycles.
            r = remote("console launch desktop")
            assert r.returncode == 0 and b"queued request" in r.stdout, r.stdout
            for _ in range(50):
                r = remote("console status")
                if b"console: idle" in r.stdout:
                    break
                time.sleep(.1)
            assert b"console: idle" in r.stdout, r.stdout
            assert b"idle" in remote("ps").stdout
            for bits in (32, 64):
                data = local(f"winexec cycles{bits}.exe")
                (out / f"cycles{bits}.log").write_bytes(data)
                assert b"!!! EXCEPTION" not in data, data[-5000:]
                if negative:
                    assert b"CPU-CYCLES FAIL 01" in data, data[-5000:]
                    passed(f"negative_pe{bits}_process_cycles_unsupported")
                else:
                    for check in ("ACCESS", "RUN-VS-WAIT", "THREAD-LIFETIME", "PROCESS-LIFETIME"):
                        assert f"CPU-CYCLES {check} PASS".encode() in data, data[-5000:]
                    assert b"CPU-CYCLES PASS" in data and b"FAIL" not in data, data[-5000:]
                    passed(f"pe{bits}_process_and_thread_cpu_cycle_contract")
            assert b"CYCLES-SSH-ALIVE" in remote("echo CYCLES-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_WGL") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_WGL_NEGATIVE") == "1"
            for bits in (32, 64):
                data = local(f"winexec wgl{bits}.exe")
                (out / f"wgl{bits}.log").write_bytes(data)
                assert b"!!! EXCEPTION" not in data, data[-5000:]
                if negative:
                    assert b"WGL-BACKEND INVALID-SUCCESS" in data, data[-5000:]
                    assert b"WGL-BACKEND FAIL" in data, data[-5000:]
                    passed(f"negative_pe{bits}_publishes_no_renderer_context")
                else:
                    assert b"WGL-BACKEND PASS" in data, data[-5000:]
                    assert b"FAIL" not in data, data[-5000:]
                    passed(f"pe{bits}_no_renderer_context_and_pixel_format_contract")
            if os.environ.get("OSITO_SSH_TEST_WGL_HELPER_DIR"):
                data = local("winexec gldriverquery64.exe")
                (out / "gldriverquery64.log").write_bytes(data)
                if negative:
                    assert b"!!! EXCEPTION: #PF" in data, data[-5000:]
                    assert b"return=0x0x00000001400B6E57" in data, data[-5000:]
                    passed("negative_steam_gl_helper_exact_null_call")
                else:
                    assert b"!!! EXCEPTION" not in data, data[-5000:]
                    assert b"[WINEXEC] PE64 process exited, code=2" in data, data[-5000:]
                    assert b"context unavailable: no rendering backend" in data, data[-5000:]
                    passed("steam_gl_helper_exits_without_renderer_or_fault")
            assert b"WGL-SSH-ALIVE" in remote("echo WGL-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_TLS_REAP") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_TLS_REAP_NEGATIVE") == "1"
            if not negative:
                data = local("winexec pfimports32.exe")
                assert b"PF-IMPORTS32 PASS" in data and b"PF-IMPORTS32 FAIL" not in data, data[-5000:]
                assert b"!!! EXCEPTION" not in data, data[-5000:]
                passed("pe32_missing_imports_stdcall_and_cdecl_contract")
            # Desktop startup currently owns the reaper/idle-task startup.
            r = remote("console launch desktop")
            assert r.returncode == 0 and b"queued request" in r.stdout, r.stdout
            for _ in range(50):
                r = remote("console status")
                if b"console: idle" in r.stdout:
                    break
                time.sleep(.1)
            assert b"console: idle" in r.stdout, r.stdout
            assert b"reaper" in remote("ps").stdout
            symbols = {}
            for line in subprocess.check_output(["nm", str(out / "kernel.elf")], text=True).splitlines():
                parts = line.split()
                if len(parts) == 3:
                    symbols[parts[2]] = int(parts[0], 16)
            probe_args = " ".join(f"{symbols[name]:x}" for name in (
                "win64_tls_register_static", "win64_tls_unregister_image",
                "proc_current_pid", "proc_kill_pid", "paging_translate_in_cr3"))
            r = remote(f"console launch winexec tlsparent.exe {probe_args} {0 if negative else 1}")
            assert r.returncode == 0 and b"queued request" in r.stdout, r.stdout
            deadline = time.monotonic() + 80
            while time.monotonic() < deadline:
                data = serial.read_bytes()
                if b"TLS-REAP64 PASS" in data or b"Process: reaper" in data:
                    break
                assert b"TLS-REAP64 FAIL" not in data, data[-6000:]
                time.sleep(.2)
            if negative:
                assert b"TLS-REAP64 ABNORMAL-STOP" in data and b"Process: reaper" in data, data[-8000:]
                r = remote("ps")
                assert r.returncode == 0 and any(b"ZOMBIE" in line and b"reaper" in line for line in r.stdout.splitlines()), r.stdout
                passed("negative_private_tls_vector_fault_kills_reaper")
            else:
                assert b"TLS-REAP64 PASS" in data and b"TLS-REAP64 IMPORT-CONTRACT PASS" in data, data[-8000:]
                assert not any(s in data for s in (b"!!! EXCEPTION", b"inaccessible vector slot", b"TLS-REAP64 FAIL")), data[-8000:]
                r = remote("ps")
                assert r.returncode == 0 and any(b"reaper" in line and b"ZOMBIE" not in line for line in r.stdout.splitlines()), r.stdout
                passed("tls_normal_exit_and_reaper_cleanup_private_cr3")
                passed("crt_lead_byte_and_process_cycles_query_contract")
            assert b"TLS-REAP-SSH-ALIVE" in remote("echo TLS-REAP-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_FILEGUARD") == "1":
            symbols = {}
            for line in subprocess.check_output(["nm", str(out / "kernel.elf")], text=True).splitlines():
                parts = line.split()
                if len(parts) == 3:
                    symbols[parts[2]] = int(parts[0], 16)
            probe_args = " ".join(f"{symbols[name]:x}" for name in (
                "g_handle_table", "handle_lookup_for_process",
                "nt_file_io_guard_begin", "nt_file_io_guard_end"))

            def word(name, offset=0, size=8):
                address = symbols[name] - 0xffff800000000000 + offset
                unit = "g" if size == 8 else "w"
                raw = qmp({"execute": "human-monitor-command", "arguments": {
                    "command-line": f"xp /1{unit}x 0x{address:x}"}})
                return int(raw.split(":", 1)[1].strip(), 16)

            if os.environ.get("OSITO_SSH_TEST_FILEGUARD_NEGATIVE") == "1":
                assert b"lockup: armed" in remote("lockup arm 5").stdout
                r = remote(f"console launch winexec fgparent.exe {probe_args}")
                assert r.returncode == 0 and b"queued request" in r.stdout, r.stdout
                deadline = time.monotonic() + 30
                while not word("lockup_nmi_captured", size=4) and time.monotonic() < deadline:
                    time.sleep(.2)
                assert word("lockup_nmi_captured", size=4) == 1
                rip = word("lockup_nmi_frame", 136)
                flags = word("lockup_nmi_frame", 152)
                results["negative_snapshot"] = {"rip": hex(rip), "flags": hex(flags),
                    "cr2": hex(word("lockup_nmi_cr2"))}
                assert symbols["file_lock_cancel_waiters"] <= rip < symbols["nt_file_io_guard_begin"], hex(rip)
                assert not flags & 0x200 and word("lockup_nmi_cr2") == 0x6000000008
                passed("old_kernel_private_guard_fault_then_irq_off_cleanup_deadlock")
                return
            for attempt in range(2):
                raw = local(f"winexec fgparent.exe {probe_args}")
                assert b"FILEGUARD64 PASS" in raw and b"FILEGUARD64 FAIL" not in raw, raw[-5000:]
                assert raw.count(b"FILEGUARD64 CHILD PASS") == 2, raw[-5000:]
                assert b"FILEGUARD64 CHILD DIFFERENT-CR3" in raw, raw[-5000:]
                assert word("g_file_io_guards") == 0
                assert word("g_file_range_locks") == 0
                passed(f"cross_cr3_guard_and_abandoned_token_reclaimed_{attempt}")
            raw = local("win32-file-test")
            assert b"NT file contract test: PASS" in raw, raw[-5000:]
            passed("nt_file_contract_selftest")
            raw = local("winexec flock32.exe")
            assert b"FILE LOCK TEST PASS" in raw and b"FILE LOCK TEST FAIL" not in raw, raw[-5000:]
            assert not any(marker in serial.read_bytes() for marker in (
                b"!!! EXCEPTION", b"Controller DEAD", b"BOT transport error"))
            assert b"FILEGUARD-SSH-ALIVE" in remote("echo FILEGUARD-SSH-ALIVE").stdout
            passed("pe32_locks_and_ssh_after_pe64_process_teardown")
            return
        if os.environ.get("OSITO_SSH_TEST_LOCKUP") == "1":
            symbols = {}
            for line in subprocess.check_output(["nm", str(out / "kernel.elf")], text=True).splitlines():
                parts = line.split()
                if len(parts) == 3:
                    symbols[parts[2]] = int(parts[0], 16)
            probe_args = " ".join(f"{symbols[name]:x}" for name in
                                  ("lockup_nmi_captured", "sched_preempt_depth"))

            def read_word(name, size=4, offset=0):
                addr = symbols[name] - 0xffff800000000000 + offset
                unit = "g" if size == 8 else "w"
                raw = qmp({"execute": "human-monitor-command", "arguments": {
                    "command-line": f"xp /1{unit}x 0x{addr:x}"}})
                return int(raw.split(":", 1)[1].strip(), 16)

            r = remote("lockup status")
            assert r.returncode == 0 and b"observer_running=0000000000000000" in r.stdout
            r = remote("lockup arm 2")
            assert r.returncode == 0 and b"lockup: armed" in r.stdout, r.stdout
            r = remote("lockup sample")
            assert r.returncode == 0 and b"sample requested" in r.stdout, r.stdout
            time.sleep(.2)
            r = remote("lockup status")
            assert b"requested sample" in r.stdout and b"RIP=" in r.stdout, r.stdout
            assert b"No completed" not in r.stdout, r.stdout
            assert b"stop requested" in remote("lockup off").stdout
            time.sleep(.2)
            passed("requested_nmi_returns_to_live_ssh")
            for soft in (0, 1):
                assert b"lockup: armed" in remote("lockup arm 2").stdout
                raw = local(f"winexec lockup64.exe {probe_args} {soft}")
                assert b"LOCKUP64 PASS" in raw and b"LOCKUP64 FAIL" not in raw, raw[-5000:]
                time.sleep(.2)
                r = remote("lockup status")
                # Under CLI both stop. The older scheduler observation can
                # cross its threshold just before the last timer observation.
                reasons = (b"BSP scheduling stopped",) if soft else (
                    b"BSP timer stopped", b"BSP scheduling stopped")
                assert any(why in r.stdout for why in reasons) and b"RIP=" in r.stdout, r.stdout
                flags = int(r.stdout.split(b"FLAGS=", 1)[1].split()[0], 16)
                assert bool(flags & 0x200) == bool(soft) and flags & 0x400, r.stdout
                assert read_word("lockup_nmi_captured") == 1
                passed(f"nmi_preserves_fpu_and_flags_{'preempt' if soft else 'cli'}")
            assert b"lockup: armed" in remote("lockup arm 2").stdout
            for c in (f"winexec lockup64.exe {probe_args} 2\r").encode():
                uart.sendall(bytes([c])); time.sleep(.05)
            deadline = time.monotonic() + 15
            while not read_word("lockup_nmi_captured") and time.monotonic() < deadline:
                time.sleep(.2)
            assert read_word("lockup_nmi_captured") == 1
            assert not (read_word("lockup_nmi_frame", 8, 152) & 0x200)
            time.sleep(1)
            qmp({"execute": "screendump", "arguments": {"filename": str(out / "lockup.ppm")}})
            magic, geometry, maximum, pixels = (out / "lockup.ppm").read_bytes().split(b"\n", 3)
            assert magic == b"P6" and maximum == b"255"
            width, height = map(int, geometry.split())
            spec = importlib.util.spec_from_file_location("osito_screen",
                    Path(__file__).resolve().parents[3] / "tools/osito-screen.py")
            screen = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(screen)
            screen.save_png(out / "lockup.png", {"width": width, "height": height}, pixels)
            assert sum(pixels[i:i+3] == b"\xff\xff\xff" for i in range(0, len(pixels), 3)) > 1000
            assert not any(s in serial.read_bytes() for s in (b"!!! EXCEPTION", b"Controller DEAD"))
            passed("hard_stall_register_snapshot_and_visible_gop_report")
            return
        if os.environ.get("OSITO_SSH_TEST_THREAD_OWNER") == "1":
            raw = local("winexec ownparent.exe")
            (out / "thread-owner.log").write_bytes(raw)
            assert b"THREAD-OWNER64 CHILD SAME-PEB" in raw, raw[-5000:]
            negative = os.environ.get("OSITO_SSH_TEST_THREAD_OWNER_NEGATIVE") == "1"
            if negative:
                assert b"THREAD-OWNER64 FAIL 11" in raw, raw[-5000:]
            else:
                assert b"THREAD-OWNER64 WORKER-CONTEXT PASS" in raw, raw[-5000:]
                assert b"THREAD-OWNER64 PASS" in raw and b"THREAD-OWNER64 FAIL" not in raw, raw[-5000:]
            assert not any(s in raw for s in (b"!!! EXCEPTION", b"Controller DEAD", b"BOT transport error")), raw[-5000:]
            r = remote("echo SSH-AFTER-THREAD-OWNER")
            assert r.returncode == 0 and b"SSH-AFTER-THREAD-OWNER" in r.stdout, (r.stdout, r.stderr)
            passed("negative_same_peb_worker_misidentified" if negative else
                   "same_peb_process_lifetime_and_worker_context_isolation")
            return
        if os.environ.get("OSITO_SSH_TEST_FILE_CACHE") == "1":
            assert storage == "usb"
            def cache_usb_reads(label):
                r = remote("usb-perf")
                assert r.returncode == 0, (r.stdout, r.stderr)
                (out / f"cache-usb-{label}.log").write_bytes(r.stdout)
                rows = {}
                for line in r.stdout.decode().splitlines():
                    fields = dict(token.split("=", 1) for token in line.split() if "=" in token)
                    if fields.get("op") == "read":
                        rows = fields
                assert rows and int(rows["failures"]) == 0 and int(rows["bad_clock"]) == 0, rows
                return int(rows["calls"])

            measurements = {}
            for bits in (32, 64):
                raw = local(f"winexec filecache{bits}.exe prepare")
                (out / f"cache-{bits}-prepare.log").write_bytes(raw)
                assert b"FILE-CACHE PREPARED" in raw and b"FILE-CACHE FAIL" not in raw, raw[-5000:]
                mode_counts = {}
                for mode in ("unbuffered", "random", "buffered", "wide", "exit-open"):
                    before = cache_usb_reads(f"{bits}-{mode}-before")
                    raw = local(f"winexec filecache{bits}.exe {mode}")
                    (out / f"cache-{bits}-{mode}.log").write_bytes(raw)
                    assert b"FILE-CACHE STREAM PASS" in raw and b"FILE-CACHE FAIL" not in raw, raw[-5000:]
                    after = cache_usb_reads(f"{bits}-{mode}-after")
                    mode_counts[mode] = after - before
                raw = local(f"winexec filecache{bits}.exe contract")
                (out / f"cache-{bits}-contract.log").write_bytes(raw)
                assert b"FILE-CACHE CONTRACT PASS" in raw and b"FILE-CACHE FAIL" not in raw, raw[-5000:]
                measurements[bits] = mode_counts
                (out / "cache-read-calls.json").write_text(json.dumps(measurements, indent=2))
                assert mode_counts["unbuffered"] >= 1024 and mode_counts["random"] >= 1024, mode_counts
                if os.environ.get("OSITO_SSH_TEST_FILE_CACHE_NEGATIVE") == "1":
                    assert mode_counts["buffered"] >= 1024, mode_counts
                    passed(f"pe{bits}_negative_small_reads_reach_usb_without_cache")
                else:
                    for mode in ("buffered", "wide", "exit-open"):
                        assert 32 <= mode_counts[mode] < mode_counts["unbuffered"] // 8, mode_counts
                    passed(f"pe{bits}_small_reads_coalesce_and_writes_truncate_locks_close_remain_correct")
            raw = serial.read_bytes()
            assert not any(s in raw for s in (b"!!! EXCEPTION", b"Controller DEAD",
                b"BOT transport error", b"Bulk transfer timeout")), raw[-5000:]
            passed("file_cache_both_abis_keep_ssh_and_usb_alive")
            return
        if os.environ.get("OSITO_SSH_TEST_USB_READ") == "1":
            assert storage == "usb"
            def usb_perf():
                r = remote("usb-perf")
                assert r.returncode == 0 and b"[USB-PERF]" in r.stdout, (r.stdout, r.stderr)
                rows = {}
                for line in r.stdout.decode().splitlines():
                    fields = dict(token.split("=", 1) for token in line.split() if "=" in token)
                    if "op" in fields:
                        name = fields.pop("op")
                        rows[name] = {k: int(v) for k, v in fields.items()}
                assert set(rows) == {"init", "read", "write", "flush"}, rows
                assert all(row["bad_clock"] == row["failures"] == 0 for row in rows.values()), rows
                return r.stdout, rows

            _, previous = usb_perf()
            for bits in (32, 64):
                raw = local(f"winexec usbread{bits}.exe")
                (out / f"usb-read{bits}.log").write_bytes(raw)
                assert b"USB-READ ALL PASS" in raw and b"USB-READ FAIL" not in raw, raw[-5000:]
                assert raw.count(b"USB-READ PASS round=") == 6, raw[-5000:]
                assert not any(s in raw for s in (
                    b"!!! EXCEPTION", b"Controller DEAD", b"BOT transport error",
                    b"Bulk transfer timeout")), raw[-5000:]
                r = remote(f"echo SSH-AFTER-USB-READ-{bits}")
                assert r.returncode == 0 and f"SSH-AFTER-USB-READ-{bits}".encode() in r.stdout, (r.stdout, r.stderr)
                passed(f"pe{bits}_usb_batched_reads_match_bytes_boundaries_eof_and_ssh_survives")
                report, counters = usb_perf()
                (out / f"usb-perf{bits}.log").write_bytes(report)
                assert counters["read"]["ok_bytes"] - previous["read"]["ok_bytes"] >= 6 * 2 * 1024 * 1024
                assert counters["read"]["hold_ticks"] > previous["read"]["hold_ticks"]
                assert counters["read"]["max_hold_ticks"] > 0
                previous = counters
            r = remote("usb-perf reset")
            assert r.returncode == 126, (r.stdout, r.stderr)
            passed("usb_wait_timings_count_both_abis_without_resetting_or_changing_io")
            return
        if os.environ.get("OSITO_SSH_TEST_USB_STACK") == "1":
            assert storage == "usb"
            raw = local("winexec usbdma64.exe")
            assert b"USB-STACK64 PASS" in raw and b"USB-STACK64 FAIL" not in raw, raw[-4000:]
            assert not any(s in raw for s in (b"Controller DEAD", b"BOT transport error", b"Bulk transfer timeout")), raw[-4000:]
            r = remote("echo SSH-AFTER-PE64")
            assert r.returncode == 0 and b"SSH-AFTER-PE64" in r.stdout, (r.stdout, r.stderr)
            passed("pe64_private_stack_usb_file_roundtrip_and_ssh_survives")
            return
        if os.environ.get("OSITO_SSH_TEST_STARTUP_ONLY") or os.environ.get("OSITO_SSH_TEST_REBOOT_ONLY"):
            r = remote("sshd autostart on")
            assert r.returncode == 0 and b"autostart policy saved" in r.stdout, (r.stdout, r.stderr)
            start = serial.stat().st_size
            r = remote("kexec next.elf", extra=("-o", "ServerAliveInterval=3", "-o", "ServerAliveCountMax=3"))
            assert r.returncode == 255, (r.returncode, r.stdout, r.stderr)
            assert b"SSHTest" in prompt(start)
            for _ in range(20):
                r = remote("grep sshd- diag/boot1.log", extra=("-o", "StrictHostKeyChecking=yes",))
                if r.returncode == 0:
                    break
                time.sleep(.5)
            assert r.returncode == 0, (r.stdout, r.stderr)
            positions = [r.stdout.find(stage) for stage in stages]
            assert all(n >= 0 for n in positions) and positions == sorted(positions), r.stdout[-2000:]
            passed("automatic_ssh_start_after_kexec_with_persistent_checkpoints")
            if os.environ.get("OSITO_SSH_TEST_REBOOT_ONLY"):
                for boot in (2, 3):
                    start = serial.stat().st_size
                    r = remote("reboot --check")
                    assert r.returncode == 0 and b"reset disabled" in r.stdout, (r.stdout, r.stderr)
                    assert b"ACPI reset space=" in r.stdout and b"Checkpoint accepted by storage" in r.stdout, r.stdout
                    assert b"SSHTest" not in serial.read_bytes()[start:]
                    r = remote(f"grep reboot-check diag/boot{boot - 1}.log")
                    assert r.returncode == 0 and b"reason=reboot-check" in r.stdout, (r.stdout, r.stderr)
                    r = remote("reboot --typo")
                    assert r.returncode == 126 and b"Usage: reboot" in r.stdout, (r.stdout, r.stderr)
                    r = remote("console status")
                    assert r.returncode == 0 and b"console: idle" in r.stdout, (r.stdout, r.stderr)
                    r = remote("console launch reboot --check")
                    assert r.returncode == 0 and b"queued request" in r.stdout, (r.stdout, r.stderr)
                    for _ in range(20):
                        r = remote("console status")
                        if b"console: idle" in r.stdout and b"returned" in r.stdout:
                            break
                        time.sleep(.1)
                    assert r.returncode == 0 and b"returned" in r.stdout, (r.stdout, r.stderr)
                    assert b"SSHTest" not in serial.read_bytes()[start:]
                    r = remote(f"grep REBOOT diag/boot{boot - 1}.log")
                    assert r.returncode == 0 and b"Check only; reset disabled" in r.stdout, (r.stdout, r.stderr)
                    assert b"ACPI reset space=" in r.stdout and b"Checkpoint accepted by storage" in r.stdout, r.stdout
                    start = serial.stat().st_size
                    r = remote("reboot", extra=("-o", "ServerAliveInterval=3", "-o", "ServerAliveCountMax=3"))
                    assert r.returncode in (0, 255), (r.returncode, r.stdout, r.stderr)
                    restarted = prompt(start)
                    assert b"SSHTest" in restarted
                    for _ in range(20):
                        r = remote(f"grep sshd- diag/boot{boot}.log", extra=("-o", "StrictHostKeyChecking=yes",))
                        if r.returncode == 0:
                            break
                        time.sleep(.5)
                    assert r.returncode == 0, (r.stdout, r.stderr)
                    assert all(stage in r.stdout for stage in stages), r.stdout
                    r = remote(f"grep reboot-enter diag/boot{boot - 1}.log")
                    assert r.returncode == 0 and b"reason=reboot-enter" in r.stdout, (r.stdout, r.stderr)
                    r = remote(f"grep reboot-request diag/boot{boot - 1}.log")
                    assert r.returncode == 0 and b"[BDIAG] reboot-request" in r.stdout, (r.stdout, r.stderr)
                    # UART bytes still queued when the chipset resets can be
                    # lost. The pre-reset disk checkpoint is the evidence.
                    r = remote(f"grep REBOOT diag/boot{boot - 1}.log")
                    assert r.returncode == 0 and b"[REBOOT] ACPI reset space=" in r.stdout, (r.stdout, r.stderr)
                    assert b"fallback" not in r.stdout, r.stdout
                passed("two_remote_acpi_reboots_persisted_request_and_ssh_autostart")
            return
        if os.environ.get("OSITO_SSH_TEST_CPU_IDLE_ONLY"):
            r = remote("cpuidle status")
            assert r.returncode == 0 and b"CPU idle policy: HLT" in r.stdout, (r.stdout, r.stderr)
            assert r.stdout.count(b" APIC ") == 4, r.stdout
            passed("idle_caps_on_all_four_cpus")
            r = remote("console launch desktop")
            assert r.returncode == 0, (r.stdout, r.stderr)
            time.sleep(2)
            for policy in ("hlt", "mwait", "hlt"):
                r = remote("cpuidle " + policy)
                assert r.returncode == 0 and b"CPU idle:" in r.stdout, (r.stdout, r.stderr)
                time.sleep(.2)
                r = remote("cpuidle selftest")
                assert r.returncode == 0 and b"CPUs/completed/passed=4/4/4 PASS" in r.stdout, (r.stdout, r.stderr)
                r = remote("cpuidle status")
                assert r.returncode == 0, (r.stdout, r.stderr)
                rows = [line for line in r.stdout.splitlines() if b" APIC " in line]
                assert len(rows) == 4 and all(line.endswith(b"/0") for line in rows), r.stdout
                assert int(rows[0].split(b"=")[-1].split(b"/")[0]) > 0, r.stdout
            passed("bsp_ap_fault_fixups_and_ipi_wakeup")
            assert b"[SCHED] Idle task online" in serial.read_bytes()
            assert b"AP dispatch smoke test: OK" in serial.read_bytes()
            passed("scheduler_idle_and_ap_worker_dispatch")
        if os.environ.get("OSITO_SSH_TEST_THERMAL_ONLY") or os.environ.get("OSITO_SSH_TEST_CPU_IDLE_ONLY"):
            r = remote("thermal selftest")
            assert r.returncode == 0 and b"output unchanged PASS" in r.stdout, (r.stdout, r.stderr)
            passed("msr_fault_fixup_in_kernel")
            for _ in range(3):
                r = remote("thermal")
                assert r.returncode == 0 and b"Thermal diagnostics (read-only" in r.stdout, (r.stdout, r.stderr)
            passed("read_only_thermal_query_over_ssh")
            for _ in range(3):
                r = remote("thermal sample")
                assert r.returncode == 0 and b"Power sample (read-only" in r.stdout, (r.stdout, r.stderr)
                assert b"Sampling unavailable:" in r.stdout or b"Package RAPL average:" in r.stdout, r.stdout
            passed("read_only_power_sample_over_ssh")
            r = remote("echo AFTER-MSR-FAULT")
            assert r.returncode == 0 and b"AFTER-MSR-FAULT" in r.stdout
            assert b"!!! EXCEPTION" not in serial.read_bytes()
            passed("kernel_and_ssh_alive_after_msr_fault")
            return
        spec = importlib.util.spec_from_file_location("osito_screen",
                Path(__file__).resolve().parents[3] / "tools/osito-screen.py")
        screen = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(screen)
        screen_argv = ["ssh", "-F", "/dev/null", "-p", str(port), "-i", str(work / "client"),
                       "-oIdentitiesOnly=yes", "-oBatchMode=yes", "-oConnectTimeout=8",
                       "-oStrictHostKeyChecking=yes", "-oRekeyLimit=256K",
                       "-o", f"UserKnownHostsFile={out / 'known_hosts'}",
                       "-T", "-s", "osito@127.0.0.1", "osito-screen-v1"]
        if os.environ.get("OSITO_SSH_TEST_LAYERED") == "1":
            negative = os.environ.get("OSITO_SSH_TEST_LAYERED_NEGATIVE") == "1"
            local("desktop")
            if not negative:
                raw = local("win32-wm-test")
                (out / "window-model.log").write_bytes(raw)
                assert b"USER32 window-model test: PASS" in raw, raw[-6000:]
                passed("layered_window_model_and_input")
            for bits in (32, 64):
                raw = local(f"winexec layered{bits}.exe")
                (out / f"layered{bits}.log").write_bytes(raw)
                assert b"!!! EXCEPTION" not in raw, raw[-6000:]
                expected = b"LAYERED MISSING UpdateLayeredWindow" if negative else b"LAYERED CONTRACT PASS"
                assert expected in raw and b"LAYERED FAIL:" not in raw, raw[-6000:]
                passed(f"pe{bits}_layered_{'negative' if negative else 'contract'}")
                if negative:
                    continue
                start = serial.stat().st_size
                r = remote(f"console launch winexec layered{bits}.exe --visual")
                assert r.returncode == 0 and b"queued request " in r.stdout, r.stdout
                job = int(r.stdout.split(b"queued request ", 1)[1].split()[0])
                deadline = time.monotonic() + 60
                while b"LAYERED VISUAL READY" not in serial.read_bytes()[start:]:
                    assert b"LAYERED FAIL:" not in serial.read_bytes()[start:]
                    assert time.monotonic() < deadline, serial.read_bytes()[-6000:]
                    time.sleep(.1)
                time.sleep(.5)
                client = screen.sync.Client(screen_argv, timeout=90, version=b"OSCR0001", errors=screen.ERRORS)
                try:
                    info, rgb = screen.capture(client, chunk_size=4096)
                    client.close()
                except BaseException:
                    client.close(graceful=False)
                    raise
                screen.save_png(out / f"layered{bits}.png", info, rgb)
                def pixel(x, y):
                    offset = (y * info["width"] + x) * 3
                    return tuple(rgb[offset:offset + 3])
                for x, y, expected_color in (
                    (34, 54, (32, 64, 96)),
                    (48, 68, (255, 0, 0)), (80, 68, (16, 160, 48)),
                    (48, 92, (32, 64, 96)), (80, 92, (0, 0, 255)),
                    (148, 68, (144, 32, 48)), (180, 68, (24, 112, 72)),
                    (148, 92, (32, 64, 96)), (180, 92, (16, 32, 176)),
                    (248, 68, (32, 64, 96)), (280, 68, (0, 128, 0)),
                    (248, 92, (0, 0, 0)), (280, 92, (0, 0, 255))):
                    actual = pixel(x, y)
                    assert all(abs(a - b) <= 1 for a, b in zip(actual, expected_color)), (bits, x, y, actual, expected_color)
                passed(f"pe{bits}_layered_composited_pixels")
                r = remote(f"console stop {job}")
                assert r.returncode == 0, r.stdout
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    r = remote("console status")
                    if b"console: idle" in r.stdout and b"returned" in r.stdout:
                        break
                    time.sleep(.2)
                assert b"console: idle" in r.stdout, r.stdout
                passed(f"pe{bits}_layered_stop_cleanup")
            assert b"LAYERED-SSH-ALIVE" in remote("echo LAYERED-SSH-ALIVE").stdout
            return
        if os.environ.get("OSITO_SSH_TEST_REMOTE_IO") == "1":
            for bits in (32, 64):
                launch_start = serial.stat().st_size
                r = remote(f"console launch winexec io{bits}.exe")
                assert r.returncode == 0, (r.stdout, r.stderr)
                job = int(r.stdout.split(b"queued request ", 1)[1].split()[0])
                deadline = time.monotonic() + 90
                while b"REMOTE-IO START" not in serial.read_bytes()[launch_start:]:
                    assert time.monotonic() < deadline, serial.read_bytes()[-5000:]
                    time.sleep(.05)
                overlap = False
                for command, expected in (
                        ("console status", b"running"), ("uptime", b"up "),
                        ("usb-perf", b"[USB-PERF] op=read"),
                        ("dmesg | tail -n 8", b""),
                        ("echo IO-REMOTE > io-remote.txt", b"synced"),
                        ("cat io-remote.txt", b"IO-REMOTE")):
                    r = remote(command)
                    assert r.returncode == 0 and expected in r.stdout, (command, r.stdout, r.stderr)
                    if b"REMOTE-IO PASS" not in serial.read_bytes()[launch_start:]:
                        overlap = True
                assert overlap, "queries did not overlap the sustained read workload"
                passed(f"pe{bits}_ssh_during_sustained_usb_read")
                deadline = time.monotonic() + 600
                while b"REMOTE-IO PASS" not in serial.read_bytes()[launch_start:]:
                    raw = serial.read_bytes()[launch_start:]
                    assert b"REMOTE-IO FAIL" not in raw and b"!!! EXCEPTION" not in raw, raw[-5000:]
                    assert time.monotonic() < deadline, raw[-5000:]
                    time.sleep(.1)
                passed(f"pe{bits}_sustained_read_bytes_verified")
                r = remote(f"console stop {job}")
                assert r.returncode == 0 and b"stop requested" in r.stdout, (r.stdout, r.stderr)
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    r = remote("console status")
                    if r.returncode == 0 and b"console: idle" in r.stdout and b"returned" in r.stdout:
                        break
                    time.sleep(.2)
                assert r.returncode == 0 and b"console: idle" in r.stdout, r.stdout
                passed(f"pe{bits}_stop_after_sustained_read")
            return
        if os.environ.get("OSITO_SSH_TEST_REMOTE_MAINTENANCE") == "1":
            for bits in (32, 64):
                launch_start = serial.stat().st_size
                r = remote(f"console launch winexec maint{bits}.exe")
                assert r.returncode == 0, (r.stdout, r.stderr)
                job = int(r.stdout.split(b"queued request ", 1)[1].split()[0])
                deadline = time.monotonic() + 30
                while b"REMOTE-MAINTENANCE READY" not in serial.read_bytes()[launch_start:]:
                    assert time.monotonic() < deadline, serial.read_bytes()[-5000:]
                    time.sleep(.1)
                for command, expected in (
                    (f"echo SESSION-ONLY-{bits} > maint-log.txt", b"synced"),
                    ("cat < maint-log.txt", f"SESSION-ONLY-{bits}".encode()),
                    ("dmesg | tail -n 4 > maint-dmesg.txt", b"synced"),
                    ("cp maint-log.txt maint-copy.txt", b"cp: ok"),
                    ("head maint-copy.txt -n 1", f"SESSION-ONLY-{bits}".encode()),
                    ("rm maint-copy.txt", b"rm: ok"),
                    ("sshd status", b"listening"),
                    ("usb-perf", b"[USB-PERF] op=read"),
                    ("console status", b"running")):
                    r = remote(command)
                    assert r.returncode == 0 and expected in r.stdout, (command, r.stdout, r.stderr)
                assert f"SESSION-ONLY-{bits}".encode() not in serial.read_bytes()[launch_start:]
                passed(f"pe{bits}_maintenance_io_while_foreground_busy")
                if os.environ.get("OSITO_SSH_TEST_KPROF") == "1":
                    r = remote("kprof start")
                    assert r.returncode == 0 and b"[KPROF] Started" in r.stdout, (r.stdout, r.stderr)
                    time.sleep(2)
                    r = remote("kprof stop")
                    assert r.returncode == 0 and b"[KPROF] Stopped" in r.stdout, (r.stdout, r.stderr)
                    first = remote("kprof")
                    second = remote("kprof")
                    assert first.returncode == second.returncode == 0, (first.stderr, second.stderr)
                    assert first.stdout == second.stdout and b"address=0x" in first.stdout, first.stdout
                    (out / f"kprof{bits}.txt").write_bytes(first.stdout)
                    r = remote("kprof raw")
                    assert r.returncode == 0 and b"rip=0x" in r.stdout and b" cr3=0x" in r.stdout, (r.stdout, r.stderr)
                    (out / f"kprof{bits}-raw.txt").write_bytes(r.stdout)
                    ticks = [int(line.split(b" tick=", 1)[1].split()[0])
                             for line in r.stdout.splitlines() if b" tick=" in line]
                    assert ticks == sorted(set(ticks)) and len(ticks) >= 2, ticks
                    for command, expected in (("sched stats", b"Context switches:"),
                                              ("perf snapshot", b"[PERF]"),
                                              ("serial-stats", b"[SERIAL]")):
                        r = remote(command)
                        assert r.returncode == 0 and expected in r.stdout, (command, r.stdout, r.stderr)
                    for command in ("kprof unknown", "sched", "perf reset", "serial-stats extra"):
                        r = remote(command)
                        assert r.returncode == 126, (command, r.stdout, r.stderr)
                    assert b"[KPROF]" not in serial.read_bytes()[launch_start:]
                    passed(f"pe{bits}_hardware_profiler_while_foreground_busy")
                for command in ("kexec next.elf", "kupdate --file next.elf --no-kexec",
                                "reboot --check > blocked-reboot.txt"):
                    r = remote(command)
                    assert r.returncode == 126 and b"exclusive console access" in r.stdout, (command, r.stdout)
                r = remote(f"console stop {job + 1}")
                assert r.returncode == 126 and b"no matching" in r.stdout, r.stdout
                passed(f"pe{bits}_exclusive_operations_and_stale_stop_rejected")
                screen_client = screen.sync.Client(screen_argv, timeout=90, version=b"OSCR0001", errors=screen.ERRORS)
                try:
                    info, rgb = screen.capture(screen_client, chunk_size=4096)
                    screen_client.close()
                except BaseException:
                    screen_client.close(graceful=False)
                    raise
                screen.save_png(out / f"screen-maint{bits}.png", info, rgb)
                passed(f"pe{bits}_snapshot_with_runnable_workers")
                r = remote(f"console stop {job}")
                assert r.returncode == 0 and b"stop requested" in r.stdout, (r.stdout, r.stderr)
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    r = remote("console status")
                    if b"console: idle" in r.stdout and b"returned" in r.stdout:
                        break
                    time.sleep(.2)
                assert b"console: idle" in r.stdout, r.stdout
                raw = serial.read_bytes()[launch_start:]
                assert b"[WINEXEC-EXIT] delivered to main" in raw, raw[-5000:]
                assert b"[WINEXEC] stopped owned threads=4" in raw, raw[-5000:]
                assert b"!!! EXCEPTION" not in raw, raw[-5000:]
                r = remote(f"console stop {job}")
                assert r.returncode == 126, r.stdout
                assert b"REMOTE-MAINT-ALIVE" in remote("echo REMOTE-MAINT-ALIVE").stdout
                passed(f"pe{bits}_stop_unwinds_owner_and_preserves_ssh")
            return
        screen_client = screen.sync.Client(screen_argv, timeout=90, version=b"OSCR0001", errors=screen.ERRORS)
        try:
            info, rgb = screen.capture(screen_client)
            screen_client.close()
        except BaseException:
            screen_client.close(graceful=False)
            raise
        screen.save_png(out / "screen-console.png", info, rgb)
        qmp({"execute": "screendump", "arguments": {"filename": str(out / "screen-qmp.ppm")}})
        magic, geometry, maximum, reference = (out / "screen-qmp.ppm").read_bytes().split(b"\n", 3)
        assert magic == b"P6" and maximum == b"255"
        assert tuple(map(int, geometry.split())) == (info["width"], info["height"])
        assert rgb == reference, "remote GOP pixels differ from QEMU scanout"
        passed("screen_gop_pixels_match_qmp")
        r = remote("console status")
        assert r.returncode == 0 and b"console: idle" in r.stdout and b"0 none" in r.stdout, r.stdout
        partial_start = serial.stat().st_size
        for c in b"echo LOCAL-PARTIAL-741":
            uart.sendall(bytes([c])); time.sleep(.03)
        r = remote("console launch desktop")
        assert r.returncode == 126 and b"nothing queued" in r.stdout, (r.returncode, r.stdout)
        uart.sendall(b"\r")
        assert b"LOCAL-PARTIAL-741" in prompt(partial_start)
        r = remote("console launch desktop > launch-should-not-exist.txt")
        assert r.returncode == 126 and b"nothing queued" in r.stdout, r.stdout
        r = remote("console launch wlan connect")
        assert r.returncode == 126, r.stdout
        passed("console_launch_rejects_partial_input_redirects_and_interactive_prompts")
        launch_start = serial.stat().st_size
        r = remote("console launch desktop")
        assert r.returncode == 0 and b"queued request 1" in r.stdout, (r.returncode, r.stdout, r.stderr)
        raw = prompt(launch_start)
        assert b"[SSH console]" in raw and b"Desktop launched" in raw, raw
        r = remote("console status")
        assert r.returncode == 0 and b"console: idle" in r.stdout and b"1 returned" in r.stdout, r.stdout
        passed("console_launch_runs_on_local_prompt_and_reports_return")
        # Keep the real local lock across both SSH handshakes and snapshot.
        # A single four-packet ping can finish between those operations.
        busy_start = serial.stat().st_size
        busy_command = " | ".join(["ping 10.0.2.254"] * 4) + "\r"
        for c in busy_command.encode():
            uart.sendall(bytes([c])); time.sleep(.05)
        r = remote("reboot --check > busy-should-not-exist.txt")
        assert r.returncode == 126 and b"exclusive console access" in r.stdout, (r.returncode, r.stdout, r.stderr)
        assert b"osito> " not in serial.read_bytes()[busy_start:]
        passed("remote_exclusive_command_rejected_while_local_shell_busy")
        screen_client = screen.sync.Client(screen_argv, timeout=90, version=b"OSCR0001", errors=screen.ERRORS)
        try:
            assert len(screen_client.rpc(b"C")) == 72
            assert b"osito> " not in serial.read_bytes()[busy_start:]
            screen_client.rpc(b"X")
            screen_client.close()
        except BaseException:
            screen_client.close(graceful=False)
            raise
        prompt(busy_start)
        passed("screen_while_local_shell_busy")
        # Reuse a foreground pipeline to retain its local capture buffers.
        # Remote queries must neither acquire that lock nor borrow those I/O.
        for queries in (("mem", "ps", "dmesg | grep SSHD | tail -n 4"),
                        ("ifconfig", "route", "echo REMOTE-BUSY-731 | cat"),
                        ("echo REMOTE-FILE-731 > concurrent.txt", "cat < concurrent.txt",
                         "echo allowed | cat >> concurrent.txt"),
                        ("console status", "console launch desktop", "mem")):
            busy_start = serial.stat().st_size
            for c in busy_command.encode():
                uart.sendall(bytes([c])); time.sleep(.05)
            for cmd in queries:
                r = remote(cmd)
                denied = cmd.startswith("console launch")
                assert r.returncode == (126 if denied else 0), (cmd, r.returncode, r.stdout, r.stderr)
                assert r.stdout and (not denied or b"nothing queued" in r.stdout), (cmd, r.stdout)
                assert b"osito> " not in serial.read_bytes()[busy_start:], "local pipeline ended during concurrency test"
            prompt(busy_start)
        assert b"REMOTE-BUSY-731" not in serial.read_bytes()
        assert b"REMOTE-FILE-731" not in serial.read_bytes()
        passed("diagnostics_with_pipelines_while_local_shell_busy")
        passed("busy_session_files_and_redirects_are_independent")
        passed("concurrent_local_and_remote_output_isolation")
        r = remote("echo PIPE-OK | cat")
        assert r.returncode == 0 and b"PIPE-OK" in r.stdout, (r.stdout, r.stderr)
        r = remote("echo saved-over-ssh > remote.txt")
        assert r.returncode == 0, r.stderr
        r = remote("cat remote.txt")
        assert r.returncode == 0 and b"saved-over-ssh" in r.stdout, (r.stdout, r.stderr)
        passed("pipes_and_file_redirection")
        spec = importlib.util.spec_from_file_location("osito_sync",
                Path(__file__).resolve().parents[3] / "tools/osito-sync.py")
        sync_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(sync_module)
        sync_argv = ["ssh", "-F", "/dev/null", "-p", str(port), "-i", str(work / "client"),
                    "-oIdentitiesOnly=yes", "-oBatchMode=yes", "-oConnectTimeout=8",
                    "-oStrictHostKeyChecking=yes", "-oRekeyLimit=256K",
                    "-o", f"UserKnownHostsFile={out / 'known_hosts'}",
                    "-T", "-s", "osito@127.0.0.1", "osito-sync-v1"]
        sync_client = sync_module.Client(sync_argv, timeout=90)
        try:
            assert sync_client.stat("/busy-should-not-exist.txt") is None
            assert sync_client.stat("/launch-should-not-exist.txt") is None
            passed("busy_remote_command_not_queued_or_executed")
            source = out / "sync-source"
            (source / "empty").mkdir(parents=True)
            payload = bytes(range(256)) * 4097 + b"\x00\r\n"
            (source / "binary.dat").write_bytes(payload)
            (source / "space name.txt").write_bytes(b"empty directories and names\n")
            task = sync_module.Sync(sync_client, quiet=True)
            task.push(source, "/sync-test", recursive=True)
            task.pull("/sync-test", out / "sync-download", recursive=True)
            assert (out / "sync-download/binary.dat").read_bytes() == payload
            assert (out / "sync-download/empty").is_dir()
            passed("sync_binary_tree_rekey_over_1mib")
            task = sync_module.Sync(sync_client, quiet=True)
            task.push(source, "/sync-test", recursive=True)
            assert task.stats["files_skipped"] == 2 and task.stats["data_bytes"] == 0
            passed("sync_unchanged_skipped")
            payload = payload[:33000] + b"X" + payload[33001:]
            (source / "binary.dat").write_bytes(payload)
            task = sync_module.Sync(sync_client, quiet=True)
            task.push(source / "binary.dat", "/sync-test/binary.dat")
            assert task.stats["data_bytes"] == sync_module.BLOCK, task.stats
            assert task.stats["reused_bytes"] == len(payload) - sync_module.BLOCK
            task.pull("/sync-test/binary.dat", out / "sync-download/binary.dat")
            assert (out / "sync-download/binary.dat").read_bytes() == payload
            passed("sync_one_block_delta_and_atomic_replace")
            sync_client.rpc(b"B" + (3).to_bytes(8, "big") + b"/sync-test/binary.dat")
            sync_client.rpc(b"Wbad")
            sync_client.process.kill()
        finally:
            sync_client.close(graceful=False)
        r = remote("echo after-sync", extra=("-o", "StrictHostKeyChecking=yes",))
        assert r.returncode == 0 and b"after-sync" in r.stdout, (r.stdout, r.stderr)
        sync_client = sync_module.Client(sync_argv)
        try:
            assert not any(name.startswith(".osync-") for name, _ in sync_client.listing("/sync-test"))
            task = sync_module.Sync(sync_client, quiet=True)
            task.pull("/sync-test/binary.dat", out / "sync-after-disconnect.bin")
            assert (out / "sync-after-disconnect.bin").read_bytes() == payload
            sync_client.close()
        except BaseException:
            sync_client.close(graceful=False)
            raise
        passed("sync_disconnect_preserves_old_file_and_cleans_stage")
        r = remote("desktop")
        assert r.returncode == 126 and b"does not support" in r.stdout
        passed("reject_local_only_program")
        r = remote(data=b"echo interactive-ok\rexit\r", extra=("-tt",))
        assert r.returncode == 0 and b"interactive-ok" in r.stdout, (r.stdout, r.stderr)
        assert b"LOCAL-OK" in local("echo LOCAL-OK")
        passed("interactive_and_local_survives")
        assert b"stopping" in local("sshd stop")
        time.sleep(.5)
        assert b"sshd: stopped" in local("sshd status")
        assert b"listening on TCP 22" in local("sshd start")
        r = remote("echo restart-ok", extra=("-o", "StrictHostKeyChecking=yes",))
        assert r.returncode == 0 and b"restart-ok" in r.stdout, r.stderr
        passed("stop_restart_same_identity")
        r = remote("sshd autostart on")
        assert r.returncode == 0 and b"autostart policy saved" in r.stdout, (r.stdout, r.stderr)
        passed("persist_ssh_autostart_policy")
        assert b"stopping" in local("sshd stop")
        time.sleep(.5)
        assert b"sshd: stopped" in local("sshd status")
        local("sync")
        # Cold boot the same disks; this gate tests persistence, not ACPI reset.
        stop.set()
        uart.close()
        reader.join(timeout=2)
        qmp({"execute": "quit"})
        process.wait(timeout=10)
        serial.rename(out / "serial-first-boot.log")
        stop.clear()
        with (out / "qemu.log").open("ab") as log:
            process = subprocess.Popen(args, stdout=log, stderr=log)
        assert b"SSHTest" in prompt()
        uart = socket.socket(socket.AF_UNIX); uart.settimeout(.2)
        uart.connect(str(work / "serial.sock"))
        reader = threading.Thread(target=drain); reader.start()
        for _ in range(20):
            r = remote("echo reboot-ok", extra=("-o", "StrictHostKeyChecking=yes",))
            if r.returncode == 0:
                break
            time.sleep(.5)
        assert r.returncode == 0 and b"reboot-ok" in r.stdout, r.stderr
        passed("autostart_after_cold_boot_same_host_key")
        sync_client = sync_module.Client(sync_argv)
        try:
            task = sync_module.Sync(sync_client, quiet=True)
            task.pull("/sync-test/binary.dat", out / "sync-after-reboot.bin")
            assert (out / "sync-after-reboot.bin").read_bytes() == payload
            sync_client.close()
        except BaseException:
            sync_client.close(graceful=False)
            raise
        passed("sync_contents_survive_cold_boot")
        start = serial.stat().st_size
        r = remote("kexec next.elf", extra=("-o", "ServerAliveInterval=3", "-o", "ServerAliveCountMax=3"))
        assert r.returncode == 255, (r.returncode, r.stdout, r.stderr)
        reboot = prompt(start)
        assert b"[KEXEC] reserved transient data" in reboot and b"SSHTest" in reboot, reboot[-2000:]
        for _ in range(20):
            r = remote("cat remote.txt", extra=("-o", "StrictHostKeyChecking=yes",))
            if r.returncode == 0:
                break
            time.sleep(.5)
        assert r.returncode == 0 and b"saved-over-ssh" in r.stdout, (r.returncode, r.stdout, r.stderr)
        passed("remote_kexec_automatic_reconnect_same_identity_and_files")
        assert b"Desktop launched" in local("desktop")
        time.sleep(.5)
        screen_client = screen.sync.Client(screen_argv, timeout=90, version=b"OSCR0001", errors=screen.ERRORS)
        try:
            desktop, desktop_rgb = screen.capture(screen_client)
            screen_client.close()
        except BaseException:
            screen_client.close(graceful=False)
            raise
        assert (desktop["width"], desktop["height"]) == (info["width"], info["height"])
        assert desktop_rgb != rgb and len(set(desktop_rgb)) > 16
        screen.save_png(out / "screen-desktop.png", desktop, desktop_rgb)
        qmp({"execute": "screendump", "arguments": {"filename": str(out / "screen-desktop-qmp.ppm")}})
        magic, geometry, maximum, reference = (out / "screen-desktop-qmp.ppm").read_bytes().split(b"\n", 3)
        assert magic == b"P6" and maximum == b"255"
        assert tuple(map(int, geometry.split())) == (desktop["width"], desktop["height"])
        # Uptime/FPS counters can change while the frozen snapshot is transferred.
        differences = sum(a != b for a, b in zip(desktop_rgb, reference))
        assert len(reference) == len(desktop_rgb) and differences < len(reference) // 100
        results["desktop_changed_bytes"] = differences
        passed("screen_full_desktop_after_terminal_redirection")
    finally:
        stop.set()
        if uart: uart.close()
        if reader: reader.join(timeout=2)
        if process.poll() is None:
            try: qmp({"execute": "quit"})
            except Exception: process.terminate()
        try: process.wait(timeout=10)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__": main()
