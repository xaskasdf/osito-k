# UT99 on OsitoK — Open Bugs (post-playable)

State as of 2026-06-07: UT99 boots, renders its menus with correct color, takes
mouse in the menus, New Game reaches Character Creation, and the game is
**playable across the full (decompressed) map set** — it enters a map and runs
thousands of frames fluidly. Fixes landed in commit `a4fbc69` (FName NULL-FILL
gate, DDraw `GetPixelFormat`, mouse bridge) and the full-map-set image grow
(`arch/x86/scripts/osfs2_grow_addmaps.py`).

The items below are the remaining known defects. Most are **layer** issues (our
Win32/compat32 layer), not the binary — UT99 runs correctly on real NT.

---

## B1 — In-game mouse not wired (can't aim / move the weapon)
**Symptom:** in the menus the mouse works; once in a map the mouse does not move
the view / weapon.
**Cause:** two different input paths. The menu (UWindow) polls `GetCursorPos`
and consumes `WM_MOUSEMOVE`, which our `win32_post_mouse_abs` feeds
(`user32_shim.c`). In-game, UE1 reads the mouse through the **viewport**
(`UWindowsViewport`/`UWindowsClient::UpdateInput`) — historically via
**DirectInput** (`dinput.dll`, `IDirectInputDevice::GetDeviceData`/`GetDeviceState`)
or relative-delta `WM_MOUSEMOVE` with cursor re-warp. That path is not wired.
**Note:** QEMU's "mouse grab" is NOT a fix and can crash (see B3) — implement the
viewport input path instead. No `LoadLibrary dinput` is currently seen, so check
whether UT falls back to window-message relative deltas (then feed relative
`WM_MOUSEMOVE` + honor `SetCursorPos` re-centering) or actually needs a minimal
`dinput` shim.

## B2 — In-game keyboard not fully taken (movement / weapon switch)
**Symptom:** keyboard reaches the menus, but in-game movement/weapon-switch keys
don't register well.
**Cause (suspected):** the in-game key path goes through `UInput` key bindings +
the viewport, distinct from the menu's `WM_KEYDOWN` dispatch. The xHCI→win32
keyboard bridge (`hid_route_to_win32` in `drivers/xhci.c`) delivers PS/2 set-1
scancodes as window messages; verify UE1's viewport input reads them (it may use
`GetAsyncKeyState`/`GetKeyboardState` or raw scancodes via the viewport, which our
`key_state[]` must reflect — confirm `GetAsyncKeyState` returns live state).

