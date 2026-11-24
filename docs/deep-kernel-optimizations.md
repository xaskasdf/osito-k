# OsitoK Deep Kernel Optimizations — Apr 19 2026

Commit `4475141` on `experiment` branch. 10 hardware-level features across
25 files (+1104 -105 lines). These exploit OsitoK's full hardware control
to eliminate indirection layers that exist in conventional kernels for
security/compatibility reasons that don't apply here.

---

## 1. Hot/Cold/Init Code Splitting

**Files:** `kernel.ld`, `include/types.h`, 13 kernel/driver `.c` files

### Macros

```c
#define __hot   __attribute__((section(".text.hot")))
#define __cold  __attribute__((section(".text.cold")))
#define __initk __attribute__((section(".text.init")))
```

### Linker Layout

```
.text.hot     ->  image start, L1i-resident (~17KB)
.text         ->  normal code
.text.cold    ->  rarely called (shell_run, etc.)
.text.init    ->  page-aligned, reclaimable after boot
```

### Annotated Functions

| Section | Functions |
|---------|-----------|
| `__hot` | `sched_tick`, `net_poll`, `compositor_render_frame`, `matvec_q4_0_avx2`, `syscall_dispatch`, `smp_submit_any`, `smp_wait` |
| `__cold` | `shell_run` |
| `__initk` | `kernel_entry`, `mem_init`, `idt_init`, `paging_init`, `pci_scan`, `nvme_init`, `i211_init` |

### Init Memory Reclaim

`reclaim_init_memory()` in `memory.c` frees `__init_start..__init_end`
pages back to the page allocator after boot completes (~20KB = 5 pages).
Called from `main.c` just before `shell_run()`.

---

## 2. VDSO Page

**Files:** `kernel/syscall.c`, `kernel/process.c`, `kernel/idt.c`, `kernel/main.c`

One physical 4KB page, mapped read-only at `0x7FFFE000` in every process
address space. Kernel updates it on every timer tick (~20 instructions).

### Structure

```c
typedef struct __attribute__((aligned(4096))) {
    volatile uint32_t seq;       /* seqlock: odd = updating */
    uint64_t monotonic_ns;       /* nanoseconds since boot */
    uint64_t tsc_at_update;      /* rdtsc value at last update */
    uint64_t tsc_per_sec;        /* TSC frequency for interpolation */
    int64_t  unix_timestamp;     /* UTC seconds (from NTP) */
    uint64_t boot_ticks;         /* raw 100Hz tick count */
    uint32_t cpu_count;          /* online CPUs */
    uint64_t total_memory;       /* bytes */
    uint64_t free_memory;        /* bytes (updated each tick) */
    uint64_t random_seed;        /* XOR'd with TSC each tick */
} vdso_data_t;
```

### Userspace Read Protocol

```c
vdso_data_t *v = (vdso_data_t *)0x7FFFE000;
uint64_t ns;
uint32_t seq;
do {
    seq = v->seq;
    lfence();
    if (seq & 1) continue;  /* update in progress */
    ns = v->monotonic_ns;
    /* Optional TSC interpolation for sub-tick precision:
     * ns += (rdtsc() - v->tsc_at_update) * 1e9 / v->tsc_per_sec */
    lfence();
} while (v->seq != seq);
```

### Integration

- `vdso_init()` called from `main.c` after `syscall_init()`, before `proc_init()`
- `vdso_update()` called from timer ISR in `idt.c` after `tick_count` update
- `vdso_map_process(cr3)` called from `proc_exec()` after `paging_create_process_cr3()`
- PID/UID not included -- VDSO is shared across all processes; `getpid()` is
  already a single memory load, not worth a per-process VDSO variant

---

## 3. TSC-Deadline Scheduling

**Files:** `kernel/idt.c`, `kernel/process.c`

Replaces the fixed 100Hz periodic LAPIC timer with TSC-deadline mode for
sub-microsecond scheduling precision.

### Detection

```
CPUID.1:ECX[24]          -> TSC-deadline support
CPUID.80000007H:EDX[8]   -> invariant TSC
```

Both required. Falls back to periodic mode if either absent.

### Calibration

TSC frequency calibrated via PIT channel 2 at boot (same PIT sequence as
the existing APIC calibration, but now also measures TSC elapsed).

### Per-QoS Quanta (microseconds)

| QoS Class | Periodic (ticks) | TSC-Deadline |
|-----------|-----------------|--------------|
| IDLE | 20 (200ms) | 100,000 (100ms) |
| BACKGROUND | 10 (100ms) | 20,000 (20ms) |
| DEFAULT | 5 (50ms) | 5,000 (5ms) |
| INTERACTIVE | 2 (20ms) | 2,000 (2ms) |
| REALTIME | 1 (10ms) | **100 (100us)** |

### Tickless Idle

When `sched_get_next_deadline_us()` returns 0 (no READY processes), no
deadline is programmed. CPU enters HLT/C-state and wakes only on NIC,
keyboard, or IPI interrupt. Zero timer interrupts when idle.

