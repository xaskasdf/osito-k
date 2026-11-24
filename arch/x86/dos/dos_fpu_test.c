#include "dos_fpu.h"
#include "cpu8086.h"
#include "dos_hostmem.h"
#include "dos_jit.h"
#include "dos_fpu_numeric_cases.h"
#include "dos_fpu_convert_cases.h"
#include "dos_fpu_softfloat.h"
#include "dos_fpu_state.h"

extern void serial_puts(const char *);
extern void serial_putdec(uint64_t);
extern uint64_t idt_get_ticks(void);

static uint16_t test_u16(const void *p) { return *(const uint16_t *)p; }
static uint32_t test_u32(const void *p) { return *(const uint32_t *)p; }
static void test_w16(void *p, uint16_t v) { *(uint16_t *)p = v; }
static void test_w32(void *p, uint32_t v) { *(uint32_t *)p = v; }

static cpu_event_fault_t fpu_test_step(dos_vm_t *vm, unsigned opcode,
                                       unsigned modrm, uint32_t address, unsigned prefix)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint8_t *code = vm->mem + 0x1000;
    unsigned n = 0;
    bool wide = cpu->addr_size_32 ^ (prefix == 0x67);
    if (prefix) code[n++] = prefix;
    code[n++] = opcode;
    if (opcode != 0x9b) {
        code[n++] = modrm >= 0xc0 ? modrm : (modrm & 0x38) | (wide ? 5 : 6);
        if (modrm < 0xc0) {
            for (unsigned i = 0; i < (wide ? 4u : 2u); i++) code[n++] = address >> (8 * i);
        }
    }
    cpu->eip = 0x1000;
    cpu_event_fault_t fault = {0};
    cpu->delivery_fault = &fault;
    (void)cpu8086_run_one(vm);
    cpu->delivery_fault = NULL;
    return fault;
}

static void fpu_test_mode(dos_vm_t *vm, unsigned mode)
{
    cpu8086_state_t *cpu = vm->cpu;
    cpu8086_init(cpu, vm);
    cpu->cr0 |= 0x20u;
    cpu->esp = 0x9000;
    if (mode) {
        dpmi_descriptor_t data = { .limit_lo = 0xffff, .access = 0xf3,
                                  .flags_lim = mode == 2 ? DESC_32BIT : 0 };
        dpmi_descriptor_t code = data;
        code.access = 0xfb;
        cpu->protected_mode = true;
        cpu->cr0 |= 1;
        cpu8086_cache_cs(cpu, 0x0f, &code, 3);
        for (unsigned seg = 0; seg < 6; seg++)
            if (seg != 1) cpu8086_cache_segment(cpu, seg, 0x17, &data);
    }
    vm->emulate_cpu = true;
    vm->start_ticks = vm->last_timer_tick = idt_get_ticks();
    vm->timer_irq_pending = false;
}

static void fpu_test_seed_stack(uint8_t *image, unsigned top, unsigned tags, uint16_t control)
{
    test_w16(image, control);
    test_w16(image + 2, (top << 11) | 0x0300u); /* C0/C1 start set. */
    image[4] = tags;
    for (unsigned i = 0; i < 8; i++) {
        uint8_t *reg = image + 32 + i * 16;
        memset(reg, 0, 16);
        test_w32(reg, ((top + i) & 7u) + 1);
        test_w32(reg + 4, 0x80000000u);
        test_w16(reg + 8, 0x3fff);
    }
}

static bool fpu_test_same_registers(const uint8_t *after, const uint8_t *before, unsigned mask)
{
    unsigned old_top = (test_u16(before + 2) >> 11) & 7u;
    unsigned new_top = (test_u16(after + 2) >> 11) & 7u;
    for (unsigned physical = 0; physical < 8; physical++) {
        if (!(mask & (1u << physical))) continue;
        if (memcmp(after + 32 + ((physical - new_top) & 7u) * 16,
                   before + 32 + ((physical - old_top) & 7u) * 16, 10)) return false;
    }
    return true;
}

static unsigned fpu_test_arithmetic_stack(dos_vm_t *vm, unsigned mode, unsigned *total)
{
    enum { EXCHANGE = 1, PAIR = 2, CLEAR_C2 = 4, CONDITIONAL = 8 };
    struct stack_case { uint8_t opcode, modrm, sources, destination, pops, flags; } cases[256];
    unsigned count = 0, checks = 0, failures = 0;
    static const uint8_t groups[] = {0,1,4,5,6,7}, register_ops[] = {0xd8,0xdc,0xde};
    static const uint8_t unary[] = {0xe0,0xe1,0xf0,0xfa,0xfc,0xfe,0xff};
    static const uint8_t reduce[] = {0xf1,0xf3,0xf9}, binary[] = {0xf5,0xf8,0xfd};
    static const uint8_t pair[] = {0xf2,0xf4,0xfb};
#define STACK_CASE(op, mr, src, dst, pop, flags) \
    cases[count++] = (struct stack_case){op, mr, src, dst, pop, flags}
    for (unsigned op = 0; op < 3; op++) for (unsigned g = 0; g < 6; g++)
        for (unsigned i = 0; i < 8; i++)
            STACK_CASE(register_ops[op], 0xc0 | (groups[g] << 3) | i,
                       1 | (1u << i), op ? i : 0, op == 2, 0);
    for (unsigned op = 0xd8; op <= 0xde; op += 2) for (unsigned g = 0; g < 6; g++)
        STACK_CASE(op, groups[g] << 3, 1, 0, 0, 0);
    for (unsigned i = 0; i < sizeof(unary); i++)
        STACK_CASE(0xd9, unary[i], 1, 0, 0, unary[i] >= 0xfe ? CLEAR_C2 : 0);
    for (unsigned i = 0; i < sizeof(reduce); i++) STACK_CASE(0xd9, reduce[i], 3, 1, 1, 0);
    for (unsigned i = 0; i < sizeof(binary); i++)
        STACK_CASE(0xd9, binary[i], 3, 0, 0, binary[i] == 0xfd ? 0 : CLEAR_C2);
    for (unsigned i = 0; i < sizeof(pair); i++)
        STACK_CASE(0xd9, pair[i], 1, 0, 0, PAIR | (pair[i] == 0xf4 ? 0 : CLEAR_C2));
    for (unsigned i = 0; i < 8; i++) STACK_CASE(0xd9, 0xc8 | i, 1 | (1u << i), i, 0, EXCHANGE);
    for (unsigned op = 0xda; op <= 0xdb; op++) for (unsigned cc = 0; cc < 4; cc++)
        for (unsigned i = 0; i < 8; i++)
            STACK_CASE(op, 0xc0 | (cc << 3) | i, 1 | (1u << i), 0, 0, CONDITIONAL);
#undef STACK_CASE
    unsigned ci = 0, top = 0, masked = 0;
#define STACK_CHECK(condition) do { \
    checks++; if (!(condition)) { failures++; if (failures <= 12) { \
        serial_puts("[DOS-FPU-STACK] FAIL line="); serial_putdec(__LINE__); \
        serial_puts(" mode="); serial_putdec(mode); serial_puts(" case="); serial_putdec(ci); \
        serial_puts(" top="); serial_putdec(top); serial_puts(" masked="); serial_putdec(masked); \
        serial_puts("\n"); } } \
} while (0)
    STACK_CHECK(count == sizeof(cases) / sizeof(cases[0]));
    uint8_t *image = vm->native_fp->image, *data = vm->mem + 0x4000;
    static const uint8_t indefinite[10] = {0,0,0,0,0,0,0,0xc0,0xff,0xff};
    for (ci = 0; ci < count; ci++) for (top = 0; top < 8; top++)
        for (masked = 0; masked < 2; masked++) {
            const struct stack_case *c = &cases[ci];
            unsigned variants = c->flags & CONDITIONAL ? 8 : 1;
            for (unsigned flags = 0; flags < variants; flags++)
                for (unsigned missing = c->sources;; missing = (missing - 1) & c->sources) {
                    for (unsigned full = 0; full < (c->flags & PAIR ? 2u : 1u); full++) {
                        cpu_event_fault_t fault = fpu_test_step(vm, 0xdb, 0xe3, 0, 0);
                        STACK_CHECK(!fault.raised);
                        unsigned tags = 0xff;
                        for (unsigned i = 0; i < 8; i++)
                            if (missing & (1u << i)) tags &= ~(1u << ((top + i) & 7u));
                        unsigned next = (top + 7) & 7u, index = c->modrm & 7u;
                        if ((c->flags & PAIR) && !full) tags &= ~(1u << next);
                        fpu_test_seed_stack(image, top, tags, 0x037e | masked);
                        /* Keep healthy transcendental inputs inside their domain. */
                        if (c->opcode == 0xd9 && (c->modrm == 0xf0 || c->modrm == 0xf9))
                            test_w16(image + 40, 0x3ffc);
                        test_w16(image + 2, test_u16(image + 2) | 0x4500u);
                        uint8_t prior[160]; memcpy(prior, image, sizeof(prior));
                        memset(data, 0, 16);
                        uint32_t input_flags = FLAGS_FIXED | FLAG_OF | FLAG_SF | FLAG_AF |
                            (flags & 1 ? FLAG_CF : 0) | (flags & 2 ? FLAG_ZF : 0) | (flags & 4 ? FLAG_PF : 0);
                        vm->cpu->eflags = input_flags;
                        fault = fpu_test_step(vm, c->opcode, c->modrm, 0x4000, 0);
                        STACK_CHECK(!fault.raised);
                        STACK_CHECK(test_u16(image) == (0x037e | masked));
                        STACK_CHECK((vm->cpu->eflags & 0x8d5u) == (input_flags & 0x8d5u));
                        bool stack_fault = missing || full;
                        bool commits = !stack_fault || masked;
                        unsigned new_top = top, expected_tags = tags, changed = 0;
                        if (commits) {
                            changed = 1u << ((top + c->destination) & 7u);
                            if (c->flags & EXCHANGE) changed |= 1u << top;
                            if (c->flags & PAIR) { changed |= 1u << next; new_top = next; }
                            expected_tags |= changed;
                            if (c->pops) {
                                expected_tags &= ~(1u << top);
                                new_top = (top + 1) & 7u;
                            }
                        }
                        STACK_CHECK(image[4] == expected_tags &&
                                    (test_u16(image + 2) & 0x3800u) == new_top << 11);
                        STACK_CHECK(fpu_test_same_registers(image, prior, tags & ~changed));
                        if (stack_fault) {
                            uint16_t status = 0x41u | (!masked ? 0x8080u : 0) | (!missing && full ? 0x0200u : 0);
                            STACK_CHECK((test_u16(image + 2) & 0x82c1u) == status);
                            if (c->flags & CLEAR_C2) STACK_CHECK(!(test_u16(image + 2) & 0x0400u));
                            if (masked && !(c->flags & EXCHANGE)) {
                                for (unsigned physical = 0; physical < 8; physical++)
                                    if ((changed & expected_tags) & (1u << physical))
                                        STACK_CHECK(!memcmp(image + 32 + ((physical - new_top) & 7u) * 16, indefinite, 10));
                            }
                        } else {
                            STACK_CHECK(!(test_u16(image + 2) & 0x0041u));
                            if (c->flags & CONDITIONAL) {
                                static const uint8_t conditions[] = {1,2,3,4};
                                bool move = (flags & conditions[(c->modrm >> 3) & 3u]) != 0;
                                if (c->opcode == 0xdb) move = !move;
                                STACK_CHECK(!memcmp(image + 32, prior + 32 + (move ? index : 0) * 16, 10));
                            }
                        }
                        if (commits && (c->flags & EXCHANGE)) {
                            const uint8_t *a = missing & (1u << index) ? indefinite : prior + 32 + index * 16;
                            const uint8_t *b = missing & 1u ? indefinite : prior + 32;
                            STACK_CHECK(!memcmp(image + 32, a, 10));
                            STACK_CHECK(!memcmp(image + 32 + index * 16, b, 10));
                        }
                        STACK_CHECK(test_u32(data) == 0 && test_u32(data + 4) == 0 &&
                                    test_u32(data + 8) == 0 && test_u32(data + 12) == 0);
                    }
                    if (!missing) break;
                }
        }
    serial_puts("[DOS-FPU-STACK] mode="); serial_putdec(mode);
    serial_puts(" checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures);
    serial_puts(" sampled="); serial_putdec(failures < 12 ? failures : 12); serial_puts("\n");
    *total += checks;
#undef STACK_CHECK
    return failures;
}

static void fpu_test_numeric_value(uint8_t *p, unsigned kind)
{
    test_w32(p, fpn_values[kind].significand);
    test_w32(p + 4, fpn_values[kind].significand >> 32);
    test_w16(p + 8, fpn_values[kind].exponent);
}

