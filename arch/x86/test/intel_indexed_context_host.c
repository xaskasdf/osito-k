/* Mapping lifetime, independent index ownership and faulted retirement. */
#define INTEL_VERTEX_CONTEXT_ENTRY indexed_vertex_context_baseline
#include "intel_vertex_context_host.c"
static intel_gpu_buffer_t index_buffers[3];
static const uint64_t index_gpu=0x50000000;
static intel_bcs_index_binding_t index_binding(unsigned bank)
{ return (intel_bcs_index_binding_t){&index_buffers[bank],index_gpu}; }
static void index_setup(bool activate)
{
    vertex_setup(false);memset(index_buffers,0,sizeof(index_buffers));
    for(unsigned i=0;i<3;i++)CHECK(intel_gpu_buffer_create(&index_buffers[i],&allocator,&ram,i==2 ? 9 : 7,8192)==INTEL_GPU_BUFFER_OK);
    if(activate) {
        CHECK(intel_bcs_context_activate_indexed(&sampled,7,&banks[0][0],&banks[0][1],&banks[0][2],vertex_binding(0),index_binding(0),bindings[0],16)==INTEL_BCS_OK);
        CHECK(intel_bcs_context_attach(&sampled,&sampled_queue,NULL));
    }
}
static void index_cleanup(void)
{
    fail_operation=ram.fail_sync=ram.fail_release=0;
    for(unsigned i=0;i<3;i++)index_buffers[i].busy=0;
    if(sampled.access_token)CHECK(intel_bcs_context_retire(&sampled,sampled.access_token)==INTEL_BCS_OK);
    if(sampled.queue) {
        CHECK(intel_bcs_queue_close_context(&sampled_queue,sampled.queue_owner,sampled.queue_id)==INTEL_BCS_OK);
        sampled_queue.stopped=true;sampled_engine.retired=true;
    }
    CHECK(intel_bcs_context_destroy(&sampled));
    for(unsigned i=0;i<3;i++)CHECK(intel_gpu_buffer_close(&index_buffers[i],i==2 ? 9 : 7)==INTEL_GPU_BUFFER_OK);
    vertex_cleanup();
    CHECK(ram.allocated==ram.freed);
}
static enum intel_bcs_status index_change(unsigned bank,bool retarget)
{
    return retarget ? intel_bcs_context_retarget_indexed(&sampled,9,79,&banks[bank][0],&banks[bank][1],&banks[bank][2],vertex_binding(bank),index_binding(bank),bindings[bank],16) :
        intel_bcs_context_rebind_indexed(&sampled,&banks[bank][0],&banks[bank][1],&banks[bank][2],vertex_binding(bank),index_binding(bank),bindings[bank],16);
}
static void index_verify(unsigned bank)
{
    vertex_verify(bank);
    CHECK(sampled.indices.buffer==&index_buffers[bank] && sampled.indices.gpu==index_gpu && sampled.indices_pinned);
    CHECK(!sampled.retained_indices && index_buffers[bank].residents==1);
    CHECK(sampled_pte(index_gpu)==(index_buffers[bank].dma|0x19));
    CHECK(sampled_pte(index_gpu+4096)==((index_buffers[bank].dma+4096)|0x19));
    CHECK(!sampled_pte(index_gpu+8192));
}
static void index_lifetime_cases(void)
{
    index_setup(true);index_verify(0);intel_gpu_cpu_access_t cpu={0};
    CHECK(intel_gpu_buffer_acquire_cpu(&index_buffers[0],7,&cpu)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_bcs_context_acquire(&sampled,1)==INTEL_BCS_BUSY);
    CHECK(sampled.vertex_program_access.self && !sampled.index_access.self && !sampled.texture_access[0].self);
    CHECK(index_change(1,false)==INTEL_BCS_INVALID);
    CHECK(intel_bcs_context_retire(&sampled,1)==INTEL_BCS_OK);
    CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_bcs_context_acquire(&sampled,2)==INTEL_BCS_OK);
    CHECK(intel_gpu_buffer_acquire_cpu(&index_buffers[0],7,&cpu)==INTEL_GPU_BUFFER_BUSY);
    index_buffers[0].busy=1;CHECK(intel_bcs_context_retire(&sampled,2)==INTEL_BCS_BUSY && sampled.index_access.self);
    CHECK(!intel_bcs_context_destroy(&sampled));index_buffers[0].busy=0;
    CHECK(intel_bcs_context_retire(&sampled,2)==INTEL_BCS_OK);
    CHECK(index_change(0,false)==INTEL_BCS_OK);index_verify(0);
    CHECK(index_change(1,false)==INTEL_BCS_OK);index_verify(1);CHECK(!index_buffers[0].residents);
    CHECK(vertex_change(0,false)==INTEL_BCS_OK);vertex_verify(0);
    CHECK(!sampled.indices.buffer && !sampled.indices.gpu && !sampled.indices_pinned);
    CHECK(!sampled_pte(index_gpu) && !index_buffers[1].residents);
    CHECK(index_change(1,false)==INTEL_BCS_OK);index_verify(1);
    CHECK(intel_bcs_context_close_bindings(&sampled)==INTEL_BCS_OK);
    CHECK(sampled.bindings_closed && !sampled.indices.buffer && !index_buffers[1].residents && !sampled_pte(index_gpu));
    CHECK(index_change(2,true)==INTEL_BCS_OK);index_verify(2);CHECK(sampled.owner==9 && sampled.epoch==79);
    index_cleanup();
}
static void index_negative_cases(void)
{
    for(unsigned mode=0;mode<2;mode++) {
        index_setup(mode!=0);
        unsigned char *before=malloc(sampled.tables.bytes);CHECK(before);memcpy(before,sampled.tables.cpu,sampled.tables.bytes);
        for(unsigned bad=0;bad<12;bad++) {
            intel_bcs_index_binding_t b=index_binding(1);
            switch(bad) {
            case 0:b.buffer=&banks[1][0];break;
            case 1:b.buffer=&banks[1][1];break;
            case 2:b.buffer=&banks[1][2];break;
            case 3:b.buffer=&banks[1][3];break;
            case 4:b.buffer=&vertex_code[1];break;
            case 5:b.gpu=vertex_gpu;break;
            case 6:b.gpu++;break;
            case 7:b.gpu=INTEL_PPGTT_VA_LIMIT;break;
            case 8:b.buffer=&sampled.tables;break;
            case 9:b.buffer=&index_buffers[2];break;
            case 10:b.gpu=0x40000000;break;
            case 11:b.buffer=NULL;break;
            }
            enum intel_bcs_status r=mode ? intel_bcs_context_rebind_indexed(&sampled,&banks[1][0],&banks[1][1],&banks[1][2],vertex_binding(1),b,bindings[1],16) :
                intel_bcs_context_activate_indexed(&sampled,7,&banks[1][0],&banks[1][1],&banks[1][2],vertex_binding(1),b,bindings[1],16);
            CHECK(r==INTEL_BCS_INVALID && !memcmp(before,sampled.tables.cpu,sampled.tables.bytes));
            CHECK(!index_buffers[1].residents && !index_buffers[2].residents && !sampled.retained_indices);
        }
        free(before);index_cleanup();
    }
    for(unsigned mode=0;mode<5;mode++) {
        index_setup(true);
        if(mode==0 || mode==2)fail_operation=operations+1;
        if(mode==1 || mode==3)ram.fail_sync=ram.syncs+1;
        if(mode==4)index_buffers[0].busy=1;
        enum intel_bcs_status r=mode<2 ? index_change(1,false) : intel_bcs_context_close_bindings(&sampled);
        CHECK(r==INTEL_BCS_QUARANTINED && !sampled.ready && index_buffers[0].residents==1);
        if(mode<2)CHECK(index_buffers[1].residents==1 && sampled.retained_indices==&index_buffers[1]);
        index_cleanup();
    }
}
static void index_coherent_cases(void)
{
    index_setup(true);
    for(unsigned i=0;i<28;i++) {
        CHECK(intel_gpu_buffer_create(&fence_buffers[i],&allocator,&ram,7,4096)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_coherent(&fence_buffers[i],7,&fences[i])==INTEL_GPU_BUFFER_OK);
        CHECK(intel_bcs_context_bind_coherent(&sampled,&fences[i],0x80000000ULL+i*0x200000,&sampled_policy)==INTEL_BCS_OK);
    }
    CHECK(index_change(1,false)==INTEL_BCS_OK);index_verify(1);
    for(unsigned i=0;i<28;i++)CHECK(sampled_pte(0x80000000ULL+i*0x200000)==(fence_buffers[i].dma|0x83));
    CHECK(intel_bcs_queue_close_context(&sampled_queue,7,sampled.queue_id)==INTEL_BCS_OK);
    CHECK(index_change(2,true)==INTEL_BCS_OK);index_verify(2);
    for(unsigned i=0;i<28;i++)CHECK(!sampled_pte(0x80000000ULL+i*0x200000) && !fences[i].references);
    index_cleanup();
}
int main(int argc,char **argv)
{
    CHECK(indexed_vertex_context_baseline(argc,argv)==0);
    index_lifetime_cases();index_negative_cases();index_coherent_cases();
    printf("Native indexed context PASS checks=%u independent-index-ownership\n",checks);return 0;
}
