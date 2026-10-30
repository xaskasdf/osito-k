typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef void *HANDLE;
#define NULL ((void *)0)
#define W(s) ((const WCHAR *)u##s)
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API void CALL SetLastError(DWORD);
API DWORD CALL GetLastError(void);
API int CALL GetBinaryTypeW(const WCHAR *,DWORD *);
static unsigned checks,failures;
static void say(const char *text)
{DWORD n=0,w;while(text[n])n++;WriteFile(GetStdHandle((DWORD)-11),text,n,&w,0);}
static void hex(DWORD value)
{char text[9];for(unsigned i=0;i<8;i++)text[i]="0123456789abcdef"[(value>>((7-i)*4))&15];text[8]=0;say(text);}
static void check(int ok,unsigned test,unsigned item)
{checks++;if(!ok){failures++;if(failures<40){say("BINARY FAIL case=");hex(test);say(" item=");hex(item);say("\n");}}}
#define CHECK_CASE(c,t,i) check((c),(t),(i))
#define BINARY_CALLER_32 (sizeof(void *)==4)
#include "binary_type_checks.h"
void binary_type_entry(void)
{
    binary_contract_checks();
    say("BINARY DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");
    ExitProcess(failures?1:0);
}
