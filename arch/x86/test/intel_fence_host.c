#define main ppgtt_suite_main
#include "intel_ppgtt_host.c"
#undef main
#include "../drivers/intel_bcs.h"
static uint32_t coherent_reg,coherent_ops,coherent_fail;
static bool coherent_read(void *ctx,uint32_t reg,uint32_t *value)
{
    CHECK(!ctx && reg==0x40e4);
    if (++coherent_ops==coherent_fail) return false;
    *value=coherent_reg; return true;
}
static bool coherent_write(void *ctx,uint32_t reg,uint32_t value)
{
    CHECK(!ctx && reg==0x40e4); coherent_reg=value;
    return ++coherent_ops!=coherent_fail;
}
int main(void)
{
    CHECK(!ppgtt_suite_main());
    intel_ppgtt_range_t ranges[2]={{0x100000000ULL,0x500000,8192,false},{0x200000000ULL,0x600000,4096,true}};
    uint32_t used=0;
    CHECK(intel_ppgtt_build_ranges_coherent(storage+1,32*4096,dma,ranges,2,2,&used));
    CHECK(walk(ranges[0].gpu,used)==0x500019 && walk(ranges[0].gpu+4096,used)==0x501019);
    CHECK(walk(ranges[1].gpu,used)==0x600083);
    memcpy(saved,storage,sizeof(storage));
    CHECK(!intel_ppgtt_build_ranges_coherent(storage+1,32*4096,dma,ranges,2,4,&used));
    ranges[1].dma=0x501000;
    CHECK(!intel_ppgtt_build_ranges_coherent(storage+1,32*4096,dma,ranges,2,2,&used));
    CHECK(!memcmp(saved,storage,sizeof(storage)));
    CHECK(intel_ppgtt_build_ranges_coherent(storage+1,32*4096,dma,ranges,2,3,&used));
    CHECK(walk(ranges[0].gpu,used)==0x500081 && walk(ranges[1].gpu,used)==0x501083);
    intel_gt_io_t io={.read=coherent_read,.write=coherent_write};
    for (unsigned fail=0;fail<=6;fail++) {
        intel_ppgtt_pat_t state={0};coherent_reg=0xa1b2c303;coherent_ops=0;coherent_fail=fail;
        bool acquired=intel_ppgtt_coherent_pat_acquire(&io,NULL,&state);
        CHECK(acquired==(fail==0 || fail>3));
        CHECK(!acquired || (state.ready && coherent_reg==0xa1b2c307));
        bool restored=intel_ppgtt_coherent_pat_restore(&io,NULL,&state);
        CHECK(restored==(fail<4 || fail==0));
        coherent_fail=0;
        CHECK(intel_ppgtt_coherent_pat_restore(&io,NULL,&state));
        CHECK(!state.changed && !state.ready && coherent_reg==0xa1b2c303);
    }
    intel_ppgtt_pat_t state={0};coherent_reg=0xa1b2c303;coherent_ops=coherent_fail=0;
    CHECK(intel_ppgtt_coherent_pat_acquire(&io,NULL,&state));
    coherent_reg=0x12345605;
    CHECK(!intel_ppgtt_coherent_pat_restore(&io,NULL,&state) && coherent_reg==0x12345605);
    coherent_reg=0x12345607;
    CHECK(intel_ppgtt_coherent_pat_restore(&io,NULL,&state) && coherent_reg==0x12345603);
    uint32_t context[2048],ring[1024];uint64_t descriptor=0;
    CHECK(intel_bcs_build_fence(context,sizeof(context),ring,sizeof(ring),0x100000,0x102000,dma,7,11,
        0x400000080ULL,0xfedcba9876543210ULL,&descriptor));
    CHECK(context[1031]==96 && ring[0]==0x13244003 && !ring[3] && !ring[4]);
    CHECK(ring[8]==0x10200003 && ring[9]==0x80 && ring[10]==4 && ring[11]==0x76543210 && ring[12]==0xfedcba98);
    CHECK(ring[16]==0x13244003 && ring[19]==11 && !ring[20]);
    for (unsigned i=0;i<8;i++) {
        uint32_t before=ring[0];uint64_t d=descriptor;
        CHECK(!intel_bcs_build_fence(context,sizeof(context),ring,sizeof(ring),0x100000,0x102000,dma,7,11,
            i ? 0x400000080ULL+i : INTEL_PPGTT_VA_LIMIT,1,&descriptor));
        CHECK(ring[0]==before && descriptor==d);
    }
    printf("Intel coherent fence mappings: %u checks passed\n",checks);
    return 0;
}
