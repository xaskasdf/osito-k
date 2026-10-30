/* Contracts used by the extracted DirectX setup executable. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UP;
typedef __INTPTR_TYPE__ IP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void CALL SetLastError(DWORD);
API DWORD CALL GetLastError(void);
static DWORD failures;
#define SAY(s) do{DWORD w;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&w,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("DXCHILD PASS " name "\n");else{SAY("DXCHILD FAIL " name "\n");failures++;}}while(0)
#define GET(type,name,module) type name=(type)GetProcAddress(module,#name)
static void child_number(DWORD n)
{
    char b[10];DWORD len=0,w;
    do{b[len++]=(char)('0'+n%10);n/=10;}while(n);
    while(len)WriteFile(GetStdHandle((DWORD)-11),b+--len,1,&w,0);
}
static void child_report(const WORD *s)
{
    DWORD w;
    while(*s){char c=(char)*s++;WriteFile(GetStdHandle((DWORD)-11),&c,1,&w,0);}
}
void installer_child_entry(void)
{
    void *k=LoadLibraryA("kernel32.dll"),*u=LoadLibraryA("user32.dll"),*s=LoadLibraryA("shlwapi.dll");
    const char *kn[]={"GetSystemDefaultUILanguage","MoveFileExA"};
    const char *un[]={"LoadStringW","SetDlgItemTextW","IsDlgButtonChecked"};
    for(unsigned i=0;i<2;i++)CHECK(GetProcAddress(k,kn[i]),"kernel-export");
    for(unsigned i=0;i<3;i++)CHECK(GetProcAddress(u,un[i]),"user-export");
    CHECK(GetProcAddress(s,"PathRemoveFileSpecW"),"path-export");
    if(failures)ExitProcess(1);
    typedef WORD (CALL *langfn)(void);
    GET(langfn,GetSystemDefaultUILanguage,k);
    CHECK(GetSystemDefaultUILanguage()!=0,"system-ui-language");
    typedef int (CALL *stringfn)(void *,DWORD,WORD *,int);
    GET(stringfn,LoadStringW,u);
    WORD text[64];
    for(unsigned i=0;i<64;i++)text[i]='#';
    int n=LoadStringW(0,33,text,64);
    const WORD *expected=(const WORD *)L"DirectX test";
    int equal=n==12;
    for(unsigned i=0;i<13;i++)if(text[i]!=expected[i])equal=0;
    CHECK(equal&&text[13]=='#',"wide-string-resource");
    text[5]='#';
    CHECK(LoadStringW(0,33,text,5)==4&&text[0]=='D'&&text[3]=='e'&&text[4]==0&&text[5]=='#',"wide-string-truncation");
    text[0]='#';text[1]='#';
    CHECK(LoadStringW(0,33,text,1)==0&&text[0]==0&&text[1]=='#',"wide-string-one-char-buffer");
    struct {UP before;const WORD *pointer;UP after;} borrowed={(UP)0x11223344,0,(UP)0x55667788};
    n=LoadStringW(0,33,(WORD *)&borrowed.pointer,0);
    equal=n==12&&borrowed.pointer;
    if(equal)for(unsigned i=0;i<12;i++)if(borrowed.pointer[i]!=expected[i])equal=0;
    CHECK(equal&&borrowed.before==0x11223344&&borrowed.after==0x55667788,"wide-string-borrowed-pointer-width");
    text[0]='#';
    CHECK(LoadStringW(0,9000,text,64)==0&&text[0]==0,"missing-wide-string");
    borrowed.pointer=(const WORD *)(UP)0x12345678;
    n=LoadStringW(0,9000,(WORD *)&borrowed.pointer,0);
    SAY("DXCHILD MISSING POINTER ");child_number((DWORD)(UP)borrowed.pointer);SAY("\n");
    CHECK(n==0&&(UP)borrowed.pointer==0x12345678&&borrowed.before==0x11223344&&borrowed.after==0x55667788,"missing-string-pointer-guards");
    typedef int (CALL *pathfn)(WORD *);
    GET(pathfn,PathRemoveFileSpecW,s);
    static const struct {const WORD *from;const WORD *to;int result;} paths[]={
        {(const WORD *)L"",(const WORD *)L"",0},
        {(const WORD *)L"file.dll",(const WORD *)L"",1},
        {(const WORD *)L"C:",(const WORD *)L"C:",0},
        {(const WORD *)L"C:\\",(const WORD *)L"C:\\",0},
        {(const WORD *)L"C:\\file.dll",(const WORD *)L"C:\\",1},
        {(const WORD *)L"C:\\Temp\\file.dll",(const WORD *)L"C:\\Temp",1},
        {(const WORD *)L"C:\\Temp\\",(const WORD *)L"C:\\Temp",1},
        {(const WORD *)L"C:relative",(const WORD *)L"C:",1},
        {(const WORD *)L"\\",(const WORD *)L"\\",0},
        {(const WORD *)L"\\file",(const WORD *)L"\\",1},
        {(const WORD *)L"\\\\server\\share\\file",(const WORD *)L"\\\\server\\share",1},
        {(const WORD *)L"\\\\server\\share",(const WORD *)L"\\\\server",1},
        {(const WORD *)L"\\\\server",(const WORD *)L"\\\\",1},
        {(const WORD *)L"\\\\",(const WORD *)L"\\\\",0},
        {(const WORD *)L"folder\\file",(const WORD *)L"folder",1},
        {(const WORD *)L"C:/Temp/file",(const WORD *)L"C:",1},
        {(const WORD *)L"folder/file",(const WORD *)L"",1},
        {(const WORD *)L"C:\\Temp\\\\",(const WORD *)L"C:\\Temp",1}
    };
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
        unsigned j=0;do{text[j]=paths[i].from[j];}while(paths[i].from[j++]);
        int r=PathRemoveFileSpecW(text);
        SAY("DXCHILD PATH ");child_number(i);SAY(" ");child_number((DWORD)r);SAY(" [");child_report(text);SAY("]\n");
        equal=(!!r)==paths[i].result;
        for(j=0;text[j]&&paths[i].to[j]&&text[j]==paths[i].to[j];j++);
        CHECK(equal&&text[j]==paths[i].to[j],"remove-file-spec");
    }
    typedef void *(CALL *createfn)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
    typedef int (CALL *closefn)(void *);
    typedef int (CALL *deletefn)(const char *);
    typedef int (CALL *movefn)(const char *,const char *,DWORD);
    typedef DWORD (CALL *attrfn)(const char *);
    typedef int (CALL *readfn)(void *,void *,DWORD,DWORD *,void *);
    GET(createfn,CreateFileA,k);GET(closefn,CloseHandle,k);GET(deletefn,DeleteFileA,k);
    GET(movefn,MoveFileExA,k);GET(attrfn,GetFileAttributesA,k);GET(readfn,ReadFile,k);
    const char *from="dxchild_src.tmp",*to="dxchild_dst.tmp",*newname="dxchild_new.tmp";
    DeleteFileA(from);DeleteFileA(to);DeleteFileA(newname);
    void *a=CreateFileA(from,0x40000000,3,0,1,0x80,0),*b=CreateFileA(to,0x40000000,3,0,1,0x80,0);DWORD count;
    CHECK(a!=(void *)(UP)-1&&b!=(void *)(UP)-1&&WriteFile(a,"payload",7,&count,0)&&count==7,"move-create-files");
    if(a!=(void *)(UP)-1)CloseHandle(a);
    if(b!=(void *)(UP)-1)CloseHandle(b);
    SetLastError(0);
    CHECK(!MoveFileExA(from,to,0)&&GetLastError()==183&&GetFileAttributesA(from)!=(DWORD)-1,"move-existing-without-replace");
    CHECK(MoveFileExA(from,to,1|2|8)&&GetFileAttributesA(from)==(DWORD)-1,"move-replace-write-through");
    a=CreateFileA(to,0x80000000,3,0,3,0x80,0);char content[8]={0};count=0;
    CHECK(a!=(void *)(UP)-1&&ReadFile(a,content,7,&count,0)&&count==7&&content[0]=='p'&&content[6]=='d',"move-preserves-content");
    if(a!=(void *)(UP)-1)CloseHandle(a);
    CHECK(MoveFileExA(to,newname,0)&&GetFileAttributesA(to)==(DWORD)-1,"move-new-destination");
    CHECK(DeleteFileA(newname),"move-cleanup");
    SetLastError(0);CHECK(!MoveFileExA(from,to,0)&&GetLastError()==2,"move-missing-source");
    typedef void *(CALL *desktopfn)(const WORD *,const WORD *,void *,DWORD,DWORD,void *);
    typedef int (CALL *setdesktopfn)(void *);
    desktopfn desktop=GetProcAddress(u,"CreateDesktopW");
    setdesktopfn setdesktop=GetProcAddress(u,"SetThreadDesktop");
    if(desktop&&setdesktop){void *d=desktop((const WORD *)L"OsitoInstallerChildTest",0,0,0,0xF01FF,0);CHECK(d&&setdesktop(d),"private-desktop");}
    typedef void *(CALL *windowfn)(DWORD,const char *,const char *,DWORD,int,int,int,int,void *,void *,void *,void *);
    typedef int (CALL *destroyfn)(void *);
    typedef int (CALL *settextfn)(void *,int,const WORD *);
    typedef DWORD (CALL *gettextfn)(void *,WORD *,int);
    typedef DWORD (CALL *checkfn)(void *,int);
    typedef IP (CALL *messagefn)(void *,DWORD,UP,IP);
    GET(windowfn,CreateWindowExA,u);GET(destroyfn,DestroyWindow,u);
    GET(settextfn,SetDlgItemTextW,u);GET(gettextfn,GetWindowTextW,u);
    GET(checkfn,IsDlgButtonChecked,u);GET(messagefn,SendMessageA,u);
    void *parent=CreateWindowExA(0,"STATIC","installer",0x80000000U,0,0,100,100,0,0,0,0);
    void *child=CreateWindowExA(0,"BUTTON","agree",0x40000005U,0,0,40,20,parent,(void *)23,0,0);
    CHECK(parent&&child,"create-child-controls");
    CHECK(SetDlgItemTextW(parent,23,(const WORD *)L"DirectX setup")&&GetWindowTextW(child,text,64)==13&&text[0]=='D'&&text[12]=='p',"set-wide-control-text");
    CHECK(IsDlgButtonChecked(parent,23)==0,"unchecked-control");
    SendMessageA(child,0xF1,1,0);CHECK(IsDlgButtonChecked(parent,23)==1,"checked-control");
    SendMessageA(child,0xF1,2,0);CHECK(IsDlgButtonChecked(parent,23)==2,"indeterminate-control");
    SetLastError(0);CHECK(!SetDlgItemTextW(parent,99,(const WORD *)L"missing")&&GetLastError()==1421,"missing-text-control");
    CHECK(IsDlgButtonChecked(parent,99)==0,"missing-check-control");
    CHECK(DestroyWindow(parent),"destroy-child-controls");
    if(!failures)SAY("DXCHILD DONE\n");
    ExitProcess(failures?1:0);
}
