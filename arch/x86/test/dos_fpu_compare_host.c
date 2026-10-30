#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <mpfr.h>
#include "../dos/dos_fpu_softfloat.h"

unsigned cpu_xsave_active;
extern const uint8_t dos_fpu_register_ops[], dos_fpu_memory_ops[];
extern uint64_t dos_fpu_host_step(void *, const uint8_t *, void *, uint32_t);
struct extended { uint64_t sig; uint16_t exp; };
static const struct extended values[] = {
    {0,0},{1,0},{0x4000000000000000ull,0},{0x7fffffffffffffffull,0},
    {0x8000000000000000ull,0},{0x8000000000000001ull,0},{UINT64_MAX,0},
    {0x8000000000000000ull,1},{0x8000000000000001ull,1},
    {0x8000000000000000ull,0x3ffe},{UINT64_MAX,0x3ffe},
    {0x8000000000000000ull,0x3fff},{0x8000000000000001ull,0x3fff},
    {0xc000000000000000ull,0x3fff},{0x8000000000000000ull,0x4000},
    {UINT64_MAX,0x7ffe},{0x8000000000000000ull,0x7fff},
    {0xa000000000000123ull,0x7fff},{0xc000000000000123ull,0x7fff},
    {0,1},{0x4000000000000000ull,0x3fff},{0,0x7fff},
    {1,0x7fff},{0x4000000000000000ull,0x7fff}
};
static const uint32_t singles[] = {
    0,1,0x400000,0x7fffff,0x800000,0x800001,0xffffff,0x3f000000,
    0x3f7fffff,0x3f800000,0x3f800001,0x3fc00000,0x40000000,0x7f7fffff,
    0x7f800000,0x7fa00123,0x7fc00123,0x7fffffff,0x3a000000,0x47800000,
    0x4effffff,0x4f000000,0x01000000,0x3eaaaaab
};
static const uint64_t doubles[] = {
    0,1,0x8000000000000ull,0xfffffffffffffull,0x10000000000000ull,
    0x10000000000001ull,0x1fffffffffffffull,0x3fe0000000000000ull,
    0x3fefffffffffffffull,0x3ff0000000000000ull,0x3ff0000000000001ull,
    0x3ff8000000000000ull,0x4000000000000000ull,0x7fefffffffffffffull,
    0x7ff0000000000000ull,0x7ff4000000000123ull,0x7ff8000000000123ull,
    0x7fffffffffffffffull,0x001fffffffffffffull,0x40f0000000000000ull,
    0x41dfffffffc00000ull,0x41e0000000000000ull,0x20000000000000ull,0x3fd5555555555555ull
};
static const uint32_t integers[] = {
    0,1,2,3,7,16,127,128,255,256,32767,32768,65535,65536,
    0x7fffffff,0x80000000,0xffffffff,0xfffffffe,0xffff7fff,0xffff8000,
    0xffff0000,0x10001,0x40000000,0xc0000000
};
enum { REG, SINGLE, DOUBLE, INT16, INT32 };
struct form { unsigned op, mr, memory, index, pops; bool quiet, flags, test, examine; };
static struct form forms[76];
static unsigned form_count, current_form, current_cw, current_input;
static uint64_t checks, failures, evaluations, native_checks, native_legacy_invalid, native_integer_c1;
static uint64_t random_state=0x13198a2e03707344ull;

static uint64_t random_bits(void)
{
    random_state^=random_state<<13;
    random_state^=random_state>>7;
    random_state^=random_state<<17;
    return random_state;
}

static void check(bool condition, const char *label)
{
    checks++;
    if (condition) return;
    failures++;
    if (failures <= 30) printf("FAIL form=%u op=%02x%02x input=%u cw=%04x %s\n",
        current_form,forms[current_form].op,forms[current_form].mr,current_input,current_cw,label);
}
static unsigned get16(const uint8_t *p) { return p[0]|(unsigned)p[1]<<8; }
static void put(uint8_t *p,uint64_t v,unsigned bytes)
{ for(unsigned i=0;i<bytes;i++) p[i]=v>>(8*i); }
static void store_value(uint8_t *p,struct extended v)
{ put(p,v.sig,8); put(p+8,v.exp,2); }
static unsigned exponent(struct extended a) { return a.exp&0x7fff; }
static bool unsupported(struct extended a) { return exponent(a) && !(a.sig>>63); }
static bool nan_value(struct extended a) { return exponent(a)==0x7fff && (a.sig<<1); }
static bool snan(struct extended a) { return nan_value(a) && !(a.sig&(1ull<<62)); }
static bool denormal(struct extended a) { return !exponent(a) && a.sig; }

