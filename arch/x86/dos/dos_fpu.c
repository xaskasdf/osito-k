/* Guest memory, stack effects and exception delivery are owned by the layer. */
#include "dos_fpu.h"
#include "dos_fpu_softfloat.h"
#include "dos_fpu_state.h"
#include "cpu8086.h"
#include "dos_hostmem.h"
#include "dos_io.h"

extern void serial_puts(const char *);
extern void *memmove(void *dst, const void *src, uint64_t length);

static uint16_t fp_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t fp_u32(const uint8_t *p) { return fp_u16(p) | (uint32_t)fp_u16(p + 2) << 16; }
static void fp_w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void fp_w32(uint8_t *p, uint32_t v) { fp_w16(p, v); fp_w16(p + 2, v >> 16); }

bool dos_fpu_ensure(dos_vm_t *vm)
{
    if (vm->native_fp) return true;
    vm->native_fp = dos_host_alloc_pages(1);
    if (!vm->native_fp) {
        serial_puts("[DOS-FPU] state allocation failed\n");
        vm->cpu->running = false;
        vm->cpu->exit_code = -1;
        return false;
    }
    memset(vm->native_fp, 0, sizeof(*vm->native_fp));
    fp_w16(vm->native_fp->image, 0x037f);
    fp_w32(vm->native_fp->image + 24, 0x1f80);
    fp_w32(vm->native_fp->image + 28, 0xffff);
    fp_w32(vm->native_fp->image + 512, 3);
    return true;
}

bool dos_fpu_decode(uint8_t opcode, uint8_t modrm, bool op32, dos_fpu_op_t *op)
{
    *op = (dos_fpu_op_t){ .waiting = true };
    if (opcode < 0xd8 || opcode > 0xdf) return false;
    unsigned group = (modrm >> 3) & 7u;
    if (modrm >= 0xc0) {
        bool valid = false;
        switch (opcode) {
        case 0xd8: valid = true; break;
        case 0xd9:
            valid = modrm < 0xd0 || modrm == 0xd0 || modrm == 0xe0 || modrm == 0xe1 ||
                    modrm == 0xe4 || modrm == 0xe5 || (modrm >= 0xe8 && modrm <= 0xee) ||
                    modrm >= 0xf0;
            break;
        case 0xda: valid = modrm < 0xe0 || modrm == 0xe9; break;
        case 0xdb:
            valid = modrm <= 0xe4 || (modrm >= 0xe8 && modrm <= 0xf7);
            op->control = modrm >= 0xe0 && modrm <= 0xe4;
            op->waiting = !op->control;
            op->reset = modrm == 0xe3;
            break;
        case 0xdc: valid = modrm < 0xd0 || modrm >= 0xe0; break;
        case 0xdd: valid = modrm < 0xc8 || (modrm >= 0xd0 && modrm < 0xf0); break;
        case 0xde: valid = modrm < 0xd0 || modrm == 0xd9 || modrm >= 0xe0; break;
        case 0xdf:
            valid = modrm == 0xe0 || (modrm >= 0xe8 && modrm <= 0xf7);
            op->control = op->ax = modrm == 0xe0;
            op->waiting = !op->control;
            break;
        }
        if (!valid) return false;
        op->flags = (opcode == 0xdb || opcode == 0xdf) && modrm >= 0xe8 && modrm <= 0xf7;
        return true;
    }

    static const uint8_t sizes[8][8] = {
        {4,4,4,4,4,4,4,4}, {4,0,4,4,28,2,28,2},
        {4,4,4,4,4,4,4,4}, {4,4,4,4,0,10,0,10},
        {8,8,8,8,8,8,8,8}, {8,8,8,8,108,0,108,2},
        {2,2,2,2,2,2,2,2}, {2,2,2,2,10,8,10,8}
    };
    op->size = sizes[opcode - 0xd8][group];
    if (!op->size) return false;
    if (opcode & 1u) {
        op->write = group == 2 || group == 3 || group == 6 || group == 7 ||
                    (opcode != 0xd9 && group == 1);
        op->environment = (opcode == 0xd9 || opcode == 0xdd) && (group == 4 || group == 6);
        op->control = op->environment || (opcode == 0xd9 && (group == 5 || group == 7)) ||
                      (opcode == 0xdd && group == 7);
        op->waiting = !(op->control && op->write);
        op->restore = op->environment && !op->write;
        op->reset = opcode == 0xdd && group == 6;
        if (op->environment && !op32) op->size -= 14;
    }
    return true;
}

