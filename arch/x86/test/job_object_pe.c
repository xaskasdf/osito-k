/* Same PE32/PE64 contract fixture runs on Windows and OsitoK. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef void *HANDLE;
typedef __SIZE_TYPE__ SIZE;
typedef unsigned long long U64;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define SELF ((HANDLE)(__INTPTR_TYPE__)-1)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
void *memcpy(void *out,const void *in,SIZE n)
{ for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=((const BYTE *)in)[i];return out; }
void *memset(void *out,int c,SIZE n)
{ for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=(BYTE)c;return out; }
typedef struct {
    U64 process_time,job_time; DWORD flags;
    SIZE min_ws,max_ws; DWORD active; SIZE affinity;
    DWORD priority,scheduling;
} BASIC;
typedef struct { BASIC basic; U64 io[6]; SIZE process_memory,job_memory,peak_process,peak_job; } EXTENDED;
typedef struct { U64 times[4]; DWORD faults,total,active,terminated; } ACCOUNTING;
typedef struct {
    DWORD cb; char *reserved,*desktop,*title;
    DWORD x,y,w,h,xc,yc,fill,flags; WORD show,bytes; BYTE *data;
    HANDLE input,output,error;
} STARTUP;
typedef struct { HANDLE process,thread; DWORD pid,tid; } PROCESS;
_Static_assert(sizeof(BASIC)==(sizeof(void *)==4?48:64),"basic ABI");
_Static_assert(sizeof(EXTENDED)==(sizeof(void *)==4?112:144),"extended ABI");
_Static_assert(sizeof(ACCOUNTING)==48,"accounting ABI");
static HANDLE (CALL *create_job)(void *,const WORD *);
static BOOL (CALL *set_job)(HANDLE,DWORD,const void *,DWORD);
static BOOL (CALL *query_job)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *assign_job)(HANDLE,HANDLE);
static BOOL (CALL *in_job)(HANDLE,HANDLE,BOOL *);
static BOOL (CALL *terminate_job)(HANDLE,DWORD);
static BOOL (CALL *close_handle)(HANDLE);
static BOOL (CALL *handle_flags)(HANDLE,DWORD,DWORD);
static BOOL (CALL *terminate_process)(HANDLE,DWORD);
static DWORD (CALL *nt_close)(HANDLE);
static BOOL (CALL *dup_handle)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static DWORD (CALL *wait_object)(HANDLE,DWORD);
static BOOL (CALL *exit_code)(HANDLE,DWORD *);
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static DWORD (CALL *resume_thread)(HANDLE);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static char *(CALL *command_line)(void);
static void (CALL *sleep_ms)(DWORD);
static void *(CALL *alloc)(void *,SIZE,DWORD,DWORD);
static BOOL (CALL *free_vm)(void *,SIZE,DWORD);
static HANDLE (CALL *get_heap)(void);
static void *(CALL *heap_alloc)(HANDLE,DWORD,SIZE);
static HANDLE (CALL *create_event)(void *,BOOL,BOOL,const char *);
static BOOL (CALL *set_event)(HANDLE);
static void (CALL *raise_exception)(DWORD,DWORD,DWORD,const SIZE *);
static DWORD (CALL *set_error_mode)(DWORD);
static void *(CALL *add_vectored)(DWORD,void *);
static DWORD (CALL *remove_vectored)(void *);
static BOOL continue_exception;
static DWORD failures;
static char path[320];
static HANDLE expected_job;
static HANDLE ready_event,release_event;

static void report(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void hex(DWORD n)
{ char s[9];for(DWORD i=0;i<8;i++)s[i]="0123456789abcdef"[(n>>(28-4*i))&15];s[8]=0;report(s); }
static void check(BOOL ok,const char *s)
{ if(!ok){failures++;report("JOB-OBJECT FAIL: ");report(s);report(" error=");hex(GetLastError());report("\n");} }
static BOOL contains(const char *text,const char *needle)
{ for(;*text;text++){DWORD i=0;while(needle[i]&&text[i]==needle[i])i++;if(!needle[i])return 1;}return 0; }
static DWORD append_handle(char *cmd,DWORD i,char key,HANDLE handle)
{
    cmd[i++]=' ';cmd[i++]=key;cmd[i++]=':';
    for(DWORD d=sizeof(SIZE)*2;d;d--)cmd[i++]="0123456789abcdef"[((SIZE)handle>>((d-1)*4))&15];
    return i;
}
static BOOL spawn(const char *args,DWORD flags,PROCESS *pi)
{
    char cmd[180]="job-object ";DWORD i=11,j=0;
    while(args[j]&&i+64<sizeof(cmd))cmd[i++]=args[j++];
    if(expected_job)i=append_handle(cmd,i,'j',expected_job);
    if(ready_event)i=append_handle(cmd,i,'r',ready_event);
    if(release_event)i=append_handle(cmd,i,'s',release_event);
    cmd[i]=0;
    STARTUP startup={0};startup.cb=sizeof(startup);memset(pi,0,sizeof(*pi));
    return create_process(path,cmd,0,0,expected_job!=0,flags,0,0,&startup,pi);
}
static DWORD finish(PROCESS *pi,BOOL resume)
{
    DWORD code=0xffffffff;
    if(resume)check(resume_thread(pi->thread)==1,"resume child");
    DWORD wait=wait_object(pi->process,30000);
    check(wait==0,"wait child exit");
    if(wait!=0){terminate_process(pi->process,99);wait_object(pi->process,5000);}
    check(exit_code(pi->process,&code),"get child exit code");
    close_handle(pi->thread);close_handle(pi->process);
    return code;
}
static void expect_member(HANDLE process,HANDLE job,BOOL expected,const char *label)
{ BOOL member=!expected;check(in_job(process,job,&member)&&member==expected,label); }
static int CALL job_vectored_filter(void *info)
{
    DWORD *record=*(DWORD **)info;
    if(record[0]!=0xc000001d)return 0;
    set_event(ready_event);
    return continue_exception ? -1 : 0;
}
static void exception_child(BOOL caught)
{
    EXTENDED limits={0};
    check(query_job(0,9,&limits,sizeof(limits),0)&&(limits.basic.flags&0x400),
          "exception child observes die-on-unhandled limit");
    if(failures)return;
    continue_exception=caught;
    set_error_mode(0);
    void *veh=add_vectored(1,job_vectored_filter);
    check(veh!=0,"install exception handler");
    if(!veh)return;
    raise_exception(0xc000001d,0,0,0);
    check(caught,"unhandled exception must not return");
    check(remove_vectored(veh)!=0,"remove exception handler");
}
static void exception_contract(void)
{
    HANDLE job=create_job(0,0);
    EXTENDED limits={0},readback={0};
    BASIC basic={0};basic.flags=0x400;
    check(!set_job(job,2,&basic,sizeof(basic))&&GetLastError()==87,
          "die-on-unhandled requires extended information");
    limits.basic.flags=0x400;
    BOOL configured=set_job(job,9,&limits,sizeof(limits));
    check(configured,"configure die-on-unhandled limit");
    if(!configured){close_handle(job);return;}
    check(query_job(job,9,&readback,sizeof(readback),0)&&readback.basic.flags==0x400,
          "query die-on-unhandled limit");
    basic.flags=8;basic.active=3;
    check(set_job(job,2,&basic,sizeof(basic)),"basic update with die-on-unhandled");
    check(query_job(job,9,&readback,sizeof(readback),0)&&readback.basic.flags==0x408,
          "basic update preserves die-on-unhandled");
    limits.basic.flags=0x80000400;
    check(!set_job(job,9,&limits,sizeof(limits))&&GetLastError()==87,
          "invalid flags still rejected with die-on-unhandled");
    check(query_job(job,9,&readback,sizeof(readback),0)&&readback.basic.flags==0x408,
          "failed update leaves die-on-unhandled intact");
    limits.basic.flags=0;
    check(set_job(job,9,&limits,sizeof(limits)),"clear die-on-unhandled");
    check(query_job(job,9,&readback,sizeof(readback),0)&&readback.basic.flags==0,
          "extended update clears die-on-unhandled");
    check(handle_flags(job,1,1),"inherit exception job handle");expected_job=job;
    ready_event=create_event(0,1,0,0);
    check(ready_event&&handle_flags(ready_event,1,1),"exception filter event");
    PROCESS peer={0},fault={0};
    BOOL peer_created=spawn("--member",4,&peer);
    check(peer_created,"create exception peer");
    if(peer_created)check(assign_job(job,peer.process),"assign before enabling exception limit");
    limits.basic.flags=0x400;
    check(set_job(job,9,&limits,sizeof(limits)),"enable die-on-unhandled on populated job");
    for(DWORD caught=0;caught<2;caught++){
        BOOL made=spawn(caught?"--caught-job":"--fault-job",4,&fault);
        check(made,"create exception child");
        if(made){
            check(assign_job(job,fault.process),"assign exception child");
            DWORD code=finish(&fault,1);
            report("Exception mode/exit=");hex(caught);report("/");hex(code);report("\n");
            check(code==(caught?0:0xc000001d),
                  caught?"handled exception resumes despite job limit":"unhandled exception preserves exit code");
            check(wait_object(ready_event,0)==0,"job limit does not bypass vectored handler");
        }
        close_handle(ready_event);
        ready_event=caught?0:create_event(0,1,0,0);
        if(ready_event)check(handle_flags(ready_event,1,1),"inherit next exception event");
    }
    if(peer_created){
        check(wait_object(peer.process,0)==258,"exception does not terminate other job members");
        check(finish(&peer,1)==0,"exception peer can still run");
    }
    close_handle(job);expected_job=0;
}
static void memory_child(void)
{
    EXTENDED limits={0};DWORD returned=0;
    check(query_job(0,9,&limits,sizeof(limits),&returned)&&returned==sizeof(limits),"query current job");
    SIZE cap=limits.process_memory?limits.process_memory:limits.job_memory;
    check(cap>=32*1024*1024,"child observes configured memory limit");
    void *reserve=alloc(0,cap*2,0x2000,4);
    check(reserve!=0,"reservation does not consume commit quota");
    if(reserve){
        check(!alloc(reserve,cap*2,0x1000,4),"commit beyond job limit denied");
        BYTE *p=alloc(reserve,8*1024*1024,0x1000,4);
        check(p==reserve,"commit within limit");
        if(p){
            p[0]=0x53;
            for(DWORD i=0;i<16;i++)check(alloc(p,8*1024*1024,0x1000,4)==p&&p[0]==0x53,"recommit has no duplicate charge");
            for(DWORD i=0;i<16;i++){
                check(free_vm(p,8*1024*1024,0x4000),"decommit returns quota");
                check(alloc(p,8*1024*1024,0x1000,4)==p&&p[0]==0,"recommit zeroes new pages");
            }
        }
        check(free_vm(reserve,0,0x8000),"release reservation");
    }
    check(!heap_alloc(get_heap(),0,cap*2),"heap growth obeys memory limit");
    for(DWORD i=0;i<12;i++){
        void *p=alloc(0,8*1024*1024,0x3000,4);
        check(p!=0,"release makes commit quota reusable");
        if(p)check(free_vm(p,0,0x8000),"release charged allocation");
    }
}
static void inheritance_child(const char *cmd)
{
    EXTENDED limits={0};check(query_job(0,9,&limits,sizeof(limits),0),"parent has job");
    PROCESS grandchild={0};BOOL silent=(limits.basic.flags&0x1000)!=0;
    if(contains(cmd,"--deny-spawn")) {
        BOOL made=spawn("--member",0,&grandchild);
        check(!made,"inherited active limit denies child creation");
        if(made)finish(&grandchild,0);
        return;
    }
    BOOL made=spawn(silent?"--outside":"--member",0,&grandchild);
    check(made,"create inherited grandchild");
    if(made)check(finish(&grandchild,0)==0,"grandchild membership");
    made=spawn("--outside",0x01000000,&grandchild);
    if(limits.basic.flags&(0x0800|0x1000)) {
        check(made,"allowed breakaway creates child");
        if(made)check(finish(&grandchild,0)==0,"breakaway child outside job");
    }else{
        check(!made&&GetLastError()==5,"denied breakaway is not silently ignored");
        if(made)finish(&grandchild,0);
    }
}
static void contract(void)
{
    exception_contract();
    HANDLE many[20]={0};
    for(DWORD i=0;i<20;i++){many[i]=create_job(0,0);check(many[i]!=0,"more than eight independent jobs");}
    for(DWORD i=0;i<20;i++)if(many[i])close_handle(many[i]);
    HANDLE job=create_job(0,0),alias=0,query_only=0;
    EXTENDED limits={0},readback={0};DWORD ui=0xdeadbeef,returned=0;
    check(query_job(job,4,&ui,4,&returned)&&ui==0&&returned==4,"new job has no UI restrictions");
    check(set_job(job,4,&ui,4),"zero UI restrictions accepted");
    check(!set_job(job,4,&ui,3)&&GetLastError()==24,"short UI setter rejected");
    check(!set_job(job,4,(void *)1,4)&&GetLastError()==998,"invalid UI input rejected");
    ui=0x10000;
    check(!set_job(job,4,&ui,4)&&GetLastError()==87,"invalid UI mask rejected");
    check(!query_job(job,9,&readback,sizeof(readback)-1,0)&&GetLastError()==24,"short limit query rejected");
    limits.basic.flags=0x2108;limits.basic.active=1;limits.process_memory=128*1024*1024;
    check(set_job(job,9,&limits,sizeof(limits)),"set extended limits with correct ABI");
    check(query_job(job,9,&readback,sizeof(readback),&returned)&&returned==sizeof(readback)&&
          readback.basic.flags==limits.basic.flags&&readback.basic.active==1&&
          readback.process_memory==limits.process_memory,"query complete limits");
    BASIC basic={0};basic.flags=8;basic.active=2;
    check(set_job(job,2,&basic,sizeof(basic)),"set basic limits");
    check(query_job(job,9,&readback,sizeof(readback),0)&&readback.basic.flags==0x2108&&
          readback.basic.active==2&&readback.process_memory==limits.process_memory,
          "basic update preserves extended limits");
    check(set_job(job,9,&limits,sizeof(limits)),"restore active limit");
    check(dup_handle(SELF,job,SELF,&query_only,4,0,0),"query-only job handle");
    ui=0;
    check(!set_job(query_only,4,&ui,4)&&GetLastError()==5,"set requires attributes access");
    check(!terminate_job(query_only,9)&&GetLastError()==5,"terminate requires terminate access");
    check(query_job(query_only,4,&ui,4,0),"query right permits query");
    close_handle(query_only);
    PROCESS first={0},second={0};
    if(spawn("--deny-spawn",4,&first)){
        check(assign_job(job,first.process),"assign first child");
        expect_member(first.process,job,1,"assigned child membership");
        if(spawn("--member",4,&second)){
            BOOL result=assign_job(job,second.process);DWORD error=GetLastError();
            report("Active limit result/error=");hex(result);report("/");hex(error);report("\n");
            check(!result,"active limit rejects second child");
            DWORD code=finish(&second,wait_object(second.process,0)==258);
            report("Active limit exit=");hex(code);report("\n");
        }else check(0,"create second child");
        check(finish(&first,1)==0,"child observes active process limit");
    }else check(0,"create first child");
    ACCOUNTING account={0};
    check(query_job(job,1,&account,sizeof(account),0)&&account.active==0,"exited members no longer active");
    report("Accounting total/active/terminated=");hex(account.total);report("/");hex(account.active);report("/");hex(account.terminated);report("\n");
    close_handle(job);

    job=create_job(0,0);memset(&limits,0,sizeof(limits));limits.basic.flags=0x2000;
    check(set_job(job,9,&limits,sizeof(limits)),"kill-on-close limit");
    check(dup_handle(SELF,job,SELF,&alias,0,0,2),"duplicate job reference");
    if(spawn("--member",4,&first)){
        check(assign_job(job,first.process),"assign close-lifetime child");
        close_handle(job);
        check(wait_object(first.process,0)==258,"not killed while duplicate handle exists");
        check(nt_close(alias)==0,"NtClose drops last handle");
        (void)finish(&first,0);
    }else{check(0,"create close-lifetime child");close_handle(job);close_handle(alias);}

    for(DWORD mode=0;mode<3;mode++){
        job=create_job(0,0);memset(&limits,0,sizeof(limits));
        limits.basic.flags=mode==1?0x800:mode==2?0x1000:0;
        check(set_job(job,9,&limits,sizeof(limits)),"configure inheritance");
        check(handle_flags(job,1,1),"inheritable job handle");expected_job=job;
        if(spawn("--inherit",4,&first)){
            check(assign_job(job,first.process),"assign inheritance parent");
            close_handle(job); /* Membership survives closure without kill-on-close. */
            check(finish(&first,1)==0,"inheritance and breakaway contract");
        }else{check(0,"create inheritance parent");close_handle(job);}
        expected_job=0;
    }
    for(DWORD mode=0;mode<2;mode++){
        job=create_job(0,0);memset(&limits,0,sizeof(limits));
        limits.basic.flags=mode?0x200:0x100;
        if(mode)limits.job_memory=128*1024*1024;else limits.process_memory=128*1024*1024;
        check(set_job(job,9,&limits,sizeof(limits)),"configure memory limit");
        if(spawn("--memory",4,&first)){
            check(assign_job(job,first.process),"assign memory-test child");
            check(finish(&first,1)==0,"private commit and heap quota contract");
            check(query_job(job,9,&readback,sizeof(readback),0)&&readback.peak_process>=8*1024*1024&&
                  readback.peak_job>=readback.peak_process,"peak commitment survives process exit");
        }else check(0,"create memory-test child");
        close_handle(job);
    }

    job=create_job(0,0);memset(&limits,0,sizeof(limits));limits.basic.flags=0x2000;
    check(set_job(job,9,&limits,sizeof(limits)),"cross-owner kill-on-close");
    check(handle_flags(job,1,1),"inherit last job handle");expected_job=job;
    if(spawn("--close-job",4,&first)){
        check(assign_job(job,first.process),"assign final-handle owner");
        close_handle(job);expected_job=0;
        check(finish(&first,1)==0,"last handle closed by child terminates child");
    }else{check(0,"create final-handle owner");close_handle(job);expected_job=0;}

    job=create_job(0,0);
    if(spawn("--member",4,&first)&&spawn("--member",4,&second)){
        check(assign_job(job,first.process)&&assign_job(job,second.process),"assign termination group");
        check(terminate_job(job,0x44),"terminate group");
        check(finish(&first,0)==0x44&&finish(&second,0)==0x44,"all members receive termination status");
    }else check(0,"create termination group");
    close_handle(job);

    job=create_job(0,0);memset(&limits,0,sizeof(limits));limits.basic.flags=0x200;
    limits.job_memory=128*1024*1024;
    check(set_job(job,9,&limits,sizeof(limits)),"aggregate memory limit");
    handle_flags(job,1,1);expected_job=job;
    ready_event=create_event(0,1,0,0);release_event=create_event(0,1,0,0);
    handle_flags(ready_event,1,1);handle_flags(release_event,1,1);
    if(spawn("--hold-memory",4,&first)){
        check(assign_job(job,first.process),"assign memory holder");
        check(resume_thread(first.thread)==1,"start memory holder");
        check(wait_object(ready_event,30000)==0,"memory holder ready");
        if(spawn("--probe-memory",4,&second)){
            check(assign_job(job,second.process),"assign aggregate probe");
            check(finish(&second,1)==0,"aggregate limit includes both processes");
        }else check(0,"create aggregate probe");
        set_event(release_event);check(finish(&first,0)==0,"release holder");
    }else check(0,"create memory holder");
    close_handle(ready_event);close_handle(release_event);ready_event=release_event=0;
    if(spawn("--after-memory",4,&first)){
        check(assign_job(job,first.process),"assign after group exits");
        check(finish(&first,1)==0,"process exit returns aggregate quota");
    }else check(0,"create post-exit memory probe");
    close_handle(job);expected_job=0;
}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),n=LoadLibraryA("ntdll.dll");
#define GET(variable,name) variable=GetProcAddress(k,name);if(!variable){report("JOB-OBJECT MISSING " name "\n");ExitProcess(2);}
    GET(create_job,"CreateJobObjectW");GET(set_job,"SetInformationJobObject");
    GET(query_job,"QueryInformationJobObject");GET(assign_job,"AssignProcessToJobObject");
    GET(in_job,"IsProcessInJob");GET(terminate_job,"TerminateJobObject");
    GET(close_handle,"CloseHandle");GET(dup_handle,"DuplicateHandle");
    GET(handle_flags,"SetHandleInformation");GET(terminate_process,"TerminateProcess");
    GET(wait_object,"WaitForSingleObject");GET(exit_code,"GetExitCodeProcess");
    GET(create_process,"CreateProcessA");GET(resume_thread,"ResumeThread");
    GET(module_name,"GetModuleFileNameA");GET(command_line,"GetCommandLineA");
    GET(sleep_ms,"Sleep");GET(alloc,"VirtualAlloc");GET(free_vm,"VirtualFree");
    GET(get_heap,"GetProcessHeap");GET(heap_alloc,"HeapAlloc");
    GET(create_event,"CreateEventA");GET(set_event,"SetEvent");
    GET(raise_exception,"RaiseException");GET(set_error_mode,"SetErrorMode");
    GET(add_vectored,"AddVectoredExceptionHandler");
    GET(remove_vectored,"RemoveVectoredExceptionHandler");
    nt_close=GetProcAddress(n,"NtClose");if(!nt_close)ExitProcess(2);
    check(module_name(0,path,sizeof(path))!=0,"executable path");
    const char *cmd=command_line();
    for(const char *p=cmd;*p;p++)if((p[0]=='j'||p[0]=='r'||p[0]=='s')&&p[1]==':'){
        char key=p[0];SIZE value=0;p+=2;
        while((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')){
            value=value*16+(*p<='9'?*p-'0':*p-'a'+10);p++;
        }
        if(key=='j')expected_job=(HANDLE)value;
        else if(key=='r')ready_event=(HANDLE)value;
        else release_event=(HANDLE)value;
        if(!*p)break;
    }
    if(contains(cmd,"--fault-job")||contains(cmd,"--caught-job"))
        exception_child(contains(cmd,"--caught-job"));
    else if(contains(cmd,"--close-job")){
        check(nt_close(expected_job)==0,"close inherited final handle");
        for(;;)sleep_ms(1);
    }
    else if(contains(cmd,"--hold-memory")||contains(cmd,"--after-memory")){
        void *p=alloc(0,80*1024*1024,0x3000,4);check(p!=0,"holder commits below group limit");
        if(ready_event){set_event(ready_event);check(wait_object(release_event,30000)==0,"holder released");}
        /* Deliberately leave memory to process teardown. */
    }
    else if(contains(cmd,"--probe-memory")){
        check(!alloc(0,80*1024*1024,0x3000,4),"combined commitments exceed job limit");
        void *p=alloc(0,8*1024*1024,0x3000,4);check(p!=0,"small commit still fits job limit");
        if(p)free_vm(p,0,0x8000);
    }
    else if(contains(cmd,"--memory"))memory_child();
    else if(contains(cmd,"--inherit")||contains(cmd,"--deny-spawn"))inheritance_child(cmd);
    else if(contains(cmd,"--member"))expect_member(SELF,expected_job,1,"grandchild inherits job");
    else if(contains(cmd,"--outside"))expect_member(SELF,expected_job,0,"grandchild is outside job");
    else{contract();report(failures?"JOB-OBJECT FAILED\n":"JOB-OBJECT PASS\n");}
    ExitProcess(failures?1:0);
}
