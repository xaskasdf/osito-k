/* The resource directory order is deliberately different from UI preference. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
static DWORD failures;
#define SAY(s) do{DWORD n;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&n,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("RESLANG PASS " name "\n");else{SAY("RESLANG FAIL " name "\n");failures++;}}while(0)
typedef void *(CALL *FIND)(void *,const void *,const void *);
typedef void *(CALL *LOAD)(void *,void *);
typedef void *(CALL *LOCK)(void *);
typedef DWORD(CALL *SIZE)(void *,void *);
static FIND finda,findw;static LOAD load;static LOCK lock;static SIZE size;
typedef void *(CALL *FINDEX)(void *,const void *,const void *,WORD);
static FINDEX findexa,findexw;
static DWORD(CALL *geterror)(void);
static void(CALL *seterror)(DWORD);
#define HEX(v) do{char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
static void resource_language_extended(void)
{
    static const WORD languages[]={0,9,0x409,0x809,0x404,0x40c,0x411,0x400,0x800,0xffff};
    /* Recorded against native Windows, with thread UI language en-US. */
    static const WORD expected[6][10]={
        {0x409,0xffff,0x409,0xffff,0x404,0xffff,0xffff,0x409,0x409,0xffff},
        {0,0,0x409,0,0,0,0,0x409,0x409,0},
        {0x409,9,0x409,9,0xffff,0xffff,0xffff,0x409,0x409,0xffff},
        {0x40c,0xffff,0xffff,0xffff,0xffff,0x40c,0xffff,0x40c,0x40c,0xffff},
        {9,9,9,9,0xffff,0xffff,0xffff,9,9,0xffff},
        {0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff}
    };
    for(unsigned wide=0;wide<2;wide++)for(unsigned id=101;id<=106;id++){
        for(unsigned n=0;n<sizeof(languages)/sizeof(languages[0]);n++){
            seterror(0x13579);
            void *r=(wide?findexw:findexa)(0,(void *)10,(void *)(UP)id,languages[n]);
            DWORD last=geterror(),value=0xffffffffU;
            const WORD *data=r?lock(load(0,r)):0;
            if(data&&size(0,r)==2)value=*data;
            DWORD wanted=expected[id-101][n],wanted_error=0x13579;
            if(wanted==0xffff){wanted=0xffffffffU;wanted_error=id==106?1814:1815;}
            CHECK((r!=0)==(wanted!=0xffffffffU)&&value==wanted&&last==wanted_error,"explicit-language-selection-and-error");
            SAY("RESLANG EX ");HEX(wide);HEX(id);HEX(languages[n]);HEX(r!=0);HEX(last);HEX(value);SAY("\n");
        }
    }
}
static int resource_language_check(unsigned id,WORD expected)
{
    void *a=finda(0,(const void *)(UP)id,(const void *)10);
    void *w=findw(0,(const void *)(UP)id,(const void *)10);
    const WORD *data=a?lock(load(0,a)):0;
    return a&&w==a&&size(0,a)==2&&data&&*data==expected;
}
void resource_language_entry(void)
{
    void *k=LoadLibraryA("kernel32.dll");
    finda=GetProcAddress(k,"FindResourceA");findw=GetProcAddress(k,"FindResourceW");
    findexa=GetProcAddress(k,"FindResourceExA");findexw=GetProcAddress(k,"FindResourceExW");
    geterror=GetProcAddress(k,"GetLastError");seterror=GetProcAddress(k,"SetLastError");
    load=GetProcAddress(k,"LoadResource");lock=GetProcAddress(k,"LockResource");size=GetProcAddress(k,"SizeofResource");
    typedef WORD(CALL *SETLANG)(WORD);
    SETLANG setlang=GetProcAddress(k,"SetThreadUILanguage");
    if(setlang)setlang(0x409);
    CHECK(finda&&findw&&findexa&&findexw&&geterror&&seterror&&load&&lock&&size,"exports");
    if(failures)ExitProcess(1);
    CHECK(resource_language_check(101,0x409),"ui-language-before-first-directory-entry");
    CHECK(resource_language_check(102,0),"neutral-before-ui-language");
    CHECK(resource_language_check(103,0x409),"exact-before-primary-neutral");
    CHECK(resource_language_check(104,0x40c),"single-language-fallback");
    CHECK(resource_language_check(105,9),"primary-neutral-fallback");
    resource_language_extended();
    if(!failures)SAY("RESLANG DONE\n");
    ExitProcess(failures?1:0);
}
