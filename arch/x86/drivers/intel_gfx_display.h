#ifndef OSITO_INTEL_GFX_DISPLAY_H
#define OSITO_INTEL_GFX_DISPLAY_H
#include "../win32/d3dkmt_registry.h"
#include "../include/display_source.h"
/* The display owner may install this provider after acquiring/binding the
 * physical source. Merely obtaining these callbacks publishes no adapter. */
const D3DKMT_DISPLAY_PROVIDER *intel_gfx_display_provider(void);
/* Retain the verified current Intel GOP mode against backend/mode changes.
 * This grants neither a modeset nor a rendering capability. Release only
 * after KMT unregister drains all callbacks. */
NTSTATUS intel_gfx_display_acquire(display_source_owner_t *);
NTSTATUS intel_gfx_display_bind(OKMT_LUID,const display_source_owner_t *);
#endif
