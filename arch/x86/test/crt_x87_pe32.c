/* Exercise compiler intrinsics through their real x87-stack ABI. */
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API const char *CALL GetCommandLineA(void);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API void CALL ExitProcess(DWORD);
int _fltused;

static unsigned checks, failures;
static void report(const char *text)
{
    DWORD n=0, written;
    while (text[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11),text,n,&written,0);
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
static BOOL near(double actual, double expected)
{
    double difference=actual-expected;
    return difference>=-1e-12 && difference<=1e-12;
}
typedef union { double value; unsigned long long bits; } FP_BITS;
static BOOL is_nan(double value)
{
    FP_BITS number={.value=value};
    return (number.bits&0x7FF0000000000000ULL)==0x7FF0000000000000ULL &&
           (number.bits&0x000FFFFFFFFFFFFFULL)!=0;
}
static void partial_remainder(void)
{
    const double x=0x1p200, y=3.0;
    unsigned short status;
    __asm__ volatile ("fldl %2; fldl %1; fprem; fnstsw %0; fstp %%st(0); fstp %%st(0)"
                      : "=m"(status) : "m"(x), "m"(y) : "st", "st(1)");
    check((status&0x400)!=0,"partial remainder fixture sets C2");
}
static double intrinsic(void *entry, double x, double y, BOOL binary)
{
    const double sentinel=123.25;
    double result, remaining;
    unsigned short before, after, control_before, control_after;
    __asm__ volatile ("fnstsw %0; fnstcw %1" : "=m"(before), "=m"(control_before));
    /* Keep a live value below the arguments, as optimized callers can. */
    if (binary) {
        __asm__ volatile (
            "fldl %[sentinel]; fldl %[x]; fldl %[y]; call *%[entry]; "
            "fstpl %[result]; fstpl %[remaining]"
            : [result] "=m"(result), [remaining] "=m"(remaining)
            : [entry] "r"(entry), [x] "m"(x), [y] "m"(y), [sentinel] "m"(sentinel)
            : "eax", "ecx", "edx", "st", "st(1)", "st(2)", "st(3)",
              "st(4)", "st(5)", "st(6)", "st(7)", "xmm0", "xmm1",
              "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "cc", "memory");
    } else {
        __asm__ volatile (
            "fldl %[sentinel]; fldl %[x]; call *%[entry]; "
            "fstpl %[result]; fstpl %[remaining]"
            : [result] "=m"(result), [remaining] "=m"(remaining)
            : [entry] "r"(entry), [x] "m"(x), [sentinel] "m"(sentinel)
            : "eax", "ecx", "edx", "st", "st(1)", "st(2)", "st(3)",
              "st(4)", "st(5)", "st(6)", "st(7)", "xmm0", "xmm1",
              "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "cc", "memory");
    }
    __asm__ volatile ("fnstsw %0; fnstcw %1" : "=m"(after), "=m"(control_after));
    check(remaining==sentinel && (before&0x3800)==(after&0x3800),
          "intrinsic preserves underlying x87 stack");
    check(control_before==control_after,"intrinsic preserves x87 control word");
    return result;
}

void mainCRTStartup(void)
{
    const char *command=GetCommandLineA(); BOOL ucrt=0;
    for (;command && *command;command++) {
        if (command[0]=='u' && command[1]=='c' && command[2]=='r' && command[3]=='t') {
            ucrt=1; break;
        }
    }
    HANDLE module=LoadLibraryA(ucrt?"ucrtbase.dll":"msvcrt.dll");
    void *acos_entry=GetProcAddress(module,"_CIacos");
    void *fmod_entry=GetProcAddress(module,"_CIfmod");
    check(module && acos_entry && fmod_entry,"x87 intrinsic exports");
    if (!acos_entry || !fmod_entry) ExitProcess(1);
    const double arguments[]={0.0,0.5,-0.5,1.0,-1.0,0.9999999999999999};
    const double expected[]={1.5707963267948966,1.0471975511965977,
                             2.0943951023931955,0.0,3.141592653589793,
                             1.4901161193847656e-8};
    for (unsigned i=0;i<sizeof(arguments)/sizeof(arguments[0]);i++)
        check(near(intrinsic(acos_entry,arguments[i],0,0),expected[i]),
              "acos value in its real domain");
    const double dividends[]={7.5,-7.5,7.5,0x1p200,0x1p201,-0x1p200,
                              0x1p-900,0.0};
    const double divisors[]={2.0,2.0,-2.0,3.0,3.0,3.0,0x1.8p-950,3.0};
    const double remainders[]={1.5,-1.5,1.5,1.0,2.0,-1.0,0x1p-950,0.0};
    for (unsigned i=0;i<sizeof(dividends)/sizeof(dividends[0]);i++)
        check(intrinsic(fmod_entry,dividends[i],divisors[i],1)==remainders[i],
              "fmod completes reduction and preserves dividend sign");
    FP_BITS infinity={.bits=0x7FF0000000000000ULL};
    FP_BITS nan={.bits=0x7FF8000000000001ULL};
    FP_BITS negative_zero={.bits=0x8000000000000000ULL};
    check(is_nan(intrinsic(acos_entry,1.5,0,0)),"acos outside its real domain yields NaN");
    check(is_nan(intrinsic(acos_entry,nan.value,0,0)),"acos propagates NaN");
    check(intrinsic(fmod_entry,9.0,infinity.value,1)==9.0,"fmod with infinite divisor");
    check(is_nan(intrinsic(fmod_entry,infinity.value,3.0,1)),"fmod infinite dividend yields NaN");
    check(is_nan(intrinsic(fmod_entry,3.0,0.0,1)),"fmod zero divisor yields NaN");
    check(is_nan(intrinsic(fmod_entry,nan.value,3.0,1)),"fmod propagates NaN");
    FP_BITS zero_result={.value=intrinsic(fmod_entry,negative_zero.value,3.0,1)};
    check(zero_result.bits==negative_zero.bits,"fmod preserves negative zero");
    partial_remainder();
    check(is_nan(intrinsic(fmod_entry,infinity.value,3.0,1)),
          "invalid fmod completes with C2 already set");
    report("CRT-X87 "); number(checks); report(" checks, ");
    number(failures); report(" failures\n");
    ExitProcess(failures?1:0);
}
