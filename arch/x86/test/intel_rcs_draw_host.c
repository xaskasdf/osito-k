#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_rcs_draw.h"
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %u: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint32_t allocation[2050],snapshot[2050];
static const intel_rcs_draw_t fixture={0x1000200000ULL,0x1000210000ULL,0x1000220000ULL,
    20480,29952,64,64,320,96,72,416,3,5,7,11,37,29};

static void draw_cases(void)
{
    uint32_t *b=allocation+1;
    memset(allocation,0xa5,sizeof(allocation));
    CHECK(intel_rcs_draw_build(b,8192,&fixture));
    CHECK(allocation[0]==0xa5a5a5a5 && allocation[2049]==0xa5a5a5a5);
    CHECK(b[0]==0x69040300);
    unsigned offset=1,primitive=0,base=0,ps=0,end=0;
    while (offset<1024) {
        uint32_t header=b[offset];
        if (header==0x05000000) { end=offset; break; }
        if (!header) { offset++; continue; }
        CHECK((header>>29)==3);
        unsigned length=(header&255)+2;
        CHECK(offset+length<=1024);
        switch (header&0xffff0000u) {
        case 0x61010000:
            base++; CHECK(length==18);
            CHECK(b[offset+4]==0x200001 && b[offset+5]==0x10);
            CHECK(b[offset+6]==b[offset+4] && b[offset+10]==b[offset+4]);
            CHECK(b[offset+13]==0x2001 && b[offset+15]==0x2001); break;
        case 0x78200000:
            ps++; CHECK(length==12 && b[offset+1]==0x1100);
            CHECK((b[offset+6]&3)==2 && b[offset+7]==(6<<16)); break;
        case 0x7b000000:
            primitive++; CHECK(length==7 && b[offset+2]==3 && b[offset+4]==1); break;
        default: break;
        }
        offset+=length;
    }
    CHECK(base==1 && ps==1 && primitive==1 && end>200 && end<512);
    for (unsigned i=end+1;i<1024;i++) CHECK(!b[i]);
    CHECK(b[1024]==0x1040 && b[1025]==0x1080);
    CHECK(b[0x1040/4+2]==(71u<<16|95) && b[0x1040/4+3]==415);
    CHECK(b[0x1080/4+2]==(63u<<16|63) && b[0x1080/4+3]==319);
    CHECK(b[0x1040/4+8]==0x220000 && b[0x1040/4+9]==0x10);
    CHECK(b[0x1080/4+8]==0x210000 && b[0x1080/4+9]==0x10);
    CHECK(b[0x1100/4+11]==0x08840001 && b[0x1100/4+15]==0x90031000);
    /* Independent host FPU oracle for all power-of-two texture coordinates. */
    for (unsigned extent=1;extent<=4096;extent*=2) for (unsigned x=0;x<extent;x++) {
        intel_rcs_draw_t d=fixture;
        d.source_width=d.source_height=extent; d.source_pitch=extent*4;
        d.source_bytes=(uint64_t)extent*extent*4;
        d.source_gpu=0x200000000ULL; d.destination_gpu=0x300000000ULL;
        d.source_x=x; d.source_y=0; d.width=d.height=1;
        CHECK(intel_rcs_draw_build(b,8192,&d));
        float f=(float)(x+1)/extent; uint32_t bits; memcpy(&bits,&f,4);
        CHECK(b[0x1180/4+1]==bits);
        f=(float)x/extent; memcpy(&bits,&f,4);
        CHECK(b[0x1180/4+4]==bits);
    }
    for (unsigned bad=0;bad<17;bad++) {
        intel_rcs_draw_t d=fixture;
        switch(bad) {
        case 0:d.batch_gpu=0;break;
        case 1:d.batch_gpu++;break;
        case 2:d.source_gpu=d.batch_gpu+4096;break;
        case 3:d.destination_gpu=d.source_gpu+4096;break;
        case 4:d.batch_gpu=(1ULL<<47)-4096;break;
        case 5:d.source_bytes--;break;
        case 6:d.destination_bytes--;break;
        case 7:d.source_width=0;break;
        case 8:d.destination_height=16385;break;
        case 9:d.source_pitch=255;break;
        case 10:d.destination_pitch=262148;break;
        case 11:d.width=0;break;
        case 12:d.height=UINT32_MAX;break;
        case 13:d.source_x=UINT32_MAX;break;
        case 14:d.source_y=40;break;
        case 15:d.destination_x=70;break;
        case 16:d.destination_y=50;break;
        }
        memcpy(snapshot,allocation,sizeof(snapshot));
        CHECK(!intel_rcs_draw_build(b,8192,&d));
        CHECK(!memcmp(snapshot,allocation,sizeof(snapshot)));
    }
    CHECK(!intel_rcs_draw_build(b,8191,&fixture));
    CHECK(!intel_rcs_draw_build((uint32_t *)((char *)b+1),8192,&fixture));
}

