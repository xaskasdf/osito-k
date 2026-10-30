"""Retain client/nonclient DCs through resize and repeated window destruction."""
# Helpers and the evidence record are provided by test-gui-desktop-qemu.py.
assert fixture.name in ("gdi-surface32.exe", "gdi-surface64.exe")
start = serial.stat().st_size
for byte in b"winexec /guiwm.exe\r":
    uart.sendall(bytes([byte]))
    time.sleep(.03)
data = gui_wait_serial(b"GDI SURFACE CONTRACT PASS:", start, 120)
data = gui_wait_serial(b"Execution finished, exit code = 0", start, 30)
assert b"GDI SURFACE FAIL:" not in data and b"GDI SURFACE MISSING:" not in data
match = re.search(rb"GDI SURFACE CONTRACT PASS: (\d+) checks", data)
assert match and int(match[1]) == 1060, data[-3000:]
gui_wait_serial(b"osito> ", start, 30)
record["passed"].append("retained GDI surfaces: 1060 checks, shell returned")
gui_qemu_capture("gdi-surfaces-returned")
print(json.dumps({"fixture": fixture.name, "checks": 1060}), flush=True)
