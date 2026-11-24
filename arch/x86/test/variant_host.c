#include "variant_host_types.h"
#include "../win32/ole32_shim.h"
#include "../win32/compat32.h"
#include "../win32/msvcrt_shim.h"
#include <time.h>
uint64_t ntp_get_utc_100ns(void) { return (uint64_t)time(NULL) * 10000000; }
typedef struct {
    DWORD source[4];const WORD *text;DWORD text_length;
    WORD target,flags;DWORD status,output[4];const WORD *output_text;DWORD output_length;
} VARIANT_CASE;
#include "variant_cases.h"
static DWORD last_error;
static unsigned failures, checks, allocations;
static int compat;
int *proc_win32_compat32_mode_slot(void) { return &compat; }
void WINAPI SetLastError(DWORD v) { last_error=v; }
DWORD WINAPI GetLastError(void) { return last_error; }
PVOID WINAPI crt_malloc(SIZE_T n) { void *p=malloc(n);if(p)allocations++;return p; }
void WINAPI crt_free(PVOID p) { if(p){allocations--;free(p);} }
double WINAPI crt_strtod(const char *s,char **end) { return strtod(s,end); }
int WINAPI crt_snprintf(char *p,SIZE_T n,const char *format,...)
{
    __builtin_ms_va_list ap;__builtin_ms_va_start(ap,format);
    unsigned a=__builtin_va_arg(ap,unsigned),b=__builtin_va_arg(ap,unsigned),d=__builtin_va_arg(ap,unsigned);
    int result;
    if(strchr(format,'/')) result=snprintf(p,n,format,a,b,d);
    else { const char *s=__builtin_va_arg(ap,const char *);result=snprintf(p,n,format,a,b,d,s); }
    __builtin_ms_va_end(ap);return result;
}
uint32_t compat32_callback_args(uint32_t f,int n,const uint32_t *args)
{ (void)f;(void)n;(void)args;abort(); }
static void check(int okay,unsigned index,const char *label,DWORD actual,DWORD expected)
{
    checks++;
    if(!okay){failures++;if(failures<200)printf("VARIANT FAIL case=%u %s got=%08x expected=%08x\n",index,label,actual,expected);}
}
int main(void)
{
    for(unsigned i=0;i<sizeof(variant_cases)/sizeof(*variant_cases);i++){
        const VARIANT_CASE *r=&variant_cases[i];
        uint64_t source[3]={0},dest[4]={0},before[3];
        memcpy(source,r->source,16);PWSTR owner=NULL;
        if(r->text){owner=shim_SysAllocStringLen(r->text,r->text_length);if(!owner)abort();source[1]=(uintptr_t)owner;}
        dest[0]=3;dest[1]=321;dest[3]=0x123456789abcdef0ULL;
        memcpy(before,dest,24);last_error=0x12345678;
        DWORD hr=(DWORD)shim_VariantChangeType(dest,source,r->flags,r->target);
        check(hr==r->status,i,"status",hr,r->status);
        check(last_error==0x12345678,i,"last-error",last_error,0x12345678);
        check(dest[3]==0x123456789abcdef0ULL,i,"guard",(DWORD)dest[3],0x9abcdef0);
        if(hr==r->status){
            if(hr)check(!memcmp(before,dest,24),i,"failure preserves destination",(DWORD)dest[0],3);
            else if(r->target==8){
                PCWSTR text=(PCWSTR)(uintptr_t)dest[1];
                check((text!=NULL)==(r->output_text!=NULL),i,"BSTR null",text!=NULL,r->output_text!=NULL);
                if(text&&r->output_text){
                    unsigned bytes=shim_SysStringByteLen(text);
                    check(bytes==r->output_length*2,i,"BSTR bytes",bytes,r->output_length*2);
                    check(bytes==r->output_length*2&&!memcmp(text,r->output_text,bytes),i,"BSTR text",text[0],r->output_text[0]);
                }
            }else {
                for(unsigned w=0;w<4;w++)check(((DWORD *)dest)[w]==r->output[w],i,"payload",((DWORD *)dest)[w],r->output[w]);
            }
        }
        if(!hr)shim_VariantClear(dest);
        shim_SysFreeString(owner);
        check(allocations==0,i,"allocation balance",allocations,0);
    }
    printf("VARIANT HOST checks=%u failures=%u allocations=%u\n",checks,failures,allocations);
    return failures!=0;
}
