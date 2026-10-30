#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_rcs_draw.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %u: %s\n",__LINE__,#x); exit(1); } } while (0)
static uint32_t allocation[2050],before[2050],blob[144];
static const intel_rcs_pixel_draw_t fixture={0x1000200000ULL,0x1000210000ULL,0x1000220000ULL,32768,
    96,72,416,7,11,37,29,7,{0}};

static void pixel_packets(const uint32_t *b,unsigned mask)
{
    /* Gen9 Variable Pixel Dispatch table: index 3 means unused KSP. */
    static const unsigned slots[7][3]={{0,3,3},{1,3,3},{0,3,1},{2,3,3},{0,2,3},{3,2,1},{0,2,1}};
    unsigned ps=0,sba=0,sbe=0,swiz=0,wm=0,extra=0,end=0;
    CHECK(b[0]==0x69040300);
    for (unsigned at=1;at<1024;) {
        unsigned header=b[at],length=(header&255)+2;
        if (header==0x05000000) {end=at;break;}
        if (!header) {at++;continue;} /* MI_NOOP alignment */
        CHECK((header>>29)==3 && at+length<=1024);
        switch (header&0xffff0000u) {
        case 0x61010000:
            sba++;CHECK(length==18 && b[at+4]==0x200001 && b[at+5]==0x10);
            CHECK(b[at+10]==0x210001 && b[at+11]==0x10 && b[at+15]==4097);break;
        case 0x78200000:
            ps++;CHECK(length==12 && b[at+3]==((1<<18)|(1<<16)));
            CHECK(b[at+6]==((63u<<23)|mask));
            for (unsigned k=0;k<3;k++) {
                unsigned index=slots[mask-1][k],at_ksp=k ? 6+k*2 : 1;
                CHECK(b[at+at_ksp]==(index==3 ? 0 : index*128));
                CHECK(b[at+at_ksp+1]==0);
                CHECK(((b[at+7]>>(16-8*k))&127)==(index==3 ? 0 : 2+index*2));
            }break;
        case 0x781f0000:
            sbe++;CHECK(((b[at+1]>>22)&63)==1 && (b[at+1]&(1<<21)));
            CHECK(b[at+3]==1 && b[at+4]==3);break;
        case 0x78510000:swiz++;CHECK(length==11 && b[at+1]==0xf000);break;
        case 0x78140000:wm++;CHECK(length==2 && b[at+1]==0);break;
        case 0x784f0000:extra++;CHECK(b[at+1]==0x80000100u);break;
        default:break;
        }
        at+=length;
    }
    CHECK(ps==1 && sba==1 && sbe==1 && swiz==1 && wm==1 && extra==1 && end);
    CHECK(b[0x1000/4]==0x1040 && b[0x1000/4+1]==0);
    CHECK(b[0x1040/4+8]==0x220000 && b[0x1040/4+9]==0x10);
    for (unsigned i=0x1080/4;i<0x1180/4;i++) CHECK(b[i]==0); /* no texture/sampler/builtin code */
    CHECK(b[0x1180/4]==((40u<<16)|44));
}

