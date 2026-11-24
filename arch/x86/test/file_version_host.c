#include <stdio.h>
#include <stdlib.h>
#include "../win32/version_shim.c"
static int compat32;
int *proc_win32_compat32_mode_slot(void){return &compat32;}
static DWORD last_error;
static long allocations,handles,fail_after=-1;
void serial_puts(const char *s){(void)s;}
void WINAPI SetLastError(DWORD error){last_error=error;}
DWORD WINAPI GetLastError(void){return last_error;}
HANDLE WINAPI LocalAlloc(UINT flags,SIZE_T bytes){(void)flags;if(fail_after==0)return NULL;if(fail_after>0)fail_after--;void *p=malloc(bytes);if(p)allocations++;return p;}
HANDLE WINAPI LocalFree(HANDLE p){if(p)allocations--;free(p);return NULL;}
int WINAPI MultiByteToWideChar(DWORD cp,DWORD flags,PCSTR s,int n,PWSTR out,int cap)
{(void)cp;(void)flags;if(n<0)n=(int)strlen(s)+1;if(!out)return n;if(cap<n)return 0;for(int i=0;i<n;i++)out[i]=(unsigned char)s[i];return n;}
int WINAPI WideCharToMultiByte(DWORD cp,DWORD flags,PCWSTR s,int n,PSTR out,int cap,PCSTR def,BOOL *used)
{(void)cp;(void)flags;(void)def;(void)used;if(n<0){n=0;while(s[n])n++;n++;}if(!out)return n;if(cap<n)return 0;for(int i=0;i<n;i++)out[i]=(char)s[i];return n;}
/* The parser harness uses explicit host paths. Loader search is exercised by
 * version_search_pe.c against Windows and the complete kernel. */
BOOL dll_resolve_data_path(const char *name,char path[260])
{if(name[0]!='/'||strlen(name)>=260){SetLastError(206);return FALSE;}strcpy(path,name+1);return TRUE;}
BOOL nt_thread_fs_redirection(int disabled)
{static BOOL state;BOOL previous=state;if(disabled>=0)state=disabled;return previous;}
DWORD WINAPI GetUserDefaultLCID(void){return 0x409;}
int WINAPI GetLocaleInfoW(DWORD locale,DWORD type,PWSTR data,int chars)
{(void)locale;(void)type;if(chars<6)return 0;const char *s="en-US";for(int i=0;i<6;i++)data[i]=s[i];return 6;}
HANDLE WINAPI CreateFileW(PCWSTR name,DWORD access,DWORD share,PVOID sa,DWORD creation,DWORD flags,HANDLE tmpl)
{(void)access;(void)share;(void)sa;(void)creation;(void)flags;(void)tmpl;char path[2048];unsigned i=0;for(;name[i]&&i<2047;i++)path[i]=name[i]=='\\'?'/':(char)name[i];path[i]=0;FILE *f=fopen(path,"rb");if(!f){SetLastError(2);return INVALID_HANDLE_VALUE;}handles++;return f;}
BOOL WINAPI CloseHandle(HANDLE h){handles--;return fclose(h)==0;}
DWORD WINAPI GetFileSize(HANDLE h,DWORD *high){long pos=ftell(h);fseek(h,0,SEEK_END);long n=ftell(h);fseek(h,pos,SEEK_SET);if(high)*high=0;return n;}
DWORD WINAPI SetFilePointer(HANDLE h,LONG pos,LONG *high,DWORD how){(void)high;(void)how;if(fseek(h,pos,SEEK_SET))return 0xffffffffu;return ftell(h);}
BOOL WINAPI ReadFile(HANDLE h,PVOID data,DWORD bytes,DWORD *read,PVOID ov){(void)ov;*read=fread(data,1,bytes,h);return !ferror(h);}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    WCHAR file[2048];MultiByteToWideChar(0,0,argv[1],-1,file,2048);
    for(unsigned flags=0;flags<12;flags++) {
        DWORD h=0xa5;SetLastError(0x1234);DWORD size=shim_GetFileVersionInfoSizeExW(flags,file,&h);
        printf("SIZE %u %u %u %u\n",flags,size,GetLastError(),h);
        if(size==1004) {
            BYTE data[1200];PVOID value=NULL;UINT chars=0;
            if(!shim_GetFileVersionInfoExW(flags,file,0,sizeof(data),data) ||
               !shim_VerQueryValueA(data,"\\StringFileInfo\\040904b0\\FileVersion",&value,&chars) ||
               chars!=11 || strcmp(value,"7.11.13.17"))return 5;
            WCHAR key[80];MultiByteToWideChar(0,0,"\\StringFileInfo\\040904b0\\FileVersion",-1,key,80);
            if(!shim_VerQueryValueW(data,key,&value,&chars) || chars!=11 || ((WCHAR *)value)[0]!='7')return 6;
        }
        for(unsigned length=0;length<1100;length++) {
            BYTE buffer[1200];memset(buffer,0xa5,sizeof(buffer));SetLastError(0x1234);
            BOOL ok=shim_GetFileVersionInfoExW(flags,file,99,length,buffer);DWORD error=GetLastError();
            unsigned hash=2166136261u;for(unsigned i=0;i<sizeof(buffer);i++)hash=(hash^buffer[i])*16777619u;
            printf("ROW %u %u %u %u %08x\n",flags,length,ok,error,hash);
        }
    }
    for(fail_after=0;fail_after<16;fail_after++) {
        long budget=fail_after;DWORD h;shim_GetFileVersionInfoSizeExW(3,file,&h);
        if(allocations||handles)return 4;
        fail_after=budget;
    }
    return allocations||handles?3:0;
}
