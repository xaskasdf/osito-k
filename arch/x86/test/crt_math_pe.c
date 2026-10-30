/* Native/Osito CRT comparison: double stack arguments and MSVC x87 intrinsics. */
typedef unsigned int DWORD;
typedef unsigned long long U64;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API const char *CALL GetCommandLineA(void);
int _fltused;
static DWORD failures,checks;
static void say(const char *text)
{DWORD n=0,written;while(text[n])n++;WriteFile(GetStdHandle((DWORD)-11),text,n,&written,0);}
static void check(int ok,const char *name)
{checks++;say(ok?"MATH PASS ":"MATH FAIL ");say(name);say("\n");if(!ok)failures++;}
void *memset(void *destination,int value,UPTR size)
{unsigned char *p=destination;for(UPTR i=0;i<size;i++)p[i]=(unsigned char)value;return destination;}
void *memcpy(void *destination,const void *source,UPTR size)
{unsigned char *p=destination;const unsigned char *q=source;for(UPTR i=0;i<size;i++)p[i]=q[i];return destination;}
typedef union {double value;U64 bits;} FP;
static U64 bits(double value){FP f={.value=value};return f.bits;}
#if defined(__i386__)
static void show_bits(double value)
{
    U64 raw=bits(value);char output[18];
    for(unsigned i=0;i<16;i++)output[i]="0123456789abcdef"[(raw>>(60-i*4))&15];
    output[16]='\n';output[17]=0;say(output);
}
#endif
static int nan(double value)
{U64 b=bits(value)&0x7fffffffffffffffULL;return b>0x7ff0000000000000ULL;}
#if defined(__i386__)
static int near(double value,double expected)
{double diff=value-expected,tolerance=1e-12*(1+(expected<0?-expected:expected));return diff<=tolerance&&diff>=-tolerance;}
static double intrinsic(void *entry,double x,double y,int binary)
{
    const double sentinel=123.25;
    double result,remaining;
    unsigned short before,after,control_before,control_after;
    __asm__ volatile("fnstsw %0; fnstcw %1":"=m"(before),"=m"(control_before));
    if(binary){
        __asm__ volatile(
            "fldl %[sentinel]; fldl %[x]; fldl %[y]; call *%[entry]; "
            "fstpl %[result]; fstpl %[remaining]"
            :[result]"=m"(result),[remaining]"=m"(remaining)
            :[entry]"r"(entry),[x]"m"(x),[y]"m"(y),[sentinel]"m"(sentinel)
            :"eax","ecx","edx","st","st(1)","st(2)","st(3)","st(4)",
             "st(5)","st(6)","st(7)","xmm0","xmm1","xmm2","xmm3", "xmm4","xmm5","xmm6","xmm7","cc","memory");
    }else{
        __asm__ volatile(
            "fldl %[sentinel]; fldl %[x]; call *%[entry]; "
            "fstpl %[result]; fstpl %[remaining]"
            :[result]"=m"(result),[remaining]"=m"(remaining)
            :[entry]"r"(entry),[x]"m"(x),[sentinel]"m"(sentinel)
            :"eax","ecx","edx","st","st(1)","st(2)","st(3)","st(4)",
             "st(5)","st(6)","st(7)","xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","cc","memory");
    }
    __asm__ volatile("fnstsw %0; fnstcw %1":"=m"(after),"=m"(control_after));
    check(remaining==sentinel&&(before&0x3800)==(after&0x3800),"intrinsic-live-x87-stack");
    check(control_before==control_after,"intrinsic-control-word");
    return result;
}
#endif
void math_crt_entry(void)
{
    void *crt=LoadLibraryA("msvcrt.dll");
    typedef double (*frexp_fn)(double,int *);
    typedef double (*ldexp_fn)(double,int);
    typedef double (*modf_fn)(double,double *);
    typedef int *(*errno_fn)(void);
    frexp_fn split=GetProcAddress(crt,"frexp");
    ldexp_fn scale=GetProcAddress(crt,"ldexp");
    modf_fn fraction=GetProcAddress(crt,"modf");
    errno_fn error=GetProcAddress(crt,"_errno");
    check(split&&scale&&fraction&&error,"exports");
    if(failures)ExitProcess(1);
    const U64 values[]={0,0x8000000000000000ULL,1,0x8000000000000001ULL,
        0x0010000000000000ULL,0x3ff0000000000000ULL,0xbffc000000000000ULL,
        0x7fefffffffffffffULL};
    const int exponents[]={0,0,-1073,-1073,-1021,1,1,1024};
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++){
        FP value={.bits=values[i]};int exponent=12345;
        double mantissa=split(value.value,&exponent);
        check(exponent==exponents[i],"frexp-exponent");
        check(bits(scale(mantissa,exponent))==(i==1?0:value.bits),"frexp-ldexp-roundtrip");
    }
    double integer=99;
    check(fraction(-14.875,&integer)==-0.875&&integer==-14,"modf-negative");
    check(bits(fraction(-2,&integer))==(sizeof(void*)==4?0x8000000000000000ULL:0)&&integer==-2,"modf-negative-integral");
    check(fraction(-0.25,&integer)==-0.25&&bits(integer)==0x8000000000000000ULL,"modf-negative-zero-integer");
    FP infinity={.bits=0xfff0000000000000ULL},qnan={.bits=0x7ff8000000000001ULL};
    check(bits(fraction(infinity.value,&integer))==0x8000000000000000ULL&&bits(integer)==infinity.bits,"modf-infinity");
    check(nan(fraction(qnan.value,&integer))&&nan(integer),"modf-nan");
    *error()=0;check(bits(scale(1,1024))==0x7ff0000000000000ULL&&*error()==34,"ldexp-overflow-errno");
    check(bits(scale(1,-1074))==1,"ldexp-smallest-subnormal");
    check(scale(-1,3)==-8&&scale(16,-3)==2,"ldexp-signed-exponent");
    check(bits(scale(-0.0,2000))==0x8000000000000000ULL,"ldexp-negative-zero");
    check(nan(scale(qnan.value,1)),"ldexp-nan");
    int nonfinite_exponent=123;
    check(nan(split(infinity.value,&nonfinite_exponent))&&nonfinite_exponent==-1,"frexp-infinity-legacy-contract");
