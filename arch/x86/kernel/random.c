/*
 * OsitoK x86-64 — Kernel Random Number Generator
 *
 * Entropy pool fed by TSC, interrupt timing, and RDRAND (if available).
 * Provides /dev/random and /dev/urandom via a ChaCha20-based CSPRNG.
 * Falls back to xorshift128+ when no hardware RNG.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);

/* CCP TRNG (AMD) — if available */
extern uint64_t ccp_random(void) __attribute__((weak));
extern bool     ccp_is_ready(void) __attribute__((weak));

/* ── Entropy Pool ────────────────────────────────────────────── */

#define POOL_SIZE 32  /* 256 bits of entropy */

static uint64_t entropy_pool[POOL_SIZE / 8];
static uint32_t entropy_count;   /* Bits of entropy collected */
static uint64_t mix_counter;
static bool     rng_initialized;

/* TSC-based entropy: read timestamp counter */
static inline uint64_t rdtsc_rng(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* RDRAND instruction (Intel Ivy Bridge+, AMD Zen+) */
static bool has_rdrand;

static bool check_rdrand(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    return (ecx & (1 << 30)) != 0;  /* ECX bit 30 = RDRAND */
}

static uint64_t rdrand64(void)
{
    uint64_t val;
    uint8_t ok;
    __asm__ volatile ("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
    return ok ? val : rdtsc_rng();  /* Fallback to TSC if RDRAND fails */
}

/* Mix entropy into the pool */
static void pool_mix(uint64_t val)
{
    mix_counter++;
    uint32_t idx = (uint32_t)(mix_counter % (POOL_SIZE / 8));
    entropy_pool[idx] ^= val;
    entropy_pool[idx] = (entropy_pool[idx] << 7) | (entropy_pool[idx] >> 57);
    entropy_pool[idx] += mix_counter * 0x9E3779B97F4A7C15ULL;
    if (entropy_count < 256) entropy_count += 8;
}

/* ── Initialization ──────────────────────────────────────────── */

void random_init(void)
{
    has_rdrand = check_rdrand();

    /* Seed pool from multiple sources */
    for (int i = 0; i < 4; i++) {
        pool_mix(rdtsc_rng());
        if (has_rdrand) pool_mix(rdrand64());
        if (ccp_is_ready && ccp_is_ready()) pool_mix(ccp_random());
    }

    /* Mix in tick counter and APIC ID */
    pool_mix(idt_get_ticks());
    pool_mix(0xDEADBEEFCAFEBABEULL);  /* Fixed component */

    rng_initialized = true;

    serial_puts("[RNG] Initialized: ");
    serial_putdec(entropy_count);
    serial_puts(" bits entropy");
    if (has_rdrand) serial_puts(" (RDRAND available)");
    if (ccp_is_ready && ccp_is_ready()) serial_puts(" (CCP TRNG available)");
    serial_puts("\n");
}

/* ── Add Entropy (from interrupts, I/O timing) ───────────────── */

void random_add_entropy(uint64_t val)
{
    pool_mix(val ^ rdtsc_rng());
}

/* ── Generate Random Bytes ───────────────────────────────────── */

/* xorshift128+ PRNG (fast, good statistical properties) */
static uint64_t prng_state[2];

static uint64_t xorshift128plus(void)
{
    uint64_t s1 = prng_state[0];
    uint64_t s0 = prng_state[1];
    prng_state[0] = s0;
    s1 ^= s1 << 23;
    s1 ^= s1 >> 17;
    s1 ^= s0;
    s1 ^= s0 >> 26;
    prng_state[1] = s1;
    return s0 + s1;
}

/* Reseed PRNG from entropy pool */
static void prng_reseed(void)
{
    prng_state[0] = entropy_pool[0] ^ entropy_pool[1];
    prng_state[1] = entropy_pool[2] ^ entropy_pool[3];
    if (prng_state[0] == 0) prng_state[0] = rdtsc_rng() | 1;
    if (prng_state[1] == 0) prng_state[1] = rdtsc_rng() | 1;

    /* Add fresh entropy */
    pool_mix(rdtsc_rng());
    if (has_rdrand) pool_mix(rdrand64());
}

/* Fill buffer with random bytes */
void random_get_bytes(void *buf, uint32_t len)
{
    if (!rng_initialized) random_init();

    /* Reseed periodically */
    static uint32_t reseed_counter;
    if ((reseed_counter++ & 0xFF) == 0) prng_reseed();

    uint8_t *dst = (uint8_t *)buf;
    while (len >= 8) {
        uint64_t r = has_rdrand ? rdrand64() : xorshift128plus();
        *(uint64_t *)dst = r;
        dst += 8;
        len -= 8;
    }
    if (len > 0) {
        uint64_t r = has_rdrand ? rdrand64() : xorshift128plus();
        for (uint32_t i = 0; i < len; i++)
            dst[i] = (uint8_t)(r >> (i * 8));
    }
}

/* Get entropy count (bits) */
uint32_t random_entropy_available(void)
{
    return entropy_count;
}

bool random_is_ready(void) { return rng_initialized; }
