# Dark Aero GUI integration

This guide records the shared desktop overhaul, the x86 integration points and
the evidence needed to integrate it alongside kernel stability work. The GUI
uses dark blue glass inspired by Vista and Windows 7, proportional text and
scale-aware controls. The dock remains the default; a bottom taskbar is optional.
The current user-facing controls and build commands are in the
[GUI reference](../gui/README.md).

The [Win32 graphics review](win32-graphics-latency.md) traces application painting
through shared surfaces, composition and presentation. It records the observed
costs, the first GDI optimization and the remaining latency measurements.

## Implemented behavior

| Area | Behavior | Main implementation |
| --- | --- | --- |
| Desktop layout | Dock plus top panel, or bottom taskbar; task paging when icons do not fit | `gui_theme.c`, `gui_dock.c`, `gui_panel.c` |
| Text | Proportional Noto Sans, native atlases at seven scales, bounded UTF-8 and ellipsis | `gui_font.c`, `gui_font_data.h`, `gui/fonts/` |
| Display scale | Automatic or 100/125/150/175/200/250/300%; validated physical size when available | `gui_theme.c`, desktop settings |
| Glass | Live regional backdrop blur, dark tint, reflections, illuminated rims and soft shadows | `gui_widgets.c`, `gui_window.c` |
| Native windows | Shared title-button hit geometry, drag, maximize/restore, close/minimize and dock restore | Desktop and x86 compositor |
| Focus | Native foreground pixels, pointer ownership and focus highlighting agree | Desktop, compositor and USER32 guard |
| Custom Win32 frames | Application hit testing initiates caption movement and resizing; child hits retain their semantics | USER32 integration |
| Selectors | Current-value check, keyboard or pointer highlight, bounded popup and scrolling choices | Shared popup widgets and desktop input |
| Appearance form | Stacked labels in narrow windows, fixed heading, scrolling, clipped content and visible keyboard focus | `gui_desktop.c` |
| Scrollbar drag | Stable grab point, pointer capture outside System, release and Escape/focus cancellation | Desktop pointer API and compositor |
| USB wheel | Signed HID wheel deltas reach native input and the optional Win32 bridge | `xhci.c`, compositor wheel routing |

Settings apply immediately for the current kernel session. Glass and reduced
motion are independently configurable. Opaque client content and popup lists
keep text readable over changing backgrounds.

## Display size and legibility

Design dimensions are logical pixels. `gui_px()` converts each dimension to
physical pixels using the effective scale. The font renderer chooses a native
atlas at that scale instead of enlarging a small raster font. The legacy
Terminal cell font and application-owned pixel surfaces keep their own rendering.
The Terminal window adds an 8-logical-pixel inner margin on all sides.
`gui_desktop_terminal_rect()` is shared by painting and initial surface sizing,
so allocating the text grid accounts for that margin rather than cropping it.

Automatic scale uses physical display dimensions when available. The x86
desktop consults the existing GPU EDID path after it is ready. Invalid checksums,
dimensions and inconsistent aspect ratios are rejected. A labelled resolution
estimate is used when physical size is unavailable; the Appearance panel also
accepts a manual display diagonal.

The scale is capped to retain at least 640 by 420 logical pixels where the
display permits it. The panel reports physical resolution, effective workspace,
scale and measured or estimated size information. Scaled form heights sum the
individually rounded dimensions used by the painter so the final text line stays
reachable at fractional scale.

## Glass rendering and cost

The renderer snapshots the already-composited background behind each glass
region, including a sampling margin. It filters a screen-aligned reduced image
with two separable box passes and interpolates it back to physical pixels.
Sampling follows display scale and becomes coarser for large regions to fit
two fixed buffers totalling 256 KiB.

This path uses integer arithmetic and no per-frame allocation. It does not blur
the entire framebuffer for every window. A luminance limit, dark tint and text
shadow preserve title contrast; reflections and edge lighting finish the glass.
Native window frames sample their background before their opaque client area is
painted. Turning glass off skips backdrop sampling and filtering.

The recorded Clang host benchmark on a Ryzen 7 5800X measured approximately
2.9 ms for a background fill plus four chrome regions at 1080p/100%, and 11.3 ms
at 4K/200%. These are host painter measurements from the glass validation run;
they do not establish whole-compositor frame rates or physical-P1 performance.

## Focus and custom windows

The compositor draws the background, inactive native windows, managed client
surfaces, the focused native window, then panel/dock chrome once. Raising a
native window therefore changes both its visible position in the stack and its
pointer ownership. It does not require repainting the glass over itself.

The USER32 desktop guard prevents a native click from activating a covered
Win32 surface. Managed windows retain their parent/child and popup ordering;
explicit application capture and relative-pointer modes keep their existing
rules. Keyboard focus and pointer hit testing remain separate decisions.

