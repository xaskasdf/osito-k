/* Mode publication, stable snapshots and both PE copyout ABIs. Names/driver
 * objects here are test fixtures; no physical screen ownership is inferred. */
#define main modes_registry_suite_main
#define win32_user_range_readable modes_base_readable
#define win32_user_range_writable modes_base_writable
#include "d3dkmt_registry_host.c"
#undef main
#undef win32_user_range_readable
#undef win32_user_range_writable
#include <stdlib.h>

static const OKMT_WCHAR screen_name[]=u"\\\\.\\DISPLAY1";
BOOL gdi32_dc_display_name(HANDLE dc,WCHAR name[32])
{ (void)dc; (void)name; return FALSE; }

static unsigned allocations,frees,fail_allocation;
void *__real_malloc(size_t);
void __real_free(void *);
void *__wrap_malloc(size_t bytes)
{
    if (__atomic_exchange_n(&fail_allocation,0,__ATOMIC_RELAXED)) return NULL;
    void *p=__real_malloc(bytes);
    if (p) __atomic_add_fetch(&allocations,1,__ATOMIC_RELAXED);
    return p;
}
void __wrap_free(void *p)
{
    if (p) __atomic_add_fetch(&frees,1,__ATOMIC_RELAXED);
    __real_free(p);
}

static OKMT_LUID modes_adapter;
static OKMT_DISPLAY_MODE modes_input[5];
static unsigned during_copyout;
int win32_user_range_readable(const void *p,SIZE_T bytes,BOOL compat)
{ return modes_base_readable(p,bytes,compat); }
int win32_user_range_writable(void *p,SIZE_T bytes,BOOL compat)
{
    unsigned action=during_copyout; during_copyout=0;
    if (action==1) CHECK(d3dkmt_publish_display_modes(modes_adapter,1,modes_input,5)==0);
    if (action==2) CHECK(d3dkmt_unbind_display(modes_adapter,1)==0);
    if (action==3) CHECK(d3dkmt_unregister_adapter(modes_adapter)==0);
    return modes_base_writable(p,bytes,compat);
}

static void fill_modes(OKMT_DISPLAY_MODE *modes,DWORD count,DWORD width)
{
    for (DWORD i=0;i<count;i++) modes[i]=(OKMT_DISPLAY_MODE){
        .width=width,.height=720+i,.format=22,.integer_refresh=60,
        .refresh_numerator=60000,.refresh_denominator=1000,
        .scanline_order=1,.orientation=1,.fixed_output=0,.flags={1,0x80}};
}
static BOOL sentinel(const void *data,size_t bytes)
{
    const BYTE *p=data;
    while (bytes--) if (*p++!=0x5a) return FALSE;
    return TRUE;
}
static void header(OKMT_HANDLE adapter,DWORD source,uintptr_t modes,DWORD count)
{
    DWORD head[]={adapter,source};
    memset(user,0x5a,user_bytes);
    memcpy(user,head,8); memcpy(user+8,&modes,g_compat32_mode?4:8);
    memcpy(user+(g_compat32_mode?12:16),&count,4);
}
static DWORD output_count(void)
{
    DWORD value; memcpy(&value,user+(g_compat32_mode?12:16),4); return value;
}

