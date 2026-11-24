#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <mpfr.h>
#include "../dos/dos_fpu_state.h"

unsigned cpu_xsave_active;
extern const uint8_t dos_fpu_register_ops[], dos_fpu_memory_ops[];
extern uint64_t dos_fpu_host_step(void *, const uint8_t *, void *, uint32_t);
enum { NONE, LOAD, EXCHANGE, STORE, POPSTORE, CMOV, NEGCMOV, SIGN, ABS, DEC, INC,
       FREE, CONSTANT, NOP, CLEAR, INIT, AX, CWLOAD, CWSAVE, SWSAVE, ENVLOAD, ENVSAVE, RESTORE, SAVE };
static unsigned classify(unsigned op, unsigned mr)
{
    if (mr < 0xc0) {
        unsigned g = mr >> 3 & 7;
        if (op == 0xd9) return g == 4 ? ENVLOAD : g == 5 ? CWLOAD : g == 6 ? ENVSAVE : g == 7 ? CWSAVE : NONE;
        if (op == 0xdd) return g == 4 ? RESTORE : g == 6 ? SAVE : g == 7 ? SWSAVE : NONE;
        return NONE;
    }
    if (op == 0xd9) {
        if (mr < 0xc8) return LOAD;
        if (mr < 0xd0) return EXCHANGE;
        if (mr == 0xd0) return NOP;
        if (mr == 0xe0) return SIGN;
        if (mr == 0xe1) return ABS;
        if (mr >= 0xe8 && mr <= 0xee) return CONSTANT;
        if (mr == 0xf6 || mr == 0xf7) return mr == 0xf6 ? DEC : INC;
    }
    if (op == 0xda && mr < 0xe0) return CMOV;
    if (op == 0xdb) {
        if (mr < 0xe0) return NEGCMOV;
        if (mr == 0xe2) return CLEAR;
        if (mr == 0xe3) return INIT;
        if (mr == 0xe0 || mr == 0xe1 || mr == 0xe4) return NOP;
    }
    if (op == 0xdd) {
        if (mr < 0xc8) return FREE;
        if (mr >= 0xd0 && mr < 0xe0) return mr < 0xd8 ? STORE : POPSTORE;
    }
    if (op == 0xdf && mr == 0xe0) return AX;
    return NONE;
}
static uint64_t checks, failures, cases, undefined_cc;
static uint32_t mxcsr_mask;
static unsigned cur_op, cur_mr, cur_sample, cur_top, cur_rc, cur_pc, cur_flags;
static bool exhaustive;
static unsigned load_control;
static uint8_t constants[7][4][10];
static uint64_t get(const uint8_t *p, unsigned bytes)
{ uint64_t v = 0; for (unsigned b = 0; b < bytes; b++) v |= (uint64_t)p[b] << (8*b); return v; }
static void put(uint8_t *p, uint64_t v, unsigned bytes)
{ for (unsigned b = 0; b < bytes; b++) p[b] = v >> (8*b); }
static void check(bool condition, const char *label)
{
    static const char *last_label;
    static unsigned last_op, last_mr;
    checks++;
    if (condition) return;
    failures++;
    if (last_op != cur_op || last_mr != cur_mr || last_label != label) {
        printf("FAIL %02x%02x sample=%u top=%u rc=%u pc=%u flags=%x %s\n",
            cur_op, cur_mr, cur_sample, cur_top, cur_rc, cur_pc, cur_flags, label);
        last_op = cur_op; last_mr = cur_mr; last_label = label;
    }
}
static void bytes(const uint8_t *got, const uint8_t *expected, unsigned size, const char *label)
{ check(!memcmp(got, expected, size), label); }

static void constant_oracle(void)
{
    static const mpfr_rnd_t rounding[] = {MPFR_RNDN, MPFR_RNDD, MPFR_RNDU, MPFR_RNDZ};
    mpfr_t real, rounded;
    mpfr_init2(real, 512); mpfr_init2(rounded, 64);
    for (unsigned i = 0; i < 7; i++) {
        if (i == 0) mpfr_set_ui(real, 1, MPFR_RNDN);
        if (i == 1) { mpfr_set_ui(real, 10, MPFR_RNDN); mpfr_log2(real, real, MPFR_RNDN); }
        if (i == 2) { mpfr_const_log2(real, MPFR_RNDN); mpfr_ui_div(real, 1, real, MPFR_RNDN); }
        if (i == 3) mpfr_const_pi(real, MPFR_RNDN);
        if (i == 4) { mpfr_set_ui(real, 2, MPFR_RNDN); mpfr_log10(real, real, MPFR_RNDN); }
        if (i == 5) mpfr_const_log2(real, MPFR_RNDN);
        if (i == 6) mpfr_set_ui(real, 0, MPFR_RNDN);
        for (unsigned rc = 0; rc < 4; rc++) {
            /* The SDM specifies internal 66-bit constants, not rounding each
             * mathematical value to 66 bits first. That would double-round
             * FLDLG2 incorrectly. Check the final 64-bit rounding against the
             * native instruction independently below. */
            mpfr_set(rounded, real, rounding[rc]);
            long exponent = i == 6 ? -16382 : mpfr_get_exp(rounded);
            mpfr_mul_2si(rounded, rounded, 64 - exponent, MPFR_RNDN);
            put(constants[i][rc], mpfr_get_uj(rounded, MPFR_RNDN), 8);
            put(constants[i][rc] + 8, exponent + 16382, 2);
            printf("CONSTANT index=%u rc=%u bits=%04" PRIx64 ":%016" PRIx64 "\n", i, rc,
                get(constants[i][rc] + 8, 2), get(constants[i][rc], 8));
        }
    }
    mpfr_clears(real, rounded, (mpfr_ptr)0);
}

