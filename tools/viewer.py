#!/usr/bin/env python3
"""
OsitoK Video Bridge — Renders 128x64 framebuffer from UART in a pygame window.

Protocol: scans serial stream for sync header (0x00 0xFF 0x00 0xFF),
then reads 1024 bytes as a raw 128x64 1bpp framebuffer.
Non-frame bytes are printed to stdout as serial console text.
Keyboard input is forwarded to serial for shell/game control.

Usage: py tools/viewer.py [COM_PORT] [BAUD]
  Default: COM4, 74880
"""

import sys
import time

try:
    import serial
except ImportError:
    print("error: pyserial required (pip install pyserial)", file=sys.stderr)
    sys.exit(1)

try:
    import pygame
except ImportError:
    print("error: pygame required (pip install pygame)", file=sys.stderr)
    sys.exit(1)

# Framebuffer dimensions
FB_WIDTH = 128
FB_HEIGHT = 64
FB_SIZE = 1024
SCALE = 4
WIN_WIDTH = FB_WIDTH * SCALE
WIN_HEIGHT = FB_HEIGHT * SCALE

# Sync header
SYNC = bytes([0x00, 0xFF, 0x00, 0xFF])

# Colors
COLOR_ON = (0, 255, 68)    # Green phosphor
COLOR_OFF = (0, 0, 0)

def decode_framebuffer(data, surface):
    """Decode 1KB 1bpp framebuffer into pygame surface."""
    pixels = pygame.surfarray.pixels2d(surface)
    on = surface.map_rgb(COLOR_ON)
    off = surface.map_rgb(COLOR_OFF)

    for y in range(FB_HEIGHT):
        row_offset = y * 16
        for byte_idx in range(16):
            byte_val = data[row_offset + byte_idx]
            for bit in range(8):
                x = byte_idx * 8 + bit
                if byte_val & (0x80 >> bit):
                    pixels[x][y] = on
                else:
                    pixels[x][y] = off

    del pixels


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 74880

    print(f"OsitoK Video Bridge")
    print(f"  Port: {port} @ {baud} baud")
    print(f"  Window: {WIN_WIDTH}x{WIN_HEIGHT} (framebuffer {FB_WIDTH}x{FB_HEIGHT} x{SCALE})")
    print(f"  Keyboard input forwarded to serial")
    print(f"  Press Ctrl+C or close window to exit")
    print()

    # Open serial
    ser = serial.Serial(port, baud, timeout=0)
    ser.dtr = False
    ser.rts = False

    # Init pygame
    pygame.init()
    screen = pygame.display.set_mode((WIN_WIDTH, WIN_HEIGHT))
    pygame.display.set_caption("OsitoK Video Bridge")

    # Small surface for framebuffer (will be scaled up)
    fb_surface = pygame.Surface((FB_WIDTH, FB_HEIGHT))
    fb_surface.fill(COLOR_OFF)

    clock = pygame.time.Clock()
    frame_count = 0

    # State machine for sync detection
    sync_state = 0       # how many sync bytes matched
    frame_buf = bytearray()
    collecting_frame = False

    running = True
    while running:
        # Handle pygame events
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN:
                # Forward key to serial
                if event.key == pygame.K_RETURN:
                    ser.write(b'\r')
                elif event.key == pygame.K_BACKSPACE:
                    ser.write(b'\x7f')
                elif event.key == pygame.K_ESCAPE:
                    ser.write(b'\x1b')
                elif event.unicode and ord(event.unicode) < 128:
                    ser.write(event.unicode.encode('ascii'))

        # Read serial data
        data = ser.read(4096)
        if data:
            for byte in data:
                if collecting_frame:
                    frame_buf.append(byte)
                    if len(frame_buf) >= FB_SIZE:
                        # Full frame received
                        decode_framebuffer(frame_buf, fb_surface)
                        scaled = pygame.transform.scale(fb_surface, (WIN_WIDTH, WIN_HEIGHT))
                        screen.blit(scaled, (0, 0))
                        pygame.display.flip()
                        frame_count += 1
                        collecting_frame = False
                        sync_state = 0
                        frame_buf = bytearray()
                else:
                    # Sync detection state machine
                    expected = SYNC[sync_state]
                    if byte == expected:
                        sync_state += 1
                        if sync_state >= len(SYNC):
                            # Full sync found — start collecting frame
                            collecting_frame = True
                            frame_buf = bytearray()
                    else:
                        # Output buffered sync bytes as text
                        for i in range(sync_state):
                            b = SYNC[i]
                            if 0x20 <= b < 0x7F or b == 0x0A or b == 0x0D:
                                sys.stdout.write(chr(b))
                        sync_state = 0

                        # Check if current byte starts a new sync
                        if byte == SYNC[0]:
                            sync_state = 1
                        else:
                            # Output as console text
                            if 0x20 <= byte < 0x7F or byte == 0x0A or byte == 0x0D or byte == 0x09:
                                sys.stdout.write(chr(byte))
                                sys.stdout.flush()

        clock.tick(60)

    pygame.quit()
    ser.close()
    print(f"\n{frame_count} frames received")


if __name__ == '__main__':
    main()
