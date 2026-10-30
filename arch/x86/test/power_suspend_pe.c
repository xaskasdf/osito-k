/* Guest-only: a native Windows call could suspend the development machine. */
typedef unsigned int DWORD;
typedef __SIZE_TYPE__ SIZE;
typedef int BOOL;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API char *CALL GetCommandLineA(void);
static void report(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void hex(DWORD v)
{ char s[9];for(int i=7;i>=0;i--){s[i]="0123456789abcdef"[v&15];v>>=4;}s[8]=0;report(s); }
void power_suspend_entry(void)
{
    const char *s=GetCommandLineA(),*flag="--osito-guest";
    BOOL guest=0;
    for(DWORD i=0;s&&s[i];i++){
        DWORD j=0;while(flag[j]&&s[i+j]==flag[j])j++;
        if(!flag[j])guest=1;
    }
    if(!guest){report("POWER-SUSPEND SKIP requires --osito-guest\n");ExitProcess(2);}
    BOOL (CALL *suspend)(BOOL,BOOL)=GetProcAddress(LoadLibraryA("kernel32.dll"),"SetSystemPowerState");
    if(!suspend){report("POWER-SUSPEND MISSING\n");ExitProcess(2);}
    const BOOL values[]={0,1,-1,256};DWORD checks=0,failures=0;
    for(DWORD i=0;i<4;i++)for(DWORD j=0;j<4;j++){
        SetLastError(0x12345678);BOOL ok=suspend(values[i],values[j]);DWORD error=GetLastError();
        checks+=2;if(ok)failures++;if(error!=50)failures++;
    }
    report("POWER-SUSPEND DONE checks=");hex(checks);report(" failures=");hex(failures);report("\n");
    ExitProcess(failures?1:0);
}
