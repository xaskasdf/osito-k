#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include "../win32/d3dkmt_registry.h"
#include "../win32/d3dkmt.h"

static unsigned checks,failures;
#define CHECK(x) do { __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED); if(!(x)) { \
    __atomic_add_fetch(&failures,1,__ATOMIC_RELAXED); \
    printf("FAIL line %d: %s\n",__LINE__,#x); } } while(0)

int g_compat32_mode;
static DWORD caller=100;
static unsigned char *user;
static size_t user_bytes;
static uintptr_t denied_first,denied_last;
static BOOL cancellation_requested;
static unsigned cancellation_deliveries;
/* Model only the NT reference boundary; the actual handle/event code has its
 * own suite. This fixture performs no GPU execution. */
static unsigned event_refs,event_signals;
NTSTATUS ntsync_reference_event_for_process(HANDLE h,DWORD pid,PVOID *out)
{
    if (pid!=100 || (uintptr_t)h!=0x40) return STATUS_INVALID_HANDLE;
    event_refs++; *out=&event_refs; return STATUS_SUCCESS;
}
void ntsync_signal_event_reference(PVOID object,DWORD pid)
{ CHECK(object==&event_refs && event_refs && pid==100); event_signals++; }
void ntsync_event_release(PVOID object)
{ CHECK(object==&event_refs && event_refs); event_refs--; }
BOOL win32_termination_requested(void) { return cancellation_requested; }
void win32_main_termination_checkpoint(void)
{
    if (cancellation_requested && !d3dkmt_deferred_current()) cancellation_deliveries++;
}
DWORD WINAPI GetCurrentProcessId(void) { return caller; }
int win32_user_range_readable(const void *p,SIZE_T bytes,BOOL compat)
{
    uintptr_t address=(uintptr_t)p,base=(uintptr_t)user;
    if (address<base || bytes>user_bytes || address-base>user_bytes-bytes) return 0;
    return !compat || (address<0x80000000u && bytes<=0x80000000u-address);
}
int win32_user_range_writable(void *p,SIZE_T bytes,BOOL compat)
{
    uintptr_t address=(uintptr_t)p;
    return win32_user_range_readable(p,bytes,compat) &&
           !(address<denied_last &&
             (address>=denied_first || bytes>denied_first-address));
}

static OKMT_LUID thread_adapter;
static void *registry_worker(void *arg)
{
    DWORD pid=(DWORD)(uintptr_t)arg;
    for (unsigned n=0;n<3000;n++) {
        OKMT_HANDLE h=0;
        DWORD value=0;
        CHECK(d3dkmt_open_adapter(pid,thread_adapter,&h)==0);
        CHECK(d3dkmt_query_adapter(pid,h,OKMT_QUERY_DRIVER_VERSION,&value,4,FALSE)==0);
        CHECK(value==3200);
        CHECK(d3dkmt_close_adapter(pid+100,h)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_close_adapter(pid,h)==0);
        CHECK(d3dkmt_close_adapter(pid,h)==STATUS_INVALID_PARAMETER);
    }
    return NULL;
}

int main(void)
{
    OKMT_ADAPTER_INFO listed[D3DKMT_MAX_ADAPTERS];
    DWORD count=99,value=0;
    CHECK(d3dkmt_enumerate_adapters(1,0,NULL,&count)==0 && count==0);
    CHECK(d3dkmt_enumerate_adapters(0,0,NULL,&count)==STATUS_INVALID_PARAMETER);
    D3DKMT_ADAPTER_DESCRIPTION description={0};
    description.driver_version=3200;
    description.adapter_type=OKMT_ADAPTER_RENDER | OKMT_ADAPTER_DISPLAY;
    description.source_count=1;
    description.device_ids.vendor=0x1234;
    /* Test fixture names never enter the production driver registry. */
    description.umd32[0]='3'; description.umd64[0]='6';
    OKMT_LUID luid;
    CHECK(d3dkmt_register_adapter(&description,&luid)==0);
    CHECK(d3dkmt_enumerate_adapters(1,0,listed,&count)==D3DKMT_STATUS_BUFFER_TOO_SMALL);
    CHECK(d3dkmt_enumerate_adapters(1,D3DKMT_MAX_ADAPTERS,listed,&count)==0 && count==1);
    OKMT_HANDLE first=listed[0].adapter,second=0;
    CHECK(d3dkmt_open_adapter(1,luid,&second)==0 && second!=first);
    CHECK(d3dkmt_query_adapter(2,first,OKMT_QUERY_DRIVER_VERSION,&value,4,0)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_close_adapter(2,first)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_close_adapter(1,second)==0);
    CHECK(d3dkmt_query_adapter(1,first,OKMT_QUERY_DRIVER_VERSION,&value,4,0)==0 && value==3200);
    CHECK(d3dkmt_query_adapter(1,first,OKMT_QUERY_ADAPTER_TYPE,&value,4,0)==0 && value==3);
    OKMT_UMD_FILENAME filename={0};
    CHECK(d3dkmt_query_adapter(1,first,OKMT_QUERY_UMD_FILENAME,&filename,sizeof(filename),1)==0 && filename.name[0]=='3');
    CHECK(d3dkmt_query_adapter(1,first,OKMT_QUERY_UMD_FILENAME,&filename,sizeof(filename),0)==0 && filename.name[0]=='6');
    filename.version=2;
    CHECK(d3dkmt_query_adapter(1,first,OKMT_QUERY_UMD_FILENAME,&filename,sizeof(filename),0)==STATUS_NOT_SUPPORTED);
    OKMT_QUERY_DEVICE_IDS ids={0};
    CHECK(d3dkmt_query_adapter(1,first,OKMT_QUERY_DEVICE_IDENTIFIERS,&ids,sizeof(ids),0)==0 && ids.vendor==0x1234);
    ids.physical_index=1;
    CHECK(d3dkmt_query_adapter(1,first,OKMT_QUERY_DEVICE_IDENTIFIERS,&ids,sizeof(ids),0)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_unregister_adapter(luid)==0);
    CHECK(d3dkmt_query_adapter(1,first,OKMT_QUERY_DRIVER_VERSION,&value,4,0)==D3DKMT_STATUS_DEVICE_REMOVED);
    OKMT_LUID replacement;
    CHECK(d3dkmt_register_adapter(&description,&replacement)==0 && replacement.low!=luid.low);
    CHECK(d3dkmt_query_adapter(1,first,OKMT_QUERY_DRIVER_VERSION,&value,4,0)==D3DKMT_STATUS_DEVICE_REMOVED);
    CHECK(d3dkmt_close_adapter(1,first)==0);
    CHECK(d3dkmt_open_adapter(1,replacement,&second)==0 && second!=first);
    CHECK(d3dkmt_query_adapter(1,first,OKMT_QUERY_DRIVER_VERSION,&value,4,0)==STATUS_INVALID_PARAMETER);
    d3dkmt_release_process(1);
    CHECK(d3dkmt_query_adapter(1,second,OKMT_QUERY_DRIVER_VERSION,&value,4,0)==STATUS_INVALID_PARAMETER);

    /* Exhaustion must not publish half an enumeration or leak references. */
    OKMT_LUID other;
    CHECK(d3dkmt_register_adapter(&description,&other)==0);
    for (unsigned i=0;i<D3DKMT_MAX_HANDLES-1;i++)
        CHECK(d3dkmt_open_adapter(3,replacement,&second)==0);
    memset(listed,0x5a,sizeof(listed)); count=77;
    CHECK(d3dkmt_enumerate_adapters(4,D3DKMT_MAX_ADAPTERS,listed,&count)==STATUS_NO_MEMORY);
    CHECK(count==77 && listed[0].adapter==0x5a5a5a5a);
    CHECK(d3dkmt_open_adapter(4,other,&second)==0);
    CHECK(d3dkmt_open_adapter(4,other,&first)==STATUS_NO_MEMORY);
    d3dkmt_release_process(3);
    CHECK(d3dkmt_query_adapter(4,second,OKMT_QUERY_DRIVER_VERSION,&value,4,0)==0);
    d3dkmt_release_process(4);
    CHECK(d3dkmt_unregister_adapter(other)==0);
    thread_adapter=replacement;
    pthread_t workers[4];
    for (unsigned i=0;i<4;i++) CHECK(pthread_create(&workers[i],NULL,registry_worker,(void *)(uintptr_t)(10+i))==0);
    for (unsigned i=0;i<4;i++) CHECK(pthread_join(workers[i],NULL)==0);

    /* Exercise the same marshaller with PE32 low addresses and native PE64.
     * A failed copyout must roll back the references allocated by enumeration. */
    user_bytes=4096;
    user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    if (user!=MAP_FAILED) {
        for (int compat=0;compat<=1;compat++) {
            g_compat32_mode=compat;
            const DWORD legacy_counts[]={0,1,16,17,0xffffffffu};
            for (unsigned trial=0;trial<sizeof(legacy_counts)/sizeof(legacy_counts[0]);trial++) {
                memset(user,0x5a,4096);
                OKMT_ENUM_ADAPTERS *legacy=(void *)(user+4);
                legacy->count=legacy_counts[trial];
                CHECK(D3DKMTEnumAdapters(legacy)==0 && legacy->count==1);
                CHECK(*(DWORD *)user==0x5a5a5a5a && legacy->adapters[1].adapter==0x5a5a5a5a);
                CHECK(legacy->adapters[0].luid.low==replacement.low);
                first=legacy->adapters[0].adapter;
                CHECK(d3dkmt_close_adapter(caller+1,first)==STATUS_INVALID_PARAMETER);
                CHECK(d3dkmt_close_adapter(caller,first)==0);
                CHECK(d3dkmt_close_adapter(caller,first)==STATUS_INVALID_PARAMETER);
            }
            memset(user,0x5a,4096);
            denied_first=(uintptr_t)(user+323); denied_last=denied_first+1;
            CHECK(D3DKMTEnumAdapters(user)==STATUS_INVALID_PARAMETER);
            CHECK(*(DWORD *)user==0x5a5a5a5a);
            denied_first=denied_last=0;
            CHECK(D3DKMTEnumAdapters(NULL)==STATUS_INVALID_PARAMETER);
            CHECK(D3DKMTEnumAdapters(user+4096-323)==STATUS_INVALID_PARAMETER);
            CHECK(*(DWORD *)user==0x5a5a5a5a);
            /* Resource exhaustion must leave every output byte unchanged. */
            for (unsigned i=0;i<D3DKMT_MAX_HANDLES;i++)
                CHECK(d3dkmt_open_adapter(caller,replacement,&first)==0);
            CHECK(D3DKMTEnumAdapters(user)==STATUS_NO_MEMORY);
            for (unsigned i=0;i<sizeof(OKMT_ENUM_ADAPTERS);i++) CHECK(user[i]==0x5a);
            d3dkmt_release_process(caller);
            CHECK(D3DKMTEnumAdapters(user)==0);
            CHECK(d3dkmt_close_adapter(caller,((OKMT_ENUM_ADAPTERS *)user)->adapters[0].adapter)==0);
            memset(user,0,4096);
            if (compat) { ((DWORD *)user)[0]=16; ((DWORD *)user)[1]=(DWORD)(uintptr_t)(user+256); }
            else *(OKMT_ENUM_ADAPTERS2 *)user=(OKMT_ENUM_ADAPTERS2){16,(OKMT_ADAPTER_INFO *)(user+256)};
            CHECK(D3DKMTEnumAdapters2(user)==0 && *(DWORD *)user==1);
            first=((OKMT_ADAPTER_INFO *)(user+256))->adapter;
            /* Guard beside output proves the 32-bit pointer wasn't widened. */
            *(DWORD *)(user+1028)=0x4b4d5447;
            if (compat) {
                DWORD *q=(DWORD *)(user+64); q[0]=first; q[1]=13;
                q[2]=(DWORD)(uintptr_t)(user+1024); q[3]=4;
            } else *(OKMT_QUERY_ADAPTER_INFO *)(user+64)=(OKMT_QUERY_ADAPTER_INFO){first,13,user+1024,4};
            CHECK(D3DKMTQueryAdapterInfo(user+64)==0 && *(DWORD *)(user+1024)==3200);
            CHECK(*(DWORD *)(user+1028)==0x4b4d5447);
            caller++;
            CHECK(D3DKMTQueryAdapterInfo(user+64)==STATUS_INVALID_PARAMETER);
            caller--;
            *(OKMT_OPEN_ADAPTER_FROM_LUID *)(user+128)=(OKMT_OPEN_ADAPTER_FROM_LUID){replacement,0};
            CHECK(D3DKMTOpenAdapterFromLuid(user+128)==0);
            second=((OKMT_OPEN_ADAPTER_FROM_LUID *)(user+128))->adapter;
            *(OKMT_CLOSE_ADAPTER *)(user+192)=(OKMT_CLOSE_ADAPTER){second};
            CHECK(D3DKMTCloseAdapter(user+192)==0);
            CHECK(D3DKMTQueryAdapterInfo(user+64)==0);
            d3dkmt_release_process(caller);
            CHECK(D3DKMTQueryAdapterInfo(user+64)==STATUS_INVALID_PARAMETER);
            denied_first=(uintptr_t)(user+256); denied_last=denied_first+20;
            /* More attempts than table slots detect rollback leaks. */
            for (unsigned i=0;i<D3DKMT_MAX_HANDLES+10;i++) {
                *(DWORD *)user=16;
                CHECK(D3DKMTEnumAdapters2(user)==STATUS_INVALID_PARAMETER);
            }
            denied_first=denied_last=0;
            CHECK(D3DKMTEnumAdapters2(user)==0);
            d3dkmt_release_process(caller);
            CHECK(D3DKMTEnumAdapters2(NULL)==STATUS_INVALID_PARAMETER);
            CHECK(D3DKMTQueryAdapterInfo((void *)1)==STATUS_INVALID_PARAMETER);
            CHECK(D3DKMTCloseAdapter((void *)1)==STATUS_INVALID_PARAMETER);
            CHECK(D3DKMTOpenAdapterFromLuid((void *)1)==STATUS_INVALID_PARAMETER);
        }
        CHECK(munmap(user,4096)==0);
    }
    CHECK(d3dkmt_unregister_adapter(replacement)==0);
    CHECK(d3dkmt_enumerate_adapters(1,0,NULL,&count)==0 && count==0);
    printf("D3DKMT checks=%u failures=%u\n",checks,failures);
    return failures ? 1 : 0;
}