bool dos_fpu_pending(const dos_vm_t *vm)
{
    return vm->native_fp && (fp_u16(vm->native_fp->image + 2) & 0x80u);
}

void dos_fpu_sync_irq(dos_vm_t *vm)
{
    dos_native_fp_state_t *fp = vm->native_fp;
    if (!fp) return;
    if (!dos_fpu_pending(vm)) {
        fp->ferr = false;
        fp->ignne = false;
    } else if (!fp->ferr) {
        fp->ferr = fp->irq_pending = true;
        dos_io_raise_irq(vm, 13);
    }
}

void dos_fpu_ack_irq(dos_vm_t *vm)
{
    if (!vm->native_fp) return;
    vm->native_fp->irq_pending = false;
    vm->native_fp->ignne = true;
    dos_io_clear_irq(vm, 13);
}

static uint16_t fp_tag_word(const uint8_t *image)
{
    uint16_t tags = 0;
    unsigned top = (fp_u16(image + 2) >> 11) & 7u;
    for (unsigned physical = 0; physical < 8; physical++) {
        unsigned tag = 3;
        if (image[4] & (1u << physical)) {
            const uint8_t *reg = image + 32 + ((physical - top) & 7u) * 16;
            uint16_t exponent = fp_u16(reg + 8) & 0x7fffu;
            uint64_t significand = fp_u32(reg) | (uint64_t)fp_u32(reg + 4) << 32;
            tag = !exponent && !significand ? 1u :
                  !exponent || exponent == 0x7fffu || !(reg[7] & 0x80u) ? 2u : 0u;
        }
        tags |= tag << (physical * 2u);
    }
    return tags;
}

void dos_fpu_export_environment(const uint8_t *image, uint16_t fcs, uint16_t fds,
                                uint8_t *environment)
{
    memset(environment, 0, 28);
    fp_w16(environment, fp_u16(image));
    fp_w16(environment + 4, fp_u16(image + 2));
    fp_w16(environment + 8, fp_tag_word(image));
    fp_w32(environment + 12, fp_u32(image + 8));
    fp_w16(environment + 16, fcs);
    fp_w16(environment + 18, fp_u16(image + 6));
    fp_w32(environment + 20, fp_u32(image + 16));
    fp_w16(environment + 24, fds);
}

void dos_fpu_capture_native(dos_vm_t *vm, const void *frame)
{
    uint8_t *image = vm->native_fp->image;
    const uint8_t *env = (const uint8_t *)frame + DOS_NATIVE_FP_BYTES;
    memcpy(image, frame, DOS_NATIVE_FP_BYTES);
    /* A compatibility client has 32-bit offsets. FNSTENV also supplies
     * diagnostics which some CPUs omit from FXSAVE without a pending error. */
    fp_w16(image + 6, fp_u16(env + 18) & 0x7ffu);
    fp_w32(image + 8, fp_u32(env + 12));
    fp_w32(image + 12, 0);
    fp_w32(image + 16, fp_u32(env + 20));
    fp_w32(image + 20, 0);
    vm->native_fp->fcs = fp_u16(env + 16);
    vm->native_fp->fds = fp_u16(env + 24);
}

static void fp_stack_pop(uint8_t *image)
{
    uint16_t status = fp_u16(image + 2);
    unsigned top = (status >> 11) & 7u;
    uint8_t value[16];
    memcpy(value, image + 32, 16);
    memmove(image + 32, image + 48, 7 * 16);
    memcpy(image + 144, value, 16);
    image[4] &= ~(1u << top);
    fp_w16(image + 2, (status & ~0x3800u) | (((top + 1) & 7u) << 11));
}

