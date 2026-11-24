#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <mpfr.h>
#include "../dos/dos_fpu_softfloat.h"

unsigned cpu_xsave_active;
extern const uint8_t dos_fpu_memory_ops[];
extern uint64_t dos_fpu_host_step(void *, const uint8_t *, void *, uint32_t);
enum { R32, R64, R80, I16, I32, I64, BCD };
struct extended { uint64_t sig; uint16_t exp; };
struct form { unsigned op, mr, type, bytes; bool load, pop, truncate; };
static const struct form forms[] = {
    {0xd9,0,R32,4,1,0,0},{0xdd,0,R64,8,1,0,0},{0xdb,0x28,R80,10,1,0,0},
    {0xdf,0,I16,2,1,0,0},{0xdb,0,I32,4,1,0,0},{0xdf,0x28,I64,8,1,0,0},
    {0xdf,0x20,BCD,10,1,0,0},
    {0xd9,0x10,R32,4,0,0,0},{0xdd,0x10,R64,8,0,0,0},
    {0xd9,0x18,R32,4,0,1,0},{0xdd,0x18,R64,8,0,1,0},{0xdb,0x38,R80,10,0,1,0},
    {0xdf,0x10,I16,2,0,0,0},{0xdb,0x10,I32,4,0,0,0},
    {0xdf,0x18,I16,2,0,1,0},{0xdb,0x18,I32,4,0,1,0},{0xdf,0x38,I64,8,0,1,0},
    {0xdf,0x08,I16,2,0,1,1},{0xdb,0x08,I32,4,0,1,1},{0xdd,0x08,I64,8,0,1,1},
    {0xdf,0x30,BCD,10,0,1,0}
};
static const struct extended special[] = {
    {0,0},{1,0},{0x4000000000000000ull,0},{0x7fffffffffffffffull,0},
    {1ull<<63,0},{UINT64_MAX,0},{1ull<<63,1},{0x8000000000000001ull,1},
    {1ull<<63,0x3ffe},{UINT64_MAX,0x3ffe},{1ull<<63,0x3fff},
    {0x8000000000000001ull,0x3fff},{0xc000000000000000ull,0x3fff},
    {UINT64_MAX,0x7ffe},{1ull<<63,0x7fff},{0xa000000000000123ull,0x7fff},
    {0xc000000000000123ull,0x7fff},{0,1},{0x4000000000000000ull,0x3fff},
    {0,0x7fff},{1,0x7fff},{0x4000000000000000ull,0x7fff}
};
static const uint64_t memory_values[]={0,1,2,0x7fff,0x8000,0x7fffffff,0x80000000,
    0x7fffffffffffffffull,0x8000000000000000ull,UINT64_MAX,0x7f800000,0x7fc00123,
    0x7fa00123,0x7f7fffff,0x800000,0x7fffff,0x7ff0000000000000ull,0x7ff8000000000123ull,
    0x7ff4000000000123ull,0x7fefffffffffffffull,0x10000000000000ull,0xfffffffffffffull,
    999999999999999999ull,999999999999999998ull};
static uint64_t checks, failures, evaluations, inputs, native_checks;
static unsigned current_form, current_cw, current_top;
static struct extended current_value;
static uint64_t random_state=0xa4093822299f31d0ull;

static uint64_t random_bits(void)
{
    random_state^=random_state<<13; random_state^=random_state>>7; random_state^=random_state<<17;
    return random_state;
}
static void equal(uint64_t got,uint64_t expected,const char *label)
{
    checks++;
    if (got==expected) return;
    failures++;
    if (failures<=40) printf("FAIL form=%u input=%" PRIu64 " cw=%04x top=%u a=%04x:%016" PRIx64
        " got=%" PRIx64 " expected=%" PRIx64 " %s\n",current_form,inputs,current_cw,current_top,
        current_value.exp,current_value.sig,got,expected,label);
}
static void bytes_equal(const uint8_t *a,const uint8_t *b,unsigned bytes,const char *label)
{ equal(memcmp(a,b,bytes)==0,1,label); }
static uint64_t get(const uint8_t *p,unsigned bytes)
{ uint64_t v=0;for(unsigned i=0;i<bytes;i++)v|=(uint64_t)p[i]<<(8*i);return v; }
static void put(uint8_t *p,uint64_t v,unsigned bytes)
{ for(unsigned i=0;i<bytes;i++)p[i]=v>>(8*i); }
static void encode(uint8_t *p,struct extended a)
{ put(p,a.sig,8);put(p+8,a.exp,2); }
static bool unsupported(struct extended a) { return (a.exp&0x7fff) && !(a.sig>>63); }
static bool nan_value(struct extended a) { return (a.exp&0x7fff)==0x7fff && (a.sig<<1); }

