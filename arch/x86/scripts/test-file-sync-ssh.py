#!/usr/bin/env python3
"""Exercise binary streaming/rekey/cleanup through the production TinySSH port."""
import importlib.util
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("osito_sync", repo / "tools/osito-sync.py")
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)


def main():
    build = Path(sys.argv[1]).resolve()
    results = {}
    with tempfile.TemporaryDirectory(prefix="osito-sync-ssh-") as temporary:
        root = Path(temporary)
        remote = root / "remote"
        remote.mkdir()
        key = root / "client"
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(key)], check=True)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        with (build / "sync-ssh.log").open("wb") as log:
            server = subprocess.Popen([str(build / "ssh-host"), str(port), str(root / "host.key"),
                                       str(key) + ".pub"], stdout=subprocess.PIPE, stderr=log,
                                      env=dict(os.environ, OSITO_SYNC_ROOT=str(remote)))
            client = None
            try:
                with selectors.DefaultSelector() as sel:
                    sel.register(server.stdout, selectors.EVENT_READ)
                    assert sel.select(30) and server.stdout.readline() == b"READY\n"
                ssh = ["ssh", "-F", "/dev/null", "-p", str(port), "-i", str(key),
                       "-oIdentitiesOnly=yes", "-oBatchMode=yes", "-oConnectTimeout=5",
                       "-o", f"UserKnownHostsFile={root / 'known_hosts'}"]
                trusted = subprocess.run(ssh + ["-oStrictHostKeyChecking=accept-new",
                    "osito@127.0.0.1", "echo before-sync"], capture_output=True, timeout=30)
                assert trusted.returncode == 0 and trusted.stdout == b"before-sync\n"
                binary = ssh + ["-oStrictHostKeyChecking=yes", "-oRekeyLimit=128K",
                                "-T", "-s", "osito@127.0.0.1", "osito-sync-v1"]
                client = sync.Client(binary, timeout=60)
                task = sync.Sync(client, quiet=True)
                original = bytes(range(256)) * 4097 + b"\x00\r\n"
                source = root / "source.bin"
                source.write_bytes(original)
                task.push(source, "/large.bin")
                task.pull("/large.bin", root / "download.bin")
                assert (root / "download.bin").read_bytes() == original
                assert task.stats["data_bytes"] == 2 * len(original)
                results["binary_over_1mib_fragmented_rekey_roundtrip"] = True
                same = sync.Sync(client, quiet=True)
                same.push(source, "/large.bin")
                assert same.stats["files_skipped"] == 1 and same.stats["data_bytes"] == 0
                results["unchanged_skipped"] = True
                # Leave an unpublished upload then close TCP without Q/EOF.
                client.rpc(b"B" + (3).to_bytes(8, "big") + b"/large.bin")
                client.rpc(b"Wnew")
                client.process.kill()
                client.close(graceful=False); client = None
                after = subprocess.run(ssh + ["-oStrictHostKeyChecking=yes",
                    "osito@127.0.0.1", "echo after-sync"], capture_output=True, timeout=30)
                assert after.returncode == 0 and after.stdout == b"after-sync\n"
                assert (remote / "large.bin").read_bytes() == original
                assert not list(remote.glob(".osync-*"))
                results["disconnect_cleanup_and_console_reconnect"] = True
                invalid = subprocess.run(ssh + ["-oStrictHostKeyChecking=yes", "-T", "-s",
                    "osito@127.0.0.1", "unsupported-subsystem"], capture_output=True, timeout=30)
                assert invalid.returncode != 0
                results["unknown_subsystem_rejected"] = True
            finally:
                if client:
                    client.close(graceful=False)
                server.terminate()
                server.wait(timeout=5)
                server.stdout.close()
                (build / "sync-ssh-results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
