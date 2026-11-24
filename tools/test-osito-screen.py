#!/usr/bin/env python3
"""Host validation of snapshot bounds, integrity, and PNG publication."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

spec = importlib.util.spec_from_file_location("screen", Path(__file__).with_name("osito-screen.py"))
screen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(screen)


def decode_png(path):
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, compressed, kinds = 8, bytearray(), []
    while pos < len(data):
        size, = struct.unpack_from(">I", data, pos)
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + size]
        crc, = struct.unpack_from(">I", data, pos + 8 + size)
        assert crc == zlib.crc32(kind + body) & 0xffffffff
        kinds.append(kind)
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">2I5B", body)
            assert (depth, color, compression, filtering, interlace) == (8, 2, 0, 0, 0)
        if kind == b"IDAT":
            compressed.extend(body)
        pos += size + 12
    assert pos == len(data) and kinds[0] == b"IHDR" and kinds[-1] == b"IEND"
    raw = zlib.decompress(compressed)
    stride = width * 3 + 1
    assert len(raw) == height * stride
    assert all(raw[y * stride] == 0 for y in range(height))
    return width, height, b"".join(raw[y * stride + 1:(y + 1) * stride] for y in range(height))


class FakeClient:
    def __init__(self):
        self.rgb = bytes((255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 3, 5, 7))
        self.meta = struct.pack(">6I2Q", 3, 2, 1, 1, 1, 0, len(self.rgb), 12345) + hashlib.sha256(self.rgb).digest()
        self.released = False
        self.corrupt = self.short = False
        self.legacy = False

    def rpc(self, request):
        if request == b"C":
            return self.meta
        if request == b"X":
            self.released = True
            return b""
        if request[:1] == b"Z" and self.legacy:
            raise screen.sync.SyncError("unsupported", 2)
        offset, count = struct.unpack(">QI", request[1:])
        block = self.rgb[offset:offset + count]
        if self.short:
            block = block[:-1]
        if self.corrupt:
            block = bytes([block[0] ^ 1]) + block[1:]
        return b"\x01" + zlib.compress(block) if request[:1] == b"Z" else block


class ScreenTests(unittest.TestCase):
    def test_legacy_fallback_and_raw(self):
        for legacy, compressed in ((True, True), (False, False)):
            client = FakeClient(); client.legacy = legacy
            info, rgb = screen.capture(client, compressed=compressed)
            self.assertEqual(rgb, client.rgb)
            self.assertFalse(info["compressed"])
            self.assertEqual(info["wire_bytes"], len(rgb))

    def test_compressed_bounds(self):
        for bad in (b"", b"\x02", b"\x01" + zlib.compress(b"test")[:-1],
                    b"\x01" + zlib.compress(b"test") + b"junk",
                    b"\x01" + zlib.compress(b"x" * 100000)):
            with self.assertRaises(screen.sync.SyncError):
                screen.decode_block(bad, 4)
        self.assertEqual(screen.decode_block(b"\0test", 4), b"test")

    def test_rgb_and_png(self):
        for chunk in (1, 4, 7, 4096, 32768):
            client = FakeClient()
            info, rgb = screen.capture(client, chunk)
            self.assertEqual(rgb, client.rgb)
            self.assertTrue(client.released)
            self.assertFalse(info["atomic"])
            with tempfile.TemporaryDirectory() as tmp:
                path = screen.save_png(Path(tmp) / "screen.png", info, rgb)
                self.assertEqual(decode_png(path), (3, 2, client.rgb))
                with self.assertRaises(screen.sync.SyncError):
                    screen.save_png(path, info, rgb)
                self.assertEqual(decode_png(path)[2], client.rgb)
                screen.save_png(path, info, rgb, force=True)
                self.assertEqual(decode_png(path)[2], client.rgb)
                self.assertFalse(list(Path(tmp).glob("*.part")))

    def test_invalid_metadata(self):
        valid = FakeClient().meta
        cases = [valid[:-1], valid + b"x"]
        for field, value in ((0, 0), (0, 16385), (1, 0), (2, 2), (3, 2), (4, 0), (5, 1)):
            meta = bytearray(valid)
            struct.pack_into(">I", meta, field * 4, value)
            cases.append(meta)
        bad_size = bytearray(valid)
        struct.pack_into(">Q", bad_size, 24, 1 << 63)
        cases.append(bad_size)
        for meta in cases:
            client = FakeClient(); client.meta = meta
            with self.assertRaises(screen.sync.SyncError):
                screen.capture(client)
            self.assertTrue(client.released)

    def test_corrupt_and_short(self):
        for mode in ("short", "corrupt"):
            client = FakeClient(); setattr(client, mode, True)
            with self.assertRaises(screen.sync.SyncError):
                screen.capture(client)
            self.assertTrue(client.released)

    def test_symlinks_and_bad_rgb(self):
        client = FakeClient(); info, rgb = screen.capture(client)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            target = root / "target"; target.write_bytes(b"keep")
            link = root / "link.png"
            try:
                link.symlink_to(target)
            except OSError:
                pass
            else:
                with self.assertRaises(screen.sync.SyncError):
                    screen.save_png(link, info, rgb, force=True)
                self.assertEqual(target.read_bytes(), b"keep")
            with self.assertRaises(screen.sync.SyncError):
                screen.save_png(root / "bad.png", info, rgb[:-1])
            self.assertFalse((root / "bad.png").exists())


if __name__ == "__main__":
    unittest.main()
