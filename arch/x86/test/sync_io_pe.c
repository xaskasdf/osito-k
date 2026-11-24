/* Native Windows / Osito-K contract probe, PE32 and PE32+, no CRT. */
typedef unsigned int DWORD;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef __SIZE_TYPE__ SIZE;
typedef __UINTPTR_TYPE__ UPTR;
typedef int BOOL;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define SELF ((HANDLE)(UPTR)-1)
#define THREAD_SELF ((HANDLE)(UPTR)-2)
#define BAD SELF
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
void *memset(void *p, int c, SIZE n)
{ for (SIZE i=0;i<n;i++) ((BYTE *)p)[i]=(BYTE)c; return p; }
void *memcpy(void *p, const void *s, SIZE n)
{ for (SIZE i=0;i<n;i++) ((BYTE *)p)[i]=((const BYTE *)s)[i]; return p; }
typedef struct { UPTR internal, bytes; DWORD lo, hi; HANDLE event; } OVERLAPPED;
static BOOL (CALL *cancel_sync)(HANDLE);
static BOOL (CALL *read_file)(HANDLE,void *,DWORD,DWORD *,void *);
static BOOL (CALL *create_pipe)(HANDLE *,HANDLE *,void *,DWORD);
static HANDLE (CALL *create_thread)(void *,SIZE,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
static HANDLE (CALL *open_thread)(DWORD,BOOL,DWORD);
static HANDLE (CALL *create_event)(void *,BOOL,BOOL,const char *);
static BOOL (CALL *set_event)(HANDLE), (CALL *close_handle)(HANDLE);
static DWORD (CALL *wait_object)(HANDLE,DWORD), (CALL *get_tid)(void);
static void (CALL *sleep_ms)(DWORD);
static BOOL (CALL *duplicate)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static HANDLE (CALL *create_file)(const char *,DWORD,DWORD,void *,DWORD,DWORD,HANDLE);
static HANDLE (CALL *named_pipe)(const WORD *,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,void *);
static BOOL (CALL *connect_pipe)(HANDLE,void *);
static BOOL (CALL *overlapped_result)(HANDLE,void *,DWORD *,BOOL);
static BOOL (CALL *lock_file)(HANDLE,DWORD,DWORD,DWORD,DWORD,void *);
static BOOL (CALL *unlock_file)(HANDLE,DWORD,DWORD,DWORD,void *);
static BOOL (CALL *delete_file)(const char *);
static DWORD checks, failures;
static BYTE large_buffer[65536];
static void report(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void hex(DWORD v)
{ char s[9];for(int i=7;i>=0;i--){s[i]="0123456789abcdef"[v&15];v>>=4;}s[8]=0;report(s); }
static void check(BOOL ok,const char *name)
{ checks++;if(!ok){failures++;report("SYNCIO FAIL ");report(name);report(" error=");hex(GetLastError());report("\n");} }
static void need(BOOL ok,const char *name)
{ check(ok,name);if(!ok)ExitProcess(2); }
static void no_request(HANDLE h,DWORD error,const char *name)
{ SetLastError(0x12345678);BOOL r=cancel_sync(h);DWORD e=GetLastError();check(!r&&e==error,name); }
typedef struct {
    HANDLE file, ready, gate, done;
    DWORD mode, tid;
    volatile DWORD bytes, error, result, next_bytes, next_result;
    BYTE byte;
    OVERLAPPED ov;
} JOB;
static DWORD CALL worker(void *p)
{
    JOB *j=p;
    j->tid=get_tid();j->bytes=0xfeedbeef;
    set_event(j->ready);SetLastError(0x12345678);
    if(j->mode==1)j->result=WriteFile(j->file,large_buffer,sizeof(large_buffer),(DWORD *)&j->bytes,0);
    else if(j->mode==2)j->result=lock_file(j->file,2,0,1,0,&j->ov);
    else j->result=read_file(j->file,&j->byte,1,(DWORD *)&j->bytes,0);
    j->error=GetLastError();set_event(j->done);
    wait_object(j->gate,10000);
    if(j->mode==0)j->next_result=read_file(j->file,&j->byte,1,(DWORD *)&j->next_bytes,0);
    return 0;
}
static HANDLE start(JOB *j,HANDLE file,DWORD mode)
{
    memset(j,0,sizeof(*j));j->file=file;j->mode=mode;
    j->ready=create_event(0,1,0,0);j->done=create_event(0,1,0,0);j->gate=create_event(0,1,0,0);
    need(j->ready&&j->done&&j->gate,"create worker events");
    HANDLE t=create_thread(0,0,worker,j,0,0);need(t!=0,"create worker");
    need(wait_object(j->ready,5000)==0,"worker reached I/O");return t;
}
static void cancel_pending(HANDLE t,JOB *j)
{
    BOOL found=0;DWORD last=0;
    for(DWORD i=0;i<2000;i++){
        SetLastError(0x12345678);found=cancel_sync(t);last=GetLastError();
        if(found||last!=1168||wait_object(j->done,0)==0)break;
        sleep_ms(1);
    }
    need(found,"found pending synchronous operation");
    check(last==0x12345678,"successful cancel preserves last error");
    need(wait_object(j->done,5000)==0,"cancelled operation completed");
    check(!j->result&&j->error==995,"operation aborted status");
    if(j->mode!=2)check(j->bytes==0,"cancelled transfer reports zero bytes");
    no_request(t,1168,"completed request cannot be cancelled again");
}
static void finish(HANDLE t,JOB *j)
{
    set_event(j->gate);need(wait_object(t,5000)==0,"worker finished");
    no_request(t,1168,"terminated thread has no request");
    close_handle(t);close_handle(j->ready);close_handle(j->done);close_handle(j->gate);
}
static void test_readers(DWORD round)
{
    HANDLE r=0,w=0;need(create_pipe(&r,&w,0,4096),"create shared reader pipe");
    JOB a,b;HANDLE ta=start(&a,r,0),tb=start(&b,r,3);
    HANDLE limited=open_thread(0x40,0,a.tid);
    need(limited!=0,"open query-only thread");
    no_request(limited,5,"cancel requires THREAD_TERMINATE");close_handle(limited);
    HANDLE target=0;
    if(round&1){
        target=open_thread(1,0,a.tid);need(target!=0,"open terminate-only thread");
    }else{
        need(duplicate(SELF,ta,SELF,&target,0,0,2),"duplicate target thread handle");
    }
    cancel_pending(target,&a);
    check(wait_object(b.done,0)==258,"other reader remains pending");
    DWORD n=0;BYTE value=0x5a;
    need(WriteFile(w,&value,1,&n,0)&&n==1,"write to surviving reader");
    need(wait_object(b.done,5000)==0,"other reader completes");
    check(b.result&&b.bytes==1&&b.byte==value,"other reader receives byte");
    no_request(target,1168,"cancel does not arm next read");
    value=0xa5;need(WriteFile(w,&value,1,&n,0)&&n==1,"prepare next read");
    finish(ta,&a);check(a.next_result&&a.next_bytes==1&&a.byte==value,"same thread next I/O succeeds");
    finish(tb,&b);close_handle(target);close_handle(r);close_handle(w);
}
static void test_writer(void)
{
    HANDLE r=0,w=0;need(create_pipe(&r,&w,0,4096),"create writer pipe");
    JOB j;HANDLE t=start(&j,w,1);cancel_pending(t,&j);finish(t,&j);
    close_handle(r);close_handle(w);
}
static void test_lock(void)
{
    const char *name="sync-io-lock.tmp";
    HANDLE a=create_file(name,0xc0000000,3,0,2,0x80,0);
    HANDLE b=create_file(name,0xc0000000,3,0,3,0x80,0);
    need(a!=BAD&&b!=BAD,"open lock fixture twice");
    OVERLAPPED ov={0};need(lock_file(a,3,0,1,0,&ov),"hold conflicting lock");
    JOB j;HANDLE t=start(&j,b,2);cancel_pending(t,&j);finish(t,&j);
    need(unlock_file(a,0,1,0,&ov),"release owner lock");
    check(lock_file(b,3,0,1,0,&ov),"cancel leaves no residual waiter or lock");
    unlock_file(b,0,1,0,&ov);close_handle(a);close_handle(b);delete_file(name);
}
static void test_async(void)
{
    WORD name[80];const char *ascii="\\\\.\\pipe\\osito-sync-io-probe";
    DWORD i=0;do{name[i]=(WORD)(BYTE)ascii[i];}while(ascii[i++]);
    HANDLE server=named_pipe(name,3|0x40000000,0,1,4096,4096,0,0);
    need(server!=BAD,"create overlapped named pipe");
    HANDLE client=create_file(ascii,0xc0000000,0,0,3,0,0);
    need(client!=BAD,"open named pipe client");
    BOOL connected=connect_pipe(server,0);DWORD error=GetLastError();
    check(connected||error==535,"named pipe connected");
    OVERLAPPED ov={0};ov.event=create_event(0,1,0,0);
    BYTE value=0;DWORD count=99;
    check(!read_file(server,&value,1,&count,&ov)&&GetLastError()==997,"asynchronous read pending");
    no_request(THREAD_SELF,1168,"synchronous cancel excludes overlapped request");
    check(wait_object(ov.event,0)==258,"overlapped read remains pending");
    BYTE sent=0x37;need(WriteFile(client,&sent,1,&count,0),"complete asynchronous read normally");
    need(overlapped_result(server,&ov,&count,1),"retrieve asynchronous completion");
    check(count==1&&value==sent,"overlapped data preserved");
    close_handle(ov.event);close_handle(client);close_handle(server);
}
void sync_io_entry(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll");
#define LOAD(v,n) v=GetProcAddress(k,n);if(!v){report("SYNCIO MISSING " n "\n");ExitProcess(2);}
    LOAD(cancel_sync,"CancelSynchronousIo");LOAD(read_file,"ReadFile");LOAD(create_pipe,"CreatePipe");
    LOAD(create_thread,"CreateThread");LOAD(open_thread,"OpenThread");LOAD(create_event,"CreateEventA");
    LOAD(set_event,"SetEvent");LOAD(close_handle,"CloseHandle");LOAD(wait_object,"WaitForSingleObject");
    LOAD(get_tid,"GetCurrentThreadId");LOAD(sleep_ms,"Sleep");LOAD(duplicate,"DuplicateHandle");
    LOAD(create_file,"CreateFileA");LOAD(named_pipe,"CreateNamedPipeW");LOAD(connect_pipe,"ConnectNamedPipe");
    LOAD(overlapped_result,"GetOverlappedResult");LOAD(lock_file,"LockFileEx");
    LOAD(unlock_file,"UnlockFileEx");LOAD(delete_file,"DeleteFileA");
#undef LOAD
    no_request(0,6,"null handle");no_request(BAD,6,"process pseudohandle is not a thread");
    no_request(THREAD_SELF,1168,"idle current thread");no_request((HANDLE)(UPTR)0x7ffffffc,6,"invalid handle");
    HANDLE event=create_event(0,1,0,0);no_request(event,6,"wrong object type");close_handle(event);
    HANDLE self=open_thread(1,0,get_tid());need(self!=0,"open idle main thread");
    no_request(self,1168,"idle real thread handle");close_handle(self);
    for(DWORD i=0;i<8;i++)test_readers(i);
    test_writer();test_lock();test_async();
    report("SYNCIO DONE checks=");hex(checks);report(" failures=");hex(failures);report("\n");
    ExitProcess(failures?1:0);
}
