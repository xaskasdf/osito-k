/* Walk the production builder's complete hierarchy, including level edges. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_ppgtt.h"
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr,"FAIL PPGTT %d: %s\n",__LINE__,#x); abort(); \
} } while (0)
static uint64_t storage[32*512+2], saved[32*512+2];
static const uint64_t dma = 0x123450000ULL;
static struct { uint32_t value, ops, fail, writes; } pat;
static bool pat_read(void *ctx, uint32_t reg, uint32_t *value)
{
    CHECK(ctx==&pat && reg==0x40e0);
    if (++pat.ops==pat.fail) return false;
    *value=pat.value; return true;
}
static bool pat_write(void *ctx, uint32_t reg, uint32_t value)
{
    CHECK(ctx==&pat && reg==0x40e0);
    pat.writes++; pat.value=value;
    return ++pat.ops!=pat.fail; /* write may apply even on an error return */
}
static uint64_t walk(uint64_t gpu, uint32_t used)
{
    uint64_t *table = storage+1;
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t pte = table[(gpu >> shift) & 511];
        if (!(pte & 1)) return 0;
        if (shift == 12) return pte;
        CHECK((pte & 4095) == 3 && (pte & ~4095ULL) >= dma);
        uint64_t offset = (pte & ~4095ULL) - dma;
        CHECK(offset < (uint64_t)used * 4096);
        table = storage+1+offset/8;
    }
    abort();
}
int main(void)
{
    const uint64_t va[] = {4096,0x1ff000,0x200000,0x3ffff000,0x40000000,
        0xfffff000,0x100000000ULL,0x7ffffff000ULL,0x8000000000ULL,
        INTEL_PPGTT_VA_LIMIT-4096};
    intel_ppgtt_page_t pages[10];
    for (unsigned i=0;i<10;i++) pages[i]=(intel_ppgtt_page_t){va[i],0x4000000+4096*i,(i&1)!=0};
    memset(storage,0xa7,sizeof(storage)); uint32_t used=99;
    CHECK(intel_ppgtt_build(storage+1,32*4096,dma,pages,10,&used));
    CHECK(used==20);
    CHECK(storage[0]==0xa7a7a7a7a7a7a7a7ULL && storage[32*512+1]==storage[0]);
    for (unsigned i=0;i<10;i++) {
        CHECK(walk(va[i],used)==(pages[i].dma|0x19|(pages[i].writable?2:0)));
        if (!i || va[i]-va[i-1]>4096) CHECK(!walk(va[i]-4096,used));
        if (i==9 || va[i+1]-va[i]>4096) CHECK(!walk(va[i]+4096,used));
    }
    for (unsigned i=used*512+1;i<32*512+1;i++) CHECK(!storage[i]);
    memcpy(saved,storage,sizeof(storage));
    uint32_t old_used=used;
    CHECK(!intel_ppgtt_build(storage+1,(used-1)*4096,dma,pages,10,&used));
    CHECK(used==old_used && !memcmp(saved,storage,sizeof(storage)));
    const uint64_t bad_va[]={0,1,INTEL_PPGTT_VA_LIMIT,UINT64_MAX};
    for (unsigned i=0;i<4;i++) {
        uint64_t old=pages[0].gpu; pages[0].gpu=bad_va[i];
        CHECK(!intel_ppgtt_build(storage+1,32*4096,dma,pages,10,&used)); pages[0].gpu=old;
    }
    const uint64_t bad_pa[]={0,1,dma,dma+31*4096,INTEL_GGTT_PHYS_LIMIT,UINT64_MAX};
    for (unsigned i=0;i<6;i++) {
        uint64_t old=pages[0].dma; pages[0].dma=bad_pa[i];
        CHECK(!intel_ppgtt_build(storage+1,32*4096,dma,pages,10,&used)); pages[0].dma=old;
    }
    pages[1].gpu=pages[0].gpu;
    CHECK(!intel_ppgtt_build(storage+1,32*4096,dma,pages,10,&used)); pages[1].gpu=va[1];
    CHECK(!intel_ppgtt_build(storage+1,32*4096,dma,(void *)(storage+2),10,&used));
    CHECK(!intel_ppgtt_build(storage+1,32*4096,dma,pages,10,(void *)(storage+5)));
    CHECK(!intel_ppgtt_build(storage+1,32*4096,dma,pages,10,(void *)pages));
    CHECK(!intel_ppgtt_build(storage+1,32*4096,INTEL_GGTT_PHYS_LIMIT-4096,pages,10,&used));
    CHECK(!memcmp(saved,storage,sizeof(storage)) && used==old_used);
    CHECK(intel_ppgtt_build(storage+1,4096,dma,NULL,0,&used) && used==1);
    for (unsigned i=1;i<=512;i++) CHECK(!storage[i]);
    /* A contiguous range crosses several PTs; readonly and writable leaves
     * share intermediate tables, but never inherit a sibling's permission. */
    static intel_ppgtt_page_t many[2048];
    for (unsigned i=0;i<2048;i++) many[i]=(intel_ppgtt_page_t){0x1ff000+4096ULL*i,0x9000000+4096ULL*i,(i&1)!=0};
    CHECK(intel_ppgtt_build(storage+1,32*4096,dma,many,2048,&used) && used==8);
    for (unsigned i=0;i<2048;i++) CHECK(walk(many[i].gpu,used)==(many[i].dma|0x19|(many[i].writable?2:0)));
    const intel_gt_io_t io={.read=pat_read,.write=pat_write};
    intel_ppgtt_pat_t state={0}; pat.value=0x03030303;
    CHECK(intel_ppgtt_pat_acquire(&io,&pat,&state) && state.ready && state.changed);
    CHECK(pat.value==0x00030303 && pat.writes==1);
    CHECK(!intel_ppgtt_pat_acquire(&io,&pat,&state) && pat.writes==1);
    pat.value=0x00010203; /* preserve changes to other slots on restoration */
    CHECK(intel_ppgtt_pat_restore(&io,&pat,&state) && pat.value==0x03010203);
    CHECK(!state.changed && !state.ready);
    for(unsigned fail=1;fail<=3;fail++) {
        memset(&state,0,sizeof(state)); memset(&pat,0,sizeof(pat));
        pat.value=0x03030303; pat.fail=fail;
        CHECK(!intel_ppgtt_pat_acquire(&io,&pat,&state) && !state.ready);
        CHECK(intel_ppgtt_pat_restore(&io,&pat,&state) && pat.value==0x03030303 && !state.changed);
    }
    for(unsigned fail=1;fail<=3;fail++) {
        memset(&state,0,sizeof(state)); memset(&pat,0,sizeof(pat)); pat.value=0x03030303;
        CHECK(intel_ppgtt_pat_acquire(&io,&pat,&state)); pat.fail=pat.ops+fail;
        CHECK(!intel_ppgtt_pat_restore(&io,&pat,&state) && state.changed && !state.ready);
        CHECK(intel_ppgtt_pat_restore(&io,&pat,&state) && !state.changed && pat.value==0x03030303);
    }
    memset(&state,0,sizeof(state)); memset(&pat,0,sizeof(pat)); pat.value=0x03030303;
    CHECK(intel_ppgtt_pat_acquire(&io,&pat,&state)); pat.value=0x02030303;
    CHECK(!intel_ppgtt_pat_restore(&io,&pat,&state) && state.changed && pat.writes==1);
    memset(&state,0,sizeof(state)); memset(&pat,0,sizeof(pat)); pat.value=0x00030303;
    CHECK(intel_ppgtt_pat_acquire(&io,&pat,&state) && !state.changed && !pat.writes);
    CHECK(intel_ppgtt_pat_restore(&io,&pat,&state) && !pat.writes);
    printf("Intel PPGTT: %u checks passed\n",checks);
    return 0;
}
