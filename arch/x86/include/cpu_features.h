/*
 * arch/x86/include/cpu_features.h — centralized CPU capability detection
 *
 * Populated once at boot by cpu_features_detect(). All other kernel
 * subsystems (dispatch.c, perf.c, tensor.c, tensor_arena.c, ASLR) read
 * from the shared struct instead of issuing CPUID themselves.
 */
#ifndef OSITOK_CPU_FEATURES_H
#define OSITOK_CPU_FEATURES_H

#include "types.h"
#include "stdint.h"

typedef struct {
    /* Vendor */
    char     vendor[13];          /* "GenuineIntel", "AuthenticAMD", ... */
    char     brand[49];           /* CPUID brand string */

    /* SIMD / compute */
    bool     sse, sse2, sse3, ssse3, sse4_1, sse4_2;
    bool     avx, avx2, fma, f16c;
    bool     avx512f;
    bool     bmi1, bmi2, adx;
    bool     erms;                /* Enhanced REP MOVSB/STOSB */
    bool     xsave, osxsave;

    /* RNG / misc */
    bool     rdrand, rdseed;
    bool     invariant_tsc;
    bool     tsc_deadline;
    bool     rdtscp;

    /* PMU (Performance Monitoring Unit) */
    bool     pmu_available;
    uint8_t  pmu_version;         /* CPUID.0Ah.EAX[7:0] */
    uint8_t  pmu_num_counters;    /* CPUID.0Ah.EAX[15:8] */
    uint8_t  pmu_counter_width;   /* CPUID.0Ah.EAX[23:16] */
    uint8_t  pmu_num_fixed;       /* CPUID.0Ah.EDX[4:0] */

    /* Paging features */
    bool     pge, pse, pae;
    bool     huge_1g;             /* 1 GB pages supported */
    bool     la57;                /* 5-level paging */

    /* Topology */
    bool     hyperthreading;
    uint8_t  num_logical_cpus;    /* from CPUID.1.EBX[23:16] */

    /* Cache hierarchy (bytes) */
    uint32_t l1d_size, l1d_line;
    uint32_t l2_size,  l2_line;
    uint32_t l3_size,  l3_line;
    uint32_t cache_line_size;     /* the dominant line size, usually 64 */
} cpu_features_t;

extern cpu_features_t cpu_features;

/* Call once, early in boot (before paging_init). Populates cpu_features. */
void cpu_features_detect(void);

/* Dump to serial for visibility. */
void cpu_features_dump(void);

/* hw_random64 — uses RDRAND if available, else rdtsc-mixed fallback. */
uint64_t hw_random64(void);

#endif /* OSITOK_CPU_FEATURES_H */
