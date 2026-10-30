#define INTEL_PAGING_SUITE_ENTRY residency_regression_main
#include "intel_gpu_paging_host.c"

typedef struct { intel_gpu_memory_t *memory;uint32_t device;uint64_t cookies[2]; } RESIDENCY_RACE;
static void *residency_parallel(void *argument)
{
 RESIDENCY_RACE *r=argument;
 for(unsigned i=0;i<1000;i++) {
  CHECK(intel_gpu_memory_residency(r->memory,7,r->device,true,r->cookies,2)==INTEL_GPU_BUFFER_OK);
  CHECK(intel_gpu_memory_residency(r->memory,7,r->device,false,r->cookies,2)==INTEL_GPU_BUFFER_OK);
 }
 return NULL;
}
static void residency_native_cases(void)
{
 res_reset();intel_gpu_memory_t m={0};uint32_t device,a,b,other;
 CHECK(intel_gpu_memory_init(&m,&allocator,&ram,64,64));
 CHECK(intel_gpu_memory_open_device(&m,7,&device)==INTEL_GPU_BUFFER_OK);
 CHECK(intel_gpu_memory_open_device(&m,8,&other)==INTEL_GPU_BUFFER_OK);
 CHECK(intel_gpu_memory_create(&m,7,device,4096,&a)==INTEL_GPU_BUFFER_OK);
 CHECK(intel_gpu_memory_create(&m,7,device,8192,&b)==INTEL_GPU_BUFFER_OK);
 intel_gpu_memory_resource_t *ra=NULL,*rb=NULL;
 for(unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) {
  if(m.resources[i].id==a)ra=&m.resources[i];
  if(m.resources[i].id==b)rb=&m.resources[i];
 }
 CHECK(ra && rb && ra->residency_refs==1 && rb->residency_refs==1);
 uint64_t list[1024];for(unsigned i=0;i<1024;i++)list[i]=i&1?a:b;
 CHECK(intel_gpu_memory_residency(&m,7,device,true,list,1024)==INTEL_GPU_BUFFER_OK);
 CHECK(ra->residency_refs==513 && rb->residency_refs==513);
 CHECK(intel_gpu_memory_residency(&m,7,device,false,list,1024)==INTEL_GPU_BUFFER_OK);
 CHECK(ra->residency_refs==1 && rb->residency_refs==1);
 CHECK(intel_gpu_memory_residency(&m,7,device,true,NULL,1)==INTEL_GPU_BUFFER_INVALID);
 CHECK(intel_gpu_memory_residency(&m,7,device,true,list,0)==INTEL_GPU_BUFFER_INVALID);
 CHECK(intel_gpu_memory_residency(&m,7,device,true,list,1025)==INTEL_GPU_BUFFER_INVALID);
 CHECK(intel_gpu_memory_residency(&m,8,device,true,list,2)==INTEL_GPU_BUFFER_INVALID);
 CHECK(intel_gpu_memory_residency(&m,8,other,true,list,2)==INTEL_GPU_BUFFER_INVALID);
 for(unsigned make=0;make<2;make++) {
  uint64_t mixed[]={a,b,0x100000000ULL|a};
  CHECK(intel_gpu_memory_residency(&m,7,device,make,mixed,3)==INTEL_GPU_BUFFER_INVALID);
  CHECK(ra->residency_refs==1 && rb->residency_refs==1);
  rb->busy=true;
  CHECK(intel_gpu_memory_residency(&m,7,device,make,list,2)==INTEL_GPU_BUFFER_BUSY);
  CHECK(ra->residency_refs==1 && rb->residency_refs==1);rb->busy=false;
 }
 ra->residency_refs=UINT32_MAX-1;uint64_t overflow[]={b,a,a};
 CHECK(intel_gpu_memory_residency(&m,7,device,true,overflow,3)==INTEL_GPU_BUFFER_NO_MEMORY);
 CHECK(ra->residency_refs==UINT32_MAX-1 && rb->residency_refs==1);ra->residency_refs=1;
 RESIDENCY_RACE race={&m,device,{a,b}};pthread_t threads[4];
 for(unsigned i=0;i<4;i++)CHECK(!pthread_create(&threads[i],NULL,residency_parallel,&race));
 for(unsigned i=0;i<4;i++)CHECK(!pthread_join(threads[i],NULL));
 CHECK(ra->residency_refs==1 && rb->residency_refs==1);
 intel_gpu_memory_resident_lease_t capture={0},queued={0},denied={0};
 CHECK(intel_gpu_memory_acquire_resident(&m,7,device,a,&capture)==INTEL_GPU_BUFFER_OK);
 CHECK(intel_gpu_memory_acquire_resident(&m,7,device,b,&queued)==INTEL_GPU_BUFFER_OK);
 CHECK(intel_gpu_memory_reserve_work(&queued)==INTEL_GPU_BUFFER_OK);
 CHECK(intel_gpu_memory_residency(&m,7,device,false,race.cookies,2)==INTEL_GPU_BUFFER_OK);
 CHECK(!ra->residency_refs && !rb->residency_refs && rb->pending_work==1 && !ram.freed);
 CHECK(intel_gpu_memory_acquire_resident(&m,7,device,a,&denied)==INTEL_GPU_BUFFER_NOT_RESIDENT && !denied.self);
 CHECK(intel_gpu_memory_reserve_work(&capture)==INTEL_GPU_BUFFER_NOT_RESIDENT && !capture.pending_work);
 CHECK(intel_gpu_memory_residency(&m,7,device,false,race.cookies,2)==INTEL_GPU_BUFFER_OK);
 CHECK(!ra->residency_refs && !rb->residency_refs);
 uint32_t location[2]={0};CHECK(intel_gpu_memory_properties(&m,7,device,INTEL_GPU_PROPERTY_RESIDENCY,race.cookies,2,location)==INTEL_GPU_BUFFER_OK);
 CHECK(location[0]==2 && location[1]==2 && m.pages==3); /* still physically pinned RAM */
 CHECK(intel_gpu_memory_residency(&m,7,device,true,race.cookies,2)==INTEL_GPU_BUFFER_OK);
 CHECK(intel_gpu_memory_reserve_work(&capture)==INTEL_GPU_BUFFER_OK);
 CHECK(intel_gpu_memory_close_device(&m,7,device)==INTEL_GPU_BUFFER_OK);
 CHECK(intel_gpu_memory_residency(&m,7,device,true,race.cookies,2)==INTEL_GPU_BUFFER_INVALID);
 CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
 CHECK(intel_gpu_memory_release_resident(&capture)==INTEL_GPU_BUFFER_OK);
 CHECK(intel_gpu_memory_release_resident(&queued)==INTEL_GPU_BUFFER_OK);
 for(unsigned i=0;i<4;i++)(void)intel_gpu_memory_collect(&m);
 CHECK(!m.pages && ram.freed==2);
 CHECK(intel_gpu_memory_close_device(&m,8,other)==INTEL_GPU_BUFFER_OK);
 CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK);res_reset();
}
static void residency_kmt_fault_case(void)
{
 res_reset();process_closing=false;intel_gpu_memory_t memory={0};intel_gpu_kmt_t provider={0};
 CHECK(intel_gpu_memory_init(&memory,&allocator,&ram,64,64));
 CHECK(intel_gpu_memory_publish(&memory)==INTEL_GPU_BUFFER_OK);
 CHECK(intel_gpu_kmt_provider(&provider,&memory,kmt_provider_notify,&wake_count));
 D3DKMT_ADAPTER_DESCRIPTION desc={.driver_version=3200};OKMT_LUID luid;OKMT_HANDLE adapter,device,allocation;
 CHECK(d3dkmt_register_memory_driver(&desc,&provider.device,&provider.allocation,&luid)==0);
 CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0 && d3dkmt_create_device(7,adapter,0,&device)==0);
 D3DKMT_ALLOCATION_DESCRIPTION bytes={4096};CHECK(d3dkmt_create_allocations(7,device,1,&bytes,&allocation)==0);
 D3DKMT_ALLOCATION_LEASE held={0};CHECK(d3dkmt_acquire_allocation(7,device,allocation,&held)==0);
 CHECK(intel_gpu_memory_residency(&memory,7,(uint32_t)held.device_cookie,false,&held.cookie,1)==INTEL_GPU_BUFFER_OK);
 intel_gpu_kmt_resident_lease_t denied={0};
 CHECK(intel_gpu_kmt_acquire_resident(7,device,allocation,&denied)==D3DKMT_STATUS_DEVICE_ERROR);
 CHECK(!denied.reference.self && !denied.native.self);
 DWORD state=0;CHECK(d3dkmt_query_device_execution(7,device,&state)==0 && state==OKMT_EXECUTION_DMA_FAULT);
 CHECK(d3dkmt_release_allocation(&held)==0);d3dkmt_release_process(7);
 CHECK(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_OK && !memory.pages);
 CHECK(d3dkmt_unregister_adapter(luid)==0);
 intel_gpu_memory_shutdown(&memory);CHECK(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_OK);
 res_reset();
}
static void residency_provider_cases(void)
{
 for(unsigned action=0;action<7;action++) {
  res_reset();model_pid=7;process_closing=false;CHECK(intel_gfx_driver_start()==0);
  OKMT_ADAPTER_INFO adapter;DWORD count=0;
  CHECK(d3dkmt_enumerate_adapters(7,1,&adapter,&count)==0 && count==1);
  OKMT_HANDLE device=0,queue=0,sync=0,objects[2];uint64_t cpu=0;
  CHECK(d3dkmt_create_device(7,adapter.adapter,0,&device)==0);
  CHECK(d3dkmt_create_paging_queue(7,device,0,FALSE,&queue,&sync,&cpu)==0);
  D3DKMT_ALLOCATION_DESCRIPTION sizes[2]={{4096},{8192}};
  CHECK(d3dkmt_create_allocations(7,device,2,sizes,objects)==0);
  intel_gpu_kmt_device_lease_t binding={0};CHECK(intel_gpu_kmt_acquire_device(7,device,&binding)==0);
  D3DKMT_ALLOCATION_LEASE allocation={0};CHECK(d3dkmt_acquire_allocation(7,device,objects[0],&allocation)==0);
  intel_gpu_memory_resource_t *resource=NULL;
  for(unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)if(binding.memory->resources[i].id==allocation.cookie)resource=&binding.memory->resources[i];
  CHECK(resource && resource->residency_refs==1);CHECK(d3dkmt_release_allocation(&allocation)==0);
  D3DKMT_RESIDENCY_WORK *work=calloc(1,sizeof(*work));CHECK(work);
  work->handles[0]=work->handles[1]=objects[0];work->handles[2]=objects[1];
  CHECK(d3dkmt_residency_prepare(7,queue,TRUE,3,work)==0);
  if(action==1)CHECK(d3dkmt_destroy_paging_queue(7,queue)==0);
  if(action==2)CHECK(d3dkmt_destroy_allocations(7,device,1,objects)==0);
  if(action==3)CHECK(d3dkmt_destroy_device(7,device)==0);
  if(action==4){vm_model_exit(7);d3dkmt_release_process(7);}
  if(action==5)CHECK(d3dkmt_unregister_adapter(adapter.luid)==D3DKMT_STATUS_DEVICE_BUSY);
  if(action==6)CHECK(d3dkmt_mark_device_error(7,device,6)==0);
  CHECK((d3dkmt_residency_commit(work)==0)==(action==0));
  CHECK(resource->residency_refs==(action?1:3));
  CHECK(d3dkmt_residency_commit(work)==STATUS_INVALID_PARAMETER);
  CHECK(d3dkmt_residency_release(work)==0 && !work->self);
  if(!action){
   CHECK(d3dkmt_residency_prepare(7,device,FALSE,3,work)==0);
   CHECK(d3dkmt_residency_commit(work)==0 && resource->residency_refs==1);
   CHECK(d3dkmt_residency_release(work)==0);
  }
  free(work);CHECK(intel_gpu_kmt_release_device(&binding)==0);
  vm_model_exit(7);d3dkmt_release_process(7);NTSTATUS stopped=D3DKMT_STATUS_DEVICE_BUSY;
  for(unsigned i=0;i<64 && stopped;i++){executor_step();service_step();stopped=intel_gfx_driver_stop();}
  CHECK(stopped==0 && !vm_count && intel_gfx_capture_clean() && intel_gpu_executor_quiescent());
 }
}
int main(void)
{
 if(residency_regression_main())return 1;
 unsigned before=checks;residency_native_cases();residency_kmt_fault_case();residency_provider_cases();
 printf("Intel residency requirements: %u checks passed, atomic batches, 4000 concurrent pairs, retained DMA and device fault\n",checks-before);
 return 0;
}
