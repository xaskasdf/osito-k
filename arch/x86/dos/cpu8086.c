/*
 * OsitoK -- 8086 CPU Emulator
 *
 * Software interpreter for Intel 8086/80186 instruction set.
 * Runs 16-bit DOS code within the 64-bit OsitoK kernel.
 *
 * No libc -- bare-metal kernel environment.
 */

#include "cpu8086.h"
#include "dos_audio.h"
#include "dos_io.h"
#include "dos_jit.h"
#include "dos_paging.h"

/* ── External interfaces ─────────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* INT dispatch (dos_int.c) */
extern void dos_int_dispatch(dos_vm_t *vm, uint8_t int_num);
extern void dos_transfer_to_native(dos_vm_t *vm);
extern int kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
extern uint64_t idt_get_ticks(void);
extern int sched_sleep_ticks(uint64_t ticks);
extern void sched_yield(void);

#define DOS_CR0_MP (1U << 1)
#define DOS_CR0_EM (1U << 2)
#define DOS_CR0_TS (1U << 3)
#define DOS_CR0_ET (1U << 4)
#define DOS_CR0_NW (1U << 29)
#define DOS_CR0_CD (1U << 30)
#define DOS_CR0_VALID 0xE005003FU
#define DOS_CR3_VALID 0xFFFFF018U

#define DOS_CPUID_MAX_BASIC    0x00000001U
#define DOS_CPUID_MAX_EXTENDED 0x80000000U
#define DOS_CPUID_SIGNATURE    0x00000500U

static void cpu_cpuid(cpu8086_state_t *cpu)
{
    uint32_t leaf = cpu->eax;

    cpu->eax = 0;
    cpu->ebx = 0;
    cpu->ecx = 0;
    cpu->edx = 0;

    switch (leaf) {
    case 0:
        cpu->eax = DOS_CPUID_MAX_BASIC;
        cpu->ebx = 0x7469734FU; /* "Osit" */
        cpu->edx = 0x4D564B6FU; /* "oKVM" */
        cpu->ecx = 0x20555043U; /* "CPU " */
        break;
    case 1:
        /* Pentium-class integer core. No optional feature is advertised
         * until its instruction and state-management contracts exist. */
        cpu->eax = DOS_CPUID_SIGNATURE;
        break;
    case DOS_CPUID_MAX_EXTENDED:
        cpu->eax = DOS_CPUID_MAX_EXTENDED;
        break;
    default:
        break;
    }
}

/* VGA flush (dos_vga.c) */
extern void dos_vga_flush(dos_vm_t *vm);
extern void dos_vga_mark_dirty(dos_vm_t *vm, uint32_t addr);
extern void dos_vga_mode13_present(void);

/* ── Memory write (with VGA dirty tracking) ──────────────────────── */

void dos_mem_write8(dos_vm_t *vm, uint32_t addr, uint8_t val)
{
    if (!vm || !vm->mem || addr >= vm->total_mem_size)
        return;
    if (vm->io && vm->vga_mode == 0x13u && !vm->vbe_active &&
        addr - DOS_VGA_APERTURE_BASE < DOS_VGA_APERTURE_SIZE) {
        dos_io_vga_write_memory(vm, addr, val);
        return;
    }
    addr = dos_vbe_memory_address(vm, addr);
    if (addr >= vm->total_mem_size)
        return;
    uint32_t frame_offset = addr - DOS_EMS_PAGE_FRAME_BASE;
    if (frame_offset < DOS_EMS_FRAME_PAGES * DOS_EMS_PAGE_SIZE) {
        uint32_t backing = vm->ems_frame_bases[frame_offset /
                                                DOS_EMS_PAGE_SIZE];
        if (backing) {
            addr = backing + (frame_offset & (DOS_EMS_PAGE_SIZE - 1u));
            if (addr >= vm->total_mem_size)
                return;
        }
    }
    /* Watchpoint: catch MCB owner corruption at 0x600-0x604 */
    if (addr >= 0x600 && addr <= 0x604) {
        serial_puts("[WATCH] write @");
        serial_puthex(addr, 4);
        serial_puts("=");
        serial_puthex(val, 2);
        serial_puts(" EIP=");
        serial_puthex(vm->cpu->eip, 8);
        serial_puts(" CS=");
        serial_puthex(vm->cpu->cs, 4);
        serial_puts(" #");
        serial_putdec(vm->cpu->insn_count);
        serial_puts("\n");
    }
    vm->mem[addr] = val;
    if ((addr >= DOS_CONV_TOP &&
         addr < DOS_VRAM_BASE + DOS_VRAM_SIZE) ||
        (addr >= DOS_VBE_FB_BASE &&
         addr < DOS_VBE_FB_BASE + DOS_VBE_FB_SIZE))
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
    /* The interpreter exposes the x86 software-emulation contract. */
    cpu->cr0 = DOS_CR0_ET | DOS_CR0_EM | DOS_CR0_MP;
    cpu->cr2 = 0; cpu->cr3 = 0;
    for (unsigned i = 0; i < 8; i++) cpu->dr[i] = 0;
    cpu->dr[6] = 0xFFFF0FF0u;
    cpu->dr[7] = 0x00000400u;
    cpu->gdtr.limit = 0; cpu->gdtr.base = 0;
    cpu->idtr.limit = 0x3FF; cpu->idtr.base = 0;
    cpu8086_use_host_ldt(cpu);
    cpu8086_cache_tr(cpu, 0, NULL);
    cpu->protected_mode = false;
    cpu->pm_cs_loaded    = false;
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
    cpu->rep_compare.active = false;
    cpu->irq_shadow = 0;
    cpu->guest_idt = false;
    cpu->delivery_fault = NULL;
    cpu->vm = vm;
    cpu8086_reset_real_cs(cpu, 0);
    cpu8086_sync_data(cpu);
}

void cpu8086_cache_cs(cpu8086_state_t *cpu, uint16_t selector,
                       const dpmi_descriptor_t *descriptor, unsigned cpl)
{
    cpu->cs_cache = descriptor ? (cpu_system_segment_t){ *descriptor, true }
                              : (cpu_system_segment_t){0};
    cpu->cs = selector;
    cpu->cpl = cpl;
    cpu->pm_cs_loaded = true;
    cpu->op_size_32 = cpu->addr_size_32 = descriptor &&
        (cpu->cs_cache.descriptor.flags_lim & DESC_32BIT) != 0;
}

void cpu8086_reset_real_cs(cpu8086_state_t *cpu, uint16_t segment)
{
    dpmi_descriptor_t descriptor = { .limit_lo = 0xFFFFu,
        .access = DESC_PRESENT | DESC_SEGMENT | DESC_CODE | DESC_READABLE | 1u };
    dpmi_desc_set_base(&descriptor, (uint32_t)segment << 4);
    cpu8086_cache_cs(cpu, segment, &descriptor, 0);
    cpu->pm_cs_loaded = false;
}

void cpu8086_load_real_cs(cpu8086_state_t *cpu, uint16_t segment)
{
    if ((cpu->cr0 & 1u) && (cpu->eflags & FLAG_VM)) {
        cpu8086_reset_real_cs(cpu, segment);
        cpu->cs_cache.descriptor.access |= DESC_DPL3;
        cpu->cpl = 3;
        return;
    }
    /* Pentium-class real mode reloads the base, not the hidden limit/D bit. */
    dpmi_desc_set_base(&cpu->cs_cache.descriptor, (uint32_t)segment << 4);
    cpu->cs_cache.valid = true;
    cpu->cs_cache.descriptor.access |= DESC_PRESENT | DESC_SEGMENT;
    cpu->cs = segment;
    cpu->cpl = 0;
    cpu->pm_cs_loaded = false;
}

void cpu8086_cache_ldtr(cpu8086_state_t *cpu, uint16_t selector,
                         const dpmi_descriptor_t *descriptor)
{
    cpu->ldtr = selector;
    cpu->host_ldt = false;
    cpu->ldt_cache = (cpu_system_segment_t){0};
    if (descriptor) cpu->ldt_cache = (cpu_system_segment_t){ *descriptor, true };
}

void cpu8086_cache_tr(cpu8086_state_t *cpu, uint16_t selector,
                       const dpmi_descriptor_t *descriptor)
{
    cpu->tr = selector;
    cpu->tss_cache = (cpu_system_segment_t){0};
    if (descriptor) cpu->tss_cache = (cpu_system_segment_t){ *descriptor, true };
}

void cpu8086_use_host_ldt(cpu8086_state_t *cpu)
{
    cpu8086_cache_ldtr(cpu, 0, NULL);
    cpu->host_ldt = true;
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

void cpu8086_cache_segment(cpu8086_state_t *cpu, unsigned segment,
                            uint16_t selector, const dpmi_descriptor_t *descriptor)
{
    if (segment == 1) {
        cpu8086_cache_cs(cpu, selector, descriptor, cpu8086_cpl(cpu));
        return;
    }
    cpu->segment_cache[segment] = descriptor
        ? (cpu_system_segment_t){ *descriptor, true } : (cpu_system_segment_t){0};
    *seg_ptr(cpu, segment) = selector;
}

void cpu8086_load_real_segment(cpu8086_state_t *cpu, unsigned segment,
                                uint16_t selector)
{
    if (segment == 1) { cpu8086_load_real_cs(cpu, selector); return; }
    cpu_system_segment_t *cache = &cpu->segment_cache[segment];
    if ((cpu->cr0 & 1u) && (cpu->eflags & FLAG_VM)) {
        cache->descriptor = (dpmi_descriptor_t){ .limit_lo = 0xFFFFu,
            .access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT | DESC_WRITABLE | 1u };
    }
    dpmi_desc_set_base(&cache->descriptor, (uint32_t)selector << 4);
    cache->descriptor.access |= DESC_PRESENT | DESC_SEGMENT;
    cache->valid = true;
    *seg_ptr(cpu, segment) = selector;
}

void cpu8086_sync_segment(cpu8086_state_t *cpu, unsigned segment)
{
    if (segment == 1) { cpu8086_sync_cs(cpu); return; }
    uint16_t selector = *seg_ptr(cpu, segment);
    dpmi_descriptor_t descriptor;
    if (!cpu->protected_mode) {
        descriptor = (dpmi_descriptor_t){ .limit_lo = 0xFFFFu,
            .access = DESC_PRESENT | DESC_SEGMENT | DESC_WRITABLE | 1u };
        if ((cpu->cr0 & 1u) && (cpu->eflags & FLAG_VM)) descriptor.access |= DESC_DPL3;
        dpmi_desc_set_base(&descriptor, (uint32_t)selector << 4);
    } else if (!dpmi_guest_descriptor(cpu->vm, selector, &descriptor)) {
        cpu8086_cache_segment(cpu, segment, selector, NULL);
        return;
    }
    if (cpu->protected_mode) {
        uint8_t access = descriptor.access;
        unsigned cpl = cpu8086_cpl(cpu), dpl = (access & DESC_DPL_MASK) >> 5;
        bool code = (access & DESC_CODE) != 0, conforming = code && (access & 4u);
        bool allowed = (access & (DESC_PRESENT | DESC_SEGMENT)) ==
                        (DESC_PRESENT | DESC_SEGMENT);
        if (segment == 2) allowed &= !code && (access & DESC_WRITABLE) &&
                                      (selector & 3u) == cpl && dpl == cpl;
        else allowed &= (!code || (access & DESC_READABLE)) &&
                        (conforming || (cpl <= dpl && (selector & 3u) <= dpl));
        if (!allowed) {
            cpu8086_cache_segment(cpu, segment, selector, NULL);
            return;
        }
    }
    cpu8086_cache_segment(cpu, segment, selector, &descriptor);
}

void cpu8086_sync_data(cpu8086_state_t *cpu)
{
    for (unsigned s = 0; s < 6; s++) if (s != 1) cpu8086_sync_segment(cpu, s);
}

/* ── ModRM decoding ──────────────────────────────────────────────── */

/*
 * Decoded ModRM result.
 * If is_reg == true:  the operand is a register (use reg8/reg16 pointer).
 * Memory operands retain the effective offset until checked access.
 */
typedef struct {
    uint32_t offset;      /* effective offset before segment translation */
    uint8_t  seg_index;   /* source segment register */
    uint8_t  reg_field;   /* the /reg field (bits 5-3) */
    uint8_t  rm_field;    /* the r/m field (bits 2-0) */
    uint8_t  mod_field;   /* the mod field (bits 7-6) */
    bool     is_reg;      /* true if mod==11 (register operand) */
    bool     esp_base;
    bool     read_modify_write;
    bool     checked_write;
    uint8_t  checked_size;
    uint32_t physical[4];
} modrm_t;

enum cpu_descriptor_query {
    CPU_QUERY_LAR, CPU_QUERY_LSL, CPU_QUERY_READ, CPU_QUERY_WRITE
};

static bool cpu_descriptor_rights(cpu8086_state_t *cpu, uint16_t selector,
                                   const dpmi_descriptor_t *descriptor,
                                   enum cpu_descriptor_query query, uint32_t *value)
{
    uint8_t access = descriptor->access;
    bool segment = (access & DESC_SEGMENT) != 0;
    bool code = segment && (access & DESC_CODE);
    bool conforming = code && (access & 0x04u);
    unsigned cpl = cpu8086_cpl(cpu);
    unsigned dpl = (access & DESC_DPL_MASK) >> 5;
    if (!conforming && (cpl > dpl || (selector & 3u) > dpl)) return false;

    /* Presence is not part of these queries: a visible nonpresent segment
     * can still report its type, rights and limit without being loaded. */
    if (query == CPU_QUERY_READ)
        return segment && (!code || (access & DESC_READABLE));
    if (query == CPU_QUERY_WRITE)
        return segment && !code && (access & DESC_WRITABLE);

    if (!segment) {
        switch (access & 0x0Fu) {
        case 1: case 2: case 3: case 9: case 11: /* TSS or LDT */
            break;
        case 4: case 5: case 12: /* call/task gate: LAR only */
            if (query != CPU_QUERY_LAR) return false;
            break;
        default:
            return false;
        }
    }
    *value = query == CPU_QUERY_LAR
        ? ((uint32_t)access << 8) | ((uint32_t)(descriptor->flags_lim & 0xF0u) << 16)
        : dpmi_desc_get_limit(descriptor);
    return true;
}

static bool cpu_query_descriptor(cpu8086_state_t *cpu, uint16_t selector,
                                  enum cpu_descriptor_query query,
                                  uint32_t *value, dos_page_fault_t *fault)
{
    dpmi_descriptor_ref_t reference;
    return dpmi_lookup_descriptor(cpu->vm, selector, &reference, fault) &&
           cpu_descriptor_rights(cpu, selector, &reference.descriptor, query, value);
}

static void cpu_raise_page_fault(cpu8086_state_t *cpu, uint32_t insn_eip,
                                  const dos_page_fault_t *fault)
{
    cpu->cr2 = fault->linear;
    (void)cpu_deliver_exception(cpu->vm, 14, insn_eip, fault->error, true);
}

static bool cpu_system_privileged(cpu8086_state_t *cpu, uint32_t insn_eip)
{
    /* Before the first protected far transfer CS still has its real-mode
     * value, whose low bits are not a CPL. VM86 always has client privilege. */
    if (cpu8086_cpl(cpu)) {
        (void)cpu_deliver_exception(cpu->vm, 13, insn_eip, 0, true);
        return false;
    }
    return true;
}

static bool cpu_write_cr0(cpu8086_state_t *cpu, uint32_t value, uint32_t insn_eip)
{
    if (((value & DOS_CR0_PG) && !(value & 1u)) ||
        ((value & DOS_CR0_NW) && !(value & DOS_CR0_CD))) {
        (void)cpu_deliver_exception(cpu->vm, 13, insn_eip, 0, true);
        return false;
    }
    cpu->cr0 = value & DOS_CR0_VALID;
    bool protected = (value & 1u) != 0;
    if (cpu->protected_mode && !protected) {
        cpu->pm_cs_loaded = false;
        cpu->cpl = 0;
    }
    cpu->protected_mode = protected;
    return true;
}

/* Validate a reloaded CS and update its default operand/address size. */
void cpu8086_sync_cs(cpu8086_state_t *cpu)
{
    dos_vm_t *vm = cpu->vm;
    if (!cpu->protected_mode) {
        cpu8086_reset_real_cs(cpu, cpu->cs);
        return;
    }
    dpmi_descriptor_t desc;
    if (!dpmi_guest_descriptor(vm, cpu->cs, &desc) ||
        (desc.access & (DESC_PRESENT | DESC_SEGMENT | DESC_CODE)) !=
        (DESC_PRESENT | DESC_SEGMENT | DESC_CODE)) {
        cpu8086_cache_cs(cpu, cpu->cs, NULL, cpu->cs & 3u);
        serial_puts("[DPMI] Invalid CS selector: ");
        serial_puthex(cpu->cs, 4);
        serial_puts("\n");
        return;
    }

    bool d32 = (desc.flags_lim & DESC_32BIT) != 0;
    bool old32 = cpu->op_size_32;
    cpu8086_cache_cs(cpu, cpu->cs, &desc, cpu->cs & 3u);
    if (d32 != old32 && cpu->cs_mode_log_count < 16u) {
        serial_puts("[DPMI] CS mode: ");
        serial_puthex(cpu->cs, 4);
        serial_puts(d32 ? " -> USE32" : " -> USE16");
        serial_puts(" EIP=");
        serial_puthex(cpu->eip, 8);
        serial_puts(" base=");
        serial_puthex(dpmi_desc_get_base(&desc), 8);
        serial_puts("\n");
        if (++cpu->cs_mode_log_count == 16u)
            serial_puts("[DPMI] further CS mode transitions omitted\n");
    }
}

static void cpu_commit_cs_load(cpu8086_state_t *cpu)
{
    if (cpu->protected_mode) cpu8086_sync_cs(cpu);
    else cpu8086_load_real_cs(cpu, cpu->cs);
    if (cpu->protected_mode && cpu->op_size_32)
        dos_transfer_to_native(cpu->vm);
}

typedef struct {
    uint32_t start, base, limit, page_linear;
    dos_page_translation_t page;
    dos_page_fault_t fault;
    unsigned length;
    bool ready, protected, page_valid;
} cpu_fetch_state_t;

static bool cpu_fetch_value(cpu8086_state_t *cpu, cpu_fetch_state_t *fetch,
                             unsigned width, uint32_t *value)
{
    if (fetch->length + width > 15u) return false;
    if (!fetch->ready) {
        fetch->protected = (cpu->protected_mode && cpu->pm_cs_loaded) || cpu->op_size_32;
        if (!cpu->cs_cache.valid) return false;
        fetch->base = dpmi_desc_get_base(&cpu->cs_cache.descriptor);
        fetch->limit = dpmi_desc_get_limit(&cpu->cs_cache.descriptor);
        fetch->ready = true;
    }
    uint64_t last = (uint64_t)fetch->start + fetch->length + width - 1u;
    if (last > fetch->limit) return false;
    uint32_t linear = fetch->base + fetch->start + fetch->length;
    uint32_t result = 0;
    if (!(cpu->cr0 & DOS_CR0_PG)) {
        result = width == 1 ? dos_mem_read8(cpu->vm, linear) :
                 width == 2 ? dos_mem_read16(cpu->vm, linear) : dos_mem_read32(cpu->vm, linear);
    } else {
        uint32_t addresses[4];
        dos_page_translation_t pages[2];
        unsigned done = 0, count = 0;
        while (done < width) {
            uint32_t current = linear + done;
            uint32_t page_linear = current & ~0xFFFu;
            unsigned size = DOS_PAGE_SIZE - (current & 0xFFFu);
            if (size > width - done) size = width - done;
            if (fetch->page_valid && page_linear == fetch->page_linear) {
                pages[count] = fetch->page;
                pages[count].physical += current & 0xFFFu;
            } else if (!dos_page_probe(cpu->vm, current,
                        cpu8086_cpl(cpu) == 3u ? DOS_PAGE_USER : DOS_PAGE_READ,
                        &pages[count], &fetch->fault)) return false;
            for (unsigned i = 0; i < size; i++) addresses[done + i] = pages[count].physical + i;
            done += size;
            count++;
        }
        for (unsigned i = 0; i < count; i++) dos_page_commit(cpu->vm, &pages[i], false);
        fetch->page = pages[count - 1u];
        fetch->page.physical &= ~0xFFFu;
        fetch->page_linear = (linear + width - 1u) & ~0xFFFu;
        fetch->page_valid = true;
        for (unsigned i = 0; i < width; i++)
            result |= (uint32_t)dos_mem_read8(cpu->vm, addresses[i]) << (i * 8u);
    }
    fetch->length += width;
    cpu->eip = fetch->protected ? fetch->start + fetch->length
                                : (uint16_t)(fetch->start + fetch->length);
    *value = result;
    return true;
}

/* Decode failures leave through an explicit local label, before evaluating
 * the rest of the expression. No longjmp context survives a host callback.
 * Both decoder and dispatcher own a `fetch` pointer and a fetch_fault label. */
#define CPU_FETCH(cpu, width) ({ \
    uint32_t fetch_value; \
    if (!cpu_fetch_value((cpu), fetch, (width), &fetch_value)) goto fetch_fault; \
    fetch_value; \
})
#define CPU_FETCH8(cpu)  ((uint8_t)CPU_FETCH((cpu), 1u))
#define CPU_FETCH16(cpu) ((uint16_t)CPU_FETCH((cpu), 2u))
#define CPU_FETCH32(cpu) CPU_FETCH((cpu), 4u)

static bool cpu_decode_modrm(cpu8086_state_t *cpu, cpu_fetch_state_t *fetch,
                              uint8_t modrm, int32_t extra_offset, modrm_t *out)
{
    modrm_t result = {0};
    result.mod_field = (modrm >> 6) & 3;
    result.reg_field = (modrm >> 3) & 7;
    result.rm_field  = modrm & 7;
    result.is_reg    = false;
    result.offset    = 0;
    result.seg_index = 3;

    if (result.mod_field == 3) {
        /* Register operand -- no memory access */
        result.is_reg = true;
        *out = result;
        return true;
    }

    bool adr32 = cpu->addr_size_32 ^ cpu->prefix_67;
    bool use_ss = false;

    if (adr32) {
        /* ── 32-bit addressing mode ─────────────────────────────── */
        uint32_t offset = 0;

        if (result.rm_field == 4) {
            /* SIB byte follows */
            uint8_t sib   = CPU_FETCH8(cpu);
            uint8_t scale = (sib >> 6) & 3;
            uint8_t index = (sib >> 3) & 7;
            uint8_t base  = sib & 7;

            /* Base register (base=5 with mod=0 means disp32 only) */
            if (base == 5 && result.mod_field == 0) {
                offset = CPU_FETCH32(cpu);
            } else {
                offset = *reg32_ptr(cpu, base);
                result.esp_base = base == 4;
                if (base == 4 || base == 5) use_ss = true; /* ESP/EBP */
            }

            /* Index register (index=4 means no index) */
            if (index != 4)
                offset += *reg32_ptr(cpu, index) << scale;

        } else if (result.rm_field == 5 && result.mod_field == 0) {
            /* [disp32] — absolute address */
            offset = CPU_FETCH32(cpu);
        } else {
            offset = *reg32_ptr(cpu, result.rm_field);
            if (result.rm_field == 5) use_ss = true; /* EBP */
        }

        /* Displacement */
        if (result.mod_field == 1) {
            offset += (uint32_t)(int32_t)(int8_t)CPU_FETCH8(cpu);
        } else if (result.mod_field == 2) {
            offset += CPU_FETCH32(cpu);
        }

        /* Retain the effective address; memory is resolved after decoding. */
        result.seg_index = cpu->seg_override >= 0 ? (uint8_t)cpu->seg_override
                                                 : use_ss ? 2u : 3u;
        result.offset = offset + (uint32_t)extra_offset;
        *out = result;
        return true;
    }

    /* ── 16-bit addressing mode ─────────────────────────────────── */
    uint16_t offset = 0;

    switch (result.rm_field) {
    case 0: offset = cpu->bx + cpu->si; break;
    case 1: offset = cpu->bx + cpu->di; break;
    case 2: offset = cpu->bp + cpu->si; use_ss = true; break;
    case 3: offset = cpu->bp + cpu->di; use_ss = true; break;
    case 4: offset = cpu->si; break;
    case 5: offset = cpu->di; break;
    case 6:
        if (result.mod_field == 0) {
            offset = CPU_FETCH16(cpu);
        } else {
            offset = cpu->bp;
            use_ss = true;
        }
        break;
    case 7: offset = cpu->bx; break;
    }

    if (result.mod_field == 1) {
        int8_t disp8 = (int8_t)CPU_FETCH8(cpu);
        offset += (uint16_t)(int16_t)disp8;
    } else if (result.mod_field == 2) {
        uint16_t disp16 = CPU_FETCH16(cpu);
        offset += disp16;
    }

    result.seg_index = cpu->seg_override >= 0 ? (uint8_t)cpu->seg_override
                                             : use_ss ? 2u : 3u;
    result.offset = (uint16_t)(offset + (uint32_t)extra_offset);
    *out = result;
    return true;

fetch_fault:
    return false;
}

#define CPU_DECODE_MODRM_OFFSET(cpu, modrm, offset) ({ \
    modrm_t decoded_operand; \
    if (!cpu_decode_modrm((cpu), fetch, (modrm), (offset), &decoded_operand)) goto fetch_fault; \
    decoded_operand; \
})
#define CPU_DECODE_MODRM(cpu, modrm) CPU_DECODE_MODRM_OFFSET((cpu), (modrm), 0)