static unsigned fpu_test_numeric(dos_vm_t *vm, unsigned mode, unsigned *total)
{
    uint8_t *image = vm->native_fp->image, *data = vm->mem + 0x4000;
    unsigned checks = 0, failures = 0;
    for (unsigned ci = 0; ci < sizeof(fpn_cases) / sizeof(fpn_cases[0]); ci++) {
        const struct fpn_case *c = &fpn_cases[ci];
        unsigned case_failures = 0;
        for (unsigned top = 0; top < 8; top++) for (unsigned rc = 0; rc < 4; rc++)
            for (unsigned masked = 0; masked < 2; masked++) {
#define NUM_CHECK(condition) do { \
    checks++; if (!(condition)) { failures++; case_failures++; if (case_failures <= 2) { \
        serial_puts("[DOS-FPU-NUMERIC] FAIL line="); serial_putdec(__LINE__); \
        serial_puts(" mode="); serial_putdec(mode); serial_puts(" case="); serial_puts(c->name); \
        serial_puts(" top="); serial_putdec(top); serial_puts(" rc="); serial_putdec(rc); \
        serial_puts(" masked="); serial_putdec(masked); serial_puts("\n"); \
    } } \
} while (0)
                cpu_event_fault_t fault = fpu_test_step(vm, 0xdb, 0xe3, 0, 0);
                NUM_CHECK(!fault.raised);
                uint16_t control = (0x037f & ~(masked ? 0 : c->exception)) | (rc << 10);
                unsigned next = (top + 7) & 7u, tags = 0xffu & ~(1u << next);
                fpu_test_seed_stack(image, top, tags, control);
                test_w16(image + 2, test_u16(image + 2) | 0x4500u);
                fpu_test_numeric_value(image + 32, c->a);
                fpu_test_numeric_value(image + 48, c->b);
                memset(data, 0xa5, 16);
                if (c->memory) {
                    memset(data, 0, 10);
                    if (c->memory == FPN_SNAN32) test_w32(data, 0x7fa00001);
                    if (c->memory == FPN_SNAN64) { test_w32(data, 1); test_w32(data + 4, 0x7ff40000); }
                    if (c->memory == FPN_DENORMAL32 || c->memory == FPN_DENORMAL64) data[0] = 1;
                    if (c->memory == FPN_SNAN80) fpu_test_numeric_value(data, FPN_SNAN);
                    if (c->memory == FPN_DENORMAL80) fpu_test_numeric_value(data, FPN_DENORMAL);
                }
                uint8_t prior[160], memory[16];
                memcpy(prior, image, sizeof(prior)); memcpy(memory, data, sizeof(memory));
                vm->cpu->eflags = FLAGS_FIXED | FLAG_OF | FLAG_SF | FLAG_AF | FLAG_PF | FLAG_ZF;
                fault = fpu_test_step(vm, c->opcode, c->modrm, 0x4000, 0);
                NUM_CHECK(!fault.raised);
                NUM_CHECK(test_u16(image) == control);
                unsigned expected_exception = c->exception;
                if (masked && c->opcode == 0xd9 && c->modrm == 0xfd && c->exception == 16)
                    expected_exception = 0; /* The tiny FSCALE result is exact. */
                uint16_t status = test_u16(image + 2);
                NUM_CHECK((status & 0x5fu) == (expected_exception & 0x1fu) &&
                          (!(expected_exception & 32u) || (status & 32u)));
                bool pending = !masked && c->exception;
                NUM_CHECK((status & 0x8080u) == (pending ? 0x8080u : 0));
                bool inhibited = pending && ((c->exception & 5u) ||
                    (c->exception == 2 && c->effect != FPN_PUSH) ||
                    ((c->exception & 24u) && c->effect == FPN_STORE_POP));
                unsigned pops = c->effect == FPN_COMPARE_POP2 ? 2 :
                    c->effect == FPN_POP || c->effect == FPN_COMPARE_POP ||
                    c->effect == FPN_FLAGS_POP || c->effect == FPN_STORE_POP ||
                    c->effect == FPN_STORE_REG_POP;
                bool push = c->effect == FPN_PUSH || c->effect == FPN_PAIR;
                unsigned expected_top = top, expected_tags = tags, changed = 0;
                if (!inhibited) {
                    if (push) { expected_top = next; expected_tags |= 1u << next; changed |= 1u << next; }
                    if (c->effect == FPN_DEST0 || c->effect == FPN_PAIR) changed |= 1u << top;
                    if (c->effect == FPN_POP || c->effect == FPN_STORE_REG_POP) changed |= 1u << ((top + 1) & 7u);
                    for (unsigned i = 0; i < pops; i++) expected_tags &= ~(1u << ((top + i) & 7u));
                    if (pops) expected_top = (top + pops) & 7u;
                }
                NUM_CHECK(image[4] == expected_tags && (status & 0x3800u) == expected_top << 11);
                NUM_CHECK(fpu_test_same_registers(image, prior, expected_tags & ~changed));
                if (c->effect == FPN_FLAGS_POP) {
                    unsigned flags = c->exception == 1 ? (masked ? 0x45u : 0x44u) : 1u;
                    NUM_CHECK((vm->cpu->eflags & 0x8d5u) == flags);
                } else NUM_CHECK((vm->cpu->eflags & 0x8d5u) == 0x8d4u);
                if (c->effect != FPN_STORE_POP || inhibited)
                    NUM_CHECK(!memcmp(data, memory, 16));
                else {
                    NUM_CHECK(!memcmp(data + 4, memory + 4, 12));
                    NUM_CHECK(memcmp(data, memory, 4));
                }
                if (!inhibited && c->effect == FPN_PUSH && c->exception == 2) {
                    NUM_CHECK(test_u16(image + 40) == (c->memory == FPN_DENORMAL32 ? 0x3f6a : 0x3bcd));
                    NUM_CHECK(test_u32(image + 32) == 0 && test_u32(image + 36) == 0x80000000u);
                }
                if (!masked && (c->exception & 24u) && c->effect != FPN_STORE_POP) {
                    unsigned exponent;
                    if (c->opcode == 0xde)
                        exponent = c->modrm == 0xc9 ? (c->exception == 8 ? 0x5ffd : 0x2003) :
                                   c->modrm == 0xc1 ? 0x1fff : 0x5ffc;
                    else exponent = c->b == FPN_SCALE_LARGE ? 0x7fff : c->exception == 8 ? 0x1fff : 0x6000;
                    NUM_CHECK(test_u16(image + 40) == exponent &&
                              test_u32(image + 32) == 0 && test_u32(image + 36) == 0x80000000u);
                }
                /* The faulting operation retires; WAIT owns the deferred #MF. */
                uint8_t after[160]; memcpy(after, image, sizeof(after));
                fault = fpu_test_step(vm, 0x9b, 0, 0, 0);
                NUM_CHECK(pending ? fault.raised && fault.vector == 16 && fault.return_eip == 0x1000 : !fault.raised);
                NUM_CHECK(!memcmp(image, after, sizeof(after)));
#undef NUM_CHECK
            }
        if (case_failures) {
            serial_puts("[DOS-FPU-NUMERIC] case="); serial_puts(c->name);
            serial_puts(" failures="); serial_putdec(case_failures); serial_puts("\n");
        }
    }
    serial_puts("[DOS-FPU-NUMERIC] mode="); serial_putdec(mode);
    serial_puts(" checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    *total += checks;
    return failures;
}

/* Exact tiny inputs exercise direction, biased underflow and both result
 * lanes without depending on a host CPU's transcendental approximation. */
static unsigned fpu_test_trig(dos_vm_t *vm, unsigned mode, unsigned *total)
{
    static const struct { uint64_t sig; uint16_t exp; } inputs[] = {
        {0,0}, {0x8000000000000000ull,0x3fb9}, {0x4000000000000000ull,0},
        {0x8000000000000000ull,0x7fff}, {0xa000000000000123ull,0x7fff},
        {0xc000000000000123ull,0x7fff}, {0x4000000000000000ull,0x3fff},
        {0x8000000000000000ull,0x403e}
    };
    static const unsigned ops[] = {0xf2,0xfb,0xfe,0xff}, pcs[] = {0,2,3};
    unsigned checks = 0, failures = 0;
    uint8_t *image = vm->native_fp->image, *data = vm->mem + 0x4000;
    for (unsigned ci = 0; ci < sizeof(inputs) / sizeof(inputs[0]); ci++)
    for (unsigned oi = 0; oi < 4; oi++) for (unsigned top = 0; top < 8; top++)
    for (unsigned sign = 0; sign < 2; sign++) for (unsigned rc = 0; rc < 4; rc++)
    for (unsigned pc = 0; pc < 3; pc++) for (unsigned mask = 0; mask < 4; mask++) {
#define TRIG_CHECK(condition) do { \
    checks++; if (!(condition)) { failures++; if (failures <= 12) { \
        serial_puts("[DOS-FPU-TRIG] FAIL line="); serial_putdec(__LINE__); \
        serial_puts(" mode="); serial_putdec(mode); serial_puts(" case="); serial_putdec(ci); \
        serial_puts(" op="); serial_putdec(ops[oi]); serial_puts(" top="); serial_putdec(top); \
        serial_puts(" sign="); serial_putdec(sign); serial_puts(" rc="); serial_putdec(rc); \
        serial_puts(" mask="); serial_putdec(mask); serial_puts("\n"); } } \
} while (0)
        cpu_event_fault_t fault = fpu_test_step(vm, 0xdb, 0xe3, 0, 0);
        TRIG_CHECK(!fault.raised);
        /* Mask all, unmask all, or isolate underflow/precision completion. */
        unsigned masks[] = {0x3f,0,0x2f,0x1f};
        uint16_t control = 0x40u | masks[mask] | (pcs[pc] << 8) | (rc << 10);
        unsigned next = (top + 7) & 7u, tags = 0xffu & ~(1u << next);
        fpu_test_seed_stack(image, top, tags, control);
        test_w32(image + 32, inputs[ci].sig);
        test_w32(image + 36, inputs[ci].sig >> 32);
        test_w16(image + 40, inputs[ci].exp | (sign << 15));
        test_w16(image + 2, test_u16(image + 2) | 0x4500u);
        memset(data, 0xa5, 16);
        uint8_t before[160]; memcpy(before, image, sizeof(before));
        vm->cpu->eflags = FLAGS_FIXED | FLAG_OF | FLAG_SF | FLAG_AF | FLAG_PF | FLAG_ZF;
        fault = fpu_test_step(vm, 0xd9, ops[oi], 0x4000, 0);
        TRIG_CHECK(!fault.raised);
        bool pair = oi < 2, invalid = ci == 3 || ci == 4 || ci == 6;
        bool tiny = ci == 1 || ci == 2, outside = ci == 7;
        bool away = rc == (sign ? 1u : 2u);
        bool rounded_up = tiny && (oi == 0 ? away : oi == 3 ? rc == 0 || rc == 2 : rc == 0 || away);
        unsigned raised = invalid ? 1u : tiny ? 32u : 0;
        if (ci == 2) raised |= 2u | (oi == 3 ? 0 : 16u);
        if ((raised & 2u) && !(control & 2u)) { raised = 2; rounded_up = false; }
        bool pending = (raised & ~control) != 0;
        bool inhibited = (raised & ~control & 3u) != 0;
        unsigned new_top = top, changed = 0, expected_tags = tags;
        if (!inhibited && !outside) {
            changed = 1u << top;
            if (pair) { new_top = next; expected_tags |= 1u << next; changed |= 1u << next; }
        }
        uint16_t status = test_u16(image + 2);
        TRIG_CHECK(test_u16(image) == control);
        TRIG_CHECK((status & 0x87ffu) == (0x0100u | raised | (rounded_up ? 0x200u : 0) |
                    (outside ? 0x400u : 0) | (pending ? 0x8080u : 0)));
        TRIG_CHECK((status & 0x3800u) == new_top << 11 && image[4] == expected_tags);
        TRIG_CHECK(fpu_test_same_registers(image, before, expected_tags & ~changed));
        TRIG_CHECK((vm->cpu->eflags & 0x8d5u) == 0x8d4u);
        TRIG_CHECK(test_u32(data) == 0xa5a5a5a5u && test_u32(data + 4) == 0xa5a5a5a5u &&
                   test_u32(data + 8) == 0xa5a5a5a5u && test_u32(data + 12) == 0xa5a5a5a5u);
        TRIG_CHECK(test_u32(image + 8) == 0x1000 && test_u16(image + 6) == (0x100u | ops[oi]));
        if (!inhibited && !outside) for (unsigned lane = 0; lane < (pair ? 2u : 1u); lane++) {
            uint64_t sig = inputs[ci].sig;
            unsigned exp = inputs[ci].exp, negative = sign;
            bool cosine = oi == 3 || (oi == 1 && lane == 1);
            bool one = oi == 0 && lane == 1;
            if (invalid || ci == 5) {
                sig = ci == 4 || ci == 5 ? inputs[ci].sig | (1ull << 62) : 0xc000000000000000ull;
                exp = 0x7fff;
                negative = ci == 4 || ci == 5 ? sign : 1;
            } else if (one || cosine) {
                sig = 0x8000000000000000ull; exp = 0x3fff; negative = 0;
                if (cosine && tiny && rc != 0 && rc != 2) { exp--; sig = UINT64_MAX; }
            } else if (tiny) {
                bool gradual = ci == 2 && (control & 16u);
                sig = gradual ? 0x4000000000000000ull : 0x8000000000000000ull;
                exp = ci == 1 ? 0x3fb9 : gradual ? 0 : 0x6000;
                if (oi == 0 && away) sig++;
                if (oi != 0 && rc != 0 && !away) {
                    sig--;
                    if (!gradual) { sig = UINT64_MAX; exp--; }
                }
            }
            unsigned index = pair ? 1 - lane : 0;
            uint8_t *out = image + 32 + index * 16;
            TRIG_CHECK(test_u32(out) == (uint32_t)sig && test_u32(out + 4) == sig >> 32 &&
                       test_u16(out + 8) == (exp | (negative << 15)));
        }
        uint8_t after[160]; memcpy(after, image, sizeof(after));
        fault = fpu_test_step(vm, 0x9b, 0, 0, 0);
        TRIG_CHECK(pending ? fault.raised && fault.vector == 16 && fault.return_eip == 0x1000 : !fault.raised);
        TRIG_CHECK(!memcmp(after, image, sizeof(after)));
#undef TRIG_CHECK
    }
    serial_puts("[DOS-FPU-TRIG] mode="); serial_putdec(mode);
    serial_puts(" checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    *total += checks;
    return failures;
}

