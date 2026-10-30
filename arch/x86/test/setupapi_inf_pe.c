/* SetupAPI INF file operations. All destinations stay in this probe's cwd. */
typedef unsigned int DWORD;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
typedef void *(CALL *OPEN)(const char *,const char *,DWORD,DWORD *);
typedef int(CALL *INSTALL)(void *,void *,const char *,DWORD,void *,const char *,DWORD,void *,void *,void *,void *);
static void *(CALL *create)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
static int(CALL *close)(void *), (CALL *readfile)(void *,void *,DWORD,DWORD *,void *);
static int(CALL *mkdir)(const char *,void *), (CALL *deletefile)(const char *);
static DWORD(CALL *error)(void), (CALL *attributes)(const char *);
static void(CALL *seterror)(DWORD);
static OPEN openinf;
static INSTALL install;
static void(CALL *closeinf)(void *);
static DWORD failures,mode,notifications[32];
static char fullpath[1024],sourcepath[1024],inftext[8192];
#define SAY(s) do{DWORD w;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&w,0);}while(0)
#define HEX(v) do{char h[9];DWORD n=(DWORD)(v),w;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[n&15];n>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&w,0);}while(0)
static void inf_probe_check(int ok,const char *name)
{
    if(ok)SAY("INF PASS ");else{SAY("INF FAIL ");failures++;}
    DWORD n=0,w;while(name[n])n++;WriteFile(GetStdHandle((DWORD)-11),name,n,&w,0);SAY("\n");
}
static void inf_probe_append(char *target,const char *text)
{
    DWORD n=0;while(target[n])n++;while(*text)target[n++]=*text++;target[n]=0;
}
static int inf_probe_write(const char *path,const char *text)
{
    void *f=create(path,0x40000000,7,0,2,0x80,0);
    if(f==(void *)(UP)-1)return 0;
    DWORD n=0,w=0;while(text[n])n++;
    int result=WriteFile(f,text,n,&w,0)&&w==n;close(f);return result;
}
static int inf_probe_contents(const char *path,const char *expected)
{
    void *f=create(path,0x80000000,7,0,3,0x80,0);
    if(f==(void *)(UP)-1)return 0;
    char buf[100];DWORD n=0,want=0;while(expected[want])want++;
    int ok=readfile(f,buf,sizeof(buf),&n,0)&&n==want;
    if(ok)for(DWORD i=0;i<n;i++)if(buf[i]!=expected[i])ok=0;
    close(f);return ok;
}
static DWORD CALL inf_probe_callback(void *ctx,DWORD notification,UP p1,UP p2)
{
    (void)p2;
    if(ctx!=(void *)(UP)0x12345678){failures++;return 0;}
    if(notification<32)notifications[notification]++;
    SAY("INF NOTIFY ");HEX(notification);
    if(notification==5||notification==6||notification==8||notification==9||notification==11||notification==12||notification==13){
        const struct {const char *target,*source;DWORD error,flags;} *paths=(const void *)p1;
        HEX(paths->error);HEX(paths->flags);
        if(paths->target){DWORD n=0,w;while(paths->target[n])n++;WriteFile(GetStdHandle((DWORD)-11),paths->target,n,&w,0);}
    }else{HEX(p1);HEX(p2);}SAY("\n");
    if(notification==1||notification==3||notification==14)return 1;
    if(notification==11)return mode==1?0:mode==2?2:1;
    if(notification==5||notification==8)return 1;
    return 0;
}
static int inf_probe_install(void *inf,const char *section,DWORD flags,DWORD copyflags)
{
    for(unsigned i=0;i<32;i++)notifications[i]=0;
    seterror(0x13579);
    int result=install(0,inf,section,flags,0,sourcepath,copyflags,(void *)inf_probe_callback,(void *)(UP)0x12345678,0,0);
    DWORD last=error();SAY("INF RESULT ");HEX(result);HEX(last);SAY("\n");return result;
}
void inf_probe_entry(void)
{
    void *s=LoadLibraryA("setupapi.dll"),*k=LoadLibraryA("kernel32.dll");
#define LOAD(dst,module,name) dst=(__typeof__(dst))GetProcAddress(module,name)
    LOAD(create,k,"CreateFileA");LOAD(readfile,k,"ReadFile");LOAD(close,k,"CloseHandle");
    LOAD(mkdir,k,"CreateDirectoryA");LOAD(deletefile,k,"DeleteFileA");LOAD(attributes,k,"GetFileAttributesA");
    LOAD(error,k,"GetLastError");LOAD(seterror,k,"SetLastError");
    LOAD(openinf,s,"SetupOpenInfFileA");LOAD(closeinf,s,"SetupCloseInfFile");LOAD(install,s,"SetupInstallFromInfSectionA");
    void *(CALL *openqueue)(void);int(CALL *closequeue)(void *);
    DWORD(CALL *full)(const char *,DWORD,char *,char **);
    LOAD(openqueue,s,"SetupOpenFileQueue");LOAD(closequeue,s,"SetupCloseFileQueue");LOAD(full,k,"GetFullPathNameA");
    inf_probe_check(create&&readfile&&close&&mkdir&&deletefile&&attributes&&error&&seterror&&openinf&&closeinf&&install&&openqueue&&closequeue&&full,"exports");
    if(failures)ExitProcess(1);
    mkdir("inf-fixture",0);mkdir("inf-fixture\\source",0);mkdir("inf-fixture\\source\\media",0);mkdir("inf-fixture\\source\\media\\sub",0);
    full("inf-fixture\\destination",sizeof(fullpath),fullpath,0);
    full("inf-fixture\\source",sizeof(sourcepath),sourcepath,0);
    inf_probe_check(inf_probe_write("inf-fixture\\source\\media\\sub\\source.txt","original-data"),"source-fixture");
    inf_probe_append(inftext,"[Version]\r\nSignature=\"$CHICAGO$\"\r\nClass=OsitoProbe\r\n[SourceDisksNames]\r\n1=\"Fixture\",,,media\r\n[SourceDisksFiles]\r\nsource.txt=1,sub\r\n[DestinationDirs]\r\nFiles=-1,\"");
    inf_probe_append(inftext,fullpath);
    inf_probe_append(inftext,"\"\r\nDefaultDestDir=-1,\"");inf_probe_append(inftext,fullpath);
    inf_probe_append(inftext,"\"\r\nSystem11=11,directx\\websetup\r\nWow64=16425,directx\\websetup\r\n[Install]\r\nCopyFiles=Files\r\n[Files]\r\n%Name%,source.txt,,32 ; comment\r\n[Clean]\r\nDelFiles=Files\r\n[Rename]\r\nRenFiles=Renames\r\n[Renames]\r\nrenamed.txt,target.txt\r\n[CleanRename]\r\nDelFiles=Removed\r\n[Removed]\r\nrenamed.txt\r\n[NoFiles]\r\n[Strings]\r\nName=\"target.txt\"\r\n");
    inf_probe_check(inf_probe_write("inf-fixture\\test.inf",inftext),"inf-fixture");
    DWORD line=0xcccccccc;seterror(0x13579);void *inf=openinf(".\\inf-fixture\\test.inf",0,2,&line);
    SAY("INF OPEN ");HEX((UP)inf);HEX(error());HEX(line);SAY("\n");
    inf_probe_check(inf!=(void *)(UP)-1,"open-valid-inf");if(inf==(void *)(UP)-1)ExitProcess(1);
    /* Read-only native directory oracle; no files are copied to these paths. */
    int(CALL *targetpath)(void *,void *,const char *,char *,DWORD,DWORD *);
    LOAD(targetpath,s,"SetupGetTargetPathA");
    if(targetpath){
        const char *sections[2]={"System11","Wow64"};
        for(unsigned i=0;i<2;i++){
            DWORD need=0;int ok=targetpath(inf,0,sections[i],fullpath,sizeof(fullpath),&need);
            SAY("INF SYSTEM DIRECTORY ");HEX(i);HEX(ok);HEX(error());HEX(need);
            if(ok){DWORD n=0,w;while(fullpath[n])n++;WriteFile(GetStdHandle((DWORD)-11),fullpath,n,&w,0);}SAY("\n");
        }
    }
    deletefile("inf-fixture\\destination\\target.txt");deletefile("inf-fixture\\destination\\renamed.txt");
    inf_probe_check(inf_probe_install(inf,"Install",16,0),"copy-section");
    inf_probe_check(inf_probe_contents("inf-fixture\\destination\\target.txt","original-data"),"copy-bytes-and-source-subdirectories");
    inf_probe_check(notifications[1]==1&&notifications[2]==1&&notifications[11]==1&&notifications[12]==1,"copy-notifications");
    inf_probe_check(inf_probe_write("inf-fixture\\destination\\target.txt","keep"),"overwrite-fixture");
    inf_probe_check(inf_probe_install(inf,"Install",16,0x1000)&&inf_probe_contents("inf-fixture\\destination\\target.txt","keep"),"force-no-overwrite");
    inf_probe_check(inf_probe_install(inf,"Install",16,0)&&inf_probe_contents("inf-fixture\\destination\\target.txt","original-data"),"overwrite-unversioned-target");
    inf_probe_check(inf_probe_install(inf,"Rename",16,0),"rename-section");
    inf_probe_check(inf_probe_contents("inf-fixture\\destination\\renamed.txt","original-data")&&attributes("inf-fixture\\destination\\target.txt")==0xffffffff,"rename-bytes");
    inf_probe_check(inf_probe_install(inf,"CleanRename",16,0)&&attributes("inf-fixture\\destination\\renamed.txt")==0xffffffff,"delete-section");
    mode=2;inf_probe_check(inf_probe_install(inf,"Install",16,0)&&attributes("inf-fixture\\destination\\target.txt")==0xffffffff,"callback-skip");
    mode=1;int result=inf_probe_install(inf,"Install",16,0);DWORD last=error();
    SAY("INF ABORT ");HEX(result);HEX(last);SAY("\n");
    inf_probe_check(!result&&attributes("inf-fixture\\destination\\target.txt")==0xffffffff,"callback-abort");mode=0;
    inf_probe_check(inf_probe_install(inf,"Install",0,0)&&attributes("inf-fixture\\destination\\target.txt")==0xffffffff,"files-flag-required");
    inf_probe_check(inf_probe_install(inf,"NoFiles",16,0),"empty-section");
    result=inf_probe_install(inf,"AbsentSection",16,0);SAY("INF ABSENT SECTION ");HEX(result);HEX(error());SAY("\n");
    inf_probe_check(inf_probe_install(inf,"Install",16,0)&&inf_probe_install(inf,"Clean",16,0)&&attributes("inf-fixture\\destination\\target.txt")==0xffffffff,"copy-delete-list");
    closeinf(inf);
    line=0xcccccccc;inf=openinf(".\\inf-fixture\\test.inf","OtherClass",2,&line);
    inf_probe_check(inf==(void *)(UP)-1,"class-mismatch");SAY("INF CLASS ");HEX(error());HEX(line);SAY("\n");if(inf!=(void *)(UP)-1)closeinf(inf);
    line=0xcccccccc;inf=openinf(".\\inf-fixture\\absent.inf",0,2,&line);
    inf_probe_check(inf==(void *)(UP)-1,"missing-inf");SAY("INF MISSING ");HEX(error());HEX(line);SAY("\n");
    inf_probe_write("inf-fixture\\bad.inf","[Version]\nSignature=wrong\n");
    line=0xcccccccc;inf=openinf(".\\inf-fixture\\bad.inf",0,2,&line);
    inf_probe_check(inf==(void *)(UP)-1,"bad-signature");SAY("INF SIGNATURE ");HEX(error());HEX(line);SAY("\n");
    void *q1=openqueue(),*q2=openqueue();
    inf_probe_check(q1!=(void *)(UP)-1&&q2!=(void *)(UP)-1&&q1&&q2&&q1!=q2,"distinct-file-queues");
    inf_probe_check(closequeue(q1)&&closequeue(q2),"close-file-queues");
    inf_probe_check(!closequeue((void *)(UP)-1),"invalid-queue");
    if(!failures)SAY("INF DONE\n");ExitProcess(failures?1:0);
}
