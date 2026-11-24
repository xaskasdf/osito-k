/* Public pointer capture and lifetime tests. Synthetic provider, real registry. */
#define win32_user_range_writable offer_base_writable
#define win32_termination_requested offer_base_cancelled
#define D3DKMT_PAGING_SUITE_ENTRY offer_paging_regression_main
#include "d3dkmt_paging_host.c"
#undef win32_user_range_writable
#undef win32_termination_requested

static unsigned offer_writes,offer_cancels,offer_deny_at,offer_cancel_at,offer_hook;
static unsigned offer_prepares,offer_commits,offer_finishes,offer_frees;
static NTSTATUS offer_failure;
static OKMT_HANDLE offer_queue,offer_groups[2],offer_objects[4];
static BOOL offer_live[4],offer_marked[4];
static DWORD offer_lost[4];
typedef struct {DWORD mode,count,cookies[1024];BOOL committed;} OFFER_MODEL;
_Static_assert(sizeof(OFFER_MODEL)<=D3DKMT_OFFER_STORAGE_BYTES,"offer model fits");
int win32_user_range_writable(void *p,SIZE_T bytes,BOOL compat)
{
    if (++offer_writes==offer_deny_at) return 0;
    return offer_base_writable(p,bytes,compat);
}
BOOL win32_termination_requested(void)
{
    if (++offer_cancels==offer_cancel_at) cancellation_requested=TRUE;
    return offer_base_cancelled();
}
static NTSTATUS offer_allocate(void *opaque,DWORD pid,uint64_t device,
    const D3DKMT_ALLOCATION_DESCRIPTION *description,uint64_t *cookie)
{
    CHECK(opaque==&ctx && pid==100 && device && description->bytes==4096);
    for (unsigned i=0;i<4;i++) if (!offer_live[i]) {offer_live[i]=TRUE;*cookie=i+1;return 0;}
    return STATUS_NO_MEMORY;
}
static NTSTATUS offer_free(void *opaque,DWORD pid,uint64_t device,uint64_t cookie)
{
    CHECK(opaque==&ctx && pid==100 && device && cookie && cookie<=4 && offer_live[cookie-1]);
    offer_live[cookie-1]=FALSE;offer_frees++;return 0;
}
static NTSTATUS offer_prepare(void *opaque,DWORD pid,uint64_t device,DWORD mode,DWORD priority,
    DWORD flags,const uint64_t *cookies,DWORD count,void *storage,DWORD *values)
{
    CHECK(opaque==&ctx && pid==100 && device && count && count<=1024 && d3dkmt_deferred_current());
    CHECK(mode==D3DKMT_OFFER ? priority>=1 && priority<=4 && flags<=3 : !priority && !flags);
    OFFER_MODEL *m=storage;m->mode=mode;m->count=count;m->committed=FALSE;offer_prepares++;
    for (unsigned i=0;i<count;i++) {
        CHECK(cookies[i] && cookies[i]<=4 && offer_live[cookies[i]-1]);
        m->cookies[i]=cookies[i];values[i]=offer_lost[cookies[i]-1];
    }
    unsigned action=offer_hook;offer_hook=0;
    if (action==1) {memset(user,0xcc,64);memset(user+256,0xff,4096);}
    if (action==2) CHECK(d3dkmt_destroy_paging_queue(caller,offer_queue)==0);
    if (action==3) CHECK(d3dkmt_destroy_allocations(caller,sync_device,1,offer_objects)==0);
    if (action==4) CHECK(d3dkmt_destroy_device(caller,sync_device)==0);
    if (action==5) d3dkmt_release_process(caller);
    if (action==6) CHECK(d3dkmt_unregister_adapter(ctx.luid)==D3DKMT_STATUS_DEVICE_BUSY);
    if (action==7) CHECK(d3dkmt_mark_device_error(caller,sync_device,6)==0);
    if (action==8) CHECK(d3dkmt_destroy_resource(caller,sync_device,offer_groups[0])==0);
    if (action==9) {
        D3DKMT_ALLOCATION_DESCRIPTION d={4096};OKMT_HANDLE group=0,object=0;
        CHECK(d3dkmt_create_resource_allocations(caller,sync_device,offer_groups[0],FALSE,1,&d,&group,&object)==D3DKMT_STATUS_DEVICE_BUSY);
    }
    if (action==10) values[count-1]=2; /* Unsupported commitment-loss result. */
    for (unsigned i=0;i<count;i++) CHECK(offer_live[cookies[i]-1]);
    return offer_failure;
}
static NTSTATUS offer_commit(void *storage)
{
    OFFER_MODEL *m=storage;CHECK(!m->committed);m->committed=TRUE;offer_commits++;
    for (DWORD i=0;i<m->count;i++) offer_marked[m->cookies[i]-1]=m->mode==D3DKMT_OFFER;
    return 0;
}
static void offer_finish(void *storage)
{
    OFFER_MODEL *m=storage;offer_finishes++;
    for (DWORD i=0;i<m->count;i++) CHECK(offer_live[m->cookies[i]-1]);
}
static const D3DKMT_OFFER_PROVIDER offer_model={offer_prepare,offer_commit,offer_finish};
static void offer_setup(BOOL compat,BOOL supported)
{
    caller=100;g_compat32_mode=compat;cancellation_requested=FALSE;denied_first=denied_last=0;
    offer_writes=offer_cancels=offer_deny_at=offer_cancel_at=offer_hook=0;
    offer_prepares=offer_commits=offer_finishes=offer_frees=0;offer_failure=0;
    memset(offer_live,0,sizeof(offer_live));memset(offer_marked,1,sizeof(offer_marked));
    memset(offer_lost,0,sizeof(offer_lost));offer_lost[3]=1;
    monitored_creates=monitored_closes=monitored_race=0;monitored_fail_close=monitored_poison_output=false;
    user_bytes=16384;user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);CHECK(user!=MAP_FAILED);
    D3DKMT_ALLOCATION_PROVIDER memory={.allocate=offer_allocate,.close=offer_free,.offer=supported?&offer_model:NULL};
    D3DKMT_RENDER_PROVIDER render={.submit=sync_render_unused,.synchronize=sync_submit};
    D3DKMT_MONITORED_PROVIDER monitored={monitored_create,monitored_close,monitored_notify};
    CHECK(d3dkmt_register_monitored_driver(&adapter_desc,&device_ops,&memory,&context_ops,&render,&monitored,&ctx.luid)==0);
    OKMT_HANDLE adapter=0;CHECK(d3dkmt_open_adapter(caller,ctx.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(caller,adapter,0,&sync_device)==0);
    OKMT_HANDLE sync=0;uint64_t cpu=0;CHECK(d3dkmt_create_paging_queue(caller,sync_device,0,compat,&offer_queue,&sync,&cpu)==0);
    D3DKMT_ALLOCATION_DESCRIPTION input[2]={{4096},{4096}};
    for (unsigned i=0;i<2;i++) CHECK(d3dkmt_create_resource_allocations(caller,sync_device,0,TRUE,2,input,&offer_groups[i],offer_objects+i*2)==0);
}
static void offer_teardown(void)
{
    offer_hook=offer_deny_at=offer_cancel_at=0;cancellation_requested=FALSE;caller=100;
    d3dkmt_release_process(caller);for (unsigned i=0;i<12;i++) (void)d3dkmt_collect_devices();
    CHECK(d3dkmt_unregister_adapter(ctx.luid)==0 && offer_frees==4 && monitored_closes==1);
    CHECK(!munmap(user,user_bytes));user=NULL;
}
static void offer_header(DWORD mode,BOOL groups,DWORD count,DWORD priority,DWORD flags,BOOL output)
{
    memset(user,0,64);memset(user+8192,0xa5,4096);offer_writes=offer_cancels=0;
    OKMT_HANDLE *list=(void *)(user+256);DWORD *result=(void *)(user+8192);
    for (unsigned i=0;i<count && i<1024;i++) list[i]=groups?offer_groups[(i&1)?0:1]:offer_objects[(i&1)?0:3];
    DWORD *p=(void *)user;
    if (g_compat32_mode) {
        p[0]=mode==D3DKMT_RECLAIM2?offer_queue:sync_device;
        p[mode==D3DKMT_RECLAIM2?(groups?2:3):(groups?1:2)]=(DWORD)(uintptr_t)list;
        if (mode==D3DKMT_OFFER) {p[3]=count;p[4]=priority;p[5]=flags;}
        else if (mode==D3DKMT_RECLAIM) {p[3]=output?(DWORD)(uintptr_t)result:0;p[4]=count;}
        else {p[1]=count;p[4]=output?(DWORD)(uintptr_t)result:0;memset(user+24,0xa5,8);}
    } else if (mode==D3DKMT_OFFER) {
        *(OKMT_OFFER_ALLOCATIONS *)user=(OKMT_OFFER_ALLOCATIONS){sync_device,groups?list:NULL,groups?NULL:list,count,priority,flags};
    } else if (mode==D3DKMT_RECLAIM) {
        *(OKMT_RECLAIM_ALLOCATIONS *)user=(OKMT_RECLAIM_ALLOCATIONS){sync_device,groups?list:NULL,groups?NULL:list,output?result:NULL,count};
    } else *(OKMT_RECLAIM_ALLOCATIONS2 *)user=(OKMT_RECLAIM_ALLOCATIONS2){offer_queue,count,groups?list:NULL,groups?NULL:list,output?result:NULL,0xa5a5a5a5a5a5a5a5ULL};
}
static NTSTATUS offer_call(DWORD mode)
{return mode==0?D3DKMTOfferAllocations(user):mode==1?D3DKMTReclaimAllocations(user):D3DKMTReclaimAllocations2(user);}
static void offer_abi_cases(BOOL compat,DWORD mode,BOOL groups)
{
    for (unsigned variant=0;variant<5;variant++) {
        offer_setup(compat,TRUE);DWORD count=variant==4?1024:3;
        offer_header(mode,groups,count,variant+1,variant&3,TRUE);
        if (mode==0 && variant==4) {offer_header(mode,groups,count,4,3,TRUE);}
        CHECK(offer_call(mode)==0 && offer_prepares==1 && offer_commits==1 && offer_finishes==1);
        if (mode) for (unsigned i=0;i<count;i++) CHECK(((DWORD *)(user+8192))[i]==((i&1)?0:1));
        if (mode==2) {uint64_t fence;memcpy(&fence,user+(compat?24:32),8);CHECK(!fence);}
        offer_teardown();
    }
    offer_setup(compat,TRUE);offer_header(mode,groups,3,2,0,FALSE);CHECK(offer_call(mode)==0);offer_teardown();
    for (unsigned bad=0;bad<9;bad++) {
        offer_setup(compat,TRUE);offer_header(mode,groups,3,2,0,TRUE);
        if (bad==0) ((DWORD *)user)[0]=0;
        if (bad==1) ((OKMT_HANDLE *)(user+256))[2]=0;
        if (bad==2) ((OKMT_HANDLE *)(user+256))[2]=groups?offer_objects[0]:offer_groups[0];
        if (bad==3) caller=101;
        if (bad==4) user_bytes=260; /* Partial captured handle list. */
        if (bad==5) user_bytes=4; /* Partial header. */
        if (bad==6) offer_cancel_at=1;
        if (bad==7) offer_cancel_at=2;
        if (bad==8) offer_failure=STATUS_NO_MEMORY;
        BYTE header[64];memcpy(header,user,64);
        CHECK(offer_call(mode)!=0 && !offer_commits && !memcmp(header,user,64));
        user_bytes=16384;
        for (unsigned i=0;i<3;i++) CHECK(((DWORD *)(user+8192))[i]==0xa5a5a5a5u);
        offer_teardown();
    }
    if (mode) for (unsigned deny=1;deny<=(mode==2?4u:2u);deny++) {
        offer_setup(compat,TRUE);offer_header(mode,groups,3,2,0,TRUE);offer_deny_at=deny;
        CHECK(offer_call(mode)==STATUS_INVALID_PARAMETER && !offer_commits);
        CHECK(((DWORD *)(user+8192))[0]==0xa5a5a5a5u);offer_teardown();
    }
    offer_setup(compat,FALSE);offer_header(mode,groups,3,2,0,TRUE);
    CHECK(offer_call(mode)==STATUS_NOT_SUPPORTED && !offer_prepares);offer_teardown();
    for (unsigned action=1;action<=10;action++) {
        if ((action==2 && mode!=2) || (action==9 && !groups) || (action==10 && !mode)) continue;
        offer_setup(compat,TRUE);offer_header(mode,groups,3,2,0,TRUE);offer_hook=action;
        NTSTATUS status=offer_call(mode);
        if (action==1 || action==9) CHECK(!status && offer_commits==1);
        else CHECK(status!=0 && !offer_commits);
        CHECK(offer_finishes==1);offer_teardown();
    }
}
int main(void)
{
    unsigned before=checks;
    for (unsigned compat=0;compat<2;compat++) for (DWORD mode=0;mode<3;mode++)
        for (unsigned groups=0;groups<2;groups++) offer_abi_cases(compat,mode,groups);
    printf("D3DKMT offer/reclaim ABI: checks=%u failures=%u PE32/PE64\n",checks-before,failures);
    return failures?1:0;
}