static unsigned fpu_test_elementary(dos_vm_t *vm, unsigned mode, unsigned *total)
{
    enum { VALUE, PI_VALUE, TINY_ATAN, POWER_SUBNORMAL, POWER_OVERFLOW };
    static const struct {
        const char *name;
        unsigned op, kind, flags;
        struct { uint64_t sig; uint16_t exp; } a, b, value;
    } cases[] = {
        {"exp-zero",0xf0,VALUE,0,{0,0},{0,1},{0,0}},
        {"exp-negative-zero",0xf0,VALUE,0,{0,0x8000},{0,1},{0,0x8000}},
        {"exp-one",0xf0,VALUE,32,{1ull<<63,0x3fff},{0,1},{1ull<<63,0x3fff}},
        {"exp-minus-one",0xf0,VALUE,32,{1ull<<63,0xbfff},{0,1},{1ull<<63,0xbffe}},
        {"exp-minus-infinite",0xf0,VALUE,0,{1ull<<63,0xffff},{0,1},{1ull<<63,0xbfff}},
        {"exp-snan",0xf0,VALUE,1,{0xa000000000000123ull,0xffff},{0,1},{0xe000000000000123ull,0xffff}},
        {"exp-qnan-unused-snan",0xf0,VALUE,0,{0xc000000000000123ull,0x7fff},
            {0xa000000000000456ull,0x7fff},{0xc000000000000123ull,0x7fff}},
        {"exp-unsupported",0xf0,VALUE,1,{0,1},{0,0},{0xc000000000000000ull,0xffff}},
        {"log-zero",0xf1,VALUE,4,{0,0},{1ull<<63,0x3fff},{1ull<<63,0xffff}},
        {"log-zero-infinite",0xf1,VALUE,0,{0,0},{1ull<<63,0x7fff},{1ull<<63,0xffff}},
        {"log-negative",0xf1,VALUE,1,{1ull<<63,0xbfff},{0,0},{0xc000000000000000ull,0xffff}},
        {"log-one-denormal",0xf1,VALUE,2,{1ull<<63,0x3fff},{1,0x8000},{0,0x8000}},
        {"log-positive-zero",0xf1,VALUE,0,{1ull<<63,0x4000},{0,0},{0,0}},
        {"log-negative-zero",0xf1,VALUE,0,{1ull<<63,0x3ffe},{0,0},{0,0x8000}},
        {"log-snan-qnan",0xf1,VALUE,1,{0xa000000000000456ull,0x7fff},
            {0xc000000000000123ull,0xffff},{0xc000000000000123ull,0xffff}},
        {"log-subnormal",0xf1,POWER_SUBNORMAL,50,{1ull<<63,0x4000},{1ull<<62,0},{1ull<<62,0}},
        {"log-smallest",0xf1,POWER_SUBNORMAL,50,{1ull<<63,0x4000},{1,0},{1,0}},
        {"log-boundary",0xf1,VALUE,34,{1ull<<63,0x3ffd},{1ull<<62,0},{1ull<<63,0x8001}},
        {"log-overflow",0xf1,POWER_OVERFLOW,40,{1ull<<63,0x4001},{UINT64_MAX,0x7ffe},{UINT64_MAX,0x7fff}},
        {"log-negative-overflow",0xf1,POWER_OVERFLOW,40,{1ull<<63,0x4001},{UINT64_MAX,0xfffe},{UINT64_MAX,0xffff}},
        {"logp1-zero",0xf9,VALUE,0,{0,0},{1ull<<63,0x3fff},{0,0}},
        {"logp1-negative-zero",0xf9,VALUE,0,{0,0x8000},{1ull<<63,0x3fff},{0,0x8000}},
        {"logp1-zero-infinite",0xf9,VALUE,1,{0,0},{1ull<<63,0x7fff},{0xc000000000000000ull,0xffff}},
        {"logp1-infinite",0xf9,VALUE,0,{1ull<<63,0x3ffd},{1ull<<63,0x7fff},{1ull<<63,0x7fff}},
        {"logp1-negative-infinite",0xf9,VALUE,0,{1ull<<63,0xbffd},{1ull<<63,0x7fff},{1ull<<63,0xffff}},
        {"logp1-denormal-zero",0xf9,VALUE,2,{1,0},{0,0x8000},{0,0x8000}},
        {"atan-zero-zero",0xf3,VALUE,0,{0,0},{0,0},{0,0}},
        {"atan-negative-zero",0xf3,VALUE,0,{0,0},{0,0x8000},{0,0x8000}},
        {"atan-pi",0xf3,PI_VALUE,32,{0,0x8000},{0,0},{0xc90fdaa22168c234ull,0x4000}},
        {"atan-minus-pi",0xf3,PI_VALUE,32,{0,0x8000},{0,0x8000},{0xc90fdaa22168c234ull,0xc000}},
        {"atan-pi2",0xf3,PI_VALUE,32,{0,0},{1ull<<63,0x3fff},{0xc90fdaa22168c234ull,0x3fff}},
        {"atan-infinite-pair",0xf3,PI_VALUE,32,{1ull<<63,0x7fff},{1ull<<63,0x7fff},
            {0xc90fdaa22168c234ull,0x3ffe}},
        {"atan-infinite-x",0xf3,VALUE,0,{1ull<<63,0x7fff},{1ull<<63,0x3fff},{0,0}},
        {"atan-boundary",0xf3,TINY_ATAN,32,{1ull<<63,0x3fff},{1ull<<63,1},{1ull<<63,1}},
        {"atan-negative-boundary",0xf3,TINY_ATAN,32,{1ull<<63,0x3fff},{1ull<<63,0x8001},{1ull<<63,0x8001}},
        {"atan-subnormal",0xf3,TINY_ATAN,32,{1ull<<63,0x4000},{1ull<<63,1},{1ull<<62,0}},
        {"atan-smallest",0xf3,TINY_ATAN,34,{1ull<<63,0x3fff},{1,0},{1,0}},
        {"atan-tiny",0xf3,TINY_ATAN,32,{1ull<<63,0x3fff},{1ull<<63,0x3fb8},{1ull<<63,0x3fb8}},
        {"atan-tiny-boundary",0xf3,TINY_ATAN,32,{1ull<<63,0x3fff},{1ull<<63,0x3fb9},{1ull<<63,0x3fb9}},
        {"atan-exact-ratio",0xf3,TINY_ATAN,32,{1ull<<63,0x3fff},{1ull<<63,0x3fd7},{1ull<<63,0x3fd7}}
    };
    static const unsigned pcs[] = {0,2,3}, masks[] = {63,0,62,61,59,55,47,31};
    unsigned checks = 0, failures = 0;
    uint8_t *image = vm->native_fp->image, *data = vm->mem + 0x4000;
    for (unsigned ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++)
    for (unsigned top = 0; top < 8; top++) for (unsigned rc = 0; rc < 4; rc++)
    for (unsigned pc = 0; pc < 3; pc++) for (unsigned mask = 0; mask < 8; mask++) {
#define ELEMENTARY_CHECK(condition) do { \
    checks++; if (!(condition)) { failures++; if (failures <= 12) { \
        serial_puts("[DOS-FPU-ELEMENTARY] FAIL line="); serial_putdec(__LINE__); \
        serial_puts(" mode="); serial_putdec(mode); serial_puts(" case="); serial_puts(cases[ci].name); \
        serial_puts(" top="); serial_putdec(top); serial_puts(" rc="); serial_putdec(rc); \
        serial_puts(" mask="); serial_putdec(mask); serial_puts("\n"); } } \
} while (0)
        cpu_event_fault_t fault = fpu_test_step(vm, 0xdb, 0xe3, 0, 0);
        ELEMENTARY_CHECK(!fault.raised);
        uint16_t control = 0x40u | masks[mask] | (pcs[pc] << 8) | (rc << 10);
        unsigned tags = 0xffu & ~(1u << ((top + 7) & 7u));
        fpu_test_seed_stack(image, top, tags, control);
        test_w32(image + 32, cases[ci].a.sig); test_w32(image + 36, cases[ci].a.sig >> 32);
        test_w16(image + 40, cases[ci].a.exp);
        test_w32(image + 48, cases[ci].b.sig); test_w32(image + 52, cases[ci].b.sig >> 32);
        test_w16(image + 56, cases[ci].b.exp);
        test_w16(image + 2, (top << 11) | 0x4700u);
        memset(data, 0xa5, 16);
        uint8_t before[160]; memcpy(before, image, sizeof(before));
        vm->cpu->eflags = FLAGS_FIXED | FLAG_OF | FLAG_SF | FLAG_AF | FLAG_PF | FLAG_ZF;
        fault = fpu_test_step(vm, 0xd9, cases[ci].op, 0x4000, 0);
        ELEMENTARY_CHECK(!fault.raised);
        uint64_t sig = cases[ci].value.sig;
        int exp = cases[ci].value.exp & 0x7fff;
        unsigned sign = cases[ci].value.exp >> 15, raised = cases[ci].flags;
        bool up = false, away = rc == (sign ? 1u : 2u);
        if (cases[ci].kind == PI_VALUE) { up = rc == 0 || away; sig += up; }
        if (cases[ci].kind == POWER_OVERFLOW) {
            if (!(control & 8)) exp -= 24576;
            else if (rc == 0 || away) { sig = 1ull << 63; up = true; }
            else exp--;
        }
        if (cases[ci].kind == TINY_ATAN || cases[ci].kind == POWER_SUBNORMAL) {
            uint64_t original = sig;
            bool subnormal = !exp;
            if (!exp) { exp = 1; while (!(sig >> 63)) { sig <<= 1; exp--; } }
            if (cases[ci].kind == TINY_ATAN) {
                up = rc == 0 || away;
                if (!up) { sig = UINT64_MAX; exp--; }
                if (exp <= 0) raised |= 16;
            }
            if (exp <= 0) {
                if (!(control & 16)) exp += 24576;
                else {
                    exp = 0;
                    sig = (subnormal ? original : 1ull << 63) -
                          (cases[ci].kind == TINY_ATAN && !up);
                }
            }
        }
        if ((raised & 2) && !(control & 2)) { raised = 2; up = false; }
        bool pending = (raised & ~control) != 0, inhibited = (raised & ~control & 7) != 0;
        bool pop = cases[ci].op != 0xf0 && !inhibited;
        unsigned new_top = (top + pop) & 7u;
        unsigned expected_tags = tags & ~(pop ? 1u << top : 0);
        unsigned changed = inhibited ? 0 : 1u << new_top;
        uint16_t status = test_u16(image + 2);
        ELEMENTARY_CHECK(test_u16(image) == control);
        ELEMENTARY_CHECK((status & 0xc7ffu) == (0x4500u | raised | (up ? 0x200u : 0) |
                          (pending ? 0x8080u : 0)));
        ELEMENTARY_CHECK((status & 0x3800u) == new_top << 11 && image[4] == expected_tags);
        ELEMENTARY_CHECK(fpu_test_same_registers(image, before, expected_tags & ~changed));
        ELEMENTARY_CHECK((vm->cpu->eflags & 0x8d5u) == 0x8d4u);
        ELEMENTARY_CHECK(test_u32(data) == 0xa5a5a5a5u && test_u32(data + 4) == 0xa5a5a5a5u &&
                          test_u32(data + 8) == 0xa5a5a5a5u && test_u32(data + 12) == 0xa5a5a5a5u);
        ELEMENTARY_CHECK(test_u32(image + 8) == 0x1000 && test_u16(image + 6) == (0x100u | cases[ci].op));
        if (!inhibited) ELEMENTARY_CHECK(test_u32(image + 32) == (uint32_t)sig &&
            test_u32(image + 36) == sig >> 32 && test_u16(image + 40) == (exp | (sign << 15)));
        uint8_t after[160]; memcpy(after, image, sizeof(after));
        fault = fpu_test_step(vm, 0x9b, 0, 0, 0);
        ELEMENTARY_CHECK(pending ? fault.raised && fault.vector == 16 && fault.return_eip == 0x1000 : !fault.raised);
        ELEMENTARY_CHECK(!memcmp(image, after, sizeof(after)));
#undef ELEMENTARY_CHECK
    }
    serial_puts("[DOS-FPU-ELEMENTARY] mode="); serial_putdec(mode);
    serial_puts(" checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    *total += checks;
    return failures;
}

