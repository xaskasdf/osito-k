#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <mpfr.h>
#include "../dos/dos_fpu_softfloat.h"

unsigned cpu_xsave_active;
extern const uint8_t dos_fpu_register_ops[];
extern uint64_t dos_fpu_host_step(void *, const uint8_t *, void *, uint32_t);
struct extended { uint64_t sig; uint16_t exp; };
static const struct extended edges[] = {
    {0,0}, {1,0}, {2,0}, {3,0}, {4,0}, {0x4000000000000000ull,0},
    {0x7fffffffffffffffull,0}, {0x8000000000000000ull,0},
    {0x8000000000000000ull,1}, {0x8000000000000001ull,1},
    {0xffffffffffffffffull,1}, {0x8000000000000000ull,2},
    {0x8000000000000000ull,0x3ffe}, {0x8000000000000000ull,0x3fff},
    {0x8000000000000001ull,0x3fff}, {0xc000000000000000ull,0x3fff},
    {0x8000000000000000ull,0x4000}, {0xc000000000000000ull,0x4000},
    {0xa000000000000000ull,0x4001}, {0xe000000000000000ull,0x4001},
    {0x8000000000000123ull,0x403e}, {0x8000000000000123ull,0x403f},
    {0x8000000000000123ull,0x4040}, {0xffffffffffffffffull,0x407f},
    {0xffffffffffffffffull,0x7ffe}, {0x8000000000000000ull,0x7fff},
    {0xa000000000000123ull,0x7fff}, {0xc000000000000123ull,0x7fff},
    {0xd000000000000456ull,0x7fff}, {0x4000000000000000ull,0x3fff},
    {1,0x7fff}, {0,1}
};
static uint64_t checks, failures, evaluations, native_checks, partial_steps, loops;
static unsigned input_id, current_op, current_cw;
static struct extended current_a, current_b;

static void check(bool condition, const char *what)
{
    checks++;
    if (condition) return;
    failures++;
    static unsigned last = UINT32_MAX, reports;
    if (last != input_id && reports++ < 80) {
        last = input_id;
        printf("FAIL input=%u op=d9%02x cw=%04x a=%04x:%016" PRIx64
            " b=%04x:%016" PRIx64 " %s\n",input_id,current_op,current_cw,
            current_a.exp,current_a.sig,current_b.exp,current_b.sig,what);
    }
}

static unsigned exponent(struct extended v) { return v.exp & 0x7fff; }
static bool negative(struct extended v) { return v.exp >> 15; }
static bool zero(struct extended v) { return !exponent(v) && !v.sig; }
static bool unsupported(struct extended v) { return exponent(v) && !(v.sig >> 63); }
static bool nan_value(struct extended v) { return exponent(v) == 0x7fff && (v.sig << 1); }
static bool infinite(struct extended v) { return exponent(v) == 0x7fff && v.sig == (1ull << 63); }
static bool denormal(struct extended v) { return !exponent(v) && v.sig; }
static bool same(struct extended a, struct extended b) { return a.sig == b.sig && a.exp == b.exp; }
static void put(uint8_t *p, uint64_t value, unsigned bytes)
{ for (unsigned i=0;i<bytes;i++) p[i]=value>>(8*i); }
static uint64_t get(const uint8_t *p, unsigned bytes)
{ uint64_t v=0; for(unsigned i=0;i<bytes;i++) v|=(uint64_t)p[i]<<(8*i); return v; }
static void store_value(uint8_t *p, struct extended v)
{ put(p,v.sig,8); put(p+8,v.exp,2); }
static struct extended decode(const uint8_t *p)
{ return (struct extended){get(p,8),get(p+8,2)}; }

static void set_extended(mpfr_t x, struct extended v)
{
    if (infinite(v)) { mpfr_set_inf(x, negative(v) ? -1 : 1); return; }
    if (zero(v)) { mpfr_set_zero(x, negative(v) ? -1 : 1); return; }
    mpfr_set_uj(x,v.sig,MPFR_RNDN);
    mpfr_mul_2si(x,x,(long)(exponent(v) ? exponent(v) : 1)-16446,MPFR_RNDN);
    if (negative(v)) mpfr_neg(x,x,MPFR_RNDN);
}