static const uint8_t fp_indefinite[10] = {0,0,0,0,0,0,0,0xc0,0xff,0xff};

static void fp_stack_push(uint8_t *image, const uint8_t *value)
{
    uint16_t status = fp_u16(image + 2);
    unsigned next = (((status >> 11) & 7u) + 7u) & 7u;
    uint8_t prior[16];
    memcpy(prior, image + 144, 16);
    memmove(image + 48, image + 32, 7 * 16);
    memcpy(image + 32, prior, 16);
    memcpy(image + 32, value, 10);
    image[4] |= 1u << next;
    fp_w16(image + 2, (status & ~0x3800u) | (next << 11));
}

static void fp_indefinite_register(uint8_t *image, unsigned index)
{
    unsigned top = (fp_u16(image + 2) >> 11) & 7u;
    memcpy(image + 32 + index * 16, fp_indefinite, 10);
    image[4] |= 1u << ((top + index) & 7u);
}

/* Tag checks and masked stack responses belong to the guest architecture.
 * Valid operands then reach software arithmetic or register/control handling. */
static bool fp_stack_fault(uint8_t *image, const dos_fpu_op_t *op,
                           uint8_t opcode, uint8_t modrm,
                           uint8_t *operand, uint64_t *result)
{
    if (op->control) return false;
    unsigned group = (modrm >> 3) & 7u, index = modrm & 7u;
    bool memory = modrm < 0xc0;
    bool push = memory ? (opcode & 1u) && !op->write :
                opcode == 0xd9 && (modrm < 0xc8 || (modrm >= 0xe8 && modrm <= 0xee));
    bool store = memory ? op->write : opcode == 0xdd && modrm >= 0xd0 && modrm < 0xe0;
    bool compare = op->flags || (memory ? !(opcode & 1u) && (group == 2 || group == 3) :
        (opcode == 0xd8 && (group == 2 || group == 3)) ||
        (opcode == 0xdd && modrm >= 0xe0 && modrm < 0xf0) ||
        (opcode == 0xda && modrm == 0xe9) || (opcode == 0xde && modrm == 0xd9) ||
        (opcode == 0xd9 && modrm == 0xe4));
    bool exchange = !memory && opcode == 0xd9 && modrm >= 0xc8 && modrm < 0xd0;
    bool conditional = !memory && (opcode == 0xda || opcode == 0xdb) && modrm < 0xe0;
    bool arithmetic = !compare && !(opcode & 1u) &&
                      (memory || opcode == 0xd8 || opcode == 0xdc || opcode == 0xde);
    bool indexed = !memory && (arithmetic || exchange || conditional);
    bool second = false, pair = false, clear_c2 = false;
    unsigned destination = arithmetic && !memory && opcode != 0xd8 ? index : 0;
    unsigned pops = arithmetic && !memory && opcode == 0xde ? 1 : 0;
    if (!memory && opcode == 0xd9) {
        switch (modrm) {
        case 0xe0: case 0xe1: case 0xf0: case 0xfa: case 0xfc:
            arithmetic = true;
            break;
        case 0xf1: case 0xf3: case 0xf9:
            arithmetic = second = true;
            destination = pops = 1;
            break;
        case 0xf2: case 0xfb:
            clear_c2 = true;
            /* fall through */
        case 0xf4:
            arithmetic = pair = true;
            push = true;
            break;
        case 0xf5: case 0xf8:
            clear_c2 = true;
            /* fall through */
        case 0xfd:
            arithmetic = second = true;
            break;
        case 0xfe: case 0xff:
            arithmetic = clear_c2 = true;
            break;
        }
    }
    if (!push && !store && !compare && !arithmetic && !exchange && !conditional) return false;

    uint16_t status = fp_u16(image + 2);
    unsigned top = (status >> 11) & 7u, next = (top - 1) & 7u;
    unsigned sources = store || compare || arithmetic || exchange || conditional ? 1u << top : 0;
    if (!memory && ((push && modrm < 0xc8) || (compare && opcode != 0xd9) || indexed))
        sources |= 1u << ((top + index) & 7u);
    if (second) sources |= 1u << ((top + 1) & 7u);
    bool underflow = (image[4] & sources) != sources;
    bool overflow = push && (image[4] & (1u << next)) && !(pair && underflow);
    if (!overflow && !underflow) return false;

    status = (status & ~0x0200u) | 0x0041u | (overflow ? 0x0200u : 0);
    if (clear_c2) status &= ~0x0400u;
    if (!(fp_u16(image) & 1u)) {
        fp_w16(image + 2, status | 0x8080u);
        return true;
    }
    fp_w16(image + 2, status);
    if (push) {
        if (pair) fp_indefinite_register(image, 0);
        fp_stack_push(image, fp_indefinite);
    } else if (store) {
        if (!memory) {
            fp_indefinite_register(image, index);
        } else {
            memset(operand, 0, op->size);
            if (opcode == 0xd9) fp_w32(operand, 0xffc00000u);
            else if (opcode == 0xdd && group != 1) fp_w32(operand + 4, 0xfff80000u);
            else if (op->size == 10) memcpy(operand, fp_indefinite, 10); /* real80 or BCD */
            else operand[op->size - 1] = 0x80; /* integer indefinite */
        }
        if (group != 2) fp_stack_pop(image);
    } else if (compare) {
        if (op->flags) *result = (uint64_t)(FLAG_CF | FLAG_PF | FLAG_ZF) << 32;
        else fp_w16(image + 2, status | 0x4500u);
        pops = memory ? group == 3 :
            (opcode == 0xda || opcode == 0xde) ? 2 :
            opcode == 0xdf || (opcode == 0xd8 && group == 3) ||
            (opcode == 0xdd && group == 5);
        while (pops--) fp_stack_pop(image);
    } else if (exchange) {
        /* Only empty inputs become indefinite; exchange the valid input too. */
        if (!(image[4] & (1u << top))) fp_indefinite_register(image, 0);
        if (!(image[4] & (1u << ((top + index) & 7u)))) fp_indefinite_register(image, index);
        if (index) {
            uint8_t value[10];
            memcpy(value, image + 32, 10);
            memcpy(image + 32, image + 32 + index * 16, 10);
            memcpy(image + 32 + index * 16, value, 10);
        }
    } else {
        fp_indefinite_register(image, destination);
        if (pops) fp_stack_pop(image);
    }
    return true;
}