static void set_extended(mpfr_t x,struct extended a)
{
    bool negative=a.exp>>15;
    if (exponent(a)==0x7fff) { mpfr_set_inf(x,negative ? -1 : 1); return; }
    mpfr_set_uj(x,a.sig,MPFR_RNDN);
    mpfr_mul_2si(x,x,(long)(exponent(a) ? exponent(a) : 1)-16446,MPFR_RNDN);
    if (negative) mpfr_neg(x,x,MPFR_RNDN);
}

static struct extended from_binary(uint64_t bits,unsigned width)
{
    unsigned fraction=width==32 ? 23 : 52, bias=width==32 ? 127 : 1023;
    unsigned sign=bits>>(width-1), exp=(bits>>fraction)&(2*bias+1);
    uint64_t sig=bits&((1ull<<fraction)-1);
    if (exp==2*bias+1) return (struct extended){(1ull<<63)|(sig<<(63-fraction)),0x7fff|(sign<<15)};
    if (!exp && !sig) return (struct extended){0,sign<<15};
    sig=(sig|(exp ? 1ull<<fraction : 0))<<(63-fraction);
    int extended_exp=(exp ? exp : 1)+16383-bias;
    while (!(sig>>63)) { sig<<=1; extended_exp--; }
    return (struct extended){sig,extended_exp|(sign<<15)};
}

static unsigned classify(struct extended a,bool empty)
{
    unsigned condition;
    if (empty) condition=0x4100;
    else if (unsupported(a)) condition=0;
    else if (exponent(a)==0x7fff) condition=nan_value(a) ? 0x100 : 0x500;
    else if (!exponent(a)) condition=a.sig ? 0x4400 : 0x4000;
    else condition=0x400;
    return condition|((a.exp&0x8000)>>6);
}

