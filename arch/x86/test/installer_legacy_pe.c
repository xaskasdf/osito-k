/* Real file, resource and child-control contracts used by legacy installers. */
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
static DWORD failures,language_count,language_mask,stop_languages;
#define SAY(s) do{DWORD w;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&w,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("DXLEGACY PASS " name "\n");else{SAY("DXLEGACY FAIL " name "\n");failures++;}}while(0)
#define GET(type,name,module) type name=(type)GetProcAddress(module,#name)
static int CALL installer_language_callback(void *module,const char *type,const char *name,WORD language,IP context)
{
    (void)module;
    if((UP)type!=10||(UP)name!=77||(UP)context!=0x2468)failures++;
    language_count++;
    if(language==1033)language_mask|=1;
    if(language==3082)language_mask|=2;
    return !stop_languages;
}
void installer_legacy_entry(void)
{
    void *k=LoadLibraryA("kernel32.dll"),*u=LoadLibraryA("user32.dll");
    const char *kn[]={"lstrcmpA","lstrcatA","lstrcpynA","_lopen","_llseek","_lclose",
        "GetShortPathNameA","DosDateTimeToFileTime","EnumResourceLanguagesA"};
    const char *un[]={"CharUpperA","CharPrevA","SendDlgItemMessageA","GetDlgItemTextA","LoadStringA"};
    for(unsigned i=0;i<9;i++)CHECK(GetProcAddress(k,kn[i]),"kernel-export");
    for(unsigned i=0;i<5;i++)CHECK(GetProcAddress(u,un[i]),"user-export");
    if(failures)ExitProcess(1);
    typedef int (CALL *comparefn)(const char *,const char *);
    typedef char *(CALL *catfn)(char *,const char *);
    typedef char *(CALL *copyfn)(char *,const char *,int);
    GET(comparefn,lstrcmpA,k);GET(catfn,lstrcatA,k);GET(copyfn,lstrcpynA,k);
    char text[32]="dx";
    CHECK(lstrcmpA("setup","setup")==0&&lstrcmpA("setup-a","setup-b")<0&&lstrcmpA("setup-b","setup-a")>0,"string-comparison");
    CHECK(lstrcatA(text,"web")==text&&lstrcmpA(text,"dxweb")==0,"string-concatenation");
    text[4]='#';
    CHECK(lstrcpynA(text,"abcdef",4)==text&&lstrcmpA(text,"abc")==0&&text[4]=='#',"bounded-string-copy");
    text[0]='#';CHECK(lstrcpynA(text,"abc",0)==text&&text[0]=='#',"zero-capacity-string-copy");
    typedef char *(CALL *upperfn)(char *);
    typedef char *(CALL *prevfn)(const char *,const char *);
    GET(upperfn,CharUpperA,u);GET(prevfn,CharPrevA,u);
    char upper[]={'a','z',(char)0xe9,0};
    CHECK(CharUpperA(upper)==upper&&upper[0]=='A'&&upper[1]=='Z'&&(unsigned char)upper[2]==0xc9,"ansi-uppercase");
    CHECK((UP)CharUpperA((char *)(UP)'z')=='Z',"uppercase-character-operand");
    CHECK(CharPrevA(upper,upper)==upper&&CharPrevA(upper,upper+2)==upper+1,"previous-character");
    typedef int (CALL *dosfn)(WORD,WORD,void *);
    GET(dosfn,DosDateTimeToFileTime,k);
    unsigned long long ticks=0;
    CHECK(DosDateTimeToFileTime(33,0,&ticks)&&ticks==119600064000000000ULL,"dos-epoch");
    ticks=0x12345678;SetLastError(0);
    CHECK(!DosDateTimeToFileTime(0,0,&ticks)&&ticks==0x12345678&&GetLastError()==87,"invalid-dos-date");
    typedef void *(CALL *createfn)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
    typedef int (CALL *closefn)(void *);
    typedef int (CALL *deletefn)(const char *);
    typedef int (CALL *openfn)(const char *,int);
    typedef int (CALL *seekfn)(int,int,int);
    typedef int (CALL *lclosefn)(int);
    typedef DWORD (CALL *shortfn)(const char *,char *,DWORD);
    GET(createfn,CreateFileA,k);GET(closefn,CloseHandle,k);GET(deletefn,DeleteFileA,k);
    GET(openfn,_lopen,k);GET(seekfn,_llseek,k);GET(lclosefn,_lclose,k);GET(shortfn,GetShortPathNameA,k);
    const char *file="dxw_test.dat";
    void *h=CreateFileA(file,0x40000000,3,0,2,0x80,0);DWORD wrote=0;
    CHECK(h!=(void *)(UP)-1&&WriteFile(h,"abcdef",6,&wrote,0)&&wrote==6,"create-legacy-file");
    if(h!=(void *)(UP)-1)CloseHandle(h);
    int old=_lopen(file,0x40);
    CHECK(old!=-1,"legacy-open");
    if(old!=-1){CHECK(_llseek(old,0,2)==6&&_llseek(old,-2,1)==4&&_llseek(old,1,0)==1,"legacy-seek");CHECK(_lclose(old)==0,"legacy-close");}
    SetLastError(0);CHECK(_lclose(0)==-1&&GetLastError()==6,"legacy-invalid-handle");
    SetLastError(0x13579);CHECK(_lclose(-1)==0&&GetLastError()==0x13579,"legacy-pseudo-handle");
    char short_path[260];DWORD n=GetShortPathNameA(file,short_path,260);
    CHECK(n&&n<260&&short_path[n]==0,"short-path-existing-file");
    CHECK(GetShortPathNameA(file,0,0)==n+1,"short-path-size-query");
    short_path[0]='#';CHECK(GetShortPathNameA(file,short_path,1)==n+1&&short_path[0]=='#',"short-path-small-buffer");
    CHECK(DeleteFileA(file),"remove-legacy-file");
    SetLastError(0);CHECK(GetShortPathNameA(file,short_path,260)==0&&GetLastError()==2,"short-path-missing-file");
    typedef int (CALL *enumfn)(void *,const char *,const char *,void *,IP);
    typedef int (CALL *stringfn)(void *,DWORD,char *,int);
    GET(enumfn,EnumResourceLanguagesA,k);GET(stringfn,LoadStringA,u);
    CHECK(EnumResourceLanguagesA(0,(const char *)10,(const char *)77,installer_language_callback,0x2468)&&language_count==2&&language_mask==3,"enumerate-resource-languages");
    stop_languages=1;language_count=0;SetLastError(0);
    CHECK(!EnumResourceLanguagesA(0,(const char *)10,(const char *)77,installer_language_callback,0x2468)&&language_count==1&&GetLastError()==15106,"stop-resource-enumeration");
    CHECK(LoadStringA(0,33,text,32)==12&&lstrcmpA(text,"DirectX test")==0,"load-string-resource");
    text[5]='#';CHECK(LoadStringA(0,33,text,5)==4&&lstrcmpA(text,"Dire")==0&&text[5]=='#',"truncate-string-resource");
    text[0]='#';CHECK(LoadStringA(0,9000,text,32)==0&&text[0]==0,"missing-string-resource");
    typedef void *(CALL *desktopfn)(const WORD *,const WORD *,void *,DWORD,DWORD,void *);
    typedef int (CALL *setdesktopfn)(void *);
    desktopfn desktop=GetProcAddress(u,"CreateDesktopW");
    setdesktopfn setdesktop=GetProcAddress(u,"SetThreadDesktop");
    static const WORD desktop_name[]={'O','s','i','t','o','I','n','s','t','a','l','l','e','r','T','e','s','t',0};
    if(desktop&&setdesktop){void *d=desktop(desktop_name,0,0,0,0xF01FF,0);CHECK(d&&setdesktop(d),"private-desktop");}
    typedef void *(CALL *windowfn)(DWORD,const char *,const char *,DWORD,int,int,int,int,void *,void *,void *,void *);
    typedef int (CALL *destroyfn)(void *);
    typedef IP (CALL *itemfn)(void *,int,DWORD,UP,IP);
    typedef DWORD (CALL *textfn)(void *,int,char *,int);
    GET(windowfn,CreateWindowExA,u);GET(destroyfn,DestroyWindow,u);
    GET(itemfn,SendDlgItemMessageA,u);GET(textfn,GetDlgItemTextA,u);
    void *parent=CreateWindowExA(0,"STATIC","installer",0x80000000U,0,0,100,100,0,0,0,0);
    void *child=CreateWindowExA(0,"STATIC","child",0x40000000U,0,0,40,20,parent,(void *)23,0,0);
    CHECK(parent&&child,"create-dialog-controls");
    CHECK(SendDlgItemMessageA(parent,23,12,0,(IP)"installing")!=0,"send-control-message");
    CHECK(GetDlgItemTextA(parent,23,text,32)==10&&lstrcmpA(text,"installing")==0,"read-control-text");
    text[4]='#';CHECK(GetDlgItemTextA(parent,23,text,4)==3&&lstrcmpA(text,"ins")==0&&text[4]=='#',"truncate-control-text");
    SetLastError(0);CHECK(!SendDlgItemMessageA(parent,99,14,0,0),"missing-control");
    CHECK(DestroyWindow(parent),"destroy-dialog-controls");
    if(!failures)SAY("DXLEGACY DONE\n");
    ExitProcess(failures?1:0);
}