static unsigned fpu_test_reduce(dos_vm_t *vm, unsigned mode, unsigned *total)
{
    enum { PARTIAL = 0x80 };
    static const struct {
        const char *name;
        unsigned op, flags, quotient;
        struct { uint64_t sig; uint16_t exp; } a, b, value, pushed;
    } cases[] = {
        {"extract-one",0xf4,0,0,{1ull<<63,0x3fff},{0,1},{0,0},{1ull<<63,0x3fff}},
        {"extract-minus-six",0xf4,0,0,{0xc000000000000000ull,0xc001},{0,1},
            {1ull<<63,0x4000},{0xc000000000000000ull,0xbfff}},
        {"extract-half",0xf4,0,0,{1ull<<63,0x3ffe},{0,1},{1ull<<63,0xbfff},{1ull<<63,0x3fff}},
        {"extract-zero",0xf4,4,0,{0,0},{0,1},{1ull<<63,0xffff},{0,0}},
        {"extract-minus-zero",0xf4,4,0,{0,0x8000},{0,1},{1ull<<63,0xffff},{0,0x8000}},
        {"extract-infinite",0xf4,0,0,{1ull<<63,0xffff},{0,1},{1ull<<63,0x7fff},{1ull<<63,0xffff}},
        {"extract-smallest",0xf4,2,0,{1,0},{0,1},{0x807a000000000000ull,0xc00d},{1ull<<63,0x3fff}},
        {"extract-pseudo",0xf4,2,0,{1ull<<63,0},{0,1},{0xfff8000000000000ull,0xc00c},{1ull<<63,0x3fff}},
        {"extract-snan",0xf4,1,0,{0xa000000000000123ull,0xffff},{0,1},
            {0xe000000000000123ull,0xffff},{0xe000000000000123ull,0xffff}},
        {"extract-qnan-unused-snan",0xf4,0,0,{0xc000000000000123ull,0x7fff},
            {0xa000000000000456ull,0x7fff},{0xc000000000000123ull,0x7fff},{0xc000000000000123ull,0x7fff}},
        {"extract-unsupported",0xf4,1,0,{0,1},{0,0},
            {0xc000000000000000ull,0xffff},{0xc000000000000000ull,0xffff}},
        {"rem-zero",0xf8,0,0,{0,0x8000},{1ull<<63,0x3fff},{0,0x8000},{0,0}},
        {"rem-tie-even",0xf5,0,2,{0xa000000000000000ull,0x4001},{1ull<<63,0x4000},{1ull<<63,0x3fff},{0,0}},
        {"rem-tie-odd",0xf5,0,4,{0xe000000000000000ull,0x4001},{1ull<<63,0x4000},{1ull<<63,0xbfff},{0,0}},
        {"rem-truncate",0xf8,0,3,{0xe000000000000000ull,0x4001},{1ull<<63,0x4000},{1ull<<63,0x3fff},{0,0}},
        {"rem-negative",0xf8,0,2,{0xa000000000000000ull,0xc001},{1ull<<63,0x4000},{1ull<<63,0xbfff},{0,0}},
        {"rem-negative-divisor",0xf5,0,2,{0xa000000000000000ull,0x4001},{1ull<<63,0xc000},{1ull<<63,0x3fff},{0,0}},
        {"rem-quotient-seven",0xf8,0,7,{0xf000000000000000ull,0x4002},{1ull<<63,0x4000},{1ull<<63,0x3fff},{0,0}},
        {"rem-quotient-wrap",0xf5,0,0,{0xf000000000000000ull,0x4002},{1ull<<63,0x4000},{1ull<<63,0xbfff},{0,0}},
        {"rem-tiny-infinite",0xf8,2,0,{1,0},{1ull<<63,0x7fff},{1,0},{0,0}},
        {"rem1-tiny-infinite",0xf5,2,0,{1,0x8000},{1ull<<63,0x7fff},{1,0x8000},{0,0}},
        {"rem-tiny-one",0xf8,2,0,{1,0},{1ull<<63,0x3fff},{1,0},{0,0}},
        {"rem1-tiny-one",0xf5,2,0,{1,0},{1ull<<63,0x3fff},{1,0},{0,0}},
        {"rem-tiny-divisor",0xf8,2,0,{3,0},{4,0},{3,0},{0,0}},
        {"rem1-tiny-divisor",0xf5,2,1,{3,0},{4,0},{1,0x8000},{0,0}},
        {"rem-exact-tiny",0xf8,2,2,{4,0},{2,0},{0,0},{0,0}},
        {"rem-zero-divisor",0xf8,1,0,{1ull<<63,0x3fff},{0,0},{0xc000000000000000ull,0xffff},{0,0}},
        {"rem-infinite-dividend",0xf5,1,0,{1ull<<63,0x7fff},{1ull<<63,0x3fff},
            {0xc000000000000000ull,0xffff},{0,0}},
        {"rem-qnan-denormal",0xf8,0,0,{0xc000000000000123ull,0xffff},{1,0},
            {0xc000000000000123ull,0xffff},{0,0}},
        {"rem-snan-qnan",0xf5,1,0,{0xa000000000000456ull,0x7fff},{0xc000000000000123ull,0xffff},
            {0xc000000000000123ull,0xffff},{0,0}},
        {"rem-unsupported",0xf8,1,0,{0,1},{1ull<<63,0x3fff},{0xc000000000000000ull,0xffff},{0,0}},
        {"rem-partial",0xf8,PARTIAL,2,{1ull<<63,0x4040},{0xc000000000000000ull,0x4000},
            {1ull<<63,0x4000},{0,0}},
        {"rem1-partial",0xf5,PARTIAL,3,{1ull<<63,0x4040},{0xc000000000000000ull,0x4000},
            {1ull<<63,0xbfff},{0,0}}
    };
    static const unsigned pcs[] = {0,2,3};
    unsigned checks = 0, failures = 0;
    uint8_t *image = vm->native_fp->image, *data = vm->mem + 0x4000;
    for (unsigned ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++)
    for (unsigned top = 0; top < 8; top++) for (unsigned rc = 0; rc < 4; rc++)
    for (unsigned pc = 0; pc < 3; pc++) for (unsigned mask = 0; mask < 64; mask++) {
#define REDUCE_CHECK(condition) do { \
    checks++; if (!(condition)) { failures++; if (failures <= 12) { \
        serial_puts("[DOS-FPU-REDUCE] FAIL line="); serial_putdec(__LINE__); \
        serial_puts(" mode="); serial_putdec(mode); serial_puts(" case="); serial_puts(cases[ci].name); \
        serial_puts(" top="); serial_putdec(top); serial_puts(" rc="); serial_putdec(rc); \
        serial_puts(" mask="); serial_putdec(mask); serial_puts("\n"); } } \
} while (0)
        cpu_event_fault_t fault = fpu_test_step(vm, 0xdb, 0xe3, 0, 0);
        REDUCE_CHECK(!fault.raised);
        uint16_t control = 0x40u | mask | (pcs[pc] << 8) | (rc << 10);
        unsigned next = (top + 7) & 7u, tags = 0xffu & ~(1u << next);
        fpu_test_seed_stack(image, top, tags, control);
        test_w32(image + 32, cases[ci].a.sig); test_w32(image + 36, cases[ci].a.sig >> 32);
        test_w16(image + 40, cases[ci].a.exp);
        test_w32(image + 48, cases[ci].b.sig); test_w32(image + 52, cases[ci].b.sig >> 32);
        test_w16(image + 56, cases[ci].b.exp);
        test_w16(image + 2, (top << 11) | 0x4700u);
        memset(data, 0xa5, 16);
        uint8_t before[160]; memcpy(before, image, sizeof(before));
        vm->cpu->eflags = FLAGS_FIXED | FLAG_OF | FLAG_SF | FLAG_AF | FLAG_PF | FLAG_ZF;
        fault = fpu_test_step(vm, 0xd9, cases[ci].op, 0x4000, 0);
        REDUCE_CHECK(!fault.raised);
        if (cases[ci].flags & PARTIAL) {
            REDUCE_CHECK(test_u16(image + 2) & 0x400u);
            unsigned previous = cases[ci].a.exp & 0x7fffu, steps = 0;
            while ((test_u16(image + 2) & 0x400u) && steps++ < 1030) {
                unsigned exponent = test_u16(image + 40) & 0x7fffu;
                REDUCE_CHECK(!exponent || previous >= exponent + 32);
                REDUCE_CHECK(!(test_u16(image + 2) & 0x80ffu) && image[4] == tags);
                REDUCE_CHECK(fpu_test_same_registers(image, before, tags & ~(1u << top)));
                previous = exponent;
                fault = fpu_test_step(vm, 0xd9, cases[ci].op, 0x4000, 0);
                REDUCE_CHECK(!fault.raised);
            }
            REDUCE_CHECK(!(test_u16(image + 2) & 0x400u));
        }
        bool extract = cases[ci].op == 0xf4;
        unsigned raised = cases[ci].flags & 63u;
        uint64_t sig = cases[ci].value.sig;
        int exp = cases[ci].value.exp & 0x7fff;
        unsigned sign = cases[ci].value.exp & 0x8000u;
        if (!extract && !exp && sig && !(control & 16)) {
            raised |= 16;
            exp = 1;
            while (!(sig >> 63)) { sig <<= 1; exp--; }
            exp += 24576;
        }
        if ((raised & 2) && !(control & 2)) raised = 2;
        bool pending = (raised & ~control) != 0, inhibited = (raised & ~control & 7) != 0;
        bool push = extract && !inhibited;
        unsigned new_top = push ? next : top, expected_tags = tags | (push ? 1u << next : 0);
        unsigned changed = inhibited ? 0 : (1u << top) | (push ? 1u << next : 0);
        bool unordered = cases[ci].value.exp == 0xffff || cases[ci].value.exp == 0x7fff;
        unsigned q = cases[ci].quotient;
        unsigned condition = extract ? 0x4500u : inhibited || unordered ? 0x4100u :
                             ((q & 1u) << 9) | ((q & 2u) << 13) | ((q & 4u) << 6);
        REDUCE_CHECK(test_u16(image) == control);
        REDUCE_CHECK((test_u16(image + 2) & 0xc7ffu) == (condition | raised | (pending ? 0x8080u : 0)));
        REDUCE_CHECK((test_u16(image + 2) & 0x3800u) == new_top << 11 && image[4] == expected_tags);
        REDUCE_CHECK(fpu_test_same_registers(image, before, expected_tags & ~changed));
        REDUCE_CHECK((vm->cpu->eflags & 0x8d5u) == 0x8d4u);
        REDUCE_CHECK(test_u32(data) == 0xa5a5a5a5u && test_u32(data + 4) == 0xa5a5a5a5u &&
                     test_u32(data + 8) == 0xa5a5a5a5u && test_u32(data + 12) == 0xa5a5a5a5u);
        REDUCE_CHECK(test_u32(image + 8) == 0x1000 && test_u16(image + 6) == (0x100u | cases[ci].op));
        if (!inhibited) {
            const uint8_t *value = image + (push ? 48 : 32);
            REDUCE_CHECK(test_u32(value) == (uint32_t)sig && test_u32(value + 4) == sig >> 32 &&
                         test_u16(value + 8) == (exp | sign));
            if (push) REDUCE_CHECK(test_u32(image + 32) == (uint32_t)cases[ci].pushed.sig &&
                test_u32(image + 36) == cases[ci].pushed.sig >> 32 && test_u16(image + 40) == cases[ci].pushed.exp);
        }
        uint8_t after[160]; memcpy(after, image, sizeof(after));
        fault = fpu_test_step(vm, 0x9b, 0, 0, 0);
        REDUCE_CHECK(pending ? fault.raised && fault.vector == 16 && fault.return_eip == 0x1000 : !fault.raised);
        REDUCE_CHECK(!memcmp(image, after, sizeof(after)));
#undef REDUCE_CHECK
    }
    serial_puts("[DOS-FPU-REDUCE] mode="); serial_putdec(mode);
    serial_puts(" checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    *total += checks;
    return failures;
}

