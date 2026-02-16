#!/usr/bin/env python3
"""
OsitoK binary file upload tool.

Protocol:
  1. Send: fs upload <name> <size>\r\n
  2. Wait for: READY\n
  3. For each 4096-byte sector:
     a. Send sector data
     b. Wait for '#' ACK
  4. Wait for: OK 0x<crc16>\n

Usage:
  py tools/upload.py COM4 localfile.bin remotename.bin
  py tools/upload.py COM4 localfile.bin remotename.bin --baud 74880
"""
import serial
import sys
import time
import os


def crc16_ccitt(data):
    """CRC16-CCITT matching OsitoK's fs_crc16."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def main():
    if len(sys.argv) < 4:
        print("Usage: py tools/upload.py <port> <local_file> <remote_name> [--baud N]")
        print("Example: py tools/upload.py COM4 game.bin game.bin --baud 74880")
        sys.exit(1)

    port = sys.argv[1]
    local_file = sys.argv[2]
    remote_name = sys.argv[3]

    baud = 74880
    if "--baud" in sys.argv:
        idx = sys.argv.index("--baud")
        baud = int(sys.argv[idx + 1])

    # Read file
    data = open(local_file, "rb").read()
    size = len(data)
    if size == 0:
        print("Error: file is empty")
        sys.exit(1)

    print(f"File: {local_file} ({size} bytes)")
    print(f"Remote: {remote_name}")
    print(f"Port: {port} @ {baud} baud")

    # Calculate expected CRC
    expected_crc = crc16_ccitt(data)
    print(f"CRC16: 0x{expected_crc:04x}")

    sectors = (size + 4095) // 4096
    print(f"Sectors: {sectors}")

    # Open serial
    ser = serial.Serial(port, baud, timeout=5)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.5)  # let port settle

    # Drain any pending data
    ser.reset_input_buffer()

    # Send upload command
    cmd = f"fs upload {remote_name} {size}\r\n"
    print(f"Sending: {cmd.strip()}")
    ser.write(cmd.encode("ascii"))

    # Wait for READY
    deadline = time.time() + 10
    ready = False
    while time.time() < deadline:
        line = ser.readline()
        if line:
            text = line.decode("ascii", errors="replace").strip()
            if "READY" in text:
                ready = True
                break
            elif "ERR" in text or "not" in text or "full" in text:
                print(f"Error from device: {text}")
                ser.close()
                sys.exit(1)

    if not ready:
        print("Timeout waiting for READY")
        ser.close()
        sys.exit(1)

    print("Device ready, uploading...")

    # Send data sector by sector
    offset = 0
    for sec in range(sectors):
        chunk_size = min(4096, size - offset)
        chunk = data[offset : offset + chunk_size]

        ser.write(chunk)

        # Wait for '#' ACK
        deadline = time.time() + 15
        acked = False
        while time.time() < deadline:
            b = ser.read(1)
            if b == b"#":
                acked = True
                break

        if not acked:
            print(f"\nTimeout waiting for ACK on sector {sec}")
            ser.close()
            sys.exit(1)

        offset += chunk_size
        pct = offset * 100 // size
        print(f"\r  [{pct:3d}%] {offset}/{size} bytes", end="", flush=True)

    print()  # newline after progress

    # Wait for OK line
    deadline = time.time() + 10
    result = ""
    while time.time() < deadline:
        line = ser.readline()
        if line:
            text = line.decode("ascii", errors="replace").strip()
            if text.startswith("OK"):
                result = text
                break
            elif text.startswith("ERR"):
                print(f"Upload error: {text}")
                ser.close()
                sys.exit(1)

    if not result:
        print("Timeout waiting for final OK")
        ser.close()
        sys.exit(1)

    # Verify CRC
    # Result format: "OK 0x<hex>"
    parts = result.split()
    if len(parts) >= 2:
        device_crc = int(parts[1], 16)
        if device_crc == expected_crc:
            print(f"OK! CRC match: 0x{expected_crc:04x}")
        else:
            print(f"CRC MISMATCH! Expected 0x{expected_crc:04x}, got 0x{device_crc:04x}")
            ser.close()
            sys.exit(1)
    else:
        print(f"Upload complete: {result}")

    ser.close()


if __name__ == "__main__":
    main()
