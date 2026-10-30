# Dark glass desktop

For the complete feature record, integration boundaries, evidence and remaining
hardware checks, see [Dark Aero GUI integration](../docs/gui-aero-integration.md).

The shared desktop uses blurred, smoked blue glass, opaque dark content surfaces,
soft outer shadows, proportional text and vector control icons. The default layout
keeps the top panel and centered dock. Open the **System** gear in the dock to
switch to a bottom taskbar or change scale, display size and visual effects.

Real-kernel QEMU captures with backdrop blur:
[dock and top panel](../docs/screenshots/gui-aero-glass-dock.png) and
[bottom taskbar](../docs/screenshots/gui-aero-glass-taskbar.png).
The [material comparison](../docs/screenshots/gui-aero-glass-material.png)
uses the same software painter on the host to show blurred versus opaque chrome.

Settings apply immediately and live for the current kernel session. They are
not yet serialized to disk. Selectors support pointer input, Tab/Shift+Tab,
arrows, Enter/Space and Escape. Close and minimize hide built-in windows; the
dock restores them. Window title buttons use the same geometry for painting
and hit testing. The dock pages through overflow instead of placing items off
screen. **Glass transparency** enables live backdrop blur, dark tint, broad
diagonal reflections and illuminated rims on titlebars, the panel and the dock.
Disabling it uses fully opaque chrome and skips backdrop sampling and blur.
Client content and selector lists stay opaque for legibility.

Terminal text has an 8-logical-pixel inset on all sides, following the GUI scale.
`gui_desktop_terminal_rect()` supplies the same inner viewport to surface
allocation and painting, so the initial text grid is sized inside the margin.
See the [Terminal margin in QEMU](../docs/screenshots/gui-terminal-padding.png).

Selectors use a separate arrow button and a raised blue highlight. Their menus
mark the current value with a check, independently of the pointer/keyboard row.
Moving the pointer restores mouse highlighting; keyboard navigation resumes it
from the keyboard. The popup's shared geometry chooses space below or above the
field, keeps complete rows inside the visible owner, and provides a scroll rail
when the list cannot fit. Keyboard navigation keeps its target visible. Borders
and padding consume clicks without selecting an item; Escape or an outside click
dismisses the menu without changing its value.

Control previews: [kernel popup in QEMU](../docs/screenshots/gui-aero-controls.png)
and [short-window list with scroll buttons](../docs/screenshots/gui-aero-controls-compact.png)
(the latter uses the host renderer).

Appearance stacks labels above fields in narrow windows and scrolls vertically
when its contents exceed the available height. Its heading stays fixed; content
and popup painting are clipped on all four sides. The wheel, Page Up/Page Down,
scrollbar arrows and track clicks move through the form. Tab/Shift+Tab reveal the
focused control and its label; Home/End focus the first/last control. If pointer
scrolling hides the current focus, the next activation reveals it before another
press can change its value. Grabbing the scrollbar thumb keeps its position;
dragging moves the content even after the pointer leaves System. Button-up ends
the gesture. Escape restores its starting position; other navigation, focus loss,
hiding or geometry changes cancel the drag. Wheel events are ignored during the
drag, and capture prevents it from becoming a click or move in another window.
Holding the thumb still does not request continuous redraws.

An open selector owns wheel and page-key navigation until it is dismissed or
confirmed; browsing the choices does not apply them. The compositor routes wheel
events to Appearance only over its focused native window. Other targets retain
the existing event path. USB HID wheel deltas now reach native input and the
optional Win32 bridge, with the current pointer position established first.

Responsive previews: [175% in QEMU](../docs/screenshots/gui-aero-responsive.png)
and [narrow form with stacked labels](../docs/screenshots/gui-aero-responsive-narrow.png)
(the latter uses the host renderer).

## Scale and display metrics