### tick_count Stability

Subsystems (TCP retransmit, POSIX timers, display VBlank) depend on
`idt_get_ticks()` advancing at ~100Hz. In TSC-deadline mode, tick_count
is advanced based on elapsed TSC rather than interrupt count:

```c
uint64_t tsc_per_tick = tsc_freq / 100;
while (now_tsc - tsc_last_tick >= tsc_per_tick) {
    tick_count++;
    tsc_last_tick += tsc_per_tick;
}
```

---

## 4. Interrupt-Driven NIC (NAPI Hybrid)

**Files:** `drivers/i211.c`, `drivers/i211.h`, `kernel/pci.c`,
`kernel/idt.c`, `kernel/isr_stubs.S`, `kernel/net.c`, `kernel/main.c`

Transforms I211 from pure polling to interrupt-driven with Linux NAPI-style
adaptive switching.

### PCI MSI Support

`pci_enable_msi(bus, dev, func, vector)` -- generic function that:
1. Walks PCI capability list to find MSI capability (Cap ID 0x05)
2. Programs MSI message address (`0xFEE00000` = BSP LAPIC)
3. Programs MSI message data (IDT vector number)
4. Sets MSI Enable bit, disables INTx

No IOAPIC needed -- MSI writes directly to LAPIC via PCIe.

### NAPI Flow

```
1. Packet arrives -> NIC fires MSI -> IDT vector 40
2. i211_isr(): read ICR (auto-clear), set irq_pending, disable RX via IMC
3. net_poll() (called from sched_tick): if irq_pending, drain ring
4. Ring drained -> i211_rx_irq_reenable() -> re-enable RX via IMS
```

### Interrupt Throttle

`EITR0 = 390 << 2` = ~100us between interrupts = ~10K interrupts/sec max.
Prevents interrupt storm on packet flood while keeping latency low.

### Latency

| Mode | Packet latency |
|------|---------------|
| Polling (before) | Up to 10ms (next `sched_tick`) |
| MSI + NAPI (after) | ~1-5us (interrupt arrival) |

---

## 5. WC Compositor Flip

**File:** `kernel/display.c` (1-line change)

```c
// Before:
memcpy(disp.gop_fb, disp.back, disp.fb_size);
// After:
memcpy_nt(disp.gop_fb, disp.back, disp.fb_size);
```

`movntdq` stores bypass L1/L2/L3 cache for the ~3MB frame copy. Frees
cache capacity for inference, processes, and network processing. `sfence`
is already inside `memcpy_nt`. `display_force_refresh()` uses regular
`memcpy` as fallback for QEMU/HVF dirty-page tracking.

Full WC back buffer was considered and rejected -- compositor reads back
from the buffer during scale-blit, which would hit DRAM at ~100ns/cacheline
through WC. The NT-flip approach gets 90% of the benefit with zero risk.

---

## 6. F16C + Unrolled AVX2 Matvec

**Files:** `kernel/tensor_avx2.c`, `Makefile`

### Key Changes

1. **F16C scale conversion**: `vcvtph2ps` (2 instructions) replaces
   `f16_to_f32()` software function (~20 instructions with branch for
   denormals/inf/nan). `-mf16c` added to `AVXFLAGS`. F16C is present
   on all Haswell+ CPUs (= all AVX2 CPUs).

2. **4-block unroll with 2 accumulators**: Processes 128 Q4_0 values
   per iteration. Two accumulator registers (`acc0`, `acc1`) hide FMA
   latency (5 cycles on Haswell, 4 on Skylake+). For dim=2048: 64
   blocks/row = 16 iterations of the unrolled loop.

3. **`process_q4_block` helper**: `always_inline` function shared
   between unrolled and remainder loops. Eliminates code duplication.

### Generated Code

- 5x `vcvtph2ps` (4 unrolled + 1 remainder)
- 21x `vfmadd` instructions total
- No register spills (GCC allocates well with the inline helper pattern)

---

## 7. NVMe Async DMA + Tensor Streaming

**Files:** `drivers/nvme.c`, `fs/gguf.c`, `fs/gguf.h`, `fs/ositofs2.c`,
`kernel/inference.c`, `kernel/inference.h`

### Async NVMe Primitives

```c
uint16_t nvme_io_submit_async(nvme_sqe_t *cmd);  /* returns cmd_id */
int      nvme_poll_cq(uint16_t cid);             /* 0=done, -1=pending, -2=error */
int      nvme_wait_cq(uint16_t cid);             /* blocking spin */
int      nvme_read_async(uint64_t lba, uint32_t count, uint64_t phys_addr);
```

Max 2 commands in-flight (ping-pong). `nvme_inflight` counter prevents
queue overflow.

### Tensor DMA Map

```c
typedef struct {
    uint64_t lba;        /* starting LBA on NVMe */
    uint32_t lba_count;  /* number of LBAs */
    uint64_t size;        /* exact byte size */
} tensor_dma_entry_t;
```

