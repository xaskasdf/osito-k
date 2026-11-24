/* Differential probe. Only marks freshly created, idle, process-owned devices. */
#include "../win32/d3dkmt_abi.h"
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL GetStdHandle(OKMT_U32);
API int CALL WriteFile(void *,const void *,OKMT_U32,OKMT_U32 *,void *);
API void CALL ExitProcess(OKMT_U32);
static void text(const char *s)
{OKMT_U32 n=0,w;while(s[n])n++;WriteFile(GetStdHandle((OKMT_U32)-11),s,n,&w,0);}
static void hex(OKMT_U32 v)
{char b[9];for(unsigned i=0;i<8;i++)b[i]="0123456789abcdef"[(v>>(28-i*4))&15];b[8]=0;text(b);}
typedef struct {OKMT_HANDLE device;OKMT_U32 reason;} MARK;
void device_error_oracle_entry(void)
{
 void *g=LoadLibraryA("gdi32.dll");
 void *k=LoadLibraryA("kernel32.dll");
 void *(CALL *CreateEventA)(void *,int,int,const char *)=GetProcAddress(k,"CreateEventA");
 OKMT_U32 (CALL *WaitForSingleObject)(void *,OKMT_U32)=GetProcAddress(k,"WaitForSingleObject");
 int (CALL *CloseHandle)(void *)=GetProcAddress(k,"CloseHandle");
#define RESOLVE(n,t) OKMT_S32 (CALL *n)(t *)=GetProcAddress(g,"D3DKMT" #n)
 RESOLVE(EnumAdapters2,OKMT_ENUM_ADAPTERS2);RESOLVE(CreateDevice,OKMT_CREATE_DEVICE);
 RESOLVE(DestroyDevice,OKMT_DESTROY_DEVICE);RESOLVE(CloseAdapter,OKMT_CLOSE_ADAPTER);
 RESOLVE(GetDeviceState,OKMT_DEVICE_EXECUTION);RESOLVE(MarkDeviceAsError,MARK);
 RESOLVE(CreateContext,OKMT_CREATE_CONTEXT);RESOLVE(DestroyContext,OKMT_DESTROY_CONTEXT);
 RESOLVE(CreateSynchronizationObject,OKMT_CREATE_SYNC);RESOLVE(DestroySynchronizationObject,OKMT_DESTROY_SYNC);
 RESOLVE(CreateSynchronizationObject2,OKMT_CREATE_SYNC2);
 RESOLVE(WaitForSynchronizationObjectFromCpu,OKMT_WAIT_SYNC_CPU);
 RESOLVE(SignalSynchronizationObjectFromCpu,OKMT_SIGNAL_SYNC_CPU);
#undef RESOLVE
 if(!g||!EnumAdapters2||!CreateDevice||!DestroyDevice||!CloseAdapter||!GetDeviceState||!MarkDeviceAsError){text("missing exports\n");ExitProcess(1);}
 OKMT_ADAPTER_INFO adapters[16];OKMT_ENUM_ADAPTERS2 list={16,adapters};
 OKMT_S32 status=EnumAdapters2(&list);text("enum=");hex(status);text(" count=");hex(list.count);text("\n");
 if(status||!list.count||list.count>16)ExitProcess(2);
 OKMT_U32 reasons[]={0x80000000,0x80000006,0,0xffffffff,1,2,3,4,5,6,7,0x80000001,0x80000007};
 for(unsigned i=0;i<sizeof(reasons)/sizeof(reasons[0]);i++) {
  OKMT_CREATE_DEVICE d={0};d.object.adapter=adapters[0].adapter;
  status=CreateDevice(&d);text("create=");hex(status);text("\n");if(status)break;
  OKMT_U32 buffer[32]={0};OKMT_DEVICE_EXECUTION *q=(void *)buffer;q->device=d.device;q->type=1;q->state=0xfeed;
  status=GetDeviceState(q);text("before=");hex(status);text(" state=");hex(q->state);text("\n");
  OKMT_CREATE_CONTEXT baseline={0};baseline.device=d.device;
  status=CreateContext(&baseline);text("context-before=");hex(status);text("\n");
  if(!status){OKMT_DESTROY_CONTEXT drop={baseline.context};DestroyContext(&drop);}
  OKMT_CREATE_SYNC2 monitored={0};monitored.device=d.device;monitored.info.type=OKMT_SYNC_MONITORED;
  if(i==12)monitored.info.flags=2;
  status=CreateSynchronizationObject2(&monitored);text("monitored=");hex(status);text("\n");
  void *event=CreateEventA(0,1,0,0);OKMT_U64 value=1;
  if(!status) {
   OKMT_WAIT_SYNC_CPU wait={d.device,1,&monitored.object,&value,event,0};
   status=WaitForSynchronizationObjectFromCpu(&wait);text("wait-before=");hex(status);text("\n");
  }
  MARK mark={d.device,reasons[i]};status=MarkDeviceAsError(&mark);text("reason=");hex(mark.reason);text(" mark=");hex(status);text("\n");
  if(monitored.object) {
   text("event-after=");hex(WaitForSingleObject(event,100));text(" fence-low=");
   hex(*(volatile OKMT_U32 *)monitored.info.data.monitored.cpu_address);text("\n");
   OKMT_SIGNAL_SYNC_CPU signal={d.device,1,&monitored.object,&value,0};
   status=SignalSynchronizationObjectFromCpu(&signal);text("signal-after=");hex(status);
   text(" event=");hex(WaitForSingleObject(event,100));text(" fence-high=");
   hex(((volatile OKMT_U32 *)monitored.info.data.monitored.cpu_address)[1]);text(" fence-low=");
   hex(*(volatile OKMT_U32 *)monitored.info.data.monitored.cpu_address);text("\n");
   OKMT_DESTROY_SYNC drop={monitored.object};DestroySynchronizationObject(&drop);
  }
  CloseHandle(event);
  monitored=(OKMT_CREATE_SYNC2){0};monitored.device=d.device;monitored.info.type=OKMT_SYNC_MONITORED;
  status=CreateSynchronizationObject2(&monitored);text("monitored-after=");hex(status);
  if(!status){text(" value=");hex(*(volatile OKMT_U32 *)monitored.info.data.monitored.cpu_address);
   OKMT_DESTROY_SYNC drop={monitored.object};DestroySynchronizationObject(&drop);}
  text("\n");
  q->state=0xfeed;status=GetDeviceState(q);text("after=");hex(status);text(" state=");hex(q->state);text("\n");
  status=MarkDeviceAsError(&mark);text("repeat=");hex(status);text("\n");
  OKMT_CREATE_CONTEXT context={0};context.device=d.device;
  status=CreateContext(&context);text("context-after=");hex(status);text("\n");
  if(!status){OKMT_DESTROY_CONTEXT drop={context.context};DestroyContext(&drop);}
  OKMT_CREATE_SYNC sync={0};sync.device=d.device;sync.info.type=OKMT_SYNC_SEMAPHORE;sync.info.data.semaphore.maximum=1;
  status=CreateSynchronizationObject(&sync);text("sync-after=");hex(status);text("\n");
  if(!status){OKMT_DESTROY_SYNC drop={sync.object};DestroySynchronizationObject(&drop);}
  mark.reason=0x80000006;status=MarkDeviceAsError(&mark);text("driver-again=");hex(status);
  q->state=0xfeed;status=GetDeviceState(q);text(" query=");hex(status);text(" state=");hex(q->state);text("\n");
  mark.reason=0x80000000;status=MarkDeviceAsError(&mark);text("generic-again=");hex(status);
  q->state=0xfeed;status=GetDeviceState(q);text(" query=");hex(status);text(" state=");hex(q->state);text("\n");
  OKMT_DESTROY_DEVICE drop={d.device};status=DestroyDevice(&drop);text("destroy=");hex(status);text("\n");
 }
 for(unsigned i=0;i<list.count;i++){OKMT_CLOSE_ADAPTER close={adapters[i].adapter};CloseAdapter(&close);}
 ExitProcess(0);
}
