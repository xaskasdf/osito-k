/* Opt-in native VS diagnostic, not a public submission format. */
#ifndef OSITOK_INTEL_RCS_VERTEX_TEST_H
#define OSITOK_INTEL_RCS_VERTEX_TEST_H
#include "intel_rcs_sampled_test.h"
#include "intel_rcs_vertex.h"
/* Version 4: case.reserved covers this structure followed by vertex_bytes
 * bytes containing an ABI4 program. Both programs and all DMA inputs are
 * validated before engine admission. The pixel oracle stays CPU-only. */
typedef struct {
    intel_rcs_sampled_test_t sampled;
    intel_rcs_vertex_binding_t vertex;
    uint32_t vertex_bytes,reserved;
} intel_rcs_vertex_test_t;
bool intel_rcs_vertex_test_validate(const struct intel_d3d9_program *,size_t,
    const void *,size_t,uint32_t);
/* Version 5: vertices and indices occupy disjoint 4 KiB views of the owned
 * 8 KiB diagnostic allocation. Original index bytes live in its second half.
 * Optional ABI4 program follows this header (zero bytes selects POSITIONT). */
typedef struct {
    intel_rcs_sampled_test_t sampled;
    intel_rcs_vertex_binding_t vertex;
    intel_rcs_index_binding_t indices;
    uint32_t vertex_bytes,reserved;
} intel_rcs_indexed_test_t;
bool intel_rcs_indexed_test_validate(const struct intel_d3d9_program *,size_t,
    const void *,size_t,uint32_t);
#endif
