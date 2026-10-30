/* Run unchanged on Windows and Osito-K; no CRT or platform test hooks. */
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
API HANDLE CALL GetModuleHandleA(const char *);
void *memcpy(void *out,const void *in,SIZE n)
{for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=((const BYTE *)in)[i];return out;}
void *memset(void *out,int c,SIZE n)
{for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=(BYTE)c;return out;}

#ifdef DEPENDENCY
static DWORD attached;
__declspec(dllexport) DWORD CALL DependencyReady(void) {return attached;}
BOOL CALL DllMain(HANDLE module,DWORD reason,void *reserved)
{
    (void)module;(void)reserved;
    if(reason==1){
        void (CALL *notify)(void)=GetProcAddress(GetModuleHandleA(0),"NotifyAttach");
        if(!notify)return 0;
        notify();attached=1;
    }
    return 1;
}
#else
API DWORD CALL DependencyReady(void);
static volatile DWORD marker=0x13572468,entered,attached,tls_seen,tls_marker;
static const volatile DWORD readonly_marker=0x98765432;
static DWORD failures;
__declspec(dllexport) void CALL NotifyAttach(void) {attached++;}
static void CALL tls_callback(HANDLE module,DWORD reason,void *reserved)
{(void)module;(void)reserved;if(reason==1){tls_seen++;tls_marker=marker;}}
static DWORD tls_index;
static BYTE tls_data;
static void (CALL *callbacks[])(HANDLE,DWORD,void *)={tls_callback,0};
__declspec(allocate(".rdata$T")) const struct {
    const void *start,*end;DWORD *index;void *callbacks;DWORD zero,flags;
} _tls_used={&tls_data,&tls_data,&tls_index,callbacks,0,0};
typedef struct {
    DWORD cb;WORD *reserved,*desktop,*title;
    DWORD x,y,w,h,xc,yc,fill,flags;WORD show,bytes;BYTE *data;
    HANDLE input,output,error;
} STARTUP;
typedef struct {HANDLE process,thread;DWORD pid,tid;} PROCESS;
typedef struct {void *base,*allocation;DWORD allocation_protect;SIZE size;DWORD state,protect,type;} MEMORY;
typedef struct {long status;void *peb;UPTR affinity;long priority;UPTR pid,parent;} BASIC;
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static DWORD (CALL *resume_thread)(HANDLE);
static DWORD (CALL *wait_object)(HANDLE,DWORD);
static BOOL (CALL *close_handle)(HANDLE);
static BOOL (CALL *terminate_process)(HANDLE,DWORD);
static BOOL (CALL *exit_code)(HANDLE,DWORD *);
static BOOL (CALL *read_memory)(HANDLE,const void *,void *,SIZE,SIZE *);
static BOOL (CALL *write_memory)(HANDLE,void *,const void *,SIZE,SIZE *);
static SIZE (CALL *query_memory)(HANDLE,const void *,void *,SIZE);
static BOOL (CALL *protect_memory)(HANDLE,void *,SIZE,DWORD,DWORD *);
static long (CALL *query_process)(HANDLE,DWORD,void *,DWORD,DWORD *);
static DWORD (CALL *status_error)(long);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static char *(CALL *command_line)(void);
static void (CALL *sleep_ms)(DWORD);
static HANDLE (CALL *create_file)(const char *,DWORD,DWORD,void *,DWORD,DWORD,HANDLE);
static BOOL (CALL *delete_file)(const char *);
static BOOL (CALL *copy_file)(const char *,const char *,BOOL);
static DWORD (CALL *seek_file)(HANDLE,long,long *,DWORD);
static char path[320];
static BYTE *own_base;
static BYTE *nt_base,*nt_open_thread;
static void report(const char *s)
{DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void check(BOOL ok,const char *s)
{if(!ok){failures++;report("SUSPENDED-IMAGE FAIL: ");report(s);report("\n");}}
static void hex(DWORD value)
{char s[9];for(DWORD i=0;i<8;i++)s[i]="0123456789abcdef"[(value>>(28-i*4))&15];s[8]=0;report(s);}
static BOOL contains(const char *s,const char *word)
{for(;*s;s++){DWORD i=0;while(word[i]&&s[i]==word[i])i++;if(!word[i])return 1;}return 0;}
static void *remote(BYTE *base,const volatile void *local)
{return base+((const BYTE *)local-own_base);}
static DWORD read_word(PROCESS *pi,BYTE *base,const volatile void *local)
{
    DWORD value=0xffffffff;SIZE got=0;
    check(read_memory(pi->process,remote(base,local),&value,4,&got)&&got==4,"read suspended global");
    return value;
}
static BOOL launch(PROCESS *pi)
{
    STARTUP startup={0};startup.cb=sizeof(startup);
    char command[]="suspended-image --child";
    return create_process(path,command,0,0,0,4,0,0,&startup,pi);
}
static void finish(PROCESS *pi,DWORD expected)
{
    DWORD code=0xffffffff,wait=wait_object(pi->process,20000);
    if(wait!=0){terminate_process(pi->process,0xee);wait_object(pi->process,5000);}
    check(wait==0&&exit_code(pi->process,&code)&&code==expected,"child exit status");
    close_handle(pi->thread);close_handle(pi->process);
}
static void check_suspended_ntdll(PROCESS *pi)
{
    BYTE header[128]={0},code[32]={0};SIZE got=0;
    BOOL present=read_memory(pi->process,nt_base,header,sizeof(header),&got)&&got==sizeof(header);
    check(present,"NTDLL mapped at common ABI base before resume");
    if(!present)return;
    check(*(WORD *)header==0x5a4d,"suspended NTDLL image header");
    check(read_memory(pi->process,nt_open_thread,code,sizeof(code),&got)&&got==sizeof(code),
          "NT service entry readable before resume");
    BOOL same=1;for(SIZE i=0;i<sizeof(code);i++)if(code[i]!=nt_open_thread[i])same=0;
    check(same,"NT service entry matches parent before resume");
    MEMORY memory={0};
    check(query_memory(pi->process,nt_open_thread,&memory,sizeof(memory))&&
          memory.allocation==nt_base&&memory.protect==0x20&&memory.type==0x1000000,
          "suspended NTDLL service is RX image memory");
    /* DOS reserved words are not executable and survive normal loader setup.
     * Patching only the child must not modify the parent's system image. */
    DWORD original=*(DWORD *)(nt_base+0x28),value=0x514e5444,old=0,ignored=0;
    BOOL writable=protect_memory(pi->process,nt_base+0x28,4,4,&old);
    check(writable&&old==2,"unprotect suspended NTDLL header");
    if(writable){
        check(write_memory(pi->process,nt_base+0x28,&value,4,&got)&&got==4,
              "patch suspended NTDLL private page");
        check(protect_memory(pi->process,nt_base+0x28,4,old,&ignored),"restore NTDLL header protection");
    }
    check(*(DWORD *)(nt_base+0x28)==original,"parent NTDLL remains private");
}
static void check_ntdll_address_conflict(void)
{
    char conflict[352];DWORD length=0;
    while(path[length]){conflict[length]=path[length];length++;}
    memcpy(conflict+length,".ntdll-conflict.exe",19);
    BOOL copied=copy_file(path,conflict,1);
    check(copied,"copy core-image address-conflict fixture");
    if(!copied)return;
    HANDLE file=create_file(conflict,0x40000000,0,0,3,0x80,0);
    BOOL patched=0;
    check(file!=(HANDLE)(UPTR)-1,"open core-image address-conflict fixture");
    if(file!=(HANDLE)(UPTR)-1){
        /* The fixture is /fixed. Change only its preferred base, then keep
         * creation suspended so no code could run with unrelocated pointers. */
        DWORD offset=*(DWORD *)(own_base+60)+24+(sizeof(void *)==8?24:28),written=0;
        patched=seek_file(file,(long)offset,0,0)==offset&&
                WriteFile(file,&nt_base,sizeof(nt_base),&written,0)&&written==sizeof(nt_base);
        check(patched,"write conflicting preferred image base");
        close_handle(file);
    }
    if(patched){
        for(DWORD i=0;i<3;i++){
            PROCESS pi={0};STARTUP startup={0};startup.cb=sizeof(startup);
            BOOL created=create_process(conflict,0,0,0,0,4,0,0,&startup,&pi);
            DWORD error=GetLastError();
            report("NTDLL conflict result/error=");hex(created);report("/");hex(error);report("\n");
            /* WOW64 reports 308 for this fixed x86 image; native x64 and
             * OsitoK's non-WOW64 loader report ERROR_INVALID_ADDRESS. */
            check(!created&&(error==487||(sizeof(void *)==4&&error==308)),
                  "core image address collision fails creation");
            if(created){terminate_process(pi.process,77);finish(&pi,77);}
        }
    }
    check(delete_file(conflict),"delete core-image address-conflict fixture");
}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),n=LoadLibraryA("ntdll.dll");
    nt_base=n;nt_open_thread=GetProcAddress(n,"NtOpenThread");
    check(nt_base&&nt_open_thread,"NTDLL service exported");
    if(!nt_base||!nt_open_thread)ExitProcess(2);
