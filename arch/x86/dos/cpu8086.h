/*
 * OsitoK — 8086/386 CPU Emulator Interface
 *
 * Software interpreter for Intel 8086 through 80386 instruction set.
 * Supports real mode (16-bit) and protected mode (32-bit via DPMI).
 * Runs DOS 16-bit and 32-bit protected mode code within the 64-bit OsitoK kernel.
 */

#ifndef CPU8086_H
#define CPU8086_H

#include "dos_types.h"

/* ── CPU state ──────────────────────────────────────────────────── */

typedef struct cpu8086_state {
    /* General-purpose registers: 32-bit with 16-bit and 8-bit overlays.
     * On little-endian x86-64: EAX at offset 0, AX overlaps low 16 bits,
     * AL at offset 0 (low byte), AH at offset 1 (next byte). */
    union { uint32_t eax; union { uint16_t ax; struct { uint8_t al, ah; }; }; };
    union { uint32_t ecx; union { uint16_t cx; struct { uint8_t cl, ch; }; }; };
    union { uint32_t edx; union { uint16_t dx; struct { uint8_t dl, dh; }; }; };
    union { uint32_t ebx; union { uint16_t bx; struct { uint8_t bl, bh; }; }; };

    /* Index and pointer registers: 32-bit with 16-bit overlay */
    union { uint32_t esp; uint16_t sp; };
    union { uint32_t ebp; uint16_t bp; };
    union { uint32_t esi; uint16_t si; };
    union { uint32_t edi; uint16_t di; };

    /* Segment registers (always 16-bit, even in protected mode) */
    uint16_t cs, ds, es, ss;
    uint16_t fs, gs;

    /* Instruction pointer: 32-bit with 16-bit overlay */
    union { uint32_t eip; uint16_t ip; };

    /* Flags register: 32-bit with 16-bit overlay */
    union { uint32_t eflags; uint16_t flags; };

    /* ── Control registers (386+) ─────────────────────────────── */
    uint32_t cr0;         /* bit 0 = PE (protected mode enable) */
    uint32_t cr2;         /* page fault linear address (stub) */
    uint32_t cr3;         /* page directory base (stub) */

    /* Descriptor table registers */
    struct { uint16_t limit; uint32_t base; } gdtr;
    struct { uint16_t limit; uint32_t base; } idtr;

    /* ── Mode state ───────────────────────────────────────────── */
    bool     protected_mode;   /* true when CR0.PE=1 and DPMI active */
    bool     op_size_32;       /* default operand size for current CS (D bit) */
    bool     addr_size_32;     /* default address size for current CS */

    /* ── Execution control ────────────────────────────────────── */
    bool     running;
    bool     halted;
    int32_t  exit_code;
    uint64_t insn_count;

    /* ── Prefix state (reset each instruction) ────────────────── */
    int      seg_override;  /* -1=none, 0=ES, 1=CS, 2=SS, 3=DS, 4=FS, 5=GS */
    bool     rep_active;
    uint8_t  rep_type;      /* 0=none, 1=REP/REPZ, 2=REPNZ */
    bool     prefix_66;     /* operand size override */
    bool     prefix_67;     /* address size override */

    /* Back-pointer to VM */
    dos_vm_t *vm;
} cpu8086_state_t;

/* ── Flags ──────────────────────────────────────────────────────── */

#define FLAG_CF   (1 << 0)
#define FLAG_PF   (1 << 2)
#define FLAG_AF   (1 << 4)
#define FLAG_ZF   (1 << 6)
#define FLAG_SF   (1 << 7)
#define FLAG_TF   (1 << 8)
#define FLAG_IF   (1 << 9)
#define FLAG_DF   (1 << 10)
#define FLAG_OF   (1 << 11)

/* Always-1 bits in flags register */
#define FLAGS_FIXED  0x0002

/* ── Memory access ──────────────────────────────────────────────── */

/* Real-mode linear address: segment * 16 + offset */
static inline uint32_t dos_linear(uint16_t seg, uint16_t off)
{
    return ((uint32_t)seg << 4) + off;
}

/* Forward declaration for protected-mode translation */
struct dpmi_state;
uint32_t dpmi_translate(dos_vm_t *vm, uint16_t selector, uint32_t offset);

