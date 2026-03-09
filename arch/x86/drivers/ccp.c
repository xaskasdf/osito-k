/*
 * OsitoK x86-64 — AMD CCP TRNG Driver
 *
 * Accesses the hardware True Random Number Generator in the
 * AMD Cryptographic Co-Processor (PSP) [1022:1486].
 *
 * BAR2 contains the TRNG output register at offset 0x010C.
 * Each read returns a fresh 32-bit hardware random value.
 */

#include "../include/types.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);

/* ── Registers ───────────────────────────────────────────────── */

#define CCP_TRNG_OUT    0x010C    /* 32-bit TRNG output */

/* ── State ───────────────────────────────────────────────────── */

static volatile void *ccp_bar2;
static bool ccp_ready;

/* ── Register access ─────────────────────────────────────────── */

static uint32_t ccp_read32(uint32_t offset)
{
    return mmio_read32((volatile void *)((uint64_t)ccp_bar2 + offset));
}

/* ── Public API ──────────────────────────────────────────────── */

/* Returns 64-bit hardware random number.
 * Falls back to RDTSC if CCP not initialized. */
uint64_t ccp_random(void)
{
    if (!ccp_ready) {
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
    }
    uint32_t lo = ccp_read32(CCP_TRNG_OUT);
    rmb();
    uint32_t hi = ccp_read32(CCP_TRNG_OUT);
    return ((uint64_t)hi << 32) | lo;
}

/* Returns 32-bit hardware random number. */
uint32_t ccp_random32(void)
{
    if (!ccp_ready) {
        uint32_t lo;
        __asm__ volatile ("rdtsc" : "=a"(lo) : : "edx");
        return lo;
    }
    return ccp_read32(CCP_TRNG_OUT);
}

bool ccp_is_ready(void) { return ccp_ready; }

/* ── Init ────────────────────────────────────────────────────── */

int ccp_init(uint64_t bar2_phys)
{
    ccp_bar2 = (volatile void *)bar2_phys;
    ccp_ready = false;

    serial_puts("[CCP] AMD CCP BAR2=");
    serial_puthex(bar2_phys, 16);
    serial_puts("\n");

    /* Sanity: read TRNG twice */
    uint32_t val1 = ccp_read32(CCP_TRNG_OUT);
    uint32_t val2 = ccp_read32(CCP_TRNG_OUT);

    if (val1 == 0xFFFFFFFF && val2 == 0xFFFFFFFF) {
        serial_puts("[CCP] BAR2 not accessible\n");
        return -1;
    }

    /* If same value, pause and retry */
    if (val1 == val2) {
        for (int i = 0; i < 1000; i++)
            __asm__ volatile ("pause");
        val2 = ccp_read32(CCP_TRNG_OUT);
    }

    serial_puts("[CCP] TRNG sample: ");
    serial_puthex(val1, 8);
    serial_puts(" ");
    serial_puthex(val2, 8);
    serial_puts("\n");

    ccp_ready = true;
    serial_puts("[CCP] Hardware TRNG ready\n");
    fb_puts(" CCP: TRNG ready\n");
    return 0;
}