static void batch_cases(void)
{
    uint32_t *context=calloc(1,INTEL_RCS_CONTEXT_BYTES),*ring=calloc(1,4096);
    CHECK(context && ring);
    uint64_t descriptor;
    CHECK(intel_rcs_build(context,INTEL_RCS_CONTEXT_BYTES,ring,4096,
        0x1000000,0x1016000,0x234000,0x2001,123,&descriptor));
    uint32_t tail=64,next;
    for (unsigned round=0;round<96;round++) {
        context[1029]=context[1031]=tail;
        context[INTEL_RCS_CONTEXT_BYTES/4-1]=0xabcdef;
        CHECK(intel_rcs_append_batch(context,INTEL_RCS_CONTEXT_BYTES,ring,4096,
            tail,fixture.batch_gpu,round+124,&next));
        CHECK(ring[tail/4]==0x18800101 && ring[(tail/4+2)&1023]==0x10);
        CHECK(ring[(tail/4+4)&1023]==0x7a000004 && ring[(tail/4+10)&1023]==0x7a000004);
        CHECK(ring[(tail/4+14)&1023]==round+124 && next==(tail+128)%4096);
        CHECK(context[INTEL_RCS_CONTEXT_BYTES/4-1]==0xabcdef);
        tail=next;
    }
    context[1029]=context[1031]=tail;
    uint32_t before=ring[tail/4];next=999;
    CHECK(!intel_rcs_append_batch(context,INTEL_RCS_CONTEXT_BYTES,ring,4096,tail,3,1,&next));
    CHECK(next==999 && context[1031]==tail && ring[tail/4]==before);
    free(context);free(ring);
}
static struct { uint32_t regs[3],ops,fail; } model;
static bool cache_read(void *ctx,uint32_t reg,uint32_t *value)
{
    (void)ctx; unsigned i=reg==0xc800?0:reg==0xc804?1:2;
    CHECK(reg==0xc800 || reg==0xc804 || reg==0xb020);
    *value=model.regs[i]; return ++model.ops!=model.fail;
}
static bool cache_write(void *ctx,uint32_t reg,uint32_t value)
{
    (void)ctx; unsigned i=reg==0xc800?0:reg==0xc804?1:2;
    CHECK(reg==0xc800 || reg==0xc804 || reg==0xb020);
    model.regs[i]=value; return ++model.ops!=model.fail; /* applied but failed */
}
static void cache_cases(void)
{
    const intel_gt_io_t io={cache_read,cache_write,0,0};
    const uint32_t original[]={0x38,0x9,0x00300030};
    for (unsigned failure=0;failure<=15;failure++) {
        memset(&model,0,sizeof(model)); memcpy(model.regs,original,sizeof(original));
        model.fail=failure; intel_rcs_cache_t s={0};
        bool acquired=intel_rcs_cache_acquire(&io,NULL,&s);
        CHECK(acquired==(failure==0 || failure>9));
        if (acquired) CHECK(s.ready && s.changed==7 && model.regs[2]==0x00100010);
        bool restored=intel_rcs_cache_restore(&io,NULL,&s);
        if (!restored) {
            CHECK(s.changed!=0 && !s.ready); model.fail=0;
            CHECK(intel_rcs_cache_restore(&io,NULL,&s));
        }
        CHECK(!s.changed && !memcmp(model.regs,original,sizeof(original)));
    }
}
int main(void)
{
    draw_cases();batch_cases();cache_cases();
    printf("Intel RCS draw: %u checks passed\n",checks);
    return 0;
}