static bool cpu_memory_addresses_at_cpl(cpu8086_state_t *cpu, uint32_t linear,
                                         unsigned size, bool write, bool dirty,
                                         unsigned cpl, uint32_t insn_eip,
                                         uint32_t *addresses)
{
    dos_vm_t *vm = cpu->vm;
    bool paging = (cpu->cr0 & DOS_CR0_PG) != 0;
    dos_page_translation_t pages[2];
    unsigned count = 0, done = 0;
    unsigned access = (write ? DOS_PAGE_WRITE : DOS_PAGE_READ) |
                      (cpl == 3u ? DOS_PAGE_USER : 0);
    while (done < size) {
        uint32_t current = linear + done;
        unsigned part = DOS_PAGE_SIZE - (current & (DOS_PAGE_SIZE - 1u));
        if (part > size - done) part = size - done;
        uint32_t physical = current;
        if (paging) {
            dos_page_fault_t fault;
            if (!dos_page_probe(vm, current, access, &pages[count], &fault)) {
                cpu_raise_page_fault(cpu, insn_eip, &fault);
                return false;
            }
            physical = pages[count++].physical;
        }
        for (unsigned i = 0; i < part; i++) addresses[done + i] = physical + i;
        done += part;
    }
    for (unsigned i = 0; i < count; i++) dos_page_commit(vm, &pages[i], dirty);
    return true;
}

static bool cpu_memory_addresses(cpu8086_state_t *cpu, uint32_t linear,
                                  unsigned size, bool write, bool dirty,
                                  uint32_t insn_eip, uint32_t *addresses)
{
    return cpu_memory_addresses_at_cpl(cpu, linear, size, write, dirty,
                                        cpu8086_cpl(cpu), insn_eip, addresses);
}

static bool cpu_operand_addresses(cpu8086_state_t *cpu, unsigned segment,
                                   uint32_t offset, unsigned size, bool write,
                                   uint32_t insn_eip, uint32_t *addresses)
{
    dos_vm_t *vm = cpu->vm;
    const cpu_system_segment_t *cache = &cpu->segment_cache[segment];
    const dpmi_descriptor_t *descriptor = &cache->descriptor;
    uint8_t access = descriptor->access;
    if (!cache->valid || (access & (DESC_PRESENT | DESC_SEGMENT)) !=
            (DESC_PRESENT | DESC_SEGMENT)) goto segment_fault;
    if ((cpu->protected_mode || segment != 1) &&
        (write ? (access & DESC_CODE) || !(access & DESC_WRITABLE)
               : (access & DESC_CODE) && !(access & DESC_READABLE)))
        goto segment_fault;
    uint32_t base = dpmi_desc_get_base(descriptor);
    uint32_t limit = dpmi_desc_get_limit(descriptor);
    bool expand_down = !(access & DESC_CODE) && (access & 4u);
    uint32_t top = descriptor->flags_lim & DESC_32BIT ? UINT32_MAX : 0xFFFFu;
    uint64_t last = (uint64_t)offset + size - 1u;
    if (expand_down ? offset <= limit || last > top : last > limit)
        goto segment_fault;

    /* These operands are at most eight bytes. Validate both possible pages
     * before A/D updates, memory writes, or a side-effecting port read. */
    return cpu_memory_addresses(cpu, base + offset, size, write, write,
                                 insn_eip, addresses);

segment_fault:
    (void)cpu_deliver_exception(vm, segment == 2 ? 12u : 13u,
                                insn_eip, 0, true);
    return false;
}

static bool cpu_operand_record(cpu8086_state_t *cpu, const modrm_t *operand,
                                unsigned size, bool write,
                                uint32_t insn_eip, uint8_t *bytes)
{
    uint32_t addresses[8];
    if (!cpu_operand_addresses(cpu, operand->seg_index, operand->offset,
                                size, write, insn_eip, addresses)) return false;
    for (unsigned i = 0; i < size; i++) {
        if (write) dos_mem_write8(cpu->vm, addresses[i], bytes[i]);
        else bytes[i] = dos_mem_read8(cpu->vm, addresses[i]);
    }
    return true;
}

static bool cpu_prepare_segment_load(cpu8086_state_t *cpu, unsigned segment,
                                      uint16_t selector, uint32_t insn_eip,
                                      cpu_system_segment_t *loaded)
{
    dos_vm_t *vm = cpu->vm;
    *loaded = (cpu_system_segment_t){0};
    if (cpu->protected_mode) {
        dpmi_descriptor_ref_t reference;
        dos_page_fault_t fault;
        bool null = (selector & ~3u) == 0;
        if (null) {
            if (segment == 2) goto invalid_selector;
        } else {
            if (!dpmi_lookup_descriptor(vm, selector, &reference, &fault)) {
                if (fault.raised) {
                    cpu_raise_page_fault(cpu, insn_eip, &fault);
                    return false;
                }
                goto invalid_selector;
            }
            dpmi_descriptor_t descriptor = reference.descriptor;
            unsigned cpl = cpu8086_cpl(cpu);
            unsigned dpl = (descriptor.access & DESC_DPL_MASK) >> 5;
            if (segment == 2) {
                if ((descriptor.access & (DESC_SEGMENT | DESC_CODE | DESC_WRITABLE))
                        != (DESC_SEGMENT | DESC_WRITABLE) ||
                    (selector & 3u) != cpl || dpl != cpl)
                    goto invalid_selector;
            } else if (!cpu_descriptor_rights(cpu, selector, &descriptor, CPU_QUERY_READ, NULL)) {
                goto invalid_selector;
            }
            if (!(descriptor.access & DESC_PRESENT)) {
                (void)cpu_deliver_exception(vm, segment == 2 ? 12u : 11u,
                                            insn_eip, selector & ~3u, true);
                return false;
            }
            if (!dpmi_descriptor_set_accessed(vm, &reference, &fault)) {
                cpu_raise_page_fault(cpu, insn_eip, &fault);
                return false;
            }
            descriptor.access |= 1u;
            *loaded = (cpu_system_segment_t){ descriptor, true };
        }
    }
    return true;

invalid_selector:
    (void)cpu_deliver_exception(vm, 13, insn_eip, selector & ~3u, true);
    return false;
}

bool cpu8086_probe_native_segments(cpu8086_state_t *cpu,
                                    cpu_system_segment_t loaded[6], cpu_event_fault_t *fault)
{
    if (!cpu || !cpu->vm || !loaded || !fault) return false;
    *fault = (cpu_event_fault_t){0};
    if (!cpu->protected_mode || cpu8086_cpl(cpu) != 3u || cpu->delivery_fault)
        return false;

    /* Probe in the DOS gate's data-register restore order. No loaded cache
     * changes until the caller admits the entire return image. */
    static const unsigned data[] = { 5, 4, 0, 3 };
    cpu->delivery_fault = fault;
    bool valid = false;
    for (unsigned i = 0; i < sizeof(data) / sizeof(data[0]); i++) {
        unsigned segment = data[i];
        if (!cpu_prepare_segment_load(cpu, segment, *seg_ptr(cpu, segment),
                                       cpu->eip, &loaded[segment])) goto done;
    }

    dpmi_descriptor_ref_t reference;
    dos_page_fault_t page_fault;
    uint32_t error = cpu->cs & ~3u;
    if (!error || (cpu->cs & 3u) != 3u) goto invalid_code;
    if (!dpmi_lookup_descriptor(cpu->vm, cpu->cs, &reference, &page_fault)) {
        if (page_fault.raised) {
            cpu_raise_page_fault(cpu, cpu->eip, &page_fault);
            goto done;
        }
        goto invalid_code;
    }
    dpmi_descriptor_t code = reference.descriptor;
    if ((code.access & (DESC_SEGMENT | DESC_CODE)) != (DESC_SEGMENT | DESC_CODE) ||
        (code.flags_lim & 0x20u) ||
        (!(code.access & 4u) && (code.access & DESC_DPL_MASK) != DESC_DPL3))
        goto invalid_code;
    if (!(code.access & DESC_PRESENT)) {
        (void)cpu_deliver_exception(cpu->vm, 11, cpu->eip, error, true);
        goto done;
    }
    if (cpu->eip > dpmi_desc_get_limit(&code)) {
        error = 0;
        goto invalid_code;
    }
    if (!dpmi_descriptor_set_accessed(cpu->vm, &reference, &page_fault)) {
        cpu_raise_page_fault(cpu, cpu->eip, &page_fault);
        goto done;
    }
    code.access |= 1u;
    loaded[1] = (cpu_system_segment_t){ code, true };
    valid = cpu_prepare_segment_load(cpu, 2, cpu->ss, cpu->eip, &loaded[2]);
    goto done;

invalid_code:
    (void)cpu_deliver_exception(cpu->vm, 13, cpu->eip, error, true);
done:
    cpu->delivery_fault = NULL;
    return valid;
}

static void cpu_commit_segment_load(cpu8086_state_t *cpu, unsigned segment,
                                     uint16_t selector, const cpu_system_segment_t *loaded)
{
    if (cpu->protected_mode)
        cpu8086_cache_segment(cpu, segment, selector, loaded->valid ? &loaded->descriptor : NULL);
    else cpu8086_load_real_segment(cpu, segment, selector);
}

static void cpu_load_far_pointer(cpu8086_state_t *cpu, const modrm_t *operand,
                                 unsigned segment, bool wide, bool locked,
                                 uint32_t insn_eip)
{
    if (locked || operand->is_reg) {
        (void)cpu_deliver_exception(cpu->vm, 6, insn_eip, 0, false);
        return;
    }
    unsigned width = wide ? 4u : 2u;
    uint8_t bytes[6];
    if (!cpu_operand_record(cpu, operand, width + 2u, false, insn_eip, bytes)) return;
    uint32_t offset = 0;
    for (unsigned i = 0; i < width; i++) offset |= (uint32_t)bytes[i] << (8u * i);
    uint16_t selector = bytes[width] | ((uint16_t)bytes[width + 1u] << 8);
    cpu_system_segment_t loaded;
    if (!cpu_prepare_segment_load(cpu, segment, selector, insn_eip, &loaded)) return;
    cpu_commit_segment_load(cpu, segment, selector, &loaded);
    if (wide) *reg32_ptr(cpu, operand->reg_field) = offset;
    else *reg16_ptr(cpu, operand->reg_field) = (uint16_t)offset;
}

static bool cpu_modrm_prepare(cpu8086_state_t *cpu, modrm_t *m,
                               unsigned size, bool write, uint32_t insn_eip)
{
    if (m->is_reg) return true;
    write |= m->read_modify_write;
    if (m->checked_size == size && (!write || m->checked_write)) return true;
    if (!cpu_operand_addresses(cpu, m->seg_index, m->offset, size, write,
                                insn_eip, m->physical)) return false;
    m->checked_size = size;
    m->checked_write = write;
    return true;
}

static uint32_t cpu_modrm_read(cpu8086_state_t *cpu, const modrm_t *m,
                                unsigned size)
{
    if (m->is_reg) {
        if (size == 1u) return *reg8_ptr(cpu, m->rm_field);
        if (size == 2u) return *reg16_ptr(cpu, m->rm_field);
        return *reg32_ptr(cpu, m->rm_field);
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < size; i++)
        value |= (uint32_t)dos_mem_read8(cpu->vm, m->physical[i]) << (i * 8u);
    return value;
}

static void cpu_modrm_write(cpu8086_state_t *cpu, const modrm_t *m,
                             unsigned size, uint32_t value)
{
    if (m->is_reg) {
        if (size == 1u) *reg8_ptr(cpu, m->rm_field) = (uint8_t)value;
        else if (size == 2u) *reg16_ptr(cpu, m->rm_field) = (uint16_t)value;
        else *reg32_ptr(cpu, m->rm_field) = value;
        return;
    }
    for (unsigned i = 0; i < size; i++)
        dos_mem_write8(cpu->vm, m->physical[i], (uint8_t)(value >> (i * 8u)));
}

/* Faults leave the dispatcher before evaluating the remaining expression.
 * An RMW validates writing before its read, including MMIO read effects.
 * The write macro validates before a value expression can change FLAGS. */
#define CPU_MODRM_READ(cpu, operand, size) ({ \
    if (!cpu_modrm_prepare((cpu), (operand), (size), false, insn_eip)) \
        goto instruction_complete; \
    cpu_modrm_read((cpu), (operand), (size)); \
})
#define CPU_MODRM_WRITE(cpu, operand, size, value) do { \
    if (!cpu_modrm_prepare((cpu), (operand), (size), true, insn_eip)) \
        goto instruction_complete; \
    cpu_modrm_write((cpu), (operand), (size), (value)); \
} while (0)
#define modrm_read8(cpu, m) ((uint8_t)CPU_MODRM_READ((cpu), (m), 1u))
#define modrm_read16(cpu, m) ((uint16_t)CPU_MODRM_READ((cpu), (m), 2u))
#define modrm_read32(cpu, m) CPU_MODRM_READ((cpu), (m), 4u)
#define modrm_write8(cpu, m, value) CPU_MODRM_WRITE((cpu), (m), 1u, (value))
#define modrm_write16(cpu, m, value) CPU_MODRM_WRITE((cpu), (m), 2u, (value))
#define modrm_write32(cpu, m, value) CPU_MODRM_WRITE((cpu), (m), 4u, (value))

typedef struct {
    uint32_t base, limit, top, mask;
    unsigned cpl;
    bool expand_down;
} cpu_stack_access_t;

/* Keep an instruction-local view of the loaded SS, including its B bit. */
static bool cpu_stack_begin(cpu8086_state_t *cpu, cpu_stack_access_t *stack,
                             bool locked, uint32_t insn_eip)
{
    if (locked) {
        (void)cpu_deliver_exception(cpu->vm, 6, insn_eip, 0, false);
        return false;
    }
    dpmi_descriptor_t descriptor = cpu->ss_cache.descriptor;
    unsigned cpl = cpu8086_cpl(cpu);
    if (!cpu->ss_cache.valid ||
        (descriptor.access & (DESC_PRESENT | DESC_SEGMENT)) != (DESC_PRESENT | DESC_SEGMENT) ||
        (descriptor.access & (DESC_CODE | DESC_WRITABLE)) != DESC_WRITABLE)
        goto stack_fault;
    stack->base = dpmi_desc_get_base(&descriptor);
    stack->cpl = cpl;
    stack->limit = dpmi_desc_get_limit(&descriptor);
    stack->mask = descriptor.flags_lim & DESC_32BIT ? UINT32_MAX : 0xFFFFu;
    stack->top = stack->mask;
    stack->expand_down = (descriptor.access & 4u) != 0;
    return true;

stack_fault:
    (void)cpu_deliver_exception(cpu->vm, 12, insn_eip, 0, true);
    return false;
}

static uint32_t cpu_stack_next_esp(const cpu_stack_access_t *stack,
                                   uint32_t esp, uint32_t offset)
{
    return (esp & ~stack->mask) | (offset & stack->mask);
}

static bool cpu_stack_slot(cpu8086_state_t *cpu, const cpu_stack_access_t *stack,
                            uint32_t offset, unsigned width, bool write,
                            bool dirty, uint32_t insn_eip, modrm_t *slot)
{
    uint64_t last = (uint64_t)offset + width - 1u;
    if (stack->expand_down ? offset <= stack->limit || last > stack->top
                           : last > stack->limit) {
        (void)cpu_deliver_exception(cpu->vm, 12, insn_eip, 0, true);
        return false;
    }
    *slot = (modrm_t){ .seg_index = 2, .offset = offset };
    return cpu_memory_addresses_at_cpl(cpu, stack->base + offset, width, write, dirty,
                                        stack->cpl, insn_eip, slot->physical);
}

static bool cpu_stack_push_value(cpu8086_state_t *cpu, uint32_t value,
                                  unsigned width, bool locked, uint32_t insn_eip)
{
    cpu_stack_access_t stack;
    modrm_t slot;
    if (!cpu_stack_begin(cpu, &stack, locked, insn_eip)) return false;
    uint32_t next = cpu_stack_next_esp(&stack, cpu->esp, cpu->esp - width);
    if (!cpu_stack_slot(cpu, &stack, next & stack.mask, width, true, true,
                         insn_eip, &slot)) return false;
    cpu_modrm_write(cpu, &slot, width, value);
    cpu->esp = next;
    return true;
}

bool cpu8086_read_stack_words(cpu8086_state_t *cpu, uint16_t *words, unsigned count)
{
    if (!cpu || !words || !count || count > 32u || cpu->delivery_fault) return false;
    cpu_stack_access_t stack;
    modrm_t slots[32];
    cpu_event_fault_t fault = {0};
    cpu->delivery_fault = &fault;
    bool valid = cpu_stack_begin(cpu, &stack, false, cpu->eip);
    for (unsigned i = 0; valid && i < count; i++) {
        valid = cpu_stack_slot(cpu, &stack, (cpu->esp + i * 2u) & stack.mask,
                                  2, false, false, cpu->eip, &slots[i]);
        if (valid) for (unsigned byte = 0; byte < 2; byte++)
            if (slots[i].physical[byte] >= cpu->vm->total_mem_size) valid = false;
    }
    cpu->delivery_fault = NULL;
    /* Resolve every word before copying: SS.B wraps between words, not
     * within one, and a paged record need not be physically contiguous. */
    if (valid) for (unsigned i = 0; i < count; i++)
        words[i] = (uint16_t)cpu_modrm_read(cpu, &slots[i], 2);
    return valid;
}

static bool cpu_stack_pop_value(cpu8086_state_t *cpu, uint32_t *value,
                                 unsigned width, bool locked, uint32_t insn_eip)
{
    cpu_stack_access_t stack;
    modrm_t slot;
    if (!cpu_stack_begin(cpu, &stack, locked, insn_eip) ||
        !cpu_stack_slot(cpu, &stack, cpu->esp & stack.mask, width, false, false,
                         insn_eip, &slot)) return false;
    *value = cpu_modrm_read(cpu, &slot, width);
    cpu->esp = cpu_stack_next_esp(&stack, cpu->esp, cpu->esp + width);
    return true;
}

static void cpu_flags_stack(cpu8086_state_t *cpu, bool pop, unsigned width,
                              uint32_t insn_eip)
{
    uint32_t old = cpu8086_flags_image(cpu);
    unsigned cpl = cpu8086_cpl(cpu), iopl = (old & FLAG_IOPL_MASK) >> 12;
    /* VME is not advertised by this CPU. Privilege rejection precedes SS
     * and paging checks, and must not consume the operand. */
    if ((cpu->cr0 & 1u) && (old & FLAG_VM) && iopl != 3u) {
        (void)cpu_deliver_exception(cpu->vm, 13, insn_eip, 0, true);
        return;
    }
    if (!pop) {
        (void)cpu_stack_push_value(cpu, old & ~(FLAG_RF | FLAG_VM), width, false, insn_eip);
        return;
    }
    uint32_t value;
    if (!cpu_stack_pop_value(cpu, &value, width, false, insn_eip)) return;
    uint32_t writable = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF |
                        FLAG_TF | FLAG_DF | FLAG_OF | FLAG_NT;
    if (width == 4) writable |= FLAG_AC | FLAG_ID;
    if (cpl == 0) writable |= FLAG_IOPL_MASK;
    if (cpl <= iopl) writable |= FLAG_IF;
    /* POPF cannot load RF, VM, VIF or VIP. RF is cleared even for word POPF. */
    cpu->eflags = (((old & ~writable) | (value & writable)) & ~FLAG_RF) | FLAGS_FIXED;
    cpu8086_restore_virtual_flags(cpu);
}

static void cpu_interrupt_flag(cpu8086_state_t *cpu, bool enable, uint32_t insn_eip)
{
    dos_vm_t *vm = cpu->vm;
    bool host = cpu->protected_mode && !cpu->guest_idt && vm->dpmi.active;
    if (!host && cpu8086_cpl(cpu) > ((cpu->eflags & FLAG_IOPL_MASK) >> 12)) {
        (void)cpu_deliver_exception(vm, 13, insn_eip, 0, true);
        return;
    }
    bool was_enabled = host ? vm->dpmi.virtual_interrupts_enabled : (cpu->eflags & FLAG_IF) != 0;
    if (enable && !was_enabled && !cpu->irq_shadow) cpu->irq_shadow = 2;
    if (host) vm->dpmi.virtual_interrupts_enabled = enable;
    cpu->eflags = (cpu->eflags & ~FLAG_IF) | (host || enable ? FLAG_IF : 0);
}

static bool cpu_near_target_valid(cpu8086_state_t *cpu, uint32_t target,
                                   uint32_t limit, uint32_t insn_eip)
{
    if (target <= limit) return true;
    (void)cpu_deliver_exception(cpu->vm, 13, insn_eip, 0, true);
    return false;
}

static bool cpu_near_jump(cpu8086_state_t *cpu, uint32_t target, bool wide,
                          uint32_t limit, uint32_t insn_eip)
{
    if (!wide) target = (uint16_t)target;
    if (!cpu_near_target_valid(cpu, target, limit, insn_eip)) return false;
    cpu->eip = target;
    return true;
}

static void cpu_near_call(cpu8086_state_t *cpu, uint32_t target, unsigned width,
                           uint32_t limit, uint32_t insn_eip)
{
    if (width == 2) target = (uint16_t)target;
    /* The CALL contract checks the destination before attempting the push. */
    if (!cpu_near_target_valid(cpu, target, limit, insn_eip) ||
        !cpu_stack_push_value(cpu, cpu->eip, width, false, insn_eip)) return;
    cpu->eip = target;
}

static void cpu_near_return(cpu8086_state_t *cpu, unsigned width, unsigned discard,
                             uint32_t limit, uint32_t insn_eip)
{
    cpu_stack_access_t stack;
    modrm_t source;
    if (!cpu_stack_begin(cpu, &stack, false, insn_eip) ||
        !cpu_stack_slot(cpu, &stack, cpu->esp & stack.mask, width, false, false,
                         insn_eip, &source)) return;
    uint32_t target = cpu_modrm_read(cpu, &source, width);
    if (!cpu_near_target_valid(cpu, target, limit, insn_eip)) return;
    cpu->esp = cpu_stack_next_esp(&stack, cpu->esp, cpu->esp + width + discard);
    cpu->eip = target;
}

static bool cpu_far_fault(cpu8086_state_t *cpu, unsigned vector, uint16_t selector,
                           uint32_t insn_eip)
{
    (void)cpu_deliver_exception(cpu->vm, vector, insn_eip, selector & ~3u, true);
    return false;
}

static bool cpu_far_descriptor(cpu8086_state_t *cpu, uint16_t selector,
                                unsigned vector, uint32_t insn_eip,
                                dpmi_descriptor_ref_t *reference)
{
    dos_page_fault_t fault;
    if (dpmi_lookup_descriptor(cpu->vm, selector, reference, &fault)) return true;
    if (fault.raised) cpu_raise_page_fault(cpu, insn_eip, &fault);
    else cpu_far_fault(cpu, vector, selector, insn_eip);
    return false;
}

static bool cpu_far_accessed(cpu8086_state_t *cpu, dpmi_descriptor_ref_t *reference,
                              uint32_t insn_eip)
{
    dos_page_fault_t fault;
    if (dpmi_descriptor_set_accessed(cpu->vm, reference, &fault)) {
        reference->descriptor.access |= DESC_ACCESSED;
        return true;
    }
    cpu_raise_page_fault(cpu, insn_eip, &fault);
    return false;
}

static bool cpu_load_system_segment(cpu8086_state_t *cpu, uint16_t selector,
                                     bool tss, uint32_t insn_eip)
{
    if (!tss && !(selector & ~3u)) {
        cpu8086_cache_ldtr(cpu, selector, NULL);
        return true;
    }
    if (selector & 4u) return cpu_far_fault(cpu, 13, selector, insn_eip);
    dpmi_descriptor_ref_t reference;
    if (!cpu_far_descriptor(cpu, selector, 13, insn_eip, &reference)) return false;
    dpmi_descriptor_t *d = &reference.descriptor;
    unsigned type = d->access & 15u;
    if ((d->access & DESC_SEGMENT) || (tss ? type != 1u && type != 9u : type != 2u))
        return cpu_far_fault(cpu, 13, selector, insn_eip);
    if (!(d->access & DESC_PRESENT)) return cpu_far_fault(cpu, 11, selector, insn_eip);
    if (tss) {
        dos_page_fault_t fault;
        if (!dpmi_descriptor_set_busy(cpu->vm, &reference, &fault)) {
            cpu_raise_page_fault(cpu, insn_eip, &fault);
            return false;
        }
        d->access |= 2u;
        cpu8086_cache_tr(cpu, selector, d);
    } else cpu8086_cache_ldtr(cpu, selector, d);
    return true;
}

/* Validate the whole frame, including selector padding, without wrapping
 * individual words through the stack-address boundary. */
static bool cpu_far_stack_range(cpu8086_state_t *cpu, const cpu_stack_access_t *stack,
                                 uint32_t start, unsigned size, uint32_t insn_eip)
{
    if (!size) return true;
    uint64_t last = (uint64_t)start + size - 1u;
    if (stack->expand_down ? start <= stack->limit || last > stack->top
                           : last > stack->limit)
        return cpu_far_fault(cpu, 12, 0, insn_eip);
    return true;
}