/* Normalize the 14/28-byte guest environment to a protected 28-byte layout. */
static void fp_load_environment(uint8_t *out, const uint8_t *in, bool wide, bool pm)
{
    unsigned stride = wide ? 4 : 2;
    memset(out, 0, 28);
    fp_w16(out, fp_u16(in));
    fp_w16(out + 4, fp_u16(in + stride));
    fp_w16(out + 8, fp_u16(in + 2 * stride));
    uint32_t ip = wide && pm ? fp_u32(in + 12) : fp_u16(in + 3 * stride);
    uint32_t dp = wide && pm ? fp_u32(in + 20) : fp_u16(in + 5 * stride);
    if (pm) {
        fp_w16(out + 16, fp_u16(in + 4 * stride));
        fp_w16(out + 24, fp_u16(in + 6 * stride));
        if (wide) fp_w16(out + 18, fp_u16(in + 18) & 0x7ffu);
    } else {
        uint32_t ih = wide ? fp_u32(in + 16) : fp_u16(in + 8);
        uint32_t dh = wide ? fp_u32(in + 24) : fp_u16(in + 12);
        ip |= (ih & (wide ? 0x0ffff000u : 0xf000u)) << 4;
        dp |= (dh & (wide ? 0x0ffff000u : 0xf000u)) << 4;
        fp_w16(out + 18, ih & 0x7ffu);
    }
    fp_w32(out + 12, ip);
    fp_w32(out + 20, dp);
}

