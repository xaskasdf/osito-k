#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define OSITOK_X86_TYPES_H
#include "../win32/msi_products.c"
static unsigned checks,failures,allocations,opened,closed;
static int compat,fail_alloc;
static DWORD state=5,schema=1,context=4,fail_read;
static PCWSTR missing_name,bad_type,growing;
static const void *deny_read,*deny_write;
static void product_check(int condition,unsigned line)
{checks++;if(!condition){failures++;printf("MSIPRODUCT FAIL line=%u\n",line);}}
void *kmalloc(uint64_t n){if(fail_alloc)return NULL;void *p=malloc(n);if(p)allocations++;return p;}
void kfree(void *p){if(p){allocations--;free(p);}}
int *proc_win32_compat32_mode_slot(void){return &compat;}
int win32_user_range_readable(const void *p,SIZE_T n,BOOL mode)
{(void)n;(void)mode;return (uintptr_t)p>=4096&&p!=deny_read;}
int win32_user_range_writable(void *p,SIZE_T n,BOOL mode)
{(void)n;(void)mode;return (uintptr_t)p>=4096&&p!=deny_write;}
LONG WINAPI RegOpenKeyExW(HKEY root,PCWSTR path,DWORD reserved,DWORD access,PHKEY out)
{
    product_check(root==HKEY_LOCAL_MACHINE&&!reserved&&access==(KEY_READ|KEY_WOW64_64KEY),__LINE__);
    if(fail_read)return fail_read;
    if(!product_equal(path,PW("Software\\OsitoK\\Installer\\Products\\{13A4EE12-23EA-3371-91EE-EFB36DDFFF3E}")))return 2;
    opened++;*out=(HKEY)0x1234;return 0;
}
LONG WINAPI RegCloseKey(HKEY key){product_check(key==(HKEY)0x1234,__LINE__);closed++;return 0;}
LONG WINAPI RegQueryValueExW(HKEY key,PCWSTR name,DWORD *reserved,DWORD *type,BYTE *data,DWORD *size)
{
    product_check(key==(HKEY)0x1234&&!reserved,__LINE__);
    if(missing_name&&product_equal(name,missing_name))return 2;
    const void *value=NULL;DWORD bytes=4;*type=REG_DWORD;
    if(product_equal(name,PW("State")))value=&state;
    if(product_equal(name,PW("SchemaVersion")))value=&schema;
    if(product_equal(name,PW("Context")))value=&context;
    static const struct {PCWSTR name,text;} values[]={
        {PW("ProductVersion"),PW("12.0.21005")},{PW("ProductName"),PW("Microsoft Visual C++ 2013")},
        {PW("Manufacturer"),PW("Microsoft Corporation")},{PW("ProductLanguage"),PW("1033")},
        {PW("PackageCode"),PW("{E9934153-EAB1-4DA6-AA72-86C8BB1EDF2C}")},
        {PW("LocalPackage"),PW("C:\\Windows\\Installer\\{E9934153-EAB1-4DA6-AA72-86C8BB1EDF2C}.msi")},
        {PW("InstallSource"),PW("C:\\minimum\\")}
    };
    for(unsigned i=0;i<sizeof(values)/sizeof(*values);i++)if(product_equal(name,values[i].name)){
        value=values[i].text;bytes=(product_length(values[i].text)+1)*2;*type=REG_SZ;
    }
    if(!value)return 2;
    if(bad_type&&product_equal(name,bad_type))*type=REG_BINARY;
    if(growing&&data&&product_equal(name,growing)){*size=bytes+2;return 234;}
    if(data&&*size<bytes){*size=bytes;return 234;}
    if(data)memcpy(data,value,bytes);*size=bytes;return 0;
}
#include "msi_products_checks.h"
int main(void)
{
    product_negative_checks();product_installed_checks();
    PCWSTR required[]={PW("State"),PW("SchemaVersion"),PW("Context"),PW("ProductVersion")};
    for(unsigned i=0;i<4;i++){
        WCHAR text[32];text[0]='Z';DWORD size=32;
        missing_name=required[i];
        PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,PW("VersionString"),text,&size)==(i?1610U:1605U));
        PRODUCT_CHECK(text[0]=='Z'&&size==32);missing_name=NULL;
        bad_type=required[i];PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,PW("VersionString"),text,&size)==1610);bad_type=NULL;
    }
    fail_alloc=1;PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,PW("VersionString"),NULL,NULL)==8);fail_alloc=0;
    state=2;PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,PW("VersionString"),NULL,NULL)==1605);state=5;
    schema=2;PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,PW("VersionString"),NULL,NULL)==1610);schema=1;
    context=1;PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,PW("VersionString"),NULL,NULL)==1610);context=4;
    growing=PW("ProductVersion");PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,PW("VersionString"),NULL,NULL)==1610);growing=NULL;
    fail_read=5;PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,PW("VersionString"),NULL,NULL)==5);fail_read=0;
    WCHAR text[32];DWORD size=32;
    deny_read=product_minimum;PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,PW("VersionString"),text,&size)==87);deny_read=NULL;
    deny_write=text;PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,PW("VersionString"),text,&size)==87&&size==32);deny_write=NULL;
    DWORD parts[3];PRODUCT_CHECK(product_version(PW("3.12.10150.0"),parts)==0&&parts[0]==3&&parts[1]==12&&parts[2]==10150);
    PRODUCT_CHECK(product_version(PW("256.0.0"),parts)==1610);
    PRODUCT_CHECK(product_version(PW("1.256.0"),parts)==1610);
    PRODUCT_CHECK(product_version(PW("1.0.65536"),parts)==1610);
    PRODUCT_CHECK(product_version(PW("1.0"),parts)==1610);
    PRODUCT_CHECK(!allocations&&opened==closed);
    printf("MSIPRODUCT HOST checks=%u failures=%u allocations=%u handles=%u/%u\n",checks,failures,allocations,opened,closed);
    return failures?1:0;
}