static struct extended encode(mpfr_srcptr x)
{
    struct extended v={0,mpfr_signbit(x) ? 0x8000u : 0};
    if (mpfr_inf_p(x)) { v.exp|=0x7fff; v.sig=1ull<<63; return v; }
    if (mpfr_zero_p(x)) return v;
    long exp=mpfr_get_exp(x);
    mpfr_t scaled; mpfr_init2(scaled,64);
    mpfr_abs(scaled,x,MPFR_RNDN);
    mpfr_mul_2si(scaled,scaled,exp < -16381 ? 16445 : 64-exp,MPFR_RNDN);
    v.sig=mpfr_get_uj(scaled,MPFR_RNDN);
    v.exp|=exp < -16381 ? 0 : exp+16382;
    mpfr_clear(scaled);
    return v;
}

struct candidate { struct extended value, biased; bool tiny; };
struct reference {
    struct candidate candidates[32];
    struct extended pushed;
    unsigned count, flags, conditions;
    bool partial, exceptional;
};

static struct candidate candidate(mpfr_srcptr x)
{
    struct candidate r={.value=encode(x)};
    r.tiny=!mpfr_zero_p(x) && !mpfr_inf_p(x) && mpfr_get_exp(x) < -16381;
    if (r.tiny) {
        mpfr_t biased; mpfr_init2(biased,128);
        mpfr_mul_2si(biased,x,24576,MPFR_RNDN);
        r.biased=encode(biased);
        mpfr_clear(biased);
    }
    return r;
}

static unsigned quotient_flags(long q)
{
    unsigned long magnitude=q < 0 ? 0ul-(unsigned long)q : (unsigned long)q;
    return ((magnitude&1)<<9)|((magnitude&2)<<13)|((magnitude&4)<<6);
}

static struct reference reference(unsigned op, struct extended a, struct extended b)
{
    bool extract=op==0xf4;
    if (extract) b=(struct extended){0,0};
    struct reference r={.count=1};
    struct extended special={0xc000000000000000ull,0xffff};
    bool na=nan_value(a), nb=nan_value(b);
    if (unsupported(a)||unsupported(b)) { r.flags=1; r.exceptional=true; }
    else if (na||nb) {
        bool sa=na && !(a.sig&(1ull<<62)), sb=nb && !(b.sig&(1ull<<62));
        r.flags=sa||sb ? 1 : 0;
        if (!na) special=b;
        else if (!nb) special=a;
        else if (sa!=sb) special=sa ? b : a;
        else special=a.sig>b.sig || (a.sig==b.sig && a.exp<b.exp) ? a : b;
        special.sig|=1ull<<62;
        r.exceptional=true;
    } else if (!extract && (infinite(a)||zero(b))) { r.flags=1; r.exceptional=true; }
    if (r.exceptional) {
        r.candidates[0].value=r.pushed=special;
        return r;
    }
    r.flags=denormal(a)||denormal(b) ? 2 : 0;
    mpfr_t x,y,z,modulus;
    mpfr_inits2(128,x,y,z,modulus,(mpfr_ptr)0);
    set_extended(x,a); set_extended(y,b);
    if (extract) {
        if (zero(a)) { r.flags=4; mpfr_set_inf(z,-1); r.pushed=a; }
        else if (infinite(a)) { mpfr_set_inf(z,1); r.pushed=a; }
        else {
            mpfr_exp_t e;
            mpfr_frexp(&e,z,x,MPFR_RNDN);
            mpfr_mul_2ui(z,z,1,MPFR_RNDN);
            r.pushed=encode(z);
            mpfr_set_si(z,e-1,MPFR_RNDN);
        }
        r.candidates[0]=candidate(z);
    } else if (zero(a)||infinite(b)) r.candidates[0]=candidate(x);
    else {
        long difference=mpfr_get_exp(x)-mpfr_get_exp(y), quotient;
        r.partial=difference>=64;
        if (r.partial) {
            r.count=32; r.conditions=0x400;
            /* Accept any SDM-permitted reduction width, not the production
             * helper's particular choice of N. Each candidate is exact. */
            for (unsigned n=32;n<=63;n++) {
                mpfr_mul_2si(modulus,y,difference-n,MPFR_RNDN);
                mpfr_fmod(z,x,modulus,MPFR_RNDN);
                r.candidates[n-32]=candidate(z);
            }
        } else {
            if (op==0xf5) mpfr_remquo(z,&quotient,x,y,MPFR_RNDN);
            else mpfr_fmodquo(z,&quotient,x,y,MPFR_RNDN);
            r.conditions=quotient_flags(quotient);
            r.candidates[0]=candidate(z);
        }
    }
    mpfr_clears(x,y,z,modulus,(mpfr_ptr)0);
    return r;
}

