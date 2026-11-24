/* Custom regsvr32 oracle: writes only a local diagnostic file, never registry. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void *CALL CreateFileA(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API int CALL CloseHandle(void *);
API DWORD CALL GetEnvironmentVariableA(const char *,char *,DWORD);
API int CALL CoInitializeEx(void *,DWORD);
API void CALL CoUninitialize(void);

static void regsvr_fixture_event(char event,const WORD *command,int com)
{
    char text[1200]; unsigned length=0;
    text[length++]=event;
    if(com) {
        DWORD status=CoInitializeEx(0,0);
        text[length++]=':';
        for(int i=7;i>=0;i--)text[length++]="0123456789abcdef"[(status>>(i*4))&15];
        if((int)status>=0)CoUninitialize();
    }
    if(event=='I'||event=='X') {
        text[length++]=':';
        if(!command)text[length++]='!';
        else for(unsigned i=0;command[i]&&length<sizeof(text)-5;i++)
            for(int j=3;j>=0;j--)text[length++]="0123456789abcdef"[(command[i]>>(j*4))&15];
    }
    text[length++]=';';
    void *file=CreateFileA("regsvr-events.log",4,3,0,4,0x80,0);
    if(file!=(void *)(UPTR)-1) {DWORD done;WriteFile(file,text,length,&done,0);CloseHandle(file);}
}

#ifndef MISSING_EXPORTS
static int regsvr_fixture_result(char event)
{
    char value[16];
    if(GetEnvironmentVariableA("OSITO_REGSVR_FAIL",value,sizeof(value))&&value[0]==event)
        return (int)0x80004005;
    if(GetEnvironmentVariableA("OSITO_REGSVR_S_FALSE",value,sizeof(value))&&value[0]=='1')
        return 1;
    return 0;
}
int CALL DllRegisterServer(void)
{regsvr_fixture_event('R',0,1);return regsvr_fixture_result('R');}
int CALL DllUnregisterServer(void)
{regsvr_fixture_event('U',0,1);return regsvr_fixture_result('U');}
int CALL DllInstall(int install,const WORD *command)
{char event=install?'I':'X';regsvr_fixture_event(event,command,1);return regsvr_fixture_result(event);}
#endif

int CALL regsvr_fixture_main(void *module,DWORD reason,void *reserved)
{
    (void)module;(void)reserved;
    if(reason==1)regsvr_fixture_event('A',0,0);
    if(reason==0)regsvr_fixture_event('D',0,0);
    return 1;
}
