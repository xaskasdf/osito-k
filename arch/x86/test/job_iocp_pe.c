/* Same no-CRT job/IOCP contract on Windows and OsitoK. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef void *HANDLE;
typedef __SIZE_TYPE__ SIZE;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define SELF ((HANDLE)(UPTR)-1)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
void *memcpy(void *out,const void *in,SIZE n)
{for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=((const BYTE *)in)[i];return out;}
void *memset(void *out,int c,SIZE n)
{for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=(BYTE)c;return out;}
typedef struct {void *key;HANDLE port;} ASSOCIATION;
typedef struct {DWORD cb;char *reserved,*desktop,*title;DWORD x,y,w,h,xc,yc,fill,flags;
    WORD show,bytes;BYTE *data;HANDLE input,output,error;} STARTUP;
typedef struct {HANDLE process,thread;DWORD pid,tid;} PROCESS;
static HANDLE (CALL *create_job)(void *,const WORD *);
static BOOL (CALL *set_job)(HANDLE,DWORD,const void *,DWORD);
static BOOL (CALL *query_job)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *assign_job)(HANDLE,HANDLE);
static BOOL (CALL *close_handle)(HANDLE);
static BOOL (CALL *terminate_process)(HANDLE,DWORD);
static DWORD (CALL *nt_close)(HANDLE);
static BOOL (CALL *dup_handle)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static DWORD (CALL *wait_object)(HANDLE,DWORD);
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static HANDLE (CALL *create_port)(HANDLE,HANDLE,UPTR,DWORD);
static BOOL (CALL *get_packet)(HANDLE,DWORD *,UPTR *,void **,DWORD);
static BOOL (CALL *post_packet)(HANDLE,DWORD,UPTR,void *);
static HANDLE (CALL *create_event)(void *,BOOL,BOOL,const char *);
static void *(CALL *alloc_ex)(HANDLE,void *,SIZE,DWORD,DWORD);
static BOOL (CALL *free_ex)(HANDLE,void *,SIZE,DWORD);
static DWORD failures;
static void report(const char *s)
{DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void hex(UPTR v)
{char s[17];DWORD n=sizeof(v)*2;for(DWORD i=n;i;i--){s[i-1]="0123456789abcdef"[v&15];v>>=4;}s[n]=0;report(s);}
static void check(BOOL ok,const char *label)
{if(!ok){failures++;report("JOB-IOCP FAIL: ");report(label);report(" error=");hex(GetLastError());report("\n");}}
static BOOL probe(const char *label,HANDLE job,const void *data,DWORD size,DWORD expected)
{
    SetLastError(0x13572468);BOOL ok=set_job(job,7,data,size);DWORD error=GetLastError();
    report("JOB-IOCP ");report(label);report(" result/error=");hex(ok);report("/");hex(error);report("\n");
    check(expected?(!ok&&error==expected):(ok&&error==0x13572468),label);
    return ok;
}
static void packet(HANDLE port,DWORD expected,UPTR key,DWORD pid)
{
    DWORD message=0;UPTR actual_key=0;void *value=0;
    BOOL ok=get_packet(port,&message,&actual_key,&value,5000);
    report("JOB-IOCP packet result/message/key/pid=");hex(ok);report("/");hex(message);
    report("/");hex(actual_key);report("/");hex((UPTR)value);report("\n");
    check(ok&&message==expected&&actual_key==key&&(UPTR)value==pid,"job notification payload");
}
static void empty(HANDLE port)
{
    DWORD message=0;UPTR key=0;void *value=(void *)(UPTR)0xaaaaaaaa;
    BOOL ok=get_packet(port,&message,&key,&value,0);
    check(!ok&&GetLastError()==258&&!value,"completion queue empty");
}
static PROCESS spawn(void)
{
    char path[320],cmd[]="jobiocp --suspended";module_name(0,path,sizeof(path));
    STARTUP startup={0};startup.cb=sizeof(startup);PROCESS pi={0};
    check(create_process(path,cmd,0,0,0,4,0,0,&startup,&pi),"create suspended child");
    return pi;
}
static void finish(PROCESS *pi,DWORD code)
{
    if(wait_object(pi->process,0)!=0)
        check(terminate_process(pi->process,code),"terminate child");
    check(wait_object(pi->process,5000)==0,"child exits");
    check(close_handle(pi->thread)&&close_handle(pi->process),"close child handles");
}
static void limit_notifications(void)
{
    for(DWORD kind=0;kind<3;kind++){
        HANDLE job=create_job(0,0),port=create_port(SELF,0,0,1);
        ASSOCIATION a={(void *)(UPTR)(0x71+kind),port};
        check(set_job(job,7,&a,sizeof(a)),"associate limit notifications");
        _Alignas(8) BYTE limits[144]={0};
        *(DWORD *)(limits+16)=kind==0?8:kind==1?0x100:0x200;
        DWORD cls=kind?9:2,size=sizeof(void *)==4?(kind?112:48):(kind?144:64);
        DWORD offset=sizeof(void *)==4?(kind==1?96:100):(kind==1?112:120);
        if(kind){UPTR cap=64*1024*1024;memcpy(limits+offset,&cap,sizeof(cap));}
        check(set_job(job,cls,limits,size),"configure notification limit");
        PROCESS pi=spawn();if(!pi.process)break;
        if(!kind){
            check(!assign_job(job,pi.process)&&GetLastError()==1816,"active count limit rejects child");
            packet(port,3,(UPTR)a.key,0);
            /* A rejected suspended child need not signal before explicit cleanup. */
            finish(&pi,0);
        }else{
            check(assign_job(job,pi.process),"assign memory-limited child");packet(port,6,(UPTR)a.key,pi.pid);
            void *memory=alloc_ex(pi.process,0,128*1024*1024,0x3000,4);
            check(!memory,"remote commit exceeds job memory limit");
            if(memory)free_ex(pi.process,memory,0,0x8000);
            packet(port,kind==1?9:10,(UPTR)a.key,pi.pid);
            DWORD pid=pi.pid;finish(&pi,0);packet(port,7,(UPTR)a.key,pid);packet(port,4,(UPTR)a.key,0);
        }
        empty(port);close_handle(job);close_handle(port);
    }
}
static void shared_port(void)
{
    HANDLE port=create_port(SELF,0,0,1),jobs[2]={create_job(0,0),create_job(0,0)};
    PROCESS children[2];
    for(DWORD i=0;i<2;i++){
        ASSOCIATION a={(void *)(UPTR)(0x81+i),port};
        check(set_job(jobs[i],7,&a,sizeof(a)),"multiple jobs share one port");
        children[i]=spawn();check(assign_job(jobs[i],children[i].process),"shared port assignment");
        packet(port,6,(UPTR)a.key,children[i].pid);
    }
    for(DWORD i=0;i<2;i++){
        DWORD pid=children[i].pid;finish(&children[i],1);
        packet(port,7,0x81+i,pid);packet(port,4,0x81+i,0);close_handle(jobs[i]);
    }
    empty(port);close_handle(port);
}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),n=LoadLibraryA("ntdll.dll");
#define LOAD(mod,var,name) var=GetProcAddress(mod,name);if(!var){report("JOB-IOCP MISSING " name "\n");ExitProcess(2);}
    LOAD(k,create_job,"CreateJobObjectW");LOAD(k,set_job,"SetInformationJobObject");
    LOAD(k,query_job,"QueryInformationJobObject");LOAD(k,assign_job,"AssignProcessToJobObject");
    LOAD(k,close_handle,"CloseHandle");LOAD(k,terminate_process,"TerminateProcess");
    LOAD(k,dup_handle,"DuplicateHandle");LOAD(k,wait_object,"WaitForSingleObject");
    LOAD(k,create_process,"CreateProcessA");LOAD(k,module_name,"GetModuleFileNameA");
    LOAD(k,create_port,"CreateIoCompletionPort");LOAD(k,get_packet,"GetQueuedCompletionStatus");
    LOAD(k,post_packet,"PostQueuedCompletionStatus");LOAD(k,create_event,"CreateEventA");
    LOAD(k,alloc_ex,"VirtualAllocEx");LOAD(k,free_ex,"VirtualFreeEx");
    LOAD(n,nt_close,"NtClose");
