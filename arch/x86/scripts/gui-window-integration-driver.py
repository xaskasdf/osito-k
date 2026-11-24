"""Optional custom-frame scenario, loaded by test-gui-desktop-qemu.py."""
# Helpers, QMP connection, record and scratch paths are supplied by the runner.

def wm_status(stage, start):
    data = gui_wait_serial(("[GUI-WM] " + stage + " ").encode(), start, 20)
    matches = re.findall(rb"\[GUI-WM\] " + stage.encode() + rb" ((?:-?\d+ ?){11})", data)
    assert matches, data[-2000:]
    return [int(value) for value in matches[-1].split()]


def wm_key_status(key, stage="state"):
    start = serial.stat().st_size
    gui_qemu_key(key)
    return wm_status(stage, start)


def wm_point(x, y):
    gui_qmp("input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": round(x * 32767 / (width - 1))}},
        {"type": "abs", "data": {"axis": "y", "value": round(y * 32767 / (height - 1))}}]})
    time.sleep(.18)


def wm_button(down):
    gui_qmp("input-send-event", {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]})
    time.sleep(.2)


def wm_drag(start, end, cancel=False):
    wm_point(*start)
    begin = serial.stat().st_size
    wm_button(True)
    wm_point(*end)
    if cancel:
        gui_qemu_key("esc")
    wm_button(False)
    return wm_status("changed", begin)


def wm_geometry(state, expected, name):
    assert all(abs(actual - wanted) <= 1 for actual, wanted in zip(state[:4], expected)), (name, state, expected)
    record["passed"].append(name)


def wm_pixel(name, x, y):
    data = (out / (name + ".ppm")).read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    image_width = int(header[1])
    offset = header.end() + (y * image_width + x) * 3
    return data[offset:offset+3]


start = serial.stat().st_size
for byte in b"winexec /guiwm.exe\r":
    uart.sendall(bytes([byte])); time.sleep(.03)
state = wm_status("ready", start)
wm_geometry(state, (240, 200, 520, 330), "create custom frame with nested children")
gui_qemu_capture("wm-01-app-focused")
terminal_color = wm_pixel("01-desktop", 450, 400)
assert wm_pixel("wm-01-app-focused", 450, 400) != terminal_color
gui_qemu_click(500, 360)
state = wm_key_status("k")
assert state[8] == 1, ("client click reaches leaf exactly once", state)

# Focus changes must agree with both final pixels and pointer ownership.
gui_qemu_key("tab", "alt")
gui_qemu_capture("wm-02-terminal-focused")
assert wm_pixel("wm-02-terminal-focused", 450, 400) == terminal_color
gui_qemu_click(450, 400)
wm_point(300, 68); wm_button(True); wm_point(360, 98); wm_button(False)
gui_qemu_capture("wm-03-terminal-dragged")
assert wm_pixel("wm-03-terminal-dragged", 450, 400) == terminal_color
gui_qemu_key("tab", "alt"); gui_qemu_key("tab", "alt")
state = wm_key_status("k")
assert state[8] == 1, ("native click leaked to covered client", state)
gui_qemu_capture("wm-04-app-restored")
assert wm_pixel("wm-04-app-restored", 450, 400) != terminal_color
record["passed"].append("native focus, drag and client focus agree with pixels; no click-through")

state = wm_drag((340, 220), (390, 250))
wm_geometry(state, (290, 230, 520, 330), "drag through two HTTRANSPARENT child levels")
gui_qemu_capture("wm-05-custom-drag")
cases = [
    ("left", (241, 365), (201, 365), (200, 200, 560, 330), 1),
    ("right", (759, 365), (829, 365), (240, 200, 590, 330), 2),
    ("top", (500, 201), (500, 161), (240, 160, 520, 370), 3),
    ("top-left", (241, 201), (211, 171), (210, 170, 550, 360), 4),
    ("top-right", (759, 201), (799, 171), (240, 170, 560, 360), 5),
    ("bottom", (500, 529), (500, 579), (240, 200, 520, 380), 6),
    ("bottom-left", (241, 529), (211, 559), (210, 200, 550, 360), 7),
    ("bottom-right", (759, 529), (799, 559), (240, 200, 560, 360), 8),
]
for name, first, last, expected, edge in cases:
    wm_key_status("r", "ready")
    state = wm_drag(first, last)
    wm_geometry(state, expected, "resize " + name)
    assert state[9] == edge, ("WM_SIZING edge", edge, state)
gui_qemu_capture("wm-06-resized")

wm_key_status("r", "ready")
state = wm_drag((759, 529), (400, 300))
wm_geometry(state, (240, 200, 320, 200), "WM_GETMINMAXINFO minimum dimensions")
wm_key_status("r", "ready")
state = wm_drag((759, 529), (1199, 749))
wm_geometry(state, (240, 200, 760, 540), "WM_GETMINMAXINFO maximum dimensions")

wm_key_status("r", "ready"); wm_key_status("s")
state = wm_drag((759, 529), (796, 552))
wm_geometry(state, (240, 200, 544, 352), "WM_SIZING callback edits copied back")
wm_key_status("s")
wm_key_status("r", "ready")
state = wm_drag((759, 529), (799, 569), cancel=True)
wm_geometry(state, (240, 200, 520, 330), "Escape cancels resizing and restores geometry")
gui_qemu_capture("wm-07-cancel-restored")
assert state[4] == state[5] and state[5] >= 13, state
start = serial.stat().st_size
gui_qemu_key("q")
gui_wait_serial(b"GUI-WM PASS", start, 20)
data = serial.read_bytes()
assert b"GUI-WM FAIL" not in data and b"!!! EXCEPTION" not in data and b"SYSTEM HALTED" not in data
record["passed"].append("PE input fixture completed with balanced enter/exit notifications")
