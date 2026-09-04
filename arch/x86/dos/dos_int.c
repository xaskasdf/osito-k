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
#include "dos_hostmem.h"
#include "dos_audio.h"
#include "dos_io.h"
#include "dos_mouse.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);

/* Forward declarations for service handlers */
void dos_int21_dispatch(dos_vm_t *vm);
void dos_int10_video(dos_vm_t *vm);
void dos_int16_keyboard(dos_vm_t *vm);
void dos_int1a_timer(dos_vm_t *vm);
void dos_int2f_dispatch(dos_vm_t *vm);
void dos_int31_dpmi(dos_vm_t *vm);
void dpmi_enter_protected_mode(dos_vm_t *vm);
void dos_int67_dispatch(dos_vm_t *vm);

static uint16_t dos_u16_clamp(uint32_t value)
{
    return value > 0xFFFFu ? 0xFFFFu : (uint16_t)value;
}

bool dos_int_has_pm_translator(uint8_t int_num)
{
    switch (int_num) {
    case 0x10:
    case 0x15:
    case 0x16:
    case 0x1A:
    case 0x20:
    case 0x21:
    case 0x2F:
    case 0x31:
    case 0x33:
    case 0x67:
        return true;
    default:
        return false;
    }
}

static bool dos_dpmi_pm_stub_source(const dos_vm_t *vm,
                                    uint32_t stub_offset)
{
    const cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    return cpu && cpu->protected_mode && vm->dpmi.active &&
           cpu->cs == vm->dpmi.sel_host_code &&
           cpu->eip == stub_offset + 2u;
}

static bool dos_dpmi_pm_reflect_source(const dos_vm_t *vm,
                                       uint8_t *reflected_int)
{
    const cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !cpu->protected_mode || !vm->dpmi.active ||
        cpu->cs != vm->dpmi.sel_host_code)
        return false;

    uint32_t software_first = DPMI_PM_REFLECT_BASE_OFF + 2u;
    uint32_t software_last = software_first +
                             255u * DPMI_PM_REFLECT_STUB_SIZE;
    uint32_t hardware_first = DPMI_PM_HW_REFLECT_BASE_OFF + 2u;
    uint32_t hardware_last = hardware_first +
                             255u * DPMI_PM_REFLECT_STUB_SIZE;
    uint32_t relative;
    if (cpu->eip >= software_first && cpu->eip <= software_last)
        relative = cpu->eip - software_first;
    else if (cpu->eip >= hardware_first && cpu->eip <= hardware_last)
        relative = cpu->eip - hardware_first;
    else
        return false;

    if (relative % DPMI_PM_REFLECT_STUB_SIZE)
        return false;
    if (reflected_int)
        *reflected_int = (uint8_t)(relative / DPMI_PM_REFLECT_STUB_SIZE);
    return true;
}

static bool dos_dpmi_pm_private_source(const dos_vm_t *vm, uint8_t int_num)
{
    if (int_num == DPMI_DEFAULT_REFLECT_INT)
        return dos_dpmi_pm_reflect_source(vm, NULL);
    if (int_num == DPMI_CALLBACK_RETURN_INT)
        return dos_dpmi_pm_stub_source(vm, DPMI_CALLBACK_RETURN_OFF);
    if (int_num == DPMI_RAW_SWITCH_INT)
        return dos_dpmi_pm_stub_source(vm, DPMI_RAW_SWITCH_OFF);
    if (int_num == DPMI_EXCEPTION_RETURN_INT)
        return dos_dpmi_pm_stub_source(vm, DPMI_EXCEPTION_RETURN_OFF);
    return false;
}

static bool dos_dpmi_rm_stub_source(const dos_vm_t *vm,
                                    uint32_t stub_offset)
{
    const cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    return cpu && !cpu->protected_mode && cpu->cs == DPMI_ENTRY_SEG &&
           cpu->eip == stub_offset + 2u;
}

static bool dos_dpmi_rm_callback_source(const dos_vm_t *vm)
{
    const cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    uint32_t first_return = DPMI_CALLBACK_BASE_OFF + 2u;
    uint32_t last_return = first_return +
                           (DPMI_MAX_CALLBACKS - 1u) *
                           DPMI_CALLBACK_STUB_SIZE;
    if (!cpu || cpu->protected_mode || cpu->cs != DPMI_ENTRY_SEG ||
        cpu->eip < first_return || cpu->eip > last_return)
        return false;
    return (cpu->eip - first_return) % DPMI_CALLBACK_STUB_SIZE == 0;
}

static void dos_int15_e801(cpu8086_state_t *cpu, uint32_t total_bytes)
{
    const uint32_t one_mb = 1u << 20;
    const uint32_t sixteen_mb = 16u << 20;
    uint32_t below_16m_kb = 0;
    uint32_t above_16m_blocks = 0;

    if (total_bytes > one_mb) {
        uint32_t below_16m_end = total_bytes < sixteen_mb ?
                                 total_bytes : sixteen_mb;
        below_16m_kb = (below_16m_end - one_mb) >> 10;
    }
    if (total_bytes > sixteen_mb)
        above_16m_blocks = (total_bytes - sixteen_mb) >> 16;

    cpu->ax = dos_u16_clamp(below_16m_kb);
    cpu->bx = dos_u16_clamp(above_16m_blocks);
    cpu->cx = cpu->ax;
    cpu->dx = cpu->bx;
    cpu->flags &= ~FLAG_CF;
}

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
        case 0x88: { /* Get extended memory size (KB above 1MB) */
            uint32_t system_memory = vm->system_mem_size
                                   ? vm->system_mem_size : vm->total_mem_size;
            c15->ax = (system_memory > 0x100000) ?
                      (uint16_t)((system_memory - 0x100000) / 1024) : 0;
            c15->flags &= ~FLAG_CF;
            break;
        }
        case 0xE8: /* Get memory map (E820h) */
            if (c15->al == 0x01) {
                uint32_t system_memory = vm->system_mem_size
                                       ? vm->system_mem_size
                                       : vm->total_mem_size;
                dos_int15_e801(c15, system_memory);
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
        vm->termination_type = 0;
        vm->process_terminated = true;
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
        dos_int33_mouse(vm);
        break;

    case 0x67:
        dos_int67_dispatch(vm);
        break;

    case DPMI_DEFAULT_REFLECT_INT: {
        uint8_t reflected_int;
        if (!dos_dpmi_pm_reflect_source(vm, &reflected_int))
            goto generic_interrupt;
        if (!dpmi_dispatch_default_interrupt(
                vm, reflected_int, vm->software_int_frame_bytes)) {
            serial_puts("[DPMI] Default interrupt reflection failed\n");
            vm->cpu->running = false;
            vm->cpu->exit_code = -1;
        }
        break;
    }

    case DPMI_CALLBACK_ENTRY_INT:
        if (!dos_dpmi_rm_callback_source(vm))
            goto generic_interrupt;
        if (!dpmi_callback_enter(vm)) {
            serial_puts("[DPMI] Invalid real-mode callback entry\n");
            vm->cpu->running = false;
            vm->cpu->exit_code = -1;
        }
        break;

    case DPMI_CALLBACK_RETURN_INT:
        if (!dos_dpmi_pm_stub_source(vm, DPMI_CALLBACK_RETURN_OFF))
            goto generic_interrupt;
        if (!dpmi_callback_return(vm, true)) {
            serial_puts("[DPMI] Invalid callback return\n");
            vm->cpu->running = false;
            vm->cpu->exit_code = -1;
        }
        break;

    case DPMI_EXCEPTION_RETURN_INT:
        if (!dos_dpmi_pm_stub_source(vm, DPMI_EXCEPTION_RETURN_OFF))
            goto generic_interrupt;
        if (!dpmi_exception_return(vm)) {
            vm->cpu->running = false;
            vm->cpu->exit_code = -1;
        }
        break;

    case DPMI_RAW_SWITCH_INT:
        if (!dos_dpmi_pm_stub_source(vm, DPMI_RAW_SWITCH_OFF) &&
            !dos_dpmi_rm_stub_source(vm, DPMI_RAW_SWITCH_OFF))
            goto generic_interrupt;
        if (!dpmi_raw_mode_switch(vm)) {
            vm->cpu->running = false;
            vm->cpu->exit_code = -1;
        }
        break;

    case DPMI_ENTRY_INT:
        /* DPMI entry trigger: switch to protected mode */
        if (!dos_dpmi_rm_stub_source(vm, DPMI_ENTRY_OFF))
            goto generic_interrupt;
        dpmi_enter_protected_mode(vm);
        break;

    default:
generic_interrupt: {
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

int dos_bios_memory_selftest(void)
{
    dos_vm_t vm = {0};
    cpu8086_state_t cpu = {0};
    int failures = 0;

    vm.cpu = &cpu;
    cpu.vm = &vm;

    vm.total_mem_size = 8u << 20;
    cpu.ax = 0xE801;
    cpu.flags = FLAG_CF;
    dos_int_dispatch(&vm, 0x15);
    if ((cpu.flags & FLAG_CF) || cpu.ax != 7u * 1024u ||
        cpu.cx != cpu.ax || cpu.bx != 0 || cpu.dx != 0)
        failures++;

    vm.total_mem_size = 16u << 20;
    cpu.ax = 0xE801;
    cpu.flags = FLAG_CF;
    dos_int_dispatch(&vm, 0x15);
    if ((cpu.flags & FLAG_CF) || cpu.ax != 0x3C00 ||
        cpu.cx != 0x3C00 || cpu.bx != 0 || cpu.dx != 0)
        failures++;

    vm.total_mem_size = 48u << 20;
    cpu.ax = 0xE801;
    cpu.flags = FLAG_CF;
    dos_int_dispatch(&vm, 0x15);
    if ((cpu.flags & FLAG_CF) || cpu.ax != 0x3C00 ||
        cpu.cx != 0x3C00 || cpu.bx != 512 || cpu.dx != 512)
        failures++;

    cpu.ax = 0xBF00;
    cpu.flags = 0;
    dos_int_dispatch(&vm, 0x15);
    if (!(cpu.flags & FLAG_CF)) failures++;

    return failures;
}

/* ── Hardware interrupt delivery (timer, etc.) ──────────────────── */

static bool cpu_deliver_pm_idt_gate(dos_vm_t *vm, uint8_t vector,
                                    uint32_t return_eip,
                                    uint32_t error_code,
                                    bool has_error_code)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t gate_offset = (uint32_t)vector * 8u;
    if (!cpu->idtr.base || gate_offset + 7u > cpu->idtr.limit)
        return false;

    uint32_t idt_linear = cpu->idtr.base + gate_offset;
    uint32_t entry = dpmi_translate(vm, 0, idt_linear);
    if (vm->total_mem_size < 8u || entry > vm->total_mem_size - 8u)
        return false;

    uint16_t off_lo = dos_mem_read16(vm, entry);
    uint16_t sel = dos_mem_read16(vm, entry + 2u);
    uint8_t attr = dos_mem_read8(vm, entry + 5u);
    uint16_t off_hi = dos_mem_read16(vm, entry + 6u);
    uint8_t gate_type = attr & 0x1Fu;
    bool gate32 = gate_type == 0x0E || gate_type == 0x0F;
    bool gate16 = gate_type == 0x06 || gate_type == 0x07;
    if (!(attr & 0x80) || (!gate16 && !gate32) || !sel)
        return false;

    if (gate32) {
        cpu_push32(cpu, cpu->eflags);
        cpu_push32(cpu, (uint32_t)cpu->cs);
        cpu_push32(cpu, return_eip);
        if (has_error_code) cpu_push32(cpu, error_code);
        cpu->eip = ((uint32_t)off_hi << 16) | off_lo;
    } else {
        cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
        cpu_push16(cpu, cpu->cs);
        cpu_push16(cpu, (uint16_t)return_eip);
        if (has_error_code) cpu_push16(cpu, (uint16_t)error_code);
        cpu->eip = off_lo;
    }
    cpu->cs = sel;
    cpu->flags &= ~FLAG_TF;
    if (gate_type == 0x06 || gate_type == 0x0E)
        cpu->flags &= ~FLAG_IF;
    cpu8086_sync_cs(cpu);
    return true;
}

static bool cpu_deliver_rm_vector(dos_vm_t *vm, uint8_t vector,
                                  uint32_t return_eip)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t ivt_addr = (uint32_t)vector * 4u;
    uint16_t off = dos_mem_read16(vm, ivt_addr);
    uint16_t seg = dos_mem_read16(vm, ivt_addr + 2u);

    /* Zero vectors and initialized ROM IRET stubs are host-owned vectors. */
    if ((!seg && !off) || seg >= (DOS_ROM_BASE >> 4))
        return true;

    cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
    cpu_push16(cpu, cpu->cs);
    cpu_push16(cpu, (uint16_t)return_eip);
    cpu->flags &= ~(FLAG_IF | FLAG_TF);
    cpu->cs = seg;
    cpu->ip = off;
    return true;
}

