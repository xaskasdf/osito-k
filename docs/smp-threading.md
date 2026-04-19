# SMP Threading & Speculative Multithreading

OsitoK's SMP subsystem enables true multi-core parallel compute on x86-64.
All 4 CPUs (BSP + 3 APs) are active and available for work dispatch.

## Architecture

```
BSP (CPU 0)                    AP 1-3
┌─────────────┐               ┌─────────────┐
│  Scheduler  │               │ Worker Loop  │
│  Processes  │  IPI 0xFE     │  sti; hlt    │
│  ISR full   │──────────────>│  ISR fast    │
│  FPU[0]     │               │  FPU[1..3]   │
└─────────────┘               └─────────────┘
       │                            │
       └── smp_submit() ───────────>│ execute func(arg)
       <── smp_wait() ─────────────>│ done = 1
```

### Per-CPU Isolation

- **FPU state**: `fpu_state_ptrs[lapic_id]` — each CPU has its own 512-byte
  fxsave buffer. BSP's points to the current process's `fpu_state[]`, APs
  have static buffers in `fpu_state_ap_bufs[]`.

- **ISR fast-path**: APs entering `isr_common` check LAPIC ID first. Non-BSP
  CPUs do EOI + iretq immediately without entering `isr_handler`, touching
  the scheduler, or accessing `sched_switch_rsp`.

- **Context switch**: Only BSP checks `sched_switch_rsp`. APs skip it entirely
  to prevent stealing a context switch meant for the BSP.

## API

### Low-level: smp_submit / smp_wait

```c
#include "smp_work.h"

// Submit to specific AP
int smp_submit(int ap_idx, smp_work_func_t func, void *arg, void *result);

// Submit to any idle AP
int smp_submit_any(smp_work_func_t func, void *arg, void *result);

// Wait for completion
void smp_wait(int ap_idx);
void smp_barrier(void);  // wait for all
```

### High-level: tls_parallel_for / tls_parallel_reduce

```c
// Distribute independent loop iterations across all CPUs
int tls_parallel_for(int64_t start, int64_t end, int64_t step,
                     void (*body)(int64_t iter, void *ctx), void *ctx);

// Same but with per-chunk accumulation + final sum
int64_t tls_parallel_reduce(int64_t start, int64_t end, int64_t step,
                            void (*body)(int64_t iter, void *ctx, int64_t *accum),
                            void *ctx);
```

Both auto-fallback to sequential if <64 iterations or no APs available.

## Performance

Q4_0 matvec 2048x2048 (tensor inference hot path):
- Single-core: 256M cycles (~85ms)
- 4-core SMP:   65M cycles (~21ms)
- **Speedup: 4x** (linear scaling)

## Speculative Prefetch

`spec_prefetch_ahead(rip, cr3)` is called from `sched_tick` when the BSP
decides not to context-switch. An idle AP prefetches 2KB of code ahead:

- **Kernel code**: direct `prefetcht0` (already in direct map)
- **Userspace code**: page-table walk via `paging_get_pte_in_cr3()` without
  CR3 switch, then prefetch via `PHYS_TO_VIRT(phys)`. Demand-paged pages
  are silently skipped.

## Binary Analysis

`spec_analyze_text()` runs at ELF load time, building a CFG:
- Minimal x86-64 decoder (branch/call/ret/syscall detection)
- Up to 4096 basic blocks, 256 syscall sites, 128 loops
- Loops detected via back-edge analysis
- Results stored in `process_t.spec_info`

## Files

| File | Purpose |
|------|---------|
| `kernel/smp_work.h` | AP control block, API declarations |
| `kernel/smp_work.c` | Work queue, IPI dispatch, worker loop |
| `kernel/spec_prefetch.c` | Speculative code prefetch |
| `kernel/spec_analyze.c` | Binary CFG builder + loop detector |
| `kernel/spec_tls.c` | parallel_for / parallel_reduce |
| `kernel/isr_stubs.S` | Per-CPU FPU + AP fast-path + IPI stub |
| `kernel/process.c` | fpu_state_ptrs[], AP FPU buffers |
| `kernel/smp.c` | AP boot, worker loop entry, accessors |
