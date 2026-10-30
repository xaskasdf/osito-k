#define INTEL_OWNER_FENCE_SUITE_ENTRY paging_native_regression_main
#include "intel_gpu_owner_fences_host.c"

static void paging_native_cases(void)
{
 for(unsigned compat=0;compat<2;compat++)for(unsigned mode=0;mode<3;mode++) {
  res_reset();model_pid=7;process_closing=false;
  CHECK(intel_gfx_driver_start()==0);
  OKMT_ADAPTER_INFO adapter;DWORD count=0;CHECK(d3dkmt_enumerate_adapters(7,1,&adapter,&count)==0 && count==1);
  OKMT_HANDLE device=0,context=0,queue=0,object=0;
  CHECK(d3dkmt_create_device(7,adapter.adapter,0,&device)==0);
  D3DKMT_CONTEXT_DESCRIPTION desc={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
  memcpy(desc.private_data,&command_layout,sizeof(command_layout));D3DKMT_CONTEXT_BUFFERS buffers={0};
  CHECK(d3dkmt_create_context(7,device,&desc,&context,&buffers)==0);
  unsigned readonly=vm_readonly_maps;uint64_t cpu=0;
  CHECK(d3dkmt_create_paging_queue(7,device,(LONG)mode-1,compat,&queue,&object,&cpu)==0);
  CHECK(queue && object && queue!=object && cpu && (compat?cpu<=UINT32_MAX:cpu>UINT32_MAX));
  CHECK(vm_readonly_maps==readonly+!compat);
  intel_gpu_kmt_context_lease_t parent={0};CHECK(intel_gpu_kmt_acquire_context(7,context,&parent)==0);
  intel_gpu_fences_t *fences=parent.native.owner->fences;intel_gpu_fence_lease_t backing={0};
  CHECK(intel_gpu_fence_acquire(fences,7,parent.native.device,1,&backing)==0);
  CHECK(__atomic_load_n((uint64_t *)backing.cpu,__ATOMIC_ACQUIRE)==0);
  CHECK(intel_gpu_kmt_release_context(&parent)==0);
  uint64_t value=9;CHECK(d3dkmt_signal_sync_cpu(7,device,1,&object,&value,FALSE)==STATUS_ACCESS_DENIED);
  D3DKMT_SYNC_COMMAND wait={.count=1,.objects={object},.value=UINT64_MAX,.extended=TRUE};
  D3DKMT_SYNC_LEASE retained={0};CHECK(d3dkmt_sync_capture(7,context,&wait,&retained)==0);
  unsigned publication=D3DKMT_WORK_QUEUED;
  if(mode==0) {
   CHECK(d3dkmt_destroy_paging_queue(7,queue)==0);
   CHECK(d3dkmt_sync_step(&retained,&publication)==0);
  } else if(mode==1) {
   CHECK(d3dkmt_destroy_device(7,device)==0);
   CHECK(d3dkmt_sync_step(&retained,&publication)!=0);
  } else {
   vm_model_exit(7);d3dkmt_release_process(7);
   CHECK(d3dkmt_sync_step(&retained,&publication)!=0);
  }
  CHECK(d3dkmt_sync_release(&retained)==0);
  for(unsigned i=0;i<8;i++){executor_step();service_step();}
  CHECK(intel_gpu_fence_closed(&backing));
  CHECK(__atomic_load_n((uint64_t *)backing.cpu,__ATOMIC_ACQUIRE)==(mode==0?UINT64_MAX:0));
  CHECK(intel_gpu_fence_release(&backing)==0);
  vm_model_exit(7);d3dkmt_release_process(7);
  NTSTATUS stopped=D3DKMT_STATUS_DEVICE_BUSY;
  for(unsigned i=0;i<64 && stopped;i++) {
   executor_step();service_step();stopped=intel_gfx_driver_stop();
  }
  CHECK(stopped==0 && !vm_count && intel_gfx_capture_clean() && intel_gpu_executor_quiescent());
 }
}
/* An unused fence has no CPU/GPU lease-release path to collect it for us.
 * Only production service scheduling may finish device/process teardown. */
static void paging_native_abandon_cases(void)
{
 for(unsigned compat=0;compat<2;compat++)for(unsigned process=0;process<2;process++) {
  res_reset();model_pid=7;process_closing=false;
  CHECK(intel_gfx_driver_start()==0);
  OKMT_ADAPTER_INFO adapter;DWORD count=0;
  CHECK(d3dkmt_enumerate_adapters(7,1,&adapter,&count)==0 && count==1);
  OKMT_HANDLE device=0;CHECK(d3dkmt_create_device(7,adapter.adapter,0,&device)==0);
  for(unsigned i=0;i<3;i++) {
   OKMT_HANDLE queue=0,sync=0;uint64_t cpu=0;
   CHECK(d3dkmt_create_paging_queue(7,device,(LONG)i-1,compat,&queue,&sync,&cpu)==0);
  }
  OKMT_HANDLE sync=0;uint64_t cpu=0,gpu=0;
  CHECK(d3dkmt_create_monitored_sync(7,device,0,compat,&sync,&cpu,&gpu)==0);
  service_step();CHECK(intel_gpu_service_idle());
  if(process) { vm_model_exit(7);d3dkmt_release_process(7); }
  else CHECK(d3dkmt_destroy_device(7,device)==0);
  CHECK(!intel_gpu_service_idle());
  for(unsigned i=0;i<8;i++) { executor_step();service_step(); }
  vm_model_exit(7);d3dkmt_release_process(7);
  NTSTATUS stopped=D3DKMT_STATUS_DEVICE_BUSY;
  for(unsigned i=0;i<64 && stopped;i++) {
   executor_step();service_step();stopped=intel_gfx_driver_stop();
  }
  CHECK(stopped==0 && !vm_count && intel_gfx_capture_clean() && intel_gpu_executor_quiescent());
 }
}
static unsigned paging_close_calls;
static bool paging_close_busy;
static NTSTATUS paging_retirement_close(void *context,DWORD pid,uint64_t cookie)
{
 CHECK(context==(void *)1 && pid==7 && cookie==0xfeed);
 paging_close_calls++;
 return paging_close_busy ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}
static void paging_retirement_retry_cases(void)
{
 D3DKMT_ADAPTER_DESCRIPTION desc={.driver_version=3200};OKMT_LUID luid;OKMT_HANDLE adapter,devices[65];
 D3DKMT_DEVICE_PROVIDER provider={.open=kmt_other_open,.close=paging_retirement_close,.context=(void *)1};
 CHECK(d3dkmt_register_driver(&desc,&provider,&luid)==0);
 CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0);
 for(unsigned i=0;i<65;i++) CHECK(d3dkmt_create_device(7,adapter,0,&devices[i])==0);
 D3DKMT_DEVICE_LEASE held={0};CHECK(d3dkmt_acquire_device(7,devices[0],&held)==0);
 paging_close_busy=true;paging_close_calls=0;
 for(unsigned i=0;i<65;i++) CHECK(d3dkmt_destroy_device(7,devices[i])==0);
 CHECK(paging_close_calls==64 && d3dkmt_collect_budget()==64);
 for(unsigned i=0;i<2;i++) {
  unsigned before=paging_close_calls;service_step();
  CHECK(paging_close_calls-before==32 && waited_ticks==1 && !intel_gpu_service_idle());
  CHECK(!sched_signal_take(service_signal)); /* retry timer, no self-wake storm */
 }
 paging_close_busy=false; /* readiness changes without another client call */
 service_step();CHECK(d3dkmt_collect_budget()==32 && waited_ticks==1);
 service_step();CHECK(!d3dkmt_collect_budget() && intel_gpu_service_idle());
 CHECK(d3dkmt_unregister_adapter(luid)==D3DKMT_STATUS_DEVICE_BUSY);
 unsigned before=paging_close_calls;
 CHECK(d3dkmt_release_device(&held)==0 && paging_close_calls==before+1);
 service_step();CHECK(intel_gpu_service_idle());
 CHECK(d3dkmt_close_adapter(7,adapter)==0 && d3dkmt_unregister_adapter(luid)==0);
}
#ifndef INTEL_PAGING_SUITE_ENTRY
#define INTEL_PAGING_SUITE_ENTRY main
#endif
int INTEL_PAGING_SUITE_ENTRY(void)
{
 if(paging_native_regression_main())return 1;
 unsigned before=checks;paging_retirement_retry_cases();paging_native_abandon_cases();paging_native_cases();
 printf("Intel native paging queues: %u checks passed, retained backing and teardown PE32/PE64\n",checks-before);
 return 0;
}