static void set_extended(mpfr_t x,struct extended a)
{
    if ((a.exp&0x7fff)==0x7fff) mpfr_set_inf(x,a.exp>>15 ? -1 : 1);
    else {
        mpfr_set_uj(x,a.sig,MPFR_RNDN);
        mpfr_mul_2si(x,x,(long)((a.exp&0x7fff) ? a.exp&0x7fff : 1)-16446,MPFR_RNDN);
        if (a.exp>>15) mpfr_neg(x,x,MPFR_RNDN);
    }
}
static struct extended get_extended(const mpfr_t x)
{
    unsigned sign=mpfr_signbit(x) ? 0x8000 : 0;
    if (mpfr_zero_p(x)) return (struct extended){0,sign};
    long exponent=mpfr_get_exp(x);
    mpfr_t scaled;mpfr_init2(scaled,64);
    mpfr_abs(scaled,x,MPFR_RNDN);
    mpfr_mul_2si(scaled,scaled,exponent+16382>0 ? 64-exponent : 16445,MPFR_RNDN);
    uint64_t sig=mpfr_get_uj(scaled,MPFR_RNDN);mpfr_clear(scaled);
    return (struct extended){sig,sign|(exponent+16382>0 ? exponent+16382 : 0)};
}

static unsigned load_reference(const struct form *f,const uint8_t *operand,uint8_t *out)
{
    if (f->type==R80) { memcpy(out,operand,10);return 0; }
    mpfr_t x;mpfr_init2(x,64);
    unsigned raised=0;
    if (f->type==R32 || f->type==R64) {
        unsigned fraction=f->type==R32 ? 23 : 52, bias=f->type==R32 ? 127 : 1023;
        uint64_t bits=get(operand,f->bytes), sig=bits&((1ull<<fraction)-1);
        unsigned exp=(bits>>fraction)&(bias*2+1), sign=bits>>(f->bytes*8-1);
        if (exp==bias*2+1) {
            struct extended value={(1ull<<63)|(sig<<(63-fraction)),0x7fff|(sign<<15)};
            if (sig) {
                if (!(sig>>(fraction-1))) raised=1;
                value.sig|=1ull<<62;
            }
            encode(out,value);mpfr_clear(x);return raised;
        }
        mpfr_set_uj(x,sig|(exp ? 1ull<<fraction : 0),MPFR_RNDN);
        mpfr_mul_2si(x,x,(long)(exp ? exp : 1)-bias-fraction,MPFR_RNDN);
        if (sign) mpfr_neg(x,x,MPFR_RNDN);
        if (!exp && sig) raised=2;
    } else if (f->type==BCD) {
        char decimal[21];decimal[0]=operand[9]&0x80 ? '-' : '+';
        for(unsigned i=0;i<18;i++) decimal[1+i]='0'+((operand[8-i/2]>>(i&1 ? 0 : 4))&15);
        decimal[19]=0;
        equal(mpfr_set_str(x,decimal,10,MPFR_RNDN),0,"BCD oracle parse");
    } else {
        uint64_t bits=get(operand,f->bytes);
        int64_t integer=f->type==I16 ? (int16_t)bits : f->type==I32 ? (int32_t)bits : (int64_t)bits;
        mpfr_set_sj(x,integer,MPFR_RNDN);
    }
    encode(out,get_extended(x));mpfr_clear(x);return raised;
}

