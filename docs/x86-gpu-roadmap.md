# OsitoK x86-64 — GPU Compute Roadmap

> Status: X1–X40 + X-CPU1 done. Phase A + B + C complete.
> Last updated: 2026-03-06

## Current State

X28 implemented the correct GBL-based FWSEC-FRTS boot sequence:
- PIO-load GBL microcode to Falcon IMEM
- BootloaderDmemDescV2 in DMEM with physical addresses of FWSEC in system RAM
- FBIF TRANSCFG for coherent system memory DMA
- SEC2 target for Turing, GSP for Ampere+
- Legacy VRAM+DMATRFBASE fallback preserved

**Phase A complete.** X27-X31 implement the full GSP secure boot chain infrastructure.
Hardware testing will determine which steps succeed on specific GPU generations.

X32 corrected all RPC function IDs (verified against `rpc_global_enums.h` 535.113.01)
and added generic RM alloc/control + subdevice + VASPACE allocation.
X33 allocated TSG + GPFIFO channel with instance/USERD memory.
X34 binds compute class to channel, activates via CTRL_BIND+SCHEDULE,
pushes SET_OBJECT + cache invalidate + semaphore fence through GPFIFO.
X35 implements CE DMA with physical addressing (sysmem↔VRAM), semaphore fencing.
X36 adds QMD construction (QMDV02_03), SEND_PCAS dispatch, kernel wait, result readback.
**Phase B complete.** Phase C (NTransformer port to GPU) also complete: X37-X40.

---

## Phase A: GSP Secure Boot Chain Fix (X27–X31) — HIGH RISK

The make-or-break phase. Without WPR2, GSP-RM cannot function.

| Feature | Description | Est. Lines | Risk |
|---------|-------------|-----------|------|
| **X27** | **Falcon PIO Load** — write code to IMEM/DMEM via IMEMC/IMEMD registers | ~150 | Done |
| **X28** | **GBL extraction + FWSEC-FRTS via GBL** — extract GBL from VBIOS, PIO-load to IMEM, GBL DMA-loads FWSEC from sysmem, execute FRTS | ~350 | Done |
| **X29** | **WPR2 metadata + Radix3 page tables** — build radix3 PT for GSP firmware, write WPR meta to VRAM | ~300 | Done |
| **X30** | **Two-stage GSP boot** — Falcon DMA load, bootloader-first boot, WPR meta mailboxes, legacy fallback | ~250 | Done |
| **X31** | **SEC2 booter load** — load booter.bin to SEC2 via DMA, boot with WPR meta, firmware→WPR2 | ~200 | Done |

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
| **X32** | **RPC ID fix + generic RM alloc/control** — corrected IDs, RM_ALLOC/RM_CONTROL wrappers, subdevice + VASPACE | ~200 | Done |
| **X33** | **Channel + GPFIFO** — TSG alloc, GPFIFO channel alloc, ring buffer + inst/USERD memory | ~200 | Done |
| **X34** | **Compute class bind + kernel dispatch** — bind compute class, push methods, semaphore fence | ~300 | Done |
| **X35** | **Copy Engine DMA** — host-to-device and device-to-host transfers, semaphore fence | ~250 | Done |
| **X36** | **Kernel completion + semaphore sync** — QMD build, SEND_PCAS dispatch, wait, result readback | ~200 | Done |

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
| **X37** | **Offline SASS compilation** — SASS instruction encoding, pre-encoded kernels (NOP/S2R/NOP4), VRAM upload, smoke test | ~300 | Done |
| **X38** | **GMMU page tables + kernel loader** — GP100+ MMU v2 identity map, constant buffer QMD, memory windows, instance block PDB | ~400 | Done |
| **X39** | **GPU tensor ops** — store_pattern + vec_add_f32 SASS kernels, VRAM buffer mgmt, dispatch layer, PTX docs | ~600 | Done |
| **X40** | **GPU-accelerated Llama inference** — hybrid GPU/CPU forward pass, kernel dispatch table, VRAM scratch, benchmark | ~450 | Done |

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
4th       X29        WPR2 metadata + Radix3 page tables — DONE
          X37        SASS compile — can parallelize
5th       X30 → X31 → X32 (all DONE) → X33 → X36 → X38 → X40
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
| `arch/x86/drivers/gsp.c` | X32-X36 | RM alloc/control, channel, compute, CE, dispatch |
| `arch/x86/drivers/gpu.h` | X32-X36 | Compute/CE/QMD types + API declarations |
| `arch/x86/drivers/sass.h` | X37 | SASS instruction types, encoding defines, kernel catalog |
| `arch/x86/drivers/sass.c` | X37 | Pre-encoded SASS kernels, VRAM upload, smoke test |
| `arch/x86/drivers/gmmu.c` | X38 | GMMU 5-level page tables, identity map, instance block PDB |
| `arch/x86/drivers/gpu_tensor.h` | X39 | GPU tensor ops API (VRAM alloc, transfer, dispatch) |
| `arch/x86/drivers/gpu_tensor.c` | X39 | Dispatch layer, PRAMIN transfer, self-test, PTX source docs |
| `arch/x86/drivers/sass.c` | X39 | +store_pattern (STG test), +vec_add_f32 (LDG+FADD+STG) |
| `arch/x86/drivers/gpu_inference.h` | X40 | GPU inference orchestration API (hybrid GPU/CPU forward pass) |
| `arch/x86/drivers/gpu_inference.c` | X40 | GPU-accelerated forward pass, dispatch table, benchmark |
| `arch/x86/kernel/main.c` | X40 | Integration: GPU inference run after GPU boot |
