#define NATIVE_DRAW_SUITE_ENTRY draw_regression_main
#include "intel_gpu_draw_host.c"
int main(int argc,char **argv)
{
    CHECK(argc==2); CHECK(draw_regression_main()==0);
    FILE *file=fopen(argv[1],"rb");CHECK(file);
    BYTE binary[512];size_t bytes=fread(binary,1,sizeof(binary),file);fclose(file);
    struct intel_d3d9_program original;CHECK(bytes>sizeof(original));memcpy(&original,binary,sizeof(original));
    CHECK(original.parameter_count==4 && original.code_bytes<=sizeof(reference_native));
    unsigned before=checks;
    for (unsigned mode=0;mode<29;mode++) {
        DRAW_FIXTURE f;draw_setup(&f);OWNER_FIXTURE *b=&f.render.base;
        CONTEXT_FIXTURE *n=&b->jobs.base.service.native;
        intel_gpu_command_context_t *ctx=&n->contexts.contexts[0];
        BYTE *memory=f.programs[0]->cpu;
        memcpy(memory,binary,original.code_offset);
        memcpy(memory+4096,binary+original.code_offset,original.code_bytes);
        struct intel_d3d9_program *program=(void *)memory;
        program->code_offset=4096;program->allocation_bytes=4096+program->code_bytes;
        intel_umd_draw_constants_t *command=(void *)((BYTE *)ctx->cpu.cpu+128);
        command->draw.version=2;command->draw.bytes=152;command->draw.program_bytes=program->allocation_bytes;
        command->parameter_count=4;command->reserved=0;
        for (unsigned i=0;i<4;i++) command->values[i]=0x3f000000+i;
        unsigned length=152;
        intel_umd_rectangle_t *rects=(void *)((BYTE *)command+152);
        if (mode>=14) {
            command->draw.version=3;command->reserved=mode==28 ? 32 : 2;
            command->draw.bytes=length=152+command->reserved*16;
            for (unsigned i=0;i<command->reserved;i++)rects[i]=(intel_umd_rectangle_t){i%16,i/16,1,1};
        }
        switch(mode) {
        case 0:command->parameter_count=513;break;
        case 1:command->parameter_count=0;break;
        case 2:command->parameter_count=3;command->draw.bytes=length=148;break;
        case 3:command->reserved=1;break;
        case 4:command->draw.version=1;break;
        case 5:command->draw.bytes--;break;
        case 6:length=151;break;
        case 7:length=128;command->draw.bytes=128;break;
        case 8:program->parameter_map_offset=4096;break;
        case 9:((DWORD *)(memory+program->parameter_map_offset))[0]=896;break;
        case 10:program->curb_read_length=0;break;
        case 11:length=sizeof(*command)+4;break;
        case 14:command->reserved=0;break;
        case 15:command->reserved=33;break;
        case 16:length-=4;command->draw.bytes=length;break;
        case 17:length+=4;command->draw.bytes=length;break;
        case 18:rects[1].width=0;break;
        case 19:rects[1].height=UINT32_MAX;break;
        case 20:rects[1].x=UINT32_MAX;break;
        case 21:rects[1].y=16;break;
        case 22:rects[1].width=16;break;
        case 23:command->draw.width=1;break;
        case 24:command->draw.version=2;break;
        case 25:command->parameter_count=513;break;
        default:break;
        }
        if (mode!=13 && mode<27) {
            intel_gpu_capture_t capture={0};
            NTSTATUS status=intel_gpu_capture_begin(7,b->jobs.base.context,128,length,3,3,&capture);
            CHECK((status==0)==(mode==12 || mode==26));
            if (!status) {
                memset(command,0xa5,length);
                CHECK(capture.constants.parameter_count==4 && capture.constants.values[2]==0x3f000002);
                if(mode==26) {
                    const intel_umd_rectangle_t *saved=(const void *)((const BYTE *)&capture.batch+152);
                    CHECK(capture.constants.reserved==2 && saved[1].x==1 && saved[1].width==1);
                }
            }
            CHECK(intel_gpu_capture_release(&capture)==0);
        } else {
            uint64_t id;CHECK(intel_gpu_jobs_submit(&b->jobs.jobs,7,b->jobs.base.context,128,length,3,3,&id)==0);
            memset(command,0xa5,length);
            for (unsigned i=0;i<8 && !b->jobs.done_count;i++) {executor_step();service_step();}
            CHECK(b->jobs.done_count==1 && b->jobs.done_status[0]==0 && b->owner.engine.completed_jobs==1);
            intel_bcs_context_t *native=&b->owner.slots[0].native;
            const DWORD *batch=native->batch.cpu;
            for (unsigned i=0;i<4;i++) CHECK(batch[0x1800/4+i]==0x3f000000+i);
            for (unsigned i=4;i<512;i++) CHECK(batch[0x1800/4+i]==0);
            if(mode>=27)for(unsigned i=0;i<(mode==28 ? 32u : 2u);i++) {
                unsigned expected=(i%16)|((i/16)<<16);
                CHECK(batch[(0x1400+i*32)/4]==expected && batch[(0x1400+i*32)/4+1]==expected);
            }
        }
        owner_cleanup(b);CHECK(ram.allocated==ram.freed);
    }
    printf("Intel native constant capture/queue: %u checks passed\n",checks-before);return 0;
}
