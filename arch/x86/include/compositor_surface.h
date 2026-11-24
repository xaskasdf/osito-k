#ifndef OSITOK_COMPOSITOR_SURFACE_H
#define OSITOK_COMPOSITOR_SURFACE_H

#include "types.h"

/* One owned SHM reference. Pitch is in pixels; the allocation stays alive
 * until release even if its window is replaced/destroyed. Pixel contents
 * remain writable. A client view may offset pixels and reduce width/height. */
typedef struct {
    uint32_t handle;
    uint32_t *pixels;
    uint32_t width, height, pitch;
} compositor_surface_ref_t;

bool compositor_acquire_surface(uint32_t window_id, compositor_surface_ref_t *surface);
void compositor_release_surface(compositor_surface_ref_t *surface);

#endif