static bool cpu_push_private_int_frame(cpu8086_state_t *cpu, unsigned width,
                                        uint32_t return_eip, uint32_t flags,
                                        uint32_t insn_eip)
{
    cpu_stack_access_t stack;
    if (!cpu_stack_begin(cpu, &stack, false, insn_eip)) return false;
    unsigned size = 3u * width;
    uint32_t next = cpu_stack_next_esp(&stack, cpu->esp, cpu->esp - size);
    uint32_t start = next & stack.mask, physical[12];
    if (!cpu_far_stack_range(cpu, &stack, start, size, insn_eip) ||
        !cpu_memory_addresses_at_cpl(cpu, stack.base + start, size, true, true,
                                      stack.cpl, insn_eip, physical)) return false;
    const uint32_t fields[] = { return_eip, cpu->cs, flags };
    for (unsigned i = 0; i < 3; i++)
        for (unsigned b = 0; b < width; b++)
            dos_mem_write8(cpu->vm, physical[i * width + b], fields[i] >> (8u * b));
    cpu->esp = next;
    return true;
}

bool cpu8086_enter_dpmi_interrupt(cpu8086_state_t *cpu, uint16_t selector,
                                   uint32_t offset, unsigned width,
                                   uint32_t return_eip, uint32_t flags,
                                   uint32_t fault_eip)
{
    dpmi_descriptor_ref_t code;
    if (!cpu || !cpu->protected_mode || (width != 2u && width != 4u)) return false;
    if (!cpu_far_descriptor(cpu, selector, 13, fault_eip, &code)) return false;
    const dpmi_descriptor_t *d = &code.descriptor;
    if ((selector & 3u) != 3u ||
        (d->access & (DESC_SEGMENT | DESC_CODE)) != (DESC_SEGMENT | DESC_CODE) ||
        (d->flags_lim & 0x20u) ||
        (!(d->access & 4u) && (d->access & DESC_DPL_MASK) != DESC_DPL3))
        return cpu_far_fault(cpu, 13, selector, fault_eip);
    if (!(d->access & DESC_PRESENT)) return cpu_far_fault(cpu, 11, selector, fault_eip);
    if (offset > dpmi_desc_get_limit(d)) return cpu_far_fault(cpu, 13, 0, fault_eip);
    if (!cpu_far_accessed(cpu, &code, fault_eip) ||
        !cpu_push_private_int_frame(cpu, width, return_eip, flags, fault_eip)) return false;
    /* The frame may alias the descriptor table. Enter the admitted snapshot. */
    cpu8086_cache_cs(cpu, selector, &code.descriptor, 3u);
    cpu->eip = offset;
    return true;
}

static bool cpu_far_stack_load(cpu8086_state_t *cpu, uint16_t selector, unsigned cpl,
                                bool from_tss, uint32_t insn_eip,
                                dpmi_descriptor_ref_t *reference,
                                cpu_stack_access_t *stack)
{
    unsigned invalid = from_tss ? 10u : 13u;
    if (!cpu_far_descriptor(cpu, selector, invalid, insn_eip, reference)) return false;
    dpmi_descriptor_t *d = &reference->descriptor;
    if ((d->access & (DESC_SEGMENT | DESC_CODE | DESC_WRITABLE)) !=
            (DESC_SEGMENT | DESC_WRITABLE) || (selector & 3u) != cpl ||
        ((d->access & DESC_DPL_MASK) >> 5) != cpl)
        return cpu_far_fault(cpu, invalid, selector, insn_eip);
    if (!(d->access & DESC_PRESENT)) return cpu_far_fault(cpu, 12, selector, insn_eip);
    *stack = (cpu_stack_access_t){ .base = dpmi_desc_get_base(d),
        .limit = dpmi_desc_get_limit(d), .cpl = cpl,
        .mask = d->flags_lim & DESC_32BIT ? UINT32_MAX : 0xFFFFu,
        .expand_down = (d->access & 4u) != 0 };
    stack->top = stack->mask;
    return true;
}

static bool cpu_far_tss_stack(cpu8086_state_t *cpu, unsigned cpl, uint32_t insn_eip,
                               uint16_t *selector, uint32_t *esp)
{
    if (!cpu->tss_cache.valid || !(cpu->tr & ~3u) || (cpu->tr & 4u))
        return cpu_far_fault(cpu, 10, cpu->tr, insn_eip);
    const dpmi_descriptor_t *d = &cpu->tss_cache.descriptor;
    unsigned type = d->access & 0x0Fu;
    if ((d->access & DESC_SEGMENT) || !(d->access & DESC_PRESENT) ||
        (type != 1u && type != 3u && type != 9u && type != 11u))
        return cpu_far_fault(cpu, 10, cpu->tr, insn_eip);
    unsigned width = type & 8u ? 4u : 2u;
    unsigned offset = width + cpl * width * 2u, size = width + 2u;
    if ((uint64_t)offset + size - 1u > dpmi_desc_get_limit(d))
        return cpu_far_fault(cpu, 10, cpu->tr, insn_eip);
    uint32_t addresses[6];
    if (!cpu_memory_addresses_at_cpl(cpu, dpmi_desc_get_base(d) + offset,
                                      size, false, false, 0, insn_eip, addresses)) return false;
    *esp = 0;
    for (unsigned i = 0; i < width; i++)
        *esp |= (uint32_t)dos_mem_read8(cpu->vm, addresses[i]) << (i * 8u);
    *selector = dos_mem_read8(cpu->vm, addresses[width]) |
                ((uint16_t)dos_mem_read8(cpu->vm, addresses[width + 1u]) << 8);
    return true;
}

static void cpu_far_commit(cpu8086_state_t *cpu, uint16_t selector, uint32_t target,
                            const dpmi_descriptor_ref_t *code)
{
    cpu->eip = target;
    if (code) cpu8086_cache_cs(cpu, selector, &code->descriptor, selector & 3u);
    else cpu8086_load_real_cs(cpu, selector);
    /* The checked descriptor is already available. Do not reread a table
     * that a just-written return frame could alias. */
    if (cpu->protected_mode && cpu->op_size_32 && (selector & 3u) == 3u)
        dos_transfer_to_native(cpu->vm);
}

typedef enum { CPU_TASK_JUMP, CPU_TASK_CALL, CPU_TASK_RETURN } cpu_task_source_t;

static bool cpu_task_current(cpu8086_state_t *cpu, uint32_t fault_eip)
{
    const dpmi_descriptor_t *d = &cpu->tss_cache.descriptor;
    unsigned type = d->access & 31u;
    if (!cpu->tss_cache.valid || !(cpu->tr & ~7u) || (cpu->tr & 4u) ||
        (type != 1 && type != 3 && type != 9 && type != 11) || !(d->access & DESC_PRESENT) ||
        dpmi_desc_get_limit(d) < (type & 8u ? 103u : 43u))
        return cpu_far_fault(cpu, 10, cpu->tr, fault_eip);
    return true;
}

static uint32_t cpu_task_read(dos_vm_t *vm, const uint32_t *addresses,
                               unsigned offset, unsigned width)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < width; i++)
        value |= (uint32_t)dos_mem_read8(vm, addresses[offset + i]) << (8u * i);
    return value;
}

static void cpu_task_write(dos_vm_t *vm, const uint32_t *addresses,
                            unsigned offset, unsigned width, uint32_t value)
{
    for (unsigned i = 0; i < width; i++)
        dos_mem_write8(vm, addresses[offset + i], value >> (8u * i));
}

static bool cpu_task_segment(cpu8086_state_t *cpu, unsigned segment)
{
    uint16_t selector = *seg_ptr(cpu, segment);
    if (!(selector & ~3u)) {
        if (segment == 1 || segment == 2) return cpu_far_fault(cpu, 10, selector, cpu->eip);
        return true;
    }
    dpmi_descriptor_ref_t reference;
    if (!cpu_far_descriptor(cpu, selector, 10, cpu->eip, &reference)) return false;
    const dpmi_descriptor_t *d = &reference.descriptor;
    unsigned cpl = cpu8086_cpl(cpu), rpl = selector & 3u, dpl = (d->access >> 5) & 3u;
    bool code = (d->access & DESC_CODE) != 0, conforming = code && (d->access & 4u);
    bool valid = (d->access & DESC_SEGMENT) != 0;
    if (segment == 1) valid &= code && (conforming ? dpl <= rpl : dpl == rpl);
    else if (segment == 2) valid &= !code && (d->access & DESC_WRITABLE) && dpl == cpl && rpl == cpl;
    else valid &= (!code || (d->access & DESC_READABLE)) && (conforming || (dpl >= cpl && dpl >= rpl));
    if (!valid) return cpu_far_fault(cpu, 10, selector, cpu->eip);
    if (!(d->access & DESC_PRESENT)) return cpu_far_fault(cpu, segment == 2 ? 12 : 11, selector, cpu->eip);
    if (!cpu_far_accessed(cpu, &reference, cpu->eip)) return false;
    if (segment == 1) cpu8086_cache_cs(cpu, selector, d, cpl);
    else cpu8086_cache_segment(cpu, segment, selector, d);
    return true;
}

static bool cpu_task_switch(cpu8086_state_t *cpu, uint16_t selector, cpu_task_source_t source,
                             uint32_t return_eip, uint32_t fault_eip, uint32_t saved_flags,
                             bool has_error, uint32_t error)
{
    dos_vm_t *vm = cpu->vm;
    unsigned admission_fault = source == CPU_TASK_RETURN ? 10u : 13u;
    if (selector & 4u) return cpu_far_fault(cpu, admission_fault, selector, fault_eip);
    dpmi_descriptor_ref_t reference;
    if (!cpu_far_descriptor(cpu, selector, admission_fault, fault_eip, &reference)) return false;
    dpmi_descriptor_t descriptor = reference.descriptor;
    unsigned type = descriptor.access & 31u;
    bool wide = (type & 8u) != 0;
    if (type != (source == CPU_TASK_RETURN ? (wide ? 11u : 3u) : (wide ? 9u : 1u)))
        return cpu_far_fault(cpu, admission_fault, selector, fault_eip);
    if (!(descriptor.access & DESC_PRESENT)) return cpu_far_fault(cpu, 11, selector, fault_eip);
    if (dpmi_desc_get_limit(&descriptor) < (wide ? 103u : 43u))
        return cpu_far_fault(cpu, 10, selector, fault_eip);
    if (!cpu_task_current(cpu, fault_eip)) return false;

    /* Probe the old address space before writing task state. TR's loaded
     * base/limit are authoritative even if its GDT descriptor was edited. */
    bool old_wide = (cpu->tss_cache.descriptor.access & 8u) != 0;
    unsigned old_width = old_wide ? 4u : 2u, old_start = old_wide ? 32u : 14u;
    unsigned new_width = wide ? 4u : 2u, new_start = wide ? 28u : 14u;
    uint32_t old_base = dpmi_desc_get_base(&cpu->tss_cache.descriptor);
    uint32_t base = dpmi_desc_get_base(&descriptor);
    uint32_t outgoing[62], incoming[74], backlink[2], old_busy = 0, new_busy = 0;
    if (!cpu_memory_addresses_at_cpl(cpu, old_base + old_start, old_wide ? 62u : 28u,
                                      true, true, 0, fault_eip, outgoing) ||
        !cpu_memory_addresses_at_cpl(cpu, base + new_start, wide ? 74u : 30u,
                                      false, false, 0, fault_eip, incoming)) return false;
    if (source == CPU_TASK_CALL &&
        !cpu_memory_addresses_at_cpl(cpu, base, 2, true, true, 0, fault_eip, backlink)) return false;
    if (source != CPU_TASK_CALL &&
        !cpu_memory_addresses_at_cpl(cpu, cpu->gdtr.base + (cpu->tr & ~7u) + 5u,
                                      1, true, true, 0, fault_eip, &old_busy)) return false;
    if (source != CPU_TASK_RETURN &&
        !cpu_memory_addresses_at_cpl(cpu, reference.linear + 5u, 1, true, true,
                                      0, fault_eip, &new_busy)) return false;

    if (source == CPU_TASK_RETURN) saved_flags &= ~FLAG_NT;
    cpu_task_write(vm, outgoing, 0, old_width, return_eip);
    cpu_task_write(vm, outgoing, old_width, old_width, saved_flags);
    for (unsigned i = 0; i < 8; i++)
        cpu_task_write(vm, outgoing, (i + 2u) * old_width, old_width, *reg32_ptr(cpu, i));
    for (unsigned s = 0; s < (old_wide ? 6u : 4u); s++)
        cpu_task_write(vm, outgoing, (s + 10u) * old_width, 2, *seg_ptr(cpu, s));

    unsigned first = wide ? 4u : 0;
    uint32_t next_cr3 = wide ? cpu_task_read(vm, incoming, 0, 4) : cpu->cr3;
    uint32_t next_eip = cpu_task_read(vm, incoming, first, new_width);
    uint32_t flags = cpu_task_read(vm, incoming, first + new_width, new_width);
    uint32_t registers[8];
    uint16_t segments[6] = {0};
    for (unsigned i = 0; i < 8; i++)
        registers[i] = cpu_task_read(vm, incoming, first + (i + 2u) * new_width, new_width);
    for (unsigned s = 0; s < (wide ? 6u : 4u); s++)
        segments[s] = cpu_task_read(vm, incoming, first + (s + 10u) * new_width, 2);
    uint16_t ldt = cpu_task_read(vm, incoming, wide ? 68u : 28u, 2);
    bool trap = wide && (cpu_task_read(vm, incoming, 72, 2) & 1u);
    if (source == CPU_TASK_CALL) {
        cpu_task_write(vm, backlink, 0, 2, cpu->tr);
        flags |= FLAG_NT;
    } else dos_mem_write8(vm, old_busy, dos_mem_read8(vm, old_busy) & ~2u);
    if (source != CPU_TASK_RETURN) dos_mem_write8(vm, new_busy, dos_mem_read8(vm, new_busy) | 2u);

    /* Commit: all subsequent descriptor/stack faults belong to the incoming
     * task. Install every visible selector before qualifying any descriptor. */
    descriptor.access |= 2u;
    cpu8086_cache_tr(cpu, selector, &descriptor);
    cpu->cr0 |= DOS_CR0_TS;
    if (wide && (cpu->cr0 & DOS_CR0_PG)) cpu->cr3 = next_cr3 & DOS_CR3_VALID;
    cpu->eflags = (flags & (wide ? 0x003F7FD5u : 0x7FD5u)) | FLAGS_FIXED;
    cpu->protected_mode = !(cpu->eflags & FLAG_VM);
    cpu->guest_idt = true;
    cpu->irq_shadow = 0;
    cpu->rep_compare.active = false;
    cpu->halted = false;
    cpu->dr[7] &= ~0x55u;
    for (unsigned i = 0; i < 8; i++) *reg32_ptr(cpu, i) = registers[i];
    cpu8086_cache_cs(cpu, segments[1], NULL, cpu->protected_mode ? segments[1] & 3u : 3u);
    for (unsigned s = 0; s < 6; s++) if (s != 1)
        cpu8086_cache_segment(cpu, s, segments[s], NULL);
    if (!cpu->protected_mode) {
        cpu8086_load_real_cs(cpu, segments[1]);
        for (unsigned s = 0; s < 6; s++) if (s != 1) cpu8086_load_real_segment(cpu, s, segments[s]);
    }
    cpu->eip = next_eip;
    cpu8086_cache_ldtr(cpu, ldt, NULL);
    if (ldt & 4u) return cpu_far_fault(cpu, 10, ldt, next_eip);
    if (ldt & ~3u) {
        dpmi_descriptor_ref_t entry;
        if (!cpu_far_descriptor(cpu, ldt, 10, next_eip, &entry)) return false;
        if ((entry.descriptor.access & (DESC_PRESENT | 31u)) != 0x82u)
            return cpu_far_fault(cpu, 10, ldt, next_eip);
        cpu8086_cache_ldtr(cpu, ldt, &entry.descriptor);
    }
    if (cpu->protected_mode) {
        const uint8_t order[] = {1, 2, 0, 3, 4, 5};
        for (unsigned i = 0; i < 6; i++) if (!cpu_task_segment(cpu, order[i])) return false;
    }
    if (!cpu_near_target_valid(cpu, next_eip, dpmi_desc_get_limit(&cpu->cs_cache.descriptor), next_eip)) return false;
    if (has_error && !cpu_stack_push_value(cpu, error, new_width, false, next_eip)) return false;
    if (trap) {
        cpu->dr[6] |= 1u << 15;
        cpu->dr[7] &= ~(1u << 13);
        if (cpu->delivery_fault) {
            cpu->delivery_fault->task_trap = true;
            cpu->delivery_fault->return_eip = next_eip;
            return true;
        }
        return cpu_deliver_exception(vm, 1, next_eip, 0, false);
    }
    return true;
}

static void cpu_far_transfer(cpu8086_state_t *cpu, uint16_t selector, uint32_t target,
                              unsigned width, bool call, bool locked, uint32_t insn_eip)
{
    if (locked) {
        (void)cpu_deliver_exception(cpu->vm, 6, insn_eip, 0, false);
        return;
    }
    dpmi_descriptor_ref_t code = {0}, new_ss = {0};
    unsigned cpl = cpu8086_cpl(cpu);
    unsigned dest_cpl = cpl, parameters = 0;
    uint16_t stack_selector = cpu->ss;
    uint32_t stack_pointer = cpu->esp, limit = dpmi_desc_get_limit(&cpu->cs_cache.descriptor);
    bool inward = false;
    if (width == 2) target = (uint16_t)target;
    if (cpu->protected_mode) {
        if (!cpu_far_descriptor(cpu, selector, 13, insn_eip, &code)) return;
        dpmi_descriptor_t d = code.descriptor;
        unsigned dpl = (d.access & DESC_DPL_MASK) >> 5;
        bool gate = !(d.access & DESC_SEGMENT);
        if (gate) {
            unsigned type = d.access & 0x0Fu;
            if (type != 4u && type != 12u && type != 1u && type != 5u && type != 9u) {
                cpu_far_fault(cpu, 13, selector, insn_eip);
                return;
            }
            if (dpl < cpl || dpl < (selector & 3u)) {
                cpu_far_fault(cpu, 13, selector, insn_eip);
                return;
            }
            if (!(d.access & DESC_PRESENT)) {
                cpu_far_fault(cpu, 11, selector, insn_eip);
                return;
            }
            if (type == 1u || type == 5u || type == 9u) {
                (void)cpu_task_switch(cpu, type == 5u ? d.base_lo : selector,
                    call ? CPU_TASK_CALL : CPU_TASK_JUMP, cpu->eip, insn_eip,
                    cpu8086_flags_image(cpu), false, 0);
                return;
            }
            width = type == 12u ? 4u : 2u;
            target = d.limit_lo;
            if (width == 4) target |= ((uint32_t)d.flags_lim << 16) | ((uint32_t)d.base_hi << 24);
            parameters = d.base_mid & 31u;
            selector = d.base_lo;
            if (!cpu_far_descriptor(cpu, selector, 13, insn_eip, &code)) return;
            d = code.descriptor;
            dpl = (d.access & DESC_DPL_MASK) >> 5;
        }
        bool conforming = (d.access & 4u) != 0;
        if ((d.access & (DESC_SEGMENT | DESC_CODE)) != (DESC_SEGMENT | DESC_CODE) ||
            (conforming ? dpl > cpl : gate && call ? dpl > cpl
                : dpl != cpl || (!gate && (selector & 3u) > cpl))) {
            cpu_far_fault(cpu, 13, selector, insn_eip);
            return;
        }
        if (!(d.access & DESC_PRESENT)) {
            cpu_far_fault(cpu, 11, selector, insn_eip);
            return;
        }
        inward = gate && call && !conforming && dpl < cpl;
        if (inward) dest_cpl = dpl;
        else parameters = 0;
        selector = (selector & ~3u) | dest_cpl;
        limit = dpmi_desc_get_limit(&d);
    }

    cpu_stack_access_t stack;
    modrm_t destinations[35];
    uint32_t values[35];
    unsigned count = inward ? parameters + 4u : 2u;
    uint32_t next = cpu->esp;
    if (call) {
        if (inward) {
            if (!cpu_far_tss_stack(cpu, dest_cpl, insn_eip, &stack_selector, &stack_pointer) ||
                !cpu_far_stack_load(cpu, stack_selector, dest_cpl, true, insn_eip, &new_ss, &stack)) return;
        } else if (!cpu_stack_begin(cpu, &stack, false, insn_eip)) return;
        next = cpu_stack_next_esp(&stack, stack_pointer, stack_pointer - count * width);
        if (!cpu_far_stack_range(cpu, &stack, next & stack.mask, count * width, insn_eip)) return;
    }
    if (!cpu_near_target_valid(cpu, target, limit, insn_eip)) return;
    if (call) {
        values[0] = cpu->eip;
        values[1] = cpu->cs;
        if (inward) {
            cpu_stack_access_t source;
            if (parameters) {
                if (!cpu_stack_begin(cpu, &source, false, insn_eip) ||
                    !cpu_far_stack_range(cpu, &source, cpu->esp & source.mask,
                                          parameters * width, insn_eip)) return;
                for (unsigned i = 0; i < parameters; i++) {
                    modrm_t slot;
                    if (!cpu_stack_slot(cpu, &source, (cpu->esp & source.mask) + i * width,
                                          width, false, false, insn_eip, &slot)) return;
                    values[i + 2u] = cpu_modrm_read(cpu, &slot, width);
                }
            }
            values[count - 2u] = cpu->esp;
            values[count - 1u] = cpu->ss;
        }
        for (unsigned i = 0; i < count; i++)
            if (!cpu_stack_slot(cpu, &stack, (next & stack.mask) + i * width,
                                 width, true, true, insn_eip, &destinations[i])) return;
    }
    if (cpu->protected_mode && !cpu_far_accessed(cpu, &code, insn_eip)) return;
    if (inward && !cpu_far_accessed(cpu, &new_ss, insn_eip)) return;
    if (call) {
        for (unsigned i = 0; i < count; i++) cpu_modrm_write(cpu, &destinations[i], width, values[i]);
        cpu->esp = next;
        if (inward) cpu8086_cache_segment(cpu, 2, stack_selector, &new_ss.descriptor);
    }
    cpu_far_commit(cpu, selector, target, cpu->protected_mode ? &code : NULL);
}

static void cpu_far_return(cpu8086_state_t *cpu, unsigned width, unsigned discard,
                            bool locked, uint32_t insn_eip)
{
    cpu_stack_access_t stack, outer;
    modrm_t source[2];
    if (!cpu_stack_begin(cpu, &stack, locked, insn_eip)) return;
    uint32_t start = cpu->esp & stack.mask;
    if (!cpu_far_stack_range(cpu, &stack, start, 2u * width, insn_eip)) return;
    /* Fetch the selector slot first, as required for far-return validation. */
    for (unsigned i = 2; i-- > 0;)
        if (!cpu_stack_slot(cpu, &stack, start + i * width, width, false, false,
                              insn_eip, &source[i])) return;
    uint16_t selector = (uint16_t)cpu_modrm_read(cpu, &source[1], width);
    uint32_t target = cpu_modrm_read(cpu, &source[0], width);
    uint32_t next = cpu_stack_next_esp(&stack, cpu->esp, cpu->esp + 2u * width + discard);
    uint16_t stack_selector = cpu->ss;
    dpmi_descriptor_ref_t code = {0}, new_ss = {0};
    unsigned clear_segments = 0;
    if (cpu->protected_mode) {
        if (!cpu_far_descriptor(cpu, selector, 13, insn_eip, &code)) return;
        dpmi_descriptor_t d = code.descriptor;
        unsigned cpl = cpu8086_cpl(cpu);
        unsigned rpl = selector & 3u, dpl = (d.access & DESC_DPL_MASK) >> 5;
        if (rpl < cpl || (d.access & (DESC_SEGMENT | DESC_CODE)) != (DESC_SEGMENT | DESC_CODE) ||
            (d.access & 4u ? dpl > rpl : dpl != rpl)) {
            cpu_far_fault(cpu, 13, selector, insn_eip);
            return;
        }
        if (!(d.access & DESC_PRESENT)) {
            cpu_far_fault(cpu, 11, selector, insn_eip);
            return;
        }
        if (rpl > cpl) {
            if (!cpu_far_stack_range(cpu, &stack, start, 4u * width + discard, insn_eip)) return;
            unsigned offset = 2u * width + discard;
            for (unsigned i = 2; i-- > 0;)
                if (!cpu_stack_slot(cpu, &stack, start + offset + i * width, width,
                                      false, false, insn_eip, &source[i])) return;
            stack_selector = (uint16_t)cpu_modrm_read(cpu, &source[1], width);
            next = cpu_modrm_read(cpu, &source[0], width);
            if (!cpu_far_stack_load(cpu, stack_selector, rpl, false, insn_eip, &new_ss, &outer)) return;
            next = cpu_stack_next_esp(&outer, next, next + discard);
            for (unsigned s = 0; s < 6; s++) {
                if (s == 1 || s == 2) continue;
                if (!cpu->segment_cache[s].valid) {
                    clear_segments |= 1u << s;
                    continue;
                }
                uint8_t access = cpu->segment_cache[s].descriptor.access;
                bool conforming = (access & (DESC_SEGMENT | DESC_CODE | 4u)) ==
                                            (DESC_SEGMENT | DESC_CODE | 4u);
                if (!conforming && rpl > ((access & DESC_DPL_MASK) >> 5)) clear_segments |= 1u << s;
            }
        }
        if (!cpu_near_target_valid(cpu, target, dpmi_desc_get_limit(&d), insn_eip) ||
            !cpu_far_accessed(cpu, &code, insn_eip)) return;
        if (rpl > cpl && !cpu_far_accessed(cpu, &new_ss, insn_eip)) return;
    } else if (!cpu_near_target_valid(cpu, target,
                dpmi_desc_get_limit(&cpu->cs_cache.descriptor), insn_eip)) return;
    cpu->esp = next;
    if (cpu->protected_mode && (selector & 3u) > cpu8086_cpl(cpu))
        cpu8086_cache_segment(cpu, 2, stack_selector, &new_ss.descriptor);
    for (unsigned s = 0; s < 6; s++) if (clear_segments & (1u << s))
        cpu8086_cache_segment(cpu, s, 0, NULL);
    cpu_far_commit(cpu, selector, target, cpu->protected_mode ? &code : NULL);
}

