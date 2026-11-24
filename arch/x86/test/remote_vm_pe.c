/* Identical native Windows / Osito-K contract fixture, without CRT. */
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
static BOOL (CALL *duplicate)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static BOOL (CALL *read_memory)(HANDLE,const void *,void *,SIZE,SIZE *);
static BOOL (CALL *write_memory)(HANDLE,void *,const void *,SIZE,SIZE *);
static SIZE (CALL *query_memory)(HANDLE,const void *,void *,SIZE);
static BOOL (CALL *protect_memory)(HANDLE,void *,SIZE,DWORD,DWORD *);
static void *(CALL *allocate)(HANDLE,void *,SIZE,DWORD,DWORD);
static BOOL (CALL *release)(HANDLE,void *,SIZE,DWORD);
static long (CALL *nt_allocate)(HANDLE,void **,UPTR,SIZE *,DWORD,DWORD);
static long (CALL *nt_free)(HANDLE,void **,SIZE *,DWORD);
static long (CALL *nt_protect)(HANDLE,void **,SIZE *,DWORD,DWORD *);
static long (CALL *nt_query)(HANDLE,void *,DWORD,void *,SIZE,SIZE *);
static long (CALL *query_process)(HANDLE,DWORD,void *,DWORD,DWORD *);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static char *(CALL *command_line)(void);
static volatile UPTR child_allocation;
static DWORD failures;
static HANDLE self=(HANDLE)(UPTR)-1;
static void report(const char *s)
{DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void check(BOOL ok,const char *s)
{if(!ok){failures++;report("REMOTE-VM FAIL: ");report(s);report("\n");}}
static BOOL contains(const char *s,const char *word)
{for(;*s;s++){DWORD i=0;while(word[i]&&s[i]==word[i])i++;if(!word[i])return 1;}return 0;}
static BOOL query(HANDLE p,void *address,DWORD state,DWORD protection)
{
    struct {MEMORY info;DWORD guard;} m={0};m.guard=0xfeed1234;
    SIZE n=query_memory(p,address,&m.info,sizeof(m.info));
    check(m.guard==0xfeed1234,"VirtualQueryEx output width");
    return n==sizeof(m.info)&&m.info.state==state&&
           (state!=0x1000||m.info.protect==protection);
}
static DWORD read_word(HANDLE p,void *address)
{
    DWORD v=0xffffffff;struct {SIZE n;DWORD guard;} count={0,0xfeed4321};
    check(read_memory(p,address,&v,4,&count.n)&&count.n==4,"remote read");
    check(count.guard==0xfeed4321,"remote read count width");return v;
}
static void write_word(HANDLE p,void *address,DWORD v)
{
    struct {SIZE n;DWORD guard;} count={0,0xfeed4321};
    check(write_memory(p,address,&v,4,&count.n)&&count.n==4,"remote write");
    check(count.guard==0xfeed4321,"remote write count width");
}
static void finish(PROCESS *p,DWORD expected)
{
    DWORD code=0xffffffff,wait=wait_object(p->process,20000);
    if(wait!=0){terminate_process(p->process,99);wait_object(p->process,5000);}
    check(wait==0&&exit_code(p->process,&code)&&code==expected,"child completed");
}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),n=LoadLibraryA("ntdll.dll");
#define LOAD(module,var,name) var=GetProcAddress(module,name);if(!var){report("REMOTE-VM MISSING " name "\n");ExitProcess(2);}
    LOAD(k,create_process,"CreateProcessA");LOAD(k,resume_thread,"ResumeThread");
    LOAD(k,wait_object,"WaitForSingleObject");LOAD(k,close_handle,"CloseHandle");
    LOAD(k,terminate_process,"TerminateProcess");LOAD(k,exit_code,"GetExitCodeProcess");
    LOAD(k,duplicate,"DuplicateHandle");
    LOAD(k,read_memory,"ReadProcessMemory");LOAD(k,write_memory,"WriteProcessMemory");
    LOAD(k,query_memory,"VirtualQueryEx");LOAD(k,protect_memory,"VirtualProtectEx");
    LOAD(k,allocate,"VirtualAllocEx");LOAD(k,release,"VirtualFreeEx");
    LOAD(n,nt_allocate,"NtAllocateVirtualMemory");LOAD(n,nt_free,"NtFreeVirtualMemory");
    LOAD(n,nt_protect,"NtProtectVirtualMemory");LOAD(n,nt_query,"NtQueryVirtualMemory");
    LOAD(n,query_process,"NtQueryInformationProcess");LOAD(k,module_name,"GetModuleFileNameA");
    LOAD(k,command_line,"GetCommandLineA");
