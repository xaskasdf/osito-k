/* Directory contracts and installed-library lookup; no DirectX redistributables. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void CALL SetLastError(DWORD);
API DWORD CALL GetLastError(void);
static unsigned failures;
static void path_report(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void path_number(DWORD n)
{ char b[11];unsigned i=10;b[i]=0;do{b[--i]=(char)('0'+n%10);n/=10;}while(n);path_report(b+i); }
#define CHECK(ok,label) do {int pass=!!(ok);path_report(pass?"SYSPATH PASS ":"SYSPATH FAIL ");path_report(label);path_report("\n");if(!pass)failures++;}while(0)
void system_paths_entry(void)
{
    void *k=LoadLibraryA("kernel32.dll");
    const char *names[]={"GetSystemDirectoryA","GetWindowsDirectoryA","GetSystemWow64DirectoryA",
                         "GetSystemDirectoryW","GetWindowsDirectoryW","GetSystemWow64DirectoryW"};
    typedef DWORD (CALL *dirA)(char *,DWORD);
    typedef DWORD (CALL *dirW)(WCHAR *,DWORD);
    static char buffer[264];static WCHAR wide[264];
    for(unsigned i=0;i<6;i++) {
        void *proc=GetProcAddress(k,names[i]);
        if(!proc){path_report("SYSPATH MISSING ");path_report(names[i]);path_report("\n");failures++;continue;}
        for(unsigned j=0;j<264;j++){buffer[j]='#';wide[j]='#';}
        SetLastError(0x13579);
        DWORD size=i<3?((dirA)proc)(buffer,260):((dirW)proc)(wide,260);
        DWORD error=GetLastError();
        if(i>=3)for(unsigned j=0;j<260;j++)buffer[j]=(char)wide[j];
        path_report("SYSPATH VALUE ");path_report(names[i]);path_report(" ");
        if(size<260)path_report(buffer);
        path_report(" length=");path_number(size);path_report(" error=");path_number(error);path_report("\n");
        CHECK(size>3&&size<260&&buffer[1]==':'&&buffer[size]==0&&buffer[size-1]!='\\',names[i]);
        DWORD query=i<3?((dirA)proc)(0,0):((dirW)proc)(0,0);
        CHECK(query==size+1,"size-query-includes-null");
        buffer[0]='#';wide[0]='#';
        DWORD tiny=i<3?((dirA)proc)(buffer,1):((dirW)proc)(wide,1);
        CHECK(tiny==size+1&&(i<3?buffer[0]=='#':wide[0]=='#'),"small-buffer-unchanged");
    }
    typedef int (CALL *folderfn)(void *,int,void *,DWORD,char *);
    folderfn folder=GetProcAddress(LoadLibraryA("shell32.dll"),"SHGetFolderPathA");
    const int csidls[]={0x24,0x25,0x29};
    for(unsigned i=0;i<3;i++) {
        char folder_path[260];
        int result=folder?folder(0,csidls[i],0,0,folder_path):-1;
        path_report("SYSPATH FOLDER ");path_number((DWORD)csidls[i]);path_report(" ");
        if(!result)path_report(folder_path);path_report("\n");
        dirA directory=GetProcAddress(k,names[i==0?1:i==1?0:2]);
        DWORD size=directory?directory(buffer,260):0;
        int same=result==0&&size>0;
        for(DWORD j=0;same&&j<=size;j++){
            unsigned char a=(unsigned char)folder_path[j],b=(unsigned char)buffer[j];
            if(a>='A'&&a<='Z')a+='a'-'A';
            if(b>='A'&&b<='Z')b+='a'-'A';
            if(a!=b)same=0;
        }
        CHECK(same,"shell-and-kernel-directory-agree");
    }
    typedef char *(CALL *cmdfn)(void);
    cmdfn cmd=GetProcAddress(k,"GetCommandLineA");
    char *line=cmd?cmd():0;int guest=0;
    for(unsigned i=0;line&&line[i];i++)if(line[i]==' '&&line[i+1]=='g')guest=1;
    if(guest){
        typedef DWORD (CALL *attrfn)(const char *);
        attrfn attr=GetProcAddress(k,"GetFileAttributesA");
        const char *dirs[]={"C:\\Windows","C:\\Windows\\System32","C:\\Windows\\SysWOW64","C:\\Temp"};
        for(unsigned i=0;i<4;i++){DWORD a=attr?attr(dirs[i]):~0U;CHECK(a!=~0U&&(a&16),dirs[i]);}
        typedef int (CALL *copyfn)(const char *,const char *,int);
        copyfn copy=GetProcAddress(k,"CopyFileA");
        const char *source=sizeof(void *)==4?"C:\\sysfixture32.dll":"C:\\sysfixture64.dll";
        const char *destination=sizeof(void *)==4?"C:\\Windows\\SysWOW64\\osito-system-probe.dll":"C:\\Windows\\System32\\osito-system-probe.dll";
        CHECK(copy&&copy(source,destination,0),"install-library-in-system-directory");
        typedef void *(CALL *loadexfn)(const char *,void *,DWORD);
        typedef int (CALL *freefn)(void *);
        typedef DWORD (*valuefn)(void);
        loadexfn loadex=GetProcAddress(k,"LoadLibraryExA");
        freefn release=GetProcAddress(k,"FreeLibrary");
        const DWORD flags[]={0,0x800,0x1000};
        for(unsigned i=0;i<3;i++){
            void *m=loadex?loadex("osito-system-probe.dll",0,flags[i]):0;
            valuefn value=m?GetProcAddress(m,"system_path_probe_value"):0;
            CHECK(value&&value()==sizeof(void *)*8,"installed-library-matches-caller-architecture");
            if(m&&release)release(m);
        }
        CHECK(copy&&copy(source,"C:\\cwd-only-probe.dll",0),"prepare-application-only-library");
        void *restricted=loadex?loadex("cwd-only-probe.dll",0,0x800):0;
        CHECK(!restricted,"system-only-search-excludes-application-folder");
        if(restricted&&release)release(restricted);
        restricted=loadex?loadex("C:\\missing\\cwd-only-probe.dll",0,0):0;
        CHECK(!restricted,"explicit-missing-path-does-not-fall-back-to-basename");
        if(restricted&&release)release(restricted);
        void *application=LoadLibraryA("cwd-only-probe.dll");
        valuefn app_value=application?GetProcAddress(application,"system_path_probe_value"):0;
        CHECK(app_value&&app_value()==sizeof(void *)*8,"normal-search-keeps-application-library");
        if(application&&release)release(application);
    }
    path_report("SYSPATH DONE failures=");path_number(failures);path_report("\n");
    ExitProcess(failures?1:0);
}