static void test_pair(unsigned op, struct extended a, struct extended b, bool exhaustive)
{
    current_op=op; current_a=a; current_b=b;
    struct reference expected=reference(op,a,b);
    const unsigned pcs[]={0,2,3};
    for(unsigned mask=0;mask<64;mask++) {
        if (!exhaustive && mask!=63 && mask!=61 && mask!=47 && mask!=0) continue;
        dos_fpu_numeric_result_t baseline={0};
        for(unsigned rc=0;rc<4;rc++) for(unsigned pc=0;pc<3;pc++) {
            current_cw=0x40|mask|(pcs[pc]<<8)|(rc<<10);
            _Alignas(64) uint8_t image[1024]={0};
            uint8_t operand[108]={0}, before[160];
            put(image,current_cw,2); put(image+2,0x4700,2); image[4]=0x7f;
            put(image+24,0x1f80,4);
            store_value(image+32,a); store_value(image+48,b);
            memcpy(before,image,sizeof(before));
            dos_fpu_numeric_result_t got;
            bool accepted=dos_fpu_soft_evaluate(image,operand,0xd9,op,&got);
            check(accepted,"accepted");
            if (!accepted) return;
            evaluations++;
            check(!memcmp(before,image,sizeof(before)),"immutable input");
            check(got.write && !got.pops && !got.destination && got.push==(op==0xf4),"write/push/destination");
            bool inhibited=(expected.flags&~mask&7)!=0;
            unsigned base_flags=(expected.flags&2) && !(mask&2) ? 2 : expected.flags;
            bool match=false;
            for(unsigned i=0;i<expected.count;i++) {
                bool bias=op!=0xf4 && expected.candidates[i].tiny && !(mask&16);
                unsigned flags=base_flags|(!inhibited && bias ? 16 : 0);
                struct extended value=bias ? expected.candidates[i].biased : expected.candidates[i].value;
                if (got.exceptions==flags && (inhibited || same(decode(got.value),value))) match=true;
            }
            check(match,"exact value and mask-aware exceptions");
            if (op==0xf4) {
                check(got.condition_clear==0x200 && !got.conditions,"extract undefined flags policy");
                check(inhibited || same(decode(got.pushed_value),expected.pushed),"extracted significand");
            } else {
                unsigned clear=expected.exceptional||inhibited ? 0x600 : 0x4700;
                check(got.condition_clear==clear && got.conditions==
                    (expected.exceptional||inhibited ? 0 : expected.conditions),"quotient bits and C2");
            }
            if (pc||rc) check(got.exceptions==baseline.exceptions && got.conditions==baseline.conditions &&
                got.condition_clear==baseline.condition_clear && !memcmp(got.value,baseline.value,10) &&
                !memcmp(got.pushed_value,baseline.pushed_value,10),"PC and RC independent");
            else baseline=got;
            /* Partial reductions are implementation-dependent. Compare the
             * completed operation and defined flags with the native CPU. */
            if (!expected.partial) {
                dos_fpu_host_step(image,dos_fpu_register_ops+(64+op-0xc0)*8,operand,0);
                unsigned status=get(image+2,2);
                check((status&0x3f)==got.exceptions,"native exceptions");
                if (!inhibited) {
                    check(same(decode(image+(op==0xf4 ? 48 : 32)),decode(got.value)),"native result");
                    if (op==0xf4) check(same(decode(image+32),decode(got.pushed_value)),"native pushed result");
                    else if (!expected.exceptional) check((status&0x4700)==got.conditions,"native quotient bits");
                }
                native_checks++;
            }
        }
    }
    input_id++;
}

