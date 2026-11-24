"""Native SHM references through the public syscall ABI, with desktop active."""
assert fixture.name == "shm-lifetime.elf"
start = serial.stat().st_size
for byte in b"exec /guiwm.exe\r":
    uart.sendall(bytes([byte]))
    time.sleep(.03)
data = gui_wait_serial(b"SHM LIFETIME PASS: 1669 checks", start, 90)
assert b"SHM LIFETIME FAIL" not in data
gui_wait_serial(b"osito> ", start, 30)
record["passed"].append("native SHM lifetime: 1669 checks, shell returned")
gui_qemu_capture("shm-lifetime-returned")
