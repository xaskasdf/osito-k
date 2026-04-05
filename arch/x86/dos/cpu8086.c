/*
 * OsitoK -- 8086 CPU Emulator
 *
 * Software interpreter for Intel 8086/80186 instruction set.
 * Runs 16-bit DOS code within the 64-bit OsitoK kernel.
 *
 * No libc -- bare-metal kernel environment.
 */

#include "cpu8086.h"
#include "dos_jit.h"

/* ── External interfaces ─────────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* INT dispatch (dos_int.c) */
extern void dos_int_dispatch(dos_vm_t *vm, uint8_t int_num);

/* VGA flush (dos_vga.c) */
extern void dos_vga_flush(dos_vm_t *vm);
extern void dos_vga_mark_dirty(dos_vm_t *vm, uint32_t addr);

/* I/O ports (dos_io.c) */
extern uint8_t  dos_io_read8(dos_vm_t *vm, uint16_t port);
extern uint16_t dos_io_read16(dos_vm_t *vm, uint16_t port);
extern void     dos_io_write8(dos_vm_t *vm, uint16_t port, uint8_t val);
extern void     dos_io_write16(dos_vm_t *vm, uint16_t port, uint16_t val);

/* ── Memory write (with VGA dirty tracking) ──────────────────────── */

void dos_mem_write8(dos_vm_t *vm, uint32_t addr, uint8_t val)
{
    if (addr >= DOS_MEM_SIZE)
        return;
    vm->mem[addr] = val;
    if (addr >= DOS_VRAM_BASE && addr < DOS_VRAM_BASE + DOS_VRAM_SIZE)
        dos_vga_mark_dirty(vm, addr);
}

void dos_mem_write16(dos_vm_t *vm, uint32_t addr, uint16_t val)
{
    dos_mem_write8(vm, addr,     (uint8_t)(val & 0xFF));
    dos_mem_write8(vm, addr + 1, (uint8_t)(val >> 8));
}

void dos_mem_write32(dos_vm_t *vm, uint32_t addr, uint32_t val)
{
    dos_mem_write16(vm, addr, (uint16_t)(val & 0xFFFF));
    dos_mem_write16(vm, addr + 2, (uint16_t)(val >> 16));
}

/* ── CPU initialization ──────────────────────────────────────────── */

void cpu8086_init(cpu8086_state_t *cpu, dos_vm_t *vm)
{
    cpu->eax = 0; cpu->ebx = 0; cpu->ecx = 0; cpu->edx = 0;
    cpu->esi = 0; cpu->edi = 0; cpu->esp = 0; cpu->ebp = 0;
    cpu->cs = 0; cpu->ds = 0; cpu->es = 0; cpu->ss = 0;
    cpu->fs = 0; cpu->gs = 0;
    cpu->eip = 0;
    cpu->eflags = FLAGS_FIXED;
    cpu->cr0 = 0; cpu->cr2 = 0; cpu->cr3 = 0;
    cpu->gdtr.limit = 0; cpu->gdtr.base = 0;
    cpu->idtr.limit = 0; cpu->idtr.base = 0;
    cpu->protected_mode = false;
    cpu->op_size_32     = false;
    cpu->addr_size_32   = false;
    cpu->prefix_66      = false;
    cpu->prefix_67      = false;
    cpu->running   = true;
    cpu->halted    = false;
    cpu->exit_code = 0;
    cpu->insn_count = 0;
    cpu->seg_override = -1;
    cpu->rep_active = false;
    cpu->rep_type   = 0;
    cpu->vm = vm;
}

/* ── Register pointer helpers ────────────────────────────────────── */

/*
 * Return a pointer to the 8-bit register selected by 'reg' (0-7).
 * Encoding: 0=AL, 1=CL, 2=DL, 3=BL, 4=AH, 5=CH, 6=DH, 7=BH
 */
static uint8_t *reg8_ptr(cpu8086_state_t *cpu, uint8_t reg)
{
    switch (reg & 7) {
    case 0: return &cpu->al;
    case 1: return &cpu->cl;
    case 2: return &cpu->dl;
    case 3: return &cpu->bl;
    case 4: return &cpu->ah;
    case 5: return &cpu->ch;
    case 6: return &cpu->dh;
    case 7: return &cpu->bh;
    }
    return &cpu->al; /* unreachable */
}

/*
 * Return a pointer to the 16-bit register selected by 'reg' (0-7).
 * Encoding: 0=AX, 1=CX, 2=DX, 3=BX, 4=SP, 5=BP, 6=SI, 7=DI
 */
static uint16_t *reg16_ptr(cpu8086_state_t *cpu, uint8_t reg)
{
    switch (reg & 7) {
    case 0: return &cpu->ax;
    case 1: return &cpu->cx;
    case 2: return &cpu->dx;
    case 3: return &cpu->bx;
    case 4: return &cpu->sp;
    case 5: return &cpu->bp;
    case 6: return &cpu->si;
    case 7: return &cpu->di;
    }
    return &cpu->ax; /* unreachable */
}

/*
 * Return a pointer to the 32-bit register selected by 'reg' (0-7).
 * Encoding: 0=EAX, 1=ECX, 2=EDX, 3=EBX, 4=ESP, 5=EBP, 6=ESI, 7=EDI
 */
static uint32_t *reg32_ptr(cpu8086_state_t *cpu, uint8_t reg)
{
    switch (reg & 7) {
    case 0: return &cpu->eax;
    case 1: return &cpu->ecx;
    case 2: return &cpu->edx;
    case 3: return &cpu->ebx;
    case 4: return &cpu->esp;
    case 5: return &cpu->ebp;
    case 6: return &cpu->esi;
    case 7: return &cpu->edi;
    }
    return &cpu->eax; /* unreachable */
}

/*
 * Return a pointer to a segment register by index.
 * 0=ES, 1=CS, 2=SS, 3=DS, 4=FS, 5=GS
 */
static uint16_t *seg_ptr(cpu8086_state_t *cpu, uint8_t idx)
{
    switch (idx & 7) {
    case 0: return &cpu->es;
    case 1: return &cpu->cs;
    case 2: return &cpu->ss;
    case 3: return &cpu->ds;
    case 4: return &cpu->fs;
    case 5: return &cpu->gs;
    }
    return &cpu->ds; /* unreachable */
}

/* ── ModRM decoding ──────────────────────────────────────────────── */

/*
 * Decoded ModRM result.
 * If is_reg == true:  the operand is a register (use reg8/reg16 pointer).
 * If is_reg == false: the operand is a memory address (use addr).
 */
typedef struct {
    uint32_t addr;        /* linear address (memory operand) */
    uint8_t  reg_field;   /* the /reg field (bits 5-3) */
    uint8_t  rm_field;    /* the r/m field (bits 2-0) */
    uint8_t  mod_field;   /* the mod field (bits 7-6) */
    bool     is_reg;      /* true if mod==11 (register operand) */
} modrm_t;

static modrm_t decode_modrm(cpu8086_state_t *cpu, uint8_t modrm)
{
    modrm_t result;
    result.mod_field = (modrm >> 6) & 3;
    result.reg_field = (modrm >> 3) & 7;
    result.rm_field  = modrm & 7;
    result.is_reg    = false;
    result.addr      = 0;

    dos_vm_t *vm = cpu->vm;
    (void)vm;

    if (result.mod_field == 3) {
        /* Register operand -- no memory access */
        result.is_reg = true;
        return result;
    }

    /* Compute the effective address (offset within segment) */
    uint16_t offset = 0;
    bool use_ss = false; /* true when BP is base -> default segment is SS */

    switch (result.rm_field) {
    case 0: offset = cpu->bx + cpu->si; break;
    case 1: offset = cpu->bx + cpu->di; break;
    case 2: offset = cpu->bp + cpu->si; use_ss = true; break;
    case 3: offset = cpu->bp + cpu->di; use_ss = true; break;
    case 4: offset = cpu->si; break;
    case 5: offset = cpu->di; break;
    case 6:
        if (result.mod_field == 0) {
            /* Direct address */
            offset = cpu_fetch16(cpu);
        } else {
            offset = cpu->bp;
            use_ss = true;
        }
        break;
    case 7: offset = cpu->bx; break;
    }

    /* Add displacement */
    if (result.mod_field == 1) {
        int8_t disp8 = (int8_t)cpu_fetch8(cpu);
        offset += (uint16_t)(int16_t)disp8;
    } else if (result.mod_field == 2) {
        uint16_t disp16 = cpu_fetch16(cpu);
        offset += disp16;
    }

    /* Determine segment */
    uint16_t seg;
    if (cpu->seg_override >= 0) {
        seg = *seg_ptr(cpu, (uint8_t)cpu->seg_override);
    } else if (use_ss) {
        seg = cpu->ss;
    } else {
        seg = cpu->ds;
    }

    result.addr = dos_linear(seg, offset);
    return result;
}

/*
 * Read an 8-bit value from a decoded ModRM operand.
 */
static uint8_t modrm_read8(cpu8086_state_t *cpu, modrm_t *m)
{
    if (m->is_reg)
        return *reg8_ptr(cpu, m->rm_field);
    return dos_mem_read8(cpu->vm, m->addr);
}

/*
 * Write an 8-bit value to a decoded ModRM operand.
 */
static void modrm_write8(cpu8086_state_t *cpu, modrm_t *m, uint8_t val)
{
    if (m->is_reg)
        *reg8_ptr(cpu, m->rm_field) = val;
    else
        dos_mem_write8(cpu->vm, m->addr, val);
}

/*
 * Read a 16-bit value from a decoded ModRM operand.
 */
static uint16_t modrm_read16(cpu8086_state_t *cpu, modrm_t *m)
{
    if (m->is_reg)
        return *reg16_ptr(cpu, m->rm_field);
    return dos_mem_read16(cpu->vm, m->addr);
}

/*
 * Write a 16-bit value to a decoded ModRM operand.
 */
static void modrm_write16(cpu8086_state_t *cpu, modrm_t *m, uint16_t val)
{
    if (m->is_reg)
        *reg16_ptr(cpu, m->rm_field) = val;
    else
        dos_mem_write16(cpu->vm, m->addr, val);
}

/*
 * Read a 32-bit value from a decoded ModRM operand.
 */
static uint32_t modrm_read32(cpu8086_state_t *cpu, modrm_t *m)
{
    if (m->is_reg)
        return *reg32_ptr(cpu, m->rm_field);
    return dos_mem_read32(cpu->vm, m->addr);
}

/*
 * Write a 32-bit value to a decoded ModRM operand.
 */
static void modrm_write32(cpu8086_state_t *cpu, modrm_t *m, uint32_t val)
{
    if (m->is_reg)
        *reg32_ptr(cpu, m->rm_field) = val;
    else
        dos_mem_write32(cpu->vm, m->addr, val);
}

/* ── Flag helpers ────────────────────────────────────────────────── */

static inline void set_flag(cpu8086_state_t *cpu, uint16_t flag, bool val)
{
    if (val)
        cpu->flags |= flag;
    else
        cpu->flags &= ~flag;
}

static inline bool get_flag(cpu8086_state_t *cpu, uint16_t flag)
{
    return (cpu->flags & flag) != 0;
}

/* Update CF, ZF, SF, PF, AF, OF for 8-bit addition */
static void update_flags_add8(cpu8086_state_t *cpu, uint8_t a, uint8_t b, uint16_t result)
{
    uint8_t r8 = (uint8_t)result;
    set_flag(cpu, FLAG_CF, result > 0xFF);
    set_flag(cpu, FLAG_ZF, r8 == 0);
    set_flag(cpu, FLAG_SF, (r8 & 0x80) != 0);
    set_flag(cpu, FLAG_PF, parity8(r8));
    set_flag(cpu, FLAG_AF, ((a ^ b ^ r8) & 0x10) != 0);
    /* Overflow: both operands same sign, result different sign */
    set_flag(cpu, FLAG_OF, ((~(a ^ b)) & (a ^ r8) & 0x80) != 0);
}

/* Update CF, ZF, SF, PF, AF, OF for 16-bit addition */
static void update_flags_add16(cpu8086_state_t *cpu, uint16_t a, uint16_t b, uint32_t result)
{
    uint16_t r16 = (uint16_t)result;
    set_flag(cpu, FLAG_CF, result > 0xFFFF);
    set_flag(cpu, FLAG_ZF, r16 == 0);
    set_flag(cpu, FLAG_SF, (r16 & 0x8000) != 0);
    set_flag(cpu, FLAG_PF, parity8((uint8_t)r16));
    set_flag(cpu, FLAG_AF, ((a ^ b ^ r16) & 0x10) != 0);
    set_flag(cpu, FLAG_OF, ((~(a ^ b)) & (a ^ r16) & 0x8000) != 0);
}

/* Update CF, ZF, SF, PF, AF, OF for 8-bit subtraction (a - b = result) */
static void update_flags_sub8(cpu8086_state_t *cpu, uint8_t a, uint8_t b, uint16_t result)
{
    uint8_t r8 = (uint8_t)result;
    set_flag(cpu, FLAG_CF, a < b);
    set_flag(cpu, FLAG_ZF, r8 == 0);
    set_flag(cpu, FLAG_SF, (r8 & 0x80) != 0);
    set_flag(cpu, FLAG_PF, parity8(r8));
    set_flag(cpu, FLAG_AF, ((a ^ b ^ r8) & 0x10) != 0);
    /* Overflow: operands different sign, result different sign from a */
    set_flag(cpu, FLAG_OF, (((a ^ b)) & (a ^ r8) & 0x80) != 0);
}

/* Update CF, ZF, SF, PF, AF, OF for 16-bit subtraction */
static void update_flags_sub16(cpu8086_state_t *cpu, uint16_t a, uint16_t b, uint32_t result)
{
    uint16_t r16 = (uint16_t)result;
    set_flag(cpu, FLAG_CF, a < b);
    set_flag(cpu, FLAG_ZF, r16 == 0);
    set_flag(cpu, FLAG_SF, (r16 & 0x8000) != 0);
    set_flag(cpu, FLAG_PF, parity8((uint8_t)r16));
    set_flag(cpu, FLAG_AF, ((a ^ b ^ r16) & 0x10) != 0);
    set_flag(cpu, FLAG_OF, (((a ^ b)) & (a ^ r16) & 0x8000) != 0);
}

/* Update ZF, SF, PF and clear CF, OF for 8-bit logical operations */
static void update_flags_logic8(cpu8086_state_t *cpu, uint8_t result)
{
    set_flag(cpu, FLAG_CF, false);
    set_flag(cpu, FLAG_OF, false);
    set_flag(cpu, FLAG_ZF, result == 0);
    set_flag(cpu, FLAG_SF, (result & 0x80) != 0);
    set_flag(cpu, FLAG_PF, parity8(result));
    set_flag(cpu, FLAG_AF, false); /* undefined, but many impls clear it */
}

/* Update ZF, SF, PF and clear CF, OF for 16-bit logical operations */
static void update_flags_logic16(cpu8086_state_t *cpu, uint16_t result)
{
    set_flag(cpu, FLAG_CF, false);
    set_flag(cpu, FLAG_OF, false);
    set_flag(cpu, FLAG_ZF, result == 0);
    set_flag(cpu, FLAG_SF, (result & 0x8000) != 0);
    set_flag(cpu, FLAG_PF, parity8((uint8_t)result));
    set_flag(cpu, FLAG_AF, false);
}

/* ── 32-bit flag helpers (386+) ─────────────────────────────────── */

static void update_flags_add32(cpu8086_state_t *cpu, uint32_t a, uint32_t b, uint64_t result)
{
    uint32_t r32 = (uint32_t)result;
    set_flag(cpu, FLAG_CF, result > 0xFFFFFFFFULL);
    set_flag(cpu, FLAG_ZF, r32 == 0);
    set_flag(cpu, FLAG_SF, (r32 & 0x80000000U) != 0);
    set_flag(cpu, FLAG_PF, parity8((uint8_t)r32));
    set_flag(cpu, FLAG_AF, ((a ^ b ^ r32) & 0x10) != 0);
    set_flag(cpu, FLAG_OF, ((~(a ^ b)) & (a ^ r32) & 0x80000000U) != 0);
}

static void update_flags_sub32(cpu8086_state_t *cpu, uint32_t a, uint32_t b, uint64_t result)
{
    uint32_t r32 = (uint32_t)result;
    set_flag(cpu, FLAG_CF, a < b);
    set_flag(cpu, FLAG_ZF, r32 == 0);
    set_flag(cpu, FLAG_SF, (r32 & 0x80000000U) != 0);
    set_flag(cpu, FLAG_PF, parity8((uint8_t)r32));
    set_flag(cpu, FLAG_AF, ((a ^ b ^ r32) & 0x10) != 0);
    set_flag(cpu, FLAG_OF, (((a ^ b)) & (a ^ r32) & 0x80000000U) != 0);
}

static void update_flags_logic32(cpu8086_state_t *cpu, uint32_t result)
{
    set_flag(cpu, FLAG_CF, false);
    set_flag(cpu, FLAG_OF, false);
    set_flag(cpu, FLAG_ZF, result == 0);
    set_flag(cpu, FLAG_SF, (result & 0x80000000U) != 0);
    set_flag(cpu, FLAG_PF, parity8((uint8_t)result));
    set_flag(cpu, FLAG_AF, false);
}

