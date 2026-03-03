# OsitoK x86-64 — GPU Compute Roadmap

> Status: X1–X30 + X-CPU1 done. X31 next.
> Last updated: 2026-03-03

## Current State

X28 implemented the correct GBL-based FWSEC-FRTS boot sequence:
- PIO-load GBL microcode to Falcon IMEM
- BootloaderDmemDescV2 in DMEM with physical addresses of FWSEC in system RAM
- FBIF TRANSCFG for coherent system memory DMA
- SEC2 target for Turing, GSP for Ampere+
- Legacy VRAM+DMATRFBASE fallback preserved

**Next**: X31 validates GSP-RM functional RPC with the proper boot chain in place.

---

## Phase A: GSP Secure Boot Chain Fix (X27–X31) — HIGH RISK

The make-or-break phase. Without WPR2, GSP-RM cannot function.

| Feature | Description | Est. Lines | Risk |
|---------|-------------|-----------|------|
| **X27** | **Falcon PIO Load** — write code to IMEM/DMEM via IMEMC/IMEMD registers | ~150 | Done |
| **X28** | **GBL extraction + FWSEC-FRTS via GBL** — extract GBL from VBIOS, PIO-load to IMEM, GBL DMA-loads FWSEC from sysmem, execute FRTS | ~350 | Done |
| **X29** | **WPR2 metadata + Radix3 page tables** — build radix3 PT for GSP firmware, write WPR meta to VRAM | ~300 | Done |
| **X30** | **Two-stage GSP boot** — Falcon DMA load, bootloader-first boot, WPR meta mailboxes, legacy fallback | ~250 | Done |
| **X31** | **GSP-RM functional RPC** — with proper boot chain, existing X22-X23 RPC code should work | ~200 | Low |

### X27: Falcon PIO Load (prerequisite)

PIO (Programmed I/O) load writes code/data directly to Falcon IMEM/DMEM through
MMIO registers. This is the standard method to load small bootstrap code (GBL).

**Registers** (offset from Falcon base):
```
IMEMC (0x180): IMEM control — bits 15:0 = byte address, bit 24 = auto-inc on write, bit 25 = auto-inc on read
IMEMD (0x184): IMEM data — write dwords sequentially, address auto-increments
DMEMC (0x1C0): DMEM control — same format as IMEMC
DMEMD (0x1C4): DMEM data — same as IMEMD
```

**Functions**:
- `falcon_pio_load_imem(base, dst, data, size)` — stream dwords to IMEM
- `falcon_pio_load_dmem(base, dst, data, size)` — stream dwords to DMEM
- `falcon_pio_read_imem(base, src, buf, size)` — read back for verify
- `falcon_reset(base)` — halt, clear mailboxes, verify halted
- `falcon_boot(base, boot_addr)` — BOOTVEC + STARTCPU

**Reference**: nouveau `nvkm_falcon_pio_wr()`, nova-core `Falcon::pio_wr()`.

### X28: GBL + FWSEC-FRTS (critical path)

The Generic Bootloader (GBL) is a small (~4KB) Falcon program embedded in VBIOS.
It's designed to DMA-load larger payloads (like FWSEC) from system memory into
Falcon IMEM/DMEM, then execute them.

**Sequence**:
1. Extract GBL from VBIOS (BIT token or known offset)
2. Allocate DMA-accessible system memory for FWSEC image
3. PIO-load GBL to GSP Falcon IMEM (using X27)
4. Set DMEM arguments: FWSEC physical address, size, FRTS command
5. Set MAILBOX0 = FRTS command (0x15)
6. BOOTVEC=0, STARTCPU
7. GBL executes: DMA-loads FWSEC → runs FWSEC → FWSEC creates WPR2

**Why high risk**: Requires correct GBL extraction (VBIOS layout varies), correct
DMA buffer setup (physical addresses that Falcon DMA engine can reach), and the
FWSEC image must be the right version for the GPU.

### X29: WPR2 + Radix3

