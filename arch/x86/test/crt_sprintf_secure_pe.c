/* Differential probe for the legacy MSVCRT APIs imported by d3d9.dll. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef __SIZE_TYPE__ SZ;
typedef __builtin_va_list VA;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API void CALL ExitProcess(DWORD);
int _fltused;
static int (*format)(char *,SZ,const char *,...);
static int (*vformat)(char *,SZ,const char *,VA);
static int *(*error_slot)(void);
static unsigned checks,failures,invalid_calls,invalid_args;
static int invalid_errno;
static unsigned char *invalid_buffer;
static unsigned invalid_first;
#define CHECK(x) do {checks++;if(!(x))failures++;} while(0)
static void invalid(const WCHAR *a,const WCHAR *b,const WCHAR *c,unsigned d,SZ e)
{
    invalid_calls++;invalid_args+=(a!=0)+(b!=0)+(c!=0)+(d!=0)+(e!=0);
    invalid_errno=*error_slot();
    invalid_first=invalid_buffer?invalid_buffer[0]:0;
}
static int call_v(char *buffer,SZ size,const char *fmt,...)
{
    VA ap;__builtin_va_start(ap,fmt);
    int result=vformat(buffer,size,fmt,ap);
    __builtin_va_end(ap);return result;
}
static char row[400];
static unsigned used;
static void number(unsigned value)
{
    for(int shift=28;shift>=0;shift-=4)row[used++]="0123456789abcdef"[(value>>shift)&15];
    row[used++]=' ';
}
static void emit(const char *tag)
{
    DWORD n=0,written;while(tag[n])n++;
    WriteFile(GetStdHandle((DWORD)-11),tag,n,&written,0);
    row[used-1]='\n';WriteFile(GetStdHandle((DWORD)-11),row,used,&written,0);used=0;
}
void sprintf_secure_entry(void)
{
    void *module=LoadLibraryA("msvcrt.dll");
    format=module?GetProcAddress(module,"sprintf_s"):0;
    vformat=module?GetProcAddress(module,"vsprintf_s"):0;
    int (*iscsym)(int)=module?GetProcAddress(module,"__iscsym"):0;
    void *(*handler)(void *)=module?GetProcAddress(module,"_set_invalid_parameter_handler"):0;
    error_slot=module?GetProcAddress(module,"_errno"):0;
    CHECK(module&&format&&vformat&&iscsym&&error_slot);
    if(failures){number(failures);emit("SPRINTSEC MISSING ");ExitProcess(2);}
    /* The OS-supplied legacy MSVCRT does not export a handler setter.
     * Its supported rows remain a differential oracle; exercise the
     * documented error/handler contract when the provider exposes it. */
    void *previous=handler?handler((void *)invalid):0;
    /* Include EOF and all byte values under the default C locale. */
    for(int c=-1;c<256;c++) {
        *error_slot()=123;
        int result=iscsym(c);
        int want=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
        CHECK((result!=0)==want);CHECK(*error_slot()==123);
    }
    for(unsigned path=0;path<2;path++)for(unsigned trial=0;trial<23;trial++) {
        int bad=trial<=4||(trial>=7&&trial<=13)||trial==18;
        if(bad&&!handler)continue;
        unsigned char memory[96];
        for(unsigned i=0;i<sizeof(memory);i++)memory[i]=0x5a;
        char *dest=(char *)memory+8;
        SZ size=72;
        const char *fmt="%s:%d/%I64x/%.2f";
        if(trial<7){size=trial;fmt="%s";}
        if(trial==7){dest=0;size=0;fmt=0;}
        if(trial==8){dest=0;fmt="%s";}
        if(trial==9)fmt=0;
        if(trial==10){size=0;fmt="%s";}
        if(trial==11)fmt="pre%q";
        if(trial==12)fmt="pre%";
        if(trial==13)fmt="pre%n";
        if(trial==14)fmt="%*.*s";
        if(trial==15)fmt="%+08d %04X %%";
        if(trial==16)fmt="";
        if(trial==17){size=1;fmt="";}
        if(trial==18){size=1;fmt="x";}
        if(trial==19)fmt="%s";
        if(trial==20)fmt="%hs";
        if(trial==21)fmt="%.0d/%#.0x/%I64u";
        invalid_calls=invalid_args=0;invalid_errno=-1;invalid_first=0xffffffff;
        invalid_buffer=size?(unsigned char *)dest:0;
        *error_slot()=123;SetLastError(0x12345678);
        int result,nvalue=0x12345678;
#define RUN(...) (path?call_v(dest,size,fmt,__VA_ARGS__):format(dest,size,fmt,__VA_ARGS__))
        if(trial==13)result=RUN(&nvalue);
        else if(trial==14)result=RUN(-9,3,"abcdefgh");
        else if(trial==15)result=RUN(123,0xab);
        else if(trial==19)result=RUN((char *)0);
        else if(trial==21)result=RUN(0,0,0x12345678abcdef01ULL);
        else result=RUN("abcd",-42,0x12345678abcdef01ULL,1.25);
#undef RUN
        int err=*error_slot();DWORD last=GetLastError();
        number(path);number(trial);number((unsigned)result);number((unsigned)err);
        number(last);number(invalid_calls);number((unsigned)invalid_errno);
        number(invalid_first);number((unsigned)nvalue);
        for(unsigned i=0;i<sizeof(memory);i++) {
            row[used++]="0123456789abcdef"[memory[i]>>4];
            row[used++]="0123456789abcdef"[memory[i]&15];
        }
        row[used++]=' ';emit("SPRINTSEC ");
        CHECK(!invalid_args);CHECK(last==0x12345678);
        for(unsigned i=0;i<8;i++)CHECK(memory[i]==0x5a);
        for(unsigned i=80;i<96;i++)CHECK(memory[i]==0x5a);
        for(unsigned i=8+(dest?size:0);i<80;i++)CHECK(memory[i]==0x5a);
        CHECK(nvalue==0x12345678);
        if(bad) {
            CHECK(result<0);CHECK(invalid_calls==1);
            CHECK(err==((trial>=1&&trial<=4)||trial==18?34:22));
            if(dest&&size)CHECK(!dest[0]);
        } else {CHECK(result>=0);CHECK(!invalid_calls);CHECK(err==123);}
    }
    if(handler)handler(previous);
    number(checks);number(failures);emit("SPRINTSEC DONE ");
    ExitProcess(failures?1:0);
}
