"""Optional real-input scenario loaded by test-gui-desktop-qemu.py.

At 175%, the standard 1280x800 GOP display needs a scrollable settings pane.
Screenshots and comparisons cover actual framebuffer output, not a mockup.
"""
def responsive_move(x, y):
    gui_qmp("input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": round(x * 32767 / (width - 1))}},
        {"type": "abs", "data": {"axis": "y", "value": round(y * 32767 / (height - 1))}}]})
    time.sleep(.3)


def responsive_wheel(button):
    for down in (True, False):
        gui_qmp("input-send-event", {"events": [{"type": "btn", "data": {
            "down": down, "button": button}}]})
        time.sleep(.2)


def responsive_region(name, box=(350, 220, 1180, 510)):
    data = (out / (name + ".ppm")).read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    assert header
    w, h = map(int, header.groups())
    x0, y0, x1, y1 = box
    assert 0 <= x0 < x1 <= w and 0 <= y0 < y1 <= h
    pixels = memoryview(data)[header.end():]
    return b"".join(pixels[(y*w+x0)*3:(y*w+x1)*3] for y in range(y0, y1))


gui_qemu_key("tab"); gui_qemu_key("ret")
for _ in range(4):
    gui_qemu_key("down")
gui_qemu_key("ret")
responsive_move(300, 400)
gui_qemu_capture("03-responsive-175-top")
responsive_wheel("wheel-down")
gui_qemu_capture("04-responsive-175-scrolled")
assert responsive_region("03-responsive-175-top") != responsive_region("04-responsive-175-scrolled"), "wheel did not move the settings content"
record["passed"].append("USB wheel scrolls the native settings content at 175 percent")
responsive_wheel("wheel-up")
gui_qemu_capture("05-responsive-wheel-restored")
assert responsive_region("03-responsive-175-top") == responsive_region("05-responsive-wheel-restored"), "wheel up did not restore the same controls"
record["passed"].append("wheel up restores the original control pixels")

responsive_move(150, 300)
responsive_wheel("wheel-down")
gui_qemu_capture("06-responsive-outside-wheel")
assert responsive_region("05-responsive-wheel-restored") == responsive_region("06-responsive-outside-wheel"), "outside wheel moved the native pane"
record["passed"].append("wheel outside System leaves its form unchanged")

gui_qemu_key("ret")
responsive_move(301, 400)
gui_qemu_capture("07-responsive-popup-top")
responsive_wheel("wheel-down")
gui_qemu_capture("08-responsive-popup-wheel")
assert responsive_region("07-responsive-popup-top", (620, 220, 1200, 630)) != responsive_region("08-responsive-popup-wheel", (620, 220, 1200, 630)), "wheel did not move popup choices"
record["passed"].append("wheel scrolls the open selector choices")
gui_qemu_key("esc")
responsive_move(302, 400)  # Return to mouse mode, making focus pixels comparable.
gui_qemu_capture("09-responsive-popup-cancelled")
assert responsive_region("03-responsive-175-top") == responsive_region("09-responsive-popup-cancelled"), "popup wheel committed a value or moved the form"
record["passed"].append("Escape leaves the scale and form position unchanged after popup scrolling")

gui_qemu_key("pgdn")
gui_qemu_capture("10-responsive-page-down")
assert responsive_region("03-responsive-175-top") != responsive_region("10-responsive-page-down")
gui_qemu_key("pgup")
gui_qemu_capture("11-responsive-page-up")
assert responsive_region("03-responsive-175-top") == responsive_region("11-responsive-page-up")
record["passed"].append("Page Down and Page Up scroll and restore the form")

runpy.run_path("arch/x86/scripts/gui-scroll-drag-qemu-driver.py", init_globals=globals())

gui_qemu_key("tab", "alt")
start = serial.stat().st_size
for char in "echo guiresponsive\n":
    gui_qemu_key({" ": "spc", "\n": "ret"}.get(char, char))
gui_wait_serial(b"\nguiresponsive\n", start, 15)
record["passed"].append("terminal receives USB keyboard input after scrolling System")
data = serial.read_bytes()
assert b"!!! EXCEPTION" not in data and b"SYSTEM HALTED" not in data and b"DOUBLE PAGE-FREE" not in data
record["passed"].append("responsive desktop session completed without a fatal diagnostic")