/* ── ALU operation helpers for Group 1 ───────────────────────────── */

static uint8_t alu_add8(cpu8086_state_t *cpu, uint8_t a, uint8_t b)
{
    uint16_t result = (uint16_t)a + (uint16_t)b;
    update_flags_add8(cpu, a, b, result);
    return (uint8_t)result;
}

static uint16_t alu_add16(cpu8086_state_t *cpu, uint16_t a, uint16_t b)
{
    uint32_t result = (uint32_t)a + (uint32_t)b;
    update_flags_add16(cpu, a, b, result);
    return (uint16_t)result;
}

static uint8_t alu_adc8(cpu8086_state_t *cpu, uint8_t a, uint8_t b)
{
    uint8_t carry = get_flag(cpu, FLAG_CF) ? 1 : 0;
    uint16_t result = (uint16_t)a + (uint16_t)b + carry;
    update_flags_add8(cpu, a, b + carry, result);
    /* Re-check CF/AF more precisely for ADC */
    set_flag(cpu, FLAG_CF, result > 0xFF);
    set_flag(cpu, FLAG_AF, ((a ^ b ^ (uint8_t)result) & 0x10) != 0);
    set_flag(cpu, FLAG_OF, ((~(a ^ b)) & (a ^ (uint8_t)result) & 0x80) != 0);
    return (uint8_t)result;
}

static uint16_t alu_adc16(cpu8086_state_t *cpu, uint16_t a, uint16_t b)
{
    uint16_t carry = get_flag(cpu, FLAG_CF) ? 1 : 0;
    uint32_t result = (uint32_t)a + (uint32_t)b + carry;
    update_flags_add16(cpu, a, b + carry, result);
    set_flag(cpu, FLAG_CF, result > 0xFFFF);
    set_flag(cpu, FLAG_AF, ((a ^ b ^ (uint16_t)result) & 0x10) != 0);
    set_flag(cpu, FLAG_OF, ((~(a ^ b)) & (a ^ (uint16_t)result) & 0x8000) != 0);
    return (uint16_t)result;
}

static uint8_t alu_sub8(cpu8086_state_t *cpu, uint8_t a, uint8_t b)
{
    uint16_t result = (uint16_t)a - (uint16_t)b;
    update_flags_sub8(cpu, a, b, result);
    return (uint8_t)result;
}

static uint16_t alu_sub16(cpu8086_state_t *cpu, uint16_t a, uint16_t b)
{
    uint32_t result = (uint32_t)a - (uint32_t)b;
    update_flags_sub16(cpu, a, b, result);
    return (uint16_t)result;
}

static uint8_t alu_sbb8(cpu8086_state_t *cpu, uint8_t a, uint8_t b)
{
    uint8_t borrow = get_flag(cpu, FLAG_CF) ? 1 : 0;
    uint16_t result = (uint16_t)a - (uint16_t)b - borrow;
    uint8_t r8 = (uint8_t)result;
    set_flag(cpu, FLAG_CF, (uint16_t)a < (uint16_t)b + borrow);
    set_flag(cpu, FLAG_ZF, r8 == 0);
    set_flag(cpu, FLAG_SF, (r8 & 0x80) != 0);
    set_flag(cpu, FLAG_PF, parity8(r8));
    set_flag(cpu, FLAG_AF, ((a ^ b ^ r8) & 0x10) != 0);
    set_flag(cpu, FLAG_OF, (((a ^ b)) & (a ^ r8) & 0x80) != 0);
    return r8;
}

static uint16_t alu_sbb16(cpu8086_state_t *cpu, uint16_t a, uint16_t b)
{
    uint16_t borrow = get_flag(cpu, FLAG_CF) ? 1 : 0;
    uint32_t result = (uint32_t)a - (uint32_t)b - borrow;
    uint16_t r16 = (uint16_t)result;
    set_flag(cpu, FLAG_CF, (uint32_t)a < (uint32_t)b + borrow);
    set_flag(cpu, FLAG_ZF, r16 == 0);
    set_flag(cpu, FLAG_SF, (r16 & 0x8000) != 0);
    set_flag(cpu, FLAG_PF, parity8((uint8_t)r16));
    set_flag(cpu, FLAG_AF, ((a ^ b ^ r16) & 0x10) != 0);
    set_flag(cpu, FLAG_OF, (((a ^ b)) & (a ^ r16) & 0x8000) != 0);
    return r16;
}

static uint8_t alu_or8(cpu8086_state_t *cpu, uint8_t a, uint8_t b)
{
    uint8_t result = a | b;
    update_flags_logic8(cpu, result);
    return result;
}

static uint16_t alu_or16(cpu8086_state_t *cpu, uint16_t a, uint16_t b)
{
    uint16_t result = a | b;
    update_flags_logic16(cpu, result);
    return result;
}

static uint8_t alu_and8(cpu8086_state_t *cpu, uint8_t a, uint8_t b)
{
    uint8_t result = a & b;
    update_flags_logic8(cpu, result);
    return result;
}

static uint16_t alu_and16(cpu8086_state_t *cpu, uint16_t a, uint16_t b)
{
    uint16_t result = a & b;
    update_flags_logic16(cpu, result);
    return result;
}

static uint8_t alu_xor8(cpu8086_state_t *cpu, uint8_t a, uint8_t b)
{
    uint8_t result = a ^ b;
    update_flags_logic8(cpu, result);
    return result;
}

static uint16_t alu_xor16(cpu8086_state_t *cpu, uint16_t a, uint16_t b)
{
    uint16_t result = a ^ b;
    update_flags_logic16(cpu, result);
    return result;
}

/* CMP is SUB without writeback */
static void alu_cmp8(cpu8086_state_t *cpu, uint8_t a, uint8_t b)
{
    alu_sub8(cpu, a, b);
}

static void alu_cmp16(cpu8086_state_t *cpu, uint16_t a, uint16_t b)
{
    alu_sub16(cpu, a, b);
}

/* ── 32-bit ALU helpers (386+) ──────────────────────────────────── */

static uint32_t alu_add32(cpu8086_state_t *cpu, uint32_t a, uint32_t b)
{
    uint64_t result = (uint64_t)a + (uint64_t)b;
    update_flags_add32(cpu, a, b, result);
    return (uint32_t)result;
}

static uint32_t alu_sub32(cpu8086_state_t *cpu, uint32_t a, uint32_t b)
{
    uint64_t result = (uint64_t)a - (uint64_t)b;
    update_flags_sub32(cpu, a, b, result);
    return (uint32_t)result;
}

static uint32_t alu_or32(cpu8086_state_t *cpu, uint32_t a, uint32_t b)
{
    uint32_t result = a | b;
    update_flags_logic32(cpu, result);
    return result;
}

static uint32_t alu_and32(cpu8086_state_t *cpu, uint32_t a, uint32_t b)
{
    uint32_t result = a & b;
    update_flags_logic32(cpu, result);
    return result;
}

static uint32_t alu_xor32(cpu8086_state_t *cpu, uint32_t a, uint32_t b)
{
    uint32_t result = a ^ b;
    update_flags_logic32(cpu, result);
    return result;
}

static void alu_cmp32(cpu8086_state_t *cpu, uint32_t a, uint32_t b)
{
    alu_sub32(cpu, a, b);
}

static uint32_t alu_adc32(cpu8086_state_t *cpu, uint32_t a, uint32_t b)
{
    uint32_t carry = get_flag(cpu, FLAG_CF) ? 1 : 0;
    uint64_t result = (uint64_t)a + (uint64_t)b + carry;
    update_flags_add32(cpu, a, b + carry, result);
    set_flag(cpu, FLAG_CF, result > 0xFFFFFFFFULL);
    set_flag(cpu, FLAG_AF, ((a ^ b ^ (uint32_t)result) & 0x10) != 0);
    set_flag(cpu, FLAG_OF, ((~(a ^ b)) & (a ^ (uint32_t)result) & 0x80000000U) != 0);
    return (uint32_t)result;
}

static uint32_t alu_sbb32(cpu8086_state_t *cpu, uint32_t a, uint32_t b)
{
    uint32_t borrow = get_flag(cpu, FLAG_CF) ? 1 : 0;
    uint64_t result = (uint64_t)a - (uint64_t)b - borrow;
    uint32_t r32 = (uint32_t)result;
    set_flag(cpu, FLAG_CF, (uint64_t)a < (uint64_t)b + borrow);
    set_flag(cpu, FLAG_ZF, r32 == 0);
    set_flag(cpu, FLAG_SF, (r32 & 0x80000000U) != 0);
    set_flag(cpu, FLAG_PF, parity8((uint8_t)r32));
    set_flag(cpu, FLAG_AF, ((a ^ b ^ r32) & 0x10) != 0);
    set_flag(cpu, FLAG_OF, (((a ^ b)) & (a ^ r32) & 0x80000000U) != 0);
    return r32;
}

/* ── Group 1 ALU dispatch (32-bit) ──────────────────────────────── */

static uint32_t group1_alu32(cpu8086_state_t *cpu, uint8_t op, uint32_t a, uint32_t b)
{
    switch (op) {
    case 0: return alu_add32(cpu, a, b);
    case 1: return alu_or32(cpu, a, b);
    case 2: return alu_adc32(cpu, a, b);
    case 3: return alu_sbb32(cpu, a, b);
    case 4: return alu_and32(cpu, a, b);
    case 5: return alu_sub32(cpu, a, b);
    case 6: return alu_xor32(cpu, a, b);
    case 7: alu_cmp32(cpu, a, b); return a;
    }
    return a;
}

/* ── INC / DEC (preserve CF) ─────────────────────────────────────── */

static uint8_t alu_inc8(cpu8086_state_t *cpu, uint8_t val)
{
    bool cf = get_flag(cpu, FLAG_CF);
    uint16_t result = (uint16_t)val + 1;
    update_flags_add8(cpu, val, 1, result);
    set_flag(cpu, FLAG_CF, cf); /* INC does not affect CF */
    return (uint8_t)result;
}

static uint16_t alu_inc16(cpu8086_state_t *cpu, uint16_t val)
{
    bool cf = get_flag(cpu, FLAG_CF);
    uint32_t result = (uint32_t)val + 1;
    update_flags_add16(cpu, val, 1, result);
    set_flag(cpu, FLAG_CF, cf);
    return (uint16_t)result;
}

static uint8_t alu_dec8(cpu8086_state_t *cpu, uint8_t val)
{
    bool cf = get_flag(cpu, FLAG_CF);
    uint16_t result = (uint16_t)val - 1;
    update_flags_sub8(cpu, val, 1, result);
    set_flag(cpu, FLAG_CF, cf); /* DEC does not affect CF */
    return (uint8_t)result;
}

static uint16_t alu_dec16(cpu8086_state_t *cpu, uint16_t val)
{
    bool cf = get_flag(cpu, FLAG_CF);
    uint32_t result = (uint32_t)val - 1;
    update_flags_sub16(cpu, val, 1, result);
    set_flag(cpu, FLAG_CF, cf);
    return (uint16_t)result;
}

/* ── Group 1 ALU dispatch (for opcodes 0x80-0x83) ────────────────── */

static uint8_t group1_alu8(cpu8086_state_t *cpu, uint8_t op, uint8_t a, uint8_t b)
{
    switch (op) {
    case 0: return alu_add8(cpu, a, b);
    case 1: return alu_or8(cpu, a, b);
    case 2: return alu_adc8(cpu, a, b);
    case 3: return alu_sbb8(cpu, a, b);
    case 4: return alu_and8(cpu, a, b);
    case 5: return alu_sub8(cpu, a, b);
    case 6: return alu_xor8(cpu, a, b);
    case 7: alu_cmp8(cpu, a, b); return a; /* CMP: no writeback */
    }
    return a;
}

static uint16_t group1_alu16(cpu8086_state_t *cpu, uint8_t op, uint16_t a, uint16_t b)
{
    switch (op) {
    case 0: return alu_add16(cpu, a, b);
    case 1: return alu_or16(cpu, a, b);
    case 2: return alu_adc16(cpu, a, b);
    case 3: return alu_sbb16(cpu, a, b);
    case 4: return alu_and16(cpu, a, b);
    case 5: return alu_sub16(cpu, a, b);
    case 6: return alu_xor16(cpu, a, b);
    case 7: alu_cmp16(cpu, a, b); return a;
    }
    return a;
}

/* ── Shift / rotate helpers (Group 2) ────────────────────────────── */

static uint8_t shift_rotate8(cpu8086_state_t *cpu, uint8_t op, uint8_t val, uint8_t count)
{
    count &= 0x1F; /* mask to 5 bits (186+ behaviour) */
    if (count == 0)
        return val;

    uint8_t result = val;
    uint8_t i;
    bool cf;

    switch (op) {
    case 0: /* ROL */
        for (i = 0; i < count; i++) {
            cf = (result & 0x80) != 0;
            result = (result << 1) | (cf ? 1 : 0);
        }
        set_flag(cpu, FLAG_CF, result & 1);
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ val) & 0x80) != 0);
        break;

    case 1: /* ROR */
        for (i = 0; i < count; i++) {
            cf = (result & 1) != 0;
            result = (result >> 1) | (cf ? 0x80 : 0);
        }
        set_flag(cpu, FLAG_CF, (result & 0x80) != 0);
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ (result << 1)) & 0x80) != 0);
        break;

    case 2: /* RCL */
        for (i = 0; i < count; i++) {
            cf = get_flag(cpu, FLAG_CF);
            set_flag(cpu, FLAG_CF, (result & 0x80) != 0);
            result = (result << 1) | (cf ? 1 : 0);
        }
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ val) & 0x80) != 0);
        break;

    case 3: /* RCR */
        for (i = 0; i < count; i++) {
            cf = get_flag(cpu, FLAG_CF);
            set_flag(cpu, FLAG_CF, (result & 1) != 0);
            result = (result >> 1) | (cf ? 0x80 : 0);
        }
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ (result << 1)) & 0x80) != 0);
        break;

    case 4: /* SHL / SAL */ {
        uint16_t tmp = (uint16_t)result;
        for (i = 0; i < count; i++) {
            set_flag(cpu, FLAG_CF, (tmp & 0x80) != 0);
            tmp <<= 1;
        }
        result = (uint8_t)tmp;
        update_flags_logic8(cpu, result);
        set_flag(cpu, FLAG_CF, (count <= 8) ? ((val >> (8 - count)) & 1) : 0);
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ val) & 0x80) != 0);
        break;
    }

    case 5: /* SHR */ {
        uint16_t tmp = (uint16_t)result;
        if (count == 1)
            set_flag(cpu, FLAG_OF, (tmp & 0x80) != 0);
        for (i = 0; i < count; i++) {
            set_flag(cpu, FLAG_CF, (tmp & 1) != 0);
            tmp >>= 1;
        }
        result = (uint8_t)tmp;
        update_flags_logic8(cpu, result);
        set_flag(cpu, FLAG_CF, (count <= 8) ? ((val >> (count - 1)) & 1) : 0);
        break;
    }

    case 7: /* SAR */ {
        int8_t stmp = (int8_t)result;
        if (count == 1)
            set_flag(cpu, FLAG_OF, false); /* OF always cleared for SAR with count=1 */
        for (i = 0; i < count; i++) {
            set_flag(cpu, FLAG_CF, (stmp & 1) != 0);
            stmp >>= 1; /* arithmetic shift preserves sign */
        }
        result = (uint8_t)stmp;
        update_flags_logic8(cpu, result);
        set_flag(cpu, FLAG_CF, ((int8_t)val >> (count - 1)) & 1);
        break;
    }

    default: /* op 6: undefined on 8086, treat as SHL */
        break;
    }

    return result;
}

