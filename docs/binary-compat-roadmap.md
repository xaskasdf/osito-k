# OsitoK Binary Compatibility Roadmap

Phased plan for running third-party binaries on OsitoK x86-64, separated from the
native inference path (X14/X15/X40). The native path runs Llama directly on bare metal
with custom tensor ops. This document covers running *other people's code*.

Target binaries, in order of difficulty:
1. Static Linux ELF (musl-linked) -- busybox, toybox, static Go
2. Dynamic Linux ELF (glibc-linked) -- Claude Code (227MB Bun+JSC)
3. Windows PE -- console applications via NT syscall translation

---

## Phase 1: Static Linux Binaries (current + 2-3 months)

### What exists

OsitoK already runs static ELF64 binaries compiled with TCC or GCC.

**ELF loader** (`arch/x86/kernel/elf.c`):
- ELF64 validation (magic, class, endian, machine, ET_EXEC/ET_DYN)
- PT_LOAD segment loading (contiguous allocation, fixed or PIE)
- Stack setup with argc/argv/envp (Linux-compatible layout)
- Memory tracking per process for cleanup on exit

**Syscalls** (`arch/x86/kernel/syscall.c`) -- 13 implemented:

| Nr | Name | Status |
|----|------|--------|
| 0 | read | Full (console + OsitoFS) |
| 1 | write | Full (console + OsitoFS) |
| 2 | open | Full (O_CREAT, O_APPEND, O_TRUNC) |
| 3 | close | Full |
| 5 | fstat | Full (S_IFREG, S_IFCHR) |
| 8 | lseek | Full (SET/CUR/END) |
| 12 | brk | Full (4MB lazy alloc) |
| 16 | ioctl | Stub (ENOTTY) |
| 20 | writev | Full |
| 21 | access | Exists check only |
| 60 | exit | Full (longjmp to kernel) |
| 87 | unlink | Full |
| 158 | arch_prctl | Stub (ENOSYS) |

**Process model** (`arch/x86/kernel/process.c`):
- 16-slot process table, PID allocation, per-process FD table
- setjmp/longjmp exec/exit lifecycle (no preemption)
- Memory region tracking with cleanup on exit

**Paging** (`arch/x86/kernel/paging.c`):
- 4-level page tables, identity map, 2MB large pages
- Everything runs ring 0 (no user/kernel separation)

### What's needed for musl-static binaries

musl-linked static binaries (busybox, toybox, static Go) use ~40-60 syscalls.
The critical missing ones, roughly ordered by frequency of use:

**Tier 1 -- Needed for "hello world" musl binary (5 syscalls):**

| Nr | Name | Complexity | Notes |
|----|------|-----------|-------|
| 9 | mmap | High | Anonymous + file-backed. Core allocator for musl. |
| 11 | munmap | Medium | Free mmap'd regions. |
| 10 | mprotect | Medium | Change page permissions (RWX). Needs per-page PTE updates. |
| 63 | uname | Trivial | Return static struct (sysname="OsitoK", machine="x86_64"). |
| 218 | set_tid_address | Trivial | Store TID pointer, return PID. musl calls at startup. |

**Tier 2 -- Needed for basic I/O utilities (10 syscalls):**

| Nr | Name | Complexity | Notes |
|----|------|-----------|-------|
| 4 | stat | Medium | Path-based stat (vs fstat which is fd-based). |
| 6 | lstat | Low | Same as stat (no symlinks). |
| 33 | dup2 | Low | Duplicate fd. Needed for shell redirection. |
| 72 | fcntl | Medium | F_GETFD, F_SETFD, F_GETFL, F_SETFL. |
| 79 | getcwd | Trivial | Return "/" (flat filesystem). |
| 80 | chdir | Trivial | No-op or ENOSYS (flat filesystem). |
| 39 | getpid | Trivial | Return current_proc->pid. |
| 102 | getuid | Trivial | Return 0 (root). |
| 104 | getgid | Trivial | Return 0. |
| 89 | readlink | Low | Return ENOENT (no symlinks). |

