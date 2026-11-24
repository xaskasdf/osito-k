/* Exercise original Cabinet FDI on Windows and the guest's callback adapter.
 * Extracted installer payloads are data; this fixture never runs them. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef __UINTPTR_TYPE__ UP;
typedef __INTPTR_TYPE__ IP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
typedef struct { int oper,type,error; } ERF;
typedef struct {
    int bytes; char *name,*disk,*path; void *user; IP file;
    WORD date,time,attributes,set,number,folder; int error;
} NOTICE;
static void *(*make_fdi)(void *(*)(DWORD),void (*)(void *),IP (*)(char *,int,int),
                        DWORD (*)(IP,void *,DWORD),DWORD (*)(IP,void *,DWORD),
                        int (*)(IP),int (*)(IP,int,int),int,ERF *);
static int (*copy_fdi)(void *,char *,char *,int,IP (*)(int,NOTICE *),void *,void *);
static int (*destroy_fdi)(void *);
static void *(CALL *allocate)(DWORD,UP);
static void *(CALL *release)(void *);
static void *(CALL *open_file)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
static int (CALL *read_file)(void *,void *,DWORD,DWORD *,void *);
static int (CALL *close_file)(void *);
static DWORD (CALL *seek_file)(void *,int,int *,DWORD);
static const WORD *(CALL *command_line)(void);
typedef struct { void *process,*thread; DWORD pid,tid; } PROCESS;
static DWORD (CALL *module_path)(void *,char *,DWORD);
static int (CALL *create_process)(const char *,char *,void *,void *,int,DWORD,void *,const char *,void *,PROCESS *);
static DWORD (CALL *wait_for)(void *,DWORD);
static int (CALL *exit_code)(void *,DWORD *);
static DWORD failures,checks,mode,active_allocations,allocations,free_calls;
static DWORD inputs,outputs,cab_notices,copy_notices,close_notices,enum_notices;
static DWORD output_size,output_capacity,fail_alloc,hash_value;
static BYTE *output_data;
static const char *case_name;

static void text(const char *s)
{
    DWORD n=0,written;while(s[n])n++;
    WriteFile(GetStdHandle((DWORD)-11),s,n,&written,0);
}
static void hex(DWORD value)
{
    char s[9];for(unsigned i=0;i<8;i++)s[i]="0123456789abcdef"[(value>>(28-4*i))&15];
    s[8]=0;text(s);
}
static void check(int good,const char *label)
{
    checks++;if(!good)failures++;
    text(good?"FDI PASS ":"FDI FAIL ");text(label);text("\n");
}
static void *fdi_alloc(DWORD bytes)
{
    allocations++;
    if(fail_alloc && --fail_alloc==0)return 0;
    void *p=allocate(0,bytes);if(p)active_allocations++;return p;
}
static void fdi_free(void *p)
{
    if(p){free_calls++;check(active_allocations!=0,"free balances allocation");active_allocations--;release(p);}
}
static IP fdi_open(char *path,int flags,int pmode)
{
    (void)flags;(void)pmode;
    void *h=open_file(path,0x80000000U,7,0,3,0x80,0);
    if(h!=(void *)(UP)-1)inputs++;
    return (IP)h;
}
static DWORD fdi_read(IP handle,void *buffer,DWORD count)
{
    DWORD got=0;
    if(mode==7)return (DWORD)-1;
    return read_file((void *)handle,buffer,count,&got,0)?got:(DWORD)-1;
}
static DWORD fdi_write(IP handle,void *buffer,DWORD count)
{
    if(mode==10){text("FDI BUSY EXIT\n");ExitProcess(0);}
    check(handle==2 && outputs==1,"destination callback handle");
    if(mode==6)return count?count-1:0;
    if(output_size>output_capacity || count>output_capacity-output_size){check(0,"output bounds");return (DWORD)-1;}
    const BYTE *source=buffer;
    for(DWORD i=0;i<count;i++){
        output_data[output_size++]=source[i];
        hash_value=(hash_value^source[i])*16777619U;
    }
    return count;
}
static int fdi_close(IP handle)
{
    if(handle==2){
        check(outputs==1,"abort closes output once");outputs=0;
        release(output_data);output_data=0;return 0;
    }
    check(inputs!=0,"input closes once");
    if(inputs)inputs--;
    return close_file((void *)handle)?0:-1;
}
static int fdi_seek(IP handle,int offset,int origin)
{
    return (int)seek_file((void *)handle,offset,0,(DWORD)origin);
}
static IP fdi_notify(int kind,NOTICE *n)
{
    check(n->user==(void *)(UP)0x123abc,"notification user context");
    if(kind==0){
        cab_notices++;
        text("FDI CAB ");text(case_name);text(" set=");hex(n->set);
        text(" number=");hex(n->number);text(" next=");text(n->name);text("\n");
        return mode==2?-1:0;
    }
    if(kind==5){
        enum_notices++;
        if(mode==8)n->folder=0;
        if(mode==9)return -1;
        return 0;
    }
    if(kind==2){
        copy_notices++;
        text("FDI MEMBER ");text(case_name);text(" ");text(n->name);
        text(" bytes=");hex(n->bytes);text(" attr=");hex(n->attributes);
        text(" folder=");hex(n->folder);text("\n");
        if(mode==1)return 0;
        if(mode==3)return -1;
        check(outputs==0 && n->bytes>=0,"one output at a time");
        output_capacity=(DWORD)n->bytes;output_size=0;hash_value=2166136261U;
        output_data=allocate(0,output_capacity?output_capacity:1);
        check(output_data!=0,"allocate destination");
        if(!output_data)return -1;
        outputs=1;return 2;
    }
    if(kind==3){
        close_notices++;
        check(n->file==2 && outputs==1,"close notification handle");
        check(output_size==output_capacity,"complete file bytes");
        text("FDI CONTENT ");text(case_name);text(" ");text(n->name);
        text(" size=");hex(output_size);text(" fnv=");hex(hash_value);
        text(" execute=");hex(n->bytes);text(" attr=");hex(n->attributes);text("\n");
        if(mode==0){
            char path[260];unsigned p=0;
            const char *part="out-";while(*part)path[p++]=*part++;
            part=case_name;while(*part && p<250)path[p++]=*part++;
            path[p++]='-';part=n->name;
            while(*part && p<259){char c=*part++;path[p++]=(c=='/'||c=='\\')?'_':c;}
            path[p]=0;
            void *file=open_file(path,0x40000000U,7,0,2,0x80,0);
            DWORD written=0;
            check(file!=(void *)(UP)-1 && WriteFile(file,output_data,output_size,&written,0) && written==output_size,"save extracted fixture");
            if(file!=(void *)(UP)-1)close_file(file);
        }
        fdi_close(2);
        return mode==4?0:mode==5?-1:1;
    }
    check(0,"unexpected spanned-cabinet callback");return -1;
}
static void run_case(const char *label,const char *file,DWORD action)
{
    ERF error={0};case_name=label;mode=action;
    inputs=outputs=cab_notices=copy_notices=close_notices=enum_notices=0;
    void *context=make_fdi(fdi_alloc,fdi_free,fdi_open,fdi_read,fdi_write,fdi_close,fdi_seek,-1,&error);
    check(context!=0,"create context");if(!context)return;
    int result=copy_fdi(context,(char *)file,"",0,fdi_notify,0,(void *)(UP)0x123abc);
    text("FDI ROW ");text(label);text(" result=");hex(result);
    text(" oper=");hex(error.oper);text(" type=");hex(error.type);text(" error=");hex(error.error);
    text(" cab=");hex(cab_notices);text(" copy=");hex(copy_notices);
    text(" close=");hex(close_notices);text(" enum=");hex(enum_notices);text("\n");
    check(inputs==0 && outputs==0,"copy releases file handles");
    check(destroy_fdi(context),"destroy context");
    check(active_allocations==0,"context releases callback memory");
}
void cabinet_fdi_entry(void)
{
    void *cabinet=LoadLibraryA("cabinet.dll"),*kernel=LoadLibraryA("kernel32.dll");
#define LOAD(dst,mod,name) dst=(__typeof__(dst))GetProcAddress(mod,name)
    LOAD(make_fdi,cabinet,"FDICreate");LOAD(copy_fdi,cabinet,"FDICopy");LOAD(destroy_fdi,cabinet,"FDIDestroy");
    LOAD(allocate,kernel,"LocalAlloc");LOAD(release,kernel,"LocalFree");LOAD(open_file,kernel,"CreateFileA");
    LOAD(read_file,kernel,"ReadFile");LOAD(close_file,kernel,"CloseHandle");LOAD(seek_file,kernel,"SetFilePointer");
    LOAD(command_line,kernel,"GetCommandLineW");
    LOAD(module_path,kernel,"GetModuleFileNameA");LOAD(create_process,kernel,"CreateProcessA");
    LOAD(wait_for,kernel,"WaitForSingleObject");LOAD(exit_code,kernel,"GetExitCodeProcess");
    check(make_fdi && copy_fdi && destroy_fdi && allocate && release && open_file && read_file && close_file && seek_file && command_line,"exports");
    check(module_path && create_process && wait_for && exit_code,"process exports");
    if(failures)ExitProcess(1);
    check((void *)make_fdi==GetProcAddress(cabinet,(const char *)(UP)20),"FDICreate ordinal 20");
    check((void *)copy_fdi==GetProcAddress(cabinet,(const char *)(UP)22),"FDICopy ordinal 22");
    check((void *)destroy_fdi==GetProcAddress(cabinet,(const char *)(UP)23),"FDIDestroy ordinal 23");
    const WORD *line=command_line();
    for(unsigned i=0;line && line[i];i++)if(line[i]==' ' && line[i+1]=='b' && line[i+2]=='u' && line[i+3]=='s' && line[i+4]=='y'){
        run_case("abandon-copy","payload1.cab",10);
        check(0,"exit from write callback");ExitProcess(1);
    }
    for(unsigned i=0;line && line[i];i++)if(line[i]==' ' && line[i+1]=='l' && line[i+2]=='e' && line[i+3]=='a' && line[i+4]=='k'){
        static ERF errors[48];
        for(unsigned j=0;j<48;j++)check(make_fdi(fdi_alloc,fdi_free,fdi_open,fdi_read,fdi_write,fdi_close,fdi_seek,-1,&errors[j])!=0,"abandoned context");
        text("FDI LEAK DONE\n");ExitProcess(failures?1:0);
    }
    static const char *labels[]={"stored","skip","abort-cab","abort-copy","abort-close-zero","abort-close-minus","short-write","read-error","stop-enum","abort-enum"};
    for(unsigned action=0;action<10;action++)run_case(labels[action],"stored.cab",action);
    run_case("mszip","mszip.cab",0);
    run_case("bad-checksum","bad-checksum.cab",0);
    run_case("missing","missing.cab",0);
    run_case("truncated","truncated.cab",0);
    run_case("p0","payload0.cab",0);
    run_case("p1","payload1.cab",0);
    run_case("minimum","minimum.cab",0);
    ERF parent_error={0};
    void *parent=make_fdi(fdi_alloc,fdi_free,fdi_open,fdi_read,fdi_write,fdi_close,fdi_seek,-1,&parent_error);
    check(parent!=0,"parent context survives child exit");
    char executable[512];check(module_path(0,executable,sizeof(executable))!=0,"child executable path");
    for(unsigned i=0;i<8;i++){
        char args[540];unsigned p=0,n=0;args[p++]='"';
        while(executable[n] && p<520)args[p++]=executable[n++];args[p++]='"';
        const char *suffix=i<4?" leak":" busy";while(*suffix)args[p++]=*suffix++;args[p]=0;
        BYTE startup[sizeof(UP)==8?104:68]={0};*(DWORD *)startup=sizeof(startup);
        PROCESS child={0};DWORD code=1;
        int made=create_process(executable,args,0,0,0,0,0,0,startup,&child);
        check(made,"create FDI owner child");
        if(!made)continue;
        check(wait_for(child.process,30000)==0 && exit_code(child.process,&code) && code==0,"child releases abandoned FDI resources");
        check(close_file(child.thread) && close_file(child.process),"child handles closed");
        case_name="parent-survives";mode=1;
        check(parent && copy_fdi(parent,"stored.cab","",0,fdi_notify,0,(void *)(UP)0x123abc),"parent context remains usable");
        check(!inputs && !outputs,"parent copy releases file handles");
    }
    check(parent && destroy_fdi(parent),"destroy surviving parent context");
    check(active_allocations==0,"surviving parent frees cached callback memory");
    for(unsigned i=0;i<160;i++){
        ERF error={0};void *context=make_fdi(fdi_alloc,fdi_free,fdi_open,fdi_read,fdi_write,fdi_close,fdi_seek,-1,&error);
        check(context && destroy_fdi(context),"reuse context slot");
    }
    ERF error={0};fail_alloc=1;
    void *failed=make_fdi(fdi_alloc,fdi_free,fdi_open,fdi_read,fdi_write,fdi_close,fdi_seek,-1,&error);
    check(failed==0 && error.oper==5 && error.error,"allocation failure contract");
    if(failed)destroy_fdi(failed);
    check(active_allocations==0,"all callback allocations released");
    text("FDI DONE checks=");hex(checks);text(" failures=");hex(failures);text("\n");
    ExitProcess(failures?1:0);
}