Custom-frame Win32 windows use `WM_NCHITTEST` to distinguish caption, client and
sizing edges. The integration handles all eight resizing directions,
`WM_GETMINMAXINFO` limits, `WM_SIZING` rectangle changes and Escape cancellation.
`HTTRANSPARENT` continues through the same-thread parent chain; normal child
client hits stay with that child. PE32 pointer-bearing callbacks use
process-mapped scratch memory. The PE32/PE64 fixture validates these contracts;
it does not contain Steam.

## Form input contracts

Appearance has five keyboard targets: layout, scale, display size, glass and
reduced motion. Tab/Shift+Tab reveal the new target and its label. Home/End focus
the first/last target. Page Up/Page Down, the wheel, scrollbar arrows and track
clicks move the content. If scrolling hides the current target, its next
activation first reveals it; a subsequent press changes it. No invisible option
is changed by that first press.

The scrollbar thumb captures its gesture when pressed and uses the shared form
geometry throughout dragging. The first press does not jump the content.
Leaving System or moving horizontally keeps the gesture owned by the thumb;
button-up applies the final position and ends capture. Escape restores the
position at the start of the drag. Other navigation, focus loss, hiding, scale
changes or a changed window rectangle cancel dragging. Wheel input does not
compete with an active thumb drag.
Holding the thumb still retains capture without continuously requesting frames.

`gui_desktop_pointer_update()` must run before other pointer hit testing while
the widget captures; it also consumes the final release/cancellation. The
compositor's desktop guard includes this capture so crossing a managed surface
does not transfer the native gesture. Native titlebar dragging and managed
application capture remain separate paths.

An open selector consumes its navigation until confirmation or dismissal.
Browsing with arrows, page keys or the wheel changes the highlighted candidate,
not the applied value. Enter/Space confirms; Escape or an outside click cancels.
Popup geometry keeps complete rows within the visible owner. Border and padding
clicks consume the event without selecting the first item.

The compositor routes wheel input into Appearance only over its focused native
window. Other targets retain the existing wheel path, including already queued
events. USB HID handling first establishes pointer position and buttons, then
posts the wheel once. Native input uses wheel steps; the optional Win32 bridge
receives the scaled wheel value, with saturation before narrowing to `short`.

## Files that must be integrated together

| Change group | Files and dependencies |
| --- | --- |
| Shared GUI | `gui/gui_theme.*`, `gui/gui_font*`, font license/data, `gui/gui_widgets.c`, `gui/gui_window.c`, `gui/gui_desktop.c`, `gui/gui_dock.c`, `gui/gui_panel.c`, `gui/gui.h` |
| Architecture builds | GUI source and header dependencies in the x86, ARM and WASM Makefiles |
| x86 composition | Foreground native rendering, task/focus selection, pointer guard and form input routing in `arch/x86/kernel/compositor.c` |
| Win32 frame behavior | The GUI-related hit-test, move/size and pointer-routing changes within `arch/x86/win32/user32_shim.c`; related declarations as shown by the reviewed diff |
| USB wheel | Wheel extraction and delivery in `arch/x86/drivers/xhci.c`; the matching stub in `arch/x86/test/xhci_poll_host.c` |
| Evidence | GUI host suites, QEMU drivers/fixtures, this guide, `gui/README.md` and `docs/screenshots/gui-*.png` |

The shared branch also contains unrelated stabilization work, including edits to
some of these files. Integrate the relevant functions or reviewed hunks instead
of replacing complete files from a previous snapshot. In particular,
`gui_draw.c` already contains opaque-shadow optimizations from other work; the
glass painter is separate and those changes must be preserved.

New native input consumers must share the same geometry for painting and hit
testing, respect the compositor's pointer owner and release capture on focus or
geometry changes. Win32 clients continue to own their content rendering and
custom-frame decisions.

## Reproducible validation

