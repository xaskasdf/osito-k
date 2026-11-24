/* C++ decorated allocation imports; no OOM/exception-handler claim. */
typedef unsigned int DWORD;
typedef __SIZE_TYPE__ SIZE_T;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
#define SAY(s) do { DWORD w_; WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&w_,0); } while (0)
#define CHECK(v) do { if (v) SAY("CRTNEW PASS\n");else {failures++;SAY("CRTNEW FAIL\n");} } while (0)
void new_crt_entry(void)
{
    DWORD failures=0;
    void *crt=LoadLibraryA("msvcrt.dll");
    const char *names32[]={"??2@YAPAXI@Z","??3@YAXPAX@Z","??_U@YAPAXI@Z","??_V@YAXPAX@Z"};
    const char *names64[]={"??2@YAPEAX_K@Z","??3@YAXPEAX@Z","??_U@YAPEAX_K@Z","??_V@YAXPEAX@Z"};
    const char **names=sizeof(void*)==4?names32:names64;
    void *functions[4];
    for (DWORD i=0;i<4;i++) { functions[i]=GetProcAddress(crt,names[i]);CHECK(functions[i]!=0); }
    if (failures) ExitProcess(1);
    const SIZE_T sizes[]={0,1,24,257,4096,65537};
    for (DWORD kind=0;kind<2;kind++) {
        void *(*allocate)(SIZE_T)=functions[kind*2];
        void (*release)(void*)=functions[kind*2+1];
        for (DWORD i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
            SIZE_T n=sizes[i];
            volatile unsigned char *p=allocate(n);
            CHECK(p!=0);
            if (!p) continue;
            CHECK(((SIZE_T)p & (sizeof(void*)==4?7:15))==0);
            for (SIZE_T j=0;j<n;j++) p[j]=(unsigned char)(j*37+13);
            int valid=1;
            for (SIZE_T j=0;j<n;j++) if(p[j]!=(unsigned char)(j*37+13)) valid=0;
            CHECK(valid);
            release((void*)p);
        }
        release(0);
        CHECK(1);
    }
    if (!failures) SAY("CRTNEW DONE\n");
    ExitProcess(failures?1:0);
}
