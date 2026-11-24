#include "../include/compat_iret.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t value);

static uint64_t espfix_pdpt[512] __attribute__((aligned(4096)));
static uint64_t espfix_pd[512] __attribute__((aligned(4096)));
static uint64_t espfix_pt[512] __attribute__((aligned(4096)));
uint8_t x86_espfix_slots[X86_ESPFIX_CPUS * X86_ESPFIX_SLOT_SIZE]
    __attribute__((aligned(4096)));

void x86_compat_iret_init(uint64_t *pml4)
{
    const uint64_t read_only_nx = 1ULL | (1ULL << 63);
    /* Share the same PT across every 2 MiB directory entry and the same
     * directory across four GiB. Each CPU slot then has a 64 KiB alias
     * for every possible ESP[31:16], using only three table pages. */
    for (unsigned repeat = 0; repeat < 32; repeat++)
        for (unsigned page = 0; page < sizeof(x86_espfix_slots) / 4096u; page++)
            espfix_pt[repeat * 16u + page] =
                kvirt_to_phys(x86_espfix_slots + page * 4096u) | read_only_nx;
    for (unsigned i = 0; i < 512; i++)
        espfix_pd[i] = kvirt_to_phys(espfix_pt) | read_only_nx;
    for (unsigned i = 0; i < 4; i++)
        espfix_pdpt[i] = kvirt_to_phys(espfix_pd) | read_only_nx;
    pml4[((uint64_t)X86_ESPFIX_BASE >> 39) & 511u] =
        kvirt_to_phys(espfix_pdpt) | read_only_nx;
    serial_puts("[PAGE] SS16 IRET aliases ready (28 KiB, 256 CPU slots)\n");
}

int x86_compat_iret_selftest(void)
{
    const uint32_t high_words[] = { 0, 0x10000, 0x1D60000, 0x3AF0000,
                                    0x7FFF0000, 0xFFFF0000 };
    uint64_t roots[] = { paging_get_kernel_cr3(), paging_create_process_cr3() };
    unsigned checks = 0;
    int failures = 0;
    for (unsigned root = 0; root < 2; root++) {
        if (!roots[root]) { failures++; continue; }
        for (unsigned cpu = 0; cpu < X86_ESPFIX_CPUS; cpu++)
        for (unsigned high = 0; high < sizeof(high_words) / sizeof(high_words[0]); high++) {
            uint64_t address = (uint64_t)X86_ESPFIX_BASE | high_words[high] |
                               (cpu * X86_ESPFIX_SLOT_SIZE);
            uint64_t physical = kvirt_to_phys(x86_espfix_slots + cpu * X86_ESPFIX_SLOT_SIZE);
            uint64_t flags = 0, size = 0;
            if (paging_translate_in_cr3(roots[root], address) != physical ||
                paging_translate_in_cr3(roots[root], address + 63u) != physical + 63u ||
                paging_query_mapping_in_cr3(roots[root], address, &flags, &size) != 0 ||
                !(flags & 1u) || (flags & 6u) || !(flags & (1ULL << 63)) || size != 4096)
                failures++;
            checks++;
        }
        if (paging_translate_in_cr3(roots[root], (uint64_t)X86_ESPFIX_BASE + 0x4000u)
                != UINT64_MAX ||
            paging_translate_in_cr3(roots[root], (uint64_t)X86_ESPFIX_BASE + (1ULL << 32))
                != UINT64_MAX) failures++;
        checks++;
    }
    if (roots[1]) paging_free_process_cr3(roots[1]);
    serial_puts("[ESPFIX-TEST] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}
