#!/usr/bin/env python3
"""Interop tests use the actual OpenSSH client, not a simulated handshake."""
import argparse
import json
import os
from pathlib import Path
import re
import selectors
import socket
import subprocess
import tempfile
import shutil
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("build", type=Path)
    args = parser.parse_args()
    root = args.build.resolve()
    root.mkdir(parents=True, exist_ok=True)
    private_dir = tempfile.TemporaryDirectory(prefix=".osito-ssh-test-", dir=Path.home())
    private = Path(private_dir.name)
    for name in ("client", "wrong"):
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(private / name)], check=True)
        shutil.copyfile(private / (name + ".pub"), root / (name + ".pub"))
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    results = {}
    server = None
    log = (root / "server.log").open("ab")

    def start(fault=None):
        env = os.environ.copy()
        env.pop("OSITO_SSH_TEST_FAULT", None)
        if fault:
            env["OSITO_SSH_TEST_FAULT"] = fault
        proc = subprocess.Popen([str(root / "ssh-host"), str(port), str(root / "host.key"), str(root / "client.pub")], stdout=subprocess.PIPE, stderr=log, env=env)
        sel = selectors.DefaultSelector()
        sel.register(proc.stdout, selectors.EVENT_READ)
        try:
            assert sel.select(30), "server startup timeout"
            assert proc.stdout.readline() == b"READY\n", "server startup failed"
        finally:
            sel.close()
        return proc

    def ssh(command=None, *, user="osito", key="client", data=None, options=()):
        argv = ["ssh", "-F", "/dev/null", "-p", str(port), "-i", str(private / key),
                "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
                "-o", "StrictHostKeyChecking=accept-new", "-o", f"UserKnownHostsFile={root / 'known_hosts'}",
                *options, f"{user}@127.0.0.1"]
        if command is not None:
            argv.append(command)
        return subprocess.run(argv, input=data, capture_output=True, timeout=60)

    try:
        server = start()
        r = ssh("echo encrypted-fragmented")
        assert r.returncode == 0 and r.stdout == b"encrypted-fragmented\n", (r.returncode, r.stdout, r.stderr)
        results["fragmented_encrypted_exec"] = True
        for label, kw in (("wrong_key", {"key": "wrong"}), ("wrong_user", {"user": "root"}),
                          ("no_auth", {"options": ("-o", "PreferredAuthentications=none")})):
            r = ssh("echo MUST-NOT-RUN", **kw)
            assert r.returncode == 255 and b"MUST-NOT-RUN" not in r.stdout, (label, r.stderr)
            results[label] = True
        for payload in (b"not-ssh\r\n", b"SSH-2.0-malformed\r\n" + b"\xff" * 32):
            with socket.create_connection(("127.0.0.1", port), 5) as s:
                s.sendall(payload)
        r = ssh("echo survives-malformed")
        assert r.returncode == 0 and r.stdout == b"survives-malformed\n", r.stderr
        results["malformed_then_reconnect"] = True
        r = ssh("large", options=("-o", "RekeyLimit=16K"))
        assert r.returncode == 0 and r.stdout == b"x" * 131072, (len(r.stdout), r.stderr)
        results["rekey_flow_control"] = True
        r = ssh("large", options=("-o", "KexAlgorithms=curve25519-sha256",))
        assert r.returncode == 0 and len(r.stdout) == 131072, r.stderr
        results["curve25519"] = True
        r = ssh(data=b"echo session-one\recho session-two\rexit\r", options=("-tt",))
        assert r.returncode == 0 and b"session-one\nosito>" in r.stdout and b"session-two\nosito>" in r.stdout, (r.stdout, r.stderr)
        results["interactive_console"] = True
        r = ssh("unsupported")
        assert r.returncode == 127, r.stderr
        results["exit_status"] = True
        r = ssh("overflow")
        assert r.returncode == 1 and b"output limit exceeded" in r.stdout, (r.returncode, len(r.stdout), r.stderr)
        results["bounded_output"] = True
        faults = (
            ("send-timeout", "banner-send", "deadline"),
            ("receive-timeout", "banner-receive", "deadline"),
            ("send-closed", "banner-send", "write-closed"),
            ("receive-eof", "banner-receive", "read-closed"),
            ("cancelled", "banner-send", "cancelled"),
            ("entropy", None, "entropy"),
        )
        for fault, phase, reason in faults:
            server.terminate(); server.wait(timeout=5)
            offset = (root / "server.log").stat().st_size
            server = start(fault)
            r = ssh("echo MUST-NOT-RUN")
            assert r.returncode == 255 and b"MUST-NOT-RUN" not in r.stdout, (fault, r.stderr)
            r = ssh("echo recovered-after-failure")
            assert r.returncode == 0 and r.stdout == b"recovered-after-failure\n", (fault, r.stderr)
            trace = (root / "server.log").read_bytes()[offset:]
            failures = re.findall(rb"session failed phase=([a-z-]+) reason=([a-z-]+) rx=(\d+) tx=(\d+)", trace)
            assert len(failures) == 1, (fault, trace)
            observed_phase, observed_reason, rx, tx = failures[0]
            assert phase is None or observed_phase.decode() == phase, (fault, failures)
            assert observed_reason.decode() == reason, (fault, failures)
            if phase == "banner-send":
                assert int(rx) == int(tx) == 0, (fault, failures)
            elif phase == "banner-receive":
                assert int(rx) == 0 and int(tx) > 0, (fault, failures)
            assert b"MUST-NOT-RUN" not in trace and b"recovered-after-failure" not in trace
            results[f"failure_diagnostic_{fault}"] = True
        saved = (root / "host.key").read_bytes()
        server.terminate(); server.wait(timeout=5); server = start()
        r = ssh("echo persisted", options=("-o", "StrictHostKeyChecking=yes",))
        assert r.returncode == 0 and (root / "host.key").read_bytes() == saved, r.stderr
        results["persistent_host_identity"] = True
        print(json.dumps(results, indent=2))
    finally:
        if server and server.poll() is None:
            server.terminate()
            server.wait(timeout=5)
        log.close()
        private_dir.cleanup()
        (root / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