static unsigned store_reference(const struct form *f,struct extended a,unsigned cw,uint8_t *out)
{
    if (f->type==R80) { encode(out,a);return 0; }
    static const mpfr_rnd_t modes[]={MPFR_RNDN,MPFR_RNDD,MPFR_RNDU,MPFR_RNDZ};
    mpfr_rnd_t round=f->truncate ? MPFR_RNDZ : modes[(cw>>10)&3];
    bool bad=unsupported(a), nan=nan_value(a), negative=a.exp>>15;
    unsigned raised=0;
    mpfr_t x,y,bounded,limit;mpfr_inits2(64,x,y,limit,(mpfr_ptr)0);
    mpfr_init2(bounded,f->type==R32 ? 24 : 53);
    set_extended(x,a);
    if (f->type==R32 || f->type==R64) {
        unsigned fraction=f->type==R32 ? 23 : 52, exponent_bits=f->type==R32 ? 8 : 11;
        uint64_t bits;
        if (bad || nan) {
            bits=(((1ull<<exponent_bits)-1)<<fraction)|(1ull<<(fraction-1));
            bits|=(uint64_t)(bad || negative)<<(f->bytes*8-1);
            if (!bad) bits|=(a.sig>>(63-fraction))&((1ull<<fraction)-1);
            if (bad || !(a.sig&(1ull<<62))) raised=1;
        } else {
            if (f->type==R32) {
                float v=mpfr_get_flt(x,round);uint32_t raw;memcpy(&raw,&v,4);bits=raw;
                mpfr_set_flt(y,v,MPFR_RNDN);
            } else {
                double v=mpfr_get_d(x,round);memcpy(&bits,&v,8);mpfr_set_d(y,v,MPFR_RNDN);
            }
            if (!mpfr_inf_p(x) && !mpfr_zero_p(x)) {
                mpfr_set(bounded,x,round);
                long exp=mpfr_get_exp(bounded), minimum=f->type==R32 ? -125 : -1021;
                long maximum=f->type==R32 ? 128 : 1024;
                bool overflow=exp>maximum, tiny=exp<minimum;
                bool unmasked=(overflow && !(cw&8)) || (tiny && !(cw&16));
                if (overflow) raised|=8;
                /* SDM 4.9.1.6: memory #O/#U traps suppress #P and C1. */
                bool inexact=!unmasked && !mpfr_equal_p(x,y);
                if (tiny && (inexact || !(cw&16))) raised|=16;
                if (inexact) {
                    raised|=32;
                    if (mpfr_cmpabs(y,x)>0) raised|=0x200;
                }
            }
        }
        put(out,bits,f->bytes);
    } else {
        bool invalid=bad || nan || mpfr_inf_p(x);
        if (!invalid) {
            mpfr_rint(y,x,round);
            if (f->type==BCD) {
                mpfr_set_uj(limit,999999999999999999ull,MPFR_RNDN);
                invalid=mpfr_cmpabs(y,limit)>0;
            } else {
                mpfr_set_ui_2exp(limit,1,f->bytes*8-1,MPFR_RNDN);
                invalid=mpfr_cmp(y,limit)>=0;
                mpfr_neg(limit,limit,MPFR_RNDN);invalid|=mpfr_cmp(y,limit)<0;
            }
        }
        if (invalid) {
            raised=1;
            if (f->type==BCD) { out[7]=0xc0;out[8]=out[9]=0xff; }
            else out[f->bytes-1]=0x80;
        } else {
            if (!mpfr_equal_p(x,y)) { raised=32;if(mpfr_cmpabs(y,x)>0)raised|=0x200; }
            if (f->type!=BCD) put(out,(uint64_t)mpfr_get_sj(y,MPFR_RNDZ),f->bytes);
            else {
                mpfr_abs(y,y,MPFR_RNDN);
                char decimal[19];snprintf(decimal,sizeof(decimal),"%018" PRIu64,(uint64_t)mpfr_get_uj(y,MPFR_RNDZ));
                for(unsigned i=0;i<9;i++) out[i]=(decimal[17-2*i]-'0')|((decimal[16-2*i]-'0')<<4);
                out[9]=negative ? 0x80 : 0;
            }
        }
    }
    mpfr_clears(x,y,bounded,limit,(mpfr_ptr)0);return raised;
}

