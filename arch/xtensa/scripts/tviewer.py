#!/usr/bin/env python3
"""
OsitoK Terminal Video Bridge — 128x64 framebuffer in pure terminal (SSH-safe).

Renders the 1bpp framebuffer using Unicode half-block characters.
Each character cell = 1x2 pixels -> 128 cols x 32 rows.
Console text appears below. Keyboard input forwarded to serial.

Protocol: sync header 0x00 0xFF 0x00 0xFF + 1024 bytes raw 1bpp framebuffer.

Usage:
    python3 tools/tviewer.py                        # /dev/ttyUSB0 @ 74880
    python3 tools/tviewer.py /dev/ttyUSB0           # custom port
    python3 tools/tviewer.py /dev/ttyUSB0 115200    # custom baud

Exit: Ctrl+C or Ctrl+]
Reset: Ctrl+R
"""

import sys
import os
import signal
import time
import threading
import select
import locale
import io

# Fix broken LC_CTYPE from SSH (e.g. "UTF-8" instead of "es_CL.UTF-8")
if os.environ.get("LC_CTYPE") == "UTF-8":
    os.environ["LC_CTYPE"] = "en_US.UTF-8"
try:
    locale.setlocale(locale.LC_ALL, "")
except locale.Error:
    pass

try:
    import serial
except ImportError:
    print("error: pyserial required (pip install pyserial)", file=sys.stderr)
    sys.exit(1)

import tty
import termios

# Framebuffer
FB_WIDTH  = 128
FB_HEIGHT = 64
FB_SIZE   = 1024
FB_STRIDE = 16  # bytes per row (128/8)

# Display: each char = 1x2 pixels -> 128 cols x 32 rows
DISP_COLS = FB_WIDTH
DISP_ROWS = FB_HEIGHT // 2

# Sync header
SYNC = bytes([0x00, 0xFF, 0x00, 0xFF])

# ANSI escapes
CSI        = "\033["
HIDE_CUR   = CSI + "?25l"
SHOW_CUR   = CSI + "?25h"
CLEAR      = CSI + "2J"
HOME       = CSI + "H"
RST        = CSI + "0m"
GREEN_FG   = CSI + "32m"
BLACK_BG   = CSI + "40m"
DIM        = CSI + "2m"
BOLD       = CSI + "1m"
CYAN       = CSI + "36m"
ERASE_EOL  = CSI + "K"

# Newline in raw mode must be \r\n
NL = "\r\n"

# Console text ring buffer
CONSOLE_LINES = 8


def safe_write(s):
    """Write to stdout, silently ignore BrokenPipe."""
    try:
        os.write(sys.stdout.fileno(), s.encode("utf-8"))
    except (BrokenPipeError, OSError):
        pass


def render_frame(data):
    """Convert 1024-byte 1bpp framebuffer to terminal string using half-blocks."""
    parts = []

    # Top border
    title = " OsitoK 128x64 "
    pad = DISP_COLS - len(title)
    left = pad // 2
    right = pad - left
    parts.append(f"{DIM}\u250c{'\u2500' * left}{RST}{CYAN}{BOLD}{title}{RST}{DIM}{'\u2500' * right}\u2510{RST}")

    # Framebuffer rows (2 pixel rows per character row)
    for char_row in range(DISP_ROWS):
        y_top = char_row * 2
        y_bot = y_top + 1

        row_chars = []
        for x in range(FB_WIDTH):
            byte_idx = x >> 3
            bit_mask = 0x80 >> (x & 7)

            top = data[y_top * FB_STRIDE + byte_idx] & bit_mask
            bot = data[y_bot * FB_STRIDE + byte_idx] & bit_mask

            if top and bot:
                row_chars.append("\u2588")   # full block
            elif top:
                row_chars.append("\u2580")   # upper half
            elif bot:
                row_chars.append("\u2584")   # lower half
            else:
                row_chars.append(" ")

        parts.append(f"{DIM}\u2502{RST}{GREEN_FG}{BLACK_BG}{''.join(row_chars)}{RST}{DIM}\u2502{RST}")

    # Bottom border
    parts.append(f"{DIM}\u2514{'\u2500' * DISP_COLS}\u2518{RST}")

    return NL.join(parts)


def render_console(console_buf, console_idx):
    """Render the console text area."""
    parts = []
    parts.append(f"{DIM}{'\u2500' * (DISP_COLS + 2)} console{RST}")
    for i in range(CONSOLE_LINES):
        idx = (console_idx - CONSOLE_LINES + 1 + i) % CONSOLE_LINES
        text = console_buf[idx][:DISP_COLS]
        parts.append(f" {text}{ERASE_EOL}")
    return NL.join(parts)


