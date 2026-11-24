/* GetFullPathName's optional output is one guest pointer, not a host pointer. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
void *memcpy(void *p,const void *q,__UINTPTR_TYPE__ n)
{unsigned char *b=p;const unsigned char *a=q;for(__UINTPTR_TYPE__ i=0;i<n;i++)b[i]=a[i];return p;}
static unsigned failures;
#define SAY(s) do{DWORD n;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&n,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("PATHPTR PASS " name "\n");else{SAY("PATHPTR FAIL " name "\n");failures++;}}while(0)
static int equal(const char *a,const char *b)
{while(*a&&*a==*b){a++;b++;}return *a==*b;}
void fullpath_pointer_entry(void)
{
    void *module=LoadLibraryA("kernel32.dll");
    typedef DWORD (CALL *pathA_fn)(const char *,DWORD,char *,char **);
    typedef DWORD (CALL *pathW_fn)(const WCHAR *,DWORD,WCHAR *,WCHAR **);
    pathA_fn pathA=GetProcAddress(module,"GetFullPathNameA");
    pathW_fn pathW=GetProcAddress(module,"GetFullPathNameW");
    CHECK(pathA&&pathW,"exports");
    if(failures)ExitProcess(1);
    char buffer[260];WCHAR wide[260];
    struct{char *part;volatile DWORD guard;}outA;
    struct{WCHAR *part;volatile DWORD guard;}outW;
    const char input[]="C:\\osito\\shader.hlsl";
    const WCHAR inputW[]={'C',':','\\','o','s','i','t','o','\\','s','h','a','d','e','r','.','h','l','s','l',0};
    outA.part=0;outA.guard=0x13579bdf;
    DWORD n=pathA(input,260,buffer,&outA.part);
    CHECK(n==20&&outA.part==buffer+9&&equal(outA.part,"shader.hlsl"),"ansi-filepart");
    CHECK(outA.guard==0x13579bdf,"ansi-output-pointer-width");
    outW.part=0;outW.guard=0x2468ace0;
    n=pathW(inputW,260,wide,&outW.part);
    CHECK(n==20&&outW.part==wide+9&&outW.part[0]=='s'&&outW.part[10]=='l'&&!outW.part[11],"wide-filepart");
    CHECK(outW.guard==0x2468ace0,"wide-output-pointer-width");
    outA.guard=0x13579bdf;outW.guard=0x2468ace0;
    CHECK(pathA(input,1,buffer,&outA.part)==21&&outA.guard==0x13579bdf,"ansi-size-query-guard");
    CHECK(pathW(inputW,1,wide,&outW.part)==21&&outW.guard==0x2468ace0,"wide-size-query-guard");
    if(!failures)SAY("PATHPTR DONE\n");
    ExitProcess(failures?1:0);
}
