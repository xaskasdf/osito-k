/* The same floating-point control contracts run on Windows and OsitoK. */
typedef unsigned int UINT, DWORD;
typedef int BOOL;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
int _fltused;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API const char *CALL GetCommandLineA(void);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API void CALL ExitProcess(UINT);
API void CALL Sleep(DWORD);
API DWORD CALL GetTickCount(void);
API HANDLE CALL CreateThread(void *, UPTR, DWORD (CALL *)(void *), void *, DWORD, DWORD *);
API DWORD CALL WaitForSingleObject(HANDLE, DWORD);
API BOOL CALL CloseHandle(HANDLE);

typedef unsigned (*CONTROL)(unsigned, unsigned);
typedef int (*CONTROL_S)(unsigned *, unsigned, unsigned);
typedef void (*INVALID_HANDLER)(const unsigned short *, const unsigned short *,
                                const unsigned short *, unsigned, UPTR);
typedef INVALID_HANDLER (*SET_INVALID)(INVALID_HANDLER);
typedef int *(*ERRNO_FN)(void);
static CONTROL control, control87;
static CONTROL_S control_s;
static unsigned checks, failures;
static volatile unsigned phase, thread_state;
static unsigned invalid_calls, local_invalid_calls;
enum { EM=0x8001F, DENORMAL=0x80000, RC=0x300, DOWN=0x100, UP=0x200,
       PC=0x30000, PC24=0x20000, PC53=0x10000, AMBIGUOUS=0x80000000U };

static void report(const char *text)
{
    DWORD n=0, written;
    while (text[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), text, n, &written, 0);
}
static void number(unsigned value)
{
    char digits[12]; unsigned count=0;
    do { digits[count++]='0'+value%10; value/=10; } while (value);
    while (count) { char one[2]={digits[--count],0}; report(one); }
}
static void check(BOOL ok, const char *name)
{
    checks++;
    if (!ok) { failures++; report("FAIL: "); report(name); report("\n"); }
}
static unsigned short x87_read(void)
{
    unsigned short word;
    __asm__ volatile ("fnstcw %0" : "=m"(word));
    return word;
}
static unsigned sse_read(void)
{
    unsigned word;
    __asm__ volatile ("stmxcsr %0" : "=m"(word));
    return word;
}
static void restore(unsigned short x87, unsigned sse)
{
    __asm__ volatile ("fldcw %0; ldmxcsr %1" : : "m"(x87), "m"(sse) : "memory");
}
static int x87_round(void)
{
    const double input=1.75; int result;
    __asm__ volatile ("fldl %1; fistpl %0" : "=m"(result) : "m"(input) : "st");
    return result;
}
static int sse_round(void)
{
    const double input=1.75; int result;
    __asm__ volatile ("cvtsd2si %1, %0" : "=r"(result) : "m"(input));
    return result;
}
#ifndef _WIN64
static BOOL x87_low_bit_survives(void)
{
    const double base=16777216.0; double result;
    __asm__ volatile ("fldl %1; fld1; faddp; fsubl %1; fstpl %0"
                      : "=m"(result) : "m"(base) : "st", "st(1)");
    return result==1.0;
}
#endif
static DWORD CALL worker(void *unused)
{
    (void)unused;
    control(UP, RC);
    phase=1;
    DWORD start=GetTickCount();
    while (phase==1 && (DWORD)(GetTickCount()-start)<3000) Sleep(1);
    thread_state=control(0,0)&RC;
    return 0;
}
static void invalid_parameter(const unsigned short *expression,
                              const unsigned short *function,
                              const unsigned short *file, unsigned line,
                              UPTR reserved)
{
    (void)expression; (void)function; (void)file; (void)line; (void)reserved;
    invalid_calls++;
}
static void local_invalid_parameter(const unsigned short *expression,
                                    const unsigned short *function,
                                    const unsigned short *file, unsigned line,
                                    UPTR reserved)
{
    (void)expression; (void)function; (void)file; (void)line; (void)reserved;
    local_invalid_calls++;
}

