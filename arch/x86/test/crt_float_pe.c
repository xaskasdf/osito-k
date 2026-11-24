/* Native/guest CRT oracle. Only the host's ordinary msvcrt API is used. */
typedef unsigned int DWORD;
typedef unsigned long long U64;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
int _fltused;
typedef union { float value; DWORD bits; } FP32;
typedef union { double value; U64 bits; } FP64;
static DWORD failures;
#define SAY(s) do { DWORD w_; WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&w_,0); } while (0)
#define HEX(v) do { DWORD v_=(DWORD)(v),w_; char b_[9]; \
    for (int j_=7;j_>=0;j_--) { b_[j_]="0123456789abcdef"[v_&15];v_>>=4; } \
    b_[8]=' '; WriteFile(GetStdHandle((DWORD)-11),b_,sizeof(b_),&w_,0); } while (0)
static void float_crt_check(int ok)
{
    if (ok) SAY("CRTFLOAT PASS\n");
    else { failures++; SAY("CRTFLOAT FAIL\n"); }
}
static float float_crt_call(void *entry, float x, float y, int binary)
{
#if __SIZEOF_POINTER__ == 4
    const double sentinel=123.25;
    double remaining;
    float result;
    unsigned short before,after,cw_before,cw_after;
    __asm__ volatile("fnstsw %0; fnstcw %1":"=m"(before),"=m"(cw_before));
    if (binary) {
        __asm__ volatile("fldl %[sentinel]; flds %[x]; flds %[y]; subl $8,%%esp; "
            "fstps 4(%%esp); fstps (%%esp); call *%[fn]; addl $8,%%esp; "
            "fstps %[result]; fstpl %[remaining]"
            :[result]"=m"(result),[remaining]"=m"(remaining)
            :[fn]"r"(entry),[x]"m"(x),[y]"m"(y),[sentinel]"m"(sentinel)
            :"eax","ecx","edx","st","st(1)","st(2)","st(3)","st(4)","st(5)","st(6)","st(7)",
             "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","cc","memory");
    } else {
        __asm__ volatile("fldl %[sentinel]; flds %[x]; subl $4,%%esp; fstps (%%esp); "
            "call *%[fn]; addl $4,%%esp; fstps %[result]; fstpl %[remaining]"
            :[result]"=m"(result),[remaining]"=m"(remaining)
            :[fn]"r"(entry),[x]"m"(x),[sentinel]"m"(sentinel)
            :"eax","ecx","edx","st","st(1)","st(2)","st(3)","st(4)","st(5)","st(6)","st(7)",
             "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","cc","memory");
    }
    __asm__ volatile("fnstsw %0; fnstcw %1":"=m"(after),"=m"(cw_after));
    float_crt_check(remaining==sentinel && (before&0x3800)==(after&0x3800));
    float_crt_check(cw_before==cw_after);
    return result;
#else
    return binary ? ((float (*)(float,float))entry)(x,y) : ((float (*)(float))entry)(x);
#endif
}
void float_crt_entry(void)
{
    void *crt=LoadLibraryA("msvcrt.dll");
    int *(*error)(void)=crt?GetProcAddress(crt,"_errno"):0;
    float_crt_check(error!=0); if (!error) ExitProcess(1);
    const char *names[]={"sqrtf","sinf","cosf","asinf","acosf","logf","floorf","ceilf",
        "powf","atan2f","fmodf","modff"};
    const DWORD arguments[]={0,0x80000000,0x3f000000,0xbf000000,0x3f800000,0xbf800000,
        0x40000000,0xc0000000,0x40490fdb,0x60ad78ec,0x7f7fffff,1,0x80000001,
        0x7f800000,0xff800000,0x7fc00042};
    const float pairs[][2]={{2,3},{-2,3},{-2,4},{-2,0.5f},{0,0},{0,-2},
        {-0.0f,3},{-0.0f,-3},{2,128},{2,-149},{2,-150},{0.5f,149},{10,2.25f},
        {1,2},{1,-2},{-1,-2},{-0.0f,-0.0f},{3.5f,2},{-3.5f,2}};
    for (DWORD function=0;function<12;function++) {
        void *entry=GetProcAddress(crt,names[function]);
        SAY("CRTFLOAT EXPORT ");HEX(function);HEX(entry!=0);SAY("\n");
        if (!entry) continue;
        DWORD count=(function>=8 && function<=10) ? sizeof(pairs)/sizeof(pairs[0]) : sizeof(arguments)/sizeof(arguments[0]);
        for (DWORD i=0;i<count;i++) {
            FP32 x={.bits=arguments[i%16]},result;
            struct { DWORD before; float value; DWORD after; } integral={0x2468ace0,99,0x13579bdf};
            *error()=123;
            if (function==11) result.value=((float (*)(float,float *))entry)(x.value,&integral.value);
            else if (function>=8) result.value=float_crt_call(entry,pairs[i][0],pairs[i][1],1);
            else result.value=float_crt_call(entry,x.value,0,0);
            int last=*error();
            FP32 whole={.value=integral.value};
            SAY("CRTFLOAT ROW ");HEX(function);HEX(i);HEX(result.bits);HEX(last);HEX(whole.bits);SAY("\n");
            if (function==11) float_crt_check(integral.before==0x2468ace0 && integral.after==0x13579bdf);
        }
    }
    const char *double_names[]={"log","exp","asin","atan","atan2","fmod"};
    const U64 doubles[]={0,0x8000000000000000ULL,0x3fe0000000000000ULL,0xbfe0000000000000ULL,
        0x3ff0000000000000ULL,0xbff0000000000000ULL,0x4000000000000000ULL,0xc000000000000000ULL,
        0x400921fb54442d18ULL,0x4415af1d78b58c40ULL,0x7fefffffffffffffULL,1,0x8000000000000001ULL,
        0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000042ULL};
    for (DWORD function=0;function<6;function++) {
        void *entry=GetProcAddress(crt,double_names[function]);
        SAY("CRTDOUBLE EXPORT ");HEX(function);HEX(entry!=0);SAY("\n");
        if (!entry) continue;
        DWORD count=function<4?16:sizeof(pairs)/sizeof(pairs[0]);
        for (DWORD i=0;i<count;i++) {
            FP64 x={.bits=doubles[i%16]},result;
            *error()=123;
            result.value=function<4?((double (*)(double))entry)(x.value)
                :((double (*)(double,double))entry)(pairs[i][0],pairs[i][1]);
            int last=*error();
            SAY("CRTDOUBLE ROW ");HEX(function);HEX(i);HEX(result.bits>>32);HEX(result.bits);HEX(last);SAY("\n");
        }
    }
    if (!failures) SAY("CRTFLOAT DONE\n");
    ExitProcess(failures?1:0);
}
