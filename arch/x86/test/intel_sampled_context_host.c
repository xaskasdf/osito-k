/* Native resource/context code; only DMA allocation and MMIO are modeled. */
#define main sampled_resource_baseline_main
#include "intel_gpu_resource_host.c"
#undef main
#include "../drivers/intel_rcs_sampled.h"
#include "../drivers/intel_gen9_shader.h"

static intel_bcs_context_t sampled;
static intel_gpu_buffer_t banks[3][19],fence_buffers[28];
static intel_bcs_texture_binding_t bindings[3][16];
static intel_gpu_coherent_access_t fences[28];
static intel_ggtt_space_t sampled_space;
static uint8_t sampled_bitmap[8];
static intel_bcs_queue_t sampled_queue;
static intel_bcs_state_t sampled_engine;
static unsigned operations,fail_operation,tlb_writes,interrupt_close,hold_old;
static uint64_t sampled_ns;

static bool sampled_read(void *ctx,uint32_t reg,uint32_t *value)
{
    CHECK(ctx==&sampled && (reg==0x2234 || reg==0x20b8 || reg==0x4260));
    *value=reg==0x2234 ? 1 : 0;
    return ++operations!=fail_operation;
}
static bool sampled_write(void *ctx,uint32_t reg,uint32_t value)
{
    CHECK(ctx==&sampled && reg==0x4260 && value==1);tlb_writes++;
    CHECK(sampled_queue.worker_busy && sampled_queue.contexts[0].updating);
    for (unsigned i=0;i<sampled.texture_count;i++) CHECK(sampled.textures[i].buffer->residents);
    if (interrupt_close) CHECK(intel_bcs_queue_close_context(&sampled_queue,sampled.queue_owner,sampled.queue_id)==INTEL_BCS_OK);
    if (hold_old) banks[0][hold_old+2].busy=1;
    return ++operations!=fail_operation;
}
static uint64_t sampled_now(void *ctx) { CHECK(ctx==&sampled);return sampled_ns+=10000; }
static void sampled_relax(void *ctx) { CHECK(ctx==&sampled); }
static const intel_ppgtt_pat_t sampled_policy={.started=true,.ready=true};