static unsigned full_tags(const uint8_t *image)
{
    unsigned tags = 0, top = get(image + 2, 2) >> 11 & 7;
    for (unsigned p = 0; p < 8; p++) {
        const uint8_t *v = image + 32 + ((p - top) & 7) * 16;
        uint64_t significand = get(v, 8);
        unsigned exponent = get(v + 8, 2) & 0x7fff, tag = 3;
        if (image[4] & (1u << p)) {
            tag = 0;
            if (!exponent && !significand) tag = 1;
            else if (!exponent || exponent == 0x7fff || !(significand >> 63)) tag = 2;
        }
        tags |= tag << (2*p);
    }
    return tags;
}

static void test(unsigned kind)
{
    static const struct { uint64_t sig; uint16_t exp; } values[] = {
        {0,0}, {0,0x8000}, {1,0}, {UINT64_MAX >> 1,0}, {1ull << 63,0},
        {1ull << 63,1}, {1ull << 63,0x3fff}, {UINT64_MAX,0xbfff},
        {1ull << 63,0x7fff}, {1ull << 63,0xffff}, {0xc000000000000123ull,0x7fff},
        {0xa000000000000123ull,0xffff}, {0,1}, {1,0x7fff}, {UINT64_MAX,0x7ffe}, {UINT64_MAX,0xfffe}
    };
    _Alignas(64) uint8_t image[1024] = {0}, native[1024];
    uint8_t expected[1024], operand[128], native_operand[128], memory[128], physical[8][16];
    unsigned cw = 0x40 | (cur_pc << 8) | (cur_rc << 10), sw = (cur_top << 11) | 0x4700;
    bool no_wait = kind == CLEAR || kind == INIT || kind == AX || kind == CWSAVE || kind == SWSAVE ||
                   kind == ENVSAVE || kind == SAVE || (kind == NOP && cur_op == 0xdb);
    cw |= no_wait ? cur_sample ^ 63u : 63;
    sw |= cur_sample;
    if (sw & ~cw & 63) sw |= 0x8080;
    put(image, cw, 2); put(image + 2, sw, 2); put(image + 24, 0x1f80, 4);
    put(image + 28, mxcsr_mask, 4);
    memset(image + 160, 0x5a, 256);
    put(image + 6, 0x341, 2); put(image + 8, 0x12345678, 8); put(image + 16, 0x76543210, 8);
    image[4] = 0xff & ~((kind == LOAD || kind == CONSTANT) ? 1u << ((cur_top + 7) & 7) : 0);
    for (unsigned i = 0; i < 8; i++) {
        unsigned v = (i + cur_sample) % (sizeof(values)/sizeof(values[0]));
        put(image + 32 + i*16, values[v].sig, 8); put(image + 40 + i*16, values[v].exp, 2);
        memset(image + 42 + i*16, 0x80 + i, 6);
        memcpy(physical[(i + cur_top) & 7], image + 32 + i*16, 16);
    }
    memcpy(expected, image, sizeof(image)); memcpy(native, image, sizeof(image));
    memset(operand, 0xa5, sizeof(operand));
    if (kind == CWLOAD || kind == ENVLOAD || kind == RESTORE)
        put(operand, exhaustive ? load_control : cur_sample * 1040u, 2);
    if (kind == ENVLOAD || kind == RESTORE) {
        put(operand + 4, ((cur_top + 3) & 7) << 11 | cur_sample, 2);
        put(operand + 8, exhaustive ? load_control : cur_sample * 0x1041u, 2);
        for (unsigned i = 0; i < 8; i++) memcpy(operand + 28 + i*10, image + 32 + ((i+2)&7)*16, 10);
    }
    memcpy(native_operand, operand, sizeof(operand)); memcpy(memory, operand, sizeof(operand));
    unsigned new_top = cur_top, tags = image[4], width = 0;
    uint64_t expected_ax = 0, result = 0;
    unsigned dest = (cur_top + (cur_mr & 7)) & 7;
    switch (kind) {
    case LOAD: new_top = (cur_top + 7) & 7; memcpy(physical[new_top], physical[dest], 10); tags |= 1u << new_top; break;
    case CONSTANT: new_top = (cur_top + 7) & 7; memcpy(physical[new_top], constants[cur_mr-0xe8][cur_rc], 10); tags |= 1u << new_top; break;
    case EXCHANGE: { uint8_t tmp[10]; memcpy(tmp, physical[cur_top], 10);
        memcpy(physical[cur_top], physical[dest], 10); memcpy(physical[dest], tmp, 10); break; }
    case STORE: case POPSTORE:
        memcpy(physical[dest], physical[cur_top], 10); tags |= 1u << dest;
        if (kind == POPSTORE) { tags &= ~(1u << cur_top); new_top = (cur_top + 1) & 7; } break;
    case CMOV: case NEGCMOV: {
        unsigned group = cur_mr >> 3 & 3;
        bool take = group == 0 ? cur_flags & 1 : group == 1 ? cur_flags & 0x40 :
                    group == 2 ? cur_flags & 0x41 : cur_flags & 4;
        if (take != (kind == NEGCMOV)) memcpy(physical[cur_top], physical[dest], 10);
        break; }
    case SIGN: physical[cur_top][9] ^= 0x80; break;
    case ABS: physical[cur_top][9] &= 0x7f; break;
    case DEC: new_top = (cur_top + 7) & 7; break;
    case INC: new_top = (cur_top + 1) & 7; break;
    case FREE: tags &= ~(1u << dest); break;
    case CLEAR: sw &= 0x7f00; break;
    case INIT: cw = 0x37f; sw = 0; tags = 0; new_top = 0; break;
    case AX: expected_ax = sw; break;
    case CWSAVE: width = 2; put(memory, cw, 2); break;
    case SWSAVE: width = 2; put(memory, sw, 2); break;
    case CWLOAD: cw = (get(operand, 2) & 0x1f3f) | 0x40; break;
    case ENVLOAD: case RESTORE:
        cw = (get(operand, 2) & 0x1f3f) | 0x40; sw = get(operand + 4, 2); new_top = sw >> 11 & 7;
        tags = 0;
        for (unsigned p = 0; p < 8; p++) if ((get(operand + 8, 2) >> (2*p) & 3) != 3) tags |= 1u << p;
        if (kind == RESTORE) for (unsigned i = 0; i < 8; i++) memcpy(physical[(new_top+i)&7], operand+28+i*10, 10);
        break;
    case ENVSAVE: case SAVE:
        width = kind == SAVE ? 108 : 28; memset(memory, 0, 28);
        put(memory, cw, 2); put(memory + 4, sw, 2); put(memory + 8, full_tags(image), 2);
        if (kind == SAVE) {
            for (unsigned i = 0; i < 8; i++) memcpy(memory + 28 + i*10, image + 32 + i*16, 10);
            cw = 0x37f; sw = 0; tags = 0; new_top = 0;
        } else cw |= 63;
        break;
    default: break;
    }
    if (kind <= INC) sw &= ~0x200u;
    if (kind == CWLOAD || kind == ENVLOAD || kind == RESTORE || kind == ENVSAVE) {
        sw &= ~0x8080u; if (sw & ~cw & 63) sw |= 0x8080;
    }
    sw = (sw & ~0x3800u) | new_top << 11;
    put(expected, cw, 2); put(expected + 2, sw, 2); expected[4] = tags;
    if (kind == INIT || kind == SAVE) memset(expected + 6, 0, 18);
    for (unsigned i = 0; i < 8; i++) memcpy(expected + 32 + i*16, physical[(new_top+i)&7], 16);
    if (kind == CONSTANT) put(expected + 2, sw & ~0x200u, 2);
    check(dos_fpu_state_step(image, operand, cur_op, cur_mr, cur_flags, &result), "accepted");
    bytes(image, expected, sizeof(image), "reference state");
    bytes(operand, memory, sizeof(memory), "reference memory and guards");
    check(result == expected_ax, "reference AX");
    const uint8_t *code = cur_mr < 0xc0 ? dos_fpu_memory_ops + ((cur_op-0xd8)*8+(cur_mr>>3))*8 :
        dos_fpu_register_ops + ((cur_op-0xd8)*64+cur_mr-0xc0)*8;
    uint64_t returned = dos_fpu_host_step(native, code, native_operand, cur_flags);
    check(((returned >> 32) & 0x45u) == cur_flags, "native integer flags");
    check(get(native, 2) == get(image, 2), "native control");
    unsigned status_mask = 0xbaff;
    if (kind == CMOV || kind == NEGCMOV || kind == FREE || kind == CLEAR ||
        kind == CWLOAD || kind == ENVSAVE) status_mask &= ~0x200u;
    check((get(native+2,2) & status_mask) == (get(image+2,2) & status_mask), "native defined status/TOP");
    undefined_cc += ((get(native+2,2) ^ get(image+2,2)) & 0x4700u & ~status_mask) != 0;
    check(native[4] == image[4], "native tags");
    for (unsigned p = 0; p < 8; p++) if (image[4] & (1u << p))
        bytes(native + 32 + ((p-new_top)&7)*16, image + 32 + ((p-new_top)&7)*16, 10, "native live register");
    bytes(native + 24, image + 24, 8, "native SSE state");
    bytes(native + 160, image + 160, 256, "native XMM state");
    if (kind == AX) check((returned & 0xffff) == result, "native AX");
    if (kind == SAVE || kind == ENVSAVE) {
        for (unsigned i = 0; i < 3; i++) bytes(native_operand+i*4, operand+i*4, 2, "native environment word");
        if (kind == SAVE) bytes(native_operand+28, operand+28, 80, "native saved registers");
    } else if (width) bytes(native_operand, operand, width, "native stored word");
    bytes(native_operand + width, memory + width, sizeof(memory)-width, "native memory guard");
    cases++;
}

