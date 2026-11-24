# OsitoK ARM64 — Future Work & Research Notes

> Reference document for ongoing debugging and future implementation.
> Generated 2026-03-20 from investigation sessions.

## 1. NEON SIMD Optimization (In Progress — Needs Debugging)

### Current State
- `tensor_neon.c` exists with NEON implementations of hot-path functions
- AVX2 stubs in `stubs.c` redirect to NEON: `rmsnorm_avx2→rmsnorm_neon`, etc.
- Compiles clean on both virt and SM8350 platforms
- **Not yet tested with real inference workload** — needs debugging on hardware

### Implementation Details

**tensor_neon.c functions:**
- `matvec_q4_0_neon()` — Q4_0 quantized matrix-vector multiply (90% of inference)
- `rmsnorm_neon()` — RMS normalization with vectorized sum-of-squares
- `vec_add_neon()`, `vec_mul_neon()` — trivial 4-wide float ops

**NEON vs AVX2 comparison:**

| Aspect | AVX2 (x86) | NEON (ARM64) |
|--------|-----------|-------------|
| Register width | 256-bit | 128-bit |
| Floats per register | 8 | 4 |
| FMA latency | 5 cycles | 3 cycles |
| FMA throughput | 1/cycle (2 ports) | 2/cycle (2 pipes) |
| Equivalent throughput | 8 floats/cycle | 8 floats/cycle |
| Loop iterations per block | 4 groups | 8 groups (2x wider loop) |

**Key NEON intrinsics used:**
```c
vld1q_f32()      // Load 4 floats
vfmaq_f32()      // Fused multiply-add: d = a + b*c
vaddvq_f32()     // Horizontal sum (4→1)
vld1q_u8()       // Load 16 bytes (for nibble unpacking)
vandq_u8()       // Bitwise AND (extract lo nibble)
vshrq_n_u8()     // Shift right (extract hi nibble)
vmovl_u8()       // Widen u8→u16
vmovl_u16()      // Widen u16→u32
vcvtq_f32_s32()  // Convert i32→f32
vsubq_s32()      // Subtract (bias removal)
vdupq_n_f32()    // Broadcast scalar to vector
```

### Known Issues to Debug

1. **Nibble interleaving in matvec_q4_0_neon may be incorrect**
   - Q4_0 format: 16 bytes encode 32 values (4-bit each)
   - Low nibble = even indices, high nibble = odd indices
   - The interleave pattern `{lo[0], hi[0], lo[1], hi[1]...}` maps to
     `{val[0], val[1], val[2], val[3]...}` — verify this matches x86 scalar
   - Test: compute same row with scalar and NEON, compare outputs

2. **Input pointer stride may be wrong**
   - Scalar code reads `inp[j*2]` and `inp[j*2+1]` (stride 2)
   - NEON code reads `inp`, `inp+1`, `inp+8`, `inp+9` etc.
   - These must match the same memory layout — verify with a known weight matrix

3. **f16_to_f32_neon may have edge cases**
   - Denormals, NaN, infinity handling
   - Compare against x86 `_cvtsh_ss()` output for edge inputs

4. **Software math functions (from tensor.c) need validation**
   - `sqrtf_bare()` — Quake fast inverse sqrt, 2 Newton iterations
   - `expf_bare()` — Pade/Taylor approximation
   - `sinf_bare()`, `cosf_bare()` — Taylor series with range reduction
   - `powf_bare()` — via log2 + expf
   - These replaced x87 FPU instructions; accuracy may differ for extreme inputs

### Debugging Strategy
```bash
# 1. Cross-compile a test binary that calls matvec with known data
aarch64-linux-gnu-gcc -static -nostdlib -o neon_test test/neon_test.S

# 2. Load via OsitoFS, exec from shell
# 3. Compare output vs scalar reference
# 4. Print intermediate values (scale, nibble values, partial sums)
```

