/* Device lifetime for the native KMT backend; independent of PE callers. */
#ifndef OSITOK_INTEL_GFX_DRIVER_H
#define OSITOK_INTEL_GFX_DRIVER_H
#include "../win32/d3dkmt_registry.h"
enum { INTEL_GFX_DRIVER_OFF,INTEL_GFX_DRIVER_STARTING,INTEL_GFX_DRIVER_ON,INTEL_GFX_DRIVER_CLOSING };
typedef struct {
    unsigned phase;
    BOOL adapter_registered,engine_owned;
    DWORD completed,failed,last_pid,node;
    NTSTATUS last_result;
    OKMT_LUID luid;
} intel_gfx_driver_snapshot_t;
/* Serialized, nonblocking BSP control with IRQs enabled. The execution backend
 * exposes no rendering capability or UMD path. A verified current physical
 * display may be retained/bound on the same adapter; other configurations
 * remain unbound prototypes. The driver is opt-in
 * until full runtime DDIs, presentation and recovery are implemented.
 * start selects BCS, start_render selects RCS textured copies. Engine choice
 * is fixed until stop completes; this is not simultaneous multi-engine use.
 * Stop withdraws new admission, retains active-client/DMA ownership, and may
 * return BUSY. Retry; no timeout authorizes discarding the device storage. */
NTSTATUS intel_gfx_driver_start(void);
NTSTATUS intel_gfx_driver_start_render(void);
NTSTATUS intel_gfx_driver_stop(void);
NTSTATUS intel_gfx_driver_snapshot(intel_gfx_driver_snapshot_t *);
#endif
