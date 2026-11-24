#ifndef OSITOK_MSI_CONDITION_H
#define OSITOK_MSI_CONDITION_H
#include "../include/types.h"

typedef struct {
    const uint16_t *text; /* Borrowed for the duration of the evaluation. */
    uint32_t length;
    int64_t integer;
    int is_integer;
} msi_condition_value_t;

/* prefix is zero for properties, or %, $, ?, &, ! for MSI access prefixes.
 * An unresolved symbol is an empty string. Return zero on lookup failure
 * (such as an unavailable environment or component-state provider). */
typedef int (*msi_condition_lookup_t)(void *context, uint16_t prefix,
    const uint16_t *name, uint32_t length, msi_condition_value_t *value);

/* MSICONDITION values: false=0, true=1, empty=2, invalid=3.
 * Evaluation is bounded to 128 nested expressions; no heap allocation. */
int msi_condition_evaluate(const uint16_t *expression,
                          msi_condition_lookup_t lookup, void *context);
#endif
