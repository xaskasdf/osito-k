/* Selected primary-token creation, also run unchanged on native Windows. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
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
API void CALL SetLastError(DWORD);
void *memcpy(void *out,const void *in,__SIZE_TYPE__ n)
{for(__SIZE_TYPE__ i=0;i<n;i++)((BYTE *)out)[i]=((const BYTE *)in)[i];return out;}
void *memset(void *out,int c,__SIZE_TYPE__ n)
{for(__SIZE_TYPE__ i=0;i<n;i++)((BYTE *)out)[i]=(BYTE)c;return out;}
typedef struct { BYTE revision,count,authority[6]; DWORD sub[2]; } SID;
typedef struct { void *sid; DWORD flags; } GROUP;
typedef struct { DWORD count; GROUP groups[1]; } GROUPS;
typedef struct { BYTE revision,reserved; WORD size,count,reserved2; BYTE ace[120]; } ACL;
typedef struct { BYTE revision,reserved; WORD control; void *owner,*group,*sacl,*dacl; } SD;
typedef struct {
    DWORD cb; WORD *reserved,*desktop,*title;
    DWORD x,y,w,h,xc,yc,fill,flags; WORD show,bytes; BYTE *data;
    HANDLE input,output,error;
} STARTUP;
typedef struct { HANDLE process,thread; DWORD pid,tid; } PROCESS;
typedef struct { DWORD size; void *descriptor; BOOL inherit; } ATTR;
static BOOL (CALL *open_token)(HANDLE,DWORD,HANDLE *);
static BOOL (CALL *dup_token)(HANDLE,DWORD,void *,DWORD,DWORD,HANDLE *);
static BOOL (CALL *get_info)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *set_info)(HANDLE,DWORD,void *,DWORD);
static BOOL (CALL *restrict_token)(HANDLE,DWORD,DWORD,void *,DWORD,void *,DWORD,void *,HANDLE *);
static BOOL (CALL *is_restricted)(HANDLE);
static BOOL (CALL *access_check)(void *,HANDLE,DWORD,void *,void *,DWORD *,DWORD *,BOOL *);
static BOOL (CALL *set_thread_token)(HANDLE *,HANDLE);
static BOOL (CALL *open_thread_token)(HANDLE,DWORD,BOOL,HANDLE *);
static BOOL (CALL *revert_self)(void);
static BOOL (CALL *close_handle)(HANDLE);
static BOOL (CALL *dup_handle)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static DWORD (CALL *wait_object)(HANDLE,DWORD);
static BOOL (CALL *exit_code)(HANDLE,DWORD *);
static BOOL (CALL *create_as_user)(HANDLE,const WORD *,WORD *,void *,void *,BOOL,DWORD,void *,const WORD *,void *,void *);
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static DWORD (CALL *resume_thread)(HANDLE);
static BOOL (CALL *terminate_process)(HANDLE,DWORD);
static BOOL (CALL *set_handle_info)(HANDLE,DWORD,DWORD);
static BOOL (CALL *get_handle_info)(HANDLE,DWORD *);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static char *(CALL *command_line)(void);
static DWORD failures;
static BYTE user_buffer[128];
static void *user;
static SID world={1,1,{0,0,0,0,0,1},{0,0}};
static SID users={1,2,{0,0,0,0,0,5},{32,545}};
static SID authenticated={1,1,{0,0,0,0,0,5},{11,0}};
static char path[320];
static WORD path_w[320];
static ACL policy_a,policy_b;
static void report(const char *s)
{DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void hex(DWORD n)
{char s[9];for(DWORD i=0;i<8;i++)s[i]="0123456789abcdef"[(n>>(28-4*i))&15];s[8]=0;report(s);}
static void check(BOOL ok,const char *s)
{if(!ok){failures++;report("PROCESS-TOKEN FAIL: ");report(s);report(" error=");hex(GetLastError());report("\n");}}
static BOOL same(const void *a,const void *b,DWORD n)
{for(DWORD i=0;i<n;i++)if(((const BYTE *)a)[i]!=((const BYTE *)b)[i])return 0;return 1;}
static BOOL sid_equal(const void *a,const void *b)
{return ((BYTE *)a)[1]==((BYTE *)b)[1]&&same(a,b,8+4*((BYTE *)a)[1]);}
static void wide(WORD *out,const char *in)
{do{*out++=(BYTE)*in;}while(*in++);}
static const char *find(const char *s,const char *needle)
{for(;*s;s++){DWORD i=0;while(needle[i]&&s[i]==needle[i])i++;if(!needle[i])return s+i;}return 0;}
static void make_acl(ACL *acl,void *sid,DWORD mask)
{
    memset(acl,0,sizeof(*acl));acl->revision=2;acl->count=1;
    DWORD n=8+4*((BYTE *)sid)[1];acl->size=16+n;
    *(WORD *)(acl->ace+2)=8+n;*(DWORD *)(acl->ace+4)=mask;memcpy(acl->ace+8,sid,n);
}
static void expect_dacl(HANDLE token,const ACL *expected,const char *label)
{
    BYTE buf[512];DWORD n=0;
    BOOL ok=get_info(token,6,buf,sizeof(buf),&n);check(ok,label);
    if(ok){ACL *acl=*(ACL **)buf;check(acl&&acl->size==expected->size&&same(acl,expected,expected->size),label);}
}
static void identity(HANDLE token,BOOL restricted)
{
    BYTE buf[2048];DWORD n=0,type=0;
    check(get_info(token,8,&type,4,&n)&&type==1,"child token is primary");
    check(is_restricted(token)==restricted,"selected restricting SIDs retained");
    if(!restricted)return;
    BOOL ok=get_info(token,11,buf,sizeof(buf),&n);check(ok,"query child restricting SIDs");
    if(ok){GROUPS *g=(void *)buf;check(g->count==2&&sid_equal(g->groups[0].sid,user)&&
        sid_equal(g->groups[1].sid,&users),"child has selected restricting SIDs");}
    ok=get_info(token,2,buf,sizeof(buf),&n);check(ok,"query child groups");
    if(ok){GROUPS *g=(void *)buf;BOOL found=0;for(DWORD i=0;i<g->count;i++)
        if(sid_equal(g->groups[i].sid,&world)){found=1;check(g->groups[i].flags==0x10,"disabled group remains deny-only");}
        check(found,"world group retained");}
    HANDLE imp=0;check(dup_token(token,8,0,2,2,&imp),"derive impersonation token from child identity");
    if(imp){
        SD sd={1,0,4,&world,&world,0,0};ACL acl;DWORD mapping[4]={1,2,4,7};BYTE privileges[64];
        DWORD size=sizeof(privileges),granted=0;BOOL allowed=1;
        make_acl(&acl,&authenticated,1);sd.dacl=&acl;
        check(access_check(&sd,imp,1,mapping,privileges,&size,&granted,&allowed)&&!allowed,
              "restricting SID denies authenticated-users-only access");
        make_acl(&acl,user,1);size=sizeof(privileges);allowed=0;
        check(access_check(&sd,imp,1,mapping,privileges,&size,&granted,&allowed)&&allowed&&granted==1,
              "restricting SID permits explicit user access");close_handle(imp);
    }
}
static BOOL launch(HANDLE token,const char *mode,DWORD flags,BOOL inherit,void *pa,void *ta,PROCESS *pi)
{
    WORD command[256];wide(command,mode);STARTUP startup={0};startup.cb=sizeof(startup);
    startup.flags=0x100;startup.input=GetStdHandle((DWORD)-10);
    startup.output=GetStdHandle((DWORD)-11);startup.error=GetStdHandle((DWORD)-12);
    return create_as_user(token,path_w,command,pa,ta,inherit,flags|8,0,0,&startup,pi);
}
static void finish(PROCESS *pi,const char *label)
{
    DWORD code=0xffffffff;
    DWORD wait=wait_object(pi->process,30000);
    if(wait!=0){terminate_process(pi->process,0xee);wait_object(pi->process,5000);}
    BOOL got=exit_code(pi->process,&code);
    if(wait!=0||!got||code){report("Child wait/exit=");hex(wait);report("/");hex(code);report("\n");}
    check(wait==0&&got&&code==0,label);
    close_handle(pi->thread);close_handle(pi->process);
}
static void start_restricted(PROCESS *pi,HANDLE primary)
{
    /* Like a sandbox broker, allow loader startup before entry reverts. */
    HANDLE initial=0,filtered=0;
    GROUP initial_sids[]={{user,0},{&users,0},{&world,0},{&authenticated,0}};
    check(restrict_token(primary,1,0,0,0,0,4,initial_sids,&filtered),"filter initial token privileges");
    check(dup_token(filtered,0xc,0,2,2,&initial),"create initial thread token");
    close_handle(filtered);
    check(set_thread_token(&pi->thread,initial),"set initial thread identity");
    close_handle(initial);
    HANDLE actual=0;DWORD level=0,n=0;
    check(open_thread_token(pi->thread,8,1,&actual)&&get_info(actual,9,&level,4,&n)&&level==2,
          "initial thread token keeps impersonation level");
    if(level!=2){report("Initial impersonation level=");hex(level);report("\n");}
    close_handle(actual);
    check(resume_thread(pi->thread)==1,"resume selected child");
}
static void rejected(HANDLE token,DWORD error,const char *label)
{
    PROCESS pi={0};BOOL result=launch(token,"process-token --plain-child",4,0,0,0,&pi);
    DWORD got=GetLastError();report(label);report(" result/error=");hex(result);report("/");hex(got);report("\n");
    check(!result&&got==error,label);
    if(result){terminate_process(pi.process,0xee);wait_object(pi.process,5000);close_handle(pi.thread);close_handle(pi.process);}
}
void mainCRTStartup(void)
{
    HANDLE a=LoadLibraryA("advapi32.dll"),k=LoadLibraryA("kernel32.dll");
#define LOAD(module,variable,name) variable=GetProcAddress(module,name);if(!variable){report("PROCESS-TOKEN MISSING " name "\n");ExitProcess(2);}
    LOAD(a,open_token,"OpenProcessToken");LOAD(a,dup_token,"DuplicateTokenEx");
    LOAD(a,get_info,"GetTokenInformation");LOAD(a,set_info,"SetTokenInformation");
    LOAD(a,restrict_token,"CreateRestrictedToken");LOAD(a,is_restricted,"IsTokenRestricted");
    LOAD(a,access_check,"AccessCheck");LOAD(a,create_as_user,"CreateProcessAsUserW");
    LOAD(a,set_thread_token,"SetThreadToken");LOAD(a,revert_self,"RevertToSelf");
    LOAD(a,open_thread_token,"OpenThreadToken");
    LOAD(k,close_handle,"CloseHandle");LOAD(k,dup_handle,"DuplicateHandle");
    LOAD(k,wait_object,"WaitForSingleObject");LOAD(k,exit_code,"GetExitCodeProcess");
    LOAD(k,create_process,"CreateProcessA");LOAD(k,resume_thread,"ResumeThread");
    LOAD(k,terminate_process,"TerminateProcess");LOAD(k,module_name,"GetModuleFileNameA");
    LOAD(k,command_line,"GetCommandLineA");LOAD(k,set_handle_info,"SetHandleInformation");
    LOAD(k,get_handle_info,"GetHandleInformation");
#undef LOAD
    HANDLE primary=0;DWORD needed;
    check(open_token(SELF,0x8b,&primary),"open primary");
    check(get_info(primary,1,user_buffer,sizeof(user_buffer),&needed),"get user SID");
    user=*(void **)user_buffer;if(!primary||!user)ExitProcess(3);
    check(module_name(0,path,sizeof(path))!=0,"get executable path");wide(path_w,path);
    make_acl(&policy_a,user,0x10000000);make_acl(&policy_b,user,0x1fffff);
    const char *cmd=command_line();
    if(find(cmd,"--plain-child")){identity(primary,0);close_handle(primary);ExitProcess(failures?1:0);}
    if(find(cmd,"--restricted-child")||find(cmd,"--grandchild")){
        check(revert_self(),"child reverts to selected primary identity");
        identity(primary,1);
        const char *parent_text=find(cmd,"--restricted-child ");
        if(parent_text){
            __UINTPTR_TYPE__ handle=0;
            for(DWORD i=0;i<sizeof(handle)*2;i++){
                BYTE c=(BYTE)parent_text[i];handle=(handle<<4)|(c<='9'?c-'0':c-'a'+10);
            }
            rejected((HANDLE)handle,1314,"cannot restore unrestricted parent identity");
        }
        if(!find(cmd,"--grandchild")){
            PROCESS pi={0};STARTUP startup={0};startup.cb=sizeof(startup);
            char command[]="process-token --grandchild";
            BOOL ok=create_process(path,command,0,0,0,4|8,0,0,&startup,&pi);
            check(ok,"ordinary child inherits restricted primary");
            if(ok){HANDLE grand=0;check(open_token(pi.process,0xb,&grand),"open grandchild token");
                identity(grand,1);close_handle(grand);terminate_process(pi.process,0);
                finish(&pi,"suspended grandchild cleanup");}
        }
        close_handle(primary);ExitProcess(failures?1:0);
    }
    HANDLE selected=0;GROUP disable={&world,0},restrict_sid[2]={{user,0},{&users,0}};
    check(restrict_token(primary,1,1,&disable,0,0,2,restrict_sid,&selected),"create selected restricted token");
    if(!selected)ExitProcess(3);
    ACL *acl=&policy_a;check(set_info(selected,6,&acl,sizeof(acl)),"set selected default ACL");
    PROCESS pi={0};ATTR attr={sizeof(attr),0,1};
    char child_command[128]="process-token --restricted-child ";DWORD length=0;
    while(child_command[length])length++;
    for(DWORD i=0;i<sizeof(primary)*2;i++)
        child_command[length+i]="0123456789abcdef"[((__UINTPTR_TYPE__)primary>>(sizeof(primary)*8-4-4*i))&15];
    child_command[length+sizeof(primary)*2]=0;
    check(set_handle_info(primary,1,1),"inherit parent token for privilege rejection test");
    BOOL created=launch(selected,child_command,4,1,&attr,&attr,&pi);
    check(set_handle_info(primary,1,0),"clear parent token inheritance");
    check(created,"create process with selected token");
    if(created){
        DWORD flag=0;check(get_handle_info(pi.process,&flag)&&(flag&1),"process handle inheritance flag");
        check(get_handle_info(pi.thread,&flag)&&(flag&1),"thread handle inheritance flag");
        HANDLE child=0;check(open_token(pi.process,0x8b,&child),"open selected child token while suspended");
        identity(child,1);expect_dacl(child,&policy_a,"child inherits selected default ACL");
        BYTE source_stats[56],child_stats[56];
        check(get_info(selected,10,source_stats,sizeof(source_stats),&needed)&&
              get_info(child,10,child_stats,sizeof(child_stats),&needed),"query token IDs");
        report("Selected/child TokenId=");hex(*(DWORD *)source_stats);report("/");hex(*(DWORD *)child_stats);report("\n");
        acl=&policy_b;check(set_info(selected,6,&acl,sizeof(acl)),"mutate selected source after creation");
        expect_dacl(child,&policy_a,"child token snapshots selected source");
        close_handle(selected);selected=0;
        start_restricted(&pi,primary);finish(&pi,"child runs with selected identity");
        identity(child,1);close_handle(child);
    }
    if(selected)close_handle(selected);
    identity(primary,0);
    HANDLE imp=0,query=0;check(dup_token(primary,0xb,0,2,2,&imp),"create impersonation token");
    created=launch(imp,"process-token --plain-child",0,1,0,0,&pi);
    check(created,"convert impersonation token to process primary");
    if(created)finish(&pi,"impersonation source child");close_handle(imp);
    check(dup_token(primary,0xb,0,1,2,&imp),"create identification token");
    rejected(imp,1346,"reject identification token as process source");close_handle(imp);
    check(dup_handle(SELF,primary,SELF,&query,8,0,0),"create query-only handle");
    rejected(query,5,"require assign and duplicate rights");close_handle(query);
    check(dup_handle(SELF,primary,SELF,&query,3,0,0),"create handle without query rights");
    rejected(query,5,"require query rights");close_handle(query);
    rejected((HANDLE)(__INTPTR_TYPE__)0x123456,6,"reject invalid token handle");
    created=launch(0,"process-token --plain-child",0,0,0,0,&pi);
    check(created,"NULL token inherits caller primary");if(created)finish(&pi,"NULL token child");
    for(DWORD i=0;i<12;i++){
        selected=0;check(restrict_token(primary,1,1,&disable,0,0,2,restrict_sid,&selected),"repeat token creation");
        if(!selected)break;
        created=launch(selected,"process-token --grandchild",4,1,0,0,&pi);
        close_handle(selected);check(created,"repeat selected process creation");
        if(created){start_restricted(&pi,primary);finish(&pi,"repeat child exit");}
    }
    close_handle(primary);
    report(failures?"PROCESS-TOKEN FAILED\n":"PROCESS-TOKEN PASS\n");ExitProcess(failures?1:0);
}
