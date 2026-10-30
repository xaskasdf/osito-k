/* The identical no-CRT PE fixture runs on Windows and OsitoK. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef int LONG;
typedef __SIZE_TYPE__ SIZE;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define SELF ((HANDLE)(UPTR)-1)
#define THREAD ((HANDLE)(UPTR)-2)
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
typedef struct {DWORD cb;WORD *reserved,*desktop,*title;DWORD x,y,w,h,xc,yc,fill,flags;
    WORD show,bytes;BYTE *data;HANDLE input,output,error;} STARTUP;
typedef struct {HANDLE process,thread;DWORD pid,tid;} PROCESS;
static LONG (CALL *nt_process)(HANDLE,DWORD,HANDLE *);
static LONG (CALL *nt_process_ex)(HANDLE,DWORD,DWORD,HANDLE *);
static LONG (CALL *nt_thread)(HANDLE,DWORD,BYTE,HANDLE *);
static LONG (CALL *nt_thread_ex)(HANDLE,DWORD,BYTE,DWORD,HANDLE *);
static LONG (CALL *zw_process)(HANDLE,DWORD,HANDLE *);
static LONG (CALL *zw_process_ex)(HANDLE,DWORD,DWORD,HANDLE *);
static LONG (CALL *zw_thread)(HANDLE,DWORD,BYTE,HANDLE *);
static LONG (CALL *zw_thread_ex)(HANDLE,DWORD,BYTE,DWORD,HANDLE *);
static LONG (CALL *query_object)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *open_token)(HANDLE,DWORD,HANDLE *);
static BOOL (CALL *open_thread_token)(HANDLE,DWORD,BOOL,HANDLE *);
static BOOL (CALL *get_info)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *dup_token)(HANDLE,DWORD,void *,DWORD,DWORD,HANDLE *);
static BOOL (CALL *set_thread_token)(HANDLE *,HANDLE),(CALL *revert_self)(void);
static BOOL (CALL *close_handle)(HANDLE),(CALL *get_flags)(HANDLE,DWORD *);
static BOOL (CALL *set_flags)(HANDLE,DWORD,DWORD);
static HANDLE (CALL *open_process)(DWORD,BOOL,DWORD),(CALL *open_thread)(DWORD,BOOL,DWORD);
static DWORD (CALL *get_pid)(void),(CALL *get_tid)(void),(CALL *wait_object)(HANDLE,DWORD);
static void (CALL *set_error)(DWORD);
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static BOOL (CALL *terminate)(HANDLE,DWORD);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static DWORD failures;
static BOOL defer_report;
static char deferred[8192];
static DWORD deferred_used;
static void report(const char *s)
{
    DWORD n=0,w;while(s[n])n++;
    if(defer_report){
        if(n+deferred_used>=sizeof(deferred)){failures++;return;}
        memcpy(deferred+deferred_used,s,n);deferred_used+=n;
    }else WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);
}
static void flush_report(void)
{defer_report=0;deferred[deferred_used]=0;report(deferred);deferred_used=0;}
static void hex(DWORD v)
{char s[9];for(int i=7;i>=0;i--){s[i]="0123456789abcdef"[v&15];v>>=4;}s[8]=0;report(s);}
static void check(BOOL ok,const char *s)
{if(!ok){failures++;report("NATIVE-TOKEN FAIL: ");report(s);report("\n");}}
static void status_is(LONG actual,DWORD expected,const char *label)
{
    if((DWORD)actual!=expected){check(0,label);report(" expected=");hex(expected);
        report(" actual=");hex((DWORD)actual);report("\n");}
}
static void close(HANDLE h)
{DWORD f=0;if(get_flags(h,&f)&&(f&2))set_flags(h,2,0);check(close_handle(h),"close token handle");}
static void token_same(HANDLE a,HANDLE b)
{
    DWORD x[16]={0},y[16]={0},n=0;
    check(get_info(a,10,x,sizeof(x),&n)&&get_info(b,10,y,sizeof(y),&n),"query token statistics");
    check(x[0]==y[0]&&x[1]==y[1]&&x[2]==y[2]&&x[3]==y[3],"token object identity, not a fabricated token");
}
static LONG probe(const char *label,int thread,HANDLE object,DWORD access,DWORD attrs,BYTE as_self,HANDLE *result)
{
    struct {HANDLE h;DWORD guard;} out={(HANDLE)(UPTR)0x12345678,0xfeedbeef};
    set_error(0x13572468);
    LONG s=thread?nt_thread_ex(object,access,as_self,attrs,&out.h):nt_process_ex(object,access,attrs,&out.h);
    check(GetLastError()==0x13572468,"NT entry leaves last error unchanged");
    check(out.guard==0xfeedbeef,"output width");
    report("NATIVE-TOKEN ");report(label);report(" status=");hex((DWORD)s);
    if(!s){DWORD basic[14]={0},n=0,f=0;
        check(query_object(out.h,0,basic,sizeof(basic),&n)==0,"granted access query");
        check(get_flags(out.h,&f),"handle flags query");
        DWORD expected=access&~0xf2000000U;
        if(access&0x80000000U)expected|=0x2001a;
        if(access&0x40000000U)expected|=0x201e0;
        if(access&0x20000000U)expected|=0x20005;
        if(access&0x12000000U)expected|=0xf01ff;
        check(basic[1]==expected,"requested token rights, including generic mapping");
        check(f==((attrs&2)?1U:0U),"inherit attribute, no implicit protect-close");
        report(" access=");hex(basic[1]);report(" flags=");hex(f);
    }else{check((UPTR)out.h==(sizeof(HANDLE)==4?0U:0x12345678U),"failed output width and preservation");
        report(" output=");hex((DWORD)(UPTR)out.h);}
    report("\n");
    if(!s){if(result)*result=out.h;else close(out.h);}return s;
}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),a=LoadLibraryA("advapi32.dll"),n=LoadLibraryA("ntdll.dll");
#define LOAD(mod,var,name) var=GetProcAddress(mod,name);if(!var){report("NATIVE-TOKEN MISSING " name "\n");ExitProcess(2);}
    LOAD(n,nt_process,"NtOpenProcessToken");LOAD(n,nt_process_ex,"NtOpenProcessTokenEx");
    LOAD(n,nt_thread,"NtOpenThreadToken");LOAD(n,nt_thread_ex,"NtOpenThreadTokenEx");
    LOAD(n,zw_process,"ZwOpenProcessToken");LOAD(n,zw_process_ex,"ZwOpenProcessTokenEx");
    LOAD(n,zw_thread,"ZwOpenThreadToken");LOAD(n,zw_thread_ex,"ZwOpenThreadTokenEx");
    LOAD(n,query_object,"NtQueryObject");LOAD(a,open_token,"OpenProcessToken");
    LOAD(a,open_thread_token,"OpenThreadToken");
    LOAD(a,get_info,"GetTokenInformation");LOAD(a,dup_token,"DuplicateTokenEx");
    LOAD(a,set_thread_token,"SetThreadToken");LOAD(a,revert_self,"RevertToSelf");
    LOAD(k,close_handle,"CloseHandle");LOAD(k,get_flags,"GetHandleInformation");
    LOAD(k,set_flags,"SetHandleInformation");LOAD(k,open_process,"OpenProcess");
    LOAD(k,open_thread,"OpenThread");LOAD(k,get_pid,"GetCurrentProcessId");
    LOAD(k,get_tid,"GetCurrentThreadId");LOAD(k,set_error,"SetLastError");
    LOAD(k,create_process,"CreateProcessA");LOAD(k,terminate,"TerminateProcess");
    LOAD(k,wait_object,"WaitForSingleObject");LOAD(k,module_name,"GetModuleFileNameA");
#undef LOAD
    HANDLE primary=0,h=0;
    check(open_token(SELF,0x02000000,&primary),"Win32 primary source");
    check(probe("primary",0,SELF,8,0,0,&h)==0,"NT primary open");
    if(h){token_same(primary,h);close(h);}
    h=0;check(nt_process(SELF,8,&h)==0,"three argument process entry");if(h)close(h);
    h=0;check(zw_process(SELF,8,&h)==0,"process Zw alias");if(h)close(h);
    h=0;check(zw_process_ex(SELF,8,0,&h)==0,"process Ex Zw alias");if(h)close(h);
    DWORD masks[]={0,8,2,0x80000000,0x40000000,0x20000000,0x10000000,0x02000000,0x00100000,0x00000400,0x08000000,0x01000000};
    DWORD mask_status[]={0xc0000022,0,0,0,0,0,0,0,0xc0000022,0xc0000022,0xc0000022,0xc0000061};
    for(DWORD i=0;i<sizeof(masks)/sizeof(masks[0]);i++){
        report("NATIVE-TOKEN requested=");hex(masks[i]);report("\n");
        status_is(probe("mask",0,SELF,masks[i],0,0,0),mask_status[i],"token access mask validation");
    }
    DWORD attrs[]={1,2,0x40,0x80,0x100,0x200,0x400,0x800,0x1000,0x80000000};
    for(DWORD i=0;i<sizeof(attrs)/sizeof(attrs[0]);i++){
        report("NATIVE-TOKEN attributes=");hex(attrs[i]);report("\n");
        status_is(probe("attributes",0,SELF,8,attrs[i],0,0),attrs[i]==0x100?0xc000000d:0,"token attributes");
    }
    status_is(probe("invalid-process",0,0,8,0,0,0),0xc0000008,"invalid process");
    status_is(probe("thread-as-process",0,THREAD,8,0,0,0),0xc0000024,"thread pseudo type");
    status_is(probe("token-as-process",0,primary,8,0,0,0),0xc0000024,"token process type");
    status_is(probe("no-thread-token",1,THREAD,8,0,1,0),0xc000007c,"no impersonation token");
    status_is(probe("invalid-thread",1,0,8,0,1,0),0xc0000008,"invalid thread");
    status_is(probe("process-as-thread",1,SELF,8,0,1,0),0xc0000024,"process pseudo type");
    status_is(probe("token-as-thread",1,primary,8,0,1,0),0xc0000024,"token thread type");
    status_is(nt_process(SELF,8,0),0xc0000005,"null output");
    status_is(nt_process(SELF,8,(HANDLE *)(UPTR)1),0xc0000005,"inaccessible output");
    status_is(nt_thread(THREAD,8,1,0),0xc0000005,"thread output probed before lookup");
    if(sizeof(UPTR)==8){
        status_is(probe("zero-extended-self",0,(HANDLE)(UPTR)0xffffffffU,8,0,0,0),0xc0000008,"native pseudo handle width");
        status_is(probe("high-self",0,(HANDLE)(((UPTR)3<<31)+0x7fffffffU),8,0,0,0),0xc0000008,"native high handle bits");
    }
    DWORD process_access[]={0x400,0x1000,0x100000};
    for(DWORD i=0;i<3;i++){
        HANDLE p=open_process(process_access[i],0,get_pid());check(p!=0,"source process handle");
        report("NATIVE-TOKEN process-rights=");hex(process_access[i]);report("\n");
        status_is(probe("source-process",0,p,8,0,0,0),i==2?0xc0000022:0,"source process query access");if(p)close(p);
    }
    for(DWORD level=0;level<4;level++){
        HANDLE imp=0;check(dup_token(primary,0x02000000,0,level,2,&imp),"create impersonation token");
        /* Anonymous/identification impersonation can deny writes to stdout. */
        defer_report=1;
        check(imp&&set_thread_token(0,imp),"attach impersonation token");
        report("NATIVE-TOKEN level=");hex(level);report("\n");
        status_is(probe("thread-as-thread",1,THREAD,8,0,0,0),
            level==0?0xc00000a6:level==1?0xc00000a5:0,"thread effective impersonation level");
        h=0;LONG s=probe("thread-as-self",1,THREAD,8,2,1,&h);
        status_is(s,level==0?0xc00000a6:0,"OpenAsSelf and anonymous target");
        status_is(probe("process-under-impersonation",0,SELF,8,0,0,0),
            level<2?0xc00000a5:0,"process open effective impersonation level");
        if(s==0&&h){token_same(imp,h);close(h);}
        h=0;BOOL opened=open_thread_token(THREAD,8,1,&h);
        check(level==0?(!opened&&GetLastError()==1347):opened,"Win32 thread token and anonymous error");
        if(opened&&h){token_same(imp,h);close(h);}
        h=0;opened=open_token(SELF,8,&h);
        check(level<2?(!opened&&GetLastError()==1346):opened,"Win32 primary token and impersonation error");
        if(opened&&h){token_same(primary,h);close(h);}
        if(level==2){
            h=0;check(nt_thread(THREAD,8,1,&h)==0,"four argument thread entry");if(h)close(h);
            h=0;check(zw_thread(THREAD,8,1,&h)==0,"thread Zw alias");if(h)close(h);
            h=0;check(zw_thread_ex(THREAD,8,1,0,&h)==0,"thread Ex Zw alias");if(h)close(h);
            DWORD access[]={0x40,0x800,0x100000};
            for(DWORD j=0;j<3;j++){
                HANDLE t=open_thread(access[j],0,get_tid());check(t!=0,"source thread handle");
                report("NATIVE-TOKEN thread-rights=");hex(access[j]);report("\n");
                status_is(probe("source-thread",1,t,8,0,1,0),j==2?0xc0000022:0,"source thread query access");if(t)close(t);
            }
            h=0;check(probe("retained-thread",1,THREAD,8,0,1,&h)==0,"retain impersonation token");
            check(revert_self(),"revert before retained query");if(h){token_same(imp,h);close(h);}
        }
        check(revert_self(),"revert identity");flush_report();if(imp)close(imp);
    }
    for(DWORD i=0;i<3;i++){
        char path[320],command[]="nativetoken --unused-child";module_name(0,path,sizeof(path));
        STARTUP startup={0};startup.cb=sizeof(startup);PROCESS pi={0};
        check(create_process(path,command,0,0,0,4,0,0,&startup,&pi),"create suspended token target");
        if(!pi.process)break;
        HANDLE a=0,b=0;
        check(open_token(pi.process,8,&a),"Win32 selected child token");
        check(probe("child-token",0,pi.process,8,0,0,&b)==0,"NT selected child token");
        if(a&&b)token_same(a,b);
        check(terminate(pi.process,93)&&wait_object(pi.process,5000)==0,"exit selected child");
        status_is(probe("exited-child-token",0,pi.process,8,0,0,0),0,"retained exited child token");
        close(pi.thread);close(pi.process);
        if(a&&b)token_same(a,b);if(a)close(a);if(b)close(b);
    }
    if(primary)close(primary);
    report(failures?"NATIVE-TOKEN FAILED\n":"NATIVE-TOKEN PASS\n");
    ExitProcess(failures?1:0);
}
