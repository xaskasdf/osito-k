#!/usr/bin/env python3
"""Windows public-API oracle and deliberate wire-corruption negative control."""
import argparse
import importlib.util
import json
from pathlib import Path
import socket
import subprocess
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixtures', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--corrupt-at', type=int, default=-1)
    args = parser.parse_args()
    args.output.mkdir(exist_ok=False)
    spec = importlib.util.spec_from_file_location('stream_server',
                                                 Path(__file__).with_name('winsock-stream-server.py'))
    server = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(server)
    results = []
    for bits in (32, 64):
        for mode in (0, 1):
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                listener.listen(1)
                listener.settimeout(10)
                state = {}
                def serve():
                    try:
                        conn, _ = listener.accept()
                        with conn:
                            state['result'] = server.transfer(conn, corrupt_at=args.corrupt_at)
                    except (OSError, RuntimeError, ValueError) as error:
                        state['error'] = str(error)
                worker = threading.Thread(target=serve)
                worker.start()
                try:
                    run = subprocess.run([str((args.fixtures / f'stream{bits}.exe').resolve()),
                                          '127.0.0.1', str(listener.getsockname()[1]), str(mode), '2'],
                                         capture_output=True, timeout=600)
                    (args.output / f'pe{bits}-mode{mode}.log').write_bytes(run.stdout + run.stderr)
                finally:
                    worker.join(65)
                assert not worker.is_alive(), 'server did not finish'
            if args.corrupt_at < 0:
                assert run.returncode == 0 and b'STREAM PRIVATE PASS' in run.stdout, run.stdout
                assert 'result' in state, state
            else:
                marker = f'STREAM MISMATCH offset={args.corrupt_at} '.encode()
                assert run.returncode != 0 and marker in run.stdout, run.stdout
                assert b'STREAM PRIVATE PASS' not in run.stdout, run.stdout
            results.append(dict(bits=bits, mode=mode, exit=run.returncode, **state))
            print(json.dumps(results[-1]), flush=True)
    (args.output / 'result.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
