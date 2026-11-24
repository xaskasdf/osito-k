#ifndef OSITO_DRIVER_FIRMWARE_H
#define OSITO_DRIVER_FIRMWARE_H
#include "../include/types.h"
typedef struct {
    const char *file, *host, *path, *sha256;
    unsigned max_size;
} driver_firmware_t;
/* Allocates only after FS/network prerequisites; caller frees successful data.
 * Returns 0 ready, 1 missing/offline, -1 invalid/I/O. Existing invalid firmware
 * is preserved for diagnosis, never executed or silently overwritten. */
int driver_firmware_get(const driver_firmware_t *, void **, unsigned *, const char **);
extern const driver_firmware_t driver_ax200_firmware, driver_ax200_license;
#endif