/* Unified address translation: real mode or protected mode */
static inline uint32_t dos_addr(dos_vm_t *vm, uint16_t seg, uint32_t off)
{
    if (vm->cpu && vm->cpu->protected_mode)
        return dpmi_translate(vm, seg, off);
    return ((uint32_t)seg << 4) + (uint16_t)off;
}

static inline uint8_t dos_mem_read8(dos_vm_t *vm, uint32_t addr)
{
    if (addr >= vm->total_mem_size) return 0xFF;
    return vm->mem[addr];
}

static inline uint16_t dos_mem_read16(dos_vm_t *vm, uint32_t addr)
{
    return dos_mem_read8(vm, addr) | ((uint16_t)dos_mem_read8(vm, addr + 1) << 8);
}

static inline uint32_t dos_mem_read32(dos_vm_t *vm, uint32_t addr)
{
    return dos_mem_read16(vm, addr) | ((uint32_t)dos_mem_read16(vm, addr + 2) << 16);
}

/* Write with VGA dirty tracking */
void dos_mem_write8(dos_vm_t *vm, uint32_t addr, uint8_t val);
void dos_mem_write16(dos_vm_t *vm, uint32_t addr, uint16_t val);
void dos_mem_write32(dos_vm_t *vm, uint32_t addr, uint32_t val);

/* ── Fetch helpers ──────────────────────────────────────────────── */

static inline uint8_t cpu_fetch8(cpu8086_state_t *cpu)
{
    uint32_t addr = cpu->protected_mode
        ? dos_addr(cpu->vm, cpu->cs, cpu->eip)
        : dos_linear(cpu->cs, cpu->ip);
    uint8_t val = dos_mem_read8(cpu->vm, addr);
    cpu->eip++;  /* works for both modes: in real mode only low 16 bits used */
    return val;
}

static inline uint16_t cpu_fetch16(cpu8086_state_t *cpu)
{
    uint32_t addr = cpu->protected_mode
        ? dos_addr(cpu->vm, cpu->cs, cpu->eip)
        : dos_linear(cpu->cs, cpu->ip);
    uint16_t val = dos_mem_read16(cpu->vm, addr);
    cpu->eip += 2;
    return val;
}

static inline uint32_t cpu_fetch32(cpu8086_state_t *cpu)
{
    uint32_t addr = cpu->protected_mode
        ? dos_addr(cpu->vm, cpu->cs, cpu->eip)
        : dos_linear(cpu->cs, cpu->ip);
    uint32_t val = dos_mem_read32(cpu->vm, addr);
    cpu->eip += 4;
    return val;
}

/* ── Stack operations ───────────────────────────────────────────── */

static inline void cpu_push16(cpu8086_state_t *cpu, uint16_t val)
{
    cpu->sp -= 2;
    uint32_t addr = cpu->protected_mode
        ? dos_addr(cpu->vm, cpu->ss, cpu->esp)
        : dos_linear(cpu->ss, cpu->sp);
    dos_mem_write16(cpu->vm, addr, val);
}

static inline uint16_t cpu_pop16(cpu8086_state_t *cpu)
{
    uint32_t addr = cpu->protected_mode
        ? dos_addr(cpu->vm, cpu->ss, cpu->esp)
        : dos_linear(cpu->ss, cpu->sp);
    uint16_t val = dos_mem_read16(cpu->vm, addr);
    cpu->sp += 2;
    return val;
}

static inline void cpu_push32(cpu8086_state_t *cpu, uint32_t val)
{
    cpu->esp -= 4;
    uint32_t addr = cpu->protected_mode
        ? dos_addr(cpu->vm, cpu->ss, cpu->esp)
        : dos_linear(cpu->ss, cpu->sp);
    dos_mem_write32(cpu->vm, addr, val);
}

static inline uint32_t cpu_pop32(cpu8086_state_t *cpu)
{
    uint32_t addr = cpu->protected_mode
        ? dos_addr(cpu->vm, cpu->ss, cpu->esp)
        : dos_linear(cpu->ss, cpu->sp);
    uint32_t val = dos_mem_read32(cpu->vm, addr);
    cpu->esp += 4;
    return val;
}

/* ── Parity lookup ──────────────────────────────────────────────── */

static inline bool parity8(uint8_t v)
{
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (~v) & 1;
}

/* ── Public API ─────────────────────────────────────────────────── */

void cpu8086_init(cpu8086_state_t *cpu, dos_vm_t *vm);
int cpu8086_run(dos_vm_t *vm);

#endif /* CPU8086_H */