Run the host commands in [the GUI reference](../gui/README.md#validation) with
fresh output directories. They compile the production renderer and input code
under GCC and Clang, normally and with AddressSanitizer/UndefinedBehaviorSanitizer.
The USB transport suite also builds with and without the optional Win32 bridge.
Use `test-xhci-poll.sh` after changing HID delivery.

Use an isolated kernel output directory to preserve another session's build:

```sh
make -C arch/x86 CLANG=1 BUILD=../../build/gui-review/kernel-build \
  QUIET=1 SKIP_MODEL=1 -j2 \
  ../../build/gui-review/kernel-build/kernel.elf \
  ../../build/gui-review/kernel-build/boot.efi

OSITO_GUI_SELFTEST=1 bash arch/x86/scripts/qemu-test-disks.sh run -- \
  python3 arch/x86/scripts/test-gui-desktop-qemu.py \
  build/gui-review/kernel-build/boot.efi \
  build/gui-review/kernel-build/kernel.elf build/gui-review/qemu-desktop

OSITO_GUI_RESPONSIVE_TEST=1 bash arch/x86/scripts/qemu-test-disks.sh run -- \
  python3 arch/x86/scripts/test-gui-desktop-qemu.py \
  build/gui-review/kernel-build/boot.efi \
  build/gui-review/kernel-build/kernel.elf build/gui-review/qemu-responsive
```

The QEMU runner uses fresh disposable disks and no network interface. Its
`results.json` records the kernel hash, capture hashes and actual assertions;
`serial.log` is the kernel trace. A capture alone does not prove a test passed.
The standard scenario checks dock input and return to Terminal, captures both
layouts and scale changes, and can run the USER32 self-tests. The responsive
scenario compares framebuffer regions after wheel/page navigation, selector
cancellation and scrollbar capture. It covers grabbing without a jump, dragging
outside System, release, Escape rollback, focus loss and resumed wheel input.

For custom frames, build `build-gui-window-integration.sh` and supply its
PE32 or PE64 fixture as the QEMU runner's fourth argument. For AArch64, compile
the shared GUI sources with the architecture include path, freestanding flags
and warnings as errors. A full Emscripten build has not been run here.

## Recorded checkpoints

These are completed runs from 2026-10-04. Build folders are ignored local
artifacts, not files guaranteed to exist in a fresh checkout. Their manifests
identify the exact tested source and kernel hashes.

| Checkpoint | Local evidence | Verified scope |
| --- | --- | --- |
| Initial overhaul | `build/gui-aero-20261004/validation.json` | GUI metrics/text/geometry, foreground native focus, PE32/PE64 custom frames and USER32 self-tests |
| Glass | `build/gui-glass-20261004/validation.json` | Regional blur, bounds/guards, contrast, repeated redraw and host timings |
| Selectors | `build/gui-controls-20261004/validation.json` | Popup geometry, input modality, current-value check and short lists |
| Responsive form | `build/gui-responsive-20261004/validation.json` | 32 host variants, two QEMU scenarios with 14 assertions, USB wheel transport, AArch64 GUI compilation |
| Scrollbar capture | `build/gui-scroll-drag-20261004/validation.json` | 16 host variants, two final QEMU scenarios with 20 assertions, stationary capture without continuous redraws, AArch64 GUI compilation |
| Terminal margin | `build/gui-terminal-padding-20261004/validation.json` | 8 existing GUI/focus host variants, standard QEMU desktop scenario and AArch64 compilation of the changed desktop source |

The responsive checkpoint's kernel SHA-256 is
`ceef2c109c489dc49a741599354fd8a58bea78ed77dcc58a7d070aaf4dc5ca3c`.
Later changes require their own build and evidence; the old hash remains a
historical checkpoint rather than a claim about the current working tree.

Visual references:

- [Glass dock](screenshots/gui-aero-glass-dock.png) and [glass taskbar](screenshots/gui-aero-glass-taskbar.png), captured in QEMU.
- [Blur material comparison](screenshots/gui-aero-glass-material.png), using the host painter.
- [Managed application](screenshots/gui-focus-application.png) and [focused Terminal](screenshots/gui-focus-terminal.png), captured by the integration scenario.
- [Responsive 175% form](screenshots/gui-aero-responsive.png), captured in QEMU, and [narrow stacked form](screenshots/gui-aero-responsive-narrow.png), using the host painter.
- [Active scrollbar thumb](screenshots/gui-aero-scroll-drag.png), captured in the final QEMU drag scenario.
- [Terminal inner margin](screenshots/gui-terminal-padding.png), captured in QEMU at 100% scale.

## Remaining validation and limitations

The GUI validation runs reported here did not deploy a kernel to the physical
P1. These records do not establish hardware integration. Hardware validation
must cover display dimensions/scale, readable text, both layouts,
glass over moving content, focus switching between Terminal and a managed app,
custom-frame dragging/resizing, menus and pointer scrolling. Steam Library
behavior must be assessed in a separate real Steam session; the GUI fixtures
do not establish that its unrelated stability problems are resolved.

Settings are not persisted to disk. Multi-monitor DPI, Win32 DPI reporting,
automatic scaling of client surfaces and keyboard-initiated `SC_SIZE` without
an edge remain outside this implementation. Font coverage is Latin-1 plus the
documented punctuation and fallback glyph, not a complete international shaping
engine. P1 frame time and full WASM behavior remain unmeasured.
