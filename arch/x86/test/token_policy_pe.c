/* Token policy contracts run unchanged on Windows and OsitoK. */
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
{ for (__SIZE_TYPE__ i=0;i<n;i++) ((BYTE *)out)[i]=((const BYTE *)in)[i]; return out; }
void *memset(void *out,int c,__SIZE_TYPE__ n)
{ for (__SIZE_TYPE__ i=0;i<n;i++) ((BYTE *)out)[i]=(BYTE)c; return out; }
typedef struct { BYTE revision,count,authority[6]; DWORD sub; } SID;
typedef struct { BYTE revision,reserved; WORD size,count,reserved2; BYTE aces[120]; } ACL;
typedef struct {
    DWORD cb; char *reserved,*desktop,*title;
    DWORD x,y,w,h,xc,yc,fill,flags; WORD show,bytes; BYTE *data;
    HANDLE input,output,error;
} STARTUP;
typedef struct { HANDLE process,thread; DWORD pid,tid; } PROCESS;
static SID world={1,1,{0,0,0,0,0,1},0};
static BOOL (CALL *open_token)(HANDLE,DWORD,HANDLE *);
static BOOL (CALL *dup_token)(HANDLE,DWORD,void *,DWORD,DWORD,HANDLE *);
static BOOL (CALL *get_info)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *set_info)(HANDLE,DWORD,void *,DWORD);
static BOOL (CALL *restrict_token)(HANDLE,DWORD,DWORD,void *,DWORD,void *,DWORD,void *,HANDLE *);
static BOOL (CALL *close_handle)(HANDLE);
static BOOL (CALL *dup_handle)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static HANDLE (CALL *create_thread)(void *,__SIZE_TYPE__,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
static DWORD (CALL *wait_object)(HANDLE,DWORD);
static BOOL (CALL *exit_code)(HANDLE,DWORD *);
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static DWORD (CALL *resume_thread)(HANDLE);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static char *(CALL *command_line)(void);
static void (CALL *sleep_ms)(DWORD);
static DWORD failures;
static DWORD workers_ready,workers_go;
static BYTE original[4096],user_buffer[128];
static BYTE large_acl[65532],large_query[65560];
static ACL policy_a,policy_b;
static void *user;
static HANDLE concurrent;

static void report(const char *s)
{ DWORD n=0,written; while(s[n])n++; WriteFile(GetStdHandle((DWORD)-11),s,n,&written,0); }
static void hex(DWORD n)
{ char s[9]; for(DWORD i=0;i<8;i++)s[i]="0123456789abcdef"[(n>>(28-4*i))&15];s[8]=0;report(s); }
static void check(BOOL ok,const char *s)
{ if(!ok){failures++;report("TOKEN-POLICY FAIL: ");report(s);report(" error=");hex(GetLastError());report("\n");} }
static BOOL same(const void *a,const void *b,DWORD n)
{ for(DWORD i=0;i<n;i++)if(((const BYTE *)a)[i]!=((const BYTE *)b)[i])return 0;return 1; }
static void make_acl(ACL *acl,DWORD mask)
{
    memset(acl,0,sizeof(*acl)); acl->revision=2; acl->size=8; acl->count=1;
    DWORD sid_size=8+4*((BYTE *)user)[1];
    BYTE *ace=acl->aces; *(WORD *)(ace+2)=8+sid_size; *(DWORD *)(ace+4)=mask;
    memcpy(ace+8,user,sid_size); acl->size+=8+sid_size;
}
static BOOL set_dacl(HANDLE token,ACL *acl)
{ return set_info(token,6,&acl,sizeof(acl)); }
static void expect_dacl(HANDLE token,const ACL *expected,const char *label)
{
    BYTE buffer[512]; DWORD needed=0;
    memset(buffer,0xcc,sizeof(buffer));
    BOOL ok=get_info(token,6,buffer,sizeof(buffer),&needed);
    if(!ok){check(0,label);return;}
    ACL *acl=*(ACL **)buffer;
    DWORD size=expected?expected->size:0;
    check(needed==sizeof(void *)+size,label);
    if(!expected){check(!acl,label);return;}
    check((BYTE *)acl==buffer+sizeof(void *),"ACL is inside output buffer");
    if((BYTE *)acl!=buffer+sizeof(void *))return;
    check(same(acl,expected,size),label);
    memset(acl,0,size); /* Queries must never return pointers into token storage. */
}
static void rejected(HANDLE token,void *data,DWORD size,DWORD expected,const char *label)
{
    SetLastError(0x1234);
    BOOL result=set_info(token,6,data,size); DWORD error=GetLastError();
    report(label);report(" result=");hex(result);report(" error=");hex(error);report("\n");
    check(!result&&error==expected,label);
}
static DWORD CALL writer(void *unused)
{
    (void)unused;
    __atomic_add_fetch(&workers_ready,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&workers_go,__ATOMIC_ACQUIRE))sleep_ms(0);
    for(DWORD i=0;i<2048;i++) {
        if(!set_dacl(concurrent,i&1?&policy_a:&policy_b))return 1;
        if(!(i&31))sleep_ms(0);
    }
    return 0;
}
static DWORD CALL reader(void *unused)
{
    (void)unused;
    __atomic_add_fetch(&workers_ready,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&workers_go,__ATOMIC_ACQUIRE))sleep_ms(0);
    for(DWORD i=0;i<2048;i++) {
        BYTE buffer[512];DWORD needed;
        HANDLE copy=0;
        if(!(i&31)&&!dup_token(concurrent,0,0,2,1,&copy))return 4;
        BOOL ok=get_info(copy?copy:concurrent,6,buffer,sizeof(buffer),&needed);
        if(copy)close_handle(copy);
        if(!ok)return 2;
        ACL *acl=*(ACL **)buffer;
        if((BYTE *)acl!=buffer+sizeof(void *)||needed!=sizeof(void *)+policy_a.size||
           (!same(acl,&policy_a,policy_a.size)&&!same(acl,&policy_b,policy_b.size)))return 3;
        if(!(i&31))sleep_ms(0);
    }
    return 0;
}
static BOOL child_mode(void)
{
    const char *p=command_line(),*needle="--policy-child";
    for(;*p;p++){DWORD i=0;while(needle[i]&&p[i]==needle[i])i++;if(!needle[i])return 1;}
    return 0;
}
static void process_policy(HANDLE primary)
{
    DWORD needed;
    check(get_info(primary,6,original,sizeof(original),&needed),"save process DACL");
    check(set_dacl(primary,&policy_a),"set process DACL");
    HANDLE reopened=0;
    check(open_token(SELF,0x88,&reopened),"reopen current process token");
    expect_dacl(reopened,&policy_a,"same process opens share token");
    char path[320],command[]="token-policy --policy-child";
    STARTUP startup={0}; PROCESS pi={0}; startup.cb=sizeof(startup);
    check(module_name(0,path,sizeof(path))!=0,"query executable path");
    BOOL created=create_process(path,command,0,0,0,4,0,0,&startup,&pi);
    check(created,"create suspended child with inherited primary token");
    if(created) {
        HANDLE child=0;
        check(open_token(pi.process,0x88,&child),"open child's primary token");
        expect_dacl(child,&policy_a,"child inherits DACL at creation");
        check(set_dacl(primary,&policy_b),"change parent's DACL");
        expect_dacl(child,&policy_a,"child token independent of parent mutation");
        ACL empty={2,0,8,0,0,{0}};
        check(set_dacl(child,&empty),"change child token through process handle");
        expect_dacl(primary,&policy_b,"child mutation does not affect parent");
        check(resume_thread(pi.thread)==1,"resume child");
        DWORD code=0xffffffff;
        check(wait_object(pi.process,30000)==0&&exit_code(pi.process,&code)&&code==0,
              "child observes its own token");
        close_handle(child); child=0;
        check(open_token(pi.process,8,&child),"exited process handle retains primary token");
        expect_dacl(child,&empty,"exited process token query");
        close_handle(pi.thread);close_handle(pi.process);
        expect_dacl(child,&empty,"token handle outlives process handles");close_handle(child);
    }
    ACL *saved=*(ACL **)original;
    check(set_dacl(primary,saved),"restore original process DACL");
    if(reopened)close_handle(reopened);
}
void mainCRTStartup(void)
{
    HANDLE a=LoadLibraryA("advapi32.dll"),k=LoadLibraryA("kernel32.dll");
    open_token=GetProcAddress(a,"OpenProcessToken"); dup_token=GetProcAddress(a,"DuplicateTokenEx");
    get_info=GetProcAddress(a,"GetTokenInformation"); set_info=GetProcAddress(a,"SetTokenInformation");
    restrict_token=GetProcAddress(a,"CreateRestrictedToken");
    close_handle=GetProcAddress(k,"CloseHandle"); dup_handle=GetProcAddress(k,"DuplicateHandle");
    create_thread=GetProcAddress(k,"CreateThread");wait_object=GetProcAddress(k,"WaitForSingleObject");
    exit_code=GetProcAddress(k,"GetExitCodeProcess");create_process=GetProcAddress(k,"CreateProcessA");
    resume_thread=GetProcAddress(k,"ResumeThread");module_name=GetProcAddress(k,"GetModuleFileNameA");
    command_line=GetProcAddress(k,"GetCommandLineA");
    sleep_ms=GetProcAddress(k,"Sleep");
    if(!open_token||!dup_token||!get_info||!set_info||!restrict_token||!close_handle||!dup_handle||
       !create_thread||!wait_object||!exit_code||!create_process||!resume_thread||!module_name||!command_line||!sleep_ms)
        {report("TOKEN-POLICY MISSING EXPORT\n");ExitProcess(2);}
    HANDLE primary=0;
    check(open_token(SELF,0x8b,&primary),"open primary");
    if(child_mode()) {
        ACL empty={2,0,8,0,0,{0}};expect_dacl(primary,&empty,"child current token");
        close_handle(primary);ExitProcess(failures?1:0);
    }
    DWORD needed;
    check(get_info(primary,1,user_buffer,sizeof(user_buffer),&needed),"query user");
    user=*(void **)user_buffer;if(!user||!primary)ExitProcess(3);
    make_acl(&policy_a,0x10000000);make_acl(&policy_b,0x10020000);
    HANDLE token=0,alias=0,copy=0,filtered=0;
    check(dup_token(primary,0,0,2,1,&token),"duplicate primary");
    BYTE before[56],after[56];
    check(get_info(token,10,before,sizeof(before),&needed),"statistics before mutation");
    check(set_dacl(token,&policy_a),"set duplicate DACL");
    check(get_info(token,10,after,sizeof(after),&needed)&&same(before,after,8)&&
          !same(before+48,after+48,8),"DACL mutation changes ModifiedId, not TokenId");
    expect_dacl(token,&policy_a,"round-trip DACL");expect_dacl(token,&policy_a,"query is a copy");
    BYTE buffer[512];memset(buffer,0xcc,sizeof(buffer));needed=0;
    check(!get_info(token,6,0,0,&needed)&&GetLastError()==122&&needed==sizeof(void *)+policy_a.size,
          "query size negotiation");
    DWORD short_size=needed-1;
    check(!get_info(token,6,buffer,short_size,&needed)&&GetLastError()==122&&buffer[0]==0xcc,
          "short query preserves buffer");
    ACL guest=policy_a;
    guest.size=sizeof(guest);
    check(set_dacl(token,&guest),"set ACL with spare capacity");
    expect_dacl(token,&guest,"ACL capacity is preserved");
    guest=policy_a;
    check(set_dacl(token,&guest),"set from caller storage");memset(&guest,0,sizeof(guest));
    expect_dacl(token,&policy_a,"setter copies caller storage");
    check(dup_handle(SELF,token,SELF,&alias,0,0,2),"duplicate handle");
    check(dup_token(token,0,0,2,1,&copy),"duplicate token with policy");
    check(get_info(copy,10,before,sizeof(before),&needed)&&!same(before,after,8),
          "token duplicate receives unique TokenId");
    check(restrict_token(token,1,0,0,0,0,0,0,&filtered),"filter token with policy");
    check(set_dacl(alias,&policy_b),"change policy through alias");
    expect_dacl(token,&policy_b,"duplicate handle shares policy");
    expect_dacl(copy,&policy_a,"duplicate token isolates policy");
    expect_dacl(filtered,&policy_a,"restricted token copied policy");
    check(set_dacl(filtered,&policy_b),"restricted token DACL can change");
    expect_dacl(filtered,&policy_b,"restricted token query reflects change");
    check(set_dacl(token,0),"set NULL DACL");expect_dacl(alias,0,"NULL DACL query");
    ACL empty={2,0,8,0,0,{0}};
    check(set_dacl(token,&empty),"set empty DACL");expect_dacl(token,&empty,"empty is not NULL");
    ACL *arg=&policy_a;
    rejected(token,&arg,sizeof(arg)-1,24,"short input");
    rejected(token,0,sizeof(arg),998,"NULL input");
    ACL bad=policy_a;bad.revision=1;arg=&bad;
    rejected(token,&arg,sizeof(arg),1336,"invalid ACL revision");
    bad=policy_a;bad.size=4;rejected(token,&arg,sizeof(arg),1336,"invalid ACL length");
    bad=policy_a;bad.count=30;rejected(token,&arg,sizeof(arg),1336,"ACE exceeds ACL");
    bad=policy_a;*(WORD *)(bad.aces+2)=4;
    rejected(token,&arg,sizeof(arg),1336,"allow ACE missing SID");
    bad=policy_a;bad.aces[8]=0;
    rejected(token,&arg,sizeof(arg),1336,"invalid ACE SID revision");
    arg=(ACL *)(__INTPTR_TYPE__)1;rejected(token,&arg,sizeof(arg),998,"inaccessible ACL");
    expect_dacl(token,&empty,"failed updates preserve policy");
    HANDLE query_only=0;
    check(dup_handle(SELF,token,SELF,&query_only,8,0,0),"query-only handle");
    arg=&policy_a;rejected(query_only,&arg,sizeof(arg),5,"requires TOKEN_ADJUST_DEFAULT");
    close_handle(query_only);
    memset(large_acl,0,sizeof(large_acl));memcpy(large_acl,&policy_a,policy_a.size);
    ((ACL *)large_acl)->size=sizeof(large_acl);
    if(set_dacl(token,(ACL *)large_acl)) {
        check(get_info(token,6,large_query,sizeof(large_query),&needed)&&
              needed==sizeof(void *)+sizeof(large_acl)&&
              same(*(void **)large_query,large_acl,sizeof(large_acl)),"large ACL exact round-trip");
        report("TOKEN-POLICY large ACL allocated\n");
    } else {
        /* Windows token dynamic-pool quotas differ from this kernel's heap.
         * Resource rejection must be explicit and leave the policy intact. */
        check(GetLastError()==1344,"large ACL rejected only by token quota");
        expect_dacl(token,&empty,"quota rejection preserves policy");
        report("TOKEN-POLICY large ACL exceeds token quota\n");
    }
    check(set_dacl(token,&policy_a),"initial concurrent policy"); concurrent=token;
    HANDLE w=create_thread(0,0,writer,0,0,0),r=create_thread(0,0,reader,0,0,0);
    DWORD (CALL *thread_code)(HANDLE,DWORD *)=GetProcAddress(k,"GetExitCodeThread");
    check(w&&r,"start concurrent policy workers");
    if(w&&r)while(__atomic_load_n(&workers_ready,__ATOMIC_ACQUIRE)!=2)sleep_ms(0);
    __atomic_store_n(&workers_go,1,__ATOMIC_RELEASE);
    HANDLE threads[]={w,r};
    for(DWORD i=0;i<2;i++)if(threads[i]) {
        DWORD code=0xffffffff;
        check(wait_object(threads[i],30000)==0&&thread_code(threads[i],&code)&&!code,
              "concurrent read/write snapshot");close_handle(threads[i]);
    }
    process_policy(primary);
    close_handle(filtered);close_handle(copy);close_handle(alias);close_handle(token);close_handle(primary);
    report(failures?"TOKEN-POLICY FAILED\n":"TOKEN-POLICY PASS\n");ExitProcess(failures?1:0);
}
