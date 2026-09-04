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

## 5. Open questions / notes
- `WINAPI` is a no-op at 64-bit (shims run as native 64-bit); arg-count only
  drives the **32-bit thunk's `RET n*4`**. So GT-argc must be the count of
  **32-bit stack DWORDs the 32-bit caller pushed**, not the 64-bit ABI.
- Variadic CRT fns (printf family) are cdecl → caller cleans, so a too-large
  argc (12) is harmless for cleanup; it only affects how many stack DWORDs the
  dispatcher copies. Keep but document.
- COM vtable methods (DD_*, Surf_*) are stdcall-with-`this`; argc includes `this`.
