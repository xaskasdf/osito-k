#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_rcs_draw.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %u: %s\n",__LINE__,#x); exit(1); } } while (0)
static uint32_t allocation[2051],before[2051],blob[144];
static const intel_rcs_primitive_draw_t fixture={
    {0x1000200000ULL,0x1000210000ULL,0x1000220000ULL,32768,96,72,416,7,11,37,29,3,{0}},
    0x1000300000ULL,1u<<28,20,32,6,INTEL_RCS_TRIANGLE_LIST,{0}};

static void packet(const uint32_t *b,const intel_rcs_primitive_draw_t *d)
{
    unsigned found=0,end=0;
    for (unsigned at=1;at<1024;) {
        unsigned op=b[at],len=(op&255)+2;
        if (op==0x05000000) {end=at;break;}
        if (!op) {at++;continue;}
        CHECK((op>>29)==3 && at+len<=1024);
        switch (op&0xffff0000u) {
        case 0x780d0000:CHECK(len==2 && b[at+1]==16);found|=1;break;
        case 0x78500000:CHECK(b[at+1]==((1<<21)|(1<<16)|2));found|=2;break;
        case 0x78080000:
            CHECK(len==5 && b[at+1]==((1<<14)|d->vertex_stride));
            CHECK(((uint64_t)b[at+3]<<32|b[at+2])==d->vertices_gpu+d->vertex_offset);
            CHECK(b[at+4]==(d->vertex_count-1)*d->vertex_stride+16);found|=4;break;
        case 0x78090000:
            CHECK(len==7 && b[at+1]==1<<25 && b[at+2]==0x22220000);
            CHECK(b[at+3]==1<<25 && b[at+4]==0x11110000);
            CHECK(b[at+5]==1<<25 && b[at+6]==0x22220000);found|=8;break;
        case 0x784b0000:CHECK(len==2 && b[at+1]==d->topology);found|=16;break;
        case 0x7b000000:
            CHECK(len==7 && b[at+1]==0 && b[at+2]==d->vertex_count && !b[at+3]);
            CHECK(b[at+4]==1 && !b[at+5] && !b[at+6]);found|=32;break;
        default:break;
        }
        at+=len;
    }
    CHECK(found==63 && end);
    CHECK(b[0x1340/4]==(d->pixel.x|(d->pixel.y<<16)));
    CHECK(b[0x1344/4]==((d->pixel.x+d->pixel.width-1)|((d->pixel.y+d->pixel.height-1)<<16)));
}

static void scissors_cases(const struct intel_d3d9_program *p)
{
    uint32_t *b=allocation+2;
    intel_rcs_scissor_t rects[32];
    for (unsigned count=1;count<=32;count++) {
        for (unsigned i=0;i<count;i++)rects[i]=(intel_rcs_scissor_t){7+i,11,1,29};
        memset(allocation,0xa5,sizeof(allocation));
        CHECK(intel_rcs_primitives_build(b,8192,&fixture,p,sizeof(blob),rects,count));
        unsigned draws=0,scissors=0,base=0,end=0;
        for (unsigned at=1;at<1024;) {
            unsigned op=b[at],len=(op&255)+2;
            if (op==0x05000000) { end=at;break; }
            if (!op) { at++;continue; }
            CHECK((op>>29)==3 && at+len<=1024);
            if ((op&0xffff0000u)==0x61010000)base++;
            if ((op&0xffff0000u)==0x780f0000) {
                unsigned offset=b[at+1];
                if (offset!=0x1340) {
                    CHECK(scissors<count && offset==0x1400+scissors*32 && !(offset&31));
                    const intel_rcs_scissor_t *r=&rects[scissors++];
                    CHECK(b[offset/4]==(r->x|(r->y<<16)));
                    CHECK(b[offset/4+1]==((r->x+r->width-1)|((r->y+r->height-1)<<16)));
                    for (unsigned j=2;j<8;j++)CHECK(b[offset/4+j]==0);
                }
            }
            if ((op&0xffff0000u)==0x7b000000) { CHECK(scissors==++draws); }
            at+=len;
        }
        CHECK(base==1 && draws==count && scissors==count && end && end<1024);
        CHECK(allocation[0]==0xa5a5a5a5 && allocation[1]==0xa5a5a5a5 && allocation[2050]==0xa5a5a5a5);
    }
    for (unsigned invalid=0;invalid<8;invalid++) {
        unsigned count=2;rects[0]=rects[1]=(intel_rcs_scissor_t){7,11,37,29};
        switch(invalid) {
        case 0:count=33;break;case 1:rects[1].width=0;break;
        case 2:rects[1].height=UINT32_MAX;break;case 3:rects[1].x=UINT32_MAX;break;
        case 4:rects[1].y=UINT32_MAX;break;case 5:rects[1].x=6;break;
        case 6:rects[1].y=10;break;case 7:rects[1].width=38;break;
        }
        memcpy(before,allocation,sizeof(before));
        CHECK(!intel_rcs_primitives_build(b,8192,&fixture,p,sizeof(blob),rects,count));
        CHECK(!memcmp(before,allocation,sizeof(before)));
    }
    CHECK(!intel_rcs_primitives_build(b,8192,&fixture,p,sizeof(blob),NULL,1));
    rects[0]=(intel_rcs_scissor_t){7,11,1,1};memcpy(b,rects,sizeof(rects[0]));
    CHECK(intel_rcs_primitives_build(b,8192,&fixture,p,sizeof(blob),(const void *)b,1));
    CHECK(b[0x1400/4]==(7|(11<<16)) && b[0x1404/4]==(7|(11<<16)));
}

