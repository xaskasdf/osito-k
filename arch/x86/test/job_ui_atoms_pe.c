/* Run the same job atom namespace contract on Windows and OsitoK, PE32/PE64. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef void *HANDLE;
typedef __SIZE_TYPE__ SIZE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
void *memset(void *out,int c,SIZE n)
{ for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=(BYTE)c;return out; }
void *memcpy(void *out,const void *in,SIZE n)
{ for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=((const BYTE *)in)[i];return out; }
typedef struct {
    DWORD cb; char *reserved,*desktop,*title;
    DWORD x,y,w,h,xc,yc,fill,flags; WORD show,bytes; BYTE *data;
    HANDLE input,output,error;
} STARTUP;
typedef struct { HANDLE process,thread; DWORD pid,tid; } PROCESS;
static HANDLE (CALL *create_job)(void *,const WORD *);
static BOOL (CALL *set_job)(HANDLE,DWORD,const void *,DWORD);
static BOOL (CALL *query_job)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *assign_job)(HANDLE,HANDLE);
static BOOL (CALL *close_handle)(HANDLE);
static BOOL (CALL *terminate_process)(HANDLE,DWORD);
static BOOL (CALL *dup_handle)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static DWORD (CALL *wait_object)(HANDLE,DWORD);
static BOOL (CALL *exit_code)(HANDLE,DWORD *);
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static DWORD (CALL *resume_thread)(HANDLE);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static DWORD (CALL *get_pid)(void);
static char *(CALL *command_line)(void);
static WORD (CALL *global_add_a)(const char *);
static WORD (CALL *global_add_w)(const WORD *);
static WORD (CALL *global_find_a)(const char *);
static WORD (CALL *global_find_w)(const WORD *);
static WORD (CALL *global_delete)(WORD);
static DWORD (CALL *global_name_a)(WORD,char *,int);
static DWORD (CALL *global_name_w)(WORD,WORD *,int);
static WORD (CALL *local_add)(const char *);
static WORD (CALL *local_find)(const char *);
static WORD (CALL *local_delete)(WORD);
static DWORD failures;
static char path[320];
static char names[4][64];
static void run(HANDLE job,DWORD root,DWORD stage);
static DWORD root_pid;

static void report(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void hex(DWORD n)
{ char s[9];for(DWORD i=0;i<8;i++)s[i]="0123456789abcdef"[(n>>(28-4*i))&15];s[8]=0;report(s); }
static void check(BOOL ok,const char *label)
{
    if(!ok){DWORD error=GetLastError();failures++;report("JOB-UI-ATOM FAIL: ");
        report(label);report(" error=");hex(error);report("\n");}
}
static BOOL equal(const char *a,const char *b)
{ while(*a&&*a==*b){a++;b++;}return *a==*b; }
static DWORD token(const char *cmd,char key)
{
    for(;*cmd;cmd++)if(cmd[0]==' '&&cmd[1]==key&&cmd[2]==':'){
        DWORD value=0;cmd+=3;
        for(DWORD i=0;i<8;i++){
            char c=cmd[i];DWORD digit=c>='0'&&c<='9'?(DWORD)(c-'0'):
                c>='a'&&c<='f'?(DWORD)(c-'a'+10):16;
            if(digit>15)return value;
            value=(value<<4)|digit;
        }
        return value;
    }
    return 0;
}
static void put_hex(char *out,DWORD n)
{ for(DWORD i=0;i<8;i++)out[i]="0123456789abcdef"[(n>>(28-4*i))&15]; }
static void make_names(DWORD root)
{
    for(DWORD n=0;n<4;n++){
        char *out=names[n];const char *prefix="OsitoK.JobAtoms.";DWORD i=0;
        while(*prefix)out[i++]=*prefix++;
        put_hex(out+i,root);i+=8;out[i++]='.';out[i++]=(char)('0'+n);out[i]=0;
    }
}
static void wide_name(DWORD index,WORD out[64])
{ DWORD i=0;do{out[i]=(BYTE)names[index][i];}while(names[index][i++]); }
static void lookup(DWORD index,BOOL present)
{
    WORD wide[64],buffer_w[64];char buffer[64];wide_name(index,wide);
    WORD atom=global_find_a(names[index]);
    check((atom!=0)==present,"ANSI lookup respects namespace");
    check(global_find_w(wide)==atom,"Unicode lookup uses same namespace");
    wide[0]='o';
    check(global_find_w(wide)==atom,"case-insensitive lookup uses selected namespace");
    wide[0]='O';
    if(atom&&present){
        check(global_name_a(atom,buffer,64)>0&&equal(buffer,names[index]),
              "ANSI name belongs to selected namespace");
        DWORD length=global_name_w(atom,buffer_w,64),i=0;
        while(wide[i]&&wide[i]==buffer_w[i])i++;
        check(length>0&&wide[i]==buffer_w[i],"Unicode name belongs to selected namespace");
    }
}
static void child(DWORD stage)
{
    DWORD mask=0,expected=stage==4?0:0x20;
    SetLastError(1234);
    check(query_job(0,4,&mask,4,0)&&mask==expected&&GetLastError()==1234,
          "child sees current job UI state");
    if(stage==4){lookup(0,1);lookup(1,0);lookup(2,0);return;}
    lookup(0,0);
    check(local_find(names[3])==0,"local atoms do not leak across processes");
    WORD local=local_add(names[3]);
    check(local!=0&&local_find(names[3])==local,"local atom table stays usable");
    if(stage==1){
        lookup(1,0);lookup(2,0);
        WORD wide[64];wide_name(1,wide);
        WORD atom=global_add_a(names[1]);
        check(atom!=0&&global_add_w(wide)==atom,"job global add references shared A/W entry");
        check(global_delete(atom)==0,"release one job atom reference");
        lookup(1,1);
        /* Leave one private global and one local entry for lifetime checks. */
    }else if(stage==2){
        lookup(1,1);lookup(2,0);
        WORD atom=global_find_a(names[1]);
        check(global_delete(atom)==0,"delete inherited job atom");lookup(1,0);
        check(global_add_a(names[1])!=0,"recreate job atom");
        check(local_delete(local)==0,"delete local atom independently");
    }else if(stage==3){
        lookup(1,0);lookup(2,0);
        check(global_add_a(names[2])!=0,"second job private atom");lookup(2,1);
    }else if(stage==5){
        lookup(1,1);lookup(2,0);
    }else if(stage==6){
        lookup(1,0);lookup(2,1);
    }else if(stage==7){
        lookup(1,0);lookup(2,0);
        char bulk[]="OsitoK.JobAtoms.bulk.00";
        for(DWORD i=0;i<64;i++){
            bulk[sizeof(bulk)-3]="0123456789abcdef"[i>>4];
            bulk[sizeof(bulk)-2]="0123456789abcdef"[i&15];
            check(global_add_a(bulk)!=0,"fresh job atom capacity after destruction");
        }
    }else if(stage==8){
        run(0,root_pid,2);
        lookup(1,1);
        check(local_find(names[3])==local,"inherited job does not merge local atom tables");
    }
    check(global_add_a((const char *)(SIZE)123)==123&&
          global_find_w((const WORD *)(SIZE)123)==123&&global_delete(123)==0,
          "integer atoms do not allocate a namespace entry");
}
static BOOL spawn(HANDLE job,DWORD root,DWORD stage,PROCESS *pi)
{
    char cmd[]="job-ui-atoms p:00000000 s:00000000";
    put_hex(cmd+15,root);put_hex(cmd+26,stage);
    STARTUP startup={0};startup.cb=sizeof(startup);memset(pi,0,sizeof(*pi));
    check(create_process(path,cmd,0,0,0,4,0,0,&startup,pi),"create suspended namespace child");
    if(!pi->process)return 0;
    BOOL assigned=job?assign_job(job,pi->process):1;
    check(assigned,"assign namespace child to job");
    if(!assigned){
        terminate_process(pi->process,99);wait_object(pi->process,5000);
        close_handle(pi->thread);close_handle(pi->process);pi->process=0;
    }
    return assigned;
}
static void finish(PROCESS *pi)
{
    if(!pi->process)return;
    check(resume_thread(pi->thread)==1,"resume namespace child");
    DWORD wait=wait_object(pi->process,30000);check(wait==0,"namespace child completes");
    if(wait!=0){terminate_process(pi->process,99);wait_object(pi->process,5000);}
    DWORD code=0xffffffff;
    check(exit_code(pi->process,&code)&&code==0,"namespace child exit status");
    close_handle(pi->thread);close_handle(pi->process);
}
static void run(HANDLE job,DWORD root,DWORD stage)
{
    PROCESS pi={0};
    if(spawn(job,root,stage,&pi))finish(&pi);
}
static BOOL configure(HANDLE job,DWORD mask)
{
    SetLastError(1234);
    BOOL ok=set_job(job,4,&mask,4);
    check(ok,"configure global atom UI limit");
    if(!ok)return 0;
    check(GetLastError()==1234,"UI setter preserves last error");
    DWORD readback=~mask,returned=0;
    check(query_job(job,4,&readback,4,&returned)&&readback==mask&&returned==4&&
          GetLastError()==1234,"UI query returns mask and preserves last error");
    return 1;
}
static void contract(DWORD root)
{
    HANDLE a=create_job(0,0),b=create_job(0,0);
    check(a&&b,"create independent jobs");
    if(!a||!b)goto done;
    if(!configure(a,0x20)||!configure(b,0x20))goto done;
    DWORD mask=0x80000020,readback=0;
    check(!set_job(a,4,&mask,4)&&GetLastError()==87,"unknown UI bits rejected");
    mask=0;
    check(!set_job(a,4,&mask,3)&&GetLastError()==24,"short UI setter rejected");
    check(!set_job(a,4,(void *)1,4)&&GetLastError()==998,"unreadable UI mask rejected");
    check(query_job(a,4,&readback,4,0)&&readback==0x20,"failed setters preserve UI state");
    if(token(command_line(),'u')){
        mask=0x28;
        check(set_job(a,4,&mask,4),"system-parameter and atom restrictions combined");
        mask=0xff;
        check(set_job(a,4,&mask,4),"full basic UI mask accepted");
        check(query_job(a,4,&readback,4,0)&&readback==0xff,"full basic UI mask readback");
        mask=0x20;
        check(set_job(a,4,&mask,4),"restore atom-only restriction");
        mask=0;
    }
    HANDLE query_only=0,self=(HANDLE)(__INTPTR_TYPE__)-1;
    check(dup_handle(self,a,self,&query_only,4,0,0),"duplicate query-only job");
    if(query_only){
        check(!set_job(query_only,4,&mask,4)&&GetLastError()==5,"setting UI requires attributes access");
        close_handle(query_only);
    }
    WORD global=global_add_a(names[0]),local=local_add(names[3]);
    check(global&&local,"parent atoms outside jobs");
    run(a,root,1);run(a,root,2);run(a,root,8);run(b,root,3);
    lookup(0,1);lookup(1,0);lookup(2,0);
    check(local_find(names[3])==local,"children cannot change parent's local table");
    if(configure(a,0))run(a,root,4);
    if(configure(a,0x20))run(a,root,5);
    configure(a,0);
    close_handle(a);a=0;
    run(b,root,6);
    HANDLE held=create_job(0,0);check(held!=0,"create process-retained job");
    if(held){
        PROCESS first={0},second={0};
        if(configure(held,0x20)){
            spawn(held,root,1,&first);spawn(held,root,2,&second);
        }
        close_handle(held);
        finish(&first);finish(&second);
    }
    global_delete(global);local_delete(local);
    for(DWORD i=0;i<10;i++){
        HANDLE job=create_job(0,0);check(job!=0,"create replacement atom job");
        if(job){if(configure(job,0x20))run(job,root,7);close_handle(job);}
    }
done:
    if(a)close_handle(a);
    if(b)close_handle(b);
}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll");
    check(LoadLibraryA("user32.dll")!=0,"initialize USER atom services");