static uint16_t shift_rotate16(cpu8086_state_t *cpu, uint8_t op, uint16_t val, uint8_t count)
{
    count &= 0x1F;
    if (count == 0)
        return val;

    uint16_t result = val;
    uint8_t i;
    bool cf;

    switch (op) {
    case 0: /* ROL */
        for (i = 0; i < count; i++) {
            cf = (result & 0x8000) != 0;
            result = (result << 1) | (cf ? 1 : 0);
        }
        set_flag(cpu, FLAG_CF, result & 1);
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ val) & 0x8000) != 0);
        break;

    case 1: /* ROR */
        for (i = 0; i < count; i++) {
            cf = (result & 1) != 0;
            result = (result >> 1) | (cf ? 0x8000 : 0);
        }
        set_flag(cpu, FLAG_CF, (result & 0x8000) != 0);
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ (result << 1)) & 0x8000) != 0);
        break;

    case 2: /* RCL */
        for (i = 0; i < count; i++) {
            cf = get_flag(cpu, FLAG_CF);
            set_flag(cpu, FLAG_CF, (result & 0x8000) != 0);
            result = (result << 1) | (cf ? 1 : 0);
        }
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ val) & 0x8000) != 0);
        break;

    case 3: /* RCR */
        for (i = 0; i < count; i++) {
            cf = get_flag(cpu, FLAG_CF);
            set_flag(cpu, FLAG_CF, (result & 1) != 0);
            result = (result >> 1) | (cf ? 0x8000 : 0);
        }
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ (result << 1)) & 0x8000) != 0);
        break;

    case 4: /* SHL / SAL */ {
        uint32_t tmp = (uint32_t)result;
        for (i = 0; i < count; i++) {
            set_flag(cpu, FLAG_CF, (tmp & 0x8000) != 0);
            tmp <<= 1;
        }
        result = (uint16_t)tmp;
        update_flags_logic16(cpu, result);
        set_flag(cpu, FLAG_CF, (count <= 16) ? ((val >> (16 - count)) & 1) : 0);
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ val) & 0x8000) != 0);
        break;
    }

    case 5: /* SHR */ {
        uint32_t tmp = (uint32_t)result;
        if (count == 1)
            set_flag(cpu, FLAG_OF, (tmp & 0x8000) != 0);
        for (i = 0; i < count; i++) {
            set_flag(cpu, FLAG_CF, (tmp & 1) != 0);
            tmp >>= 1;
        }
        result = (uint16_t)tmp;
        update_flags_logic16(cpu, result);
        set_flag(cpu, FLAG_CF, (count <= 16) ? ((val >> (count - 1)) & 1) : 0);
        break;
    }

    case 7: /* SAR */ {
        int16_t stmp = (int16_t)result;
        if (count == 1)
            set_flag(cpu, FLAG_OF, false);
        for (i = 0; i < count; i++) {
            set_flag(cpu, FLAG_CF, (stmp & 1) != 0);
            stmp >>= 1;
        }
        result = (uint16_t)stmp;
        update_flags_logic16(cpu, result);
        set_flag(cpu, FLAG_CF, ((int16_t)val >> (count - 1)) & 1);
        break;
    }

    default:
        break;
    }

    return result;
}

/* ── Jcc condition code evaluation ───────────────────────────────── */

static bool eval_condition(cpu8086_state_t *cpu, uint8_t cond)
{
    switch (cond & 0x0F) {
    case 0x0: return get_flag(cpu, FLAG_OF);                          /* JO */
    case 0x1: return !get_flag(cpu, FLAG_OF);                         /* JNO */
    case 0x2: return get_flag(cpu, FLAG_CF);                          /* JB/JNAE/JC */
    case 0x3: return !get_flag(cpu, FLAG_CF);                         /* JNB/JAE/JNC */
    case 0x4: return get_flag(cpu, FLAG_ZF);                          /* JZ/JE */
    case 0x5: return !get_flag(cpu, FLAG_ZF);                         /* JNZ/JNE */
    case 0x6: return get_flag(cpu, FLAG_CF) || get_flag(cpu, FLAG_ZF); /* JBE/JNA */
    case 0x7: return !get_flag(cpu, FLAG_CF) && !get_flag(cpu, FLAG_ZF); /* JA/JNBE */
    case 0x8: return get_flag(cpu, FLAG_SF);                          /* JS */
    case 0x9: return !get_flag(cpu, FLAG_SF);                         /* JNS */
    case 0xA: return get_flag(cpu, FLAG_PF);                          /* JP/JPE */
    case 0xB: return !get_flag(cpu, FLAG_PF);                         /* JNP/JPO */
    case 0xC: return get_flag(cpu, FLAG_SF) != get_flag(cpu, FLAG_OF); /* JL/JNGE */
    case 0xD: return get_flag(cpu, FLAG_SF) == get_flag(cpu, FLAG_OF); /* JGE/JNL */
    case 0xE: return get_flag(cpu, FLAG_ZF) ||                        /* JLE/JNG */
                     (get_flag(cpu, FLAG_SF) != get_flag(cpu, FLAG_OF));
    case 0xF: return !get_flag(cpu, FLAG_ZF) &&                       /* JG/JNLE */
                     (get_flag(cpu, FLAG_SF) == get_flag(cpu, FLAG_OF));
    }
    return false;
}

/* ── String operation segment:offset helpers ─────────────────────── */

static uint16_t string_src_seg(cpu8086_state_t *cpu)
{
    if (cpu->seg_override >= 0)
        return *seg_ptr(cpu, (uint8_t)cpu->seg_override);
    return cpu->ds;
}

/* ── Main execution loop ─────────────────────────────────────────── */

