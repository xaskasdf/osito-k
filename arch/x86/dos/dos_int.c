/*
 * OsitoK — DOS Interrupt Dispatch
 *
 * Routes software INT instructions to the appropriate handler:
 *   INT 20h → terminate
 *   INT 21h → DOS API (dos_api.c)
 *   INT 10h → BIOS video (dos_bios.c)
 *   INT 16h → BIOS keyboard (dos_bios.c)
 *   INT 1Ah → BIOS timer (dos_bios.c)
 *   Others  → fall through to IVT
 */

#include "cpu8086.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* Forward declarations for service handlers */
void dos_int21_dispatch(dos_vm_t *vm);
void dos_int10_video(dos_vm_t *vm);
void dos_int16_keyboard(dos_vm_t *vm);
void dos_int1a_timer(dos_vm_t *vm);
void dos_int2f_dispatch(dos_vm_t *vm);
void dos_int31_dpmi(dos_vm_t *vm);
void dpmi_enter_protected_mode(dos_vm_t *vm);
void dos_int67_dispatch(dos_vm_t *vm);

/* ── INT dispatch ───────────────────────────────────────────────── */

void dos_int_dispatch(dos_vm_t *vm, uint8_t int_num)
{
    switch (int_num) {
    case 0x10:
        dos_int10_video(vm);
        break;

    case 0x16:
        dos_int16_keyboard(vm);
        break;

    case 0x15: {
        /* BIOS services — extended memory, system config */
        cpu8086_state_t *c15 = vm->cpu;
        switch (c15->ah) {
        case 0x87: /* Move block (extended memory copy) */
            c15->flags &= ~FLAG_CF;  /* success */
            c15->ah = 0;
            break;
        case 0x88: /* Get extended memory size (KB above 1MB) */
            c15->ax = (vm->total_mem_size > 0x100000) ?
                      (uint16_t)((vm->total_mem_size - 0x100000) / 1024) : 0;
            c15->flags &= ~FLAG_CF;
            break;
        case 0xBF: /* DOS4GW extended memory query */
            /* Return: AX = extended memory in KB */
            c15->ax = (vm->total_mem_size > 0x100000) ?
                      (uint16_t)((vm->total_mem_size - 0x100000) / 1024) : 0;
            c15->flags &= ~FLAG_CF;
            break;
        case 0xE8: /* Get memory map (E820h) */
            if (c15->al == 0x01) {
                /* E801h: Get memory size for >64MB */
                c15->ax = 0x3C00;  /* 15MB in 1KB units */
                c15->bx = 0;       /* 0 in 64KB units above 16MB */
                c15->cx = 0x3C00;
                c15->dx = 0;
                c15->flags &= ~FLAG_CF;
            } else {
                c15->flags |= FLAG_CF;
            }
            break;
        default:
            c15->flags |= FLAG_CF;  /* unsupported */
            break;
        }
        break;
    }

    case 0x1A:
        dos_int1a_timer(vm);
        break;

    case 0x20:
        /* Terminate program */
        vm->cpu->running = false;
        vm->cpu->exit_code = 0;
        break;

    case 0x21:
        dos_int21_dispatch(vm);
        break;

    case 0x2F:
        dos_int2f_dispatch(vm);
        break;

    case 0x31:
        dos_int31_dpmi(vm);
        break;

    case 0x33:
        /* Mouse stub: not installed */
        vm->cpu->ax = 0x0000;
        break;

    case 0x67:
        dos_int67_dispatch(vm);
        break;

    case 0xFE:
        /* DPMI entry trigger: switch to protected mode */
        dpmi_enter_protected_mode(vm);
        break;

    default: {
        /* Check IVT for user-installed handlers */
        uint32_t ivt_addr = (uint32_t)int_num * 4;
        uint16_t off = dos_mem_read16(vm, ivt_addr);
        uint16_t seg = dos_mem_read16(vm, ivt_addr + 2);

        if (seg >= (DOS_ROM_BASE >> 4)) {
            /* ROM stub — log which INT hit it (may reveal missing handlers) */
            if (vm->cpu->insn_count < 5000) {
                serial_puts("[STUB] INT ");
                serial_puthex(int_num, 2);
                serial_puts(" -> ROM stub #");
                serial_putdec(vm->cpu->insn_count);
                serial_puts("\n");
            }
            break;
        }

        /* User-installed handler: jump to it.
         * case 0xCD already pushed the interrupt frame (flags/CS/IP).
         * The handler's IRET will pop that frame and return. */
        vm->cpu->cs = seg;
        vm->cpu->ip = off;
        vm->cpu->flags &= ~(FLAG_IF | FLAG_TF);
        break;
    }
    }
}

/* ── Hardware interrupt delivery (timer, etc.) ──────────────────── */