static bool cpu_iret_read(cpu8086_state_t *cpu, const cpu_stack_access_t *stack,
                           uint32_t offset, unsigned width, uint32_t insn_eip,
                           uint32_t *value)
{
    modrm_t slot;
    if (!cpu_stack_slot(cpu, stack, offset, width, false, false, insn_eip, &slot)) return false;
    *value = cpu_modrm_read(cpu, &slot, width);
    return true;
}

static void cpu_interrupt_return(cpu8086_state_t *cpu, unsigned width,
                                  bool locked, uint32_t insn_eip)
{
    bool v86 = (cpu->cr0 & 1u) && (cpu->eflags & FLAG_VM);
    bool pm = cpu->protected_mode && !v86;
    unsigned cpl = cpu8086_cpl(cpu);
    uint32_t old_flags = cpu8086_flags_image(cpu);
    unsigned iopl = (old_flags & FLAG_IOPL_MASK) >> 12;
    if (locked) {
        (void)cpu_deliver_exception(cpu->vm, 6, insn_eip, 0, false);
        return;
    }
    if (v86 && iopl != 3u) {
        cpu_far_fault(cpu, 13, 0, insn_eip); /* CR4.VME is not exposed. */
        return;
    }
    if (pm && (old_flags & FLAG_NT)) {
        uint32_t addresses[2];
        if (!cpu_task_current(cpu, insn_eip) ||
            !cpu_memory_addresses_at_cpl(cpu, dpmi_desc_get_base(&cpu->tss_cache.descriptor),
                                          2, false, false, 0, insn_eip, addresses)) return;
        uint16_t selector = cpu_task_read(cpu->vm, addresses, 0, 2);
        (void)cpu_task_switch(cpu, selector, CPU_TASK_RETURN, cpu->eip, insn_eip,
                               old_flags, false, 0);
        return;
    }

    cpu_stack_access_t stack, outer;
    if (!cpu_stack_begin(cpu, &stack, false, insn_eip)) return;
    uint32_t start = cpu->esp & stack.mask, frame[9] = {0};
    if (pm && !cpu_far_stack_range(cpu, &stack, start, width * 3u, insn_eip)) return;
    for (unsigned n = 0; n < 3; n++) {
        unsigned field = pm ? 2u - n : n;
        /* Real/v86 pops wrap between fields according to SS.B. A single
         * field must still fit; protected IRET checks a contiguous frame. */
        uint32_t offset = start + field * width;
        if (!pm) offset &= stack.mask;
        if (!cpu_iret_read(cpu, &stack, offset, width,
                            insn_eip, &frame[field])) return;
    }
    uint32_t target = frame[0];
    uint16_t selector = (uint16_t)frame[1], stack_selector = cpu->ss;
    uint32_t next = cpu_stack_next_esp(&stack, cpu->esp, cpu->esp + width * 3u);
    dpmi_descriptor_ref_t code = {0}, new_ss = {0};
    bool outward = false;

    if (pm && !cpl && width == 4 && (frame[2] & FLAG_VM)) {
        if (!cpu_far_stack_range(cpu, &stack, start, 36, insn_eip)) return;
        for (unsigned field = 3; field < 9; field++)
            if (!cpu_iret_read(cpu, &stack, start + field * 4u, 4,
                                insn_eip, &frame[field])) return;
        /* The v86 bridge retains PE/PG while using real segment addressing.
         * IRETD loads all of ESP; the v86 stack subsequently addresses SP. */
        cpu->guest_idt = cpu8086_uses_guest_idt(cpu);
        cpu->eflags = (frame[2] & 0x003F7FD5u) | FLAGS_FIXED;
        cpu->protected_mode = false;
        cpu->esp = frame[3];
        cpu->eip = (uint16_t)target;
        cpu8086_load_real_cs(cpu, selector);
        const unsigned segments[] = { 2, 0, 3, 4, 5 };
        for (unsigned n = 0; n < 5; n++)
            cpu8086_load_real_segment(cpu, segments[n], (uint16_t)frame[4u + n]);
        return;
    }

    if (pm) {
        if (!cpu_far_descriptor(cpu, selector, 13, insn_eip, &code)) return;
        const dpmi_descriptor_t *d = &code.descriptor;
        unsigned rpl = selector & 3u, dpl = (d->access & DESC_DPL_MASK) >> 5;
        if (rpl < cpl || (d->access & (DESC_SEGMENT | DESC_CODE)) !=
                (DESC_SEGMENT | DESC_CODE) || (d->access & 4u ? dpl > rpl : dpl != rpl)) {
            cpu_far_fault(cpu, 13, selector, insn_eip);
            return;
        }
        if (!(d->access & DESC_PRESENT)) {
            cpu_far_fault(cpu, 11, selector, insn_eip);
            return;
        }
        outward = rpl > cpl;
        if (outward) {
            if (!cpu_far_stack_range(cpu, &stack, start, width * 5u, insn_eip) ||
                !cpu_iret_read(cpu, &stack, start + width * 4u, 2, insn_eip, &frame[4])) return;
            stack_selector = (uint16_t)frame[4];
            if (!cpu_far_stack_load(cpu, stack_selector, rpl, false, insn_eip, &new_ss, &outer) ||
                !cpu_iret_read(cpu, &stack, start + width * 3u, width, insn_eip, &frame[3])) return;
            next = cpu_stack_next_esp(&outer, cpu->esp, frame[3]);
        }
        if (!cpu_near_target_valid(cpu, target, dpmi_desc_get_limit(d), insn_eip) ||
            !cpu_far_accessed(cpu, &code, insn_eip)) return;
        if (outward && !cpu_far_accessed(cpu, &new_ss, insn_eip)) return;
    } else if (!cpu_near_target_valid(cpu, target,
                   dpmi_desc_get_limit(&cpu->cs_cache.descriptor), insn_eip)) return;

    uint32_t mask = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF |
                    FLAG_TF | FLAG_DF | FLAG_NT | FLAG_RF | FLAG_AC | FLAG_ID;
    if (!pm || cpl <= iopl) mask |= FLAG_IF;
    if ((!pm && !v86) || (pm && !cpl)) mask |= FLAG_IOPL_MASK;
    if (pm && !cpl) mask |= FLAG_VIF | FLAG_VIP;
    if (width == 2) mask &= 0xFFFFu;
    /* Permissions use the old CPL/IOPL, including on an outward return.
     * No architectural registers change before the last faultable access. */
    cpu->eflags = (((old_flags & ~mask) | (frame[2] & mask)) & 0x003F7FD5u) | FLAGS_FIXED;
    cpu->esp = next;
    if (outward) {
        cpu8086_cache_segment(cpu, 2, stack_selector, &new_ss.descriptor);
        for (unsigned s = 0; s < 6; s++) {
            if (s == 1 || s == 2) continue;
            uint8_t access = cpu->segment_cache[s].descriptor.access;
            bool conforming = (access & (DESC_SEGMENT | DESC_CODE | 4u)) ==
                                         (DESC_SEGMENT | DESC_CODE | 4u);
            if (!cpu->segment_cache[s].valid ||
                (!conforming && (selector & 3u) > ((access & DESC_DPL_MASK) >> 5)))
                cpu8086_cache_segment(cpu, s, 0, NULL);
        }
    }
    cpu8086_restore_virtual_flags(cpu);
    cpu_far_commit(cpu, selector, target, pm ? &code : NULL);
}

static bool cpu_idt_gate_enter(cpu8086_state_t *cpu, uint8_t vector,
                                cpu_event_kind_t kind, uint32_t return_eip,
                                uint32_t fault_eip, uint32_t error, bool has_error)
{
    bool v86 = (cpu->eflags & FLAG_VM) != 0;
    unsigned cpl = cpu8086_cpl(cpu);
    bool software = kind == CPU_EVENT_SOFTWARE || kind == CPU_EVENT_SOFTWARE_EXCEPTION;
    if (v86 && kind == CPU_EVENT_SOFTWARE && (cpu->eflags & FLAG_IOPL_MASK) != FLAG_IOPL_MASK)
        return cpu_far_fault(cpu, 13, 0, fault_eip);

    uint32_t index = (uint32_t)vector * 8u;
    if (index + 7u > cpu->idtr.limit) {
        (void)cpu_deliver_exception(cpu->vm, 13, fault_eip, index | 2u, true);
        return false;
    }
    uint32_t addresses[8];
    uint8_t gate[8];
    if (!cpu_memory_addresses_at_cpl(cpu, cpu->idtr.base + index, 8, false, false,
                                      0, fault_eip, addresses)) return false;
    for (unsigned i = 0; i < 8; i++) gate[i] = dos_mem_read8(cpu->vm, addresses[i]);
    unsigned type = gate[5] & 31u;
    if ((type != 5 && type != 6 && type != 7 && type != 14 && type != 15) ||
        (software && ((gate[5] >> 5) & 3u) < cpl)) {
        (void)cpu_deliver_exception(cpu->vm, 13, fault_eip, index | 2u, true);
        return false;
    }
    if (!(gate[5] & DESC_PRESENT)) {
        (void)cpu_deliver_exception(cpu->vm, 11, fault_eip, index | 2u, true);
        return false;
    }
    uint16_t selector = gate[2] | ((uint16_t)gate[3] << 8);
    dpmi_descriptor_ref_t code, new_ss;
    if (type == 5) {
        uint32_t flags = cpu8086_flags_image(cpu);
        if (kind == CPU_EVENT_EXCEPTION && vector != 1 && vector != 3 && vector != 4 &&
            vector != 8 && vector != 18) flags |= FLAG_RF;
        return cpu_task_switch(cpu, selector, CPU_TASK_CALL, return_eip, fault_eip,
                                 flags, has_error, error);
    }
    if (!cpu_far_descriptor(cpu, selector, 13, fault_eip, &code)) return false;
    const dpmi_descriptor_t *d = &code.descriptor;
    unsigned dpl = (d->access & DESC_DPL_MASK) >> 5;
    if ((d->access & (DESC_SEGMENT | DESC_CODE)) != (DESC_SEGMENT | DESC_CODE) || dpl > cpl)
        return cpu_far_fault(cpu, 13, selector, fault_eip);
    if (!(d->access & DESC_PRESENT)) return cpu_far_fault(cpu, 11, selector, fault_eip);
    bool inward = !(d->access & 4u) && dpl < cpl;
    unsigned destination_cpl = inward ? dpl : cpl;
    unsigned width = type & 8u ? 4u : 2u;
    uint32_t target = gate[0] | ((uint32_t)gate[1] << 8);
    if (width == 4) target |= ((uint32_t)gate[6] << 16) | ((uint32_t)gate[7] << 24);
    cpu_stack_access_t stack;
    uint16_t stack_selector = cpu->ss;
    uint32_t pointer = cpu->esp;
    if (inward) {
        if (!cpu_far_tss_stack(cpu, destination_cpl, fault_eip, &stack_selector, &pointer)) return false;
        if (v86 && destination_cpl) return cpu_far_fault(cpu, 13, selector, fault_eip);
        if (!cpu_far_stack_load(cpu, stack_selector, destination_cpl, true, fault_eip,
                                 &new_ss, &stack)) return false;
    } else {
        if (v86) return cpu_far_fault(cpu, 13, selector, fault_eip);
        if (!cpu_stack_begin(cpu, &stack, false, fault_eip)) return false;
    }
    if (!cpu_near_target_valid(cpu, target, dpmi_desc_get_limit(d), fault_eip)) return false;

    uint32_t flags = cpu8086_flags_image(cpu);
    if (kind == CPU_EVENT_EXCEPTION && vector != 1 && vector != 3 && vector != 4 &&
        vector != 8 && vector != 18) flags |= FLAG_RF;
    uint32_t values[10];
    unsigned count = 0;
    if (v86) {
        values[count++] = cpu->gs; values[count++] = cpu->fs;
        values[count++] = cpu->ds; values[count++] = cpu->es;
    }
    if (inward) { values[count++] = cpu->ss; values[count++] = cpu->esp; }
    values[count++] = flags;
    values[count++] = cpu->cs;
    values[count++] = return_eip;
    if (has_error) values[count++] = error;

    /* Retain the loaded old SS or the validated new SS. Frame writes may
     * alias descriptor tables, so neither cache is reread after the stores. */
    modrm_t slots[10];
    for (unsigned i = 0; i < count; i++) {
        uint32_t offset = (pointer - (i + 1u) * width) & stack.mask;
        if (!cpu_stack_slot(cpu, &stack, offset, width, true, true, fault_eip, &slots[i])) return false;
    }
    if (!cpu_far_accessed(cpu, &code, fault_eip) ||
        (inward && !cpu_far_accessed(cpu, &new_ss, fault_eip))) return false;
    for (unsigned i = 0; i < count; i++) cpu_modrm_write(cpu, &slots[i], width, values[i]);
    cpu->esp = cpu_stack_next_esp(&stack, cpu->esp, pointer - count * width);
    cpu->eflags &= ~(FLAG_TF | FLAG_NT | FLAG_VM | FLAG_RF);
    if (!(type & 1u)) cpu->eflags &= ~FLAG_IF;
    cpu->protected_mode = true;
    if (inward) cpu8086_cache_segment(cpu, 2, stack_selector, &new_ss.descriptor);
    if (v86) for (unsigned s = 0; s < 6; s++) if (s != 1 && s != 2)
        cpu8086_cache_segment(cpu, s, 0, NULL);
    cpu8086_cache_cs(cpu, (selector & ~3u) | destination_cpl, &code.descriptor, destination_cpl);
    cpu->eip = target;
    return true;
}

static bool cpu_exception_contributory(unsigned vector)
{
    return vector == 0 || (vector >= 10 && vector <= 13);
}

static bool cpu_real_interrupt_enter(cpu8086_state_t *cpu, uint8_t vector,
                                      cpu_event_kind_t kind, uint32_t return_eip,
                                      uint32_t fault_eip, bool *host_service)
{
    bool private_call = kind == CPU_EVENT_SOFTWARE && dos_rm_private_interrupt(cpu->vm, vector);
    /* DPMI owns reflected timer, Ctrl+C and critical-error events.
     * This host contract is separate from raw CPU interrupt delivery. */
    bool reflected = kind == CPU_EVENT_SOFTWARE && dpmi_owns_real_interrupt(cpu->vm, vector);
    bool host_owned = private_call || reflected;
    uint32_t index = (uint32_t)vector * 4u, addresses[4] = {0};
    if (!host_owned) {
        if (index + 3u > cpu->idtr.limit) return cpu_far_fault(cpu, 13, 0, fault_eip);
        if (!cpu_memory_addresses_at_cpl(cpu, cpu->idtr.base + index, 4, false, false,
                                          0, fault_eip, addresses)) return false;
    }
    cpu_stack_access_t stack;
    modrm_t slots[3];
    if (!cpu_stack_begin(cpu, &stack, false, fault_eip)) return false;
    uint32_t values[] = { cpu8086_flags_image(cpu), cpu->cs, return_eip };
    for (unsigned i = 0; i < 3; i++)
        if (!cpu_stack_slot(cpu, &stack, (cpu->esp - (i + 1u) * 2u) & stack.mask,
                               2, true, true, fault_eip, &slots[i])) return false;

    uint16_t target = 0, segment = 0;
    if (!host_owned) {
        uint8_t entry[4];
        /* Real interrupt entry reads the vector after pushing its frame.
         * Resolve physical aliases before committing any guest stack bytes. */
        for (unsigned byte = 0; byte < 4; byte++) {
            entry[byte] = dos_mem_read8(cpu->vm, addresses[byte]);
            for (unsigned field = 0; field < 3; field++)
                for (unsigned part = 0; part < 2; part++)
                    if (addresses[byte] == slots[field].physical[part])
                        entry[byte] = (uint8_t)(values[field] >> (part * 8u));
        }
        target = entry[0] | ((uint16_t)entry[1] << 8);
        segment = entry[2] | ((uint16_t)entry[3] << 8);
        if (!cpu_near_target_valid(cpu, target, dpmi_desc_get_limit(&cpu->cs_cache.descriptor),
                                     fault_eip)) return false;
        /* Only the installed default exception policy is host-owned, not
         * arbitrary vectors in ROM or a guest's valid 0000:0000 handler. */
        if (kind != CPU_EVENT_SOFTWARE && kind != CPU_EVENT_EXTERNAL &&
            !target && segment == (DOS_ROM_BASE >> 4) &&
            dos_mem_read8(cpu->vm, DOS_ROM_BASE) == 0xCF) {
            serial_puts("[DOS] Unhandled real-mode exception 0x"); serial_puthex(vector, 2);
            serial_puts("\n");
            cpu->eip = fault_eip;
            cpu->running = false;
            cpu->exit_code = -1;
            return false;
        }
    }
    *host_service = host_owned ||
        (kind == CPU_EVENT_SOFTWARE && !cpu->op_size_32 &&
         dos_rm_host_vector(cpu->vm, vector, segment, target));
    if (*host_service && !host_owned) {
        uint32_t stub = ((uint32_t)segment << 4) + target;
        for (unsigned field = 0; field < 3; field++)
            for (unsigned byte = 0; byte < 2; byte++)
                if (slots[field].physical[byte] - stub < 3u) *host_service = false;
    }
    for (unsigned i = 0; i < 3; i++) cpu_modrm_write(cpu, &slots[i], 2, values[i]);
    cpu->esp = cpu_stack_next_esp(&stack, cpu->esp, cpu->esp - 6u);
    cpu->eflags &= ~(FLAG_IF | FLAG_TF | FLAG_AC | FLAG_RF);
    if (*host_service) cpu->eip = return_eip;
    else {
        cpu8086_load_real_cs(cpu, segment);
        cpu->eip = target;
    }
    return true;
}

void cpu8086_real_host_interrupt(cpu8086_state_t *cpu, uint8_t vector, uint32_t flags)
{
    dos_vm_t *vm = cpu->vm;
    uint16_t cs = cpu->cs;
    uint32_t eip = cpu->eip;
    uint8_t previous_bytes = vm->software_int_frame_bytes;
    uint32_t previous_flags = vm->software_int_return_flags;
    vm->software_int_frame_bytes = 6;
    vm->software_int_return_flags = flags;
    dos_int_dispatch(vm, vector);
    bool frame_consumed = !vm->software_int_frame_bytes;
    vm->software_int_frame_bytes = previous_bytes;
    vm->software_int_return_flags = previous_flags;
    if (cpu->cs == cs && cpu->eip == eip && !frame_consumed) {
        cpu_stack_adjust(cpu, 6);
        const uint32_t status = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
        cpu->eflags = (flags & ~(status | FLAG_AC | FLAG_RF)) |
                      (cpu->eflags & status) | FLAGS_FIXED;
        if (cpu->protected_mode && vm->dpmi.active) cpu->flags |= FLAG_IF;
    } else if (cpu->cs != cs || cpu->eip != eip) cpu_commit_cs_load(cpu);
}

void cpu8086_rm_service(dos_vm_t *vm, uint8_t service)
{
    cpu8086_state_t *cpu = vm->cpu;
    cpu_stack_access_t stack;
    modrm_t slot;
    cpu_event_fault_t fault = {0};
    if (cpu->delivery_fault) return;
    /* This is the ROM bridge's ABI buffer, not another CPU interrupt.
     * Reject a malformed caller frame before invoking a service. */
    cpu->delivery_fault = &fault;
    bool valid = cpu_stack_begin(cpu, &stack, false, cpu->eip);
    if (valid) valid = cpu_stack_slot(cpu, &stack,
        (cpu->esp + vm->software_int_frame_bytes + 4u) & stack.mask,
        2, true, true, cpu->eip, &slot);
    cpu->delivery_fault = NULL;
    if (valid) for (unsigned i = 0; i < 2; i++)
        if (slot.physical[i] >= vm->total_mem_size) valid = false;
    if (!valid) {
        serial_puts("[DOS] Invalid ROM service return frame\n");
        cpu->running = false;
        cpu->exit_code = -1;
        return;
    }
    uint16_t saved = (uint16_t)cpu_modrm_read(cpu, &slot, 2);
    uint32_t previous = vm->software_int_return_flags;
    vm->software_int_return_flags = saved;
    dos_int_dispatch(vm, service);
    vm->software_int_return_flags = previous;
    /* The outer IRET belongs to PUSHF/CALL FAR or DPMI 0302h. */
    const uint16_t status = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
    cpu_modrm_write(cpu, &slot, 2, (saved & ~status) | (cpu->flags & status) | FLAGS_FIXED);
}

bool cpu8086_deliver_guest_interrupt(dos_vm_t *vm, uint8_t vector,
                                      cpu_event_kind_t kind, uint32_t return_eip,
                                      uint32_t fault_eip, uint32_t error, bool has_error)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || (cpu->protected_mode && !cpu8086_uses_guest_idt(cpu)) || cpu->delivery_fault)
        return false;
    cpu->irq_shadow = 0;
    cpu->rep_compare.active = false;
    cpu->halted = false;
    for (;;) {
        cpu_event_fault_t fault = {0};
        bool host_service = false;
        uint32_t flags = cpu8086_flags_image(cpu);
        cpu->delivery_fault = &fault;
        bool delivered = cpu8086_uses_guest_idt(cpu)
            ? cpu_idt_gate_enter(cpu, vector, kind, return_eip, fault_eip, error, has_error)
            : cpu_real_interrupt_enter(cpu, vector, kind, return_eip, fault_eip, &host_service);
        cpu->delivery_fault = NULL;
        /* Host services can reenter the interpreter (callbacks/reflection).
         * No instruction-local fault recorder may escape into that work. */
        if (delivered && host_service) cpu8086_real_host_interrupt(cpu, vector, flags);
        if (delivered && fault.task_trap) {
            /* The original event finished, including any error-code push.
             * TSS.T starts a separate #DB even if that event was #DF. */
            vector = 1;
            kind = CPU_EVENT_PRIVILEGED_TRAP;
            return_eip = fault_eip = fault.return_eip;
            error = 0;
            has_error = false;
            continue;
        }
        if (delivered) return true;
        if (!cpu->running) return false;
        if (!fault.raised) {
            serial_puts("[DOS-CPU] IDT delivery failed without a processor fault\n");
            cpu->running = false;
            cpu->exit_code = -1;
            return false;
        }
        /* A TSS may already have committed a new CS:EIP and address space. */
        fault_eip = fault.return_eip;
        if (kind == CPU_EVENT_EXCEPTION && vector == 8) {
            serial_puts("[DOS-CPU] triple fault delivering guest #DF; stopping client\n");
            cpu->eip = fault_eip;
            cpu->running = false;
            cpu->exit_code = -1;
            return false;
        }
        /* EXT describes an external event or an exception being delivered,
         * not a software INT. Page-fault P/W/U bits must remain untouched. */
        bool external = kind != CPU_EVENT_SOFTWARE && kind != CPU_EVENT_SOFTWARE_EXCEPTION;
        if (fault.vector >= 10 && fault.vector <= 13)
            fault.error = (fault.error & ~1u) | (unsigned)external;
        bool double_fault = kind == CPU_EVENT_EXCEPTION &&
            ((cpu_exception_contributory(vector) && cpu_exception_contributory(fault.vector)) ||
             (vector == 14 && (fault.vector == 14 || cpu_exception_contributory(fault.vector))));
        vector = double_fault ? 8u : fault.vector;
        error = double_fault ? 0 : fault.error;
        has_error = double_fault || fault.has_error;
        kind = CPU_EVENT_EXCEPTION;
        return_eip = fault_eip;
    }
}

static void cpu_stack_all(cpu8086_state_t *cpu, bool pop, unsigned width,
                           bool locked, uint32_t insn_eip)
{
    cpu_stack_access_t stack;
    modrm_t slots[8];
    uint32_t values[8];
    if (!cpu_stack_begin(cpu, &stack, locked, insn_eip)) return;
    for (unsigned i = 0; i < 8; i++) {
        uint32_t offset = (pop ? cpu->esp + i * width
                               : cpu->esp - (i + 1u) * width) & stack.mask;
        if (!cpu_stack_slot(cpu, &stack, offset, width, !pop, !pop,
                             insn_eip, &slots[i])) return;
        if (!pop) values[i] = *reg32_ptr(cpu, i);
    }
    for (unsigned i = 0; i < 8; i++) {
        if (pop) values[i] = cpu_modrm_read(cpu, &slots[i], width);
        else cpu_modrm_write(cpu, &slots[i], width, values[i]);
    }
    cpu->esp = cpu_stack_next_esp(&stack, cpu->esp,
                                  pop ? cpu->esp + 8u * width : cpu->esp - 8u * width);
    if (pop) for (unsigned i = 0; i < 8; i++) {
        if (i == 3) continue; /* The saved SP/ESP is consumed, never loaded. */
        if (width == 4) *reg32_ptr(cpu, 7u - i) = values[i];
        else *reg16_ptr(cpu, 7u - i) = (uint16_t)values[i];
    }
}

