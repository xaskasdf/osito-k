#ifndef OSITOK_MSI_PAYLOAD_H
#define OSITOK_MSI_PAYLOAD_H
#include "msi_plan.h"

typedef struct {
    uint8_t *bytes;
    uint32_t size;
    uint16_t date, time, attributes;
} msi_payload_file_t;
typedef struct {
    msi_payload_file_t *files; /* Same order as plan->files. */
    uint32_t count;
} msi_payload_t;
/* Read a cabinet from the package source directory. Return a Win32 error or
 * zero and a kmalloc-owned buffer. The callee never receives a target path. */
typedef uint32_t (*msi_payload_source_t)(void *context, const char *cabinet,
    uint8_t **bytes, uint32_t *size);

/* Prepare and validate every selected payload before the first install write.
 * Cabinets are limited to 128 MiB; selected file data to 256 MiB. */
msi_payload_t *msi_payload_prepare(msi_plan_t *plan, msi_payload_source_t source,
                                 void *context, uint32_t *error);
void msi_payload_close(msi_payload_t *payload);
/* Stage the unchanged MSI in the same file journal. Copies bytes and target,
 * then extends both arrays atomically; failure leaves both objects untouched.
 * The extra file has no authored component (component == UINT32_MAX). */
uint32_t msi_payload_stage_cache(msi_plan_t *plan, msi_payload_t *payload,
    const uint16_t *target, const uint8_t *data, uint32_t size);
#endif
