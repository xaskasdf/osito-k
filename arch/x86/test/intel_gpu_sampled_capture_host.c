/* Production capture/registry/executor/queue, synthetic MMIO and owned RAM.
 * The existing compiled shader fixtures execute no CPU pixel interpreter. */
#define NATIVE_DRAW_SUITE_ENTRY sampled_legacy_draw_main
#include "intel_gpu_draw_host.c"
#include "../drivers/intel_gen9_shader.h"

static struct {
    RENDER_OWNER_FIXTURE render;
    OKMT_HANDLE allocations[21];
    intel_gpu_buffer_t *buffers[21];
    intel_umd_draw_sampled_t packet;
    OKMT_ALLOCATION_LIST list[INTEL_UMD_MAX_RESOURCES];
    OKMT_PATCH_LOCATION patches[INTEL_UMD_MAX_RESOURCES];
    unsigned count,patch_count,bytes,first_texture;
} sample;
static const char *program_directory;
static const char *capacity_directory;
static unsigned sampled_jobs;

static void sample_setup(void)
{
    memset(&sample,0,sizeof(sample));
    OWNER_FIXTURE *b=&sample.render.base;
    jobs_setup(&b->jobs);bcs_model_reset();b->pat=0x03112233;
    sample.render.cache[0]=0x11;sample.render.cache[1]=0x22;sample.render.cache[2]=0x33445566;
    b->jobs.jobs.ready=intel_gpu_executor_notify;
    CONTEXT_FIXTURE *n=&b->jobs.base.service.native;
    CHECK(d3dkmt_destroy_context(7,b->jobs.base.context)==0);
    CHECK(d3dkmt_destroy_allocations(7,n->device,2,b->jobs.base.allocations)==0);service_step();
    intel_umd_context_description_t layout=command_layout;layout.command_bytes=8192;
    D3DKMT_CONTEXT_DESCRIPTION desc={.node=INTEL_UMD_NODE_RENDER,.private_bytes=sizeof(layout)};
    memcpy(desc.private_data,&layout,sizeof(layout));D3DKMT_CONTEXT_BUFFERS views;
    CHECK(d3dkmt_create_context(7,n->device,&desc,&b->jobs.base.context,&views)==0);
    D3DKMT_ALLOCATION_DESCRIPTION sizes[21];
    for (unsigned i=0;i<21;i++) sizes[i]=(D3DKMT_ALLOCATION_DESCRIPTION){i==2 ? 8192 : 4096};
    CHECK(d3dkmt_create_allocations(7,n->device,21,sizes,sample.allocations)==0);
    for (unsigned i=0;i<21;i++) {
        intel_gpu_kmt_cpu_lease_t cpu={0};
        CHECK(intel_gpu_kmt_acquire_cpu(7,n->device,sample.allocations[i],&cpu)==0);
        sample.buffers[i]=cpu.access.buffer;memset(cpu.access.cpu,0,cpu.access.bytes);
        CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
    }
    b->jobs.base.allocations[0]=sample.allocations[0];b->jobs.base.allocations[1]=sample.allocations[1];
    const intel_ggtt_io_t ggtt_io={res_ggtt_read,res_ggtt_write,res_ggtt_sync};
    const intel_ggtt_layout_t ggtt_layout={.ggtt_bytes=8ULL<<20,.gpu_va_bytes=1ULL<<32};
    CHECK(intel_ggtt_space_init(&b->space,&ggtt_layout,1,63,sample.render.bitmap,sizeof(sample.render.bitmap),&ggtt_io,ggtt)==0);
    intel_gpu_bcs_config_t config={.space=&b->space,.allocator=allocator,.allocator_context=&ram,
        .io={render_owner_read,render_owner_write,owner_now,owner_relax},.hardware_context=&sample.render,
        .token_read=render_owner_token,.mapping_stopped=owner_mapping,.ggtt_limit=0x10000000,
        .source_gpu=0x100000000ULL,.destination_gpu=0x110000000ULL,
        .batch_gpu=0x120000000ULL,.program_gpu=0x130000000ULL,
        .render=true,.first_fence=0x11223344,.first_hardware_id=0x1001};
    CHECK(intel_gpu_bcs_owner_init(&b->owner,&b->jobs.jobs,&config)==0);
    CHECK(intel_gpu_bcs_owner_reserve(&b->owner,2,64)==0);
    CHECK(intel_gpu_bcs_owner_start(&b->owner)==0);
}
static void sample_stage(void)
{
    intel_gpu_command_context_t *ctx=&sample.render.base.jobs.base.service.native.contexts.contexts[0];
    memcpy((BYTE *)ctx->cpu.cpu+128,&sample.packet,sample.bytes);
    memcpy((BYTE *)ctx->cpu.cpu+ctx->allocation_offset,sample.list,sample.count*sizeof(*sample.list));
    memcpy((BYTE *)ctx->cpu.cpu+ctx->patch_offset,sample.patches,sample.patch_count*sizeof(*sample.patches));
}
static void sample_fill(const char *name,bool alias,unsigned rotation,bool permute)
{
    char path[1024];snprintf(path,sizeof(path),"%s/%s.program",
        !strcmp(name,"ps30-all16") ? capacity_directory : program_directory,name);
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long size=ftell(file);rewind(file);
    CHECK(size>=128 && size<65536);BYTE *blob=malloc((size_t)size);CHECK(blob);
    CHECK(fread(blob,1,(size_t)size,file)==(size_t)size);fclose(file);
    struct intel_d3d9_program p=*(struct intel_d3d9_program *)blob;
    CHECK(intel_gen9_resource_shader_validate(&p,blob,(size_t)size));
    unsigned old=p.code_offset;CHECK(old<=4096 && p.code_bytes<=4096);
    p.code_offset=4096;p.allocation_bytes=4096+p.code_bytes;
    intel_gpu_cpu_access_t upload={0};CHECK(intel_gpu_buffer_acquire_cpu(sample.buffers[2],7,&upload)==0);
    memset(upload.cpu,0,upload.bytes);memcpy(upload.cpu,blob,old);
    memcpy((BYTE *)upload.cpu+4096,blob+old,p.code_bytes);memcpy(upload.cpu,&p,sizeof(p));free(blob);
    CHECK(intel_gen9_resource_shader_validate(&p,upload.cpu,p.allocation_bytes));
    CHECK(intel_gpu_buffer_release_cpu(&upload)==0);
    memset(&sample.packet,0,sizeof(sample.packet));memset(sample.list,0,sizeof(sample.list));
    memset(sample.patches,0,sizeof(sample.patches));sample.count=sample.patch_count=3;
    sample.packet.draw=(intel_umd_draw32_t){.magic=INTEL_UMD_COMMAND_MAGIC,.version=INTEL_UMD_DRAW_SAMPLED_VERSION,
        .opcode=INTEL_UMD_DRAW32,.destination_width=16,.destination_height=16,.destination_pitch=64,
        .width=16,.height=16,.vertex_stride=144,.vertex_count=3,.topology=INTEL_RCS_TRIANGLE_LIST,
        .program_bytes=p.allocation_bytes,.dispatch_mask=p.simd_mask,
        .reserved2={INTEL_UMD_BLEND_VALID|INTEL_UMD_BLEND_WRITE_ALL,2|(1<<5)|(1<<10),2|(1<<5)|(1<<10)}};
    sample.packet.parameter_count=p.parameter_count;sample.packet.rectangle_count=2;sample.packet.bound_mask=p.texture_mask;
    for (unsigned i=0;i<8;i++) if (p.texcoord_mask&(1u<<i)) sample.packet.texcoord_offset[i]=16+i*16;
    for (unsigned i=0;i<p.parameter_count;i++) sample.packet.values[i]=0x3f800000;
    intel_umd_rectangle_t *rect=(void *)(sample.packet.values+p.parameter_count);
    rect[0]=(intel_umd_rectangle_t){0,0,8,16};rect[1]=(intel_umd_rectangle_t){8,0,8,16};
    for (unsigned i=0;i<3;i++) {
        sample.list[i]=(OKMT_ALLOCATION_LIST){sample.allocations[i],i==1};
        sample.patches[i]=(OKMT_PATCH_LOCATION){i,i,0,0,128+80+i*8,0};
    }
    sample.first_texture=16;
    for (unsigned i=0;i<16;i++) if (p.texture_mask&(1u<<i)) {
        if (sample.first_texture==16) sample.first_texture=i;
        unsigned index=alias && sample.count>3 ? 3 : sample.count++;
        sample.list[index]=(OKMT_ALLOCATION_LIST){sample.allocations[3+(alias ? rotation : (i+rotation)%18)],0};
        sample.packet.textures[i]=(intel_umd_texture_t){.width=4,.height=4,.levels=3,.format=21,.count=3,
            .sampler={1,1,0,3,3,3,0,0,0,0}};
        sample.patches[sample.patch_count++]=(OKMT_PATCH_LOCATION){index,3+i,0,0,
            128+__builtin_offsetof(intel_umd_draw_sampled_t,textures)+i*sizeof(intel_umd_texture_t),0};
    }
    if (permute) {
        for (unsigned i=0;i<sample.count/2;i++) {
            OKMT_ALLOCATION_LIST a=sample.list[i];sample.list[i]=sample.list[sample.count-1-i];sample.list[sample.count-1-i]=a;
        }
        for (unsigned i=0;i<sample.patch_count;i++) sample.patches[i].allocation_index=sample.count-1-sample.patches[i].allocation_index;
        for (unsigned i=0;i<sample.patch_count/2;i++) {
            OKMT_PATCH_LOCATION pch=sample.patches[i];sample.patches[i]=sample.patches[sample.patch_count-1-i];sample.patches[sample.patch_count-1-i]=pch;
        }
    }
    sample.bytes=__builtin_offsetof(intel_umd_draw_sampled_t,values)+p.parameter_count*4+32;
    sample.packet.draw.bytes=sample.bytes;sample_stage();
}
static void sample_capture_cases(void)
{
    for (unsigned mode=0;mode<43;mode++) {
        sample_setup();sample_fill("ps30-pair",false,0,false);
        intel_umd_draw_sampled_t *p=&sample.packet;unsigned first=sample.first_texture;
        intel_umd_texture_t *t=&p->textures[first];
        switch(mode) {
        case 0: p->reserved=1;break;
        case 1: p->bound_mask|=1u<<16;break;
        case 2: p->bound_mask^=1u<<7;break;
        case 3: t->image=4096;break;
        case 4: t->width=0;break;
        case 5: t->height=4097;break;
        case 6: t->levels=14;break;
        case 7: t->format=0;break;
        case 8: t->pitch=63;break;
        case 9: t->first=3;break;
        case 10: t->count=0;break;
        case 11: t->count=4;break;
        case 12: t->srgb=2;break;
        case 13: t->sampler.min_filter=3;break;
        case 14: t->sampler.address_v=6;break;
        case 15: t->sampler.lod_bias=-4097;break;
        case 16: t->sampler.max_lod=4096;break;
        case 17: t->width=t->height=64;break;
        case 18: p->textures[14].width=1;break;
        case 19:
            for (unsigned i=0;i<8;i++) if (!p->texcoord_offset[i]) {p->texcoord_offset[i]=16;break;}
            break;
        case 20: p->texcoord_offset[0]=143;break;
        case 21: p->draw.vertex_offset=4096-32;break;
        case 22: p->draw.dispatch_mask=8;break;
        case 23: p->parameter_count=513;break;
        case 24: p->rectangle_count=33;break;
        case 25: ((intel_umd_rectangle_t *)(p->values+p->parameter_count))[1].width=17;break;
        case 26: sample.patches[3].allocation_index=2;break;
        case 27: sample.patches[3].slot=2;break;
        case 28: sample.patches[3].patch_offset++;break;
        case 29: sample.patches[3].allocation_offset=4;break;
        case 30: sample.list[3].flags=1;break;
        case 31: sample.list[3].allocation=sample.list[0].allocation;break;
        case 32: sample.list[sample.count++]=(OKMT_ALLOCATION_LIST){sample.allocations[20],0};break;
        case 33: sample.patch_count--;break;
        case 34: sample.bytes=1452;p->draw.bytes=sample.bytes;break;
        case 35: sample.bytes-=4;p->draw.bytes=sample.bytes;break;
        case 36: p->draw.reserved2[0]=0;break;
        case 37: ((uint32_t *)sample.buffers[2]->cpu)[1024]^=0x20;break;
        case 38: p->draw.version=INTEL_UMD_DRAW_BLEND_VERSION;break;
        case 39: sample_fill("ps30-pair",true,0,true);break;
        case 40: sample_fill("ps30-dead-sample",false,0,true);break;
        case 41: sample_fill("ps30-sparse",false,0,true);break;
        case 42: break;
        }
        sample_stage();intel_gpu_capture_t capture={0};
        NTSTATUS status=intel_gpu_capture_begin(7,sample.render.base.jobs.base.context,128,
            sample.bytes,sample.count,sample.patch_count,&capture);
        if ((status==0)!=(mode>=39)) fprintf(stderr,"sample capture mode=%u status=%08x resources=%u\n",
            mode,(unsigned)status,capture.resource_count);
        CHECK((status==0)==(mode>=39));
        if (mode>=39) CHECK(capture.resource_count==sample.count && capture.valid);
        CHECK(intel_gpu_capture_release(&capture)==0 && !capture.self);
        owner_cleanup(&sample.render.base);CHECK(ram.allocated==ram.freed);
    }
}
static void sample_run_cases(void)
{
    const char *names[]={"ps20-sample","ps30-sample","ps30-sparse","ps30-pair","ps30-dependent",
        "ps30-tint","ps30-coordinate-swizzle","ps30-constant-coordinate","ps30-input-color",
        "ps30-unused-declaration","ps30-dead-sample","ps20-hlsl-sample","ps30-hlsl-pair","ps30-all16"};
    for (unsigned mode=0;mode<8;mode++) {
        sample_setup();OWNER_FIXTURE *b=&sample.render.base;CONTEXT_FIXTURE *n=&b->jobs.base.service.native;
        for (unsigned round=0;round<(mode ? 1u : 56u);round++) {
            b->jobs.done_count=0;sample_fill(mode ? "ps30-pair" : names[round%14],round&1,(round/14)%2,round&2);
            uint64_t id=0;CHECK(intel_gpu_jobs_submit(&b->jobs.jobs,7,b->jobs.base.context,128,
                sample.bytes,sample.count,sample.patch_count,&id)==0);
            intel_gpu_command_context_t *ctx=&n->contexts.contexts[0];
            memset((BYTE *)ctx->cpu.cpu+128,0xa5,sample.bytes);
            memset((BYTE *)ctx->cpu.cpu+ctx->allocation_offset,0xa5,sample.count*sizeof(*sample.list));
            memset((BYTE *)ctx->cpu.cpu+ctx->patch_offset,0xa5,sample.patch_count*sizeof(*sample.patches));
            intel_gpu_cpu_access_t cpu={0};
            if (mode==1) ((uint32_t *)sample.buffers[2]->cpu)[1024]^=0x20;
            if (mode==2) CHECK(d3dkmt_destroy_allocations(7,n->device,1,&sample.allocations[3])==0);
            if (mode==3) CHECK(intel_gpu_buffer_acquire_cpu(sample.buffers[3],7,&cpu)==0);
            if (mode==4 || mode==5) hw.delay=1000;
            if (mode==6) CHECK(intel_gpu_jobs_cancel(&b->jobs.jobs,7,id)==0);
            if (mode==7) {vm_model_exit(7);process_closing=true;d3dkmt_release_process(7);}
            for (unsigned i=0;i<8 && !b->jobs.done_count;i++) {executor_step();service_step();}
            intel_bcs_context_t *native=&b->owner.slots[0].native;
            if (mode==3) {
                CHECK(!b->jobs.done_count && !b->owner.engine.submitted_jobs && cpu.self);
                CHECK(intel_gpu_buffer_release_cpu(&cpu)==0);
                for (unsigned i=0;i<8 && !b->jobs.done_count;i++) {executor_step();service_step();}
            }
            if (mode==4 || mode==5) {
                CHECK(!b->jobs.done_count && b->owner.engine.inflight && native->texture_count==2);
                for (unsigned i=0;i<2;i++) CHECK(native->texture_access[i].self &&
                    intel_gpu_buffer_acquire_cpu(native->textures[i].buffer,7,&cpu)==INTEL_GPU_BUFFER_BUSY);
                if (mode==5) {vm_model_exit(7);process_closing=true;d3dkmt_release_process(7);}
                hw.stall_stop=true;CHECK(intel_gpu_bcs_owner_close(&b->owner)==D3DKMT_STATUS_DEVICE_BUSY);
                executor_step();service_step();CHECK(!b->owner.engine.retired && native->texture_access[0].self);
            } else {
                CHECK(b->jobs.done_count==1);bool valid=mode==0 || mode==3;
                CHECK((b->jobs.done_status[0]==0)==valid);
                CHECK(b->owner.engine.submitted_jobs==(valid ? round+1 : 0));
                if (valid) {sampled_jobs++;CHECK(native->texture_count==sample.count-3 && !native->access_token);}
            }
        }
        owner_cleanup(b);CHECK(ram.allocated==ram.freed);
    }
}
static void sample_capacity_cases(void)
{
    for (unsigned mode=0;mode<22;mode++) {
        sample_setup();sample_fill("ps30-all16",false,0,false);
        CHECK(sample.count==19 && sample.patch_count==19);
        unsigned count=sample.count,patches=sample.patch_count;
        if (mode<16) {
            CONTEXT_FIXTURE *n=&sample.render.base.jobs.base.service.native;
            CHECK(d3dkmt_destroy_allocations(7,n->device,1,&sample.allocations[3+mode])==0);
        } else if (mode==16) {sample.packet.textures[15].width=4096;sample_stage();}
        else if (mode==17) count=20;
        else if (mode==18) patches=20;
        else if (mode==19) {sample.patches[18].allocation_index=0;sample_stage();}
        else if (mode==20) {sample.patches[18].allocation_index=3;sample_stage();} /* unused last allocation */
        intel_gpu_capture_t capture={0};
        NTSTATUS status=intel_gpu_capture_begin(7,sample.render.base.jobs.base.context,128,
            sample.bytes,count,patches,&capture);
        CHECK((status==0)==(mode==21));
        if (mode==21) {
            CHECK(capture.resource_count==19 && capture.texture_resource[15]==18);
            for (unsigned i=0;i<19;i++) CHECK(capture.resources[i].reference.self);
        }
        CHECK(intel_gpu_capture_release(&capture)==0 && !capture.self);
        owner_cleanup(&sample.render.base);CHECK(ram.allocated==ram.freed);
    }
}
#ifndef NATIVE_SAMPLED_SUITE_ENTRY
#define NATIVE_SAMPLED_SUITE_ENTRY main
#endif
int NATIVE_SAMPLED_SUITE_ENTRY(int argc,char **argv)
{
    CHECK(argc==3);program_directory=argv[1];capacity_directory=argv[2];
    static SERVICE_FIXTURE bootstrap;service_setup(&bootstrap);service_cleanup(&bootstrap,false);
    CHECK(intel_gpu_executor_start()==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(intel_gpu_executor_start()==STATUS_INSUFFICIENT_RESOURCES);CHECK(intel_gpu_executor_start()==0);
    sample_capture_cases();sample_capacity_cases();sample_run_cases();
    printf("Native sampled capture PASS checks=%u completed_jobs=%u programs=14\n",checks,sampled_jobs);return 0;
}