static unsigned fpu_test_compare(dos_vm_t *vm, unsigned mode, unsigned *total)
{
    enum { E = 0x4000, L = 0x100, G = 0, U = 0x4500, Q = 0x40 };
    enum { ZERO, NEGZERO, ONE, TWO, NEGONE, NEGTWO, INF, NEGINF,
           TINY, PSEUDO, NORMAL, QNAN, SNAN, UNSUPPORTED };
    static const struct {
        uint64_t sig;
        uint16_t exp, classification, zero_relation, exceptions;
    } values[] = {
        {0,0,0x4000,E,0}, {0,0x8000,0x4000,E,0},
        {1ull<<63,0x3fff,0x400,G,0}, {1ull<<63,0x4000,0x400,G,0},
        {1ull<<63,0xbfff,0x400,L,0}, {1ull<<63,0xc000,0x400,L,0},
        {1ull<<63,0x7fff,0x500,G,0}, {1ull<<63,0xffff,0x500,L,0},
        {1,0,0x4400,G,2}, {1ull<<63,0,0x4400,G,2},
        {1ull<<63,1,0x400,G,0},
        {0xc000000000000123ull,0x7fff,0x100,U,Q},
        {0xa000000000000123ull,0x7fff,0x100,U,1}, {0,1,0,U,1}
    };
    static const struct {
        unsigned a, b;
        uint32_t real32;
        uint64_t real64;
        int32_t integer;
        unsigned relation, real_relation, int16_relation, int32_relation, real_exceptions;
    } cases[] = {
        {ONE,TWO,0x40000000,0x4000000000000000ull,2,L,L,L,L,0},
        {TWO,ONE,0x3f800000,0x3ff0000000000000ull,1,G,G,G,G,0},
        {ONE,ONE,0x3f800000,0x3ff0000000000000ull,1,E,E,E,E,0},
        {NEGONE,NEGTWO,0xc0000000,0xc000000000000000ull,-2,G,G,G,G,0},
        {ZERO,NEGZERO,0x80000000,0x8000000000000000ull,0,E,E,E,E,0},
        {NEGZERO,ZERO,0,0,0,E,E,E,E,0},
        {INF,INF,0x7f800000,0x7ff0000000000000ull,0x7fffffff,E,E,G,G,0},
        {NEGINF,NEGINF,0xff800000,0xfff0000000000000ull,(-2147483647-1),E,E,L,L,0},
        {TINY,ZERO,0,0,0,G,G,G,G,0},
        {PSEUDO,ZERO,0,0,0,G,G,G,G,0},
        {ONE,TINY,1,1,0,G,G,G,G,2},
        {ONE,PSEUDO,1,1,0,G,G,G,G,2},
        {QNAN,TINY,1,1,0,U,U,U,U,2},
        {SNAN,ONE,0x3f800000,0x3ff0000000000000ull,1,U,U,U,U,0},
        {UNSUPPORTED,ONE,0x3f800000,0x3ff0000000000000ull,1,U,U,U,U,0},
        {ONE,QNAN,0x7fc00123,0x7ff8000000000123ull,0,U,U,G,G,Q},
        {ONE,SNAN,0x7fa00123,0x7ff4000000000123ull,0,U,U,G,G,1},
        {ONE,UNSUPPORTED,0x7fc00123,0x7ff8000000000123ull,0,U,U,G,G,Q},
        {NORMAL,PSEUDO,0,0,0,E,G,G,G,0},
        {QNAN,SNAN,0x7fa00123,0x7ff4000000000123ull,0,U,U,U,U,1},
        {ONE,NEGONE,0xbf800000,0xbff0000000000000ull,-1,G,G,G,G,0},
        {NEGONE,ONE,0x3f800000,0x3ff0000000000000ull,1,L,L,L,L,0},
        {ZERO,ONE,0x3f800000,0x3ff0000000000000ull,(-2147483647-1),L,L,E,G,0},
        {ONE,ONE,0x3f800000,0x3ff0000000000000ull,0x7fffffff,E,E,G,L,0}
    };
    enum { REG, REAL32, REAL64, INT32, INT16 };
    struct compare_form { unsigned op, mr, memory, index, pops; bool quiet, flags, test, examine; } forms[76];
    unsigned count = 0, checks = 0, failures = 0;
    for (unsigned i = 0; i < 8; i++) {
        forms[count++] = (struct compare_form){0xd8,0xd0+i,REG,i,0,false,false,false,false};
        forms[count++] = (struct compare_form){0xd8,0xd8+i,REG,i,1,false,false,false,false};
        forms[count++] = (struct compare_form){0xdd,0xe0+i,REG,i,0,true,false,false,false};
        forms[count++] = (struct compare_form){0xdd,0xe8+i,REG,i,1,true,false,false,false};
        forms[count++] = (struct compare_form){0xdb,0xe8+i,REG,i,0,true,true,false,false};
        forms[count++] = (struct compare_form){0xdb,0xf0+i,REG,i,0,false,true,false,false};
        forms[count++] = (struct compare_form){0xdf,0xe8+i,REG,i,1,true,true,false,false};
        forms[count++] = (struct compare_form){0xdf,0xf0+i,REG,i,1,false,true,false,false};
    }
    forms[count++] = (struct compare_form){0xda,0xe9,REG,1,2,true,false,false,false};
    forms[count++] = (struct compare_form){0xde,0xd9,REG,1,2,false,false,false,false};
    forms[count++] = (struct compare_form){0xd9,0xe4,REG,0,0,false,false,true,false};
    forms[count++] = (struct compare_form){0xd9,0xe5,REG,0,0,false,false,false,true};
    static const unsigned opcodes[] = {0xd8,0xdc,0xda,0xde}, pcs[] = {0,2,3};
    for (unsigned pop = 0; pop < 2; pop++) for (unsigned memory = REAL32; memory <= INT16; memory++)
        forms[count++] = (struct compare_form){opcodes[memory-1],0x10+8*pop,memory,0,pop,false,false,false,false};
    uint8_t *image = vm->native_fp->image, *data = vm->mem + 0x4000;
    for (unsigned fi = 0; fi < count; fi++) for (unsigned ci = 0; ci < sizeof(cases)/sizeof(cases[0]); ci++)
    for (unsigned top = 0; top < 8; top++) for (unsigned rc = 0; rc < 4; rc++)
    for (unsigned pc = 0; pc < 3; pc++) for (unsigned mask = 0; mask < 4; mask++)
    for (unsigned sample = 0; sample < (forms[fi].examine ? 4u : 1u); sample++) {
#define COMPARE_CHECK(condition) do { \
    checks++; if (!(condition)) { failures++; if (failures <= 12) { \
        serial_puts("[DOS-FPU-COMPARE] FAIL line="); serial_putdec(__LINE__); \
        serial_puts(" mode="); serial_putdec(mode); serial_puts(" form="); serial_putdec(fi); \
        serial_puts(" case="); serial_putdec(ci); serial_puts(" top="); serial_putdec(top); \
        serial_puts(" rc="); serial_putdec(rc); serial_puts(" pc="); serial_putdec(pc); \
        serial_puts(" mask="); serial_putdec(mask); serial_puts(" sample="); serial_putdec(sample); \
        serial_puts("\n"); } } \
} while (0)
        const struct compare_form *f = &forms[fi];
        unsigned a = cases[ci].a, b = cases[ci].b;
        cpu_event_fault_t fault = fpu_test_step(vm, 0xdb, 0xe3, 0, 0);
        COMPARE_CHECK(!fault.raised);
        uint16_t control = 0x7cu | mask | (pcs[pc] << 8) | (rc << 10);
        unsigned tags = 0xffu & ~(f->examine && (sample & 1) ? 1u << top : 0);
        unsigned initial = ((top & 1) << 8) | ((top & 2) << 9) | ((top & 4) << 12) |
                           0x200u | (ci & 1 ? 0x20u : 0);
        fpu_test_seed_stack(image, top, tags, control);
        for (unsigned i = 0; i < 8; i++) {
            unsigned v = i ? b : a;
            test_w32(image + 32 + i*16, values[v].sig);
            test_w32(image + 36 + i*16, values[v].sig >> 32);
            test_w16(image + 40 + i*16, values[v].exp ^ (!i && (sample & 2) ? 0x8000u : 0));
        }
        test_w16(image + 2, (top << 11) | initial);
        memset(data, 0xa5, 16);
        if (f->memory == REAL32) test_w32(data, cases[ci].real32);
        else if (f->memory == REAL64) {
            test_w32(data, cases[ci].real64); test_w32(data + 4, cases[ci].real64 >> 32);
        } else if (f->memory == INT32) test_w32(data, cases[ci].integer);
        else if (f->memory == INT16) test_w16(data, cases[ci].integer);
        uint8_t before[160], memory_before[16];
        memcpy(before, image, sizeof(before)); memcpy(memory_before, data, sizeof(memory_before));
        uint32_t flags = FLAGS_FIXED | FLAG_DF | FLAG_OF | FLAG_AF | FLAG_SF |
                         (top & 1 ? FLAG_CF : 0) | (top & 2 ? FLAG_PF : 0) | (top & 4 ? FLAG_ZF : 0);
        vm->cpu->eflags = flags;
        unsigned classes = values[a].exceptions, condition = cases[ci].relation;
        if (f->memory == REAL32 || f->memory == REAL64) {
            classes |= cases[ci].real_exceptions; condition = cases[ci].real_relation;
        } else if (f->memory) condition = f->memory == INT16 ? cases[ci].int16_relation : cases[ci].int32_relation;
        else if (f->test) condition = values[a].zero_relation;
        else if (!f->index) condition = E;
        else classes |= values[b].exceptions;
        unsigned raised = classes & (Q | 1) ? ((classes & 1) || !f->quiet ? 1 : 0) : classes & 2;
        if (classes & (Q | 1)) condition = U;
        if (f->examine) {
            condition = (sample & 1 ? 0x4100u : values[a].classification) |
                        ((test_u16(before + 40) & 0x8000u) >> 6);
            raised = 0;
        }
        bool pending = (raised & ~control) != 0, invalid = (raised & ~control & 1) != 0;
        unsigned pops = pending ? 0 : f->pops, new_top = (top + pops) & 7u, new_tags = tags;
        for (unsigned i = 0; i < pops; i++) new_tags &= ~(1u << ((top + i) & 7u));
        unsigned sw = (initial & 0x20u) | raised | (pending ? 0x8080u : 0) |
                      (f->flags || invalid ? initial & 0x4500u : condition);
        if (f->flags) {
            flags &= ~(FLAG_OF | FLAG_AF | FLAG_SF);
            if (!invalid) flags = (flags & ~0x45u) | ((condition >> 8) & 0x45u);
        }
        fault = fpu_test_step(vm, f->op, f->mr, 0x4000, 0);
        COMPARE_CHECK(!fault.raised);
        COMPARE_CHECK(test_u16(image) == control);
        COMPARE_CHECK((test_u16(image + 2) & 0xc7ffu) == sw);
        COMPARE_CHECK((test_u16(image + 2) & 0x3800u) == new_top << 11 && image[4] == new_tags);
        COMPARE_CHECK(fpu_test_same_registers(image, before, new_tags));
        COMPARE_CHECK(vm->cpu->eflags == flags);
        COMPARE_CHECK(!memcmp(data, memory_before, sizeof(memory_before)));
        unsigned mr = f->memory ? (f->mr & 0x38) | (mode == 2 ? 5 : 6) : f->mr;
        COMPARE_CHECK(test_u32(image + 8) == 0x1000 && test_u16(image + 6) == ((f->op & 7) << 8 | mr));
        if (f->memory) COMPARE_CHECK(test_u32(image + 16) == 0x4000);
        uint8_t after[160]; memcpy(after, image, sizeof(after));
        fault = fpu_test_step(vm, 0x9b, 0, 0, 0);
        COMPARE_CHECK(pending ? fault.raised && fault.vector == 16 && fault.return_eip == 0x1000 : !fault.raised);
        COMPARE_CHECK(!memcmp(image, after, sizeof(after)));
#undef COMPARE_CHECK
    }
    serial_puts("[DOS-FPU-COMPARE] mode="); serial_putdec(mode);
    serial_puts(" forms="); serial_putdec(count); serial_puts(" checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    *total += checks;
    return failures;
}

static unsigned fpu_test_convert(dos_vm_t *vm, unsigned mode, unsigned *total)
{
    unsigned checks = 0, failures = 0;
    const unsigned count = sizeof(dos_fpu_convert_cases) / sizeof(dos_fpu_convert_cases[0]);
    static const unsigned pcs[] = {0, 2, 3};
    uint8_t *image = vm->native_fp->image, *guard = vm->mem + 0x3ff8, *data = guard + 8;
    for (unsigned ci = 0; ci < count; ci++) for (unsigned top = 0; top < 8; top++)
    for (unsigned rc = 0; rc < 4; rc++) for (unsigned pc = 0; pc < 3; pc++)
    for (unsigned mask = 0; mask < 32; mask++) {
#define CONVERT_CHECK(condition) do { \
    checks++; if (!(condition)) { failures++; if (failures <= 12) { \
        serial_puts("[DOS-FPU-CONVERT] FAIL line="); serial_putdec(__LINE__); \
        serial_puts(" mode="); serial_putdec(mode); serial_puts(" case="); serial_putdec(ci); \
        serial_puts(" top="); serial_putdec(top); serial_puts(" rc="); serial_putdec(rc); \
        serial_puts(" pc="); serial_putdec(pc); serial_puts(" mask="); serial_putdec(mask); \
        serial_puts("\n"); } } \
} while (0)
        const struct dos_fpu_convert_case *c = &dos_fpu_convert_cases[ci];
        cpu_event_fault_t fault = fpu_test_step(vm, 0xdb, 0xe3, 0, 0);
        CONVERT_CHECK(!fault.raised);
        /* Conversions never raise #Z; cross all five relevant masks. */
        uint16_t control = 0x44u | (mask & 3u) | ((mask & 28u) << 1) |
                           (pcs[pc] << 8) | (rc << 10);
        unsigned next = (top + 7) & 7u, tags = 0xffu & ~(c->load ? 1u << next : 0);
        unsigned initial = ((top & 1) << 8) | ((top & 2) << 9) | ((top & 4) << 12) |
                           0x200u | (control & 0x20u && (ci & 1) ? 0x20u : 0);
        fpu_test_seed_stack(image, top, tags, control);
        test_w16(image + 2, initial | (top << 11));
        memcpy(image + 32, c->source, 10);
        memset(guard, 0xa5, 32); memcpy(data, c->memory, c->width);
        uint8_t before[160], expected_memory[32];
        memcpy(before, image, sizeof(before)); memcpy(expected_memory, guard, sizeof(expected_memory));
        uint32_t flags = FLAGS_FIXED | FLAG_DF | FLAG_OF | FLAG_AF | FLAG_SF |
                         (top & 1 ? FLAG_CF : 0) | (top & 2 ? FLAG_PF : 0) | (top & 4 ? FLAG_ZF : 0);
        vm->cpu->eflags = flags;
        unsigned raised = c->unmasked[rc] & ~control & 0x18u ? c->unmasked[rc] : c->masked[rc];
        bool pending = (raised & ~control & 0x3fu) != 0;
        bool inhibited = (raised & ~control & (c->load ? 1u : 0x19u)) != 0;
        unsigned new_top = inhibited ? top : c->load ? next : (top + c->pop) & 7u;
        unsigned changed = c->load && !inhibited ? 1u << next : 0;
        unsigned new_tags = tags | changed;
        if (c->pop && !inhibited) new_tags &= ~(1u << top);
        unsigned sw = (initial & 0x4520u) | raised | (pending ? 0x8080u : 0) | (new_top << 11);
        if (!c->load && !inhibited) memcpy(expected_memory + 8, c->result[rc], c->width);
        fault = fpu_test_step(vm, c->opcode, c->modrm, 0x4000, 0);
        CONVERT_CHECK(!fault.raised && vm->cpu->eip == 0x1000u + (mode == 2 ? 6u : 4u));
        CONVERT_CHECK(test_u16(image) == control && test_u16(image + 2) == sw);
        CONVERT_CHECK(image[4] == new_tags);
        CONVERT_CHECK(fpu_test_same_registers(image, before, new_tags & ~changed));
        CONVERT_CHECK(vm->cpu->eflags == flags);
        CONVERT_CHECK(!memcmp(guard, expected_memory, sizeof(expected_memory)));
        if (changed) CONVERT_CHECK(!memcmp(image + 32, c->result[rc], 10));
        unsigned mr = c->modrm | (mode == 2 ? 5 : 6);
        CONVERT_CHECK(test_u32(image + 8) == 0x1000 && !test_u32(image + 12) &&
                      test_u16(image + 6) == ((c->opcode & 7) << 8 | mr));
        CONVERT_CHECK(test_u32(image + 16) == 0x4000 && !test_u32(image + 20));
        CONVERT_CHECK(vm->native_fp->fcs == (mode ? 0x0f : 0) &&
                      vm->native_fp->fds == (mode ? 0x17 : 0));
        CONVERT_CHECK(!memcmp(image + 24, before + 24, 8));
        uint8_t after[160]; memcpy(after, image, sizeof(after));
        fault = fpu_test_step(vm, 0x9b, 0, 0, 0);
        CONVERT_CHECK(pending ? fault.raised && fault.vector == 16 && fault.return_eip == 0x1000 : !fault.raised);
        CONVERT_CHECK(!memcmp(image, after, sizeof(after)));
#undef CONVERT_CHECK
    }
    serial_puts("[DOS-FPU-CONVERT] mode="); serial_putdec(mode);
    serial_puts(" cases="); serial_putdec(count); serial_puts(" checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    *total += checks;
    return failures;
}

