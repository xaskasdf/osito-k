#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#define OSITOK_X86_TYPES_H
#define __KERNEL_X86__ 1
#define W(s) ((PCWSTR)u##s)
#include "../win32/binary_type.c"
static DWORD last_error;
static int compat;
static unsigned checks,failures,opened,closed,reads,seeks;
static int fail_open,fail_read=-1,fail_seek=-1;
static DWORD max_read=0xffffffffU,file_type=1;
static const void *deny_read,*deny_write;
static const char *fixture_root;
static void check(int ok,unsigned test,unsigned item)
{checks++;if(!ok){failures++;if(failures<40)printf("BINARY FAIL case=%u item=%u\n",test,item);}}
#define CHECK_CASE(c,t,i) check((c),(t),(i))
int *proc_win32_compat32_mode_slot(void){return &compat;}
int win32_user_range_readable(const void *p,SIZE_T n,BOOL mode)
{(void)n;(void)mode;return (uintptr_t)p>=4096&&p!=deny_read;}
int win32_user_range_writable(void *p,SIZE_T n,BOOL mode)
{(void)n;(void)mode;return (uintptr_t)p>=4096&&p!=deny_write;}
void WINAPI SetLastError(DWORD value){last_error=value;}
DWORD WINAPI GetLastError(void){return last_error;}
static void host_path(PCWSTR path,char output[1024])
{
    size_t n=strlen(fixture_root);memcpy(output,fixture_root,n);output[n++]='/';
    for(unsigned i=0;path[i]&&n<1019;i++) {
        uint32_t cp=path[i];if(cp=='\\')cp='/';
        if(cp<128)output[n++]=(char)cp;
        else if(cp<2048){output[n++]=(char)(0xc0|(cp>>6));output[n++]=(char)(0x80|(cp&63));}
        else {output[n++]=(char)(0xe0|(cp>>12));output[n++]=(char)(0x80|((cp>>6)&63));output[n++]=(char)(0x80|(cp&63));}
    }
    output[n]=0;
}
DWORD WINAPI GetFileAttributesW(PCWSTR path)
{
    char name[1024];host_path(path,name);struct stat st;
    if(stat(name,&st)){SetLastError(errno==EACCES?5:2);return 0xffffffffU;}
    SetLastError(0);return S_ISDIR(st.st_mode)?16:32;
}
HANDLE WINAPI CreateFileW(PCWSTR path,DWORD access,DWORD share,PVOID security,DWORD creation,DWORD flags,HANDLE template)
{
    CHECK_CASE(access==GENERIC_READ&&share==7&&!security&&creation==3&&!flags&&!template,2000,0);
    if(fail_open){SetLastError(5);return INVALID_HANDLE_VALUE;}
    char name[1024];host_path(path,name);FILE *file=fopen(name,"rb");
    if(!file){SetLastError(2);return INVALID_HANDLE_VALUE;}
    opened++;return file;
}
BOOL WINAPI CloseHandle(HANDLE file)
{closed++;SetLastError(777);return fclose(file)==0;}
DWORD WINAPI GetFileType(HANDLE file){(void)file;return file_type;}
DWORD WINAPI GetFileSize(HANDLE file,DWORD *high)
{
    struct stat st;if(fstat(fileno(file),&st)){SetLastError(6);return 0xffffffffU;}
    *high=(DWORD)((uint64_t)st.st_size>>32);return (DWORD)st.st_size;
}
BOOL WINAPI ReadFile(HANDLE file,PVOID buffer,DWORD size,DWORD *read,PVOID overlapped)
{
    CHECK_CASE(!overlapped,2001,0);
    if((int)reads++==fail_read){SetLastError(23);return FALSE;}
    if(size>max_read)size=max_read;
    *read=(DWORD)fread(buffer,1,size,file);return !ferror(file);
}
DWORD WINAPI SetFilePointer(HANDLE file,LONG low,LONG *high,DWORD method)
{
    CHECK_CASE(method==0&&high,2002,0);
    if((int)seeks++==fail_seek){SetLastError(25);return 0xffffffffU;}
    uint64_t offset=(uint32_t)low|((uint64_t)(uint32_t)*high<<32);
    if(fseeko(file,(off_t)offset,SEEK_SET)){SetLastError(25);return 0xffffffffU;}
    return (DWORD)offset;
}
#define BINARY_CALLER_32 compat
#include "binary_type_checks.h"
int main(int argc,char **argv)
{
    if(argc!=2)return 2;fixture_root=argv[1];
    binary_contract_checks();compat=1;binary_contract_checks();compat=0;
    PCWSTR path=W("binary-fixtures\\r1-valid32.exe");DWORD type=0xabababab;
    reads=seeks=0;CHECK_CASE(GetBinaryTypeW(path,&type)&&type==0,3000,0);
    unsigned baseline_reads=reads,baseline_seeks=seeks;
    for(unsigned i=0;i<baseline_reads;i++) {
        reads=0;fail_read=(int)i;type=0xabababab;
        CHECK_CASE(!GetBinaryTypeW(path,&type)&&GetLastError()==23&&type==0xabababab,3001,i);
        CHECK_CASE(opened==closed,3002,i);
    }
    fail_read=-1;
    for(unsigned i=0;i<baseline_seeks;i++) {
        seeks=0;fail_seek=(int)i;type=0xabababab;
        CHECK_CASE(!GetBinaryTypeW(path,&type)&&GetLastError()==25&&type==0xabababab,3003,i);
        CHECK_CASE(opened==closed,3004,i);
    }
    fail_seek=-1;max_read=3;CHECK_CASE(GetBinaryTypeW(path,&type)&&type==0,3005,0);max_read=0xffffffffU;
    fail_open=1;CHECK_CASE(!GetBinaryTypeW(path,&type)&&GetLastError()==5,3005,1);fail_open=0;
    file_type=2;CHECK_CASE(!GetBinaryTypeW(path,&type)&&GetLastError()==5,3005,2);file_type=1;
    deny_read=path;CHECK_CASE(!GetBinaryTypeW(path,&type)&&GetLastError()==87,3005,3);deny_read=NULL;
    deny_write=&type;CHECK_CASE(!GetBinaryTypeW(path,&type)&&GetLastError()==87,3005,4);deny_write=NULL;
    CHECK_CASE(!GetBinaryTypeW(path,NULL)&&GetLastError()==87,3005,5);
    CHECK_CASE(opened==closed,3006,0);
    printf("BINARY HOST checks=%u failures=%u handles=%u/%u\n",checks,failures,opened,closed);
    return failures?1:0;
}
