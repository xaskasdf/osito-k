# Win32 Layer Correctness — ABI Inventory & Fix Plan

> **Goal:** make the osito-k Win32 compat layer correctly *execute Win32 apps*,
> using the NT/Win32 sources as the spec — **not** patch the layer/binary for one
> executable (UT99). See memory `feedback-win32-fix-layer-not-binary`.
>
> This doc is the living inventory we iterate on. Last update: 2026-09-01.

---

## 1. How the layer dispatches a Win32 call (mechanism)

PE32 code runs in 32-bit compat mode. Each imported function's IAT entry is
patched (in `compat32.c` import resolver, ~line 1016) to point at a generated
**thunk** in low memory. The thunk (`emit_thunk`, compat32.c:188) is:

```
B8 <idx>        MOV EAX, thunk_index
B9 <nargs>      MOV ECX, num_args
CD 2E           INT 0x2E              ; enter 64-bit kernel dispatcher
C2 <n*4> | C3   RET n*4 (stdcall) | RET (cdecl)   ; <-- STACK CLEANUP
```

The kernel handler (`compat32_dispatch`) reads the 32-bit stack, calls the
64-bit shim, returns. **The thunk's `RET n*4` is where correctness lives:**

- **stdcall** (all Win32 API DLLs): callee cleans → `RET n*4`. If `n` ≠ the real
  arg count, the thunk over/under-pops the caller's stack → the caller's
  **callee-saved EDI/ESI/EBX/EBP get corrupted** → garbage vtable calls, pool
  corruption, wild jumps. **This is the root of every band-aid.**
- **cdecl** (msvcrt/ucrt): caller cleans → `RET` (n is irrelevant for cleanup).

### The two decoupled lookups (the design flaw)

| What | Decided by | Keyed on | Failure mode |
|------|-----------|----------|--------------|
| `num_args` (n) | `guess_num_args(name)` compat32.c:454 | **function name** | **default `return 4`** for any name not in the hand-maintained `known[]` table → wrong `RET n*4` |
| callconv | `dll_calling_convention(dll_name)` compat32.c:849 | **DLL name** | msvcrt*/ucrt*→cdecl, else stdcall; per-fn C++ mangled overrides in dllloader.c |

The arg-count is **divorced from the shim function pointer**. The shim
`GlobalAddAtomW_stub` is declared `WINAPI ...(1 param)` — ground truth = 1 — but
the layer re-derives "1" from a separate name table, and if the entry is missing
it silently guesses 4. That mismatch *is* the GlobalAddAtomW bug, and the
remaining band-aids mask more of the same.

### Ground truth = the shim prototype

Every shim is declared in its `.h` with the real signature:
`RET WINAPI Name(p1, p2, …)`. So:
- **arg-count (32-bit stack DWORDs)** = number of params, counting any
  64-bit-by-value param (`__int64`, `double`, `LARGE_INTEGER`/`ULARGE_INTEGER`
  by value) as **2** slots. (Win32 params are otherwise 4-byte DWORD/ptr/HANDLE.)
- **callconv** = `WINAPI`/`APIENTRY`/`CALLBACK`/`PASCAL` → stdcall; plain `__cdecl`
  or no marker on a CRT export → cdecl.

**The correct fix (Phase 1):** carry `argc` + `callconv` *with the shim
registration* (co-located with the function pointer), so `dll_resolve_import`
returns them together and the thunk is emitted with values that **cannot drift**
from the prototype, and **eliminate the `default 4`** (a miss becomes a loud
error, never a silent wrong guess).

---

## 1.5 What NT actually does (verified against the leaked source)

> Source: `D:\Stuff\dev\Microsoft leaked source code archive_2020-09-24` →
> `windows_2000_source_code` (extracted to `_extracted/win2k`). Spec for
> `feedback-win32-fix-layer-not-binary` / [[reference-leak-source]].

**(a) The NT loader never computes or stores an arg count.**
`LdrpSnapThunk` (private/ntos/dll/ldrsnap.c:2656) resolves an import to the raw
function VA and writes it straight into the IAT:
```c
Addr = (PULONG)((ULONG_PTR)DllBase + ExportDirectory->AddressOfFunctions);
Thunk->u1.Function = ((ULONG_PTR)DllBase + Addr[OrdinalNumber]);   // raw VA, no argc
```
The `call [IAT]` jumps directly into the real DLL function, whose compiler-emitted
`ret N` (stdcall) does the cleanup. **The arg count lives in the callee's
epilogue, never in a table.** Our thunk + arg table only exist because our
"callee" (the shim) is 64-bit and cannot emit a 32-bit `ret N` — a self-inflicted
artifact of the 32→64 boundary. Conclusion: **a per-API arg count is mandatory
for cross-ABI thunks** (you cannot avoid it for stdcall — the caller will not
clean), so the *category* of what we do is correct.

**(b) When NT DOES thunk across an ABI, it declares each API's typed signature —
it never guesses.** The Win32 thunk compiler is fed `.thk` spec files
(private/shell/thunk/*.thk) that declare every thunked export with its **typed
signature**, e.g.:
```
BOOL GetOpenFileName(LPOPENFILENAME lpOfn) = BOOL ThkGetOpenFileName(...) { ... }
```
The compiler derives stack size + marshaling from the **types**. Every thunked
API is listed; there is **no default-guess path**. This is the gold-standard
model and exactly what Phase 1 should imitate: argc/callconv derived from the
real signature, complete, mechanical.

**(c) For C++ (mangled) imports, the exact signature is *in the name* — derive
it by demangling.** `undname.cxx`'s `getCallingConvention()` (private/windbg64/
langapi/undname/undname.cxx:2032) decodes the callconv from one char
(`callCode = *gName - 'A'`: `A`=__cdecl, `G`=__stdcall, `E`=__thiscall,
`I`=__fastcall) and parses the typed arg list. UE1's cross-DLL imports
(Core.dll/Engine.dll/.u, all `?Name@@YA…@Z`) currently fall to default-4; a
small MSVC demangler gives **exact argc + callconv**, deterministically. e.g.
`?appUnwindf@@YAXPBGZZ` → `A`=__cdecl, ret `X`=void, args `PBG`=`const WCHAR*`,
`ZZ`=`...` → cdecl, varargs, 1 fixed arg (matches the hand-coded override).

### Verdict on "are we doing num_args all wrong?"
Half-right. The **approach** (per-API arg count) is unavoidable and matches NT's
thunk compiler. The **implementation** is wrong in two specific ways NT never is:
1. **We guess** (`return 4`). NT declares every thunked API; a miss is impossible.
   → Eliminate default-4: a miss becomes a loud `[ABI-MISS]` error.
2. **We key on the name, divorced from the signature.** NT derives the stack size
   from the **type** (.thk) or by **demangling** (undname). → Derive argc from the
   shim prototype (co-located), and demangle `?…@@…` imports.

### Important caveat (measured)
In a real UT99 run, of the ~68 functions hitting default-4, **almost all Win32
stdcall ones genuinely have 4 args** (`MoveToEx`, `CreatePipe`, `RegisterHotKey`,
`GetMenuItemInfoA`…) and the rest are cdecl CRT (`ceil`, `qsort`, `_ftol` →
caller cleans, argc irrelevant). So the default-4 is a **latent** landmine, **not**
what the surviving band-aids (ENGINE-PATCH/BROWSE-FIX/…) mask. Their root is a
*wrong table entry*, a callconv error, or int2e register preservation — found by
Phase 2 (disable→observe→root-cause), not by completing the table. Phase 1 is
correct foundational hygiene that removes the class; it is not expected to delete
the band-aids on its own.

---

## 2. Inventory legend

For each shim function: **GT-argc** = ground-truth arg DWORDs from the prototype;
**GT-cc** = ground-truth callconv; **tbl-argc** = what `known[]` returns (or
`DEF4` if it falls through to the default); **status**:

- ✅ `OK` — tbl-argc matches GT-argc (and cc correct)
- ❌ `MISMATCH` — tbl-argc ≠ GT-argc (active stack-imbalance bug, stdcall only)
- ⚠️ `DEF4` — not in table → gets 4; **bug iff GT-argc ≠ 4 and stdcall**
- 🟡 `CC?` — callconv ambiguity (cdecl export in a stdcall DLL, or vice-versa)

---

## 3. Per-DLL function inventory

> Filled from parallel extraction of the shim `.h` prototypes (ground truth)
> cross-referenced against the `known[]` table in compat32.c:459-810.

> Ground truth (GT) extracted from shim `.h`/`.c` prototypes. `tbl` = value
> `known[]` returns today (`—` = absent → falls to **default 4**). Only rows where
> **tbl ≠ GT** are listed as ❌; everything else in the prototypes matched or is
> cdecl (cleanup-irrelevant). Full per-function GT lists archived below the verdict.

### 3.9 ⭐ CROSS-CHECK VERDICT — confirmed wrong entries (the bug surface)

**(A) OVER-CLEANERS — stdcall, GT < 4, absent from `known[]` → default-4 RET 16
over-pops the caller stack → corrupts callee-saved EDI/ESI/EBX. THIS IS THE
GlobalAddAtomW CRASH CLASS.** Prime Phase-2 suspects (verified absent via grep):

| Function | DLL | GT | tbl | over-pop | likely imported by UT99 |
|---|---|---|---|---|---|
| `lstrlenA` / `lstrlenW` | kernel32 | 1 | — (4) | 12 B | **yes — extremely common** |
| `GetSystemTimeAsFileTime` | kernel32 | 1 | — (4) | 12 B | **yes — UE1 timing/appInit** |
| `IsDebuggerPresent` | kernel32 | 0 | — (4) | 16 B | **yes — CRT/appInit** |
| `GetTickCount64` | kernel32 | 0 | — (4) | 16 B | maybe (returns ULONGLONG) |
| `GetEnvironmentStringsA` | kernel32 | 0 | — (4) | 16 B | maybe |
| `PulseEvent` | kernel32 | 1 | — (4) | 12 B | maybe (threading) |
| `TryEnterCriticalSection` | kernel32 | 1 | — (4) | 12 B | maybe (threading) |
| `UnmapViewOfFile` | kernel32 | 1 | — (4) | 12 B | maybe (file mapping) |

**(B) UNDER-CLEANERS — stdcall, GT > 4, absent → default-4 RET 16 under-pops →
leaves stale args on the stack (logic corruption, slower-burning than A):**

| Function | DLL | GT | tbl | under-pop |
|---|---|---|---|---|
| `CreateFileMappingA` / `CreateFileMappingW` | kernel32 | 6 | — (4) | 8 B |
| `MapViewOfFile` | kernel32 | 5 | — (4) | 4 B |
| `InterlockedCompareExchange` | kernel32 | 3 | — (4) | over-pop 4 B |
| `InterlockedExchange` | kernel32 | 2 | — (4) | over-pop 8 B |
| `LoadLibraryExA` | kernel32 | 3 | — (4) | over-pop 4 B |
| `InitializeCriticalSectionAndSpinCount` | kernel32 | 2 | — (4) | over-pop 8 B |

> NOTE on the agents' "almost all default-4 hits were genuinely 4-arg" finding:
> that survey only covered names that *actually appeared* in one early-crash run's
> log. The functions above are **also** absent from `known[]` but may be imported
> on code paths the band-aids currently keep alive — so they don't show up until
> the band-aids are removed. **Action: add explicit entries for all of (A) and (B)
> BEFORE the Phase-2 band-aid bisect**, so the bisect isn't masking a known
> over-cleaner. The (A) group especially must be fixed first — it is the exact
> mechanism (callee-saved corruption) the EBX-era band-aids were invented for.

**(C) Verified-correct high-arg entries** (tbl == GT, no action): CreateFileA=7,
ReadFile/WriteFile=5, DuplicateHandle=7, WideCharToMultiByte=8,
MultiByteToWideChar=6, CreateThread=6, CreateWindowExA/W=12, BitBlt=9,
CreateFontA/W=14, SetWindowPos=7, ExtTextOutA=8, RegCreateKeyExA/W=9,
RegEnumKeyExA=8(❓ check table), ShellExecuteA/W=6, waveOutOpen=6, surf_Blt=7,
CoCreateInstance=5, GlobalAddAtomW=1 (the original fix). The COM thunks
(DD_*/Surf_*/Pal_*) register their argc explicitly via `dd_args[]`/`sf_args[]`,
not through `known[]` — those are self-consistent.

