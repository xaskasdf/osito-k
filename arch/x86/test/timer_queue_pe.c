typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API void *CALL GetStdHandle(UINT);
API int CALL WriteFile(void *,const void *,UINT,UINT *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API UINT CALL GetLastError(void);
API void CALL SetLastError(UINT);
void *memset(void *p,int c,UPTR n){unsigned char *s=p;for(UPTR i=0;i<n;i++)s[i]=(unsigned char)c;return p;}
static unsigned failures;
#define SAY(s) do{UINT n;WriteFile(GetStdHandle((UINT)-11),s,sizeof(s)-1,&n,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("TQUEUE PASS " name "\n");else{SAY("TQUEUE FAIL " name "\n");failures++;}}while(0)
#define INVALID ((void *)(UPTR)-1)
#define FN(name,type) type name=(type)GetProcAddress(kernel,#name)
typedef void (CALL *callback_fn)(void *,BYTE);
typedef void *(CALL *queue_fn)(void);
typedef int (CALL *create_fn)(void **,void *,callback_fn,void *,UINT,UINT,UINT);
typedef int (CALL *change_fn)(void *,void *,UINT,UINT);
typedef int (CALL *delete_fn)(void *,void *,void *);
typedef int (CALL *delqueue_fn)(void *,void *);
typedef void *(CALL *event_fn)(void *,int,int,const char *);
typedef int (CALL *handle_fn)(void *);
typedef UINT (CALL *wait_fn)(void *,UINT);
typedef UINT (CALL *tid_fn)(void);
typedef void (CALL *sleep_fn)(UINT);
typedef UINT (CALL *threadproc_fn)(void *);
typedef void *(CALL *thread_fn)(void *,UPTR,threadproc_fn,void *,UINT,UINT *);
static wait_fn wait_one;
static handle_fn set_event;
static tid_fn thread_id;
static delete_fn delete_timer;
static void *queue;

typedef struct {
    volatile UINT count,bad,tid;
    UINT tag,threshold;
    void *entered,*release,*returned,*timer,*completion;
    volatile UINT delete_result,delete_error;
    int self_delete;
} State;

static void CALL callback(void *parameter,BYTE fired)
{
    State *state=parameter;
    if(!state||state->tag!=0x1234abcd)return;
    if(fired!=1)__atomic_add_fetch(&state->bad,1,__ATOMIC_SEQ_CST);
    state->tid=thread_id();
    UINT count=__atomic_add_fetch(&state->count,1,__ATOMIC_SEQ_CST);
    if(count>=state->threshold)set_event(state->entered);
    if(state->self_delete){
        SetLastError(0);state->delete_result=delete_timer(queue,state->timer,0);
        state->delete_error=GetLastError();
    }
    if(state->release&&wait_one(state->release,10000)!=0)__atomic_add_fetch(&state->bad,1,__ATOMIC_SEQ_CST);
    if(state->returned)set_event(state->returned);
}

static UINT CALL delete_worker(void *parameter)
{
    State *state=parameter;
    set_event(state->completion);
    state->delete_result=delete_timer(queue,state->timer,INVALID);
    state->delete_error=GetLastError();
    set_event(state->returned);
    return 0;
}

static void init(State *state,event_fn event,UINT threshold,int blocking)
{
    memset(state,0,sizeof(*state));state->tag=0x1234abcd;state->threshold=threshold;
    state->entered=event(0,1,0,0);
    if(blocking)state->release=event(0,1,0,0);
}
static void close_state(State *state,handle_fn close)
{
    if(state->entered)close(state->entered);
    if(state->release)close(state->release);
    if(state->returned)close(state->returned);
    if(state->completion)close(state->completion);
}

void CALL timer_queue_entry(void)
{
    void *kernel=LoadLibraryA("kernel32.dll");
    FN(CreateTimerQueue,queue_fn);FN(CreateTimerQueueTimer,create_fn);
    FN(ChangeTimerQueueTimer,change_fn);FN(DeleteTimerQueueTimer,delete_fn);
    FN(DeleteTimerQueueEx,delqueue_fn);FN(CreateEventA,event_fn);
    FN(SetEvent,handle_fn);FN(CloseHandle,handle_fn);FN(WaitForSingleObject,wait_fn);
    FN(GetCurrentThreadId,tid_fn);FN(Sleep,sleep_fn);FN(CreateThread,thread_fn);
    CHECK(CreateTimerQueue&&CreateTimerQueueTimer&&ChangeTimerQueueTimer&&DeleteTimerQueueTimer&&DeleteTimerQueueEx&&CreateEventA&&SetEvent&&CloseHandle&&WaitForSingleObject&&GetCurrentThreadId&&Sleep&&CreateThread,"exports");
    if(failures)ExitProcess(1);
    wait_one=WaitForSingleObject;set_event=SetEvent;thread_id=GetCurrentThreadId;delete_timer=DeleteTimerQueueTimer;
    queue=CreateTimerQueue();CHECK(queue!=0,"create-queue");if(!queue)ExitProcess(1);
    State state;init(&state,CreateEventA,1,0);
    struct{void *timer;volatile UINT guard;} output={0,0xabcdef01};
    CHECK(CreateTimerQueueTimer(&output.timer,queue,callback,&state,30,0,0)&&output.timer&&output.guard==0xabcdef01,"create-output-pointer-width");
    CHECK(WaitForSingleObject(state.entered,5000)==0&&state.count==1&&!state.bad&&state.tid!=GetCurrentThreadId(),"one-shot-context-and-thread");
    CHECK(DeleteTimerQueueTimer(queue,output.timer,INVALID),"delete-one-shot-synchronous");
    Sleep(50);CHECK(state.count==1,"one-shot-no-repeat");close_state(&state,CloseHandle);

    init(&state,CreateEventA,1,0);
    CHECK(CreateTimerQueueTimer(&state.timer,queue,callback,&state,5000,1000,0)&&WaitForSingleObject(state.entered,20)==258,"future-timer-not-early");
    CHECK(ChangeTimerQueueTimer(queue,state.timer,20,0)&&WaitForSingleObject(state.entered,5000)==0,"change-deadline-and-period");
    CHECK(DeleteTimerQueueTimer(queue,state.timer,INVALID)&&state.count==1&&!state.bad,"changed-timer-one-shot");close_state(&state,CloseHandle);

    init(&state,CreateEventA,3,0);
    CHECK(CreateTimerQueueTimer(&state.timer,queue,callback,&state,20,20,0)&&WaitForSingleObject(state.entered,5000)==0,"periodic-callbacks");
    CHECK(DeleteTimerQueueTimer(queue,state.timer,INVALID),"periodic-delete-waits");
    UINT stopped=state.count;Sleep(80);CHECK(stopped>=3&&state.count==stopped&&!state.bad,"periodic-stops-after-delete");close_state(&state,CloseHandle);

    init(&state,CreateEventA,2,1);
    CHECK(CreateTimerQueueTimer(&state.timer,queue,callback,&state,20,30,0)&&WaitForSingleObject(state.entered,5000)==0,"periodic-callbacks-overlap");
    SetEvent(state.release);
    CHECK(DeleteTimerQueueTimer(queue,state.timer,INVALID)&&state.count>=2&&!state.bad,"overlap-drained-on-delete");close_state(&state,CloseHandle);

    init(&state,CreateEventA,1,1);state.returned=CreateEventA(0,1,0,0);
    CHECK(CreateTimerQueueTimer(&state.timer,queue,callback,&state,10,0,0)&&WaitForSingleObject(state.entered,5000)==0,"blocking-callback-started");
    SetLastError(0);int deleted=DeleteTimerQueueTimer(queue,state.timer,0);UINT error=GetLastError();
    CHECK(!deleted&&error==997&&WaitForSingleObject(state.returned,0)==258,"async-delete-reports-pending");
    SetEvent(state.release);CHECK(WaitForSingleObject(state.returned,5000)==0&&!state.bad,"pending-callback-completes");
    /* returned is the final guest operation; queue deletion below joins the
     * worker's bookkeeping before the state storage is reused. */
    CHECK(DeleteTimerQueueEx(queue,INVALID),"queue-drains-async-deletion");close_state(&state,CloseHandle);
    queue=CreateTimerQueue();CHECK(queue!=0,"queue-recreated");

    init(&state,CreateEventA,1,1);state.completion=CreateEventA(0,1,0,0);
    CHECK(CreateTimerQueueTimer(&state.timer,queue,callback,&state,10,0,0)&&WaitForSingleObject(state.entered,5000)==0,"event-completion-callback-started");
    SetLastError(0);deleted=DeleteTimerQueueTimer(queue,state.timer,state.completion);error=GetLastError();
    CHECK((deleted||error==997)&&WaitForSingleObject(state.completion,20)==258,"completion-event-awaits-callback");
    SetEvent(state.release);CHECK(WaitForSingleObject(state.completion,5000)==0&&!state.bad,"completion-event-signaled");close_state(&state,CloseHandle);

    init(&state,CreateEventA,1,1);state.completion=CreateEventA(0,1,0,0);state.returned=CreateEventA(0,1,0,0);
    CHECK(CreateTimerQueueTimer(&state.timer,queue,callback,&state,10,0,0)&&WaitForSingleObject(state.entered,5000)==0,"sync-delete-callback-started");
    /* The callback must not signal returned: this test uses it for the waiter. */
    void *waiter_done=state.returned;state.returned=0;
    State waiter=state;waiter.returned=waiter_done;
    void *worker=CreateThread(0,0,delete_worker,&waiter,0,0);
    CHECK(worker&&WaitForSingleObject(state.completion,5000)==0&&WaitForSingleObject(waiter_done,20)==258,"sync-delete-blocks-during-callback");
    SetEvent(state.release);
    CHECK(WaitForSingleObject(waiter_done,5000)==0&&WaitForSingleObject(worker,5000)==0&&waiter.delete_result&&!state.bad,"sync-delete-returns-after-callback");
    CloseHandle(worker);CloseHandle(waiter_done);close_state(&state,CloseHandle);

    init(&state,CreateEventA,1,0);state.self_delete=1;state.returned=CreateEventA(0,1,0,0);
    CHECK(CreateTimerQueueTimer(&state.timer,queue,callback,&state,100,50,0)&&WaitForSingleObject(state.returned,5000)==0,"callback-can-delete-itself");
    CHECK(!state.delete_result&&state.delete_error==997,"self-delete-is-pending");
    CHECK(DeleteTimerQueueEx(queue,INVALID)&&state.count==1&&!state.bad,"self-delete-queue-drained");close_state(&state,CloseHandle);

    queue=CreateTimerQueue();init(&state,CreateEventA,1,0);
    CHECK(CreateTimerQueueTimer(&state.timer,queue,callback,&state,20,0,0x20)&&WaitForSingleObject(state.entered,5000)==0&&!state.bad,"timer-thread-callback");
    CHECK(DeleteTimerQueueTimer(queue,state.timer,INVALID),"timer-thread-callback-deleted");close_state(&state,CloseHandle);
    init(&state,CreateEventA,1,0);
    CHECK(CreateTimerQueueTimer(&state.timer,queue,callback,&state,5000,50,0)&&DeleteTimerQueueEx(queue,INVALID),"queue-cancels-future-timers");
    CHECK(WaitForSingleObject(state.entered,50)==258&&state.count==0,"canceled-queue-no-callback");close_state(&state,CloseHandle);

    queue=CreateTimerQueue();init(&state,CreateEventA,1,1);state.completion=CreateEventA(0,1,0,0);
    CHECK(CreateTimerQueueTimer(&state.timer,queue,callback,&state,10,0,0)&&WaitForSingleObject(state.entered,5000)==0,"queue-completion-callback-started");
    SetLastError(0);deleted=DeleteTimerQueueEx(queue,state.completion);error=GetLastError();
    CHECK((deleted||error==997)&&WaitForSingleObject(state.completion,20)==258,"queue-completion-event-waits");
    SetEvent(state.release);CHECK(WaitForSingleObject(state.completion,5000)==0&&!state.bad,"queue-completion-event-signaled");close_state(&state,CloseHandle);

    init(&state,CreateEventA,1,0);
    CHECK(CreateTimerQueueTimer(&state.timer,0,callback,&state,20,0,0)&&WaitForSingleObject(state.entered,5000)==0,"default-queue-callback");
    CHECK(DeleteTimerQueueTimer(0,state.timer,INVALID)&&!state.bad,"default-queue-deletion");close_state(&state,CloseHandle);

    /* Deliberately leave a disabled timer and its workers for ExitProcess:
     * repeated QEMU launches verify the process cleanup path. */
    init(&state,CreateEventA,1,0);
    CHECK(CreateTimerQueueTimer(&state.timer,0,callback,&state,~0U,0,0),"process-owned-disabled-timer");
    if(!failures)SAY("TQUEUE DONE\n");
    ExitProcess(failures?1:0);
}
