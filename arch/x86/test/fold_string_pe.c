typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef void *HANDLE;
#define NULL ((void *)0)
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API void CALL SetLastError(DWORD);
API DWORD CALL GetLastError(void);
API int CALL FoldStringW(DWORD,const WCHAR *,int,WCHAR *,int);
API DWORD CALL GetShortPathNameW(const WCHAR *,WCHAR *,DWORD);
static unsigned checks,failures;
void *memset(void *p,int value,__SIZE_TYPE__ size)
{volatile unsigned char *out=p;for(__SIZE_TYPE__ i=0;i<size;i++)out[i]=(unsigned char)value;return p;}
static void say(const char *text)
{DWORD n=0,w;while(text[n])n++;WriteFile(GetStdHandle((DWORD)-11),text,n,&w,0);}
static void hex(DWORD value)
{char text[9];for(unsigned i=0;i<8;i++)text[i]="0123456789abcdef"[(value>>((7-i)*4))&15];text[8]=0;say(text);}
static void check(int ok,unsigned test,unsigned item)
{checks++;if(!ok){failures++;if(failures<30){say("FOLD FAIL case=");hex(test);say(" item=");hex(item);say("\n");}}}
#define CHECK_CASE(c,t,i) check((c),(t),(i))
#include "fold_string_checks.h"
void fold_string_entry(void)
{
    fold_contract_checks();
    say("FOLD DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");
    ExitProcess(failures?1:0);
}