static void test_input(struct extended a,const uint8_t *memory,bool exhaustive)
{
    const struct form *f=&forms[current_form];current_value=a;
    static const unsigned pcs[]={0,2,3};
    for(unsigned mask=0;mask<64;mask++) {
        if (!exhaustive && mask!=0 && mask!=31 && mask!=47 && mask!=55 && mask!=62 && mask!=63) continue;
        for(unsigned rc=0;rc<4;rc++)for(unsigned pc=0;pc<3;pc++) {
            current_cw=0x40|mask|(pcs[pc]<<8)|(rc<<10);current_top=inputs&7;
            _Alignas(64) uint8_t image[1024]={0};
            uint8_t before[1024],operand[16],memory_before[16],expected[10]={0};
            memcpy(operand,memory,16);memcpy(memory_before,operand,16);
            put(image,current_cw,2);put(image+2,0x4700|(current_top<<11),2);put(image+24,0x1f80,4);
            unsigned next=(current_top+7)&7;
            image[4]=0xff&~(f->load ? 1u<<next : 0);
            for(unsigned i=0;i<8;i++) encode(image+32+16*i,a);
            memcpy(before,image,sizeof(before));
            unsigned raised=f->load ? load_reference(f,operand,expected) : store_reference(f,a,current_cw,expected);
            bool pending=(raised&~current_cw&63)!=0;
            bool inhibited=(raised&~current_cw&(f->load ? 1 : 0x19))!=0;
            dos_fpu_numeric_result_t got;
            equal(dos_fpu_soft_evaluate(image,operand,f->op,f->mr,&got),1,"accepted");evaluations++;
            equal(got.write,0,"no register overwrite");equal(got.push,f->load,"push request");
            equal(got.pops,f->pop,"pop request");equal(got.memory_size,f->load ? 0 : f->bytes,"memory width");
            equal(got.condition_clear,0x200,"C1 mask");equal(got.conditions,0,"condition result");
            equal(got.integer_flags,0,"no EFLAGS result");equal(got.exceptions,raised,"MPFR exception/round-up flags");
            if (!inhibited) bytes_equal(f->load ? got.pushed_value : got.value,expected,f->load ? 10 : f->bytes,"MPFR value");
            bytes_equal(image,before,sizeof(image),"immutable input state");
            bytes_equal(operand,memory_before,16,"immutable input memory");
            const uint8_t *code=dos_fpu_memory_ops+((f->op-0xd8)*8+(f->mr>>3))*8;
            dos_fpu_host_step(image,code,operand,0);native_checks++;
            unsigned sw=get(image+2,2), changed=f->load && !inhibited ? 1u<<next : 0;
            unsigned top=inhibited ? current_top : f->load ? next : (current_top+f->pop)&7;
            unsigned tags=before[4]|changed;
            if (f->pop && !inhibited) tags&=~(1u<<current_top);
            equal(sw&0x82ff,raised|(pending ? 0x8080 : 0),"native exceptions and C1");
            equal((sw>>11)&7,top,"native TOP");equal(image[4],tags,"native tags");
            for(unsigned physical=0;physical<8;physical++) if ((tags&~changed)&(1u<<physical))
                bytes_equal(image+32+((physical-top)&7)*16,before+32+((physical-current_top)&7)*16,10,"native live operands");
            if (f->load) {
                if (!inhibited) bytes_equal(image+32,expected,10,"native load");
                bytes_equal(operand,memory_before,16,"native load memory");
            } else if (inhibited) bytes_equal(operand,memory_before,16,"native store inhibition");
            else {
                bytes_equal(operand,expected,f->bytes,"native store");
                bytes_equal(operand+f->bytes,memory_before+f->bytes,16-f->bytes,"native store guard");
            }
        }
    }
    inputs++;
}

static void memory_input(uint8_t *memory,uint64_t bits,struct extended a)
{
    const struct form *f=&forms[current_form];memset(memory,0xa5,16);
    if (!f->load) return;
    if (f->type==R80) encode(memory,a);
    else if (f->type==BCD) {
        uint64_t magnitude=(bits&0x7fffffffffffffffull)%1000000000000000000ull;
        char decimal[19];snprintf(decimal,sizeof(decimal),"%018" PRIu64,magnitude);
        for(unsigned i=0;i<9;i++) memory[i]=(decimal[17-2*i]-'0')|((decimal[16-2*i]-'0')<<4);
        memory[9]=(bits>>63 ? 0x80 : 0)|((bits>>40)&0x7f);
    } else put(memory,bits,f->bytes);
}

static void emit_bytes(const uint8_t *value, unsigned count)
{
    putchar('{');
    for (unsigned i=0;i<count;i++) printf("%s0x%02x",i ? "," : "",value[i]);
    putchar('}');
}

