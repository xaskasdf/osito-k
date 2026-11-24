"""Run the existing PE32 cursor-count/subclass fixture on the desktop runner."""
# UART, serial checks, capture helper and record come from the QEMU runner.
assert fixture.name == "subclass_pe32.exe"
start = serial.stat().st_size
for byte in b"winexec /guiwm.exe\r":
    uart.sendall(bytes([byte]))
    time.sleep(.03)
data = gui_wait_serial(b"[SUBCLASS] PASS", start, 90)
assert b"[SUBCLASS] FAIL" not in data, data[-4000:]
gui_wait_serial(b"osito> ", start, 30)
record["passed"].append("PE32 subclass and thread-local cursor counts")
gui_qemu_capture("cursor-fixture-completed")
print(json.dumps({"fixture": fixture.name, "passed": True}), flush=True)
