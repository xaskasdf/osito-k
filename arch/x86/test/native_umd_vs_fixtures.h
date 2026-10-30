#ifndef OSITOK_NATIVE_UMD_VS_FIXTURES_H
#define OSITOK_NATIVE_UMD_VS_FIXTURES_H
/* Original diagnostic shaders: POSITION v0 -> m4x4(c0..c3) -> position;
 * TEXCOORD0 v15 -> texcoord output. VS3 deliberately uses output register 7,
 * independently of the input register and declaration element order. */
static const unsigned vs2_code[]={
    0xfffe0200,0x0200001f,0x80000000,0x900f0000,
    0x0200001f,0x80000005,0x900f000f,
    0x03000014,0xc00f0000,0x90e40000,0xa0e40000,
    0x02000001,0xe00f0000,0x90e4000f,0x0000ffff};
static const unsigned vs3_code[]={
    0xfffe0300,0x0200001f,0x80000000,0x900f0000,
    0x0200001f,0x80000005,0x900f000f,
    0x0200001f,0x80000000,0xe00f0000,
    0x0200001f,0x80000005,0xe00f0007,
    0x03000014,0xe00f0000,0x90e40000,0xa0e40000,
    0x02000001,0xe00f0007,0x90e4000f,0x0000ffff};
#endif
