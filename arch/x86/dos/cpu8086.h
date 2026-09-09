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
#include "dos_io.h"
#include "../include/interrupt.h"

/* ── CPU state ──────────────────────────────────────────────────── */

typedef struct cpu_system_segment {
    dpmi_descriptor_t descriptor;
    bool valid;
} cpu_system_segment_t;

typedef struct {
    uint32_t error, return_eip;
    uint8_t vector;
    bool raised, has_error, task_trap;
} cpu_event_fault_t;

typedef enum {
    CPU_EVENT_EXCEPTION, CPU_EVENT_SOFTWARE, CPU_EVENT_SOFTWARE_EXCEPTION,
    CPU_EVENT_EXTERNAL, CPU_EVENT_PRIVILEGED_TRAP
} cpu_event_kind_t;

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
    uint32_t cr2;         /* last checked guest page-fault linear address */
    uint32_t cr3;         /* guest page directory base */
    uint32_t dr[8];       /* VCPI-visible debug register state */

    /* Descriptor table registers */
    struct { uint16_t limit; uint32_t base; } gdtr;
    struct { uint16_t limit; uint32_t base; } idtr;

    /* ── Mode state ───────────────────────────────────────────── */
    bool     protected_mode;   /* protected-selector semantics are active */
    bool     pm_cs_loaded;     /* true after first far JMP/RETF loads PM selector into CS */
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

    /* Appended so the fixed offsets consumed by the JIT remain unchanged. */
    uint16_t ldtr;
    uint16_t tr;
    uint32_t cs_mode_log_count;

    /* Only an internal REP chunk may retain the original comparison FLAGS.
     * Interrupts, faults and native-frame imports end this continuation. */
    struct {
        uint32_t eip, next_eip, eflags;
        uint16_t cs;
        uint8_t opcode, width, rep_type;
        int8_t seg_override;
        bool adr32, active;
    } rep_compare;

    /* Interpreter instruction boundaries remaining after MOV/POP SS or STI. */
    uint8_t irq_shadow;

    cpu_system_segment_t ldt_cache, tss_cache;
    /* The DPMI-owned LDT is a host context, not architectural LLDT null. */
    bool host_ldt;
    union {
        struct {
            cpu_system_segment_t es_cache, cs_cache, ss_cache;
            cpu_system_segment_t ds_cache, fs_cache, gs_cache;
        };
        cpu_system_segment_t segment_cache[6];
    };
    uint8_t cpl;
    /* VCPI/guest VM86 owns its IDT; the host's virtual v86 monitor does not. */
    bool guest_idt;
    /* Temporary synchronous delivery probe, never retained by a guest frame. */
    cpu_event_fault_t *delivery_fault;
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
#define FLAG_IOPL_MASK (3 << 12)
#define FLAG_NT   (1 << 14)
#define FLAG_RF   (1 << 16)
#define FLAG_VM   (1 << 17)
#define FLAG_AC   (1 << 18)
#define FLAG_VIF  (1 << 19)
#define FLAG_VIP  (1 << 20)
#define FLAG_ID   (1 << 21)

/* Always-1 bits in flags register */
#define FLAGS_FIXED  0x0002

static inline unsigned cpu8086_cpl(const cpu8086_state_t *cpu)
{
    if ((cpu->cr0 & 1u) && (cpu->eflags & FLAG_VM)) return 3u;
    return cpu->protected_mode ? cpu->cpl : 0u;
}

static inline bool cpu8086_uses_guest_idt(const cpu8086_state_t *cpu)
{
    return cpu && (cpu->protected_mode || ((cpu->cr0 & 1u) && (cpu->eflags & FLAG_VM))) &&
           (cpu->guest_idt || (cpu->protected_mode && !cpu->vm->dpmi.active));
}

/* Native DPMI exposes physical FLAGS at CPL3/IOPL0. Software-only sessions
 * can instead expose virtual IOPL3 and a restorable client interrupt flag. */
static inline uint32_t cpu8086_flags_image(const cpu8086_state_t *cpu)
{
    uint32_t flags = cpu->eflags | FLAGS_FIXED;
    if (cpu->protected_mode && !cpu->guest_idt && cpu->vm->dpmi.active && cpu->vm->emulate_cpu) {
        flags = (flags & ~FLAG_IF) | FLAG_IOPL_MASK;
        if (cpu->vm->dpmi.virtual_interrupts_enabled) flags |= FLAG_IF;
    }
    return flags;
}