static bool dpmi_deliver_exception(dos_vm_t *vm, uint8_t vector,
                                   uint32_t return_eip,
                                   uint32_t error_code)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint16_t handler_sel = vm->dpmi.exception_vectors[vector].sel;
    if (!handler_sel) return false;

    uint16_t host_sel = dpmi_get_host_code_selector(vm);
    if (!host_sel ||
        vm->dpmi.exception_depth >= DPMI_MAX_EXCEPTION_DEPTH)
        return false;

    uint8_t exception_depth = vm->dpmi.exception_depth;
    vm->dpmi.exception_virtual_interrupts[exception_depth] =
        vm->dpmi.virtual_interrupts_enabled;
    vm->dpmi.virtual_interrupts_enabled = false;

    uint16_t old_ss = cpu->ss;
    uint32_t old_esp = cpu->esp;
    uint16_t old_cs = cpu->cs;
    uint32_t old_flags = cpu->eflags | FLAGS_FIXED;

    if (vm->dpmi.is_32bit) {
        cpu_push32(cpu, old_ss);
        cpu_push32(cpu, old_esp);
        cpu_push32(cpu, old_flags);
        cpu_push32(cpu, old_cs);
        cpu_push32(cpu, return_eip);
        cpu_push32(cpu, error_code);
        cpu_push32(cpu, host_sel);
        cpu_push32(cpu, DPMI_EXCEPTION_RETURN_OFF);
    } else {
        cpu_push16(cpu, old_ss);
        cpu_push16(cpu, (uint16_t)old_esp);
        cpu_push16(cpu, (uint16_t)old_flags);
        cpu_push16(cpu, old_cs);
        cpu_push16(cpu, (uint16_t)return_eip);
        cpu_push16(cpu, (uint16_t)error_code);
        cpu_push16(cpu, host_sel);
        cpu_push16(cpu, DPMI_EXCEPTION_RETURN_OFF);
    }

    vm->dpmi.exception_depth = exception_depth + 1u;
    cpu->cs = handler_sel;
    cpu->eip = vm->dpmi.exception_vectors[vector].off;
    cpu->flags = (cpu->flags | FLAG_IF) & ~FLAG_TF;
    cpu8086_sync_cs(cpu);
    return true;
}

static bool dpmi_exception_return_frame(dos_vm_t *vm,
                                        bool discard_private_int_frame)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !cpu->protected_mode || !vm->dpmi.exception_depth ||
        cpu->cs != vm->dpmi.sel_host_code ||
        cpu->eip != DPMI_EXCEPTION_RETURN_OFF + 2u) {
        serial_puts("[DPMI] Invalid exception return\n");
        return false;
    }

    uint32_t frame_size;
    if (vm->dpmi.is_32bit)
        frame_size = discard_private_int_frame ? 36u : 24u;
    else
        frame_size = discard_private_int_frame ? 18u : 12u;
    uint32_t stack = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
    if (stack > vm->total_mem_size ||
        frame_size > vm->total_mem_size - stack) {
        serial_puts("[DPMI] Truncated exception return frame\n");
        return false;
    }

    uint16_t restored_ss;
    uint32_t restored_esp;
    uint16_t restored_cs;
    uint32_t restored_eip;
    uint32_t restored_flags;
    if (vm->dpmi.is_32bit) {
        if (discard_private_int_frame)
            cpu_stack_adjust(cpu, 12); /* interpreted INT FD frame */
        (void)cpu_pop32(cpu);        /* exception error code */
        restored_eip = cpu_pop32(cpu);
        restored_cs = (uint16_t)cpu_pop32(cpu);
        restored_flags = cpu_pop32(cpu);
        restored_esp = cpu_pop32(cpu);
        restored_ss = (uint16_t)cpu_pop32(cpu);
    } else {
        if (discard_private_int_frame)
            cpu_stack_adjust(cpu, 6); /* interpreted INT FD frame */
        (void)cpu_pop16(cpu);        /* exception error code */
        restored_eip = cpu_pop16(cpu);
        restored_cs = cpu_pop16(cpu);
        restored_flags = cpu_pop16(cpu);
        restored_esp = cpu_pop16(cpu);
        restored_ss = cpu_pop16(cpu);
    }

    cpu->ss = restored_ss;
    cpu->esp = restored_esp;
    cpu->cs = restored_cs;
    cpu->eip = restored_eip;
    cpu->eflags = (restored_flags & 0x003FFFFFu) |
                  FLAGS_FIXED | FLAG_IF;
    cpu->halted = false;
    vm->dpmi.exception_depth--;
    vm->dpmi.virtual_interrupts_enabled =
        vm->dpmi.exception_virtual_interrupts[vm->dpmi.exception_depth];
    cpu8086_sync_cs(cpu);
    return true;
}

bool dpmi_exception_return(dos_vm_t *vm)
{
    return dpmi_exception_return_frame(vm, true);
}

static bool dpmi_exception_return_native(dos_vm_t *vm)
{
    /* Native INT FD switches to IST2, so its CPU IRET frame is not part of
     * the DPMI client stack that begins at the exception error-code field. */
    return dpmi_exception_return_frame(vm, false);
}

