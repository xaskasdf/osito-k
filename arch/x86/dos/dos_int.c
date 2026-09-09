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
#include "dos_keyboard.h"
#include "dos_jit.h"
#include "dos_paging.h"

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
static bool dpmi_interrupt_return(dos_vm_t *vm, bool discard_private_frame);

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

static bool dos_dpmi_pm_exception_return_source(const dos_vm_t *vm)
{
    return dos_dpmi_pm_stub_source(vm, DPMI_EXCEPTION_RETURN_OFF) ||
           dos_dpmi_pm_stub_source(vm, DPMI_EXCEPTION_EXT_RETURN_OFF);
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

static bool dos_dpmi_pm_exception_source(const dos_vm_t *vm, uint8_t *vector)
{
    const cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    uint32_t first = DPMI_PM_EXCEPTION_BASE_OFF + 2u;
    if (!cpu || !cpu->protected_mode || !vm->dpmi.active ||
        cpu->cs != vm->dpmi.sel_host_code || cpu->eip < first)
        return false;
    uint32_t relative = cpu->eip - first;
    if (relative >= 32u * DPMI_PM_EXCEPTION_STUB_SIZE ||
        relative % DPMI_PM_EXCEPTION_STUB_SIZE)
        return false;
    if (vector) *vector = (uint8_t)(relative / DPMI_PM_EXCEPTION_STUB_SIZE);
    return true;
}

static bool dos_dpmi_pm_private_source(const dos_vm_t *vm, uint8_t int_num)
{
    if (int_num == DPMI_DEFAULT_REFLECT_INT)
        return dos_dpmi_pm_reflect_source(vm, NULL) ||
               dos_dpmi_pm_exception_source(vm, NULL);
    if (int_num == DPMI_CALLBACK_RETURN_INT)
        return dos_dpmi_pm_stub_source(vm, DPMI_CALLBACK_RETURN_OFF);
    if (int_num == DPMI_RAW_SWITCH_INT)
        return dos_dpmi_pm_stub_source(vm, DPMI_RAW_SWITCH_OFF) ||
               dos_dpmi_pm_stub_source(vm, DPMI_SAVE_STATE_OFF);
    if (int_num == DPMI_EXCEPTION_RETURN_INT)
        return dos_dpmi_pm_exception_return_source(vm) ||
               dos_dpmi_pm_stub_source(vm, DPMI_INTERRUPT_RETURN_OFF);
    return false;
}

static bool dos_dpmi_rm_stub_source(const dos_vm_t *vm,
                                    uint32_t stub_offset)
{
    const cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    return cpu && !cpu->protected_mode && cpu->cs == DPMI_ENTRY_SEG &&
           dpmi_desc_get_base(&cpu->cs_cache.descriptor) == (DPMI_ENTRY_SEG << 4) &&
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
        dpmi_desc_get_base(&cpu->cs_cache.descriptor) != (DPMI_ENTRY_SEG << 4) ||
        cpu->eip < first_return || cpu->eip > last_return)
        return false;
    return (cpu->eip - first_return) % DPMI_CALLBACK_STUB_SIZE == 0;
}

static bool dos_rm_service_source(const dos_vm_t *vm, uint8_t *service)
{
    const cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || cpu->protected_mode) return false;
    uint32_t address = dpmi_desc_get_base(&cpu->cs_cache.descriptor) + cpu->eip;
    uint32_t first = DOS_ROM_BASE + DOS_RM_SERVICE_BASE_OFF + 2u;
    if (address < first) return false;
    uint32_t relative = address - first;
    if (relative >= 256u * DOS_RM_SERVICE_STUB_SIZE ||
        relative % DOS_RM_SERVICE_STUB_SIZE) return false;
    uint8_t number = (uint8_t)(relative / DOS_RM_SERVICE_STUB_SIZE);
    if (number != 9U && !dos_int_has_pm_translator(number)) return false;
    *service = number;
    return true;
}

bool dos_rm_private_interrupt(const dos_vm_t *vm, uint8_t vector)
{
    uint8_t service;
    if (vector == DOS_RM_SERVICE_INT) return dos_rm_service_source(vm, &service);
    if (vector == DPMI_ENTRY_INT) return dos_dpmi_rm_stub_source(vm, DPMI_ENTRY_OFF);
    if (vector == DPMI_CALLBACK_ENTRY_INT) return dos_dpmi_rm_callback_source(vm);
    if (vector == DPMI_RAW_SWITCH_INT)
        return dos_dpmi_rm_stub_source(vm, DPMI_RAW_SWITCH_OFF) ||
               dos_dpmi_rm_stub_source(vm, DPMI_SAVE_STATE_OFF);
    return false;
}

bool dos_rm_host_vector(dos_vm_t *vm, uint8_t vector,
                          uint16_t segment, uint16_t offset)
{
    if (segment != (DOS_ROM_BASE >> 4)) return false;
    if (vector == 8 || (vector != 9 && !dos_int_has_pm_translator(vector))) return false;
    if (offset != DOS_RM_SERVICE_BASE_OFF + vector * DOS_RM_SERVICE_STUB_SIZE) return false;
    uint32_t address = DOS_ROM_BASE + offset;
    return dos_mem_read8(vm, address) == 0xCD &&
           dos_mem_read8(vm, address + 1u) == DOS_RM_SERVICE_INT &&
           dos_mem_read8(vm, address + 2u) == 0xCF;
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
    case 0x08:
        vm->cpu->cs = DOS_ROM_BASE >> 4;
        cpu8086_load_real_cs(vm->cpu, vm->cpu->cs);
        vm->cpu->ip = 8;
        break;

    case 0x09:
        dos_keyboard_irq(vm);
        break;

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

    case 0x1C:
        if (vm->cpu->protected_mode || !dpmi_has_real_timer_handler(vm))
            goto generic_interrupt;
        if (!dpmi_reflect_timer(vm)) {
            serial_puts("[DPMI] Invalid reflected INT 1Ch handler or return\n");
            vm->cpu->running = false;
            vm->cpu->exit_code = -1;
        }
        break;

    case 0x20:
        /* Terminate program */
        vm->termination_type = 0;
        vm->process_terminated = true;
        vm->cpu->running = false;
        vm->cpu->exit_code = 0;
        break;

    case 0x23:
    case 0x24:
        if (vm->cpu->protected_mode || !vm->dpmi.active) goto generic_interrupt;
        if (!dpmi_reflect_dos_interrupt(vm, int_num)) {
            serial_puts("[DPMI] Invalid reflected DOS control handler or return\n");
            vm->cpu->running = false;
            vm->cpu->exit_code = -1;
        }
        break;

    case 0x21:
        dos_int21_dispatch(vm);
        break;

    case 0x2F:
        dos_int2f_dispatch(vm);
        break;

    case 0x31: {
        uint16_t caller_cs = vm->cpu->cs;
        bool caller_pm = vm->cpu->protected_mode;
        dos_int31_dpmi(vm);
        /* The host return reloads CS even when the service edited its entry. */
        if (caller_pm && vm->cpu->protected_mode && vm->cpu->cs == caller_cs) {
            cpu8086_sync_cs(vm->cpu);
            cpu8086_sync_data(vm->cpu);
        }
        break;
    }

    case 0x33:
        dos_int33_mouse(vm);
        break;

    case 0x67:
        dos_int67_dispatch(vm);
        break;

    case DOS_RM_SERVICE_INT: {
        uint8_t service;
        if (!dos_rm_service_source(vm, &service))
            goto generic_interrupt;
        cpu8086_rm_service(vm, service);
        break;
    }

    case DPMI_DEFAULT_REFLECT_INT: {
        uint8_t exception;
        if (dos_dpmi_pm_exception_source(vm, &exception)) {
            if (!dpmi_dispatch_default_exception(
                    vm, exception, vm->software_int_frame_bytes)) {
                serial_puts("[DPMI] Invalid default exception frame\n");
                vm->cpu->running = false;
                vm->cpu->exit_code = -1;
            }
            break;
        }
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
        if (dpmi_callback_enter(vm) == DPMI_SERVICE_INVALID) {
            serial_puts("[DPMI] Invalid real-mode callback entry\n");
            vm->cpu->running = false;
            vm->cpu->exit_code = -1;
        }
        break;

    case DPMI_CALLBACK_RETURN_INT:
        if (!dos_dpmi_pm_stub_source(vm, DPMI_CALLBACK_RETURN_OFF))
            goto generic_interrupt;
        if (dpmi_callback_return(vm, true) == DPMI_SERVICE_INVALID) {
            serial_puts("[DPMI] Invalid callback return\n");
            vm->cpu->running = false;
            vm->cpu->exit_code = -1;
        }
        break;

    case DPMI_EXCEPTION_RETURN_INT:
        if (dos_dpmi_pm_stub_source(vm, DPMI_INTERRUPT_RETURN_OFF)) {
            if (!dpmi_interrupt_return(vm, true)) {
                vm->cpu->running = false;
                vm->cpu->exit_code = -1;
            }
            break;
        }
        if (!dos_dpmi_pm_exception_return_source(vm))
            goto generic_interrupt;
        if (!dpmi_exception_return(vm)) {
            vm->cpu->running = false;
            vm->cpu->exit_code = -1;
        }
        break;

    case DPMI_RAW_SWITCH_INT:
        if (dos_dpmi_pm_stub_source(vm, DPMI_SAVE_STATE_OFF) ||
            dos_dpmi_rm_stub_source(vm, DPMI_SAVE_STATE_OFF)) {
            if (dpmi_save_restore_state(vm) == DPMI_SERVICE_INVALID) {
                serial_puts("[DPMI] Invalid 0305h state request or buffer\n");
                vm->cpu->running = false;
                vm->cpu->exit_code = -1;
            }
            break;
        }
        if (!dos_dpmi_pm_stub_source(vm, DPMI_RAW_SWITCH_OFF) &&
            !dos_dpmi_rm_stub_source(vm, DPMI_RAW_SWITCH_OFF))
            goto generic_interrupt;
        if (!dpmi_raw_mode_switch(vm, vm->software_int_frame_bytes)) {
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

        if ((!seg && !off) || seg >= (DOS_ROM_BASE >> 4)) {
            /* DOS's default divide-error handler terminates the program.
             * Returning from an unhandled #DE would retry the same DIV. */
            if (!int_num) {
                serial_puts("[DOS] Divide error: terminating client\n");
                vm->cpu->running = false;
                vm->cpu->exit_code = -1;
                break;
            }
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
        cpu8086_load_real_cs(vm->cpu, seg);
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

static uint8_t dpmi_exception_pte(dos_vm_t *vm, uint32_t linear)
{
    if (!(vm->cpu->cr0 & DOS_CR0_PG)) return 0;
    uint32_t address = (vm->cpu->cr3 & 0xFFFFF000u) + (linear >> 22) * 4u;
    if ((uint64_t)address + 4u > vm->total_mem_size) return 0;
    uint32_t pde = dos_mem_read32(vm, address);
    if (!(pde & 1u)) return 0;
    address = (pde & 0xFFFFF000u) + ((linear >> 12) & 0x3FFu) * 4u;
    if ((uint64_t)address + 4u > vm->total_mem_size) return 0;
    return (uint8_t)dos_mem_read32(vm, address);
}

typedef struct dpmi_real_exception {
    cpu8086_state_t cpu;
    dpmi_stack_t real_stack, protected_stack;
    dpmi_paging_t real_paging;
    uint8_t pte;
} dpmi_real_exception_t;

static bool dpmi_deliver_exception_frame(dos_vm_t *vm, uint8_t vector,
                                         uint32_t return_eip, uint32_t error_code,
                                         dpmi_real_exception_t *real)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (!vm->dpmi.active || vector >= 32u) return false;

    uint16_t host_sel = dpmi_get_host_code_selector(vm);
    if (!host_sel ||
        vm->dpmi.exception_depth >= DPMI_MAX_EXCEPTION_DEPTH)
        return false;

    uint16_t handler_sel = real ? vm->dpmi.real_exception_vectors[vector].sel
                                : vm->dpmi.exception_vectors[vector].sel;
    uint32_t handler_off = real ? vm->dpmi.real_exception_vectors[vector].off
                                : vm->dpmi.exception_vectors[vector].off;
    if (!handler_sel) {
        handler_sel = host_sel;
        handler_off = DPMI_PM_EXCEPTION_BASE_OFF +
                      vector * DPMI_PM_EXCEPTION_STUB_SIZE;
    }

    if (real) {
        dpmi_descriptor_t handler;
        uint8_t required = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT | DESC_CODE;
        if ((handler_sel & 3u) != 3u || !dpmi_guest_descriptor(vm, handler_sel, &handler) ||
            (handler.access & required) != required || (handler.flags_lim & 0x20u) ||
            handler_off > dpmi_desc_get_limit(&handler)) return false;
    }

    uint8_t exception_depth = vm->dpmi.exception_depth;
    dpmi_host_wait_t *wait = vm->dpmi.host_wait;
    bool host = wait && wait->delivering && wait->psp == vm->current_psp &&
                wait->depth == exception_depth;
    if (host) wait->delivering = false;
    bool extended = real || vm->dpmi.exception_extended[vector];
    uint32_t old_flags = real ? real->cpu.eflags | FLAG_VM | FLAGS_FIXED
                              : cpu8086_flags_image(cpu);
    uint16_t old_ss = cpu->ss;
    uint32_t old_esp = cpu->esp;
    uint16_t old_cs = cpu->cs;
    dpmi_stack_t stack;
    unsigned width = vm->dpmi.is_32bit ? 4u : 2u;
    uint32_t frame_size = extended ? DPMI_EXCEPTION_EXT_FRAME_SIZE : 8u * width;
    const uint32_t fields[] = { DPMI_EXCEPTION_RETURN_OFF, host_sel, error_code,
                                return_eip, old_cs, old_flags, old_esp, old_ss };
    uint8_t bytes[DPMI_EXCEPTION_EXT_FRAME_SIZE] = {0};
    for (unsigned i = 0; i < (real ? 2u : 8u); i++)
        for (unsigned b = 0; b < width; b++) bytes[i * width + b] = fields[i] >> (b * 8u);
    if (extended) {
        /* The extension always starts at +20h, including USE16 clients.
         * Each return address chooses one frame; the other is ignored. */
        const uint32_t extra[] = {
            DPMI_EXCEPTION_EXT_RETURN_OFF | (width == 2u ? (uint32_t)host_sel << 16 : 0),
            width == 4u ? host_sel : 0,
            vector == 1u ? (old_flags & FLAG_TF ? 0x8000u : 0u) : error_code,
            return_eip, old_cs | (host ? DPMI_EXCEPTION_HOST << 16 : 0),
            old_flags, old_esp, old_ss, cpu->es, cpu->ds, cpu->fs, cpu->gs,
            vector == 14u ? cpu->cr2 : 0,
            vector == 14u ? (real ? real->pte : dpmi_exception_pte(vm, cpu->cr2)) : 0
        };
        for (unsigned i = 0; i < 14u; i++)
            for (unsigned b = 0; b < 4u; b++) bytes[32u + i * 4u + b] = extra[i] >> (b * 8u);
    }
    if (!dpmi_locked_stack_top(vm, frame_size, &stack) ||
        !dpmi_push_stack_bytes(vm, &stack, bytes, frame_size)) {
        serial_puts("[DPMI] Exception stack unavailable or exhausted\n");
        return false;
    }

    vm->dpmi.exception_virtual_interrupts[exception_depth] =
        vm->dpmi.virtual_interrupts_enabled;
    vm->dpmi.exception_esp_high[exception_depth] = old_esp & 0xFFFF0000u;
    vm->dpmi.exception_context[exception_depth] = (dpmi_exception_context_t){
        .cs = old_cs, .eip = return_eip, .extended = extended, .host = host, .real = real
    };
    vm->dpmi.exception_software_entries[exception_depth] = (dpmi_software_entry_t){0};
    vm->dpmi.virtual_interrupts_enabled = false;
    cpu->cpl = 3;
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    if (real) {
        vm->dpmi.real_mode_stack = (dpmi_stack_t){ real->cpu.ss, real->cpu.sp };
        vm->dpmi.real_mode_paging = (dpmi_paging_t){ real->cpu.cr0, real->cpu.cr3 };
        cpu->eflags &= ~FLAG_VM;
        cpu->ds = cpu->es = cpu->fs = cpu->gs = 0;
        for (unsigned s = 0; s < 6u; s++) if (s != 1u && s != 2u)
            cpu8086_cache_segment(cpu, s, 0, NULL);
    }

    vm->dpmi.exception_depth = exception_depth + 1u;
    cpu->halted = false;
    cpu->irq_shadow = 0;
    cpu->rep_compare.active = false;
    cpu->cs = handler_sel;
    cpu->eip = handler_off;
    cpu->flags = (cpu->flags | FLAG_IF) & ~FLAG_TF;
    cpu8086_sync_cs(cpu);
    return true;
}

static bool dpmi_deliver_exception(dos_vm_t *vm, uint8_t vector,
                                   uint32_t return_eip, uint32_t error_code)
{
    return dpmi_deliver_exception_frame(vm, vector, return_eip, error_code, NULL);
}

static bool dpmi_return_destination_valid(dos_vm_t *vm, uint16_t cs,
                                           uint32_t eip, uint16_t ss)
{
    dpmi_descriptor_t code, stack;
    if ((cs & 3u) != 3u || (ss & 3u) != 3u ||
        !dpmi_guest_descriptor(vm, cs, &code) ||
        !dpmi_guest_descriptor(vm, ss, &stack)) return false;

    uint8_t code_required = DESC_PRESENT | DESC_SEGMENT | DESC_CODE;
    uint8_t stack_required = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT | DESC_WRITABLE;
    if ((code.access & code_required) != code_required ||
        (code.flags_lim & 0x20u) ||
        (!(code.access & 4u) && (code.access & DESC_DPL_MASK) != DESC_DPL3) ||
        eip > dpmi_desc_get_limit(&code) ||
        (stack.access & stack_required) != stack_required ||
        (stack.access & DESC_CODE)) return false;

    /* Loading SS:ESP does not access the new stack. Its limit, backing and
     * effective SS.B-sized offset are checked when memory is accessed, not
     * here: an empty stack or a full ESP on SS16 is a legal return value.
     * Instruction fetch/paging is likewise separate from the CS limit. */
    return true;
}

static bool dpmi_enter_software_interrupt(dos_vm_t *vm,
                                          const dpmi_software_entry_t *entry,
                                          uint32_t fault_eip, bool completed_int);

static bool dpmi_exception_data_segment(dos_vm_t *vm, uint16_t selector,
                                         dpmi_descriptor_t *descriptor)
{
    if (!(selector & ~3u)) {
        *descriptor = (dpmi_descriptor_t){0};
        return true;
    }
    if (!dpmi_guest_descriptor(vm, selector, descriptor)) return false;
    uint8_t access = descriptor->access;
    if ((access & (DESC_PRESENT | DESC_SEGMENT)) != (DESC_PRESENT | DESC_SEGMENT)) return false;
    if ((access & DESC_CODE) && !(access & DESC_READABLE)) return false;
    return (access & (DESC_CODE | 4u)) == (DESC_CODE | 4u) ||
           (access & DESC_DPL_MASK) == DESC_DPL3;
}

static bool dpmi_exception_return_frame(dos_vm_t *vm,
                                        bool discard_private_int_frame)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !cpu->protected_mode || !vm->dpmi.exception_depth ||
        !dos_dpmi_pm_exception_return_source(vm)) {
        serial_puts("[DPMI] Invalid exception return\n");
        return false;
    }

    unsigned width = vm->dpmi.is_32bit ? 4u : 2u;
    bool extended = cpu->eip == DPMI_EXCEPTION_EXT_RETURN_OFF + 2u;
    dpmi_exception_context_t context =
        vm->dpmi.exception_context[vm->dpmi.exception_depth - 1u];
    if ((extended && !context.extended) || (context.real && !extended)) return false;
    unsigned start = discard_private_int_frame ? 3u * width : 0;
    if (extended && width == 2u) start += 4u; /* Reserved DWORD after USE16 RETF. */
    unsigned field_width = extended ? 4u : width;
    unsigned count = extended ? 12u : 6u;
    uint32_t frame_size = start + count * field_width;
    uint8_t bytes[64];
    if (!dpmi_read_stack_frame(vm, bytes, frame_size)) {
        serial_puts("[DPMI] Truncated exception return frame\n");
        return false;
    }

    uint16_t restored_ss;
    uint32_t restored_esp;
    uint16_t restored_cs;
    uint32_t restored_eip;
    uint32_t restored_flags;
    uint32_t fields[12] = {0};
    for (unsigned i = 0; i < count; i++)
        for (unsigned b = 0; b < field_width; b++)
            fields[i] |= (uint32_t)bytes[start + i * field_width + b] << (8u * b);
    restored_eip = fields[1];
    restored_cs = (uint16_t)fields[2];
    restored_flags = fields[3];
    restored_ss = (uint16_t)fields[5];
    if (extended || vm->dpmi.is_32bit) {
        restored_esp = fields[4];
    } else {
        /* A 16-bit public frame replaces SP, not the unrepresented ESP
         * high word. The locked handler stack cannot supply that word. */
        restored_esp = vm->dpmi.exception_esp_high[vm->dpmi.exception_depth - 1u] |
                       fields[4];
    }
    bool redirected = extended && context.host &&
                      ((fields[2] >> 16) & DPMI_EXCEPTION_REDIRECT);
    if (extended && context.host && !redirected) {
        restored_cs = context.cs;
        restored_eip = context.eip;
        if (context.real) restored_flags |= FLAG_VM;
    }
    bool real_return = context.real && (restored_flags & FLAG_VM);
    dpmi_descriptor_t data[4];
    if (extended && !real_return) {
        if (restored_flags & FLAG_VM) return false;
        for (unsigned i = 0; i < 4u; i++)
            if (!dpmi_exception_data_segment(vm, (uint16_t)fields[6u + i], &data[i]))
                return false;
    }
    if (real_return ? restored_eip > dpmi_desc_get_limit(&context.real->cpu.cs_cache.descriptor)
                    : !dpmi_return_destination_valid(vm, restored_cs, restored_eip, restored_ss)) {
        serial_puts("[DPMI] Invalid exception return destination\n");
        return false;
    }

    dpmi_paging_t protected_paging = { cpu->cr0, cpu->cr3 };
    if (real_return) {
        cpu8086_state_t restored = context.real->cpu;
        restored.eax = cpu->eax; restored.ebx = cpu->ebx;
        restored.ecx = cpu->ecx; restored.edx = cpu->edx;
        restored.esi = cpu->esi; restored.edi = cpu->edi; restored.ebp = cpu->ebp;
        restored.insn_count = cpu->insn_count;
        restored.cr2 = cpu->cr2;
        restored.eflags = (restored_flags & (0x003FFFFFu & ~FLAG_VM)) |
                         (restored.eflags & FLAG_VM) | FLAGS_FIXED;
        restored.esp = restored_esp;
        restored.eip = restored_eip;
        restored.irq_shadow = 0;
        restored.rep_compare.active = false;
        uint16_t selectors[] = { (uint16_t)fields[6], restored_cs, restored_ss,
                                 (uint16_t)fields[7], (uint16_t)fields[8], (uint16_t)fields[9] };
        for (unsigned s = 0; s < 6u; s++) cpu8086_load_real_segment(&restored, s, selectors[s]);
        *cpu = restored;
    } else {
        cpu->ss = restored_ss;
        cpu8086_sync_segment(cpu, 2);
        cpu->esp = restored_esp;
        cpu->cs = restored_cs;
        cpu->eip = restored_eip;
        cpu->eflags = (restored_flags & 0x003FFFFFu) | FLAGS_FIXED | FLAG_IF;
    }
    if (extended && !real_return) {
        static const unsigned segment[] = {0, 3, 4, 5};
        cpu->es = (uint16_t)fields[6]; cpu->ds = (uint16_t)fields[7];
        cpu->fs = (uint16_t)fields[8]; cpu->gs = (uint16_t)fields[9];
        for (unsigned i = 0; i < 4u; i++)
            cpu8086_cache_segment(cpu, segment[i], (uint16_t)fields[6u + i],
                                  (uint16_t)fields[6u + i] & ~3u ? &data[i] : NULL);
    }
    cpu->halted = false;
    vm->dpmi.exception_depth--;
    dpmi_software_entry_t pending =
        vm->dpmi.exception_software_entries[vm->dpmi.exception_depth];
    vm->dpmi.exception_software_entries[vm->dpmi.exception_depth] = (dpmi_software_entry_t){0};
    vm->dpmi.exception_esp_high[vm->dpmi.exception_depth] = 0;
    vm->dpmi.exception_context[vm->dpmi.exception_depth] = (dpmi_exception_context_t){0};
    vm->dpmi.virtual_interrupts_enabled =
        vm->emulate_cpu && !real_return ? (restored_flags & FLAG_IF) != 0 :
        vm->dpmi.exception_virtual_interrupts[vm->dpmi.exception_depth];
    if (context.real) {
        vm->dpmi.real_mode_stack = context.real->real_stack;
        vm->dpmi.real_mode_paging = context.real->real_paging;
        vm->dpmi.suspended_stack = context.real->protected_stack;
        vm->dpmi.suspended_paging = protected_paging;
    }
    if (!real_return) cpu8086_sync_cs(cpu);
    dpmi_host_wait_t *wait = vm->dpmi.host_wait;
    if (wait && wait->depth == vm->dpmi.exception_depth &&
        wait->psp == vm->current_psp) {
        wait->returned = true;
        wait->redirected = redirected;
    }
    /* A native gate has already executed INT. Resume only its unfinished
     * entry after an unchanged return destination, using the repaired stack.
     * An edited CS:EIP abandons it, just like an edited instruction return. */
    if (pending.active && cpu->cs == pending.cs && cpu->eip == pending.return_eip)
        return dpmi_enter_software_interrupt(vm, &pending, cpu->eip, true);
    return true;
}

bool dpmi_exception_return(dos_vm_t *vm)
{
    return dpmi_exception_return_frame(vm, true);
}

static bool dpmi_default_real_exception(dos_vm_t *vm, uint8_t vector,
                                        uint32_t return_eip)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t divide = vector ? 0 : dos_mem_read32(vm, 0);
    bool missing_divide = !vector && (!divide ||
        (divide == 0xF0000000u && dos_mem_read8(vm, DOS_ROM_BASE) == 0xCF));
    if (vector == 6u || vector >= 8u || missing_divide) {
        serial_puts("[DPMI] Unhandled real-mode exception "); serial_puthex(vector, 2);
        serial_puts(" at "); serial_puthex(cpu->cs, 4);
        serial_puts(":"); serial_puthex(return_eip, 8); serial_puts("\n");
        cpu->running = false;
        cpu->exit_code = -1;
        return true;
    }
    return cpu8086_deliver_guest_interrupt(vm, vector, CPU_EVENT_EXCEPTION,
                                            return_eip, return_eip, 0, false);
}

bool dpmi_dispatch_default_real_exception(dos_vm_t *vm, uint8_t vector,
                                          uint8_t private_frame_bytes)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !cpu->protected_mode || !vm->dpmi.active || vector >= 32u ||
        !vm->dpmi.exception_depth ||
        !vm->dpmi.exception_context[vm->dpmi.exception_depth - 1u].real) return false;

    cpu8086_state_t saved = *cpu;
    uint8_t saved_bytes = vm->software_int_frame_bytes;
    uint32_t frame[DPMI_EXCEPTION_EXT_FRAME_SIZE / 4u];
    unsigned width = vm->dpmi.is_32bit ? 4u : 2u;
    cpu_stack_adjust(cpu, private_frame_bytes);
    if (!dpmi_read_stack_frame(vm, frame, sizeof(frame))) goto invalid;
    uint32_t return_off = width == 4u ? frame[8] : (uint16_t)frame[8];
    uint16_t return_sel = width == 4u ? (uint16_t)frame[9] : frame[8] >> 16;
    if (return_off != DPMI_EXCEPTION_EXT_RETURN_OFF || return_sel != vm->dpmi.sel_host_code)
        goto invalid;

    /* Real exceptions have no usable legacy return frame. Complete the
     * extended return before building the real IVT handler's IRET frame. */
    cpu_stack_adjust(cpu, 32u + 2u * width);
    cpu->cs = vm->dpmi.sel_host_code;
    cpu->eip = DPMI_EXCEPTION_EXT_RETURN_OFF + 2u;
    vm->software_int_frame_bytes = 0;
    if (!dpmi_exception_return_frame(vm, false)) goto invalid;
    return cpu->protected_mode || dpmi_default_real_exception(vm, vector, cpu->eip);
invalid:
    *cpu = saved;
    vm->software_int_frame_bytes = saved_bytes;
    return false;
}

static bool dpmi_deliver_real_exception(dos_vm_t *vm, uint8_t vector,
                                        uint32_t return_eip, uint32_t error_code,
                                        bool service)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (!vm->dpmi.real_exception_vectors[vector].sel) {
        bool handled = dpmi_default_real_exception(vm, vector, return_eip);
        return !service && handled;
    }

    dpmi_real_exception_t real = { .cpu = *cpu,
        .real_stack = vm->dpmi.real_mode_stack, .real_paging = vm->dpmi.real_mode_paging,
        .protected_stack = vm->dpmi.suspended_stack,
        .pte = vector == 14u ? dpmi_exception_pte(vm, cpu->cr2) : 0 };
    real.cpu.eip = return_eip;
    dpmi_host_wait_t wait = { .previous = vm->dpmi.host_wait,
        .psp = vm->current_psp, .depth = vm->dpmi.exception_depth, .delivering = service };
    vm->dpmi.host_wait = &wait;
    cpu->cr0 = vm->dpmi.suspended_paging.cr0;
    cpu->cr3 = vm->dpmi.suspended_paging.cr3;
    /* Callback records belong to the dormant protected address space,
     * although the suspended caller is real mode. Report that PTE image. */
    if (service && vector == 14u) real.pte = dpmi_exception_pte(vm, cpu->cr2);
    bool delivered = dpmi_deliver_exception_frame(vm, vector, return_eip, error_code, &real);
    if (delivered) {
        vm->native_dispatch_depth++;
        (void)cpu8086_run_until_signal(vm, &wait.returned);
        vm->native_dispatch_depth--;
    } else {
        *cpu = real.cpu;
        if (service) {
            serial_puts("[DPMI] Cannot deliver real-mode host exception\n");
            cpu->running = false;
            cpu->exit_code = -1;
        }
    }
    vm->dpmi.host_wait = wait.previous;
    /* A failed/killed handler must not leave a pointer to this C activation.
     * Normal and nested returns have already popped their private contexts. */
    if (wait.depth < DPMI_MAX_EXCEPTION_DEPTH &&
        vm->dpmi.exception_context[wait.depth].real == &real) {
        for (unsigned i = wait.depth; i < vm->dpmi.exception_depth; i++) {
            vm->dpmi.exception_context[i] = (dpmi_exception_context_t){0};
            vm->dpmi.exception_software_entries[i] = (dpmi_software_entry_t){0};
            vm->dpmi.exception_esp_high[i] = 0;
        }
        vm->dpmi.exception_depth = wait.depth;
        vm->dpmi.virtual_interrupts_enabled = vm->dpmi.exception_virtual_interrupts[wait.depth];
        vm->dpmi.real_mode_stack = real.real_stack;
        vm->dpmi.real_mode_paging = real.real_paging;
        vm->dpmi.suspended_stack = real.protected_stack;
    }
    cpu = vm->cpu;
    if (service)
        return delivered && wait.returned && cpu->running && !wait.redirected &&
               vm->current_psp == wait.psp && !cpu->protected_mode &&
               cpu->cs == real.cpu.cs && cpu->eip == return_eip;
    return delivered && (wait.returned || !cpu->running);
}

bool dpmi_real_service_exception(dos_vm_t *vm, uint8_t vector, uint32_t error,
                                  uint32_t linear)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !cpu->running || cpu->protected_mode || !vm->dpmi.active ||
        vector >= 32u) return false;
    if (vector == 14u) cpu->cr2 = linear;
    return dpmi_deliver_real_exception(vm, vector, cpu->eip, error, true);
}

static bool dpmi_host_exception(dos_vm_t *vm, uint8_t vector, uint32_t error,
                                 uint32_t linear, bool allow_redirect)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !cpu->running || !cpu->protected_mode || !vm->dpmi.active) return false;
    uint16_t cs = cpu->cs;
    uint32_t eip = cpu->eip;
    /* INT has entered the host already. Expose the caller's post-INT stack,
     * and ensure the interpreter does not discard this private frame again. */
    if (vm->software_int_frame_bytes) {
        cpu_stack_adjust(cpu, vm->software_int_frame_bytes);
        cpu->eflags = vm->software_int_return_flags | FLAGS_FIXED | FLAG_IF;
        vm->software_int_frame_bytes = 0;
    }
    dpmi_host_wait_t wait = { .previous = vm->dpmi.host_wait,
        .psp = vm->current_psp, .depth = vm->dpmi.exception_depth, .delivering = true };
    if (vector == 14u) cpu->cr2 = linear;
    vm->dpmi.host_wait = &wait;
    if (!cpu_deliver_exception(vm, vector, eip, error, true) || !cpu->running ||
        vm->dpmi.exception_depth != wait.depth + 1u) {
        vm->dpmi.host_wait = wait.previous;
        return false;
    }
    wait.delivering = false;
    vm->native_dispatch_depth++;
    bool returned = cpu8086_run_until_signal(vm, &wait.returned);
    vm->native_dispatch_depth--;
    vm->dpmi.host_wait = wait.previous;
    cpu = vm->cpu;
    return returned && cpu->running && vm->current_psp == wait.psp &&
           cpu->protected_mode && (allow_redirect ||
           (!wait.redirected && cpu->cs == cs && cpu->eip == eip));
}

bool dpmi_service_exception(dos_vm_t *vm, uint8_t vector, uint32_t error,
                             uint32_t linear)
{
    return dpmi_host_exception(vm, vector, error, linear, false);
}

bool dos_native_prepare_return(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !cpu->running) return false;
    for (;;) {
        cpu_system_segment_t loaded[6];
        cpu_event_fault_t fault = {0};
        if (cpu8086_probe_native_segments(cpu, loaded, &fault)) {
            uint16_t selectors[] = { cpu->es, cpu->cs, cpu->ss, cpu->ds, cpu->fs, cpu->gs };
            for (unsigned i = 0; i < 6; i++)
                cpu8086_cache_segment(cpu, i, selectors[i], loaded[i].valid ? &loaded[i].descriptor : NULL);
            return true;
        }
        if (!fault.raised ||
            !dpmi_host_exception(vm, fault.vector, fault.error, cpu->cr2, true)) {
            serial_puts("[DOS-NT] Cannot restore client segment state\n");
            cpu = vm->cpu;
            if (cpu->running) {
                cpu->running = false;
                cpu->exit_code = -1;
            }
            return false;
        }
        /* The handler may have replaced selectors or CS:EIP. Validate its
         * complete result before any host MOV segment or IRET executes. */
        cpu = vm->cpu;
    }
}

static bool dos_native_admit_gate_return(dos_vm_t *vm)
{
    /* Repair the caller before an IRQ can replace its CS/SS. Then admit
     * the handler image too, without replaying the completed host service. */
    if (!dos_native_prepare_return(vm)) return false;
    if (!vm->native_dispatch_depth && cpu8086_service_interrupts(vm))
        return dos_native_prepare_return(vm);
    return vm->cpu->running;
}

static bool dpmi_exception_return_native(dos_vm_t *vm)
{
    /* Native INT FD switches to IST2, so its CPU IRET frame is not part of
     * the DPMI client stack that begins at the exception error-code field. */
    return dpmi_exception_return_frame(vm, false);
}

static bool dpmi_deliver_locked_interrupt(dos_vm_t *vm, uint16_t selector,
                                          uint32_t offset, uint32_t return_eip,
                                          bool hardware)
{
    cpu8086_state_t *cpu = vm->cpu;
    dpmi_state_t *dpmi = &vm->dpmi;
    dpmi_stack_t stack;
    dpmi_stack_t caller = { cpu->ss, cpu->esp };
    uint32_t flags = cpu8086_flags_image(cpu);
    uint16_t host = dpmi_get_host_code_selector(vm);
    const uint32_t fields[] = { DPMI_INTERRUPT_RETURN_OFF, host, flags };
    if (!host || dpmi->interrupt_depth >= DPMI_MAX_INTERRUPT_DEPTH ||
        !dpmi_locked_stack_top(vm, dpmi->is_32bit ? 12u : 6u, &stack) ||
        !dpmi_push_stack_frame(vm, &stack, fields, 3)) {
        serial_puts("[DPMI] Locked interrupt stack unavailable or exhausted\n");
        cpu->running = false;
        cpu->exit_code = -1;
        return true;
    }
    dpmi_interrupt_frame_t *frame = &dpmi->interrupt_frames[dpmi->interrupt_depth++];
    frame->caller = caller;
    frame->handler = stack;
    frame->cs = cpu->cs;
    frame->eip = return_eip;
    frame->virtual_if = dpmi->virtual_interrupts_enabled;
    cpu->cs = selector;
    cpu->eip = offset;
    cpu->flags = (cpu->flags | FLAG_IF) & ~FLAG_TF;
    if (hardware) dpmi->virtual_interrupts_enabled = false;
    cpu8086_sync_cs(cpu);
    return true;
}

static bool dpmi_interrupt_return(dos_vm_t *vm, bool discard_private_frame)
{
    cpu8086_state_t *cpu = vm->cpu;
    dpmi_state_t *dpmi = &vm->dpmi;
    if (!cpu->protected_mode || !dpmi->interrupt_depth ||
        !dos_dpmi_pm_stub_source(vm, DPMI_INTERRUPT_RETURN_OFF)) return false;
    dpmi_interrupt_frame_t *frame = &dpmi->interrupt_frames[dpmi->interrupt_depth - 1u];
    uint32_t after = cpu->esp;
    uint32_t flags = cpu->eflags;
    if (discard_private_frame) {
        unsigned width = dpmi->is_32bit ? 4u : 2u;
        uint8_t bytes[12];
        if (!dpmi_read_stack_frame(vm, bytes, 3u * width))
            return false;
        flags = 0;
        for (unsigned b = 0; b < width; b++)
            flags |= (uint32_t)bytes[2u * width + b] << (8u * b);
        after = cpu_stack_addr32(cpu) ? after + 3u * width
              : (after & 0xFFFF0000u) | (uint16_t)(after + 3u * width);
    }
    if (cpu->ss != frame->handler.ss || after != frame->handler.esp) {
        serial_puts("[DPMI] Unbalanced locked interrupt return\n");
        return false;
    }
    if (!dpmi_return_destination_valid(vm, frame->cs, frame->eip, frame->caller.ss)) {
        serial_puts("[DPMI] Invalid locked interrupt return destination\n");
        return false;
    }
    cpu->ss = frame->caller.ss;
    cpu8086_sync_segment(cpu, 2);
    cpu->esp = frame->caller.esp;
    cpu->cs = frame->cs;
    cpu->eip = frame->eip;
    cpu->eflags = (flags & 0x003FFFFFu) | FLAGS_FIXED | FLAG_IF;
    dpmi->virtual_interrupts_enabled = vm->emulate_cpu
                                    ? (flags & FLAG_IF) != 0 : frame->virtual_if;
    dpmi->interrupt_depth--;
    cpu8086_sync_cs(cpu);
    return true;
}

static bool dpmi_enter_software_interrupt(dos_vm_t *vm,
                                          const dpmi_software_entry_t *entry,
                                          uint32_t fault_eip, bool completed_int)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t flags = cpu8086_flags_image(cpu);
    if (!vm->emulate_cpu) flags |= FLAG_IF;
    unsigned depth = vm->dpmi.exception_depth;
    if (!cpu8086_enter_dpmi_interrupt(cpu, entry->handler_sel, entry->handler_off,
                                       vm->dpmi.is_32bit ? 4u : 2u,
                                       entry->return_eip, flags, fault_eip)) {
        if (completed_int && vm->dpmi.exception_depth == depth + 1u)
            vm->dpmi.exception_software_entries[depth] = *entry;
        return true; /* A selected vector's fault must not fall through to DOS. */
    }
    cpu->flags = (cpu->flags | FLAG_IF) & ~FLAG_TF;
    if (entry->vector <= 7u) vm->dpmi.virtual_interrupts_enabled = false;
    return true;
}

static bool dpmi_deliver_software_interrupt(dos_vm_t *vm, uint8_t int_num,
                                            uint32_t return_eip, uint32_t fault_eip,
                                            bool completed_int)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !cpu->protected_mode || cpu8086_uses_guest_idt(cpu) || !vm->dpmi.active ||
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

    if (int_num == 0x1Cu || int_num == 0x23u || int_num == 0x24u)
        return dpmi_deliver_locked_interrupt(vm, handler_sel, handler_off, return_eip, false);

    const dpmi_software_entry_t entry = { .cs = cpu->cs,
        .handler_sel = handler_sel, .return_eip = return_eip,
        .handler_off = handler_off, .vector = int_num, .active = true };
    return dpmi_enter_software_interrupt(vm, &entry, fault_eip, completed_int);
}

bool cpu_deliver_pm_software_interrupt(dos_vm_t *vm, uint8_t int_num,
                                       uint32_t return_eip, uint32_t fault_eip)
{
    return dpmi_deliver_software_interrupt(vm, int_num, return_eip, fault_eip, false);
}

bool cpu_deliver_hw_interrupt(dos_vm_t *vm, uint8_t int_num)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || cpu->irq_shadow) return false;

    if (!cpu->protected_mode || cpu8086_uses_guest_idt(cpu)) {
        if (!(cpu->flags & FLAG_IF)) return false;
        return cpu8086_deliver_guest_interrupt(vm, int_num, CPU_EVENT_EXTERNAL,
                                                cpu->eip, cpu->eip, 0, false);
    }
    /* Without DPMI, protected interrupts already take the guest IDT path. */
    if (!vm->dpmi.virtual_interrupts_enabled) return false;
    cpu->halted = false;
    cpu->flags |= FLAG_IF;

    uint16_t handler_sel = vm->dpmi.pm_vectors[int_num].sel;
    uint32_t handler_off = vm->dpmi.pm_vectors[int_num].off;
    if (!handler_sel) {
        handler_sel = dpmi_get_host_code_selector(vm);
        handler_off = DPMI_PM_HW_REFLECT_BASE_OFF +
                      int_num * DPMI_PM_REFLECT_STUB_SIZE;
    }

    if (handler_sel) {
        bool delivered = dpmi_deliver_locked_interrupt(vm, handler_sel, handler_off, cpu->eip, true);
        if (delivered) cpu->rep_compare.active = false;
        return delivered;
    }

    cpu->rep_compare.active = false;
    return true;  /* host-owned hardware vector */
}

bool cpu_deliver_exception(dos_vm_t *vm, uint8_t vector,
                           uint32_t return_eip, uint32_t error_code,
                           bool has_error_code)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || vector >= 32u) return false;

    if (cpu->delivery_fault) {
        *cpu->delivery_fault = (cpu_event_fault_t){ .vector = vector, .error = error_code,
            .return_eip = return_eip, .raised = true, .has_error = has_error_code };
        return false;
    }
    if (cpu8086_uses_guest_idt(cpu) || (!cpu->protected_mode && !vm->dpmi.active))
        return cpu8086_deliver_guest_interrupt(vm, vector, CPU_EVENT_EXCEPTION,
                                                return_eip, return_eip, error_code, has_error_code);
    if (!cpu->protected_mode) {
        if (dpmi_deliver_real_exception(vm, vector, return_eip, error_code, false)) return true;
        goto unhandled;
    }
    cpu->irq_shadow = 0;
    cpu->rep_compare.active = false;
    cpu->halted = false;
    if (vm->dpmi.active) {
        if (dpmi_deliver_exception(vm, vector, return_eip, error_code))
            return true;
        goto unhandled;
    }
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
 * GS, FS, ES, DS, R15-R8, RBP, RDI, RSI, RDX, RCX, RBX, RAX */

typedef struct {
    uint64_t gs, fs, es, ds;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_num;
    /* IRET frame (pushed by CPU on INT entry, popped by IRETQ on return).
     * Layout matches dos_int_stub.S after the GPR area. */
    uint64_t iret_rip, iret_cs, iret_rflags, iret_rsp, iret_ss;
} dos_native_regs_t;

_Static_assert(__builtin_offsetof(dos_native_regs_t, int_num) == 19 * 8,
               "DOS INT stub vector offset");
_Static_assert(__builtin_offsetof(dos_native_regs_t, iret_rip) == 20 * 8,
               "DOS INT stub IRET offset");
_Static_assert(sizeof(dos_native_regs_t) == 25 * 8, "DOS INT stub frame size");

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

static cpu8086_state_t *dos_native_import_frame(
    dos_vm_t *vm, const x86_interrupt_frame_t *frame,
    const dpmi_descriptor_t *code)
{
    cpu8086_state_t *cpu = &g_native_cpu;
    cpu->irq_shadow = 0;
    cpu->rep_compare.active = false;
    vm->cpu = cpu;
    cpu->vm = vm;
    cpu->eax = (uint32_t)frame->rax;
    cpu->ebx = (uint32_t)frame->rbx;
    cpu->ecx = (uint32_t)frame->rcx;
    cpu->edx = (uint32_t)frame->rdx;
    cpu->esi = (uint32_t)frame->rsi;
    cpu->edi = (uint32_t)frame->rdi;
    cpu->ebp = (uint32_t)frame->rbp;
    cpu->esp = (uint32_t)frame->rsp;
    cpu->cs = (uint16_t)frame->cs;
    cpu->ss = (uint16_t)frame->ss;
    /* isr_common preserves the native data selectors across its C call. */
    __asm__ volatile ("mov %%ds, %0" : "=r"(cpu->ds));
    __asm__ volatile ("mov %%es, %0" : "=r"(cpu->es));
    __asm__ volatile ("mov %%fs, %0" : "=r"(cpu->fs));
    __asm__ volatile ("mov %%gs, %0" : "=r"(cpu->gs));
    cpu->eip = (uint32_t)frame->rip;
    cpu->eflags = (uint32_t)frame->rflags | FLAGS_FIXED;
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->op_size_32 = (code->flags_lim & DESC_32BIT) != 0;
    cpu->addr_size_32 = cpu->op_size_32;
    cpu8086_cache_cs(cpu, cpu->cs, code, cpu->cs & 3u);
    cpu8086_sync_data(cpu);
    cpu->running = true;
    cpu->halted = false;
    return cpu;
}

static void dos_native_export_frame(const cpu8086_state_t *cpu,
                                     x86_interrupt_frame_t *frame)
{
    frame->rax = cpu->eax;
    frame->rbx = cpu->ebx;
    frame->rcx = cpu->ecx;
    frame->rdx = cpu->edx;
    frame->rsi = cpu->esi;
    frame->rdi = cpu->edi;
    frame->rbp = cpu->ebp;
    frame->rsp = cpu->esp;
    frame->cs = cpu->cs;
    frame->ss = cpu->ss;
    frame->rip = cpu->op_size_32 ? cpu->eip : cpu->ip;
    frame->rflags = (cpu->eflags | FLAGS_FIXED | FLAG_IF) &
                    ~(uint64_t)FLAG_IOPL_MASK;
}

static bool dos_native_step_data(dos_vm_t *vm, x86_interrupt_frame_t *frame,
                                  const dpmi_descriptor_t *code)
{
    cpu8086_state_t *cpu = dos_native_import_frame(vm, frame, code);
    unsigned exception_depth = vm->dpmi.exception_depth;
    if (!cpu8086_run_one(vm)) return false;
    if (vm->dpmi.exception_depth != exception_depth) dos_native_sync_ldt(vm);
    dos_native_export_frame(cpu, frame);
    return true;
}

bool dos_native_handle_exception(x86_interrupt_frame_t *frame)
{
    /* NMI, double fault, machine check and newer system events are not
     * evidence of a recoverable DOS client exception. */
    const uint32_t client_vectors = (1u << 0) | (1u << 3) | (1u << 4) |
        (1u << 5) | (1u << 6) | (1u << 7) | (1u << 9) | (1u << 10) |
        (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) |
        (1u << 16) | (1u << 17) | (1u << 19);
    if (!frame || !g_native_dos_vm || (frame->cs & 3u) != 3u ||
        frame->vector >= 32 || !(client_vectors & (1u << frame->vector)))
        return false;

    uint64_t fault_address = 0;
    if (frame->vector == 14)
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
    extern uint64_t paging_get_kernel_cr3(void);
    uint64_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    uint64_t kernel_cr3 = paging_get_kernel_cr3();
    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(kernel_cr3) : "memory");

    dos_vm_t *vm = g_native_dos_vm;
    dpmi_descriptor_t code;
    bool handled = false;
    if (!vm->native_active || saved_cr3 != vm->native_cr3 ||
        !vm->dpmi.active || vm->native_dispatch_depth ||
        !dpmi_guest_descriptor(vm, (uint16_t)frame->cs, &code) ||
        !(code.access & DESC_PRESENT) || !(code.access & DESC_SEGMENT) ||
        !(code.access & DESC_CODE))
        goto done;

    if (frame->vector == 14 && vm->vga_mode == 0x13u && !vm->vbe_active) {
        if ((frame->error_code == 5u || frame->error_code == 7u) &&
            fault_address - DOS_VGA_APERTURE_BASE < DOS_VGA_APERTURE_SIZE)
            goto done; /* an unsupported host VGA trap is not a guest #PF */
    }

    uint8_t vector = (uint8_t)frame->vector;
    uint32_t return_eip = (uint32_t)frame->rip;
    uint32_t error_code = (uint32_t)frame->error_code;
    if (vector == 13) {
        /* DPL0 host gates turn native INT3/INTO into #GP. Preserve their
         * trap (next-IP) semantics without making INT n an exception. */
        for (unsigned cursor = 0; cursor < 15u; cursor++) {
            uint8_t opcode;
            if (cursor > UINT32_MAX - return_eip ||
                !dos_native_fetch_code_byte(vm, &code, return_eip + cursor,
                                            &opcode))
                break;
            if (opcode == 0x26 || opcode == 0x2E || opcode == 0x36 ||
                opcode == 0x3E || opcode == 0x64 || opcode == 0x65 ||
                opcode == 0x66 || opcode == 0x67 ||
                opcode == 0xF2 || opcode == 0xF3)
                continue;
            if ((opcode == 0xCC && error_code == ((3u << 3) | 2u)) ||
                (opcode == 0xCE && (frame->rflags & FLAG_OF) &&
                 error_code == ((4u << 3) | 2u))) {
                vector = opcode == 0xCC ? 3u : 4u;
                return_eip += cursor + 1u;
                if (!(code.flags_lim & DESC_32BIT)) return_eip &= 0xFFFFu;
                error_code = 0;
            }
            break;
        }
    }

    cpu8086_state_t *cpu = dos_native_import_frame(vm, frame, &code);
    if (vector != frame->vector)
        cpu->eflags &= ~FLAG_RF; /* remove RF introduced by the host #GP */
    if (vector == 14) cpu->cr2 = (uint32_t)fault_address;
    bool has_error = vector == 10 || vector == 11 || vector == 12 ||
                     vector == 13 || vector == 14 || vector == 17;
    if (!cpu_deliver_exception(vm, vector, return_eip, error_code, has_error))
        goto done;
    dos_native_sync_ldt(vm);
    dos_native_export_frame(cpu, frame);
    handled = true;

done:
    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
    return handled;
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
            /* String I/O re-decodes the complete instruction below. */
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

        cpu8086_state_t *cpu = dos_native_import_frame(vm, frame, &code);
        cpu->eflags &= ~FLAG_RF; /* host #GP introduced this bit */

        uint32_t return_eip = rip + cursor + 1u;
        if (!default32) return_eip = (uint16_t)return_eip;
        if (!cpu_deliver_pm_software_interrupt(vm, int_num, return_eip, rip))
            goto done;

        if (!cpu->running) goto done;
        dos_native_sync_ldt(vm);
        dos_native_export_frame(cpu, frame);
        handled = 1;
        goto done;
    }

    if (frame->error_code != 0)
        goto done;

    if (opcode >= 0x6Cu && opcode <= 0x6Fu) {
        handled = dos_native_step_data(vm, frame, &code);
        goto done;
    }

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

bool dos_native_handle_memory_fault(x86_interrupt_frame_t *frame,
                                     uint64_t fault_address)
{
    /* Only deliberate user data-access protection faults on this VM's VGA
     * aperture belong here. Ordinary faults retain the normal exception path. */
    if (!frame || !g_native_dos_vm || (frame->cs & 3u) != 3u ||
        (frame->error_code != 5u && frame->error_code != 7u) ||
        fault_address - DOS_VGA_APERTURE_BASE >= DOS_VGA_APERTURE_SIZE)
        return false;

    extern uint64_t paging_get_kernel_cr3(void);
    uint64_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    uint64_t kernel_cr3 = paging_get_kernel_cr3();
    if (kernel_cr3 && saved_cr3 != kernel_cr3)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(kernel_cr3) : "memory");

    dos_vm_t *vm = g_native_dos_vm;
    dpmi_descriptor_t code;
    bool handled = false;
    if (!vm->io || vm->vga_mode != 0x13u || vm->vbe_active ||
        saved_cr3 != vm->native_cr3 ||
        !dpmi_guest_descriptor(vm, (uint16_t)frame->cs, &code) ||
        !(code.access & DESC_PRESENT) || !(code.access & DESC_SEGMENT) ||
        !(code.access & DESC_CODE))
        goto done;

    uint8_t opcode = 0;
    unsigned cursor;
    for (cursor = 0; cursor < 15u; cursor++) {
        if (!dos_native_fetch_code_byte(vm, &code,
                                        (uint32_t)frame->rip + cursor,
                                        &opcode))
            goto done;
        if (opcode != 0x26 && opcode != 0x2E && opcode != 0x36 &&
            opcode != 0x3E && opcode != 0x64 && opcode != 0x65 &&
            opcode != 0x66 && opcode != 0x67 && opcode != 0xF0 &&
            opcode != 0xF2 && opcode != 0xF3)
            break;
    }
    if (cursor == 15u) goto done;

    /* These integer memory operations have matching 16/32-bit interpreter
     * semantics and preserve segment registers. Do not pretend that x87,
     * vector memory operations or control transfers are supported here. */
    bool supported = (opcode <= 0x3Bu && (opcode & 7u) <= 3u) ||
        (opcode >= 0x80 && opcode <= 0x8B) ||
        (opcode >= 0xA0 && opcode <= 0xA7) ||
        (opcode >= 0xAA && opcode <= 0xAF) ||
        opcode == 0xC6 || opcode == 0xC7;
    if (!supported) {
        serial_puts("[DOS/VGA] unsupported native memory opcode ");
        serial_puthex(opcode, 2);
        serial_puts(" at ");
        serial_puthex(frame->cs, 4);
        serial_puts(":");
        serial_puthex(frame->rip, 8);
        serial_puts("\n");
        goto done;
    }

    if (!dos_native_step_data(vm, frame, &code)) goto done;
    vm->native_vga_faults++;
    handled = true;

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
    /* This frame is an actual native interrupt boundary. Hardware has already
     * honored native SS inhibition; interpreter continuations are obsolete. */
    cpu->irq_shadow = 0;
    cpu->rep_compare.active = false;
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

    cpu8086_sync_cs(cpu);
    cpu8086_sync_segment(cpu, 2);
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

bool dos_native_service_keyboard_irq(x86_interrupt_frame_t *frame)
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
    bool pending = dos_io_keyboard_poll(vm);
    bool delivered = pending && dos_native_deliver_irq(vm, frame, 1);
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

    /* The VM object may live on a host stack absent from the DOS CR3.
     * Guest RAM itself is accessed through the kernel direct map. */
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
    cpu->fs  = (uint16_t)regs->fs;
    cpu->gs  = (uint16_t)regs->gs;
    cpu->cs  = (uint16_t)regs->iret_cs;
    cpu->eip = (uint32_t)regs->iret_rip;
    cpu->ss  = (uint16_t)regs->iret_ss;
    cpu->esp = (uint32_t)regs->iret_rsp;
    cpu->eflags = (uint32_t)regs->iret_rflags | FLAGS_FIXED;
    cpu->running = true;
    cpu->protected_mode = true;
    cpu->vm = vm;

    uint8_t reflected_int = 0;
    cpu8086_sync_cs(cpu);
    cpu8086_sync_data(cpu);
    bool rewrite_iret = false;
    bool software_redirected = false;
    bool host_reflect = int_num == DPMI_DEFAULT_REFLECT_INT &&
                        dos_dpmi_pm_reflect_source(vm, &reflected_int);
    uint8_t default_exception = 0;
    bool host_default_exception = int_num == DPMI_DEFAULT_REFLECT_INT &&
        dos_dpmi_pm_exception_source(vm, &default_exception);
    bool host_raw_switch = int_num == DPMI_RAW_SWITCH_INT &&
                           dos_dpmi_pm_stub_source(
                               vm, DPMI_RAW_SWITCH_OFF);
    bool host_save_state = int_num == DPMI_RAW_SWITCH_INT &&
                           dos_dpmi_pm_stub_source(vm, DPMI_SAVE_STATE_OFF);
    bool host_exception_return = int_num == DPMI_EXCEPTION_RETURN_INT &&
                                 dos_dpmi_pm_exception_return_source(vm);
    bool host_interrupt_return = int_num == DPMI_EXCEPTION_RETURN_INT &&
                                 dos_dpmi_pm_stub_source(vm, DPMI_INTERRUPT_RETURN_OFF);
    bool host_callback_return = int_num == DPMI_CALLBACK_RETURN_INT &&
                                dos_dpmi_pm_stub_source(
                                    vm, DPMI_CALLBACK_RETURN_OFF);
    bool host_private_interrupt = host_reflect || host_default_exception ||
                                  host_raw_switch || host_save_state ||
                                  host_exception_return || host_interrupt_return ||
                                  host_callback_return;
    if (int_num <= 0xFFu && !host_private_interrupt &&
        dpmi_deliver_software_interrupt(vm, (uint8_t)int_num, cpu->eip, cpu->eip, true)) {
        rewrite_iret = true;
        software_redirected = true;
        if (cpu->running) dos_native_sync_ldt(vm);
    }

    if (software_redirected) {
        /* Return from the host gate directly into the client's handler. */
    } else if (host_default_exception) {
        vm->native_dispatch_depth++;
        bool handled = dpmi_dispatch_default_exception(
            vm, default_exception, 0);
        vm->native_dispatch_depth--;
        if (!handled) {
            serial_puts("[DPMI] Invalid native default exception frame\n");
            cpu->running = false;
            cpu->exit_code = -1;
        }
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
    } else if (host_save_state) {
        if (dpmi_save_restore_state(vm) == DPMI_SERVICE_INVALID) {
            serial_puts("[DPMI] Invalid native 0305h state request or buffer\n");
            cpu->running = false;
            cpu->exit_code = -1;
        }
    } else if (host_raw_switch) {
        if (!dpmi_raw_mode_switch(vm, 0) || cpu->protected_mode ||
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
    } else if (host_interrupt_return) {
        rewrite_iret = true;
        if (!dpmi_interrupt_return(vm, false)) {
            cpu->running = false;
            cpu->exit_code = -1;
        }
    } else if (host_exception_return) {
        rewrite_iret = true;
        if (!dpmi_exception_return_native(vm)) {
            cpu->running = false;
            cpu->exit_code = -1;
        }
        if (cpu->running) dos_native_sync_ldt(vm);
    } else if (host_callback_return) {
        dpmi_service_result_t result = dpmi_callback_return(vm, false);
        cpu = vm->cpu;
        rewrite_iret = true;
        if (result == DPMI_SERVICE_INVALID ||
            (result == DPMI_SERVICE_COMPLETE &&
             (cpu->protected_mode || !g_native_interpreter_cpu))) {
            serial_puts("[DOS-NT] Callback PM->RM return failed\n");
            cpu->running = false;
            cpu->exit_code = -1;
        } else if (result == DPMI_SERVICE_COMPLETE) {
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
        const uint32_t status = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
        if (cpu->cs != (uint16_t)regs->iret_cs || cpu->eip != (uint32_t)regs->iret_rip ||
            cpu->ss != (uint16_t)regs->iret_ss || cpu->esp != (uint32_t)regs->iret_rsp ||
            ((cpu->eflags ^ (uint32_t)regs->iret_rflags) & ~status))
            rewrite_iret = true;
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

    if (cpu->running) {
        (void)dos_native_admit_gate_return(vm);
        cpu = vm->cpu;
        rewrite_iret = true;
        if (cpu->running) dos_native_sync_tables(vm);
    }

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
    regs->fs  = cpu->fs;
    regs->gs  = cpu->gs;

    if (rewrite_iret && cpu->running) {
        regs->iret_rip = cpu->eip;
        regs->iret_cs = cpu->cs;
        regs->iret_rflags = (cpu->eflags | FLAGS_FIXED | FLAG_IF) &
                            ~(uint64_t)FLAG_IOPL_MASK;
        regs->iret_rsp = cpu->esp;
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

/* Older operand fixtures build a CPU image by assigning selectors/tables.
 * Import that image explicitly at their dispatch boundary. Cache-lifetime
 * tests use the CPU/JIT entry points directly, without these fixture helpers. */
static void dos_test_import_cs(dos_vm_t *vm)
{
    bool pm = vm->cpu->protected_mode;
    if (pm && vm->cpu->pm_cs_loaded)
        cpu8086_sync_cs(vm->cpu);
    else {
        cpu8086_reset_real_cs(vm->cpu, vm->cpu->cs);
        vm->cpu->protected_mode = false;
    }
    cpu8086_sync_data(vm->cpu);
    vm->cpu->protected_mode = pm;
}

static bool dos_test_run_one(dos_vm_t *vm)
{
    dos_test_import_cs(vm);
    return cpu8086_run_one(vm);
}

static int dos_test_run(dos_vm_t *vm)
{
    dos_test_import_cs(vm);
    return cpu8086_run(vm);
}

static bool dos_test_run_until(dos_vm_t *vm, bool pm, uint16_t cs, uint32_t ip)
{
    dos_test_import_cs(vm);
    return cpu8086_run_until(vm, pm, cs, ip);
}

static bool dos_test_run_until_real(dos_vm_t *vm, uint16_t cs, uint16_t ip)
{
    return dos_test_run_until(vm, false, cs, ip);
}

static int dos_test_jit_decode(dos_vm_t *vm, jit_block_t *block)
{
    dos_test_import_cs(vm);
    return jit_decode_block(vm, block);
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
    dpmi_desc_set_limit(&vm->dpmi.ldt[dpmi_sel_to_index(handler_sel)],
                        use32 ? vm->total_mem_size - 1u : 0xFFFFu);
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
        if (cpu->ss != vm->dpmi.sel_exception_stack ||
            cpu->esp != DPMI_EXCEPTION_STACK_SIZE - 32u ||
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
        if (cpu->ss != vm->dpmi.sel_exception_stack ||
            cpu->esp != DPMI_EXCEPTION_STACK_SIZE - 16u ||
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

static int dos_dpmi_extended_exception_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint16_t code = dpmi_index_to_sel(1), stack = dpmi_index_to_sel(2);
    const uint16_t data = dpmi_index_to_sel(3), alternate = dpmi_index_to_sel(4);
    const uint16_t execute_only = dpmi_index_to_sel(5);
    bool old_emulate = vm->emulate_cpu;
    unsigned checks = 0;
    int failures = 0;
    for (unsigned use32 = 0; use32 < 2u; use32++)
    for (unsigned stack32 = 0; stack32 < 2u; stack32++)
    for (unsigned path = 0; path < 3u; path++)
    for (unsigned scenario = 0; scenario < 23u; scenario++) {
#define EXT_CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        if (failures < 20) { \
            serial_puts("[DPMI-EXT-EXCEPTION] failure line="); serial_putdec(__LINE__); \
            serial_puts(" case="); serial_putdec(scenario); serial_puts(" path="); serial_putdec(path); \
            serial_puts(" use32="); serial_putdec(use32); serial_puts(" ss32="); serial_putdec(stack32); \
            serial_puts("\n"); \
        } \
        failures++; \
    } \
} while (0)
        dpmi_init(vm);
        cpu8086_init(cpu, vm);
        vm->emulate_cpu = true;
        vm->software_int_frame_bytes = 0;
        vm->dpmi.active = true;
        vm->dpmi.is_32bit = use32;
        vm->dpmi.callback_depth = 1; /* Exercise the current SS.B-sized locked cursor. */
        vm->dpmi.virtual_interrupts_enabled = true;
        dos_test_descriptor(&vm->dpmi, code, true, use32);
        dos_test_descriptor(&vm->dpmi, stack, false, stack32);
        dos_test_descriptor(&vm->dpmi, data, false, false);
        dos_test_descriptor(&vm->dpmi, alternate, false, false);
        dos_test_descriptor(&vm->dpmi, execute_only, true, use32);
        vm->dpmi.ldt[5].access &= ~DESC_READABLE;
        dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000);
        dpmi_desc_set_base(&vm->dpmi.ldt[3], 0x40000);
        dpmi_desc_set_base(&vm->dpmi.ldt[4], 0x50000);
        cpu->protected_mode = true;
        cpu->pm_cs_loaded = true;
        cpu->cpl = 3;
        cpu->cs = code; cpu->ss = stack;
        cpu->ds = cpu->es = data; cpu->fs = alternate; cpu->gs = 0;
        cpu->eip = 0x1234;
        cpu->esp = (stack32 ? 0u : 0xA5A50000u) | (scenario == 22u ? 87u : 0x9000u);
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_DF | (scenario == 17u ? FLAG_TF : 0);
        cpu->eax = 0x13572468; cpu->ebx = 0x24681357;
        cpu8086_sync_cs(cpu);
        cpu8086_sync_data(cpu);
        unsigned width = use32 ? 4u : 2u;
        uint8_t vector = scenario == 17u ? 1u : scenario >= 18u && scenario <= 20u ? 14u : 13u;
        cpu->ax = 0x0212; cpu->bl = vector; cpu->cx = code;
        cpu->edx = use32 ? 0x3000u : 0xB6B63000u;
        dos_int31_dpmi(vm);
        EXT_CHECK(!(cpu->eflags & FLAG_CF) && vm->dpmi.exception_extended[vector]);
        cpu->ax = 0x0210; cpu->edx = 0xC7C70000;
        dos_int31_dpmi(vm);
        EXT_CHECK(!(cpu->eflags & FLAG_CF) && cpu->cx == code &&
                  cpu->edx == (use32 ? 0x3000u : 0xC7C73000u));
        cpu->ax = 0x0203; cpu->edx = 0x3000;
        dos_int31_dpmi(vm);
        EXT_CHECK(!(cpu->eflags & FLAG_CF) && vm->dpmi.exception_extended[vector]);
        cpu->ax = 0x0212; cpu->bl = 32;
        dos_int31_dpmi(vm);
        EXT_CHECK((cpu->eflags & FLAG_CF) && cpu->ax == 0x8021);
        cpu->ax = 0x0212; cpu->bl = vector; cpu->cx = data;
        dos_int31_dpmi(vm);
        EXT_CHECK((cpu->eflags & FLAG_CF) && cpu->ax == 0x8022 &&
                  vm->dpmi.exception_vectors[vector].sel == code);
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_DF | (scenario == 17u ? FLAG_TF : 0);

        uint8_t pte_flags = 0;
        if (vector == 14u) {
            for (unsigned i = 0; i < 1024u; i++) {
                dos_mem_write32(vm, 0x80000u + i * 4u, i ? 0u : 0x81007u);
                dos_mem_write32(vm, 0x81000u + i * 4u, i * 4096u | 7u);
            }
            cpu->cr0 |= DOS_CR0_PG | DOS_CR0_WP | 1u;
            cpu->cr3 = 0x80000;
            cpu->cr2 = scenario == 20u ? 0x400000u : 0x9000u;
            pte_flags = scenario == 18u ? 0x9Du : scenario == 19u ? 0xC4u : 0;
            dos_mem_write32(vm, 0x81000u + 9u * 4u, 0x9000u | pte_flags);
        }
        bool host = scenario == 12u || scenario == 13u;
        dpmi_host_wait_t wait = { .psp = vm->current_psp, .depth = 0, .delivering = true };
        vm->dpmi.host_wait = host ? &wait : NULL;
        cpu->eflags = cpu8086_flags_image(cpu);
        cpu8086_state_t original = *cpu;
        uint32_t top = cpu_stack_offset(cpu);
        uint32_t address = 0x20000u + top - (scenario == 22u ? 0u : DPMI_EXCEPTION_EXT_FRAME_SIZE);
        for (unsigned i = 0; i < DPMI_EXCEPTION_EXT_FRAME_SIZE; i++)
            vm->mem[address + i] = 0xA5;
        bool delivered = dpmi_deliver_exception(vm, vector, original.eip, 0x123);
        EXT_CHECK(delivered == (scenario != 22u));
        if (!delivered) {
            EXT_CHECK(cpu->ss == original.ss && cpu->esp == original.esp &&
                      cpu->cs == original.cs && cpu->eip == original.eip && !vm->dpmi.exception_depth);
            bool intact = true;
            for (unsigned i = 0; i < DPMI_EXCEPTION_EXT_FRAME_SIZE; i++)
                intact &= vm->mem[address + i] == 0xA5;
            EXT_CHECK(intact);
            vm->dpmi.host_wait = NULL;
            continue;
        }
        uint16_t host_cs = vm->dpmi.sel_host_code;
        EXT_CHECK(cpu->ss == stack && cpu_stack_offset(cpu) == top - DPMI_EXCEPTION_EXT_FRAME_SIZE &&
                  vm->dpmi.exception_depth == 1 && !vm->dpmi.virtual_interrupts_enabled);
        EXT_CHECK(dos_mem_read16(vm, address) == DPMI_EXCEPTION_RETURN_OFF &&
                  dos_mem_read16(vm, address + width) == host_cs);
        EXT_CHECK(dos_mem_read32(vm, address + 0x20u) ==
                  (DPMI_EXCEPTION_EXT_RETURN_OFF | (use32 ? 0u : (uint32_t)host_cs << 16)) &&
                  dos_mem_read32(vm, address + 0x24u) == (use32 ? host_cs : 0u));
        EXT_CHECK(dos_mem_read32(vm, address + 0x28u) == (scenario == 17u ? 0x8000u : 0x123u) &&
                  dos_mem_read32(vm, address + 0x2Cu) == original.eip &&
                  dos_mem_read32(vm, address + 0x30u) == (code | (host ? 0x10000u : 0u)));
        EXT_CHECK(dos_mem_read32(vm, address + 0x34u) == original.eflags &&
                  dos_mem_read32(vm, address + 0x38u) == original.esp &&
                  dos_mem_read32(vm, address + 0x3Cu) == stack);
        EXT_CHECK(dos_mem_read32(vm, address + 0x40u) == data &&
                  dos_mem_read32(vm, address + 0x44u) == data &&
                  dos_mem_read32(vm, address + 0x48u) == alternate &&
                  !dos_mem_read32(vm, address + 0x4Cu));
        EXT_CHECK(dos_mem_read32(vm, address + 0x50u) == (vector == 14u ? cpu->cr2 : 0u) &&
                  dos_mem_read32(vm, address + 0x54u) == pte_flags);
        if (!use32) {
            bool padding = true;
            for (unsigned i = 16u; i < 32u; i++) padding &= vm->mem[address + i] == 0;
            EXT_CHECK(padding);
        }
        if (scenario == 17u) continue; /* This checks the image, not debug retirement. */

        bool old_return = scenario == 1u || scenario == 3u;
        bool valid = !(scenario >= 4u && scenario <= 11u) && scenario != 21u;
        uint32_t expected_eip = original.eip, expected_esp = original.esp;
        uint16_t expected_es = data, expected_fs = alternate;
        if (scenario == 2u) {
            expected_eip = 0x2345;
            expected_esp = stack32 ? 0x8123u : 0xC7C78123u;
            expected_es = alternate;
            dos_mem_write32(vm, address + 0x2Cu, expected_eip);
            dos_mem_write32(vm, address + 0x38u, expected_esp);
            dos_mem_write32(vm, address + 0x40u, alternate);
            dos_mem_write16(vm, address + 4u * width, 0); /* Unchosen old CS. */
        }
        if (scenario == 3u) {
            expected_eip = 0x3456;
            if (use32) dos_mem_write32(vm, address + 3u * width, expected_eip);
            else dos_mem_write16(vm, address + 3u * width, expected_eip);
            dos_mem_write32(vm, address + 0x30u, 0); /* Unchosen extended CS/ES. */
            dos_mem_write32(vm, address + 0x40u, 0xFFFF);
        }
        if (scenario == 4u) dos_mem_write32(vm, address + 0x40u, 0xFFFF);
        if (scenario == 5u) vm->dpmi.ldt[3].access &= ~DESC_DPL_MASK;
        if (scenario == 6u) vm->dpmi.ldt[4].access &= ~DESC_PRESENT;
        if (scenario == 7u) dos_mem_write32(vm, address + 0x4Cu, execute_only);
        if (scenario == 8u) dos_mem_write32(vm, address + 0x3Cu, code);
        if (scenario == 9u) dos_mem_write32(vm, address + 0x30u, data);
        if (scenario == 10u) {
            dpmi_desc_set_limit(&vm->dpmi.ldt[2], top - 16u);
            cpu8086_sync_segment(cpu, 2);
        }
        if (scenario == 11u) dos_mem_write32(vm, address + 0x34u, original.eflags | FLAG_VM);
        if (host) {
            dos_mem_write32(vm, address + 0x2Cu, 0x4567);
            if (scenario == 13u) {
                expected_eip = 0x4567;
                dos_mem_write32(vm, address + 0x30u, code | 0x50000u);
            }
        }
        if (scenario == 14u) {
            cpu8086_state_t outer = *cpu;
            vm->dpmi.exception_vectors[6].sel = code;
            vm->dpmi.exception_vectors[6].off = 0x4000;
            vm->dpmi.exception_extended[6] = true;
            EXT_CHECK(dpmi_deliver_exception(vm, 6, outer.eip, 0) && vm->dpmi.exception_depth == 2);
            cpu_stack_adjust(cpu, 32u + 2u * width);
            cpu->cs = host_cs; cpu->eip = DPMI_EXCEPTION_EXT_RETURN_OFF + 2u;
            EXT_CHECK(dpmi_exception_return_native(vm) && vm->dpmi.exception_depth == 1 &&
                      cpu->ss == outer.ss && cpu->esp == outer.esp && cpu->cs == outer.cs &&
                      cpu->eip == outer.eip && vm->dpmi.exception_context[0].extended);
        }
        if (scenario == 15u) {
            expected_es = code; expected_fs = 3;
            dos_mem_write32(vm, address + 0x40u, code);
            dos_mem_write32(vm, address + 0x48u, 3);
        }
        if (scenario == 16u) dpmi_desc_set_base(&vm->dpmi.ldt[3], 0x60000);
        if (scenario == 21u) vm->dpmi.exception_context[0].extended = false;
        uint32_t return_off = old_return ? DPMI_EXCEPTION_RETURN_OFF : DPMI_EXCEPTION_EXT_RETURN_OFF;
        bool returned;
        if (path == 2u) {
            uint32_t emit = 0x3000;
            if (!old_return) {
                if (use32 != stack32) vm->mem[emit++] = 0x66;
                vm->mem[emit++] = 0x83; vm->mem[emit++] = 0xC4; vm->mem[emit++] = 0x20;
            }
            vm->mem[emit++] = 0xCB;
            cpu->eip = 0x3000;
            cpu8086_sync_cs(cpu);
            if (!old_return) EXT_CHECK(cpu8086_run_one(vm));
            EXT_CHECK(cpu8086_run_one(vm) && cpu->cs == host_cs && cpu->eip == return_off);
            (void)cpu8086_run_one(vm);
            returned = cpu->running && !vm->dpmi.exception_depth;
        } else {
            cpu_stack_adjust(cpu, (old_return ? 0u : 32u) + 2u * width);
            cpu->cs = host_cs; cpu->eip = return_off + 2u;
            if (path == 1u) {
                if (use32) { cpu_push32(cpu, cpu->eflags); cpu_push32(cpu, host_cs); cpu_push32(cpu, cpu->eip); }
                else { cpu_push16(cpu, cpu->flags); cpu_push16(cpu, host_cs); cpu_push16(cpu, cpu->ip); }
            }
            cpu8086_state_t before = *cpu;
            returned = dpmi_exception_return_frame(vm, path == 1u);
            if (!valid) EXT_CHECK(cpu->ss == before.ss && cpu->esp == before.esp &&
                                  cpu->cs == before.cs && cpu->eip == before.eip && cpu->es == before.es);
        }
        EXT_CHECK(returned == valid);
        if (valid) {
            EXT_CHECK(cpu->ss == stack && cpu->esp == expected_esp && cpu->cs == code &&
                      cpu->eip == expected_eip && cpu->eflags == original.eflags);
            EXT_CHECK(cpu->es == expected_es && cpu->ds == data && cpu->fs == expected_fs && !cpu->gs);
            EXT_CHECK(!vm->dpmi.exception_depth && !vm->dpmi.exception_context[0].extended &&
                      !vm->dpmi.exception_esp_high[0] && vm->dpmi.callback_depth == 1);
            if (scenario == 16u) EXT_CHECK(dpmi_desc_get_base(&cpu->ds_cache.descriptor) == 0x60000);
            if (host) EXT_CHECK(wait.returned && wait.redirected == (scenario == 13u));
        } else EXT_CHECK(vm->dpmi.exception_depth == 1);
        vm->dpmi.host_wait = NULL;
#undef EXT_CHECK
    }
    dpmi_init(vm);
    vm->emulate_cpu = old_emulate;
    serial_puts("[DPMI-EXT-EXCEPTION] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_dpmi_real_exception_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint16_t code = dpmi_index_to_sel(1), stack = dpmi_index_to_sel(2);
    bool emulate = vm->emulate_cpu;
    unsigned checks = 0;
    int failures = 0;
    for (unsigned use32 = 0; use32 < 2u; use32++)
    for (unsigned stack32 = 0; stack32 < 2u; stack32++)
    for (unsigned path = 0; path < 2u; path++)
    for (unsigned scenario = 0; scenario < 14u; scenario++) {
#define REAL_CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        if (failures < 24) { \
            serial_puts("[DPMI-REAL-EXCEPTION] failure line="); serial_putdec(__LINE__); \
            serial_puts(" case="); serial_putdec(scenario); serial_puts(" path="); serial_putdec(path); \
            serial_puts(" use32="); serial_putdec(use32); serial_puts(" ss32="); serial_putdec(stack32); \
            serial_puts("\n"); \
        } \
        failures++; \
    } \
} while (0)
        dpmi_init(vm);
        cpu8086_init(cpu, vm);
        vm->emulate_cpu = true;
        vm->software_int_frame_bytes = 0;
        vm->dpmi.active = true;
        vm->dpmi.is_32bit = use32;
        vm->dpmi.callback_depth = 1;
        vm->dpmi.virtual_interrupts_enabled = true;
        dos_test_descriptor(&vm->dpmi, code, true, use32);
        dos_test_descriptor(&vm->dpmi, stack, false, stack32);
        dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000);
        uint8_t vector = scenario == 7u ? 3u : scenario == 9u ? 14u : 6u;
        cpu->ax = 0x0211; cpu->bl = vector; cpu->edx = 0xCAFE0000;
        dos_int31_dpmi(vm);
        REAL_CHECK(!(cpu->flags & FLAG_CF) && cpu->cx == vm->dpmi.sel_host_code &&
                   cpu->edx == ((use32 ? 0u : 0xCAFE0000u) | DPMI_PM_EXCEPTION_BASE_OFF |
                                vector * DPMI_PM_EXCEPTION_STUB_SIZE));
        uint16_t host = cpu->cx;
        cpu->ax = 0x0213; cpu->cx = code; cpu->edx = 0xA5A53000;
        if (use32) cpu->edx = 0x3000;
        dos_int31_dpmi(vm);
        REAL_CHECK(!(cpu->flags & FLAG_CF) && vm->dpmi.real_exception_vectors[vector].sel == code &&
                   vm->dpmi.real_exception_vectors[vector].off == 0x3000 &&
                   !vm->dpmi.exception_vectors[vector].sel && !vm->dpmi.exception_extended[vector]);
        cpu->ax = 0x0211; cpu->edx = 0xBEEF0000;
        dos_int31_dpmi(vm);
        REAL_CHECK(!(cpu->flags & FLAG_CF) && cpu->cx == code &&
                   cpu->edx == (use32 ? 0x3000u : 0xBEEF3000u));
        cpu->ax = 0x0213; cpu->cx = stack;
        dos_int31_dpmi(vm);
        REAL_CHECK((cpu->flags & FLAG_CF) && cpu->ax == 0x8022 &&
                   vm->dpmi.real_exception_vectors[vector].sel == code);
        cpu->ax = 0x0213; cpu->bl = 32;
        dos_int31_dpmi(vm);
        REAL_CHECK((cpu->flags & FLAG_CF) && cpu->ax == 0x8021);
        cpu->ax = 0x0211;
        dos_int31_dpmi(vm);
        REAL_CHECK((cpu->flags & FLAG_CF) && cpu->ax == 0x8021);

        uint16_t segments[] = {0x1200, 0x1000, 0x4000, 0x2300, 0x3400, 0x4500};
        for (unsigned s = 0; s < 6u; s++) cpu8086_load_real_segment(cpu, s, segments[s]);
        cpu->eip = 0x2345; cpu->esp = 0xBEEF8F00;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_DF;
        cpu->cr0 = 0x10; cpu->cr3 = 0xABCDE000; cpu->cr2 = 0x9876;
        cpu->eax = 0x13572468;
        dpmi_desc_set_limit(&cpu->ds_cache.descriptor, 0x1FFFF);
        vm->dpmi.suspended_stack = (dpmi_stack_t){stack, (stack32 ? 0u : 0xABCD0000u) | 0x9000u};
        vm->dpmi.real_mode_stack = (dpmi_stack_t){0x5100, 0x800};
        vm->dpmi.real_mode_paging = (dpmi_paging_t){0x10, 0x765000};
        vm->dpmi.suspended_paging = (dpmi_paging_t){1, 0x80000};
        dpmi_real_exception_t real = { .cpu = *cpu, .real_stack = vm->dpmi.real_mode_stack,
            .protected_stack = vm->dpmi.suspended_stack, .real_paging = vm->dpmi.real_mode_paging,
            .pte = 0x9D };
        bool host_fault = scenario == 10u || scenario == 11u;
        dpmi_host_wait_t wait = { .psp = vm->current_psp, .depth = 0, .delivering = host_fault };
        vm->dpmi.host_wait = &wait;
        cpu->cr0 = vm->dpmi.suspended_paging.cr0;
        cpu->cr3 = vm->dpmi.suspended_paging.cr3;
        if (scenario == 12u) vm->dpmi.suspended_stack.esp = 87;
        if (scenario == 13u) vm->dpmi.ldt[1].access &= ~DESC_PRESENT;
        bool delivered = dpmi_deliver_exception_frame(vm, vector, real.cpu.eip, 0x123, &real);
        REAL_CHECK(delivered == (scenario < 12u));
        if (!delivered) {
            REAL_CHECK(!cpu->protected_mode && cpu->ss == real.cpu.ss && cpu->esp == real.cpu.esp &&
                       !vm->dpmi.exception_depth);
            vm->dpmi.host_wait = NULL;
            continue;
        }
        uint32_t address = 0x29000u - DPMI_EXCEPTION_EXT_FRAME_SIZE;
        REAL_CHECK(cpu->protected_mode && !cpu->ds && !cpu->es && !cpu->fs && !cpu->gs &&
                   !(cpu->eflags & FLAG_VM) && cpu->ss == stack && cpu->cs == code &&
                   cpu_stack_offset(cpu) == 0x9000u - DPMI_EXCEPTION_EXT_FRAME_SIZE);
        REAL_CHECK(dos_mem_read32(vm, address + 0x2Cu) == real.cpu.eip &&
                   dos_mem_read32(vm, address + 0x30u) == (real.cpu.cs | (host_fault ? 0x10000u : 0u)) &&
                   dos_mem_read32(vm, address + 0x34u) == (real.cpu.eflags | FLAG_VM) &&
                   dos_mem_read32(vm, address + 0x38u) == real.cpu.esp);
        REAL_CHECK(dos_mem_read32(vm, address + 0x3Cu) == real.cpu.ss &&
                   dos_mem_read32(vm, address + 0x40u) == real.cpu.es &&
                   dos_mem_read32(vm, address + 0x44u) == real.cpu.ds &&
                   dos_mem_read32(vm, address + 0x48u) == real.cpu.fs &&
                   dos_mem_read32(vm, address + 0x4Cu) == real.cpu.gs);
        REAL_CHECK(dos_mem_read32(vm, address + 0x50u) == (vector == 14u ? real.cpu.cr2 : 0u) &&
                   dos_mem_read32(vm, address + 0x54u) == (vector == 14u ? 0x9Du : 0u));
        bool padding = true;
        for (unsigned b = 2u * (use32 ? 4u : 2u); b < 32u; b++) padding &= vm->mem[address + b] == 0;
        REAL_CHECK(padding && vm->dpmi.exception_context[0].real == &real);

        uint32_t wanted_eip = real.cpu.eip, wanted_esp = real.cpu.esp;
        bool valid = scenario != 2u && scenario != 3u && scenario != 4u;
        if (scenario == 1u) {
            wanted_eip = 0x3456; wanted_esp = 0xCAFE8123;
            dos_mem_write32(vm, address + 0x2Cu, wanted_eip);
            dos_mem_write32(vm, address + 0x38u, wanted_esp);
            dos_mem_write32(vm, address + 0x44u, 0x6600);
        }
        if (scenario == 3u) dos_mem_write32(vm, address + 0x2Cu, 0x10000);
        if (scenario == 4u) {
            dpmi_desc_set_limit(&vm->dpmi.ldt[2], 0x8FF0);
            cpu8086_sync_segment(cpu, 2);
        }
        if (scenario == 5u) {
            dos_mem_write32(vm, address + 0x34u, FLAGS_FIXED | FLAG_IF);
            dos_mem_write32(vm, address + 0x30u, code);
            dos_mem_write32(vm, address + 0x3Cu, stack);
            for (unsigned i = 0; i < 4u; i++) dos_mem_write32(vm, address + 0x40u + i * 4u, 0);
        }
        if (scenario == 6u) {
            vm->dpmi.exception_vectors[6].sel = code;
            vm->dpmi.exception_vectors[6].off = 0x3500;
            vm->dpmi.exception_extended[6] = true;
            uint32_t outer_esp = cpu->esp;
            REAL_CHECK(dpmi_deliver_exception(vm, 6, 0x3000, 0));
            cpu_stack_adjust(cpu, 32u + 2u * (use32 ? 4u : 2u));
            cpu->cs = host; cpu->eip = DPMI_EXCEPTION_EXT_RETURN_OFF + 2u;
            REAL_CHECK(dpmi_exception_return_frame(vm, false) && cpu->esp == outer_esp &&
                       vm->dpmi.exception_depth == 1 && vm->dpmi.exception_context[0].real == &real);
        }
        if (host_fault) {
            dos_mem_write32(vm, address + 0x2Cu, 0x4567);
            if (scenario == 11u) {
                wanted_eip = 0x4567;
                dos_mem_write32(vm, address + 0x30u, real.cpu.cs | 0x50000u);
            }
        }
        cpu->eax = 0x24681357;
        bool returned;
        unsigned width = use32 ? 4u : 2u;
        if (scenario == 7u || scenario == 8u) {
            dos_mem_write32(vm, 3u * 4u, 0x08000000);
            vm->mem[0x8000] = 0xCF;
            cpu->cs = host;
            cpu->eip = DPMI_PM_EXCEPTION_BASE_OFF + vector * DPMI_PM_EXCEPTION_STUB_SIZE + 2u;
            if (path == 1u) {
                if (use32) { cpu_push32(cpu, cpu->eflags); cpu_push32(cpu, host); cpu_push32(cpu, cpu->eip); }
                else { cpu_push16(cpu, cpu->flags); cpu_push16(cpu, host); cpu_push16(cpu, cpu->ip); }
            }
            vm->software_int_frame_bytes = path == 1u ? 3u * width : 0;
            returned = dpmi_dispatch_default_exception(vm, vector, vm->software_int_frame_bytes);
            if (scenario == 7u) {
                REAL_CHECK(cpu->cs == 0x800 && cpu->esp == real.cpu.esp - 6u &&
                           dos_mem_read16(vm, 0x40000u + cpu->sp) == wanted_eip);
                (void)cpu8086_run_one(vm);
            }
        } else {
            bool legacy = scenario == 2u;
            cpu_stack_adjust(cpu, (legacy ? 0u : 32u) + 2u * width);
            cpu->cs = host;
            cpu->eip = (legacy ? DPMI_EXCEPTION_RETURN_OFF : DPMI_EXCEPTION_EXT_RETURN_OFF) + 2u;
            if (path == 1u) {
                if (use32) { cpu_push32(cpu, cpu->eflags); cpu_push32(cpu, host); cpu_push32(cpu, cpu->eip); }
                else { cpu_push16(cpu, cpu->flags); cpu_push16(cpu, host); cpu_push16(cpu, cpu->ip); }
            }
            cpu8086_state_t before = *cpu;
            returned = dpmi_exception_return_frame(vm, path == 1u);
            if (!valid) REAL_CHECK(cpu->ss == before.ss && cpu->esp == before.esp &&
                                   cpu->cs == before.cs && cpu->eip == before.eip && cpu->protected_mode);
        }
        REAL_CHECK(returned == valid);
        if (valid) {
            REAL_CHECK(!vm->dpmi.exception_depth && !vm->dpmi.exception_context[0].real &&
                       vm->dpmi.callback_depth == 1 && wait.returned &&
                       wait.redirected == (scenario == 11u));
            if (scenario == 8u) REAL_CHECK(!cpu->running && cpu->exit_code == -1);
            else if (scenario == 5u) REAL_CHECK(cpu->protected_mode && cpu->cs == code && cpu->ss == stack);
            else {
                REAL_CHECK(!cpu->protected_mode && cpu->cs == real.cpu.cs && cpu->eip == wanted_eip &&
                           cpu->ss == real.cpu.ss && cpu->esp == wanted_esp && cpu->eax == 0x24681357);
                REAL_CHECK(cpu->cr0 == real.cpu.cr0 && cpu->cr3 == real.cpu.cr3 &&
                           cpu->eflags == real.cpu.eflags && cpu->ds == (scenario == 1u ? 0x6600 : real.cpu.ds) &&
                           dpmi_desc_get_limit(&cpu->ds_cache.descriptor) == 0x1FFFF);
            }
            REAL_CHECK(vm->dpmi.real_mode_stack.ss == real.real_stack.ss &&
                       vm->dpmi.real_mode_stack.esp == real.real_stack.esp &&
                       vm->dpmi.real_mode_paging.cr3 == real.real_paging.cr3 &&
                       vm->dpmi.suspended_stack.esp == real.protected_stack.esp &&
                       vm->dpmi.suspended_paging.cr3 == 0x80000);
        } else REAL_CHECK(vm->dpmi.exception_depth == 1);
        vm->dpmi.host_wait = NULL;
        vm->dpmi.exception_context[0].real = NULL;
#undef REAL_CHECK
    }
    dpmi_init(vm);
    vm->emulate_cpu = emulate;
    dos_init_ivt(vm);
    serial_puts("[DPMI-REAL-EXCEPTION] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_dpmi_locked_stack_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint16_t code_sel = dpmi_index_to_sel(1);
    const uint16_t stack_sel = dpmi_index_to_sel(2);
    const uint16_t repair_sel = dpmi_index_to_sel(3);
    unsigned checks = 0;
    int failures = 0;
    for (unsigned use32 = 0; use32 < 2; use32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++) {
        unsigned width = use32 ? 4u : 2u;
        dpmi_init(vm);
        dos_test_descriptor(&vm->dpmi, code_sel, true, use32);
        dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
        dos_test_descriptor(&vm->dpmi, repair_sel, false, stack32);
        dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000);
        dpmi_desc_set_base(&vm->dpmi.ldt[3], 0x30000);
        vm->dpmi.active = true;
        vm->dpmi.is_32bit = use32;
        cpu8086_init(cpu, vm);
        cpu->protected_mode = true;
        cpu->cs = code_sel;
        cpu->ss = stack_sel;
        cpu->esp = 2;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_DF;
        cpu8086_sync_cs(cpu);
        vm->dpmi.exception_vectors[12].sel = code_sel;
        vm->dpmi.exception_vectors[12].off = 0x2000;
        for (unsigned i = 0; i < 16; i++) {
            vm->mem[0x20000u + i] = 0xA5;
            vm->mem[0x2FFF0u + i] = 0x5A;
        }
        bool ok = dpmi_deliver_exception(vm, 12, 0x1000, 0);
        uint16_t locked_sel = vm->dpmi.sel_exception_stack;
        uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
        if (!locked_sel || cpu->ss != locked_sel ||
            cpu->esp != DPMI_EXCEPTION_STACK_SIZE - 8u * width ||
            cpu_stack_addr32(cpu) != (use32 != 0)) ok = false;
        for (unsigned i = 0; i < 16; i++)
            if (vm->mem[0x20000u + i] != 0xA5 || vm->mem[0x2FFF0u + i] != 0x5A)
                ok = false;
        if (use32) {
            if (dos_mem_read32(vm, frame + 24u) != 2 ||
                dos_mem_read32(vm, frame + 28u) != stack_sel) ok = false;
            dos_mem_write32(vm, frame + 12u, 0x1234);
            dos_mem_write32(vm, frame + 24u, 0x800);
            dos_mem_write32(vm, frame + 28u, repair_sel);
        } else {
            if (dos_mem_read16(vm, frame + 12u) != 2 ||
                dos_mem_read16(vm, frame + 14u) != stack_sel) ok = false;
            dos_mem_write16(vm, frame + 6u, 0x1234);
            dos_mem_write16(vm, frame + 12u, 0x800);
            dos_mem_write16(vm, frame + 14u, repair_sel);
        }
        cpu_stack_adjust(cpu, 2u * width); /* FAR return to the host stub */
        cpu->cs = vm->dpmi.sel_host_code;
        cpu->eip = DPMI_EXCEPTION_RETURN_OFF + 2u;
        if (!dpmi_exception_return_native(vm) || cpu->ss != repair_sel ||
            cpu->esp != 0x800 || cpu->eip != 0x1234 || cpu->cs != code_sel ||
            cpu->eflags != (FLAGS_FIXED | FLAG_IF | FLAG_DF) ||
            vm->dpmi.exception_depth) ok = false;
        if (!ok) failures++;
        checks++;

        /* Nested delivery stays on the handler's current stack, including
         * a client-selected replacement, and must fail before any write. */
        for (unsigned scenario = 0; scenario < 12; scenario++) {
            dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
            dpmi_descriptor_t *descriptor = &vm->dpmi.ldt[2];
            dpmi_desc_set_base(descriptor, 0x20000);
            cpu->ss = stack_sel;
            cpu->esp = 0x9000;
            cpu->cs = code_sel;
            cpu->eip = 0x2100;
            cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_DF;
            vm->dpmi.exception_depth = 1;
            vm->dpmi.callback_depth = 0;
            vm->dpmi.virtual_interrupts_enabled = true;
            bool allowed = scenario == 0 || scenario == 1 || scenario == 11;
            if (scenario == 1) {
                descriptor->access |= 4u; /* expand-down stack */
                dpmi_desc_set_limit(descriptor, 0x8000);
            }
            if (scenario == 2) cpu->esp = 8u * width - 1u;
            if (scenario == 3) dpmi_desc_set_limit(descriptor, 0x8FFEu);
            if (scenario == 4) descriptor->access &= ~DESC_WRITABLE;
            if (scenario == 5) descriptor->access &= ~DESC_PRESENT;
            if (scenario == 6) descriptor->access |= DESC_CODE;
            if (scenario == 7) {
                descriptor->access |= 4u;
                dpmi_desc_set_limit(descriptor, 0x9000u - 8u * width);
            }
            if (scenario == 8) dpmi_desc_set_base(descriptor, vm->total_mem_size);
            if (scenario == 9) dpmi_desc_set_base(descriptor, UINT32_MAX - 4u);
            if (scenario == 10) descriptor->access &= ~DESC_DPL_MASK;
            if (scenario == 11) {
                vm->dpmi.exception_depth = 0;
                vm->dpmi.callback_depth = 1;
            }
            cpu8086_sync_segment(cpu, 2);
            cpu8086_state_t before = *cpu;
            unsigned depth = vm->dpmi.exception_depth;
            for (unsigned i = 0; i < 64; i++) vm->mem[0x28FC0 + i] = 0xC3;
            bool delivered = dpmi_deliver_exception(vm, 12, 0x2100, 0);
            ok = delivered == allowed && cpu->ss == stack_sel;
            if (allowed) {
                uint32_t nested = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
                if (cpu->esp != before.esp - 8u * width ||
                    vm->dpmi.exception_depth != depth + 1u ||
                    vm->dpmi.virtual_interrupts_enabled ||
                    (use32 ? dos_mem_read32(vm, nested + 24u)
                           : dos_mem_read16(vm, nested + 12u)) != before.esp)
                    ok = false;
            } else {
                const uint8_t *a = (const uint8_t *)cpu;
                const uint8_t *b = (const uint8_t *)&before;
                for (unsigned i = 0; i < sizeof(before); i++) if (a[i] != b[i]) ok = false;
                if (vm->dpmi.exception_depth != depth ||
                    !vm->dpmi.virtual_interrupts_enabled) ok = false;
                for (unsigned i = 0; i < 64; i++)
                    if (vm->mem[0x28FC0 + i] != 0xC3) ok = false;
            }
            if (!ok) failures++;
            checks++;
        }
        /* Return frames also require a writable, present data stack with
         * a complete, non-wrapping span; rejection must not consume it. */
        for (unsigned scenario = 0; scenario < 6; scenario++) {
            dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
            dpmi_descriptor_t *descriptor = &vm->dpmi.ldt[2];
            cpu->ss = stack_sel;
            cpu->esp = 0x9000;
            cpu->cs = vm->dpmi.sel_host_code;
            cpu->eip = DPMI_EXCEPTION_RETURN_OFF + 2u;
            vm->dpmi.exception_depth = 1;
            if (scenario == 0) dpmi_desc_set_limit(descriptor, 0x9000u + 6u * width - 2u);
            if (scenario == 1) descriptor->access &= ~DESC_WRITABLE;
            if (scenario == 2) descriptor->access &= ~DESC_PRESENT;
            if (scenario == 3) dpmi_desc_set_base(descriptor, vm->total_mem_size);
            if (scenario == 4) {
                descriptor->flags_lim &= ~DESC_32BIT;
                cpu->esp = 0xFFFE;
            }
            if (scenario == 5) cpu->ss = 0;
            cpu8086_sync_segment(cpu, 2);
            uint32_t old_sp = cpu->esp;
            if (dpmi_exception_return_native(vm) || cpu->esp != old_sp ||
                vm->dpmi.exception_depth != 1) failures++;
            checks++;
        }
    }
    vm->dpmi.callback_depth = 0;
    vm->dpmi.exception_depth = 0;
    serial_puts("[DOS-EXSTACK] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_dpmi_paged_frame_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint16_t code_sel = dpmi_index_to_sel(1), stack_sel = dpmi_index_to_sel(2);
    int failures = 0;
    unsigned checks = 0;
    bool emulate = vm->emulate_cpu;
    for (unsigned use32 = 0; use32 < 2; use32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned kind = 0; kind < 3; kind++)
    for (unsigned split = 0; split < (kind == 2 ? 2u : 1u); split++)
    for (unsigned policy = 0; policy < 14; policy++) {
        bool irq = kind == 1, extended = kind == 2;
        dpmi_init(vm);
        cpu8086_init(cpu, vm);
        dos_test_descriptor(&vm->dpmi, code_sel, true, use32);
        dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
        vm->dpmi.active = true;
        vm->dpmi.is_32bit = use32;
        vm->emulate_cpu = true;
        uint16_t host = dpmi_get_host_code_selector(vm);
        unsigned width = use32 ? 4u : 2u;
        unsigned size = extended ? DPMI_EXCEPTION_EXT_FRAME_SIZE : (irq ? 3u : 8u) * width;
        unsigned first_size = split ? 47u : 1u;
        uint32_t linear = (high ? 0x800000u : 0x90000u) - first_size;
        uint32_t first_physical = 0x41000u - first_size;
        uint32_t original_sp = stack32 ? 0x9000u : 0xCAFE9000u;
        dpmi_descriptor_t *descriptor = &vm->dpmi.ldt[2];
        dpmi_desc_set_base(descriptor, linear - (0x9000u - size));
        if (policy == 8) dpmi_desc_set_limit(descriptor, 0x8FFEu);
        if (policy == 10) dpmi_desc_set_base(descriptor, UINT32_MAX - 4u);
        if (policy == 12 || policy == 13) {
            descriptor->access |= 4u;
            dpmi_desc_set_limit(descriptor, 0x9000u - size - (policy == 12));
        }
        cpu->protected_mode = cpu->pm_cs_loaded = true;
        cpu->cs = code_sel;
        cpu->eip = 0x1234;
        cpu->ss = stack_sel;
        cpu->esp = original_sp;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_DF | FLAG_CF | FLAG_IOPL_MASK;
        cpu8086_sync_cs(cpu);
        cpu8086_sync_segment(cpu, 2);
        if (policy == 9) cpu->ss_cache.valid = false;
        dpmi_descriptor_t loaded = *descriptor;
        if (policy == 11) {
            dpmi_desc_set_base(descriptor, 0xA0000);
            descriptor->access = 0;
            descriptor->flags_lim ^= DESC_32BIT;
        }
        vm->dpmi.exception_depth = 1;
        vm->dpmi.virtual_interrupts_enabled = true;
        vm->dpmi.exception_vectors[13].sel = code_sel;
        vm->dpmi.exception_vectors[13].off = 0x2000;
        vm->dpmi.exception_extended[13] = extended;
        for (unsigned i = 0; i < 0x3000; i++) vm->mem[0x10000 + i] = 0;
        uint32_t pde1 = 0x10000u + (linear >> 22) * 4u;
        uint32_t pde2 = 0x10000u + ((linear + size - 1u) >> 22) * 4u;
        uint32_t pte1 = 0x11000u + ((linear >> 12) & 1023u) * 4u;
        uint32_t pte2 = (high ? 0x12000u : 0x11000u) +
                        (((linear + size - 1u) >> 12) & 1023u) * 4u;
        dos_mem_write32(vm, pde1, 0x11007);
        dos_mem_write32(vm, pde2, (high ? 0x12000u : 0x11000u) | 7u);
        dos_mem_write32(vm, pte1, 0x40007);
        dos_mem_write32(vm, pte2, 0x60007);
        if (policy >= 1 && policy <= 3)
            dos_mem_write32(vm, pte2, 0x60007u & ~(1u << (policy - 1u)));
        if (policy >= 4 && policy <= 6)
            dos_mem_write32(vm, pde2, dos_mem_read32(vm, pde2) & ~(1u << (policy - 4u)));
        if (policy == 7) dos_mem_write32(vm, pte2, vm->total_mem_size | 7u);
        uint32_t entries[] = { pde1, pde2, pte1, pte2 }, before_entries[4];
        for (unsigned i = 0; i < 4; i++) before_entries[i] = dos_mem_read32(vm, entries[i]);
        for (unsigned i = 0; i < 160; i++) {
            vm->mem[0x40FC0 + i] = vm->mem[0x60000 + i] = 0xA5;
            if (!high) vm->mem[linear - 16u + i] = 0x5A;
        }
        cpu->cr0 = DOS_CR0_PG | DOS_CR0_WP | 1u;
        cpu->cr3 = 0x10000;
        cpu8086_state_t before = *cpu;
        bool allowed = policy == 0 || policy == 11 || policy == 12;
        bool delivered = irq ? dpmi_deliver_locked_interrupt(vm, code_sel, 0x2000, 0x1234, true)
                             : dpmi_deliver_exception(vm, 13, 0x1234, 0x123);
        bool ok = delivered == (irq || allowed);
        if (allowed) {
            if (cpu->ss != stack_sel || cpu->esp != original_sp - size ||
                cpu->cs != code_sel || cpu->eip != 0x2000 ||
                vm->dpmi.exception_depth != (irq ? 1 : 2) ||
                vm->dpmi.interrupt_depth != irq || vm->dpmi.virtual_interrupts_enabled ||
                dpmi_desc_get_base(&cpu->ss_cache.descriptor) != dpmi_desc_get_base(&loaded) ||
                cpu_stack_addr32(cpu) != (stack32 != 0)) ok = false;
            const uint32_t fields[] = { irq ? DPMI_INTERRUPT_RETURN_OFF : DPMI_EXCEPTION_RETURN_OFF,
                host, irq ? before.eflags : 0x123, 0x1234, code_sel,
                before.eflags, original_sp, stack_sel };
            uint8_t expected[DPMI_EXCEPTION_EXT_FRAME_SIZE] = {0};
            for (unsigned i = 0; i < (irq ? 3u : 8u) * width; i++)
                expected[i] = fields[i / width] >> (8u * (i % width));
            if (extended) {
                const uint32_t extra[] = {
                    DPMI_EXCEPTION_EXT_RETURN_OFF | (use32 ? 0u : (uint32_t)host << 16),
                    use32 ? host : 0, 0x123, 0x1234, code_sel,
                    before.eflags, original_sp, stack_sel, 0, 0, 0, 0, 0, 0
                };
                for (unsigned i = 0; i < 56; i++)
                    expected[32u + i] = extra[i / 4u] >> (8u * (i % 4u));
            }
            for (unsigned i = 0; i < size; i++)
                if (vm->mem[i < first_size ? first_physical + i : 0x60000u + i - first_size] !=
                    expected[i]) ok = false;
            for (unsigned i = 0; i < 4; i++)
                if (dos_mem_read32(vm, entries[i]) != (before_entries[i] | (i < 2 ? 0x20u : 0x60u)))
                    ok = false;
        } else {
            if (irq) { before.running = false; before.exit_code = -1; }
            const uint8_t *a = (const uint8_t *)cpu, *b = (const uint8_t *)&before;
            for (unsigned i = 0; i < sizeof(before); i++) if (a[i] != b[i]) ok = false;
            if (vm->dpmi.exception_depth != 1 || vm->dpmi.interrupt_depth ||
                !vm->dpmi.virtual_interrupts_enabled) ok = false;
            for (unsigned i = 0; i < 4; i++)
                if (dos_mem_read32(vm, entries[i]) != before_entries[i]) ok = false;
        }
        for (unsigned i = 0; i < 160; i++) {
            uint32_t first = 0x40FC0u + i;
            if ((!allowed || first < first_physical || first >= 0x41000u) &&
                vm->mem[first] != 0xA5) ok = false;
            if ((!allowed || i >= size - first_size) && vm->mem[0x60000u + i] != 0xA5) ok = false;
            if (!high && vm->mem[linear - 16u + i] != 0x5A) ok = false;
        }
        if (!ok) {
            failures++;
            serial_puts("[DPMI-PAGED-FRAME] delivery failed case="); serial_putdec(checks);
            serial_puts("\n");
        }
        checks++;
        if (!allowed) continue;

        /* Return admission reads through paging, including a read-only PTE.
         * An unavailable second page must not consume the handler frame. */
        *descriptor = loaded;
        unsigned consumed = (extended ? 32u : 0u) + (irq ? 3u : 2u) * width;
        cpu_stack_adjust(cpu, consumed);
        cpu->cs = host;
        cpu->eip = (irq ? DPMI_INTERRUPT_RETURN_OFF : extended ?
                    DPMI_EXCEPTION_EXT_RETURN_OFF : DPMI_EXCEPTION_RETURN_OFF) + 2u;
        if (irq) {
            const dpmi_stack_t cursor = { cpu->ss, cpu->esp };
            const uint32_t private_fields[] = { cpu->eip, host, before.eflags };
            if (!dpmi_push_stack_frame(vm, &cursor, private_fields, 3)) failures++;
        }
        for (unsigned mapped = 0; mapped < 2; mapped++) {
            cpu8086_state_t return_before = *cpu;
            unsigned ex_depth = vm->dpmi.exception_depth, int_depth = vm->dpmi.interrupt_depth;
            for (unsigned i = 0; i < 4; i++)
                dos_mem_write32(vm, entries[i], before_entries[i] & ~0x60u);
            dos_mem_write32(vm, pte1, 0x40005);
            dos_mem_write32(vm, pte2, mapped ? 0x60005 : 0x60004);
            bool returned = irq ? dpmi_interrupt_return(vm, true) : dpmi_exception_return_native(vm);
            ok = returned == (mapped != 0);
            if (!mapped) {
                const uint8_t *a = (const uint8_t *)cpu, *b = (const uint8_t *)&return_before;
                for (unsigned i = 0; i < sizeof(return_before); i++) if (a[i] != b[i]) ok = false;
                if (vm->dpmi.exception_depth != ex_depth || vm->dpmi.interrupt_depth != int_depth ||
                    dos_mem_read32(vm, pte1) != 0x40005 || dos_mem_read32(vm, pte2) != 0x60004)
                    ok = false;
            } else if (cpu->ss != stack_sel || cpu->esp != original_sp ||
                       cpu->cs != code_sel || cpu->eip != 0x1234 || cpu->eflags != before.eflags ||
                       vm->dpmi.exception_depth != 1 || vm->dpmi.interrupt_depth ||
                       !vm->dpmi.virtual_interrupts_enabled ||
                       dos_mem_read32(vm, pte2) != 0x60025 ||
                       dos_mem_read32(vm, pte1) != (irq || consumed < first_size ? 0x40025u : 0x40005u))
                ok = false;
            if (!ok) {
                failures++;
                serial_puts("[DPMI-PAGED-FRAME] return failed case="); serial_putdec(checks);
                serial_puts("\n");
            }
            checks++;
        }
    }
    cpu->cr0 = cpu->cr3 = 0;
    vm->emulate_cpu = emulate;
    dpmi_init(vm);
    serial_puts("[DPMI-PAGED-FRAME] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_dpmi_private_frame_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint16_t code_sel = dpmi_index_to_sel(1), stack_sel = dpmi_index_to_sel(2);
    unsigned checks = 0;
    int failures = 0;
    for (unsigned use32 = 0; use32 < 2; use32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned missing = 0; missing < 2; missing++) {
        dpmi_init(vm);
        cpu8086_init(cpu, vm);
        dos_test_descriptor(&vm->dpmi, code_sel, true, use32);
        dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
        vm->dpmi.active = true;
        vm->dpmi.is_32bit = use32;
        unsigned width = use32 ? 4u : 2u, size = 3u * width;
        uint32_t linear = high ? 0x7FFFFFu : 0x8FFFFu;
        dpmi_desc_set_base(&vm->dpmi.ldt[2], linear - (0x9000u - size));
        cpu->protected_mode = cpu->pm_cs_loaded = true;
        cpu->cs = code_sel;
        cpu->eip = 0x1000;
        cpu->ss = stack_sel;
        cpu->esp = stack32 ? 0x9000 : 0xBEEF9000;
        cpu->eax = 0xA5A50400;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_DF | FLAG_IOPL_MASK;
        cpu8086_sync_cs(cpu);
        cpu8086_sync_segment(cpu, 2);
        for (unsigned i = 0; i < 0x4000; i++) vm->mem[0x10000 + i] = 0;
        uint32_t pde1 = 0x10000u + (linear >> 22) * 4u;
        uint32_t pde2 = 0x10000u + ((linear + size - 1u) >> 22) * 4u;
        uint32_t pte1 = 0x11000u + ((linear >> 12) & 1023u) * 4u;
        uint32_t pte2 = (high ? 0x12000u : 0x11000u) +
                        (((linear + size - 1u) >> 12) & 1023u) * 4u;
        dos_mem_write32(vm, 0x10000, high ? 0x13007 : 0x11007);
        dos_mem_write32(vm, high ? 0x13004 : 0x11004, 0x1007);
        dos_mem_write32(vm, pde1, 0x11007);
        dos_mem_write32(vm, pde2, (high ? 0x12000u : 0x11000u) | 7u);
        dos_mem_write32(vm, pte1, 0x40007);
        dos_mem_write32(vm, pte2, missing ? 0x60006 : 0x60007);
        vm->mem[0x1000] = 0xCD;
        vm->mem[0x1001] = 0x31;
        for (unsigned i = 0; i < 32; i++) {
            vm->mem[0x40FF0 + i] = vm->mem[0x60000 + i] = 0xA5;
            if (!high) vm->mem[linear - 4u + i] = 0x5A;
        }
        cpu->cr0 = DOS_CR0_PG | DOS_CR0_WP | 1u;
        cpu->cr3 = 0x10000;
        uint32_t sp = cpu->esp, flags = cpu->eflags;
        cpu_event_fault_t fault = {0};
        cpu->delivery_fault = &fault;
        (void)cpu8086_run_one(vm);
        cpu->delivery_fault = NULL;
        bool ok = cpu->running && cpu->esp == sp && cpu->cs == code_sel;
        if (missing) {
            if (!fault.raised || fault.vector != 14 || fault.error != 6 ||
                fault.return_eip != 0x1000 || cpu->cr2 != linear + 1u ||
                cpu->eax != 0xA5A50400 || cpu->eflags != flags ||
                dos_mem_read32(vm, pte1) != 0x40007 ||
                dos_mem_read32(vm, pte2) != 0x60006) ok = false;
        } else {
            if (fault.raised || cpu->eip != 0x1002 || cpu->ax != 0x005A ||
                (cpu->flags & FLAG_CF) || dos_mem_read32(vm, pte1) != 0x40067 ||
                dos_mem_read32(vm, pte2) != 0x60067) ok = false;
            const uint32_t fields[] = { 0x1002, code_sel, flags };
            for (unsigned i = 0; i < size; i++)
                if (vm->mem[i ? 0x60000u + i - 1u : 0x40FFFu] !=
                    (uint8_t)(fields[i / width] >> (8u * (i % width)))) ok = false;
        }
        for (unsigned i = 0; i < 32; i++) {
            if ((missing || i != 15) && vm->mem[0x40FF0u + i] != 0xA5) ok = false;
            if ((missing || i >= size - 1u) && vm->mem[0x60000u + i] != 0xA5) ok = false;
            if (!high && vm->mem[linear - 4u + i] != 0x5A) ok = false;
        }
        if (!ok) {
            failures++;
            serial_puts("[DPMI-PRIVATE-FRAME] failed case="); serial_putdec(checks);
            serial_puts("\n");
        }
        checks++;
    }
    cpu->cr0 = cpu->cr3 = 0;
    dpmi_init(vm);
    serial_puts("[DPMI-PRIVATE-FRAME] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_dpmi_software_frame_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    enum { VALID, PAGE_ABSENT, PAGE_RO, PAGE_SUPERVISOR, PDE_ABSENT,
           PDE_RO, PDE_SUPERVISOR, SHORT_SS, EXPAND_VALID, EXPAND_INVALID,
           INVALID_SS, EDITED_SS, CODE_ABSENT, CODE_DATA, CODE_SHORT,
           CODE_EXECUTE_ONLY, POLICIES };
    const uint16_t cs = dpmi_index_to_sel(1), ss = dpmi_index_to_sel(2);
    const uint16_t target = dpmi_index_to_sel(3);
    bool emulate = vm->emulate_cpu;
    unsigned checks = 0, retries = 0;
    int failures = 0;
#define SOFTWARE_FRAME_CHECK(ok) do { \
    if (!(ok)) { \
        failures++; \
        serial_puts("[DPMI-SOFTWARE-FRAME] failed case="); serial_putdec(checks); \
        serial_puts(" cs:eip="); serial_puthex(cpu->cs, 4); serial_puts(":"); \
        serial_puthex(cpu->eip, 8); serial_puts(" depth="); \
        serial_putdec(vm->dpmi.exception_depth); serial_puts("\n"); \
    } \
    checks++; \
} while (0)
    for (unsigned use32 = 0; use32 < 2; use32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned source = 0; source < 2; source++)
    for (unsigned low_vector = 0; low_vector < 2; low_vector++)
    for (unsigned policy = 0; policy < POLICIES; policy++) {
        dpmi_init(vm);
        cpu8086_init(cpu, vm);
        dos_test_descriptor(&vm->dpmi, cs, true, use32);
        dos_test_descriptor(&vm->dpmi, ss, false, stack32);
        dos_test_descriptor(&vm->dpmi, target, true, use32);
        vm->emulate_cpu = true;
        vm->dpmi.active = true;
        vm->dpmi.is_32bit = use32;
        vm->dpmi.virtual_interrupts_enabled = stack32 != 0;
        unsigned width = use32 ? 4u : 2u, size = width * 3u;
        uint8_t vector = low_vector ? 7u : 0x60u;
        vm->dpmi.pm_vectors[vector].sel = target;
        vm->dpmi.pm_vectors[vector].off = 0x1800;
        uint32_t linear = high ? 0x7FFFFFu : 0x8FFFFu;
        dpmi_descriptor_t *stack = &vm->dpmi.ldt[2];
        dpmi_desc_set_base(stack, linear - (0x9000u - size));
        if (policy == SHORT_SS) dpmi_desc_set_limit(stack, 0x8FFEu);
        if (policy == EXPAND_VALID || policy == EXPAND_INVALID) {
            stack->access |= 4u;
            dpmi_desc_set_limit(stack, 0x9000u - size - (policy == EXPAND_VALID));
        }
        cpu->protected_mode = cpu->pm_cs_loaded = true;
        cpu->cs = cs;
        cpu->eip = source ? 0x1005 : 0x1000;
        cpu->ss = ss;
        cpu->esp = stack32 ? 0x9000 : 0xBEEF9000;
        cpu->eax = 0xA5A54000;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL_MASK;
        cpu8086_sync_cs(cpu);
        cpu8086_sync_segment(cpu, 2);
        if (policy == INVALID_SS) cpu->ss_cache.valid = false;
        if (policy == EDITED_SS) {
            stack->access = 0;
            dpmi_desc_set_base(stack, 0xE0000);
            stack->flags_lim ^= DESC_32BIT;
        }
        if (policy == CODE_ABSENT) vm->dpmi.ldt[3].access &= ~DESC_PRESENT;
        if (policy == CODE_DATA) vm->dpmi.ldt[3].access &= ~DESC_CODE;
        if (policy == CODE_SHORT) dpmi_desc_set_limit(&vm->dpmi.ldt[3], 0x17FF);
        if (policy == CODE_EXECUTE_ONLY) vm->dpmi.ldt[3].access &= ~DESC_READABLE;
        for (unsigned i = 0; i < 0x4000; i++) vm->mem[0x10000 + i] = 0;
        uint32_t pde1 = 0x10000u + (linear >> 22) * 4u;
        uint32_t pde2 = 0x10000u + ((linear + size - 1u) >> 22) * 4u;
        uint32_t pte1 = 0x11000u + ((linear >> 12) & 1023u) * 4u;
        uint32_t pte2 = (high ? 0x12000u : 0x11000u) +
                        (((linear + size - 1u) >> 12) & 1023u) * 4u;
        dos_mem_write32(vm, 0x10000, high ? 0x13007 : 0x11007);
        dos_mem_write32(vm, high ? 0x13004 : 0x11004, 0x1007);
        dos_mem_write32(vm, pde1, 0x11007);
        dos_mem_write32(vm, pde2, (high ? 0x12000u : 0x11000u) | 7u);
        dos_mem_write32(vm, pte1, 0x40007);
        dos_mem_write32(vm, pte2, 0x60007);
        if (policy >= PAGE_ABSENT && policy <= PAGE_SUPERVISOR)
            dos_mem_write32(vm, pte2, 0x60007u & ~(1u << (policy - PAGE_ABSENT)));
        if (policy >= PDE_ABSENT && policy <= PDE_SUPERVISOR)
            dos_mem_write32(vm, pde2, dos_mem_read32(vm, pde2) & ~(1u << (policy - PDE_ABSENT)));
        /* Keep fetch valid when both low stack pages share a restricted PDE. */
        if (!high && policy >= PDE_ABSENT && policy <= PDE_SUPERVISOR) {
            dpmi_desc_set_base(&vm->dpmi.ldt[1], 0x400000);
            dpmi_desc_set_base(&vm->dpmi.ldt[3], 0x400000);
            dos_mem_write32(vm, 0x10004, 0x13007);
            dos_mem_write32(vm, 0x13004, 0x1007);
            cpu8086_sync_cs(cpu);
        }
        const uint8_t instruction[] = { 0x66, 0x67, 0x3E, 0xCD, vector };
        for (unsigned i = 0; i < sizeof(instruction); i++) vm->mem[0x1000 + i] = instruction[i];
        vm->mem[0x1800] = 0xCF;
        for (unsigned i = 0; i < 32; i++) {
            vm->mem[0x40FF0 + i] = vm->mem[0x60000 + i] = 0xA5;
            if (!high) vm->mem[linear - 4u + i] = 0x5A;
        }
        cpu->cr0 = DOS_CR0_PG | DOS_CR0_WP | 1u;
        cpu->cr3 = 0x10000;
        uint32_t sp = cpu->esp, flags = cpu->eflags, image = cpu8086_flags_image(cpu);
        cpu_event_fault_t fault = {0};
        cpu->delivery_fault = &fault;
        bool accepted = source ? cpu_deliver_pm_software_interrupt(vm, vector, 0x1005, 0x1000)
                               : cpu8086_run_one(vm);
        cpu->delivery_fault = NULL;
        bool allowed = policy == VALID || policy == EXPAND_VALID || policy == EDITED_SS ||
                       policy == CODE_EXECUTE_ONLY;
        bool ok = accepted && cpu->running && cpu->eax == 0xA5A54000 &&
                  vm->dpmi.exception_depth == 0 && vm->dpmi.interrupt_depth == 0;
        if (allowed) {
            ok &= !fault.raised && cpu->cs == target && cpu->eip == 0x1800 &&
                  cpu->ss == ss && cpu->esp == sp - size && !(cpu->flags & FLAG_TF) &&
                  vm->dpmi.virtual_interrupts_enabled == (!low_vector && stack32);
            const uint32_t fields[] = { 0x1005, cs, image };
            for (unsigned i = 0; i < size; i++)
                if (vm->mem[i ? 0x60000u + i - 1u : 0x40FFFu] !=
                    (uint8_t)(fields[i / width] >> (8u * (i % width)))) ok = false;
            ok &= dos_mem_read32(vm, pte1) == 0x40067 && dos_mem_read32(vm, pte2) == 0x60067;
            ok &= cpu8086_run_one(vm) && cpu->cs == cs && cpu->eip == 0x1005 &&
                  cpu->esp == sp && vm->dpmi.virtual_interrupts_enabled == (stack32 != 0);
        } else {
            unsigned expected_vector = policy <= PDE_SUPERVISOR ? 14u :
                policy == CODE_ABSENT ? 11u : policy >= CODE_DATA ? 13u : 12u;
            unsigned error = expected_vector == 14u ?
                ((policy == PAGE_ABSENT || policy == PDE_ABSENT) ? 6u : 7u) :
                ((policy == CODE_ABSENT || policy == CODE_DATA) ? target & ~3u : 0u);
            ok &= fault.raised && fault.vector == expected_vector && fault.error == error &&
                  fault.return_eip == 0x1000 && cpu->cs == cs && cpu->ss == ss &&
                  cpu->esp == sp && cpu->eflags == flags &&
                  vm->dpmi.virtual_interrupts_enabled == (stack32 != 0);
            if (expected_vector == 14u)
                ok &= cpu->cr2 == linear + ((!high && policy >= PDE_ABSENT) ? 0u : 1u);
        }
        for (unsigned i = 0; i < 32; i++) {
            if ((!allowed || i != 15) && vm->mem[0x40FF0u + i] != 0xA5) ok = false;
            if ((!allowed || i >= size - 1u) && vm->mem[0x60000u + i] != 0xA5) ok = false;
            if (!high && vm->mem[linear - 4u + i] != 0x5A) ok = false;
        }
        SOFTWARE_FRAME_CHECK(ok);
    }

    /* Execute real exception returns: known-origin INTs retry the instruction;
     * completed native gates retry only their selected, unfinished entry. */
    for (unsigned use32 = 0; use32 < 2; use32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned completed = 0; completed < 2; completed++)
    for (unsigned policy = 0; policy < 6; policy++) {
        dpmi_init(vm);
        cpu8086_init(cpu, vm);
        dos_test_descriptor(&vm->dpmi, cs, true, use32);
        dos_test_descriptor(&vm->dpmi, ss, false, stack32);
        dos_test_descriptor(&vm->dpmi, target, true, use32);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(4), false, stack32);
        vm->emulate_cpu = !completed;
        vm->dpmi.active = true;
        vm->dpmi.is_32bit = use32;
        vm->dpmi.virtual_interrupts_enabled = false;
        dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x40000);
        dpmi_desc_set_limit(&vm->dpmi.ldt[2], 0x8FFE);
        dpmi_desc_set_base(&vm->dpmi.ldt[4], 0x60000);
        vm->dpmi.pm_vectors[0x60].sel = target;
        vm->dpmi.pm_vectors[0x60].off = 0x1800;
        vm->dpmi.exception_vectors[12].sel = cs;
        vm->dpmi.exception_vectors[12].off = 0x3000;
        vm->dpmi.exception_vectors[6].sel = cs;
        vm->dpmi.exception_vectors[6].off = 0x3000;
        cpu->protected_mode = cpu->pm_cs_loaded = true;
        cpu->cs = cs;
        cpu->eip = completed ? 0x1005 : 0x1000;
        cpu->ss = ss;
        cpu->esp = stack32 ? 0x9000 : 0xBEEF9000;
        cpu->ebx = 0;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_DF;
        cpu8086_sync_cs(cpu);
        cpu8086_sync_segment(cpu, 2);
        const uint8_t instruction[] = { 0x66, 0x67, 0x3E, 0xCD, 0x60 };
        for (unsigned i = 0; i < sizeof(instruction); i++) vm->mem[0x1000 + i] = instruction[i];
        vm->mem[0x1800] = 0x43; /* INC BX/EBX, count actual handler executions. */
        vm->mem[0x1801] = 0xCF;
        vm->mem[0x3000] = 0xCB;
        for (unsigned i = 0; i < 16; i++) vm->mem[0x48FF0 + i] = 0xA5;
        unsigned width = use32 ? 4u : 2u;
        uint32_t original_sp = cpu->esp;
        bool ok = completed ? dpmi_deliver_software_interrupt(vm, 0x60, 0x1005, 0x1005, true)
                            : cpu8086_run_one(vm);
        ok &= vm->dpmi.exception_depth == 1 && cpu->eip == 0x3000 && cpu->ebx == 0 &&
              vm->dpmi.exception_software_entries[0].active == (completed != 0);
        for (unsigned i = 0; i < 16; i++) if (vm->mem[0x48FF0 + i] != 0xA5) ok = false;
        uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
        uint32_t expected_ip = completed ? 0x1005 : 0x1000;
        ok &= (width == 4 ? dos_mem_read32(vm, frame + 3u * width)
                         : dos_mem_read16(vm, frame + 3u * width)) == expected_ip;
        if (policy == 5) {
            ok &= dpmi_deliver_exception(vm, 6, 0x3000, 0) && vm->dpmi.exception_depth == 2;
            ok &= cpu8086_run_one(vm) && cpu8086_run_one(vm) &&
                  vm->dpmi.exception_depth == 1 && cpu->eip == 0x3000 &&
                  vm->dpmi.exception_software_entries[0].active == (completed != 0);
        }
        if (policy == 1) {
            ok &= cpu8086_run_one(vm) && cpu8086_run_one(vm);
            if (!completed) ok &= cpu8086_run_one(vm);
            ok &= vm->dpmi.exception_depth == 1 && cpu->eip == 0x3000 && cpu->ebx == 0 &&
                  vm->dpmi.exception_software_entries[0].active == (completed != 0);
            frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
        }
        if (policy == 2 || policy == 3 || policy == 4) {
            unsigned field = policy == 2 ? 3u : policy == 3 ? 4u : 7u;
            uint32_t value = policy == 2 ? 0x1006u : policy == 3 ? target : dpmi_index_to_sel(4);
            if (width == 4) dos_mem_write32(vm, frame + field * width, value);
            else dos_mem_write16(vm, frame + field * width, value);
        } else dpmi_desc_set_limit(&vm->dpmi.ldt[2], 0xFFFF);
        ok &= cpu8086_run_one(vm) && cpu8086_run_one(vm);
        if (policy == 2 || policy == 3) {
            ok &= cpu->cs == (policy == 3 ? target : cs) &&
                  cpu->eip == (policy == 2 ? 0x1006u : expected_ip) && cpu->ebx == 0;
        } else {
            if (!completed) ok &= cpu8086_run_one(vm);
            ok &= cpu->cs == target && cpu->eip == 0x1800 && cpu->ebx == 0;
            ok &= cpu8086_run_one(vm) && cpu8086_run_one(vm) &&
                  cpu->cs == cs && cpu->eip == 0x1005 && cpu->ebx == 1 &&
                  cpu->esp == original_sp && cpu->ss == (policy == 4 ? dpmi_index_to_sel(4) : ss);
            retries++;
        }
        ok &= cpu->running && vm->dpmi.exception_depth == 0 &&
              !vm->dpmi.exception_software_entries[0].active &&
              !vm->dpmi.exception_software_entries[1].active;
        SOFTWARE_FRAME_CHECK(ok);
    }
#undef SOFTWARE_FRAME_CHECK
    cpu->cr0 = cpu->cr3 = 0;
    vm->emulate_cpu = emulate;
    dpmi_init(vm);
    serial_puts("[DPMI-SOFTWARE-FRAME] checks="); serial_putdec(checks);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_dpmi_return_destination_selftest(dos_vm_t *vm,
                                                cpu8086_state_t *cpu)
{
    enum {
        RETURN_EDITED, RETURN_EMPTY_STACK, RETURN_STACK_END, RETURN_EXPAND_DOWN,
        RETURN_CODE_END, RETURN_EXECUTE_ONLY, RETURN_CONFORMING,
        RETURN_CONFORMING_DPL0, RETURN_OTHER_CODE_WIDTH,
        RETURN_NULL_CS, RETURN_FREE_CS, RETURN_CS_RPL, RETURN_CS_DPL,
        RETURN_CS_DATA, RETURN_CS_SYSTEM, RETURN_CS_ABSENT, RETURN_CS_LONG,
        RETURN_CODE_OVER_LIMIT, RETURN_NULL_SS, RETURN_FREE_SS, RETURN_SS_RPL,
        RETURN_SS_DPL, RETURN_SS_READ_ONLY, RETURN_SS_CODE, RETURN_SS_SYSTEM,
        RETURN_SS_ABSENT, RETURN_BAD_CS_INDEX, RETURN_BAD_SS_INDEX, RETURN_CASES
    };
    const uint16_t handler_sel = dpmi_index_to_sel(1);
    const uint16_t frame_sel = dpmi_index_to_sel(2);
    const uint16_t code_sel = dpmi_index_to_sel(3);
    const uint16_t stack_sel = dpmi_index_to_sel(4);
    bool emulate = vm->emulate_cpu;
    unsigned checks = 0;
    int failures = 0;
    for (unsigned use32 = 0; use32 < 2; use32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned interpreted = 0; interpreted < 2; interpreted++)
    for (unsigned scenario = 0; scenario < RETURN_CASES; scenario++) {
        dpmi_init(vm);
        cpu8086_init(cpu, vm);
        dos_test_descriptor(&vm->dpmi, handler_sel, true, use32);
        dos_test_descriptor(&vm->dpmi, frame_sel, false, stack32);
        dos_test_descriptor(&vm->dpmi, code_sel, true, use32);
        dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
        dpmi_descriptor_t *code = &vm->dpmi.ldt[3];
        dpmi_descriptor_t *stack = &vm->dpmi.ldt[4];
        dpmi_desc_set_base(code, 0x10000);
        dpmi_desc_set_base(stack, 0x20000);
        vm->dpmi.active = true;
        vm->dpmi.is_32bit = use32;
        vm->dpmi.exception_depth = 1;
        vm->dpmi.exception_virtual_interrupts[0] = true;
        vm->dpmi.virtual_interrupts_enabled = false;
        vm->emulate_cpu = interpreted;
        cpu->protected_mode = true;
        cpu->cs = dpmi_get_host_code_selector(vm);
        cpu->eip = DPMI_EXCEPTION_RETURN_OFF + 2u;
        cpu->ss = frame_sel;
        cpu->esp = stack32 ? 0x8000 : 0xABCD8000;
        cpu->eflags = FLAGS_FIXED | FLAG_IF;
        cpu8086_sync_cs(cpu);
        cpu8086_sync_segment(cpu, 2);

        uint16_t cs = code_sel, ss = stack_sel;
        uint32_t ip = use32 ? 0x12345 : 0x1234;
        uint32_t sp = use32 && !stack32 ? 0xCAFE9000 : 0x9000;
        uint32_t flags = FLAGS_FIXED | FLAG_DF | FLAG_CF |
                         (use32 ? FLAG_RF : 0);
        dpmi_desc_set_limit(code, 0x1FFFF);
        bool allowed = scenario < RETURN_NULL_CS;
        switch (scenario) {
        case RETURN_EMPTY_STACK: sp = 0; break;
        case RETURN_STACK_END:
            dpmi_desc_set_limit(stack, 0x8FFF); break;
        case RETURN_EXPAND_DOWN:
            stack->access |= 4u;
            dpmi_desc_set_limit(stack, 0x9000); break;
        case RETURN_CODE_END: dpmi_desc_set_limit(code, ip); break;
        case RETURN_EXECUTE_ONLY: code->access &= ~DESC_READABLE; break;
        case RETURN_CONFORMING: code->access |= 4u; break;
        case RETURN_CONFORMING_DPL0:
            code->access = (code->access & ~DESC_DPL_MASK) | 4u; break;
        case RETURN_OTHER_CODE_WIDTH: code->flags_lim ^= DESC_32BIT; break;
        case RETURN_NULL_CS: cs = 0; break;
        case RETURN_FREE_CS: vm->dpmi.descriptor_state[3] = DPMI_DESC_FREE; break;
        case RETURN_CS_RPL: cs &= ~3u; break;
        case RETURN_CS_DPL: code->access &= ~DESC_DPL_MASK; break;
        case RETURN_CS_DATA: code->access &= ~DESC_CODE; break;
        case RETURN_CS_SYSTEM: code->access &= ~DESC_SEGMENT; break;
        case RETURN_CS_ABSENT: code->access &= ~DESC_PRESENT; break;
        case RETURN_CS_LONG: code->flags_lim |= 0x20u; break;
        case RETURN_CODE_OVER_LIMIT: dpmi_desc_set_limit(code, ip - 1u); break;
        case RETURN_NULL_SS: ss = 0; break;
        case RETURN_FREE_SS: vm->dpmi.descriptor_state[4] = DPMI_DESC_FREE; break;
        case RETURN_SS_RPL: ss &= ~3u; break;
        case RETURN_SS_DPL: stack->access &= ~DESC_DPL_MASK; break;
        case RETURN_SS_READ_ONLY: stack->access &= ~DESC_WRITABLE; break;
        case RETURN_SS_CODE: stack->access |= DESC_CODE; break;
        case RETURN_SS_SYSTEM: stack->access &= ~DESC_SEGMENT; break;
        case RETURN_SS_ABSENT: stack->access &= ~DESC_PRESENT; break;
        case RETURN_BAD_CS_INDEX: cs = dpmi_index_to_sel(DPMI_MAX_DESCRIPTORS); break;
        case RETURN_BAD_SS_INDEX: ss = dpmi_index_to_sel(DPMI_MAX_DESCRIPTORS); break;
        default: break;
        }
        unsigned width = use32 ? 4u : 2u;
        uint32_t address = 0x8000;
        uint32_t frame = address + (interpreted ? 3u * width : 0);
        const uint32_t fields[] = { 0, ip, cs, flags, sp, ss };
        for (unsigned i = 0; i < 48; i++) vm->mem[address + i] = 0xA5;
        for (unsigned i = 0; i < 6; i++) {
            if (use32) dos_mem_write32(vm, frame + i * width, fields[i]);
            else dos_mem_write16(vm, frame + i * width, (uint16_t)fields[i]);
        }
        uint8_t frame_before[48];
        for (unsigned i = 0; i < sizeof(frame_before); i++)
            frame_before[i] = vm->mem[address + i];
        cpu8086_state_t before = *cpu;
        bool returned = dpmi_exception_return_frame(vm, interpreted);
        bool ok = returned == allowed;
        if (allowed) {
            if (cpu->cs != cs || cpu->eip != ip || cpu->ss != ss || cpu->esp != sp ||
                cpu->eflags != (flags | FLAG_IF) || vm->dpmi.exception_depth ||
                vm->dpmi.virtual_interrupts_enabled != !interpreted ||
                cpu->op_size_32 != ((code->flags_lim & DESC_32BIT) != 0)) ok = false;
        } else {
            const uint8_t *a = (const uint8_t *)cpu;
            const uint8_t *b = (const uint8_t *)&before;
            for (unsigned i = 0; i < sizeof(before); i++) if (a[i] != b[i]) ok = false;
            if (vm->dpmi.exception_depth != 1 || vm->dpmi.virtual_interrupts_enabled ||
                !vm->dpmi.exception_virtual_interrupts[0]) ok = false;
        }
        for (unsigned i = 0; i < sizeof(frame_before); i++)
            if (frame_before[i] != vm->mem[address + i]) ok = false;
        if (!ok) {
            failures++;
            serial_puts("[DPMI-RETURN] failed case="); serial_putdec(scenario);
            serial_puts(" client="); serial_putdec(use32 ? 32 : 16);
            serial_puts(" SS.B="); serial_putdec(stack32);
            serial_puts(" private="); serial_putdec(interpreted); serial_puts("\n");
        }
        checks++;
    }
    dpmi_init(vm);
    vm->emulate_cpu = emulate;
    serial_puts("[DPMI-RETURN] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_dpmi_default_exception_selftest(dos_vm_t *vm,
                                               cpu8086_state_t *cpu)
{
    const uint16_t code_sel = dpmi_index_to_sel(1);
    const uint16_t stack_sel = dpmi_index_to_sel(2);
    unsigned checks = 0;
    int failures = 0;
    for (unsigned use32 = 0; use32 < 2; use32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned route = 0; route < 3; route++)
    for (unsigned vector = 0; vector < 32; vector++) {
        dpmi_init(vm);
        dos_test_descriptor(&vm->dpmi, code_sel, true, use32);
        dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
        vm->dpmi.active = true;
        vm->dpmi.is_32bit = use32;
        vm->emulate_cpu = true;
        cpu8086_init(cpu, vm);
        cpu->protected_mode = true;
        cpu->cs = code_sel;
        cpu->ss = cpu->ds = cpu->es = stack_sel;
        cpu8086_sync_cs(cpu);
        cpu->eax = 0xA5A50202u;
        cpu->ebx = 0xB6B60100u | vector;
        cpu->ecx = 0xC7C70000u;
        cpu->edx = 0xD8D80000u;
        cpu->eflags = FLAGS_FIXED | FLAG_CF;
        dos_int31_dpmi(vm);
        uint16_t host_sel = cpu->cx;
        uint32_t host_off = DPMI_PM_EXCEPTION_BASE_OFF +
                            vector * DPMI_PM_EXCEPTION_STUB_SIZE;
        bool ok = !(cpu->flags & FLAG_CF) && host_sel &&
            host_sel == vm->dpmi.sel_host_code &&
            cpu->eax == 0xA5A50202u &&
            cpu->ebx == (0xB6B60100u | vector) &&
            cpu->ecx == (0xC7C70000u | host_sel) &&
            cpu->edx == (use32 ? host_off : 0xD8D80000u | host_off);
        if (route) {
            cpu->ax = 0x0203;
            cpu->cx = route == 1 ? host_sel : code_sel;
            cpu->edx = (use32 ? 0 : 0xD8D80000u) |
                       (route == 1 ? host_off : 0x2200u);
            dos_int31_dpmi(vm);
            if ((cpu->flags & FLAG_CF) || cpu->ax != 0x0203 ||
                vm->dpmi.exception_vectors[vector].off !=
                    (route == 1 ? host_off : 0x2200u)) ok = false;
        }
        /* A client handler may tail-chain without changing the DPMI frame. */
        vm->mem[0x2200] = 0xEA;
        if (use32) {
            dos_mem_write32(vm, 0x2201, host_off);
            dos_mem_write16(vm, 0x2205, host_sel);
        } else {
            dos_mem_write16(vm, 0x2201, (uint16_t)host_off);
            dos_mem_write16(vm, 0x2203, host_sel);
        }
        uint32_t old_ivt = dos_mem_read32(vm, vector * 4u);
        dos_mem_write32(vm, vector * 4u, 0x00007000u);
        static const uint8_t real_handler[] = {
            0x50, 0x1E, 0x31, 0xC0, 0x8E, 0xD8,
            0xFE, 0x06, 0x00, 0x71, 0x1F, 0x58, 0xCF
        };
        for (unsigned i = 0; i < sizeof(real_handler); i++)
            vm->mem[0x7000u + i] = real_handler[i];
        vm->mem[0x7100] = 0;

        const uint32_t old_sp = use32 && !stack32 ? 0xABCD9000u : 0x9000u;
        /* Software DPMI exposes IOPL3. A reflected real-mode IRET must
         * preserve it, not accidentally erase it with a 12-bit FLAGS mask. */
        const uint32_t flags = FLAGS_FIXED | FLAG_IF | FLAG_DF | FLAG_CF | FLAG_IOPL_MASK;
        cpu->eax = 0xA1A11234u;
        cpu->ebx = 0xB2B22345u;
        cpu->ecx = 0xC3C33456u;
        cpu->edx = 0xD4D44567u;
        cpu->esi = 0xE5E55678u;
        cpu->edi = 0xF6F66789u;
        cpu->ebp = 0x1717789Au;
        cpu->eip = 0x2000;
        cpu->esp = old_sp;
        cpu->eflags = flags;
        vm->dpmi.virtual_interrupts_enabled = true;
        bool fatal = vector == 6 || vector >= 8;
        if (!cpu_deliver_exception(vm, (uint8_t)vector, 0x2000, 0x1234, true))
            ok = false;
        for (unsigned step = 0; cpu->running && step < 8; step++) {
            (void)dos_test_run_one(vm);
            if (!vm->dpmi.exception_depth) break;
        }
        if (fatal ? (cpu->running || cpu->exit_code != -1 || vm->mem[0x7100])
                  : (!cpu->running || vm->dpmi.exception_depth ||
                     cpu->eip != 0x2000 || cpu->cs != code_sel ||
                     cpu->esp != old_sp || cpu->ss != stack_sel ||
                     cpu->eflags != flags || vm->mem[0x7100] != 1))
            ok = false;
        if (cpu->eax != 0xA1A11234u || cpu->ebx != 0xB2B22345u ||
            cpu->ecx != 0xC3C33456u || cpu->edx != 0xD4D44567u ||
            cpu->esi != 0xE5E55678u || cpu->edi != 0xF6F66789u ||
            cpu->ebp != 0x1717789Au || cpu->ds != stack_sel ||
            cpu->es != stack_sel || cpu->fs || cpu->gs) ok = false;
        dos_mem_write32(vm, vector * 4u, old_ivt);
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-EXDEFAULT] FAIL mode="); serial_putdec(use32);
                serial_puts(" stack32="); serial_putdec(stack32);
                serial_puts(" route="); serial_putdec(route);
                serial_puts(" vector="); serial_putdec(vector);
                serial_puts(" cs:ip="); serial_puthex(cpu->cs, 4);
                serial_puts(":"); serial_puthex(cpu->eip, 8);
                serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }

    for (unsigned use32 = 0; use32 < 2; use32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++) {
        for (unsigned invalid = 0; invalid < 8; invalid++) {
            dpmi_init(vm);
            dos_test_descriptor(&vm->dpmi, code_sel, true, use32);
            dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
            vm->dpmi.active = true;
            vm->dpmi.is_32bit = use32;
            cpu8086_init(cpu, vm);
            cpu->protected_mode = true;
            cpu->cs = code_sel;
            cpu->ss = stack_sel;
            cpu->esp = 0x9000;
            cpu8086_sync_cs(cpu);
            (void)cpu_deliver_exception(vm, 13, 0x2000, 0x1234, true);
            uint32_t width = use32 ? 4u : 2u;
            uint32_t offset = cpu_stack_offset(cpu);
            uint32_t frame = dos_addr(vm, cpu->ss, offset);
            dpmi_descriptor_t *stack = &vm->dpmi.ldt[dpmi_sel_to_index(cpu->ss)];
            if (invalid == 0) vm->dpmi.exception_depth = 0;
            if (invalid == 1) dos_mem_write16(vm, frame, 0);
            if (invalid == 2) dos_mem_write16(vm, frame + width, code_sel);
            if (invalid == 3)
                dpmi_desc_set_limit(stack, offset + 8u * width - 2u);
            if (invalid == 4) stack->access &= ~DESC_WRITABLE;
            if (invalid == 5) vm->dpmi.active = false;
            if (invalid == 6) cpu->protected_mode = false;
            if (invalid == 3 || invalid == 4) cpu8086_sync_segment(cpu, 2);
            if (dpmi_dispatch_default_exception(vm, invalid == 7 ? 32 : 13, 0) ||
                !cpu->running) failures++;
            checks++;
        }
        /* A missing real-mode INT 0 handler must not turn a divide fault
         * into an endless sequence of successful no-op reflections. */
        dpmi_init(vm);
        dos_test_descriptor(&vm->dpmi, code_sel, true, use32);
        dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
        vm->dpmi.active = true;
        vm->dpmi.is_32bit = use32;
        cpu8086_init(cpu, vm);
        cpu->protected_mode = true;
        cpu->cs = code_sel;
        cpu->ss = stack_sel;
        cpu->esp = 0x9000;
        cpu8086_sync_cs(cpu);
        uint32_t old_ivt = dos_mem_read32(vm, 0);
        dos_mem_write32(vm, 0, 0);
        (void)cpu_deliver_exception(vm, 0, 0x2000, 0, false);
        (void)dos_test_run_one(vm);
        if (cpu->running || cpu->exit_code != -1) failures++;
        dos_mem_write32(vm, 0, old_ivt);
        checks++;
    }

    for (unsigned use32 = 0; use32 < 2; use32++) {
        dpmi_init(vm);
        dos_test_descriptor(&vm->dpmi, code_sel, true, use32);
        vm->dpmi.active = true;
        vm->dpmi.is_32bit = use32;
        cpu8086_init(cpu, vm);
        cpu->protected_mode = true;
        for (unsigned vector = 32; vector < 256; vector++)
        for (unsigned setting = 0; setting < 2; setting++) {
            cpu->ax = setting ? 0x0203 : 0x0202;
            cpu->ebx = 0xA5A50100u | vector;
            cpu->ecx = 0xB6B60000u | code_sel;
            cpu->edx = 0xC7C74000u;
            cpu->eflags = FLAGS_FIXED | FLAG_ZF;
            dos_int31_dpmi(vm);
            if (cpu->flags != (FLAGS_FIXED | FLAG_ZF | FLAG_CF) ||
                cpu->ax != 0x8021 || cpu->ebx != (0xA5A50100u | vector) ||
                cpu->ecx != (0xB6B60000u | code_sel) ||
                cpu->edx != 0xC7C74000u) failures++;
            checks++;
        }
    }
    serial_puts("[DOS-EXDEFAULT] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures);
    serial_puts("\n");
    vm->emulate_cpu = false;
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

    int result = dos_test_run(vm);
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

    dos_test_import_cs(vm);
    vm->dpmi.virtual_interrupts_enabled = false;
    if (cpu_deliver_hw_interrupt(vm, 8) || cpu->eip != code ||
        cpu->esp != 0x8000u)
        failures++;

    vm->dpmi.virtual_interrupts_enabled = true;
    uint16_t irq_stack = dpmi_get_exception_stack_selector(vm);
    uint32_t irq_frame = dpmi_translate(vm, irq_stack, DPMI_EXCEPTION_STACK_SIZE - 6u);
    if (!cpu_deliver_hw_interrupt(vm, 8) ||
        vm->dpmi.virtual_interrupts_enabled || cpu->eip != 0x1100u ||
        cpu->ss != irq_stack || cpu->esp != DPMI_EXCEPTION_STACK_SIZE - 6u ||
        !(cpu->flags & FLAG_IF) ||
        dos_mem_read16(vm, irq_frame) != DPMI_INTERRUPT_RETURN_OFF ||
        dos_mem_read16(vm, irq_frame + 2u) != vm->dpmi.sel_host_code ||
        !(dos_mem_read16(vm, irq_frame + 4u) & FLAG_IF))
        failures++;
    vm->mem[0x1100] = 0xCF;
    if (!dos_test_run_until(vm, true, code_sel, code) ||
        cpu->ss != stack_sel || cpu->esp != 0x8000u || vm->dpmi.interrupt_depth)
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
    dos_test_import_cs(vm);
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
    int default_irq_result = dos_test_run(vm);
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

    int result = dos_test_run(vm);
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

    result = dos_test_run(vm);
    if (result != 0x2A || cpu->running || cpu->bx != 0x5678u ||
        cpu->di != 0x0901u || cpu->si != 0x0900u || cpu->dx != 0x1234u ||
        !vm->dpmi.virtual_interrupts_enabled || !(cpu->flags & FLAG_IF))
        failures++;
    return failures;
}

static int dos_dpmi_interrupt_stack_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint16_t code_sel = dpmi_index_to_sel(1);
    const uint16_t stack_sel = dpmi_index_to_sel(2);
    const uint16_t replacement_sel = dpmi_index_to_sel(3);
    const uint8_t vectors[] = { 8, 0x1C, 0x23, 0x24 };
    bool emulate = vm->emulate_cpu;
    vm->emulate_cpu = true;
    unsigned checks = 0;
    int failures = 0;
    for (unsigned use32 = 0; use32 < 2; use32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned event = 0; event < sizeof(vectors); event++) {
        int before = failures;
        dpmi_init(vm);
        dos_test_descriptor(&vm->dpmi, code_sel, true, use32);
        dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
        dos_test_descriptor(&vm->dpmi, replacement_sel, false, stack32);
        dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000);
        dpmi_desc_set_base(&vm->dpmi.ldt[3], 0x30000);
        vm->dpmi.active = true;
        vm->dpmi.is_32bit = use32;
        vm->dpmi.pm_vectors[vectors[event]].sel = code_sel;
        vm->dpmi.pm_vectors[vectors[event]].off = 0x2000;
        vm->dpmi.pm_vectors[0x1C].sel = code_sel;
        vm->dpmi.pm_vectors[0x1C].off = 0x2000;
        cpu8086_init(cpu, vm);
        cpu->protected_mode = true;
        cpu->pm_cs_loaded = true;
        cpu->cs = code_sel;
        cpu->eip = 0x1000;
        cpu->ss = stack_sel;
        uint32_t original_sp = stack32 ? 2 : 0xBEEF0002;
        cpu->esp = original_sp;
        const uint32_t original_flags = FLAGS_FIXED | FLAG_IF | FLAG_CF |
                                        FLAG_DF | FLAG_IOPL_MASK;
        cpu->eflags = original_flags;
        cpu8086_sync_cs(cpu);
        vm->mem[0x2000] = 0xCF; /* handler IRET */
        for (unsigned i = 0; i < 16; i++) {
            vm->mem[0x20000 + i] = 0xA5;
            vm->mem[0x2FFF0 + i] = 0x5A;
        }
        vm->start_ticks = idt_get_ticks();
        vm->timer_irq_pending = false;
        bool delivered = event ? cpu_deliver_pm_software_interrupt(vm, vectors[event], 0x1000, cpu->eip)
                               : cpu_deliver_hw_interrupt(vm, vectors[event]);
        unsigned width = use32 ? 4u : 2u;
        bool ok = delivered && cpu->running && vm->dpmi.interrupt_depth == 1 &&
                  cpu->ss == vm->dpmi.sel_exception_stack && cpu->ss != stack_sel &&
                  cpu->esp == DPMI_EXCEPTION_STACK_SIZE - 3u * width &&
                  vm->dpmi.virtual_interrupts_enabled == (event != 0);
        for (unsigned i = 0; i < 16; i++)
            if (vm->mem[0x20000 + i] != 0xA5 || vm->mem[0x2FFF0 + i] != 0x5A) ok = false;
        if (!ok) failures++;
        checks++;
        if (!delivered || !cpu->running) continue;

        cpu8086_state_t outer = *cpu;
        cpu->ss = replacement_sel;
        uint32_t nested_sp = stack32 ? 0x9000 : 0xCAFE9000;
        cpu->esp = nested_sp;
        cpu8086_sync_segment(cpu, 2);
        bool nested = cpu_deliver_pm_software_interrupt(vm, 0x1C, 0x2100, cpu->eip);
        if (!nested || cpu->ss != replacement_sel || cpu->esp != nested_sp - 3u * width ||
            vm->dpmi.interrupt_depth != 2 ||
            !dos_test_run_until(vm, true, code_sel, 0x2100) ||
            cpu->ss != replacement_sel || cpu->esp != nested_sp ||
            vm->dpmi.interrupt_depth != 1 ||
            vm->dpmi.virtual_interrupts_enabled != (event != 0))
            failures++;
        checks++;
        *cpu = outer;
        if (!dos_test_run_until(vm, true, code_sel, 0x1000) ||
            cpu->ss != stack_sel || cpu->esp != original_sp || vm->dpmi.interrupt_depth ||
            !vm->dpmi.virtual_interrupts_enabled ||
            cpu->eflags != original_flags) failures++;
        checks++;

        /* Host-owned return cursors still need live descriptor validation:
         * a handler can release or change the interrupted client's segment. */
        for (unsigned interpreted = 0; interpreted < 2; interpreted++)
        for (unsigned revoked = 0; revoked < 3; revoked++) {
            *cpu = outer;
            cpu_stack_adjust(cpu, 3u * width);
            cpu->cs = vm->dpmi.sel_host_code;
            cpu->eip = DPMI_INTERRUPT_RETURN_OFF + 2u;
            cpu->eflags = original_flags;
            if (interpreted) {
                if (use32) {
                    cpu_push32(cpu, original_flags);
                    cpu_push32(cpu, cpu->cs);
                    cpu_push32(cpu, cpu->eip);
                } else {
                    cpu_push16(cpu, (uint16_t)original_flags);
                    cpu_push16(cpu, cpu->cs);
                    cpu_push16(cpu, cpu->ip);
                }
            }
            vm->dpmi.interrupt_depth = 1;
            vm->dpmi.virtual_interrupts_enabled = event != 0;
            dpmi_descriptor_t code_before = vm->dpmi.ldt[1];
            dpmi_descriptor_t stack_before = vm->dpmi.ldt[2];
            if (revoked == 0) vm->dpmi.ldt[1].access &= ~DESC_PRESENT;
            if (revoked == 1) vm->dpmi.ldt[2].access &= ~DESC_WRITABLE;
            if (revoked == 2) dpmi_desc_set_limit(&vm->dpmi.ldt[1], 0xFFF);
            cpu8086_state_t return_before = *cpu;
            ok = !dpmi_interrupt_return(vm, interpreted) &&
                 vm->dpmi.interrupt_depth == 1 &&
                 vm->dpmi.virtual_interrupts_enabled == (event != 0);
            const uint8_t *a = (const uint8_t *)cpu;
            const uint8_t *b = (const uint8_t *)&return_before;
            for (unsigned i = 0; i < sizeof(return_before); i++)
                if (a[i] != b[i]) ok = false;
            if (!ok) failures++;
            checks++;
            vm->dpmi.ldt[1] = code_before;
            vm->dpmi.ldt[2] = stack_before;
        }
        vm->dpmi.interrupt_depth = 0;

        /* A mode switch parks the active PM cursor, not its original top. */
        vm->dpmi.callback_depth = 1;
        vm->dpmi.suspended_stack = (dpmi_stack_t){ replacement_sel, nested_sp };
        cpu->protected_mode = false;
        cpu->ss = 0x500;
        cpu->esp = 0x8000;
        dpmi_stack_t selected;
        if (!dpmi_locked_stack_top(vm, 3u * width, &selected) ||
            selected.ss != replacement_sel || selected.esp != nested_sp ||
            cpu->ss != 0x500 || cpu->esp != 0x8000) failures++;
        checks++;
        vm->dpmi.suspended_stack.esp = 1;
        if (dpmi_locked_stack_top(vm, 3u * width, &selected)) failures++;
        checks++;
        if (failures != before) {
            serial_puts("[DPMI-LOCKSTACK] failed client="); serial_putdec(use32 ? 32 : 16);
            serial_puts(" SS.B="); serial_putdec(stack32);
            serial_puts(" vector="); serial_puthex(vectors[event], 2); serial_puts("\n");
        }
    }
    dpmi_init(vm);
    vm->emulate_cpu = emulate;
    serial_puts("[DPMI-LOCKSTACK] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_native_segments_selftest(dos_vm_t *vm, cpu8086_state_t *cpu);

int dos_dpmi_stack_selftest(void)
{
    const uint64_t pages = (2u * 1024u * 1024u) / 4096u;
    uint8_t *memory = dos_host_alloc_pages(pages);
    if (!memory) return 1;
    for (uint64_t i = 0; i < pages * 4096u; i++) memory[i] = 0;
    dos_vm_t vm = { .mem = memory, .total_mem_size = pages * 4096u,
                    .native_dispatch_depth = 1 };
    cpu8086_state_t cpu;
    vm.cpu = &cpu;
    dos_init_ivt(&vm);
    int failures = dos_dpmi_extended_exception_selftest(&vm, &cpu);
    failures += dos_dpmi_real_exception_selftest(&vm, &cpu);
    failures += dos_dpmi_paged_frame_selftest(&vm, &cpu);
    failures += dos_dpmi_private_frame_selftest(&vm, &cpu);
    failures += dos_dpmi_software_frame_selftest(&vm, &cpu);
    failures += dos_dpmi_locked_stack_selftest(&vm, &cpu);
    failures += dos_dpmi_return_destination_selftest(&vm, &cpu);
    failures += dos_dpmi_interrupt_stack_selftest(&vm, &cpu);
    failures += dos_native_segments_selftest(&vm, &cpu);
    dos_host_free_pages(memory, pages);
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

    int result = dos_test_run(vm);
    if (result != 0x2A || cpu->running || cpu->bx != 0x5678u ||
        cpu->dx != 0x1234u || cpu->ds != stack_sel ||
        cpu->es != code_sel || cpu->esp != 0x8000u ||
        !vm->dpmi.virtual_interrupts_enabled || !(cpu->flags & FLAG_IF))
        return 1;
    return 0;
}

static int dos_bit_instruction_selftest(dos_vm_t *vm,
                                        cpu8086_state_t *cpu)
{
    static const int32_t indices[] = {
        -33, -17, -1, 0, 1, 15, 16, 18, 21, 31, 32, 33, 63, 255,
        0x7FFF, 0x8000, 0x7FFFFFFF, (-2147483647 - 1)
    };
    static const uint8_t opcodes[] = {0xA3, 0xAB, 0xB3, 0xBB};
    const uint32_t code = 0x1000u;
    unsigned checks = 0;
    int failures = 0;

    for (unsigned mode = 0; mode < 3; mode++) {
        bool pm = mode != 0, use32 = mode == 2;
        dpmi_init(vm);
        for (unsigned i = 1; i <= 4; i++) {
            dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(i),
                                i == 1, use32);
            dpmi_desc_set_base(&vm->dpmi.ldt[i], (i - 1u) * 0x10000u);
            dpmi_desc_set_limit(&vm->dpmi.ldt[i], 0x1FFFFu);
        }
        vm->dpmi.active = pm;
        vm->dpmi.is_32bit = use32;
        for (unsigned wide = 0; wide < 2; wide++)
        for (unsigned adr32 = 0; adr32 < 2; adr32++)
        for (unsigned form = 0; form < 5; form++)
        for (unsigned immediate = 0; immediate < 2; immediate++)
        for (unsigned op = 0; op < 4; op++)
        for (unsigned n = 0; n < sizeof(indices) / sizeof(indices[0]); n++)
        for (unsigned invert = 0; invert < 2; invert++) {
            if ((form == 3 && op == 0) || (form && n >= 16)) continue;
            cpu8086_init(cpu, vm);
            cpu->protected_mode = pm;
            cpu->pm_cs_loaded = pm;
            cpu->op_size_32 = use32;
            cpu->addr_size_32 = use32;
            cpu->cs = pm ? dpmi_index_to_sel(1) : 0;
            cpu->ds = pm ? dpmi_index_to_sel(2) : 0x1000;
            cpu->ss = pm ? dpmi_index_to_sel(3) : 0x2000;
            cpu->fs = pm ? dpmi_index_to_sel(4) : 0x3000;
            cpu->eip = code;
            cpu->ebx = form == 4 ? 0xFFE0u : 0x3000u;
            cpu->esi = form == 4 ? (adr32 ? 0x10u : 0x20u)
                                 : (adr32 ? 0x80u : 0x100u);
            cpu->ebp = 0x3100u;
            cpu->ecx = (uint32_t)indices[n];
            uint32_t before = invert ? 0x5A5AA5A5u : 0xA5A55A5Au;
            cpu->edx = before;
            uint32_t index = immediate ? (uint8_t)indices[n]
                                      : (uint32_t)indices[n];
            unsigned bit = index % (wide ? 32u : 16u);
            uint32_t mask = 1u << bit;
            uint32_t flags = FLAGS_FIXED | FLAG_DF | FLAG_IF | FLAG_ZF |
                             0x00240000u | ((before & mask) ? 0 : FLAG_CF);
            cpu->eflags = flags;

            uint32_t p = code;
            if (form == 3) {
                vm->mem[p++] = 0xF0; /* LOCK */
                vm->mem[p++] = 0x64; /* FS override */
            }
            if (wide != use32) vm->mem[p++] = 0x66;
            if (adr32 != use32) vm->mem[p++] = 0x67;
            vm->mem[p++] = 0x0F;
            vm->mem[p++] = immediate ? 0xBA : opcodes[op];
            uint8_t reg = (uint8_t)((immediate ? 4u + op : 1u) << 3);
            if (form == 0) vm->mem[p++] = 0xC2 | reg; /* DX/EDX */
            else {
                vm->mem[p++] = reg | (form == 2 ? (adr32 ? 0x45 : 0x46)
                                                               : (adr32 ? 0x44 : 0x40));
                if (adr32 && form != 2) vm->mem[p++] = 0x73; /* EBX+ESI*2 */
                vm->mem[p++] = form == 4 ? 0 : 7;
            }
            if (immediate) vm->mem[p++] = (uint8_t)indices[n];

            uint32_t address = 0;
            uint8_t expected[12];
            if (form) {
                int32_t signed_index = wide ? indices[n] : (int16_t)indices[n];
                int32_t width = wide ? 32 : 16;
                int32_t element = signed_index / width;
                if (signed_index < 0 && signed_index % width) element--;
                uint32_t offset = form == 4 ? 0x10000u : 0x3107u;
                if (!immediate) offset += (uint32_t)(element * (width / 8));
                /* Segment-limit fault delivery is a separate interpreter
                 * contract; do not bless real-mode 32-bit offset truncation. */
                if (!pm && adr32 && offset > 0xFFFFu - (wide ? 3u : 1u))
                    continue;
                if (!adr32 || !pm) offset &= 0xFFFFu;
                address = (form == 2 ? 0x20000u : form == 3 ? 0x30000u
                                                                    : 0x10000u) + offset;
                for (unsigned i = 0; i < sizeof(expected); i++)
                    expected[i] = vm->mem[address - 4u + i] = 0x69;
                for (unsigned i = 0; i < (wide ? 4u : 2u); i++)
                    expected[4u + i] = vm->mem[address + i] = before >> (i * 8u);
                uint8_t bit_byte = (uint8_t)(1u << (bit % 8u));
                if (op == 1) expected[4u + bit / 8u] |= bit_byte;
                if (op == 2) expected[4u + bit / 8u] &= (uint8_t)~bit_byte;
                if (op == 3) expected[4u + bit / 8u] ^= bit_byte;
            }
            uint32_t result = before;
            if (!form) {
                if (op == 1) result |= mask;
                if (op == 2) result &= ~mask;
                if (op == 3) result ^= mask;
            }
            bool ok = dos_test_run_until(vm, pm, cpu->cs, p);
            ok = ok && cpu->edx == result && cpu->ecx == (uint32_t)indices[n] &&
                 cpu->eflags == ((flags & ~FLAG_CF) | ((before & mask) ? FLAG_CF : 0));
            if (form)
                for (unsigned i = 0; i < sizeof(expected); i++)
                    if (vm->mem[address - 4u + i] != expected[i]) ok = false;
            checks++;
            if (!ok) {
                if (failures < 8) {
                    serial_puts("[DOS-BITOPS] FAIL case=");
                    serial_putdec(checks);
                    serial_puts("\n");
                }
                failures++;
            }
        }
    }

    /* Stop at #UD entry: invalid encodings must leave their destination
     * intact and save the first prefix's IP, not the decoded instruction end. */
    dpmi_init(vm);
    dos_mem_write32(vm, 6u * 4u, 0x00002000u);
    for (unsigned encoding = 0; encoding < 14; encoding++) {
        cpu8086_init(cpu, vm);
        cpu->cs = 0;
        cpu->eip = code;
        cpu->ss = 0x2000;
        cpu->esp = 0x8000;
        cpu->edx = 0xA5A55A5Au;
        cpu->ecx = 21;
        cpu->ebx = 0x3000;
        cpu->ds = 0x1000;
        uint32_t p = code;
        bool locked = encoding >= 4;
        if (locked) vm->mem[p++] = 0xF0;
        vm->mem[p++] = 0x66;
        vm->mem[p++] = 0x0F;
        vm->mem[p++] = encoding < 8 || encoding == 12
                    ? 0xBA : encoding == 13 ? 0xA3 : opcodes[encoding - 8u];
        if (encoding < 8) vm->mem[p++] = 0xC2 | (encoding << 3);
        else if (encoding < 12) vm->mem[p++] = 0xCA;
        else vm->mem[p++] = encoding == 12 ? 0x27 : 0x0F; /* LOCK BT [BX] */
        vm->mem[p++] = 21;
        dos_mem_write32(vm, 0x13000u, 0x12345678u);
        bool ok = dos_test_run_until_real(vm, 0, 0x2000);
        checks++;
        if (!ok || cpu->edx != 0xA5A55A5Au || cpu->ecx != 21 ||
            cpu->sp != 0x7FFAu || dos_mem_read16(vm, 0x27FFAu) != code ||
            dos_mem_read32(vm, 0x13000u) != 0x12345678u)
            failures++;
    }
    serial_puts("[DOS-BITOPS] checks=");
    serial_putdec(checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

static bool dos_selector_query_test(dos_vm_t *vm, cpu8086_state_t *cpu,
                                    unsigned query, bool wide, unsigned form,
                                    bool valid, uint32_t value)
{
    uint32_t p = 0x1000u;
    bool adr32 = form == 2;
    cpu->eip = p;
    cpu->eax |= 0xA5A50000u;
    cpu->edx = form == 3 ? cpu->eax : 0x76543210u;
    cpu->esi = 0x0200;
    uint32_t original = cpu->edx, source = cpu->eax;
    uint32_t flags = FLAGS_FIXED | FLAG_CF | FLAG_PF | FLAG_AF | FLAG_SF |
                     FLAG_IF | FLAG_DF | FLAG_OF | 0x00240000u |
                     ((source & 1u) ? FLAG_ZF : 0);
    cpu->eflags = flags;
    dos_mem_write32(vm, 0x6200u, 0xFFFF0000u | cpu->ax);
    if (wide != cpu->op_size_32) vm->mem[p++] = 0x66;
    if (adr32 != cpu->addr_size_32) vm->mem[p++] = 0x67;
    vm->mem[p++] = 0x0F;
    vm->mem[p++] = query < 2 ? 2u + query : 0;
    uint8_t reg = (query < 2 ? 2u : query + 2u) << 3;
    vm->mem[p++] = reg | (form == 0 ? 0xC0 : form == 3 ? 0xC2
                                                     : adr32 ? 0x06 : 0x04);
    uint32_t expected = original;
    if (valid && query < 2)
        expected = wide ? value : (original & 0xFFFF0000u) | (uint16_t)value;
    vm->step_limit = vm->step_count + 4u;
    bool reached = cpu8086_run_until(vm, true, cpu->cs, p);
    vm->step_limit = 0;
    vm->step_limit_reached = false;
    return reached && cpu->edx == expected && cpu->eax == source &&
           cpu->eflags == ((flags & ~FLAG_ZF) | (valid ? FLAG_ZF : 0)) &&
           dos_mem_read32(vm, 0x6200u) == (0xFFFF0000u | (uint16_t)source);
}

static int dos_pop_instruction_selftest(dos_vm_t *vm,
                                        cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    dpmi_init(vm);
    const uint16_t code_sel = dpmi_index_to_sel(1);
    const uint16_t stack_sel = dpmi_index_to_sel(2);
    const uint16_t data_sel = dpmi_index_to_sel(3);
    uint32_t *registers[] = { &cpu->eax, &cpu->ecx, &cpu->edx, &cpu->ebx,
                             &cpu->esp, &cpu->ebp, &cpu->esi, &cpu->edi };
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned form = 0; form < 11; form++) {
        if ((!mode && stack32) || (!adr32 && form >= 9)) continue;
        cpu8086_init(cpu, vm);
        cpu->protected_mode = cpu->pm_cs_loaded = mode != 0;
        cpu->op_size_32 = cpu->addr_size_32 = mode == 2;
        vm->dpmi.active = mode != 0;
        dos_test_descriptor(&vm->dpmi, code_sel, true, mode == 2);
        dos_test_descriptor(&vm->dpmi, stack_sel, false, stack32);
        dos_test_descriptor(&vm->dpmi, data_sel, false, false);
        dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x8000);
        if (stack32) dpmi_desc_set_limit(&vm->dpmi.ldt[2], 0x1FFFF);
        dpmi_desc_set_base(&vm->dpmi.ldt[3], 0x20000);
        cpu->cs = mode ? code_sel : 0;
        cpu->ss = mode ? stack_sel : 0x800;
        cpu->ds = mode ? data_sel : 0x2000;
        uint32_t expected[8];
        for (unsigned i = 0; i < 8; i++)
            *registers[i] = 0xA0B00100u + i * 0x1111u;
        cpu->ebx = cpu->esi = 0x3000;
        cpu->ecx = 3;
        cpu->esp = stack32 ? 0x15000u : form >= 9 ? 0x5000u : 0xA5A55000u;
        uint32_t width = wide ? 4u : 2u;
        uint32_t next_sp = stack32 ? cpu->esp + width
            : (cpu->esp & 0xFFFF0000u) | (uint16_t)(cpu->sp + width);
        uint32_t source = 0x8000u + (stack32 ? cpu->esp : cpu->sp);
        uint32_t destination = form == 8 ? 0x23000u
                               : 0x8000u + next_sp + (form == 10 ? 26u : 0u);
        uint32_t marker = 0x12345678u;
        dos_mem_write32(vm, source, marker);
        if (form >= 8) {
            dos_mem_write32(vm, destination - 4u, 0xCCCCCCCCu);
            dos_mem_write32(vm, destination, 0xCCCCCCCCu);
            dos_mem_write32(vm, destination + 4u, 0xCCCCCCCCu);
            dos_mem_write32(vm, source, marker);
        }
        for (unsigned i = 0; i < 8; i++) expected[i] = *registers[i];
        expected[4] = next_sp;
        if (form < 8)
            expected[form] = wide ? marker
                : (expected[form] & 0xFFFF0000u) | (uint16_t)marker;
        uint32_t flags = FLAGS_FIXED | FLAG_CF | FLAG_PF | FLAG_AF |
                         FLAG_ZF | FLAG_SF | FLAG_IF | FLAG_DF | FLAG_OF;
        cpu->eflags = flags;
        cpu->eip = 0x1000;
        uint32_t p = 0x1000;
        if (wide != cpu->op_size_32) vm->mem[p++] = 0x66;
        if (adr32 != cpu->addr_size_32) vm->mem[p++] = 0x67;
        vm->mem[p++] = 0x8F;
        if (form < 8) vm->mem[p++] = 0xC0u | form;
        else if (form == 8) vm->mem[p++] = adr32 ? 0x06 : 0x07;
        else {
            vm->mem[p++] = form == 9 ? 0x04 : 0x44;
            vm->mem[p++] = form == 9 ? 0x24 : 0x8C; /* ESP or ESP+ECX*4+14 */
            if (form == 10) vm->mem[p++] = 14;
        }
        bool ok = dos_test_run_until(vm, mode != 0, cpu->cs, p) &&
                  cpu->eflags == flags;
        for (unsigned i = 0; i < 8; i++)
            if (*registers[i] != expected[i]) ok = false;
        if (form >= 8 &&
            (dos_mem_read32(vm, destination) !=
             (wide ? marker : 0xCCCC0000u | (uint16_t)marker) ||
             dos_mem_read32(vm, destination + 4u) != 0xCCCCCCCCu)) ok = false;
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-POP] FAIL case=");
                serial_putdec(checks);
                serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    dpmi_init(vm);
    dos_mem_write32(vm, 6u * 4u, 0x00002000u);
    for (unsigned invalid = 0; invalid < 8; invalid++) {
        cpu8086_init(cpu, vm);
        cpu->eip = 0x1000;
        cpu->ss = 0x200;
        cpu->sp = 0x8000;
        cpu->eax = 0xA5A55A5Au;
        vm->mem[0x1000] = invalid ? 0x66 : 0xF0;
        vm->mem[0x1001] = 0x8F;
        vm->mem[0x1002] = 0xC0u | (invalid << 3);
        bool reached = dos_test_run_until_real(vm, 0, 0x2000);
        if (!reached || cpu->eax != 0xA5A55A5Au || cpu->sp != 0x7FFAu ||
            dos_mem_read16(vm, 0x9FFAu) != 0x1000u) failures++;
        checks++;
    }
    serial_puts("[DOS-POP] checks=");
    serial_putdec(checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

static int dos_group5_instruction_selftest(dos_vm_t *vm,
                                           cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    const unsigned operations[] = { 0, 1, 2, 4, 6 };
    const uint32_t values[] = { 0x7FFFFFFFu, 0x80000000u,
                                0xFFFFFFFFu, 0x0000FFFFu };
    uint32_t *registers[] = { &cpu->eax, &cpu->ecx, &cpu->edx, &cpu->ebx,
                             &cpu->esp, &cpu->ebp, &cpu->esi, &cpu->edi };
    dpmi_init(vm);
    dos_mem_write32(vm, 13u * 4u, 0x3000u);
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned form = 0; form < 5; form++)
    for (unsigned op = 0; op < 5; op++)
    for (unsigned sample = 0; sample < 4; sample++) {
        unsigned operation = operations[op];
        if ((!mode && stack32) || (form == 3 && !adr32) ||
            (form == 4 && operation > 1) || (operation > 1 && sample))
            continue;
        cpu8086_init(cpu, vm);
        cpu->protected_mode = cpu->pm_cs_loaded = mode != 0;
        cpu->op_size_32 = cpu->addr_size_32 = mode == 2;
        vm->dpmi.active = mode != 0;
        vm->dpmi.is_32bit = mode == 2;
        vm->dpmi.exception_depth = 0;
        vm->dpmi.virtual_interrupts_enabled = true;
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(1), true, mode == 2);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(2), false, stack32);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(3), false, false);
        dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x8000);
        dpmi_desc_set_base(&vm->dpmi.ldt[3], 0x20000);
        dpmi_desc_set_limit(&vm->dpmi.ldt[1], 0x3FFFFu);
        dpmi_desc_set_limit(&vm->dpmi.ldt[2], 0x3FFFFu);
        cpu->cs = mode ? dpmi_index_to_sel(1) : 0;
        cpu->ss = mode ? dpmi_index_to_sel(2) : 0x800;
        cpu->ds = mode ? dpmi_index_to_sel(3) : 0x2000;
        dos_test_import_cs(vm);
        vm->dpmi.exception_vectors[13].sel = cpu->cs;
        vm->dpmi.exception_vectors[13].off = 0x3000u;
        cpu->ebx = cpu->esi = 0x3000;
        cpu->esp = stack32 ? 0x15000u : form == 3 ? 0x5000u : 0xA5A55000u;
        uint32_t source = operation <= 1 ? values[sample] : 0x12345678u;
        if (operation == 2 || operation == 4) source = 0x12340u;
        cpu->eax = source;
        uint32_t operand = form == 3 ? 0x8000u + cpu->esp : 0x23000u;
        if (form == 2 || form == 3) dos_mem_write32(vm, operand, source);
        if (form == 1) source = cpu->esp;
        uint32_t expected[8];
        for (unsigned i = 0; i < 8; i++) expected[i] = *registers[i];
        uint32_t mask = wide ? 0xFFFFFFFFu : 0xFFFFu;
        uint32_t value = source & mask;
        uint32_t flags = FLAGS_FIXED | FLAG_IF | FLAG_DF |
                         (sample & 1 ? FLAG_CF : 0);
        cpu->eflags = flags;
        uint32_t result = value;
        if (operation <= 1) {
            result = (operation ? value - 1u : value + 1u) & mask;
            uint32_t sign = wide ? 0x80000000u : 0x8000u;
            unsigned parity = 0;
            for (unsigned b = 0; b < 8; b++) parity ^= (result >> b) & 1u;
            if (!result) flags |= FLAG_ZF;
            if (result & sign) flags |= FLAG_SF;
            if (!parity) flags |= FLAG_PF;
            if ((value ^ result) & 0x10u) flags |= FLAG_AF;
            if (operation ? value == sign : result == sign) flags |= FLAG_OF;
            result = (source & ~mask) | result;
            if (form < 2 || form == 4) expected[form == 1 ? 4 : 0] = result;
        }
        uint32_t width = wide ? 4u : 2u;
        uint32_t push_addr = 0x8000u + cpu_stack_offset(cpu) - width;
        dos_mem_write32(vm, push_addr - 4u, 0xCCCCCCCCu);
        if (wide) dos_mem_write32(vm, push_addr, 0xCCCCCCCCu);
        else dos_mem_write16(vm, push_addr, 0xCCCCu);
        if (operation == 2 || operation == 6) {
            expected[4] = stack32 ? cpu->esp - width
                : (cpu->esp & 0xFFFF0000u) | (uint16_t)(cpu->sp - width);
        }
        cpu->eip = 0x1000;
        uint32_t p = 0x1000;
        if (wide != cpu->op_size_32) vm->mem[p++] = 0x66;
        if (adr32 != cpu->addr_size_32) vm->mem[p++] = 0x67;
        if (form == 4) vm->mem[p++] = operation ? 0x48 : 0x40;
        else {
            vm->mem[p++] = 0xFF;
            unsigned rm = form == 0 ? 0xC0 : form == 1 ? 0xC4
                          : form == 2 ? (adr32 ? 0x06 : 0x07) : 0x04;
            vm->mem[p++] = (uint8_t)(rm | (operation << 3));
            if (form == 3) vm->mem[p++] = 0x24; /* [ESP], before decrement. */
        }
        bool transfer = operation == 2 || operation == 4;
        bool fault = transfer && value > (mode ? 0x3FFFFu : 0xFFFFu);
        uint32_t old_esp = cpu->esp, stack_start = 0x8000u + cpu_stack_offset(cpu) - 8u;
        uint16_t old_cs = cpu->cs, old_ss = cpu->ss;
        uint8_t expected_stack[12];
        for (unsigned i = 0; i < sizeof(expected_stack); i++)
            expected_stack[i] = vm->mem[stack_start + i];
        bool ok = dos_test_run_one(vm);
        if (fault) {
            unsigned frame_width = mode == 2 ? 4u : 2u;
            expected[4] = mode ? DPMI_EXCEPTION_STACK_SIZE - 8u * frame_width
                : (old_esp & 0xFFFF0000u) | (uint16_t)(old_esp - 6u);
            uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu)), fields[8];
            for (unsigned i = 0; i < (mode ? 8u : 3u); i++)
                fields[i] = frame_width == 4 ? dos_mem_read32(vm, frame + i * frame_width)
                                             : dos_mem_read16(vm, frame + i * frame_width);
            if (cpu->eip != 0x3000u || cpu->cs != old_cs) ok = false;
            if (mode) {
                if (vm->dpmi.exception_depth != 1 || fields[2] || fields[3] != 0x1000u ||
                    fields[4] != old_cs || (fields[5] & ~FLAG_IOPL_MASK) != flags ||
                    fields[6] != (mode == 2 ? old_esp : (uint16_t)old_esp) ||
                    fields[7] != old_ss || vm->dpmi.exception_esp_high[0] != (old_esp & 0xFFFF0000u))
                    ok = false;
            } else {
                uint16_t saved[] = { 0x1000u, old_cs, (uint16_t)flags };
                if (cpu->ss != old_ss || cpu->eflags != (flags & ~(FLAG_IF | FLAG_TF))) ok = false;
                for (unsigned i = 0; i < 3; i++) {
                    if (fields[i] != saved[i]) ok = false;
                    for (unsigned b = 0; b < 2; b++) expected_stack[2u + i * 2u + b] = saved[i] >> (b * 8u);
                }
            }
            faults++;
        } else {
            if (cpu->eflags != flags || cpu->eip != (transfer ? value : p)) ok = false;
            if (operation == 2 || operation == 6) {
                uint32_t pushed = operation == 2 ? p : value;
                for (unsigned b = 0; b < width; b++) expected_stack[8u - width + b] = pushed >> (b * 8u);
            }
        }
        for (unsigned i = 0; i < 8; i++)
            if (*registers[i] != expected[i]) ok = false;
        if ((form == 2 || form == 3) &&
            dos_mem_read32(vm, operand) != (operation <= 1 ? result : source))
            ok = false;
        if (form == 3 && operation <= 1)
            for (unsigned b = 0; b < 4; b++) expected_stack[8u + b] = result >> (b * 8u);
        for (unsigned i = 0; i < sizeof(expected_stack); i++)
            if (vm->mem[stack_start + i] != expected_stack[i]) ok = false;
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-GROUP5] FAIL case=");
                serial_putdec(checks);
                serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    dpmi_init(vm);
    dos_mem_write32(vm, 6u * 4u, 0x00002000u);
    const uint8_t invalid[][3] = {
        {0x66, 0xFF, 0xF8}, {0x66, 0xFF, 0x3F},
        {0x66, 0xFF, 0xD8}, {0x66, 0xFF, 0xE8},
        {0xF0, 0xFF, 0xC0}, {0xF0, 0xFF, 0xC8},
        {0xF0, 0xFF, 0x17}, {0xF0, 0xFF, 0x27},
        {0xF0, 0xFF, 0x37}, {0xF0, 0x40, 0x90}, {0xF0, 0x48, 0x90},
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        cpu8086_init(cpu, vm);
        cpu->eip = 0x1000;
        cpu->ss = 0x800;
        cpu->sp = 0x5000;
        cpu->eax = 0x12345678u;
        for (unsigned b = 0; b < 3; b++) vm->mem[0x1000u + b] = invalid[i][b];
        if (!dos_test_run_until_real(vm, 0, 0x2000) ||
            cpu->eax != 0x12345678u || cpu->sp != 0x4FFAu ||
            dos_mem_read16(vm, 0xCFFAu) != 0x1000u) failures++;
        checks++;
    }
    serial_puts("[DOS-GROUP5] checks=");
    serial_putdec(checks);
    serial_puts(" faults=");
    serial_putdec(faults);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

static int dos_accumulator_instruction_selftest(dos_vm_t *vm,
                                                cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    dpmi_init(vm);
    const uint32_t inputs[] = { 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0 };
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned op = 0; op < 9; op++)
    for (unsigned sample = 0; sample < 4; sample++)
    for (unsigned cf = 0; cf < 2; cf++) {
        uint32_t result[2], flags[2];
        bool ok = true;
        for (unsigned form = 0; form < 2; form++) {
            cpu8086_init(cpu, vm);
            cpu->protected_mode = cpu->pm_cs_loaded = mode != 0;
            cpu->op_size_32 = cpu->addr_size_32 = mode == 2;
            vm->dpmi.active = mode != 0;
            dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(1), true, mode == 2);
            cpu->cs = mode ? dpmi_index_to_sel(1) : 0;
            cpu->eip = 0x1000;
            cpu->eax = inputs[sample];
            cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_DF | (cf ? FLAG_CF : 0);
            uint32_t p = 0x1000;
            if (wide != cpu->op_size_32) vm->mem[p++] = 0x66;
            if (!form) vm->mem[p++] = op == 8 ? 0xA9 : 0x05u | (op << 3);
            else {
                vm->mem[p++] = op == 8 ? 0xF7 : 0x81;
                vm->mem[p++] = op == 8 ? 0xC0 : 0xC0u | (op << 3);
            }
            uint32_t imm = 0xFFFFCDFFu;
            if (wide) { dos_mem_write32(vm, p, imm); p += 4; }
            else { dos_mem_write16(vm, p, (uint16_t)imm); p += 2; }
            if (!dos_test_run_one(vm) || cpu->eip != p) ok = false;
            result[form] = cpu->eax;
            flags[form] = cpu->eflags;
            if (!wide && (cpu->eax >> 16) != (inputs[sample] >> 16)) ok = false;
            if (op >= 7 && cpu->eax != inputs[sample]) ok = false;
        }
        if (!ok || result[0] != result[1] || flags[0] != flags[1]) failures++;
        checks++;
    }
    serial_puts("[DOS-ACCUM] checks=");
    serial_putdec(checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

static int dos_loop_instruction_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    const uint32_t counts[] = { 0, 1, 2, 0x10000u, 0x10001u, 0xFFFFFFFFu };
    dpmi_init(vm);
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned op = 0; op < 4; op++)
    for (unsigned sample = 0; sample < 6; sample++)
    for (unsigned zf = 0; zf < 2; zf++)
    for (unsigned site = 0; site < 2; site++)
    for (unsigned backward = 0; backward < 2; backward++) {
        cpu8086_init(cpu, vm);
        cpu->protected_mode = cpu->pm_cs_loaded = mode != 0;
        cpu->op_size_32 = cpu->addr_size_32 = mode == 2;
        vm->dpmi.active = mode != 0;
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(1), true, mode == 2);
        dpmi_desc_set_limit(&vm->dpmi.ldt[1], 0x3FFFFu);
        cpu->cs = mode ? dpmi_index_to_sel(1) : 0;
        cpu->eip = site ? (mode == 2 ? 0x1FF80u : 0xFF80u) : 0x1000u;
        cpu->ecx = counts[sample];
        uint32_t flags = FLAGS_FIXED | FLAG_IF | FLAG_CF | FLAG_AF |
                         FLAG_PF | FLAG_SF | FLAG_DF | FLAG_OF | (zf ? FLAG_ZF : 0);
        cpu->eflags = flags;
        uint32_t p = cpu->eip;
        uint32_t start_ip = p;
        uint16_t saved_sp = cpu->sp;
        dos_mem_write32(vm, 13u * 4u, 0x3000);
        if (wide != cpu->op_size_32) vm->mem[p++] = 0x66;
        if (adr32 != cpu->addr_size_32) vm->mem[p++] = 0x67;
        vm->mem[p++] = (uint8_t)(0xE0u + op);
        int32_t rel = backward ? -128 : 127;
        vm->mem[p++] = (uint8_t)rel;
        uint32_t next_count = counts[sample];
        if (op != 3) next_count = adr32 ? next_count - 1u
            : (next_count & 0xFFFF0000u) | (uint16_t)(next_count - 1u);
        bool nonzero = (adr32 ? next_count : (uint16_t)next_count) != 0;
        bool take = op == 3 ? !nonzero : nonzero;
        if (!op) take = take && !zf;
        if (op == 1) take = take && zf;
        uint32_t target = p + (uint32_t)rel;
        uint32_t next_ip = take ? (wide ? target : (uint16_t)target) : p;
        bool outside = take && next_ip > (mode ? 0x3FFFFu : 0xFFFFu);
        bool ok = dos_test_run_one(vm);
        if (outside) {
            uint32_t frame = ((uint32_t)cpu->ss << 4) + cpu->sp;
            if (cpu->ecx != counts[sample] || cpu->eip != 0x3000 || cpu->cs ||
                cpu->sp != (uint16_t)(saved_sp - 6u) ||
                dos_mem_read16(vm, frame) != (uint16_t)start_ip ||
                dos_mem_read16(vm, frame + 4u) != flags ||
                cpu->eflags != (flags & ~(FLAG_IF | FLAG_TF))) ok = false;
        } else if (cpu->ecx != next_count || cpu->eip != next_ip || cpu->eflags != flags) ok = false;
        if (!ok) failures++;
        checks++;
    }
    serial_puts("[DOS-LOOP] checks=");
    serial_putdec(checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

/* Compare arithmetic against the host CPU, only after proving DIV cannot
 * fault. Undefined arithmetic flags are deliberately excluded by callers. */
static void dos_group3_reference(unsigned operation, bool wide,
                                 uint32_t *a, uint32_t *d,
                                 uint32_t *value, uint32_t *flags)
{
    uint32_t low = *a, high = *d, source = *value;
    uint64_t host_flags = 0;
#define UNARY_REFERENCE(instruction) do { \
    if (wide) __asm__ volatile (instruction "l %k0; pushfq; popq %1" \
        : "+r"(source), "=r"(host_flags) :: "cc", "memory"); \
    else __asm__ volatile (instruction "w %w0; pushfq; popq %1" \
        : "+r"(source), "=r"(host_flags) :: "cc", "memory"); \
} while (0)
#define PAIR_REFERENCE(instruction) do { \
    if (wide) __asm__ volatile (instruction "l %k3; pushfq; popq %2" \
        : "+&a"(low), "+&d"(high), "=r"(host_flags) : "r"(source) \
        : "cc", "memory"); \
    else __asm__ volatile (instruction "w %w3; pushfq; popq %2" \
        : "+&a"(low), "+&d"(high), "=r"(host_flags) : "r"(source) \
        : "cc", "memory"); \
} while (0)
    switch (operation) {
    case 2: UNARY_REFERENCE("not"); break;
    case 3: UNARY_REFERENCE("neg"); break;
    case 4: PAIR_REFERENCE("mul"); break;
    case 5: PAIR_REFERENCE("imul"); break;
    case 6: PAIR_REFERENCE("div"); break;
    case 7: PAIR_REFERENCE("idiv"); break;
    }
#undef PAIR_REFERENCE
#undef UNARY_REFERENCE
    *a = low;
    *d = high;
    *value = source;
    *flags = (uint32_t)host_flags;
}

static int dos_group3_instruction_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint32_t values[] = { 0, 1, 2, 0xFFFFu, 0x7FFFFFFFu,
                                0x80000000u, 0xFFFFFFFFu, 0x12345678u };
    uint32_t *registers[] = { &cpu->eax, &cpu->ecx, &cpu->edx, &cpu->ebx,
                             &cpu->esp, &cpu->ebp, &cpu->esi, &cpu->edi };
    const uint32_t status = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
    unsigned checks = 0, faults = 0, invalid_locks = 0;
    int failures = 0;
    dpmi_init(vm);
    dos_mem_write32(vm, 0, 0x00003000u);
    dos_mem_write32(vm, 6u * 4u, 0x00003100u);
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned form = 0; form < 4; form++)
    for (unsigned operation = 2; operation < 8; operation++)
    for (unsigned locked = 0; locked < 2; locked++)
    for (unsigned sample = 0; sample < 8; sample++)
    for (unsigned high_bits = 0; high_bits < 4; high_bits++) {
        cpu8086_init(cpu, vm);
        cpu->protected_mode = cpu->pm_cs_loaded = mode != 0;
        cpu->op_size_32 = cpu->addr_size_32 = mode == 2;
        vm->dpmi.active = mode != 0;
        vm->dpmi.is_32bit = mode == 2;
        vm->dpmi.exception_depth = 0;
        vm->dpmi.virtual_interrupts_enabled = true;
        for (unsigned i = 1; i <= 3; i++)
            dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(i), i == 1, mode == 2);
        dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000);
        cpu->cs = mode ? dpmi_index_to_sel(1) : 0;
        cpu->ds = mode ? dpmi_index_to_sel(2) : 0x2000;
        cpu->ss = mode ? dpmi_index_to_sel(3) : 0;
        vm->dpmi.exception_vectors[0].sel = cpu->cs;
        vm->dpmi.exception_vectors[0].off = 0x3000;
        vm->dpmi.exception_vectors[6].sel = cpu->cs;
        vm->dpmi.exception_vectors[6].off = 0x3100;
        cpu->esp = 0x9000;
        cpu->ebx = cpu->esi = 0x3000;
        cpu->eax = values[sample];
        cpu->edx = high_bits == 0 ? 0 : high_bits == 1 ? 0xFFFFFFFFu
                                  : high_bits == 2 ? 0x80008000u : 0x80000000u;
        cpu->ecx = high_bits == 3 ? 0xFFFFFFFFu : values[(sample + 3u) % 8u];
        uint32_t source = form < 3 ? *registers[form] : cpu->ecx;
        dos_mem_write32(vm, 0x23000u, source);
        dos_mem_write32(vm, 0x23004u, 0xCCCCCCCCu);
        uint32_t expected[8];
        for (unsigned i = 0; i < 8; i++) expected[i] = *registers[i];
        uint32_t width_mask = wide ? 0xFFFFFFFFu : 0xFFFFu;
        uint32_t divisor = source & width_mask;
        uint32_t high = cpu->edx & width_mask;
        bool divide_error = false;
        if (operation == 6) divide_error = !divisor || high >= divisor;
        if (operation == 7) {
            uint64_t bits = wide ? ((uint64_t)high << 32) | cpu->eax
                                 : (high << 16) | cpu->ax;
            int64_t dividend = wide ? (int64_t)bits : (int32_t)bits;
            int64_t signed_divisor = wide ? (int32_t)divisor : (int16_t)divisor;
            divide_error = !signed_divisor ||
                           (wide && bits == (1ULL << 63) && signed_divisor == -1);
            if (!divide_error) {
                int64_t quotient = dividend / signed_divisor;
                divide_error = wide ? quotient < -2147483648LL || quotient > 2147483647LL
                                    : quotient < -32768 || quotient > 32767;
            }
        }
        uint32_t original_flags = FLAGS_FIXED | FLAG_IF | FLAG_DF | status;
        cpu->eflags = original_flags;
        uint32_t reference_flags = 0, expected_memory = source;
        bool invalid_lock = locked && (form < 3 || operation > 3);
        if (!divide_error && !invalid_lock) {
            uint32_t a = cpu->eax, d = cpu->edx, value = source;
            dos_group3_reference(operation, wide, &a, &d, &value, &reference_flags);
            if (operation < 4) {
                if (form < 3) expected[form] = value;
                else expected_memory = value;
            } else { expected[0] = a; expected[2] = d; }
        }
        cpu->eip = 0x1000;
        uint32_t p = 0x1000;
        if (wide != cpu->op_size_32) vm->mem[p++] = 0x66;
        if (adr32 != cpu->addr_size_32) vm->mem[p++] = 0x67;
        if (locked) vm->mem[p++] = 0xF0;
        vm->mem[p++] = 0xF7;
        vm->mem[p++] = (uint8_t)((operation << 3) |
                                 (form < 3 ? 0xC0u | form : adr32 ? 6u : 7u));
        bool ok = dos_test_run_one(vm);
        if (divide_error || invalid_lock) {
            unsigned frame_width = mode == 2 ? 4u : 2u;
            expected[4] = mode ? DPMI_EXCEPTION_STACK_SIZE - 8u * frame_width
                               : expected[4] - 6u;
            uint32_t saved_ip_addr = dos_addr(vm, cpu->ss, expected[4]) +
                                     (mode ? 3u * frame_width : 0);
            if (cpu->eip != (invalid_lock ? 0x3100u : 0x3000u) ||
                cpu->cs != (mode ? dpmi_index_to_sel(1) : 0) ||
                (mode == 2 ? dos_mem_read32(vm, saved_ip_addr)
                           : dos_mem_read16(vm, saved_ip_addr)) != 0x1000u ||
                (mode && vm->dpmi.exception_depth != 1)) ok = false;
            if (invalid_lock) invalid_locks++;
            else faults++;
        } else {
            uint32_t defined = operation == 3 ? status
                             : operation == 4 || operation == 5 ? FLAG_CF | FLAG_OF : 0;
            if (cpu->eip != p || ((cpu->eflags ^ reference_flags) & defined) ||
                ((cpu->eflags ^ original_flags) & ~status) ||
                (operation == 2 && cpu->eflags != original_flags)) ok = false;
        }
        for (unsigned i = 0; i < 8; i++) if (*registers[i] != expected[i]) ok = false;
        if (dos_mem_read32(vm, 0x23000u) != expected_memory ||
            dos_mem_read32(vm, 0x23004u) != 0xCCCCCCCCu) ok = false;
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-GROUP3] FAIL case="); serial_putdec(checks);
                serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-GROUP3] checks="); serial_putdec(checks);
    serial_puts(" divide faults="); serial_putdec(faults);
    serial_puts(" invalid locks="); serial_putdec(invalid_locks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

static int dos_scalar_instruction_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint32_t inputs[] = { 0, 0x1234007Fu, 0x12340080u, 0x12347FFFu,
                                0x12348000u, 0x80000000u, 0xFFFFFFFFu };
    uint32_t *registers[] = { &cpu->eax, &cpu->ecx, &cpu->edx, &cpu->ebx,
                             &cpu->esp, &cpu->ebp, &cpu->esi, &cpu->edi };
    unsigned checks = 0;
    int failures = 0;
    dpmi_init(vm);
    dos_mem_write32(vm, 6u * 4u, 0x00003100u);
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned opcode = 0x91; opcode <= 0x99; opcode++)
    for (unsigned locked = 0; locked < 2; locked++)
    for (unsigned sample = 0; sample < sizeof(inputs) / sizeof(inputs[0]); sample++) {
        cpu8086_init(cpu, vm);
        cpu->protected_mode = cpu->pm_cs_loaded = mode != 0;
        cpu->op_size_32 = cpu->addr_size_32 = mode == 2;
        vm->dpmi.active = mode != 0;
        vm->dpmi.is_32bit = mode == 2;
        vm->dpmi.exception_depth = 0;
        vm->dpmi.virtual_interrupts_enabled = true;
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(1), true, mode == 2);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(2), false, mode == 2);
        cpu->cs = mode ? dpmi_index_to_sel(1) : 0;
        cpu->ss = mode ? dpmi_index_to_sel(2) : 0;
        vm->dpmi.exception_vectors[6].sel = cpu->cs;
        vm->dpmi.exception_vectors[6].off = 0x3100;
        uint32_t expected[8];
        for (unsigned i = 0; i < 8; i++)
            *registers[i] = expected[i] = i ? 0xABCD0123u + i : inputs[sample];
        if (locked) {
            cpu->esp = 0x9000;
            expected[4] = mode ? DPMI_EXCEPTION_STACK_SIZE - 8u * (mode == 2 ? 4u : 2u)
                               : 0x9000u - 6u;
        } else if (opcode < 0x98) {
            unsigned r = opcode - 0x90;
            uint32_t mask = wide ? 0xFFFFFFFFu : 0xFFFFu;
            expected[0] = (cpu->eax & ~mask) | (expected[r] & mask);
            expected[r] = (expected[r] & ~mask) | (cpu->eax & mask);
        } else if (opcode == 0x98) {
            expected[0] = wide ? (uint32_t)(int32_t)(int16_t)cpu->ax
                : (cpu->eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)cpu->al;
        } else {
            expected[2] = wide ? (cpu->eax & 0x80000000u ? 0xFFFFFFFFu : 0)
                : (cpu->edx & 0xFFFF0000u) | (cpu->ax & 0x8000u ? 0xFFFFu : 0);
        }
        uint32_t flags = FLAGS_FIXED | FLAG_IF | FLAG_DF | FLAG_CF | FLAG_OF;
        cpu->eflags = flags;
        cpu->eip = 0x1000;
        uint32_t p = 0x1000;
        if (wide != cpu->op_size_32) vm->mem[p++] = 0x66;
        if (locked) vm->mem[p++] = 0xF0;
        vm->mem[p++] = (uint8_t)opcode;
        bool ok = dos_test_run_one(vm);
        if (locked) {
            unsigned frame_width = mode == 2 ? 4u : 2u;
            uint32_t saved_ip_addr = dos_addr(vm, cpu->ss, expected[4]) +
                                     (mode ? 3u * frame_width : 0);
            if (cpu->eip != 0x3100u || cpu->cs != (mode ? dpmi_index_to_sel(1) : 0) ||
                (mode == 2 ? dos_mem_read32(vm, saved_ip_addr)
                           : dos_mem_read16(vm, saved_ip_addr)) != 0x1000u ||
                (mode && vm->dpmi.exception_depth != 1)) ok = false;
        } else if (cpu->eip != p || cpu->eflags != flags) ok = false;
        for (unsigned i = 0; i < 8; i++) if (*registers[i] != expected[i]) ok = false;
        if (!ok) failures++;
        checks++;
    }
    serial_puts("[DOS-SCALAR] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

static int dos_far_pointer_instruction_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint8_t opcodes[] = { 0xC4, 0xC5, 0xB2, 0xB4, 0xB5 };
    const uint8_t destinations[] = { 0, 3, 2, 4, 5 };
    const uint8_t prefixes[] = { 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65 };
    const uint16_t real_segments[] = { 0x3000, 0, 0, 0x2000, 0x4000, 0x5000 };
    const unsigned descriptor_indices[] = { 4, 1, 2, 3, 5, 6 };
    uint16_t *segments[] = { &cpu->es, &cpu->cs, &cpu->ss,
                            &cpu->ds, &cpu->fs, &cpu->gs };
    uint32_t *registers[] = { &cpu->eax, &cpu->ecx, &cpu->edx, &cpu->ebx,
                             &cpu->esp, &cpu->ebp, &cpu->esi, &cpu->edi };
    enum { SOURCE_LIMIT = 6, NULL_ZERO, NULL_RPL3, FREE_SELECTOR,
           NOT_PRESENT, READ_ONLY, EXECUTE_ONLY, READABLE_CODE, LOW_DPL,
           CONFORMING_CODE, LOW_RPL, SYSTEM_DESCRIPTOR, BAD_TYPE_ABSENT,
           EXPAND_VALID, EXPAND_INVALID, SOURCE_NULL, SOURCE_EXECUTE_ONLY,
           REGISTER_SOURCE, LOCKED, STACK_LIMIT, OFFSET_64K,
           GDT_TARGET, GDT_ABSENT, GUEST_LDT, CASE_COUNT };
    unsigned checks = 0, faults = 0;
    int failures = 0;
    dpmi_init(vm);
    for (unsigned vector = 0; vector < 32; vector++)
        dos_mem_write32(vm, vector * 4u, 0x6000u + vector * 16u);
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned instruction = 0; instruction < 5; instruction++)
    for (unsigned reg = 0; reg < 8; reg++)
    for (unsigned scenario = 0; scenario < CASE_COUNT; scenario++) {
        cpu8086_init(cpu, vm);
        cpu->protected_mode = cpu->pm_cs_loaded = mode != 0;
        cpu->op_size_32 = cpu->addr_size_32 = mode == 2;
        vm->dpmi.active = mode != 0;
        vm->dpmi.is_32bit = mode == 2;
        vm->dpmi.exception_depth = 0;
        vm->dpmi.virtual_interrupts_enabled = true;
        for (unsigned i = 1; i <= 7; i++) {
            dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(i), i == 1, mode == 2);
            dpmi_desc_set_base(&vm->dpmi.ldt[i], i < 3 ? 0 : (i - 1u) * 0x10000u);
        }
        for (unsigned i = 0; i < 6; i++)
            *segments[i] = mode ? dpmi_index_to_sel(descriptor_indices[i])
                                : real_segments[i];
        for (unsigned i = 0; i < 32; i++) {
            vm->dpmi.exception_vectors[i].sel = cpu->cs;
            vm->dpmi.exception_vectors[i].off = 0x6000u + i * 16u;
        }
        unsigned destination = destinations[instruction];
        unsigned source = scenario < 6 ? scenario : scenario == STACK_LIMIT ? 2u : 3u;
        uint32_t source_offset = scenario == SOURCE_LIMIT || scenario == STACK_LIMIT
                               ? 0xFFFEu : scenario == OFFSET_64K ? 0x10000u : 0x3000u;
        uint32_t effective = adr32 ? source_offset : (uint16_t)source_offset;
        uint32_t address = ((uint32_t)real_segments[source] << 4) + effective;
        uint16_t selector = mode ? dpmi_index_to_sel(7) : 0xABCD;
        int vector = -1;
        uint16_t error_code = 0;
        unsigned width = wide ? 4u : 2u;
        if ((uint64_t)effective + width + 1u > 0xFFFFu)
            vector = source == 2 ? 12 : 13;
        if (scenario == NULL_ZERO || scenario == NULL_RPL3) {
            selector = scenario == NULL_ZERO ? 0 : 3;
            if (mode && destination == 2) vector = 13;
        }
        if (mode) {
            dpmi_descriptor_t *target = &vm->dpmi.ldt[7];
            switch (scenario) {
            case FREE_SELECTOR:
                vm->dpmi.descriptor_state[7] = DPMI_DESC_FREE;
                vector = 13;
                break;
            case NOT_PRESENT: case GDT_ABSENT:
                target->access = 0x72;
                vector = destination == 2 ? 12 : 11;
                break;
            case READ_ONLY:
                target->access = 0xF0;
                if (destination == 2) vector = 13;
                break;
            case EXECUTE_ONLY: case BAD_TYPE_ABSENT:
                target->access = scenario == EXECUTE_ONLY ? 0xF8 : 0x78;
                vector = 13;
                break;
            case READABLE_CODE: case CONFORMING_CODE:
                target->access = scenario == READABLE_CODE ? 0xFA : 0x9E;
                if (destination == 2) vector = 13;
                break;
            case LOW_DPL:
                target->access = 0x92;
                vector = 13;
                break;
            case LOW_RPL:
                selector &= ~3u;
                if (destination == 2) vector = 13;
                break;
            case SYSTEM_DESCRIPTOR:
                target->access = 0x82;
                vector = 13;
                break;
            case EXPAND_VALID: case EXPAND_INVALID:
                vm->dpmi.ldt[3].access = 0xF6;
                vm->dpmi.ldt[3].limit_lo = scenario == EXPAND_VALID ? 0x2FFF : 0x3000;
                if (scenario == EXPAND_INVALID) vector = 13;
                break;
            case SOURCE_NULL:
                cpu->ds = 3;
                vector = 13;
                break;
            case SOURCE_EXECUTE_ONLY:
                vm->dpmi.ldt[3].access = 0xF8;
                vector = 13;
                break;
            }
            if (scenario == GDT_TARGET || scenario == GDT_ABSENT) {
                cpu->gdtr.base = 0x10000;
                cpu->gdtr.limit = 0x3F;
                selector = (7u << 3) | 3u;
                for (unsigned i = 0; i < 8; i++)
                    dos_mem_write8(vm, 0x10038u + i, ((uint8_t *)target)[i]);
            }
            if (scenario == GUEST_LDT) {
                (void)dpmi_get_host_code_selector(vm);
                (void)dpmi_get_exception_stack_selector(vm);
                cpu->gdtr.base = 0x10000;
                cpu->gdtr.limit = 0x87;
                dpmi_descriptor_t ldt = { .base_lo = 0x8000,
                                         .limit_lo = sizeof(vm->dpmi.ldt) - 1u,
                                         .access = 0x82 };
                cpu8086_cache_ldtr(cpu, 0x80, &ldt);
                for (unsigned i = 0; i < 8; i++)
                    dos_mem_write8(vm, 0x10080u + i, ((uint8_t *)&ldt)[i]);
                for (unsigned i = 0; i < DPMI_MAX_DESCRIPTORS; i++)
                    for (unsigned b = 0; b < 8; b++)
                        dos_mem_write8(vm, 0x8000u + i * 8u + b,
                                       ((uint8_t *)&vm->dpmi.ldt[i])[b]);
            }
            if (vector >= 0 && scenario >= FREE_SELECTOR && scenario <= BAD_TYPE_ABSENT)
                error_code = selector & ~3u;
            if (scenario == GDT_ABSENT) error_code = selector & ~3u;
        }
        if (scenario == REGISTER_SOURCE || scenario == LOCKED) {
            vector = 6;
            error_code = 0;
        }
        uint32_t expected[8];
        uint16_t expected_segments[6];
        for (unsigned i = 0; i < 8; i++) *registers[i] = 0xABCD0123u + i;
        cpu->esp = 0x9000;
        cpu->ebx = cpu->ebp = source_offset;
        for (unsigned i = 0; i < 8; i++) expected[i] = *registers[i];
        for (unsigned i = 0; i < 6; i++) expected_segments[i] = *segments[i];
        dos_mem_write32(vm, address, 0x76543210u);
        dos_mem_write16(vm, address + width, selector);
        dos_mem_write16(vm, address + width + 2u, 0xCCCC);
        uint32_t flags = FLAGS_FIXED | FLAG_IF | FLAG_DF | FLAG_CF | FLAG_OF;
        cpu->eflags = flags;
        cpu->eip = 0x1000;
        uint32_t p = 0x1000;
        if (wide != cpu->op_size_32) vm->mem[p++] = 0x66;
        if (adr32 != cpu->addr_size_32) vm->mem[p++] = 0x67;
        if (source != 2 && source != 3) vm->mem[p++] = prefixes[source];
        if (scenario == LOCKED) vm->mem[p++] = 0xF0;
        if (instruction >= 2) vm->mem[p++] = 0x0F;
        vm->mem[p++] = opcodes[instruction];
        if (scenario == REGISTER_SOURCE) vm->mem[p++] = 0xC0u | (reg << 3);
        else if (source == 2 && adr32) {
            vm->mem[p++] = 0x84u | (reg << 3);
            vm->mem[p++] = 0x24;
            dos_mem_write32(vm, p, source_offset - cpu->esp);
            p += 4;
        } else if (source == 2) {
            vm->mem[p++] = 0x46u | (reg << 3);
            vm->mem[p++] = 0;
        } else vm->mem[p++] = (reg << 3) | (adr32 ? 3u : 7u);
        uint8_t target_access = vm->dpmi.ldt[7].access;
        bool ok = dos_test_run_one(vm);
        if (vector >= 0) {
            unsigned frame_width = mode == 2 ? 4u : 2u;
            expected[4] = mode ? DPMI_EXCEPTION_STACK_SIZE - 8u * frame_width
                               : expected[4] - 6u;
            if (mode) expected_segments[2] = vm->dpmi.sel_exception_stack;
            uint32_t frame = dos_addr(vm, cpu->ss, expected[4]);
            uint32_t saved_ip_addr = frame + (mode ? 3u * frame_width : 0);
            if (cpu->eip != 0x6000u + (unsigned)vector * 16u ||
                (mode == 2 ? dos_mem_read32(vm, saved_ip_addr)
                           : dos_mem_read16(vm, saved_ip_addr)) != 0x1000u ||
                (mode && (vm->dpmi.exception_depth != 1 ||
                 (mode == 2 ? dos_mem_read32(vm, frame + 2u * frame_width)
                            : dos_mem_read16(vm, frame + 2u * frame_width)) != error_code)))
                ok = false;
            faults++;
        } else {
            expected[reg] = wide ? 0x76543210u : (expected[reg] & 0xFFFF0000u) | 0x3210u;
            expected_segments[destination] = selector;
            if (cpu->eip != p || cpu->eflags != flags) ok = false;
            if (mode && (selector & ~3u)) target_access |= 1u;
        }
        for (unsigned i = 0; i < 8; i++) if (*registers[i] != expected[i]) ok = false;
        for (unsigned i = 0; i < 6; i++) if (*segments[i] != expected_segments[i]) ok = false;
        if (dos_mem_read16(vm, address) != 0x3210u ||
            (wide && dos_mem_read16(vm, address + 2u) != 0x7654u) ||
            dos_mem_read16(vm, address + width) != selector ||
            dos_mem_read16(vm, address + width + 2u) != 0xCCCCu) ok = false;
        uint8_t actual_access = mode && (scenario == GDT_TARGET || scenario == GDT_ABSENT)
            ? dos_mem_read8(vm, 0x1003D) : mode && scenario == GUEST_LDT
            ? dos_mem_read8(vm, 0x803D) : vm->dpmi.ldt[7].access;
        if (mode && actual_access != target_access) ok = false;
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-FARPTR] FAIL case="); serial_putdec(checks);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-FARPTR] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

static int dos_selector_instruction_selftest(dos_vm_t *vm,
                                             cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    dpmi_init(vm);
    vm->dpmi.active = true;
    dpmi_descriptor_t code = { .limit_lo = 0xFFFF, .access = 0xFA };
    dpmi_descriptor_t data = { .limit_lo = 0xFFFF, .base_lo = 0x6000, .access = 0xF2 };
    dpmi_descriptor_t ldt = { .limit_lo = 0x17, .base_lo = 0x8000, .access = 0x82 };
    for (unsigned i = 0; i < 8; i++) {
        vm->mem[8u + i] = ((uint8_t *)&code)[i];
        vm->mem[24u + i] = ((uint8_t *)&ldt)[i];
        vm->mem[32u + i] = ((uint8_t *)&data)[i];
    }
    vm->dpmi.descriptor_state[2] = DPMI_DESC_MUTABLE;

    for (unsigned table = 0; table < 3; table++)
    for (unsigned use32 = 0; use32 < 2; use32++)
    for (unsigned access = 0; access < 256; access++)
    for (unsigned cpl = 0; cpl < 4; cpl++)
    for (unsigned rpl = 0; rpl < 4; rpl++)
    for (unsigned query = 0; query < 4; query++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned form = 0; form < 4; form++) {
        cpu8086_init(cpu, vm);
        cpu->protected_mode = cpu->pm_cs_loaded = true;
        cpu->op_size_32 = cpu->addr_size_32 = use32;
        cpu->gdtr.base = 0;
        cpu->gdtr.limit = 0x27;
        if (table == 2) cpu8086_cache_ldtr(cpu, 0x18, &ldt);
        cpu->cs = 8u | cpl;
        code.flags_lim = use32 ? DESC_32BIT : 0;
        code.access = 0x9Au | (cpl << 5);
        cpu8086_cache_cs(cpu, cpu->cs, &code, cpl);
        cpu->ds = 32u | cpl;
        cpu8086_cache_segment(cpu, 3, cpu->ds, &data);
        cpu->eax = 16u | (table == 1 ? 0 : 4u) | rpl;
        dpmi_descriptor_t descriptor = { .limit_lo = 0xABCD,
            .access = access, .flags_lim = use32 ? 0xD5 : 0x51 };
        vm->dpmi.ldt[2] = descriptor;
        uint32_t target = table == 2 ? 0x8010u : 0x10u;
        for (unsigned i = 0; i < 8; i++)
            vm->mem[target + i] = ((uint8_t *)&descriptor)[i];

        /* Intel legacy-mode type tables; presence does not affect queries. */
        bool segment = (access & 0x10u) != 0;
        unsigned type = access & 15u, dpl = (access >> 5) & 3u;
        bool visible = segment && (type & 12u) == 12u;
        visible = visible || (cpl <= dpl && rpl <= dpl);
        bool valid_type = query == 0 ? segment || ((0x1A3Eu >> type) & 1u)
                        : query == 1 ? segment || ((0x0A0Eu >> type) & 1u)
                        : query == 2 ? segment && (type < 8 || (type & 2u))
                                     : segment && type < 8 && (type & 2u);
        uint32_t value = query == 0 ? (access << 8) | (use32 ? 0xD00000u : 0x500000u)
                                   : use32 ? 0x5ABCDFFFu : 0x0001ABCDu;
        bool ok = dos_selector_query_test(vm, cpu, query, wide, form,
                                          visible && valid_type, value);
        for (unsigned i = 0; i < 8; i++)
            if (vm->mem[target + i] != ((uint8_t *)&descriptor)[i]) ok = false;
        checks++;
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-SELECTOR] FAIL case=");
                serial_putdec(checks);
                serial_puts("\n");
            }
            failures++;
        }
    }

    for (unsigned invalid = 0; invalid < 14; invalid++)
    for (unsigned query = 0; query < 4; query++) {
        cpu8086_init(cpu, vm);
        cpu->protected_mode = cpu->pm_cs_loaded = true;
        cpu->cs = 0x0B;
        code.flags_lim = 0;
        code.access = 0xFA;
        cpu8086_cache_cs(cpu, cpu->cs, &code, 3);
        cpu->ds = 0x23;
        cpu8086_cache_segment(cpu, 3, cpu->ds, &data);
        cpu->gdtr.limit = 0x27;
        cpu->eax = invalid < 4 ? invalid : 0x13;
        ldt = (dpmi_descriptor_t){ .limit_lo = 0x17, .base_lo = 0x8000, .access = 0x82 };
        if (invalid == 4) cpu->eax = 0x8Fu; /* free host LDT slot */
        if (invalid == 5) cpu->gdtr.limit = 0x16;
        if (invalid == 6) cpu->gdtr.base = vm->total_mem_size - 20u;
        if (invalid == 7) cpu->gdtr.base = 0xFFFFFFF8u;
        if (invalid >= 8) {
            cpu->eax = 0x17;
        }
        if (invalid == 8) ldt.access &= (uint8_t)~DESC_PRESENT;
        if (invalid == 9) ldt.access = 0x92;
        if (invalid == 10) ldt.limit_lo = 0x16;
        if (invalid == 11) dpmi_desc_set_base(&ldt, vm->total_mem_size - 20u);
        if (invalid == 12) dpmi_desc_set_base(&ldt, 0xFFFFFFF8u);
        /* These fixtures describe unusable or bounded cached LDTs; LLDT
         * rejection of bad GDT descriptors is tested at the load itself. */
        if (invalid >= 8)
            cpu8086_cache_ldtr(cpu, 0x18, invalid == 13 ? NULL : &ldt);
        for (unsigned i = 0; i < 8; i++) vm->mem[24u + i] = ((uint8_t *)&ldt)[i];
        /* Keep CS executable independently of the descriptor being queried.
         * Linear table addresses wrap at 32 bits; unbacked physical bytes
         * read FF rather than turning the lookup into a selector error. */
        for (unsigned i = 0; i < 8; i++) {
            vm->mem[8u + i] = ((uint8_t *)&code)[i];
            dos_mem_write8(vm, cpu->gdtr.base + 8u + i, ((uint8_t *)&code)[i]);
        }
        bool bus_tail = invalid == 6 || invalid == 11;
        bool wrapped = invalid == 7 || invalid == 12;
        if (bus_tail) {
            dos_mem_write32(vm, vm->total_mem_size - 4u, 0xABCDu);
        }
        bool valid = (bus_tail || wrapped) && query < 3u;
        uint32_t value = query == 0 ? bus_tail ? 0xF0FF00u : 0xFA00u
                                   : bus_tail ? 0xFABCDFFFu : 0xFFFFu;
        checks++;
        if (!dos_selector_query_test(vm, cpu, query, true, 0, valid, value)) {
            serial_puts("[DOS-SELECTOR] boundary mismatch scenario="); serial_putdec(invalid);
            serial_puts(" query="); serial_putdec(query); serial_puts("\n");
            failures++;
        }
    }

    /* Illegal real-mode queries fault at the first prefix, recoverably. */
    dpmi_init(vm);
    dos_mem_write32(vm, 6u * 4u, 0x00002000u);
    for (unsigned query = 0; query < 4; query++) {
        cpu8086_init(cpu, vm);
        cpu->cs = 0;
        cpu->eip = 0x1000;
        cpu->ss = 0x200;
        cpu->sp = 0x8000;
        cpu->edx = 0xA5A55A5Au;
        vm->mem[0x1000] = 0x66;
        vm->mem[0x1001] = 0x0F;
        vm->mem[0x1002] = query < 2 ? query + 2u : 0;
        vm->mem[0x1003] = query < 2 ? 0xD0 : 0xE0 + (query - 2u) * 8u;
        bool reached = dos_test_run_until_real(vm, 0, 0x2000);
        checks++;
        if (!reached || cpu->edx != 0xA5A55A5Au || cpu->sp != 0x7FFAu ||
            dos_mem_read16(vm, 0x9FFAu) != 0x1000u) failures++;
    }
    for (unsigned forbidden = 0; forbidden < 2; forbidden++)
    for (unsigned query = 0; query < 4; query++) {
        dpmi_init(vm);
        const uint16_t code_sel = dpmi_index_to_sel(1);
        const uint16_t stack_sel = dpmi_index_to_sel(2);
        dos_test_descriptor(&vm->dpmi, code_sel, true, false);
        dos_test_descriptor(&vm->dpmi, stack_sel, false, false);
        vm->dpmi.active = true;
        vm->dpmi.exception_vectors[6].sel = code_sel;
        vm->dpmi.exception_vectors[6].off = 0x2000;
        cpu8086_init(cpu, vm);
        cpu->protected_mode = cpu->pm_cs_loaded = true;
        cpu->cs = code_sel;
        cpu->ss = stack_sel;
        cpu->sp = 0x8000;
        cpu->eip = 0x1000;
        cpu->eax = stack_sel;
        cpu->edx = 0xA5A55A5Au;
        cpu->eflags = FLAGS_FIXED | (forbidden ? FLAG_VM : 0);
        vm->mem[0x1000] = forbidden ? 0x66 : 0xF0;
        vm->mem[0x1001] = 0x0F;
        vm->mem[0x1002] = query < 2 ? query + 2u : 0;
        vm->mem[0x1003] = query < 2 ? 0xD0 : 0xE0 + (query - 2u) * 8u;
        bool reached = dos_test_run_until(vm, true, code_sel, 0x2000);
        uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
        if (!reached || cpu->edx != 0xA5A55A5Au ||
            cpu->ss != vm->dpmi.sel_exception_stack ||
            cpu->sp != DPMI_EXCEPTION_STACK_SIZE - 16u ||
            dos_mem_read16(vm, frame + 6u) != 0x1000u) failures++;
        checks++;
    }
    serial_puts("[DOS-SELECTOR] checks=");
    serial_putdec(checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

static void dos_step_test_prepare(dos_vm_t *vm, cpu8086_state_t *cpu,
                                   unsigned mode, uint64_t limit)
{
    cpu8086_init(cpu, vm);
    dpmi_init(vm);
    vm->emulate_cpu = true;
    vm->step_limit = limit;
    vm->step_count = 0;
    vm->step_limit_reached = false;
    vm->process_terminated = false;
    vm->interpreter_stop_active = false;
    vm->interpreter_stop_reached = false;
    vm->start_ticks = vm->last_timer_tick = idt_get_ticks();
    vm->timer_irq_pending = false;
    vm->jit = NULL;
    vm->dpmi.active = mode != 0;
    vm->dpmi.is_32bit = mode == 2;
    vm->dpmi.virtual_interrupts_enabled = false;
    cpu->protected_mode = cpu->pm_cs_loaded = mode != 0;
    cpu->op_size_32 = cpu->addr_size_32 = mode == 2;
    if (mode) {
        cpu->cr0 |= 1u;
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(1), true, mode == 2);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(2), false, mode == 2);
        cpu->cs = dpmi_index_to_sel(1);
        cpu->ds = cpu->es = cpu->ss = dpmi_index_to_sel(2);
    }
    cpu->eip = 0x1000;
    cpu->esp = 0x9000;
}

static uint32_t dos_step_test_jit(cpu8086_state_t *cpu)
{
    cpu->running = false;
    cpu->exit_code = 99;
    return 0;
}

static int dos_step_limit_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const unsigned limits[] = { 1, 2, 3, 49, 50, 51, 256, 1000 };
    int failures = 0;
    unsigned checks = 0;
    for (unsigned mode = 0; mode < 3; mode++) {
        for (unsigned i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
            unsigned limit = limits[i];
            dos_step_test_prepare(vm, cpu, mode, limit);
            vm->mem[0x1000] = 0x40; /* INC AX/EAX; JMP back. */
            vm->mem[0x1001] = 0xEB;
            vm->mem[0x1002] = 0xFD;
            if (dos_test_run(vm) != -1 || cpu->running ||
                !vm->step_limit_reached || vm->step_count != limit ||
                cpu->insn_count != limit || cpu->eax != (limit + 1u) / 2u ||
                cpu->eip != 0x1000u + (limit & 1u)) failures++;
            checks++;

            /* Restoring a CPU snapshot must not revive a stopped session. */
            cpu8086_init(cpu, vm);
            cpu->eip = 0x1000;
            if (dos_test_run(vm) != -1 || cpu->running || cpu->insn_count ||
                cpu->eip != 0x1000 || vm->step_count != limit) failures++;
            checks++;
        }

        dos_step_test_prepare(vm, cpu, mode, 3);
        vm->mem[0x1000] = vm->mem[0x1001] = vm->mem[0x1002] = 0x90;
        if (!dos_test_run_until(vm, mode != 0, cpu->cs, 0x1003) ||
            vm->step_limit_reached || vm->step_count != 3 || !cpu->running)
            failures++;
        checks++;
        if (dos_test_run_one(vm) || !vm->step_limit_reached ||
            cpu->eip != 0x1003 || cpu->insn_count != 3 || vm->step_count != 3)
            failures++;
        checks++;

        /* Unlimited mode does not meter dispatches, even above the old cap. */
        dos_step_test_prepare(vm, cpu, mode, 0);
        cpu->insn_count = 499999999u;
        vm->step_count = 123;
        if (!dos_test_run_until(vm, mode != 0, cpu->cs, 0x1003) ||
            cpu->insn_count != 500000002u || vm->step_count != 123 ||
            vm->step_limit_reached) failures++;
        checks++;

        dos_step_test_prepare(vm, cpu, mode, UINT64_MAX);
        vm->step_count = UINT64_MAX - 1u;
        if (dos_test_run(vm) != -1 || vm->step_count != UINT64_MAX ||
            cpu->insn_count != 1 || cpu->eip != 0x1001) failures++;
        checks++;

        /* REP is a bounded interpreter dispatch, not a retired-instruction
         * counter: each step performs at most 256 string iterations. */
        for (unsigned limit = 1; limit <= 3; limit++) {
            dos_step_test_prepare(vm, cpu, mode, limit);
            vm->mem[0x1000] = 0xF3; vm->mem[0x1001] = 0xAA;
            for (unsigned i = 0; i < 514; i++) vm->mem[0x4000 + i] = 0;
            cpu->ecx = 513;
            cpu->edi = 0x4000;
            cpu->al = 0xA5;
            bool reached = dos_test_run_until(vm, mode != 0, cpu->cs, 0x1002);
            unsigned written = limit == 3 ? 513 : limit * 256;
            bool ok = reached == (limit == 3) && vm->step_count == limit &&
                vm->step_limit_reached == (limit < 3) &&
                cpu->ecx == 513u - written && cpu->edi == 0x4000u + written;
            for (unsigned i = 0; i < 514; i++)
                if (vm->mem[0x4000 + i] != (i < written ? 0xA5 : 0)) ok = false;
            if (!ok) {
                serial_puts("[DOS-STEPS] REP store mismatch mode="); serial_putdec(mode);
                serial_puts(" limit="); serial_putdec(limit); serial_puts("\n");
                failures++;
            }
            checks++;
        }
        const unsigned mismatches[] = { 0, 255, 256, 512, 513 };
        for (unsigned sample = 0; sample < sizeof(mismatches) / sizeof(mismatches[0]); sample++)
        for (unsigned limit = 1; limit <= 3; limit++) {
            dos_step_test_prepare(vm, cpu, mode, limit);
            vm->mem[0x1000] = 0xF3; vm->mem[0x1001] = 0xAE;
            for (unsigned i = 0; i < 513; i++)
                vm->mem[0x4000 + i] = i == mismatches[sample] ? 0 : 0xA5;
            cpu->ecx = 513;
            cpu->edi = 0x4000;
            cpu->al = 0xA5;
            unsigned target = mismatches[sample] < 513 ? mismatches[sample] + 1 : 513;
            unsigned consumed = target < limit * 256u ? target : limit * 256u;
            bool expected_reached = consumed == target;
            bool reached = dos_test_run_until(vm, mode != 0, cpu->cs, 0x1002);
            if (reached != expected_reached ||
                vm->step_limit_reached != !expected_reached ||
                vm->step_count != (consumed + 255u) / 256u ||
                cpu->ecx != 513u - consumed || cpu->edi != 0x4000u + consumed ||
                !!(cpu->eflags & FLAG_ZF) != (consumed <= mismatches[sample])) {
                serial_puts("[DOS-STEPS] REP compare mismatch mode="); serial_putdec(mode);
                serial_puts(" limit="); serial_putdec(limit);
                serial_puts(" sample="); serial_putdec(sample); serial_puts("\n");
                failures++;
            }
            checks++;
        }
    }

    /* A normal DOS exit on the final step is not quota exhaustion. */
    for (unsigned limit = 1; limit <= 2; limit++) {
        dos_step_test_prepare(vm, cpu, 0, limit);
        const uint8_t exit_program[] = { 0xB8, 0x2A, 0x4C, 0xCD, 0x21 };
        for (unsigned i = 0; i < sizeof(exit_program); i++)
            vm->mem[0x1000 + i] = exit_program[i];
        if (dos_test_run(vm) != (limit == 2 ? 42 : -1) ||
            vm->step_limit_reached != (limit == 1) || vm->step_count != limit)
            failures++;
        checks++;
    }
    dos_step_test_prepare(vm, cpu, 0, 1);
    vm->mem[0x1000] = 0xF4;
    if (dos_test_run(vm) != -1 || !cpu->halted || !vm->step_limit_reached ||
        vm->step_count != 1 || cpu->eip != 0x1001) failures++;
    checks++;

    /* The outer INT reserves its step before the real-mode FAR call. A
     * quota-stopped callee must not publish a register image before RETF. */
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned limit = 1; limit <= 4; limit++) {
        dos_step_test_prepare(vm, cpu, mode, limit);
        vm->mem[0x1000] = 0xCD; vm->mem[0x1001] = 0x31;
        vm->mem[0x1002] = 0x43; /* INC BX/EBX after the service returns. */
        vm->mem[0x2000] = 0x40; vm->mem[0x2001] = 0xCB;
        dpmi_rm_regs_t regs = {0};
        regs.flags = FLAGS_FIXED;
        regs.ip = 0x2000;
        for (unsigned i = 0; i < sizeof(regs); i++)
            vm->mem[0x3000 + i] = ((uint8_t *)&regs)[i];
        cpu->ax = 0x0301;
        cpu->edi = 0x3000;
        bool reached = dos_test_run_until(vm, true, cpu->cs, 0x1003);
        if (reached != (limit == 4) || vm->step_count != limit ||
            vm->step_limit_reached != (limit < 4) || !cpu->protected_mode ||
            cpu->ebx != (limit == 4 ? 1u : 0) ||
            dos_mem_read32(vm, 0x3000 + 28u) != (limit >= 3 ? 1u : 0)) {
            serial_puts("[DOS-STEPS] nested call mismatch mode="); serial_putdec(mode);
            serial_puts(" limit="); serial_putdec(limit); serial_puts("\n");
            failures++;
        }
        checks++;
    }

    uint64_t jit_pages = (sizeof(jit_state_t) + 4095u) / 4096u;
    jit_state_t *jit = (jit_state_t *)dos_host_alloc_pages(jit_pages);
    if (!jit) {
        failures++;
    } else {
        for (uint64_t i = 0; i < jit_pages * 4096u; i++) ((uint8_t *)jit)[i] = 0;
        jit_block_t *block = jit_get_block(jit, 0, 0x1000);
        block->compiled = true;
        block->native_code = (uint8_t *)dos_step_test_jit;
        jit->hit_count[0x1000] = JIT_HOT_THRESHOLD;
        dos_step_test_prepare(vm, cpu, 0, 1);
        vm->jit = jit;
        vm->mem[0x1000] = 0x90;
        if (dos_test_run(vm) != -1 || cpu->insn_count != 1 ||
            vm->step_count != 1 || jit->jit_executed || block->exec_count)
            failures++;
        checks++;
        dos_step_test_prepare(vm, cpu, 0, 0);
        vm->jit = jit;
        if (dos_test_run(vm) != 99 || cpu->insn_count || vm->step_count ||
            jit->jit_executed != 1 || block->exec_count != 1) failures++;
        checks++;
        vm->jit = NULL;
        dos_host_free_pages(jit, jit_pages);
    }
    dos_step_test_prepare(vm, cpu, 0, 0);
    vm->emulate_cpu = false;
    serial_puts("[DOS-STEPS] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_jit_cpu_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint64_t pages = (sizeof(jit_state_t) + 4095u) / 4096u;
    jit_state_t *jit = (jit_state_t *)dos_host_alloc_pages(pages);
    unsigned checks = 0, failures = 0;
#define JIT_CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; serial_puts("[DOS-JIT] FAIL: " #condition "\n"); \
    } \
} while (0)
    JIT_CHECK(jit != NULL);
    if (!jit) goto done;
    jit_init(jit);
    JIT_CHECK(jit->code_buf != NULL);
    if (!jit->code_buf) goto cleanup;

    static const uint8_t operations[] = {
        0x01, 0x03, 0x29, 0x2B, 0x39, 0x3B,
        0x31, 0x33, 0x21, 0x23, 0x09, 0x0B, 0x89, 0x8B, 0x40, 0x48
    };
    static const uint32_t values[] = {
        0, 1, 0x7FFFu, 0x8000u, 0xFFFFu, 0x7FFFFFFFu, 0x80000000u, UINT32_MAX
    };
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned override = 0; override < 2; override++)
    for (unsigned op = 0; op < sizeof(operations); op++)
    for (unsigned value = 0; value < sizeof(values) / sizeof(values[0]); value++) {
        dos_step_test_prepare(vm, cpu, mode, 0);
        uint32_t ip = mode == 2 ? 0x11000u : 0x1000u;
        if (mode) dpmi_desc_set_limit(&vm->dpmi.ldt[1], 0x1FFFFu);
        cpu->eip = ip;
        cpu->eax = values[value];
        cpu->ecx = values[(value + 3u) % (sizeof(values) / sizeof(values[0]))];
        cpu->eflags = FLAGS_FIXED | FLAG_DF | FLAG_CF;
        unsigned pos = 0;
        if (override) vm->mem[ip + pos++] = 0x66;
        uint8_t opcode = operations[op];
        vm->mem[ip + pos++] = opcode;
        if (opcode != 0x40 && opcode != 0x48)
            vm->mem[ip + pos++] = (opcode & 2u) ? 0xC1 : 0xC8;
        vm->mem[ip + pos] = 0xF4; /* Interpreter terminator, not executed. */
        cpu8086_state_t before = *cpu;
        JIT_CHECK(dos_test_run_one(vm));
        cpu8086_state_t expected = *cpu;
        *cpu = before;
        jit_block_t *block = jit_get_block(jit, cpu->cs, ip);
        JIT_CHECK(dos_test_jit_decode(vm, block) > 0 &&
                  block->instruction_count == 1 && block->length == pos &&
                  jit_compile_block(jit, block) == 0);
        uint64_t host_before, host_after;
        __asm__ volatile ("pushfq; popq %0" : "=r"(host_before) :: "memory");
        bool completed = jit_exec_block(vm, block);
        __asm__ volatile ("pushfq; popq %0" : "=r"(host_after) :: "memory");
        bool logical = opcode == 0x31 || opcode == 0x33 || opcode == 0x21 ||
                       opcode == 0x23 || opcode == 0x09 || opcode == 0x0B;
        uint32_t mask = logical ? ~FLAG_AF : UINT32_MAX; /* AF is undefined. */
        JIT_CHECK(!completed && cpu->eax == expected.eax &&
                  cpu->ecx == expected.ecx && cpu->eip == expected.eip &&
                  cpu->insn_count == expected.insn_count &&
                  ((cpu->eflags ^ expected.eflags) & mask) == 0);
        JIT_CHECK(((host_before ^ host_after) &
                   (FLAG_IF | FLAG_DF | FLAG_TF | FLAG_IOPL_MASK)) == 0);
    }

    /* Full EIP keys, carry/borrow across 64 KiB, taken and not taken. */
    static const uint32_t starts[] = { 0xFFF8u, 0x10008u, 0x1FFF8u };
    const uint8_t branches[] = { 0xEB, 0xE9, 0x74, 0x75 };
    for (unsigned b = 0; b < sizeof(branches); b++)
    for (unsigned at = 0; at < sizeof(starts) / sizeof(starts[0]); at++)
    for (unsigned direction = 0; direction < 2; direction++)
    for (unsigned zf = 0; zf < 2; zf++) {
        dos_step_test_prepare(vm, cpu, 2, 0);
        dpmi_desc_set_limit(&vm->dpmi.ldt[1], 0x2FFFFu);
        uint32_t ip = starts[at], length = branches[b] == 0xE9 ? 5u : 2u;
        int32_t relative = direction ? 16 : -16;
        cpu->eip = ip;
        cpu->eflags = FLAGS_FIXED | (zf ? FLAG_ZF : 0);
        vm->mem[ip] = branches[b];
        for (unsigned i = 1; i < length; i++)
            vm->mem[ip + i] = (uint32_t)relative >> (8u * (i - 1u));
        bool taken = b < 2 || (b == 2 ? zf != 0 : zf == 0);
        uint32_t expected = ip + length + (taken ? (uint32_t)relative : 0);
        cpu8086_state_t before = *cpu;
        JIT_CHECK(dos_test_run_one(vm) && cpu->eip == expected);
        *cpu = before;
        jit_block_t *block = jit_get_block(jit, cpu->cs, ip);
        JIT_CHECK(dos_test_jit_decode(vm, block) > 0 &&
                  jit_compile_block(jit, block) == 0);
        JIT_CHECK(jit_exec_block(vm, block) && cpu->eip == expected &&
                  cpu->insn_count == 1);
    }

    /* MOV immediates preserve the other register bits at each width. */
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned override = 0; override < 2; override++) {
        dos_step_test_prepare(vm, cpu, mode, 0);
        bool wide = (mode == 2) != (override != 0);
        unsigned pos = 0;
        if (override) vm->mem[0x1000 + pos++] = 0x66;
        vm->mem[0x1000 + pos++] = 0xB8;
        for (unsigned i = 0; i < (wide ? 4u : 2u); i++)
            vm->mem[0x1000 + pos++] = 0x12345678u >> (i * 8u);
        vm->mem[0x1000 + pos++] = 0xB4;
        vm->mem[0x1000 + pos++] = 0xAB; /* AH */
        vm->mem[0x1000 + pos] = 0xF4;
        cpu->eax = 0x87654321u;
        jit_block_t *block = jit_get_block(jit, cpu->cs, 0x1000);
        JIT_CHECK(dos_test_jit_decode(vm, block) > 0 &&
                  jit_compile_block(jit, block) == 0);
        JIT_CHECK(!jit_exec_block(vm, block) &&
                  cpu->eax == (wide ? 0x1234AB78u : 0x8765AB78u) &&
                  cpu->insn_count == 2 && cpu->eip == 0x1000 + pos);
    }

    dos_step_test_prepare(vm, cpu, 2, 0);
    dpmi_descriptor_t *descriptor = &vm->dpmi.ldt[1];
    dpmi_desc_set_limit(descriptor, 0x2FFFFu);
    vm->mem[0x1000] = 0x40; vm->mem[0x1001] = 0xF4;
    jit_block_t *block = jit_get_block(jit, cpu->cs, 0x1000);
    JIT_CHECK(dos_test_jit_decode(vm, block) > 0 &&
              jit_compile_block(jit, block) == 0 && jit_block_current(vm, block));
    JIT_CHECK(jit_get_block(jit, cpu->cs, 0x11000) != block &&
              jit_get_block(jit, cpu->cs, 0x1000) == block);
    vm->mem[0x1000] = 0x48;
    JIT_CHECK(!jit_block_current(vm, block) && !jit_exec_block(vm, block) &&
              cpu->eax == 0 && cpu->insn_count == 0);
    vm->mem[0x1000] = 0x40;
    vm->mem[0x1001] = 0x90;
    JIT_CHECK(!jit_block_current(vm, block));
    vm->mem[0x1001] = 0xF4;
    dpmi_desc_set_base(descriptor, 0x10000);
    JIT_CHECK(jit_block_current(vm, block));
    cpu8086_sync_cs(cpu);
    JIT_CHECK(!jit_block_current(vm, block));
    dpmi_desc_set_base(descriptor, 0);
    dpmi_desc_set_limit(descriptor, 0x20000);
    cpu8086_sync_cs(cpu);
    JIT_CHECK(!jit_block_current(vm, block));
    dpmi_desc_set_limit(descriptor, 0x2FFFFu);
    descriptor->flags_lim ^= DESC_32BIT;
    cpu8086_sync_cs(cpu);
    JIT_CHECK(!jit_block_current(vm, block));
    descriptor->flags_lim ^= DESC_32BIT;
    descriptor->access &= ~DESC_PRESENT;
    cpu8086_sync_cs(cpu);
    JIT_CHECK(!jit_block_current(vm, block));
    descriptor->access |= DESC_PRESENT;
    cpu8086_sync_cs(cpu);
    cpu->cr0 |= 0x80000000u;
    JIT_CHECK(!jit_block_current(vm, block));
    cpu->cr0 &= ~0x80000000u;
    JIT_CHECK(jit_block_current(vm, block));

    /* A bounded decode may not consume a partial instruction or its prefixes. */
    for (unsigned remaining = 1; remaining < 5; remaining++) {
        vm->mem[0x1000] = 0xB8;
        dpmi_desc_set_limit(descriptor, 0x1000 + remaining - 1u);
        JIT_CHECK(dos_test_jit_decode(vm, block) > 0 &&
                  !block->instruction_count && !block->length);
    }
    dpmi_desc_set_limit(descriptor, 0x2FFFFu);
    for (unsigned i = 0; i < 16; i++) vm->mem[0x1000 + i] = 0x66;
    vm->mem[0x1010] = 0x90;
    JIT_CHECK(dos_test_jit_decode(vm, block) > 0 &&
              !block->instruction_count && !block->length);
    vm->mem[0x1000] = 0x66; vm->mem[0x1001] = 0x66;
    vm->mem[0x1002] = 0x40; vm->mem[0x1003] = 0xF4;
    cpu->eax = 0xABCDFFFFu;
    JIT_CHECK(dos_test_jit_decode(vm, block) > 0 &&
              jit_compile_block(jit, block) == 0 &&
              !jit_exec_block(vm, block) && cpu->eax == 0xABCD0000u);

    /* Read-ahead stops before device windows; banked code is interpreted. */
    const uint32_t windows[] = { DOS_VGA_APERTURE_BASE, DOS_EMS_PAGE_FRAME_BASE };
    dpmi_desc_set_limit(descriptor, vm->total_mem_size - 1u);
    for (unsigned i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
        uint32_t start = windows[i];
        vm->mem[start - 1u] = 0xB8;
        block = jit_get_block(jit, cpu->cs, start - 1u);
        JIT_CHECK(dos_test_jit_decode(vm, block) > 0 && !block->instruction_count &&
                  block->source_size == 1 && block->source_limit == start - 1u);
        block = jit_get_block(jit, cpu->cs, start);
        JIT_CHECK(dos_test_jit_decode(vm, block) < 0 && !block->compiled);
    }

    /* A hot block may not run through a suspended callback's stop cursor. */
    for (unsigned stop = 1; stop <= 2; stop++) {
        dos_step_test_prepare(vm, cpu, 2, 0);
        vm->mem[0x1000] = 0x40; vm->mem[0x1001] = 0x40;
        vm->mem[0x1002] = 0x50;
        block = jit_get_block(jit, cpu->cs, 0x1000);
        JIT_CHECK(dos_test_jit_decode(vm, block) > 0 &&
                  jit_compile_block(jit, block) == 0);
        vm->jit = jit;
        jit->hit_count[0x1000] = JIT_HOT_THRESHOLD;
        JIT_CHECK(dos_test_run_until(vm, true, cpu->cs, 0x1000 + stop) &&
                  cpu->eax == stop && cpu->esp == 0x9000 &&
                  cpu->insn_count == stop);
    }

    /* Cross, rather than land on, the 16K boundary with generated code. */
    dos_step_test_prepare(vm, cpu, 2, 0);
    vm->mem[0x1000] = 0x40; vm->mem[0x1001] = 0x40;
    vm->mem[0x1002] = 0xEB; vm->mem[0x1003] = 0x0C;
    block = jit_get_block(jit, cpu->cs, 0x1000);
    JIT_CHECK(dos_test_jit_decode(vm, block) > 0 &&
              jit_compile_block(jit, block) == 0);
    vm->jit = jit;
    vm->bios_ticks = UINT32_MAX;
    cpu->insn_count = 0x3FFEu;
    jit->hit_count[0x1000] = JIT_HOT_THRESHOLD;
    uint64_t translated = jit->jit_instructions;
    JIT_CHECK(dos_test_run_until(vm, true, cpu->cs, 0x1010) &&
              cpu->eax == 2 && cpu->insn_count == 0x4001u &&
              jit->jit_instructions == translated + 3u &&
              vm->bios_ticks != UINT32_MAX);

    jit_invalidate_all(jit);
    JIT_CHECK(!block->compiled && jit->block_count == 0 && jit->code_used == 0);
    failures += (unsigned)jit_cache_selftest(vm, jit);

cleanup:
    vm->jit = NULL;
    jit_destroy(jit);
    dos_host_free_pages(jit, pages);
    dos_step_test_prepare(vm, cpu, 0, 0);
    vm->emulate_cpu = false;
done:
    serial_puts("[DOS-JIT] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef JIT_CHECK
    return (int)failures;
}

static void dos_port_test_prepare(dos_vm_t *vm, cpu8086_state_t *cpu,
                                   unsigned mode)
{
    dos_step_test_prepare(vm, cpu, mode, 0);
    const unsigned indices[] = { 4, 1, 2, 3, 5, 6 };
    const uint32_t bases[] = { 0x20000, 0, 0, 0x10000, 0x30000, 0x40000 };
    uint16_t *segments[] = { &cpu->es, &cpu->cs, &cpu->ss,
                            &cpu->ds, &cpu->fs, &cpu->gs };
    for (unsigned i = 0; i < 6; i++) {
        *segments[i] = mode ? dpmi_index_to_sel(indices[i]) : bases[i] >> 4;
        if (mode) {
            dos_test_descriptor(&vm->dpmi, *segments[i], i == 1, mode == 2);
            dpmi_desc_set_base(&vm->dpmi.ldt[indices[i]], bases[i]);
        }
    }
    for (unsigned vector = 0; vector < 32; vector++) {
        dos_mem_write32(vm, vector * 4u, 0x6000u + vector * 16u);
        vm->dpmi.exception_vectors[vector].sel = cpu->cs;
        vm->dpmi.exception_vectors[vector].off = 0x6000u + vector * 16u;
    }
    dos_io_write8(vm, 0x3C8, 0);
    for (unsigned i = 0; i < 768; i++) dos_io_write8(vm, 0x3C9, i & 63u);
    dos_io_write8(vm, 0x3C8, 0);
    dos_io_write8(vm, 0x3C7, 0);
    dos_io_write8(vm, 0x3DA, 0x35);
    dos_io_write8(vm, 0x3C2, 0x6B);
}

static int dos_scalar_port_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    if (!dos_audio_init(vm)) return 1;
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned input = 0; input < 2; input++)
    for (unsigned width = 1; width <= 4; width *= 2)
    for (unsigned immediate = 0; immediate < 2; immediate++)
    for (unsigned locked = 0; locked < 2; locked++) {
        dos_port_test_prepare(vm, cpu, mode);
        dos_io_write8(vm, 0x83, 0x25);
        dos_io_write8(vm, 0x3C4, 2);
        dos_io_write8(vm, 0x3C5, 0x24);
        dos_io_write8(vm, 0x3C6, 0x5A);
        cpu->eax = 0x6B553702u;
        cpu->edx = 0xABCD03C4u;
        cpu->ecx = 0x56781234u;
        cpu->esi = cpu->edi = 0x98764321u;
        uint32_t flags = FLAGS_FIXED | FLAG_DF | FLAG_CF | FLAG_ZF | FLAG_OF;
        cpu->eflags = flags;
        uint32_t p = 0x1000;
        if (locked) vm->mem[p++] = 0xF0;
        if ((width == 4) != cpu->op_size_32) {
            vm->mem[p++] = 0x66;
            vm->mem[p++] = 0x66;
        }
        vm->mem[p++] = 0x67;
        vm->mem[p++] = 0xF3;
        vm->mem[p++] = (immediate ? 0xE4 : 0xEC) | (input ? 0 : 2) | (width != 1);
        if (immediate) vm->mem[p++] = 0x80;
        uint32_t mask = width == 1 ? 0xFFu : width == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        uint32_t expected = input && !locked
            ? (cpu->eax & ~mask) | ((immediate ? 0x25FFFFFFu : 0x035A2402u) & mask)
            : cpu->eax;
        bool ok = dos_test_run_one(vm) && cpu->eax == expected &&
            cpu->edx == 0xABCD03C4u && cpu->ecx == 0x56781234u &&
            cpu->esi == 0x98764321u && cpu->edi == 0x98764321u;
        if (locked) {
            uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
            uint32_t saved = frame + (mode == 2 ? 12u : mode ? 6u : 0);
            if (cpu->eip != 0x6060u ||
                (mode == 2 ? dos_mem_read32(vm, saved) : dos_mem_read16(vm, saved)) != 0x1000u)
                ok = false;
        } else if (cpu->eip != p || cpu->eflags != flags) ok = false;
        bool wrote = !input && !locked;
        if (dos_io_read8(vm, 0x83) != (wrote && immediate && width == 4 ? 0x6B : 0x25) ||
            dos_io_read8(vm, 0x3C5) != (wrote && !immediate && width >= 2 ? 0x37 : 0x24) ||
            dos_io_read8(vm, 0x3C6) != (wrote && !immediate && width == 4 ? 0x55 : 0x5A)) ok = false;
        if (!ok) {
            serial_puts("[DOS-PORT-SCALAR] mismatch case="); serial_putdec(checks); serial_puts("\n");
            failures++;
        }
        checks++;
    }
    dos_audio_shutdown(vm);
    serial_puts("[DOS-PORT-SCALAR] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_page_walk_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint32_t linears[] = { 0, 0x6FF0, 0x3FFFFF, 0x403FFFF0, 0xFFFFFABC };
    unsigned checks = 0;
    int failures = 0;
    cpu8086_init(cpu, vm);
    cpu->cr2 = 0xCAFE1234u;
    for (unsigned address = 0; address < sizeof(linears) / sizeof(linears[0]); address++)
    for (unsigned pd_flags = 0; pd_flags < 8; pd_flags++)
    for (unsigned pt_flags = 0; pt_flags < 8; pt_flags++)
    for (unsigned access = 0; access < 8; access += 2)
    for (unsigned wp = 0; wp < 2; wp++)
    for (unsigned ignored = 0; ignored < 2; ignored++) {
        uint32_t linear = linears[address];
        uint32_t pde = 0x10000u + (linear >> 22) * 4u;
        uint32_t pte = 0x11000u + ((linear >> 12) & 0x3FFu) * 4u;
        uint32_t pd = 0x11000u | pd_flags | (ignored ? 0xF98u : 0);
        uint32_t pt = 0x24000u | pt_flags | (ignored ? 0xF98u : 0);
        dos_mem_write32(vm, pde, pd);
        dos_mem_write32(vm, pte, pt);
        cpu->cr0 = 1u | DOS_CR0_PG | (wp ? DOS_CR0_WP : 0);
        cpu->cr3 = 0x10018u;
        bool present = (pd_flags & 1u) && (pt_flags & 1u);
        bool writable = (pd_flags & 2u) && (pt_flags & 2u);
        bool user_page = (pd_flags & 4u) && (pt_flags & 4u);
        bool write = (access & DOS_PAGE_WRITE) != 0;
        bool user = (access & DOS_PAGE_USER) != 0;
        bool allowed = present && (!user || user_page) &&
                       (!write || writable || (!user && !wp));
        dos_page_translation_t translation = { 0xA5A5, 0x5A5A, 0x1234 };
        dos_page_fault_t fault = { 0xAAAA, 0xBBBB, true };
        bool ok = dos_page_probe(vm, linear, access, &translation, &fault) == allowed &&
            dos_mem_read32(vm, pde) == pd && dos_mem_read32(vm, pte) == pt &&
            cpu->cr2 == 0xCAFE1234u;
        if (allowed) {
            if (fault.raised || translation.physical != (0x24000u | (linear & 0xFFFu)) ||
                translation.pde != pde || translation.pte != pte) ok = false;
            dos_page_commit(vm, &translation, write);
            if (dos_mem_read32(vm, pde) != (pd | 0x20u) ||
                dos_mem_read32(vm, pte) != (pt | (write ? 0x60u : 0x20u))) ok = false;
        } else if (!fault.raised || fault.linear != linear ||
                   fault.error != (access | (present ? 1u : 0)) ||
                   translation.physical != 0xA5A5 || translation.pde != 0x5A5A ||
                   translation.pte != 0x1234) ok = false;
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-PAGING] mismatch case="); serial_putdec(checks); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    /* No paging is identity even beyond RAM. An unbacked physical bus read
     * is not a missing linear mapping and must not manufacture #PF or wrap. */
    cpu->cr0 = DOS_CR0_WP;
    for (unsigned i = 0; i < sizeof(linears) / sizeof(linears[0]); i++) {
        dos_page_translation_t translation;
        dos_page_fault_t fault;
        if (!dos_page_probe(vm, linears[i], DOS_PAGE_USER | DOS_PAGE_WRITE,
                            &translation, &fault) || fault.raised ||
            translation.physical != linears[i] || translation.pde != UINT32_MAX ||
            translation.pte != UINT32_MAX) failures++;
        else dos_page_commit(vm, &translation, true);
        checks++;
    }
    cpu->cr0 = 1u | DOS_CR0_PG;
    cpu->cr3 = 0x10000;
    dos_mem_write32(vm, 0x10000, 0x11007);
    dos_mem_write32(vm, 0x11000, vm->total_mem_size | 7u);
    dos_page_translation_t translation;
    dos_page_fault_t fault;
    if (!dos_page_probe(vm, 0, DOS_PAGE_USER, &translation, &fault) || fault.raised ||
        translation.physical != vm->total_mem_size ||
        dos_mem_read8(vm, translation.physical) != 0xFF || cpu->cr2 != 0xCAFE1234u)
        failures++;
    checks++;
    serial_puts("[DOS-PAGING] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

typedef struct {
    uint32_t pde[2], pte[2];
} dos_test_page_pair_t;

static dos_test_page_pair_t dos_test_page_pair(dos_vm_t *vm, uint32_t linear)
{
    for (unsigned i = 0; i < 0x4000u; i++) vm->mem[0x10000u + i] = 0;
    vm->cpu->cr0 |= DOS_CR0_PG | DOS_CR0_WP;
    vm->cpu->cr3 = 0x10000u;
    /* Code, host stubs and exception stack remain identity-mapped. Only
     * the checked operand is relocated, never a native host return frame. */
    dos_mem_write32(vm, 0x10000, 0x11007);
    for (unsigned i = 0; i < 512; i++) dos_mem_write32(vm, 0x11000 + i * 4u, i * 4096u | 7u);
    dos_test_page_pair_t pair;
    uint32_t table = linear >> 22 ? 0x12000u : 0x11000u;
    for (unsigned i = 0; i < 2; i++) {
        uint32_t page = (linear & ~0xFFFu) + i * 4096u;
        pair.pde[i] = 0x10000u + (page >> 22) * 4u;
        if (i && pair.pde[i] != pair.pde[0]) table += 4096u;
        pair.pte[i] = table + ((page >> 12) & 0x3FFu) * 4u;
        dos_mem_write32(vm, pair.pde[i], table | 7u);
        dos_mem_write32(vm, pair.pte[i], (i ? 0x24000u : 0x20000u) | 7u);
    }
    return pair;
}

static void dos_fetch_test_prepare(dos_vm_t *vm, cpu8086_state_t *cpu, unsigned mode)
{
    dos_port_test_prepare(vm, cpu, mode);
    dpmi_desc_set_base(&vm->dpmi.ldt[3], 0);
    dpmi_desc_set_base(&vm->dpmi.ldt[4], 0);
    dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(7), true, mode == 2);
    for (unsigned i = 0; i < 32; i++)
        vm->dpmi.exception_vectors[i].sel = dpmi_index_to_sel(7);
    cpu->cr2 = 0xCAFE1234u;
    cpu->eax = 0xA1B2C3D4u;
    cpu->ebx = 0x12345678u;
    cpu->ecx = 3;
    cpu->edx = 0x3C9;
    cpu->ebp = 0x8000;
    cpu->esi = 0x5000;
    cpu->edi = 0x5100;
    cpu->eflags = FLAGS_FIXED | FLAG_CF | FLAG_ZF | FLAG_AF;
    for (unsigned i = 0; i < 32; i++) vm->mem[0x4800 + i] = 0xA5;
    for (unsigned i = 0; i < 384; i++) vm->mem[0x8F80 + i] = 0x5A;
    dos_io_write8(vm, 0x21, 0x25);
    dos_test_import_cs(vm);
}

static bool dos_test_general_registers(const cpu8086_state_t *a, const cpu8086_state_t *b)
{
    return a->eax == b->eax && a->ebx == b->ebx && a->ecx == b->ecx &&
           a->edx == b->edx && a->ebp == b->ebp && a->esi == b->esi && a->edi == b->edi;
}

static int dos_fetch_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    static const struct {
        uint8_t bytes[12], size;
        bool wide, adr32;
    } programs[] = {
        {{0x90}, 1, false, false},
        {{0xB0, 0x2A}, 2, false, false},
        {{0xB8, 0x78, 0x56}, 3, false, false},
        {{0xB8, 0x78, 0x56, 0x34, 0x12}, 5, true, true},
        {{0x05, 1, 0}, 3, false, false},
        {{0x05, 1, 0, 0, 0}, 5, true, true},
        {{0x68, 0x78, 0x56}, 3, false, false},
        {{0x68, 0x78, 0x56, 0x34, 0x12}, 5, true, true},
        {{0xC8, 0x10, 0, 0}, 4, true, true},
        {{0xE7, 0x21}, 2, true, true},
        {{0xE5, 0x21}, 2, true, true},
        {{0xC7, 0x05, 0, 0x48, 0, 0, 0x78, 0x56, 0x34, 0x12}, 10, true, true},
        {{0x80, 0x05, 0, 0x48, 0, 0, 1}, 7, false, true},
        {{0xC0, 0x25, 0, 0x48, 0, 0, 2}, 7, false, true},
        {{0xC1, 0x25, 0, 0x48, 0, 0, 2}, 7, true, true},
        {{0xF7, 0x05, 0, 0x48, 0, 0, 0x78, 0x56, 0x34, 0x12}, 10, true, true},
        {{0x69, 0x05, 0, 0x48, 0, 0, 3, 0, 0, 0}, 10, true, true},
        {{0x0F, 0xBA, 0x2D, 0, 0x48, 0, 0, 1}, 8, true, true},
        {{0x8F, 0x84, 0x24, 4, 0, 0, 0}, 7, true, true},
        {{0x8B, 0x84, 0x24, 0, 0, 0, 0}, 7, true, true},
        {{0x8D, 0x84, 0x24, 4, 0, 0, 0}, 7, true, true}
    };
    const uint32_t bases[] = { 0x2000, 0x3FF000, 0x403FF000 };
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned instruction = 0; instruction < sizeof(programs) / sizeof(programs[0]); instruction++)
    for (unsigned padded = 0; padded < 2; padded++) {
        uint8_t code[15];
        unsigned size = programs[instruction].size + (programs[instruction].wide != (mode == 2)) +
                        (programs[instruction].adr32 != (mode == 2));
        unsigned pos = 0;
        if (padded) while (pos < 15u - size) code[pos++] = 0x3E;
        if (programs[instruction].wide != (mode == 2)) code[pos++] = 0x66;
        if (programs[instruction].adr32 != (mode == 2)) code[pos++] = 0x67;
        for (unsigned i = 0; i < programs[instruction].size; i++) code[pos++] = programs[instruction].bytes[i];
        size = pos;
        dos_fetch_test_prepare(vm, cpu, mode);
        for (unsigned i = 0; i < size; i++) vm->mem[0x1000 + i] = code[i];
        if (!dos_test_run_one(vm) || cpu->eip != 0x1000u + size || vm->dpmi.exception_depth) {
            serial_puts("[DOS-FETCH] reference failed instruction="); serial_putdec(instruction); serial_puts("\n");
            failures++;
        }
        checks++;
        cpu8086_state_t reference = *cpu;
        uint8_t expected_data[32], expected_stack[384];
        for (unsigned i = 0; i < sizeof(expected_data); i++) expected_data[i] = vm->mem[0x4800 + i];
        for (unsigned i = 0; i < sizeof(expected_stack); i++) expected_stack[i] = vm->mem[0x8F80 + i];
        uint8_t expected_port = dos_io_read8(vm, 0x21);
        for (unsigned region = 0; region < 3; region++)
        for (unsigned cut = 0; cut <= size; cut++)
        for (unsigned scenario = 0; scenario < 4; scenario++) {
            dos_fetch_test_prepare(vm, cpu, mode);
            dos_test_page_pair_t pair = dos_test_page_pair(vm, bases[region] + 4095u);
            dpmi_desc_set_base(&vm->dpmi.ldt[1], bases[region]);
            uint32_t start = 4096u - cut;
            cpu->eip = start;
            for (unsigned i = 0; i < size; i++) {
                unsigned offset = start + i;
                vm->mem[offset < 4096u ? 0x20000u + offset : 0x24000u + offset - 4096u] = code[i];
            }
            if (scenario) dos_mem_write32(vm, pair.pte[1], 0x24007u &
                ~(scenario == 1 ? 1u : scenario == 2 ? 4u : 2u));
            cpu8086_state_t original = *cpu;
            bool faulted = (scenario == 1 || scenario == 2) && cut < size;
            bool ok = dos_test_run_one(vm);
            if (faulted) {
                faults++;
                unsigned width = mode == 2 ? 4u : 2u;
                uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
                uint32_t fields[8];
                for (unsigned i = 0; i < 8; i++) fields[i] = width == 4
                    ? dos_mem_read32(vm, frame + i * width) : dos_mem_read16(vm, frame + i * width);
                if (!dos_test_general_registers(cpu, &original) || cpu->cs != dpmi_index_to_sel(7) ||
                    cpu->eip != 0x60E0 || cpu->cr2 != bases[region] + 4096u ||
                    fields[2] != (scenario == 1 ? 4u : 5u) || fields[3] != start ||
                    fields[4] != original.cs || fields[6] != original.esp || fields[7] != original.ss ||
                    (fields[5] & 0x8D5u) != (original.eflags & 0x8D5u) ||
                    vm->dpmi.exception_depth != 1 || dos_io_read8(vm, 0x21) != 0x25) ok = false;
            } else if (!dos_test_general_registers(cpu, &reference) || cpu->esp != reference.esp ||
                       cpu->eip != start + size || cpu->eflags != reference.eflags ||
                       cpu->cr2 != original.cr2 || vm->dpmi.exception_depth ||
                       dos_io_read8(vm, 0x21) != expected_port) ok = false;
            for (unsigned i = 0; i < sizeof(expected_data); i++)
                if (vm->mem[0x4800 + i] != (faulted ? 0xA5 : expected_data[i])) ok = false;
            for (unsigned i = 0; i < sizeof(expected_stack); i++)
                if (vm->mem[0x8F80 + i] != (faulted ? 0x5A : expected_stack[i])) ok = false;
            if (!ok) {
                if (failures < 8) {
                    serial_puts("[DOS-FETCH] mismatch instruction="); serial_putdec(instruction);
                    serial_puts(" cut="); serial_putdec(cut);
                    serial_puts(" scenario="); serial_putdec(scenario);
                    serial_puts(" mode="); serial_putdec(mode);
                    serial_puts(" region="); serial_putdec(region);
                    serial_puts(" padded="); serial_putdec(padded);
                    serial_puts(" cs:ip="); serial_puthex(cpu->cs, 4);
                    serial_puts(":"); serial_puthex(cpu->eip, 8);
                    serial_puts(" cr2="); serial_puthex(cpu->cr2, 8);
                    serial_puts(" depth="); serial_putdec(vm->dpmi.exception_depth);
                    serial_puts(" gpr="); serial_putdec(dos_test_general_registers(cpu, &original));
                    serial_puts(" port="); serial_puthex(dos_io_read8(vm, 0x21), 2);
                    unsigned width = mode == 2 ? 4u : 2u;
                    uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
                    serial_puts(" frame="); serial_puthex(frame, 8);
                    for (unsigned i = 0; i < 8; i++) {
                        serial_puts(" ");
                        serial_puthex(width == 4 ? dos_mem_read32(vm, frame + i * width)
                                                  : dos_mem_read16(vm, frame + i * width), 8);
                    }
                    serial_puts("\n");
                }
                failures++;
            }
            checks++;
        }
    }
    /* Exact length and CS limit are independent of the operand-size prefix. */
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned length = 14; length <= 17; length++)
    for (unsigned at_limit = 0; at_limit < 2; at_limit++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        if (!mode) {
            cpu->cs = 0;
            dos_mem_write32(vm, 13u * 4u, 0x000060D0);
        }
        unsigned start = at_limit ? 0x10000u - length + 1u : 0x1000;
        cpu->eip = start;
        for (unsigned i = 0; i < length; i++) vm->mem[start + i] = i + 1u == length ? 0x90 : 0x3E;
        bool failed = length > 15 || at_limit;
        bool ok = dos_test_run_one(vm) && cpu->cr2 == 0xCAFE1234u;
        if (failed) {
            faults++;
            unsigned width = mode == 2 ? 4u : 2u;
            uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
            uint32_t saved = frame + (mode ? 3u * width : 0);
            if (cpu->eip != 0x60D0 ||
                (width == 4 ? dos_mem_read32(vm, saved) : dos_mem_read16(vm, saved)) != start) ok = false;
        } else if (cpu->eip != start + length) ok = false;
        if (!ok) failures++;
        checks++;
    }
    serial_puts("[DOS-FETCH] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_fetch_vga_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    static const struct {
        uint8_t bytes[10], size, immediate;
        bool wide;
    } programs[] = {
        {{0x80, 0x05, 0x20, 0, 0x0A, 0, 1}, 7, 1, false},
        {{0x81, 0x05, 0x20, 0, 0x0A, 0, 1, 0, 0, 0}, 10, 4, true},
        {{0x83, 0x05, 0x20, 0, 0x0A, 0, 1}, 7, 1, true},
        {{0xC0, 0x25, 0x20, 0, 0x0A, 0, 1}, 7, 1, false},
        {{0xC1, 0x25, 0x20, 0, 0x0A, 0, 1}, 7, 1, true},
        {{0xF6, 0x05, 0x20, 0, 0x0A, 0, 1}, 7, 1, false},
        {{0xF7, 0x05, 0x20, 0, 0x0A, 0, 1, 0, 0, 0}, 10, 4, true},
        {{0x69, 0x05, 0x20, 0, 0x0A, 0, 1, 0, 0, 0}, 10, 4, true},
        {{0x6B, 0x05, 0x20, 0, 0x0A, 0, 1}, 7, 1, true},
        {{0x0F, 0xBA, 0x2D, 0x20, 0, 0x0A, 0, 1}, 8, 1, true},
        {{0x0F, 0xA4, 0x05, 0x20, 0, 0x0A, 0, 1}, 8, 1, true},
        {{0x0F, 0xAC, 0x05, 0x20, 0, 0x0A, 0, 1}, 8, 1, true}
    };
    unsigned checks = 0;
    int failures = 0;
    uint8_t old_mode = vm->vga_mode;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned instruction = 0; instruction < sizeof(programs) / sizeof(programs[0]); instruction++)
    for (unsigned missing = 0; missing < 2; missing++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        vm->vga_mode = 0x13;
        dos_io_vga_set_mode(vm, 0x13, true);
        dos_io_write8(vm, 0x3C4, 4);
        dos_io_write8(vm, 0x3C5, 6); /* Planar addressing, all four planes. */
        dpmi_desc_set_limit(&vm->dpmi.ldt[3], 0xFFFFF);
        dos_io_vga_write_memory(vm, 0xA0010, 0x3C);
        for (unsigned i = 0; i < 4; i++) dos_io_vga_write_memory(vm, 0xA0020 + i, 0xA5);
        (void)dos_io_vga_read_memory(vm, 0xA0010);

        uint8_t code[12];
        unsigned size = 0;
        if (programs[instruction].wide != (mode == 2)) code[size++] = 0x66;
        if (mode == 1) code[size++] = 0x67;
        for (unsigned i = 0; i < programs[instruction].size; i++) code[size++] = programs[instruction].bytes[i];
        unsigned start = 4096u - (size - programs[instruction].immediate);
        dos_test_page_pair_t pair = dos_test_page_pair(vm, 0x2FFF);
        dpmi_desc_set_base(&vm->dpmi.ldt[1], 0x2000);
        cpu->eip = start;
        for (unsigned i = 0; i < size; i++) {
            unsigned offset = start + i;
            vm->mem[offset < 4096u ? 0x20000u + offset : 0x24000u + offset - 4096u] = code[i];
        }
        if (missing) dos_mem_write32(vm, pair.pte[1], 0x24006);
        cpu8086_state_t original = *cpu;
        bool ok = dos_test_run_one(vm);
        if (missing) {
            if (cpu->eip != 0x60E0 || cpu->cr2 != 0x3000 || vm->dpmi.exception_depth != 1 ||
                !dos_test_general_registers(cpu, &original)) ok = false;
        } else if (cpu->eip != start + size || cpu->cr2 != original.cr2 || vm->dpmi.exception_depth) ok = false;

        /* Write mode 1 exposes the current read latches through a separate
         * cell. A premature operand read is observable even without a write. */
        dos_io_write8(vm, 0x3CE, 5);
        dos_io_write8(vm, 0x3CF, 0x41);
        dos_io_vga_write_memory(vm, 0xA0030, 0);
        for (unsigned plane = 0; plane < 4; plane++) {
            dos_io_write8(vm, 0x3CE, 4);
            dos_io_write8(vm, 0x3CF, plane);
            if (dos_io_vga_read_memory(vm, 0xA0030) != (missing ? 0x3C : 0xA5)) ok = false;
        }
        if (!ok) {
            serial_puts("[DOS-FETCH-VGA] mismatch case="); serial_putdec(checks); serial_puts("\n");
            failures++;
        }
        checks++;
    }
    dos_io_vga_set_mode(vm, 0x13, true);
    vm->vga_mode = old_mode;
    serial_puts("[DOS-FETCH-VGA] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_descriptor_paging_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint32_t linears[] = { 0x6FFC, 0x3FFFFC, 0x403FFFFC, 0xFFFFFFFC };
    unsigned checks = 0;
    int failures = 0;
    for (unsigned table = 0; table < 2; table++)
    for (unsigned region = 0; region < 4; region++)
    for (unsigned wp = 0; wp < 2; wp++)
    for (unsigned accessed = 0; accessed < 2; accessed++)
    for (unsigned scenario = 0; scenario < 7; scenario++) {
        dos_fetch_test_prepare(vm, cpu, 2);
        uint32_t linear = linears[region];
        dos_test_page_pair_t pair = dos_test_page_pair(vm, linear);
        if (!wp) cpu->cr0 &= ~DOS_CR0_WP;
        cpu->gdtr.base = table ? 0x8000 : linear - 16u;
        cpu->gdtr.limit = 0x27;
        uint16_t selector = table ? 0x17 : 0x13;
        if (table) {
            if (region == 3) dos_mem_write32(vm, 0x13020, 0x8003);
            dpmi_descriptor_t ldt = { .limit_lo = 0x17, .access = 0x82 };
            dpmi_desc_set_base(&ldt, linear - 16u);
            cpu8086_cache_ldtr(cpu, 0x18, &ldt);
            for (unsigned i = 0; i < 8; i++) vm->mem[0x8018 + i] = ((uint8_t *)&ldt)[i];
        }
        dpmi_descriptor_t descriptor = { .limit_lo = 0xABCD, .access = 0xF2u | accessed };
        dpmi_desc_set_base(&descriptor, 0x12345678);
        for (unsigned i = 0; i < 8; i++) vm->mem[i < 4 ? 0x20FFCu + i : 0x24000u + i - 4u] = ((uint8_t *)&descriptor)[i];
        for (unsigned i = 0; i < 2; i++) {
            dos_mem_write32(vm, pair.pde[i], dos_mem_read32(vm, pair.pde[i]) & ~4u);
            dos_mem_write32(vm, pair.pte[i], dos_mem_read32(vm, pair.pte[i]) & ~4u);
        }
        if (scenario == 1) dos_mem_write32(vm, pair.pte[1], dos_mem_read32(vm, pair.pte[1]) & ~2u);
        if (scenario == 2) dos_mem_write32(vm, pair.pde[1], dos_mem_read32(vm, pair.pde[1]) & ~2u);
        if (scenario >= 3) {
            uint32_t entry = scenario < 5 ? pair.pte[scenario - 3] : pair.pde[scenario - 5];
            dos_mem_write32(vm, entry, dos_mem_read32(vm, entry) & ~1u);
        }
        dpmi_descriptor_ref_t reference = { .linear = 0xAABBCCDD, .selector = 0xAA55 };
        dos_page_fault_t fault;
        bool present = scenario < 3;
        bool ok = dpmi_lookup_descriptor(vm, selector, &reference, &fault) == present &&
                  cpu->cr2 == 0xCAFE1234u;
        if (present) {
            if (fault.raised || reference.internal || reference.linear != linear || reference.selector != selector)
                ok = false;
            for (unsigned i = 0; i < 8; i++)
                if (((uint8_t *)&reference.descriptor)[i] != ((uint8_t *)&descriptor)[i]) ok = false;
            bool writable = accessed || !wp || !scenario;
            if (dpmi_descriptor_set_accessed(vm, &reference, &fault) != writable) ok = false;
            if (writable ? fault.raised : !fault.raised || fault.linear != linear + 5u || fault.error != 3u)
                ok = false;
            if (vm->mem[0x24001] != (writable ? descriptor.access | 1u : descriptor.access) ||
                (dos_mem_read32(vm, pair.pte[1]) & 0x60u) !=
                    (writable && !accessed ? 0x60u : 0x20u)) ok = false;
        } else {
            /* LDTR is already cached. Only the target entry is read, with
             * supervisor permissions even when the caller is at CPL3. */
            if (!fault.raised || fault.error != 0 || reference.linear != 0xAABBCCDD ||
                reference.selector != 0xAA55 || (dos_mem_read32(vm, pair.pte[0]) & 0x20u)) ok = false;
        }
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-DESC-PAGING] lookup mismatch case="); serial_putdec(checks);
                serial_puts(" fault="); serial_puthex(fault.linear, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned instruction = 0; instruction < 5; instruction++)
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        const uint32_t linear = 0x403FFFFCu;
        dos_test_page_pair_t pair = dos_test_page_pair(vm, linear);
        cpu->gdtr.base = linear - 16u;
        cpu->gdtr.limit = 0x17;
        dpmi_descriptor_t descriptor = { .limit_lo = 0xABCD, .access = scenario == 3 ? 0xF3 : 0xF2 };
        for (unsigned i = 0; i < 8; i++) vm->mem[i < 4 ? 0x20FFCu + i : 0x24000u + i - 4u] = ((uint8_t *)&descriptor)[i];
        dos_mem_write32(vm, pair.pte[0], 0x20003); /* supervisor descriptor pages */
        dos_mem_write32(vm, pair.pte[1], scenario == 1 ? 0x24002 : scenario >= 2 ? 0x24001 : 0x24003);
        dos_mem_write32(vm, 0x4800, 0x12345678);
        dos_mem_write16(vm, 0x4804, 0x13);
        cpu->eax = 0x13;
        cpu->edx = 0xA5A55A5Au;
        uint16_t old_ds = cpu->ds;
        unsigned pos = 0x1000;
        if (mode == 1) { vm->mem[pos++] = 0x66; vm->mem[pos++] = 0x67; }
        if (instruction < 4) {
            vm->mem[pos++] = 0x0F;
            vm->mem[pos++] = instruction < 2 ? instruction + 2u : 0;
            vm->mem[pos++] = instruction < 2 ? 0xD0 : instruction == 2 ? 0xE0 : 0xE8;
        } else {
            vm->mem[pos++] = 0xC5;
            vm->mem[pos++] = 0x05;
            dos_mem_write32(vm, pos, 0x4800);
            pos += 4;
        }
        bool faulted = scenario == 1 || (instruction == 4 && scenario == 2);
        bool ok = dos_test_run_one(vm);
        if (faulted) {
            unsigned width = mode == 2 ? 4u : 2u;
            uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
            if (cpu->eip != 0x60E0 || cpu->cr2 != (scenario == 1 ? linear + 4u : linear + 5u) ||
                cpu->eax != 0x13 || cpu->edx != 0xA5A55A5Au || cpu->ds != old_ds ||
                (width == 4 ? dos_mem_read32(vm, frame + 8u) : dos_mem_read16(vm, frame + 4u)) !=
                    (scenario == 1 ? 0u : 3u) ||
                (width == 4 ? dos_mem_read32(vm, frame + 12u) : dos_mem_read16(vm, frame + 6u)) != 0x1000u)
                ok = false;
        } else {
            if (cpu->eip != pos || cpu->cr2 != 0xCAFE1234u) ok = false;
            if (instruction < 4) {
                uint32_t value = instruction == 0 ? (uint32_t)descriptor.access << 8
                                   : instruction == 1 ? 0xABCDu : 0xA5A55A5Au;
                if (!(cpu->eflags & FLAG_ZF) || cpu->edx != value || cpu->eax != 0x13) ok = false;
            } else if (cpu->eax != 0x12345678 || cpu->ds != 0x13 || vm->mem[0x24001] != 0xF3) ok = false;
        }
        if (!ok) {
            serial_puts("[DOS-DESC-PAGING] CPU mismatch case="); serial_putdec(checks); serial_puts("\n");
            failures++;
        }
        checks++;
    }
    /* Loaded CS survives descriptor-page removal. Only the user code page
     * is accessed at fetch; descriptor paging is checked by loads/queries. */
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        const uint32_t linear = 0x403FFFFCu;
        dos_test_page_pair_t pair = dos_test_page_pair(vm, linear);
        cpu->gdtr.base = linear - 8u;
        cpu->gdtr.limit = 0x0F;
        cpu->cs = 0x0B;
        dpmi_descriptor_t descriptor = {
            .limit_lo = 0xFFFF, .access = 0xFA,
            .flags_lim = mode == 2 ? DESC_32BIT : 0
        };
        cpu8086_cache_cs(cpu, cpu->cs, &descriptor, 3);
        for (unsigned i = 0; i < 8; i++) vm->mem[i < 4 ? 0x20FFCu + i : 0x24000u + i - 4u] = ((uint8_t *)&descriptor)[i];
        dos_mem_write32(vm, pair.pte[0], scenario == 1 ? 0x20000 : 0x20001);
        dos_mem_write32(vm, pair.pte[1], scenario == 2 ? 0x24000 : 0x24001);
        dos_mem_write32(vm, 0x11004, scenario == 3 ? 0x1003 : scenario == 4 ? 0x1005 : 0x1007);
        vm->mem[0x1000] = 0x90;
        cpu8086_state_t original = *cpu;
        bool faulted = scenario == 3;
        bool ok = cpu8086_run_one(vm) && dos_test_general_registers(cpu, &original);
        if (faulted) {
            unsigned width = mode == 2 ? 4u : 2u;
            uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
            uint32_t expected = 0x1000;
            if (cpu->eip != 0x60E0 || cpu->cs != dpmi_index_to_sel(7) ||
                cpu->cr2 != expected || vm->dpmi.exception_depth != 1 ||
                (width == 4 ? dos_mem_read32(vm, frame + 8u) : dos_mem_read16(vm, frame + 4u)) !=
                    (scenario == 3 ? 5u : 0u) ||
                (width == 4 ? dos_mem_read32(vm, frame + 12u) : dos_mem_read16(vm, frame + 6u)) != 0x1000)
                ok = false;
        } else if (cpu->eip != 0x1001 || cpu->cs != 0x0B || cpu->cr2 != original.cr2 ||
                   vm->dpmi.exception_depth || (dos_mem_read32(vm, 0x11004) & 0x60u) != 0x20u) ok = false;
        if ((dos_mem_read32(vm, pair.pte[0]) | dos_mem_read32(vm, pair.pte[1])) & 0x60u)
            ok = false;
        if (!ok) {
            serial_puts("[DOS-DESC-PAGING] fetch mismatch case="); serial_putdec(checks); serial_puts("\n");
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-DESC-PAGING] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_modrm_paging_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    static const struct {
        uint8_t byte_op, wide_op, extended, reg, immediate, widths, format;
        bool write;
    } programs[] = {
        /* Width masks: 1/2/4 bytes. Immediate 255 means operand width.
         * Format 0 is ModRM, 1 is moffs, 2 is XLAT. */
        {0x00,0x01,0,1,0,7,0,true}, {0x02,0x03,0,1,0,7,0,false},
        {0x08,0x09,0,1,0,7,0,true}, {0x0A,0x0B,0,1,0,7,0,false},
        {0x10,0x11,0,1,0,7,0,true}, {0x12,0x13,0,1,0,7,0,false},
        {0x18,0x19,0,1,0,7,0,true}, {0x1A,0x1B,0,1,0,7,0,false},
        {0x20,0x21,0,1,0,7,0,true}, {0x22,0x23,0,1,0,7,0,false},
        {0x28,0x29,0,1,0,7,0,true}, {0x2A,0x2B,0,1,0,7,0,false},
        {0x30,0x31,0,1,0,7,0,true}, {0x32,0x33,0,1,0,7,0,false},
        {0x38,0x39,0,1,0,7,0,false}, {0x3A,0x3B,0,1,0,7,0,false},
        {0x80,0x81,0,0,255,7,0,true}, {0x80,0x81,0,1,255,7,0,true},
        {0x80,0x81,0,2,255,7,0,true}, {0x80,0x81,0,3,255,7,0,true},
        {0x80,0x81,0,4,255,7,0,true}, {0x80,0x81,0,5,255,7,0,true},
        {0x80,0x81,0,6,255,7,0,true}, {0x80,0x81,0,7,255,7,0,false},
        {0x82,0x83,0,0,1,7,0,true}, {0x82,0x83,0,7,1,7,0,false},
        {0x84,0x85,0,1,0,7,0,false}, {0x86,0x87,0,1,0,7,0,true},
        {0x88,0x89,0,1,0,7,0,true}, {0x8A,0x8B,0,1,0,7,0,false},
        {0,0x8F,0,0,0,6,0,true},
        {0xC0,0xC1,0,0,1,7,0,true}, {0xC0,0xC1,0,1,1,7,0,true},
        {0xC0,0xC1,0,2,1,7,0,true}, {0xC0,0xC1,0,3,1,7,0,true},
        {0xC0,0xC1,0,4,1,7,0,true}, {0xC0,0xC1,0,5,1,7,0,true},
        {0xC0,0xC1,0,6,1,7,0,true}, {0xC0,0xC1,0,7,1,7,0,true},
        {0xD0,0xD1,0,4,0,7,0,true}, {0xD2,0xD3,0,4,0,7,0,true},
        {0xF6,0xF7,0,0,255,7,0,false},
        {0xF6,0xF7,0,2,0,7,0,true}, {0xF6,0xF7,0,3,0,7,0,true},
        {0xF6,0xF7,0,4,0,7,0,false}, {0xF6,0xF7,0,5,0,7,0,false},
        {0xF6,0xF7,0,6,0,7,0,false}, {0xF6,0xF7,0,7,0,7,0,false},
        {0xFE,0xFF,0,0,0,7,0,true}, {0xFE,0xFF,0,1,0,7,0,true},
        {0,0x69,0,1,255,6,0,false}, {0,0x6B,0,1,1,6,0,false},
        {0,0xAF,1,1,0,6,0,false},
        {0,0xA3,1,1,0,6,0,false}, {0,0xAB,1,1,0,6,0,true},
        {0,0xB3,1,1,0,6,0,true}, {0,0xBB,1,1,0,6,0,true},
        {0,0xBA,1,4,1,6,0,false}, {0,0xBA,1,5,1,6,0,true},
        {0,0xBA,1,6,1,6,0,true}, {0,0xBA,1,7,1,6,0,true},
        {0,0xA4,1,1,1,6,0,true}, {0,0xA5,1,1,0,6,0,true},
        {0,0xAC,1,1,1,6,0,true}, {0,0xAD,1,1,0,6,0,true},
        {0x94,0,1,0,0,1,0,true}, {0xB6,0,1,1,0,1,0,false},
        {0xBE,0,1,1,0,1,0,false}, {0,0x63,0,1,0,2,0,true},
        {0xC6,0xC7,0,0,255,7,0,true},
        {0xA0,0xA1,0,0,0,7,1,false}, {0xA2,0xA3,0,0,0,7,1,true},
        {0xD7,0,0,0,0,1,2,false}
    };
    const uint32_t bases[] = { 0xA000, 0x403FF000 };
    unsigned checks = 0, faults = 0, retries = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned instruction = 0; instruction < sizeof(programs) / sizeof(programs[0]); instruction++)
    for (unsigned width = 1; width <= 4; width *= 2)
    for (unsigned cut_index = 0; cut_index < (width == 1 ? 1u : 2u); cut_index++) {
        if (!(programs[instruction].widths & width)) continue;
        unsigned cut = cut_index ? width : 1u;
        unsigned offset = 4096u - cut;
        uint8_t code[15];
        unsigned size = 0;
        if ((width == 4) != (mode == 2)) code[size++] = 0x66;
        code[size++] = 0x3E; /* Also verify first-prefix restart IP. */
        if (programs[instruction].extended) code[size++] = 0x0F;
        code[size++] = width == 1 ? programs[instruction].byte_op : programs[instruction].wide_op;
        if (programs[instruction].format != 2) {
            if (!programs[instruction].format)
                code[size++] = (programs[instruction].reg << 3) | (mode == 2 ? 5u : 6u);
            code[size++] = offset;
            code[size++] = offset >> 8;
            if (mode == 2) { code[size++] = 0; code[size++] = 0; }
        }
        unsigned immediate = programs[instruction].immediate;
        if (immediate == 255) immediate = width;
        for (unsigned i = 0; i < immediate; i++) code[size++] = i ? 0 : 1;
        cpu8086_state_t reference = {0};
        uint8_t expected[10] = {0};
        /* First execute on ordinary RAM, then relocate the same operand.
         * Arithmetic values are also covered by the existing CPU suites. */
        for (int region = -1; region < 2; region++)
        for (unsigned scenario = 0; scenario < (region < 0 ? 1u : region ? 7u : 4u); scenario++) {
            dos_fetch_test_prepare(vm, cpu, mode);
            uint32_t base = region < 0 ? 0x20000u : bases[region];
            dpmi_desc_set_base(&vm->dpmi.ldt[3], base);
            cpu->eax = 0x1234; cpu->edx = 0; cpu->ecx = 3;
            cpu->eflags |= FLAG_IF | FLAG_IOPL_MASK;
            vm->dpmi.virtual_interrupts_enabled = true;
            if (programs[instruction].format == 2) cpu->ebx = offset - cpu->al;
            dos_mem_write32(vm, 0x9000, 0x12345678);
            dos_test_page_pair_t pair = {0};
            uint32_t entry = 0, saved_entry = 0;
            if (region >= 0) pair = dos_test_page_pair(vm, base + offset);
            bool write = programs[instruction].write;
            bool failed = scenario && (scenario % 3u != 2u || write);
            if (scenario) {
                unsigned page = cut < width ? 1u : 0u;
                entry = scenario < 4u ? pair.pte[page] : pair.pde[page];
                saved_entry = dos_mem_read32(vm, entry);
                unsigned bit = (scenario - 1u) % 3u;
                dos_mem_write32(vm, entry, saved_entry & ~(1u << bit));
            }
            for (unsigned i = 0; i < 10; i++) {
                unsigned at = offset + i - 1u;
                uint32_t physical = region < 0 || at < 4096u ? 0x20000u + at
                                                                            : 0x24000u + at - 4096u;
                vm->mem[physical] = i == 1 ? 0x31 : i == 2 ? 2 : i >= 3 && i <= 4 ? 0 : 0x5A;
                if (!region) vm->mem[base + at] = 0x6C;
            }
            for (unsigned i = 0; i < size; i++) vm->mem[0x1000 + i] = code[i];
            cpu8086_state_t original = *cpu;
            bool ok = dos_test_run_one(vm);
            if (failed) {
                faults++;
                unsigned slot = mode == 2 ? 4u : 2u;
                uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
                uint32_t fields[8];
                for (unsigned i = 0; i < 8; i++) fields[i] = slot == 4
                    ? dos_mem_read32(vm, frame + slot * i) : dos_mem_read16(vm, frame + slot * i);
                unsigned error = 4u | (write ? 2u : 0) | (scenario % 3u == 1u ? 0 : 1u);
                if (cpu->eip != 0x60E0 || cpu->cr2 != base + (cut < width ? 4096u : offset) ||
                    vm->dpmi.exception_depth != 1 || !dos_test_general_registers(cpu, &original) ||
                    fields[2] != error || fields[3] != 0x1000 || fields[4] != original.cs ||
                    (fields[5] & 0x8D5u) != (original.eflags & 0x8D5u) ||
                    fields[6] != original.esp || fields[7] != original.ss) ok = false;
            } else if (cpu->eip != 0x1000u + size || cpu->cr2 != original.cr2 ||
                       vm->dpmi.exception_depth) ok = false;
            if (region >= 0 && !failed &&
                (!dos_test_general_registers(cpu, &reference) || cpu->esp != reference.esp ||
                 cpu->eflags != reference.eflags)) ok = false;
            for (unsigned i = 0; i < 10; i++) {
                unsigned at = offset + i - 1u;
                uint32_t physical = region < 0 || at < 4096u ? 0x20000u + at
                                                                            : 0x24000u + at - 4096u;
                uint8_t before = i == 1 ? 0x31 : i == 2 ? 2 : i >= 3 && i <= 4 ? 0 : 0x5A;
                if (region < 0) expected[i] = vm->mem[physical];
                else if (vm->mem[physical] != (failed ? before : expected[i])) ok = false;
                if (!region && vm->mem[base + at] != 0x6C) ok = false;
            }
            if (region < 0) reference = *cpu;
            else for (unsigned page = 0; page < 2; page++) {
                bool touched = !failed && (!page || cut < width);
                if ((dos_mem_read32(vm, pair.pte[page]) & 0x60u) !=
                    (touched ? write ? 0x60u : 0x20u : 0)) ok = false;
            }
            /* Repair a missing mapping and return through the actual DPMI
             * handler stub before retrying, not by resetting the vCPU. */
            if (failed && scenario == 1 && region == 1 && !cut_index) {
                dos_mem_write32(vm, entry, saved_entry);
                vm->mem[0x60E0] = 0xCB;
                vm->step_limit = vm->step_count + 16u;
                bool returned = dos_test_run_until(vm, true, original.cs, 0x1000);
                vm->step_limit = 0;
                if (!returned || !dos_test_run_one(vm) || cpu->eip != 0x1000u + size ||
                    vm->dpmi.exception_depth || !dos_test_general_registers(cpu, &reference) ||
                    cpu->esp != reference.esp || cpu->eflags != reference.eflags) ok = false;
                for (unsigned i = 0; i < 10; i++) {
                    unsigned at = offset + i - 1u;
                    uint32_t physical = at < 4096u ? 0x20000u + at : 0x24000u + at - 4096u;
                    if (vm->mem[physical] != expected[i]) ok = false;
                }
                retries++;
            }
            if (!ok) {
                if (failures < 12) {
                    serial_puts("[DOS-MODRM-PAGING] mismatch instruction="); serial_putdec(instruction);
                    serial_puts(" width="); serial_putdec(width);
                    serial_puts(" mode="); serial_putdec(mode);
                    serial_puts(" cut="); serial_putdec(cut);
                    serial_puts(" region="); serial_putdec(region + 1);
                    serial_puts(" scenario="); serial_putdec(scenario);
                    serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                    serial_puts(" flags="); serial_puthex(cpu->eflags, 8);
                    serial_puts(" expected="); serial_puthex(reference.eflags, 8);
                    serial_puts(" cr2="); serial_puthex(cpu->cr2, 8); serial_puts("\n");
                }
                failures++;
            }
            checks++;
        }
    }
    serial_puts("[DOS-MODRM-PAGING] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static bool dos_test_memory_fault(dos_vm_t *vm, const cpu8086_state_t *original,
                                   unsigned vector, unsigned error)
{
    cpu8086_state_t *cpu = vm->cpu;
    unsigned width = vm->dpmi.is_32bit ? 4u : 2u;
    uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
    uint32_t fields[8];
    for (unsigned i = 0; i < 8; i++) fields[i] = width == 4
        ? dos_mem_read32(vm, frame + width * i) : dos_mem_read16(vm, frame + width * i);
    return cpu->running && cpu->eip == 0x6000u + vector * 16u &&
        vm->dpmi.exception_depth == 1 && dos_test_general_registers(cpu, original) &&
        fields[2] == error && fields[3] == original->eip && fields[4] == original->cs &&
        (fields[5] & 0x8D5u) == (original->eflags & 0x8D5u) &&
        fields[6] == (width == 4 ? original->esp : (uint16_t)original->esp) &&
        fields[7] == original->ss;
}

static int dos_modrm_boundary_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned width = 1; width <= 4; width *= 2)
    for (unsigned write = 0; write < 2; write++)
    for (unsigned scenario = 0; scenario < 11; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        bool stack = scenario == 10;
        unsigned offset = stack ? 0x9000u : 0x700u;
        dpmi_descriptor_t *descriptor = &vm->dpmi.ldt[stack ? 2 : 3];
        uint32_t base = stack ? 0 : 0x20000;
        dpmi_desc_set_base(descriptor, base);
        dos_test_page_pair_t pair = dos_test_page_pair(vm, base + offset);
        switch (scenario) {
        case 1: case 10: dpmi_desc_set_limit(descriptor, offset + width - 2u); break;
        case 2: cpu->ds = 0; break;
        case 3: descriptor->access &= ~DESC_PRESENT; break;
        case 4: descriptor->access &= ~DESC_WRITABLE; break;
        case 5: descriptor->access = 0xF8; break;
        case 6: descriptor->access = 0xFA; break;
        case 7: descriptor->access &= ~DESC_DPL_MASK; break;
        case 8: case 9:
            descriptor->access |= 4u;
            dpmi_desc_set_limit(descriptor, offset - (scenario == 8));
            break;
        }
        bool failed = scenario == 1 || scenario == 2 || scenario == 3 ||
                      scenario == 5 || scenario == 7 || scenario == 9 || stack ||
                      (write && (scenario == 4 || scenario == 6));
        /* Segment protection takes precedence over this missing page. */
        if (failed) dos_mem_write32(vm, pair.pte[0], 0x20006);
        uint32_t physical = 0x20000u + ((base + offset) & 0xFFFu);
        dos_mem_write32(vm, physical, 0x12345678);
        cpu->ecx = 0xA1B2C3D4;
        uint32_t p = 0x1000;
        if ((width == 4) != (mode == 2)) vm->mem[p++] = 0x66;
        vm->mem[p++] = stack ? 0x36 : 0x3E;
        vm->mem[p++] = (write ? 0x88 : 0x8A) | (width != 1);
        vm->mem[p++] = 8u | (mode == 2 ? 5u : 6u);
        vm->mem[p++] = offset; vm->mem[p++] = offset >> 8;
        if (mode == 2) { vm->mem[p++] = 0; vm->mem[p++] = 0; }
        cpu8086_state_t original = *cpu;
        bool ok = dos_test_run_one(vm) && cpu->cr2 == original.cr2;
        if (failed) {
            if (!dos_test_memory_fault(vm, &original, stack ? 12u : 13u, 0) ||
                dos_mem_read32(vm, physical) != 0x12345678u) ok = false;
        } else {
            uint32_t mask = width == 1 ? 0xFFu : width == 2 ? 0xFFFFu : UINT32_MAX;
            if (cpu->eip != p || vm->dpmi.exception_depth || cpu->eflags != original.eflags ||
                cpu->ecx != (write ? original.ecx : (original.ecx & ~mask) | (0x12345678u & mask)) ||
                dos_mem_read32(vm, physical) != (write ? (0x12345678u & ~mask) | (original.ecx & mask)
                                                       : 0x12345678u)) ok = false;
        }
        if (!ok) { failures++; serial_puts("[DOS-MODRM-BOUNDARY] segment case="); serial_putdec(checks); serial_puts("\n"); }
        checks++;
    }
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned width = 2; width <= 4; width *= 2)
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
        dpmi_desc_set_limit(&vm->dpmi.ldt[2], stack32 ? 0x1FFFF : 0xFFFF);
        cpu->esp = stack32 ? 0x15000 : 0xA5A59000;
        unsigned sp = stack32 ? cpu->esp : cpu->sp;
        dpmi_desc_set_base(&vm->dpmi.ldt[3], 0x403FF000);
        dos_test_page_pair_t pair = dos_test_page_pair(vm, 0x403FFFFF);
        if (scenario == 1) dos_mem_write32(vm, 0x11000u + (sp >> 12) * 4u, 0);
        if (scenario >= 2) dos_mem_write32(vm, pair.pte[1], 0);
        if (scenario == 3) dpmi_desc_set_limit(&vm->dpmi.ldt[2], sp + width - 2u);
        if (scenario == 4) dpmi_desc_set_limit(&vm->dpmi.ldt[3], 0xFFE);
        dos_mem_write32(vm, sp, 0x12345678);
        vm->mem[0x20FFF] = 0xA5;
        for (unsigned i = 0; i < 4; i++) vm->mem[0x24000 + i] = 0xA5;
        uint32_t p = 0x1000;
        if ((width == 4) != (mode == 2)) vm->mem[p++] = 0x66;
        vm->mem[p++] = 0x3E; vm->mem[p++] = 0x8F;
        vm->mem[p++] = mode == 2 ? 5 : 6;
        vm->mem[p++] = 0xFF; vm->mem[p++] = 0x0F;
        if (mode == 2) { vm->mem[p++] = 0; vm->mem[p++] = 0; }
        cpu8086_state_t original = *cpu;
        bool ok = dos_test_run_one(vm);
        if (scenario) {
            unsigned vector = scenario == 3 ? 12 : scenario == 4 ? 13 : 14;
            unsigned error = scenario == 1 ? 4 : scenario == 2 ? 6 : 0;
            uint32_t cr2 = scenario == 1 ? sp : scenario == 2 ? 0x40400000 : original.cr2;
            if (!dos_test_memory_fault(vm, &original, vector, error) || cpu->cr2 != cr2 ||
                vm->mem[0x20FFF] != 0xA5 || vm->mem[0x24000] != 0xA5) ok = false;
        } else if (cpu->eip != p || vm->dpmi.exception_depth ||
                   cpu->esp != original.esp + width || vm->mem[0x20FFF] != 0x78 ||
                   vm->mem[0x24000] != 0x56) ok = false;
        if (dos_mem_read32(vm, sp) != 0x12345678u) ok = false;
        if (!ok) { failures++; serial_puts("[DOS-MODRM-BOUNDARY] pop case="); serial_putdec(checks); serial_puts("\n"); }
        checks++;
    }
    serial_puts("[DOS-MODRM-BOUNDARY] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_memory_record_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned width = 2; width <= 4; width *= 2)
    for (unsigned kind = 0; kind < 8; kind++) {
        /* SGDT/SIDT/LGDT/LIDT, BOUND inside/outside, far JMP/CALL. */
        unsigned length = kind < 4 ? 6u : kind < 6 ? width * 2u : width + 2u;
        for (unsigned cut = 1; cut <= length; cut++)
        for (unsigned scenario = 0; scenario < 3; scenario++) {
            dos_fetch_test_prepare(vm, cpu, mode);
            bool store = kind < 2;
            bool supervisor = kind == 2 || kind == 3;
            if (supervisor) {
                cpu->cs &= ~3u; cpu->ds &= ~3u; cpu->ss &= ~3u;
                for (unsigned i = 1; i <= 3; i++) vm->dpmi.ldt[i].access &= ~DESC_DPL_MASK;
            }
            cpu->gdtr.limit = 0x1234; cpu->gdtr.base = 0x12345678;
            cpu->idtr.limit = 0x5678; cpu->idtr.base = 0x87654321;
            dpmi_desc_set_base(&vm->dpmi.ldt[3], 0x403FF000);
            unsigned offset = 4096u - cut;
            dos_test_page_pair_t pair = dos_test_page_pair(vm, 0x403FF000u + offset);
            if (scenario) {
                uint32_t entry = pair.pte[cut < length ? 1 : 0];
                dos_mem_write32(vm, entry, dos_mem_read32(vm, entry) & ~(scenario == 1 ? 1u : 2u));
            }
            uint8_t bytes[8] = { 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0, 0 };
            if (kind == 4 || kind == 5) {
                cpu->eax = kind == 4 ? 8 : 20;
                for (unsigned i = 0; i < width; i++) {
                    bytes[i] = (uint8_t)(0xFFFFFFFBu >> (i * 8u));
                    bytes[width + i] = i ? 0 : 10;
                }
            } else if (kind >= 6) {
                for (unsigned i = 0; i < width; i++) bytes[i] = (uint8_t)(0x1800u >> (i * 8u));
                bytes[width] = cpu->cs; bytes[width + 1u] = cpu->cs >> 8;
            }
            for (unsigned i = 0; i < length + 2u; i++) {
                unsigned at = offset + i - 1u;
                vm->mem[at < 4096u ? 0x20000u + at : 0x24000u + at - 4096u] =
                    i && i <= length ? bytes[i - 1u] : 0xA5;
            }
            uint32_t p = 0x1000;
            if ((width == 4) != (mode == 2)) vm->mem[p++] = 0x66;
            if (kind < 4) vm->mem[p++] = 0x0F;
            vm->mem[p++] = kind < 4 ? 0x01 : kind < 6 ? 0x62 : 0xFF;
            vm->mem[p++] = ((kind < 4 ? kind : kind < 6 ? 0u : kind == 6 ? 5u : 3u) << 3) |
                           (mode == 2 ? 5u : 6u);
            vm->mem[p++] = offset; vm->mem[p++] = offset >> 8;
            if (mode == 2) { vm->mem[p++] = 0; vm->mem[p++] = 0; }
            cpu8086_state_t original = *cpu;
            bool page_fault = scenario == 1 || (store && scenario == 2);
            bool bound_fault = !page_fault && kind == 5;
            bool ok = dos_test_run_one(vm);
            if (page_fault || bound_fault) {
                unsigned error = bound_fault ? 0u : (supervisor ? 0 : 4u) |
                                 (store ? 2u : 0) | (scenario == 2 ? 1u : 0);
                if (!dos_test_memory_fault(vm, &original, page_fault ? 14u : 5u, error) ||
                    cpu->cr2 != (page_fault ? 0x403FF000u + (cut < length ? 4096u : offset) : original.cr2))
                    ok = false;
            } else if (cpu->eip != (kind >= 6 ? 0x1800u : p) || vm->dpmi.exception_depth ||
                       !dos_test_general_registers(cpu, &original) || cpu->eflags != original.eflags ||
                       cpu->esp != (kind == 7 ? original.esp - width * 2u : original.esp)) ok = false;
            uint32_t loaded_base = width == 4 ? 0x12345678u : 0x345678u;
            if (cpu->gdtr.base != (kind == 2 && !page_fault ? loaded_base : original.gdtr.base) ||
                cpu->gdtr.limit != original.gdtr.limit ||
                cpu->idtr.base != (kind == 3 && !page_fault ? loaded_base : original.idtr.base) ||
                cpu->idtr.limit != (kind == 3 && !page_fault ? 0x1234u : original.idtr.limit)) ok = false;
            for (unsigned i = 0; i < length + 2u; i++) {
                unsigned at = offset + i - 1u;
                uint8_t expected = i && i <= length ? bytes[i - 1u] : 0xA5;
                if (store && !page_fault && i && i <= length) {
                    unsigned part = i - 1u;
                    uint16_t limit = kind ? original.idtr.limit : original.gdtr.limit;
                    uint32_t base = kind ? original.idtr.base : original.gdtr.base;
                    expected = part < 2 ? (uint8_t)(limit >> (part * 8u)) :
                                         (uint8_t)(base >> ((part - 2u) * 8u));
                }
                if (vm->mem[at < 4096u ? 0x20000u + at : 0x24000u + at - 4096u] != expected) ok = false;
            }
            if (!ok) {
                if (failures < 12) {
                    serial_puts("[DOS-MEMORY-RECORD] case="); serial_putdec(checks);
                    serial_puts(" kind="); serial_putdec(kind);
                    serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n");
                }
                failures++;
            }
            checks++;
        }
    }
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned kind = 0; kind < 4; kind++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        vm->mem[0x1000] = 0x0F; vm->mem[0x1001] = 0x01;
        vm->mem[0x1002] = 0xC0u | (kind << 3);
        cpu8086_state_t original = *cpu;
        if (!dos_test_run_one(vm) || !dos_test_memory_fault(vm, &original, 6, 0)) failures++;
        checks++;
    }
    serial_puts("[DOS-MEMORY-RECORD] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_modrm_vga_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    static const struct { uint8_t opcode, reg, extended, immediate; } programs[] = {
        {0x01,1,0,0}, {0x87,1,0,0}, {0x81,0,0,4}, {0xC1,4,0,1},
        {0xD1,4,0,0}, {0xD3,4,0,0}, {0xF7,2,0,0}, {0xF7,3,0,0},
        {0xFF,0,0,0}, {0xBA,5,1,1}, {0xA4,1,1,1}, {0xAC,1,1,1}
    };
    unsigned checks = 0;
    int failures = 0;
    uint8_t old_mode = vm->vga_mode;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned instruction = 0; instruction < sizeof(programs) / sizeof(programs[0]); instruction++)
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        vm->vga_mode = 0x13;
        dos_io_vga_set_mode(vm, 0x13, true);
        dos_io_write8(vm, 0x3C4, 4); dos_io_write8(vm, 0x3C5, 6);
        dpmi_desc_set_limit(&vm->dpmi.ldt[3], 0xFFFFF);
        dos_io_vga_write_memory(vm, 0xA0010, 0x3C);
        for (unsigned i = 0; i < 4; i++) dos_io_vga_write_memory(vm, 0xA0020 + i, 0xA5);
        (void)dos_io_vga_read_memory(vm, 0xA0010);
        dos_test_page_pair_t pair = dos_test_page_pair(vm, 0xA0020);
        dos_mem_write32(vm, pair.pte[0], 0xA0007u & ~(scenario ? 1u << (scenario - 1u) : 0));
        uint32_t p = 0x1000;
        if (mode == 1) { vm->mem[p++] = 0x66; vm->mem[p++] = 0x67; }
        if (programs[instruction].extended) vm->mem[p++] = 0x0F;
        vm->mem[p++] = programs[instruction].opcode;
        vm->mem[p++] = (programs[instruction].reg << 3) | 5u;
        vm->mem[p++] = 0x20; vm->mem[p++] = 0; vm->mem[p++] = 0x0A; vm->mem[p++] = 0;
        for (unsigned i = 0; i < programs[instruction].immediate; i++) vm->mem[p++] = i ? 0 : 1;
        cpu8086_state_t original = *cpu;
        bool ok = dos_test_run_one(vm);
        if (scenario) {
            if (!dos_test_memory_fault(vm, &original, 14, scenario == 1 ? 6u : 7u) ||
                cpu->cr2 != 0xA0020) ok = false;
        } else if (cpu->eip != p || vm->dpmi.exception_depth) ok = false;
        dos_io_write8(vm, 0x3CE, 5); dos_io_write8(vm, 0x3CF, 0x41);
        dos_io_vga_write_memory(vm, 0xA0030, 0);
        for (unsigned plane = 0; plane < 4; plane++) {
            dos_io_write8(vm, 0x3CE, 4); dos_io_write8(vm, 0x3CF, plane);
            if (dos_io_vga_read_memory(vm, 0xA0030) != (scenario ? 0x3C : 0xA5)) ok = false;
        }
        if (!ok) { failures++; serial_puts("[DOS-MODRM-VGA] case="); serial_putdec(checks); serial_puts("\n"); }
        checks++;
    }
    dos_io_vga_set_mode(vm, 0x13, true);
    vm->vga_mode = old_mode;
    serial_puts("[DOS-MODRM-VGA] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static uint32_t dos_string_compare_flags(unsigned width, uint32_t before,
                                          uint32_t left, uint32_t right)
{
    uint64_t flags;
    if (width == 1) __asm__ volatile ("cmpb %b2, %b1; pushfq; popq %0"
        : "=r"(flags) : "r"(left), "r"(right) : "cc", "memory");
    else if (width == 2) __asm__ volatile ("cmpw %w2, %w1; pushfq; popq %0"
        : "=r"(flags) : "r"(left), "r"(right) : "cc", "memory");
    else __asm__ volatile ("cmpl %k2, %k1; pushfq; popq %0"
        : "=r"(flags) : "r"(left), "r"(right) : "cc", "memory");
    return (before & ~0x8D5u) | ((uint32_t)flags & 0x8D5u);
}

static uint32_t dos_string_test_code(dos_vm_t *vm, uint8_t opcode,
                                      unsigned width, bool adr32,
                                      unsigned repeat, uint8_t segment)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t p = cpu->eip;
    if ((width == 4) != cpu->op_size_32) vm->mem[p++] = 0x66;
    if (adr32 != cpu->addr_size_32) vm->mem[p++] = 0x67;
    if (segment) vm->mem[p++] = segment;
    if (repeat) vm->mem[p++] = repeat == 1 ? 0xF3 : 0xF2;
    vm->mem[p++] = opcode | (width != 1);
    return p;
}

static int dos_string_memory_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint8_t opcodes[] = { 0xA4, 0xA6, 0xAA, 0xAC, 0xAE };
    const uint8_t prefixes[] = { 0, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65 };
    const uint32_t bases[] = { 0x10000, 0x20000, 0, 0, 0x10000, 0x30000, 0x40000 };
    const unsigned counts[] = { 0, 1, 3, 513 };
    unsigned checks = 0;
    int failures = 0;
    for (unsigned phase = 0; phase < 2; phase++)
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned op = 0; op < sizeof(opcodes); op++)
    for (unsigned width = 1; width <= 4; width *= 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned backwards = 0; backwards < 2; backwards++)
    for (unsigned rep = 0; rep < (phase ? 1u : 3u); rep++)
    for (unsigned sample = 0; sample < (phase ? 1u : 4u); sample++)
    for (unsigned segment = 0; segment < (phase ? sizeof(prefixes) : 1u); segment++) {
        dos_port_test_prepare(vm, cpu, mode);
        uint8_t opcode = opcodes[op];
        bool source = opcode == 0xA4 || opcode == 0xA6 || opcode == 0xAC;
        bool destination = opcode != 0xAC;
        bool compare = opcode == 0xA6 || opcode == 0xAE;
        unsigned repeat = phase ? 1u : rep;
        unsigned requested = phase ? 3u : counts[sample];
        unsigned count = repeat ? requested : 1u;
        unsigned stop = repeat == 1 ? count / 2u : count ? count - 1u : 0;
        unsigned target = compare && repeat && count ? stop + 1u : count;
        int32_t delta = backwards ? -(int32_t)width : (int32_t)width;
        cpu->eflags = FLAGS_FIXED | FLAG_CF | FLAG_AF | FLAG_ZF | FLAG_OF |
                      (backwards ? FLAG_DF : 0);
        cpu->eax = 0xA1B2C3D4;
        cpu->ecx = (adr32 ? 0 : 0x12340000u) | requested;
        cpu->esi = (adr32 ? 0 : 0x56780000u) | 0x4000u;
        cpu->edi = (adr32 ? 0 : 0x9ABC0000u) | 0x8000u;
        for (unsigned i = 0; i <= count; i++) {
            uint32_t value = 0x89AB7FF0u + i;
            uint32_t other = compare ? (opcode == 0xAE ? cpu->eax : value) ^
                ((repeat == 2 ? i != stop : i == stop) ? 1u : 0) : 0x5A5A5A5Au;
            for (unsigned b = 0; b < width; b++) {
                vm->mem[bases[segment] + 0x4000u + i * delta + b] = value >> (b * 8u);
                vm->mem[0x28000u + i * delta + b] = other >> (b * 8u);
            }
        }
        uint32_t next = dos_string_test_code(vm, opcode, width, adr32, repeat, prefixes[segment]);
        cpu8086_state_t original = *cpu, expected = original;
        unsigned done = 0;
        bool ok = true;
        do {
            unsigned chunk = target - done > 256u ? 256u : target - done;
            done += chunk;
            if (!dos_test_run_one(vm)) ok = false;
            if (source) expected.esi = adr32 ? original.esi + done * delta
                : (original.esi & 0xFFFF0000u) | (uint16_t)(original.si + done * delta);
            if (destination) expected.edi = adr32 ? original.edi + done * delta
                : (original.edi & 0xFFFF0000u) | (uint16_t)(original.di + done * delta);
            if (repeat) expected.ecx = adr32 ? requested - done
                : (original.ecx & 0xFFFF0000u) | (uint16_t)(requested - done);
            uint32_t value = 0x89AB7FF0u + done - 1u;
            if (done && opcode == 0xAC) {
                uint32_t mask = width == 1 ? 0xFFu : width == 2 ? 0xFFFFu : UINT32_MAX;
                expected.eax = (original.eax & ~mask) | (value & mask);
            }
            if (done && compare) {
                uint32_t left = opcode == 0xAE ? original.eax : value;
                uint32_t right = left ^ ((repeat == 2 ? done - 1u != stop : done - 1u == stop) ? 1u : 0);
                expected.eflags = dos_string_compare_flags(width, original.eflags, left, right);
            }
            if (!dos_test_general_registers(cpu, &expected) || cpu->esp != original.esp ||
                cpu->eflags != expected.eflags || cpu->eip != (done < target ? 0x1000u : next) ||
                cpu->rep_compare.active != (compare && done < target) ||
                vm->dpmi.exception_depth) ok = false;
        } while (done < target && ok);
        for (unsigned i = 0; i <= count; i++) {
            uint32_t value = 0x89AB7FF0u + i;
            uint32_t other = compare ? (opcode == 0xAE ? original.eax : value) ^
                ((repeat == 2 ? i != stop : i == stop) ? 1u : 0) : 0x5A5A5A5Au;
            if (i < done && opcode == 0xA4) other = value;
            if (i < done && opcode == 0xAA) other = original.eax;
            for (unsigned b = 0; b < width; b++) {
                if (vm->mem[bases[segment] + 0x4000u + i * delta + b] != (uint8_t)(value >> (b * 8u)) ||
                    vm->mem[0x28000u + i * delta + b] != (uint8_t)(other >> (b * 8u))) ok = false;
            }
        }
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-STRING-MEM] case="); serial_putdec(checks);
                serial_puts(" op="); serial_puthex(opcode, 2);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" flags="); serial_puthex(cpu->eflags, 8);
                serial_puts(" expected="); serial_puthex(expected.eflags, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-STRING-MEM] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static uint32_t dos_string_test_physical(uint32_t offset)
{
    return offset < 4096u ? 0x20000u + offset : 0x24000u + offset - 4096u;
}

static int dos_string_paging_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint8_t opcodes[] = { 0xA4, 0xA6, 0xAA, 0xAC, 0xAE };
    const uint32_t bases[] = { 0x6000, 0x403FF000 };
    unsigned checks = 0, faults = 0, retries = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned op = 0; op < sizeof(opcodes); op++)
    for (unsigned width = 1; width <= 4; width *= 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned backwards = 0; backwards < 2; backwards++)
    for (unsigned region = 0; region < 2; region++)
    for (unsigned partial = 0; partial < 2; partial++)
    for (unsigned operand = 0; operand < 2; operand++)
    for (unsigned scenario = 0; scenario < (region ? 7u : 4u); scenario++) {
        uint8_t opcode = opcodes[op];
        bool source = opcode == 0xA4 || opcode == 0xA6 || opcode == 0xAC;
        bool destination = opcode != 0xAC;
        bool compare = opcode == 0xA6 || opcode == 0xAE;
        if (operand ? !destination : !source) continue;
        dos_fetch_test_prepare(vm, cpu, mode);
        unsigned repeat = !region && !partial ? 0u : compare && (adr32 ^ backwards) ? 2u : 1u;
        unsigned count = !repeat ? 1u : partial ? 303u : 3u;
        int32_t delta = backwards ? -(int32_t)width : (int32_t)width;
        uint32_t offset = partial ? backwards ? 4096u + 299u * width : 4096u - 300u * width
                                  : backwards || width > 1 ? 4095u : 4096u;
        uint32_t base = bases[region];
        dpmi_desc_set_base(&vm->dpmi.ldt[3], operand ? 0x30000u : base);
        dpmi_desc_set_base(&vm->dpmi.ldt[4], operand ? base : 0x40000u);
        cpu->fs = cpu->ds;
        cpu->ds = 0; /* Only the FS override is a valid source. */
        cpu->esi = (adr32 ? 0 : 0x56780000u) | (operand ? 0x2000u : offset);
        cpu->edi = (adr32 ? 0 : 0x9ABC0000u) | (operand ? offset : 0x2000u);
        cpu->ecx = (adr32 ? 0 : 0x12340000u) | (repeat ? count : 0x2468u);
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF | FLAG_AF | FLAG_OF |
                      (backwards ? FLAG_DF : 0);
        vm->dpmi.virtual_interrupts_enabled = true;
        dos_test_page_pair_t pair = dos_test_page_pair(vm, base);
        unsigned page = backwards ? 0u : 1u;
        bool write = operand && (opcode == 0xA4 || opcode == 0xAA);
        bool failed = scenario && (scenario % 3u != 2u || write);
        uint32_t entry = scenario < 4 ? pair.pte[page] : pair.pde[page];
        uint32_t saved_entry = dos_mem_read32(vm, entry);
        if (scenario) dos_mem_write32(vm, entry, saved_entry & ~(1u << ((scenario - 1u) % 3u)));
        for (unsigned i = 0; i <= count; i++) {
            uint32_t value = 0x89AB7FF0u + i;
            uint32_t other = compare ? (opcode == 0xAE ? cpu->eax : value) ^ (repeat == 2 ? 1u : 0) : 0x5A5A5A5Au;
            for (unsigned b = 0; b < width; b++) {
                uint32_t at = offset + i * delta + b;
                vm->mem[operand ? 0x32000u + i * delta + b : dos_string_test_physical(at)] = value >> (b * 8u);
                vm->mem[operand ? dos_string_test_physical(at) : 0x42000u + i * delta + b] = other >> (b * 8u);
                if (!region) vm->mem[base + at] = 0x6C;
            }
        }
        uint32_t next = dos_string_test_code(vm, opcode, width, adr32, repeat, 0x64);
        cpu8086_state_t original = *cpu, expected = original;
        unsigned done = failed ? partial ? 300u : 0u : count;
        bool ok = true;
        for (unsigned dispatch = 0; dispatch < 3; dispatch++) {
            if (!dos_test_run_one(vm)) ok = false;
            if (vm->dpmi.exception_depth || cpu->eip != 0x1000) break;
        }
        if (source) expected.esi = adr32 ? original.esi + done * delta
            : (original.esi & 0xFFFF0000u) | (uint16_t)(original.si + done * delta);
        if (destination) expected.edi = adr32 ? original.edi + done * delta
            : (original.edi & 0xFFFF0000u) | (uint16_t)(original.di + done * delta);
        if (repeat) expected.ecx = adr32 ? count - done
            : (original.ecx & 0xFFFF0000u) | (uint16_t)(count - done);
        uint32_t mask = width == 1 ? 0xFFu : width == 2 ? 0xFFFFu : UINT32_MAX;
        if (done && opcode == 0xAC) expected.eax = (original.eax & ~mask) | ((0x89AB7FF0u + done - 1u) & mask);
        if (!failed && done && compare) {
            uint32_t left = opcode == 0xAE ? original.eax : 0x89AB7FF0u + done - 1u;
            expected.eflags = dos_string_compare_flags(width, original.eflags, left, left ^ (repeat == 2 ? 1u : 0));
        }
        if (failed) {
            unsigned error = 4u | (write ? 2u : 0) | (scenario % 3u == 1u ? 0 : 1u);
            uint32_t fault_offset = offset + done * delta;
            if (!backwards && fault_offset < 4096u) fault_offset = 4096u;
            if (!dos_test_memory_fault(vm, &expected, 14, error) || cpu->cr2 != base + fault_offset)
                ok = false;
            uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
            uint32_t saved_flags = mode == 2 ? dos_mem_read32(vm, frame + 20u) : dos_mem_read16(vm, frame + 10u);
            if (saved_flags != (mode == 2 ? original.eflags : (uint16_t)original.eflags)) ok = false;
            faults++;
        } else if (!dos_test_general_registers(cpu, &expected) || cpu->esp != original.esp ||
                   cpu->eflags != expected.eflags || cpu->eip != next ||
                   cpu->cr2 != original.cr2 || vm->dpmi.exception_depth) ok = false;
        if (cpu->rep_compare.active) ok = false;
        bool used[2] = { false, false };
        for (unsigned i = 0; i <= count; i++) {
            uint32_t value = 0x89AB7FF0u + i;
            uint32_t other = compare ? (opcode == 0xAE ? original.eax : value) ^ (repeat == 2 ? 1u : 0) : 0x5A5A5A5Au;
            if (i < done && opcode == 0xA4) other = value;
            if (i < done && opcode == 0xAA) other = original.eax;
            for (unsigned b = 0; b < width; b++) {
                uint32_t at = offset + i * delta + b;
                if (i < done) used[at >= 4096u] = true;
                if (vm->mem[operand ? 0x32000u + i * delta + b : dos_string_test_physical(at)] != (uint8_t)(value >> (b * 8u)) ||
                    vm->mem[operand ? dos_string_test_physical(at) : 0x42000u + i * delta + b] != (uint8_t)(other >> (b * 8u)) ||
                    (!region && vm->mem[base + at] != 0x6C)) ok = false;
            }
        }
        for (unsigned i = 0; i < 2; i++)
            if ((dos_mem_read32(vm, pair.pte[i]) & 0x60u) != (used[i] ? write ? 0x60u : 0x20u : 0)) ok = false;
        if (failed && region && scenario == 1 && adr32) {
            dos_mem_write32(vm, entry, saved_entry);
            vm->mem[0x60E0] = 0xCB;
            vm->step_limit = vm->step_count + 16u;
            bool returned = dos_test_run_until(vm, true, original.cs, original.eip);
            vm->step_limit = 0;
            /* Completed MOVS iterations must not replay after the return. */
            if (done && opcode == 0xA4)
                vm->mem[operand ? 0x32000u : dos_string_test_physical(offset)] ^= 0xFF;
            if (!returned || !dos_test_run_one(vm) || cpu->eip != next || vm->dpmi.exception_depth)
                ok = false;
            if (source) expected.esi = original.esi + count * delta;
            if (destination) expected.edi = original.edi + count * delta;
            expected.ecx = 0;
            if (opcode == 0xAC) expected.eax = (original.eax & ~mask) | ((0x89AB7FF0u + count - 1u) & mask);
            if (compare) {
                uint32_t left = opcode == 0xAE ? original.eax : 0x89AB7FF0u + count - 1u;
                expected.eflags = dos_string_compare_flags(width, original.eflags, left, left ^ (repeat == 2 ? 1u : 0));
            }
            if (!dos_test_general_registers(cpu, &expected) || cpu->esp != original.esp ||
                cpu->eflags != expected.eflags || cpu->rep_compare.active) ok = false;
            if (opcode == 0xA4 || opcode == 0xAA)
                for (unsigned i = 0; i < count; i++)
                for (unsigned b = 0; b < width; b++) {
                    uint32_t at = offset + i * delta + b;
                    uint32_t value = opcode == 0xA4 ? 0x89AB7FF0u + i : original.eax;
                    if (vm->mem[operand ? dos_string_test_physical(at) : 0x42000u + i * delta + b] !=
                        (uint8_t)(value >> (b * 8u))) ok = false;
                }
            retries++;
        }
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-STRING-PAGING] case="); serial_putdec(checks);
                serial_puts(" op="); serial_puthex(opcode, 2);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" partial="); serial_putdec(partial);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" count="); serial_puthex(cpu->ecx, 8);
                serial_puts(" cr2="); serial_puthex(cpu->cr2, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-STRING-PAGING] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_string_boundary_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint8_t opcodes[] = { 0xA4, 0xA6, 0xAA, 0xAC, 0xAE };
    unsigned checks = 0;
    int failures = 0;
    /* Zero count must not inspect inaccessible data, but LOCK still faults. */
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned op = 0; op < sizeof(opcodes); op++)
    for (unsigned width = 1; width <= 4; width *= 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned locked = 0; locked < 2; locked++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        cpu->ds = cpu->es = 0;
        cpu->esi = cpu->edi = 0x100FFFF;
        cpu->ecx = adr32 ? 0 : 0xA5A50000u;
        uint32_t start = cpu->eip;
        if (locked) vm->mem[cpu->eip++] = 0xF0;
        uint32_t next = dos_string_test_code(vm, opcodes[op], width, adr32, 1, 0);
        cpu->eip = start;
        cpu8086_state_t original = *cpu;
        bool ok = dos_test_run_one(vm) && dos_test_general_registers(cpu, &original);
        if (locked) {
            if (mode) ok &= dos_test_memory_fault(vm, &original, 6, 0);
            else ok &= cpu->eip == 0x6060 && cpu->cs == 0 && cpu->sp == original.sp - 6u &&
                       dos_mem_read16(vm, cpu->sp) == start;
        } else ok &= cpu->eip == next && cpu->esp == original.esp && cpu->eflags == original.eflags;
        if (cpu->rep_compare.active || cpu->cr2 != original.cr2) ok = false;
        if (!ok) { failures++; serial_puts("[DOS-STRING-BOUNDARY] zero/lock case="); serial_putdec(checks); serial_puts("\n"); }
        checks++;
    }
    /* Index wrap is between elements, never within a multi-byte element. */
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned op = 0; op < sizeof(opcodes); op++)
    for (unsigned width = 1; width <= 4; width *= 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned backwards = 0; backwards < 2; backwards++) {
        if (!mode && adr32) continue;
        dos_port_test_prepare(vm, cpu, mode);
        if (adr32) {
            dpmi_desc_set_limit(&vm->dpmi.ldt[3], UINT32_MAX);
            dpmi_desc_set_limit(&vm->dpmi.ldt[4], UINT32_MAX);
        }
        uint8_t opcode = opcodes[op];
        bool source = opcode == 0xA4 || opcode == 0xA6 || opcode == 0xAC;
        bool destination = opcode != 0xAC;
        bool compare = opcode == 0xA6 || opcode == 0xAE;
        uint32_t mask = adr32 ? UINT32_MAX : 0xFFFFu;
        int32_t delta = backwards ? -(int32_t)width : (int32_t)width;
        uint32_t offset = backwards ? 0u : mask - width + 1u;
        cpu->esi = (adr32 ? 0 : 0x56780000u) | offset;
        cpu->edi = (adr32 ? 0 : 0x9ABC0000u) | offset;
        cpu->ecx = (adr32 ? 0 : 0x12340000u) | 2;
        cpu->eax = 0xA5A5A5A5u;
        cpu->eflags = FLAGS_FIXED | FLAG_CF | FLAG_OF | (backwards ? FLAG_DF : 0);
        for (unsigned i = 0; i < 2; i++) {
            uint32_t at = (offset + i * delta) & mask;
            for (unsigned b = 0; b < width; b++) {
                vm->mem[(uint32_t)(0x10000u + at + b)] = 0xA5;
                vm->mem[(uint32_t)(0x20000u + at + b)] = compare ? 0xA5 : 0x5A;
            }
        }
        uint32_t next = dos_string_test_code(vm, opcode, width, adr32, 1, 0);
        cpu8086_state_t original = *cpu, expected = original;
        uint32_t advanced = (offset + 2u * delta) & mask;
        if (source) expected.esi = (original.esi & ~mask) | advanced;
        if (destination) expected.edi = (original.edi & ~mask) | advanced;
        expected.ecx &= ~mask;
        if (compare) expected.eflags = dos_string_compare_flags(width, original.eflags, 0xA5A5A5A5, 0xA5A5A5A5);
        bool ok = dos_test_run_one(vm) && cpu->eip == next &&
            dos_test_general_registers(cpu, &expected) && cpu->eflags == expected.eflags &&
            cpu->esp == original.esp && !vm->dpmi.exception_depth;
        for (unsigned i = 0; i < 2; i++) {
            uint32_t at = (offset + i * delta) & mask;
            for (unsigned b = 0; b < width; b++)
                if (vm->mem[(uint32_t)(0x10000u + at + b)] != 0xA5 ||
                    vm->mem[(uint32_t)(0x20000u + at + b)] != (opcode == 0xAC ? 0x5A : 0xA5)) ok = false;
        }
        if (!ok) { failures++; serial_puts("[DOS-STRING-BOUNDARY] wrap case="); serial_putdec(checks); serial_puts("\n"); }
        checks++;
    }
    /* MOVS overlap follows sequential element snapshots, not memmove. */
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned width = 1; width <= 4; width *= 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned backwards = 0; backwards < 2; backwards++)
    for (unsigned overlap = 0; overlap < 4; overlap++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        if (!mode) cpu->ds = cpu->es = 0;
        uint8_t expected[192];
        for (unsigned i = 0; i < sizeof(expected); i++) expected[i] = vm->mem[0x4000u + i] = i * 37u;
        int32_t delta = backwards ? -(int32_t)width : (int32_t)width;
        int32_t shift = (overlap & 1u) ? (int32_t)width : 1;
        if (overlap & 2u) shift = -shift;
        unsigned from = 72u + (backwards ? 16u * width : 0);
        unsigned to = from + shift;
        cpu->esi = 0x4000u + from; cpu->edi = 0x4000u + to; cpu->ecx = 17;
        cpu->eflags = FLAGS_FIXED | FLAG_CF | (backwards ? FLAG_DF : 0);
        uint32_t flags = cpu->eflags;
        for (unsigned i = 0; i < 17; i++) {
            uint8_t element[4];
            for (unsigned b = 0; b < width; b++) element[b] = expected[from + i * delta + b];
            for (unsigned b = 0; b < width; b++) expected[to + i * delta + b] = element[b];
        }
        uint32_t next = dos_string_test_code(vm, 0xA4, width, adr32, 1, 0);
        bool ok = dos_test_run_one(vm) && cpu->eip == next && !cpu->ecx && cpu->eflags == flags &&
            cpu->esi == 0x4000u + from + 17u * delta && cpu->edi == 0x4000u + to + 17u * delta;
        for (unsigned i = 0; i < sizeof(expected); i++) if (vm->mem[0x4000u + i] != expected[i]) ok = false;
        if (!ok) { failures++; serial_puts("[DOS-STRING-BOUNDARY] overlap case="); serial_putdec(checks); serial_puts("\n"); }
        checks++;
    }
    serial_puts("[DOS-STRING-BOUNDARY] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_string_restart_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned scan = 0; scan < 2; scan++)
    for (unsigned width = 1; width <= 4; width *= 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned repeat = 1; repeat <= 2; repeat++)
    for (unsigned event = 0; event < 5; event++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        dpmi_desc_set_base(&vm->dpmi.ldt[3], 0x30000);
        dpmi_desc_set_base(&vm->dpmi.ldt[4], 0x403FF000);
        cpu->fs = cpu->ds;
        cpu->eax = 0xA5A5A5A5;
        cpu->esi = 0x2000; cpu->edi = 4096u - 300u * width; cpu->ecx = 303;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF | FLAG_OF | FLAG_AF | FLAG_SF;
        vm->dpmi.virtual_interrupts_enabled = true;
        dos_test_page_pair_t pair = dos_test_page_pair(vm, 0x403FF000);
        dos_mem_write32(vm, pair.pte[1], 0x24006);
        for (unsigned i = 0; i < 303; i++)
        for (unsigned b = 0; b < width; b++) {
            vm->mem[0x32000u + i * width + b] = 0xA5;
            vm->mem[dos_string_test_physical(cpu->edi + i * width + b)] =
                b == 0 && repeat == 2 ? 0xA4 : 0xA5;
        }
        uint32_t next = dos_string_test_code(vm, scan ? 0xAE : 0xA6, width, adr32, repeat, 0x3E);
        cpu8086_state_t original = *cpu;
        bool ok = dos_test_run_one(vm) && cpu->rep_compare.active && cpu->ecx == 47 && cpu->eip == original.eip;
        uint32_t restart_flags = original.eflags;
        uint32_t after_chunk = cpu->eflags;
        if (event == 1 || event == 2) {
            vm->dpmi.pm_vectors[8].sel = original.cs;
            vm->dpmi.pm_vectors[8].off = 0x5000;
            vm->mem[0x5000] = 0xCF;
            vm->dpmi.virtual_interrupts_enabled = event == 1;
            bool delivered = cpu_deliver_hw_interrupt(vm, 8);
            if (event == 1) {
                if (!delivered || cpu->rep_compare.active) ok = false;
                vm->step_limit = vm->step_count + 16;
                if (!dos_test_run_until(vm, true, original.cs, original.eip)) ok = false;
                vm->step_limit = 0;
                restart_flags = after_chunk;
            } else if (delivered || !cpu->rep_compare.active || cpu->eip != original.eip) ok = false;
            vm->dpmi.virtual_interrupts_enabled = true;
        } else if (event == 3) {
            /* A changed segment prefix is a newly decoded instruction. */
            vm->mem[next - 3u] = 0x64;
            restart_flags = after_chunk;
        } else if (event == 4) {
            dos_mem_write32(vm, 0x11004, 0x1006); /* Fault while fetching the next chunk. */
        }
        unsigned done = event == 4 ? 256u : 300u;
        cpu8086_state_t expected = original;
        expected.eflags = restart_flags;
        expected.ecx = 303u - done;
        if (!scan) expected.esi += done * width;
        expected.edi += done * width;
        if (!dos_test_run_one(vm) || !dos_test_memory_fault(vm, &expected, 14, 4) ||
            cpu->cr2 != (event == 4 ? 0x1000u : 0x40400000u) || cpu->rep_compare.active) ok = false;
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-STRING-RESTART] case="); serial_putdec(checks);
                serial_puts(" event="); serial_putdec(event);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" count="); serial_putdec(cpu->ecx); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-STRING-RESTART] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_string_vga_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    uint8_t old_mode = vm->vga_mode;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned compare = 0; compare < 2; compare++)
    for (unsigned repeat = 1; repeat <= 2; repeat++)
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        vm->vga_mode = 0x13;
        dos_io_vga_set_mode(vm, 0x13, true);
        dos_io_write8(vm, 0x3C4, 4); dos_io_write8(vm, 0x3C5, 6);
        dos_io_vga_write_memory(vm, 0xA0010, 0x3C);
        dos_io_vga_write_memory(vm, 0xA0020, 0xA5);
        dos_io_vga_write_memory(vm, 0xA0021, 0x6A);
        dos_io_vga_write_memory(vm, 0xA0022, 0);
        (void)dos_io_vga_read_memory(vm, 0xA0010);
        dpmi_desc_set_base(&vm->dpmi.ldt[3], 0xA0000);
        dpmi_desc_set_limit(&vm->dpmi.ldt[4], scenario == 1 ? 0x4FFF : scenario == 2 ? 0x5000 : 0xFFFF);
        cpu->esi = 0x20; cpu->edi = 0x5000; cpu->ecx = 303;
        vm->mem[0x5000] = repeat == 1 ? 0xA5 : 0xA4;
        vm->mem[0x5001] = repeat == 1 ? 0x6A : 0x6B;
        if (scenario == 3) {
            (void)dos_test_page_pair(vm, 0x80000);
            dos_mem_write32(vm, 0x11014, 0x5006);
        }
        uint32_t next = dos_string_test_code(vm, compare ? 0xA6 : 0xA4, 1, mode == 2, repeat, 0);
        cpu8086_state_t original = *cpu, expected = original;
        unsigned done = scenario == 2 ? 1u : scenario ? 0u : compare ? 3u : 256u;
        /* Stop the valid comparison on its third element. */
        vm->mem[0x5002] = repeat == 1 ? 1 : 0;
        bool ok = dos_test_run_one(vm);
        expected.esi += done; expected.edi += done; expected.ecx -= done;
        if (scenario) {
            if (!dos_test_memory_fault(vm, &expected, scenario == 3 ? 14 : 13,
                                       scenario == 3 ? compare ? 4 : 6 : 0)) ok = false;
        } else if (!dos_test_general_registers(cpu, &expected) || cpu->eip != (compare ? next : original.eip)) ok = false;
        if (scenario) {
            dos_io_write8(vm, 0x3CE, 5); dos_io_write8(vm, 0x3CF, 0x41);
            dos_io_vga_write_memory(vm, 0xA0030, 0);
            for (unsigned plane = 0; plane < 4; plane++) {
                dos_io_write8(vm, 0x3CE, 4); dos_io_write8(vm, 0x3CF, plane);
                if (dos_io_vga_read_memory(vm, 0xA0030) != (done ? 0xA5 : 0x3C)) ok = false;
            }
        }
        if (!ok) { failures++; serial_puts("[DOS-STRING-VGA] case="); serial_putdec(checks); serial_puts("\n"); }
        checks++;
    }
    dos_io_vga_set_mode(vm, 0x13, true);
    vm->vga_mode = old_mode;
    serial_puts("[DOS-STRING-VGA] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_paged_operand_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint32_t bases[] = { 0x6000, 0x3FF000, 0x403FF000 };
    enum { VALID, PTE_ABSENT, PTE_READ_ONLY, PTE_SUPERVISOR,
           PDE_ABSENT, PDE_READ_ONLY, PDE_SUPERVISOR, SEGMENT_LIMIT, SCENARIOS };
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned input = 0; input < 2; input++)
    for (unsigned width = 1; width <= 4; width *= 2)
    for (unsigned region = 0; region < 3; region++)
    for (unsigned partial = 0; partial < 2; partial++)
    for (unsigned scenario = 0; scenario < SCENARIOS; scenario++) {
        if (!region && scenario >= PDE_ABSENT && scenario <= PDE_SUPERVISOR) continue;
        dos_port_test_prepare(vm, cpu, mode);
        unsigned offset = partial ? 4096u - 2u * width : 4095u;
        uint32_t linear = bases[region] + offset;
        dos_test_page_pair_t pair = dos_test_page_pair(vm, linear);
        dpmi_descriptor_t *data = &vm->dpmi.ldt[input ? 4 : 3];
        dpmi_desc_set_base(data, bases[region]);
        if (scenario == SEGMENT_LIMIT) dpmi_desc_set_limit(data, offset + width - 2u);
        bool denied = scenario == PTE_ABSENT || scenario == PDE_ABSENT ||
                      scenario == PTE_SUPERVISOR || scenario == PDE_SUPERVISOR ||
                      (input && (scenario == PTE_READ_ONLY || scenario == PDE_READ_ONLY));
        if (scenario >= PTE_ABSENT && scenario <= PDE_SUPERVISOR) {
            uint32_t entry = scenario <= PTE_SUPERVISOR ? pair.pte[1] : pair.pde[1];
            unsigned bit = (scenario - PTE_ABSENT) % 3;
            dos_mem_write32(vm, entry, dos_mem_read32(vm, entry) & ~(1u << bit));
        }
        cpu->cr2 = 0xCAFE1234u;
        cpu->esi = cpu->edi = offset;
        cpu->ecx = 3;
        cpu->edx = 0x3C9;
        cpu->eax = 0xA1B2C3D4u;
        cpu->eflags = FLAGS_FIXED | FLAG_CF | FLAG_ZF;
        for (unsigned i = 0; i < 4096u; i++) {
            vm->mem[0x20000 + i] = input ? 0xA5 : 0x2A;
            vm->mem[0x24000 + i] = input ? 0xA5 : 0x2A;
        }
        unsigned done = 0;
        while (done < 3 && scenario != SEGMENT_LIMIT &&
               (!denied || offset + (done + 1u) * width <= 4096u)) done++;
        bool failed = done < 3;
        uint32_t p = 0x1000;
        if ((width == 4) != cpu->op_size_32) vm->mem[p++] = 0x66;
        vm->mem[p++] = 0xF3;
        vm->mem[p++] = (input ? 0x6C : 0x6E) | (width != 1);
        bool ok = dos_test_run_one(vm) && cpu->ecx == 3u - done &&
            (input ? cpu->edi : cpu->esi) == offset + done * width &&
            (input ? cpu->esi : cpu->edi) == offset && cpu->eax == 0xA1B2C3D4u;
        if (failed) {
            faults++;
            unsigned vector = scenario == SEGMENT_LIMIT ? 13u : 14u;
            uint32_t expected_cr2 = linear + done * width;
            if (expected_cr2 < bases[region] + 4096u) expected_cr2 = bases[region] + 4096u;
            if (scenario == SEGMENT_LIMIT) expected_cr2 = 0xCAFE1234u;
            unsigned error = scenario == SEGMENT_LIMIT ? 0 : 4u | (input ? 2u : 0) |
                (scenario == PTE_ABSENT || scenario == PDE_ABSENT ? 0 : 1u);
            unsigned size = mode == 2 ? 4u : 2u;
            uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
            if (cpu->eip != 0x6000u + vector * 16u || cpu->cr2 != expected_cr2 ||
                vm->dpmi.exception_depth != 1 ||
                (size == 4 ? dos_mem_read32(vm, frame + 8u) : dos_mem_read16(vm, frame + 4u)) != error ||
                (size == 4 ? dos_mem_read32(vm, frame + 12u) : dos_mem_read16(vm, frame + 6u)) != 0x1000u)
                ok = false;
        } else if (cpu->eip != p || cpu->cr2 != 0xCAFE1234u) ok = false;
        if (input) {
            for (unsigned i = 0; i < 3; i++)
            for (unsigned b = 0; b < width; b++) {
                unsigned current = offset + i * width + b;
                uint32_t physical = current < 4096u ? 0x20000u + current
                                                    : 0x24000u + current - 4096u;
                uint8_t expected = i >= done ? 0xA5 : b == 0 ? i : b == 1 ? 0x35 : b == 2 ? 0xFF : 0x6B;
                if (vm->mem[physical] != expected) ok = false;
            }
            if (dos_io_read8(vm, 0x3C9) != done) ok = false;
        } else {
            dos_io_write8(vm, 0x3C7, 0);
            for (unsigned i = 0; i <= done; i++)
                if (dos_io_read8(vm, 0x3C9) != (i < done ? 0x2A : i)) ok = false;
        }
        bool first_used = done != 0;
        bool second_used = done && offset + done * width > 4096u;
        if ((dos_mem_read32(vm, pair.pte[0]) & 0x60u) != (first_used ? input ? 0x60u : 0x20u : 0) ||
            (dos_mem_read32(vm, pair.pte[1]) & 0x60u) != (second_used ? input ? 0x60u : 0x20u : 0)) ok = false;
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-PAGED-OPERAND] port mismatch case="); serial_putdec(checks);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" cr2="); serial_puthex(cpu->cr2, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    /* LDS snapshots its split-page pointer before changing DS or EAX. */
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned region = 0; region < 3; region++)
    for (unsigned missing = 0; missing < 2; missing++) {
        dos_port_test_prepare(vm, cpu, mode);
        dos_test_page_pair_t pair = dos_test_page_pair(vm, bases[region] + 4095u);
        dpmi_desc_set_base(&vm->dpmi.ldt[3], bases[region]);
        uint16_t source = cpu->ds, target = dpmi_index_to_sel(7);
        dos_test_descriptor(&vm->dpmi, target, false, mode == 2);
        unsigned width = wide ? 4u : 2u;
        uint32_t value = wide ? 0x12345678u : 0x5678u;
        uint8_t bytes[] = { 0x78, 0x56, 0x34, 0x12, 0, 0 };
        bytes[width] = target;
        bytes[width + 1u] = target >> 8;
        for (unsigned i = 0; i < width + 2u; i++)
            vm->mem[i ? 0x24000u + i - 1u : 0x20FFFu] = bytes[i];
        if (missing) dos_mem_write32(vm, pair.pte[1], 0x24006);
        cpu->cr2 = 0xCAFE1234u;
        cpu->eax = 0xABCD9876u;
        uint32_t p = 0x1000;
        if (wide != cpu->op_size_32) vm->mem[p++] = 0x66;
        vm->mem[p++] = 0xC5;
        vm->mem[p++] = mode == 2 ? 0x05 : 0x06;
        vm->mem[p++] = 0xFF;
        vm->mem[p++] = 0x0F;
        if (mode == 2) { vm->mem[p++] = 0; vm->mem[p++] = 0; }
        bool ok = dos_test_run_one(vm);
        if (missing) {
            faults++;
            if (cpu->eip != 0x60E0u || cpu->cr2 != bases[region] + 4096u ||
                cpu->ds != source || cpu->eax != 0xABCD9876u ||
                (vm->dpmi.ldt[7].access & DESC_ACCESSED)) ok = false;
        } else if (cpu->eip != p || cpu->cr2 != 0xCAFE1234u || cpu->ds != target ||
                   cpu->eax != (wide ? value : 0xABCD0000u | value) ||
                   !(vm->dpmi.ldt[7].access & DESC_ACCESSED)) ok = false;
        if (!ok) {
            serial_puts("[DOS-PAGED-OPERAND] far mismatch case="); serial_putdec(checks); serial_puts("\n");
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-PAGED-OPERAND] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_port_boundary_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    enum { SPAN_LIMIT, HIGH_OFFSET, NULL_SEGMENT, ABSENT, READ_ONLY,
           EXECUTE_ONLY, READABLE_CODE, LOW_DPL, EXPAND_VALID, EXPAND_INVALID,
           EXPAND_TOP16, EXPAND_TOP32, PARTIAL, LOCKED, LOCKED_ZERO,
           ZERO_INVALID, STACK_LIMIT, SCENARIOS };
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned input = 0; input < 2; input++)
    for (unsigned width = 1; width <= 4; width *= 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned scenario = 0; scenario < SCENARIOS; scenario++) {
        dos_port_test_prepare(vm, cpu, mode);
        unsigned segment = input ? 0u : scenario == STACK_LIMIT ? 2u : 3u;
        unsigned index = input ? 4u : segment == 2 ? 2u : 3u;
        uint32_t base = input ? 0x20000u : segment == 2 ? 0 : 0x10000u;
        dpmi_descriptor_t *descriptor = &vm->dpmi.ldt[index];
        uint16_t *selector = input ? &cpu->es : segment == 2 ? &cpu->ss : &cpu->ds;
        uint32_t offset = scenario == SPAN_LIMIT || scenario == STACK_LIMIT ||
                          scenario == EXPAND_TOP16 || scenario == EXPAND_TOP32
                          ? 0xFFFEu : scenario == HIGH_OFFSET ? 0x10000u : 0x4800u;
        uint32_t limit = 0xFFFFu, top = 0xFFFFFFFFu;
        bool expand = false, allowed = true;
        if (mode) {
            if (scenario == NULL_SEGMENT || scenario == ZERO_INVALID) {
                *selector = 0;
                allowed = false;
            }
            if (scenario == ABSENT) { descriptor->access &= ~DESC_PRESENT; allowed = false; }
            if (scenario == READ_ONLY) { descriptor->access = 0xF0; allowed = !input; }
            if (scenario == EXECUTE_ONLY) { descriptor->access = 0xF8; allowed = false; }
            if (scenario == READABLE_CODE) { descriptor->access = 0xFA; allowed = !input; }
            if (scenario == LOW_DPL) { descriptor->access = 0x92; allowed = false; }
            if (scenario >= EXPAND_VALID && scenario <= EXPAND_TOP32) {
                expand = true;
                descriptor->access = 0xF6;
                limit = scenario == EXPAND_INVALID ? offset : 0x4000u;
                top = scenario == EXPAND_TOP16 ? 0xFFFFu : 0xFFFFFFFFu;
                descriptor->flags_lim = top == 0xFFFFu ? 0 : DESC_32BIT;
            }
            if (scenario == PARTIAL) limit = offset + 2u * width - 1u;
            dpmi_desc_set_limit(descriptor, limit);
        }
        /* The exception must see 511 remaining, not a truncated chunk count. */
        unsigned count = scenario == LOCKED_ZERO || scenario == ZERO_INVALID ? 0 :
                         mode && scenario == PARTIAL ? 513 : 3;
        bool locked = scenario == LOCKED || scenario == LOCKED_ZERO;
        uint32_t start = adr32 ? offset : 0xABCD0000u | (uint16_t)offset;
        cpu->esi = cpu->edi = start;
        cpu->ecx = (adr32 ? 0 : 0x56780000u) | count;
        cpu->edx = 0x123403C9u;
        cpu->eax = 0xA1B2C3D4u;
        uint32_t flags = FLAGS_FIXED | FLAG_CF | FLAG_ZF | FLAG_AF;
        cpu->eflags = flags;
        for (unsigned i = 0; i < 4; i++) {
            uint32_t effective = adr32 ? offset + i * width : (uint16_t)(offset + i * width);
            for (unsigned b = 0; b < width; b++)
                vm->mem[base + effective + b] = input ? 0xA5 : 0x2A;
        }
        unsigned done = 0;
        int vector = locked ? 6 : -1;
        while (!locked && done < count) {
            uint32_t effective = adr32 ? offset + done * width : (uint16_t)(offset + done * width);
            uint64_t last = (uint64_t)effective + width - 1u;
            if (!allowed || (expand ? effective <= limit || last > top : last > limit)) {
                vector = segment == 2 ? 12 : 13;
                break;
            }
            done++;
        }
        uint32_t p = 0x1000;
        if (locked) vm->mem[p++] = 0xF0;
        if ((width == 4) != cpu->op_size_32) vm->mem[p++] = 0x66;
        if (adr32 != cpu->addr_size_32) vm->mem[p++] = 0x67;
        if (segment == 2) vm->mem[p++] = 0x36;
        vm->mem[p++] = 0xF3;
        vm->mem[p++] = (input ? 0x6C : 0x6E) | (width != 1);
        bool ok = dos_test_run_one(vm);
        uint32_t next = adr32 ? start + done * width
                              : (start & 0xFFFF0000u) | (uint16_t)(start + done * width);
        if (cpu->eax != 0xA1B2C3D4u || cpu->edx != 0x123403C9u ||
            cpu->ecx != ((adr32 ? 0 : 0x56780000u) | (count - done)) ||
            (input ? cpu->edi : cpu->esi) != next ||
            (input ? cpu->esi : cpu->edi) != start) ok = false;
        if (vector >= 0) {
            faults++;
            unsigned size = mode == 2 ? 4u : 2u;
            uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
            uint32_t saved = frame + (mode ? 3u * size : 0);
            if (cpu->eip != 0x6000u + (unsigned)vector * 16u ||
                (size == 4 ? dos_mem_read32(vm, saved) : dos_mem_read16(vm, saved)) != 0x1000u ||
                (mode && vm->dpmi.exception_depth != 1) ||
                (mode && (size == 4 ? dos_mem_read32(vm, frame + 8u)
                                   : dos_mem_read16(vm, frame + 4u)) != 0)) ok = false;
        } else if (cpu->eip != p || cpu->eflags != flags) ok = false;
        if (input) {
            for (unsigned i = 0; i < 4; i++) {
                uint32_t effective = adr32 ? offset + i * width : (uint16_t)(offset + i * width);
                for (unsigned b = 0; b < width; b++) {
                    uint8_t expected = i >= done ? 0xA5 :
                        b == 0 ? i : b == 1 ? 0x35 : b == 2 ? 0xFF : 0x6B;
                    if (vm->mem[base + effective + b] != expected) ok = false;
                }
            }
            if (dos_io_read8(vm, 0x3C9) != done) ok = false;
        } else {
            dos_io_write8(vm, 0x3C7, 0);
            for (unsigned i = 0; i <= done; i++)
                if (dos_io_read8(vm, 0x3C9) != (i < done ? 0x2A : i)) ok = false;
        }
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-PORT-BOUNDARY] mismatch case="); serial_putdec(checks);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" vector="); serial_putdec((uint64_t)(vector + 1));
                serial_puts(" done="); serial_putdec(done); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned input = 0; input < 2; input++)
    for (unsigned quota = 1; quota <= 3; quota++) {
        dos_port_test_prepare(vm, cpu, mode);
        vm->step_limit = quota;
        cpu->ecx = 513;
        cpu->esi = cpu->edi = 0x4800;
        cpu->dx = 0x3C9;
        for (unsigned i = 0; i < 514; i++) {
            vm->mem[0x14800 + i] = 0x2A;
            vm->mem[0x24800 + i] = 0xA5;
        }
        vm->mem[0x1000] = 0xF3;
        vm->mem[0x1001] = input ? 0x6C : 0x6E;
        bool reached = dos_test_run_until(vm, mode != 0, cpu->cs, 0x1002);
        unsigned done = quota == 3 ? 513u : quota * 256u;
        bool ok = reached == (quota == 3) && vm->step_limit_reached == (quota < 3) &&
            vm->step_count == quota && cpu->ecx == 513u - done &&
            (input ? cpu->edi : cpu->esi) == 0x4800u + done &&
            cpu->eip == (quota == 3 ? 0x1002u : 0x1000u);
        if (input) {
            for (unsigned i = 0; i < 514; i++)
                if (vm->mem[0x24800 + i] != (i < done ? i & 63u : 0xA5)) ok = false;
            if (dos_io_read8(vm, 0x3C9) != (done & 63u)) ok = false;
        } else {
            dos_io_write8(vm, 0x3C7, 0);
            for (unsigned i = 0; i <= done; i++)
                if (dos_io_read8(vm, 0x3C9) != (i < done ? 0x2A : i & 63u)) ok = false;
        }
        if (!ok) failures++;
        checks++;
    }
    serial_puts("[DOS-PORT-BOUNDARY] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

typedef struct {
    uint32_t offset;
    unsigned width;
    bool write;
} dos_stack_test_access_t;

static uint32_t dos_stack_test_code(dos_vm_t *vm, unsigned instruction,
                                     unsigned width, bool adr32, bool locked,
                                     unsigned nesting, unsigned alloc)
{
    uint32_t p = 0x1000;
    if ((width == 4) != vm->cpu->op_size_32) vm->mem[p++] = 0x66;
    if (adr32 != vm->cpu->addr_size_32) vm->mem[p++] = 0x67;
    vm->mem[p++] = 0x64; /* FS must never redirect the implicit stack. */
    if (locked) vm->mem[p++] = 0xF0;
    if (instruction < 16) vm->mem[p++] = 0x50u + instruction;
    else if (instruction == 16) {
        vm->mem[p++] = 0x68;
        for (unsigned i = 0; i < width; i++) vm->mem[p++] = 0x89ABCDEFu >> (8u * i);
    } else if (instruction == 17) {
        vm->mem[p++] = 0x6A;
        vm->mem[p++] = 0x81;
    } else if (instruction < 20) vm->mem[p++] = instruction == 18 ? 0x9C : 0x9D;
    else if (instruction == 20 || instruction == 22) {
        vm->mem[p++] = instruction == 20 ? 0xFF : 0x8F;
        vm->mem[p++] = (instruction == 20 ? 0x30 : 0) | (adr32 ? 5 : 6);
        uint32_t address = instruction == 20 ? 0x4800 : 0x4810;
        for (unsigned i = 0; i < (adr32 ? 4u : 2u); i++) vm->mem[p++] = address >> (8u * i);
    } else if (instruction == 21 || instruction == 23) {
        vm->mem[p++] = instruction == 21 ? 0xFF : 0x8F;
        vm->mem[p++] = instruction == 21 ? 0xF4 : 0xC4;
    } else if (instruction < 30) {
        const uint8_t opcodes[] = { 0x06, 0x0E, 0x16, 0x1E, 0xA0, 0xA8 };
        if (instruction >= 28) vm->mem[p++] = 0x0F;
        vm->mem[p++] = opcodes[instruction - 24u];
    } else if (instruction < 32) vm->mem[p++] = instruction == 30 ? 0x60 : 0x61;
    else if (instruction == 32) {
        vm->mem[p++] = 0xC8;
        vm->mem[p++] = alloc;
        vm->mem[p++] = alloc >> 8;
        vm->mem[p++] = nesting;
    } else vm->mem[p++] = 0xC9;
    return p;
}

static uint32_t dos_stack_test_transfer(uint8_t *memory, uint32_t origin,
                                        dos_stack_test_access_t *accesses,
                                        unsigned *count, uint32_t offset,
                                        unsigned width, bool write, uint32_t value)
{
    accesses[(*count)++] = (dos_stack_test_access_t){ offset, width, write };
    uint32_t result = 0;
    for (unsigned i = 0; i < width; i++) {
        if (write) memory[offset - origin + i] = value >> (i * 8u);
        else result |= (uint32_t)memory[offset - origin + i] << (i * 8u);
    }
    return result;
}

/* A byte-array model records the architectural accesses independently of
 * the interpreter's segment resolver, page walker and stack helpers. */
static unsigned dos_stack_test_model(cpu8086_state_t *cpu, uint8_t *memory,
                                       uint32_t origin, unsigned instruction,
                                       unsigned width, bool stack32,
                                       unsigned nesting, unsigned alloc,
                                       dos_stack_test_access_t *accesses,
                                       uint32_t *external)
{
    uint32_t *regs[] = { &cpu->eax, &cpu->ecx, &cpu->edx, &cpu->ebx,
                         &cpu->esp, &cpu->ebp, &cpu->esi, &cpu->edi };
    uint32_t mask = stack32 ? UINT32_MAX : 0xFFFFu;
    uint32_t value_mask = width == 4 ? UINT32_MAX : 0xFFFFu;
    uint32_t saved_sp = cpu->esp, sp = cpu->esp & mask;
    unsigned count = 0;
    if (instruction == 30) {
        uint32_t values[] = { cpu->eax, cpu->ecx, cpu->edx, cpu->ebx,
                              saved_sp, cpu->ebp, cpu->esi, cpu->edi };
        for (unsigned i = 0; i < 8; i++) {
            sp = (sp - width) & mask;
            dos_stack_test_transfer(memory, origin, accesses, &count, sp, width, true, values[i]);
        }
    } else if (instruction == 31) {
        for (unsigned i = 0; i < 8; i++) {
            uint32_t value = dos_stack_test_transfer(memory, origin, accesses, &count, sp, width, false, 0);
            if (i != 3) *regs[7u - i] = (*regs[7u - i] & ~value_mask) | value;
            sp = (sp + width) & mask;
        }
    } else if (instruction == 32) {
        sp = (sp - width) & mask;
        uint32_t frame = (saved_sp & ~mask) | sp;
        dos_stack_test_transfer(memory, origin, accesses, &count, sp, width, true, cpu->ebp);
        for (unsigned i = 1; i < (nesting & 31u); i++) {
            uint32_t value = dos_stack_test_transfer(memory, origin, accesses, &count,
                (cpu->ebp - i * width) & mask, width, false, 0);
            sp = (sp - width) & mask;
            dos_stack_test_transfer(memory, origin, accesses, &count, sp, width, true, value);
        }
        if (nesting & 31u) {
            sp = (sp - width) & mask;
            dos_stack_test_transfer(memory, origin, accesses, &count, sp, width, true, frame);
        }
        sp = (sp - alloc) & mask;
        accesses[count++] = (dos_stack_test_access_t){ sp, 1, true };
        cpu->ebp = (cpu->ebp & ~value_mask) | (frame & value_mask);
    } else {
        bool pop = (instruction >= 8 && instruction < 16) || instruction == 19 ||
                    instruction == 22 || instruction == 23 || instruction == 33;
        if (pop) {
            if (instruction == 33) sp = cpu->ebp & mask;
            uint32_t value = dos_stack_test_transfer(memory, origin, accesses, &count, sp, width, false, 0);
            sp = (sp + width) & mask;
            cpu->esp = (saved_sp & ~mask) | sp;
            if (instruction == 19) {
                uint32_t mutable = width == 4 ? 0x00247FD5u : 0x7FD5u;
                if (cpu->protected_mode) mutable &= ~FLAG_IOPL_MASK;
                cpu->eflags = (((cpu->eflags & ~mutable) | (value & mutable)) & ~FLAG_RF) | FLAGS_FIXED;
                if (cpu->protected_mode) cpu->eflags |= FLAG_IF | FLAG_IOPL_MASK;
            } else if (instruction == 22) *external = value;
            else {
                unsigned reg = instruction == 23 ? 4u : instruction == 33 ? 5u : instruction - 8u;
                *regs[reg] = (*regs[reg] & ~value_mask) | value;
            }
            return count;
        }
        uint32_t value = instruction < 8 ? *regs[instruction]
                       : instruction == 16 ? 0x89ABCDEFu
                       : instruction == 17 ? 0xFFFFFF81u
                       : instruction == 18 ? cpu->eflags & ~(FLAG_RF | FLAG_VM)
                       : instruction == 20 ? 0x1234ABCDu : saved_sp;
        if (instruction >= 24) {
            const uint16_t selectors[] = { cpu->es, cpu->cs, cpu->ss, cpu->ds, cpu->fs, cpu->gs };
            value = selectors[instruction - 24u];
        }
        sp = (sp - width) & mask;
        dos_stack_test_transfer(memory, origin, accesses, &count, sp, width, true, value);
    }
    cpu->esp = (saved_sp & ~mask) | sp;
    return count;
}

static bool dos_stack_test_result(dos_vm_t *vm, const cpu8086_state_t *expected,
                                   uint32_t next, uint32_t external, unsigned width)
{
    cpu8086_state_t *cpu = vm->cpu;
    bool ok = dos_test_general_registers(cpu, expected) && cpu->esp == expected->esp &&
        cpu->eflags == expected->eflags && cpu->eip == next && cpu->ss == expected->ss &&
        cpu->ds == expected->ds && cpu->es == expected->es && cpu->cs == expected->cs &&
        cpu->fs == expected->fs && cpu->gs == expected->gs && cpu->cr2 == expected->cr2 &&
        !vm->dpmi.exception_depth;
    for (unsigned i = 0; i < 4; i++) {
        if (vm->mem[0x34800 + i] != (uint8_t)(0x1234ABCDu >> (8u * i)) ||
            vm->mem[0x34810 + i] != (i < width ? (uint8_t)(external >> (8u * i)) : 0x6C)) ok = false;
    }
    return ok;
}

static int dos_checked_stack_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    uint8_t *expected_bytes = dos_host_alloc_pages(2);
    if (!expected_bytes) return 1;
    unsigned checks = 0, faults = 0, retries = 0;
    int failures = 0;
    for (unsigned profile = 0; profile < 3; profile++)
    for (unsigned mode = profile ? 1u : 0u; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < (mode ? 2u : 1u); stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned instruction = 0; instruction < 34; instruction++)
    for (unsigned region = 0; region < 2; region++)
    for (unsigned scenario = 0; scenario < (profile == 2 ? region ? 13u : 7u : 1u); scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        bool paged = profile == 2, locked = profile == 1;
        uint32_t origin = !paged && region && stack32 ? 0x12340000u : 0;
        uint32_t base = paged ? region ? 0x403FF000u : 0x4000u : 0x20000u;
        if (mode) {
            dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
            dpmi_desc_set_base(&vm->dpmi.ldt[2], base - origin);
            dpmi_desc_set_limit(&vm->dpmi.ldt[2], stack32 ? UINT32_MAX : 0xFFFFu);
        } else cpu->ss = base >> 4;
        unsigned nesting = 5, alloc = 17;
        uint32_t sp = instruction == 30 || instruction == 32 ? 4096u + 3u * width
                    : instruction == 31 ? 4096u - 3u * width
                    : (instruction < 8 || (instruction >= 16 && instruction != 19 &&
                       instruction != 22 && instruction != 23 && instruction != 33)) ? 4095u + width : 4095u;
        cpu->esp = (stack32 ? origin : 0xABCD0000u) | sp;
        cpu->ebp = (stack32 ? origin : 0x56780000u) | (instruction == 33 ? 4095u : 0x1180u);
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF | FLAG_AF | FLAG_OF;
        vm->dpmi.virtual_interrupts_enabled = true;
        dos_test_page_pair_t pair = {0};
        uint32_t entry = 0, saved_entry = 0;
        if (paged) {
            pair = dos_test_page_pair(vm, base);
            if (scenario) {
                unsigned page = (scenario - 1u) % 2u;
                entry = scenario < 7 ? pair.pte[page] : pair.pde[page];
                saved_entry = dos_mem_read32(vm, entry);
                unsigned permission = ((scenario - 1u) / 2u) % 3u;
                dos_mem_write32(vm, entry, saved_entry & ~(1u << permission));
            }
        }
        for (unsigned i = 0; i < 8192; i++) {
            expected_bytes[i] = (uint8_t)(i * 13u + 0x5A);
            vm->mem[paged ? dos_string_test_physical(i) : base + i] = expected_bytes[i];
            if (paged && !region) vm->mem[base + i] = 0x3C;
        }
        dos_mem_write32(vm, 0x34800, 0x1234ABCDu);
        dos_mem_write32(vm, 0x34810, 0x6C6C6C6Cu);
        uint32_t next = dos_stack_test_code(vm, instruction, width, region != 0,
                                             locked, nesting, alloc);
        cpu8086_state_t original = *cpu, expected = original;
        uint32_t external = 0x6C6C6C6Cu;
        dos_stack_test_access_t accesses[64];
        unsigned count = dos_stack_test_model(&expected, expected_bytes, origin, instruction,
                                               width, stack32, nesting, alloc, accesses, &external);
        bool failed = locked;
        uint32_t fault_address = 0;
        unsigned error = 0;
        if (paged && scenario) for (unsigned a = 0; a < count && !failed; a++) {
            unsigned page = (scenario - 1u) % 2u;
            unsigned permission = ((scenario - 1u) / 2u) % 3u;
            for (unsigned b = 0; b < accesses[a].width; b++) {
                uint32_t offset = accesses[a].offset + b;
                if (offset / 4096u == page && (permission != 1 || accesses[a].write)) {
                    failed = true;
                    fault_address = base + offset;
                    error = 4u | (accesses[a].write ? 2u : 0) | (permission ? 1u : 0);
                    break;
                }
            }
        }
        bool ok = dos_test_run_one(vm);
        if (failed) {
            if (!dos_test_memory_fault(vm, &original, locked ? 6u : 14u, error) ||
                cpu->cr2 != (locked ? original.cr2 : fault_address)) ok = false;
            uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
            uint32_t saved_flags = mode == 2 ? dos_mem_read32(vm, frame + 20u)
                                               : dos_mem_read16(vm, frame + 10u);
            if (saved_flags != (mode == 2 ? original.eflags : (uint16_t)original.eflags)) ok = false;
            faults++;
        } else if (!dos_stack_test_result(vm, &expected, next, external, width)) ok = false;
        for (unsigned i = 0; i < 8192; i++)
            if (vm->mem[paged ? dos_string_test_physical(i) : base + i] !=
                (failed ? (uint8_t)(i * 13u + 0x5A) : expected_bytes[i]) ||
                (paged && !region && vm->mem[base + i] != 0x3C)) ok = false;
        if (failed && !locked) {
            for (unsigned i = 0; i < 4; i++)
                if (vm->mem[0x34810 + i] != 0x6C) ok = false;
            if (scenario <= 2) {
                dos_mem_write32(vm, entry, saved_entry);
                vm->mem[0x60E0] = 0xCB;
                vm->step_limit = vm->step_count + 16u;
                bool returned = dos_test_run_until(vm, true, original.cs, original.eip);
                vm->step_limit = 0;
                expected.cr2 = fault_address;
                if (!returned || !dos_test_run_one(vm) ||
                    !dos_stack_test_result(vm, &expected, next, external, width)) ok = false;
                for (unsigned i = 0; i < 8192; i++)
                    if (vm->mem[dos_string_test_physical(i)] != expected_bytes[i]) ok = false;
                retries++;
            }
        }
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-STACK-MEM] case="); serial_putdec(checks);
                serial_puts(" instruction="); serial_putdec(instruction);
                serial_puts(" profile="); serial_putdec(profile);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" esp="); serial_puthex(cpu->esp, 8);
                serial_puts(" expected="); serial_puthex(expected.esp, 8);
                serial_puts(" cr2="); serial_puthex(cpu->cr2, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    dos_host_free_pages(expected_bytes, 2);
    serial_puts("[DOS-STACK-MEM] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_stack_segment_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const unsigned instructions[] = { 0, 4, 8, 12, 18, 19, 20, 21, 22, 23, 26, 30, 31, 32, 33 };
    uint8_t *bytes = dos_host_alloc_pages(2);
    if (!bytes) return 1;
    unsigned checks = 0, faults = 0, retries = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned n = 0; n < sizeof(instructions) / sizeof(instructions[0]); n++)
    for (unsigned scenario = 0; scenario < 12; scenario++) {
        unsigned instruction = instructions[n];
        dos_fetch_test_prepare(vm, cpu, mode);
        dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
        dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000);
        cpu->esp = (stack32 ? 0 : 0xABCD0000u) | 0x1100u;
        cpu->ebp = (stack32 ? 0 : 0x56780000u) | 0x1180u;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF | FLAG_AF | FLAG_OF;
        vm->dpmi.virtual_interrupts_enabled = true;
        if (scenario == 11) cpu->ss &= (uint16_t)~1u;
        for (unsigned i = 0; i < 8192; i++) bytes[i] = vm->mem[0x20000 + i] = (uint8_t)(i * 13u + 0x5A);
        dos_mem_write32(vm, 0x34800, 0x1234ABCDu);
        dos_mem_write32(vm, 0x34810, 0x6C6C6C6Cu);
        uint32_t next = dos_stack_test_code(vm, instruction, width, true, false, 5, 17);
        cpu8086_state_t original = *cpu, expected = original;
        dos_stack_test_access_t accesses[64];
        uint32_t external = 0x6C6C6C6Cu, first = UINT32_MAX, last = 0;
        unsigned count = dos_stack_test_model(&expected, bytes, 0, instruction, width,
                                               stack32, 5, 17, accesses, &external);
        for (unsigned i = 0; i < count; i++) {
            if (accesses[i].offset < first) first = accesses[i].offset;
            uint32_t end = accesses[i].offset + accesses[i].width - 1u;
            if (end > last) last = end;
        }
        uint32_t limit = scenario == 1 ? last - 1u : scenario == 2 ? first
                       : scenario == 3 || scenario == 4 ? first - 1u
                       : scenario == 5 ? first : last;
        dpmi_descriptor_t good = vm->dpmi.ldt[2];
        dpmi_desc_set_limit(&vm->dpmi.ldt[2], limit);
        bool expand = scenario >= 4 && scenario <= 6;
        if (expand) vm->dpmi.ldt[2].access |= 4u;
        if (scenario == 7) vm->dpmi.ldt[2].access &= (uint8_t)~DESC_WRITABLE;
        if (scenario == 8) vm->dpmi.ldt[2].access |= DESC_CODE;
        if (scenario == 9) vm->dpmi.ldt[2].access &= (uint8_t)~DESC_PRESENT;
        if (scenario == 10) vm->dpmi.ldt[2].access &= (uint8_t)~0x20u;
        bool failed = scenario >= 7;
        for (unsigned i = 0; i < count; i++)
            if (expand ? accesses[i].offset <= limit
                       : accesses[i].offset + accesses[i].width - 1u > limit) failed = true;
        bool ok = dos_test_run_one(vm);
        if (failed) {
            if (!dos_test_memory_fault(vm, &original, 12, 0) || cpu->cr2 != original.cr2) ok = false;
            faults++;
        } else if (!dos_stack_test_result(vm, &expected, next, external, width)) ok = false;
        for (unsigned i = 0; i < 8192; i++)
            if (vm->mem[0x20000 + i] != (failed ? (uint8_t)(i * 13u + 0x5A) : bytes[i])) ok = false;
        if (failed && (scenario == 1 || scenario == 5)) {
            vm->dpmi.ldt[2] = good;
            vm->mem[0x60C0] = 0xCB;
            vm->step_limit = vm->step_count + 16u;
            bool returned = dos_test_run_until(vm, true, original.cs, original.eip);
            vm->step_limit = 0;
            if (!returned || !dos_test_run_one(vm) ||
                !dos_stack_test_result(vm, &expected, next, external, width)) ok = false;
            for (unsigned i = 0; i < 8192; i++) if (vm->mem[0x20000 + i] != bytes[i]) ok = false;
            retries++;
        }
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-STACK-SEG] case="); serial_putdec(checks);
                serial_puts(" instruction="); serial_putdec(instruction);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    dos_host_free_pages(bytes, 2);
    serial_puts("[DOS-STACK-SEG] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_enter_stack_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const unsigned allocations[] = { 0, 17, 0xFFFF };
    uint8_t *bytes = dos_host_alloc_pages(2);
    if (!bytes) return 1;
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 0; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < (mode ? 2u : 1u); stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned level = 0; level < 35; level++)
    for (unsigned overlap = 0; overlap < 3; overlap++)
    for (unsigned a = 0; a < sizeof(allocations) / sizeof(allocations[0]); a++) {
        unsigned nesting = level < 32 ? level : level == 32 ? 32u : level == 33 ? 33u : 255u;
        dos_fetch_test_prepare(vm, cpu, mode);
        if (mode) {
            dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
            dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000);
            dpmi_desc_set_limit(&vm->dpmi.ldt[2], stack32 ? UINT32_MAX : 0xFFFFu);
        } else cpu->ss = 0x2000;
        cpu->esp = (stack32 ? 0 : 0xABCD0000u) | 0x1100u;
        cpu->ebp = (stack32 ? 0 : 0x56780000u) |
            (overlap == 0 ? 0x1300u : overlap == 1 ? 0x1100u : 0x1100u - width * 3u);
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF | FLAG_AF | FLAG_OF;
        vm->dpmi.virtual_interrupts_enabled = true;
        for (unsigned i = 0; i < 8192; i++) bytes[i] = vm->mem[0x20000 + i] = (uint8_t)(i * 13u + 0x5A);
        dos_mem_write32(vm, 0x34800, 0x1234ABCDu);
        dos_mem_write32(vm, 0x34810, 0x6C6C6C6Cu);
        uint32_t next = dos_stack_test_code(vm, 32, width, overlap == 1, false, nesting, allocations[a]);
        cpu8086_state_t expected = *cpu;
        uint32_t external = 0x6C6C6C6Cu;
        dos_stack_test_access_t accesses[64];
        (void)dos_stack_test_model(&expected, bytes, 0, 32, width, stack32,
                                    nesting, allocations[a], accesses, &external);
        bool ok = dos_test_run_one(vm) && dos_stack_test_result(vm, &expected, next, external, width);
        for (unsigned i = 0; i < 8192; i++) if (vm->mem[0x20000 + i] != bytes[i]) ok = false;
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-STACK-ENTER] case="); serial_putdec(checks);
                serial_puts(" level="); serial_putdec(nesting);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" esp="); serial_puthex(cpu->esp, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    dos_host_free_pages(bytes, 2);
    serial_puts("[DOS-STACK-ENTER] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_enter_paging_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    uint8_t *bytes = dos_host_alloc_pages(2);
    if (!bytes) return 1;
    unsigned checks = 0, faults = 0, retries = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned layout = 0; layout < (stack32 ? 3u : 4u); layout++)
    for (unsigned scenario = 0; scenario < 7; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
        const uint32_t base = 0x403FF000u;
        dpmi_desc_set_base(&vm->dpmi.ldt[2], base);
        dos_test_page_pair_t pair = dos_test_page_pair(vm, base);
        unsigned bad_page = layout >= 2 ? 1u : 0u;
        uint32_t entry = scenario < 4 ? pair.pte[bad_page] : pair.pde[bad_page];
        uint32_t saved_entry = dos_mem_read32(vm, entry);
        unsigned permission = scenario ? (scenario - 1u) % 3u : 0;
        if (scenario) dos_mem_write32(vm, entry, saved_entry & ~(1u << permission));
        unsigned nesting = layout == 0 || layout == 3 ? 3u : 8u;
        uint32_t sp = layout >= 2 ? 0x800u : 0x1800u;
        cpu->esp = (stack32 ? 0 : 0xABCD0000u) | sp;
        cpu->ebp = (stack32 ? 0 : 0x56780000u) |
            (layout == 0 ? 0x1700u : layout == 3 ? 0x500u : 4096u + 3u * width);
        unsigned alloc = layout == 0 || layout == 3 ? (uint16_t)(sp - 4u * width - 4095u) : 17u;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF | FLAG_AF | FLAG_OF;
        vm->dpmi.virtual_interrupts_enabled = true;
        for (unsigned i = 0; i < 8192; i++) bytes[i] = vm->mem[dos_string_test_physical(i)] = (uint8_t)(i * 13u + 0x5A);
        dos_mem_write32(vm, 0x34800, 0x1234ABCDu);
        dos_mem_write32(vm, 0x34810, 0x6C6C6C6Cu);
        uint32_t next = dos_stack_test_code(vm, 32, width, layout & 1u, false, nesting, alloc);
        cpu8086_state_t original = *cpu, expected = original;
        dos_stack_test_access_t accesses[64];
        uint32_t external = 0x6C6C6C6Cu;
        unsigned count = dos_stack_test_model(&expected, bytes, 0, 32, width,
                                               stack32, nesting, alloc, accesses, &external);
        bool failed = false;
        uint32_t fault_address = 0;
        unsigned error = 0;
        if (scenario) for (unsigned i = 0; i < count && !failed; i++)
        for (unsigned b = 0; b < accesses[i].width; b++) {
            uint32_t offset = accesses[i].offset + b;
            if (offset / 4096u == bad_page && (permission != 1 || accesses[i].write)) {
                failed = true;
                fault_address = base + offset;
                error = 4u | (accesses[i].write ? 2u : 0) | (permission ? 1u : 0);
                break;
            }
        }
        bool ok = dos_test_run_one(vm);
        if (failed) {
            if (!dos_test_memory_fault(vm, &original, 14, error) || cpu->cr2 != fault_address) ok = false;
            faults++;
        } else if (!dos_stack_test_result(vm, &expected, next, external, width)) ok = false;
        for (unsigned i = 0; i < 8192; i++)
            if (vm->mem[dos_string_test_physical(i)] != (failed ? (uint8_t)(i * 13u + 0x5A) : bytes[i])) ok = false;
        if (layout == 0 && !failed && (dos_mem_read32(vm, pair.pte[0]) & 0x60u) != 0x20u) ok = false;
        if (failed && scenario == 1) {
            dos_mem_write32(vm, entry, saved_entry);
            vm->mem[0x60E0] = 0xCB;
            vm->step_limit = vm->step_count + 16u;
            bool returned = dos_test_run_until(vm, true, original.cs, original.eip);
            vm->step_limit = 0;
            expected.cr2 = fault_address;
            if (!returned || !dos_test_run_one(vm) ||
                !dos_stack_test_result(vm, &expected, next, external, width)) ok = false;
            for (unsigned i = 0; i < 8192; i++)
                if (vm->mem[dos_string_test_physical(i)] != bytes[i]) ok = false;
            retries++;
        }
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-STACK-ENTER-PG] case="); serial_putdec(checks);
                serial_puts(" layout="); serial_putdec(layout);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" esp="); serial_puthex(cpu->esp, 8);
                serial_puts(" cr2="); serial_puthex(cpu->cr2, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    dos_host_free_pages(bytes, 2);
    serial_puts("[DOS-STACK-ENTER-PG] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_stack_vga_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    uint8_t old_mode = vm->vga_mode;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned instruction = 0; instruction < 3; instruction++)
    for (unsigned failed = 0; failed < 2; failed++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
        vm->vga_mode = 0x13;
        dos_io_vga_set_mode(vm, 0x13, true);
        dos_io_write8(vm, 0x3C4, 4); dos_io_write8(vm, 0x3C5, 6);
        dos_io_vga_write_memory(vm, 0xA0010, 0x3C);
        for (unsigned i = 0; i < 96; i++) dos_io_vga_write_memory(vm, 0xA0100 + i, 0xA5);
        (void)dos_io_vga_read_memory(vm, 0xA0010);
        cpu->esp = (stack32 ? 0 : 0xABCD0000u) | (instruction == 1 ? 0x1100u : 0x800u);
        cpu->ebp = (stack32 ? 0 : 0x56780000u) | 0x1140u;
        dpmi_desc_set_base(&vm->dpmi.ldt[2], instruction ? 0x9F000u : 0x20000u);
        if (!instruction) dpmi_desc_set_base(&vm->dpmi.ldt[5], 0xA0100u - 0x4800u);
        if (failed) {
            if (instruction == 0) dpmi_desc_set_limit(&vm->dpmi.ldt[2], 0x800u - width);
            if (instruction == 1) dpmi_desc_set_limit(&vm->dpmi.ldt[2], 0x1100u + 7u * width - 1u);
            if (instruction == 2) {
                vm->dpmi.ldt[2].access |= 4u;
                dpmi_desc_set_limit(&vm->dpmi.ldt[2], 0x800u - 3u * width);
            }
        }
        uint32_t dest = instruction ? 0x9F000u : 0x20000u;
        for (unsigned i = 0; i < 64; i++) vm->mem[dest + 0x7E0u + i] = 0x6C;
        unsigned opcode = instruction == 0 ? 20u : instruction == 1 ? 31u : 32u;
        uint32_t next = dos_stack_test_code(vm, opcode, width, true, false, 5, 0);
        cpu8086_state_t original = *cpu;
        bool ok = dos_test_run_one(vm);
        if (failed) {
            if (!dos_test_memory_fault(vm, &original, 12, 0)) ok = false;
            for (unsigned i = 0; i < 64; i++) if (vm->mem[dest + 0x7E0u + i] != 0x6C) ok = false;
        } else if (cpu->eip != next || vm->dpmi.exception_depth) ok = false;
        dos_io_write8(vm, 0x3CE, 5); dos_io_write8(vm, 0x3CF, 0x41);
        dos_io_vga_write_memory(vm, 0xA0030, 0);
        for (unsigned plane = 0; plane < 4; plane++) {
            dos_io_write8(vm, 0x3CE, 4); dos_io_write8(vm, 0x3CF, plane);
            if (dos_io_vga_read_memory(vm, 0xA0030) != (failed ? 0x3C : 0xA5)) ok = false;
        }
        if (!ok) {
            serial_puts("[DOS-STACK-VGA] case="); serial_putdec(checks); serial_puts("\n");
            failures++;
        }
        checks++;
    }
    dos_io_vga_set_mode(vm, 0x13, true);
    vm->vga_mode = old_mode;
    serial_puts("[DOS-STACK-VGA] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_exception_esp_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned interpreted = 0; interpreted < 2; interpreted++)
    for (unsigned nested = 0; nested < 2; nested++)
    for (unsigned edited = 0; edited < 2; edited++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
        cpu->esp = 0xABCD9000u;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF;
        vm->dpmi.virtual_interrupts_enabled = true;
        vm->emulate_cpu = interpreted;
        cpu8086_state_t original = *cpu;
        unsigned width = mode == 2 ? 4u : 2u;
        bool ok = dpmi_deliver_exception(vm, 12, original.eip, 0);
        uint32_t outer_sp = cpu->esp;
        uint16_t outer_ss = cpu->ss;
        uint32_t outer_frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
        if (vm->dpmi.exception_esp_high[0] != 0xABCD0000u) ok = false;
        if (nested) {
            cpu8086_state_t handler = *cpu;
            if (!dpmi_deliver_exception(vm, 13, handler.eip, 0)) ok = false;
            uint32_t inner_sp = cpu->esp;
            cpu->cs = vm->dpmi.sel_host_code;
            cpu->eip = DPMI_EXCEPTION_RETURN_OFF + 2u;
            cpu->esp = inner_sp + width * 2u - (interpreted ? width * 3u : 0);
            if (!dpmi_exception_return_frame(vm, interpreted) || cpu->ss != outer_ss ||
                cpu->esp != outer_sp || vm->dpmi.exception_depth != 1 ||
                vm->dpmi.exception_esp_high[0] != 0xABCD0000u ||
                vm->dpmi.exception_esp_high[1]) ok = false;
        }
        uint32_t wanted = edited ? mode == 2 ? 0x7788A600u : 0xABCDA600u : original.esp;
        if (edited) {
            if (mode == 2) dos_mem_write32(vm, outer_frame + 6u * width, wanted);
            else dos_mem_write16(vm, outer_frame + 6u * width, (uint16_t)wanted);
        }
        cpu->ss = outer_ss;
        cpu->cs = vm->dpmi.sel_host_code;
        cpu->eip = DPMI_EXCEPTION_RETURN_OFF + 2u;
        cpu->esp = outer_sp + width * 2u - (interpreted ? width * 3u : 0);
        dos_mem_write16(vm, outer_frame + 7u * width, 0);
        if (dpmi_exception_return_frame(vm, interpreted) || vm->dpmi.exception_depth != 1 ||
            vm->dpmi.exception_esp_high[0] != 0xABCD0000u) ok = false;
        dos_mem_write16(vm, outer_frame + 7u * width, original.ss);
        if (!dpmi_exception_return_frame(vm, interpreted) || cpu->esp != wanted ||
            cpu->ss != original.ss || cpu->cs != original.cs || cpu->eip != original.eip ||
            vm->dpmi.exception_depth || vm->dpmi.exception_esp_high[0]) ok = false;
        if (!ok) {
            serial_puts("[DPMI-EX-ESP] case="); serial_putdec(checks);
            serial_puts(" esp="); serial_puthex(cpu->esp, 8); serial_puts("\n");
            failures++;
        }
        checks++;
    }
    serial_puts("[DPMI-EX-ESP] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static int dos_stack_wrap_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    uint8_t *bytes = dos_host_alloc_pages(32);
    if (!bytes) return 1;
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 0; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < (mode ? 2u : 1u); stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned instruction = 0; instruction < 34; instruction++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        uint32_t origin = stack32 ? 0xFFFF0000u : 0;
        uint32_t size = stack32 ? 0x20000u : 0x10000u;
        uint32_t mask = stack32 ? UINT32_MAX : 0xFFFFu;
        if (mode) {
            dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
            dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x40000u - origin);
            dpmi_desc_set_limit(&vm->dpmi.ldt[2], mask);
        } else cpu->ss = 0x4000;
        bool pop = (instruction >= 8 && instruction < 16) || instruction == 19 ||
                    instruction == 22 || instruction == 23 || instruction == 31 || instruction == 33;
        uint32_t sp = instruction == 30 || instruction == 32 ? 2u * width
                    : instruction == 31 ? (0u - 3u * width) & mask
                    : pop ? (0u - width) & mask : 0;
        cpu->esp = (stack32 ? 0 : 0xABCD0000u) | sp;
        cpu->ebp = instruction == 33 ? (stack32 ? 0u - width : 0x56780000u | (uint16_t)(0u - width))
                                    : (stack32 ? origin : 0x56780000u) | 0x1180u;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF | FLAG_AF | FLAG_OF;
        vm->dpmi.virtual_interrupts_enabled = true;
        for (unsigned i = 0; i < size; i++) bytes[i] = vm->mem[0x40000u + i] = (uint8_t)(i * 13u + 0x5A);
        dos_mem_write32(vm, 0x34800, 0x1234ABCDu);
        dos_mem_write32(vm, 0x34810, 0x6C6C6C6Cu);
        uint32_t next = dos_stack_test_code(vm, instruction, width, !stack32, false, 5, 17);
        cpu8086_state_t expected = *cpu;
        uint32_t external = 0x6C6C6C6Cu;
        dos_stack_test_access_t accesses[64];
        (void)dos_stack_test_model(&expected, bytes, origin, instruction, width,
                                    stack32, 5, 17, accesses, &external);
        bool ok = dos_test_run_one(vm) && dos_stack_test_result(vm, &expected, next, external, width);
        for (unsigned i = 0; i < size; i++) if (vm->mem[0x40000u + i] != bytes[i]) ok = false;
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-STACK-WRAP] case="); serial_putdec(checks);
                serial_puts(" instruction="); serial_putdec(instruction);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" esp="); serial_puthex(cpu->esp, 8);
                serial_puts(" expected="); serial_puthex(expected.esp, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    dos_host_free_pages(bytes, 32);
    serial_puts("[DOS-STACK-WRAP] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    return failures;
}

static uint16_t *dos_segment_test_sreg(cpu8086_state_t *cpu, unsigned index)
{
    uint16_t *registers[] = { &cpu->es, &cpu->cs, &cpu->ss,
                              &cpu->ds, &cpu->fs, &cpu->gs };
    return registers[index];
}

static uint32_t *dos_segment_test_gpr(cpu8086_state_t *cpu, unsigned index)
{
    uint32_t *registers[] = { &cpu->eax, &cpu->ecx, &cpu->edx, &cpu->ebx,
                              &cpu->esp, &cpu->ebp, &cpu->esi, &cpu->edi };
    return registers[index];
}

/* Forms: MOV to segment from register/memory, POP, far pointer,
 * then MOV from segment to register/memory. FS prefixes use the old FS. */
static uint32_t dos_segment_test_code(dos_vm_t *vm, unsigned form, unsigned segment,
                                       unsigned width, bool adr32, unsigned reg,
                                       uint32_t offset, bool locked)
{
    uint32_t p = 0x1000;
    if ((width == 4) != vm->cpu->op_size_32) vm->mem[p++] = 0x66;
    if (adr32 != vm->cpu->addr_size_32) vm->mem[p++] = 0x67;
    if (locked) vm->mem[p++] = 0xF0;
    vm->mem[p++] = 0x64;
    if (form == 2) {
        if (segment >= 4) {
            vm->mem[p++] = 0x0F;
            vm->mem[p++] = segment == 4 ? 0xA1 : 0xA9;
        } else vm->mem[p++] = 0x07u + segment * 8u;
        return p;
    }
    unsigned field = segment;
    if (form == 3) {
        if (segment == 0 || segment == 3) vm->mem[p++] = segment == 0 ? 0xC4 : 0xC5;
        else { vm->mem[p++] = 0x0F; vm->mem[p++] = 0xB0u + segment; }
        field = reg;
    } else vm->mem[p++] = form >= 4 ? 0x8C : 0x8E;
    bool memory = form == 1 || form == 3 || form == 5;
    vm->mem[p++] = field * 8u | (memory ? adr32 ? 5u : 6u : 0xC0u | reg);
    if (memory) for (unsigned b = 0; b < (adr32 ? 4u : 2u); b++)
        vm->mem[p++] = offset >> (8u * b);
    return p;
}

static bool dos_segment_test_result(dos_vm_t *vm, const cpu8086_state_t *expected,
                                     uint32_t next, unsigned shadow)
{
    cpu8086_state_t *cpu = vm->cpu;
    return dos_test_general_registers(cpu, expected) && cpu->esp == expected->esp &&
        cpu->cs == expected->cs && cpu->ss == expected->ss &&
        cpu->ds == expected->ds && cpu->es == expected->es &&
        cpu->fs == expected->fs && cpu->gs == expected->gs &&
        cpu->eflags == expected->eflags && cpu->cr2 == expected->cr2 &&
        cpu->eip == next && cpu->irq_shadow == shadow && !vm->dpmi.exception_depth;
}

static bool dos_segment_test_fault(dos_vm_t *vm, const cpu8086_state_t *original,
                                    unsigned vector, unsigned error)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (cpu->irq_shadow || cpu->ds != original->ds || cpu->es != original->es ||
        cpu->fs != original->fs || cpu->gs != original->gs) return false;
    if (original->protected_mode) return dos_test_memory_fault(vm, original, vector, error);
    uint32_t frame = ((uint32_t)original->ss << 4) + (uint16_t)(original->sp - 6u);
    return dos_test_general_registers(cpu, original) && cpu->ss == original->ss &&
        cpu->esp == ((original->esp & 0xFFFF0000u) | (uint16_t)(original->sp - 6u)) &&
        cpu->eip == 0x6000u + vector * 16u && cpu->cs == 0 &&
        dos_mem_read16(vm, frame) == original->ip &&
        dos_mem_read16(vm, frame + 2u) == original->cs &&
        dos_mem_read16(vm, frame + 4u) == original->flags;
}

static int dos_segment_load_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const unsigned segments[] = { 0, 2, 3, 4, 5 };
    unsigned checks = 0, faults = 0, retries = 0;
    int failures = 0;
    for (unsigned mode = 0; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < (mode ? 2u : 1u); stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned dest = 0; dest < 5; dest++)
    for (unsigned form = 0; form < 4; form++)
    for (unsigned scenario = 0; scenario < 18; scenario++)
    for (unsigned r = 0; r < (!scenario && !form ? 8u : 1u); r++) {
        unsigned segment = segments[dest], reg = !scenario && !form ? r : 3u;
        dos_fetch_test_prepare(vm, cpu, mode);
        if (mode) {
            cpu->eflags |= FLAG_IF | FLAG_IOPL_MASK;
            vm->dpmi.virtual_interrupts_enabled = true;
        }
        (void)dpmi_get_host_code_selector(vm);
        (void)dpmi_get_exception_stack_selector(vm);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(12), false, !stack32);
        dpmi_descriptor_t *target = &vm->dpmi.ldt[12];
        dpmi_desc_set_base(target, 0x50000);
        if (mode) {
            dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
            dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x40000);
        } else cpu->ss = 0x4000;
        cpu->esp = (stack32 ? 0 : 0xABCD0000u) | 0x1100u;
        uint16_t selector = mode ? dpmi_index_to_sel(12) : 0x5678;
        int vector = -1;
        unsigned error = 0;
        switch (scenario) {
        case 1: target->access = 0xF0; if (segment == 2) vector = 13; break;
        case 2: target->access = 0xFA; if (segment == 2) vector = 13; break;
        case 3: target->access = 0xF8; vector = 13; break;
        case 4: target->access = 0x82; vector = 13; break;
        case 5: target->access = 0x72; vector = segment == 2 ? 12 : 11; break;
        case 6: target->access = 0x78; vector = 13; break;
        case 7: case 8: selector = scenario == 7 ? 0 : 3; if (segment == 2) vector = 13; break;
        case 9: vm->dpmi.descriptor_state[12] = DPMI_DESC_FREE; vector = 13; break;
        case 10: target->access = 0x9E; if (segment == 2) vector = 13; break;
        case 11: target->access = 0x92; vector = 13; break;
        case 12: selector &= ~3u; if (segment == 2) vector = 13; break;
        case 13: case 14:
            selector = 0x43;
            cpu->gdtr.base = 0x18000;
            cpu->gdtr.limit = scenario == 13 ? 0x47 : 0x46;
            for (unsigned b = 0; b < 8; b++) vm->mem[0x18040 + b] = ((uint8_t *)target)[b];
            if (scenario == 14) vector = 13;
            break;
        case 16: target->access |= DESC_ACCESSED; break;
        }
        if (!mode) vector = -1;
        if (vector >= 0) error = selector & ~3u;
        if (scenario == 15) { vector = 6; error = 0; }
        if (scenario == 17 && form) {
            if (form == 2) {
                cpu->sp = 0xFFFF;
                vector = 12;
            } else {
                if (mode) vm->dpmi.ldt[5].limit_lo = 0x4800;
                vector = 13;
            }
            error = 0;
        }
        uint32_t offset = scenario == 17 && !mode ? 0xFFFFu : 0x4800u;
        if (!form) *dos_segment_test_gpr(cpu, reg) = 0xCAFE0000u | selector;
        uint32_t address = form == 2 ? 0x40000u + (cpu->esp & (stack32 ? UINT32_MAX : 0xFFFFu))
                                     : 0x30000u + offset;
        if (form) for (unsigned b = 0; b < 6; b++)
            vm->mem[address + b] = form == 3 && b < width ? (uint8_t)(0x76543210u >> (b * 8u))
                : b - (form == 3 ? width : 0u) < 2u
                ? (uint8_t)(selector >> ((b - (form == 3 ? width : 0u)) * 8u)) : 0x5A;
        uint32_t next = dos_segment_test_code(vm, form, segment, width, adr32, reg,
                                               offset, scenario == 15);
        cpu8086_state_t original = *cpu, expected = original;
        *dos_segment_test_sreg(&expected, segment) = selector;
        if (form == 2) expected.esp = stack32 ? original.esp + width
            : (original.esp & 0xFFFF0000u) | (uint16_t)(original.sp + width);
        if (form == 3) expected.ebx = width == 4 ? 0x76543210u
            : (original.ebx & 0xFFFF0000u) | 0x3210u;
        unsigned shadow = segment == 2 && form != 3 ? 1u : 0;
        uint8_t access = scenario == 13 || scenario == 14 ? vm->mem[0x18045] : target->access;
        bool ok = dos_test_run_one(vm);
        if (vector >= 0) {
            if (!dos_segment_test_fault(vm, &original, vector, error)) ok = false;
            faults++;
        } else if (!dos_segment_test_result(vm, &expected, next, shadow)) ok = false;
        uint8_t after = scenario == 13 || scenario == 14 ? vm->mem[0x18045] : target->access;
        uint8_t wanted = mode && vector < 0 && (selector & ~3u) ? access | DESC_ACCESSED : access;
        if (after != wanted) ok = false;
        if (mode && scenario == 5) {
            target->access |= DESC_PRESENT;
            vm->mem[0x6000u + vector * 16u] = 0xCB;
            vm->step_limit = vm->step_count + 16u;
            bool returned = dos_test_run_until(vm, true, original.cs, original.eip);
            vm->step_limit = 0;
            if (!returned || !dos_test_run_one(vm) ||
                !dos_segment_test_result(vm, &expected, next, shadow)) ok = false;
            retries++;
        }
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-SEG-LOAD] case="); serial_putdec(checks);
                serial_puts(" mode="); serial_putdec(mode);
                serial_puts(" form="); serial_putdec(form);
                serial_puts(" segment="); serial_putdec(segment);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" esp="); serial_puthex(cpu->esp, 8);
                serial_puts(" flags="); serial_puthex(cpu->eflags, 8);
                serial_puts(" expected="); serial_puthex(expected.eflags, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-SEG-LOAD] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_segment_encoding_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned segment = 0; segment < 8; segment++)
    for (unsigned load = 0; load < 2; load++)
    for (unsigned memory = 0; memory < 2; memory++)
    for (unsigned locked = 0; locked < 2; locked++)
    for (unsigned reg = 0; reg < (memory ? 1u : 8u); reg++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(12), false, mode == 2);
        uint16_t value = load ? mode ? dpmi_index_to_sel(12) : 0x6789
                             : segment < 6 ? *dos_segment_test_sreg(cpu, segment) : 0;
        if (load && !memory) *dos_segment_test_gpr(cpu, reg) = 0xABCD0000u | value;
        for (unsigned i = 0; i < 6; i++) vm->mem[0x34800 + i] = 0x5A;
        if (load && memory) dos_mem_write16(vm, 0x34800, value);
        bool invalid = locked || segment >= 6 || (load && segment == 1);
        unsigned form = (load ? 0 : 4) + memory;
        /* Invalid encodings win over a data operand beyond its segment. */
        uint32_t next = dos_segment_test_code(vm, form, segment, width, adr32, reg,
                                               invalid ? 0xFFFFu : 0x4800u, locked);
        cpu8086_state_t original = *cpu, expected = original;
        if (!invalid) {
            if (load) *dos_segment_test_sreg(&expected, segment) = value;
            else if (!memory) {
                uint32_t *dst = dos_segment_test_gpr(&expected, reg);
                *dst = width == 4 ? value : (*dst & 0xFFFF0000u) | value;
            }
        }
        bool ok = dos_test_run_one(vm);
        if (invalid ? !dos_segment_test_fault(vm, &original, 6, 0)
                    : !dos_segment_test_result(vm, &expected, next, load && segment == 2)) ok = false;
        for (unsigned i = 0; i < 6; i++) {
            uint8_t wanted = i < 2 && memory && (load || !invalid)
                           ? (uint8_t)(value >> (8u * i)) : 0x5A;
            if (vm->mem[0x34800 + i] != wanted) ok = false;
        }
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-SEG-ENC] case="); serial_putdec(checks);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-SEG-ENC] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_segment_privilege_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const unsigned segments[] = { 0, 2, 3, 4, 5 };
    unsigned checks = 0;
    int failures = 0;
    for (unsigned cpl = 0; cpl < 4; cpl++)
    for (unsigned dpl = 0; dpl < 4; dpl++)
    for (unsigned rpl = 0; rpl < 4; rpl++)
    for (unsigned type = 0; type < 32; type++)
    for (unsigned present = 0; present < 2; present++)
    for (unsigned dest = 0; dest < 5; dest++) {
        dos_fetch_test_prepare(vm, cpu, 2);
        unsigned segment = segments[dest];
        cpu->cs = (cpu->cs & ~3u) | cpl;
        cpu->ss = (cpu->ss & ~3u) | cpl;
        vm->dpmi.ldt[1].access = 0x9A | (cpl << 5);
        vm->dpmi.ldt[2].access = 0x92 | (cpl << 5);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(12), false, true);
        uint8_t access = (present ? 0x80u : 0) | (dpl << 5) | type;
        vm->dpmi.ldt[12].access = access;
        uint16_t selector = (12u << 3) | 4u | rpl;
        cpu->ebx = 0xABCD0000u | selector;
        unsigned kind = type & 15u;
        bool permitted = type >= 16 && (kind < 8 || kind == 10 || kind == 11 || kind >= 14);
        if (segment == 2) permitted = type >= 16 && (kind == 2 || kind == 3 || kind == 6 || kind == 7)
                                    && rpl == cpl && dpl == cpl;
        else if (kind < 14) permitted &= cpl <= dpl && rpl <= dpl;
        int vector = !permitted ? 13 : !present ? segment == 2 ? 12 : 11 : -1;
        uint32_t next = dos_segment_test_code(vm, 0, segment, 4, true, 3, 0, false);
        cpu8086_state_t original = *cpu, expected = original;
        *dos_segment_test_sreg(&expected, segment) = selector;
        bool ok = dos_test_run_one(vm);
        if (vector >= 0 ? !dos_segment_test_fault(vm, &original, vector, selector & ~3u)
                        : !dos_segment_test_result(vm, &expected, next, segment == 2)) ok = false;
        if (vm->dpmi.ldt[12].access != (vector >= 0 ? access : access | 1u)) ok = false;
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-SEG-RIGHTS] case="); serial_putdec(checks);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-SEG-RIGHTS] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_segment_paging_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const unsigned segments[] = { 0, 2, 3, 4, 5 };
    unsigned checks = 0, faults = 0, retries = 0;
    int failures = 0;
    const uint32_t base = 0x403FF000u;
    for (unsigned profile = 0; profile < 2; profile++)
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned dest = 0; dest < 5; dest++)
    for (unsigned form = 1; form < 4; form++)
    for (unsigned accessed = 0; accessed < 2; accessed++)
    for (unsigned scenario = 0; scenario < 13; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        cpu->eflags |= FLAG_IF | FLAG_IOPL_MASK;
        vm->dpmi.virtual_interrupts_enabled = true;
        unsigned segment = segments[dest];
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(12), false, !stack32);
        dpmi_descriptor_t target = vm->dpmi.ldt[12];
        target.access |= accessed;
        vm->dpmi.ldt[12] = target;
        dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
        uint32_t source_base = profile ? 0x30000u : base;
        dpmi_desc_set_base(&vm->dpmi.ldt[2], source_base);
        dpmi_desc_set_base(&vm->dpmi.ldt[5], source_base);
        cpu->esp = (stack32 ? 0 : 0xABCD0000u) | 0xFFFu;
        dos_test_page_pair_t pair = dos_test_page_pair(vm, base);
        uint16_t selector = dpmi_index_to_sel(12);
        if (profile) {
            selector = 0x0B;
            cpu->gdtr.base = base + 4084u;
            cpu->gdtr.limit = 15;
            for (unsigned b = 0; b < 8; b++)
                vm->mem[dos_string_test_physical(4092u + b)] = ((uint8_t *)&target)[b];
        }
        uint8_t source[6];
        for (unsigned b = 0; b < 6; b++) {
            source[b] = form == 3 && b < width ? (uint8_t)(0x76543210u >> (b * 8u))
                : b - (form == 3 ? width : 0u) < 2u
                ? (uint8_t)(selector >> ((b - (form == 3 ? width : 0u)) * 8u)) : 0x5A;
            vm->mem[profile ? source_base + 4095u + b : dos_string_test_physical(4095u + b)] = source[b];
        }
        unsigned page = scenario ? (scenario - 1u) % 2u : 0;
        unsigned permission = scenario ? ((scenario - 1u) / 2u) % 3u : 0;
        uint32_t entry = scenario < 7 ? pair.pte[page] : pair.pde[page];
        uint32_t saved = dos_mem_read32(vm, entry);
        if (scenario) dos_mem_write32(vm, entry, saved & ~(1u << permission));
        bool failed = scenario && (!permission || (!profile && permission == 2) ||
                                  (profile && permission == 1 && !accessed && page == 1));
        uint32_t linear = !profile ? base + (page ? 4096u : 4095u)
                        : permission == 1 ? base + 4097u : base + (page ? 4096u : 4092u);
        unsigned error = profile ? permission == 1 ? 3u : 0u : 4u | (permission ? 1u : 0);
        uint32_t next = dos_segment_test_code(vm, form, segment, width, true, 3, 4095u, false);
        cpu8086_state_t original = *cpu, expected = original;
        *dos_segment_test_sreg(&expected, segment) = selector;
        if (form == 2) expected.esp = stack32 ? original.esp + width
            : (original.esp & 0xFFFF0000u) | (uint16_t)(original.sp + width);
        if (form == 3) expected.ebx = width == 4 ? 0x76543210u
            : (original.ebx & 0xFFFF0000u) | 0x3210u;
        unsigned shadow = segment == 2 && form != 3 ? 1u : 0;
        bool ok = dos_test_run_one(vm);
        if (failed) {
            if (!dos_segment_test_fault(vm, &original, 14, error) || cpu->cr2 != linear) ok = false;
            faults++;
        } else if (!dos_segment_test_result(vm, &expected, next, shadow)) ok = false;
        uint8_t after = profile ? vm->mem[dos_string_test_physical(4097)] : vm->dpmi.ldt[12].access;
        if (after != (failed ? target.access : target.access | 1u)) ok = false;
        for (unsigned b = 0; b < 6; b++)
            if (vm->mem[profile ? source_base + 4095u + b : dos_string_test_physical(4095u + b)] != source[b]) ok = false;
        if (failed) {
            dos_mem_write32(vm, entry, saved);
            vm->mem[0x60E0] = 0xCB;
            vm->step_limit = vm->step_count + 16u;
            bool returned = dos_test_run_until(vm, true, original.cs, original.eip);
            vm->step_limit = 0;
            expected.cr2 = linear;
            if (!returned || !dos_test_run_one(vm) ||
                !dos_segment_test_result(vm, &expected, next, shadow)) ok = false;
            retries++;
        }
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-SEG-PAGING] case="); serial_putdec(checks);
                serial_puts(" profile="); serial_putdec(profile);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" cr2="); serial_puthex(cpu->cr2, 8);
                serial_puts(" expected="); serial_puthex(linear, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-SEG-PAGING] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_segment_irq_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned start = 0; start < 6; start++)
    for (unsigned following = 0; following < (start < 3 ? 9u : 1u); following++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(12), false, mode == 2);
        uint16_t selector = mode ? dpmi_index_to_sel(12) : 0;
        cpu->eax = selector;
        cpu->eflags |= FLAG_IF;
        vm->dpmi.virtual_interrupts_enabled = true;
        if (start == 2) {
            cpu->eflags &= ~FLAG_IF;
            vm->dpmi.virtual_interrupts_enabled = false;
        }
        uint32_t next;
        if (start == 2 || start == 3) { vm->mem[0x1000] = 0xFB; next = 0x1001; }
        else {
            unsigned form = start == 1 ? 2u : start == 5 ? 3u : 0u;
            if (form == 2) dos_mem_write32(vm, 0x9000, 0xABCD0000u | selector);
            if (form == 3) {
                dos_mem_write32(vm, 0x34800, 0x8800);
                dos_mem_write16(vm, 0x34800 + width, selector);
            }
            next = dos_segment_test_code(vm, form, start == 4 ? 3u : 2u, width,
                                            false, form == 3 ? 4u : 0u, 0x4800, false);
        }
        uint32_t p = next;
        if (following == 0) {
            if ((width == 4) != cpu->op_size_32) vm->mem[p++] = 0x66;
            vm->mem[p++] = 0xBC;
            for (unsigned b = 0; b < width; b++) vm->mem[p++] = 0x8700u >> (b * 8u);
        } else if (following == 1) { vm->mem[p++] = 0x8E; vm->mem[p++] = 0xD0; }
        else if (following == 2) vm->mem[p++] = 0xFA;
        else if (following == 3) vm->mem[p++] = 0xF4;
        else if (following == 4 || following == 5) {
            vm->mem[p++] = 0xF3;
            vm->mem[p++] = following == 4 ? 0xAA : 0x6C;
            cpu->ecx = 257; cpu->edi = 0x4000; cpu->dx = 0x3C9;
        } else if (following == 6) { vm->mem[p++] = 0x0F; vm->mem[p++] = 0x0B; }
        else if (following == 7) { vm->mem[p++] = 0xCD; vm->mem[p++] = 0x60; }
        else {
            vm->mem[p++] = 0xF0; vm->mem[p++] = 0xFB; /* LOCK STI is #UD. */
        }
        dos_io_write8(vm, 0x20, 0x11);
        dos_io_write8(vm, 0x21, 8);
        dos_io_write8(vm, 0x21, 4);
        dos_io_write8(vm, 0x21, 1);
        dos_io_write8(vm, 0x21, 0xFE);
        dos_mem_write32(vm, 8u * 4u, 0x7000);
        dos_mem_write32(vm, 0x60u * 4u, 0x7100);
        vm->dpmi.pm_vectors[8].sel = dpmi_index_to_sel(7);
        vm->dpmi.pm_vectors[8].off = 0x7000;
        vm->dpmi.pm_vectors[0x60].sel = dpmi_index_to_sel(7);
        vm->dpmi.pm_vectors[0x60].off = 0x7100;
        vm->timer_irq_pending = true;
        bool ok = dos_test_run_one(vm);
        bool shadow = start < 3;
        if (cpu->irq_shadow != (unsigned)shadow || cpu->eip != next) ok = false;
        if (shadow) {
            if (cpu_deliver_hw_interrupt(vm, 8) || cpu8086_service_interrupts(vm) ||
                !vm->timer_irq_pending || cpu->eip != next) ok = false;
            dos_io_write8(vm, 0x20, 0x0B);
            if (dos_io_read8(vm, 0x20)) ok = false;
            if (!dos_test_run_one(vm) || cpu->irq_shadow) ok = false;
            if (following == 6 || following == 8) {
                if (cpu->eip != 0x6060) ok = false;
            } else if (following == 7) {
                if (cpu->eip != 0x7100) ok = false;
            } else if (following == 2) {
                if (cpu8086_service_interrupts(vm) || !vm->timer_irq_pending) ok = false;
            } else {
                if ((following == 4 || following == 5) &&
                    (cpu->ecx != 256 || cpu->edi != 0x4001 || cpu->eip != next)) ok = false;
                if (following == 0 && cpu->esp != 0x8700) ok = false;
                if (!cpu8086_service_interrupts(vm) || vm->timer_irq_pending ||
                    cpu->halted || cpu->eip != 0x7000) ok = false;
            }
        } else if (!cpu8086_service_interrupts(vm) || cpu->eip != 0x7000 || vm->timer_irq_pending) ok = false;
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-SEG-IRQ] case="); serial_putdec(checks);
                serial_puts(" start="); serial_putdec(start);
                serial_puts(" following="); serial_putdec(following);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" shadow="); serial_putdec(cpu->irq_shadow); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }

    for (unsigned mode = 0; mode < 3; mode++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(12), false, mode == 2);
        cpu->eax = mode ? dpmi_index_to_sel(12) : 0;
        cpu->eflags |= FLAG_IF;
        vm->dpmi.virtual_interrupts_enabled = true;
        uint32_t next = dos_segment_test_code(vm, 0, 2, 2, false, 0, 0, false);
        if (mode == 2) vm->mem[next++] = 0x66;
        vm->mem[next++] = 0xBC; vm->mem[next++] = 0; vm->mem[next++] = 0x87;
        vm->mem[next] = 0xF4;
        dos_io_write8(vm, 0x20, 0x20);
        dos_io_write8(vm, 0x21, 0xFE);
        dos_mem_write32(vm, 8u * 4u, 0x7000);
        vm->dpmi.pm_vectors[8].sel = cpu->cs;
        vm->dpmi.pm_vectors[8].off = 0x7000;
        vm->timer_irq_pending = true;
        vm->step_limit = 10;
        if (!dos_test_run_until(vm, mode != 0, cpu->cs, 0x7000) ||
            cpu->insn_count != 2 || cpu->halted || vm->timer_irq_pending || cpu->irq_shadow) {
            serial_puts("[DOS-SEG-IRQ] delayed boundary mismatch\n"); failures++;
        }
        checks++;
    }

    uint64_t pages = (sizeof(jit_state_t) + 4095u) / 4096u;
    jit_state_t *jit = dos_host_alloc_pages(pages);
    if (!jit) return failures + 1;
    jit_init(jit);
    for (unsigned mode = 0; mode < 3; mode++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        cpu->eax = cpu->ss;
        cpu->ebx = 0;
        uint32_t next = dos_segment_test_code(vm, 0, 2, 2, false, 0, 0, false);
        vm->mem[next] = vm->mem[next + 1u] = 0x43;
        /* A prefixed jump terminates translation after the two INCs. The
         * stop lies beyond that block so it cannot itself disable the JIT. */
        vm->mem[next + 2u] = 0x2E;
        vm->mem[next + 3u] = 0xEB;
        vm->mem[next + 4u] = 0;
        bool ok = dos_test_run_one(vm);
        vm->jit = jit;
        jit_block_t *block = jit_get_block(jit, cpu->cs, next);
        dos_test_jit_decode(vm, block);
        jit_compile_block(jit, block);
        jit->hit_count[(uint16_t)(next ^ (next >> 16))] = JIT_HOT_THRESHOLD;
        uint64_t before = jit->jit_executed;
        uint64_t instructions = cpu->insn_count;
        /* run_one always disables JIT, independently of the IRQ shadow. */
        if (!block->compiled || block->instruction_count != 2 ||
            !dos_test_run_until(vm, mode != 0, cpu->cs, next + 5u) || cpu->ebx != 2 ||
            cpu->insn_count != instructions + 3u || cpu->irq_shadow ||
            jit->jit_executed != before || block->exec_count) ok = false;
        cpu->eip = next;
        instructions = cpu->insn_count;
        if (!dos_test_run_until(vm, mode != 0, cpu->cs, next + 5u) || cpu->ebx != 4 ||
            cpu->insn_count != instructions + 3u || cpu->irq_shadow ||
            jit->jit_executed != before + 1u || block->exec_count != 1) ok = false;
        if (!ok) {
            serial_puts("[DOS-SEG-IRQ] JIT boundary mismatch mode="); serial_putdec(mode);
            serial_puts(" ip="); serial_puthex(cpu->eip, 8);
            serial_puts(" bx="); serial_puthex(cpu->ebx, 8);
            serial_puts(" blocks="); serial_putdec(jit->jit_executed - before);
            serial_puts("\n"); failures++;
        }
        checks++;
        vm->jit = NULL;
    }
    jit_destroy(jit);
    dos_host_free_pages(jit, pages);

    cpu8086_state_t saved_native = g_native_cpu;
    const uint8_t native_irqs[] = { 0, 1, 5 };
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned line = 0; line < sizeof(native_irqs); line++)
    for (unsigned shadow = 0; shadow <= 2; shadow++)
    for (unsigned enabled = 0; enabled < 2; enabled++)
    for (unsigned masked = 0; masked < 2; masked++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        uint8_t irq = native_irqs[line];
        dos_io_write8(vm, 0x20, 0x11);
        dos_io_write8(vm, 0x21, 8);
        dos_io_write8(vm, 0x21, 4);
        dos_io_write8(vm, 0x21, 1);
        dos_io_write8(vm, 0x21, masked ? 0xFF : (uint8_t)~(1u << irq));
        vm->dpmi.virtual_interrupts_enabled = enabled != 0;
        vm->dpmi.pm_vectors[8u + irq].sel = dpmi_index_to_sel(7);
        vm->dpmi.pm_vectors[8u + irq].off = 0x7000;
        x86_interrupt_frame_t frame = {
            .rax = 0x13579BDF, .rbx = 0x2468ACE0, .rcx = 0x12345678,
            .rdx = 0xABCDEF01, .rsi = 0x5000, .rdi = 0x5100, .rbp = 0x8000,
            .rip = cpu->eip, .cs = cpu->cs, .rsp = cpu->esp, .ss = cpu->ss,
            .rflags = cpu->eflags | FLAG_IF
        };
        x86_interrupt_frame_t expected = frame;
        g_native_cpu = *cpu;
        g_native_cpu.irq_shadow = shadow;
        g_native_cpu.rep_compare.active = true;
        bool delivered = dos_native_deliver_irq(vm, &frame, irq);
        bool wanted = enabled && !masked;
        bool ok = delivered == wanted;
        dos_io_write8(vm, 0x20, 0x0B);
        if (dos_io_read8(vm, 0x20) != (wanted ? 1u << irq : 0)) ok = false;
        if (enabled && (g_native_cpu.irq_shadow || g_native_cpu.rep_compare.active)) ok = false;
        if (wanted) {
            expected.rip = 0x7000;
            expected.cs = dpmi_index_to_sel(7);
            expected.rflags &= ~(uint64_t)(FLAG_IOPL_MASK | FLAG_TF);
            expected.rsp = g_native_cpu.esp;
            expected.ss = g_native_cpu.ss;
            if (vm->dpmi.virtual_interrupts_enabled ||
                g_native_cpu.cs != expected.cs || g_native_cpu.eip != expected.rip ||
                (frame.ss == cpu->ss && frame.rsp == cpu->esp)) ok = false;
        }
        if (memcmp(&frame, &expected, sizeof(frame))) ok = false;
        if (!ok) {
            serial_puts("[DOS-SEG-IRQ] native boundary mismatch mode="); serial_putdec(mode);
            serial_puts(" irq="); serial_putdec(irq);
            serial_puts(" shadow="); serial_putdec(shadow);
            serial_puts(" enabled="); serial_putdec(enabled);
            serial_puts(" masked="); serial_putdec(masked);
            serial_puts("\n"); failures++;
        }
        vm->cpu = cpu;
        checks++;
    }
    g_native_cpu = saved_native;
    dos_io_write8(vm, 0x20, 0x20);
    serial_puts("[DOS-SEG-IRQ] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_native_segments_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    bool emulate = vm->emulate_cpu;
    uint64_t step_limit = vm->step_limit, step_count = vm->step_count;
    bool limit_reached = vm->step_limit_reached, terminated = vm->process_terminated;
    unsigned dispatch_depth = vm->native_dispatch_depth;
    bool timer_pending = vm->timer_irq_pending;
    uint8_t pic_mask = dos_io_read8(vm, 0x21);
    unsigned checks = 0, recoveries = 0;
    int failures = 0;
    const uint16_t candidate = dpmi_index_to_sel(12);
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned segment = 0; segment < 6; segment++)
    for (unsigned policy = 0; policy < 18; policy++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        vm->dpmi.ldt[2].flags_lim = stack32 ? DESC_32BIT : 0;
        dos_test_import_cs(vm);
        dos_test_descriptor(&vm->dpmi, candidate, segment == 1,
                            segment == 2 ? stack32 : mode == 2);
        dpmi_descriptor_t *descriptor = &vm->dpmi.ldt[12];
        uint16_t selector = candidate;
        unsigned vector = 0;
        uint32_t error = selector & ~3u;
        switch (policy) {
        case 1: descriptor->access &= ~DESC_PRESENT; vector = segment == 2 ? 12 : 11; break;
        case 2: descriptor->access &= ~DESC_SEGMENT; vector = 13; break;
        case 3: descriptor->access = 0xF0; vector = segment == 1 || segment == 2 ? 13 : 0; break;
        case 4: descriptor->access = 0xFA; vector = segment == 2 ? 13 : 0; break;
        case 5: descriptor->access = 0xF8; vector = segment == 1 ? 0 : 13; break;
        case 6: descriptor->access &= ~DESC_DPL_MASK; vector = 13; break;
        case 7: descriptor->access = 0x9E; vector = segment == 2 ? 13 : 0; break;
        case 8: case 9:
            selector = policy == 8 ? 0 : 3;
            error = 0; vector = segment == 1 || segment == 2 ? 13 : 0; break;
        case 10: vm->dpmi.descriptor_state[12] = DPMI_DESC_FREE; vector = 13; break;
        case 11: selector &= ~3u; vector = segment == 1 || segment == 2 ? 13 : 0; break;
        case 12:
            dpmi_desc_set_limit(descriptor, 0xFFF);
            if (segment == 1) { vector = 13; error = 0; }
            break;
        case 13: if (segment == 1) { descriptor->flags_lim |= 0x20; vector = 13; } break;
        case 14: descriptor->access = 0xF6; vector = segment == 1 ? 13 : 0; break;
        case 15:
            descriptor->access = segment == 1 ? 0x72 : 0x78;
            vector = 13; break; /* Type/privilege errors precede presence. */
        case 16: dpmi_desc_set_base(descriptor, 0x50000); dpmi_desc_set_limit(descriptor, 0xFFFFF); break;
        case 17: descriptor->access |= 1u; break;
        default: break;
        }
        *dos_segment_test_sreg(cpu, segment) = selector;
        cpu->esp = stack32 ? 0xFFFFFFFFu : 0xBEEF9000u;
        cpu->eflags |= FLAG_DF;
        cpu8086_state_t before = *cpu;
        cpu_system_segment_t loaded[6];
        cpu_event_fault_t fault;
        bool valid = cpu8086_probe_native_segments(cpu, loaded, &fault);
        bool ok = valid == !vector && !cpu->delivery_fault &&
            !memcmp(cpu, &before, sizeof(before));
        if (vector) ok &= fault.raised && fault.vector == vector &&
            fault.error == error && fault.return_eip == before.eip && fault.has_error;
        else {
            ok &= !fault.raised && dos_native_prepare_return(vm) && cpu->running &&
                dos_test_general_registers(cpu, &before) && cpu->esp == before.esp &&
                cpu->eip == before.eip && cpu->eflags == before.eflags;
            for (unsigned s = 0; s < 6; s++) {
                ok &= *dos_segment_test_sreg(cpu, s) == *dos_segment_test_sreg(&before, s) &&
                    cpu->segment_cache[s].valid == loaded[s].valid;
                if (loaded[s].valid) ok &= !memcmp(&cpu->segment_cache[s].descriptor,
                    &loaded[s].descriptor, sizeof(dpmi_descriptor_t));
            }
        }
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-NATIVE-SEG] probe mode="); serial_putdec(mode);
                serial_puts(" stack="); serial_putdec(stack32);
                serial_puts(" segment="); serial_putdec(segment);
                serial_puts(" policy="); serial_putdec(policy); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }

    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned segment = 0; segment < 6; segment++)
    for (unsigned wp = 0; wp < 2; wp++)
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        const uint32_t linear = 0x403FFFFCu;
        dos_test_page_pair_t pair = dos_test_page_pair(vm, linear);
        if (!wp) cpu->cr0 &= ~DOS_CR0_WP;
        cpu->gdtr.base = linear - 16u;
        cpu->gdtr.limit = 0x17;
        dpmi_descriptor_t descriptor = { .limit_lo = 0xFFFF,
            .access = (segment == 1 ? 0xFA : 0xF2) | (scenario == 3),
            .flags_lim = mode == 2 ? DESC_32BIT : 0 };
        for (unsigned i = 0; i < 8; i++)
            vm->mem[i < 4 ? 0x20FFCu + i : 0x24000u + i - 4u] = ((uint8_t *)&descriptor)[i];
        dos_mem_write32(vm, pair.pte[0], 0x20003);
        dos_mem_write32(vm, pair.pte[1], scenario == 1 ? 0x24002 : scenario >= 2 ? 0x24001 : 0x24003);
        *dos_segment_test_sreg(cpu, segment) = 0x13;
        cpu8086_state_t before = *cpu;
        cpu_system_segment_t loaded[6];
        cpu_event_fault_t fault;
        bool faulted = scenario == 1 || (scenario == 2 && wp);
        bool ok = cpu8086_probe_native_segments(cpu, loaded, &fault) == !faulted;
        if (faulted) {
            before.cr2 = linear + (scenario == 1 ? 4u : 5u);
            ok &= fault.raised && fault.has_error && fault.vector == 14 &&
                fault.error == (scenario == 1 ? 0u : 3u) && fault.return_eip == before.eip;
        } else ok &= !fault.raised && vm->mem[0x24001] == (descriptor.access | 1u);
        ok &= !memcmp(cpu, &before, sizeof(before));
        if (!ok) {
            serial_puts("[DOS-NATIVE-SEG] paging case="); serial_putdec(checks);
            serial_puts("\n"); failures++;
        }
        checks++;
    }

    /* Real interpreted handlers repair each loaded descriptor. The caller's
     * instruction is deliberately not executable: admission must not run it. */
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned segment = 0; segment < 6; segment++)
    for (unsigned stop = 0; stop < 3; stop++)
    for (unsigned gate = 0; gate < 2; gate++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        /* Execute handlers in the interpreter, but retain native FLAGS. */
        vm->emulate_cpu = false;
        vm->dpmi.ldt[2].flags_lim = stack32 ? DESC_32BIT : 0;
        dos_test_import_cs(vm);
        uint16_t selector = *dos_segment_test_sreg(cpu, segment);
        dpmi_descriptor_t *descriptor = &vm->dpmi.ldt[dpmi_sel_to_index(selector)];
        uint16_t rights = descriptor->access | ((descriptor->flags_lim & 0xF0u) << 8);
        descriptor->access &= ~DESC_PRESENT;
        vm->mem[cpu->eip] = 0xF4;
        vm->step_limit = 100;
        cpu->esp = stack32 ? 0x9000u : 0xBEEF9000u;
        cpu->eflags |= FLAG_DF | FLAG_IF;
        cpu8086_state_t before = *cpu;
        unsigned vector = segment == 2 ? 12 : 11;
        uint32_t p = vm->dpmi.exception_vectors[vector].off;
        vm->mem[p++] = 0x60; /* PUSHA[D] */
        if (mode == 1) vm->mem[p++] = 0x66;
        vm->mem[p++] = 0x89; vm->mem[p++] = 0xE5; /* MOV EBP,ESP */
        if (mode == 1) vm->mem[p++] = 0x67;
        vm->mem[p++] = 0x81; vm->mem[p++] = 0x7D;
        vm->mem[p++] = 11u * (mode == 2 ? 4u : 2u);
        dos_mem_write16(vm, p, 0x1000); p += 2;
        if (mode == 2) { dos_mem_write16(vm, p, 0); p += 2; }
        vm->mem[p++] = 0x74; vm->mem[p++] = mode == 2 ? 6 : 5;
        if (mode == 2) vm->mem[p++] = 0x66;
        vm->mem[p++] = 0xB8; dos_mem_write16(vm, p, 0x4C7F); p += 2;
        vm->mem[p++] = 0xCD; vm->mem[p++] = 0x21;
        const uint8_t opcodes[] = { 0xB8, 0xBB, 0xB9 };
        const uint16_t operands[] = { stop ? 0x4C2A : 9, selector, rights };
        for (unsigned i = 0; i < 3; i++) {
            if (mode == 2) vm->mem[p++] = 0x66;
            vm->mem[p++] = opcodes[i];
            dos_mem_write16(vm, p, operands[i]); p += 2;
        }
        vm->mem[p++] = 0xCD; vm->mem[p++] = stop ? 0x21 : 0x31;
        vm->mem[p++] = 0x61; vm->mem[p++] = 0xCB;
        if (stop == 2) {
            vm->dpmi.exception_vectors[vector].sel = 0;
            vm->dpmi.exception_vectors[vector].off = 0;
        }
        if (gate) {
            vm->native_dispatch_depth = 0;
            vm->dpmi.virtual_interrupts_enabled = true;
            vm->dpmi.pm_vectors[8].sel = dpmi_index_to_sel(7);
            vm->dpmi.pm_vectors[8].off = 0x7000;
            vm->mem[0x7000] = 0xCF; /* IRET through the host trampoline. */
            dos_io_write8(vm, 0x20, 0x20);
            dos_io_write8(vm, 0x21, 0xFE);
            vm->timer_irq_pending = true;
        }
        bool admitted = gate ? dos_native_admit_gate_return(vm) : dos_native_prepare_return(vm);
        bool ok = !vm->step_limit_reached && !vm->dpmi.host_wait &&
            vm->native_dispatch_depth == (gate ? 0u : dispatch_depth);
        if (gate && !stop) {
            dpmi_interrupt_frame_t *frame = &vm->dpmi.interrupt_frames[0];
            bool delivered = admitted && !vm->timer_irq_pending && vm->dpmi.interrupt_depth == 1 &&
                cpu->cs == dpmi_index_to_sel(7) && cpu->eip == 0x7000 &&
                frame->cs == before.cs && frame->eip == before.eip &&
                frame->caller.ss == before.ss && frame->caller.esp == before.esp;
            /* An IRQ return must restore the already-repaired caller. */
            ok &= delivered && dos_test_run_until(vm, true, before.cs, before.eip);
        } else if (gate) ok &= vm->timer_irq_pending && !vm->dpmi.interrupt_depth;
        if (stop) ok &= !admitted && !cpu->running && cpu->exit_code == (stop == 1 ? 42 : -1);
        else {
            ok &= admitted && cpu->running && !cpu->halted &&
                !vm->dpmi.exception_depth && cpu->cs == before.cs &&
                cpu->ss == before.ss && cpu->esp == before.esp && cpu->eip == before.eip &&
                cpu->eflags == before.eflags && dos_test_general_registers(cpu, &before);
            if (ok) recoveries++;
        }
        if (!ok) {
            serial_puts("[DOS-NATIVE-SEG] handler mode="); serial_putdec(mode);
            serial_puts(" stack="); serial_putdec(stack32);
            serial_puts(" segment="); serial_putdec(segment);
            serial_puts(" stop="); serial_putdec(stop);
            serial_puts(" gate="); serial_putdec(gate);
            serial_puts(" admitted="); serial_putdec(admitted);
            serial_puts(" depth="); serial_putdec(vm->native_dispatch_depth);
            serial_puts(" flags="); serial_puthex(cpu->eflags, 8);
            serial_puts(" expected="); serial_puthex(before.eflags, 8);
            serial_puts(" esp="); serial_puthex(cpu->esp, 8);
            serial_puts(" ip="); serial_puthex(cpu->eip, 8);
            serial_puts(" gpr="); serial_putdec(dos_test_general_registers(cpu, &before));
            serial_puts("\n"); failures++;
        }
        vm->native_dispatch_depth = dispatch_depth;
        checks++;
    }
    serial_puts("[DOS-NATIVE-SEG] checks="); serial_putdec(checks);
    serial_puts(" recoveries="); serial_putdec(recoveries);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    vm->emulate_cpu = emulate;
    vm->step_limit = step_limit;
    vm->step_count = step_count;
    vm->step_limit_reached = limit_reached;
    vm->process_terminated = terminated;
    vm->timer_irq_pending = timer_pending;
    dos_io_write8(vm, 0x20, 0x20);
    dos_io_write8(vm, 0x21, pic_mask);
    return failures;
}

static uint32_t dos_near_test_code(dos_vm_t *vm, unsigned form, unsigned width,
                                    bool adr32, uint32_t target, unsigned extra,
                                    bool locked)
{
    uint32_t p = vm->cpu->eip;
    if ((width == 4) != vm->cpu->op_size_32) vm->mem[p++] = 0x66;
    if (adr32 != vm->cpu->addr_size_32) vm->mem[p++] = 0x67;
    vm->mem[p++] = form == 15 ? 0x36 : 0x64;
    if (locked) vm->mem[p++] = 0xF0;
    if (form == 1 || form == 7) {
        vm->mem[p++] = 0xFF;
        vm->mem[p++] = (form == 1 ? 0xD0 : 0xE0) | (extra & 7u);
    } else if (form == 2 || form == 8) {
        vm->mem[p++] = 0xFF;
        vm->mem[p++] = (form == 2 ? 0x10 : 0x20) | (adr32 ? 5u : 6u);
        for (unsigned b = 0; b < (adr32 ? 4u : 2u); b++) vm->mem[p++] = extra >> (8u * b);
    } else if (form == 15) {
        vm->mem[p++] = 0xFF;
        vm->mem[p++] = (adr32 ? 0x14 : 0x17) | (extra ? 0x40 : 0);
        if (adr32) vm->mem[p++] = 0x24;
        if (extra) vm->mem[p++] = extra;
    } else if (form == 3 || form == 4) {
        vm->mem[p++] = form == 3 ? 0xC3 : 0xC2;
        if (form == 4) { vm->mem[p++] = extra; vm->mem[p++] = extra >> 8; }
    } else {
        unsigned size = form == 0 || form == 5 || form == 10 ? width : 1u;
        if (form == 10) vm->mem[p++] = 0x0F;
        vm->mem[p++] = form == 0 ? 0xE8 : form == 5 ? 0xE9 : form == 6 ? 0xEB :
                       form == 9 ? 0x70u + extra : form == 10 ? 0x80u + extra : 0xE0u + form - 11u;
        uint32_t rel = target - (p + size);
        for (unsigned b = 0; b < size; b++) vm->mem[p++] = rel >> (8u * b);
    }
    return p;
}

static int dos_near_branch_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint32_t condition_flags[] = { FLAG_OF, FLAG_CF, FLAG_ZF, FLAG_CF,
                                         FLAG_SF, FLAG_PF, FLAG_SF, FLAG_ZF };
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned form = 5; form <= 14; form++)
    for (unsigned condition = 0; condition < (form == 9 || form == 10 ? 16u : 1u); condition++)
    for (unsigned taken = 0; taken < (form >= 9 ? 2u : 1u); taken++)
    for (unsigned backward = 0; backward < 2; backward++)
    for (unsigned profile = 0; profile < 5; profile++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        bool take = form < 9 || taken, locked = profile == 3;
        uint32_t site = profile == 2 ? mode ? 0x12340u : 0xFF80u
                      : profile == 1 && backward ? 0x20u : 0x1000u;
        uint32_t raw = site + (profile == 2 ? 0x80u : backward ? (uint32_t)-64 : 64u);
        uint32_t target = width == 4 ? raw : (uint16_t)raw;
        uint32_t limit = mode && profile == 2 ? 0x3FFFFu : 0xFFFFu;
        if (mode && profile == 1) limit = site + 0x10u;
        if (mode && profile == 4 && !backward) limit = target;
        if (mode) dpmi_desc_set_limit(&vm->dpmi.ldt[1], limit);
        cpu->eip = site;
        cpu->eax = width == 4 ? raw : 0xFEDC0000u | target;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_AF | FLAG_DF;
        if (form == 9 || form == 10) {
            if (take != (bool)(condition & 1u)) cpu->eflags |= condition_flags[condition >> 1];
        } else if (form == 12) cpu->eflags |= FLAG_ZF;
        vm->dpmi.virtual_interrupts_enabled = true;
        unsigned count = form == 14 ? !take : take ? 2u : 1u;
        cpu->ecx = (adr32 ? 0 : 0xABCD0000u) | count;
        dos_mem_write32(vm, 0x34800, raw);
        uint32_t next = dos_near_test_code(vm, form, width, adr32, raw,
                          form == 8 ? 0x4800u : form == 9 || form == 10 ? condition : 0, locked);
        cpu8086_state_t original = *cpu, expected = original;
        if (form >= 11 && form <= 13)
            expected.ecx = adr32 ? original.ecx - 1u
                : (original.ecx & 0xFFFF0000u) | (uint16_t)(original.ecx - 1u);
        bool failed = locked || (take && target > limit);
        bool ok = dos_test_run_one(vm);
        if (failed) {
            /* A USE16 DPMI exception record stores a 16-bit return IP. */
            cpu8086_state_t saved = original;
            if (mode == 1) saved.eip = (uint16_t)saved.eip;
            if (!dos_segment_test_fault(vm, &saved, locked ? 6u : 13u, 0) ||
                cpu->cr2 != original.cr2) ok = false;
            faults++;
        } else if (!dos_segment_test_result(vm, &expected, take ? target : next, 0)) ok = false;
        if (dos_mem_read32(vm, 0x34800) != raw) ok = false;
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-NEAR-BRANCH] case="); serial_putdec(checks);
                serial_puts(" form="); serial_putdec(form);
                serial_puts(" mode="); serial_putdec(mode);
                serial_puts(" profile="); serial_putdec(profile);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" target="); serial_puthex(target, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-NEAR-BRANCH] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_near_stack_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    uint8_t *expected_bytes = dos_host_alloc_pages(2);
    if (!expected_bytes) return 1;
    unsigned checks = 0, faults = 0, retries = 0;
    int failures = 0;
    for (unsigned profile = 0; profile < 3; profile++)
    for (unsigned mode = profile ? 1u : 0u; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < (mode ? 2u : 1u); stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned form = 0; form < 5; form++)
    for (unsigned bad_target = 0; bad_target < 2; bad_target++)
    for (unsigned scenario = 0; scenario < (profile ? profile == 2 ? 13u : 7u : mode ? 9u : 2u); scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        bool paged = profile != 0, call = form < 3, locked = !paged && scenario == 1;
        uint32_t origin = paged && stack32 ? 0x12340000u : 0;
        uint32_t base = !paged ? 0x20000u : profile == 1 ? 0x400000u : 0x403FF000u;
        uint32_t mask = stack32 ? UINT32_MAX : 0xFFFFu;
        uint32_t slot = origin + 4095u;
        cpu->esp = (stack32 ? origin : 0xABCD0000u) | (4095u + (call ? width : 0));
        uint32_t raw = bad_target ? mode ? 0x3000u : 0x10000u : 0x1800u;
        uint32_t target = width == 4 ? raw : (uint16_t)raw;
        uint32_t limit = mode ? 0x2000u : 0xFFFFu;
        cpu->eax = width == 4 ? raw : 0xFEDC0000u | target;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF | FLAG_AF | FLAG_OF;
        vm->dpmi.virtual_interrupts_enabled = true;
        dpmi_descriptor_t normal_stack = {0};
        bool stack_fault = false;
        if (mode) {
            dpmi_desc_set_limit(&vm->dpmi.ldt[1], limit);
            dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
            dpmi_desc_set_base(&vm->dpmi.ldt[2], base - origin);
            dpmi_desc_set_limit(&vm->dpmi.ldt[2], mask);
            normal_stack = vm->dpmi.ldt[2];
            if (!paged) switch (scenario) {
            case 2: dpmi_desc_set_limit(&vm->dpmi.ldt[2], slot + width - 2u); stack_fault = true; break;
            case 3: vm->dpmi.ldt[2].access &= ~DESC_PRESENT; stack_fault = true; break;
            case 4: vm->dpmi.ldt[2].access &= ~DESC_WRITABLE; stack_fault = true; break;
            case 5: case 6:
                vm->dpmi.ldt[2].access |= 4u;
                dpmi_desc_set_limit(&vm->dpmi.ldt[2], slot - (scenario == 5));
                stack_fault = scenario == 6; break;
            case 7: cpu->ss = 0; stack_fault = true; break;
            case 8: cpu->ss &= ~3u; stack_fault = true; break;
            }
        } else cpu->ss = base >> 4;
        uint32_t entry = 0, saved_entry = 0, fault_address = 0;
        unsigned page_error = 0;
        bool page_fault = false;
        if (paged) {
            dos_test_page_pair_t pair = dos_test_page_pair(vm, base);
            if (scenario) {
                unsigned page = (scenario - 1u) % 2u;
                unsigned permission = ((scenario - 1u) / 2u) % 3u;
                entry = scenario < 7 ? pair.pte[page] : pair.pde[page];
                saved_entry = dos_mem_read32(vm, entry);
                dos_mem_write32(vm, entry, saved_entry & ~(1u << permission));
                page_fault = permission != 1 || call;
                fault_address = base + (page ? 4096u : 4095u);
                page_error = 4u | (call ? 2u : 0) | (permission ? 1u : 0);
            }
        }
        for (unsigned i = 0; i < 8192; i++) {
            expected_bytes[i] = (uint8_t)(0x5Au + i * 13u);
            vm->mem[paged ? dos_string_test_physical(i) : base + i] = expected_bytes[i];
        }
        if (!call) for (unsigned b = 0; b < width; b++) {
            expected_bytes[4095u + b] = raw >> (8u * b);
            vm->mem[paged ? dos_string_test_physical(4095u + b) : base + 4095u + b] = expected_bytes[4095u + b];
        }
        dos_mem_write32(vm, 0x34800, raw);
        unsigned discard = adr32 ? 0xFFFFu : 17u;
        uint32_t next = dos_near_test_code(vm, form, width, adr32, raw,
                            form == 2 ? 0x4800u : form == 4 ? discard : 0, locked);
        cpu8086_state_t original = *cpu, expected = original;
        expected.esp = (original.esp & ~mask) |
            ((original.esp + (call ? 0u - width : width + (form == 4 ? discard : 0))) & mask);
        unsigned vector = locked ? 6u : call && target > limit ? 13u : stack_fault ? 12u
                        : page_fault ? 14u : target > limit ? 13u : 0;
        bool ok = dos_test_run_one(vm);
        if (vector) {
            if (!dos_segment_test_fault(vm, &original, vector, vector == 14 ? page_error : 0) ||
                cpu->cr2 != (vector == 14 ? fault_address : original.cr2)) ok = false;
            faults++;
            if (!mode) {
                unsigned offset = (uint16_t)(original.sp - 6u);
                uint16_t fields[] = { original.ip, original.cs, original.flags };
                for (unsigned f = 0; f < 3; f++) for (unsigned b = 0; b < 2; b++)
                    expected_bytes[offset + 2u * f + b] = fields[f] >> (8u * b);
            }
        } else {
            if (!dos_segment_test_result(vm, &expected, target, 0)) ok = false;
            if (call) for (unsigned b = 0; b < width; b++) expected_bytes[4095u + b] = next >> (8u * b);
        }
        for (unsigned i = 0; i < 8192; i++)
            if (vm->mem[paged ? dos_string_test_physical(i) : base + i] != expected_bytes[i]) ok = false;
        if (vector && mode && !locked && (paged || scenario < 7)) {
            vm->dpmi.ldt[2] = normal_stack;
            dpmi_desc_set_limit(&vm->dpmi.ldt[1], 0xFFFFu);
            if (entry) dos_mem_write32(vm, entry, saved_entry);
            vm->mem[0x6000u + vector * 16u] = 0xCB;
            vm->step_limit = vm->step_count + 16u;
            bool returned = dos_test_run_until(vm, true, original.cs, original.eip);
            vm->step_limit = 0;
            expected.cr2 = vector == 14 ? fault_address : original.cr2;
            if (!returned || !dos_test_run_one(vm) ||
                !dos_segment_test_result(vm, &expected, target, 0)) ok = false;
            if (call) for (unsigned b = 0; b < width; b++) expected_bytes[4095u + b] = next >> (8u * b);
            for (unsigned i = 0; i < 8192; i++)
                if (vm->mem[paged ? dos_string_test_physical(i) : base + i] != expected_bytes[i]) ok = false;
            retries++;
        }
        if (dos_mem_read32(vm, 0x34800) != raw) ok = false;
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-NEAR-STACK] case="); serial_putdec(checks);
                serial_puts(" form="); serial_putdec(form);
                serial_puts(" profile="); serial_putdec(profile);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" vector="); serial_putdec(vector);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" sp="); serial_puthex(cpu->esp, 8);
                serial_puts(" expected="); serial_puthex(expected.esp, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    dos_host_free_pages(expected_bytes, 2);
    serial_puts("[DOS-NEAR-STACK] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_near_boundary_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned stack32 = 0; stack32 < (mode ? 2u : 1u); stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned form = 0; form < 5; form++)
    for (unsigned overlap = 0; overlap < (form == 2 ? 2u : 1u); overlap++)
    for (unsigned high = 0; high < (mode && (!stack32 || adr32) ? 2u : 1u); high++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        uint32_t origin = stack32 && high ? 0x12340000u : 0;
        cpu->esp = (high ? 0x12340000u : 0) | 0x9000u;
        cpu->ebx = 0x9000u;
        if (mode) {
            dpmi_desc_set_limit(&vm->dpmi.ldt[1], UINT32_MAX);
            dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
            dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000u - origin);
            dpmi_desc_set_limit(&vm->dpmi.ldt[2], stack32 ? UINT32_MAX : 0xFFFFu);
            cpu->eip = 0x12340u;
        } else cpu->ss = 0x2000;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF | FLAG_AF | FLAG_OF;
        vm->dpmi.virtual_interrupts_enabled = true;
        uint32_t raw = form == 1 ? cpu->esp : mode ? 0x1ABCDu : 0x1800u;
        uint32_t target = width == 4 ? raw : (uint16_t)raw;
        uint8_t bytes[64];
        for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = vm->mem[0x28FE0u + i] = 0x5A;
        unsigned source = 32u - (form == 2 && overlap ? width : 0);
        if (form >= 2) for (unsigned b = 0; b < width; b++)
            bytes[source + b] = vm->mem[0x28FE0u + source + b] = raw >> (b * 8u);
        uint32_t next = dos_near_test_code(vm, form == 2 ? 15u : form, width, adr32, raw,
                           form == 1 ? 4u : form == 2 && overlap ? (uint8_t)(0u - width)
                           : form == 4 ? 0xFFFFu : 0, false);
        cpu8086_state_t original = *cpu, expected = original;
        uint32_t mask = stack32 ? UINT32_MAX : 0xFFFFu;
        expected.esp = (original.esp & ~mask) | ((original.esp +
            (form < 3 ? 0u - width : width + (form == 4 ? 0xFFFFu : 0))) & mask);
        bool failed = form == 2 && adr32 && high && !stack32;
        bool ok = dos_test_run_one(vm);
        if (failed) {
            cpu8086_state_t saved = original;
            if (mode == 1) saved.eip = (uint16_t)saved.eip;
            if (!dos_segment_test_fault(vm, &saved, 12, 0)) ok = false;
        } else {
            if (!dos_segment_test_result(vm, &expected, target, 0)) ok = false;
            if (form < 3) for (unsigned b = 0; b < width; b++) bytes[32u - width + b] = next >> (8u * b);
        }
        for (unsigned i = 0; i < sizeof(bytes); i++) if (vm->mem[0x28FE0u + i] != bytes[i]) ok = false;
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-NEAR-BOUNDARY] alias case="); serial_putdec(checks);
                serial_puts(" form="); serial_putdec(form);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" target="); serial_puthex(target, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }

    const unsigned forms[] = { 0, 1, 2, 3, 4, 5, 7, 8, 10 };
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned index = 0; index < sizeof(forms) / sizeof(forms[0]); index++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        unsigned form = forms[index];
        uint32_t target = width == 4 ? 0x400000u : 0x5000u;
        dpmi_desc_set_limit(&vm->dpmi.ldt[1], 0x7FFFFFu);
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_ZF;
        vm->dpmi.virtual_interrupts_enabled = true;
        cpu->eax = target;
        dos_mem_write32(vm, 0x34800, target);
        dos_mem_write32(vm, 0x9000, target);
        dos_test_page_pair_t pair = dos_test_page_pair(vm, target);
        dos_mem_write32(vm, pair.pte[0], 0);
        uint32_t next = dos_near_test_code(vm, form, width, adr32, target,
                            form == 2 || form == 8 ? 0x4800u : form == 10 ? 4u : 0, false);
        cpu8086_state_t original = *cpu, expected = original;
        uint32_t mask = mode == 2 ? UINT32_MAX : 0xFFFFu;
        if (form < 5) expected.esp = (original.esp & ~mask) |
            ((original.esp + (form < 3 ? 0u - width : width)) & mask);
        bool ok = dos_test_run_one(vm);
        if (!dos_segment_test_result(vm, &expected, target, 0)) ok = false;
        if (form < 3) {
            uint32_t address = expected.esp & mask;
            uint32_t pushed = width == 4 ? dos_mem_read32(vm, address) : dos_mem_read16(vm, address);
            if (pushed != next) ok = false;
        }
        expected.eip = target;
        /* The branch is committed; only the next instruction fetch faults. */
        if (!dos_test_run_one(vm)) ok = false;
        if (mode == 1) expected.eip = (uint16_t)expected.eip;
        if (!dos_segment_test_fault(vm, &expected, 14, 4) || cpu->cr2 != target) ok = false;
        if (!ok) {
            serial_puts("[DOS-NEAR-BOUNDARY] target fetch mismatch case="); serial_putdec(checks);
            serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n"); failures++;
        }
        checks++;
    }

    uint64_t pages = (sizeof(jit_state_t) + 4095u) / 4096u;
    jit_state_t *jit = dos_host_alloc_pages(pages);
    if (!jit) return failures + 1;
    jit_init(jit);
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned form = 0; form < 4; form++)
    for (unsigned taken = 0; taken < (form >= 2 ? 2u : 1u); taken++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        bool take = form < 2 || taken;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_AF;
        if ((form == 2 && take) || (form == 3 && !take)) cpu->eflags |= FLAG_ZF;
        vm->dpmi.virtual_interrupts_enabled = true;
        uint32_t p = 0x1000;
        vm->mem[p++] = form == 0 ? 0xE9 : form == 1 ? 0xEB : form == 2 ? 0x74 : 0x75;
        unsigned size = form ? 1u : mode == 2 ? 4u : 2u;
        uint32_t rel = 0x1080u - (p + size);
        for (unsigned b = 0; b < size; b++) vm->mem[p++] = rel >> (8u * b);
        vm->jit = jit;
        cpu8086_state_t original = *cpu;
        jit_block_t *block = jit_get_block(jit, cpu->cs, cpu->eip);
        dos_test_jit_decode(vm, block);
        jit_compile_block(jit, block);
        bool ok = block->compiled && jit_exec_block(vm, block) &&
            dos_segment_test_result(vm, &original, take ? 0x1080u : p, 0);
        *cpu = original;
        dpmi_desc_set_limit(&vm->dpmi.ldt[1], 0x1010u);
        cpu8086_sync_cs(cpu);
        if (jit_block_current(vm, block) || jit_exec_block(vm, block) ||
            !dos_segment_test_result(vm, &original, original.eip, 0)) ok = false;
        jit->hit_count[0x1000] = JIT_HOT_THRESHOLD;
        vm->step_limit = 10;
        if (!dos_test_run_until(vm, true, take ? dpmi_index_to_sel(7) : original.cs,
                                take ? 0x60D0u : p)) ok = false;
        vm->step_limit = 0;
        if (take ? !dos_segment_test_fault(vm, &original, 13, 0)
                 : !dos_segment_test_result(vm, &original, p, 0)) ok = false;
        if (!ok) {
            serial_puts("[DOS-NEAR-BOUNDARY] JIT limit mismatch mode="); serial_putdec(mode);
            serial_puts(" form="); serial_putdec(form); serial_puts("\n"); failures++;
        }
        vm->jit = NULL;
        checks++;
    }
    jit_destroy(jit);
    dos_host_free_pages(jit, pages);
    serial_puts("[DOS-NEAR-BOUNDARY] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

/* Forms: immediate/indirect CALL, immediate/indirect JMP, RETF, RETF imm. */
static uint32_t dos_far_test_code(dos_vm_t *vm, unsigned form, unsigned width,
                                   bool adr32, uint16_t selector, uint32_t target,
                                   unsigned discard, bool locked)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t p = cpu->eip;
    if ((width == 4) != cpu->op_size_32) vm->mem[p++] = 0x66;
    if (adr32 != cpu->addr_size_32) vm->mem[p++] = 0x67;
    vm->mem[p++] = 0x3E;
    if (locked) vm->mem[p++] = 0xF0;
    if (form == 1 || form == 3) {
        vm->mem[p++] = 0xFF;
        vm->mem[p++] = (form == 1 ? 0x18u : 0x28u) | (adr32 ? 5u : 6u);
        for (unsigned b = 0; b < (adr32 ? 4u : 2u); b++) vm->mem[p++] = 0x4800u >> (b * 8u);
        for (unsigned b = 0; b < width; b++) vm->mem[0x4800u + b] = target >> (b * 8u);
        dos_mem_write16(vm, 0x4800u + width, selector);
    } else if (form < 4) {
        vm->mem[p++] = form ? 0xEA : 0x9A;
        for (unsigned b = 0; b < width; b++) vm->mem[p++] = target >> (b * 8u);
        vm->mem[p++] = selector;
        vm->mem[p++] = selector >> 8;
    } else {
        vm->mem[p++] = form == 4 ? 0xCB : 0xCA;
        if (form == 5) { vm->mem[p++] = discard; vm->mem[p++] = discard >> 8; }
    }
    return p;
}

static void dos_far_test_field(dos_vm_t *vm, uint32_t address, unsigned width, uint32_t value)
{
    for (unsigned b = 0; b < width; b++) dos_mem_write8(vm, address + b, value >> (8u * b));
}

static int dos_far_contract_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned stack32 = 0; stack32 < (mode ? 2u : 1u); stack32++)
    for (unsigned dest32 = 0; dest32 < (mode ? 2u : 1u); dest32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned form = 0; form < 6; form++)
    for (unsigned profile = 0; profile < (mode ? 15u : 2u); profile++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        uint16_t selector = mode ? dpmi_index_to_sel(12) : 0x3000u;
        bool call = form < 2, ret = form >= 4;
        unsigned vector = profile == 1 ? 6u : 0, error = 0;
        unsigned discard = form == 5 ? adr32 ? 0xFFFFu : 17u : 0;
        uint32_t target = 0x1800u, mask = stack32 ? UINT32_MAX : 0xFFFFu;
        cpu->esp = stack32 ? 0x9000u : 0xABCD9000u;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF | FLAG_AF | FLAG_OF;
        vm->dpmi.virtual_interrupts_enabled = true;
        if (mode) {
            dos_test_descriptor(&vm->dpmi, selector, true, dest32);
            dos_test_descriptor(&vm->dpmi, cpu->ss, false, stack32);
            dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000u);
            dpmi_descriptor_t *d = &vm->dpmi.ldt[12];
            switch (profile) {
            case 2: selector = 3; vector = 13; break;
            case 3: vm->dpmi.descriptor_state[12] = DPMI_DESC_FREE; vector = 13; break;
            case 4: d->access &= ~DESC_CODE; vector = 13; break;
            case 5: d->access &= ~DESC_PRESENT; vector = 11; break;
            case 6: d->access &= ~0x20u; vector = 13; break;
            case 7: d->access = (d->access & ~0x20u) | 4u; break;
            case 8: d->access &= ~DESC_READABLE; break;
            case 9: dpmi_desc_set_limit(d, target - 1u); vector = 13; break;
            case 10:
                dpmi_desc_set_limit(&vm->dpmi.ldt[2], call ? 0x8FFFu - width : 0x9000u);
                if (call || ret) vector = 12;
                break;
            case 11:
                vm->dpmi.ldt[2].access &= ~DESC_WRITABLE;
                if (call || ret) vector = 12;
                break;
            case 12: case 13:
                vm->dpmi.ldt[2].access |= 4u;
                dpmi_desc_set_limit(&vm->dpmi.ldt[2], profile == 12 ? 0x8000u : 0x9000u);
                if (profile == 13 && (call || ret)) vector = 12;
                break;
            case 14: selector &= ~3u; if (ret) vector = 13; break;
            }
            if ((profile >= 2 && profile <= 6) || (profile == 14 && ret)) error = selector & ~3u;
        } else { cpu->ss = 0x2000u; cpu->ds = 0; }
        for (unsigned i = 0; i < 64; i++) vm->mem[0x28FE0u + i] = 0xA5u + i;
        if (ret) {
            dos_far_test_field(vm, 0x29000u, width, target);
            dos_far_test_field(vm, 0x29000u + width, width, 0xDEAD0000u | selector);
        }
        uint8_t bytes[64];
        for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = vm->mem[0x28FE0u + i];
        uint32_t next = dos_far_test_code(vm, form, width, adr32, selector, target, discard, profile == 1);
        cpu8086_state_t original = *cpu, expected = original;
        expected.cs = mode && !ret ? (selector & ~3u) | 3u : selector;
        if (call || ret) expected.esp = (original.esp & ~mask) |
            ((original.esp + (call ? 0u - 2u * width : 2u * width + discard)) & mask);
        bool ok = dos_test_run_one(vm);
        if (vector) {
            if (!dos_segment_test_fault(vm, &original, vector, error) || cpu->cr2 != original.cr2) ok = false;
            if (!mode) {
                uint16_t saved[] = { original.ip, original.cs, original.flags };
                for (unsigned f = 0; f < 3; f++) for (unsigned b = 0; b < 2; b++)
                    bytes[26u + f * 2u + b] = saved[f] >> (8u * b);
            }
            faults++;
        } else {
            if (!dos_segment_test_result(vm, &expected, target, 0) ||
                cpu->op_size_32 != (bool)dest32 || cpu->addr_size_32 != (bool)dest32) ok = false;
            if (mode && !(vm->dpmi.ldt[12].access & DESC_ACCESSED)) ok = false;
            if (call) {
                uint32_t saved[] = { next, original.cs };
                for (unsigned f = 0; f < 2; f++) for (unsigned b = 0; b < width; b++)
                    bytes[32u - 2u * width + f * width + b] = saved[f] >> (8u * b);
            }
        }
        for (unsigned i = 0; i < sizeof(bytes); i++) if (bytes[i] != vm->mem[0x28FE0u + i]) ok = false;
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-FAR-CONTRACT] case="); serial_putdec(checks);
                serial_puts(" mode="); serial_putdec(mode);
                serial_puts(" form="); serial_putdec(form);
                serial_puts(" profile="); serial_putdec(profile);
                serial_puts(" cs="); serial_puthex(cpu->cs, 4);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" sp="); serial_puthex(cpu->esp, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-FAR-CONTRACT] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_far_privilege_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned cpl = 0; cpl < 4; cpl++)
    for (unsigned rpl = 0; rpl < 4; rpl++)
    for (unsigned dpl = 0; dpl < 4; dpl++)
    for (unsigned conforming = 0; conforming < 2; conforming++)
    for (unsigned form = 0; form <= 4; form += 2) {
        dos_fetch_test_prepare(vm, cpu, mode);
        cpu->cs = (cpu->cs & ~3u) | cpl;
        cpu->ss = (cpu->ss & ~3u) | cpl;
        vm->dpmi.ldt[1].access = (vm->dpmi.ldt[1].access & ~DESC_DPL_MASK) | (cpl << 5);
        vm->dpmi.ldt[2].access = (vm->dpmi.ldt[2].access & ~DESC_DPL_MASK) | (cpl << 5);
        uint16_t selector = (dpmi_index_to_sel(12) & ~3u) | rpl;
        uint16_t outer_ss = (dpmi_index_to_sel(13) & ~3u) | rpl;
        dos_test_descriptor(&vm->dpmi, selector, true, mode != 2);
        vm->dpmi.ldt[12].access = (vm->dpmi.ldt[12].access & ~DESC_DPL_MASK) | (dpl << 5) | (conforming ? 4u : 0);
        dos_test_descriptor(&vm->dpmi, outer_ss, false, mode == 2);
        vm->dpmi.ldt[13].access = (vm->dpmi.ldt[13].access & ~DESC_DPL_MASK) | (rpl << 5);
        cpu->esp = 0x9000u;
        for (unsigned i = 0; i < 4; i++) {
            uint32_t fields[] = { 0x1800u, 0xA5A50000u | selector, 0x8123u, 0xBABA0000u | outer_ss };
            dos_far_test_field(vm, 0x9000u + i * width, width, fields[i]);
        }
        uint32_t next = dos_far_test_code(vm, form, width, mode == 2, selector, 0x1800u, 0, false);
        cpu8086_state_t original = *cpu, expected = original;
        bool invalid = form == 4 ? rpl < cpl || (conforming ? dpl > rpl : dpl != rpl)
                                 : conforming ? dpl > cpl : dpl != cpl || rpl > cpl;
        expected.cs = form == 4 ? selector : (selector & ~3u) | cpl;
        if (!form) expected.esp -= 2u * width;
        if (form == 4) {
            expected.esp += 2u * width;
            if (rpl > cpl) { expected.ss = outer_ss; expected.esp = 0x8123u; }
        }
        bool ok = dos_test_run_one(vm);
        if (invalid) {
            if (!dos_segment_test_fault(vm, &original, 13, selector & ~3u)) ok = false;
            faults++;
        } else {
            if (!dos_segment_test_result(vm, &expected, 0x1800u, 0) || cpu->op_size_32 != (mode != 2)) ok = false;
            if (!form && ((width == 4 ? dos_mem_read32(vm, expected.esp) : dos_mem_read16(vm, expected.esp)) != next)) ok = false;
        }
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-FAR-PRIVILEGE] case="); serial_putdec(checks);
                serial_puts(" form="); serial_putdec(form);
                serial_puts(" cpl/rpl/dpl="); serial_putdec(cpl); serial_putdec(rpl); serial_putdec(dpl);
                serial_puts(" cs="); serial_puthex(cpu->cs, 4);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-FAR-PRIVILEGE] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static void dos_far_test_gate(dos_vm_t *vm, unsigned width, unsigned parameters,
                               uint16_t selector, uint32_t target)
{
    dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(12), false, false);
    vm->dpmi.ldt[12] = (dpmi_descriptor_t){ .limit_lo = target, .base_lo = selector,
        .base_mid = parameters, .access = 0xE0u | (width == 4 ? 12u : 4u),
        .flags_lim = target >> 16, .base_hi = target >> 24 };
}

static void dos_far_test_tss(dos_vm_t *vm, unsigned width, unsigned cpl,
                              uint16_t selector, uint32_t esp)
{
    vm->cpu->gdtr.base = 0x50000u;
    vm->cpu->gdtr.limit = 15;
    dpmi_descriptor_t tss = { .access = width == 4 ? 0x8Bu : 0x83u };
    dpmi_desc_set_base(&tss, 0x51000u);
    dpmi_desc_set_limit(&tss, width == 4 ? 103u : 43u);
    cpu8086_cache_tr(vm->cpu, 8, &tss);
    for (unsigned b = 0; b < sizeof(tss); b++) vm->mem[0x50008u + b] = ((uint8_t *)&tss)[b];
    unsigned offset = width + cpl * 2u * width;
    dos_far_test_field(vm, 0x51000u + offset, width, esp);
    dos_mem_write16(vm, 0x51000u + offset + width, selector);
}

static int dos_far_gate_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const unsigned counts[] = { 0, 1, 31 };
    unsigned checks = 0, returns = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned tss_width = 2; tss_width <= 4; tss_width += 2)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned outer32 = 0; outer32 < 2; outer32++)
    for (unsigned cpl = 0; cpl < 4; cpl++)
    for (unsigned form = 0; form < 2; form++)
    for (unsigned sample = 0; sample < 3; sample++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        bool inward = cpl != 3;
        unsigned count = inward ? counts[sample] : 0;
        uint16_t code = (dpmi_index_to_sel(13) & ~3u) | cpl;
        uint16_t stack = (dpmi_index_to_sel(14) & ~3u) | cpl;
        dos_test_descriptor(&vm->dpmi, code, true, mode != 2);
        vm->dpmi.ldt[13].access = (vm->dpmi.ldt[13].access & ~DESC_DPL_MASK) | (cpl << 5);
        dos_test_descriptor(&vm->dpmi, stack, false, stack32);
        vm->dpmi.ldt[14].access = (vm->dpmi.ldt[14].access & ~DESC_DPL_MASK) | (cpl << 5);
        dpmi_desc_set_base(&vm->dpmi.ldt[14], 0x40000u);
        dos_test_descriptor(&vm->dpmi, cpu->ss, false, outer32);
        dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000u);
        cpu->esp = outer32 ? 0x9000u : 0xABCD9000u;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF | FLAG_AF | FLAG_OF;
        vm->dpmi.virtual_interrupts_enabled = true;
        dos_far_test_gate(vm, width, counts[sample], code | 3u, 0x1800u);
        dos_far_test_tss(vm, tss_width, cpl, stack, 0x8000u);
        for (unsigned i = 0; i < 256; i++) vm->mem[0x47F00u + i] = vm->mem[0x28F00u + i] = 0xA5;
        for (unsigned i = 0; i < 31; i++) dos_far_test_field(vm, 0x29000u + i * width, width, 0xCAFE1200u + i);
        /* The instruction width is deliberately the opposite of the gate. */
        uint32_t next = dos_far_test_code(vm, form, 6u - width, form, dpmi_index_to_sel(12), 0xFEDC9876u, 0, false);
        cpu8086_state_t original = *cpu, expected = original;
        unsigned fields = inward ? 4u + count : 2u;
        expected.cs = code;
        expected.ss = inward ? stack : original.ss;
        expected.esp = inward ? 0x8000u - fields * width : original.esp - 2u * width;
        uint32_t frame = inward ? 0x40000u + expected.esp : 0x20000u + (expected.esp & (outer32 ? UINT32_MAX : 0xFFFFu));
        bool ok = dos_test_run_one(vm) && dos_segment_test_result(vm, &expected, 0x1800u, 0) && cpu->op_size_32 == (mode != 2);
        for (unsigned i = 0; i < fields; i++) {
            uint32_t value = i == 0 ? next : i == 1 ? original.cs : i == fields - 2u ? original.esp
                               : i == fields - 1u ? original.ss : 0xCAFE1200u + i - 2u;
            for (unsigned b = 0; b < width; b++) if (vm->mem[frame + i * width + b] != (uint8_t)(value >> (b * 8u))) ok = false;
        }
        if (vm->mem[frame - 1u] != 0xA5) ok = false;
        if (inward) for (unsigned i = 0; i < count; i++) for (unsigned b = 0; b < width; b++)
            if (vm->mem[0x29000u + i * width + b] != (uint8_t)((0xCAFE1200u + i) >> (b * 8u))) ok = false;
        if (ok) {
            if (inward) {
                cpu->ds = (dpmi_index_to_sel(15) & ~3u) | cpl;
                cpu->fs = (dpmi_index_to_sel(16) & ~3u) | cpl;
                dos_test_descriptor(&vm->dpmi, cpu->ds, false, false);
                dos_test_descriptor(&vm->dpmi, cpu->fs, true, false);
                vm->dpmi.ldt[15].access = (vm->dpmi.ldt[15].access & ~DESC_DPL_MASK) | (cpl << 5);
                vm->dpmi.ldt[16].access = (vm->dpmi.ldt[16].access & ~DESC_DPL_MASK) | (cpl << 5) | 4u;
            }
            dos_far_test_code(vm, count ? 5u : 4u, width, form, 0, 0, count * width, false);
            expected = original;
            if (inward) {
                expected.ds = 0;
                expected.fs = cpu->fs;
                expected.gs = 0;
                uint32_t popped = width == 2 ? original.sp : original.esp;
                expected.esp = outer32 ? popped + count * width
                    : (popped & 0xFFFF0000u) | (uint16_t)(popped + count * width);
            }
            dos_test_import_cs(vm);
            if (inward) {
                /* RETF uses loaded data rights, not these edited table entries. */
                vm->dpmi.ldt[15].access |= DESC_DPL3;
                vm->dpmi.descriptor_state[16] = DPMI_DESC_FREE;
                cpu8086_cache_segment(cpu, 5, (mode + sample) & 3u, NULL);
            }
            if (!cpu8086_run_one(vm) || !dos_segment_test_result(vm, &expected, next, 0) ||
                cpu->op_size_32 != (mode == 2) ||
                (inward && (cpu->ds_cache.valid || !cpu->fs_cache.valid || cpu->gs_cache.valid))) ok = false;
            returns++;
        }
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-FAR-GATE] case="); serial_putdec(checks);
                serial_puts(" cpl="); serial_putdec(cpl);
                serial_puts(" width="); serial_putdec(width);
                serial_puts(" params="); serial_putdec(count);
                serial_puts(" cs="); serial_puthex(cpu->cs, 4);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" sp="); serial_puthex(cpu->esp, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-FAR-GATE] checks="); serial_putdec(checks);
    serial_puts(" returns="); serial_putdec(returns);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_far_paging_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    uint8_t *bytes = dos_host_alloc_pages(2);
    if (!bytes) return 1;
    unsigned checks = 0, faults = 0, retries = 0;
    int failures = 0;
    const uint32_t base = 0x403FF000u;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned form = 0; form < 6; form++)
    for (unsigned kind = 0; kind < 3; kind++)
    for (unsigned scenario = 0; scenario < 7; scenario++) {
        if ((kind == 0 && form != 1 && form != 3) || (kind == 1 && (form == 2 || form == 3))) continue;
        dos_fetch_test_prepare(vm, cpu, mode);
        bool call = form < 2, ret = form >= 4;
        uint16_t selector = kind == 2 ? 11u : dpmi_index_to_sel(12);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(12), true, mode != 2);
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF | FLAG_OF;
        vm->dpmi.virtual_interrupts_enabled = true;
        cpu->esp = kind == 1 ? 4095u + (call ? 2u * width : 0) : 0x9000u;
        if (kind == 0) dpmi_desc_set_base(&vm->dpmi.ldt[3], base + 4095u - 0x4800u);
        if (kind == 1) dpmi_desc_set_base(&vm->dpmi.ldt[2], base);
        if (kind == 2) { cpu->gdtr.base = base + 4095u - 8u; cpu->gdtr.limit = 15; }
        dos_test_page_pair_t pair = dos_test_page_pair(vm, base);
        unsigned discard = form == 5 ? 17u : 0;
        uint32_t next = dos_far_test_code(vm, form, width, mode == 2, selector, 0x1800u, discard, false);
        if (ret && kind != 1) {
            dos_far_test_field(vm, 0x9000, width, 0x1800u);
            dos_far_test_field(vm, 0x9000u + width, width, 0xABCD0000u | selector);
        }
        for (unsigned i = 0; i < 8192; i++) bytes[i] = (uint8_t)(i * 13u + 0x5Au);
        if (kind == 0 || (kind == 1 && ret)) {
            for (unsigned b = 0; b < width; b++) bytes[4095u + b] = 0x1800u >> (b * 8u);
            for (unsigned b = 0; b < (kind == 0 ? 2u : width); b++) bytes[4095u + width + b] = selector >> (b * 8u);
        }
        if (kind == 2) for (unsigned b = 0; b < 8; b++) bytes[4095u + b] = ((uint8_t *)&vm->dpmi.ldt[12])[b];
        for (unsigned i = 0; i < 8192; i++) vm->mem[dos_string_test_physical(i)] = bytes[i];
        unsigned page = scenario ? (scenario - 1u) % 2u : 0;
        unsigned permission = scenario ? (scenario - 1u) / 2u : 0;
        uint32_t entry = pair.pte[page], saved_entry = dos_mem_read32(vm, entry);
        if (scenario) dos_mem_write32(vm, entry, saved_entry & ~(1u << permission));
        bool write = kind == 1 && call;
        bool failed = scenario && (!permission || (permission == 1 && write) || (permission == 2 && kind != 2));
        bool accessed_fault = kind == 2 && scenario == 4;
        failed |= accessed_fault;
        uint32_t fault_address = base + (page ? kind == 1 && ret ? 4095u + width : 4096u : 4095u);
        unsigned error = (kind == 2 ? 0u : 4u) | (write ? 2u : 0) | (permission ? 1u : 0);
        if (accessed_fault) { fault_address = base + 4100u; error = 3; }
        cpu8086_state_t original = *cpu, expected = original;
        expected.cs = selector;
        if (call) expected.esp -= 2u * width;
        if (ret) expected.esp += 2u * width + discard;
        bool ok = dos_test_run_one(vm);
        if (failed) {
            if (!dos_segment_test_fault(vm, &original, 14, error) || cpu->cr2 != fault_address) ok = false;
            for (unsigned i = 0; i < 8192; i++) if (vm->mem[dos_string_test_physical(i)] != bytes[i]) ok = false;
            faults++;
            dos_mem_write32(vm, entry, saved_entry);
            vm->mem[0x60E0u] = 0xCB;
            vm->step_limit = vm->step_count + 16u;
            bool returned = dos_test_run_until(vm, true, original.cs, original.eip);
            vm->step_limit = 0;
            expected.cr2 = fault_address;
            if (!returned || !dos_test_run_one(vm)) ok = false;
            retries++;
        }
        if (!dos_segment_test_result(vm, &expected, 0x1800u, 0) || cpu->op_size_32 != (mode != 2)) ok = false;
        if (kind == 1 && call) {
            for (unsigned b = 0; b < width; b++) {
                bytes[4095u + b] = next >> (b * 8u);
                bytes[4095u + width + b] = original.cs >> (b * 8u);
            }
        }
        if (kind == 2) bytes[4100] |= DESC_ACCESSED;
        for (unsigned i = 0; i < 8192; i++) if (vm->mem[dos_string_test_physical(i)] != bytes[i]) ok = false;
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-FAR-PAGING] case="); serial_putdec(checks);
                serial_puts(" kind="); serial_putdec(kind);
                serial_puts(" form="); serial_putdec(form);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" cr2="); serial_puthex(cpu->cr2, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    dos_host_free_pages(bytes, 2);
    serial_puts("[DOS-FAR-PAGING] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_far_gate_fault_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned tss_width = 2; tss_width <= 4; tss_width += 2)
    for (unsigned scenario = 0; scenario < 28; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        uint16_t code = dpmi_index_to_sel(13) & ~3u;
        uint16_t stack = dpmi_index_to_sel(14) & ~3u;
        dos_test_descriptor(&vm->dpmi, code, true, mode != 2);
        dos_test_descriptor(&vm->dpmi, stack, false, true);
        vm->dpmi.ldt[13].access &= ~DESC_DPL_MASK;
        vm->dpmi.ldt[14].access &= ~DESC_DPL_MASK;
        dpmi_desc_set_base(&vm->dpmi.ldt[14], 0x40000u);
        dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000u);
        cpu->esp = 0x9000u;
        dos_far_test_gate(vm, width, 31, code, 0x1800u);
        dos_far_test_tss(vm, tss_width, 0, stack, 0x8000u);
        uint32_t frame = 0x48000u - 35u * width;
        for (unsigned i = 0; i < 256; i++) vm->mem[0x47F00u + i] = 0xA5;
        for (unsigned i = 0; i < 31; i++) dos_far_test_field(vm, 0x29000u + i * width, width, 0xCCAA0000u + i);
        unsigned vector = 0, error = 0, form = 0;
        uint32_t fault_address = cpu->cr2;
        switch (scenario) {
        case 1: vm->dpmi.ldt[12].access &= ~0x20u; vector = 13; error = dpmi_index_to_sel(12) & ~3u; break;
        case 2: vm->dpmi.ldt[12].access &= ~DESC_PRESENT; vector = 11; error = dpmi_index_to_sel(12) & ~3u; break;
        case 3: vm->dpmi.ldt[12].access &= ~0x0Fu; vector = 13; error = dpmi_index_to_sel(12) & ~3u; break;
        case 4: vm->dpmi.ldt[12].base_lo = 0; vector = 13; break;
        case 5: vm->dpmi.ldt[13].access &= ~DESC_CODE; vector = 13; error = code; break;
        case 6: vm->dpmi.ldt[13].access &= ~DESC_PRESENT; vector = 11; error = code; break;
        case 7: form = 2; vector = 13; error = code; break; /* JMP cannot lower CPL. */
        case 8: cpu8086_cache_tr(cpu, 0, NULL); vector = 10; break;
        case 9: cpu->tr |= 4u; vector = 10; error = cpu->tr; break;
        case 10: dpmi_desc_set_limit(&cpu->tss_cache.descriptor, 1); vector = 10; error = cpu->tr; break;
        case 11: cpu->tss_cache.valid = false; vector = 10; error = cpu->tr; break;
        case 12: dos_mem_write16(vm, 0x51000u + tss_width * 2u, 0); vector = 10; break;
        case 13: vm->dpmi.ldt[14].access |= 0x20u; vector = 10; error = stack; break;
        case 14: dos_mem_write16(vm, 0x51000u + tss_width * 2u, stack | 3u); vector = 10; error = stack; break;
        case 15: vm->dpmi.ldt[14].access &= ~DESC_PRESENT; vector = 12; error = stack; break;
        case 16: vm->dpmi.ldt[14].access &= ~DESC_WRITABLE; vector = 10; error = stack; break;
        case 17: dpmi_desc_set_limit(&vm->dpmi.ldt[14], 0x7FFEu); vector = 12; break;
        case 18: dpmi_desc_set_limit(&vm->dpmi.ldt[13], 0x17FFu); vector = 13; break;
        case 24: for (unsigned b = 0; b < 8; b++) vm->mem[0x50008u + b] = 0; break;
        case 25: dos_mem_write16(vm, 0x5000Au, 0xFFFF); break;
        case 26: cpu->gdtr.base = 0xDEADBEEFu; cpu->gdtr.limit = 0; break;
        case 27:
            (void)dos_test_page_pair(vm, 0x400000u);
            dos_mem_write32(vm, 0x11140u, 0); /* The GDT is no longer mapped. */
            break;
        default: break;
        }
        if (scenario >= 19 && scenario < 24) {
            (void)dos_test_page_pair(vm, 0x400000u);
            uint32_t address = scenario == 19 ? 0x51000u : scenario < 22 ? frame : 0x29000u;
            uint32_t entry = 0x11000u + (address >> 12) * 4u;
            uint32_t remove = scenario == 19 || scenario == 23 ? 1u : scenario == 20 ? 2u : 4u;
            dos_mem_write32(vm, entry, dos_mem_read32(vm, entry) & ~remove);
            if (scenario != 21) {
                vector = 14;
                error = scenario == 19 ? 0u : scenario == 20 ? 3u : scenario == 22 ? 5u : 4u;
                fault_address = scenario == 19 ? 0x51000u + tss_width : scenario == 20 ? frame : 0x29000u;
            }
        }
        uint32_t next = dos_far_test_code(vm, form, 6u - width, false, dpmi_index_to_sel(12), 0xDEADu, 0, false);
        cpu8086_state_t original = *cpu, expected = original;
        expected.cs = code;
        expected.ss = stack;
        expected.esp = frame - 0x40000u;
        bool ok = dos_test_run_one(vm);
        if (vector) {
            if (!dos_segment_test_fault(vm, &original, vector, error) || cpu->cr2 != fault_address) ok = false;
            for (unsigned i = 0; i < 256; i++) if (vm->mem[0x47F00u + i] != 0xA5) ok = false;
            faults++;
        } else {
            if (!dos_segment_test_result(vm, &expected, 0x1800u, 0)) ok = false;
            if ((width == 4 ? dos_mem_read32(vm, frame) : dos_mem_read16(vm, frame)) != next) ok = false;
        }
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-FAR-GATE-FAULT] case="); serial_putdec(checks);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" vector="); serial_putdec(vector);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" cr2="); serial_puthex(cpu->cr2, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-FAR-GATE-FAULT] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_far_boundary_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned form = 0; form < 6; form++)
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        if (!mode && scenario == 2) continue;
        dos_fetch_test_prepare(vm, cpu, mode);
        bool call = form < 2, ret = form >= 4;
        uint16_t selector = mode ? dpmi_index_to_sel(12) : 0x3000u;
        uint32_t target = scenario == 3 ? 0x10000u : width == 4 && mode ? 0x23456u : 0x2345u;
        if (width == 2) target = (uint16_t)target;
        cpu->eip = mode ? 0x12340u : 0xFF80u;
        cpu->esp = 0xABCD0000u | (scenario == 1 ? call ? 0u : 0x10000u - 2u * width
                                 : scenario == 2 ? call ? width : 0x10001u - 2u * width : 0x9000u);
        if (mode) {
            dos_test_descriptor(&vm->dpmi, selector, true, mode != 2);
            dos_test_descriptor(&vm->dpmi, cpu->ss, false, false);
            dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000u);
            dpmi_desc_set_limit(&vm->dpmi.ldt[1], 0x3FFFFu);
            dpmi_desc_set_limit(&vm->dpmi.ldt[12], scenario == 3 ? 0xFFFFu : 0x3FFFFu);
        } else { cpu->ss = 0x2000u; cpu->ds = 0; }
        for (unsigned i = 0; i < 64; i++) vm->mem[0x20000u + (uint16_t)(cpu->sp - 16u + i)] = 0xA5;
        if (ret) {
            for (unsigned f = 0; f < 2; f++) for (unsigned b = 0; b < width; b++)
                vm->mem[0x20000u + (uint16_t)(cpu->sp + f * width + b)] = (f ? selector : target) >> (8u * b);
        }
        uint8_t bytes[64];
        for (unsigned i = 0; i < 64; i++) bytes[i] = vm->mem[0x20000u + (uint16_t)(cpu->sp - 16u + i)];
        uint32_t next = dos_far_test_code(vm, form, width, mode == 2, selector, target, 0, false);
        cpu8086_state_t original = *cpu, expected = original, saved = original;
        if (mode == 1) saved.eip = original.ip;
        expected.cs = selector;
        if (call || ret) expected.sp = original.sp + (call ? 0u - 2u * width : 2u * width);
        unsigned vector = scenario == 2 && (call || ret) ? 12u : scenario == 3 && width == 4 ? 13u : 0;
        bool ok = dos_test_run_one(vm);
        if (vector) {
            if (!dos_segment_test_fault(vm, &saved, vector, 0)) ok = false;
        } else if (!dos_segment_test_result(vm, &expected, target, 0)) ok = false;
        if ((!vector && call) || (vector && !mode)) {
            unsigned field_width = vector ? 2u : width;
            unsigned fields = vector ? 3u : 2u;
            uint16_t start = original.sp - fields * field_width;
            for (unsigned f = 0; f < fields; f++) for (unsigned b = 0; b < field_width; b++) {
                unsigned index = (uint16_t)(start + f * field_width + b - (uint16_t)(original.sp - 16u));
                uint32_t value = f == 0 ? vector ? original.ip : next : f == 1 ? original.cs : original.flags;
                if (index < sizeof(bytes)) bytes[index] = value >> (8u * b);
            }
        }
        for (unsigned i = 0; i < 64; i++) if (bytes[i] != vm->mem[0x20000u + (uint16_t)(original.sp - 16u + i)]) ok = false;
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-FAR-BOUNDARY] case="); serial_putdec(checks);
                serial_puts(" form="); serial_putdec(form);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" sp="); serial_puthex(cpu->esp, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned cpl = 0; cpl < 4; cpl++)
    for (unsigned dpl = 0; dpl < 4; dpl++)
    for (unsigned conforming = 0; conforming < 2; conforming++)
    for (unsigned indirect = 0; indirect < 2; indirect++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        cpu->cs = (cpu->cs & ~3u) | cpl;
        vm->dpmi.ldt[1].access = (vm->dpmi.ldt[1].access & ~DESC_DPL_MASK) | (cpl << 5);
        cpu->ss = 0; /* JMP must not read a stack or TSS. */
        uint16_t selector = dpmi_index_to_sel(13);
        dos_test_descriptor(&vm->dpmi, selector, true, mode != 2);
        vm->dpmi.ldt[13].access = (vm->dpmi.ldt[13].access & ~DESC_DPL_MASK) | (dpl << 5) | (conforming ? 4u : 0);
        dpmi_desc_set_limit(&vm->dpmi.ldt[13], 0x3FFFFu);
        dos_far_test_gate(vm, width, 31, selector, 0x12345u);
        dos_far_test_code(vm, 2u + indirect, 6u - width, indirect, dpmi_index_to_sel(12), 0xDEADu, 0, false);
        cpu8086_state_t original = *cpu, expected = original;
        expected.cs = (selector & ~3u) | cpl;
        bool invalid = conforming ? dpl > cpl : dpl != cpl;
        bool ok = dos_test_run_one(vm);
        if (invalid ? !dos_segment_test_fault(vm, &original, 13, selector & ~3u)
                    : !dos_segment_test_result(vm, &expected, width == 4 ? 0x12345u : 0x2345u, 0)) ok = false;
        if (!ok) {
            serial_puts("[DOS-FAR-GATE-JMP] case="); serial_putdec(checks); serial_puts("\n"); failures++;
        }
        checks++;
    }
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned scenario = 0; scenario < 7; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        cpu->cs &= ~3u; cpu->ss &= ~3u;
        vm->dpmi.ldt[1].access &= ~DESC_DPL_MASK;
        vm->dpmi.ldt[2].access &= ~DESC_DPL_MASK;
        uint16_t selector = dpmi_index_to_sel(12), stack = dpmi_index_to_sel(13);
        dos_test_descriptor(&vm->dpmi, selector, true, mode != 2);
        dos_test_descriptor(&vm->dpmi, stack, false, true);
        unsigned vector = 0, error = 0;
        if (scenario == 1) { stack = 0; vector = 13; }
        if (scenario == 2) { stack &= ~3u; vector = 13; }
        if (scenario == 3) { vm->dpmi.ldt[13].access &= ~0x20u; vector = 13; }
        if (scenario == 4) { vm->dpmi.ldt[13].access &= ~DESC_WRITABLE; vector = 13; }
        if (scenario == 5) { vm->dpmi.ldt[13].access &= ~DESC_PRESENT; vector = 12; }
        if (scenario == 6) dpmi_desc_set_limit(&vm->dpmi.ldt[13], 1); /* New ESP is loaded, not dereferenced. */
        if (vector) error = stack & ~3u;
        cpu->esp = 0x9000u;
        uint32_t fields[] = { 0x1800u, selector, 0x7123u, stack };
        for (unsigned i = 0; i < 4; i++) dos_far_test_field(vm, 0x9000u + i * width, width, fields[i]);
        dos_far_test_code(vm, 4, width, false, 0, 0, 0, false);
        cpu8086_state_t original = *cpu, expected = original;
        expected.cs = selector; expected.ss = stack; expected.esp = 0x7123u;
        bool ok = dos_test_run_one(vm);
        if (vector ? !dos_segment_test_fault(vm, &original, vector, error)
                   : !dos_segment_test_result(vm, &expected, 0x1800u, 0)) ok = false;
        if (!ok) { serial_puts("[DOS-FAR-OUTER-SS] case="); serial_putdec(checks); serial_puts("\n"); failures++; }
        checks++;
    }
    serial_puts("[DOS-FAR-BOUNDARY] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static uint32_t dos_system_test_code(dos_vm_t *vm, unsigned subop, bool wide,
                                      bool adr32, unsigned reg, unsigned source,
                                      uint32_t offset, bool locked)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t p = cpu->eip = 0x1000;
    if (wide != cpu->op_size_32) vm->mem[p++] = 0x66;
    if (adr32 != cpu->addr_size_32) vm->mem[p++] = 0x67;
    if (source == 2) vm->mem[p++] = 0x36;
    if (locked) vm->mem[p++] = 0xF0;
    vm->mem[p++] = 0x0F;
    vm->mem[p++] = 0x00;
    vm->mem[p++] = (subop << 3) | (source ? adr32 ? 5u : 6u : 0xC0u | reg);
    if (source) for (unsigned i = 0; i < (adr32 ? 4u : 2u); i++)
        vm->mem[p++] = offset >> (i * 8u);
    return p;
}

static bool dos_system_cache_equal(const cpu_system_segment_t *a,
                                    const cpu_system_segment_t *b)
{
    if (a->valid != b->valid) return false;
    for (unsigned i = 0; i < sizeof(a->descriptor); i++)
        if (((const uint8_t *)&a->descriptor)[i] != ((const uint8_t *)&b->descriptor)[i])
            return false;
    return true;
}

static void dos_system_test_cpl(dos_vm_t *vm, unsigned cpl)
{
    cpu8086_state_t *cpu = vm->cpu;
    cpu->cs = (cpu->cs & ~3u) | cpl;
    cpu->ss = (cpu->ss & ~3u) | cpl;
    vm->dpmi.ldt[1].access = (vm->dpmi.ldt[1].access & ~DESC_DPL_MASK) | (cpl << 5);
    vm->dpmi.ldt[2].access = (vm->dpmi.ldt[2].access & ~DESC_DPL_MASK) | (cpl << 5);
}

static void dos_control_test_prepare(dos_vm_t *vm, cpu8086_state_t *cpu,
                                      unsigned context, unsigned cpl)
{
    /* VCPI's v86 bridge uses real segment addressing with PE and VM set. */
    unsigned mode = context == 5 ? 0u : context > 2 ? context - 2u : context;
    dos_fetch_test_prepare(vm, cpu, mode);
    if (mode) {
        dos_system_test_cpl(vm, context > 2 ? 3u : cpl);
    } else cpu->ds = 0;
    if (context > 2) cpu->eflags |= FLAG_VM;
    cpu->cr0 = 0x4005003Eu | (mode != 0 || context == 5);
    cpu->cr3 = 0xABCDF018u;
    cpu->gdtr.base = 0x12345678; cpu->gdtr.limit = 0x4567;
    cpu->idtr.base = 0x87654321; cpu->idtr.limit = 0xABCD;
    if (!mode) {
        /* Fault tests need a real IDT, also for the host v86 fixture. */
        cpu->idtr.base = 0x1800;
        for (unsigned v = 0; v < 32; v++)
            dos_mem_write32(vm, cpu->idtr.base + v * 4u, 0x6000u + v * 16u);
    }
    dpmi_descriptor_t tss = { .limit_lo = 103, .access = 0x8B, .base_hi = 0x45 };
    cpu8086_cache_tr(cpu, 0x28, &tss);
}

static uint32_t dos_control_test_code(dos_vm_t *vm, uint8_t opcode, uint8_t modrm,
                                       bool wide, bool adr32, bool stack,
                                       uint32_t offset, bool locked)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t p = cpu->eip = 0x1000;
    if (wide != cpu->op_size_32) vm->mem[p++] = 0x66;
    if (adr32 != cpu->addr_size_32) vm->mem[p++] = 0x67;
    if (stack) vm->mem[p++] = 0x36;
    if (locked) vm->mem[p++] = 0xF0;
    vm->mem[p++] = 0x0F; vm->mem[p++] = opcode;
    if (opcode == 0x01 || opcode == 0x20 || opcode == 0x22) vm->mem[p++] = modrm;
    if (opcode == 0x01 && (modrm & 0xC0u) != 0xC0u)
        for (unsigned i = 0; i < (adr32 ? 4u : 2u); i++) vm->mem[p++] = offset >> (i * 8u);
    return p;
}

static bool dos_control_test_state(const cpu8086_state_t *cpu,
                                    const cpu8086_state_t *expected)
{
    return cpu->cr0 == expected->cr0 && cpu->cr3 == expected->cr3 &&
        cpu->gdtr.base == expected->gdtr.base && cpu->gdtr.limit == expected->gdtr.limit &&
        cpu->idtr.base == expected->idtr.base && cpu->idtr.limit == expected->idtr.limit &&
        cpu->ldtr == expected->ldtr && cpu->tr == expected->tr &&
        cpu->host_ldt == expected->host_ldt &&
        dos_system_cache_equal(&cpu->ldt_cache, &expected->ldt_cache) &&
        dos_system_cache_equal(&cpu->tss_cache, &expected->tss_cache);
}

static int dos_control_group7_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned context = 0; context < 6; context++)
    for (unsigned cpl = 0; cpl < 4; cpl++)
    for (unsigned subop = 0; subop < 8; subop++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned source = 0; source < 3; source++)
    for (unsigned locked = 0; locked < 2; locked++) {
        dos_control_test_prepare(vm, cpu, context, cpl);
        cpu->eax = 0xA5A5FFF8u;
        const uint8_t initial[] = { 0xF8, 0xFF, 0x67, 0x45, 0x23, 0x81 };
        for (unsigned b = 0; b < 6; b++) vm->mem[0x4800 + b] = initial[b];
        unsigned vector = locked || subop == 5 || (!source && (subop < 4 || subop == 7))
            ? 6u : (subop == 2 || subop == 3 || subop >= 6) && context && (cpl || context > 2)
            ? 13u : 0;
        uint8_t modrm = (subop << 3) | (source ? adr32 ? 5u : 6u : 0xC0u);
        uint32_t next = dos_control_test_code(vm, 1, modrm, wide, adr32, source == 2, 0x4800, locked);
        cpu8086_state_t original = *cpu, expected = original;
        uint8_t bytes[6];
        for (unsigned b = 0; b < 6; b++) bytes[b] = initial[b];
        if (!vector) {
            if (subop < 2) {
                uint16_t limit = subop ? original.idtr.limit : original.gdtr.limit;
                uint32_t base = subop ? original.idtr.base : original.gdtr.base;
                for (unsigned b = 0; b < 6; b++) bytes[b] = b < 2
                    ? limit >> (b * 8u) : base >> ((b - 2u) * 8u);
            } else if (subop < 4) {
                uint32_t base = wide ? 0x81234567u : 0x234567u;
                if (subop == 2) { expected.gdtr.base = base; expected.gdtr.limit = 0xFFF8; }
                else { expected.idtr.base = base; expected.idtr.limit = 0xFFF8; }
            } else if (subop == 4) {
                if (!source) expected.eax = (expected.eax & 0xFFFF0000u) | (uint16_t)expected.cr0;
                else { bytes[0] = expected.cr0; bytes[1] = expected.cr0 >> 8; }
            } else if (subop == 6) expected.cr0 = (expected.cr0 & ~14u) | 8u;
        }
        bool ok = dos_test_run_one(vm);
        if (vector) {
            if (!dos_segment_test_fault(vm, &original, vector, 0)) ok = false;
            faults++;
        } else if (!dos_segment_test_result(vm, &expected, next, 0)) ok = false;
        if (!dos_control_test_state(cpu, &expected) || cpu->cr2 != expected.cr2 ||
            cpu->protected_mode != original.protected_mode) ok = false;
        for (unsigned b = 0; b < 6; b++) if (vm->mem[0x4800 + b] != bytes[b]) ok = false;
        if (vm->mem[0x4806] != 0xA5) ok = false;
        if (!ok) {
            if (failures < 12) { serial_puts("[DOS-CONTROL-G7] case="); serial_putdec(checks);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-CONTROL-G7] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_control_move_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned context = 0; context < 6; context++)
    for (unsigned cpl = 0; cpl < 4; cpl++)
    for (unsigned cr = 0; cr < 8; cr++)
    for (unsigned write = 0; write < 2; write++)
    for (unsigned mod = 0; mod < 4; mod++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned locked = 0; locked < 2; locked++) {
        dos_control_test_prepare(vm, cpu, context, cpl);
        cpu->esi = cr == 0 ? 0x40050037u - !context : cr == 4 ? 0 : 0x12345FFF;
        unsigned vector = locked || cr == 1 || cr >= 5 ? 6u
            : context && (cpl || context > 2) ? 13u : 0;
        uint32_t next = dos_control_test_code(vm, write ? 0x22 : 0x20,
            (mod << 6) | (cr << 3) | 6u, wide, !wide, true, 0, locked);
        cpu8086_state_t original = *cpu, expected = original;
        if (!vector) {
            if (!write) expected.esi = cr == 0 ? expected.cr0 : cr == 2 ? expected.cr2 :
                                       cr == 3 ? expected.cr3 : 0;
            else if (cr == 0) expected.cr0 = expected.esi;
            else if (cr == 2) expected.cr2 = expected.esi;
            else if (cr == 3) expected.cr3 = 0x12345018;
        }
        bool ok = dos_test_run_one(vm);
        if (vector) {
            if (!dos_segment_test_fault(vm, &original, vector, 0)) ok = false;
            faults++;
        } else if (!dos_segment_test_result(vm, &expected, next, 0)) ok = false;
        if (!dos_control_test_state(cpu, &expected) || cpu->cr2 != expected.cr2 ||
            cpu->protected_mode != original.protected_mode) ok = false;
        if (!ok) {
            if (failures < 12) { serial_puts("[DOS-CONTROL-MOV] case="); serial_putdec(checks);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    /* All GPRs, including ESP and r/m=4 without an encoded SIB byte. */
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned write = 0; write < 2; write++)
    for (unsigned reg = 0; reg < 8; reg++)
    for (unsigned mod = 0; mod < 4; mod++)
    for (unsigned wide = 0; wide < 2; wide++) {
        dos_control_test_prepare(vm, cpu, mode, 0);
        uint32_t next = dos_control_test_code(vm, write ? 0x22 : 0x20,
            (mod << 6) | 16u | reg, wide, !wide, false, 0, false);
        vm->mem[next] = 0xF4;
        cpu8086_state_t expected = *cpu;
        uint32_t *registers[] = { &expected.eax, &expected.ecx, &expected.edx, &expected.ebx,
            &expected.esp, &expected.ebp, &expected.esi, &expected.edi };
        if (write) expected.cr2 = *registers[reg];
        else *registers[reg] = expected.cr2;
        if (!dos_test_run_one(vm) || !dos_segment_test_result(vm, &expected, next, 0) ||
            !dos_control_test_state(cpu, &expected) || cpu->halted) failures++;
        checks++;
    }
    serial_puts("[DOS-CONTROL-MOV] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_control_bits_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned old = 0; old < 16; old++)
    for (unsigned value = 0; value < 16; value++)
    for (unsigned wide = 0; wide < 2; wide++) {
        dos_control_test_prepare(vm, cpu, mode, 0);
        unsigned source = (old + value) % 3u;
        cpu->cr0 = 0x60050030u | (old & 14u) | (mode != 0);
        cpu->eax = 0xA5A5FFF0u | value;
        dos_mem_write32(vm, 0x4800, cpu->eax);
        uint32_t next = dos_control_test_code(vm, 1, 0x30u |
            (source ? wide ? 5u : 6u : 0xC0u), wide, wide, source == 2, 0x4800, false);
        cpu8086_state_t expected = *cpu;
        expected.cr0 = (expected.cr0 & ~14u) | value;
        expected.protected_mode = (expected.cr0 & 1u) != 0;
        if (!dos_test_run_one(vm) || !dos_segment_test_result(vm, &expected, next, 0) ||
            !dos_control_test_state(cpu, &expected) || cpu->protected_mode != expected.protected_mode ||
            cpu->pm_cs_loaded != expected.pm_cs_loaded || cpu->op_size_32 != expected.op_size_32 ||
            cpu->addr_size_32 != expected.addr_size_32) failures++;
        checks++;
    }
    /* CR0 combination checks and ignored reserved bits are independent of
     * paging being enabled at the time of the write. */
    const uint32_t values[] = { 0, 1, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x80000000u,
        0x80000001u, 0x20000001u, 0x60000001u, 0x0005002Eu, 0x0005003Fu,
        0x10020041u, 0x40000001u };
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        dos_control_test_prepare(vm, cpu, mode, 0);
        cpu->eax = values[i];
        uint32_t next = dos_control_test_code(vm, 0x22, 0xC0, wide, !wide, false, 0, false);
        cpu8086_state_t original = *cpu, expected = original;
        bool invalid = ((values[i] & 0x80000000u) && !(values[i] & 1u)) ||
                       ((values[i] & 0x20000000u) && !(values[i] & 0x40000000u));
        bool ok = dos_test_run_one(vm);
        if (invalid) {
            if (!dos_segment_test_fault(vm, &original, 13, 0)) ok = false;
            faults++;
        } else {
            expected.cr0 = values[i] & 0xE005003Fu;
            if (!dos_segment_test_result(vm, &expected, next, 0) ||
                cpu->protected_mode != ((values[i] & 1u) != 0)) ok = false;
        }
        if (!dos_control_test_state(cpu, &expected) || cpu->cr2 != original.cr2) ok = false;
        if (!ok) { if (failures < 12) { serial_puts("[DOS-CONTROL-BITS] case="); serial_putdec(checks);
            serial_puts("\n"); } failures++; }
        checks++;
    }
    /* No silently accepted CR4 extension: test every nonzero bit separately. */
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned bit = 0; bit < 32; bit++) {
        dos_control_test_prepare(vm, cpu, mode, 0);
        cpu->eax = 1u << bit;
        (void)dos_control_test_code(vm, 0x22, 0xE0, false, false, false, 0, false);
        cpu8086_state_t original = *cpu;
        if (!dos_test_run_one(vm) || !dos_segment_test_fault(vm, &original, 13, 0) ||
            !dos_control_test_state(cpu, &original) || cpu->cr2 != original.cr2) failures++;
        checks++; faults++;
    }
    const uint8_t opcodes[] = { 0x06, 0x08, 0x09 };
    for (unsigned context = 0; context < 6; context++)
    for (unsigned cpl = 0; cpl < 4; cpl++)
    for (unsigned opcode = 0; opcode < 3; opcode++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned locked = 0; locked < 2; locked++) {
        dos_control_test_prepare(vm, cpu, context, cpl);
        unsigned vector = locked ? 6u : context && (cpl || context > 2) ? 13u : 0;
        uint32_t next = dos_control_test_code(vm, opcodes[opcode], 0, wide, !wide, true, 0, locked);
        cpu8086_state_t original = *cpu, expected = original;
        if (!vector && !opcode) expected.cr0 &= ~8u;
        bool ok = dos_test_run_one(vm);
        if (vector) { if (!dos_segment_test_fault(vm, &original, vector, 0)) ok = false; faults++; }
        else if (!dos_segment_test_result(vm, &expected, next, 0)) ok = false;
        if (!dos_control_test_state(cpu, &expected) || cpu->cr2 != original.cr2) ok = false;
        if (!ok) failures++;
        checks++;
    }
    serial_puts("[DOS-CONTROL-BITS] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_control_paging_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const unsigned operations[] = { 0, 1, 2, 3, 4, 6, 7 };
    const uint32_t bases[] = { 0x4000, 0x403FF000, 0xFFFFF000 };
    unsigned checks = 0, faults = 0, returns = 0, snapshots = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned op = 0; op < 7; op++)
    for (unsigned stack = 0; stack < 2; stack++)
    for (unsigned boundary = 0; boundary < 3; boundary++)
    for (unsigned scenario = 0; scenario < 9; scenario++)
    for (unsigned user = 0; user < 2; user++) {
        dos_control_test_prepare(vm, cpu, mode, user ? 3u : 0);
        cpu->eflags |= FLAG_IF | FLAG_IOPL_MASK;
        vm->dpmi.virtual_interrupts_enabled = true;
        unsigned subop = operations[op], length = subop < 4 ? 6u : 2u;
        bool store = subop < 2 || subop == 4;
        bool privileged = subop == 2 || subop == 3 || subop >= 6;
        uint32_t offset = length == 6 ? 0xFFCu : 0xFFFu;
        uint32_t linear = bases[boundary] + offset;
        dpmi_descriptor_t *segment = &vm->dpmi.ldt[stack ? 2 : 3];
        uint16_t valid_selector = stack ? cpu->ss : cpu->ds;
        dpmi_desc_set_base(segment, bases[boundary]);
        dpmi_desc_set_limit(segment, UINT32_MAX);
        dpmi_descriptor_t valid_segment = *segment;
        dos_test_page_pair_t pair = dos_test_page_pair(vm, linear);
        if (boundary == 2) for (unsigned page = 1; page < 512; page++)
            dos_mem_write32(vm, 0x13000 + page * 4u, (page << 12) | 7u);
        if (scenario == 1 || scenario == 2) {
            unsigned at = scenario == 1 ? 1u : 0;
            dos_mem_write32(vm, pair.pte[at], dos_mem_read32(vm, pair.pte[at]) & ~1u);
        }
        if (scenario == 3 || scenario == 4)
            dos_mem_write32(vm, pair.pte[1], dos_mem_read32(vm, pair.pte[1]) & ~2u);
        if (scenario == 4) cpu->cr0 &= ~DOS_CR0_WP;
        if (scenario == 5) dos_mem_write32(vm, pair.pte[1], dos_mem_read32(vm, pair.pte[1]) & ~4u);
        if (scenario == 6) segment->access &= ~DESC_PRESENT;
        if (scenario == 7) dpmi_desc_set_limit(segment, offset + length - 2u);
        if (scenario == 8) { if (stack) cpu->ss = 0; else cpu->ds = 0; }
        uint8_t bytes[] = { 0xF8, 0xFF, 0x67, 0x45, 0x23, 0x81 };
        for (unsigned b = 0; b < length + 2u; b++) {
            unsigned at = offset + b - 1u;
            vm->mem[at < 4096u ? 0x20000 + at : 0x24000 + at - 4096u] =
                b && b <= length ? bytes[b - 1u] : 0xA5;
        }
        uint32_t next = dos_control_test_code(vm, 1, (subop << 3) | 5u,
            wide, true, stack, offset, false);
        cpu8086_state_t original = *cpu, expected = original;
        unsigned vector = privileged && user ? 13u : subop == 7 ? 0u :
            scenario >= 6 ? stack ? 12u : 13u :
            scenario == 1 || scenario == 2 || (store && (scenario == 3 || (scenario == 4 && user))) ||
            (scenario == 5 && user) ? 14u : 0;
        unsigned error = vector == 14 ? (store ? 2u : 0) | (user ? 4u : 0) | (scenario >= 3) : 0;
        uint32_t address = vector == 14 ? scenario == 2 ? linear : (linear & ~0xFFFu) + 4096u : original.cr2;
        bool ok = dos_test_run_one(vm);
        if (vector) {
            if (!dos_segment_test_fault(vm, &original, vector, error) || cpu->cr2 != address ||
                !dos_control_test_state(cpu, &original)) ok = false;
            for (unsigned b = 0; b < length; b++) {
                unsigned at = offset + b;
                if (vm->mem[at < 4096u ? 0x20000 + at : 0x24000 + at - 4096u] != bytes[b]) ok = false;
            }
            for (unsigned page = 0; page < 2; page++)
                if (dos_mem_read32(vm, pair.pte[page]) & 0x60u) ok = false;
            faults++;
        }
        bool retry = vector && !(privileged && user);
        if (retry) {
            *segment = valid_segment;
            dos_mem_write32(vm, pair.pte[0], 0x20007);
            dos_mem_write32(vm, pair.pte[1], 0x24007);
            if (scenario == 8) {
                /* A null source needs an edited return selector as well as
                 * a repaired table. Exercise that case as an explicit snapshot. */
                if (stack) original.ss = valid_selector;
                else original.ds = valid_selector;
            }
            if (user && scenario != 8) {
                vm->mem[0x6000 + vector * 16u] = 0xCB;
                vm->step_limit = vm->step_count + 16u;
                if (!dos_test_run_until(vm, true, original.cs, original.eip)) ok = false;
                vm->step_limit = 0;
                returns++;
            } else {
                /* This is not a completed guest CPL0 IDT/IRET return. */
                original.cr2 = address;
                *cpu = original;
                vm->dpmi.exception_depth = 0;
                vm->dpmi.exception_esp_high[0] = 0;
                vm->dpmi.virtual_interrupts_enabled = true;
                snapshots++;
            }
            original.cr2 = address;
            expected = original;
            if (!dos_test_run_one(vm)) ok = false;
        }
        if (!vector || retry) {
            if (subop == 2 || subop == 3) {
                if (subop == 2) { expected.gdtr.base = wide ? 0x81234567u : 0x234567u; expected.gdtr.limit = 0xFFF8; }
                else { expected.idtr.base = wide ? 0x81234567u : 0x234567u; expected.idtr.limit = 0xFFF8; }
            } else if (subop == 6) expected.cr0 = (expected.cr0 & ~14u) | 8u;
            if (!dos_segment_test_result(vm, &expected, next, 0) || !dos_control_test_state(cpu, &expected)) ok = false;
            for (unsigned page = 0; page < 2; page++)
                if ((dos_mem_read32(vm, pair.pte[page]) & 0x60u) !=
                    (subop == 7 ? 0u : store ? 0x60u : 0x20u)) ok = false;
        }
        for (unsigned b = 0; b < length + 2u; b++) {
            unsigned at = offset + b - 1u;
            uint8_t value = b && b <= length ? bytes[b - 1u] : 0xA5;
            if ((!vector || retry) && store && b && b <= length) {
                unsigned part = b - 1u;
                uint16_t limit = subop == 4 ? original.cr0 : subop ? original.idtr.limit : original.gdtr.limit;
                uint32_t base = subop ? original.idtr.base : original.gdtr.base;
                value = part < 2 ? limit >> (part * 8u) : base >> ((part - 2u) * 8u);
            }
            if (vm->mem[at < 4096u ? 0x20000 + at : 0x24000 + at - 4096u] != value) ok = false;
        }
        if (!ok) {
            if (failures < 12) { serial_puts("[DOS-CONTROL-PAGING] case="); serial_putdec(checks);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" cr2="); serial_puthex(cpu->cr2, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-CONTROL-PAGING] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" handler-returns="); serial_putdec(returns);
    serial_puts(" snapshot-retries="); serial_putdec(snapshots);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_control_transition_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned operation = 0; operation < 4; operation++) {
        /* CR3 replacement, same-CR3 reload, PG enable and PG disable. */
        dos_control_test_prepare(vm, cpu, mode, 0);
        for (unsigned i = 0; i < 1024; i++) {
            dos_mem_write32(vm, 0x50000 + i * 4u, 0);
            dos_mem_write32(vm, 0x51000 + i * 4u, (i << 12) | 7u);
        }
        dos_mem_write32(vm, 0x50000, 0x51007);
        dos_mem_write32(vm, 0x51004, 0x60007);
        if (operation < 2) (void)dos_test_page_pair(vm, 0x80000);
        if (operation >= 2) cpu->cr3 = 0x50000;
        if (operation == 3) cpu->cr0 |= DOS_CR0_PG;
        cpu->eax = operation < 2 ? 0x5001Fu : operation == 2 ? cpu->cr0 | DOS_CR0_PG : cpu->cr0 & ~DOS_CR0_PG;
        if (operation == 1) {
            /* Warm the old mapping, then edit its page table before reloading
             * exactly the same CR3. This also checks the uncached walker. */
            cpu->cr3 = 0x50000;
            dos_mem_write32(vm, 0x51004, 0x1007);
            vm->mem[0x1000] = 0x90;
            cpu->eip = 0x1000;
            if (!dos_test_run_one(vm) || cpu->eip != 0x1001) failures++;
            dos_mem_write32(vm, 0x51004, 0x60007);
            cpu->eax = 0x50000;
        }
        uint32_t next = dos_control_test_code(vm, 0x22, operation < 2 ? 0xD8 : 0xC0,
            wide, !wide, false, 0, false);
        for (uint32_t at = 0x1000; at < next; at++) vm->mem[0x5F000 + at] = vm->mem[at];
        uint32_t p = next;
        if (mode == 1) vm->mem[p++] = 0x66;
        vm->mem[p++] = 0xBB;
        dos_mem_write32(vm, p, operation == 3 ? 0x12345678 : 0xDEADBEEF);
        p += 4;
        for (uint32_t at = next; at < p; at++) vm->mem[0x5F000 + at] = vm->mem[at];
        dos_mem_write32(vm, 0x5F000 + p - 4u, operation == 3 ? 0xDEADBEEF : 0x12345678);
        cpu8086_state_t expected = *cpu;
        if (operation < 2) expected.cr3 = operation ? 0x50000 : 0x50018;
        else expected.cr0 = cpu->eax;
        bool ok = dos_test_run_one(vm) && dos_segment_test_result(vm, &expected, next, 0) &&
                  dos_control_test_state(cpu, &expected);
        expected.ebx = 0x12345678;
        if (!dos_test_run_one(vm) || !dos_segment_test_result(vm, &expected, p, 0) ||
            !dos_control_test_state(cpu, &expected)) ok = false;
        if (!ok) { serial_puts("[DOS-CONTROL-TRANSITION] mapping case="); serial_putdec(checks);
            serial_puts("\n"); failures++; }
        checks++;
    }
    for (unsigned pe = 0; pe < 2; pe++)
    for (unsigned low = 1; low < 4; low++)
    for (unsigned operation = 0; operation < 4; operation++) {
        /* A real-mode CS value is not an RPL, including after setting PE
         * and before the first protected far transfer. */
        dos_control_test_prepare(vm, cpu, 0, 0);
        cpu->cs = 0x30u | low;
        cpu->protected_mode = pe != 0;
        cpu->cr0 |= pe;
        cpu->eax = operation == 1 ? 0x12345FFF : 0xFFFF0001;
        dos_mem_write16(vm, 0x4800, 0x4567);
        dos_mem_write32(vm, 0x4802, 0x87654321);
        uint32_t next = dos_control_test_code(vm, operation == 0 ? 0x06 : operation == 1 ? 0x22 : 0x01,
            operation == 1 ? 0xD8 : operation == 2 ? 0xF0 : 0x15, false, true, false, 0x4800, false);
        for (uint32_t at = 0x1000; at < next; at++) vm->mem[((uint32_t)cpu->cs << 4) + at] = vm->mem[at];
        cpu8086_state_t expected = *cpu;
        if (!operation) expected.cr0 &= ~8u;
        if (operation == 1) expected.cr3 = 0x12345018;
        if (operation == 2) { expected.cr0 = (expected.cr0 & ~14u) | 1u; expected.protected_mode = true; }
        if (operation == 3) expected.gdtr.base = 0x654321;
        if (!dos_test_run_one(vm) || !dos_segment_test_result(vm, &expected, next, 0) ||
            !dos_control_test_state(cpu, &expected) || cpu->pm_cs_loaded ||
            cpu->op_size_32 || cpu->addr_size_32 || cpu->protected_mode != expected.protected_mode) {
            serial_puts("[DOS-CONTROL-TRANSITION] PE case="); serial_putdec(checks);
            serial_puts("\n"); failures++;
        }
        checks++;
    }
    serial_puts("[DOS-CONTROL-TRANSITION] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_system_load_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned tss = 0; tss < 2; tss++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned source = 0; source < 3; source++)
    for (unsigned rpl = 0; rpl < 4; rpl++)
    for (unsigned access = 0; access < 256; access++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        dos_system_test_cpl(vm, 0);
        cpu->gdtr.base = 0x50000;
        cpu->gdtr.limit = 0x17;
        uint16_t selector = 0x10u | rpl;
        /* Loading the register must not touch the TSS/LDT body. */
        dpmi_descriptor_t descriptor = { .limit_lo = 1, .access = access,
            .flags_lim = wide ? 0x80 : 0 };
        dpmi_desc_set_base(&descriptor, 0xABCDEF00u);
        for (unsigned b = 0; b < 8; b++) vm->mem[0x50010 + b] = ((uint8_t *)&descriptor)[b];
        dpmi_descriptor_t old_tss = { .limit_lo = 103, .access = 0x8B, .base_hi = 0x45 };
        cpu8086_cache_tr(cpu, 0x38, &old_tss);
        cpu->eax = 0xA5A50000u | selector;
        dos_mem_write32(vm, 0x4800, 0xCCCC0000u | selector);
        uint32_t next = dos_system_test_code(vm, 2u + tss, wide, !wide, 0, source, 0x4800, false);
        cpu8086_state_t original = *cpu;
        unsigned type = access & 15u;
        unsigned vector = (access & 16u) || (tss ? type != 1u && type != 9u : type != 2u)
            ? 13u : !(access & 128u) ? 11u : 0;
        bool ok = dos_test_run_one(vm);
        if (vector) {
            if (!dos_segment_test_fault(vm, &original, vector, selector & ~3u) ||
                cpu->ldtr != original.ldtr || cpu->tr != original.tr ||
                cpu->host_ldt != original.host_ldt ||
                !dos_system_cache_equal(&cpu->ldt_cache, &original.ldt_cache) ||
                !dos_system_cache_equal(&cpu->tss_cache, &original.tss_cache)) ok = false;
            faults++;
        } else {
            if (!dos_segment_test_result(vm, &original, next, 0)) ok = false;
            cpu_system_segment_t expected = { descriptor, true };
            if (tss) {
                expected.descriptor.access |= 2u;
                if (cpu->tr != selector || !dos_system_cache_equal(&cpu->tss_cache, &expected) ||
                    cpu->ldtr != original.ldtr || cpu->host_ldt != original.host_ldt ||
                    !dos_system_cache_equal(&cpu->ldt_cache, &original.ldt_cache)) ok = false;
            } else if (cpu->ldtr != selector || cpu->host_ldt ||
                       !dos_system_cache_equal(&cpu->ldt_cache, &expected) ||
                       cpu->tr != original.tr ||
                       !dos_system_cache_equal(&cpu->tss_cache, &original.tss_cache)) ok = false;
        }
        for (unsigned b = 0; b < 8; b++)
            if (vm->mem[0x50010 + b] != (((uint8_t *)&descriptor)[b] |
                (!vector && tss && b == 5 ? 2u : 0))) ok = false;
        if (dos_mem_read32(vm, 0x4800) != (0xCCCC0000u | selector)) ok = false;
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-SYS-LOAD] case="); serial_putdec(checks);
                serial_puts(" access="); serial_puthex(access, 2);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-SYS-LOAD] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_system_encoding_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned subop = 0; subop < 8; subop++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned source = 0; source < 3; source++)
    for (unsigned cpl = 0; cpl < 4; cpl++)
    for (unsigned locked = 0; locked < 2; locked++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        if (mode) {
            dos_system_test_cpl(vm, cpl);
        } else cpu->ds = 0;
        dpmi_descriptor_t descriptor = { .limit_lo = 103, .access = 0x89 };
        cpu->gdtr.base = 0x50000;
        cpu->gdtr.limit = 0x17;
        for (unsigned b = 0; b < 8; b++) vm->mem[0x50010 + b] = ((uint8_t *)&descriptor)[b];
        cpu8086_cache_tr(cpu, 0x28, NULL);
        cpu->eax = 0xA5A50010u;
        dos_mem_write32(vm, 0x4800, 0xCCCC0010u);
        unsigned vector = !mode || locked || subop >= 6 ? 6u
                        : (subop == 2 || subop == 3) && cpl ? 13u
                        : subop == 2 ? 13u : 0;
        unsigned error = vector == 13 && !cpl ? 0x10u : 0;
        uint32_t next = dos_system_test_code(vm, subop, wide, adr32, 0, source, 0x4800, locked);
        cpu8086_state_t original = *cpu, expected = original;
        if (!vector && subop < 2 && !source)
            expected.eax = (wide ? 0 : 0xA5A50000u) | (subop ? 0x28u : 0);
        if (!vector && (subop == 4 || subop == 5)) expected.eflags &= ~FLAG_ZF;
        bool ok = dos_test_run_one(vm);
        if (vector) {
            if (!dos_segment_test_fault(vm, &original, vector, error)) ok = false;
            faults++;
        } else if (!dos_segment_test_result(vm, &expected, next, 0)) ok = false;
        uint32_t expected_memory = !vector && subop < 2 && source
            ? 0xCCCC0000u | (subop ? 0x28u : 0) : 0xCCCC0010u;
        if (dos_mem_read32(vm, 0x4800) != expected_memory || cpu->cr2 != original.cr2) ok = false;
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-SYS-ENCODING] case="); serial_putdec(checks);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    /* Register destinations cover ESP as well, without ever accessing it. */
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned subop = 0; subop < 2; subop++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned reg = 0; reg < 8; reg++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        uint32_t *registers[] = { &cpu->eax, &cpu->ecx, &cpu->edx, &cpu->ebx,
            &cpu->esp, &cpu->ebp, &cpu->esi, &cpu->edi };
        *registers[reg] = 0xA5A51234u;
        cpu->tr = 0xABCD;
        uint32_t next = dos_system_test_code(vm, subop, wide, false, reg, 0, 0, false);
        cpu8086_state_t expected = *cpu;
        uint32_t *destinations[] = { &expected.eax, &expected.ecx, &expected.edx, &expected.ebx,
            &expected.esp, &expected.ebp, &expected.esi, &expected.edi };
        *destinations[reg] = (wide ? 0 : 0xA5A50000u) | (subop ? 0xABCDu : 0);
        if (!dos_test_run_one(vm) || !dos_segment_test_result(vm, &expected, next, 0)) failures++;
        checks++;
    }
    serial_puts("[DOS-SYS-ENCODING] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_system_boundary_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned subop = 0; subop < 4; subop++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned source = 1; source <= 2; source++)
    for (unsigned scenario = 0; scenario < 14; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        dos_system_test_cpl(vm, 0);
        cpu->gdtr.base = 0x50000;
        cpu->gdtr.limit = 0x17;
        dpmi_descriptor_t descriptor = { .limit_lo = 0, .access = subop == 2 ? 0x82 : 0x89 };
        uint16_t selector = scenario < 4 ? scenario : scenario == 4 ? 0x14 : 0x13;
        unsigned vector = 0, error = 0;
        if (subop >= 2) {
            if (scenario < 4 && subop == 3) vector = 13;
            if (scenario == 4) { vector = 13; error = 0x14; }
            if (scenario == 5) { cpu->gdtr.limit = 22; vector = 13; error = 0x10; }
            if (scenario == 6) { descriptor.access &= ~DESC_PRESENT; vector = 11; error = 0x10; }
            if (scenario == 7) { descriptor.access = 0x83; vector = 13; error = 0x10; }
        }
        for (unsigned b = 0; b < 8; b++) vm->mem[0x50010 + b] = ((uint8_t *)&descriptor)[b];
        dos_mem_write32(vm, 0x4800, 0xCCCC0000u | selector);
        if (scenario == 8) {
            dpmi_desc_set_limit(&vm->dpmi.ldt[source == 2 ? 2 : 3], 0x4800);
            vector = source == 2 ? 12u : 13u; error = 0;
        }
        if (scenario == 9) {
            (void)dos_test_page_pair(vm, 0x400000u);
            dos_mem_write32(vm, 0x11010, 0x4006);
            vector = 14; error = subop < 2 ? 2u : 0;
        }
        if (scenario == 10) {
            dos_system_test_cpl(vm, 3);
            if (subop >= 2) vector = 13;
        }
        if (scenario == 11) {
            cpu->eflags |= FLAG_VM;
            vector = 6;
        }
        if (scenario == 12) vector = 6;
        if (scenario == 13) {
            dos_system_test_cpl(vm, 3);
            (void)dos_test_page_pair(vm, 0x400000u);
            dos_mem_write32(vm, 0x11010, 0x4006);
            vector = subop < 2 ? 14u : 13u;
            error = subop < 2 ? 6u : 0;
        }
        uint32_t next = dos_system_test_code(vm, subop, wide, !wide, 0, source, 0x4800, scenario == 12);
        cpu8086_state_t original = *cpu;
        bool ok = dos_test_run_one(vm);
        if (vector) {
            if (!dos_segment_test_fault(vm, &original, vector, error) ||
                cpu->ldtr != original.ldtr || cpu->tr != original.tr ||
                cpu->host_ldt != original.host_ldt ||
                !dos_system_cache_equal(&cpu->ldt_cache, &original.ldt_cache) ||
                !dos_system_cache_equal(&cpu->tss_cache, &original.tss_cache) ||
                cpu->cr2 != (vector == 14 ? 0x4800u : original.cr2)) ok = false;
            faults++;
        } else {
            if (!dos_segment_test_result(vm, &original, next, 0)) ok = false;
            if (subop == 2 && scenario < 4 &&
                (cpu->ldtr != selector || cpu->host_ldt || cpu->ldt_cache.valid)) ok = false;
        }
        if (dos_mem_read32(vm, 0x4800) !=
            (!vector && subop < 2 ? 0xCCCC0000u : 0xCCCC0000u | selector)) ok = false;
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-SYS-BOUNDARY] case="); serial_putdec(checks);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" cr2="); serial_puthex(cpu->cr2, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-SYS-BOUNDARY] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_system_paging_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint32_t linears[] = { 0x4FFCu, 0x403FFFFCu, 0xFFFFFFFCu };
    unsigned checks = 0, faults = 0, retries = 0, rejections = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned tss = 0; tss < 2; tss++)
    for (unsigned region = 0; region < 3; region++)
    for (unsigned wp = 0; wp < 2; wp++)
    for (unsigned scenario = 0; scenario < 7; scenario++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        dos_system_test_cpl(vm, 0);
        cpu->eflags |= FLAG_IF | FLAG_IOPL_MASK;
        vm->dpmi.virtual_interrupts_enabled = true;
        (void)dpmi_get_host_code_selector(vm);
        (void)dpmi_get_exception_stack_selector(vm);
        dpmi_descriptor_t previous = { .limit_lo = sizeof(vm->dpmi.ldt) - 1u, .access = 0x82 };
        dpmi_desc_set_base(&previous, 0x80000);
        for (unsigned i = 0; i < sizeof(vm->dpmi.ldt); i++)
            vm->mem[0x80000 + i] = ((uint8_t *)vm->dpmi.ldt)[i];
        cpu8086_cache_ldtr(cpu, 0x28, &previous);
        previous = (dpmi_descriptor_t){ .limit_lo = 103, .access = 0x8B };
        dpmi_desc_set_base(&previous, 0x90000);
        cpu8086_cache_tr(cpu, 0x30, &previous);
        uint32_t linear = linears[region];
        dos_test_page_pair_t pair = dos_test_page_pair(vm, linear);
        if (region == 2) for (unsigned i = 1; i < 512; i++)
            dos_mem_write32(vm, 0x13000u + i * 4u, i * 4096u | 7u);
        if (!wp) cpu->cr0 &= ~DOS_CR0_WP;
        cpu->gdtr.base = linear - 16u; cpu->gdtr.limit = 23;
        dpmi_descriptor_t descriptor = { .limit_lo = 0x1234, .access = tss ? 0x89 : 0x82 };
        dpmi_desc_set_base(&descriptor, 0x34567000);
        for (unsigned i = 0; i < 8; i++)
            vm->mem[i < 4 ? 0x20FFCu + i : 0x24000u + i - 4u] = ((uint8_t *)&descriptor)[i];
        uint32_t saved[2] = { 0x20003, 0x24003 };
        for (unsigned i = 0; i < 2; i++) dos_mem_write32(vm, pair.pte[i], saved[i]);
        if (scenario == 1 || scenario == 2)
            dos_mem_write32(vm, pair.pte[scenario - 1], saved[scenario - 1] & ~1u);
        if (scenario == 3 || scenario == 4)
            dos_mem_write32(vm, pair.pte[4u - scenario], saved[4u - scenario] & ~2u);
        /* High PDEs do not also contain the code and host exception stack. */
        if (scenario >= 5) {
            unsigned side = scenario - 5;
            if (region == 1) dos_mem_write32(vm, pair.pde[side], dos_mem_read32(vm, pair.pde[side]) & ~1u);
            else dos_mem_write32(vm, pair.pte[side], saved[side] & ~1u);
        }
        cpu->eax = 0xA5A50013;
        uint32_t next = dos_system_test_code(vm, tss ? 3u : 2u, mode != 2, true, 0, 0, 0, false);
        cpu8086_state_t original = *cpu;
        bool faulted = scenario == 1 || scenario == 2 || scenario >= 5 || (tss && wp && scenario == 3);
        unsigned error = tss && wp && scenario == 3 ? 3u : 0;
        uint32_t address = linear + (error ? 5u : scenario == 2 || scenario == 6 ? 4u : 0);
        bool ok = dos_test_run_one(vm);
        if (faulted) {
            if (!dos_segment_test_fault(vm, &original, 14, error) || cpu->cr2 != address ||
                cpu->ldtr != original.ldtr || cpu->tr != original.tr ||
                cpu->host_ldt != original.host_ldt ||
                !dos_system_cache_equal(&cpu->ldt_cache, &original.ldt_cache) ||
                !dos_system_cache_equal(&cpu->tss_cache, &original.tss_cache) ||
                vm->mem[0x24001] != descriptor.access) ok = false;
            faults++;
            for (unsigned i = 0; i < 2; i++) {
                dos_mem_write32(vm, pair.pte[i], saved[i]);
                dos_mem_write32(vm, pair.pde[i], dos_mem_read32(vm, pair.pde[i]) | 1u);
            }
            vm->mem[0x60E0] = 0xCB;
            vm->step_limit = vm->step_count + 16u;
            /* DPMI client returns cannot elevate to CPL0. Keep that rejection
             * explicit; a guest IDT/IRET return is a separate, open contract. */
            if (dos_test_run_until(vm, true, original.cs, original.eip) || cpu->running ||
                cpu->exit_code != -1 || cpu->eip != DPMI_EXCEPTION_RETURN_OFF + 2u ||
                vm->dpmi.exception_depth != 1 ||
                !dos_system_cache_equal(&cpu->ldt_cache, &original.ldt_cache) ||
                !dos_system_cache_equal(&cpu->tss_cache, &original.tss_cache)) ok = false;
            rejections++;
            vm->step_limit = 0;
            /* Restart the instruction from the checked interrupted snapshot,
             * without representing this fixture reset as a guest return. */
            original.cr2 = address;
            *cpu = original;
            vm->dpmi.exception_depth = 0;
            vm->dpmi.exception_esp_high[0] = 0;
            vm->dpmi.virtual_interrupts_enabled = true;
            if (!dos_test_run_one(vm)) ok = false;
            retries++;
        }
        cpu_system_segment_t expected = { descriptor, true };
        if (tss) expected.descriptor.access |= 2u;
        if (!dos_segment_test_result(vm, &original, next, 0) ||
            (tss ? cpu->tr : cpu->ldtr) != 0x13 ||
            !dos_system_cache_equal(tss ? &cpu->tss_cache : &cpu->ldt_cache, &expected) ||
            vm->mem[0x24001] != expected.descriptor.access ||
            (dos_mem_read32(vm, pair.pte[1]) & 0x60u) != (tss ? 0x60u : 0x20u)) ok = false;
        if (!ok) {
            if (failures < 12) {
                serial_puts("[DOS-SYS-PAGING] case="); serial_putdec(checks);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" cr2="); serial_puthex(cpu->cr2, 8); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-SYS-PAGING] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" snapshot-retries="); serial_putdec(retries);
    serial_puts(" DPMI-rejections="); serial_putdec(rejections);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_system_cache_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned null = 0; null < 4; null++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        cpu->gdtr.base = 0x50000; cpu->gdtr.limit = 0x27;
        dpmi_descriptor_t code = vm->dpmi.ldt[1];
        code.access &= ~DESC_DPL_MASK;
        dpmi_descriptor_t ldt = { .limit_lo = 7, .access = 0x82 };
        dpmi_desc_set_base(&ldt, 0x60000);
        dpmi_descriptor_t data = { .limit_lo = 0xABCD, .access = 0xF2 };
        for (unsigned i = 0; i < 8; i++) {
            vm->mem[0x50008 + i] = vm->mem[0x51008 + i] = ((uint8_t *)&code)[i];
            vm->mem[0x50020 + i] = ((uint8_t *)&ldt)[i];
            vm->mem[0x60000 + i] = ((uint8_t *)&data)[i];
            vm->mem[0x50010 + i] = vm->mem[0x51010 + i] = ((uint8_t *)&vm->dpmi.ldt[3])[i];
        }
        cpu->cs = 8; cpu->ds = 0x13;
        cpu->eax = 0x23;
        dos_system_test_code(vm, 2, true, true, 0, 0, 0, false);
        bool ok = dos_test_run_one(vm);
        cpu_system_segment_t cached = cpu->ldt_cache;
        for (unsigned i = 0; i < 8; i++) vm->mem[0x50020 + i] = 0;
        dpmi_descriptor_ref_t reference;
        dos_page_fault_t fault;
        if (!cached.valid || cpu->host_ldt || !dpmi_lookup_descriptor(vm, 4, &reference, &fault) ||
            reference.linear != 0x60000 || reference.descriptor.limit_lo != 0xABCD) ok = false;
        /* LGDT changes the table register, not an already loaded LDTR. */
        dos_mem_write16(vm, 0x4800, 0x27); dos_mem_write32(vm, 0x4802, 0x51000);
        uint32_t p = cpu->eip = 0x1000;
        if (mode == 1) { vm->mem[p++] = 0x66; vm->mem[p++] = 0x67; }
        vm->mem[p++] = 0x0F; vm->mem[p++] = 0x01; vm->mem[p++] = 0x15;
        dos_mem_write32(vm, p, 0x4800);
        if (!dos_test_run_one(vm) || cpu->gdtr.base != 0x51000 ||
            !dos_system_cache_equal(&cpu->ldt_cache, &cached)) ok = false;
        (void)dos_test_page_pair(vm, 0x400000u);
        dos_mem_write32(vm, 0x11140, 0); /* Old GDT absent. */
        if (!dpmi_lookup_descriptor(vm, 4, &reference, &fault) || fault.raised ||
            reference.linear != 0x60000) ok = false;
        dos_mem_write16(vm, 0x60000, 0x1234); /* LDT entries themselves remain live. */
        if (!dpmi_lookup_descriptor(vm, 4, &reference, &fault) ||
            reference.descriptor.limit_lo != 0x1234) ok = false;
        ldt.base_mid = 7;
        data.limit_lo = 0x5678;
        for (unsigned i = 0; i < 8; i++) {
            vm->mem[0x51020 + i] = ((uint8_t *)&ldt)[i];
            vm->mem[0x70000 + i] = ((uint8_t *)&data)[i];
        }
        cpu->eax = 0x23;
        dos_system_test_code(vm, 2, false, false, 0, 0, 0, false);
        if (!dos_test_run_one(vm) || !dpmi_lookup_descriptor(vm, 4, &reference, &fault) ||
            reference.linear != 0x70000 || reference.descriptor.limit_lo != 0x5678) ok = false;
        cpu->eax = null;
        dos_system_test_code(vm, 2, true, true, 0, 0, 0, false);
        if (!dos_test_run_one(vm) || cpu->ldtr != null || cpu->host_ldt || cpu->ldt_cache.valid ||
            dpmi_lookup_descriptor(vm, 4, &reference, &fault) || fault.raised ||
            dpmi_lookup_descriptor(vm, dpmi_index_to_sel(2), &reference, &fault) || fault.raised) ok = false;
        cpu->eax = dpmi_index_to_sel(2);
        dos_system_test_code(vm, 4, true, true, 0, 0, 0, false);
        if (!dos_test_run_one(vm) || (cpu->eflags & FLAG_ZF) || cpu->cs != 8 || cpu->ds != 0x13) ok = false;
        if (!ok) { serial_puts("[DOS-SYS-CACHE] case="); serial_putdec(checks); serial_puts("\n"); failures++; }
        checks++;
    }
    serial_puts("[DOS-SYS-CACHE] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static uint32_t dos_cs_test_jump(dos_vm_t *vm, uint32_t at, bool wide,
                                 uint16_t selector, uint32_t target)
{
    vm->mem[at++] = 0xEA;
    unsigned width = wide ? 4u : 2u;
    for (unsigned i = 0; i < width; i++) vm->mem[at++] = target >> (8u * i);
    dos_mem_write16(vm, at, selector);
    return at + 2u;
}

static bool dos_data_cache_load(dos_vm_t *vm, unsigned segment, uint16_t selector)
{
    vm->cpu->eip = 0x1000;
    vm->cpu->ax = selector;
    vm->mem[0x1000] = 0x8E;
    vm->mem[0x1001] = 0xC0u | (segment << 3);
    return cpu8086_run_one(vm) && vm->cpu->eip == 0x1002 && !vm->dpmi.exception_depth;
}

static bool dos_data_cache_access(dos_vm_t *vm, unsigned segment, uint32_t offset,
                                   bool write)
{
    const uint8_t prefixes[] = { 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65 };
    uint32_t p = 0x1020;
    vm->cpu->eip = p;
    if (!vm->cpu->addr_size_32) vm->mem[p++] = 0x67;
    vm->mem[p++] = prefixes[segment];
    vm->mem[p++] = write ? 0xA2 : 0xA0;
    dos_mem_write32(vm, p, offset);
    return cpu8086_run_one(vm) && vm->cpu->eip == p + 4 && !vm->dpmi.exception_depth;
}

static int dos_data_cache_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const unsigned segments[] = { 0, 2, 3, 4, 5 };
    unsigned checks = 0, failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned slot = 0; slot < 5; slot++)
    for (unsigned table = 0; table < 3; table++)
    for (unsigned mutation = 0; mutation < 8; mutation++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        unsigned s = segments[slot];
        uint16_t selector = table == 1 ? 0x43 : dpmi_index_to_sel(8);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(8), false, mode == 2);
        dpmi_descriptor_t *entry = &vm->dpmi.ldt[8];
        dpmi_desc_set_base(entry, 0x20000);
        dpmi_desc_set_limit(entry, 0x1FFFF);
        cpu->gdtr.base = 0x50000; cpu->gdtr.limit = 0xFF;
        if (table) {
            for (unsigned i = 0; i < 32u * 8u; i++)
                vm->mem[0x50000 + i] = ((uint8_t *)vm->dpmi.ldt)[i];
            entry = (dpmi_descriptor_t *)&vm->mem[0x50040];
            if (table == 2) {
                dpmi_descriptor_t ldt = { .limit_lo = 0xFF, .access = 0x82 };
                dpmi_desc_set_base(&ldt, 0x50000);
                cpu8086_cache_ldtr(cpu, 0x18, &ldt);
            }
        }
        bool ok = dos_data_cache_load(vm, s, selector);
        cpu_system_segment_t loaded = cpu->segment_cache[s];
        cpu8086_cache_segment(cpu, s, selector, &cpu->segment_cache[s].descriptor);
        if (!dos_system_cache_equal(&loaded, &cpu->segment_cache[s])) ok = false;
        cpu8086_state_t saved = *cpu;
        switch (mutation) {
        case 0: dpmi_desc_set_base(entry, 0x40000); break;
        case 1: dpmi_desc_set_limit(entry, 0x10); break;
        case 2: entry->flags_lim ^= DESC_32BIT; break;
        case 3: entry->access &= ~DESC_PRESENT; break;
        case 4: entry->access &= ~DESC_WRITABLE; break;
        case 5: entry->access &= ~DESC_DPL_MASK; break;
        case 6:
            if (table == 1) cpu->gdtr.limit = 0;
            else if (table == 2) cpu8086_cache_ldtr(cpu, 0, NULL);
            else vm->dpmi.descriptor_state[8] = DPMI_DESC_FREE;
            break;
        case 7:
            if (table == 1) cpu->gdtr.base = 0x60000;
            else cpu8086_cache_ldtr(cpu, 0, NULL);
            break;
        }
        vm->mem[0x24800] = 0x5A; vm->mem[0x44800] = 0xA5;
        if (!dos_data_cache_access(vm, s, 0x4800, false) || cpu->al != 0x5A) ok = false;
        cpu->al = 0x3C;
        if (!dos_data_cache_access(vm, s, 0x4800, true) || vm->mem[0x24800] != 0x3C ||
            vm->mem[0x44800] != 0xA5 || !dos_system_cache_equal(&loaded, &cpu->segment_cache[s]))
            ok = false;
        if (s == 2) {
            cpu->eip = 0x1040; cpu->esp = mode == 2 ? 0x19000 : 0xCAFE9000;
            cpu->eax = 0x12345678;
            vm->mem[0x1040] = 0x50;
            uint32_t next = mode == 2 ? 0x18FFCu : 0xCAFE8FFEu;
            if (!cpu8086_run_one(vm) || cpu->esp != next ||
                dos_mem_read16(vm, 0x20000u + (mode == 2 ? next : (uint16_t)next)) != 0x5678 ||
                cpu_stack_addr32(cpu) != (mode == 2)) ok = false;
        }
        cpu->gdtr = saved.gdtr; cpu->ldt_cache = saved.ldt_cache;
        cpu->ldtr = saved.ldtr; cpu->host_ldt = saved.host_ldt;
        vm->dpmi.descriptor_state[8] = DPMI_DESC_MUTABLE;
        *entry = loaded.descriptor;
        dpmi_desc_set_base(entry, 0x40000);
        entry->flags_lim ^= DESC_32BIT;
        if (!dos_data_cache_load(vm, s, selector) ||
            dpmi_desc_get_base(&cpu->segment_cache[s].descriptor) != 0x40000 ||
            !dos_data_cache_access(vm, s, 0x4800, false) || cpu->al != 0xA5 ||
            (s == 2 && cpu_stack_addr32(cpu) != (mode == 1))) ok = false;
        if (!ok) {
            serial_puts("[DOS-DATA-CACHE] mode/segment/table/edit="); serial_putdec(mode);
            serial_puts("/"); serial_putdec(s); serial_puts("/"); serial_putdec(table);
            serial_puts("/"); serial_putdec(mutation); serial_puts("\n"); failures++;
        }
        checks++;
    }

    /* Identical visible selectors may have different independently loaded bases. */
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned slot = 0; slot < 5; slot++) {
        unsigned s = segments[slot], other = s == 0 ? 3 : 0;
        dos_fetch_test_prepare(vm, cpu, mode);
        uint16_t selector = dpmi_index_to_sel(8);
        dos_test_descriptor(&vm->dpmi, selector, false, mode == 2);
        dpmi_desc_set_base(&vm->dpmi.ldt[8], 0x20000);
        bool ok = dos_data_cache_load(vm, s, selector) && dos_data_cache_load(vm, other, selector);
        dpmi_desc_set_base(&vm->dpmi.ldt[8], 0x40000);
        vm->mem[0x24800] = 0x5A; vm->mem[0x44800] = 0xA5;
        if (!dos_data_cache_load(vm, other, selector) ||
            !dos_data_cache_access(vm, s, 0x4800, false) || cpu->al != 0x5A ||
            !dos_data_cache_access(vm, other, 0x4800, false) || cpu->al != 0xA5) ok = false;
        if (!ok) { serial_puts("[DOS-DATA-CACHE] independent selector mismatch\n"); failures++; }
        checks++;
    }

    /* Rejected MOV loads retain the old segment, then retry through a real
     * DPMI exception return after only the target descriptor is repaired. */
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned slot = 0; slot < 5; slot++)
    for (unsigned failure = 0; failure < 4; failure++) {
        unsigned s = segments[slot];
        dos_fetch_test_prepare(vm, cpu, mode);
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK;
        vm->dpmi.virtual_interrupts_enabled = true;
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(8), false, mode == 2);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(9), false, mode == 2);
        dpmi_desc_set_base(&vm->dpmi.ldt[8], 0x20000);
        dpmi_desc_set_base(&vm->dpmi.ldt[9], 0x40000);
        dpmi_descriptor_t target = vm->dpmi.ldt[9];
        bool ok = dos_data_cache_load(vm, s, dpmi_index_to_sel(8));
        /* The faulting MOV follows the instruction shadow, not inside it. */
        cpu->eip = 0x1040; vm->mem[0x1040] = 0x90;
        if (!cpu8086_run_one(vm)) ok = false;
        cpu_system_segment_t loaded = cpu->segment_cache[s];
        switch (failure) {
        case 0: vm->dpmi.ldt[9].access &= ~DESC_PRESENT; break;
        case 1: vm->dpmi.ldt[9].access = 0xF8; break;
        case 2: vm->dpmi.ldt[9].access &= ~DESC_DPL_MASK; break;
        case 3: vm->dpmi.descriptor_state[9] = DPMI_DESC_FREE; break;
        }
        uint16_t selector = dpmi_index_to_sel(9);
        unsigned vector = failure ? 13u : s == 2 ? 12u : 11u;
        cpu->eip = 0x1000; cpu->ax = selector;
        cpu8086_state_t original = *cpu;
        if (dos_data_cache_load(vm, s, selector) ||
            !dos_segment_test_fault(vm, &original, vector, selector & ~3u) ||
            (s != 2 && !dos_system_cache_equal(&loaded, &cpu->segment_cache[s]))) ok = false;
        vm->dpmi.ldt[9] = target;
        vm->dpmi.descriptor_state[9] = DPMI_DESC_MUTABLE;
        vm->mem[0x6000u + vector * 16u] = 0xCB;
        vm->step_limit = vm->step_count + 16u;
        if (!cpu8086_run_until(vm, true, original.cs, original.eip) ||
            !dos_system_cache_equal(&loaded, &cpu->segment_cache[s])) ok = false;
        vm->step_limit = 0;
        if (!cpu8086_run_one(vm) || cpu->eip != 0x1002 ||
            dpmi_desc_get_base(&cpu->segment_cache[s].descriptor) != 0x40000) ok = false;
        if (!ok) {
            serial_puts("[DOS-DATA-CACHE] failed load mode/segment/case="); serial_putdec(mode);
            serial_puts("/"); serial_putdec(s); serial_puts("/"); serial_putdec(failure);
            serial_puts("\n"); failures++;
        }
        checks++;
    }

    /* The descriptor page is needed only at MOV Sreg, not at a later access. */
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned slot = 0; slot < 5; slot++)
    for (unsigned write = 0; write < 2; write++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        unsigned s = segments[slot];
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(8), false, mode == 2);
        dpmi_descriptor_t d = vm->dpmi.ldt[8];
        dpmi_desc_set_base(&d, 0x40000);
        cpu->gdtr.base = 0x50000; cpu->gdtr.limit = 0x47;
        dos_test_page_pair_t pair = dos_test_page_pair(vm, 0x50040);
        *(dpmi_descriptor_t *)&vm->mem[0x20040] = d;
        bool ok = dos_data_cache_load(vm, s, 0x43);
        dos_mem_write32(vm, pair.pte[0], 0);
        vm->mem[0x44800] = 0x5A; cpu->al = 0x3C;
        if (!dos_data_cache_access(vm, s, 0x4800, write) ||
            cpu->al != (write ? 0x3C : 0x5A) || vm->mem[0x44800] != (write ? 0x3C : 0x5A) ||
            dos_mem_read32(vm, pair.pte[0]) != 0 || cpu->cr2 != 0xCAFE1234) ok = false;
        if (!ok) { serial_puts("[DOS-DATA-CACHE] descriptor page mismatch\n"); failures++; }
        checks++;
    }

    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned slot = 0; slot < 5; slot++)
    for (unsigned null = 0; null < 4; null++) {
        unsigned s = segments[slot];
        if (s == 2) continue;
        dos_fetch_test_prepare(vm, cpu, mode);
        bool ok = dos_data_cache_load(vm, s, null) && !cpu->segment_cache[s].valid;
        cpu->eip = 0x1020;
        cpu8086_state_t original = *cpu;
        if (dos_data_cache_access(vm, s, 0x4800, false) ||
            !dos_segment_test_fault(vm, &original, 13, 0)) ok = false;
        if (!ok) { serial_puts("[DOS-DATA-CACHE] null selector mismatch\n"); failures++; }
        checks++;
    }

    /* An actual PE clear and real MOV Sreg preserve the loaded limit/B. */
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned slot = 0; slot < 5; slot++) {
        unsigned s = segments[slot];
        dos_fetch_test_prepare(vm, cpu, mode);
        dpmi_descriptor_t code = cpu->cs_cache.descriptor;
        code.access &= ~DESC_DPL_MASK;
        cpu8086_cache_cs(cpu, 8, &code, 0);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(8), false, mode == 2);
        vm->dpmi.ldt[8].access &= ~DESC_DPL_MASK;
        dpmi_desc_set_base(&vm->dpmi.ldt[8], 0x20000);
        dpmi_desc_set_limit(&vm->dpmi.ldt[8], 0x1FFFF);
        bool ok = dos_data_cache_load(vm, s, dpmi_index_to_sel(8) & ~3u);
        cpu_system_segment_t loaded = cpu->segment_cache[s];
        cpu->eip = 0x1040; cpu->eax = cpu->cr0 & ~1u;
        vm->mem[0x1040] = 0x0F; vm->mem[0x1041] = 0x22; vm->mem[0x1042] = 0xC0;
        if (!cpu8086_run_one(vm) || cpu->protected_mode ||
            !dos_system_cache_equal(&loaded, &cpu->segment_cache[s]) ||
            !dos_data_cache_load(vm, s, 0x4000)) ok = false;
        dpmi_desc_set_base(&loaded.descriptor, 0x40000);
        vm->mem[0x54800] = 0xA5;
        if (!dos_system_cache_equal(&loaded, &cpu->segment_cache[s]) ||
            !dos_data_cache_access(vm, s, 0x14800, false) || cpu->al != 0xA5) ok = false;
        if (!ok) { serial_puts("[DOS-DATA-CACHE] real reload mismatch\n"); failures++; }
        checks++;
    }

    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned slot = 0; slot < 5; slot++) {
        unsigned s = segments[slot];
        dos_fetch_test_prepare(vm, cpu, mode);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(8), false, mode == 2);
        dpmi_desc_set_limit(&vm->dpmi.ldt[8], 0x1FFFF);
        bool ok = dos_data_cache_load(vm, s, dpmi_index_to_sel(8));
        cpu->protected_mode = false;
        cpu->cr0 |= 1u; cpu->eflags |= FLAG_VM;
        cpu8086_reset_real_cs(cpu, 0);
        if (!dos_data_cache_load(vm, s, 0x4000) ||
            dpmi_desc_get_limit(&cpu->segment_cache[s].descriptor) != 0xFFFF ||
            (cpu->segment_cache[s].descriptor.flags_lim & DESC_32BIT) || cpu8086_cpl(cpu) != 3)
            ok = false;
        vm->mem[0x44800] = 0xA5;
        if (!dos_data_cache_access(vm, s, 0x4800, false) || cpu->al != 0xA5) ok = false;
        if (!ok) { serial_puts("[DOS-DATA-CACHE] v86 reload mismatch\n"); failures++; }
        checks++;
    }

    /* Setting PE makes MOV Sreg validate selectors immediately, before JMP CS. */
    for (unsigned slot = 0; slot < 5; slot++)
    for (unsigned low = 0; low < 4; low++) {
        unsigned s = segments[slot];
        dos_fetch_test_prepare(vm, cpu, 0);
        cpu8086_reset_real_cs(cpu, low);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(8), false, true);
        dpmi_descriptor_t d = vm->dpmi.ldt[8];
        d.access &= ~DESC_DPL_MASK;
        dpmi_desc_set_base(&d, 0x40000);
        cpu->gdtr.base = 0x50000; cpu->gdtr.limit = 0x47;
        *(dpmi_descriptor_t *)&vm->mem[0x50040] = d;
        uint32_t base = low << 4;
        cpu->eax = cpu->cr0 | 1u;
        vm->mem[base + 0x1000] = 0x0F; vm->mem[base + 0x1001] = 0x22;
        vm->mem[base + 0x1002] = 0xC0;
        bool ok = cpu8086_run_one(vm) && cpu->protected_mode && !cpu->pm_cs_loaded;
        cpu->ax = 0x40;
        vm->mem[base + 0x1003] = 0x8E; vm->mem[base + 0x1004] = 0xC0u | (s << 3);
        if (!cpu8086_run_one(vm) || cpu->eip != 0x1005 || cpu8086_cpl(cpu) ||
            !cpu->segment_cache[s].valid || dpmi_desc_get_base(&cpu->segment_cache[s].descriptor) != 0x40000)
            ok = false;
        if (!ok) { serial_puts("[DOS-DATA-CACHE] pre-far load mismatch\n"); failures++; }
        checks++;
    }
    serial_puts("[DOS-DATA-CACHE] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_cs_cache_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, failures = 0;
    unsigned pages = (sizeof(jit_state_t) + 4095u) / 4096u;
    jit_state_t *jit = dos_host_alloc_pages(pages);
    if (!jit) return 1;
    jit_init(jit);
    if (!jit->code_buf) { dos_host_free_pages(jit, pages); return 1; }

    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned table = 0; table < 3; table++)
    for (unsigned mutation = 0; mutation < 8; mutation++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        cpu8086_sync_cs(cpu);
        uint16_t selector = table == 1 ? 0x43u : dpmi_index_to_sel(8);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(8), true, mode == 2);
        dpmi_descriptor_t *entry = &vm->dpmi.ldt[8];
        dpmi_desc_set_base(entry, 0x20000);
        dpmi_desc_set_limit(entry, 0x1FFFF);
        cpu->gdtr.base = 0x50000;
        cpu->gdtr.limit = 0xFF;
        if (table) {
            for (unsigned i = 0; i < 32u * 8u; i++)
                vm->mem[0x50000 + i] = ((uint8_t *)vm->dpmi.ldt)[i];
            entry = (dpmi_descriptor_t *)&vm->mem[0x50040];
            if (table == 2) {
                dpmi_descriptor_t ldt = { .limit_lo = 0xFF, .access = 0x82 };
                dpmi_desc_set_base(&ldt, 0x50000);
                cpu8086_cache_ldtr(cpu, 0x18, &ldt);
            }
        }
        dos_cs_test_jump(vm, 0x1000, mode == 2, selector, 0x1000);
        bool ok = cpu8086_run_one(vm) && cpu->cs == selector && cpu->eip == 0x1000 &&
            dpmi_desc_get_base(&cpu->cs_cache.descriptor) == 0x20000 && cpu8086_cpl(cpu) == 3;
        dpmi_descriptor_t loaded = cpu->cs_cache.descriptor;
        cpu8086_cache_cs(cpu, cpu->cs, &cpu->cs_cache.descriptor, cpu->cpl);
        if (dpmi_desc_get_base(&cpu->cs_cache.descriptor) != 0x20000) ok = false;
        uint32_t p = 0x21000;
        vm->mem[p++] = 0xBB;
        for (unsigned i = 0; i < (mode == 2 ? 4u : 2u); i++) vm->mem[p++] = 0x1234u >> (8u * i);
        vm->mem[p] = 0xF4;
        jit_block_t *block = jit_get_block(jit, selector, 0x1000);
        if (jit_decode_block(vm, block) < 0 || jit_compile_block(jit, block) != 0) ok = false;
        cpu8086_state_t loaded_cpu = *cpu;
        switch (mutation) {
        case 0: dpmi_desc_set_base(entry, 0x40000); break;
        case 1: dpmi_desc_set_limit(entry, 0x10); break;
        case 2: entry->flags_lim ^= DESC_32BIT; break;
        case 3: entry->access &= ~DESC_PRESENT; break;
        case 4: entry->access &= ~DESC_CODE; break;
        case 5: entry->access &= ~DESC_READABLE; break;
        case 6:
            if (table == 1) cpu->gdtr.limit = 0;
            else if (table == 2) cpu8086_cache_ldtr(cpu, 0, NULL);
            else vm->dpmi.descriptor_state[8] = DPMI_DESC_FREE;
            break;
        case 7:
            if (table == 1) cpu->gdtr.base = 0x60000;
            else cpu8086_cache_ldtr(cpu, 0, NULL);
            break;
        }
        if (!jit_block_current(vm, block) || !cpu8086_run_one(vm) ||
            cpu->bx != 0x1234 || cpu->eip != p - 0x20000 ||
            cpu->op_size_32 != (mode == 2)) ok = false;
        cpu->eip = 0x1000;
        cpu->ebx = 0;
        (void)jit_exec_block(vm, block);
        if (cpu->bx != 0x1234 || cpu->eip != p - 0x20000) ok = false;

        p = 0x21010;
        vm->mem[p++] = 0x2E; vm->mem[p++] = 0xA0;
        for (unsigned i = 0; i < (mode == 2 ? 4u : 2u); i++) vm->mem[p++] = 0x3000u >> (8u * i);
        vm->mem[0x23000] = 0x5A; vm->mem[0x43000] = 0xA5;
        cpu->eip = 0x1010;
        if (!cpu8086_run_one(vm) || cpu->al != 0x5A || cpu->eip != p - 0x20000) ok = false;

        cpu->gdtr = loaded_cpu.gdtr;
        cpu->ldt_cache = loaded_cpu.ldt_cache;
        cpu->ldtr = loaded_cpu.ldtr;
        cpu->host_ldt = loaded_cpu.host_ldt;
        vm->dpmi.descriptor_state[8] = DPMI_DESC_MUTABLE;
        *entry = loaded;
        dpmi_desc_set_base(entry, 0x40000);
        entry->flags_lim ^= DESC_32BIT;
        dos_cs_test_jump(vm, 0x21030, mode == 2, selector, 0x1800);
        cpu->eip = 0x1030;
        if (!cpu8086_run_one(vm) || cpu->eip != 0x1800 || cpu->cs != selector ||
            cpu->op_size_32 != (mode == 1) || jit_block_current(vm, block)) ok = false;
        vm->mem[0x41800] = 0xBB;
        dos_mem_write32(vm, 0x41801, 0x6789);
        if (!cpu8086_run_one(vm) || cpu->bx != 0x6789 ||
            cpu->eip != (mode == 1 ? 0x1805u : 0x1803u)) ok = false;
        if (!ok) {
            serial_puts("[DOS-CS-CACHE] table/width/mutation="); serial_putdec(table);
            serial_puts("/"); serial_putdec(mode); serial_puts("/"); serial_putdec(mutation);
            serial_puts("\n"); failures++;
        }
        checks++;
    }

    /* A rejected load returns through the installed DPMI handler, then
     * retries after repairing only the target descriptor. */
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned table = 0; table < 2; table++)
    for (unsigned failure = 0; failure < 5; failure++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK;
        vm->dpmi.virtual_interrupts_enabled = true;
        cpu8086_sync_cs(cpu);
        uint16_t selector = table ? 0x43u : dpmi_index_to_sel(8);
        dos_test_descriptor(&vm->dpmi, dpmi_index_to_sel(8), true, mode == 2);
        dpmi_descriptor_t target = vm->dpmi.ldt[8];
        dpmi_desc_set_base(&target, 0x40000);
        cpu->gdtr.base = 0x50000; cpu->gdtr.limit = 0x47;
        dpmi_descriptor_t *entry = table ? (dpmi_descriptor_t *)&vm->mem[0x50040] : &vm->dpmi.ldt[8];
        *entry = target;
        switch (failure) {
        case 0: entry->access &= ~DESC_PRESENT; break;
        case 1: entry->access &= ~DESC_CODE; break;
        case 2: entry->access &= ~DESC_DPL_MASK; break;
        case 3: dpmi_desc_set_limit(entry, 0x17FF); break;
        case 4:
            if (table) cpu->gdtr.limit = 0x46;
            else vm->dpmi.descriptor_state[8] = DPMI_DESC_FREE;
            break;
        }
        dos_cs_test_jump(vm, 0x1000, mode == 2, selector, 0x1800);
        cpu8086_state_t original = *cpu;
        unsigned vector = failure ? 13u : 11u;
        unsigned error = failure == 3 ? 0 : selector & ~3u;
        bool ok = cpu8086_run_one(vm) && dos_segment_test_fault(vm, &original, vector, error);
        *entry = target;
        vm->dpmi.descriptor_state[8] = DPMI_DESC_MUTABLE;
        cpu->gdtr.limit = 0x47;
        vm->mem[0x6000u + vector * 16u] = 0xCB;
        vm->step_limit = vm->step_count + 16u;
        if (!cpu8086_run_until(vm, true, original.cs, original.eip) ||
            !dos_system_cache_equal(&cpu->cs_cache, &original.cs_cache) || cpu8086_cpl(cpu) != 3u)
            ok = false;
        vm->step_limit = 0;
        if (!cpu8086_run_one(vm) || cpu->cs != selector || cpu->eip != 0x1800 ||
            dpmi_desc_get_base(&cpu->cs_cache.descriptor) != 0x40000) ok = false;
        if (!ok) {
            serial_puts("[DOS-CS-CACHE] fault/retry case="); serial_putdec(checks);
            serial_puts("\n"); failures++;
        }
        checks++;
    }

    /* PE changes alone retain the code base, limit and default size. The
     * real-mode far reload changes the base; explicit host entry resets it. */
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned low = 0; low < 4; low++) {
        dos_fetch_test_prepare(vm, cpu, mode);
        dpmi_descriptor_t code = vm->dpmi.ldt[1];
        code.access &= ~DESC_DPL_MASK;
        dpmi_desc_set_base(&code, 0x20000);
        dpmi_desc_set_limit(&code, 0x1FFFF);
        cpu8086_cache_cs(cpu, 8, &code, 0);
        cpu->eax = cpu->cr0 & ~1u;
        vm->mem[0x21000] = 0x0F; vm->mem[0x21001] = 0x22; vm->mem[0x21002] = 0xC0;
        bool ok = cpu8086_run_one(vm) && !cpu->protected_mode && cpu->eip == 0x1003 &&
            cpu->op_size_32 == (mode == 2) && dpmi_desc_get_base(&cpu->cs_cache.descriptor) == 0x20000;
        uint16_t real_cs = 0x3000u | low;
        dos_cs_test_jump(vm, 0x21003, mode == 2, real_cs, 0x1800);
        if (!cpu8086_run_one(vm) || cpu->cs != real_cs || cpu->eip != 0x1800 ||
            cpu->op_size_32 != (mode == 2) || cpu8086_cpl(cpu) ||
            dpmi_desc_get_limit(&cpu->cs_cache.descriptor) != 0x1FFFF) ok = false;
        uint32_t base = (uint32_t)real_cs << 4;
        vm->mem[base + 0x1800] = 0xBB;
        dos_mem_write32(vm, base + 0x1801, 0x4567);
        if (!cpu8086_run_one(vm) || cpu->bx != 0x4567) ok = false;
        uint32_t hot_ip = mode == 2 ? 0x11000u : 0x1900u;
        vm->mem[base + hot_ip] = 0x43;
        vm->mem[base + hot_ip + 1u] = 0xEB;
        vm->mem[base + hot_ip + 2u] = 0x0D;
        cpu->eip = hot_ip;
        vm->jit = jit;
        jit->hit_count[(uint16_t)(hot_ip ^ (hot_ip >> 16))] = JIT_HOT_THRESHOLD;
        uint64_t translated = jit->jit_instructions;
        if (!cpu8086_run_until(vm, false, cpu->cs, hot_ip + 0x10u) ||
            cpu->bx != 0x4568 || jit->jit_instructions != translated + 2u) ok = false;
        vm->jit = NULL;
        cpu8086_state_t saved = *cpu;
        cpu8086_reset_real_cs(cpu, 0x1234);
        if (cpu->op_size_32 || dpmi_desc_get_limit(&cpu->cs_cache.descriptor) != 0xFFFF) ok = false;
        *cpu = saved;
        cpu->eax |= 1u;
        uint32_t ip = mode == 2 ? 0x11800u : 0x1808u;
        cpu->eip = ip;
        vm->mem[base + ip] = 0x0F; vm->mem[base + ip + 1] = 0x22; vm->mem[base + ip + 2] = 0xC0;
        vm->mem[base + ip + 3] = 0x43;
        if (!cpu8086_run_one(vm) || !cpu->protected_mode || cpu->pm_cs_loaded ||
            cpu8086_cpl(cpu) || cpu->eip != ip + 3 || !cpu8086_run_one(vm) ||
            cpu->bx != 0x4569 || cpu->eip != ip + 4) ok = false;
        if (!ok) { serial_puts("[DOS-CS-CACHE] PE transition mismatch\n"); failures++; }
        checks++;
    }

    /* A real CS paragraph's low bits are never the page-access CPL, even
     * with PE/PG set before the first protected far transfer. */
    for (unsigned low = 0; low < 4; low++)
    for (unsigned write = 0; write < 2; write++) {
        dos_fetch_test_prepare(vm, cpu, 0);
        cpu8086_reset_real_cs(cpu, 0x30u | low);
        cpu8086_load_real_segment(cpu, 3, 0);
        cpu->protected_mode = true;
        cpu->cr0 |= 1u;
        dos_test_page_pair_t pair = dos_test_page_pair(vm, 0x4800);
        dos_mem_write32(vm, 0x11004, 0x1003);
        dos_mem_write32(vm, pair.pte[0], 0x20003);
        uint32_t at = ((uint32_t)cpu->cs << 4) + 0x1000;
        vm->mem[at] = write ? 0xA2 : 0xA0;
        dos_mem_write16(vm, at + 1u, 0x4800);
        vm->mem[0x20800] = 0x5A; vm->mem[0x4800] = 0xA5;
        cpu->al = 0x3C;
        bool ok = cpu8086_run_one(vm) && cpu->eip == 0x1003 && !vm->dpmi.exception_depth &&
            vm->mem[0x20800] == (write ? 0x3C : 0x5A) && cpu->al == (write ? 0x3C : 0x5A) &&
            vm->mem[0x4800] == 0xA5 && (dos_mem_read32(vm, pair.pte[0]) & 0x60u) == (write ? 0x60u : 0x20u);
        if (!ok) { serial_puts("[DOS-CS-CACHE] pre-far paging mismatch\n"); failures++; }
        checks++;
    }

    jit_destroy(jit);
    dos_host_free_pages(jit, pages);
    dos_fetch_test_prepare(vm, cpu, 0);
    serial_puts("[DOS-CS-CACHE] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static void dos_iret_test_prepare(dos_vm_t *vm, cpu8086_state_t *cpu,
                                   unsigned mode, bool stack32, unsigned width,
                                   unsigned cpl)
{
    dos_fetch_test_prepare(vm, cpu, mode);
    if (mode) {
        dos_system_test_cpl(vm, cpl);
        dpmi_desc_set_base(&vm->dpmi.ldt[2], 0x20000);
        dpmi_desc_set_limit(&vm->dpmi.ldt[2], stack32 ? UINT32_MAX : 0xFFFFu);
        vm->dpmi.ldt[2].flags_lim = (vm->dpmi.ldt[2].flags_lim & ~DESC_32BIT) |
                                    (stack32 ? DESC_32BIT : 0);
    } else cpu->ss = 0x2000;
    dos_test_import_cs(vm);
    if (!mode && stack32) {
        cpu->ss_cache.descriptor.flags_lim |= DESC_32BIT;
        dpmi_desc_set_limit(&cpu->ss_cache.descriptor, UINT32_MAX);
    }
    cpu->esp = (stack32 ? 0 : 0xABCD0000u) | 0x9000;
    /* Exercise architectural permissions, not DPMI's virtual IOPL3 policy.
     * The bounded interpreter also prevents a native transition in tests. */
    vm->emulate_cpu = false;
    vm->step_limit = 64;
    unsigned p = 0x1000;
    if ((width == 4) != (mode == 2)) vm->mem[p++] = 0x66;
    vm->mem[p] = 0xCF;
}

static uint16_t dos_iret_test_descriptor(dos_vm_t *vm, unsigned index,
                                         unsigned cpl, bool code, bool wide)
{
    uint16_t selector = (dpmi_index_to_sel(index) & ~3u) | cpl;
    dos_test_descriptor(&vm->dpmi, selector, code, wide);
    vm->dpmi.ldt[index].access = (vm->dpmi.ldt[index].access & ~DESC_DPL_MASK) | (cpl << 5);
    return selector;
}

static void dos_iret_test_frame(dos_vm_t *vm, uint32_t address, unsigned width,
                                 const uint32_t *values, unsigned count)
{
    for (unsigned n = 0; n < count; n++) dos_far_test_field(vm, address + n * width, width, values[n]);
}

static int dos_iret_flags_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    int failures = 0;
    unsigned checks = 0;
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned cpl = 0; cpl < (mode ? 4u : 1u); cpl++)
    for (unsigned iopl = 0; iopl < 4; iopl++)
    for (unsigned set = 0; set < 2; set++) {
        dos_iret_test_prepare(vm, cpu, mode, stack32, width, cpl);
        uint16_t selector = mode ? dos_iret_test_descriptor(vm, 12, cpl, true, mode != 2) : 0x1234;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_CF | FLAG_RF | FLAG_AC |
                       FLAG_ID | FLAG_VIF | (iopl << 12);
        uint32_t incoming = set ? UINT32_MAX : 0;
        if (mode && !cpl) incoming &= ~FLAG_VM;
        const uint32_t frame[] = { 0x1800, 0xABCD0000u | selector, incoming };
        dos_iret_test_frame(vm, 0x29000, width, frame, 3);
        cpu8086_state_t expected = *cpu;
        uint32_t mask = 0x00254DD5u;
        if (!mode || cpl <= iopl) mask |= FLAG_IF;
        if (!mode || !cpl) mask |= FLAG_IOPL_MASK;
        if (mode && !cpl) mask |= FLAG_VIF | FLAG_VIP;
        if (width == 2) mask &= 0xFFFFu;
        expected.eflags = ((expected.eflags & ~mask) | (incoming & mask)) | FLAGS_FIXED;
        expected.cs = selector;
        expected.esp += width * 3u;
        bool ok = cpu8086_run_one(vm) && dos_segment_test_result(vm, &expected, 0x1800, 0) &&
            cpu8086_cpl(cpu) == cpl && cpu->op_size_32 == (mode && mode != 2) &&
            dos_system_cache_equal(&cpu->ss_cache, &expected.ss_cache);
        for (unsigned s = 0; s < 6; s++) if (s != 1 && s != 2 &&
            !dos_system_cache_equal(&cpu->segment_cache[s], &expected.segment_cache[s])) ok = false;
        if (mode && !(vm->dpmi.ldt[12].access & DESC_ACCESSED)) ok = false;
        if (!ok) {
            if (failures < 8) { serial_puts("[DOS-IRET-FLAGS] case="); serial_putdec(checks);
                serial_puts(" flags="); serial_puthex(cpu->eflags, 8);
                serial_puts(" expected="); serial_puthex(expected.eflags, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    /* DPMI's software IF remains virtual, independently of native FLAGS. */
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned enabled = 0; enabled < 2; enabled++) {
        dos_iret_test_prepare(vm, cpu, mode, mode == 2, width, 3);
        vm->emulate_cpu = true;
        cpu->eflags = FLAGS_FIXED | FLAG_IF;
        const uint32_t frame[] = { 0x1800, cpu->cs, FLAGS_FIXED | (enabled ? FLAG_IF : 0) };
        dos_iret_test_frame(vm, 0x29000, width, frame, 3);
        if (!cpu8086_run_one(vm) || cpu->eip != 0x1800 ||
            cpu->eflags != (FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK) ||
            vm->dpmi.virtual_interrupts_enabled != (enabled != 0)) failures++;
        checks++;
    }
    serial_puts("[DOS-IRET-FLAGS] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_iret_real_boundary_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    int failures = 0;
    unsigned checks = 0, faults = 0;
    for (unsigned context = 0; context < 3; context++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned scenario = 0; scenario < 7; scenario++) {
        bool stack32 = context == 1, v86 = context == 2;
        if (v86 && scenario == 6) continue;
        dos_iret_test_prepare(vm, cpu, 0, stack32, width, 0);
        cpu->eflags = FLAGS_FIXED | FLAG_IOPL_MASK | FLAG_CF | FLAG_DF;
        if (v86) {
            cpu->cr0 |= 1u;
            cpu->eflags |= FLAG_VM;
            cpu8086_load_real_cs(cpu, 0);
            for (unsigned s = 0; s < 6; s++) if (s != 1)
                cpu8086_load_real_segment(cpu, s, s == 2 ? 0x2000 : 0);
        }
        uint32_t mask = stack32 ? UINT32_MAX : 0xFFFFu;
        unsigned start = scenario == 1 ? 0xFFFFu : scenario == 2 || scenario == 6 ? 0xFFFEu : 0xFFFCu;
        cpu->esp = (stack32 ? 0 : 0xABCD0000u) | start;
        if (scenario == 3) dpmi_desc_set_limit(&cpu->ss_cache.descriptor, 0xFFFE);
        if (scenario == 4) dpmi_desc_set_limit(&cpu->cs_cache.descriptor, 0x17FF);
        if (scenario == 6) dpmi_desc_set_limit(&cpu->ss_cache.descriptor, 0x1FFFF);
        const uint32_t frame[] = { scenario == 5 && width == 4 ? 0x10000u : 0x1800u,
            0xABCD1357u, FLAGS_FIXED | FLAG_IOPL_MASK | FLAG_ZF };
        for (unsigned n = 0; n < 3; n++)
            dos_far_test_field(vm, 0x20000u + ((start + n * width) & mask), width, frame[n]);
        unsigned vector = ((!stack32 && (scenario == 1 || (scenario == 2 && width == 4))) || scenario == 3)
            ? 12u : scenario == 4 || (scenario == 5 && width == 4) ? 13u : 0;
        cpu8086_state_t original = *cpu, expected = original;
        bool ok = cpu8086_run_one(vm);
        if (scenario == 4) {
            /* The installed #GP/#DF destinations also exceed this loaded
             * real CS limit. Delivery stops without consuming the IRET. */
            ok = !ok && !cpu->running && cpu->insn_count == original.insn_count + 1u &&
                  cpu->exit_code == -1 && cpu->eip == original.eip &&
                  cpu->cs == original.cs && cpu->esp == original.esp &&
                  cpu->eflags == original.eflags && !cpu->delivery_fault &&
                  dos_test_general_registers(cpu, &original);
            faults++;
        } else if (vector) {
            if (!dos_segment_test_fault(vm, &original, vector, 0)) ok = false;
            faults++;
        } else {
            expected.cs = 0x1357;
            expected.eflags = frame[2] | (v86 ? FLAG_VM : 0);
            expected.esp = (original.esp & ~mask) | ((original.esp + 3u * width) & mask);
            if (!dos_segment_test_result(vm, &expected, frame[0], 0) ||
                !dos_system_cache_equal(&cpu->ss_cache, &original.ss_cache)) ok = false;
        }
        if (!ok) { if (failures < 8) { serial_puts("[DOS-IRET-REAL-BOUNDARY] case="); serial_putdec(checks);
            serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n"); } failures++; }
        checks++;
    }
    serial_puts("[DOS-IRET-REAL-BOUNDARY] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_iret_outer_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    int failures = 0;
    unsigned checks = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned new32 = 0; new32 < 2; new32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned cpl = 0; cpl < 3; cpl++)
    for (unsigned rpl = cpl + 1u; rpl < 4; rpl++)
    for (unsigned conforming = 0; conforming < 2; conforming++)
    for (unsigned allow_if = 0; allow_if < 2; allow_if++) {
        dos_iret_test_prepare(vm, cpu, mode, stack32, width, cpl);
        uint16_t cs = dos_iret_test_descriptor(vm, 12, rpl, true, mode != 2);
        uint16_t ss = dos_iret_test_descriptor(vm, 13, rpl, false, new32);
        dpmi_desc_set_base(&vm->dpmi.ldt[13], 0x30000);
        if (conforming) vm->dpmi.ldt[12].access =
            (vm->dpmi.ldt[12].access & ~DESC_DPL_MASK) | 4u;
        cpu->ds = dos_iret_test_descriptor(vm, 3, cpl, false, false);
        cpu->es = dos_iret_test_descriptor(vm, 4, rpl, false, false);
        cpu->fs = dos_iret_test_descriptor(vm, 5, 0, true, false);
        vm->dpmi.ldt[5].access |= 4u;
        cpu->gs = 0;
        dos_test_import_cs(vm);
        cpu8086_state_t expected = *cpu;
        /* Live tables disagree with loaded rights. IRET must use the caches
         * for the old SS and for invalidating the outer data segments. */
        vm->dpmi.ldt[2] = (dpmi_descriptor_t){0};
        vm->dpmi.ldt[3].access |= DESC_DPL3;
        vm->dpmi.ldt[4] = (dpmi_descriptor_t){0};
        vm->dpmi.ldt[5] = (dpmi_descriptor_t){0};
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_ZF | (allow_if ? FLAG_IOPL_MASK : 0);
        uint32_t frame[] = { 0x1800, cs, FLAG_CF | FLAG_DF | FLAG_IOPL_MASK | FLAG_RF,
                             0xCAFE8123u, 0xABCD0000u | ss };
        dos_iret_test_frame(vm, 0x29000, width, frame, 5);
        expected.eflags = FLAG_CF | FLAG_DF | FLAGS_FIXED |
            ((allow_if || !cpl) ? 0 : FLAG_IF) |
            ((!cpl || allow_if) ? FLAG_IOPL_MASK : 0) | (width == 4 ? FLAG_RF : 0);
        uint32_t pointer = width == 4 ? frame[3] : (uint16_t)frame[3];
        expected.esp = new32 ? pointer : (expected.esp & 0xFFFF0000u) | (uint16_t)pointer;
        expected.cs = cs; expected.ss = ss; expected.ds = expected.gs = 0;
        bool ok = cpu8086_run_one(vm) && dos_segment_test_result(vm, &expected, 0x1800, 0) &&
            cpu8086_cpl(cpu) == rpl && cpu->op_size_32 == (mode != 2) &&
            !cpu->ds_cache.valid && !cpu->gs_cache.valid &&
            dos_system_cache_equal(&cpu->es_cache, &expected.es_cache) &&
            dos_system_cache_equal(&cpu->fs_cache, &expected.fs_cache) &&
            dpmi_desc_get_base(&cpu->ss_cache.descriptor) == 0x30000 &&
            cpu_stack_addr32(cpu) == (new32 != 0) &&
            (vm->dpmi.ldt[12].access & DESC_ACCESSED) && (vm->dpmi.ldt[13].access & DESC_ACCESSED);
        if (!ok) {
            if (failures < 8) { serial_puts("[DOS-IRET-OUTER] case="); serial_putdec(checks);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" flags="); serial_puthex(cpu->eflags, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-IRET-OUTER] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_iret_fault_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    int failures = 0;
    unsigned checks = 0, faults = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned scenario = 0; scenario < 19; scenario++) {
        unsigned cpl = scenario == 7 ? 3u : 0;
        bool outer = scenario >= 9 && scenario <= 15;
        unsigned rpl = outer ? 3u : cpl;
        dos_iret_test_prepare(vm, cpu, mode, stack32, width, cpl);
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_OF;
        uint16_t cs = dos_iret_test_descriptor(vm, 12, rpl, true, mode != 2);
        uint16_t ss = dos_iret_test_descriptor(vm, 13, rpl, false, stack32);
        uint32_t frame[] = { 0x1800, cs, FLAGS_FIXED, 0x8000, ss };
        unsigned vector = 0, error = 0;
        switch (scenario) {
        case 1: vm->mem[0x1000] = 0xF0; vm->mem[0x1001] = 0xCF; vector = 6; break;
        case 2: dpmi_desc_set_limit(&cpu->ss_cache.descriptor, 0x9000u + width * 3u - 2u); vector = 12; break;
        case 3: frame[1] = 3; vector = 13; break;
        case 4: vm->dpmi.ldt[12].access &= ~DESC_PRESENT; vector = 11; error = cs & ~3u; break;
        case 5: vm->dpmi.ldt[12].access &= ~DESC_CODE; vector = 13; error = cs & ~3u; break;
        case 6: vm->dpmi.ldt[12].access |= 0x20u; vector = 13; error = cs & ~3u; break;
        case 7: frame[1] &= ~3u; vector = 13; error = cs & ~3u; break;
        case 8: dpmi_desc_set_limit(&vm->dpmi.ldt[12], 0x17FF); vector = 13; break;
        case 9: frame[4] = 0; vector = 13; break;
        case 10: vm->dpmi.ldt[13].access &= ~DESC_PRESENT; vector = 12; error = ss & ~3u; break;
        case 11: vm->dpmi.ldt[13].access &= ~DESC_WRITABLE; vector = 13; error = ss & ~3u; break;
        case 12: frame[4] &= ~3u; vector = 13; error = ss & ~3u; break;
        case 13: vm->dpmi.ldt[13].access &= ~DESC_DPL_MASK; vector = 13; error = ss & ~3u; break;
        case 14: vm->dpmi.ldt[13].access &= ~DESC_SEGMENT; vector = 13; error = ss & ~3u; break;
        case 15: dpmi_desc_set_limit(&cpu->ss_cache.descriptor, 0x9000u + width * 5u - 2u); vector = 12; break;
        case 16: cpu->ss_cache.descriptor.access |= 4u;
                 dpmi_desc_set_limit(&cpu->ss_cache.descriptor, 0x9000); vector = 12; break;
        case 17: dpmi_desc_set_limit(&cpu->ss_cache.descriptor, 0x9000u + width * 3u - 1u); break;
        case 18: cpu->esp = stack32 ? 0xFFFFFFFEu : 0xABCDFFFEu; vector = 12; break;
        }
        dos_iret_test_frame(vm, 0x29000, width, frame, 5);
        cpu8086_state_t original = *cpu, expected = original;
        bool ok = cpu8086_run_one(vm);
        if (vector) {
            if (!dos_segment_test_fault(vm, &original, vector, error) || cpu->cr2 != original.cr2) ok = false;
            faults++;
        } else {
            expected.cs = cs; expected.esp += width * 3u; expected.eflags = FLAGS_FIXED;
            if (!dos_segment_test_result(vm, &expected, 0x1800, 0)) ok = false;
        }
        for (unsigned n = 0; n < 5; n++) {
            uint32_t value = width == 4 ? dos_mem_read32(vm, 0x29000 + n * width)
                                        : dos_mem_read16(vm, 0x29000 + n * width);
            if (value != (width == 4 ? frame[n] : (uint16_t)frame[n])) ok = false;
        }
        if (!ok) {
            if (failures < 8) { serial_puts("[DOS-IRET-FAULT] case="); serial_putdec(checks);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    for (unsigned mode = 1; mode <= 2; mode++) {
        dos_iret_test_prepare(vm, cpu, mode, false, 2, 0);
        cpu->eflags |= FLAG_NT;
        cpu8086_state_t original = *cpu;
        if (!cpu8086_run_one(vm) || !dos_segment_test_fault(vm, &original, 10, 0)) failures++;
        faults++;
        checks++;
    }
    serial_puts("[DOS-IRET-FAULT] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static bool dos_iret_test_v86_result(dos_vm_t *vm, const uint32_t *frame)
{
    cpu8086_state_t *cpu = vm->cpu;
    const uint16_t selectors[] = { frame[5], frame[1], frame[4], frame[6], frame[7], frame[8] };
    const uint16_t actual[] = { cpu->es, cpu->cs, cpu->ss, cpu->ds, cpu->fs, cpu->gs };
    if (cpu->protected_mode || cpu->pm_cs_loaded || cpu->op_size_32 || cpu->addr_size_32 ||
        cpu8086_cpl(cpu) != 3 || cpu->cpl != 3 || cpu->eip != (uint16_t)frame[0] ||
        cpu->esp != frame[3] || cpu->eflags != ((frame[2] & 0x003F7FD5u) | FLAGS_FIXED) ||
        vm->dpmi.exception_depth) return false;
    for (unsigned s = 0; s < 6; s++) {
        dpmi_descriptor_t *d = &cpu->segment_cache[s].descriptor;
        if (actual[s] != selectors[s] || !cpu->segment_cache[s].valid ||
            dpmi_desc_get_base(d) != (uint32_t)selectors[s] << 4 || dpmi_desc_get_limit(d) != 0xFFFF ||
            (d->flags_lim & DESC_32BIT) || (d->access & DESC_DPL_MASK) != DESC_DPL3) return false;
    }
    return true;
}

static int dos_iret_v86_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    int failures = 0;
    unsigned checks = 0, faults = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned high = 0; high < 2; high++) {
        dos_iret_test_prepare(vm, cpu, mode, stack32, 4, 0);
        const uint32_t frame[] = { (high ? 0xABCD0000u : 0) | 0x1800, 0x1234,
            FLAG_VM | FLAG_IOPL_MASK | FLAG_IF | FLAG_NT | FLAG_RF | FLAG_AC | FLAG_VIF | FLAG_VIP | FLAG_ID,
            0xCAFE9000, 0x2345, 0, 0x3456, 0x4567, 0x5678 };
        dos_iret_test_frame(vm, 0x29000, 4, frame, 9);
        bool ok = cpu8086_run_one(vm) && dos_iret_test_v86_result(vm, frame);
        /* A subsequent IRET in v86 does not change IOPL, VIP/VIF or VM. */
        unsigned p = ((uint32_t)cpu->cs << 4) + cpu->eip;
        if (width == 4) vm->mem[p++] = 0x66;
        vm->mem[p] = 0xCF;
        const uint32_t back[] = { 0x2468, 0x1357, FLAG_CF | FLAG_DF };
        dos_iret_test_frame(vm, ((uint32_t)cpu->ss << 4) + cpu->sp, width, back, 3);
        uint32_t expected_flags = FLAGS_FIXED | FLAG_VM | FLAG_IOPL_MASK | FLAG_VIF | FLAG_VIP |
            FLAG_CF | FLAG_DF | (width == 2 ? FLAG_RF | FLAG_AC | FLAG_ID : 0);
        if (!cpu8086_run_one(vm) || cpu->cs != 0x1357 || cpu->eip != 0x2468 ||
            cpu->esp != 0xCAFE9000u + 3u * width || cpu->eflags != expected_flags ||
            cpu->protected_mode || cpu8086_cpl(cpu) != 3 || cpu->cpl != 3) ok = false;
        if (!ok) { if (failures < 8) { serial_puts("[DOS-IRET-V86] case="); serial_putdec(checks);
            serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n"); } failures++; }
        checks++;
    }
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned iopl = 0; iopl < 3; iopl++)
    for (unsigned locked = 0; locked < 2; locked++) {
        dos_iret_test_prepare(vm, cpu, 0, false, width, 0);
        cpu->cr0 |= 1;
        cpu->eflags = FLAGS_FIXED | FLAG_VM | (iopl << 12);
        cpu8086_load_real_cs(cpu, 0);
        for (unsigned s = 0; s < 6; s++) if (s != 1)
            cpu8086_load_real_segment(cpu, s, s == 2 ? 0x2000 : 0);
        if (locked) { vm->mem[0x1000] = 0xF0; vm->mem[0x1001] = 0xCF; }
        dpmi_desc_set_limit(&cpu->ss_cache.descriptor, 0);
        cpu8086_state_t original = *cpu;
        /* Observe admission before delivery: this deliberately invalid SS
         * cannot itself hold the subsequent exception frame. */
        cpu_event_fault_t fault = {0};
        cpu->delivery_fault = &fault;
        bool ok = cpu8086_run_one(vm);
        cpu->delivery_fault = NULL;
        if (!ok || !fault.raised || fault.vector != (locked ? 6u : 13u) ||
            fault.error || fault.return_eip != original.eip || !cpu->running ||
            cpu->esp != original.esp || cpu->cs != original.cs ||
            cpu->eflags != original.eflags || !dos_test_general_registers(cpu, &original)) failures++;
        checks++; faults++;
    }
    serial_puts("[DOS-IRET-V86] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_iret_paging_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    int failures = 0;
    unsigned checks = 0, faults = 0, retries = 0, snapshots = 0;
    const uint32_t base = 0x403FF000u;
    uint8_t *bytes = dos_host_alloc_pages(2);
    if (!bytes) return 1;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned kind = 0; kind < 4; kind++)
    for (unsigned scenario = 0; scenario < 6; scenario++) {
        if (kind == 3 && width == 2) continue;
        bool outer = kind == 2, v86 = kind == 3, descriptor = kind == 1 || outer;
        unsigned cpl = outer || v86 ? 0u : 3u;
        dos_iret_test_prepare(vm, cpu, mode, true, width, cpl);
        cpu->eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_CF;
        uint16_t cs = dos_iret_test_descriptor(vm, 12, outer ? 3u : cpl, true, mode != 2);
        uint16_t ss = dos_iret_test_descriptor(vm, 13, 3, false, true);
        uint32_t frame[] = { 0x1800, kind == 1 ? 11u : cs,
            FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | (v86 ? FLAG_VM : 0),
            0x8000, outer ? 11u : ss, 0x2345, 0x3456, 0x4567, 0x5678 };
        if (v86) { frame[1] = 0x1234; frame[4] = 0x456; }
        if (descriptor) { cpu->gdtr.base = base + 4095u - 8u; cpu->gdtr.limit = 15; }
        else {
            dpmi_desc_set_base(&cpu->ss_cache.descriptor, base);
            dpmi_desc_set_base(&vm->dpmi.ldt[2], base);
            cpu->esp = v86 ? 4096u - 8u * 4u : 4095u - 2u * width;
        }
        dos_test_page_pair_t pair = dos_test_page_pair(vm, base);
        for (unsigned b = 0; b < 8192; b++) bytes[b] = 0xA5;
        unsigned count = v86 ? 9u : outer ? 5u : 3u;
        if (descriptor) {
            dos_iret_test_frame(vm, 0x29000, width, frame, count);
            dpmi_descriptor_t d = vm->dpmi.ldt[outer ? 13 : 12];
            if (scenario == 5) d.access |= DESC_ACCESSED;
            for (unsigned b = 0; b < 8; b++) bytes[4095u + b] = ((uint8_t *)&d)[b];
        } else for (unsigned n = 0; n < count; n++) for (unsigned b = 0; b < width; b++)
            bytes[cpu->esp + n * width + b] = frame[n] >> (b * 8u);
        for (unsigned b = 0; b < 8192; b++) vm->mem[dos_string_test_physical(b)] = bytes[b];
        unsigned page = scenario == 1 ? 0u : 1u;
        unsigned permission = scenario < 3 ? 0u : scenario == 3 ? 2u : 1u;
        uint32_t entry = pair.pte[page], saved_entry = dos_mem_read32(vm, entry);
        if (scenario) dos_mem_write32(vm, entry, saved_entry & ~(1u << permission));
        bool failed = scenario && (!permission || (!descriptor && cpl == 3 && permission == 2) ||
                                    (descriptor && scenario == 4));
        uint32_t address = base + (page ? 4096u : descriptor ? 4095u : v86 ? cpu->esp + 8u : cpu->esp + 2u * width);
        unsigned error = descriptor || !cpl ? 0u : 4u;
        if (permission) error |= 1u;
        if (descriptor && scenario == 4) { address = base + 4100u; error = 3; }
        cpu8086_state_t original = *cpu, expected = original;
        expected.cs = frame[1]; expected.eflags = frame[2];
        expected.esp = outer ? width == 4 ? frame[3] : (uint16_t)frame[3] : original.esp + 3u * width;
        if (outer) expected.ss = 11;
        bool ok = cpu8086_run_one(vm);
        if (failed) {
            if (!dos_segment_test_fault(vm, &original, 14, error) || cpu->cr2 != address) ok = false;
            for (unsigned b = 0; b < 8192; b++) if (vm->mem[dos_string_test_physical(b)] != bytes[b]) ok = false;
            faults++;
            dos_mem_write32(vm, entry, saved_entry);
            if (cpl == 3) {
                vm->mem[0x60E0] = 0xCB;
                vm->step_limit = vm->step_count + 16u;
                if (!dos_test_run_until(vm, true, original.cs, original.eip)) ok = false;
                retries++;
            } else {
                /* DPMI cannot return a client to CPL0. This snapshot tests
                 * the repaired instruction, not the unfinished guest IDT. */
                original.cr2 = address;
                *cpu = original;
                vm->dpmi.exception_depth = 0;
                vm->dpmi.exception_esp_high[0] = 0;
                vm->dpmi.virtual_interrupts_enabled = true;
                snapshots++;
            }
            vm->step_limit = vm->step_count + 16u;
            expected.cr2 = address;
            if (!cpu8086_run_one(vm)) ok = false;
        }
        if (v86) { if (!dos_iret_test_v86_result(vm, frame)) ok = false; }
        else if (!dos_segment_test_result(vm, &expected, 0x1800, 0) ||
                 cpu->op_size_32 != (mode != 2)) ok = false;
        if (descriptor) bytes[4100] |= DESC_ACCESSED;
        for (unsigned b = 0; b < 8192; b++) if (vm->mem[dos_string_test_physical(b)] != bytes[b]) ok = false;
        if (!ok) {
            if (failures < 12) { serial_puts("[DOS-IRET-PAGING] case="); serial_putdec(checks);
                serial_puts(" kind="); serial_putdec(kind); serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8);
                serial_puts(" cr2="); serial_puthex(cpu->cr2, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    dos_host_free_pages(bytes, 2);
    serial_puts("[DOS-IRET-PAGING] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" snapshots="); serial_putdec(snapshots);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static dpmi_descriptor_t dos_idt_test_descriptor(dos_vm_t *vm, unsigned index,
                                                  uint32_t base, uint32_t limit,
                                                  uint8_t access, bool wide)
{
    dpmi_descriptor_t d = { .access = access, .flags_lim = wide ? DESC_32BIT : 0 };
    dpmi_desc_set_base(&d, base);
    dpmi_desc_set_limit(&d, limit);
    for (unsigned i = 0; i < 8; i++) dos_mem_write8(vm, vm->cpu->gdtr.base + index * 8u + i,
                                                    ((const uint8_t *)&d)[i]);
    return d;
}

static void dos_idt_test_gate(dos_vm_t *vm, unsigned vector, uint16_t selector,
                               uint32_t offset, uint8_t access)
{
    uint32_t at = vm->cpu->idtr.base + vector * 8u;
    dos_mem_write16(vm, at, (uint16_t)offset);
    dos_mem_write16(vm, at + 2, selector);
    dos_mem_write8(vm, at + 4, 0);
    dos_mem_write8(vm, at + 5, access);
    dos_mem_write16(vm, at + 6, offset >> 16);
}

static void dos_idt_test_prepare(dos_vm_t *vm, cpu8086_state_t *cpu, bool wide,
                                  bool stack32, unsigned cpl)
{
    dos_fetch_test_prepare(vm, cpu, wide ? 2u : 1u);
    vm->dpmi.active = false;
    vm->emulate_cpu = true;
    vm->step_limit = 64;
    cpu->guest_idt = true;
    cpu->gdtr.base = 0x2000; cpu->gdtr.limit = 0xFF;
    cpu->idtr.base = 0x3000; cpu->idtr.limit = 0x7FF;
    cpu8086_cache_ldtr(cpu, 0, NULL);
    dpmi_descriptor_t code = dos_idt_test_descriptor(vm, 1, 0, 0xFFFF, 0x9Bu | (cpl << 5), wide);
    dpmi_descriptor_t stack = dos_idt_test_descriptor(vm, 2, 0x20000,
        stack32 ? UINT32_MAX : 0xFFFFu, 0x93u | (cpl << 5), stack32);
    cpu8086_cache_cs(cpu, 8u | cpl, &code, cpl);
    cpu8086_cache_segment(cpu, 2, 16u | cpl, &stack);
    for (unsigned s = 0; s < 6; s++) if (s != 1 && s != 2)
        cpu8086_cache_segment(cpu, s, 16u | cpl, &stack);
    cpu->esp = (stack32 ? 0 : 0xABCD0000u) | 0x9000;
    cpu->eip = 0x1000;
    cpu->eflags = FLAGS_FIXED | FLAG_CF | FLAG_IF | FLAG_DF | FLAG_OF | FLAG_IOPL_MASK;
    cpu->irq_shadow = 0;
    dos_io_write8(vm, 0x21, 0xFF);
    dos_io_write8(vm, 0xA1, 0xFF);
    dos_idt_test_descriptor(vm, 3, 0x60000, 0xFFFF, 0x9A, wide);
    dos_idt_test_descriptor(vm, 4, 0x30000, stack32 ? UINT32_MAX : 0xFFFFu, 0xB2, stack32);
    dos_idt_test_descriptor(vm, 5, 0x60000, 0xFFFF, 0x9E, true);
    dos_idt_test_descriptor(vm, 6, 0x50000, 0xFFFF, 0x92, true);
    dpmi_descriptor_t tss = dos_idt_test_descriptor(vm, 7, 0x4000, 103, 0x8B, false);
    cpu8086_cache_tr(cpu, 56, &tss);
    for (unsigned level = 0; level < 3; level++) {
        dos_mem_write32(vm, 0x4004 + level * 8, 0x9000);
        dos_mem_write16(vm, 0x4008 + level * 8, level ? 32u | level : 48u);
    }
    for (unsigned vector = 0; vector < 256; vector++)
        dos_idt_test_gate(vm, vector, 40, 0x4000 + vector * 16u, 0xEE);
    for (unsigned i = 0; i < 64; i++) {
        vm->mem[0x28FC0 + i] = 0xA5;
        vm->mem[0x38FC0 + i] = 0x5A;
    }
    vm->mem[0x1000] = 0xCD; vm->mem[0x1001] = 0x40;
}

static uint32_t dos_idt_test_read(dos_vm_t *vm, uint32_t address, unsigned width)
{
    return width == 4 ? dos_mem_read32(vm, address) : dos_mem_read16(vm, address);
}

static bool dos_idt_test_fault(dos_vm_t *vm, const cpu8086_state_t *before,
                                unsigned vector, uint32_t error, uint32_t return_eip,
                                uint16_t handler)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t at = 0x20000 + ((before->esp - 16u) & (cpu_stack_addr32(before) ? UINT32_MAX : 0xFFFFu));
    uint32_t flags = before->eflags | ((vector != 1 && vector != 3 && vector != 4 && vector != 8) ? FLAG_RF : 0);
    return cpu->running && !cpu->delivery_fault && !vm->dpmi.exception_depth &&
        cpu->cs == (handler | cpu8086_cpl(before)) && cpu->eip == 0x4000 + vector * 16u &&
        cpu->esp == before->esp - 16u && cpu->ss == before->ss &&
        cpu8086_cpl(cpu) == cpu8086_cpl(before) && dos_test_general_registers(cpu, before) &&
        dos_mem_read32(vm, at) == error && dos_mem_read32(vm, at + 4) == return_eip &&
        dos_mem_read32(vm, at + 8) == before->cs && dos_mem_read32(vm, at + 12) == flags;
}

static void dos_task_test_image(dos_vm_t *vm, uint32_t base, bool wide,
                                  const cpu8086_state_t *state)
{
    unsigned width = wide ? 4u : 2u, start = wide ? 32u : 14u;
    for (unsigned i = 0; i < 104; i++) vm->mem[base + i] = 0xA5;
    const uint32_t fields[] = { state->eip, state->eflags, state->eax, state->ecx,
        state->edx, state->ebx, state->esp, state->ebp, state->esi, state->edi };
    for (unsigned i = 0; i < 10; i++) dos_far_test_field(vm, base + start + i * width, width, fields[i]);
    const uint16_t segments[] = { state->es, state->cs, state->ss, state->ds, state->fs, state->gs };
    for (unsigned i = 0; i < (wide ? 6u : 4u); i++)
        dos_mem_write16(vm, base + start + (i + 10u) * width, segments[i]);
    dos_mem_write16(vm, base + (wide ? 96u : 42u), state->ldtr);
    if (wide) {
        dos_mem_write32(vm, base + 28, state->cr3);
        dos_mem_write16(vm, base + 100, 0);
        dos_mem_write16(vm, base + 102, 104);
    }
}

static bool dos_task_test_saved(dos_vm_t *vm, uint32_t base, bool wide,
                                  const cpu8086_state_t *state, uint32_t ip, uint32_t flags)
{
    unsigned width = wide ? 4u : 2u, start = wide ? 32u : 14u;
    uint32_t mask = wide ? UINT32_MAX : 0xFFFFu;
    const uint32_t fields[] = { ip, flags, state->eax, state->ecx, state->edx,
        state->ebx, state->esp, state->ebp, state->esi, state->edi };
    for (unsigned i = 0; i < 10; i++)
        if (dos_idt_test_read(vm, base + start + i * width, width) != (fields[i] & mask)) return false;
    const uint16_t segments[] = { state->es, state->cs, state->ss, state->ds, state->fs, state->gs };
    for (unsigned i = 0; i < (wide ? 6u : 4u); i++)
        if (dos_mem_read16(vm, base + start + (i + 10u) * width) != segments[i]) return false;
    return true;
}

static cpu8086_state_t dos_task_test_prepare(dos_vm_t *vm, cpu8086_state_t *cpu,
                                             bool old_wide, bool wide, bool code32,
                                             bool stack32, unsigned from, unsigned to)
{
    dos_idt_test_prepare(vm, cpu, code32, stack32, from);
    dpmi_descriptor_t old = dos_idt_test_descriptor(vm, 7, 0x4000,
        old_wide ? 103u : 43u, old_wide ? 0x8B : 0x83, false);
    cpu8086_cache_tr(cpu, 56, &old);
    cpu->cr3 = 0x12000;
    cpu->dr[7] = 0xAAFF;
    dos_task_test_image(vm, 0x4000, old_wide, cpu);
    dos_idt_test_descriptor(vm, 8, 0x5000, wide ? 103u : 43u,
        (wide ? 0x89 : 0x81) | DESC_DPL3, false);
    dos_idt_test_descriptor(vm, 9, 0x60000, 0xFFFF, 0x9Au | (to << 5), !code32);
    dos_idt_test_descriptor(vm, 10, 0x70000, stack32 ? UINT32_MAX : 0xFFFFu,
        0x92u | (to << 5), stack32);
    dos_idt_test_descriptor(vm, 11, 0x80000, 0xFFFF, 0x92u | (to << 5), true);
    /* A flat readable segment for the indirect far-pointer operand. */
    dpmi_descriptor_t flat = dos_idt_test_descriptor(vm, 15, 0, 0xFFFF, 0xF3, false);
    cpu8086_cache_segment(cpu, 3, 120u | from, &flat);
    cpu8086_state_t next = *cpu;
    next.cs = 72u | to; next.ss = 80u | to;
    next.es = next.ds = next.fs = next.gs = 88u | to;
    next.ldtr = 0;
    next.eip = 0x1800;
    next.esp = 0x8000;
    next.eflags = FLAGS_FIXED | FLAG_IF | FLAG_IOPL_MASK | FLAG_PF | FLAG_ZF;
    next.eax = wide ? 0xBAADF00D : 0xF00D;
    next.ecx = 0x2222; next.edx = 0x3333; next.ebx = 0x4444;
    next.ebp = 0x5555; next.esi = 0x6666; next.edi = 0x7777;
    next.cr3 = 0x15000;
    dos_task_test_image(vm, 0x5000, wide, &next);
    return next;
}

static int dos_task_roundtrip_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, returns = 0;
    int failures = 0;
    for (unsigned old_wide = 0; old_wide < 2; old_wide++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned code32 = 0; code32 < 2; code32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned source = 0; source < 8; source++)
    for (unsigned cpl = 0; cpl < 4; cpl++) {
        unsigned to = (cpl + 1u) & 3u, width = wide ? 4u : 2u;
        cpu8086_state_t next = dos_task_test_prepare(vm, cpu, old_wide, wide, code32, stack32, cpl, to);
        uint16_t selector = 67;
        if (source == 2 || source == 3) {
            dpmi_descriptor_t gate = { .access = 0xE5, .base_lo = 64 };
            uint32_t at = 0x2000 + 104;
            if (code32) {
                dpmi_descriptor_t ldt = dos_idt_test_descriptor(vm, 12, 0x6000, 0xFF, 0x82, false);
                cpu8086_cache_ldtr(cpu, 96, &ldt);
                dos_mem_write16(vm, 0x4000 + (old_wide ? 96u : 42u), 96);
                at = 0x6008; selector = 15;
            } else selector = 107;
            for (unsigned i = 0; i < 8; i++) vm->mem[at + i] = ((uint8_t *)&gate)[i];
            vm->mem[0x2045] &= ~DESC_DPL_MASK; /* Gate access ignores target TSS DPL. */
        }
        uint32_t return_eip = 0x1000;
        if (source < 4) return_eip = dos_far_test_code(vm, source == 0 ? 0 : source == 1 ? 2 :
            source == 2 ? 1 : 3, code32 ? 2u : 4u, !code32, selector, 0xFFFFFFFFu, 0, false);
        else dos_idt_test_gate(vm, source < 6 ? 0x40 : source == 6 ? 6 : 13, 64, 0xFFFFFFFFu, 0xE5);
        if (source == 4) return_eip += 2;
        cpu8086_state_t before = *cpu;
        uint8_t original[104];
        for (unsigned i = 0; i < 104; i++) original[i] = vm->mem[0x4000 + i];
        bool ok = source < 5 ? cpu8086_run_one(vm) : source == 5 ? cpu_deliver_hw_interrupt(vm, 0x40)
            : cpu_deliver_exception(vm, source == 6 ? 6 : 13, 0x1000, 0xBEEF, source == 7);
        bool jump = source == 1 || source == 3;
        uint32_t saved_flags = before.eflags | (source >= 6 ? FLAG_RF : 0);
        uint32_t flags = next.eflags | (jump ? 0 : FLAG_NT);
        uint16_t expected_tr = source < 2 ? 67 : 64;
        if (!ok || !cpu->running || cpu->delivery_fault || cpu->tr != expected_tr ||
            cpu->eip != next.eip || cpu->cs != next.cs || cpu->ss != next.ss ||
            cpu->esp != next.esp - (source == 7 ? width : 0) || cpu8086_cpl(cpu) != to ||
            cpu->eflags != flags || !dos_test_general_registers(cpu, &next) ||
            cpu->cr3 != before.cr3 || !(cpu->cr0 & 8u) || cpu->dr[7] != (before.dr[7] & ~0x55u) ||
            cpu->op_size_32 != !code32 || cpu_stack_addr32(cpu) != (bool)stack32 ||
            !cpu->guest_idt || cpu->host_ldt || cpu->ldtr || vm->dpmi.exception_depth ||
            !dos_task_test_saved(vm, 0x4000, old_wide, &before, return_eip, saved_flags) ||
            (vm->mem[0x203D] & 2u) != (jump ? 0u : 2u) || !(vm->mem[0x2045] & 2u) ||
            dos_mem_read16(vm, 0x5000) != (jump ? 0xA5A5u : before.tr)) ok = false;
        if (cpu->es != next.es || cpu->ds != next.ds || cpu->fs != (wide ? next.fs : 0) ||
            cpu->gs != (wide ? next.gs : 0) || !(vm->mem[0x204D] & 1u) ||
            !(vm->mem[0x2055] & 1u) || !(vm->mem[0x205D] & 1u)) ok = false;
        if (source == 7 && dos_idt_test_read(vm, 0x78000 - width, width) != 0xBEEF) ok = false;
        for (unsigned i = 0; i < 104; i++) {
            bool dynamic = old_wide ? (i >= 32 && i < 72) || (i >= 72 && i < 94 && (i & 3u) < 2)
                                    : i >= 14 && i < 42;
            if (!dynamic && vm->mem[0x4000 + i] != original[i]) ok = false;
        }
        if (!jump && ok) {
            cpu8086_state_t child = *cpu;
            vm->mem[0x61800] = 0xCF;
            /* Task IRET uses NT/backlink, not the current stack frame. */
            cpu->ss_cache.valid = false;
            bool returned = cpu8086_run_one(vm);
            cpu8086_state_t expected = before;
            if (!old_wide) {
                expected.eax &= 0xFFFF; expected.ebx &= 0xFFFF; expected.ecx &= 0xFFFF;
                expected.edx &= 0xFFFF; expected.ebp &= 0xFFFF; expected.esi &= 0xFFFF; expected.edi &= 0xFFFF;
            }
            if (!returned || cpu->tr != before.tr || cpu->cs != before.cs || cpu->eip != return_eip ||
                cpu->ss != before.ss || cpu->esp != (old_wide ? before.esp : before.sp) ||
                cpu->eflags != (saved_flags & (old_wide ? UINT32_MAX : 0xFFFFu)) ||
                cpu8086_cpl(cpu) != cpl || cpu->op_size_32 != (bool)code32 ||
                !dos_test_general_registers(cpu, &expected) || cpu->ldtr != before.ldtr ||
                (vm->mem[0x2045] & 2u) || !(vm->mem[0x203D] & 2u) ||
                !dos_task_test_saved(vm, 0x5000, wide, &child, 0x1801, child.eflags & ~FLAG_NT)) ok = false;
            returns++;
        }
        if (!ok) {
            if (failures < 12) { serial_puts("[DOS-TASK-ROUNDTRIP] case="); serial_putdec(checks);
                serial_puts(" source="); serial_putdec(source); serial_puts(" TR="); serial_puthex(cpu->tr, 4);
                serial_puts(" CS:IP="); serial_puthex(cpu->cs, 4); serial_puts(":"); serial_puthex(cpu->eip, 8);
                serial_puts(" flags="); serial_puthex(cpu->eflags, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-TASK-ROUNDTRIP] checks="); serial_putdec(checks);
    serial_puts(" returns="); serial_putdec(returns);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_task_admission_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned source = 0; source < 6; source++)
    for (unsigned scenario = 0; scenario < 16; scenario++) {
        if (scenario == 12 && source >= 4) continue;
        if (scenario == 13 && source >= 2) continue;
        if (scenario >= 14 && source != 2) continue;
        (void)dos_task_test_prepare(vm, cpu, wide, wide, !wide, true, 3, 1);
        bool task_return = source == 5;
        unsigned vector = task_return ? 10u : 13u, error = 64;
        uint16_t selector = 67;
        if (task_return) {
            cpu->eflags |= FLAG_NT;
            vm->mem[0x2045] |= 2u;
        }
        switch (scenario) {
        case 0: selector = 3; error = 0; break;
        case 1: selector |= 4; error = 68; break;
        case 2: selector = 0xFFFF; error = 0xFFFC; break;
        case 3: vm->mem[0x2045] = 0x82; break;
        case 4: vm->mem[0x2045] ^= 2u; break;
        case 5: vm->mem[0x2045] &= ~DESC_PRESENT; vector = 11; break;
        case 6: dos_mem_write16(vm, 0x2040, wide ? 102 : 42); vector = 10; break;
        case 7: cpu->tss_cache.valid = false; vector = 10; error = 56; break;
        case 8: cpu->tr = 0; vector = 10; error = 0; break;
        case 9: cpu->tr |= 4; vector = 10; error = 60; break;
        case 10: cpu->tss_cache.descriptor.access = 0x82; vector = 10; error = 56; break;
        case 11: dpmi_desc_set_limit(&cpu->tss_cache.descriptor, wide ? 102 : 42); vector = 10; error = 56; break;
        case 12: vector = 6; error = 0; break;
        case 13: vm->mem[0x2045] &= ~DESC_DPL_MASK; break;
        case 14: error = 104; break;
        case 15: vector = 11; error = 104; break;
        }
        if (source == 2) {
            dpmi_descriptor_t gate = { .base_lo = selector,
                .access = scenario == 14 ? 0x85 : scenario == 15 ? 0x65 : 0xE5 };
            for (unsigned i = 0; i < 8; i++) vm->mem[0x2068 + i] = ((uint8_t *)&gate)[i];
            selector = 107;
        }
        if (source < 3) (void)dos_far_test_code(vm, source == 1 ? 2 : 0, wide ? 4u : 2u,
            wide, selector, 0xFFFFFFFFu, 0, scenario == 12);
        else if (task_return) {
            dos_mem_write16(vm, 0x4000, selector);
            vm->mem[0x1000] = 0xCF;
        } else {
            dos_idt_test_gate(vm, 0x40, selector, 0, 0xE5);
            if (scenario == 12) { vm->mem[0x1000] = 0xF0; vm->mem[0x1001] = 0xCD; vm->mem[0x1002] = 0x40; }
        }
        cpu8086_state_t before = *cpu;
        uint8_t old[104], incoming[104];
        for (unsigned i = 0; i < 104; i++) { old[i] = vm->mem[0x4000 + i]; incoming[i] = vm->mem[0x5000 + i]; }
        bool ok = source == 4 ? cpu_deliver_hw_interrupt(vm, 0x40) : cpu8086_run_one(vm);
        if (source == 4) error |= 1u;
        unsigned frame_size = vector == 6 ? 12u : 16u;
        uint32_t at = 0x20000 + before.esp - frame_size;
        if (!ok || !cpu->running || cpu->cs != 43 || cpu->eip != 0x4000 + vector * 16 ||
            cpu->esp != before.esp - frame_size || cpu->tr != before.tr || cpu->cr0 != before.cr0 ||
            cpu->cr3 != before.cr3 || !dos_test_general_registers(cpu, &before) ||
            cpu->ldtr != before.ldtr || cpu->delivery_fault || vm->dpmi.exception_depth) ok = false;
        if (vector != 6) { if (dos_mem_read32(vm, at) != error) ok = false; at += 4; }
        if (dos_mem_read32(vm, at) != 0x1000 || dos_mem_read32(vm, at + 4) != before.cs ||
            dos_mem_read32(vm, at + 8) != (before.eflags | FLAG_RF)) ok = false;
        for (unsigned i = 0; i < 104; i++)
            if (old[i] != vm->mem[0x4000 + i] || incoming[i] != vm->mem[0x5000 + i]) ok = false;
        if (!ok) {
            if (failures < 12) { serial_puts("[DOS-TASK-ADMIT] case="); serial_putdec(checks);
                serial_puts(" source="); serial_putdec(source); serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-TASK-ADMIT] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static void dos_task_test_rescue(dos_vm_t *vm, unsigned vector, uint32_t cr3)
{
    dos_idt_test_descriptor(vm, 16, 0x9000, 103, 0x89, false);
    dos_idt_test_descriptor(vm, 17, 0x60000, 0xFFFF, 0x9B, true);
    dos_idt_test_descriptor(vm, 18, 0x90000, 0xFFFF, 0x93, true);
    cpu8086_state_t rescue = { .cs = 136, .ss = 144, .eip = 0x7000 + vector * 16,
        .esp = 0x8000, .eflags = FLAGS_FIXED, .cr3 = cr3 };
    dos_task_test_image(vm, 0x9000, true, &rescue);
    for (unsigned v = 0; v < 32; v++) dos_idt_test_gate(vm, v, v == vector ? 128 : 0, 0, 0xE5);
}

static int dos_task_postcommit_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, doubles = 0;
    int failures = 0;
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned source = 0; source < 5; source++)
    for (unsigned scenario = 0; scenario < 22; scenario++) {
        if (scenario == 18 && !wide) continue;
        if (scenario == 21 && source < 3) continue;
        cpu8086_state_t next = dos_task_test_prepare(vm, cpu, true, wide, !wide, true, 0, 3);
        unsigned vector = 10, error = 0;
        switch (scenario) {
        case 0: next.ldtr = 4; error = 4; break;
        case 1: next.ldtr = 0xFFF8; error = 0xFFF8; break;
        case 2: next.ldtr = 96; dos_idt_test_descriptor(vm, 12, 0x6000, 0xFF, 0x89, false); error = 96; break;
        case 3: next.ldtr = 96; dos_idt_test_descriptor(vm, 12, 0x6000, 0xFF, 0x02, false); error = 96; break;
        case 4: next.cs = 3; break;
        case 5: next.cs = 0xFFFF; error = 0xFFFC; break;
        case 6: vm->mem[0x204D] = 0xF2; error = 72; break;
        case 7: vm->mem[0x204D] = 0xBA; error = 72; break;
        case 8: vm->mem[0x204D] &= ~DESC_PRESENT; vector = 11; error = 72; break;
        case 9: next.ss = 3; break;
        case 10: next.ss = 0xFFFF; error = 0xFFFC; break;
        case 11: vm->mem[0x2055] = 0xFA; error = 80; break;
        case 12: vm->mem[0x2055] = 0xF0; error = 80; break;
        case 13: vm->mem[0x2055] = 0xB2; error = 80; break;
        case 14: next.ss = 81; error = 80; break;
        case 15: vm->mem[0x2055] &= ~DESC_PRESENT; vector = 12; error = 80; break;
        case 16: next.ds = 0xFFFF; error = 0xFFFC; break;
        case 17: vm->mem[0x205D] &= ~DESC_PRESENT; vector = 11; error = 88; break;
        case 18: dos_idt_test_descriptor(vm, 20, 0, 0xFFFF, 0xF8, false); next.fs = 163; error = 160; break;
        case 19: vm->mem[0x205D] = 0xB2; error = 88; break;
        case 20: next.eip = 0x10000; vector = 13; break;
        case 21: dos_idt_test_descriptor(vm, 10, 0x70000, wide ? 0x7FFC : 0x7FFE, 0xF2, true);
            vector = 12; break;
        }
        if (scenario == 20 && !wide) { dos_mem_write16(vm, 0x2048, 0x17FF); next.eip = 0x1800; }
        dos_task_test_image(vm, 0x5000, wide, &next);
        unsigned delivered = source >= 3 ? 8u : vector;
        uint32_t delivered_error = source >= 3 ? 0 : error | (source == 2 ? 1u : 0);
        dos_task_test_rescue(vm, delivered, next.cr3);
        uint32_t old_return = 0x1000;
        if (!source) old_return = dos_far_test_code(vm, 0, 4, true, 64, 0xFFFFFFFFu, 0, false);
        else if (source < 3) {
            dos_idt_test_gate(vm, 0x40, 64, 0, 0xE5);
            if (source == 1) old_return += 2;
        } else dos_idt_test_gate(vm, source == 3 ? 13 : 14, 64, 0, 0xE5);
        cpu8086_state_t before = *cpu;
        bool ok = source < 2 ? cpu8086_run_one(vm) : source == 2 ? cpu_deliver_hw_interrupt(vm, 0x40)
            : cpu_deliver_exception(vm, source == 3 ? 13 : 14, 0x1000, 0xCAFE, true);
        next.eflags |= FLAG_NT | (delivered == 8 ? 0 : FLAG_RF);
        if (!wide) next.fs = next.gs = 0;
        if (!ok || !cpu->running || cpu->tr != 128 || cpu->cs != 136 ||
            cpu->eip != 0x7000 + delivered * 16 || cpu->esp != 0x7FFC || cpu->ss != 144 ||
            dos_mem_read32(vm, 0x97FFC) != delivered_error || dos_mem_read16(vm, 0x9000) != 64 ||
            cpu->delivery_fault || vm->dpmi.exception_depth ||
            !dos_task_test_saved(vm, 0x5000, wide, &next, next.eip, next.eflags) ||
            !dos_task_test_saved(vm, 0x4000, true, &before, old_return, before.eflags | (source >= 3 ? FLAG_RF : 0))) ok = false;
        if (!ok) {
            if (failures < 12) { serial_puts("[DOS-TASK-COMMIT] case="); serial_putdec(checks);
                serial_puts(" source="); serial_putdec(source); serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" TR="); serial_puthex(cpu->tr, 4); serial_puts(" IP="); serial_puthex(cpu->eip, 8);
                serial_puts(" savedIP="); serial_puthex(dos_idt_test_read(vm, 0x5000 + (wide ? 32u : 14u), wide ? 4u : 2u), 8);
                serial_puts("\n"); }
            failures++;
        }
        if (delivered == 8) doubles++;
        checks++;
    }
    serial_puts("[DOS-TASK-COMMIT] checks="); serial_putdec(checks);
    serial_puts(" doubles="); serial_putdec(doubles);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_task_v86_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned old_wide = 0; old_wide < 2; old_wide++)
    for (unsigned source = 0; source < 3; source++)
    for (unsigned cpl = 0; cpl < 4; cpl++)
    for (unsigned iopl = 0; iopl < 2; iopl++) {
        cpu8086_state_t next = dos_task_test_prepare(vm, cpu, old_wide, true, true, true, cpl, 0);
        next.cs = 0x1000; next.ss = 0x3000; next.es = 0x2000;
        next.ds = 0x4000; next.fs = 0x5000; next.gs = 0x6000;
        next.eflags = FLAGS_FIXED | FLAG_VM | FLAG_IF | FLAG_CF | (iopl ? FLAG_IOPL_MASK : FLAG_NT);
        next.esp = 0xF00D8000;
        dos_task_test_image(vm, 0x5000, true, &next);
        if (source < 2) (void)dos_far_test_code(vm, source ? 2 : 0, 2, true, 64, 0, 0, false);
        else dos_idt_test_gate(vm, 0x40, 64, 0, 0xE5);
        bool ok = source < 2 ? cpu8086_run_one(vm) : cpu_deliver_hw_interrupt(vm, 0x40);
        if (source != 1) next.eflags |= FLAG_NT;
        uint16_t selectors[] = { next.es, next.cs, next.ss, next.ds, next.fs, next.gs };
        if (!ok || cpu->protected_mode || cpu->pm_cs_loaded || cpu->op_size_32 || cpu->addr_size_32 ||
            !cpu->guest_idt || cpu8086_cpl(cpu) != 3 || cpu->eip != next.eip ||
            cpu->esp != next.esp || cpu->eflags != next.eflags) ok = false;
        for (unsigned s = 0; s < 6; s++) {
            const cpu_system_segment_t *cache = &cpu->segment_cache[s];
            if (!cache->valid || dpmi_desc_get_base(&cache->descriptor) != (uint32_t)selectors[s] << 4 ||
                dpmi_desc_get_limit(&cache->descriptor) != 0xFFFF ||
                (cache->descriptor.access & DESC_DPL_MASK) != DESC_DPL3) ok = false;
        }
        if (ok) {
            vm->mem[0x11800] = 0xB8; vm->mem[0x11801] = 0x57; vm->mem[0x11802] = 0x13;
            ok = cpu8086_run_one(vm) && cpu->eip == 0x1803 && cpu->eax == 0xBAAD1357;
            dos_task_test_rescue(vm, 1, next.cr3);
            dos_idt_test_gate(vm, 0x40, 128, 0, 0x85);
            cpu8086_state_t suspended = *cpu;
            if (!cpu_deliver_hw_interrupt(vm, 0x40) || !cpu->protected_mode || cpu->tr != 128 ||
                cpu->eip != 0x7010 || cpu8086_cpl(cpu) != 0 || cpu->eflags != (FLAGS_FIXED | FLAG_NT) ||
                !dos_task_test_saved(vm, 0x5000, true, &suspended, 0x1803, suspended.eflags)) ok = false;
            vm->mem[0x67010] = 0xCF;
            if (ok && (!cpu8086_run_one(vm) || cpu->tr != 64 || cpu->protected_mode ||
                cpu->eip != suspended.eip || cpu->cs != suspended.cs || cpu->ss != suspended.ss ||
                cpu->esp != suspended.esp || cpu->eflags != suspended.eflags ||
                !dos_test_general_registers(cpu, &suspended))) ok = false;
        }
        if (!ok || cpu->delivery_fault) {
            if (failures < 8) { serial_puts("[DOS-TASK-V86] case="); serial_putdec(checks);
                serial_puts(" TR="); serial_puthex(cpu->tr, 4); serial_puts(" IP="); serial_puthex(cpu->eip, 8);
                serial_puts(" FLAGS="); serial_puthex(cpu->eflags, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-TASK-V86] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_task_debug_cache_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned source = 0; source < 6; source++)
    for (unsigned scenario = 0; scenario < 6; scenario++) {
        cpu8086_state_t next = dos_task_test_prepare(vm, cpu, wide, wide, true, true, 3, 3);
        if (scenario == 0) dos_mem_write16(vm, 0x5064, 1); /* No trap field in a 16-bit TSS. */
        if (scenario == 1) {
            /* The loaded TR survives editing both the live base and type. */
            dos_idt_test_descriptor(vm, 7, 0xFFFFF000u, 0, 0x8F, false);
        }
        if (scenario == 2) { next.eflags |= FLAG_NT; dos_task_test_image(vm, 0x5000, wide, &next); }
        if (scenario == 3) {
            next.ldtr = 96;
            dos_idt_test_descriptor(vm, 12, 0x6000, 0xFF, 0x82, false);
            for (unsigned i = 0; i < 24; i++) vm->mem[0x6008 + i] = vm->mem[0x2048 + i];
            next.cs = 15; next.ss = 23; next.es = next.ds = next.fs = next.gs = 31;
            dos_task_test_image(vm, 0x5000, wide, &next);
        }
        if (scenario == 4) {
            /* Conforming code and data RPLs need their own privilege checks. */
            vm->mem[0x204D] = 0x9E;
            vm->mem[0x205D] = 0x9E;
            next.ds = 88; next.es = 89; next.fs = 90; next.gs = 91;
            dos_task_test_image(vm, 0x5000, wide, &next);
        }
        if (scenario == 5) {
            next.ds = 0; next.es = 1; next.fs = 2; next.gs = 3;
            dos_task_test_image(vm, 0x5000, wide, &next);
        }
        if (source == 5) {
            cpu->eflags |= FLAG_NT;
            dos_mem_write16(vm, 0x4000, 64);
            vm->mem[0x2045] |= 2u;
        }
        uint32_t return_eip = 0x1000;
        if (source < 2) return_eip = dos_far_test_code(vm, source ? 2 : 0, 4, false, 64, 0, 0, false);
        else if (source == 5) { vm->mem[0x1000] = 0xCF; return_eip++; }
        else dos_idt_test_gate(vm, source == 2 ? 0x40 : source == 3 ? 13 : 8, 64, 0, 0xE5);
        /* A #DB handler uses the incoming stack, not the suspended caller. */
        dos_idt_test_gate(vm, 1, 40, 0x4010, 0xEF);
        cpu8086_state_t before = *cpu;
        bool ok = source < 2 || source == 5 ? cpu8086_run_one(vm) : source == 2 ? cpu_deliver_hw_interrupt(vm, 0x40)
            : cpu_deliver_exception(vm, source == 3 ? 13 : 8, 0x1000, 0, true);
        bool trap = wide && scenario == 0;
        unsigned pushed = source == 3 || source == 4 ? (wide ? 4u : 2u) : 0;
        uint32_t flags = next.eflags | (source == 1 || source == 5 ? 0 : FLAG_NT);
        uint32_t saved = source == 5 ? before.eflags & ~FLAG_NT : before.eflags | (source == 3 ? FLAG_RF : 0);
        if (!ok || !cpu->running || cpu->tr != 64 || cpu->eip != (trap ? 0x4010u : next.eip) ||
            cpu->cs != (trap ? 43u : next.cs) || cpu->esp != next.esp - pushed - (trap ? 12u : 0) ||
            cpu->eflags != (trap ? flags & ~(FLAG_NT | FLAG_RF | FLAG_VM | FLAG_TF) : flags) ||
            !dos_test_general_registers(cpu, &next) || cpu->dr[6] != (before.dr[6] | (trap ? 0x8000u : 0)) ||
            cpu->dr[7] != (before.dr[7] & ~0x55u & ~(trap ? 0x2000u : 0)) ||
            !dos_task_test_saved(vm, 0x4000, wide, &before, return_eip, saved)) ok = false;
        if (trap && (dos_mem_read32(vm, 0x77FF4 - pushed) != next.eip || dos_mem_read32(vm, 0x77FF8 - pushed) != next.cs ||
            dos_mem_read32(vm, 0x77FFC - pushed) != flags)) ok = false;
        if (scenario == 5 && (cpu->es_cache.valid || cpu->ds_cache.valid || cpu->fs_cache.valid || cpu->gs_cache.valid)) ok = false;
        if (!ok || cpu->delivery_fault) {
            if (failures < 8) { serial_puts("[DOS-TASK-DEBUG-CACHE] case="); serial_putdec(checks);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-TASK-DEBUG-CACHE] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_task_paging_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint32_t base = 0x403FF000;
    unsigned checks = 0, faults = 0, retries = 0;
    int failures = 0;
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned region = 0; region < 5; region++)
    for (unsigned scenario = 0; scenario < 9; scenario++) {
        cpu8086_state_t next = dos_task_test_prepare(vm, cpu, wide, wide, true, true, 3, 3);
        bool task_return = region == 4;
        if (task_return) { cpu->eflags |= FLAG_NT; vm->mem[0x2045] |= 2u; dos_mem_write16(vm, 0x4000, 64); }
        for (unsigned index = 9; index <= 11; index++) vm->mem[0x2000 + index * 8 + 5] |= DESC_ACCESSED;
        dpmi_descriptor_t rescue = dos_idt_test_descriptor(vm, 5, 0x60000, 0xFFFF, 0x9F, true);
        for (unsigned b = 0; b < 8; b++) vm->mem[0x6028 + b] = ((uint8_t *)&rescue)[b];
        dpmi_descriptor_t ldt = { .access = 0x82, .limit_lo = 0xFF, .base_lo = 0x6000 };
        cpu8086_cache_ldtr(cpu, 96, &ldt);
        for (unsigned v = 0; v < 32; v++) dos_idt_test_gate(vm, v, 44, 0x4000 + v * 16u, 0xEE);
        dos_test_page_pair_t pair = dos_test_page_pair(vm, base);
        next.cr3 = cpu->cr3;
        dos_task_test_image(vm, 0x5000, wide, &next);
        unsigned offset = 4096u - (wide ? 64u : 24u);
        if (region == 3) offset = 4095u - 69u;
        if (region == 4) offset = 4095u - 61u;
        for (unsigned i = 0; i < 8192; i++) vm->mem[dos_string_test_physical(i)] = 0xA5;
        uint32_t original_base = region == 0 ? 0x4000u : region < 3 ? 0x5000u : 0x2000u;
        for (unsigned i = 0; i < (region < 3 ? 104u : 256u); i++)
            vm->mem[dos_string_test_physical(offset + i)] = vm->mem[original_base + i];
        if (!region) dpmi_desc_set_base(&cpu->tss_cache.descriptor, base + offset);
        else if (region < 3) dos_idt_test_descriptor(vm, 8, base + offset, wide ? 103u : 43u,
            wide ? 0xE9 : 0xE1, false);
        else cpu->gdtr.base = base + offset;
        if (task_return) vm->mem[0x1000] = 0xCF;
        else (void)dos_far_test_code(vm, region < 2 ? 2 : 0, 4, true, 64, 0, 0, false);
        unsigned page = scenario == 2 || scenario == 4 || scenario == 6 || scenario == 8;
        unsigned bit = scenario <= 2 ? 0 : scenario <= 4 || scenario >= 7 ? 1 : 2;
        uint32_t entry = pair.pte[page], saved = dos_mem_read32(vm, entry);
        if (scenario) dos_mem_write32(vm, entry, saved & ~(1u << bit));
        if (scenario >= 7) cpu->cr0 &= ~DOS_CR0_WP;
        bool write_denied = scenario == 3 || scenario == 4;
        bool failed = (scenario == 1 || scenario == 2) ||
            (write_denied && (!region || (page == 0 && region >= 2)));
        bool write_fault = !region || (region == 4 && page == 0) || write_denied;
        uint32_t fault_linear = base + (page ? 4096u : offset + (region ? (wide ? 28u : 14u) : (wide ? 32u : 14u)));
        if (region == 2 && write_denied) fault_linear = base + offset;
        if (region == 3) fault_linear = base + (write_denied ? 4095u : page ? 4096u : 4090u);
        if (region == 4) fault_linear = base + (page ? 4098u : 4095u);
        uint8_t old[104], incoming[104];
        for (unsigned i = 0; i < 104; i++) {
            old[i] = vm->mem[region == 0 ? dos_string_test_physical(offset + i) : 0x4000 + i];
            incoming[i] = vm->mem[region == 1 || region == 2 ? dos_string_test_physical(offset + i) : 0x5000 + i];
        }
        cpu8086_state_t before = *cpu;
        bool ok = cpu8086_run_one(vm);
        if (failed) {
            uint32_t error = (write_fault ? 2u : 0) | (write_denied ? 1u : 0);
            ok &= dos_idt_test_fault(vm, &before, 14, error, 0x1000, 44) &&
                  cpu->cr2 == fault_linear && cpu->tr == before.tr && cpu->cr3 == before.cr3;
            for (unsigned i = 0; i < 104; i++)
                if (old[i] != vm->mem[region == 0 ? dos_string_test_physical(offset + i) : 0x4000 + i] ||
                    incoming[i] != vm->mem[region == 1 || region == 2 ? dos_string_test_physical(offset + i) : 0x5000 + i]) ok = false;
            faults++;
            if (ok) {
                dos_mem_write32(vm, entry, saved);
                *cpu = before;
                ok = cpu8086_run_one(vm);
                retries++;
            }
        }
        if (cpu->tr != 64 || cpu->eip != next.eip || cpu->cs != next.cs || cpu->esp != next.esp ||
            !dos_test_general_registers(cpu, &next) || cpu->cr3 != before.cr3 || cpu->delivery_fault) ok = false;
        if (!ok) {
            if (failures < 12) { serial_puts("[DOS-TASK-PAGING] case="); serial_putdec(checks);
                serial_puts(" region="); serial_putdec(region); serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" CR2="); serial_puthex(cpu->cr2, 8); serial_puts(" expected="); serial_puthex(fault_linear, 8);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-TASK-PAGING] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults); serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_task_address_space_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned source = 0; source < 3; source++)
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        cpu8086_state_t next = dos_task_test_prepare(vm, cpu, true, true, true, true, 0, 3);
        (void)dos_test_page_pair(vm, 0x400000);
        for (unsigned i = 0; i < 8192; i++) vm->mem[0x15000 + i] = 0;
        dos_mem_write32(vm, 0x15000, 0x16007);
        for (unsigned i = 0; i < 512; i++) dos_mem_write32(vm, 0x16000 + i * 4, i * 4096u | 7u);
        next.cr3 = 0x15018;
        next.ldtr = 96;
        next.cs = 15; next.ss = 23; next.es = next.ds = next.fs = next.gs = 31;
        dos_idt_test_descriptor(vm, 12, 0x6000, 0xFF, 0x82, false);
        for (unsigned i = 0; i < 24; i++) vm->mem[0x6008 + i] = vm->mem[0x2048 + i];
        /* The same LDT address names different physical descriptors after CR3. */
        for (unsigned i = 0; i < 0x100; i++) vm->mem[0x17000 + i] = vm->mem[0x6000 + i];
        dpmi_descriptor_t code = { .base_hi = 0, .access = 0xFA, .limit_lo = 0xFFFF };
        dpmi_desc_set_base(&code, 0x80000);
        for (unsigned i = 0; i < 8; i++) vm->mem[0x17008 + i] = ((uint8_t *)&code)[i];
        dos_mem_write32(vm, 0x16018, 0x17007);
        bool failed = scenario != 0;
        unsigned vector = scenario == 3 ? 11u : 14u;
        uint32_t error = scenario == 3 ? 12u : scenario == 2 || scenario == 4 ? 3u : 0;
        uint32_t cr2 = cpu->cr2;
        if (scenario == 1) { dos_mem_write32(vm, 0x16018, 0x17006); cr2 = 0x6008; }
        if (scenario == 2) { dos_mem_write32(vm, 0x16018, 0x17005); cr2 = 0x600D; }
        if (scenario == 3) vm->mem[0x1700D] &= ~DESC_PRESENT;
        if (scenario == 4) {
            vm->mem[0x1700D] |= DESC_ACCESSED;
            dos_mem_write32(vm, 0x16018, 0x17005); cr2 = 0x6015;
        }
        unsigned delivered = source == 2 && failed ? 8u : vector;
        if (source == 2) error = 0;
        dos_task_test_image(vm, 0x5000, true, &next);
        dos_task_test_rescue(vm, delivered, next.cr3);
        if (!source) (void)dos_far_test_code(vm, 0, 4, true, 64, 0, 0, false);
        else dos_idt_test_gate(vm, source == 1 ? 0x40 : 14, 64, 0, 0xE5);
        bool ok = source < 2 ? cpu8086_run_one(vm) : cpu_deliver_exception(vm, 14, 0x1000, 0, true);
        if (failed) {
            next.eflags |= FLAG_NT | (delivered == 8 ? 0 : FLAG_RF);
            if (cpu->tr != 128 || cpu->eip != 0x7000 + delivered * 16 ||
                cpu->esp != 0x7FFC || dos_mem_read32(vm, 0x97FFC) != error ||
                !dos_task_test_saved(vm, 0x5000, true, &next, next.eip, next.eflags)) ok = false;
            faults++;
        } else if (cpu->tr != 64 || cpu->eip != next.eip || cpu->ldtr != 96 ||
            dpmi_desc_get_base(&cpu->cs_cache.descriptor) != 0x80000 || !(vm->mem[0x1700D] & DESC_ACCESSED) ||
            (vm->mem[0x600D] & DESC_ACCESSED)) ok = false;
        if (!ok || !cpu->running || cpu->cr3 != next.cr3 || cpu->cr2 != cr2 || cpu->delivery_fault) {
            if (failures < 8) { serial_puts("[DOS-TASK-CR3] case="); serial_putdec(checks);
                serial_puts(" TR="); serial_puthex(cpu->tr, 4); serial_puts(" CR2="); serial_puthex(cpu->cr2, 8);
                serial_puts(" CR3="); serial_puthex(cpu->cr3, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-TASK-CR3] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_idt_gate_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, returns = 0;
    int failures = 0;
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned gate32 = 0; gate32 < 2; gate32++)
    for (unsigned trap = 0; trap < 2; trap++)
    for (unsigned cpl = 0; cpl < 4; cpl++)
    for (unsigned target_cpl = 0; target_cpl <= cpl; target_cpl++)
    for (unsigned conforming = 0; conforming < 2; conforming++)
    for (unsigned tss32 = 0; tss32 < 2; tss32++) {
        dos_idt_test_prepare(vm, cpu, wide, stack32, cpl);
        bool inward = !conforming && target_cpl < cpl;
        unsigned destination = inward ? target_cpl : cpl;
        unsigned width = gate32 ? 4u : 2u;
        bool new32 = !stack32;
        dpmi_descriptor_t code = dos_idt_test_descriptor(vm, 3, 0x60000, 0xFFFF,
            0x9Au | (target_cpl << 5) | (conforming ? 4u : 0), !wide);
        dpmi_descriptor_t stack = dos_idt_test_descriptor(vm, 4, 0x30000,
            new32 ? UINT32_MAX : 0xFFFFu, 0x92u | (destination << 5), new32);
        dpmi_descriptor_t tss = dos_idt_test_descriptor(vm, 7, 0x4000, tss32 ? 103 : 43,
                                                       tss32 ? 0x8B : 0x83, false);
        cpu8086_cache_tr(cpu, 56, &tss);
        unsigned tss_width = tss32 ? 4u : 2u;
        unsigned tss_offset = tss_width + target_cpl * 2u * tss_width;
        dos_far_test_field(vm, 0x4000 + tss_offset, tss_width, 0x9000);
        dos_mem_write16(vm, 0x4000 + tss_offset + tss_width, 32u | destination);
        dos_idt_test_gate(vm, 0x40, 27u, 0x1800, 0xE0u | (gate32 ? 14u : 6u) | trap);
        cpu->eflags |= FLAG_TF | FLAG_NT | FLAG_RF;
        cpu8086_state_t before = *cpu;
        /* Loaded SS/TR must survive contradictory descriptor-table changes. */
        dos_mem_write8(vm, cpu->gdtr.base + 2u * 8u + 5u, 0);
        dos_mem_write8(vm, cpu->gdtr.base + 7u * 8u + 5u, 0);
        bool ok = cpu8086_run_one(vm);
        unsigned fields = inward ? 5u : 3u;
        uint32_t pointer = inward ? 0x9000 : before.esp;
        uint32_t mask = (inward ? new32 : stack32) ? UINT32_MAX : 0xFFFFu;
        uint32_t next = (before.esp & ~mask) | ((pointer - fields * width) & mask);
        uint32_t at = (inward ? 0x30000 : 0x20000) + (next & mask);
        uint32_t field_mask = gate32 ? UINT32_MAX : 0xFFFFu;
        uint32_t live_flags = before.eflags & ~(FLAG_TF | FLAG_NT | FLAG_RF | (trap ? 0 : FLAG_IF));
        if (cpu->cs != (24u | destination) || cpu->eip != 0x1800 || cpu->esp != next ||
            cpu8086_cpl(cpu) != destination || cpu->ss != (inward ? 32u | destination : before.ss) ||
            cpu->eflags != live_flags || cpu->op_size_32 != !wide || cpu->delivery_fault ||
            !dos_test_general_registers(cpu, &before) || dos_idt_test_read(vm, at, width) != 0x1002 ||
            dos_idt_test_read(vm, at + width, width) != before.cs ||
            dos_idt_test_read(vm, at + 2u * width, width) != (before.eflags & field_mask) ||
            (inward && (dos_idt_test_read(vm, at + 3u * width, width) != (before.esp & field_mask) ||
                         dos_idt_test_read(vm, at + 4u * width, width) != before.ss))) ok = false;
        code.access |= DESC_ACCESSED;
        if (!dos_system_cache_equal(&cpu->cs_cache, &(cpu_system_segment_t){ code, true })) ok = false;
        if (inward) {
            stack.access |= DESC_ACCESSED;
            if (!dos_system_cache_equal(&cpu->ss_cache, &(cpu_system_segment_t){ stack, true })) ok = false;
        } else if (!dos_system_cache_equal(&cpu->ss_cache, &before.ss_cache)) ok = false;
        /* Restore the descriptor only for the outward IRET's new SS load. */
        for (unsigned i = 0; i < 8; i++) dos_mem_write8(vm, cpu->gdtr.base + 16u + i,
                                                       ((uint8_t *)&before.ss_cache.descriptor)[i]);
        unsigned p = 0x61800;
        if (gate32 != !wide) vm->mem[p++] = 0x66;
        vm->mem[p] = 0xCF;
        uint32_t returned_esp = stack32 ? before.esp : (next & 0xFFFF0000u) | before.sp;
        if (!cpu8086_run_one(vm) || cpu->cs != before.cs || cpu->eip != 0x1002 ||
            cpu->ss != before.ss || cpu->esp != returned_esp ||
            cpu8086_cpl(cpu) != cpl || !dos_test_general_registers(cpu, &before)) ok = false;
        else returns++;
        if (!ok) {
            if (failures < 8) { serial_puts("[DOS-IDT-GATE] case="); serial_putdec(checks);
                serial_puts(" CS:IP="); serial_puthex(cpu->cs, 4); serial_puts(":"); serial_puthex(cpu->eip, 8);
                serial_puts(" SP="); serial_puthex(cpu->esp, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-IDT-GATE] checks="); serial_putdec(checks);
    serial_puts(" returns="); serial_putdec(returns);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_idt_source_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned cpl = 0; cpl < 4; cpl++)
    for (unsigned dpl = 0; dpl < 4; dpl++)
    for (unsigned source = 0; source < 8; source++)
    for (unsigned prefix = 0; prefix < 3; prefix++) {
        dos_idt_test_prepare(vm, cpu, wide, !wide, cpl);
        bool hardware = source == 4, exception = source == 5;
        bool software = source < 3 || source == 6;
        unsigned vector = source == 0 ? 0x40u : source == 1 ? 3u : source == 2 || source == 7 ? 4u :
                          source == 3 ? 1u : source == 4 ? 0x41u : source == 5 ? 5u : 14u;
        dos_idt_test_descriptor(vm, 3, 0x60000, 0xFFFF, 0x9Au | (cpl << 5), true);
        dos_idt_test_gate(vm, vector, 24u, 0x1800, 0x8Eu | (dpl << 5));
        unsigned p = 0x1000;
        if (prefix == 1) vm->mem[p++] = 0x66;
        if (prefix == 2) vm->mem[p++] = 0xF0;
        vm->mem[p++] = source == 1 ? 0xCC : source == 2 || source == 7 ? 0xCE : source == 3 ? 0xF1 : 0xCD;
        if (source == 0 || source == 6) vm->mem[p++] = vector;
        if (source == 7) cpu->eflags &= ~FLAG_OF;
        cpu8086_state_t before = *cpu;
        unsigned expected_fault = !hardware && !exception && prefix == 2 ? 6u :
                                  software && dpl < cpl ? 13u : 0;
        bool ok = hardware ? cpu_deliver_hw_interrupt(vm, vector) : exception ?
            cpu_deliver_exception(vm, vector, 0x1000, 0, false) : cpu8086_run_one(vm);
        if (expected_fault) {
            /* #UD has no error code; #GP does and names the IDT entry. */
            if (expected_fault == 13) ok &= dos_idt_test_fault(vm, &before, 13, vector * 8u + 2u, 0x1000, 40);
            else {
                uint32_t at = 0x20000 + ((before.esp - 12u) & (!wide ? UINT32_MAX : 0xFFFFu));
                ok &= cpu->eip == 0x4060 && cpu->esp == before.esp - 12u &&
                      dos_mem_read32(vm, at) == 0x1000 && dos_mem_read32(vm, at + 4) == before.cs;
            }
            faults++;
        } else if (source == 7) ok &= cpu->eip == p && cpu->esp == before.esp;
        else {
            uint32_t at = 0x20000 + ((before.esp - 12u) & (!wide ? UINT32_MAX : 0xFFFFu));
            ok &= cpu->cs == (24u | cpl) && cpu->eip == 0x1800 && cpu->esp == before.esp - 12u &&
                  dos_mem_read32(vm, at) == (hardware || exception ? 0x1000 : p) &&
                  dos_mem_read32(vm, at + 8) == (before.eflags | (exception ? FLAG_RF : 0));
        }
        if (!ok || cpu->delivery_fault || !dos_test_general_registers(cpu, &before)) {
            if (failures < 8) { serial_puts("[DOS-IDT-SOURCE] case="); serial_putdec(checks);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-IDT-SOURCE] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_idt_admission_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned source = 0; source < 2; source++)
    for (unsigned scenario = 0; scenario < 36; scenario++) {
        dos_idt_test_prepare(vm, cpu, wide, stack32, 3);
        unsigned width = wide ? 4u : 2u;
        dos_idt_test_descriptor(vm, 3, 0x60000, 0xFFFF, 0xBA, wide);
        dos_idt_test_gate(vm, 0x40, 27, 0x1800, wide ? 0xEE : 0xE6);
        uint32_t gate = cpu->idtr.base + 0x200;
        unsigned vector = 0, error = 0;
        switch (scenario) {
        case 1: cpu->idtr.limit = 0x206; vector = 13; error = 0x202; break;
        case 2: vm->mem[gate + 5] = 0xEC; vector = 13; error = 0x202; break;
        case 3: vm->mem[gate + 5] |= 0x10; vector = 13; error = 0x202; break;
        case 4: vm->mem[gate + 5] &= ~0x80; vector = 11; error = 0x202; break;
        case 5: dos_mem_write16(vm, gate + 2, 3); vector = 13; break;
        case 6: dos_mem_write16(vm, gate + 2, 0xFFF8); vector = 13; error = 0xFFF8; break;
        case 7: dos_mem_write16(vm, gate + 2, 0x1C); vector = 13; error = 0x1C; break;
        case 8: vm->mem[0x201D] = 0xB2; vector = 13; error = 24; break;
        case 9: vm->mem[0x201D] = 0xBA & ~0x80; vector = 11; error = 24; break;
        case 10: cpu->tss_cache.valid = false; vector = 10; error = 56; break;
        case 11: cpu->tr = 0; vector = 10; break;
        case 12: cpu->tr |= 4; vector = 10; error = 60; break;
        case 13: dpmi_desc_set_limit(&cpu->tss_cache.descriptor, 16); vector = 10; error = 56; break;
        case 14: cpu->tss_cache.descriptor.access = 0x89 & ~0x80; vector = 10; error = 56; break;
        case 15: cpu->tss_cache.descriptor.access = 0x82; vector = 10; error = 56; break;
        case 16: dos_mem_write16(vm, 0x4010, 1); vector = 10; break;
        case 17: dos_mem_write16(vm, 0x4010, 0xFFF9); vector = 10; error = 0xFFF8; break;
        case 18: dos_mem_write16(vm, 0x4010, 35); vector = 10; error = 32; break;
        case 19: vm->mem[0x2025] = 0xF2; vector = 10; error = 32; break;
        case 20: vm->mem[0x2025] = 0xB0; vector = 10; error = 32; break;
        case 21: vm->mem[0x2025] = 0xBA; vector = 10; error = 32; break;
        case 22: vm->mem[0x2025] = 0x32; vector = 12; error = 32; break;
        case 23: dos_idt_test_descriptor(vm, 4, 0x30000, 0x8FFE, 0xB2, stack32); vector = 12; break;
        case 24: dos_mem_write16(vm, 0x2018, 0x17FF); vector = 13; break;
        case 25: vm->mem[gate + 4] = 0xFF; break; /* Reserved/unused, not parameter copying. */
        case 26: vm->mem[gate + 5] = 0xE5; dos_mem_write16(vm, gate + 2, 60); vector = 13; error = 60; break;
        case 27: vm->mem[gate + 5] = 0xE5; dos_mem_write16(vm, gate + 2, 56); vector = 13; error = 56; break;
        case 28: vm->mem[gate + 5] = 0xE5; dos_mem_write16(vm, gate + 2, 56);
            vm->mem[0x203D] = 0x09; vector = 11; error = 56; break;
        case 29: vm->mem[gate + 5] = 0xE5; dos_mem_write16(vm, gate + 2, 56);
            vm->mem[0x203D] = 0x89; dos_mem_write16(vm, 0x2038, 102); vector = 10; error = 56; break;
        case 30: vm->mem[gate + 5] = 0xE5; dos_mem_write16(vm, gate + 2, 56);
            vm->mem[0x203D] = 0x81; dos_mem_write16(vm, 0x2038, 42); vector = 10; error = 56; break;
        case 31: /* A zero IDTR base is a legal table address. */
            for (unsigned i = 0; i < 0x800; i++) vm->mem[i] = vm->mem[0x3000 + i];
            cpu->idtr.base = 0; break;
        case 32: dos_idt_test_descriptor(vm, 4, 0x30000, 0x9000 - 5u * width - 1u, 0xB6, stack32); break;
        case 33: dos_idt_test_descriptor(vm, 4, 0x30000, 0x9000 - 5u * width, 0xB6, stack32); vector = 12; break;
        case 34: dos_mem_write32(vm, 0x400C, 0); break; /* Whole fields wrap SS.B. */
        case 35: dos_mem_write32(vm, 0x400C, 1); vector = 12; break;
        default: break;
        }
        cpu8086_state_t before = *cpu;
        bool ok = source ? cpu_deliver_hw_interrupt(vm, 0x40) : cpu8086_run_one(vm);
        if (vector) {
            if (source) error |= 1u;
            ok &= dos_idt_test_fault(vm, &before, vector, error, 0x1000, 40);
            for (unsigned i = 0; i < 64; i++) if (vm->mem[0x38FC0 + i] != 0x5A) ok = false;
            faults++;
        } else ok &= cpu->eip == 0x1800 && cpu8086_cpl(cpu) == 1;
        if (!ok || cpu->delivery_fault || !dos_test_general_registers(cpu, &before)) {
            if (failures < 12) { serial_puts("[DOS-IDT-ADMIT] case="); serial_putdec(checks);
                serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-IDT-ADMIT] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_idt_nested_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const unsigned initial[] = { 0, 1, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 8 };
    unsigned checks = 0, doubles = 0, triples = 0;
    int failures = 0;
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned first = 0; first < sizeof(initial) / sizeof(initial[0]); first++)
    for (unsigned missing = 0; missing < 2; missing++)
    for (unsigned fail_double = 0; fail_double < 2; fail_double++) {
        dos_idt_test_prepare(vm, cpu, wide, wide, 3);
        unsigned vector = initial[first], second = missing ? 11 : 13;
        dos_idt_test_gate(vm, vector, 24, 0x1800, missing ? 0x0E : 0x8C);
        if (fail_double) dos_idt_test_gate(vm, 8, 24, 0x1800, 0x8C);
        cpu8086_state_t before = *cpu;
        bool contributory = vector == 0 || (vector >= 10 && vector <= 13);
        bool df = contributory || vector == 14;
        bool triple = vector == 8 || (df && fail_double);
        bool ok = cpu_deliver_exception(vm, vector, 0x1000, 0x1234,
                                        vector == 8 || (vector >= 10 && vector <= 14));
        if (triple) {
            ok = !ok && !cpu->running && cpu->exit_code == -1 && cpu->cs == before.cs &&
                 cpu->esp == before.esp && cpu->eip == before.eip;
            triples++;
        } else {
            unsigned delivered = df ? 8u : second;
            unsigned error = df ? 0u : vector * 8u + 3u;
            ok &= dos_idt_test_fault(vm, &before, delivered, error, 0x1000, 40);
            if (df) doubles++;
        }
        if (!ok || cpu->delivery_fault || !dos_test_general_registers(cpu, &before)) {
            if (failures < 8) { serial_puts("[DOS-IDT-NESTED] case="); serial_putdec(checks);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-IDT-NESTED] checks="); serial_putdec(checks);
    serial_puts(" doubles="); serial_putdec(doubles);
    serial_puts(" triples="); serial_putdec(triples);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_idt_paging_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint32_t base = 0x403FF000u;
    unsigned checks = 0, faults = 0, retries = 0, triples = 0;
    int failures = 0;
    uint8_t *bytes = dos_host_alloc_pages(2);
    if (!bytes) return 1;
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned kind = 0; kind < 5; kind++)
    for (unsigned scenario = 0; scenario < 9; scenario++) {
        bool descriptor = kind == 1 || kind == 2;
        if (scenario >= 7 && !descriptor) continue;
        dos_idt_test_prepare(vm, cpu, wide, true, 3);
        unsigned width = wide ? 4u : 2u;
        dos_idt_test_descriptor(vm, 3, 0x60000, 0xFFFF, 0xBA, wide);
        dos_idt_test_gate(vm, 0x40, 24, 0x1800, wide ? 0xEE : 0xE6);
        dpmi_descriptor_t rescue = dos_idt_test_descriptor(vm, 5, 0x60000, 0xFFFF, 0x9F, true);
        for (unsigned b = 0; b < 8; b++) vm->mem[0x5000 + 40u + b] = ((uint8_t *)&rescue)[b];
        dpmi_descriptor_t ldt = { .access = 0x82, .limit_lo = 0xFF, .base_lo = 0x5000 };
        cpu8086_cache_ldtr(cpu, 64, &ldt);
        for (unsigned v = 0; v < 32; v++) dos_idt_test_gate(vm, v, 44, 0x4000 + v * 16u, 0xEE);
        dos_test_page_pair_t pair = dos_test_page_pair(vm, base);
        for (unsigned i = 0; i < 8192; i++) bytes[i] = 0xA5;
        unsigned offset = 4095;
        if (kind == 0) {
            offset -= 0x200;
            for (unsigned i = 0; i < 0x800; i++) bytes[offset + i] = vm->mem[0x3000 + i];
            cpu->idtr.base = base + offset;
        } else if (kind == 1 || kind == 2) {
            offset -= (kind == 1 ? 3u : 4u) * 8u;
            for (unsigned i = 0; i < 0x100; i++) bytes[offset + i] = vm->mem[0x2000 + i];
            if (scenario == 6 || scenario == 7) bytes[offset + 3u * 8u + 5u] |= DESC_ACCESSED;
            if (scenario == 6 || scenario == 8) bytes[offset + 4u * 8u + 5u] |= DESC_ACCESSED;
            cpu->gdtr.base = base + offset;
        } else if (kind == 3) {
            offset -= 12;
            for (unsigned i = 0; i < 104; i++) bytes[offset + i] = vm->mem[0x4000 + i];
            dpmi_desc_set_base(&cpu->tss_cache.descriptor, base + offset);
        } else {
            dpmi_descriptor_t stack = dos_idt_test_descriptor(vm, 4, base, UINT32_MAX, 0xB3, true);
            (void)stack;
            dos_mem_write32(vm, 0x400C, 4096u + width);
        }
        for (unsigned i = 0; i < 8192; i++) vm->mem[dos_string_test_physical(i)] = bytes[i];
        unsigned page = scenario == 1 ? 0 : 1;
        unsigned bit = scenario <= 2 ? 0 : scenario == 3 ? 2 : 1;
        uint32_t entry = pair.pte[page], saved = dos_mem_read32(vm, entry);
        if (scenario) dos_mem_write32(vm, entry, saved & ~(1u << bit));
        if (scenario == 5) cpu->cr0 &= ~DOS_CR0_WP;
        bool code_access_fault = kind == 1 && bit == 1 && scenario != 5 && scenario != 6 && scenario != 7;
        bool stack_access_fault = descriptor && bit == 1 && scenario != 5 && scenario != 6 && scenario != 8;
        bool failed = scenario && (!bit || (bit == 1 && scenario != 5 &&
                      (kind == 4 || code_access_fault || stack_access_fault)));
        bool triple = failed && kind == 0 && !page;
        uint32_t fault_linear = base + (page ? 4096u : kind == 2 ? 4087u : kind == 4 ? 4096u - width : 4095u);
        unsigned error = kind == 4 ? 2u : 0;
        if (bit) {
            error |= 1u;
            if (descriptor) {
                error |= 2u;
                fault_linear = base + offset + (code_access_fault ? 3u : 4u) * 8u + 5u;
            }
        }
        cpu8086_state_t before = *cpu;
        bool ok = cpu8086_run_one(vm);
        if (triple) {
            ok = !ok && !cpu->running && cpu->cs == before.cs && cpu->esp == before.esp &&
                 cpu->eip == before.eip && cpu->cr2 == cpu->idtr.base + 8u * 8u;
            triples++;
        } else if (failed) {
            ok &= dos_idt_test_fault(vm, &before, 14, error, 0x1000, 44) && cpu->cr2 == fault_linear;
            faults++;
            dos_mem_write32(vm, entry, saved);
            /* Execute the guest #PF handler's ADD ESP,4 / IRETD, then retry. */
            uint32_t handler = 0x640E0;
            vm->mem[handler] = 0x83; vm->mem[handler + 1] = 0xC4;
            vm->mem[handler + 2] = 4; vm->mem[handler + 3] = 0xCF;
            if (!cpu8086_run_one(vm) || !cpu8086_run_one(vm) ||
                cpu->cs != before.cs || cpu->eip != before.eip || cpu->esp != before.esp ||
                !cpu8086_run_one(vm)) ok = false;
            else retries++;
        }
        if (!triple && (cpu->cs != 25 || cpu->eip != 0x1800 || cpu8086_cpl(cpu) != 1)) ok = false;
        if (!ok || cpu->delivery_fault) {
            if (failures < 12) { serial_puts("[DOS-IDT-PAGING] case="); serial_putdec(checks);
                serial_puts(" kind="); serial_putdec(kind); serial_puts(" scenario="); serial_putdec(scenario);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8);
                serial_puts(" CR2="); serial_puthex(cpu->cr2, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    dos_host_free_pages(bytes, 2);
    serial_puts("[DOS-IDT-PAGING] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" triples="); serial_putdec(triples);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_idt_v86_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0, returns = 0;
    int failures = 0;
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned source = 0; source < 5; source++)
    for (unsigned iopl = 0; iopl < 4; iopl++)
    for (unsigned target = 0; target < 4; target++) {
        dos_idt_test_prepare(vm, cpu, true, true, 0);
        uint32_t frame[] = { 0x1000, 0x1234, FLAGS_FIXED | FLAG_VM | FLAG_IF | FLAG_OF | (iopl << 12),
                            0xABCD9000u, 0x2000, 0x2345, 0x3456, 0x4567, 0x5678 };
        dos_iret_test_frame(vm, 0x29000, 4, frame, 9);
        vm->mem[0x1000] = 0xCF;
        bool ok = cpu8086_run_one(vm) && !cpu->protected_mode && cpu->guest_idt && cpu->eip == 0x1000;
        unsigned vector = source == 0 ? 0x40 : source == 1 ? 3 : source == 2 ? 4 : source == 3 ? 1 : 0x41;
        unsigned dest_cpl = target == 1 ? 1 : target == 2 ? 3 : 0;
        dos_idt_test_descriptor(vm, 3, 0x60000, 0xFFFF, 0x9Au | (dest_cpl << 5) | (target == 3 ? 4u : 0), true);
        dos_idt_test_descriptor(vm, 5, 0x60000, 0xFFFF, 0x9B, true);
        dos_idt_test_descriptor(vm, 6, 0x50000, stack32 ? UINT32_MAX : 0xFFFFu, 0x93, stack32);
        dos_idt_test_gate(vm, vector, 27, 0x1800, wide ? 0xEE : 0xE6);
        uint32_t at = 0x13340;
        vm->mem[at] = source == 0 ? 0xCD : source == 1 ? 0xCC : source == 2 ? 0xCE : 0xF1;
        vm->mem[at + 1] = vector;
        uint32_t saved_ip = source == 0 ? 0x1002 : source == 4 ? 0x1000 : 0x1001;
        bool failed = (source == 0 && iopl != 3) || target;
        unsigned width = failed || wide ? 4u : 2u;
        unsigned count = failed ? 10u : 9u;
        uint32_t expected_flags = frame[2] | (failed ? FLAG_RF : 0);
        uint32_t error = source == 0 && iopl != 3 ? 0 : 24;
        if (source == 3 || source == 4) error |= 1;
        if (source == 4) ok &= cpu_deliver_hw_interrupt(vm, vector);
        else ok &= cpu8086_run_one(vm);
        uint32_t sp = (stack32 ? 0 : 0xABCD0000u) | (0x9000u - count * width);
        uint32_t address = 0x50000u + (uint16_t)sp;
        if (cpu->esp != sp || cpu->ss != 48 || cpu8086_cpl(cpu) != 0 || !cpu->protected_mode ||
            cpu->cs != (failed ? 40u : 24u) || cpu->eip != (failed ? 0x40D0u : 0x1800u) ||
            (cpu->eflags & (FLAG_VM | FLAG_TF | FLAG_RF | FLAG_NT))) ok = false;
        if (failed) {
            if (dos_mem_read32(vm, address) != error) ok = false;
            address += 4;
            saved_ip = 0x1000;
            faults++;
        }
        const uint32_t expected[] = { saved_ip, frame[1], expected_flags, frame[3], frame[4],
                                      frame[5], frame[6], frame[7], frame[8] };
        for (unsigned i = 0; i < 9; i++) if (dos_idt_test_read(vm, address + i * width, width) !=
                                             (expected[i] & (width == 4 ? UINT32_MAX : 0xFFFFu))) ok = false;
        for (unsigned s = 0; s < 6; s++) if (s != 1 && s != 2 && cpu->segment_cache[s].valid) ok = false;
        if (width == 4) {
            uint32_t handler = 0x60000 + cpu->eip;
            if (failed) {
                vm->mem[handler] = 0x83; vm->mem[handler + 1] = 0xC4; vm->mem[handler + 2] = 4;
                handler += 3;
            }
            vm->mem[handler] = 0xCF;
            if (failed && !cpu8086_run_one(vm)) ok = false;
            if (!cpu8086_run_one(vm) || cpu->protected_mode || !cpu->guest_idt ||
                cpu->cs != frame[1] || cpu->eip != saved_ip || cpu->esp != frame[3] ||
                cpu->ss != frame[4] || cpu->es != frame[5] || cpu->ds != frame[6] ||
                cpu->fs != frame[7] || cpu->gs != frame[8] || cpu->eflags != expected_flags) ok = false;
            else returns++;
        }
        if (!ok || cpu->delivery_fault) {
            if (failures < 12) { serial_puts("[DOS-IDT-V86] case="); serial_putdec(checks);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8);
                serial_puts(" SP="); serial_puthex(cpu->esp, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-IDT-V86] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" returns="); serial_putdec(returns);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_task_jit_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, returns = 0;
    int failures = 0;
    unsigned pages = (sizeof(jit_state_t) + 4095u) / 4096u;
    jit_state_t *jit = dos_host_alloc_pages(pages);
    if (!jit) return 1;
    jit_init(jit);
    if (!jit->code_buf) { dos_host_free_pages(jit, pages); return 1; }
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned position = 0; position < 4; position++)
    for (unsigned busy = 0; busy < 2; busy++) {
        bool code32 = mode == 1, v86 = mode == 2;
        cpu8086_state_t next = dos_task_test_prepare(vm, cpu, true, wide, code32, true, 3, 0);
        vm->jit = jit;
        dpmi_descriptor_t code = dos_idt_test_descriptor(vm, 1, 0, 0x1FFFFu, 0xFB, code32);
        cpu8086_cache_cs(cpu, 11, &code, 3);
        bool nop = position & 1u;
        uint32_t start = position < 2 ? 0x1000u : 0xFFFEu - (unsigned)nop;
        uint32_t fault_eip = start + (unsigned)nop;
        /* CS.D does not truncate the protected EIP saved in a 32-bit TSS. */
        uint32_t return_eip = v86 ? (uint16_t)(fault_eip + 2u) : fault_eip + 2u;
        if (v86) {
            cpu->protected_mode = false;
            cpu->eflags |= FLAG_VM;
            cpu8086_load_real_cs(cpu, 0x1000);
            for (unsigned s = 0; s < 6; s++) if (s != 1) cpu8086_load_real_segment(cpu, s, 0x2000 + s * 0x1000);
            dos_idt_test_descriptor(vm, 5, 0x60000, 0xFFFF, 0x9B, true);
            dos_mem_write32(vm, 0x4004, 0x9000); dos_mem_write16(vm, 0x4008, 48);
        }
        cpu->eip = start;
        uint32_t p = start + (v86 ? 0x10000 : 0);
        if (nop) vm->mem[p++] = 0x90;
        vm->mem[p++] = 0xCD; vm->mem[p] = 0x40;
        dos_idt_test_gate(vm, 0x40, 64, 0, 0xE5);
        if (busy) vm->mem[0x2045] |= 2u;
        jit_block_t *block = jit_get_block(jit, cpu->cs, start);
        bool ok = block && jit_decode_block(vm, block) > 0 && jit_compile_block(jit, block) == 0;
        cpu8086_state_t before = *cpu;
        uint64_t count = jit->jit_instructions;
        if (ok) {
            bool executed = jit_block_current(vm, block) && jit_exec_block(vm, block);
            if (v86) ok = executed;
            else {
                /* Protected INT remains an interpreter terminator. */
                ok = !executed && cpu->eip == fault_eip;
                if (ok) ok = cpu8086_run_one(vm);
            }
        }
        if (jit->jit_instructions != count + (unsigned)v86 + (unsigned)nop) ok = false;
        if (busy) {
            if (v86) ok &= cpu->protected_mode && cpu->cs == 40 && cpu->eip == 0x40D0 &&
                cpu->ss == 48 && cpu->esp == 0x9000 - 40 && dos_mem_read32(vm, 0x59000 - 40) == 64 &&
                dos_mem_read32(vm, 0x59000 - 36) == fault_eip;
            else ok &= dos_idt_test_fault(vm, &before, 13, 64, fault_eip, 40);
            ok &= cpu->tr == 56;
        } else {
            ok &= cpu->tr == 64 && cpu->cs == next.cs && cpu->eip == next.eip &&
                cpu->esp == next.esp && cpu->eflags == (next.eflags | FLAG_NT) &&
                dos_test_general_registers(cpu, &next) &&
                dos_task_test_saved(vm, 0x4000, true, &before, return_eip, before.eflags);
            vm->mem[0x61800] = 0xCF;
            if (ok) ok = cpu8086_run_one(vm) && cpu->tr == 56 && cpu->cs == before.cs &&
                cpu->eip == return_eip && cpu->esp == before.esp && cpu->eflags == before.eflags;
            if (ok) returns++;
        }
        if (!ok || !cpu->running || cpu->delivery_fault || vm->dpmi.exception_depth) {
            if (failures < 8) { serial_puts("[DOS-TASK-JIT] case="); serial_putdec(checks);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        vm->jit = NULL;
        checks++;
    }
    jit_destroy(jit);
    dos_host_free_pages(jit, pages);
    serial_puts("[DOS-TASK-JIT] checks="); serial_putdec(checks);
    serial_puts(" returns="); serial_putdec(returns);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_idt_jit_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0, returns = 0;
    int failures = 0;
    unsigned pages = (sizeof(jit_state_t) + 4095u) / 4096u;
    jit_state_t *jit = dos_host_alloc_pages(pages);
    if (!jit) return 1;
    jit_init(jit);
    if (!jit->code_buf) { dos_host_free_pages(jit, pages); return 1; }
    for (unsigned cached_real = 0; cached_real < 2; cached_real++)
    for (unsigned position = 0; position < 4; position++)
    for (unsigned iopl = 0; iopl < 4; iopl++)
    for (unsigned dpl = 0; dpl < 4; dpl++) {
        dos_idt_test_prepare(vm, cpu, true, true, 0);
        vm->jit = jit;
        bool nop = position & 1u;
        uint32_t start = position < 2 ? 0x1000u : 0xFFFEu - (unsigned)nop;
        uint32_t fault_ip = start + (unsigned)nop;
        uint32_t frame[] = { start, 0x1234, FLAGS_FIXED | FLAG_VM | FLAG_IF | (iopl << 12),
                            0xABCD9000u, 0x2000, 0x2345, 0x3456, 0x4567, 0x5678 };
        uint32_t p = 0x12340u + start;
        if (nop) vm->mem[p++] = 0x90;
        vm->mem[p++] = 0xCD; vm->mem[p] = 0x40;
        dos_idt_test_descriptor(vm, 5, 0x60000, 0xFFFF, 0x9B, true);
        dos_idt_test_gate(vm, 0x40, 24, 0x1800, 0x8Eu | (dpl << 5));
        jit_block_t *block = jit_get_block(jit, 0x1234, start);
        bool ok = block != NULL;
        if (cached_real && block) {
            /* Warm the source cache in a real-mode fixture before guest IRETD. */
            cpu8086_state_t pm = *cpu;
            cpu->protected_mode = false;
            cpu->cr0 = 0; cpu->eflags = FLAGS_FIXED; cpu->guest_idt = false;
            cpu8086_reset_real_cs(cpu, 0x1234);
            cpu->eip = start;
            ok &= jit_decode_block(vm, block) > 0 && jit_compile_block(jit, block) == 0;
            *cpu = pm;
        }
        dos_iret_test_frame(vm, 0x29000, 4, frame, 9);
        vm->mem[0x1000] = 0xCF;
        ok &= cpu8086_run_one(vm) && !cpu->protected_mode && cpu->guest_idt && cpu->eip == start;
        if (!cached_real && block)
            ok &= jit_decode_block(vm, block) > 0 && jit_compile_block(jit, block) == 0;
        cpu8086_state_t before = *cpu;
        uint64_t count = jit->jit_instructions;
        ok &= block && jit_block_current(vm, block) && jit_exec_block(vm, block);
        bool failed = iopl != 3 || dpl != 3;
        unsigned fields = failed ? 10u : 9u;
        uint32_t ip = failed ? fault_ip : (uint16_t)(fault_ip + 2u);
        uint32_t flags = frame[2] | (failed ? FLAG_RF : 0);
        uint32_t at = 0x59000u - fields * 4u;
        ok &= cpu->running && cpu->protected_mode && cpu8086_cpl(cpu) == 0 &&
              cpu->cs == (failed ? 40u : 24u) && cpu->eip == (failed ? 0x40D0u : 0x1800u) &&
              cpu->ss == 48 && cpu->esp == 0x9000u - fields * 4u &&
              !(cpu->eflags & (FLAG_VM | FLAG_IF | FLAG_TF | FLAG_RF)) &&
              jit->jit_instructions - count == (unsigned)nop + 1u &&
              dos_test_general_registers(cpu, &before);
        if (failed) {
            ok &= dos_mem_read32(vm, at) == (iopl != 3 ? 0u : 0x202u);
            at += 4;
            faults++;
        }
        const uint32_t expected[] = { ip, frame[1], flags, frame[3], frame[4],
                                      frame[5], frame[6], frame[7], frame[8] };
        for (unsigned i = 0; i < 9; i++) if (dos_mem_read32(vm, at + i * 4u) != expected[i]) ok = false;
        p = 0x60000u + (failed ? 0x40D0u : 0x1800u);
        if (failed) {
            vm->mem[p++] = 0x83; vm->mem[p++] = 0xC4; vm->mem[p++] = 4;
        }
        vm->mem[p] = 0xCF;
        if (failed && !cpu8086_run_one(vm)) ok = false;
        if (!cpu8086_run_one(vm) || cpu->protected_mode || !cpu->guest_idt ||
            cpu->cs != frame[1] || cpu->eip != ip || cpu->esp != frame[3] ||
            cpu->ss != frame[4] || cpu->es != frame[5] || cpu->ds != frame[6] ||
            cpu->fs != frame[7] || cpu->gs != frame[8] || cpu->eflags != flags) ok = false;
        else returns++;
        if (!ok || cpu->delivery_fault) {
            if (failures < 8) { serial_puts("[DOS-IDT-JIT] case="); serial_putdec(checks);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        vm->jit = NULL;
        checks++;
    }
    jit_destroy(jit);
    dos_host_free_pages(jit, pages);
    serial_puts("[DOS-IDT-JIT] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" returns="); serial_putdec(returns);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static bool dos_flags_test_prepare(dos_vm_t *vm, cpu8086_state_t *cpu,
                                     unsigned mode, bool stack32, unsigned cpl)
{
    if (!mode) {
        dos_fetch_test_prepare(vm, cpu, 0);
        vm->dpmi.active = false;
        vm->emulate_cpu = true;
        cpu->ss = 0x2000;
        cpu8086_sync_data(cpu);
        cpu->esp = 0xABCD9000;
    } else {
        dos_idt_test_prepare(vm, cpu, mode != 1, stack32, mode == 3 ? 0u : cpl);
        if (mode == 3) {
            const uint32_t frame[] = { 0x1000, 0, FLAGS_FIXED | FLAG_VM | FLAG_IOPL_MASK,
                                      0xABCD9000, 0x2000, 0x2345, 0x3456, 0x4567, 0x5678 };
            dos_iret_test_frame(vm, 0x29000, 4, frame, 9);
            vm->mem[0x1000] = 0xCF;
            if (!cpu8086_run_one(vm) || cpu->protected_mode || !cpu->guest_idt ||
                cpu->eip != 0x1000 || cpu->ss != 0x2000) return false;
            dos_idt_test_descriptor(vm, 5, 0x60000, 0xFFFF, 0x9B, true);
        }
    }
    cpu->irq_shadow = 0;
    vm->step_limit = 64;
    dos_io_write8(vm, 0x21, 0xFF);
    dos_io_write8(vm, 0xA1, 0xFF);
    return true;
}

static bool dos_flags_test_fault(dos_vm_t *vm, const cpu8086_state_t *before,
                                   unsigned vector, uint32_t error, unsigned target_cpl)
{
    cpu8086_state_t *cpu = vm->cpu;
    bool v86 = (before->cr0 & 1u) && (before->eflags & FLAG_VM);
    bool outer = v86 || target_cpl < cpu8086_cpl(before), has_error = vector != 6;
    unsigned fields = 3u + (unsigned)has_error + (outer ? 2u : 0) + (v86 ? 4u : 0);
    uint32_t sp = (outer ? 0x9000u : before->esp) - fields * 4u;
    uint32_t at = (outer ? 0x50000u : dpmi_desc_get_base(&before->ss_cache.descriptor)) +
                 (outer || cpu_stack_addr32(before) ? sp : (uint16_t)sp);
    bool ok = cpu->running && !cpu->delivery_fault && !vm->dpmi.exception_depth &&
        cpu->cs == (40u | target_cpl) && cpu->eip == 0x4000u + vector * 16u &&
        cpu8086_cpl(cpu) == target_cpl && cpu->esp == sp &&
        cpu->ss == (outer ? 48u : before->ss) && dos_test_general_registers(cpu, before);
    if (has_error) {
        ok &= dos_mem_read32(vm, at) == error;
        at += 4;
    }
    const uint32_t frame[] = { before->eip, before->cs, before->eflags | FLAG_RF,
                              before->esp, before->ss, before->es, before->ds, before->fs, before->gs };
    for (unsigned i = 0; i < fields - (unsigned)has_error; i++)
        if (dos_mem_read32(vm, at + i * 4u) != frame[i]) ok = false;
    return ok;
}

/* Intel's POPF table, expressed per bit rather than sharing the CPU mask. */
static uint32_t dos_flags_test_pop(uint32_t old, uint32_t value, unsigned width, unsigned cpl)
{
    uint32_t result = old | FLAGS_FIXED;
    for (unsigned bit = 0; bit < width * 8u; bit++) {
        bool writable = bit == 0 || bit == 2 || bit == 4 || bit == 6 || bit == 7 ||
                        bit == 8 || bit == 10 || bit == 11 || bit == 14 || bit == 18 || bit == 21;
        if (bit == 9) writable = cpl <= ((old >> 12) & 3u);
        if (bit == 12 || bit == 13) writable = cpl == 0;
        if (writable) result = (result & ~(1u << bit)) | (value & (1u << bit));
    }
    return result & ~FLAG_RF;
}

static int dos_flags_stack_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint32_t patterns[] = { 0, UINT32_MAX, 0xAAAAAAAA, 0x55555555 };
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 0; mode < 4; mode++)
    for (unsigned cpl = 0; cpl < (mode == 1 || mode == 2 ? 4u : 1u); cpl++)
    for (unsigned stack32 = 0; stack32 < (mode == 1 || mode == 2 ? 2u : 1u); stack32++)
    for (unsigned iopl = 0; iopl < 4; iopl++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned pattern = 0; pattern < 4; pattern++)
    for (unsigned pop = 0; pop < 2; pop++) {
        bool ok = dos_flags_test_prepare(vm, cpu, mode, stack32, cpl);
        uint32_t old = ((~patterns[pattern] & 0x003C4FD5u) | FLAGS_FIXED | FLAG_RF | (iopl << 12));
        if (mode == 3) old |= FLAG_VM;
        cpu->eflags = old;
        /* An installed guest IDT must not borrow a suspended host's IF. */
        if (mode == 1 || mode == 2) vm->dpmi.active = true;
        vm->dpmi.virtual_interrupts_enabled = !(old & FLAG_IF);
        bool host_if = vm->dpmi.virtual_interrupts_enabled;
        uint32_t p = 0x1000;
        if ((width == 4) != cpu->op_size_32) vm->mem[p++] = 0x66;
        vm->mem[p++] = pop ? 0x9D : 0x9C;
        dos_mem_write32(vm, 0x29000, patterns[pattern]);
        cpu8086_state_t before = *cpu;
        ok &= cpu8086_run_one(vm);
        bool failed = mode == 3 && iopl != 3;
        if (failed) {
            ok &= dos_flags_test_fault(vm, &before, 13, 0, 0);
            faults++;
        } else {
            uint32_t expected = pop ? dos_flags_test_pop(old, patterns[pattern], width,
                                                        mode == 3 ? 3u : cpl) : old;
            ok &= cpu->eip == p && cpu->cs == before.cs && cpu->ss == before.ss &&
                  cpu->esp == before.esp + (pop ? width : -width) &&
                  (cpu->eflags & ~FLAG_RF) == (expected & ~FLAG_RF) &&
                  (!pop || !(cpu->eflags & FLAG_RF)) && !cpu->irq_shadow &&
                  dos_test_general_registers(cpu, &before);
            if (!pop) ok &= dos_idt_test_read(vm, 0x29000u - width, width) ==
                ((old & ~(FLAG_RF | FLAG_VM)) & (width == 4 ? UINT32_MAX : 0xFFFFu));
        }
        ok &= dos_mem_read32(vm, 0x29000) == patterns[pattern] &&
              vm->dpmi.virtual_interrupts_enabled == host_if && cpu->cr2 == before.cr2;
        if (!ok) {
            if (failures < 8) { serial_puts("[DOS-FLAGS-STACK] case="); serial_putdec(checks);
                serial_puts(" flags="); serial_puthex(cpu->eflags, 8);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-FLAGS-STACK] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_flags_interrupt_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0;
    int failures = 0;
    for (unsigned mode = 0; mode < 4; mode++)
    for (unsigned cpl = 0; cpl < (mode == 1 || mode == 2 ? 4u : 1u); cpl++)
    for (unsigned iopl = 0; iopl < 4; iopl++)
    for (unsigned initial = 0; initial < 2; initial++)
    for (unsigned enable = 0; enable < 2; enable++) {
        bool ok = dos_flags_test_prepare(vm, cpu, mode, mode == 2, cpl);
        cpu->eflags = FLAGS_FIXED | FLAG_CF | FLAG_DF | FLAG_NT | FLAG_VIP | FLAG_VIF |
                      (initial ? FLAG_IF : 0) | (iopl << 12) | (mode == 3 ? FLAG_VM : 0);
        if (mode == 1 || mode == 2) vm->dpmi.active = true;
        vm->dpmi.virtual_interrupts_enabled = !initial;
        bool host_if = vm->dpmi.virtual_interrupts_enabled;
        vm->mem[0x1000] = enable ? 0xFB : 0xFA;
        vm->mem[0x1001] = enable ? 0xFB : 0xFA;
        cpu8086_state_t before = *cpu;
        ok &= cpu8086_run_one(vm);
        if ((mode == 3 ? 3u : cpl) > iopl) {
            ok &= dos_flags_test_fault(vm, &before, 13, 0, mode == 3 ? 0u : cpl);
            faults++;
        } else {
            uint32_t expected = (before.eflags & ~FLAG_IF) | (enable ? FLAG_IF : 0);
            ok &= cpu->eflags == expected && cpu->eip == 0x1001 && cpu->esp == before.esp &&
                  cpu->irq_shadow == (enable && !initial ? 1u : 0);
            ok &= cpu8086_run_one(vm) && cpu->eip == 0x1002 && !cpu->irq_shadow &&
                  cpu->eflags == expected && dos_test_general_registers(cpu, &before);
        }
        ok &= vm->dpmi.virtual_interrupts_enabled == host_if && cpu->cr2 == before.cr2;
        if (!ok) {
            if (failures < 8) { serial_puts("[DOS-FLAGS-IF] case="); serial_putdec(checks);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-FLAGS-IF] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_flags_lock_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint8_t opcodes[] = { 0x9C, 0x9D, 0x9E, 0x9F, 0xF5, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD };
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 1; mode < 4; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned op = 0; op < sizeof(opcodes); op++) {
        bool ok = dos_flags_test_prepare(vm, cpu, mode, mode == 2, 3);
        cpu->eflags = FLAGS_FIXED | FLAG_OF | FLAG_CF | FLAG_VIP | FLAG_VIF | (mode == 3 ? FLAG_VM : 0);
        /* Both #GP privilege rejection and an unusable VM86 operand compete
         * with LOCK, but #UD must win without attempting the operand. */
        if (mode == 3) dpmi_desc_set_limit(&cpu->ss_cache.descriptor, 0x7FFF);
        uint32_t p = 0x1000;
        vm->mem[p++] = 0xF0;
        if ((width == 4) != cpu->op_size_32) vm->mem[p++] = 0x66;
        vm->mem[p] = opcodes[op];
        dos_mem_write32(vm, 0x29000, 0xA5A55A5A);
        cpu8086_state_t before = *cpu;
        ok &= cpu8086_run_one(vm) && dos_flags_test_fault(vm, &before, 6, 0, mode == 3 ? 0u : 3u) &&
              dos_mem_read32(vm, 0x29000) == 0xA5A55A5A && cpu->cr2 == before.cr2;
        if (!ok) {
            if (failures < 8) { serial_puts("[DOS-FLAGS-LOCK] case="); serial_putdec(checks);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-FLAGS-LOCK] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_flags_host_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned mode = 1; mode <= 2; mode++)
    for (unsigned emulate = 0; emulate < 2; emulate++)
    for (unsigned initial = 0; initial < 2; initial++)
    for (unsigned width = 2; width <= 4; width += 2) {
        dos_fetch_test_prepare(vm, cpu, mode);
        vm->emulate_cpu = emulate;
        cpu->eflags = FLAGS_FIXED | FLAG_IF | (emulate ? FLAG_IOPL_MASK : 0);
        vm->dpmi.virtual_interrupts_enabled = initial;
        dos_io_write8(vm, 0x21, 0xFF); dos_io_write8(vm, 0xA1, 0xFF);
        uint32_t p = 0x1000;
        if ((width == 4) != cpu->op_size_32) vm->mem[p++] = 0x66;
        vm->mem[p++] = 0x9C;
        vm->mem[p++] = 0xFA;
        if ((width == 4) != cpu->op_size_32) vm->mem[p++] = 0x66;
        vm->mem[p++] = 0x9D;
        vm->mem[p++] = 0xFB;
        vm->mem[p] = 0xFB;
        uint32_t sp = cpu->esp, image = FLAGS_FIXED | (emulate ? FLAG_IOPL_MASK : 0) |
                                     (!emulate || initial ? FLAG_IF : 0);
        bool ok = cpu8086_run_one(vm) && cpu->esp == sp - width;
        uint32_t at = dpmi_desc_get_base(&cpu->ss_cache.descriptor) + cpu_stack_offset(cpu);
        ok &= dos_idt_test_read(vm, at, width) == image && cpu8086_run_one(vm) &&
              !vm->dpmi.virtual_interrupts_enabled && (cpu->eflags & FLAG_IF);
        ok &= cpu8086_run_one(vm) && cpu->esp == sp &&
              vm->dpmi.virtual_interrupts_enabled == (emulate && initial) &&
              cpu->eflags == (FLAGS_FIXED | FLAG_IF | (emulate ? FLAG_IOPL_MASK : 0));
        ok &= cpu8086_run_one(vm) && vm->dpmi.virtual_interrupts_enabled &&
              cpu->irq_shadow == (!(emulate && initial) ? 1u : 0);
        ok &= cpu8086_run_one(vm) && !cpu->irq_shadow;
        if (!ok) {
            if (failures < 8) { serial_puts("[DOS-FLAGS-HOST] case="); serial_putdec(checks);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-FLAGS-HOST] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_flags_paging_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    uint8_t *bytes = dos_host_alloc_pages(2);
    if (!bytes) return 1;
    unsigned checks = 0, faults = 0, retries = 0;
    int failures = 0;
    for (unsigned mode = 1; mode < 4; mode++)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned pop = 0; pop < 2; pop++)
    for (unsigned scenario = 0; scenario < (mode == 3 ? 7u : 5u); scenario++) {
        bool ok = dos_flags_test_prepare(vm, cpu, mode, mode == 2, 3);
        uint32_t base = mode == 3 ? 0x20000u : 0x403FF000u;
        if (mode != 3) {
            dpmi_descriptor_t stack = dos_idt_test_descriptor(vm, 2, base,
                mode == 2 ? UINT32_MAX : 0xFFFFu, 0xF3, mode == 2);
            cpu8086_cache_segment(cpu, 2, cpu->ss, &stack);
        }
        /* A separate ring-0 stack makes faults independent of the operand. */
        dos_idt_test_descriptor(vm, 5, 0x60000, 0xFFFF, 0x9B, true);
        dos_test_page_pair_t pair = dos_test_page_pair(vm, base);
        unsigned page = scenario ? (scenario - 1u) & 1u : 0;
        uint32_t entry = pair.pte[page], saved = dos_mem_read32(vm, entry);
        if (scenario) dos_mem_write32(vm, entry, saved & ~(scenario <= 2 || scenario >= 5 ? 1u : 2u));
        for (unsigned i = 0; i < 8192; i++) {
            bytes[i] = (uint8_t)(i * 13u + 0x5A);
            vm->mem[dos_string_test_physical(i)] = bytes[i];
        }
        cpu->esp = (mode == 2 ? 0 : 0xABCD0000u) | (4095u + (pop ? 0 : width));
        cpu->eflags = FLAGS_FIXED | FLAG_CF | FLAG_IF | FLAG_VIP | FLAG_VIF |
                      (scenario >= 5 ? 0 : FLAG_IOPL_MASK) | (mode == 3 ? FLAG_VM : 0);
        uint32_t p = 0x1000;
        if (scenario == 6) vm->mem[p++] = 0xF0;
        if ((width == 4) != cpu->op_size_32) vm->mem[p++] = 0x66;
        vm->mem[p++] = pop ? 0x9D : 0x9C;
        cpu8086_state_t before = *cpu;
        uint32_t value = 0;
        for (unsigned i = 0; i < width; i++) value |= (uint32_t)bytes[4095u + i] << (i * 8u);
        uint32_t expected_flags = pop ? dos_flags_test_pop(before.eflags, value, width, 3) : before.eflags;
        bool failed = scenario && (scenario <= 2 || !pop || scenario >= 5);
        unsigned vector = scenario == 6 ? 6u : scenario == 5 ? 13u : 14u;
        uint32_t error = 4u | (pop ? 0 : 2u) | (scenario >= 3 ? 1u : 0);
        uint32_t cr2 = base + (page ? 4096u : 4095u);
        uint32_t resumed_sp = before.esp;
        ok &= cpu8086_run_one(vm);
        if (failed) {
            ok &= dos_flags_test_fault(vm, &before, vector, vector == 14 ? error : 0, 0) &&
                  cpu->cr2 == (vector == 14 ? cr2 : before.cr2);
            for (unsigned i = 0; i < 8192; i++)
                if (vm->mem[dos_string_test_physical(i)] != bytes[i]) ok = false;
            faults++;
            if (vector == 14) {
                /* A protected 16-bit return SS retains the handler's ESP
                 * high word; VM86 IRETD instead restores the entire ESP. */
                if (mode == 1) resumed_sp &= 0xFFFFu;
                dos_mem_write32(vm, entry, saved);
                vm->mem[0x640E0] = 0x83; vm->mem[0x640E1] = 0xC4;
                vm->mem[0x640E2] = 4; vm->mem[0x640E3] = 0xCF;
                ok &= cpu8086_run_one(vm) && cpu8086_run_one(vm) &&
                      cpu->cs == before.cs && cpu->eip == before.eip && cpu->esp == resumed_sp;
                ok &= cpu8086_run_one(vm);
                retries++;
            }
        }
        if (!failed || vector == 14) {
            ok &= cpu->eip == p && cpu->cs == before.cs && cpu->ss == before.ss &&
                  cpu->esp == resumed_sp + (pop ? width : -width) &&
                  (cpu->eflags & ~FLAG_RF) == (expected_flags & ~FLAG_RF) &&
                  dos_test_general_registers(cpu, &before);
            if (!pop) for (unsigned i = 0; i < width; i++)
                bytes[4095u + i] = (before.eflags & ~(FLAG_RF | FLAG_VM)) >> (i * 8u);
            for (unsigned i = 0; i < 8192; i++)
                if (vm->mem[dos_string_test_physical(i)] != bytes[i]) ok = false;
        }
        if (!ok) {
            if (failures < 8) { serial_puts("[DOS-FLAGS-PAGING] case="); serial_putdec(checks);
                serial_puts(" IP="); serial_puthex(cpu->eip, 8);
                serial_puts(" CR2="); serial_puthex(cpu->cr2, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    dos_host_free_pages(bytes, 2);
    serial_puts("[DOS-FLAGS-PAGING] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" retries="); serial_putdec(retries);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_flags_jit_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint8_t opcodes[] = { 0x9C, 0x9D, 0xFA, 0xFB };
    unsigned checks = 0, faults = 0;
    int failures = 0;
    unsigned pages = (sizeof(jit_state_t) + 4095u) / 4096u;
    jit_state_t *jit = dos_host_alloc_pages(pages);
    if (!jit) return 1;
    jit_init(jit);
    if (!jit->code_buf) { dos_host_free_pages(jit, pages); return 1; }
    for (unsigned mode = 0; mode < 4; mode++)
    for (unsigned iopl = 0; iopl <= 3; iopl += 3)
    for (unsigned width = 2; width <= 4; width += 2)
    for (unsigned op = 0; op < sizeof(opcodes); op++) {
        bool prepared = dos_flags_test_prepare(vm, cpu, mode, mode == 2, 3);
        vm->jit = jit;
        cpu->eflags = FLAGS_FIXED | FLAG_CF | FLAG_VIP | FLAG_VIF |
                      (iopl << 12) | (mode == 3 ? FLAG_VM : 0);
        uint32_t p = 0x1000;
        vm->mem[p++] = 0x90;
        if ((width == 4) != cpu->op_size_32) vm->mem[p++] = 0x66;
        vm->mem[p++] = opcodes[op];
        cpu8086_state_t initial = *cpu;
        jit_block_t *block = jit_get_block(jit, cpu->cs, cpu->eip);
        prepared &= block && jit_decode_block(vm, block) > 0 && jit_compile_block(jit, block) == 0;
        for (unsigned hot = 0; hot < 2; hot++) {
            *cpu = initial;
            dos_mem_write32(vm, 0x29000, UINT32_MAX);
            uint64_t count = jit->jit_instructions;
            bool ok = prepared && !jit_exec_block(vm, block) && cpu->eip == 0x1001 &&
                      jit->jit_instructions == count + 1;
            cpu8086_state_t before = *cpu;
            ok &= cpu8086_run_one(vm);
            bool failed = mode && !iopl && (mode == 3 || op >= 2);
            if (failed) {
                ok &= dos_flags_test_fault(vm, &before, 13, 0, mode == 3 ? 0u : 3u);
                faults++;
            } else {
                uint32_t expected = op == 1 ? dos_flags_test_pop(before.eflags, UINT32_MAX, width, mode ? 3u : 0)
                                  : op == 3 ? before.eflags | FLAG_IF : before.eflags;
                ok &= cpu->eip == p && cpu->cs == before.cs && cpu->eflags == expected &&
                      cpu->esp == before.esp + (op == 0 ? -width : op == 1 ? width : 0) &&
                      cpu->irq_shadow == (op == 3 ? 1u : 0);
                if (!op) ok &= dos_idt_test_read(vm, 0x29000u - width, width) ==
                    ((before.eflags & ~(FLAG_RF | FLAG_VM)) & (width == 4 ? UINT32_MAX : 0xFFFFu));
            }
            if (!ok) {
                if (failures < 8) { serial_puts("[DOS-FLAGS-JIT] case="); serial_putdec(checks);
                    serial_puts(" IP="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
                failures++;
            }
            checks++;
        }
        vm->jit = NULL;
    }
    jit_destroy(jit);
    dos_host_free_pages(jit, pages);
    serial_puts("[DOS-FLAGS-JIT] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_flags_byte_irq_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, irqs = 0;
    int failures = 0;
    for (unsigned mode = 0; mode < 4; mode++)
    for (unsigned value = 0; value < 256; value++) {
        bool ok = dos_flags_test_prepare(vm, cpu, mode, mode == 2, 3);
        cpu->eflags = FLAGS_FIXED | FLAG_OF | FLAG_DF | FLAG_NT | FLAG_ID | FLAG_VIP |
                      (mode == 3 ? FLAG_VM : 0);
        cpu->ah = value;
        vm->mem[0x1000] = 0x9E; vm->mem[0x1001] = 0x9F;
        cpu8086_state_t before = *cpu;
        uint32_t flags = before.eflags | (value & 0xD5u);
        ok &= cpu8086_run_one(vm) && cpu->eflags == flags && cpu->eax == before.eax;
        ok &= cpu8086_run_one(vm) && cpu->eflags == flags && cpu->eip == 0x1002 &&
              cpu->eax == ((before.eax & ~0xFF00u) | ((value & 0xD5u) | FLAGS_FIXED) << 8) &&
              cpu->esp == before.esp;
        if (!ok) failures++;
        checks++;
    }
    for (unsigned mode = 1; mode < 4; mode++)
    for (unsigned following = 0; following < 3; following++) {
        bool ok = dos_flags_test_prepare(vm, cpu, mode, mode == 2, 3);
        cpu->eflags = FLAGS_FIXED | FLAG_IOPL_MASK | (mode == 3 ? FLAG_VM : 0);
        /* The suspended host says IF=0 throughout this guest-IDT sequence. */
        vm->dpmi.active = true;
        vm->dpmi.virtual_interrupts_enabled = false;
        vm->mem[0x1000] = 0xFB;
        vm->mem[0x1001] = following == 0 ? 0x90 : following == 1 ? 0xFB : 0xFA;
        dos_io_write8(vm, 0x20, 0x11); dos_io_write8(vm, 0x21, 8);
        dos_io_write8(vm, 0x21, 4); dos_io_write8(vm, 0x21, 1);
        dos_io_write8(vm, 0x21, 0xFE);
        vm->timer_irq_pending = true;
        ok &= cpu8086_run_one(vm) && cpu->irq_shadow == 1 && cpu->eip == 0x1001 &&
              !cpu8086_service_interrupts(vm) && vm->timer_irq_pending;
        ok &= cpu8086_run_one(vm) && !cpu->irq_shadow && cpu->eip == 0x1002;
        if (following == 2) {
            ok &= !cpu8086_service_interrupts(vm) && vm->timer_irq_pending;
        } else {
            ok &= cpu8086_service_interrupts(vm) && !vm->timer_irq_pending &&
                  cpu->eip == 0x4080 && cpu->cs == (mode == 3 ? 40u : 43u);
            irqs++;
        }
        ok &= !vm->dpmi.virtual_interrupts_enabled;
        if (!ok) failures++;
        checks++;
    }
    serial_puts("[DOS-FLAGS-BYTE-IRQ] checks="); serial_putdec(checks);
    serial_puts(" irqs="); serial_putdec(irqs);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static void dos_real_idt_test_prepare(dos_vm_t *vm, cpu8086_state_t *cpu,
                                       bool code32, bool stack32, uint32_t base)
{
    dos_step_test_prepare(vm, cpu, 0, 0);
    cpu->idtr.base = base;
    cpu->esp = stack32 ? 0x19000u : 0xABCD9000u;
    cpu->eip = code32 ? 0x11000u : 0x1000u;
    cpu->eflags = FLAGS_FIXED | FLAG_CF | FLAG_IF | FLAG_TF | FLAG_DF |
                  FLAG_IOPL_MASK | FLAG_AC | FLAG_RF;
    dpmi_descriptor_t code = cpu->cs_cache.descriptor;
    dpmi_desc_set_limit(&code, 0x1FFFF);
    if (code32) code.flags_lim |= DESC_32BIT;
    cpu8086_cache_cs(cpu, 0, &code, 0);
    dpmi_descriptor_t stack = cpu->ss_cache.descriptor;
    dpmi_desc_set_base(&stack, 0x20000);
    dpmi_desc_set_limit(&stack, stack32 ? 0x1FFFF : 0xFFFF);
    if (stack32) stack.flags_lim |= DESC_32BIT;
    cpu8086_cache_segment(cpu, 2, 0x2000, &stack);
    vm->software_int_frame_bytes = 0;
    vm->software_int_return_flags = 0xA5A51234;
    dos_io_write8(vm, 0x21, 0xFF);
    dos_io_write8(vm, 0xA1, 0xFF);
    for (unsigned v = 0; v < 256; v++)
        dos_mem_write32(vm, base + v * 4u, 0x4000u + v * 16u);
}

static bool dos_real_idt_test_frame(dos_vm_t *vm, const cpu8086_state_t *before,
                                     uint16_t ip)
{
    const cpu8086_state_t *cpu = vm->cpu;
    uint32_t mask = cpu_stack_addr32(before) ? UINT32_MAX : 0xFFFF;
    uint32_t values[] = { before->eflags, before->cs, ip };
    bool ok = cpu->esp == ((before->esp & ~mask) | ((before->esp - 6u) & mask));
    for (unsigned i = 0; i < 3; i++)
        ok &= dos_mem_read16(vm, 0x20000 + ((before->esp - (i + 1u) * 2u) & mask)) ==
              (uint16_t)values[i];
    return ok && dos_test_general_registers(cpu, before) && !cpu->delivery_fault;
}

static int dos_real_idt_entry_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint8_t vectors[] = { 0x21, 0x60, 0x80, DOS_RM_SERVICE_INT, 0xFF };
    unsigned checks = 0;
    int failures = 0;
    for (unsigned code32 = 0; code32 < 2; code32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned prefix = 0; prefix < 4; prefix++)
    for (unsigned at = 0; at < 3; at++)
    for (unsigned which = 0; which < sizeof(vectors); which++)
    for (unsigned relocated = 0; relocated < 2; relocated++) {
        dos_real_idt_test_prepare(vm, cpu, code32, stack32, relocated ? 0x1800 : 0);
        uint8_t vector = vectors[which];
        uint16_t segment = at == 1 ? 0xF800 : 0, target = at == 2 ? 0 : 0x4400;
        dos_mem_write32(vm, cpu->idtr.base + vector * 4u, ((uint32_t)segment << 16) | target);
        uint32_t handler = ((uint32_t)segment << 4) + target;
        if (code32) vm->mem[handler++] = 0x66;
        vm->mem[handler] = 0xCF;
        uint32_t next = cpu->eip;
        if (prefix & 1) vm->mem[next++] = 0x66;
        if (prefix & 2) vm->mem[next++] = 0x67;
        vm->mem[next++] = 0xCD; vm->mem[next++] = vector;
        cpu8086_state_t before = *cpu;
        bool ok = cpu8086_run_one(vm) && cpu->running && cpu->cs == segment && cpu->eip == target &&
            dos_real_idt_test_frame(vm, &before, (uint16_t)next) &&
            cpu->eflags == (before.eflags & ~(FLAG_IF | FLAG_TF | FLAG_AC | FLAG_RF)) &&
            cpu->op_size_32 == (code32 != 0) &&
            dpmi_desc_get_limit(&cpu->cs_cache.descriptor) == 0x1FFFF &&
            !vm->software_int_frame_bytes && vm->software_int_return_flags == 0xA5A51234;
        ok &= cpu8086_run_one(vm) && cpu->running && cpu->cs == before.cs &&
              cpu->eip == (uint16_t)next && cpu->esp == before.esp &&
              cpu->eflags == (before.eflags & ~(FLAG_AC | FLAG_RF));
        if (!ok) {
            if (failures < 8) { serial_puts("[DOS-REAL-IDT] case="); serial_putdec(checks);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n"); }
            failures++;
        }
        checks++;
    }
    dos_init_ivt(vm);
    serial_puts("[DOS-REAL-IDT] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_real_idt_fault_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0, faults = 0, stopped = 0;
    int failures = 0;
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned scenario = 0; scenario < 20; scenario++) {
        dos_real_idt_test_prepare(vm, cpu, false, stack32, 0x1800);
        uint32_t next = cpu->eip;
        uint8_t vector = 0x80;
        bool fatal = false, expected_fault = false, external = scenario == 6;
        if (scenario == 0) { cpu->idtr.limit = 13u * 4u + 3u; vector = 13; }
        if (scenario == 1) {
            cpu->idtr.limit = 13u * 4u + 2u; expected_fault = true;
        }
        if (scenario == 2 || external) {
            cpu->idtr.limit = 13u * 4u + 3u; expected_fault = true;
        }
        if (scenario == 3) { cpu->idtr.limit = 0; fatal = true; }
        if (scenario == 4) {
            dpmi_desc_set_limit(&cpu->cs_cache.descriptor, 0x3000);
            dos_mem_write32(vm, 0x1800 + 13u * 4u, 0x2000);
            expected_fault = true;
        }
        if (scenario == 5) { cpu->ss_cache.descriptor.access &= ~DESC_WRITABLE; fatal = true; }
        if (scenario >= 7 && scenario <= 14) {
            unsigned sp = scenario == 14 ? 0xFFFF : scenario - 7u;
            cpu->esp = stack32 ? sp : 0xABCD0000u | sp;
            fatal = stack32 ? sp < 6 : (sp < 6 && (sp & 1u));
        }
        if (scenario >= 15) {
            const uint8_t opcodes[] = { 0xCC, 0xCE, 0xF1, 0xF6, 0xF0 };
            vm->mem[next++] = opcodes[scenario - 15];
            if (scenario == 16) cpu->flags |= FLAG_OF;
            if (scenario == 18) { vm->mem[next++] = 0xF0; cpu->ax = 0; }
            if (scenario == 19) { vm->mem[next++] = 0xCD; vm->mem[next++] = 0x80; }
            vector = scenario == 15 ? 3 : scenario == 16 ? 4 : scenario == 17 ? 1 : scenario == 18 ? 0 : 6;
            if (scenario >= 18) { next = cpu->eip; expected_fault = true; }
        } else if (!external) {
            vm->mem[next++] = 0x66; vm->mem[next++] = 0xCD; vm->mem[next++] = vector;
        }
        if (expected_fault && scenario < 15) { next = cpu->eip; vector = scenario == 1 ? 8 : 13; }
        uint32_t mask = stack32 ? UINT32_MAX : 0xFFFF;
        for (unsigned i = 1; i <= 6; i++) {
            uint32_t offset = (cpu->esp - i) & mask;
            if (offset < 0x20000) vm->mem[0x20000 + offset] = 0xA5;
        }
        cpu8086_state_t before = *cpu;
        bool executed = external ? cpu_deliver_hw_interrupt(vm, 0x80) : cpu8086_run_one(vm);
        bool ok = true;
        if (fatal) {
            stopped++;
            ok = !cpu->running && cpu->exit_code == -1 && cpu->esp == before.esp &&
                 cpu->cs == before.cs && cpu->eflags == before.eflags && !cpu->delivery_fault;
            for (unsigned i = 1; i <= 6; i++) {
                uint32_t offset = (before.esp - i) & mask;
                if (offset < 0x20000 && vm->mem[0x20000 + offset] != 0xA5) ok = false;
            }
        } else {
            uint32_t target = scenario == 4 ? 0x2000 : 0x4000u + vector * 16u;
            ok = executed && cpu->running && !cpu->cs && cpu->eip == target &&
                 dos_real_idt_test_frame(vm, &before, (uint16_t)next);
            if (expected_fault) faults++;
        }
        if (!ok) {
            failures++; serial_puts("[DOS-REAL-IDT-FAULT] case="); serial_putdec(checks);
            serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n");
        }
        checks++;
    }
    /* IDT may alias the frame, including a same-CS:IP destination. */
    for (unsigned stack32 = 0; stack32 < 2; stack32++) {
        dos_real_idt_test_prepare(vm, cpu, false, stack32, 0x1800);
        uint32_t top = cpu->esp & (stack32 ? UINT32_MAX : 0xFFFF);
        cpu->idtr.base = 0x20000 + top - 6u - 0x80u * 4u;
        dos_mem_write32(vm, cpu->idtr.base + 0x80u * 4u, 0xFFFF1234);
        vm->mem[0x1000] = 0xCD; vm->mem[0x1001] = 0x80;
        cpu8086_state_t before = *cpu;
        if (!cpu8086_run_one(vm) || cpu->cs || cpu->eip != 0x1002 ||
            !dos_real_idt_test_frame(vm, &before, 0x1002)) failures++;
        checks++;
    }
    dos_init_ivt(vm);
    serial_puts("[DOS-REAL-IDT-FAULT] checks="); serial_putdec(checks);
    serial_puts(" faults="); serial_putdec(faults); serial_puts(" stopped="); serial_putdec(stopped);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_real_idt_jit_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    unsigned pages = (sizeof(jit_state_t) + 4095u) / 4096u;
    jit_state_t *jit = dos_host_alloc_pages(pages);
    if (!jit) return 1;
    jit_init(jit);
    if (!jit->code_buf) { dos_host_free_pages(jit, pages); return 1; }
    for (unsigned code32 = 0; code32 < 2; code32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        dos_real_idt_test_prepare(vm, cpu, code32, stack32, 0x1800);
        vm->jit = jit;
        uint32_t start = cpu->eip;
        vm->mem[start] = 0xCD; vm->mem[start + 1u] = 0x21;
        cpu8086_state_t initial = *cpu;
        jit_block_t *block = jit_get_block(jit, cpu->cs, start);
        bool prepared = block && jit_decode_block(vm, block) == 1 && jit_compile_block(jit, block) == 0;
        for (unsigned hot = 0; hot < 2; hot++) {
            *cpu = initial;
            /* Reuse native code while changing IDTR or its contents. */
            cpu->idtr.base = hot ? 0x2800 : 0x1800;
            for (unsigned v = 0; v < 256; v++) dos_mem_write32(vm, cpu->idtr.base + v * 4u, 0x4400);
            uint16_t segment = scenario == 1 ? 0xF800 : 0;
            uint16_t target = scenario == 2 ? 0 : 0x4400;
            dos_mem_write32(vm, cpu->idtr.base + 0x21u * 4u, ((uint32_t)segment << 16) | target);
            if (scenario == 3) cpu->idtr.limit = 13u * 4u + 3u;
            if (scenario == 4) cpu->ss_cache.descriptor.access &= ~DESC_WRITABLE;
            cpu8086_state_t before = *cpu;
            uint64_t count = jit->jit_instructions;
            bool ok = prepared && jit_block_current(vm, block) && jit_exec_block(vm, block) &&
                      jit->jit_instructions == count + 1;
            if (scenario == 4) ok &= !cpu->running && cpu->esp == before.esp;
            else ok &= cpu->running && cpu->cs == segment && cpu->eip == target &&
                 dos_real_idt_test_frame(vm, &before, (uint16_t)(scenario == 3 ? start : start + 2u));
            if (!ok) {
                failures++; serial_puts("[DOS-REAL-IDT-JIT] case="); serial_putdec(checks);
                serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n");
            }
            checks++;
        }
    }
    vm->jit = NULL;
    jit_destroy(jit);
    dos_host_free_pages(jit, pages);
    dos_init_ivt(vm);
    serial_puts("[DOS-REAL-IDT-JIT] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_real_idt_host_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned scenario = 0; scenario < 8; scenario++) {
        dos_real_idt_test_prepare(vm, cpu, false, stack32, 0x1800);
        dos_init_ivt(vm);
        cpu->idtr.base = 0;
        cpu->flags &= ~FLAG_TF;
        cpu->ax = 0x4400; cpu->bx = 0xFFFF;
        uint32_t before_sp = cpu->esp;
        const uint16_t outer_flags = FLAGS_FIXED | FLAG_IF | FLAG_DF | FLAG_IOPL_MASK;
        uint32_t mask = stack32 ? UINT32_MAX : 0xFFFF;
        uint16_t stub = DOS_RM_SERVICE_BASE_OFF + 0x21u * DOS_RM_SERVICE_STUB_SIZE;
        if (scenario) {
            cpu8086_load_real_cs(cpu, DOS_ROM_BASE >> 4);
            cpu->eip = stub;
            dos_mem_write16(vm, 0x20000 + (before_sp & mask), 0x1234);
            dos_mem_write16(vm, 0x20000 + ((before_sp + 2u) & mask), 0);
            dos_mem_write16(vm, 0x20000 + ((before_sp + 4u) & mask), outer_flags);
        } else {
            vm->mem[0x1000] = 0x66; vm->mem[0x1001] = 0xCD; vm->mem[0x1002] = 0x21;
        }
        /* Provenance, not IDTR, identifies a private ROM bridge call. */
        if (scenario == 2) cpu->idtr.limit = 0;
        if (scenario == 3) {
            dpmi_desc_set_base(&cpu->cs_cache.descriptor, 0x40000);
            vm->mem[0x40000 + stub] = 0xCD;
            vm->mem[0x40001 + stub] = DOS_RM_SERVICE_INT;
            dos_mem_write32(vm, DOS_RM_SERVICE_INT * 4u, 0x4400);
        }
        if (scenario == 4) {
            dpmi_desc_set_limit(&cpu->ss_cache.descriptor, (before_sp & mask) + 4u);
        }
        if (scenario == 5) {
            /* A guest hook in the ROM range is still executable code. */
            cpu8086_reset_real_cs(cpu, 0); cpu->eip = 0x1000;
            vm->mem[0x1000] = 0xCD; vm->mem[0x1001] = 0x21;
            dos_mem_write32(vm, 0x21u * 4u, 0xF8000200);
        }
        if (scenario == 6) {
            cpu8086_reset_real_cs(cpu, 0); cpu->eip = 0x1000;
            vm->mem[0x1000] = 0xCD; vm->mem[0x1001] = 0x21;
            cpu->idtr.limit = 0;
        }
        if (scenario == 7) {
            cpu8086_reset_real_cs(cpu, 0); cpu->eip = 0x1000;
            vm->mem[0x1000] = 0xCD; vm->mem[0x1001] = 0x21;
            vm->mem[DOS_ROM_BASE + stub] = 0x90;
        }
        cpu8086_state_t before = *cpu;
        (void)cpu8086_run_one(vm);
        bool ok;
        if (scenario == 4 || scenario == 6) {
            ok = !cpu->running && cpu->exit_code == -1 && cpu->ax == 0x4400 &&
                 !cpu->delivery_fault;
        } else if (scenario == 3 || scenario >= 5) {
            uint16_t target = scenario == 3 ? 0x4400 : scenario == 5 ? 0x200 : stub;
            uint16_t segment = scenario == 3 ? 0 : scenario == 5 ? 0xF800 : 0xF000;
            ok = cpu->running && cpu->cs == segment && cpu->eip == target &&
                 dos_real_idt_test_frame(vm, &before, scenario == 3 ? stub + 2u : 0x1002);
        } else {
            ok = cpu->running && cpu->esp == before_sp && cpu->ax == 6 && (cpu->flags & FLAG_CF) &&
                 cpu->cs == before.cs && cpu->eip == (scenario ? stub + 2u : 0x1003u);
            if (scenario) {
                ok &= dos_mem_read16(vm, 0x20000 + ((before_sp + 4u) & mask)) == (outer_flags | FLAG_CF);
                ok &= cpu8086_run_one(vm) && cpu->running && !cpu->cs && cpu->eip == 0x1234 &&
                      cpu->esp == before_sp + 6u && cpu->flags == (outer_flags | FLAG_CF);
            }
        }
        if (!ok) {
            failures++; serial_puts("[DOS-REAL-IDT-HOST] case="); serial_putdec(checks);
            serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n");
        }
        checks++;
    }
    dos_init_ivt(vm);
    serial_puts("[DOS-REAL-IDT-HOST] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_real_idt_timer_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    unsigned pages = (sizeof(jit_state_t) + 4095u) / 4096u;
    jit_state_t *jit = dos_host_alloc_pages(pages);
    if (!jit) return 1;
    jit_init(jit);
    if (!jit->code_buf) { dos_host_free_pages(jit, pages); return 1; }
    for (unsigned width = 0; width < 2; width++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned translated = 0; translated < 2; translated++)
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        dos_real_idt_test_prepare(vm, cpu, false, stack32, 0x1800);
        vm->step_limit = 128;
        vm->dpmi.active = scenario != 2;
        vm->dpmi.is_32bit = width != 0;
        uint16_t code = dpmi_index_to_sel(1);
        dos_test_descriptor(&vm->dpmi, code, true, width != 0);
        vm->dpmi.pm_vectors[0x1C].sel = code;
        vm->dpmi.pm_vectors[0x1C].off = 0x6000;
        if (scenario == 3) {
            vm->dpmi.pm_vectors[0x1C].sel = dpmi_get_host_code_selector(vm);
            vm->dpmi.pm_vectors[0x1C].off = DPMI_PM_REFLECT_BASE_OFF +
                                         0x1Cu * DPMI_PM_REFLECT_STUB_SIZE;
        }
        /* A host-owned timer does not consult the raw IDTR. */
        if (scenario == 1) cpu->idtr.limit = 0;
        dos_mem_write32(vm, 0x1800 + 0x1Cu * 4u, 0x4400);
        uint32_t handler = 0x6000;
        if (width) vm->mem[handler++] = 0x66;
        vm->mem[handler++] = 0xBB; /* MOV BX,5A3C; IRET */
        vm->mem[handler++] = 0x3C; vm->mem[handler++] = 0x5A;
        vm->mem[handler] = 0xCF;
        vm->mem[0x1000] = 0xCD; vm->mem[0x1001] = 0x1C;
        cpu->flags &= ~FLAG_TF;
        cpu->ebx = 0xABCD1234;
        cpu8086_state_t before = *cpu;
        bool ok;
        if (translated) {
            vm->jit = jit;
            jit_block_t *block = jit_get_block(jit, 0, 0x1000);
            ok = block && ((block->compiled && jit_block_current(vm, block)) ||
                 (jit_decode_block(vm, block) == 1 && jit_compile_block(jit, block) == 0));
            uint64_t count = jit->jit_instructions;
            ok = ok && jit_exec_block(vm, block) && jit->jit_instructions >= count + 1;
        } else {
            /* The host service also executes the two-instruction PM handler. */
            (void)cpu8086_run_one(vm);
            ok = cpu->insn_count == before.insn_count + (scenario < 2 ? 3u : 1u);
        }
        if (scenario < 2) {
            ok &= cpu->running && !cpu->protected_mode && !cpu->cs && cpu->eip == 0x1002 &&
                  cpu->esp == before.esp && cpu->ebx == 0xABCD5A3C &&
                  cpu->eflags == (before.eflags & ~(FLAG_AC | FLAG_RF)) &&
                  !vm->dpmi.control_depth && !vm->software_int_frame_bytes &&
                  vm->software_int_return_flags == 0xA5A51234 && !cpu->delivery_fault;
        } else {
            ok &= cpu->running && !cpu->cs && cpu->eip == 0x4400 &&
                  dos_real_idt_test_frame(vm, &before, 0x1002);
        }
        if (!ok) {
            failures++; serial_puts("[DOS-REAL-IDT-TIMER] case="); serial_putdec(checks);
            serial_puts(" ip="); serial_puthex(cpu->eip, 8); serial_puts("\n");
        }
        vm->jit = NULL;
        checks++;
    }
    jit_destroy(jit);
    dos_host_free_pages(jit, pages);
    vm->step_limit = 0;
    dos_init_ivt(vm);
    serial_puts("[DOS-REAL-IDT-TIMER] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_real_idt_control_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    unsigned checks = 0;
    int failures = 0;
    unsigned pages = (sizeof(jit_state_t) + 4095u) / 4096u;
    jit_state_t *jit = dos_host_alloc_pages(pages);
    if (!jit) return 1;
    jit_init(jit);
    if (!jit->code_buf) { dos_host_free_pages(jit, pages); return 1; }
    for (unsigned width = 0; width < 2; width++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned translated = 0; translated < 2; translated++)
    for (unsigned critical = 0; critical < 2; critical++)
    for (unsigned scenario = 0; scenario < 11; scenario++) {
        dos_real_idt_test_prepare(vm, cpu, false, stack32, 0x1800);
        vm->step_limit = 128;
        vm->dpmi.active = scenario != 2;
        vm->dpmi.is_32bit = width != 0;
        vm->dpmi.virtual_interrupts_enabled = false;
        uint8_t vector = critical ? 0x24 : 0x23;
        uint16_t code = dpmi_index_to_sel(1);
        dos_test_descriptor(&vm->dpmi, code, true, width != 0);
        vm->dpmi.pm_vectors[vector].sel = code;
        vm->dpmi.pm_vectors[vector].off = 0x6000;
        if (scenario == 3) {
            vm->dpmi.pm_vectors[vector].sel = dpmi_get_host_code_selector(vm);
            vm->dpmi.pm_vectors[vector].off = DPMI_PM_REFLECT_BASE_OFF +
                                             vector * DPMI_PM_REFLECT_STUB_SIZE;
        } else if (scenario == 4) vm->dpmi.pm_vectors[vector].sel = 0;
        if (scenario == 1) cpu->idtr.limit = 0;
        if (scenario == 8) cpu->esp = stack32 ? 0x1FFFA : 0xABCDFFFA;
        if (scenario == 8 && stack32) dpmi_desc_set_limit(&cpu->ss_cache.descriptor, 0x20040);
        dos_mem_write32(vm, 0x1800 + vector * 4u, 0x4400);
        uint8_t action = scenario == 6 ? 2 : scenario == 7 ? 0x7F : 1;
        uint32_t handler = 0x6000;
        if (width) vm->mem[handler++] = 0x66;
        vm->mem[handler++] = 0xBB; /* MOV BX,5A3C; MOV AL,action; STC; IRET/RETF */
        vm->mem[handler++] = 0x3C; vm->mem[handler++] = 0x5A;
        vm->mem[handler++] = 0xB0; vm->mem[handler++] = action;
        vm->mem[handler++] = 0xF9;
        vm->mem[handler] = scenario == 5 ? 0xCB : 0xCF;
        vm->mem[0x1000] = 0xCD; vm->mem[0x1001] = vector;
        cpu->eflags &= ~(FLAG_TF | (scenario == 10 ? FLAG_IF : 0));
        cpu->eax = 0xCAFE1200; cpu->ebx = 0xABCD1234;
        uint32_t mask = stack32 ? UINT32_MAX : 0xFFFF;
        for (unsigned i = 0; i < 12; i++)
            dos_mem_write16(vm, 0x20000 + ((cpu->esp + i * 2u) & mask), 0x3100u + i);
        if (scenario == 9) dpmi_desc_set_limit(&cpu->ss_cache.descriptor,
                                               (cpu->esp & mask) + 8u);
        cpu8086_state_t before = *cpu;
        bool ok = true;
        if (translated) {
            vm->jit = jit;
            jit_block_t *block = jit_get_block(jit, 0, 0x1000);
            ok = block && ((block->compiled && jit_block_current(vm, block)) ||
                 (jit_decode_block(vm, block) == 1 && jit_compile_block(jit, block) == 0));
            uint64_t count = jit->jit_instructions;
            ok = ok && jit_exec_block(vm, block) && jit->jit_instructions >= count + 1;
        } else (void)cpu8086_run_one(vm);
        bool called = scenario != 2 && scenario != 3 && scenario != 4 &&
                      !(critical && scenario == 9);
        if (scenario == 2) {
            ok &= cpu->running && !cpu->cs && cpu->eip == 0x4400 &&
                  dos_real_idt_test_frame(vm, &before, 0x1002);
        } else {
            uint8_t expected = critical ?
                (!called || scenario == 5 || action >= 2 ? 3 : action) : called ? action : 0;
            ok &= cpu->running && !cpu->protected_mode && !cpu->cs && cpu->eip == 0x1002 &&
                  cpu->esp == before.esp && cpu->eax == (0xCAFE1200u | expected) &&
                  cpu->ebx == (!critical && called ? 0xABCD5A3Cu : before.ebx) &&
                  cpu->ebp == before.ebp && cpu->eflags == (before.eflags & ~(FLAG_AC | FLAG_RF)) &&
                  !vm->dpmi.control_depth && !vm->software_int_frame_bytes &&
                  vm->software_int_return_flags == 0xA5A51234 && !cpu->delivery_fault &&
                  !vm->dpmi.virtual_interrupts_enabled;
            if (critical && called) {
                uint16_t ss = dpmi_get_exception_stack_selector(vm);
                uint32_t tail = dpmi_translate(vm, ss, DPMI_EXCEPTION_STACK_SIZE - 24u);
                for (unsigned i = 0; i < 12; i++)
                    ok &= dos_mem_read16(vm, tail + i * 2u) == 0x3100u + i;
            }
        }
        if (!ok) {
            failures++; serial_puts("[DOS-REAL-IDT-CONTROL] case="); serial_putdec(checks);
            serial_puts(" ip="); serial_puthex(cpu->eip, 8);
            serial_puts(" ax="); serial_puthex(cpu->eax, 8); serial_puts("\n");
        }
        vm->jit = NULL;
        checks++;
    }
    jit_destroy(jit);
    dos_host_free_pages(jit, pages);
    vm->step_limit = 0;
    dos_init_ivt(vm);
    serial_puts("[DOS-REAL-IDT-CONTROL] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    return failures;
}

static int dos_real_idt_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    bool emulate = vm->emulate_cpu;
    uint8_t previous_bytes = vm->software_int_frame_bytes;
    uint32_t previous_flags = vm->software_int_return_flags;
    if (!dos_io_init(vm)) return 1;
    int failures = dos_real_idt_entry_selftest(vm, cpu);
    failures += dos_real_idt_fault_selftest(vm, cpu);
    failures += dos_real_idt_jit_selftest(vm, cpu);
    failures += dos_real_idt_host_selftest(vm, cpu);
    failures += dos_real_idt_timer_selftest(vm, cpu);
    failures += dos_real_idt_control_selftest(vm, cpu);
    dos_io_shutdown(vm);
    vm->emulate_cpu = emulate;
    vm->software_int_frame_bytes = previous_bytes;
    vm->software_int_return_flags = previous_flags;
    return failures;
}

static int dos_string_io_selftest(dos_vm_t *vm, cpu8086_state_t *cpu)
{
    const uint8_t prefixes[] = { 0, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65 };
    const uint32_t bases[] = { 0x10000, 0x20000, 0, 0, 0x10000, 0x30000, 0x40000 };
    const unsigned counts[] = { 0, 1, 3, 257 };
    unsigned checks = 0;
    int failures = 0;
    if (!dos_io_init(vm)) return 1;
    failures += dos_flags_stack_selftest(vm, cpu);
    failures += dos_flags_interrupt_selftest(vm, cpu);
    failures += dos_flags_lock_selftest(vm, cpu);
    failures += dos_flags_host_selftest(vm, cpu);
    failures += dos_flags_paging_selftest(vm, cpu);
    failures += dos_flags_jit_selftest(vm, cpu);
    failures += dos_flags_byte_irq_selftest(vm, cpu);
    failures += dos_task_roundtrip_selftest(vm, cpu);
    failures += dos_task_admission_selftest(vm, cpu);
    failures += dos_task_postcommit_selftest(vm, cpu);
    failures += dos_task_v86_selftest(vm, cpu);
    failures += dos_task_debug_cache_selftest(vm, cpu);
    failures += dos_task_paging_selftest(vm, cpu);
    failures += dos_task_address_space_selftest(vm, cpu);
    failures += dos_task_jit_selftest(vm, cpu);
    failures += dos_idt_jit_selftest(vm, cpu);
    failures += dos_idt_gate_selftest(vm, cpu);
    failures += dos_idt_source_selftest(vm, cpu);
    failures += dos_idt_admission_selftest(vm, cpu);
    failures += dos_idt_nested_selftest(vm, cpu);
    failures += dos_idt_paging_selftest(vm, cpu);
    failures += dos_idt_v86_selftest(vm, cpu);
    failures += dos_iret_flags_selftest(vm, cpu);
    failures += dos_iret_real_boundary_selftest(vm, cpu);
    failures += dos_iret_outer_selftest(vm, cpu);
    failures += dos_iret_fault_selftest(vm, cpu);
    failures += dos_iret_v86_selftest(vm, cpu);
    failures += dos_iret_paging_selftest(vm, cpu);
    failures += dos_data_cache_selftest(vm, cpu);
    failures += dos_cs_cache_selftest(vm, cpu);
    for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned input = 0; input < 2; input++)
    for (unsigned size = 1; size <= 4; size *= 2)
    for (unsigned adr32 = 0; adr32 < 2; adr32++)
    for (unsigned backwards = 0; backwards < 2; backwards++)
    for (unsigned repeat = 0; repeat < 3; repeat++)
    for (unsigned seg = 0; seg < sizeof(prefixes); seg++)
    for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        dos_port_test_prepare(vm, cpu, mode);
        uint32_t flags = FLAGS_FIXED | FLAG_CF | FLAG_OF | FLAG_AF |
                         (backwards ? FLAG_DF : FLAG_ZF);
        cpu->eflags = flags;
        cpu->eax = 0xA1B2C3D4u;
        cpu->edx = 0xBAD003C9u;
        cpu->ecx = (adr32 ? 0 : 0x12340000u) | counts[c];
        cpu->esi = cpu->edi = (adr32 ? 0 : 0x56780000u) | 0x4800u;
        unsigned count = repeat ? counts[c] : 1;
        uint32_t base = input ? 0x20000u : bases[seg];
        int32_t delta = backwards ? -(int32_t)size : (int32_t)size;
        for (unsigned i = 0; i < 260; i++)
        for (unsigned b = 0; b < size; b++)
            vm->mem[base + 0x4800u + i * delta + b] =
                input ? 0xA5 : (uint8_t)(7u + i + b * 17u);
        uint32_t p = 0x1000;
        if ((size == 4) != cpu->op_size_32) vm->mem[p++] = 0x66;
        if (adr32 != cpu->addr_size_32) vm->mem[p++] = 0x67;
        if (prefixes[seg]) vm->mem[p++] = prefixes[seg];
        if (repeat) vm->mem[p++] = repeat == 1 ? 0xF3 : 0xF2;
        vm->mem[p++] = (input ? 0x6C : 0x6E) | (size != 1);
        bool ok = dos_test_run_one(vm);
        unsigned done = count > 256u ? 256u : count;
        uint32_t expected_count = (adr32 ? 0 : 0x12340000u) |
                                  (repeat ? count - done : counts[c]);
        uint32_t advanced = (adr32 ? 0 : 0x56780000u) |
                             (0x4800u + done * delta);
        if (cpu->eip != (done < count ? 0x1000u : p) ||
            cpu->ecx != expected_count || cpu->eflags != flags ||
            cpu->eax != 0xA1B2C3D4u || cpu->edx != 0xBAD003C9u ||
            (input ? cpu->edi : cpu->esi) != advanced ||
            (input ? cpu->esi : cpu->edi) != ((adr32 ? 0 : 0x56780000u) | 0x4800u))
            ok = false;
        if (done < count && ok) {
            if (!dos_test_run_one(vm) || cpu->eip != p ||
                cpu->ecx != (adr32 ? 0 : 0x12340000u) ||
                (input ? cpu->edi : cpu->esi) !=
                    ((adr32 ? 0 : 0x56780000u) | (0x4800u + count * delta))) ok = false;
        }
        if (input) {
            for (unsigned i = 0; i <= count; i++)
            for (unsigned b = 0; b < size; b++) {
                uint8_t expected = i == count ? 0xA5 :
                    b == 0 ? i & 63u : b == 1 ? 0x35 : b == 2 ? 0xFF : 0x6B;
                if (vm->mem[base + 0x4800u + i * delta + b] != expected) ok = false;
            }
            if (dos_io_read8(vm, 0x3C9) != (count & 63u)) ok = false;
        } else {
            if (dos_io_read8(vm, 0x3C8) != count / 3u) ok = false;
            dos_io_write8(vm, 0x3C7, 0);
            for (unsigned i = 0; i <= count; i++)
                if (dos_io_read8(vm, 0x3C9) !=
                    (i == count ? i & 63u : (7u + i) & 63u)) ok = false;
        }
        if (!ok) {
            if (failures < 8) {
                serial_puts("[DOS-STRINGIO] mismatch mode="); serial_putdec(mode);
                serial_puts(" input="); serial_putdec(input);
                serial_puts(" size="); serial_putdec(size);
                serial_puts(" addr32="); serial_putdec(adr32);
                serial_puts(" df="); serial_putdec(backwards);
                serial_puts(" rep="); serial_putdec(repeat);
                serial_puts(" seg="); serial_putdec(seg);
                serial_puts(" count="); serial_putdec(counts[c]); serial_puts("\n");
            }
            failures++;
        }
        checks++;
    }
    serial_puts("[DOS-STRINGIO] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
    failures += dos_far_contract_selftest(vm, cpu);
    failures += dos_far_privilege_selftest(vm, cpu);
    failures += dos_far_gate_selftest(vm, cpu);
    failures += dos_far_paging_selftest(vm, cpu);
    failures += dos_far_gate_fault_selftest(vm, cpu);
    failures += dos_far_boundary_selftest(vm, cpu);
    failures += dos_system_load_selftest(vm, cpu);
    failures += dos_system_encoding_selftest(vm, cpu);
    failures += dos_system_boundary_selftest(vm, cpu);
    failures += dos_system_paging_selftest(vm, cpu);
    failures += dos_system_cache_selftest(vm, cpu);
    failures += dos_control_group7_selftest(vm, cpu);
    failures += dos_control_move_selftest(vm, cpu);
    failures += dos_control_bits_selftest(vm, cpu);
    failures += dos_control_paging_selftest(vm, cpu);
    failures += dos_control_transition_selftest(vm, cpu);
    failures += dos_port_boundary_selftest(vm, cpu);
    failures += dos_scalar_port_selftest(vm, cpu);
    failures += dos_paged_operand_selftest(vm, cpu);
    failures += dos_modrm_paging_selftest(vm, cpu);
    failures += dos_modrm_boundary_selftest(vm, cpu);
    failures += dos_memory_record_selftest(vm, cpu);
    failures += dos_modrm_vga_selftest(vm, cpu);
    failures += dos_string_memory_selftest(vm, cpu);
    failures += dos_string_paging_selftest(vm, cpu);
    failures += dos_string_boundary_selftest(vm, cpu);
    failures += dos_string_restart_selftest(vm, cpu);
    failures += dos_string_vga_selftest(vm, cpu);
    failures += dos_checked_stack_selftest(vm, cpu);
    failures += dos_stack_segment_selftest(vm, cpu);
    failures += dos_enter_stack_selftest(vm, cpu);
    failures += dos_enter_paging_selftest(vm, cpu);
    failures += dos_stack_vga_selftest(vm, cpu);
    failures += dos_stack_wrap_selftest(vm, cpu);
    failures += dos_exception_esp_selftest(vm, cpu);
    failures += dos_segment_load_selftest(vm, cpu);
    failures += dos_segment_encoding_selftest(vm, cpu);
    failures += dos_segment_privilege_selftest(vm, cpu);
    failures += dos_segment_paging_selftest(vm, cpu);
    failures += dos_segment_irq_selftest(vm, cpu);
    failures += dos_near_branch_selftest(vm, cpu);
    failures += dos_near_stack_selftest(vm, cpu);
    failures += dos_near_boundary_selftest(vm, cpu);
    failures += dos_fetch_selftest(vm, cpu);
    failures += dos_fetch_vga_selftest(vm, cpu);
    failures += dos_descriptor_paging_selftest(vm, cpu);
    dos_io_shutdown(vm);
    dos_step_test_prepare(vm, cpu, 0, 0);
    vm->emulate_cpu = false;
    return failures;
}

int dos_interrupt_selftest(void)
{
    extern int x86_compat_iret_selftest(void);

    const uint64_t pages = (2u * 1024u * 1024u) / 4096u;
    uint8_t *memory = (uint8_t *)dos_host_alloc_pages(pages);
    if (!memory) return 1;
    for (uint64_t i = 0; i < pages * 4096u; i++) memory[i] = 0;

    dos_vm_t vm = {0};
    cpu8086_state_t cpu;
    vm.mem = memory;
    vm.total_mem_size = pages * 4096u;
    vm.cpu = &cpu;
    dos_init_ivt(&vm);

    int failures = 0;
#define DOS_INTERRUPT_TEST(test) do { \
        int count = (test); \
        if (count) { \
            serial_puts("[DOS-INT-TEST] " #test " failures="); \
            serial_putdec((uint64_t)count); serial_puts("\n"); \
        } \
        failures += count; \
    } while (0)
    DOS_INTERRUPT_TEST(x86_compat_iret_selftest());
    DOS_INTERRUPT_TEST(dos_real_idt_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_selector_instruction_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_bit_instruction_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_dpmi_exception_frame_selftest(&vm, &cpu, false, false));
    DOS_INTERRUPT_TEST(dos_dpmi_exception_frame_selftest(&vm, &cpu, false, true));
    DOS_INTERRUPT_TEST(dos_dpmi_exception_frame_selftest(&vm, &cpu, true, false));
    DOS_INTERRUPT_TEST(dos_dpmi_exception_frame_selftest(&vm, &cpu, true, true));
    DOS_INTERRUPT_TEST(dos_dpmi_extended_exception_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_dpmi_real_exception_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_dpmi_locked_stack_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_dpmi_paged_frame_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_dpmi_private_frame_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_dpmi_software_frame_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_native_segments_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_dpmi_return_destination_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_dpmi_interrupt_stack_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_dpmi_default_exception_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_stack_instruction_selftest(&vm, &cpu, false, false));
    DOS_INTERRUPT_TEST(dos_stack_instruction_selftest(&vm, &cpu, false, true));
    DOS_INTERRUPT_TEST(dos_stack_instruction_selftest(&vm, &cpu, true, false));
    DOS_INTERRUPT_TEST(dos_stack_instruction_selftest(&vm, &cpu, true, true));
    DOS_INTERRUPT_TEST(dos_dpmi_virtual_interrupt_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_dpmi_default_interrupt_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_pop_instruction_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_group5_instruction_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_accumulator_instruction_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_loop_instruction_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_group3_instruction_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_scalar_instruction_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_far_pointer_instruction_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_step_limit_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_jit_cpu_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_page_walk_selftest(&vm, &cpu));
    DOS_INTERRUPT_TEST(dos_string_io_selftest(&vm, &cpu));
#undef DOS_INTERRUPT_TEST

    /* Execute real-mode HLT and ICEBP. A pending IRQ0 must wake HLT, then
     * vector 1 must run and IRET to the byte following ICEBP. */
    for (uint64_t i = 0; i < pages * 4096u; i++) memory[i] = 0;
    dos_init_ivt(&vm);
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

    int result = dos_test_run(&vm);
    if (result != 0x2A || cpu.bx != 0x5678 || cpu.halted ||
        vm.timer_irq_pending) {
        serial_puts("[DOS-INT-TEST] real-mode HLT/ICEBP failed\n");
        failures++;
    }

    /* An unavailable x87 raises #NM at the instruction boundary. The guest
     * handler advances saved IP by two bytes and returns through IRET. */
    for (uint64_t i = 0; i < pages * 4096u; i++) memory[i] = 0;
    dos_init_ivt(&vm);
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

    result = dos_test_run(&vm);
    if (result != 0x2A || cpu.bx != 0x5678 || cpu.running) {
        serial_puts("[DOS-INT-TEST] real-mode #NM recovery failed\n");
        failures++;
    }

    /* CPUID exposes only the virtual CPU contract implemented above. UD2
     * must enter vector 6 at its first byte so a guest handler can recover.
     * A prefixed near Jcc also validates its full 32-bit displacement. */
    for (uint64_t i = 0; i < pages * 4096u; i++) memory[i] = 0;
    dos_init_ivt(&vm);
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

    result = dos_test_run(&vm);
    if (result != 0x2A || cpu.edi != 1U || cpu.ebx != 0x7469734FU ||
        cpu.edx != 0x4D564B6FU || cpu.ecx != 0x20555043U ||
        cpu.bp != 0x5678 || cpu.si != 0x5678 || cpu.running) {
        serial_puts("[DOS-INT-TEST] real-mode CPUID/#UD recovery failed\n");
        failures++;
    }

    dos_host_free_pages(memory, pages);
    return failures;
}
