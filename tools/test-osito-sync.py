#!/usr/bin/env python3
"""Production sync protocol/client with a real temporary POSIX filesystem."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("osito_sync", Path(__file__).with_name("osito-sync.py"))
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)
SERVER = str(Path(sys.argv.pop(1)).resolve())


class Transfers(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.remote = self.root / "remote"
        self.remote.mkdir()
        self.client = sync.Client([SERVER, str(self.remote)], timeout=10)

    def tearDown(self):
        self.client.close()
        self.temp.cleanup()

    def run_sync(self, direction, source, dest, recursive=False, dry=False):
        task = sync.Sync(self.client, dry_run=dry, quiet=True)
        getattr(task, direction)(source, dest, recursive)
        return task.stats

    def begin(self, path, data):
        self.client.rpc(b"B" + struct.pack(">Q", len(data)) + path.encode())
        if data:
            self.client.rpc(b"W" + data)

    def assert_status(self, status, request):
        with self.assertRaises(sync.SyncError) as ctx:
            self.client.rpc(request)
        self.assertEqual(ctx.exception.status, status)

    def test_binary_large_incremental_both_directions(self):
        original = bytes(range(256)) * 8192 + b"\x00\r\nend"
        local = self.root / "large.bin"
        local.write_bytes(original)
        result = self.run_sync("push", local, "/large.bin")
        self.assertEqual(result["data_bytes"], len(original))
        self.assertEqual((self.remote / "large.bin").read_bytes(), original)
        result = self.run_sync("push", local, "/large.bin")
        self.assertEqual(result["files_skipped"], 1)
        self.assertEqual(result["data_bytes"], 0)
        changed = original[:sync.BLOCK + 5] + b"X" + original[sync.BLOCK + 6:]
        local.write_bytes(changed)
        result = self.run_sync("push", local, "/large.bin")
        self.assertEqual(result["data_bytes"], sync.BLOCK)
        self.assertEqual(result["reused_bytes"], len(original) - sync.BLOCK)
        download = self.root / "download.bin"
        download.write_bytes(original)
        result = self.run_sync("pull", "/large.bin", download)
        self.assertEqual(result["data_bytes"], sync.BLOCK)
        self.assertEqual(download.read_bytes(), changed)
        self.assertEqual(self.run_sync("pull", "/large.bin", download)["files_skipped"], 1)

    def test_empty_shrink_grow(self):
        local = self.root / "file"
        for data in (b"", b"x" * (sync.BLOCK + 9), b"same", b"size", b"", b"b" * 77):
            local.write_bytes(data)
            self.run_sync("push", local, "/file")
            self.run_sync("pull", "/file", self.root / "out")
            self.assertEqual((self.root / "out").read_bytes(), data)

    def test_tree_spaces_utf8_and_no_delete(self):
        source = self.root / "source"
        (source / "empty").mkdir(parents=True)
        (source / "folder").mkdir()
        (source / "folder" / "caf\u00e9 with space").write_bytes(b"\x00\xff\r\n")
        self.run_sync("push", source, "/tree", True)
        (self.remote / "tree" / "extra").write_bytes(b"keep")
        stats = self.run_sync("push", source, "/tree", True)
        self.assertEqual(stats["files_skipped"], 1)
        self.assertTrue((self.remote / "tree" / "extra").exists())
        self.run_sync("pull", "/tree", self.root / "out", True)
        self.assertTrue((self.root / "out" / "empty").is_dir())
        self.assertEqual((self.root / "out" / "folder" / "caf\u00e9 with space").read_bytes(), b"\x00\xff\r\n")

    def test_dry_run_has_no_destination_writes(self):
        source = self.root / "source"
        source.mkdir()
        (source / "new").write_bytes(b"123")
        self.run_sync("push", source, "/missing/dir", True, True)
        self.assertEqual(list(self.remote.iterdir()), [])
        (self.remote / "file").write_bytes(b"abc")
        self.run_sync("pull", "/file", self.root / "absent" / "file", dry=True)
        self.assertFalse((self.root / "absent").exists())

    def test_invalid_paths_and_offsets(self):
        for path in (b"../x", b"a/../x", b"a//b", b"a/", b"a\x00b", b"a\\b", b"a:b", b"x" * 260):
            self.assert_status(3, b"S" + path)
        (self.remote / "file").write_bytes(b"abc")
        self.client.open("/file")
        for off, count in ((2**64 - 1, 1), (4, 1), (1, 3), (0, 0), (0, sync.BLOCK + 1)):
            self.assert_status(3, b"R" + struct.pack(">QI", off, count))
        self.client.rpc(b"X")
        self.assert_status(3, b"R" + b"\x00" * 12)
        self.assert_status(3, b"V")
        self.assert_status(3, b"?")

    def test_checksum_length_and_io_failures_keep_destination(self):
        for name, wanted in (("bad-hash", 7), ("corrupt-write", 7), ("fail-flush", 4)):
            (self.remote / name).write_bytes(b"old")
            self.begin("/" + name, b"new")
            digest = b"\x00" * 32 if name == "bad-hash" else hashlib.sha256(b"new").digest()
            self.assert_status(wanted, b"F" + digest)
            self.assertEqual((self.remote / name).read_bytes(), b"old")
        self.client.rpc(b"B" + struct.pack(">Q", 9) + b"/short")
        self.client.rpc(b"W123")
        self.assert_status(3, b"F" + hashlib.sha256(b"123").digest())
        self.assert_status(3, b"W1234567")
        self.client.rpc(b"A")
        self.assertFalse((self.remote / "short").exists())
        self.client.rpc(b"B" + struct.pack(">Q", 3) + b"/fail-write")
        self.assert_status(4, b"Wabc")
        self.assertFalse(any(self.remote.glob(".osync-*")))

    def test_source_and_destination_changes(self):
        path = self.remote / "file"
        path.write_bytes(b"old")
        self.client.open("/file")
        path.write_bytes(b"new content")
        self.assert_status(5, b"H" + struct.pack(">QI", 0, 3))
        self.assert_status(5, b"X")
        self.begin("/file", b"xyz")
        path.write_bytes(b"local change")
        self.assert_status(5, b"F" + hashlib.sha256(b"xyz").digest())
        self.assertEqual(path.read_bytes(), b"local change")

    def test_directory_mutation_detected(self):
        self.client.rpc(b"L/")
        (self.remote / "new").write_bytes(b"x")
        self.assert_status(5, b"N")

    def test_64bit_read_offsets(self):
        path = self.remote / "sparse"
        offset = (1 << 32) + 9
        with path.open("wb") as stream:
            stream.seek(offset)
            stream.write(b"64-bit")
        self.assertEqual(self.client.open("/sparse"), offset + 6)
        self.assertEqual(self.client.block(b"R", offset, 6), b"64-bit")
        self.assertEqual(self.client.block(b"H", offset, 6), hashlib.sha256(b"64-bit").digest())
        self.client.rpc(b"X")

    def test_symlinks_collisions_and_file_type_rejected(self):
        source = self.root / "source"
        source.mkdir()
        (source / "one").write_bytes(b"x")
        (source / "ONE").write_bytes(b"y")
        with self.assertRaises(sync.SyncError):
            self.run_sync("push", source, "/tree", True)
        link = self.root / "link"
        link.symlink_to(source, target_is_directory=True)
        with self.assertRaises(sync.SyncError):
            self.run_sync("push", link, "/tree", True)
        (self.remote / "file").write_bytes(b"a")
        with self.assertRaises(sync.SyncError):
            self.run_sync("pull", "/file", link / "new")
        (self.remote / "dir").mkdir()
        with self.assertRaises(sync.SyncError):
            self.run_sync("push", source / "one", "/dir")
        for name in ("../escape", "C:ads", "NUL", "foo.", "a\\b", "x\nname"):
            with self.assertRaises(sync.SyncError):
                sync.valid_name(name)

    def test_abort_and_disconnect_cleanup(self):
        path = self.remote / "file"
        path.write_bytes(b"old")
        self.begin("/file", b"new")
        self.client.rpc(b"A")
        self.assertEqual(path.read_bytes(), b"old")
        self.begin("/file", b"new")
        self.client.process.stdin.close()
        self.client.process.wait(timeout=5)
        self.assertEqual(path.read_bytes(), b"old")
        self.assertFalse(any(self.remote.glob(".osync-*")))

    def test_truncated_and_oversized_frames(self):
        for data in (b"\xff\xff\xff\xff", struct.pack(">I", 12) + b"Wab"):
            proc = subprocess.run([SERVER, str(self.remote)], input=data, capture_output=True, timeout=5)
            self.assertNotEqual(proc.returncode, 0)
            self.assertEqual(proc.stdout, b"")


if __name__ == "__main__":
    unittest.main(verbosity=2)