bool cpu_deliver_pm_software_interrupt(dos_vm_t *vm, uint8_t int_num,
                                       uint32_t return_eip)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !cpu->protected_mode || !vm->dpmi.active ||
        dos_dpmi_pm_private_source(vm, int_num))
        return false;

    uint16_t handler_sel = vm->dpmi.pm_vectors[int_num].sel;
    uint32_t handler_off = vm->dpmi.pm_vectors[int_num].off;
    if (!handler_sel) {
        /* Keep built-in API translators on their direct fast path. Their
         * chainable 0204h address still enters through the default stub. */
        if (dos_int_has_pm_translator(int_num))
            return false;
        handler_sel = dpmi_get_host_code_selector(vm);
        handler_off = DPMI_PM_REFLECT_BASE_OFF +
                      int_num * DPMI_PM_REFLECT_STUB_SIZE;
        if (!handler_sel)
            return false;
    }

    if (vm->dpmi.is_32bit) {
        cpu_push32(cpu, cpu->eflags | FLAGS_FIXED | FLAG_IF);
        cpu_push32(cpu, (uint32_t)cpu->cs);
        cpu_push32(cpu, return_eip);
    } else {
        cpu_push16(cpu, cpu->flags | FLAGS_FIXED | FLAG_IF);
        cpu_push16(cpu, cpu->cs);
        cpu_push16(cpu, (uint16_t)return_eip);
    }

    cpu->cs = handler_sel;
    cpu->eip = handler_off;
    cpu->flags = (cpu->flags | FLAG_IF) & ~FLAG_TF;
    if (int_num <= 7u)
        vm->dpmi.virtual_interrupts_enabled = false;
    cpu8086_sync_cs(cpu);
    return true;
}

bool cpu_deliver_hw_interrupt(dos_vm_t *vm, uint8_t int_num)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu) return false;

    cpu->halted = false;
    if (!cpu->protected_mode) {
        if (!(cpu->flags & FLAG_IF)) return false;
        return cpu_deliver_rm_vector(vm, int_num, cpu->eip);
    }

    bool virtualized = vm->dpmi.active;
    if (virtualized) {
        if (!vm->dpmi.virtual_interrupts_enabled) return false;
        cpu->flags |= FLAG_IF;
    } else if (!(cpu->flags & FLAG_IF)) {
        return false;
    }

    uint16_t handler_sel = vm->dpmi.pm_vectors[int_num].sel;
    uint32_t handler_off = vm->dpmi.pm_vectors[int_num].off;
    if (!handler_sel) {
        handler_sel = dpmi_get_host_code_selector(vm);
        handler_off = DPMI_PM_HW_REFLECT_BASE_OFF +
                      int_num * DPMI_PM_REFLECT_STUB_SIZE;
    }

    if (handler_sel) {
        if (vm->dpmi.is_32bit) {
            cpu_push32(cpu, cpu->eflags);
            cpu_push32(cpu, (uint32_t)cpu->cs);
            cpu_push32(cpu, cpu->eip);
        } else {
            cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
            cpu_push16(cpu, cpu->cs);
            cpu_push16(cpu, cpu->ip);
        }
        cpu->cs = handler_sel;
        cpu->eip = handler_off;
        if (virtualized) {
            vm->dpmi.virtual_interrupts_enabled = false;
            cpu->flags = (cpu->flags | FLAG_IF) & ~FLAG_TF;
        } else {
            cpu->flags &= ~(FLAG_IF | FLAG_TF);
        }
        cpu8086_sync_cs(cpu);
        return true;
    }

    if (cpu_deliver_pm_idt_gate(vm, int_num, cpu->eip, 0, false)) {
        if (virtualized) {
            vm->dpmi.virtual_interrupts_enabled = false;
            cpu->flags = (cpu->flags | FLAG_IF) & ~FLAG_TF;
        }
        return true;
    }
    return true;  /* host-owned hardware vector */
}

bool cpu_deliver_exception(dos_vm_t *vm, uint8_t vector,
                           uint32_t return_eip, uint32_t error_code,
                           bool has_error_code)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || vector >= 32u) return false;

    cpu->halted = false;
    if (!cpu->protected_mode) {
        uint32_t ivt_addr = (uint32_t)vector * 4U;
        uint16_t off = dos_mem_read16(vm, ivt_addr);
        uint16_t seg = dos_mem_read16(vm, ivt_addr + 2U);
        if ((seg || off) && seg < (DOS_ROM_BASE >> 4))
            return cpu_deliver_rm_vector(vm, vector, return_eip);
        goto unhandled;
    }
    if (dpmi_deliver_exception(vm, vector, return_eip, error_code))
        return true;
    if (cpu_deliver_pm_idt_gate(vm, vector, return_eip, error_code,
                                has_error_code))
        return true;

unhandled:
    serial_puts(cpu->protected_mode ? "[DPMI]" : "[DOS]");
    serial_puts(" Unhandled processor exception 0x");
    serial_puthex(vector, 2);
    serial_puts(" at ");
    serial_puthex(cpu->cs, 4);
    serial_puts(":");
    serial_puthex(return_eip, 8);
    serial_puts("\n");
    cpu->running = false;
    cpu->exit_code = -1;
    return false;
}

/* ── Native 32-bit INT dispatch (called from dos_int_stub.S) ────── */
/* Register frame layout matching the assembly stub's push order:
 * ES, DS, R15-R8, RBP, RDI, RSI, RDX, RCX, RBX, RAX */

typedef struct {
    uint64_t es, ds;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_num;
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
static cpu8086_state_t *g_native_interpreter_cpu;

void dos_set_native_vm(dos_vm_t *vm)
{
    if (vm && vm->cpu) {
        g_native_interpreter_cpu = vm->cpu;
        g_native_cpu = *vm->cpu;
        g_native_cpu.vm = vm;
    } else {
        g_native_interpreter_cpu = NULL;
    }
    g_native_dos_vm = vm;
}

int dos_native_session_active(void)
{
    /* The VM object lives on the host task stack, which the DOS CR3 does not
     * map. A non-NULL published pointer is the session-active token and is
     * cleared synchronously during teardown. */
    return g_native_dos_vm != 0;
}

void dos_native_cleanup_active(void)
{
    dos_vm_t *vm = g_native_dos_vm;
    if (vm) dos_native_cleanup(vm);
}

/* Set by the shell 'dosrun' command via kern_setjmp, read by the IDT
 * exception path and by dos_int_native_dispatch on program terminate
 * so DOS crashes / exits return cleanly to the shell prompt. */
uint64_t *dos_native_exit_jmpbuf = 0;

extern int dos_native_refresh_guest_selector(dos_vm_t *vm,
                                             uint16_t error_code);
extern void dos_native_sync_ldt(dos_vm_t *vm);

int dos_native_refresh_selector(uint16_t error_code)
{
    dos_vm_t *vm = g_native_dos_vm;
    if (!vm) return 0;
    if (!dos_native_refresh_guest_selector(vm, error_code)) return 0;

    serial_puts("[DOS-NT] refreshed selector 0x");
    serial_puthex(error_code & ~7u, 4);
    serial_puts(" from guest descriptor table\n");
    return 1;
}

static bool dos_native_selector_base(dos_vm_t *vm, uint16_t selector,
                                     uint32_t *base_out)
{
    if (!vm || !vm->cpu || !base_out || (selector & ~3u) == 0)
        return false;

    dpmi_descriptor_t descriptor;
    if (!dpmi_guest_descriptor(vm, selector, &descriptor))
        return false;
    *base_out = dpmi_desc_get_base(&descriptor);
    return true;
}

static bool dos_native_fetch_code_byte(dos_vm_t *vm,
                                       const dpmi_descriptor_t *code,
                                       uint32_t offset, uint8_t *value)
{
    if (!vm || !code || !value || offset > dpmi_desc_get_limit(code))
        return false;
    uint64_t linear = (uint64_t)dpmi_desc_get_base(code) + offset;
    if (linear >= vm->total_mem_size)
        return false;
    *value = vm->mem[linear];
    return true;
}

int dos_native_handle_privileged_fault(x86_interrupt_frame_t *frame)
{
    if (!frame || !g_native_dos_vm || (frame->cs & 3U) != 3U)
        return 0;

    extern uint64_t paging_get_kernel_cr3(void);
    uint64_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    uint64_t kernel_cr3 = paging_get_kernel_cr3();
    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(kernel_cr3) : "memory");

    dos_vm_t *vm = g_native_dos_vm;
    dpmi_descriptor_t code;
    int handled = 0;
    if (!dpmi_guest_descriptor(vm, (uint16_t)frame->cs, &code) ||
        !(code.access & DESC_PRESENT) || !(code.access & DESC_SEGMENT) ||
        !(code.access & DESC_CODE))
        goto done;

    uint32_t rip = (uint32_t)frame->rip;
    uint32_t cursor = 0;
    bool operand_override = false;
    uint8_t opcode = 0;
    while (cursor < 15U) {
        if (!dos_native_fetch_code_byte(vm, &code, rip + cursor, &opcode))
            goto done;
        if (opcode == 0x66U) {
            operand_override = true;
        } else if (opcode == 0x67U || opcode == 0x26U ||
                   opcode == 0x2EU || opcode == 0x36U ||
                   opcode == 0x3EU || opcode == 0x64U ||
                   opcode == 0x65U || opcode == 0xF2U ||
                   opcode == 0xF3U) {
            /* These prefixes do not alter scalar port I/O or CLI/STI. */
        } else {
            break;
        }
        cursor++;
    }
    if (cursor >= 15U)
        goto done;
    cursor++;

    bool default32 = (code.flags_lim & DESC_32BIT) != 0;
    if (opcode == 0xCDU && vm->dpmi.active) {
        uint8_t int_num;
        if (!dos_native_fetch_code_byte(vm, &code, rip + cursor, &int_num))
            goto done;
        uint16_t fault_selector = (uint16_t)frame->error_code;
        if (!(fault_selector & 0x02u) ||
            (fault_selector >> 3) != int_num)
            goto done;

        cpu8086_state_t *cpu = &g_native_cpu;
        vm->cpu = cpu;
        cpu->cs = (uint16_t)frame->cs;
        cpu->eip = rip;
        cpu->ss = (uint16_t)frame->ss;
        cpu->esp = (uint32_t)frame->rsp;
        cpu->eflags = (uint32_t)frame->rflags | FLAGS_FIXED | FLAG_IF;
        cpu->protected_mode = true;
        cpu->pm_cs_loaded = true;
        cpu->running = true;
        cpu->vm = vm;

        uint32_t return_eip = rip + cursor + 1u;
        if (!default32) return_eip = (uint16_t)return_eip;
        if (!cpu_deliver_pm_software_interrupt(vm, int_num, return_eip))
            goto done;

        frame->rip = cpu->eip;
        frame->cs = cpu->cs;
        frame->rflags = (cpu->eflags | FLAGS_FIXED | FLAG_IF) &
                        ~(uint64_t)FLAG_IOPL_MASK;
        frame->rsp = cpu->esp;
        frame->ss = cpu->ss;
        handled = 1;
        goto done;
    }

    if (frame->error_code != 0)
        goto done;

    if ((opcode == 0xFAU || opcode == 0xFBU) && vm->dpmi.active) {
        vm->dpmi.virtual_interrupts_enabled = opcode == 0xFBU;
        frame->rflags = (frame->rflags | FLAGS_FIXED | FLAG_IF) &
                        ~(uint64_t)FLAG_IOPL_MASK;
        frame->rip = default32 ? (uint32_t)(rip + cursor)
                               : (uint16_t)(rip + cursor);
        handled = 1;
        goto done;
    }

    bool input;
    bool immediate;
    uint32_t width;
    switch (opcode) {
    case 0xE4:
        input = true; immediate = true; width = 1; break;
    case 0xE5:
        input = true; immediate = true; width = 0; break;
    case 0xE6:
        input = false; immediate = true; width = 1; break;
    case 0xE7:
        input = false; immediate = true; width = 0; break;
    case 0xEC:
        input = true; immediate = false; width = 1; break;
    case 0xED:
        input = true; immediate = false; width = 0; break;
    case 0xEE:
        input = false; immediate = false; width = 1; break;
    case 0xEF:
        input = false; immediate = false; width = 0; break;
    default:
        goto done;
    }

    if (!width)
        width = (default32 != operand_override) ? 4U : 2U;
    uint16_t port = (uint16_t)frame->rdx;
    if (immediate) {
        uint8_t immediate_port;
        if (!dos_native_fetch_code_byte(vm, &code, rip + cursor,
                                        &immediate_port))
            goto done;
        port = immediate_port;
        cursor++;
    }

    if (input) {
        if (width == 1U) {
            frame->rax = (frame->rax & ~0xFFULL) |
                         dos_io_read8(vm, port);
        } else if (width == 2U) {
            frame->rax = (frame->rax & ~0xFFFFULL) |
                         dos_io_read16(vm, port);
        } else {
            frame->rax = dos_io_read32(vm, port);
        }
    } else if (width == 1U) {
        dos_io_write8(vm, port, (uint8_t)frame->rax);
    } else if (width == 2U) {
        dos_io_write16(vm, port, (uint16_t)frame->rax);
    } else {
        dos_io_write32(vm, port, (uint32_t)frame->rax);
    }

    frame->rip = default32 ? (uint32_t)(rip + cursor)
                           : (uint16_t)(rip + cursor);
    handled = 1;

done:
    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
    return handled;
}

