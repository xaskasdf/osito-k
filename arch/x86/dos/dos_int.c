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

    /* Log non-trivial INTs */
    static uint32_t native_int_count = 0;
    if (native_int_count < 50) {
        native_int_count++;
        serial_puts("[DOS32] INT ");
        serial_puthex(int_num, 2);
        serial_puts("h AH=");
        serial_puthex(cpu->ah, 2);
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
