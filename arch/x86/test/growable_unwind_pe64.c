/* Native-oracle coverage for JIT unwind table registration and growth. */
typedef unsigned char BYTE;
typedef unsigned int DWORD;
typedef int BOOL;
typedef __UINTPTR_TYPE__ UPTR;
typedef __SIZE_TYPE__ SIZE;
typedef void *HANDLE;
typedef struct { DWORD begin, end, unwind; } FUNCTION;
typedef struct { HANDLE process, thread; DWORD pid, tid; } PROCESS;
#define API __declspec(dllimport)
API void ExitProcess(DWORD);
API HANDLE GetStdHandle(DWORD);
API BOOL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE LoadLibraryA(const char *);
API void *GetProcAddress(HANDLE,const char *);
static DWORD (*add)(void **,FUNCTION *,DWORD,DWORD,UPTR,UPTR);
static void (*grow)(void *,DWORD);
static void (*remove_table)(void *);
static FUNCTION *(*lookup)(UPTR,UPTR *,void *);
static void *(*unwind)(DWORD,UPTR,UPTR,FUNCTION *,void *,void **,UPTR *,void *);
static BOOL (*fixed_add)(FUNCTION *,DWORD,UPTR);
static BOOL (*fixed_delete)(FUNCTION *);
static void *(*alloc)(void *,SIZE,DWORD,DWORD);
static BOOL (*release)(void *,SIZE,DWORD);
static HANDLE (*create_thread)(void *,SIZE,DWORD (*)(void *),void *,DWORD,DWORD *);
static BOOL (*close_handle)(HANDLE);
static DWORD (*wait)(HANDLE,DWORD);
static void (*sleep_ms)(DWORD);
static char *(*args)(void);
static DWORD (*module_path)(HANDLE,char *,DWORD);
static BOOL (*create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,PROCESS *);
static BOOL (*exit_code)(HANDLE,DWORD *);
static FUNCTION table[64];
static BYTE *region;
static volatile DWORD finished, worker_failed;
void *memset(void *p,int c,SIZE n)
{ for(SIZE i=0;i<n;i++)((BYTE *)p)[i]=(BYTE)c;return p; }
static void print(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void check(BOOL ok,const char *s)
{
    print(ok?"GROWABLE PASS ":"GROWABLE FAIL ");print(s);print("\n");
    if(!ok)ExitProcess(80);
}
static BOOL child_mode(void)
{ const char *s=args();for(;*s;s++)if(s[0]=='-'&&s[1]=='-'&&s[2]=='c')return 1;return 0; }
static void status_line(const char *label,DWORD status)
{
    char hex[11]="0x00000000";const char digits[]="0123456789abcdef";
    for(DWORD i=0;i<8;i++)hex[9-i]=digits[(status>>(4*i))&15];
    print("GROWABLE STATUS ");print(label);print(" ");print(hex);print("\n");
}
static DWORD observer(void *unused)
{
    (void)unused;
    while(!__atomic_load_n(&finished,__ATOMIC_ACQUIRE)){
        UPTR base=0;
        if(lookup((UPTR)region+0x105,&base,0)!=&table[0]||base!=(UPTR)region)
            worker_failed=1;
        sleep_ms(0);
    }
    return worker_failed;
}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),nt=LoadLibraryA("ntdll.dll");
    check(k&&nt,"load libraries");