static void test_pair(const struct form *f,struct extended a,struct extended b,uint64_t memory,
                      bool exhaustive,unsigned top,bool empty)
{
    unsigned exceptions=0,conditions=0;
    mpfr_t x,y; mpfr_inits2(64,x,y,(mpfr_ptr)0);
    if (f->memory==SINGLE || f->memory==DOUBLE) {
        unsigned width=f->memory==SINGLE ? 32 : 64, fraction=width==32 ? 23 : 52;
        unsigned exponent_mask=width==32 ? 255 : 2047;
        bool tiny=!((memory>>fraction)&exponent_mask) && (memory&((1ull<<fraction)-1));
        b=from_binary(memory,width);
        exceptions=tiny ? 2 : 0;
    } else if (f->memory==INT16 || f->memory==INT32) {
        int64_t integer=f->memory==INT16 ? (int16_t)memory : (int32_t)memory;
        mpfr_set_sj(y,integer,MPFR_RNDN);
        b=(struct extended){0,0};
    } else if (f->test) b=(struct extended){0,0};
    else if (!f->index && !f->examine) b=a;
    if (f->examine) conditions=classify(a,empty);
    else if (unsupported(a)||unsupported(b)||nan_value(a)||nan_value(b)) {
        exceptions=unsupported(a)||unsupported(b)||snan(a)||snan(b)||!f->quiet ? 1 : 0;
        conditions=0x4500;
    } else {
        if (denormal(a)||denormal(b)) exceptions|=2;
        set_extended(x,a);
        if (f->memory!=INT16 && f->memory!=INT32) set_extended(y,b);
        int relation=mpfr_cmp(x,y);
        conditions=relation<0 ? 0x100 : relation>0 ? 0 : 0x4000;
    }
    mpfr_clears(x,y,(mpfr_ptr)0);
    static const unsigned pcs[]={0,2,3};
    for(unsigned mask=0;mask<64;mask++) {
        if (!exhaustive && mask!=0 && mask!=61 && mask!=62 && mask!=63) continue;
        for(unsigned rc=0;rc<4;rc++)for(unsigned pc=0;pc<3;pc++) {
            current_cw=0x40|mask|(pcs[pc]<<8)|(rc<<10);
            _Alignas(64) uint8_t image[1024]={0};
            uint8_t operand[16]={0},memory_before[16],before[sizeof(image)];
            put(image,current_cw,2);put(image+24,0x1f80,4);
            unsigned initial=((current_input&1)<<8)|((current_input&2)<<9)|((current_input&4)<<12)|0x200;
            put(image+2,initial|(top<<11),2);image[4]=0xff&~(empty ? 1u<<top : 0);
            for(unsigned i=0;i<8;i++) store_value(image+32+16*i,b);
            store_value(image+32,a);put(operand,memory,8);
            memcpy(before,image,sizeof(before));
            memcpy(memory_before,operand,sizeof(memory_before));
            dos_fpu_numeric_result_t got;
            bool accepted=dos_fpu_soft_evaluate(image,operand,f->op,f->mr,&got);
            check(accepted,"accepted");
            if (!accepted) return;
            evaluations++;
            bool invalid=(exceptions&~mask&1)!=0, inhibited=(exceptions&~mask&3)!=0;
            unsigned clear=f->flags || invalid ? 0x200 : 0x4700;
            check(!got.write && !got.push && got.pops==f->pops && !got.destination,"no value writes and exact pop count");
            check(got.exceptions==exceptions,"exception priority");
            check(got.condition_clear==clear && got.conditions==
                (f->flags || invalid ? 0 : conditions),"condition mask and relation");
            check(got.integer_flags==(f->flags ? (conditions>>8)&0x45 : 0),"integer flags result");
            check(!memcmp(before,image,sizeof(before)),"immutable x87 input");
            check(!memcmp(memory_before,operand,sizeof(operand)),"immutable memory input");
            const uint8_t *code=f->memory ? dos_fpu_memory_ops+
                ((f->op-0xd8)*8+(f->mr>>3))*8 : dos_fpu_register_ops+
                ((f->op-0xd8)*64+f->mr-0xc0)*8;
            uint64_t native=dos_fpu_host_step(image,code,operand,0x8d5);
            native_checks++;
            unsigned sw=get16(image+2), expected_sw=(initial&~clear)|got.conditions|exceptions|
                (inhibited ? 0x8080 : 0);
            check((sw&0x80ff)==(expected_sw&0x80ff),"native exceptions and pending state");
            unsigned actual_pops=inhibited ? 0 : f->pops, tags=before[4];
            for(unsigned i=0;i<actual_pops;i++) tags&=~(1u<<((top+i)&7));
            check(image[4]==tags && (sw&0x3800)==((top+actual_pops)&7)<<11,"native pop inhibition and tags");
            for(unsigned physical=0;physical<8;physical++) if (tags&(1u<<physical))
                check(!memcmp(image+32+((physical-top-actual_pops)&7)*16,
                    before+32+((physical-top)&7)*16,10),"native live operands unchanged");
            check(!memcmp(operand,memory_before,16),"native memory unchanged");
            if (f->flags) {
                check(((unsigned)(native>>32)&0x45)==(invalid ? 0x45 : got.integer_flags),"native integer comparison");
                check((sw&0x4500)==(initial&0x4500),"native unrelated x87 flags");
                if (sw&0x200) native_integer_c1++;
            } else if (invalid) {
                /* AMD changes these even when #I is unmasked; Intel's
                 * preserved-flags contract is checked above, independently. */
                if ((sw&0x4700)!=(expected_sw&0x4700)) native_legacy_invalid++;
            } else check((sw&0x4700)==(expected_sw&0x4700),"native classification/relation flags");
        }
    }
    current_input++;
}