static bool dos_native_deliver_irq(dos_vm_t *vm,
                                   x86_interrupt_frame_t *frame,
                                   uint8_t irq)
{
    if (!vm->dpmi.virtual_interrupts_enabled)
        return false;
    uint8_t vector = 0;
    cpu8086_state_t *cpu = &g_native_cpu;
    vm->cpu = cpu;
    cpu->cs = (uint16_t)frame->cs;
    cpu->eip = (uint32_t)frame->rip;
    cpu->ss = (uint16_t)frame->ss;
    cpu->esp = (uint32_t)frame->rsp;
    cpu->eflags = (uint32_t)frame->rflags | FLAGS_FIXED | FLAG_IF;
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->running = true;
    cpu->vm = vm;

    if (!dos_io_irq_begin(vm, irq, &vector) ||
        !cpu_deliver_hw_interrupt(vm, vector))
        return false;

    frame->rip = cpu->eip;
    frame->cs = cpu->cs;
    /* The guest's original flags are on its emulated interrupt frame. */
    frame->rflags = (cpu->eflags | FLAGS_FIXED | FLAG_IF) &
                    ~(uint64_t)FLAG_IOPL_MASK;
    frame->rsp = cpu->esp;
    frame->ss = cpu->ss;
    return true;
}

bool dos_native_service_audio_irq(x86_interrupt_frame_t *frame)
{
    if (!frame || !g_native_dos_vm || (frame->cs & 3U) != 3U)
        return false;

    extern uint64_t paging_get_kernel_cr3(void);
    uint64_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    uint64_t kernel_cr3 = paging_get_kernel_cr3();
    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(kernel_cr3) : "memory");

    dos_vm_t *vm = g_native_dos_vm;
    uint8_t irq = 0;
    uint32_t pending = 0;
    bool delivered = false;
    if (dos_audio_take_irq(vm, &irq, &pending)) {
        delivered = dos_native_deliver_irq(vm, frame, irq);
        if (!delivered)
            dos_audio_restore_irq(vm, pending);
    }

    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
    return delivered;
}

bool dos_native_service_timer_irq(x86_interrupt_frame_t *frame)
{
    if (!frame || !g_native_dos_vm || (frame->cs & 3U) != 3U)
        return false;

    extern uint64_t paging_get_kernel_cr3(void);
    uint64_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    uint64_t kernel_cr3 = paging_get_kernel_cr3();
    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(kernel_cr3) : "memory");

    dos_vm_t *vm = g_native_dos_vm;
    uint64_t now = idt_get_ticks();
    uint32_t ticks = (uint32_t)(((now - vm->start_ticks) * 182U) / 1000U);
    if (ticks != vm->bios_ticks) {
        vm->bios_ticks = ticks;
        vm->last_timer_tick = now;
        dos_mem_write32(vm, 0x46CU, ticks);
    }
    if (dos_io_timer_poll(vm))
        vm->timer_irq_pending = true;

    bool delivered = false;
    if (vm->timer_irq_pending &&
        dos_native_deliver_irq(vm, frame, 0U)) {
        vm->timer_irq_pending = false;
        delivered = true;
    }

    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
    return delivered;
}

/* Called from the IDT [pf-ist] probe when a DOS native program faults.
 * Dumps the 16 bytes at CS:RIP plus the top of the caller's stack so
 * we can see what opcode faulted AND trace the CALL history. CR3 is
 * switched to kernel so vm->mem's PA identity-map is reachable. */
