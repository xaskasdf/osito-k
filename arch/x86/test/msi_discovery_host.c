#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#define OSITOK_X86_TYPES_H
#define __KERNEL_X86__ 1
#include MSI_DISCOVERY_HOST_SOURCE
static unsigned checks, failures, allocations, opened, closed;
static _Thread_local DWORD test_owner=1, test_thread=1;
static _Thread_local int compat, fail_after=-1;
static const WCHAR *ids[]={DW("{13a4ee12-23ea-3371-91ee-efb36ddfff3e}"),DW("{f8cfeb22-a2e7-3971-9eda-4b11edefc185}")};
static const WCHAR *family=DW("{B59F5BF1-67C8-3802-8E59-2CE551A39FC5}");
static void discovery_host_check(int ok,unsigned line)
{
    __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED);
    if(!ok){__atomic_add_fetch(&failures,1,__ATOMIC_RELAXED);printf("MSIDISCOVERY HOST FAIL line=%u\n",line);}
}
#define CHECK(c) discovery_host_check((c),__LINE__)
void *kmalloc(uint64_t size)
{
    if(fail_after>=0&&fail_after--==0)return NULL;
    void *p=malloc(size);if(p)__atomic_add_fetch(&allocations,1,__ATOMIC_RELAXED);return p;
}
void kfree(void *p)
{if(p){__atomic_sub_fetch(&allocations,1,__ATOMIC_RELAXED);free(p);}}
DWORD win32_current_process_id(void){return test_owner;}
DWORD WINAPI GetCurrentThreadId(void){return test_thread;}
int *proc_win32_compat32_mode_slot(void){return &compat;}
int win32_user_range_readable(const void *p,SIZE_T n,BOOL mode)
{(void)n;(void)mode;return (uintptr_t)p>=4096;}
int win32_user_range_writable(void *p,SIZE_T n,BOOL mode)
{(void)n;(void)mode;return (uintptr_t)p>=4096;}
LONG WINAPI RegOpenKeyExW(HKEY root,PCWSTR path,DWORD reserved,DWORD access,PHKEY out)
{
    (void)path;CHECK(!reserved&&access==(KEY_READ|KEY_WOW64_64KEY));
    if(root!=HKEY_LOCAL_MACHINE)return 2;
    *out=(HKEY)0x1000;__atomic_add_fetch(&opened,1,__ATOMIC_RELAXED);return 0;
}
LONG WINAPI RegEnumKeyExW(HKEY key,DWORD index,PWSTR name,DWORD *length,DWORD *reserved,
                           PWSTR cls,DWORD *cls_length,PVOID time)
{
    CHECK(key==(HKEY)0x1000&&!reserved&&!cls&&!cls_length&&!time&&*length>=39);
    if(index>=2)return 259;
    memcpy(name,ids[index],78);*length=38;return 0;
}
LONG WINAPI RegCloseKey(HKEY key)
{CHECK(key!=NULL);__atomic_add_fetch(&closed,1,__ATOMIC_RELAXED);return 0;}
LONG WINAPI RegQueryValueExW(HKEY key,PCWSTR name,DWORD *reserved,DWORD *type,BYTE *data,DWORD *length)
{(void)key;(void)name;(void)reserved;(void)type;(void)data;(void)length;return 2;}
DWORD msi_product_open_registered(PCWSTR product,HKEY *key)
{
    if(!discovery_guid_equal(product,ids[0])&&!discovery_guid_equal(product,ids[1]))return 1605;
    *key=(HKEY)0x2000;__atomic_add_fetch(&opened,1,__ATOMIC_RELAXED);return 0;
}
DWORD msi_product_read_string(HKEY key,PCWSTR name,PWSTR *text)
{
    (void)name;CHECK(key==(HKEY)0x2000);
    *text=kmalloc(78);if(!*text)return 8;memcpy(*text,family,78);return 0;
}
static void *discovery_host_worker(void *argument)
{
    test_thread=(DWORD)(uintptr_t)argument;WCHAR text[39];
    for(unsigned i=0;i<2000;i++){
        CHECK(MsiEnumProductsW(0,text)==0&&discovery_guid_equal(text,ids[0]));
        CHECK(MsiEnumProductsW(2,text)==87);
        CHECK(MsiEnumProductsW(1,text)==0&&discovery_guid_equal(text,ids[1]));
        CHECK(MsiEnumProductsW(1,text)==87);
        CHECK(MsiEnumProductsW(2,text)==259);
    }
    return NULL;
}
int main(void)
{
    WCHAR text[40];for(unsigned i=0;i<40;i++)text[i]='Z';
    CHECK(MsiEnumProductsW(1,text)==87);
    fail_after=0;CHECK(MsiEnumProductsW(0,text)==8);fail_after=-1;
    CHECK(text[0]=='Z'&&!allocations);
    for(int fail=0;fail<3;fail++){
        fail_after=fail;DWORD result=MsiEnumRelatedProductsW(family,0,0,text);fail_after=-1;
        CHECK(result==0||result==8);msi_discovery_release_process(test_owner);CHECK(!allocations);
    }
    CHECK(MsiEnumProductsW(0,text)==0);
    test_owner=2;CHECK(MsiEnumProductsW(1,text)==87);CHECK(MsiEnumProductsW(0,text)==0);
    msi_discovery_release_process(1);CHECK(allocations==1);
    CHECK(MsiEnumProductsW(1,text)==0&&discovery_guid_equal(text,ids[1]));
    CHECK(MsiEnumProductsW(2,text)==259);CHECK(!allocations);
    test_owner=1;
    for(unsigned i=0;i<1000;i++){
        CHECK(MsiEnumProductsW(0,text)==0);
        CHECK(MsiEnumRelatedProductsW(family,0,0,text)==0);
        msi_discovery_release_process(1);CHECK(!allocations);
    }
    pthread_t threads[4];
    for(unsigned i=0;i<4;i++)CHECK(pthread_create(&threads[i],NULL,discovery_host_worker,(void *)(uintptr_t)(i+2))==0);
    for(unsigned i=0;i<4;i++)CHECK(pthread_join(threads[i],NULL)==0);
    CHECK(!allocations&&!discovery_cursors&&opened==closed);
    printf("MSIDISCOVERY HOST checks=%u failures=%u allocations=%u handles=%u/%u\n",checks,failures,allocations,opened,closed);
    return failures?1:0;
}
