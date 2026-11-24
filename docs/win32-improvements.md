# OsitoK Win32 Compat Layer — Improvement Plan

## Status (2026-04-24, commit 1d0b507)

UT99 boot was unblocked from "stuck in kern_longjmp triple fault after 338
_initterm callbacks" to "31,308+ INT 0x2E calls deep into engine init in 30s"
by a single off-by-one fix in `compat32.c`.

**Root cause**: `static uint64_t callback_jmpbufs[MAX_CALLBACK_DEPTH][8]`
reserved 64 bytes per slot, but `kern_setjmp` writes a **9-quad / 72-byte**
jmp_buf (`rbx, rbp, r12-r15, rsp, rip, cr3`). The setjmp at depth N+1 wrote
its rbx (slot[0]) on top of slot N's cr3 (slot[8]). Depth=0 longjmp after
_initterm reloaded `cr3 = 0x40` → triple fault inside kern_longjmp.

**Fix**: `[...][8]` → `[...][9]`. UT99 now reaches NtCreateFile / NtReadFile
of config files, NtAllocateVirtualMemory at the 0x40xxxxxx heap range,
CreateFileW with UTF-16 args, K32-THREAD spawn (TID=2), and USER32
CreateDialogParamW before hitting the next blocker (`[SCHED] CORRUPT PID 1`
— the scheduler frame-validity check fires when picking PID 1 with
`CS=0x1F10 RIP=0x14271EF0`, indicating PID 1's saved kernel_rsp interrupt-
frame area was overwritten between clean save and next dispatch).

Companion fix in `arch/x86/kernel/setjmp.S`: kern_longjmp now (a) pre-loads
all jmp_buf words into registers BEFORE the cr3 mov so a stale rdi mapping
no longer triple-faults the second mov, and (b) skips the cr3 mov when the
saved CR3 already matches the current CR3.

The IST1 stack-drift compensation in compat32.c (saving/restoring
`tss_ist1_ptr` per callback depth) was already in place from earlier work
and was NOT the bug — the buffer overflow was its own problem.

## Context (historical)

UT99 reaches Client+Lighting+Rendering initialized. Remaining crashes are from
IAT corruption by the Unreal package loader and missing API coverage. This doc
describes 7 improvements to the Win32 compat layer based on debugging findings.

## 1. IAT Auto-Discovery (replace hardcoded guards)

**Problem**: Each corrupted IAT entry requires a manually hardcoded guard.
Currently only StaticLoadClass (0x105A5E08) is protected.

**Solution**: When #PF intercept detects execution at a heap address (0x40xxxxxx),
automatically reverse-lookup the original function:

```
1. Scan Engine.dll .idata for entry matching the corrupt value
2. For each IAT entry in the import descriptor:
   - Read the Import Name Table (INT) to get the function name
   - Resolve the name in the exporting DLL's export directory
   - Get the correct function address
3. Fix the IAT entry and redirect RIP
4. Add to a dynamic guard list for future dispatch checks
```