void dos_native_dump_rip(uint16_t cs, uint32_t rip, uint16_t ss_hint,
                         uint64_t frame_rsp)
{
    if (!g_native_dos_vm) return;

    static uint32_t dump_calls;
    static uint32_t last_rip = 0xFFFFFFFFu;
    if (rip == last_rip && (++dump_calls & 0x3FF) != 0) return;
    last_rip = rip;
    dump_calls = 0;

    extern uint64_t paging_get_kernel_cr3(void);
    uint64_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    uint64_t kernel_cr3 = paging_get_kernel_cr3();
    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(kernel_cr3) : "memory");

    dos_vm_t *vm = g_native_dos_vm;
    uint32_t base;
    if (dos_native_selector_base(vm, cs, &base)) {
        uint64_t linear = (uint64_t)base + rip;
        serial_puts("[pf-ist] linear=0x");
        serial_puthex(linear, 8);
        serial_puts(" bytes:");
        for (unsigned i = 0; i < 16 && linear + i < vm->total_mem_size; i++) {
            serial_puts(" ");
            serial_puthex(vm->mem[linear + i], 2);
        }
        serial_puts("\n");
    }

    uint32_t stack_base;
    if (dos_native_selector_base(vm, ss_hint, &stack_base)) {
        uint32_t esp = (uint32_t)frame_rsp;
        uint64_t linear = (uint64_t)stack_base + esp;
        serial_puts("[pf-ist] ss_base=0x");
        serial_puthex(stack_base, 8);
        serial_puts(" esp=0x");
        serial_puthex(esp, 8);
        serial_puts(" stack:");
        for (unsigned i = 0; i < 16 && linear + i < vm->total_mem_size; i++) {
            serial_puts(" ");
            serial_puthex(vm->mem[linear + i], 2);
        }
        serial_puts("\n");
    }

    if (kernel_cr3 && saved_cr3 != kernel_cr3)
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
    cpu->cs  = (uint16_t)regs->iret_cs;
    cpu->eip = (uint32_t)regs->iret_rip;
    cpu->ss  = (uint16_t)regs->iret_ss;
    cpu->esp = (uint32_t)regs->iret_rsp;
    cpu->eflags = (uint32_t)regs->iret_rflags | FLAGS_FIXED;
    cpu->running = true;
    cpu->protected_mode = true;
    cpu->vm = vm;

    uint8_t reflected_int = 0;
    bool rewrite_iret = false;
    bool software_redirected = false;
    bool host_reflect = int_num == DPMI_DEFAULT_REFLECT_INT &&
                        dos_dpmi_pm_reflect_source(vm, &reflected_int);
    bool host_raw_switch = int_num == DPMI_RAW_SWITCH_INT &&
                           dos_dpmi_pm_stub_source(
                               vm, DPMI_RAW_SWITCH_OFF);
    bool host_exception_return = int_num == DPMI_EXCEPTION_RETURN_INT &&
                                 dos_dpmi_pm_stub_source(
                                     vm, DPMI_EXCEPTION_RETURN_OFF);
    bool host_callback_return = int_num == DPMI_CALLBACK_RETURN_INT &&
                                dos_dpmi_pm_stub_source(
                                    vm, DPMI_CALLBACK_RETURN_OFF);
    bool host_private_interrupt = host_reflect || host_raw_switch ||
                                  host_exception_return ||
                                  host_callback_return;
    if (int_num <= 0xFFu && !host_private_interrupt &&
        cpu_deliver_pm_software_interrupt(vm, (uint8_t)int_num, cpu->eip)) {
        rewrite_iret = true;
        software_redirected = true;
    }

    if (software_redirected) {
        /* Return from the host gate directly into the client's handler. */
    } else if (host_reflect) {
        vm->native_dispatch_depth++;
        bool reflected = dpmi_dispatch_default_interrupt(
            vm, reflected_int, 0);
        vm->native_dispatch_depth--;
        if (!reflected) {
            serial_puts("[DPMI] Native default interrupt reflection failed\n");
            cpu->running = false;
            cpu->exit_code = -1;
        }
    } else if (host_raw_switch) {
        if (!dpmi_raw_mode_switch(vm) || cpu->protected_mode ||
            !g_native_interpreter_cpu) {
            serial_puts("[DOS-NT] PM->RM switch failed\n");
            cpu->running = false;
            cpu->exit_code = -1;
        } else {
            *g_native_interpreter_cpu = *cpu;
            g_native_interpreter_cpu->vm = vm;
            vm->cpu = g_native_interpreter_cpu;
            dos_native_suspend(vm);
        }
    } else if (int_num == 0x101u) {
        /* Processor exceptions use 0x100 | vector internally so they cannot
         * be confused with software INT services. Preserve the processor's
         * return RIP in the DPMI frame; the client handler may adjust it. */
        rewrite_iret = true;
        (void)cpu_deliver_exception(vm, 1, cpu->eip, 0, false);
        if (cpu->running)
            dos_native_sync_ldt(vm);
    } else if (host_exception_return) {
        rewrite_iret = true;
        if (!dpmi_exception_return_native(vm)) {
            cpu->running = false;
            cpu->exit_code = -1;
        }
    } else if (host_callback_return) {
        if (!dpmi_callback_return(vm, false) || cpu->protected_mode ||
            !g_native_interpreter_cpu) {
            serial_puts("[DOS-NT] Callback PM->RM return failed\n");
            cpu->running = false;
            cpu->exit_code = -1;
        } else {
            *g_native_interpreter_cpu = *cpu;
            g_native_interpreter_cpu->vm = vm;
            vm->cpu = g_native_interpreter_cpu;
            dos_native_suspend(vm);
        }
    }

    /* Log INTs — first 50 verbose, then every 256th to keep noise down */
    static uint32_t native_int_count = 0;
    native_int_count++;
    if (native_int_count < 50 || (native_int_count & 0xFF) == 0) {
        if (int_num == 0x101u) {
            serial_puts("[DOS32] EXC ");
            serial_puthex(1, 2);
        } else {
            serial_puts("[DOS32] INT ");
            serial_puthex(int_num, 2);
        }
        serial_puts("h AH=");
        serial_puthex(cpu->ah, 2);
        serial_puts(" #"); serial_putdec(native_int_count);
        serial_puts("\n");
    }

    /* Native exceptions and private transitions were consumed above. */
    if (!software_redirected && !host_private_interrupt &&
        int_num != 0x101u) {
        vm->native_dispatch_depth++;
        dos_int_dispatch(vm, (uint8_t)int_num);
        vm->native_dispatch_depth--;
    }
    uint8_t effective_int = host_reflect ? reflected_int : (uint8_t)int_num;
    if (effective_int == 0x67u && !cpu->protected_mode &&
        (cpu->eflags & FLAG_VM)) {
        if (!g_native_interpreter_cpu) {
            serial_puts("[DOS-NT] VCPI PM->V86 switch has no interpreter context\n");
            cpu->running = false;
            cpu->exit_code = -1;
        } else {
            *g_native_interpreter_cpu = *cpu;
            g_native_interpreter_cpu->vm = vm;
            vm->cpu = g_native_interpreter_cpu;
            dos_native_suspend(vm);
        }
    }
    if (effective_int == 0x31u)
        dos_native_sync_ldt(vm);

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

    if (rewrite_iret && cpu->running) {
        regs->iret_rip = cpu->eip;
        regs->iret_cs = cpu->cs;
        regs->iret_rflags = (cpu->eflags | FLAGS_FIXED | FLAG_IF) &
                            ~(uint64_t)FLAG_IOPL_MASK;
        if (cpu_stack_addr32(cpu))
            regs->iret_rsp = cpu->esp;
        else
            regs->iret_rsp = (regs->iret_rsp & ~0xFFFFULL) | cpu->sp;
        regs->iret_ss = cpu->ss;
    } else {
        /* DOS and DPMI calls return arithmetic status in FLAGS. Preserve the
         * client's control flags (IF, TF, DF, IOPL) exactly as the CPU would. */
        const uint64_t status_flags = FLAG_CF | FLAG_PF | FLAG_AF |
                                      FLAG_ZF | FLAG_SF | FLAG_OF;
        regs->iret_rflags = (regs->iret_rflags & ~status_flags) |
                           (cpu->eflags & status_flags) | FLAGS_FIXED;
    }

    /* Handle terminate (INT 20h or INT 21h/4Ch) */
    if (!cpu->running) {
        if (vm->exec_depth && g_native_interpreter_cpu) {
            *g_native_interpreter_cpu = *cpu;
            g_native_interpreter_cpu->vm = vm;
            vm->cpu = g_native_interpreter_cpu;
            dos_native_suspend(vm);
        }
        serial_puts("[DOS32] Program terminated, exit code ");
        serial_puthex(cpu->exit_code, 2);
        serial_puts("\n");
        uint64_t *exit_jmpbuf = dos_native_exit_jmpbuf;
        dos_native_cleanup(vm);
        extern void x86_tss_reset_ist2(void);
        x86_tss_reset_ist2();
        if (exit_jmpbuf) {
            extern void kern_longjmp(uint64_t *buf, int val);
            __asm__ volatile ("cli" ::: "memory");
            kern_longjmp(exit_jmpbuf, 1);
        }
        serial_puts("[DOS32] No shell recovery context; halting\n");
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }

    /* Restore DOS CR3 before returning to ring-3 DOS code. */
    if (kcr3 && saved_cr3 != kcr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(saved_cr3) : "memory");
    }
}

static void dos_test_descriptor(dpmi_state_t *dpmi, uint16_t selector,
                                bool code, bool use32)
{
    uint16_t index = dpmi_sel_to_index(selector);
    dpmi_descriptor_t *desc = &dpmi->ldt[index];
    for (unsigned i = 0; i < sizeof(*desc); i++)
        ((uint8_t *)desc)[i] = 0;
    dpmi_desc_set_base(desc, 0);
    dpmi_desc_set_limit(desc, 0xFFFFu);
    desc->access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                   (code ? (DESC_CODE | DESC_READABLE) : DESC_WRITABLE);
    if (use32) desc->flags_lim |= DESC_32BIT;
    dpmi->descriptor_state[index] = DPMI_DESC_MUTABLE;
}