void cpu_deliver_hw_interrupt(dos_vm_t *vm, uint8_t int_num)
{
    cpu8086_state_t *cpu = vm->cpu;

    /* In real mode, respect IF flag. In protected mode, always deliver
     * (DOS4GW manages its own interrupt state via the PIC/IDT and
     * may have IF=0 while still expecting timer ticks) */
    if (!cpu->protected_mode && !(cpu->flags & FLAG_IF)) return;

    if (cpu->protected_mode) {
        /* Check DPMI pm_vectors first (DOS4GW hooks INT 8 via INT 31h/0205h) */
        if (vm->dpmi.pm_vectors[int_num].sel != 0) {
            cpu_push32(cpu, cpu->eflags);
            cpu_push32(cpu, (uint32_t)cpu->cs);
            cpu_push32(cpu, cpu->eip);
            cpu->cs  = vm->dpmi.pm_vectors[int_num].sel;
            cpu->eip = vm->dpmi.pm_vectors[int_num].off;
            cpu->flags &= ~(FLAG_IF | FLAG_TF);
            return;
        }
        /* Read guest IDT via page walker (IDT may be at high virtual address) */
        if (cpu->idtr.base) {
            uint32_t idt_linear = cpu->idtr.base + (uint32_t)int_num * 8;
            uint32_t entry = dpmi_translate(vm, 0, idt_linear);
            if (entry + 7 < vm->total_mem_size) {
                uint16_t off_lo = dos_mem_read16(vm, entry);
                uint16_t sel    = dos_mem_read16(vm, entry + 2);
                uint16_t off_hi = dos_mem_read16(vm, entry + 6);
                uint32_t handler = ((uint32_t)off_hi << 16) | off_lo;
                if (sel != 0 && handler != 0) {
                    /* Log first few timer deliveries */
                    static int timer_log = 0;
                    if (timer_log < 5) {
                        serial_puts("[TIMER] INT ");
                        serial_puthex(int_num, 2);
                        serial_puts(" -> ");
                        serial_puthex(sel, 4);
                        serial_puts(":");
                        serial_puthex(handler, 8);
                        serial_puts(" IDTphys=");
                        serial_puthex(entry, 8);
                        serial_puts("\n");
                        timer_log++;
                    }
                    cpu_push32(cpu, cpu->eflags);
                    cpu_push32(cpu, (uint32_t)cpu->cs);
                    cpu_push32(cpu, cpu->eip);
                    cpu->cs  = sel;
                    cpu->eip = handler;
                    cpu->flags &= ~(FLAG_IF | FLAG_TF);
                    return;
                }
            }
        }
        /* Try IDT via paging (DOS4GW maps IDT at high virtual address) */
        if (cpu->idtr.base) {
            /* dpmi_translate will walk page tables if CR0.PG is set */
            uint32_t entry = dpmi_translate(vm, 0, cpu->idtr.base + (uint32_t)int_num * 8);
            if (entry + 7 < vm->total_mem_size) {
                uint16_t off_lo = dos_mem_read16(vm, entry);
                uint16_t sel    = dos_mem_read16(vm, entry + 2);
                uint16_t off_hi = dos_mem_read16(vm, entry + 6);
                uint32_t handler = ((uint32_t)off_hi << 16) | off_lo;
                if (sel != 0 && handler != 0) {
                    cpu_push32(cpu, cpu->eflags);
                    cpu_push32(cpu, (uint32_t)cpu->cs);
                    cpu_push32(cpu, cpu->eip);
                    cpu->cs  = sel;
                    cpu->eip = handler;
                    cpu->flags &= ~(FLAG_IF | FLAG_TF);
                }
            }
        }
    } else {
        /* Real mode: push flags/CS/IP and dispatch via IVT */
        cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
        cpu_push16(cpu, cpu->cs);
        cpu_push16(cpu, cpu->ip);
        cpu->flags &= ~(FLAG_IF | FLAG_TF);
        dos_int_dispatch(vm, int_num);
    }
}

/* ── Native 32-bit INT dispatch (called from dos_int_stub.S) ────── */
/* Register frame layout matching the assembly stub's push order:
 * ES, DS, R15-R8, RBP, RDI, RSI, RDX, RCX, RBX, RAX */

typedef struct {
    uint64_t es, ds;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* IRET frame (pushed by CPU on INT entry, popped by IRETQ on return).
     * Layout matches dos_int_stub.S after the GPR area. */
    uint64_t iret_rip, iret_cs, iret_rflags, iret_rsp, iret_ss;
} dos_native_regs_t;

/*
 * Global DOS VM state for native 32-bit execution.
 * Set up by dos_transfer_to_native() before jumping to 32-bit code.
 * The native dispatch reads/writes this to provide DOS services.
 */
static dos_vm_t *g_native_dos_vm = 0;
static cpu8086_state_t g_native_cpu;

void dos_set_native_vm(dos_vm_t *vm)
{
    g_native_dos_vm = vm;
    extern void dos_vga_set_native_vm(dos_vm_t *vm);
    dos_vga_set_native_vm(vm);
    /* Prime the keyboard buffer with a few synthetic keystrokes so DOS
     * programs that block on INT 16h AH=00 or INT 21h AH=07/08 can make
     * initial progress in headless tests. Harmless if consumed or not. */
    extern void kb_push(char c);
    kb_push('\r');   /* ENTER — dismisses prompts */
    kb_push(' ');    /* SPACE — skips DOOM intro sometimes */
    kb_push('\r');
}

/* Set by the shell 'dosrun' command via kern_setjmp, read by the IDT
 * exception path and by dos_int_native_dispatch on program terminate
 * so DOS crashes / exits return cleanly to the shell prompt. */
uint64_t *dos_native_exit_jmpbuf = 0;

/* DOS4GW-specific surgical LRETW emulation. Reads the target IP:CS
 * from the DOS stack and rewrites the iret frame so the IRETQ from
 * the kernel's #GP handler lands at the target linear address — but
 * IN THE CURRENT CS, not the popped one. Works as long as target
 * linear falls inside current CS's base..base+limit window, which is
 * true in practice for DOS4GW's overlapping code segments.
 *
 * Returns 1 if emulation succeeded (caller returns from ISR normally);
 * 0 if not applicable (caller falls back to long-jump crash recovery).
 *
 * Why this works: x86_64 hardware strictly validates CS descriptors on
 * RETF/IRET/JMP FAR (CODE bit, DPL, limit). DOS4GW pre-dates strict
 * 64-bit validation and pushes DATA selectors as CS. By keeping the
 * current (valid) CS and only adjusting RIP, we skip the hardware
 * check without sacrificing universality: only DOS4GW mode triggers. */
