/* Independent D3D9 render-state vectors shared by the Windows oracle and
 * native-driver diagnostic. These numbers are D3D enums, not GPU packets. */
typedef struct {
    unsigned enable,src,dst,op,separate,asrc,adst,aop,mask;
} native_blend_case_t;
static const native_blend_case_t native_blend_cases[]={
    {0,2,1,1,0,2,1,1,15}, {1,5,6,1,0,2,1,1,15},
    {1,2,6,1,0,2,1,1,15}, {1,2,2,1,0,2,1,1,15},
    {1,2,2,2,0,2,1,1,15}, {1,2,2,3,0,2,1,1,15},
    {1,5,6,4,0,2,1,1,15}, {1,5,6,5,0,2,1,1,15},
    {1,3,4,1,0,2,1,1,15}, {1,9,10,1,0,2,1,1,15},
    {1,7,8,1,0,2,1,1,15}, {1,11,2,1,0,2,1,1,15},
    {1,14,15,1,0,2,1,1,15}, {1,15,14,1,0,2,1,1,15},
    {1,5,6,1,1,2,1,1,15}, {1,5,6,1,1,1,2,1,15},
    {1,5,6,1,1,5,6,4,15}, {1,2,2,2,1,5,6,5,15},
    {1,5,6,1,0,2,1,1,1}, {1,5,6,1,0,2,1,1,2},
    {1,5,6,1,0,2,1,1,4}, {1,5,6,1,0,2,1,1,8},
    {1,5,6,1,0,2,1,1,3}, {1,5,6,1,0,2,1,1,0},
    {0,5,6,1,0,2,1,1,11}, {1,5,6,1,0,1,2,5,15},
    {1,6,5,1,0,2,1,1,15}, {1,5,6,1,1,11,1,1,15},
};
#define NATIVE_BLEND_CASE_COUNT (sizeof(native_blend_cases)/sizeof(native_blend_cases[0]))
#define NATIVE_BLEND_DESTINATION 0x80604020u
#define NATIVE_BLEND_FACTOR 0x402080c0u
static const unsigned native_blend_color_bits[]={0x3e800000,0x3f000000,0x3f400000,0x3f000000};
static const unsigned native_blend_bytecode[]={0xffff0200,0x02000001,0x800f0800,0xa0e40000,0x0000ffff};
