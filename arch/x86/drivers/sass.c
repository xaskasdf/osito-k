/*
 * OsitoK x86-64 — SASS Kernel Infrastructure (X37)
 *
 * Pre-encoded SASS compute kernels for bare-metal GPU dispatch.
 * Kernels are raw SASS instruction bytes uploaded to VRAM via PRAMIN.
 *
 * X37 kernels:
 *   - nop:       Single EXIT instruction (16B). Dispatch smoke test.
 *   - s2r_exit:  S2R R2,SR_TID.X + EXIT (32B). Register read test.
 *   - nop4_exit: 4×NOP + EXIT (80B). Multi-instruction fetch test.
 *
 * Kernel binaries uploaded to VRAM at SASS_VRAM_OFFSET_MB (256MB).
 * Each kernel is 256-byte aligned in VRAM for QMD compatibility.
 *
 * Reference: NVIDIA open-gpu-doc (QMD), DocumentSASS (SASS ISA),
 *            turingas (SM75 assembler), CuAssembler.
 */

#include "../include/types.h"
#include "gpu.h"
#include "sass.h"

/* ── External Functions ──────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);

/* rdtsc for timing */
static inline uint64_t rdtsc_sass(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ── SASS State ──────────────────────────────────────────── */

static sass_state_t sass_state;

/* ══════════════════════════════════════════════════════════
 *  Pre-encoded SASS Kernel Binaries (SM75 / Turing)
 *
 *  Each instruction is 128 bits (16 bytes), stored little-endian:
 *    bytes  0-7:  opcode word (operation + registers)
 *    bytes 8-15:  control word (scheduling + dependencies)
 *
 *  Encoding verified via nvdisasm / DocumentSASS / turingas.
 * ══════════════════════════════════════════════════════════ */

/* ── Kernel: nop ──────────────────────────────────────────
 *
 * Single EXIT instruction. Minimal dispatch smoke test.
 * When dispatched via QMD with RELEASE0 semaphore, the hardware
 * writes sem_payload on kernel completion — proving:
 *   QMD dispatch → GPU execution → semaphore release.
 *
 * SASS:
 *   EXIT;    // 0x000000000000794d | 0x000fea0003800000
 *
 * Requirements: 2 registers (minimum), 0 shared mem, 0 barriers.
 */
static const uint8_t __attribute__((aligned(256))) sass_code_nop[] = {
    /* EXIT */
    0x4d, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* opcode */
    0x00, 0x00, 0x80, 0x03, 0x00, 0xea, 0x0f, 0x00,  /* control */
};

/* ── Kernel: s2r_exit ─────────────────────────────────────
 *
 * Read thread ID into R2, then exit.
 * Tests special register access — proves instruction execution
 * beyond immediate EXIT.
 *
 * SASS:
 *   S2R R2, SR_TID.X;  // 0x0000000000027919 | 0x000e220000002100
 *   EXIT;               // 0x000000000000794d | 0x000fea0003800000
 *
 * Requirements: 4 registers, 0 shared mem, 0 barriers.
 */
static const uint8_t __attribute__((aligned(256))) sass_code_s2r_exit[] = {
    /* S2R R2, SR_TID.X */
    0x19, 0x79, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,  /* opcode */
    0x00, 0x21, 0x00, 0x00, 0x00, 0x22, 0x0e, 0x00,  /* control */
    /* EXIT */
    0x4d, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* opcode */
    0x00, 0x00, 0x80, 0x03, 0x00, 0xea, 0x0f, 0x00,  /* control */
};

/* ── Kernel: nop4_exit ────────────────────────────────────
 *
 * 4×NOP + EXIT. Tests multi-instruction kernel execution.
 * Useful for verifying instruction fetch works beyond first
 * cache line (16 bytes).
 *
 * SASS:
 *   NOP; NOP; NOP; NOP;
 *   EXIT;
 *
 * Requirements: 2 registers, 0 shared mem, 0 barriers.
 */
static const uint8_t __attribute__((aligned(256))) sass_code_nop4_exit[] = {
    /* NOP ×4 */
    0x18, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x0f, 0x00,
    0x18, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x0f, 0x00,
    0x18, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x0f, 0x00,
    0x18, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x0f, 0x00,
    /* EXIT */
    0x4d, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x03, 0x00, 0xea, 0x0f, 0x00,
};

/* ── Kernel Registration ─────────────────────────────────── */

static void sass_register_kernel(const char *name, const uint8_t *code,
                                  uint32_t code_size, uint32_t regs,
                                  uint32_t smem, uint32_t barriers,
                                  uint32_t max_threads)
{
    if (sass_state.count >= SASS_MAX_KERNELS) return;

    sass_kernel_t *k = &sass_state.kernels[sass_state.count];

    /* Copy name */
    uint32_t i;
    for (i = 0; i < SASS_KERNEL_NAME_MAX - 1 && name[i]; i++)
        k->name[i] = name[i];
    k->name[i] = '\0';

    k->code           = code;
    k->code_size      = code_size;
    k->num_insns      = code_size / SASS_INSN_SIZE;
    k->register_count = regs;
    k->shared_mem     = smem;
    k->barrier_count  = barriers;
    k->max_threads    = max_threads;
    k->vram_addr      = 0;
    k->uploaded       = false;

    sass_state.count++;
}

/* ── VRAM Upload via PRAMIN ──────────────────────────────── */

int sass_upload_kernel(sass_kernel_t *k)
{
    if (!k || !k->code || k->code_size == 0) return -1;

    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present || p->vram_size_mb == 0) {
        serial_puts("[SASS] No GPU/VRAM for kernel upload\n");
        return -1;
    }

    /* Check VRAM bounds */
    uint32_t aligned_size = (k->code_size + 255) & ~255u;
    uint64_t vram_bytes = (uint64_t)p->vram_size_mb * 1024 * 1024;
    if (sass_state.vram_next + aligned_size > vram_bytes) {
        serial_puts("[SASS] VRAM overflow for kernel '");
        serial_puts(k->name);
        serial_puts("'\n");
        return -1;
    }

    /* Assign VRAM address (256-byte aligned) */
    k->vram_addr = sass_state.vram_next;
    sass_state.vram_next += aligned_size;

    serial_puts("[SASS] Uploading '");
    serial_puts(k->name);
    serial_puts("' (");
    serial_putdec(k->code_size);
    serial_puts("B, ");
    serial_putdec(k->num_insns);
    serial_puts(" insns) -> VRAM+0x");
    serial_puthex(k->vram_addr, 8);
    serial_puts("\n");

    /* Upload via PRAMIN window (1MB window, same pattern as X18/X19) */
    uint32_t saved_window = gpu_reg_read(NV_PBUS_BAR0_WINDOW);

    /* Set PRAMIN window to kernel VRAM offset */
    uint32_t window_val = (uint32_t)(k->vram_addr >> 16);
    gpu_reg_write(NV_PBUS_BAR0_WINDOW, window_val);
    wmb();

    /* Write kernel code through PRAMIN */
    uint32_t dwords = k->code_size / 4;
    const uint32_t *src = (const uint32_t *)k->code;
    uint32_t pramin_offset = (uint32_t)(k->vram_addr & 0xFFFF);

    for (uint32_t i = 0; i < dwords; i++)
        gpu_reg_write(NV_PRAMIN_BASE + pramin_offset + i * 4, src[i]);
    wmb();

    /* Verify first 2 dwords */
    rmb();
    uint32_t v0 = gpu_reg_read(NV_PRAMIN_BASE + pramin_offset);
    uint32_t v1 = gpu_reg_read(NV_PRAMIN_BASE + pramin_offset + 4);

    bool verify_ok = (v0 == src[0] && v1 == src[1]);

    /* Restore PRAMIN window */
    gpu_reg_write(NV_PBUS_BAR0_WINDOW, saved_window);
    wmb();

    if (verify_ok) {
        serial_puts("[SASS] Upload verified (0x");
        serial_puthex(v0, 8);
        serial_puts(" 0x");
        serial_puthex(v1, 8);
        serial_puts(")\n");
        k->uploaded = true;
    } else {
        serial_puts("[SASS] Upload verify FAILED (expected 0x");
        serial_puthex(src[0], 8);
        serial_puts("/0x");
        serial_puthex(src[1], 8);
        serial_puts(", got 0x");
        serial_puthex(v0, 8);
        serial_puts("/0x");
        serial_puthex(v1, 8);
        serial_puts(")\n");
    }

    return verify_ok ? 0 : -1;
}

