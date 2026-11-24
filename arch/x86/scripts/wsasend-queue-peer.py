"""Independent wire verification for queued Winsock send contract fixtures."""
import hashlib
import json
from pathlib import Path
import socket
import struct
import threading
import time


class Peer:
    def __init__(self, output, expected=12, delay=2, host='127.0.0.1'):
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
        length, tail = 2097152, 4096
        pattern = bytes((i * 29 + (i >> 8) * 11 + 0x5A) & 255
                        for i in range(length + tail * 3))
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
                    connection.settimeout(35)
                    header = bytearray()
                    while len(header) < 12:
                        part = connection.recv(12 - len(header))
                        if not part:
                            raise EOFError('incomplete fixture header')
                        header += part
                    magic, declared, suite = struct.unpack('<4sII', header)
                    bits, mode = suite & 65535, suite >> 16
                    assert magic == b'OSQ1' and declared == length
                    assert bits in (32, 64) and mode in range(6)
                    record.update(bits=bits, mode=mode)
                    wanted = (pattern[:length + tail * 2] if mode == 0 else
                              pattern[:length] + pattern[length + tail:] if mode == 1 else
                              pattern[:length])
                    self.stopped.wait(self.delay)
                    # A cancelled/closed send has no acknowledgement phase.
                    # A delayed FIN must not prevent serving the next fixture.
                    connection.settimeout(2 if mode in (2, 3, 5) else 35)
                    reset = False
                    idle_cutoff = False
                    while not self.stopped.is_set():
                        try:
                            part = connection.recv(32768)
                        except ConnectionResetError:
                            reset = True
                            break
                        except socket.timeout:
                            if mode not in (2, 3, 5):
                                raise
                            idle_cutoff = True
                            break
                        if not part:
                            break
                        received += part
                        assert len(received) <= length * 2 + tail * 3
                    if mode in (2, 3, 5):
                        record['matches'] = len(received) <= length and received == pattern[:len(received)]
                        record['reset'] = reset
                        record['idle_cutoff'] = idle_cutoff
                    else:
                        if mode == 1:
                            cancelled_length = len(received) - length - tail * 2
                            record['cancelled_prefix'] = cancelled_length
                            record['matches'] = (0 <= cancelled_length <= length and
                                received == pattern[:length] + pattern[:cancelled_length] +
                                            pattern[length + tail:])
                        else:
                            record['matches'] = bytes(received) == wanted
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