## B3 — Gameplay wild-pointer #PF (crash during play)
**Symptom:** after fluid play, a #PF crash (user hit it right after toggling QEMU
mouse-grab).
**Captured:** `#PF vector 14`, `RIP=0x1039B6CB` (Engine.dll), `CR2=0x100000023`,
`ERR=0` (read, not-present). Code @ RIP: `D8 5A 24` (`fcomp dword [edx+0x24]`)
`DF E0` (`fnstsw ax`) `F6 C4 01` (`test ah,1`) `74 04` `83 C8 FF` (`or eax,-1`)
`C3` — a tiny x87 float-compare leaf. `EDX=0xFFFFFFFF` (wild object ptr) →
reads `[0xFFFFFFFF+0x24]=0x100000023` → fault.
**Next:** disasm the caller of `0x1039B6CB` to find where `edx=-1` comes from
(an actor/iterator sentinel used as a pointer?). Isolate whether it is
grab-triggered (input-mode switch mid-frame) or an independent gameplay path —
test by playing WITHOUT grab (observed stable so far → likely grab-related, i.e.
tied to B1's input path).

## B4 — Dirty layer state after a crash (can't relaunch in-OS)
**Symptom:** after a PE32 process crash recovers to the `osito>` shell, UT99
can't be relaunched cleanly.
**Cause:** crash recovery (`[WIN32] Crash recovery — returning to shell`,
longjmp in `idt.c`) does not reset the win32/compat32 global state (TEB32, SEH
chain head, thunk/handle tables, FName prefill flags, DDraw COM proxies, the
g_rcall ring, etc.). A fresh `winexec` of UT99 then starts from polluted state.
**Fix direction:** add a `win32_reset()` invoked on crash recovery that
re-initializes the compat32/win32 subsystems to boot state so a relaunch is clean.

## B5 — Not fullscreen (640×480 in a 1024×768 GOP)
**Symptom:** the game renders at 640×480 in the top-left of the 1024×768 GOP
framebuffer (letterboxed / not filling the screen).
**Cause:** SoftDrv renders a 640×480 16bpp surface; `present_surface_to_gop`
(`ddraw_shim.c`) blits it 1:1 honoring the GOP pitch, so it occupies only
640×480 of the larger framebuffer. Options: (a) integer/linear upscale 640×480 →
fit 1024×768 in the present blit; (b) negotiate a GOP-sized display mode so
SoftDrv renders at 1024×768 (heavier; UE1 must accept the mode via
EnumDisplayModes/SetDisplayMode). See B6 — fixing the present scale also fixes
the cursor mapping.

## B6 — Menu mouse position mismatch (cursor offset)
**Symptom:** the menu cursor moves but lands at the wrong on-screen position.
**Cause:** `win32_post_mouse_abs` scales the usb-tablet absolute coords
(logical_min..max) into the **top window's client size**, but that target may not
match where the 640×480 surface is actually presented in the 1024×768 GOP (B5).
The scaling target and the present rectangle must agree. **Fix together with B5:**
once the present rectangle (origin + scale) is known, scale the tablet coords to
that exact rectangle so `cursor_pos` matches the rendered cursor.

## B7 — EH-resume corruption on recoverable throws (#BP / RET-to-junk)
**Symptom (now mostly avoided):** with missing content (e.g. a map screenshot not
in the image), the recoverable `appThrowf` cascade could corrupt state →
`#BP` at a 0xCC thunk-pool tail address, or a RET to `0x13`.
**Status:** masked for normal play by shipping the full decompressed map set, but
the underlying layer bug remains for ANY missing object (mods, missing textures).
**Cause:** the catch IS selected correctly (single `catch(TCHAR*)` for the
`appThrowf` throw; not a typed-match problem) and dispatched, but resuming a
**recoverable** catch (continue execution, not re-throw) doesn't perfectly
restore the establishing frame's state across the engine's recursive `appUnwindf`
recovery. The catch funclet (`0x1015A3C8`) has a real `_EH_prolog`/`_EH_epilog`
(`pop edi;pop esi;pop ebx;mov esp,ebp;pop ebp;ret`) and expects to be **called**
with a proper frame, but `compat32_seh_dispatch` jumps to it with a hand-built
stack (`g_compat32_unwind_*`). Recent-call ring ruled out a shim arg-count bug.
**Fix direction:** invoke the catch funclet like real EH (`_CallCatchBlock`):
call it with EBP set to the establisher and a valid return frame, take its
returned continuation address, then resume — so prologue/epilog balance and
callee-saved registers survive.

---

### Diagnostics in tree (gated; for the above)
- `idt.c`: `[RET0-DIAG]` (near-NULL instruction-fetch) + `[BPDIAG]` (compat-mode
  fatal exception) dump the recent native-shim call ring (`compat32_dump_recent_calls`).
- `ddraw_shim.c`: one-shot `[DDRAW] GetPixelFormat` log.
- `compat32.c`: `g_rcall` ring (records every shim call), `[CATCH-EBP]`.
- `msvcrt_shim.c`: `[THROWMSG]`/`[CXX-Fn]` throw-message decode.
- WSL2+KVM run/watch scripts in `arch/x86/scripts/` (launch_gtk.sh, kill_qemu.sh,
  catch_crash.sh, catch_bp.sh, dump_bpdiag.sh, pf_pattern.sh, …).

See also memory notes: `project_ut99_mouse_and_mapselect_bp`,
`project_ut99_green_tint_getpixelformat`,
`project_ut99_seh_cascade_correct_repro_blocked`.