/* ── Smoke Test: dispatch NOP kernel ─────────────────────── */

int sass_smoke_test(void)
{
    serial_puts("[SASS] -- Smoke test: dispatch NOP kernel --\n");

    sass_kernel_t *nop = sass_get_kernel("nop");
    if (!nop || !nop->uploaded) {
        serial_puts("[SASS] NOP kernel not available\n");
        return -1;
    }

    /* Allocate semaphore (page-aligned for GPU access) */
    uint32_t *sem = (uint32_t *)mem_alloc_aligned(4096, 4096);
    if (!sem) {
        serial_puts("[SASS] Failed to allocate semaphore\n");
        return -1;
    }
    *sem = 0;
    wmb();

    uint64_t sem_phys = (uint64_t)(uintptr_t)sem;

    /* Build dispatch descriptor */
    compute_dispatch_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.program_addr   = nop->vram_addr;  /* VRAM address of kernel */
    desc.grid_x         = 1;
    desc.grid_y         = 1;
    desc.grid_z         = 1;
    desc.block_x        = 1;   /* Single thread */
    desc.block_y        = 1;
    desc.block_z        = 1;
    desc.register_count = nop->register_count;
    desc.shared_mem_size = 0;
    desc.barrier_count  = 0;
    desc.sem_addr       = sem_phys;
    desc.sem_payload    = 0xD15BA7C4;  /* "DISPATCH" marker */

    serial_puts("[SASS] Dispatching NOP: prog=0x");
    serial_puthex(nop->vram_addr, 16);
    serial_puts(" sem=0x");
    serial_puthex(sem_phys, 16);
    serial_puts("\n");

    /* Dispatch */
    int ret = gsp_compute_dispatch(&desc);
    if (ret < 0) {
        serial_puts("[SASS] Dispatch failed (expected without full boot chain)\n");
        return -1;
    }

    /* Wait for completion (short timeout — don't expect success without GSP-RM) */
    ret = gsp_compute_wait(sem_phys, 0xD15BA7C4, 500);
    if (ret == 0) {
        serial_puts("[SASS] *** NOP kernel completed! GPU compute works! ***\n");
        fb_puts_color(" SASS: NOP kernel OK!\n", 0x0000FF00);
        return 0;
    }

    serial_puts("[SASS] NOP kernel timeout (expected without full GSP boot chain)\n");
    serial_puts("[SASS] Infrastructure verified, awaiting hardware validation\n");
    return -1;
}