**Tier 3 -- Needed for real programs (10 syscalls):**

| Nr | Name | Complexity | Notes |
|----|------|-----------|-------|
| 56 | clone | High | Thread/process creation. Start with CLONE_VM for threads. |
| 57 | fork | High | Could be clone wrapper. Or return ENOSYS and require vfork. |
| 58 | vfork | Medium | Simpler than fork (shares address space). |
| 59 | execve | Medium | Already have proc_exec; need to wire to syscall interface. |
| 61 | wait4 | Medium | waitpid with rusage. |
| 13 | rt_sigaction | Medium | Signal handler registration. Critical path for musl. |
| 14 | rt_sigprocmask | Medium | Signal mask manipulation. |
| 15 | rt_sigreturn | Medium | Return from signal handler. |
| 35 | nanosleep | Low | APIC timer-based sleep. |
| 62 | kill | Low | Send signal to process. |

**Key implementation: mmap**

mmap is the single most important missing syscall. musl uses it for everything:
heap allocation, stack allocation, loading ELF segments, thread stacks.

```
Minimum viable mmap:
- MAP_ANONYMOUS | MAP_PRIVATE: allocate zeroed pages (most common)
- MAP_FIXED: place at exact address (used by ELF loader)
- MAP_ANONYMOUS | MAP_SHARED: same as PRIVATE for single-process
- File-backed mmap: read file into allocated pages (can defer)
```

Implementation path: extend `paging.c` with per-process VMA (virtual memory area)
tracking. `mmap` allocates physical pages, maps into page tables, records VMA.
`munmap` unmaps and frees. `mprotect` updates PTE permission bits + INVLPG.

### Goal

Run `busybox sh` (musl-static, ~1MB), `toybox` utilities, static Go binaries.
Milestone: interactive busybox shell inside OsitoK.

---

## Phase 2: Dynamic Linux Binaries (3-6 months)

### Target: Claude Code binary

Claude Code is a 227MB Bun binary (statically linked Bun runtime with JSC engine).
Bun uses `bun` as a single-binary JavaScript runtime. Despite being "static" in the
sense of no external .so dependencies, it exercises a much larger syscall surface
than simple C programs.

### Dynamic linker

For true dynamically-linked glibc binaries:

1. **ELF interpreter**: Parse PT_INTERP segment ("ld-linux-x86-64.so.2").
   Load the interpreter ELF, then let it load the main binary.
2. **Option A -- Ship musl ld.so**: Simpler, fewer syscalls needed. Requires
   recompiling or using musl-based distro binaries.
3. **Option B -- Run glibc ld.so**: Full compatibility but needs more syscalls
   (especially `mmap`, `mprotect`, `arch_prctl` for TLS).
4. **Option C -- Custom loader**: Parse DT_NEEDED, resolve symbols ourselves.
   Maximum control, significant work, but avoids ld.so complexity.

### Thread support

Bun/JSC is heavily multithreaded. Minimum viable thread support:

| Nr | Name | Notes |
|----|------|-------|
| 56 | clone | CLONE_VM + CLONE_FS + CLONE_FILES + CLONE_SIGHAND + CLONE_THREAD |
| 202 | futex | FUTEX_WAIT, FUTEX_WAKE. Core synchronization primitive. |
| 218 | set_tid_address | TID management for thread exit. |
| 186 | gettid | Return thread ID. |
| 234 | tgkill | Thread-directed signal. |
| 158 | arch_prctl | ARCH_SET_FS for TLS (Thread-Local Storage). Sets FS segment base via FSBASE MSR. |

Implementation: extend process table with thread concept. Each thread gets its own
stack + kernel stack + TLS area but shares address space (CR3) and FD table.
Scheduler: round-robin via APIC timer (already have periodic tick at ~100 Hz).

### Networking syscalls

Claude Code needs TCP/IP (connects to Anthropic API over HTTPS).
OsitoK already has TCP client (`net_tcp_connect/send/recv`) and DNS (`net_dns_resolve`).
Need to expose via socket syscalls:

| Nr | Name | Notes |
|----|------|-------|
| 41 | socket | AF_INET + SOCK_STREAM/SOCK_DGRAM |
| 42 | connect | Map to net_tcp_connect |
| 44 | sendto | Map to net_udp_send or net_tcp_send |
| 45 | recvfrom | Map to net_udp/tcp_recv |
| 49 | bind | For server sockets |
| 50 | listen | TCP server (not yet in net stack) |
| 43 | accept | TCP server |
| 48 | shutdown | Half-close |
| 54 | setsockopt | SO_REUSEADDR etc. |
| 55 | getsockopt | Queries |

For HTTPS: need TLS. Options:
- Bundle a TLS library (bearssl, mbedtls) in kernel or as static lib
- Stub TLS at network layer (terminate TLS in a proxy)

### Event loop syscalls

Bun uses epoll for its event loop:

| Nr | Name | Notes |
|----|------|-------|
| 213 | epoll_create1 | Create epoll instance |
| 233 | epoll_ctl | Add/modify/delete fd watches |
| 232 | epoll_wait | Block until events ready |
| 7 | poll | Simpler alternative, needed by some libs |
| 23 | select | Legacy, but some code paths use it |
| 232 | epoll_pwait | Signal-safe epoll_wait variant |

Implementation: epoll is a kernel-side fd→event table. On `epoll_wait`, check all
watched fds for readiness (socket has data, file is writable, etc). With our
polling-based network stack, this maps naturally.

### Signal support

Full POSIX signals needed for process lifecycle (SIGCHLD, SIGTERM, SIGPIPE, etc):