class TerminalViewer:
    def __init__(self, port, baud):
        self.port = port
        self.baud = baud
        self.ser = None
        self.running = False
        self.frame_count = 0
        self.fps = 0.0
        self.last_fps_time = time.time()
        self.last_fps_count = 0
        self.lock = threading.Lock()

        # Sync state machine
        self.sync_state = 0
        self.collecting_frame = False
        self.frame_buf = bytearray()

        # Current framebuffer for display
        self.current_fb = bytearray(FB_SIZE)
        self.fb_dirty = True

        # Console text
        self.console_buf = [""] * CONSOLE_LINES
        self.console_idx = 0
        self.console_col = 0

    def connect(self):
        try:
            self.ser = serial.Serial(
                port=self.port,
                baudrate=self.baud,
                timeout=0.01,
                dsrdtr=False,
                rtscts=False,
            )
            self.ser.dtr = False
            self.ser.rts = False
            return True
        except serial.SerialException as e:
            print(f"ERROR: Cannot open {self.port}: {e}", file=sys.stderr)
            return False

    def console_add_char(self, ch):
        """Add a character to the console text buffer."""
        if ch == '\n' or ch == '\r':
            if ch == '\n':
                self.console_idx = (self.console_idx + 1) % CONSOLE_LINES
                self.console_buf[self.console_idx] = ""
                self.console_col = 0
        elif ch == '\t':
            self.console_buf[self.console_idx] += "    "
            self.console_col += 4
        elif 0x20 <= ord(ch) < 0x7F:
            self.console_buf[self.console_idx] += ch
            self.console_col += 1
            if self.console_col >= DISP_COLS:
                self.console_idx = (self.console_idx + 1) % CONSOLE_LINES
                self.console_buf[self.console_idx] = ""
                self.console_col = 0

    def process_byte(self, byte):
        """Process one byte from serial - sync detection + frame collection."""
        if self.collecting_frame:
            self.frame_buf.append(byte)
            if len(self.frame_buf) >= FB_SIZE:
                with self.lock:
                    self.current_fb[:] = self.frame_buf
                    self.fb_dirty = True
                    self.frame_count += 1
                self.collecting_frame = False
                self.sync_state = 0
                self.frame_buf = bytearray()
        else:
            expected = SYNC[self.sync_state]
            if byte == expected:
                self.sync_state += 1
                if self.sync_state >= len(SYNC):
                    self.collecting_frame = True
                    self.frame_buf = bytearray()
            else:
                for i in range(self.sync_state):
                    b = SYNC[i]
                    if 0x20 <= b < 0x7F or b in (0x0A, 0x0D, 0x09):
                        self.console_add_char(chr(b))
                self.sync_state = 0

                if byte == SYNC[0]:
                    self.sync_state = 1
                else:
                    if 0x20 <= byte < 0x7F or byte in (0x0A, 0x0D, 0x09):
                        self.console_add_char(chr(byte))

    def reader_thread(self):
        """Read serial data in background."""
        while self.running:
            try:
                data = self.ser.read(4096)
                if data:
                    for b in data:
                        self.process_byte(b)
            except serial.SerialException:
                self.running = False
                break
            except Exception:
                break

    def display_thread(self):
        """Refresh terminal display at ~30 Hz."""
        while self.running:
            now = time.time()
            if now - self.last_fps_time >= 1.0:
                self.fps = (self.frame_count - self.last_fps_count) / (now - self.last_fps_time)
                self.last_fps_count = self.frame_count
                self.last_fps_time = now
                self.fb_dirty = True

            with self.lock:
                dirty = self.fb_dirty
                if dirty:
                    fb_copy = bytes(self.current_fb)
                    self.fb_dirty = False

            if dirty:
                frame_str = render_frame(fb_copy)
                con_str = render_console(self.console_buf, self.console_idx)
                status = f"{DIM} frames: {self.frame_count}  fps: {self.fps:.1f}  port: {self.port}  Ctrl+C=exit Ctrl+R=reset{RST}{ERASE_EOL}"

                safe_write(HOME + frame_str + NL + con_str + NL + status)

            time.sleep(0.033)

    def run(self):
        # Pre-flight checks
        if not sys.stdin.isatty() or not sys.stdout.isatty():
            print("error: not a terminal. Run directly:", file=sys.stderr)
            print(f"  python3 tools/tviewer.py {self.port} {self.baud}", file=sys.stderr)
            return 1

        if not self.connect():
            return 1

        fd = sys.stdin.fileno()
        old_settings = termios.tcgetattr(fd)

        def restore_term():
            """Restore terminal to sane state."""
            try:
                termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
            except Exception:
                pass
            safe_write(SHOW_CUR + RST + NL)

        # Handle SIGINT/SIGTERM gracefully
        def sig_handler(signum, frame):
            self.running = False

        signal.signal(signal.SIGINT, sig_handler)
        signal.signal(signal.SIGTERM, sig_handler)

        try:
            tty.setraw(fd)
            safe_write(HIDE_CUR + CLEAR + HOME)

            self.running = True

            reader = threading.Thread(target=self.reader_thread, daemon=True)
            reader.start()

            display = threading.Thread(target=self.display_thread, daemon=True)
            display.start()

            # Main thread: keyboard input
            while self.running:
                if select.select([sys.stdin], [], [], 0.05)[0]:
                    ch = sys.stdin.buffer.read(1)
                    if not ch:
                        continue

                    b = ch[0]

                    # Ctrl+] or Ctrl+C = exit
                    if b in (0x1D, 0x03):
                        break
                    # Ctrl+R = reset board
                    if b == 0x12:
                        self.ser.dtr = True
                        time.sleep(0.1)
                        self.ser.dtr = False
                        continue

                    try:
                        self.ser.write(ch)
                    except serial.SerialException:
                        break

        finally:
            self.running = False
            time.sleep(0.05)
            restore_term()

            if self.ser and self.ser.is_open:
                self.ser.close()

            print(f"{self.frame_count} frames received")

        return 0


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 74880

    print(f"OsitoK Terminal Video Bridge")
    print(f"  Port: {port} @ {baud} baud")
    print(f"  Display: {DISP_COLS}x{DISP_ROWS} chars (half-block, green phosphor)")
    print(f"  Ctrl+C=exit  Ctrl+R=reset  Keyboard -> serial")
    print()
    time.sleep(0.3)

    viewer = TerminalViewer(port, baud)
    sys.exit(viewer.run())


if __name__ == "__main__":
    main()
