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
**ROOT CAUSE (verified by disasm, 2026-06-07):** a **qsort-callback ABI bug** in
our layer — the grab was coincidental, NOT causal. `0x1039B6C0` is a `__cdecl`
qsort comparator (`mov ecx,[esp+4]; mov edx,[esp+8]; fld [ecx+0x24]; fcomp
[edx+0x24]`) — a distance/scene sort UE1 runs the first time a map is entered
(Engine.dll 0x1039A870 builds a 64×0x2C buffer, push thunk 0x10303765 → push
0x2C → call Core appQsort → MSVCRT!qsort → OUR `crt_qsort`). `crt_qsort`/
`crt_bsearch` (msvcrt_shim.c) invoked the **32-bit guest comparator DIRECTLY as a
native 64-bit pointer**, bypassing `compat32_callback_args` (the 64→32 mode switch
+ cdecl stack frame). So the comparator read its element pointers from the 64-bit
RSP (garbage above the host call's return addr) → `edx=0xFFFFFFFF` → `fcomp
[rdx+0x24]` = `0x100000023` → #PF (CR2 matches exactly).
**FIX ATTEMPT 1 (committed bf70e4d, then REVERTED):** routed the leaf comparator
through `compat32_callback_args` (`qs_cmp` helper) with the cdecl EAX
sign-extended. It DID fix `0x1039B6CB`, BUT **regressed New Game**: a
`compat32_callback_args` call per comparison, hundreds of times in qsort's loop
from inside the INT2E `crt_qsort` handler, stresses the callback machinery
(shared `callback_stack[slot]`, depth/jmpbuf, IST1 save/restore, timer mask —
designed for OCCASIONAL callbacks like wndproc/SEH, not a tight high-frequency
loop) → state corruption → a guest NULL vtable call `CR2=0x40` → **KVM
triple-fault (VM paused, internal-error)**. CONFIRMED by A/B (2026-06-08):
reverting B3 made New Game stop triple-faulting (it reaches the map; the original
`0x1039B6CB` #PF returns but recovers cleanly to shell — strictly better than a
kernel triple-fault). So `compat32_callback_args` is the WRONG vehicle here.
**FIX 2 — IN-MODE 32-BIT BLOB (FIXED + VALIDATED 2026-06-08):** the lightweight
far-return idea is impossible (the 64-bit kernel lives in the high half; a 32-bit
`lret` can't return there with a 32-bit EIP — which is exactly why
compat32_callback_args needs the jmpbuf machinery). So the real fix runs qsort
ITSELF in 32-bit mode, like real MSVCRT: a position-independent 32-bit qsort/
bsearch compiled with `clang -m32` (source `arch/x86/scripts/qsort32.c`, 498 bytes,
no relocations, qsort@0 / bsearch@0x1a0), embedded in compat32.c, installed in a
low (<4GB) executable page at boot (`[COMPAT32] qsort32 blob at 0x...`), and
`compat32_make_thunk_ex` resolves the MSVCRT `qsort`/`bsearch` imports to the blob
(like the `_ftol` native-stub path) instead of an INT 0x2E thunk into the 64-bit
crt_qsort. The guest calls the blob NATIVELY in 32-bit; it calls the comparator
32→32 native — NO INT 0x2E, NO compat32 callback, NO IST1 round-trip. VALIDATED:
New Game reaches a map (no `CR2=0x40` triple-fault, VM stays running) and gameplay
runs with NO `0x1039B6CB` / `CR2=0x100000023`. crt_qsort/crt_bsearch (msvcrt_shim.c,
64-bit) stay registered but are now bypassed for the guest.

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

## B8 — Preferences menu crash (out-of-bounds write → AV)
**Symptom:** opening the Preferences/Settings menu crashes the process.
**Captured:** first fault is a WRITE AV at Core.dll `0x1014ADBC`
(`mov word [eax],0; ret`) with `eax=CR2=0x101FD2F0` — past the end of Core.dll.
The corrupt input is a huge FName index `ecx=0x4250A` (271114, well beyond the
131072-pre-sized Names table); the config-enumeration code does
`Names.Data[index]` where `index` came from an object's Name field (`[obj+0x20]`,
obj=`ebx=0x401FFC00`). So a config/property object carries a corrupt FName index
→ Names[] overrun → wild write. Same FName-corruption family as the other UT99
bugs, surfacing in the settings-enumeration path.

**ROOT IDENTIFIED (2026-06-11, workflow analysis, confidence medium):** the corrupt
index is NOT from the `.int` data (NtReadFile serves `[Public]..Object` correctly)
and is NOT an arithmetic step — it is **stale memory** at `[0x401FFC00+0x20]` left by
**FMallocWindows pool aliasing that OUR layer creates**. Object `0x401FFC00` lives in
a 64KB FMallocWindows pool (`VirtualAlloc` base `0x401F0000`, size 0x10000, caller
UT.exe `0x109020F8`). FMallocWindows handed that sub-block out while it still aliased
another live allocation, so a colliding write dropped `0x4250A` into the Name slot.
The aliasing is **enabled by our `FMW-POOL-SKIP` band-aid** (`arch/x86/kernel/idt.c`
~1313-1351): it watches UT.exe's pool manager (RIP `0x10902000..0x10903400`, e.g.
`0x10902AF2`/`0x10902B2D`) do NULL-target writes (`*PrevLink`/`*FirstMem`, `CR2<0x1000`)
and silently **skips** them — leaving the pool link/free-list un-updated, so coalesce/
unlink never happens and blocks get re-handed-out overlapping. Those NULL targets are
themselves a symptom: the pool→`FPoolInfo` indirection (FMallocWindows masks a pointer
to its 64KB base and indexes `PoolIndirect[ptr>>16]`) returns the wrong/empty FPoolInfo
for the `0x401Fxxxx` pool, because `win32_va_alloc`/`sys_NtAllocateVirtualMemory`
(`arch/x86/win32/ntsyscall.c` ~182-251, 717-769) **ignore MEM_RESERVE vs MEM_COMMIT**
and don't guarantee FMallocWindows-compatible 64KB pointer→FPoolInfo slotting
(sub-64KB pool blocks 0xF000/0xE000/0xC000 from caller `0x109020F8` interleave with
multi-64KB large allocs from caller `0x109023F4`).
**Proposed fix (LAYER, not band-aid):** (1) delete the `FMW-POOL-SKIP` skip block in
`idt.c` — it masks meaningful pool writes and is the proximate cause of aliasing;
(2) implement real **MEM_RESERVE vs MEM_COMMIT** in `NtAllocateVirtualMemory` (reserve
address space, commit on demand) so the engine's reserve-then-grow pool yields the same
base/size it later masks against; (3) round each VirtualAlloc reservation up to a full
64KB slot so two distinct FMallocWindows allocations never share a `>>16` PoolIndirect
slot. Then `*PrevLink`/`*FirstMem` are never NULL and no skip is needed.
**OPEN (needs QEMU + UT.exe disasm):** UT.exe FMallocWindows code (`0x10902xxx`) is not
in-tree (only Core.dll + windrv.bin are), so the exact `PoolIndirect` shift/table-base
is inferred from UE1 behavior, not confirmed by disasm. Removing `FMW-POOL-SKIP` alone
may surface the now-unmasked NULL write unless the reserve/commit+granularity fix lands
first. This is the SINGLE fix for B8 (and thus B9). See `project_ut99_b8_fmw_pool_aliasing`.
Prior context: `project_ut99_fmw_pool_fix_loadmap` (FMW-POOL-SKIP was itself a workaround
— lesson: stale workarounds cause later crashes once the real root is fixed).

**ROOT CORRECTED (2026-06-11, runtime probe — it is NOT reserve/commit, it is an
ALLOCATOR MISMATCH).** A B8-PROBE at the FMW-POOL-SKIP site dumped the freed block
ptr `[ebp+8]`, the GMalloc `this` `[ebp-0x28]`, and the FPoolInfo node `[ebp-0x14]`:
`[FMW-POOL-SKIP] @0x10902AF2 blk=0x019AA000 slot=0x9A node=0x00001340 this=0x1092F738`.
The freed block `0x019AA000` is in **low physical memory** (the kmalloc/CRT/Heap pool
region, next to PE images — e.g. WinDrv maps to PA `0x019AC000`), **NOT** in the
FMallocWindows VirtualAlloc range (`0x42xxxxxx`). The PoolIndirect first-level entry
for it is NULL: `node = [this + (ptr>>16)*4 + 0x25c] + ((ptr>>16)&0xff)*0x20`
= `0 + 0x9A*0x20 = 0x1340` ⇒ garbage near-NULL node ⇒ the `*node->PrevLink`/`pool->Head`
writes hit NULL. So the real **FMallocWindows::Free is being handed a foreign pointer**
it never pool-allocated. Origin: our **stub allocator handoff**. During DLL preload
(before the EXE's appInit creates FMallocWindows' Heap) our stub GMalloc
(`msvcrt_shim.c` `stub_fmalloc_*`) + `HeapAlloc`/`crt_malloc` allocate from a `kmalloc`
pool → low `0x019xxxxx` addresses. The `msvcrt_shim.c:56-60` comment ASSUMED "FMallocWindows
uses HeapReAlloc/HeapFree on these pointers after it takes over" — but the disasm proves
THIS build's FMallocWindows is a **custom PoolIndirect allocator, not a Win32-heap wrapper**,
so it pool-frees the foreign low block → miss → corruption. (Reserve/commit is a red
herring; the 64KB slotting is already correct.)
**Real fix:** route allocator calls by pointer ownership. Install a Free/Realloc router
on GMalloc's vtable (slots [1]/[2]) that sends low/foreign pointers (< 0x40000000, our
kmalloc/Heap range) to `HeapFree`/`HeapReAlloc` and forwards native (`0x42xxxxxx`)
pointers to the real FMallocWindows method. Prefer a 32-bit branching thunk (tail-jump to
the real method for native ptrs) to avoid a 64→32 callback on the hot free path. Then the
`FMW-POOL-SKIP` band-aid and the wcscpy(L"0") cascade both disappear. FMalloc::Free is
__thiscall (ecx=this, [esp+4]=ptr). See `project_ut99_b8_fmw_pool_aliasing`.

## B9 — [CLOSED: duplicate of B8] not-present Core.dll pages during Preferences
**Resolution (2026-06-11):** root-caused as a pure **symptom of B8** (FName
corruption wild-writing page-table memory → Core.dll pages go not-present → guest
instruction-faults). See "Analysis 3" below. No independent fix exists; fixing B8
removes B9. History retained below for the paging-layer lessons.
**Symptom:** the B8 AV (recoverable; UE1 guards config code) becomes FATAL because
our SEH dispatch double-faults the KERNEL while handling it.
**Captured:** `compat32_seh_dispatch` reads the SEH frame's handler bytes at
`0x10173C1A` (Core.dll EH handler) → kernel `#PF`, `CR2=0x10173C1A`, not-present.
The CPU fault is authoritative: that page is genuinely NOT mapped in the dispatch's
active CR3 (`0x01000000`, the kernel CR3) — even though `pe_alloc` maps all of
Core.dll's SizeOfImage (0xC0000, which covers RVA 0x73C1A) into both kernel and
win32 CR3 at load. Hypothesis: the EH-handler pages are never *executed* by UT
(only read by the OS dispatcher = us), so if the mapping is lazy/partial for those
pages they stay not-present in the CR3 the dispatcher uses. Frames 0-5 all had
handlers in the unmapped 0x10173xxx region.
**Attempted (reverted):** a `seh_va_readable` CR3 page-walk guard that skips
unmapped frames. It correctly detected the unmapped pages and stopped the *first*
double-fault, but skipping the real handlers just dispatched to a wrong/outer catch
and hit a *secondary* kernel fault (an FName::Names monitor read at `0x10295D30`),
and it doesn't make Preferences work (the real handler is needed). Reverted as a
band-aid that adds an unverified page-walk to the exception hot path without fixing
the user-visible crash.
**Real fix direction:** make the EH-handler (and any never-executed PE) pages
readable to the dispatcher. `pe_image_fixup_page` (re-install a not-present PE
page's PTE from the load-time phys, tracked in pe_alloc) is the right repair
primitive. **CRITICAL — HOW to invoke it:**
- Attempt 1 (2026-06-07, REVERTED): hooked `pe_image_fixup_page` ONLY in the #PF
  handler (idt.c) and retried. HARMFUL for the `seh_dispatch` path — caused a
  **KVM triple-fault / `paused (internal-error)`**: a guest NULL vtable call
  (`CR2=0x40`) → #PF (on IST3) → `compat32_seh_dispatch` reads an unmapped PE
  handler page → **nested #PF re-enters IST3** (RSP reloaded to IST3_top,
  clobbers the outer handler frame) → triple fault. So relying on a nested #PF to
  repair is unsafe inside the #PF/IST3 context. (It IS safe for the non-IST3
  INT2E path — the gameplay FName::Names read — which is why the gameplay crash
  alone could be repaired that way.)
