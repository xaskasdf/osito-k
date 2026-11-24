#!/usr/bin/env python3
"""Type an ASCII command through a QEMU human-monitor socket."""

import argparse
import base64
import socket
import time


SHIFTED = {
    "!": "shift-1",
    '"': "shift-apostrophe",
    "#": "shift-3",
    "$": "shift-4",
    "%": "shift-5",
    "&": "shift-7",
    "(": "shift-9",
    ")": "shift-0",
    "*": "shift-8",
    "+": "shift-equal",
    ":": "shift-semicolon",
    "<": "shift-comma",
    ">": "shift-dot",
    "?": "shift-slash",
    "@": "shift-2",
    "^": "shift-6",
    "_": "shift-minus",
    "{": "shift-bracket_left",
    "|": "shift-backslash",
    "}": "shift-bracket_right",
    "~": "shift-grave",
}

PLAIN = {
    " ": "spc",
    "'": "apostrophe",
    ",": "comma",
    "-": "minus",
    ".": "dot",
    "/": "slash",
    ";": "semicolon",
    "=": "equal",
    "[": "bracket_left",
    "\\": "backslash",
    "]": "bracket_right",
    "`": "grave",
}


def qemu_key(character):
    if "a" <= character <= "z" or "0" <= character <= "9":
        return character
    if "A" <= character <= "Z":
        return "shift-" + character.lower()
    if character in PLAIN:
        return PLAIN[character]
    if character in SHIFTED:
        return SHIFTED[character]
    raise ValueError("unsupported character: {!r}".format(character))


def connect(args):
    if args.unix:
        monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        monitor.connect(args.unix)
        return monitor
    host, separator, port = args.tcp.rpartition(":")
    if not separator or not host:
        raise ValueError("TCP endpoint must be HOST:PORT")
    return socket.create_connection((host, int(port)), timeout=5)


def drain(monitor):
    monitor.settimeout(0.05)
    try:
        while monitor.recv(4096):
            pass
    except socket.timeout:
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    endpoint = parser.add_mutually_exclusive_group(required=True)
    endpoint.add_argument("--unix", metavar="PATH")
    endpoint.add_argument("--tcp", metavar="HOST:PORT")
    parser.add_argument("--hold-ms", type=int, default=40)
    parser.add_argument("--interval-ms", type=int, default=100)
    parser.add_argument("--no-enter", action="store_true")
    parser.add_argument("--base64", action="store_true",
                        help="decode the text argument from base64")
    parser.add_argument("text")
    args = parser.parse_args()

    if args.base64:
        args.text = base64.b64decode(args.text).decode("ascii")

    monitor = connect(args)
    try:
        drain(monitor)
        for character in args.text:
            command = "sendkey {} {}\n".format(
                qemu_key(character), args.hold_ms)
            monitor.sendall(command.encode("ascii"))
            time.sleep(args.interval_ms / 1000.0)
        if not args.no_enter:
            monitor.sendall(
                "sendkey ret {}\n".format(args.hold_ms).encode("ascii"))
            time.sleep(args.interval_ms / 1000.0)
        drain(monitor)
    finally:
        monitor.close()


if __name__ == "__main__":
    main()
