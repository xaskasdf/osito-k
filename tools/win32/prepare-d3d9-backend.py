#!/usr/bin/env python3
"""Prepare or verify the external WineD3D/Mesa installation tree for Osito-K."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import urllib.request

REPO = Path(__file__).resolve().parents[2]
LOCK = Path(__file__).with_name("d3d9-backend-sources.json")
METADATA = "System/Backends/d3d9"
MANIFEST = METADATA + "/manifest.json"
SPEC = importlib.util.spec_from_file_location("osito_sync", REPO / "tools/osito-sync.py")
SYNC = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SYNC)


def backend_digest(data):
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def backend_fetch(entry, cache, offline):
    destination = SYNC.local_path(cache / entry["cache_name"])
    if destination.exists():
        data = destination.read_bytes()
    else:
        if offline:
            raise ValueError(f"not cached: {destination}")
        print(f"Downloading {entry['url']}", file=sys.stderr)
        with urllib.request.urlopen(entry["url"], timeout=45) as response:
            data = response.read(entry["bytes"] + 1)
        if backend_digest(data) != {k: entry[k] for k in ("bytes", "sha256")}:
            raise ValueError(f"download checksum/size mismatch: {entry['url']}")
        cache.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as temporary:
            temporary.write(data)
            temporary_path = Path(temporary.name)
        try:
            temporary_path.replace(destination)
        finally:
            temporary_path.unlink(missing_ok=True)
    if backend_digest(data) != {k: entry[k] for k in ("bytes", "sha256")}:
        raise ValueError(f"cached checksum/size mismatch: {destination}")
    return destination


def backend_member(archive, member):
    # Extract only a named member to stdout; never unpack archive paths on disk.
    return subprocess.check_output(["tar", "-xOf", str(archive), member], timeout=30)


def backend_write(root, relative, data, files, component):
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    files[relative] = dict(backend_digest(data), component=component)


def prepare_d3d9_backend(module, output, cache, offline):
    module, output, cache = (SYNC.local_path(path) for path in (module, output, cache))
    if output.exists():
        raise ValueError("output already exists; use a new directory or verify it")
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    module_data = module.read_bytes()
    if (len(module_data) < 64 or module_data[:6] != b"\x7fELF\x02\x01" or
            struct.unpack_from("<HH", module_data, 16) != (3, 62)):
        raise ValueError("module must be an x86-64 little-endian ELF shared object")
    # Validate every download and selected member before creating the output.
    payload = {}
    for entry in lock["sources"]:
        archive = backend_fetch(entry, cache, offline)
        for member in entry["members"]:
            data = (archive.read_bytes() if member["source"] is None else
                    backend_member(archive, member["source"]))
            if backend_digest(data) != {k: member[k] for k in ("bytes", "sha256")}:
                raise ValueError(f"member checksum mismatch: {member['source']}")
            payload[member["destination"]] = (data, entry["id"])
    files = {}
    output.mkdir(parents=True)
    for relative, (data, component) in sorted(payload.items()):
        backend_write(output, relative, data, files, component)
    backend_write(output, "libGL-osito.so", module_data, files, "osito-mesa")
    backend_write(output, METADATA + "/licenses/mesa/LICENSE.rst",
                  (REPO / "arch/x86/lib/opengl/mesa/LICENSE.rst").read_bytes(),
                  files, "osito-mesa")
    manifest = {
        "format": 1,
        "backend": "wined3d-3.8-d3d9-mesa-softpipe",
        "source_lock_sha256": hashlib.sha256(LOCK.read_bytes()).hexdigest(),
        "sources": lock["sources"],
        "module_build": "make -C arch/x86/lib/opengl module",
        "files": files,
    }
    (output / MANIFEST).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                                   encoding="utf-8", newline="\n")
    return verify_d3d9_backend(output)


def verify_d3d9_backend(output):
    # Reuse the transfer client's path/reparse checks before handing it this tree.
    output = SYNC.local_path(output)
    manifest_path = SYNC.local_path(output / MANIFEST)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    if (manifest["format"] != 1 or
            manifest["source_lock_sha256"] != hashlib.sha256(LOCK.read_bytes()).hexdigest() or
            manifest["sources"] != lock["sources"]):
        raise ValueError("manifest does not match this source lock")
    expected = {member["destination"]: dict(
        bytes=member["bytes"], sha256=member["sha256"], component=entry["id"])
        for entry in lock["sources"] for member in entry["members"]}
    expected["libGL-osito.so"] = manifest["files"]["libGL-osito.so"]
    expected[METADATA + "/licenses/mesa/LICENSE.rst"] = dict(backend_digest(
        (REPO / "arch/x86/lib/opengl/mesa/LICENSE.rst").read_bytes()), component="osito-mesa")
    if manifest["files"] != expected:
        raise ValueError("manifest file list differs from the selected backend")
    found = set()
    for path in output.rglob("*"):
        SYNC.local_path(path)
        if path.is_dir():
            continue
        relative = path.relative_to(output).as_posix()
        if relative == MANIFEST:
            continue
        if relative not in expected:
            raise ValueError(f"unexpected package file: {relative}")
        SYNC.remote_path("/" + relative)
        if (not path.is_file() or backend_digest(path.read_bytes()) !=
                {k: expected[relative][k] for k in ("bytes", "sha256")}):
            raise ValueError(f"package checksum/size mismatch: {relative}")
        found.add(relative)
    if found != set(expected):
        raise ValueError(f"missing package files: {sorted(set(expected) - found)}")
    return {"verified": True, "files": len(found),
            "bytes": sum(row["bytes"] for row in expected.values()),
            "module_sha256": expected["libGL-osito.so"]["sha256"],
            "manifest_sha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest()}


def backend_cli():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("prepare", help="download pinned inputs and prepare a new tree")
    prepare.add_argument("--module", required=True, type=Path, help="locally built libGL-osito.so")
    prepare.add_argument("--output", required=True, type=Path)
    prepare.add_argument("--cache", type=Path, default=REPO / "build/d3d9-backend-cache")
    prepare.add_argument("--offline", action="store_true", help="require all pinned files in cache")
    verify = commands.add_parser("verify", help="verify exact file set and hashes before transfer")
    verify.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "verify":
            result = verify_d3d9_backend(args.output)
        else:
            result = prepare_d3d9_backend(args.module, args.output, args.cache, args.offline)
        print(json.dumps(result, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, SYNC.SyncError, subprocess.SubprocessError) as error:
        print(f"d3d9-backend: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(backend_cli())
