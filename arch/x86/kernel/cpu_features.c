/*
 * arch/x86/kernel/cpu_features.c — centralized CPUID detection
 *
 * Called once at boot. Migrates the CPUID logic previously scattered
 * across tensor.c (AVX2), process.c (MSR_FS_BASE), and various callers.
 */

#include "../include/cpu_features.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern void serial_puthex(uint64_t v, int d);

cpu_features_t cpu_features;

static inline void cpuid(uint32_t leaf,
                         uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(0));
}

static inline void cpuid_ex(uint32_t leaf, uint32_t sub,
                            uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(sub));
}

static inline uint64_t rdtsc_raw(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void cpu_features_detect(void)
{
    uint32_t a, b, c, d;

    /* ── Leaf 0: vendor string + max standard leaf ── */
    cpuid(0, &a, &b, &c, &d);
    uint32_t max_std = a;
    ((uint32_t *)cpu_features.vendor)[0] = b;
    ((uint32_t *)cpu_features.vendor)[1] = d;
    ((uint32_t *)cpu_features.vendor)[2] = c;
    cpu_features.vendor[12] = 0;

    /* ── Leaf 1: feature flags + topology ── */
    cpuid(1, &a, &b, &c, &d);
    cpu_features.sse         = (d >> 25) & 1;
    cpu_features.sse2        = (d >> 26) & 1;
    cpu_features.sse3        = (c >>  0) & 1;
    cpu_features.ssse3       = (c >>  9) & 1;
    cpu_features.sse4_1      = (c >> 19) & 1;
    cpu_features.sse4_2      = (c >> 20) & 1;
    cpu_features.avx         = (c >> 28) & 1;
    cpu_features.fma         = (c >> 12) & 1;
    cpu_features.f16c        = (c >> 29) & 1;
    cpu_features.xsave       = (c >> 26) & 1;
    cpu_features.osxsave     = (c >> 27) & 1;
    cpu_features.rdrand      = (c >> 30) & 1;
    cpu_features.tsc_deadline= (c >> 24) & 1;
    cpu_features.pse         = (d >>  3) & 1;
    cpu_features.pae         = (d >>  6) & 1;
    cpu_features.pge         = (d >> 13) & 1;
    cpu_features.hyperthreading = (d >> 28) & 1;
    cpu_features.num_logical_cpus = (uint8_t)((b >> 16) & 0xFF);

    /* ── Leaf 7: extended feature flags ── */
    if (max_std >= 7) {
        cpuid_ex(7, 0, &a, &b, &c, &d);
        cpu_features.bmi1    = (b >>  3) & 1;
        cpu_features.avx2    = (b >>  5) & 1;
        cpu_features.bmi2    = (b >>  8) & 1;
        cpu_features.erms    = (b >>  9) & 1;
        cpu_features.rdseed  = (b >> 18) & 1;
        cpu_features.adx     = (b >> 19) & 1;
        cpu_features.avx512f = (b >> 16) & 1;
        cpu_features.la57    = (c >> 16) & 1;
    }

    /* ── Leaf 0Ah: PMU version ── */
    if (max_std >= 0xA) {
        cpuid(0xA, &a, &b, &c, &d);
        cpu_features.pmu_version       = (uint8_t)(a & 0xFF);
        cpu_features.pmu_num_counters  = (uint8_t)((a >> 8) & 0xFF);
        cpu_features.pmu_counter_width = (uint8_t)((a >> 16) & 0xFF);
        cpu_features.pmu_num_fixed     = (uint8_t)(d & 0x1F);
        cpu_features.pmu_available     = (cpu_features.pmu_version != 0);
    }

    /* ── Leaf 80000007h: invariant TSC ── */
    cpuid(0x80000000u, &a, &b, &c, &d);
    uint32_t max_ext = a;
    if (max_ext >= 0x80000007u) {
        cpuid(0x80000007u, &a, &b, &c, &d);
        cpu_features.invariant_tsc = (d >> 8) & 1;
    }

    /* ── Leaf 80000001h: 1GB pages ── */
    if (max_ext >= 0x80000001u) {
        cpuid(0x80000001u, &a, &b, &c, &d);
        cpu_features.huge_1g = (d >> 26) & 1;
        cpu_features.rdtscp  = (d >> 27) & 1;
    }

    /* ── Leaves 80000002..80000004h: brand string ── */
    if (max_ext >= 0x80000004u) {
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002u + i, &a, &b, &c, &d);
            ((uint32_t *)cpu_features.brand)[i * 4 + 0] = a;
            ((uint32_t *)cpu_features.brand)[i * 4 + 1] = b;
            ((uint32_t *)cpu_features.brand)[i * 4 + 2] = c;
            ((uint32_t *)cpu_features.brand)[i * 4 + 3] = d;
        }
        cpu_features.brand[48] = 0;
    } else {
        cpu_features.brand[0] = 0;
    }

    /* ── Leaf 4: deterministic cache parameters (Intel) ── */
    if (max_std >= 4) {
        for (uint32_t i = 0; i < 16; i++) {
            cpuid_ex(4, i, &a, &b, &c, &d);
            uint8_t ctype = a & 0x1F;
            if (ctype == 0) break;
            uint8_t level = (a >> 5) & 7;
            uint32_t line = (b & 0xFFF) + 1;
            uint32_t part = ((b >> 12) & 0x3FF) + 1;
            uint32_t ways = ((b >> 22) & 0x3FF) + 1;
            uint32_t sets = c + 1;
            uint32_t size = line * part * ways * sets;
            if (level == 1 && ctype == 1) {
                cpu_features.l1d_size = size;
                cpu_features.l1d_line = line;
            } else if (level == 2) {
                cpu_features.l2_size = size;
                cpu_features.l2_line = line;
            } else if (level == 3) {
                cpu_features.l3_size = size;
                cpu_features.l3_line = line;
            }
            if (line > cpu_features.cache_line_size)
                cpu_features.cache_line_size = line;
        }
    }
    if (cpu_features.cache_line_size == 0)
        cpu_features.cache_line_size = 64;

    /* ── Enable XSAVE + AVX state (migrated from tensor.c:55-66) ── */
    if (cpu_features.xsave && cpu_features.avx) {
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        if (!(cr4 & (1ULL << 18))) {
            cr4 |= (1ULL << 18);
            __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
            cpu_features.osxsave = 1;
        }
        uint32_t xcr0_lo, xcr0_hi;
        __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
        /* isr_common allocates the standard 832-byte x87/SSE/AVX image.
         * Do not inherit firmware-enabled AVX-512 or AMX components. */
        if (xcr0_lo != 0x7 || xcr0_hi != 0)
            __asm__ volatile("xsetbv" :: "a"(0x7), "d"(0), "c"(0));
    }
}

