/* Empty-token query contracts, unchanged on Windows and OsitoK. */
typedef unsigned int DWORD;
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
static BOOL (CALL *open_token)(HANDLE,DWORD,HANDLE *);
static BOOL (CALL *dup_token)(HANDLE,DWORD,void *,DWORD,DWORD,HANDLE *);
static BOOL (CALL *get_info)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *adjust)(HANDLE,BOOL,void *,DWORD,void *,DWORD *);
static BOOL (CALL *restrict_token)(HANDLE,DWORD,DWORD,void *,DWORD,void *,DWORD,void *,HANDLE *);
static BOOL (CALL *dup_handle)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static BOOL (CALL *close_handle)(HANDLE);
static DWORD failures;
static DWORD original[1024],privileges[1024];
static void report(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void hex(DWORD n)
{ char b[9];for(DWORD i=0;i<8;i++)b[i]="0123456789abcdef"[(n>>(28-4*i))&15];b[8]=0;report(b); }
static void check(BOOL ok,const char *label)
{
    if(ok)return;
    failures++;report("TOKEN-PRIVILEGES FAIL: ");report(label);
    report(" error=");hex(GetLastError());report("\n");
}
static BOOL filled(const BYTE *p,DWORD n,BYTE value)
{ for(DWORD i=0;i<n;i++)if(p[i]!=value)return 0;return 1; }
static void query_empty(HANDLE token,DWORD length,BOOL null_buffer)
{
    DWORD buffer[16],needed=0xcccccccc;
    for(DWORD i=0;i<16;i++)buffer[i]=0xcccccccc;
    SetLastError(0x1234);
    BOOL ok=get_info(token,3,null_buffer?0:buffer+1,length,&needed);
    DWORD error=GetLastError();
    check(needed==16,"empty query requires TOKEN_PRIVILEGES header");
    check(buffer[0]==0xcccccccc,"leading guard");
    if(length<16||null_buffer) {
        check(!ok&&error==122,"short buffer rejected");
        check(filled((BYTE *)buffer,sizeof(buffer),0xcc),"short buffer untouched");
    } else {
        check(ok&&buffer[1]==0,"empty query count");
        check(error==0x1234,"successful query preserves LastError");
        check(filled((BYTE *)(buffer+5),sizeof(buffer)-20,0xcc),"trailing guard");
    }
}
static void empty_sequence(HANDLE token)
{
    DWORD needed=0;
    BOOL ok=get_info(token,3,0,0,&needed);
    check(!ok&&needed>=16,"variable-size query accepted by Chromium");
    if(ok||needed<16||needed>sizeof(privileges))return;
    check(get_info(token,3,privileges,needed,&needed)&&privileges[0]==0,
          "variable-size second query");
    check(adjust(token,0,privileges,needed,0,0),"remove empty privilege set");
    DWORD stats[14];
    check(get_info(token,10,stats,sizeof(stats),&needed)&&stats[11]==0,
          "TokenStatistics privilege count agrees");
}
void mainCRTStartup(void)
{
    HANDLE advapi=LoadLibraryA("advapi32.dll"),kernel=LoadLibraryA("kernel32.dll");
    open_token=(void *)GetProcAddress(advapi,"OpenProcessToken");
    dup_token=(void *)GetProcAddress(advapi,"DuplicateTokenEx");
    get_info=(void *)GetProcAddress(advapi,"GetTokenInformation");
    adjust=(void *)GetProcAddress(advapi,"AdjustTokenPrivileges");
    restrict_token=(void *)GetProcAddress(advapi,"CreateRestrictedToken");
    dup_handle=(void *)GetProcAddress(kernel,"DuplicateHandle");
    close_handle=(void *)GetProcAddress(kernel,"CloseHandle");
    if(!open_token||!dup_token||!get_info||!adjust||!restrict_token||!dup_handle||!close_handle) {
        report("TOKEN-PRIVILEGES FAIL: missing export\n");ExitProcess(1);
    }
    HANDLE source=0,token=0,alias=0,copy=0,restricted=0,no_query=0;
    if(!open_token(SELF,10,&source)||!dup_token(source,0xf01ff,0,2,1,&token)) {
        report("TOKEN-PRIVILEGES FAIL: token setup\n");ExitProcess(1);
    }
    DWORD original_size=0,needed=0;
    BOOL original_ok=get_info(source,3,original,sizeof(original),&original_size);
    check(original_ok,"save current process privileges");
    BOOL ok=get_info(token,3,privileges,sizeof(privileges),&needed);
    check(ok&&privileges[0]<=(sizeof(privileges)-4)/12,"read duplicate privileges");
    if(!ok||privileges[0]>(sizeof(privileges)-4)/12)goto done;
    /* Remove only from a duplicate, never the test runner's process token. */
    for(DWORD i=0;i<privileges[0];i++)privileges[3+3*i]=4;
    check(adjust(token,0,privileges,needed,0,0),"remove duplicate privileges");
    query_empty(token,0,1);
    for(DWORD length=0;length<=32;length++)query_empty(token,length,0);
    empty_sequence(token);
    check(dup_handle(SELF,token,SELF,&alias,0,0,2),"alias empty token");
    check(dup_token(token,0xf01ff,0,2,2,&copy),"copy empty token");
    check(restrict_token(token,1,0,0,0,0,0,0,&restricted),"filter empty token");
    if(alias)empty_sequence(alias);
    if(copy)empty_sequence(copy);
    if(restricted)empty_sequence(restricted);
    check(dup_token(token,2,0,2,1,&no_query),"create no-query token");
    if(no_query) {
        SetLastError(0x1234);
        ok=get_info(no_query,3,privileges,sizeof(privileges),&needed);
        check(!ok&&GetLastError()==5,"query requires TOKEN_QUERY");
    }
    SetLastError(0x1234);
    ok=get_info(0,3,privileges,sizeof(privileges),&needed);
    check(!ok&&GetLastError()==6,"invalid token rejected");
    for(DWORD i=0;i<256;i++)query_empty(token,16,0);
    ok=get_info(source,3,privileges,sizeof(privileges),&needed);
    check(ok&&original_ok&&needed==original_size,"source query unchanged");
    if(ok&&original_ok&&needed==original_size) {
        BOOL same=1;
        for(DWORD i=0;i<1+original[0]*3;i++)if(privileges[i]!=original[i])same=0;
        check(same,"source privileges unchanged");
    }
done:
    if(no_query)check(close_handle(no_query),"close no-query token");
    if(restricted)check(close_handle(restricted),"close filtered token");
    if(copy)check(close_handle(copy),"close copied token");
    if(alias)check(close_handle(alias),"close token alias");
    check(close_handle(token),"close empty token");
    check(close_handle(source),"close source");
    report(failures?"TOKEN-PRIVILEGES FAILED\n":"TOKEN-PRIVILEGES PASS\n");
    ExitProcess(failures?1:0);
}
