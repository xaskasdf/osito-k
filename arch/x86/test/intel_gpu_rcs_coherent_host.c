/* Fill all 32 table-builder ranges on the native render-context path. */
#define INTEL_RCS_OWNER_SUITE_ENTRY rcs_coherent_regression_main
#include "intel_gpu_rcs_owner_host.c"

static void rcs_coherent_maps(void)
{
    RENDER_OWNER_FIXTURE f={0};OWNER_FIXTURE *b=&f.base;
    jobs_setup(&b->jobs);bcs_model_reset();b->pat=0x03112233;
    f.cache[0]=0x11;f.cache[1]=0x22;f.cache[2]=0x33445566;
    b->jobs.jobs.ready=intel_gpu_executor_notify;
    const intel_ggtt_io_t ggtt_io={res_ggtt_read,res_ggtt_write,res_ggtt_sync};
    const intel_ggtt_layout_t layout={.ggtt_bytes=8ULL<<20,.gpu_va_bytes=1ULL<<32};
    CHECK(intel_ggtt_space_init(&b->space,&layout,1,63,f.bitmap,sizeof(f.bitmap),&ggtt_io,ggtt)==INTEL_GGTT_BIND_OK);
    intel_gpu_bcs_config_t config={.space=&b->space,.allocator=allocator,.allocator_context=&ram,
        .io={render_owner_read,render_owner_write,owner_now,owner_relax},.hardware_context=&f,
        .token_read=render_owner_token,.mapping_stopped=owner_mapping,.ggtt_limit=0x10000000,
        .source_gpu=0x10000000,.destination_gpu=0x20000000,.batch_gpu=0x30000000,
        .program_gpu=0x40000000,.render=true,.first_fence=0x11223344,.first_hardware_id=0x1001};
    CHECK(intel_gpu_bcs_owner_init(&b->owner,&b->jobs.jobs,&config)==0);
    CHECK(intel_gpu_bcs_owner_reserve(&b->owner,1,16)==0);
    intel_bcs_context_t *c=&b->owner.slots[0].native;
    intel_gpu_buffer_t normal[3]={0},coherent[INTEL_BCS_COHERENT_RANGES]={0};
    intel_gpu_coherent_access_t access[INTEL_BCS_COHERENT_RANGES]={0};
    for (unsigned i=0;i<3;i++) CHECK(intel_gpu_buffer_create(&normal[i],&allocator,&ram,7,4096)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_bcs_context_activate_draw(c,7,&normal[0],&normal[1],&normal[2])==INTEL_BCS_OK);
    CHECK(intel_gpu_bcs_owner_start(&b->owner)==0);
    const intel_ppgtt_pat_t policy={.started=true,.ready=true};
    for (unsigned i=0;i<INTEL_BCS_COHERENT_RANGES;i++) {
        CHECK(intel_gpu_buffer_create(&coherent[i],&allocator,&ram,7,4096)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_coherent(&coherent[i],7,&access[i])==INTEL_GPU_BUFFER_OK);
        CHECK(intel_bcs_context_bind_coherent(c,&access[i],0x400000000ULL+4096*(27-i),&policy)==INTEL_BCS_OK);
    }
    CHECK(intel_bcs_context_rebind_draw(c,&normal[1],&normal[0],&normal[2])==INTEL_BCS_OK);
    for (unsigned i=0;i<32;i++) {
        uint64_t gpu=i<28 ? 0x400000000ULL+4096*(27-i) :
            i==28 ? c->source_gpu : i==29 ? c->destination_gpu : i==30 ? c->batch_gpu : c->program_gpu;
        uint64_t expected=i<28 ? access[i].dma|0x83 : i==28 ? normal[1].dma|0x19 :
            i==29 ? normal[0].dma|0x1b : i==30 ? c->batch.dma|0x19 : normal[2].dma|0x19;
        uint64_t *table=c->tables.cpu;
        for (unsigned shift=39;shift>12;shift-=9) {
            uint64_t entry=table[(gpu>>shift)&511],dma=entry&~4095ULL;
            CHECK((entry&1) && dma>=c->tables.dma && dma-c->tables.dma<c->tables.bytes);
            table=(uint64_t *)((unsigned char *)c->tables.cpu+dma-c->tables.dma);
        }
        CHECK(table[(gpu>>12)&511]==expected);
    }
    for (unsigned i=0;i<28;i++) {
        CHECK(intel_bcs_context_unbind_coherent(c,0x400000000ULL+4096*(27-i))==INTEL_BCS_OK);
        CHECK(!access[i].references && !coherent[i].residents);
        CHECK(intel_gpu_buffer_release_coherent(&access[i])==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_close(&coherent[i],7)==INTEL_GPU_BUFFER_OK);
    }
    CHECK(f.invalidations==57 && c->rebindings==57);
    NTSTATUS closed=D3DKMT_STATUS_DEVICE_BUSY;
    for (unsigned i=0;i<16 && closed;i++) { closed=intel_gpu_bcs_owner_close(&b->owner);executor_step();service_step(); }
    CHECK(!closed);
    for (unsigned i=0;i<3;i++) CHECK(intel_gpu_buffer_close(&normal[i],7)==INTEL_GPU_BUFFER_OK);
    owner_cleanup(b);
}
int main(void)
{
    if (rcs_coherent_regression_main()) return 1;
    unsigned before=checks;rcs_coherent_maps();
    printf("Intel RCS coherent ranges: %u checks passed, 32 ranges, 57 TLB acknowledgments\n",checks-before);
    return 0;
}