- Signal delivery: check pending signals on syscall return + timer tick
- Signal handlers: user-mode trampoline (push sigframe, change RIP to handler)
- Signal mask: per-thread mask, sigprocmask manipulation
- Critical signals: SIGCHLD (process exit), SIGPIPE (broken pipe), SIGSEGV (#PF → signal)

### Goal

Run the Claude Code binary (Bun). Interactive `claude` CLI connected to API via TCP/TLS.
Milestone: `claude --help` prints usage, `claude "hello"` sends a prompt and gets a response.

---

## Phase 3: Windows PE Execution (6-12+ months)

### Architecture: syscall translation, not emulation

Wine/Proton runs Windows binaries on Linux by translating NT syscalls to POSIX syscalls.
No CPU emulation -- the x86-64 instructions run natively. Wine replaces the Windows
kernel (ntdll.dll, kernel32.dll) with implementations that call Linux syscalls underneath.

On OsitoK, the same approach applies but one level lower: NT syscalls translate directly
to OsitoK kernel calls instead of going through Linux.

```
Windows binary                   Wine on Linux              OsitoK
────────────────                 ─────────────              ──────
Win32 API call                   Win32 API call             Win32 API call
    ↓                                ↓                          ↓
kernel32.dll                     kernel32.dll (Wine)        kernel32.dll (OsitoK impl)
    ↓                                ↓                          ↓
ntdll.dll (SYSCALL)              ntdll.dll (Wine)           ntdll.dll (OsitoK impl)
    ↓                                ↓                          ↓
NT kernel                        Linux syscall              OsitoK syscall_dispatch
```

### PE/COFF loader

Separate from ELF loader. PE binaries have different structure:

- **DOS header** (MZ) + PE signature + COFF header + Optional header
- **Sections**: .text, .rdata, .data, .reloc, .rsrc (vs PT_LOAD segments)
- **Import table**: DLL name + function name/ordinal → resolve at load time
- **Relocations**: .reloc section with base relocation entries (PE is position-dependent
  but can be relocated, unlike ET_EXEC ELF which is fixed)
- **TLS directory**: Thread-local storage callbacks, different from ELF TLS

Implementation: `pe_loader.c` alongside `elf.c`. Parse PE headers, load sections,
apply relocations, resolve imports against built-in DLL implementations.

### NT syscall translation layer

NT syscalls use different numbers, conventions, and data structures than Linux:

**Calling convention**: `syscall` instruction with RAX=syscall number, but args in
RCX, RDX, R8, R9 (Microsoft x64 ABI, not System V). Need a separate entry point
or detect PE vs ELF process and dispatch accordingly.

**Core NT syscalls to implement**:

| NT Nr | Name | Maps to |
|-------|------|---------|
| 0x0036 | NtCreateFile | sys_open |
| 0x0006 | NtClose | sys_close |
| 0x0005 | NtReadFile | sys_read |
| 0x0008 | NtWriteFile | sys_write |
| 0x004E | NtQueryInformationFile | sys_fstat |
| 0x0025 | NtCreateSection | sys_mmap (file-backed) |
| 0x0028 | NtMapViewOfSection | sys_mmap |
| 0x002A | NtUnmapViewOfSection | sys_munmap |
| 0x0050 | NtAllocateVirtualMemory | sys_mmap (anon) |
| 0x001E | NtFreeVirtualMemory | sys_munmap |
| 0x002C | NtProtectVirtualMemory | sys_mprotect |
| 0x0055 | NtCreateThread | sys_clone |
| 0x0195 | NtCreateThreadEx | sys_clone (Vista+) |
| 0x002F | NtTerminateProcess | sys_exit |
| 0x0053 | NtTerminateThread | thread exit |
| 0x0035 | NtWaitForSingleObject | futex/poll |
| 0x002B | NtWaitForMultipleObjects | epoll/poll |
| 0x0004 | NtCreateEvent | eventfd equivalent |
| 0x000E | NtSetEvent | eventfd write |
| 0x002D | NtResetEvent | eventfd reset |
| 0x0068 | NtQueryVirtualMemory | /proc/self/maps equivalent |

**Reference material**:
- Wine/Proton source (open source): `dlls/ntdll/unix/` has the NT→POSIX translation layer.
  This is the primary reference for understanding exactly which NT syscall semantics
  matter and how they map to our kernel primitives.
- Windows leaked source: NT kernel implementation of these syscalls. Useful for
  understanding edge cases and undocumented behavior that Wine may not fully cover.
  `ntos/mm/` (memory manager), `ntos/io/` (I/O manager), `ntos/ps/` (process/thread).

### Win32 DLL implementations

Minimum set of DLLs for console applications:

**ntdll.dll** -- NT syscall wrappers + RTL (runtime library):
- Heap management (RtlAllocateHeap, RtlFreeHeap)
- String functions (RtlInitUnicodeString, etc.)
- Exception handling (RtlUnwind, structured exception handling)
- Thread pool (TpAllocWork, etc.)
- Loader (LdrLoadDll, LdrGetProcAddress)

**kernel32.dll** -- Win32 base:
- File I/O (CreateFileW, ReadFile, WriteFile, CloseHandle)
- Console I/O (GetStdHandle, WriteConsoleW, ReadConsoleW)
- Memory (VirtualAlloc, VirtualFree, VirtualProtect, HeapCreate)
- Process (CreateProcessW, ExitProcess, GetExitCodeProcess)
- Thread (CreateThread, ExitThread, WaitForSingleObject)
- Sync (CreateMutexW, CreateEventW, InitializeCriticalSection)

**ws2_32.dll** -- Winsock:
- Socket API (socket, connect, send, recv, closesocket, select)
- Maps directly to our socket syscalls from Phase 2

**advapi32.dll** -- Security (stubs for most):
- Registry (RegOpenKeyExW, RegQueryValueExW) → return ERROR_FILE_NOT_FOUND
- Security tokens → stub with admin token

**ucrtbase.dll / msvcrt.dll** -- C runtime:
- Can use Wine's implementation directly or build from source

### PE vs ELF loader differences

| Aspect | ELF | PE |
|--------|-----|-----|
| Magic | `\x7fELF` | `MZ` (DOS) + `PE\0\0` |
| Segments | PT_LOAD (few, large) | Sections (many, named) |
| Relocation | ET_DYN has RELA entries | .reloc section, base delta |
| Dynamic linking | PT_INTERP → ld.so | Import table → DLL loader |
| Entry point | e_entry (absolute) | AddressOfEntryPoint + ImageBase |
| TLS | PT_TLS + DTV | TLS directory + callbacks |
| Calling convention | System V (RDI,RSI,RDX,RCX,R8,R9) | Microsoft (RCX,RDX,R8,R9) |
| Stack alignment | 16-byte at call | 16-byte at call |
| Red zone | 128 bytes below RSP | None (must use shadow space) |

### Implementation order

1. **PE parser + section loader** -- Load PE, map sections, apply base relocations
2. **Import resolver** -- Resolve DLL imports against built-in implementations
3. **ntdll.dll core** -- NT syscall stubs, heap, string, TLS
4. **kernel32.dll core** -- File I/O, console, memory, process/thread basics
5. **Console subsystem** -- Win32 console API (WriteConsoleW, etc.)
6. **ws2_32.dll** -- Winsock mapped to OsitoK TCP/UDP stack
7. **Registry stubs** -- Return sensible defaults, no persistence needed

### Goal

Run Windows console applications (cmd.exe-level tools, Go/Rust Windows binaries).
Milestone: `hello.exe` (MSVC-compiled) prints to console. Then: simple network tools.

---

## Syscall Matrix

### Linux syscalls by phase

```
Phase 1 (static musl):
  read(0) write(1) open(2) close(3) stat(4) fstat(5)
  lstat(6) lseek(8) mmap(9) mprotect(10) munmap(11) brk(12)
  rt_sigaction(13) rt_sigprocmask(14) rt_sigreturn(15)
  ioctl(16) writev(20) access(21) dup2(33)
  nanosleep(35) getpid(39) clone(56) fork(57) vfork(58)
  execve(59) exit(60) wait4(61) kill(62) uname(63)
  fcntl(72) getcwd(79) chdir(80) unlink(87)
  readlink(89) getuid(102) getgid(104) geteuid(107) getegid(108)
  arch_prctl(158) set_tid_address(218) exit_group(231)
  ~40 syscalls total

Phase 2 (dynamic glibc / Bun):
  All Phase 1 +
  socket(41) connect(42) accept(43) sendto(44) recvfrom(45)
  bind(49) listen(50) setsockopt(54) getsockopt(55)
  clone3(435) futex(202) gettid(186) tgkill(234)
  epoll_create1(213) epoll_ctl(233) epoll_wait(232)
  eventfd2(290) pipe2(293) pread64(17) pwrite64(18)
  openat(257) fstatat(262) readlinkat(267) faccessat(269)
  getrandom(318) clock_gettime(228) clock_nanosleep(230)
  madvise(28) mremap(25) getdents64(217) newfstatat(262)
  ~70 syscalls total

Phase 3 (Windows PE):
  NT syscall set -- see table above (~30 core NT syscalls)
  + Win32 DLL API surface (~200 functions across 5 DLLs)
```

### NT syscall map (Phase 3 reference)

```
NT syscall                     → OsitoK primitive
──────────────────────────────────────────────────
NtCreateFile                   → sys_open + path conversion (UTF-16 → UTF-8, \ → /)
NtReadFile                     → sys_read (+ async via APC if overlapped)
NtWriteFile                    → sys_write
NtClose                        → sys_close
NtQueryInformationFile         → sys_fstat + FILE_*_INFORMATION structs
NtAllocateVirtualMemory        → sys_mmap(MAP_ANONYMOUS)
NtFreeVirtualMemory            → sys_munmap
NtProtectVirtualMemory         → sys_mprotect (PAGE_* → PROT_*)
NtMapViewOfSection             → sys_mmap(file-backed)
NtCreateThread(Ex)             → sys_clone(CLONE_VM|CLONE_THREAD)
NtTerminateProcess             → sys_exit_group
NtWaitForSingleObject          → futex or custom wait queue
NtWaitForMultipleObjects       → epoll_wait equivalent
NtCreateEvent/Set/Reset        → eventfd semantics
NtQueryVirtualMemory           → walk VMA list
NtDeviceIoControlFile          → sys_ioctl equivalent
```

---

## Architecture Notes

### How Wine/Proton works

Wine implements Windows DLLs as Linux shared libraries. When a Windows binary calls
`CreateFileW()` in `kernel32.dll`, Wine's implementation converts the path from
UTF-16 to UTF-8, translates `\\?\` paths to `/`, converts access flags
(`GENERIC_READ` → `O_RDONLY`), and calls the Linux `open()` syscall.

The key insight: **no instruction emulation**. x86-64 Windows binaries contain the
same machine code as Linux binaries. Only the kernel interface differs. Wine provides
the expected kernel interface by intercepting syscalls and translating them.

Proton (Valve's fork) adds DirectX→Vulkan translation (DXVK) and various gaming
patches. For OsitoK's purposes, only the core Wine ntdll/kernel32 translation matters.

### How this maps to OsitoK

OsitoK's advantage: we control the entire kernel, so NT syscall translation happens
at the dispatch level rather than through `ptrace` or signal interception.

```
Option A: Dual syscall entry
  - Detect process type (ELF vs PE) in process_t
  - SYSCALL entry checks current_proc->type
  - ELF: dispatch via Linux syscall table
  - PE:  dispatch via NT syscall table (different arg registers)

Option B: Unified dispatch with thunks
  - PE loader patches ntdll.dll import to use Linux SYSCALL convention
  - All syscalls go through same entry, NT numbers mapped to OsitoK handlers
  - Simpler entry but requires modifying PE at load time
```

Option A is cleaner and matches Wine's architecture. The SYSCALL entry point
checks a per-thread flag and dispatches to the correct table.

### Memory model differences

Windows uses `VirtualAlloc` with explicit commit/reserve semantics:
- `MEM_RESERVE`: reserve address range (no physical pages)
- `MEM_COMMIT`: map physical pages into reserved range
- `MEM_RELEASE`: free entire reservation

This maps to mmap but requires tracking reserved-but-uncommitted ranges.
Implementation: VMA entries with a `committed` flag. Reserve = create VMA with no
PTE entries. Commit = allocate pages + create PTEs. Access to uncommitted pages
triggers #PF → STATUS_ACCESS_VIOLATION (or auto-commit, depending on strategy).

### Path translation

Windows paths (`C:\Users\foo\bar.txt`) need to translate to OsitoK's flat filesystem.
Simplest approach: map `C:\` to OsitoFS root, strip drive letter and convert `\` to `/`.
Special paths (`\\.\`, `\\?\`, named pipes) return STATUS_NOT_FOUND initially.

### Unicode handling

Windows APIs use UTF-16 (WCHAR). OsitoK and OsitoFS use UTF-8.
Need: `utf16_to_utf8()` and `utf8_to_utf16()` conversion functions in the
NT compatibility layer. ~100 lines of code, well-defined algorithm.

---

## Dependencies and risks

| Risk | Impact | Mitigation |
|------|--------|-----------|
| mmap complexity | Blocks Phase 1 | Start with MAP_ANONYMOUS only, no file-backed |
| Thread support | Blocks Phase 2 | Can run single-threaded Bun with CLONE stubs initially |
| TLS (glibc) | Blocks dynamic linking | arch_prctl ARCH_SET_FS + FSBASE MSR (straightforward) |
| epoll | Blocks Bun event loop | Start with poll() fallback |
| TLS (crypto) | Blocks HTTPS | Use external TLS proxy initially |
| NT syscall numbers | Version-dependent | Pin to Windows 10 22H2 syscall table |
| Structured exceptions | Needed for some PE | Implement via #UD/#GP → SEH dispatch |

## Estimated syscall count

```
Current OsitoK:     13 syscalls
Phase 1 target:    ~40 syscalls  (+27)
Phase 2 target:    ~70 syscalls  (+30)
Phase 3 adds:      ~30 NT syscalls + ~200 Win32 API functions
```