static void cpu_pop_segment(cpu8086_state_t *cpu, unsigned segment, unsigned width,
                             bool locked, uint32_t insn_eip)
{
    cpu_stack_access_t stack;
    modrm_t source;
    if (!cpu_stack_begin(cpu, &stack, locked, insn_eip) ||
        !cpu_stack_slot(cpu, &stack, cpu->esp & stack.mask, width, false, false,
                         insn_eip, &source)) return;
    uint16_t selector = (uint16_t)cpu_modrm_read(cpu, &source, width);
    cpu_system_segment_t loaded;
    if (!cpu_prepare_segment_load(cpu, segment, selector, insn_eip, &loaded)) return;
    /* POP SS increments with the old SS.B, even when the new width differs. */
    cpu->esp = cpu_stack_next_esp(&stack, cpu->esp, cpu->esp + width);
    cpu_commit_segment_load(cpu, segment, selector, &loaded);
    if (segment == 2 && !cpu->irq_shadow) cpu->irq_shadow = 2;
}

static void cpu_stack_enter(cpu8086_state_t *cpu, unsigned width, unsigned alloc,
                             unsigned nesting, bool locked, uint32_t insn_eip)
{
    cpu_stack_access_t stack;
    modrm_t destinations[32], sources[30], final;
    if (!cpu_stack_begin(cpu, &stack, locked, insn_eip)) return;
    uint32_t first = (cpu->esp - width) & stack.mask;
    uint32_t frame = cpu_stack_next_esp(&stack, cpu->esp, first);
    unsigned count = nesting ? nesting + 1u : 1u;
    for (unsigned i = 0; i < count; i++) {
        if (i && i < nesting &&
            !cpu_stack_slot(cpu, &stack, (cpu->ebp - i * width) & stack.mask,
                             width, false, false, insn_eip, &sources[i - 1u])) return;
        if (!cpu_stack_slot(cpu, &stack, (first - i * width) & stack.mask,
                             width, true, true, insn_eip, &destinations[i])) return;
    }
    uint32_t next = cpu_stack_next_esp(&stack, cpu->esp, cpu->esp - count * width - alloc);
    /* ENTER probes the final byte for write permission without writing it. */
    if (!cpu_stack_slot(cpu, &stack, next & stack.mask, 1, true, false,
                         insn_eip, &final)) return;
    cpu_modrm_write(cpu, &destinations[0], width, cpu->ebp);
    for (unsigned i = 1; i < nesting; i++) {
        uint32_t value = cpu_modrm_read(cpu, &sources[i - 1u], width);
        cpu_modrm_write(cpu, &destinations[i], width, value);
    }
    if (nesting) cpu_modrm_write(cpu, &destinations[nesting], width, frame);
    cpu->esp = next;
    if (width == 4) cpu->ebp = frame;
    else cpu->bp = (uint16_t)frame;
}

static void cpu_stack_leave(cpu8086_state_t *cpu, unsigned width,
                             bool locked, uint32_t insn_eip)
{
    cpu_stack_access_t stack;
    modrm_t slot;
    if (!cpu_stack_begin(cpu, &stack, locked, insn_eip)) return;
    uint32_t source = cpu->ebp & stack.mask;
    if (!cpu_stack_slot(cpu, &stack, source, width, false, false,
                         insn_eip, &slot)) return;
    uint32_t value = cpu_modrm_read(cpu, &slot, width);
    cpu->esp = cpu_stack_next_esp(&stack, cpu->esp, source + width);
    if (width == 4) cpu->ebp = value;
    else cpu->bp = (uint16_t)value;
}

#define CPU_STACK_PUSH(value) do { \
    if (!cpu_stack_push_value(cpu, (value), op32 ? 4u : 2u, lock_prefix, insn_eip)) \
        goto instruction_complete; \
} while (0)
#define CPU_STACK_POP() ({ \
    uint32_t stack_value; \
    if (!cpu_stack_pop_value(cpu, &stack_value, op32 ? 4u : 2u, lock_prefix, insn_eip)) \
        goto instruction_complete; \
    stack_value; \
})

/* ── String operation helpers (16/32-bit addressing) ────────────── */

static inline void str_adv_si(cpu8086_state_t *cpu, int32_t d, bool adr32)
{
    if (adr32) cpu->esi += d; else cpu->si += (uint16_t)d;
}

static inline void str_adv_di(cpu8086_state_t *cpu, int32_t d, bool adr32)
{
    if (adr32) cpu->edi += d; else cpu->di += (uint16_t)d;
}

static inline void str_dec_cx(cpu8086_state_t *cpu, bool adr32)
{
    if (adr32) cpu->ecx--; else cpu->cx--;
}