static void sampled_setup(bool activate,bool old_batch)
{
    res_reset();operations=fail_operation=tlb_writes=interrupt_close=hold_old=0;sampled_ns=0;
    memset(&sampled,0,sizeof(sampled));memset(banks,0,sizeof(banks));memset(fence_buffers,0,sizeof(fence_buffers));
    memset(fences,0,sizeof(fences));memset(&sampled_space,0,sizeof(sampled_space));memset(sampled_bitmap,0,sizeof(sampled_bitmap));
    const intel_ggtt_io_t io={res_ggtt_read,res_ggtt_write,res_ggtt_sync};
    const intel_ggtt_layout_t layout={.ggtt_bytes=8ULL<<20,.gpu_va_bytes=1ULL<<32};
    CHECK(intel_ggtt_space_init(&sampled_space,&layout,1,63,sampled_bitmap,sizeof(sampled_bitmap),&io,ggtt)==INTEL_GGTT_BIND_OK);
    for (unsigned bank=0;bank<3;bank++) for (unsigned i=0;i<19;i++) {
        CHECK(intel_gpu_buffer_create(&banks[bank][i],&allocator,&ram,bank==2 ? 9 : 7,i==2 ? 8192 : 4096)==INTEL_GPU_BUFFER_OK);
        if (i>=3) bindings[bank][i-3]=(intel_bcs_texture_binding_t){&banks[bank][i],0x100000000ULL+(uint64_t)(i-3)*0x10000000};
    }
    CHECK((old_batch ? intel_bcs_context_reserve_draw : intel_bcs_context_reserve_sampled)
        (&sampled,&sampled_space,&allocator,&ram,1,0x1001,0x10000000,0x20000000,64,0x10000000,0x30000000,0x40000000));
    CHECK(sampled.batch.bytes==(old_batch ? 8192u : 16384u));
    sampled_engine=(intel_bcs_state_t){.engine=INTEL_CS_RENDER,.enabled=true,.started=true,.status=INTEL_BCS_OK};
    sampled_queue=(intel_bcs_queue_t){.initialized=true,.next_context=1,.next_fence=1,.active=INTEL_BCS_QUEUE_JOBS,
        .engine=&sampled_engine,.io={sampled_read,sampled_write,sampled_now,sampled_relax}};
    if (activate) {
        CHECK(intel_bcs_context_activate_sampled(&sampled,7,&banks[0][0],&banks[0][1],&banks[0][2],bindings[0],16)==INTEL_BCS_OK);
        CHECK(intel_bcs_context_attach(&sampled,&sampled_queue,NULL));
    }
}
static uint64_t sampled_pte(uint64_t gpu)
{
    uint64_t *table=sampled.tables.cpu;
    for (unsigned shift=39;shift>12;shift-=9) {
        uint64_t pte=table[(gpu>>shift)&511];if (!(pte&1))return 0;
        uint64_t dma=pte&~4095ULL;CHECK(dma>=sampled.tables.dma && dma-sampled.tables.dma<sampled.tables.bytes);
        table=(void *)((unsigned char *)sampled.tables.cpu+dma-sampled.tables.dma);
    }
    return table[(gpu>>12)&511];
}
static void sampled_verify(unsigned bank,unsigned count)
{
    CHECK(sampled.ready && sampled.texture_count==count && !sampled.retained_texture_count);
    for (unsigned i=0;i<16;i++) {
        CHECK(sampled_pte(bindings[bank][i].gpu)==(i<count ? banks[bank][i+3].dma|0x19 : 0));
        CHECK(!sampled_pte(bindings[bank][i].gpu+4096));
        if (i<count) CHECK(sampled.textures[i].buffer==&banks[bank][i+3] && banks[bank][i+3].residents==1);
        else CHECK(!sampled.textures[i].buffer);
    }
    CHECK(sampled_pte(sampled.destination_gpu)==(banks[bank][1].dma|0x1b));
    CHECK(sampled_pte(sampled.program_gpu)==(banks[bank][2].dma|0x19));
}
static void sampled_cleanup(void)
{
    ram.fail_sync=ram.fail_release=fail_operation=0;
    for (unsigned b=0;b<3;b++) for (unsigned i=0;i<19;i++) banks[b][i].busy=0;
    if (sampled.access_token) CHECK(intel_bcs_context_retire(&sampled,sampled.access_token)==INTEL_BCS_OK);
    if (sampled.queue) {
        CHECK(!intel_bcs_context_destroy(&sampled));
        CHECK(intel_bcs_queue_close_context(&sampled_queue,sampled.queue_owner,sampled.queue_id)==INTEL_BCS_OK);
        sampled_queue.stopped=true;sampled_engine.retired=true;
    }
    CHECK(intel_bcs_context_destroy(&sampled));
    for (unsigned i=0;i<28;i++) if (fences[i].self) CHECK(intel_gpu_buffer_release_coherent(&fences[i])==INTEL_GPU_BUFFER_OK);
    for (unsigned b=0;b<3;b++) for (unsigned i=0;i<19;i++)
        CHECK(intel_gpu_buffer_close(&banks[b][i],b==2 ? 9 : 7)==INTEL_GPU_BUFFER_OK);
    for (unsigned i=0;i<28;i++) if (fence_buffers[i].started)
        CHECK(intel_gpu_buffer_close(&fence_buffers[i],7)==INTEL_GPU_BUFFER_OK);
    CHECK(ram.allocated==ram.freed);
}
static enum intel_bcs_status sampled_change(unsigned bank,unsigned count,bool retarget)
{
    if (retarget) return intel_bcs_context_retarget_sampled(&sampled,9,77,&banks[bank][0],&banks[bank][1],&banks[bank][2],bindings[bank],count);
    return intel_bcs_context_rebind_sampled(&sampled,&banks[bank][0],&banks[bank][1],&banks[bank][2],bindings[bank],count);
}
static void sampled_access_cases(void)
{
    sampled_setup(true,false);sampled_verify(0,16);
    for (unsigned busy=0;busy<16;busy++) {
        intel_gpu_cpu_access_t cpu={0};
        CHECK(intel_gpu_buffer_acquire_cpu(&banks[0][busy+3],7,&cpu)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_bcs_context_acquire(&sampled,1)==INTEL_BCS_BUSY);
        for (unsigned i=0;i<16;i++) CHECK(!!sampled.texture_access[i].self==(i<busy));
        CHECK(sampled_change(1,16,false)==INTEL_BCS_INVALID && !tlb_writes);
        CHECK(intel_bcs_context_retire(&sampled,1)==INTEL_BCS_OK);
        CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_bcs_context_acquire(&sampled,2)==INTEL_BCS_OK);
        CHECK(intel_gpu_buffer_acquire_cpu(&banks[0][busy+3],7,&cpu)==INTEL_GPU_BUFFER_BUSY);
        banks[0][busy+3].busy=1;CHECK(intel_bcs_context_retire(&sampled,2)==INTEL_BCS_BUSY);
        CHECK(sampled.access_token==2 && sampled.texture_access[busy].self);
        CHECK(!intel_bcs_context_destroy(&sampled));banks[0][busy+3].busy=0;
        CHECK(intel_bcs_context_retire(&sampled,2)==INTEL_BCS_OK);
    }
    /* An identical replacement must release only its extra residency pins. */
    CHECK(sampled_change(0,16,false)==INTEL_BCS_OK);sampled_verify(0,16);
    CHECK(sampled_change(1,9,false)==INTEL_BCS_OK);sampled_verify(1,9);
    for (unsigned i=0;i<19;i++) CHECK(!banks[0][i].residents);
    CHECK(intel_bcs_context_rebind_draw(&sampled,&banks[0][0],&banks[0][1],&banks[0][2])==INTEL_BCS_OK);
    sampled_verify(0,0);
    CHECK(sampled_change(1,16,false)==INTEL_BCS_OK);sampled_verify(1,16);
    CHECK(intel_bcs_queue_close_context(&sampled_queue,7,sampled.queue_id)==INTEL_BCS_OK);
    uint64_t old=sampled.queue_id;
    CHECK(sampled_change(2,16,true)==INTEL_BCS_OK);sampled_verify(2,16);
    CHECK(sampled.owner==9 && sampled.epoch==77 && sampled.queue_owner==9 && sampled.queue_id!=old);
    for (unsigned i=0;i<19;i++) CHECK(!banks[1][i].residents);
    sampled_cleanup();
}
static void sampled_rejection_cases(void)
{
    sampled_setup(true,false);
    unsigned char *saved=malloc(sampled.tables.bytes);CHECK(saved);memcpy(saved,sampled.tables.cpu,sampled.tables.bytes);
    for (unsigned i=0;i<16;i++) {
        banks[1][i+3].busy=1;CHECK(sampled_change(1,16,false)==INTEL_BCS_BUSY);banks[1][i+3].busy=0;
        CHECK(!sampled.retained_texture_count && !memcmp(saved,sampled.tables.cpu,sampled.tables.bytes));
        for (unsigned j=0;j<19;j++) CHECK(!banks[1][j].residents);
    }
    for (unsigned mode=0;mode<10;mode++) {
        intel_bcs_texture_binding_t old=bindings[1][15];uint64_t dma=banks[1][18].dma;
        if (mode==0) bindings[1][15].buffer=&banks[1][0];
        if (mode==1) bindings[1][15].buffer=&banks[1][1];
        if (mode==2) bindings[1][15].buffer=&banks[1][2];
        if (mode==3) bindings[1][15].buffer=bindings[1][0].buffer;
        if (mode==4) bindings[1][15].buffer=&sampled.batch;
        if (mode==5) bindings[1][15].gpu=0;
        if (mode==6) bindings[1][15].gpu++;
        if (mode==7) bindings[1][15].gpu=sampled.destination_gpu;
        if (mode==8) bindings[1][15].buffer=&banks[2][18];
        if (mode==9) banks[1][18].dma=banks[1][3].dma;
        CHECK(sampled_change(1,16,false)==INTEL_BCS_INVALID);
        bindings[1][15]=old;banks[1][18].dma=dma;
        CHECK(!memcmp(saved,sampled.tables.cpu,sampled.tables.bytes) && !tlb_writes && sampled.ready);
        for (unsigned j=0;j<19;j++) CHECK(!banks[1][j].residents);
    }
    CHECK(sampled_change(1,17,false)==INTEL_BCS_INVALID);
    uint64_t capacity=sampled.tables.bytes;sampled.tables.bytes=4096;
    CHECK(sampled_change(1,16,false)==INTEL_BCS_INVALID);sampled.tables.bytes=capacity;
    CHECK(!memcmp(saved,sampled.tables.cpu,sampled.tables.bytes));free(saved);sampled_cleanup();
    sampled_setup(false,true);
    CHECK(intel_bcs_context_activate_sampled(&sampled,7,&banks[0][0],&banks[0][1],&banks[0][2],bindings[0],16)==INTEL_BCS_INVALID);
    CHECK(!sampled.owner && !banks[0][0].residents);sampled_cleanup();
}
static void sampled_failure_cases(void)
{
    /* Four modeled MMIO accesses: idle status, error, TLB write and ack read. */
    for (unsigned retarget=0;retarget<2;retarget++) for (unsigned failure=0;failure<5;failure++) {
        sampled_setup(true,false);unsigned bank=retarget ? 2 : 1;
        if (retarget) CHECK(intel_bcs_queue_close_context(&sampled_queue,7,sampled.queue_id)==INTEL_BCS_OK);
        if (!failure) ram.fail_sync=ram.syncs+1;else fail_operation=failure;
        CHECK(sampled_change(bank,16,retarget)!=INTEL_BCS_OK);
        CHECK(!sampled.ready && sampled.texture_count==16 && sampled.retained_texture_count==16);
        for (unsigned i=0;i<19;i++) CHECK(banks[0][i].residents && banks[bank][i].residents);
        CHECK(sampled.retained_owner==(retarget ? 9u : 0u));sampled_cleanup();
    }
    /* Publication succeeded, then one old texture pin could not be released. */
    sampled_setup(true,false);hold_old=9;
    CHECK(sampled_change(1,16,false)==INTEL_BCS_QUARANTINED);
    CHECK(!sampled.ready && sampled.textures[0].buffer==bindings[1][0].buffer);
    CHECK(banks[0][11].residents && sampled.retained_textures[8]==&banks[0][11]);
    sampled_cleanup();
    sampled_setup(true,false);interrupt_close=1;
    CHECK(sampled_change(1,16,false)==INTEL_BCS_OK && sampled_queue.contexts[0].closing);
    uint64_t fence=0;intel_bcs_copy_t copy={0};
    CHECK(intel_bcs_queue_enqueue(&sampled_queue,7,sampled.queue_id,&copy,&fence)==INTEL_BCS_INVALID);
    sampled_verify(1,16);sampled_cleanup();
    for (unsigned failure=0;failure<16;failure++) {
        sampled_setup(false,false);banks[0][failure+3].busy=1;
        CHECK(intel_bcs_context_activate_sampled(&sampled,7,&banks[0][0],&banks[0][1],&banks[0][2],bindings[0],16)==INTEL_BCS_BUSY);
        CHECK(!sampled.owner && sampled.reserved && !sampled.retained_texture_count);
        banks[0][failure+3].busy=0;
        CHECK(intel_bcs_context_activate_sampled(&sampled,7,&banks[0][0],&banks[0][1],&banks[0][2],bindings[0],16)==INTEL_BCS_OK);
        sampled_verify(0,16);sampled_cleanup();
    }
}
static void sampled_coherent_cases(void)
{
    sampled_setup(true,false);
    for (unsigned i=0;i<28;i++) {
        CHECK(intel_gpu_buffer_create(&fence_buffers[i],&allocator,&ram,7,4096)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_coherent(&fence_buffers[i],7,&fences[i])==INTEL_GPU_BUFFER_OK);
        CHECK(intel_bcs_context_bind_coherent(&sampled,&fences[i],0x80000000ULL+i*0x200000,&sampled_policy)==INTEL_BCS_OK);
    }
    CHECK(sampled_change(1,16,false)==INTEL_BCS_OK);sampled_verify(1,16);
    for (unsigned i=0;i<28;i++) CHECK(sampled_pte(0x80000000ULL+i*0x200000)==(fence_buffers[i].dma|0x83));
    for (unsigned i=1;i<28;i+=2) CHECK(intel_bcs_context_unbind_coherent(&sampled,0x80000000ULL+i*0x200000)==INTEL_BCS_OK);
    sampled_verify(1,16);
    for (unsigned i=0;i<28;i++) CHECK(sampled_pte(0x80000000ULL+i*0x200000)==(i&1 ? 0 : fence_buffers[i].dma|0x83));
    CHECK(intel_bcs_queue_close_context(&sampled_queue,7,sampled.queue_id)==INTEL_BCS_OK);
    CHECK(sampled_change(2,16,true)==INTEL_BCS_OK);sampled_verify(2,16);
    for (unsigned i=0;i<28;i++) CHECK(!sampled_pte(0x80000000ULL+i*0x200000) && !fences[i].references && !fence_buffers[i].residents);
    sampled_cleanup();
}
static void sampled_range_cases(void)
{
    static uint64_t arena[8*512],before[8*512];
    intel_ppgtt_range_t ranges[65];uint32_t pages=0;
    for (unsigned i=0;i<65;i++) ranges[i]=(intel_ppgtt_range_t){0x1000ULL+i*0x2000,0x800000ULL+i*0x2000,4096,i&1};
    const uint64_t mask=(1ULL<<63)|(1ULL<<32)|1;
    CHECK(intel_ppgtt_build_ranges_coherent(arena,sizeof(arena),0x1000000,ranges,64,mask,&pages));
    CHECK(pages==4);
    for (unsigned i=0;i<64;i++) {
        uint64_t *table=arena;
        for (unsigned shift=39;shift>12;shift-=9) table=arena+((table[(ranges[i].gpu>>shift)&511]&~4095ULL)-0x1000000)/8;
        CHECK(table[(ranges[i].gpu>>12)&511]==(ranges[i].dma|((mask>>i)&1 ? 0x81u : 0x19u)|(i&1 ? 2u : 0u)));
    }
    memcpy(before,arena,sizeof(arena));
    CHECK(!intel_ppgtt_build_ranges_coherent(arena,sizeof(arena),0x1000000,ranges,65,mask,&pages));
    CHECK(!intel_ppgtt_build_ranges_coherent(arena,sizeof(arena),0x1000000,ranges,63,mask,&pages));
    ranges[63].dma=ranges[62].dma;
    CHECK(!intel_ppgtt_build_ranges_coherent(arena,sizeof(arena),0x1000000,ranges,64,mask,&pages));
    CHECK(!memcmp(before,arena,sizeof(arena)) && pages==4);
}
static unsigned sampled_prepared;
static void sampled_prepare_cases(const char *directory)
{
    const char *names[]={"ps20-sample","ps30-sample","ps30-sparse","ps30-pair","ps30-dependent",
        "ps30-tint","ps30-coordinate-swizzle","ps30-constant-coordinate","ps30-input-color",
        "ps30-unused-declaration","ps30-dead-sample","ps20-hlsl-sample","ps30-hlsl-pair"};
    sampled_setup(true,false);
    for (unsigned test=0;test<sizeof(names)/sizeof(names[0]);test++) {
        char path[1024];snprintf(path,sizeof(path),"%s/%s.program",directory,names[test]);
        FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long size=ftell(file);rewind(file);
        CHECK(size>=128 && size<65536);unsigned char *blob=malloc((size_t)size);CHECK(blob);
        CHECK(fread(blob,1,(size_t)size,file)==(size_t)size);fclose(file);
        struct intel_d3d9_program original=*(struct intel_d3d9_program *)blob,p=original;
        CHECK(intel_gen9_resource_shader_validate(&original,blob,(size_t)size));
        CHECK(original.code_offset<=4096 && original.code_bytes<=4096);
        p.code_offset=4096;p.allocation_bytes=4096+p.code_bytes;
        intel_gpu_cpu_access_t upload={0};CHECK(intel_gpu_buffer_acquire_cpu(&banks[0][2],7,&upload)==INTEL_GPU_BUFFER_OK);
        memset(upload.cpu,0,banks[0][2].bytes);memcpy(upload.cpu,blob,original.code_offset);
        memcpy((unsigned char *)upload.cpu+4096,blob+original.code_offset,p.code_bytes);
        memcpy(upload.cpu,&p,sizeof(p));
        CHECK(intel_gen9_resource_shader_validate(&p,upload.cpu,p.allocation_bytes));
        CHECK(intel_gpu_buffer_release_cpu(&upload)==INTEL_GPU_BUFFER_OK);free(blob);
        intel_rcs_queue_draw_t work={0};work.program=p;
        work.primitive=(intel_rcs_primitive_draw_t){
            .pixel={.batch_gpu=sampled.batch_gpu,.program_gpu=sampled.program_gpu+4096,
                .destination_gpu=sampled.destination_gpu,.destination_bytes=4096,
                .destination_width=16,.destination_height=16,.destination_pitch=64,
                .width=16,.height=16,.dispatch_mask=p.simd_mask,.constants={.count=p.parameter_count}},
            .vertices_gpu=sampled.source_gpu,.vertices_bytes=4096,.vertex_stride=144,
            .vertex_count=3,.topology=INTEL_RCS_TRIANGLE_LIST};
        work.sampled.bound_mask=p.texture_mask;
        for (unsigned i=0;i<8;i++) work.sampled.texcoord_offset[i]=16+i*16;
        unsigned count=0,first=16;
        for (unsigned i=0;i<16;i++) if (p.texture_mask&(1u<<i)) {
            intel_rcs_texture_binding_t *t=&work.sampled.textures[i];
            if (first==16)first=i;
            t->gpu=bindings[0][count++].gpu;t->bytes=4096;t->count=3;
            CHECK(intel_gen9_image_layout(4,4,3,21,0,&t->image));
            t->sampler=(intel_gen9_sampler_t){1,1,0,3,3,3,0,0,0,0};
        }
        CHECK(sampled_change(0,count,false)==INTEL_BCS_OK);
        CHECK(intel_bcs_context_acquire(&sampled,123)==INTEL_BCS_OK);
        uint64_t descriptor=0;uint32_t tail=0;
        if (count) {
            uint64_t before=work.sampled.textures[first].gpu;work.sampled.textures[first].gpu+=4096;
            CHECK(!intel_bcs_context_prepare_draw(&sampled,&work,123,&descriptor,&tail) && !descriptor && !tail);
            work.sampled.textures[first].gpu=before;
            work.sampled.textures[first].bytes=8192;
            CHECK(!intel_bcs_context_prepare_draw(&sampled,&work,123,&descriptor,&tail));
            work.sampled.textures[first].bytes=4096;
        }
        for (unsigned dispatch=1;dispatch<8;dispatch++) if (!(dispatch&~p.simd_mask)) {
            work.primitive.pixel.dispatch_mask=dispatch;
            CHECK(intel_bcs_context_prepare_draw(&sampled,&work,123,&descriptor,&tail));
            CHECK(descriptor && tail==sampled.tail);sampled_prepared++;
            /* A real engine writes this saved head after completing the job. */
            ((uint32_t *)sampled.state.cpu)[1024+5]=tail;
        }
        CHECK(intel_bcs_context_retire(&sampled,123)==INTEL_BCS_OK);
        if (count>1) {
            /* Multiple sampler slots may read the same uniquely retained image. */
            for (unsigned i=0;i<16;i++) if (p.texture_mask&(1u<<i)) work.sampled.textures[i].gpu=bindings[0][0].gpu;
            CHECK(sampled_change(0,1,false)==INTEL_BCS_OK);
            CHECK(intel_bcs_context_acquire(&sampled,124)==INTEL_BCS_OK);
            CHECK(intel_bcs_context_prepare_draw(&sampled,&work,124,&descriptor,&tail));sampled_prepared++;
            ((uint32_t *)sampled.state.cpu)[1024+5]=tail;
            CHECK(intel_bcs_context_retire(&sampled,124)==INTEL_BCS_OK);
        }
    }
    sampled_cleanup();
}
#ifndef INTEL_SAMPLED_CONTEXT_EMBED
#define sampled_context_main main
#endif
int sampled_context_main(int argc,char **argv)
{
    CHECK(argc==2);
    sampled_range_cases();sampled_access_cases();sampled_rejection_cases();sampled_failure_cases();sampled_coherent_cases();
    sampled_prepare_cases(argv[1]);
    printf("Native sampled context PASS checks=%u prepared=%u compiled_programs=13\n",checks,sampled_prepared);return 0;
}
