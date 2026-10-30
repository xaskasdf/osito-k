/* Native Windows/Osito CRT oracle for original-runtime imports. PE32's
 * private SSE2 entries take/return doubles in XMM registers, not ST(0). */
typedef unsigned int UINT;
typedef unsigned long long U64;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API void *CALL GetStdHandle(UINT);
API int CALL WriteFile(void *,const void *,UINT,UINT *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
int _fltused;
typedef union {double value;U64 bits;} FP64;
typedef union {float value;UINT bits;} FP32;
static UINT checks,failures;
U64 input[16],output[16];
#define SAY(s) do {UINT n;WriteFile(GetStdHandle((UINT)-11),s,sizeof(s)-1,&n,0);}while(0)
#define HEX(v) do {UINT x=(UINT)(v),n;char h[9];for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((UINT)-11),h,9,&n,0);}while(0)
static void check(int ok,UINT line)
{checks++;if(!ok){failures++;SAY("CRTSSE FAIL ");HEX(line);SAY("\n");}}
#define CHECK(x) check(!!(x),__LINE__)

#if __SIZEOF_POINTER__ == 4
static U64 sse_call(void *entry,U64 x,U64 y)
{
    for(UINT i=0;i<16;i++)input[i]=0x3ff0000000000000ULL+(U64)i*0x10000000000ULL;
    input[0]=x;input[2]=y;
    const double sentinel=123.25;double remaining;
    unsigned short before,after,cw_before,cw_after;UINT mx_before,mx_after;
    __asm__ volatile("fnstsw %0; fnstcw %1; stmxcsr %2":"=m"(before),"=m"(cw_before),"=m"(mx_before));
    __asm__ volatile("fldl %[sentinel]; "
        "movdqu _input,%%xmm0; movdqu _input+16,%%xmm1; movdqu _input+32,%%xmm2; movdqu _input+48,%%xmm3; "
        "movdqu _input+64,%%xmm4; movdqu _input+80,%%xmm5; movdqu _input+96,%%xmm6; movdqu _input+112,%%xmm7; "
        "call *%[fn]; "
        "movdqu %%xmm0,_output; movdqu %%xmm1,_output+16; movdqu %%xmm2,_output+32; movdqu %%xmm3,_output+48; "
        "movdqu %%xmm4,_output+64; movdqu %%xmm5,_output+80; movdqu %%xmm6,_output+96; movdqu %%xmm7,_output+112; "
        "fstpl %[remaining]"
        :[remaining]"=m"(remaining)
        :[fn]"r"(entry),[sentinel]"m"(sentinel)
        :"eax","ecx","edx","st","st(1)","st(2)","st(3)","st(4)","st(5)","st(6)","st(7)",
         "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","cc","memory");
    __asm__ volatile("fnstsw %0; fnstcw %1; stmxcsr %2":"=m"(after),"=m"(cw_after),"=m"(mx_after));
    CHECK(remaining==sentinel && (before&0x3800)==(after&0x3800));
    CHECK(cw_before==cw_after && (mx_before&0xffc0)==(mx_after&0xffc0));
    return output[0];
}
#endif

void crt_sse2_entry(void)
{
    void *crt=LoadLibraryA("msvcrt.dll");CHECK(crt!=0);
    int *(*error)(void)=GetProcAddress(crt,"_errno");CHECK(error!=0);
    if(failures)ExitProcess(1);
#if __SIZEOF_POINTER__ == 4
    const char *names[]={"_libm_sse2_pow_precise","_libm_sse2_sqrt_precise","_libm_sse2_log_precise",
        "_libm_sse2_sin_precise","_libm_sse2_log10_precise","_libm_sse2_cos_precise","_libm_sse2_exp_precise"};
    const U64 arguments[]={0,0x8000000000000000ULL,0x3fe0000000000000ULL,0xbfe0000000000000ULL,
        0x3ff0000000000000ULL,0xbff0000000000000ULL,0x4000000000000000ULL,0xc000000000000000ULL,
        0x400921fb54442d18ULL,0x4415af1d78b58c40ULL,0x7fefffffffffffffULL,1,0x8000000000000001ULL,
        0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000042ULL,
        0x3ff0000000000001ULL,0x3fefffffffffffffULL,0x0010000000000000ULL,0x000fffffffffffffULL,
        0x40862e42fefa39efULL,0xc0874910d52d3051ULL};
    const double pairs[][2]={{2,3},{-2,3},{-2,4},{-2,0.5},{0,0},{0,-2},{-0.0,3},{-0.0,-3},
        {2,1024},{2,-1074},{2,-1075},{0.5,1074},{10,2.25},{1.0001,20000},{-1,9007199254740992.0}};
    for(UINT f=0;f<7;f++) {
        void *entry=GetProcAddress(crt,names[f]);CHECK(entry!=0);if(!entry)continue;
        UINT count=f ? sizeof(arguments)/sizeof(arguments[0]) : sizeof(pairs)/sizeof(pairs[0]);
        for(UINT i=0;i<count;i++) {
            FP64 x={.bits=arguments[i]},y={.bits=0};
            if(!f){x.value=pairs[i][0];y.value=pairs[i][1];}
            *error()=123;U64 bits=sse_call(entry,x.bits,y.bits);int last=*error();
            if(!f && !i)CHECK(bits==0x4020000000000000ULL);
            UINT preserved=0;for(UINT k=1;k<16;k++)if(output[k]==input[k])preserved|=1u<<k;
            SAY("CRTSSE ROW ");HEX(f);HEX(i);HEX(bits>>32);HEX(bits);HEX(last);HEX(preserved);SAY("\n");
        }
        /* An altered caller rounding mode must survive the register bridge. */
        UINT saved;__asm__ volatile("stmxcsr %0":"=m"(saved));
        for(UINT round=0;round<4;round++) {
            UINT mode=(saved&~0x6000u)|(round<<13);__asm__ volatile("ldmxcsr %0"::"m"(mode));
            (void)sse_call(entry,0x4000000000000000ULL,0x4008000000000000ULL);
        }
        __asm__ volatile("ldmxcsr %0"::"m"(saved));
    }
#else
    const char *names[]={"expf","log10f"};
    const UINT arguments[]={0,0x80000000,0x3f000000,0xbf000000,0x3f800000,0xbf800000,
        0x40000000,0xc0000000,0x40490fdb,0x60ad78ec,0x7f7fffff,1,0x80000001,
        0x7f800000,0xff800000,0x7fc00042,0x00800000,0x007fffff,0x42b17218,0xc2cff1b5};
    for(UINT f=0;f<2;f++) {
        float (*entry)(float)=GetProcAddress(crt,names[f]);CHECK(entry!=0);if(!entry)continue;
        for(UINT i=0;i<sizeof(arguments)/sizeof(arguments[0]);i++) {
            FP32 x={.bits=arguments[i]},result;*error()=123;result.value=entry(x.value);int last=*error();
            SAY("CRTSSE ROW ");HEX(f);HEX(i);HEX(0);HEX(result.bits);HEX(last);HEX(0);SAY("\n");
        }
    }
#endif
    SAY("CRTSSE checks=");HEX(checks);SAY(" failures=");HEX(failures);SAY("\n");
    ExitProcess(failures ? 1 : 0);
}
