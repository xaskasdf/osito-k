/* Native thread-token assignment; identical binaries run on Windows/OsitoK. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef int LONG;
typedef __UINTPTR_TYPE__ UPTR;
typedef __SIZE_TYPE__ SIZE;
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
static LONG (CALL *nt_set)(HANDLE,DWORD,void *,DWORD),(CALL *zw_set)(HANDLE,DWORD,void *,DWORD);
static BOOL (CALL *open_token)(HANDLE,DWORD,HANDLE *),(CALL *open_thread_token)(HANDLE,DWORD,BOOL,HANDLE *);
static BOOL (CALL *dup_token)(HANDLE,DWORD,void *,DWORD,DWORD,HANDLE *);
static BOOL (CALL *get_info)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *set_token)(HANDLE *,HANDLE),(CALL *revert_self)(void),(CALL *close_handle)(HANDLE);
static HANDLE (CALL *open_thread)(DWORD,BOOL,DWORD);
static DWORD (CALL *get_tid)(void),(CALL *wait_object)(HANDLE,DWORD),(CALL *module_name)(HANDLE,char *,DWORD);
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static BOOL (CALL *terminate)(HANDLE,DWORD);
static void (CALL *set_error)(DWORD);
static DWORD failures;
static void report(const char *s)
{DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void hex(DWORD v)
{char s[9];for(int i=7;i>=0;i--){s[i]="0123456789abcdef"[v&15];v>>=4;}s[8]=0;report(s);}
static void check(BOOL ok,const char *label)
{if(!ok){failures++;report("THREAD-INFO FAIL: ");report(label);report("\n");}}
static LONG probe(const char *label,HANDLE thread,DWORD cls,void *data,DWORD size,DWORD expected)
{
    set_error(0x13572468);LONG s=nt_set(thread,cls,data,size);
    check(GetLastError()==0x13572468,"NT leaves last error untouched");
    report("THREAD-INFO ");report(label);report(" status=");hex((DWORD)s);report("\n");
    check((DWORD)s==expected,label);return s;
}
static void close(HANDLE h){check(close_handle(h),"close handle");}
static void same_token(HANDLE a,HANDLE b)
{
    DWORD x[16]={0},y[16]={0},n=0;
    check(get_info(a,10,x,sizeof(x),&n)&&get_info(b,10,y,sizeof(y),&n),"token statistics");
    check(x[0]==y[0]&&x[1]==y[1]&&x[2]==y[2]&&x[3]==y[3],"assigned token identity");
}
static void thread_token(HANDLE thread,HANDLE expected)
{
    HANDLE actual=0;BOOL ok=open_thread_token(thread,8,1,&actual);
    check(ok,"query assigned thread token");if(ok){same_token(actual,expected);close(actual);}
}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),a=LoadLibraryA("advapi32.dll"),n=LoadLibraryA("ntdll.dll");
#define LOAD(mod,var,name) var=GetProcAddress(mod,name);if(!var){report("THREAD-INFO MISSING " name "\n");ExitProcess(2);}
    LOAD(n,nt_set,"NtSetInformationThread");LOAD(n,zw_set,"ZwSetInformationThread");
    LOAD(a,open_token,"OpenProcessToken");LOAD(a,open_thread_token,"OpenThreadToken");
    LOAD(a,dup_token,"DuplicateTokenEx");LOAD(a,get_info,"GetTokenInformation");
    LOAD(a,set_token,"SetThreadToken");LOAD(a,revert_self,"RevertToSelf");
    LOAD(k,close_handle,"CloseHandle");LOAD(k,open_thread,"OpenThread");
    LOAD(k,get_tid,"GetCurrentThreadId");LOAD(k,set_error,"SetLastError");
    LOAD(k,create_process,"CreateProcessA");LOAD(k,terminate,"TerminateProcess");
    LOAD(k,wait_object,"WaitForSingleObject");LOAD(k,module_name,"GetModuleFileNameA");
#undef LOAD
    HANDLE primary=0,imp=0,empty=0;
    check(open_token(SELF,0x02000000,&primary),"open primary");
    check(dup_token(primary,0x02000000,0,2,2,&imp),"duplicate impersonation");
    probe("assign-self",THREAD,5,&imp,sizeof(imp),0);
    thread_token(THREAD,imp);
    check(zw_set(THREAD,5,&empty,sizeof(empty))==0,"Zw reverts token");
    HANDLE h=0;check(!open_thread_token(THREAD,8,1,&h)&&GetLastError()==1008,"reverted thread has no token");
    probe("wrong-class",THREAD,0xffffffff,&imp,sizeof(imp),0xc0000003);
    probe("short-input",THREAD,5,&imp,sizeof(imp)-1,0xc0000004);
    probe("long-input",THREAD,5,&imp,sizeof(imp)+1,sizeof(HANDLE)==4?0:0xc0000004);
    probe("null-input",THREAD,5,0,sizeof(imp),0xc0000005);
    probe("bad-input",THREAD,5,(void *)(UPTR)1,sizeof(imp),sizeof(HANDLE)==4?0xc0000005:0x80000002);
    _Alignas(8) BYTE unaligned[24]={0};memcpy(unaligned+1,&empty,sizeof(empty));
    probe("unaligned-input",THREAD,5,unaligned+1,sizeof(empty),sizeof(HANDLE)==4?0:0x80000002);
    probe("two-aligned-input",THREAD,5,unaligned+2,sizeof(empty),sizeof(HANDLE)==4?0:0x80000002);
    probe("four-aligned-input",THREAD,5,unaligned+4,sizeof(empty),0);
    probe("aligned-bad-input",THREAD,5,(void *)(UPTR)8,sizeof(empty),0xc0000005);
    probe("invalid-thread",0,5,&imp,sizeof(imp),0xc0000008);
    probe("process-pseudo",SELF,5,&imp,sizeof(imp),0xc0000024);
    probe("token-as-thread",primary,5,&imp,sizeof(imp),0xc0000024);
    probe("primary-as-token",THREAD,5,&primary,sizeof(primary),0xc00000a8);
    HANDLE invalid=(HANDLE)(UPTR)0x12345678;
    probe("invalid-token",THREAD,5,&invalid,sizeof(invalid),0xc0000008);
    HANDLE wrong=THREAD;probe("thread-as-token",THREAD,5,&wrong,sizeof(wrong),sizeof(HANDLE)==4?0xc0000008:0xc0000024);
    if(sizeof(HANDLE)==8){
        probe("zero-extended-thread",(HANDLE)(UPTR)0xfffffffeU,5,&imp,sizeof(imp),0xc0000008);
        probe("high-thread",(HANDLE)(((UPTR)3<<31)+0x7ffffffeU),5,&imp,sizeof(imp),0xc0000008);
    }
    DWORD masks[]={0x20,0x80,0x40,0x800,0x400,0x100000};
    for(DWORD i=0;i<sizeof(masks)/sizeof(masks[0]);i++){
        HANDLE thread=open_thread(masks[i],0,get_tid());check(thread!=0,"open restricted thread");
        report("THREAD-INFO source-access=");hex(masks[i]);report("\n");
        probe("thread-rights",thread,5,&imp,sizeof(imp),masks[i]==0x80?0:0xc0000022);
        check(revert_self(),"restore thread identity");
        set_error(0);BOOL ok=set_token(&thread,imp);DWORD error=GetLastError();
        check(revert_self(),"restore Win32 identity");
        report("THREAD-INFO Win32 rights result/error=");hex(ok);report("/");hex(error);report("\n");
        check(masks[i]==0x80?ok:(!ok&&error==5),"Win32 THREAD_SET_THREAD_TOKEN required");
        close(thread);
    }
    DWORD token_masks[]={4,8,2};
    for(DWORD i=0;i<3;i++){
        HANDLE t=0;check(dup_token(primary,token_masks[i],0,2,2,&t),"create limited token");
        report("THREAD-INFO token-access=");hex(token_masks[i]);report("\n");
        probe("token-rights",THREAD,5,&t,sizeof(t),i==0?0:0xc0000022);
        check(revert_self(),"restore after limited token");close(t);
    }
    for(DWORD i=0;i<3;i++){
        char path[320],command[]="threadinfo --suspended-child";module_name(0,path,sizeof(path));
        STARTUP startup={0};startup.cb=sizeof(startup);PROCESS pi={0};
        check(create_process(path,command,0,0,0,4,0,0,&startup,&pi),"create suspended child");
        if(!pi.process)break;
        HANDLE child_token=0,retained=0;
        check(dup_token(primary,0x02000000,0,2,2,&child_token),"child impersonation token");
        probe("assign-child",pi.thread,5,&child_token,sizeof(child_token),0);
        thread_token(pi.thread,child_token);
        check(open_thread_token(pi.thread,8,1,&retained),"retain assigned child token");
        close(child_token);thread_token(pi.thread,retained);
        check(terminate(pi.process,93)&&wait_object(pi.process,5000)==0,"terminate selected child");
        probe("exited-thread",pi.thread,5,&empty,sizeof(empty),0);
        LONG exited=probe("exited-assign",pi.thread,5,&imp,sizeof(imp),0);
        if(!exited)thread_token(pi.thread,imp);
        close(pi.thread);close(pi.process);
        DWORD type=0,bytes=0;check(get_info(retained,8,&type,4,&bytes)&&type==2,"token survives thread destruction");
        close(retained);
    }
    close(imp);close(primary);
    report(failures?"THREAD-INFO FAILED\n":"THREAD-INFO PASS\n");ExitProcess(failures?1:0);
}
