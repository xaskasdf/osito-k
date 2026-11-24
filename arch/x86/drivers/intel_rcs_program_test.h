/* Diagnostic file transport for authored compiler fixtures. Not a public KMT
 * command or an installed runtime. Only used by the opt-in physical test. */
#ifndef OSITOK_INTEL_RCS_PROGRAM_TEST_H
#define OSITOK_INTEL_RCS_PROGRAM_TEST_H
#include "intel_rcs_draw.h"
#define INTEL_RCS_PROGRAM_TEST_MAGIC 0x53504749u
#define INTEL_RCS_PROGRAM_TEST_MAX_CASES 256u
#define INTEL_RCS_PROGRAM_TEST_MAX_BYTES (4u*1024*1024)
typedef struct { uint32_t magic,version,cases,bytes; } intel_rcs_program_test_header_t;
/* Followed by exactly program_bytes bytes containing intel_d3d9_program. */
typedef struct { uint32_t program_bytes,expected_bgra,dispatch_mask,reserved; } intel_rcs_program_test_case_t;
/* Version 2 uses case.reserved as geometry_bytes. After the program comes this
 * header, tightly packed XYZW float vertices, then a 96x72 one-bit coverage
 * oracle (least-significant bit first). Oracle bytes never enter GPU state. */
#define INTEL_RCS_TEST_MASK_BYTES (96u*72/8)
#define INTEL_RCS_TEST_VERTEX_BYTES 8192u
typedef struct {
    uint32_t topology,vertices,x,y,width,height,vertex_bytes,mask_bytes,reserved[2];
} intel_rcs_program_geometry_t;
int intel_gfx_rcs_program_file(const char *path);
#endif
