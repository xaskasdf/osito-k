"""Run the DirectDraw lifetime or existing no-renderer WGL contract in QEMU."""
# Shared driver helpers are supplied by test-gui-desktop-qemu.py.
ddraw = fixture.name == "ddraw-surface32.exe"
assert ddraw or fixture.name in ("wgl_backend_pe32.exe", "wgl_backend_pe64.exe")
marker = b"DDRAW SURFACE CONTRACT PASS: 1638 checks" if ddraw else b"WGL-BACKEND PASS"
start = serial.stat().st_size
for byte in b"winexec /guiwm.exe\r":
    uart.sendall(bytes([byte]))
    time.sleep(.03)
gui_wait_serial(marker, start, 180)
data = gui_wait_serial(b"Execution finished, exit code = 0", start, 30)
assert b"DDRAW SURFACE FAIL:" not in data and b"DDRAW SURFACE MISSING:" not in data
assert b"WGL-BACKEND FAIL" not in data and b"WGL-BACKEND INVALID-SUCCESS" not in data
gui_wait_serial(b"osito> ", start, 30)
record["passed"].append("DirectDraw window surfaces: 1638 checks, shell returned" if ddraw
                        else "WGL no-renderer contract, shell returned")
gui_qemu_capture("present-surface-returned")
print(json.dumps({"fixture": fixture.name, "passed": record["passed"]}), flush=True)