**(D) ntdll syscalls** (NtCreateFile=11, NtMapViewOfSection=10, NtReadFile/
NtWriteFile=9, …) — these are resolved as shims but reach the kernel via a
different path (NtXxx are not stdcall-thunked the same way; they marshal in
`ntsyscall.c`). Audit separately; if any Nt* DOES go through `guess_num_args`,
the high counts (10-11) make a default-4 catastrophic. **TODO: confirm the Nt*
dispatch path.**

**(E) cdecl / msvcrt** — caller cleans, so a wrong argc does **not** corrupt
cleanup; it only changes how many DWORDs the INT 0x2E dispatcher copies. Real
risks there are structural, not arg-count: (1) **`double`-taking fns** (`_CIpow`,
`_CIfmod`, `_CIacos`, `_isnan`, `_ftol`, `ceil`, `floor`, `difftime`) — each
`double` = 2 stack DWORDs and the `_CI*` intrinsics use the **x87 register**
convention, not the integer stack; (2) **`double`-returning fns** (`atof`,
`difftime`) return in **ST(0)**, not EAX:EDX; (3) **variadic** printf family —
dispatcher must expose the live 32-bit stack, not a fixed copy; (4)
**`??1type_info@@UAE@XZ`** is **thiscall** (`this` in ECX, 0 stack args). These
are correctness items for the dispatcher, tracked in §1.5(c)/Phase 1b.

### 3.10 Full per-function ground-truth tables (reference)
The complete GT extraction (every exported function, cc, argc) for all DLLs is
preserved verbatim in the agent run; the actionable deltas are §3.9 above. Key
high-arg references kept inline: **kernel32** CreateFile*=7, Read/Write=5,
Duplicate=7, W2MB=8, MB2W=6, CreateThread=6, RegCreateKeyEx*=9, RegEnum*=8;
**user32** CreateWindowEx*=12, SetWindowPos/TrackPopupMenu/SendMessageTimeout=7,
Move/DrawTextEx/LoadImage=6, CallWindowProc/Dialog*/Peek*/DrawText/ToAscii=5;
**gdi32** CreateFont*=14, BitBlt=9, ExtTextOut=8, PatBlt/CreateDIB*=6,
CreateBitmap/TextOutW=5; **ddraw** Surf_Blt=7, BltFast=6, SetDisplayMode=6,
Lock/CreatePalette/EnumModes/Pal*=5; **dsound** dsb_Lock=8(direct vtbl, untunked),
dsb_Unlock=5; **winmm** waveOutOpen=6, timeSetEvent=5; **wsock32** recvfrom/
sendto=6, select/getsockopt/setsockopt=5; **ole32** CoCreateInstance=5;
**shell32** ShellExecute*=6.

---

## 4. Fix plan (in order)

- [x] **Phase 1 — ABI correctness, signature-derived (NT `.thk` model). DONE 2026-06-04.**
  - [x] 1a. Co-located `{argc, callconv}` with each shim export (`WIN32_EXPORT`
    in `win32_abi.h`). All 14 shim tables migrated (kernel32/user32/gdi32/msvcrt/
    ntdll/advapi32/ddraw/dsound/winmm/wsock32/ole32/shell32/comctl32/comdlg32),
    argc derived from each prototype, registered via `win32_abi_register` in
    winexec.c. The IAT patcher (compat32.c) now calls `win32_abi_lookup` first.
  - [x] 1b. MSVC demangler (`msvc_demangle_abi` in win32_abi.c) — decodes
    callconv (`A`=cdecl/`G`=stdcall/`E`=thiscall/`I`=fastcall) + counts arg
    DWORDs from `?…@@YA…@Z`; conservative (bails → miss on by-value user types).
  - [x] 1c. Removed `guess_num_args`, its `known[]` table, DLL-level calling-
    convention guessing, and the default-4 fallback. IAT patching and compat32
    `GetProcAddress` resolve the contract by export target first, then by name or
    MSVC demangling. An unknown contract now fails closed with `[ABI-MISS]` and
    `STATUS_PROCEDURE_NOT_FOUND`/`ERROR_PROC_NOT_FOUND`.
  - **RESULT:** no guessed stack cleanup remains. `win32_abi_selftest` covers
    exact, target-alias, mangled-name, and miss behavior; the module-image test
    passes 41/41. UT99 reaches DirectDraw exclusive 640x480x16 surface creation
    and the first present with no ABI miss or CPU fault.
  - Phase 1 exposed the remaining historical workarounds for removal in Phase 2.
- [x] **Phase 2 — Remove address-specific UT99 band-aids. COMPLETED.**
  - The runtime no longer contains fixed Core/Engine/UT image ranges, absolute
    instruction probes, default UT99 hooks, or mutation callbacks. `hwbp.c`
    retains only the generic four-slot hardware-breakpoint dispatcher; `wdbg.c`
    retains loader-backed symbol lookup and explicit hook registration.
  - The final audit removed the UE1-specific FName/UObject inspectors, the
    no-op base-SEH frame, exact-size `HeapAlloc(0x54)`/FName reallocation traces,
    and application-conditioned exit diagnostics. Normal SEH dispatch, ABI
    metadata, loader-backed symbols, and the recent-call crash ring remain.
  - The historical steps below document how the original faults were isolated;
    they are not active runtime behavior.
  - **Step 1 (done):** removed the HWBP-bisect arming (winexec.c) + FNDIFF FName
    tracer (compat32.c) — pure instrumentation that flooded runs. A full run to
    the LoadMap frontier dropped 252k→115k lines, frontier intact. Runs are now
    measurable for the bisect.
  - **Step 2 (done):** non-invasive bisect — counted each band-aid's fires in a
    clean Phase-1 run (115k lines, reaches frontier):

    | band-aid | fires | verdict |
    |---|---|---|
    | BROWSE-FIX | **0** | dead → **removed** |
    | NULL-REDIRECT | **0** | dead → **removed** |
    | FMW-REPAIR | **0** | dead → **removed** |
    | ENGINE-PATCH | 1 (preventive) | load-bearing at this checkpoint; later root-caused and removed |
    | FMW-POOL-SKIP | 6 | active at this checkpoint; later removed |
    | GOBJREG-FORCE | 2 | active at this checkpoint; later removed |
    | FNAME-NULL-FILL / FNAME-RESCUE | 151 / 3 | active at this checkpoint; later removed |

    Removing the 3 zero-fire band-aids (212 lines) was byte-for-byte neutral —
    re-ran to 115082 lines, same Browse + LoadMap + "Can't find Entry.unr"
    frontier, crash 0.
  - **Step 3 (done):** the load-bearing ENGINE-PATCH was traced to the
    `GetProcAddress` ABI path and removed after fixing its stack-cleanup metadata.
    The remaining initialization overrides were retired; the runtime contains no
    FMW/GOBJ/FNAME mutation path.
  - Verification on 2026-09-01: module-image test 41/41, then UT99 reached
    DirectDraw exclusive 640x480x16, primary/offscreen surfaces, and first
    present with no ABI miss, CPU fault, or unhandled exception.
- [x] **Phase 3 — Init-ordering cleanup.** Application-state injection has been
  removed. Future initialization defects are handled through PE loader, CRT,
  process-state, and memory-manager contracts rather than executable-specific
  recovery.

## 4.5 ENGINE-PATCH root-cause — FOUND & FIXED (Phase 2 step 3, 2026-06-04)

**ROOT CAUSE (definitive, layer bug): `GetProcAddress` (kernel32_shim.c) hardcoded
a 4-arg thunk for every function it resolved.** `UWindowsClient::Init` does
`GetProcAddress(ddraw, "DirectDrawCreate")` (3 args); the 4-arg thunk's `RET 16`
**over-cleaned 4 bytes**, so a later `push &obj->field(+0xF0)` in Init landed on
its own saved-EBX stack slot `[ebp-0x2b8]`. Init's `pop ebx` then restored that
object pointer (`obj1+0xF0` = `0x4020C870`) instead of the caller's EBX, and the
engine's next `call ebx` jumped into the object and #PF'd — the crash ENGINE-PATCH
masked. Pinned deterministically by tracking `[ebp-0x2b8]` live across every shim
call from Init (jitter-immune; the per-run stack base varies via ASLR-lite, which
had defeated hardcoded-address HWBP/watchpoints).

**FIX:** `GetProcAddress` now uses `win32_abi_lookup(shim_dll, name, &argc, &cc)`
(the Phase-1 co-located ABI mechanism) for the real argc + callconv, instead of a
hardcoded 4. **ENGINE-PATCH deleted.** Verified: DirectDrawCreate thunk argc 4→3,
the saved-EBX slot stays intact through Init, the `0x4020C870` crash is gone, and
UT99 reaches the same Browse + LoadMap("Can't find Entry.unr") frontier (~121k log
lines) **with no band-aid**. This is exactly the Phase-1 class of bug (a wrong argc
producing stack-cleanup corruption), just on the GetProcAddress thunk path the
initial migration didn't cover.

### Earlier investigation (how we got here)
ENGINE-PATCH byte-NOPs `call [edx+0x54]` @Engine.dll `0x1038887A`. Step 3 chased
its real root.

**Confirmed mechanism (via the #PF stack dump).** The crash is `UGameEngine::Init`
(F)'s `call ebx` @`0x1038888F`: `[RSP+0]=0x10388891` (F's return addr), `RBX=
0x4020C870` = `obj1+0xF0` where obj1 = the `ViewportManager` (= `WinDrv.WindowsClient`)
object F builds. The kernel's own detector (idt.c) prints `*0x105A5E08=0x10101820
(StaticLoadClass) … IAT OK, EBX clobbered USER-SIDE`. So `UWindowsClient::Init`
(WinDrv `0x11101720`) returns having restored F's callee-saved EBX/ESI/EDI as
garbage → a **32-bit stack imbalance inside Init** leaves an object-field pointer on
the stack at the saved-EBX slot. **Disproven**: corrupt vtable (vt=0x1110C928 valid
WinDrv, [vt+0x54]=0x11101720 valid) and damaged class/object (header valid: Class,
Outer, Name all set; mid-Init esi=appStricmp / ebx=Logf both valid). Phase-1 shim
argc is correct, so the under-cleaner is a virtual call / callback-arg cleanup /
`_alloca`(`__chkstk`) / an un-audited argc — pin it with a **data write-watchpoint**
on Init's saved-EBX slot (deterministic, unlike exec-HWBPs).

**NT/WOW64 reference for the 32↔64 boundary** (`nt5src/.../base/wow64/cpu/amd64/
cpu/amd64/simulate.asm`): `CpupRunSimulatedCode` (64→32) saves 64-bit non-volatiles
+ restores the 32-bit register context from a **per-thread** x86 CPU-context struct;
`CpupReturnFromSimulatedCode` (32→64) saves the **full** 32-bit context into that
per-thread struct. Discipline = full register set preserved, per-thread, every
transition.

**Our audit vs NT.** `int2e_stub.S` saves all 15 GPRs on the IST1 stack and passes
the exact saved frame to `compat32_dispatch`; normal return and non-local unwind
are applied to that same frame. Callback depth, jump buffers, low callback stacks,
captured INT2E registers, pending unwind, SEH scratch, and call diagnostics are now
owned by the scheduler slot rather than mutable process-wide globals. Process
scheduling is BSP-only: AP LAPIC timers remain masked and an unexpected timer
vector on an AP is acknowledged without entering the global scheduler.

MSVC i386 EH3 filters and handlers now enter with `EBP = EstablisherFrame + 16`,
and the dispatcher publishes `EXCEPTION_POINTERS` at `[EBP-0x14]`, matching the
compiler funclet ABI. This fixes the recursive `_except_handler3` fault where a
filter inherited a kernel RBP. Both callback transition blocks end in
`__builtin_unreachable()` and `kern_longjmp` is declared `noreturn`, so the
compiler no longer models `lretq` as a fall-through path with preserved registers.

`arch/x86/test/seh3_pe32.c` is the deterministic PE32 regression for this
boundary. It allocates a page, changes it to `PAGE_NOACCESS`, and faults inside
nested MSVC `__try` scopes whose outer filter captures an establishing-frame
local and whose inner scope has a `__finally`. A passing run observes one access-
violation page fault, returns `EXCEPTION_EXECUTE_HANDLER`, executes the termination
funclet, transfers to the handler using the establishing ESP/EBP, continues after
the guarded block, returns to its caller, and exits with code 0 without nested
dispatch, `#GP`, or `#UD`.