#define LOAD(module,var,name) var=GetProcAddress(module,name);if(!var){report("SUSPENDED-IMAGE MISSING " name "\n");ExitProcess(2);}
    LOAD(k,create_process,"CreateProcessA");LOAD(k,resume_thread,"ResumeThread");
    LOAD(k,wait_object,"WaitForSingleObject");LOAD(k,close_handle,"CloseHandle");
    LOAD(k,terminate_process,"TerminateProcess");LOAD(k,exit_code,"GetExitCodeProcess");
    LOAD(k,read_memory,"ReadProcessMemory");LOAD(k,write_memory,"WriteProcessMemory");
    LOAD(k,query_memory,"VirtualQueryEx");LOAD(k,protect_memory,"VirtualProtectEx");
    LOAD(n,query_process,"NtQueryInformationProcess");LOAD(k,module_name,"GetModuleFileNameA");
    LOAD(n,status_error,"RtlNtStatusToDosError");
    LOAD(k,command_line,"GetCommandLineA");LOAD(k,sleep_ms,"Sleep");
    LOAD(k,create_file,"CreateFileA");LOAD(k,delete_file,"DeleteFileA");
    LOAD(k,copy_file,"CopyFileA");LOAD(k,seek_file,"SetFilePointer");
#undef LOAD
    entered++;
    if(contains(command_line(),"--child")){
        check(*(DWORD *)(nt_base+0x28)==0x514e5444,"NTDLL patch survives loader initialization");
        BASIC basic={0};DWORD length=0;
        check(query_process((HANDLE)(UPTR)-1,0,&basic,sizeof(basic),&length)==0&&basic.peb,
              "bootstrapped NTDLL remains callable after resume");
        check(marker==0x24681357,"parent write survives loader initialization");
        check(readonly_marker==0x12345678,"read-only patch survives initialization");
        check(tls_seen==1&&tls_marker==0x24681357,"TLS ran once after parent patch");
        check(attached==1&&DependencyReady()==1,"dependency attached after resume");
        report(failures?"SUSPENDED-IMAGE CHILD FAILED\n":"SUSPENDED-IMAGE CHILD PASS\n");
        ExitProcess(failures?1:0);
    }
    own_base=GetModuleHandleA(0);module_name(0,path,sizeof(path));
    check(status_error((long)0xc000007b)==193,"invalid-image status maps to bad executable format");
    check_ntdll_address_conflict();
    for(DWORD iteration=0;iteration<8;iteration++){
        PROCESS pi={0};BOOL ok=launch(&pi);check(ok,"create suspended process");if(!ok)continue;
        BASIC info={0};DWORD size=0;BYTE *base=0;SIZE got=0;
        check(query_process(pi.process,0,&info,sizeof(info),&size)==0&&size==sizeof(info),"query target PEB");
        check(info.peb&&read_memory(pi.process,(BYTE *)info.peb+sizeof(void *)*2,&base,sizeof(base),&got)&&
              got==sizeof(base)&&base,"mapped image exists before resume");
        if(!base){terminate_process(pi.process,77);finish(&pi,77);continue;}
        WORD magic=0;
        check(read_memory(pi.process,base,&magic,2,&got)&&magic==0x5a4d,"target DOS signature");
        sleep_ms(30);
        check(read_word(&pi,base,&marker)==0x13572468,"initial target data");
        check(read_word(&pi,base,&entered)==0,"entry has not run");
        check(read_word(&pi,base,&tls_seen)==0,"TLS has not run");
        check(read_word(&pi,base,&attached)==0,"DllMain has not run");
        check_suspended_ntdll(&pi);
        MEMORY memory={0};
        check(query_memory(pi.process,base,&memory,sizeof(memory))&&memory.protect==2,"headers read-only before resume");
        check(query_memory(pi.process,remote(base,mainCRTStartup),&memory,sizeof(memory))&&
              memory.protect==0x20,"code RX before resume");
        DWORD value=0x24681357,old=0,ignored=0;
        check(write_memory(pi.process,remote(base,&marker),&value,4,&got)&&got==4,"write suspended data");
        check(marker==0x13572468,"parent image remains private");
        check(protect_memory(pi.process,remote(base,&readonly_marker),4,4,&old)&&old==2,"unprotect target constant");
        value=0x12345678;
        check(write_memory(pi.process,remote(base,&readonly_marker),&value,4,&got)&&got==4,"patch target constant");
        check(protect_memory(pi.process,remote(base,&readonly_marker),4,old,&ignored),"restore target permissions");
        if(iteration&1){check(terminate_process(pi.process,77),"terminate before resume");finish(&pi,77);}
        else{check(resume_thread(pi.thread)==1,"initial suspend count");finish(&pi,0);}
    }
    char bad_path[336];DWORD length=0;
    while(path[length]){bad_path[length]=path[length];length++;}
    memcpy(bad_path+length,".invalid.exe",13);
    HANDLE file=create_file(bad_path,0x40000000,0,0,2,0x80,0);
    check(file!=(HANDLE)(UPTR)-1,"create invalid-image fixture");
    if(file!=(HANDLE)(UPTR)-1){
        BYTE header[64]={0x4d,0x5a};DWORD written=0;
        check(WriteFile(file,header,sizeof(header),&written,0)&&written==sizeof(header),"write invalid-image fixture");
        close_handle(file);
        for(DWORD i=0;i<4;i++){
            PROCESS pi={0};STARTUP startup={0};startup.cb=sizeof(startup);
            BOOL created=create_process(bad_path,0,0,0,0,4,0,0,&startup,&pi);
            DWORD error=GetLastError();
            report("Invalid image result/error=");hex(created);report("/");hex(error);report("\n");
            /* Windows x64 classifies this MZ-only image as unsupported DOS;
             * Osito-K rejects it as a malformed PE. Both must fail creation. */
            check(!created&&(error==193||error==216),"invalid image fails creation synchronously");
            if(created){terminate_process(pi.process,77);finish(&pi,77);}
        }
        check(delete_file(bad_path),"delete invalid-image fixture");
    }
    report(failures?"SUSPENDED-IMAGE FAILED\n":"SUSPENDED-IMAGE PASS\n");
    ExitProcess(failures?1:0);
}
#endif
