#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "../win32/delay_load.c"
int g_compat32_mode;
static BYTE *image,*scratch;
static unsigned checks,loads,frees,lookups,retains,releases,protects,readonly_slot,competition;
static DWORD last_error;
static BOOL parent_dll;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL %u: %s\n",__LINE__,#x);exit(1);}}while(0)
ULONG_PTR win32_current_image_base(void) {return parent_dll ? 0 : (ULONG_PTR)image;}
static int inside(PCVOID p,SIZE_T n,const BYTE *base,SIZE_T length)
{ULONG_PTR a=(ULONG_PTR)p,b=(ULONG_PTR)base;return base && a>=b && a-b<=length && n<=length-(a-b);}
int win32_user_range_readable(PCVOID p,SIZE_T n,BOOL compat)
{return (!compat || (ULONG_PTR)p<=UINT32_MAX) && (inside(p,n,image,8192) || inside(p,n,scratch,4096));}
int win32_user_range_writable(PVOID p,SIZE_T n,BOOL compat)
{return !readonly_slot && win32_user_range_readable(p,n,compat);}
int win32_user_range_executable(PCVOID p,SIZE_T n,BOOL compat)
{(void)n;(void)compat;return p==(void *)0x30000000 || inside(p,1,image+2048,64);}
PVOID dll_get_module_handle_by_address(PVOID p,BOOL add,BOOL pin)
{CHECK(add && !pin);if(inside(p,1,image,8192)){retains++;return image;}return NULL;}
BOOL dll_release_module(PVOID p) {CHECK(p==image);releases++;return TRUE;}
DWORD WINAPI GetLastError(void) {return last_error;}
void WINAPI SetLastError(DWORD value) {last_error=value;}
HANDLE WINAPI LoadLibraryA(PCSTR name)
{
    loads++;if(strcmp(name,"target.dll")){last_error=126;return NULL;}
    if(competition) {
        if(g_compat32_mode) *(DWORD *)(image+4096)=0x50000000;
        else *(ULONG_PTR *)(image+4096)=0x50000000;
    }
    return (HANDLE)0x50000000;
}
BOOL WINAPI FreeLibrary(HANDLE h) {CHECK(h==(HANDLE)0x50000000);frees++;return TRUE;}
PVOID WINAPI GetProcAddress(HANDLE h,PCSTR name)
{
    lookups++;CHECK(h==(HANDLE)0x50000000);
    if((ULONG_PTR)name==7 || ((ULONG_PTR)name>65535 && !strcmp(name,"Value"))) return (PVOID)0x30000000;
    last_error=127;return NULL;
}
BOOL WINAPI VirtualProtect(PVOID p,SIZE_T n,DWORD protection,DWORD *old)
{CHECK(win32_user_range_readable(p,n,g_compat32_mode));*old=readonly_slot ? PAGE_READONLY : PAGE_READWRITE;
 readonly_slot=protection!=PAGE_READWRITE;protects++;return TRUE;}
PVOID WINAPI VirtualAlloc(PVOID address,SIZE_T size,DWORD allocation,DWORD protection)
{(void)address;(void)allocation;(void)protection;CHECK(size==4096 && !scratch);
 scratch=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);CHECK(scratch!=MAP_FAILED);return scratch;}
