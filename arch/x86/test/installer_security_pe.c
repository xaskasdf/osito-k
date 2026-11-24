/* Shared native/guest oracle for the security APIs imported by VC2013 setup. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);

typedef struct { BYTE revision, count, authority[6]; DWORD sub[2]; } SID;
typedef struct { void *sid; DWORD attributes; } GROUP;
typedef struct { BYTE revision, reserved; WORD control; void *owner, *group, *sacl, *dacl; } SD;
static SID world = {1,1,{0,0,0,0,0,1},{0}};
static SID unrelated = {1,1,{0,0,0,0,0,5},{99}};
static DWORD failures, checks, rows;
static BOOL (CALL *membership)(HANDLE,void *,BOOL *);
static BOOL (CALL *set_owner)(void *,void *,BOOL);
static BOOL (CALL *set_group)(void *,void *,BOOL);
static BOOL (CALL *initialize_sd)(void *,DWORD);
static BOOL (CALL *open_token)(HANDLE,DWORD,HANDLE *);
static BOOL (CALL *duplicate_token)(HANDLE,DWORD,void *,DWORD,DWORD,HANDLE *);
static BOOL (CALL *token_info)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *restrict_token)(HANDLE,DWORD,DWORD,GROUP *,DWORD,void *,DWORD,GROUP *,HANDLE *);
static BOOL (CALL *set_thread_token)(HANDLE *,HANDLE);
static BOOL (CALL *revert)(void);
static BOOL (CALL *close_handle)(HANDLE);

static void say(const char *s)
{
    DWORD length=0,written;
    while(s[length])length++;
    WriteFile(GetStdHandle((DWORD)-11),s,length,&written,0);
}
static void hex(DWORD value)
{
    char text[9];
    for(DWORD i=0;i<8;i++)text[i]="0123456789abcdef"[(value>>(28-4*i))&15];
    text[8]=0;say(text);
}
static void check(BOOL good,const char *label)
{
    checks++;
    say(good?"INSTALLER-SECURITY PASS ":"INSTALLER-SECURITY FAIL ");
    say(label);say("\n");if(!good)failures++;
}
static void member_case(const char *label,HANDLE token,void *sid)
{
    BOOL member=0x76543210;
    SetLastError(0x1234);
    BOOL good=membership(token,sid,&member);
    DWORD error=GetLastError();
    say("INSTALLER-SECURITY ROW ");say(label);say(" result=");hex(good);
    say(" error=");hex(error);say(" member=");hex(member);say("\n");rows++;
}
static void descriptor_case(const char *label,SD *sd,BOOL owner,void *sid,BOOL defaulted)
{
    SetLastError(0x1234);
    BOOL good=owner?set_owner(sd,sid,defaulted):set_group(sd,sid,defaulted);
    DWORD error=GetLastError();
    say("INSTALLER-SECURITY ROW ");say(label);say(" result=");hex(good);
    say(" error=");hex(error);say(" control=");hex(sd->control);
    say(" equal=");hex((owner?sd->owner:sd->group)==sid);say("\n");rows++;
}
void CALL installer_security_entry(void)
{
    HANDLE advapi=LoadLibraryA("advapi32.dll"),kernel=LoadLibraryA("kernel32.dll");
#define LOAD(name,symbol,module) do{*(void **)(&name)=GetProcAddress(module,symbol);check(name!=0,symbol);}while(0)
    LOAD(membership,"CheckTokenMembership",advapi);
    LOAD(set_owner,"SetSecurityDescriptorOwner",advapi);
    LOAD(set_group,"SetSecurityDescriptorGroup",advapi);
    LOAD(initialize_sd,"InitializeSecurityDescriptor",advapi);
    LOAD(open_token,"OpenProcessToken",advapi);
    LOAD(duplicate_token,"DuplicateTokenEx",advapi);
    LOAD(token_info,"GetTokenInformation",advapi);
    LOAD(restrict_token,"CreateRestrictedToken",advapi);
    LOAD(set_thread_token,"SetThreadToken",advapi);
    LOAD(revert,"RevertToSelf",advapi);
    LOAD(close_handle,"CloseHandle",kernel);
    if(failures)ExitProcess(1);
    struct { __UINTPTR_TYPE__ before; SD value; __UINTPTR_TYPE__ after; } storage;
    storage.before=storage.after=0x87654321U;
    SD *sd=&storage.value;
    check(initialize_sd(sd,1),"initialize descriptor");
    descriptor_case("owner-default",sd,1,&world,1);
    check(sd->owner==&world && sd->control==1,"owner references SID");
    descriptor_case("group-default",sd,0,&unrelated,7);
    check(sd->group==&unrelated && sd->owner==&world && sd->control==3,"group independent of owner");
    descriptor_case("owner-replace",sd,1,&unrelated,0);
    descriptor_case("group-clear",sd,0,0,0);
    sd->control|=0x5400;
    descriptor_case("owner-clear-default",sd,1,0,1);
    descriptor_case("group-clear-default",sd,0,0,1);
    check(sd->owner==0 && sd->group==0 && sd->control==0x5403,"null identity and unrelated flags");
    sd->revision=2;
    descriptor_case("owner-bad-revision",sd,1,&world,0);
    descriptor_case("group-bad-revision",sd,0,&world,0);
    sd->revision=1;sd->control|=0x8000;
    descriptor_case("owner-relative",sd,1,&world,0);
    descriptor_case("group-relative",sd,0,&world,0);
    check(storage.before==0x87654321U && storage.after==0x87654321U,"descriptor buffer bounds");

    HANDLE primary=0;
    check(open_token((HANDLE)(__INTPTR_TYPE__)-1,0xf01ff,&primary),"open primary token");
    if(!primary)ExitProcess(1);
    BYTE user_buffer[256];DWORD needed=0;
    check(token_info(primary,1,user_buffer,sizeof(user_buffer),&needed),"query token user");
    if(failures)ExitProcess(1);
    void *user=((GROUP *)(void *)user_buffer)->sid;
    member_case("effective-user",0,user);
    member_case("effective-world",0,&world);
    member_case("effective-unrelated",0,&unrelated);
    member_case("primary",primary,&world);
    member_case("bad-handle",(HANDLE)(__UINTPTR_TYPE__)0x12345678,&world);
    for(DWORD level=0;level<4;level++){
        HANDLE token=0;
        check(duplicate_token(primary,0xf01ff,0,level,2,&token),"duplicate impersonation token");
        if(!token)continue;
        char label[]="level-0-world";label[6]=(char)('0'+level);
        member_case(label,token,&world);
        if(level==2){
            member_case("explicit-user",token,user);
            member_case("explicit-unrelated",token,&unrelated);
            HANDLE no_query=0;
            check(duplicate_token(token,2,0,2,2,&no_query),"duplicate without query");
            if(no_query){member_case("no-query",no_query,&world);check(close_handle(no_query),"close no-query");}
            GROUP disable={&world,0};HANDLE denied=0;
            check(restrict_token(token,0,1,&disable,0,0,0,0,&denied),"world deny-only token");
            if(denied){member_case("deny-only-world",denied,&world);member_case("deny-only-user",denied,user);check(close_handle(denied),"close deny-only");}
            GROUP restriction={&world,0};HANDLE restricted=0;
            check(restrict_token(token,0,0,0,0,0,1,&restriction,&restricted),"world restricted token");
            if(restricted){
                member_case("restricted-world",restricted,&world);
                member_case("restricted-user",restricted,user);
                check(set_thread_token(0,restricted),"impersonate restricted token");
                member_case("effective-restricted-world",0,&world);
                member_case("effective-restricted-user",0,user);
                check(revert(),"revert restricted token");
                check(close_handle(restricted),"close restricted");
            }
            restriction.sid=&unrelated;restricted=0;
            check(restrict_token(token,0,0,0,0,0,1,&restriction,&restricted),"unrelated restricting SID");
            if(restricted){member_case("unrelated-restriction-world",restricted,&world);member_case("unrelated-restriction-unrelated",restricted,&unrelated);check(close_handle(restricted),"close unrelated restriction");}
            restriction.sid=&world;restricted=0;
            check(restrict_token(token,8,0,0,0,0,1,&restriction,&restricted),"write-restricted token");
            if(restricted){member_case("write-restricted-world",restricted,&world);member_case("write-restricted-user",restricted,user);check(close_handle(restricted),"close write-restricted");}
        }
        check(close_handle(token),"close impersonation token");
    }
    check(close_handle(primary),"close primary token");
    say("INSTALLER-SECURITY DONE checks=");hex(checks);say(" rows=");hex(rows);say(" failures=");hex(failures);say("\n");
    ExitProcess(failures?1:0);
}
