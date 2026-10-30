/* Real MSI API client. --install is usable independently of the guest tests;
 * the other modes require the disposable QEMU Minimum/Additional fixtures. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef void *HANDLE;
#define NULL ((void *)0)
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define W(s) ((const WCHAR *)L##s)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API WCHAR *CALL GetCommandLineW(void);
API DWORD CALL GetModuleFileNameW(HANDLE,WCHAR *,DWORD);
API DWORD CALL GetFileAttributesW(const WCHAR *);
API DWORD CALL GetTickCount(void);
API void CALL Sleep(DWORD);
API HANDLE CALL CreateThread(void *,__SIZE_TYPE__,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
API DWORD CALL WaitForSingleObject(HANDLE,DWORD);
API int CALL GetExitCodeThread(HANDLE,DWORD *);
API int CALL GetExitCodeProcess(HANDLE,DWORD *);
API int CALL CloseHandle(HANDLE);
API int CALL TerminateThread(HANDLE,DWORD);
API int CALL TerminateProcess(HANDLE,DWORD);
API DWORD CALL MsiInstallProductW(const WCHAR *,const WCHAR *);
API DWORD CALL MsiGetProductInfoW(const WCHAR *,const WCHAR *,WCHAR *,DWORD *);
typedef struct {
    DWORD cb; WCHAR *reserved,*desktop,*title;
    DWORD x,y,width,height,columns,rows,fill,flags;
    unsigned short show,reserved_bytes; unsigned char *reserved_data;
    HANDLE input,output,error;
} STARTUP;
typedef struct { HANDLE process,thread; DWORD pid,tid; } PROCESS;
API int CALL CreateProcessW(const WCHAR *,WCHAR *,void *,void *,int,DWORD,void *,const WCHAR *,STARTUP *,PROCESS *);
static unsigned checks,failures;
static const WCHAR *minimum=W("C:\\minimum\\runtime.msi");
static const WCHAR *additional=W("C:\\additional\\runtime.msi");
static const WCHAR *properties=W("MSIFASTINSTALL=7 NOVSUI=1");
static const WCHAR *minimum_product=W("{13A4EE12-23EA-3371-91EE-EFB36DDFFF3E}");
static const WCHAR *additional_product=W("{F8CFEB22-A2E7-3971-9EDA-4B11EDEFC185}");
void *memcpy(void *out,const void *in,__SIZE_TYPE__ size)
{unsigned char *d=out;const unsigned char *s=in;for(__SIZE_TYPE__ i=0;i<size;i++)d[i]=s[i];return out;}
void *memset(void *out,int value,__SIZE_TYPE__ size)
{unsigned char *p=out;for(__SIZE_TYPE__ i=0;i<size;i++)p[i]=(unsigned char)value;return out;}
static void install_say(const char *text)
{DWORD size=0,written;while(text[size])size++;WriteFile(GetStdHandle((DWORD)-11),text,size,&written,NULL);}
static void install_hex(DWORD value)
{char text[9];for(unsigned i=0;i<8;i++)text[i]="0123456789abcdef"[(value>>((7-i)*4))&15];text[8]=0;install_say(text);}
static void install_check(int condition,unsigned line)
{checks++;if(!condition){failures++;install_say("MSIINSTALL FAIL line=");install_hex(line);install_say("\n");}}
#define CHECK(c) install_check((c),__LINE__)
static int install_equal(const WCHAR *a,const WCHAR *b)
{while(*a&&*a==*b){a++;b++;}return *a==*b;}
static const WCHAR *install_argument(const WCHAR *command,WCHAR *text,unsigned capacity)
{
    while(*command==' '||*command=='\t')command++;
    unsigned length=0;int quote=0;
    while(*command&&(quote||(*command!=' '&&*command!='\t'))){
        if(*command=='"')quote=!quote;
        else {if(length+1>=capacity)return NULL;text[length++]=*command;}
        command++;
    }
    if(quote)return NULL;
    text[length]=0;
    while(*command==' '||*command=='\t')command++;
    return command;
}
static void install_absent(int extra)
{
    const WCHAR *product=extra?additional_product:minimum_product;
    const WCHAR *file=extra?W("C:\\Windows\\SysWOW64\\mfc120u.dll"):W("C:\\Windows\\SysWOW64\\msvcr120.dll");
    CHECK(MsiGetProductInfoW(product,W("VersionString"),NULL,NULL)==1605);
    CHECK(GetFileAttributesW(file)==0xffffffffU);
}
static int install_wait_busy(void)
{
    DWORD start=GetTickCount();
    do {
        Sleep(1);
        DWORD error=MsiInstallProductW(W("C:\\msi-cancellation-missing.msi"),NULL);
        if(error==1618){install_say("MSIINSTALL PROTECTED OWNER OBSERVED\n");return 1;}
        if(error!=1619)return 0;
    } while((DWORD)(GetTickCount()-start)<30000);
    return 0;
}
static DWORD install_when_available(const WCHAR *path)
{
    DWORD start=GetTickCount(),error;
    do {
        error=MsiInstallProductW(path,properties);
        if(error!=1618)return error;
        Sleep(1);
    } while((DWORD)(GetTickCount()-start)<30000);
    return error;
}
static DWORD CALL install_worker(void *context)
{(void)context;return install_when_available(minimum);}
static void install_cancel_thread(void)
{
    HANDLE thread=CreateThread(NULL,0,install_worker,NULL,0,NULL);CHECK(thread!=NULL);
    if(!thread)return;
    int busy=install_wait_busy();CHECK(busy);
    if(busy)CHECK(TerminateThread(thread,23));
    CHECK(WaitForSingleObject(thread,60000)==0);
    DWORD status=99;CHECK(GetExitCodeThread(thread,&status)&&status==23);
    CHECK(CloseHandle(thread));
    install_absent(0);
}
static void install_cancel_process(void)
{
    WCHAR path[260],command[]=L"msiinstall --child";
    CHECK(GetModuleFileNameW(NULL,path,260)>0);
    STARTUP startup={0};PROCESS process={0};startup.cb=sizeof(startup);
    int created=CreateProcessW(path,command,NULL,NULL,0,0,NULL,NULL,&startup,&process);
    CHECK(created);
    if(!created)return;
    int busy=install_wait_busy();CHECK(busy);
    if(busy)CHECK(TerminateProcess(process.process,23));
    CHECK(WaitForSingleObject(process.process,60000)==0);
    DWORD status=99;CHECK(GetExitCodeProcess(process.process,&status)&&status==23);
    CHECK(CloseHandle(process.thread));CHECK(CloseHandle(process.process));
    install_absent(1);
}
void msi_install_entry(void)
{
    WCHAR argument[260];const WCHAR *command=install_argument(GetCommandLineW(),argument,260);
    if(command)command=install_argument(command,argument,260);
    if(!command){install_say("MSIINSTALL invalid arguments\n");ExitProcess(87);return;}
    if(install_equal(argument,W("--install"))){
        command=install_argument(command,argument,260);
        DWORD result=command?MsiInstallProductW(argument,*command?command:NULL):87;
        install_say("MSIINSTALL RESULT=");install_hex(result);install_say("\n");ExitProcess(result);return;
    }
    if(install_equal(argument,W("--child"))){ExitProcess(install_when_available(additional));return;}
    if(install_equal(argument,W("--noadmin"))){
        install_absent(0);
        CHECK(MsiInstallProductW(minimum,properties)==5);
        install_absent(0);
    }else{
        int extra=install_equal(argument,W("--process"));
        if(!extra&&!install_equal(argument,W("--thread"))){install_say("MSIINSTALL usage: --install <package.msi> [properties]\n");ExitProcess(87);return;}
        CHECK(MsiInstallProductW(NULL,NULL)==87);
        CHECK(MsiInstallProductW(W(""),NULL)==87);
        CHECK(MsiInstallProductW((void *)1,NULL)==87);
        CHECK(MsiInstallProductW(minimum,(void *)1)==87);
        CHECK(MsiInstallProductW(W("C:\\msi-cancellation-missing.msi"),NULL)==1619);
        install_absent(extra);
        if(extra)install_cancel_process();else install_cancel_thread();
        CHECK(MsiInstallProductW(extra?additional:minimum,properties)==0);
        CHECK(MsiGetProductInfoW(extra?additional_product:minimum_product,W("VersionString"),NULL,NULL)==0);
        CHECK(GetFileAttributesW(extra?W("C:\\Windows\\SysWOW64\\mfc120u.dll"):W("C:\\Windows\\SysWOW64\\msvcr120.dll"))!=0xffffffffU);
        CHECK(MsiInstallProductW(extra?additional:minimum,properties)==1638);
    }
    install_say("MSIINSTALL DONE checks=");install_hex(checks);install_say(" failures=");install_hex(failures);install_say("\n");
    ExitProcess(failures?1:0);
}
