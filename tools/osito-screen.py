#!/usr/bin/env python3
"""Read-only screen snapshots over authenticated SSH; save verified RGB as PNG."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import time
import zlib

spec = importlib.util.spec_from_file_location("osito_sync", Path(__file__).with_name("osito-sync.py"))
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)
ERRORS = {1: "no framebuffer", 2: "invalid screen request or geometry",
          3: "snapshot allocation failed", 4: "primary scanout backend is not supported",
          5: "snapshot already held", 6: "scanout changed or legacy capture timeout",
          7: "capture cancelled", 8: "capture timed out under load"}
MAX_PIXELS = 16 * 1024 * 1024


def decode_block(block, count):
    if not block or block[0] not in (0, 1):
        raise sync.SyncError("invalid snapshot encoding")
    if block[0] == 0:
        data = block[1:]
    else:
        decoder = zlib.decompressobj()
        try:
            data = decoder.decompress(block[1:], count + 1)
        except zlib.error as error:
            raise sync.SyncError("invalid compressed snapshot") from error
        if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
            raise sync.SyncError("truncated, oversized or trailing compressed snapshot")
    if len(data) != count:
        raise sync.SyncError("short or oversized snapshot block")
    return data


def capture(client, chunk_size=32768, compressed=True):
    if not 1 <= chunk_size <= sync.BLOCK:
        raise ValueError("invalid chunk size")
    metadata = client.rpc(b"C")
    failed = False
    try:
        if len(metadata) != 72:
            raise sync.SyncError("invalid snapshot metadata length")
        width, height, fmt, source, flags, reserved, size, uptime = struct.unpack(
            ">6I2Q", metadata[:40])
        if (not 0 < width <= 16384 or not 0 < height <= 16384 or
                width * height > MAX_PIXELS or size != width * height * 3 or
                fmt != 1 or source != 1 or flags != 1 or reserved):
            raise sync.SyncError("unsupported or invalid snapshot metadata")
        expected = metadata[40:]
        data = bytearray()
        digest = hashlib.sha256()
        wire_bytes = 0
        transfer_started = time.monotonic()
        for offset in range(0, size, chunk_size):
            count = min(chunk_size, size - offset)
            request = struct.pack(">QI", offset, count)
            if compressed:
                try:
                    encoded = client.rpc(b"Z" + request)
                except sync.SyncError as error:
                    if offset or error.status != 2:
                        raise
                    compressed = False  # Older v1 servers only implement R.
                else:
                    wire_bytes += len(encoded)
                    block = decode_block(encoded, count)
            if not compressed:
                block = client.rpc(b"R" + request)
                wire_bytes += len(block)
            if len(block) != count:
                raise sync.SyncError("short snapshot block")
            digest.update(block)
            data.extend(block)
        if digest.digest() != expected:
            raise sync.SyncError("snapshot SHA-256 mismatch")
        return dict(width=width, height=height, source="GOP", atomic=False,
                    uptime_ms=uptime, bytes=size, sha256=digest.hexdigest(),
                    wire_bytes=wire_bytes, compressed=compressed,
                    transfer_seconds=round(time.monotonic() - transfer_started, 3)), data
    except BaseException:
        failed = True
        raise
    finally:
        try:
            if client.rpc(b"X"):
                raise sync.SyncError("invalid release response")
        except Exception:
            if not failed:
                raise


def png_chunk(stream, kind, data):
    stream.write(struct.pack(">I", len(data)))
    stream.write(kind)
    stream.write(data)
    stream.write(struct.pack(">I", zlib.crc32(data, zlib.crc32(kind)) & 0xffffffff))


def save_png(destination, info, rgb, force=False):
    width, height = info["width"], info["height"]
    if not 0 < width <= 16384 or not 0 < height <= 16384 or width * height > MAX_PIXELS:
        raise sync.SyncError("invalid PNG dimensions")
    if len(rgb) != width * height * 3:
        raise sync.SyncError("invalid RGB byte count")
    destination = sync.local_path(destination)
    if destination.exists() and not force:
        raise sync.SyncError(f"destination exists: {destination}")
    if destination.exists() and not destination.is_file():
        raise sync.SyncError("destination must be a regular file")
    sync.local_path(destination.parent).mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".oscreen-", suffix=".part",
                dir=destination.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(b"\x89PNG\r\n\x1a\n")
            png_chunk(output, b"IHDR", struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0))
            compressor = zlib.compressobj()
            for y in range(height):
                offset = y * width * 3
                encoded = compressor.compress(b"\0" + rgb[offset:offset + width * 3])
                if encoded:
                    png_chunk(output, b"IDAT", encoded)
            png_chunk(output, b"IDAT", compressor.flush())
            png_chunk(output, b"IEND", b"")
            output.flush()
            os.fsync(output.fileno())
        sync.local_path(destination)
        if force:
            os.replace(temporary, destination)
            temporary = None
        else:
            # Publish without replacing a file another process created meanwhile.
            os.link(temporary, destination)
    finally:
        if temporary:
            temporary.unlink(missing_ok=True)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("-i", "--identity")
    parser.add_argument("--known-hosts")
    parser.add_argument("--host-key-alias")
    parser.add_argument("--bind-address")
    parser.add_argument("--ssh-config")
    parser.add_argument("-p", "--port", type=int, default=22)
    parser.add_argument("--ssh", default="ssh")
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--chunk-size", type=int, default=32768)
    parser.add_argument("--raw", action="store_true", help="disable wire compression for transport measurements")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", help="exact local PNG path; existing files need --force")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.@:\[\]-]*", args.host):
        parser.error("invalid SSH destination")
    if not 1 <= args.port <= 65535 or not 0 < args.timeout <= 600 or not 1 <= args.chunk_size <= sync.BLOCK:
        parser.error("invalid port, timeout or chunk size")
    command = [args.ssh, "-T", "-s", "-oBatchMode=yes", "-oStrictHostKeyChecking=yes",
               "-oConnectTimeout=10", "-oServerAliveInterval=5", "-oServerAliveCountMax=3",
               "-p", str(args.port)]
    for value, option in ((args.identity, "-i"), (args.bind_address, "-b"),
                          (args.ssh_config, "-F")):
        if value:
            command += [option, value]
    if args.identity:
        command += ["-oIdentitiesOnly=yes"]
    if args.known_hosts:
        command += ["-o", "UserKnownHostsFile=" + args.known_hosts]
    if args.host_key_alias:
        command += ["-o", "HostKeyAlias=" + args.host_key_alias]
    command += [args.host, "osito-screen-v1"]
    client = None
    started = time.monotonic()
    try:
        destination = sync.local_path(args.output)
        if destination.exists() and not args.force:
            raise sync.SyncError(f"destination exists: {destination}")
        client = sync.Client(command, args.timeout, version=b"OSCR0001", errors=ERRORS)
        info, rgb = capture(client, args.chunk_size, not args.raw)
        client.close(); client = None
        info["output"] = str(save_png(destination, info, rgb, args.force))
        info["seconds"] = round(time.monotonic() - started, 3)
        print(json.dumps(info, sort_keys=True))
        return 0
    except (sync.SyncError, OSError, ValueError, KeyboardInterrupt) as error:
        print(f"osito-screen: {error}", file=sys.stderr)
        return 1
    finally:
        if client:
            client.close(graceful=False)


if __name__ == "__main__":
    sys.exit(main())