static int dos_dpmi_exception_frame_selftest(dos_vm_t *vm,
                                             cpu8086_state_t *cpu,
                                             bool use32,
                                             bool stack32)
{
    const uint16_t handler_sel = dpmi_index_to_sel(1);
    const uint16_t stack_sel = dpmi_index_to_sel(2);
    const uint32_t old_eip = use32 ? 0x00123456u : 0x3456u;
    const uint32_t old_esp = use32 && !stack32
                           ? 0x03A48000u
                           : (use32 ? 0x00008000u : 0x00009000u);
    const uint32_t old_flags = FLAG_IF | FLAG_DF | FLAGS_FIXED;
    const bool old_virtual_interrupts = use32 == stack32;
    int failures = 0;

    dpmi_init(vm);
    dos_test_descriptor(&vm->dpmi, handler_sel, true, use32);
    dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
    vm->dpmi.active = true;
    vm->dpmi.is_32bit = use32;
    vm->dpmi.virtual_interrupts_enabled = old_virtual_interrupts;
    vm->dpmi.exception_vectors[1].sel = handler_sel;
    vm->dpmi.exception_vectors[1].off = use32 ? 0x2000u : 0x0200u;

    cpu8086_init(cpu, vm);
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->op_size_32 = use32;
    cpu->addr_size_32 = use32;
    cpu->cs = handler_sel;
    cpu->eip = old_eip;
    cpu->ss = stack_sel;
    cpu->esp = old_esp;
    cpu->eflags = old_flags;

    if (!cpu_deliver_exception(vm, 1, old_eip, 0, false) ||
        cpu->cs != handler_sel ||
        cpu->eip != vm->dpmi.exception_vectors[1].off ||
        vm->dpmi.exception_depth != 1 ||
        vm->dpmi.virtual_interrupts_enabled ||
        !(cpu->flags & FLAG_IF))
        return 1;

    uint16_t host_sel = vm->dpmi.sel_host_code;
    uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
    if (use32) {
        uint32_t expected_esp = stack32
                              ? old_esp - 32u
                              : (old_esp & 0xFFFF0000u) |
                                (uint16_t)((uint16_t)old_esp - 32u);
        if (cpu->esp != expected_esp ||
            dos_mem_read32(vm, frame) != DPMI_EXCEPTION_RETURN_OFF ||
            dos_mem_read32(vm, frame + 4u) != host_sel ||
            dos_mem_read32(vm, frame + 8u) != 0 ||
            dos_mem_read32(vm, frame + 12u) != old_eip ||
            dos_mem_read32(vm, frame + 16u) != handler_sel ||
            dos_mem_read32(vm, frame + 20u) != old_flags ||
            dos_mem_read32(vm, frame + 24u) != old_esp ||
            dos_mem_read32(vm, frame + 28u) != stack_sel)
            failures++;

        cpu->eip = cpu_pop32(cpu);
        cpu->cs = (uint16_t)cpu_pop32(cpu);
        cpu->eip = DPMI_EXCEPTION_RETURN_OFF + 2u;
        cpu_push32(cpu, cpu->eflags);
        cpu_push32(cpu, cpu->cs);
        cpu_push32(cpu, cpu->eip);
    } else {
        uint32_t expected_esp = stack32
                              ? old_esp - 16u
                              : (old_esp & 0xFFFF0000u) |
                                (uint16_t)((uint16_t)old_esp - 16u);
        if (cpu->esp != expected_esp ||
            dos_mem_read16(vm, frame) != DPMI_EXCEPTION_RETURN_OFF ||
            dos_mem_read16(vm, frame + 2u) != host_sel ||
            dos_mem_read16(vm, frame + 4u) != 0 ||
            dos_mem_read16(vm, frame + 6u) != (uint16_t)old_eip ||
            dos_mem_read16(vm, frame + 8u) != handler_sel ||
            dos_mem_read16(vm, frame + 10u) != (uint16_t)old_flags ||
            dos_mem_read16(vm, frame + 12u) != (uint16_t)old_esp ||
            dos_mem_read16(vm, frame + 14u) != stack_sel)
            failures++;

        cpu->ip = cpu_pop16(cpu);
        cpu->cs = cpu_pop16(cpu);
        cpu->ip = DPMI_EXCEPTION_RETURN_OFF + 2u;
        cpu_push16(cpu, cpu->flags);
        cpu_push16(cpu, cpu->cs);
        cpu_push16(cpu, cpu->ip);
    }

    cpu->flags &= ~(FLAG_IF | FLAG_TF);
    dos_int_dispatch(vm, DPMI_EXCEPTION_RETURN_INT);
    if (!cpu->running || vm->dpmi.exception_depth != 0 ||
        cpu->cs != handler_sel || cpu->eip != old_eip ||
        cpu->ss != stack_sel || cpu->esp != old_esp ||
        cpu->eflags != old_flags ||
        vm->dpmi.virtual_interrupts_enabled != old_virtual_interrupts)
        failures++;
    return failures;
}

static int dos_stack_instruction_selftest(dos_vm_t *vm,
                                          cpu8086_state_t *cpu,
                                          bool use32,
                                          bool stack32)
{
    const uint16_t code_sel = dpmi_index_to_sel(1);
    const uint16_t stack_sel = dpmi_index_to_sel(2);
    const uint32_t code = 0x1000u;
    const uint32_t body = code + 1u;
    const uint32_t function = code + 0x30u;
    const uint32_t top = stack32 ? 0x00009000u : 0x03A49000u;
    const uint32_t width = use32 ? 4u : 2u;
    const uint32_t old_bp = use32 && !stack32
                          ? 0x11227020u : 0x00007020u;
    const uint32_t marker = use32 ? 0xA1B2C3D4u : 0x0000BEEFu;

    dpmi_init(vm);
    dos_test_descriptor(&vm->dpmi, code_sel, true, use32);
    dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
    vm->dpmi.active = true;
    vm->dpmi.is_32bit = use32;

    cpu8086_init(cpu, vm);
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->op_size_32 = use32;
    cpu->addr_size_32 = use32;
    cpu->cs = code_sel;
    cpu->ss = stack_sel;
    cpu->esp = top;
    cpu->ebp = old_bp;

    for (uint32_t i = 0; i < 0x50u; i++)
        vm->mem[code + i] = 0x90;

    vm->mem[code] = 0xCF;             /* IRET, exercised by USE16 */
    uint32_t p = body;
    vm->mem[p++] = 0x83;              /* SUB SP/ESP,8 */
    vm->mem[p++] = 0xEC;
    vm->mem[p++] = 0x08;
    vm->mem[p++] = 0xE8;              /* CALL function */
    if (use32) {
        uint32_t next = p + 4u;
        dos_mem_write32(vm, p, function - next);
        p = next;
        vm->mem[p++] = 0xB8;          /* MOV EAX,00004C2A */
        dos_mem_write32(vm, p, 0x00004C2Au);
        p += 4u;
    } else {
        uint32_t next = p + 2u;
        dos_mem_write16(vm, p, (uint16_t)(function - next));
        p = next;
        vm->mem[p++] = 0xB8;          /* MOV AX,4C2A */
        dos_mem_write16(vm, p, 0x4C2Au);
        p += 2u;
    }
    vm->mem[p++] = 0xCD;
    vm->mem[p++] = 0x21;

    p = function;
    vm->mem[p++] = 0xC8;              /* ENTER 32,2 */
    dos_mem_write16(vm, p, 0x20u);
    p += 2u;
    vm->mem[p++] = 0x02;
    vm->mem[p++] = 0x89;              /* MOV DI/EDI,SP/ESP */
    vm->mem[p++] = 0xE7;
    vm->mem[p++] = 0x89;              /* MOV SI/ESI,BP/EBP */
    vm->mem[p++] = 0xEE;
    vm->mem[p++] = 0xC9;              /* LEAVE */
    vm->mem[p++] = 0xC2;              /* RET 8 */
    dos_mem_write16(vm, p, 8u);

    uint32_t source = stack32 ? old_bp - width
                              : (uint16_t)(old_bp - width);
    uint32_t source_addr = dos_addr(vm, stack_sel, source);
    if (use32)
        dos_mem_write32(vm, source_addr, marker);
    else
        dos_mem_write16(vm, source_addr, (uint16_t)marker);

    if (use32) {
        cpu->eip = body;
    } else {
        cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
        cpu_push16(cpu, code_sel);
        cpu_push16(cpu, (uint16_t)body);
        cpu->eip = code;
    }

    int result = cpu8086_run(vm);
    uint16_t frame_low = (uint16_t)(top - 8u - width - width);
    uint32_t frame = use32 && !stack32
                   ? (top & 0xFFFF0000u) | frame_low
                   : frame_low;
    uint16_t local_low = (uint16_t)(frame_low - 2u * width - 0x20u);
    uint32_t local = use32 && !stack32
                   ? (top & 0xFFFF0000u) | local_low
                   : local_low;
    uint32_t display_marker = dos_addr(vm, stack_sel,
                                      (uint16_t)(frame_low - width));
    uint32_t display_frame = dos_addr(vm, stack_sel,
                                     (uint16_t)(frame_low - 2u * width));

    int failures = 0;
    if (result != 0x2A || cpu->esp != top || cpu->ebp != old_bp ||
        cpu->esi != frame || cpu->edi != local)
        failures++;
    if (use32) {
        if (dos_mem_read32(vm, display_marker) != marker ||
            dos_mem_read32(vm, display_frame) != frame)
            failures++;
    } else if (dos_mem_read16(vm, display_marker) != (uint16_t)marker ||
               dos_mem_read16(vm, display_frame) != (uint16_t)frame) {
        failures++;
    }
    return failures;
}