void cpu_features_dump(void)
{
    serial_puts("[CPU] ");
    serial_puts(cpu_features.brand[0] ? cpu_features.brand : cpu_features.vendor);
    serial_puts("\n[CPU] Flags:");
    if (cpu_features.sse2)     serial_puts(" SSE2");
    if (cpu_features.sse4_2)   serial_puts(" SSE4.2");
    if (cpu_features.avx)      serial_puts(" AVX");
    if (cpu_features.avx2)     serial_puts(" AVX2");
    if (cpu_features.fma)      serial_puts(" FMA");
    if (cpu_features.f16c)     serial_puts(" F16C");
    if (cpu_features.bmi2)     serial_puts(" BMI2");
    if (cpu_features.erms)     serial_puts(" ERMS");
    if (cpu_features.rdrand)   serial_puts(" RDRAND");
    if (cpu_features.rdseed)   serial_puts(" RDSEED");
    if (cpu_features.avx512f)  serial_puts(" AVX512F");
    if (cpu_features.huge_1g)  serial_puts(" 1GPAGES");
    if (cpu_features.invariant_tsc) serial_puts(" TSC_INV");
    if (cpu_features.rdtscp)        serial_puts(" RDTSCP");
    serial_puts("\n[CPU] Cache: L1d=");
    serial_putdec(cpu_features.l1d_size / 1024);
    serial_puts("KB L2=");
    serial_putdec(cpu_features.l2_size / 1024);
    serial_puts("KB L3=");
    serial_putdec(cpu_features.l3_size / 1024);
    serial_puts("KB line=");
    serial_putdec(cpu_features.cache_line_size);
    serial_puts("\n");
    if (cpu_features.pmu_available) {
        serial_puts("[CPU] PMU v");
        serial_putdec(cpu_features.pmu_version);
        serial_puts(" (");
        serial_putdec(cpu_features.pmu_num_counters);
        serial_puts(" programmable + ");
        serial_putdec(cpu_features.pmu_num_fixed);
        serial_puts(" fixed, ");
        serial_putdec(cpu_features.pmu_counter_width);
        serial_puts("-bit)\n");
    } else {
        serial_puts("[CPU] PMU unavailable\n");
    }
}

uint64_t hw_random64(void)
{
    if (cpu_features.rdrand) {
        for (int tries = 0; tries < 10; tries++) {
            uint64_t v;
            uint8_t ok;
            __asm__ volatile("rdrand %0; setc %1"
                : "=r"(v), "=r"(ok));
            if (ok) return v;
        }
    }
    /* Fallback: TSC mixed with itself */
    uint64_t t = rdtsc_raw();
    t ^= t << 17;
    t ^= t >> 23;
    t ^= t << 29;
    return t;
}
