/* Original SDK layout; Windows mode only reads the existing EventLog service.
 * Native modes create an inert probe service and never start its executable.
 * Phase 1 leaves the fixture for a reboot; phase 2 verifies and removes it. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>

#ifndef SCM_PHASE
#define SCM_PHASE 0
#endif
_Static_assert(sizeof(QUERY_SERVICE_CONFIGA) == (sizeof(void *) == 8 ? 64 : 36), "A size");
_Static_assert(sizeof(QUERY_SERVICE_CONFIGW) == sizeof(QUERY_SERVICE_CONFIGA), "W size");
_Static_assert(sizeof(((QUERY_SERVICE_CONFIGA *)0)->dwTagId) == 4, "tag is DWORD");
_Static_assert(offsetof(QUERY_SERVICE_CONFIGA, dwTagId) == (sizeof(void *) == 8 ? 32 : 20), "tag offset");
_Static_assert(offsetof(QUERY_SERVICE_CONFIGA, lpDependencies) == (sizeof(void *) == 8 ? 40 : 24), "deps offset");

static SC_HANDLE (WINAPI *manager_a)(LPCSTR,LPCSTR,DWORD);
static SC_HANDLE (WINAPI *open_a)(SC_HANDLE,LPCSTR,DWORD);
static SC_HANDLE (WINAPI *open_w)(SC_HANDLE,LPCWSTR,DWORD);
static SC_HANDLE (WINAPI *create_w)(SC_HANDLE,LPCWSTR,LPCWSTR,DWORD,DWORD,DWORD,DWORD,LPCWSTR,LPCWSTR,LPDWORD,LPCWSTR,LPCWSTR,LPCWSTR);
static BOOL (WINAPI *query_a)(SC_HANDLE,LPQUERY_SERVICE_CONFIGA,DWORD,LPDWORD);
static BOOL (WINAPI *query_w)(SC_HANDLE,LPQUERY_SERVICE_CONFIGW,DWORD,LPDWORD);
static BOOL (WINAPI *change_w)(SC_HANDLE,DWORD,DWORD,DWORD,LPCWSTR,LPCWSTR,LPDWORD,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR);
static BOOL (WINAPI *close_service)(SC_HANDLE);
static BOOL (WINAPI *delete_service)(SC_HANDLE);
static unsigned checks, failures;
static char row[128];
static unsigned used;
static void number(unsigned n)
{
    for(int s=28;s>=0;s-=4) row[used++]="0123456789abcdef"[(n>>s)&15];
    row[used++]=' ';
}
static void emit(const char *tag)
{
    DWORD length=0,written;
    while(tag[length])length++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),tag,length,&written,0);
    row[used-1]='\n';
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),row,used,&written,0);used=0;
}
#define CHECK(x) do {checks++; if(!(x)){failures++;number(__LINE__);emit("SCM FAIL ");}} while(0)
static union { ULONGLONG align; unsigned char bytes[8192]; } a_memory,w_memory;
static void fill(unsigned char *p, unsigned n)
{
    for(unsigned i=0;i<n;i++)p[i]=0x5a;
}
static int unchanged(const unsigned char *p,unsigned start,unsigned end)
{
    for(unsigned i=start;i<end;i++)if(p[i]!=0x5a)return 0;
    return 1;
}
static int inside(const void *p,const unsigned char *base,DWORD size)
{
    return (ULONG_PTR)p>=(ULONG_PTR)base && (ULONG_PTR)p<(ULONG_PTR)base+size;
}
static int text_equal(const char *a,const WCHAR *w)
{
    if(!a||!w)return !a&&!w;
    for(unsigned i=0;i<4096;i++){
        if((unsigned char)a[i]!=w[i])return 0;
        if(!a[i])return 1;
    }
    return 0;
}
static int ascii_equal(const char *a,const char *b)
{
    for(unsigned i=0;i<4096;i++){
        if(a[i]!=b[i])return 0;
        if(!a[i])return 1;
    }
    return 0;
}
static void query_pair(SC_HANDLE service,int empty)
{
    BYTE *ab=a_memory.bytes+16,*wb=w_memory.bytes+16;
    DWORD an=0,wn=0,n=0;
    CHECK(!query_a(service,0,0,&an)&&GetLastError()==ERROR_INSUFFICIENT_BUFFER);
    CHECK(!query_w(service,0,0,&wn)&&GetLastError()==ERROR_INSUFFICIENT_BUFFER);
    CHECK(an>=sizeof(QUERY_SERVICE_CONFIGA)&&an<=8000);
    CHECK(wn>=sizeof(QUERY_SERVICE_CONFIGW)&&wn<=8000);
    if(an>8000||wn>8000||!an||!wn)return;
    /* Windows can overestimate the size-only result. Use a buffer smaller
     * than the header there; the native fixture also tests exact admission. */
    DWORD short_a=SCM_PHASE==3?sizeof(QUERY_SERVICE_CONFIGA)-1:an-1;
    DWORD short_w=SCM_PHASE==3?sizeof(QUERY_SERVICE_CONFIGW)-1:wn-1;
    fill(a_memory.bytes,sizeof(a_memory.bytes));fill(w_memory.bytes,sizeof(w_memory.bytes));
    CHECK(!query_a(service,(void *)ab,short_a,&n)&&GetLastError()==ERROR_INSUFFICIENT_BUFFER);
    CHECK(SCM_PHASE==3?n>=sizeof(QUERY_SERVICE_CONFIGA):n==an);
    number(0);number(n);number(an);number(unchanged(a_memory.bytes,0,sizeof(a_memory.bytes)));emit("SCM SHORT ");
    CHECK(unchanged(a_memory.bytes,0,16)&&unchanged(a_memory.bytes,16+short_a,sizeof(a_memory.bytes)));
    if(SCM_PHASE!=3)CHECK(unchanged(a_memory.bytes,0,sizeof(a_memory.bytes)));
    CHECK(!query_w(service,(void *)wb,short_w,&n)&&GetLastError()==ERROR_INSUFFICIENT_BUFFER);
    CHECK(SCM_PHASE==3?n>=sizeof(QUERY_SERVICE_CONFIGW):n==wn);
    number(1);number(n);number(wn);number(unchanged(w_memory.bytes,0,sizeof(w_memory.bytes)));emit("SCM SHORT ");
    CHECK(unchanged(w_memory.bytes,0,16)&&unchanged(w_memory.bytes,16+short_w,sizeof(w_memory.bytes)));
    if(SCM_PHASE!=3)CHECK(unchanged(w_memory.bytes,0,sizeof(w_memory.bytes)));
    BOOL aok=query_a(service,(void *)ab,an,&n),wok=query_w(service,(void *)wb,wn,&n);
    CHECK(aok&&wok);
    if(!aok||!wok)return;
    CHECK(unchanged(a_memory.bytes,0,16)&&unchanged(a_memory.bytes,16+an,sizeof(a_memory.bytes)));
    CHECK(unchanged(w_memory.bytes,0,16)&&unchanged(w_memory.bytes,16+wn,sizeof(w_memory.bytes)));
    QUERY_SERVICE_CONFIGA *a=(void *)ab;
    QUERY_SERVICE_CONFIGW *w=(void *)wb;
    CHECK(a->dwServiceType==w->dwServiceType);
    CHECK(a->dwStartType==w->dwStartType&&a->dwErrorControl==w->dwErrorControl);
    CHECK(a->dwTagId==0&&w->dwTagId==0);
    const char *ap[5]={a->lpBinaryPathName,a->lpLoadOrderGroup,a->lpDependencies,a->lpServiceStartName,a->lpDisplayName};
    const WCHAR *wp[5]={w->lpBinaryPathName,w->lpLoadOrderGroup,w->lpDependencies,w->lpServiceStartName,w->lpDisplayName};
    for(unsigned i=0;i<5;i++){
        CHECK(inside(ap[i],ab,an)&&inside(wp[i],wb,wn));
        if(!inside(ap[i],ab,an)||!inside(wp[i],wb,wn))return;
    }
    if(SCM_PHASE!=3){
        for(unsigned i=0;i<5;i++)CHECK(text_equal(ap[i],wp[i]));
        CHECK(a->dwServiceType==SERVICE_WIN32_OWN_PROCESS&&a->dwStartType==SERVICE_DEMAND_START);
        CHECK(ascii_equal(a->lpBinaryPathName,"C:\\Diagnostics\\inert-scm-probe.exe"));
        CHECK(ascii_equal(a->lpDisplayName,"Osito SCM probe"));
        CHECK(ascii_equal(a->lpServiceStartName,"LocalSystem"));
        if(empty)CHECK(!a->lpDependencies[0]&&!a->lpDependencies[1]&&!w->lpDependencies[0]&&!w->lpDependencies[1]);
        else {
            const char expected[]="depA\0+groupB\0";
            for(unsigned i=0;i<sizeof(expected);i++)
                CHECK(a->lpDependencies[i]==expected[i]&&w->lpDependencies[i]==(WCHAR)expected[i]);
        }
    }
    number(a->dwTagId==0&&w->dwTagId==0);number(failures);emit("SCM CONFIG ");
}
void scm_config_entry(void)
{
    HMODULE dll=LoadLibraryA("advapi32.dll");
    HMODULE api=LoadLibraryA("api-ms-win-service-winsvc-l1-1-0.dll");
    CHECK(dll&&api);
    if(!dll||!api)ExitProcess(2);
#define GET(var,name) var=(void *)GetProcAddress(dll,name);CHECK(var!=0)
    GET(manager_a,"OpenSCManagerA");GET(open_a,"OpenServiceA");GET(open_w,"OpenServiceW");
    GET(query_a,"QueryServiceConfigA");GET(query_w,"QueryServiceConfigW");
    GET(close_service,"CloseServiceHandle");GET(create_w,"CreateServiceW");
    GET(change_w,"ChangeServiceConfigW");GET(delete_service,"DeleteService");
#undef GET
    CHECK(GetProcAddress(api,"OpenSCManagerA")!=0);
    CHECK(GetProcAddress(api,"OpenServiceA")!=0);
    CHECK(GetProcAddress(api,"QueryServiceConfigA")!=0);
    if(failures)ExitProcess(2);
    /* Invoke all three imports through the exact API-set used by d3d9. */
    manager_a=(void *)GetProcAddress(api,"OpenSCManagerA");
    open_a=(void *)GetProcAddress(api,"OpenServiceA");
    query_a=(void *)GetProcAddress(api,"QueryServiceConfigA");
    const char *name=SCM_PHASE==3?"EventLog":sizeof(void *)==8?"OsitoScmProbe64":"OsitoScmProbe32";
    WCHAR wide_name[64];unsigned i=0;
    do {wide_name[i]=(WCHAR)name[i];} while(name[i++]);
    SC_HANDLE manager=manager_a(0,0,SC_MANAGER_CONNECT|(SCM_PHASE==0||SCM_PHASE==1?SC_MANAGER_CREATE_SERVICE:0));
    CHECK(manager!=0);if(!manager)ExitProcess(3);
    SC_HANDLE service=0;
    if(SCM_PHASE==0||SCM_PHASE==1) {
        service=create_w(manager,wide_name,L"Osito SCM probe",SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS,SERVICE_DEMAND_START,SERVICE_ERROR_NORMAL,
            L"C:\\Diagnostics\\inert-scm-probe.exe",0,0,L"depA\0+groupB\0",0,0);
    } else service=open_a(manager,name,SERVICE_QUERY_CONFIG|(SCM_PHASE==2?DELETE:0));
    CHECK(service!=0);if(!service){close_service(manager);ExitProcess(4);}
    query_pair(service,0);
    SC_HANDLE second=open_w(manager,wide_name,SERVICE_QUERY_STATUS);
    CHECK(second!=0);
    DWORD n=0;
    if(second){
        CHECK(!query_a(second,(void *)a_memory.bytes,sizeof(a_memory.bytes),&n)&&GetLastError()==ERROR_ACCESS_DENIED);
        CHECK(!query_w(second,(void *)w_memory.bytes,sizeof(w_memory.bytes),&n)&&GetLastError()==ERROR_ACCESS_DENIED);
        CHECK(close_service(second));
    }
    CHECK(!open_a((SC_HANDLE)(ULONG_PTR)0x1234,name,SERVICE_QUERY_CONFIG)&&GetLastError()==ERROR_INVALID_HANDLE);
    CHECK(!open_a(manager,"OsitoSCM_missing_91c73e",SERVICE_QUERY_CONFIG)&&GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST);
    CHECK(!query_a((SC_HANDLE)(ULONG_PTR)0x1234,(void *)a_memory.bytes,sizeof(a_memory.bytes),&n)&&GetLastError()==ERROR_INVALID_HANDLE);
    CHECK(!query_w((SC_HANDLE)(ULONG_PTR)0x1234,(void *)w_memory.bytes,sizeof(w_memory.bytes),&n)&&GetLastError()==ERROR_INVALID_HANDLE);
    number(GetLastError());number(failures);emit("SCM ERRORS ");
    if(SCM_PHASE==0||SCM_PHASE==1){
        CHECK(change_w(service,SERVICE_NO_CHANGE,SERVICE_NO_CHANGE,SERVICE_NO_CHANGE,0,0,0,L"",0,0,0));
        query_pair(service,1);
        CHECK(change_w(service,SERVICE_NO_CHANGE,SERVICE_NO_CHANGE,SERVICE_NO_CHANGE,0,0,0,L"depA\0+groupB\0",0,0,0));
        query_pair(service,0);
    }
    if(SCM_PHASE==0||SCM_PHASE==2)CHECK(delete_service(service));
    CHECK(close_service(service));CHECK(close_service(manager));
    CHECK(!query_a(service,(void *)a_memory.bytes,sizeof(a_memory.bytes),&n)&&GetLastError()==ERROR_INVALID_HANDLE);
    number(SCM_PHASE);number(checks);number(failures);emit("SCM DONE ");
    ExitProcess(failures?1:0);
}