static unsigned fpu_test_convert_memory(dos_vm_t *vm, unsigned *total)
{
    unsigned checks = 0, failures = 0, forms = 0;
    unsigned previous = 0;
    uint8_t *image = vm->native_fp->image;
    for (unsigned ci = 0; ci < sizeof(dos_fpu_convert_cases)/sizeof(dos_fpu_convert_cases[0]); ci++) {
        const struct dos_fpu_convert_case *c = &dos_fpu_convert_cases[ci];
        unsigned encoding = (c->opcode << 8) | c->modrm;
        if (encoding == previous) continue;
        previous = encoding; forms++;
        for (unsigned protection = 0; protection < (c->load ? 2u : 3u); protection++) {
#define CONVERT_MEMORY_CHECK(condition) do { \
    checks++; if (!(condition)) { failures++; if (failures <= 12) { \
        serial_puts("[DOS-FPU-CONVERT-MEMORY] FAIL line="); serial_putdec(__LINE__); \
        serial_puts(" form="); serial_putdec(forms); serial_puts(" protection="); serial_putdec(protection); \
        serial_puts("\n"); } } \
} while (0)
            fpu_test_mode(vm, 2);
            cpu_event_fault_t fault = fpu_test_step(vm, 0xdb, 0xe3, 0, 0);
            CONVERT_MEMORY_CHECK(!fault.raised);
            fpu_test_seed_stack(image, 0, c->load ? 0x7f : 0xff, 0x037f);
            memcpy(image + 32, c->source, 10);
            uint32_t address = 0x5000 - c->width / 2;
            uint8_t *guard = vm->mem + address - 8, memory[32], before[512];
            memset(guard, 0xa5, 32); memcpy(guard + 8, c->memory, c->width);
            memcpy(memory, guard, sizeof(memory)); memcpy(before, image, sizeof(before));
            memset(vm->mem + 0x10000, 0, 8192);
            test_w32(vm->mem + 0x10000, 0x11007);
            for (unsigned page = 0; page < 256; page++)
                test_w32(vm->mem + 0x11000 + page * 4, (page << 12) | 7);
            test_w32(vm->mem + 0x11014, protection == 0 ? 0 : protection == 1 ? 0x5003 : 0x5005);
            vm->cpu->cr0 |= DOS_CR0_PG;
            vm->cpu->cr3 = 0x10000;
            fault = fpu_test_step(vm, c->opcode, c->modrm, address, 0);
            unsigned error = 4u | (c->load ? 0 : 2u) | (protection ? 1u : 0);
            CONVERT_MEMORY_CHECK(fault.raised && fault.vector == 14 && fault.error == error &&
                                 fault.return_eip == 0x1000 && vm->cpu->cr2 == 0x5000);
            CONVERT_MEMORY_CHECK(!memcmp(image, before, sizeof(before)));
            CONVERT_MEMORY_CHECK(!memcmp(guard, memory, sizeof(memory)));
            CONVERT_MEMORY_CHECK(!(test_u32(vm->mem + 0x11010) & 0x40u) &&
                                 !(test_u32(vm->mem + 0x11014) & 0x40u));
            test_w32(vm->mem + 0x11014, c->load ? 0x5005 : 0x5007);
            fault = fpu_test_step(vm, c->opcode, c->modrm, address, 0);
            CONVERT_MEMORY_CHECK(!fault.raised && !(test_u16(image + 2) & 0x80ffu));
            if (c->load) {
                CONVERT_MEMORY_CHECK(!memcmp(image + 32, c->result[0], 10) && image[4] == 0xff &&
                                     (test_u16(image + 2) >> 11 & 7u) == 7);
            } else {
                memcpy(memory + 8, c->result[0], c->width);
                CONVERT_MEMORY_CHECK(image[4] == (c->pop ? 0xfe : 0xff) &&
                                     (test_u16(image + 2) >> 11 & 7u) == c->pop);
            }
            CONVERT_MEMORY_CHECK(!memcmp(guard, memory, sizeof(memory)));
            unsigned dirty = c->load ? 0 : 0x40;
            CONVERT_MEMORY_CHECK((test_u32(vm->mem + 0x11010) & 0x40u) == dirty &&
                                 (test_u32(vm->mem + 0x11014) & 0x40u) == dirty);
#undef CONVERT_MEMORY_CHECK
        }
    }
    serial_puts("[DOS-FPU-CONVERT-MEMORY] forms="); serial_putdec(forms);
    serial_puts(" checks="); serial_putdec(checks); serial_puts(" failures="); serial_putdec(failures);
    serial_puts("\n");
    *total += checks;
    return failures;
}

static unsigned fpu_test_dispatch(unsigned *total)
{
    unsigned checks = 0, failures = 0, numeric = 0, state = 0;
    for (unsigned opcode = 0xd8; opcode <= 0xdf; opcode++)
    for (unsigned modrm = 0; modrm <= 0xff; modrm++) {
        if (modrm < 0xc0 && (modrm & ~0x38u)) continue;
        dos_fpu_op_t op;
        if (!dos_fpu_decode(opcode, modrm, true, &op)) continue;
        uint8_t image[1024] = {0}, operand[108] = {0};
        fpu_test_seed_stack(image, 0, 0xff, 0x037f);
        dos_fpu_numeric_result_t result;
        uint64_t flags = 0;
        bool n = dos_fpu_soft_evaluate(image, operand, opcode, modrm, &result);
        bool s = dos_fpu_state_step(image, operand, opcode, modrm, 0, &flags);
        checks++; numeric += n; state += s;
        if (n == s) failures++;
    }
    checks++; if (numeric != 279 || state != 129) failures++;
    serial_puts("[DOS-FPU-DISPATCH] numeric="); serial_putdec(numeric);
    serial_puts(" state="); serial_putdec(state); serial_puts(" checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    *total += checks;
    return failures;
}

static unsigned fpu_test_state(dos_vm_t *vm, unsigned mode, unsigned *total)
{
    /* Final rounded values checked independently by MPFR and native FLD
     * instructions in dos_fpu_state_host.c, not the production lookup table. */
    static const uint64_t constants[7][4] = {
        {0x8000000000000000ull,0x8000000000000000ull,0x8000000000000000ull,0x8000000000000000ull},
        {0xd49a784bcd1b8afeull,0xd49a784bcd1b8afeull,0xd49a784bcd1b8affull,0xd49a784bcd1b8afeull},
        {0xb8aa3b295c17f0bcull,0xb8aa3b295c17f0bbull,0xb8aa3b295c17f0bcull,0xb8aa3b295c17f0bbull},
        {0xc90fdaa22168c235ull,0xc90fdaa22168c234ull,0xc90fdaa22168c235ull,0xc90fdaa22168c234ull},
        {0x9a209a84fbcff799ull,0x9a209a84fbcff798ull,0x9a209a84fbcff799ull,0x9a209a84fbcff798ull},
        {0xb17217f7d1cf79acull,0xb17217f7d1cf79abull,0xb17217f7d1cf79acull,0xb17217f7d1cf79abull},
        {0,0,0,0}
    };
    static const uint16_t exponents[] = {0x3fff,0x4000,0x3fff,0x4000,0x3ffd,0x3ffe,0};
    static const unsigned precision[] = {0,2,3};
    unsigned checks = 0, failures = 0;
    uint8_t *image = vm->native_fp->image, *data = vm->mem + 0x4000;
#define STATE_CHECK(condition) do { \
    checks++; if (!(condition)) { failures++; \
        serial_puts("[DOS-FPU-STATE] FAIL line="); serial_putdec(__LINE__); serial_puts("\n"); } \
} while (0)
#define STATE_STEP(opcode, modrm, address, prefix) do { \
    cpu_event_fault_t fault = fpu_test_step(vm, opcode, modrm, address, prefix); \
    STATE_CHECK(!fault.raised && vm->cpu->running); \
} while (0)
    for (unsigned top = 0; top < 8; top++) for (unsigned rc = 0; rc < 4; rc++)
    for (unsigned pc = 0; pc < 3; pc++) for (unsigned c = 0; c < 7; c++) {
        STATE_STEP(0xdb, 0xe3, 0, 0);
        unsigned next = (top + 7) & 7u, control = 0x7f | precision[pc] << 8 | rc << 10;
        fpu_test_seed_stack(image, top, 0xffu & ~(1u << next), control);
        test_w16(image + 2, top << 11 | 0x4720u);
        uint8_t before[160]; memcpy(before, image, sizeof(before));
        STATE_STEP(0xd9, 0xe8 + c, 0, 0);
        STATE_CHECK(test_u16(image) == control && test_u16(image + 2) == (next << 11 | 0x4520u));
        STATE_CHECK(image[4] == 0xff && test_u16(image + 40) == exponents[c]);
        STATE_CHECK((test_u32(image + 32) | (uint64_t)test_u32(image + 36) << 32) == constants[c][rc]);
        STATE_CHECK(fpu_test_same_registers(image, before, 0xffu & ~(1u << next)));
    }
    for (unsigned top = 0; top < 8; top++) for (unsigned tags = 0; tags < 256; tags++)
    for (unsigned wide = 0; wide < 2; wide++) for (unsigned pending = 0; pending < 2; pending++) {
        unsigned prefix = (wide != (mode == 2)) ? 0x66 : 0, env = wide ? 28 : 14, stride = wide ? 4 : 2;
        STATE_STEP(0xdb, 0xe3, 0, 0);
        unsigned control = pending ? 0x037e : 0x037f, sw = top << 11 | 0x4701u | (pending ? 0x8080u : 0);
        fpu_test_seed_stack(image, top, tags, control); test_w16(image + 2, sw);
        test_w16(image + 6, 0x5ab); test_w32(image + 8, 0x2345); test_w32(image + 16, 0x5678);
        vm->native_fp->fcs = mode ? vm->cpu->cs : 0; vm->native_fp->fds = mode ? vm->cpu->ds : 0;
        memset(data - 1, 0xa5, 110); memset(data + 0xff, 0xa5, 110);
        uint8_t before[160]; memcpy(before, image, sizeof(before));
        STATE_STEP(0xd9, 0x30, 0x4000, prefix);
        STATE_CHECK(test_u16(data) == control && test_u16(data + stride) == sw);
        unsigned full_tags = 0;
        for (unsigned p = 0; p < 8; p++) if (!(tags & (1u << p))) full_tags |= 3u << (p * 2);
        STATE_CHECK(test_u16(data + stride * 2) == full_tags && data[-1] == 0xa5 && data[env] == 0xa5);
        STATE_CHECK(test_u16(image) == 0x037f && test_u16(image + 2) == (sw & ~0x8080u));
        STATE_CHECK(fpu_test_same_registers(image, before, 0xff));
        unsigned loaded_top = (top + 3) & 7u, loaded_sw = (sw & ~0x3800u) | loaded_top << 11;
        test_w16(data + stride, loaded_sw);
        STATE_STEP(0xd9, 0x20, 0x4000, prefix);
        STATE_CHECK(test_u16(image) == control && test_u16(image + 2) == loaded_sw && image[4] == tags);
        STATE_CHECK(fpu_test_same_registers(image, before, 0xff));
        STATE_CHECK(test_u32(image + 8) == 0x2345 && test_u32(image + 16) == 0x5678 &&
                    test_u16(image + 6) == (mode && !wide ? 0 : 0x5ab));
        STATE_CHECK(vm->native_fp->fcs == (mode ? vm->cpu->cs : 0) &&
                    vm->native_fp->fds == (mode ? vm->cpu->ds : 0));
        memcpy(before, image, sizeof(before));
        STATE_STEP(0xdd, 0x30, 0x4100, prefix);
        STATE_CHECK(test_u16(image) == 0x037f && !test_u16(image + 2) && !image[4]);
        STATE_CHECK(!test_u16(image + 6) && !test_u32(image + 8) && !test_u32(image + 16) &&
                    !vm->native_fp->fcs && !vm->native_fp->fds);
        for (unsigned i = 0; i < 8; i++)
            STATE_CHECK(!memcmp(data + 0x100 + env + i * 10, before + 32 + i * 16, 10));
        STATE_CHECK(data[0xff] == 0xa5 && data[0x100 + env + 80] == 0xa5);
        STATE_STEP(0xdd, 0x20, 0x4100, prefix);
        STATE_CHECK(test_u16(image) == control && test_u16(image + 2) == loaded_sw && image[4] == tags);
        STATE_CHECK(fpu_test_same_registers(image, before, 0xff));
        cpu_event_fault_t fault = fpu_test_step(vm, 0x9b, 0, 0, 0);
        STATE_CHECK(pending ? fault.raised && fault.vector == 16 : !fault.raised);
    }
    STATE_STEP(0xdb, 0xe3, 0, 0);
    serial_puts("[DOS-FPU-STATE] mode="); serial_putdec(mode); serial_puts(" checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    *total += checks;
#undef STATE_STEP
#undef STATE_CHECK
    return failures;
}

