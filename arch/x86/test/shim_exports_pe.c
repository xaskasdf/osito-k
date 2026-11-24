/* Exercise the same PE export contract on native Windows and OsitoK. */
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef int BOOL;
typedef int LONG;
typedef __UINTPTR_TYPE__ UPTR;
typedef __SIZE_TYPE__ SIZE;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define SELF ((HANDLE)(__INTPTR_TYPE__)-1)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
API LONG CALL NtQueryInformationProcess(HANDLE,DWORD,void *,DWORD,DWORD *);
void *memcpy(void *out,const void *in,SIZE n)
{for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=((const BYTE *)in)[i];return out;}
void *memset(void *out,int c,SIZE n)
{for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=(BYTE)c;return out;}
typedef struct {
    DWORD flags,time; WORD major,minor;
    DWORD name,base,functions,names,function_rva,name_rva,ordinal_rva;
} EXPORTS;
typedef struct {
    BYTE *base; DWORD size,export_rva,export_size;
    EXPORTS *exports; DWORD *functions,*names; WORD *ordinals;
} IMAGE;
typedef struct {
    LONG status; void *peb; UPTR affinity; LONG priority; UPTR pid,parent;
} BASIC;
typedef struct { WORD length,maximum; char *buffer; } STRING;
typedef struct {
    void *base,*allocation; DWORD allocation_protect; SIZE size;
    DWORD state,protect,type;
} MEMORY;
typedef struct {
    DWORD cb; WORD *reserved,*desktop,*title;
    DWORD x,y,w,h,xc,yc,fill,flags; WORD show,bytes; BYTE *data;
    HANDLE input,output,error;
} STARTUP;
typedef struct { HANDLE process,thread; DWORD pid,tid; } PROCESS;
static DWORD failures;
static BOOL child;
static void report(const char *s)
{DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void check(BOOL ok,const char *s)
{if(!ok){failures++;report("SHIM-EXPORTS FAIL: ");report(s);report("\n");}}
static int compare(const char *a,const char *b)
{while(*a&&*a==*b){a++;b++;}return (BYTE)*a-(BYTE)*b;}
static BOOL range(IMAGE *i,DWORD rva,SIZE size)
{return rva<i->size&&size<=i->size-rva;}
static const char *string(IMAGE *i,DWORD rva)
{
    if(!range(i,rva,1))return 0;
    for(DWORD n=rva;n<i->size;n++)if(!i->base[n])return (char *)i->base+rva;
    return 0;
}
static BOOL parse(HANDLE module,IMAGE *i)
{
    BYTE *base=module; i->base=base;
    if(!base||*(WORD *)base!=0x5a4d)return 0;
    DWORD nt=*(DWORD *)(base+60);
    if(nt>4096||*(DWORD *)(base+nt)!=0x4550)return 0;
    BYTE *opt=base+nt+24;WORD magic=*(WORD *)opt;
    if(magic!= (sizeof(void *)==8?0x20b:0x10b))return 0;
    i->size=*(DWORD *)(opt+56);
    DWORD data=magic==0x20b?112:96;
    if(*(DWORD *)(opt+data-4)<1)return 0;
    i->export_rva=*(DWORD *)(opt+data);i->export_size=*(DWORD *)(opt+data+4);
    if(!i->export_rva||i->export_size<sizeof(EXPORTS)||
       !range(i,i->export_rva,i->export_size))return 0;
    i->exports=(void *)(base+i->export_rva);EXPORTS *e=i->exports;
    if(!e->functions||!e->names||e->names>65535||e->functions>65535||
       !string(i,e->name)||!range(i,e->function_rva,(SIZE)e->functions*4)||
       !range(i,e->name_rva,(SIZE)e->names*4)||!range(i,e->ordinal_rva,(SIZE)e->names*2))return 0;
    i->functions=(void *)(base+e->function_rva);
    i->names=(void *)(base+e->name_rva);i->ordinals=(void *)(base+e->ordinal_rva);
    for(DWORD n=0;n<e->names;n++){
        const char *name=string(i,i->names[n]);
        if(!name||i->ordinals[n]>=e->functions)return 0;
        if(n&&compare(string(i,i->names[n-1]),name)>=0)return 0;
        DWORD rva=i->functions[i->ordinals[n]];
        if(!rva||!range(i,rva,1))return 0;
    }
    return 1;
}
static void *lookup(IMAGE *i,const char *name,DWORD *ordinal)
{
    DWORD low=0,high=i->exports->names;
    while(low<high){
        DWORD m=low+(high-low)/2;int c=compare(name,(char *)i->base+i->names[m]);
        if(c<0)high=m;else if(c>0)low=m+1;
        else {if(ordinal)*ordinal=i->exports->base+i->ordinals[m];
            return i->base+i->functions[i->ordinals[m]];}
    }
    return 0;
}
static BOOL has(const char *s,const char *word)
{for(;*s;s++){DWORD n=0;while(word[n]&&word[n]==s[n])n++;if(!word[n])return 1;}return 0;}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),nt=LoadLibraryA("ntdll.dll");
    char *(CALL *command)(void)=GetProcAddress(k,"GetCommandLineA");
    child=command&&has(command()," export-child");
    IMAGE i={0};BOOL valid=parse(nt,&i);
    check(valid,"PE export directory and sorted bounded names");
    if(!valid)ExitProcess(1);
    EXPORTS *e=i.exports;
    for(DWORD n=0;n<e->names;n++){
        const char *name=(char *)i.base+i.names[n];
        DWORD rva=i.functions[i.ordinals[n]],ordinal=e->base+i.ordinals[n];
        /* Forwarders are strings rather than local addresses. NTDLL normally
         * has none, but the fixture does not call a forwarder as machine code. */
        if(rva>=i.export_rva&&rva-i.export_rva<i.export_size)continue;
        void *fn=i.base+rva;
        check(GetProcAddress(nt,name)==fn,"named lookup equals raw EAT");
        if(ordinal<=65535)check(GetProcAddress(nt,(const char *)(UPTR)ordinal)==fn,
                               "ordinal lookup equals raw EAT");
    }
    check(!GetProcAddress(nt,"ntQueryInformationProcess")&&GetLastError()==127,
          "case-sensitive lookup");
    check(!GetProcAddress(nt,"OsitoMissingExport_ContractTest")&&GetLastError()==127,
          "missing name reports procedure not found");
    DWORD beyond=e->base+e->functions;
    if(beyond<=65535)check(!GetProcAddress(nt,(const char *)(UPTR)beyond),"out-of-range ordinal");
    DWORD ordinal=0;
    LONG (CALL *query)(HANDLE,DWORD,void *,DWORD,DWORD *)=lookup(&i,"NtQueryInformationProcess",&ordinal);
    check(query!=0&&query==(void *)NtQueryInformationProcess,"IAT and raw EAT share entry point");
    if(!query)ExitProcess(1);
    struct { UPTR before; BASIC info; UPTR after; } basic={0};
    basic.before=basic.after=0x76543210;
    DWORD returned=0;
    DWORD (CALL *pid)(void)=GetProcAddress(k,"GetCurrentProcessId");
    check(query(SELF,0,&basic.info,sizeof(basic.info),&returned)==0&&returned==sizeof(basic.info)&&
          basic.info.peb&&basic.info.pid==pid(),"direct NT query returns current process identity");
    check(query(SELF,0,&basic.info,0,&returned)<0,"direct NT query enforces output size");
    check(basic.before==0x76543210&&basic.after==0x76543210,"NT query output canaries");
    LONG (CALL *ldr)(HANDLE,STRING *,DWORD,void **)=lookup(&i,"LdrGetProcedureAddress",0);
    STRING name={25,25,"NtQueryInformationProcessNOT_IN_COUNTED_STRING"};
    struct { UPTR before; void *address; UPTR after; } via={0x12345678,0,0x87654321};
    check(ldr&&ldr(nt,&name,0,&via.address)==0&&via.address==(void *)query,"Ldr counted name resolution");
    via.address=0;
    check(ldr&&ldr(nt,0,ordinal,&via.address)==0&&via.address==(void *)query,"Ldr ordinal resolution");
    check(via.before==0x12345678&&via.after==0x87654321,"Ldr pointer-width output canaries");
    LONG (CALL *allocate)(HANDLE,void **,UPTR,SIZE *,DWORD,DWORD)=lookup(&i,"NtAllocateVirtualMemory",0);
    LONG (CALL *release)(HANDLE,void **,SIZE *,DWORD)=lookup(&i,"NtFreeVirtualMemory",0);
    void *memory=0;SIZE size=4096;
    check(allocate&&release,"six-argument NT export available");
    if(allocate&&release){
        check(allocate(SELF,&memory,0,&size,0x3000,4)==0&&memory&&size>=4096,
              "direct NT allocation and pointer-sized inout");
        if(memory){*(volatile DWORD *)memory=0x12345678;size=0;
            check(release(SELF,&memory,&size,0x8000)==0,"direct NT release");}
    }
    SIZE (CALL *vquery)(const void *,MEMORY *,SIZE)=GetProcAddress(k,"VirtualQuery");
    MEMORY mbi={0};
    check(vquery&&vquery(e,&mbi,sizeof(mbi))==sizeof(mbi)&&
          ((mbi.protect&255)==2||(mbi.protect&255)==0x20),
          "export metadata readable non-writable");
    check(vquery&&vquery((void *)query,&mbi,sizeof(mbi))==sizeof(mbi)&&(mbi.protect&255)==0x20,
          "export entry executable read-only");
    BOOL (CALL *free_library)(HANDLE)=GetProcAddress(k,"FreeLibrary");
    for(DWORD n=0;n<16;n++){
        HANDLE again=LoadLibraryA("ntdll.dll");
        check(again==nt&&GetProcAddress(again,"NtQueryInformationProcess")==query,
              "repeated load preserves published entries");
        check(free_library&&free_library(again),"release extra facade reference");
    }
    if(child){
        BOOL (CALL *protect)(void *,SIZE,DWORD,DWORD *)=GetProcAddress(k,"VirtualProtect");
        DWORD old=0;
        check(protect&&protect(i.base+e->name,1,4,&old),"child private metadata page");
        if(!failures){i.base[e->name]='!';check(protect(i.base+e->name,1,old,&old),"restore child protection");}
    }else{
        DWORD (CALL *filename)(HANDLE,char *,DWORD)=GetProcAddress(k,"GetModuleFileNameA");
        BOOL (CALL *create)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *)=GetProcAddress(k,"CreateProcessA");
        DWORD (CALL *wait)(HANDLE,DWORD)=GetProcAddress(k,"WaitForSingleObject");
        BOOL (CALL *exit_code)(HANDLE,DWORD *)=GetProcAddress(k,"GetExitCodeProcess");
        BOOL (CALL *close)(HANDLE)=GetProcAddress(k,"CloseHandle");
        char path[320],cmd[352];DWORD n=filename(0,path,sizeof(path));
        check(n&&n<sizeof(path),"child executable path");
        if(n&&n<sizeof(path)){
            cmd[0]='"';memcpy(cmd+1,path,n);cmd[n+1]='"';memcpy(cmd+n+2," export-child",14);
            for(DWORD r=0;r<4;r++){
                STARTUP start={0};PROCESS process={0};start.cb=sizeof(start);DWORD code=99;
                BOOL ok=create(path,cmd,0,0,0,0,0,0,&start,&process);
                check(ok,"create child with private export image");
                if(ok){check(wait(process.process,30000)==0&&exit_code(process.process,&code)&&code==0,
                             "child export contract and teardown");close(process.thread);close(process.process);}
                check(i.base[e->name]!='!',"child writes do not modify parent facade");
            }
        }
    }
    free_library(nt);free_library(k);
    if(!failures)report(child?"SHIM-EXPORTS CHILD PASS\n":"SHIM-EXPORTS PASS\n");
    ExitProcess(failures?1:0);
}