static void pixel_cases(void)
{
    struct intel_d3d9_program *p=(void *)blob;
    *p=(struct intel_d3d9_program){.abi=1,.header_bytes=128,.allocation_bytes=sizeof(blob),.pci_id=0x3e9b,
        .bytecode_offset=128,.bytecode_bytes=4,.code_offset=192,.code_bytes=384,.stage=1,
        .simd_mask=7,.grf_used=32,.kernel_offset={0,128,256},.dispatch_grf_start={2,4,6},
        .flags=1,.varying_inputs=1,.layer_input=0};
    CHECK(intel_rcs_pixel_validate(p,sizeof(blob)));
    uint32_t *b=allocation+1;
    for (unsigned mask=1;mask<=7;mask++) {
        memset(allocation,0xa5,sizeof(allocation));
        intel_rcs_pixel_draw_t d=fixture;d.dispatch_mask=mask;
        CHECK(intel_rcs_pixel_build(b,8192,&d,p,sizeof(blob)));
        CHECK(allocation[0]==0xa5a5a5a5 && allocation[2049]==0xa5a5a5a5);
        pixel_packets(b,mask);
    }
    struct intel_d3d9_program good=*p;
    for (unsigned bad=0;bad<34;bad++) {
        *p=good;
        switch(bad) {
        case 0:p->abi=3;break;case 1:p->header_bytes--;break;case 2:p->allocation_bytes--;break;
        case 3:p->pci_id++;break;case 4:p->bytecode_offset++;break;case 5:p->bytecode_bytes=0;break;
        case 6:p->bytecode_bytes=3;break;case 7:p->bytecode_bytes=65540;break;case 8:p->code_offset=128;break;
        case 9:p->code_offset++;break;case 10:p->code_offset=0xffffffc0;break;case 11:p->code_bytes=0;break;
        case 12:p->code_bytes=65544;break;case 13:p->code_bytes--;break;case 14:p->stage++;break;
        case 15:p->simd_mask=0;break;case 16:p->simd_mask=8;break;case 17:p->grf_used=0;break;
        case 18:p->grf_used=129;break;case 19:p->scratch_bytes=1024;break;case 20:p->curb_read_length=1;break;
        case 21:p->constant_data_bytes=4;break;case 22:p->constant_data_offset=64;break;case 23:p->flags=2;break;
        case 24:p->varying_inputs=2;break;case 25:p->barycentric_modes=1;break;case 26:p->parameter_count=1;break;
        case 27:p->layer_input=UINT32_MAX;break;case 28:p->varying_inputs=0;break;case 29:p->reserved[4]=1;break;
        case 30:p->kernel_offset[1]++;break;case 31:p->kernel_offset[2]=384;break;
        case 32:p->dispatch_grf_start[0]=128;break;case 33:p->code_bytes+=8;break;
        }
        memcpy(before,allocation,sizeof(before));
        CHECK(!intel_rcs_pixel_validate(p,sizeof(blob)));
        CHECK(!intel_rcs_pixel_build(b,8192,&fixture,p,sizeof(blob)));
        CHECK(!memcmp(allocation,before,sizeof(before)));
    }
    *p=good;
    for (unsigned bad=0;bad<21;bad++) {
        intel_rcs_pixel_draw_t d=fixture;
        switch(bad) {
        case 0:d.batch_gpu=0;break;case 1:d.program_gpu++;break;case 2:d.destination_gpu++;break;
        case 3:d.program_gpu=d.batch_gpu+4096;break;case 4:d.destination_gpu=d.program_gpu;break;
        case 5:d.destination_gpu=d.batch_gpu;break;case 6:d.program_gpu=1ULL<<47;break;
        case 7:d.destination_bytes=0;break;case 8:d.destination_bytes=(1u<<28)+1;break;
        case 9:d.destination_width=0;break;case 10:d.destination_height=4097;break;
        case 11:d.destination_pitch=383;break;case 12:d.destination_pitch=262148;break;
        case 13:d.destination_bytes=29951;break;case 14:d.width=0;break;case 15:d.height=73;break;
        case 16:d.x=UINT32_MAX;break;case 17:d.y=44;break;case 18:d.dispatch_mask=0;break;
        case 19:d.dispatch_mask=8;break;case 20:d.width=97;break;
        }
        memcpy(before,allocation,sizeof(before));
        CHECK(!intel_rcs_pixel_build(b,8192,&d,p,sizeof(blob)));
        CHECK(!memcmp(allocation,before,sizeof(before)));
    }
    CHECK(!intel_rcs_pixel_validate(NULL,128));
    CHECK(!intel_rcs_pixel_validate(p,127));
    CHECK(!intel_rcs_pixel_validate((void *)((char *)p+1),sizeof(blob)));
    CHECK(!intel_rcs_pixel_build(b,8191,&fixture,p,sizeof(blob)));
    CHECK(!intel_rcs_pixel_build((void *)((char *)b+1),8192,&fixture,p,sizeof(blob)));
    p->simd_mask=2;
    CHECK(!intel_rcs_pixel_build(b,8192,&fixture,p,sizeof(blob)));
    *p=good;p->varying_inputs=0;p->layer_input=UINT32_MAX;
    CHECK(intel_rcs_pixel_build(b,8192,&fixture,p,sizeof(blob)));
    /* Input metadata may alias the output; its snapshot precedes all writes. */
    *p=good;memcpy(b,p,sizeof(blob));memcpy(b+513,&fixture,sizeof(fixture));
    CHECK(!intel_rcs_pixel_build(b,8192,(void *)(b+512),(void *)b,sizeof(blob)));
    CHECK(intel_rcs_pixel_build(b,8192,(void *)(b+513),(void *)b,sizeof(blob)));
    pixel_packets(b,7);
}

int main(void)
{
    pixel_cases();
    printf("Intel RCS pixel: %u checks passed\n",checks);
    return 0;
}
