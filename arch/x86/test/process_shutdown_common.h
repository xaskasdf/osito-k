#ifndef PROCESS_SHUTDOWN_COMMON_H
#define PROCESS_SHUTDOWN_COMMON_H
#include "loader_state_common.h"
API void CALL Sleep(DWORD);
API DWORD CALL GetCurrentThreadId(void);
API void *CALL GetCurrentProcess(void);
API int CALL TerminateProcess(void *,DWORD);
API void *CALL OpenThread(DWORD,int,DWORD);
API void *CALL CreateEventA(void *,int,int,const char *);
API int CALL SetEvent(void *);
static void shutdown_row(const char *name,DWORD value)
{
    char out[100];unsigned n=0;const char prefix[]="SHUTDOWN ";
    for(unsigned i=0;prefix[i];i++)out[n++]=prefix[i];
    while(*name)out[n++]=*name++;
    out[n++]='=';
    for(int i=7;i>=0;i--)out[n++]="0123456789abcdef"[(value>>(4*i))&15];
    out[n++]='\n';DWORD written;WriteFile(GetStdHandle((DWORD)-11),out,n,&written,0);
}
#endif
