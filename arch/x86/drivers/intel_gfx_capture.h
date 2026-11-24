/* Opt-in physical qualification of captured KMT work over the native owner. */
#ifndef OSITOK_INTEL_GFX_CAPTURE_H
#define OSITOK_INTEL_GFX_CAPTURE_H
#include "intel_gpu_jobs.h"
bool intel_gfx_capture_available(void);
bool intel_gfx_capture_memory_init(intel_gpu_memory_t *);
NTSTATUS intel_gfx_capture_open(intel_gpu_jobs_t *);
NTSTATUS intel_gfx_capture_open_render(intel_gpu_jobs_t *);
NTSTATUS intel_gfx_capture_close(void);
bool intel_gfx_capture_clean(void);
#endif