#undef LOAD
    HANDLE job=create_job(0,0),port=create_port(SELF,0,0,1),other=create_port(SELF,0,0,1);
    check(job&&port&&other,"create job and ports");
    UPTR key=sizeof(UPTR)==8?(UPTR)0x123456789abcdef0ULL:0x9abcdef0U;
    ASSOCIATION a={(void *)key,port},zero={0,0};
    if(!probe("associate-empty",job,&a,sizeof(a),0)){check(0,"associate empty job");ExitProcess(2);}
    empty(port);
    probe("repeat-same",job,&a,sizeof(a),87);
    ASSOCIATION b={(void *)(key+1),other};probe("replace-port",job,&b,sizeof(b),87);
    probe("detach",job,&zero,sizeof(zero),0);
    probe("detach-again",job,&zero,sizeof(zero),0);
    probe("short-buffer",job,&a,sizeof(a)-1,24);
    probe("long-buffer",job,&a,sizeof(a)+1,sizeof(HANDLE)==4?0:24);
    probe("null-buffer",job,0,sizeof(a),998);
    probe("bad-buffer",job,(void *)(UPTR)1,sizeof(a),998);
    ASSOCIATION bad={0,(HANDLE)(UPTR)0x12345678};probe("invalid-port",job,&bad,sizeof(bad),6);
    HANDLE event=create_event(0,1,0,0);bad.port=event;probe("event-not-port",job,&bad,sizeof(bad),6);
    close_handle(event);
    HANDLE restricted=0;check(dup_handle(SELF,job,SELF,&restricted,4,0,0),"duplicate query-only job");
    probe("job-rights",restricted,&a,sizeof(a),5);close_handle(restricted);
    for(DWORD access=1;access<=2;access++){
        check(dup_handle(SELF,port,SELF,&restricted,access,0,0),"duplicate restricted port");
        bad.port=restricted;probe(access==1?"port-query-right":"port-modify-right",job,&bad,sizeof(bad),access==1?5:0);
        probe("detach-restricted",job,&zero,sizeof(zero),0);close_handle(restricted);
    }
    ASSOCIATION queried={(void *)(UPTR)0xaaaaaaaa,(HANDLE)(UPTR)0xbbbbbbbb};DWORD returned=0xcccccccc;
    SetLastError(0);BOOL queried_ok=query_job(job,7,&queried,sizeof(queried),&returned);
    report("JOB-IOCP query-association result/error=");hex(queried_ok);report("/");hex(GetLastError());report("\n");
    check(!queried_ok&&GetLastError()==87,"association is not queryable");
    check(probe("associate-lifecycle",job,&a,sizeof(a),0),"associate for lifecycle");
    for(DWORD i=0;i<3;i++){
        PROCESS pi=spawn();if(!pi.process)break;
        check(assign_job(job,pi.process),"assign child to associated job");packet(port,6,key,pi.pid);
        DWORD pid=pi.pid;finish(&pi,i==1?0xc0000005:73);
        packet(port,i==1?8:7,key,pid);packet(port,4,key,0);empty(port);
    }
    check(probe("detach-before-late",job,&zero,sizeof(zero),0),"detach before late association");
    PROCESS late=spawn();check(assign_job(job,late.process),"assign before port");empty(port);
    check(probe("late-association",job,&a,sizeof(a),0),"associate populated job");packet(port,6,key,late.pid);
    HANDLE alias=0;check(dup_handle(SELF,port,SELF,&alias,0,0,2),"duplicate live port");
    check(nt_close(port)==0,"close original port through NT");port=0;
    HANDLE reused=create_port(SELF,0,0,1);check(reused!=0,"allocate another port after close");
    DWORD late_pid=late.pid;finish(&late,0);packet(alias,7,key,late_pid);packet(alias,4,key,0);empty(reused);
    check(post_packet(alias,123,key,0),"post through surviving alias");packet(alias,123,key,0);
    close_handle(alias);close_handle(reused);close_handle(other);
    check(probe("detach-closed-port",job,&zero,sizeof(zero),0),"detach after final caller handle closes");
    close_handle(job);
    shared_port();limit_notifications();
    for(DWORD i=0;i<72;i++){
        job=create_job(0,0);port=create_port(SELF,0,0,1);a.port=port;
        check(job&&port&&set_job(job,7,&a,sizeof(a)),"association slots reusable");
        close_handle(port);close_handle(job);
    }
    report(failures?"JOB-IOCP FAILED\n":"JOB-IOCP PASS\n");ExitProcess(failures?1:0);
}