int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    printf("COMPARE MPFR version=%s seed=13198a2e03707344\n",mpfr_get_version());
    for(unsigned i=0;i<8;i++) {
        forms[form_count++]=(struct form){0xd8,0xd0+i,REG,i,0,0,0,0,0};
        forms[form_count++]=(struct form){0xd8,0xd8+i,REG,i,1,0,0,0,0};
        forms[form_count++]=(struct form){0xdd,0xe0+i,REG,i,0,1,0,0,0};
        forms[form_count++]=(struct form){0xdd,0xe8+i,REG,i,1,1,0,0,0};
        forms[form_count++]=(struct form){0xdb,0xe8+i,REG,i,0,1,1,0,0};
        forms[form_count++]=(struct form){0xdb,0xf0+i,REG,i,0,0,1,0,0};
        forms[form_count++]=(struct form){0xdf,0xe8+i,REG,i,1,1,1,0,0};
        forms[form_count++]=(struct form){0xdf,0xf0+i,REG,i,1,0,1,0,0};
    }
    forms[form_count++]=(struct form){0xda,0xe9,REG,1,2,1,0,0,0};
    forms[form_count++]=(struct form){0xde,0xd9,REG,1,2,0,0,0,0};
    forms[form_count++]=(struct form){0xd9,0xe4,REG,0,0,0,0,1,0};
    forms[form_count++]=(struct form){0xd9,0xe5,REG,0,0,0,0,0,1};
    for(unsigned pop=0;pop<2;pop++) {
        forms[form_count++]=(struct form){0xd8,0x10+pop*8,SINGLE,0,pop,0,0,0,0};
        forms[form_count++]=(struct form){0xdc,0x10+pop*8,DOUBLE,0,pop,0,0,0,0};
        forms[form_count++]=(struct form){0xda,0x10+pop*8,INT32,0,pop,0,0,0,0};
        forms[form_count++]=(struct form){0xde,0x10+pop*8,INT16,0,pop,0,0,0,0};
    }
    check(form_count==76,"form inventory");
    for(current_form=0;current_form<form_count;current_form++) {
        const struct form *f=&forms[current_form];
        unsigned count=sizeof(values)/sizeof(values[0]);
        for(unsigned ai=0;ai<count;ai++)for(unsigned bi=0;bi<(f->test || f->examine ? 1 : count);bi++)
        for(unsigned sign=0;sign<(f->test || f->examine ? 2 : 4);sign++) {
            struct extended a=values[ai],b=values[bi];
            a.exp^=(sign&1)<<15;b.exp^=(sign&2)<<14;
            uint64_t bits=f->memory==SINGLE ? singles[bi] : f->memory==DOUBLE ? doubles[bi] : integers[bi];
            if (sign&2) {
                if (f->memory==SINGLE) bits^=1ull<<31;
                else if (f->memory==DOUBLE) bits^=1ull<<63;
                else bits=0-bits;
            }
            if (f->examine) {
                for(unsigned top=0;top<8;top++)for(unsigned empty=0;empty<2;empty++)
                    test_pair(f,a,b,bits,true,top,empty);
            } else test_pair(f,a,b,bits,f->index==1 || f->memory || f->test,0,false);
        }
        for(unsigned sample=0;sample<1024;sample++) {
            struct extended a={random_bits(),random_bits()&0xffff};
            struct extended b={random_bits(),random_bits()&0xffff};
            /* Mix raw encodings with close, fully supported finite pairs. */
            if (sample&1) {
                a.sig|=1ull<<63;
                a.exp=(a.exp&0x8000)|((a.exp&0x7fff)%0x7ffe + 1);
                b=a;
                if (sample&2) b.sig^=1;
                if (sample&4) b.exp^=0x8000;
            }
            uint64_t bits=random_bits();
            if (f->memory==SINGLE) bits&=UINT32_MAX;
            test_pair(f,a,b,bits,false,sample&7,f->examine && (sample&8));
        }
        printf("COMPARE form=%u op=%02x%02x evaluations=%" PRIu64 " failures=%" PRIu64 "\n",
            current_form,f->op,f->mr,evaluations,failures);
    }
    mpfr_free_cache();
    printf("COMPARE MPFR forms=%u inputs=%u evaluations=%" PRIu64 " checks=%" PRIu64
        " native=%" PRIu64 " failures=%" PRIu64 "\n",form_count,current_input,evaluations,checks,native_checks,failures);
    printf("NATIVE DIFFERENCES legacy-unmasked-invalid=%" PRIu64 " integer-C1=%" PRIu64 " (observations, not conformance passes)\n",
        native_legacy_invalid,native_integer_c1);
    return failures!=0;
}
