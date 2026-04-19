# OsitoK Kernel Architecture — Apr 18-19 2026

Three commits implementing 14 kernel features across scheduler, memory,
I/O, inference, and debugging subsystems.

## 1. O(1) Preemptive Scheduler

**File:** `kernel/process.c`

### Before
`sched_tick()` (100Hz ISR) scanned all 64 `proctab` slots linearly to find
the highest-QoS READY process. `proc_find(pid)` also O(n).

### After

```
ready_bitmap: uint64_t    (1 bit per slot, fits in register)
runq_head[5]: int16_t     (per-QoS class: IDLE..REALTIME)
runq_tail[5]: int16_t     (tail for O(1) enqueue)
pid_to_idx[4096]: int16_t (PID → proctab index)
```

- `sched_tick`: iterates 5 QoS levels high→low, picks head of first
  non-empty queue — O(1) regardless of process count
- `proc_find`: direct `pid_to_idx[]` lookup — O(1)
- `proc_transition(p, new_state)` centralizes all 26 state assignment
  sites, maintaining bitmap + run queue invariants automatically

## 2. REP MOVSB memcpy/memset

**File:** `libc/crt.c`

```
< 64 bytes:  uint64_t word copies (8 bytes/iter), byte tail
≥ 64 bytes:  REP MOVSB (ERMS — internal 256-bit stores on modern CPUs)
```

No SSE state, no XMM registers, works in ISR context. Affects kernel and
all user programs (crt.c linked everywhere).

## 3. Compositor Frame-Level Dirty Skip

**File:** `kernel/compositor.c`

Global `comp_frame_dirty` flag + cursor position tracking. When nothing
changed (no window signal, no mouse move, no window create/destroy),
`compositor_render_frame()` returns immediately — skipping desktop render,
window blit, and cursor draw.

Forces dirty once per second for RTC clock update in the status panel.
Result: 0% CPU when desktop is idle (was 100% at 60fps).

## 4. Shared fd_table_t for Threads

**Files:** `include/fd.h`, `kernel/process.c`

```c
typedef struct {
    int         refcount;
    fd_entry_t  entries[MAX_FDS];  /* 128 entries */
} fd_table_t;
```

- `process_t` has `fd_table_t *fd_table` (pointer, not inline array)
- `proc_fork()`: allocates new copy (correct for fork)
- `proc_clone_thread()`: shares pointer with `refcount++` (POSIX CLONE_FILES)
- `proc_free()`: decrements refcount, frees at 0
- process_t shrinks ~8KB (fd table no longer inline)

## 5. Futex Hash Buckets

**File:** `kernel/process.c`

32 buckets with singly-linked lists + free list. Hash function:
`((addr >> 2) * 0x9e370001) >> 27`. Average scan: 8 entries vs 256.
Free list avoids linear scan for slot allocation.

## 6. Pipe Bulk Copy

**File:** `kernel/syscall.c`

Replaced byte-by-byte pipe read/write loops with contiguous chunk memcpy.
Handles ring buffer wrap-around at the boundary. A 4096-byte pipe write
goes from 4096 loop iterations to 1-2 `memcpy()` calls (which use REP MOVSB).

## 7. Always-On Kernel Profiling

**Files:** `kernel/kprof.c`, `kernel/idt.c`

Histogram records RIP at every 100Hz timer tick unconditionally. One
`uint32_t` array write per tick — unmeasurable overhead. Ring buffer for
detailed (RIP, tick) tuples stays opt-in behind `kprof_start()`.

Hooked in `idt.c` timer ISR, before `sched_tick()`:
```c
kprof_record(frame->rip);
```

Shell command `kprof` dumps top-10 hotspots anytime, no start/stop needed.

## 8. Batched Syscall (SYS_BATCH = 520)

**File:** `kernel/syscall.c`

Execute an array of `batch_entry_t` in one SYSCALL trap:

```c
typedef struct {
    uint64_t nr;         /* syscall number */
    uint64_t args[6];
    int64_t  result;     /* filled by kernel */
    uint32_t flags;      /* BATCH_STOP_ON_ERROR */
} batch_entry_t;
```

`BATCH_USE_PREV_RESULT` (0xFFFF...F) magic value in any arg slot substitutes
the previous entry's result — enables chaining:
```
open("file", O_RDONLY) → read(fd=PREV, buf, 4096) → close(fd=PREV)
```
One trap instead of three. Max 32 entries per batch.

## 9. Tensor Scratch Cache Coloring

**File:** `kernel/inference.c`

32KB `L2_COLOR_PAD` inserted between buffers used concurrently on different
cores. Avoids L2 cache set aliasing when BSP computes Q while AP0 does K
and AP1 does V.

```
[x][xb][xb2][q]──32KB──[k]──32KB──[v][att][hb]──32KB──[hb2][logits]
```

Scratch grows 215KB → 311KB. Only measurable on real hardware with
set-associative L2 (QEMU doesn't model cache sets).

## 10. AP Input Vector Prefetch

**File:** `kernel/inference.c`

`matvec_worker` and `attention_heads_worker` issue `__builtin_prefetch`
for the input vector at function entry. The vector is hot in BSP's L2
from `rmsnorm` but cold on APs.

## 11. Syscall-Free Command Ring (SYS_CMDRING_INIT = 521)

**File:** `kernel/syscall.c`, `kernel/process.c`

Shared-memory command ring for trusted processes. Process writes
`cmd_entry_t` to ring, kernel drains up to 8 entries per scheduler tick
(in `sched_tick` context where `current_proc` and `fd_table` are correct).

```c
typedef struct {
    uint32_t opcode;     /* CMD_WRITE, CMD_READ, CMD_CLOSE, CMD_LSEEK */
    int32_t  fd;
    uint64_t addr, len;
    int64_t  result;     /* written by kernel */
    volatile uint32_t state;  /* FREE / PENDING / DONE */
} cmd_entry_t;
```

64-entry ring. Process spins on `state == CMD_DONE` with `pause`.
Zero SYSCALL/SYSRET overhead — just memory writes + mfence.

## 12. Memory Quarantine (UAF Detection)

**File:** `kernel/syscall.c`

On `munmap` with quarantine enabled:
1. Pages unmapped from page tables (immediate #PF on access)
2. Physical page held in 256-entry FIFO ring for 500 ticks (5 seconds)
3. On page fault with no VMA: checks quarantine ring for address match
4. If match: reports UAF with faulting address, freed RIP, and symbolized
   function name via `user_symbolize()` from `usym.c`

Zero overhead when disabled (default). Enable via `quarantine_set(true)`.
Expired entries recycled after 5 seconds or when ring wraps.

## Verification

All features boot-tested in QEMU:
- Kernel boots to shell prompt, no panics
- `exec zsh.elf` — prompt appears, no hang
- `chat hi` — inference runs with 311KB colored scratch
- Serial log clean (no #PF, #UD, DOUBLE FREE)