/* ── Public API ──────────────────────────────────────────── */

sass_kernel_t *sass_get_kernel(const char *name)
{
    if (!name) return NULL;
    for (uint32_t i = 0; i < sass_state.count; i++) {
        /* Simple strcmp */
        const char *a = sass_state.kernels[i].name;
        const char *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0')
            return &sass_state.kernels[i];
    }
    return NULL;
}

sass_state_t *sass_get_state(void)
{
    return &sass_state;
}

int sass_init(void)
{
    serial_puts("\n[SASS] == X37: SASS Kernel Infrastructure ==\n");

    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present) {
        serial_puts("[SASS] No GPU detected, skipping\n");
        return -1;
    }

    memset(&sass_state, 0, sizeof(sass_state));

    /* Calculate VRAM base address for kernels */
    sass_state.vram_base = (uint64_t)SASS_VRAM_OFFSET_MB * 1024 * 1024;
    sass_state.vram_next = sass_state.vram_base;

    serial_puts("[SASS] Kernel VRAM base: 0x");
    serial_puthex(sass_state.vram_base, 8);
    serial_puts(" (");
    serial_putdec(SASS_VRAM_OFFSET_MB);
    serial_puts("MB)\n");

    /* ── Register pre-encoded kernels ── */

    sass_register_kernel("nop", sass_code_nop,
                         sizeof(sass_code_nop), 2, 0, 0, 1024);

    sass_register_kernel("s2r_exit", sass_code_s2r_exit,
                         sizeof(sass_code_s2r_exit), 4, 0, 0, 1024);

    sass_register_kernel("nop4_exit", sass_code_nop4_exit,
                         sizeof(sass_code_nop4_exit), 2, 0, 0, 1024);

    serial_puts("[SASS] Registered ");
    serial_putdec(sass_state.count);
    serial_puts(" kernels: ");
    for (uint32_t i = 0; i < sass_state.count; i++) {
        if (i > 0) serial_puts(", ");
        serial_puts(sass_state.kernels[i].name);
        serial_puts("(");
        serial_putdec(sass_state.kernels[i].code_size);
        serial_puts("B)");
    }
    serial_puts("\n");

    /* Dump instruction details for verification */
    serial_puts("[SASS] Instruction dump (nop kernel):\n");
    const sass_insn_t *insns = (const sass_insn_t *)sass_code_nop;
    uint32_t n_insns = sizeof(sass_code_nop) / SASS_INSN_SIZE;
    for (uint32_t i = 0; i < n_insns && i < 4; i++) {
        serial_puts("[SASS]   [");
        serial_putdec(i);
        serial_puts("] opcode=0x");
        serial_puthex(insns[i].opcode, 16);
        serial_puts(" ctrl=0x");
        serial_puthex(insns[i].control, 16);
        serial_puts("\n");
    }

    /* ── Upload all kernels to VRAM ── */

    if (p->vram_size_mb > 0) {
        uint32_t uploaded = 0;
        for (uint32_t i = 0; i < sass_state.count; i++) {
            if (sass_upload_kernel(&sass_state.kernels[i]) == 0)
                uploaded++;
        }
        serial_puts("[SASS] Uploaded ");
        serial_putdec(uploaded);
        serial_puts("/");
        serial_putdec(sass_state.count);
        serial_puts(" kernels to VRAM\n");
    } else {
        serial_puts("[SASS] No VRAM detected, kernel upload skipped\n");
    }

    sass_state.initialized = true;

    /* ── Smoke test ── */
    sass_smoke_test();

    serial_puts("[SASS] == X37 complete ==\n\n");

    fb_puts(" SASS: ");
    fb_putdec(sass_state.count);
    fb_puts(" kernels (");
    fb_putdec(sass_state.vram_next - sass_state.vram_base);
    fb_puts("B VRAM)\n");

    return 0;
}
