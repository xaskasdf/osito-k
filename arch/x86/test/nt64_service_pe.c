/* The same service-copy/interception contract runs on Windows and OsitoK. */
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef int LONG;
typedef int BOOL;
typedef __UINTPTR_TYPE__ UPTR;
typedef __SIZE_TYPE__ SIZE;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define SELF ((HANDLE)(UPTR)-1)
API void ExitProcess(DWORD);
API HANDLE GetStdHandle(DWORD);
API BOOL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE LoadLibraryA(const char *);
API void *GetProcAddress(HANDLE,const char *);
API DWORD GetLastError(void);
API HANDLE GetModuleHandleA(const char *);
void *memcpy(void *out,const void *in,SIZE n)
{for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=((const BYTE *)in)[i];return out;}
void *memset(void *out,int c,SIZE n)
{for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=(BYTE)c;return out;}
typedef struct {LONG status;void *peb;UPTR affinity;LONG priority;UPTR pid,parent;} BASIC;
typedef struct {WORD length,maximum;WORD *buffer;} STRING;
typedef struct {DWORD size;HANDLE root;STRING *name;DWORD flags;void *security,*qos;} ATTR;
typedef struct {UPTR status,information;} IO;
typedef struct {DWORD cb;WORD *reserved,*desktop,*title;DWORD x,y,w,h,xc,yc,fill,flags;
    WORD show,bytes;BYTE *data;HANDLE input,output,error;} STARTUP;
