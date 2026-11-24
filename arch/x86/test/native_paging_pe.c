/* Public paging-queue ABI probe; no runtime DLL or installer is redistributed. */
#include "../win32/d3dkmt_abi.h"
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL GetStdHandle(OKMT_U32);
API int CALL WriteFile(void *,const void *,OKMT_U32,OKMT_U32 *,void *);
API void CALL ExitProcess(OKMT_U32);
API OKMT_U32 CALL SetErrorMode(OKMT_U32);
API void *CALL CreateEventA(void *,int,int,const char *);
API OKMT_U32 CALL WaitForSingleObject(void *,OKMT_U32);
API int CALL CloseHandle(void *);
#if defined(PAGING_PHYSICAL) && !defined(PAGING_ORACLE)
API OKMT_U32 CALL GetCurrentProcessId(void);
API OKMT_U32 CALL GetModuleFileNameA(void *,char *,OKMT_U32);
typedef struct {
 OKMT_U32 bytes;char *reserved,*desktop,*title;
 OKMT_U32 x,y,width,height,x_chars,y_chars,fill,flags;
 unsigned short show,reserved_bytes;void *reserved_data,*input,*output,*error;
} PAGING_STARTUP;
typedef struct {void *process,*thread;OKMT_U32 pid,tid;} PAGING_PROCESS;
API int CALL CreateProcessA(const char *,char *,void *,void *,int,OKMT_U32,void *,const char *,PAGING_STARTUP *,PAGING_PROCESS *);
API int CALL GetExitCodeProcess(void *,OKMT_U32 *);
#endif
typedef struct {void *base,*allocation;OKMT_U32 allocation_protection;__UINTPTR_TYPE__ bytes;OKMT_U32 state,protection,type;} MEMORY_INFO;
static unsigned checks,failures;
static void number(const char *label,OKMT_U32 value)
{
 char text[128];unsigned n=0;while(label[n]){text[n]=label[n];n++;}
 for(unsigned i=0;i<8;i++)text[n++]="0123456789abcdef"[(value>>(28-i*4))&15];
 text[n++]='\n';OKMT_U32 written;WriteFile(GetStdHandle((OKMT_U32)-11),text,n,&written,0);
}
static __attribute__((unused)) void zero(void *data,unsigned bytes){volatile unsigned char *p=data;while(bytes--)*p++=0;}
#define CHECK(x) do{checks++;if(!(x)){failures++;number("PAGING FAIL line=",__LINE__);}}while(0)
#define NEED(x) do{CHECK(x);if(failures)goto done;}while(0)
void native_paging_entry(void)
{
 SetErrorMode(0x8003);
#if defined(PAGING_PHYSICAL) && !defined(PAGING_ORACLE)
 /* The shell's PID 1 persists across winexec. Exercise actual process exit
  * in an owned child, so an early failure also gets NT resource teardown. */
 if(GetCurrentProcessId()==1){
  char path[512];PAGING_STARTUP startup;PAGING_PROCESS child;
  zero(&startup,sizeof(startup));zero(&child,sizeof(child));startup.bytes=sizeof(startup);
  OKMT_U32 bytes=GetModuleFileNameA(0,path,sizeof(path));NEED(bytes && bytes<sizeof(path));
  NEED(CreateProcessA(path,0,0,0,0,0,0,0,&startup,&child) && child.pid!=1);
  NEED(WaitForSingleObject(child.process,15000)==0);OKMT_U32 code=259;
  CHECK(GetExitCodeProcess(child.process,&code));CHECK(CloseHandle(child.thread));CHECK(CloseHandle(child.process));
  CHECK(code==0);goto done;
 }
#endif
 void *gdi=LoadLibraryA("gdi32.dll");
 OKMT_S32 (CALL *create)(OKMT_CREATE_PAGING_QUEUE *)=GetProcAddress(gdi,"D3DKMTCreatePagingQueue");
 OKMT_S32 (CALL *destroy)(const OKMT_DESTROY_PAGING_QUEUE *)=GetProcAddress(gdi,"D3DKMTDestroyPagingQueue");
 NEED(gdi && create && destroy);CHECK(create(0)<0);CHECK(destroy(0)<0);
 for(unsigned i=0;i<5;i++){
  struct{OKMT_U64 before;OKMT_CREATE_PAGING_QUEUE args;OKMT_U64 after;} input;
  input.before=input.after=0x1234567887654321ULL;
  input.args=(OKMT_CREATE_PAGING_QUEUE){0,i==0?-2:i==1?2:0,0xfeed,0xbeef,(void *)0xabcd,i==2?1:0};
  if(i==3)input.args.device=0xbad001;
  CHECK(create(&input.args)<0);CHECK(input.before==0x1234567887654321ULL && input.after==0x1234567887654321ULL);
 }
 {OKMT_DESTROY_PAGING_QUEUE input={0};CHECK(destroy(&input)<0);input.queue=0xbad001;CHECK(destroy(&input)<0);}
#ifdef PAGING_PHYSICAL
 OKMT_S32 (CALL *enumerate)(OKMT_ENUM_ADAPTERS2 *)=GetProcAddress(gdi,"D3DKMTEnumAdapters2");
 OKMT_S32 (CALL *device_create)(OKMT_CREATE_DEVICE *)=GetProcAddress(gdi,"D3DKMTCreateDevice");
 OKMT_S32 (CALL *device_destroy)(OKMT_DESTROY_DEVICE *)=GetProcAddress(gdi,"D3DKMTDestroyDevice");
 OKMT_S32 (CALL *adapter_close)(OKMT_CLOSE_ADAPTER *)=GetProcAddress(gdi,"D3DKMTCloseAdapter");
 OKMT_S32 (CALL *wait)(OKMT_WAIT_SYNC_CPU *)=GetProcAddress(gdi,"D3DKMTWaitForSynchronizationObjectFromCpu");
 OKMT_S32 (CALL *signal)(OKMT_SIGNAL_SYNC_CPU *)=GetProcAddress(gdi,"D3DKMTSignalSynchronizationObjectFromCpu");
 OKMT_S32 (CALL *sync_destroy)(OKMT_DESTROY_SYNC *)=GetProcAddress(gdi,"D3DKMTDestroySynchronizationObject");
 __UINTPTR_TYPE__ (CALL *query)(const void *,MEMORY_INFO *,__UINTPTR_TYPE__)=GetProcAddress(LoadLibraryA("kernel32.dll"),"VirtualQuery");
 NEED(enumerate && device_create && device_destroy && adapter_close && wait && signal && sync_destroy && query);
 OKMT_ADAPTER_INFO adapters[16];OKMT_ENUM_ADAPTERS2 list={16,adapters};NEED(enumerate(&list)==0 && list.count>0 && list.count<=16);
 OKMT_CREATE_DEVICE device;zero(&device,sizeof(device));device.object.adapter=adapters[0].adapter;
 NEED(device_create(&device)==0 && device.device);
 for(int priority=-1;priority<=1;priority++){
  OKMT_CREATE_PAGING_QUEUE queue={device.device,priority,0,0,0,0};OKMT_S32 status=create(&queue);
  number("PAGING create=",(OKMT_U32)status);NEED(status==0 && queue.queue && queue.sync && queue.queue!=queue.sync && queue.cpu_address);
  MEMORY_INFO map;zero(&map,sizeof(map));NEED(query(queue.cpu_address,&map,sizeof(map))==sizeof(map));
  number("PAGING protection=",map.protection);CHECK(map.state==0x1000);
#ifndef PAGING_ORACLE
  CHECK(map.protection==(sizeof(void *)==4?4u:2u));
#endif
  number("PAGING initial-low=",((volatile OKMT_U32 *)queue.cpu_address)[0]);
  number("PAGING initial-high=",((volatile OKMT_U32 *)queue.cpu_address)[1]);
#ifndef PAGING_ORACLE
  CHECK(((volatile OKMT_U32 *)queue.cpu_address)[0]==0 && ((volatile OKMT_U32 *)queue.cpu_address)[1]==0);
#endif
  OKMT_HANDLE object=queue.sync;OKMT_U64 value=0;
  OKMT_WAIT_SYNC_CPU read={device.device,1,&object,&value,0,0};CHECK(wait(&read)==0);
#ifndef PAGING_ORACLE
  value=1;OKMT_SIGNAL_SYNC_CPU write={device.device,1,&object,&value,0};CHECK(signal(&write)<0);
  OKMT_DESTROY_SYNC forbidden={queue.sync};CHECK(sync_destroy(&forbidden)<0);
  CHECK(((volatile OKMT_U32 *)queue.cpu_address)[0]==0);
  void *event=CreateEventA(0,1,0,0);NEED(event);read.event=event;
  CHECK(wait(&read)==0);CHECK(WaitForSingleObject(event,0)==258);
#ifdef PAGING_EXIT
  if(priority==1){CloseHandle(event);number("PAGING EXIT pending-wait=",1);goto done;}
#endif
#endif
  OKMT_DESTROY_PAGING_QUEUE close={queue.queue};CHECK(destroy(&close)==0);
#ifndef PAGING_ORACLE
  CHECK(WaitForSingleObject(event,3000)==0);CHECK(CloseHandle(event));CHECK(wait(&read)<0);CHECK(destroy(&close)<0);
#endif
 }
#ifdef PAGING_ORACLE
 {OKMT_CREATE_PAGING_QUEUE queue={device.device,0,0,0,0,0};NEED(create(&queue)==0);
  OKMT_U64 value=1;OKMT_SIGNAL_SYNC_CPU write={device.device,1,&queue.sync,&value,0};
  number("PAGING oracle signal=",(OKMT_U32)signal(&write));
  OKMT_DESTROY_SYNC close_sync={queue.sync};number("PAGING oracle destroy-sync=",(OKMT_U32)sync_destroy(&close_sync));
  OKMT_DESTROY_PAGING_QUEUE close={queue.queue};number("PAGING oracle destroy-queue=",(OKMT_U32)destroy(&close));}
 {OKMT_CREATE_PAGING_QUEUE queue={device.device,0,0,0,0,0};NEED(create(&queue)==0);
  OKMT_U64 value=0xfffffffffffffffeULL;void *event=CreateEventA(0,1,0,0);NEED(event);
  OKMT_WAIT_SYNC_CPU pending={device.device,1,&queue.sync,&value,event,0};
  number("PAGING oracle wait=",(OKMT_U32)wait(&pending));
  number("PAGING oracle event-before=",WaitForSingleObject(event,0));
  OKMT_DESTROY_PAGING_QUEUE close={queue.queue};number("PAGING oracle close-pending=",(OKMT_U32)destroy(&close));
  number("PAGING oracle event-after=",WaitForSingleObject(event,300));
  MEMORY_INFO map;zero(&map,sizeof(map));query(queue.cpu_address,&map,sizeof(map));
  number("PAGING oracle mapping-after=",map.state);CloseHandle(event);}

#endif
 {OKMT_DESTROY_DEVICE close={device.device};CHECK(device_destroy(&close)==0);}
 for(unsigned i=0;i<list.count;i++){OKMT_CLOSE_ADAPTER close={adapters[i].adapter};CHECK(adapter_close(&close)==0);}
#endif
done:number("PAGING checks=",checks);number("PAGING failures=",failures);ExitProcess(failures?1:0);
}
