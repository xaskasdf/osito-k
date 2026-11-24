#!/usr/bin/env python3
"""Build the Osito-K system root store from platform or PEM roots."""

from __future__ import annotations

import argparse
import base64
import hashlib
import re
import ssl
import struct
import sys
from pathlib import Path


MAGIC = b"OSITOCA\0"
VERSION = 2
HEADER_SIZE = 56
ENTRY_HEADER_SIZE = 44
MAX_ROOTS = 256
MAX_CERT_SIZE = 16384
ALL_USAGES = 0xFFFFFFFF
MAX_TRUST_OIDS = 64
MAX_TRUST_BYTES = 4096
OID_TEXT = re.compile(r"^[0-9]+(?:\.[0-9]+)+$")
PEM_CERT = re.compile(
    rb"-----BEGIN CERTIFICATE-----\s*(.*?)\s*-----END CERTIFICATE-----",
    re.DOTALL,
)


def validate_der(cert: bytes, source: str) -> None:
    if len(cert) < 4 or len(cert) > MAX_CERT_SIZE or cert[0] != 0x30:
        raise ValueError(f"{source}: invalid DER certificate size or tag")
    length_byte = cert[1]
    if length_byte < 0x80:
        header_size = 2
        body_size = length_byte
    else:
        length_octets = length_byte & 0x7F
        if length_octets == 0 or length_octets > 4 or 2 + length_octets > len(cert):
            raise ValueError(f"{source}: invalid DER length")
        header_size = 2 + length_octets
        body_size = int.from_bytes(cert[2:header_size], "big")
    if header_size + body_size != len(cert):
        raise ValueError(f"{source}: DER certificate has trailing or missing data")


Certificate = tuple[bytes, frozenset[str] | None]


def certificates_from_pem(path: Path) -> list[Certificate]:
    data = path.read_bytes()
    matches = PEM_CERT.findall(data)
    if not matches:
        raise ValueError(f"{path}: no PEM certificates found")
    result = []
    for index, encoded in enumerate(matches):
        compact = re.sub(rb"\s+", b"", encoded)
        cert = base64.b64decode(compact, validate=True)
        validate_der(cert, f"{path} certificate {index + 1}")
        result.append((cert, None))
    return result


def certificates_from_der(path: Path) -> list[Certificate]:
    paths = sorted(path.rglob("*")) if path.is_dir() else [path]
    result = []
    for item in paths:
        if not item.is_file():
            continue
        cert = item.read_bytes()
        validate_der(cert, str(item))
        result.append((cert, None))
    return result


def certificates_from_system() -> list[Certificate]:
    if hasattr(ssl, "enum_certificates"):
        result = []
        unsupported = set()
        for cert, encoding, trust in ssl.enum_certificates("ROOT"):
            if encoding != "x509_asn":
                unsupported.add(encoding)
                continue
            validate_der(cert, "Windows ROOT store")
            policy = None if trust is True else frozenset(trust)
            result.append((cert, policy))
        if unsupported:
            print(
                "warning: skipped unsupported Windows encodings: "
                + ", ".join(sorted(unsupported)),
                file=sys.stderr,
            )
        return result

    cafile = ssl.get_default_verify_paths().cafile
    if not cafile:
        raise RuntimeError("platform has no discoverable default CA file")
    return certificates_from_pem(Path(cafile))


def merge_policy(
    current: frozenset[str] | None, incoming: frozenset[str] | None
) -> frozenset[str] | None:
    if current is None or incoming is None:
        return None
    return current | incoming


def encode_policy(policy: frozenset[str] | None) -> tuple[int, bytes]:
    if policy is None:
        return ALL_USAGES, b""
    ordered = sorted(policy)
    if len(ordered) > MAX_TRUST_OIDS:
        raise ValueError(f"trust OID count {len(ordered)} exceeds {MAX_TRUST_OIDS}")
    for oid in ordered:
        if not OID_TEXT.fullmatch(oid):
            raise ValueError(f"invalid trust OID: {oid!r}")
    encoded = b"".join(oid.encode("ascii") + b"\0" for oid in ordered)
    if len(encoded) > MAX_TRUST_BYTES:
        raise ValueError("encoded trust policy is too large")
    return len(ordered), encoded


def build_store(certificates: list[Certificate]) -> bytes:
    unique: dict[bytes, Certificate] = {}
    for cert, policy in certificates:
        fingerprint = hashlib.sha256(cert).digest()
        if fingerprint in unique:
            old_cert, old_policy = unique[fingerprint]
            unique[fingerprint] = (old_cert, merge_policy(old_policy, policy))
        else:
            unique[fingerprint] = (cert, policy)
    ordered = sorted(unique.items())
    if not ordered or len(ordered) > MAX_ROOTS:
        raise ValueError(f"root count {len(ordered)} is outside 1..{MAX_ROOTS}")

    payload = bytearray()
    for fingerprint, (cert, policy) in ordered:
        trust_count, trust_data = encode_policy(policy)
        entry = (
            struct.pack(
                "<III32s", len(cert), trust_count, len(trust_data), fingerprint
            )
            + trust_data
            + cert
        )
        if len(entry) < ENTRY_HEADER_SIZE:
            raise AssertionError("root-store entry layout changed")
        payload.extend(entry)
        payload.extend(b"\0" * ((-len(entry)) & 3))

    header = struct.pack(
        "<8sIIII32s",
        MAGIC,
        VERSION,
        HEADER_SIZE,
        len(ordered),
        len(payload),
        hashlib.sha256(payload).digest(),
    )
    if len(header) != HEADER_SIZE:
        raise AssertionError("root-store header layout changed")
    return header + payload


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build tls/ca-roots.bin for the Osito-K certificate manager"
    )
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--system", action="store_true", help="read the platform default root store"
    )
    parser.add_argument("--pem", action="append", type=Path, default=[])
    parser.add_argument(
        "--der", action="append", type=Path, default=[],
        help="add one DER certificate or every file in a directory",
    )
    args = parser.parse_args()

    certificates = []
    if args.system or (not args.pem and not args.der):
        certificates.extend(certificates_from_system())
    for path in args.pem:
        certificates.extend(certificates_from_pem(path))
    for path in args.der:
        certificates.extend(certificates_from_der(path))

    store = build_store(certificates)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(store)
    count = struct.unpack_from("<I", store, 16)[0]
    print(f"Wrote {args.output}: {count} roots, {len(store)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
