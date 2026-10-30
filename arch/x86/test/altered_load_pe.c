typedef unsigned int DWORD;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API DWORD CALL GetModuleFileNameA(void *,char *,DWORD);
API void *CALL LoadLibraryExA(const char *,void *,DWORD);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL GetModuleHandleA(const char *);
API int CALL FreeLibrary(void *);
API int CALL SetCurrentDirectoryA(const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
#define SAY(s) do{DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define HEX(v) do{char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
static char directory[1024],parent[1024],cwd[1024];
static unsigned failures;
static void altered_load_check(int ok)
{if(ok)SAY("ALTERED PASS\n");else{failures++;SAY("ALTERED FAIL\n");}}
void altered_load_entry(void)
{
    DWORD n=GetModuleFileNameA(0,directory,sizeof(directory));
    altered_load_check(n>0&&n<sizeof(directory)-32);
    if(!n||n>=sizeof(directory)-32)ExitProcess(1);
    while(n&&directory[n-1]!='\\'&&directory[n-1]!='/')n--;
    directory[n]=0;
    for(unsigned i=0;i<n;i++)parent[i]=cwd[i]=directory[i];
    const char *suffix="plugin\\parent.dll";unsigned j=0;do{parent[n+j]=suffix[j];}while(suffix[j++]);
    suffix="cwd";j=0;do{cwd[n+j]=suffix[j];}while(suffix[j++]);
    altered_load_check(SetCurrentDirectoryA(cwd));
    DWORD flags[]={0,8,0,8,8,0x1008};
    DWORD values[]={0x1111,0x2222,0x1111,0x2222,0x1111,0};
    for(unsigned i=0;i<6;i++) {
#ifdef ISOLATED_CASE
        if(i!=ISOLATED_CASE)continue;
#endif
        const char *file=i<3?parent:i==3?"cwd-only.dll":"altered-dep.dll";
        SetLastError(0x12345678);void *module=LoadLibraryExA(file,0,flags[i]);DWORD error=GetLastError();
        altered_load_check(i==5?!module&&error==87:module!=0);
        DWORD value=0;
        if(module){
            int (*get)(void)=GetProcAddress(module,i<3?"altered_value":"child");
            value=get?(DWORD)get():0;
            altered_load_check(get&&value==values[i]);
            altered_load_check(FreeLibrary(module));
        }
#ifdef ISOLATED_CASE
        /* Keep the independent dependency-unload observation visible. The
         * unfiltered test above continues to assert the full native contract. */
        SAY("ALTERED RETAINED ");HEX(GetModuleHandleA("altered-dep.dll")!=0);SAY("\n");
#else
        altered_load_check(!GetModuleHandleA("altered-dep.dll"));
#endif
        SAY("ALTERED ROW ");HEX(i);HEX(module!=0);HEX(value);HEX(module?0:error);SAY("\n");
    }
    if(!failures)SAY("ALTERED DONE\n");ExitProcess(failures?1:0);
}
