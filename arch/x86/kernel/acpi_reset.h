#ifndef OSITOK_ACPI_RESET_H
#define OSITOK_ACPI_RESET_H

#include "../include/types.h"

typedef struct {
    uint64_t address;
    uint8_t space, value;
} acpi_reset_register_t;

/* Prepare mappings before disabling interrupts. NULL means a valid target. */
const char *acpi_reset_prepare(const void *fadt, size_t available,
                               acpi_reset_register_t *reset);
bool acpi_reset_write(const acpi_reset_register_t *reset);
bool acpi_reset_8042(void);

#endif