static int dos_dpmi_virtual_interrupt_selftest(dos_vm_t *vm,
                                                cpu8086_state_t *cpu)
{
    const uint16_t code_sel = dpmi_index_to_sel(1);
    const uint16_t stack_sel = dpmi_index_to_sel(2);
    const uint32_t code = 0x1000u;
    int failures = 0;

    dpmi_init(vm);
    dos_test_descriptor(&vm->dpmi, code_sel, true, false);
    dos_test_descriptor(&vm->dpmi, stack_sel, false, false);
    vm->dpmi.active = true;
    vm->dpmi.is_32bit = false;
    vm->dpmi.pm_vectors[8].sel = code_sel;
    vm->dpmi.pm_vectors[8].off = 0x1100u;

    cpu8086_init(cpu, vm);
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->cs = code_sel;
    cpu->eip = code;
    cpu->ss = stack_sel;
    cpu->esp = 0x8000u;
    cpu->eflags = FLAGS_FIXED | FLAG_IF;

    vm->dpmi.virtual_interrupts_enabled = false;
    if (cpu_deliver_hw_interrupt(vm, 8) || cpu->eip != code ||
        cpu->esp != 0x8000u)
        failures++;

    vm->dpmi.virtual_interrupts_enabled = true;
    if (!cpu_deliver_hw_interrupt(vm, 8) ||
        vm->dpmi.virtual_interrupts_enabled || cpu->eip != 0x1100u ||
        cpu->esp != 0x7FFAu || !(cpu->flags & FLAG_IF) ||
        dos_mem_read16(vm, 0x7FFAu) != (uint16_t)code ||
        dos_mem_read16(vm, 0x7FFCu) != code_sel ||
        !(dos_mem_read16(vm, 0x7FFEu) & FLAG_IF))
        failures++;

    /* A hardware vector without a client handler uses a distinct host stub.
     * Its STI restores virtual IF after the reflected real-mode handler. */
    vm->dpmi.pm_vectors[8].sel = 0;
    vm->dpmi.pm_vectors[8].off = 0;
    cpu8086_init(cpu, vm);
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->cs = code_sel;
    cpu->eip = code;
    cpu->ss = stack_sel;
    cpu->esp = 0x8000u;
    cpu->eflags = FLAGS_FIXED | FLAG_IF;
    vm->dpmi.virtual_interrupts_enabled = true;
    vm->timer_irq_pending = false;
    vm->start_ticks = idt_get_ticks();
    vm->last_timer_tick = vm->start_ticks;

    const uint16_t rm_irq_segment = 0x0180u;
    const uint8_t irq_return_program[] = {
        0xB8, 0x2A, 0x4C,             /* MOV AX,4C2A */
        0xCD, 0x21                    /* INT 21h */
    };
    const uint8_t rm_irq_handler[] = {
        0xBB, 0x78, 0x56,             /* MOV BX,5678 */
        0xCF                          /* IRET */
    };
    for (unsigned i = 0; i < sizeof(irq_return_program); i++)
        vm->mem[code + i] = irq_return_program[i];
    uint32_t rm_irq_address = (uint32_t)rm_irq_segment << 4;
    for (unsigned i = 0; i < sizeof(rm_irq_handler); i++)
        vm->mem[rm_irq_address + i] = rm_irq_handler[i];
    dos_mem_write16(vm, 8u * 4u, 0);
    dos_mem_write16(vm, 8u * 4u + 2u, rm_irq_segment);

    if (!cpu_deliver_hw_interrupt(vm, 8) ||
        vm->dpmi.virtual_interrupts_enabled ||
        cpu->cs != vm->dpmi.sel_host_code ||
        cpu->eip != DPMI_PM_HW_REFLECT_BASE_OFF +
                    8u * DPMI_PM_REFLECT_STUB_SIZE)
        failures++;
    int default_irq_result = cpu8086_run(vm);
    if (default_irq_result != 0x2A || cpu->running ||
        cpu->bx != 0x5678u || cpu->esp != 0x8000u ||
        !vm->dpmi.virtual_interrupts_enabled || !(cpu->flags & FLAG_IF))
        failures++;

    cpu8086_init(cpu, vm);
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->cs = code_sel;
    cpu->eip = code;
    cpu->ss = stack_sel;
    cpu->esp = 0x8000u;
    cpu->eflags = FLAGS_FIXED | FLAG_IF;
    vm->dpmi.virtual_interrupts_enabled = true;

    const uint8_t program[] = {
        0xFA,                         /* CLI: virtual IF = 0 */
        0xB8, 0x02, 0x09,             /* MOV AX,0902 */
        0xCD, 0x31,                   /* INT 31h */
        0x3D, 0x00, 0x09,             /* CMP AX,0900 */
        0x75, 0x10,                   /* JNE fail */
        0xFB,                         /* STI: virtual IF = 1 */
        0xB8, 0x02, 0x09,             /* MOV AX,0902 */
        0xCD, 0x31,                   /* INT 31h */
        0x3D, 0x01, 0x09,             /* CMP AX,0901 */
        0x75, 0x05,                   /* JNE fail */
        0xB8, 0x2A, 0x4C,             /* MOV AX,4C2A */
        0xCD, 0x21,                   /* INT 21h */
        0xB8, 0x76, 0x4C,             /* fail: MOV AX,4C76 */
        0xCD, 0x21                    /* INT 21h */
    };
    for (unsigned i = 0; i < sizeof(program); i++)
        vm->mem[code + i] = program[i];

    int result = cpu8086_run(vm);
    if (result != 0x2A || cpu->running ||
        !vm->dpmi.virtual_interrupts_enabled ||
        !(cpu->flags & FLAG_IF))
        failures++;

    dpmi_init(vm);
    dos_test_descriptor(&vm->dpmi, code_sel, true, false);
    dos_test_descriptor(&vm->dpmi, stack_sel, false, false);
    vm->dpmi.active = true;
    vm->dpmi.is_32bit = false;
    vm->dpmi.virtual_interrupts_enabled = true;
    vm->dpmi.pm_vectors[0x60].sel = code_sel;
    vm->dpmi.pm_vectors[0x60].off = 0x1200u;
    vm->dpmi.pm_vectors[0x07].sel = code_sel;
    vm->dpmi.pm_vectors[0x07].off = 0x1300u;

    cpu8086_init(cpu, vm);
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->cs = code_sel;
    cpu->eip = code;
    cpu->ss = stack_sel;
    cpu->esp = 0x8000u;
    cpu->eflags = FLAGS_FIXED | FLAG_IF;

    const uint8_t vector_program[] = {
        0xCD, 0x60,                   /* installed software vector */
        0xCD, 0x07,                   /* low vector disables virtual IF */
        0xB8, 0x2A, 0x4C,             /* MOV AX,4C2A */
        0xCD, 0x21                    /* INT 21h */
    };
    const uint8_t vector_handler[] = {
        0xB8, 0x02, 0x09,             /* MOV AX,0902 */
        0xCD, 0x31,                   /* INT 31h */
        0x89, 0xC7,                   /* MOV DI,AX */
        0xBB, 0x78, 0x56,             /* MOV BX,5678 */
        0xCF                          /* IRET */
    };
    const uint8_t low_vector_handler[] = {
        0xB8, 0x02, 0x09,             /* MOV AX,0902 */
        0xCD, 0x31,                   /* INT 31h */
        0x89, 0xC6,                   /* MOV SI,AX */
        0xFB,                         /* STI */
        0xBA, 0x34, 0x12,             /* MOV DX,1234 */
        0xCF                          /* IRET */
    };
    for (unsigned i = 0; i < sizeof(vector_program); i++)
        vm->mem[code + i] = vector_program[i];
    for (unsigned i = 0; i < sizeof(vector_handler); i++)
        vm->mem[0x1200u + i] = vector_handler[i];
    for (unsigned i = 0; i < sizeof(low_vector_handler); i++)
        vm->mem[0x1300u + i] = low_vector_handler[i];

    result = cpu8086_run(vm);
    if (result != 0x2A || cpu->running || cpu->bx != 0x5678u ||
        cpu->di != 0x0901u || cpu->si != 0x0900u || cpu->dx != 0x1234u ||
        !vm->dpmi.virtual_interrupts_enabled || !(cpu->flags & FLAG_IF))
        failures++;
    return failures;
}

