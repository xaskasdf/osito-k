/* GUEST ONLY: executes an external diagnostic DLL's registration exports.
 * Never run this probe with Microsoft installer payloads on the host. */
typedef unsigned int DWORD;
typedef int LONG;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API int CALL FreeLibrary(void *);
API void *CALL GetProcAddress(void *,const char *);
API char *CALL GetCommandLineA(void);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
#define SAY(s) do {DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define HEX(v) do {char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
void dll_register_entry(void)
{
    const char *command=GetCommandLineA();unsigned quoted=0;
    while(*command){if(*command=='"')quoted^=1;else if(!quoted&&(*command==' '||*command=='\t'))break;command++;}
    while(*command==' '||*command=='\t')command++;
    char target[260];unsigned length=0;quoted=*command=='"';if(quoted)command++;
    while(*command&&length<259){if(quoted?*command=='"':(*command==' '||*command=='\t'))break;target[length++]=*command++;}
    target[length]=0;if(!length){SAY("DLLREGISTER FAIL target missing\n");ExitProcess(1);}
    SetLastError(0x12345678);void *module=LoadLibraryA(target);DWORD error=GetLastError();
    SAY("DLLREGISTER LOAD ");HEX(module!=0);HEX(error);SAY("\n");
    if(!module){SAY("DLLREGISTER FAIL load\n");ExitProcess(1);}
    LONG (CALL *reg)(void)=GetProcAddress(module,"DllRegisterServer");
    LONG (CALL *unreg)(void)=GetProcAddress(module,"DllUnregisterServer");
    if(!reg||!unreg){FreeLibrary(module);SAY("DLLREGISTER FAIL exports\n");ExitProcess(1);}
    LONG status=reg();SAY("DLLREGISTER REGISTER ");HEX(status);SAY("\n");
    if(status<0){FreeLibrary(module);SAY("DLLREGISTER FAIL register\n");ExitProcess(1);}
    status=unreg();SAY("DLLREGISTER UNREGISTER ");HEX(status);SAY("\n");
    int released=FreeLibrary(module);SAY("DLLREGISTER FREE ");HEX(released);SAY("\n");
    if(status<0||!released){SAY("DLLREGISTER FAIL cleanup\n");ExitProcess(1);}
    SAY("DLLREGISTER DONE\n");ExitProcess(0);
}
