/* Read-only adapter/mode probes; identical exports are exercised on Windows. */
typedef unsigned int DWORD, UINT;
typedef unsigned short WORD;
typedef int BOOL, LONG;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
typedef struct { DWORD a; WORD b,c; unsigned char d[8]; } GUID;
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API BOOL CALL FreeLibrary(HANDLE);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API void CALL ExitProcess(UINT);
API BOOL CALL EnumDisplaySettingsA(const char *, DWORD, void *);
API LONG CALL ChangeDisplaySettingsA(void *, DWORD);
typedef BOOL (CALL *ENUM_A_CB)(GUID *, char *, char *, void *);
typedef BOOL (CALL *ENUM_W_CB)(GUID *, WORD *, WORD *, void *);
typedef BOOL (CALL *ENUM_EX_A_CB)(GUID *, char *, char *, void *, HANDLE);
typedef BOOL (CALL *ENUM_EX_W_CB)(GUID *, WORD *, WORD *, void *, HANDLE);
typedef LONG (CALL *ENUM_A)(ENUM_A_CB, void *);
typedef LONG (CALL *ENUM_W)(ENUM_W_CB, void *);
typedef LONG (CALL *ENUM_EX_A)(ENUM_EX_A_CB, void *, DWORD);
typedef LONG (CALL *ENUM_EX_W)(ENUM_EX_W_CB, void *, DWORD);

static UINT checks, failures, callbacks, primary;
static void *context;
static BOOL continue_enum;
void *memset(void *p, int c, __SIZE_TYPE__ n)
{
    unsigned char *b=p; while(n--) *b++=(unsigned char)c; return p;
}
static void report(const char *s)
{
    DWORD n=0,written; while(s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11),s,n,&written,0);
}
static void number(DWORD value)
{
    char b[11]; UINT n=10; b[n]=0;
    do { b[--n]='0'+value%10; value/=10; } while(value);
    report(b+n);
}
static void hex(DWORD value)
{
    char b[9]; for(UINT i=0;i<8;i++) b[i]="0123456789ABCDEF"[(value>>(28-4*i))&15];
    b[8]=0; report(b);
}
static void check(BOOL ok,const char *name)
{
    checks++; if(!ok) {failures++; report("FAIL "); report(name); report("\n");}
}
static BOOL enum_common(GUID *id,void *ctx)
{
    callbacks++; if(!id) primary++;
    check(ctx==context,"adapter callback preserves context");
    return continue_enum;
}
static BOOL CALL adapter_a(GUID *id,char *description,char *name,void *ctx)
{
    check(description && description[0],"ANSI adapter description accessible");
    check(name && name[0],"ANSI adapter name accessible");
    return enum_common(id,ctx);
}
static BOOL CALL adapter_w(GUID *id,WORD *description,WORD *name,void *ctx)
{
    check(description && description[0],"Unicode adapter description accessible");
    check(name && name[0],"Unicode adapter name accessible");
    return enum_common(id,ctx);
}
static BOOL CALL adapter_ex_a(GUID *id,char *description,char *name,void *ctx,HANDLE monitor)
{
    if(!id) check(!monitor,"primary default adapter has no monitor handle");
    return adapter_a(id,description,name,ctx);
}
static BOOL CALL adapter_ex_w(GUID *id,WORD *description,WORD *name,void *ctx,HANDLE monitor)
{
    if(!id) check(!monitor,"primary default wide adapter has no monitor handle");
    return adapter_w(id,description,name,ctx);
}
static void adapters(HANDLE dll)
{
    ENUM_A a=(ENUM_A)GetProcAddress(dll,"DirectDrawEnumerateA");
    ENUM_W w=(ENUM_W)GetProcAddress(dll,"DirectDrawEnumerateW");
    ENUM_EX_A exa=(ENUM_EX_A)GetProcAddress(dll,"DirectDrawEnumerateExA");
    ENUM_EX_W exw=(ENUM_EX_W)GetProcAddress(dll,"DirectDrawEnumerateExW");
    check(a && w && exa && exw,"all adapter enumeration exports exist");
    context=(void *)((UPTR)&checks); continue_enum=1;
    if(a) {
        check(a(0,context)==(LONG)0x80070057,"ANSI null callback rejected");
        callbacks=primary=0;
        check(a(adapter_a,context)==0 && callbacks && primary==1,"ANSI primary enumeration");
    }
    if(w) {
        check(w(0,context)==(LONG)0x80070057,"Unicode null callback rejected");
        callbacks=primary=0;
        LONG result=w(adapter_w,context);
        report("[ENUM-W] result=0x"); hex(result); report(" callbacks="); number(callbacks); report("\n");
        check((result==0 && callbacks && primary==1) || result==(LONG)0x80004001,
              "Unicode primary enumeration or unsupported");
    }
    if(exa) {
        check(exa(0,context,0)==(LONG)0x80070057,"extended null callback rejected");
        callbacks=0;
        check(exa(adapter_ex_a,context,8)==(LONG)0x80070057 && !callbacks,
              "extended invalid flags rejected without callback");
        for(UINT flags=0;flags<8;flags++) {
            callbacks=primary=0; continue_enum=flags!=7;
            check(exa(adapter_ex_a,context,flags)==0 && callbacks && primary==1,
                  "extended enumeration flags");
            if(!continue_enum) check(callbacks==1,"extended callback cancellation");
        }
    }
    if(exw) {
        callbacks=primary=0; continue_enum=0;
        LONG result=exw(adapter_ex_w,context,7);
        report("[ENUM-EX-W] result=0x"); hex(result); report(" callbacks="); number(callbacks); report("\n");
        check((result==0 && callbacks==1 && primary==1) || result==(LONG)0x80004001,
              "wide extended enumeration and cancellation");
    }
}

