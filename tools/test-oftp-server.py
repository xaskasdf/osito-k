#!/usr/bin/env python3
"""Host OFTP serving checks, including Windows path syntax on either host OS."""
import importlib.util
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def main():
    script = Path(__file__).with_name("oftp-server.py")
    spec = importlib.util.spec_from_file_location("oftp", script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for name in ("", ".", "..", "../secret", "..\\secret", "C:\\secret",
                 "C:secret", "kernel.elf:secret", "\\secret", "/secret", "a\x00b"):
        assert not module.safe_filename(name), name
    assert module.safe_filename("kernel.elf")
    with tempfile.TemporaryDirectory(prefix="osito-oftp-test-") as tmp:
        root = Path(tmp) / "serve"
        root.mkdir()
        content = bytes(range(256)) * 128
        (root / "kernel.elf").write_bytes(content)
        outside = Path(tmp) / "secret"
        outside.write_bytes(b"not served")
        symlink = False
        try:
            (root / "escape").symlink_to(outside)
            symlink = True
        except OSError:
            pass
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        server = subprocess.Popen([sys.executable, str(script), "127.0.0.1", str(port),
                                   str(root), "--read-only"], stdout=subprocess.DEVNULL)
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.settimeout(.2)
                client.connect(("127.0.0.1", port))
                for _ in range(50):
                    assert server.poll() is None, "server terminated"
                    try:
                        client.send(b"OFTQmissing")
                        assert client.recv(2048).startswith(b"OFTE")
                        break
                    except (socket.timeout, ConnectionRefusedError, ConnectionResetError):
                        time.sleep(.05)
                else:
                    raise TimeoutError("server did not start")
                client.settimeout(3)
                client.send(b"OFTQkernel.elf" + bytes(50))
                received = bytearray()
                while True:
                    packet = client.recv(2048)
                    assert packet[:4] == b"OFTD", packet[:20]
                    total, offset, count = struct.unpack(">III", packet[4:16])
                    assert total == len(content) and offset == len(received)
                    assert len(packet) == count + 16
                    if not count:
                        break
                    received.extend(packet[16:])
                assert received == content
                client.send(b"OFTU" + struct.pack(">I", 1) + b"unauthorized")
                assert client.recv(2048) == b"OFTEread only"
                names = ["../secret", "..\\secret", "C:\\secret", "C:secret",
                         "kernel.elf:secret", "\\secret", "/secret", "a\x00b"]
                if symlink:
                    names.append("escape")
                for name in names:
                    client.send(b"OFTQ" + name.encode())
                    assert client.recv(2048) == b"OFTEnot found", name
                assert not (root / "uploads").exists()
                print("PASS OFTP exact transfer, read-only mode, path confinement")
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == "__main__":
    main()