static inline void cpu8086_restore_virtual_flags(cpu8086_state_t *cpu)
{
    if (cpu->protected_mode && !cpu->guest_idt && cpu->vm->dpmi.active && cpu->vm->emulate_cpu) {
        cpu->vm->dpmi.virtual_interrupts_enabled = (cpu->eflags & FLAG_IF) != 0;
        cpu->eflags |= FLAG_IOPL_MASK | FLAG_IF;
    }
}

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
    /* Only use GDT translation after CS has been loaded with a PM selector
     * (via far JMP/RETF). Between LMSW and the far JMP, the CPU still uses
     * the cached real-mode segment base for fetches. */
    if (vm->cpu && vm->cpu->protected_mode && vm->cpu->pm_cs_loaded)
        return dpmi_translate(vm, seg, off);
    return ((uint32_t)seg << 4) + (uint16_t)off;
}

static inline uint32_t dos_vbe_memory_address(const dos_vm_t *vm,
                                              uint32_t addr)
{
    uint32_t window_offset = addr - DOS_VBE_WINDOW_BASE;
    if (!vm->vbe_active || vm->vbe_linear ||
        window_offset >= DOS_VBE_WINDOW_SIZE)
        return addr;

    uint64_t framebuffer_offset =
        (uint64_t)vm->vbe_bank * DOS_VBE_WINDOW_SIZE + window_offset;
    if (framebuffer_offset >= DOS_VBE_FB_SIZE)
        return vm->total_mem_size;
    return DOS_VBE_FB_BASE + (uint32_t)framebuffer_offset;
}