static void publication_cases(void)
{
    unsigned outstanding=allocations-frees;
    D3DKMT_ADAPTER_DESCRIPTION desc={.driver_version=1000,.adapter_type=OKMT_ADAPTER_DISPLAY,.source_count=2};
    CHECK(d3dkmt_register_adapter(&desc,&modes_adapter)==0);
    OKMT_HANDLE handle=0;CHECK(d3dkmt_open_adapter(caller,modes_adapter,&handle)==0);
    D3DKMT_DISPLAY_SNAPSHOT old={0},current={0};
    CHECK(d3dkmt_acquire_display_modes(caller,handle,0,&old)==D3DKMT_STATUS_INVALID_SOURCE);
    CHECK(d3dkmt_acquire_display_modes(caller,handle,0xffffffffu,&old)==D3DKMT_STATUS_INVALID_SOURCE);
    CHECK(d3dkmt_bind_display(modes_adapter,1,screen_name,TRUE)==0);
    CHECK(d3dkmt_acquire_display_modes(caller,handle,1,&old)==D3DKMT_STATUS_NO_DISPLAY_TARGET);
    fill_modes(modes_input,5,1280);
    CHECK(d3dkmt_publish_display_modes(modes_adapter,0,modes_input,3)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_publish_display_modes(modes_adapter,1,NULL,1)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_publish_display_modes(modes_adapter,1,modes_input,0)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_publish_display_modes(modes_adapter,1,modes_input,D3DKMT_MAX_DISPLAY_MODES+1)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_publish_display_modes(modes_adapter,1,modes_input,3)==0);
    CHECK(d3dkmt_acquire_display_modes(caller+1,handle,1,&old)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_acquire_display_modes(caller,handle,1,&old)==0);
    CHECK(old.count==3 && !memcmp(old.modes,modes_input,3*sizeof(*modes_input)));
    CHECK(d3dkmt_acquire_display_modes(caller,handle,1,&old)==STATUS_INVALID_PARAMETER);
    D3DKMT_DISPLAY_SNAPSHOT copy=old;
    CHECK(d3dkmt_release_display_modes(&copy)==STATUS_INVALID_PARAMETER);
    memset(modes_input,0xcc,sizeof(modes_input));
    CHECK(old.modes[0].width==1280 && old.modes[2].height==722);
    fill_modes(modes_input,5,1920);
    fail_allocation=1;
    CHECK(d3dkmt_publish_display_modes(modes_adapter,1,modes_input,5)==STATUS_NO_MEMORY);
    CHECK(d3dkmt_acquire_display_modes(caller,handle,1,&current)==0 && current.count==3);
    CHECK(d3dkmt_release_display_modes(&current)==0);
    OKMT_DISPLAY_MODE invalid=modes_input[0]; invalid.refresh_denominator=0;
    CHECK(d3dkmt_publish_display_modes(modes_adapter,1,&invalid,1)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_publish_display_modes(modes_adapter,1,modes_input,5)==0);
    CHECK(d3dkmt_acquire_display_modes(caller,handle,1,&current)==0 && current.count==5);
    CHECK(current.modes[0].width==1920 && old.modes[0].width==1280);
    CHECK(d3dkmt_release_display_modes(&current)==0);
    CHECK(d3dkmt_publish_display_modes(modes_adapter,1,NULL,0)==0);
    CHECK(d3dkmt_acquire_display_modes(caller,handle,1,&current)==D3DKMT_STATUS_NO_DISPLAY_TARGET);
    CHECK(d3dkmt_unbind_display(modes_adapter,1)==0);
    CHECK(d3dkmt_acquire_display_modes(caller,handle,1,&current)==D3DKMT_STATUS_INVALID_SOURCE);
    CHECK(d3dkmt_unregister_adapter(modes_adapter)==0);
    CHECK(old.modes[0].width==1280 && old.modes[2].height==722);
    CHECK(d3dkmt_release_display_modes(&old)==0);
    CHECK(d3dkmt_release_display_modes(&old)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_acquire_display_modes(caller,handle,1,&current)==D3DKMT_STATUS_DEVICE_REMOVED);
    CHECK(d3dkmt_close_adapter(caller,handle)==0);
    CHECK(d3dkmt_acquire_display_modes(caller,handle,1,&current)==STATUS_INVALID_PARAMETER);
    CHECK(allocations-frees==outstanding);
    for (unsigned prototype=0;prototype<2;prototype++) {
        desc.adapter_type=prototype ? OKMT_ADAPTER_DISPLAY|OKMT_ADAPTER_PROTOTYPE : 0;
        CHECK(d3dkmt_register_adapter(&desc,&modes_adapter)==0);
        CHECK(d3dkmt_open_adapter(caller,modes_adapter,&handle)==0);
        CHECK(d3dkmt_acquire_display_modes(caller,handle,0xffffffffu,&current)==STATUS_NOT_SUPPORTED);
        CHECK(d3dkmt_publish_display_modes(modes_adapter,0,modes_input,5)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_unregister_adapter(modes_adapter)==0);
        CHECK(d3dkmt_close_adapter(caller,handle)==0);
    }
}

