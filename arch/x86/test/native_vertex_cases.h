#ifndef OSITOK_NATIVE_VERTEX_CASES_H
#define OSITOK_NATIVE_VERTEX_CASES_H
/* One constant attribute across a triangle. Two layouts exercise offsets
 * before/after POSITIONT, with the final read exactly at the stride boundary. */
typedef struct {
    unsigned type,bytes,data[4];
    float scale[4],bias[4];
} native_vertex_case_t;
#define VIDENT {1,1,1,1},{0,0,0,0}
#define VSIGNED {0.5f,0.5f,0.5f,0.5f},{0.5f,0.5f,0.5f,0.5f}
#define VSHORT {1.0f/65536,1.0f/65536,1.0f/65536,1.0f/65536},{0.5f,0.5f,0.5f,0.5f}
static const native_vertex_case_t native_vertex_cases[]={
    {0,4,{0x3e800000},VIDENT},
    {1,8,{0x3e800000,0x3f000000},VIDENT},
    {2,12,{0x3e800000,0x3f000000,0x3f400000},VIDENT},
    {3,16,{0x3e800000,0x3f000000,0x3f400000,0x3f800000},VIDENT},
    {4,4,{0x80c04020},VIDENT},
    {5,4,{0xffaa5511},{1.0f/255,1.0f/255,1.0f/255,1.0f/255},{0,0,0,0}},
    {6,4,{0x4000c000},VSHORT},
    {7,8,{0x2000c000,0x80006000},VSHORT},
    {8,4,{0xffaa5511},VIDENT},
    {9,4,{0x40008000},VSIGNED},
    {10,8,{0xc0008000,0x7fff0000},VSIGNED},
    {11,4,{0xc0004000},VIDENT},
    {12,8,{0x40000000,0xffffc000},VIDENT},
    {13,4,{1u|(511u<<10)|(1023u<<20)|(3u<<30)},{1.0f/1024,1.0f/1024,1.0f/1024,1},{0,0,0,0}},
    {14,4,{0x200u|(0x300u<<10)|(0x1ffu<<20)|(3u<<30)},VSIGNED},
    {15,4,{0x38003400},VIDENT},
    {16,8,{0x38003400,0x3c003a00},VIDENT},
};
#undef VIDENT
#undef VSIGNED
#undef VSHORT
/* Independent equivalent float4 attributes for HALs whose D3D9 declaration
 * caps omit some types. These are oracle inputs, never application vertices. */
static const float native_vertex_values[17][4]={
    {.25f,0,0,1},{.25f,.5f,0,1},{.25f,.5f,.75f,1},{.25f,.5f,.75f,1},
    {192.0f/255,64.0f/255,32.0f/255,128.0f/255},{17,85,170,255},
    {-16384,16384,0,1},{-16384,8192,24576,-32768},
    {17.0f/255,85.0f/255,170.0f/255,1},
    {-1,16384.0f/32767,0,1},{-1,-16384.0f/32767,0,1},
    {16384.0f/65535,49152.0f/65535,0,1},{0,16384.0f/65535,49152.0f/65535,1},
    {1,511,1023,1},{-1,-256.0f/511,1,1},{.25f,.5f,0,1},{.25f,.5f,.75f,1}
};
/* ps_3_0: dcl_texcoord0 v0; mul r0, v0, c0; add oC0, r0, c1.
 * Separate instructions obey D3D9's constant-register read-port limit. */
static const unsigned native_vertex_shader[]={0xffff0300,0x0200001f,0x80000005,0x900f0000,
    0x03000005,0x800f0000,0x90e40000,0xa0e40000,
    0x03000002,0x800f0800,0x80e40000,0xa0e40001,0x0000ffff};
#define NATIVE_VERTEX_CASE_COUNT (sizeof(native_vertex_cases)/sizeof(native_vertex_cases[0]))
#endif
