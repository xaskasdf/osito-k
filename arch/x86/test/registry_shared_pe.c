/* Cross-ABI HKCU settings; only new private leaves are written and removed. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define W(s) ((const WCHAR *)u##s)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API DWORD CALL GetCurrentProcessId(void);
API DWORD CALL RegCreateKeyExA(HANDLE,const char *,DWORD,char *,DWORD,DWORD,void *,HANDLE *,DWORD *);
API DWORD CALL RegOpenKeyExA(HANDLE,const char *,DWORD,DWORD,HANDLE *);
API DWORD CALL RegSetValueExW(HANDLE,const WCHAR *,DWORD,DWORD,const void *,DWORD);
API DWORD CALL RegQueryValueExA(HANDLE,const char *,DWORD *,DWORD *,void *,DWORD *);
API DWORD CALL RegDeleteKeyExA(HANDLE,const char *,DWORD,DWORD);
API DWORD CALL RegCloseKey(HANDLE);
static unsigned checks,failures;
static void say(const char *text)
{DWORD n=0,w;while(text[n])n++;WriteFile(GetStdHandle((DWORD)-11),text,n,&w,0);}
static void hex(DWORD value)
{char text[9];for(unsigned i=0;i<8;i++)text[i]="0123456789abcdef"[(value>>((7-i)*4))&15];text[8]=0;say(text);}
#define CHECK(c) do {checks++;if(!(c)){failures++;say("REGSHARED FAIL line=");hex(__LINE__);say("\n");}} while(0)
static unsigned copy(char *out,const char *in)
{unsigned n=0;while(in[n]){out[n]=in[n];n++;}out[n]=0;return n;}
static int equal(const char *a,const char *b)
{while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void query(HANDLE key,const char *expected)
{
    char buffer[32];DWORD size=sizeof(buffer),type=0;
    CHECK(RegQueryValueExA(key,"Text",0,&type,buffer,&size)==0);
    CHECK(type==1&&equal(buffer,expected));
}
void registry_shared_entry(void)
{
    static const struct {const char *path;int redirected;} cases[]={
        {"Software\\OsitoK\\Tests",0},
        {"Software\\Classes\\AppId",0},
        {"Software\\Classes\\OsitoK",0},
        {"Software\\Classes\\CLSID",1},
        {"Software\\Classes\\DirectShow",1},
        {"Software\\Classes\\Interface",1},
        {"Software\\Classes\\Media Type",1},
        {"Software\\Classes\\MediaFoundation",1},
        {"Software\\Classes\\CLSIDExtra",0},
        {"Software\\Classes\\MediaFoundationExtra",0},
    };
    HANDLE hkcu=(HANDLE)(__INTPTR_TYPE__)(int)0x80000001U;
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++) {
        char path[180];unsigned n=copy(path,cases[i].path);n+=copy(path+n,"\\Osito-Shared-20261007-");
        DWORD pid=GetCurrentProcessId();for(unsigned j=0;j<8;j++)path[n+j]="0123456789abcdef"[(pid>>((7-j)*4))&15];path[n+8]=0;
        HANDLE key32=0,key64=0;DWORD disposition=0;
        DWORD error=RegCreateKeyExA(hkcu,path,0,0,0,0xf023f,0,&key32,&disposition);
        CHECK(error==0&&disposition==1);if(error||disposition!=1){if(key32)RegCloseKey(key32);continue;}
        CHECK(RegSetValueExW(key32,W("Text"),0,1,W("from32"),14)==0);
        error=RegOpenKeyExA(hkcu,path,0,0x20119,&key64);
        CHECK(error==(cases[i].redirected?2U:0U));
        if(!error){query(key64,"from32");RegCloseKey(key64);key64=0;}
        CHECK(RegCreateKeyExA(hkcu,path,0,0,0,0xf013f,0,&key64,&disposition)==0);
        CHECK(disposition==(cases[i].redirected?1U:2U));
        CHECK(RegSetValueExW(key64,W("Text"),0,1,W("from64"),14)==0);
        query(key64,"from64");query(key32,cases[i].redirected?"from32":"from64");
        CHECK(RegCloseKey(key32)==0);CHECK(RegCloseKey(key64)==0);
        CHECK(RegDeleteKeyExA(hkcu,path,0x200,0)==0);
        CHECK(RegDeleteKeyExA(hkcu,path,0x100,0)==(cases[i].redirected?0U:2U));
    }
    say("REGSHARED DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");ExitProcess(failures?1:0);
}
