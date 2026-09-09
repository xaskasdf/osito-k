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

## 4.27 DOS real-mode critical-device errors (2026-09-05)

Unavailable AUX/PRN transfers now deliver the installed real-mode INT 24h
handler instead of bypassing it with error 21. The handler sees character
device/read-write/allowed-action flags in AH, device status 2 in DI, and a
BP:SI device header with the logical name and attributes. These host-owned
headers expose identity; their strategy/interrupt offsets are not guest
driver entrypoints. The standard user-stack frame includes the original
INT 21h registers and return frame. InDOS is zero during the callback,
and the preceding byte publishes the critical-error flag. A nested device
failure cannot recursively invoke INT 24h.

IRET responses implement ignore, retry, fail and abort. Retry yields and
reissues the current device transfer without replaying completed bytes.
Only explicit ignore permits an unsuccessful transfer to be treated as
completed; unread buffer bytes are left unchanged. Cooked non-console I/O
uses character-sized requests; raw I/O uses the requested block. Abort,
including an unknown response, produces termination type 2 and code 0.
An explicit AH=4Ch in the handler keeps its normal exit status. Invalid
RETF returns produce a diagnostic and fail the request instead of unwinding
the wrong guest stack. The default shell policy is noninteractive failure,
and its saved vector remains callable for chaining.

Handle reads/writes report CF with AX=5 after Fail; AH=59h reports 83,
class 13, action 4 and locus 1. Within the handler, the underlying not-ready
error is 21, class 5, action 7 and locus 4. The action/locus values were
corrected after checking the source tables, not inferred from the passing
probe. Legacy character calls retain this layer's existing CF/AX=21
extension on failure; those calls have no specified DOS carry-error ABI.
Invalid handles, access-mode failures, zero-length transfers and readiness
queries do not become critical errors.

Primary references are the published MS-DOS 4.0
[critical-error dispatcher](https://github.com/microsoft/MS-DOS/blob/2d04cacc5322951f187bb17e017c12920ac8ebe2/v4.0/src/DOS/CTRLC.ASM),
[device I/O](https://github.com/microsoft/MS-DOS/blob/2d04cacc5322951f187bb17e017c12920ac8ebe2/v4.0/src/DOS/DISK.ASM),
[error mapping](https://github.com/microsoft/MS-DOS/blob/2d04cacc5322951f187bb17e017c12920ac8ebe2/v4.0/src/DOS/MS_CODE.ASM)
and [classification tables](https://github.com/microsoft/MS-DOS/blob/2d04cacc5322951f187bb17e017c12920ac8ebe2/v4.0/src/DOS/MS_TABLE.ASM).

```sh
make -C arch/x86 CLANG=1 -j4 all build/test/doscrit.com \
    build/test/dcchild.com build/test/dosioctl.com
```

Copy both critical-error COM files to the guest root and run `dosrun
doscrit.com`, or use `dos_critical.autoload`. This probe requires no input
and creates no writable fixture. It checks the frame, registers, device
identity, InDOS/error flag, extended errors, raw/cooked ignore, retries
after partial completion, recursion suppression, chaining, invalid RETF,
ordinary and load-only EXEC aborts, explicit exits and default failure.
The initial probe fails at stage 2 on kernel `682dc27e` in
`/root/osito-dos-critical-before-20260905-92TtW4/`: no callback is delivered.
This is a before/after test of Osito-K, not a differential run of MS-DOS.

The full regression run is
`/root/osito-dos-critical-verified-20260905-hShmma/`: DOS API/HOSTMEM
(15 checks), real-mode Ctrl-C, DPMI16/native DPMI32 Ctrl-C, stdio, IOCTL
and EXEC pass; the native DPMI probe exits with expected code 42. Win32
module/ABI passes 45/45 and CRT format passes 194/194 for PE32 MSVCRT and
PE64 UCRT. The critical-error probe also passes again after Win32.
The expanded partial-retry probe passes twice in
`/root/osito-dos-critical-retry-20260905-tGGFmc/`. Both runs use the same
kernel with KVM, 8 GiB/four CPUs, isolated VNC :12/UDP 7790 and copied
snapshot disks. Neither log contains a CPU fault or contract-failure marker;
the invalid-INT-24-return diagnostic is intentional negative coverage.
Both VMs were stopped and reaped. Built and booted kernel SHA-256:
`433d37f14e972f0bc9a16a8e481f510f8e796f96ff99fa32c6853102314b9889`.

Protected-mode INT 24h hooks are still not delivered: their locked-stack
frame translation, including real-mode segment values and INT 31h/0300h
reflection, remains a gap. These requests fail rather than invoking a
real-mode handler on a protected stack. Actual UART/printer transports,
block-filesystem error provenance, installed guest device drivers and
handlers that unwind directly to the application also remain unfinished.
No UT99 gameplay, physical audio integration or legacy Unix/Linux binary
was exercised here. This does not complete the Win32/DOS layer goal.

## 4.28 DPMI reflected critical errors and DOS vectors (2026-09-05)

Critical-device failures raised inside a real-mode DOS call now reach the
client's protected INT 24h vector. The callback uses the resident DPMI stack
pool and receives SS:(E)BP pointing to the interrupt frame. For 32-bit clients,
only the handler IRET frame expands; the nine DOS registers and caller IRET
remain 16-bit values copied from the actual real-mode frame. Segment fields
are therefore real paragraphs, not fabricated protected selectors.

Ignore and Retry retain their DOS meanings. Abort and unknown responses fail
the DOS request instead of terminating the protected client. The unhooked
protected vector and calls chained through its saved address return Fail
without invoking an installed real-mode hook. Invalid RETF returns produce
a diagnostic and fail the request. The original CPU state, real stack,
InDOS/critical-error state and virtual interrupt state are restored on return.
Nested unavailable-device requests do not recursively deliver INT 24h.

The implementation exposed two adjacent contract gaps. Callback entry must
activate protected segment translation before pushing the IRET frame; otherwise
the frame is written through a real-mode address and read from the resident
protected stack. This ordering was corrected for INT 23h as well. Also, DOS
AH=25h/35h now use the existing DPMI set/get-vector services when called from
protected mode. They share the protected vector table and leave the IVT
untouched, with ES:EBX/DS:EDX pointers for 32-bit clients.

The contract follows sections 8.2.1 and 12 of Microsoft's
[MS-DOS API Extensions for DPMI Hosts](https://docs.pcjs.org/specs/dpmi/1991_03_11-MSDOS_DPMI_EXTENSIONS.pdf).

```sh
make -C arch/x86 CLANG=1 -j4 all build/test/dpcrit16.com \
    build/test/dpcrit32.com
```

Copy these COM files to the guest root and run `dosrun dpcrit16.com` and
`dosrun dpcrit32.com`; `dpmi_critical.autoload` starts the 16-bit variant.
Neither requires input or writable fixtures. The tests cover unhooked failure,
Ignore, Retry, Abort, chaining, invalid RETF, nested direct/reflected AUX
failures, DOS error metadata, full frame/register preservation, protected/real
vector separation, host/client real stacks, and a real-mode procedure called
through INT 31h/0301h. Virtual IF is tested with both caller states. The 32-bit
client runs natively, while the synchronous protected handler runs through
the interpreter so the suspended host service remains in control.

The initial probes fail at stage 3 on `3c89c79e` in
`/root/osito-dpmi-critical-before-20260905-1bxyiN/`, because no handler runs.
Intermediate runs detected the stack-translation and vector-table issues;
the tests were not weakened to accommodate them. The expanded final suite
passes in `/root/osito-dpmi-critical-verified-20260905-sTz28E/` with KVM,
8 GiB/four CPUs, isolated VNC :12/UDP 7790 and a copied snapshot disk.
Both critical-error variants pass twice, including after Win32 and IOCTL.
DOS API/HOSTMEM (15 checks), DPMI16/native DPMI32 Ctrl-C, real-mode INT 24h,
IOCTL and EXEC pass. Win32 module/ABI passes 45/45; CRT formatting passes
194/194 for PE32 MSVCRT and PE64 UCRT. The final log has no CPU-fault or
contract-failure marker; the invalid-return diagnostics are intentional
negative coverage. All owned QEMUs were stopped and reaped.

Built and booted kernel SHA-256:
`77b1838c89eaeb0222c8cacca5a587122216b2a63dbc273c423a77e02ab63364`.
Probe hashes: `dpcrit16.com`
`a93b469ddf22014102f7f42404e09f2c70f70903e232169516a1280a65cd6054`,
`dpcrit32.com`
`a69d09eed96d7ab992069edfdd88575f9f3ed3e30fae32b01c6e0456bf94a1e9`.

Direct protected-mode DOS device I/O still fails without delivering INT 24h.
That path needs an actual DOS buffer translator, including 32-bit offsets,
counts and returned byte counts, before it can supply a real-mode error frame.
This is distinct from the now-working explicit INT 31h reflection path.
INT 31h/0302h has no dedicated critical-error probe yet. Exhausted callback
stacks/descriptors and invalidated handler selectors need further negative
coverage. Physical AUX/printer transports, block-filesystem error provenance
and guest device-driver dispatch remain gaps. No UT99 gameplay, physical
audio integration or legacy Unix/Linux binary was tested in this slice;
the Win32/DOS layer goal remains unfinished.

## 4.29 Direct protected DOS handle I/O (2026-09-05)

Protected INT 21h/AH=3Fh and 40h now translate DS:(E)DX buffers and invoke
the real-mode DOS path. USE32 consumes ECX and returns EAX byte counts;
USE16 uses CX/DX and preserves the upper register words. Handle/access
checks precede translation. Buffer validation checks selector presence,
data/code access rights, segment bounds (including expand-down segments),
offset overflow and guest-memory ranges before starting the transfer.
Read-only data is accepted as an output source, not an input destination.

Each active call owns an MCB allocation containing a bounded bounce buffer
and a separate 4 KiB real-mode stack. The buffer is at most 65,520 bytes and
shrinks to fit available conventional memory. Larger requests are split,
without restarting completed chunks. Every normal/error return frees the
allocation. No free block means DOS error 8, not an overlapping host stack
or a false successful transfer. This currently requires free conventional
memory; reserving host workspace at DPMI entry remains future work.

The actual DOS dispatcher retains file positions, EOF, zero-length write
truncation and error metadata. Read scratch starts with the destination's
bytes so Ignore and partial failures preserve untouched data. Critical
device failures now reach the protected INT 24h handler for direct handle
I/O as well as explicit INT 31h simulation. Nested direct I/O gets a separate
allocation and stack. Critical Fail retains AH=59h error 83 while returning
the DOS handle error 5. Cooked CON terminators stop chunking even when the
line fills the scratch buffer exactly.

The USE16 probe exposed two interpreter defects: string operations selected
real-mode segment arithmetic whenever the address size was 16 bits, and
opcode 3Dh always consumed an imm16, even for CMP EAX,imm32. The former
could write through selector*16 instead of its descriptor; the latter
executed the remaining immediate bytes as instructions. String addresses
now use unified segment translation, and accumulator CMP honors operand
size. Both fixes are exercised by the probes, not bypassed in test code.

The I/O contract follows sections 5, 6 and 8.2.4 of Microsoft's
[MS-DOS API Extensions for DPMI Hosts](https://docs.pcjs.org/specs/dpmi/1991_03_11-MSDOS_DPMI_EXTENSIONS.pdf).

```sh
make -C arch/x86 CLANG=1 -j4 all build/test/dpio16.com build/test/dpio32.com
```

Run `dosrun dpio16.com` and `dosrun dpio32.com` in a disposable writable
guest: they create/replace `dpio16.tmp` and `dpio32.tmp` respectively, then
delete them on success. `dpmi_file_io.autoload` starts USE16. Coverage includes
65,024-byte USE16 and 131,107-byte USE32 transfers above 1 MiB, a USE32 offset
above 64 KiB, preserved register high words, data round trips, EOF/short reads,
truncation with a null buffer, access/limit failures, Ignore/Retry/Fail,
nested protected callbacks, unchanged free-memory capacity, a 16-byte bounce
buffer under memory pressure and graceful allocation failure. String checks
exercise MOVS/CMPS/LODS/SCAS/STOS and DF in both directions. `dosrun dpio16.com
con` or `dosrun dpio32.com con` additionally waits at `DPMI FILE IO CONSOLE
READY`; enter `abcdefghijklmn` and CR to test a 16-byte CR/LF line boundary.

The initial USE32 probe fails on the previous kernel in
`/root/osito-dpmi-io-before-20260905-IuknxJ/`. Intermediate USE16 failures
identified the string and CMP defects. Final verification is in
`/root/osito-dpmi-io-verified-20260905-KhiqcU/`, KVM, 8 GiB/four CPUs,
isolated VNC :12/UDP 7790 and a copied snapshot disk. Both variants pass
with console input and again after Win32/IOCTL. DOS API/HOSTMEM (15 checks),
USE16/native USE32 critical errors and Ctrl-C, real-mode critical errors,
IOCTL and EXEC pass. Win32 module/ABI passes 45/45; PE32 MSVCRT and PE64
UCRT formatting each pass 194/194. The final log has no CPU-fault or
contract-failure marker; invalid critical-handler return diagnostics are
intentional negative coverage. All owned QEMUs were stopped and reaped.

Built and booted kernel SHA-256:
`6c25ff585fc242567d078e96ddf518d504cd28dcd67e7520e193ff1145be71e1`.
Probe hashes: `dpio16.com`
`c09ff33bcde10b9585dbc0be6095219f23a36578e3441e13e98379f74a9083ac`,
`dpio32.com`
`1f7b6d330bc19dd32ea24514b3a2537ff2fc0729a41791f2ebb3472569ad2955`.

This does not implement a general protected DOS pointer translator. At this
stage legacy console/AUX/PRN services still needed translation (see 4.30).
Path/EXEC buffers and other pointer-bearing calls remain incomplete.
Non-identity guest paging, device-driver
hooks changing mappings during I/O, and block-filesystem critical-error
provenance need further coverage. No UT99 gameplay, physical audio or legacy
Unix/Linux binary was tested in this slice. GitNexus was unavailable; impact
was checked through call-site searches and the regressions above.

## 4.30 Protected legacy console and stable RM trampolines (2026-09-05)

Protected INT 21h/AH=01h through 0Ch now invoke the real-mode DOS path.
AH=09h translates a validated selector:(E)DX dollar-terminated string,
splitting long output into bounded real buffers. AH=0Ah and 0Ch/AL=0Ah
translate the complete line buffer, including its maximum/count header
and existing contents. A line is never split: editing and flush happen in
one DOS call. Zero-capacity input preserves the buffer and consumes no input.
Register-only services do not dereference an unused DS or DX.

Each active call owns an MCB buffer and a separate 4 KiB real-mode stack;
nested INT 23h/24h callbacks cannot overwrite an outer call's workspace.
String buffers can shrink with available conventional memory. A line that
does not fit, or a missing stack allocation, returns error 8 before input is
consumed. Input destinations require writable descriptors; output accepts
read-only data. Segment limits and guest ranges are validated before copying
and again after callbacks. Real DOS retains redirection, line editing,
Ctrl-C, Ignore/Retry/Fail and extended-error handling. CF is not treated as a
legacy-output success flag. Register high-word changes made by real-mode
hooks return to the caller, while translated offsets and protected selectors
are restored. This follows sections 5 and 6 of the Microsoft DPMI DOS
extensions cited in 4.29; legacy editing details follow Microsoft's MIT
MS-DOS 4.0 `CPMIO.ASM`, `CPMIO2.ASM` and `STRIN.ASM` sources.

Interactive testing exposed a second defect in shared DPMI simulation.
The host rewrote the INT operand at F000:0130 for every reflected interrupt.
Once the JIT compiled that address, later DOS calls could execute a cached
BIOS/IRQ interrupt instead. DPMI now initializes immutable `INT n; RETF`
trampolines at F000:1800..1BFF, one per vector. `dpmi_simulate_rm_call` selects
an entry instead of rewriting code. This also removes cross-vector code
overwrites during nested reflection, without flushing or disabling the JIT.
It does not solve general self-modifying guest-code invalidation.

```sh
make -C arch/x86 CLANG=1 -j4 all build/test/dpcon16.com build/test/dpcon32.com
```

Run `dosrun dpcon16.com` and `dosrun dpcon32.com` in a disposable writable
guest. The probes use CREATE_NEW for `dpcon16.in/out` or `dpcon32.in/out`,
restore stdin/stdout, and delete their owned fixtures on completion.
`dpmi_console.autoload` starts USE16. An optional `con` argument enables
interactive checks: send `J` at `DPMI CONSOLE FLUSH READY`, then `XY` and CR
at `DPMI CONSOLE LINE READY`. Waiting at the first marker also exercises
timer reflection before the next DOS call.

Coverage includes buffers above 1 MiB crossing page boundaries, a USE32
offset above 64 KiB, 4,117/65,557-byte string output and byte-exact readback,
CF preservation, redirected line editing and capacity handling, EOF/status,
raw character services, protected critical-error and Ctrl-C handlers, nested
calls, descriptor limits/access, memory-pressure fallback and full memory
release. A temporary real-mode INT 21h hook checks translated addresses and
modified register returns. Sixty-four INT 31h/0300h simulations warm the JIT
before console calls, making stale-trampoline failure deterministic.

The initial probes fail at segment validation (USE16) and high-offset input
(USE32) on kernel `6c25ff58...` in
`/root/osito-dpmi-console-before-20260905-v1emOe/`. Both hot-JIT variants
fail at input stage 3 on kernel `06dbd576...` in
`/root/osito-dpmi-console-hot-before-20260905-89K5MG/`: the log explicitly
shows INT 11h executing for AH=0Ah/09h. An intermediate probe incorrectly
assumed ES survived AH=59h, causing a test-side USE32 #GP; the corrected
probe reloads ES according to that API's result contract.

Final verification is in
`/root/osito-dpmi-console-hot-verified-20260905-XjA0dv/`, KVM, 8 GiB/four CPUs,
isolated VNC :12/UDP 7790 and a copied snapshot disk. USE16 and native USE32
pass with hot-JIT coverage and interactive input after prolonged keyboard
waits. DOS API/HOSTMEM (15 checks), direct protected file I/O, real/protected
critical errors, Ctrl-C, real DOS stdio, IOCTL and EXEC pass. The native DPMI
transition/callback probe returns its expected code 42. Win32 module/ABI
passes 45/45; PE32 MSVCRT and PE64 UCRT formatting each pass 194/194.

The old `dpmi_break.S` USE32 report left a deliberately nonzero upper EDX
word in its AH=09h pointer. Its contract checks returned zero but its message
was rejected by the new pointer validation. The probe now loads full EDX for
path/report pointers and checks the report result before returning success.
Rebuilt USE16/USE32 probes both print PASS and exit zero in
`/root/osito-dpmi-console-report-20260905-Md29Y9/` on the same kernel; the
console USE16 autoload also passes again there. Both final logs have no CPU
exception, panic-recovery or contract-failure marker. Invalid INT 24h return
diagnostics and the unsupported INT 11h warm-up stub are intentional test
coverage. All owned QEMUs were stopped and reaped.

Built/booted kernel SHA-256:
`5538ea1ef5bae4a6450b7d025702b1b20245dc33828f8c9ff1973e2b5a101167`.
Probe hashes: `dpcon16.com`
`c1d78489139e0d09b1e62b68c9f3b4c620f8f9666a31805d8267efb913101e9e`,
`dpcon32.com`
`10ac0217b90c906c9fe12e1dbb5a62a21187c480c639d87f00a2c5b8a1735448`.

Remaining work includes conventional workspace reservation at DPMI entry,
path/EXEC pointer translation, non-identity paging and mapping changes during
callbacks, and general JIT invalidation for guest code. Translation reuses
the existing line editor; full DOS template/function-key editing is not
implemented by this change. UT99 gameplay, physical audio and legacy Unix
binaries are not validated by these DOS probes. GitNexus was unavailable;
manual call-site analysis covered DOS dispatch, DPMI initialization, real-mode
simulation, callbacks, BIOS/IRQ reflection and their in-OS self-tests.

### 4.31 DOS/4GW bootstrap: JIT handoff and default BIOS vectors

On 2026-09-05 the local bound `DOOM.EXE` was tested through `dosrun`, not
Win32, on a fresh 128 MiB OsitoFS3 snapshot disk. The source archive is
`DOOM v1.9 [SWR] (1995)(id Software, Inc.) [Action].zip`; neither the archive
nor its extracted executable was patched. DOOM.EXE is 709,905 bytes, SHA-256
`b8020523561a5ad9706e009a52d61c578f37faafd85ac471962308406292ce27`.
The supplied WAD is 11,159,840 bytes, SHA-256
`ff2c301b8719465a6e386a512bfa319931b7f64ea517d337c5a47afe03951902`.

The executable contains a DOS/4GW Professional banner. The old DOS-native
status note incorrectly inferred that a nested `BW` DOS/16M header excluded
DOS/4GW. Rational's DOS/16M is the basis of DOS/4GW's protected-mode support,
as documented in the [Open Watcom redistribution guide](https://github.com/open-watcom/open-watcom-v2/blob/master/bld/redist/dos4gw/dos4gw.doc).

The unchanged kernel `5538ea1e...` stalled in real-mode startup at
`0232:62E9`. GDB observed about 8.7 billion JIT dispatches: generated blocks
for unsupported instructions returned their input IP, and the dispatcher
immediately ran them again. Stack operations had the same ambiguous return.
Hot counters wrapping to zero temporarily re-enabled interpretation, masking
the defect as extreme slowness. No renderer or audio initialization was
involved in this stall.

The generic JIT contract now distinguishes a completed block, a request for
one interpreted instruction, and an interrupt vector. A fallback resumes
decoding at the updated CS:IP in the same dispatcher iteration, including
partial blocks. PUSH/POP terminate decode, hot counters saturate, and INT 0
is no longer confused with a normal block return. INC/DEC preserve guest CF
rather than copying host carry. JZ/JNZ test guest ZF in memory rather than
loading guest RFLAGS into the kernel, preserving host IF/DF/control flags.
An exhausted 500M interpreter budget now reports failure, not exit zero;
the existing limit itself was not increased.

With that corrected, the actual guest loop still searched the IVT for a
shared default interrupt handler. All 256 unassigned vectors previously had
different IRET addresses. Initialization now shares the no-service handler
and retains distinct installed-service entries, mouse detection, and the
DOS INT 23h/24h policies. This follows the common-default initialization used
by [SeaBIOS](https://github.com/coreboot/seabios/blob/master/src/post.c), without
special-casing a program name, patching IVT entries during execution, or
inventing protected-mode descriptors.

```sh
make -C arch/x86 CLANG=1 -j4 all build/test/dosjit.com build/test/dosivt.com
```

Copy those two COM files to a disposable DOS test disk. `dosrun dosjit.com`
warms 256 iterations covering memory/prefix fallbacks, PUSH/POP/CALL/RET,
INC/DEC carry, both outcomes of JZ/JNZ with guest DF/IF, and a hooked INT 0.
`dosrun dosivt.com` checks shared unused vectors, a direct far call to their
IRET, INT dispatch, and distinct mouse/break/critical handlers. Neither
creates guest files. `dos-api-test` includes 97 host-memory/JIT checks,
including partial/zero-progress exits, preserved register high words, and
host-control-flag isolation.

Evidence directories (all isolated KVM, 8 GiB/four CPUs, VNC :12/UDP 7790):

- `/root/osito-dos4gw-before-20260905-ukG2M9/`: unchanged kernel, JIT stall.
- `/root/osito-dos4gw-jit-20260905-s55TTf/`: JIT fixes pass, but the unique-IVT
  scan exhausts the old budget without reaching CPU detection.
- `/root/osito-dos4gw-ivt-20260905-FHiJx1/`: final JIT/IVT fixes, original
  `dosrun DOOM.EXE -nosound -timedemo demo1` passes the scan and returns to
  the shell on guest #UD after 2,272 interpreted instructions.

The blocker measured at this stage was `66 0F BA FA 15` (`BTC EDX,21`) at guest
`0232:6601`, file offset `0x83D1`, during CPU identification. Group-8 bit
operations were absent from the interpreter; section 4.32 records their
subsequent implementation and the next measured blocker. This run does not validate
protected game startup, VGA rendering, a timedemo result, or game audio.

Final-kernel regression results: `dosjit.com`, `dosivt.com` and
`dos-api-test` PASS (97 host-memory/JIT checks, zero failures). The 16-bit
and native 32-bit break, critical-error, file-I/O and console probes all
PASS and exit zero. The interactive `dosrun dpcon32.com con` also PASSes
after the flush/read and buffered-line checks, then returns to the shell.
`win32-module-test` reports 45 checks, zero failures. All three diagnostic
QEMUs were stopped through their own QMP sockets; unrelated sessions and
disks were not changed. Serial logs remain in the evidence directories.

Final built/booted kernel SHA-256:
`5e05b0b08298142a737b8e37b3bafa6294f9e6f96656841f1a488cecb60c9e8d`.
New probe hashes: `dosjit.com` (265 bytes)
`70473aa39a2af569a5dd725f559dd9eda0f834bd755fa914fa7604e54fe62d74`,
`dosivt.com` (165 bytes)
`17441c0efe9e1fc61d1cb45137188461baf5d3ef833131788a95b4aafd7f954b`.

GitNexus was unavailable. Manual impact analysis covered the two JIT API
callers (`cpu8086_run` and `dos_hostmem_selftest`), decoder/code generator,
IVT initialization and real/protected interrupt delivery. General guest-code
invalidation, JIT cache turnover, and scheduling/accounting in fully compiled
loops remain separate work; these fixes do not claim to solve them.

### 4.32 DOS bit instructions and DOS/4GW CPU detection

The subsequent 2026-09-05 test implements `BT`, `BTS`, `BTR` and `BTC` in
the interpreter: `0F A3/AB/B3/BB` and group-8 `0F BA /4..7`. This resolves
the CPU-detection blocker recorded in section 4.31 without modifying DOOM,
forcing an extender mode or supplying synthetic selectors.

Both 16/32-bit operand sizes accept register or immediate indices. Register
destinations reduce the index modulo their width; memory register indices
are signed and can select preceding/following elements. Immediate high bits
are ignored, not added to the memory address. CF receives the original bit;
ZF/control flags are preserved, with undefined arithmetic flags also left
unchanged by policy. `LOCK` is accepted for modifying memory forms, rejected
for register destinations and BT; group-8 /0..3 raises #UD. These rules follow
the [Intel instruction reference](https://cdrdv2-public.intel.com/812383/253666-sdm-vol-2a.pdf).

`decode_modrm_offset` applies the signed element displacement before segment
translation and 16-bit address wrapping; the ordinary `decode_modrm` wrapper
passes zero, preserving other opcode paths. The virtual CPU executes each
read/modify/write before delivering guest interrupts. This is not a general
LOCK-prefix audit or a shared-memory SMP implementation. Existing segment
permission/limit and paging-fault delivery gaps remain; the test deliberately
excludes invalid real-mode 32-bit offsets instead of blessing their current
truncation in `dos_addr`.

`dos-api-test` now includes 14,534 bit-instruction cases: real mode, protected
USE16/USE32, operand/address overrides, register aliasing in the separate COM,
DS/SS/FS bases, SIB/displacement, negative indices, immediate masking,
16-bit destination high-word preservation and neighboring memory canaries.
Fourteen cases verify #UD delivery, saved instruction-start IP and unchanged
destinations. `dosrun dosbits.com` warms 256 iterations through the JIT
fallback and specifically toggles EDX bits 21/18 as used by DOS/4GW.

```sh
make -C arch/x86 CLANG=1 -j4 all build/test/dosbits.com
```

Evidence directories use independent 128 MiB OsitoFS3 snapshot disks, KVM,
8 GiB/four CPUs, VNC :12 and UDP 7790:

- `/root/osito-dos4gw-bits-20260905-mHfDVJ/`: initial matrix, COM and DOOM;
  `ud-gdb.log` records a read-only exception inspection.
- `/root/osito-dos4gw-bits-final-20260905-Yxs0eu/`: final kernel and regressions.

At this checkpoint, `dosrun DOOM.EXE -nosound -timedemo demo1` discovers DPMI,
enters as a 32-bit client with USE16 bootstrap selectors and installs its
handlers. The next unsupported instruction is `0F 02 C8`, `LAR CX,AX`, at
`0087:7051`; live GDB confirms those bytes at linear `0xC341`, with the CS
base `0x52F0` recorded during DPMI entry. They correspond to file offset
`0x8E21` in the relocated bootstrap. DOS/4GW handles the exception, displays
`Error [35]: Invalid Opcode` and exits with code 255 to the shell. Native
execution visible in this trace belongs to exception delivery, not gameplay.
Section 4.33 implements the missing selector queries and records further
progress. No game rendering, timedemo result or game audio is validated here.

Final-kernel results: `dos-api-test` PASS, including 14,534 bit-instruction
cases and 97 host-memory/JIT checks, all with zero failures; `dosbits.com`,
`dosjit.com` and `dosivt.com` PASS/exit zero. The 16/32-bit break, critical,
file-I/O and noninteractive console probes all PASS/exit zero.
`win32-module-test` reports 45 checks and zero failures. DOOM reproduces the
LAR diagnostic and exit 255 on the final kernel. Both temporary QEMUs were
closed through their own QMP sockets; the GDB session detached and exited.
No user disk, unrelated VM, or original executable was changed.

Final kernel (5,506,712 bytes) SHA-256:
`a87366d7ffb1665f4140ba6b5745e7953dd2e5fc7bde1d023acdfc0b0129023f`.
`dosbits.com` (342 bytes) SHA-256:
`0c5be63514cb958a7928ea52ffcc7b7ffd5ce2193e248ea902293c79f9cf27b6`.

GitNexus was unavailable. Manual impact analysis covered the local ModR/M
decoder callers, `cpu8086_run`, DOS EXEC, DPMI callbacks/reflection, and
`dos_interrupt_selftest` as invoked by `dos-api-test`. The JIT layout/API,
descriptor representation and unrelated Win32 application paths are unchanged.

### 4.33 DOS selector queries and DOS/4GW bootstrap

The interpreter now implements LAR, LSL, VERR and VERW from actual guest
descriptors. Null/out-of-table selectors fail the query; type and CPL/RPL/DPL
checks determine visibility, with the conforming-code exception. A visible
nonpresent descriptor may be queried. LAR returns access rights, LSL expands
the limit's granularity, and failure leaves the destination unchanged while
clearing ZF. Source selectors are always 16 bits; successful 16-bit results
preserve the destination's high word. Real-mode, VM-flag and LOCK use raises
#UD instead of inventing a descriptor. [Intel instruction reference](https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf).

The shared descriptor reader accepts a GDT at linear zero and checks
base-plus-offset arithmetic at 64-bit width before accessing guest memory.
This prevents an overflowing table address from aliasing low memory. It is
not a replacement for still-incomplete segment access and paging exceptions.

`dos-api-test` covers 786,500 cases: host LDT, guest GDT and guest LDT; every
access byte, CPL/RPL combination, default/overridden operand and address size,
register/memory operands, source/destination aliasing, descriptor canaries,
invalid tables, and #UD guards. All pass on the final kernel in section 4.35.
The VM-flag tests exercise opcode rejection, not a complete VM86 monitor.

DOOM passes its former `LAR CX,AX` failure at `0087:7051`. The intermediate
run `/root/osito-dos4gw-selectors-20260905-2U0Oys/` then exits with code 8.
Read-only GDB tracing identifies a separate defect: DPMI 0302h calls the
saved real-mode DOS vector `F000:0021`, but that vector contains only IRET.
The apparent calls to allocate, seek, read and print never reach DOS. During
cleanup, restoring an exception vector previously returned as null also
fails with 8022h. That secondary default-exception-vector contract remains
open; it is not the cause of the skipped real-mode services.

### 4.34 Callable real-mode DOS and BIOS service vectors

Installed host service vectors now point to immutable ROM stubs in
`F000:1C00..1FFF`, separate from DPMI entry/callback/reflection stubs. A
validated private INT dispatches the selected service, writes status flags
into the caller's outer interrupt frame, and IRET restores its control
flags. This supports both PUSHF/CALL FAR chaining and DPMI 0302h, which uses
the explicit CS:IP in the real-mode register structure. [DPMI specification](https://openwatcom.org/ftp/devel/docs/dpmi10.pdf).

Calling a saved handler does not redispatch through a newly installed IVT
hook. The private opcode outside its ROM entry remains a guest interrupt;
ROM aliases are recognized by their linear address. Unassigned vectors keep
their shared IRET, and the existing timer, Ctrl-C and critical-error policies
are unchanged. No application names or executable signatures select a path.

`dosivt.com` now exercises hot FAR calls, returned DOS/BIOS status, stack
balance, IF/DF restoration and a guest hook on the private interrupt number.
`dprom16.com` and `dprom32.com` call the saved DOS vector with 0302h after
replacing the IVT entry, then verify version, allocation/release, invalid
handle errors and opening/reading/closing NUL with a memory canary.

All three probes fail on the pre-fix kernel in
`/root/osito-dos4gw-rom-before-20260905-VFoW4C/` and pass on the corrected
kernel in `/root/osito-dos4gw-rom-after-20260905-drFuxU/`. DOOM now allocates
extended memory and loads the next executable segments. It proceeds to a
later corrupted interrupt return, diagnosed in section 4.35.

### 4.35 DOS/4GW interrupt frames and memory POP

The next DOOM failure was an IRETD into `0000:00000000`. The loaded handler
uses operand-size-prefixed POPs into memory when saving its interrupt frame,
but opcode 8F always popped and wrote only 16 bits. That consumed the wrong
number of stack bytes before the eventual return.

8F /0 now honors the 16/32-bit operand size independently of SS stack size.
ESP-based memory destinations use the incremented ESP, while POP ESP writes
its destination after incrementing the old stack pointer. Invalid group
members and LOCK raise #UD before consuming data. [Intel POP reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf).

The 208-case selftest covers every general-purpose register destination,
memory destinations, SIB/ESP addressing, both operand/address sizes,
real/protected mode, USE16/USE32, mixed stack sizes, high-word preservation,
unchanged flags and invalid-opcode frames. It avoids the processor-specific
POP [ESP] destination case when a 16-bit stack wraps. `dospop.com` adds
128 hot iterations through the JIT fallback. Its negative control fails on
the old kernel in `/root/osito-dos4gw-pop-before-20260905-vLOb2m/`; it passes
on the corrected kernel.

```sh
make -C arch/x86 CLANG=1 -j4 all build/test/dosivt.com \
  build/test/dprom16.com build/test/dprom32.com build/test/dospop.com
```

Final validation directory: `/root/osito-dos4gw-pop-final-20260905-9eEgpJ/`.
Like the preceding runs, it uses an independent 128 MiB OsitoFS3 snapshot,
KVM, 8 GiB/four CPUs, VNC :12 and UDP 7790. The original DOOM EXE/WAD are
unchanged; no user disk or unrelated VM is used.

Kernel (5,515,048 bytes) SHA-256:
`82ac69211b9fe5a9aeed120e9adf440336f15b348ea75231214eb06624dfe550`.
`dospop.com` (184 bytes):
`529204ef5514736c85cd459ba9cac90777a20897ebdac7fd1c0962cd28d616d7`.

Final-kernel regressions pass: `dos-api-test` (786,500 selector, 14,534 bit,
208 POP and 97 host-memory/JIT checks, zero failures), `dosjit.com`,
`dosbits.com`, `dospop.com`, `dosivt.com`, `dprom16.com`, `dprom32.com`, and
the 16/32-bit break, critical-error, file-I/O and console contracts. Each
COM returns zero. `win32-module-test` reports 45 checks and zero failures.

The original `dosrun DOOM.EXE -nosound -timedemo demo1` now reaches the game,
prints `timed 1710 gametics in 98 realtics` and returns to the shell. The
guest exit code is 1, not zero; cleanup also logs AX=8022h/8001h calls after
the timedemo result. Default exception-vector restoration remains open as
described in section 4.33. This run establishes completion of the timedemo,
not accurate wall-clock performance, clean API restoration, audio or a
fully playable display.

A second launch, `dosrun DOOM.EXE -nosound -warp 1 1`, renders recognizable
E1M1 content but repeats it in a mosaic within the centered 320x200 surface.
An Escape key sent through QMP produces no visible menu. Read-only GDB
inspection records native execution at `01F7:0023E99D`, with VGA sequencer
memory mode 04h (Chain-4 off), map mask 01h, graphics read map 03h and CRTC
mode control 40h. The presenter still reads A0000h as packed 320-byte rows;
the memory helpers and native mappings do not implement VGA plane/latch
semantics. Register values are stored by the I/O bus but do not select
backing planes or affect the scanout address. The next graphics work must
cover that memory contract, native accesses and CRTC scanout together,
rather than reshaping the already-overwritten aperture for this executable.

Input is separately unvalidated: BIOS/DOS console polling is not an IRQ1
keyboard path, and native interrupt service currently wires timer and audio
IRQs, not keyboard delivery. The observed keyboard controller queue remains
empty after the Escape test. Scancode make/break delivery and the controller
to PIC to guest-handler route need focused tests before claiming gameplay.

Evidence in the final run directory includes `serial.log`, `doom-first.png`
(timedemo result), `doom-e1m1.png` and `doom-menu.png`. Local ignored artifacts
include `arch/x86/build/dos4gw-vga-state.log` and the 64 KiB
`dos4gw-vga-aperture.bin`; the GDB script resets physical-memory inspection
mode before detaching. All diagnostic GDB and QEMU sessions from sections
4.33-4.35 have exited. Only temporary VMs were stopped through their own QMP
sockets; user sessions :0/:1 and original images were untouched.

GitNexus tools were unavailable. Manual impact analysis covered descriptor
readers used by CPU/DPMI/native selector loading, the interpreter and its
JIT fallback, ROM vector initialization, real-mode/DPMI interrupt chaining,
and the DOS API selftest entry point. No Win32 shim ABI or application-name
special case was added. `git diff --check` passes; no commit was made.

### 4.36 DOS/4GW VGA memory and native MMIO

The original DOOM executable now renders a coherent scene and HUD instead
of four interleaved copies. The change is in the DOS VGA contract, not the
executable or its content. Each VM owns 256 KiB of video memory, released
with its I/O state. CPU reads load four latches; writes implement modes
0-3, set/reset, rotate/logical operations, bit masks and sequencer plane
selection. Read mode 1 applies color compare/don't-care. Chain-4 and
odd/even addressing do not rearrange stored plane data when toggled.

BIOS mode 13h programs sequencer, graphics and CRTC defaults, with the
no-clear bit preserving video memory. Indexed scanout uses CRTC start,
offset, addressing mode and scan repetition rather than treating A0000h
as permanently packed pixels. Page changes, palette/mask writes and
display blanking invalidate presentation. Geometry changes clear the old
footprint. The timer presenter switches to the kernel CR3 before using
the bound VM, whose shell-stack address is not mapped by the DOS CR3.

Architectural reference: IBM's [VGA/XGA Technical Reference Manual,
May 1992](https://bitsavers.trailing-edge.com/pdf/ibm/pc/cards/IBM_VGA_XGA_Technical_Reference_Manual_May92.pdf),
sequencer memory mode, graphics-controller data paths and CRTC addressing.
The physical plane representation is shared by the interpreter and native
path; scanout does not mutate CPU latches.

Native CPL3 video accesses now intentionally fault on the supervisor-only
A0000h-BFFFFh aperture in VGA graphics mode. The early DOS handler accepts
only user data-protection faults on the active VM's CR3 and mediates
supported integer memory instructions through the existing interpreter.
Other faults retain their normal path. A single-step invocation neither
overwrites the suspended native-return context nor runs scheduling/process
lifecycle work. REP executes at most 256 iterations per fault, restarting
at the original instruction with the remaining count. CMPS/SCAS stop
conditions and DF are preserved across chunk boundaries. VBE and text
keep ordinary native mappings. Successful CS-mode trace messages are
bounded to 16 per CPU state; selector errors remain visible. Cleanup
reports a per-VM count of successfully mediated VGA faults.

Validation commands:

```sh
make -C arch/x86 CLANG=1 -j4 all build/test/dpvgamem.com \
  build/test/dpmi_vbe.com build/test/dosvga.com
```

The new `dpvgamem.com` probe exercises native byte/word/dword access,
read-modify-write, ES/FS/GS overrides, REP copies and fills, comparison
stops immediately before/after the chunk boundary, descending transfers,
plane isolation and the return to native text access. It exits zero and
reports 26 mediated faults. Its SHA-256 (2,756 bytes) is
`98563ca8db8436e46dc39e63370877ff6b8cadae69e1edba9a6858d6d6510d2e`.
The I/O selftest adds 328,923 checks, including a bit-level write-pipeline
reference, read compare, latch retention, Chain-4 toggles, Mode X page
selection, panning, blanking, aperture maps and separate VM storage.

The first isolated run, `/root/osito-dos4gw-vga-20260905-7CtJeh/`, passes
`dos-api-test`, `dpvgamem.com`, the JIT/bit/POP/IVT and 16/32-bit ROM,
break, critical-error, file-I/O and console probes. VBE exits zero; the
interactive `dosvga.com` text/indexed stages also return zero after input.
Console probes are allowed to finish before issuing another shell command:
serial silence alone is not a completion marker. `win32-module-test`
passes 45 checks. DOOM completes 1,710 gametics in 2,052 reported realtics;
two captures show different, correctly arranged gameplay frames.

Final kernel (5,463,288 bytes), with trace limiting and fault counting:
`d5091a557daf4e865c2fc28b47d8b0dca68a29c9807a6f822ddb1698381dfee0`.
Final run: `/root/osito-dos4gw-vga-final-20260905-7atRwq/`, independent
128 MiB OsitoFS3 snapshot, KVM, 8 GiB/four CPUs, VNC :12 and UDP 7790.
The original EXE/WAD hashes from section 4.33 are unchanged. The final
kernel repeats the DOS API tests (including all VGA checks), JIT startup,
native VGA probe, VBE probe and Win32 module test successfully.

Final DOOM command: `dosrun DOOM.EXE -nosound -timedemo demo1`.
It prints `timed 1710 gametics in 1425 realtics`, returns to the shell,
and reports **74,013,124 mediated video-memory faults**. This is an
intentional MMIO count, not 74 million application crashes. It identifies
a substantial amount of emulation work to optimize; it does not by itself
attribute a percentage of CPU time. Guest realtics are not a validated
wall-clock benchmark, particularly under heavy fault/IRQ traffic.

Both DOOM runs still exit with code 1 and AX=8022h/8001h cleanup warnings.
Keyboard IRQ1 delivery remains absent; BIOS console input is not proof of
native gameplay input. Audio was disabled. Four-color shift modes, the
sequencer's reduced-memory configuration, the broader BIOS video-mode
table and other CRTC combinations are not fully implemented/validated.
Native x87/vector memory operations and segment/control-transfer operands
on VGA remain unsupported and follow the diagnostic fault path. There is
no claim of complete VGA or DOS compatibility.

Evidence: each run retains `serial.log` and kernel/disk snapshots. Final
captures are `doom-vga-final.png` and `timedemo-result.png`; the local
ignored copy is `arch/x86/build/dos4gw-vga-final.png`. Both temporary QEMU
sessions and all command helpers have exited. Only their own QMP sockets
were used to stop them; user VMs :0/:1 and user images were untouched.
GitNexus was unavailable. Manual impact analysis covered DOS memory and
I/O, BIOS/VBE mapping, the interpreter and native return context, the
guarded IDT handler, presentation and cleanup. `git diff --check` passes.
No commit was made.

### 4.37 DOS/4GW keyboard ownership and IRQ1

The original `DOOM.EXE` now accepts real keyboard input through the DOS
hardware contract. USB HID transitions and PS/2 set-1 bytes feed an
exclusive, owner-checked input stream. Producers never dereference the
owner token, so a VM on the host shell stack remains safe to identify
while the native DOS CR3 is active. Acquisition happens after loading;
cleanup releases the stream. Captured keys do not also enter the desktop,
Win32 or shell queues. USB packets preserve extended make/break prefixes,
modifiers, keypad keys and the Print Screen/Pause sequences.

The per-VM 8042 output buffer now drives IRQ1 through the virtual PIC.
Port 60h consumes controller bytes, not BIOS words. Port 64h reports OBF;
interface/IRQ enable bits, PIC masks, in-service priority, EOI and a fresh
output-buffer edge govern delivery. Command D2h can enqueue an output
byte, which the regression probes use without host input. EOI alone does
not repeatedly deliver the same unread byte. The existing controller
model in [QEMU's pckbd.c](https://raw.githubusercontent.com/qemu/qemu/master/hw/input/pckbd.c)
was consulted for output-buffer and IRQ-line semantics.

The default ROM IRQ1 handler translates set-1 input into BIOS key words
and modifier flags. Installed real/protected-mode handlers still receive
the interrupt first; BIOS polling does not bypass them with host ASCII.
The default IRQ0 handler now chains INT 1Ch and sends its own specific
EOI, necessary once the PIC respects in-service priority. Interpreter
polling and native CPL3 timer service deliver timer, keyboard and audio
in that priority order. Native INT 1Ah now has a saved/restored IDT gate,
and pending IRQs are also serviced at BIOS/DOS return boundaries. A tight
native clock-polling loop otherwise spent every host timer sample inside
the kernel and starved the client keyboard handler.

Blocking INT 16h enables host interrupts and uses HLT between polls,
preserving the caller's host IF state on return. In the final run, QMP
sampled `HLT=1` in `dos_int16_keyboard` while `dosvga.com` waited; USB,
PS/2 and serial input each woke the probe and it returned zero. This
checks blocking BIOS input, not the CPU cost of applications that choose
to busy-poll the nonblocking API.

Build and guest probes:

```sh
make -C arch/x86 CLANG=1 -j4 all build/test/doskey.com \
  build/test/dpkey16.com build/test/dpkey32.com
# In the guest:
dos-api-test
dosrun doskey.com
dosrun dpkey16.com
dosrun dpkey32.com
dosrun DOOM.EXE -nosound
```

`dos_keyboard_irq.S` builds real-mode, DPMI16 and native DPMI32 variants.
Each masks IRQ1, inserts `1E 9E E0 48 E0 C8`, verifies no masked delivery,
unmasks, checks the exact handler byte order/ISR state, restores the old
vector, then checks default BIOS translation with another byte. All three
pass with seven controller reads and seven IRQ1 deliveries. The I/O
selftest adds **564 checks, zero failures**, covering controller/BIOS
queue separation, per-VM isolation, edge/EOI behavior and capture ownership,
including wrong-owner operations and bounded FIFO overflow. BIOS tests
also cover letter, Shift, Ctrl and extended-arrow translation.

USB gameplay evidence is in
`/root/osito-dos4gw-keyboard-return-20260905-3pr6WI/`: new game, movement,
turning and firing work; ammunition changes from 50 to 48 and remains at
48 after release. F10/Y exits zero, with 38 host bytes, 38 reads and 38
IRQ1 deliveries. A subsequent USB-typed `help` reaches the shell.

Final kernel (5,476,616 bytes, including the HLT wait):
`3fa64a8f853ddc7a1251bebd1eaf35f9353c1730698f13baadddbf8bc05564a5`.
Final run: `/root/osito-dos4gw-keyboard-verified-20260905-Y8P1uH/`, an
independent 128 MiB OsitoFS3 snapshot, KVM, 8 GiB/four CPUs, VNC :12 and
UDP 7790. EXE/WAD hashes from section 4.33 are unchanged. The native
console probe passes its interactive J/XY/Enter checks with USB. After
removing only this temporary VM's USB keyboard through its QOM path,
PS/2 repeats the BIOS probe and a DOOM new game: fire, forward, turn,
release and exit all work. Exit is zero, with **22 host bytes, 22 reads,
22 IRQ1 deliveries**; a PS/2-typed `help` works afterward. The final
gameplay captures are `doom-ps2-newgame.png`, `doom-ps2-fired.png`,
`doom-ps2-moved.png`, `doom-ps2-turned.png` and `doom-ps2-released.png`.
The last two retain the same camera view and ammunition after release.

The final kernel also passes `dos-api-test` (including 328,923 VGA checks),
JIT startup, bit/POP/IVT probes, both ROM-chain/critical-error/break/file-I/O
variants, native VGA (26 mediated faults), VBE and the 45-check Win32
module test. Long redirected console output still takes time; one live
sample during its file test resolved to `nvme_io_submit_wait`. This is a
separate I/O performance follow-up, not proof of keyboard deadlock.

Remaining limits: keyboard scan sets 2/3, typematic programming, complete
enhanced-BIOS behavior and direct BDA keyboard-ring access are not covered.
Overflow reports set-1 byte 00h but does not reconstruct lost transitions;
capture handoff while modifiers are already held needs separate tests.
Gameplay audio remains disabled. VGA MMIO emulation remains substantial
(101,133,974 mediated faults in the final interactive run); this is neither
a crash count nor a wall-clock benchmark. AX=8001h/8022h DPMI cleanup
warnings remain despite the zero exit status. These results establish a
tested keyboard path, not complete DOS or DOS/4GW compatibility.

All four temporary keyboard-test QEMU runs and command helpers have exited.
Only their own QMP sockets were used; user sessions :0/:1 and original
images were untouched. GitNexus was unavailable; manual impact analysis
covered PS/2/USB producers, owned input, 8042/PIC state, BIOS/ROM vectors,
interpreter/native IRQ delivery and IDT restoration. No executable-name
special case was added. `git diff --check` passes; no commit was made.

### 4.38 DOS sound DMA request ordering and remaining DOOM audio blocker

The initial DOOM audio run was silent by configuration: `default.cfg` had
both `snd_sfxdevice` and `snd_musicdevice` set to zero. A separate
`audio.cfg` in the temporary test image selects Sound Blaster at 220h,
IRQ5, DMA1, eight channels, with both volumes set to eight. Neither the
original EXE/WAD nor the user's images were modified.

With `dosrun DOOM.EXE -config audio.cfg`, the original executable stalls
in `I_StartupTimer()`. Live samples show a USE32 CPL3 loop at 002343E2h
waiting for the counter at 0028E820h. PIT channel 0 is running with reload
214Ah; its period counter advances and IRQ0 is pending. PIC masks and
in-service bits are clear, physical IF is set, but the client's virtual
interrupt flag remains clear. The protected-mode vectors at selector
0117h belong to DOS/4GW's own USE16 handlers, not the host's default ROM.

DMX uses critical sections with PUSHF/CLI/POPF. Native CPL3 POPF does not
trap and cannot restore IF at IOPL0. This behavior is permitted by the
[DPMI 1.0 specification, virtual interrupts](https://docs.pcjs.org/specs/dpmi/1991_03_12-DPMI_Spec_v10.pdf#page=28):
clients of a virtual-IF host must use STI or function 0901h to enable
interrupts; PUSHF reports physical flags, and POPF/IRET do not restore
virtual IF. Supporting legacy code that assumes different semantics
needs a general execution/virtualization solution, not an IRQ timeout,
unconditional re-enable, guest binary patch or elevated host I/O privilege.
No such workaround was added. DOOM audio is still blocked here.

For diagnosis only, one discarded VM had VIF enabled once through GDB.
It advanced to Sound Blaster initialization, produced a first playback
request, then rejected a later DSP 14h request against an exhausted DMA
descriptor. It also read the unimplemented OPL status port 388h. This
intervention is not a successful audio run and is not part of the kernel.
Hardware breakpoints are needed for this QEMU/KVM trace: a software
breakpoint across a guest CR3 change escaped GDB and raised a host #BP in
an earlier discarded diagnostic attempt.

An independent native probe reproduced one concrete DMA ordering defect:
after the first single-cycle transfer, masking DMA1 and issuing the next
DSP command before reprogramming DMA caused an immediate failure/IRQ.
The layer validated the old exhausted address/count even though memory
access was not enabled. The old kernel fails this probe with exit 75h.

`dos_audio.c` now retains the DSP request while its selected DMA channel
is masked or its controller is disabled. Address/count binding and buffer
validation happen when the channel becomes enabled. Waiting requests
produce neither memory access nor completion IRQ; the DMA status exposes
the request. Pause/resume, reset, queued single-cycle playback after
auto-init, and a partially received subsequent DSP command keep their
own state. This covers the existing legacy PCM/ADPCM and SB16 playback/
capture commands, without changing the shared PCM scheduler or HDA.
DMA mask/request semantics were checked against the
[Intel DMA register documentation, sections 12.2.4-12.2.6](https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/8-series-chipset-pch-datasheet.pdf#page=403).

Build and regression commands:

```sh
make -C arch/x86 CLANG=1 -j4 all dos-audio-native-test
# In the guest, with build/test/dpmi_audio.com copied to the test image:
dosrun dpmi_audio.com
dos-api-test
dosrun dpmi_audio.com
dosrun dpkey32.com
dosrun DOOM.EXE -nosound
```

The extended native audio probe performs three 4096-byte 48 kHz transfers,
including two DSP-before-DMA rounds. Each checks one IRQ5, DSP acknowledge,
PIC in-service state before/after specific EOI, virtual IF on handler entry
and return, terminal-count read/clear, and final DMA address/count. Both
invocations pass with exit 2Ah. The selftest adds **102 checks, zero
failures** for 8/16-bit playback/capture, channel/controller gates,
pause/resume, parser preservation, queued auto-init exit and reset.
The complete `dos-api-test` passes, including 564 keyboard and 328,923
VGA checks. The subsequent native keyboard probe passes with seven reads
and seven IRQ1 deliveries. DOOM renders its attract demo without sound
and exits zero through F10/Y, returning to the shell. Existing DPMI
8001h/8022h cleanup warnings remain; this is not full DOS compatibility.

Final kernel: 5,484,808 bytes, SHA-256
`1a8aec89f9224f96968839ba3bd59ad60a0a1a35a3213ab4c54a19df901860c7`.
Final run: `/root/osito-dos4gw-dma-wait-20260905-js0MKR/`, 128 MiB OsitoFS3
snapshot, KVM, 8 GiB/four CPUs, VNC :12 and UDP 7790. It retains the kernel,
serial log, disk, `doom-nosound-final.png` and `audio.wav`. The finalized
WAV contains 24,625 stereo frames at 48 kHz (about 0.513 seconds), identical
left/right channels and both -16,384 and +16,384 samples. This establishes
actual PCM output from both probe invocations, not bit-perfect timing or
working DOOM music. EXE/WAD hashes from section 4.33 are unchanged.

The before-fix ordering run is
`/root/osito-dos4gw-dma-order-20260905-Iq9J8r/`; timer diagnosis is in
`/root/osito-dos4gw-audio-config-20260905-6RXTQJ/` and
`/root/osito-dos4gw-audio-trace-20260905-mLN70P/`.
All temporary QEMU sessions and command helpers from this audio work have
exited through their own QMP sockets; user sessions :0/:1 were untouched.

Remaining audio work includes the virtual-IF compatibility model and OPL
emulation. The DMA model also still lacks automatic channel masking at
non-auto-init terminal count, described by Intel section 12.2.11; the new
probe explicitly masks the channel and does not establish that contract.
DMA window wrapping and broader live reprogramming are not implemented by
this change. GitNexus was unavailable; manual impact analysis covered the
DOS audio state machine, virtual port dispatch and native probe. No
executable-name special case or commit was added. `git diff --check` passes.

### 4.39 Software DPMI FLAGS and 16/32-bit interpreter contracts

An explicit `dosrun --emulate <file> [args]` session now prevents the
protected-mode native transfer. The default execution policy remains
native CPL3/IOPL0. The software profile exposes virtual IOPL3 and a FLAGS image
whose IF agrees with DPMI 0900h/0901h/0902h. PUSHF, POPF, IRET and the
interrupt/exception frames use that image; no guest IOPL is loaded into
host RFLAGS and port accesses remain mediated. The policy is per VM and
survives an EXEC child. This is an experimental compatibility profile,
not an automatic fallback or a complete 386 implementation.

`dpmi_flags.S` builds USE16 and USE32 probes. They cover nested word/dword
FLAGS saves, IF changes through POPFD without CLI, immutable virtual
IOPL3, DPMI flag APIs, a nested software interrupt with IF clear, and two
timer IRQs whose handlers acknowledge the PIC and use IRET without STI.
The original stack pointer, CF and DF must survive. Both probes return
2Ah. Native IOPL0 is intentionally not expected to offer these semantics.

Trying the original DOOM under this profile exposed interpreter defects
previously hidden by native execution:

- `FF /6` ignored operand size: PUSH r/m32 pushed two bytes, while POP
  removed four. DOS/4GW's USE16 vector-management routine then consumed
  its saved registers and far return at the wrong offsets. The captured
  return loaded CS=0, IP=011Fh around instruction 246,007.
- `FF` near CALL/JMP and INC/DEC, plus the compact INC/DEC register
  encodings, also ignored 32-bit operands. They now use the requested
  width, preserve CF for INC/DEC, evaluate PUSH/CALL sources before
  changing SP/ESP and reject invalid group-5/LOCK combinations via #UD.
- Accumulator-immediate ALU operations and TEST consumed only half of
  imm32. A FLAGS probe's `AND EAX,FFFFCDFFh` left `FF FF` in the stream;
  the old undocumented PUSH alias could conceal this as a spurious
  stack operation. The corrected probe explicitly checks stack balance.
  Accumulator forms share the existing ModR/M ALU helpers, and both TEST
  immediate encodings support the operand width.
- LOOPcc and JCXZ/JECXZ now select CX/ECX by address size independently
  of the branch operand size. The previous CX-only implementation made
  the software audio probe time out after about 447,000 instructions.
  It now completes all three transfers in about seven million.

These contracts follow Intel's
[instruction-set reference](https://cdrdv2-public.intel.com/774492/325383-sdm-vol-2abcd.pdf).
The in-OS regression adds 941 group-5/INC/DEC cases, 432 comparisons of
accumulator and ModR/M immediate forms, and 2,304 loop cases. Coverage
includes real mode, USE16/USE32, independent stack/operand/address sizes,
register and memory operands, high register preservation, CF/ZF behavior,
positive/negative branches and IP wrapping. The accumulator comparison
checks decoder consistency, not an independent oracle for every ALU flag.

Validation on the final kernel:

```sh
make -C arch/x86 CLANG=1 -j4 all dos-flags-test
# In the diagnostic guest:
dos-api-test
dosrun --emulate dpflags16.com
dosrun --emulate dpflags32.com
dosrun --emulate dpmi_audio.com
dosrun dpmi_audio.com
win32-module-test
dosrun --emulate DOOM.EXE -config audio.cfg -nomusic
dosrun DOOM.EXE -nosound
```

The full DOS API suite passes, including all 3,677 new CPU cases and the
102 DMA-wait cases. Both FLAGS probes and both audio execution profiles
return 2Ah; Win32 module-image checks pass 45/45. Audio success covers the
existing three-transfer IRQ/DMA probe, not DOOM's sound engine.
After the failed software run, the same boot runs DOOM natively with
`-nosound`: the attract demo renders, F10/Y delivers four keyboard bytes
and four IRQ1s, and the client exits zero back to the shell. The native
VGA mediator counts 48,910,992 memory faults during that run; this is not
a performance pass, and the existing DPMI 8001h cleanup warnings remain.

DOOM's software run no longer fails at that far return, but does not
reach gameplay or establish working audio. It logs a rejected allocation
of FFFF1000h bytes, then repeatedly traverses DOS/4GW and real-mode BIOS
services while displaying a garbled text row. Samples include
0117:0BBDh, F000:1C42h and 01F7:0025F5BFh. The 500-million-instruction
diagnostic budget stops it with -1 and returns to the shell. The cause
of the later corruption is not yet established. Remaining operand-size
gaps include XCHG accumulator forms, CBW/CWD versus CWDE/CDQ, and the
unary/multiply/divide group; more complete callback/exception FLAGS and
segment validation coverage is also needed. None is papered over with
an executable-specific rule, forced IRQ, binary patch or successful exit.

Kernel: 5,489,040 bytes, SHA-256
`6ebd412cd8606628c97d9f24be63056196cb9e216ed0fd4e81f1cbea4de3b4e8`.
Final diagnostic directory:
`/root/osito-dos4gw-loop32-20260905-Tn1tgB/`, fresh 128 MiB OsitoFS3
snapshot, KVM, 8 GiB/four CPUs, VNC :12, UDP 7790. The serial log and
`doom-software-stall.png` retain the failed software run. A 4 MiB guest
RAM sample is in ignored `arch/x86/build/dos4gw-loop32-sample.bin`.
`doom-native-final.png` records the subsequent demo and
`doom-native-exit.png` records the restored shell.
After shutting down the owned QEMU, `audio.wav` contains about 0.5133
seconds of nonzero stereo PCM at 48 kHz, with peak magnitude 16,384.
The earlier native render/exit-zero check is also retained in
`/root/osito-dos4gw-cpu-contract-20260905-2GZEsa/`. The EXE/WAD hashes
from section 4.33 remain unchanged. GitNexus was unavailable; manual
impact analysis covered the DOS interpreter, DPMI frames, execution
selector and tests. All owned QEMU/helper processes were stopped, no
user VM was touched, and `git diff --check` passes. No commit was made.

### 4.40 Integer widths and far-pointer loads: software DOOM demo and audio

The next software-profile run exposed additional operand-size gaps in
`cpu8086.c`. Group F7 NOT/NEG/MUL/IMUL/DIV/IDIV now supports both word and
dword operands, including EDX:EAX products/dividends and register-source
aliasing. DIV/IDIV check zero divisors and quotient overflow before
committing results; INT64_MIN/-1 is rejected before host C division.
The guest receives #DE with its original register pair and fault IP.
Compact XCHG now selects AX/EAX by operand size, and opcodes 98h/99h
implement CBW/CWDE and CWD/CDQ accordingly. Invalid LOCK forms receive #UD.

With those changes alone, DOOM no longer requests FFFF1000h bytes and
stalls in the earlier initialization loop. It loads the WAD, initializes
the refresh engine and reaches `I_StartupTimer()`, where DOS/4GW reports
exception 06h at 01F7:0023259Ah. The instruction is `LSS ESP,[EDX+8]`;
the corresponding restore uses `LSS ESP,[ESP]`. LSS was absent, and the
older LDS/LES handlers only loaded a 16-bit offset.

LDS/LES/LSS/LFS/LGS now share a 16/32-bit far-pointer load path. It reads
the complete source before changing an address register, DS or SS,
validates the source segment range (including expand-down bounds), and
validates destination selector type, privilege and presence. It handles
null data selectors, rejects null SS, reports #GP/#NP/#SS with the
appropriate selector error code, and updates the accessed bit in the
managed LDT, guest GDT or guest LDT. Register sources and LOCK receive
#UD. ModR/M retains the effective offset and source segment alongside
its unchanged existing translated address. The load rules follow
[Intel's instruction reference](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf).

The in-OS suite adds 33,588 cases:

- Group F7: 18,432 cases, including 2,070 #DE and 8,448 invalid LOCK
  cases. Valid arithmetic is compared with host CPU instructions;
  undefined flags are excluded, and invalid divisors never reach the
  host arithmetic oracle.
- Compact XCHG and sign extension: 756 cases, including locked forms.
- Far pointers: 14,400 cases, including 5,744 expected faults. Coverage
  includes real mode, USE16/USE32, independent operand/address widths,
  all eight destination registers, six source segments, ESP-based SIB
  addressing, invalid selectors, bounds, exception frames and accessed
  bits in all three descriptor-table arrangements.

All new cases and the full DOS API suite pass. Final-kernel checks:

```sh
make -C arch/x86 CLANG=1 -j4 all dos-flags-test
# In the diagnostic guest:
dos-api-test
dosrun --emulate dpflags16.com
dosrun --emulate dpflags32.com
dosrun --emulate dpmi_audio.com
dosrun dpmi_audio.com
win32-module-test
dosrun --emulate DOOM.EXE -config audio.cfg -nomusic
dosrun DOOM.EXE -nosound
```

Both FLAGS probes return 2Ah at 49,196/49,198 instructions. The software
audio probe returns 2Ah at 7,274,557 instructions, and the native probe
also returns 2Ah; both complete all three DMA/IRQ transfers. Win32
module-image checks pass 45/45. Native DOOM with `-nosound` renders the
attract demo and exits zero after F10/Y, with four keyboard bytes and
four IRQ1 deliveries. The native VGA mediator records 59,020,821 memory
faults, so this does not establish acceptable native rendering performance.

Software DOOM now reaches its title and moving attract demo and starts
Sound Blaster playback at 11,025 Hz, 8-bit stereo. Two manual runs reached
the 500-million-instruction diagnostic limit while playing; those are
not normal-exit passes. A subsequent smoke sends F10/Y shortly after
audio initialization, observes the confirmation menu, and exits zero at
121,995,818 instructions, with four keyboard bytes/four IRQ1 deliveries.
The shell resumes after the original ENDOOM screen. No executable patch,
forced interrupt, guest-memory edit or successful-exit substitution is used.

The isolated DOOM capture contains 91.5515 seconds of 48 kHz stereo PCM
across those three runs: 6,302,101 nonzero samples, peak magnitude 32,421,
RMS about 3,779.85. Its changing signal is distinct from the synthetic
DMA probe's constant tone. This establishes output from DOOM's sound
engine, not perceptual audio quality or working music (`-nomusic` was
used). The separate final probe capture is about 0.5129 seconds, peak
16,384, with 49,240 nonzero samples.

Final kernel: 5,505,464 bytes, SHA-256
`fb0b88920f3a01829682381c312731ec27372123d51a0a165146ea49b170f63a`.
Diagnostic artifacts use fresh 128 MiB OsitoFS3 snapshots, KVM, 8 GiB,
four CPUs, VNC :12 and UDP 7790:

- `/root/osito-dos4gw-arith32-20260905-fezYNi/` retains the intermediate
  arithmetic-only run and the LSS exception at 15,968,608 instructions.
- `/root/osito-dos4gw-farptr-20260906-m6o7d9/` retains the software demo,
  normal-exit serial evidence, `doom-software-farptr.png`,
  `doom-smoke-menu.png`, `doom-smoke-exit.png` and isolated `audio.wav`.
- `/root/osito-dos4gw-arith-final-20260906-wX7hH7/` retains the repeated
  full suite, FLAGS/audio probes, Win32 checks and native render/exit.

Remaining limitations are explicit: the fixed instruction cap still
ends a valid long-running software session; native CPL3/IOPL0 has not
gained the software profile's virtual POPF/IF semantics; the generic
paging translator and descriptor-cache model still need normalization.
This change does not retrofit all MOV/POP segment loads or claim general
386 conformance. Existing DPMI 8022h/8001h cleanup warnings remain.
GitNexus was unavailable; manual impact review covered the internal
ModR/M decoder, interpreter dispatch, local load helpers and DOS tests.
All owned VMs and helpers were stopped. No user VM or original image was
touched, `git diff --check` passes, and no commit was made.

### 4.41 Unbounded DOS sessions and explicit interpreter step quotas

The unconditional 500-million-instruction stop is removed. Normal DOS
sessions, including `--emulate`, no longer terminate because a debug
counter reaches that value. A bounded diagnostic run is now explicit:

```text
dosrun --emulate --max-steps 10000 DOOM.EXE -nosound
dosrun --max-steps=150 --emulate dosexec.com
dosrun --emulate DOOM.EXE -config audio.cfg -nomusic
```

`--max-steps` accepts a positive decimal uint64, requires `--emulate`,
rejects duplicate/overflowing/missing values, and supports both separated
and `=` syntax. Options end at the executable or `--`; subsequent tokens
remain the program's PSP command tail. Calls using the existing
`dos_run(filename, 0, NULL)` form remain valid. Zero is the internal
unlimited default, not an accepted explicit quota.

The quota counts interpreter dispatch steps, not architectural retired
instructions or elapsed time. An outer INT reserves its step before a
service can recurse. Count and exhaustion state belong to the VM session,
not the CPU snapshot, so EXEC children, DPMI real-mode calls and callback
unwinding cannot restart an exhausted session. A latched stop returns -1
through the existing cleanup path, with a signed diagnostic. A normal
exit or return sentinel reached on exactly the final step still succeeds.

Bounded sessions neither allocate a JIT cache nor dispatch compiled
blocks, and the native transition is also guarded. REP memory/string
operations use the existing 256-iteration restart mechanism in this mode;
previously it applied only to native MMIO single-step mediation. This
preserves remaining count/IP and conditional termination at chunk edges.
Unlimited REP/JIT behavior is unchanged. A step quota is not a wall-clock
watchdog for blocking host services or an idle guest awaiting an interrupt.

In-OS coverage adds 173 passing cases: 46 launch/parser cases and 127
runtime cases. Runtime checks include RM/USE16/USE32, exact exhaustion,
CPU restoration after a stop, UINT64_MAX without wraparound, sentinel and
normal-exit boundaries, HLT on the last step, REP stores/comparisons at
255/256/512/513 boundaries, nested DPMI FAR calls, and a valid hot JIT
block that executes only without the quota. A short seeded-counter test
also crosses the old 500M threshold in each CPU mode; it is separate from
the actual long-running DOOM evidence below.

Final regression commands and results:

```sh
make -C arch/x86 CLANG=1 -j4 all dos-exec-test
# In the diagnostic guest:
dos-api-test
dosrun --emulate dpflags16.com
dosrun --emulate dpflags32.com
dosrun --emulate dpmi_audio.com
dosrun dpmi_audio.com
win32-module-test
dosrun --emulate --max-steps=150 dosexec.com
dosrun dosexec.com
dosrun --emulate --max-steps=10000 DOOM.EXE -nosound
```

The full DOS API suite passes, both FLAGS probes return 42 at
49,196/49,198 instructions, and both audio profiles return 42 with all
three DMA transfers. The software audio probe takes 6,733,888 instructions
in this run. Win32 module-image checks pass. The 150-step run enters a
COM EXEC child, stops exactly at 150 and returns -1 to the shell; the
10,000-step DOOM run likewise stops exactly at its explicit limit. Native
`dosexec.com` passes the COM, MZ, DPMI and load-only child contracts and
exits zero at 1,626 interpreter instructions (native work is not counted).

The software EXEC suite is NOT an end-to-end pass: with no quota and with
a 1,000,000-step quota it reaches the same DPMI-child exit 72h, followed by
parent exit 49 at 1,007 instructions. Its `native_debug_handler` explicitly
increments the saved ICEBP return IP. In the interpreter this returns at
07B0h, inside the CMP starting at 07AFh, after ICEBP at 07AEh. The test and
native #DB contract need independent normalization; no guest binary was
patched and the failure was not converted to success. Bounded load-only
EXEC completion is not established by that failed software suite.

The unmodified original DOOM executable, launched with `--emulate`, the
Sound Blaster configuration and `-nomusic`, now runs past 1,000M
instructions. Captures at 600M and 1,000M show different positions and
game state in its attract demo, rather than an idle instruction loop.
F10 opens the original confirmation menu; Y exits zero at
**1,062,371,882 instructions**, with four keyboard bytes and four IRQ1
deliveries. ENDOOM is displayed and the shell prompt returns. The isolated
audio capture contains 88.8741 seconds of 48 kHz stereo PCM, 6,110,598
nonzero samples, peak magnitude 29,148 and RMS about 3,507.04. This is
evidence of game sound output, not an audio-quality or music claim.
The existing 8022h/8001h DPMI cleanup warnings remain visible.

Kernel: 5,513,840 bytes, SHA-256
`41b02f337bacc4ad6c28bd550a9f161decee83fc2307c162a77fbf8a5453dbc0`.
Artifacts are fresh 128 MiB OsitoFS3 snapshots, KVM, 8 GiB, four CPUs,
VNC :12 and UDP 7790:

- `/root/osito-dos4gw-quota-20260906-GCxdMZ/` retains the initial nine
  REP-test failures before diagnostic chunking was enabled.
- `/root/osito-dos4gw-quota-final-20260906-EMff44/` contains the passing
  full suite, boundary runs, native EXEC pass and software EXEC failure.
- `/root/osito-dos4gw-unbounded-20260906-W7g5Zt/` contains the actual
  long DOOM run, `doom-600M.png`, `doom-1000M.png`, the confirmation/exit
  captures and isolated `audio.wav`.

GitNexus was unavailable. Manual impact review covered launch parsing,
the VM policy fields, interpreter entry points, native/JIT transitions,
EXEC state restoration, shell help and test registration. All owned VMs
and helpers were stopped. User VMs/original images were not modified;
`git diff --check` passes and no commit was made.

### 4.42 ICEBP return-IP contract and EXEC regression normalization

The software EXEC failure in 4.41 was caused by its DPMI fixture, not by
the new step quota. `dpmi_native.S` incremented the saved return EIP in
its ICEBP handler. ICEBP generates a trap: the saved EIP already points
to the following instruction. This is distinct from fault-class #DB
conditions, which must retain the faulting instruction address.
[Intel SDM, volume 3A, Debug Exception](https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf).

The fixture now checks that return address against `native_debug_resume`
and fails with 7Ch on a mismatch, without modifying the frame or returning
to an incorrect IP. It still checks that the private INT FDh exception
return does not invoke the client's own FDh vector. This affects both
`dpmi_native.com` and the EXEC child `dexdpmi.com`; the production kernel
and original game binaries are unchanged in this follow-up.

`make -C arch/x86 dos-debug-test` adds independent USE16 and USE32 probes,
`dpdebug16.com` and `dpdebug32.com`. Each installs an old-style DPMI #DB
handler, checks the saved next IP, returns without adjusting it, and
checks the callback count and restored stack. The mismatch path retains
the observed/expected values and exits 71h before LRET, so a broken host
cannot make the test loop indefinitely. A successful probe exits 2Ah;
setup and other contract errors return 70h and 72h respectively.

Measured matrix, with the same kernel and no GDB or host breakpoints:

| Execution profile | USE32 ICEBP result |
|---|---|
| Osito software CPU on KVM 8.2.2 / 10.2.0 | Pass, next EIP 0199h |
| Native on KVM 8.2.2 / 10.2.0 | Fail 71h, observed 0198h instead of 0199h |
| Native on TCG 8.2.2 | #UD at 0198h, not a valid ICEBP reference |
| Native on TCG 10.2.0 | Pass, next EIP 0199h |
| Osito software CPU on TCG 8.2.2 / 10.2.0 | Pass (10.2.0 checked through the full EXEC fixture) |

The KVM mismatch was read from the probe's saved data through QMP physical
memory inspection, with its address derived from the serial-reported
VM backing address and the fixture's symbol table. Saved ESP, expected IP,
observed IP and successful-call count were FFFCh, 0199h, 0198h and zero.
USE16 passes too, but that profile remains interpreted and is not evidence
of native USE16 exception delivery.

This isolates a dependency on the KVM/WSL native execution path, not its
precise failing component. The host is AMD Ryzen 7 5800X under WSL kernel
`6.6.87.2-microsoft-standard-WSL2-udmabuf+`; QEMU 10.2.0 alone does not fix
the KVM result. No blanket `EIP++` was added to the kernel: it would corrupt
valid traps and fault-class debug exceptions. The native KVM failure stays
visible. Use `--emulate` for this contract on the current host; the native
TCG 10.2.0 result is a separate reference, not a performance recommendation.

After correcting the fixture, the complete EXEC suite passes in software
at 2,242 instructions, including COM/MZ/DPMI children, raw PM/RM callbacks,
overlay loading, handle/vector/memory restoration, and both load-only
children. Native TCG 10.2.0 passes at 1,626 interpreter instructions; that
count excludes native work. Native KVM now correctly exposes child exit
7Ch and parent exit 49 instead of hiding the bad return in the fixture.

```text
dosrun --emulate dosexec.com
dosrun --emulate --max-steps 1000000 dosexec.com
dosrun --emulate --max-steps 2242 dosexec.com
dosrun --emulate --max-steps 2241 dosexec.com
```

The first three return zero. The last exhausts exactly 2,241 steps and
returns -1, even though the fixture printed its PASS message immediately
before the final DOS termination call. Exit status, not just a PASS line,
is required to judge a quota run. Bounded load-only EXEC completion is now
established, superseding that open item in 4.41.

The complete DOS API suite passes again, including the 46 option and 127
step cases; Win32 module-image checks remain 45/45. A real DOOM software
run on KVM with QEMU 10.2.0 shows coherent attract-demo graphics, opens its
original F10 confirmation menu, and exits zero on Y at 1,120,174,635
instructions. Four keyboard bytes produce four IRQ1 deliveries. ENDOOM
and the returned shell are captured; the 8022h/8001h cleanup warnings remain.
The automatic screenshot helper failed because this local QEMU build has
no PNG support. Manual QMP PPM captures, converted with `pnmtopng`, completed
the verification and menu-driven exit. Its `audio.wav` also contains the
earlier DOS API audio tests, so it is not an isolated DOOM audio benchmark.

Build: `make -C arch/x86 CLANG=1 -j4 all dos-debug-test dos-exec-test dos-dpmi-test`.
The kernel hash is unchanged from 4.41. Final `dexdpmi.com` SHA-256:
`6c21c1e92a01f3d530fd11d1c6083a03926981b30cedd39d01e802ba2512094a`.
Fresh 128 MiB OsitoFS3 snapshots, 8 GiB, four CPUs, VNC :12, UDP 7790:

- `/root/osito-dos4gw-icebp-kvm-20260906-HIM4Ip/`: initial 8.2.2 KVM mismatch.
- `/root/osito-dos4gw-icebp-tcg-20260906-Y7bEJt/`: 8.2.2 TCG #UD and software pass.
- `/root/osito-dos4gw-icebp-tcg10-20260906-JaBcSk/`: independent native pass and
  the old fixture's failing extra increment under TCG 10.2.0.
- `/root/osito-dos4gw-icebp-kvm10-20260906-lySs3E/`: KVM mismatch, corrected
  software EXEC/quota passes, API regressions, DOOM demo/menu/exit captures.
- `/root/osito-dos4gw-icebp-final-tcg10-20260906-QhveM9/`: corrected native
  EXEC and software bounded EXEC passes under TCG 10.2.0.

GitNexus was unavailable; manual impact review covered fixture entry and
handler labels, Makefile targets and EXEC's DPMI child dependency. All
owned VMs/helpers were stopped, user VMs and original images were untouched,
and no commit was made. Native IOPL0 FLAGS limitations, REP port-I/O
coverage, performance and broader DOS/Win32/x64 compatibility remain open.

### 4.43 DOS string port I/O and restartable native mediation

The software CPU previously executed exactly one transfer for INS/OUTS,
ignoring REP (even count zero), and always used 16 bits for the wide
opcodes. Native IOPL0 mediation handled scalar IN/OUT but not string port
instructions. The scalar software opcodes also ignored 32-bit operands.

The interpreter now implements byte/word/dword transfers, independent
operand/address sizes, DF, CX/ECX, preserved high register halves for
16-bit addressing, source overrides for OUTS, and fixed ES destination
for INS. These size/index rules follow Intel's
[INS](https://pdos.csail.mit.edu/6.828/2008/readings/i386/INS.htm) and
[OUTS](https://pdos.csail.mit.edu/6.828/2008/readings/i386/OUTS.htm)
instruction descriptions. LOCK raises a guest #UD before any port effect.
F2 and F3 follow the existing software CPU's repeat-prefix model; F3 is
the prefix used by the native fixture.

Each string I/O dispatch performs at most 256 transfers. It decrements
the architectural count only after a completed transfer and retains the
original prefixed EIP while repetitions remain. There is no temporary
truncation of ECX: a segment fault after two transfers from count 513
exposes 511, not 254, to the handler. Zero count does not access a segment
or a device. Input validates the complete destination span before reading
the port. Segment presence, access rights, limits and expand-down ranges
reuse a shared address helper with far-pointer loads. Both paths continue
to use the existing DPMI paging translator.

Native #GP mediation recognizes opcodes 6Ch-6Fh and executes a bounded
interpreter dispatch, using the actual saved general and segment registers.
The shared data-step helper also serves native VGA MMIO faults. It copies
back CS/SS as well as registers and flags when an instruction delivers a
DPMI exception, synchronizing the LDT if delivery introduces a host return
selector. This is a decoded data-operation path, not an arbitrary native
instruction fallback.

Measured baseline: the first 6,048 string I/O cases failed 4,704 times,
while the existing DOS API suite passed. After correction:

| In-kernel test group | Checks | Failures |
|---|---:|---:|
| String I/O widths, REP, count zero, direction, sizes and overrides | 6,048 | 0 |
| Port segment boundaries, permissions, locks and step quotas | 630 | 0 |
| Scalar port widths, immediate/DX, locks and ignored prefixes | 72 | 0 |
| Existing far-pointer instructions | 14,400 | 0 |
| Existing step-limit contracts | 127 | 0 |
| Existing VGA contracts | 328,923 | 0 |

The boundary group deliberately delivers 358 guest exceptions. The other
existing DOS API checks, audio DMA and keyboard tests also pass. Tests use
the virtual VGA DAC and DMA page register as observable device state, not
a new test-only I/O callback. Win32 module-image regression remains 45/45.

`make -C arch/x86 dos-string-io-test` builds `dpstr16.com` and `dpstr32.com`.
The fixture checks 257-element transfers of all three widths with both
address sizes and DF values, including preserved CF and upper register
halves. The USE32 client also reduces ES's limit: two dwords fit, the third
delivers #GP, and the handler verifies count/index/error/EIP, enlarges the
segment, and LRETs without changing the saved IP. Execution finishes the
remaining transfer and restores the original stack. USE16 remains an
interpreted profile, not evidence of native USE16 execution.

```text
dosrun --emulate dpstr16.com   # exit 42, 16,066 interpreter dispatches
dosrun --emulate dpstr32.com   # exit 42, 16,142 interpreter dispatches
dosrun dpstr32.com            # native USE32, exit 42
dosrun dpvgamem.com           # native VGA, exit 0, 26 mediated memory faults
win32-module-test             # 45 checks, zero failures
dosrun --emulate DOOM.EXE -config audio.cfg -nomusic
```

On QEMU 8.2.2/KVM, DOOM renders coherent attract-demo graphics, accepts
F10/Y in its original confirmation menu and exits zero at 674,153,002
instructions. Four host keyboard bytes cause four reads and four IRQ1
deliveries. `doom-600M.png`, `doom-smoke-menu.png` and
`doom-smoke-exit.png` retain the captures. The finalized 48 kHz stereo
`audio.wav` has 46.2844375 seconds, 3,278,542 nonzero samples, peak 27,829
and RMS 3,896.34. Earlier probes in that VM did not start an audio stream;
the signal belongs to this DOOM run, with its preceding silence. This
checks digital audio activity, not sample-perfect hardware equivalence.

Build: `make -C arch/x86 CLANG=1 -j4 all dos-string-io-test`.
Fresh 128 MiB OsitoFS3 snapshots, 8 GiB, four CPUs, VNC :12, UDP 7790:

- `/root/osito-dos4gw-stringio-before-20260906-qWQqmt/`: failing baseline.
- `/root/osito-dos4gw-stringio-after-20260906-WcgZyj/`: full passing API suite.
- `/root/osito-dos4gw-stringio-native-20260906-vkSFqD/`: software/native
  fixtures, native VGA, Win32 module regression, DOOM graphics/audio/exit.
- `/root/osito-dos4gw-stringio-final-20260906-GD9xig/`: final API regression
  after extending the partial-fault case to count 513; native USE32 and
  Win32 module checks also pass again on this final kernel.

The native/DOOM kernel SHA-256 was
`53b99d04a8c336cc096b1b50356da1c3a948a62f83004b6b985cb82ebfa1f7fd`.
The final kernel differs only in that test and comments; its SHA-256 is
`50ea840eed1a9e8172ceb40f2c8bb3bdff5671c0c9bb296f5decd5bed52c1e3a`.
`dpstr16.com` SHA-256 is
`6af6fb306575ee1f8aaf59ff776059d9865ea9b5c37435dfcb0752cbc4f1a82c`;
`dpstr32.com` SHA-256 is
`4408b45e8a37c66b20b44792a6ed734ff283f88ef53cf920e4376933f02783bd`.

Remaining boundaries are explicit: the shared guest pager still lacks
complete page-fault/permission/A-D semantics and can wrap translations
into RAM; guest TSS I/O-permission checks and descriptor hidden caches are
not implemented by this change. No port access is passed through to raw
host hardware. Native IOPL0 FLAGS restrictions and the KVM/WSL ICEBP
mismatch from 4.42 remain. DOOM's 8022h/8001h DPMI cleanup warnings, music,
performance, and broader DOS/Win32/x64 coverage are still open.

GitNexus was unavailable. Manual impact review covered the interpreter,
far-pointer reads, native #GP/VGA dispatch, DOS API tests and fixture build
targets. No original game binary or user disk image was modified.
All owned diagnostic VMs and helpers were stopped. No commit was made.

### 4.44 DPMI default exception vectors and DOS4GW shutdown

The 8022h/8001h shutdown cascade was not a request for additional INT 31h
functions. A temporary trace captured the first failing call:

```text
func=0203 error=8022 BX=0100 CX=0000 EDX=00000000 caller=0087:00007640
```

DOS4GW had saved the addresses returned by 0202h. The host returned a null
selector for every unhooked exception, then correctly rejected those same
values in 0203h. The client's restore loop loads AX=0203h once and increments
BL, so the first error became the next function number. This is distinct
from its later attempts to free DOS-memory descriptors through 0001h, which
remain rejected. The initial hypothesis about the 0.9 versus 1.0 error ABI
was not used as a workaround: global error codes and CF behavior are unchanged.

0202h now returns a valid immutable host code selector and a per-exception
entry point. 0203h can restore that pointer without accepting null/data
selectors. USE16 consumes/returns DX while preserving EDX's upper half;
USE32 uses the complete offset. These are the contracts documented by
[Get Processor Exception Handler](https://www.delorie.com/djgpp/doc/dpmi/api/310202.html)
and [Set Processor Exception Handler](https://www.delorie.com/djgpp/doc/dpmi/api/310203.html).

The new ROM stubs use the existing private F9 gate, distinguished by their
host selector and exact entry offset. Ordinary client INT F9 remains a
software interrupt. A handler may tail-chain to a saved default address;
the host validates the complete DPMI frame and original FAR return target.
Exceptions 0, 1, 2, 3, 4, 5 and 7 reflect to real mode, then RETF through
the existing exception-return path. Exceptions 6 and 8-31 terminate the
client with the original fault location in the serial diagnostic, following
the [DPMI default exception policy](https://www.delorie.com/djgpp/doc/dpmi/ch4.5.html).
Unhooked real-mode INT 0 terminates with a divide-error diagnostic instead
of returning to the same DIV indefinitely. Application-installed real-mode
handlers remain callable. No game binary, name or instruction address is
special-cased.

`dos-api-test` passes, including the new `[DOS-EXDEFAULT]` group:

| Cases | Checks | Failures |
|---|---:|---:|
| 32 vectors, USE16/USE32, both stack sizes, implicit/restored/chained defaults | 384 | 0 |
| Invalid exception numbers in get/set | 896 | 0 |
| Absent/truncated/unwritable/malformed frames and invalid context | 32 | 0 |
| Unhandled divide error without a real-mode hook | 4 | 0 |

The existing vector-isolation test now explicitly selects USE32 for its
32-bit offsets; the new matrix separately verifies USE16's high-half
preservation. Existing descriptor, opcode, I/O, keyboard, VGA and audio
tests remain passing. `win32-module-test` also passes 45/45, and native
`dpstr32.com` still exits 42 after its restartable partial-transfer fault.

`make -C arch/x86 dos-exception-test` builds `dpex16.com`, `dpex32.com`,
`dpxf16.com` and `dpxf32.com`. The first two save/replace/restore all 32
exception vectors and tail-chain #DB through a real-mode handler. The
client handler deliberately redirects saved IP to a continuation, exercising
the editable DPMI frame; this does not retest or fix the KVM ICEBP return-IP
mismatch in 4.42. Both software clients and native USE32 exit 42. The fatal
variants chain a mediated port-I/O #GP to its default handler and exit -1
(FF in the native serial message), returning control to the shell. Native
generic delivery of every CPU exception is not established by these tests.

The earlier kernel fails the independent vector fixture with exit 113 in
USE16 and native USE32. After correction, DOOM's attract demo renders and
accepts F10/Y, exiting zero at 659,014,367 interpreter dispatches without
the 8022h/8001h cascade. The captures in the `after` run below include
`doom-600M.png`, the confirmation menu and the returned DOS screen.
The final kernel repeats this result: exit zero at 657,179,179 dispatches,
four host keyboard bytes, four data reads and four IRQ1 deliveries. Its
serial log has no 8022h/8001h call cascade; `doom-600M.png`,
`doom-smoke-menu.png` and `doom-smoke-exit.png` record rendering, the exit
confirmation and the returned shell.

Build: `make -C arch/x86 CLANG=1 -j4 all dos-exception-test`.
All runs use fresh 128 MiB OsitoFS3 snapshots, 8 GiB, four CPUs, VNC :12,
UDP 7790 and QEMU 8.2.2/KVM, leaving original images and user VMs untouched:

- `/root/osito-dos4gw-dpmi-calltrace-20260906-IWB1au/`: temporary diagnostic
  kernel proving the original 0203h/null-selector failure; trace code removed.
- `/root/osito-dos4gw-exvectors-before-20260906-2WVQ7S/`: failing vector fixtures
  on the final 4.43 kernel.
- `/root/osito-dos4gw-exvectors-after-20260906-yFt7iN/`: initial successful
  fixtures and DOOM shutdown, before correcting the USE32 test setup and
  adding the malformed-frame/divide-default tests.
- `/root/osito-dos4gw-exvectors-final-20260906-JPWx7Q/`: final 1,316-case group,
  full DOS API pass, software/native chain and fatal fixtures, port-I/O and
  Win32 module regressions, plus the final DOOM rendering/exit smoke test.

Final kernel SHA-256:
`90dbfde0a395a65c2928581c70bb5729a27184a3913836815f123eb26fdf26b5`.
Fixture SHA-256 values:

```text
dpex16  b71625b824f51d964f82ec3541c470e3dfe5f30b81db7409c4cd41f0b2cc5437
dpex32  a703df9d10c7e4016fdfee1420a3739bb3c41355949d9f11415c4d9ede8af062
dpxf16  8b12c0e7bb877de6c6ed2d11136e90921e81bd8118ad572a797e6e13c3e80873
dpxf32  f9506469e623a5fa274db4c3a4c4ca71270e397e83134589fa234815825876ed
```

GitNexus was unavailable. Manual impact review covered DPMI queries,
default interrupts, exception delivery/return in both backends and tests.
The 4.43 paging/TSS/FLAGS boundaries, broader native exception coverage,
music and performance remain open. No complete DOS/Win32/x64 compatibility
claim is made. All owned diagnostic VMs and helpers were stopped; no commit
was made in this follow-up.

### 4.45 Native DPMI exception delivery

Installed DPMI exception handlers were bypassed when the native CPU, rather
than an emulated instruction, raised a fault. The independent USE32 fixture
reproduced this with #DE on the final 4.44 kernel: the kernel printed its
generic exception dump and recovered to the shell without invoking the
client's installed handler.

`dos_native_handle_exception()` now bridges remaining client faults after
the existing selector, privileged-instruction and VGA mediation paths. It
requires the active native VM's CR3, CPL3, a valid code descriptor and no
active native DOS service dispatch. System events such as NMI, double fault
and machine check stay on the host path. Unsupported host-induced VGA traps
are not misreported as application page faults.

The bridge preserves the register context, captures CR2 before calling
helpers, delivers through the existing DPMI exception machinery, synchronizes
the LDT and exports the modified return frame. Client handlers use the
editable 0.9 FAR-return frame; host-transparent faults are serviced first,
as described by the [DPMI exception contract](https://www.delorie.com/djgpp/doc/dpmi/ch4.5.html)
and [0203h handler API](https://www.delorie.com/djgpp/doc/dpmi/api/310203.html).
No executable name, binary patch or instruction-address special case is used.

The DPL0 host gates mediate native INT3 and INTO through #GP. Recognized
trap opcodes, including valid prefixes, now deliver #BP/#OF with the next
instruction pointer; `CD 03` and `CD 04` remain software interrupts. The
synthesized traps also clear RF introduced by that host #GP. Real CPU
faults retain RF, matching the [Intel 386 RF/trap rules](https://pdos.csail.mit.edu/6.828/2008/readings/i386/s12_03.htm).
The prefix and RF assertions each failed before their corresponding fix.

`make -C arch/x86 CLANG=1 dos-exception-test` builds four additional COM
fixtures from `test/dpmi_native_faults.S`. All execute USE32 code:

| Fixture | Stack | Expected native exit |
|---|---|---|
| `dpfault.com` | SS16 | 42 |
| `dpfalt32.com` | SS32 | 42 |
| `dpff16.com` | SS16 | FF after restoring the default #UD handler |
| `dpff32.com` | SS32 | FF after restoring the default #UD handler |

Each variant checks 13 handled deliveries: #DE, #UD, #GP, #NP, #SS,
plain/prefixed INT3 and INTO, BOUND, #PF and nested #DE/#UD. Checks include
vector/error/IP, saved CS/SS/ESP, GPRs, data selectors, arithmetic flags,
DF, RF, handler-edited return IP/CF and nested stack restoration. OF-clear
INTO is a no-op, and two explicit software interrupts have separate handlers.
Both fatal variants diagnose the unhandled #UD and return to the shell.

Coverage correction: the original SS16 variant did not clear the initial
stack descriptor's B bit. Section 4.47 explicitly selects and tests both
stack widths; its fourteen-delivery fixture supersedes that earlier claim.

The #SS case attempts to load a non-present replacement selector while the
old stack remains valid. It does not establish recovery from an exhausted
or invalid current stack. A complete locked DPMI exception stack is still
missing. Native BIOS INT 10h still owns vector 16, colliding with x87 #MF;
the bridge does not resolve that gate ownership. Broader #NM/#MF/#AC/#XM
coverage, virtual IF/POPF, the KVM ICEBP mismatch, paging/TSS semantics,
music and performance remain open.

### 4.46 PE32 SEH abandonment must restore IST3

Running the unchanged SEH3 fixture and then `dos-api-test` exposed a
cross-layer regression: `[DOS-HOSTMEM]` failed its existing IST3 reset
assertion. This was not a test assumption to remove. In the isolated run,
the shell's private stack top was `FFFF800001D68000`, but the saved cursor
was `FFFF800001D5FEA0`, two abandoned `isr_common` frames below the top.
The EH3 nonlocal transfer restored INT2E/IST1 ownership but never retired
the hardware fault frames associated with the abandoned callbacks.

Callback state now also captures IST3. On abandonment, the INT2E completion
path restores the snapshot of the outer callback that remains live, or
resets the current task's private fault stack when returning to the primary
PE32 stack. This is not an unconditional reset during nested exception
handling: the surviving outer ISR must still return through its own frame.
The DOS host-memory assertion is unchanged.

The extended `seh3_pe32.c` repeats eight main-stack access violations and
eight more inside a vectored BOUND handler, with `Sleep(1)` between nested
deliveries. The outer context and continuation remain intact. Existing
EH3 filter/finally, software exceptions, VEH order, BOUND recovery and the
independent EH4 fixture still exit zero. Running the full DOS API test
afterward passes, including all 97 host-memory checks. This does not audit
every possible Win32 process-exit or nonlocal-transfer path.

The final mixed-session run also passes all four native exception variants
above, native `dpstr32.com` and `dpex32.com` (42), `dpvgamem.com` (0, 26
mediated VGA accesses), and `win32-module-test` (45/45).

DOOM was then exercised in both backends in that same VM:

```text
dosrun --emulate DOOM.EXE -config audio.cfg -nomusic
dosrun DOOM.EXE -nosound
```

Both show coherent attract-demo graphics, display the original F10 exit
confirmation and return to the shell with code zero after Y. The software
run exits at 664,044,075 dispatches; the native run reports 45,729,650
mediated VGA memory faults. Each run records four host keyboard bytes,
four data reads and four IRQ1 deliveries, without the 8022h/8001h shutdown
cascade. The final native smoke test does not validate sound. Its trap
count identifies substantial VGA mediation work, not a measured CPU-time
breakdown or a performance improvement.

The finalized software-audio recording is 48 kHz stereo, 53.36277 seconds,
with 3,893,911 nonzero samples, peak 32,524 and RMS 3,923.71. The serial log
contains one audio-source registration, during software DOOM. This verifies
PCM activity, not audio fidelity or music. `doom-600M.png`,
`doom-smoke-menu.png`, `doom-smoke-exit.png` and the `doom-native-*.png`
captures retain the graphics and exit evidence. SEH4 exits zero once more
after native DOOM, checking the reverse DOS-to-Win32 transition as well.

Build commands:

```sh
make -C arch/x86 CLANG=1 -j4 all dos-exception-test
sh arch/x86/scripts/build-seh3-test.sh
sh arch/x86/scripts/build-seh4-test.sh
```

Diagnostics use separate 128 MiB OsitoFS3 snapshots, 8 GiB, four CPUs,
QEMU 8.2.2/KVM, VNC :12 and UDP 7790:

- `/root/osito-dos4gw-nativefault-before-20260906-Exj915/`: native #DE baseline.
- `/root/osito-dos4gw-nativefault-prefix-20260906-Anharg/`: prefix regression.
- `/root/osito-dos4gw-nativefault-rf-before-20260906-WnI0ke/`: RF regression.
- `/root/osito-dos4gw-ist3-order-20260906-nYhFd1/`: isolated SEH3/IST3 failure.
- `/root/osito-dos4gw-ist3-fix-20260906-smaLk8/`: final mixed-session regressions.

Final kernel SHA-256:
`c94f6c10692be35d82b818d0218f2c3b80fcf2007a5013ad6a00efc98abc9df5`.
Extended SEH3 executable SHA-256:
`52086764d8c7ae1160f262a7509b4ac6035390f29cd3a991cb151744da6c7364`.
GitNexus was unavailable; manual impact review covered the native frame
bridge, IDT ordering, shared PE32 callbacks, scheduler cursor restoration,
fixture targets and regressions. Original user images and VMs are untouched.
All owned diagnostic VMs and helpers were stopped. No commit was made.

### 4.47 Resident DPMI exception stack

Exception delivery previously pushed its frame onto the application's
current stack. A handler could therefore be installed and still fail to
recover when the stack itself was exhausted. DPMI specifies a host-owned
resident stack of at least 4 KiB, preserving the current stack during
nested delivery and restoring the original stack on return. See
[stack switching](https://www.delorie.com/djgpp/doc/dpmi/ch4.3.html).

The host now reserves a separate 4 KiB exception stack after the existing
64 KiB callback area. Both areas are excluded from the extended-memory
allocator; the exception descriptor is reserved during transactional DPMI
entry, before the application can exhaust the LDT. Allocation failure
rolls back the client and host descriptors and reports 8011h. The stack
descriptor supports queries/aliases but not modification or release,
following the [descriptor rules](https://www.delorie.com/djgpp/doc/dpmi/descriptor-rules.html).

The first exception switches SS:ESP to that resident stack. Nested
exceptions retain the current handler stack, including a client-selected
replacement or an active callback stack. The existing 16/32-bit 0203h FAR
frame preserves the original SS:SP/ESP; the handler may repair it before
returning, as required by the [exception frame contract](https://www.delorie.com/djgpp/doc/dpmi/ch4.5.html).
Both delivery and return validate the complete frame's data permissions,
presence, privilege, segment bounds, expand-down bounds and resident RAM
span before modifying the CPU or stack. Invalid frames cannot wrap through
the legacy address translator. Failed DPMI delivery does not fall through
to an unrelated raw-IDT frame layout.

`dos-api-test` adds 76 stack checks: 16/32-bit clients and stacks, an
exhausted application stack left untouched, edited return SS:SP/ESP/IP,
nested and callback-active delivery, expand-down stacks, rejected incomplete
or inaccessible frames, overflow and atomic failure. Entry tests also
cover immutable stack ownership, LDT exhaustion and rollback after each
partial allocation. The interrupt/entry fixtures now allocate 2 MiB to
include real host stack storage beyond conventional memory. Existing
instruction tests inspect SS-relative frames rather than assuming base zero.

The native fixture explicitly programs SS.B for both variants. Each now
checks fourteen deliveries, including a real #SS from pushing a dword at
ESP=2. The handler verifies the original exhausted pointer, uses a different
SS, repairs the saved ESP, and resumes successfully. Nested #UD retains
the outer handler's SS and restores its exact ESP. No executable-specific
patch or bypass is used.

This is not yet the complete shared DPMI stack lifecycle: hardware IRQs,
INT 1Ch/23h/24h and real-mode callbacks still need unified ownership across
nested mode switches. General guest paging, hidden descriptor state and
validation of edited return destinations also remain separate gaps.

### 4.48 Preserve full ESP on native SS16 returns

Explicitly clearing SS.B in the native fixture exposed a second, shared
return-path defect. The SS16 case passed in a fresh session but failed its
saved-SP assertion after PE32 SEH3 or SEH4. The diagnostic run recorded
`0097:03AFFFFC` at #DE while the fixture's saved/expected ESP was
`01D6FFFC`. The changing high word came from the kernel interrupt stack,
not from DOS stack arithmetic. The SS32 variant still passed.

IRET to a 16-bit stack restores only the low 16 bits of the stack pointer.
The established alias-stack approach is described in the primary
[Linux espfix implementation](https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/kernel/espfix_64.c).
Osito now has a common `x86_compat_iret` tail for kernel interrupt returns,
DOS service returns, INT 2Eh and initial native DOS entry. It examines SS.B
and CS.L, copying the frame only for compatibility-mode SS16 returns.

Each xAPIC CPU has a 64-byte slot containing three saved scratch registers
and the five-qword IRET frame. The tail writes through its ordinary kernel
mapping, then chooses a read-only alias whose address contains the requested
ESP[31:16]. It performs no allocation, C call, SIMD operation or user-stack
write. Saved GPRs and the hardware return flags remain intact. Kernel,
SS32 and native 64-bit returns do not use an alias.

Three static page-table pages and four slot pages cost 28 KiB total. The
aliases occupy the reserved PML4[510] region, outside the physical direct
map, and are present in process CR3s as supervisor-only, read-only, NX
mappings. Unused subranges are unmapped. Osito's existing NMI and fault
gates already use IST stacks; a nested kernel return does not overwrite a
pending CPU slot. This does not add Linux's separate double-fault fixup
machinery or claim to validate every invalid client return destination.

The DOS service dispatcher also exports the restored full `cpu->esp` when
rewriting IRET. Merging its low word with the current handler frame was
incorrect after a stack switch: those high bits belonged to the handler,
not the restored application stack.

The SS16 native fixture now seeds ESP's high word with `A5A5h`, compares the
full saved pointer and exercises an exhausted stack plus nested exceptions.
Diagnostic exit stages distinguish register, segment, flag and frame-field
failures. In the mixed-session regression, SS16 exits 42 before and after
SEH3, SS32 exits 42 after SEH4, and both restored-default #UD variants exit
FF with a diagnostic and a normal return to the shell. SEH3 and SEH4 exit
zero. The full DOS API suite passes afterward, including 3,074 alias and
mapping-permission checks, 76 exception-stack checks and 97 host-memory
checks. CR3 isolation passes 13/13 and Win32 module images pass 45/45.
`dpex16/32.com` and `dpstr16/32.com` exit 42; native `dpvgamem.com` exits
zero after 26 mediated VGA accesses. SS16 still exits 42 after that sequence.

The same kernel/session then ran the original DOOM executable in both modes:

```text
dosrun --emulate DOOM.EXE -config audio.cfg -nomusic
dosrun DOOM.EXE -nosound
```

Both show coherent attract-demo graphics and the original F10 confirmation,
then return to the shell with exit zero after Y. The software run completed
665,141,800 interpreter dispatches. The native run reported 51,990,816
mediated VGA memory faults. Both recorded four host keyboard bytes, four
data reads and four IRQ1 deliveries. This trap count is not a performance
measurement or an improvement claim. The native smoke does not test audio.
SEH4 exits zero again after native DOOM, followed by SS16 exit 42.

The finalized software PCM recording is 48 kHz stereo, 45.90860 seconds,
with 3,346,167 nonzero samples, peak 32,505 and RMS 4,180.73. The serial log
contains a single audio-source registration, during software DOOM. This
checks signal activity only, not audio fidelity or music. Inspected captures
include `doom-600M.png`, `doom-smoke-menu.png`, `doom-native-demo.png` and
`doom-native-menu.png`; exit and final-shell captures are also retained.

Build and whitespace checks passed:

```sh
make -C arch/x86 CLANG=1 -j4 all dos-exception-test
git diff --check
```

The final diagnostic run is
`/root/osito-dos4gw-espfix-20260906-J75hfG/`, using a separate 128 MiB
OsitoFS3 snapshot, QEMU/KVM, 8 GiB and four CPUs on VNC :12 / UDP 7790.
The SS16 failure-stage and saved-SP diagnostic runs are respectively
`/root/osito-dos4gw-exstack-ss16-20260906-McuXtw/` and
`/root/osito-dos4gw-exstack-spdiag-20260906-fClPJb/`.

Final kernel SHA-256:
`ed3732f6e90abe193846193a9cb31278557e798a2a0b8c407d1f02f0dfb1f633`.
The booted copy and workspace build match. Final native fixture hashes:

```text
dpfault   ac1dfc8ba9d231b0ed58e3795006917c9997b583dfbdbf432010692555ea41b3
dpfalt32  cb0dfd8b9ec421b88c16d0cf9c318b411c2f003eb989b5bd63df91c6447955aa
dpff16    690994e7b0bc4149f0794fb8d3e86b717a9e2abed33635b19ad0c5921b08bb4c
dpff32    eb2b9e04168409cd358fa102bc156b868413007e84117f2a5065d1296c617a62
```

GitNexus was unavailable. Manual impact analysis covered DPMI allocation,
exception entry/return, CPU stack helpers, native frame export, all shared
IRET tails, paging initialization/process clones, both kernel build lists
and the regression fixtures. This follow-up uses no binary-specific patch.
All owned diagnostic VMs and helpers are stopped; user images and VMs were
not modified. No commit was made. Unified IRQ/callback stack ownership,
edited-return validation, general guest paging, x87 gate ownership, virtual
FLAGS, music and performance remain open.

### 4.49 Shared DPMI locked-stack ownership

Protected hardware IRQs, protected INT 1Ch/23h/24h, translated Ctrl-C and
critical errors, real-mode callbacks and exceptions now use the same
resident-stack selection. This follows the DPMI
[stack-switching contract](https://www.delorie.com/djgpp/doc/dpmi/ch4.3.html):
the first event selects the host stack, while nested events retain the
current stack, including a client-selected replacement. The old 64 KiB
callback pool is removed; only the shared 4 KiB stack remains reserved
outside the extended-memory allocator. This supersedes the separate-pool
layout described in 4.47.

Selection validates the complete required span before switching SS:ESP.
It respects SS.B independently of client width, preserves the high word
of ESP for SS16, and shares the existing permission, expand-down and RAM
bounds checks. Ordinary INT 31h/0300h-0302h simulations park the active PM
cursor while real-mode code executes, restoring the previous cursor and
virtual interrupt state when the call unwinds. Nested callbacks therefore
continue below their suspended caller instead of resetting a private stack.

Protected interrupt delivery saves the original SS:ESP and CS:EIP in
host-owned LIFO metadata. The client gets an ordinary IRET frame to a
distinct host return stub at F000:0168, not an exception frame. Return
checks the balanced handler SS:ESP and restores the original full pointer.
Native return consumes the kernel IST frame; interpreted return also
discards the interpreter's private INT frame. Hardware IRQs disable virtual
interrupts on delivery; software INT 1Ch/23h/24h preserve the incoming
virtual IF, as required by the
[software interrupt rules](https://www.delorie.com/djgpp/doc/dpmi/ch4.4.3.html).
Native physical FLAGS virtualization is still a separate limitation.

Ctrl-C and critical-error delivery track their own nesting instead of
borrowing callback slots. Their existing normal/error response policy is
unchanged, with balanced return-stack validation. Callback entry preserves
the prior virtual IF for return; its register-structure and IRET interface
remain the standard [callback interface](https://www.delorie.com/djgpp/doc/dpmi/ch4.6.html).

`dos-api-test` adds 80 checks spanning client widths, SS16/SS32 and all four
interrupt kinds: exhausted application-stack canaries, nested delivery on
a replacement stack, exact outer restoration, virtual IF preservation,
parked real-mode cursors and rejection of depleted parked stacks. The
full suite passes, including the existing 76 exception-stack checks,
3,074 compatibility-return checks and 97 host-memory checks.

New `dpmi_locked_stack.S` builds as `dplock16.com` and `dplock32.com` via
`dos-exception-test`. Starting with application ESP=2, each performs two
INT 1Ch -> INT 60h -> real-mode callback -> #UD -> return sequences and
checks stack identity, descending nesting and exact restoration. Both
identical final binaries fail with exit 128 on the previous 4.48 kernel
and exit 42 on the new kernel. USE32 passes with native entry and with
`--emulate`; USE16 uses the interpreter. The callback reached through the
simulated real-mode call is also interpreted, even in the native-entry run.

Additional final-kernel regressions in the same QEMU session:

- `dpcrit16/32`, `dpbreak16/32`, and noninteractive `dpcon16/32`: PASS, exit 0.
- PE32 SEH3/SEH4: exit 0; native SS16/SS32 exception fixtures: exit 42.
- Restored-default fatal #UD fixtures: diagnostic, exit FF and shell return.
- DOOM software, `-config audio.cfg -nomusic`: coherent attract demo,
  F10 confirmation and Y exit 0 after 667,664,939 interpreter dispatches.
- DOOM native, `-nosound`: coherent moving demo, F10/Y exit 0;
  35,701,227 mediated VGA memory faults. This count is not a speed claim.
- Both DOOM runs: four keyboard bytes, reads and IRQ1 deliveries.
- After native DOOM: SEH4 exits 0, then SS16 and `dplock32` each exit 42.

The software PCM capture is 48 kHz stereo, 45.50077 seconds, with 3,344,973
nonzero samples, absolute peak 32,768 and RMS 4,038.96. This verifies signal
activity, not fidelity or freedom from clipping. Native audio and music
were not tested. Inspected captures include `doom-600M.png`,
`doom-smoke-menu.png`, `doom-native-demo.png` and `doom-native-menu.png`.

The long redirected-string console test also exposed a performance issue
outside this stack change: AH=09h emits file output character by character,
and a live execution sample resolved to `nvme_io_submit_wait`. The test
eventually passed. Batching must preserve DOS error and Ctrl-C semantics;
no executable-specific shortcut was added.

Build and `git diff --check` passed. Final diagnostics are in
`/root/osito-dpmi-shared-stack-20260906-RApHEw/`; the comparison against the
unchanged 4.48 kernel is in
`/root/osito-dpmi-shared-baseline-20260906-HVwoCW/`. Both used separate
128 MiB snapshots, QEMU/KVM, 8 GiB and four CPUs, VNC :12 / UDP 7790.
Final kernel and fixture SHA-256 values:

```text
kernel    52f956fb3e12a05bdbb9fe0189ce699fe09fed67e24f7cfba9a09803eaefbd00
dplock16  0a75187167db9429993318819b12b34617ffc015d35a386c2066ffcc12d360e9
dplock32  4fec2308b250149df61ebc7f1e39522ba53500aaa249375980920f84fc41844d
```

The booted kernel matches the workspace build. All owned test VMs and
helpers are stopped; no user VM or original disk image was changed.
GitNexus was unavailable, so impact review followed allocation, shared
stack selection, interrupt delivery/return, native dispatch, mode simulation
and the fixtures manually. No commit was made.

Remaining scope at this checkpoint (the first two items are addressed in
4.50 below): nested raw switches still need real host-state
save/restore via 0305h; its current zero-sized RETF service does not preserve
the new suspended cursor. Real-mode INT 1Ch still follows the ROM path and
needs reflection to an installed protected handler. General guest paging,
hidden descriptor state, edited return-destination validation, native FLAGS,
x87 vector ownership, music and performance remain open. This is not a
claim of complete DPMI or DOS/4GW compatibility.

### 4.50 DPMI dormant-mode state and real-mode timer reflection

0305h is no longer a zero-sized RETF service. It reports a 12-byte opaque
record and exposes register-preserving FAR-call gates in both modes:
AL=0 saves, AL=1 restores the *other* mode's dormant stack cursor.
The PM entry uses ES:DI or ES:EDI according to client width; the real-mode
entry uses ES:DI. Buffer access checks the complete span and PM permissions
before writing, translates each guest page, and rejects malformed records
without changing the cursor. Invalid calls terminate with a diagnostic;
there is no invented carry/status return for this procedure.
[DPMI 0305h contract](https://www.delorie.com/djgpp/doc/dpmi/api/310305.html).

Only actual implicit state is serialized: SS and full ESP, with a version
and mode tag. CS:IP and destination segment registers are explicit inputs
to raw switching in this host; automatic return contexts remain on its C
stack or callback frames. The record contains no kernel pointers, host
continuations or client-editable nesting depths. Suspended stack descriptor
and range validity is checked when the cursor is consumed. This does not
implement hidden descriptor-cache state or general edited-return validation.

Raw PM->RM now parks the outgoing protected cursor, excluding the private
INT frame (the native gate has no such client-stack frame). RM->PM parks
the real cursor. Both directions preserve the logical interrupt state:
the native CPU's physical IF must not reenable a client that used 0900h.
Ordinary 0300h-0302h calls preserve both cursors automatically, while
callbacks and reflected control handlers maintain their suspended real
stack in LIFO order. Nested calls with SS:SP=0 consequently allocate below
the suspended real frame, instead of restarting at F000:FFFE.
[DPMI stack and mode-switch rules](https://www.delorie.com/djgpp/doc/dpmi/ch4.3.html).

Real-mode INT 1Ch now enters an installed protected handler on the shared
locked stack, including the ROM timer path and 0300h with a custom real
IVT handler. Chaining the default PM vector calls that real IVT target
with an IRET frame, rather than issuing another INT 1Ch. There is no global
recursion-disable flag: a tick nested inside the real handler is still
reflected. The handler's stack return is checked and the real caller is
restored afterwards.
[DPMI special software interrupts](https://www.delorie.com/djgpp/doc/dpmi/ch4.4.3.html).

`dpstate16.com` and `dpstate32.com` test top-level and nested raw switches,
both save/restore gates, dormant-stack restoration in both directions,
stack canaries, preserved CF/DF, disabled virtual IF, real-to-protected
timer delivery, default-vector chaining, a tick inside the chained real
handler, and 0300h interception. Both return 42; USE32 passes through native
entry and with `--emulate`. The identical fixtures return 129 on the
unchanged 4.49 kernel, rejecting its zero-sized state service. The locked
stack fixture now disables asynchronous virtual IRQs before installing its
counted INT 1Ch handler, keeping its two explicit deliveries deterministic.

Final regression results:

- DOS API suite: PASS, including 54 state-service, 80 locked-stack,
  76 exception-stack and 3,074 ESP-return checks, all with zero failures.
- `dplock16/32`, including emulated USE32, and native SS16/SS32 fault
  fixtures return 42. Ctrl-C and critical-error USE16/USE32 probes pass.
- PE32 SEH3 and SEH4 exit zero. The emulated full DPMI fixture exits 42;
  native KVM still exits 7Ch at the previously documented ICEBP mismatch.
- Emulated FLAGS fixtures pass. Without `--emulate` both still exit 71h;
  the same FLAGS and ICEBP failures were reproduced on the 4.49 kernel.
- DOOM with `--emulate -config audio.cfg -nomusic` renders its demo and
  exits zero at 673,563,180 instructions after F10/Y, with four key bytes,
  reads and IRQ1 deliveries. The VM capture contains 44.088 seconds of
  48 kHz stereo PCM, 3,112,916 nonzero samples, peak 32,404 and RMS 3,759.00.
  This verifies signal output, not audio fidelity or music support.
- Native DOOM with `-nosound` renders and exits zero using F10/Y; four key
  bytes/reads/IRQs and 45,254,339 mediated VGA accesses were recorded.
  This is not a performance benchmark. State and SS16 fault probes still
  pass afterwards.

Final artifacts: `/root/osito-dpmi-state-if-20260906-xA2KRE/`.
Before comparison: `/root/osito-dpmi-state-before-20260906-EpH10f/`.
Both used separate 128 MiB snapshots, QEMU/KVM, 8 GiB, four CPUs,
VNC :12 and UDP 7790. Inspected captures include `doom-600M.png` and
`doom-native-demo.png`; the run also retains confirmation-menu captures.

```text
kernel    9c61e2cc2d9114b7037db0722ade85fc6b2e785d5a8ea2e393e3035234a8866d
dpstate16 7ecd620c9c9faab158d73802dfc5e99f057346ea35f6e16b5a6c293929173fef
dpstate32 d52e0ad216aedd9c872dacc1d3a5440f3732015235c8d04eb962988603f704ba
```

The tested kernel matches the workspace build. Build and whitespace checks
pass; all owned VMs/helpers are stopped and original user images were not
modified. GitNexus was unavailable; manual impact review covered DPMI
state, raw/automatic transitions, callbacks, special interrupt reflection,
interpreter/native private gates and fixtures. No commit was made.

At this checkpoint, general guest paging, hidden descriptor state, edited
return-destination validation, native FLAGS, x87 vector ownership, music and
performance remain open. Section 4.51 below addresses the exception and
locked-interrupt part of return validation. These changes do not establish
complete DPMI or DOS/4GW compatibility.

### 4.51 Transactional DPMI exception and locked interrupt returns

The DPMI 0203h exception frame allows the client to change the interrupted
CS:(E)IP and SS:(E)SP. Previously the private return service checked the
source frame's span, then popped and installed those destinations without
checking their segment validity. It now reads the complete frame into
locals and validates the destination before changing CPU registers, nesting
depth or virtual IF. Native and interpreted private gates share this path;
their different private INT frame sizes remain explicit.
[DPMI exception contract](https://www.delorie.com/djgpp/doc/dpmi/ch4.5.html).

The shared validator requires client RPL3, a present executable legacy code
segment, a compatible code DPL, EIP within the inclusive code limit, and a
present writable DPL3 data segment for SS. Execute-only code is valid;
conforming code uses its architectural privilege rule. CS.L cannot request
a 64-bit destination through this legacy DPMI service. The interrupted
stack pointer is preserved, including the upper ESP word for USE32/SS16.
An empty stack, a one-past-end cursor, or an expand-down cursor is not
rejected merely because the next stack access might fault: loading SS:ESP
does not access that memory. Source-frame access remains separately checked.
[Intel IRET reference, Volume 2A](https://cdrdv2-public.intel.com/812383/253666-sdm-vol-2a.pdf).

Locked IRQ and special-interrupt returns also revalidate the saved caller
segments before restoring their host-owned cursor. A handler can change a
descriptor while active, even though it cannot edit that cursor directly.
Rejected returns emit a diagnostic and use the existing client-termination
path, without partially consuming the frame. This does not introduce a
restartable host-exception protocol or change the existing FLAGS policy.

Tests on 2026-09-06:

- `dos-api-test`: PASS. The new destination matrix has 224 checks across
  USE16/USE32, SS16/SS32 and native/interpreted private-frame layouts. It
  covers legal edits, code and stack rights, presence, selector bounds and
  code limits. Rejection checks compare CPU bytes, depth, virtual IF and
  source-frame canaries; they do not execute invalid return destinations.
- Locked-stack coverage grows from 80 to 176 checks, including descriptor
  changes between delivery and return. Exception-stack checks remain 76,
  dormant-state checks 54 and ESP-return checks 3,074, all with zero failures.
  The older USE32 frame test now gives its code descriptor a limit covering
  its existing 32-bit test EIP instead of using an inconsistent 64 KiB limit.
- `dplock16.com` and `dplock32.com` now allocate distinct code/stack aliases
  and resume through a handler-edited CS:IP and SS:ESP. Both exit 42; USE32
  does so through native entry and with `--emulate`, including full ESP on
  SS16. `dpstate16/32`, native `dpfault` and `dpfalt32` also exit 42.
- DOOM `--emulate -config audio.cfg -nomusic` renders its demo and exits 0
  at 665,436,715 instructions after F10/Y. Native DOOM `-nosound` renders and
  exits 0, with 58,270,304 mediated VGA accesses. Each run records four key
  bytes, reads and IRQ1 deliveries. These are regression checks, not speed
  benchmarks. The VM's 48 kHz stereo capture has 3,752,009 nonzero samples,
  peak 31,354 and RMS 3,951.07; that verifies signal, not fidelity or music.
- PE32 SEH3 and SEH4 exit 0 after DOOM; native `dplock32` still exits 42
  afterwards. Build with `make -s -C arch/x86 CLANG=1 -j4 all
  dos-exception-test` and `git diff --check` pass.

Artifacts are archived in `arch/x86/build/dpmi-return-20260906/`, including
the tested kernel, fixtures, `serial.log`, WAV, and inspected
`doom-600M.png` / `doom-native-demo.png` captures. The isolated QEMU/KVM VM
used 8 GiB, four CPUs, a 128 MiB snapshot, VNC :12 and UDP 7790. It and its
helpers are stopped. Original user images and sessions were not modified.

```text
kernel   b639eeea6ae48f1a11c041f945ff7c285996e2a0b1ad63bf15f019693e09bd1c
dplock16 aa926aeadc4c0fe98f5e5eba328f36b44862e1bc6170b98127449fcafab21180
dplock32 6e6331bb09ea2f423c043497d7fc7636d865b319b724d453f8f2c10e4f051ce7
```

This is segment-level validation for these two DPMI return paths, not
complete guest return validation. General IRET/RETF, callback/raw-switch
destinations, instruction-fetch translation/paging, and hidden descriptor
state still need their own contracts. Native FLAGS, the KVM ICEBP mismatch,
x87 ownership, music and performance remain open. GitNexus tools were not
available; manual impact review covered the shared return routines, their
native/interpreted callers and tests. No commit was made.

### 4.52 Checked DPMI records and alternate callback buffers

Fixed-size DPMI records no longer use a single physical address from the
legacy wrapping translator. A local buffer view validates the complete
segment span and resolves at most two 4 KiB pages before touching payload
memory. GDT and LDT records use the same presence, privilege and read/write
checks, including readable code and expand-down data. Linear overflow and
addresses outside VM backing are rejected instead of aliasing low memory.
When CR0.PG is set, the view walks low and high linear addresses, checks
P/U/RW at both levels, and updates accessed/dirty bits for transferred pages.
Translations are captured before writes, including a record which aliases
its own page-table entry. This covers the vCPU's 386-style 4 KiB mappings;
it does not introduce CR4, PSE, PAE or general CPU paging/fault delivery.

Callback entry requires a writable 50-byte register record. Return only
requires a readable record, and ES:DI/EDI may name another buffer. The host
snapshots that record and validates the private source-frame span before
changing registers, callback depth or the dormant real-stack cursor. It
does not advance the protected SP before restoring the returned real SS:SP.
The executable fixture copies the register image to a different buffer,
returns through a read-only alias, and relocates the suspended 0301h
caller's output descriptor. Copyback follows the new mapping, leaving the
old output image intact. This follows the
[DPMI callback contract](https://www.delorie.com/djgpp/doc/dpmi/ch4.6.html).

Services 0300h-0302h, 0303h, 0305h, default interrupt/exception reflection,
and 0500h use the checked record view. Output is revalidated after nested
real-mode execution rather than retaining a stale physical destination.
0500h now uses DI for USE16 clients even when EDI has a nonzero upper word,
as required by the
[memory-information ABI](https://www.delorie.com/djgpp/doc/dpmi/api/310500.html).

Tests on 2026-09-06:

- `dos-api-test`: PASS. Buffer coverage has 1,334 checks: descriptor access
  matrix, inclusive/expand-down limits, missing and read-only pages, split
  records across a PDE boundary, accessed/dirty bits, page-table aliasing,
  paged 0301h and 0500h, and a split 0305h record whose missing second page
  must leave the dormant cursor untouched. Callback coverage adds 52 checks
  for USE16/USE32 and SS16/SS32; rejected source records/frames leave CPU
  bytes and callback ownership unchanged without executing invalid targets.
- New `dpcbuf16.com` and `dpcbuf32.com` exit 42. USE32 passes native entry
  and `--emulate`; USE16 includes nonzero upper EDI words at both 0301h
  entry and callback return. `dplock16/32`, `dpstate16/32`, native `dpfault`
  and `dpfalt32` also exit 42; `dplock32 --emulate` passes as well.
- DOOM `--emulate -config audio.cfg -nomusic` renders its demo and exits 0
  at 676,659,755 instructions after F10/Y. Native DOOM `-nosound` renders
  and exits 0, with 51,179,355 mediated VGA accesses. Both runs record four
  key bytes, reads and IRQ1 deliveries. The 48 kHz stereo WAV contains
  3,173,147 nonzero samples, peak 32,650 and RMS 3,921.86 across 44.937 s.
  These are regression and signal checks, not fidelity or speed benchmarks.
- PE32 SEH3 and SEH4 exit 0 after DOOM; native `dpcbuf32` exits 42 again
  afterwards. `make -s -C arch/x86 CLANG=1 -j4 all dos-exception-test` and
  `git diff --check` pass.

Final artifacts are in `arch/x86/build/dpmi-buffer-final-20260906/`: tested
kernel and callback fixtures, serial log, audio and inspected
`doom-600M.png` / `doom-native-demo.png` captures. The earlier buffer-only
run is retained separately in `arch/x86/build/dpmi-buffer-20260906/`.
Both isolated QEMU/KVM runs used 8 GiB, four CPUs, a 128 MiB snapshot,
VNC :12 and UDP 7790. They and their helpers are stopped; original user
images and sessions were not changed.

```text
kernel   98757cf6fdcf22413924542b5a3685ffbbd77852d4f4d661e8d621b2a57ecaad
dpcbuf16 1c331a83f3f29246d61cd9f77952260a8a8ea390d43bfd28403685bb4871cc1d
dpcbuf32 9ab4484cc14ad574d1c05f35977beb61f8242827b15f46ad52459b1d74d9d1c7
```

General interpreter memory accesses, descriptor-table paging and hidden
segment state are not normalized by this bounded host-record change.
Raw-switch destination faults, general IRET/RETF, native FLAGS, the KVM
ICEBP mismatch, x87 vector ownership, music and performance remain open.
This is not a claim of complete DPMI or DOS/4GW compatibility. GitNexus
tools were unavailable; manual impact review covered the record consumers,
native/interpreted callback gates and tests. No commit was made.

### 4.53 Protected 16/32-bit JIT blocks and dispatch accounting

The existing real-mode translator now also accepts resident protected
USE16/USE32 code when the user selects `--emulate`. MOV immediates/registers,
register ADD/SUB/CMP/AND/OR/XOR, INC/DEC, NOP and unprefixed near JMP/JZ/JNZ
use the guest operand width. Operand-size prefixes on register operations
are decoded without toggling on repeated 66h. Unsupported instructions,
including stack, FLAGS, memory operands, I/O, protected INT and mode changes,
return to their interpreter handlers at the first prefix byte. The native
CPL3 profile is unchanged; this is not a complete binary translator.

The cache uses hashed CS:full-EIP keys instead of a linear search over
16-bit IPs. Each translation records its executable base, bounded limit,
default width and inspected source bytes. Reuse revalidates that context
and compares bytes, so changed code, terminators, descriptors and mode do
not reuse stale machine code. Fetch is bounded by segment/backing limits
and the 15-byte instruction limit. Read-ahead stops before VGA and EMS
windows; banked/device code, guest paging and unsupported fetch contexts
remain interpreted. Filling the table or code cache clears the index and
compiled markers without compiling into an orphaned entry. The existing
4,096-entry / 256 KiB capacity still uses whole-cache eviction.

Generated register arithmetic merges only status bits. It never loads
guest IF, DF, TF or IOPL into host RFLAGS, and INC/DEC retain guest CF.
Protected short branches now update full EIP in the interpreter as well,
including carry/borrow across 64 KiB; the affected JMP/Jcc handlers clear
the upper word for 16-bit operands. See Intel's [JMP and Jcc contracts](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf).

JIT instructions now contribute to the CPU instruction counter, including
a translated prefix before an interpreter fallback. The shared dispatcher
services the periodic VGA/IRQ checkpoint when a block crosses a 16K boundary,
not only when it lands exactly on one. Callback stop cursors cannot be
skipped by a cached block. Single-step, TF and explicit diagnostic quotas
remain unbatched. Normal emulated exits print translated instruction totals
and protected-mode totals; these are dispatch/accounting diagnostics, not
hardware retired-instruction counters or a speed benchmark.

Tests on 2026-09-06, final kernel SHA-256
`a1e8e1b05bc612caf18ee378a2d61bc17ea3990c90dfea839f7b91942ac0fb5f`:

- `dos-api-test`: PASS, including 7,355 new JIT checks. These compare the
  interpreter with generated 16/32-bit register arithmetic, both ModR/M
  directions, operand overrides, high-byte immediates, carry/overflow edges,
  branch directions and full-EIP cache keys. Other cases cover stale code
  and descriptors, partial/overlong instructions, device-window boundaries,
  source rejection without executing invalid destinations, cache exhaustion,
  callback stop positions and crossing a timer-service checkpoint.
- `dpflags16/32 --emulate` exit 42 with 163,615 / 24,340 protected JIT
  instructions in this run. `dpcbuf16/32`, `dpstate16/32` and `dplock16/32`
  exit 42 under `--emulate`; native `dpcbuf32` and `dplock32` also exit 42.
  Real-mode `dosjit`, `dosbits`, `dospop` and `dosivt` exit 0.
- DOOM `--emulate -config audio.cfg -nomusic` renders its demo and exits 0
  after F10/Y at 662,864,435 instructions. JIT totals are 201,185,519,
  including 201,179,014 protected instructions. It compiled 91,591 blocks;
  frequent whole-cache invalidation remains a performance issue to measure.
  The 48 kHz stereo capture has 4,100,460 nonzero samples, peak 31,834 and
  RMS 3,971.04 over 55.065 seconds. This verifies signal, not music or fidelity.
- Native DOOM `-nosound` renders and exits 0 with 65,092,617 mediated VGA
  accesses. Both DOOM runs record four key bytes, reads and IRQ1 deliveries.
  PE32 SEH3/SEH4 exit 0 afterwards, followed by another successful
  `dpflags32 --emulate` run.
- `make -s -C arch/x86 CLANG=1 -j4 all dos-exception-test dos-flags-test`
  and `git diff --check` pass. Final build size is 5,585,048 bytes.

Artifacts are archived in `arch/x86/build/dpmi-jit-verified-20260906/`,
including the tested kernel, serial log, WAV and inspected `doom-600M.png`
and `doom-native-demo.png`. Earlier suite runs are retained separately in
`dpmi-jit-20260906/` and `dpmi-jit-interim-20260906/`. The isolated QEMU/KVM
runs used 8 GiB, four CPUs, a 128 MiB snapshot, VNC :12 and UDP 7790. All
owned VMs and helpers are stopped; user sessions and original images were
not modified. GitNexus tools were unavailable; manual impact review covered
the decoder/compiler, dispatcher, cache consumers, DOS runner and tests.
No commit was made.

Native `dpflags32` still exits 71h, not 42: CLI is mediated, but the next
nontrapping PUSHFD sees physical IF. Enabling PVI is not a fix for PUSHF;
Intel's [protected-mode virtual-interrupt description](https://cdrdv2-public.intel.com/850979/253669-087-sdm-vol-3b.pdf)
distinguishes virtual CLI/STI from the other FLAGS instructions. Closing
this gap requires a complete translated/virtualized execution contract,
not granting hardware IOPL3 or patching a particular executable. General
guest memory/paging, hidden segment state, remaining return paths, the KVM
ICEBP mismatch, x87 ownership, music and performance also remain open.

### 4.54 Bounded JIT cache reuse and interpreter backoff

This supersedes the whole-cache pressure eviction recorded in 4.53. The
4,096-entry index and 256 KiB executable arena have independent intrusive
LRU lists. Index pressure unlinks one cold entry from its hash chain and
reuses its slot. Code pressure releases cold native ranges but keeps their
source and IR indexed; the dispatcher recompiles retained IR only after
validating the current guest context and bytes. Explicit invalidation and
teardown still clear the entire cache.

The code arena is a buddy allocator with 64-byte minimum allocations and
bounded metadata outside executable memory. Compilation first emits into
a bounded scratch buffer, then allocates the measured byte count rounded
to a size class. Re-decoding/recompiling releases the previous range, and
adjacent free buddies coalesce. Diagnostics distinguish allocated bytes,
payload bytes, index/code evictions and explicit resets. These changes do
not increase the entry count or executable budget; the links, buddy tree
and emission buffer add bounded metadata. Generated functions have no
inter-block links or host calls and return before DOS interrupt/callback
dispatch, so eviction does not retire code suspended inside a guest callback.

LRU alone was insufficient: an initial run of the same timedemo compiled
63,174 blocks with 263,526 entry evictions. Repeated zero-instruction
translations were competing with useful blocks. An unsupported start now
resets its existing coarse hotness counter, so the JIT retries after it
warms again instead of on every interpreter dispatch. The interpreter
still executes every instruction, including code modified during this
interval. This is an admission heuristic, not a permanent unsupported-code
cache or an executable-specific exception.

Verified on 2026-09-06 with kernel SHA-256
`9c00d508a7773e7625c0b6c06fb3c1de7f884d5d75d69133beb1568d1e393550`:

- `dos-api-test`: PASS, including 10,466 cache and 3,257 CPU/JIT checks.
  The cache tests reconstruct hash/LRU membership and occupied ranges
  independently of allocator metadata. Cases cover colliding keys, hot
  entry retention, mixed allocation sizes, payload preservation under
  eviction, full-arena coalescence, 1,024 recompilations without growth,
  retained-IR recompilation through the real dispatcher and modified
  instructions taking effect before and after the backoff interval.
- Emulated `dpflags16/32`, `dpcbuf16/32`, `dpstate16/32`, `dplock16/32`
  exit 42. Native `dpcbuf32` and `dplock32` exit 42. Real-mode `dosbits`,
  `dospop` and `dosivt` exit 0. Native `dpflags32` still exits 71h, which
  remains a known FLAGS gap, not a passing compatibility result.

The before/after comparison uses the same QEMU/KVM configuration (8 GiB,
four CPUs, VNC :12, UDP 7790 and a disposable 128 MiB snapshot) and the
same DOOM executable, WAD and configuration. Each kernel runs three times:

```text
dosrun --emulate DOOM.EXE -config audio.cfg -nosound -timedemo demo1 -nodraw
```

| Kernel | Wall seconds, runs 1/2/3 | Compiled blocks, runs 1/2/3 | Guest realtics, runs 1/2/3 | Full resets per run |
|---|---|---|---|---|
| 4.53, a1e8e1b0 | 40.783 / 40.808 / 40.660 | 12095 / 12096 / 12097 | 256 / 256 / 257 | 13 |
| 4.54, 9c00d508 | 39.939 / 38.328 / 37.535 | 6702 / 6619 / 6618 | 231 / 227 / 228 | 0 |

Every run completes 1,710 gametics and reports exit 1 after the timedemo
summary. DOOM intentionally reports completed timedemos through
[`G_CheckDemoStatus` / `I_Error`](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/g_game.c#L1576-L1585);
this is distinct from the interactive exit-0 checks. Wall time is measured
from submitting the command's CR to the returned shell prompt and includes
initialization and shutdown. Median recompilations fall from 12,096 to
6,619; median wall time falls from 40.783 to 38.328 seconds. This small
sample excludes drawing and sound and is not a rendered-FPS or general
application-speed claim. The final runs still evict about 40,000 index
entries, so this does not eliminate all translation overhead.

A second comparison removes only `-nodraw`, keeping sound disabled. One
complete rendered pass per kernel produces:

| Kernel | Wall seconds | Guest realtics | Compiled blocks | Full resets | Guest instructions |
|---|---|---|---|---|---|
| 4.53, a1e8e1b0 | 180.910 | 5159 | 163089 | 168 | 1526138388 |
| 4.54, 9c00d508 | 171.555 | 4883 | 75000 | 0 | 1525630485 |

Both complete the same 1,710 gametics and return the timedemo report to
the shell. This pair shows about 5% lower wall time and 54% fewer compiled
blocks, not a statistically established speedup across DOS applications.
The final rendered run still interprets 1,161,108,591 instructions and
evicts 769,080 index entries; most guest work remains outside this limited
register/control-flow translator. Memory operands and the other excluded
instruction families still require their existing interpreter contracts.

Interactive regressions on the same final kernel also pass:

- `--emulate -config audio.cfg -nomusic` renders DOOM and exits 0 after
  F10/Y at 661,127,730 instructions, including 206,715,261 JIT instructions
  (206,703,952 protected). It compiles 147,864 blocks, with 648,936 index
  evictions and no full reset. This timed interactive session is not the
  fixed-demo workload above and cannot be used as a matched performance
  comparison with an earlier attract-mode capture.
- The finalized 48 kHz stereo WAV spans 51.140 seconds, with 3,786,815
  nonzero samples, peak 31,447 and RMS 3,953.27. This verifies digital
  signal, not music or fidelity. `doom-600M.png` shows the rendered game.
- Native DOOM `-nosound` renders and exits 0 with 13,164,507 mediated VGA
  accesses. `doom-native-demo.png` shows its game view. Both interactive
  runs record four key bytes, data reads and IRQ1 deliveries.
- After both DOS profiles and the rendered timedemo, PE32 SEH3/SEH4 exit
  0 and `dpflags32 --emulate` exits 42 again.
- `make -s -C arch/x86 CLANG=1 -j4 all dos-exception-test dos-flags-test`
  and `git diff --check` pass. Build size is 5,597,504 bytes; the workspace
  kernel and archived tested kernel have the same SHA-256 above.

Final-kernel artifacts are in `arch/x86/build/dpmi-cache-final-20260906/`,
including the kernel, serial log, WAV, screenshots and per-run benchmark
JSON/log files. The previous kernel's no-draw results are retained in
`dpmi-cache-before-20260906/`; the initial LRU-only run is retained in
`dpmi-cache-initial-20260906/` rather than discarded from the comparison.
The previous kernel's rendered pass is in
`dpmi-cache-render-before-20260906/`. All owned diagnostic QEMU instances
and helpers are stopped. User sessions and original disk images were not
modified.
GitNexus tools were unavailable; manual impact review covered cache
consumers, the decoder/compiler, interpreter dispatcher, DOS runner,
interrupt/callback re-entry and in-kernel tests. No commit was made.

Native FLAGS, general guest memory/paging, hidden descriptor state,
remaining return paths, the KVM ICEBP mismatch, x87 ownership and music
remain open. The fixed-size IR table and other performance costs also
remain; this is not complete DPMI or DOS/4GW compatibility.

### 4.55 Shared guest page walker and checked operand faults

The bounded DPMI record path and the interpreter's already-preflighted
operands now share `dos_paging.c`. This is a foundation for normalizing
memory access, not a claim that arbitrary paged guests now work.

`dos_page_probe` walks every enabled 4 KiB mapping, including low linear
addresses, combines PDE/PTE permissions and reports the linear fault
address plus P/W/U error bits. Supervisor writes honor CR0.WP; user writes
always require R/W at both levels. The advertised integer CPU has no PSE
or PAE, so PDE.PS and PTE bit 7 do not enable large pages or reserved-bit
faults. This follows the non-PAE, PSE-disabled contract in the
[Intel SDM, volume 3A, sections 5.3, 5.6 and 5.7](https://cdrdv2-public.intel.com/874249/253668-090-sdm-vol-3a.pdf).

Probing leaves the output unchanged on failure and never changes CR2 or
A/D bits. Callers validate all spans before `dos_page_commit` updates A
at both levels and D on a written PTE. Physical backing is a separate
contract: a present mapping outside guest RAM is not silently wrapped
and does not manufacture a missing-page exception. Physical bus helpers
retain their unbacked-read/write behavior; host records also require
resident paging entries and resident data spans.

- Fixed-size DPMI buffers retain CF/error returns, their two-page snapshot,
  read-only input support, nested-call revalidation, and page-table-alias
  behavior. The old private page walker has been removed from this path.
- INS/OUTS and LES/LDS/LSS/LFS/LGS source operands check their segment first,
  then snapshot both possible page translations. A missing or protected
  page delivers #PF with CR2, the access error and the instruction's prefix
  address. A failed INS cannot consume a port value or partially write its
  destination. REP retains completed transfers and the remaining count.

Verification on 2026-09-06 uses QEMU/KVM, 8 GiB, four CPUs, VNC :12,
UDP 7790, and a disposable 128 MiB snapshot. Tested kernel SHA-256:
`db207c4ce3a08cefe268d32b4ef3ab9aa7f3bd5e3d6a9607f8c2c8c59b820a94`
(5,605,896 bytes).

- `dos-api-test` passes. The new page-walk matrix has 5,126 checks covering
  P/RW/US combinations at both levels, supervisor/user read/write, WP,
  ignored flags, low/high addresses, metadata updates and unbacked RAM.
- The new checked-operand suite passes 528 cases, with 384 expected faults:
  USE16/USE32, byte/word/dword ports, split pages/directories, partial REP,
  segment-fault precedence, CR2/error/restart frames, unchanged failed
  transfers, and split-page LDS. These are software CPU tests using the
  host-managed LDT and identity-mapped code/exception support, not tests of
  paged GDT/LDT or native return destinations.
- Existing DPMI record-buffer checks (1,334), callback-buffer checks (52),
  JIT cache checks (10,466) and CPU/JIT checks (3,257) pass.
- Emulated `dpflags16/32`, `dpcbuf16/32`, `dpstate16/32`, `dplock16/32`
  and native `dpcbuf32`/`dplock32` exit 42. Real-mode `dosbits`, `dospop`,
  `dosivt` and the `dosjit` autoload exit 0. Native `dpflags32` still exits
  71h, the known FLAGS gap, not a passing test.
- Emulated DOOM `-config audio.cfg -nomusic` renders and exits 0 after
  F10/Y at 660,636,210 instructions. It executes 204,773,273 JIT instructions
  (204,761,724 protected), with no cache reset. The finalized 48 kHz stereo
  WAV contains 53.055 seconds, 3,927,062 nonzero samples, peak 32,450 and
  RMS 3,970.27. This establishes digital signal, not music or fidelity.
- Native DOOM `-nosound` renders and exits 0 with 13,239,072 mediated VGA
  accesses. Both DOOM profiles consume four key bytes and deliver four
  IRQ1 events. Their game screenshots were inspected.
- After both DOOM runs, PE32 SEH3/SEH4 exit 0 and emulated `dpflags32`
  exits 42 again. The full x86 build, DOS exception/FLAGS fixture builds
  and `git diff --check` pass.

Kernel, serial log, WAV and screenshots are retained in
`arch/x86/build/dos-paging-verified-20260906/`. Diagnostic QEMU and helpers
are stopped; no user VM or original disk image was modified. GitNexus was
unavailable. Manual impact review covered the shared walker, bounded DPMI
buffers, the two checked CPU operand callers and their in-kernel tests.
No commit was made.

General paging remains open: `dpmi_translate` still retains the legacy
out-of-RAM-only walk and modulo fallback for unchecked consumers. Fetch,
ordinary ModRM, stack and non-port string accesses still need precise
fault propagation and restart semantics. Guest GDT/LDT reads and descriptor
accessed-bit writes remain physical; hidden descriptor state is not modeled.
Do not replace those consumers with a success-shaped sentinel on failure.
Native FLAGS, remaining return paths, KVM ICEBP behavior, x87 ownership,
music and performance also remain open. This is not complete DPMI or
DOS/4GW compatibility.

### 4.56 Checked instruction fetch and paged guest descriptors

Instruction fetch now has a stack-local decode state and explicit failure
exits, rather than returning a success-shaped byte from an invalid address.
Every opcode, prefix, ModRM/SIB byte, displacement and immediate goes through
the same checked path. It enforces the CS limit and 15-byte instruction
limit, translates enabled 4 KiB pages with the current privilege, and
delivers #PF with CR2/error bits and the first-prefix IP. #GP for length or
segment failure leaves CR2 unchanged. A scalar fetch crossing pages is
preflighted before reading its bytes; no unused bytes are prefetched.

The page cache lasts only for the current instruction. Fetch marks pages
accessed, not dirty, and accepts read-only user code. It does not set the
page-fault I/D bit: this CPU profile exposes neither SMEP nor PAE/NXE. The
length and error-code rules follow the
[Intel SDM, volume 3A](https://cdrdv2-public.intel.com/874249/253668-090-sdm-vol-3a.pdf).
Simultaneous overlength and missing-next-page priority is not established
by these tests. No host longjmp guard survives a callback or native resume.

ModRM decoding now retains an effective address without resolving guest
memory. Immediate-bearing handlers fetch the immediate before reading the
operand. This matters for read-side-effect devices, not just register or
RAM writes. POP with an ESP-based destination adjusts its already-decoded
offset after popping instead of fetching the SIB/displacement a second
time. Existing POP and general instruction tests remain in place.

`dpmi_lookup_descriptor` returns a descriptor snapshot plus its table
location and a typed paging failure. Guest GDT/LDT references are paged
supervisor reads even at CPL3, as specified by the
[Intel 80386 protection reference, section 6.4.3](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s06_04.htm).
Both possible pages are validated before reading the descriptor. The
linear address wraps at 32 bits; unbacked physical bytes retain the bus
contract rather than being confused with an invalid selector.

LAR, LSL, VERR, VERW and checked operand validation distinguish descriptor
page faults from selector failures. Far-pointer segment loads use the same
snapshot to validate the destination and set its accessed bit before
committing the segment or destination GPR. That write is a supervisor
access honoring CR0.WP. An already-set accessed bit requires no write;
queries do not set it. Legacy callers retain the boolean descriptor wrapper
and have not all gained typed fault delivery.

The new fetch checks also exposed a real transition error:
`dpmi_simulate_rm_call` cleared PE but left PG enabled. Its real-mode phase
now clears both and restores the full saved CPU state afterward. Two added
record-buffer assertions verify restoration of CR0 and CR3. This fixes
ordinary simulated real-mode execution, not all nested paged callbacks.

Verification on 2026-09-06 uses QEMU/KVM, 8 GiB, four CPUs, VNC :12,
UDP 7790 and a disposable 128 MiB snapshot. Tested kernel SHA-256:
`63f738bf63032d2c99cf3c9ca518fafdb71dd3967a602c501ea94ebbe07f0752`
(5,627,064 bytes), matching the workspace kernel.

- `dos-api-test` passes. The fetch matrix passes 11,940 cases with 5,430
  expected faults: USE16/USE32, 21 instruction shapes, every byte boundary,
  15-byte padding, low/high page and directory splits, missing/supervisor
  pages, read-only code, complete restart frames, registers, memory and PIC
  port state. Separate cases cover real/protected CS limits and overlength.
- The VGA fetch suite passes 48 cases. It copies planar read latches with
  VGA write mode 1 after an immediate fetch succeeds or faults. Twelve
  instruction shapes prove that an unavailable immediate cannot cause a
  premature operand read, including instructions that do not write memory.
- The descriptor paging suite passes 274 cases: GDT and external LDT,
  split pages/directories, 32-bit wrapping, supervisor-only descriptor
  pages, CR0.WP, accessed-bit writes, unchanged outputs on lookup failure,
  precise CPU query/load faults and CS-descriptor versus code-page faults.
- Existing selector checks (786,500), page-walk checks (5,126), checked
  operand checks (528), DPMI record-buffer checks (1,336), callback-buffer
  checks (52), JIT cache checks (10,466) and CPU/JIT checks (3,257) pass.
- Emulated `dpflags16/32`, `dpcbuf16/32`, `dpstate16/32`, `dplock16/32`
  and native `dpcbuf32`/`dplock32` exit 42. Real-mode `dosbits`, `dospop`,
  `dosivt` and the `dosjit` autoload exit 0. Native `dpflags32` still exits
  71h, the known FLAGS gap, not a passing test.

End-to-end checks on that same kernel:

- Emulated DOOM `-config audio.cfg -nomusic` renders and exits 0 after
  F10/Y at 658,670,130 instructions, including 203,206,269 JIT instructions
  (203,194,813 protected), with no cache reset. The finalized 48 kHz stereo
  WAV contains 52.265 seconds, 3,830,917 nonzero samples, peak 30,311 and
  RMS 3,898.70. This establishes digital signal, not music or fidelity.
- Native DOOM `-nosound` renders and exits 0, with 13,228,740 mediated VGA
  accesses. Both profiles consume four key bytes and deliver four IRQ1
  events. Their game screenshots were inspected.
- After both runs, PE32 SEH3/SEH4 exit 0 and emulated `dpflags32` exits 42
  again. The full x86 build, DOS exception/FLAGS fixture builds and
  `git diff --check` pass.

Kernel, serial log, WAV and screenshots are retained in
`arch/x86/build/dos-fetch-verified-20260906/`. Diagnostic QEMU and all
helpers are stopped; user VMs and original disk images were not modified.
GitNexus was unavailable. Manual impact review covered descriptor lookup
consumers, interpreter fetch/decoding, immediate-bearing handlers, POP
addressing, DPMI mode simulation and the in-kernel tests. No commit was made.

Initial failed runs are retained separately. The selector fixtures needed
a valid current CS and explicit expectations for physical bus holes and
32-bit address wrapping. The fetch fixture originally checked a DMA page
register without an initialized audio/DMA controller, reading FFh; it now
uses the available PIC mask port and exercises actual immediate IN/OUT
effects. Neither discrepancy was hidden by relaxing the CPU fault checks.

This is still partial guest paging. Ordinary ModRM, stack, non-port string
operations and several descriptor consumers retain legacy translation or
incomplete fault propagation. CS and other hidden descriptor state are not
modeled; re-reading the current CS descriptor is not equivalent to a real
cached descriptor. Paged callback/timer re-entry and raw PM/RM switches need
saved dormant CR0/CR3 as well as stack state; the current 0305h record does
not preserve that paging context. Privileged control-register validation,
other return paths, native FLAGS, KVM ICEBP, x87 ownership, music and
performance remain open. This is not complete DPMI or DOS/4GW compatibility.

### 4.57 Dormant paging context across DPMI re-entry

DPMI now keeps CR0/CR3 with each dormant mode's stack cursor. Raw PM/RM
switches save the outgoing context and restore the incoming one. Ordinary
0300h/0301h/0302h simulations retain their automatic, host-owned save/restore,
including when a real-mode call is nested inside a protected callback.
Real-mode execution no longer borrows the protected client's paging root.

Callback, real-mode timer, Ctrl-Break and critical-error entry stage the
protected context before validating protected code and record buffers.
Rejected entries restore the original CPU without consuming the real-mode
frame. Callback return remembers its outgoing protected context and restores
the actual real-mode caller's CR0/CR3. Per-depth ownership also preserves a
previously suspended real-mode context. Synchronous timer/control handlers
restore the interrupted CPU and the prior dormant real-mode context.

The opaque 0305h state record grows from 12 to 20 bytes (signature `DPS2`),
adding the other mode's CR0/CR3. Clients obtain the size from AX rather than
depending on its layout. Restore rejects a mismatched mode/PE bit or paging
enabled in a real-mode record before changing either cursor or paging state.
It does not alter the active CPU. Raw destinations remain explicit register
arguments; host continuations and nesting bookkeeping never enter this
client-owned buffer. The distinction between automatic mode switching and
0305h-managed nested raw switching follows the
[DPMI mode-switch contract](https://www.delorie.com/djgpp/doc/dpmi/ch4.3.html).
Storing CR0/CR3 is this software CPU's implementation choice, not a prescribed
public DPMI record format.

Verification on 2026-09-06 uses QEMU/KVM, 8 GiB, four CPUs, VNC :12,
UDP 7790 and a disposable 128 MiB snapshot. Tested kernel SHA-256:
`4d7a5b8050138b19951446da243d6257dfed49dc6adfbb97abb4466cd6ba3a79`
(5,639,400 bytes), matching the workspace kernel.

- `dos-api-test` passes. The new re-entry suite passes 308 checks with two
  different page directories and both client widths. Callback records use
  low/high virtual addresses and split, noncontiguous physical pages.
  Missing, supervisor-only and read-only buffer cases verify rejection
  without overwriting the original record or its unmapped identity alias.
- Timer, Ctrl-Break and critical-error fixtures execute actual mapped
  protected handlers and return to their original real-mode CR0/CR3. Missing
  and supervisor-only code pages reject entry without executing the handler.
- Nested raw switches save state A through 0305h, temporarily replace the
  dormant context with B, then restore and execute in A again. A separate
  0301h -> callback -> nested 0301h -> real-mode leaf sequence checks both
  register results and automatic restoration of the previous owners.
- The 0305h suite now passes 74 checks, up from 54. Together these suites
  add 328 assertions. They use the bounded software CPU, not native malformed
  return tests; host support code and stacks remain identity mapped.
- Existing fetch checks (11,940), descriptor checks (274), page-walk checks
  (5,126), checked operands (528), DPMI record buffers (1,336), callback
  buffers (52), JIT cache (10,466) and CPU/JIT checks (3,257) pass.
- Emulated `dpflags16/32`, `dpcbuf16/32`, `dpstate16/32`, `dplock16/32`
  and native `dpcbuf32`/`dplock32` exit 42. Real-mode `dosbits`, `dospop`,
  `dosivt` and the `dosjit` autoload exit 0. Native `dpflags32` still exits
  71h, the known FLAGS gap, not a passing test.

End-to-end checks on that same kernel:

- Emulated DOOM `-config audio.cfg -nomusic` renders and exits 0 after
  F10/Y at 656,065,074 instructions, including 200,620,975 JIT instructions
  (200,609,200 protected), with no cache reset. The finalized 48 kHz stereo
  WAV contains 53.125 seconds, 3,968,820 nonzero samples, peak 32,496 and
  RMS 3,988.65. This establishes digital signal, not music or fidelity.
- Native DOOM `-nosound` renders and exits 0, with 13,145,485 mediated VGA
  accesses. Both profiles consume four key bytes and deliver four IRQ1
  events. Their game screenshots were inspected.
- After both runs, PE32 SEH3/SEH4 exit 0 and emulated `dpflags32` exits 42
  again. The full x86 build, DOS exception/FLAGS fixture builds and
  `git diff --check` pass.

Kernel, serial log, WAV and screenshots are retained in
`arch/x86/build/dpmi-paging-state-verified-20260906/`. The initial failed run
is retained in `arch/x86/build/dpmi-paging-state-initial-20260906/`: the
metadata-only outer DPMI fixture had bypassed `dpmi_init`, so it lacked a
valid dormant protected CR0. It now initializes the normal defaults before
assigning its logical memory size, without accessing absent physical RAM.
No production validation was relaxed to make the fixture pass.

Diagnostic QEMU and helpers are stopped; user VMs and original disk images
were not modified. GitNexus was unavailable. Manual impact review covered
the DPMI state layout, raw switches, simulation, callback/timer/control
entry and return, 0305h and their in-kernel tests. No commit was made.

This does not complete guest paging. Ordinary stack, ModRM and non-port
string accesses still need checked translation and precise fault/restart
semantics. In particular, a paged callback buffer and code do not establish
support for arbitrary paged handler stacks or data instructions. Hidden
descriptor state, remaining descriptor consumers and return destinations,
control-register validation, native FLAGS, KVM ICEBP, x87 ownership, music
and performance remain open. This is not complete DPMI or DOS/4GW
compatibility.

### 4.58 Checked ModRM operands and multi-field memory records

The software CPU no longer translates only the first byte of an ordinary
ModRM memory operand. Decoding retains the segment and effective offset;
the access helpers validate the complete 1/2/4-byte range through the shared
segment and page checks. Moffs loads/stores and XLAT use the same path.
Faults leave the dispatcher explicitly before later register, FLAGS or
memory effects, with the original instruction address and page-fault
information. The fault/restart distinction follows the
[80386 exception contract](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s09_08.htm).

Read-modify-write instructions declare write intent before their initial
read. A read-only or absent destination therefore cannot change VGA read
latches, arithmetic flags or a partial memory result. The translated byte
addresses are reused by the write within that instruction, not cached across
instructions or callbacks. This does not add a new guest TLB or change the
CPU/JIT state layout.

POP r/m checks both the stack source and destination before consuming the
stack or reading side-effecting memory. ESP-based destinations use the
post-increment address, while stack wrap follows SS.B independently of the
operand size. This treatment applies to opcode 8F, not yet to every stack
instruction or return path.

A checked record helper covers far-pointer loads, indirect far CALL/JMP
sources, BOUND's paired limits and the six-byte GDTR/IDTR operands. Records
may cross noncontiguous guest pages; every byte is checked before a store
or a table-register update. BOUND now compares signed 16-bit or 32-bit values
according to operand size. GDTR/IDTR loads retain their 24/32-bit base rule,
and stores retain all six bytes, as described by the
[80386 LGDT/LIDT contract](https://pdos.csail.mit.edu/6.828/2018/readings/i386/LGDT.htm).
The complete privilege and destination contracts of these instructions are
not claimed here.

Verification on 2026-09-07 uses QEMU/KVM, 8 GiB, four CPUs, VNC :12,
UDP 7790 and a disposable 128 MiB snapshot. Tested kernel SHA-256:
`608bd9dd4ee41ba8f53297700fc895988e6558233e3f9204a1309491a612749a`
(5,659,992 bytes), matching the workspace kernel.

- `dos-api-test` passes. The four new suites pass 8,748 checks: 7,920 scalar
  paging cases, 172 segment/POP boundary cases, 560 memory-record cases and
  96 planar-VGA read-side-effect cases.
- The scalar matrix covers both client widths, low/high virtual addresses,
  split physical pages, PTE/PDE absence, read-only and supervisor protection,
  CR2, error codes, first-prefix restart IP, saved registers and flags,
  A/D bits, guards and untouched identity aliases. It observes 5,232 expected
  faults and successfully performs 386 actual DPMI returns and retries after
  repairing a missing mapping.
- Boundary cases check segment limits, access rights, expand-down segments,
  segment-fault precedence and SS16/SS32 POP with both operand widths. The
  record suite cuts at every byte, checks missing/read-only pages, table
  state and invalid register forms, and verifies a source page fault before
  BOUND's bounds exception. LGDT/LIDT fixtures use CPL0 in the software CPU.
- Existing POP (208), far pointers (14,400), fetch (11,940), descriptor
  paging (274), page walker (5,126), checked operands (528), DPMI record
  buffers (1,336), dormant paging (308), JIT cache (10,466) and CPU/JIT
  (3,257) checks pass, along with the remaining DOS API suites.
- Emulated `dpflags16/32`, `dpcbuf16/32`, `dpstate16/32`, `dplock16/32`
  and native `dpcbuf32`/`dplock32` exit 42. Real-mode `dosbits`, `dospop`,
  `dosivt` and the `dosjit` autoload exit 0. Native `dpflags32` still exits
  71h; that known FLAGS gap is not counted as passing compatibility.

End-to-end checks on the same kernel:

- Emulated DOOM `-config audio.cfg -nomusic` renders and exits 0 after
  F10/Y at 652,870,195 instructions, including 200,403,786 JIT instructions
  (200,391,298 protected), with no cache reset. The finalized 48 kHz stereo
  WAV contains 59.347625 seconds, 4,441,737 nonzero samples, peak 32,768 and
  RMS 3,881.74. This establishes digital signal, not music or fidelity.
- Native DOOM `-nosound` renders gameplay and exits 0, with 13,032,714
  mediated VGA accesses. Both profiles consume four key bytes and deliver
  four IRQ1 events. Captures were inspected: emulated title/credits and
  native gameplay, plus the normal exits recorded by the serial helper.
- After both runs, PE32 SEH3/SEH4 exit 0 and emulated `dpflags32` exits 42
  again. The full x86 build, DOS exception/FLAGS fixture builds and
  `git diff --check` pass.

Kernel, serial log, WAV and screenshots are retained in
`arch/x86/build/dos-modrm-verified-20260907/`. The two initial runs are kept
in `arch/x86/build/dos-modrm-initial-20260907/` and
`arch/x86/build/dos-modrm-checks-initial-20260907/`. The first exposed an old
POP fixture with ESP 15000h beyond its SS32 limit FFFFh; its intended stack
now has limit 1FFFFh. The second exposed a new retry fixture that compared
unvirtualized IOPL bits against the DPMI return image; it now initializes IF,
IOPL and virtual IF consistently. Production checks were not relaxed.

Diagnostic QEMU and helpers are stopped; user VMs and original disk images
were not modified. GitNexus was unavailable. Manual impact review covered
the interpreter's operand helpers, decoder, affected opcode consumers and
their in-kernel tests. No commit was made.

General stack and non-port string accesses still need checked translation
and restart semantics. Hidden descriptor state, remaining segment loads and
return destinations, privileged-instruction validation (including LGDT/LIDT),
zero-count SHLD/SHRD memory semantics, native FLAGS, KVM ICEBP, x87 ownership,
music and performance remain open. Checking a far CALL's source does not
make its stack writes or target validation transactional. This is not
complete guest paging, DPMI or DOS/4GW compatibility.

### 4.59 Checked string memory and restartable REP

MOVS, CMPS, STOS, LODS and SCAS now share one checked software-CPU path for
byte, word and dword operands. Each element validates its complete source
and destination, where applicable, before memory or register effects.
Source segment overrides do not redirect the ES destination. MOVS snapshots
one element before writing it, preserving sequential overlap behavior rather
than substituting memmove semantics. The index and operand-size rules follow
the [80386 string contract](https://pdos.csail.mit.edu/6.828/2018/readings/i386/MOVS.htm).

All repeated memory strings use bounded 256-element dispatches, as port
strings already did. This is a scheduling boundary, not a total iteration
limit. CX/ECX is never replaced by an internal chunk count: only completed
elements consume it or advance SI/DI/ESI/EDI. A failed element retains its
indices and the first-prefix restart address. Zero count does not access
data; LOCK still raises the invalid-opcode exception. Ordinary emulation
polls guest interrupts between unfinished chunks, including port strings.
Single-step native mediation continues to leave interrupt delivery to its
native caller.

Repeated CMPS/SCAS faults restore the pre-instruction FLAGS, not the flags
from the last comparison. A small CPU continuation record preserves those
flags across internal chunks and checked fetch faults. It is keyed by CS:IP
and the decoded instruction form; completion, a changed instruction, an
exception, a delivered interrupt or a native-frame import invalidates it.
A masked interrupt does not invalidate it. After a real guest interrupt
return, the next attempt starts from the returned FLAGS. The record is
appended after existing CPU fields, leaving the JIT's fixed offsets intact.
The distinction between completed iterations and fault-time FLAGS follows
the [Intel REP exception contract](https://cdrdv2-public.intel.com/671200/325462-sdm-vol-1-2abcd-3abcd.pdf).

Verification on 2026-09-07 uses QEMU/KVM, 8 GiB, four CPUs, VNC :12,
UDP 7790 and a disposable 128 MiB snapshot. Tested kernel SHA-256:
`2fd40cca1af5d241db1a650847e7e41db4831ecc53a7a9778a0a9d80bc92fde2`
(5,676,312 bytes), matching the workspace kernel.

- `dos-api-test` passes. Five new suites pass 7,862 checks: ordinary string
  memory (3,420), paging (3,696), boundaries (474), continuation/restart
  (240) and planar-VGA read effects (32).
- Ordinary cases cover real mode, USE16/USE32, both address sizes, all
  element widths, DF directions, segment prefixes, zero/nonzero counts and
  multiple chunks. Comparison flags are checked against width-specific host
  CMP instructions, not another invocation of the guest ALU implementation.
- Paging cases use low/high virtual addresses, noncontiguous physical pages,
  missing/read-only/supervisor PTEs and PDEs, source and destination faults,
  complete saved FLAGS, CR2, error codes, remaining counts, indices, A/D bits,
  memory guards and untouched identity aliases. They observe 2,304 expected
  faults and perform 168 actual DPMI exception returns and successful retries.
  The partial cases complete 300 elements before faulting, crossing the
  internal 256-element boundary without losing the original FLAGS or count.
- Boundary cases check zero count with inaccessible operands, LOCK, 16-bit
  index wrap, protected 32-bit index/linear-address wrap and overlapping MOVS.
  Restart cases distinguish internal chunks, delivered and masked interrupts,
  a changed segment prefix and a code-fetch page fault. VGA cases verify that
  a rejected destination does not consume a new source read latch.
- Existing ModRM paging (7,920), fetch (11,940), descriptor paging (274), page
  walker (5,126), port-string (6,048), DPMI record (1,336), dormant paging
  (308), JIT cache (10,466), CPU/JIT (3,257) and remaining DOS API suites pass.
- Emulated `dpflags16/32`, `dpcbuf16/32`, `dpstate16/32`, `dplock16/32`
  and native `dpcbuf32`/`dplock32` exit 42. Real-mode `dosbits`, `dospop`,
  `dosivt` and the `dosjit` autoload exit 0. Native `dpflags32` still exits
  71h, not a passing FLAGS result.
- The existing `dpvgamem.com` fixture prints `DPMI VGA MEMORY PASS` and exits
  0 in emulated and native profiles. Its 513-element MOVS, CMPS, SCAS and
  backward STOS cases test termination on both sides of a chunk boundary.

End-to-end checks on that same kernel:

- Emulated DOOM `-config audio.cfg -nomusic` renders gameplay and exits 0
  after F10/Y at 650,854,973 instructions, including 205,530,425 JIT
  instructions (205,517,290 protected), with no cache reset. The finalized
  48 kHz stereo WAV contains 63.3015 seconds, 4,617,688 nonzero samples,
  peak 32,432 and RMS 3,778.00. This establishes digital signal, not music,
  audio fidelity or a performance improvement.
- Native DOOM `-nosound` renders gameplay and exits 0, with 13,121,024
  mediated VGA accesses. Both profiles consume four key bytes and deliver
  four IRQ1 events. Their gameplay screenshots were inspected.
- After both runs, PE32 SEH3/SEH4 exit 0 and emulated `dpflags32` exits 42
  again. The full x86 build, DOS exception/FLAGS fixture builds and
  `git diff --check` pass.

Kernel, serial log, WAV and screenshots are retained in
`arch/x86/build/dos-string-verified-20260907/`. Diagnostic QEMU and helpers
are stopped; user VMs and original disk images were not modified. GitNexus
was unavailable. Manual impact review covered CPU initialization/layout,
string helpers, the dispatcher, hardware-interrupt/exception delivery,
native-frame import and their in-kernel tests. No commit was made.

This closes the legacy unchecked memory-string path in the software CPU,
not general DOS compatibility. Ordinary stack operations, multi-word stack
transactions, remaining segment loads and return destinations still need
checked fault/restart behavior. Hidden descriptor state, privileged control
validation, zero-count SHLD/SHRD memory semantics, native FLAGS, KVM ICEBP,
x87 ownership, music and performance remain open. Software fault-FLAGS tests
and successful native VGA transfers do not establish full native exception
FLAGS provenance across MMIO-mediated retries. General guest paging and
complete DPMI/DOS4GW compatibility remain unproven.

### 4.60 Checked data stack and restartable ENTER

The software CPU now checks the data-stack paths for register, immediate,
memory, segment and FLAGS pushes; register, memory and FLAGS pops; PUSHA,
POPA, ENTER and LEAVE. An instruction-local SS view supplies its base,
limit, expand-down range and B-bit address mask. Operand size determines
the transfer width independently of SS.B and address-size prefixes. The
shared physical-address preparation checks complete operands across both
possible pages before data effects. It does not substitute a failed
descriptor lookup with a 16-bit stack or treat a virtual address as a
physical address. SS is resolved once per multi-word instruction, not once
per word; this is not an implementation of hidden descriptor state.

Scalar pushes commit ESP only after the write. Scalar pops read before
changing ESP or the destination, retaining the special POP ESP result and
the post-increment effective address of POP [ESP]. Memory pushes prepare
both operands before reading a potentially side-effecting source. PUSHA
uses the original register values, including SP/ESP. POPA prepares its
whole access sequence before consuming it and does not load the saved
SP/ESP slot into the register. These rules follow the
[80386 stack operations](https://pdos.csail.mit.edu/6.828/2018/readings/i386/PUSH.htm).

ENTER validates all frame sources and destinations before writing, then
executes its frame-copy sequence in order. Overlapping frames therefore
see earlier writes rather than a snapshot of every source. Its final
stack-pointer byte is checked for write permission without changing that
byte or marking its page dirty solely for the probe. The final-byte
requirement is documented in the
[Intel system manual](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/253669.pdf).
LEAVE checks the complete operand at SS:(E)BP before committing either
pointer. LOCK on these data-stack operations delivers the invalid-opcode
exception without consuming the stack.

An expanded regression exposed a separate return bug: a 16-bit DPMI
exception frame contains SP, and the host was replacing all of ESP with
that word. Six ENTER paging/retry cases completed the instruction but lost
the original high word. The failing kernel and serial log are retained in
`arch/x86/build/dos-stack-esp-regression-20260907/` (SHA-256
`dfeb9e0c5545d8f163581b156b96086539f88719ed7e16b43b35fe6d74cfbdbe`).
Each exception depth now retains that unrepresented ESP high word in
private host state. A 16-bit return combines it with the editable SP field;
a 32-bit return continues to use the full public ESP field. Invalid return
destinations retain the private record for a subsequent valid return, and
successful returns release only their own record. The public frame layout
is unchanged; see the
[DPMI exception contract](https://www.delorie.com/djgpp/doc/dpmi/ch4.5.html).

Final verification passed on kernel
`080b2323a53e27c3e0f6f41b7e0a3ae2820a7199f50b406422d9343b9da7141b`
(5,705,248 bytes), built with
`make -s -C arch/x86 CLANG=1 -j4 all dos-exception-test dos-flags-test`.
The only build warning was the existing unused `install_dos_idt_entry`.
The complete `dos-api-test` passed, including the existing page-walker,
ModRM, memory-string, fetch, descriptor, JIT, DPMI, VGA and audio contracts.
New results are:

| Contract | Checks | Controlled faults | Handler returns and retries | Failures |
|---|---:|---:|---:|---:|
| Data-stack memory and permissions | 6,664 | 4,816 | 1,088 | 0 |
| SS limits, expand-down and permissions | 1,440 | 1,200 | 240 | 0 |
| ENTER nesting, allocation and overlap | 3,150 | - | - | 0 |
| ENTER source and final-byte paging | 196 | 112 | 24 | 0 |
| Stack/VGA side effects | 48 | - | - | 0 |
| SS16/SS32 pointer wrap | 340 | - | - | 0 |
| DPMI exception ESP ownership | 32 | - | - | 0 |
| Total | 11,870 | 6,128 | 1,352 | 0 |

The matrices cover real mode, USE16/USE32, independent operand and stack
widths, noncontiguous pages, high linear addresses and PDE crossings.
Permission failures retain CR2, the error code, registers and guarded data;
repair-and-retry cases execute the actual software DPMI handler return.
ENTER checks all nesting values through 31, masked larger immediates,
overlapping frames and its single final-byte probe. Wrap fixtures distinguish
a pointer wrapping between complete operands from an operand crossing a
segment limit. The ESP suite also checks nested records, edited SP/ESP and
rejected then repaired return selectors. Its direct native-frame parser
calls are not native execution of malformed return instructions.

Binary regressions passed for the emulated 16/32-bit FLAGS, callback-buffer,
mode-state and locked-stack fixtures; native callback-buffer and locked-stack
fixtures; real-mode bitops, POP and IVT; and both VGA-memory backends.
The native FLAGS fixture still exits `71h`: it remains a known failure,
not a compatibility success.

On the same kernel, DOOM emulated with `-config audio.cfg -nomusic` reached
visible gameplay and exited through F10/Y at 656,032,317 instructions.
Of those, 203,711,752 were JIT instructions (203,699,160 protected-mode).
The finalized 48 kHz stereo WAV contains 61.4685 seconds, 4,575,660 nonzero
samples, peak 30,925 and RMS 3,817.76. This confirms digital output, not
music support, audio fidelity or a performance improvement. Native DOOM
with `-nosound` also rendered gameplay and exited normally after 13,149,582
mediated VGA accesses. Both runs recorded four host keyboard bytes, four
data reads and four IRQ1 deliveries. Subsequent `seh3_pe32.exe` and
`seh4_pe32.exe` runs exited zero, followed by a successful emulated FLAGS
fixture.

Evidence, including the exact kernel, serial log, gameplay/menu/exit PNGs
and WAV, is in `arch/x86/build/dos-stack-verified-20260907/`. Testing used
the disposable KVM 8 GiB/four-CPU snapshot on VNC `:12` and UDP `7790`;
the diagnostic VM and helpers have terminated. No user VM or original disk
was modified. `git diff --check` passed. GitNexus was unavailable; scope
was checked manually across the shared operand path, stack dispatcher,
exception delivery/return and their regression callers. No commit was made.

Still open: segment-register pops and loads, ordinary CALL/RET/IRET and
interrupt-frame transactions, return-destination checks outside the DPMI
return paths, hidden descriptors, privileged control validation, native
FLAGS and complete short-frame EIP/EFLAGS ownership. Real-mode shutdown
edge cases, zero-count SHLD/SHRD, KVM ICEBP, x87 ownership, music and
performance also remain unproven. The bounded software-memory fixtures do
not establish native processor equivalence for every partial-fault effect.

### 4.61 Checked segment loads and IRQ shadow

MOV to DS/ES/FS/GS/SS and the five segment POP instructions now share the
descriptor validation previously used by LES/LDS/LSS/LFS/LGS. Null data
selectors remain legal to load, but a null SS is rejected. Descriptor type,
RPL/CPL/DPL, table bounds and presence are checked before committing a
selector, GPR or stack pointer. Invalid types take priority over absence;
nonpresent SS uses #SS, nonpresent data segments use #NP, and descriptor
page faults retain their linear address and access error. The accessed-bit
write must also succeed before the load commits. These checks follow the
[80386 segment-load contract](https://pdos.csail.mit.edu/6.828/2018/readings/i386/MOV.htm).

POP validates its old SS and complete operand-sized stack slot before
reading, following the [80386 POP model](https://pdos.csail.mit.edu/6.828/2018/readings/i386/POP.htm).
It then validates the new selector and only commits the increment after
success. POP SS uses the old SS.B for that increment, even when the new SS
has a different address size. This does not establish the narrower segment
POP read behavior of later processors. MOV-to-segment memory sources are always
words. The outgoing MOV writes a word to memory, preserves the upper half
for a 16-bit register destination and zero-extends a 32-bit register
destination. LOCK, MOV-to-CS and invalid segment-register encodings raise
#UD without reading the data operand. See the
[Intel MOV reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf).

Successful MOV/POP SS now delay modeled maskable IRQs across the following
instruction boundary. STI arms this delay only when it enables guest IF;
already-enabled STI and consecutive SS loads do not extend an existing
delay. LSS loads the pair atomically without introducing this delay.
The dispatcher does not enter a multi-instruction JIT block while the
delay is active, and limits a following REP to its first iteration before
offering an IRQ boundary. PIC requests remain pending until delivery is
allowed, and expiry polls them without waiting for the periodic 16K check.
Exceptions and explicit software INT entry clear the delay; native-frame
imports discard the interpreter-only state. The boundary rule is described
in [Intel's interrupt model](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s09_02.htm).

The initial test image completed all old contracts, but 320 new retry
assertions used an inconsistent initial virtual-FLAGS representation.
Those fixtures now start with the existing DPMI representation of IF/IOPL;
the production exception-return FLAGS behavior was not changed to make
them pass. Initial evidence is retained in
`arch/x86/build/dos-segment-initial-20260907/`, kernel SHA-256
`745d7c50a719c576e8ee35b41a9e107161371f45b3066b31e85bc127f8586097`.

The expanded image then passed the load, encoding, privilege and paging
matrices, but its three JIT-boundary assertions incorrectly expected
`cpu8086_run_one` to execute compiled code. That API always disables JIT.
The corrected fixture uses the normal dispatcher, with a stop beyond the
compiled block: the armed shadow must prevent that hot block from running,
and a second pass without the shadow must execute it. Its finite bytecode
also checks instruction accounting. The failed run remains in
`arch/x86/build/dos-segment-expanded-20260907/`, kernel SHA-256
`ca445c87b300f744e1d317f0ba1b304c2c2470603a028774dcc6db613c7d0590`.

The next candidate passed the full API and supported independent probes
(native FLAGS still exited 71h), plus
emulated DOOM at 657,556,029 instructions with digital sound and normal
keyboard exit. Native DOOM nevertheless timed out before its first
nonzero frame. A read-only CPU/PIC snapshot showed virtual IF enabled,
a pending timer tick and IRQ0 still in service. The native IRQ importer
could retain an interpreter IRQ shadow: PIC acknowledgement happened
before the CPU rejected that stale delay, leaving IRQ0 in service without
delivering its handler. A later native import could clear the CPU shadow
without repairing the already consumed PIC state. Evidence from this
failed native run is preserved in
`arch/x86/build/dos-segment-native-stall-20260907/`, including
`native-state.log`, kernel SHA-256
`4cd34fb5bb4c33a37b2c47584870c85f9e853b7ba14aa5f5afebb63bfa4f56b9`.

The native IRQ bridge now discards interpreter-only IRQ and REP
continuations before PIC acknowledgement, just as the native data-frame
importer does. An actual hardware IRQ frame has already passed the native
CPU's interrupt boundary. This fixes stale software state on that import;
it does not implement exporting a newly emulated delay back to hardware.
Synthetic native-frame tests cover USE16/USE32, IRQ0/1/5, shadows 0/1/2,
virtual IF on/off and PIC masks, including untouched frames and no PIC
in-service bit when delivery is denied. All 72 native-import checks pass.
Native DOOM now renders and exits normally with F10/Y, with 13,050,639
VGA-memory mediations and four IRQ1 deliveries.

The corrected matrices pass 38,334 checks, including 3,440 actual handler
returns and retries:

| Contract | Checks | Result |
|---|---:|---|
| Segment loads, old/new stack size, selector faults and repairs | 7,900 | PASS, 3,324 faults and 320 retries |
| MOV segment encodings, widths, GPRs and invalid LOCK forms | 3,456 | PASS |
| All CPL/DPL/RPL values, descriptor types and presence | 20,480 | PASS |
| Cross-page sources, GDT records and accessed-bit writes | 6,240 | PASS, 3,120 faults and retries |
| SS/STI delay, pending IRQs, REP, exceptions, JIT and native imports | 258 | PASS |

Verification uses a disposable KVM snapshot with 8 GiB, four CPUs,
VNC `:12` and UDP `7790`. The candidate was built with
`make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf`;
the 5,687,800-byte copied kernel matches the workspace binary, SHA-256
`2918e05cf3d1c029c7533d586fba337ca4090a645949eb25a6334a264225d6ef`.
The final image passes `dos-api-test`; software/native callback-buffer,
locked-stack and VGA-memory probes; software dormant-state and FLAGS
probes; and DOS bit/POP/IVT probes. `dpflags32.com` still exits 71h natively,
not its success code 2Ah: that known FLAGS gap is not counted as a PASS.
After native DOOM, another emulated run using
`dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` completed normally
at 655,966,781 instructions, including 204,166,136 protected-mode JIT
instructions, zero cache resets and four keyboard IRQ deliveries.
The 300M capture shows gameplay and the 600M capture shows the normal
attract-loop credits. Native gameplay was also inspected visually.
The subsequent `seh3_pe32.exe` and `seh4_pe32.exe` both exit zero, and a
fresh emulated FLAGS probe exits 2Ah.

Evidence is archived in `arch/x86/build/dos-segment-verified-20260907/`:
kernel, serial log, screenshots and a stereo 48 kHz WAV lasting 58.1765 s,
with 4,363,269 nonzero samples and RMS 3,845.737. Its peak magnitude reaches
32,768; this confirms digital PCM output, not an unclipped-fidelity or
music result. The owned VM and all test helpers were stopped and reaped.
User displays `:0`/`:1` and original disk images were not touched.
`git diff --check` passed. GitNexus was unavailable; the manual scope check
covered segment operands, interpreter dispatch, exception entry and the
native timer/keyboard/audio IRQ callers. No commit was made. No complete
DOS or segment-state compatibility claim is made.

Still open: hidden descriptor state, the pre-far-transfer transition after
PE changes, native interruptibility transfer, NMI and complete debug/trap
suppression, ordinary CALL/RET/IRET and interrupt-stack transactions, other
return destinations and privileged controls. Native FLAGS still need their
own correction; music, x87 ownership and performance remain separate work.

### 4.62 Checked near control transfers

Same-CS CALL, RET, JMP, Jcc and LOOP now validate the resulting offset
against the current code-segment limit before committing the transfer.
They reuse the instruction fetch's checked CS view instead of looking up
the descriptor again for every branch. Taken operand-size-16 transfers
clear EIP's upper half, independently of the address size used by indirect
operands or by CX/ECX. A conditional branch that is not taken does not
validate its unused destination. LOCK forms raise #UD before data access.

Near CALL reads an indirect destination using the original ESP, checks
the target offset, then checks and writes the complete return-address
slot before committing ESP and EIP. This target-before-stack ordering
follows the 16/32-bit near CALL operation in the
[Intel instruction reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2a-manual.pdf),
not a speculative execution model. RET first checks and reads its old
SS-sized stack slot, validates the target, then commits the pop and
optional unsigned immediate adjustment together. Operand width and SS.B
remain independent; RET does not access the skipped argument bytes. See
the [80386 RET contract](https://pdos.csail.mit.edu/6.828/2018/readings/i386/RET.htm).

LOOP computes its tentative count locally so that a destination fault
preserves the pre-instruction CX/ECX, including the unmodified high half
with address size 16. Successful fallthrough still decrements the count;
JCXZ/JECXZ never does. The counter and branch widths follow the
[80386 LOOP contract](https://pdos.csail.mit.edu/6.828/2018/readings/i386/LOOP.htm).
A destination within CS but on a missing page is a different case: the
transfer commits first and the subsequent instruction fetch raises #PF
at that new EIP. No speculative target-page read was added to CALL/RET.

The existing JIT source/limit guards already invalidate blocks after CS
changes and send out-of-limit static branches to the interpreter. New
tests exercise that existing path, including a not-taken conditional
branch with an invalid static destination. JIT production code was not
changed or globally disabled for this milestone.

The old LOOP matrix expected an operand-size-32 real-mode branch past
FFFFh to succeed; it now checks #GP and the original count instead. The
first complete near-transfer candidate also exposed 22 analogous
expectations in the old group-5 matrix: real-mode destinations above
FFFFh and CALL/JMP ESP with a value outside the protected CS limit.
That fixture now installs a #GP handler and checks its saved state plus
the surrounding stack bytes, preserving the original input cases.
This is a fixture correction, not removal of the new destination checks.
The complete initial API run failed only those 22 assertions; its 14,116
new near-transfer checks already passed. Evidence is retained in
`arch/x86/build/dos-near-initial-20260907/`, kernel SHA-256
`64f95f8d13a705776bb88774b1d40e6af5f01370273decd98c03a72937bc23fb`.

The next group-5 reference correctly accepted all 22 faults, but its
expanded stack snapshot omitted the legitimate operand write performed
by INC/DEC [ESP]. All 80 such cases failed that snapshot comparison.
The reference now applies the independent arithmetic result to those
operand bytes while retaining the surrounding sentinels. This second
run was stopped early after identifying the fixture error, not counted
as a full regression. Its evidence is in
`arch/x86/build/dos-near-stack-fixture-20260907/`, kernel SHA-256
`25af782e42b760e6f41ee1ef254b11c05f34d10ce10df949ea0299a670b73ef4`.

The final candidate passes all 14,116 new checks. Its branch, stack and
boundary matrices cover real mode, USE16/USE32, both operand/address
sizes, forward/backward branches, exact CS limits, source/stack aliasing,
SS16 high ESP, expand-down stacks, cross-page operands, high linear
addresses, PDE boundaries, page permissions, handler repairs and retries.
The destination-fetch and stale-JIT cases are separate assertions.

| Contract | Checks | Result |
|---|---:|---|
| JMP/Jcc/LOOP, target limits, width and LOCK | 9,120 | PASS, 2,704 expected faults |
| Near CALL/RET, stack limits, paging and repair | 4,720 | PASS, 4,178 faults and 3,648 retries |
| High EIP/ESP, aliased CALL sources, target fetch and stale JIT | 276 | PASS |

The corrected group-5 matrix also passes all 941 cases, including 22
expected #GP deliveries; the existing 2,304 LOOP cases pass. The final
image passes `dos-api-test` in full, including all older stack, segment,
fetch, paging, JIT and I/O matrices. Software/native callback-buffer,
locked-stack and VGA-memory programs pass, as do software dormant-state
and FLAGS programs and the DOS bit/POP/IVT probes. Native `dpflags32.com`
still exits 71h instead of 2Ah; it remains a known gap, not a PASS.

Verification used a disposable KVM snapshot with 8 GiB, four CPUs,
VNC `:12` and UDP `7790`, built with
`make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf`.
The copied 5,704,304-byte kernel matches the workspace binary, SHA-256
`8bcd78e3a2e267f8e2628df42ee41e3c362cbdbd670db5e017631e1d80949acd`.
Native `dosrun DOOM.EXE -nosound` renders gameplay and exits normally
with F10/Y, with 13,048,796 VGA-memory mediations and four keyboard IRQ
deliveries. The subsequent
`dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` run also exits
zero with F10/Y at 651,887,164 instructions, including 204,235,086
protected-mode JIT instructions, zero cache resets and four IRQ1
deliveries. The 300M capture shows gameplay and the 600M capture shows
the normal attract-loop credits. Native gameplay was inspected too.
After both runs, `seh3_pe32.exe` and `seh4_pe32.exe` exit zero, followed
by a fresh emulated FLAGS probe exiting 2Ah.

Evidence is in `arch/x86/build/dos-near-verified-20260907/`: copied
kernel, serial log, screenshots and a stereo 48 kHz WAV lasting
59.79096 s, with 4,484,868 nonzero samples, peak magnitude 32,560 and
RMS 3,852.227. This establishes digital PCM output, not music or full
audio fidelity. All owned VMs and test helpers were stopped and reaped;
user displays `:0`/`:1` and original disk images were untouched.
`git diff --check` passed. GitNexus was unavailable; manual scope review
covered near-transfer dispatch, checked stack/operand helpers, existing
JIT guards and the affected in-kernel tests. No commit was made.

Still open: ordinary far transfers and their selector/accessed-bit
contracts, inter-privilege transitions, IRET and interrupt-stack
transactions, hidden descriptors and the pre-far-transfer state after
changing PE. Native FLAGS (71h), native interruptibility transfer,
NMI/debug suppression, x87 ownership, music and performance remain open.
This is not a claim of complete CPU, DPMI or DOS compatibility.

### 4.63 Checked far calls, jumps and returns

The immediate and indirect far CALL/JMP paths and RETF now share checked
descriptor, stack and page accesses. They validate before committing
CS:EIP or SS:ESP, preserve the original instruction address on a fault,
clear EIP's upper half for operand-size-16 transfers and keep operand
width independent of SS.B. LOCK raises #UD. Complete return frames,
including selector padding, are checked without wrapping their individual
words through the stack-address boundary. Far CALL checks stack capacity
before the destination offset; JMP does not access SS or a TSS.

Code permissions distinguish conforming and nonconforming segments,
normalize CS.RPL on calls/jumps, and validate return privilege separately.
Call gates use their own 16/32-bit width and offset, ignoring the immediate
offset. Inward calls read the appropriate TSS stack, copy up to 31
parameters and save the old stack and return address. Destination stack
page accesses use its new CPL; TSS and descriptor accesses are supervisor
references. RETF restores an outer stack, releases parameters on both
stacks and clears inaccessible data selectors. The new ESP is loaded,
not dereferenced. These rules follow the
[Intel CALL/JMP reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2a-manual.pdf)
and [RET reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf).

All frame destinations are probed before any payload write; saved return
values and copied parameters are collected before the commit. Accessed-bit
writes use the existing supervisor page walker. A failed accessed-bit
write still delivers #PF with its address/error, without changing the
guest return frame or committing CPU registers. Page-table A/D updates
are not rolled back. The validated CS descriptor is used for mode
selection without rereading a table that the frame might alias. Automatic
native entry is limited to a checked USE32 ring-3 target; guest ring-0
code stays in the interpreter. No binary-specific exception was added.

The new in-kernel matrices pass 6,176 cases in the disposable QEMU run:

| Contract | Cases | Result |
|---|---:|---|
| RM/USE16/USE32 far forms, widths, selector/stack faults and LOCK | 2,928 | PASS, 1,816 expected faults |
| CPL/RPL/DPL, conforming/nonconforming code and outer returns | 1,536 | PASS, 976 expected faults |
| 16/32-bit call gates/TSS, SS widths and 0/1/31 parameters | 768 | PASS, including 768 actual RETF returns |
| Paged pointers, frames, GDT reads/accessed writes across a PDE boundary | 336 | PASS, 184 faults repaired and retried |
| Gate/TSS/SS faults, supervisor TSS reads and destination-CPL writes | 192 | PASS, 176 expected faults |
| High EIP/ESP, stack ends, gate JMP and outer-SS validation | 416 | PASS |

The first diagnostic run exposed eight errors in the new real-mode
indirect-transfer fixture: it wrote its pointer at an absolute address
but retained the port-test fixture's nonzero DS. The corrected fixture
sets DS explicitly, retaining all input cases. That run was stopped
before the full suite completed, and is not a full regression PASS.
Its copied kernel/log are retained in
`arch/x86/build/dos-far-initial-20260907/`, SHA-256
`964b12d777d200d87c279062863eb5f45b379b0bc84b5f836e27b06353f3b9e1`.

The final image passes `dos-api-test` in full, not only the new far
matrices. Existing stack, segment, instruction-fetch, near-transfer,
paging, IRQ, I/O and JIT tests all pass. Independent callback-buffer,
locked-stack and VGA-memory probes pass in software/native modes;
software dormant-state and FLAGS probes and DOS bit/POP/IVT programs
pass too. Native `dpflags32.com` still exits 71h instead of 2Ah and is
reported as a known gap, not a PASS.

Verification used a disposable KVM snapshot, 8 GiB/four CPUs, VNC `:12`,
UDP `7790`, built with
`make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf`.
The 5,733,272-byte copied kernel matches the workspace binary, SHA-256
`1ca02d3382ea58cb9d1cc35e27c1f8da75e49c7234c49dcf59248e4f7690ecc2`.
Native `dosrun DOOM.EXE -nosound` renders gameplay and exits zero via
F10/Y, recording 13,086,353 VGA-memory mediations and four keyboard IRQ1
deliveries. The subsequent
`dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` also exits zero
via F10/Y at 655,131,197 instructions, including 205,035,757 protected
JIT instructions, zero cache resets and four IRQ1 deliveries. Native
gameplay and the emulated 300M gameplay/600M attract-loop credits were
visually inspected. After both DOOM runs, `seh3_pe32.exe` and
`seh4_pe32.exe` exit zero, followed by a fresh emulated FLAGS probe
exiting 2Ah.

Evidence is in `arch/x86/build/dos-far-verified-20260907/`: kernel,
serial log, screenshots and a stereo 48 kHz WAV lasting 58.66804 s,
with 4,370,165 nonzero samples, peak magnitude 32,384 and RMS 3,865.885.
This confirms digital PCM output, not music or complete audio fidelity.
All owned VMs and test helpers were stopped and reaped; user displays
`:0`/`:1` and original images were untouched. `git diff --check` passed.
GitNexus was unavailable; manual scope review covered the far dispatch,
shared checked stack/page helpers, CS mode commit, existing JIT guards
and the in-kernel tests. No commit was made.

Remaining dependencies are explicit. Descriptor views, including TR,
LDT and the selectors cleared on an outer return, are still resolved
from tables rather than a complete hidden CPU cache. LTR/LLDT loading,
the pre-far-transfer state after changing PE and edited descriptor tables
therefore need their own follow-up. Task-gate/TSS transfers are not
implemented: they report an explicit diagnostic and reject with #GP,
which is an unsupported-operation fallback, not the architectural task
switch contract. IRET, NT/VM86 task returns and interrupt stack
transactions are unchanged. Native FLAGS (71h), native interruptibility,
NMI/debug suppression, x87 ownership, music and performance remain open.
This is not complete far-transfer, CPU, DPMI or DOS compatibility.

### 4.64 Cached LDTR/TR and checked system-selector loads

The interpreter now stores the hidden descriptor of LDTR and TR separately
from the visible selector. LLDT/LTR validate GDT membership, bounds, type
and presence before committing either part. They require protected mode
and CPL0; invalid encodings, LOCK and real/virtual-8086 execution raise
#UD. LTR accepts available 16/32-bit TSS descriptors and marks them busy
through a supervisor page write before committing TR. It does not switch
tasks, read the TSS body or clear the previously loaded TSS's busy bit.
RPL and descriptor DPL do not restrict these ring-zero loads. Null LLDT
invalidates the guest LDT without selecting the DPMI host's internal LDT.

SLDT/STR remain unprivileged within the modeled legacy protected mode.
Memory destinations are always 16 bits; 32-bit register destinations are
zero-extended (the documented P6 behavior for SLDT). These contracts follow
[Intel LLDT/LTR](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2a-manual.pdf)
and [SLDT/STR](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf).
UMIP and alignment checking are not implemented or claimed here.

LDT entry queries use the cached table base/limit, while call gates obtain
their privilege stack from the cached TSS. Later GDT edits, LGDT or loss
of the old GDT mapping do not reload either cache. The table entries and
TSS stack fields themselves remain live memory. CPU snapshot helpers
distinguish a DPMI-owned LDT from an invalid guest LDT. VCPI saves/restores
both caches and their host ownership along with the visible registers;
its existing physical descriptor-table admission remains unchanged.
Successful DPMI entry and raw RM-to-PM return explicitly select the host
LDT when installing host-owned selectors. Failed initial entry retains
the previous LDTR/cache instead of publishing a partial host context.

The checked descriptor writer is shared with accessed-bit updates. Failed
GDT reads or busy-bit writes retain the previous visible/hidden registers
and deliver the original instruction's #PF/CR2/error. Tests repair the
mapping and retry from the checked interrupted CPU snapshot. Existing
tests that fabricated LDTR/TR now install explicit cached snapshots;
invalid descriptor admission is tested at LLDT/LTR, not by pretending a
loaded register must reread its GDT entry.

The revised system-register matrices and additional cached-TSS cases pass
27,600 cases:

| Contract | Cases | Result |
|---|---:|---|
| LLDT/LTR, all 256 access bytes, RPL, widths, register/DS/SS sources | 24,576 | PASS, 24,000 expected faults |
| Group-6 modes, CPL, LOCK, reserved encodings and all register destinations | 2,368 | PASS, 1,896 expected faults |
| Null/TI selectors, bounds/presence, operand faults and fault priority | 448 | PASS, 272 expected faults |
| Supervisor GDT reads/busy writes, WP and page/PDE/32-bit-wrap boundaries | 168 | PASS, 102 snapshot restarts and DPMI rejections |
| LDT cache survival, actual LGDT/reload/null and live LDT entries | 8 | PASS |
| Additional call-gate cases after GDT edits, replacement or unmapping | 32 | PASS; full gate-fault matrix is now 224 cases |

VCPI also checks both a saved guest descriptor context and a saved
DPMI-owned LDT context across its PM/v86 round trip. Initial DPMI entry
tests cover USE16/USE32 takeover of a previously loaded guest LDT and
preservation of that context when admission fails.

The initial diagnostic had 102 restart-fixture failures: it attempted to
return from a DPMI client handler to the CPL0 code executing LLDT/LTR.
The existing DPMI return-destination validator correctly rejects that
destination. The revised fixture asserts that rejection and preservation
of both caches, then restores the checked interrupted CPU snapshot to
test the repaired instruction separately. Those are snapshot restarts,
not completed guest IDT/IRET returns. No DPMI privilege check was relaxed.
The stopped diagnostic's copied kernel/log remain in
`arch/x86/build/dos-sysreg-initial-20260907/`, SHA-256
`9b5b39417ea7b8a65ab79f4d8d07128f1b3ef7f58411c6fd16d9346a5d36389e`.
It was stopped before the full suite completed and is not a regression PASS.

The revised build passes `dos-api-test` in full, including the existing
fetch, descriptor, segment, stack, near/far transfer, IRQ, I/O and JIT
matrices. Independent callback-buffer, locked-stack and VGA-memory probes
pass in software/native modes; software FLAGS and dormant-state probes
and DOS bit/POP/IVT programs pass as well. Native `dpflags32.com` still
exits 71h rather than 2Ah, explicitly recorded as a known gap.

Verification used a disposable 8 GiB/four-CPU KVM snapshot, VNC `:12`,
UDP `7790`, built with
`make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf`.
The 5,754,016-byte copied kernel matches the workspace binary, SHA-256
`f9b04c43d21ae0fddaa8efc2da450a0b556f1bdb61da589b955865a39c981484`.
Native `dosrun DOOM.EXE -nosound` renders gameplay and exits zero via
F10/Y, with 13,113,085 VGA-memory mediations and four IRQ1 deliveries.
The subsequent
`dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exits zero via
F10/Y at 648,315,452 instructions, including 204,528,864 protected JIT
instructions, zero cache resets and four IRQ1 deliveries. Native gameplay
and the emulated 300M gameplay/600M credits captures were visually inspected.
After both runs, `seh3_pe32.exe` and `seh4_pe32.exe` exit zero, followed
by another emulated FLAGS probe exiting 2Ah.

Evidence is archived in `arch/x86/build/dos-sysreg-verified-20260907/`:
kernel, serial log, screenshots and a stereo 48 kHz WAV lasting 62.65385 s,
with 4,574,386 nonzero samples, peak magnitude 29,969 and RMS 3,763.361.
That confirms digital PCM output, not music or complete audio fidelity.
All owned VMs and helpers were stopped and reaped; user displays `:0`/`:1`
and original images were untouched. `git diff --check` passed and edited
sources retain LF endings. GitNexus was unavailable; manual impact review
covered CPU initialization/dispatch, descriptor lookup and bit writes,
call-gate TSS access, VCPI transitions, DPMI entry/raw return and the tests.
No commit was made.

This is the system-descriptor cache dependency, not complete hidden CPU
state. CS/SS/DS/ES/FS/GS still have table-backed views; preserving their
loaded bases/limits/rights across LLDT, table edits and PE transitions
requires the next segment-cache change, including JIT source validation
and host selector imports. IRET, hardware task switching, NT/VM86 returns,
interrupt-frame transactions and other privileged system instructions
remain open. Native FLAGS still have the previously measured 71h gap;
native interruptibility, NMI/debug suppression, x87 ownership, music and
performance are not resolved by this change.

### 4.65 Privileged mode-control and table-register instructions

The interpreter now checks privilege and invalid encodings for group 7,
MOV CR, CLTS, INVD and WBINVD before changing the virtual CPU. LMSW only
loads CR0's low four bits and cannot clear PE. SMSW keeps its 16-bit
memory destination; this implementation retains the undefined upper half
of a legacy 32-bit register destination. LGDT/LIDT use checked six-byte
records and reject nonzero CPL and VM86; their existing 24/32-bit base
decoding is retained. Unsupported group-7 register encodings and LOCK
raise #UD. INVLPG decodes its effective address without accessing the
named memory or setting that page's A/D bits. CLTS clears TS only.
These contracts follow the [Intel system-instruction reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2a-manual.pdf).

MOV CR always transfers 32 bits and ignores ModRM.mod, without fetching
a SIB or displacement. CR1/5/6/7 and LOCK raise #UD. Invalid PG/PE and
NW/CD combinations raise #GP before CR0 changes; reserved CR0/CR3 bits
are masked. CR2 remains a full 32-bit register. This virtual CPU currently
implements no CR4 extension: reads return zero, zero writes succeed and
nonzero writes raise #GP instead of silently accepting unsupported state.
That is a bounded capability contract, not complete CR4 support. See
[Intel MOV CR and SMSW](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf).

The shared privilege check treats CS's low bits as CPL only after a
protected CS has been loaded. It is also used for LLDT/LTR. A real-mode
CS value remains a paragraph, including between setting PE and the first
protected far transfer. The paging walker has no persistent TLB, and
guest data memory is coherent without a modeled write-back cache; no
host CR, TLB or cache instruction is executed for these guest requests.
The JIT already dispatches these system instructions to the interpreter.
Unconditional LGDT/LIDT/MOV-CR logging and LMSW's speculative next-byte
dump were removed from the affected instruction paths.

VCPI's v86 return uses real segment addressing (`protected_mode=false`)
while retaining CR0.PE and EFLAGS.VM. The privilege check recognizes that
state independently of the interpreter's addressing-mode flag. The
admission matrices include it; the existing v86 exception transport is
not replaced with a complete guest IDT/IRET mechanism here.

The new matrices check admission, unchanged CPU state and memory on
fault, 16/32-bit operands/addresses, all GPRs including ESP, reserved
registers, low-bit LMSW combinations and independent CR4 bits. Paging
cases cover DS/SS, user/supervisor permissions, WP, segment failures and
records crossing a page, PDE and the 32-bit linear wrap. Ring-three
handler returns and explicit snapshot restarts are counted separately.
VM-flag admission tests do not establish complete VM86 address or return
semantics. The transition fixtures execute the next instruction after
CR3 replacement/reload and PG enable/disable, rather than only checking
the written register value. All 16,192 new cases pass in the final
VCPI-aware revision:

| Contract | Cases | Result |
|---|---:|---|
| Group-7 modes, CPL, VM/VCPI-v86, prefixes and register/DS/SS operands | 4,608 | PASS; 3,720 expected faults |
| MOV CR admission, ignored mod/size, all GPRs and reserved registers | 6,528 | PASS; 5,760 expected faults |
| LMSW/CR0 bits, CR4 rejection, CLTS/INVD/WBINVD admission | 1,992 | PASS; 366 expected faults |
| Checked records, paging/segments/WP and INVLPG non-access | 3,024 | PASS; 2,232 expected faults |
| Next-fetch CR3/PG transitions and pre-far-transfer CS values | 40 | PASS |

The paging matrix completes 504 actual ring-three handler returns and
864 explicit snapshot restarts. Ring-zero instructions and repaired
null-source selectors use the latter path; it is not evidence of a
complete guest IDT/IRET return.

The final revision passes `dos-api-test` in full, including all existing
fetch, descriptor, near/far transfer, segment, stack, IRQ, I/O and JIT
matrices. Independent software FLAGS, dormant-state, callback-buffer,
locked-stack, DOS bit/POP/IVT and VGA-memory programs pass; the callback,
locked-stack and VGA-memory probes also pass natively. Native
`dpflags32.com` still exits 71h rather than 2Ah and is recorded as a known
gap, not a passing FLAGS implementation.

Verification used a disposable 8 GiB/four-CPU KVM snapshot on VNC `:12`
and UDP `7790`, built with
`make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf`.
The copied 5,766,480-byte kernel matches the workspace binary, SHA-256
`b26a0dfa1c8bb4313b95b83cf49fc468b62eab94c0aa4bbc208397ce5ce8c8dc`.
Native `dosrun DOOM.EXE -nosound` renders gameplay and exits zero via
F10/Y, with 13,100,646 VGA-memory mediations and four IRQ1 deliveries.
The subsequent
`dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` also exits zero
via F10/Y at 655,983,165 instructions, including 200,130,346 protected
JIT instructions, zero cache resets and four IRQ1 deliveries. Native
gameplay and both emulated 300M/600M gameplay captures were inspected.
After both runs, `seh3_pe32.exe` and `seh4_pe32.exe` exit zero, followed
by another software FLAGS probe exiting 2Ah.

Evidence is in `arch/x86/build/dos-control-verified-20260907/`: kernel,
serial log, screenshots and a stereo 48 kHz WAV lasting 59.64715 seconds,
with 4,445,208 nonzero samples, peak magnitude 31,597 and RMS 3,854.031.
This confirms digital PCM output, not music or full audio fidelity.
All owned VMs/helpers were stopped and reaped; user displays `:0`/`:1`
and original images were untouched. `git diff --check` passed and edited
files retain LF endings. GitNexus was unavailable; manual impact review
covered the interpreter dispatch/public run entry points, checked
operand/page helpers, JIT fallback, the VCPI v86 representation and the
DOS API self-test call chain. No commit was made.

Two preliminary runs were deliberately stopped after the new matrices
to include additional coverage in the next copied kernel. They are not
full-suite passes. `arch/x86/build/dos-control-initial-20260907/` records
14,312 passing new cases before the transition fixture, kernel SHA-256
`02fdfbe250637e3e6d3a46c150a0fe3d9910b15d6600f9d2dfed2d5b684a1136`.
`arch/x86/build/dos-control-transition-20260907/` additionally passes the
40 transition cases, SHA-256
`d1dbacadab52c73c34af9304b75bb2ede8414959dfbff8c252001429ba07657a`.
The latter still lacked the separate real-addressed VCPI v86 admission
state found during review. Both archives contain their copied kernel and
serial log; their helper disconnects were caused by the deliberate VM
shutdown, not an observed guest crash.

The six ordinary segment caches remain table-backed. In particular,
clearing PE still uses the legacy real-mode reset of CS defaults, not a
complete hidden-segment transition. Fetch/scalar page-access privilege
also still derives from CS's low bits; the general pre-far-transfer
paging/CPL case needs the ordinary cached-CS state, beyond the system
instruction admission checks tested here. Guest IDT/IRET, task switching,
general VM86 execution, CR4 extensions, debug-register moves, privileged
HLT, native FLAGS/interruptibility, NMI/debug
suppression, x87 ownership, alignment checking, music and performance
remain separate open work. This change does not complete the DOS layer.

### 4.66 Loaded CS cache and page-access privilege

The software CPU now keeps an explicit loaded CS descriptor and CPL.
Instruction fetch, CS-relative data reads and JIT source validation use
that cache rather than resolving the visible selector for each access.
Far transfers commit the descriptor already validated by the instruction,
including its default size; a descriptor/table edit does not retroactively
change the current code segment. This follows the loaded-register model
in [Intel's segmentation reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-system-programming-manual-325384.pdf).

Clearing or setting PE alone preserves CS's base, limit and default size.
The modeled Pentium-class real-mode far reload changes the base while
retaining the hidden limit and default-size attributes. Real/v86 contexts
created by the host instead have an explicit normal 64-KiB initialization.
The distinction matters for large real-mode code, including JIT block
selection with EIP above 64 KiB. See the mode-switch procedure in
[Intel's initialization reference](https://software.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developers-manual.pdf)
and the real-mode compatibility distinction documented by the
[Bochs implementation](https://github.com/bochs-emu/Bochs/blob/master/bochs/cpu/segment_ctrl_pro.cc).

Page-access privilege uses the explicit CPU CPL, with VM86 at ring three,
not the low bits of a real CS paragraph. Scalar and stack page checks no
longer depend on whether the first protected far transfer has occurred.
The existing guest page walker still has no persistent TLB. JIT validation
continues checking source bytes, device-window exclusions, limits, mode
and default size; changing the backing instructions still invalidates a
compiled block even when CS itself has not been reloaded.

DPMI entry/raw switches, callback and interrupt returns, real-mode vector
dispatch, VCPI v86 return, DOS loading and native frame imports initialize
CS explicitly. Native imports use the host-owned descriptor available at
that boundary; they do not claim to recover an arbitrary physical CPU's
hidden descriptor state from a visible selector. Whole-CPU snapshots copy
the cache. The partial real-mode control-break restore also restores the
saved cache/defaults rather than reconstructing it from a table. The
software INT 31h host return explicitly reloads a preserved caller CS.
The guest IRET implementation is not made fully transactional here.

Existing operand fixtures that assign visible selectors/tables now use
named test-only context-import helpers at their dispatch boundary. Those
helpers are not in the production run path and are not used by the new
cache-lifetime tests. The old query fixture separately initializes its
loaded code width/CPL, rather than keeping a contradictory 16-bit table
entry with a 32-bit CPU default. It also has a bounded failure path.
Descriptor-fetch paging cases now check that an already loaded CS survives
descriptor-page removal; descriptor loads and queries retain their checked
supervisor page-access tests. JIT table-edit tests distinguish a table
mutation from an explicit CS reload.

The new 84 scenarios pass in the final verification run: 48 exercise
host LDT/GDT/guest LDT edits, cached CS-relative reads, interpreter/JIT
agreement and same-selector far reloads; 20 exercise a rejected far load,
actual DPMI handler return, descriptor repair and retry; eight cover PE
changes, real far reloads, snapshots and high-EIP JIT dispatch; eight cover
supervisor fetch/scalar paging before the first protected far transfer.
The complete `dos-api-test` passes, including the 786,500 selector-query
checks and all existing JIT, fetch, descriptor, transfer, stack, segment,
paging, IRQ, I/O and VGA matrices. Independent software FLAGS, callback,
dormant-state, locked-stack, bit/POP/IVT and VGA-memory probes pass;
callback, locked-stack and VGA-memory probes also pass natively. Native
`dpflags32.com` still exits 71h instead of 2Ah: a known gap, not a passing
FLAGS implementation.

Verification used a disposable 8 GiB/four-CPU KVM snapshot on VNC `:12`
and UDP `7790`, built with
`make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf`.
The copied 5,783,040-byte kernel matches the workspace, SHA-256
`830a6fc00ee7540d8fc50e8fde1807f757ceb82922160e4e699965f669373b75`.
Native `dosrun DOOM.EXE -nosound` renders gameplay and exits zero via
F10/Y, with 13,069,908 VGA-memory mediations and four IRQ1 deliveries.
The subsequent
`dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exits zero via
F10/Y at 672,956,988 instructions, including 218,583,393 protected JIT
instructions, zero cache resets and four IRQ1 deliveries. Native gameplay
and both emulated 300M/600M gameplay captures were inspected; the latter
show the demo advancing. After both runs, `seh3_pe32.exe` and
`seh4_pe32.exe` exit zero, followed by another software FLAGS probe
exiting 2Ah.

Evidence is in `arch/x86/build/dos-cs-cache-verified-20260907/`: kernel,
serial log, screenshots and a stereo 48 kHz WAV lasting 52.319604 seconds,
with 3,786,087 nonzero samples, peak magnitude 32,529 and RMS 3,961.414.
This confirms digital PCM output, not music or full audio fidelity.
All owned VMs/helpers were stopped and reaped; user displays `:0`/`:1`
and original images were untouched. The final build is up to date,
`git diff --check` passes, and edited files retain LF endings. GitNexus
was unavailable; manual impact review covered CPU dispatch/fetch and
checked operands, JIT source validity, real/DPMI/VCPI context entry and
return, native frame imports and the DOS self-test fixtures. No commit
was made.

Two preliminary runs were deliberately stopped and are not full-suite
passes. `arch/x86/build/dos-cs-cache-initial-20260907/` exposed the
contradictory USE32 selector-query fixture, kernel SHA-256
`2f242da6a613cf6a4dda0f1c3eb5719df3343b4246d2b1d456832e5955e6057b`.
`arch/x86/build/dos-cs-cache-intermediate-20260907/` passes the then-current
64 cache scenarios and all selector-query cases, SHA-256
`ceab63c90168e66f1f1ba7da5375f92791f1ee1d21cf538dacf3eb4e64d82071`.
It was stopped to include the high-EIP JIT correction and 20 actual
handler-return/retry cases in the final kernel. Both helper disconnects
were caused by deliberate VM shutdown, not an observed guest crash.

SS/DS/ES/FS/GS still use table-backed descriptor views. Their independent
loaded caches, segment-load validation immediately after setting PE, outer
return data-segment invalidation and complete guest IDT/IRET transactions
remain open. This is the first ordinary-cache integration, not all six
segment caches or complete DOS compatibility. Native FLAGS/interruptibility,
task switching, general VM86, CR4 extensions, debug-register moves,
privileged HLT, NMI/debug suppression, x87 ownership, music and performance
remain separate work.

### 4.67 Loaded data and stack segment caches

The software CPU now keeps independent loaded SS/DS/ES/FS/GS descriptors,
alongside CS. Checked scalar, string, port-string and stack operands use
their cached base, limit, expand-down bound and access rights. SS.B comes
from the loaded stack, including POP SS's old-stack increment. Changing a
descriptor, moving a table, or removing its backing page does not reload
a segment. Two registers with the same visible selector may consequently
have different cached bases.

MOV/POP/LxS validate a new protected selector and its accessed-bit write
before committing the visible register/cache. Protected validation starts
as soon as PE is set, not after the first far transfer. Null data selectors
create an unusable cache. Checked call-gate stack changes commit the
descriptor already validated before writing the frame; outer RETF checks
the loaded data privileges and clears unusable/inaccessible registers
without walking their old descriptor tables. Real-mode data reloads retain
the hidden attributes, while the modeled VM86 loads restore 16-bit limits.
The loaded-state and return rules are also visible in the primary
[Bochs segment implementation](https://github.com/bochs-emu/Bochs/blob/master/bochs/cpu/segment_ctrl_pro.cc)
and its [checked memory access implementation](https://github.com/bochs-emu/Bochs/blob/master/bochs/cpu/access.cc).

DOS loading, DPMI initial/raw/callback transitions, locked/exception stack
changes, service-return ES values and VCPI transitions explicitly import
or restore their segment state. Whole CPU snapshots retain every cache;
the partial control-break restore retains SS as well as CS. Native frame
imports use the available host-owned descriptors, not a claimed readout of
arbitrary physical hidden state. The legacy host frame/address helpers are
not converted into complete guest IDT/IRET transactions by this change.

Test-only imports distinguish a prepared protected context from a real
cached context just after setting PE. Query fixtures import DS explicitly,
stack fixtures initialize SS before computing expected stack addresses,
and privileged system fixtures select matching CS/SS privilege levels.
The IRQ fixture imports its protected context before directly injecting an
interrupt, VM86 privilege fixtures use CPL3 stacks, and a repaired null-SS
snapshot restores the original selector including its RPL. The interrupt
aggregator now names a failing subtest and its parameters instead of only
adding otherwise unreported failures to the final total.
The cache-lifetime cases run the actual CPU entry points without those
per-dispatch fixture imports. Their 382 cases cover:

- 240 loaded-cache lifetime cases across both code widths, all five data
  registers, host LDT/GDT/guest LDT and eight table/descriptor mutations.
- 10 independently loaded register pairs sharing the same visible selector.
- 40 rejected loads followed by actual DPMI handler returns and retries
  after repairing only the target descriptor.
- 20 accesses after removing the page containing the loaded descriptor.
- 32 null-selector loads and subsequent checked access failures.
- 10 PE-clear/real-mode reloads retaining the previous limit and SS.B.
- 10 VM86 reloads restoring 16-bit limits and attributes.
- 20 protected data loads after setting PE but before reloading CS.

The existing 768 call-gate returns also check cached data privileges after
editing/removing their backing descriptors, including unusable selectors.
The first complete run reports 113,793 fixture failures; the second reports
146 (two IRQ, 72 control Group 7 and 72 control paging cases). Their logs
are retained in `arch/x86/build/dos-data-cache-initial-20260907/` and
`arch/x86/build/dos-data-cache-second-20260907/`, respectively. The new
382-case matrix passes in the second run; these full-suite failures are
not represented as successful compatibility checks.

Final verification on 2026-09-07 (local date):

- `make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf`
  succeeds. The tested kernel is 5,799,280 bytes, SHA-256
  `ed86bdfb66a06ad7e47a091c4b1e2b398a4c198161878abf00c9d58884639b8f`.
- The complete `dos-api-test` reports PASS, including all 382 new cache
  cases, 768 actual call-gate returns, 4,608 control Group 7 cases and
  3,024 control paging cases. No assertion was removed to clear the
  previously failing fixtures.
- Independent USE16/USE32 software FLAGS, alternate callback buffers,
  0305h/raw-switch state and locked-stack probes exit 2Ah. Native callback
  and locked-stack probes also exit 2Ah; bit, POP, IVT and software/native
  VGA-memory probes exit zero. Native FLAGS still exit 71h, not 2Ah:
  this remains a known gap, not a passing FLAGS compatibility result.
- `dosrun DOOM.EXE -nosound` displays the demo and exits with code 00h
  through F10/Y. `dosrun --emulate DOOM.EXE -config audio.cfg -nomusic`
  displays gameplay at 300M and 600M instructions and exits normally at
  664,191,549 instructions, with 206,459,338 JIT PM instructions and zero
  cache resets. The captures were visually inspected. These runs are not
  a controlled throughput comparison.
- The WAV contains 60.668375 seconds of 48 kHz stereo PCM, 4,618,893
  nonzero samples, peak 32,612 and RMS 3,874.71. This checks digital output,
  not music support or complete audio fidelity.
- After both DOOM runs, `seh3_pe32.exe` and `seh4_pe32.exe` exit zero;
  the software 32-bit FLAGS probe still exits 2Ah.

The final kernel, serial log, WAV and PNG captures are archived in
`arch/x86/build/dos-data-cache-verified-20260907/`. The preceding integration
run is retained in `arch/x86/build/dos-data-cache-integration-20260907/`.
All runs use private disposable 128 MiB images with KVM, 8 GiB RAM, four
vCPUs, VNC `:12` and UDP 7790. Their QEMU processes and test helpers were
stopped/reaped; the original images and user sessions `:0`/`:1` were not
modified. `git diff --check` passes. No commit was made.

Guest IDT/IRET transactions, task switching, full VM86 execution, native
FLAGS/interruptibility, other privileged instructions, alignment checking,
x87 ownership, music and performance remain open. This does not establish
complete DOS or DOS/4GW compatibility.

### 4.68 Checked architectural interrupt returns

The software CPU's IRET/IRETD no longer consumes the return stack with
unchecked pops. It reads the frame using the loaded SS base, limit and B
bit, checks destination descriptors and accessed-bit writes, then commits
CS:EIP, FLAGS and any outer SS:ESP. Protected returns enforce code type,
presence, RPL/DPL and conforming-code rules. Outward returns validate a
writable stack at the new privilege and invalidate inaccessible loaded
data segments without rereading their former descriptor tables. A 16-bit
destination stack preserves the old ESP high word. Flags permissions use
the old CPL/IOPL, not the return CPL. LOCK is rejected before stack access.
These contracts can be compared with the primary
[Bochs protected IRET implementation](https://github.com/bochs-emu/Bochs/blob/master/bochs/cpu/iret.cc).

Real-mode 16-bit IRET now preserves the represented IOPL/NT bits instead
of masking FLAGS down to twelve bits. Real/VM86 pops advance SP/ESP between
fields, allowing stack-address wrap between complete words/dwords but
never splitting an individual out-of-limit operand into wrapped bytes.
Protected IRET checks a contiguous frame. See the primary
[Bochs real IRET path](https://github.com/bochs-emu/Bochs/blob/master/bochs/cpu/ctrl_xfer16.cc)
and [stack operations](https://github.com/bochs-emu/Bochs/blob/master/bochs/cpu/stack.h).
Protected IRETD at CPL0 recognizes
the nine-dword VM86 frame; it loads the real-addressed segment caches,
keeps full ESP and uses the low IP word, following the behavior recorded
in [Bochs' VM86 implementation](https://github.com/bochs-emu/Bochs/blob/master/bochs/cpu/vm8086.cc).
IRET while already in VM86 checks IOPL3, since CR4.VME is not exposed,
and cannot change VM, IOPL or VIP/VIF. VM86 CS reloads also retain modeled
CPL/DPL3. This is a return contract, not general VM86 interrupt support.

Software DPMI still exposes virtual IOPL3 and restores its separate IF.
The default-exception fixture now initializes that advertised flags image
and virtual IF consistently. Its former expectation accidentally depended
on real-mode IRET erasing IOPL. No assertion was dropped. A nested-task
IRET (current NT=1 in protected mode) stops the emulated client with an
explicit unsupported-task diagnostic and exit -1, preserving the return
state. It is not silently interpreted as a stack return or misreported
as an architectural #GP. Task switching remains unimplemented.

The 986 new cases pass in the diagnostic kernel:

- 296 FLAGS cases across real/USE16/USE32, stack widths, CPL/IOPL,
  operand overrides and DPMI virtual IF.
- 40 real/VM86 boundary cases, with 21 expected faults, distinguishing
  wrapped fields, indivisible operands, retained large real SS limits and
  independent SS.B from the operand size.
- 384 outward returns across old/new privilege and stack widths,
  conforming code and contradictory live data/stack descriptors.
- 154 admission/bounds cases, including 136 modeled faults and two
  explicitly unsupported nested-task returns.
- 28 VM86 cases, including 12 privilege/LOCK faults, full frame loads
  and subsequent in-VM86 returns.
- 84 paging cases across a PDE boundary, including supervisor descriptor
  access, accessed-bit write protection and an absent VM86 tail page.
  Forty faults are repaired: 24 return through a real DPMI handler and
  16 restart from checked CPL0 snapshots. DPMI correctly forbids returning
  its client to ring zero; the snapshots do not claim a guest-IDT return.

The initial run is archived in `arch/x86/build/dos-iret-initial-20260908/`.
It recorded 84 old FLAGS-fixture failures and 16 invalid CPL0 DPMI retry
expectations before being stopped during the broader suite. It is not a
completed or passing full-suite run. After fixing those fixtures, the
946-case matrix and default-exception tests pass. That intermediate run
was also stopped to add the real-stack wrap cases; it is retained in
`arch/x86/build/dos-iret-prewrap-20260908/`. The final 986-case matrix
passes with the complete API suite. Neither interrupted run is counted
as a full-suite success.

Final integration evidence is archived in
`arch/x86/build/dos-iret-verified-20260908/`. The tested kernel is 5,823,968
bytes, SHA-256
`f75f8da72c9f2f42d913262e06d0bf9d728d9ce208adb7ffa6b675bae017dce0`.
The isolated KVM run used 8 GiB, four CPUs, a disposable 128 MiB disk,
VNC `:12` and UDP 7790:

- `dos-api-test` finishes with `DOS API contract test: PASS`; the archived
  log has no nonzero assertion-failure counters or mismatch diagnostics.
  The corrected default-exception fixture passes all 1,316 cases.
- The separate USE16/USE32 FLAGS, callback-buffer, dormant-state and
  locked-stack probes pass under emulation. Native callback-buffer and
  locked-stack probes, DOS bit/pop/IVT probes and both VGA-memory routes
  also pass. The native FLAGS probe still exits 71h instead of 2Ah;
  that is an explicit known gap, not a compatibility success.
- `dosrun DOOM.EXE -nosound` displays the demo and exits 00h through
  F10/Y. `dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exits zero
  after 660,603,452 instructions, including 207,707,272 protected JIT
  instructions and zero cache resets. Inspected captures show native
  gameplay, emulated gameplay at 300M, credits at 600M and the quit prompt.
  These are functional checks, not a controlled performance comparison.
- The 48 kHz stereo PCM capture contains 63.0185 seconds, 4,679,398
  nonzero samples, peak 30,951 and overall RMS 3,778.74. Signal is present
  in 53 one-second analysis windows. This establishes digital output,
  not sound fidelity or music support.
- After both DOOM exits, `seh3_pe32.exe` and `seh4_pe32.exe` finish with
  code zero; the emulated 32-bit FLAGS probe still exits 2Ah.

The diagnostic VM and helpers were stopped/reaped after archiving the
kernel, serial log, WAV and screenshots. User sessions `:0`/`:1` and the
original images were not modified. `git diff --check` passes. No commit
was made.

Guest IDT delivery, recursive delivery faults/double faults, task switching,
general VM86 execution, other FLAGS-instruction privilege rules,
NMI/debug interruptibility, native FLAGS, alignment checking, x87 ownership,
music and performance remain separate work. This does not establish
complete DOS or DOS/4GW compatibility.

### 4.69 Checked guest-owned IDT delivery

Raw protected-mode and VCPI clients now deliver interrupts through a checked
guest IDT path rather than unchecked descriptor reads and stack pushes.
The path accepts 16/32-bit interrupt and trap gates, checks table limits,
gate type/presence, software gate DPL and target-code privilege, then uses
the loaded SS or an inward stack from the cached TR. Stack operands honor
SS.B, expand-down limits and paging at the destination CPL. Descriptor
reads and accessed-bit writes use supervisor access. A zero IDTR base is
legal. The primary
[Bochs interrupt implementation](https://github.com/bochs-emu/Bochs/blob/master/bochs/cpu/exception.cc)
provides a contract reference for gate admission and nested-fault classes.

Frame operands are preflighted before stores; CPU control state is committed
only after admission. Cached descriptors are retained through stores that
may alias tables. Software INT saves the following IP, while a delivery
fault resumes at the original instruction. Interrupt gates clear IF;
trap gates retain it. Both clear live TF/NT/RF. Fault-frame RF handling can
be compared with
[QEMU's protected interrupt path](https://github.com/qemu/qemu/blob/master/target/i386/tcg/seg_helper.c).
This does not implement RF's debug-execution semantics or hardware-fidelity
partial stores during unsuccessful event delivery.

Low-level admission faults are captured synchronously, then delivered
without recursive host calls. Selector errors retain the IDT/TI/EXT fields;
page-fault P/W/U errors and CR2 remain distinct. Contributory/contributory
and page-fault/contributory-or-page-fault pairs enter #DF with error zero.
A fault delivering #DF stops only the emulated client, with an explicit
triple-fault diagnostic. Valid task gates also stop with an unsupported-task
diagnostic instead of pretending a stack transfer succeeded. Task switching
is not implemented.

Guest event ownership is separate from the host's virtual v86/DPMI monitor.
VCPI saves/restores it, and an architectural IRETD into VM86 retains it.
Guest VM86 delivery saves the extra segment fields, enters nonconforming
CPL0 code and invalidates data caches until IRETD restores them. Ordinary
INT requires IOPL3; INT3, INTO and ICEBP retain their distinct gate/source
rules. The advertised VCPI protected entry stub remains a host ABI entry;
an ordinary guest INT 67h is not a blanket host-dispatch exception.
Guest-owned IDT contexts cannot enter the native DPMI trampoline.

The JIT's unprefixed INT terminator now selects event ownership at execution
time, including cached real-mode blocks subsequently used in guest VM86.
It shares the checked IDT path and reconstructs the fault IP from the actual
INT, not the block start. It does not disable register-block translation.

All 2,326 new cases pass on the current diagnostic kernel:

- 640 gate entries and real IRET returns across code/stack/gate widths,
  conforming code, CPL transitions and cached 16/32-bit TSS state.
- 768 source cases covering software INT, INT3, INTO, ICEBP, hardware
  delivery, exceptions, operand overrides and LOCK; 288 expected faults.
- 288 admission/boundary cases: 232 expected faults and 16 explicit
  unsupported task gates, plus zero IDTR base and whole-field stack wrap.
- 104 nested-delivery cases, including 24 delivered double faults and
  32 client-local triple-fault stops.
- 78 cross-PDE paging cases for IDT/GDT/TSS/stack access: 32 repaired faults
  return through guest ADD ESP,4 / IRETD and retry the interrupted INT.
  Two intentionally unavailable IDTs triple-fault. No CPL0 snapshot is
  substituted for these page-fault returns. CS/SS accessed-bit writes are
  tested independently, including already-accessed read-only descriptors.
- 320 guest VM86 cases with 252 expected faults and 286 actual returns.
- 128 compiled INT cases with 120 expected privilege faults and 128 actual
  VM86 returns, including cached real-mode blocks, preceding instructions
  and 16-bit IP wrap.

The initial run is archived in `arch/x86/build/dos-idt-initial-20260908/`.
Eight fixture failures retained page granularity while reducing stack
limits; two omitted the second accessed-bit write on a read-only page.
The corrected 2,198-case pre-JIT run passes its new matrices and is archived
in `arch/x86/build/dos-idt-prejit-20260908/`. Both runs were stopped during
the broader suite and are not counted as full-suite successes.

Final integration evidence is archived in
`arch/x86/build/dos-idt-verified-20260908/`. The 5,848,824-byte kernel has SHA-256
`64626b0e1c81907fa5e69b195b5179d4c71900e1ab74c1ac8cf440dc2700fa8c`.
It was built with
`make -C arch/x86 CLANG=1 SKIP_MODEL=1 QUIET=1 -j4 build/kernel.elf`.
The isolated KVM run used 8 GiB, four CPUs, a disposable 128 MiB disk,
VNC `:12` and UDP 7790:

- `dos-api-test` finishes with `DOS API contract test: PASS`. The archived
  log has no nonzero assertion-failure counters or mismatch diagnostics.
- Separate USE16/USE32 FLAGS, callback-buffer, dormant-state and locked-stack
  probes pass under emulation. Native callback/locked-stack, DOS bit/pop/IVT
  and native/emulated VGA-memory probes also pass. Native FLAGS still exit
  71h instead of 2Ah; that remains a known gap, not a successful contract.
- `dosrun DOOM.EXE -nosound` displays gameplay and exits 00h through F10/Y.
  `dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exits zero after
  659,489,340 instructions, including 203,553,163 protected JIT instructions
  and zero cache resets. Inspected native and emulated 300M/600M captures
  show gameplay progressing; the emulated exit-menu capture catches a
  transition to the credits, not a fully drawn quit prompt. Serial output
  independently confirms normal exit after F10/Y. These are functional
  checks, not a controlled performance comparison.
- The 48 kHz stereo PCM capture contains 61.3606875 seconds, 4,553,417
  nonzero samples, peak 31,066 and overall RMS 3,784.67. Signal appears in
  52 one-second analysis windows. This verifies digital output, not music
  support or perceptual sound fidelity.
- After both exits, `seh3_pe32.exe` and `seh4_pe32.exe` finish with code
  zero, and the emulated 32-bit FLAGS probe still exits 2Ah.

The diagnostic VM and helpers were stopped/reaped after archiving the
kernel, serial log, WAV and PNGs. User sessions `:0`/`:1` and original images
were not modified. `git diff --check` passes; no commit was made.

Task switching, general VM86 execution/CR4.VME, real-mode IDTR semantics,
additional FLAGS-instruction privilege rules, NMI/debug interruptibility,
native FLAGS, alignment checks, x87 ownership, music and performance remain
separate work. This does not establish complete DOS/DOS4GW compatibility.

### 4.70 Checked 16/32-bit hardware task switches

The interpreter now shares one TSS engine between far CALL/JMP, GDT/LDT
task gates, IDT task gates and NT/backlink IRET. Those paths no longer
report an unsupported task switch. This is guest CPU state management,
not a change to the native scheduler or the Win32 execution layer.

The architectural references are Intel's
[task management and exception contracts](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf)
(Volume 3A, sections 6 and 7), its
[debug-register and task-trap definitions](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3b-part-2-manual.pdf)
(Volume 3B, section 17), and the phase separation in
[QEMU's task-switch implementation](https://github.com/qemu/qemu/blob/master/target/i386/tcg/seg_helper.c).
Gate privilege checks apply to the gate, while a direct TSS transfer checks
the TSS DPL. CALL/JMP require an available global TSS; IRET requires a busy
backlink target. Type/table/limit/presence failures retain their appropriate
GP/TS/NP/SS classification. Task faults after the commit point belong to
the incoming task, including its new paging context.

Implementation details:

- The loaded TR cache supplies the outgoing TSS base, type and limit;
  modifying its live descriptor does not redirect the saved register image.
  Only dynamic TSS fields are saved. CR3, LDTR, privilege stacks, I/O map
  state and reserved words remain software-owned static fields.
- Supervisor page probes cover outgoing saves, incoming reads, backlink
  writes and busy-bit changes before any task-state store. Split guest
  pages use checked physical addresses, never a host pointer cast. Busy
  changes preserve unrelated bits in the live descriptor access byte.
- CALL/interrupt tasks link to the suspended task and set NT. JMP preserves
  the loaded NT value without nesting. IRET clears NT in the outgoing image,
  clears that task's busy bit, and does not read an interrupt stack frame.
  The incoming TSS width, CS.D and SS.B are independent.
- Commit installs TR, CR0.TS, registers, FLAGS and all visible selectors.
  A 32-bit TSS loads CR3 only while paging is enabled; a 16-bit TSS retains
  it. New LDTR and segment descriptors are then qualified under that address
  space. Unqualified caches cannot retain the outgoing task's descriptors.
  This emulator zero-extends the undefined upper GPR halves of 16-bit TSS
  loads and clears the unavailable FS/GS selectors.
- VM86 tasks receive real-mode segment bases/limits, CPL3 and guest IDT
  ownership. A protected interrupt task can suspend VM86 and IRET back
  through the busy backlink without transferring into a native trampoline.
- Fault capture now retains the actual fault-return EIP. Secondary IDT
  faults therefore save the incoming task's EIP after a task commit, not
  the old CALL/INT address. Contributory faults still combine into DF.
- Successful switches clear local breakpoint enables. TSS.T sets DR6.BT,
  clears DR7.GD and starts a separate DB event after any error-code push.
  In particular, a task-switch debug trap after successfully entering a DF
  task must not be misclassified as a triple fault.

GitNexus tools were unavailable. Manual call-site analysis scoped edits to
`cpu8086.c`, its transient event-fault type, the exception capture in
`dos_int.c`, and in-OS tests. No JIT ABI offsets were changed.

The new test matrices cover:

- 512 direct/gated/interrupt switches across TSS, code and stack widths and
  all CPLs, including 384 actual nested-task IRET returns and unchanged
  static/reserved TSS bytes.
- 160 pre-commit admission faults, preserving both task images.
- 209 post-commit faults recovered through a separate exception task,
  including 86 double-fault deliveries and the new task's saved EIP.
- 48 VM86 task entries, instruction execution, interrupt-task suspensions
  and returns to VM86.
- 72 debug/cache cases, including a TSS.T trap following DF delivery,
  IRET-triggered traps, edited live TR descriptors, LDT segments, conforming
  code and null data selectors.
- 90 cross-page TSS/GDT cases: 30 supervisor faults repaired and retried,
  with read-only and supervisor pages and CR0.WP variations.
- 15 separate-address-space cases, with 12 faults under the incoming CR3
  and descriptor accessed-bit writes to the incoming physical LDT.
- 48 JIT/task-gate cases, including 24 actual task returns, preceding
  instructions, busy targets, VM86 IP wrap and full protected EIP across
  the 64-KiB boundary. Protected INT retains its interpreter handoff; VM86
  INT is compiled and uses runtime IDT ownership. CS.D does not select the
  saved EIP width of a 32-bit TSS.

The initial run is preserved in `arch/x86/build/dos-task-initial-20260908/`.
Its four failing stack-limit fixtures retained page granularity. The
corrected 1,106-case pre-final run is preserved in
`arch/x86/build/dos-task-predebug-20260908/`; its new matrices pass. Both
runs were stopped during the broader suite and are not full-suite passes.
`arch/x86/build/dos-task-jit-initial-20260908/` preserves a further partial
run: its 32 new JIT cases incorrectly expected protected INT to be compiled
instead of testing the existing interpreter handoff. The corrected matrix
keeps that policy and adds direct compiled VM86 INT/task-gate coverage.
`arch/x86/build/dos-task-jit-boundary-20260908/` retains the partial run
with four fixture expectations incorrectly truncating protected EIP for
USE16 code. The corrected fixture keeps full protected EIP and gives its
code segment a sufficient limit for the actual nested-task return.

Final integration evidence is archived in
`arch/x86/build/dos-task-verified-20260908/`. The pinned 5,877,800-byte
kernel has SHA-256
`bf9d4eedd7e70e2d6cd17577bc7c9f00333ac7bfa2b0c498d8717176fd3235d9`,
matching the build output. Build command:
`make -C arch/x86 CLANG=1 SKIP_MODEL=1 QUIET=1 -j4 build/kernel.elf`.
The isolated QEMU run used KVM, 8 GiB, four CPUs, a disposable 128-MiB
OsitoFS3 image, VNC `:12` and UDP 7790.

- All 1,154 new checks pass, including the DR7.GD adjustment and compiled
  VM86 task-gate cases. The complete `dos-api-test` reports PASS at serial
  line 189002, with no nonzero assertion-failure counters or mismatch
  diagnostics. Existing IDT, IRET, paging, descriptor, cache, fetch,
  transfer, stack, segment, IRQ, I/O, keyboard and JIT matrices also pass.
- The independent emulated 16/32-bit FLAGS, callback-buffer, mode-state
  and locked-stack probes return 42. Native callback and locked-stack
  probes return 2Ah; bit/POP/IVT and native/emulated VGA probes pass.
  Native FLAGS still returns 71h instead of 2Ah. This is the retained
  known gap, not a passing FLAGS test.
- `dosrun DOOM.EXE -nosound` runs the native demo and exits normally with
  code 0 after F10/Y. Four keyboard bytes are consumed through four IRQ1
  deliveries, and the shell is usable afterward.
- `dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exits normally
  after 655,131,196 instructions, including 209,851,278 protected JIT
  instructions, with zero cache resets. Four keyboard bytes and four
  IRQ1 deliveries confirm the F10/Y path here too.
- Inspected captures show native gameplay, emulated gameplay at 300M,
  credits at 600M, both quit prompts and both returns to the shell.
  Digital PCM is 48-kHz stereo for 68.699792 seconds, with 4,909,572
  nonzero samples, peak 32,283 and RMS 3,678.17. Signal is present in 56
  of 69 analysis windows. These are digital-output checks, not music or
  acoustic-fidelity certification.
- After both DOS sessions, `seh3_pe32.exe` and `seh4_pe32.exe` return 0;
  emulated `dpflags32.com` returns 42 again.

The diagnostic VM was stopped before collecting the final WAV. The kernel,
serial log, WAV and PNGs are archived, and all helper sessions have been
reaped. User sessions `:0`/`:1` and original images were not modified.
`git diff --check` passes; no commit was made.

General VM86/CR4.VME execution, real-mode IDTR semantics, additional FLAGS
privilege rules, NMI/general debug interruptibility, native FLAGS,
alignment checks, x87 ownership, music and performance remain separate
work. This is not a claim of complete DOS/DOS4GW compatibility.

### 4.71. Checked FLAGS privileges and DPMI interrupt ownership

The software CPU now applies the FLAGS privilege rules of its represented
CPU profile to PUSHF/POPF, CLI and STI. POPF/POPFD changes IOPL only at
CPL0 and changes IF only when the old IOPL admits the current CPL. A word
POP preserves upper flags except RF; both widths clear RF and cannot load
VM, VIF or VIP from the operand. AC and ID are writable only for POPFD.
PUSHFD clears RF and VM in its saved image without changing the shared
flags-image helper used by interrupt and TSS frames. These rules follow
the [Intel instruction reference, POPF table 4-15 and PUSHF](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf).

Without VME/PVI, which this CPU does not advertise, VM86 PUSHF/POPF at
IOPL below 3 and guest CLI/STI below the required I/O privilege raise
GP(0). The admission check runs before accessing the stack. LOCK on the
FLAGS instructions raises UD before privilege, segment or paging checks;
this includes SAHF, LAHF, CMC, CLC/STC and CLD/STD. LAHF constructs the
defined low-byte image. Successful STI retains the one-instruction
interrupt shadow only on a disabled-to-enabled transition; another STI
does not extend it.

An active DPMI host no longer steals CLI/STI from a guest-owned IDT.
Host DPMI continues to use its separate virtual IF, while guest-IDT
contexts use the guest's actual IF and CPL/IOPL. No CPU structure layout,
JIT ABI, native IOPL or default backend policy changes were made. The
existing checked-stack test model was corrected to preserve flags that
POPF cannot modify; it no longer expects VM/VIF/VIP to be copied from
arbitrary stack bytes.

Seven new in-OS matrices add 2,623 checks:

- 1,152 stack-image cases across real mode, protected CPL0-3 and VM86,
  operand/stack widths, all IOPLs and four flags patterns; 48 VM86 GP
  frames are inspected without changing the operand or suspended IF.
- 160 CLI/STI cases, including 60 privilege faults, physical versus host
  virtual IF ownership and non-extending consecutive STI shadows.
- 66 invalid LOCK cases, including competition with a VM86 privilege
  failure and an unusable operand segment.
- 16 host-profile sequences covering physical IOPL0 and virtual IOPL3,
  PUSHF/CLI/POPF/STI, both client widths and both virtual IF values.
  These are interpreter mediation tests, not native FLAGS conformance.
- 68 cross-page cases with 44 faults and 36 repairs followed by guest
  ADD ESP,4 / IRETD and an actual retry. Complete operand snapshots check
  that rejected pushes do not partially write. VM86 privilege and LOCK
  rejections win over absent operand pages.
- 128 cold/hot JIT cases execute a compiled NOP and hand the FLAGS
  instruction to the checked interpreter; 32 guest GP frames are checked.
- 1,033 byte/IRQ cases exhaust AH values for SAHF/LAHF and verify IRQ0
  blocking, six real deliveries after the shadow and the CLI case that
  keeps the IRQ pending despite a suspended DPMI host.

The initial run, archived in
`arch/x86/build/dos-flags-initial-20260908/`, passed all new matrices
except 12 protected 16-bit paging-return expectations. The fixture had
assumed IRETD restored the entire old ESP, but a protected destination
SS with B=0 retains the handler's high ESP word. This is the existing
contract, also represented by
[Bochs' protected return](https://github.com/bochs-emu/Bochs/blob/master/bochs/cpu/iret.cc)
and described by [Linux ESPFIX](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/espfix_64.c).
Only that test expectation was corrected. VM86 still requires restoration
of full ESP. The partial run was stopped and is not a full-suite PASS.

Final verification is archived in
`arch/x86/build/dos-flags-verified-20260908/`. The pinned 5,894,352-byte
kernel has SHA-256
`e82eefac25a9d473a13cfcf9cd4416a21a6d444e1464ea72aa3758e66f92436b`,
matching the build output. Build command:
`make -C arch/x86 CLANG=1 SKIP_MODEL=1 QUIET=1 -j4 build/kernel.elf`.
The isolated run used KVM, 8 GiB, four CPUs, a disposable 128-MiB OsitoFS3
image, VNC `:12` and UDP 7790.

- All 2,623 new checks pass. The complete `dos-api-test` reports PASS at
  serial line 191566; there are no nonzero assertion-failure counters or
  mismatch diagnostics. The existing TSS, IDT, IRET, system/control,
  paging, port, memory, stack, segment, fetch and JIT matrices also pass.
- Fifteen independent probes pass: emulated 16/32-bit FLAGS, callback
  buffers, mode-state and locked stacks; native callbacks/locked stacks;
  bit operations, POP, IVT and native/emulated VGA. Native FLAGS remains
  the known 71h result instead of the software-profile 2Ah result.
- Native `dosrun DOOM.EXE -nosound` runs the demo and exits with code 0
  after F10/Y, with four keyboard bytes and four IRQ1 deliveries.
- `dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exits normally
  after 670,728,764 instructions, including 214,032,760 protected JIT
  instructions, with zero cache resets. Four keyboard bytes and four
  IRQ1 deliveries confirm the normal exit path here too.
- Inspected captures show native gameplay, emulated gameplay at 300M
  and 600M, both quit prompts and both returns to the shell. Digital PCM
  contains 51.8814375 seconds of 48-kHz stereo, 3,814,123 nonzero samples,
  peak 31,919 and RMS 3,949.82, with signal in 43 of 52 analysis windows.
  This verifies digital output, not music or acoustic fidelity.
- After both DOS sessions, `seh3_pe32.exe` and `seh4_pe32.exe` return 0,
  and emulated `dpflags32.com` returns 42 again.

The diagnostic VM was stopped before collecting the final WAV. Its
kernel, serial log, WAV, PNGs and machine-readable summary are archived;
all helper sessions were reaped. User sessions `:0`/`:1` and original
images were not changed. `git diff --check` passes; no commit was made.

This does not virtualize native PUSHF/POPF. The existing native FLAGS
probe tests a legacy software-IOPL3 contract and intentionally remains
separate from native DPMI's physical/virtual IF separation, permitted by
the [DPMI 1.0 virtual interrupt contract](https://docs.pcjs.org/specs/dpmi/1991_03_12-DPMI_Spec_v10.pdf#page=28).
Supporting that legacy contract natively still requires complete execution
translation or virtualization, not elevated host IOPL, unconditional
interrupt re-enabling or a game-specific patch. General TF/RF retirement,
debug/NMI interruptibility, VME/PVI, alignment checks, real-mode IDTR,
x87 ownership, music and performance remain open. This is not complete
DOS or DOS/4GW compatibility.

## 4.72 Checked real-mode interrupt entry and ROM ownership

Raw real CPU interrupts now share one checked path between the interpreter,
hardware/exception delivery and JIT INT exits. The initial IDTR is
base zero, limit `03FFh`; subsequent entries use its current base and
limit. The four-byte vector must fit before any frame store. Real INT
always stores FLAGS, CS and IP as three words, including with `66h`,
and never appends an exception error code. This follows Intel's
[real-address interrupt contract](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2a-manual.pdf)
and [IDTR initialization](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3b-part-2-manual.pdf).

The frame uses the loaded SS base, limit and B bit. All three slots are
validated before writing, including per-word wrapping on a 16-bit stack.
Entry preserves loaded CS attributes, checks its target limit and clears
IF/TF/AC/RF. Same-physical-address IVT/frame aliases are resolved in push
order before committing the frame, matching the vector-after-push order
in [Bochs real-mode delivery](https://github.com/bochs-emu/Bochs/blob/master/bochs/cpu/exception.cc).
Delivery faults use the existing nested-exception machinery; an
undeliverable double fault stops this DOS client, not OsitoK.

The BIOS/DOS optimization is now conditional on the actual installed
vector and intact service-stub bytes. Replaced INT 21h vectors, other
guest hooks in ROM and raw `0000:0000` destinations execute as guest
code. Reusing a JIT block does not cache the interrupt destination or
discard an IDTR change. The old duplicate unchecked JIT frame path is
removed; protected INT instructions still follow the decoder's existing
interpreter boundary.

DPMI entry, state/mode switching, callbacks and the ROM bridge remain
private host calls only at their recognized instruction origins. Hidden
CS bases participate in real-call provenance. The ROM bridge validates
the outer FLAGS buffer against loaded SS and backing memory, including
32-bit stacks, before calling a service. Reflection releases the local
fault recorder before reentering the interpreter. DPMI's default divide
exception explicitly rejects a missing DOS handler instead of relying
on the raw CPU to ignore a zero vector. Direct host API dispatch and
the protected DPMI translator retain their separate contracts.

DPMI's installed protected INT 1Ch handler is an explicit host-owned
software event, separate from raw IDTR delivery. It takes precedence over
the real vector, including after LIDT; default protected vectors and an
inactive DPMI host do not intercept it. This preserves the
[DPMI software-interrupt rule](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf),
not a game-specific exception. Default PM chaining still calls the saved
real handler directly, allowing a nested INT 1Ch to reflect again.

The new matrices contain 610 checks: 480 real entries/IRETs, 42 boundary
and delivery-fault cases, 40 cold/hot JIT cases, 16 host/provenance cases
and 32 DPMI timer-ownership cases. The latter execute actual interpreter
and JIT INT instructions, not only the reflection helper, with USE16/USE32
handlers and 16/32-bit real stacks. `dos_ivt_contract.S` additionally
installs and chains an INT 21h
hook, executes LIDT with a copied IVT and invokes both INT and `66 INT`
through its relocated vector. Older fixtures now install their BIOS/DOS
vectors explicitly. Their invalid-CS return case expects the resulting
client-local triple fault; VM86 admission-priority tests capture the
first fault before trying to deliver it onto their deliberately invalid
stack. The fixture preserves its caller's emulation/interrupt metadata.

An earlier candidate passed the complete API suite but looped in
`dpstate16.com`: admitting the PM timer only at the default ROM vector
let the installed real timer recursively call itself. The retained log
is `arch/x86/build/dos-real-idt-timer-regression-20260908/serial.log`.
Separating DPMI ownership fixes that regression; the independent
`dpstate16.com`, `dpstate32.com` and extended `dosivt.com` probes now pass.
The new interpreter timer fixture initially expected `run_one` to count
only its INT, although the host service also retires MOV/IRET in the PM
handler. It now checks the three-instruction total and restored CPU/host
state; the corresponding JIT cases already passed. That partial run is
retained as `arch/x86/build/dos-real-idt-timer-fixture-20260908/`.

Verified on an isolated KVM VM at `:12`, 8 GiB/4 CPUs, with a disposable
128 MiB image. The final kernel has 5,906,912
bytes and SHA-256
`71e7789f4e9aff3ad1db79d0e34ce0891f75af7dd2c78e76f7d6af38bab3b988`.
Build command: `make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf`.

- All 610 new checks and the complete API suite pass. The final log has
  no nonzero assertion-failure counters or mismatch diagnostics.
- Fifteen independent DOS probes pass, including USE16/USE32 state,
  callback and locked-stack tests, the extended IVT probe and native/
  emulated VGA memory tests. Native FLAGS still exits 71h, recorded as a
  known profile gap rather than a passing 2Ah result.
- Native `dosrun DOOM.EXE -nosound` exits normally with code zero after
  F10/Y. Four keyboard bytes produce four IRQ1 deliveries.
- `dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exits normally
  after 667,124,284 instructions, including 210,530,387 protected JIT
  instructions, with zero cache resets and four IRQ1 deliveries.
- Subsequent `seh3_pe32.exe` and `seh4_pe32.exe` exit zero; the emulated
  FLAGS probe still exits 42.
- Captured PCM is 48 kHz stereo, 53.1274375 seconds, with 3,928,358 nonzero
  samples, peak 28,321 and RMS 4,008.28. This measures digital output,
  not music support or acoustic fidelity.

Native gameplay, emulated gameplay at 300M/600M, both quit prompts and
both shell returns were visually checked. Kernel, serial log, screenshots,
WAV and the structured verification summary are retained in
`arch/x86/build/dos-real-idt-verified-20260908/`. The archived kernel hash
matches the build. The diagnostic VM and helpers are stopped; user
sessions `:0` and `:1` were not touched. `git diff --check` passes.

This does not complete DOS compatibility. Native legacy FLAGS remains
the separate 71h profile gap. General TF/RF retirement, debug/NMI
interruptibility, VME/PVI, alignment/A20 behavior, full VM86 execution,
x87 ownership, music and performance remain open. DPMI's higher-level
real-mode reflection policies are distinct from raw CPU IDTR delivery;
this is not a claim that every remaining ROM-range policy is removed.
Instruction-level INT 23h/24h reflection needs its own follow-up: existing
helper-level control-break/critical-error checks do not prove that route.

### 4.73 Checked real-mode Ctrl+C and critical-error reflection

Real-mode software INT 23h and INT 24h now use explicit DPMI ownership,
alongside the existing installed INT 1Ch handler. Interpreter and JIT
instructions reach the protected handlers even with a real hook or a
relocated IDTR. Inactive DPMI and guest-owned IDTs retain CPU delivery.
The default protected policies are Ignore for Ctrl+C and Fail for critical
errors, as specified by the
[MS-DOS DPMI extensions, section 12](https://docs.pcjs.org/specs/dpmi/1991_03_11-MSDOS_DPMI_EXTENSIONS.pdf).
This extends host reflection, not the list of ordinary BIOS translators.

The handlers run on the shared locked stack. A raw software event retains
its incoming virtual IF; a DOS service-origin notification retains the
existing DOS interrupt-disabled context. Ctrl+C ignores returned CF and
accepts balanced IRET or RETF. Raw reflection returns general registers;
the DOS service wrapper preserves its interrupted request. INT 24h still
requires IRET and maps Abort/unknown actions to Fail. Its protected IRET
has the client's width, but the following nine DOS registers and caller
IRET remain twelve words with real-mode segment values. SS:(E)BP identifies
that mixed frame. These rules follow the
[DPMI software-interrupt contract](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf)
and the DOS extensions, not executable-specific handling.

The critical frame is snapshotted through the loaded real SS before
switching to protected paging. The reader respects SS.B, hidden base and
limit, word-boundary wrapping and non-contiguous pages. The destination
uses the checked fixed-record buffer API; missing, supervisor-only or
read-only destination pages reject the frame before partial stores.
Invalid critical frames produce a diagnostic and Fail, without consuming
the suspended stack or retaining temporary fault-delivery state.

INT 31h/0300h control-event simulation no longer inserts a FAR trampoline
return between its IRET and copied DOS words. Argument copying also skips
the interpreter's private INT 31h frame; native gates have no such frame
on the client stack. This corrects the argument start for 0300h/0301h/0302h
without changing the suspended protected stack.

Added coverage comprises 176 interpreter/JIT control-event checks and
108 paged-frame checks. The former cover both handler and real-stack
widths, default/null vectors, real hooks, invalid IDTR, IF state, stack
wrapping, truncated input and response/return policies. The latter use
different real/protected page tables, mismatched visible/hidden SS bases,
split source/destination pages and denied source/destination accesses.
The extended `dpmi_critical.S` and `dpmi_break.S` probes exercise actual
real INTs through 0301h and direct 0300h, including copied arguments and
both IF/return forms, in emulated USE16/USE32 and native USE32.

Initial probe failures are retained separately:
`arch/x86/build/dos-control-fixture-20260908/` exposed legacy fixture
writes through read-only CS, now replaced with data-selector stores;
`arch/x86/build/dos-control-arguments-20260908/` exposed the interpreted
INT 31h argument offset. Neither is counted as a passing run.

Verified on an isolated KVM VM at `:12`, with 8 GiB/4 CPUs and a disposable
128 MiB disk. Kernel size is 5,915,400 bytes, SHA-256
`3b748ab48422b52d6073edac5f2d193734d4a9bb5f8008ef8ab9400879f28942`.
Build command: `make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf`
plus the four `build/test/dpcrit{16,32}.com` and
`build/test/dpbreak{16,32}.com` targets.

- All 284 new checks and the complete API suite pass. The paging reentry
  suite now has 416 checks, up from 308, with zero failures. The final
  serial log contains no nonzero assertion-failure counters or mismatch
  diagnostics; the API PASS is at line 193634.
- Six extended control/critical executable runs and fifteen other
  independent probes pass. Native FLAGS still exits 71h, explicitly a
  known profile gap, not a passing 2Ah result.
- A framebuffer-worker diagnostic interrupted the VGA probe's PASS text.
  Its process exited zero; after stripping only that diagnostic in the
  observer, both emulated and native VGA probes were rerun successfully.
- Native `dosrun DOOM.EXE -nosound` exits zero via F10/Y; four keyboard
  bytes produce four IRQ1 deliveries.
- `dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exits zero after
  670,646,844 instructions, with 214,647,684 protected JIT instructions,
  zero cache resets and four IRQ1 deliveries.
- Subsequent `seh3_pe32.exe` and `seh4_pe32.exe` exit zero. Emulated
  `dpflags32.com` still exits 42 after returning from both DOOM sessions.
- PCM capture is 48 kHz stereo, 51.0143125 seconds, with 3,721,935 nonzero
  samples, peak 29,998 and RMS 3,940.72. This verifies digital signal,
  not music compatibility or acoustic fidelity.

All eight screenshots were visually checked: native gameplay, both quit
prompts and shell returns, and emulated title/gameplay at 300M/600M.
The existing unscaled 320x200 presentation and text-mode box-drawing font
gap remain visible, not introduced or fixed here. Kernel, serial log,
four COM probes, screenshots, WAV and `verification-summary.json` are
retained in `arch/x86/build/dos-control-verified-20260908/`. The archived
kernel matches the build. The diagnostic VM and all helpers are stopped;
user sessions `:0` and `:1` were untouched. `git diff --check` passes.

This is not complete DOS compatibility. General paged argument copying
for arbitrary-length 0300h/0301h/0302h calls, nested raw-mode hidden-state
coverage, general TF/RF retirement, debug/NMI interruptibility, native
legacy FLAGS, VME/PVI, full VM86, x87 ownership, music and performance
remain separate work. The fixed-record critical-frame validation does
not claim to normalize every legacy host stack helper.

### 4.74 Checked arbitrary-length DPMI real-call arguments

The 0300h/0301h/0302h stack path no longer copies through unchecked
linear addresses. It validates the loaded protected SS cache (including
SS.B, expand-down limits and linear overflow), then captures every source
page under protected CR0/CR3 and every destination page under the dormant
real address space. Translation probes do not publish a mode switch or
write stack payload/A/D metadata when admission fails.

The page list is bounded by the real stack's architectural 64 KiB span,
including a possible seventeenth page from linear misalignment. It is
not the 64-byte fixed-record buffer limit. After admission, the host
commits captured page metadata, snapshots all argument bytes into a
temporary host buffer, then publishes the complete real frame and its
arguments. This handles non-contiguous pages and physical overlap even
when virtual ranges suggest no overlap. Nonempty arguments allocate at
most sixteen temporary host pages, freed before executing the real
procedure; zero-argument calls allocate no scratch pages.

0300h now uses the same six-byte IRET layout as 0302h, including ordinary
real IVT handlers rather than only protected control-event reflection.
0301h uses a four-byte FAR frame. Canonical host services consume the
prevalidated IRET through the existing CPU service bridge. Other vectors
execute their guest code; an arbitrary ROM address is not a host-service
fallback. Return validation checks the original SS and the expected SP,
without accepting removal of the copied arguments. A balanced RETF 2 is
accepted for 0302h. These call/return rules follow functions 0300h-0302h
in the [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf).

Stack translation failures report 8012h, scratch allocation failure
reports 8013h, and invalid stack spans, targets or return balance report
8021h. The existing output-record path still revalidates the caller's
selector/mapping after callbacks. This work does not make the entire
service rollback-atomic: reading/writing that separate register record
has its own access and A/D semantics.

The new internal matrix has 324 checks. It covers USE16/USE32 stacks,
zero/two/8,208/65,528-byte arguments, separate page directories, rotated
physical aliases, hidden SS base changes, default real stacks, readable
but read-only source pages, supervisor WP=0 writes, expand-down stacks,
missing first/last pages, denied permissions, absent backing, invalid
descriptors, overflow and oversized CX. Admission-failure checks compare
the complete CPU/register record plus stack sentinels and page metadata.
The old FAR fixture was corrected to inspect rather than remove its
argument, matching caller cleanup.

`make -C arch/x86 dos-rm-arguments-test` builds `dpargs16.com` and
`dpargs32.com` from `test/dpmi_rm_arguments.S`. The binaries pass 2,304
argument words through all three services, IRET and RETF 2, and a PM
callback that makes a nested 0301h call with its own argument. They check
every returned protected argument, reject oversized CX and an unbalanced
return, and exercise both SS.B values in USE32.

The first partial run is retained in
`arch/x86/build/dos-arguments-fixture-20260908/`:
two assertions expected a stack-mapping error when the earlier code-target
lookup already failed. Those expectations were corrected; the interrupted
API run is not counted as a pass.

Verified on the isolated KVM `:12` VM, 8 GiB/4 CPUs, disposable 128 MiB
disk. The kernel is 5,923,744 bytes, SHA-256
`0e5aff97ce3e744ac2a46335c7af85e1dba633afd1ae480266764a66fde03714`.
Build: `make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf dos-rm-arguments-test`.

- All 324 new internal checks and the complete API suite pass; the API
  PASS is at serial line 192693. No nonzero failure counters or mismatch
  diagnostics occur in the final archived log.
- All three argument binaries pass again on this kernel: emulated USE16
  and USE32 exit 42, native USE32 exits 2Ah. Both USE32 stack widths and
  the nested argument call are covered by those binaries.
- Six control/critical probes and fifteen other independent probes pass.
  Native FLAGS retains the known 71h result, not a passing 2Ah result.
- Native `dosrun DOOM.EXE -nosound` exits zero via F10/Y, with four input
  bytes and four IRQ1 deliveries; 12,693,033 VGA faults are mediated.
- Emulated `dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exits
  zero after 656,933,437 instructions, with 210,208,248 protected JIT
  instructions, zero cache resets and four IRQ1 deliveries.
- Subsequent PE32 `seh3_pe32.exe` and `seh4_pe32.exe` exit zero;
  emulated `dpflags32.com` exits 42 after both game sessions.
- PCM is 48 kHz stereo, 64.577625 seconds, 4,683,229 nonzero samples,
  peak 32,079 and RMS 3,730.08. This verifies digital signal, not music
  compatibility, acoustic fidelity or a performance improvement.

All eight captures were viewed: native gameplay, emulated title and
300M gameplay, the 600M credit screen, both quit prompts and shell
returns. The existing unscaled 320x200 presentation and text-mode
box-drawing glyph gap remain. Kernel, serial log, both COM probes,
screenshots, WAV and `verification-summary.json` are archived at
`arch/x86/build/dos-arguments-verified-20260908/`. The archived kernel
matches the build. The test VM and helpers are stopped; user sessions
`:0` and `:1` were not touched. `git diff --check` passes.

Remaining scope includes allocation-failure injection, page-table/payload
alias edge cases and exhaustive nested-call depth tests. The default PM
timer's special real-IVT chaining path and other legacy DOS buffers still
need their own paged-access audit. General debug retirement, native FLAGS,
full VM86, x87 ownership, music and performance remain open; this is not
complete DPMI or DOS/4GW compatibility.

### 4.75 Checked protected-mode DOS file and console buffers

The protected INT 21h file/console bridges no longer copy through
`dpmi_translate`, whose legacy path skipped guest paging for low linear
addresses. They share descriptor-region validation with fixed DPMI records
and use the canonical page walker for every protected buffer, regardless
of whether its linear address happens to lie below guest RAM size.
Readable sources, writable destinations, conforming code, expand-down
limits, USER permissions, physical backing and linear overflow are checked.

Whole requests are preflighted in bounded page lists without setting their
payload pages' A/D bits. Each admitted chunk captures all translations,
commits metadata and snapshots its bytes before writing. This preserves
overlapping payloads and page-table aliases; a payload store cannot redirect
a later page of the same copy. Guest GDT/LDT descriptor fetches retain their
separate architectural access semantics, so this is not whole-service
rollback atomicity. Payload access still uses the DOS memory helpers for
EMS and banked VGA apertures instead of assuming plain contiguous RAM.

The architectural chunk limit is still 64 KiB, not the 64-byte fixed-record
limit. Large USE32 requests remain split across real calls, retaining
ECX/EAX counts and EDX offsets as specified by Microsoft's
[MS-DOS extensions for DPMI](https://docs.pcjs.org/specs/dpmi/1991_03_11-MSDOS_DPMI_EXTENSIONS.pdf).
Each nonempty bridge invocation owns at most sixteen host scratch pages,
reused across chunks and released with its conventional allocation.
Nested calls have independent storage. Empty transfers allocate no host
scratch pages and still invoke the underlying DOS operation.

Copy-out resolves the current destination after real handlers/callbacks.
Short reads copy only the returned count. Error paths retain the initialized
bounce buffer's untouched bytes, and a newly invalid output page is rejected
before the chunk writes anything. These guarantees do not undo an already
completed device operation or earlier chunks.

The `$` scanner validates the descriptor once, then reads page fragments
bounded by the segment, linear address range and physical backing. A
terminator at the last readable byte does not require the next page.
USE16 does not scan beyond offset FFFFh. AH=0Ah and AX=0C0Ah with a zero
maximum need only the first byte, including a read-only descriptor ending
there; nonempty line buffers still require writable room for the header
and maximum line. Microsoft's [DOS 4 input routine](https://github.com/microsoft/MS-DOS/blob/main/v4.0/src/DOS/KSTRIN.ASM)
also returns before input when the maximum is zero. It fetches both header
bytes with LODSW; accepting a one-byte protected endpoint here is a host
translation policy, not a claim to reproduce that routine's bus accesses.

The new internal matrix has 786 passing checks: low/high mappings,
USE16/USE32, permissions, first/last backing limits, expand-down and code
descriptors, large preflights, seventeen-page physical permutations,
page-table/payload aliases, terminator boundaries, and real INT 21h hooks
that remap or revoke the destination before copy-out. It also checks short
reads, failure preservation and conventional allocation recovery.
`dpio16/32.com` and `dpcon16/32.com` pass in emulated USE16/USE32 and native
USE32. The console fixture now covers zero-maximum read-only endpoints;
both fixtures update protected retry counters through a data selector,
and the file fixture's failure reporter no longer writes through CS.

Verified on the isolated KVM `:12` VM, 8 GiB/4 CPUs and a disposable
128 MiB disk. The kernel is 5,931,976 bytes, SHA-256
`5174367f7707cd46fdad2828bdaa6dc51df11f5797ee78fb920c7d03854cf9db`.
Build: `make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf build/test/dpio16.com build/test/dpio32.com build/test/dpcon16.com build/test/dpcon32.com`.

- All 786 new checks pass at serial line 1740, and the complete API suite
  passes at line 193664. No nonzero failure counters or mismatch markers
  occur in the archived log.
- Six file/console probes, three argument probes, six control probes and
  fifteen independent probes pass. Native FLAGS retains its known 71h
  result, not a passing 2Ah result.
- Native `dosrun DOOM.EXE -nosound` exits zero through F10/Y, with
  12,882,759 mediated VGA memory faults and four IRQ1 deliveries.
- Emulated `dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exits
  zero after 658,178,621 instructions, with 210,853,019 protected JIT
  instructions, zero cache resets and four IRQ1 deliveries.
- After both games, PE32 `seh3_pe32.exe` and `seh4_pe32.exe` exit zero;
  emulated `dpflags32.com` exits 42 after 524,333 instructions.
- PCM is 48 kHz stereo, 67.7901875 seconds, 4,900,660 nonzero samples,
  peak 32,456 and RMS 3,709.24. This verifies digital signal only, not
  music compatibility, acoustic fidelity or a measured speedup.

All eight PNG captures were viewed: native gameplay, emulated title and
300M gameplay, the 600M credit screen, both quit prompts and shell returns.
Unscaled 320x200 presentation and text-mode box-drawing glyphs remain open.
Kernel, serial log, four edited COM probes, screenshots, WAV and
`verification-summary.json` are archived at
`arch/x86/build/dos-io-buffers-verified-20260908/`. The archived kernel
matches the build. The test VM and helpers are stopped, user sessions
`:0` and `:1` were untouched, and `git diff --check` passes.

Remaining scope is explicit: real DOS services still use physical
conventional pointers internally. General paged real/VCPI/VM86 DOS buffers,
legacy 000Bh/000Ch descriptor records and the real 0305h record still need
checked access. Descriptor changes during callbacks,
scratch-allocation failure injection and cross-chunk string mutation need
separate coverage. Default timer real-IVT chaining, debug retirement,
native FLAGS, x87 ownership, music and performance remain open. No runtime
speedup or complete DOS/4GW compatibility is claimed here.

### 4.76 Checked DPMI descriptor and real-state records

`000Bh` and `000Ch` now access the complete eight-byte descriptor through
the checked fixed-record path. Get requires writable ES:(E)DI; Set accepts
read-only input and snapshots it before validating rights and changing BX.
The buffer selector may equal the descriptor being changed. Allocator
ownership remains separate from the descriptor's Present bit, and host
descriptors remain immutable. Low linear addresses no longer bypass paging.

Fixed-record page admission is shared by protected records and the real
`0305h` entry. Both translations are captured before access, including
noncontiguous pages and aliases of paging structures. Segment bounds,
linear overflow, backing limits and page permissions are checked before
payload access. Rejected admission does not modify payload or its page A/D
bits; descriptor-table fetches retain their separate access semantics.
An admitted Set with invalid descriptor contents does read its source and
mark it accessed, but leaves the target descriptor unchanged.

The real state entry keeps the 64-KiB DI boundary and ignores EDI's upper
half, even for a USE32 client. Its page walk uses the current mode's
privilege and CR0.WP rather than imposing protected-client permissions on
real-mode accesses. It preserves the caller CPU image and the existing
20-byte opaque dormant stack/paging record. This does not enable PG with
PE clear on an actual CPU, implement general VM86, or expand the saved
record to cover all hidden descriptor state.

The [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf)
documents the descriptor buffers on pages 67-68, state procedures on page
94 and invalid-pointer exceptions on pages 12 and 154. Buffer rejection
here follows the existing host policy: CF/8022h for descriptor services
and a failed state-service request for 0305h. This is defensive rejection,
not completion of the specification's client-visible pointer-fault and
restart contract. Normalizing that exception path remains required.

The new matrix passes 1,689 checks across USE16/USE32 descriptor services,
low/high mappings, noncontiguous pages, read-only input, invalid endpoints,
self-selected descriptors, immutable targets and page-table aliases.
Real-state cases cover exact segment ends, poisoned upper EDI, identity
and paged access, VM86 permissions, supervisor WP behavior, truncated
physical backing, invalid record contents and unchanged caller registers.
These synthetic paging cases supplement, rather than claim, full VM86
instruction execution. An initial fixture failure was traced to its
identity-address sentinel overlapping its own page directory; the sentinel
was moved, with the failed run retained separately.

`dpcbuf16/32.com` now exercises Get/Set records via actual INT 31h gates,
including read-only input/output and ES equal to BX, before its existing
callback relocation test. Both emulated widths and native USE32 pass.
The new `dos-dpmi-test` shell command runs the entire DPMI selftest directly;
`dos-api-test` still includes it as part of the broader DOS suite.

Verified on isolated KVM `:12`, 8 GiB/4 CPUs and a disposable 128 MiB
disk, with kernel SHA-256
`9c0b30372c3615c2ad3eabf8f48c630cd1df7a47bee227b0dbefab50c55b343d`
(5,948,456 bytes). Build:
`make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf build/test/dpcbuf16.com build/test/dpcbuf32.com`.

- New checks pass at serial line 1080; the complete DPMI selftest passes
  at line 1292, including the prior 74 state, 1,336 fixed-buffer, 52
  callback-buffer, 324 argument, 416 paging-state and 786 I/O checks.
- Fifteen independent probes, six file/console probes, three argument
  probes and six control probes pass. Native FLAGS still exits 71h,
  explicitly not counted as passing. The broader `dos-api-test` was not
  rerun on this kernel; its previous result is not attributed to this run.
- Native DOOM exits zero through F10/Y, with 12,753,578 mediated VGA
  faults and four IRQ1 deliveries. Emulated DOOM with digital audio and
  `-nomusic` exits zero after 657,572,820 instructions, with 209,150,906
  protected JIT instructions, zero cache resets and four IRQ1 deliveries.
- Post-DOS `seh3_pe32.exe` and `seh4_pe32.exe` exit zero; emulated
  `dpflags32.com` exits 42 after 458,801 instructions.
- PCM is 48 kHz stereo, 65.478979 seconds, 4,851,962 nonzero samples,
  peak 31,248 and RMS 3,797.04. This is digital signal evidence, not a
  claim of music support, acoustic fidelity or measured performance.

All eight screenshots were viewed: native gameplay, emulated title and
300M gameplay, 600M credits, both quit prompts and shell returns. The
existing 320x200 scaling and text box-drawing glyph gaps remain. Artifacts
are in `arch/x86/build/dos-records-verified-20260908/`; the fixture-failure
run is in `arch/x86/build/dos-records-fixture-failure-20260908/`.
The VM and helpers are stopped/reaped, sessions `:0`/`:1` were untouched,
and `git diff --check` passes. GitNexus was unavailable; scope was reviewed
manually without installing or repairing it. No commit was requested.

Remaining DOS scope includes client-visible host buffer exceptions and
restart, general real/VCPI/VM86 service buffers, guest descriptor-fetch
backing/wrap validation, nested raw hidden state, default timer real-IVT
chaining, debug retirement, native FLAGS, x87 ownership, music and
performance. This record-copy improvement does not complete DOS/4GW.

### 4.77 Checked paged DPMI exception and locked interrupt frames

Exception entry and locked interrupt entry now admit and store their entire
16/32-bit public frames before publishing SS:ESP, nesting depth or virtual
IF changes. The existing fixed-record copier captures both page translations
and checks permissions and physical backing, including low linear addresses
and records spanning a page-directory boundary. Rejected admission leaves
payload and paging A/D metadata unchanged. Locked-stack exhaustion keeps
its diagnostic termination policy; it is not silently retried or reported
as a successful interrupt.

Active nested stacks use the loaded SS descriptor and B bit, even when
the client has edited the LDT entry. Newly selected and dormant protected
stacks still load their descriptors from the table. This distinction does
not implement complete hidden-state preservation across nested raw switches.
Exception and locked-interrupt returns read complete paged snapshots rather
than treating a linear address as contiguous physical memory. Return reads
accept read-only page mappings, but SS itself must remain writable data.
Default exception/interrupt flag records now use the same loaded-SS rules.
Edited return destinations retain their separate live-selector validation.

The interpreter's private host INT frame now uses checked loaded-SS and
page access before storing any bytes or calling the service. A missing
page raises #PF at the INT instruction, with the expected write/user error
and CR2, instead of silently spilling a partial frame into adjacent RAM.
The [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf)
describes locked-stack nesting and the public exception frame in its
stacks/mode-switching and CPU-exception sections (pages 24-33).

New in-OS coverage includes 320 host-frame checks and 16 actual INT 31h
instruction checks. These cover USE16/USE32, both SS.B sizes, low/high
linear addresses, noncontiguous pages, PDE boundaries, absent/read-only/
supervisor mappings, unbacked physical pages, loaded-descriptor lifetime,
expand-down limits and unchanged payload/state on rejected admission.
The paging matrix directly exercises return helpers; it does not claim
native execution with guest page tables. The focused
`dos-dpmi-stack-test` also runs the existing 76 exception-stack, 224 return
destination and 176 interrupt-stack checks. All pass on this kernel.

Initial fixture corrections are retained separately: synthetic images now
load SS explicitly, virtual FLAGS assertions include IOPL, and the version
probe expects AH=0/AL=90 rather than AL=9. The independent `dplock32.com`
probe exposed a real previously unchecked access: after verifying an
out-of-limit returned ESP it used that stack for INT 21h. Both widths now
restore a backed stack only after the return-value assertion, including
their failure exit path. The production return validation is not weakened.

Verification on 2026-09-08 used a disposable 128 MiB OsitoFS3 snapshot with
KVM, 8 GiB RAM and four CPUs, on VNC `:12` / UDP 7790. User sessions `:0`
and `:1` were not touched. Both build commands completed successfully:

```text
make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf
make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/test/dplock16.com build/test/dplock32.com
```

- `dos-dpmi-stack-test`: 812 checks pass, including the 336 new checks;
  the focused PASS is at serial line 1514.
- The complete `dos-api-test` was rerun and passes at serial line 194701,
  including the prior descriptor/state record and buffer matrices.
- Fifteen independent probes, six file/console probes, three real-call
  argument probes and six control-reflection probes pass. The native
  FLAGS result remains `71h`, recorded separately and not counted as a pass.
- Native DOOM (`-nosound`) renders, accepts F10/Y and exits with code zero.
  Its 13,114,227 mediated VGA memory faults are device accesses, not crashes.
- Emulated DOOM (`-config audio.cfg -nomusic`) renders, accepts F10/Y and
  exits with code zero after 662,897,620 instructions. JIT accounts for
  205,989,576 instructions, including 205,981,719 protected instructions;
  no full cache resets occurred. This is a regression run, not a speedup claim.
- The 48 kHz stereo PCM capture contains 4,249,235 nonzero samples over
  55.749625 seconds, peak 30,853 and RMS 3,942.35. This verifies digital
  signal output, not music support or audio fidelity.
- Subsequent `seh3_pe32.exe` and `seh4_pe32.exe` both exit zero;
  emulated `dpflags32.com` exits `2Ah` after 491,567 instructions.

All eight PNGs were inspected: native gameplay/quit/shell and emulated
title/gameplay/quit/text-exit/shell are present. The small unscaled 320x200
viewport and text box-drawing glyph substitutions remain visible gaps.
The VM was stopped and its helper sessions reaped. Evidence and the JSON
manifest are in `arch/x86/build/dos-host-frames-verified-20260908/`.
The archived 5,961,136-byte kernel matches the build, SHA-256
`a37f5539bf8a40be8a000b5d358639960deabf807c42d0b668b3343165a46925`.
Earlier fixture failures are preserved separately in
`dos-host-frames-fixture-failure-20260908/` and
`dos-host-frames-fixture-b-20260908/` under the same build directory.
The changed source files retain LF endings and `git diff --check` passes.

Remaining scope includes host-service pointer exception reflection and
continuation without repeating already completed DOS operations. Ordinary
non-locked PM software interrupt pushes, callback entry and the special
real-timer entry still contain legacy host pushes. Those consumers must
be migrated before claiming support for arbitrary paged locked stacks;
the expanded shared admission checks do not make their writes page-aware.
General real/VCPI/VM86
service buffers, guest descriptor-fetch backing/wrap validation, complete
nested raw hidden state, default timer real-IVT chaining, debug retirement,
native FLAGS, x87 ownership, music and performance remain open. This is
an exception-frame prerequisite, not complete DOS/4GW compatibility.

### 4.78 Paged callback and reflected real-mode entry frames

Callback entry and the special real-mode timer path no longer build their
protected frames with legacy `cpu_push16/32`. Timer, Ctrl+C and critical
error reflection share prepared frame records: admission captures the
selected SS descriptor, its B bit, the final cursor and every destination
translation before storing the complete record. CPU mode, SS:ESP and
nesting are published only after the payload is stored. INT 24h retains
its mixed layout: a client-width IRET followed by twelve DOS words.

A callback has two outputs, its 50-byte real-register image and its
protected return frame. Both are admitted first, and both sets of A/D
metadata are committed before either payload. This matters when either
output aliases the other's PTEs, including circular aliases: merely
changing the order of the two copies does not preserve both records.
Callback entry also captures its ES descriptor before output and does
not reload the admitted SS during the remaining data-segment setup.

The callback's private real-mode frame is read using the loaded real SS
and its paging context before applying the dormant protected context.
Source reads may update accessed bits; rejected output admission does
not write either output record or publish a mode/nesting transition.
Callback-return private-frame validation now requires readable pages,
not writable pages; the loaded SS descriptor itself still must be writable
data. This follows the callback entry/IRET contract described on pages
34-35 and 91 of the [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf).

The new in-OS entry-frame matrix reports 2,010 checks with zero failures:

- USE16/USE32 with independent SS.B16/SS.B32, low/high linear addresses,
  noncontiguous physical pages and a crossed PDE boundary.
- Absent, read-only, supervisor and unbacked output pages; ordinary and
  expand-down limits; invalid dormant selectors and a changed LDT entry
  behind an already loaded protected SS.
- Actual timer/Ctrl+C/critical handler IRETs, and callback IRET followed
  by its private return INT, with mode, cursor and nesting restoration.
- Bidirectional and circular output/PTE aliases, checked immediately
  after callback entry rather than pretending the overwritten page
  tables can still execute an ordinary handler return.
- Paged real input and read-only paged callback-return records, plus
  negative cases preserving CPU state and callback depth.

Verification on 2026-09-08 used KVM with 8 GiB RAM, four CPUs and a
disposable 128 MiB OsitoFS3 snapshot on VNC `:12` / UDP 7790. User sessions
`:0` and `:1` were not touched. The kernel build succeeded:

```text
make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf
```

- The 2,010 entry-frame checks pass at serial line 1503. The complete
  `dos-api-test`, including the existing stack and record matrices, passes
  at line 194320; no matrix reports a nonzero failure count.
- Fifteen independent probes, six file/console probes, three real-call
  argument probes and six control-reflection probes pass. Native FLAGS
  still exits `71h`, recorded as a known gap rather than a passing `2Ah`.
- Native DOOM (`-nosound`) renders, accepts F10/Y and exits zero. Its
  13,126,213 mediated VGA memory faults are device accesses, not crashes.
- Emulated DOOM (`-config audio.cfg -nomusic`) renders, accepts F10/Y and
  exits zero after 675,283,516 instructions. JIT accounts for 217,141,559
  instructions, including 217,134,212 protected instructions; there are
  no full cache resets. This verifies regression behavior, not a speedup.
- The 48 kHz stereo PCM capture has 3,676,577 nonzero samples over
  50.8361875 seconds, peak 31,557 and RMS 3,956.06. This checks digital
  signal output, not music support or audio fidelity.
- Subsequent `seh3_pe32.exe` and `seh4_pe32.exe` exit zero; emulated
  `dpflags32.com` exits `2Ah` after 65,585 instructions.

All eight PNGs were inspected: native gameplay/quit/shell and emulated
title/gameplay/quit/text-exit/shell are visible. The small unscaled 320x200
viewport and text box-drawing glyph substitutions remain. The VM was
stopped and all helper sessions were reaped. Evidence and the JSON manifest
are in `arch/x86/build/dos-entry-frames-verified-20260908/`. The archived
5,973,512-byte kernel matches the build, SHA-256
`eac1c3594cc6c39fb29b41b6a5b46ab9642a395654657d89a172c2ca95fb216b`.
The initial 1,898-check pilot also passed; it was archived in
`arch/x86/build/dos-entry-frames-initial-20260908/` before adding the final
112 read-only callback-return checks. GitNexus was unavailable; manual
impact review covered all shared frame/buffer consumers. The edited files
retain LF endings and `git diff --check` passes.

These are modeled-vCPU paging tests, not native execution with arbitrary
guest CR3 values. Ordinary non-locked PM software interrupt frames and
the inactive-DPMI hardware fallback still contain legacy host pushes.
Host-service pointer-fault reflection also remains incomplete: the
0300h/0301h/0302h output revalidation still returns 8022h on failure, and
file/console copies return DOS errors. Their replacement needs a typed
fault and a host continuation retaining completed results; retrying the
whole service would duplicate already completed real-mode side effects.
General real/VCPI/VM86 buffers, descriptor-fetch backing/wrap admission,
nested raw hidden state, default timer real-IVT chaining, native FLAGS,
debug retirement, x87 ownership, music, presentation and performance
remain open. This follow-up does not complete the DOS layer.

### 4.79 Checked ordinary DPMI software entry and native continuation

Ordinary protected software vectors now use the same complete stack-record
admission as the interpreter's private INT frame. The caller supplies both
the successful return EIP and the fault EIP. Loaded SS, its independent B
bit, segment limits and every page permission are checked before any frame
payload is written or ESP is published. Handler selection is consumed even
when admission faults; it cannot accidentally fall through to the built-in
DOS translator. Destination code is checked for type, presence, privilege
and limit before entry, and its admitted descriptor is cached rather than
reloaded after a possibly aliasing stack write. Descriptor accessed-bit
updates remain permitted before a later stack fault.

The inactive-DPMI hardware push branch was unreachable: protected execution
without DPMI already routes through `cpu8086_uses_guest_idt` and the checked
guest IDT dispatcher. It was removed, not replaced with a second hardware
interrupt implementation. Remaining `cpu_push16/32` calls in `dos_int.c` are
fixture construction, not production interrupt delivery.

There are two distinct native boundaries:

- A DPL fault on a native INT retains the original instruction address,
  including prefixes. The native bridge now imports all GPRs and data
  selectors, clears the host-generated RF bit, uses checked entry, and
  synchronizes the LDT before exporting the resulting handler frame.
- A direct native gate has already executed INT and supplies only the
  post-INT address. Its host-side entry fault therefore reports that address,
  not a guessed backward decode. An exception-depth-owned continuation
  retains the selected vector target. An unchanged CS:EIP return completes
  that entry using repaired SS:ESP; an edited destination abandons it.
  Repeated faults retain the pending entry, and nested exceptions cannot
  consume a different depth's continuation. This is host-entry continuation,
  not a claim that the original prefixed instruction start was recovered.

The continuation retains only an unfinished software-handler entry. It does
not retry DOS I/O or solve host-service buffer exceptions. Software-vector
virtual-IF behavior remains distinct from hardware and CPU-exception
delivery, as described in the [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf)
on pages 27-31.

The new in-OS matrix reports 560 checks, including 32 repaired returns,
with zero failures. It covers USE16/USE32, independent SS.B sizes,
noncontiguous pages and PDE boundaries, absent/read-only/supervisor maps,
ordinary and expand-down limits, edited loaded SS, invalid code targets,
and virtual IF for vectors 0-7 versus ordinary software vectors. Executed
RETF/private-return/IRET sequences check retry, re-fault, nesting, edited
CS:EIP cancellation and replacement SS. `dos-dpmi-stack-test` includes this
matrix as well as the prior 812 checks.

`test/dpmi_software_frame.S` builds four independent COM fixtures:
`dpsw16` and `dpsw32` run with `--emulate`; `dpswn16` and `dpswn32` run
USE32 natively with SS16 and SS32. Each verifies four stack-fault repairs
and exactly four handler calls, using plain INT and 15-byte prefixed INT
through vectors 60h and 33h, with GPR sentinels checked inside the exception
handler and after the final return. All four exit `2Ah` and print their
PASS marker. The first USE16 pilot exited `2Ah` but could not print because
the COM still owned all DOS memory. The corrected fixture shrinks its PSP
block before DPMI entry and checks the console result; no production rule
was weakened for it.

Verification on 2026-09-08 used
`make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf`.
The 5,981,880-byte kernel has SHA-256
`14dfa9adc4bfec08cc85392be416bb1a3c60421ab5d0e59617ab2382594adf23`.
The build succeeded; its pre-existing unused `install_dos_idt_entry` warning
in `dos_init.c:44` is unrelated to this change. An isolated KVM VM used 8 GiB,
four CPUs, a disposable 128 MiB OsitoFS3 snapshot, VNC `:12` and UDP 7790.
It ran all four COM fixtures, then the complete `dos-api-test`: PASS at
serial line 195517, with the new 560-check matrix at line 4728. The 15
existing independent probes, six file/console probes, three argument
probes and six Ctrl+C/critical-error probes passed. Native FLAGS remain
the explicitly separate 71h failure, not a passing 2Ah result.

In that same VM, native `dosrun DOOM.EXE -nosound` exited zero after
13,103,341 mediated VGA accesses; emulated
`dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exited zero after
672,285,245 instructions, including 216,257,965 JIT instructions and
216,250,630 protected JIT instructions. The cache retained 4,096 blocks,
with 605,447 lookup evictions, no code evictions and no resets. These are
regression observations, not a measured speedup. Both runs accepted F10/Y
and delivered four IRQ1 events. Subsequent PE32 `seh3` and `seh4` exited
zero, and emulated `dpflags32` exited 2Ah.

The WAV contains 50.803 seconds of 48 kHz stereo PCM, 3,687,065 nonzero
samples, peak 31,197 and RMS 3,974.33. This verifies digital output, not
music or audio fidelity. All eight PNGs were inspected: native gameplay,
quit confirmation and shell return, plus emulated title, changed gameplay
at 300M/600M instructions, quit confirmation and shell return. The existing
320x200 scaling and text box-drawing glyph limitations remain visible.
The matching kernel, serial log, WAV, PNGs, four COM binaries and manifest
are archived in `arch/x86/build/dos-software-frames-verified-20260908/`.
Only this VM was stopped via its QMP socket, and all helpers were reaped;
user sessions `:0` and `:1` were untouched. GitNexus was unavailable, so
impact was checked manually. Edited files retain LF, and `git diff --check`
passes. No commit was made for this follow-up.

General host-service buffer fault reflection and side-effect-preserving
continuations, real/VCPI/VM86 service buffers, descriptor backing/wrap
admission, full raw hidden-state preservation, native FLAGS, debug
retirement, x87 ownership, music, presentation and performance remain open.
This follow-up does not complete the DOS layer.

### 4.80. Restartable DPMI real-call register buffers

INT 31h functions 0300h, 0301h and 0302h now distinguish the input read
from the output write of their 50-byte real-mode register record. Buffer
probes retain a typed selector, segment or page fault rather than converting
every bad pointer to CF/8022h. Page faults retain the walker's CR2 and error
bits. Invalid host RAM backing is not reported as a fictitious guest page
fault. Other services retain their existing boolean admission interfaces.
The [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf)
defines the register record and result direction on pages 85-89, and the
legacy and extended exception contracts on pages 30-32.

The real call executes once. Its result stays on the live host C stack
while a client exception handler repairs the output mapping. Only the
buffer probe/copy is retried; neither callbacks nor completed real-mode
side effects are replayed. Both input and output are resolved again after
repair, including descriptor relocation and page-table changes.

A host-owned wait record identifies the suspended service by PSP and
exception depth. Only the matching validated exception return signals it.
Nested handlers and services cannot complete an outer wait. An unchanged
CS:EIP resumes the pending access; an edited destination cancels it before
executing the replacement code. Termination or the interpreter step quota
unwinds the service without copying a pending result. The interpreter
discards its private INT frame exactly once and preserves edited control
FLAGS. Native dispatch exports edited CS:EIP, SS:ESP and control FLAGS
through its IRET frame. A signalled return stops before periodic IRQ
polling; the pending IRQ remains deliverable after the host continuation.

The visible legacy exception frame uses the caller's post-INT address.
This does not recover an instruction start by backward decoding, implement
the DPMI 1.0 extended host-exception frame, or advertise its restartability
capability. The retained arguments identify the original register record;
the handler repairs its mapping rather than replacing the pending call's
parameters through GPR edits.

EXEC parent snapshots now preserve the complete wait context: 32-bit EIP,
PSP, protected/real mode, signal and status. Child startup clears the parent
signal, and parent restoration restores it together with the DPMI wait
record. This also fixes the previous 16-bit truncation of a saved stop EIP.
The isolated snapshot round-trip test reports 32 checks with zero failures.
That is snapshot coverage, not an end-to-end EXEC launched from a live
buffer-fault handler.

The new DPMI matrix reports 6,476 checks, 576 recovered calls and zero
failures. It covers USE16/USE32 with both SS.B sizes, direct and decoded
15-byte INT dispatch, all three real-call forms, noncontiguous pages and
PDE crossings. Cases include absent/read-only/supervisor pages, segment
permissions and limits, repeated faults, nested exceptions and services,
edited CS:EIP cancellation, replacement SS:ESP, termination and step quota.
Four additional timer-boundary cases verify that the matched return is
observed before IRQ delivery and that the IRQ is not lost afterward.

`test/dpmi_service_buffer.S` builds four independent normal fixtures:
`dpsb16` (USE16/SS16), `dpsb32` (USE32/SS32), `dpsbs16` (USE32/SS16), and
`dpsbs32` (USE16/SS32). Each exercises 21 calls: three real-call forms times
seven buffer-recovery scenarios. A protected callback entered from real
mode revokes the output descriptor's write or present bit, so the fault
occurs after the observable effect. The tests check one callback per call,
retry without repair, nested #BP, edited EIP cancellation, edited DF, and
GPR/ESP sentinels. Four emulated and two native runs passed in the COM pilot.

The first in-OS pilot hit the step quota because its real-mode test code
addressed a PTE above DS's 64 KiB limit. The fixture now uses a valid
segment:offset; no production admission rule was relaxed. Another pilot
exposed a separate native bridge defect: making an already-loaded ES
descriptor absent during INT 31h/0009h faults at the host's `mov %ax,%es`
in `dos_int_common`, before the intended register-buffer service starts.
`dpsbnp32.com`, built with `LOADED_NP=1`, retains that failing scenario for
follow-up. The normal fixtures revoke presence inside the callback instead;
they do not claim that stale native segment restoration is fixed. Pilot
kernel/serial evidence is retained in the `dos-service-buffer-*-20260908`
build archives. An interrupted full-suite pilot is not counted as a pass.

The native follow-up must preserve descriptor reload semantics, not silently
keep an old hidden cache: the specification requires reload after 0009h for
DPMI 1.0 and recommends it for 0.9 (page 64). The unresolved boundary is
admission and client fault reflection before restoring host-saved segments.

A later full-suite pilot exposed two obsolete step-quota expectations:
the nested real callee had executed INC, but not its RETF. Those tests now
expect output only after the return instruction completes. A stopped callee
retains its completed memory side effects without publishing a partial
register record. The 127-check quota suite passes with this expectation;
the interrupted pilot is retained separately, not counted as a full pass.

Final verification used the 5,994,352-byte kernel with SHA-256
`057d8212335d600524892539a12a1f783cb130bbe8aca1309b670211e27b4e9e`.
The build succeeded; the initial header rebuild had the pre-existing unused
`install_dos_idt_entry` warning at `dos_init.c:44`, and the final incremental
build had no warnings. An isolated KVM VM used 8 GiB, four CPUs, a disposable
128 MiB OsitoFS3 snapshot, VNC `:12` and UDP 7790. The complete `dos-api-test`
passed at serial line 198811, including the 32 EXEC snapshot checks at line
420 and 6,476 service-buffer checks at line 3959. All six new COM runs passed
again, as did the four previous software-frame COMs, fifteen independent
probes, six file/console probes, three argument probes and six control probes.

Ordinary emulated `dosexec.com` passed at 2,293 instructions. Native KVM EXEC
did not pass: its DPMI child exited 7Ch and the parent exited 49, matching the
ICEBP return mismatch documented in 4.42. The child binary still has the
same SHA-256 recorded there. Independent `dpdebug16` and `dpdebug32` emulated
runs passed with 2Ah; native `dpdebug32` reproduced the 71h mismatch. Neither
this native EXEC result nor the separate native FLAGS 71h result is counted
as a pass. No return-IP adjustment or relaxed fixture expectation was added.

In the same VM, native `dosrun DOOM.EXE -nosound` exited zero after 13,067,846
mediated VGA accesses. Emulated
`dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exited zero after
678,887,996 instructions, including 220,639,134 JIT instructions and
220,631,783 protected JIT instructions. The cache retained 4,096 blocks,
with 580,592 lookup evictions, no code evictions and no resets. These are
regression observations, not a speedup measurement. Both runs accepted F10/Y
and delivered four IRQ1 events. Subsequent PE32 `seh3` and `seh4` exited zero;
emulated `dpflags32` then exited 2Ah.

The WAV contains 47.632 seconds of 48 kHz stereo PCM, 3,437,855 nonzero
samples, peak 32,092 and RMS 3,984.53. This supports digital output, not music
or audio fidelity. All eight PNGs were inspected: native gameplay, quit
confirmation and shell return, plus emulated title, changed gameplay at
300M/600M instructions, quit confirmation and shell return. The existing
320x200 scaling and text box-drawing glyph limitations remain visible.
The matching kernel, serial log, WAV, PNGs, selected COM/EXE fixtures and
manifest are archived in `arch/x86/build/dos-service-buffer-verified-20260908/`.
Only this VM was stopped via its QMP socket, and all helpers were reaped;
user sessions `:0` and `:1` were untouched. GitNexus was unavailable, so impact
was checked manually. Edited files retain LF, and `git diff --check` passes.
No commit was made for this follow-up.

General DOS file/console copyout continuations, other DPMI records,
real/VCPI/VM86 buffers, extended host-exception contracts, descriptor
backing/wrap admission, native stale-selector restoration, raw hidden
state, native FLAGS/ICEBP, debug retirement, x87 ownership, music, presentation
and performance remain open. This follow-up does not complete the DOS layer.

### 4.81. Checked native DOS segment restoration

The native ES reload failure retained in 4.80 is now handled before the
host's segment MOVs execute. A shared probe checks GS, FS, ES, DS, CS and
SS against the current guest descriptors. Data and stack checks reuse the
interpreter's load rules; CS admission checks code type, privilege,
presence, compatibility mode and EIP limit. The probe returns typed #GP,
#NP, #SS or #PF information without changing the loaded CPU caches.
Descriptor accessed bits and page-walk A/D updates retain their normal
side effects. A page fault retains CR2 and supervisor descriptor-access
error bits. Loading SS:ESP does not read the new stack or fetch code.

Admission runs before first/re-entry to native execution and before a
native DOS gate restores its saved state. A client exception handler runs
on the interpreter while the host continuation remains live. A matched
return revalidates all six segments, including edited selectors or CS:EIP;
it never repeats the completed DOS service. Only a fully admitted image
commits the caches and is exported to the native IRET frame. GDT/LDT
backend tables are synchronized before restoring the hardware selectors.
Termination preserves the client's exit status and unwinds the wait.
The caller is admitted before polling pending IRQs, so interrupt delivery
cannot hide a bad caller CS/SS or move its fault to the IRQ handler's EIP.
If an IRQ is delivered, the handler image is admitted separately. A fatal
caller return leaves the pending IRQ unconsumed.

The DOS assembly gate now clears DF before entering C, after saving the
client's IRET frame. IRET restores the client's DF. This is a host ABI fix,
not a change to the client's direction flag or a workaround for one binary.

The [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf),
pages 30-32 and 63-64, permits edited legacy exception return destinations
and describes 0009h reload semantics. It also permits arbitrary type bits
in an absent descriptor, subject to DPL and the segment bit. The common
rights validator previously rejected absent execute-only code. It now
applies the readable/non-conforming restriction only to present code;
the existing reserved long-mode-bit restriction remains. A 65,536-case
rights matrix covers all access/flags-byte combinations. This validator
is shared by 0009h and 000Ch.

The native-admission matrix has 672 cases: 432 descriptor-policy probes,
96 descriptor paging probes and 144 real-handler recovery/termination
cases. It covers USE16/USE32, both SS.B sizes, all six segments, null and
unallocated selectors, RPL/DPL, code/data rights, presence priority, limits,
new bases and accessed bits. Paging cases cross a PDE boundary, test CR0.WP,
and compare the full CPU snapshot except CR2 on #PF. Repair cases must
stop before the caller's deliberately non-executable next instruction;
48 recoveries preserve the native FLAGS and register/stack state. Paired
cases latch IRQ0 before admission, require the fault handler to see the
original EIP, and verify the eventual IRQ return to the repaired caller.
Explicit 42h exits and fatal default handlers must unwind without resuming
the caller or consuming its pending IRQ.

`test/dpmi_native_segments.S` builds `dpnseg16.com` and `dpnseg32.com`:
both are USE32 native clients, with SS16 and SS32 respectively. Each runs
18 cases across the six loaded registers: absent descriptors, invalid
types and an edited EIP. A separate handler code selector and data alias
allow repair even when the client's CS or DS is invalid. Frame error,
post-0009h EIP, CS, SS:ESP, DF/CF and GPR sentinels are checked. The SS16
variant keeps a nonzero ESP high word. The older `dpsbnp32` now expects
its first fault at the completed 0009h boundary, before the real-call
buffer operation; the four normal buffer COM binaries are unchanged.

Pilot A exposed the absent-type validator gap. Pilot B passed both native
selector COMs and the old ES reproduction. The internal recovery fixture
initially compared emulated IOPL3 with native IOPL0; it now explicitly uses
native FLAGS while interpreting the handlers. Pilot C passed the focused
600 checks but its full-suite run exposed test-state leakage: the new
fixture assumed a dispatcher depth of one and left its 100-step quota
active. It now checks the incoming depth and restores the original quota,
count, termination and emulation policy. That interrupted run is retained
as a failed pilot, not counted as a full regression pass.

Review during pilot D found the IRQ ordering gap above. Its full run was
stopped before completion. An isolated reproduction with IRQ-first ordering
failed all 72 pending-IRQ cases: a data fault observed the IRQ handler's
EIP, while IRQ delivery replaced bad caller CS/SS. The reproduction kernel
and serial log are in `dos-native-segments-irq-repro-20260908/`, with kernel
SHA-256 `d22fc3439e2440dd5460eabbb7880efe462066d2998bffa40b11494ea70a256f`.
After ordering admission before IRQ delivery and checking the handler image
afterward, the focused suite reports 672 checks, 48 recoveries and zero
failures. The fixture also restores its original timer latch and PIC mask.

Final verification used the 6,002,752-byte kernel with SHA-256
`bb4eb36a7cf532974e40b6e5ae317b9be271490ca75982495653d758ed52237d`.
The clang build succeeded. An isolated KVM VM used 8 GiB, four CPUs, a
disposable 128 MiB OsitoFS3 snapshot, VNC `:12` and UDP 7790. The focused
672-check suite passed at serial line 3664; the complete `dos-api-test`
passed at line 205985, including the same matrix at line 15196, all 65,536
rights cases at line 11497 and the previous 6,476 buffer cases at line 9906.
Both new native COMs and the old loaded-ES reproduction passed. So did the
six normal buffer runs, four software-frame runs, fifteen independent
probes, six file/console probes, three argument probes and six control
probes. An accidentally overlapping serial helper was stopped, its partial
shell input was cancelled, and all file/console and argument probes were
rerun sequentially; only those clean runs are counted.

Emulated EXEC passed at 2,282 instructions. Native EXEC still failed with
DPMI child 7Ch and parent 49, matching the existing KVM ICEBP mismatch;
the child's SHA-256 remains
`6c21c1e92a01f3d530fd11d1c6083a03926981b30cedd39d01e802ba2512094a`.
Both emulated debug probes passed. Native debug and native FLAGS each
reproduced 71h, not the success code 2Ah. None of these known native failures
is counted as a pass or hidden by a changed fixture expectation.

Native `dosrun DOOM.EXE -nosound` exited zero after 13,074,387 mediated VGA
accesses. Emulated `dosrun --emulate DOOM.EXE -config audio.cfg -nomusic`
exited zero after 675,791,829 instructions, including 218,631,590 JIT
instructions and 218,624,223 protected JIT instructions. Its cache retained
4,096 blocks, with 591,318 lookup evictions, no code evictions and no resets.
These are regression observations, not comparative performance results.
Both runs accepted F10/Y and recorded four IRQ1 deliveries. Subsequent
PE32 `seh3` and `seh4` exited zero at lines 229793 and 229847; emulated
`dpflags32` then exited 2Ah at line 229916.

All eight PNGs were inspected: native gameplay, quit confirmation and shell
return, plus the emulated title, changed gameplay at 300M/600M instructions,
quit confirmation and shell return. The existing 320x200 scaling and text
box-drawing glyph limitations remain visible. The WAV contains 48.038
seconds of 48 kHz stereo PCM, 3,529,064 nonzero samples, peak 31,593 and
RMS 3,986.69. This confirms digital output, not music or audio fidelity.
The matching kernel, serial log, WAV, PNGs, selected COM/EXE fixtures and
manifest are archived in `arch/x86/build/dos-native-segments-verified-20260908/`.
Only this VM was stopped through its QMP socket; helpers were reaped and
user sessions `:0` and `:1` were untouched. GitNexus was unavailable, so
impact was checked manually. Edited files retain LF and `git diff --check`
passes. No commit was made for this follow-up.

General guest paging in the native backend, descriptor backing/wrap
admission, raw hidden state, extended host-exception frames, other service
copyout continuations, native FLAGS/ICEBP, debug retirement, x87 ownership,
music, presentation and performance remain open. This is not complete
DPMI or DOS/4GW compatibility.

### 4.82. Restartable protected DOS file and console copies

Protected INT 21h file reads/writes (3Fh/40h), dollar-terminated output
(09h) and buffered input (0Ah/0C0Ah) now retain their host continuation
across a client buffer fault. Descriptor and page-span probes preserve
typed #GP, #NP and #PF information, including selector/error and CR2.
A bad protected pointer is no longer reported as DOS access denied.
Actual DOS device, handle and memory errors keep their existing results.
The production changes are confined to `dos_dpmi.c`; dispatch and the
native return bridge reuse the shared service-exception wait from 4.80.

The original request arguments stay captured while a handler runs. Whole
request admission precedes the first real-mode I/O operation, and each chunk is
checked again before use. A completed real-mode output chunk is frozen
in host scratch before invoking a repair handler. Repair retries only
the protected copy, using newly admitted mappings; it does not repeat
the real read, advance the file again, consume more input or flush twice.
Nested DOS calls and aliases cannot replace that frozen output. Complete
chunk translations are captured before page metadata and payload writes,
preserving the overlap and page-table alias behavior introduced in 4.75.

The shared wait accepts only its matching validated exception return.
Changing the return destination cancels the pending service; termination
or the configured interpreter quota unwinds it. Scratch and conventional
allocations are released without publishing stale AX, GPRs or DOS status
over the edited client image. Already completed chunks remain completed;
unexecuted chunks stay unexecuted. Successful completion merges arithmetic
status flags, leaving handler changes to control flags such as DF intact.
Short file reads copy only their returned byte count. Failed DOS reads
retain the existing initialized-tail and device-written-byte behavior.
The read-terminator check uses the frozen result, not the bounce buffer
which a nested handler may have changed.

String scans can restart before output begins. Buffered input captures
the original maximum, validates its entire single-call buffer and retains
the completed line across copyout repair. It must not infer a copy length
from AX: unlike 3Fh, 0Ah and 0Ch with AL=0Ah do not return a byte count in
that register; the line count is in the buffer. This follows the
[Microsoft MS-DOS Encyclopedia system-call reference](https://www.pcjs.org/documents/books/mspl13/msdos/encyclopedia/section5/).
Editable legacy exception return frames remain governed by the
[DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf),
pages 30-32. Extended host-exception frames are a separate, still-open
capability; this change does not advertise that support.

The new in-OS matrix reports 13,648 checks, 928 recovered services and
zero failures. It crosses USE16/USE32 with both SS.B sizes, low and
PDE-crossing buffers, direct dispatch and decoded 15-byte INT instructions.
Cases cover absent/read-only/supervisor pages, descriptor type/presence/
limit faults, repeated repair, a nested breakpoint, a nested protected
DOS write, edited-EIP cancellation, explicit exit and quota exhaustion.
It also tests remapping the destination while clobbering the real bounce
buffer, short reads, actual DOS errors, FLAGS, GPRs, SS:ESP and allocation
cleanup. Limited conventional memory forces multiple chunks: later
repair or cancellation must neither replay the committed prefix nor
execute an unrequested tail. Fault-time call/byte counters are asserted.

`test/dpmi_io_recovery.S` builds `dprec16`, `dprec32`, `dprecs16` and
`dprecs32`. These independent COMs create a real 64-byte file, chain a
real INT 21h read and then use a protected callback to revoke its output
descriptor. Each runs seven cases, checking that exactly one 16-byte read
advances the file position to 16 even after repeated faults or cancellation.
They verify copied bytes, nested exception delivery, edited DF, retained
GPRs and full SS:ESP; cancellation leaves the destination unchanged. A
case overwrites the bounce buffer after the completed read and still
requires the original bytes. The four emulated runs and USE32 native
SS32/SS16 runs print `DPMI IO RECOVERY PASS` and exit 2Ah.

The older `dpmi_file_io.S` and `dpmi_console.S` negative pointer cases
now install #GP handlers, validate the error and post-INT destination,
and redirect to a distinct cancellation label. They expect the original
AX, preserved registers and no file-position advance, rather than the
old synthetic AX=5. Real DOS permission/device-error cases are unchanged.
All six emulated/native file and console runs pass.

Pilot A had 32 fixture failures: its short-read policy incorrectly applied
AX=2 to line-input copying. Correcting that expectation, without changing
the production copy, removed those failures. Pilot B added the multi-chunk
cases and passed the 13,648-check matrix, the focused DPMI suite and the
independent probes. Its old file fixture stopped at the obsolete AX=5
expectation; that is retained as a failed pilot, not a file-I/O pass.
Both pilot archives remain under `arch/x86/build/`.

Final verification used the 6,015,136-byte kernel with SHA-256
`656b80a132c1ac349cf847e15bb3a593220fd3e2e70f740c134c58b06df2468a`.
The clang build succeeded; the kernel and all eight new/updated COMs
remain current under `make -q`. The isolated KVM VM used 8 GiB, four CPUs,
a disposable 128 MiB OsitoFS3 snapshot, VNC `:12` and UDP 7790. The complete
`dos-api-test` passed at serial line 206489, including the new I/O matrix
at line 11999, all 65,536 rights checks at line 12001 and the previous
672 native-segment checks at line 15700. The six real-file recovery runs,
six file/console runs, three native-restoration runs, six normal buffer
runs, four software-frame runs, fifteen independent probes, three argument
runs and six control runs all passed. Serial helpers ran sequentially.

Emulated EXEC passed at 2,293 instructions. Native EXEC retained the known
failure: child 7Ch and parent 49 at line 211687, after 749 instructions.
Its child binary still has SHA-256
`6c21c1e92a01f3d530fd11d1c6083a03926981b30cedd39d01e802ba2512094a`.
Both emulated debug probes passed. Native FLAGS and native debug retained
71h at lines 206680 and 211818, not success code 2Ah. These failures are
recorded separately and are not counted as passes.

Native `dosrun DOOM.EXE -nosound` exited zero at line 215837 after
13,211,419 mediated VGA accesses. Emulated
`dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exited zero at line
231222 after 673,530,429 instructions, including 216,587,345 JIT and
216,579,870 protected JIT instructions. The cache retained 4,096 blocks,
with 629,653 lookup evictions, no code evictions and no resets. Both runs
accepted F10/Y and recorded four IRQ1 deliveries. These are regression
observations, not comparative performance measurements. Subsequent PE32
`seh3` and `seh4` exited zero at lines 231563 and 231617; emulated
`dpflags32` then exited 2Ah at line 231690.

All eight PNGs were inspected: native gameplay, quit confirmation and
shell return, plus the emulated title, changed gameplay at 300M/600M,
quit confirmation and shell return. Existing unscaled 320x200 output
and text box-drawing substitutions remain visible. The WAV contains
47.579 seconds of 48 kHz stereo PCM, 3,494,937 nonzero samples, peak 32,462
and RMS 4,025.59. This verifies digital output, not music or fidelity.
The matching kernel, serial log, WAV, PNGs, 46 COM/EXE fixtures and manifest
are in `arch/x86/build/dos-io-service-verified-20260909/`. Only this VM was
stopped through its QMP socket; helpers were reaped and user sessions `:0`
and `:1` were untouched. GitNexus was unavailable, so impact was checked
manually. Edited sources/docs retain LF and `git diff --check` passes.
No commit was made for this follow-up.

Other DOS service buffers and record continuations, real/VCPI/VM86
copyouts, general guest paging in the native backend, descriptor backing
and wrap admission, raw hidden state, extended host-exception frames,
native FLAGS/ICEBP, debug retirement, x87 ownership, music, presentation
and performance remain open. This is not complete DPMI or DOS/4GW
compatibility.

### 4.83. Restartable DPMI descriptor and memory-information records

INT 31h Get Descriptor (000Bh), Set Descriptor (000Ch) and Get Free Memory
Information (0500h) now use the shared service-buffer continuation. An
inaccessible eight-byte descriptor or 48-byte information record preserves
its typed #GP, #NP or #PF, including the selector/error and CR2, instead of
being conflated with an invalid target selector. The installed client
handler can repair the mapping or descriptor and resume the pending copy.
This production change is confined to the three dispatcher branches in
`dos_dpmi.c`; it reuses the checked page-span transfer and the shared wait
from 4.80, including native return admission from 4.81.

The original selector and buffer arguments remain captured across repair.
Both descriptor functions check the target before accessing the buffer and
again after the handler returns: freeing that target during recovery must
not publish a stale descriptor or write into a freed slot. Get Descriptor
reads its current contents after repair. Set Descriptor admits and consumes
the complete input before changing the target. Invalid target/mutability
still returns 8022h, and invalid descriptor rights still return 8021h.
The memory-information values are sampled after repair and preserve the
existing reserved fields and advisory accounting.

The distinction between invalid target selectors, invalid descriptor values
and pointer faults follows the [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf),
pages 66-67 and Appendix B. Page 99 defines the successful 0500h result.
This implements the existing host's recoverable-copy policy; it does not
advertise the separate DPMI 1.0 host-exception or write-protection flags.
Only the matching validated exception return resumes a copy. An edited
return destination cancels it; termination or the interpreter quota unwinds
it without publishing results over the client's edited register image.
Successful completion changes CF while retaining handler changes to DF.

The in-OS matrix reports 7,392 checks, 496 recovered services and zero
failures. It crosses USE16/USE32, SS16/SS32, low and PDE-crossing buffers,
and direct dispatch versus decoded 15-byte INT instructions. Policies
cover absent/read-only/supervisor pages, descriptor type/presence/limit
faults, a repeated fault, nested #BP and nested 000Bh, edited-EIP
cancellation, explicit exit, quota exhaustion and an unhandled default.
Additional cases free or edit the original target, change the handler's
returned BX/EDI/ES, reject an invalid descriptor after repair, or reject
an invalid/host-owned target before touching its inaccessible buffer.
Checks include exact output and neighboring sentinels, current descriptor
contents, public frame fields, error/CR2, GPRs, FLAGS, full SS:ESP and wait
cleanup. The existing raw record-admission matrix has 1,833 passing checks;
its pointer-negative fixtures check typed admission without pretending
that a delivered exception is a normal AX/CF service error.

`test/dpmi_record_service.S` builds `dprs16`, `dprs32`, `dprss16` and
`dprss32`; `make -C arch/x86 CLANG=1 dos-record-service-test` builds all four.
Each independent COM executes 20 cases using real client INT instructions:
repair, repeated repair, nested #BP, cancellation, edited DF, freed target
and edited request registers, omitting the target-free case for 0500h.
They verify descriptor bytes, untouched cancelled output, information
fields, CF/AX, GPRs and full SS:ESP. All four emulated variants and the
native USE32 SS32/SS16 variants print `DPMI RECORD SERVICE PASS` and exit
2Ah. Native page-table repair is not claimed by these descriptor fixtures.

The older `dpmi_callback_buffer.S` read-only output case now installs a
#GP handler, checks its error and post-INT destination, and cancels to a
distinct label. It requires the original AX/CF and exactly one fault, not
the obsolete 8022h result. The subsequent alternate read-only callback
return and relocated outer output checks are retained. No callback
production path was changed for this follow-up.

Final verification used the 6,023,384-byte kernel with SHA-256
`a4af53cd1b6f989c01161e07041372986fe7e1fef96fc94444ede906b1ab6b47`.
The clang build and all six new/updated COM builds passed and remain
current under `make -q`. The isolated KVM VM used 8 GiB, four CPUs, a
disposable 128 MiB OsitoFS3 snapshot, VNC `:12` and UDP 7790. The complete
`dos-api-test` passed at serial line 210323, including the new matrix at
line 10893, all 65,536 rights checks at line 15834 and the 672 native
segment checks at line 19533. All six record COM runs, fifteen independent
probes, six real-file recovery runs, six file/console runs, three native
segment runs, six real-call buffer runs, four software-frame runs, three
argument runs and six control runs passed. Serial helpers ran sequentially.

The identical final record COMs were also run against the archived 4.82
kernel (`656b80a132c1ac349cf847e15bb3a593220fd3e2e70f740c134c58b06df2468a`).
All six failed with 71h at the first inaccessible output, instead of
reaching the new kernel's PASS/2Ah result. Those failures, matching COMs
and old kernel are retained in
`arch/x86/build/dos-record-service-baseline-final-20260909/`. They are
negative control results, not successful compatibility runs. Pilot A
passed the internal matrix but stopped at the old callback fixture's
obsolete 8022h expectation. Pilot B found a USE32 fixture diagnostic that
changed only DX instead of the full EDX string pointer. Both pilot archives
remain available; neither failed probe is counted as a pass.

Emulated EXEC and both emulated debug probes passed. Native EXEC retained
the known child 7Ch/parent 49 result at line 217314; its child binary hash
is unchanged. Native FLAGS and native debug retained 71h at lines 210510
and 217006, not the success code 2Ah. These are recorded separately.

Native `dosrun DOOM.EXE -nosound` exited zero at line 221286 after
13,139,294 mediated VGA accesses. Emulated
`dosrun --emulate DOOM.EXE -config audio.cfg -nomusic` exited zero at line
236663 after 678,593,084 instructions, including 220,384,020 JIT and
220,376,673 protected JIT instructions. The cache retained 4,096 blocks,
with 611,112 lookup evictions, no code evictions and no resets. Both runs
accepted F10/Y and recorded four IRQ1 deliveries. Subsequent PE32 `seh3`
and `seh4` exited zero at lines 237004 and 237058, followed by an emulated
FLAGS pass. These are regression observations, not performance comparisons.

All eight PNGs were inspected: native gameplay, quit confirmation and
shell return, plus the emulated title, changed gameplay at 300M/600M,
quit confirmation and shell return. Existing unscaled 320x200 output and
text box-drawing substitutions remain visible. The WAV contains 49.179
seconds of 48 kHz stereo PCM, 3,596,711 nonzero samples, peak 29,500 and
RMS 3,965.59. This verifies digital output, not music or fidelity. The
kernel, serial log, WAV, PNGs, 50 COM/EXE fixtures and manifest are archived
in `arch/x86/build/dos-record-service-verified-20260909/`. The test VMs were
stopped through their own QMP sockets and the helpers were reaped. User
sessions `:0` and `:1` were untouched. GitNexus was unavailable, so impact
was checked manually. Edited files retain LF and `git diff --check` passes.
No commit was made for this follow-up.

Other DOS service buffers, callback/state record continuations, real/VCPI/VM86
copyouts, general guest paging in the native backend, descriptor backing
and wrap admission, raw hidden state, extended host-exception frames,
native FLAGS/ICEBP, debug retirement, x87 ownership, music, presentation
and performance remain open. This is not complete DPMI or DOS/4GW
compatibility.

### 4.84 Restartable protected 0305h state records

The protected state save/restore procedure now delivers typed #GP/#NP/#PF
buffer faults through the existing matching-exception continuation. It
re-probes the original ES:(E)DI and preserves the original AL operation
across repair. A save snapshots the dormant cursor and CR0/CR3 before
yielding: nested handler transitions must not replace the pending payload.
A restore reads the complete admitted record after repair and validates
its signature, mode, reserved byte and CR0 before publishing dormant state.
Neither operation switches the current CPU's address space.

The public host result now distinguishes invalid requests/records, completed
operations and interrupted operations. Both interpreted and native private
dispatchers preserve a handler's redirected CPU state or exit instead of
turning interruption into a new fatal error. On retry, only the interpreter's
private INT frame is consumed; the procedure's FAR caller remains on SS.
On redirection, that FAR frame belongs to the client and is not discarded
again by the abandoned service. Registers and FLAGS are not overwritten
by successful state transfer; deliberate handler changes remain visible.

The [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf),
page 93, defines the two FAR procedures, ES:(E)DI buffer, AL operation,
register preservation and opposite-mode state. This change implements the
existing host's recoverable-copy policy. It does not advertise DPMI 1.0
extended host-exception frames or restartability capability flags. The
real/VM86 procedure still uses checked, non-restartable admission; it cannot
borrow a protected exception context that does not exist.

The in-OS matrix passes 6,688 checks with 368 recovered operations and zero
failures, crossing USE16/USE32, SS16/SS32, low/PDE-crossing records, save and
restore, and direct dispatch/decoded private INT. It covers absent,
read-only and supervisor pages, invalid descriptor type/presence/limit,
repeated faults, nested #BP and nested 0305h calls, edited CS:EIP, explicit
exit, quota exhaustion, unhandled faults, changed AL/EDI/ES, modified input,
invalid records after repair and remapped output pages. Checks include
complete payloads and neighboring sentinels, no premature A/D bits, exact
error/CR2 and exception frame fields, dormant/current paging separation,
GPRs/FLAGS, full SS:ESP, FAR return and continuation cleanup.

Raw admission tests retain their negative pointer cases without dispatching
handlers. State-service assertions now distinguish COMPLETE from INTERRUPTED
instead of accepting either nonzero result. Existing real/VM86 record and
raw-mode round-trip coverage remains intact.

`test/dpmi_state_recovery.S` builds four COMs with
`make -C arch/x86 CLANG=1 dos-state-recovery-test`: `dpsr16`, `dpsr32`,
`dpsrs16` (USE32/SS16) and `dpsrs32` (USE16/SS32). Each executes fourteen
cases with real FAR calls: save/restore repair, repeated faults, nested
#BP, edited CS:EIP cancellation, edited DF, changed AL/EDI/ES and nested
state calls. They compare opaque state bytes, unchanged cancelled output,
GPRs/FLAGS, the original FAR caller and full SS:ESP. All four emulated and
both native USE32 stack variants print `DPMI STATE RECOVERY PASS` and exit
2Ah. Native guest page-table repair is not claimed by these selector tests.

Final verification used the 6,035,672-byte kernel with SHA-256
`d018e8214296490d920b9d08f379cfb9ce5b61289494234ffa8269f044cf59e8`.
The clang build, four COM builds and `make -q` check pass. The isolated
KVM VM used 8 GiB, four CPUs, a disposable 128 MiB OsitoFS3 snapshot,
VNC `:12` and UDP 7790. The complete `dos-api-test` passed at serial line
212260; the new matrix passed at line 4410. The six new COM runs passed
at lines 664, 957, 1251, 1535, 1774 and 1952. Fifteen independent probes,
six descriptor/info runs, six real-file recovery runs, six file/console
runs, three native segment runs, six real-call buffer runs, four software
frame runs, three argument runs and six control runs also passed.

Both emulated debug probes and emulated EXEC passed. Native FLAGS and
ICEBP retained 71h at lines 212451 and 220931, not the success code 2Ah.
Native EXEC retained child 7Ch and parent 49 at lines 221229/221233;
its parent and DPMI-child fixture hashes match the previous archive.
These known gaps are not counted as successful runs.

Native DOOM with `-nosound` accepted F10/Y and exited zero after 12,611,506
mediated VGA accesses. Emulated DOOM with `-config audio.cfg -nomusic`
accepted F10/Y and exited zero at line 240862 after 656,654,908
instructions, including 208,297,334 JIT and 208,289,103 protected JIT
instructions. The cache retained 4,096 blocks, with 727,713 lookup
evictions, no code evictions and no resets. Both runs recorded four
IRQ1 deliveries. Subsequent PE32 `seh3`/`seh4` exited zero at lines
241203/241257, followed by an emulated FLAGS pass. These are regression
observations, not a performance comparison.

All eight PNGs were inspected: native gameplay, quit confirmation and
shell return; emulated title, gameplay at 300M, credits at 600M, quit
confirmation and shell return. Existing unscaled 320x200 output and
text box-drawing substitutions remain. The WAV contains 66.591 seconds
of 48 kHz stereo PCM, 4,918,141 nonzero samples, peak 31,905 and RMS
3,744.11. This establishes digital output, not music or fidelity.
Kernel, serial log, WAV, PNGs, 54 fixtures and verification metadata are
retained in `arch/x86/build/dos-state-recovery-verified-20260909/`.

The identical four new COMs were run six times against the archived 4.83
kernel (`a4af53cd1b6f989c01161e07041372986fe7e1fef96fc94444ede906b1ab6b47`).
All stopped at the first inaccessible save buffer: emulated exit -1 at
lines 433, 492, 551 and 601; native FFh at lines 663 and 713. None reached
the new kernel's PASS/2Ah result. These negative controls, matching COMs
and old kernel are in `arch/x86/build/dos-state-recovery-baseline-20260909/`.
The earlier pilot archive contains a 6,128-check version of the new matrix,
before adding explicit A/D assertions; its focused DPMI suite and existing
independent probes passed too.

All serial helpers were run sequentially, test VMs were stopped through
their own QMP sockets, and their exec sessions were reaped. User sessions
`:0` and `:1` were untouched. GitNexus was unavailable; impact was checked
manually. LF and `git diff --check` pass. No commit was made.

Callback registration/entry/return continuation, real/VCPI/VM86 copyout
recovery, general native guest paging, raw hidden descriptor state, extended
host-exception frames, native FLAGS/ICEBP, debug retirement, x87 ownership,
music, presentation and performance remain open. This is not complete
DPMI or DOS/4GW compatibility.

### 4.85 Restartable callback return records

The protected callback-return service now delivers typed #GP/#NP/#PF
record faults through the matching host-exception continuation instead of
turning every inaccessible record into a fatal callback error. It captures
the original ES:(E)DI and real-mode paging context before yielding, then
re-probes the whole 50-byte record after repair. A handler may change the
record's mapping or contents, but changing its own ES/EDI cannot retarget
the pending copy. Read-only return records remain valid.

The [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf),
pages 34-35, permits a different return buffer from the one registered
with 0303h. The callback prepares the real CS:IP and SS:SP and returns by
IRET. This change extends the existing host's recoverable-copy policy; it
does not advertise DPMI 1.0 extended host-exception frames or capability
flags. Callback registration and entry from real mode are separate paths
and are not changed by this return fix.

The callback remains on the active mode stack throughout repair. A nested
callback cannot free the suspended callback through 0304h, and a balanced
nested invocation must return to the same slot and depth. Only after the
complete read does the host pop the callback, restore the enclosing real
cursor/virtual IF, save the current protected CR0/CR3 and switch to the
captured real address space. A nested 0305h restore cannot replace that
pending real-mode destination. Admission does not set record-page A/D
bits or consume any partial payload.

The C result distinguishes invalid entry/frame, complete transition and
interruption. Both dispatchers preserve handler-directed cancellation or
termination. In particular, the native dispatcher no longer interprets
remaining in protected mode after a cancelled read as a failed real-mode
switch. The private interpreted INT frame is removed once by exception
delivery; the callback's IRET frame was already consumed. A cancelled
client may rebuild its saved IRET frame and retry while its callback
remains active.

The in-OS matrix passes 3,664 checks with 208 recovered reads and zero
failures. It crosses USE16/USE32, SS16/SS32, low/PDE-crossing records and
direct/decoded private INT dispatch. Cases cover absent, read-only and
supervisor pages, invalid descriptor type/presence/limit, physical backing
outside guest RAM, repeated faults, nested #BP, nested callbacks, 0304h
rejection while active, cancelled-and-retried returns, explicit exit,
quota exhaustion, unhandled exceptions, changed ES/EDI/GPRs, edited record
contents, remapped pages and a nested dormant-state restore. It checks
real registers/FLAGS, error/CR2, full USE32 exception ESP, stack ownership,
current/dormant paging, neighboring sentinels, A/D bits and wait cleanup.
Existing raw admission tests retain side-effect-free negative coverage.

`test/dpmi_callback_recovery.S` builds `dpcr16`, `dpcr32`, `dpcrs16`
(USE32/SS16) and `dpcrs32` (USE16/SS32) with
`make -C arch/x86 CLANG=1 dos-callback-recovery-test`. Each runs eleven
cases through 0303h/0304h, real FAR calls and IRET: read-only/exact-limit
records, short limits, repeated repair, nested #BP, changed ES/EDI,
cancellation with reconstructed IRET, a nested callback with active-free
rejection, selector relocation and modified record contents. Raw 0306h
switches place the outer callback outside an enclosing native INT 31h,
so USE32 native runs exercise the native callback-return gate itself.
All four emulated and both USE32 native stack variants print
`DPMI CALLBACK RECOVERY PASS` and exit 2Ah. The selector fixtures do not
claim native guest page-table repair.

The final clang build is a 6,048,016-byte kernel with SHA-256
`f837b79f5d8ee3e62524bdc5bbfa489db15128cbd1a2a55cf4a5c02b7d10bca8`.
The build and `make -q` with matching `CLANG=1 QUIET=1 SKIP_MODEL=1`
options pass. The isolated KVM VM uses 8 GiB, four CPUs, a disposable
128 MiB OsitoFS3 snapshot, VNC `:12` and UDP 7790. The complete
`dos-api-test` passed at serial line 214112; the new matrix passed at
line 9775. The six new COM runs passed at lines 688, 1021, 1355, 1663,
2100 and 2476, with a native callback-return gate logged at line 1987.

Fifteen existing independent probes, six state-procedure runs, six
descriptor/info runs, six real-file recovery runs, six file/console runs,
three native-segment runs, six real-call buffer runs, four software-frame
runs, three argument runs and six control runs passed. Both emulated
debug probes and emulated EXEC also passed. Native FLAGS and ICEBP still
exit 71h at lines 214303 and 224291, not the successful 2Ah. Native EXEC
still exits with child 7Ch and parent 49 at lines 224589/224593; these
known failures are not counted as successes.

Native DOOM with `-nosound` reached gameplay and returned to the shell with
exit 0 after F10/Y (line 228568), with 12,725,864 mediated VGA accesses.
Emulated DOOM with `-config audio.cfg -nomusic` also returned with exit 0
after F10/Y (line 244067), at 659,489,340 instructions, including
210,922,403 JIT instructions and 210,914,148 protected-mode JIT instructions.
The post-DOS `seh3_pe32` and `seh4_pe32` checks returned 0 at lines
244408/244462; emulated FLAGS also passed. The 66.124-second, 48 kHz stereo
capture contains 4,825,868 nonzero samples, peak 32768 and RMS 3736.672.
This demonstrates digital PCM output, not music support or audio fidelity.
All eight retained screenshots were inspected: native gameplay/quit/shell,
emulated title/gameplay/credits/quit/ENDOOM. The 600M screenshot is credits,
not gameplay; unscaled presentation and missing ENDOOM text glyphs remain.

The evidence is retained under ignored
`arch/x86/build/dos-callback-recovery-verified-20260909/`, including the
kernel, serial log, WAV, eight PNGs, COM fixtures and a hash manifest.
The EXEC parent/child hashes match the preceding 4.84 evidence exactly.
A separate control in `dos-callback-recovery-baseline-20260909/` used the
6,035,672-byte 4.84 kernel, SHA-256
`d018e8214296490d920b9d08f379cfb9ce5b61289494234ffa8269f044cf59e8`,
and the identical four new COM files. All six runs stopped on their first
inaccessible return record: emulated exit -1 at lines 461/550/639/717,
native exit FFh at lines 826/923. None printed the new PASS marker.
These expected control failures distinguish the fix from a fixture that
never reaches the faulty path. Both owned VMs and their helpers were
stopped and reaped; the user's VNC sessions were not touched.

Callback registration/entry continuation, real/VCPI/VM86 copyout recovery,
general native guest paging, raw hidden descriptor state, extended host
exceptions, native FLAGS/ICEBP/EXEC, debug retirement, x87 ownership, music,
presentation and performance remain open. This is not complete DPMI or
DOS/4GW compatibility.

### 4.86 Restartable callback registration

INT 31h/0303h now separates inaccessible client pointers from unavailable
callback resources. The [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf),
Function 0303h and Appendix B, assigns 8015h to callback allocation failure and
permits invalid function pointers to be reported through an exception
handler. This implementation extends the host's existing recoverable-copy
policy; it does not advertise the extended DPMI 1.0 exception frames or
restartability capability flags.

The request captures DS:(E)SI and ES:(E)DI before running a handler. The
code-target probe reports #GP for invalid type/range/backing, #NP for a
non-present descriptor and #PF with the guest fault address/error for a
missing or inaccessible page. It retains the existing code-selector
eligibility rules. The 50-byte register record must be writable on entry,
unlike the read-only record accepted by callback return. Neither probe
reads an instruction or record payload, writes a record, nor sets its
page A/D bits. Allocation does not claim to lock or pin client pages.

Both admissions are repeated after every completed repair. A record
handler may invalidate an already admitted code target, so retaining the
earlier result would be incorrect. Changed DS/ES/ESI/EDI values do not
retarget the pending request, but descriptor changes and remapping are
observed. A cancelled or terminated handler abandons allocation without
overwriting its selected destination, flags or exit status.

Only after successful admission does the allocator choose a callback slot
and check its real-mode stub range. A nested registration can consume the
first free slot while the outer request is suspended. The host then
obtains the shared exception-stack/code descriptors and the callback's
real-stack alias. Resource failure releases only newly allocated host
descriptors, restores prior shared descriptor contents/selectors and the
allocation cursor, and returns CF/AX=8015h. No partial callback or stub is
published. Shared descriptors already owned by the host remain owned.

`dpmi_callback_registration_selftest` passes 7,960 checks with 320 repaired
requests and zero failures. Its 32 policies cross USE16/USE32, SS16/SS32,
low/PDE-crossing records and direct/decoded INT 31h dispatch. It covers
record and code permissions/presence/limits/backing, address overflow,
read-only code, repeated faults, nested #BP and registration, changed
arguments, cancellation followed by retry, exit, quota, unhandled faults,
revalidation of a changed code target and remapped records. Resource
cases cover all callback slots occupied, zero/one/two free descriptors,
pre-existing shared descriptors and a truncated real stub. Checks include
exception fields/full USE32 ESP, unchanged records and A/D bits, callback
ownership, descriptor rollback, successful free and private-frame/wait
cleanup.

`test/dpmi_callback_register.S` builds four independent COM files through
`make -C arch/x86 CLANG=1 dos-callback-register-test`: `dprg16`, `dprg32`,
`dprgs16` (USE32/SS16) and `dprgs32` (USE16/SS32). Each runs eleven
registration cases, invokes each successfully registered outer callback
through a real FAR call and IRET, verifies its returned registers and
frees it. It then exhausts the callback pool, requires at least sixteen
successful allocations, frees them and checks duplicate-free rejection.
All four emulated and both USE32 native variants print
`DPMI CALLBACK REGISTER PASS` and exit 2Ah. Native COMs exercise selector
limit/type/write-permission repair; guest paging cases are in the C matrix.

The verified clang kernel is 6,064,464 bytes, SHA-256
`dd777b1dd2585ad14e4dc3ea4649d478e8a62b6b012b386ab65f718557fb885d`.
Build and `make -q` pass with matching `CLANG=1 QUIET=1 SKIP_MODEL=1`
flags. The isolated KVM VM uses 8 GiB, four CPUs, a disposable 128 MiB
OsitoFS3 snapshot, VNC `:12` and UDP 7790. The six COM PASS markers are at
serial lines 742/1124/1506/1868/2208/2488. The registration matrix passed
again at line 10860; the preceding callback-return matrix retained its
3,664 checks, 208 recoveries and zero failures at line 12189. The complete
DOS API suite passed at line 216545, including JIT-cache 10,466/0 and
JIT-contract 3,258/0 checks/failures.

Fifteen existing independent probes and the callback-return, control DOS,
native-segment, software-frame, real-call buffer, state, descriptor/info,
file recovery, file/console and argument groups passed. Both emulated
debug probes and emulated EXEC passed. Native FLAGS and ICEBP still exit
71h at lines 216740/228764, and native EXEC still exits with child 7Ch and
parent 49 at lines 229068/229072. These are known failures, not successes.
Native DOOM with `-nosound` returned normally through F10/Y with exit 0
at line 233231, after 12,807,895 mediated VGA memory accesses.
Emulated DOOM with `-config audio.cfg -nomusic` also returned through
F10/Y with exit 0 at line 248714, after 653,460,029 instructions including
205,604,748 JIT instructions and 205,596,575 protected-mode JIT
instructions. Post-DOS `seh3_pe32` and `seh4_pe32` returned 0 at lines
249055/249109; emulated FLAGS passed again. The 62.928-second, 48 kHz
stereo WAV contains 4,661,617 nonzero samples, peak 32768 and RMS 3822.531.
This verifies digital PCM output, not music support, fidelity or a
performance improvement.

All eight screenshots were inspected: native gameplay/quit/shell and
emulated title/gameplay/credits/quit/ENDOOM. The 600M capture shows
credits, not gameplay. Centered unscaled graphics and missing ENDOOM
border glyphs remain presentation gaps. The kernel, serial log, WAV,
eight PNGs, COM fixtures and hash manifest are retained in ignored
`arch/x86/build/dos-callback-register-verified-20260909/`. The EXEC
parent/child fixture hashes match the preceding 4.85 evidence exactly.

The separate control in `dos-callback-register-baseline-20260909/` used
the 6,048,016-byte 4.85 kernel, SHA-256
`f837b79f5d8ee3e62524bdc5bbfa489db15128cbd1a2a55cf4a5c02b7d10bca8`,
and the identical four new COM files. All six runs failed at the first
inaccessible record, reporting `FAIL case=1 faults=0`: emulated exit 113
at lines 450/528/605/672 and native exit 71h at lines 754/826. No new PASS
marker was printed. The second marker is split by a framebuffer trace
at lines 524/525; the checker removes only that trace before matching.
These expected failures demonstrate that the fixtures reach the old
non-recoverable registration path. Serial line references count LF
delimiters, including the log's CR/CR/LF output. The pilot, primary and
control VMs and their helpers were stopped and reaped without touching
the user's VNC sessions.

Real-mode callback entry still uses non-resumable admission in the dormant
protected address space. It cannot borrow the protected-mode caller's
exception context. That continuation, real/VCPI/VM86 recovery, general
native paging/hidden descriptor state, extended host exceptions, native
FLAGS/ICEBP/EXEC, debug retirement, x87 ownership, music, presentation and
performance remain open. This is not complete DPMI or DOS/4GW support.

### 4.87 Extended protected-mode exception frames

INT 31h/0210h and 0212h now get/set the protected-mode exception owner.
They share vector validation and ownership with 0202h/0203h. Once an
extended owner has been installed, that vector retains the expanded frame
until client reset, so a subsequently installed legacy owner can FAR JMP
to an earlier extended handler. The [DPMI 1.0 specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf),
pages 30-33 and Functions 0210h/0212h, defines these mixed handler chains.
This does not implement the real-mode 0211h/0213h vectors or advertise the
0401h Exceptions Restartability capability.

The 88-byte frame retains the legacy USE16/USE32 prefix and places the
extension at +20h for either client width. It includes full EIP/EFLAGS/ESP,
CS/SS/ES/DS/FS/GS, host information bits and page-fault CR2/PTE low bits.
The non-PAE PTE snapshot is taken before storing the handler frame; an
absent PDE or unavailable PTE has a zero image. The debug error field
currently represents TF in virtual DR6 bit 15 only, not implemented DPMI
watchpoints or complete debug-event retirement.

Frame admission snapshots both page translations before touching payload
or A/D bits, including non-contiguous physical pages and PDE crossings.
The fixed-record transport and temporary stack payload now admit 88 bytes.
The handler cursor is published only after the complete store succeeds.
The old return stub selects only the old fields; discarding 20h and using
the extended RETF selects only the extension. USE16 skips the reserved
DWORD after its packed far return address and restores full ESP. Return
admission validates CS/SS and all four data selectors before changing CPU
state. Null data selectors and readable code are accepted; invalid types,
privilege, presence, truncated frames and VM-mode returns are rejected.

Each nesting level records private frame format and host continuation.
Host-service faults are tagged only during their own delivery, not when a
handler subsequently raises an ordinary client exception. Default extended
host return retries the saved service continuation even if the public
CS:EIP or host bit was edited. Bit 2 explicitly cancels the pending service
and honors the selected destination, including cancellation to the same
post-INT address. Legacy return behavior remains unchanged. Nested waits
retain their parent and are removed on return, cancellation or termination.

`dos_dpmi_extended_exception_selftest` passes 4,744 checks across client
width, stack width, direct/private-INT/decoded-RETF paths and 23 scenarios.
It checks both frame choices, ignored edits to the unchosen frame, data
selector admission/reload, full ESP, host classification/retry/redirection,
nested frames, CR2/PTE/debug images and exhausted-stack rollback. The
expanded `dos_dpmi_paged_frame_selftest` passes 640 checks, including
88-byte delivery and return across two pages, read-only return mappings,
missing/forbidden pages, expand-down bounds and cached stack descriptors.
The short `dos-dpmi-stack-test` passes with both matrices included.

`test/dpmi_extended_exception.S` builds `dpee16`, `dpee32`, `dpees16`
(USE32/SS16) and `dpees32` (USE16/SS32) with
`make -C arch/x86 CLANG=1 dos-extended-exception-test`. Its nine stages
exercise actual #BP/#UD delivery, old/new RETF, old-to-new FAR chaining,
full ESP and ES edits, recoverable 0303h calls, explicit cancellation,
an ordinary #BP and another failing host call nested inside a host
handler, and exit from the final handler. All four emulated variants and
both USE32 native variants print `DPMI EXTENDED EXCEPTION PASS` and exit
2Ah. The native variants exercise the private return gate as well as the
interpreted handlers used during host-service suspension.

The current 6,080,960-byte kernel has SHA-256
`88adcc0488eecf11fa4e2e0307af96f07df14770b9023666d2b2d5b987cf2598`.
The isolated KVM configuration remains 8 GiB/four CPUs, VNC `:12`, UDP
7790 and a disposable OsitoFS3 snapshot. Evidence is retained under
`arch/x86/build/dos-extended-exception-verified-20260909/` and
`arch/x86/build/dos-extended-exception-baseline-20260909/`. The live serial
log is mirrored to the workspace so a WSL restart cannot discard the only
copy. An earlier positive pilot lost its temporary logs in a WSL restart;
it is not used as the retained verification archive.

The baseline runs the identical four COM files against the preceding
6,064,464-byte kernel, SHA-256
`dd777b1dd2585ad14e4dc3ea4649d478e8a62b6b012b386ab65f718557fb885d`.
All six invocations fail at the first unsupported 0212h call: four
emulated exits are 113 and two native exits are 71h. Its
`verification-summary.json` records fixture hashes and serial line numbers;
there are no unexpected passes. This distinguishes new extended-frame
coverage from success on an implementation that ignores the new service.

The persistent run also passes the callback registration/return, control,
native segment, software frame, service buffer, saved state, descriptor
record, file/console and argument regressions. Emulated debug and EXEC
pass; native ICEBP and native EXEC retain their documented failures. The
first EXEC audit stopped because its checker expected the wrong child-exit
message. The corrected checker requires the actual `Exit(0x7C)`, resumed
interpreter and failing parent exit 49 sequence, which is also present in
the preceding kernel's evidence. That is a known gap, not a passing EXEC
contract or a runtime fix.

DOOM renders in native `-nosound` and emulated digital-audio runs and exits
normally with F10/Y. The emulated run exceeds 600 million instructions.
Retained screenshots show changing 320x200 content centered in a 1280x800
display; scaling/presentation remains open. Both PE32 SEH probes and an
emulated FLAGS probe pass after DOOM. The closed WAV contains 65.48 seconds
of 48 kHz, 16-bit stereo PCM, with 4,846,973 nonzero samples. This checks
digital output, not music support or subjective audio quality.

The complete DOS API suite passes at `serial.log:254142`, including the
4,744/640 frame matrices, 10,466 JIT-cache and 3,258 JIT checks, all with
zero failures. Its legacy COM regressions finish with 15 passing runs and
the known native FLAGS failure (71h). Both PE32 SEH programs and another
emulated FLAGS probe pass again after the suite. The original 30-minute
monitor timed out while the guest was still progressing; a continuation
observer verified the existing serial completion and ran only the remaining
commands. `dos-api-test` was not reissued, QEMU was not restarted, and the
original monitor error is retained in `full-api-monitor-timeout.log`.
Future full-suite monitor deadlines are one hour; other command deadlines
are unchanged.

The verified archive includes scoped sources, harness scripts, fixture and
artifact hashes, serial marker lines, PCM statistics and checked captures
in `verification-summary.json`. The isolated VM, serial mirror and test
helpers are stopped. The earlier EXEC checker error and the full-suite
monitor timeout are preserved separately from guest contract failures.

Real-mode callback entry needs its own suspended real context and extended
exception continuation, rather than borrowing a protected caller's frame.
That work, real/VCPI/VM86 recovery, general native paging/hidden descriptor
state, native FLAGS/ICEBP/EXEC, debug retirement, x87 ownership, music,
presentation and performance remain open.

### 4.88 Extended real-mode exception delivery

The active DPMI client now has a separate real-mode exception table,
registered/read through 0213h/0211h. This does not alias 0212h/0210h or
the real IVT. The default addresses remain host stubs, whose private
exception context distinguishes protected from real origins. The setter
validates code selectors and preserves the client's 16/32-bit offset ABI.

`cpu_deliver_exception` routes real-mode processor exceptions through
this table while a DPMI client is active. Guest-owned IDT/VCPI/VM86 events
retain their CPU delivery path. A real exception enters protected mode
on the locked stack, using the suspended protected CR0/CR3. Its private
context retains the interrupted CPU, hidden segment state, real paging
and suspended cursors. This context remains live until the matching
return, including nested protected and real exceptions.

Real origins use the 88-byte extended frame described in the
[DPMI 1.0 specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf).
The public EFLAGS image indicates real mode with VM; this synthetic bit
does not change the actual saved CPU execution mode. ES/DS/FS/GS are
cleared on protected entry. The legacy prefix is not a usable return
frame, and an old-format RETF is rejected. Extended return restores
edited real CS:EIP, full ESP, segment bases and FLAGS while preserving
the saved real hidden/control state. Clearing VM instead selects a
protected destination and applies the existing protected selector checks.

Chaining to the default handler completes the extended return and then
builds the real IVT handler's IRET frame. It does not reflect the event
through a second protected-to-real simulation. Unhandled #UD, vectors
8..31 and a missing real #DE handler terminate the guest with a diagnostic.
Failed handler delivery/return removes the private real-context pointer;
it must not outlive its synchronous host activation.

The frame matrix has 1,720 checks across both client widths, both stack
widths, direct/private-INT returns and 14 scenarios. They cover separate
vector ownership, frame contents, arbitrary real segment values, edited
EIP/ESP/DS, hidden limits, protected return selection, nested PM frames,
default IVT/fatal policy, CR2/PTE images, exhausted stacks and invalid
returns. The existing 4,744 extended PM and 640 paged-frame checks also
pass, as does the short stack suite.

`test/dpmi_real_exception.S` builds `dpre16`, `dpre32`, `dpres16`
(USE32/SS16) and `dpres32` (USE16/SS32) with
`make -C arch/x86 CLANG=1 dos-real-exception-test`. All four emulated
variants and both native USE32 variants pass. They execute real #BP/#UD
inside 0301h, a protected #BP nested in the real-exception handler,
another real #UD nested through 0301h, extended RETF and FAR chaining to
the default handler. A literal software INT 3 still invokes the IVT
handler without entering the processor-exception chain.

The retained candidate is the 6,131,776-byte kernel with SHA-256
`b168b57b737d32d73c9297665fbde11486fc81e55cb5318a0664485ffb801665`.
Its verified evidence directory is
`arch/x86/build/dos-real-exception-run2-20260909/`. The initial pilot
passed the frame tests. The first COM trial exposed a fixture bug:
the mixed-width test switched to DS instead of its configured stack
descriptor. The corrected fixture selects the intended SS; no kernel
change was needed for that failure. Earlier trial directories are not
the final verification result.

The identical corrected COM files were also run against the preceding
6,080,960-byte kernel (`88adcc04...2598`) in
`arch/x86/build/dos-real-exception-baseline2-20260909/`. All six runs fail
at unsupported 0211h: four emulated exits 113 and two native exits 71h.
The negative control and the candidate use the same isolated KVM setup:
8 GiB, four CPUs, VNC `:12`, UDP 7790 and disposable OsitoFS3 snapshots.

The completed run has 96 checked invocations: 93 pass and three retain
the known native FLAGS, ICEBP and EXEC failures. DPMI, stack, extended PM,
callback registration/return, control, segment, software-frame, buffer,
saved-state, descriptor-record, I/O and argument regressions pass.
The real-frame matrix is at `serial.log:21911`, the PM matrix at 21765,
the paged-frame matrix at 22785, DPMI suite PASS at 20396 and stack suite
PASS at 25624 (LF-based line numbers). The exhaustive `dos-api-test` was
not rerun in this checkpoint; the preceding section's full-suite result
is historical evidence, not a result for this new kernel.

Native DOOM (`-nosound`) and emulated DOOM (`-config audio.cfg -nomusic`)
render and exit normally via F10/Y. The emulated run completes
662,946,364 instructions. The closed WAV contains 57.02 seconds of 48 kHz,
16-bit stereo PCM and 4,261,615 nonzero samples. Two PE32 SEH probes and
emulated FLAGS pass after both runs. Eight inspected captures retain
gameplay, quit prompts and shell returns; game images remain 320x200
centered in 1280x800, so presentation/scaling is still open.

`verification-summary.json` verifies both kernel identities, identical
fixture hashes, completed groups, serial markers, source/harness snapshots,
PCM statistics and capture pixels. The VM, mirrors and helpers are stopped.

This is not full DPMI 1.0 or complete DOS support. Routing to a suspended
primary client across EXEC/RSP boundaries remains open: only the active
`vm.dpmi` context is consulted here. Callback entry still returns a
failure for an inaccessible protected record; it needs a real-mode
host-service continuation, typed cancellation and atomic re-admission.
The direct matrix tests host retry/redirection frame semantics, not a
working callback-entry repair path. General native paging/hidden state,
native FLAGS/ICEBP/EXEC, debug retirement, x87 ownership, music,
presentation and performance also remain open. Version/capability
advertisement is unchanged.

### 4.89 Restartable real-mode callback entry

Callback admission now uses the active client's real-mode exception table
when its registered protected code or 50-byte register buffer fails
validation. It reports the actual #GP/#NP/#PF metadata rather than treating
every inaccessible buffer as an invalid callback. The suspended CPU is
real mode; the host fault's CR2/PTE image describes the dormant protected
mapping that the callback needs to access.

The continuation uses the 88-byte real exception frame and HOST/REDIRECT
semantics from the [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf).
It consumes the private INT FB frame once, before delivering the first
fault. A successful matching return retries both code and record admission
using the repaired protected mapping and real registers/stack. It does
not replay the private frame read or restore the original real SP over
a handler's edit. Cancellation, guest exit and failed delivery return a
distinct interrupted result; the dispatcher does not replace that outcome
with another invalid-entry error. The real host-INT bridge also checks
whether its frame was consumed, including same-CS:EIP cancellation.

The register record and locked-stack return frame are not published until
admission completes. Both output translations retain the existing joint
A/D-before-payload commit rule. Callback slots have monotonically increasing
registration identities: a fault handler may free/reallocate a slot, but
the suspended entry cannot silently enter that replacement. Counter
exhaustion returns 8015h without allocating resources or wrapping identity.
Freeing an already executing callback remains rejected as before.

The new C matrix has 3,800 checks, with 192 recovered entries. It covers
USE16/USE32, both protected stack widths, direct entry, the real host-INT
bridge and decoded INT FB. Cases include code/record permissions, limits,
missing pages, repeated repair, code invalidated by a record repair,
remapping, real GPR/segment/SS:ESP edits, nested protected exceptions,
same/different-address cancellation, free/reuse, guest exit, absent handlers
and bounded execution quotas. The 2,010 existing entry-frame checks and
416 paging-state checks also pass; their no-handler expectations now
require a controlled fatal real exception for inaccessible callback buffers,
while still checking exact CPU state and untouched payloads.

`test/dpmi_callback_entry.S` builds four independent COM files with
`make -C arch/x86 CLANG=1 dos-callback-entry-test`: `dpce16`, `dpce32`,
`dpces16` (USE32/SS16) and `dpces32` (USE16/SS32). All four emulated and
both native USE32 variants pass nine real FAR-call scenarios each. They
exercise code/buffer descriptor repair, revalidation, nested PM exceptions,
edited real EAX/stack, IRET to the actual real caller, explicit cancellation
and raw mode-switch return. The stack variants describe the protected
caller; the dedicated callback/exception stack retains its host-selected ABI.

The preceding kernel (`b168b57b...1665`) fails all six variants at callback
admission with the same corrected COM bytes. Evidence is retained in
`arch/x86/build/dos-callback-entry-baseline2-20260909/`. The first fixture
trial incorrectly expected a 16-bit raw switch to restore ESP's high half;
the fixture was corrected, not the kernel. One native negative-control run
was repeated because the checker expected a native termination message
instead of the interpreter's real-mode exit. That archive contains seven
actual runs and six checked variants, not seven distinct tests.

The final 6,144,176-byte kernel has SHA-256
`48b1d90af0fa0580bb11c7e430663f1e2c37bb55b3b1649fe9c9aaa806e9a06f`.
The verified archive is `arch/x86/build/dos-callback-entry-run1-20260909/`.
It contains 102 checked invocations: 99 passes and the three known native
FLAGS, ICEBP and EXEC failures. DPMI, stack, old/new exception, callback,
control, segment, software-frame, buffer, saved-state, descriptor-record,
I/O and argument regressions pass. The new entry matrix is at
`serial.log:15127`, DPMI suite PASS at 23933 and stack suite PASS at 29143
(LF-based lines). The exhaustive `dos-api-test` was not rerun; its older
result is not evidence for this kernel.

Native DOOM (`-nosound`) and emulated DOOM (`-config audio.cfg -nomusic`)
render and exit normally through F10/Y. The emulated run completes
661,815,869 instructions. The closed WAV contains 59.64 seconds of 48 kHz,
16-bit stereo PCM, with 4,473,986 nonzero samples. This proves digital
output, not music support or subjective audio quality. Both PE32 SEH probes
and emulated FLAGS pass again after DOOM. Eight inspected captures preserve
the title/gameplay, quit prompts and shell returns; gameplay remains
320x200 centered in 1280x800, so presentation/scaling is still open.

`verification-summary.json` checks the kernel identities, identical fixture
hashes, completed groups, serial markers, PCM, capture pixels and archived
sources/harnesses. The pilot's 24 old no-handler expectations and the two
fixture/checker issues are retained separately from the verified result.
The isolated KVM VM (8 GiB, four CPUs, VNC `:12`, UDP 7790, disposable
OsitoFS3 snapshot), serial mirrors and test helpers are stopped.

This remains active-client support, not full DPMI 1.0 or a complete DOS
layer. An unreadable private real INT frame or unavailable host locked stack
is still an invalid-entry error. Suspended-primary-client routing across
EXEC/RSP and general cross-mode cancellation ownership remain open, as do
native paging/hidden state, native FLAGS/ICEBP/EXEC, debug retirement, x87
ownership, music, presentation and performance. Capability advertisement
is unchanged.

### 4.90 DPMI primary-client lifetime across real-mode EXEC children

DOS EXEC now preserves a live primary DPMI client while the new DOS process
remains in real mode. Creating a PSP is not itself an initial DPMI entry.
The complete primary state stays live, including callbacks, real exception
vectors, dormant paging/stack state, locked-stack ownership and host-service
continuations. Changes made by its handlers during a child survive that
child's return. This also applies to nested children and 4B01h load-only
execution, without changing the DOS-visible current PSP to the parent's.

An executing child that requests its own initial DPMI entry takes a snapshot
of the latest primary state, then initializes its own client state. A failed
entry restores that snapshot transactionally; successful entry records the
child's PSP as client owner. Returning from the child restores the previous
client. The explicit client owner also stamps DPMI 0100h conventional-memory
allocations, so a primary handler's allocation is not freed as child-owned
memory when the real child exits. Ordinary DOS allocation still uses the
current DOS PSP.

Two native EXEC lifetime errors became visible in the new fixtures. A 4B01h
return with an interpreted INT frame must not decrement an outer native
dispatch depth. Also, the saved host TLS token belongs to the same backend
as its CR3/GDT/LDT: detaching those tables without detaching the token lets
child teardown invalidate the parent's LDTR. EXEC now snapshots, detaches
and restores that ownership together with the saved IDT image. Neither fix
depends on the executable name or a binary-specific instruction address.

The new C matrix passes 642 checks across two client widths, real/protected
parents, one to three nested children and five initial-entry policies. It
checks persistent handler mutations, host-wait identity, allocation owners,
fresh child state and byte-exact rollback/parent restoration. The expanded
continuation matrix passes 41 checks, including all combinations of guest
INT frame size 0/6/12 and outer native depth 0/1/2, plus TLS/IDT restoration.

`test/dpmi_exec_primary.S` builds seven COM files with
`make -C arch/x86 CLANG=1 dos-exec-primary-test`. Four parent variants cover
USE16/USE32 and SS16/SS32. Their real children exercise nested EXEC, missing
files, load-only execution, rejected initial entry and a later valid child
entry. Parent real #BP/#UD handlers, callbacks and edited timer vectors must
remain usable, including when the sequence starts on a protected exception
stack. All four emulated and both native USE32 parent variants pass.

The final 6,152,616-byte kernel has SHA-256
`6cc1610462d03c92705d31b823cf6686fd43027cb369e9818463160fdb1f28df`.
Its evidence directory is
`arch/x86/build/dos-exec-primary-run3-20260909/`. The identical seven COM
files were run against the preceding kernel (`48b1d90a...06f`) in
`arch/x86/build/dos-exec-primary-baseline1-20260909/`. All six parent runs
fail at the child's unhandled real #BP: four emulated exits 112 and two
native exits 70h. There is no kernel halt in that negative control.

The initial fixture trial incorrectly wrote counters through CS; it was
corrected to use the data selector. The next two candidates passed the four
emulated cases but halted on native return with #GP at the saved ES load.
Those trials exposed the dispatch-depth and TLS-ownership bugs above; they
are retained separately and are not completed regressions.

The completed candidate round has 108 checked suite invocations: 105 passes
and the three known native FLAGS, ICEBP and legacy EXEC failures. Unlike
the two preceding checkpoints, this run includes the exhaustive
`dos-api-test`, which passes at `serial.log:218715`. The new primary matrix
is at line 1927, continuation results at 1773 and 1944, and the additional
stack suite PASS at 226191 (LF-based lines). Existing callback, exception,
control, buffer, saved-state, I/O, argument and segment regressions pass.

Native DOOM (`-nosound`) and emulated DOOM (`-config audio.cfg -nomusic`)
render and exit normally through F10/Y. The emulated run completes
661,979,708 instructions. The closed WAV contains 61.61 seconds of 48 kHz,
16-bit stereo PCM and 4,585,964 nonzero samples; this does not establish
music support or subjective audio quality. Both PE32 SEH probes and
emulated FLAGS pass after DOOM. Eight inspected captures show title/gameplay,
the demo transition, quit prompts and shell returns. Game output remains
320x200 centered in 1280x800; presentation/scaling is still open.

`verification-summary.json` checks both kernel identities, identical
fixtures, completed groups, serial markers, PCM statistics, capture pixels
and source/harness snapshots. Both isolated KVM VMs (8 GiB, four CPUs,
VNC `:12`, UDP 7790, disposable OsitoFS3 snapshots), serial mirrors and
test helpers are stopped. Other VNC sessions were not touched.

This implements a bounded part of the primary/current-client distinction
in the [DPMI specification](https://www.sudleyplace.com/dpmione/dpmispec1.0.pdf),
not complete multi-client or RSP support. General routing to a suspended
parent while a newer protected client is active, cross-client callback-stub
and host-stack allocation, shared paging ownership and cross-mode
cancellation remain open. Native paging/hidden state, FLAGS/ICEBP/EXEC,
debug retirement, x87 ownership, music, presentation and performance also
remain open. Version and capability advertisement are unchanged.

## 5. Open questions / notes
- `WINAPI` selects the Microsoft x64 ABI for native shims, not the kernel's
  SysV ABI. Export arg-count metadata also drives the **32-bit thunk's `RET n*4`**.
  So GT-argc must be the count of
  **32-bit stack DWORDs the 32-bit caller pushed**, not the 64-bit ABI.
- Variadic CRT fns (printf family) are cdecl → caller cleans, so a too-large
  argc (12) is harmless for cleanup; it only affects how many stack DWORDs the
  dispatcher copies. Keep but document.
- COM vtable methods (DD_*, Surf_*) are stdcall-with-`this`; argc includes `this`.
