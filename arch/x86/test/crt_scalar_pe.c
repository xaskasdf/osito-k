/* Native/guest scalar CRT oracle, including PE32 stack arguments and ST(0)
 * results while another x87 value is live. Never loads DirectX payloads. */
typedef unsigned int DWORD;
typedef unsigned long long U64;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
int _fltused;
typedef union { double value; U64 bits; } FP;
static unsigned failures;
#define SAY(s) do {DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define HEX(v) do {char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
static U64 scalar_bits(double value) {FP f={.value=value};return f.bits;}
static void scalar_check(int ok)
{if(ok)SAY("SCALAR PASS\n");else{failures++;SAY("SCALAR FAIL\n");}}
static double scalar_call(void *function,double x,double y,int binary)
{
#if __SIZEOF_POINTER__ == 4
    double result,remaining;const double sentinel=123.25;
    unsigned short before,after,control_before,control_after;
    __asm__ volatile("fnstsw %0; fnstcw %1":"=m"(before),"=m"(control_before));
    if(binary){
        __asm__ volatile("fldl %[sentinel]; fldl %[x]; fldl %[y]; subl $16,%%esp; "
                         "fstpl 8(%%esp); fstpl (%%esp); call *%[fn]; addl $16,%%esp; "
                         "fstpl %[result]; fstpl %[remaining]"
            :[result]"=m"(result),[remaining]"=m"(remaining)
            :[fn]"r"(function),[x]"m"(x),[y]"m"(y),[sentinel]"m"(sentinel)
            :"eax","ecx","edx","st","st(1)","st(2)","st(3)","st(4)","st(5)","st(6)","st(7)",
             "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","cc","memory");
    }else{
        __asm__ volatile("fldl %[sentinel]; fldl %[x]; subl $8,%%esp; fstpl (%%esp); "
                         "call *%[fn]; addl $8,%%esp; fstpl %[result]; fstpl %[remaining]"
            :[result]"=m"(result),[remaining]"=m"(remaining)
            :[fn]"r"(function),[x]"m"(x),[sentinel]"m"(sentinel)
            :"eax","ecx","edx","st","st(1)","st(2)","st(3)","st(4)","st(5)","st(6)","st(7)",
             "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","cc","memory");
    }
    __asm__ volatile("fnstsw %0; fnstcw %1":"=m"(after),"=m"(control_after));
    scalar_check(remaining==sentinel&&(before&0x3800)==(after&0x3800));
    scalar_check(control_before==control_after);
    return result;
#else
    return binary?((double (*)(double,double))function)(x,y):((double (*)(double))function)(x);
#endif
}
static void scalar_row(unsigned function,unsigned caseid,double result,int error)
{
    U64 b=scalar_bits(result);
    SAY("SCALAR ROW ");HEX(function);HEX(caseid);HEX(b>>32);HEX(b);HEX(error);SAY("\n");
}
void scalar_crt_entry(void)
{
    void *crt=LoadLibraryA("msvcrt.dll");
    int *(*geterror)(void)=GetProcAddress(crt,"_errno");
    const char *names[]={"cos","sin","acos","log10","pow"};
    void *functions[5];
    for(unsigned i=0;i<5;i++){functions[i]=GetProcAddress(crt,names[i]);scalar_check(functions[i]!=0);}
    scalar_check(geterror!=0);if(failures)ExitProcess(1);
    const U64 arguments[]={0,0x8000000000000000ULL,0x3fe0000000000000ULL,
        0xbfe0000000000000ULL,0x3ff0000000000000ULL,0xbff0000000000000ULL,
        0x4000000000000000ULL,0xc000000000000000ULL,0x400921fb54442d18ULL,
        0x4415af1d78b58c40ULL,0x7fefffffffffffffULL,1,0x8000000000000001ULL,
        0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000042ULL};
    for(unsigned function=0;function<4;function++){
        for(unsigned i=0;i<sizeof(arguments)/sizeof(arguments[0]);i++){
            FP x={.bits=arguments[i]};*geterror()=123;
            double result=scalar_call(functions[function],x.value,0,0);
            int last=*geterror();
            if(function==0&&i==4)scalar_check(result>0.54030230586&&result<0.54030230588);
            if(function==1&&i==4)scalar_check(result>0.84147098480&&result<0.84147098482);
            if(function==2&&i==0)scalar_check(result>1.57079632678&&result<1.57079632680);
            if(function==3&&i==4)scalar_check(result==0);
            scalar_row(function,i,result,last);
        }
    }
    static const double powers[][2]={{2,3},{-2,3},{-2,4},{-2,0.5},{0,0},{0,2},
        {0,-2},{-0.0,3},{-0.0,-3},{2,1024},{2,-1074},{2,-1075},{0.5,1074},
        {10,2.25},{1.0001,20000},{1,-1000000},{-1,9007199254740992.0}};
    for(unsigned i=0;i<sizeof(powers)/sizeof(powers[0]);i++){
        *geterror()=123;double result=scalar_call(functions[4],powers[i][0],powers[i][1],1);
        int last=*geterror();if(!i)scalar_check(result==8);
        scalar_row(4,i,result,last);
    }
    double checksum=0;
    for(unsigned i=0;i<128;i++)checksum+=((double (*)(double))functions[0])(0);
    scalar_check(checksum==128);
    if(!failures)SAY("SCALAR DONE\n");ExitProcess(failures?1:0);
}