int dos_fpu_selftest(void)
{
    unsigned failures = 0, checks = 0;
    const unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    if (!vm) return 1;
    memset(vm, 0, sizeof(*vm));
    cpu8086_state_t cpu;
    vm->cpu = &cpu;
    vm->mem = dos_host_alloc_pages(512);
    vm->total_mem_size = 2u << 20;
    if (!vm->mem) { dos_host_free_pages(vm, vm_pages); return 1; }
    memset(vm->mem, 0, vm->total_mem_size);
    cpu8086_init(&cpu, vm);
    if (!dos_fpu_ensure(vm)) { failures++; goto done; }
    failures += fpu_test_dispatch(&checks);
    uint8_t *image = vm->native_fp->image;
    uint8_t *data = vm->mem + 0x4000;
    cpu_event_fault_t fault;
#define FP_CHECK(condition) do { \
    checks++; if (!(condition)) { failures++; \
        serial_puts("[DOS-FPU] FAIL line="); serial_putdec(__LINE__); serial_puts("\n"); } \
} while (0)
#define FP_STEP(op, mr, addr, pre) do { \
    fault = fpu_test_step(vm, op, mr, addr, pre); FP_CHECK(!fault.raised); \
} while (0)
    for (unsigned mode = 0; mode < 3; mode++) {
        fpu_test_mode(vm, mode);
        FP_STEP(0xdb, 0xe3, 0, 0);
        FP_CHECK(test_u16(image) == 0x037f && !test_u16(image + 2) && !image[4]);
        /* Exact integer and real arithmetic, all three integer store widths. */
        FP_STEP(0xd9, 0xe8, 0, 0); /* 1 */
        FP_STEP(0xd9, 0xe8, 0, 0); /* 1 */
        FP_STEP(0xde, 0xc1, 0, 0); /* 2 */
        FP_STEP(0xdf, 0x10, 0x4000, 0);
        FP_CHECK(test_u16(data) == 2);
        FP_STEP(0xdb, 0x10, 0x4000, 0);
        FP_CHECK(test_u32(data) == 2);
        FP_STEP(0xdf, 0x38, 0x4000, 0);
        FP_CHECK(test_u32(data) == 2 && !test_u32(data + 4) && !image[4]);
        test_w32(data, 0x40400000); /* 3.0f */
        FP_STEP(0xd9, 0x00, 0x4000, 0);
        FP_STEP(0xd8, 0x08, 0x4000, 0); /* square */
        FP_STEP(0xd9, 0xfa, 0, 0); /* sqrt */
        FP_STEP(0xdd, 0x18, 0x4000, 0);
        FP_CHECK(test_u32(data) == 0 && test_u32(data + 4) == 0x40080000);
        /* 80-bit precision: 2^63 + 1 is not representable as binary64. */
        test_w32(data, 0); test_w32(data + 4, 0x80000000); test_w16(data + 8, 0x403e);
        FP_STEP(0xdb, 0x28, 0x4000, 0);
        FP_STEP(0xd9, 0xe8, 0, 0);
        FP_STEP(0xde, 0xc1, 0, 0);
        FP_STEP(0xdb, 0x38, 0x4000, 0);
        FP_CHECK(test_u32(data) == 1 && test_u32(data + 4) == 0x80000000 && test_u16(data + 8) == 0x403e);
        /* Precision control, all rounding modes, signed integer conversion. */
        static const int16_t rounded[4] = {2, 1, 2, 1};
        for (unsigned rc = 0; rc < 4; rc++) {
            test_w16(data, 0x037f | (rc << 10));
            FP_STEP(0xd9, 0x28, 0x4000, 0);
            test_w32(data, 0x3fc00000); /* 1.5 */
            FP_STEP(0xd9, 0x00, 0x4000, 0);
            FP_STEP(0xdf, 0x18, 0x4004, 0);
            FP_CHECK(test_u16(data + 4) == (uint16_t)rounded[rc]);
            FP_STEP(0xd9, 0x00, 0x4000, 0);
            FP_STEP(0xdf, 0x08, 0x4004, 0); /* FISTTP must ignore RC. */
            FP_CHECK(test_u16(data + 4) == 1 && test_u16(image) == (0x037f | (rc << 10)));
        }
        FP_STEP(0xdb, 0xe3, 0, 0);
        memset(data, 0, 10); data[0] = 0x45; data[1] = 0x23; data[2] = 0x01; data[9] = 0x80;
        FP_STEP(0xdf, 0x20, 0x4000, 0); /* packed BCD */
        FP_STEP(0xdf, 0x30, 0x4010, 0);
        FP_CHECK(!memcmp(data, data + 16, 10));
        /* Eight physical tags, stack fault, constants and transcendental ops. */
        for (unsigned i = 0; i < 8; i++) FP_STEP(0xd9, 0xe8, 0, 0);
        FP_CHECK(image[4] == 0xff && !(test_u16(image + 2) & 0x3800));
        FP_STEP(0xd9, 0xe8, 0, 0);
        FP_CHECK((test_u16(image + 2) & 0x0241) == 0x0241);
        FP_STEP(0xdb, 0xe3, 0, 0);
        FP_STEP(0xd9, 0xee, 0, 0); FP_STEP(0xd9, 0xfb, 0, 0); /* sincos(0) */
        FP_STEP(0xdb, 0x18, 0x4000, 0); FP_CHECK(test_u32(data) == 1);
        FP_STEP(0xdb, 0x18, 0x4000, 0); FP_CHECK(test_u32(data) == 0);
        /* FCOMI/FCMOV use guest FLAGS, never the kernel's comparison flags. */
        FP_STEP(0xd9, 0xe8, 0, 0); FP_STEP(0xd9, 0xee, 0, 0);
        cpu.eflags = FLAGS_FIXED | FLAG_OF | FLAG_SF | FLAG_AF;
        FP_STEP(0xdb, 0xf1, 0, 0);
        FP_CHECK((cpu.eflags & 0x8d5u) == FLAG_CF);
        FP_STEP(0xda, 0xc1, 0, 0); /* FCMOVB ST,ST1 */
        FP_STEP(0xdb, 0x18, 0x4000, 0); FP_CHECK(test_u32(data) == 1);
        FP_STEP(0xdb, 0xe3, 0, 0);
        /* 14/28-byte environment and 94/108-byte register images. */
        for (unsigned alternate = 0; alternate < 2; alternate++) {
            unsigned prefix = alternate ? 0x66 : 0;
            bool wide = (mode == 2) ^ (alternate != 0);
            unsigned env = wide ? 28 : 14;
            FP_STEP(0xd9, 0xe8, 0, 0);
            FP_STEP(0xd9, 0x30, 0x4100, prefix); /* FNSTENV */
            FP_CHECK(test_u16(vm->mem + 0x4100) == 0x037f);
            uint32_t ip = wide && mode ? test_u32(vm->mem + 0x410c) : test_u16(vm->mem + 0x4100 + (wide ? 12 : 6));
            FP_CHECK(ip == 0x1000);
            if (mode) FP_CHECK(test_u16(vm->mem + 0x4100 + (wide ? 16 : 8)) == cpu.cs);
            FP_STEP(0xdd, 0x30, 0x4200, prefix); /* FNSAVE */
            FP_CHECK(!image[4] && test_u16(image) == 0x037f);
            FP_CHECK(test_u32(vm->mem + 0x4200 + env + 4) == 0x80000000);
            FP_STEP(0xdd, 0x20, 0x4200, prefix); /* FRSTOR */
            FP_STEP(0xdb, 0x18, 0x4000, 0); FP_CHECK(test_u32(data) == 1 && !image[4]);
        }
        /* Error detection is deferred to WAIT, which retains its own IP/RF. */
        test_w16(data, 0x037e);
        FP_STEP(0xd9, 0x28, 0x4000, 0);
        FP_STEP(0xd9, 0xee, 0, 0); FP_STEP(0xd9, 0xee, 0, 0);
        FP_STEP(0xde, 0xf9, 0, 0);
        FP_CHECK(dos_fpu_pending(vm));
        FP_STEP(0xdf, 0xe0, 0, 0); FP_CHECK(cpu.ax & 0x81);
        cpu.eflags |= FLAG_RF;
        fault = fpu_test_step(vm, 0x9b, 0, 0, 0);
        FP_CHECK(fault.raised && fault.vector == 16 && fault.return_eip == 0x1000 && (cpu.eflags & FLAG_RF));
        FP_STEP(0xdb, 0xe2, 0, 0);
        FP_CHECK(!dos_fpu_pending(vm));
        /* Unmasked invalid store leaves memory and TOP intact. */
        FP_STEP(0xdb, 0xe3, 0, 0);
        test_w16(data, 0x037e); FP_STEP(0xd9, 0x28, 0x4000, 0);
        test_w32(data, 0x11223344);
        uint16_t before_top = test_u16(image + 2) & 0x3800;
        FP_STEP(0xdb, 0x18, 0x4000, 0);
        FP_CHECK(test_u32(data) == 0x11223344 && (test_u16(image + 2) & 0x3800) == before_top);
        FP_STEP(0xdb, 0xe3, 0, 0);
        test_w16(data, 0x037e); FP_STEP(0xd9, 0x28, 0x4000, 0);
        cpu.eflags = FLAGS_FIXED | FLAG_CF | FLAG_OF | FLAG_AF | FLAG_SF;
        FP_STEP(0xdb, 0xf1, 0, 0); /* unmasked empty-stack FCOMI */
        FP_CHECK((cpu.eflags & 0x8d5u) == FLAG_CF && dos_fpu_pending(vm));
        FP_STEP(0xdb, 0xe3, 0, 0);
        /* Unmasked precision still stores the rounded result and pops. */
        test_w16(data, 0x035f); FP_STEP(0xd9, 0x28, 0x4000, 0);
        test_w32(data, 0x3fc00000); FP_STEP(0xd9, 0, 0x4000, 0);
        FP_STEP(0xdb, 0x18, 0x4004, 0);
        FP_CHECK(test_u32(data + 4) == 2 && !image[4] && (test_u16(image + 2) & 0xa0) == 0xa0);
        FP_STEP(0xdb, 0xe3, 0, 0);
        /* FLD pushes the normalized value even with #D unmasked (SDM FLD). */
        test_w16(data, 0x037d); FP_STEP(0xd9, 0x28, 0x4000, 0);
        test_w32(data, 1); FP_STEP(0xd9, 0, 0x4000, 0);
        FP_CHECK(image[4] == 0x80 && test_u16(image + 40) == 0x3f6a &&
                 (test_u16(image + 2) & 0x3882) == 0x3882);
        FP_STEP(0xdb, 0xe3, 0, 0);
        FP_STEP(0xd9, 0, 0x4000, 0);
        FP_CHECK(test_u16(image + 40) == 0x3f6a && (test_u16(image + 2) & 2));
        FP_STEP(0xdb, 0xe3, 0, 0);
        /* Overflowing a smaller store does not alter its destination. */
        test_w16(data, 0x0377); FP_STEP(0xd9, 0x28, 0x4000, 0);
        test_w32(data, 0); test_w32(data + 4, 0x80000000); test_w16(data + 8, 0x4080);
        FP_STEP(0xdb, 0x28, 0x4000, 0);
        test_w32(data + 16, 0x12345678);
        FP_STEP(0xd9, 0x18, 0x4010, 0);
        FP_CHECK(test_u32(data + 16) == 0x12345678 && image[4] && (test_u16(image + 2) & 0x88) == 0x88);
        FP_STEP(0xdb, 0xe3, 0, 0);
        /* #NM and #UD occur without a memory operand access or FP change. */
        uint8_t before[512]; memcpy(before, image, 512);
        cpu.cr0 |= 4;
        fault = fpu_test_step(vm, 0xd9, 0, 0xffff, 0);
        FP_CHECK(fault.raised && fault.vector == 7 && !memcmp(before, image, 512));
        cpu.cr0 &= ~4u;
        fault = fpu_test_step(vm, 0xd9, 0xc0, 0, 0xf0);
        FP_CHECK(fault.raised && fault.vector == 6 && !memcmp(before, image, 512));
        /* Ten-byte memory accesses and address-size override share checked spans. */
        fault = fpu_test_step(vm, 0xdb, 0x28, 0xfff8, 0);
        FP_CHECK(fault.raised && fault.vector == 13 && !memcmp(before, image, 512));
        test_w32(data, 0x3f800000);
        FP_STEP(0xd9, 0x00, 0x4000, 0x67);
        FP_STEP(0xdb, 0x18, 0x4000, 0x67); FP_CHECK(test_u32(data) == 1);

        /* Physical tags, not a stack-depth approximation: wrap every TOP and
         * preserve unrelated live registers on masked/unmasked transfers. */
        static const uint8_t loads[][2] = {
            {0xd9,0x00}, {0xdd,0x00}, {0xdb,0x28}, {0xdf,0x00},
            {0xdb,0x00}, {0xdf,0x28}, {0xdf,0x20},
            {0xd9,0xe8}, {0xd9,0xe9}, {0xd9,0xea}, {0xd9,0xeb},
            {0xd9,0xec}, {0xd9,0xed}, {0xd9,0xee},
            {0xd9,0xc0}, {0xd9,0xc1}, {0xd9,0xc2}, {0xd9,0xc3},
            {0xd9,0xc4}, {0xd9,0xc5}, {0xd9,0xc6}, {0xd9,0xc7}
        };
        static const struct {
            uint8_t opcode, modrm, size, pop, value[10];
        } stores[] = {
            {0xd9,0x10,4,0,{0,0,0xc0,0xff}}, {0xd9,0x18,4,1,{0,0,0xc0,0xff}},
            {0xdd,0x10,8,0,{0,0,0,0,0,0,0xf8,0xff}}, {0xdd,0x18,8,1,{0,0,0,0,0,0,0xf8,0xff}},
            {0xdb,0x38,10,1,{0,0,0,0,0,0,0,0xc0,0xff,0xff}},
            {0xdb,0x10,4,0,{0,0,0,0x80}}, {0xdb,0x18,4,1,{0,0,0,0x80}},
            {0xdb,0x08,4,1,{0,0,0,0x80}}, {0xdf,0x10,2,0,{0,0x80}},
            {0xdf,0x18,2,1,{0,0x80}}, {0xdf,0x08,2,1,{0,0x80}},
            {0xdf,0x38,8,1,{0,0,0,0,0,0,0,0x80}}, {0xdd,0x08,8,1,{0,0,0,0,0,0,0,0x80}},
            {0xdf,0x30,10,1,{0,0,0,0,0,0,0,0xc0,0xff,0xff}}
        };
        static const struct { uint8_t opcode, modrm, pops, flags; } compares[] = {
            {0xd8,0xd1,0,0}, {0xd8,0xd9,1,0}, {0xdd,0xe1,0,0}, {0xdd,0xe9,1,0},
            {0xda,0xe9,2,0}, {0xde,0xd9,2,0}, {0xd9,0xe4,0,0},
            {0xdb,0xe9,0,1}, {0xdb,0xf1,0,1}, {0xdf,0xe9,1,1}, {0xdf,0xf1,1,1},
            {0xd8,0x10,0,0}, {0xd8,0x18,1,0}, {0xda,0x10,0,0}, {0xda,0x18,1,0},
            {0xdc,0x10,0,0}, {0xdc,0x18,1,0}, {0xde,0x10,0,0}, {0xde,0x18,1,0}
        };
        static const uint8_t indefinite[10] = {0,0,0,0,0,0,0,0xc0,0xff,0xff};
        for (unsigned top = 0; top < 8; top++) for (unsigned masked = 0; masked < 2; masked++) {
            uint8_t prior[160];
            unsigned next = (top + 7) & 7u;
            uint16_t control = 0x037e | masked;
            for (unsigned i = 0; i < sizeof(loads) / sizeof(loads[0]); i++) {
                FP_STEP(0xdb, 0xe3, 0, 0);
                fpu_test_seed_stack(image, top, 0xff, control);
                memcpy(prior, image, sizeof(prior));
                memset(data, 0, 10);
                FP_STEP(loads[i][0], loads[i][1], 0x4000, 0);
                FP_CHECK(test_u16(image) == control && image[4] == 0xff);
                FP_CHECK((test_u16(image + 2) & 0xbbc1u) ==
                         ((masked ? next : top) << 11 | (masked ? 0x0341u : 0x83c1u)));
                FP_CHECK(fpu_test_same_registers(image, prior, masked ? 0xffu & ~(1u << next) : 0xffu));
                FP_CHECK(!masked || !memcmp(image + 32, indefinite, 10));
            }
            for (unsigned source = 0; source < 8; source++) {
                FP_STEP(0xdb, 0xe3, 0, 0);
                unsigned tags = 0xffu & ~(1u << next) & ~(1u << ((top + source) & 7u));
                fpu_test_seed_stack(image, top, tags, control);
                memcpy(prior, image, sizeof(prior));
                FP_STEP(0xd9, 0xc0 + source, 0, 0);
                FP_CHECK(image[4] == (tags | (masked ? 1u << next : 0)));
                FP_CHECK((test_u16(image + 2) & 0xbbc1u) ==
                         ((masked ? next : top) << 11 | (masked ? 0x0141u : 0x81c1u)));
                FP_CHECK(fpu_test_same_registers(image, prior, tags));
                FP_CHECK(!masked || !memcmp(image + 32, indefinite, 10));
            }
            for (unsigned i = 0; i < sizeof(stores) / sizeof(stores[0]); i++) {
                FP_STEP(0xdb, 0xe3, 0, 0);
                unsigned tags = 0xffu & ~(1u << top);
                fpu_test_seed_stack(image, top, tags, control);
                memcpy(prior, image, sizeof(prior));
                memset(data - 1, 0xa5, 12);
                FP_STEP(stores[i].opcode, stores[i].modrm, 0x4000, 0);
                FP_CHECK(image[4] == tags && test_u16(image) == control);
                unsigned new_top = (top + (masked ? stores[i].pop : 0)) & 7u;
                FP_CHECK((test_u16(image + 2) & 0xbbc1u) ==
                         (new_top << 11 | (masked ? 0x0141u : 0x81c1u)));
                FP_CHECK(fpu_test_same_registers(image, prior, tags));
                for (unsigned b = 0; b < stores[i].size; b++)
                    FP_CHECK(data[b] == (masked ? stores[i].value[b] : 0xa5));
                FP_CHECK(data[-1] == 0xa5 && data[stores[i].size] == 0xa5);
            }
            for (unsigned target = 0; target < 8; target++) for (unsigned pop = 0; pop < 2; pop++) {
                FP_STEP(0xdb, 0xe3, 0, 0);
                unsigned tags = 0xffu & ~(1u << top), dest = (top + target) & 7u;
                fpu_test_seed_stack(image, top, tags, control);
                memcpy(prior, image, sizeof(prior));
                FP_STEP(0xdd, 0xd0 + pop * 8 + target, 0, 0);
                unsigned expected = tags | (masked ? 1u << dest : 0);
                if (masked && pop) expected &= ~(1u << top);
                FP_CHECK(image[4] == expected);
                FP_CHECK((test_u16(image + 2) & 0xbbc1u) ==
                         (((top + (masked ? pop : 0)) & 7u) << 11 | (masked ? 0x0141u : 0x81c1u)));
                FP_CHECK(fpu_test_same_registers(image, prior, masked ? tags & ~(1u << dest) : tags));
                if (masked && (expected & (1u << dest)))
                    FP_CHECK(!memcmp(image + 32 + ((target - pop) & 7u) * 16, indefinite, 10));
            }
            for (unsigned i = 0; i < sizeof(compares) / sizeof(compares[0]); i++) {
                unsigned variants = compares[i].modrm >= 0xc0 && compares[i].opcode != 0xd9 ? 2 : 1;
                for (unsigned missing = 0; missing < variants; missing++) {
                    FP_STEP(0xdb, 0xe3, 0, 0);
                    unsigned tags = 0xffu & ~(1u << ((top + missing) & 7u));
                    fpu_test_seed_stack(image, top, tags, control);
                    memcpy(prior, image, sizeof(prior));
                    cpu.eflags = FLAGS_FIXED | FLAG_CF | FLAG_OF | FLAG_SF | FLAG_AF;
                    memset(data, 0, 10);
                    FP_STEP(compares[i].opcode, compares[i].modrm, 0x4000, 0);
                    unsigned pops = masked ? compares[i].pops : 0;
                    for (unsigned p = 0; p < pops; p++) tags &= ~(1u << ((top + p) & 7u));
                    FP_CHECK(image[4] == tags);
                    FP_CHECK((test_u16(image + 2) & 0xbbc1u) ==
                             (((top + pops) & 7u) << 11 | (masked ? 0x0141u : 0x81c1u)));
                    FP_CHECK((test_u16(image + 2) & 0x4500u) ==
                             (masked && !compares[i].flags ? 0x4500u : 0x0100u));
                    FP_CHECK((cpu.eflags & 0x8d5u) == (compares[i].flags ?
                             (masked ? FLAG_CF | FLAG_PF | FLAG_ZF : FLAG_CF) :
                             FLAG_CF | FLAG_OF | FLAG_SF | FLAG_AF));
                    FP_CHECK(fpu_test_same_registers(image, prior, tags));
                }
            }
            /* Occupied ST(0): inhibited narrowing stores retain every physical
             * value even if the host produced a result and advanced TOP. */
            if (!masked) for (unsigned narrow = 0; narrow < 2; narrow++)
                for (unsigned underflow = 0; underflow < 2; underflow++) {
                    FP_STEP(0xdb, 0xe3, 0, 0);
                    fpu_test_seed_stack(image, top, 0xff, underflow ? 0x036f : 0x0377);
                    test_w32(image + 32, 0);
                    test_w16(image + 40, underflow ? (narrow ? 0x3b00 : 0x3f00) :
                                                          (narrow ? 0x4400 : 0x4080));
                    memcpy(prior, image, sizeof(prior));
                    memset(data, 0xa5, 10);
                    FP_STEP(narrow ? 0xdd : 0xd9, 0x18, 0x4000, 0);
                    FP_CHECK(image[4] == 0xff && (test_u16(image + 2) & 0x3800u) == top << 11);
                    uint16_t exception = underflow ? 0x8090u : 0x8088u;
                    FP_CHECK((test_u16(image + 2) & exception) == exception);
                    FP_CHECK(fpu_test_same_registers(image, prior, 0xff));
                    for (unsigned b = 0; b < 10; b++) FP_CHECK(data[b] == 0xa5);
                }
        }
        failures += fpu_test_arithmetic_stack(vm, mode, &checks);
        failures += fpu_test_numeric(vm, mode, &checks);
        failures += fpu_test_trig(vm, mode, &checks);
        failures += fpu_test_elementary(vm, mode, &checks);
        failures += fpu_test_reduce(vm, mode, &checks);
        failures += fpu_test_compare(vm, mode, &checks);
        failures += fpu_test_convert(vm, mode, &checks);
        failures += fpu_test_state(vm, mode, &checks);
    }

    failures += fpu_test_convert_memory(vm, &checks);

    /* Cross-page operand probing must not consume ST(0) or dirty either page. */
    fpu_test_mode(vm, 2);
    FP_STEP(0xdb, 0xe3, 0, 0); FP_STEP(0xd9, 0xe8, 0, 0);
    memset(vm->mem + 0x10000, 0, 8192);
    test_w32(vm->mem + 0x10000, 0x11007);
    for (unsigned i = 0; i < 256; i++) test_w32(vm->mem + 0x11000 + i * 4, (i << 12) | 7);
    test_w32(vm->mem + 0x11000 + 5 * 4, 0); /* absent second operand page */
    cpu.cr0 |= DOS_CR0_PG;
    cpu.cr3 = 0x10000;
    memset(vm->mem + 0x4ffc, 0xa5, 8);
    uint8_t before[512]; memcpy(before, image, 512);
    fault = fpu_test_step(vm, 0xdd, 0x18, 0x4ffc, 0);
    FP_CHECK(fault.raised && fault.vector == 14 && fault.error == 6 && cpu.cr2 == 0x5000);
    FP_CHECK(!memcmp(before, image, 512) && test_u32(vm->mem + 0x4ffc) == 0xa5a5a5a5 &&
             !(test_u32(vm->mem + 0x11010) & 0x40));
    test_w32(vm->mem + 0x11014, 0x5007);
    FP_STEP(0xdd, 0x18, 0x4ffc, 0);
    FP_CHECK(test_u32(vm->mem + 0x4ffc) == 0 && test_u32(vm->mem + 0x5000) == 0x3ff00000 && !image[4]);
    FP_CHECK((test_u32(vm->mem + 0x11010) & 0x40) && (test_u32(vm->mem + 0x11014) & 0x40));

    /* PC/AT error latch: IRQ13, port F0 acknowledgement, and IGNNE recovery. */
    fpu_test_mode(vm, 0);
    cpu.cr0 &= ~0x20u;
    FP_CHECK(dos_io_init(vm));
    FP_STEP(0xdb, 0xe3, 0, 0);
    test_w16(data, 0x037e); FP_STEP(0xd9, 0x28, 0x4000, 0);
    FP_STEP(0xd9, 0xee, 0, 0); FP_STEP(0xd9, 0xee, 0, 0); FP_STEP(0xde, 0xf9, 0, 0);
    FP_CHECK(vm->native_fp->irq_pending && (dos_io_read8(vm, 0xa0) & 0x20));
    uint8_t vector = 0;
    FP_CHECK(dos_io_irq_begin(vm, 13, &vector) && vector == 0x75);
    dos_io_write8(vm, 0xf0, 0);
    FP_CHECK(!vm->native_fp->irq_pending && vm->native_fp->ignne);
    FP_STEP(0x9b, 0, 0, 0);
    FP_STEP(0xd9, 0xe8, 0, 0); /* arithmetic can resume under IGNNE */
    FP_CHECK(dos_fpu_pending(vm));
    FP_STEP(0xdb, 0xe2, 0, 0);
    FP_CHECK(!dos_fpu_pending(vm) && !vm->native_fp->ignne);

    /* Every documented register encoding can execute behind the validated
     * thunk table; all rejected encodings must deliver guest #UD. */
    fpu_test_mode(vm, 2);
    for (unsigned opcode = 0xd8; opcode <= 0xdf; opcode++)
        for (unsigned modrm = 0xc0; modrm <= 0xff; modrm++) {
            FP_STEP(0xdb, 0xe3, 0, 0);
            dos_fpu_op_t op;
            bool admitted = dos_fpu_decode(opcode, modrm, true, &op);
            fault = fpu_test_step(vm, opcode, modrm, 0, 0);
            FP_CHECK(admitted ? !fault.raised : fault.raised && fault.vector == 6);
        }
    for (unsigned mode = 0; mode < 3; mode++) {
        fpu_test_mode(vm, mode);
        for (unsigned opcode = 0xd8; opcode <= 0xdf; opcode++)
            for (unsigned group = 0; group < 8; group++) {
                FP_STEP(0xdb, 0xe3, 0, 0);
                memset(data, 0, 108);
                dos_fpu_op_t op;
                bool admitted = dos_fpu_decode(opcode, group << 3, mode == 2, &op);
                fault = fpu_test_step(vm, opcode, group << 3, 0x4000, 0);
                FP_CHECK(admitted ? !fault.raised : fault.raised && fault.vector == 6);
            }
    }
    /* Client policy outlives CPU snapshots used for host calls/raw switches. */
    for (unsigned mode = 0; mode < 3; mode++) {
        for (unsigned control = 0; control < 4; control++) {
            fpu_test_mode(vm, mode);
            vm->dpmi.active = true;
            vm->dpmi.coprocessor_requested = false;
            uint32_t original_cr0 = cpu.cr0;
            cpu.eax = 0xa5a50e01;
            cpu.ebx = 0x1234fffcu | control;
            cpu.ecx = 0xcafebabe;
            cpu.edx = 0x87654321;
            cpu.eflags = FLAGS_FIXED | FLAG_CF | FLAG_DF | FLAG_IF;
            dos_int31_dpmi(vm);
            FP_CHECK(cpu.eax == 0xa5a50e01 && cpu.ebx == (0x1234fffcu | control) &&
                     cpu.ecx == 0xcafebabe && cpu.edx == 0x87654321 &&
                     cpu.eflags == (FLAGS_FIXED | FLAG_DF | FLAG_IF));
            FP_CHECK(cpu.cr0 == ((original_cr0 & ~6u) | (control << 1)));
            cpu.ax = 0x0e00;
            dos_int31_dpmi(vm);
            FP_CHECK(cpu.eax == (0xa5a50044u | control) && !(cpu.eflags & FLAG_CF));
            cpu.cr0 = original_cr0; /* A stale full-CPU restore cannot undo 0E01h. */
            fault = fpu_test_step(vm, 0xd9, 0xd0, 0, 0);
            FP_CHECK(control & 2u ? fault.raised && fault.vector == 7 : !fault.raised);
            FP_CHECK((cpu.cr0 & 6u) == (control << 1));
            dpmi_host_activation_t activation = dpmi_host_activation_save(&vm->dpmi);
            vm->dpmi.coprocessor_control = control ^ 3u;
            dpmi_host_activation_restore(vm, &activation);
            dpmi_apply_coprocessor(&cpu);
            FP_CHECK((cpu.cr0 & 6u) == ((control ^ 3u) << 1));
        }
    }
    vm->dpmi.active = vm->dpmi.coprocessor_requested = false;
    fpu_test_mode(vm, 2);
    extern void dos_set_native_vm(dos_vm_t *vm);
    extern int dos_native_session_active(void);
    extern unsigned dos_fpu_irq_selftest(void);
    extern unsigned dos_fpu_native_selftest(dos_vm_t *vm, unsigned *coverage);
    extern unsigned dos_fpu_backend_observe(unsigned *coverage);
    unsigned irq_failures = 7;
    unsigned native_failures = 15, native_coverage = 0;
    unsigned backend_loss = 0, backend_coverage = 0;
    if (!dos_native_session_active()) {
        dos_set_native_vm(vm);
        irq_failures = dos_fpu_irq_selftest();
        vm->native_fp->fcs = 0x137;
        vm->native_fp->fds = 0x147;
        native_failures = dos_fpu_native_selftest(vm, &native_coverage);
        backend_loss = dos_fpu_backend_observe(&backend_coverage);
        dos_set_native_vm(NULL);
    }
    FP_CHECK(!(irq_failures & 1u));
    FP_CHECK(!(irq_failures & 2u));
    FP_CHECK(!(irq_failures & 4u));
    FP_CHECK(!(native_failures & 1u));
    FP_CHECK(!(native_failures & 2u));
    FP_CHECK(!(native_failures & 4u));
    FP_CHECK(!(native_failures & 8u));
    serial_puts("[DOS-FPU-NATIVE] failure-mask="); serial_putdec(native_failures);
    serial_puts(" pointer-selector-coverage="); serial_putdec(native_coverage); serial_puts("\n");
    serial_puts("[DOS-FPU-BACKEND-OBSERVATION] loss-mask="); serial_putdec(backend_loss);
    serial_puts(" seed-coverage="); serial_putdec(backend_coverage); serial_puts(" irq-disabled=1\n");
    serial_puts("[DOS-FPU-ISR] failure-mask="); serial_putdec(irq_failures); serial_puts("\n");
    serial_puts("[DOS-FPU] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef FP_STEP
#undef FP_CHECK
done:
    dos_io_shutdown(vm);
    dos_host_free_pages(vm->native_fp, 1);
    dos_host_free_pages(vm->mem, 512);
    dos_host_free_pages(vm, vm_pages);
    return failures;
}