static void rejected_encodings(void)
{
    uint8_t image[1024], before[1024], operand[128], memory[128];
    unsigned rejected = 0;
    memset(image, 0xa5, sizeof(image)); memcpy(before, image, sizeof(image));
    memset(operand, 0x5a, sizeof(operand)); memcpy(memory, operand, sizeof(operand));
    for (cur_op = 0; cur_op < 256; cur_op++) for (cur_mr = 0; cur_mr < 256; cur_mr++) {
        if (classify(cur_op, cur_mr)) continue;
        uint64_t result = UINT64_MAX;
        check(!dos_fpu_state_step(image, operand, cur_op, cur_mr, 0, &result), "rejected encoding");
        bytes(image, before, sizeof(image), "rejected state unchanged");
        bytes(operand, memory, sizeof(memory), "rejected operand unchanged");
        check(result == UINT64_MAX, "rejected result unchanged");
        rejected++;
    }
    printf("STATE REJECT encodings=%u\n", rejected);
    check(rejected == 65246, "rejected encoding inventory");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    _Alignas(16) uint8_t host[512];
    __asm__ volatile("fxsave %0" : "=m"(host));
    mxcsr_mask = get(host + 28, 4);
    constant_oracle();
    unsigned forms = 0;
    for (cur_op = 0xd8; cur_op <= 0xdf; cur_op++) for (cur_mr = 0; cur_mr <= 0xff; cur_mr++) {
        unsigned kind = classify(cur_op, cur_mr);
        if (cur_mr < 0xc0 && (cur_mr & ~0x38u)) continue;
        if (!kind) continue;
        /* FLD ST(7) necessarily underflows or overflows: the outer executor's
         * stack matrix owns it, not this prevalidated state helper. */
        if (kind == LOAD && cur_mr == 0xc7) continue;
        forms++;
        for (cur_sample = 0; cur_sample < 64; cur_sample++) for (cur_top = 0; cur_top < 8; cur_top++)
        for (cur_rc = 0; cur_rc < 4; cur_rc++) for (cur_pc = 0; cur_pc < 4; cur_pc++)
        for (unsigned flags = 0; flags < (kind == CMOV || kind == NEGCMOV ? 8u : 1u); flags++) {
            cur_flags = (flags & 1) | (flags & 2) << 1 | (flags & 4) << 4;
            test(kind);
        }
        printf("STATE form=%u opcode=%02x%02x cases=%" PRIu64 " failures=%" PRIu64 "\n", forms, cur_op, cur_mr, cases, failures);
    }
    exhaustive = true;
    for (load_control = 0; load_control < 65536; load_control++) {
        cur_sample = (load_control >> 10) ^ 0x15u; cur_top = load_control >> 13;
        cur_pc = 3; cur_rc = 0; cur_flags = 0;
        cur_op = 0xd9; cur_mr = 0x28; test(CWLOAD);
        cur_mr = 0x20; test(ENVLOAD);
        cur_op = 0xdd; test(RESTORE);
    }
    printf("STATE CONTROL encodings=65536 forms=3 tag-words=65536\n");
    rejected_encodings();
    printf("OBSERVATION native-undefined-cc=%" PRIu64 " stack-only-forms=1\n", undefined_cc);
    printf("STATE HOST forms=%u cases=%" PRIu64 " checks=%" PRIu64 " failures=%" PRIu64 "\n", forms, cases, checks, failures);
    mpfr_free_cache();
    return failures != 0;
}
