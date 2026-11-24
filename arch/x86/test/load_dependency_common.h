#ifndef LOAD_DEPENDENCY_COMMON_H
#define LOAD_DEPENDENCY_COMMON_H
#include "dll_lifecycle_common.h"
API void *CALL CreateEventA(void *,int,int,const char *);
API void *CALL OpenEventA(DWORD,int,const char *);
API int CALL SetEvent(void *);
API int CALL ResetEvent(void *);
static void dependency_name(char out[80])
{
    lifecycle_name(out,0);
    unsigned n=0;while(out[n])n++;
    const char suffix[]="-dependent-attach";
    for(unsigned i=0;i<sizeof(suffix);i++)out[n+i]=suffix[i];
}
#endif