#if defined(__i386__)
    const char *command=GetCommandLineA();int legacy_reference=0;
    for(const char *p=command;*p;p++)if(p[0]=='l'&&p[1]=='e'&&p[2]=='g'&&p[3]=='a'&&p[4]=='c'&&p[5]=='y')legacy_reference=1;
    const struct{const char *name;double x,y,expected;int binary;}cases[]={
        {"_CIasin",0.5,0,0.5235987755982989,0},
        {"_CIasin",1,0,1.5707963267948966,0},
        {"_CIatan",1,0,0.7853981633974483,0},
        {"_CIatan2",1,2,0.4636476090008061,1},
        {"_CIatan2",-1,-2,-2.677945044588987,1},
        {"_CIcos",1,0,0.5403023058681398,0},
        {"_CIcos",1e100,0,0.9247242387519338,0},
        {"_CIcosh",1,0,1.5430806348152437,0},
        {"_CIlog",2,0,0.6931471805599453,0},
        {"_CIsin",1,0,0.8414709848078965,0},
        {"_CIsin",1e100,0,-0.3806377310050287,0},
        {"_CIsinh",-1,0,-1.1752011936438014,0},
        {"_CIsinh",1e-18,0,1e-18,0},
        {"_CItan",1,0,1.5574077246549023,0},
        {"_CItan",1e100,0,-0.4116229628832498,0},
        {"_CItanh",-1,0,-0.7615941559557649,0},
        {"_CItanh",1000,0,1,0}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
        void *entry=GetProcAddress(crt,cases[i].name);
        check(entry!=0,cases[i].name);
        if(entry){
            double actual=intrinsic(entry,cases[i].x,cases[i].y,cases[i].binary);
            if(legacy_reference&&cases[i].x==1e100){
                /* This legacy CRT's argument reduction loses accuracy here.
                 * QEMU must match the full-range reference values above. */
                say("MATH INFO legacy-large-angle ");say(cases[i].name);say(" ");show_bits(actual);
            }else check(near(actual,cases[i].expected),cases[i].name);
        }
    }
    void *asin_entry=GetProcAddress(crt,"_CIasin");
    if(asin_entry)check(nan(intrinsic(asin_entry,2,0,0)),"asin-domain-nan");
    void *log_entry=GetProcAddress(crt,"_CIlog");
    if(log_entry)check(bits(intrinsic(log_entry,0,0,0))==0xfff0000000000000ULL,"log-zero-negative-infinity");
#endif
    if(!failures)say("MATH DONE\n");
    ExitProcess(failures?1:0);
}