static inline uint8_t dos_mem_read8(dos_vm_t *vm, uint32_t addr)
{
    if (vm->io && vm->vga_mode == 0x13u && !vm->vbe_active &&
        addr - DOS_VGA_APERTURE_BASE < DOS_VGA_APERTURE_SIZE)
        return dos_io_vga_read_memory(vm, addr);
    addr = dos_vbe_memory_address(vm, addr);
    uint32_t frame_offset = addr - DOS_EMS_PAGE_FRAME_BASE;
    if (frame_offset < DOS_EMS_FRAME_PAGES * DOS_EMS_PAGE_SIZE) {
        uint32_t backing = vm->ems_frame_bases[frame_offset /
                                                DOS_EMS_PAGE_SIZE];
        if (backing)
            addr = backing + (frame_offset & (DOS_EMS_PAGE_SIZE - 1u));
    }
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
void dos_native_map_video(dos_vm_t *vm);
void dos_native_ems_map_frame(dos_vm_t *vm, unsigned frame,
                              uint32_t backing);
void dos_vcpi_cleanup(dos_vm_t *vm);

/* ── Stack operations ───────────────────────────────────────────── */

static inline bool cpu_stack_addr32(const cpu8086_state_t *cpu)
{
    return cpu && cpu->ss_cache.valid &&
           (cpu->ss_cache.descriptor.flags_lim & DESC_32BIT) != 0;
}

static inline uint32_t cpu_segment_linear(const cpu8086_state_t *cpu,
                                           unsigned segment, uint32_t offset)
{
    return dpmi_desc_get_base(&cpu->segment_cache[segment].descriptor) + offset;
}

/* Legacy host frame helpers; guest stack instructions use checked spans. */
static inline uint32_t cpu_stack_address(const cpu8086_state_t *cpu, uint32_t offset)
{
    uint32_t linear = cpu_segment_linear(cpu, 2, offset);
    return cpu->cr0 & 0x80000000u ? dpmi_translate(cpu->vm, 0, linear) : linear;
}

static inline uint32_t cpu_stack_offset(const cpu8086_state_t *cpu)
{
    return cpu_stack_addr32(cpu) ? cpu->esp : cpu->sp;
}

static inline void cpu_stack_adjust(cpu8086_state_t *cpu, int32_t delta)
{
    if (cpu_stack_addr32(cpu))
        cpu->esp = (uint32_t)(cpu->esp + delta);
    else
        cpu->sp = (uint16_t)((int32_t)cpu->sp + delta);
}

static inline void cpu_stack_set_offset(cpu8086_state_t *cpu, uint32_t value)
{
    if (cpu_stack_addr32(cpu))
        cpu->esp = value;
    else
        cpu->sp = (uint16_t)value;
}

static inline void cpu_push16(cpu8086_state_t *cpu, uint16_t val)
{
    bool stack32 = cpu_stack_addr32(cpu);
    if (stack32)
        cpu->esp -= 2u;
    else
        cpu->sp -= 2u;
    uint32_t offset = stack32 ? cpu->esp : cpu->sp;
    uint32_t addr = cpu_stack_address(cpu, offset);
    dos_mem_write16(cpu->vm, addr, val);
}

static inline uint16_t cpu_pop16(cpu8086_state_t *cpu)
{
    bool stack32 = cpu_stack_addr32(cpu);
    uint32_t offset = stack32 ? cpu->esp : cpu->sp;
    uint32_t addr = cpu_stack_address(cpu, offset);
    uint16_t val = dos_mem_read16(cpu->vm, addr);
    if (stack32)
        cpu->esp += 2u;
    else
        cpu->sp += 2u;
    return val;
}

static inline void cpu_push32(cpu8086_state_t *cpu, uint32_t val)
{
    bool stack32 = cpu_stack_addr32(cpu);
    if (stack32)
        cpu->esp -= 4u;
    else
        cpu->sp -= 4u;
    uint32_t offset = stack32 ? cpu->esp : cpu->sp;
    uint32_t addr = cpu_stack_address(cpu, offset);
    dos_mem_write32(cpu->vm, addr, val);
}

static inline uint32_t cpu_pop32(cpu8086_state_t *cpu)
{
    bool stack32 = cpu_stack_addr32(cpu);
    uint32_t offset = stack32 ? cpu->esp : cpu->sp;
    uint32_t addr = cpu_stack_address(cpu, offset);
    uint32_t val = dos_mem_read32(cpu->vm, addr);
    if (stack32)
        cpu->esp += 4u;
    else
        cpu->sp += 4u;
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
/* Install an already validated descriptor or restore an explicit CPU snapshot. */
void cpu8086_cache_ldtr(cpu8086_state_t *cpu, uint16_t selector,
                         const dpmi_descriptor_t *descriptor);
void cpu8086_cache_tr(cpu8086_state_t *cpu, uint16_t selector,
                       const dpmi_descriptor_t *descriptor);
void cpu8086_use_host_ldt(cpu8086_state_t *cpu);
/* Commit a validated protected CS, or initialize a host real/v86 context.
 * Guest real-mode far transfers preserve the previously loaded attributes. */
void cpu8086_cache_cs(cpu8086_state_t *cpu, uint16_t selector,
                       const dpmi_descriptor_t *descriptor, unsigned cpl);
void cpu8086_reset_real_cs(cpu8086_state_t *cpu, uint16_t segment);
void cpu8086_load_real_cs(cpu8086_state_t *cpu, uint16_t segment);
void cpu8086_cache_segment(cpu8086_state_t *cpu, unsigned segment,
                            uint16_t selector, const dpmi_descriptor_t *descriptor);
bool cpu8086_probe_native_segments(cpu8086_state_t *cpu,
                                    cpu_system_segment_t loaded[6], cpu_event_fault_t *fault);
void cpu8086_load_real_segment(cpu8086_state_t *cpu, unsigned segment,
                                uint16_t selector);
/* Explicit host context import, not an instruction-dispatch synchronization. */
void cpu8086_sync_segment(cpu8086_state_t *cpu, unsigned segment);
void cpu8086_sync_data(cpu8086_state_t *cpu);
int cpu8086_run(dos_vm_t *vm);
/* One bounded dispatch for decoded memory/port instructions, including REP. */
bool cpu8086_run_one(dos_vm_t *vm);
bool cpu8086_run_until(dos_vm_t *vm, bool protected_mode,
                       uint16_t stop_cs, uint32_t stop_ip);
bool cpu8086_run_until_signal(dos_vm_t *vm, const bool *signal);
bool cpu8086_run_until_real(dos_vm_t *vm, uint16_t stop_cs,
                            uint16_t stop_ip);
void cpu8086_sync_cs(cpu8086_state_t *cpu);
bool cpu_deliver_hw_interrupt(dos_vm_t *vm, uint8_t int_num);
bool cpu8086_deliver_guest_interrupt(dos_vm_t *vm, uint8_t vector,
                                      cpu_event_kind_t kind, uint32_t return_eip,
                                      uint32_t fault_eip, uint32_t error, bool has_error);
bool dos_vcpi_pm_entry_source(const dos_vm_t *vm, uint8_t vector);
bool cpu8086_service_interrupts(dos_vm_t *vm);
bool cpu_deliver_pm_software_interrupt(dos_vm_t *vm, uint8_t int_num,
                                       uint32_t return_eip, uint32_t fault_eip);
bool cpu8086_enter_dpmi_interrupt(cpu8086_state_t *cpu, uint16_t selector,
                                   uint32_t offset, unsigned width,
                                   uint32_t return_eip, uint32_t flags,
                                   uint32_t fault_eip);
bool dos_int_has_pm_translator(uint8_t int_num);
bool dos_rm_private_interrupt(const dos_vm_t *vm, uint8_t vector);
bool dos_rm_host_vector(dos_vm_t *vm, uint8_t vector,
                          uint16_t segment, uint16_t offset);
void cpu8086_rm_service(dos_vm_t *vm, uint8_t service);
/* Read a word-based host ABI record without changing the loaded stack. */
bool cpu8086_read_stack_words(cpu8086_state_t *cpu, uint16_t *words, unsigned count);
/* Complete a host-owned real interrupt whose checked six-byte frame exists. */
void cpu8086_real_host_interrupt(cpu8086_state_t *cpu, uint8_t vector, uint32_t flags);
void dos_init_ivt(dos_vm_t *vm);
bool cpu_deliver_exception(dos_vm_t *vm, uint8_t vector,
                           uint32_t return_eip, uint32_t error_code,
                           bool has_error_code);
bool dpmi_exception_return(dos_vm_t *vm);
void dos_int21_dispatch(dos_vm_t *vm);
void dos_api_init(dos_vm_t *vm);
void dos_api_close_all(dos_vm_t *vm);
bool dos_exec_activate_loaded_child(dos_vm_t *vm);
bool dos_exec_complete_termination(dos_vm_t *vm);
void dos_exec_cleanup(dos_vm_t *vm);
bool dos_exec_begin_dpmi_client(dos_vm_t *vm);
void dos_exec_abort_dpmi_client(dos_vm_t *vm);
int dos_exec_dpmi_selftest(void);
int dos_mem_free_owner(dos_vm_t *vm, uint16_t owner);
void dos_native_cleanup(dos_vm_t *vm);
void dos_native_release_backend(dos_vm_t *vm);
void dos_native_cleanup_active(void);
void dos_native_suspend(dos_vm_t *vm) __attribute__((noreturn));
int dos_native_session_active(void);
void dos_native_sync_ldt(dos_vm_t *vm);
void dos_native_sync_tables(dos_vm_t *vm);
bool dos_native_prepare_return(dos_vm_t *vm);
int dos_native_refresh_selector(uint16_t error_code);
int dos_native_handle_privileged_fault(x86_interrupt_frame_t *frame);
bool dos_native_handle_exception(x86_interrupt_frame_t *frame);
bool dos_native_handle_memory_fault(x86_interrupt_frame_t *frame,
                                     uint64_t fault_address);
bool dos_native_service_audio_irq(x86_interrupt_frame_t *frame);
bool dos_native_service_timer_irq(x86_interrupt_frame_t *frame);
bool dos_native_service_keyboard_irq(x86_interrupt_frame_t *frame);
void dos_native_dump_rip(uint16_t cs, uint32_t rip, uint16_t ss_hint,
                         uint64_t frame_rsp);
int dos_api_selftest(void);
int dos_run_options_selftest(void);
int dos_mem_selftest(void);
int dpmi_selftest(void);
int dos_bios_memory_selftest(void);
int dos_bios_contract_selftest(void);
int dos_interrupt_selftest(void);
int dos_dpmi_stack_selftest(void);
int dos_vcpi_selftest(void);

#endif /* CPU8086_H */
