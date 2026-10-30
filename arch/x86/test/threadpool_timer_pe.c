/* Native Windows reference and Osito-K PE32/PE64 timer lifetime contract. */
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef __UINTPTR_TYPE__ UPTR;
typedef long long TIME;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
static unsigned checks,failures;
static HANDLE (CALL *create_timer)(void (CALL *)(void *,void *,HANDLE),void *,void *);
static void (CALL *set_timer)(HANDLE,const TIME *,DWORD,DWORD);
static void (CALL *wait_timer)(HANDLE,BOOL), (CALL *close_timer)(HANDLE);
static BOOL (CALL *is_set)(HANDLE);
static HANDLE (CALL *create_event)(void *,BOOL,BOOL,const char *);
static BOOL (CALL *set_event)(HANDLE), (CALL *reset_event)(HANDLE), (CALL *close_handle)(HANDLE);
static DWORD (CALL *wait_one)(HANDLE,DWORD), (CALL *thread_id)(void);
static void (CALL *sleep_ms)(DWORD), (CALL *system_time)(TIME *);
static HANDLE (CALL *create_thread)(void *,UPTR,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
static DWORD main_tid;
typedef struct {
    volatile DWORD calls,bad;
    DWORD threshold,mode;
    HANDLE timer,entered,release,returned,wait_started,wait_done;
    BOOL cancel;
} STATE;
static STATE states[12];
typedef struct {
    DWORD version;
    void *fields[6];
    DWORD flags,priority,size;
} ENVIRONMENT;
static ENVIRONMENT environment;

static void tp_report(const char *s)
{
    DWORD n=0,written;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11),s,n,&written,0);
}
static void tp_check(BOOL ok,const char *name)
{
    char record[256];
    const char *prefix=ok?"TPOOL PASS ":"TPOOL FAIL ";
    unsigned n=0;
    checks++;if (!ok) failures++;
    while (*prefix) record[n++]=*prefix++;
    while (*name && n<sizeof(record)-2) record[n++]=*name++;
    if (*name) ExitProcess(2);
    record[n++]='\n';record[n]=0;
    tp_report(record);
    if (!ok) ExitProcess(1);
}
static void CALL tp_callback(void *instance,void *context,HANDLE timer)
{
    STATE *s=context;
    if (!instance || timer!=s->timer || thread_id()==main_tid)
        __atomic_add_fetch(&s->bad,1,__ATOMIC_SEQ_CST);
    DWORD calls=__atomic_add_fetch(&s->calls,1,__ATOMIC_SEQ_CST);
    if (s->mode==1 && calls==1) {
        TIME relative=-300000;
        set_timer(timer,&relative,0,0);
    }
    if (calls>=s->threshold) set_event(s->entered);
    if (s->release && wait_one(s->release,10000)!=0)
        __atomic_add_fetch(&s->bad,1,__ATOMIC_SEQ_CST);
    if (s->mode==2) close_timer(timer);
    if (s->returned) set_event(s->returned);
}
static DWORD CALL tp_waiter(void *context)
{
    STATE *s=context;
    set_event(s->wait_started);
    wait_timer(s->timer,s->cancel);
    set_event(s->wait_done);
    return 0;
}
static void tp_init(STATE *s,DWORD threshold,BOOL blocking,void *env)
{
    volatile unsigned char *p=(volatile unsigned char *)s;
    for (UPTR i=0;i<sizeof(*s);i++) p[i]=0;
    s->threshold=threshold;
    s->entered=create_event(0,1,0,0);
    s->returned=create_event(0,1,0,0);
    if (blocking) s->release=create_event(0,1,0,0);
    s->timer=create_timer(tp_callback,s,env);
    tp_check(s->entered && s->returned && s->timer && (!blocking || s->release),"create timer and events");
}
static void tp_close_state(STATE *s)
{
    set_timer(s->timer,0,0,0);
    wait_timer(s->timer,1);
    tp_check(!s->bad,"callback instance context timer and worker ABI");
    close_timer(s->timer);
    close_handle(s->entered);close_handle(s->returned);
    if (s->release) close_handle(s->release);
    if (s->wait_started) close_handle(s->wait_started);
    if (s->wait_done) close_handle(s->wait_done);
}
void pool_timer_entry(void)
{
    HANDLE kernel=LoadLibraryA("kernel32.dll");
#define LOAD(variable,name) do {variable=GetProcAddress(kernel,name);tp_check(variable!=0,"export " name);}while(0)
    LOAD(create_timer,"CreateThreadpoolTimer");LOAD(set_timer,"SetThreadpoolTimer");
    LOAD(wait_timer,"WaitForThreadpoolTimerCallbacks");LOAD(close_timer,"CloseThreadpoolTimer");
    LOAD(is_set,"IsThreadpoolTimerSet");LOAD(create_event,"CreateEventA");
    LOAD(set_event,"SetEvent");LOAD(reset_event,"ResetEvent");LOAD(close_handle,"CloseHandle");
    LOAD(wait_one,"WaitForSingleObject");LOAD(thread_id,"GetCurrentThreadId");
    LOAD(sleep_ms,"Sleep");LOAD(system_time,"GetSystemTimeAsFileTime");LOAD(create_thread,"CreateThread");
#undef LOAD
    main_tid=thread_id();
    TIME relative=-200000,immediate=0;
    STATE *s=&states[0];tp_init(s,1,0,0);
    sleep_ms(30);tp_check(!s->calls && !is_set(s->timer),"new timer is disarmed");
    wait_timer(s->timer,1);tp_check(!s->calls,"wait on idle timer returns");
    set_timer(s->timer,&relative,0,0);
    tp_check(is_set(s->timer),"setting relative deadline marks set");
    tp_check(wait_one(s->entered,5000)==0,"relative deadline fires");
    wait_timer(s->timer,0);tp_check(s->calls==1 && is_set(s->timer),"one-shot expiration retains set state");
    set_timer(s->timer,0,0,0);tp_check(!is_set(s->timer),"null deadline disarms");
    reset_event(s->entered);s->threshold=2;
    set_timer(s->timer,&relative,0,0);
    tp_check(wait_one(s->entered,5000)==0,"expired one-shot can be rearmed");
    wait_timer(s->timer,1);tp_check(s->calls==2,"rearm executes exactly once");
    set_timer(s->timer,0,0,0);reset_event(s->entered);s->threshold=5;
    set_timer(s->timer,&relative,15,25);
    tp_check(wait_one(s->entered,5000)==0,"periodic timer fires repeatedly");
    set_timer(s->timer,0,0,0);wait_timer(s->timer,1);
    DWORD count=s->calls;sleep_ms(50);tp_check(s->calls==count,"disarm and drain stops callbacks");
    reset_event(s->entered);s->threshold=count+1;
    TIME absolute;system_time(&absolute);absolute+=300000;
    set_timer(s->timer,&absolute,0,0);
    tp_check(wait_one(s->entered,5000)==0,"absolute UTC deadline fires");
    wait_timer(s->timer,0);
    reset_event(s->entered);s->threshold=s->calls+1;
    set_timer(s->timer,&immediate,0,0);
    tp_check(wait_one(s->entered,5000)==0,"zero deadline fires immediately");
    wait_timer(s->timer,0);count=s->calls;
    TIME distant=(TIME)(1ULL<<63);set_timer(s->timer,&distant,0,0);
    sleep_ms(30);tp_check(s->calls==count && is_set(s->timer),"INT64_MIN relative deadline stays in future");
    tp_close_state(s);

    environment.version=3;environment.priority=1;environment.size=sizeof(environment);
    tp_init(&states[1],2,0,&environment);s=&states[1];s->mode=1;
    set_timer(s->timer,&relative,0,0);
    tp_check(wait_one(s->entered,5000)==0,"callback rearms itself in default environment");
    wait_timer(s->timer,1);tp_check(s->calls==2,"self rearm produces second callback");
    tp_close_state(s);

    for (DWORD cancel=0;cancel<2;cancel++) {
        s=&states[2+cancel];tp_init(s,1,1,0);s->cancel=(BOOL)cancel;
        s->wait_started=create_event(0,1,0,0);s->wait_done=create_event(0,1,0,0);
        set_timer(s->timer,&immediate,1,0);
        tp_check(wait_one(s->entered,5000)==0,"blocking periodic callback entered");
        sleep_ms(100);set_timer(s->timer,0,0,0);
        HANDLE thread=create_thread(0,0,tp_waiter,s,0,0);
        tp_check(thread && wait_one(s->wait_started,5000)==0,"callback wait started on another thread");
        tp_check(wait_one(s->wait_done,40)==258,"wait keeps running callbacks alive");
        count=s->calls;set_event(s->release);
        tp_check(wait_one(s->wait_done,5000)==0 && wait_one(thread,5000)==0,"callback wait finishes after release");
        tp_check(cancel ? s->calls==count : s->calls>=count,"pending callback cancellation or drain");
        close_handle(thread);tp_close_state(s);
    }

    s=&states[4];tp_init(s,1,1,0);set_timer(s->timer,&immediate,0,0);
    tp_check(wait_one(s->entered,5000)==0,"async close callback entered");
    close_timer(s->timer);
    tp_check(wait_one(s->returned,0)==258,"close returns while callback still running");
    set_event(s->release);tp_check(wait_one(s->returned,5000)==0,"callback can return after asynchronous close");
    sleep_ms(20);tp_check(!s->bad,"async close preserves callback arguments");
    close_handle(s->entered);close_handle(s->returned);close_handle(s->release);

    s=&states[5];tp_init(s,1,0,0);s->mode=2;set_timer(s->timer,&relative,0,0);
    tp_check(wait_one(s->returned,5000)==0,"callback closes its own timer");
    sleep_ms(20);tp_check(!s->bad && s->calls==1,"self-close executes once");
    close_handle(s->entered);close_handle(s->returned);

    for (unsigned i=0;i<48;i++) {
        HANDLE timer=create_timer(tp_callback,&states[6],0);
        tp_check(timer!=0,"timer slot reuse");close_timer(timer);
    }
    /* Process teardown owns an armed timer and its worker pool. The next PE
     * launch must not inherit callbacks or stale process-owned tokens. */
    tp_init(&states[7],1,0,0);set_timer(states[7].timer,&relative,15,0);
    tp_check(wait_one(states[7].entered,5000)==0,"live timer before process exit");
    tp_report(failures?"TPOOL DONE FAIL\n":"TPOOL DONE PASS\n");
    ExitProcess(failures?1:0);
}