#define LOAD(variable,name) variable=GetProcAddress(k,name);if(!variable){report("JOB-UI-ATOM MISSING " name "\n");ExitProcess(3);}
    LOAD(create_job,"CreateJobObjectW");LOAD(set_job,"SetInformationJobObject");
    LOAD(query_job,"QueryInformationJobObject");LOAD(assign_job,"AssignProcessToJobObject");
    LOAD(close_handle,"CloseHandle");LOAD(dup_handle,"DuplicateHandle");
    LOAD(wait_object,"WaitForSingleObject");LOAD(exit_code,"GetExitCodeProcess");
    LOAD(create_process,"CreateProcessA");LOAD(resume_thread,"ResumeThread");
    LOAD(module_name,"GetModuleFileNameA");LOAD(command_line,"GetCommandLineA");
    LOAD(get_pid,"GetCurrentProcessId");LOAD(terminate_process,"TerminateProcess");
    LOAD(global_add_a,"GlobalAddAtomA");LOAD(global_add_w,"GlobalAddAtomW");
    LOAD(global_find_a,"GlobalFindAtomA");LOAD(global_find_w,"GlobalFindAtomW");
    LOAD(global_delete,"GlobalDeleteAtom");LOAD(global_name_a,"GlobalGetAtomNameA");
    LOAD(global_name_w,"GlobalGetAtomNameW");LOAD(local_add,"AddAtomA");
    LOAD(local_find,"FindAtomA");LOAD(local_delete,"DeleteAtom");
#undef LOAD
    module_name(0,path,sizeof(path));
    DWORD stage=token(command_line(),'s'),root=token(command_line(),'p');
    if(!stage)root=get_pid();
    root_pid=root;
    make_names(root);
    if(stage)child(stage);else contract(root);
    if(!stage)report(failures?"JOB-UI-ATOM FAILED\n":"JOB-UI-ATOM PASS\n");
    ExitProcess(failures?1:0);
}