int main(void)
{
    struct intel_d3d9_program *p=(void *)blob;
    *p=(struct intel_d3d9_program){.abi=1,.header_bytes=128,.allocation_bytes=sizeof(blob),.pci_id=0x3e9b,
        .bytecode_offset=128,.bytecode_bytes=4,.code_offset=192,.code_bytes=384,.stage=1,.simd_mask=3,
        .grf_used=32,.kernel_offset={0,128},.dispatch_grf_start={2,4},.varying_inputs=1,.layer_input=0};
    uint32_t *b=allocation+2;
    for (unsigned topo=4;topo<=6;topo++) for (unsigned count=3;count<67;count++) {
        if (topo==4 && count%3) continue;
        for (unsigned stride=16;stride<=2048;stride*=2) {
            intel_rcs_primitive_draw_t d=fixture;
            d.topology=topo;d.vertex_count=count;d.vertex_stride=stride;
            memset(allocation,0xa5,sizeof(allocation));
            CHECK(intel_rcs_primitive_build(b,8192,&d,p,sizeof(blob)));
            CHECK(allocation[0]==0xa5a5a5a5 && allocation[1]==0xa5a5a5a5 && allocation[2050]==0xa5a5a5a5);
            packet(b,&d);
        }
    }
    for (unsigned bad=0;bad<26;bad++) {
        intel_rcs_primitive_draw_t d=fixture;
        switch(bad) {
        case 0:d.vertices_gpu=0;break;case 1:d.vertices_gpu++;break;
        case 2:d.vertices_bytes=0;break;case 3:d.vertices_bytes=(1u<<28)+1;break;
        case 4:d.vertices_gpu=(1ULL<<47)-4096;break;case 5:d.vertex_stride=12;break;
        case 6:d.vertex_stride=17;break;case 7:d.vertex_stride=2052;break;
        case 8:d.vertex_offset=1;break;case 9:d.vertex_count=0;break;
        case 10:d.vertex_count=2;break;case 11:d.vertex_count=65536;break;
        case 12:d.vertex_count=4;break;case 13:d.topology=3;break;case 14:d.topology=7;break;
        case 15:d.vertex_offset=UINT32_MAX-3;break;case 16:d.vertices_bytes=195;break;
        case 17:d.vertices_gpu=d.pixel.batch_gpu;break;case 18:d.vertices_gpu=d.pixel.program_gpu;break;
        case 19:d.vertices_gpu=d.pixel.destination_gpu;break;
        case 20:d.vertices_gpu=d.pixel.batch_gpu-4096;break;
        case 21:d.pixel.width=0;break;case 22:d.pixel.x=UINT32_MAX;break;
        case 23:d.pixel.dispatch_mask=4;break;case 24:d.pixel.program_gpu=d.pixel.batch_gpu;break;
        case 25:d.pixel.destination_bytes=29951;break;
        }
        memcpy(before,allocation,sizeof(before));
        CHECK(!intel_rcs_primitive_build(b,8192,&d,p,sizeof(blob)));
        CHECK(!memcmp(before,allocation,sizeof(before)));
    }
    CHECK(!intel_rcs_primitive_build(NULL,8192,&fixture,p,sizeof(blob)));
    CHECK(!intel_rcs_primitive_build(b,8191,&fixture,p,sizeof(blob)));
    CHECK(!intel_rcs_primitive_build(b,8192,NULL,p,sizeof(blob)));
    CHECK(!intel_rcs_primitive_build(b,8192,(void *)((char *)&fixture+1),p,sizeof(blob)));
    CHECK(!intel_rcs_primitive_build(b,8192,&fixture,NULL,sizeof(blob)));
    intel_rcs_primitive_draw_t exact=fixture;exact.vertices_bytes=196;
    CHECK(intel_rcs_primitive_build(b,8192,&exact,p,sizeof(blob)));
    /* Snapshot both descriptors before touching an aliased output buffer. */
    memcpy(b,p,sizeof(blob));memcpy(b+512,&fixture,sizeof(fixture));
    CHECK(intel_rcs_primitive_build(b,8192,(void *)(b+512),(void *)b,sizeof(blob)));
    packet(b,&fixture);
    scissors_cases(p);
    printf("Intel RCS primitive: %u checks passed\n",checks);
    return 0;
}
