/* Filename comparisons used by DirectX metadata selection, native and guest. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
static unsigned failures;
#define SAY(s) do {DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define HEX(v) do {char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
static void compare_case_check(int ok)
{if(ok)SAY("COMPARECASE PASS\n");else{failures++;SAY("COMPARECASE FAIL\n");}}
void compare_case_entry(void)
{
    void *module=LoadLibraryA("kernel32.dll");
    int (CALL *a)(DWORD,DWORD,const char *,int,const char *,int)=GetProcAddress(module,"CompareStringA");
    int (CALL *w)(DWORD,DWORD,const WCHAR *,int,const WCHAR *,int)=GetProcAddress(module,"CompareStringW");
    int (CALL *ex)(const WCHAR *,DWORD,const WCHAR *,int,const WCHAR *,int,void *,void *,__UINTPTR_TYPE__)=GetProcAddress(module,"CompareStringEx");
    compare_case_check(a&&w&&ex);if(failures)ExitProcess(1);
    /* Native Windows initializes locale data lazily on the first call. */
    (void)a(0x409,1,"warmup",-1,"WARMUP",-1);
    const struct {const char *left,*right;int n1,n2,result;DWORD flags;} cases[]={
        {"Apr2005_d3dx9_25_x86.inf","apr2005_d3dx9_25_x86.inf",-1,-1,2,1},
        {"Aug2009_XAudio_x86.inf","AUG2009_XAudio_x86.inf",-1,-1,2,1},
        {"Jun2010_XAudio_x86.inf","JUN2010_XAudio_x86.inf",-1,-1,2,1},
        {"Feb2006_XACT_x86.inf","feb2006_xact_x86.inf",-1,-1,2,1},
        {"Mar2008_XAudio_x86.inf","Mar2008_XAudio_x86.inf",-1,-1,2,0},
        {"ABC","abc",3,3,2,1}, {"ABC-extra","abc-other",3,3,2,1},
        {"ab","ABC",-1,-1,1,1},{"ABC","ab",-1,-1,3,1},
        {"FILE2","file3",-1,-1,1,1},{"file3","FILE2",-1,-1,3,1},
        {"","",0,0,2,1},{"x","",1,0,3,1},{"","X",0,1,1,1},
        {"same","same",-2,-2,2,1},{"unchanged","unchanged",-1,-1,2,0}
    };
    const WCHAR locale[]={'e','n','-','U','S',0};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
        WCHAR left[96],right[96];unsigned j;
        for(j=0;cases[i].left[j];j++)left[j]=(unsigned char)cases[i].left[j];left[j]=0;
        for(j=0;cases[i].right[j];j++)right[j]=(unsigned char)cases[i].right[j];right[j]=0;
        for(unsigned api=0;api<3;api++){
            SetLastError(0x12345678);int value;
            if(api==0)value=a(0x409,cases[i].flags,cases[i].left,cases[i].n1,cases[i].right,cases[i].n2);
            else if(api==1)value=w(0x409,cases[i].flags,left,cases[i].n1,right,cases[i].n2);
            else value=ex(locale,cases[i].flags,left,cases[i].n1,right,cases[i].n2,0,0,0);
            DWORD error=GetLastError();compare_case_check(value==cases[i].result);compare_case_check(error==0x12345678);
            SAY("COMPARECASE ROW ");HEX(i);HEX(api);HEX(value);HEX(error);SAY("\n");
        }
    }
    if(!failures)SAY("COMPARECASE DONE\n");ExitProcess(failures?1:0);
}
