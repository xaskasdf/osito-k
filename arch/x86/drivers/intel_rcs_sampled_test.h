/* Opt-in physical diagnostic transport, never an application submission ABI. */
#ifndef OSITOK_INTEL_RCS_SAMPLED_TEST_H
#define OSITOK_INTEL_RCS_SAMPLED_TEST_H
#include "intel_rcs_sampled.h"
#define INTEL_RCS_SAMPLED_TEST_SOURCE_BYTES 20480u
#define INTEL_RCS_SAMPLED_TEST_VERTEX_BYTES 8192u
#define INTEL_RCS_SAMPLED_TEST_TARGET_BYTES 32768u
/* Version 3 of intel_rcs_program_test_header_t uses case.reserved=sizeof(this).
 * Source/vertices are uploaded verbatim. Expected pixels are an offline test
 * oracle and are never sent to the GPU. All GPU addresses below are fixed by
 * the isolated diagnostic mapping, not taken on trust from the file. */
typedef struct {
    intel_rcs_sampled_draw_t draw;
    uint32_t source[INTEL_RCS_SAMPLED_TEST_SOURCE_BYTES/4];
    uint32_t vertices[INTEL_RCS_SAMPLED_TEST_VERTEX_BYTES/4];
    uint32_t expected[INTEL_RCS_SAMPLED_TEST_TARGET_BYTES/4];
} intel_rcs_sampled_test_t;
bool intel_rcs_sampled_test_validate(const struct intel_d3d9_program *,size_t,
    const void *,size_t,uint32_t dispatch_mask);
#endif