BOOL WINAPI VirtualFree(PVOID p,SIZE_T size,DWORD type)
{CHECK(p==scratch && size==0 && type==MEM_RELEASE);CHECK(!munmap(scratch,4096));scratch=NULL;return TRUE;}
uint32_t compat32_callback_args(uint32_t p,int count,const uint32_t *args)
{(void)p;(void)count;(void)args;CHECK(0);return 0;}
void NTAPI RtlRaiseException(PEXCEPTION_RECORD record)
{CHECK(record->ExceptionCode==0x505 && record->ExceptionFlags==1 && record->NumberParameters==2);}
static void pointer(unsigned offset,ULONG_PTR value)
{if(g_compat32_mode)memcpy(image+offset,&value,4);else memcpy(image+offset,&value,8);}
static DELAY_DESCRIPTOR *prepare(void)
{
    memset(image,0,8192);loads=frees=lookups=retains=releases=protects=readonly_slot=competition=0;last_error=0;
    IMAGE_DOS_HEADER *dos=(void *)image;dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=128;
    *(DWORD *)(image+128)=IMAGE_NT_SIGNATURE;*(WORD *)(image+152)=g_compat32_mode ? 0x10b : 0x20b;
    *(DWORD *)(image+208)=8192;
    DELAY_DESCRIPTOR *d=(void *)(image+1024);*d=(DELAY_DESCRIPTOR){1,1100,4096,4104,1200,0,0,0};
    strcpy((char *)image+1100,"target.dll");strcpy((char *)image+1302,"Value");
    pointer(1200,1300);pointer(4104,(ULONG_PTR)image+2048);return d;
}
int main(void)
{
    image=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);CHECK(image!=MAP_FAILED);
    for(g_compat32_mode=0;g_compat32_mode<=1;g_compat32_mode++) {
        for(unsigned mode=0;mode<5;mode++) {
            DELAY_DESCRIPTOR *d=prepare();parent_dll=mode==4;readonly_slot=mode==1;competition=mode==2;
            if(mode==3)pointer(1200,(g_compat32_mode ? 1ULL<<31 : 1ULL<<63)|7);
            CHECK(ResolveDelayLoadedAPI(image,d,NULL,NULL,image+4104,0)==(PVOID)0x30000000);
            CHECK(loads==1 && lookups==1 && frees==(mode==2) && retains==releases);
            CHECK(readonly_slot==(mode==1) && protects==(mode==1 ? 4u : 0u));
            CHECK(ResolveDelayLoadedAPI(image,d,NULL,NULL,image+4104,0)==(PVOID)0x30000000);
            CHECK(loads==1 && lookups==1 && retains==releases);
        }
        parent_dll=FALSE;
        for(unsigned bad=0;bad<22;bad++) {
            DELAY_DESCRIPTOR *d=prepare();PVOID thunk=image+4104;ULONG flags=0;
            switch(bad) {
            case 0:d->attributes=0;break;case 1:d->attributes=3;break;case 2:d->name=8192;break;
            case 3:d->module=8191;break;case 4:d->iat=0xfffffff8;break;case 5:d->names=8192;break;
            case 6:d->name=0;break;case 7:d->module=0;break;case 8:d->iat=0;break;case 9:d->names=0;break;
            case 10:thunk=image+4105;break;case 11:thunk=image+4088;break;case 12:thunk=image+8192;break;
            case 13:pointer(1200,8191);break;case 14:pointer(1200,0);break;
            case 15:((IMAGE_DOS_HEADER *)image)->e_lfanew=4096;break;case 16:*(WORD *)(image+152)=0;break;
            case 17:*(DWORD *)(image+208)=128;break;case 18:flags=1;break;
            case 19:d=(void *)(image+8180);break;case 20:memset(image+1100,'x',260);break;
            case 21:memset(image+1302,'x',512);break;
            }
            CHECK(!ResolveDelayLoadedAPI(image,d,NULL,NULL,thunk,flags));CHECK(last_error==87 && loads==0);
        }
        for(unsigned ordinal=0;ordinal<2;ordinal++) {
            DELAY_DESCRIPTOR *d=prepare();strcpy((char *)image+1302,"Missing");
            if(ordinal)pointer(1200,(g_compat32_mode ? 1ULL<<31 : 1ULL<<63)|60000);
            CHECK(!ResolveDelayLoadedAPI(image,d,NULL,NULL,image+4104,0));CHECK(last_error==(ordinal ? 182u : 127u));
            CHECK(delay_pointer(image+4104,g_compat32_mode)==(ULONG_PTR)image+2048);
        }
        /* Random metadata stays inside a mapped arena while the image bounds
         * vary; sanitizer builds check every rejected pointer calculation. */
        uint32_t seed=0x334489;
        for(unsigned i=0;i<10000;i++) {
            DELAY_DESCRIPTOR *d=prepare();seed=seed*1664525+1013904223;
            ((DWORD *)d)[i%5]=seed;
            (void)ResolveDelayLoadedAPI(image,d,NULL,NULL,image+4104,0);
            CHECK(retains==releases && !scratch);
        }
    }
    DelayLoadFailureHook("x","y");CHECK(!munmap(image,8192));
    printf("Native delay binding: %u checks passed\n",checks);return 0;
}
