/* Authored public KMT probe. Microsoft binaries are never build inputs. */
#include "../win32/d3dkmt_abi.h"
#include "../drivers/intel_gpu_umd_abi.h"
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL GetStdHandle(OKMT_U32);
API int CALL WriteFile(void *,const void *,OKMT_U32,OKMT_U32 *,void *);
API void CALL ExitProcess(OKMT_U32);
API OKMT_U32 CALL SetErrorMode(OKMT_U32);
static unsigned checks,failures;
static void number(const char *label,unsigned value)
{
    char text[96];unsigned n=0;OKMT_U32 written;
    while (label[n]) {text[n]=label[n];n++;}
    for (unsigned i=0;i<8;i++) text[n++]="0123456789abcdef"[(value>>(28-i*4))&15];
    text[n++]='\n';WriteFile(GetStdHandle((OKMT_U32)-11),text,n,&written,0);
}
#define CHECK(x) do {checks++;if (!(x)) {failures++;number("OFFER FAIL line=",__LINE__);}} while (0)
#define REQUIRE(x) do {CHECK(x);if (failures) goto done;} while (0)
void native_offer_entry(void)
{
    SetErrorMode(0x8003);void *gdi=LoadLibraryA("gdi32.dll");
#define RESOLVE(name,type) OKMT_S32 (CALL *name)(type *)=GetProcAddress(gdi,"D3DKMT" #name)
    RESOLVE(OfferAllocations,OKMT_OFFER_ALLOCATIONS);
    RESOLVE(ReclaimAllocations,OKMT_RECLAIM_ALLOCATIONS);
    RESOLVE(ReclaimAllocations2,OKMT_RECLAIM_ALLOCATIONS2);
    REQUIRE(gdi && OfferAllocations && ReclaimAllocations && ReclaimAllocations2);
    CHECK(OfferAllocations(0)<0 && ReclaimAllocations(0)<0 && ReclaimAllocations2(0)<0);
    for (unsigned bad=0;bad<9;bad++) {
        OKMT_HANDLE list[2]={0xdead001,0xdead002};OKMT_U32 values[2]={0xfeed,0xbeef};
        OKMT_OFFER_ALLOCATIONS offer={bad?0xdead003:0,0,list,bad==1?0:bad==2?1025:2,bad==3?0:bad==4?5:2,bad==5?4:0};
        if (bad==6) offer.allocations=0;
        if (bad==7) offer.resources=list;
        if (bad==8) offer.allocations=(void *)1;
        OKMT_RECLAIM_ALLOCATIONS reclaim={offer.device,offer.resources,offer.allocations,values,offer.count};
        OKMT_RECLAIM_ALLOCATIONS2 reclaim2={offer.device,offer.count,offer.resources,offer.allocations,values,0xfeedbeef};
        CHECK(OfferAllocations(&offer)<0);
        CHECK(ReclaimAllocations(&reclaim)<0 && values[0]==0xfeed && values[1]==0xbeef);
        CHECK(ReclaimAllocations2(&reclaim2)<0 && values[0]==0xfeed && values[1]==0xbeef && reclaim2.fence==0xfeedbeef);
    }
#ifdef OFFER_PHYSICAL
    RESOLVE(EnumAdapters2,OKMT_ENUM_ADAPTERS2);RESOLVE(QueryAdapterInfo,OKMT_QUERY_ADAPTER_INFO);
    RESOLVE(CreateDevice,OKMT_CREATE_DEVICE);RESOLVE(DestroyDevice,OKMT_DESTROY_DEVICE);RESOLVE(CloseAdapter,OKMT_CLOSE_ADAPTER);
    RESOLVE(CreateAllocation,OKMT_CREATE_ALLOCATION);RESOLVE(DestroyAllocation,OKMT_DESTROY_ALLOCATION);
    RESOLVE(QueryAllocationResidency,OKMT_ALLOCATION_PROPERTIES);
    RESOLVE(Lock,OKMT_LOCK);RESOLVE(Unlock,OKMT_UNLOCK);
    RESOLVE(CreatePagingQueue,OKMT_CREATE_PAGING_QUEUE);RESOLVE(DestroyPagingQueue,OKMT_DESTROY_PAGING_QUEUE);
    REQUIRE(EnumAdapters2 && QueryAdapterInfo && CreateDevice && DestroyDevice && CloseAdapter &&
        CreateAllocation && DestroyAllocation && QueryAllocationResidency && Lock && Unlock && CreatePagingQueue && DestroyPagingQueue);
    OKMT_ADAPTER_INFO adapters[16];OKMT_ENUM_ADAPTERS2 list={16,adapters};
    REQUIRE(EnumAdapters2(&list)==0 && list.count<=16);OKMT_HANDLE selected=0;
    for (unsigned i=0;i<list.count;i++) {
        OKMT_QUERY_DEVICE_IDS ids={0};OKMT_QUERY_ADAPTER_INFO info={adapters[i].adapter,OKMT_QUERY_DEVICE_IDENTIFIERS,&ids,sizeof(ids)};
        if (QueryAdapterInfo(&info)==0 && ids.vendor==0x8086 && ids.device==0x3e9b) selected=adapters[i].adapter;
    }
    REQUIRE(selected);OKMT_CREATE_DEVICE device={0};device.object.adapter=selected;
    REQUIRE(CreateDevice(&device)==0 && device.device);
    OKMT_CREATE_PAGING_QUEUE queue={0};queue.device=device.device;
    REQUIRE(CreatePagingQueue(&queue)==0 && queue.queue && queue.sync);
    intel_umd_buffer_description_t blob={INTEL_UMD_BUFFER_MAGIC,INTEL_UMD_BUFFER_VERSION,5000,0,0};
    OKMT_ALLOCATION_INFO info[3]={{0}};OKMT_HANDLE handles[3];
    for (unsigned i=0;i<3;i++) {info[i].private_data=&blob;info[i].private_bytes=sizeof(blob);}
    OKMT_CREATE_ALLOCATION create={0};create.device=device.device;create.count=3;create.allocations=info;create.flags=1;
    REQUIRE(CreateAllocation(&create)==0 && create.resource);
    for (unsigned i=0;i<3;i++) {handles[i]=info[i].allocation;REQUIRE(handles[i]);}
    for (unsigned round=0;round<8;round++) {
        for (unsigned i=0;i<3;i++) {
            OKMT_LOCK lock={0};lock.device=device.device;lock.allocation=handles[i];
            lock.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_DO_NOT_WAIT|OKMT_LOCK_WRITE_ONLY;
            REQUIRE(Lock(&lock)==0 && lock.data);
            for (unsigned j=0;j<5000;j++) ((unsigned char *)lock.data)[j]=(unsigned char)(j+i+round);
            OKMT_UNLOCK unlock={device.device,1,handles+i};REQUIRE(Unlock(&unlock)==0);
        }
        OKMT_U32 values[3]={0xfeed,0xfeed,0xfeed};OKMT_HANDLE groups[3]={create.resource,create.resource,create.resource};
        unsigned grouped=round&1,flags=round&3;
        OKMT_OFFER_ALLOCATIONS offer={device.device,grouped?groups:0,grouped?0:handles,3,1+(round&3),flags};
        REQUIRE(OfferAllocations(&offer)==0);
        OKMT_ALLOCATION_PROPERTIES properties={device.device,0,handles,3,values};
        REQUIRE(QueryAllocationResidency(&properties)==0);
        for (unsigned i=0;i<3;i++) CHECK(values[i]==((flags&1)?3u:2u));
#ifdef OFFER_EXIT
        if (round==1) {number("OFFER exit-owned=",3);goto done;}
#endif
        if (round&2) {
            OKMT_RECLAIM_ALLOCATIONS2 reclaim={queue.queue,3,grouped?groups:0,grouped?0:handles,values,0xfeed};
            REQUIRE(ReclaimAllocations2(&reclaim)==0 && reclaim.fence==0);
        } else {
            OKMT_RECLAIM_ALLOCATIONS reclaim={device.device,grouped?groups:0,grouped?0:handles,values,3};
            REQUIRE(ReclaimAllocations(&reclaim)==0);
        }
        for (unsigned i=0;i<3;i++) {
            CHECK(values[i]==(flags&1));
            OKMT_LOCK lock={0};lock.device=device.device;lock.allocation=handles[i];
            lock.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_DO_NOT_WAIT|OKMT_LOCK_READ_ONLY;
            REQUIRE(Lock(&lock)==0 && lock.data);
            unsigned good=1;
            for (unsigned j=0;j<5000;j++) if (((unsigned char *)lock.data)[j]!=((flags&1)?0:(unsigned char)(j+i+round))) good=0;
            CHECK(good);OKMT_UNLOCK unlock={device.device,1,handles+i};REQUIRE(Unlock(&unlock)==0);
        }
    }
    OKMT_DESTROY_ALLOCATION drop={device.device,create.resource,0,0};REQUIRE(DestroyAllocation(&drop)==0);
    OKMT_DESTROY_PAGING_QUEUE closeq={queue.queue};REQUIRE(DestroyPagingQueue(&closeq)==0);
    OKMT_DESTROY_DEVICE close={device.device};REQUIRE(DestroyDevice(&close)==0);
    for (unsigned i=0;i<list.count;i++) {OKMT_CLOSE_ADAPTER closea={adapters[i].adapter};CHECK(CloseAdapter(&closea)==0);}
#endif
done:
    number("OFFER checks=",checks);number("OFFER failures=",failures);ExitProcess(failures?1:0);
}