static void emit_case(struct extended value, uint64_t bits)
{
    const struct form *f=&forms[current_form];
    uint8_t source[10],memory[16],result[4][10]={{0}};
    unsigned masked[4],unmasked[4];
    encode(source,value);memory_input(memory,bits,value);
    for (unsigned rc=0;rc<4;rc++) {
        uint8_t discarded[10]={0};
        masked[rc]=f->load ? load_reference(f,memory,result[rc]) :
            store_reference(f,value,0x37f|(rc<<10),result[rc]);
        unmasked[rc]=f->load ? masked[rc] : store_reference(f,value,0x340|(rc<<10),discarded);
    }
    printf("    {0x%02x,0x%02x,%u,%u,%u,",f->op,f->mr,f->bytes,f->load,f->pop);
    emit_bytes(source,10);putchar(',');emit_bytes(memory,10);printf(",{\n        ");
    for (unsigned rc=0;rc<4;rc++) { if(rc)putchar(',');emit_bytes(result[rc],10); }
    printf("},{");
    for (unsigned rc=0;rc<4;rc++) printf("%s0x%03x",rc ? "," : "",masked[rc]);
    printf("},{");
    for (unsigned rc=0;rc<4;rc++) printf("%s0x%03x",rc ? "," : "",unmasked[rc]);
    printf("}},\n");
}

static int emit_guest(void)
{
    printf("/* Generated by dos_fpu_convert_host --emit-guest, MPFR %s.\n"
           " * Values come from the independent oracle, never the production adapter.\n"
           " * Regenerate with the host runner's test executable. */\n"
           "#ifndef DOS_FPU_CONVERT_CASES_H\n#define DOS_FPU_CONVERT_CASES_H\n\n"
           "static const struct dos_fpu_convert_case {\n"
           "    uint8_t opcode, modrm, width, load, pop;\n"
           "    uint8_t source[10], memory[10], result[4][10];\n"
           "    uint16_t masked[4], unmasked[4];\n"
           "} dos_fpu_convert_cases[] = {\n",mpfr_get_version());
    mpfr_t x,base,delta;mpfr_inits2(64,x,base,delta,(mpfr_ptr)0);
    for (current_form=0;current_form<sizeof(forms)/sizeof(forms[0]);current_form++) {
        const struct form *f=&forms[current_form];
        if (f->load && f->type!=R80) {
            for (unsigned i=0;i<sizeof(memory_values)/sizeof(memory_values[0]);i++)
            for (unsigned sign=0;sign<2;sign++) {
                unsigned sign_bit=f->bytes>=8 ? 63 : f->bytes*8-1;
                emit_case(special[10],memory_values[i]^(sign ? 1ull<<sign_bit : 0));
            }
            continue;
        }
        for (unsigned i=0;i<sizeof(special)/sizeof(special[0]);i++)for(unsigned sign=0;sign<2;sign++) {
            struct extended a=special[i];a.exp^=sign<<15;emit_case(a,0);
        }
        if (f->type==R80) continue;
        int high=f->type==R32 ? 128 : f->type==R64 ? 1024 : f->type==BCD ? 60 : f->bytes*8-1;
        int low=f->type==R32 ? -149 : -1074;
        int powers[]={high-1,high,low-1,low,low+(f->type==R32 ? 23 : 52)};
        for (unsigned i=0;i<(f->type==R32 || f->type==R64 ? 5u : 2u);i++)
        for (unsigned neighbor=0;neighbor<3;neighbor++)for(unsigned sign=0;sign<2;sign++) {
            mpfr_set_ui_2exp(x,1,powers[i],MPFR_RNDN);
            if(neighbor==1)mpfr_nextabove(x);else if(neighbor==2)mpfr_nextbelow(x);
            if(sign)mpfr_neg(x,x,MPFR_RNDN);
            emit_case(get_extended(x),0);
        }
        if (f->type==R32 || f->type==R64) {
            int minimum=f->type==R32 ? -126 : -1022;
            mpfr_set_ui_2exp(base,1,minimum,MPFR_RNDN);
            mpfr_set_ui_2exp(delta,1,minimum-(f->type==R32 ? 24 : 53),MPFR_RNDN);
        } else {
            if(f->type==BCD)mpfr_set_uj(base,1000000000000000000ull,MPFR_RNDN);
            else mpfr_set_ui_2exp(base,1,high,MPFR_RNDN);
            mpfr_set_ui_2exp(delta,1,-1,MPFR_RNDN);
        }
        for (unsigned neighbor=0;neighbor<3;neighbor++)for(unsigned sign=0;sign<2;sign++) {
            mpfr_sub(x,base,delta,MPFR_RNDN);
            if(neighbor==1)mpfr_nextabove(x);else if(neighbor==2)mpfr_nextbelow(x);
            if(sign)mpfr_neg(x,x,MPFR_RNDN);
            emit_case(get_extended(x),0);
        }
    }
    printf("};\n\n#endif\n");
    mpfr_clears(x,base,delta,(mpfr_ptr)0);mpfr_free_cache();
    return failures!=0;
}

