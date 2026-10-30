#ifndef DLL_LIFECYCLE_COMMON_H
#define DLL_LIFECYCLE_COMMON_H
typedef unsigned int DWORD;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL GetModuleHandleA(const char *);
API int CALL FreeLibrary(void *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API DWORD CALL GetCurrentProcessId(void);
API void *CALL CreateMutexA(void *,int,const char *);
API void *CALL OpenMutexA(DWORD,int,const char *);
API int CALL CloseHandle(void *);
API int CALL DisableThreadLibraryCalls(void *);
API DWORD CALL TlsAlloc(void);
API int CALL TlsFree(DWORD);
API int CALL TlsSetValue(DWORD,void *);
API void *CALL TlsGetValue(DWORD);
API void *CALL CreateThread(void *,UP,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
API DWORD CALL WaitForSingleObject(void *,DWORD);
API int CALL GetExitCodeThread(void *,DWORD *);
static void lifecycle_name(char *out,unsigned failing)
{
    const char prefix[]="Local\\Osito-DLL-Lifecycle-";
    unsigned n=0;while(prefix[n]){out[n]=prefix[n];n++;}
    DWORD pid=GetCurrentProcessId();
    for(int i=7;i>=0;i--)out[n++]= "0123456789abcdef"[(pid>>(i*4))&15];
    out[n++]=failing?'F':'G';out[n]=0;
}
#endif