void mainCRTStartup(void)
{
    const char *command=GetCommandLineA(); BOOL ucrt=0;
    for (;command && *command;command++) {
        if (command[0]=='u' && command[1]=='c' && command[2]=='r' && command[3]=='t') {
            ucrt=1; break;
        }
    }
    HANDLE module=LoadLibraryA(ucrt ? "ucrtbase.dll" : "msvcrt.dll");
    control=(CONTROL)GetProcAddress(module,"_controlfp");
    control_s=(CONTROL_S)GetProcAddress(module,"_controlfp_s");
    control87=(CONTROL)GetProcAddress(module,"_control87");
    check(module && control && control_s,"CRT control exports");
    if (!control || !control_s) ExitProcess(1);
    unsigned short original_x87=x87_read();
    unsigned original_sse=sse_read();
    unsigned initial=control(0,0), current=0xDEADBEEF;
    check(control_s(&current,0,0)==0 && current==initial,"secure query reports actual control");
    check(x87_read()==original_x87 && sse_read()==original_sse,"queries preserve registers");
    unsigned result=control(DOWN,RC);
    check((result&RC)==DOWN && (control(0,0)&RC)==DOWN,"round-down readback");
    check((sse_read()&0x6000)==0x2000 && sse_round()==1,"round-down configures SSE arithmetic");
#ifdef _WIN64
    check(x87_read()==original_x87,"PE64 control leaves x87 unchanged");
#else
    check((x87_read()&0xC00)==0x400 && x87_round()==1,"round-down configures x87 arithmetic");
    result=control(PC24,PC);
    check((result&PC)==PC24 && (x87_read()&0x300)==0,"24-bit x87 precision");
    result=control(PC53,PC);
    check((result&PC)==PC53 && (x87_read()&0x300)==0x200,"53-bit x87 precision");
    control(PC24,PC|RC);
    check(!x87_low_bit_survives(),"24-bit precision affects arithmetic");
    control(PC53,PC);
    check(x87_low_bit_survives(),"53-bit precision preserves additional mantissa bits");
#endif
    check(control_s(&current,UP,RC)==0 && (current&RC)==UP,"secure setter reports new state");
    check((control(0,0)&RC)==UP && sse_round()==2,"secure setter configures SSE arithmetic");
#ifndef _WIN64
    check(x87_round()==2,"secure setter configures x87 arithmetic");
#endif
    check(control_s(0,DOWN,RC)==0 && (control(0,0)&RC)==DOWN,"secure setter permits null output");
    restore(original_x87,original_sse);
    unsigned short direct_x87=(original_x87&~0xC00)|0x800;
    unsigned direct_sse=(original_sse&~0x6000)|0x4000;
    restore(direct_x87,direct_sse);
    check((control(0,0)&RC)==UP,"query observes direct hardware changes");
    for (unsigned mode=0;mode<4;mode++) {
        const unsigned hardware[4]={0,0x8040,0x40,0x8000};
        result=control(mode<<24,0x03000000);
        check((result&0x03000000)==(mode<<24) &&
              (sse_read()&0x8040)==hardware[mode],"SSE denormal operand/result mode");
    }
    restore(original_x87, original_sse|0x21);
    control(0,0);
    check((sse_read()&0x3F)==((original_sse|0x21)&0x3F),"query preserves SSE status flags");
    control(DOWN,RC);
    check((sse_read()&0x3F)==0,"control change clears pending SSE exceptions");
    restore(original_x87,original_sse);
    check(control87!=0,"control87 export");
    if (control87) {
        const unsigned abstract[6]={0x10,DENORMAL,8,4,2,1};
        for (unsigned i=0;i<6;i++) {
            control87(EM,EM);
            /* Earlier arithmetic leaves x87 flags pending; isolate mask tests. */
            __asm__ volatile ("fnclex" : : : "memory");
            control87(0,abstract[i]);
            check((control87(0,0)&EM)==(EM&~abstract[i]) &&
                  !(sse_read()&(1U<<(i+7))),"exception mask mapping and isolation");
#ifndef _WIN64
            check(!(x87_read()&(1U<<i)),"x87 exception mask mapping");
#endif
            control87(EM,EM);
        }
        control87(0,DENORMAL);
        check(!(control87(0,0)&DENORMAL),"control87 unmasks denormal operand");
        control(DENORMAL,DENORMAL);
        check(!(control87(0,0)&DENORMAL),"controlfp preserves denormal exception mask");
        control87(DENORMAL,DENORMAL);
        check((control87(0,0)&DENORMAL)!=0,"control87 restores denormal mask");
    }
#ifndef _WIN64
    restore((original_x87&~0xC00)|0x400, original_sse&~0x6000);
    check((control(0,0)&AMBIGUOUS)!=0,"query flags divergent x87/SSE control");
    control(DOWN,RC);
    check(!(control(0,0)&AMBIGUOUS),"matching controls clear ambiguity");
#endif
    control(DOWN,RC);
    HANDLE thread=CreateThread(0,0,worker,0,0,0);
    check(thread!=0,"create independent FP thread");
    if (thread) {
        DWORD start=GetTickCount();
        while (!phase && (DWORD)(GetTickCount()-start)<3000) Sleep(1);
        check(phase==1,"FP worker reached checkpoint");
        check((control(0,0)&RC)==DOWN && sse_round()==1,"worker preserves parent FP state");
        phase=2;
        check(WaitForSingleObject(thread,5000)==0,"FP worker joins");
        check(thread_state==UP,"parent preserves worker FP state");
        CloseHandle(thread);
    }
    restore(original_x87,original_sse);
    SET_INVALID set_invalid=(SET_INVALID)GetProcAddress(module,"_set_invalid_parameter_handler");
    ERRNO_FN get_errno=(ERRNO_FN)GetProcAddress(module,"_errno");
    if (ucrt) check(set_invalid && get_errno,"invalid parameter handler and errno exports");
    if (set_invalid && get_errno) {
        INVALID_HANDLER previous=set_invalid(invalid_parameter);
        unsigned short before_x87=x87_read(); unsigned before_sse=sse_read();
        *get_errno()=0;
        int error=control_s(&current,0x80000000U,0x80000000U);
        check(error==22 && *get_errno()==22,"invalid selected control bit reports EINVAL");
        check(invalid_calls==1,"secure control invokes invalid parameter handler");
        check(x87_read()==before_x87 && sse_read()==before_sse,"invalid request preserves hardware");
#ifdef _WIN64
        error=control_s(&current,PC24,PC);
        check(error==0 && invalid_calls==1 && x87_read()==before_x87,
              "release PE64 CRT ignores x87-only precision control");
#endif
        SET_INVALID set_local=(SET_INVALID)GetProcAddress(module,"_set_thread_local_invalid_parameter_handler");
        if (ucrt) check(set_local!=0,"thread-local invalid parameter handler export");
        if (set_local) {
            INVALID_HANDLER local_previous=set_local(local_invalid_parameter);
            unsigned global_calls=invalid_calls;
            error=control_s(&current,0x80000000U,0x80000000U);
            check(error==22 && local_invalid_calls==1 && invalid_calls==global_calls,
                  "thread-local invalid parameter handler takes precedence");
            set_local(local_previous);
        }
        set_invalid(previous);
    }
    report("CRT-FP "); number(checks); report(" checks, ");
    number(failures); report(" failures\n");
    ExitProcess(failures?1:0);
}