static inline bool str_cx_nz(cpu8086_state_t *cpu, bool adr32)
{
    return adr32 ? (cpu->ecx != 0) : (cpu->cx != 0);
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

static uint32_t shift_rotate32(cpu8086_state_t *cpu, uint8_t op, uint32_t val, uint8_t count)
{
    count &= 0x1F;
    if (count == 0)
        return val;

    uint32_t result = val;
    uint8_t i;
    bool cf;

    switch (op) {
    case 0: /* ROL */
        for (i = 0; i < count; i++) {
            cf = (result & 0x80000000u) != 0;
            result = (result << 1) | (cf ? 1 : 0);
        }
        set_flag(cpu, FLAG_CF, result & 1);
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ val) & 0x80000000u) != 0);
        break;

    case 1: /* ROR */
        for (i = 0; i < count; i++) {
            cf = (result & 1) != 0;
            result = (result >> 1) | (cf ? 0x80000000u : 0);
        }
        set_flag(cpu, FLAG_CF, (result & 0x80000000u) != 0);
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ (result << 1)) & 0x80000000u) != 0);
        break;

    case 2: /* RCL */
        for (i = 0; i < count; i++) {
            cf = get_flag(cpu, FLAG_CF);
            set_flag(cpu, FLAG_CF, (result & 0x80000000u) != 0);
            result = (result << 1) | (cf ? 1 : 0);
        }
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ val) & 0x80000000u) != 0);
        break;

    case 3: /* RCR */
        for (i = 0; i < count; i++) {
            cf = get_flag(cpu, FLAG_CF);
            set_flag(cpu, FLAG_CF, (result & 1) != 0);
            result = (result >> 1) | (cf ? 0x80000000u : 0);
        }
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ (result << 1)) & 0x80000000u) != 0);
        break;

    case 4: /* SHL / SAL */ {
        uint64_t tmp = (uint64_t)result;
        for (i = 0; i < count; i++) {
            set_flag(cpu, FLAG_CF, (tmp & 0x80000000u) != 0);
            tmp <<= 1;
        }
        result = (uint32_t)tmp;
        update_flags_logic32(cpu, result);
        set_flag(cpu, FLAG_CF, (count <= 32) ? ((val >> (32 - count)) & 1) : 0);
        if (count == 1)
            set_flag(cpu, FLAG_OF, ((result ^ val) & 0x80000000u) != 0);
        break;
    }

    case 5: /* SHR */ {
        uint64_t tmp = (uint64_t)result;
        if (count == 1)
            set_flag(cpu, FLAG_OF, (tmp & 0x80000000u) != 0);
        for (i = 0; i < count; i++) {
            set_flag(cpu, FLAG_CF, (tmp & 1) != 0);
            tmp >>= 1;
        }
        result = (uint32_t)tmp;
        update_flags_logic32(cpu, result);
        set_flag(cpu, FLAG_CF, (count <= 32) ? ((val >> (count - 1)) & 1) : 0);
        break;
    }

    case 7: /* SAR */ {
        int32_t stmp = (int32_t)result;
        if (count == 1)
            set_flag(cpu, FLAG_OF, false);
        for (i = 0; i < count; i++) {
            set_flag(cpu, FLAG_CF, (stmp & 1) != 0);
            stmp >>= 1;
        }
        result = (uint32_t)stmp;
        update_flags_logic32(cpu, result);
        set_flag(cpu, FLAG_CF, ((int32_t)val >> (count - 1)) & 1);
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

/* Each dispatch has bounded work; CX/ECX always retains the guest count. */
#define CPU_STRING_ITERATIONS 256u

static bool cpu_string_memory(cpu8086_state_t *cpu, uint8_t opcode,
                               unsigned width, bool adr32, uint32_t insn_eip)
{
    unsigned operation = opcode & ~1u;
    bool source = operation == 0xA4u || operation == 0xA6u || operation == 0xACu;
    bool destination = operation != 0xACu;
    bool write = operation == 0xA4u || operation == 0xAAu;
    bool compare = operation == 0xA6u || operation == 0xAEu;
    uint32_t count = cpu->rep_active ? (adr32 ? cpu->ecx : cpu->cx) : 1u;
    unsigned iterations = count > CPU_STRING_ITERATIONS ? CPU_STRING_ITERATIONS : count;
    if (cpu->irq_shadow && iterations > 1u) iterations = 1;
    int32_t delta = cpu->eflags & FLAG_DF ? -(int32_t)width : (int32_t)width;
    uint32_t next_eip = cpu->eip;
    uint32_t original_flags = cpu->rep_compare.active ? cpu->rep_compare.eflags : cpu->eflags;
    cpu->rep_compare.active = false;

    for (unsigned i = 0; i < iterations; i++) {
        modrm_t from = { .seg_index = cpu->seg_override >= 0 ? cpu->seg_override : 3,
                         .offset = adr32 ? cpu->esi : cpu->si };
        modrm_t to = { .seg_index = 0, .offset = adr32 ? cpu->edi : cpu->di };
        uint32_t iteration_flags = cpu->eflags;
        /* A repeated comparison faults with its pre-instruction FLAGS, even
         * after completed iterations. Prepare can deliver the exception. */
        if (compare && cpu->rep_active) cpu->eflags = original_flags;
        if ((source && !cpu_modrm_prepare(cpu, &from, width, false, insn_eip)) ||
            (destination && !cpu_modrm_prepare(cpu, &to, width, write, insn_eip)))
            return false;
        cpu->eflags = iteration_flags;

        /* Validate both operands before a read can alter an MMIO latch. */
        uint32_t value = source ? cpu_modrm_read(cpu, &from, width) : cpu->eax;
        if (write) cpu_modrm_write(cpu, &to, width, value);
        else if (compare) {
            uint32_t other = cpu_modrm_read(cpu, &to, width);
            if (width == 1u) alu_cmp8(cpu, (uint8_t)value, (uint8_t)other);
            else if (width == 2u) alu_cmp16(cpu, (uint16_t)value, (uint16_t)other);
            else alu_cmp32(cpu, value, other);
        } else if (width == 1u) cpu->al = value;
        else if (width == 2u) cpu->ax = value;
        else cpu->eax = value;

        if (source) str_adv_si(cpu, delta, adr32);
        if (destination) str_adv_di(cpu, delta, adr32);
        if (cpu->rep_active) {
            str_dec_cx(cpu, adr32);
            if (compare && (!!(cpu->eflags & FLAG_ZF) != (cpu->rep_type == 1)))
                return false;
        }
    }
    if (!cpu->rep_active || !str_cx_nz(cpu, adr32)) return false;
    cpu->eip = insn_eip;
    if (compare) {
        cpu->rep_compare.eip = insn_eip;
        cpu->rep_compare.next_eip = next_eip;
        cpu->rep_compare.eflags = original_flags;
        cpu->rep_compare.cs = cpu->cs;
        cpu->rep_compare.opcode = opcode;
        cpu->rep_compare.width = width;
        cpu->rep_compare.rep_type = cpu->rep_type;
        cpu->rep_compare.seg_override = cpu->seg_override;
        cpu->rep_compare.adr32 = adr32;
        cpu->rep_compare.active = true;
    }
    return true;
}

static bool cpu_string_port_io(cpu8086_state_t *cpu, bool input,
                                unsigned width, bool adr32, uint32_t insn_eip)
{
    dos_vm_t *vm = cpu->vm;
    unsigned segment = input ? 0u : cpu->seg_override >= 0
                                      ? (unsigned)cpu->seg_override : 3u;
    uint32_t count = cpu->rep_active ? (adr32 ? cpu->ecx : cpu->cx) : 1u;
    int32_t delta = cpu->eflags & FLAG_DF ? -(int32_t)width : (int32_t)width;
    unsigned iterations = count > CPU_STRING_ITERATIONS ? CPU_STRING_ITERATIONS : count;
    if (cpu->irq_shadow && iterations > 1u) iterations = 1;
    for (unsigned i = 0; i < iterations; i++) {
        uint32_t offset = input ? (adr32 ? cpu->edi : cpu->di)
                                : (adr32 ? cpu->esi : cpu->si);
        uint32_t addresses[4], value = 0;
        if (!cpu_operand_addresses(cpu, segment, offset, width, input,
                                    insn_eip, addresses)) return false;
        if (input) {
            value = width == 1 ? dos_io_read8(vm, cpu->dx) :
                    width == 2 ? dos_io_read16(vm, cpu->dx) : dos_io_read32(vm, cpu->dx);
            for (unsigned b = 0; b < width; b++)
                dos_mem_write8(vm, addresses[b], (uint8_t)(value >> (b * 8u)));
            str_adv_di(cpu, delta, adr32);
        } else {
            for (unsigned b = 0; b < width; b++)
                value |= (uint32_t)dos_mem_read8(vm, addresses[b]) << (b * 8u);
            if (width == 1) dos_io_write8(vm, cpu->dx, (uint8_t)value);
            else if (width == 2) dos_io_write16(vm, cpu->dx, (uint16_t)value);
            else dos_io_write32(vm, cpu->dx, value);
            str_adv_si(cpu, delta, adr32);
        }
        /* Keep the real remaining count visible to an exception handler;
         * only completed transfers advance the index and consume REP count. */
        if (cpu->rep_active) str_dec_cx(cpu, adr32);
    }
    if (!cpu->rep_active || !str_cx_nz(cpu, adr32)) return false;
    cpu->eip = insn_eip;
    return true;
}

bool cpu8086_service_interrupts(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint64_t now = idt_get_ticks();
    uint64_t elapsed = now - vm->start_ticks;
    uint32_t ticks = (uint32_t)((elapsed * 182u) / 1000u);
    bool bios_tick_changed = ticks != vm->bios_ticks;

    if (bios_tick_changed) {
        vm->bios_ticks = ticks;
        vm->last_timer_tick = now;
        dos_mem_write32(vm, 0x46C, ticks);
    }
    if (dos_io_timer_poll(vm) || (!vm->io && bios_tick_changed))
        vm->timer_irq_pending = true;
    bool keyboard_pending = dos_io_keyboard_poll(vm);

    bool interrupts_enabled = cpu->protected_mode && !cpu->guest_idt && vm->dpmi.active
                            ? vm->dpmi.virtual_interrupts_enabled
                            : (cpu->flags & FLAG_IF) != 0;
    if (!interrupts_enabled || cpu->irq_shadow)
        return false;

    uint8_t vector = 0;
    if (vm->timer_irq_pending && dos_io_irq_begin(vm, 0, &vector) &&
        cpu_deliver_hw_interrupt(vm, vector)) {
        vm->timer_irq_pending = false;
        return true;
    }
    if (keyboard_pending && dos_io_irq_begin(vm, 1, &vector) &&
        cpu_deliver_hw_interrupt(vm, vector))
        return true;

    uint8_t audio_irq = 0;
    uint32_t audio_pending = 0;
    if (dos_audio_take_irq(vm, &audio_irq, &audio_pending)) {
        uint8_t audio_vector = 0;
        if (dos_io_irq_begin(vm, audio_irq, &audio_vector) &&
            cpu_deliver_hw_interrupt(vm, audio_vector))
            return true;
        dos_audio_restore_irq(vm, audio_pending);
    }

    return false;
}

static void cpu_wait_halted(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    while (cpu->running && cpu->halted) {
        if (cpu8086_service_interrupts(vm))
            break;
        if (sched_sleep_ticks(1) < 0)
            sched_yield();
    }
}

/* ── Main execution loop ─────────────────────────────────────────── */

static void cpu_stop_for_step_limit(dos_vm_t *vm)
{
    if (!vm->step_limit_reached) {
        serial_puts("[DOS] Interpreter step limit reached: ");
        serial_putdec(vm->step_count);
        serial_puts("\n");
    }
    vm->step_limit_reached = true;
    vm->cpu->running = false;
    vm->cpu->exit_code = -1;
}

static int cpu8086_run_internal(dos_vm_t *vm, bool single_step)
{
    cpu8086_state_t *cpu;
    uint8_t opcode;
    uint8_t modrm_byte;
    modrm_t m;

    jit_state_t *jit;
    uint16_t prev_cs;

    /* Native DPMI clients return through INT FC. Resume here, above the
     * opcode handlers that initiated the native transition, so no stale
     * handler-local state can overwrite the restored real-mode CS:IP. */
    int native_resume = 0;
    if (!single_step) {
        vm->native_resume_armed = true;
        native_resume = kern_setjmp(vm->native_resume_jmpbuf);
    }

    cpu = vm->cpu;
    jit = single_step ? NULL : (jit_state_t *)vm->jit;
    prev_cs = cpu->cs;

    if (native_resume != 0) {
        __asm__ volatile ("sti" ::: "memory");
        serial_puts("[DOS-NT] resumed interpreter at ");
        serial_puthex(cpu->cs, 4);
        serial_puts(":");
        serial_puthex(cpu->eip, 8);
        serial_puts(" ss:sp=");
        serial_puthex(cpu->ss, 4);
        serial_puts(":");
        serial_puthex(cpu->esp, 8);
        serial_puts("\n");
    }

    for (;;) {
        if (vm->step_limit_reached) {
            cpu_stop_for_step_limit(vm);
            break;
        }
        if (single_step && (!cpu->running || cpu->halted))
            break;
        if (!cpu->running) {
            /* Let the suspended DOS service unwind before its process is
             * reaped. Children started inside the callback still run normally. */
            if (vm->interpreter_stop_active &&
                vm->current_psp == vm->interpreter_stop_psp)
                break;
            if (!dos_exec_complete_termination(vm)) break;
            cpu = vm->cpu;
            jit = (jit_state_t *)vm->jit;
            prev_cs = cpu->cs;
        }

        if (!single_step && dos_exec_activate_loaded_child(vm)) {
            cpu = vm->cpu;
            jit = (jit_state_t *)vm->jit;
            prev_cs = cpu->cs;
        }

        if (!single_step && vm->interpreter_stop_active &&
            vm->current_psp == vm->interpreter_stop_psp &&
            (vm->interpreter_stop_signal ? *vm->interpreter_stop_signal :
             (cpu->protected_mode == vm->interpreter_stop_protected &&
              cpu->cs == vm->interpreter_stop_cs &&
              cpu->eip == vm->interpreter_stop_ip))) {
            vm->interpreter_stop_reached = true;
            break;
        }

        if (vm->step_limit && vm->step_count >= vm->step_limit) {
            cpu_stop_for_step_limit(vm);
            break;
        }

        if (cpu->halted) {
            cpu_wait_halted(vm);
            if (!cpu->running) continue;
        }

        uint32_t insn_eip = cpu->eip;
        uint64_t previous_insns = cpu->insn_count;
        bool had_irq_shadow = cpu->irq_shadow != 0;
        bool string_pending = false;
        if (cpu->rep_compare.active &&
            (cpu->cs != cpu->rep_compare.cs || insn_eip != cpu->rep_compare.eip))
            cpu->rep_compare.active = false;

        /* ── Hybrid dispatcher: JIT cache → compile if hot → interpret ── */
        if (jit && !cpu->irq_shadow && (!cpu->protected_mode || vm->emulate_cpu) &&
            !vm->step_limit && !(cpu->eflags & FLAG_TF)) {
            uint32_t ip = cpu->protected_mode || cpu->op_size_32 ? cpu->eip : cpu->ip;
            uint16_t hot = (uint16_t)(ip ^ (ip >> 16));
            if (jit->hit_count[hot] != 0xFFFFu)
                jit->hit_count[hot]++;

            if (jit->hit_count[hot] >= JIT_HOT_THRESHOLD) {
                /* Hot path — look up or compile block */
                jit_block_t *block = jit_get_block(jit, cpu->cs, ip);
                if (block) {
                    if ((!block->compiled && !block->source_valid) ||
                        !jit_block_current(vm, block)) {
                        jit_decode_block(vm, block);
                    }
                    /* Code eviction retains valid IR for this guest context. */
                    if (!block->compiled && block->instruction_count > 0)
                        jit_compile_block(jit, block);
                    if (!block->instruction_count) {
                        /* Unsupported starts need not compete with translated
                         * blocks on every dispatch. Changed code remains correct
                         * in the interpreter while this coarse counter warms. */
                        jit->hit_count[hot] = 0;
                        jit->interpreter_backoffs++;
                    }
                    bool crosses_stop = vm->interpreter_stop_active &&
                        !vm->interpreter_stop_signal &&
                        vm->current_psp == vm->interpreter_stop_psp &&
                        cpu->protected_mode == vm->interpreter_stop_protected &&
                        cpu->cs == vm->interpreter_stop_cs &&
                        vm->interpreter_stop_ip > ip &&
                        (uint64_t)vm->interpreter_stop_ip <=
                            (uint64_t)ip + block->length;
                    if (block->compiled && !crosses_stop) {
                        /* Execute native code */
                        bool completed = jit_exec_block(vm, block);
                        jit->jit_executed++;
                        if (completed) {
                            opcode = 0x90; /* No interpreter opcode this dispatch. */
                            goto execution_complete;
                        }
                        insn_eip = cpu->eip;
                    }
                }
            }
        }
        if (jit) jit->interpreted++;

        /* ── Interpreter: fetch-decode-execute ── */

        /* Reserve this dispatch before an INT can recurse into the same
         * session. REP work is already split into bounded dispatch chunks. */
        if (vm->step_limit) vm->step_count++;

        /* Reset prefix state */
        cpu->seg_override = -1;
        cpu->rep_active   = false;
        cpu->rep_type     = 0;
        cpu->prefix_66    = false;
        cpu->prefix_67    = false;
        bool lock_prefix = false;
        cpu_fetch_state_t fetch_state = {
            .start = (cpu->protected_mode && cpu->pm_cs_loaded) || cpu->op_size_32
                     ? cpu->eip : cpu->ip
        };
        cpu_fetch_state_t *fetch = &fetch_state;
        opcode = 0;

        /* Handle prefixes */
    fetch_prefix:
        opcode = CPU_FETCH8(cpu);

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
        case 0xF0: /* LOCK */
            lock_prefix = true;
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

        unsigned string_width = !(opcode & 1u) ? 1u : op32 ? 4u : 2u;
        if (cpu->rep_compare.active &&
            (lock_prefix || !cpu->rep_active || cpu->rep_compare.opcode != opcode ||
             cpu->rep_compare.width != string_width || cpu->rep_compare.adr32 != adr32 ||
             cpu->rep_compare.rep_type != cpu->rep_type ||
             cpu->rep_compare.seg_override != cpu->seg_override ||
             cpu->rep_compare.next_eip != cpu->eip))
            cpu->rep_compare.active = false;

        if (lock_prefix && ((opcode >= 0x9C && opcode <= 0x9F) ||
                            (opcode >= 0xF8 && opcode <= 0xFD) || opcode == 0xF5)) {
            (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
            goto instruction_complete;
        }

        /* ── Dispatch opcode ─────────────────────────────────────── */

        switch (opcode) {

        /* ════════════════════════════════════════════════════════════
         *  ADD  (0x00 - 0x05)
         * ════════════════════════════════════════════════════════════ */
        case 0x00: { /* ADD r/m8, r8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_add8(cpu, val, reg));
            break;
        }
        case 0x01: { /* ADD r/m16/32, r16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t reg = *reg32_ptr(cpu, m.reg_field);
                modrm_write32(cpu, &m, alu_add32(cpu, val, reg));
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t reg = *reg16_ptr(cpu, m.reg_field);
                modrm_write16(cpu, &m, alu_add16(cpu, val, reg));
            }
            break;
        }
        case 0x02: { /* ADD r8, r/m8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_add8(cpu, *dst, val);
            break;
        }
        case 0x03: { /* ADD r16/32, r/m16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                uint32_t *dst = reg32_ptr(cpu, m.reg_field);
                uint32_t val = modrm_read32(cpu, &m);
                *dst = alu_add32(cpu, *dst, val);
            } else {
                uint16_t *dst = reg16_ptr(cpu, m.reg_field);
                uint16_t val = modrm_read16(cpu, &m);
                *dst = alu_add16(cpu, *dst, val);
            }
            break;
        }
        case 0x04: { /* ADD AL, imm8 */
            uint8_t imm = CPU_FETCH8(cpu);
            cpu->al = alu_add8(cpu, cpu->al, imm);
            break;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x35: case 0x3D: { /* ALU AX/EAX, immediate */
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            unsigned operation = (opcode >> 3) & 7u;
            if (op32) {
                uint32_t imm = CPU_FETCH32(cpu);
                cpu->eax = group1_alu32(cpu, operation, cpu->eax, imm);
            } else {
                uint16_t imm = CPU_FETCH16(cpu);
                cpu->ax = group1_alu16(cpu, operation, cpu->ax, imm);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  PUSH / POP segment registers
         * ════════════════════════════════════════════════════════════ */
        case 0x06: /* PUSH ES */
            CPU_STACK_PUSH(cpu->es);
            break;
        case 0x07: /* POP ES */
            cpu_pop_segment(cpu, 0, op32 ? 4u : 2u, lock_prefix, insn_eip);
            break;

        /* ════════════════════════════════════════════════════════════
         *  OR  (0x08 - 0x0D)
         * ════════════════════════════════════════════════════════════ */
        case 0x08: { /* OR r/m8, r8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_or8(cpu, val, reg));
            break;
        }
        case 0x09: { /* OR r/m16/32, r16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t reg = *reg32_ptr(cpu, m.reg_field);
                modrm_write32(cpu, &m, alu_or32(cpu, val, reg));
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t reg = *reg16_ptr(cpu, m.reg_field);
                modrm_write16(cpu, &m, alu_or16(cpu, val, reg));
            }
            break;
        }
        case 0x0A: { /* OR r8, r/m8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_or8(cpu, *dst, val);
            break;
        }
        case 0x0B: { /* OR r16/32, r/m16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                uint32_t *dst = reg32_ptr(cpu, m.reg_field);
                uint32_t val = modrm_read32(cpu, &m);
                *dst = alu_or32(cpu, *dst, val);
            } else {
                uint16_t *dst = reg16_ptr(cpu, m.reg_field);
                uint16_t val = modrm_read16(cpu, &m);
                *dst = alu_or16(cpu, *dst, val);
            }
            break;
        }
        case 0x0C: { /* OR AL, imm8 */
            uint8_t imm = CPU_FETCH8(cpu);
            cpu->al = alu_or8(cpu, cpu->al, imm);
            break;
        }
        case 0x0E: /* PUSH CS */
            CPU_STACK_PUSH(cpu->cs);
            break;

        /* ════════════════════════════════════════════════════════════
         *  Two-byte escape (0x0F)
         * ════════════════════════════════════════════════════════════ */
        case 0x0F: {
            uint8_t op2 = CPU_FETCH8(cpu);
            switch (op2) {
            case 0x80: case 0x81: case 0x82: case 0x83:
            case 0x84: case 0x85: case 0x86: case 0x87:
            case 0x88: case 0x89: case 0x8A: case 0x8B:
            case 0x8C: case 0x8D: case 0x8E: case 0x8F: { /* Jcc near */
                if (lock_prefix) {
                    (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                    break;
                }
                int32_t rel = op32 ? (int32_t)CPU_FETCH32(cpu)
                                   : (int16_t)CPU_FETCH16(cpu);
                if (eval_condition(cpu, op2 - 0x80))
                    (void)cpu_near_jump(cpu, cpu->eip + (uint32_t)rel, op32,
                                         fetch->limit, insn_eip);
                break;
            }
            /* ── 0F 00: Group 6 (SLDT/STR/LLDT/LTR/VERR/VERW) ─── */
            case 0x00: {
                uint8_t g6_modrm = CPU_FETCH8(cpu);
                unsigned subop = (g6_modrm >> 3) & 7u;
                if (!cpu->protected_mode || (cpu->eflags & FLAG_VM) ||
                    lock_prefix || subop >= 6u) {
                    (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                    break;
                }
                if ((subop == 2u || subop == 3u) &&
                    !cpu_system_privileged(cpu, insn_eip)) {
                    break;
                }
                modrm_t g6m = CPU_DECODE_MODRM(cpu, g6_modrm);
                switch (g6m.reg_field) {
                case 0: /* SLDT */
                case 1: { /* STR: memory is always 16 bits; r32 zero-extends. */
                    uint16_t selector = subop ? cpu->tr : cpu->ldtr;
                    if (g6m.is_reg && op32) modrm_write32(cpu, &g6m, selector);
                    else modrm_write16(cpu, &g6m, selector);
                    break;
                }
                case 2: /* LLDT */
                case 3: { /* LTR */
                    uint16_t selector = modrm_read16(cpu, &g6m);
                    (void)cpu_load_system_segment(cpu, selector, subop == 3u, insn_eip);
                    break;
                }
                case 4: /* VERR */
                case 5: { /* VERW */
                    dos_page_fault_t fault;
                    bool valid = cpu_query_descriptor(cpu,
                        modrm_read16(cpu, &g6m), subop == 4u
                        ? CPU_QUERY_READ : CPU_QUERY_WRITE, NULL, &fault);
                    if (fault.raised) cpu_raise_page_fault(cpu, insn_eip, &fault);
                    else set_flag(cpu, FLAG_ZF, valid);
                    break;
                }
                default:
                    (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                    break;
                }
                break;
            }
            case 0x02: /* LAR */
            case 0x03: { /* LSL */
                if (!cpu->protected_mode || (cpu->eflags & FLAG_VM) || lock_prefix) {
                    (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                    break;
                }
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
                uint32_t value;
                dos_page_fault_t fault;
                bool valid = cpu_query_descriptor(cpu, modrm_read16(cpu, &m),
                    op2 == 0x02 ? CPU_QUERY_LAR : CPU_QUERY_LSL, &value, &fault);
                if (fault.raised) {
                    cpu_raise_page_fault(cpu, insn_eip, &fault);
                    break;
                }
                set_flag(cpu, FLAG_ZF, valid);
                if (valid) {
                    if (op32) *reg32_ptr(cpu, m.reg_field) = value;
                    else *reg16_ptr(cpu, m.reg_field) = (uint16_t)value;
                }
                break;
            }
            /* ── 0F 01: Group 7 (LGDT/SGDT/LIDT/SIDT/LMSW/SMSW) ─── */
            case 0x01: {
                uint8_t g7_modrm = CPU_FETCH8(cpu);
                unsigned subop = (g7_modrm >> 3) & 7u;
                bool register_operand = (g7_modrm & 0xC0u) == 0xC0u;
                if (lock_prefix || subop == 5u ||
                    (register_operand && (subop < 4u || subop == 7u))) {
                    (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                    break;
                }
                if ((subop == 2u || subop == 3u || subop == 6u || subop == 7u) &&
                    !cpu_system_privileged(cpu, insn_eip)) break;
                modrm_t g7m = CPU_DECODE_MODRM(cpu, g7_modrm);
                switch (g7m.reg_field) {
                case 0: case 1: /* SGDT / SIDT */
                case 2: case 3: { /* LGDT / LIDT */
                    bool idt = (g7m.reg_field & 1u) != 0;
                    bool store = g7m.reg_field < 2u;
                    uint16_t limit = idt ? cpu->idtr.limit : cpu->gdtr.limit;
                    uint32_t base = idt ? cpu->idtr.base : cpu->gdtr.base;
                    uint8_t bytes[6] = { limit, limit >> 8, base, base >> 8, base >> 16, base >> 24 };
                    if (!cpu_operand_record(cpu, &g7m, sizeof(bytes), store, insn_eip, bytes))
                        goto instruction_complete;
                    if (store) break;
                    limit = bytes[0] | ((uint16_t)bytes[1] << 8);
                    base = bytes[2] | ((uint32_t)bytes[3] << 8) | ((uint32_t)bytes[4] << 16);
                    if (op32) base |= (uint32_t)bytes[5] << 24;
                    if (idt) { cpu->idtr.base = base; cpu->idtr.limit = limit; }
                    else { cpu->gdtr.base = base; cpu->gdtr.limit = limit; }
                    break;
                }
                case 4: { /* SMSW: store machine status word */
                    uint16_t msw = (uint16_t)cpu->cr0;
                    modrm_write16(cpu, &g7m, msw);
                    break;
                }
                case 6: { /* LMSW: load machine status word */
                    uint16_t msw = modrm_read16(cpu, &g7m);
                    cpu->cr0 = (cpu->cr0 & ~0xEu) | (msw & 0xFu);
                    cpu->protected_mode = (cpu->cr0 & 1u) != 0;
                    break;
                }
                case 7: /* INVLPG names an address but does not read it.
                         * The page walker has no persistent translation cache. */
                    break;
                default:
                    (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                    break;
                }
                break;
            }

            case 0x20: /* MOV r32, CRn */
            case 0x22: { /* MOV CRn, r32: mod and operand size are ignored. */
                uint8_t cr_modrm = CPU_FETCH8(cpu);
                uint8_t cr_num = (cr_modrm >> 3) & 7;
                uint8_t cr_reg = cr_modrm & 7;
                if (lock_prefix || cr_num == 1u || cr_num >= 5u) {
                    (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                    break;
                }
                if (!cpu_system_privileged(cpu, insn_eip)) break;
                uint32_t *reg = reg32_ptr(cpu, cr_reg);
                if (op2 == 0x20) {
                    *reg = cr_num == 0 ? cpu->cr0 : cr_num == 2 ? cpu->cr2 :
                           cr_num == 3 ? cpu->cr3 : 0;
                } else {
                    switch (cr_num) {
                    case 0: (void)cpu_write_cr0(cpu, *reg, insn_eip); break;
                    case 2: cpu->cr2 = *reg; break;
                    case 3: cpu->cr3 = *reg & DOS_CR3_VALID; break;
                    case 4:
                        /* No CR4-controlled extension is implemented or
                         * advertised by this vCPU. Zero is the only state. */
                        if (*reg) (void)cpu_deliver_exception(vm, 13, insn_eip, 0, true);
                        break;
                    }
                }
                break;
            }

            case 0x06: /* CLTS */
            case 0x08: /* INVD */
            case 0x09: /* WBINVD */
                if (lock_prefix) {
                    (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                    break;
                }
                if (!cpu_system_privileged(cpu, insn_eip)) break;
                if (op2 == 0x06) cpu->cr0 &= ~DOS_CR0_TS;
                /* Guest memory is coherent and has no virtual data cache. */
                break;

            case 0x0B: /* UD2 */
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;

            case 0xA0: /* PUSH FS */
                CPU_STACK_PUSH(cpu->fs);
                break;

            case 0xA1: /* POP FS */
                cpu_pop_segment(cpu, 4, op32 ? 4u : 2u, lock_prefix, insn_eip);
                break;

            case 0xA2: /* CPUID */
                cpu_cpuid(cpu);
                break;

            case 0xA3: /* BT r/m,reg */
            case 0xAB: /* BTS r/m,reg */
            case 0xB3: /* BTR r/m,reg */
            case 0xBB: /* BTC r/m,reg */
            case 0xBA: { /* Group 8: BT/BTS/BTR/BTC r/m,imm8 */
                modrm_byte = CPU_FETCH8(cpu);
                bool immediate = op2 == 0xBA;
                unsigned operation = immediate ? (modrm_byte >> 3) & 7u
                                               : (op2 >> 3) & 7u;
                if (operation < 4u) {
                    (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                    break;
                }

                unsigned bit_mask = op32 ? 31u : 15u;
                unsigned source = (modrm_byte >> 3) & 7u;
                int32_t index = immediate ? 0 : op32
                    ? (int32_t)*reg32_ptr(cpu, source)
                    : (int16_t)*reg16_ptr(cpu, source);
                unsigned bit = (uint32_t)index & bit_mask;
                /* Register indices address a signed bit string in memory.
                 * Subtract the bit remainder before division to round down,
                 * including negative indices; immediates never move the base. */
                int32_t displacement = (index - (int32_t)bit) / 8;
                m = CPU_DECODE_MODRM_OFFSET(cpu, modrm_byte, displacement);
                m.read_modify_write = operation != 4u;
                if (immediate) bit = CPU_FETCH8(cpu) & bit_mask;

                if (lock_prefix && (m.is_reg || operation == 4u)) {
                    (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                    break;
                }
                uint32_t value = op32 ? modrm_read32(cpu, &m)
                                     : modrm_read16(cpu, &m);
                uint32_t selected = 1u << bit;
                set_flag(cpu, FLAG_CF, (value & selected) != 0);
                if (operation == 4u) break;
                if (operation == 5u) value |= selected;
                else if (operation == 6u) value &= ~selected;
                else value ^= selected;
                /* One interpreter instruction owns this virtual CPU's RMW;
                 * guest interrupts are delivered only after it completes. */
                if (op32) modrm_write32(cpu, &m, value);
                else modrm_write16(cpu, &m, (uint16_t)value);
                break;
            }

            case 0xA8: /* PUSH GS */
                CPU_STACK_PUSH(cpu->gs);
                break;

            case 0xA9: /* POP GS */
                cpu_pop_segment(cpu, 5, op32 ? 4u : 2u, lock_prefix, insn_eip);
                break;

            case 0xB2: /* LSS */
            case 0xB4: /* LFS */
            case 0xB5: /* LGS */
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
                cpu_load_far_pointer(cpu, &m, op2 == 0xB2 ? 2u : op2 - 0xB0u,
                                      op32, lock_prefix, insn_eip);
                break;

            case 0xC8: case 0xC9: case 0xCA: case 0xCB:
            case 0xCC: case 0xCD: case 0xCE: case 0xCF: { /* BSWAP r32 */
                if (!op32) {
                    (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                    break;
                }
                uint32_t *reg = reg32_ptr(cpu, op2 - 0xC8);
                uint32_t value = *reg;
                *reg = ((value & 0x000000FFU) << 24) |
                       ((value & 0x0000FF00U) << 8) |
                       ((value & 0x00FF0000U) >> 8) |
                       ((value & 0xFF000000U) >> 24);
                break;
            }

            /* ── 0F 90-9F: SETcc r/m8 ────────────────────────────── */
            case 0x90: case 0x91: case 0x92: case 0x93:
            case 0x94: case 0x95: case 0x96: case 0x97:
            case 0x98: case 0x99: case 0x9A: case 0x9B:
            case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
                uint8_t set_val = eval_condition(cpu, op2 - 0x90) ? 1 : 0;
                modrm_write8(cpu, &m, set_val);
                break;
            }

            /* ── 0F A4: SHLD r/m16/32, r16/32, imm8 ─────────────── */
            case 0xA4: {
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
                m.read_modify_write = true;
                uint8_t shld_cnt = CPU_FETCH8(cpu);
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
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
                m.read_modify_write = true;
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
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
                m.read_modify_write = true;
                uint8_t shrd_cnt = CPU_FETCH8(cpu);
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
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
                m.read_modify_write = true;
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
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
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
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
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
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
                uint16_t src = modrm_read16(cpu, &m);
                *reg32_ptr(cpu, m.reg_field) = (uint32_t)src;
                break;
            }

            /* ── 0F BC: BSF r16/32, r/m16/32 ────────────────────── */
            case 0xBC: {
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
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
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
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
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
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
                modrm_byte = CPU_FETCH8(cpu);
                m = CPU_DECODE_MODRM(cpu, modrm_byte);
                int16_t src = (int16_t)modrm_read16(cpu, &m);
                *reg32_ptr(cpu, m.reg_field) = (uint32_t)(int32_t)src;
                break;
            }

            default:
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  ADC  (0x10 - 0x15)
         * ════════════════════════════════════════════════════════════ */
        case 0x10: { /* ADC r/m8, r8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_adc8(cpu, val, reg));
            break;
        }
        case 0x11: { /* ADC r/m16/32, r16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t reg = *reg32_ptr(cpu, m.reg_field);
                modrm_write32(cpu, &m, alu_adc32(cpu, val, reg));
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t reg = *reg16_ptr(cpu, m.reg_field);
                modrm_write16(cpu, &m, alu_adc16(cpu, val, reg));
            }
            break;
        }
        case 0x12: { /* ADC r8, r/m8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_adc8(cpu, *dst, val);
            break;
        }
        case 0x13: { /* ADC r16/32, r/m16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                uint32_t *dst = reg32_ptr(cpu, m.reg_field);
                uint32_t val = modrm_read32(cpu, &m);
                *dst = alu_adc32(cpu, *dst, val);
            } else {
                uint16_t *dst = reg16_ptr(cpu, m.reg_field);
                uint16_t val = modrm_read16(cpu, &m);
                *dst = alu_adc16(cpu, *dst, val);
            }
            break;
        }
        case 0x14: { /* ADC AL, imm8 */
            uint8_t imm = CPU_FETCH8(cpu);
            cpu->al = alu_adc8(cpu, cpu->al, imm);
            break;
        }
        case 0x16: /* PUSH SS */
            CPU_STACK_PUSH(cpu->ss);
            break;
        case 0x17: /* POP SS */
            cpu_pop_segment(cpu, 2, op32 ? 4u : 2u, lock_prefix, insn_eip);
            break;

        /* ════════════════════════════════════════════════════════════
         *  SBB  (0x18 - 0x1D)
         * ════════════════════════════════════════════════════════════ */
        case 0x18: { /* SBB r/m8, r8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_sbb8(cpu, val, reg));
            break;
        }
        case 0x19: { /* SBB r/m16/32, r16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t reg = *reg32_ptr(cpu, m.reg_field);
                modrm_write32(cpu, &m, alu_sbb32(cpu, val, reg));
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t reg = *reg16_ptr(cpu, m.reg_field);
                modrm_write16(cpu, &m, alu_sbb16(cpu, val, reg));
            }
            break;
        }
        case 0x1A: { /* SBB r8, r/m8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_sbb8(cpu, *dst, val);
            break;
        }
        case 0x1B: { /* SBB r16/32, r/m16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                uint32_t *dst = reg32_ptr(cpu, m.reg_field);
                uint32_t val = modrm_read32(cpu, &m);
                *dst = alu_sbb32(cpu, *dst, val);
            } else {
                uint16_t *dst = reg16_ptr(cpu, m.reg_field);
                uint16_t val = modrm_read16(cpu, &m);
                *dst = alu_sbb16(cpu, *dst, val);
            }
            break;
        }
        case 0x1C: { /* SBB AL, imm8 */
            uint8_t imm = CPU_FETCH8(cpu);
            cpu->al = alu_sbb8(cpu, cpu->al, imm);
            break;
        }
        case 0x1E: /* PUSH DS */
            CPU_STACK_PUSH(cpu->ds);
            break;
        case 0x1F: /* POP DS */
            cpu_pop_segment(cpu, 3, op32 ? 4u : 2u, lock_prefix, insn_eip);
            break;

        /* ════════════════════════════════════════════════════════════
         *  AND  (0x20 - 0x25)
         * ════════════════════════════════════════════════════════════ */
        case 0x20: { /* AND r/m8, r8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_and8(cpu, val, reg));
            break;
        }
        case 0x21: { /* AND r/m16/32, r16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t reg = *reg32_ptr(cpu, m.reg_field);
                modrm_write32(cpu, &m, alu_and32(cpu, val, reg));
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t reg = *reg16_ptr(cpu, m.reg_field);
                modrm_write16(cpu, &m, alu_and16(cpu, val, reg));
            }
            break;
        }
        case 0x22: { /* AND r8, r/m8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_and8(cpu, *dst, val);
            break;
        }
        case 0x23: { /* AND r16/32, r/m16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                uint32_t *dst = reg32_ptr(cpu, m.reg_field);
                uint32_t val = modrm_read32(cpu, &m);
                *dst = alu_and32(cpu, *dst, val);
            } else {
                uint16_t *dst = reg16_ptr(cpu, m.reg_field);
                uint16_t val = modrm_read16(cpu, &m);
                *dst = alu_and16(cpu, *dst, val);
            }
            break;
        }
        case 0x24: { /* AND AL, imm8 */
            uint8_t imm = CPU_FETCH8(cpu);
            cpu->al = alu_and8(cpu, cpu->al, imm);
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
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_sub8(cpu, val, reg));
            break;
        }
        case 0x29: { /* SUB r/m16/32, r16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t reg = *reg32_ptr(cpu, m.reg_field);
                modrm_write32(cpu, &m, alu_sub32(cpu, val, reg));
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t reg = *reg16_ptr(cpu, m.reg_field);
                modrm_write16(cpu, &m, alu_sub16(cpu, val, reg));
            }
            break;
        }
        case 0x2A: { /* SUB r8, r/m8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_sub8(cpu, *dst, val);
            break;
        }
        case 0x2B: { /* SUB r16/32, r/m16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                uint32_t *dst = reg32_ptr(cpu, m.reg_field);
                uint32_t val = modrm_read32(cpu, &m);
                *dst = alu_sub32(cpu, *dst, val);
            } else {
                uint16_t *dst = reg16_ptr(cpu, m.reg_field);
                uint16_t val = modrm_read16(cpu, &m);
                *dst = alu_sub16(cpu, *dst, val);
            }
            break;
        }
        case 0x2C: { /* SUB AL, imm8 */
            uint8_t imm = CPU_FETCH8(cpu);
            cpu->al = alu_sub8(cpu, cpu->al, imm);
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
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            modrm_write8(cpu, &m, alu_xor8(cpu, val, reg));
            break;
        }
        case 0x31: { /* XOR r/m16/32, r16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t reg = *reg32_ptr(cpu, m.reg_field);
                modrm_write32(cpu, &m, alu_xor32(cpu, val, reg));
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t reg = *reg16_ptr(cpu, m.reg_field);
                modrm_write16(cpu, &m, alu_xor16(cpu, val, reg));
            }
            break;
        }
        case 0x32: { /* XOR r8, r/m8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint8_t *dst = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            *dst = alu_xor8(cpu, *dst, val);
            break;
        }
        case 0x33: { /* XOR r16/32, r/m16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                uint32_t *dst = reg32_ptr(cpu, m.reg_field);
                uint32_t val = modrm_read32(cpu, &m);
                *dst = alu_xor32(cpu, *dst, val);
            } else {
                uint16_t *dst = reg16_ptr(cpu, m.reg_field);
                uint16_t val = modrm_read16(cpu, &m);
                *dst = alu_xor16(cpu, *dst, val);
            }
            break;
        }
        case 0x34: { /* XOR AL, imm8 */
            uint8_t imm = CPU_FETCH8(cpu);
            cpu->al = alu_xor8(cpu, cpu->al, imm);
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
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            alu_cmp8(cpu, val, reg);
            break;
        }
        case 0x39: { /* CMP r/m16/32, r16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t reg = *reg32_ptr(cpu, m.reg_field);
                alu_cmp32(cpu, val, reg);
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t reg = *reg16_ptr(cpu, m.reg_field);
                alu_cmp16(cpu, val, reg);
            }
            break;
        }
        case 0x3A: { /* CMP r8, r/m8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            alu_cmp8(cpu, reg, val);
            break;
        }
        case 0x3B: { /* CMP r16/32, r/m16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                uint32_t reg = *reg32_ptr(cpu, m.reg_field);
                uint32_t val = modrm_read32(cpu, &m);
                alu_cmp32(cpu, reg, val);
            } else {
                uint16_t reg = *reg16_ptr(cpu, m.reg_field);
                uint16_t val = modrm_read16(cpu, &m);
                alu_cmp16(cpu, reg, val);
            }
            break;
        }
        case 0x3C: { /* CMP AL, imm8 */
            uint8_t imm = CPU_FETCH8(cpu);
            alu_cmp8(cpu, cpu->al, imm);
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
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            if (op32) {
                bool cf = get_flag(cpu, FLAG_CF);
                uint32_t *r = reg32_ptr(cpu, opcode - 0x40);
                *r = alu_add32(cpu, *r, 1);
                set_flag(cpu, FLAG_CF, cf);
            } else {
                uint16_t *r = reg16_ptr(cpu, opcode - 0x40);
                *r = alu_inc16(cpu, *r);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  DEC reg16  (0x48 - 0x4F)
         * ════════════════════════════════════════════════════════════ */
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            if (op32) {
                bool cf = get_flag(cpu, FLAG_CF);
                uint32_t *r = reg32_ptr(cpu, opcode - 0x48);
                *r = alu_sub32(cpu, *r, 1);
                set_flag(cpu, FLAG_CF, cf);
            } else {
                uint16_t *r = reg16_ptr(cpu, opcode - 0x48);
                *r = alu_dec16(cpu, *r);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  PUSH reg16/32  (0x50 - 0x57)
         * ════════════════════════════════════════════════════════════ */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            if (op32)
                CPU_STACK_PUSH(*reg32_ptr(cpu, opcode - 0x50));
            else
                CPU_STACK_PUSH(*reg16_ptr(cpu, opcode - 0x50));
            break;

        /* ════════════════════════════════════════════════════════════
         *  POP reg16/32  (0x58 - 0x5F)
         * ════════════════════════════════════════════════════════════ */
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            if (op32)
                *reg32_ptr(cpu, opcode - 0x58) = CPU_STACK_POP();
            else
                *reg16_ptr(cpu, opcode - 0x58) = (uint16_t)CPU_STACK_POP();
            break;

        /* ════════════════════════════════════════════════════════════
         *  Jcc short  (0x70 - 0x7F)
         * ════════════════════════════════════════════════════════════ */
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            int8_t rel = (int8_t)CPU_FETCH8(cpu);
            if (eval_condition(cpu, opcode - 0x70))
                (void)cpu_near_jump(cpu, cpu->eip + (uint32_t)(int32_t)rel, op32,
                                     fetch->limit, insn_eip);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  Group 1  (0x80 - 0x83)
         * ════════════════════════════════════════════════════════════ */
        case 0x80: { /* Group 1 r/m8, imm8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = m.reg_field != 7u;
            uint8_t imm = CPU_FETCH8(cpu);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t result = group1_alu8(cpu, m.reg_field, val, imm);
            if (m.reg_field != 7) /* not CMP */
                modrm_write8(cpu, &m, result);
            break;
        }
        case 0x81: { /* Group 1 r/m16/32, imm16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = m.reg_field != 7u;
            if (op32) {
                uint32_t imm = CPU_FETCH32(cpu);
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t result = group1_alu32(cpu, m.reg_field, val, imm);
                if (m.reg_field != 7)
                    modrm_write32(cpu, &m, result);
            } else {
                uint16_t imm = CPU_FETCH16(cpu);
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t result = group1_alu16(cpu, m.reg_field, val, imm);
                if (m.reg_field != 7)
                    modrm_write16(cpu, &m, result);
            }
            break;
        }
        case 0x82: { /* Group 1 r/m8, imm8 (alias of 0x80 on 8086) */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = m.reg_field != 7u;
            uint8_t imm = CPU_FETCH8(cpu);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t result = group1_alu8(cpu, m.reg_field, val, imm);
            if (m.reg_field != 7)
                modrm_write8(cpu, &m, result);
            break;
        }
        case 0x83: { /* Group 1 r/m16/32, sign-extended imm8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = m.reg_field != 7u;
            if (op32) {
                uint32_t imm = (uint32_t)(int32_t)(int8_t)CPU_FETCH8(cpu);
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t result = group1_alu32(cpu, m.reg_field, val, imm);
                if (m.reg_field != 7)
                    modrm_write32(cpu, &m, result);
            } else {
                uint16_t imm = (uint16_t)(int16_t)(int8_t)CPU_FETCH8(cpu);
                uint16_t val = modrm_read16(cpu, &m);
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
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint8_t val = modrm_read8(cpu, &m);
            uint8_t reg = *reg8_ptr(cpu, m.reg_field);
            update_flags_logic8(cpu, val & reg);
            break;
        }
        case 0x85: { /* TEST r/m16/32, r16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                uint32_t reg = *reg32_ptr(cpu, m.reg_field);
                update_flags_logic32(cpu, val & reg);
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                uint16_t reg = *reg16_ptr(cpu, m.reg_field);
                update_flags_logic16(cpu, val & reg);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  XCHG  (0x86 - 0x87)
         * ════════════════════════════════════════════════════════════ */
        case 0x86: { /* XCHG r/m8, r8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            uint8_t *reg = reg8_ptr(cpu, m.reg_field);
            uint8_t val = modrm_read8(cpu, &m);
            modrm_write8(cpu, &m, *reg);
            *reg = val;
            break;
        }
        case 0x87: { /* XCHG r/m16/32, r16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            if (op32) {
                uint32_t *rp = reg32_ptr(cpu, m.reg_field);
                uint32_t mem_val = modrm_read32(cpu, &m);
                modrm_write32(cpu, &m, *rp);
                *rp = mem_val;
            } else {
                uint16_t *reg = reg16_ptr(cpu, m.reg_field);
                uint16_t val = modrm_read16(cpu, &m);
                modrm_write16(cpu, &m, *reg);
                *reg = val;
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  MOV  (0x88 - 0x8B)
         * ════════════════════════════════════════════════════════════ */
        case 0x88: { /* MOV r/m8, r8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            modrm_write8(cpu, &m, *reg8_ptr(cpu, m.reg_field));
            break;
        }
        case 0x89: { /* MOV r/m16/32, r16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32)
                modrm_write32(cpu, &m, *reg32_ptr(cpu, m.reg_field));
            else
                modrm_write16(cpu, &m, *reg16_ptr(cpu, m.reg_field));
            break;
        }
        case 0x8A: { /* MOV r8, r/m8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            *reg8_ptr(cpu, m.reg_field) = modrm_read8(cpu, &m);
            break;
        }
        case 0x8B: { /* MOV r16/32, r/m16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
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
            modrm_byte = CPU_FETCH8(cpu);
            if (lock_prefix || ((modrm_byte >> 3) & 7u) >= 6u) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint16_t seg = *seg_ptr(cpu, m.reg_field);
            if (op32 && m.is_reg) *reg32_ptr(cpu, m.rm_field) = seg;
            else modrm_write16(cpu, &m, seg);
            break;
        }

        case 0x8D: { /* LEA r16/32, m */
            modrm_byte = CPU_FETCH8(cpu);
            uint8_t lea_mod = (modrm_byte >> 6) & 3;
            uint8_t lea_reg = (modrm_byte >> 3) & 7;
            uint8_t lea_rm  = modrm_byte & 7;

            if (adr32) {
                /* 32-bit effective address (no segment applied for LEA) */
                uint32_t ea32 = 0;
                if (lea_rm == 4) {
                    /* SIB */
                    uint8_t sib   = CPU_FETCH8(cpu);
                    uint8_t scale = (sib >> 6) & 3;
                    uint8_t index = (sib >> 3) & 7;
                    uint8_t base  = sib & 7;
                    if (base == 5 && lea_mod == 0)
                        ea32 = CPU_FETCH32(cpu);
                    else
                        ea32 = *reg32_ptr(cpu, base);
                    if (index != 4)
                        ea32 += *reg32_ptr(cpu, index) << scale;
                } else if (lea_rm == 5 && lea_mod == 0) {
                    ea32 = CPU_FETCH32(cpu);
                } else {
                    ea32 = *reg32_ptr(cpu, lea_rm);
                }
                if (lea_mod == 1)
                    ea32 += (uint32_t)(int32_t)(int8_t)CPU_FETCH8(cpu);
                else if (lea_mod == 2)
                    ea32 += CPU_FETCH32(cpu);

                if (op32)
                    *reg32_ptr(cpu, lea_reg) = ea32;
                else
                    *reg16_ptr(cpu, lea_reg) = (uint16_t)ea32;
            } else {
                /* 16-bit effective address */
                uint16_t ea = 0;
                switch (lea_rm) {
                case 0: ea = cpu->bx + cpu->si; break;
                case 1: ea = cpu->bx + cpu->di; break;
                case 2: ea = cpu->bp + cpu->si; break;
                case 3: ea = cpu->bp + cpu->di; break;
                case 4: ea = cpu->si; break;
                case 5: ea = cpu->di; break;
                case 6:
                    if (lea_mod == 0)
                        ea = CPU_FETCH16(cpu);
                    else
                        ea = cpu->bp;
                    break;
                case 7: ea = cpu->bx; break;
                }
                if (lea_mod == 1)
                    ea += (uint16_t)(int16_t)(int8_t)CPU_FETCH8(cpu);
                else if (lea_mod == 2)
                    ea += CPU_FETCH16(cpu);

                if (op32)
                    *reg32_ptr(cpu, lea_reg) = (uint32_t)ea;
                else
                    *reg16_ptr(cpu, lea_reg) = ea;
            }
            break;
        }

        case 0x8E: { /* MOV sreg, r/m16 */
            modrm_byte = CPU_FETCH8(cpu);
            unsigned segment = (modrm_byte >> 3) & 7u;
            if (lock_prefix || segment == 1u || segment >= 6u) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint16_t val = modrm_read16(cpu, &m);
            cpu_system_segment_t loaded;
            if (!cpu_prepare_segment_load(cpu, segment, val, insn_eip, &loaded)) break;
            cpu_commit_segment_load(cpu, segment, val, &loaded);
            if (segment == 2 && !cpu->irq_shadow) cpu->irq_shadow = 2;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  POP r/m16/32  (0x8F)
         * ════════════════════════════════════════════════════════════ */
        case 0x8F: { /* POP r/m16/32 (only reg_field=0 is valid) */
            modrm_byte = CPU_FETCH8(cpu);
            if (lock_prefix || (modrm_byte & 0x38u)) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            unsigned size = op32 ? 4u : 2u;
            cpu_stack_access_t stack;
            modrm_t source;
            if (!cpu_stack_begin(cpu, &stack, false, insn_eip) ||
                !cpu_stack_slot(cpu, &stack, cpu->esp & stack.mask, size,
                                 false, false, insn_eip, &source))
                goto instruction_complete;
            /* Both operands must be accessible before POP consumes the stack.
             * ESP-based destinations use the post-increment address even SS16. */
            uint32_t next_esp = cpu_stack_next_esp(&stack, cpu->esp, cpu->esp + size);
            if (m.esp_base) m.offset += next_esp - cpu->esp;
            if (!cpu_modrm_prepare(cpu, &m, size, true, insn_eip))
                goto instruction_complete;
            uint32_t val = cpu_modrm_read(cpu, &source, size);
            cpu->esp = next_esp;
            if (op32) modrm_write32(cpu, &m, val);
            else modrm_write16(cpu, &m, (uint16_t)val);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  XCHG AX, reg16  (0x90 - 0x97)  (0x90 = NOP)
         * ════════════════════════════════════════════════════════════ */
        case 0x90: /* NOP (XCHG AX,AX) */
            break;
        case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97: {
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            if (op32) {
                uint32_t *r = reg32_ptr(cpu, opcode - 0x90);
                uint32_t tmp = cpu->eax;
                cpu->eax = *r;
                *r = tmp;
            } else {
                uint16_t *r = reg16_ptr(cpu, opcode - 0x90);
                uint16_t tmp = cpu->ax;
                cpu->ax = *r;
                *r = tmp;
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  CBW / CWD  (0x98 / 0x99)
         * ════════════════════════════════════════════════════════════ */
        case 0x98: /* CBW / CWDE */
        case 0x99: /* CWD / CDQ */
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            if (opcode == 0x98) {
                if (op32) cpu->eax = (uint32_t)(int32_t)(int16_t)cpu->ax;
                else cpu->ax = (uint16_t)(int16_t)(int8_t)cpu->al;
            } else {
                if (op32) cpu->edx = (cpu->eax & 0x80000000u) ? 0xFFFFFFFFu : 0;
                else cpu->dx = (cpu->ax & 0x8000) ? 0xFFFF : 0;
            }
            break;

        /* ════════════════════════════════════════════════════════════
         *  CALL far  (0x9A)
         * ════════════════════════════════════════════════════════════ */
        case 0x9A: { /* CALL far ptr16:16/32 */
            uint32_t off = op32 ? CPU_FETCH32(cpu) : CPU_FETCH16(cpu);
            uint16_t seg = CPU_FETCH16(cpu);
            cpu_far_transfer(cpu, seg, off, op32 ? 4u : 2u, true, lock_prefix, insn_eip);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  FWAIT / SAHF / LAHF  (0x9B / 0x9E / 0x9F)
         * ════════════════════════════════════════════════════════════ */
        case 0x9B: /* FWAIT/WAIT */
            if ((cpu->cr0 & (DOS_CR0_MP | DOS_CR0_TS)) ==
                (DOS_CR0_MP | DOS_CR0_TS))
                (void)cpu_deliver_exception(vm, 7, insn_eip, 0, false);
            break;
        case 0x9E: /* SAHF — AH → flags low byte */
            cpu->flags = (cpu->flags & 0xFF00) | (cpu->ah & 0xD5) | FLAGS_FIXED;
            break;
        case 0x9F: /* LAHF — flags low byte → AH */
            cpu->ah = (uint8_t)((cpu->flags & 0xD5) | FLAGS_FIXED);
            break;

        /* ════════════════════════════════════════════════════════════
         *  PUSHF / POPF  (0x9C / 0x9D)
         * ════════════════════════════════════════════════════════════ */
        case 0x9C: /* PUSHF / PUSHFD */
        case 0x9D: /* POPF / POPFD */
            cpu_flags_stack(cpu, opcode == 0x9D, op32 ? 4u : 2u, insn_eip);
            break;

        /* ════════════════════════════════════════════════════════════
         *  MOV AL/AX, moffs  (0xA0 - 0xA3)
         * ════════════════════════════════════════════════════════════ */
        case 0xA0: case 0xA1: /* MOV accumulator, moffs */
        case 0xA2: case 0xA3: { /* MOV moffs, accumulator */
            uint32_t off = adr32 ? CPU_FETCH32(cpu) : CPU_FETCH16(cpu);
            m = (modrm_t){ .offset = off,
                .seg_index = cpu->seg_override >= 0 ? (uint8_t)cpu->seg_override : 3u };
            unsigned size = opcode & 1u ? op32 ? 4u : 2u : 1u;
            if (opcode & 2u) CPU_MODRM_WRITE(cpu, &m, size, cpu->eax);
            else {
                uint32_t value = CPU_MODRM_READ(cpu, &m, size);
                if (size == 1u) cpu->al = value;
                else if (size == 2u) cpu->ax = value;
                else cpu->eax = value;
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  String operations (0xA4 - 0xAF)
         * ════════════════════════════════════════════════════════════ */
        case 0xA4: case 0xA5: /* MOVS */
        case 0xA6: case 0xA7: /* CMPS */
        case 0xAA: case 0xAB: /* STOS */
        case 0xAC: case 0xAD: /* LODS */
        case 0xAE: case 0xAF: /* SCAS */
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            string_pending = cpu_string_memory(cpu, opcode, string_width, adr32, insn_eip);
            break;

        /* ════════════════════════════════════════════════════════════
         *  TEST AL/AX, imm  (0xA8 / 0xA9)
         * ════════════════════════════════════════════════════════════ */
        case 0xA8: { /* TEST AL, imm8 */
            uint8_t imm = CPU_FETCH8(cpu);
            update_flags_logic8(cpu, cpu->al & imm);
            break;
        }
        case 0xA9: { /* TEST AX/EAX, imm16/32 */
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            if (op32) {
                uint32_t imm = CPU_FETCH32(cpu);
                update_flags_logic32(cpu, cpu->eax & imm);
            } else {
                uint16_t imm = CPU_FETCH16(cpu);
                update_flags_logic16(cpu, cpu->ax & imm);
            }
            break;
        }


        /* ════════════════════════════════════════════════════════════
         *  MOV reg8, imm8  (0xB0 - 0xB7)
         * ════════════════════════════════════════════════════════════ */
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
            uint8_t imm = CPU_FETCH8(cpu);
            *reg8_ptr(cpu, opcode - 0xB0) = imm;
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  MOV reg16/32, imm16/32  (0xB8 - 0xBF)
         * ════════════════════════════════════════════════════════════ */
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            if (op32) {
                uint32_t imm = CPU_FETCH32(cpu);
                *reg32_ptr(cpu, opcode - 0xB8) = imm;
            } else {
                uint16_t imm = CPU_FETCH16(cpu);
                *reg16_ptr(cpu, opcode - 0xB8) = imm;
            }
            break;

        /* ════════════════════════════════════════════════════════════
         *  Shift/Rotate Group 2: r/m, imm8  (0xC0/0xC1 - 186+)
         * ════════════════════════════════════════════════════════════ */
        case 0xC0: { /* Group 2 r/m8, imm8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            uint8_t cnt = CPU_FETCH8(cpu);
            uint8_t val = modrm_read8(cpu, &m);
            modrm_write8(cpu, &m, shift_rotate8(cpu, m.reg_field, val, cnt));
            break;
        }
        case 0xC1: { /* Group 2 r/m16/32, imm8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            uint8_t cnt;
            if (op32) {
                cnt = CPU_FETCH8(cpu);
                uint32_t val = modrm_read32(cpu, &m);
                modrm_write32(cpu, &m, shift_rotate32(cpu, m.reg_field, val, cnt));
            } else {
                cnt = CPU_FETCH8(cpu);
                uint16_t val = modrm_read16(cpu, &m);
                modrm_write16(cpu, &m, shift_rotate16(cpu, m.reg_field, val, cnt));
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  RET near  (0xC2 / 0xC3)
         * ════════════════════════════════════════════════════════════ */
        case 0xC2: case 0xC3: { /* RET near, optionally discard arguments */
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            uint16_t pop_bytes = opcode == 0xC2 ? CPU_FETCH16(cpu) : 0;
            cpu_near_return(cpu, op32 ? 4u : 2u, pop_bytes, fetch->limit, insn_eip);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  MOV r/m, imm  (0xC6 / 0xC7)
         * ════════════════════════════════════════════════════════════ */
        case 0xC6: { /* MOV r/m8, imm8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            uint8_t imm = CPU_FETCH8(cpu);
            modrm_write8(cpu, &m, imm);
            break;
        }
        case 0xC7: { /* MOV r/m16/32, imm16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                uint32_t imm = CPU_FETCH32(cpu);
                modrm_write32(cpu, &m, imm);
            } else {
                uint16_t imm = CPU_FETCH16(cpu);
                modrm_write16(cpu, &m, imm);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  ENTER / LEAVE  (0xC8 / 0xC9, 186+)
         * ════════════════════════════════════════════════════════════ */
        case 0xC8: { /* ENTER imm16, imm8 */
            uint16_t alloc_size = CPU_FETCH16(cpu);
            uint8_t nesting = CPU_FETCH8(cpu) & 0x1Fu;
            cpu_stack_enter(cpu, op32 ? 4u : 2u, alloc_size, nesting, lock_prefix, insn_eip);
            break;
        }
        case 0xC9: /* LEAVE */
            cpu_stack_leave(cpu, op32 ? 4u : 2u, lock_prefix, insn_eip);
            break;

        /* ════════════════════════════════════════════════════════════
         *  RETF  (0xCA / 0xCB)
         * ════════════════════════════════════════════════════════════ */
        case 0xCA: { /* RETF, pop imm16 */
            uint16_t pop_bytes = CPU_FETCH16(cpu);
            cpu_far_return(cpu, op32 ? 4u : 2u, pop_bytes, lock_prefix, insn_eip);
            break;
        }
        case 0xCB: /* RETF */
            cpu_far_return(cpu, op32 ? 4u : 2u, 0, lock_prefix, insn_eip);
            break;

        /* ════════════════════════════════════════════════════════════
         *  INT 3 (0xCC) — Breakpoint
         * ════════════════════════════════════════════════════════════ */
        case 0xCC: { /* INT 3 */
            if (lock_prefix) (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
            else if (cpu8086_uses_guest_idt(cpu))
                (void)cpu8086_deliver_guest_interrupt(vm, 3, CPU_EVENT_SOFTWARE_EXCEPTION,
                                                       cpu->eip, insn_eip, 0, false);
            else (void)cpu_deliver_exception(vm, 3, cpu->eip, 0, false);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  INT  (0xCD)
         * ════════════════════════════════════════════════════════════ */
        case 0xCD: { /* INT imm8 */
            uint8_t int_num = CPU_FETCH8(cpu);
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            if ((!cpu->protected_mode || cpu8086_uses_guest_idt(cpu)) &&
                !dos_vcpi_pm_entry_source(vm, int_num)) {
                (void)cpu8086_deliver_guest_interrupt(vm, int_num, CPU_EVENT_SOFTWARE,
                                                       cpu->eip, insn_eip, 0, false);
                break;
            }
            cpu->irq_shadow = 0;
            bool frame32 = cpu->protected_mode ? cpu->op_size_32 : op32;
            /* Log INTs during early boot only. Post-5K insns the serial
             * spam dominates wall-clock (every [INT] is ~40 bytes at 115200
             * baud → caps the emulator at ~3000 insns/s). Unhandled INTs
             * still print via default: cases in dos_api.c / dos_dpmi.c. */
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
            uint32_t saved_eflags = cpu8086_flags_image(cpu);

            if (cpu_deliver_pm_software_interrupt(vm, int_num,
                                                  saved_eip, insn_eip))
                break;

            if (!cpu_push_private_int_frame(cpu, frame32 ? 4u : 2u,
                                              saved_eip, saved_eflags, insn_eip)) break;
            set_flag(cpu, FLAG_IF, false);
            set_flag(cpu, FLAG_TF, false);

            uint8_t previous_frame_bytes = vm->software_int_frame_bytes;
            uint32_t previous_return_flags =
                vm->software_int_return_flags;
            vm->software_int_frame_bytes = frame32 ? 12u : 6u;
            vm->software_int_return_flags = saved_eflags;
            dos_int_dispatch(vm, int_num);
            bool frame_consumed = !vm->software_int_frame_bytes;
            vm->software_int_frame_bytes = previous_frame_bytes;
            vm->software_int_return_flags = previous_return_flags;

            /* If C handler (CS:IP unchanged), discard the frame — no IRET.
             * Keep current flags (handler set CF etc.), only restore SP.
             * If IVT redirect (CS:IP changed), leave frame for handler's IRET. */
            if (cpu->cs == saved_cs && cpu->eip == saved_eip) {
                /* Discard the interrupt frame from stack */
                if (!frame_consumed) cpu_stack_adjust(cpu, frame32 ? 12 : 6);
                const uint32_t status_flags = FLAG_CF | FLAG_PF | FLAG_AF |
                                              FLAG_ZF | FLAG_SF | FLAG_OF;
                if (!frame_consumed)
                    cpu->eflags = (saved_eflags & ~status_flags) |
                                  (cpu->eflags & status_flags) | FLAGS_FIXED;
                if (cpu->protected_mode && vm->dpmi.active)
                    cpu->flags |= FLAG_IF;
            } else {
                cpu_commit_cs_load(cpu);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  INTO  (0xCE) — Interrupt on Overflow
         * ════════════════════════════════════════════════════════════ */
        case 0xCE: /* INTO */
            if (lock_prefix) (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
            else if (cpu->flags & FLAG_OF) {
                if (cpu8086_uses_guest_idt(cpu))
                    (void)cpu8086_deliver_guest_interrupt(vm, 4, CPU_EVENT_SOFTWARE_EXCEPTION,
                                                           cpu->eip, insn_eip, 0, false);
                else (void)cpu_deliver_exception(vm, 4, cpu->eip, 0, false);
            }
            break;

        /* ════════════════════════════════════════════════════════════
         *  IRET  (0xCF)
         * ════════════════════════════════════════════════════════════ */
        case 0xCF: /* IRET */
            cpu_interrupt_return(cpu, op32 ? 4u : 2u, lock_prefix, insn_eip);
            break;

        /* ════════════════════════════════════════════════════════════
         *  Shift/Rotate Group 2: r/m, 1  (0xD0/0xD1)
         * ════════════════════════════════════════════════════════════ */
        case 0xD0: { /* Group 2 r/m8, 1 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            uint8_t val = modrm_read8(cpu, &m);
            modrm_write8(cpu, &m, shift_rotate8(cpu, m.reg_field, val, 1));
            break;
        }
        case 0xD1: { /* Group 2 r/m16/32, 1 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                modrm_write32(cpu, &m, shift_rotate32(cpu, m.reg_field, val, 1));
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                modrm_write16(cpu, &m, shift_rotate16(cpu, m.reg_field, val, 1));
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  Shift/Rotate Group 2: r/m, CL  (0xD2/0xD3)
         * ════════════════════════════════════════════════════════════ */
        case 0xD2: { /* Group 2 r/m8, CL */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            uint8_t val = modrm_read8(cpu, &m);
            modrm_write8(cpu, &m, shift_rotate8(cpu, m.reg_field, val, cpu->cl));
            break;
        }
        case 0xD3: { /* Group 2 r/m16/32, CL */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
            if (op32) {
                uint32_t val = modrm_read32(cpu, &m);
                modrm_write32(cpu, &m, shift_rotate32(cpu, m.reg_field, val, cpu->cl));
            } else {
                uint16_t val = modrm_read16(cpu, &m);
                modrm_write16(cpu, &m, shift_rotate16(cpu, m.reg_field, val, cpu->cl));
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  XLAT  (0xD7)
         * ════════════════════════════════════════════════════════════ */
        case 0xD7: { /* XLAT */
            uint32_t offset = (adr32 ? cpu->ebx : cpu->bx) + cpu->al;
            m = (modrm_t){ .offset = adr32 ? offset : (uint16_t)offset,
                .seg_index = cpu->seg_override >= 0 ? (uint8_t)cpu->seg_override : 3u };
            cpu->al = modrm_read8(cpu, &m);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  LOOP / LOOPZ / LOOPNZ  (0xE0 - 0xE2)
         * ════════════════════════════════════════════════════════════ */
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: { /* LOOPcc / (E)CX zero */
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            int8_t rel = (int8_t)CPU_FETCH8(cpu);
            uint32_t count = adr32 ? cpu->ecx : cpu->cx;
            if (opcode != 0xE3) count = adr32 ? count - 1u : (uint16_t)(count - 1u);
            bool take = opcode == 0xE3 ? count == 0 : count != 0;
            if (opcode == 0xE0) take = take && !get_flag(cpu, FLAG_ZF);
            if (opcode == 0xE1) take = take && get_flag(cpu, FLAG_ZF);
            if (take && !cpu_near_jump(cpu, cpu->eip + (uint32_t)(int32_t)rel, op32,
                                        fetch->limit, insn_eip)) break;
            if (opcode != 0xE3) {
                if (adr32) cpu->ecx = count;
                else cpu->cx = (uint16_t)count;
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  IN / OUT imm8 or DX  (0xE4 - 0xE7, 0xEC - 0xEF)
         * ════════════════════════════════════════════════════════════ */
        case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        case 0xEC: case 0xED: case 0xEE: case 0xEF: {
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            uint16_t port = opcode & 8u ? cpu->dx : CPU_FETCH8(cpu);
            unsigned width = !(opcode & 1u) ? 1u : op32 ? 4u : 2u;
            if (!(opcode & 2u)) {
                if (width == 1) cpu->al = dos_io_read8(vm, port);
                else if (width == 2) cpu->ax = dos_io_read16(vm, port);
                else cpu->eax = dos_io_read32(vm, port);
            } else {
                if (width == 1) dos_io_write8(vm, port, cpu->al);
                else if (width == 2) dos_io_write16(vm, port, cpu->ax);
                else dos_io_write32(vm, port, cpu->eax);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  CALL near  (0xE8)
         * ════════════════════════════════════════════════════════════ */
        case 0xE8: { /* CALL near rel16/32 */
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            int32_t rel = op32 ? (int32_t)CPU_FETCH32(cpu) : (int16_t)CPU_FETCH16(cpu);
            cpu_near_call(cpu, cpu->eip + (uint32_t)rel, op32 ? 4u : 2u,
                            fetch->limit, insn_eip);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  JMP near / short  (0xE9 / 0xEB)
         * ════════════════════════════════════════════════════════════ */
        case 0xE9: { /* JMP near rel16/32 */
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            int32_t rel = op32 ? (int32_t)CPU_FETCH32(cpu) : (int16_t)CPU_FETCH16(cpu);
            (void)cpu_near_jump(cpu, cpu->eip + (uint32_t)rel, op32, fetch->limit, insn_eip);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  JMP far  (0xEA)
         * ════════════════════════════════════════════════════════════ */
        case 0xEA: { /* JMP far ptr16:16 or ptr16:32 */
            uint32_t off = op32 ? CPU_FETCH32(cpu) : CPU_FETCH16(cpu);
            uint16_t seg = CPU_FETCH16(cpu);
            cpu_far_transfer(cpu, seg, off, op32 ? 4u : 2u, false, lock_prefix, insn_eip);
            break;
        }

        case 0xEB: { /* JMP short rel8 */
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            int8_t rel = (int8_t)CPU_FETCH8(cpu);
            (void)cpu_near_jump(cpu, cpu->eip + (uint32_t)(int32_t)rel, op32,
                                 fetch->limit, insn_eip);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  F2/F3 prefixes handled at top as prefix
         * ════════════════════════════════════════════════════════════ */

        /* ════════════════════════════════════════════════════════════
         *  HLT  (0xF4)
         * ════════════════════════════════════════════════════════════ */
        case 0xF4: /* HLT */
            /* HLT resumes only after an interrupt is accepted. The outer
             * loop parks this host task instead of ending the DOS process. */
            cpu->halted = true;
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
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = m.reg_field == 2u || m.reg_field == 3u;
            switch (m.reg_field) {
            case 0: /* TEST r/m8, imm8 */
            case 1: { /* TEST r/m8, imm8 (undocumented alias) */
                uint8_t imm = CPU_FETCH8(cpu);
                uint8_t val = modrm_read8(cpu, &m);
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
                    (void)cpu_deliver_exception(vm, 0, insn_eip, 0, false);
                    break;
                }
                uint16_t dividend = cpu->ax;
                uint16_t quotient = dividend / val;
                uint8_t  remainder = dividend % val;
                if (quotient > 0xFF) {
                    (void)cpu_deliver_exception(vm, 0, insn_eip, 0, false);
                    break;
                }
                cpu->al = (uint8_t)quotient;
                cpu->ah = remainder;
                break;
            }
            case 7: { /* IDIV r/m8 (signed) */
                uint8_t val = modrm_read8(cpu, &m);
                if (val == 0) {
                    (void)cpu_deliver_exception(vm, 0, insn_eip, 0, false);
                    break;
                }
                int16_t dividend = (int16_t)cpu->ax;
                int16_t divisor  = (int16_t)(int8_t)val;
                int16_t quotient = dividend / divisor;
                int8_t  remainder = dividend % divisor;
                if (quotient < -128 || quotient > 127) {
                    (void)cpu_deliver_exception(vm, 0, insn_eip, 0, false);
                    break;
                }
                cpu->al = (uint8_t)(int8_t)quotient;
                cpu->ah = (uint8_t)remainder;
                break;
            }
            } /* switch reg_field */
            break;
        }

        case 0xF7: { /* Group 3 r/m16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = m.reg_field == 2u || m.reg_field == 3u;
            if (lock_prefix && (m.is_reg ||
                                (m.reg_field != 2 && m.reg_field != 3))) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            switch (m.reg_field) {
            case 0: /* TEST r/m16/32, imm16/32 */
            case 1: {
                if (lock_prefix) {
                    (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                    break;
                }
                if (op32) {
                    uint32_t imm = CPU_FETCH32(cpu);
                    uint32_t val = modrm_read32(cpu, &m);
                    update_flags_logic32(cpu, val & imm);
                } else {
                    uint16_t imm = CPU_FETCH16(cpu);
                    uint16_t val = modrm_read16(cpu, &m);
                    update_flags_logic16(cpu, val & imm);
                }
                break;
            }
            case 2: { /* NOT r/m16/32 */
                if (op32) modrm_write32(cpu, &m, ~modrm_read32(cpu, &m));
                else modrm_write16(cpu, &m, (uint16_t)~modrm_read16(cpu, &m));
                break;
            }
            case 3: { /* NEG r/m16/32 */
                if (op32) {
                    uint32_t val = modrm_read32(cpu, &m);
                    modrm_write32(cpu, &m, alu_sub32(cpu, 0, val));
                } else {
                    uint16_t val = modrm_read16(cpu, &m);
                    modrm_write16(cpu, &m, alu_sub16(cpu, 0, val));
                }
                break;
            }
            case 4: { /* MUL r/m16/32 (unsigned) */
                uint32_t val = op32 ? modrm_read32(cpu, &m) : modrm_read16(cpu, &m);
                uint64_t result = (uint64_t)(op32 ? cpu->eax : cpu->ax) * val;
                bool overflow;
                if (op32) {
                    cpu->eax = (uint32_t)result;
                    cpu->edx = (uint32_t)(result >> 32);
                    overflow = cpu->edx != 0;
                } else {
                    cpu->ax = (uint16_t)result;
                    cpu->dx = (uint16_t)(result >> 16);
                    overflow = cpu->dx != 0;
                }
                set_flag(cpu, FLAG_CF, overflow);
                set_flag(cpu, FLAG_OF, overflow);
                break;
            }
            case 5: { /* IMUL r/m16/32 (signed) */
                int64_t val = op32 ? (int32_t)modrm_read32(cpu, &m)
                                   : (int16_t)modrm_read16(cpu, &m);
                int64_t result = (op32 ? (int64_t)(int32_t)cpu->eax
                                      : (int64_t)(int16_t)cpu->ax) * val;
                bool overflow;
                if (op32) {
                    cpu->eax = (uint32_t)result;
                    cpu->edx = (uint32_t)((uint64_t)result >> 32);
                    overflow = result < -2147483648LL || result > 2147483647LL;
                } else {
                    cpu->ax = (uint16_t)result;
                    cpu->dx = (uint16_t)((uint64_t)result >> 16);
                    overflow = result < -32768 || result > 32767;
                }
                set_flag(cpu, FLAG_CF, overflow);
                set_flag(cpu, FLAG_OF, overflow);
                break;
            }
            case 6: { /* DIV r/m16/32 (unsigned) */
                uint32_t val = op32 ? modrm_read32(cpu, &m) : modrm_read16(cpu, &m);
                uint32_t high = op32 ? cpu->edx : cpu->dx;
                /* A high half >= divisor cannot produce a fitting quotient. */
                if (!val || high >= val) {
                    (void)cpu_deliver_exception(vm, 0, insn_eip, 0, false);
                    break;
                }
                uint64_t dividend = op32 ? ((uint64_t)high << 32) | cpu->eax
                                        : ((uint64_t)high << 16) | cpu->ax;
                uint32_t quotient = (uint32_t)(dividend / val);
                uint32_t remainder = (uint32_t)(dividend % val);
                if (op32) {
                    cpu->eax = quotient;
                    cpu->edx = remainder;
                } else {
                    cpu->ax = (uint16_t)quotient;
                    cpu->dx = (uint16_t)remainder;
                }
                break;
            }
            case 7: { /* IDIV r/m16/32 (signed) */
                int64_t divisor = op32 ? (int32_t)modrm_read32(cpu, &m)
                                       : (int16_t)modrm_read16(cpu, &m);
                uint64_t bits = op32 ? ((uint64_t)cpu->edx << 32) | cpu->eax
                                    : ((uint32_t)cpu->dx << 16) | cpu->ax;
                int64_t dividend = op32 ? (int64_t)bits : (int32_t)bits;
                /* Avoid host signed-division overflow before checking the
                 * narrower architectural quotient. Guest faults keep AX/DX. */
                if (!divisor || (op32 && bits == (1ULL << 63) && divisor == -1)) {
                    (void)cpu_deliver_exception(vm, 0, insn_eip, 0, false);
                    break;
                }
                int64_t quotient = dividend / divisor;
                int64_t minimum = op32 ? -2147483648LL : -32768;
                int64_t maximum = op32 ? 2147483647LL : 32767;
                if (quotient < minimum || quotient > maximum) {
                    (void)cpu_deliver_exception(vm, 0, insn_eip, 0, false);
                    break;
                }
                if (op32) {
                    cpu->eax = (uint32_t)quotient;
                    cpu->edx = (uint32_t)(dividend % divisor);
                } else {
                    cpu->ax = (uint16_t)quotient;
                    cpu->dx = (uint16_t)(dividend % divisor);
                }
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
        case 0xFB: /* STI */
            cpu_interrupt_flag(cpu, opcode == 0xFB, insn_eip);
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
            /* ICEBP is a one-byte debug trap. Its saved IP points to the
             * instruction following F1, like the processor's #DB trap. */
            if (lock_prefix) (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
            else if (cpu8086_uses_guest_idt(cpu))
                (void)cpu8086_deliver_guest_interrupt(vm, 1, CPU_EVENT_PRIVILEGED_TRAP,
                                                       cpu->eip, insn_eip, 0, false);
            else (void)cpu_deliver_exception(vm, 1, cpu->eip, 0, false);
            break;

        /* ════════════════════════════════════════════════════════════
         *  Group 4: INC/DEC r/m8  (0xFE)
         * ════════════════════════════════════════════════════════════ */
        case 0xFE: {
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
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
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = m.reg_field <= 1u;
            if (m.reg_field == 7 ||
                (m.is_reg && (m.reg_field == 3 || m.reg_field == 5)) ||
                (lock_prefix && (m.is_reg || m.reg_field > 1))) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            switch (m.reg_field) {
            case 0: /* INC r/m16/32 */
            case 1: { /* DEC r/m16/32 */
                bool cf = get_flag(cpu, FLAG_CF);
                if (op32) {
                    uint32_t val = modrm_read32(cpu, &m);
                    val = m.reg_field ? alu_sub32(cpu, val, 1)
                                      : alu_add32(cpu, val, 1);
                    modrm_write32(cpu, &m, val);
                } else {
                    uint16_t val = modrm_read16(cpu, &m);
                    val = m.reg_field ? alu_dec16(cpu, val)
                                      : alu_inc16(cpu, val);
                    modrm_write16(cpu, &m, val);
                }
                set_flag(cpu, FLAG_CF, cf);
                break;
            }
            case 2: { /* CALL r/m16/32 (near indirect) */
                uint32_t target = op32 ? modrm_read32(cpu, &m)
                                       : modrm_read16(cpu, &m);
                cpu_near_call(cpu, target, op32 ? 4u : 2u, fetch->limit, insn_eip);
                break;
            }
            case 3: { /* CALL FAR m16:16/32 (indirect) */
                unsigned width = op32 ? 4u : 2u;
                uint8_t bytes[6];
                if (!cpu_operand_record(cpu, &m, width + 2u, false, insn_eip, bytes))
                    goto instruction_complete;
                uint32_t offset = 0;
                for (unsigned i = 0; i < width; i++) offset |= (uint32_t)bytes[i] << (8u * i);
                uint16_t seg = bytes[width] | ((uint16_t)bytes[width + 1u] << 8);
                cpu_far_transfer(cpu, seg, offset, width, true, false, insn_eip);
                break;
            }
            case 4: { /* JMP r/m16/32 (near indirect) */
                uint32_t target = op32 ? modrm_read32(cpu, &m) : modrm_read16(cpu, &m);
                (void)cpu_near_jump(cpu, target, op32, fetch->limit, insn_eip);
                break;
            }
            case 5: { /* JMP FAR m16:16/32 (indirect) */
                unsigned width = op32 ? 4u : 2u;
                uint8_t bytes[6];
                if (!cpu_operand_record(cpu, &m, width + 2u, false, insn_eip, bytes))
                    goto instruction_complete;
                uint32_t offset = 0;
                for (unsigned i = 0; i < width; i++) offset |= (uint32_t)bytes[i] << (8u * i);
                uint16_t seg = bytes[width] | ((uint16_t)bytes[width + 1u] << 8);
                cpu_far_transfer(cpu, seg, offset, width, false, false, insn_eip);
                break;
            }
            case 6: { /* PUSH r/m16/32: evaluate before changing SP/ESP. */
                unsigned width = op32 ? 4u : 2u;
                cpu_stack_access_t stack;
                modrm_t destination;
                if (!cpu_modrm_prepare(cpu, &m, width, false, insn_eip) ||
                    !cpu_stack_begin(cpu, &stack, false, insn_eip)) goto instruction_complete;
                uint32_t next = cpu_stack_next_esp(&stack, cpu->esp, cpu->esp - width);
                if (!cpu_stack_slot(cpu, &stack, next & stack.mask, width, true, true,
                                     insn_eip, &destination)) goto instruction_complete;
                uint32_t val = cpu_modrm_read(cpu, &m, width);
                cpu_modrm_write(cpu, &destination, width, val);
                cpu->esp = next;
                break;
            }
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  ARPL (0x63) — Adjust RPL of selector
         * ════════════════════════════════════════════════════════════ */
        case 0x63: {
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            m.read_modify_write = true;
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
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (m.is_reg || lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            unsigned width = op32 ? 4u : 2u;
            uint8_t bytes[8];
            if (!cpu_operand_record(cpu, &m, width * 2u, false, insn_eip, bytes))
                goto instruction_complete;
            uint32_t lower = 0, upper = 0;
            for (unsigned i = 0; i < width; i++) {
                lower |= (uint32_t)bytes[i] << (8u * i);
                upper |= (uint32_t)bytes[width + i] << (8u * i);
            }
            int32_t idx = op32 ? (int32_t)*reg32_ptr(cpu, m.reg_field)
                              : (int16_t)*reg16_ptr(cpu, m.reg_field);
            int32_t lo = op32 ? (int32_t)lower : (int16_t)lower;
            int32_t hi = op32 ? (int32_t)upper : (int16_t)upper;
            if (idx < lo || idx > hi) {
                (void)cpu_deliver_exception(vm, 5, insn_eip, 0, false);
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  PUSHA / POPA (186+)
         * ════════════════════════════════════════════════════════════ */
        case 0x60: { /* PUSHA / PUSHAD */
            cpu_stack_all(cpu, false, op32 ? 4u : 2u, lock_prefix, insn_eip);
            break;
        }
        case 0x61: { /* POPA / POPAD */
            cpu_stack_all(cpu, true, op32 ? 4u : 2u, lock_prefix, insn_eip);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  INS/OUTS (186+) — String I/O
         * ════════════════════════════════════════════════════════════ */
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
            if (lock_prefix) {
                (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
                break;
            }
            string_pending = cpu_string_port_io(cpu, !(opcode & 2u),
                                !(opcode & 1u) ? 1u : op32 ? 4u : 2u,
                                adr32, insn_eip);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  IMUL r16, r/m16, imm (186+)
         * ════════════════════════════════════════════════════════════ */
        case 0x69: { /* IMUL r16, r/m16, imm16 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                int32_t imm = (int32_t)CPU_FETCH32(cpu);
                int32_t src = (int32_t)modrm_read32(cpu, &m);
                int64_t result = (int64_t)src * (int64_t)imm;
                *reg32_ptr(cpu, m.reg_field) = (uint32_t)result;
                set_flag(cpu, FLAG_CF, result != (int32_t)result);
                set_flag(cpu, FLAG_OF, result != (int32_t)result);
            } else {
                int16_t imm = (int16_t)CPU_FETCH16(cpu);
                int16_t src = (int16_t)modrm_read16(cpu, &m);
                int32_t result = (int32_t)src * (int32_t)imm;
                *reg16_ptr(cpu, m.reg_field) = (uint16_t)result;
                set_flag(cpu, FLAG_CF, result != (int16_t)result);
                set_flag(cpu, FLAG_OF, result != (int16_t)result);
            }
            break;
        }
        case 0x6B: { /* IMUL r16, r/m16, imm8 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            if (op32) {
                int32_t imm = (int32_t)(int8_t)CPU_FETCH8(cpu);
                int32_t src = (int32_t)modrm_read32(cpu, &m);
                int64_t result = (int64_t)src * (int64_t)imm;
                *reg32_ptr(cpu, m.reg_field) = (uint32_t)result;
                set_flag(cpu, FLAG_CF, result != (int32_t)result);
                set_flag(cpu, FLAG_OF, result != (int32_t)result);
            } else {
                int16_t imm = (int16_t)(int8_t)CPU_FETCH8(cpu);
                int16_t src = (int16_t)modrm_read16(cpu, &m);
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
                CPU_STACK_PUSH(CPU_FETCH32(cpu));
            else
                CPU_STACK_PUSH(CPU_FETCH16(cpu));
            break;
        case 0x6A: { /* PUSH imm8 (sign-extended to 16/32) */
            int8_t val = (int8_t)CPU_FETCH8(cpu);
            if (op32)
                CPU_STACK_PUSH((uint32_t)(int32_t)val);
            else
                CPU_STACK_PUSH((uint16_t)(int16_t)val);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  LES/LDS — Load far pointer
         * ════════════════════════════════════════════════════════════ */
        case 0xC4: /* LES reg16/32, m16:16/32 */
        case 0xC5: { /* LDS reg16/32, m16:16/32 */
            modrm_byte = CPU_FETCH8(cpu);
            m = CPU_DECODE_MODRM(cpu, modrm_byte);
            cpu_load_far_pointer(cpu, &m, opcode == 0xC4 ? 0u : 3u,
                                  op32, lock_prefix, insn_eip);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  AAM / AAD  (0xD4 / 0xD5)
         * ════════════════════════════════════════════════════════════ */
        case 0xD4: { /* AAM imm8 */
            uint8_t base = CPU_FETCH8(cpu);
            if (base == 0) {
                (void)cpu_deliver_exception(vm, 0, insn_eip, 0, false);
                break;
            }
            uint8_t al = cpu->al;
            cpu->ah = al / base;
            cpu->al = al % base;
            update_flags_logic8(cpu, cpu->al);
            break;
        }
        case 0xD5: { /* AAD imm8 */
            uint8_t base = CPU_FETCH8(cpu);
            cpu->al = (uint8_t)(cpu->ah * base + cpu->al);
            cpu->ah = 0;
            update_flags_logic8(cpu, cpu->al);
            break;
        }
        case 0xD6: /* SALC (undocumented: AL = CF ? 0xFF : 0x00) */
            cpu->al = (cpu->flags & FLAG_CF) ? 0xFF : 0x00;
            break;

        /* ════════════════════════════════════════════════════════════
         *  FPU escape opcodes (0xD8-0xDF)
         * ════════════════════════════════════════════════════════════ */
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
            /* A guest x87 emulator can handle vector 7 and retry or skip. */
            (void)cpu_deliver_exception(vm, 7, insn_eip, 0, false);
            break;
        }

        /* ════════════════════════════════════════════════════════════
         *  Unknown opcode
         * ════════════════════════════════════════════════════════════ */
        default:
            (void)cpu_deliver_exception(vm, 6, insn_eip, 0, false);
            break;

        } /* switch (opcode) */

        goto instruction_complete;

fetch_fault:
        cpu->eip = insn_eip;
        if (cpu->rep_compare.active) cpu->eflags = cpu->rep_compare.eflags;
        if (fetch->fault.raised) cpu_raise_page_fault(cpu, insn_eip, &fetch->fault);
        else (void)cpu_deliver_exception(vm, 13, insn_eip, 0, true);

instruction_complete:
        if (cpu->irq_shadow) cpu->irq_shadow--;
        /* Increment instruction counter */
        cpu->insn_count++;
execution_complete:
        if (vm->step_limit_reached) cpu_stop_for_step_limit(vm);
        if (single_step) break;
        /* Return to the suspended host before polling IRQs can replace the
         * destination just validated by its exception-return stub. */
        if (vm->interpreter_stop_active && vm->interpreter_stop_signal &&
            *vm->interpreter_stop_signal && vm->current_psp == vm->interpreter_stop_psp)
            continue;

        /* Trace PM instructions for debugging */
        if (cpu->pm_cs_loaded && cpu->insn_count >= 38640 && cpu->insn_count < 38670) {
            serial_puts("[PM] #");
            serial_putdec(cpu->insn_count);
            serial_puts(" op=");
            serial_puthex(opcode, 2);
            serial_puts(" CS:EIP=");
            serial_puthex(cpu->cs, 4);
            serial_puts(":");
            serial_puthex(cpu->eip, 8);
            serial_puts(" ESP=");
            serial_puthex(cpu->esp, 8);
            serial_puts(" EAX=");
            serial_puthex(cpu->eax, 8);
            serial_puts(" EBX=");
            serial_puthex(cpu->ebx, 8);
            serial_puts(" ECX=");
            serial_puthex(cpu->ecx, 8);
            serial_puts(" DS=");
            serial_puthex(cpu->ds, 4);
            serial_puts("\n");
        }

        /* Detect CS changes in PM */
        if (cpu->pm_cs_loaded && cpu->cs != prev_cs && cpu->insn_count < 1000000) {
            serial_puts("[CS] ");
            serial_puthex(prev_cs, 4);
            serial_puts(" -> ");
            serial_puthex(cpu->cs, 4);
            serial_puts(" at #");
            serial_putdec(cpu->insn_count);
            serial_puts(" op=");
            serial_puthex(opcode, 2);
            serial_puts(" EIP=");
            serial_puthex(cpu->eip, 8);
            serial_puts("\n");
            prev_cs = cpu->cs;
        }
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
        if ((previous_insns >> 14) != (cpu->insn_count >> 14)) {
            dos_vga_flush(vm);
            dos_vga_mode13_present();
            (void)cpu8086_service_interrupts(vm);
        } else if (string_pending || (had_irq_shadow && !cpu->irq_shadow))
            (void)cpu8086_service_interrupts(vm);

        /* Milestones for debugging DOS4GW init */
        if (previous_insns < 5000 && cpu->insn_count >= 5000) {
            serial_puts("[8086] 5K insn, CS:EIP=");
            serial_puthex(cpu->cs, 4);
            serial_puts(":");
            serial_puthex(cpu->eip, 8);
            /* Dump 16 bytes at the loop */
            uint32_t la = dos_addr(vm, cpu->cs, cpu->eip - 8);
            serial_puts(" bytes[-8..+8]:");
            for (int i = 0; i < 16; i++) {
                serial_puts(" ");
                serial_puthex(dos_mem_read8(vm, la + i), 2);
            }
            serial_puts("\n");
        }

        /* Periodic status log every 100M instructions */
        if (previous_insns / 100000000 != cpu->insn_count / 100000000) {
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

    } /* execution loop */

    int exit_code = cpu->exit_code;
    if (!single_step) vm->native_resume_armed = false;
    return exit_code;
}

int cpu8086_run(dos_vm_t *vm)
{
    return cpu8086_run_internal(vm, false);
}

bool cpu8086_run_one(dos_vm_t *vm)
{
    if (!vm || !vm->cpu || !vm->cpu->running || vm->cpu->halted)
        return false;
    uint64_t before = vm->cpu->insn_count;
    (void)cpu8086_run_internal(vm, true);
    return vm->cpu->running && vm->cpu->insn_count == before + 1u;
}

static bool cpu_run_until(dos_vm_t *vm, bool protected_mode,
                           uint16_t stop_cs, uint32_t stop_ip, const bool *signal)
{
    if (!vm || !vm->cpu)
        return false;

    uint64_t saved_resume_jmpbuf[9];
    for (unsigned i = 0; i < 9; i++)
        saved_resume_jmpbuf[i] = vm->native_resume_jmpbuf[i];

    bool saved_resume_armed = vm->native_resume_armed;
    bool saved_stop_active = vm->interpreter_stop_active;
    bool saved_stop_reached = vm->interpreter_stop_reached;
    uint16_t saved_stop_cs = vm->interpreter_stop_cs;
    uint32_t saved_stop_ip = vm->interpreter_stop_ip;
    uint16_t saved_stop_psp = vm->interpreter_stop_psp;
    bool saved_stop_protected = vm->interpreter_stop_protected;
    const bool *saved_stop_signal = vm->interpreter_stop_signal;
    bool saved_running = vm->cpu->running;

    vm->interpreter_stop_cs = stop_cs;
    vm->interpreter_stop_ip = stop_ip;
    vm->interpreter_stop_psp = vm->current_psp;
    vm->interpreter_stop_protected = protected_mode;
    vm->interpreter_stop_reached = false;
    vm->interpreter_stop_active = true;
    vm->interpreter_stop_signal = signal;
    vm->cpu->running = true;

    (void)cpu8086_run(vm);
    bool reached = vm->interpreter_stop_reached;

    vm->interpreter_stop_active = saved_stop_active;
    vm->interpreter_stop_reached = saved_stop_reached;
    vm->interpreter_stop_cs = saved_stop_cs;
    vm->interpreter_stop_ip = saved_stop_ip;
    vm->interpreter_stop_psp = saved_stop_psp;
    vm->interpreter_stop_protected = saved_stop_protected;
    vm->interpreter_stop_signal = saved_stop_signal;
    vm->native_resume_armed = saved_resume_armed;
    for (unsigned i = 0; i < 9; i++)
        vm->native_resume_jmpbuf[i] = saved_resume_jmpbuf[i];

    if (reached)
        vm->cpu->running = saved_running;
    return reached;
}

bool cpu8086_run_until(dos_vm_t *vm, bool protected_mode,
                       uint16_t stop_cs, uint32_t stop_ip)
{
    return cpu_run_until(vm, protected_mode, stop_cs, stop_ip, NULL);
}

bool cpu8086_run_until_signal(dos_vm_t *vm, const bool *signal)
{
    return signal && cpu_run_until(vm, true, 0, 0, signal);
}

bool cpu8086_run_until_real(dos_vm_t *vm, uint16_t stop_cs,
                            uint16_t stop_ip)
{
    return cpu8086_run_until(vm, false, stop_cs, stop_ip);
}
