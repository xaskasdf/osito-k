/* Run unchanged on Windows and OsitoK. Mutate copies or a suspended child,
 * never the host test process's primary token. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define SELF ((HANDLE)(__INTPTR_TYPE__)-1)
#define SENTINEL 0x13572468U
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
void *memcpy(void *out,const void *in,__SIZE_TYPE__ n)
{ for(__SIZE_TYPE__ i=0;i<n;i++)((BYTE *)out)[i]=((const BYTE *)in)[i];return out; }
void *memset(void *out,int c,__SIZE_TYPE__ n)
{ for(__SIZE_TYPE__ i=0;i<n;i++)((BYTE *)out)[i]=(BYTE)c;return out; }
typedef struct { BYTE revision,count,authority[6]; DWORD sub[15]; } SID;
typedef struct { void *sid; DWORD attributes; } LABEL;
typedef struct { DWORD count; LABEL entries[1]; } GROUPS;
typedef struct {
    DWORD cb; char *reserved,*desktop,*title;
    DWORD x,y,w,h,xc,yc,fill,flags; WORD show,bytes; BYTE *data;
    HANDLE input,output,error;
} STARTUP;
typedef struct { HANDLE process,thread; DWORD pid,tid; } PROCESS;
static SID world={1,1,{0,0,0,0,0,1},{0}};
static BOOL (CALL *open_token)(HANDLE,DWORD,HANDLE *);
static BOOL (CALL *dup_token)(HANDLE,DWORD,void *,DWORD,DWORD,HANDLE *);
static BOOL (CALL *get_info)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *set_info)(HANDLE,DWORD,void *,DWORD);
static BOOL (CALL *restrict_token)(HANDLE,DWORD,DWORD,void *,DWORD,void *,DWORD,void *,HANDLE *);
static BOOL (CALL *close_handle)(HANDLE);
static BOOL (CALL *dup_handle)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static DWORD (CALL *resume_thread)(HANDLE);
static DWORD (CALL *wait_object)(HANDLE,DWORD);
static BOOL (CALL *exit_code)(HANDLE,DWORD *);
static BOOL (CALL *terminate)(HANDLE,DWORD);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static char *(CALL *command_line)(void);
static DWORD failures;
static void report(const char *s)
{ DWORD n=0,written;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&written,0); }
static void hex(DWORD n)
{ char s[9];for(DWORD i=0;i<8;i++)s[i]="0123456789abcdef"[(n>>(28-4*i))&15];s[8]=0;report(s); }
static void check(BOOL ok,const char *name)
{ if(!ok){failures++;report("TOKEN-INTEGRITY FAIL: ");report(name);report(" error=");hex(GetLastError());report("\n");} }
static BOOL same(const void *a,const void *b,DWORD n)
{ for(DWORD i=0;i<n;i++)if(((const BYTE *)a)[i]!=((const BYTE *)b)[i])return 0;return 1; }
static DWORD level(HANDLE token)
{
    BYTE buffer[256];DWORD needed=0;
    SetLastError(SENTINEL);
    BOOL ok=get_info(token,25,buffer,sizeof(buffer),&needed);
    DWORD error=GetLastError();
    check(ok,"query label");if(!ok)return 0xffffffff;
    LABEL *label=(void *)buffer;SID *sid=label->sid;
    check(needed==sizeof(LABEL)+12&&label->attributes==0x60&&
          (BYTE *)sid==buffer+sizeof(LABEL),"label ABI and owned output");
    check(error==SENTINEL,"query success preserves LastError");
    if((BYTE *)sid!=buffer+sizeof(LABEL))return 0xffffffff;
    check(sid->revision==1&&sid->count==1&&sid->authority[5]==16,"normalized integrity SID");
    DWORD rid=sid->sub[0];memset(buffer,0,sizeof(buffer));return rid;
}
static void expect_level(HANDLE token,DWORD want,const char *name)
{ DWORD got=level(token);if(got!=want){report("RID got/want=");hex(got);report("/");hex(want);report("\n");}check(got==want,name); }
static void expect_set(HANDLE token,void *input,DWORD size,DWORD error,const char *name)
{
    SetLastError(SENTINEL);
    BOOL ok=set_info(token,25,input,size);DWORD got=GetLastError();
    report(name);report(" result=");hex(ok);report(" error=");hex(got);report("\n");
    check(ok==(error==0)&&got==(error?error:SENTINEL),name);
}
static void lower(HANDLE token,DWORD rid,const char *name)
{ SID sid={1,1,{0,0,0,0,0,16},{rid}};LABEL label={&sid,0x20};expect_set(token,&label,sizeof(label),0,name); }
static HANDLE copy_of(HANDLE token)
{ HANDLE copy=0;check(dup_token(token,0,0,2,1,&copy),"duplicate token");return copy; }
static void groups_match(HANDLE token,DWORD rid)
{
    static BYTE buffer[4096];
    BYTE stats[56];DWORD needed;
    BOOL ok=get_info(token,2,buffer,sizeof(buffer),&needed);
    check(ok,"query groups");if(!ok)return;
    GROUPS *groups=(void *)buffer;DWORD found=0;
    if(groups->count>128){check(0,"bounded group count");return;}
    for(DWORD i=0;i<groups->count;i++) {
        LABEL *label=&groups->entries[i];SID *sid=label->sid;
        if(label->attributes&0x20) {
            found++;
            check(label->attributes==0x60&&sid->count==1&&sid->authority[5]==16&&
                  sid->sub[0]==rid,"TokenGroups agrees with TokenIntegrityLevel");
        }
    }
    check(found==1,"exactly one integrity group");
    check(get_info(token,10,stats,sizeof(stats),&needed)&&
          *(DWORD *)(stats+40)==groups->count,"statistics includes integrity group");
}
static BOOL child_mode(void)
{
    const char *p=command_line(),*needle="--integrity-child";
    for(;*p;p++){DWORD i=0;while(needle[i]&&p[i]==needle[i])i++;if(!needle[i])return 1;}
    return 0;
}
static void child_lifetime(HANDLE primary,DWORD original)
{
    char path[320],command[]="token-integrity --integrity-child";
    STARTUP startup={0};PROCESS pi={0};startup.cb=sizeof(startup);
    check(module_name(0,path,sizeof(path))!=0,"executable path");
    BOOL created=create_process(path,command,0,0,0,4,0,0,&startup,&pi);
    check(created,"suspended child");if(!created)return;
    HANDLE child=0;
    check(open_token(pi.process,0x88,&child),"selected child token");
    lower(child,0x1000,"lower selected child");
    expect_level(primary,original,"child mutation leaves parent unchanged");
    check(resume_thread(pi.thread)==1,"resume child");
    DWORD code=0xffffffff;
    BOOL done=wait_object(pi.process,30000)==0;
    check(done&&exit_code(pi.process,&code)&&code==0,"child observes label");
    if(!done){terminate(pi.process,1);check(wait_object(pi.process,10000)==0,"terminate test child");}
    close_handle(pi.thread);close_handle(pi.process);
    expect_level(child,0x1000,"token survives selected process exit");close_handle(child);
}
void mainCRTStartup(void)
{
    HANDLE a=LoadLibraryA("advapi32.dll"),k=LoadLibraryA("kernel32.dll");
    open_token=GetProcAddress(a,"OpenProcessToken");dup_token=GetProcAddress(a,"DuplicateTokenEx");
    get_info=GetProcAddress(a,"GetTokenInformation");set_info=GetProcAddress(a,"SetTokenInformation");
    restrict_token=GetProcAddress(a,"CreateRestrictedToken");close_handle=GetProcAddress(k,"CloseHandle");
    dup_handle=GetProcAddress(k,"DuplicateHandle");create_process=GetProcAddress(k,"CreateProcessA");
    resume_thread=GetProcAddress(k,"ResumeThread");wait_object=GetProcAddress(k,"WaitForSingleObject");
    exit_code=GetProcAddress(k,"GetExitCodeProcess");terminate=GetProcAddress(k,"TerminateProcess");
    module_name=GetProcAddress(k,"GetModuleFileNameA");command_line=GetProcAddress(k,"GetCommandLineA");
    if(!open_token||!dup_token||!get_info||!set_info||!restrict_token||!close_handle||!dup_handle||
       !create_process||!resume_thread||!wait_object||!exit_code||!terminate||!module_name||!command_line)
        {report("TOKEN-INTEGRITY MISSING EXPORT\n");ExitProcess(2);}
    BOOL child=child_mode();
    HANDLE primary=0;check(open_token(SELF,child?8:0x8b,&primary),"open primary");
    if(!primary)ExitProcess(3);
    if(child) {
        expect_level(primary,0x1000,"child current process label");groups_match(primary,0x1000);
        close_handle(primary);ExitProcess(failures?1:0);
    }
    DWORD original=level(primary);check(original>=0x2000,"reference process is at least medium");
    if(original<0x2000)ExitProcess(3);
    HANDLE token=copy_of(primary),alias=0,copy=0,filtered=0;
    BYTE before[56],after[56];DWORD needed;
    check(get_info(token,10,before,sizeof(before),&needed),"statistics before");
    lower(token,0x1000,"lower primary copy");
    if(level(token)!=0x1000) {
        report("TOKEN-INTEGRITY FAIL: missing label state\n");
        close_handle(token);close_handle(primary);ExitProcess(1);
    }
    check(get_info(token,10,after,sizeof(after),&needed)&&same(before,after,8)&&
          !same(before+48,after+48,8),"label mutation updates ModifiedId, not TokenId");
    expect_level(token,0x1000,"round trip");expect_level(token,0x1000,"query returns copy");
    check(dup_handle(SELF,token,SELF,&alias,0,0,2),"alias handle");copy=copy_of(token);
    LABEL restricting={&world,0};
    check(restrict_token(token,1,0,0,0,0,1,&restricting,&filtered),"restricted token");
    expect_level(filtered,0x1000,"restricted token copies label");
    lower(alias,0,"lower through alias");expect_level(token,0,"alias shares label");
    expect_level(copy,0x1000,"token copy isolates label");
    lower(filtered,0,"lower restricted token");expect_level(filtered,0,"restricted label persists");
    groups_match(token,0);groups_match(copy,0x1000);groups_match(filtered,0);
    SID sid={1,1,{0,0,0,0,0,16},{original}};LABEL label={&sid,0x20};
    expect_set(token,&label,sizeof(label),1314,"cannot raise even to caller level");
    expect_level(token,0,"failed raise preserves label");
    DWORD attrs[]={0,1,2,4,8,0x10,0x20,0x40,0x60,0x20000000,0x80000000};
    sid.sub[0]=0x1001;
    for(DWORD i=0;i<sizeof(attrs)/sizeof(attrs[0]);i++) {
        HANDLE fresh=copy_of(primary);label.attributes=attrs[i];
        expect_set(fresh,&label,sizeof(label),0,"accepted attributes");
        expect_level(fresh,0x1001,"intermediate RID and normalized attrs");close_handle(fresh);
    }
    HANDLE fresh=copy_of(primary);label.attributes=0x80;
    expect_set(fresh,&label,sizeof(label),87,"unknown attributes");
    expect_level(fresh,original,"invalid attributes preserve label");
    label.attributes=0x20;
    expect_set(fresh,&label,sizeof(label)-1,24,"short input");
    expect_set(fresh,0,sizeof(label),998,"null input");
    expect_set(fresh,(void *)(__INTPTR_TYPE__)1,sizeof(label),998,"inaccessible input");
    label.sid=0;expect_set(fresh,&label,sizeof(label),998,"null SID");
    label.sid=(void *)(__INTPTR_TYPE__)1;expect_set(fresh,&label,sizeof(label),998,"inaccessible SID");
    label.sid=&sid;sid.revision=0;expect_set(fresh,&label,sizeof(label),1337,"bad SID revision");sid.revision=1;
    sid.authority[5]=5;expect_set(fresh,&label,sizeof(label),1299,"not an integrity SID");sid.authority[5]=16;
    expect_level(fresh,original,"invalid SID preserves label");
    expect_set(fresh,&label,sizeof(label)+12,0,"oversized input");expect_level(fresh,0x1001,"oversized label stored");
    sid.count=2;sid.sub[1]=0x1000;
    expect_set(fresh,&label,sizeof(label),0,"normalize final subauthority");expect_level(fresh,0x1000,"last RID");
    sid.count=0;expect_set(fresh,&label,sizeof(label),0,"no subauthority becomes untrusted");expect_level(fresh,0,"zero RID");
    close_handle(fresh);sid.count=1;sid.sub[0]=0;
    HANDLE query=0,adjust=0,impersonation=0;
    check(dup_handle(SELF,token,SELF,&query,8,0,0),"query-only alias");
    check(dup_handle(SELF,token,SELF,&adjust,0x80,0,0),"adjust-only alias");
    expect_set(query,&label,sizeof(label),5,"requires TOKEN_ADJUST_DEFAULT");
    expect_set(adjust,&label,sizeof(label),0,"TOKEN_ADJUST_DEFAULT alone suffices");
    expect_set((HANDLE)(__INTPTR_TYPE__)0x123400,&label,sizeof(label),6,"invalid handle");
    check(dup_token(primary,0,0,2,2,&impersonation),"duplicate impersonation token");
    lower(impersonation,0x1000,"lower impersonation token");expect_level(impersonation,0x1000,"impersonation label");
    BYTE output[128];memset(output,0xcc,sizeof(output));needed=0;
    check(!get_info(token,25,0,0,&needed)&&GetLastError()==122&&needed==sizeof(LABEL)+12,"query length negotiation");
    check(!get_info(token,25,output,needed-1,&needed)&&GetLastError()==122&&output[0]==0xcc,"short query preserves buffer");
    check(!get_info(token,25,(void *)(__INTPTR_TYPE__)1,needed,&needed)&&GetLastError()==998,"bad query buffer");
    check(!get_info(token,25,output,sizeof(output),(DWORD *)(__INTPTR_TYPE__)1)&&GetLastError()==998,"bad query length pointer");
    close_handle(query);close_handle(adjust);close_handle(impersonation);close_handle(token);
    expect_level(alias,0,"alias outlives original handle");close_handle(alias);close_handle(copy);close_handle(filtered);
    for(DWORD i=0;i<3;i++)child_lifetime(primary,original);
    expect_level(primary,original,"caller token unchanged");close_handle(primary);
    report(failures?"TOKEN-INTEGRITY FAIL\n":"TOKEN-INTEGRITY PASS\n");ExitProcess(failures?1:0);
}