#define BIND(module,var,name) do{var=(void *)GetProcAddress(module,name);check(var!=0,name);}while(0)
    BIND(nt,add,"RtlAddGrowableFunctionTable"); BIND(nt,grow,"RtlGrowFunctionTable");
    BIND(nt,remove_table,"RtlDeleteGrowableFunctionTable"); BIND(nt,lookup,"RtlLookupFunctionEntry");
    BIND(nt,unwind,"RtlVirtualUnwind"); BIND(nt,fixed_add,"RtlAddFunctionTable");
    BIND(nt,fixed_delete,"RtlDeleteFunctionTable"); BIND(k,alloc,"VirtualAlloc");
    BIND(k,release,"VirtualFree"); BIND(k,create_thread,"CreateThread");
    BIND(k,close_handle,"CloseHandle"); BIND(k,wait,"WaitForSingleObject");
    BIND(k,sleep_ms,"Sleep"); BIND(k,args,"GetCommandLineA");
    BIND(k,module_path,"GetModuleFileNameA"); BIND(k,create_process,"CreateProcessA");
    BIND(k,exit_code,"GetExitCodeProcess");
    region=alloc(0,65536,0x3000,0x40);check(region!=0,"code allocation");
    /* UWOP_ALLOC_SMALL, 40 bytes, after SUB RSP,40 at prologue offset 4. */
    region[0xf00]=1;region[0xf01]=4;region[0xf02]=1;
    region[0xf04]=4;region[0xf05]=0x42;
    for(DWORD i=0;i<64;i++){
        DWORD pc=0x100+32*i;
        table[i]=(FUNCTION){pc,pc+16,0xf00};
        for(DWORD j=0;j<16;j++)region[pc+j]=0x90;
        region[pc]=0x48;region[pc+1]=0x83;region[pc+2]=0xec;region[pc+3]=0x28;
        region[pc+11]=0x48;region[pc+12]=0x83;region[pc+13]=0xc4;
        region[pc+14]=0x28;region[pc+15]=0xc3;
    }
    if(child_mode()){
        for(DWORD i=0;i<16;i++){
            void *handle=0;
            check(add(&handle,table,1,64,(UPTR)region+4096*i,(UPTR)region+4096*(i+1))==0&&handle,
                  "child owned registration");
        }
        print("GROWABLE CHILD DONE\n");ExitProcess(41);
    }
    void *handle=0; UPTR base=0;
    check(!lookup((UPTR)region+0x105,&base,0),"unregistered code");
    check(add(&handle,table,0,64,(UPTR)region,(UPTR)region+4096)==0&&handle,"empty registration");
    void *collision=0;
    DWORD collision_status=add(&collision,table,1,64,(UPTR)region,(UPTR)region+4096);
    status_line("same range",collision_status);
    check(collision_status==0xc0000018U,"same range rejected");
    if(!collision_status)remove_table(collision);
    collision=0;
    collision_status=add(&collision,table,1,64,(UPTR)region+2048,(UPTR)region+6144);
    status_line("overlapping range",collision_status);
    check(collision_status==0xc0000018U,"overlapping range rejected");
    if(!collision_status)remove_table(collision);
    collision=0;
    collision_status=add(&collision,table,1,64,(UPTR)region+4096,(UPTR)region+8192);
    status_line("adjacent range",collision_status);
    check(collision_status==0&&collision,"adjacent range accepted");
    if(!collision_status)remove_table(collision);
    check(!lookup((UPTR)region+0x105,&base,0),"zero published entries");
    grow(handle,1);
    check(lookup((UPTR)region+0x105,&base,0)==&table[0]&&base==(UPTR)region,"first entry published");
    check(!lookup((UPTR)region+0x125,&base,0),"unpublished suffix hidden");
    HANDLE worker=create_thread(0,0,observer,0,0,0);check(worker!=0,"concurrent reader");
    for(DWORD count=2;count<=64;count++){
        grow(handle,count);sleep_ms(1);
        check(lookup((UPTR)region+table[count-1].begin+5,&base,0)==&table[count-1],"grown entry lookup");
    }
    __atomic_store_n(&finished,1,__ATOMIC_RELEASE);
    check(wait(worker,5000)==0&&!worker_failed,"registration remains visible during growth");
    check(close_handle(worker),"reader closed");
    check(!lookup((UPTR)region+table[0].end,&base,0),"function end is exclusive");
    check(!lookup((UPTR)region+4096,&base,0),"registered range end is exclusive");
    BYTE context[1232] __attribute__((aligned(16)))={0};UPTR stack[8]={0};
    stack[5]=(UPTR)&mainCRTStartup;
    *(UPTR *)(context+0x98)=(UPTR)stack;
    *(UPTR *)(context+0xf8)=(UPTR)region+table[63].begin+5;
    void *data=0;UPTR frame=0;
    unwind(0,(UPTR)region,*(UPTR *)(context+0xf8),&table[63],context,&data,&frame,0);
    check(*(UPTR *)(context+0x98)==(UPTR)stack+48&&
          *(UPTR *)(context+0xf8)==(UPTR)&mainCRTStartup,"grown function virtual unwind");
    remove_table(handle);
    check(!lookup((UPTR)region+0x105,&base,0),"removed table is invisible");
    check(fixed_add(table,1,(UPTR)region),"ordinary registration after growable removal");
    check(lookup((UPTR)region+0x105,&base,0)==&table[0],"ordinary lookup preserved");
    check(fixed_delete(table),"ordinary removal preserved");
    for(DWORD i=0;i<160;i++){
        handle=0;
        check(add(&handle,table,1,1,(UPTR)region,(UPTR)region+4096)==0&&handle,"registration slot reused");
        remove_table(handle);
    }
    char path[512];check(module_path(0,path,sizeof(path))!=0,"image path");
    for(DWORD i=0;i<9;i++){
        char command[540];DWORD p=0,n=0;command[p++]='"';
        while(path[n])command[p++]=path[n++];command[p++]='"';
        const char *suffix=" --child";while(*suffix)command[p++]=*suffix++;command[p]=0;
        BYTE startup[104]={0};*(DWORD *)startup=sizeof(startup);PROCESS child={0};DWORD code=0;
        check(create_process(path,command,0,0,0,0,0,0,startup,&child),"create registration owner");
        check(wait(child.process,10000)==0&&exit_code(child.process,&code)&&code==41,"owner exit releases tables");
        check(close_handle(child.thread)&&close_handle(child.process),"child handles closed");
    }
    check(release(region,0,0x8000),"code released after unregister");
    print("GROWABLE DONE\n");ExitProcess(0);
}