int dos_native_emulate_lretw(void *frame_ptr);

int dos_native_promote_to_code(uint16_t sel)
{
    if (!g_native_dos_vm) return 0;
    if ((sel & 0x04) == 0) return 0;              /* must be LDT */
    uint16_t idx = (sel >> 3) & 0x1FFF;
    if (idx >= DPMI_MAX_DESCRIPTORS) return 0;

    dpmi_descriptor_t *d = &g_native_dos_vm->dpmi.ldt[idx];
    if (!(d->access & 0x80))         return 0;    /* not present */
    if (d->access & 0x08)            return 0;    /* already CODE */

    static uint32_t promote_count = 0;
    if (++promote_count > 32) return 0;            /* rate-limit */

    /* Flip DATA → CODE readable, keep DPL=0 (matches our transfer).
     * 0x9B = P(1) DPL(00) S(1) type(1011=code-readable-non-conforming-ACCESSED).
     * A (bit 0) is set preemptively — some CPU validation paths refuse
     * to load a descriptor without the A bit if the LDT page isn't
     * confirmed writable to the CPU's access-bit update.
     * Also expand the segment limit to 4GB. The original descriptor was
     * SetDesc'd with lim=0, which rejects any EIP>0 after the CS load.
     * Set G=1 (4KB granularity) and limit[19:16]=0xF, limit[15:0]=0xFFFF
     * for a 4GB flat segment. */
    uint8_t old_acc = d->access;
    uint8_t old_flg = d->flags_lim;
    d->access    = 0x9B;
    d->limit_lo  = 0xFFFF;
    /* flags_lim: high nibble = G|D|L|AVL; low nibble = limit[19:16]. */
    /* G=1 (4KB granularity) + D=0 (16-bit default — match DOOM's current
     * 16-bit LDT[0] CS so a CALL/RET can transition without operand-size
     * mismatch) + limit[19:16]=0xF → flags_lim = 0x8F. */
    d->flags_lim = 0x8F;
    serial_puts("[DOS-NT] LDT[");  serial_putdec(idx);
    serial_puts("] access 0x");    serial_puthex(old_acc, 2);
    serial_puts("/flags 0x");      serial_puthex(old_flg, 2);
    serial_puts(" -> 0x9B/0x");    serial_puthex(d->flags_lim, 2);
    serial_puts(" limit=4GB flat (CODE readable)\n");
    return 1;
}

/* Minimal mirror of the kernel interrupt_frame_t. Kept local so this
 * file doesn't need to include the kernel IDT header. Layout must
 * match isr_stubs.S / idt.c's interrupt_frame_t. */
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} dos_nt_iframe_t;

static uint32_t dos_nt_ldt_base(dos_vm_t *vm, uint16_t sel)
{
    uint16_t idx = (sel >> 3) & 0x1FFF;
    /* GDT selector (TI=0): read base out of kernel_gdt directly.  Used
     * for the DOS4GW aliases at slots 3 (sel 0x18) and 4 (sel 0x20). */
    if ((sel & 0x04) == 0) {
        extern uint64_t kernel_gdt[];
        uint64_t d = kernel_gdt[idx];
        uint32_t base = (uint32_t)((d >> 16) & 0xFFFF)
                      | (uint32_t)(((d >> 32) & 0xFF) << 16)
                      | (uint32_t)(((d >> 56) & 0xFF) << 24);
        return base;
    }
    /* LDT selector (TI=1): read out of vm->dpmi.ldt. */
    if (idx >= DPMI_MAX_DESCRIPTORS) return 0;
    dpmi_descriptor_t *d = &vm->dpmi.ldt[idx];
    return ((uint32_t)d->base_lo)
         | ((uint32_t)d->base_mid << 16)
         | ((uint32_t)d->base_hi  << 24);
}

