#!/usr/bin/env python3
"""Real OpenSSH screen subsystem tests with the production protocol engine."""
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
spec = importlib.util.spec_from_file_location("screen", repo / "tools/osito-screen.py")
screen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(screen)


def main():
    build = Path(sys.argv[1]).resolve()
    results = {}
    with tempfile.TemporaryDirectory(prefix="osito-screen-ssh-") as tmp:
        root = Path(tmp)
        remote = root / "remote"
        remote.mkdir()
        key = root / "client"
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(key)], check=True)
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
        with (build / "screen-ssh.log").open("wb") as log:
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
                trust = subprocess.run(ssh + ["-oStrictHostKeyChecking=accept-new",
                    "osito@127.0.0.1", "echo ready"], capture_output=True, timeout=30)
                assert trust.returncode == 0 and trust.stdout == b"ready\n"
                binary = ssh + ["-oStrictHostKeyChecking=yes", "-oRekeyLimit=128K",
                    "-T", "-s", "osito@127.0.0.1", "osito-screen-v1"]
                client = screen.sync.Client(binary, version=b"OSCR0001", errors=screen.ERRORS)
                for _ in range(2):
                    info, rgb = screen.capture(client)
                    assert (info["width"], info["height"]) == (801, 601)
                    for y in range(601):
                        for x in range(801):
                            offset = (y * 801 + x) * 3
                            assert rgb[offset:offset + 3] == bytes((x * 3 & 255, y * 5 & 255, (x ^ y) & 255))
                screen.save_png(build / "screen-reference.png", info, rgb, force=True)
                results["rgb_pitch_padding_over_1mib_rekey_repeat"] = True
                assert len(client.rpc(b"C")) == 72
                client.process.kill(); client.close(graceful=False); client = None
                client = screen.sync.Client(binary, version=b"OSCR0001", errors=screen.ERRORS)
                info, rgb = screen.capture(client)
                client.close(); client = None
                results["disconnect_releases_snapshot"] = True
                for mode in ("-tt",):
                    result = subprocess.run(ssh + ["-oStrictHostKeyChecking=yes", mode, "-s",
                        "osito@127.0.0.1", "osito-screen-v1"], capture_output=True, timeout=30)
                    assert result.returncode != 0
                results["pty_rejected"] = True
                result = subprocess.run(ssh + ["-oStrictHostKeyChecking=yes",
                    "osito@127.0.0.1", "echo after-screen"], capture_output=True, timeout=30)
                assert result.returncode == 0 and result.stdout == b"after-screen\n"
                results["console_reconnect"] = True
                result = subprocess.run([sys.executable, str(repo / "tools/osito-screen.py"),
                    "--host", "osito@127.0.0.1", "--port", str(port), "--ssh-config", "/dev/null",
                    "-i", str(key), "--known-hosts", str(root / "known_hosts"),
                    "--force", str(build / "screen-cli.png")], capture_output=True, timeout=90)
                assert result.returncode == 0, result.stderr
                assert json.loads(result.stdout)["sha256"] == info["sha256"]
                results["cli_png"] = True
            finally:
                if client: client.close(graceful=False)
                server.terminate(); server.wait(timeout=5); server.stdout.close()
                (build / "screen-results.json").write_text(json.dumps(results, indent=2) + "\n")
        assert b"FAIL" not in (build / "screen-ssh.log").read_bytes()
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
