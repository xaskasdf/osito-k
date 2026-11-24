/* Bounded CRT memory/string operations used by independently installed DLLs.
 * Pointer validity and non-overlap are the caller's normal CRT preconditions;
 * size/null contract violations use the process/thread invalid handler. */
#include "msvcrt_shim.h"

static int crt_secure_error(int error)
{
    *crt_errno()=error;
    crt_report_invalid_parameter();
    return error;
}

int WINAPI crt_memcpy_s(PVOID destination,SIZE_T size,PCVOID source,SIZE_T count)
{
    if (!count) return 0;
    if (!destination) return crt_secure_error(22);
    if (!source || size<count) {
        crt_memset(destination,0,size);
        return crt_secure_error(source ? 34 : 22);
    }
    crt_memcpy(destination,source,count);
    return 0;
}

int WINAPI crt_memmove_s(PVOID destination,SIZE_T size,PCVOID source,SIZE_T count)
{
    if (!count) return 0;
    if (!destination || !source) return crt_secure_error(22);
    if (size<count) return crt_secure_error(34);
    crt_memmove(destination,source,count);
    return 0;
}

int WINAPI crt_strcpy_s(char *destination,SIZE_T size,const char *source)
{
    if (!destination || !size) return crt_secure_error(22);
    if (!source) { destination[0]=0; return crt_secure_error(22); }
    for (SIZE_T i=0;i<size;i++) {
        if (!(destination[i]=source[i])) return 0;
    }
    destination[0]=0;
    return crt_secure_error(34);
}

int WINAPI crt_strcat_s(char *destination,SIZE_T size,const char *source)
{
    if (!destination || !size) return crt_secure_error(22);
    if (!source) { destination[0]=0; return crt_secure_error(22); }
    SIZE_T used=0;
    while (used<size && destination[used]) used++;
    if (used==size) { destination[0]=0; return crt_secure_error(22); }
    while (used<size) {
        if (!(destination[used++]=*source++)) return 0;
    }
    destination[0]=0;
    return crt_secure_error(34);
}
