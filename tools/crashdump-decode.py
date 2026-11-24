#!/usr/bin/env python3
"""Decode OsitoK crash_report_t dumps (v1/v2) into a readable summary."""

from __future__ import annotations

import pathlib
import struct
import sys


CRASH_MAGIC = 0x4F534B43
CRASH_MAX_FRAMES = 32


VECTOR_NAMES = {
    0: "divide error",
    1: "debug",
    2: "nmi",
    3: "breakpoint",
    4: "overflow",
    5: "bounds",
    6: "invalid opcode",
    7: "device not available",
    8: "double fault",
    10: "invalid tss",
    11: "segment not present",
    12: "stack fault",
    13: "general protection",
    14: "page fault",
    16: "x87 floating point",
    17: "alignment check",
    18: "machine check",
    19: "simd floating point",
    20: "virtualization",
    21: "control protection",
}


def u32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf: bytes, off: int) -> int:
    return struct.unpack_from("<Q", buf, off)[0]


def cstr(raw: bytes) -> str:
    raw = raw.split(b"\0", 1)[0]
    return raw.decode("utf-8", "replace")


def hx(v: int, digits: int = 16) -> str:
    return f"0x{v:0{digits}x}"


def byte_line(data: bytes) -> str:
    return " ".join(f"{b:02x}" for b in data)


def parse(buf: bytes) -> dict:
    if len(buf) < 2920:
        raise ValueError(f"file is too small for crash_report_t: {len(buf)} bytes")
    magic = u32(buf, 0)
    if magic != CRASH_MAGIC:
        raise ValueError(f"bad magic {hx(magic, 8)}; expected OSKC")

    regs_off = 112
    reg_names = [
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        "rip", "rflags", "cs", "ss",
    ]
    regs = {name: u64(buf, regs_off + i * 8) for i, name in enumerate(reg_names)}

    frame_count = min(u32(buf, 272), CRASH_MAX_FRAMES)
    frames = []
    off = 280
    for _ in range(frame_count):
        addr = u64(buf, off)
        symbol = cstr(buf[off + 8:off + 72])
        sym_off = u64(buf, off + 72)
        frames.append((addr, symbol, sym_off))
        off += 80

    code_valid = bool(buf[2912]) if len(buf) > 2912 else False
    stack_valid = bool(buf[4976]) if len(buf) > 4976 else False

    return {
        "version": u32(buf, 4),
        "name": cstr(buf[8:72]) or "(unknown)",
        "pid": u32(buf, 72),
        "uptime_ticks": u64(buf, 80),
        "vector": u32(buf, 88),
        "error_code": u64(buf, 96),
        "fault_addr": u64(buf, 104),
        "regs": regs,
        "frames": frames,
        "code_valid": code_valid,
        "code_base": u64(buf, 2904) if len(buf) >= 2912 else 0,
        "code_before": buf[2840:2872] if len(buf) >= 2872 else b"",
        "code_after": buf[2872:2904] if len(buf) >= 2904 else b"",
        "stack_valid": stack_valid,
        "stack_base": u64(buf, 2920) if len(buf) >= 2928 else 0,
        "stack_words": [
            u64(buf, 2928 + i * 8)
            for i in range(min(256, max(0, (len(buf) - 2928) // 8)))
        ],
    }


def render(path: pathlib.Path, report: dict) -> str:
    vector = report["vector"]
    regs = report["regs"]
    lines = [
        f"{path}:",
        f"OsitoK crash report v{report['version']}",
        f"process: {report['name']}",
        f"pid: {report['pid']}",
        f"uptime_ticks: {report['uptime_ticks']}",
        f"vector: {vector} ({VECTOR_NAMES.get(vector, 'unknown')})",
        f"error_code: {hx(report['error_code'])}",
        f"fault_addr: {hx(report['fault_addr'])}",
        "",
        "registers:",
        f"  rip={hx(regs['rip'])} rsp={hx(regs['rsp'])} rbp={hx(regs['rbp'])}",
        f"  rax={hx(regs['rax'])} rbx={hx(regs['rbx'])} rcx={hx(regs['rcx'])} rdx={hx(regs['rdx'])}",
        f"  rsi={hx(regs['rsi'])} rdi={hx(regs['rdi'])} rflags={hx(regs['rflags'])}",
        f"  r8 ={hx(regs['r8'])} r9 ={hx(regs['r9'])} r10={hx(regs['r10'])} r11={hx(regs['r11'])}",
        f"  r12={hx(regs['r12'])} r13={hx(regs['r13'])} r14={hx(regs['r14'])} r15={hx(regs['r15'])}",
        f"  cs={hx(regs['cs'], 4)} ss={hx(regs['ss'], 4)}",
        "",
        "backtrace:",
    ]

    if report["frames"]:
        for idx, (addr, symbol, sym_off) in enumerate(report["frames"]):
            suffix = f" {symbol}+{hx(sym_off, 4)}" if symbol else ""
            lines.append(f"  #{idx} {hx(addr)}{suffix}")
    else:
        lines.append("  (none)")

    if report["code_valid"]:
        lines.extend([
            "",
            "code bytes:",
            f"  base={hx(report['code_base'])}",
            f"  before: {byte_line(report['code_before'])}",
            f"  at_rip: {byte_line(report['code_after'])}",
        ])

    if report["stack_valid"]:
        lines.extend(["", "stack top:", f"  base={hx(report['stack_base'])}"])
        for idx, word in enumerate(report["stack_words"][:16]):
            lines.append(f"  [{idx}] {hx(word)}")

    return "\n".join(lines)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: crashdump-decode.py <crash.bin> [...]", file=sys.stderr)
        return 2

    rc = 0
    for arg in argv[1:]:
        path = pathlib.Path(arg)
        try:
            report = parse(path.read_bytes())
            print(render(path, report))
        except Exception as exc:  # keep batch decoding useful
            rc = 1
            print(f"{path}: {exc}", file=sys.stderr)
        if arg != argv[-1]:
            print()
    return rc


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
