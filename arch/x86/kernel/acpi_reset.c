#include "acpi_reset.h"
#include "../include/pci.h"

extern int paging_map_mmio(uint64_t, uint64_t);

enum { FADT_MIN_RESET = 129, FADT_MAX_SIZE = 4096, FADT_FLAGS = 112,
       FADT_RESET_REG = 116, FADT_RESET_VALUE = 128 };

#ifdef ACPI_RESET_HOST_TEST
extern void acpi_reset_test_outb(uint16_t, uint8_t);
extern uint8_t acpi_reset_test_inb(uint16_t);
#define reset_outb acpi_reset_test_outb
#define reset_inb acpi_reset_test_inb
#else
static void reset_outb(uint16_t port, uint8_t value)
{ __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port) : "memory"); }
static uint8_t reset_inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
#endif

const char *acpi_reset_prepare(const void *fadt, size_t available,
                               acpi_reset_register_t *reset)
{
    if (!reset) return "missing reset target";
    *reset = (acpi_reset_register_t){0};
    if (!fadt || available < FADT_MIN_RESET) return "FADT has no reset fields";
    const uint8_t *p = fadt;
    uint32_t length, flags;
    memcpy(&length, p + 4, sizeof(length));
    if (memcmp(p, "FACP", 4) || length < FADT_MIN_RESET ||
        length > available || length > FADT_MAX_SIZE || p[8] < 2)
        return "invalid FADT header";
    uint8_t checksum = 0;
    for (uint32_t i = 0; i < length; i++) checksum += p[i];
    if (checksum) return "invalid FADT checksum";
    memcpy(&flags, p + FADT_FLAGS, sizeof(flags));
    if (!(flags & (1U << 10))) return "ACPI reset not supported";
    const uint8_t *gas = p + FADT_RESET_REG;
    if (gas[1] != 8 || gas[2] != 0 || gas[3] > 1)
        return "reset register is not byte-accessible";
    acpi_reset_register_t r = {.space = gas[0], .value = p[FADT_RESET_VALUE]};
    memcpy(&r.address, gas + 4, sizeof(r.address));
    if (!r.address) return "reset register has no address";
    switch (r.space) {
    case 0:
        /* This kernel maps physical MMIO into the low canonical VA range. */
        if (r.address >= (1ULL << 47)) return "reset MMIO address not mappable";
        if (paging_map_mmio(r.address & ~4095ULL, 4096))
            return "reset MMIO mapping failed";
        break;
    case 1:
        if (r.address > 65535) return "reset I/O port out of range";
        break;
    case 2:
        /* ACPI GAS PCI encoding: device[47:32], function[31:16], offset[15:0].
         * Reset registers are restricted to segment zero, bus zero. */
        if ((r.address >> 32) > 31 || ((r.address >> 16) & 65535) > 7 ||
            (r.address & 65535) > 4095 || pci_diag_segment(0) != 0)
            return "reset PCI address out of range";
        break;
    default:
        return "unsupported reset address space";
    }
    *reset = r;
    return NULL;
}

bool acpi_reset_write(const acpi_reset_register_t *reset)
{
    if (!reset || !reset->address) return false;
    if (reset->space == 0) {
        *(volatile uint8_t *)reset->address = reset->value;
        __asm__ volatile ("mfence" ::: "memory");
        return true;
    }
    if (reset->space == 1) {
        reset_outb((uint16_t)reset->address, reset->value);
        return true;
    }
    if (reset->space == 2)
        return pci_cfg_write8(0, (uint8_t)(reset->address >> 32),
            (uint8_t)(reset->address >> 16), (uint16_t)reset->address, reset->value);
    return false;
}

bool acpi_reset_8042(void)
{
    /* Some modern controllers do not wire the legacy reset output. Never
     * wait indefinitely or send a command while the input buffer is busy. */
    for (unsigned i = 0; i < 100000; i++) {
        uint8_t status = reset_inb(0x64);
        if (status == 255) return false;
        if (!(status & 2)) { reset_outb(0x64, 0xfe); return true; }
        __asm__ volatile ("pause");
    }
    return false;
}