static void fp_store_environment(uint8_t *out, const uint8_t *env, bool wide, bool pm)
{
    unsigned stride = wide ? 4 : 2;
    memset(out, 0, wide ? 28 : 14);
    for (unsigned i = 0; i < 3; i++) {
        fp_w16(out + i * stride, fp_u16(env + i * 4));
        if (wide) fp_w16(out + i * stride + 2, 0xffff);
    }
    uint32_t ip = fp_u32(env + 12), dp = fp_u32(env + 20);
    if (pm) {
        if (wide) { fp_w32(out + 12, ip); fp_w32(out + 20, dp); }
        else { fp_w16(out + 6, ip); fp_w16(out + 10, dp); }
        fp_w16(out + 4 * stride, fp_u16(env + 16));
        fp_w16(out + 6 * stride, fp_u16(env + 24));
        if (wide) fp_w16(out + 18, fp_u16(env + 18) & 0x7ffu);
    } else {
        fp_w16(out + 3 * stride, ip);
        fp_w16(out + 5 * stride, dp);
        uint32_t ih = ((ip >> 4) & (wide ? 0x0ffff000u : 0xf000u)) | (fp_u16(env + 18) & 0x7ffu);
        uint32_t dh = (dp >> 4) & (wide ? 0x0ffff000u : 0xf000u);
        if (wide) { fp_w32(out + 16, ih); fp_w32(out + 24, dh); }
        else { fp_w16(out + 8, ih); fp_w16(out + 12, dh); }
    }
}

static bool fp_software_arithmetic(uint8_t *image, uint8_t *operand,
                                    uint8_t opcode, uint8_t modrm, uint64_t *flags)
{
    dos_fpu_numeric_result_t result;
    if (!dos_fpu_soft_evaluate(image, operand, opcode, modrm, &result)) return false;
    uint16_t status = (fp_u16(image + 2) & ~result.condition_clear) |
                      result.exceptions | result.conditions;
    if (status & ~fp_u16(image) & 0x3fu) status |= 0x8080u;
    fp_w16(image + 2, status);
    if (result.write) memcpy(image + 32 + result.destination * 16, result.value, 10);
    if (result.memory_size) memcpy(operand, result.value, result.memory_size);
    for (unsigned pop = 0; pop < result.pops; pop++) fp_stack_pop(image);
    if (result.push) fp_stack_push(image, result.pushed_value);
    *flags = (uint64_t)result.integer_flags << 32;
    return true;
}

