"""
+============================================================+
|          OsitoK OPERATOR CONSOLE  -  Rev 1.0               |
|          Serial Terminal for the LX106 Kernel               |
+============================================================+

Usage:
    py tools/console.py                   # COM4 @ 74880 (default)
    py tools/console.py COM3              # custom port
    py tools/console.py COM4 115200       # custom baud

Exit:  Ctrl+]  or  Ctrl+C
Reset: Ctrl+R  (toggle DTR to reset board)
"""

import sys
import time
import threading
import argparse

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed.")
    print("  Install with:  py -m pip install pyserial")
    sys.exit(1)

# -- ANSI colors (for Windows, enable VT100 first) --

def enable_vt100():
    """Enable ANSI escape codes on Windows 10+ terminal."""
    if sys.platform == "win32":
        try:
            import ctypes
            k32 = ctypes.windll.kernel32
            handle = k32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
            mode = ctypes.c_ulong()
            k32.GetConsoleMode(handle, ctypes.byref(mode))
            k32.SetConsoleMode(handle, mode.value | 0x0004)  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
        except Exception:
            pass

DIM    = "\033[2m"
BOLD   = "\033[1m"
GREEN  = "\033[32m"
YELLOW = "\033[33m"
CYAN   = "\033[36m"
RED    = "\033[31m"
RESET  = "\033[0m"

BANNER = f"""{CYAN}{BOLD}
+============================================================+
|          OsitoK OPERATOR CONSOLE  -  Rev 1.0               |
|          Serial Terminal for the LX106 Kernel               |
+============================================================+{RESET}
"""

# -- Terminal raw mode (cross-platform) --

class RawTerminal:
    """Context manager for raw terminal input (no echo, no line buffering)."""

    def __init__(self):
        self._old = None

    def __enter__(self):
        if sys.platform == "win32":
            try:
                import msvcrt
                self._msvcrt = msvcrt
            except ImportError:
                pass
        else:
            import tty, termios
            self._fd = sys.stdin.fileno()
            self._old = termios.tcgetattr(self._fd)
            tty.setraw(self._fd)
        return self

    def __exit__(self, *args):
        if self._old is not None:
            import termios
            termios.tcsetattr(self._fd, termios.TCSADRAIN, self._old)

    def getch(self):
        """Read a single character. Returns bytes."""
        if sys.platform == "win32":
            import msvcrt
            ch = msvcrt.getch()
            return ch
        else:
            return sys.stdin.buffer.read(1)


# -- Main console --

class Console:
    def __init__(self, port, baud):
        self.port = port
        self.baud = baud
        self.ser = None
        self.running = False
        self.bytes_rx = 0
        self.bytes_tx = 0
        self.t_start = time.time()

    def connect(self):
        """Open serial port."""
        try:
            self.ser = serial.Serial(
                port=self.port,
                baudrate=self.baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.05,
                # Don't toggle DTR/RTS on connect (avoids board reset)
                dsrdtr=False,
                rtscts=False,
            )
            # Explicitly set DTR/RTS low after open
            self.ser.dtr = False
            self.ser.rts = False
            return True
        except serial.SerialException as e:
            print(f"{RED}ERROR: Cannot open {self.port}: {e}{RESET}")
            return False

    def reader_thread(self):
        """Background thread: read from serial, write to stdout."""
        while self.running:
            try:
                data = self.ser.read(256)
                if data:
                    self.bytes_rx += len(data)
                    # Decode with replacement for garbled boot bytes
                    text = data.decode("ascii", errors="replace")
                    sys.stdout.write(text)
                    sys.stdout.flush()
            except serial.SerialException:
                if self.running:
                    sys.stdout.write(f"\n{RED}--- Serial port disconnected ---{RESET}\n")
                    sys.stdout.flush()
                    self.running = False
                break
            except Exception:
                if self.running:
                    break

    def reset_board(self):
        """Toggle DTR to reset the board."""
        sys.stdout.write(f"\n{YELLOW}--- Resetting board ---{RESET}\n")
        sys.stdout.flush()
        self.ser.dtr = True
        time.sleep(0.1)
        self.ser.dtr = False

    def print_stats(self):
        """Print session statistics on exit."""
        elapsed = time.time() - self.t_start
        mins = int(elapsed) // 60
        secs = int(elapsed) % 60
        print(f"\n{DIM}--- Session: {mins}m {secs}s | RX: {self.bytes_rx} bytes | TX: {self.bytes_tx} bytes ---{RESET}")

    def run(self):
        """Main loop: read keyboard, send to serial."""
        print(BANNER)
        print(f"  {DIM}Port:{RESET} {BOLD}{self.port}{RESET}")
        print(f"  {DIM}Baud:{RESET} {BOLD}{self.baud}{RESET}")
        print(f"  {DIM}Exit:{RESET} Ctrl+]   {DIM}Reset:{RESET} Ctrl+R")
        print(f"{DIM}  ────────────────────────────────────{RESET}")
        print()

        if not self.connect():
            return 1

        self.running = True
        self.t_start = time.time()

        # Start reader thread
        reader = threading.Thread(target=self.reader_thread, daemon=True)
        reader.start()

        # Main thread: keyboard input
        raw = RawTerminal()
        with raw:
            try:
                while self.running:
                    ch = raw.getch()
                    if not ch:
                        continue

                    b = ch[0] if isinstance(ch[0], int) else ord(ch[0])

                    # Ctrl+] (0x1D) = exit
                    if b == 0x1D:
                        break

                    # Ctrl+C (0x03) = exit
                    if b == 0x03:
                        break

                    # Ctrl+R (0x12) = reset board
                    if b == 0x12:
                        self.reset_board()
                        continue

                    # Send byte to serial
                    try:
                        self.ser.write(ch)
                        self.bytes_tx += 1
                    except serial.SerialException:
                        break

            except KeyboardInterrupt:
                pass

        self.running = False
        reader.join(timeout=1.0)

        if self.ser and self.ser.is_open:
            self.ser.close()

        self.print_stats()
        return 0


def main():
    enable_vt100()

    parser = argparse.ArgumentParser(
        description="OsitoK Operator Console - Serial terminal for the LX106 kernel",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="  Ctrl+]  Exit    Ctrl+R  Reset board    Ctrl+C  Exit",
    )
    parser.add_argument("port", nargs="?", default="COM4",
                        help="Serial port (default: COM4)")
    parser.add_argument("baud", nargs="?", type=int, default=74880,
                        help="Baud rate (default: 74880)")

    args = parser.parse_args()

    console = Console(args.port, args.baud)
    sys.exit(console.run())


if __name__ == "__main__":
    main()
