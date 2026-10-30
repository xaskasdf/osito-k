#ifndef LOADER_STATE_COMMON_H
#define LOADER_STATE_COMMON_H
typedef unsigned int DWORD;
typedef unsigned char BYTE;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API BYTE CALL RtlDllShutdownInProgress(void);
API BYTE CALL RtlIsThreadWithinLoaderCallout(void);
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL GetModuleHandleA(const char *);
API int CALL FreeLibrary(void *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API void *CALL CreateThread(void *,UP,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
API DWORD CALL WaitForSingleObject(void *,DWORD);
API int CALL GetExitCodeThread(void *,DWORD *);
API int CALL CloseHandle(void *);
static void loader_state_row(const char *label,DWORD reason,void *reserved)
{
    char out[160];unsigned n=0;const char prefix[]="LDRSTATE ";
    for(unsigned i=0;prefix[i];i++)out[n++]=prefix[i];
    while(*label)out[n++]=*label++;
    BYTE callout=RtlIsThreadWithinLoaderCallout(),shutdown=RtlDllShutdownInProgress();
    const char *fields[]={" reason="," callout="," shutdown="," reserved="};
    DWORD values[]={reason,callout,shutdown,reserved!=0};
    for(unsigned f=0;f<4;f++) {
        for(unsigned i=0;fields[f][i];i++)out[n++]=fields[f][i];
        for(int i=7;i>=0;i--)out[n++]="0123456789abcdef"[(values[f]>>(i*4))&15];
    }
    out[n++]='\n';DWORD written;WriteFile(GetStdHandle((DWORD)-11),out,n,&written,0);
}
#endif