static int dos_dpmi_default_interrupt_selftest(dos_vm_t *vm,
                                                cpu8086_state_t *cpu)
{
    const uint16_t code_sel = dpmi_index_to_sel(1);
    const uint16_t stack_sel = dpmi_index_to_sel(2);
    const uint32_t code = 0x1000u;
    const uint16_t rm_handler_seg = 0x0180u;

    dpmi_init(vm);
    dos_test_descriptor(&vm->dpmi, code_sel, true, false);
    dos_test_descriptor(&vm->dpmi, stack_sel, false, false);
    vm->dpmi.active = true;
    vm->dpmi.is_32bit = false;
    vm->dpmi.virtual_interrupts_enabled = true;
    if (!dpmi_get_host_code_selector(vm))
        return 1;

    cpu8086_init(cpu, vm);
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->cs = code_sel;
    cpu->eip = code;
    cpu->ss = stack_sel;
    cpu->esp = 0x8000u;
    cpu->ds = stack_sel;
    cpu->es = code_sel;
    cpu->eflags = FLAGS_FIXED | FLAG_IF;

    dos_mem_write16(vm, 0x60u * 4u, 0);
    dos_mem_write16(vm, 0x60u * 4u + 2u, rm_handler_seg);

    const uint8_t program[] = {
        0xBB, 0x11, 0x11,             /* MOV BX,1111 */
        0xBA, 0x22, 0x22,             /* MOV DX,2222 */
        0xF8,                         /* CLC */
        0xCD, 0x60,                   /* default PM vector -> RM handler */
        0x73, 0x11,                   /* JNC fail */
        0x81, 0xFB, 0x78, 0x56,       /* CMP BX,5678 */
        0x75, 0x0B,                   /* JNE fail */
        0x81, 0xFA, 0x34, 0x12,       /* CMP DX,1234 */
        0x75, 0x05,                   /* JNE fail */
        0xB8, 0x2A, 0x4C,             /* MOV AX,4C2A */
        0xCD, 0x21,                   /* INT 21h */
        0xB8, 0x71, 0x4C,             /* fail: MOV AX,4C71 */
        0xCD, 0x21                    /* INT 21h */
    };
    const uint8_t rm_handler[] = {
        0xBB, 0x78, 0x56,             /* MOV BX,5678 */
        0xBA, 0x34, 0x12,             /* MOV DX,1234 */
        0xB8, 0x99, 0x99,             /* MOV AX,9999 */
        0x8E, 0xD8,                   /* MOV DS,AX */
        0xB8, 0x88, 0x88,             /* MOV AX,8888 */
        0x8E, 0xC0,                   /* MOV ES,AX */
        0x55,                         /* PUSH BP */
        0x89, 0xE5,                   /* MOV BP,SP */
        0x83, 0x4E, 0x06, 0x01,       /* OR word [BP+6],1 (saved CF) */
        0x5D,                         /* POP BP */
        0xCF                          /* IRET */
    };
    for (unsigned i = 0; i < sizeof(program); i++)
        vm->mem[code + i] = program[i];
    uint32_t rm_handler_address = (uint32_t)rm_handler_seg << 4;
    for (unsigned i = 0; i < sizeof(rm_handler); i++)
        vm->mem[rm_handler_address + i] = rm_handler[i];

    int result = cpu8086_run(vm);
    if (result != 0x2A || cpu->running || cpu->bx != 0x5678u ||
        cpu->dx != 0x1234u || cpu->ds != stack_sel ||
        cpu->es != code_sel || cpu->esp != 0x8000u ||
        !vm->dpmi.virtual_interrupts_enabled || !(cpu->flags & FLAG_IF))
        return 1;
    return 0;
}

int dos_interrupt_selftest(void)
{

    const uint64_t pages = (DOS_MEM_SIZE + 4095u) / 4096u;
    uint8_t *memory = (uint8_t *)dos_host_alloc_pages(pages);
    if (!memory) return 1;
    for (uint64_t i = 0; i < pages * 4096u; i++) memory[i] = 0;

    dos_vm_t vm = {0};
    cpu8086_state_t cpu;
    vm.mem = memory;
    vm.total_mem_size = DOS_MEM_SIZE;
    vm.cpu = &cpu;

    int failures = 0;
    failures += dos_dpmi_exception_frame_selftest(&vm, &cpu, false, false);
    failures += dos_dpmi_exception_frame_selftest(&vm, &cpu, false, true);
    failures += dos_dpmi_exception_frame_selftest(&vm, &cpu, true, false);
    failures += dos_dpmi_exception_frame_selftest(&vm, &cpu, true, true);
    failures += dos_stack_instruction_selftest(&vm, &cpu, false, false);
    failures += dos_stack_instruction_selftest(&vm, &cpu, false, true);
    failures += dos_stack_instruction_selftest(&vm, &cpu, true, false);
    failures += dos_stack_instruction_selftest(&vm, &cpu, true, true);
    failures += dos_dpmi_virtual_interrupt_selftest(&vm, &cpu);
    failures += dos_dpmi_default_interrupt_selftest(&vm, &cpu);

    /* Execute real-mode HLT and ICEBP. A pending IRQ0 must wake HLT, then
     * vector 1 must run and IRET to the byte following ICEBP. */
    for (uint64_t i = 0; i < pages * 4096u; i++) memory[i] = 0;
    cpu8086_init(&cpu, &vm);
    vm.start_ticks = idt_get_ticks();
    vm.last_timer_tick = vm.start_ticks;
    vm.timer_irq_pending = true;
    cpu.cs = 0x0100;
    cpu.ip = 0;
    cpu.ss = 0x0200;
    cpu.sp = 0x0800;

    dos_mem_write16(&vm, 1u * 4u, 0);
    dos_mem_write16(&vm, 1u * 4u + 2u, 0x0120);
    const uint8_t program[] = {
        0xFB,                   /* STI */
        0xF4,                   /* HLT */
        0xF1,                   /* ICEBP */
        0xB8, 0x2A, 0x4C,       /* MOV AX,4C2A */
        0xCD, 0x21              /* INT 21h */
    };
    const uint8_t handler[] = {
        0xBB, 0x78, 0x56,       /* MOV BX,5678 */
        0xCF                    /* IRET */
    };
    for (unsigned i = 0; i < sizeof(program); i++)
        memory[0x1000u + i] = program[i];
    for (unsigned i = 0; i < sizeof(handler); i++)
        memory[0x1200u + i] = handler[i];

    int result = cpu8086_run(&vm);
    if (result != 0x2A || cpu.bx != 0x5678 || cpu.halted ||
        vm.timer_irq_pending)
        failures++;

    /* An unavailable x87 raises #NM at the instruction boundary. The guest
     * handler advances saved IP by two bytes and returns through IRET. */
    for (uint64_t i = 0; i < pages * 4096u; i++) memory[i] = 0;
    cpu8086_init(&cpu, &vm);
    vm.timer_irq_pending = false;
    vm.start_ticks = idt_get_ticks();
    vm.last_timer_tick = vm.start_ticks;
    cpu.cs = 0x0100;
    cpu.ip = 0;
    cpu.ss = 0x0200;
    cpu.sp = 0x0800;

    dos_mem_write16(&vm, 7U * 4U, 0);
    dos_mem_write16(&vm, 7U * 4U + 2U, 0x0130);
    const uint8_t x87_program[] = {
        0xD9, 0xE8,             /* FLD1 -> #NM */
        0xB8, 0x2A, 0x4C,       /* MOV AX,4C2A */
        0xCD, 0x21              /* INT 21h */
    };
    const uint8_t nm_handler[] = {
        0x58,                   /* POP AX (saved IP) */
        0x05, 0x02, 0x00,       /* ADD AX,2 */
        0x50,                   /* PUSH AX */
        0xBB, 0x78, 0x56,       /* MOV BX,5678 */
        0xCF                    /* IRET */
    };
    for (unsigned i = 0; i < sizeof(x87_program); i++)
        memory[0x1000U + i] = x87_program[i];
    for (unsigned i = 0; i < sizeof(nm_handler); i++)
        memory[0x1300U + i] = nm_handler[i];

    result = cpu8086_run(&vm);
    if (result != 0x2A || cpu.bx != 0x5678 || cpu.running)
        failures++;

    /* CPUID exposes only the virtual CPU contract implemented above. UD2
     * must enter vector 6 at its first byte so a guest handler can recover.
     * A prefixed near Jcc also validates its full 32-bit displacement. */
    for (uint64_t i = 0; i < pages * 4096u; i++) memory[i] = 0;
    cpu8086_init(&cpu, &vm);
    vm.timer_irq_pending = false;
    vm.start_ticks = idt_get_ticks();
    vm.last_timer_tick = vm.start_ticks;
    cpu.cs = 0x0100;
    cpu.ip = 0;
    cpu.ss = 0x0200;
    cpu.sp = 0x0800;

    dos_mem_write16(&vm, 6U * 4U, 0);
    dos_mem_write16(&vm, 6U * 4U + 2U, 0x0140);
    const uint8_t cpuid_program[] = {
        0x66, 0x31, 0xC0,       /* XOR EAX,EAX */
        0x0F, 0xA2,             /* CPUID leaf 0 */
        0x66, 0x89, 0xC7,       /* MOV EDI,EAX */
        0x0F, 0x0B,             /* UD2 -> #UD */
        0x31, 0xC0,             /* XOR AX,AX (ZF=1) */
        0x66, 0x0F, 0x84,       /* JZ rel32 */
        0x05, 0x00, 0x00, 0x00,
        0xBE, 0xAD, 0xDE,       /* MOV SI,DEAD (bad path) */
        0xEB, 0x03,             /* JMP exit */
        0xBE, 0x78, 0x56,       /* MOV SI,5678 (good path) */
        0xB8, 0x2A, 0x4C,       /* MOV AX,4C2A */
        0xCD, 0x21              /* INT 21h */
    };
    const uint8_t ud_handler[] = {
        0x58,                   /* POP AX (saved IP) */
        0x05, 0x02, 0x00,       /* ADD AX,2 */
        0x50,                   /* PUSH AX */
        0xBD, 0x78, 0x56,       /* MOV BP,5678 */
        0xCF                    /* IRET */
    };
    for (unsigned i = 0; i < sizeof(cpuid_program); i++)
        memory[0x1000U + i] = cpuid_program[i];
    for (unsigned i = 0; i < sizeof(ud_handler); i++)
        memory[0x1400U + i] = ud_handler[i];

    result = cpu8086_run(&vm);
    if (result != 0x2A || cpu.edi != 1U || cpu.ebx != 0x7469734FU ||
        cpu.edx != 0x4D564B6FU || cpu.ecx != 0x20555043U ||
        cpu.bp != 0x5678 || cpu.si != 0x5678 || cpu.running)
        failures++;

    dos_host_free_pages(memory, pages);
    return failures;
}
