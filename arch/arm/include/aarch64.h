/*
 * aarch64.h -- AArch64 system register helpers for EL1
 *
 * Inline assembly wrappers for system registers, cache ops, barriers.
 * Used by Osito-K kernel running at EL1 on SM8350.
 */

#ifndef OSITO_AARCH64_H
#define OSITO_AARCH64_H

#include <stdint.h>

/* ========================================================================
 * System register reads
 * ======================================================================== */

static inline uint64_t read_midr_el1(void) {
    uint64_t v; __asm__ volatile("mrs %0, MIDR_EL1" : "=r"(v)); return v;
}

static inline uint64_t read_mpidr_el1(void) {
    uint64_t v; __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(v)); return v;
}

static inline uint64_t read_currentel(void) {
    uint64_t v; __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return (v >> 2) & 3;
}

static inline uint64_t read_daif(void) {
    uint64_t v; __asm__ volatile("mrs %0, DAIF" : "=r"(v)); return v;
}

static inline uint64_t read_sctlr_el1(void) {
    uint64_t v; __asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(v)); return v;
}

static inline uint64_t read_tcr_el1(void) {
    uint64_t v; __asm__ volatile("mrs %0, TCR_EL1" : "=r"(v)); return v;
}

static inline uint64_t read_ttbr0_el1(void) {
    uint64_t v; __asm__ volatile("mrs %0, TTBR0_EL1" : "=r"(v)); return v;
}

static inline uint64_t read_ttbr1_el1(void) {
    uint64_t v; __asm__ volatile("mrs %0, TTBR1_EL1" : "=r"(v)); return v;
}

static inline uint64_t read_vbar_el1(void) {
    uint64_t v; __asm__ volatile("mrs %0, VBAR_EL1" : "=r"(v)); return v;
}

static inline uint64_t read_esr_el1(void) {
    uint64_t v; __asm__ volatile("mrs %0, ESR_EL1" : "=r"(v)); return v;
}

static inline uint64_t read_elr_el1(void) {
    uint64_t v; __asm__ volatile("mrs %0, ELR_EL1" : "=r"(v)); return v;
}

static inline uint64_t read_far_el1(void) {
    uint64_t v; __asm__ volatile("mrs %0, FAR_EL1" : "=r"(v)); return v;
}

/* ========================================================================
 * System register writes
 * ======================================================================== */

static inline void write_vbar_el1(uint64_t v) {
    __asm__ volatile("msr VBAR_EL1, %0" :: "r"(v));
}

static inline void write_sctlr_el1(uint64_t v) {
    __asm__ volatile("msr SCTLR_EL1, %0" :: "r"(v));
}

static inline void write_tcr_el1(uint64_t v) {
    __asm__ volatile("msr TCR_EL1, %0" :: "r"(v));
}

static inline void write_ttbr0_el1(uint64_t v) {
    __asm__ volatile("msr TTBR0_EL1, %0" :: "r"(v));
}

static inline void write_mair_el1(uint64_t v) {
    __asm__ volatile("msr MAIR_EL1, %0" :: "r"(v));
}

/* ========================================================================
 * Timer (ARM Generic Timer, 19.2 MHz on SM8350)
 * ======================================================================== */

static inline uint64_t read_cntpct_el0(void) {
    uint64_t v; __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(v)); return v;
}

static inline uint32_t read_cntfrq_el0(void) {
    uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return (uint32_t)v;
}

static inline void write_cntp_tval_el0(uint32_t v) {
    __asm__ volatile("msr CNTP_TVAL_EL0, %0" :: "r"((uint64_t)v));
}

static inline void write_cntp_ctl_el0(uint32_t v) {
    __asm__ volatile("msr CNTP_CTL_EL0, %0" :: "r"((uint64_t)v));
}

static inline uint32_t read_cntp_ctl_el0(void) {
    uint64_t v; __asm__ volatile("mrs %0, CNTP_CTL_EL0" : "=r"(v));
    return (uint32_t)v;
}

/* ========================================================================
 * GICv3 system register interface (ICC)
 * ======================================================================== */

static inline void write_icc_sre_el1(uint64_t v) {
    __asm__ volatile("msr S3_0_C12_C12_5, %0" :: "r"(v));  /* ICC_SRE_EL1 */
}

static inline uint64_t read_icc_sre_el1(void) {
    uint64_t v; __asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(v));
    return v;
}

static inline void write_icc_pmr_el1(uint64_t v) {
    __asm__ volatile("msr S3_0_C4_C6_0, %0" :: "r"(v));    /* ICC_PMR_EL1 */
}

static inline void write_icc_igrpen1_el1(uint64_t v) {
    __asm__ volatile("msr S3_0_C12_C12_7, %0" :: "r"(v));  /* ICC_IGRPEN1_EL1 */
}

static inline uint64_t read_icc_iar1_el1(void) {
    uint64_t v; __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v));
    return v;   /* ICC_IAR1_EL1 */
}

static inline void write_icc_eoir1_el1(uint64_t v) {
    __asm__ volatile("msr S3_0_C12_C12_1, %0" :: "r"(v));  /* ICC_EOIR1_EL1 */
}

/* ========================================================================
 * Interrupt enable/disable
 * ======================================================================== */

static inline void irq_enable(void) {
    __asm__ volatile("msr DAIFClr, #0x2" ::: "memory");
}

static inline void irq_disable(void) {
    __asm__ volatile("msr DAIFSet, #0x2" ::: "memory");
}

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("mrs %0, DAIF" : "=r"(flags));
    __asm__ volatile("msr DAIFSet, #0x2" ::: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("msr DAIF, %0" :: "r"(flags) : "memory");
}

/* ========================================================================
 * Cache operations
 * ======================================================================== */

static inline void icache_invalidate_all(void) {
    __asm__ volatile("ic iallu" ::: "memory");
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");
}

static inline void dcache_clean_invalidate_poc(void *addr) {
    __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
}

static inline void tlb_invalidate_all(void) {
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");
}

/* ========================================================================
 * Context frame for scheduler
 *
 * AArch64 context: 34 x 8 = 272 bytes minimum (no NEON).
 * With NEON (32 x Q128): 272 + 512 = 784 bytes.
 *
 * ESP8266 equivalent: 80 bytes (a0-a15 + PS + SAR + EPC1 + pad)
 * ======================================================================== */

struct aarch64_context {
    uint64_t x[31];     /* x0-x30 (x30 = LR) */
    uint64_t sp;        /* SP_EL0 */
    uint64_t elr;       /* ELR_EL1 (return address) */
    uint64_t spsr;      /* SPSR_EL1 (saved PSTATE) */
    /* NEON state would go here if needed (32 x 16 = 512 bytes) */
};

/* Stack must be 16-byte aligned (AArch64 ABI).
 * Recommended: 8KB per task (ESP8266 uses 1536B). */
#define TASK_STACK_SIZE     8192
#define TASK_STACK_ALIGN    16

#endif /* OSITO_AARCH64_H */
