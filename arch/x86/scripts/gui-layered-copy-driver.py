"""Check the existing layered-window ABI and retained pixels on the desktop."""
# Helpers and the evidence record are provided by test-gui-desktop-qemu.py.
assert fixture.name in ("layered32.exe", "layered64.exe")
start = serial.stat().st_size
for byte in b"winexec /guiwm.exe\r":
    uart.sendall(bytes([byte]))
    time.sleep(.03)
data = gui_wait_serial(b"LAYERED CONTRACT PASS", start, 90)
assert b"LAYERED FAIL:" not in data, data[-4000:]
gui_wait_serial(b"osito> ", start, 30)
record["passed"].append("layered public ABI contract")

start = serial.stat().st_size
for byte in b"winexec /guiwm.exe --visual\r":
    uart.sendall(bytes([byte]))
    time.sleep(.03)
data = gui_wait_serial(b"LAYERED VISUAL READY", start, 90)
assert b"LAYERED FAIL:" not in data, data[-4000:]
gui_qemu_capture("layered-retained-pixels")
data = (out / "layered-retained-pixels.ppm").read_bytes()
header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
assert header
image_width = int(header[1])
record["pixel_checks"] = []
for x, y, expected in (
    (34, 54, (32, 64, 96)),
    (48, 68, (255, 0, 0)), (80, 68, (16, 160, 48)),
    (48, 92, (32, 64, 96)), (80, 92, (0, 0, 255)),
    (148, 68, (144, 32, 48)), (180, 68, (24, 112, 72)),
    (148, 92, (32, 64, 96)), (180, 92, (16, 32, 176)),
    (248, 68, (32, 64, 96)), (280, 68, (0, 128, 0)),
    (248, 92, (0, 0, 0)), (280, 92, (0, 0, 255)),
):
    offset = header.end() + (y * image_width + x) * 3
    actual = tuple(data[offset:offset + 3])
    assert all(abs(a - b) <= 1 for a, b in zip(actual, expected)), (x, y, actual, expected)
    record["pixel_checks"].append({"xy": [x, y], "expected": expected, "actual": actual})
record["passed"].append("13 composited pixels after source DIB/DC destruction")
print(json.dumps({"fixture": fixture.name, "pixel_checks": len(record["pixel_checks"])}), flush=True)