`tensor_dma_build_map()` computes LBA addresses from GGUF tensor offsets +
`osfs2_file_byte_offset()` (new accessor that returns absolute byte offset
of a file on the NVMe device).

### Ping-Pong Layer Streaming

```
llama_forward_streaming(state, token):
    Pre-load layer 0 -> buf[0]  (sync)
    for l = 0..n_layers:
        DMA layer l+1 -> buf[l+1 & 1]  (async, ~1ms at 3.5 GB/s)
        compute layer l from buf[l & 1]  (~62ms)
        wait DMA  (already done -- hidden behind compute)
```

For Llama 3.2 1B in Q4_0: ~33MB per layer, 2 buffers = ~66MB total weight
memory instead of ~500MB. Global tensors (token_embd ~147MB, output_norm,
output) stay in RAM.

---

## 8. Speculative Token Execution

**Files:** `kernel/inference.c`, `kernel/inference.h`

### Design

While BSP does softmax + top-p sampling for token T (~200us), an AP starts
the forward pass for the predicted token T+1 (argmax of logits, ~50us to
compute).

Shadow KV buffers prevent main KV cache corruption on misprediction:
- Hit: `memcpy(shadow -> main KV)` for the speculated layers, then
  `forward_from_layer(start=4)` to skip the computed layers
- Miss: discard shadow buffers (zero cleanup)

### State

```c
typedef struct {
    float *shadow_k[4];     /* per-speculated-layer KV */
    float *shadow_v[4];
    float *spec_x, *spec_xb, *spec_xb2;  /* separate scratch */
    float *spec_q, *spec_k, *spec_v;
    float *spec_att, *spec_hb, *spec_hb2;
    uint32_t predicted_token;
    volatile int spec_complete;  /* 0=running, 1=done, -1=cancelled */
    uint64_t hits, misses;
} spec_state_t;
```

Total speculative scratch: ~100KB + 16KB shadow KV = ~116KB.

### Expected Performance

| Temperature | Hit rate | Tokens saved/hit | Avg improvement |
|-------------|----------|-------------------|-----------------|
| 0.0 (greedy) | 100% | 4 layers | ~15% |
| 0.6, top_p=0.9 | ~60-80% | 4 layers | ~10% |

**Status:** `llama_spec_init()` allocates state. Decode loop integration
(the overlap between sampling and speculative forward) is infrastructure-
ready but not yet wired into `llama_generate()`.

---

## 9. KV Cache Checkpoint/Restore

**Files:** `kernel/inference.c`, `kernel/inference.h`

Instant resume from a previously processed prompt by saving/restoring the
KV cache to/from an OsitoFS file.

### Checkpoint

```c
int llama_checkpoint_kv(llama_state_t *state, const char *filename);
```

Writes:
1. Header: magic (`0x4F534B4C`), version, pos, n_layers, kv_dim, max_seq
2. K entries `[0..pos-1]` for each layer
3. V entries `[0..pos-1]` for each layer

Only the filled portion is saved (not the full `max_seq` allocation).

### Restore

```c
int llama_restore_kv(llama_state_t *state, const char *filename);
```

Validates header (magic, version, model match), loads KV data directly
into the existing cache buffers, sets `state->pos`. Generation can
continue immediately -- zero recompute.

### Size Example

Llama 3.2 1B, 512-token context:
```
16 layers x 2 (K+V) x 512 positions x 512 kv_dim x 4 bytes = 32 MB
At NVMe write speed (2 GB/s): ~16ms to save
At NVMe read speed (3.5 GB/s): ~9ms to restore
```

---

## Verification

### Build

```bash
cd ~/osito-k/arch/x86 && make build/kernel.elf
```

### QEMU

```bash
# Standard boot:
bash arch/x86/scripts/qemu-test.sh --no-build

# TSC-deadline (needs CPU support):
# Add to QEMU flags: -cpu host  (KVM) or -cpu Skylake-Client (TCG)

# NIC interrupts (needs igb device):
# Add to QEMU flags: -device igb,netdev=n0 -netdev user,id=n0
```

### Serial Log Indicators

```
[INIT] Reclaimed 20 KB of init memory     <- hot/cold split working
[VDSO] Initialized, TSC XXXX MHz          <- VDSO page allocated
[IDT] TSC-deadline mode, freq=XXXX MHz    <- TSC-deadline active
[PCI] MSI enabled: vector 40 for B:D.F    <- NIC MSI configured
[I211] Interrupts enabled (MSI vector 40) <- NAPI mode active
[INF] Streaming init: XXXX KB/layer       <- NVMe streaming ready
[INF] Speculative execution initialized   <- spec buffers allocated
[CKPT] Saved KV cache: pos=XXX, YYY KB    <- checkpoint written
[CKPT] Restored KV cache: pos=XXX         <- checkpoint loaded
```