## 4.6 Callback preemption and SMP rendering (2026-09-04)

PE32 callbacks no longer save, mask, or restore the BSP LAPIC timer. Callback
frames already belong to individual scheduler tasks; restoring a CPU-wide mask
after a callback blocked could restore another task's stale mask and stop timer
preemption. The transition protects segment/stack changes, and normal callback
return restores the caller's interrupt flags after restoring callback state.
Process cleanup no longer compensates by forcibly unmasking the timer.

The related rendering audit found three independent SMP issues:
- APs parked before worker initialization needed an explicit startup IPI. Both
  startup and idle waits now check readiness/work with interrupts masked before
  parking. The smoke test distinguishes AP execution from BSP fallback.
- Rejected compositor jobs left entire image bands undrawn. Window copies and
  integer scaling now execute rejected bands locally and wait for accepted jobs.
- Speculative prefetch retained mutable arguments and a process CR3. The BSP now
  snapshots targets while the process is current, with one pending job and at
  most two page translations. AP workers never walk a departed address space.

Validation commands:
```sh
make -C arch/x86 CLANG=1 -j4
bash arch/x86/scripts/test-compositor-bands.sh
bash arch/x86/scripts/test-spec-prefetch.sh
sh arch/x86/scripts/build-callback-preemption-test.sh
```

The host tests pass with GCC and clang: 880 compositor cases and 1101 prefetch
checks. The compositor test also passes with ASan/UBSan. Copy the generated
`callback_preemption_pe32.exe` and its `.autoload` fixture to a disposable guest
disk to run the callback test. The identical PE32 binary failed with exit 4 on
the previous timer-masking kernel and passed twice with exit 0 on the fixed one.
SEH3, SEH4, unwind32, legacy WinMM timers, and WaveOut also exited with code 0;
native USER32 window/input/dialog, DirectDraw, and DOS API contracts passed.

In the UT99 QEMU run, fullscreen/windowed transitions preserved the complete
image and continued receiving newly pressed Escape keys; the BSP timer stayed
unmasked. This was a menu-level smoke test, not a gameplay/performance benchmark.
PE32 `CallWindowProcA` subclass dispatch, Alt+F4/system-key semantics, and the
UT99 menu-click workflow remain unverified or incomplete; they are not covered
by the fullscreen/preemption result. Section 4.7 records the subsequent
subclass and keyboard contract work.

## 4.7 PE32 subclassing and system-key contracts (2026-09-04)

`CallWindowProcA` now invokes the previous PE32 procedure instead of silently
falling back to `DefWindowProcA`. Callback invocation is separate from message
delivery, so a subclass link does not generate another `WH_CALLWNDPROC` or
return-hook notification. PE32 `LRESULT` values are sign-extended at the
callback boundary, including the `WM_CREATE` return value of -1 that rejects
window creation.

Physical keyboard input and `SendInput` share message classification and
`lParam` construction. Window messages use generic modifier virtual keys while
key state retains left/right identity. Tests cover repeat, extended keys,
Alt/Ctrl release ordering, F10, and an active window without keyboard focus.
`TranslateMessage` generates `WM_SYSCHAR` for system keystrokes. The default
Alt+F4 path posts `WM_SYSCOMMAND/SC_CLOSE` to the root window, respects
`CS_NOCLOSE`, and permits the application to consume the command or `WM_CLOSE`.

Validation commands:
```sh
make -C arch/x86 CLANG=1 -j4
sh arch/x86/scripts/build-subclass-test.sh
```

The generated `arch/x86/build/test-pe32/subclass_pe32.exe` runs unchanged on
Windows and OsitoK. It creates hidden, process-owned windows and installs only
a calling-thread hook. It checks two subclass links, nested sends, high-bit
arguments, signed results, per-window isolation, procedure restoration, native
BUTTON forwarding, hook counts, rejected creation, and the default close path.
The initial subclass probe failed with exit 3 on the previous kernel and passed
on Windows; the final expanded probe passes with exit 0 on both systems. The
`CallWindowProcW` case covers a numeric message, not ANSI/Unicode conversion.

For guest execution, copy the probe to `probes/subclass_pe32.exe` and use
`arch/x86/test/subclass.autoload` on a disposable disk. The final QEMU serial log
at `/root/osito-subclass-20260904-r4/serial.log` records input 147/147, window
model 95/95, dialog 10/10, DirectDraw, DOS API, and DirectSound passes. Callback
preemption, SEH3, and legacy WinMM timer probes also exit with code 0. The SEH3
page fault is intentional and handled by its regression fixture.

UT99 smoke-test artifacts are under `/root/osito-ut99-subclass-20260904-r1/`.
Repeated Alt+Enter transitions preserved the full image and kept the timer and
compositor progressing without an unexpected CPU fault. This does not establish
complete game input compatibility: clicking Options did not open its menu, and
Alt+F4 did not exit UT99 despite delivery of system-key messages. Both remain
open investigations. ANSI/Unicode subclass text conversion, full menu-loop
semantics, and the broader message-hook contract also remain incomplete.

## 4.8 Thread-owned cursor visibility (2026-09-04)

`ShowCursor` now maintains independent display counts per process/thread instead
of one global counter. Only nonzero counts allocate kernel-owned state; balancing
the count, releasing a thread, and releasing a process reclaim that state without
resetting another thread's count. Allocation and freeing occur outside the cursor
spinlock. Relative-pointer translation consults the calling owner's count.

The compositor respects the managed surface owner's count in both ordinary and
fullscreen rendering. Native desktop chrome keeps its pointer. `GetCursorInfo`
reports the pointer target's visibility in both native and PE32 layouts. This
removes the extra native arrow over games that hide it and draw their own cursor;
it does not complete cursor shapes, `SetCursor(NULL)`, `WM_SETCURSOR`, or attached
input-queue semantics.

Validation commands:
```sh
make -C arch/x86 CLANG=1 -j4
bash arch/x86/scripts/test-compositor-cursor.sh
CC=clang bash arch/x86/scripts/test-compositor-cursor.sh
bash arch/x86/scripts/test-compositor-bands.sh
sh arch/x86/scripts/build-subclass-test.sh
```

The production-overlay host test passes 22 checks with GCC, clang, and
ASan/UBSan; the band test passes 880 cases. The expanded `subclass_pe32.exe`
checks independent worker counts and cleanup when a worker exits hidden. It
passes unchanged on Windows. QEMU logs under `/root/osito-cursor-20260904-r1/`
and `r2/` record the same probe failing on the preceding kernel with exit 3 and
passing on the fixed kernel with exit 0. The clean contract run also passes
window, dialog, DirectDraw, DirectSound, DOS API, callback-preemption, SEH3, and
legacy WinMM timer tests. The SEH3 fault is intentional and handled.

UT99 artifacts are under `/root/osito-ut99-input-20260904-r2/`. Options and
Multiplayer menus respond using the game's cursor; fullscreen/windowed switches
preserve the image and restore the appropriate pointer. Console `quit` exits
with code 0 and leaves the cursor-count list empty. The final kernel passes
input 156/156 and window-model 95/95 checks after that exit. UT99 still consumes
Alt+F4 without reaching `DefWindowProcA`; its shutdown behavior, the initial
setup wizard's painting, and the first absolute-to-relative sample remain open.

The subsequent `dos-api-test` did not pass: `dos_vcpi_selftest` faulted while
zeroing a 16 MiB allocation using its physical address as a pointer. The fault
at RIP `0xFFFF800002417740` writes to unmapped `0x10B00000` from allocation
base `0x10991000`. This points to DOS relying on low identity aliases that Win32
image teardown can remove. No baseline UT99-exit/DOS A/B comparison has yet
established whether the cursor changes affect reproduction. The DOS allocation
boundary needs stable kernel virtual mappings while retaining physical addresses
for freeing and guest mappings; no DOS mapping fix is included here. The later
DirectSound command could not run after the halt, so the clean-boot contract
passes must not be read as a passing post-UT99 sequence.

## 4.9 DOS backing memory and native-exit IST3 ownership (2026-09-04)

The post-UT99 DOS fault from section 4.8 also reproduces on the unchanged
`b7a1dcf3` kernel: `dos_vcpi_selftest` writes to unmapped `0x10900000` from
allocation base `0x107BB000`, at the same RIP `0xFFFF800002417740`. Artifacts
are under `/root/osito-dos-hostmem-20260904-r1/`.

DOS host allocations now use `dos_host_alloc_pages` and `dos_host_free_pages`.
The allocator returns the kernel direct-map pointer; only freeing and native
guest/page-table mappings convert back to physical addresses. This covers guest
RAM, image/EXEC buffers, EMS/VCPI state, native table pages, and JIT storage.
Audio and VGA consume that pointer directly without applying the direct-map
offset twice. Guest-visible addresses and the physical allocator are unchanged.
The JIT still relies on an executable kernel direct map; this is not a W^X fix.

The memory fix exposed another native-DOS exit issue: resetting IST3 to the
global bootstrap stack discarded the shell task's private Win32 fault-stack
ownership. The new scheduler helper resets the current owner's private stack
when present, falling back to the global stack only when no private stack exists.

Validation commands:
```sh
make -C arch/x86 CLANG=1 -j4
make -C arch/x86 CLANG=1 dos-dpmi-test dos-vbe-native-test \
    dos-audio-native-test dos-exec-test dos-vbe-test
```

`arch/x86/test/dos_hostmem.autoload` runs `dos-api-test`. Its 13 host-memory
checks include a private CR3 without low aliases, a real high-pointer write/read
under that CR3, zeroed native table allocation, production JIT execution, and
IST3 reset. GCC also compiles the changed `dos_exec.c` and `process.c` separately.

The final QEMU sequence under `/root/osito-dos-hostmem-20260904-r3/` renders the
UT99 intro, exits through console `quit` with code 0, and passes input 156/156,
window-model 95/95, DOS API, and DirectSound contracts. Native DPMI callbacks,
EXEC COM/MZ/load-only children, native VBE, native audio DMA/IRQ, and interpreted
VBE fixtures complete. DPMI and audio fixtures use exit 42 as their success
value. Repeating EXEC, native audio, and DOS API leaves the observed free-page
count unchanged at 1908492. This is a bounded cycle check, not proof of complete
application-memory reclamation. No scheduler owner/IST3 guard warning, double
page free, or unhandled CPU fault appears in that run.

Clean-boot validation under `/root/osito-dos-hostmem-20260904-r4/` also passes
the 13 checks with no private IST3 stack, followed by native VBE/audio and PE32
subclass/callback-preemption probes. However, the subsequent SEH3 probe fails
with an unhandled `#GP` at `0x004010B7` while reading `FS:0`, before installing
its exception handler; it exits with `0xC0000005`. This is not SEH3's expected
handled test fault. The DOS-to-PE32 segment-state transition remains open:
restoring the FS base alone may not restore a usable compatibility-mode segment.
No FS-selector fix or complete interleaving guarantee is included here.

## 4.10 FS/GS ownership across DOS, PE32, and ELF (2026-09-04)

The section 4.9 SEH3 failure is a descriptor-state issue, not just a wrong
FS base. On the unchanged `12b12000` kernel, SEH3 passes before native DOS;
after native VBE, QEMU reports null, unusable FS/GS descriptors. Installing
the PE32 TEB with WRMSR alone leaves `FS:0` faulting at `0x004010B7`.
Baseline artifacts are under `/root/osito-fs-segments-20260904-r1/` and `r2/`.

PE32 TEB setup now loads the flat data selector before writing FS_BASE, with
interrupts masked through publication of the task's TLS state. The scheduler
saves both selectors and bases and restores descriptors before bases. Fork and
clone capture live architectural state rather than stale cached parent bases.

Native DOS saves the host FS/GS state for its session and restores it after the
host GDT is restored. Native entry, interrupt frames, and callback transitions
carry guest FS/GS selectors. Successful DPMI descriptor freeing clears matching
data selectors; invalid frees preserve unrelated selectors. The assembly frame
layout has corresponding C offset/size assertions. Native DOS still excludes
ordinary task scheduling while it owns the descriptor tables; this does not
implement concurrent native DOS sessions.

