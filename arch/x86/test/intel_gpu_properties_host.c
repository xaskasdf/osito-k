#define KMT_SUITE_ENTRY properties_kmt_regression
#include "intel_gpu_kmt_host.c"
static D3DKMT_ALLOCATION_PROPERTIES_WORK properties_work;
int main(void)
{
    CHECK(properties_kmt_regression()==0);
    res_reset();process_closing=false;wake_count=0;
    intel_gpu_memory_t memory={0};intel_gpu_kmt_t provider={0};
    CHECK(intel_gpu_memory_init(&memory,&allocator,&ram,16,16));
    CHECK(intel_gpu_memory_publish(&memory)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_kmt_provider(&provider,&memory,kmt_provider_notify,&wake_count));
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=1000};
    OKMT_LUID luid;OKMT_HANDLE adapter=0,device=0,group=0,handles[3];
    D3DKMT_ALLOCATION_DESCRIPTION input[3]={{5000},{3},{4096}};
    CHECK(d3dkmt_register_memory_driver(&description,&provider.device,&provider.allocation,&luid)==0);
    CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0 && d3dkmt_create_device(7,adapter,0,&device)==0);
    CHECK(d3dkmt_create_resource_allocations(7,device,0,TRUE,3,input,&group,handles)==0);
    memcpy(properties_work.handles,handles,sizeof(handles));
    CHECK(d3dkmt_allocation_properties(7,device,0,3,D3DKMT_PROPERTY_RESIDENCY,&properties_work)==0);
    CHECK(properties_work.values[0]==2 && properties_work.values[1]==2 && properties_work.values[2]==2);
    uint64_t cookies[3];memcpy(cookies,properties_work.cookies,sizeof(cookies));
    D3DKMT_DEVICE_LEASE owner={0};CHECK(d3dkmt_acquire_device(7,device,&owner)==0);
    uint32_t values[3]={0};
    CHECK(intel_gpu_memory_properties(&memory,7,owner.cookie,INTEL_GPU_PROPERTY_GET_PRIORITY,cookies,3,values)==0);
    CHECK(values[0]==0x78000000 && values[1]==0x78000000 && values[2]==0x78000000);
    properties_work.values[0]=0xa0000000;
    CHECK(d3dkmt_allocation_properties(7,device,group,0,D3DKMT_PROPERTY_SET_PRIORITY,&properties_work)==0);
    CHECK(intel_gpu_memory_properties(&memory,7,owner.cookie,INTEL_GPU_PROPERTY_GET_PRIORITY,cookies,3,values)==0);
    CHECK(values[0]==0xa0000000 && values[1]==0xa0000000 && values[2]==0xa0000000);
    uint64_t invalid[3]={cookies[0],cookies[1],0x100000001ULL};
    uint32_t changed[3]={1,2,3};
    CHECK(intel_gpu_memory_properties(&memory,7,owner.cookie,INTEL_GPU_PROPERTY_SET_PRIORITY,invalid,3,changed)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_properties(&memory,7,owner.cookie,INTEL_GPU_PROPERTY_GET_PRIORITY,cookies,3,values)==0);
    CHECK(values[0]==0xa0000000 && values[1]==0xa0000000);
    CHECK(intel_gpu_memory_properties(&memory,8,owner.cookie,INTEL_GPU_PROPERTY_RESIDENCY,cookies,3,values)==INTEL_GPU_BUFFER_INVALID);
    intel_gpu_kmt_cpu_lease_t access={0};CHECK(intel_gpu_kmt_acquire_cpu(7,device,handles[0],&access)==0);
    CHECK(d3dkmt_allocation_properties(7,device,group,0,D3DKMT_PROPERTY_RESIDENCY,&properties_work)==0 && properties_work.values[0]==2);
    CHECK(intel_gpu_kmt_release_cpu(&access)==0);
    CHECK(d3dkmt_release_device(&owner)==0);
    CHECK(d3dkmt_destroy_resource(7,device,group)==0);
    for (unsigned i=0;i<3;i++) CHECK(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_OK);
    CHECK(!memory.pages && ram.allocated==ram.freed);
    d3dkmt_release_process(7);CHECK(d3dkmt_unregister_adapter(luid)==0);
    intel_gpu_memory_shutdown(&memory);CHECK(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_OK);
    res_reset();printf("Intel allocation properties: %u checks passed, native backing and atomic priorities\n",checks);
    return 0;
}