bool dos_fpu_execute(dos_vm_t *vm, const dos_fpu_op_t *op,
                       uint8_t opcode, uint8_t modrm, bool op32,
                       uint32_t ip, unsigned segment, uint32_t offset,
                       uint8_t *operand)
{
    cpu8086_state_t *cpu = vm->cpu;
    dos_native_fp_state_t *fp = vm->native_fp;
    uint8_t *image = fp->image;
    uint8_t stage[108] = {0}, prior[28] = {0};
    bool pm = cpu->protected_mode && !(cpu->eflags & FLAG_VM);
    dos_fpu_export_environment(image, fp->fcs, fp->fds, prior);
    unsigned env_size = op32 ? 28 : 14;
    if (op->restore) {
        fp_load_environment(stage, operand, op32, pm);
        if (opcode == 0xdd) memcpy(stage + 28, operand + env_size, 80);
    } else if (op->size && !op->write) memcpy(stage, operand, op->size);

    uint16_t control = fp_u16(image);
    bool checkpoint = !op->control && (control & 0x1fu) != 0x1fu;
    uint8_t stack[128], tags = image[4];
    if (checkpoint) memcpy(stack, image + 32, sizeof(stack));
    uint16_t ignored = 0;
    if (op->waiting && !(cpu->cr0 & 0x20u) && fp->ignne && dos_fpu_pending(vm)) {
        ignored = fp_u16(image + 2) & ~control & 0x3fu;
        fp_w16(image + 2, fp_u16(image + 2) & ~(ignored | 0x8080u));
    }
    uint64_t result = 0;
    if (!fp_stack_fault(image, op, opcode, modrm, stage, &result) &&
        !fp_software_arithmetic(image, stage, opcode, modrm, &result) &&
        !dos_fpu_state_step(image, stage, opcode, modrm, cpu->eflags, &result)) {
        serial_puts("[DOS-FPU] admitted instruction has no software handler\n");
        cpu->running = false;
        cpu->exit_code = -1;
        return false;
    }
    uint16_t pending = fp_u16(image + 2) & ~fp_u16(image) & 0x3fu;
    /* #I/#D/#Z inhibit register results too. FLD m32/m64 still pushes on
     * unmasked #D; register #O/#U store a biased result, and #P completes. */
    bool denormal_load = op->size && !op->write && !op->control &&
        (opcode == 0xd9 || opcode == 0xdd) && !(modrm & 0x38u);
    uint16_t inhibit = 5u | (denormal_load ? 0 : 2u) | (op->write ? 0x18u : 0);
    bool stored = op->control || !(pending & inhibit);
    if (checkpoint && !stored) {
        /* Keep exception status and diagnostic pointers, but discard the
         * inhibited register writes, pushes and pops. */
        memcpy(image + 32, stack, sizeof(stack));
        image[4] = tags;
        fp_w16(image + 2, (fp_u16(image + 2) & ~0x3800u) | (fp_u16(prior + 4) & 0x3800u));
    }
    if (ignored && !op->restore) {
        uint16_t status = fp_u16(image + 2) | ignored;
        if (status & ~fp_u16(image) & 0x3fu) status |= 0x8080u;
        fp_w16(image + 2, status);
    }
    if (op->environment && op->write) {
        /* Serialize guest diagnostic pointers, never staging addresses. */
        memcpy(stage + 12, prior + 12, 16);
        fp_store_environment(operand, stage, op32, pm);
        if (opcode == 0xdd) memcpy(operand + env_size, stage + 28, 80);
        stored = true;
    } else if (op->write && stored) memcpy(operand, stage, op->size);

    if (op->reset) {
        fp->fcs = fp->fds = 0;
    } else if (op->restore) {
        fp_w32(image + 8, fp_u32(stage + 12));
        fp_w16(image + 6, fp_u16(stage + 18));
        fp_w32(image + 16, fp_u32(stage + 20));
        fp->fcs = fp_u16(stage + 16);
        fp->fds = fp_u16(stage + 24);
    } else if (!op->control) {
        fp_w32(image + 8, pm ? ip : cpu_segment_linear(cpu, 1, ip));
        fp_w32(image + 12, 0);
        fp_w16(image + 6, ((opcode & 7u) << 8) | modrm);
        fp->fcs = pm ? cpu->cs : 0;
        if (op->size) {
            fp_w32(image + 16, pm ? offset : cpu_segment_linear(cpu, segment, offset));
            fp_w32(image + 20, 0);
            const uint16_t selectors[] = { cpu->es, cpu->cs, cpu->ss, cpu->ds, cpu->fs, cpu->gs };
            fp->fds = pm ? selectors[segment] : 0;
        } else {
            memcpy(image + 16, prior + 20, 4);
            fp_w32(image + 20, 0);
        }
    } else {
        memcpy(image + 8, prior + 12, 4);
        fp_w32(image + 12, 0);
        memcpy(image + 16, prior + 20, 4);
        fp_w32(image + 20, 0);
        fp_w16(image + 6, fp_u16(prior + 18));
    }
    image[512] |= 1; /* x87 must be restored on the next native XSAVE entry. */
    if (op->ax) cpu->ax = result;
    if (op->flags) {
        /* Intel clears these even for an unmasked invalid comparison. Older
         * host emulators and other CPU families need not have that behavior. */
        cpu->eflags &= ~(FLAG_OF | FLAG_SF | FLAG_AF);
        fp_w16(image + 2, fp_u16(image + 2) & ~0x0200u);
        if (!(pending & 1u)) {
            const uint32_t mask = FLAG_CF | FLAG_PF | FLAG_ZF;
            cpu->eflags = (cpu->eflags & ~mask) | ((uint32_t)(result >> 32) & mask);
        }
    }
    dos_fpu_sync_irq(vm);
    return stored;
}