static void marshalling_cases(void)
{
    user_bytes=8192;
    user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);if(user==MAP_FAILED)return;
    for (int compat=0;compat<=1;compat++) {
        g_compat32_mode=compat;
        D3DKMT_ADAPTER_DESCRIPTION desc={.driver_version=1000,.adapter_type=OKMT_ADAPTER_DISPLAY,.source_count=2};
        CHECK(d3dkmt_register_adapter(&desc,&modes_adapter)==0);
        CHECK(d3dkmt_bind_display(modes_adapter,1,screen_name,TRUE)==0);
        fill_modes(modes_input,5,1280);
        CHECK(d3dkmt_publish_display_modes(modes_adapter,1,modes_input,3)==0);
        OKMT_HANDLE handle; CHECK(d3dkmt_open_adapter(caller,modes_adapter,&handle)==0);
        for (unsigned mode=0;mode<9;mode++) {
            DWORD capacities[]={0,1,0,1,2,3,4,0xffffffffu,3};
            uintptr_t destination=mode<2 ? 0 : (uintptr_t)(user+(mode==8?user_bytes-3*sizeof(*modes_input):512));
            header(handle,1,destination,capacities[mode]);
            BYTE before[24];memcpy(before,user,sizeof(before));
            NTSTATUS status=D3DKMTGetDisplayModeList(user);
            CHECK(status==(mode>=2 && mode<=4 ? D3DKMT_STATUS_BUFFER_TOO_SMALL : 0));
            CHECK(output_count()==3);
            CHECK(!memcmp(user,before,compat?12:16));
            CHECK(sentinel(user+(compat?16:20),4));
            if (mode<=4) CHECK(sentinel(user+512,3*sizeof(*modes_input)));
            else CHECK(!memcmp((void *)destination,modes_input,3*sizeof(*modes_input)));
            if (mode!=8) CHECK(sentinel(user+512+3*sizeof(*modes_input),44));
        }
        for (unsigned denied=0;denied<3;denied++) {
            header(handle,1,(uintptr_t)(user+512),7);
            denied_first=(uintptr_t)(user+(denied==0?(compat?12:16):denied==1?512:512+131));
            denied_last=denied_first+1;
            CHECK(D3DKMTGetDisplayModeList(user)==STATUS_INVALID_PARAMETER);
            CHECK(output_count()==7 && sentinel(user+512,132));
            denied_first=denied_last=0;
        }
        header(handle,1,1,7);
        CHECK(D3DKMTGetDisplayModeList(user)==STATUS_INVALID_PARAMETER && output_count()==7);
        header(handle,1,1,0);
        CHECK(D3DKMTGetDisplayModeList(user)==D3DKMT_STATUS_BUFFER_TOO_SMALL && output_count()==3);
        CHECK(D3DKMTGetDisplayModeList(NULL)==STATUS_INVALID_PARAMETER);
        CHECK(D3DKMTGetDisplayModeList(user+user_bytes-4)==STATUS_INVALID_PARAMETER);
        header(handle,1,(uintptr_t)(user+512),0xffffffffu);
        cancellation_requested=TRUE;
        unsigned deliveries=cancellation_deliveries;
        CHECK(D3DKMTGetDisplayModeList(user)==STATUS_PROCESS_IS_TERMINATING);
        CHECK(cancellation_deliveries==deliveries+1 && output_count()==0xffffffffu);
        cancellation_requested=FALSE;
        /* An update after snapshot acquisition must return the old complete
         * vector, not mix its count or storage with the replacement. */
        header(handle,1,(uintptr_t)(user+512),3);
        fill_modes(modes_input,5,1920);during_copyout=1;
        CHECK(D3DKMTGetDisplayModeList(user)==0 && output_count()==3);
        CHECK(((OKMT_DISPLAY_MODE *)(user+512))[0].width==1280);
        header(handle,1,(uintptr_t)(user+512),3);
        CHECK(D3DKMTGetDisplayModeList(user)==D3DKMT_STATUS_BUFFER_TOO_SMALL && output_count()==5);
        header(handle,1,(uintptr_t)(user+512),5);during_copyout=2;
        CHECK(D3DKMTGetDisplayModeList(user)==0 && output_count()==5);
        CHECK(!memcmp(user+512,modes_input,5*sizeof(*modes_input)));
        header(handle,1,0,0);
        CHECK(D3DKMTGetDisplayModeList(user)==D3DKMT_STATUS_INVALID_SOURCE && output_count()==0);
        CHECK(d3dkmt_bind_display(modes_adapter,1,screen_name,TRUE)==0);
        CHECK(d3dkmt_publish_display_modes(modes_adapter,1,modes_input,5)==0);
        header(handle,1,(uintptr_t)(user+512),5);during_copyout=3;
        CHECK(D3DKMTGetDisplayModeList(user)==0 && output_count()==5);
        CHECK(!memcmp(user+512,modes_input,5*sizeof(*modes_input)));
        header(handle,1,0,0);
        CHECK(D3DKMTGetDisplayModeList(user)==D3DKMT_STATUS_DEVICE_REMOVED && output_count()==0);
        CHECK(d3dkmt_close_adapter(caller,handle)==0);
    }
    CHECK(munmap(user,user_bytes)==0);user=NULL;user_bytes=0;
}