#undef LOAD
    if(contains(command_line(),"--child")){
        void *p=(void *)child_allocation;
        check(p&&query(self,p,0x1000,2),"child owns broker allocation and protection");
        if(p){check(*(volatile DWORD *)p==0x76543210,"child sees broker data");
              check(release(self,p,0,0x8000),"child frees broker allocation");}
        report(failures?"REMOTE-VM CHILD FAILED\n":"REMOTE-VM CHILD PASS\n");
        ExitProcess(failures?1:0);
    }
    char path[320];module_name(0,path,sizeof(path));
    BYTE *parent=allocate(self,(void *)(UPTR)0x60000000,0x10000,0x3000,4);
    check(parent==(void *)(UPTR)0x60000000,"parent sentinel allocation");
    if(!parent)ExitProcess(1);
    *(DWORD *)(parent+4096)=0x12345678;
    for(DWORD iteration=0;iteration<4;iteration++){
        STARTUP startup={0};startup.cb=sizeof(startup);PROCESS a={0},b={0};
        char cmd[]="remote-vm --child";
        BOOL made_a=create_process(path,cmd,0,0,0,4,0,0,&startup,&a);
        BOOL made_b=create_process(path,cmd,0,0,0,4,0,0,&startup,&b);
        check(made_a&&made_b,"create two suspended children");
        if(!made_a||!made_b){
            if(made_a){terminate_process(a.process,77);finish(&a,77);
                       close_handle(a.thread);close_handle(a.process);}
            if(made_b){terminate_process(b.process,77);finish(&b,77);
                       close_handle(b.thread);close_handle(b.process);}
            break;
        }
        check(query(a.process,parent,0x10000,0),"child address initially free, not parent's VMA");
        void *reserved=allocate(a.process,parent+0x1234,0x2100,0x2000,4);
        check(reserved==parent,"remote reserve and allocation-granularity alignment");
        if(reserved!=parent){
            terminate_process(a.process,77);terminate_process(b.process,77);
            finish(&a,77);finish(&b,77);
            close_handle(a.thread);close_handle(a.process);close_handle(b.thread);close_handle(b.process);
            continue;
        }
        check(query(a.process,parent,0x2000,0),"child reserve-only state");
        check(query(b.process,parent,0x10000,0),"second child has no reservation");
        check(allocate(a.process,parent+4096,8192,0x1000,4)==parent+4096,"commit remote subset");
        check(read_word(a.process,parent+4096)==0,"remote commit zero-filled");
        write_word(a.process,parent+4096,0x11223344);
        DWORD old=0;
        check(protect_memory(a.process,parent+4096,4096,2,&old)&&old==4,"remote protect");
        check(query(a.process,parent+4096,0x1000,2),"child changed protection");
        check(query(self,parent+4096,0x1000,4),"parent protection unchanged");
        check(*(DWORD *)(parent+4096)==0x12345678,"parent contents unchanged");
        check(read_word(a.process,parent+4096)==0x11223344,"child contents private");
        check(release(a.process,parent+4096,4096,0x4000),"remote decommit");
        check(query(a.process,parent+4096,0x2000,0),"decommit retained reservation");
        check(allocate(a.process,parent+4096,4096,0x1000,4)==parent+4096,"remote recommit");
        check(read_word(a.process,parent+4096)==0,"recommit zero-filled");
        check(!release(a.process,parent,4096,0x8000)&&GetLastError()==87,"release requires zero size");
        check(release(a.process,parent,0,0x8000),"remote release reservation");
        check(query(a.process,parent,0x10000,0),"released child address free");
        check(query(self,parent,0x1000,4)&&*(DWORD *)(parent+4096)==0x12345678,"remote free preserved parent");

        struct {void *p;DWORD guard;} base={parent,0xabcdef01};
        struct {SIZE n;DWORD guard;} size={4096,0xabcdef02};
        check(nt_allocate(a.process,&base.p,0,&size.n,0x3000,4)==0&&base.p==parent,
              "direct NT remote allocation");
        check(base.guard==0xabcdef01&&size.guard==0xabcdef02,"NT in-out pointer widths");
        write_word(a.process,parent,0x76543210);
        check(nt_protect(a.process,&base.p,&size.n,2,&old)==0&&old==4,"direct NT remote protect");
        struct {MEMORY m;DWORD guard;} mem={0};mem.guard=0xabcdef03;
        struct {SIZE n;DWORD guard;} length={0,0xabcdef04};
        check(nt_query(a.process,parent,0,&mem.m,sizeof(mem.m),&length.n)==0&&
              length.n==sizeof(mem.m)&&mem.m.protect==2,"direct NT remote query");
        check(mem.guard==0xabcdef03&&length.guard==0xabcdef04,"NT query output widths");
        size.n=0;
        check(nt_free(a.process,&base.p,&size.n,0x8000)==0,"direct NT remote free");
        check(base.guard==0xabcdef01&&size.guard==0xabcdef02,"NT free output widths");

        HANDLE query_only=0,op_only=0;
        check(duplicate(self,a.process,self,&query_only,0x0400,0,0),"duplicate query-only handle");
        check(duplicate(self,a.process,self,&op_only,0x0008,0,0),"duplicate operation-only handle");
        check(query(query_only,parent,0x10000,0),"query-only handle can query");
        check(!allocate(query_only,0,4096,0x1000,4)&&GetLastError()==5,"allocate denied without VM_OPERATION");
        void *auto_page=allocate(op_only,0,4096,0x1000,4);
        check(auto_page!=0,"commit-only NULL implicitly reserves remotely");
        MEMORY denied;
        check(!query_memory(op_only,auto_page,&denied,sizeof(denied))&&GetLastError()==5,"query denied without QUERY_INFORMATION");
        check(!protect_memory(query_only,auto_page,4096,2,&old)&&GetLastError()==5,"protect denied without VM_OPERATION");
        check(!release(query_only,auto_page,0,0x8000)&&GetLastError()==5,"free denied without VM_OPERATION");
        base.p=0;size.n=4096;
        check(nt_allocate(query_only,&base.p,0,&size.n,0x1000,4)==(long)0xc0000022,"NT allocation enforces rights");
        if(auto_page){write_word(a.process,auto_page,0x1234);
                     check(release(op_only,auto_page,0,0x8000),"operation-only handle can free");}
        close_handle(query_only);close_handle(op_only);
        check(!allocate((HANDLE)(UPTR)0x1234,0,4096,0x1000,4)&&GetLastError()==6,"invalid process handle rejected");

        check(allocate(a.process,parent,4096,0x3000,4)==parent,"prepare resumed child data");
        check(allocate(b.process,parent,4096,0x3000,4)==parent,"same VA in sibling");
        write_word(a.process,parent,0x76543210);write_word(b.process,parent,0x55667788);
        check(read_word(a.process,parent)==0x76543210,"siblings do not alias");
        check(protect_memory(a.process,parent,4096,2,&old),"prepare child read-only page");
        BASIC info={0};DWORD returned=0;SIZE got=0;BYTE *image=0;
        check(query_process(a.process,0,&info,sizeof(info),&returned)==0,"query child PEB");
        check(read_memory(a.process,(BYTE *)info.peb+2*sizeof(void *),&image,sizeof(image),&got)&&image,"read child image base");
        void *own_image=GetModuleHandleA(0);
        check(query(self,own_image,0x1000,2),"parent image header read-only");
        check(query(a.process,image,0x1000,2),"child image header read-only");
        check(protect_memory(a.process,image,4096,1,&old)&&old==2,"protect child image header as no-access");
        check(query(a.process,image,0x1000,1),"child image header changed protection");
        check(query(self,own_image,0x1000,2),"remote image protection preserved parent");
        check(protect_memory(a.process,image,4096,2,&old)&&old==1,"restore child image header");
        void *slot=image+((BYTE *)&child_allocation-(BYTE *)GetModuleHandleA(0));
        UPTR remote_address=(UPTR)parent;
        check(write_memory(a.process,slot,&remote_address,sizeof(remote_address),&got),"pass allocation to child");
        check(resume_thread(a.thread)==1,"resume prepared child");finish(&a,0);
        check(terminate_process(b.process,77),"terminate sibling with live allocation");finish(&b,77);
        check(!allocate(b.process,0,4096,0x1000,4),"cannot allocate in exited process");
        close_handle(a.thread);close_handle(a.process);close_handle(b.thread);close_handle(b.process);
        check(query(self,parent,0x1000,4)&&*(DWORD *)(parent+4096)==0x12345678,"child exit preserves parent");
    }
    check(release(self,parent,0,0x8000),"release parent sentinel");
    report(failures?"REMOTE-VM FAILED\n":"REMOTE-VM PASS\n");
    ExitProcess(failures?1:0);
}