typedef struct {HANDLE process,thread;DWORD pid,tid;} PROCESS;
typedef LONG (*QUERY)(HANDLE,DWORD,void *,DWORD,DWORD *);
static QUERY query,original_query;
static LONG (*delay)(BOOL,const long long *);
static LONG (*allocate)(HANDLE,void **,UPTR,SIZE *,DWORD,DWORD);
static LONG (*release)(HANDLE,void **,SIZE *,DWORD);
static void *(*valloc)(void *,SIZE,DWORD,DWORD);
static BOOL (*vfree)(void *,SIZE,DWORD);
static BOOL (*protect)(HANDLE,void *,SIZE,DWORD,DWORD *);
static BOOL (*write_memory)(HANDLE,void *,const void *,SIZE,SIZE *);
static BOOL (*read_memory)(HANDLE,const void *,void *,SIZE,SIZE *);
static BOOL (*flush)(HANDLE,const void *,SIZE);
static DWORD (*wait)(HANDLE,DWORD);
static BOOL (*close_handle)(HANDLE);
static BOOL (*exit_code)(HANDLE,DWORD *);
static BOOL (*terminate)(HANDLE,DWORD);
static DWORD (*get_pid)(void);
static DWORD failures;
static volatile LONG hook_calls;
extern BOOL nt64_register_probe(void *service);
extern LONG nt64_stack_probe(void *service,void *stack_end,void *out);
static void report(const char *s)
{DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void check(BOOL ok,const char *s)
{if(!ok){failures++;report("NT64-SERVICE FAIL: ");report(s);report("\n");}}
static BOOL contains(const char *s,const char *word)
{for(;*s;s++){DWORD i=0;while(word[i]&&s[i]==word[i])i++;if(!word[i])return 1;}return 0;}
static void *copy_service(void *entry)
{
    BYTE *s=entry;
    BOOL normal=s&&*(DWORD *)s==0xb8d18b4c&&s[8]==0x0f&&s[9]==5&&s[10]==0xc3;
    BOOL fallback=s&&*(DWORD *)s==0xb8d18b4c&&s[8]==0xf6&&s[18]==0x0f&&s[19]==5;
    check(normal||fallback,"position-independent NT service instruction contract");
    if(!normal&&!fallback)return 0;
    BYTE *copy=valloc(0,4096,0x3000,4);DWORD old=0;
    check(copy!=0,"allocate service copy");if(!copy)return 0;
    memcpy(copy,entry,32);
    check(protect(SELF,copy,4096,0x20,&old)&&flush(SELF,copy,32),"publish RX service copy");
    return copy;
}
static LONG intercept(HANDLE process,DWORD cls,void *out,DWORD size,DWORD *returned)
{
    hook_calls++;
    return original_query(process,cls,out,size,returned);
}
static DWORD worker(void *unused)
{
    (void)unused;
    for(DWORD i=0;i<32;i++){
        BASIC b={0};DWORD bytes=0;long long interval=-10000;
        if(query(SELF,0,&b,sizeof(b),&bytes)!=0||b.pid!=get_pid()||
           bytes!=sizeof(b)||delay(0,&interval)!=0)return 1;
    }
    return 0;
}
static void test_file(HANDLE k,HANDLE nt)
{
    DWORD (*filename)(HANDLE,WORD *,DWORD)=GetProcAddress(k,"GetModuleFileNameW");
    BOOL (*unlink)(const WORD *)=GetProcAddress(k,"DeleteFileW");
    LONG (*create)(HANDLE *,DWORD,ATTR *,IO *,const long long *,DWORD,DWORD,DWORD,DWORD,void *,DWORD)=
        copy_service(GetProcAddress(nt,"NtCreateFile"));
    LONG (*write)(HANDLE,HANDLE,void *,void *,IO *,void *,DWORD,const long long *,void *)=
        copy_service(GetProcAddress(nt,"NtWriteFile"));
    LONG (*read)(HANDLE,HANDLE,void *,void *,IO *,void *,DWORD,const long long *,void *)=
        copy_service(GetProcAddress(nt,"NtReadFile"));
    LONG (*ntclose)(HANDLE)=copy_service(GetProcAddress(nt,"NtClose"));
    if(!create||!write||!read||!ntclose)goto done;
    WORD path[360]={'\\','\?','\?','\\'};DWORD length=filename(0,path+4,320);
    check(length&&length<320,"file-test executable path");if(!length||length>=320)goto done;
    const char suffix[]=".nt64.tmp";
    for(DWORD i=0;i<sizeof(suffix);i++)path[4+length+i]=(BYTE)suffix[i];
    length+=4+sizeof(suffix)-1;
    STRING name={(WORD)(length*2),(WORD)((length+1)*2),path};
    ATTR attributes={sizeof(attributes),0,&name,0x40,0,0};IO io={0};HANDLE file=0;
    LONG status=create(&file,0xc0100000,&attributes,&io,0,0x80,1,2,0x60,0,0);
    check(status==0&&file,"eleven-argument copied NtCreateFile");
    if(status==0&&file){
        DWORD sent=0x13572468,received=0;long long offset=0;
        check(write(file,0,0,0,&io,&sent,sizeof(sent),&offset,0)==0&&io.information==sizeof(sent),
              "nine-argument copied NtWriteFile");
        check(read(file,0,0,0,&io,&received,sizeof(received),&offset,0)==0&&
              io.information==sizeof(received)&&received==sent,"nine-argument copied NtReadFile");
        check(ntclose(file)==0,"copied NtClose");
        check(unlink(path+4),"delete service file fixture");
    }
done:
    if(create)vfree(create,0,0x8000);if(write)vfree(write,0,0x8000);
    if(read)vfree(read,0,0x8000);if(ntclose)vfree(ntclose,0,0x8000);
}
static void test_interception(HANDLE k,void *nt_query)
{
    BOOL (*create)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *)=
        GetProcAddress(k,"CreateProcessA");
    DWORD (*filename)(HANDLE,char *,DWORD)=GetProcAddress(k,"GetModuleFileNameA");
    void *(*remote_alloc)(HANDLE,void *,SIZE,DWORD,DWORD)=GetProcAddress(k,"VirtualAllocEx");
    DWORD (*resume)(HANDLE)=GetProcAddress(k,"ResumeThread");
    char path[320],command[]="nt64-service --child";filename(0,path,sizeof(path));
    PROCESS p={0};STARTUP startup={0};startup.cb=sizeof(startup);
    BOOL created=create(path,command,0,0,0,4,0,0,&startup,&p);
    check(created,"create suspended interception target");if(!created)return;
    BYTE code[32];SIZE got=0;DWORD old=0,ignored=0;
    void *copy=remote_alloc(p.process,0,4096,0x3000,4);
    BOOL ok=copy&&read_memory(p.process,nt_query,code,32,&got)&&got==32&&
        write_memory(p.process,copy,code,32,&got)&&got==32&&
        protect(p.process,copy,4096,0x20,&old)&&flush(p.process,copy,32)&&
        write_memory(p.process,&original_query,&copy,sizeof(copy),&got)&&got==sizeof(copy);
    /* /fixed gives this fixture's child the same code/data addresses. Only
     * the child's NTDLL entry is redirected; the original lives separately. */
    BYTE patch[12]={0x48,0xb8};UPTR destination=(UPTR)intercept;
    memcpy(patch+2,&destination,8);patch[10]=0xff;patch[11]=0xe0;
    if(ok)ok=protect(p.process,nt_query,12,0x80,&old);
    if(ok){
        BOOL wrote=write_memory(p.process,nt_query,patch,12,&got)&&got==12;
        BOOL restored=protect(p.process,nt_query,12,old,&ignored);
        ok=wrote&&restored&&flush(p.process,nt_query,12);
    }
    check(ok,"remote original copy and protected NTDLL interception");
    check(*(BYTE *)nt_query==0x4c,"parent NTDLL entry not patched");
    if(ok)check(resume(p.thread)==1,"resume intercepted child");
    else terminate(p.process,77);
    DWORD finished=wait(p.process,20000),status=99;
    if(finished!=0){terminate(p.process,78);wait(p.process,5000);}
    check(finished==0&&exit_code(p.process,&status)&&status==(ok?0:77),"intercepted child exit");
    close_handle(p.thread);close_handle(p.process);
}
void mainCRTStartup(void)
{
    report("NT64-SERVICE start\n");
    HANDLE k=LoadLibraryA("kernel32.dll"),nt=LoadLibraryA("ntdll.dll");
#define LOAD(module,var,name) var=GetProcAddress(module,name);if(!var){report("NT64-SERVICE MISSING " name "\n");ExitProcess(2);}
    LOAD(k,valloc,"VirtualAlloc");LOAD(k,vfree,"VirtualFree");
    LOAD(k,protect,"VirtualProtectEx");LOAD(k,flush,"FlushInstructionCache");
    LOAD(k,read_memory,"ReadProcessMemory");LOAD(k,write_memory,"WriteProcessMemory");
    LOAD(k,wait,"WaitForSingleObject");LOAD(k,close_handle,"CloseHandle");
    LOAD(k,terminate,"TerminateProcess");LOAD(k,exit_code,"GetExitCodeProcess");
    LOAD(k,get_pid,"GetCurrentProcessId");
    char *(*command)(void)=GetProcAddress(k,"GetCommandLineA");
    void *entry=GetProcAddress(nt,"NtQueryInformationProcess");
    if(command&&contains(command(),"--child")){
        query=entry;BASIC b={0};DWORD bytes=0;
        LONG before=hook_calls;
        LONG status=original_query?query(SELF,0,&b,sizeof(b),&bytes):-1;
        LONG after=hook_calls;
        check(status==0&&b.pid==get_pid()&&after==before+1,
              "interceptor invokes relocated original service");
        report(failures?"NT64-SERVICE CHILD FAILED\n":"NT64-SERVICE CHILD PASS\n");
        ExitProcess(failures?1:0);
    }
    query=copy_service(entry);
    delay=copy_service(GetProcAddress(nt,"NtDelayExecution"));
    allocate=copy_service(GetProcAddress(nt,"NtAllocateVirtualMemory"));
    release=copy_service(GetProcAddress(nt,"NtFreeVirtualMemory"));
    if(!query||!delay||!allocate||!release)ExitProcess(1);
    report("NT64-SERVICE copies ready\n");
    BASIC b={0};DWORD bytes=0;
    check(query(SELF,0,&b,sizeof(b),&bytes)==0&&bytes==sizeof(b)&&b.pid==get_pid(),
          "five-argument relocated service");
    check(query(SELF,0,&b,1,&bytes)==(LONG)0xc0000004,"copied service output size validation");
    report("NT64-SERVICE query ready\n");
    check(nt64_register_probe(GetProcAddress(nt,"NtQueryPerformanceCounter")),
          "nonvolatile GPR and SIMD state preserved");
    BYTE *stack=valloc(0,0x21000,0x3000,4);DWORD stack_old=0;
    check(stack!=0,"guarded service stack allocation");
    if(stack){
        BOOL guarded=protect(SELF,stack+0x20000,4096,1,&stack_old);
        check(guarded,"guarded service stack protection");
        if(guarded)check(nt64_stack_probe(query,stack+0x20000,&b)==(LONG)0xc0000005,
                         "inaccessible stack argument rejected");
        vfree(stack,0,0x8000);
    }
    void *memory=0;SIZE size=4096;
    check(allocate(SELF,&memory,0,&size,0x3000,4)==0&&memory&&size==4096,"six-argument service");
    if(memory){*(DWORD *)memory=0xabcdef01;size=0;
        check(release(SELF,&memory,&size,0x8000)==0,"four-argument service");}
    report("NT64-SERVICE memory ready\n");
    BYTE *invalid=valloc(0,4096,0x3000,4);DWORD old=0;
    check(invalid!=0,"invalid service allocation");
    if(invalid){
        BYTE code[]={0x4c,0x8b,0xd1,0xb8,0,0,0,0,0x0f,0x05,0xc3};
        /* Keep the native table selector; bit 12 would select Win32k on
         * Windows and can trigger its user callback initialization. */
        DWORD number=(*(DWORD *)((BYTE *)query+4)&0xffff0000U)|0x0fff;
        memcpy(code+4,&number,4);
        memcpy(invalid,code,sizeof(code));
        check(protect(SELF,invalid,4096,0x20,&old)&&flush(SELF,invalid,sizeof(code)),"invalid service RX");
        check(((LONG (*)(void))invalid)()==(LONG)0xc000001c,"unknown service rejected");
        vfree(invalid,0,0x8000);
    }
    report("NT64-SERVICE invalid ready\n");
    HANDLE (*thread)(void *,SIZE,void *,void *,DWORD,DWORD *)=GetProcAddress(k,"CreateThread");
    DWORD (*thread_exit)(HANDLE,DWORD *)=GetProcAddress(k,"GetExitCodeThread");
    HANDLE threads[4]={0};
    for(DWORD i=0;i<4;i++){threads[i]=thread(0,0,worker,0,0,0);check(threads[i]!=0,"concurrent caller");}
    for(DWORD i=0;i<4;i++)if(threads[i]){DWORD code=99;
        check(wait(threads[i],20000)==0&&thread_exit(threads[i],&code)&&code==0,
              "service waits preserve per-thread arguments");close_handle(threads[i]);}
    report("NT64-SERVICE threads ready\n");
    test_file(k,nt);report("NT64-SERVICE file ready\n");test_interception(k,entry);
    vfree(query,0,0x8000);vfree(delay,0,0x8000);vfree(allocate,0,0x8000);vfree(release,0,0x8000);
    report(failures?"NT64-SERVICE FAILED\n":"NT64-SERVICE PASS\n");
    ExitProcess(failures?1:0);
}