Even with FWSEC creating the WPR2 region, GSP firmware needs radix3 page tables
to map its address space. The host must build these and write metadata.

### X30–X31: GSP Boot + RPC

Once WPR2 is established, the existing GSP boot (X20) and RPC (X22-X23) code
should work with minimal changes. X31 validates the full pipeline end-to-end.

---

## Phase B: GPU Compute Pipeline (X32–X36) — MEDIUM RISK

Requires Phase A complete (functional GSP-RM). Sets up GPU compute dispatch.

| Feature | Description | Est. Lines | Risk |
|---------|-------------|-----------|------|
| **X32** | **GMMU 4-level page tables** — GPU virtual memory (PDE3→PDE2→PDE1→PTE) | ~400 | Medium |
| **X33** | **Channel + GPFIFO** — allocate channel via RM, setup GPFIFO ring buffer | ~350 | Medium |
| **X34** | **Compute class bind + kernel dispatch** — bind compute class, push methods | ~500 | Medium |
| **X35** | **Copy Engine DMA** — host-to-device and device-to-host transfers | ~250 | Low |
| **X36** | **Kernel completion + semaphore sync** — wait for kernel, read results | ~100 | Low |

### Key structures

```
GMMU page tables:  PDE3 (512 entries) → PDE2 → PDE1 → PTE (4KB pages)
Channel:           GPFIFO ring (64 entries × 8B), runlist, RAMFC
Compute dispatch:  SET_OBJECT → LAUNCH_DMA → GRID_PARAM → LAUNCH
Copy Engine:       CE methods via GPFIFO, src/dst address pairs
```

---

## Phase C: NTransformer Port (X37–X40) — MEDIUM RISK

Port inference pipeline to GPU compute. Requires Phase B.

| Feature | Description | Est. Lines | Risk |
|---------|-------------|-----------|------|
| **X37** | **Offline SASS compilation** — tools/cuda/ SASS assembler for compute kernels | ~300 | Medium |
| **X38** | **Kernel loader** — load compiled SASS from OsitoFS, setup launch params | ~400 | Medium |
| **X39** | **GPU tensor ops** — SASS kernels: matvec_q4_0, rmsnorm, softmax, RoPE | ~600 | Medium |
| **X40** | **GPU-accelerated Llama inference** — orchestrate GPU kernels for full forward pass | ~400 | Medium |

### SASS Kernel Strategy

NVIDIA GPUs execute SASS (Shader ASSembly) natively. Without CUDA/PTX toolchain,
we compile SASS offline using `nvdisasm`/`cuobjdump` reverse-engineered formats
or use a minimal SASS assembler.

Key kernels needed:
- `matvec_q4_0_kernel` — Q4_0 dequant + dot product (shared memory tiling)
- `rmsnorm_kernel` — parallel reduction
- `softmax_kernel` — parallel max + exp + sum + normalize
- `rope_kernel` — element-wise sin/cos rotation
- `silu_kernel` — element-wise activation

---

## Phase D: CPU Parallel Tracks (independent) — LOW RISK

These can be done anytime, independently of GPU work.

| Feature | Description | Est. Lines | Benefit |
|---------|-------------|-----------|---------|
| **X-CPU1** | **SSE/AVX2 vectorized tensor ops** — 4-8x matvec speedup | ~300 | Immediate |
| **X-CPU2** | **NVMe write support** — persistent storage | ~200 | Utility |
| **X-CPU3** | **UDP prompt server** — network inference API | ~300 | Usability |

### X-CPU1: AVX2 Tensor Ops

Vectorize the hot paths with AVX2/FMA intrinsics:
- `matvec_q4_0`: SIMD nibble unpack + FMA dot product (~8x speedup)
- `rmsnorm`: AVX2 sum-of-squares + element-wise multiply (~4x speedup)
- Runtime CPUID detection with scalar fallback

**Implementation**: Separate `tensor_avx2.c` compiled with `-mavx2 -mfma`, runtime
dispatch via CPUID check. Enables AVX state (CR4.OSXSAVE + XCR0) on bare-metal.