- REQUIRED approach: **PRE-PROBE** — before each guest PE read in
  `compat32_seh_dispatch` (handler bytes @compat32.c:2031, FuncInfo @2043,
  TryBlockMap, HandlerArray, scopetable, frame chain) AND the FName reads in
  `compat32_dispatch`, call `paging_va_present(va)` and if absent
  `pe_image_fixup_page(va)` — so NO nested #PF ever occurs. Then the dispatch
  reads the real handler, UE1 catches the B8 AV, Preferences fails gracefully or
  works. A #PF-handler hook may stay ONLY as a backstop for non-IST3 contexts.
Pair with fixing B8's root so the AV doesn't fire at all. Focused paging-layer
task — do it carefully, it's in the critical exception path.

**Attempt 2 — PRE-PROBE, built + tested (2026-06-08, REVERTED as incomplete):**
implemented `pe_image_fixup_page` (winexec.c, phys-tracked) + `paging_va_present`
(paging.c, CR3 walk) + `seh_ensure_pe` pre-probes in compat32_seh_dispatch and the
compat32_dispatch FName read. With B3 fixed (so the New-Game CR2=0x40 no longer
masks the result), Preferences was tested cleanly. RESULT: the pre-probe MECHANISM
WORKS — `[PEFIX] repaired PE page 0x10295000 -> PA 0x01C7B000` fired (FName::Names
page repaired, content correct since the engine kept running). BUT it's
**whack-a-mole**: the next fault moved to `isr_handler` (idt.c) itself reading a
not-present Core.dll code page (`CR2=0x10102E14`) in the CRASH-DUMP path — a guest
PE read OUTSIDE the dispatcher pre-probes. Recovered to shell (no triple-fault).
So Preferences still crashes. TWO remaining problems: (1) MANY kernel paths read
guest PE memory (dispatcher + isr_handler diagnostics/EBP-walk/code-dump); pre-
probing each is fragile — the ROOT (why eagerly-mapped Core.dll pages go
not-present) must be found instead. NOT memcompress (inactive, doesn't target PE).
Candidates: the 2MB→4KB split TLB dance (paging.c ~139, QEMU-TCG stale entries), or
a later remap. (2) Even with all reads repaired, the AV is B8 (corrupt FName index)
so UE1's catch may not fully recover → Preferences may not open. Also: the pre-probe
adds a CR3 page-walk to EVERY INT2E (thousands/frame) = real gameplay overhead.
REVERTED to keep B3 clean + no overhead. **Next time:** find the not-present-PE ROOT
(instrument what unmaps the page between pe_alloc and the fault), fix B8's index, and
only then the dispatcher reads + isr_handler dump are moot.

**Analysis 3 — root resolved: B9 IS a symptom of B8 (2026-06-11).** Re-read the live
fault trail with B3 fixed. The FIRST fault of the Preferences crash is NOT a
dispatcher read — it is the **guest itself instruction-fetching a not-present page**:
`[EXC32] vec=14 RIP=0x10102E14 err=0x10 CR2=0x10102E14` (`err` bit4=1 ⇒ I-fetch,
bit0=0 ⇒ not-present). `0x10102000` is the *second page of Core.dll `.text`*
(`.text` VMA 0x10101000) — code the menu runs on constantly, so it was mapped and
went **not-present at runtime**. It faults immediately after
`[wcscpy] src=L"0" dst=0x42930074 inner_eip=0x10122E80 outer_eip=0x1010796D` — the
B8 corrupt-FName write. Disasm of `0x10107900` (objdump Core.dll) shows an
FString-build (`rep movs` + the `wcscpy` thunk at `0x10122e80→IAT`) **appending the
decimal string "0"** = the classic index-0/None0 FName signature; `dst=0x42930074`
is a wild heap pointer. The other not-present page `0x10295000` (PEFIX-repaired)
sits in Core.dll's `.data`→`.idata` **section gap** (.data ends 0x101b4000, .idata
0x102ab000) — mapped (zero-filled) by whole-image `pe_alloc` but never section-backed.
The two dead pages are in **different 2MB regions** (0x101xxxxx vs 0x102xxxxx) ⇒
**two separate page-table corruptions**, consistent with B8 wild writes landing on
page-table physical pages (`pt_alloc_page` draws from the same low-mem pool as PE
backing and the engine heap, so a corrupt FName pointer can clobber a PT page →
clears 512 PTEs = a 2MB swath of Core.dll). **Conclusion:** NO paging-layer repair
(pre-probe, #PF fixup) can fix B9 — they are band-aids for memory the engine itself
is corrupting. **B9 is closed as a duplicate of B8.** The single fix is B8's corrupt
FName index/pointer (the long-running None0/FName-table saga, here surfacing in the
`.int`-driven Preferences-enumeration path). See `project_ut99_b9_is_b8_symptom`.

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
