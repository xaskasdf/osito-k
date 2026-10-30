#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#define OSITOK_X86_TYPES_H
#define __KERNEL_X86__ 1
#include "../win32/fold_string.c"
#include "../win32/short_path.c"
static _Thread_local DWORD last_error;
static _Thread_local unsigned allocations, attempts;
static _Thread_local int fail_at=-1, compat;
static _Thread_local const void *deny_read,*deny_write;
static unsigned checks,failures;
void *kmalloc(uint64_t n)
{if((int)attempts++==fail_at)return NULL;void *p=malloc(n);if(p)allocations++;return p;}
void kfree(void *p){if(p){allocations--;free(p);}}
int *proc_win32_compat32_mode_slot(void){return &compat;}
int win32_user_range_readable(const void *p,SIZE_T n,BOOL mode)
{(void)n;(void)mode;return (uintptr_t)p>=4096&&p!=deny_read;}
int win32_user_range_writable(void *p,SIZE_T n,BOOL mode)
{(void)n;(void)mode;return (uintptr_t)p>=4096&&p!=deny_write;}
void WINAPI SetLastError(DWORD value){last_error=value;}
DWORD WINAPI GetLastError(void){return last_error;}
DWORD WINAPI GetFileAttributesW(PCWSTR path)
{
    char name[64];unsigned i=0;
    for(;path[i]&&i<63;i++)name[i]=(char)path[i];name[i]=0;
    if(!strcmp(name,"C:\\")||!strcmp(name,"C:\\Windows")||!strcmp(name,"C:\\WINDOWS")) {
        SetLastError(0);return 16; /* Verify that the wrapper preserves last error. */
    }
    SetLastError(2);return 0xffffffffU;
}
static void check(int ok,unsigned test,unsigned item)
{
    __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED);
    if(!ok){unsigned count=__atomic_add_fetch(&failures,1,__ATOMIC_RELAXED);
        if(count<35)printf("FOLD FAIL case=%u item=%u\n",test,item);}
}
#define CHECK_CASE(c,t,i) check((c),(t),(i))
#include "fold_string_checks.h"

/* SysV entry for the independent Unicode conformance corpus. */
int fold_host_call(DWORD flags,PCWSTR input,int length,PWSTR output,int capacity,DWORD *error)
{
    SetLastError(0x1234);
    int result=FoldStringW(flags,input,length,output,capacity);
    *error=GetLastError();return result;
}

static void *fold_worker(void *argument)
{
    (void)argument;WCHAR input[]={65,0x315,0x300,0xe6,0xdf,0},out[20];
    const WCHAR expected[]={0xc0,0x315,'a','e','s','s',0};
    for(unsigned i=0;i<1500;i++) {
        SetLastError(0x1234);
        CHECK_CASE(FoldStringW(0x2010,input,-1,out,20)==7,20000,1);
        CHECK_CASE(!memcmp(out,expected,sizeof(expected))&&GetLastError()==0x1234,20000,2);
    }
    CHECK_CASE(!allocations,20000,3);return NULL;
}

int main(void)
{
    fold_contract_checks();CHECK_CASE(!allocations,30000,1);
    WCHAR long_input[1536],out[1600];
    for(unsigned i=0;i<1536;i++)long_input[i]=i%2?0x345:0x334;
    attempts=0;
    CHECK_CASE(FoldStringW(0x40,long_input,1536,out,1600)==1536,30001,1);
    unsigned allocation_count=attempts;
    for(unsigned i=0;i<1536;i++)CHECK_CASE(out[i]==(i<768?0x334:0x345),30001,2);
    for(unsigned n=0;n<allocation_count;n++) {
        attempts=0;fail_at=(int)n;out[0]='Z';
        CHECK_CASE(!FoldStringW(0x40,long_input,1536,out,1600)&&GetLastError()==14,30002,n);
        CHECK_CASE(out[0]=='Z'&&!allocations,30003,n);
    }
    fail_at=-1;
    WCHAR invalid[]={0xd800,0},valid[]={'A',0};out[0]='Z';
    CHECK_CASE(!FoldStringW(16,invalid,1,out,20)&&GetLastError()==1113&&out[0]=='Z',30004,1);
    deny_read=valid;
    CHECK_CASE(!FoldStringW(16,valid,1,out,20)&&GetLastError()==87,30004,2);
    CHECK_CASE(!GetShortPathNameW(valid,out,20)&&GetLastError()==87,30004,3);
    deny_read=NULL;deny_write=out;
    CHECK_CASE(!FoldStringW(16,valid,1,out,20)&&GetLastError()==87,30004,4);
    CHECK_CASE(!GetShortPathNameW((PCWSTR)u"C:\\Windows",out,20)&&GetLastError()==87,30004,5);
    deny_write=NULL;
    pthread_t threads[4];
    for(unsigned i=0;i<4;i++)CHECK_CASE(!pthread_create(threads+i,NULL,fold_worker,NULL),30005,i);
    for(unsigned i=0;i<4;i++)CHECK_CASE(!pthread_join(threads[i],NULL),30006,i);
    CHECK_CASE(!allocations,30007,1);
    printf("FOLD HOST checks=%u failures=%u allocations=%u\n",checks,failures,allocations);
    return failures?1:0;
}