The extended sequence exposed two more gaps in the intermediate `r3/` run:
ELF `ARCH_SET_GS` returned ENOSYS, and synchronous ELF exit left the shell using
the child's TLS and a reset IST1 cursor. GS get/set is now implemented, and
synchronous exec restores parent TLS and the exact saved IST1/IST3 cursors on
exit and loader failure, before allowing task switches. Noncanonical SET bases
are rejected; GET validates the writable output mapping before copying. Upper
canonical bases remain accepted for the current native ELF stack ABI. These
changes do not complete user/kernel address-space isolation.

Validation commands:
```sh
make -C arch/x86 CLANG=1 -j4
make -C arch/x86 CLANG=1 dos-dpmi-test dos-vbe-native-test \
    dos-audio-native-test dos-exec-test dos-vbe-test
sh arch/x86/scripts/build-teb32-test.sh
sh arch/x86/scripts/build-tls-segments-test.sh
```

The PE32 probe passes unchanged on Windows and OsitoK. It checks four live
worker TEBs, per-thread TLS/last-error values, and window callbacks across
scheduling. The ELF probe passes on Linux and OsitoK with four clone threads,
128 scheduling rounds per thread, independent FS/GS values and selectors, and
fork inheritance. This probe does not yet cover the GET error-boundary cases.

Final QEMU artifacts are under `/root/osito-fs-segments-20260904-r4/` (8 GiB,
four CPUs, KVM, HDA, disposable snapshot disk). The booted kernel's SHA-256
matches the build: `7ba948c41751a18514fbf6d5bf147d62a3f3e2bde70bc38c8fedb0da4c604c90`.
The log records DOS API with 15/15 host-memory checks, native DPMI callbacks
(exit 42), PE32 TEB, ELF TLS/clone/fork, SEH3 (exit 0), and PEB/TEB64 passes.
Native audio (exit 42), native VBE (exit 0), and COM/MZ EXEC (exit 0) also pass,
followed by another PE32 TEB run. Passing a COM fixture to the ELF loader is
rejected with bad magic; the subsequent PE32 TEB probe still passes. Input
156/156, window-model 95/95, and DirectSound contracts pass as well. SEH3's
intentional exceptions are handled; no unexpected CPU fault or scheduler
owner/IST guard warning appears in this run. UT99 was not rerun for this change.

## 4.11 GDI brush identity and rectangle painting (2026-09-04)

Solid and pattern brushes now have independent handles, process ownership,
selection references, and copied pattern pixels. Deleting a source bitmap does
not invalidate its pattern brush. Stock and system brushes are recognized by
`GetObjectType`, and `GetObject` reports the caller's PE32/PE64 LOGBRUSH layout.
`GetSysColorBrush` provides cached system brush handles instead of allocating
objects on each call.

`FillRect` and fill-only BitBlt/PatBlt operations use the brush color or repeating
pattern, viewport origin, destination pixel format, and selected clip region.
`SelectClipRgn` copies region rectangles, including the distinction between an
empty clip and no clip. `CreateBitmap` preserves monochrome input and WORD row
alignment. `DrawFocusRect` draws an XOR border instead of returning success
without painting.

Validation commands:
```sh
make -C arch/x86 CLANG=1 -j4
sh arch/x86/scripts/build-gdi-paint-test.sh
```

The generated `gdi_paint_pe32.exe` and `gdi_paint_pe64.exe` both pass unchanged
on Windows and OsitoK. They check brush identity, LOGBRUSH ABI, selected-object
preservation, PATCOPY, viewport translation, copied and empty clips, monochrome
pattern ownership/colors/origin, system brushes, XOR restoration, and 24-bit
bottom-up row bounds. The `.autoload` fixture starts the PE32 executable from
`probes/`; the PE64 executable is run with `winexec probes/gdi_paint_pe64.exe`.

QEMU artifacts are under `/root/osito-gdi-commit-20260904-r1/` (8 GiB, four CPUs,
KVM, separate snapshot disk). GDI DIB 32/32, window-model 95/95, input 156/156,
dialog 10/10, DirectDraw ABI, and DOS API contracts also pass without unexpected
CPU faults in this run. The fixture disk lacks the optional `diag/` directory,
so boot-log file creation reports parent-missing errors on that disk.

This is not complete GDI or control painting support. DrawText, default control
painting, broader mapping-mode behavior, and clipping in other presentation
paths remain outside this change. UT99's initial wizard was not rerun during
this validation. The probe explicitly releases its objects; it does not prove
complete process-exit reclamation or concurrent GDI lifetime correctness.

## 4.12 USER32 paint lifecycle and dialog backgrounds (2026-09-04)

`BeginPaint` now consumes the pending update before calling `WM_ERASEBKGND`,
clips drawing to that update, and reports whether erasing remains necessary in
the caller's PE32/PE64 PAINTSTRUCT. Callback reinvalidation survives the paint
cycle. The transient paint clip is independent of `SelectClipRgn`; clearing an
application clip cannot bypass it. Nested `EndPaint` on an owned DC removes the
paint clip, matching the native Windows probe. Window destruction releases its
retained GDI DC, selected brush reference, and outstanding paint-clip frames.

Default `WM_PAINT` uses BeginPaint/EndPaint rather than merely validating.
Default background erasing uses the registered class brush, and dialogs obtain
their brush through `WM_CTLCOLORDLG`. PE32 brush results are zero-extended from
32 bits, rather than treating signed LRESULT as a native-width handle.
`GetUpdateRect(TRUE)` performs requested erasing without validating the region;
an empty update query clears its output RECT.

Validation commands:
```sh
make -C arch/x86 CLANG=1 -j4
sh arch/x86/scripts/build-user32-paint-test.sh
```

The generated `user32_paint_pe32.exe` and `user32_paint_pe64.exe` each pass 85
checks unchanged on Windows and OsitoK. Coverage includes callback ordering,
class and dialog brushes, fErase, empty updates, FillRect/BitBlt clipping,
application-clip intersection, nested painting, callback reinvalidation, and
destruction during erasing. The `.autoload` fixture runs the PE32 executable
from `probes/`; the PE64 executable runs with
`winexec "probes/user32_paint_pe64.exe"`.

Final QEMU artifacts are under `/root/osito-user32-paint-20260904-r6/` (8 GiB,
four CPUs, KVM, separate snapshot disk). The booted kernel matches the build's
SHA-256: `7fb6bfe86cc38448c475b64f59fde3f5d72bb0c6d35759357f683735d8637ef5`.
Both GDI paint probes, GDI DIB 32/32, window-model 95/95, input 156/156, dialog
10/10, DirectDraw ABI, DirectSound, and DOS API with 15/15 host-memory checks
pass. The PE32 paint probe also passes again after DOS API. No unexpected CPU
fault appears in this run. The fixture still lacks the optional `diag/`
directory, producing the documented parent-missing boot-log errors.

The update region still uses a bounding rectangle, not a complex region.
Common-DC caching, child/occlusion clipping, caret handling, DrawText, and
default control painting remain incomplete. These checks do not prove complete
process-exit reclamation or concurrent GDI lifetime correctness. UT99's setup
wizard was not visually validated on this final kernel, so this change does
not establish that its controls or text render correctly.

## 4.13 GDI text output and USER32 text layout (2026-09-04)

`TextOut` and `ExtTextOut` now share ANSI/Unicode rendering, explicit character
advances, ETO_PDY, per-call clipping, opaque rectangle fills, alignment, and
TA_UPDATECP state. `MoveToEx`, `GetCurrentPositionEx`, and the text alignment
and color getters expose the DC state used by those operations. Viewport,
selected-region, and paint clipping remain in effect. Text measurement rejects
invalid arguments and reports an empty string as a zero-sized extent.

`DrawTextA/W` and `DrawTextExA/W` use a shared formatter, replacing the no-op
stubs and adding the missing DrawTextW export. It handles measured lines,
word wrapping, tabs, alignment, CALCRECT, clipping, mnemonic prefixes, Ex
margins/length reporting, and end/path/word ellipsis. MODIFYSTRING edits source
spans so untouched line endings and mnemonic markers are preserved. Coordinate
arithmetic uses checked wide intermediates, and temporary allocations are
released on failure as well as success.

Validation commands:
```sh
make -C arch/x86 CLANG=1 -j4
sh arch/x86/scripts/build-gdi-text-test.sh
bash arch/x86/scripts/test-user32-text-layout.sh
CC=clang bash arch/x86/scripts/test-user32-text-layout.sh
```

The generated `gdi_text_pe32.exe` and `gdi_text_pe64.exe` each pass 77 checks
unchanged on native Windows and OsitoK. Raster comparisons use TextOut as the
reference on the same platform; they do not assert identical fonts across OSes.
Coverage includes explicit advances, clipping, opaque backgrounds, current
position, layout geometry, ellipsis, source preservation, and long text runs.
The host harness builds the production formatter with mocked GDI and runs
20,000 cases under ASan/UBSan with both GCC and Clang, including allocation
failure injection. Its allocation and bounds checks are not a rendering oracle.

Final QEMU artifacts are under `/root/osito-gdi-text-20260904-r3/` (8 GiB,
four CPUs, KVM, disposable snapshot disk). The booted kernel matches the build's
SHA-256: `a20eb4c7dc76f2e584678795d18ac593350bb05a5a0d2596a47f616655e84c57`.
Both text probes, USER32 paint 85/85 on PE32/PE64, both GDI paint probes, GDI
DIB 32/32, GDI region 40/40, window-model 95/95, input 156/156, dialog 10/10,
DirectDraw ABI, DirectSound, and DOS API with 15/15 host-memory checks pass.
The PE32 text probe passes again after DOS API. No unexpected CPU fault appears
in this run. The fixture still lacks the optional `diag/` directory.

The UT99 integration run at `/root/osito-ut99-text-20260904-r1/` uses the same
kernel. Its screenshot after pointer movement shows the wizard's light
background, but child controls remain black without their labels. Default
control painting and behavior still need implementation; these text contracts
alone do not establish a usable UT99 wizard.

Font rendering and metrics still use the existing fixed 8x16 bitmap font.
Selected-font realization, Unicode shaping, general mapping modes, and full
flag-combination coverage are not completed here. The tests explicitly release
their objects and do not prove process-exit reclamation or concurrent GDI
lifetime correctness. This change does not modify DOS or audio implementations.

## 4.14 Native BUTTON/STATIC controls and window text (2026-09-04)

Native BUTTON and STATIC classes now have per-window state, painting,
WM_SETFONT/GETFONT, dialog-code classification, parent control-color callbacks,
and owner drawing with the PE32/PE64 DRAWITEMSTRUCT layout. Button behavior
includes push/check/radio/three-state controls, BM_* state messages, automatic
radio groups and tab stops, mouse capture/cancellation, and space-key clicks.
STATIC supports text, frames, rectangles, and caller-owned bitmap/icon handles.
Control state survives reentrant destruction until the active callback returns;
borrowed fonts and images are not deleted with their controls.

Window captions use dynamic UTF-16 storage rather than the compositor's bounded
title buffer. Set/GetWindowText and WM_SETTEXT/GETTEXT/GETTEXTLENGTH use the
window procedure, with ANSI/Unicode conversion for SendMessageA/W and a
per-thread encoding context retained through queued synchronous delivery.
GetWindowTextA and IsWindowUnicode are exported. Basic ANSI subclass forwarding
is covered without application names or fixed binary addresses.

Temporary GDI clips also work with memory DCs, so BUTTON WM_PRINTCLIENT paints
and invokes owner-draw callbacks on bitmaps. Nested clips intersect the existing
paint bounds instead of widening a partial BeginPaint update. DC destruction
releases outstanding clip frames as well as the selected brush reference.

Validation commands:
```sh
make -C arch/x86 CLANG=1 -j4
sh arch/x86/scripts/build-user32-controls-test.sh
```

The generated `user32_controls_pe32.exe` and `user32_controls_pe64.exe` each
pass 64 checks unchanged on native Windows and OsitoK. Coverage includes long
captions, A/W conversion and subclassing, cross-thread Unicode text, button
state/notifications/capture, bitmap pixels/ownership/geometry, owner-draw ABI,
partial-update and memory-DC clipping, and destruction inside draw/color
callbacks. The `.autoload` fixture starts the PE32 probe from `probes/`.

