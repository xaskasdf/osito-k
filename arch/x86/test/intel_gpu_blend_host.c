#define NATIVE_DRAW_SUITE_ENTRY draw_regression_main
#include "intel_gpu_draw_host.c"
#include "../drivers/intel_gen9_blend.h"

static void blend_capture_cases(void)
{
    for (unsigned mode=0;mode<18;mode++) {
        DRAW_FIXTURE f;draw_setup(&f);OWNER_FIXTURE *b=&f.render.base;
        CONTEXT_FIXTURE *n=&b->jobs.base.service.native;
        intel_gpu_command_context_t *ctx=&n->contexts.contexts[0];
        intel_umd_draw_constants_t *command=(void *)((BYTE *)ctx->cpu.cpu+128);
        command->draw.version=4;command->draw.bytes=136;
        command->parameter_count=command->reserved=0;
        intel_umd_blend_t state={INTEL_UMD_BLEND_VALID|0xf1,5|(6<<5)|(1<<10),2|(1<<5)|(1<<10),0x80402010};
        if (mode==1)state.flags|=2;
        if (mode==2)state.flags=INTEL_UMD_BLEND_VALID|0x50;
        if (mode==3)state.flags=INTEL_UMD_BLEND_VALID|1; /* No channels written. */
        memcpy(command->draw.reserved2,&state,sizeof(state));
        unsigned bytes=136;
        switch(mode) {
        case 4:command->draw.reserved2[0]&=~INTEL_UMD_BLEND_VALID;break;
        case 5:command->draw.reserved2[0]|=4;break;
        case 6:command->draw.reserved2[1]&=~31u;break;
        case 7:command->draw.reserved2[2]|=1u<<31;break;
        case 8:command->draw.reserved2[4]=1;break;
        case 9:command->draw.version=1;break;
        case 10:bytes=128;command->draw.bytes=128;break;
        case 11:command->parameter_count=1;bytes=140;command->draw.bytes=140;break;
        case 12:command->reserved=33;break;
        case 13:command->draw.reserved1[1]=1;break;
        case 14:command->draw.reserved2[1]=(command->draw.reserved2[1]&~31u)|16;break;
        case 15:command->draw.bytes++;break;
        case 16:bytes=140;command->draw.bytes=140;break;
        case 17: {
            command->reserved=2;bytes=168;command->draw.bytes=168;
            intel_umd_rectangle_t *rects=(void *)((BYTE *)command+136);
            rects[0]=(intel_umd_rectangle_t){0,0,1,1};rects[1]=(intel_umd_rectangle_t){15,15,1,1};
            break;
        }
        }
        bool valid=mode<4 || mode==17;
        if (!valid) {
            intel_gpu_capture_t capture={0};
            CHECK(intel_gpu_capture_begin(7,b->jobs.base.context,128,bytes,3,3,&capture)!=0);
            CHECK(intel_gpu_capture_release(&capture)==0);
            CHECK(!b->owner.engine.submitted_jobs);
        } else {
            uint64_t id;CHECK(intel_gpu_jobs_submit(&b->jobs.jobs,7,b->jobs.base.context,128,bytes,3,3,&id)==0);
            memset(command,0xa5,bytes); /* Poison the producer after capture. */
            for(unsigned i=0;i<8 && !b->jobs.done_count;i++){executor_step();service_step();}
            CHECK(b->jobs.done_count==1 && !b->jobs.done_status[0] && b->owner.engine.completed_jobs==1);
            const DWORD *batch=b->owner.slots[0].native.batch.cpu;
            intel_gen9_blend_state_t expected;CHECK(intel_gen9_blend_pack(&state,&expected));
            CHECK(batch[0x1200/4]==expected.header && batch[0x1204/4]==expected.entry[0]);
            CHECK(batch[0x1208/4]==expected.entry[1] && !memcmp(batch+0x11c8/4,expected.constant,16));
            CHECK(((batch[0x1040/4]>>18)&0x1ff)==expected.format);
            if(mode==17)CHECK(batch[0x1400/4]==0 && batch[0x1420/4]==0x000f000f);
        }
        owner_cleanup(b);CHECK(ram.allocated==ram.freed);
    }
}
static void blend_uniform_cases(const char *path)
{
    BYTE binary[512];FILE *file=fopen(path,"rb");CHECK(file);
    size_t size=fread(binary,1,sizeof(binary),file);fclose(file);
    struct intel_d3d9_program program;CHECK(size>sizeof(program));memcpy(&program,binary,sizeof(program));
    CHECK(program.parameter_count==4 && program.code_bytes<=sizeof(reference_native));
    for(unsigned rectangles=0;rectangles<2;rectangles++) {
        DRAW_FIXTURE f;draw_setup(&f);OWNER_FIXTURE *b=&f.render.base;
        intel_gpu_command_context_t *ctx=&b->jobs.base.service.native.contexts.contexts[0];
        BYTE *memory=f.programs[0]->cpu;
        memcpy(memory,binary,program.code_offset);memcpy(memory+4096,binary+program.code_offset,program.code_bytes);
        struct intel_d3d9_program *owned=(void *)memory;
        owned->code_offset=4096;owned->allocation_bytes=4096+program.code_bytes;
        intel_umd_draw_constants_t *command=(void *)((BYTE *)ctx->cpu.cpu+128);
        unsigned bytes=152+rectangles*32;
        command->draw.version=4;command->draw.bytes=bytes;command->draw.program_bytes=owned->allocation_bytes;
        command->parameter_count=4;command->reserved=rectangles*2;
        for(unsigned i=0;i<4;i++)command->values[i]=0x3f000000+i;
        command->draw.reserved2[0]=INTEL_UMD_BLEND_VALID|0xf1;
        command->draw.reserved2[1]=5|(6<<5)|(1<<10);command->draw.reserved2[2]=2|(1<<5)|(1<<10);
        command->draw.reserved2[3]=0x80402010;
        if(rectangles) {
            intel_umd_rectangle_t *list=(void *)((BYTE *)command+152);
            list[0]=(intel_umd_rectangle_t){0,0,1,1};list[1]=(intel_umd_rectangle_t){15,15,1,1};
        }
        uint64_t id;CHECK(intel_gpu_jobs_submit(&b->jobs.jobs,7,b->jobs.base.context,128,bytes,3,3,&id)==0);
        memset(command,0xa5,bytes);
        for(unsigned i=0;i<8 && !b->jobs.done_count;i++){executor_step();service_step();}
        CHECK(b->jobs.done_count==1 && !b->jobs.done_status[0] && b->owner.engine.completed_jobs==1);
        const DWORD *batch=b->owner.slots[0].native.batch.cpu;
        CHECK(batch[0x1204/4]&0x80000000u);
        for(unsigned i=0;i<4;i++)CHECK(batch[0x1800/4+i]==0x3f000000+i);
        if(rectangles)CHECK(batch[0x1400/4]==0 && batch[0x1420/4]==0x000f000f);
        owner_cleanup(b);CHECK(ram.allocated==ram.freed);
    }
}
int main(int argc,char **argv)
{
    CHECK(argc==2);CHECK(draw_regression_main()==0);unsigned before=checks;
    blend_capture_cases();blend_uniform_cases(argv[1]);
    printf("Intel native blend capture/queue: %u checks passed\n",checks-before);return 0;
}
