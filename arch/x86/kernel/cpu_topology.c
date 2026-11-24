/*
 * OsitoK x86-64 — CPU Topology Detection
 *
 * Detects cores, threads, packages, NUMA nodes via CPUID.
 * Exposes via /sys/devices/system/cpu/cpuN/topology/.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* ── CPUID helper ────────────────────────────────────────────── */

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile ("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

/* ── CPU Info ────────────────────────────────────────────────── */

typedef struct {
    char     vendor[13];      /* "GenuineIntel" or "AuthenticAMD" */
    char     brand[49];       /* Full CPU name string */
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t max_leaf;
    uint32_t cores_per_package;
    uint32_t threads_per_core;
    uint32_t total_threads;
    uint32_t packages;
    bool     has_ht;          /* Hyper-threading */
    bool     has_avx;
    bool     has_avx2;
    bool     has_aes;
    bool     has_rdrand;
    uint32_t l1d_size_kb;
    uint32_t l1i_size_kb;
    uint32_t l2_size_kb;
    uint32_t l3_size_kb;
} cpu_info_t;

static cpu_info_t cpu;
static bool topology_detected;

/* ── Detection ───────────────────────────────────────────────── */

void cpu_topology_detect(void)
{
    uint32_t eax, ebx, ecx, edx;

    /* Leaf 0: vendor + max leaf */
    cpuid(0, &eax, &ebx, &ecx, &edx);
    cpu.max_leaf = eax;
    *(uint32_t *)(cpu.vendor + 0) = ebx;
    *(uint32_t *)(cpu.vendor + 4) = edx;
    *(uint32_t *)(cpu.vendor + 8) = ecx;
    cpu.vendor[12] = '\0';

    /* Leaf 1: family/model/stepping + features */
    cpuid(1, &eax, &ebx, &ecx, &edx);
    cpu.stepping = eax & 0xF;
    cpu.model = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);
    cpu.family = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);

    cpu.has_ht = (edx & (1 << 28)) != 0;
    cpu.has_aes = (ecx & (1 << 25)) != 0;
    cpu.has_avx = (ecx & (1 << 28)) != 0;
    cpu.has_rdrand = (ecx & (1 << 30)) != 0;

    /* Logical processors per package */
    uint32_t max_logical = (ebx >> 16) & 0xFF;

    /* Leaf 7: extended features (AVX2) */
    if (cpu.max_leaf >= 7) {
        cpuid(7, &eax, &ebx, &ecx, &edx);
        cpu.has_avx2 = (ebx & (1 << 5)) != 0;
    }

    /* Leaf 4: core topology (Intel) */
    if (cpu.max_leaf >= 4) {
        uint32_t eax4;
        __asm__ volatile ("cpuid" : "=a"(eax4), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(4), "c"(0));
        cpu.cores_per_package = ((eax4 >> 26) & 0x3F) + 1;
    } else {
        cpu.cores_per_package = 1;
    }

    cpu.threads_per_core = cpu.has_ht ? 2 : 1;
    if (max_logical > 0 && cpu.cores_per_package > 0)
        cpu.threads_per_core = max_logical / cpu.cores_per_package;
    if (cpu.threads_per_core == 0) cpu.threads_per_core = 1;

    cpu.total_threads = cpu.cores_per_package * cpu.threads_per_core;
    cpu.packages = 1;

    /* Brand string (leaves 0x80000002-0x80000004) */
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004) {
        uint32_t *brand = (uint32_t *)cpu.brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid(leaf, &brand[0], &brand[1], &brand[2], &brand[3]);
            brand += 4;
        }
        cpu.brand[48] = '\0';
    }

    /* Cache info (leaf 0x80000006 for L2/L3 on AMD, leaf 4 for Intel) */
    cpuid(0x80000006, &eax, &ebx, &ecx, &edx);
    cpu.l2_size_kb = (ecx >> 16) & 0xFFFF;
    cpu.l3_size_kb = ((edx >> 18) & 0x3FFF) * 512;

    topology_detected = true;

    serial_puts("[CPU] ");
    serial_puts(cpu.brand[0] ? cpu.brand : cpu.vendor);
    serial_puts(" — ");
    serial_putdec(cpu.cores_per_package);
    serial_puts(" cores, ");
    serial_putdec(cpu.threads_per_core);
    serial_puts(" threads/core");
    if (cpu.has_avx2) serial_puts(", AVX2");
    if (cpu.has_aes)  serial_puts(", AES-NI");
    if (cpu.l2_size_kb) { serial_puts(", L2="); serial_putdec(cpu.l2_size_kb); serial_puts("KB"); }
    serial_puts("\n");
}

/* ── Query API ───────────────────────────────────────────────── */

const char *cpu_get_vendor(void) { return topology_detected ? cpu.vendor : "unknown"; }
const char *cpu_get_brand(void)  { return topology_detected ? cpu.brand : ""; }
uint32_t cpu_get_cores(void)     { return topology_detected ? cpu.cores_per_package : 1; }
uint32_t cpu_get_threads(void)   { return topology_detected ? cpu.total_threads : 1; }
bool     cpu_has_avx2(void)      { return topology_detected && cpu.has_avx2; }
bool     cpu_has_aes(void)       { return topology_detected && cpu.has_aes; }
bool     cpu_has_rdrand(void)    { return topology_detected && cpu.has_rdrand; }
uint32_t cpu_get_l2_kb(void)     { return cpu.l2_size_kb; }
uint32_t cpu_get_l3_kb(void)     { return cpu.l3_size_kb; }
uint32_t cpu_get_family(void)    { return cpu.family; }
uint32_t cpu_get_model(void)     { return cpu.model; }
