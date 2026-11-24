"""Loopback peer that delays reads to exercise overlapped send backpressure."""
import hashlib
import json
from pathlib import Path
import socket
import struct
import threading
import time


class Peer:
    def __init__(self, output, expected=4, delay=2, host='127.0.0.1'):
        self.output = Path(output)
        self.expected = expected
        self.delay = delay
        self.results = []
        self.stopped = threading.Event()
        self.connection = None
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        self.listener.bind((host, 0))
        self.port = self.listener.getsockname()[1]
        self.listener.listen(4)
        self.listener.settimeout(.2)
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()
        return self

    def close(self):
        self.stopped.set()
        self.listener.close()
        if self.connection:
            self.connection.close()
        self.thread.join(timeout=5)

    def run(self):
        expected = bytes((i * 29 + (i >> 8) * 11 + 0x5A) & 255 for i in range(2097152))
        while len(self.results) < self.expected and not self.stopped.is_set():
            try:
                connection, address = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.connection = connection
            record = {'peer': address[0], 'started': time.time()}
            received = bytearray()
            try:
                with connection:
                    connection.settimeout(30)
                    header = bytearray()
                    while len(header) < 12:
                        part = connection.recv(12 - len(header))
                        if not part:
                            raise EOFError('incomplete fixture header')
                        header += part
                    magic, length, suite = struct.unpack('<4sII', header)
                    assert magic == b'OSC1' and length == len(expected)
                    bits, nonblocking = suite & 65535, suite >> 16
                    assert bits in (32, 64) and nonblocking in (0, 1)
                    record.update(bits=bits, nonblocking=nonblocking, requested=length)
                    # The advertised small receive window fills during this pause.
                    self.stopped.wait(self.delay)
                    while len(received) < length and not self.stopped.is_set():
                        part = connection.recv(min(32768, length - len(received)))
                        if not part:
                            break
                        received += part
                    record['matches'] = bytes(received) == expected
                    if record['matches']:
                        connection.sendall(struct.pack('<4sI', b'PASS', len(received)))
            except (OSError, EOFError, AssertionError) as error:
                record['error'] = str(error)
            finally:
                self.connection = None
                record.update(received=len(received), sha256=hashlib.sha256(received).hexdigest(),
                              finished=time.time())
                self.results.append(record)
                self.output.write_text(json.dumps(self.results, indent=2) + '\n')
                print(json.dumps(record), flush=True)
