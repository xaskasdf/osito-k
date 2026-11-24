/* Typed vertex admission and real command encoding through production queue.
 * This test inspects commands; it does not interpret/render vertex data. */
#define NATIVE_SAMPLED_SUITE_ENTRY vertex_sampled_baseline
#include "intel_gpu_sampled_capture_host.c"
static unsigned typed_jobs;
static void vertex_capture_cases(void)
{
    static const unsigned sizes[17]={4,8,12,16,4,4,4,8,4,4,8,4,8,4,4,4,8};
    static const unsigned formats[17]={216,133,64,0,192,245,246,147,199,205,129,204,128,243,197,208,132};
    static const unsigned controls[17]={0x12230000,0x11230000,0x11130000,0x11110000,
        0x11110000,0x11110000,0x11230000,0x11110000,0x11110000,0x11230000,0x11110000,
        0x11230000,0x11110000,0x11130000,0x11130000,0x11230000,0x11110000};
    for (unsigned type=0;type<17;type++) for (unsigned layout=0;layout<2;layout++) {
        sample_setup();OWNER_FIXTURE *b=&sample.render.base;
        sample_fill("ps30-sample",false,0,false);
        unsigned position=layout ? sizes[type] : 0,coordinate=layout ? 0 : 16;
        sample.packet.draw.version=INTEL_UMD_DRAW_VERTEX_VERSION;
        sample.packet.draw.vertex_stride=16+sizes[type];
        sample.packet.position_offset=position;
        sample.packet.texcoord_offset[0]=coordinate|(type<<16);
        /* A final vertex exactly ends at the allocation boundary. */
        sample.packet.draw.vertex_offset=4096-3*sample.packet.draw.vertex_stride;
        intel_umd_draw_sampled_t valid=sample.packet;
        for (unsigned fault=0;fault<9;fault++) {
            sample.packet=valid;
            switch (fault) {
            case 1: sample.packet.draw.vertex_offset+=4;break;
            case 2: sample.packet.texcoord_offset[0]|=1;break;
            case 3: sample.packet.texcoord_offset[0]=coordinate|(17u<<16);break;
            case 4: sample.packet.texcoord_offset[0]=coordinate|(0xffffu<<16);break;
            case 5: sample.packet.position_offset=sample.packet.draw.vertex_stride-12;break;
            case 6: sample.packet.position_offset|=1;break;
            case 7: sample.packet.texcoord_offset[7]=3u<<16;break;
            case 8: sample.packet.draw.version=7;break;
            }
            sample_stage();intel_gpu_capture_t capture={0};
            NTSTATUS status=intel_gpu_capture_begin(7,b->jobs.base.context,128,sample.bytes,sample.count,sample.patch_count,&capture);
            CHECK((status==0)==(fault==0));
            CHECK(intel_gpu_capture_release(&capture)==0 && !capture.self);
        }
        sample.packet=valid;sample_stage();uint64_t id=0;
        CHECK(intel_gpu_jobs_submit(&b->jobs.jobs,7,b->jobs.base.context,128,sample.bytes,sample.count,sample.patch_count,&id)==0);
        intel_gpu_command_context_t *ctx=&b->jobs.base.service.native.contexts.contexts[0];
        memset((BYTE *)ctx->cpu.cpu+128,0xa5,sample.bytes);
        for (unsigned i=0;i<8 && !b->jobs.done_count;i++) {executor_step();service_step();}
        CHECK(b->jobs.done_count==1 && b->jobs.done_status[0]==0);
        const uint32_t *batch=b->owner.slots[0].native.batch.cpu;unsigned found=0;
        for (unsigned i=0;i<1024;i++) if ((batch[i]&0xffff0000u)==0x78090000u) {
            CHECK(batch[i+3]==((1u<<25)|position) && batch[i+4]==0x11110000u);
            CHECK(batch[i+5]==((1u<<25)|(formats[type]<<16)|coordinate));
            CHECK(batch[i+6]==controls[type]);found++;
        }
        CHECK(found==1);typed_jobs++;
        owner_cleanup(b);CHECK(ram.allocated==ram.freed);
    }
}
int main(int argc,char **argv)
{
    CHECK(vertex_sampled_baseline(argc,argv)==0);
    vertex_capture_cases();
    printf("Native vertex fetch PASS checks=%u typed_jobs=%u types=17 layouts=2\n",checks,typed_jobs);
    return 0;
}