static OKMT_HANDLE threaded_handle;
static void *update_worker(void *arg)
{
    OKMT_DISPLAY_MODE modes[5];
    for (unsigned iteration=0;iteration<2000;iteration++) {
        if (arg) {
            DWORD count=iteration&1?3:5;fill_modes(modes,count,count==3?1280:1920);
            CHECK(d3dkmt_publish_display_modes(modes_adapter,1,modes,count)==0);
        } else {
            D3DKMT_DISPLAY_SNAPSHOT snapshot={0};
            CHECK(d3dkmt_acquire_display_modes(caller,threaded_handle,1,&snapshot)==0);
            if (!snapshot.self) continue;
            CHECK(snapshot.count==3 || snapshot.count==5);
            for (DWORD i=0;i<snapshot.count;i++) {
                CHECK(snapshot.modes[i].width==(snapshot.count==3?1280u:1920u));
                CHECK(snapshot.modes[i].height==720+i);
            }
            CHECK(d3dkmt_release_display_modes(&snapshot)==0);
        }
    }
    return NULL;
}
static void concurrent_cases(void)
{
    D3DKMT_ADAPTER_DESCRIPTION desc={.driver_version=1000,.adapter_type=OKMT_ADAPTER_DISPLAY,.source_count=2};
    CHECK(d3dkmt_register_adapter(&desc,&modes_adapter)==0);
    CHECK(d3dkmt_bind_display(modes_adapter,1,screen_name,TRUE)==0);
    fill_modes(modes_input,5,1920);
    CHECK(d3dkmt_publish_display_modes(modes_adapter,1,modes_input,5)==0);
    CHECK(d3dkmt_open_adapter(caller,modes_adapter,&threaded_handle)==0);
    pthread_t threads[3];
    CHECK(pthread_create(&threads[0],NULL,update_worker,(void *)1)==0);
    for (unsigned i=1;i<3;i++) CHECK(pthread_create(&threads[i],NULL,update_worker,NULL)==0);
    for (unsigned i=0;i<3;i++) CHECK(pthread_join(threads[i],NULL)==0);
    CHECK(d3dkmt_unregister_adapter(modes_adapter)==0);
    CHECK(d3dkmt_close_adapter(caller,threaded_handle)==0);
}

