/* Native Windows and OsitoK execute the same fixture, without a CRT. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int LONG;
typedef int BOOL;
typedef __SIZE_TYPE__ SIZE;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define SELF ((HANDLE)(UPTR)-1)
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
typedef struct {LONG status;void *peb;UPTR affinity;LONG priority;UPTR pid,parent;} BASIC;
typedef struct {DWORD cb;WORD *reserved,*desktop,*title;DWORD x,y,w,h,xc,yc,fill,flags;
    WORD show,bytes;BYTE *data;HANDLE input,output,error;} STARTUP;
typedef struct {HANDLE process,thread;DWORD pid,tid;} PROCESS;
static LONG (CALL *nt_open)(HANDLE *,DWORD,OA *,CID *);
static LONG (CALL *zw_open)(HANDLE *,DWORD,OA *,CID *);
static LONG (CALL *query_object)(HANDLE,DWORD,void *,DWORD,DWORD *);
static LONG (CALL *query_process)(HANDLE,DWORD,void *,DWORD,DWORD *);
static HANDLE (CALL *open_process)(DWORD,BOOL,DWORD);
static DWORD (CALL *get_pid)(void),(CALL *get_tid)(void),(CALL *process_id)(HANDLE);
static DWORD (CALL *wait_object)(HANDLE,DWORD),(CALL *resume)(HANDLE);
static BOOL (CALL *close_handle)(HANDLE),(CALL *handle_flags)(HANDLE,DWORD *);
static BOOL (CALL *get_exit)(HANDLE,DWORD *),(CALL *terminate)(HANDLE,DWORD);
static BOOL (CALL *duplicate)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static BOOL (CALL *read_memory)(HANDLE,const void *,void *,SIZE,SIZE *);
static LONG (CALL *nt_write_memory)(HANDLE,void *,const void *,SIZE,SIZE *);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static char *(CALL *command_line)(void);
static void (CALL *sleep_ms)(DWORD);
static DWORD failures;
static volatile DWORD child_marker=0x1234abcd;
static void report(const char *s)
{DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void hex(DWORD v)
{char s[9];for(int i=7;i>=0;i--){s[i]="0123456789abcdef"[v&15];v>>=4;}s[8]=0;report(s);}
static void check(BOOL ok,const char *s)
{if(!ok){failures++;report("OPEN-PROCESS FAIL: ");report(s);report("\n");}}
static const char *find(const char *s,const char *word)
{for(;*s;s++){DWORD i=0;while(word[i]&&s[i]==word[i])i++;if(!word[i])return s+i;}return 0;}
static DWORD granted(HANDLE handle)
{
    DWORD basic[14]={0},n=0;
    check(query_object(handle,0,basic,sizeof(basic),&n)==0,"query granted access");
    return basic[1];
}
static LONG attempt(const char *label,DWORD access,OA *oa,CID *cid,HANDLE *result)
{
    struct {HANDLE handle;DWORD guard;} out={(HANDLE)(UPTR)0x12345678,0xfeedbeef};
    LONG s=nt_open(&out.handle,access,oa,cid);
    check(out.guard==0xfeedbeef,"output handle width");
    if(s&&sizeof(UPTR)==8)
        check(out.handle==(HANDLE)(UPTR)0x12345678,"native failure preserves output");
    report("OPEN-PROCESS ");report(label);report(" status=");hex((DWORD)s);
    if(s==0){report(" access=");hex(granted(out.handle));}
    else{report(" output=");hex((DWORD)(UPTR)out.handle);}
    report("\n");
    if(s==0){if(result)*result=out.handle;else close_handle(out.handle);}
    return s;
}
static void retired(OA *oa,CID *cid)
{
    for(DWORD i=0;i<100;i++){
        HANDLE h=0;LONG s=nt_open(&h,0x1000,oa,cid);
        if((DWORD)s==0xc000000b)return;
        if(s==0)close_handle(h);else break;
        sleep_ms(10);
    }
    check(0,"unreferenced process CID retired");
}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),n=LoadLibraryA("ntdll.dll");
#define LOAD(module,var,name) var=GetProcAddress(module,name);if(!var){report("OPEN-PROCESS MISSING " name "\n");ExitProcess(2);}
    LOAD(n,nt_open,"NtOpenProcess");LOAD(n,zw_open,"ZwOpenProcess");
    LOAD(n,query_object,"NtQueryObject");LOAD(n,query_process,"NtQueryInformationProcess");
    LOAD(n,nt_write_memory,"NtWriteVirtualMemory");
    LOAD(k,open_process,"OpenProcess");LOAD(k,get_pid,"GetCurrentProcessId");
    LOAD(k,get_tid,"GetCurrentThreadId");LOAD(k,process_id,"GetProcessId");
    LOAD(k,wait_object,"WaitForSingleObject");LOAD(k,resume,"ResumeThread");
    LOAD(k,close_handle,"CloseHandle");LOAD(k,handle_flags,"GetHandleInformation");
    LOAD(k,get_exit,"GetExitCodeProcess");LOAD(k,terminate,"TerminateProcess");
    LOAD(k,duplicate,"DuplicateHandle");LOAD(k,create_process,"CreateProcessA");
    LOAD(k,read_memory,"ReadProcessMemory");LOAD(k,module_name,"GetModuleFileNameA");
    LOAD(k,command_line,"GetCommandLineA");LOAD(k,sleep_ms,"Sleep");
#undef LOAD
    OA oa={0};oa.length=sizeof(oa);
    CID self={(HANDLE)(UPTR)get_pid(),0};HANDLE h=0;DWORD flags=0;
    const char *child=find(command_line(),"--child ");
    if(child){
        UPTR inherited=0;for(DWORD i=0;i<8;i++){
            char c=child[i];inherited=(inherited<<4)|(c<='9'?c-'0':c-'a'+10);
        }
        h=(HANDLE)inherited;
        check(h&&handle_flags(h,&flags)&&(flags&1),"child inherited handle");
        check(process_id(h)!=0&&process_id(h)!=get_pid(),"child handle refers to parent");
        BASIC b={0};DWORD bytes=0;
        check(query_process(h,0,&b,sizeof(b),&bytes)==0&&b.pid==process_id(h),"child NT parent identity");
        check(close_handle(h),"child closes its own inherited entry");
        report(failures?"OPEN-PROCESS CHILD FAILED\n":"OPEN-PROCESS CHILD PASS\n");
        ExitProcess(failures?94:93);
    }
    check(attempt("pid",0x100410,&oa,&self,0)==0,"open by PID");
    CID full={self.pid,(HANDLE)(UPTR)get_tid()},only={0,full.tid};
    check(attempt("full-cid",0x400,&oa,&full,0)==0,"matching full CID");
    check((DWORD)attempt("tid-only",0x400,&oa,&only,0)==0xc000000b,"PID required");
    CID bad={self.pid,(HANDLE)(UPTR)0x7ffffffc};
    check((DWORD)attempt("bad-tid",0x400,&oa,&bad,0)==0xc000000b,"unknown TID rejected");
    bad.pid=(HANDLE)(UPTR)0x7ffffffc;bad.tid=full.tid;
    check((DWORD)attempt("mismatched-pid",0x400,&oa,&bad,0)==0xc000000b,"mismatched CID rejected");
    bad.tid=0;check((DWORD)attempt("unknown-pid",0x400,&oa,&bad,0)==0xc000000b,"unknown PID rejected");
    bad.pid=0;check((DWORD)attempt("zero-cid",0x400,&oa,&bad,0)==0xc000000b,"zero CID rejected");
    h=0;check(zw_open(&h,0x1000,&oa,&self)==0,"Zw alias callable");if(h)close_handle(h);
    struct {DWORD requested,status,access;} masks[]={
        {0,0xc0000022,0},{0x80000000,0,0x21410},{0x40000000,0,0x23bea},
        {0x20000000,0,0x121001},{0x10000000,0,0x1fffff},{0x02000000,0,0x1fffff},
        {0x08000000,0xc0000022,0},{0x01000000,0xc0000061,0},
        {0x8000,0,0x8000},{0x400,0,0x1400},{0x200,0,0x2200}};
    for(DWORD i=0;i<sizeof(masks)/sizeof(masks[0]);i++){
        report("OPEN-PROCESS requested=");hex(masks[i].requested);report("\n");
        h=0;
        check((DWORD)attempt("mask",masks[i].requested,&oa,&self,&h)==masks[i].status,"requested access status");
        if(h){check(granted(h)==masks[i].access,"generic and implied access mapping");close_handle(h);}
    }
    check((DWORD)attempt("null-oa",0x400,0,&self,0)==0xc0000005,"null OA rejected");
    check((DWORD)attempt("null-cid",0x400,&oa,0,0)==0xc0000030,"null CID rejected");
    oa.length=0;
    check((DWORD)attempt("bad-oa-length",0x400,&oa,&self,0)==
          (sizeof(UPTR)==8?0:0xc000000d),"OA length marshalling");oa.length=sizeof(oa);
    WORD text[]={'x',0};STRING name={2,4,text};oa.name=&name;
    check((DWORD)attempt("named",0x400,&oa,&self,0)==0xc0000030,"object name rejected");oa.name=0;
    oa.root=SELF;check(attempt("root",0x400,&oa,&self,0)==0,"CID ignores root directory");oa.root=0;
    DWORD attrs[]={1,2,0x40,0x200,0x80000000};
    for(DWORD i=0;i<sizeof(attrs)/sizeof(attrs[0]);i++){
        oa.flags=attrs[i];check(attempt("attribute",0x400,&oa,&self,0)==0,"CID attribute handling");
    }
    oa.flags=0;
    check((DWORD)nt_open(0,0x400,&oa,&self)==0xc0000005,"null output rejected");
    DWORD misaligned=sizeof(UPTR)==8?0x80000002:0xc0000005;
    check((DWORD)attempt("bad-cid-pointer",0x400,&oa,(CID *)(UPTR)1,0)==misaligned,"bad CID pointer");
    check((DWORD)attempt("bad-oa-pointer",0x400,(OA *)(UPTR)1,&self,0)==misaligned,"bad OA pointer");
    check((DWORD)attempt("unmapped-cid",0x400,&oa,(CID *)(UPTR)4096,0)==0xc0000005,"unmapped CID");
    check((DWORD)attempt("unmapped-oa",0x400,(OA *)(UPTR)4096,&self,0)==0xc0000005,"unmapped OA");
    if(sizeof(UPTR)==8){
        CID high={(HANDLE)((UPTR)self.pid+((UPTR)1<<31)*2),0};
        check((DWORD)attempt("high-pid",0x400,&oa,&high,0)==0xc000000b,"PID cannot truncate");
        high.pid=self.pid;high.tid=(HANDLE)((UPTR)full.tid+((UPTR)1<<31)*2);
        check((DWORD)attempt("high-tid",0x400,&oa,&high,0)==0xc000000b,"TID cannot truncate");
    }
    check(!open_process(0x1000,0,0)&&GetLastError()==87,"Win32 invalid PID error");
    h=open_process(0x1000,1,get_pid());flags=0;
    check(h&&handle_flags(h,&flags)&&(flags&1),"Win32 inherit flag");if(h)close_handle(h);
    oa.flags=2;h=0;attempt("inherited",0x1000,&oa,&self,&h);flags=0;
    check(h&&handle_flags(h,&flags)&&(flags&1),"NT inherit flag");if(h)close_handle(h);oa.flags=0;
    HANDLE restricted=open_process(0x100000,0,get_pid());DWORD code=0;
    check(restricted&&!process_id(restricted)&&GetLastError()==5,"GetProcessId query rights");
    check(restricted&&!get_exit(restricted,&code)&&GetLastError()==5,"GetExitCodeProcess query rights");
    BASIC basic={0};DWORD bytes=0;
    LONG qs=query_process(restricted,0,&basic,sizeof(basic),&bytes);
    report("OPEN-PROCESS restricted-query status=");hex((DWORD)qs);report("\n");
    check((DWORD)qs==0xc0000022,"NT query rights");
    if(restricted)close_handle(restricted);
    HANDLE inherited=open_process(0x1000,1,get_pid());
    check(inherited!=0,"inheritance source");
    for(DWORD cycle=0;cycle<4&&inherited;cycle++){
        char path[320],command[]="openprocess --child 00000000";
        module_name(0,path,sizeof(path));
        for(DWORD i=0;i<8;i++)command[20+i]="0123456789abcdef"[((UPTR)inherited>>(28-i*4))&15];
        PROCESS p={0};STARTUP startup={0};startup.cb=sizeof(startup);
        check(create_process(path,command,0,0,1,4,0,0,&startup,&p),"create suspended child");
        if(!p.process)break;
        CID cid={(HANDLE)(UPTR)p.pid,0};HANDLE opened=0,copy=0;
        CID child_full={cid.pid,(HANDLE)(UPTR)p.tid};
        check(attempt("child-full-cid",0x1000,&oa,&child_full,0)==0,"child full CID");
        child_full.tid=full.tid;
        check((DWORD)attempt("cross-process-tid",0x1000,&oa,&child_full,0)==0xc000000b,"thread must belong to target process");
        HANDLE query_only=0;
        check(attempt("query-only",0x1000,&oa,&cid,&query_only)==0,"query-only child handle");
        if(query_only){
            DWORD marker=0;SIZE got=0;
            check(!read_memory(query_only,(const void *)&child_marker,&marker,sizeof(marker),&got)&&
                  GetLastError()==5,"query rights do not imply VM_READ");
            check((DWORD)nt_write_memory(query_only,(void *)&child_marker,&marker,sizeof(marker),&got)==
                  0xc0000022,"query rights do not imply VM_WRITE");
            close_handle(query_only);
        }
        check(attempt("child",0x100411,&oa,&cid,&opened)==0,"open suspended process");
        if(!opened){terminate(p.process,91);wait_object(p.process,5000);close_handle(p.thread);close_handle(p.process);break;}
        check(process_id(opened)==p.pid,"opened child identity");
        DWORD marker=0;SIZE got=0;
        check(read_memory(opened,(const void *)&child_marker,&marker,sizeof(marker),&got)&&
              got==sizeof(marker)&&marker==0x1234abcd,"opened child VM_READ");
        check(duplicate(SELF,opened,SELF,&copy,0,0,2),"duplicate opened process");
        check(close_handle(p.process),"close original process handle");
        if(cycle<2)check(resume(p.thread)==1,"resume child");
        else check(terminate(opened,93),"terminate suspended child through opened process");
        check(wait_object(opened,15000)==0&&get_exit(opened,&code)&&code==93,"retained process wait/exit");
        check(attempt("exited-referenced",0x1000,&oa,&cid,0)==0,"open referenced exited process");
        check(close_handle(opened)&&close_handle(copy)&&close_handle(p.thread),"process handle cleanup");
        retired(&oa,&cid);
        check(process_id(inherited)==get_pid(),"child close did not close parent's handle");
    }
    if(inherited)close_handle(inherited);
    report(failures?"OPEN-PROCESS FAILED\n":"OPEN-PROCESS PASS\n");
    ExitProcess(failures?1:0);
}
