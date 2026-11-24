/* Same executable on native Windows and Osito-K, no CRT. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef __SIZE_TYPE__ SIZE;
typedef __UINTPTR_TYPE__ UPTR;
typedef int BOOL;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
void *memcpy(void *out,const void *in,SIZE n)
{for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=((const BYTE *)in)[i];return out;}
void *memset(void *out,int c,SIZE n)
{for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=(BYTE)c;return out;}
typedef struct {HANDLE pid,tid;} CID;
typedef struct {DWORD length;HANDLE root;void *name;DWORD flags;void *sd,*qos;} OA;
typedef struct {WORD length,max;WORD *text;} STRING;
typedef struct {
    DWORD cb;WORD *reserved,*desktop,*title;
    DWORD x,y,w,h,xc,yc,fill,flags;WORD show,bytes;BYTE *data;
    HANDLE input,output,error;
} STARTUP;
typedef struct {HANDLE process,thread;DWORD pid,tid;} PROCESS;
static long (CALL *nt_open)(HANDLE *,DWORD,OA *,CID *);
static long (CALL *zw_open)(HANDLE *,DWORD,OA *,CID *);
static long (CALL *nt_query)(HANDLE,DWORD,void *,DWORD,DWORD *);
static HANDLE (CALL *open_thread)(DWORD,BOOL,DWORD);
static HANDLE (CALL *create_thread)(void *,SIZE,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
static DWORD (CALL *get_tid)(void), (CALL *get_pid)(void);
static DWORD (CALL *thread_id)(HANDLE), (CALL *resume_thread)(HANDLE);
static DWORD (CALL *wait_object)(HANDLE,DWORD);
static BOOL (CALL *exit_code)(HANDLE,DWORD *);
static BOOL (CALL *close_handle)(HANDLE), (CALL *handle_flags)(HANDLE,DWORD *);
static BOOL (CALL *duplicate)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static BOOL (CALL *terminate_process)(HANDLE,DWORD);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static void (CALL *sleep_ms)(DWORD);
static DWORD failures;
static void report(const char *s)
{DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void hex(DWORD v)
{char s[9];for(int i=7;i>=0;i--){s[i]="0123456789abcdef"[v&15];v>>=4;}s[8]=0;report(s);}
static void check(BOOL ok,const char *s)
{if(!ok){failures++;report("OPEN-THREAD FAIL: ");report(s);report("\n");}}
static DWORD CALL worker(void *p)
{
    volatile DWORD *seen=p;*seen=get_tid();return 73;
}
static DWORD granted(HANDLE h)
{
    DWORD basic[14]={0},n=0;
    check(nt_query(h,0,basic,sizeof(basic),&n)==0,"query granted access");
    return basic[1];
}
static long attempt(const char *label,DWORD access,OA *oa,CID *cid,HANDLE *result)
{
    struct {HANDLE h;DWORD guard;} out={(HANDLE)(UPTR)0x12345678,0xfeedbeef};
    long s=nt_open(&out.h,access,oa,cid);
    check(out.guard==0xfeedbeef,"NtOpenThread output width");
    report("OPEN-THREAD ");report(label);report(" status=");hex((DWORD)s);
    if(s==0){report(" access=");hex(granted(out.h));}
    else{report(" output=");hex((DWORD)(UPTR)out.h);}
    report("\n");
    if(s==0){if(result)*result=out.h;else close_handle(out.h);}
    return s;
}
static void retired(const char *label,OA *oa,CID *cid)
{
    /* Process signaling can precede the kernel's final thread reference drop. */
    for(DWORD i=0;i<100;i++){
        HANDLE h=0;long s=nt_open(&h,0x40,oa,cid);
        if((DWORD)s==0xc000000b){report("OPEN-THREAD retired ");report(label);report("\n");return;}
        if(s==0)close_handle(h);else break;
        sleep_ms(10);
    }
    check(0,label);
}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),n=LoadLibraryA("ntdll.dll");
#define LOAD(module,var,name) var=GetProcAddress(module,name);if(!var){report("OPEN-THREAD MISSING " name "\n");ExitProcess(2);}
    LOAD(n,nt_open,"NtOpenThread");LOAD(n,zw_open,"ZwOpenThread");LOAD(n,nt_query,"NtQueryObject");
    LOAD(k,open_thread,"OpenThread");LOAD(k,create_thread,"CreateThread");
    LOAD(k,get_tid,"GetCurrentThreadId");LOAD(k,get_pid,"GetCurrentProcessId");
    LOAD(k,thread_id,"GetThreadId");LOAD(k,resume_thread,"ResumeThread");
    LOAD(k,wait_object,"WaitForSingleObject");LOAD(k,exit_code,"GetExitCodeThread");
    LOAD(k,close_handle,"CloseHandle");LOAD(k,handle_flags,"GetHandleInformation");
    LOAD(k,duplicate,"DuplicateHandle");LOAD(k,create_process,"CreateProcessA");
    LOAD(k,terminate_process,"TerminateProcess");LOAD(k,module_name,"GetModuleFileNameA");
    LOAD(k,sleep_ms,"Sleep");
