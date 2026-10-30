#!/usr/bin/env python3
"""Check the raw UART stream, without filling gaps from dmesg or screenshots."""

import argparse
import fcntl
import pathlib
import re
import socket
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def check_frames(data):
    require(b"[SERIAL-CORE] checks=118 failures=0\r\n" in data, "core matrix failed")
    formatted = (
        b"[SERIAL-FORMAT] 0xFFFFFFFFFFFFFFFF 0xCDEF 0x 0x0000000000000001 0x "
        b"18446744073709551615 0\r\n"
    )
    require(formatted in data, "numeric formatting or CRLF mismatch")
    require(b"[SERIAL-NUL] ab\r\n" in data, "bounded write/NUL mismatch")
    frames = re.findall(
        rb"(?m)^\[SERIAL-FRAME\] ([0-9A-F]{2}) ([0-9A-F]{4}) "
        rb"([0-9A-F]{2}) ([0-9A-F]{8})\r\n", data
    )
    require(len(frames) == 256, f"expected 256 whole records, got {len(frames)}")
    require(data.count(b"[SERIAL-FRAME]") == 256, "extra or malformed records")
    next_seq = [0] * 4
    mask = 0
    for fields in frames:
        producer, seq, cpu, checksum = (int(value, 16) for value in fields)
        require(producer < 4 and cpu < 16, "invalid producer or CPU")
        require(seq == next_seq[producer], "missing, duplicated or reordered record")
        expected = (0x9E3779B9 ^ ((producer + 1) * 0x10001) ^
                    ((seq * 0x45D9F3B) & 0xFFFFFFFF) ^ cpu)
        require(checksum == expected, "corrupt record checksum")
        next_seq[producer] += 1
        mask |= 1 << cpu
    require(next_seq == [64] * 4, "incomplete producer")
    summary = re.search(
        rb"\[SERIAL-SMP\] records=256 cpumask=0x([0-9A-F]{8}) dropped=0 pending=0\r\n", data
    )
    require(summary and int(summary[1], 16) == mask, "SMP summary disagrees with wire records")
    require(mask & (mask - 1), "test did not execute on multiple CPUs")
    require(b"Serial contract test: PASS\r\n" in data, "guest serial test failed")
    check_stats(data)
    return f"256 records, cpu mask={mask:04x}"


def check_stats(data):
    match = re.search(
        rb"\[SERIAL\] accepted=([0-9]+) sent=([0-9]+) pending=([0-9]+) "
        rb"dropped=([0-9]+) timeouts=([0-9]+) busy=([0-9]+) capacity=16384 fifo=(1|16) present=1\r\n",
        data,
    )
    require(match, "missing UART counters")
    accepted, sent, pending, dropped = (int(value) for value in match.groups()[:4])
    require(not dropped and not pending and accepted == sent, "UART lost or retained bytes")


def run_command(channel, command, capture):
    with capture.open("xb") as saved:
        for value in (command + "\r").encode("ascii"):
            channel.sendall(bytes([value]))
            time.sleep(0.08)
        received = bytearray()
        deadline = time.monotonic() + 1800
        while time.monotonic() < deadline:
            try:
                data = channel.recv(65536)
            except TimeoutError:
                continue
            require(data, "serial disconnected")
            saved.write(data)
            saved.flush()
            received.extend(data)
            require(b"SYSTEM HALTED" not in received, "guest halted")
            if received.endswith(b"osito> "):
                return bytes(received)
        raise TimeoutError(f"observe existing command before retrying: {command}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=pathlib.Path)
    parser.add_argument("--suite", choices=("serial", "dos"), default="serial")
    parser.add_argument("--repeat", type=int, default=3)
    args = parser.parse_args()
    require(1 <= args.repeat <= 100, "repeat must be between 1 and 100")
    commands = ["serial-test"] if args.suite == "serial" else [
        "dos-umb-test", "dos-xms-test", "dos-bios-test", "serial-stats"
    ]
    matrices = {
        "dos-umb-test": (b"DOS UMB contract test: PASS\r\n", [b"DOS-UMB"]),
        "dos-xms-test": (b"DOS XMS contract test: PASS\r\n", [b"DOS-XMS"]),
        "dos-bios-test": (b"DOS BIOS contract test: PASS\r\n", [b"DOS-A20", b"DOS-BIOS-MOVE"]),
    }
    with (args.run / ".serial-checker.lock").open("a") as lease:
        try:
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise SystemExit("Another serial checker owns this VM; no command was sent")
        require(not list(args.run.glob(f"serial-{args.suite}-*.bin")),
                "this suite already has evidence; do not overwrite or reissue it")
        with socket.socket(socket.AF_UNIX) as channel:
            channel.connect(str(args.run / "serial.sock"))
            channel.settimeout(0.2)
            for iteration in range(args.repeat):
                for index, command in enumerate(commands):
                    print(f"RUN {iteration + 1}: {command}", flush=True)
                    capture = args.run / f"serial-{args.suite}-{iteration + 1:02}-{index:02}.bin"
                    data = run_command(channel, command, capture)
                    detail = ""
                    if command == "serial-test":
                        detail = check_frames(data)
                    elif command == "serial-stats":
                        check_stats(data)
                    else:
                        expected, labels = matrices[command]
                        require(expected in data, "missing complete DOS result")
                        require(not re.search(rb"failures=[1-9][0-9]*", data), "DOS matrix failed")
                        for label in labels:
                            pattern = rb"\[" + label + rb"\] checks=[0-9]+ failures=0\r\n"
                            require(re.search(pattern, data), f"incomplete matrix: {label!r}")
                    print(f"PASS {command} {detail}".rstrip(), flush=True)


if __name__ == "__main__":
    main()
