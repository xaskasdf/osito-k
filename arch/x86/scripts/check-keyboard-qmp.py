#!/usr/bin/env python3
"""Type into a booted QEMU desktop terminal; verify output via serial log.

Requires an idle, focused terminal with Caps Lock off. Run once with PS/2
devices and once with USB devices. No serial input injection is used here.
"""

import argparse
import json
from pathlib import Path
import socket
import time


def check_keyboard(qmp_path, serial_log):
    def qmp(command, arguments):
        with socket.socket(socket.AF_UNIX) as channel:
            channel.settimeout(15)
            channel.connect(str(qmp_path))
            stream = channel.makefile("rwb", buffering=0)
            json.loads(stream.readline())
            for request in ({"execute": "qmp_capabilities"},
                            {"execute": command, "arguments": arguments}):
                stream.write(json.dumps(request).encode() + b"\n")
                while True:
                    reply = json.loads(stream.readline())
                    if "error" in reply:
                        raise RuntimeError(reply["error"])
                    if "return" in reply:
                        break

    held = set()

    def transition(code, down):
        qmp("input-send-event", {"events": [{"type": "key", "data": {
            "down": down, "key": {"type": "qcode", "data": code}}}]})
        if down:
            held.add(code)
        else:
            held.discard(code)
        time.sleep(0.08)

    def tap(code):
        transition(code, True)
        transition(code, False)

    def type_text(text):
        codes = {" ": "spc", "/": "slash", "-": "minus", "=": "equal",
                 ",": "comma", ".": "dot", "[": "bracket_left",
                 "]": "bracket_right", ";": "semicolon"}
        for char in text:
            tap(codes.get(char, char))

    results = []

    def expect(name, expected, type_input):
        start = serial_log.stat().st_size
        type_input()
        tap("ret")
        deadline = time.monotonic() + 30
        marker = b"\n" + expected.encode() + b"\n"
        while time.monotonic() < deadline:
            raw = serial_log.read_bytes()[start:].replace(b"\r", b"")
            position = raw.find(marker)
            if position >= 0 and b"osito> " in raw[position + len(marker):]:
                results.append(name)
                print("PASS desktop keyboard: " + name, flush=True)
                return
            time.sleep(0.1)
        raise AssertionError(f"{name}: expected {expected!r}; serial tail {raw[-2000:]!r}")

    try:
        alphabet = "qwertyuiop asdfghjkl zxcvbnm 0123456789"
        expect("physical rows and digits", alphabet, lambda: type_text("echo " + alphabet))
        expect("punctuation", "/-=,.[ ];", lambda: type_text("echo /-=,.[ ];"))

        def shifted():
            type_text("echo ")
            transition("shift", True)
            transition("shift_r", True)
            transition("shift", False)
            tap("q")
            transition("shift_r", False)
            tap("w")

        expect("independent left/right Shift", "Qw", shifted)

        def caps():
            type_text("echo ")
            tap("caps_lock")
            tap("q")
            transition("shift", True)
            tap("w")
            transition("shift", False)
            tap("e")
            tap("caps_lock")

        expect("Caps Lock and Shift inversion", "QwE", caps)

        def navigation():
            type_text("echo acx")
            tap("backspace")
            tap("left")
            tap("b")
            tap("right")

        expect("Backspace and extended arrows", "abc", navigation)

        def delete_home_end():
            type_text("echo axbc")
            tap("home")
            for _ in range(6):
                tap("right")
            tap("delete")
            tap("end")
            tap("d")

        expect("Home, End and Delete", "abcd", delete_home_end)

        def ctrl_u():
            type_text("echo discarded")
            transition("ctrl", True)
            transition("ctrl_r", True)
            transition("ctrl", False)
            tap("u")
            transition("ctrl_r", False)
            type_text("echo retained")

        expect("independent left/right Ctrl and line clearing", "retained", ctrl_u)

        def alt_l():
            type_text("echo a")
            transition("alt", True)
            tap("l")
            transition("alt", False)
            tap("b")

        expect("Alt+L does not switch focus", "alb", alt_l)

        def alt_tab():
            for _ in range(2):
                transition("alt", True)
                tap("tab")
                transition("alt", False)
            type_text("echo focused")

        expect("Alt+Tab returns to terminal", "focused", alt_tab)
    finally:
        for code in list(held):
            transition(code, False)
    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qmp", type=Path, required=True)
    parser.add_argument("--serial-log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    results = check_keyboard(args.qmp, args.serial_log)
    with args.output.open("x") as output:
        json.dump({"passed": results}, output, indent=2)