#undef LOAD
    OA oa={0};oa.length=sizeof(oa);CID self={(HANDLE)(UPTR)get_pid(),(HANDLE)(UPTR)get_tid()};
    check(attempt("self",0x100840,&oa,&self,0)==0,"open main by full CID");
    CID only={0,self.tid};
    check(attempt("tid-only",0x40,&oa,&only,0)==0,"open by TID only");
    HANDLE h=0;check(zw_open(&h,0x40,&oa,&only)==0,"Zw alias callable");if(h)close_handle(h);
    DWORD masks[]={0,0x80000000,0x40000000,0x20000000,0x10000000,0x02000000,0x08000000,0x01000000,0x8000};
    DWORD mask_status[]={0xc0000022,0,0,0,0,0,0xc0000022,0xc0000061,0};
    DWORD mask_granted[]={0,0x20848,0x21437,0x121800,0x1fffff,0x1fffff,0,0,0x8000};
    for(DWORD i=0;i<sizeof(masks)/sizeof(masks[0]);i++){
        report("OPEN-THREAD requested=");hex(masks[i]);report("\n");
        h=0;long s=attempt("mask",masks[i],&oa,&self,&h);
        check((DWORD)s==mask_status[i],"access-mask status");
        if(h){check(granted(h)==mask_granted[i],"generic/specific access mapping");close_handle(h);}
    }
    check((DWORD)attempt("null-oa",0x40,0,&self,0)==0xc0000005,"null OA rejected");
    check((DWORD)attempt("null-cid",0x40,&oa,0,0)==0xc0000030,"null CID rejected");
    CID bad={self.pid,0};check((DWORD)attempt("zero-tid",0x40,&oa,&bad,0)==0xc000000b,"zero TID rejected");
    bad.tid=(HANDLE)(UPTR)0x7ffffffc;check((DWORD)attempt("invalid-tid",0x40,&oa,&bad,0)==0xc000000b,"unknown TID rejected");
    bad.tid=self.tid;bad.pid=(HANDLE)(UPTR)0x7ffffffc;check((DWORD)attempt("mismatch-pid",0x40,&oa,&bad,0)==0xc000000b,"PID mismatch rejected");
    oa.length=0;attempt("bad-oa-length",0x40,&oa,&self,0);oa.length=sizeof(oa);
    WORD name[]={ 'x',0 };STRING str={2,4,name};oa.name=&str;
    check((DWORD)attempt("named",0x40,&oa,&self,0)==0xc0000030,"object name rejected");oa.name=0;
    oa.root=(HANDLE)(UPTR)-1;attempt("root",0x40,&oa,&self,0);oa.root=0;
    DWORD attrs[]={1,2,0x40,0x200,0x80000000};
    for(DWORD i=0;i<sizeof(attrs)/sizeof(attrs[0]);i++){
        oa.flags=attrs[i];report("OPEN-THREAD attr=");hex(attrs[i]);report("\n");
        attempt("attribute",0x40,&oa,&self,0);
    }
    oa.flags=0;
    check((DWORD)nt_open(0,0x40,&oa,&self)==0xc0000005,"null output rejected");
    DWORD misaligned=sizeof(UPTR)==8?0x80000002:0xc0000005;
    check((DWORD)attempt("bad-cid-pointer",0x40,&oa,(CID *)(UPTR)1,0)==misaligned,"bad CID pointer rejected");
    check((DWORD)attempt("bad-oa-pointer",0x40,(OA *)(UPTR)1,&self,0)==misaligned,"bad OA pointer rejected");
    check((DWORD)attempt("unmapped-cid",0x40,&oa,(CID *)(UPTR)4096,0)==0xc0000005,"unmapped CID rejected");
    check((DWORD)attempt("unmapped-oa",0x40,(OA *)(UPTR)4096,&self,0)==0xc0000005,"unmapped OA rejected");
    if(sizeof(UPTR)==8){
        CID high={0,(HANDLE)((UPTR)self.tid+((UPTR)1<<31)*2)};
        check((DWORD)attempt("high-tid",0x40,&oa,&high,0)==0xc000000b,"TID cannot be truncated");
    }
    h=open_thread(0x40,1,get_tid());DWORD flags=0;
    check(h&&handle_flags(h,&flags)&&(flags&1),"Win32 inherit flag");if(h)close_handle(h);
    oa.flags=2;h=0;attempt("inherited",0x40,&oa,&self,&h);
    flags=0;check(h&&handle_flags(h,&flags)&&(flags&1),"NT inherit flag");if(h)close_handle(h);oa.flags=0;
    for(DWORD i=0;i<4;i++){
        volatile DWORD seen=0;DWORD tid=0,code=0;
        HANDLE t=create_thread(0,0,worker,(void *)&seen,4,&tid);
        check(t&&tid&&tid!=get_tid()&&thread_id(t)==tid,"created thread identity");
        if(!t)break;
        CID cid={self.pid,(HANDLE)(UPTR)tid};HANDLE opened=0,copy=0;
        check(attempt("suspended-worker",0x100842,&oa,&cid,&opened)==0,"open suspended worker");
        if(!opened){resume_thread(t);wait_object(t,5000);close_handle(t);break;}
        check(thread_id(opened)==tid,"opened handle identity");
        HANDLE restricted=open_thread(0x100000,0,tid);code=0;
        check(restricted&&!thread_id(restricted)&&GetLastError()==5,"GetThreadId requires query access");
        check(restricted&&!exit_code(restricted,&code)&&GetLastError()==5,"exit query requires access");
        check(restricted&&resume_thread(restricted)==0xffffffff&&GetLastError()==5,"resume requires access");
        if(restricted)close_handle(restricted);
        check(duplicate((HANDLE)(UPTR)-1,opened,(HANDLE)(UPTR)-1,&copy,0,0,2),"duplicate opened handle");
        check(close_handle(t),"close original thread handle");
        check(resume_thread(opened)==1,"resume through opened handle");
        check(wait_object(opened,10000)==0&&exit_code(opened,&code)&&code==73&&seen==tid,"run same TEB thread ID");
        check(attempt("terminated-referenced",0x100840,&oa,&cid,0)==0,"reopen retained terminated thread");
        check(close_handle(opened),"close opened handle");
        code=0;check(copy&&wait_object(copy,0)==0&&exit_code(copy,&code)&&code==73,"duplicate retains exit state");
        if(copy)close_handle(copy);
        retired("closed worker ID",&oa,&cid);
    }
    char path[320];module_name(0,path,sizeof(path));STARTUP si={0};si.cb=sizeof(si);
    PROCESS a={0},b={0};HANDLE retained=0;char command[]="open-thread child";
    BOOL made_a=create_process(path,command,0,0,0,4,0,0,&si,&a);
    BOOL made_b=create_process(path,command,0,0,0,4,0,0,&si,&b);
    check(made_a&&made_b,"create two suspended processes");
    if(made_a&&made_b){
        check(a.tid!=b.tid&&a.tid!=get_tid()&&b.tid!=get_tid(),"global primary thread IDs");
        check(thread_id(a.thread)==a.tid&&thread_id(b.thread)==b.tid,"primary handle IDs");
        CID ca={(HANDLE)(UPTR)a.pid,(HANDLE)(UPTR)a.tid};
        check(attempt("child-primary",0x100840,&oa,&ca,&retained)==0,"open child primary before resume");
        ca.pid=0;check(attempt("child-tid-only",0x40,&oa,&ca,0)==0,"open child by TID only");
        ca.pid=(HANDLE)(UPTR)b.pid;check((DWORD)attempt("sibling-mismatch",0x40,&oa,&ca,0)==0xc000000b,"sibling cannot alias thread ID");
    }
    if(made_a){
        close_handle(a.thread);terminate_process(a.process,81);wait_object(a.process,5000);
        CID cid={(HANDLE)(UPTR)a.pid,(HANDLE)(UPTR)a.tid};DWORD code=0;
        if(retained){
            check(wait_object(retained,10000)==0&&exit_code(retained,&code)&&code==81,"retained primary reports exit");
            check(attempt("exited-primary-retained",0x40,&oa,&cid,0)==0,"open retained primary after process exits");
            close_handle(retained);
        }
        retired("process handle does not keep thread CID alive",&oa,&cid);
        close_handle(a.process);
    }
    if(made_b){
        close_handle(b.thread);terminate_process(b.process,0);wait_object(b.process,5000);
        CID cid={(HANDLE)(UPTR)b.pid,(HANDLE)(UPTR)b.tid};
        retired("no thread reference at process exit",&oa,&cid);
        close_handle(b.process);
    }
    report(failures?"OPEN-THREAD FAILED\n":"OPEN-THREAD PASS\n");ExitProcess(failures?1:0);
}
