#define win32_user_range_writable residency_base_writable
#define win32_termination_requested residency_base_cancelled
#define D3DKMT_PAGING_SUITE_ENTRY residency_abi_regression_main
#include "d3dkmt_paging_host.c"
#undef win32_user_range_writable
#undef win32_termination_requested

static unsigned residency_writes,residency_cancels,residency_deny_at,residency_cancel_at,residency_action;
static unsigned residency_calls,residency_frees;
static NTSTATUS residency_result;
static OKMT_HANDLE residency_queue,residency_objects[2];
static struct {BOOL live;DWORD refs;uint64_t device;} residency_nodes[4];
int win32_user_range_writable(void *p,SIZE_T bytes,BOOL compat)
{
 residency_writes++;
 if(residency_writes==residency_deny_at)return 0;
 return residency_base_writable(p,bytes,compat);
}
BOOL win32_termination_requested(void)
{
 residency_cancels++;
 if(residency_cancels==residency_cancel_at)cancellation_requested=TRUE;
 if(residency_action && residency_cancels==2) {
  unsigned action=residency_action;residency_action=0;
  if(action==1)CHECK(d3dkmt_destroy_paging_queue(caller,residency_queue)==0);
  if(action==2)CHECK(d3dkmt_destroy_allocations(caller,sync_device,1,residency_objects)==0);
  if(action==3)CHECK(d3dkmt_destroy_device(caller,sync_device)==0);
  if(action==4)d3dkmt_release_process(caller);
  if(action==5)CHECK(d3dkmt_unregister_adapter(ctx.luid)==D3DKMT_STATUS_DEVICE_BUSY);
  if(action==6)CHECK(d3dkmt_mark_device_error(caller,sync_device,6)==0);
 }
 return residency_base_cancelled();
}
static NTSTATUS residency_allocate(void *data,DWORD pid,uint64_t device,
    const D3DKMT_ALLOCATION_DESCRIPTION *description,uint64_t *cookie)
{
 CHECK(data==&ctx && pid==100 && description->bytes==4096);
 for(unsigned i=0;i<4;i++)if(!residency_nodes[i].live){
  residency_nodes[i].live=TRUE;residency_nodes[i].refs=1;residency_nodes[i].device=device;*cookie=i+1;return 0;
 }
 return STATUS_NO_MEMORY;
}
static NTSTATUS residency_free(void *data,DWORD pid,uint64_t device,uint64_t cookie)
{
 CHECK(data==&ctx && pid==100 && cookie && cookie<=4);
 CHECK(residency_nodes[cookie-1].live && residency_nodes[cookie-1].device==device);
 residency_nodes[cookie-1].live=FALSE;residency_frees++;return 0;
}
static NTSTATUS residency_commit(void *data,DWORD pid,uint64_t device,BOOL make,const uint64_t *cookies,DWORD count)
{
 CHECK(data==&ctx && pid==100 && count && count<=1024 && d3dkmt_deferred_current());
 residency_calls++;
 if(residency_result)return residency_result;
 unsigned occurrences[4]={0};
 for(unsigned i=0;i<count;i++){
  CHECK(cookies[i] && cookies[i]<=4 && residency_nodes[cookies[i]-1].live && residency_nodes[cookies[i]-1].device==device);
  occurrences[cookies[i]-1]++;
 }
 for(unsigned i=0;i<4;i++)if(make)residency_nodes[i].refs+=occurrences[i];
 else residency_nodes[i].refs=residency_nodes[i].refs>occurrences[i]?residency_nodes[i].refs-occurrences[i]:0;
 return 0;
}
static void residency_setup(BOOL compat,BOOL supported)
{
 caller=100;g_compat32_mode=compat;cancellation_requested=FALSE;denied_first=denied_last=0;
 residency_writes=residency_cancels=residency_deny_at=residency_cancel_at=residency_action=0;
 residency_calls=residency_frees=0;residency_result=0;memset(residency_nodes,0,sizeof(residency_nodes));
 monitored_creates=monitored_closes=monitored_race=0;monitored_fail_close=monitored_poison_output=false;
 user_bytes=16384;user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);CHECK(user!=MAP_FAILED);
 D3DKMT_ALLOCATION_PROVIDER memory={.allocate=residency_allocate,.close=residency_free,.residency=supported?residency_commit:NULL};
 D3DKMT_RENDER_PROVIDER render={.submit=sync_render_unused,.synchronize=sync_submit};
 D3DKMT_MONITORED_PROVIDER monitored={monitored_create,monitored_close,monitored_notify};
 CHECK(d3dkmt_register_monitored_driver(&adapter_desc,&device_ops,&memory,&context_ops,&render,&monitored,&ctx.luid)==0);
 OKMT_HANDLE adapter=0;CHECK(d3dkmt_open_adapter(caller,ctx.luid,&adapter)==0);
 CHECK(d3dkmt_create_device(caller,adapter,0,&sync_device)==0);
 OKMT_HANDLE sync=0;uint64_t cpu=0;CHECK(d3dkmt_create_paging_queue(caller,sync_device,0,compat,&residency_queue,&sync,&cpu)==0);
 D3DKMT_ALLOCATION_DESCRIPTION input[2]={{4096},{4096}};
 CHECK(d3dkmt_create_allocations(caller,sync_device,2,input,residency_objects)==0);
}
static void residency_teardown(void)
{
 residency_action=residency_cancel_at=residency_deny_at=0;caller=100;cancellation_requested=FALSE;
 d3dkmt_release_process(caller);for(unsigned i=0;i<8;i++)(void)d3dkmt_collect_devices();
 CHECK(d3dkmt_unregister_adapter(ctx.luid)==0 && residency_frees==2 && monitored_closes==1);
 CHECK(!munmap(user,user_bytes));user=NULL;
}
static void residency_header(BOOL make,DWORD count,DWORD flags)
{
 residency_writes=residency_cancels=0;memset(user,0xa5,64);
 OKMT_HANDLE *list=(void *)(user+512);for(unsigned i=0;i<count && i<1024;i++)list[i]=residency_objects[i&1];
 DWORD *p=(void *)user;p[0]=make?residency_queue:sync_device;p[1]=count;
 if(g_compat32_mode){p[2]=(DWORD)(uintptr_t)list;if(make){p[3]=1;p[4]=flags;}else p[3]=flags;}
 else if(make){OKMT_MAKE_RESIDENT *h=(void *)user;h->allocations=list;h->priorities=(void *)1;h->flags=flags;}
 else {OKMT_EVICT *h=(void *)user;h->allocations=list;h->flags=flags;}
}
static NTSTATUS residency_call(BOOL make){return make?D3DKMTMakeResident(user):D3DKMTEvict(user);}
static void residency_abi_cases(BOOL compat,BOOL make)
{
 residency_setup(compat,TRUE);CHECK(D3DKMTMakeResident(NULL)==STATUS_INVALID_PARAMETER);CHECK(D3DKMTEvict(NULL)==STATUS_INVALID_PARAMETER);
 for(unsigned bad=0;bad<11;bad++){
  residency_header(make,2,0);residency_deny_at=residency_cancel_at=0;cancellation_requested=FALSE;
  if(bad==0)((DWORD *)user)[0]=0;
  if(bad==1)((DWORD *)user)[1]=0;
  if(bad==2)((DWORD *)user)[1]=1025;
  if(bad==3){if(compat)((DWORD *)user)[make?4:3]=4;else if(make)((OKMT_MAKE_RESIDENT *)user)->flags=4;else ((OKMT_EVICT *)user)->flags=4;}
  if(bad==4)((DWORD *)(user+512))[1]=0;
  if(bad==5)((DWORD *)(user+512))[1]=residency_queue;
  if(bad==6)caller=101;
  if(bad==7)residency_cancel_at=1;
  if(bad==8)residency_cancel_at=2;
  if(bad==9){if(!make && compat)continue;residency_deny_at=1;}
  if(bad==10){if(!make && compat)continue;residency_deny_at=make?3:2;}
  BYTE before[64];memcpy(before,user,64);unsigned calls=residency_calls;
  CHECK(residency_call(make)!=0 && residency_calls==calls && !memcmp(before,user,64));
  CHECK(residency_nodes[0].refs==1 && residency_nodes[1].refs==1);caller=100;
 }
 residency_deny_at=residency_cancel_at=0;cancellation_requested=FALSE;
 for(unsigned option=0;option<4;option++) {
  residency_header(make,1024,option);BYTE before[64];memcpy(before,user,64);unsigned calls=residency_calls;
  CHECK(residency_call(make)==0 && residency_calls==calls+1);
  uint64_t zero=0;
  if(make){memcpy(before+(compat?24:32),&zero,8);memcpy(before+(compat?32:40),&zero,8);}
  else if(!compat)memcpy(before+24,&zero,8);
  CHECK(!memcmp(before,user,64));
  CHECK(residency_nodes[0].refs==(make?1+(option+1)*512:0) && residency_nodes[1].refs==residency_nodes[0].refs);
 }
 for(unsigned failure=0;failure<3;failure++) {
  residency_header(make,2,0);BYTE before[64];memcpy(before,user,64);DWORD refs=residency_nodes[0].refs;
  residency_result=failure==0?D3DKMT_STATUS_DEVICE_BUSY:failure==1?STATUS_INSUFFICIENT_RESOURCES:STATUS_PENDING;
  CHECK(residency_call(make)!=0 && !memcmp(before,user,64) && residency_nodes[0].refs==refs);
 }
 residency_result=0;residency_teardown();
 residency_setup(compat,FALSE);residency_header(make,2,0);CHECK(residency_call(make)==STATUS_NOT_SUPPORTED && !residency_calls);residency_teardown();
 for(unsigned action=make?1:2;action<=6;action++) {
  residency_setup(compat,TRUE);residency_header(make,2,0);residency_action=action;
  BYTE before[64];memcpy(before,user,64);
  CHECK(residency_call(make)!=0 && !residency_calls && !memcmp(before,user,64));
  CHECK(residency_nodes[0].refs==1 && residency_nodes[1].refs==1);residency_teardown();
 }
}
int main(void)
{
 CHECK(residency_abi_regression_main()==0);unsigned before=checks;
 for(unsigned compat=0;compat<2;compat++)for(unsigned make=0;make<2;make++)residency_abi_cases(compat,make);
 printf("D3DKMT residency ABI: checks=%u failures=%u PE32/PE64\n",checks-before,failures);return failures?1:0;
}