### Expected Performance (once correct)
- Scalar C: ~1.0x baseline
- NEON: ~3-4x (limited by memory bandwidth on small models)
- GPU (SASS): ~100x (when PCIe GPU attached)

---

## 2. x86 PE32 Binary Compatibility (Research Phase)

### Problem Statement
OsitoK x86 can run Windows PE32 binaries (UT99 engine initializing).
ARM64 cannot run x86 binaries natively (different ISA).
Use cases: robotics teams want to run x86 tools/utilities on ARM64 boards.

### Options Analyzed

#### Option A: Software x86 Interpreter
- **LOC**: 10,000-15,000 lines
- **Performance**: 100-500x slower than native
- **Viability**: LOW for bare-metal (privileged instruction handling)
- **Approach**: Decode each x86 instruction, emulate in a big switch
- **Bare-metal gotchas**:
  - CPUID, RDMSR, WRMSR must return stubbed values
  - x87 FPU state emulation is expensive
  - Unaligned access: x86 allows, ARM64 faults → must catch and emulate
  - No privilege separation = one bad instruction crashes kernel

#### Option B: Binary Translation / JIT (Rosetta-style)
- **LOC**: 30,000-50,000 lines
- **Performance**: 2-5x overhead (amortized)
- **Viability**: MEDIUM (high effort, best long-term ROI)
- **Approach**: Translate x86 basic blocks → ARM64 at runtime, cache results
- **Key challenges**:
  - x86 EFLAGS tracking (per-instruction flags vs ARM64 NZCV on arithmetic only)
  - Code cache management: RWX pages, eviction
  - Indirect branch trampolines
  - x87/SSE minimal emulation (most apps use SSE for float)
- **Reference**: Apple Rosetta 2, Windows x86-on-ARM, QEMU TCG
- **Reusable from OsitoK**: DLL shim layer (100%), PE loader (100%)

#### Option C: API-Level Thunks (No Instruction Emulation)
- **LOC**: 500-1,000 lines
- **Performance**: Native
- **Viability**: HIGH (but limited utility)
- **Approach**: Detect PE32, patch IAT to ARM64 thunk functions
- **Limitation**: Only works if binary makes API calls, no inline x86 asm
- **Use case**: Console utilities, simple tools — NOT games or heavy apps

#### Option D: WASM Intermediate
- **Skip** — over-engineered for bare-metal, no advantage over interpreter

#### Option E: PE32+ ARM64 Native (Recommended First Step)
- **LOC**: 3,000-5,000 lines
- **Performance**: Native
- **Viability**: MEDIUM (need ARM64 PE test binaries)
- **Approach**: Adapt PE loader for ARM64 machine type, recompile DLL shims
- **What can be reused from x86 Win32 layer**:
  - `pe.c` (508 lines) — PE header validation, section loading, relocations
  - `dllloader.c` (581 lines) — DLL tracking, export resolution
  - `winexec.c` (774 lines) — PEB/TEB setup, orchestration
  - 15 DLL shims — all pure C, recompilable for ARM64
  - IAT patching mechanism — same architecture
- **What CANNOT be reused**:
  - `compat32.c` (2,067 lines) — INT 0x2E mode switching is x86-only
  - Thunk generation — cdecl→ms_abi conversion is x86-specific
- **Challenge**: Almost no ARM64 Windows PE binaries exist publicly

### Recommended Roadmap

**Phase 1 (Immediate)**: PE32+ ARM64 loader
```
1. Copy pe.c, dllloader.c, winexec.c from x86/win32/ to arm/win32/
2. Change machine type check: IMAGE_FILE_MACHINE_ARM64 (0xAA64)
3. Recompile DLL shims (kernel32, msvcrt, etc.) for ARM64
4. Remove INT 0x2E thunk layer (not needed — ARM64 native calls)
5. Test with a simple ARM64 PE binary (cross-compile with MSVC)
```

