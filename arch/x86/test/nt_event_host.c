/* Real NT event pool + handle table. Scheduler/IOCP callbacks are fixtures. */
#define main address_suite_main
#include "address_wait_host.c"
#undef main
#define HANDLE_HOST_TEST 1
#include "../win32/handle.c"
HANDLE_TABLE g_handle_table;
static unsigned handle_depth, event_cleanup;
uint64_t handle_host_lock(void) { CHECK(!handle_depth && !lock_depth); handle_depth=1; return 0x200; }
void handle_host_unlock(uint64_t f) { CHECK(handle_depth==1 && f==0x200); handle_depth=0; }
void k32_iocp_release_event(PVOID p)
{
    EVENT_OBJECT *e=p;
    CHECK(!handle_depth && !lock_depth && e->allocated && !e->references);
    CHECK(!ntsync_event_retain(e)); /* no resurrection during final cleanup */
    event_cleanup++;
}
void advapi32_token_retain(PCVOID p) { (void)p; CHECK(0); }
void advapi32_token_release(PCVOID p) { (void)p; CHECK(0); }
void nt_job_handle_retain(PVOID p) { (void)p; CHECK(0); }
void nt_job_handle_release(PVOID p) { (void)p; CHECK(0); }
void nt_process_token_handle_closed(OBJECT_TYPE_ID t,PVOID p) { (void)t; (void)p; CHECK(0); }
BOOL nt_section_allows_access_escalation(PVOID p,ACCESS_MASK a) { (void)p; (void)a; return FALSE; }
NTSTATUS nt_close_handle_for_process(HANDLE h,ULONG pid)
{ return handle_close_entry_for_process(&g_handle_table,h,pid,NULL); }
NTSTATUS kernel32_nt_open_named_event(POBJECT_ATTRIBUTES a,ACCESS_MASK b,PHANDLE c)
{ (void)a; (void)b; (void)c; CHECK(0); return STATUS_NOT_SUPPORTED; }
NTSTATUS kernel32_nt_publish_named_event(POBJECT_ATTRIBUTES a,ACCESS_MASK b,HANDLE c,PHANDLE d,BOOL *e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; CHECK(0); return STATUS_NOT_SUPPORTED; }
int main(void)
{
    CHECK(!address_suite_main());
    current_pid=7; handle_table_init(&g_handle_table);
    /* An unused slot must never follow zero-filled owner record zero. That
     * record may be free after an unrelated handle closed, with next == 0. */
    printf("NT HANDLE empty-slot regression start\n");
    HANDLE unused=INDEX_TO_HANDLE(MAX_HANDLES-1);
    ULONG unused_flags=0x12345678;
    CHECK(handle_retain_for_process(&g_handle_table,unused,8)==STATUS_INVALID_HANDLE);
    CHECK(handle_query_flags_for_process(&g_handle_table,unused,7,&unused_flags)==STATUS_INVALID_HANDLE);
    CHECK(unused_flags==0x12345678);
    CHECK(handle_update_flags_for_process(&g_handle_table,unused,7,1,1)==STATUS_INVALID_HANDLE);
    CHECK(handle_inherit_all_for_process(&g_handle_table,7,8)==0 && !g_handle_table.count);
    HANDLE first=NULL,live=NULL;
    CHECK(handle_alloc(&g_handle_table,OBJ_TYPE_SEMAPHORE,GENERIC_ALL,(PVOID)1,&first)==0);
    CHECK(handle_alloc(&g_handle_table,OBJ_TYPE_SEMAPHORE,GENERIC_ALL,(PVOID)2,&live)==0);
    CHECK(handle_update_flags_for_process(&g_handle_table,live,7,1,1)==0);
    CHECK(handle_close(&g_handle_table,first)==0);
    CHECK(handle_inherit_all_for_process(&g_handle_table,7,8)==0);
    PVOID inherited=NULL;
    CHECK(handle_lookup_for_process(&g_handle_table,live,8,OBJ_TYPE_SEMAPHORE,&inherited)==0 && inherited==(PVOID)2);
    CHECK(handle_close_entry_for_process(&g_handle_table,live,8,NULL)==0);
    CHECK(handle_close(&g_handle_table,live)==0 && !g_handle_table.count);
    printf("NT HANDLE empty-slot regression PASS\n");
    for (unsigned round=0;round<1000;round++) {
        HANDLE h=NULL,duplicate=NULL,restricted=NULL,reused=NULL;
        ULONG_PTR args[]={(ULONG_PTR)&h,GENERIC_ALL,0,0,FALSE};
        CHECK(sys_NtCreateEvent(args)==0 && h);
        PVOID owned=NULL,denied=(PVOID)1;
        CHECK(ntsync_reference_event_for_process(h,8,&denied)==STATUS_INVALID_HANDLE && denied==(PVOID)1);
        CHECK(ntsync_reference_event_for_process((HANDLE)((ULONG_PTR)h+1),7,&denied)==STATUS_INVALID_HANDLE);
        CHECK(ntsync_reference_event_for_process((HANDLE)((ULONG_PTR)h+(1ULL<<32)),7,&denied)==STATUS_INVALID_HANDLE);
        CHECK(ntsync_reference_event_for_process(h,7,&owned)==0 && owned);
        EVENT_OBJECT *e=owned;
        CHECK(e->allocated && e->references==2 && !e->signaled);
        CHECK(handle_duplicate_for_process(&g_handle_table,h,7,&g_handle_table,&duplicate,8,0,FALSE,DUPLICATE_SAME_ACCESS)==0);
        CHECK(handle_duplicate_for_process(&g_handle_table,h,7,&g_handle_table,&restricted,7,0x100000,FALSE,0)==0);
        CHECK(ntsync_reference_event_for_process(restricted,7,&denied)==STATUS_ACCESS_DENIED && denied==(PVOID)1);
        CHECK(e->references==4);
        CHECK(handle_retain_for_process(&g_handle_table,h,9)==0 && e->references==4);
        CHECK(nt_close_handle_for_process(h,7)==0 && e->references==4);
        CHECK(nt_close_handle_for_process(h,9)==0 && e->references==3);
        CHECK(nt_close_handle_for_process(duplicate,8)==0 && e->references==2);
        CHECK(nt_close_handle_for_process(restricted,7)==0 && e->references==1);
        unsigned before=event_cleanup;
        args[0]=(ULONG_PTR)&reused;
        CHECK(sys_NtCreateEvent(args)==0 && reused==h);
        PVOID other=NULL;
        CHECK(handle_lookup(&g_handle_table,reused,OBJ_TYPE_EVENT,&other)==0 && other!=e);
        ntsync_signal_event_reference(owned,7);
        CHECK(e->signaled && !((EVENT_OBJECT *)other)->signaled && event_cleanup==before);
        ntsync_event_release(owned);
        CHECK(!e->allocated && event_cleanup==before+1);
        CHECK(nt_close_handle_for_process(reused,7)==0 && event_cleanup==before+2);
        CHECK(!g_handle_table.count);
    }
    /* Failure to allocate a handle must release the creator's pool reference. */
    HANDLE fillers[MAX_HANDLES],failed=(HANDLE)1;
    unsigned count=0;
    while (handle_alloc(&g_handle_table,OBJ_TYPE_SEMAPHORE,0,(PVOID)1,&fillers[count])==0) count++;
    unsigned before=event_cleanup;
    ULONG_PTR args[]={(ULONG_PTR)&failed,GENERIC_ALL,0,0,FALSE};
    CHECK(sys_NtCreateEvent(args)==STATUS_INSUFFICIENT_RESOURCES && !failed && event_cleanup==before+1);
    for (unsigned i=0;i<count;i++) CHECK(handle_close(&g_handle_table,fillers[i])==0);
    for (unsigned i=0;i<MAX_EVENTS;i++) CHECK(!g_events[i].allocated);
    printf("NT EVENT HOST PASS checks=%u cleanup=%u\n",checks,event_cleanup);
    return 0;
}