Final QEMU artifacts are under
`/root/osito-controls-commit-20260904-S6rozQ/` (8 GiB, four CPUs, KVM, separate
snapshot disk). The booted kernel matches the build's SHA-256:
`0128764abfb08d009413dbb9d358e855d1df96b0e2f61f4da2a669264ae59fb8`.
Both control probes, GDI text 77/77 and USER32 paint 85/85 on both PE ABIs,
both GDI paint probes, GDI DIB 32/32, GDI region 40/40, window-model 95/95,
input 156/156, dialog 10/10, DirectDraw ABI, DirectSound, and DOS API with
15/15 host-memory checks pass. The PE32 control probe passes again after DOS.
No unexpected CPU fault appears. The fixture lacks the optional `diag/`
directory, producing the previously documented boot-log parent-missing errors.

The UT99 run at `/root/osito-ut99-controls-commit-20260904-HcaWzi/` uses the
same kernel and a disposable snapshot of the UT99 fixture. `wizard.png` shows
the banner bitmap, instructions, radio labels, and Next/Cancel buttons. The
device LISTBOX remains black, so this is not yet a usable renderer-selection
wizard or a validated game launch. No CPU fault was recorded in this run.

LISTBOX and broader dialog keyboard navigation remain incomplete. Fonts still
use the fixed bitmap realization. Full A/W CREATESTRUCT delivery, previous-
procedure encoding thunks, posted messages, SendMessageTimeout text conversion,
and multibyte/surrogate boundary handling are not established by these probes.
Some legacy caption consumers still use the bounded display title. Exact
external-DC state parity after WM_PRINTCLIENT is not established: native
Windows can retain a clip on the supplied DC, while this implementation pops
its internal temporary clip. Tests explicitly release their objects; they do
not prove full process-exit reclamation or concurrent GDI lifetime correctness.
This change does not modify the DOS or audio implementation.

## 4.15 Native LISTBOX control contracts (2026-09-05)

Commit `17cb1705` adds per-window LISTBOX state with dynamically grown UTF-16
item storage, ANSI/Unicode message conversion, item data, sorting/search,
single/multiple selection, caret/anchor/top-index geometry, mouse capture,
keyboard selection, and parent notifications. Fixed/variable owner drawing
uses the PE32/PE64 MEASUREITEM, COMPAREITEM, DRAWITEM and DELETEITEM layouts.
Control state stays alive until reentrant callbacks return; destroying a list
does not release its borrowed font.

Build the probes with `sh arch/x86/scripts/build-user32-listbox-test.sh`.
`user32_listbox_pe32.exe` and `user32_listbox_pe64.exe` each pass 65 checks on
native Windows and 86 checks in OsitoK. The counts differ because OsitoK paints
more often and the probe checks each owner-draw callback. The optional
`--reentrant` mode passes 91 checks per ABI in OsitoK; destruction from inside
owner drawing is a robustness test, not established native Windows parity.
Artifacts for that initial QEMU run are in
`/root/osito-listbox-20260904-5z7Idk/`.

The UT99 wizard now paints its renderer list. The initial integration run
`/root/osito-ut99-listbox-20260904-NCGf1d/` nevertheless crashed the compositor
on a physical click, unlike the message-driven probes. This is addressed and
independently reproduced in section 4.16.

Full locale collation, native nonclient scrollbars, integral-height resizing,
and directory population remain incomplete. LB_DIR/LB_ADDFILE report failure
rather than inventing entries. Concurrent mutation between cross-encoding
LB_GETTEXTLEN/LB_GETTEXT requests is not validated. Existing bitmap-font and
broader ANSI/Unicode thunk limitations from section 4.14 still apply.

## 4.16 Window callback scheduler ownership (2026-09-05)

The root PE runtime and native tasks without a Win32 context both expose the
fallback Win32 PID/TID `1:1`. Comparing only those IDs let compositor task 3
call a PE32 window procedure belonging to task 1, using the wrong address
space, TEB and callback mode. The saved fault was a compositor instruction
fetch at RIP zero while dispatching WM_KILLFOCUS from SetFocus.

