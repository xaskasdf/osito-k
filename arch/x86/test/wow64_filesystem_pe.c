/* Native read-only Windows oracle; writes are restricted to disposable guests. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void CALL SetLastError(DWORD);
API DWORD CALL GetLastError(void);
static void *(CALL *openA)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
static void *(CALL *openW)(const WCHAR *,DWORD,DWORD,void *,DWORD,DWORD,void *);
static int (CALL *readfile)(void *,void *,DWORD,DWORD *,void *);
static DWORD (CALL *seekfile)(void *,int,int *,DWORD);
static int (CALL *closefile)(void *);
static int (CALL *disable)(void **);
static int (CALL *revert)(void *);
static unsigned char (CALL *enable)(unsigned char);
static int (CALL *setcwd)(const char *);
static DWORD (CALL *getcwd)(DWORD,char *);
static char windows[260],system_file[260];
static const char *file_name;
static unsigned failures,checks;
static void wow_report(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void wow_number(UPTR n)
{ char b[22];unsigned i=21;b[i]=0;do{b[--i]=(char)('0'+n%10);n/=10;}while(n);wow_report(b+i); }
#define CHECK(ok,label) do {int pass=!!(ok);checks++;wow_report(pass?"WOW64 PASS ":"WOW64 FAIL ");wow_report(label);wow_report("\n");if(!pass)failures++;}while(0)
#define VALUE(label,value) do {wow_report("WOW64 VALUE " label "=");wow_number((UPTR)(value));wow_report("\n");}while(0)
static void wow_path(char out[260],const char *directory,const char *name)
{
    unsigned i=0;
    while(windows[i]&&i<259){out[i]=windows[i];i++;}
    if(i<259)out[i++]='\\';
    while(*directory&&i<259)out[i++]=*directory++;
    if(*name&&i<259)out[i++]='\\';
    while(*name&&i<259)out[i++]=*name++;
    out[i]=0;
}
static DWORD wow_machine_handle(void *handle)
{
    unsigned char header[64];DWORD got=0,machine=0;
    if(!handle||handle==(void *)(UPTR)-1)return 0;
    if(readfile(handle,header,64,&got,0)&&got==64&&header[0]=='M'&&header[1]=='Z'){
        DWORD offset=*(DWORD *)(header+60);
        if(offset<0x100000&&seekfile(handle,(int)offset,0,0)==offset&&
           readfile(handle,header,6,&got,0)&&got==6&&header[0]=='P'&&header[1]=='E')
            machine=*(WCHAR *)(header+4);
    }
    closefile(handle);return machine;
}
static DWORD wow_machine(const char *path)
{ return wow_machine_handle(openA(path,0x80000000,7,0,3,0,0)); }
static DWORD CALL wow_thread(void *unused)
{ (void)unused;return wow_machine(system_file); }
static DWORD wow_root_relative(const char *directory)
{
    typedef struct {WCHAR length,maximum;WCHAR *buffer;} US;
    typedef struct {DWORD length;void *root;US *name;DWORD attributes;void *security,*qos;} OA;
    typedef struct {UPTR status,info;} IOS;
    typedef int (CALL *ntcreatefn)(void **,DWORD,OA *,IOS *,void *,DWORD,DWORD,DWORD,DWORD,void *,DWORD);
    ntcreatefn ntcreate=GetProcAddress(LoadLibraryA("ntdll.dll"),"NtCreateFile");
    void *root=openA(directory,0x80000000,7,0,3,0x02000000,0);
    struct {void *file;DWORD guard;} output={0,0x6543210};
    if(root==(void *)(UPTR)-1||!ntcreate)return 0;
    WCHAR name[260];unsigned n=0;while(file_name[n]){name[n]=(WCHAR)file_name[n];n++;}name[n]=0;
    US us={(WCHAR)(n*2),(WCHAR)((n+1)*2),name};
    OA oa={sizeof(OA),root,&us,0x40,0,0};
    struct {IOS ios;DWORD guard;} completion={{0,0},0x76543210};
    int status=ntcreate(&output.file,0x80100000,&oa,&completion.ios,0,0,7,1,0x60,0,0);
    CHECK(output.guard==0x6543210&&completion.guard==0x76543210,"NT-output-handle-and-iosb-width");
    CHECK(status==0&&completion.ios.status==0&&completion.ios.info==1,"NT-open-status-and-information");
    closefile(root);return status<0?0:wow_machine_handle(output.file);
}
void wow64_filesystem_entry(void)
{
    void *k=LoadLibraryA("kernel32.dll");
    openA=GetProcAddress(k,"CreateFileA");openW=GetProcAddress(k,"CreateFileW");
    readfile=GetProcAddress(k,"ReadFile");seekfile=GetProcAddress(k,"SetFilePointer");
    closefile=GetProcAddress(k,"CloseHandle");
    disable=GetProcAddress(k,"Wow64DisableWow64FsRedirection");
    revert=GetProcAddress(k,"Wow64RevertWow64FsRedirection");
    enable=GetProcAddress(k,"Wow64EnableWow64FsRedirection");
    setcwd=GetProcAddress(k,"SetCurrentDirectoryA");getcwd=GetProcAddress(k,"GetCurrentDirectoryA");
    typedef DWORD (CALL *dirfn)(char *,DWORD);
    typedef char *(CALL *cmdfn)(void);
    dirfn windir=GetProcAddress(k,"GetWindowsDirectoryA");
    cmdfn command=GetProcAddress(k,"GetCommandLineA");
    char *line=command();int guest=0,child=0,leave=0,nativecwd=0,rootcwd=0;
    for(unsigned i=0;line[i];i++)if(line[i]==' '){
        if(line[i+1]=='g')guest=1;
        if(line[i+1]=='c')child=1;
        if(line[i+1]=='l')leave=1;
        if(line[i+1]=='n')nativecwd=1;
        if(line[i+1]=='r')rootcwd=1;
    }
    file_name=guest?"osito-system-probe.dll":"kernel32.dll";
    windir(windows,260);wow_path(system_file,"System32",file_name);
    DWORD own=sizeof(void *)==4?0x14c:0x8664;
    if(child){
        DWORD machine=wow_machine(nativecwd?file_name:system_file);
        if(rootcwd){char cwd[260];machine=getcwd(260,cwd)==3?own:0;}
        wow_report("WOW64 CHILD DONE\n");ExitProcess(nativecwd?machine:machine==own?0:1);
    }
    CHECK(wow_machine(system_file)==own,"System32-opens-caller-architecture");
    char path[260],saved[260],current[260];WCHAR wide[260];
    for(unsigned i=0;i<260;i++){wide[i]=(WCHAR)system_file[i];if(!system_file[i])break;}
    CHECK(wow_machine_handle(openW(wide,0x80000000,7,0,3,0,0))==own,"wide-System32-opens-caller-architecture");
    wow_path(path,"SysWOW64",file_name);
    CHECK(wow_machine(path)==0x14c,"explicit-SysWOW64-is-32bit");
    wow_path(path,"Sysnative",file_name);
    CHECK(wow_machine(path)==(sizeof(void *)==4?0x8664:0),"Sysnative-only-for-32bit");
    wow_path(path,"Sysnative","");
    CHECK(wow_root_relative(path)==(sizeof(void *)==4?0x14c:0),"NT-relative-native-root-is-redirected");
    wow_path(path,"System32","");
    CHECK(wow_root_relative(path)==own,"NT-relative-System32-root");
    wow_path(path,"sYsTeM32\\.\\..\\System32",file_name);
    CHECK(wow_machine(path)==own,"case-and-dot-components");
    typedef DWORD (CALL *fullfn)(const char *,DWORD,char *,char **);
    fullfn full=GetProcAddress(k,"GetFullPathNameA");
    full(system_file,260,current,0);
    unsigned equal=1;
    for(unsigned i=0;system_file[i]||current[i];i++)if(system_file[i]!=current[i]){equal=0;break;}
    CHECK(equal,"full-path-remains-lexical-System32");
    getcwd(260,saved);
    CHECK(disable&&revert&&enable,"redirection-APIs-exported");
    if(disable&&revert&&enable){
        struct {void *value;DWORD guard;} cookie={(void *)(UPTR)123,0x76543210};
        void *old,*nested=(void *)(UPTR)456;
        SetLastError(0x13579);int result=disable(&cookie.value);DWORD error=GetLastError();old=cookie.value;
        VALUE("disable-result",result);VALUE("disable-old",old);VALUE("disable-error",error);
        CHECK(sizeof(void *)==4?(result&&error==0x13579):(!result&&error==1&&old==(void *)(UPTR)123),"disable-architecture-and-last-error");
        CHECK(cookie.guard==0x76543210,"disable-output-pointer-width");
        if(result){
            CHECK(wow_machine(system_file)==0x8664,"disabled-System32-is-native");
            wow_path(path,"Sysnative",file_name);CHECK(wow_machine(path)==0x8664,"disabled-Sysnative-still-native");
            wow_path(path,"System32","");CHECK(wow_root_relative(path)==0x8664,"disabled-NT-root-is-native");
            CHECK(disable(&nested),"nested-disable");VALUE("nested-cookie",nested);
            CHECK(revert(nested)&&wow_machine(system_file)==0x8664,"nested-revert-stays-disabled");
            typedef void *(CALL *threadfn)(void *,UPTR,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
            typedef DWORD (CALL *waitfn)(void *,DWORD);
            typedef int (CALL *exitfn)(void *,DWORD *);
            threadfn create=GetProcAddress(k,"CreateThread");waitfn wait=GetProcAddress(k,"WaitForSingleObject");
            exitfn code=GetProcAddress(k,"GetExitCodeThread");
            DWORD exit_code=0;void *thread=create(0,0,wow_thread,0,0,0);
            CHECK(thread&&wait(thread,30000)==0&&code(thread,&exit_code)&&exit_code==0x14c,"new-thread-redirection-enabled");
            if(thread)closefile(thread);
            CHECK(wow_machine(system_file)==0x8664,"child-thread-does-not-change-parent");
            typedef DWORD (CALL *modulefn)(void *,char *,DWORD);
            typedef int (CALL *processfn)(const char *,char *,void *,void *,int,DWORD,void *,const char *,void *,void *);
            modulefn module=GetProcAddress(k,"GetModuleFileNameA");
            processfn process=GetProcAddress(k,"CreateProcessA");
            exitfn process_code=GetProcAddress(k,"GetExitCodeProcess");
            char application[260],child_command[300];unsigned n=0;
            module(0,application,260);child_command[n++]='"';
            for(unsigned i=0;application[i];i++)child_command[n++]=application[i];
            child_command[n++]='"';const char *tail=guest?" child guest":" child";
            while(*tail)child_command[n++]=*tail++;child_command[n]=0;
            unsigned char startup[104]={0};*(DWORD *)startup=sizeof(void *)==4?68:104;
            struct {void *process,*thread;DWORD pid,tid;} pi={0,0,0,0};
            int launched=process(application,child_command,0,0,0,0,0,0,startup,&pi);
            CHECK(launched&&wait(pi.process,120000)==0&&process_code(pi.process,&exit_code)&&exit_code==0,"new-process-does-not-inherit-disabled-state");
            if(launched){closefile(pi.thread);closefile(pi.process);}
            CHECK(revert(old)&&wow_machine(system_file)==own,"outer-revert-restores-default");
            wow_path(path,"System32","");
            CHECK(setcwd(path),"set-current-System32");getcwd(260,current);wow_report("WOW64 VALUE cwd=");wow_report(current);wow_report("\n");
            CHECK(wow_machine(file_name)==0x14c,"relative-CWD-default");
            if(disable(&old)){CHECK(wow_machine(file_name)==0x14c,"relative-CWD-stays-bound-while-disabled");revert(old);}
            setcwd(saved);
            wow_path(path,"Sysnative","");
            CHECK(setcwd(path),"set-current-Sysnative");getcwd(260,current);wow_report("WOW64 VALUE cwd-native=");wow_report(current);wow_report("\n");
            CHECK(wow_machine(file_name)==0x8664,"relative-Sysnative-CWD-stays-native");
            setcwd(saved);
            disable(&old);wow_path(path,"System32","");setcwd(path);revert(old);
            n=0;child_command[n++]='"';
            for(unsigned i=0;application[i];i++)child_command[n++]=application[i];
            child_command[n++]='"';tail=guest?" child native guest":" child native";
            while(*tail)child_command[n++]=*tail++;child_command[n]=0;
            for(unsigned i=0;i<2;i++){
                launched=process(application,child_command,0,0,0,0,0,i?".":0,startup,&pi);
                if(launched&&wait(pi.process,120000)==0&&process_code(pi.process,&exit_code)){
                    CHECK(exit_code==0x14c,i?"child-explicit-dot-CWD-reopens-redirected":"child-inherited-CWD-reopens-redirected");
                }else CHECK(0,"launch-CWD-child");
                if(launched){closefile(pi.thread);closefile(pi.process);}
            }
            n=0;child_command[n++]='"';
            for(unsigned i=0;application[i];i++)child_command[n++]=application[i];
            child_command[n++]='"';tail=guest?" child root guest":" child root";
            while(*tail)child_command[n++]=*tail++;child_command[n]=0;
            launched=process(application,child_command,0,0,0,0,0,"C:\\",startup,&pi);
            CHECK(launched&&wait(pi.process,120000)==0&&process_code(pi.process,&exit_code)&&exit_code==0,"child-explicit-root-CWD");
            if(launched){closefile(pi.thread);closefile(pi.process);}
            setcwd(saved);
            SetLastError(0x13579);unsigned char enabled=enable(0);error=GetLastError();
            VALUE("enable-false-result",enabled);VALUE("enable-false-error",error);
            CHECK(wow_machine(system_file)==0x8664,"Enable-false-disables");
            enabled=enable(1);VALUE("enable-true-result",enabled);
            CHECK(wow_machine(system_file)==own,"Enable-true-enables");
        }
    }
    if(guest){
        typedef int (CALL *copyfn)(const char *,const char *,int);
        typedef int (CALL *movefn)(const char *,const char *);
        typedef int (CALL *deletefn)(const char *);
        typedef int (CALL *mkdirfn)(const char *,void *);
        typedef DWORD (CALL *attrfn)(const char *);
        typedef int (CALL *setattrfn)(const char *,DWORD);
        typedef void *(CALL *findfn)(const char *,void *);
        typedef int (CALL *freefn)(void *);
        typedef DWORD (*valuefn)(void);
        copyfn copy=GetProcAddress(k,"CopyFileA");movefn move=GetProcAddress(k,"MoveFileA");
        deletefn remove=GetProcAddress(k,"DeleteFileA"),rmdir=GetProcAddress(k,"RemoveDirectoryA");
        mkdirfn mkdir=GetProcAddress(k,"CreateDirectoryA");
        attrfn attr=GetProcAddress(k,"GetFileAttributesA");setattrfn setattr=GetProcAddress(k,"SetFileAttributesA");
        findfn find=GetProcAddress(k,"FindFirstFileA");freefn findclose=GetProcAddress(k,"FindClose"),release=GetProcAddress(k,"FreeLibrary");
        const char *source=sizeof(void *)==4?"C:\\sysfixture32.dll":"C:\\sysfixture64.dll";
        const char *backing=sizeof(void *)==4?"SysWOW64":"System32";
        char target[260],physical[260],renamed[260];
        wow_path(target,"System32","wow-redirect-test");
        CHECK(mkdir(target,0),"create-redirected-directory");
        wow_path(physical,backing,"wow-redirect-test");
        CHECK(attr(physical)!=~0U&&(attr(physical)&16),"directory-created-in-physical-target");
        void *dir=openA(target,0x80000000,7,0,3,0x02000000,0);
        CHECK(dir!=(void *)(UPTR)-1,"open-redirected-directory-backup-semantics");if(dir!=(void *)(UPTR)-1)closefile(dir);
        wow_path(target,"System32\\wow-redirect-test","copy.dll");
        CHECK(copy(source,target,1),"copy-to-virtual-System32");
        wow_path(physical,backing,"wow-redirect-test\\copy.dll");
        CHECK(wow_machine(physical)==own,"copy-landed-in-physical-target");
        unsigned char found[592];void *search=find(target,found);
        CHECK(search!=(void *)(UPTR)-1,"find-redirected-file");if(search!=(void *)(UPTR)-1)findclose(search);
        CHECK(setattr(target,1)&&(attr(physical)&1),"attributes-change-physical-target");setattr(target,0x80);
        void *dll=LoadLibraryA(target);valuefn value=dll?GetProcAddress(dll,"system_path_probe_value"):0;
        CHECK(value&&value()==sizeof(void *)*8,"load-explicit-virtual-System32-DLL");if(dll)release(dll);
        wow_path(renamed,"System32\\wow-redirect-test","renamed.dll");
        CHECK(move(target,renamed),"rename-redirected-file");
        CHECK(attr(target)==~0U&&wow_machine(renamed)==own,"rename-source-removed-destination-readable");
        CHECK(remove(renamed)&&attr(renamed)==~0U,"delete-redirected-file");
        wow_path(target,"System32\\wow-redirect-test","write.bin");
        void *file=openA(target,0x40000000,7,0,1,0,0);DWORD written=0;
        CHECK(file!=(void *)(UPTR)-1&&WriteFile(file,"WOW!",4,&written,0)&&written==4,"CreateFile-writes-redirected-file");
        if(file!=(void *)(UPTR)-1)closefile(file);
        wow_path(physical,backing,"wow-redirect-test\\write.bin");
        CHECK(attr(physical)!=~0U,"CreateFile-physical-write-visible");
        typedef int (__attribute__((cdecl)) *statfn)(const char *,void *);
        typedef int (__attribute__((cdecl)) *chmodfn)(const char *,int);
        void *crt=LoadLibraryA("msvcrt.dll");statfn stat=GetProcAddress(crt,"_stat64");
        chmodfn chmod=GetProcAddress(crt,"_chmod");unsigned char stat_buffer[80];
        CHECK(stat&&stat(target,stat_buffer)==0,"CRT-stat-uses-redirected-path");
        CHECK(chmod&&chmod(target,0x100)==0&&(attr(physical)&1),"CRT-chmod-uses-redirected-path");
        if(chmod)chmod(target,0x180);remove(target);
        typedef int (CALL *iniputfn)(const char *,const char *,const char *,const char *);
        typedef DWORD (CALL *inigetfn)(const char *,const char *,const char *,char *,DWORD,const char *);
        iniputfn putini=GetProcAddress(k,"WritePrivateProfileStringA");
        inigetfn getini=GetProcAddress(k,"GetPrivateProfileStringA");
        wow_path(target,"System32\\wow-redirect-test","settings.ini");
        wow_path(physical,backing,"wow-redirect-test\\settings.ini");
        CHECK(putini("path","value","wow",target),"INI-write-redirected-file");
        char answer[32];DWORD answer_length=getini("path","value","missing",answer,32,physical);
        CHECK(answer_length==3&&answer[0]=='w'&&answer[1]=='o'&&answer[2]=='w',"INI-read-physical-file-agrees");remove(target);
        wow_path(target,"System32","wow-redirect-test");
        CHECK(rmdir(target),"remove-redirected-directory");
        wow_path(target,"System32","drivers");mkdir(target,0);
        const char *exceptions[]={"catroot","catroot2","driverstore","drivers\\etc","logfiles","spool"};
        for(unsigned i=0;i<6;i++){
            wow_path(target,"System32",exceptions[i]);mkdir(target,0);
            unsigned n=0;while(target[n])n++;target[n++]='\\';
            const char *tail="osito-exclusion.dll";while(*tail)target[n++]=*tail++;target[n]=0;
            if(sizeof(void *)==8)CHECK(copy(source,target,0),"seed-excluded-native-directory");
            CHECK(wow_machine(target)==0x8664,"excluded-directory-preserves-native-file");
        }
        wow_path(target,"System32extra","");mkdir(target,0);
        wow_path(target,"System32extra","osito-prefix.dll");
        CHECK(copy(source,target,0)&&wow_machine(target)==own,"System32-prefix-requires-component-boundary");remove(target);
        wow_path(target,"lastgood","");mkdir(target,0);
        wow_path(target,"lastgood\\System32","");mkdir(target,0);
        wow_path(target,"lastgood\\System32","osito-lastgood.dll");
        CHECK(copy(source,target,0),"write-lastgood-System32");
        wow_path(physical,sizeof(void *)==4?"lastgood\\SysWOW64":"lastgood\\System32","osito-lastgood.dll");
        CHECK(wow_machine(physical)==own,"lastgood-physical-directory");remove(target);
        if(sizeof(void *)==8){
            wow_path(target,"", "regedit.exe");
            CHECK(copy(source,target,0),"seed-native-regedit");
            wow_path(target,"SysWOW64","regedit.exe");
            CHECK(copy("C:\\sysfixture32.dll",target,0),"seed-32bit-regedit");
        }
        wow_path(target,"","regedit.exe");
        CHECK(wow_machine(target)==own,"Windows-regedit-is-redirected");
        if(sizeof(void *)==4&&disable&&revert){
            void *old=0;
            CHECK(disable(&old),"disable-for-loader");
            dll=LoadLibraryA("osito-system-probe.dll");
            CHECK(!dll,"disabled-default-loader-rejects-native-DLL");if(dll)release(dll);
            revert(old);
            dll=LoadLibraryA("osito-system-probe.dll");value=dll?GetProcAddress(dll,"system_path_probe_value"):0;
            CHECK(value&&value()==32,"loader-recovers-after-restore");if(dll)release(dll);
            wow_path(target,"Sysnative","osito-native-settings.ini");
            CHECK(putini("path","value","native",target),"INI-Sysnative-write");
            void *old_state=0;disable(&old_state);
            wow_path(physical,"System32","osito-native-settings.ini");
            answer_length=getini("path","value","missing",answer,32,physical);
            CHECK(answer_length==6&&answer[0]=='n',"INI-Sysnative-persistence-is-not-redirected-twice");
            remove(physical);revert(old_state);
        }
    }
    if(leave&&sizeof(void *)==4&&disable){void *old_state=0;CHECK(disable(&old_state),"leave-disabled-for-next-launch-reset-test");}
    wow_report("WOW64 DONE checks=");wow_number(checks);wow_report(" failures=");wow_number(failures);wow_report("\n");
    ExitProcess(failures?1:0);
}