**Phase 2 (Future)**: x86 JIT translator
```
1. x86 instruction decoder (~2,000 lines)
2. ARM64 code generator (~8,000 lines)
3. Block cache + indirect branch handling (~4,000 lines)
4. EFLAGS lazy evaluation (~2,000 lines)
5. SSE/x87 minimal emulation (~3,000 lines)
Total: ~20,000 lines for basic x86-64→ARM64 translation
```

**Phase 3 (Stretch)**: Full compatibility
```
1. Self-modifying code detection
2. Memory model emulation (x86 TSO vs ARM64 weak ordering)
3. Exception handling (SEH→ARM64 exception vectors)
4. Threading model (x86 segments → ARM64 TPIDR)
```

### Architecture Notes

**x86 Win32 layer on OsitoK (current, x86-64 only):**
```
PE32 binary (32-bit x86 code)
    ↓ INT 0x2E
compat32.c (32→64 bit mode switch via IST1 stack)
    ↓ thunk dispatch
DLL shims (kernel32, msvcrt, user32, etc.)
    ↓ bare-metal syscall
Kernel services (OsitoFS, framebuffer, network, etc.)
```

**Proposed ARM64 Win32 layer (Option E):**
```
PE32+ binary (ARM64 native Windows code)
    ↓ direct call (no mode switch needed)
DLL shims (same C code, recompiled for ARM64)
    ↓ bare-metal syscall
Kernel services (OsitoFS, framebuffer, network, etc.)
```

**Proposed ARM64 x86 compat (Option B, future):**
```
PE32 binary (32-bit x86 code)
    ↓ JIT translation
ARM64 translated code (cached basic blocks)
    ↓ direct call
DLL shims (ARM64 native)
    ↓ bare-metal syscall
Kernel services
```

### Memory Barrier Mapping (for JIT translator)

| x86 | ARM64 | Usage |
|-----|-------|-------|
| MFENCE | DMB ISH | Full barrier |
| SFENCE | DMB ISHST | Store barrier |
| LFENCE | DMB ISHLD | Load barrier |
| LOCK prefix | LDAXR/STLXR | Atomic RMW |
| XCHG (implicit LOCK) | SWP or LDAXR/STLXR | Atomic exchange |

### Register Mapping (for JIT translator)

| x86-64 | ARM64 | Notes |
|--------|-------|-------|
| RAX | X0 | Return value |
| RCX | X1 | 1st arg (ms_abi) |
| RDX | X2 | 2nd arg |
| R8 | X3 | 3rd arg |
| R9 | X4 | 4th arg |
| RSP | SP | Stack pointer |
| RBP | X29 | Frame pointer |
| RIP | PC | Instruction pointer |
| RFLAGS | NZCV + lazy | Flags (see below) |

**EFLAGS lazy evaluation**: x86 sets flags on every ALU op. ARM64 only sets NZCV
when explicitly requested (`adds` vs `add`). JIT must track which flags are "live"
and only compute them when consumed (e.g., by conditional branch).

---

## 3. Other Future Work

### Dynamic Linking (dl_open/dl_sym)
- x86 has `dynlink.c` (35 KB) — needs ARM64 ELF relocation types
- `R_AARCH64_ABS64`, `R_AARCH64_JUMP_SLOT`, `R_AARCH64_GLOB_DAT`
- PLT/GOT uses `adrp + ldr` sequences instead of x86 `jmp *got_entry`

### QuickJS REPL
- x86 has `qjs_main.c` (6 KB) + QuickJS headers
- Pure C — should compile for ARM64 with minimal changes
- Needs: expanded syscall set, working malloc/free (already have)

### Self-Compilation (TCC)
- x86 compiles its own kernel with TCC in-OS
- ARM64: TCC has an arm64 backend (community contrib, less mature)
- Would need: TCC cross-compile for ARM64, test with OsitoK kernel sources

### Compositor/Display Pipeline
- x86 has `compositor.c`, `display.c`, `shm.c` for GUI windowing
- ARM64 has basic `gui_desktop.c` rendering
- Gap: no window management, no shared memory IPC for multi-app GUI
