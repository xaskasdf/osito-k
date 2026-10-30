#!/usr/bin/env python3
"""Incremental binary file/tree copies over Osito-K's authenticated SSH service."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time

BLOCK = 32768
FRAME = BLOCK + 32
ERRORS = {1: "not found", 2: "end of directory", 3: "invalid request/path/type",
          4: "filesystem I/O failed", 5: "file/directory changed during transfer",
          6: "file is busy", 7: "SHA-256 verification failed"}


class SyncError(Exception):
    def __init__(self, message, status=None):
        super().__init__(message)
        self.status = status


def remote_path(value):
    if value in ("", ".", "/"):
        return "/"
    value = value.rstrip("/")
    for component in value.lstrip("/").split("/"):
        valid_name(component)
    path = "/" + value.lstrip("/")
    if len(path.encode("utf-8")) >= 260:
        raise SyncError("remote path exceeds OsitoFS3's 259-byte limit")
    return path


def valid_name(name):
    # A portable namespace avoids Windows ADS, reserved device names and aliases.
    if (not name or name in (".", "..") or name[-1:] in (".", " ") or
            any(ord(c) < 32 or ord(c) == 127 or c in '/\\:<>"|?*' for c in name) or
            len(name.encode("utf-8")) > 255 or
            name.split(".")[0].upper() in
            {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)),
             *(f"LPT{i}" for i in range(1, 10))}):
        raise SyncError(f"unsafe or non-portable filename: {name!r}")
    return name


def local_path(path):
    path = Path(os.path.abspath(path))
    for part in (path, *path.parents):
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise SyncError(f"symlink/reparse point is not supported: {part}")
    return path


def snapshot(path):
    try:
        s = local_path(path).lstat()
    except FileNotFoundError:
        return None
    if not stat.S_ISREG(s.st_mode):
        raise SyncError(f"not a regular file: {path}")
    return s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns, s.st_mode


def open_regular(path):
    local_path(path)
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0))
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        os.close(fd)
        raise SyncError(f"not a regular file: {path}")
    return os.fdopen(fd, "rb")


def digest_file(stream):
    stream.seek(0)
    h = hashlib.sha256()
    while data := stream.read(BLOCK):
        h.update(data)
    return h.digest()


class Client:
    def __init__(self, command, timeout=60, *, version=b"OSYNC001", errors=ERRORS):
        self.errors = errors
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, bufsize=0)
        self.timeout = timeout
        self.deadline = None
        self.stopped = threading.Event()
        self.timed_out = False
        self.watchdog = threading.Thread(target=self._watch, daemon=True)
        self.watchdog.start()
        try:
            if self.rpc(b"V") != version + struct.pack(">I", BLOCK):
                raise SyncError("unsupported osito-sync protocol version")
        except BaseException:
            self.close(graceful=False)
            raise

    def _watch(self):
        while not self.stopped.wait(0.1):
            if self.deadline is not None and time.monotonic() >= self.deadline:
                self.timed_out = True
                self.process.kill()
                return

    def _read(self, count):
        data = bytearray()
        while len(data) < count:
            chunk = self.process.stdout.read(count - len(data))
            if not chunk:
                reason = "transfer timed out" if self.timed_out else "SSH channel closed unexpectedly"
                raise SyncError(reason)
            data.extend(chunk)
        return bytes(data)

    def rpc(self, request):
        if not 0 < len(request) <= FRAME:
            raise SyncError("request exceeds negotiated frame size")
        self.deadline = time.monotonic() + self.timeout
        try:
            packet = memoryview(struct.pack(">I", len(request)) + request)
            while packet:
                sent = self.process.stdin.write(packet)
                if not sent:
                    raise SyncError("SSH input closed")
                packet = packet[sent:]
            n, = struct.unpack(">I", self._read(4))
            if not 1 <= n <= FRAME:
                raise SyncError("invalid response size")
            reply = self._read(n)
            if reply[0]:
                raise SyncError(self.errors.get(reply[0], "unknown server error"), reply[0])
            return reply[1:]
        except (BrokenPipeError, OSError) as e:
            raise SyncError(f"SSH transport failed: {e}") from e
        finally:
            self.deadline = None

    def stat(self, path):
        try:
            data = self.rpc(b"S" + remote_path(path).encode())
        except SyncError as e:
            if e.status == 1:
                return None
            raise
        if len(data) != 9 or data[0] not in (1, 2):
            raise SyncError("invalid stat response")
        return data[0], struct.unpack(">Q", data[1:])[0]

    def listing(self, path):
        self.rpc(b"L" + remote_path(path).encode())
        entries, names = [], set()
        while True:
            try:
                data = self.rpc(b"N")
            except SyncError as e:
                if e.status == 2:
                    break
                raise
            if len(data) < 2 or data[0] not in (1, 2):
                raise SyncError("invalid directory entry")
            name = valid_name(data[1:].decode("utf-8", errors="strict"))
            if name.casefold() in names:
                raise SyncError(f"duplicate/case-colliding entry: {name!r}")
            names.add(name.casefold())
            entries.append((name, data[0]))
        return sorted(entries)

    def open(self, path):
        data = self.rpc(b"O" + remote_path(path).encode())
        if len(data) != 8:
            raise SyncError("invalid open response")
        return struct.unpack(">Q", data)[0]

    def block(self, op, offset, count):
        data = self.rpc(op + struct.pack(">QI", offset, count))
        expected = 32 if op == b"H" else count if op == b"R" else 0
        if len(data) != expected:
            raise SyncError("invalid block response")
        return data

    def close(self, graceful=True):
        try:
            if graceful and self.process.poll() is None:
                self.rpc(b"Q")
            if self.process.stdin:
                self.process.stdin.close()
            try:
                code = self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                code = self.process.wait()
            if graceful and code:
                raise SyncError(f"SSH exited with status {code}")
        finally:
            self.stopped.set()
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.process.stdout.close()
            if self.process.stdin and not self.process.stdin.closed:
                self.process.stdin.close()
            self.watchdog.join(timeout=2)


class Sync:
    def __init__(self, client, dry_run=False, quiet=False):
        self.client, self.dry_run, self.quiet = client, dry_run, quiet
        self.stats = dict(files_copied=0, files_skipped=0, data_bytes=0,
                          reused_bytes=0, directories_created=0, dry_run=dry_run)

    def report(self, action, path):
        if not self.quiet:
            print(f"{'would ' if self.dry_run else ''}{action}: {path}", flush=True)

    def remote_mkdirs(self, path):
        current = ""
        for part in remote_path(path).strip("/").split("/"):
            if not part:
                continue
            current += "/" + part
            info = self.client.stat(current)
            if info and info[0] != 2:
                raise SyncError(f"not a directory: {current}")
            if not info:
                self.report("mkdir", current)
                if not self.dry_run:
                    self.client.rpc(b"M" + current.encode())
                self.stats["directories_created"] += 1

    def push_file(self, source, destination):
        source = local_path(source)
        before = snapshot(source)
        if before is None:
            raise SyncError(f"missing source: {source}")
        size = before[2]
        destination = remote_path(destination)
        info = self.client.stat(destination)
        if info and info[0] != 1:
            raise SyncError(f"not a regular destination: {destination}")
        opened = False
        with open_regular(source) as stream, tempfile.SpooledTemporaryFile(max_size=1048576) as matches:
            remote_size = self.client.open(destination) if info else 0
            opened = bool(info)
            same = bool(info) and size == remote_size
            expected = hashlib.sha256()
            try:
                for offset in range(0, size, BLOCK):
                    count = min(BLOCK, size - offset)
                    data = stream.read(count)
                    if len(data) != count:
                        raise SyncError("source changed during scan")
                    expected.update(data)
                    match = (opened and offset + count <= remote_size and
                             hashlib.sha256(data).digest() == self.client.block(b"H", offset, count))
                    matches.write(bytes([bool(match)]))
                    same = same and match
                if snapshot(source) != before:
                    raise SyncError("source changed during scan")
                if same:
                    self.stats["files_skipped"] += 1
                    self.report("skip", destination)
                    return
                self.report("copy", destination)
                if not self.dry_run:
                    self.remote_mkdirs(destination.rsplit("/", 1)[0])
                    self.client.rpc(b"B" + struct.pack(">Q", size) + destination.encode())
                stream.seek(0); matches.seek(0)
                for offset in range(0, size, BLOCK):
                    count = min(BLOCK, size - offset)
                    data = stream.read(count)
                    if len(data) != count:
                        raise SyncError("source changed during transfer")
                    match = matches.read(1) == b"\x01"
                    self.stats["reused_bytes" if match else "data_bytes"] += count
                    if not self.dry_run:
                        if match:
                            self.client.block(b"C", offset, count)
                        else:
                            self.client.rpc(b"W" + data)
                if snapshot(source) != before:
                    raise SyncError("source changed during transfer")
                if opened:
                    self.client.rpc(b"X"); opened = False
                if not self.dry_run:
                    self.client.rpc(b"F" + expected.digest())
                self.stats["files_copied"] += 1
            finally:
                if opened:
                    self.client.rpc(b"X")

    def pull_file(self, source, destination):
        destination = local_path(destination)
        before = snapshot(destination)
        size = self.client.open(source)
        temporary = None
        opened = True
        basis = open_regular(destination) if before else None
        try:
            with contextlib.ExitStack() as stack:
                if basis:
                    stack.enter_context(basis)
                signatures = stack.enter_context(tempfile.SpooledTemporaryFile(max_size=1048576))
                same = before is not None and before[2] == size
                for offset in range(0, size, BLOCK):
                    count = min(BLOCK, size - offset)
                    signature = self.client.block(b"H", offset, count)
                    signatures.write(signature)
                    data = basis.read(count) if basis else b""
                    match = len(data) == count and hashlib.sha256(data).digest() == signature
                    same = same and match
                if snapshot(destination) != before:
                    raise SyncError("destination changed during scan")
                if same:
                    self.client.rpc(b"X"); opened = False
                    self.stats["files_skipped"] += 1
                    self.report("skip", destination)
                    return
                self.report("copy", destination)
                if not self.dry_run:
                    local_path(destination.parent).mkdir(parents=True, exist_ok=True)
                    output = stack.enter_context(tempfile.NamedTemporaryFile(
                        prefix=".osync-", suffix=".part", dir=destination.parent, delete=False))
                    temporary = Path(output.name)
                if basis:
                    basis.seek(0)
                signatures.seek(0)
                expected = hashlib.sha256()
                for offset in range(0, size, BLOCK):
                    count = min(BLOCK, size - offset)
                    signature = signatures.read(32)
                    data = basis.read(count) if basis else b""
                    match = len(data) == count and hashlib.sha256(data).digest() == signature
                    self.stats["reused_bytes" if match else "data_bytes"] += count
                    if not self.dry_run:
                        if not match:
                            data = self.client.block(b"R", offset, count)
                        if hashlib.sha256(data).digest() != signature:
                            raise SyncError("download block SHA-256 mismatch")
                        expected.update(data)
                        output.write(data)
                self.client.rpc(b"X"); opened = False
                if not self.dry_run:
                    output.flush(); os.fsync(output.fileno())
                    if digest_file(output) != expected.digest():
                        raise SyncError("download readback SHA-256 mismatch")
                    if before:
                        os.chmod(temporary, stat.S_IMODE(before[5]))
            if not self.dry_run:
                if snapshot(destination) != before:
                    raise SyncError("destination changed before replacement")
                os.replace(temporary, destination)
                temporary = None
                if os.name != "nt":
                    fd = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
                    try:
                        os.fsync(fd)
                    finally:
                        os.close(fd)
            self.stats["files_copied"] += 1
        finally:
            if temporary:
                temporary.unlink(missing_ok=True)
            if opened:
                self.client.rpc(b"X")

    def push(self, source, destination, recursive=False):
        source, destination = local_path(source), remote_path(destination)
        if source.is_dir():
            if not recursive:
                raise SyncError("directory source requires --recursive")
            entries = sorted(source.iterdir())
            names = set()
            for entry in entries:
                name = valid_name(entry.name)
                local_path(entry)
                if name.casefold() in names:
                    raise SyncError(f"case-colliding filename: {name!r}")
                names.add(name.casefold())
            self.remote_mkdirs(destination)
            for entry in entries:
                self.push(entry, destination.rstrip("/") + "/" + entry.name, True)
        else:
            self.push_file(source, destination)

    def pull(self, source, destination, recursive=False):
        source, destination = remote_path(source), local_path(destination)
        info = self.client.stat(source)
        if not info:
            raise SyncError(f"missing source: {source}")
        if info[0] == 2:
            if not recursive:
                raise SyncError("directory source requires --recursive")
            entries = self.client.listing(source)
            if destination.exists() and not destination.is_dir():
                raise SyncError(f"not a directory: {destination}")
            if not destination.exists():
                self.report("mkdir", destination)
                self.stats["directories_created"] += 1
                if not self.dry_run:
                    destination.mkdir(parents=True)
            for name, _ in entries:
                self.pull(source.rstrip("/") + "/" + name, destination / name, True)
        else:
            self.pull_file(source, destination)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", required=True, help="OpenSSH destination, e.g. osito@192.168.1.20")
    p.add_argument("-i", "--identity")
    p.add_argument("--known-hosts")
    p.add_argument("--host-key-alias")
    p.add_argument("-p", "--port", type=int, default=22)
    p.add_argument("--ssh", default="ssh", help="OpenSSH executable")
    p.add_argument("--timeout", type=float, default=60)
    p.add_argument("-r", "--recursive", action="store_true")
    p.add_argument("-n", "--dry-run", action="store_true")
    p.add_argument("--json", action="store_true")
    p.add_argument("direction", choices=("push", "pull"))
    p.add_argument("source")
    p.add_argument("destination", help="exact destination path, not a parent directory")
    a = p.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.@:\[\]-]*", a.host):
        p.error("invalid SSH destination")
    if not 1 <= a.port <= 65535 or not 0 < a.timeout <= 600:
        p.error("invalid port or timeout (must be > 0 and <= 600 seconds)")
    command = [a.ssh, "-T", "-s", "-oBatchMode=yes", "-oStrictHostKeyChecking=yes",
               "-oConnectTimeout=10", "-oServerAliveInterval=5", "-oServerAliveCountMax=3",
               "-p", str(a.port)]
    if a.identity:
        command += ["-oIdentitiesOnly=yes", "-i", a.identity]
    if a.known_hosts:
        command += ["-o", "UserKnownHostsFile=" + a.known_hosts]
    if a.host_key_alias:
        command += ["-o", "HostKeyAlias=" + a.host_key_alias]
    command += [a.host, "osito-sync-v1"]
    client = None
    try:
        client = Client(command, a.timeout)
        sync = Sync(client, a.dry_run, a.json)
        getattr(sync, a.direction)(a.source, a.destination, a.recursive)
        client.close(); client = None
        print(json.dumps(sync.stats, sort_keys=True))
        return 0
    except (SyncError, OSError, UnicodeError, KeyboardInterrupt) as e:
        print(f"osito-sync: {e}", file=sys.stderr)
        return 1
    finally:
        if client:
            client.close(graceful=False)


if __name__ == "__main__":
    sys.exit(main())
