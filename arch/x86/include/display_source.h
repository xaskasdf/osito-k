#ifndef OSITOK_DISPLAY_SOURCE_H
#define OSITOK_DISPLAY_SOURCE_H
#ifdef __KERNEL_X86__
#include "types.h"
#else
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#endif

/* Kernel-only lifetime of the primary scanout. Tokens are noncopyable and
 * must outlive their admission. No spinlock is held across hardware access,
 * allocation, or scheduling. Calls fail rather than waiting for another
 * transition or discarding an owner. pitch is in pixels. */
typedef struct {
    void *base;
    uint32_t width,height,pitch;
} display_source_mode_t;
typedef struct { const void *self; uint64_t serial; } display_source_change_t;
typedef struct { const void *self; uint64_t serial; } display_source_owner_t;

/* NULL requests a hardware/backend change. A mode requests a back-buffer-only
 * update, allowed under ownership only if the physical mode/base is unchanged.
 * Such an update must not program a different hardware backend. */
bool display_source_begin(display_source_change_t *,const display_source_mode_t *preserved);
bool display_source_end(display_source_change_t *);
/* Commit a verified hardware snapshot while the exclusive transition is held.
 * The caller has checked that it is the physical primary, not merely present. */
bool display_source_adopt(display_source_change_t *,display_source_owner_t *,const display_source_mode_t *);
bool display_source_mode(const display_source_owner_t *,display_source_mode_t *);
bool display_source_release(display_source_owner_t *);

/* Called only while holding an exclusive transition. Copies the current GOP
 * view if the GUI has not selected another backend or different geometry. */
bool display_get_retained_gop(display_source_mode_t *);
#endif