### X-CPU2: NVMe Write Support

Current NVMe driver is read-only (X5). Adding write support enables:
- Model output persistence
- Checkpoint saving
- Log storage

### X-CPU3: UDP Prompt Server

Extend X10 network stack with a prompt/response protocol:
- Receive prompt via UDP
- Run inference
- Send generated tokens back

---

## Phase E: Contingency Plans — VARIABLE RISK

If GSP boot chain (Phase A) fails, these are fallback paths.

| Feature | Description | Risk | Viability |
|---------|-------------|------|-----------|
| **X-ALT1** | **UEFI GOP state exploitation** — use pre-initialized GPU state from UEFI, push commands to PFIFO directly | Medium | Turing only |
| **X-ALT2** | **SEC2 Falcon for FWSEC** — on Turing, FWSEC runs on SEC2 not GSP. Use SEC2 (0x087000) as bootstrap | Medium | Turing only |
| **X-ALT3** | **CPU-only optimization ceiling** — AVX-512, multi-core, memory-mapped I/O, push scalar to limit | Low | Universal |

### X-ALT1: UEFI GOP State Exploitation

After UEFI's GOP driver initializes the GPU for display, some state remains:
- PFIFO may be configured
- Display engine is active
- Some channels may exist

We could potentially:
1. Discover existing PFIFO channels
2. Allocate a compute channel alongside the display channel
3. Push compute commands directly

**Risk**: Undocumented UEFI GPU state, may conflict with display.

### X-ALT2: SEC2 Falcon for FWSEC

On Turing GPUs, FWSEC-FRTS is executed by SEC2 (not GSP):
- SEC2 base: 0x087000
- Same Falcon architecture, same PIO load mechanism
- FWSEC runs on SEC2, creates WPR2, then GSP boots normally

This is a Turing-specific codepath that may not work on Ampere/Ada.

### X-ALT3: CPU-Only Optimization Ceiling

If GPU compute is unreachable, maximize CPU inference:
- AVX-512 (if available): 16 floats/cycle → ~2x over AVX2
- Multi-core: parallelize across layers (requires SMP init)
- Batched I/O: overlap NVMe reads with compute
- Estimated ceiling: ~1-2 tok/s for 1B model (vs ~50+ tok/s on GPU)

---

## Recommended Execution Order

```
Priority  Feature    Rationale
───────── ────────── ──────────────────────────────────────────
1st       X-CPU1     AVX2 tensorops — DONE
2nd       X27        Falcon PIO load — DONE
3rd       X28        GBL + FWSEC-FRTS — DONE (needs hardware validation)
4th       X29        WPR2 metadata + Radix3 page tables  ← NEXT
          X37        SASS compile — can parallelize with X29
5th       X30 → X31 → X32 → X36 → X38 → X40
```

## Dependencies

```
X-CPU1 ──────────────────────────────────────────────── (independent)
X-CPU2 ──────────────────────────────────────────────── (independent)
X-CPU3 ──────────────────────────────────────────────── (independent)

X27 → X28 → X29 → X30 → X31 ──┐
                                ├── X32 → X33 → X34 → X35 → X36
X37 ───────────────────────────┘         ↓
                                    X38 → X39 → X40
```

## Files

| File | Phase | Description |
|------|-------|-------------|
| `arch/x86/kernel/tensor_avx2.c` | X-CPU1 | AVX2 vectorized tensor ops |
| `arch/x86/kernel/tensor.c` | X-CPU1 | Runtime dispatch (AVX2/scalar) |
| `arch/x86/drivers/gsp.c` | X27-X31 | Falcon PIO load + boot chain fix |
| `arch/x86/drivers/gpu.h` | X27-X31 | PIO API declarations |
| `arch/x86/drivers/gmmu.c` | X32 | GPU page tables (new) |
| `arch/x86/drivers/channel.c` | X33 | GPFIFO channel management (new) |
| `arch/x86/drivers/compute.c` | X34 | Compute dispatch (new) |
| `tools/cuda/` | X37 | SASS compilation tools (new) |
