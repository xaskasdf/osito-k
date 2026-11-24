/* Differential oracle using a locally installed SDK and Windows GDI only.
 * No SDK header, runtime DLL or installer is copied into the OS image. */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

static void number(const char *label,UINT value)
{
 char text[128];UINT n=0;DWORD written;while(label[n]){text[n]=label[n];n++;}
 for(UINT i=0;i<8;i++)text[n++]="0123456789abcdef"[(value>>(28-i*4))&15];
 text[n++]='\n';WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&written,NULL);
}
static void clear(void *data,SIZE_T size){volatile BYTE *p=data;while(size--)*p++=0;}
void *memset(void *data,int value,size_t size){BYTE *p=data;while(size--)*p++=(BYTE)value;return data;}
static unsigned failures;
#define CHECK(x) do{if(!(x)){failures++;number("RESIDENCY FAIL line=",__LINE__);}}while(0)
#define GET(name,type) type name=(type)GetProcAddress(gdi,#name);if(!name){number("RESIDENCY missing line=",__LINE__);ExitProcess(2);}

void residency_windows_entry(void)
{
 SetErrorMode(0x8003);
 HMODULE gdi=LoadLibraryA("gdi32.dll");CHECK(gdi);
 GET(D3DKMTEnumAdapters2,PFND3DKMT_ENUMADAPTERS2);
 GET(D3DKMTCreateDevice,PFND3DKMT_CREATEDEVICE);
 GET(D3DKMTDestroyDevice,PFND3DKMT_DESTROYDEVICE);
 GET(D3DKMTCloseAdapter,PFND3DKMT_CLOSEADAPTER);
 GET(D3DKMTCreateAllocation2,PFND3DKMT_CREATEALLOCATION2);
 GET(D3DKMTDestroyAllocation2,PFND3DKMT_DESTROYALLOCATION2);
 GET(D3DKMTCreatePagingQueue,PFND3DKMT_CREATEPAGINGQUEUE);
 GET(D3DKMTDestroyPagingQueue,PFND3DKMT_DESTROYPAGINGQUEUE);
 GET(D3DKMTMakeResident,PFND3DKMT_MAKERESIDENT);
 GET(D3DKMTEvict,PFND3DKMT_EVICT);
 GET(D3DKMTWaitForSynchronizationObjectFromCpu,PFND3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU);
 D3DKMT_ADAPTERINFO adapters[8];D3DKMT_ENUMADAPTERS2 list={8,adapters};clear(adapters,sizeof(adapters));
 CHECK(D3DKMTEnumAdapters2(&list)==0 && list.NumAdapters && list.NumAdapters<=8);
 void *backing=VirtualAlloc(NULL,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(backing);
 unsigned tested=0;
 for(UINT index=0;backing && index<list.NumAdapters && index<8;index++) {
  D3DKMT_CREATEDEVICE device;clear(&device,sizeof(device));device.hAdapter=adapters[index].hAdapter;
  NTSTATUS status=D3DKMTCreateDevice(&device);number("RESIDENCY device=",status);if(status<0)continue;
  D3DKMT_CREATEPAGINGQUEUE queue;clear(&queue,sizeof(queue));queue.hDevice=device.hDevice;
  status=D3DKMTCreatePagingQueue(&queue);number("RESIDENCY queue=",status);
  if(status>=0) {
   D3DDDI_ALLOCATIONINFO2 info;clear(&info,sizeof(info));info.pSystemMem=backing;
   D3DKMT_CREATESTANDARDALLOCATION standard;clear(&standard,sizeof(standard));
   standard.Type=D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;standard.ExistingHeapData.Size=65536;
   D3DKMT_CREATEALLOCATION allocation;clear(&allocation,sizeof(allocation));
   allocation.hDevice=device.hDevice;allocation.NumAllocations=1;allocation.pAllocationInfo2=&info;
   allocation.pStandardAllocation=&standard;allocation.Flags.CreateResource=1;allocation.Flags.CreateShared=1;
   allocation.Flags.StandardAllocation=1;allocation.Flags.ExistingSysMem=1;allocation.Flags.CrossAdapter=1;
   status=D3DKMTCreateAllocation2(&allocation);number("RESIDENCY allocation=",status);
   if(status>=0) {
    tested++;
    D3DKMT_EVICT evict;clear(&evict,sizeof(evict));evict.hDevice=device.hDevice;evict.NumAllocations=1;evict.AllocationList=&info.hAllocation;
    for(unsigned i=0;i<3;i++) {evict.NumBytesToTrim=0xfeed;number("RESIDENCY initial-evict=",D3DKMTEvict(&evict));number("RESIDENCY trim=",(UINT)evict.NumBytesToTrim);}
    for(unsigned i=0;i<2;i++) {
     D3DDDI_MAKERESIDENT make;clear(&make,sizeof(make));make.hPagingQueue=queue.hPagingQueue;make.NumAllocations=1;make.AllocationList=&info.hAllocation;
     make.PagingFenceValue=make.NumBytesToTrim=0xfeed;
     status=D3DKMTMakeResident(&make);number("RESIDENCY make=",status);number("RESIDENCY count=",make.NumAllocations);
     number("RESIDENCY fence=",(UINT)make.PagingFenceValue);number("RESIDENCY trim=",(UINT)make.NumBytesToTrim);CHECK(status>=0);
     if(status==0x103) {
      D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait;clear(&wait,sizeof(wait));wait.hDevice=device.hDevice;wait.ObjectCount=1;
      wait.ObjectHandleArray=&queue.hSyncObject;wait.FenceValueArray=&make.PagingFenceValue;
      CHECK(D3DKMTWaitForSynchronizationObjectFromCpu(&wait)==0);
     }
    }
    for(unsigned i=0;i<3;i++) {evict.NumBytesToTrim=0xfeed;number("RESIDENCY after-make-evict=",D3DKMTEvict(&evict));}
    D3DKMT_HANDLE duplicates[2]={info.hAllocation,info.hAllocation};
    D3DDDI_MAKERESIDENT duplicate;clear(&duplicate,sizeof(duplicate));
    duplicate.hPagingQueue=queue.hPagingQueue;duplicate.NumAllocations=2;duplicate.AllocationList=duplicates;
    duplicate.PriorityList=(const UINT *)1; /* documented ignored pointer */
    status=D3DKMTMakeResident(&duplicate);number("RESIDENCY duplicate-make=",status);
    number("RESIDENCY duplicate-count=",duplicate.NumAllocations);CHECK(status>=0);
    if(status==0x103) {
     D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait;clear(&wait,sizeof(wait));
     wait.hDevice=device.hDevice;wait.ObjectCount=1;wait.ObjectHandleArray=&queue.hSyncObject;wait.FenceValueArray=&duplicate.PagingFenceValue;
     CHECK(D3DKMTWaitForSynchronizationObjectFromCpu(&wait)==0);
    }
    evict.NumAllocations=2;evict.AllocationList=duplicates;
    number("RESIDENCY duplicate-evict=",D3DKMTEvict(&evict));
    for(unsigned variant=0;variant<3;variant++) {
     D3DDDI_MAKERESIDENT make;clear(&make,sizeof(make));make.hPagingQueue=queue.hPagingQueue;
     make.NumAllocations=variant?1:0;make.AllocationList=variant?&info.hAllocation:NULL;
     make.Flags.Value=variant==1?4:variant==2?0xffffffff:0;
     make.PagingFenceValue=make.NumBytesToTrim=0xfeed;
     number("RESIDENCY variant=",variant);number("RESIDENCY variant-make=",D3DKMTMakeResident(&make));
     number("RESIDENCY variant-count=",make.NumAllocations);number("RESIDENCY variant-fence=",(UINT)make.PagingFenceValue);
     D3DKMT_EVICT drop;clear(&drop,sizeof(drop));drop.hDevice=device.hDevice;
     drop.NumAllocations=variant?1:0;drop.AllocationList=variant?&info.hAllocation:NULL;drop.Flags.Value=make.Flags.Value;
     drop.NumBytesToTrim=0xfeed;number("RESIDENCY variant-evict=",D3DKMTEvict(&drop));
    }
    D3DKMT_DESTROYALLOCATION2 release;clear(&release,sizeof(release));release.hDevice=device.hDevice;release.hResource=allocation.hResource;
    CHECK(D3DKMTDestroyAllocation2(&release)==0);
   }
   D3DDDI_DESTROYPAGINGQUEUE release={queue.hPagingQueue};CHECK(D3DKMTDestroyPagingQueue(&release)==0);
  }
  D3DKMT_DESTROYDEVICE close={device.hDevice};CHECK(D3DKMTDestroyDevice(&close)==0);
 }
 for(UINT i=0;i<list.NumAdapters && i<8;i++){D3DKMT_CLOSEADAPTER close={adapters[i].hAdapter};CHECK(D3DKMTCloseAdapter(&close)==0);}
 if(backing)CHECK(VirtualFree(backing,0,MEM_RELEASE));CHECK(tested);
 number("RESIDENCY adapters-tested=",tested);number("RESIDENCY failures=",failures);ExitProcess(failures?1:0);
}
