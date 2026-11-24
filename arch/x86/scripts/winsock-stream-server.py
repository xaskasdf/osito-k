#!/usr/bin/env python3
"""Bounded, synthetic stream oracle. No files or commands from the peer."""
import argparse
import hashlib
import json
import socket
import struct
import time


def pattern(size, seed):
    result = bytearray()
    for offset in range((size + 3) // 4):
        n = (offset + seed) & 0xFFFFFFFF
        n = ((n ^ (n >> 16)) * 0x7FEB352D) & 0xFFFFFFFF
        n = ((n ^ (n >> 15)) * 0x846CA68B) & 0xFFFFFFFF
        result.extend(struct.pack('<I', n ^ (n >> 16)))
    return bytes(result[:size])


def exact(conn, count):
    result = bytearray()
    while len(result) < count:
        data = conn.recv(count - len(result))
        if not data:
            raise RuntimeError(f'early EOF at {len(result)} of {count}')
        result.extend(data)
    return bytes(result)


def transfer(conn, *, corrupt_at=-1, max_mib=8):
    conn.settimeout(60)
    magic, size, seed = struct.unpack('<4sII', exact(conn, 12))
    if magic != b'OSS1' or not 1 <= size <= max_mib * 1024 * 1024:
        raise ValueError('invalid stream header')
    started = time.monotonic()
    rx = pattern(size, seed)
    tx = pattern(size, seed ^ 0xA5A5A5A5)
    if corrupt_at >= 0:
        if corrupt_at >= size:
            raise ValueError('corruption offset outside stream')
        rx = rx[:corrupt_at] + bytes([rx[corrupt_at] ^ 1]) + rx[corrupt_at + 1:]
    sent = 0
    chunks = (1, 7, 4093, 16384, 65535, 131073, 29)
    turn = 0
    while sent < size:
        block = rx[sent:sent + chunks[turn % len(chunks)]]
        conn.sendall(block)
        sent += len(block)
        if turn % 11 == 0:
            time.sleep(.01)
        turn += 1
    received = 0
    digest = hashlib.sha256()
    while received < size:
        data = conn.recv(min(65536, size - received))
        if not data:
            raise RuntimeError(f'TX early EOF at {received} of {size}')
        expected = tx[received:received + len(data)]
        if data != expected:
            mismatch = next(i for i, (a, b) in enumerate(zip(data, expected)) if a != b)
            raise RuntimeError(f'TX mismatch at {received + mismatch}')
        received += len(data)
        digest.update(data)
        if received % 131072 == 0:
            time.sleep(.02)
    if conn.recv(1):
        raise RuntimeError('TX contains extra data')
    conn.sendall(b'PASS' + struct.pack('<I', received))
    return dict(bytes_each_direction=size, seconds=round(time.monotonic() - started, 3),
                rx_sha256=hashlib.sha256(rx).hexdigest(), tx_sha256=digest.hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=0)
    parser.add_argument('--peer', help='accept only this source IPv4')
    parser.add_argument('--connections', type=int, default=4)
    parser.add_argument('--corrupt-at', type=int, default=-1)
    args = parser.parse_args()
    if not 1 <= args.connections <= 16 or not 0 <= args.port <= 65535:
        parser.error('invalid connection count or port')
    failures = 0
    with socket.socket() as listener:
        listener.bind((args.bind, args.port))
        listener.listen(4)
        listener.settimeout(300)
        print(json.dumps(dict(listening=listener.getsockname(), connections=args.connections)), flush=True)
        for index in range(args.connections):
            conn, peer = listener.accept()
            with conn:
                if args.peer and peer[0] != args.peer:
                    raise RuntimeError('unexpected client')
                try:
                    result = transfer(conn, corrupt_at=args.corrupt_at)
                    print(json.dumps(dict(index=index, peer=peer, ok=True, **result)), flush=True)
                except (OSError, RuntimeError, ValueError) as error:
                    failures += 1
                    print(json.dumps(dict(index=index, peer=peer, ok=False, error=str(error))), flush=True)
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
