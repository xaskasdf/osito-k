"""Run an existing GDI/USER32 paint PE fixture on the desktop QEMU runner.

Set OSITO_GUI_FIXTURE_DRIVER to this path and pass the PE as the fourth runner
argument. Runner globals supply the UART, QMP helpers and evidence record.
"""
pattern = (rb"\[USER32-PAINT\] checks=(\d+) failures=0 PASS"
           if fixture.name.startswith("user32_paint_") else
           rb"\[GDI-DIB\] checks=0x([0-9A-Fa-f]+) failures=0x0+ PASS")
start = serial.stat().st_size
for byte in b"winexec /guiwm.exe\r":
    uart.sendall(bytes([byte]))
    time.sleep(.03)
data = gui_wait_serial(b" PASS", start, 90)
match = re.search(pattern, data)
assert match, data[-4000:]
assert b"FAIL" not in data, data[-4000:]
record["passed"].append(match.group().decode())
record["pe_checks"] = int(match[1], 10 if b"USER32" in match.group() else 16)
gui_wait_serial(b"osito> ", start, 30)
gui_qemu_capture("gdi-fixture-completed")
print(json.dumps({"fixture": fixture.name, "checks": record["pe_checks"]}), flush=True)
