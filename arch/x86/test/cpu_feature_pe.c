/* Native/guest feature reporting compared with CPUID and enabled XCR0 state. */
typedef unsigned int DWORD;
typedef unsigned long long U64;
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
static void feature_check(int ok)
{if(ok)SAY("CPUFEATURE PASS\n");else{failures++;SAY("CPUFEATURE FAIL\n");}}
static void feature_cpuid(DWORD leaf,DWORD *b,DWORD *c,DWORD *d)
{DWORD a;__asm__ volatile("cpuid":"=a"(a),"=b"(*b),"=c"(*c),"=d"(*d):"a"(leaf),"c"(0));}
void cpu_feature_entry(void)
{
    void *module=LoadLibraryA("kernel32.dll");
    int (CALL *present)(DWORD)=GetProcAddress(module,"IsProcessorFeaturePresent");
    feature_check(present!=0);if(!present)ExitProcess(1);
    DWORD b,c,d,eb,ec,ed,lo=0,hi=0;
    feature_cpuid(1,&b,&c,&d);feature_cpuid(7,&eb,&ec,&ed);
    if((c>>27)&1)__asm__ volatile("xgetbv":"=a"(lo),"=d"(hi):"c"(0));
    U64 xcr=(U64)lo|((U64)hi<<32);int avx_state=((c>>27)&1)&&((xcr&6)==6);
    const DWORD flags[]={6,10,13,17,36,37,38,39,40,41,29,0xffffffff};
    int expected[]={ (d>>25)&1,(d>>26)&1,c&1,((c>>26)&1)&&((c>>27)&1),
        (c>>9)&1,(c>>19)&1,(c>>20)&1,((c>>28)&1)&&avx_state,
        ((c>>28)&1)&&avx_state&&((eb>>5)&1),avx_state&&((xcr&0xe0)==0xe0)&&((eb>>16)&1),0,0 };
    SAY("CPUFEATURE CPUID ");HEX(c);HEX(d);HEX(eb);HEX(lo);HEX(hi);SAY("\n");
    for(unsigned i=0;i<sizeof(flags)/sizeof(flags[0]);i++){
        SetLastError(0x12345678);int actual=present(flags[i]);DWORD error=GetLastError();
        feature_check(!!actual==expected[i]);feature_check(error==0x12345678);
        SAY("CPUFEATURE ROW ");HEX(flags[i]);HEX(actual);HEX(expected[i]);HEX(error);SAY("\n");
    }
    if(!failures)SAY("CPUFEATURE DONE\n");ExitProcess(failures?1:0);
}