`gui_theme.c` holds presentation options and primary-display metrics. Design
measurements are logical pixels; `gui_px()` rounds them to physical pixels.
This follows the usual separation between
[DIPs and physical pixels](https://learn.microsoft.com/en-us/windows/win32/learnwin32/dpi-and-device-independent-pixels).
Supported scale steps are 100, 125, 150, 175, 200, 250 and 300 percent.

Automatic scaling uses physical dimensions when available. The existing x86
GPU display driver can provide a checksum-validated EDID at desktop startup;
other display backends can call `gui_theme_from_edid()` or
`gui_theme_set_physical_size()`. GOP/Intel paths that currently expose only
resolution use a conservative resolution estimate, clearly labelled in System.
A manual display diagonal derives physical dimensions using the active aspect
ratio. Invalid dimensions and inconsistent aspect ratios are rejected.

The GUI reports both physical resolution and effective workspace. It caps the
scale to retain at least 640 x 420 logical pixels when the display can provide
that space. Existing application/terminal pixel surfaces keep their own content
rendering; this change does not alter Win32 DPI reporting or stretch client
surfaces. Multi-monitor DPI and persistence remain future work.

## Rendering boundaries

- `gui_theme.*`: palette tokens, scale, work area and rectangle operations.
- `gui_font.c`: bounded UTF-8 decoding, measurement, clipping and ellipsis.
- `gui_widgets.c`: glass, shadows, controls, popup geometry/input and vector icons.
- `gui_window.c`: shared frame and title-button geometry.
- `gui_dock.c`: shared dock/taskbar geometry, overflow and task painting.
- `gui_desktop.c`: background cache, native window contents and System settings.

The x86 compositor draws the background, inactive native windows, client
surfaces, the foreground native window, then desktop chrome once. Native
window rendering is separate from the background so glass is never painted
twice to bring Terminal forward. The shared focus value also drives titlebar
and dock highlighting, pointer ownership and the USER32 click-through guard.
The managed layer preserves USER32's parent/child and popup ordering.
`gui_desktop_render()` is the combined standalone entry point, while
`gui_desktop_render_base()` draws background and all native windows. Frame
advances during animations explicitly request a repaint,
including the last frame. Background copies respect destination pitch.

The existing opaque shadow primitives in `gui_draw.c` retain their optimized
interior exclusion. Glass frames use a separate continuous-falloff shadow that
also visits only outside strips. Neither path allocates per-frame buffers.

Glass snapshots the already-composited layers behind each region, including a
small sampling halo. It filters a reduced-resolution, screen-aligned grid with
two separable box passes and interpolates back to physical pixels. The sampling
step follows display scale and increases for unusually large regions to fit two
fixed buffers totalling 256 KiB. The second buffer is reused for row interpolation.
This is integer-only software rendering on the GUI thread, with no per-frame
allocation or full-screen blur. A luminance limit keeps light title text readable
over bright applications; title text also has a small dark shadow. Native frames
sample their backdrop before drawing their own opaque client area.

## Font source

UI text uses rasterizations of
[Noto Sans Regular](https://github.com/notofonts/noto-fonts/blob/main/hinted/ttf/NotoSans/NotoSans-Regular.ttf),
distributed under the SIL Open Font License in [fonts/LICENSE.txt](fonts/LICENSE.txt).
The bundled TTF SHA-256 is
`b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5`.

`gui_font_data.h` contains 194 glyphs at seven native pixel sizes, packed as
4-bit coverage runs (173,654 coverage bytes). ASCII, Latin-1 and common dashes /
ellipsis are covered; unsupported codepoints use `?`. Kernel builds consume
the checked-in atlas and require no font library, host fonts or network access.
The legacy terminal cell font has its existing API.

Regenerate using Pillow (the initial atlas used Pillow 12.3.0):

```sh
python tools/generate-gui-font.py
```

## Validation

```sh
bash arch/x86/scripts/test-gui-aero.sh build/gui-aero-host
bash arch/x86/scripts/test-gui-glass.sh build/gui-glass-host
bash arch/x86/scripts/test-gui-popup.sh build/gui-popup-host
bash arch/x86/scripts/test-gui-responsive.sh build/gui-responsive-host
bash arch/x86/scripts/test-gui-usb-wheel.sh build/gui-usb-wheel-host
bash arch/x86/scripts/test-gui-shadow.sh build/gui-shadow-host
bash arch/x86/scripts/test-gui-compositor-focus.sh build/gui-focus-host
```

The GUI suite runs GCC and Clang with warnings as errors, both normally and
with AddressSanitizer / UndefinedBehaviorSanitizer. It checks metrics, EDID
validation, text clipping, UTF-8, padded buffers, allocation failure, off-screen
frames, hit geometry, paged docks and keyboard settings. It also writes PPM
previews at 800 x 600, 1280 x 800, 1920 x 1080 and 3840 x 2160.

The glass suite adds guarded buffers, clipping and extreme rectangles at every
scale; it checks backdrop sensitivity, opaque mode, sampling across panel edges,
fresh snapshots, loss of fine detail, bright-background legibility and adaptive
sampling for large panels. It produces a material comparison using the actual
renderer and reports CPU timings for four chrome regions at 1080p and 4K.

The popup suite checks layout and hit boundaries at all seven scales, short and
narrow owners, pointer scrolling, keyboard visibility, cancellation and input
modality changes in the real desktop. It also verifies that moving a glass window
restores the previous background and that repeated full redraws are identical.

The responsive suite checks narrow/short forms at all seven scales, Tab visibility,
offscreen activation guards, wheel/page navigation, scrollbar hits and capture,
popup cancellation and four-edge paint clipping with padded buffers. The USB wheel
suite exercises production HID handling, signed deltas, shared field offsets,
report IDs, saturation and builds with/without the optional Win32 bridge. The
compositor focus suite also checks wheel ownership and preserves queued input
for other applications.

The opaque-shadow suite keeps an independent rasterization oracle. Its old
whole-window comparison assumed an opaque titlebar; window safety and geometry
now belong to the GUI suite.

For a real-kernel desktop smoke test on fresh disposable disks:

```sh
bash arch/x86/scripts/qemu-test-disks.sh run -- \
  python3 arch/x86/scripts/test-gui-desktop-qemu.py \
  path/to/boot.efi path/to/kernel.elf build/gui-desktop-qemu
```

This requires QEMU/KVM, OVMF, mtools and the existing OsitoFS host tools. It opens
System using USB pointer input, captures close/minimize/restore/maximize,
changes layout and scale using the keyboard, then verifies an `echo` command
after returning to Terminal with Alt+Tab. Captures need visual review; the
script's `passed` list identifies its automatic assertions separately.

Set `OSITO_GUI_RESPONSIVE_TEST=1` for the additional 175% scenario. It compares
framebuffer regions after USB wheel and Page Up/Page Down input, checks that
outside scrolling leaves Appearance unchanged, scrolls a selector without
applying its value, exercises thumb drag/release/Escape/focus loss, then verifies
keyboard input in Terminal. The QEMU driver
uses only the Python standard library.

For the native/Win32 boundary, build `build-gui-window-integration.sh` and pass
its `pe32/gui-wm.exe` or `pe64/gui-wm.exe` as the fourth argument to the QEMU
runner. This replaces the appearance scenario with a custom-frame application
containing two nested child windows. Assertions cover pixel ordering across
Alt+Tab, native drag without click-through, child-to-parent caption dragging,
all eight sizing edges/corners, size limits, callback rectangle changes and
Escape restoring the original rectangle. The fixture runs on fresh disks and
does not contain or launch Steam.

USER32 now tracks interactive sizing alongside moving. The application keeps
control of its custom frame through
[`WM_NCHITTEST`](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-nchittest),
[`WM_GETMINMAXINFO`](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-getminmaxinfo)
and [`WM_SIZING`](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-sizing).
Transparent child hits continue through their same-thread parent chain;
ordinary client hits stay in the child. PE32 pointer-bearing callbacks use
process-mapped scratch memory. Interactive size selection through a keyboard
system menu (`SC_SIZE` without an edge) is still unimplemented.

The implementation was also compiled as AArch64 GUI objects. Emscripten was not
available in the development environment; its source list and header
dependencies are updated, but a full WASM build is not claimed.

The 2026-10-04 validation run passed all four host variants (263,052 assertions
each), all four opaque-shadow variants (3,789 cases each), and the QEMU desktop
smoke test. The QEMU captures were visually reviewed for native window controls,
both layouts and the 125% scale change. Shared GUI objects also compile for
AArch64 with warnings as errors. Results, logs and additional previews are in
`build/gui-aero-20261004/`; this run did not deploy a kernel to the physical P1.

The focus regression passed in four host variants (23 assertions each). Both
PE32 and PE64 custom-frame fixtures passed the full QEMU interaction scenario.
The existing window-model and input self-tests also passed in the final kernel
(102 and 166 assertions respectively). This verifies the shared integration,
not a new Steam session on physical hardware.

Focus captures: [application active](../docs/screenshots/gui-focus-application.png)
and [Terminal active](../docs/screenshots/gui-focus-terminal.png).

The subsequent glass iteration in `build/gui-glass-20261004/` passed all four
glass variants (1,116,056 checks each), the four GUI variants (263,052 each),
and the four focus variants (23 each). The updated kernel passed the QEMU
desktop scenario; its 13 captures include both layouts, window controls,
selectors and the 125% scale change. Shared GUI objects compiled for AArch64
with warnings as errors. In a Clang host benchmark on a Ryzen 7 5800X, a
background fill plus four chrome regions took about 2.9 ms at 1920 x 1080 / 100%
and 11.3 ms at 3840 x 2160 / 200% with glass enabled. These are host CPU timings,
not physical-P1 frame rates. The run's hashes and evidence are recorded in
`build/gui-glass-20261004/validation.json`.

The controls follow-up in `build/gui-controls-20261004/` passed all four popup
variants (217,267 checks each), all four GUI variants (263,052 each), and all
four focus variants (23 each). The updated x86 kernel completed the QEMU desktop
scenario, including both layouts, the scale selector, a 125% scale change and
returning keyboard input to Terminal. The full shared GUI compiled for AArch64
with warnings as errors. Evidence and hashes are in that directory's
`validation.json`; this run did not deploy to the physical P1.

The responsive follow-up in `build/gui-responsive-20261004/` passed the new
responsive and USB-wheel suites, plus the GUI, popup, focus and existing xHCI
poll/storage regressions under GCC/Clang, normally and with sanitizers. The x86
kernel completed both QEMU scenarios: the usual desktop controls/layout/scale
sequence with the USER32 self-tests, and nine responsive assertions using real
USB input and framebuffer comparisons. Shared GUI sources compiled as AArch64
objects with warnings as errors. Captures, source/kernel hashes and results are
in that directory's `validation.json`. Physical-P1 behavior is still unverified
for this iteration; the kernel was not deployed there.

The scrollbar follow-up in `build/gui-scroll-drag-20261004/` adds pointer capture,
Escape rollback, focus/geometry cancellation and a regression for stationary
capture avoiding continuous redraws. All four responsive, GUI, popup and focus
host variants passed. The final x86 kernel passed 20 assertions across both QEMU
scenarios, including the USER32 self-tests; shared GUI sources compiled for
AArch64 with warnings as errors. See its `validation.json` for hashes and
[the captured thumb](../docs/screenshots/gui-aero-scroll-drag.png) for its active
appearance. The [integration guide](../docs/gui-aero-integration.md) consolidates
the complete GUI work and the remaining hardware checks.