**Files**: `idt.c` (#PF handler), `dllloader.c` (reverse lookup helper)

**Key data structures**:
- Import Directory: DataDirectory[1] → array of IMAGE_IMPORT_DESCRIPTOR
- Each descriptor has: OriginalFirstThunk (INT), FirstThunk (IAT), Name (DLL)
- INT entries have Hint + Name for each import
- Walk INT and IAT in parallel to find which import was corrupted

## 2. Callback Return Without longjmp

**Problem**: kern_longjmp bypasses the INT 0x2E stub restore path, requiring
complex compensations (per-depth RSP stack, RBP save in global, depth counters).

**Solution**: Replace longjmp-based callback return with a cooperative return:

```
Current flow:
  PE32 → return stub → INT 0x2E → dispatch → longjmp → callback_args return

New flow:
  PE32 → return stub → INT 0x2E → dispatch → set flag + retval → return
  → stub detects flag → skip register restore → return to callback_args
```

**Implementation**:
- Add `g_callback_return_pending` flag + `g_callback_return_value`
- In dispatch: if magic 0xFFFFFFFE, set flag and return normally (no longjmp)
- In int2e_stub.S: after `call compat32_dispatch`, check flag
  - If set: don't restore regs, adjust RSP to callback_args frame, jmp to return
  - If not set: normal restore + IRETQ

**Benefit**: Eliminates ALL longjmp-related bugs (RSP, RBP, register corruption).
This is the single highest-impact architectural improvement.

**Risk**: High — changes the fundamental callback mechanism. Needs careful testing.

## 3. Per-Thread Compat32 State

**Problem**: All callback state is global (callback_depth, jmpbufs, retval, stacks).
With SMP and real Win32 threading, concurrent PE32 threads corrupt each other.

**Solution**: Move to per-thread struct:

```c
typedef struct {
    int      depth;
    uint64_t jmpbufs[MAX_CALLBACK_DEPTH][8];
    uint64_t saved_ist1[MAX_CALLBACK_DEPTH];
    uint32_t retval_per_depth[MAX_CALLBACK_DEPTH];
    uint8_t  stacks[MAX_CALLBACK_DEPTH][CALLBACK_STACK_SIZE];
} compat32_thread_t;
```

**Indexing**: By kernel PID (sched_my_cpu() or current process index).
Allocate on first use per-thread. Store pointer in process_t or TEB.

**Files**: `compat32.c` (all callback_* globals), `int2e_stub.S` (RSP stack)

## 4. VirtualAlloc Caller-Aware Guard

**Problem**: VirtualAlloc rejects >512MB (corrupted TArray) but the call trace
only logs — doesn't prevent future corruption.

**Solution**: When VirtualAlloc rejects, use the call trace to identify the
corrupted IAT entry and auto-add it to the guard list:

```c
void va_rejected_add_guard(void) {
    // Get most recent PE32 caller from ring buffer
    uint32_t caller = g_call_trace[(g_call_trace_idx - 1) % CALL_TRACE_SIZE];
    // Scan .idata for entry that would route to this caller
    // Add to dynamic guard table
}
```

**Files**: `kernel32_shim.c` (VirtualAlloc), `compat32.c` (guard table)

## 5. Missing Win32 APIs for UT99

APIs that UT99 needs but we stub or handle incorrectly:

| API | Current | Fix |
|-----|---------|-----|
| GetPrivateProfileSectionNames | Returns empty | Enumerate INI sections |
| GetPrivateProfileSection | Returns empty | Return all key=value pairs |
| SetFilePointer (LARGE) | May truncate | Handle 64-bit offsets |
| GetFileAttributesA | Always FILE_ATTRIBUTE_NORMAL | Check if file exists |
| GetTempPathA | Returns empty | Return "." or "/tmp/" |
| GetSystemDirectoryA | Returns empty | Return "System/" |
| GetWindowsDirectoryA | Returns empty | Return "System/" |
| _controlfp | Stub | Set FPU control word properly |
| _isnan / _finite | May be missing | Implement with FPU checks |

**Files**: `kernel32_shim.c`, `msvcrt_shim.c`

## 6. .idata Write-Through via Single-Step

**Problem**: Page-level protection failed because the instruction decoder was
imprecise. Timer watchdog doesn't run with IF=0.

**Solution**: Use hardware single-step for precise write-through:

```
1. #PF on .idata write → make page writable + set RFLAGS.TF (trap flag)
2. IRETQ → CPU executes ONE instruction (the write)
3. #DB (debug exception) fires → save original IAT value → re-protect page → clear TF
4. Resume execution normally
```

**Prerequisite**: #DB needs IST entry for compat32 mode (currently no IST for #DB).
Add IST2 for debug exceptions, similar to IST1 for INT 0x2E.

**Files**: `idt.c` (#PF + #DB handlers), `int2e_stub.S` or TSS setup

**Risk**: Medium — requires IST setup for #DB which is non-trivial.

## 7. DLL Name Normalization

**Problem**: `dll_try_load_from_fs` can load duplicates with different case.

**Solution**: Normalize all DLL names to lowercase before comparison:

```c
static void normalize_dll_name(char *name) {
    for (int i = 0; name[i]; i++)
        if (name[i] >= 'A' && name[i] <= 'Z') name[i] += 32;
}
```

Apply in: `dll_find_module()`, `dll_load()`, `dll_try_load_from_fs()`

**Files**: `dllloader.c`

---

## Priority Order

| # | Improvement | Impact | Effort | Risk |
|---|-------------|--------|--------|------|
| 1 | IAT auto-discovery | HIGH | Medium | Low |
| 4 | VA caller-aware guard | HIGH | Low | Low |
| 5 | Missing Win32 APIs | HIGH | Low | Low |
| 7 | DLL name normalization | MEDIUM | Low | Low |
| 2 | Callback without longjmp | VERY HIGH | High | High |
| 3 | Per-thread compat32 | HIGH | High | Medium |
| 6 | .idata single-step | MEDIUM | High | Medium |

Start with 1, 4, 5, 7 (high impact, low risk). Then 2 and 3 (architectural).

---

## Kernel Parallel Session Audit Summary

34 new files (6,980 LOC) added in parallel sessions. All compile, all integrated.

### Issues found:
- klog.c: ring buffer wrap edge case
- ipv6.c: Echo Reply + Neighbor Advertisement TODO
- socket.c: MAX_SOCKETS=32 too low
- 2 unused variables (sysv_ipc.c, panic.c)

### New subsystems:
- Networking: DHCP, NTP, IPv6, mDNS, BSD sockets
- Storage: FAT32 R/W, tmpfs, ext2/NTFS/Btrfs/APFS read-only
- Security: capabilities, cgroups, namespaces, OOM killer
- IPC: SysV shm/sem, PTY, virtual terminals
- Drivers: virtio PCI/block/net
- Crypto: ChaCha20/Poly1305/SHA-512
- Advanced: kernel modules, TLS 1.3, SSH-2 server, RT scheduler
