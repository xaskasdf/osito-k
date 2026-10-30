#ifndef OSITOK_DISPLAY_IDENTITY_H
#define OSITOK_DISPLAY_IDENTITY_H

#include "types.h"

/* Identity of the device maintaining primary scanout, independent of render
 * capability or KMT source ownership. A zero vendor means PCI is unverified.
 * Description points to a kernel-owned static string. */
typedef struct {
    const char *description;
    uint16_t vendor, device;
    bool vga_compatible;
} display_identity_t;

void display_get_primary_identity(display_identity_t *out);

#endif