int cpu8086_run(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint8_t opcode;
    uint8_t modrm_byte;
    modrm_t m;

    /* JIT engine (optional — NULL if not initialized) */
    jit_state_t *jit = (jit_state_t *)vm->jit;

    while (cpu->running && !cpu->halted) {

        /* ── Hybrid dispatcher: JIT cache → compile if hot → interpret ── */
        if (jit && !cpu->protected_mode) {
            /* Check hit counter for current IP */
            uint16_t ip = cpu->ip;
            jit->hit_count[ip]++;

            if (jit->hit_count[ip] >= JIT_HOT_THRESHOLD) {
                /* Hot path — look up or compile block */
                jit_block_t *block = jit_get_block(jit, cpu->cs, ip);
                if (block) {
                    if (!block->compiled) {
                        /* First time: decode + compile */
                        jit_decode_block(vm, block);
                        if (block->ir_count > 0) {
                            jit_compile_block(jit, block);
                        }
                    }
                    if (block->compiled) {
                        /* Execute native code */
                        jit_exec_block(vm, block);
                        jit->jit_executed++;
                        continue;  /* CS:IP updated by JIT, loop back */
                    }
                }
            }
            jit->interpreted++;
        }

        /* ── Interpreter: fetch-decode-execute ── */

        /* Reset prefix state */
        cpu->seg_override = -1;
        cpu->rep_active   = false;
        cpu->rep_type     = 0;
        cpu->prefix_66    = false;
        cpu->prefix_67    = false;

        /* Handle prefixes */
    fetch_prefix:
        opcode = cpu_fetch8(cpu);

        switch (opcode) {
        case 0x26: /* ES: */
            cpu->seg_override = 0;
            goto fetch_prefix;
        case 0x2E: /* CS: */
            cpu->seg_override = 1;
            goto fetch_prefix;
        case 0x36: /* SS: */
            cpu->seg_override = 2;
            goto fetch_prefix;
        case 0x3E: /* DS: */
            cpu->seg_override = 3;
            goto fetch_prefix;
        case 0x64: /* FS: */
            cpu->seg_override = 4;
            goto fetch_prefix;
        case 0x65: /* GS: */
            cpu->seg_override = 5;
            goto fetch_prefix;
        case 0x66: /* Operand size override */
            cpu->prefix_66 = true;
            goto fetch_prefix;
        case 0x67: /* Address size override */
            cpu->prefix_67 = true;
            goto fetch_prefix;
        case 0xF0: /* LOCK (ignore) */
            goto fetch_prefix;
        case 0xF2: /* REPNZ / REPNE */
            cpu->rep_active = true;
            cpu->rep_type = 2;
            goto fetch_prefix;
        case 0xF3: /* REP / REPZ / REPE */
            cpu->rep_active = true;
            cpu->rep_type = 1;
            goto fetch_prefix;
        default:
            break;
        }

        /* Compute effective operand/address sizes (XOR with prefix) */
        bool op32  = cpu->op_size_32  ^ cpu->prefix_66;
        bool adr32 = cpu->addr_size_32 ^ cpu->prefix_67;
        (void)adr32; /* used later as more opcodes get 32-bit addressing */

        /* ── Dispatch opcode ─────────────────────────────────────── */

        switch (opcode) {

        /* ════════════════════════════════════════════════════════════
         *  ADD  (0x00 - 0x05)
         * ════════════════════════════════════════════════════════════ */
        case 0x00: { /* ADD r/m8, r8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_add8(cpu, val, reg));
            break;
        }
        case 0x01: { /* ADD r/m16, r16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            uint16_t reg = *reg16_ptr(cpu, m.reg_field);
            modrm_write16(cpu, &m, alu_add16(cpu, val, reg));
            break;
        }
        case 0x02: { /* ADD r8, r/m8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_add8(cpu, *dst, val);
            break;
        }
        case 0x03: { /* ADD r16, r/m16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t *dst = reg16_ptr(cpu, m.reg_field);
            uint16_t val = modrm_read16(cpu, &m);
            *dst = alu_add16(cpu, *dst, val);
            break;
        }
        case 0x04: { /* ADD AL, imm8 */
            uint8_t imm = cpu_fetch8(cpu);
            cpu->al = alu_add8(cpu, cpu->al, imm);
            break;
        }
        case 0x05: { /* ADD AX, imm16 */
            uint16_t imm = cpu_fetch16(cpu);
            cpu->ax = alu_add16(cpu, cpu->ax, imm);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  PUSH / POP segment registers
         * ════════════════════════════════════════════════════════════ */
        case 0x06: /* PUSH ES */
            cpu_push16(cpu, cpu->es);
            break;
        case 0x07: /* POP ES */
            cpu->es = cpu_pop16(cpu);
            break;

        /* ════════════════════════════════════════════════════════════
         *  OR  (0x08 - 0x0D)
         * ════════════════════════════════════════════════════════════ */
        case 0x08: { /* OR r/m8, r8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_or8(cpu, val, reg));
            break;
        }
        case 0x09: { /* OR r/m16, r16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            uint16_t reg = *reg16_ptr(cpu, m.reg_field);
            modrm_write16(cpu, &m, alu_or16(cpu, val, reg));
            break;
        }
        case 0x0A: { /* OR r8, r/m8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_or8(cpu, *dst, val);
            break;
        }
        case 0x0B: { /* OR r16, r/m16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t *dst = reg16_ptr(cpu, m.reg_field);
            uint16_t val = modrm_read16(cpu, &m);
            *dst = alu_or16(cpu, *dst, val);
            break;
        }
        case 0x0C: { /* OR AL, imm8 */
            uint8_t imm = cpu_fetch8(cpu);
            cpu->al = alu_or8(cpu, cpu->al, imm);
            break;
        }
        case 0x0D: { /* OR AX, imm16 */
            uint16_t imm = cpu_fetch16(cpu);
            cpu->ax = alu_or16(cpu, cpu->ax, imm);
            break;
        }

        case 0x0E: /* PUSH CS */
            cpu_push16(cpu, cpu->cs);
            break;

        /* ════════════════════════════════════════════════════════════
         *  Two-byte escape (0x0F)
         * ════════════════════════════════════════════════════════════ */
        case 0x0F: {
            uint8_t op2 = cpu_fetch8(cpu);
            switch (op2) {
            case 0x80: { /* JO near (386+) */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (get_flag(cpu, FLAG_OF))
                    cpu->ip += rel;
                break;
            }
            case 0x81: { /* JNO near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (!get_flag(cpu, FLAG_OF))
                    cpu->ip += rel;
                break;
            }
            case 0x82: { /* JB/JC near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (get_flag(cpu, FLAG_CF))
                    cpu->ip += rel;
                break;
            }
            case 0x83: { /* JNB/JAE/JNC near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (!get_flag(cpu, FLAG_CF))
                    cpu->ip += rel;
                break;
            }
            case 0x84: { /* JZ/JE near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (get_flag(cpu, FLAG_ZF))
                    cpu->ip += rel;
                break;
            }
            case 0x85: { /* JNZ/JNE near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (!get_flag(cpu, FLAG_ZF))
                    cpu->ip += rel;
                break;
            }
            case 0x86: { /* JBE/JNA near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (get_flag(cpu, FLAG_CF) || get_flag(cpu, FLAG_ZF))
                    cpu->ip += rel;
                break;
            }
            case 0x87: { /* JA/JNBE near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (!get_flag(cpu, FLAG_CF) && !get_flag(cpu, FLAG_ZF))
                    cpu->ip += rel;
                break;
            }
            case 0x88: { /* JS near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (get_flag(cpu, FLAG_SF))
                    cpu->ip += rel;
                break;
            }
            case 0x89: { /* JNS near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (!get_flag(cpu, FLAG_SF))
                    cpu->ip += rel;
                break;
            }
            case 0x8A: { /* JP/JPE near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (get_flag(cpu, FLAG_PF))
                    cpu->ip += rel;
                break;
            }
            case 0x8B: { /* JNP/JPO near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (!get_flag(cpu, FLAG_PF))
                    cpu->ip += rel;
                break;
            }
            case 0x8C: { /* JL/JNGE near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (get_flag(cpu, FLAG_SF) != get_flag(cpu, FLAG_OF))
                    cpu->ip += rel;
                break;
            }
            case 0x8D: { /* JGE/JNL near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (get_flag(cpu, FLAG_SF) == get_flag(cpu, FLAG_OF))
                    cpu->ip += rel;
                break;
            }
            case 0x8E: { /* JLE/JNG near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (get_flag(cpu, FLAG_ZF) ||
                    (get_flag(cpu, FLAG_SF) != get_flag(cpu, FLAG_OF)))
                    cpu->ip += rel;
                break;
            }
            case 0x8F: { /* JG/JNLE near */
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                if (!get_flag(cpu, FLAG_ZF) &&
                    (get_flag(cpu, FLAG_SF) == get_flag(cpu, FLAG_OF)))
                    cpu->ip += rel;
                break;
            }
            /* ── 0F 00: Group 6 (SLDT/STR/LLDT/LTR/VERR/VERW) ─── */
            case 0x00: {
                uint8_t g6_modrm = cpu_fetch8(cpu);
                modrm_t g6m = decode_modrm(cpu, g6_modrm);
                switch (g6m.reg_field) {
                case 0: /* SLDT */
                    modrm_write16(cpu, &g6m, 0); break;
                case 1: /* STR */
                    modrm_write16(cpu, &g6m, 0); break;
                case 2: /* LLDT */
                    (void)modrm_read16(cpu, &g6m); break;
                case 3: /* LTR */
                    (void)modrm_read16(cpu, &g6m); break;
                case 4: /* VERR */
                    set_flag(cpu, FLAG_ZF, true); break;
                case 5: /* VERW */
                    set_flag(cpu, FLAG_ZF, true); break;
                default: break;
                }
                break;
            }
            /* ── 0F 01: Group 7 (LGDT/SGDT/LIDT/SIDT/LMSW/SMSW) ─── */
            case 0x01: {
                uint8_t g7_modrm = cpu_fetch8(cpu);
                modrm_t g7m = decode_modrm(cpu, g7_modrm);
                switch (g7m.reg_field) {
                case 0: { /* SGDT: store GDTR to m */
                    dos_mem_write16(vm, g7m.addr, cpu->gdtr.limit);
                    dos_mem_write32(vm, g7m.addr + 2, cpu->gdtr.base);
                    break;
                }
                case 1: { /* SIDT: store IDTR to m */
                    dos_mem_write16(vm, g7m.addr, cpu->idtr.limit);
                    dos_mem_write32(vm, g7m.addr + 2, cpu->idtr.base);
                    break;
                }
                case 2: { /* LGDT: load GDTR from m */
                    cpu->gdtr.limit = dos_mem_read16(vm, g7m.addr);
                    if (op32) {
                        cpu->gdtr.base = dos_mem_read32(vm, g7m.addr + 2);
                    } else {
                        /* 16-bit: 24-bit base (3 bytes), high byte forced to 0 */
                        cpu->gdtr.base = dos_mem_read16(vm, g7m.addr + 2) |
                                         ((uint32_t)dos_mem_read8(vm, g7m.addr + 4) << 16);
                    }
                    serial_puts("[CPU] LGDT base=");
                    serial_puthex(cpu->gdtr.base, 8);
                    serial_puts(" limit=");
                    serial_puthex(cpu->gdtr.limit, 4);
                    serial_puts("\n");
                    break;
                }
                case 3: { /* LIDT: load IDTR from m */
                    cpu->idtr.limit = dos_mem_read16(vm, g7m.addr);
                    if (op32) {
                        cpu->idtr.base = dos_mem_read32(vm, g7m.addr + 2);
                    } else {
                        cpu->idtr.base = dos_mem_read16(vm, g7m.addr + 2) |
                                         ((uint32_t)dos_mem_read8(vm, g7m.addr + 4) << 16);
                    }
                    serial_puts("[CPU] LIDT base=");
                    serial_puthex(cpu->idtr.base, 8);
                    serial_puts(" limit=");
                    serial_puthex(cpu->idtr.limit, 4);
                    serial_puts(op32 ? " [32]\n" : " [16]\n");
                    serial_puts("  PM=");
                    serial_putdec(cpu->protected_mode);
                    serial_puts(" op_size_32=");
                    serial_putdec(cpu->op_size_32);
                    serial_puts(" #");
                    serial_putdec(cpu->insn_count);
                    serial_puts("\n");
                    break;
                }
                case 4: { /* SMSW: store machine status word */
                    uint16_t msw = (uint16_t)cpu->cr0;
                    modrm_write16(cpu, &g7m, msw);
                    break;
                }
                case 6: { /* LMSW: load machine status word */
                    uint16_t msw = modrm_read16(cpu, &g7m);
                    cpu->cr0 = (cpu->cr0 & 0xFFFF0000U) | msw;
                    if ((msw & 1) && !cpu->protected_mode) {
                        cpu->protected_mode = true;
                        serial_puts("[DOS] LMSW: PE=1 CS=");
                        serial_puthex(cpu->cs, 4);
                        serial_puts(" EIP=");
                        serial_puthex(cpu->eip, 8);
                        serial_puts(" #");
                        serial_putdec(cpu->insn_count);
                        serial_puts("\n");
                        /* Dump next 16 bytes to see the JMP FAR */
                        {
                            uint32_t next = dos_linear(cpu->cs, cpu->ip);
                            serial_puts("[DOS] Next bytes: ");
                            for (int ii = 0; ii < 16; ii++) {
                                serial_puthex(dos_mem_read8(vm, next + ii), 2);
                                serial_puts(" ");
                            }
                            serial_puts("\n");
                        }
                    }
                    break;
                }
                default:
                    serial_puts("[386] unknown 0F 01 /");
                    serial_puthex(g7m.reg_field, 1);
                    serial_puts("\n");
                    break;
                }
                break;
            }

            /* ── 0F 20: MOV r32, CRn ─────────────────────────────── */
            case 0x20: {
                uint8_t cr_modrm = cpu_fetch8(cpu);
                uint8_t cr_num = (cr_modrm >> 3) & 7;
                uint8_t cr_reg = cr_modrm & 7;
                uint32_t cr_val = 0;
                switch (cr_num) {
                case 0: cr_val = cpu->cr0; break;
                case 2: cr_val = cpu->cr2; break;
                case 3: cr_val = cpu->cr3; break;
                }
                *reg32_ptr(cpu, cr_reg) = cr_val;
                break;
            }

            /* ── 0F 22: MOV CRn, r32 ─────────────────────────────── */
            case 0x22: {
                uint8_t cr_modrm = cpu_fetch8(cpu);
                uint8_t cr_num = (cr_modrm >> 3) & 7;
                uint8_t cr_reg = cr_modrm & 7;
                uint32_t cr_val = *reg32_ptr(cpu, cr_reg);
                switch (cr_num) {
                case 0:
                    cpu->cr0 = cr_val;
                    if ((cr_val & 1) && !cpu->protected_mode) {
                        cpu->protected_mode = true;
                        serial_puts("[DOS] MOV CR0: PE bit set — switching to native execution\n");
                        serial_puts("[DOS] GDT base=");
                        serial_puthex(cpu->gdtr.base, 8);
                        serial_puts(" IDT base=");
                        serial_puthex(cpu->idtr.base, 8);
                        serial_puts("\n");
                        /* Transfer to native 32-bit execution.
                         * The next instruction after MOV CR0 is typically a far JMP
                         * to a 32-bit code segment. We let the interpreter execute
                         * that JMP, then transfer on the next fetch in PM. */
                        extern void dos_transfer_to_native(dos_vm_t *vm);
                        dos_transfer_to_native(vm);
                        /* If transfer succeeds, does not return here.
                         * If it fails (e.g., can't set up GDT), continues interpreting. */
                    }
                    break;
                case 2: cpu->cr2 = cr_val; break;
                case 3:
                    cpu->cr3 = cr_val;
                    serial_puts("[CPU] MOV CR3, ");
                    serial_puthex(cr_val, 8);
                    serial_puts("\n");
                    break;
                }
                break;
            }

            /* ── 0F 90-9F: SETcc r/m8 ────────────────────────────── */
            case 0x90: case 0x91: case 0x92: case 0x93:
            case 0x94: case 0x95: case 0x96: case 0x97:
            case 0x98: case 0x99: case 0x9A: case 0x9B:
            case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
                modrm_byte = cpu_fetch8(cpu);
                m = decode_modrm(cpu, modrm_byte);
                uint8_t set_val = eval_condition(cpu, op2 - 0x90) ? 1 : 0;
                modrm_write8(cpu, &m, set_val);
                break;
            }

            /* ── 0F A4: SHLD r/m16/32, r16/32, imm8 ─────────────── */
            case 0xA4: {
                modrm_byte = cpu_fetch8(cpu);
                m = decode_modrm(cpu, modrm_byte);
                uint8_t shld_cnt = cpu_fetch8(cpu);
                shld_cnt &= 0x1F;
                if (shld_cnt != 0) {
                    if (op32) {
                        uint32_t dst_val = modrm_read32(cpu, &m);
                        uint32_t src_val = *reg32_ptr(cpu, m.reg_field);
                        uint64_t combined = ((uint64_t)dst_val << 32) | src_val;
                        uint32_t res = (uint32_t)(combined << shld_cnt >> 32);
                        modrm_write32(cpu, &m, res);
                        update_flags_logic32(cpu, res);
                        set_flag(cpu, FLAG_CF, (dst_val >> (32 - shld_cnt)) & 1);
                    } else {
                        uint16_t dst_val = modrm_read16(cpu, &m);
                        uint16_t src_val = *reg16_ptr(cpu, m.reg_field);
                        uint32_t combined = ((uint32_t)dst_val << 16) | src_val;
                        uint16_t res = (uint16_t)(combined << shld_cnt >> 16);
                        modrm_write16(cpu, &m, res);
                        update_flags_logic16(cpu, res);
                        set_flag(cpu, FLAG_CF, (dst_val >> (16 - shld_cnt)) & 1);
                    }
                }
                break;
            }

            /* ── 0F A5: SHLD r/m16/32, r16/32, CL ───────────────── */
            case 0xA5: {
                modrm_byte = cpu_fetch8(cpu);
                m = decode_modrm(cpu, modrm_byte);
                uint8_t shld_cnt = cpu->cl & 0x1F;
                if (shld_cnt != 0) {
                    if (op32) {
                        uint32_t dst_val = modrm_read32(cpu, &m);
                        uint32_t src_val = *reg32_ptr(cpu, m.reg_field);
                        uint64_t combined = ((uint64_t)dst_val << 32) | src_val;
                        uint32_t res = (uint32_t)(combined << shld_cnt >> 32);
                        modrm_write32(cpu, &m, res);
                        update_flags_logic32(cpu, res);
                        set_flag(cpu, FLAG_CF, (dst_val >> (32 - shld_cnt)) & 1);
                    } else {
                        uint16_t dst_val = modrm_read16(cpu, &m);
                        uint16_t src_val = *reg16_ptr(cpu, m.reg_field);
                        uint32_t combined = ((uint32_t)dst_val << 16) | src_val;
                        uint16_t res = (uint16_t)(combined << shld_cnt >> 16);
                        modrm_write16(cpu, &m, res);
                        update_flags_logic16(cpu, res);
                        set_flag(cpu, FLAG_CF, (dst_val >> (16 - shld_cnt)) & 1);
                    }
                }
                break;
            }

            /* ── 0F AC: SHRD r/m16/32, r16/32, imm8 ─────────────── */
            case 0xAC: {
                modrm_byte = cpu_fetch8(cpu);
                m = decode_modrm(cpu, modrm_byte);
                uint8_t shrd_cnt = cpu_fetch8(cpu);
                shrd_cnt &= 0x1F;
                if (shrd_cnt != 0) {
                    if (op32) {
                        uint32_t dst_val = modrm_read32(cpu, &m);
                        uint32_t src_val = *reg32_ptr(cpu, m.reg_field);
                        uint64_t combined = ((uint64_t)src_val << 32) | dst_val;
                        uint32_t res = (uint32_t)(combined >> shrd_cnt);
                        modrm_write32(cpu, &m, res);
                        update_flags_logic32(cpu, res);
                        set_flag(cpu, FLAG_CF, (dst_val >> (shrd_cnt - 1)) & 1);
                    } else {
                        uint16_t dst_val = modrm_read16(cpu, &m);
                        uint16_t src_val = *reg16_ptr(cpu, m.reg_field);
                        uint32_t combined = ((uint32_t)src_val << 16) | dst_val;
                        uint16_t res = (uint16_t)(combined >> shrd_cnt);
                        modrm_write16(cpu, &m, res);
                        update_flags_logic16(cpu, res);
                        set_flag(cpu, FLAG_CF, (dst_val >> (shrd_cnt - 1)) & 1);
                    }
                }
                break;
            }

            /* ── 0F AD: SHRD r/m16/32, r16/32, CL ───────────────── */
            case 0xAD: {
                modrm_byte = cpu_fetch8(cpu);
                m = decode_modrm(cpu, modrm_byte);
                uint8_t shrd_cnt = cpu->cl & 0x1F;
                if (shrd_cnt != 0) {
                    if (op32) {
                        uint32_t dst_val = modrm_read32(cpu, &m);
                        uint32_t src_val = *reg32_ptr(cpu, m.reg_field);
                        uint64_t combined = ((uint64_t)src_val << 32) | dst_val;
                        uint32_t res = (uint32_t)(combined >> shrd_cnt);
                        modrm_write32(cpu, &m, res);
                        update_flags_logic32(cpu, res);
                        set_flag(cpu, FLAG_CF, (dst_val >> (shrd_cnt - 1)) & 1);
                    } else {
                        uint16_t dst_val = modrm_read16(cpu, &m);
                        uint16_t src_val = *reg16_ptr(cpu, m.reg_field);
                        uint32_t combined = ((uint32_t)src_val << 16) | dst_val;
                        uint16_t res = (uint16_t)(combined >> shrd_cnt);
                        modrm_write16(cpu, &m, res);
                        update_flags_logic16(cpu, res);
                        set_flag(cpu, FLAG_CF, (dst_val >> (shrd_cnt - 1)) & 1);
                    }
                }
                break;
            }

            /* ── 0F AF: IMUL r16/32, r/m16/32 ────────────────────── */
            case 0xAF: {
                modrm_byte = cpu_fetch8(cpu);
                m = decode_modrm(cpu, modrm_byte);
                if (op32) {
                    uint32_t *dst = reg32_ptr(cpu, m.reg_field);
                    int32_t a = (int32_t)*dst;
                    int32_t b = (int32_t)modrm_read32(cpu, &m);
                    int64_t result = (int64_t)a * (int64_t)b;
                    *dst = (uint32_t)result;
                    bool overflow = (result != (int64_t)(int32_t)(uint32_t)result);
                    set_flag(cpu, FLAG_CF, overflow);
                    set_flag(cpu, FLAG_OF, overflow);
                } else {
                    uint16_t *dst = reg16_ptr(cpu, m.reg_field);
                    int16_t a = (int16_t)*dst;
                    int16_t b = (int16_t)modrm_read16(cpu, &m);
                    int32_t result = (int32_t)a * (int32_t)b;
                    *dst = (uint16_t)result;
                    bool overflow = (result < -32768 || result > 32767);
                    set_flag(cpu, FLAG_CF, overflow);
                    set_flag(cpu, FLAG_OF, overflow);
                }
                break;
            }

            /* ── 0F B6: MOVZX r16/32, r/m8 ──────────────────────── */
            case 0xB6: {
                modrm_byte = cpu_fetch8(cpu);
                m = decode_modrm(cpu, modrm_byte);
                uint8_t src = modrm_read8(cpu, &m);
                if (op32) {
                    *reg32_ptr(cpu, m.reg_field) = (uint32_t)src;
                } else {
                    *reg16_ptr(cpu, m.reg_field) = (uint16_t)src;
                }
                break;
            }

            /* ── 0F B7: MOVZX r32, r/m16 ────────────────────────── */
            case 0xB7: {
                modrm_byte = cpu_fetch8(cpu);
                m = decode_modrm(cpu, modrm_byte);
                uint16_t src = modrm_read16(cpu, &m);
                *reg32_ptr(cpu, m.reg_field) = (uint32_t)src;
                break;
            }

            /* ── 0F BC: BSF r16/32, r/m16/32 ────────────────────── */
            case 0xBC: {
                modrm_byte = cpu_fetch8(cpu);
                m = decode_modrm(cpu, modrm_byte);
                if (op32) {
                    uint32_t src = modrm_read32(cpu, &m);
                    if (src == 0) {
                        set_flag(cpu, FLAG_ZF, true);
                    } else {
                        set_flag(cpu, FLAG_ZF, false);
                        uint32_t idx = 0;
                        while (!(src & (1U << idx))) idx++;
                        *reg32_ptr(cpu, m.reg_field) = idx;
                    }
                } else {
                    uint16_t src = modrm_read16(cpu, &m);
                    if (src == 0) {
                        set_flag(cpu, FLAG_ZF, true);
                    } else {
                        set_flag(cpu, FLAG_ZF, false);
                        uint16_t idx = 0;
                        while (!(src & (1U << idx))) idx++;
                        *reg16_ptr(cpu, m.reg_field) = idx;
                    }
                }
                break;
            }

            /* ── 0F BD: BSR r16/32, r/m16/32 ────────────────────── */
            case 0xBD: {
                modrm_byte = cpu_fetch8(cpu);
                m = decode_modrm(cpu, modrm_byte);
                if (op32) {
                    uint32_t src = modrm_read32(cpu, &m);
                    if (src == 0) {
                        set_flag(cpu, FLAG_ZF, true);
                    } else {
                        set_flag(cpu, FLAG_ZF, false);
                        uint32_t idx = 31;
                        while (!(src & (1U << idx))) idx--;
                        *reg32_ptr(cpu, m.reg_field) = idx;
                    }
                } else {
                    uint16_t src = modrm_read16(cpu, &m);
                    if (src == 0) {
                        set_flag(cpu, FLAG_ZF, true);
                    } else {
                        set_flag(cpu, FLAG_ZF, false);
                        uint16_t idx = 15;
                        while (!(src & (1U << idx))) idx--;
                        *reg16_ptr(cpu, m.reg_field) = idx;
                    }
                }
                break;
            }

            /* ── 0F BE: MOVSX r16/32, r/m8 ──────────────────────── */
            case 0xBE: {
                modrm_byte = cpu_fetch8(cpu);
                m = decode_modrm(cpu, modrm_byte);
                int8_t src = (int8_t)modrm_read8(cpu, &m);
                if (op32) {
                    *reg32_ptr(cpu, m.reg_field) = (uint32_t)(int32_t)src;
                } else {
                    *reg16_ptr(cpu, m.reg_field) = (uint16_t)(int16_t)src;
                }
                break;
            }

            /* ── 0F BF: MOVSX r32, r/m16 ────────────────────────── */
            case 0xBF: {
                modrm_byte = cpu_fetch8(cpu);
                m = decode_modrm(cpu, modrm_byte);
                int16_t src = (int16_t)modrm_read16(cpu, &m);
                *reg32_ptr(cpu, m.reg_field) = (uint32_t)(int32_t)src;
                break;
            }

            default:
                /* Unknown 0F opcode — most have a ModRM byte.
                 * Consume it silently to stay aligned. */
                if (op2 != 0x0B && op2 != 0x06 && op2 != 0x08 &&
                    op2 != 0x09 && op2 != 0x77) {
                    /* These are single-byte 0F opcodes (UD2, CLTS, INVD, WBINVD, EMMS).
                     * Everything else has ModRM. */
                    modrm_byte = cpu_fetch8(cpu);
                    (void)decode_modrm(cpu, modrm_byte);
                }
                break;
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  ADC  (0x10 - 0x15)
         * ════════════════════════════════════════════════════════════ */
        case 0x10: { /* ADC r/m8, r8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_adc8(cpu, val, reg));
            break;
        }
        case 0x11: { /* ADC r/m16, r16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            uint16_t reg = *reg16_ptr(cpu, m.reg_field);
            modrm_write16(cpu, &m, alu_adc16(cpu, val, reg));
            break;
        }
        case 0x12: { /* ADC r8, r/m8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_adc8(cpu, *dst, val);
            break;
        }
        case 0x13: { /* ADC r16, r/m16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t *dst = reg16_ptr(cpu, m.reg_field);
            uint16_t val = modrm_read16(cpu, &m);
            *dst = alu_adc16(cpu, *dst, val);
            break;
        }
        case 0x14: { /* ADC AL, imm8 */
            uint8_t imm = cpu_fetch8(cpu);
            cpu->al = alu_adc8(cpu, cpu->al, imm);
            break;
        }
        case 0x15: { /* ADC AX, imm16 */
            uint16_t imm = cpu_fetch16(cpu);
            cpu->ax = alu_adc16(cpu, cpu->ax, imm);
            break;
        }

        case 0x16: /* PUSH SS */
            cpu_push16(cpu, cpu->ss);
            break;
        case 0x17: /* POP SS */
            cpu->ss = cpu_pop16(cpu);
            break;

        /* ════════════════════════════════════════════════════════════
         *  SBB  (0x18 - 0x1D)
         * ════════════════════════════════════════════════════════════ */
        case 0x18: { /* SBB r/m8, r8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_sbb8(cpu, val, reg));
            break;
        }
        case 0x19: { /* SBB r/m16, r16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            uint16_t reg = *reg16_ptr(cpu, m.reg_field);
            modrm_write16(cpu, &m, alu_sbb16(cpu, val, reg));
            break;
        }
        case 0x1A: { /* SBB r8, r/m8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_sbb8(cpu, *dst, val);
            break;
        }
        case 0x1B: { /* SBB r16, r/m16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t *dst = reg16_ptr(cpu, m.reg_field);
            uint16_t val = modrm_read16(cpu, &m);
            *dst = alu_sbb16(cpu, *dst, val);
            break;
        }
        case 0x1C: { /* SBB AL, imm8 */
            uint8_t imm = cpu_fetch8(cpu);
            cpu->al = alu_sbb8(cpu, cpu->al, imm);
            break;
        }
        case 0x1D: { /* SBB AX, imm16 */
            uint16_t imm = cpu_fetch16(cpu);
            cpu->ax = alu_sbb16(cpu, cpu->ax, imm);
            break;
        }

        case 0x1E: /* PUSH DS */
            cpu_push16(cpu, cpu->ds);
            break;
        case 0x1F: /* POP DS */
            cpu->ds = cpu_pop16(cpu);
            break;

        /* ════════════════════════════════════════════════════════════
         *  AND  (0x20 - 0x25)
         * ════════════════════════════════════════════════════════════ */
        case 0x20: { /* AND r/m8, r8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_and8(cpu, val, reg));
            break;
        }
        case 0x21: { /* AND r/m16, r16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            uint16_t reg = *reg16_ptr(cpu, m.reg_field);
            modrm_write16(cpu, &m, alu_and16(cpu, val, reg));
            break;
        }
        case 0x22: { /* AND r8, r/m8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_and8(cpu, *dst, val);
            break;
        }
        case 0x23: { /* AND r16, r/m16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t *dst = reg16_ptr(cpu, m.reg_field);
            uint16_t val = modrm_read16(cpu, &m);
            *dst = alu_and16(cpu, *dst, val);
            break;
        }
        case 0x24: { /* AND AL, imm8 */
            uint8_t imm = cpu_fetch8(cpu);
            cpu->al = alu_and8(cpu, cpu->al, imm);
            break;
        }
        case 0x25: { /* AND AX, imm16 */
            uint16_t imm = cpu_fetch16(cpu);
            cpu->ax = alu_and16(cpu, cpu->ax, imm);
            break;
        }

        /* 0x26 = ES: handled as prefix above */

        /* ════════════════════════════════════════════════════════════
         *  DAA / DAS / AAA / AAS
         * ════════════════════════════════════════════════════════════ */
        case 0x27: { /* DAA */
            uint8_t old_al = cpu->al;
            bool old_cf = get_flag(cpu, FLAG_CF);
            set_flag(cpu, FLAG_CF, false);
            if ((cpu->al & 0x0F) > 9 || get_flag(cpu, FLAG_AF)) {
                cpu->al += 6;
                set_flag(cpu, FLAG_CF, old_cf || (cpu->al < old_al));
                set_flag(cpu, FLAG_AF, true);
            } else {
                set_flag(cpu, FLAG_AF, false);
            }
            if (old_al > 0x99 || old_cf) {
                cpu->al += 0x60;
                set_flag(cpu, FLAG_CF, true);
            }
            set_flag(cpu, FLAG_ZF, cpu->al == 0);
            set_flag(cpu, FLAG_SF, (cpu->al & 0x80) != 0);
            set_flag(cpu, FLAG_PF, parity8(cpu->al));
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  SUB  (0x28 - 0x2D)
         * ════════════════════════════════════════════════════════════ */
        case 0x28: { /* SUB r/m8, r8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_sub8(cpu, val, reg));
            break;
        }
        case 0x29: { /* SUB r/m16, r16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            uint16_t reg = *reg16_ptr(cpu, m.reg_field);
            modrm_write16(cpu, &m, alu_sub16(cpu, val, reg));
            break;
        }
        case 0x2A: { /* SUB r8, r/m8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_sub8(cpu, *dst, val);
            break;
        }
        case 0x2B: { /* SUB r16, r/m16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t *dst = reg16_ptr(cpu, m.reg_field);
            uint16_t val = modrm_read16(cpu, &m);
            *dst = alu_sub16(cpu, *dst, val);
            break;
        }
        case 0x2C: { /* SUB AL, imm8 */
            uint8_t imm = cpu_fetch8(cpu);
            cpu->al = alu_sub8(cpu, cpu->al, imm);
            break;
        }
        case 0x2D: { /* SUB AX, imm16 */
            uint16_t imm = cpu_fetch16(cpu);
            cpu->ax = alu_sub16(cpu, cpu->ax, imm);
            break;
        }

        /* 0x2E = CS: prefix handled above */

        case 0x2F: { /* DAS */
            uint8_t old_al = cpu->al;
            bool old_cf = get_flag(cpu, FLAG_CF);
            set_flag(cpu, FLAG_CF, false);
            if ((cpu->al & 0x0F) > 9 || get_flag(cpu, FLAG_AF)) {
                cpu->al -= 6;
                set_flag(cpu, FLAG_CF, old_cf || (cpu->al > old_al));
                set_flag(cpu, FLAG_AF, true);
            } else {
                set_flag(cpu, FLAG_AF, false);
            }
            if (old_al > 0x99 || old_cf) {
                cpu->al -= 0x60;
                set_flag(cpu, FLAG_CF, true);
            }
            set_flag(cpu, FLAG_ZF, cpu->al == 0);
            set_flag(cpu, FLAG_SF, (cpu->al & 0x80) != 0);
            set_flag(cpu, FLAG_PF, parity8(cpu->al));
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  XOR  (0x30 - 0x35)
         * ════════════════════════════════════════════════════════════ */
        case 0x30: { /* XOR r/m8, r8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_xor8(cpu, val, reg));
            break;
        }
        case 0x31: { /* XOR r/m16, r16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            uint16_t reg = *reg16_ptr(cpu, m.reg_field);
            modrm_write16(cpu, &m, alu_xor16(cpu, val, reg));
            break;
        }
        case 0x32: { /* XOR r8, r/m8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_xor8(cpu, *dst, val);
            break;
        }
        case 0x33: { /* XOR r16, r/m16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t *dst = reg16_ptr(cpu, m.reg_field);
            uint16_t val = modrm_read16(cpu, &m);
            *dst = alu_xor16(cpu, *dst, val);
            break;
        }
        case 0x34: { /* XOR AL, imm8 */
            uint8_t imm = cpu_fetch8(cpu);
            cpu->al = alu_xor8(cpu, cpu->al, imm);
            break;
        }
        case 0x35: { /* XOR AX, imm16 */
            uint16_t imm = cpu_fetch16(cpu);
            cpu->ax = alu_xor16(cpu, cpu->ax, imm);
            break;
        }

        /* 0x36 = SS: prefix handled above */

        case 0x37: { /* AAA */
            if ((cpu->al & 0x0F) > 9 || get_flag(cpu, FLAG_AF)) {
                cpu->al += 6;
                cpu->ah += 1;
                set_flag(cpu, FLAG_AF, true);
                set_flag(cpu, FLAG_CF, true);
            } else {
                set_flag(cpu, FLAG_AF, false);
                set_flag(cpu, FLAG_CF, false);
            }
            cpu->al &= 0x0F;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  CMP  (0x38 - 0x3D)
         * ════════════════════════════════════════════════════════════ */
        case 0x38: { /* CMP r/m8, r8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            alu_cmp8(cpu, val, reg);
            break;
        }
        case 0x39: { /* CMP r/m16, r16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            uint16_t reg = *reg16_ptr(cpu, m.reg_field);
            alu_cmp16(cpu, val, reg);
            break;
        }
        case 0x3A: { /* CMP r8, r/m8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            alu_cmp8(cpu, reg, val);
            break;
        }
        case 0x3B: { /* CMP r16, r/m16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t reg = *reg16_ptr(cpu, m.reg_field);
            uint16_t val = modrm_read16(cpu, &m);
            alu_cmp16(cpu, reg, val);
            break;
        }
        case 0x3C: { /* CMP AL, imm8 */
            uint8_t imm = cpu_fetch8(cpu);
            alu_cmp8(cpu, cpu->al, imm);
            break;
        }
        case 0x3D: { /* CMP AX, imm16 */
            uint16_t imm = cpu_fetch16(cpu);
            alu_cmp16(cpu, cpu->ax, imm);
            break;
        }

        /* 0x3E = DS: prefix handled above */

        case 0x3F: { /* AAS */
            if ((cpu->al & 0x0F) > 9 || get_flag(cpu, FLAG_AF)) {
                cpu->al -= 6;
                cpu->ah -= 1;
                set_flag(cpu, FLAG_AF, true);
                set_flag(cpu, FLAG_CF, true);
            } else {
                set_flag(cpu, FLAG_AF, false);
                set_flag(cpu, FLAG_CF, false);
            }
            cpu->al &= 0x0F;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  INC reg16  (0x40 - 0x47)
         * ════════════════════════════════════════════════════════════ */
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: {
            uint16_t *r = reg16_ptr(cpu, opcode - 0x40);
            *r = alu_inc16(cpu, *r);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  DEC reg16  (0x48 - 0x4F)
         * ════════════════════════════════════════════════════════════ */
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            uint16_t *r = reg16_ptr(cpu, opcode - 0x48);
            *r = alu_dec16(cpu, *r);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  PUSH reg16/32  (0x50 - 0x57)
         * ════════════════════════════════════════════════════════════ */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            if (op32)
                cpu_push32(cpu, *reg32_ptr(cpu, opcode - 0x50));
            else
                cpu_push16(cpu, *reg16_ptr(cpu, opcode - 0x50));
            break;

        /* ════════════════════════════════════════════════════════════
         *  POP reg16/32  (0x58 - 0x5F)
         * ════════════════════════════════════════════════════════════ */
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            if (op32)
                *reg32_ptr(cpu, opcode - 0x58) = cpu_pop32(cpu);
            else
                *reg16_ptr(cpu, opcode - 0x58) = cpu_pop16(cpu);
            break;

        /* ════════════════════════════════════════════════════════════
         *  Jcc short  (0x70 - 0x7F)
         * ════════════════════════════════════════════════════════════ */
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int8_t rel = (int8_t)cpu_fetch8(cpu);
            if (eval_condition(cpu, opcode - 0x70))
                cpu->ip += (int16_t)rel;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  Group 1  (0x80 - 0x83)
         * ════════════════════════════════════════════════════════════ */
        case 0x80: { /* Group 1 r/m8, imm8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t imm = cpu_fetch8(cpu);
            uint8_t result = group1_alu8(cpu, m.reg_field, val, imm);
            if (m.reg_field != 7) /* not CMP */
                modrm_write8(cpu, &m, result);
            break;
        }
        case 0x81: { /* Group 1 r/m16/32, imm16/32 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t imm = cpu_fetch32(cpu);
                uint32_t result = group1_alu32(cpu, m.reg_field, val, imm);
                if (m.reg_field != 7)
                    modrm_write32(cpu, &m, result);
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t imm = cpu_fetch16(cpu);
                uint16_t result = group1_alu16(cpu, m.reg_field, val, imm);
                if (m.reg_field != 7)
                    modrm_write16(cpu, &m, result);
            }
            break;
        }
        case 0x82: { /* Group 1 r/m8, imm8 (alias of 0x80 on 8086) */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t imm = cpu_fetch8(cpu);
            uint8_t result = group1_alu8(cpu, m.reg_field, val, imm);
            if (m.reg_field != 7)
                modrm_write8(cpu, &m, result);
            break;
        }
        case 0x83: { /* Group 1 r/m16/32, sign-extended imm8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t imm = (uint32_t)(int32_t)(int8_t)cpu_fetch8(cpu);
                uint32_t result = group1_alu32(cpu, m.reg_field, val, imm);
                if (m.reg_field != 7)
                    modrm_write32(cpu, &m, result);
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t imm = (uint16_t)(int16_t)(int8_t)cpu_fetch8(cpu);
                uint16_t result = group1_alu16(cpu, m.reg_field, val, imm);
                if (m.reg_field != 7)
                    modrm_write16(cpu, &m, result);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  TEST  (0x84 - 0x85)
         * ════════════════════════════════════════════════════════════ */
        case 0x84: { /* TEST r/m8, r8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            update_flags_logic8(cpu, val & reg);
            break;
        }
        case 0x85: { /* TEST r/m16, r16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            uint16_t reg = *reg16_ptr(cpu, m.reg_field);
            update_flags_logic16(cpu, val & reg);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  XCHG  (0x86 - 0x87)
         * ════════════════════════════════════════════════════════════ */
        case 0x86: { /* XCHG r/m8, r8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t *reg = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            modrm_write8(cpu, &m, *reg);
            *reg = val;
            break;
        }
        case 0x87: { /* XCHG r/m16, r16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t *reg = reg16_ptr(cpu, m.reg_field);
            uint16_t val = modrm_read16(cpu, &m);
            modrm_write16(cpu, &m, *reg);
            *reg = val;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  MOV  (0x88 - 0x8B)
         * ════════════════════════════════════════════════════════════ */
        case 0x88: { /* MOV r/m8, r8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            modrm_write8(cpu, &m, *reg8_ptr(cpu, m.reg_field));
            break;
        }
        case 0x89: { /* MOV r/m16/32, r16/32 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            if (op32)
                modrm_write32(cpu, &m, *reg32_ptr(cpu, m.reg_field));
            else
                modrm_write16(cpu, &m, *reg16_ptr(cpu, m.reg_field));
            break;
        }
        case 0x8A: { /* MOV r8, r/m8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            *reg8_ptr(cpu, m.reg_field) = modrm_read8(cpu, &m);
            break;
        }
        case 0x8B: { /* MOV r16/32, r/m16/32 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            if (op32)
                *reg32_ptr(cpu, m.reg_field) = modrm_read32(cpu, &m);
            else
                *reg16_ptr(cpu, m.reg_field) = modrm_read16(cpu, &m);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  MOV r/m16, sreg  /  MOV sreg, r/m16  (0x8C / 0x8E)
         * ════════════════════════════════════════════════════════════ */
        case 0x8C: { /* MOV r/m16, sreg */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t seg = *seg_ptr(cpu, m.reg_field & 7);
            modrm_write16(cpu, &m, seg);
            break;
        }

        case 0x8D: { /* LEA r16, m */
            modrm_byte = cpu_fetch8(cpu);
            uint8_t lea_mod = (modrm_byte >> 6) & 3;
            uint8_t lea_reg = (modrm_byte >> 3) & 7;
            uint8_t lea_rm  = modrm_byte & 7;
            uint16_t ea = 0;

            /* Compute offset without segment (LEA ignores segment) */
            switch (lea_rm) {
            case 0: ea = cpu->bx + cpu->si; break;
            case 1: ea = cpu->bx + cpu->di; break;
            case 2: ea = cpu->bp + cpu->si; break;
            case 3: ea = cpu->bp + cpu->di; break;
            case 4: ea = cpu->si; break;
            case 5: ea = cpu->di; break;
            case 6:
                if (lea_mod == 0) {
                    ea = cpu_fetch16(cpu);
                } else {
                    ea = cpu->bp;
                }
                break;
            case 7: ea = cpu->bx; break;
            }

            if (lea_mod == 1) {
                ea += (uint16_t)(int16_t)(int8_t)cpu_fetch8(cpu);
            } else if (lea_mod == 2) {
                ea += cpu_fetch16(cpu);
            }
            /* mod==3 is technically invalid for LEA, but we handle it */

            *reg16_ptr(cpu, lea_reg) = ea;
            break;
        }

        case 0x8E: { /* MOV sreg, r/m16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            *seg_ptr(cpu, m.reg_field & 7) = val;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  POP r/m16  (0x8F)
         * ════════════════════════════════════════════════════════════ */
        case 0x8F: { /* POP r/m16 (only reg_field=0 is valid) */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = cpu_pop16(cpu);
            modrm_write16(cpu, &m, val);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  XCHG AX, reg16  (0x90 - 0x97)  (0x90 = NOP)
         * ════════════════════════════════════════════════════════════ */
        case 0x90: /* NOP (XCHG AX,AX) */
            break;
        case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97: {
            uint16_t *r = reg16_ptr(cpu, opcode - 0x90);
            uint16_t tmp = cpu->ax;
            cpu->ax = *r;
            *r = tmp;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  CBW / CWD  (0x98 / 0x99)
         * ════════════════════════════════════════════════════════════ */
        case 0x98: /* CBW */
            cpu->ax = (uint16_t)(int16_t)(int8_t)cpu->al;
            break;
        case 0x99: /* CWD */
            cpu->dx = (cpu->ax & 0x8000) ? 0xFFFF : 0x0000;
            break;

        /* ════════════════════════════════════════════════════════════
         *  CALL far  (0x9A)
         * ════════════════════════════════════════════════════════════ */
        case 0x9A: { /* CALL far ptr16:16 */
            uint16_t off = cpu_fetch16(cpu);
            uint16_t seg = cpu_fetch16(cpu);
            cpu_push16(cpu, cpu->cs);
            cpu_push16(cpu, cpu->ip);
            cpu->cs = seg;
            cpu->ip = off;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  FWAIT / SAHF / LAHF  (0x9B / 0x9E / 0x9F)
         * ════════════════════════════════════════════════════════════ */
        case 0x9B: /* FWAIT/WAIT — NOP (no FPU to wait for) */
            break;
        case 0x9E: /* SAHF — AH → flags low byte */
            cpu->flags = (cpu->flags & 0xFF00) | (cpu->ah & 0xD5) | FLAGS_FIXED;
            break;
        case 0x9F: /* LAHF — flags low byte → AH */
            cpu->ah = (uint8_t)(cpu->flags & 0xFF);
            break;

        /* ════════════════════════════════════════════════════════════
         *  PUSHF / POPF  (0x9C / 0x9D)
         * ════════════════════════════════════════════════════════════ */
        case 0x9C: /* PUSHF / PUSHFD */
            if (op32)
                cpu_push32(cpu, cpu->eflags | FLAGS_FIXED);
            else
                cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
            break;
        case 0x9D: /* POPF / POPFD */
            if (op32)
                cpu->eflags = (cpu_pop32(cpu) & 0x003F7FD5) | FLAGS_FIXED;
            else
                cpu->flags = (cpu_pop16(cpu) & 0x7FD5) | FLAGS_FIXED;
            /* Preserve IOPL (bits 12-13) for 386 CPU detection */
            break;

        /* ════════════════════════════════════════════════════════════
         *  MOV AL/AX, moffs  (0xA0 - 0xA3)
         * ════════════════════════════════════════════════════════════ */
        case 0xA0: { /* MOV AL, [moffs16] */
            uint16_t off = cpu_fetch16(cpu);
            uint16_t seg = (cpu->seg_override >= 0)
                           ? *seg_ptr(cpu, (uint8_t)cpu->seg_override)
                           : cpu->ds;
            cpu->al = dos_mem_read8(vm, dos_linear(seg, off));
            break;
        }
        case 0xA1: { /* MOV AX, [moffs16] */
            uint16_t off = cpu_fetch16(cpu);
            uint16_t seg = (cpu->seg_override >= 0)
                           ? *seg_ptr(cpu, (uint8_t)cpu->seg_override)
                           : cpu->ds;
            cpu->ax = dos_mem_read16(vm, dos_linear(seg, off));
            break;
        }
        case 0xA2: { /* MOV [moffs16], AL */
            uint16_t off = cpu_fetch16(cpu);
            uint16_t seg = (cpu->seg_override >= 0)
                           ? *seg_ptr(cpu, (uint8_t)cpu->seg_override)
                           : cpu->ds;
            dos_mem_write8(vm, dos_linear(seg, off), cpu->al);
            break;
        }
        case 0xA3: { /* MOV [moffs16], AX */
            uint16_t off = cpu_fetch16(cpu);
            uint16_t seg = (cpu->seg_override >= 0)
                           ? *seg_ptr(cpu, (uint8_t)cpu->seg_override)
                           : cpu->ds;
            dos_mem_write16(vm, dos_linear(seg, off), cpu->ax);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  String operations (0xA4 - 0xAF)
         * ════════════════════════════════════════════════════════════ */
        case 0xA4: { /* MOVSB */
            uint16_t src_seg = string_src_seg(cpu);
            if (cpu->rep_active) {
                while (cpu->cx != 0) {
                    uint8_t val = dos_mem_read8(vm, dos_linear(src_seg, cpu->si));
                    dos_mem_write8(vm, dos_linear(cpu->es, cpu->di), val);
                    cpu->si += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
                    cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
                    cpu->cx--;
                }
            } else {
                uint8_t val = dos_mem_read8(vm, dos_linear(src_seg, cpu->si));
                dos_mem_write8(vm, dos_linear(cpu->es, cpu->di), val);
                cpu->si += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
                cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
            }
            break;
        }
        case 0xA5: { /* MOVSW */
            uint16_t src_seg = string_src_seg(cpu);
            if (cpu->rep_active) {
                while (cpu->cx != 0) {
                    uint16_t val = dos_mem_read16(vm, dos_linear(src_seg, cpu->si));
                    dos_mem_write16(vm, dos_linear(cpu->es, cpu->di), val);
                    cpu->si += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
                    cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
                    cpu->cx--;
                }
            } else {
                uint16_t val = dos_mem_read16(vm, dos_linear(src_seg, cpu->si));
                dos_mem_write16(vm, dos_linear(cpu->es, cpu->di), val);
                cpu->si += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
                cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
            }
            break;
        }
        case 0xA6: { /* CMPSB */
            uint16_t src_seg = string_src_seg(cpu);
            if (cpu->rep_active) {
                while (cpu->cx != 0) {
                    uint8_t s = dos_mem_read8(vm, dos_linear(src_seg, cpu->si));
                    uint8_t d = dos_mem_read8(vm, dos_linear(cpu->es, cpu->di));
                    alu_cmp8(cpu, s, d);
                    cpu->si += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
                    cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
                    cpu->cx--;
                    /* REPZ: stop if ZF=0, REPNZ: stop if ZF=1 */
                    if (cpu->rep_type == 1 && !get_flag(cpu, FLAG_ZF)) break;
                    if (cpu->rep_type == 2 && get_flag(cpu, FLAG_ZF))  break;
                }
            } else {
                uint8_t s = dos_mem_read8(vm, dos_linear(src_seg, cpu->si));
                uint8_t d = dos_mem_read8(vm, dos_linear(cpu->es, cpu->di));
                alu_cmp8(cpu, s, d);
                cpu->si += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
                cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
            }
            break;
        }
        case 0xA7: { /* CMPSW */
            uint16_t src_seg = string_src_seg(cpu);
            if (cpu->rep_active) {
                while (cpu->cx != 0) {
                    uint16_t s = dos_mem_read16(vm, dos_linear(src_seg, cpu->si));
                    uint16_t d = dos_mem_read16(vm, dos_linear(cpu->es, cpu->di));
                    alu_cmp16(cpu, s, d);
                    cpu->si += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
                    cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
                    cpu->cx--;
                    if (cpu->rep_type == 1 && !get_flag(cpu, FLAG_ZF)) break;
                    if (cpu->rep_type == 2 && get_flag(cpu, FLAG_ZF))  break;
                }
            } else {
                uint16_t s = dos_mem_read16(vm, dos_linear(src_seg, cpu->si));
                uint16_t d = dos_mem_read16(vm, dos_linear(cpu->es, cpu->di));
                alu_cmp16(cpu, s, d);
                cpu->si += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
                cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  TEST AL/AX, imm  (0xA8 / 0xA9)
         * ════════════════════════════════════════════════════════════ */
        case 0xA8: { /* TEST AL, imm8 */
            uint8_t imm = cpu_fetch8(cpu);
            update_flags_logic8(cpu, cpu->al & imm);
            break;
        }
        case 0xA9: { /* TEST AX, imm16 */
            uint16_t imm = cpu_fetch16(cpu);
            update_flags_logic16(cpu, cpu->ax & imm);
            break;
        }

        case 0xAA: { /* STOSB */
            if (cpu->rep_active) {
                while (cpu->cx != 0) {
                    dos_mem_write8(vm, dos_linear(cpu->es, cpu->di), cpu->al);
                    cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
                    cpu->cx--;
                }
            } else {
                dos_mem_write8(vm, dos_linear(cpu->es, cpu->di), cpu->al);
                cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
            }
            break;
        }
        case 0xAB: { /* STOSW */
            if (cpu->rep_active) {
                while (cpu->cx != 0) {
                    dos_mem_write16(vm, dos_linear(cpu->es, cpu->di), cpu->ax);
                    cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
                    cpu->cx--;
                }
            } else {
                dos_mem_write16(vm, dos_linear(cpu->es, cpu->di), cpu->ax);
                cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
            }
            break;
        }
        case 0xAC: { /* LODSB */
            uint16_t src_seg = string_src_seg(cpu);
            if (cpu->rep_active) {
                while (cpu->cx != 0) {
                    cpu->al = dos_mem_read8(vm, dos_linear(src_seg, cpu->si));
                    cpu->si += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
                    cpu->cx--;
                }
            } else {
                cpu->al = dos_mem_read8(vm, dos_linear(src_seg, cpu->si));
                cpu->si += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
            }
            break;
        }
        case 0xAD: { /* LODSW */
            uint16_t src_seg = string_src_seg(cpu);
            if (cpu->rep_active) {
                while (cpu->cx != 0) {
                    cpu->ax = dos_mem_read16(vm, dos_linear(src_seg, cpu->si));
                    cpu->si += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
                    cpu->cx--;
                }
            } else {
                cpu->ax = dos_mem_read16(vm, dos_linear(src_seg, cpu->si));
                cpu->si += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
            }
            break;
        }
        case 0xAE: { /* SCASB */
            if (cpu->rep_active) {
                while (cpu->cx != 0) {
                    uint8_t val = dos_mem_read8(vm, dos_linear(cpu->es, cpu->di));
                    alu_cmp8(cpu, cpu->al, val);
                    cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
                    cpu->cx--;
                    if (cpu->rep_type == 1 && !get_flag(cpu, FLAG_ZF)) break;
                    if (cpu->rep_type == 2 && get_flag(cpu, FLAG_ZF))  break;
                }
            } else {
                uint8_t val = dos_mem_read8(vm, dos_linear(cpu->es, cpu->di));
                alu_cmp8(cpu, cpu->al, val);
                cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-1 : 1;
            }
            break;
        }
        case 0xAF: { /* SCASW */
            if (cpu->rep_active) {
                while (cpu->cx != 0) {
                    uint16_t val = dos_mem_read16(vm, dos_linear(cpu->es, cpu->di));
                    alu_cmp16(cpu, cpu->ax, val);
                    cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
                    cpu->cx--;
                    if (cpu->rep_type == 1 && !get_flag(cpu, FLAG_ZF)) break;
                    if (cpu->rep_type == 2 && get_flag(cpu, FLAG_ZF))  break;
                }
            } else {
                uint16_t val = dos_mem_read16(vm, dos_linear(cpu->es, cpu->di));
                alu_cmp16(cpu, cpu->ax, val);
                cpu->di += get_flag(cpu, FLAG_DF) ? (uint16_t)-2 : 2;
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  MOV reg8, imm8  (0xB0 - 0xB7)
         * ════════════════════════════════════════════════════════════ */
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
            uint8_t imm = cpu_fetch8(cpu);
            *reg8_ptr(cpu, opcode - 0xB0) = imm;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  MOV reg16/32, imm16/32  (0xB8 - 0xBF)
         * ════════════════════════════════════════════════════════════ */
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            if (op32) {
                uint32_t imm = cpu_fetch32(cpu);
                *reg32_ptr(cpu, opcode - 0xB8) = imm;
            } else {
                uint16_t imm = cpu_fetch16(cpu);
                *reg16_ptr(cpu, opcode - 0xB8) = imm;
            }
            break;

        /* ════════════════════════════════════════════════════════════
         *  Shift/Rotate Group 2: r/m, imm8  (0xC0/0xC1 - 186+)
         * ════════════════════════════════════════════════════════════ */
        case 0xC0: { /* Group 2 r/m8, imm8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t cnt = cpu_fetch8(cpu);
            modrm_write8(cpu, &m, shift_rotate8(cpu, m.reg_field, val, cnt));
            break;
        }
        case 0xC1: { /* Group 2 r/m16, imm8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            uint8_t cnt = cpu_fetch8(cpu);
            modrm_write16(cpu, &m, shift_rotate16(cpu, m.reg_field, val, cnt));
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  RET near  (0xC2 / 0xC3)
         * ════════════════════════════════════════════════════════════ */
        case 0xC2: { /* RET near, pop imm16 bytes */
            uint16_t pop_bytes = cpu_fetch16(cpu);
            if (op32) {
                cpu->eip = cpu_pop32(cpu);
                cpu->esp += pop_bytes;
            } else {
                cpu->ip = cpu_pop16(cpu);
                cpu->sp += pop_bytes;
            }
            break;
        }
        case 0xC3: /* RET near */
            if (op32)
                cpu->eip = cpu_pop32(cpu);
            else
                cpu->ip = cpu_pop16(cpu);
            break;

        /* ════════════════════════════════════════════════════════════
         *  MOV r/m, imm  (0xC6 / 0xC7)
         * ════════════════════════════════════════════════════════════ */
        case 0xC6: { /* MOV r/m8, imm8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t imm = cpu_fetch8(cpu);
            modrm_write8(cpu, &m, imm);
            break;
        }
        case 0xC7: { /* MOV r/m16, imm16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t imm = cpu_fetch16(cpu);
            modrm_write16(cpu, &m, imm);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  ENTER / LEAVE  (0xC8 / 0xC9, 186+)
         * ════════════════════════════════════════════════════════════ */
        case 0xC8: { /* ENTER imm16, imm8 */
            uint16_t alloc_size = cpu_fetch16(cpu);
            uint8_t nesting = cpu_fetch8(cpu);
            if (op32) {
                cpu_push32(cpu, cpu->ebp);
                uint32_t frame = cpu->esp;
                if (nesting > 0) {
                    for (uint8_t i = 1; i < nesting; i++) {
                        cpu->ebp -= 4;
                        cpu_push32(cpu, dos_mem_read32(vm, dos_addr(vm, cpu->ss, cpu->ebp)));
                    }
                    cpu_push32(cpu, frame);
                }
                cpu->ebp = frame;
                cpu->esp -= alloc_size;
            } else {
                cpu_push16(cpu, cpu->bp);
                uint16_t frame = cpu->sp;
                if (nesting > 0) {
                    for (uint8_t i = 1; i < nesting; i++) {
                        cpu->bp -= 2;
                        cpu_push16(cpu, dos_mem_read16(vm, dos_linear(cpu->ss, cpu->bp)));
                    }
                    cpu_push16(cpu, frame);
                }
                cpu->bp = frame;
                cpu->sp -= alloc_size;
            }
            break;
        }
        case 0xC9: /* LEAVE */
            if (op32) {
                cpu->esp = cpu->ebp;
                cpu->ebp = cpu_pop32(cpu);
            } else {
                cpu->sp = cpu->bp;
                cpu->bp = cpu_pop16(cpu);
            }
            break;

        /* ════════════════════════════════════════════════════════════
         *  RETF  (0xCA / 0xCB)
         * ════════════════════════════════════════════════════════════ */
        case 0xCA: { /* RETF, pop imm16 */
            uint16_t pop_bytes = cpu_fetch16(cpu);
            cpu->ip = cpu_pop16(cpu);
            cpu->cs = cpu_pop16(cpu);
            cpu->sp += pop_bytes;
            break;
        }
        case 0xCB: /* RETF */
            if (op32) {
                cpu->eip = cpu_pop32(cpu);
                cpu->cs  = (uint16_t)cpu_pop32(cpu);
            } else {
                cpu->ip = cpu_pop16(cpu);
                cpu->cs = cpu_pop16(cpu);
            }
            if (cpu->protected_mode && !cpu->pm_cs_loaded) {
                cpu->pm_cs_loaded = true;
                cpu->op_size_32 = true;
                cpu->addr_size_32 = true;
                serial_puts("[DOS] PM RETF: CS=");
                serial_puthex(cpu->cs, 4);
                serial_puts(" EIP=");
                serial_puthex(cpu->eip, 8);
                serial_puts(" — PM selector loaded\n");
            }
            break;

        /* ════════════════════════════════════════════════════════════
         *  INT 3 (0xCC) — Breakpoint
         * ════════════════════════════════════════════════════════════ */
        case 0xCC: { /* INT 3 */
            uint16_t s_cs = cpu->cs; uint32_t s_eip = cpu->eip;
            if (cpu->pm_cs_loaded) {
                cpu_push32(cpu, cpu->eflags);
                cpu_push32(cpu, (uint32_t)cpu->cs);
                cpu_push32(cpu, cpu->eip);
            } else {
                cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
                cpu_push16(cpu, cpu->cs);
                cpu_push16(cpu, cpu->ip);
            }
            set_flag(cpu, FLAG_IF, false);
            set_flag(cpu, FLAG_TF, false);
            dos_int_dispatch(vm, 3);
            if (cpu->cs == s_cs && cpu->eip == s_eip) {
                if (cpu->pm_cs_loaded) cpu->esp += 12;
                else cpu->sp += 6;
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  INT  (0xCD)
         * ════════════════════════════════════════════════════════════ */
        case 0xCD: { /* INT imm8 */
            uint8_t int_num = cpu_fetch8(cpu);
            /* Log ALL INTs */
            if (cpu->insn_count < 5000) {
                serial_puts("[INT] ");
                serial_puthex(int_num, 2);
                serial_puts(" AH=");
                serial_puthex(cpu->ah, 2);
                serial_puts(" #");
                serial_putdec(cpu->insn_count);
                serial_puts("\n");
            }
            /* Save return address for detecting C-handled vs IVT-redirect */
            uint16_t saved_cs = cpu->cs;
            uint32_t saved_eip = cpu->eip;

            /* Push interrupt frame */
            if (cpu->pm_cs_loaded) {
                cpu_push32(cpu, cpu->eflags);
                cpu_push32(cpu, (uint32_t)cpu->cs);
                cpu_push32(cpu, cpu->eip);
            } else {
                cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
                cpu_push16(cpu, cpu->cs);
                cpu_push16(cpu, cpu->ip);
            }
            set_flag(cpu, FLAG_IF, false);
            set_flag(cpu, FLAG_TF, false);

            dos_int_dispatch(vm, int_num);

            /* If C handler (CS:IP unchanged), discard the frame — no IRET.
             * Keep current flags (handler set CF etc.), only restore SP.
             * If IVT redirect (CS:IP changed), leave frame for handler's IRET. */
            if (cpu->cs == saved_cs && cpu->eip == saved_eip) {
                /* Discard the interrupt frame from stack */
                if (cpu->pm_cs_loaded) {
                    cpu->esp += 12;  /* 3 × 4 bytes (EIP + CS + EFLAGS) */
                } else {
                    cpu->sp += 6;    /* 3 × 2 bytes (IP + CS + FLAGS) */
                }
                /* Don't restore flags — handler's flags (CF etc.) are correct */
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  INTO  (0xCE) — Interrupt on Overflow
         * ════════════════════════════════════════════════════════════ */
        case 0xCE: /* INTO */
            if (cpu->flags & FLAG_OF) {
                uint16_t s_cs4 = cpu->cs; uint32_t s_eip4 = cpu->eip;
                if (cpu->pm_cs_loaded) {
                    cpu_push32(cpu, cpu->eflags);
                    cpu_push32(cpu, (uint32_t)cpu->cs);
                    cpu_push32(cpu, cpu->eip);
                } else {
                    cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
                    cpu_push16(cpu, cpu->cs);
                    cpu_push16(cpu, cpu->ip);
                }
                set_flag(cpu, FLAG_IF, false);
                set_flag(cpu, FLAG_TF, false);
                dos_int_dispatch(vm, 4);
                if (cpu->cs == s_cs4 && cpu->eip == s_eip4) {
                    if (cpu->pm_cs_loaded) cpu->esp += 12;
                    else cpu->sp += 6;
                }
            }
            break;

        /* ════════════════════════════════════════════════════════════
         *  IRET  (0xCF)
         * ════════════════════════════════════════════════════════════ */
        case 0xCF: /* IRET */
            if (cpu->pm_cs_loaded) {
                cpu->eip    = cpu_pop32(cpu);
                cpu->cs     = (uint16_t)cpu_pop32(cpu);
                cpu->eflags = (cpu_pop32(cpu) & 0x003FFFFF) | FLAGS_FIXED;
            } else {
                cpu->ip    = cpu_pop16(cpu);
                cpu->cs    = cpu_pop16(cpu);
                cpu->flags = (cpu_pop16(cpu) & 0x0FFF) | FLAGS_FIXED;
            }
            break;

        /* ════════════════════════════════════════════════════════════
         *  Shift/Rotate Group 2: r/m, 1  (0xD0/0xD1)
         * ════════════════════════════════════════════════════════════ */
        case 0xD0: { /* Group 2 r/m8, 1 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            modrm_write8(cpu, &m, shift_rotate8(cpu, m.reg_field, val, 1));
            break;
        }
        case 0xD1: { /* Group 2 r/m16, 1 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            modrm_write16(cpu, &m, shift_rotate16(cpu, m.reg_field, val, 1));
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  Shift/Rotate Group 2: r/m, CL  (0xD2/0xD3)
         * ════════════════════════════════════════════════════════════ */
        case 0xD2: { /* Group 2 r/m8, CL */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            modrm_write8(cpu, &m, shift_rotate8(cpu, m.reg_field, val, cpu->cl));
            break;
        }
        case 0xD3: { /* Group 2 r/m16, CL */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            modrm_write16(cpu, &m, shift_rotate16(cpu, m.reg_field, val, cpu->cl));
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  XLAT  (0xD7)
         * ════════════════════════════════════════════════════════════ */
        case 0xD7: { /* XLAT */
            uint16_t seg = (cpu->seg_override >= 0)
                           ? *seg_ptr(cpu, (uint8_t)cpu->seg_override)
                           : cpu->ds;
            cpu->al = dos_mem_read8(vm, dos_linear(seg, cpu->bx + cpu->al));
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  LOOP / LOOPZ / LOOPNZ  (0xE0 - 0xE2)
         * ════════════════════════════════════════════════════════════ */
        case 0xE0: { /* LOOPNZ / LOOPNE */
            int8_t rel = (int8_t)cpu_fetch8(cpu);
            cpu->cx--;
            if (cpu->cx != 0 && !get_flag(cpu, FLAG_ZF))
                cpu->ip += (int16_t)rel;
            break;
        }
        case 0xE1: { /* LOOPZ / LOOPE */
            int8_t rel = (int8_t)cpu_fetch8(cpu);
            cpu->cx--;
            if (cpu->cx != 0 && get_flag(cpu, FLAG_ZF))
                cpu->ip += (int16_t)rel;
            break;
        }
        case 0xE2: { /* LOOP */
            int8_t rel = (int8_t)cpu_fetch8(cpu);
            cpu->cx--;
            if (cpu->cx != 0)
                cpu->ip += (int16_t)rel;
            break;
        }
        case 0xE3: { /* JCXZ */
            int8_t rel = (int8_t)cpu_fetch8(cpu);
            if (cpu->cx == 0)
                cpu->ip += (int16_t)rel;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  IN / OUT imm8  (0xE4 - 0xE7)
         * ════════════════════════════════════════════════════════════ */
        case 0xE4: { /* IN AL, imm8 */
            uint8_t port = cpu_fetch8(cpu);
            cpu->al = dos_io_read8(vm, port);
            break;
        }
        case 0xE5: { /* IN AX, imm8 */
            uint8_t port = cpu_fetch8(cpu);
            cpu->ax = dos_io_read16(vm, port);
            break;
        }
        case 0xE6: { /* OUT imm8, AL */
            uint8_t port = cpu_fetch8(cpu);
            dos_io_write8(vm, port, cpu->al);
            break;
        }
        case 0xE7: { /* OUT imm8, AX */
            uint8_t port = cpu_fetch8(cpu);
            dos_io_write16(vm, port, cpu->ax);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  CALL near  (0xE8)
         * ════════════════════════════════════════════════════════════ */
        case 0xE8: { /* CALL near rel16/32 */
            if (op32) {
                int32_t rel = (int32_t)cpu_fetch32(cpu);
                cpu_push32(cpu, cpu->eip);
                cpu->eip += rel;
            } else {
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                cpu_push16(cpu, cpu->ip);
                cpu->ip += rel;
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  JMP near / short  (0xE9 / 0xEB)
         * ════════════════════════════════════════════════════════════ */
        case 0xE9: { /* JMP near rel16/32 */
            if (op32) {
                int32_t rel = (int32_t)cpu_fetch32(cpu);
                cpu->eip += rel;
            } else {
                int16_t rel = (int16_t)cpu_fetch16(cpu);
                cpu->ip += rel;
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  JMP far  (0xEA)
         * ════════════════════════════════════════════════════════════ */
        case 0xEA: { /* JMP far ptr16:16 or ptr16:32 */
            uint32_t off;
            uint16_t seg;
            if (op32) {
                off = cpu_fetch32(cpu);
                seg = cpu_fetch16(cpu);
            } else {
                off = cpu_fetch16(cpu);
                seg = cpu_fetch16(cpu);
            }
            cpu->cs = seg;
            cpu->eip = off;

            /* First FAR JMP in PM: CS now has a valid PM selector.
             * Enable GDT translation for subsequent memory accesses. */
            if (cpu->protected_mode && !cpu->pm_cs_loaded) {
                cpu->pm_cs_loaded = true;
                cpu->op_size_32 = true;
                cpu->addr_size_32 = true;
                serial_puts("[DOS] PM FAR JMP: CS=");
                serial_puthex(seg, 4);
                serial_puts(" EIP=");
                serial_puthex(off, 8);
                serial_puts(" — PM selector loaded, GDT translation active\n");
            }
            break;
        }

        case 0xEB: { /* JMP short rel8 */
            int8_t rel = (int8_t)cpu_fetch8(cpu);
            cpu->ip += (int16_t)rel;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  IN / OUT DX  (0xEC - 0xEF)
         * ════════════════════════════════════════════════════════════ */
        case 0xEC: /* IN AL, DX */
            cpu->al = dos_io_read8(vm, cpu->dx);
            break;
        case 0xED: /* IN AX, DX */
            cpu->ax = dos_io_read16(vm, cpu->dx);
            break;
        case 0xEE: /* OUT DX, AL */
            dos_io_write8(vm, cpu->dx, cpu->al);
            break;
        case 0xEF: /* OUT DX, AX */
            dos_io_write16(vm, cpu->dx, cpu->ax);
            break;

        /* ════════════════════════════════════════════════════════════
         *  F2/F3 prefixes handled at top as prefix
         * ════════════════════════════════════════════════════════════ */

        /* ════════════════════════════════════════════════════════════
         *  HLT  (0xF4)
         * ════════════════════════════════════════════════════════════ */
        case 0xF4: /* HLT */
            if (cpu->protected_mode) {
                /* In PM, HLT waits for an interrupt (typically timer).
                 * Don't halt the emulator — just advance the BDA tick
                 * counter and continue. DOS4GW uses STI;HLT to idle. */
                extern uint64_t idt_get_ticks(void);
                uint64_t now = idt_get_ticks();
                if (now - vm->last_timer_tick >= 5) {
                    vm->last_timer_tick = now;
                    vm->bios_ticks++;
                    dos_mem_write32(vm, 0x46C, vm->bios_ticks);
                }
                /* Don't halt — continue executing next instruction */
            } else {
                cpu->halted = true;
            }
            break;

        /* ════════════════════════════════════════════════════════════
         *  CMC  (0xF5)
         * ════════════════════════════════════════════════════════════ */
        case 0xF5: /* CMC */
            set_flag(cpu, FLAG_CF, !get_flag(cpu, FLAG_CF));
            break;

        /* ════════════════════════════════════════════════════════════
         *  Group 3  (0xF6 / 0xF7)
         * ════════════════════════════════════════════════════════════ */
        case 0xF6: { /* Group 3 r/m8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            switch (m.reg_field) {
            case 0: /* TEST r/m8, imm8 */
            case 1: { /* TEST r/m8, imm8 (undocumented alias) */
                uint8_t val = modrm_read8(cpu, &m);
                uint8_t imm = cpu_fetch8(cpu);
                update_flags_logic8(cpu, val & imm);
                break;
            }
            case 2: { /* NOT r/m8 */
                uint8_t val = modrm_read8(cpu, &m);
                modrm_write8(cpu, &m, ~val);
                break;
            }
            case 3: { /* NEG r/m8 */
                uint8_t val = modrm_read8(cpu, &m);
                uint8_t result = alu_sub8(cpu, 0, val);
                set_flag(cpu, FLAG_CF, val != 0);
                modrm_write8(cpu, &m, result);
                break;
            }
            case 4: { /* MUL r/m8 (unsigned) */
                uint8_t val = modrm_read8(cpu, &m);
                uint16_t result = (uint16_t)cpu->al * (uint16_t)val;
                cpu->ax = result;
                set_flag(cpu, FLAG_CF, cpu->ah != 0);
                set_flag(cpu, FLAG_OF, cpu->ah != 0);
                break;
            }
            case 5: { /* IMUL r/m8 (signed) */
                uint8_t val = modrm_read8(cpu, &m);
                int16_t result = (int16_t)(int8_t)cpu->al * (int16_t)(int8_t)val;
                cpu->ax = (uint16_t)result;
                bool overflow = (result < -128 || result > 127);
                set_flag(cpu, FLAG_CF, overflow);
                set_flag(cpu, FLAG_OF, overflow);
                break;
            }
            case 6: { /* DIV r/m8 (unsigned) */
                uint8_t val = modrm_read8(cpu, &m);
                if (val == 0) {
                    /* Division by zero: trigger INT 0 */
                    cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
                    cpu_push16(cpu, cpu->cs);
                    cpu_push16(cpu, cpu->ip);
                    dos_int_dispatch(vm, 0);
                    break;
                }
                uint16_t dividend = cpu->ax;
                uint16_t quotient = dividend / val;
                uint8_t  remainder = dividend % val;
                if (quotient > 0xFF) {
                    /* Overflow: trigger INT 0 */
                    cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
                    cpu_push16(cpu, cpu->cs);
                    cpu_push16(cpu, cpu->ip);
                    dos_int_dispatch(vm, 0);
                    break;
                }
                cpu->al = (uint8_t)quotient;
                cpu->ah = remainder;
                break;
            }
            case 7: { /* IDIV r/m8 (signed) */
                uint8_t val = modrm_read8(cpu, &m);
                if (val == 0) {
                    cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
                    cpu_push16(cpu, cpu->cs);
                    cpu_push16(cpu, cpu->ip);
                    dos_int_dispatch(vm, 0);
                    break;
                }
                int16_t dividend = (int16_t)cpu->ax;
                int16_t divisor  = (int16_t)(int8_t)val;
                int16_t quotient = dividend / divisor;
                int8_t  remainder = dividend % divisor;
                if (quotient < -128 || quotient > 127) {
                    cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
                    cpu_push16(cpu, cpu->cs);
                    cpu_push16(cpu, cpu->ip);
                    dos_int_dispatch(vm, 0);
                    break;
                }
                cpu->al = (uint8_t)(int8_t)quotient;
                cpu->ah = (uint8_t)remainder;
                break;
            }
            } /* switch reg_field */
            break;
        }

        case 0xF7: { /* Group 3 r/m16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            switch (m.reg_field) {
            case 0: /* TEST r/m16, imm16 */
            case 1: {
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t imm = cpu_fetch16(cpu);
                update_flags_logic16(cpu, val & imm);
                break;
            }
            case 2: { /* NOT r/m16 */
                uint16_t val = modrm_read16(cpu, &m);
                modrm_write16(cpu, &m, ~val);
                break;
            }
            case 3: { /* NEG r/m16 */
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t result = alu_sub16(cpu, 0, val);
                set_flag(cpu, FLAG_CF, val != 0);
                modrm_write16(cpu, &m, result);
                break;
            }
            case 4: { /* MUL r/m16 (unsigned) */
                uint16_t val = modrm_read16(cpu, &m);
                uint32_t result = (uint32_t)cpu->ax * (uint32_t)val;
                cpu->ax = (uint16_t)result;
                cpu->dx = (uint16_t)(result >> 16);
                set_flag(cpu, FLAG_CF, cpu->dx != 0);
                set_flag(cpu, FLAG_OF, cpu->dx != 0);
                break;
            }
            case 5: { /* IMUL r/m16 (signed) */
                uint16_t val = modrm_read16(cpu, &m);
                int32_t result = (int32_t)(int16_t)cpu->ax * (int32_t)(int16_t)val;
                cpu->ax = (uint16_t)result;
                cpu->dx = (uint16_t)((uint32_t)result >> 16);
                bool overflow = (result < -32768 || result > 32767);
                set_flag(cpu, FLAG_CF, overflow);
                set_flag(cpu, FLAG_OF, overflow);
                break;
            }
            case 6: { /* DIV r/m16 (unsigned) */
                uint16_t val = modrm_read16(cpu, &m);
                if (val == 0) {
                    cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
                    cpu_push16(cpu, cpu->cs);
                    cpu_push16(cpu, cpu->ip);
                    dos_int_dispatch(vm, 0);
                    break;
                }
                uint32_t dividend = ((uint32_t)cpu->dx << 16) | cpu->ax;
                uint32_t quotient = dividend / val;
                uint16_t remainder = dividend % val;
                if (quotient > 0xFFFF) {
                    cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
                    cpu_push16(cpu, cpu->cs);
                    cpu_push16(cpu, cpu->ip);
                    dos_int_dispatch(vm, 0);
                    break;
                }
                cpu->ax = (uint16_t)quotient;
                cpu->dx = remainder;
                break;
            }
            case 7: { /* IDIV r/m16 (signed) */
                uint16_t val = modrm_read16(cpu, &m);
                if (val == 0) {
                    cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
                    cpu_push16(cpu, cpu->cs);
                    cpu_push16(cpu, cpu->ip);
                    dos_int_dispatch(vm, 0);
                    break;
                }
                int32_t dividend = (int32_t)(((uint32_t)cpu->dx << 16) | cpu->ax);
                int32_t divisor  = (int32_t)(int16_t)val;
                int32_t quotient = dividend / divisor;
                int16_t remainder = dividend % divisor;
                if (quotient < -32768 || quotient > 32767) {
                    cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
                    cpu_push16(cpu, cpu->cs);
                    cpu_push16(cpu, cpu->ip);
                    dos_int_dispatch(vm, 0);
                    break;
                }
                cpu->ax = (uint16_t)(int16_t)quotient;
                cpu->dx = (uint16_t)remainder;
                break;
            }
            } /* switch reg_field */
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  CLC / STC / CLI / STI / CLD / STD  (0xF8 - 0xFD)
         * ════════════════════════════════════════════════════════════ */
        case 0xF8: /* CLC */
            set_flag(cpu, FLAG_CF, false);
            break;
        case 0xF9: /* STC */
            set_flag(cpu, FLAG_CF, true);
            break;
        case 0xFA: /* CLI */
            set_flag(cpu, FLAG_IF, false);
            break;
        case 0xFB: /* STI */
            set_flag(cpu, FLAG_IF, true);
            break;
        case 0xFC: /* CLD */
            set_flag(cpu, FLAG_DF, false);
            break;
        case 0xFD: /* STD */
            set_flag(cpu, FLAG_DF, true);
            break;

        /* ════════════════════════════════════════════════════════════
         *  ICEBP / INT1  (0xF1) — debug breakpoint (undocumented)
         * ════════════════════════════════════════════════════════════ */
        case 0xF1:
            /* Treat as NOP — DOS4GW uses this as a debug trap */
            break;

        /* ════════════════════════════════════════════════════════════
         *  Group 4: INC/DEC r/m8  (0xFE)
         * ════════════════════════════════════════════════════════════ */
        case 0xFE: {
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            switch (m.reg_field) {
            case 0: { /* INC r/m8 */
                uint8_t val = modrm_read8(cpu, &m);
                modrm_write8(cpu, &m, alu_inc8(cpu, val));
                break;
            }
            case 1: { /* DEC r/m8 */
                uint8_t val = modrm_read8(cpu, &m);
                modrm_write8(cpu, &m, alu_dec8(cpu, val));
                break;
            }
            default:
                break;  /* silently ignore invalid FE /2-7 */
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  Group 5: INC/DEC/CALL/JMP/PUSH r/m16  (0xFF)
         * ════════════════════════════════════════════════════════════ */
        case 0xFF: {
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            switch (m.reg_field) {
            case 0: { /* INC r/m16 */
                uint16_t val = modrm_read16(cpu, &m);
                modrm_write16(cpu, &m, alu_inc16(cpu, val));
                break;
            }
            case 1: { /* DEC r/m16 */
                uint16_t val = modrm_read16(cpu, &m);
                modrm_write16(cpu, &m, alu_dec16(cpu, val));
                break;
            }
            case 2: { /* CALL r/m16 (near indirect) */
                uint16_t target = modrm_read16(cpu, &m);
                cpu_push16(cpu, cpu->ip);
                cpu->ip = target;
                break;
            }
            case 3: { /* CALL FAR m16:16 (indirect) */
                if (m.is_reg) {
                    serial_puts("[8086] FF /3 on register\n");
                    break;
                }
                uint16_t off = dos_mem_read16(vm, m.addr);
                uint16_t seg = dos_mem_read16(vm, m.addr + 2);
                cpu_push16(cpu, cpu->cs);
                cpu_push16(cpu, cpu->ip);
                cpu->cs = seg;
                cpu->ip = off;
                break;
            }
            case 4: { /* JMP r/m16 (near indirect) */
                uint16_t target = modrm_read16(cpu, &m);
                cpu->ip = target;
                break;
            }
            case 5: { /* JMP FAR m16:16/32 (indirect) */
                if (m.is_reg) {
                    serial_puts("[8086] FF /5 on register\n");
                    break;
                }
                if (op32) {
                    uint32_t off32 = dos_mem_read32(vm, m.addr);
                    uint16_t seg = dos_mem_read16(vm, m.addr + 4);
                    cpu->cs = seg;
                    cpu->eip = off32;
                } else {
                    uint16_t off = dos_mem_read16(vm, m.addr);
                    uint16_t seg = dos_mem_read16(vm, m.addr + 2);
                    cpu->cs = seg;
                    cpu->ip = off;
                }
                /* Detect PM far jump after LMSW/MOV CR0 */
                if (cpu->protected_mode) {
                    serial_puts("[DOS] FAR JMP indirect in PM: CS=");
                    serial_puthex(cpu->cs, 4);
                    serial_puts(" EIP=");
                    serial_puthex(cpu->eip, 8);
                    serial_puts("\n");
                    extern void dos_transfer_to_native(dos_vm_t *vm);
                    dos_transfer_to_native(vm);
                }
                break;
            }
            case 6: { /* PUSH r/m16 */
                uint16_t val = modrm_read16(cpu, &m);
                cpu_push16(cpu, val);
                break;
            }
            case 7: { /* PUSH r/m16 (undocumented alias of /6) */
                uint16_t val = modrm_read16(cpu, &m);
                cpu_push16(cpu, val);
                break;
            }
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  ARPL (0x63) — Adjust RPL of selector
         * ════════════════════════════════════════════════════════════ */
        case 0x63: {
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint16_t dst = modrm_read16(cpu, &m);
            uint16_t src = *reg16_ptr(cpu, m.reg_field);
            if ((dst & 3) < (src & 3)) {
                dst = (dst & ~3) | (src & 3);
                modrm_write16(cpu, &m, dst);
                set_flag(cpu, FLAG_ZF, true);
            } else {
                set_flag(cpu, FLAG_ZF, false);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  BOUND (0x62) — Check array bounds
         * ════════════════════════════════════════════════════════════ */
        case 0x62: {
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            int16_t idx = (int16_t)*reg16_ptr(cpu, m.reg_field);
            int16_t lo  = (int16_t)dos_mem_read16(vm, m.addr);
            int16_t hi  = (int16_t)dos_mem_read16(vm, m.addr + 2);
            if (idx < lo || idx > hi) {
                /* Bounds check failed — trigger INT 5 */
                cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
                cpu_push16(cpu, cpu->cs);
                cpu_push16(cpu, cpu->ip);
                dos_int_dispatch(vm, 5);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  PUSHA / POPA (186+)
         * ════════════════════════════════════════════════════════════ */
        case 0x60: { /* PUSHA / PUSHAD */
            if (op32) {
                uint32_t tmp_esp = cpu->esp;
                cpu_push32(cpu, cpu->eax);
                cpu_push32(cpu, cpu->ecx);
                cpu_push32(cpu, cpu->edx);
                cpu_push32(cpu, cpu->ebx);
                cpu_push32(cpu, tmp_esp);
                cpu_push32(cpu, cpu->ebp);
                cpu_push32(cpu, cpu->esi);
                cpu_push32(cpu, cpu->edi);
            } else {
                uint16_t tmp_sp = cpu->sp;
                cpu_push16(cpu, cpu->ax);
                cpu_push16(cpu, cpu->cx);
                cpu_push16(cpu, cpu->dx);
                cpu_push16(cpu, cpu->bx);
                cpu_push16(cpu, tmp_sp);
                cpu_push16(cpu, cpu->bp);
                cpu_push16(cpu, cpu->si);
                cpu_push16(cpu, cpu->di);
            }
            break;
        }
        case 0x61: { /* POPA / POPAD */
            if (op32) {
                cpu->edi = cpu_pop32(cpu);
                cpu->esi = cpu_pop32(cpu);
                cpu->ebp = cpu_pop32(cpu);
                cpu_pop32(cpu); /* skip ESP */
                cpu->ebx = cpu_pop32(cpu);
                cpu->edx = cpu_pop32(cpu);
                cpu->ecx = cpu_pop32(cpu);
                cpu->eax = cpu_pop32(cpu);
            } else {
                cpu->di = cpu_pop16(cpu);
                cpu->si = cpu_pop16(cpu);
                cpu->bp = cpu_pop16(cpu);
                cpu_pop16(cpu); /* skip SP */
                cpu->bx = cpu_pop16(cpu);
                cpu->dx = cpu_pop16(cpu);
                cpu->cx = cpu_pop16(cpu);
                cpu->ax = cpu_pop16(cpu);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  INS/OUTS (186+) — String I/O
         * ════════════════════════════════════════════════════════════ */
        case 0x6C: { /* INSB: ES:[DI] <- port[DX] */
            uint8_t val = dos_io_read8(vm, cpu->dx);
            dos_mem_write8(vm, dos_linear(cpu->es, cpu->di), val);
            cpu->di += (cpu->flags & FLAG_DF) ? (uint16_t)-1 : 1;
            break;
        }
        case 0x6D: { /* INSW: ES:[DI] <- port[DX] */
            uint16_t val = dos_io_read16(vm, cpu->dx);
            dos_mem_write16(vm, dos_linear(cpu->es, cpu->di), val);
            cpu->di += (cpu->flags & FLAG_DF) ? (uint16_t)-2 : 2;
            break;
        }
        case 0x6E: { /* OUTSB: port[DX] <- DS:[SI] */
            uint8_t val = dos_mem_read8(vm, dos_linear(cpu->ds, cpu->si));
            dos_io_write8(vm, cpu->dx, val);
            cpu->si += (cpu->flags & FLAG_DF) ? (uint16_t)-1 : 1;
            break;
        }
        case 0x6F: { /* OUTSW: port[DX] <- DS:[SI] */
            uint16_t val = dos_mem_read16(vm, dos_linear(cpu->ds, cpu->si));
            dos_io_write16(vm, cpu->dx, val);
            cpu->si += (cpu->flags & FLAG_DF) ? (uint16_t)-2 : 2;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  IMUL r16, r/m16, imm (186+)
         * ════════════════════════════════════════════════════════════ */
        case 0x69: { /* IMUL r16, r/m16, imm16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            if (op32) {
                int32_t src = (int32_t)modrm_read32(cpu, &m);
                int32_t imm = (int32_t)cpu_fetch32(cpu);
                int64_t result = (int64_t)src * (int64_t)imm;
                *reg32_ptr(cpu, m.reg_field) = (uint32_t)result;
                set_flag(cpu, FLAG_CF, result != (int32_t)result);
                set_flag(cpu, FLAG_OF, result != (int32_t)result);
            } else {
                int16_t src = (int16_t)modrm_read16(cpu, &m);
                int16_t imm = (int16_t)cpu_fetch16(cpu);
                int32_t result = (int32_t)src * (int32_t)imm;
                *reg16_ptr(cpu, m.reg_field) = (uint16_t)result;
                set_flag(cpu, FLAG_CF, result != (int16_t)result);
                set_flag(cpu, FLAG_OF, result != (int16_t)result);
            }
            break;
        }
        case 0x6B: { /* IMUL r16, r/m16, imm8 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            if (op32) {
                int32_t src = (int32_t)modrm_read32(cpu, &m);
                int32_t imm = (int32_t)(int8_t)cpu_fetch8(cpu);
                int64_t result = (int64_t)src * (int64_t)imm;
                *reg32_ptr(cpu, m.reg_field) = (uint32_t)result;
                set_flag(cpu, FLAG_CF, result != (int32_t)result);
                set_flag(cpu, FLAG_OF, result != (int32_t)result);
            } else {
                int16_t src = (int16_t)modrm_read16(cpu, &m);
                int16_t imm = (int16_t)(int8_t)cpu_fetch8(cpu);
                int32_t result = (int32_t)src * (int32_t)imm;
                *reg16_ptr(cpu, m.reg_field) = (uint16_t)result;
                set_flag(cpu, FLAG_CF, result != (int16_t)result);
                set_flag(cpu, FLAG_OF, result != (int16_t)result);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  PUSH imm (186+)
         * ════════════════════════════════════════════════════════════ */
        case 0x68: /* PUSH imm16/32 */
            if (op32)
                cpu_push32(cpu, cpu_fetch32(cpu));
            else
                cpu_push16(cpu, cpu_fetch16(cpu));
            break;
        case 0x6A: { /* PUSH imm8 (sign-extended to 16/32) */
            int8_t val = (int8_t)cpu_fetch8(cpu);
            if (op32)
                cpu_push32(cpu, (uint32_t)(int32_t)val);
            else
                cpu_push16(cpu, (uint16_t)(int16_t)val);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  LES/LDS — Load far pointer
         * ════════════════════════════════════════════════════════════ */
        case 0xC4: /* LES reg16, m16:16 */
        case 0xC5: { /* LDS reg16, m16:16 */
            modrm_byte = cpu_fetch8(cpu);
            m = decode_modrm(cpu, modrm_byte);
            uint32_t addr = m.addr;
            uint16_t off_val = dos_mem_read16(vm, addr);
            uint16_t seg_val = dos_mem_read16(vm, addr + 2);
            *reg16_ptr(cpu, m.reg_field) = off_val;
            if (opcode == 0xC4)
                cpu->es = seg_val;
            else
                cpu->ds = seg_val;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  AAM / AAD  (0xD4 / 0xD5)
         * ════════════════════════════════════════════════════════════ */
        case 0xD4: { /* AAM imm8 */
            uint8_t base = cpu_fetch8(cpu);
            if (base == 0) { dos_int_dispatch(vm, 0); break; } /* divide by zero */
            uint8_t al = cpu->al;
            cpu->ah = al / base;
            cpu->al = al % base;
            update_flags_logic8(cpu, cpu->al);
            break;
        }
        case 0xD5: { /* AAD imm8 */
            uint8_t base = cpu_fetch8(cpu);
            cpu->al = (uint8_t)(cpu->ah * base + cpu->al);
            cpu->ah = 0;
            update_flags_logic8(cpu, cpu->al);
            break;
        }
        case 0xD6: /* SALC (undocumented: AL = CF ? 0xFF : 0x00) */
            cpu->al = (cpu->flags & FLAG_CF) ? 0xFF : 0x00;
            break;

        /* ════════════════════════════════════════════════════════════
         *  FPU escape opcodes (0xD8-0xDF) — stub: consume ModRM, NOP
         *  Real FPU emulation is Phase 5+. For now, skip cleanly.
         * ════════════════════════════════════════════════════════════ */
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
            /* FPU instructions have a ModRM byte.
             * If mod != 3, there's a memory operand to skip.
             * If mod == 3, it's register-only (just the ModRM byte). */
            modrm_byte = cpu_fetch8(cpu);
            if ((modrm_byte >> 6) != 3) {
                /* Memory operand — decode to consume displacement bytes */
                (void)decode_modrm(cpu, modrm_byte);
            }
            /* Silently NOP — no FPU state maintained */
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  Unknown opcode
         * ════════════════════════════════════════════════════════════ */
        default:
            /* Log once, don't halt — let the program continue */
            if (cpu->insn_count < 1000000) {
                serial_puts("[8086] unknown opcode: ");
                serial_puthex(opcode, 2);
                serial_puts(" at ");
                serial_puthex(cpu->cs, 4);
                serial_puts(":");
                serial_puthex(cpu->eip - 1, 4);
                serial_puts("\n");
            }
            cpu->exit_code = -1;
            break;

        } /* switch (opcode) */

        /* Increment instruction counter */
        cpu->insn_count++;

        /* Detect CS corruption: log when CS changes to null/invalid in PM */
        if (cpu->pm_cs_loaded && cpu->cs == 0x0000 && cpu->insn_count < 50000) {
            serial_puts("[BUG] CS=0 at #");
            serial_putdec(cpu->insn_count);
            serial_puts(" op=");
            serial_puthex(opcode, 2);
            serial_puts(" EIP=");
            serial_puthex(cpu->eip, 8);
            serial_puts("\n");
            cpu->running = false; /* stop to analyze */
        }


        /* Periodic checks every 16K instructions */
        if ((cpu->insn_count & 0x3FFF) == 0) {
            dos_vga_flush(vm);

            /* Timer tick (~18.2 Hz): update BDA counter + deliver INT 8 */
            extern uint64_t idt_get_ticks(void);
            extern void cpu_deliver_hw_interrupt(dos_vm_t *vm, uint8_t int_num);
            uint64_t now = idt_get_ticks();
            if (now - vm->last_timer_tick >= 5) {  /* 5 ticks @ 100Hz ≈ 50ms */
                vm->last_timer_tick = now;
                vm->bios_ticks++;
                /* Update BIOS Data Area timer counter at 0040:006C (linear 0x46C) */
                dos_mem_write32(vm, 0x46C, vm->bios_ticks);
                /* Don't inject INT 8 — DOS4GW's IDT at base 0 has no valid
                 * gate descriptors (just repeated 0x1308 pattern).
                 * Instead, only update BDA tick counter. DOS4GW polls the
                 * PIT port (0x40) and BDA tick count for timing. */
            }
        }

        /* Periodic status log every 100M instructions */
        if ((cpu->insn_count % 100000000) == 0) {
            serial_puts("[8086] ");
            serial_putdec(cpu->insn_count / 1000000);
            serial_puts("M insn, CS:EIP=");
            serial_puthex(cpu->cs, 4);
            serial_puts(":");
            serial_puthex(cpu->eip, 8);
            serial_puts(cpu->protected_mode ? " [PM]" : " [RM]");
            /* Dump: translated address + instruction bytes */
            uint32_t dump_addr = dos_addr(vm, cpu->cs, cpu->eip);
            serial_puts(" linear=");
            serial_puthex(dump_addr, 8);
            serial_puts(" op=");
            serial_puthex(dos_mem_read8(vm, dump_addr), 2);
            serial_puts(" GDT_idx=");
            serial_puthex(cpu->cs >> 3, 4);
            serial_puts("/lim=");
            serial_puthex(cpu->gdtr.limit, 4);
            serial_puts("\n");
        }
        /* Safety: halt after 2 billion instructions */
        if (cpu->insn_count > 500000000ULL) {
            serial_puts("[8086] 2B instruction limit reached, halting\n");
            cpu->running = false;
        }

    } /* while running */

    return cpu->exit_code;
}
