/* Original Steam DLL reload diagnostic; no account or user API is called. */
typedef unsigned int DWORD;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetModuleHandleA(const char *);
API int CALL FreeLibrary(void *);
API int CALL CloseHandle(void *);
API DWORD CALL GetLastError(void);
__declspec(dllimport) void *CALL CreateIoCompletionPort(void *,void *,UPTR,DWORD);
__declspec(dllimport) int CALL GetProcessHandleCount(void *,DWORD *);
__declspec(dllimport) void CALL Sleep(DWORD);

#define SAY(text) WriteFile(output,text,sizeof(text)-1,&written,0)
#define HEX(value) do { \
    DWORD number=(DWORD)(value);char digits[8]; \
    for(int pos=7;pos>=0;pos--){digits[pos]="0123456789abcdef"[number&15];number>>=4;} \
    WriteFile(output,digits,8,&written,0); \
} while(0)
#define CHECK(condition) do {checks++;if(!(condition)){failures++;SAY("STEAMRELOAD FAIL line=");HEX(__LINE__);SAY(" error=");HEX(GetLastError());SAY("\n");}} while(0)

void steam_reload_entry(void)
{
    void *output=GetStdHandle((DWORD)-11);DWORD written=0,checks=0,failures=0;
#ifdef _WIN64
    const char *support[]={"tier0_s64.dll","vstdlib_s64.dll"};
    const char *client="steamclient64.dll";
#else
    const char *support[]={"tier0_s.dll","vstdlib_s.dll"};
    const char *client="steamclient.dll";
#endif
    void *modules[2]={0,0};
    for(unsigned i=0;i<2;i++){modules[i]=LoadLibraryA(support[i]);CHECK(modules[i]!=0);}
    DWORD count=0;
    CHECK(GetProcessHandleCount((void *)(UPTR)-1,&count));
    SAY("STEAMRELOAD BEFORE handles=");HEX(count);SAY("\n");
    if(!failures)for(unsigned i=0;i<96;i++){
        void *module=LoadLibraryA(client);
        CHECK(module!=0);
        if(!module)break;
        CHECK(FreeLibrary(module));
        Sleep(2);
        void *port=CreateIoCompletionPort((void *)(UPTR)-1,0,0,1);
        CHECK(port!=0);
        if(port)CHECK(CloseHandle(port));
        CHECK(GetProcessHandleCount((void *)(UPTR)-1,&count));
        SAY("STEAMRELOAD STEP=");HEX(i+1);SAY(" handles=");HEX(count);
        SAY(" resident=");HEX(GetModuleHandleA(client)!=0);SAY("\n");
        if(!port)break;
    }
    for(unsigned i=2;i>0;i--)if(modules[i-1])CHECK(FreeLibrary(modules[i-1]));
    SAY("STEAMRELOAD DONE checks=");HEX(checks);SAY(" failures=");HEX(failures);SAY("\n");
    ExitProcess(failures?1:0);
}
