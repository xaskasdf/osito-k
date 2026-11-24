/* The same real mapping/queue code, with an independent VS allocation. */
#define INTEL_SAMPLED_CONTEXT_EMBED
#include "intel_sampled_context_host.c"
static intel_gpu_buffer_t vertex_code[3];
static unsigned vertex_prepared;
static const uint64_t vertex_gpu=0x48000000;
static intel_bcs_program_binding_t vertex_binding(unsigned bank)
{ return (intel_bcs_program_binding_t){&vertex_code[bank],vertex_gpu}; }
static void vertex_setup(bool activate)
{
    sampled_setup(false,false);memset(vertex_code,0,sizeof(vertex_code));
    for (unsigned i=0;i<3;i++) CHECK(intel_gpu_buffer_create(&vertex_code[i],&allocator,&ram,i==2 ? 9 : 7,8192)==INTEL_GPU_BUFFER_OK);
    if (activate) {
        CHECK(intel_bcs_context_activate_vertex(&sampled,7,&banks[0][0],&banks[0][1],&banks[0][2],vertex_binding(0),bindings[0],16)==INTEL_BCS_OK);
        CHECK(intel_bcs_context_attach(&sampled,&sampled_queue,NULL));
    }
}
static void vertex_cleanup(void)
{
    fail_operation=ram.fail_sync=ram.fail_release=0;
    for (unsigned i=0;i<3;i++)vertex_code[i].busy=0;
    if (sampled.access_token)CHECK(intel_bcs_context_retire(&sampled,sampled.access_token)==INTEL_BCS_OK);
    if (sampled.queue) {
        CHECK(intel_bcs_queue_close_context(&sampled_queue,sampled.queue_owner,sampled.queue_id)==INTEL_BCS_OK);
        sampled_queue.stopped=true;sampled_engine.retired=true;
    }
    CHECK(intel_bcs_context_destroy(&sampled));
    for (unsigned i=0;i<3;i++)CHECK(intel_gpu_buffer_close(&vertex_code[i],i==2 ? 9 : 7)==INTEL_GPU_BUFFER_OK);
    sampled_cleanup();
}
static enum intel_bcs_status vertex_change(unsigned bank,bool retarget)
{
    if (retarget)return intel_bcs_context_retarget_vertex(&sampled,9,78,&banks[bank][0],&banks[bank][1],&banks[bank][2],vertex_binding(bank),bindings[bank],16);
    return intel_bcs_context_rebind_vertex(&sampled,&banks[bank][0],&banks[bank][1],&banks[bank][2],vertex_binding(bank),bindings[bank],16);
}
static void vertex_verify(unsigned bank)
{
    sampled_verify(bank,16);
    CHECK(sampled.vertex_program.buffer==&vertex_code[bank] && sampled.vertex_program.gpu==vertex_gpu);
    CHECK(sampled.vertex_program_pinned && !sampled.retained_vertex_program && vertex_code[bank].residents==1);
    CHECK(sampled_pte(vertex_gpu)==(vertex_code[bank].dma|0x19));
    CHECK(sampled_pte(vertex_gpu+4096)==((vertex_code[bank].dma+4096)|0x19));
    CHECK(!sampled_pte(vertex_gpu+8192));
}
static void vertex_lifetime_cases(void)
{
    vertex_setup(true);vertex_verify(0);
    intel_gpu_cpu_access_t cpu={0};
    CHECK(intel_gpu_buffer_acquire_cpu(&vertex_code[0],7,&cpu)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_bcs_context_acquire(&sampled,1)==INTEL_BCS_BUSY);
    CHECK(sampled.program_access.self && !sampled.vertex_program_access.self && !sampled.texture_access[0].self);
    CHECK(vertex_change(1,false)==INTEL_BCS_INVALID);
    CHECK(intel_bcs_context_retire(&sampled,1)==INTEL_BCS_OK);
    CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_bcs_context_acquire(&sampled,2)==INTEL_BCS_OK);
    CHECK(intel_gpu_buffer_acquire_cpu(&vertex_code[0],7,&cpu)==INTEL_GPU_BUFFER_BUSY);
    vertex_code[0].busy=1;
    CHECK(intel_bcs_context_retire(&sampled,2)==INTEL_BCS_BUSY && sampled.vertex_program_access.self);
    CHECK(!intel_bcs_context_destroy(&sampled));vertex_code[0].busy=0;
    CHECK(intel_bcs_context_retire(&sampled,2)==INTEL_BCS_OK);
    CHECK(vertex_change(0,false)==INTEL_BCS_OK);vertex_verify(0);
    CHECK(vertex_change(1,false)==INTEL_BCS_OK);vertex_verify(1);CHECK(!vertex_code[0].residents);
    CHECK(sampled_change(0,16,false)==INTEL_BCS_OK);sampled_verify(0,16);
    CHECK(!sampled.vertex_program.buffer && !sampled.vertex_program.gpu && !sampled.vertex_program_pinned);
    CHECK(!sampled_pte(vertex_gpu) && !vertex_code[1].residents);
    CHECK(vertex_change(1,false)==INTEL_BCS_OK);vertex_verify(1);
    CHECK(intel_bcs_queue_close_context(&sampled_queue,7,sampled.queue_id)==INTEL_BCS_OK);
    uint64_t old=sampled.queue_id;
    CHECK(vertex_change(2,true)==INTEL_BCS_OK);vertex_verify(2);
    CHECK(sampled.queue_id!=old && sampled.owner==9 && sampled.epoch==78 && !vertex_code[1].residents);
    vertex_cleanup();
}
static void vertex_negative_cases(void)
{
    for (unsigned mode=0;mode<2;mode++) {
        vertex_setup(mode!=0);
        unsigned char *before=malloc(sampled.tables.bytes);CHECK(before);memcpy(before,sampled.tables.cpu,sampled.tables.bytes);
        for (unsigned bad=0;bad<9;bad++) {
            intel_bcs_program_binding_t b=vertex_binding(1);
            switch(bad) {
            case 0:b.buffer=&banks[1][0];break;
            case 1:b.buffer=&banks[1][1];break;
            case 2:b.buffer=&banks[1][2];break;
            case 3:b.buffer=&banks[1][3];break;
            case 4:b.gpu=0x40000000;break;
            case 5:b.gpu++;break;
            case 6:b.gpu=INTEL_PPGTT_VA_LIMIT;break;
            case 7:b.buffer=&sampled.tables;break;
            case 8:b.buffer=&vertex_code[2];break;
            }
            enum intel_bcs_status r=mode ? intel_bcs_context_rebind_vertex(&sampled,&banks[1][0],&banks[1][1],&banks[1][2],b,bindings[1],16) :
                intel_bcs_context_activate_vertex(&sampled,7,&banks[1][0],&banks[1][1],&banks[1][2],b,bindings[1],16);
            CHECK(r==INTEL_BCS_INVALID);CHECK(!memcmp(before,sampled.tables.cpu,sampled.tables.bytes));
            CHECK(!vertex_code[1].residents && !vertex_code[2].residents && !sampled.retained_vertex_program);
        }
        vertex_code[1].busy=1;
        enum intel_bcs_status r=mode ? vertex_change(1,false) : intel_bcs_context_activate_vertex(&sampled,7,&banks[1][0],&banks[1][1],&banks[1][2],vertex_binding(1),bindings[1],16);
        CHECK(r==INTEL_BCS_BUSY);CHECK(!memcmp(before,sampled.tables.cpu,sampled.tables.bytes));vertex_code[1].busy=0;
        free(before);vertex_cleanup();
    }
    vertex_setup(true);fail_operation=operations+1;
    CHECK(vertex_change(1,false)==INTEL_BCS_QUARANTINED);
    CHECK(!sampled.ready && vertex_code[0].residents==1 && vertex_code[1].residents==1);
    CHECK(sampled.retained_vertex_program==&vertex_code[1]);vertex_cleanup();
    vertex_setup(true);ram.fail_sync=ram.syncs+1;
    CHECK(vertex_change(1,false)==INTEL_BCS_QUARANTINED);
    CHECK(!sampled.ready && vertex_code[0].residents==1 && vertex_code[1].residents==1);vertex_cleanup();
}
static void vertex_coherent_cases(void)
{
    vertex_setup(true);
    for (unsigned i=0;i<28;i++) {
        CHECK(intel_gpu_buffer_create(&fence_buffers[i],&allocator,&ram,7,4096)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_coherent(&fence_buffers[i],7,&fences[i])==INTEL_GPU_BUFFER_OK);
        CHECK(intel_bcs_context_bind_coherent(&sampled,&fences[i],0x80000000ULL+i*0x200000,&sampled_policy)==INTEL_BCS_OK);
    }
    vertex_verify(0);CHECK(vertex_change(1,false)==INTEL_BCS_OK);vertex_verify(1);
    for (unsigned i=0;i<28;i++)CHECK(sampled_pte(0x80000000ULL+i*0x200000)==(fence_buffers[i].dma|0x83));
    CHECK(intel_bcs_queue_close_context(&sampled_queue,7,sampled.queue_id)==INTEL_BCS_OK);
    CHECK(vertex_change(2,true)==INTEL_BCS_OK);vertex_verify(2);
    for (unsigned i=0;i<28;i++)CHECK(!sampled_pte(0x80000000ULL+i*0x200000) && !fences[i].references);
    vertex_cleanup();
}
static void vertex_upload(const char *path,intel_gpu_buffer_t *buffer,void *header,unsigned header_bytes)
{
    FILE *f=fopen(path,"rb");CHECK(f && !fseek(f,0,SEEK_END));long bytes=ftell(f);rewind(f);
    CHECK(bytes>256 && bytes<8192);unsigned char *blob=malloc(bytes);CHECK(blob);CHECK(fread(blob,1,bytes,f)==(size_t)bytes);fclose(f);
    struct intel_d3d9_program *p=(void *)blob;unsigned offset=p->code_offset;
    CHECK(offset<4096 && p->code_bytes<=4096);p->code_offset=4096;p->allocation_bytes=4096+p->code_bytes;
    memcpy(buffer->cpu,blob,offset);memcpy((unsigned char *)buffer->cpu+4096,blob+offset,p->code_bytes);memcpy(header,blob,header_bytes);free(blob);
}
static void vertex_prepare_cases(const char *directory)
{
    vertex_setup(false);
    CHECK(intel_bcs_context_activate_vertex(&sampled,7,&banks[0][0],&banks[0][1],&banks[0][2],vertex_binding(0),bindings[0],1)==INTEL_BCS_OK);
    CHECK(intel_bcs_context_attach(&sampled,&sampled_queue,NULL));
    intel_rcs_queue_draw_t work={0};char path[1024];
    snprintf(path,sizeof(path),"%s/ps30-tint.program",directory);vertex_upload(path,&banks[0][2],&work.program,sizeof(work.program));
    vertex_upload("build/native-vertex-pipeline/fixtures/vs30-transform.program",&vertex_code[0],&work.vertex_program,sizeof(work.vertex_program));
    CHECK(intel_gen9_resource_shader_validate(&work.program,banks[0][2].cpu,work.program.allocation_bytes));
    CHECK(intel_gen9_vertex_shader_validate(&work.vertex_program,vertex_code[0].cpu,work.vertex_program.base.allocation_bytes));
    work.primitive=(intel_rcs_primitive_draw_t){.pixel={.batch_gpu=sampled.batch_gpu,.program_gpu=sampled.program_gpu+4096,
        .destination_gpu=sampled.destination_gpu,.destination_bytes=4096,.destination_width=16,.destination_height=16,.destination_pitch=64,
        .width=16,.height=16,.dispatch_mask=1,.constants={.count=work.program.parameter_count}},
        .vertices_gpu=sampled.source_gpu,.vertices_bytes=4096,.vertex_stride=32,.vertex_count=3,.topology=4};
    work.vertex=(intel_rcs_vertex_binding_t){.program_gpu=vertex_gpu+4096,.viewport_width=16,.viewport_height=16,
        .constants={.count=work.vertex_program.base.parameter_count}};
    work.vertex.attribute_type[0]=work.vertex.attribute_type[15]=3;work.vertex.attribute_offset[15]=16;
    work.sampled.bound_mask=1;intel_rcs_texture_binding_t *image=&work.sampled.textures[0];
    CHECK(intel_gen9_image_layout(4,4,1,21,0,&image->image));image->gpu=bindings[0][0].gpu;image->bytes=4096;image->count=1;
    image->sampler=(intel_gen9_sampler_t){1,1,0,3,3,3,0,0,0,0};
    CHECK(intel_bcs_context_acquire(&sampled,8)==INTEL_BCS_OK);
    uint64_t descriptor=0;uint32_t tail=0;
    work.vertex.program_gpu+=4096;CHECK(!intel_bcs_context_prepare_draw(&sampled,&work,8,&descriptor,&tail));work.vertex.program_gpu-=4096;
    CHECK(!descriptor && !tail);
    CHECK(intel_bcs_context_prepare_draw(&sampled,&work,8,&descriptor,&tail));vertex_prepared++;
    CHECK(descriptor && tail==sampled.tail);((uint32_t *)sampled.state.cpu)[1024+5]=tail;
    work.vertex_program.base.abi=0;CHECK(!intel_bcs_context_prepare_draw(&sampled,&work,8,&descriptor,&tail));
    CHECK(intel_bcs_context_retire(&sampled,8)==INTEL_BCS_OK);vertex_cleanup();
}
static void vertex_close_cases(void)
{
    for (unsigned mode=0;mode<6;mode++) {
        vertex_setup(true);vertex_verify(0);
        if (mode==1)ram.fail_sync=ram.syncs+1;
        if (mode==2)fail_operation=operations+1;
        if (mode==3)sampled_queue.worker_busy=true;
        if (mode==4)sampled_queue.active=0;
        if (mode==5)vertex_code[0].busy=1;
        enum intel_bcs_status result=intel_bcs_context_close_bindings(&sampled);
        if (mode==3 || mode==4) {
            CHECK(result==INTEL_BCS_BUSY);vertex_verify(0);
            sampled_queue.worker_busy=false;sampled_queue.active=INTEL_BCS_QUEUE_JOBS;
            result=intel_bcs_context_close_bindings(&sampled);
        }
        if (mode==1 || mode==2 || mode==5) {
            CHECK(result==INTEL_BCS_QUARANTINED && !sampled.ready && vertex_code[0].residents==1);
            vertex_code[0].busy=0;
        } else {
            CHECK(result==INTEL_BCS_OK && sampled.bindings_closed && sampled.ready);
            CHECK(!sampled.source && !sampled.destination && !sampled.program && !sampled.vertex_program.buffer);
            CHECK(!sampled.source_pinned && !sampled.destination_pinned && !vertex_code[0].residents);
            CHECK(!sampled.texture_count && !sampled_pte(vertex_gpu));
            for (unsigned i=0;i<19;i++)CHECK(!banks[0][i].residents);
            CHECK(intel_bcs_context_acquire(&sampled,5)==INTEL_BCS_INVALID);
            uint64_t updates=sampled.rebindings;
            CHECK(intel_bcs_context_close_bindings(&sampled)==INTEL_BCS_OK && sampled.rebindings==updates);
            CHECK(vertex_change(2,true)==INTEL_BCS_OK);vertex_verify(2);
            CHECK(!sampled.bindings_closed && sampled.source_pinned && sampled.destination_pinned);
            CHECK(intel_bcs_context_acquire(&sampled,6)==INTEL_BCS_OK);
            CHECK(intel_bcs_context_retire(&sampled,6)==INTEL_BCS_OK);
        }
        vertex_cleanup();
    }
}
#ifndef INTEL_VERTEX_CONTEXT_ENTRY
#define INTEL_VERTEX_CONTEXT_ENTRY main
#endif
int INTEL_VERTEX_CONTEXT_ENTRY(int argc,char **argv)
{
    CHECK(argc==2);CHECK(sampled_context_main(argc,argv)==0);
    vertex_lifetime_cases();vertex_negative_cases();vertex_coherent_cases();vertex_prepare_cases(argv[1]);
    vertex_close_cases();
    printf("Native vertex context PASS checks=%u prepared=%u independent-code-ownership\n",checks,vertex_prepared);return 0;
}