int dos_native_emulate_lretw(void *frame_ptr)
{
    dos_nt_iframe_t *f = (dos_nt_iframe_t *)frame_ptr;
    dos_vm_t *vm = g_native_dos_vm;
    /* Rate-limit the entry trace so the segment-load loops don't drown
     * out the rest of the run. Show first 32 calls in full, then sample
     * 1 in every 1024 thereafter. */
    static uint32_t emu_calls = 0;
    int verbose = (++emu_calls < 32) || ((emu_calls & 0x3FF) == 0);
    if (verbose) {
        serial_puts("[emu] enter cs=0x"); serial_puthex(f->cs & 0xFFFF, 4);
        serial_puts(" rip=0x"); serial_puthex(f->rip, 8);
        serial_puts(" #"); serial_putdec(emu_calls);
        serial_puts("\n");
    }
    if (!vm || !vm->dos4gw_mode) return 0;

    /* Accept either an LDT selector (TI=1) OR one of the DOS4GW GDT
     * aliases we install at slots 3 / 4 (sel 0x18, 0x20). The CS lookup
     * for these uses the kernel GDT in dos_nt_ldt_base() — we patch
     * that helper below. */
    if ((f->cs & 0x04) == 0) {
        uint16_t cs = (uint16_t)f->cs;
        if (cs != 0x18 && cs != 0x20) return 0;
    }

    /* vm->mem is a PA that is identity-mapped only in the kernel CR3,
     * so switch temporarily to read/write it safely. */
    extern uint64_t paging_get_kernel_cr3(void);
    uint64_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    uint64_t kcr3 = paging_get_kernel_cr3();
    if (kcr3 && saved_cr3 != kcr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(kcr3) : "memory");

    /* Verify the faulting instruction is a far return variant that
     * reads 4 bytes (IP16:CS16) from the stack:
     *   0xCB            — RETF (no imm)
     *   0xCA imm16      — RETF <imm>
     * Other forms (CALLF/IRET) are left alone for now. */
    uint32_t cs_base = dos_nt_ldt_base(vm, (uint16_t)f->cs);
    uint32_t fault_linear = cs_base + (uint32_t)f->rip;
    if (fault_linear >= vm->total_mem_size) return 0;
    #define DOS_NT_EMU_FAIL do { \
        if (kcr3 && saved_cr3 != kcr3) \
            __asm__ volatile ("mov %0, %%cr3" :: "r"(saved_cr3) : "memory"); \
        return 0; } while (0)

    /* Support a few far-control opcodes DOS4GW triggers #GP on:
     *   0xCB              — RETF (pop IP:CS, 4 bytes)
     *   0xCA imm16        — RETF <imm16> (pop IP:CS, 4 + imm)
     *   0xCF              — IRET (pop IP:CS:FLAGS, 6 bytes)
     *   0x66 0xCF         — IRETD (pop EIP:CS:EFLAGS, 12 bytes)
     *   0xEA off16:seg16  — JMP FAR direct (no stack pop)
     *   0xFF /5 m16:16    — JMP FAR [mem] (indirect) — read IP:CS from [DS:disp16]
     *   0xFF /3 m16:16    — CALL FAR [mem] (indirect) — push CS:IP, then jump */
    /* Skip up to 4 instruction prefixes (segment-override + operand-/
     * address-size). Track the operand-size override (0x66) explicitly
     * since some opcode variants change behavior based on it. */
    uint8_t opc = vm->mem[fault_linear];
    uint8_t opc_prefix = 0;
    uint8_t opc_modrm  = 0;
    uint32_t prefix_bytes = 0;
    int opc_is_ff_5    = 0;     /* JMP FAR indirect */
    int opc_is_ff_3    = 0;     /* CALL FAR indirect */
    int opc_is_mov_seg = 0;     /* 0x8E: MOV Sreg, r/m16 */
    int opc_is_les     = 0;     /* 0xC4: LES r16, m16:16 */
    int opc_is_lds     = 0;     /* 0xC5: LDS r16, m16:16 */
    for (int p = 0; p < 4; p++) {
        if (fault_linear + prefix_bytes >= vm->total_mem_size) break;
        uint8_t b = vm->mem[fault_linear + prefix_bytes];
        if (b == 0x66) {                 /* operand-size override */
            opc_prefix = 0x66;
            prefix_bytes++;
        } else if (b == 0x67 ||          /* addr-size override */
                   b == 0x26 || b == 0x2E || b == 0x36 ||
                   b == 0x3E || b == 0x64 || b == 0x65 ||
                   b == 0xF0 || b == 0xF2 || b == 0xF3) {
            prefix_bytes++;              /* skip but don't track */
        } else {
            break;
        }
    }
    /* op_off = absolute address of the opcode byte (fault_linear is the
     * first byte of the instruction, which may include prefixes). */
    uint32_t op_off = fault_linear + prefix_bytes;
    opc = vm->mem[op_off];
    if (opc == 0xFF) {
        if (op_off + 1 >= vm->total_mem_size) DOS_NT_EMU_FAIL;
        opc_modrm = vm->mem[op_off + 1];
        uint8_t reg = (opc_modrm >> 3) & 7;
        if (reg == 5) opc_is_ff_5 = 1;
        else if (reg == 3) opc_is_ff_3 = 1;
        else DOS_NT_EMU_FAIL;
    } else if (opc == 0x8E) {
        if (op_off + 1 >= vm->total_mem_size) DOS_NT_EMU_FAIL;
        opc_modrm = vm->mem[op_off + 1];
        opc_is_mov_seg = 1;
    } else if (opc == 0xC4 || opc == 0xC5) {
        if (op_off + 1 >= vm->total_mem_size) DOS_NT_EMU_FAIL;
        opc_modrm = vm->mem[op_off + 1];
        if (opc == 0xC4) opc_is_les = 1; else opc_is_lds = 1;
    } else if (opc != 0xCB && opc != 0xCA && opc != 0xCF && opc != 0xEA) {
        DOS_NT_EMU_FAIL;
    }

    uint16_t pop_imm  = 0;
    uint16_t new_ip   = 0;
    uint16_t new_cs   = 0;
    uint32_t new_eip  = 0;
    uint32_t insn_len = 1;
    int uses_stack    = 1;      /* LRETW/IRET pop from stack */
    int is_iretd      = 0;
    int pushes_retaddr = 0;     /* CALL FAR variants */

    if (opc_is_mov_seg || opc_is_les || opc_is_lds) {
        /* Compute instruction length: prefixes + opcode + modrm + addr. */
        uint8_t mod = (opc_modrm >> 6) & 3;
        uint8_t rm  = opc_modrm & 7;
        uint32_t len = prefix_bytes + 1 /*opcode*/ + 1 /*modrm*/;
        if (mod == 0) {
            if (rm == 6) len += 2;
        } else if (mod == 1) {
            len += 1;
        } else if (mod == 2) {
            len += 2;
        }
        /* Select target segreg:
         *   MOV Sreg: ModR/M reg field (0=ES,3=DS,4=FS,5=GS).
         *   LES: ES.  LDS: DS. */
        uint8_t sreg = opc_is_mov_seg ? ((opc_modrm >> 3) & 7)
                     : opc_is_les     ? 0
                     :                  3; /* lds */
        /* Strategy: treat the failing selector value as a real-mode
         * segment number and synthesize a 64KB data descriptor whose
         * base is (sel * 16). DOS4GW often passes raw RM-style segment
         * values to PM-mode segreg loads when bridging through DPMI
         * functions (especially during relocation walks). Aliasing all
         * loads to a single fixed sel kept DOOM stuck in a tight loop
         * because every cmp es:[bx] dereferenced the same place; with
         * a per-selector base the dereferences hit different memory
         * and the loop can actually terminate.
         *
         * The base is clamped into [0, total_mem - 0x10000] so we never
         * generate a descriptor pointing past vm->mem. We use kernel
         * GDT slot 14 as a single rolling scratch — only one segreg can
         * be in flight per fault, so reuse is safe across faults. */
        uint16_t bad_sel = (uint16_t)f->error_code;
        uint32_t synth_base = (uint32_t)bad_sel * 16u;
        if (synth_base + 0x10000u > vm->total_mem_size)
            synth_base = vm->total_mem_size > 0x10000u
                       ? vm->total_mem_size - 0x10000u : 0;
        extern uint64_t kernel_gdt[];
        const int SCRATCH_SLOT = 14;
        uint64_t scratch_desc =
              ((uint64_t)0xFFFF)                              /* limit[15:0]   */
            | ((uint64_t)(synth_base & 0xFFFF) << 16)         /* base[15:0]    */
            | ((uint64_t)((synth_base >> 16) & 0xFF) << 32)   /* base[23:16]   */
            | ((uint64_t)0x92 << 40)                          /* P|DPL|S|type=2*/
            | ((uint64_t)0x00 << 52)                          /* flags+limit hi*/
            | ((uint64_t)((synth_base >> 24) & 0xFF) << 56);  /* base[31:24]   */
        kernel_gdt[SCRATCH_SLOT] = scratch_desc;
        uint16_t safe_sel = (uint16_t)(SCRATCH_SLOT << 3);
        /* If the synth base was clamped to 0 and the original sel was
         * also nonsensical, fall back to current DS so we at least have
         * a writable segment. */
        if (synth_base == 0 && bad_sel != 0) {
            uint16_t cpu_ds = (uint16_t)vm->cpu->ds;
            if (cpu_ds & 0x04) safe_sel = cpu_ds;
        }
        /* Load the target segment register NOW. iretq won't restore
         * ES/DS/FS/GS in same-CPL transitions, so this sticks. */
        switch (sreg) {
            case 0: __asm__ volatile ("movw %0, %%es" :: "r"(safe_sel)); break;
            case 3: __asm__ volatile ("movw %0, %%ds" :: "r"(safe_sel)); break;
            case 4: __asm__ volatile ("movw %0, %%fs" :: "r"(safe_sel)); break;
            case 5: __asm__ volatile ("movw %0, %%gs" :: "r"(safe_sel)); break;
            default: break;
        }
        f->rip += len;
        if (verbose) {
            serial_puts("[DOS-NT] emu ");
            serial_puts(opc_is_les ? "0xC4(LES)" :
                        opc_is_lds ? "0xC5(LDS)" : "0x8E(MOV)");
            serial_puts(" sreg="); serial_putdec(sreg);
            serial_puts(" <- 0x");  serial_puthex(safe_sel, 4);
            serial_puts(" (err=0x"); serial_puthex((uint64_t)f->error_code, 4);
            serial_puts(") skip len="); serial_putdec(len); serial_puts("\n");
        }
        if (kcr3 && saved_cr3 != kcr3)
            __asm__ volatile ("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
        return 1;
    }

    if (opc_is_ff_5 || opc_is_ff_3) {
        /* FF /5 or /3 with ModR/M. Only handle mod=00 rm=6 (disp16) for now
         * — the common DOS-tables pattern. Bail on other addressing modes. */
        uint8_t mod = (opc_modrm >> 6) & 3;
        uint8_t rm  = opc_modrm & 7;
        if (mod != 0 || rm != 6) DOS_NT_EMU_FAIL;
        if (op_off + 3 >= vm->total_mem_size) DOS_NT_EMU_FAIL;
        uint16_t disp16 = (uint16_t)vm->mem[op_off + 2]
                        | ((uint16_t)vm->mem[op_off + 3] << 8);
        /* Effective address is DS:disp16. Read 4 bytes: IP (2) + CS (2).
         * DS isn't in the iret frame; grab it from the CPU (isr_common
         * doesn't clobber DS before calling us). Fall back to vm->cpu->ds
         * if the current DS doesn't look like a DOOM LDT selector. */
        uint16_t ds_sel;
        __asm__ volatile ("mov %%ds, %0" : "=r"(ds_sel));
        if ((ds_sel & 0x04) == 0) ds_sel = (uint16_t)vm->cpu->ds;
        uint32_t ds_base = dos_nt_ldt_base(vm, ds_sel);
        uint32_t ea = ds_base + disp16;
        if (opc_prefix == 0x66) {
            /* 32-bit operand: read EIP (4) + CS (2) = 6 bytes at ea. */
            if (ea + 5 >= vm->total_mem_size) DOS_NT_EMU_FAIL;
            new_eip = (uint32_t)vm->mem[ea]
                    | ((uint32_t)vm->mem[ea+1] << 8)
                    | ((uint32_t)vm->mem[ea+2] << 16)
                    | ((uint32_t)vm->mem[ea+3] << 24);
            new_cs  = (uint16_t)vm->mem[ea+4] | ((uint16_t)vm->mem[ea+5] << 8);
        } else {
            if (ea + 3 >= vm->total_mem_size) DOS_NT_EMU_FAIL;
            new_ip = (uint16_t)vm->mem[ea]     | ((uint16_t)vm->mem[ea+1] << 8);
            new_cs = (uint16_t)vm->mem[ea + 2] | ((uint16_t)vm->mem[ea+3] << 8);
        }
        insn_len = prefix_bytes + 1 + 1 + 2; /* prefixes + opcode + modrm + disp16 */
        uses_stack = 0;
        if (opc_is_ff_3) {
            /* CALL FAR: push current CS:IP of the byte AFTER this insn.
             * 16-bit stack: PUSH CS (2B), then PUSH IP (2B). SP -= 4. */
            uint16_t ret_ip = (uint16_t)((uint32_t)f->rip + insn_len);
            uint16_t ret_cs = (uint16_t)f->cs;
            uint32_t ss_base0 = dos_nt_ldt_base(vm, (uint16_t)f->ss);
            uint32_t sp_off0  = (uint32_t)(f->rsp & 0xFFFF);
            uint32_t new_sp_off = (sp_off0 - 4) & 0xFFFF;
            uint32_t stk0 = ss_base0 + new_sp_off;
            if (stk0 + 3 < vm->total_mem_size) {
                vm->mem[stk0]     = (uint8_t)(ret_ip);
                vm->mem[stk0 + 1] = (uint8_t)(ret_ip >> 8);
                vm->mem[stk0 + 2] = (uint8_t)(ret_cs);
                vm->mem[stk0 + 3] = (uint8_t)(ret_cs >> 8);
            }
            f->rsp = (f->rsp & ~0xFFFFULL) | new_sp_off;
            pushes_retaddr = 1;
        }
    } else if (opc == 0xCA) {
        if (op_off + 2 >= vm->total_mem_size) DOS_NT_EMU_FAIL;
        pop_imm = (uint16_t)vm->mem[op_off + 1]
                | ((uint16_t)vm->mem[op_off + 2] << 8);
        insn_len = prefix_bytes + 3;
    } else if (opc == 0xEA) {
        /* JMP FAR imm16:imm16 (or imm32:imm16 w/ 66h). */
        if (opc_prefix == 0x66) {
            if (op_off + 6 >= vm->total_mem_size) DOS_NT_EMU_FAIL;
            new_eip = (uint32_t)vm->mem[op_off + 1]
                    | ((uint32_t)vm->mem[op_off + 2] << 8)
                    | ((uint32_t)vm->mem[op_off + 3] << 16)
                    | ((uint32_t)vm->mem[op_off + 4] << 24);
            new_cs  = (uint16_t)vm->mem[op_off + 5]
                    | ((uint16_t)vm->mem[op_off + 6] << 8);
            insn_len = prefix_bytes + 7;
        } else {
            if (op_off + 4 >= vm->total_mem_size) DOS_NT_EMU_FAIL;
            new_ip  = (uint16_t)vm->mem[op_off + 1]
                    | ((uint16_t)vm->mem[op_off + 2] << 8);
            new_cs  = (uint16_t)vm->mem[op_off + 3]
                    | ((uint16_t)vm->mem[op_off + 4] << 8);
            insn_len = prefix_bytes + 5;
        }
        uses_stack = 0;
    } else if (opc == 0xCF && opc_prefix == 0x66) {
        is_iretd = 1;
        insn_len = prefix_bytes + 1;
    } else if (opc == 0xCF) {
        insn_len = prefix_bytes + 1;
    } else if (opc == 0xCB) {
        insn_len = prefix_bytes + 1;
    }

    /* Read IP:CS (:FLAGS) from DOS stack if this opcode pops. */
    uint32_t ss_base = dos_nt_ldt_base(vm, (uint16_t)f->ss);
    uint32_t sp_off  = (uint32_t)(f->rsp & 0xFFFF);
    uint32_t stk     = ss_base + sp_off;
    uint32_t stack_advance = 0;
    if (uses_stack) {
        if (is_iretd) {
            if (stk + 11 >= vm->total_mem_size) DOS_NT_EMU_FAIL;
            new_eip = (uint32_t)vm->mem[stk]
                    | ((uint32_t)vm->mem[stk+1] << 8)
                    | ((uint32_t)vm->mem[stk+2] << 16)
                    | ((uint32_t)vm->mem[stk+3] << 24);
            new_cs  = (uint16_t)vm->mem[stk+4]
                    | ((uint16_t)vm->mem[stk+5] << 8);
            /* Skip EFLAGS (bytes 8-11) */
            stack_advance = 12;
        } else if (opc == 0xCF) {
            if (stk + 5 >= vm->total_mem_size) DOS_NT_EMU_FAIL;
            new_ip = (uint16_t)vm->mem[stk]     | ((uint16_t)vm->mem[stk+1] << 8);
            new_cs = (uint16_t)vm->mem[stk + 2] | ((uint16_t)vm->mem[stk+3] << 8);
            stack_advance = 6;
        } else {
            if (stk + 3 >= vm->total_mem_size) DOS_NT_EMU_FAIL;
            new_ip = (uint16_t)vm->mem[stk]     | ((uint16_t)vm->mem[stk+1] << 8);
            new_cs = (uint16_t)vm->mem[stk + 2] | ((uint16_t)vm->mem[stk+3] << 8);
            stack_advance = 4 + pop_imm;
        }
    }
    (void)insn_len;

    /* Pick the effective offset: 32-bit for IRETD / 0x66 0xEA / 0x66 FF/5.
     * FF /3 CALL FAR uses whichever matches its operand size too. */
    int use_eip = is_iretd
               || (opc == 0xEA && opc_prefix == 0x66)
               || ((opc_is_ff_5 || opc_is_ff_3) && opc_prefix == 0x66);
    uint32_t eff_ip = use_eip ? new_eip : (uint32_t)new_ip;

    /* Compute target linear via the intended CS's base in DOOM's LDT.
     * Several conditions force a fallback to "stay in current CS":
     *   1. new_cs is a GDT selector (TI=0) — DOS user code shouldn't be
     *      jumping into kernel GDT entries; the offset is more likely a
     *      DOOM-relative one with a stale/wrong CS push.
     *   2. tgt_base resolves but the result lands BELOW current CS base —
     *      that always means the popped CS was bogus.
     *   3. tgt_base resolves but the result lands ABOVE vm->total_mem_size.
     * In all those cases, treat eff_ip as a current-CS relative offset
     * so DOOM stays in its own code segment.  This is the same DOS4GW
     * quirk the simpler "tgt_base == 0" branch already handled. */
    uint32_t tgt_base = dos_nt_ldt_base(vm, new_cs);
    int new_cs_is_gdt = (new_cs != 0) && ((new_cs & 0x04) == 0);
    int target_cs_unresolved = 0;
    if (new_cs != 0 && (tgt_base == 0 || new_cs_is_gdt)) {
        tgt_base = cs_base;
        target_cs_unresolved = 1;
    }
    uint32_t tgt_lin = tgt_base + eff_ip;
    if (tgt_lin < cs_base || tgt_lin >= vm->total_mem_size) {
        /* Last-chance: re-base on current CS. */
        tgt_lin = cs_base + eff_ip;
        target_cs_unresolved = 1;
        if (tgt_lin >= vm->total_mem_size) {
            serial_puts("[emu] FAIL tgt_lin=0x"); serial_puthex(tgt_lin, 8);
            serial_puts(" out-of-range new_cs=0x");
            serial_puthex(new_cs, 4);
            serial_puts(" eff_ip=0x");
            serial_puthex(eff_ip, 8);
            serial_puts("\n");
            DOS_NT_EMU_FAIL;
        }
    }
    uint64_t new_rip_in_current_cs = tgt_lin - cs_base;
    (void)target_cs_unresolved;

    /* Advance DOS SP by the amount the opcode would have popped. */
    if (uses_stack) {
        uint16_t new_sp = (uint16_t)(sp_off + stack_advance);
        f->rsp = (f->rsp & ~0xFFFFULL) | new_sp;
    }
    f->rip = new_rip_in_current_cs;

    serial_puts("[DOS-NT] emu opc=0x");
    serial_puthex(opc, 2);
    if (opc_is_ff_5) serial_puts("/5");
    if (opc_is_ff_3) serial_puts("/3");
    if (pushes_retaddr) serial_puts(" CALL");
    if (opc_prefix) { serial_puts(" pfx=0x"); serial_puthex(opc_prefix, 2); }
    serial_puts(" -> ");
    serial_puthex(new_cs, 4); serial_puts(":");
    serial_puthex(eff_ip, 8); serial_puts(" (linear 0x");
    serial_puthex(tgt_lin, 8); serial_puts(") RIP=0x");
    serial_puthex(new_rip_in_current_cs, 8);
    serial_puts("\n");
    /* Wrap the per-success log only when verbose so the segment-load
     * loops don't drown out the trace.  The verbose flag was set at the
     * top of this function. */

    /* Restore CR3 so the IRETQ resumes in DOS CR3. */
    if (kcr3 && saved_cr3 != kcr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
    return 1;
}

/* Called from the IDT [pf-ist] probe when a DOS native program faults.
 * Dumps the 16 bytes at CS:RIP plus the top of the caller's stack so
 * we can see what opcode faulted AND trace the CALL history. CR3 is
 * switched to kernel so vm->mem's PA identity-map is reachable. */
void dos_native_dump_rip(uint16_t cs, uint32_t rip, uint16_t ss_hint,
                         uint64_t frame_rsp)
{
    (void)ss_hint; (void)frame_rsp;
    if (!g_native_dos_vm) return;
    /* Rate-limit: same RIP back-to-back gets sampled instead of dumped
     * every time. The pf-ist line itself still prints unconditionally
     * (idt.c handles that); we only suppress this 32-byte dump here. */
    static uint32_t dump_calls = 0;
    static uint32_t last_rip   = 0xFFFFFFFFu;
    if (rip == last_rip && (++dump_calls & 0x3FF) != 0) return;
    last_rip = rip;
    dump_calls = 0;
    extern uint64_t paging_get_kernel_cr3(void);
    uint64_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    uint64_t kcr3 = paging_get_kernel_cr3();
    if (kcr3 && saved_cr3 != kcr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(kcr3) : "memory");

    dos_vm_t *vm = g_native_dos_vm;
    uint16_t idx = (cs >> 3) & 0x1FFF;
    if (idx < DPMI_MAX_DESCRIPTORS) {
        dpmi_descriptor_t *d = &vm->dpmi.ldt[idx];
        uint32_t base = (uint32_t)d->base_lo
                      | ((uint32_t)d->base_mid << 16)
                      | ((uint32_t)d->base_hi  << 24);
        uint64_t linear = (uint64_t)base + rip;
        serial_puts("[pf-ist] linear=0x");
        serial_puthex(linear, 8);
        serial_puts(" bytes:");
        for (int i = 0; i < 16 &&
             (linear + i) < vm->total_mem_size; i++) {
            serial_puts(" ");
            serial_puthex(vm->mem[linear + i], 2);
        }
        serial_puts("\n");

        /* Stack dump: the iret frame has the DOS SS:RSP where DOOM was
         * pushing. Read up to 16 bytes from it to see the caller's
         * return address pushed by CALL FAR. We can't access the iret
         * frame from here without more plumbing, so use cpu8086's
         * stored state as a fallback (pre-transfer cpu->esp). */
        uint16_t ss_sel = vm->cpu->ss & ~3;
        uint16_t ss_idx = (ss_sel >> 3) & 0x1FFF;
        if (ss_idx < DPMI_MAX_DESCRIPTORS) {
            dpmi_descriptor_t *sd = &vm->dpmi.ldt[ss_idx];
            uint32_t ss_base = (uint32_t)sd->base_lo
                             | ((uint32_t)sd->base_mid << 16)
                             | ((uint32_t)sd->base_hi  << 24);
            /* Use the iret-frame RSP (passed as frame_rsp) so we see
             * the actual stack state at fault time, not the snapshot
             * from before the native transfer. */
            uint32_t rt_esp = (uint32_t)(frame_rsp & 0xFFFFFFFF);
            uint64_t sp_linear = (uint64_t)ss_base + rt_esp;
            serial_puts("[pf-ist] ss_base=0x");
            serial_puthex(ss_base, 8);
            serial_puts(" rt_esp=0x");
            serial_puthex(rt_esp, 8);
            serial_puts(" stack[0..15]:");
            for (int i = 0; i < 16 &&
                 (sp_linear + i) < vm->total_mem_size; i++) {
                serial_puts(" ");
                serial_puthex(vm->mem[sp_linear + i], 2);
            }
            serial_puts("\n");
        }
    }

    if (kcr3 && saved_cr3 != kcr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
}

void dos_int_native_dispatch(uint64_t int_num, dos_native_regs_t *regs)
{
#ifdef COMPAT_TRACE
    /* Panorama probe: mirrors the [int2e] probe for DOS native INT path.
     * Silent on -smp 1; any line with cpu != 0 confirms risk #1. */
    {
        volatile uint32_t *_apic_id =
            (volatile uint32_t *)(uintptr_t)(0xFFFF800000000000ULL + 0xFEE00020ULL);
        uint32_t _cpu = (*_apic_id >> 24) & 0xFF;
        if (_cpu != 0) {
            serial_puts("[dos-int] cpu=");
            serial_putdec(_cpu);
            serial_puts(" vec=0x");
            serial_puthex(int_num, 2);
            serial_puts("\n");
        }
    }
#endif

    /* Switch to kernel CR3 so handlers can access vm->mem via its PA
     * (identity-mapped <4 GB in kernel CR3) and any other kernel-side
     * structures that aren't mapped in the DOS CR3. Restored before return. */
    extern uint64_t paging_get_kernel_cr3(void);
    uint64_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    uint64_t kcr3 = paging_get_kernel_cr3();
    if (kcr3 && saved_cr3 != kcr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kcr3) : "memory");
    }

    if (!g_native_dos_vm) {
        if (kcr3 && saved_cr3 != kcr3)
            __asm__ volatile ("mov %0, %%cr3" : : "r"(saved_cr3) : "memory");
        return;
    }

    dos_vm_t *vm = g_native_dos_vm;
    cpu8086_state_t *cpu = &g_native_cpu;
    vm->cpu = cpu;

    /* Copy native registers → emulated CPU state for DOS handlers */
    cpu->eax = (uint32_t)regs->rax;
    cpu->ebx = (uint32_t)regs->rbx;
    cpu->ecx = (uint32_t)regs->rcx;
    cpu->edx = (uint32_t)regs->rdx;
    cpu->esi = (uint32_t)regs->rsi;
    cpu->edi = (uint32_t)regs->rdi;
    cpu->ebp = (uint32_t)regs->rbp;
    cpu->ds  = (uint16_t)regs->ds;
    cpu->es  = (uint16_t)regs->es;
    cpu->flags = 0x0202;  /* IF=1, fixed bits */
    cpu->running = true;
    cpu->protected_mode = true;
    cpu->vm = vm;

    /* Log INTs — first 50 verbose, then every 256th to keep noise down */
    static uint32_t native_int_count = 0;
    native_int_count++;
    if (native_int_count < 50 || (native_int_count & 0xFF) == 0) {
        serial_puts("[DOS32] INT ");
        serial_puthex(int_num, 2);
        serial_puts("h AH=");
        serial_puthex(cpu->ah, 2);
        serial_puts(" #"); serial_putdec(native_int_count);
        serial_puts("\n");
    }

    /* Dispatch to existing handlers (dos_api.c, dos_bios.c, etc.) */
    dos_int_dispatch(vm, (uint8_t)int_num);

    /* Copy results back → native registers */
    regs->rax = cpu->eax;
    regs->rbx = cpu->ebx;
    regs->rcx = cpu->ecx;
    regs->rdx = cpu->edx;
    regs->rsi = cpu->esi;
    regs->rdi = cpu->edi;
    regs->rbp = cpu->ebp;
    regs->ds  = cpu->ds;
    regs->es  = cpu->es;

    /* Force IF=1 and clear TF in the saved RFLAGS that IRETQ will pop.
     * DOOM does CLI/STI sequences and POPF runs that occasionally leave
     * the flags with IF=0; without external IRQ delivery the program
     * gets stuck in a busy-poll loop with no way to make progress. The
     * 0x202 base (IF=1 + reserved bit) plus IOPL=3 keeps DOOM able to
     * issue IN/OUT freely. Preserve other flags (CF/ZF/SF/etc) so DOS
     * function results are visible to the caller. */
    regs->iret_rflags = (regs->iret_rflags & ~(uint64_t)0x100ULL) /* clear TF */
                      | 0x200ULL  /* IF=1 */
                      | 0x3000ULL /* IOPL=3 */
                      | 0x002ULL; /* reserved bit 1 */

    /* Handle terminate (INT 20h or INT 21h/4Ch) */
    if (!cpu->running) {
        serial_puts("[DOS32] Program terminated, exit code ");
        serial_puthex(cpu->exit_code, 2);
        serial_puts("\n");
        /* Halt forever — return-to-shell via setjmp is a separate task. */
        __asm__ volatile ("cli\nhlt\n");
    }

    /* Restore DOS CR3 before returning to ring-3 DOS code. */
    if (kcr3 && saved_cr3 != kcr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(saved_cr3) : "memory");
    }
}