WINDOW now records its creating scheduler task. Immediate window-message,
window-position, native drag, capture and destruction paths check that task
as well as the Win32 IDs. Queued synchronous requests also retain their target
task, preventing a sender with colliding fallback IDs from consuming its own
request. UpdateWindow uses the synchronous message path for owner-thread
painting. This follows the ownership model described in
[Creating Windows in Threads](https://learn.microsoft.com/en-us/windows/win32/procthread/creating-windows-in-threads);
it does not change application-visible process IDs or add game-specific rules.

The extended `win32-input-test` spawns a real native input producer. It checks
deferred focus delivery, owner-thread focus/mouse/key callbacks, synchronous
results, owner-thread repainting, and rejection of foreign capture/destruction.
Before the fix the first version failed 2 of 162 checks, specifically premature
callbacks and wrong-task execution, in
`/root/osito-input-owner-before-20260905-bb156d/`. After the fix those 162 pass;
the expanded final version passes 166/166.

Final regression artifacts: `/root/osito-input-owner-final-20260905-b9HdRx/`.
The CLANG=1 build and booted kernel share SHA-256
`473a8bd0f296f0f749c8f689b0b6da9959dcf272ef09b5baa2f962558d773c1a`.
Window-model 95/95, dialog 10/10, GDI DIB 32/32, regions 40/40, DirectDraw,
DirectSound, and DOS API with 15/15 host-memory checks pass. After DOS, LISTBOX
86/86 and reentrant 91/91, BUTTON/STATIC 64/64, and paint 85/85 all pass on both
PE ABIs. No unexpected CPU fault appears in this regression run.

The same kernel in `/root/osito-ut99-input-owner-20260905-OrxMo4/` boots the
UT99 image with `-snapshot`, 8 GiB, four CPUs and KVM. QMP absolute-pointer
input selects the second list item (`selected.png`), and Down selects the
third (`keyboard.png`). Focus logs show `tasks=1:3`. Next advances through
the detail and ready pages (`next.png`, `ready.png`); Run loads SoftDrv,
configures DirectDraw to 640x480, and reaches the rendered game menu after
Escape (`escape.png`). No compositor fault recurs during this sequence.

This is not full UT99 validation. The detail page still has an unimplemented
EDIT control. Five renderer rows share the Software Rendering label even
though read-only extraction of D3DDrv.int, SoftDrv.int and OpenGlDrv.int shows
distinct ClassCaption values (the fixture cause is isolated in section 4.17).
A pointer-driven practice-menu attempt did not
open its dialog, so gameplay, in-game pointer behavior and audible output are
not established. General native/Win32 ID mapping, per-thread input attachment,
queue teardown races, and complete process-exit reclamation remain separate
work. The DOS and audio implementations are unchanged by this patch.

## 4.17 UT99 fixture paths and renderer enumeration (2026-09-05)

The repeated renderer labels were not a LISTBOX string-storage or private-
profile bug. With kernel SHA-256
`473a8bd0f296f0f749c8f689b0b6da9959dcf272ef09b5baa2f962558d773c1a`,
GDB observations in `/root/osito-ut99-labels-20260905-kPMJfu/` showed:

- `labels-gdb.log`: LB_ADDSTRING already receives repeated UTF-16 Software
  Rendering strings; GetPrivateProfileStringA/W are not called on that path.
- `localize-gdb.log`: Show all devices obtains the distinct ClassCaption
  values for D3DDrv, GlideDrv, MeTaLDrv, OpenGLDrv and SoftDrv.
- `iterator-gdb.log`: the application's registry-object array contains 25
  entries, five repetitions of those five classes. Its compatibility filter
  selects the software entry from each repetition.

Both INIs in the flat image `/root/osito-dialog-ut99/nvme_ut99.img` contain
`Paths=*.u`, `*.unr`, `*.utx`, `*.uax` and `*.umx`. The engine searches
each corresponding directory for localization metadata; all five resolve to
`C:\\*.int`. Serial logs confirm five identical directory searches.
Deduplicating strings in USER32 would hide an image-layout error and violate
normal list insertion behavior.

A separate OSFS2 copy, `/root/osito-ut99-layout-20260905-f3O3tU/disk.img`,
keeps executables, DLLs, .int and .u files at the root, and relocates 265
resource files with the existing journaled ositofs-rename tool: 96 maps,
110 texture packages, 29 sound packages and 30 music packages. Both INIs use:

```ini
Paths=*.u
Paths=Maps/*.unr
Paths=Textures/*.utx
Paths=Sounds/*.uax
Paths=Music/*.umx
```

Preflight also found an existing overlap in the source image: the empty
UnrealTournament.log entry and Entry.unr both claimed block 857. The map's
bytes were still intact. Only the copy's empty log was rewritten with zero
allocated blocks; this was not a blanket fsck repair. The map retains SHA-256
`4bd31b4af4195d257d4b100026c12c7ad511b000f3bc8653e4a34a30fbbf689c`
after relocation. ositofs-fsck reports a clean journal, matching superblocks,
731 files, correct block accounting and no overlaps. The original image's
SHA-256 remains unchanged; it still contains the preexisting overlap.

With the same kernel, KVM, 8 GiB, four CPUs and -snapshot, the corrected copy
shows one compatible Software entry (`certified.png`) and five distinct
entries under Show all devices (`all-devices.png`). `registry-gdb.log`
independently confirms the application array has five entries, not 25.
Next/Run reaches the rendered menu. Escape, Down, Down, Enter opens Start
Practice Session with the DM-Agony preview (`practice.png`), exercising the
relocated map and texture paths.

This does not establish full gameplay or audible output. Native EDIT is still
unimplemented, and an absolute pointer move to guest-screen (939,600) places
the visible in-game pointer near (294,140), not the Start button
(`practice-pointer.png`). The exclusive-mode pointer coordinate path needs
separate investigation. No Win32/DOS runtime code or application binary was
changed for this fixture correction.

## 4.18 Thread-owned CRT floating-point control (2026-09-05)

`_controlfp` previously updated a global shadow without changing hardware;
`_controlfp_s` always returned success and a zero control value. The early
PE32/PE64 probes failed 11/21 and 7/16 checks with the old kernel in
`/root/osito-crt-fp-baseline-20260905-0jOWbz/`.

The shared CRT shim now reads and updates the current thread's x87 control
word and MXCSR. `_control87` maps CRT exception masks, rounding, precision,
and denormal modes to hardware; `_controlfp` preserves the denormal exception
mask. PE32 reports ambiguous x87/SSE state, while PE64 controls SSE without
changing x87. DAZ changes honor the processor's MXCSR mask. Queries preserve
sticky SSE status; an effective control change clears it, as observed on
native Windows. The existing scheduler FPU context provides thread isolation.

`_controlfp_s` reports the actual resulting control and rejects selected
unknown bits through the invalid-parameter handler and EINVAL. Process-level
handler storage uses the existing process-owned CRT state; a thread-local
handler takes precedence. Callbacks run outside the state lock through the
appropriate PE ABI. This follows the
[CRT control API](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/control87-controlfp-control87-2?view=msvc-170).
The installed release UCRT treats PE64 x87-only precision requests as a
successful no-op, which the probe records explicitly rather than assuming
the invalid-parameter behavior described for that case in the documentation.

Build with `sh arch/x86/scripts/build-crt-fp-test.sh`. Copy the two executables
from `arch/x86/build/test-crt-fp/pe{32,64}/` to an image's `probes/` directory.
Run `winexec probes/crt_fp_pe32.exe` and the PE64 equivalent, both with and
without a trailing `ucrt` argument. `crt_fp.autoload` starts the PE32 MSVCRT
case. The same binaries run directly on Windows. Coverage includes actual
rounding and precision arithmetic, all exception masks, denormal modes,
hardware readback, thread isolation, and invalid-parameter handler precedence.
Mask tests clear pending x87 flags left by earlier arithmetic before
unmasking exceptions, keeping the cases independent.

| Runtime | PE32 MSVCRT | PE32 UCRT | PE64 MSVCRT | PE64 UCRT |
| --- | --- | --- | --- | --- |
| Native Windows | 44/44 | 50/50 | 31/31 | 38/38 |
| OsitoK | 48/48 | 50/50 | 36/36 | 38/38 |

Counts differ because the host MSVCRT lacks the process invalid-parameter
handler export; those conditional checks run in OsitoK and UCRT. Final QEMU
artifacts are in `/root/osito-crt-fp-commit-20260905-MjpSGF/`, using an isolated
OSFS3 snapshot, 8 GiB, four CPUs, KVM, VNC :12 and UDP 7790. The CLANG=1 build
and booted kernel share SHA-256
`2c93557771ab849704a472fcdfdf7870251d633e5efcd4cb4552897c2bae485f`.
Input 166/166, window-model 95/95, dialog 10/10, GDI DIB 32/32, regions 40/40,
DirectDraw ABI, DirectSound, and DOS API with 15/15 host-memory checks pass.
Both UCRT probes pass again after DOS. No unexpected CPU fault appears in
this regression run.

This does not establish full CRT floating-point coverage: startup FPU defaults,
`__control87_2`, `_statusfp`, `_clearfp`, `_fpreset`, and the no-handler fatal
path remain outside these probes. No DOS or audio implementation changed.
The UT99 render defect still needs an integration rerun: with the old kernel,
`/root/osito-ut99-relative-20260905-dDAv7S/` reached DM-Agony using a relative
USB mouse, but a raw submitted RGB565 buffer (`frame8.png`) already contained
the stale scene and accumulated HUD. That isolates the observed corruption
upstream of the compositor; it does not prove this CRT fix resolves it.

## 4.19 PE32 x87 acos and remainder contracts (2026-09-05)

The native PE32 `_CIacos` sequence encoded `x*x-1` where its square root
requires `1-x*x`. `_CIfmod` executed FPREM only once, returning an intermediate
remainder when the operands' exponents were far apart. The emitted sequences
now use the correct reverse subtraction and repeat reduction while C2 is set,
preserving the scratch EAX register. The partial-reduction rule is defined in
the [Intel instruction reference](https://cdrdv2-public.intel.com/825760/325383-sdm-vol-2abcd.pdf).
The existing provider registrations and x87 argument/result ABI are unchanged;
these fixes apply to the shared PE32 CRT aliases, not the PE64 fallbacks or DOS.

Build `crt_x87_pe32.exe` with `sh arch/x86/scripts/build-crt-x87-test.sh` and
run `winexec probes/crt_x87_pe32.exe`, optionally followed by `ucrt`.
`crt_x87.autoload` starts the MSVCRT case. The probe calls the real intrinsics
with live values below their x87 operands and checks stack depth, preserved
values/control, acos endpoints/interior values, large exponent differences,
remainder signs, subnormals, signed zero, infinities and NaNs. A final case
sets C2 through a partial reduction before requesting an invalid remainder.

The initial 43-check version passed on native Windows MSVCRT and UCRT but
failed seven checks in OsitoK, through both aliases, in
`/root/osito-crt-x87-before-20260905-jrqh3M/`. The final 68-check version passes
on native UCRT and on both OsitoK aliases. Native MSVCRT differs on two edge
values: remainder with an infinite divisor and negative-zero preservation.
Those checks deliberately remain visible failures for that legacy reference;
the shared shim follows the UCRT values instead of introducing app-specific
or host-detection exceptions into the probe.

Regression artifacts: `/root/osito-crt-x87-edge-20260905-WA3hmj/`, with
8 GiB, four CPUs, KVM and an isolated OSFS3 snapshot. The built and booted
kernel share SHA-256
`a00b3766affd23956e11543b7bb138478e25dd09c0c8910d36a3c8f3cebab7d6`.
All four CRT-FP variants pass (48/50 PE32, 36/38 PE64 for MSVCRT/UCRT).
Input 166/166, DirectDraw ABI, DirectSound, and DOS API with 15/15 host-memory
checks pass. The UCRT x87 probe passes 68/68 again after DOS. No unexpected
CPU fault appears. errno/_matherr compatibility, unmasked exceptions, all
precision/rounding combinations and the other `_CI` intrinsics are not
established by this change.

The final probe explicitly declares volatile SSE registers at its inline
assembly calls. `/root/osito-crt-x87-final-20260905-Eh6DE5/` repeats 68/68 on
both aliases before and after DOS with the same kernel. Its executable has
SHA-256 `a768cde7d476f2b0c7434fcd7c2b2c338259d87cf7217cc1dd8a3dbb58c8a5c2`.

Two UT99 integration runs retain the corrected fixture from section 4.17.
`/root/osito-ut99-fp-20260905-HhrROT/` uses the control-word fix alone;
`fp-frame-v2-gdb.log` observes the application's precision requests changing
the hardware x87 word, but the raw submitted `frame8.png` is already stale.
`/root/osito-ut99-x87-20260905-Dp3xiN/` adds these intrinsic corrections.
Both reach DM-Agony and start the match through actual relative mouse and
keyboard input. `playing.png`, `moving.png` and `settled.png` in the latter
still show a static scene, missing first-person weapon and accumulating HUD.
The startup sample observes `_CIfmod` calls but no `_CIacos` calls; it does
not establish coverage throughout gameplay. The render problem therefore
remains open, rather than being attributed to either fixed math defect.

Both game runs exit through their console with code zero, and the layer
reports releasing 24 root-process modules. No unexpected CPU fault is logged.
The QEMU and GDB sessions were stopped. The base fixture remains unchanged at
SHA-256 `cdec52eccec06dac6a24e83752f23fcbf59fba4eebd014f8f8fc79d2b307d350`;
no application binary or persistent game configuration was patched.

## 4.20 CRT classification and rounding ABIs (2026-09-05)

The UT99 boundary trace exposed two additional CRT contract gaps. PE32
`_isnan` supplied its double as two stack DWORDs, but the native shim read
XMM0, which was unrelated caller state. `ceil` and `floor` had neither a
double-argument adapter nor the required PE32 ST(0) result. Their native
implementations also converted through `long`, losing signed zero and
producing incorrect results outside the integer range, including infinities
and NaNs. These arithmetic defects affected PE64 as well.

`_isnan` now uses the existing integer-bits bridge pattern from `_finite`.
The shared rounding implementation uses FRNDINT with directed rounding and
restores the caller's x87 control word. PE32 `ceil` and `floor` reuse the
existing `strtod` result-wrapper emitter: an integer-bits helper returns
EDX:EAX, then the wrapper loads ST(0). A bounded registration table retains
the original `strtod` offset and statically checks that all wrappers fit the
runtime math page. Provider aliases share these contracts; there is no
game-specific dispatch, binary patch, or change to the INT2E dispatcher.

Build the independent probes with:

```sh
sh arch/x86/scripts/build-crt-classify-test.sh
sh arch/x86/scripts/build-crt-round-test.sh
```

Copy the PE32/PE64 executables from `arch/x86/build/test-crt-classify/` and
`arch/x86/build/test-crt-round/` into an image's `probes/` directory. Run
`winexec probes/crt_classify_pe32.exe` and `winexec probes/crt_round_pe32.exe`,
then the PE64 equivalents, with and without a trailing `ucrt` argument.
`crt_classify.autoload` starts the PE32 MSVCRT classifier case. The same
binaries run directly on Windows.

The classifier covers 22 IEEE double patterns, including NaNs with payloads
in either DWORD, unrelated zero/NaN XMM0 state, and a live x87 value below
the call. Rounding covers signed zero, subnormals, fractions, large finite
values, infinities and NaNs across all four x87 rounding modes and three
precision settings. PE32 also checks a live underlying x87 value. Four
`strtod` cases verify the reused return path and end-pointer behavior.

| Probe | Native Windows PE32 | Native Windows PE64 | OsitoK PE32 | OsitoK PE64 |
| --- | --- | --- | --- | --- |
| Classify, each CRT alias | 133/133 | 67/67 | 133/133 | 67/67 |
| Round/strtod, each CRT alias | 1305/1305 | 873/873 | 1305/1305 | 873/873 |

The old-kernel classifier run in
`/root/osito-crt-classify-before-20260905-tSLMqE/` failed 16 of 133 PE32
checks through each alias; PE64 already passed. Before adding the eight
`strtod` checks, the old-kernel rounding probe in
`/root/osito-crt-round-before-20260905-oHShZQ/` failed 864 of 1297 PE32 and
240 of 865 PE64 checks through each alias. The final extended probe was not
rerun against that old kernel.

Fixed-kernel artifacts are in
`/root/osito-crt-math-fixed-20260905-EsWRHY/` and
`/root/osito-crt-math-final-20260905-fFbYsM/`. Broader regressions in
`/root/osito-crt-math-regress-20260905-7QejHS/` pass all four CRT-FP variants
(48/50 PE32, 36/38 PE64 for MSVCRT/UCRT), DOS API with 15/15 host-memory
checks, the UCRT x87 probe with 68/68 after DOS, and the module-image/ABI
test with 45/45. The final PE32 rounding and classifier probes also pass
again after DOS. No unexpected CPU fault is logged in these runs.

The general PE32 console probe is not green: it exits with code 134 at
`_fail_crt_fstat_time`, before its `strtod` assertions. That failure was not
compared against the old kernel and remains a separate investigation; the
direct `strtod` cases above do not replace the failing filesystem check.
The probes do not establish errno/_matherr callbacks, unmasked-exception
continuation, complete status-flag parity, or every MXCSR configuration.

`make -C arch/x86 CLANG=1 -j4` and `git diff --check` pass. The built and
booted kernel share SHA-256
`8620de1dd48475e4b91b45bfa5a8802516fd648a02471671a7de755d844e97cd`.
UT99 integration artifacts are in
`/root/osito-ut99-math-fixed-20260905-3ArKTu/`, using the corrected fixture,
8 GiB, four CPUs, KVM, a relative USB mouse and an isolated snapshot with no
GDB attached. DM-Agony loads and the match advances, but `turning.png` still
shows accumulated HUD text and incorrect rendering. These CRT fixes do not
resolve the game's rendering defect. Console `quit` exits with code zero
and releases 24 root-process modules; no unexpected CPU fault is logged.
The QEMU session was stopped and the fixture retains SHA-256
`cdec52eccec06dac6a24e83752f23fcbf59fba4eebd014f8f8fc79d2b307d350`.
No DOS, audio, application binary, or persistent game configuration changed.

## 4.21 Legacy wide formatting and UT99 FOV reload (2026-09-05)

The stale UT99 world and accumulating HUD have a configuration cause. In
`/root/osito-ut99-render-trace-20260905-j59T0S/`, `DrawWorld` was reached but
SoftDrv received no world surfaces. The actor's FOV was zero; the original
Core.dll `appTan` correctly returned zero, after which Engine.dll divided
the viewport half-width by that value. The projection became infinite and
the clipping planes contained NaNs. A diagnostic `fov 90` command restored
world rendering and the weapon, but was not used as a persistent solution.

A clean snapshot in `/root/osito-ut99-config-trace-20260905-E1oCnb/` locates
the loss of the default. `config-gdb.log` shows PlayerPawn and derived class
defaults entering `UObject::LoadConfig` with FOV 90. Missing DesiredFOV and
DefaultFOV INI keys are skipped, and ordinary `1.0`/`0.8` imports work.
Starting the practice match subsequently reloads the literal string `%f`
into both FOV properties. A data watchpoint catches DefaultFOV changing
from 90 to zero in `UFloatProperty::ImportText`; a later live call confirms
that the native `crt_vsnwprintf` returns the literal `%f` for a double.
The original binary and packaged class defaults were not patched.

`_vsnwprintf` had its own incomplete parser, with no floating conversion
and unconditional PE32 argument slots even for PE64 callers. It now uses
the existing wide PE32/PE64 parsers, selecting the caller ABI as the other
explicit-va_list entry point does. A context-local legacy count policy lets
this API fill all `count` WCHARs and omit NUL when no room remains. Exact
fit returns the character count; truncation returns -1. Existing terminated
wide formatter consumers retain their previous policy. This matches the
[documented legacy count contract](https://learn.microsoft.com/en-us/previous-versions/1kt27hek%28v%3Dvs.140%29)
and is not an application-specific fallback.

Build the standalone probe with
`sh arch/x86/scripts/build-crt-wformat-test.sh`. Copy its PE32/PE64 outputs
from `arch/x86/build/test-crt-wformat/pe32/` and `pe64/` into `probes/`, then
run `winexec probes/crt_wformat_pe32.exe` and the PE64 equivalent. A trailing
`ucrt` tests OsitoK's provider alias. Native Windows UCRT does not export
these legacy entry points directly; the native reference here is MSVCRT,
not a claimed UCRT pass. `crt_wformat.autoload` starts the PE32 case.

The probe covers mixed doubles/integers/pointers, 64-bit integers, width and
precision arguments, wide/narrow strings, exact fit, zero count, truncation,
measurement without a destination, storage canaries, and the existing
terminated/direct-varargs paths. Both native Windows builds pass 60/60.
In `/root/osito-crt-wformat-before-20260905-VDrHpx/`, the old kernel fails
46/60 PE32 checks and 42/54 PE64 checks through each alias. The PE64 `basic`
baseline excludes the two pointer-string cases because the old parser
truncates 64-bit pointers; the fixed kernel runs the full probe.

`/root/osito-crt-wformat-fixed-20260905-q5l2KO/` passes 60/60 for both
architectures and both OsitoK aliases. Additional regressions pass:
rounding/strtod 1305/1305 PE32 MSVCRT and 873/873 PE64 UCRT; CRT-FP 48/48
PE32 MSVCRT and 38/38 PE64 UCRT; DOS API with 15/15 host-memory checks;
module-image/ABI 45/45; x87 68/68 after DOS. The PE32 MSVCRT and PE64 UCRT
wide probes pass again after DOS. No unexpected CPU fault is logged.

The fixed integration run is
`/root/osito-ut99-wformat-fixed-20260905-6PdkE4/`, with 8 GiB, four CPUs,
KVM, a relative USB mouse and an isolated snapshot of the same fixture.
DM-Agony starts through the practice-session UI without a `fov` command or
configuration override. A bounded, read-only hardware-breakpoint trace in
`projection-gdb.log` observes actor and class-default FOV values of 90,
finite projection `(-319.5, -239.5, 320)`, and 18 SoftDrv world-surface calls
before the third projection sample. The debugger then detaches.

`playing.png`, `moving.png` and `respawn.png` show the world, weapon, match
progression and respawn, without the old accumulating HUD. This establishes
the FOV fix, not complete rendering correctness: the scene remains very
dark and some surfaces appear black, so lighting/texture correctness still
needs investigation. Console `quit` exits with code zero and releases 24
root-process modules. No unexpected CPU fault is logged. QEMU was stopped
and reaped, and the base fixture retains SHA-256
`cdec52eccec06dac6a24e83752f23fcbf59fba4eebd014f8f8fc79d2b307d350`.
No persistent game settings or binaries were modified.

`make -C arch/x86 CLANG=1 -j4` and `git diff --check` pass. The fixed kernel
has SHA-256 `70492ef780b075b6999bc45c98b8894df6c3ab33b2812bd76be91b3ea9a4bbb7`.
The shared formatter still needs complete scientific/general formatting,
large and non-finite values, signed-zero/rounding parity, locale conversion,
and invalid-parameter callback coverage. This formatter change does not
establish those contracts. The separate console-probe filesystem failure
from section 4.20 is addressed below. No DOS, audio, or application code
is changed.

## 4.22 CRT path-stat timestamp parity (2026-09-05)

The console PE32 probe's exit code 134 (`_fail_crt_fstat_time`) was a real
path-versus-descriptor metadata mismatch. `crt_stat_query` used the legacy
32-bit `osfs2_file_ctime`/`osfs2_file_mtime` accessors and synthesized access
time from modification time. On OSFS3, the former exposes inode metadata
change time, not file creation; the latter truncates timestamps beyond
2106. `_fstat64` already obtained distinct, 64-bit creation/access/write
times through `NtQueryInformationFile(FileBasicInformation)`.

Path queries now use the existing `osfs2_file_get_times` dispatcher, as
the NT descriptor path does. CRT `st_ctime` receives creation time, not
POSIX inode change time; `st_atime` and `st_mtime` remain independent.
Values outside signed 64-bit storage return `EOVERFLOW`. This affects the
ten `_stat*`/`_wstat*` export entries through their four shared layouts,
without changing their ABI structures or either filesystem implementation.

Build `sh arch/x86/scripts/build-crt-stat-test.sh` and copy the PE32/PE64
executables from `arch/x86/build/test-crt-stat/pe32/` and `pe64/` into
`probes/`. Run `winexec probes/crt_stat_pe32.exe ucrt far` and the PE64
equivalent. Omit `ucrt` for OsitoK's MSVCRT provider alias; omit `far` for
the two ordinary-date phases. `crt_stat.autoload` starts PE32 UCRT without
the optional far-date phase.

The probe compares all four narrow and wide path layouts with `_fstat64`,
checks size and output canaries, assigns distinct creation/access/write
times twice, and changes file mode without losing creation time. The
optional third phase exercises time64 layouts after unsigned 32-bit
seconds overflow. It uses `CREATE_NEW` and deletes only its owned file.
Native Windows UCRT passes 104/104 for both architectures. This reference
does not establish legacy Windows MSVCRT timezone-conversion parity.

The baseline `/root/osito-crt-stat-before-20260905-o2LZHo/` records 16/82
failures for PE32 UCRT and 20/104 for PE64 UCRT with `far`, followed by
console exit 134. In `/root/osito-crt-stat-commit-20260905-0b5azQ/`, an
isolated OSFS3 snapshot with KVM, 8 GiB and four CPUs passes 104/104 for
both architectures through both provider aliases. The same console probe
now exits with code zero.

DOS API passes, including 15/15 host-memory checks; module-image/ABI passes
45/45. After DOS, wide formatting passes 60/60 for all four architecture/
provider combinations, x87 passes 68/68, and rounding passes 1305/1305
PE32 MSVCRT and 873/873 PE64 UCRT. No CPU fault is logged. QEMU was stopped
and reaped. The kernel SHA-256 is
`65bc121573ce8043671820bbf528d13a490980ffde476b3dcdc82682f49ddc2b`;
`make -C arch/x86 CLANG=1 -j4`, both new probe builders, and
`git diff --check` pass.

This validation covers OSFS3 regular files. Directory timestamps, time32
overflow behavior, and OSFS2's reduced access-time granularity remain
outside this probe's coverage. No application-specific timestamp or
timezone correction is introduced.

## 4.23 Printf destinations, argument widths, and count policies (2026-09-05)

The narrow formatter treated a null buffer as console output. Consequently,
measurement calls printed their text, `fprintf`/`vfprintf` ignored the supplied
FILE, and `printf` bypassed CRT descriptor redirection. Explicit `va_list`
exports always used packed PE32 arguments, including for PE64 callers. The
UCRT common narrow/wide entry points had the opposite problem: native-only
argument parsing and six DWORDs of thunk metadata for a seven-DWORD PE32
signature whose first parameter is a 64-bit options field.

The formatter context now distinguishes bounded storage, measurement, and
FILE output. Stream output uses a 256-byte staging buffer and the existing
CRT file resolver/write path, including PE32 FILE proxies. It propagates
write errors and preserves embedded NULs; stdout comes from the CRT's current
descriptor, so `_dup2` redirection applies. `crt_fwrite` now sets errno and
the stream error flag for invalid, failed, and short writes. Its existing
large-transfer, text-mode, and synchronization policies are not replaced.

Legacy `_snprintf` and `_vsnprintf` can fill all `count` bytes without NUL;
exact fit returns the length and truncation returns -1. A nonnull buffer with
zero count fails without writing; null/zero measurement is silent. Contexts
that require terminated output keep that policy. The common UCRT APIs select
legacy or standard count behavior from options, use the caller's argument
layout, and register fixed-signature PE32 bridges that reassemble the options
DWORDs. The native and PE32 narrow integer parsers also distinguish Windows
32-bit `long`, 64-bit integers, pointer-sized integers, and short/char
narrowing. Alternate radix output, negative dynamic width, and default `%p`
formatting now have native-reference cases rather than host-ABI assumptions.

Build `sh arch/x86/scripts/build-crt-format-test.sh`, copy its executables
from `arch/x86/build/test-crt-format/pe32/` and `pe64/` to `probes/`, then run
`winexec probes/crt_format_pe32.exe` and the PE64 equivalent. A trailing
`ucrt` selects that provider; `crt_format.autoload` starts PE32 MSVCRT.
The probe covers mixed argument widths, pointer strings, storage canaries,
count 0 through 11, exact fit, truncation, measurement, common narrow/wide
options, FILE routing, stdout redirection, chunk boundaries, embedded NUL,
and read-only stream errors. It creates its fixture with `CREATE_NEW` and
deletes only that owned file; a duplicated Win32 handle preserves diagnostics
while stdout is redirected.

| Reference/provider | PE32 | PE64 |
| --- | --- | --- |
| Native Windows MSVCRT | 94/94 | 94/94 |
| Native Windows UCRT | 133/133 | 133/133 |
| OsitoK MSVCRT | 194/194 | 194/194 |
| OsitoK UCRT | 194/194 | 194/194 |

The native DLLs expose different export sets, so the native rows are not
identical coverage: common UCRT entry points are absent from native MSVCRT,
and legacy formatting exports are absent from native UCRT. Modern `hh`
integer cases use UCRT as their reference, not legacy Windows MSVCRT.
OsitoK exposes both groups through each alias and runs the combined probe.

The initial `basic` baseline in
`/root/osito-crt-format-before-20260905-lD1OLp/` fails 38 of 77 PE32 checks
and 62 of 175 PE64 checks. It excludes pointer cases and the broken PE32
common thunk; additional integer cases were added later, so these are not
194-check baselines. The final run in
`/root/osito-crt-format-final-20260905-GXf8O5/` passes all four combined
variants. Console PE32 exits with code zero; DOS API passes with 15/15
host-memory checks; module-image/ABI passes 45/45. After DOS, formatting
passes 194/194 for PE32 MSVCRT and PE64 UCRT, wide formatting 60/60 and
far-date stat 104/104 for those same variants, x87 68/68, and rounding
1305/1305 PE32 MSVCRT and 873/873 PE64 UCRT. No CPU fault is logged.

UT99 integration in `/root/osito-ut99-format-20260905-zBN1eJ/` uses KVM,
8 GiB, four CPUs, a relative USB mouse and an isolated snapshot. DM-Agony
starts from the practice-session UI with no FOV override. `loaded.png`,
`playing.png`, and `moving.png` show world geometry, textures, the weapon,
HUD, and movement; the match advances. Console `quit` exits with code zero
and releases 24 root-process modules. No CPU fault is logged, QEMU was
stopped and reaped, and the base image retains SHA-256
`cdec52eccec06dac6a24e83752f23fcbf59fba4eebd014f8f8fc79d2b307d350`.
This is an integration smoke test, not full renderer or audio certification.

`make -C arch/x86 CLANG=1 -j4`, the probe builder, and `git diff --check`
pass. The built kernel and both final booted copies share SHA-256
`0446ed0266541040eb91c987eaa2710d63252fa16c63c0273ecbf3fe6b559d65`.
No DOS runtime, audio driver, filesystem implementation, application binary,
or persistent game settings changed. Remaining CRT work includes complete
floating conversions and their scratch-storage bounds, large width/precision
parsing, locale/encoding behavior, `%n` opt-in policy, invalid-parameter
callbacks, more UCRT option combinations, and FILE lifetime/locking/text
translation. This probe validates binary streams, not those remaining
contracts.

## 4.24 Binary64 formatting and native CRT policies (2026-09-05)

Narrow PE32/PE64 and wide formatting now share `win32/crt_float.c` for
`f/F`, `e/E`, `g/G`, and `a/A`. The converter consumes IEEE binary64 bits
and uses integer decimal blocks rather than casting the integer part to
64 bits or building a precision-sized stack string. Decimal expansion and
emission are adapted from musl's MIT-licensed `vfprintf`; the source retains
its license and attribution. Callbacks write into the existing bounded,
measuring, or FILE contexts, including wide output and error propagation.
Padding for bounded and measuring contexts counts skipped output in bulk.

The legacy export profile and common UCRT options are separate. They cover
legacy non-finite spellings, two/three-digit decimal exponents, hexadecimal
precision and padding, and the standard-rounding option. Native MSVCRT's
17-significant-digit decimal seed and 512-digit precision limit are legacy
policies, not limits on UCRT output. Standard rounding observes the caller's
x87 or SSE rounding control without modifying it. Signed zero, subnormals,
largest finite values, infinities, and quiet/signaling NaNs have reference
cases. No application-name or game-specific formatting path is introduced.

Build with `sh arch/x86/scripts/build-crt-float-test.sh`. From an empty
reference working directory on Windows, run each PE32/PE64 executable with
`record`, then with `record ucrt`. Recording uses `CREATE_NEW`; it will not
replace an existing reference. Run without `record` to compare against
those files, adding `ucrt` for the common APIs. The reference DLL versions
are MSVCRT `7.0.26100.8875` and UCRT `10.0.26100.8875` for both architectures.

Copy the executables and their four `crt-float-*.ref` files into guest
`probes/`, since `winexec` sets the application's working directory there.
Run `winexec probes/crt_float_pe32.exe`, its PE64 equivalent, and both with
`ucrt`. `crt_float.autoload` starts the PE32 legacy variant. The optional
PE64 `local` mode compares the linked converter directly with Windows;
guest verification must use the normal comparison mode, not `local` or
`record`. `basic` selects a reduced reference set and is not the full suite.

| Reference comparison | PE32 | PE64 |
| --- | --- | --- |
| Native Windows MSVCRT | 9026/9026 | 9026/9026 |
| Native Windows UCRT | 72194/72194 | 72194/72194 |
| OsitoK MSVCRT | 9026/9026 | 9026/9026 |
| OsitoK UCRT | 72194/72194 | 72194/72194 |

Each legacy run has 1128 conversion cases; UCRT runs eight option profiles
for 9024 cases. Checks compare narrow/wide text and counts, storage guards,
terminators, rounding-control preservation, and exact reference consumption.
The cases include precision up to 1200 and deterministic binary64 samples.
Direct local converter comparisons additionally pass 9024 legacy and 72192
UCRT checks on native Windows.

The isolated KVM snapshot in
`/root/osito-crt-float-commit-20260905-h0QHCP/` uses 8 GiB and four CPUs.
Formatting passes 194/194 and wide formatting 60/60 through both provider
aliases on both architectures. DOS API passes, including 15/15 host-memory
checks; module-image/ABI passes 45/45; the console PE32 probe exits zero.
After DOS, x87 passes 68/68, rounding passes 1305/1305 PE32 MSVCRT and
873/873 PE64 UCRT, and the remaining format/wide alias variants also pass.
No CPU fault is logged. QEMU was stopped and reaped; the probe builder,
`make -C arch/x86 CLANG=1 -j4`, and `git diff --check` pass.
The booted kernel SHA-256 is
`3fcf4354d61aca642df501563b5945da4788233612804660044eda99c4b60f67`.

This change does not complete the CRT: large literal width/precision parsing,
locale/encoding contracts, `%n` policy, invalid-parameter callbacks, legacy
output-format configuration, and FILE lifetime/locking remain separate work.
Wide FILE formatting still measures and allocates its whole converted output.
UT99 gameplay and audio were not rerun for this formatter change.

## 4.25 DOS standard handles and legacy console routing (2026-09-05)

Commit `0a5efac7` added shared SFT IOCTL modes, CON line buffering, raw
handle I/O, and a common BIOS/DOS keyboard queue. The older `AH=01h..0Ch`
services still bypassed the JFT and accessed the framebuffer/keyboard
directly. AUX and PRN's legacy entry points were missing entirely.

These calls now resolve the current process's standard handles, including
duplicates and inherited file positions. File reads/writes share the same
helpers as `AH=3Fh/40h`; status probes restore the position after reading.
The character-driver path deliberately differs from handle-based cooked
I/O: embedded NUL and Ctrl-Z bytes are not string terminators for these
legacy calls. Cooked output expands tabs using a VM-local column, while
`AH=06h` and AUX/PRN output remain raw. These contracts were reviewed against
[MS-DOS CPMIO](https://github.com/microsoft/MS-DOS/blob/2d04cacc5322951f187bb17e017c12920ac8ebe2/v4.0/src/DOS/CPMIO.ASM)
and [CPMIO2](https://github.com/microsoft/MS-DOS/blob/2d04cacc5322951f187bb17e017c12920ac8ebe2/v4.0/src/DOS/CPMIO2.ASM).

`AH=0Ah` reads stdin and echoes to stdout. It handles a leading LF from a
previous redirected CR/LF line, keeps reading after an internal LF, and
preserves the capacity/terminating-CR contract. An explicit cooked CON
handle read uses that device for both input and echo, even with redirected
standard handles. `AH=0Ch` only flushes the keyboard when stdin names CON.
See the [line-input implementation](https://github.com/microsoft/MS-DOS/blob/2d04cacc5322951f187bb17e017c12920ac8ebe2/v4.0/src/DOS/STRIN.ASM)
and [CON handle swapping](https://github.com/microsoft/MS-DOS/blob/2d04cacc5322951f187bb17e017c12920ac8ebe2/v4.0/src/DOS/DISK.ASM).

Build with:

```sh
make -C arch/x86 CLANG=1 -j4 all \
    build/test/dstdio.com build/test/dscchild.com
```

Copy both COM files to the guest root and run
`dosrun dstdio.com`, or use `dos_stdio.autoload`. Feed `K`, `J`, `R`, and
`G` followed by CR after the KEYBOARD, FLUSH, ECHO, and CON READY markers,
respectively. This is a source-derived contract probe, not a differential
run against a native MS-DOS installation. It compares 36 captured bytes,
checks returns and positions, preserves BIOS lookahead across a file flush,
and restores handles before deleting only its CREATE_NEW-owned fixtures.
Cleanup failures make the test fail; capture differences print hexadecimal.

The initial probe fails with exit code 2 on the previous kernel in
`/root/osito-dos-stdio-before-20260905-UulfD5/`. The extended probe passes
twice in `/root/osito-dos-stdio-verified-20260905-oBmOVl/`, including after
DOS/Win32 transitions. NUL status/output, closed handles, read-only output,
and unavailable AUX/PRN are covered. The error cases check OsitoK's CF/AX
and extended-error reporting, not the still-missing INT 24h protocol.
NUL legacy input's unspecified character is zeroed rather than synthesizing
Ctrl-Z; the probe does not assert a native character value for that case.

On that same KVM 8 GiB/four-CPU snapshot, `dos-api-test` passes with 15/15
host-memory checks, `dosioctl.com`, `doscon.com`, and `dosexec.com` pass,
and `dpmi_native.com` exits with its expected code 42. Module-image/ABI
passes 45/45, and CRT format passes 194/194 for PE32 MSVCRT and PE64 UCRT.
No CPU fault or contract failure appears in the final log. QEMU was stopped
and reaped. The built and booted kernel SHA-256 is
`17c8f76ec6a53e16e2b7404f637f642da6644866be8c42bf2c440a8cb2683c0f`.

This does not complete DOS console compatibility. INT 23h control-break
callbacks, INT 24h critical-error actions, Ctrl-S/Ctrl-P processing, template
editing and caret/tab-aware erasure still need implementation. Legacy
blocking reads at disk EOF have no EOF return and yield while waiting;
cancellation and growth during that wait are not covered here. UART/printer
backends remain unavailable. No UT99 gameplay, audio-device integration, or
legacy Linux/Unix binary run was performed for this change.

## 4.26 DOS Ctrl-C callbacks and protected InDOS pointers (2026-09-05)

The standard-handle work in `27bcbad5` exposed another missing contract:
checked character input returned Ctrl-C as an ordinary byte and `AH=33h`
only stored a flag. Checked console calls now consume Ctrl-C, unwind the
active service, publish InDOS=0, and invoke the application's INT 23h.
Physical-console checks work with redirected stdin, including while waiting
at disk EOF. `AH=06h/07h` remain raw; BREAK ON also checks ordinary disk
services without changing the early `AH=33h/50h/51h/62h` dispatch contract.

Real-mode handlers can return with IRET or clear-carry RETF to redispatch
the service, including a changed AH. Set-carry RETF produces termination
type 1 and code 0; an explicit AH=4Ch in a handler retains its normal exit
code. A ROM default handler also supports chaining through the saved vector.
This follows the published
[MS-DOS Ctrl-C implementation](https://github.com/microsoft/MS-DOS/blob/2d04cacc5322951f187bb17e017c12920ac8ebe2/v4.0/src/DOS/CTRLC.ASM).
The nested interpreter stops at the host return address in the matching
CPU mode. It must unwind before reaping its current process, so termination
inside an INT 23h handler also works for an EXEC 4B01 load-only child.

DPMI uses its separate contract: an unhooked protected INT 23h is ignored,
and a hooked handler returns through a resident host stack without using
carry as an abort request. The existing callback-stack pool is shared by
depth, and the native outer interrupt context stays intact while nested
callbacks are interpreted. A handler can chain to the default reflector.
This covers direct protected calls and notifications raised during an
INT 31h/0300h simulated real-mode service. See Microsoft's
[MS-DOS API Extensions for DPMI Hosts, section 12](https://docs.pcjs.org/specs/dpmi/1991_03_11-MSDOS_DPMI_EXTENSIONS.pdf).

The new probes also found that AH=34h returned a real segment to DPMI
callers. It now returns a reusable segment-alias selector and synchronizes
new descriptors with the native LDT. INT 31h/0002h uses the same allocator.
Both direct native access and access inside callbacks verify that the
returned InDOS byte is zero after the query returns.

```sh
make -C arch/x86 CLANG=1 -j4 all build/test/dosbreak.com \
    build/test/dbchild.com build/test/dpbreak16.com build/test/dpbreak32.com
```

Copy these four COM files to the guest root. `dosrun dosbreak.com` (or
`dos_break.autoload`) expects one Ctrl-C after each RAW READY, EXTENDED READY,
and EOF READY marker. It checks raw bypass, IRET/RETF continuation, registers
and stack, edited-line restart, status consumption, ordinary and load-only
EXEC exits, BREAK ON/OFF, and cancellation at redirected EOF. `dpbreak16.com`
and `dpbreak32.com` need no keyboard input. They check the unhooked default,
installed handler, carry-independent return, chaining, InDOS, and real-mode
reflection. The tests use CREATE_NEW and remove only their owned fixtures.
These are source-derived probes, not differential runs against MS-DOS.

The initial real-mode probe fails at stage 3 on the previous kernel in
`/root/osito-dos-break-before-20260905-QGCWrg/`. The final run is
`/root/osito-dos-break-verified-20260905-DgBK2g/`, KVM with 8 GiB/four CPUs.
All three break probes pass, with both DPMI variants repeated after Win32.
DOS API/HOSTMEM (15 checks), stdio, IOCTL, console input, and EXEC pass;
the native DPMI probe exits with its expected code 42. Win32 module/ABI
passes 45/45 and CRT format passes 194/194 for PE32 MSVCRT and PE64 UCRT.
No CPU fault or contract-failure marker appears in that final log. The VM
was stopped and reaped. Built and booted kernel SHA-256:
`61c9d6d30b59ea213faa8c54129799cc12064190eea271a703d75188b29dc3b9`.

Remaining work includes INT 24h critical-error actions, physical Ctrl-Break
(INT 1Bh), Ctrl-S/Ctrl-P, full line editing, and standalone software INT 23h
delivery outside a checked DOS call. Callback-pool exhaustion, malformed
returns, and deeply nested handlers need dedicated negative coverage.
No UT99 gameplay, physical audio integration, or legacy Unix/Linux binary
was exercised in this slice. This does not complete the Win32/DOS layer goal.

## 5. Open questions / notes
- `WINAPI` selects the Microsoft x64 ABI for native shims, not the kernel's
  SysV ABI. Export arg-count metadata also drives the **32-bit thunk's `RET n*4`**.
  So GT-argc must be the count of
  **32-bit stack DWORDs the 32-bit caller pushed**, not the 64-bit ABI.
- Variadic CRT fns (printf family) are cdecl → caller cleans, so a too-large
  argc (12) is harmless for cleanup; it only affects how many stack DWORDs the
  dispatcher copies. Keep but document.
- COM vtable methods (DD_*, Surf_*) are stdcall-with-`this`; argc includes `this`.