int main(int argc, char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--emit-guest")) return emit_guest();
    if (argc!=1) return 2;
    setvbuf(stdout,NULL,_IOLBF,0);
    printf("CONVERT MPFR version=%s seed=a4093822299f31d0\n",mpfr_get_version());
    static const int powers[]={-16382,-1075,-1074,-1022,-150,-149,-126,-1,0,1,14,15,30,31,58,59,62,63,64,127,128,1023,1024,16383};
    mpfr_t x,base,delta;mpfr_inits2(64,x,base,delta,(mpfr_ptr)0);
    for(current_form=0;current_form<sizeof(forms)/sizeof(forms[0]);current_form++) {
        for(unsigned i=0;i<sizeof(special)/sizeof(special[0]);i++)
        for(unsigned j=0;j<(forms[current_form].load ? sizeof(memory_values)/sizeof(memory_values[0]) : 1);j++)
        for(unsigned sign=0;sign<2;sign++) {
            struct extended a=special[i];a.exp^=sign<<15;
            unsigned sign_bit=forms[current_form].bytes>=8 ? 63 : forms[current_form].bytes*8-1;
            uint8_t memory[16];memory_input(memory,memory_values[j]^(sign ? 1ull<<sign_bit : 0),a);
            test_input(a,memory,true);
        }
        if (!forms[current_form].load) for(unsigned i=0;i<sizeof(powers)/sizeof(powers[0]);i++)
        for(unsigned neighbor=0;neighbor<3;neighbor++)for(unsigned sign=0;sign<2;sign++) {
            mpfr_set_ui_2exp(x,1,powers[i],MPFR_RNDN);
            if (neighbor==1) mpfr_nextabove(x);else if(neighbor==2)mpfr_nextbelow(x);
            if (sign) mpfr_neg(x,x,MPFR_RNDN);
            struct extended a=get_extended(x);uint8_t memory[16];memory_input(memory,0,a);test_input(a,memory,true);
        }
        if (!forms[current_form].load) for(unsigned boundary=0;boundary<6;boundary++)
        for(unsigned neighbor=0;neighbor<3;neighbor++)for(unsigned sign=0;sign<2;sign++) {
            if (boundary<4) {
                if (boundary==3) mpfr_set_uj(base,1000000000000000000ull,MPFR_RNDN);
                else mpfr_set_ui_2exp(base,1,boundary==0 ? 15 : boundary==1 ? 31 : 63,MPFR_RNDN);
                mpfr_set_ui_2exp(delta,1,-1,MPFR_RNDN);
            } else {
                int minimum=boundary==4 ? -126 : -1022;
                mpfr_set_ui_2exp(base,1,minimum,MPFR_RNDN);
                mpfr_set_ui_2exp(delta,1,minimum-(boundary==4 ? 24 : 53),MPFR_RNDN);
            }
            mpfr_sub(x,base,delta,MPFR_RNDN);
            if (neighbor==1) mpfr_nextabove(x);else if(neighbor==2)mpfr_nextbelow(x);
            if (sign) mpfr_neg(x,x,MPFR_RNDN);
            struct extended a=get_extended(x);uint8_t memory[16];memory_input(memory,0,a);test_input(a,memory,true);
        }
        for(unsigned sample=0;sample<1024;sample++) {
            struct extended a={random_bits(),random_bits()&0xffff};
            if (sample&1) a.sig|=1ull<<63;
            uint8_t memory[16];memory_input(memory,random_bits(),a);test_input(a,memory,false);
        }
        printf("CONVERT form=%u evaluations=%" PRIu64 " failures=%" PRIu64 "\n",current_form,evaluations,failures);
    }
    mpfr_clears(x,base,delta,(mpfr_ptr)0);mpfr_free_cache();
    printf("CONVERT MPFR forms=21 inputs=%" PRIu64 " evaluations=%" PRIu64 " checks=%" PRIu64
        " native=%" PRIu64 " failures=%" PRIu64 "\n",inputs,evaluations,checks,native_checks,failures);
    return failures!=0;
}
