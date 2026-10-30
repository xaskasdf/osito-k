/* Read-only native oracle for legacy installer administrator detection. */
typedef unsigned int DWORD;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
typedef int (CALL *ADMIN)(DWORD,DWORD *);
typedef DWORD (CALL *ERRORFN)(void);
typedef void (CALL *SETERROR)(DWORD);
static ADMIN admin;
static ERRORFN error;
static SETERROR seterror;
static DWORD failures;
static unsigned char groups[65536];
static char executable[1024],child_command[1050];
typedef struct {DWORD cb;void *reserved,*desktop,*title;DWORD x,y,w,h,xc,yc,fill,flags;
    unsigned short show,bytes;void *data,*input,*output,*error;} STARTUP;
typedef struct {void *process,*thread;DWORD pid,tid;} PROCESS;
#define SAY(s) do{DWORD w;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&w,0);}while(0)
#define HEX(v) do{char h[9];DWORD n=(DWORD)(v),w;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[n&15];n>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&w,0);}while(0)
static void admin_probe_check(int ok,const char *name)
{
    if(ok)SAY("ADMIN PASS ");else{SAY("ADMIN FAIL ");failures++;}
    DWORD size=0,w;while(name[size])size++;WriteFile(GetStdHandle((DWORD)-11),name,size,&w,0);SAY("\n");
}
static int admin_probe_run(DWORD reserved,int pointer)
{
    DWORD value=0xcccccccc;seterror(0x13579);
    int result=admin(reserved,pointer?&value:0);DWORD last=error();
    SAY("ADMIN RESULT ");HEX(reserved);HEX(pointer);HEX(result);HEX(last);HEX(value);SAY("\n");
    admin_probe_check(value==0xcccccccc,"reserved-output-untouched");
    return result;
}
void admin_probe_entry(void)
{
    void *a=LoadLibraryA("advapi32.dll"),*k=LoadLibraryA("kernel32.dll"),*p=LoadLibraryA("advpack.dll");
    admin=(ADMIN)GetProcAddress(p,"IsNTAdmin");
    error=(ERRORFN)GetProcAddress(k,"GetLastError");seterror=(SETERROR)GetProcAddress(k,"SetLastError");
    typedef int(CALL *OPEN)(void *,DWORD,void **);
    typedef int(CALL *INFO)(void *,DWORD,void *,DWORD,DWORD *);
    typedef int(CALL *CLOSE)(void *);
    typedef int(CALL *ANON)(void *);
    typedef int(CALL *REVERT)(void);
    typedef int(CALL *FILTER)(void *,DWORD,DWORD,void *,DWORD,void *,DWORD,void *,void **);
    typedef int(CALL *DUP)(void *,DWORD,void *,DWORD,DWORD,void **);
    typedef int(CALL *SET)(void **,void *);
    typedef const char *(CALL *CMD)(void);
    typedef DWORD(CALL *MODULE)(void *,char *,DWORD);
    typedef int(CALL *CREATE)(const char *,char *,void *,void *,int,DWORD,void *,const char *,STARTUP *,PROCESS *);
    typedef DWORD(CALL *WAIT)(void *,DWORD);
    typedef int(CALL *EXITCODE)(void *,DWORD *);
    OPEN open=(OPEN)GetProcAddress(a,"OpenProcessToken");
    INFO info=(INFO)GetProcAddress(a,"GetTokenInformation");
    CLOSE close=(CLOSE)GetProcAddress(k,"CloseHandle");
    ANON anon=(ANON)GetProcAddress(a,"ImpersonateAnonymousToken");
    REVERT revert=(REVERT)GetProcAddress(a,"RevertToSelf");
    REVERT shell_admin=(REVERT)GetProcAddress(LoadLibraryA("shell32.dll"),"IsUserAnAdmin");
    FILTER filter=(FILTER)GetProcAddress(a,"CreateRestrictedToken");
    DUP duplicate=(DUP)GetProcAddress(a,"DuplicateTokenEx");
    SET assign=(SET)GetProcAddress(a,"SetThreadToken");
    CMD command=(CMD)GetProcAddress(k,"GetCommandLineA");
    MODULE module=(MODULE)GetProcAddress(k,"GetModuleFileNameA");
    CREATE create=(CREATE)GetProcAddress(k,"CreateProcessA");
    WAIT wait=(WAIT)GetProcAddress(k,"WaitForSingleObject");
    EXITCODE exitcode=(EXITCODE)GetProcAddress(k,"GetExitCodeProcess");
    admin_probe_check(admin&&error&&seterror&&open&&info&&close&&anon&&revert&&shell_admin&&filter&&duplicate&&assign&&command&&module&&create&&wait&&exitcode,"exports");
    if(failures)ExitProcess(1);
    void *token=0;DWORD bytes=0;
    admin_probe_check(open((void *)(UP)-1,14,&token),"open-process-token");
    admin_probe_check(info(token,2,groups,sizeof(groups),&bytes),"process-groups");
    if(failures)ExitProcess(1);
    DWORD count=*(DWORD *)(void *)groups,found=0,attributes=0;
    for(DWORD i=0;i<count;i++){
        unsigned char *entry=groups+sizeof(UP)+i*2*sizeof(UP);
        const unsigned char *sid=*(const unsigned char **)(void *)entry;
        if(sid[0]==1&&sid[1]==2&&sid[7]==5&&*(const DWORD *)(const void *)(sid+8)==32&&*(const DWORD *)(const void *)(sid+12)==544){
            found=1;attributes=*(DWORD *)(void *)(entry+sizeof(UP));
        }
    }
    DWORD elevation=0,type=0;
    admin_probe_check(info(token,18,&type,4,&bytes)&&info(token,20,&elevation,4,&bytes),"process-elevation");
    SAY("ADMIN PROCESS ");HEX(found);HEX(attributes);HEX(type);HEX(elevation);SAY("\n");
    int first=admin_probe_run(0,1);
    int enabled=found&&(attributes&4)&&!(attributes&0x10);
    admin_probe_check(first==enabled&&shell_admin()==enabled,"enabled-admin-group");
    const char *cmd=command();int child=0,expected=-1;
    for(unsigned i=0;cmd[i];i++){
        if(cmd[i]==' '&&cmd[i+1]=='c'&&cmd[i+2]=='h'&&cmd[i+3]=='i'&&cmd[i+4]=='l'&&cmd[i+5]=='d')child=1;
        if(cmd[i]==' '&&(cmd[i+1]=='0'||cmd[i+1]=='1')&&!cmd[i+2])expected=cmd[i+1]-'0';
    }
    if(expected>=0)admin_probe_check(first==expected,"requested-launch-role");
    if(child){close(token);ExitProcess(failures?99:40+(DWORD)first);}
    admin_probe_check(first==admin_probe_run(0,0),"optional-reserved-pointer");
    admin_probe_check(first==admin_probe_run(0x1234,1),"reserved-input-ignored");
    admin_probe_check(anon((void *)(UP)-2),"anonymous-impersonation");
    int anonymous=admin_probe_run(0,1);
    SAY("ADMIN ANONYMOUS ");HEX(anonymous);HEX(first);SAY("\n");
    admin_probe_check(anonymous==first&&!shell_admin(),"legacy-cache-and-effective-anonymous-token");
    admin_probe_check(revert(),"revert-impersonation");
    admin_probe_check(first==admin_probe_run(0,1),"process-identity-stable");
    static const unsigned char admin_sid[16]={1,2,0,0,0,0,0,5,32,0,0,0,32,2,0,0};
    struct {const void *sid;DWORD attributes;} disabled={admin_sid,0};
    void *filtered=0,*impersonation=0;
    admin_probe_check(filter(token,0,1,&disabled,0,0,0,0,&filtered),"disable-admin-group");
    if(filtered){
        admin_probe_check(duplicate(filtered,12,0,2,2,&impersonation),"duplicate-filtered-impersonation");
        if(impersonation){
            admin_probe_check(assign(0,impersonation),"assign-filtered-token");
            admin_probe_check(!shell_admin(),"disabled-admin-not-effective");
            admin_probe_check(first==admin_probe_run(0,1),"legacy-positive-cache-retained");
            admin_probe_check(revert(),"revert-filtered-token");close(impersonation);
        }
        close(filtered);
    }
    close(token);
    DWORD length=module(0,executable,sizeof(executable));
    admin_probe_check(length&&length<sizeof(executable),"child-executable-path");
    if(length&&length<sizeof(executable)){
        unsigned n=0;child_command[n++]='"';for(unsigned i=0;i<length;i++)child_command[n++]=executable[i];
        child_command[n++]='"';const char *suffix=" child";while(*suffix)child_command[n++]=*suffix++;child_command[n]=0;
        STARTUP startup={0};PROCESS process={0};startup.cb=sizeof(startup);
        int made=create(executable,child_command,0,0,0,0,0,0,&startup,&process);
        admin_probe_check(made,"create-child");
        if(made){DWORD code=0;admin_probe_check(wait(process.process,15000)==0&&exitcode(process.process,&code)&&code==40+(DWORD)first,"child-inherits-launch-role");close(process.thread);close(process.process);}
    }
    if(!failures)SAY("ADMIN DONE\n");ExitProcess(failures?1:0);
}