static void test_loop(unsigned op, struct extended a, struct extended b)
{
    current_op=op; current_cw=0x37f; current_a=a; current_b=b;
    mpfr_t x,y,answer,residue,actual,next;
    mpfr_inits2(128,x,y,answer,residue,actual,next,(mpfr_ptr)0);
    set_extended(x,a); set_extended(y,b);
    long quotient;
    if (op==0xf5) mpfr_remquo(answer,&quotient,x,y,MPFR_RNDN);
    else mpfr_fmodquo(answer,&quotient,x,y,MPFR_RNDN);
    mpfr_fmod(residue,x,y,MPFR_RNDN);
    uint8_t image[160]={0},operand[16]={0};
    put(image,current_cw,2); store_value(image+32,a); store_value(image+48,b);
    bool complete=false;
    for(unsigned step=0;step<1030;step++) {
        dos_fpu_numeric_result_t got;
        bool accepted=dos_fpu_soft_evaluate(image,operand,0xd9,op,&got);
        check(accepted,"loop accepted");
        if (!accepted) break;
        check(!(got.exceptions&~2u),"loop exact, no precision or masked underflow");
        struct extended value=decode(got.value);
        if (!(got.conditions&0x400)) {
            check(same(value,encode(answer)),"complete iterative remainder");
            check(got.conditions==quotient_flags(quotient),"complete iterative quotient");
            complete=true;
            break;
        }
        set_extended(actual,value);
        mpfr_fmod(next,actual,y,MPFR_RNDN);
        check(mpfr_equal_p(next,residue),"partial congruence");
        check(mpfr_zero_p(actual)||mpfr_get_exp(actual)<=mpfr_get_exp(x)-32,"partial exponent progress");
        check(negative(value)==negative(a),"partial dividend sign");
        store_value(image+32,value);
        mpfr_set(x,actual,MPFR_RNDN);
        partial_steps++;
    }
    check(complete,"bounded convergence over full extended exponent range");
    mpfr_clears(x,y,answer,residue,actual,next,(mpfr_ptr)0);
    loops++; input_id++;
}

int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    printf("REDUCE MPFR version=%s seed=243f6a8885a308d3\n",mpfr_get_version());
    const unsigned ops[]={0xf4,0xf5,0xf8};
    unsigned count=sizeof(edges)/sizeof(edges[0]);
    for(unsigned o=0;o<3;o++) {
        for(unsigned a=0;a<count;a++) for(unsigned b=0;b<(o ? count : 1);b++)
        for(unsigned signs=0;signs<4;signs++) {
            struct extended x=edges[a],y=o ? edges[b] : (struct extended){0xa000000000000123ull,0x7fff};
            x.exp^=(signs&1)<<15; y.exp^=(signs&2)<<14;
            test_pair(ops[o],x,y,true);
        }
        printf("REDUCE class op=d9%02x inputs=%u failures=%" PRIu64 "\n",ops[o],input_id,failures);
    }
    /* Consecutive integer half-way quotients cover all eight quotient flags. */
    mpfr_t x; mpfr_init2(x,64);
    for(unsigned n=1;n<=65;n++) for(unsigned neighbor=0;neighbor<3;neighbor++) {
        mpfr_set_ui(x,n,MPFR_RNDN);
        if (!neighbor) mpfr_nextbelow(x);
        if (neighbor==2) mpfr_nextabove(x);
        for(unsigned sign=0;sign<4;sign++) {
            struct extended a=encode(x),b={1ull<<63,0x4000};
            a.exp^=(sign&1)<<15; b.exp^=(sign&2)<<14;
            test_pair(0xf5,a,b,false); test_pair(0xf8,a,b,false);
        }
    }
    mpfr_clear(x);
    uint64_t state=0x243f6a8885a308d3ull;
    for(unsigned i=0;i<1024;i++) {
        state^=state<<13; state^=state>>7; state^=state<<17;
        struct extended a={state|(1ull<<63),(1+state%0x7ffe)|((i&1)<<15)};
        struct extended b={(state*0xd1342543de82ef95ull)|(1ull<<63),
            (1+(state>>16)%0x7ffe)|((i&2)<<14)};
        test_pair(0xf4,a,b,false);
        test_pair(0xf5,a,b,false); test_pair(0xf8,a,b,false);
        if (exponent(a)>exponent(b)+64) { test_loop(0xf5,a,b); test_loop(0xf8,a,b); }
    }
    mpfr_free_cache();
    printf("REDUCE MPFR inputs=%u evaluations=%" PRIu64 " checks=%" PRIu64
        " native=%" PRIu64 " loops=%" PRIu64 " partial-steps=%" PRIu64 " failures=%" PRIu64 "\n",
        input_id,evaluations,checks,native_checks,loops,partial_steps,failures);
    return failures!=0;
}