static void large_and_retirement_cases(void)
{
    D3DKMT_ADAPTER_DESCRIPTION desc={.driver_version=1000,.adapter_type=OKMT_ADAPTER_DISPLAY,.source_count=64};
    CHECK(d3dkmt_register_adapter(&desc,&modes_adapter)==0);
    CHECK(d3dkmt_bind_display(modes_adapter,1,screen_name,TRUE)==0);
    const DWORD count=D3DKMT_MAX_DISPLAY_MODES;
    OKMT_DISPLAY_MODE *input=__real_malloc((size_t)count*sizeof(*input));
    CHECK(input);if(!input)return;
    fill_modes(input,count,1920);
    CHECK(d3dkmt_publish_display_modes(modes_adapter,1,input,count)==0);
    OKMT_HANDLE handle;CHECK(d3dkmt_open_adapter(caller,modes_adapter,&handle)==0);
    user_bytes=512+(size_t)count*sizeof(*input)+4096;
    user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    if (user!=MAP_FAILED) {
        for (int compat=0;compat<=1;compat++) {
            g_compat32_mode=compat;
            header(handle,1,(uintptr_t)(user+512),count-1);
            CHECK(D3DKMTGetDisplayModeList(user)==D3DKMT_STATUS_BUFFER_TOO_SMALL);
            CHECK(output_count()==count && sentinel(user+512,(size_t)count*sizeof(*input)));
            header(handle,1,(uintptr_t)(user+512),count);
            CHECK(D3DKMTGetDisplayModeList(user)==0 && output_count()==count);
            CHECK(!memcmp(user+512,input,(size_t)count*sizeof(*input)));
            CHECK(sentinel(user+512+(size_t)count*sizeof(*input),4096));
        }
        CHECK(munmap(user,user_bytes)==0);
    }
    __real_free(input);user=NULL;user_bytes=0;
    CHECK(d3dkmt_unbind_display(modes_adapter,1)==0);
    /* Every source slot is reused only after dropping its published ref;
     * externally retained immutable snapshots survive adapter replacement. */
    D3DKMT_DISPLAY_SNAPSHOT snapshots[64]={0};
    OKMT_DISPLAY_MODE mode;fill_modes(&mode,1,1280);
    for (unsigned i=0;i<64;i++) {
        OKMT_WCHAR name[8]={'d',(OKMT_WCHAR)('A'+i/10),(OKMT_WCHAR)('0'+i%10),0};
        CHECK(d3dkmt_bind_display(modes_adapter,i,name,FALSE)==0);
        mode.width=1000+i;
        CHECK(d3dkmt_publish_display_modes(modes_adapter,i,&mode,1)==0);
        CHECK(d3dkmt_acquire_display_modes(caller,handle,i,&snapshots[i])==0);
    }
    CHECK(d3dkmt_unregister_adapter(modes_adapter)==0);
    CHECK(d3dkmt_close_adapter(caller,handle)==0);
    CHECK(d3dkmt_register_adapter(&desc,&modes_adapter)==0);
    CHECK(d3dkmt_bind_display(modes_adapter,1,screen_name,TRUE)==0);
    CHECK(d3dkmt_publish_display_modes(modes_adapter,1,&mode,1)==0);
    CHECK(d3dkmt_unregister_adapter(modes_adapter)==0);
    for (unsigned i=0;i<64;i++) {
        CHECK(snapshots[i].count==1 && snapshots[i].modes[0].width==1000+i);
        CHECK(d3dkmt_release_display_modes(&snapshots[i])==0);
    }
}

int main(void)
{
    if (modes_registry_suite_main()) return 1;
    publication_cases();marshalling_cases();concurrent_cases();large_and_retirement_cases();
    CHECK(allocations==frees);
    printf("KMTMODES checks=%u failures=%u allocations=%u frees=%u\n",checks,failures,allocations,frees);
    return failures ? 1 : 0;
}