static void user_modes(void)
{
    DWORD dm[39]={0}; ((WORD *)dm)[18]=156;
    check(EnumDisplaySettingsA(0,(DWORD)-1,dm),"current display mode");
    DWORD current_w=dm[27],current_h=dm[28];
    report("[DISPLAY-CURRENT] "); number(current_w); report("x"); number(current_h); report("\n");
    DWORD count=0,found=0;
    for(DWORD i=0;i<8192;i++) {
        memset(dm,0,sizeof(dm)); ((WORD *)dm)[18]=156;
        if(!EnumDisplaySettingsA(0,i,dm)) break;
        count++;
        check(dm[27] && dm[28] && dm[26],"enumerated display dimensions/depth");
        check(ChangeDisplaySettingsA(dm,2)==0,"enumerated display mode passes CDS_TEST");
        if(dm[27]==current_w && dm[28]==current_h) found++;
        report("[DISPLAY-MODE] "); number(dm[27]); report("x"); number(dm[28]);
        report("x"); number(dm[26]); report(" @"); number(dm[30]); report("\n");
    }
    check(count && count<8192 && found,"mode enumeration includes desktop");
}

#ifndef _WIN64
typedef struct { void **v; } COM;
typedef LONG (CALL *CREATE)(GUID *,COM **,void *);
typedef LONG (CALL *QUERY)(COM *,const GUID *,COM **);
typedef DWORD (CALL *RELEASE)(COM *);
typedef LONG (CALL *MODE_CB)(DWORD *,void *);
typedef LONG (CALL *MODES)(COM *,DWORD,DWORD *,void *,MODE_CB);
static DWORD expected_size,expected_bpp,expected_w,expected_h,expected_refresh,mode_count;
static BOOL stop_mode,refresh_requested;
static LONG CALL mode_callback(DWORD *d,void *ctx)
{
    check(ctx==context,"mode callback context");
    check(d[0]==expected_size,"mode descriptor ABI size");
    check(d[2] && d[3],"DirectDraw mode dimensions");
    check(d[18]==32,"mode pixel format size");
    if(refresh_requested && (d[1]&0x40000)) check(d[6]>0,"refresh value uses dwRefreshRate field");
    if(expected_bpp) check(d[21]==expected_bpp,"mode depth filter");
    if(expected_w) check(d[3]==expected_w,"mode width filter");
    if(expected_h) check(d[2]==expected_h,"mode height filter");
    if(expected_refresh) check(d[6]==expected_refresh,"mode refresh filter");
    if(mode_count<4) {
        report("[DDRAW-MODE] "); number(d[3]); report("x"); number(d[2]);
        report("x"); number(d[21]); report(" @"); number(d[6]); report("\n");
    }
    mode_count++; return !stop_mode;
}
static void directdraw_modes(HANDLE dll)
{
    CREATE create=(CREATE)GetProcAddress(dll,"DirectDrawCreate");
    COM *dd=0; check(create && create(0,&dd,0)==0 && dd,"DirectDraw creation");
    if(!dd) return;
    static const GUID versions[]={
        {0x6C14DB80,0xA733,0x11CE,{0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60}},
        {0xB3A6F3E0,0x2B43,0x11CF,{0xA2,0xDE,0x00,0xAA,0x00,0xB9,0x33,0x56}},
        {0x15E65EC0,0x3B9C,0x11D2,{0xB9,0x2F,0x00,0x60,0x97,0x97,0xEA,0x5B}}
    };
    for(UINT v=0;v<3;v++) {
        COM *obj=0;
        check(((QUERY)dd->v[0])(dd,&versions[v],&obj)==0 && obj,"DirectDraw interface query");
        if(!obj) continue;
        MODES modes=(MODES)obj->v[8]; expected_size=v==2?124:108;
        check(modes(obj,0,0,context,0)==(LONG)0x80070057,"null mode callback rejected");
        DWORD filter[31]={0}; filter[0]=expected_size;
        filter[1]=0x1000; filter[18]=32; filter[19]=0x40; filter[21]=32;
        expected_bpp=32; mode_count=0; stop_mode=0; refresh_requested=v>0;
        DWORD flags=refresh_requested?1:0;
        check(modes(obj,flags,filter,context,mode_callback)==0 && mode_count,"32-bit display modes");
        mode_count=0; stop_mode=1;
        LONG result=modes(obj,flags,filter,context,mode_callback);
        report("[MODE-CANCEL] version="); number(v); report(" result=0x"); hex(result);
        report(" callbacks="); number(mode_count); report("\n");
        check(result==0 && mode_count==1,"mode cancellation");
        filter[0]=8; mode_count=0;
        check(modes(obj,0,filter,context,mode_callback)==(LONG)0x80070057 && !mode_count,
              "bad mode filter size rejected");
        filter[0]=expected_size; filter[1]|=0x40000; filter[6]=123;
        expected_refresh=123; mode_count=0; stop_mode=0;
        check(modes(obj,flags,filter,context,mode_callback)==0,"unsupported refresh enumeration succeeds empty");
        expected_refresh=0;
        ((RELEASE)obj->v[2])(obj);
    }
    static const GUID d3d7={0xF5049E77,0x4861,0x11D2,{0xA4,0x07,0x00,0xA0,0xC9,0x06,0x29,0xA8}};
    COM *d3d=0;
    LONG result=((QUERY)dd->v[0])(dd,&d3d7,&d3d);
    report("[DIRECT3D7] QueryInterface=0x"); hex(result); report("\n");
    if(result==0 && d3d) ((RELEASE)d3d->v[2])(d3d);
    ((RELEASE)dd->v[2])(dd);
}
#endif
void mainCRTStartup(void)
{
    user_modes();
    HANDLE dll=LoadLibraryA("ddraw.dll");
    check(dll!=0,"load DirectDraw");
    if(dll) {
        adapters(dll);
#ifndef _WIN64
        directdraw_modes(dll);
#endif
        FreeLibrary(dll);
    }
    report("[DDRAW-DISPLAY] checks="); number(checks); report(" failures="); number(failures);
    report(failures?" FAIL\n":" PASS\n"); ExitProcess(failures?3:0);
}
