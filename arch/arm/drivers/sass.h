/*
 * OsitoK x86-64 — SASS Kernel Infrastructure (X37)
 *
 * Pre-encoded SASS (Shader ASSembly) kernels for NVIDIA SM75+ GPUs.
 * Compute kernels are raw SASS instruction bytes — no SPH (Shader Program
 * Header) needed since QMD (Queue Meta Data) carries all execution metadata.
 *
 * Instruction format (Volta/Turing/Ampere, SM70+):
 *   128 bits per instruction = 2 × 64-bit words (little-endian):
 *     Word 0 (bits 63:0):   Opcode + register operands + modifiers
 *     Word 1 (bits 127:64): Control/scheduling codes
 *
 * Control code bit layout (upper 64 bits):
 *   Bits  1:4   — Stall count (0-15 cycles)
 *   Bit   5     — Yield hint
 *   Bits  6:8   — Write barrier index (7 = none)
 *   Bits  9:11  — Read barrier index (7 = none)
 *   Bits  12:17 — Wait barrier mask
 *   Bits  18:21 — Register reuse flags
 *
 * Reference: envytools, DocumentSASS, turingas, CuAssembler
 */

#ifndef OSITOK_SASS_H
#define OSITOK_SASS_H

#include "../include/types.h"

/* ── SASS Instruction Encoding ──────────────────────────── */

/* 128-bit SASS instruction (little-endian pair) */
typedef struct {
    uint64_t opcode;    /* Lower 64 bits: operation encoding */
    uint64_t control;   /* Upper 64 bits: scheduling/dependency */
} sass_insn_t;

/* ── Known SM75 (Turing) Instruction Encodings ─────────── */

/* Verified via nvdisasm / DocumentSASS / turingas */

/* EXIT — terminate thread execution */
#define SASS_EXIT_OPCODE    0x000000000000794dULL
#define SASS_EXIT_CONTROL   0x000fea0003800000ULL

/* NOP — no operation */
#define SASS_NOP_OPCODE     0x0000000000007918ULL
#define SASS_NOP_CONTROL    0x000fc00000000000ULL

/* S2R R2, SR_TID.X — load thread ID (X dimension) into R2 */
#define SASS_S2R_R2_TID_X_OPCODE   0x0000000000027919ULL
#define SASS_S2R_R2_TID_X_CONTROL  0x000e220000002100ULL

/* ── Control Code Helpers ──────────────────────────────── */

/* Build control code from individual fields */
#define SASS_CTRL(stall, yield, wbar, rbar, wmask, reuse) \
    (((uint64_t)(stall)  & 0xF)  << 1  | \
     ((uint64_t)(yield)  & 0x1)  << 5  | \
     ((uint64_t)(wbar)   & 0x7)  << 6  | \
     ((uint64_t)(rbar)   & 0x7)  << 9  | \
     ((uint64_t)(wmask)  & 0x3F) << 12 | \
     ((uint64_t)(reuse)  & 0xF)  << 18)

/* Default control: max stall, no barriers, no reuse */
#define SASS_CTRL_DEFAULT   SASS_CTRL(0xF, 0, 7, 7, 0, 0)

/* ── Kernel Descriptor ─────────────────────────────────── */

#define SASS_KERNEL_NAME_MAX  32
#define SASS_INSN_SIZE        16    /* 128 bits = 16 bytes */
#define SASS_CODE_ALIGNMENT   256   /* Must match QMD alignment */

/* VRAM placement for compiled kernels */
#define SASS_VRAM_OFFSET_MB   256   /* Kernel binaries at VRAM+256MB */

typedef struct {
    char          name[SASS_KERNEL_NAME_MAX];
    const uint8_t *code;           /* SASS binary data (host RAM) */
    uint32_t       code_size;      /* Size in bytes (multiple of 16) */
    uint32_t       num_insns;      /* Instruction count */
    uint32_t       register_count; /* GPRs per thread (for QMD) */
    uint32_t       shared_mem;     /* Shared memory bytes (for QMD) */
    uint32_t       barrier_count;  /* Barriers used (for QMD) */
    uint32_t       max_threads;    /* Max threads per CTA */
    uint64_t       vram_addr;      /* GPU VRAM address after upload */
    bool           uploaded;       /* Kernel is in VRAM */
} sass_kernel_t;

/* ── Kernel Catalog ───────────────────────────────────── */

#define SASS_MAX_KERNELS  24

typedef struct {
    sass_kernel_t  kernels[SASS_MAX_KERNELS];
    uint32_t       count;
    uint64_t       vram_base;     /* Base VRAM address for all kernels */
    uint64_t       vram_next;     /* Next available VRAM offset */
    bool           initialized;
} sass_state_t;

/* ── API ──────────────────────────────────────────────── */

/* Initialize SASS infrastructure, upload pre-encoded kernels to VRAM */
int  sass_init(void);

/* Get kernel by name (returns NULL if not found) */
sass_kernel_t *sass_get_kernel(const char *name);

/* Get SASS state */
sass_state_t *sass_get_state(void);

/* Upload a single kernel binary to VRAM (via PRAMIN) */
int  sass_upload_kernel(sass_kernel_t *k);

/* Smoke test: dispatch NOP kernel, check QMD semaphore */
int  sass_smoke_test(void);

/* X39: Store test — dispatch store_pattern, verify via PRAMIN */
int  sass_store_test(void);

/* X39: Patch vec_add_f32 addresses and re-upload to VRAM */
int  sass_patch_vec_add(uint32_t src_a, uint32_t src_b, uint32_t dst);

#endif /* OSITOK_SASS_H */
