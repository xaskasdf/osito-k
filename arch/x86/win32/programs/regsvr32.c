/* Osito-K DLL registration utility. Microsoft runtime payloads stay external. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API const WORD *CALL GetCommandLineW(void);
API WORD **CALL CommandLineToArgvW(const WORD *,int *);
API void *CALL LocalAlloc(DWORD,__SIZE_TYPE__);
API void *CALL LocalFree(void *);
API void *CALL LoadLibraryExW(const WORD *,void *,DWORD);
API void *CALL GetProcAddress(void *,const char *);
API int CALL FreeLibrary(void *);
API DWORD CALL GetLastError(void);
API DWORD CALL SetErrorMode(DWORD);
API int CALL OleInitialize(void *);
API void CALL OleUninitialize(void);
API int CALL MessageBoxW(void *,const WORD *,const WORD *,DWORD);
#define W(s) ((const WORD *)L##s)

static int silent;

static void regsvr_message(const WORD *message,const WORD *file,DWORD code)
{
    if(silent)return;
    WORD *text=LocalAlloc(0,2048*sizeof(WORD));
    if(!text)return;
    unsigned length=0;
    while(*message&&length<1900)text[length++]=*message++;
    if(file) {
        text[length++]='\n';
        while(*file&&length<1980)text[length++]=*file++;
    }
    if(code) {
        text[length++]='\n';text[length++]='0';text[length++]='x';
        for(int i=7;i>=0;i--)text[length++]="0123456789abcdef"[(code>>(i*4))&15];
    }
    text[length]=0;
    MessageBoxW(0,text,W("RegSvr32"),0);
    LocalFree(text);
}

static int regsvr_operation(void *module,const WORD *file,int install,
                            int unregister,const WORD *command)
{
    const char *name=install?"DllInstall":unregister?"DllUnregisterServer":"DllRegisterServer";
    void *procedure=GetProcAddress(module,name);
    if(!procedure) {
        regsvr_message(install?W("The module does not export DllInstall."):
                      unregister?W("The module does not export DllUnregisterServer."):
                      W("The module does not export DllRegisterServer."),file,GetLastError());
        return 4;
    }
    int status=install?((int (CALL *)(int,const WORD *))procedure)(!unregister,command):
                       ((int (CALL *)(void))procedure)();
    if(status<0) {
        regsvr_message(install?W("DllInstall failed."):
                      unregister?W("DllUnregisterServer failed."):W("DllRegisterServer failed."),file,(DWORD)status);
        return 5;
    }
    regsvr_message(install?W("DllInstall succeeded."):
                  unregister?W("DllUnregisterServer succeeded."):W("DllRegisterServer succeeded."),file,0);
    return 0;
}

static int regsvr_run(int count,WORD **arguments)
{
    int unregister=0,install=0,skip_register=0,invalid=0,files=0;
    const WORD *command=W("");
    /* Windows reads all switches before processing any module. It accepts
     * either prefix, ignores suffixes on ordinary switches, and uses the
     * last /i command. Quoting is handled by CommandLineToArgvW. */
    for(int i=1;i<count;i++) {
        WORD *arg=arguments[i];
        if(arg[0]!='/'&&arg[0]!='-'){files++;continue;}
        WORD option=arg[1];if(option>='A'&&option<='Z')option+='a'-'A';
        switch(option) {
        case 's':silent=1;break;
        case 'u':unregister=1;break;
        case 'n':skip_register=1;break;
        case 'c':break; /* Accepted legacy console flag. */
        case 'i':install=1;command=arg[2]==':'?arg+3:W("");break;
        default:invalid=1;break;
        }
        arguments[i]=0;
    }
    if(invalid||!files||(skip_register&&!install)) {
        regsvr_message(W("Usage: regsvr32 [/u] [/s] [/n] [/i[:command]] module.dll\n"
                        "/u unregisters, /s is silent, /n requires /i and skips registration."),0,0);
        return 1;
    }
    int initialized=OleInitialize(0);
    if(initialized<0){regsvr_message(W("Unable to initialize OLE."),0,(DWORD)initialized);return 2;}
    SetErrorMode(0x8001); /* A failed silent load must not open a system dialog. */
    int result=0;
    for(int i=1;i<count;i++) {
        const WORD *file=arguments[i];if(!file)continue;
        void *module=LoadLibraryExW(file,0,8); /* LOAD_WITH_ALTERED_SEARCH_PATH */
        if(!module){regsvr_message(W("Unable to load the module."),file,GetLastError());result=3;break;}
        int status=0;
        if(install&&unregister)status=regsvr_operation(module,file,1,1,command);
        if(!status&&!skip_register)status=regsvr_operation(module,file,0,unregister,command);
        if(!status&&install&&!unregister)status=regsvr_operation(module,file,1,0,command);
        FreeLibrary(module);
        if(status)result=status;
    }
    OleUninitialize();
    return result;
}

void regsvr_entry(void)
{
    int count=0;
    WORD **arguments=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!arguments)ExitProcess(1);
    int result=regsvr_run(count,arguments);
    LocalFree(arguments);
    ExitProcess((DWORD)result);
}
