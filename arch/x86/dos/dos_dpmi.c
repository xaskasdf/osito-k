/*
 * OsitoK -- DPMI 0.9 Host Implementation
 *
 * DOS Protected Mode Interface host for 32-bit DOS extenders.
 * Provides LDT management, extended memory allocation, real-mode
 * interrupt simulation, and mode switching for DOS4GW-style clients.
 * This is the path DOOM takes to run: INT 2Fh/1687h detects DPMI,
 * the entry stub switches to protected mode, and INT 31h services
 * handle memory allocation and real-mode INT callbacks.
 */

#include "cpu8086.h"
#include "dos_hostmem.h"
#include "dos_dpmi.h"
#include "dos_io.h"
#include "dos_mem.h"
#include "dos_paging.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);

/* DOS INT dispatch (for real-mode interrupt simulation) */
extern void dos_int21_dispatch(dos_vm_t *vm);
extern void dos_int10_video(dos_vm_t *vm);
extern void dos_int16_keyboard(dos_vm_t *vm);
extern void dos_int1a_timer(dos_vm_t *vm);
extern void dos_int_dispatch(dos_vm_t *vm, uint8_t int_num);

/* ── Helper: zero memory (no libc) ─────────────────────────────── */

static dpmi_paging_t dpmi_current_paging(const cpu8086_state_t *cpu)
{
    return (dpmi_paging_t){ cpu->cr0, cpu->cr3 };
}

static void dpmi_apply_paging(cpu8086_state_t *cpu, dpmi_paging_t paging)
{
    cpu->cr3 = paging.cr3;
    cpu->cr0 = paging.cr0;
}

static void dpmi_zero(void *dst, uint64_t len)
{
    uint8_t *p = (uint8_t *)dst;
    for (uint64_t i = 0; i < len; i++)
        p[i] = 0;
}

static uint32_t dpmi_memory_limit(const dos_vm_t *vm)
{
    if (!vm) return 0;
    uint32_t system_memory = vm->system_mem_size
                           ? vm->system_mem_size : vm->total_mem_size;
    if (system_memory > vm->total_mem_size)
        system_memory = vm->total_mem_size;
    return system_memory < DOS_TOTAL_MEM ? system_memory : DOS_TOTAL_MEM;
}

static uint32_t dpmi_ext_limit(const dos_vm_t *vm)
{
    uint32_t total = dpmi_memory_limit(vm);
    if (total <= DPMI_EXT_BASE + DPMI_HOST_STACK_AREA)
        return DPMI_EXT_BASE;
    return total - DPMI_HOST_STACK_AREA;
}

static bool dpmi_ext_page_used(const dpmi_state_t *dpmi, uint32_t page)
{
    return (dpmi->ext_page_bitmap[page >> 3] &
            (uint8_t)(1u << (page & 7u))) != 0;
}

static void dpmi_ext_page_set(dpmi_state_t *dpmi, uint32_t page, bool used)
{
    uint8_t mask = (uint8_t)(1u << (page & 7u));
    if (used)
        dpmi->ext_page_bitmap[page >> 3] |= mask;
    else
        dpmi->ext_page_bitmap[page >> 3] &= (uint8_t)~mask;
}

uint32_t dpmi_ext_total_pages(const dos_vm_t *vm)
{
    uint32_t limit = dpmi_ext_limit(vm);
    if (limit <= DPMI_EXT_BASE) return 0;
    uint32_t pages = (limit - DPMI_EXT_BASE) / DPMI_EXT_PAGE_SIZE;
    return pages < DPMI_EXT_MAX_PAGES ? pages : DPMI_EXT_MAX_PAGES;
}

uint32_t dpmi_ext_free_page_count(const dos_vm_t *vm)
{
    if (!vm) return 0;
    uint32_t total = dpmi_ext_total_pages(vm);
    uint32_t free_pages = 0;
    for (uint32_t page = 0; page < total; page++)
        if (!dpmi_ext_page_used(&vm->dpmi, page)) free_pages++;
    return free_pages;
}

uint32_t dpmi_ext_largest_free_page_count(const dos_vm_t *vm)
{
    if (!vm) return 0;
    uint32_t total = dpmi_ext_total_pages(vm);
    uint32_t largest = 0;
    uint32_t run = 0;
    for (uint32_t page = 0; page < total; page++) {
        if (dpmi_ext_page_used(&vm->dpmi, page)) {
            run = 0;
        } else {
            run++;
            if (run > largest) largest = run;
        }
    }
    return largest;
}

static bool dpmi_ext_claim_pages(dos_vm_t *vm, uint32_t first,
                                 uint32_t count)
{
    if (!vm || !count) return false;
    uint32_t total = dpmi_ext_total_pages(vm);
    if (first >= total || count > total - first) return false;
    for (uint32_t page = 0; page < count; page++)
        if (dpmi_ext_page_used(&vm->dpmi, first + page)) return false;
    for (uint32_t page = 0; page < count; page++)
        dpmi_ext_page_set(&vm->dpmi, first + page, true);

    uint32_t base = DPMI_EXT_BASE + first * DPMI_EXT_PAGE_SIZE;
    dpmi_zero(vm->mem + base, (uint64_t)count * DPMI_EXT_PAGE_SIZE);
    return true;
}

uint32_t dpmi_ext_alloc_pages(dos_vm_t *vm, uint32_t count, bool high)
{
    if (!vm || !vm->mem || !count) return 0;
    uint32_t total = dpmi_ext_total_pages(vm);
    if (count > total) return 0;

    if (high) {
        for (uint32_t first = total - count + 1u; first-- > 0;) {
            if (dpmi_ext_claim_pages(vm, first, count))
                return DPMI_EXT_BASE + first * DPMI_EXT_PAGE_SIZE;
        }
    } else {
        for (uint32_t first = 0; first <= total - count; first++) {
            if (dpmi_ext_claim_pages(vm, first, count))
                return DPMI_EXT_BASE + first * DPMI_EXT_PAGE_SIZE;
        }
    }
    return 0;
}

bool dpmi_ext_free_pages(dos_vm_t *vm, uint32_t base, uint32_t count)
{
    if (!vm || !vm->mem || !count || base < DPMI_EXT_BASE ||
        (base & (DPMI_EXT_PAGE_SIZE - 1u)))
        return false;

    uint32_t first = (base - DPMI_EXT_BASE) / DPMI_EXT_PAGE_SIZE;
    uint32_t total = dpmi_ext_total_pages(vm);
    if (first >= total || count > total - first) return false;
    for (uint32_t page = 0; page < count; page++)
        if (!dpmi_ext_page_used(&vm->dpmi, first + page)) return false;

    dpmi_zero(vm->mem + base, (uint64_t)count * DPMI_EXT_PAGE_SIZE);
    for (uint32_t page = 0; page < count; page++)
        dpmi_ext_page_set(&vm->dpmi, first + page, false);
    return true;
}

/* ── Helper: build a descriptor ────────────────────────────────── */

static void dpmi_build_desc(dpmi_descriptor_t *d, uint32_t base,
                            uint32_t limit, uint8_t access, uint8_t flags)
{
    dpmi_zero(d, sizeof(*d));
    dpmi_desc_set_base(d, base);
    d->access = access;
    d->flags_lim = flags;
    dpmi_desc_set_limit(d, limit);
}

static bool dpmi_limit_is_representable(uint32_t limit)
{
    return limit <= 0xFFFFFU || (limit & 0xFFFU) == 0xFFFU;
}

/* ══════════════════════════════════════════════════════════════════
 * 1. dpmi_init -- Initialize DPMI host state
 * ══════════════════════════════════════════════════════════════════ */

void dpmi_init(dos_vm_t *vm)
{
    dpmi_state_t *dpmi = &vm->dpmi;

    /* Zero entire DPMI state */
    dpmi_zero(dpmi, sizeof(*dpmi));

    dpmi->next_free_index = DPMI_SPECIFIC_DESCRIPTOR_COUNT;
    dpmi->next_handle = 1;
    dpmi->virtual_interrupts_enabled = true;
    dpmi->real_mode_paging = vm->cpu ? dpmi_current_paging(vm->cpu)
                                    : (dpmi_paging_t){0};
    dpmi->real_mode_paging.cr0 &= ~(1u | DOS_CR0_PG);
    dpmi->suspended_paging = dpmi->real_mode_paging;
    dpmi->suspended_paging.cr0 |= 1u;

    /* Write DPMI entry stub in ROM area at F000:0100
     * CD FE   INT 0xFE
     * CB      RETF
     */
    uint32_t stub_addr = dos_linear(DPMI_ENTRY_SEG, DPMI_ENTRY_OFF);
    if (stub_addr + 3 <= vm->total_mem_size) {
        vm->mem[stub_addr + 0] = 0xCD;  /* INT */
        vm->mem[stub_addr + 1] = 0xFE;  /* 0xFE */
        vm->mem[stub_addr + 2] = 0xCB;  /* RETF */
    }

    vm->dpmi.real_mode_stack = (dpmi_stack_t){ DPMI_ENTRY_SEG, DPMI_RM_STACK_TOP };

    /* 0305h is a FAR-call target; the private gate preserves all registers.
     * 0306h is a FAR-jump target; INT FC consumes its register contract and
     * resumes directly at the requested destination in the other mode. */
    uint32_t save_stub = dos_linear(DPMI_ENTRY_SEG, DPMI_SAVE_STATE_OFF);
    uint32_t ms_stub = dos_linear(DPMI_ENTRY_SEG, DPMI_RAW_SWITCH_OFF);
    uint32_t exception_stub = dos_linear(DPMI_ENTRY_SEG,
                                         DPMI_EXCEPTION_RETURN_OFF);
    uint32_t callback_return_stub = dos_linear(DPMI_ENTRY_SEG,
                                               DPMI_CALLBACK_RETURN_OFF);
    uint32_t pm_reflect_base = dos_linear(DPMI_ENTRY_SEG,
                                          DPMI_PM_REFLECT_BASE_OFF);
    uint32_t pm_hw_reflect_base = dos_linear(
        DPMI_ENTRY_SEG, DPMI_PM_HW_REFLECT_BASE_OFF);
    if (ms_stub + 2 <= vm->total_mem_size) {
        vm->mem[save_stub] = 0xCD;
        vm->mem[save_stub + 1u] = DPMI_RAW_SWITCH_INT;
        vm->mem[save_stub + 2u] = 0xCB;            /* RETF */
        vm->mem[ms_stub] = 0xCD;                   /* INT imm8 */
        vm->mem[ms_stub + 1] = DPMI_RAW_SWITCH_INT;
    }
    if (exception_stub + 3 <= vm->total_mem_size) {
        vm->mem[exception_stub] = 0xCD;             /* INT imm8 */
        vm->mem[exception_stub + 1] = DPMI_EXCEPTION_RETURN_INT;
        vm->mem[exception_stub + 2] = 0xF4;         /* must not return */
    }
    uint32_t extended_return = dos_linear(DPMI_ENTRY_SEG,
                                           DPMI_EXCEPTION_EXT_RETURN_OFF);
    if (extended_return + 3u <= vm->total_mem_size) {
        vm->mem[extended_return] = 0xCD;
        vm->mem[extended_return + 1u] = DPMI_EXCEPTION_RETURN_INT;
        vm->mem[extended_return + 2u] = 0xF4;
    }
    uint32_t interrupt_return = dos_linear(DPMI_ENTRY_SEG,
                                           DPMI_INTERRUPT_RETURN_OFF);
    if (interrupt_return + 3u <= vm->total_mem_size) {
        vm->mem[interrupt_return] = 0xCD;
        vm->mem[interrupt_return + 1u] = DPMI_EXCEPTION_RETURN_INT;
        vm->mem[interrupt_return + 2u] = 0xF4;
    }
    if (callback_return_stub + 3 <= vm->total_mem_size) {
        vm->mem[callback_return_stub] = 0xCD;        /* INT imm8 */
        vm->mem[callback_return_stub + 1] = DPMI_CALLBACK_RETURN_INT;
        vm->mem[callback_return_stub + 2] = 0xF4;    /* must not return */
    }
    uint32_t pm_reflect_size = 256u * DPMI_PM_REFLECT_STUB_SIZE;
    if (pm_reflect_base <= vm->total_mem_size &&
        pm_reflect_size <= vm->total_mem_size - pm_reflect_base) {
        for (uint16_t int_num = 0; int_num < 256u; int_num++) {
            uint32_t stub = pm_reflect_base +
                            int_num * DPMI_PM_REFLECT_STUB_SIZE;
            vm->mem[stub] = 0xCD;  /* INT F9h enters the host reflector. */
            vm->mem[stub + 1u] = DPMI_DEFAULT_REFLECT_INT;
            if (int_num <= 7u) {
                vm->mem[stub + 2u] = 0xFB;  /* STI */
                vm->mem[stub + 3u] = 0xCF;  /* IRET(D) */
            } else {
                vm->mem[stub + 2u] = 0xCF;  /* IRET(D) */
                vm->mem[stub + 3u] = 0x90;  /* NOP */
            }
        }
    }
    if (pm_hw_reflect_base <= vm->total_mem_size &&
        pm_reflect_size <= vm->total_mem_size - pm_hw_reflect_base) {
        for (uint16_t int_num = 0; int_num < 256u; int_num++) {
            uint32_t stub = pm_hw_reflect_base +
                            int_num * DPMI_PM_REFLECT_STUB_SIZE;
            vm->mem[stub] = 0xCD;       /* INT F9h */
            vm->mem[stub + 1u] = DPMI_DEFAULT_REFLECT_INT;
            vm->mem[stub + 2u] = 0xFB; /* host IRQ handler reenables VIF */
            vm->mem[stub + 3u] = 0xCF; /* IRET(D) */
        }
    }

    uint32_t pm_exception_base = dos_linear(DPMI_ENTRY_SEG,
                                            DPMI_PM_EXCEPTION_BASE_OFF);
    uint32_t pm_exception_size = 32u * DPMI_PM_EXCEPTION_STUB_SIZE;
    if (pm_exception_base <= vm->total_mem_size &&
        pm_exception_size <= vm->total_mem_size - pm_exception_base) {
        for (uint16_t vector = 0; vector < 32u; vector++) {
            uint32_t stub = pm_exception_base +
                            vector * DPMI_PM_EXCEPTION_STUB_SIZE;
            vm->mem[stub] = 0xCD;
            vm->mem[stub + 1u] = DPMI_DEFAULT_REFLECT_INT;
            vm->mem[stub + 2u] = 0xCB; /* RETF to the exception return stub */
            vm->mem[stub + 3u] = 0xF4;
        }
    }

    /* Immutable per-vector trampolines are safe to cache in the JIT and
     * cannot be overwritten by a nested simulation of another interrupt. */
    uint32_t rm_int_base = dos_linear(DPMI_ENTRY_SEG, DPMI_RM_INT_BASE_OFF);
    uint32_t rm_int_size = 256u * DPMI_RM_INT_STUB_SIZE;
    if (rm_int_base <= vm->total_mem_size &&
        rm_int_size <= vm->total_mem_size - rm_int_base) {
        for (uint16_t int_num = 0; int_num < 256u; int_num++) {
            uint32_t stub = rm_int_base + int_num * DPMI_RM_INT_STUB_SIZE;
            vm->mem[stub] = 0xCD;
            vm->mem[stub + 1u] = (uint8_t)int_num;
            vm->mem[stub + 2u] = 0xCB;
            vm->mem[stub + 3u] = 0x90;
        }
    }

    serial_puts("[DPMI] Host initialized, entry at F000:0100,"
                " save F000:0110, mode-switch F000:0118,"
                " exception-return F000:0120, callback-return F000:0140,"
                " reflectors F000:1000/1400\n");
}

/* ══════════════════════════════════════════════════════════════════
 * 2. dpmi_translate -- Selector:offset to linear address
 * ══════════════════════════════════════════════════════════════════ */

static bool dpmi_descriptor_at(dos_vm_t *vm, uint32_t address,
                               dpmi_descriptor_t *descriptor, dos_page_fault_t *fault)
{
    dos_page_translation_t pages[2];
    uint32_t addresses[sizeof(*descriptor)];
    unsigned done = 0, count = 0;
    while (done < sizeof(*descriptor)) {
        uint32_t linear = address + done;
        unsigned size = DOS_PAGE_SIZE - (linear & (DOS_PAGE_SIZE - 1u));
        if (size > sizeof(*descriptor) - done) size = sizeof(*descriptor) - done;
        /* GDT/LDT references are implicit supervisor reads, even at CPL3. */
        if (!dos_page_probe(vm, linear, DOS_PAGE_READ, &pages[count], fault)) return false;
        for (unsigned i = 0; i < size; i++) addresses[done + i] = pages[count].physical + i;
        done += size;
        count++;
    }
    for (unsigned i = 0; i < count; i++) dos_page_commit(vm, &pages[i], false);
    dpmi_descriptor_t result;
    for (unsigned i = 0; i < sizeof(result); i++)
        ((uint8_t *)&result)[i] = dos_mem_read8(vm, addresses[i]);
    *descriptor = result;
    return true;
}

bool dpmi_lookup_descriptor(dos_vm_t *vm, uint16_t selector,
                             dpmi_descriptor_ref_t *reference, dos_page_fault_t *fault)
{
    if (fault) *fault = (dos_page_fault_t){0};
    if (!vm || !vm->cpu || !vm->mem || !reference || (selector & ~3u) == 0)
        return false;
    dpmi_descriptor_ref_t result = { .selector = selector };
    uint32_t table = vm->cpu->gdtr.base;
    uint16_t index = selector >> 3;
    if ((selector & 4u) && vm->cpu->host_ldt) {
        if (index >= DPMI_MAX_DESCRIPTORS ||
            vm->dpmi.descriptor_state[index] == DPMI_DESC_FREE)
            return false;
        result.descriptor = vm->dpmi.ldt[index];
        result.internal = true;
        *reference = result;
        return true;
    }
    uint32_t offset = (uint32_t)index * sizeof(dpmi_descriptor_t);
    uint32_t limit = vm->cpu->gdtr.limit;
    if (selector & 4u) {
        const cpu_system_segment_t *ldt = &vm->cpu->ldt_cache;
        if (!ldt->valid || !(ldt->descriptor.access & DESC_PRESENT) ||
            (ldt->descriptor.access & DESC_SEGMENT) ||
            (ldt->descriptor.access & 0x0Fu) != 0x02u)
            return false;
        table = dpmi_desc_get_base(&ldt->descriptor);
        limit = dpmi_desc_get_limit(&ldt->descriptor);
    }
    if (offset + sizeof(dpmi_descriptor_t) - 1u > limit)
        return false;
    result.linear = table + offset;
    if (!dpmi_descriptor_at(vm, result.linear, &result.descriptor, fault)) return false;
    *reference = result;
    return true;
}

bool dpmi_guest_descriptor(dos_vm_t *vm, uint16_t selector,
                           dpmi_descriptor_t *descriptor)
{
    dpmi_descriptor_ref_t reference;
    if (!descriptor || !dpmi_lookup_descriptor(vm, selector, &reference, NULL)) return false;
    *descriptor = reference.descriptor;
    return true;
}

static bool dpmi_descriptor_set_access_bits(dos_vm_t *vm,
                                             const dpmi_descriptor_ref_t *reference,
                                             uint8_t bits, dos_page_fault_t *fault)
{
    if (fault) *fault = (dos_page_fault_t){0};
    if ((reference->descriptor.access & bits) == bits) return true;
    if (reference->internal) {
        vm->dpmi.ldt[reference->selector >> 3].access |= bits;
        return true;
    }
    dos_page_translation_t translation;
    if (!dos_page_probe(vm, reference->linear + 5u, DOS_PAGE_WRITE,
                        &translation, fault)) return false;
    dos_page_commit(vm, &translation, true);
    dos_mem_write8(vm, translation.physical, reference->descriptor.access | bits);
    return true;
}

bool dpmi_descriptor_set_accessed(dos_vm_t *vm, const dpmi_descriptor_ref_t *reference,
                                  dos_page_fault_t *fault)
{
    return dpmi_descriptor_set_access_bits(vm, reference, DESC_ACCESSED, fault);
}

bool dpmi_descriptor_set_busy(dos_vm_t *vm, const dpmi_descriptor_ref_t *reference,
                              dos_page_fault_t *fault)
{
    return dpmi_descriptor_set_access_bits(vm, reference, 2u, fault);
}

uint32_t dpmi_translate(dos_vm_t *vm, uint16_t selector, uint32_t offset)
{
    cpu8086_state_t *cpu = vm->cpu;
    dpmi_descriptor_t descriptor;
    uint32_t base = dpmi_guest_descriptor(vm, selector, &descriptor)
                  ? dpmi_desc_get_base(&descriptor) : 0;

    uint32_t linear = base + offset;

    /* If paging is enabled (CR0.PG bit 31) and address is out of range,
     * walk the guest's page tables to find the physical address */
    if ((cpu->cr0 & 0x80000000) && linear >= vm->total_mem_size) {
        uint32_t cr3 = cpu->cr3 & 0xFFFFF000;
        if (cr3 < vm->total_mem_size) {
            /* 32-bit paging: PDE (bits 31:22) → PTE (bits 21:12) → offset (bits 11:0) */
            uint32_t pde_idx = (linear >> 22) & 0x3FF;
            uint32_t pde_addr = cr3 + pde_idx * 4;
            if (pde_addr + 3 < vm->total_mem_size) {
                uint32_t pde = dos_mem_read32(vm, pde_addr);
                if (pde & 1) {  /* present */
                    if (pde & 0x80) {
                        /* 4MB large page (PSE) */
                        uint32_t phys = (pde & 0xFFC00000) | (linear & 0x003FFFFF);
                        if (phys < vm->total_mem_size) return phys;
                    } else {
                        /* 4KB page: walk PTE */
                        uint32_t pt_base = pde & 0xFFFFF000;
                        uint32_t pte_idx = (linear >> 12) & 0x3FF;
                        uint32_t pte_addr = pt_base + pte_idx * 4;
                        if (pte_addr + 3 < vm->total_mem_size) {
                            uint32_t pte = dos_mem_read32(vm, pte_addr);
                            if (pte & 1) {  /* present */
                                uint32_t phys = (pte & 0xFFFFF000) | (linear & 0xFFF);
                                if (phys < vm->total_mem_size) return phys;
                            }
                        }
                    }
                }
            }
        }
    }

    if (linear >= vm->total_mem_size)
        linear %= vm->total_mem_size;

    return linear;
}

/* ══════════════════════════════════════════════════════════════════
 * 3. dos_int2f_dispatch -- INT 2Fh multiplex
 * ══════════════════════════════════════════════════════════════════ */

void dos_int2f_dispatch(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    switch (cpu->ax) {
    case 0x1687:
        /* DPMI detection: report DPMI 0.90 available */
        serial_puts("[DPMI] INT 2Fh/1687h: DPMI detected\n");
        cpu->ax = 0x0000;    /* DPMI host present */
        cpu->bx = 0x0001;    /* 32-bit programs supported */
        cpu->cl = 0x03;      /* processor type: 386 */
        cpu->dh = 0x00;      /* version major: 0 */
        cpu->dl = 0x5A;      /* version minor: 90 (0x5A) */
        cpu->si = 0x0000;    /* no private data area needed */
        cpu->es = DPMI_ENTRY_SEG;
        cpu8086_sync_segment(cpu, 0);
        cpu->di = DPMI_ENTRY_OFF;
        cpu->eflags &= ~FLAG_CF;  /* success */
        break;

    default:
        /* Unhandled INT 2Fh function */
        cpu->eflags |= FLAG_CF;
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 5. dpmi_alloc_descriptor -- Allocate an LDT entry
 * ══════════════════════════════════════════════════════════════════ */

static bool dpmi_selector_allocated(const dpmi_state_t *dpmi, uint16_t sel,
                                    uint16_t *index)
{
    if (!(sel & 0x04)) return false;
    uint16_t idx = dpmi_sel_to_index(sel);
    if (idx >= DPMI_MAX_DESCRIPTORS ||
        dpmi->descriptor_state[idx] == DPMI_DESC_FREE)
        return false;
    if (index) *index = idx;
    return true;
}

static bool dpmi_selector_mutable(const dpmi_state_t *dpmi, uint16_t sel,
                                  uint16_t *index)
{
    uint16_t idx;
    if (!dpmi_selector_allocated(dpmi, sel, &idx) ||
        dpmi->descriptor_state[idx] != DPMI_DESC_MUTABLE)
        return false;
    if (index) *index = idx;
    return true;
}

static bool dpmi_selector_is_code(const dpmi_state_t *dpmi, uint16_t sel)
{
    uint16_t idx;
    if (!dpmi_selector_allocated(dpmi, sel, &idx))
        return false;

    uint8_t access = dpmi->ldt[idx].access;
    uint8_t required = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                       DESC_CODE | DESC_READABLE;
    return (access & required) == required && !(access & 0x04);
}

static bool dpmi_selector_is_data(const dpmi_state_t *dpmi, uint16_t sel,
                                  bool require_writable)
{
    if ((sel & ~3u) == 0)
        return !require_writable;

    uint16_t idx;
    if (!dpmi_selector_allocated(dpmi, sel, &idx))
        return false;

    uint8_t access = dpmi->ldt[idx].access;
    uint8_t required = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT;
    if ((access & required) != required)
        return false;
    if (access & DESC_CODE)
        return !require_writable && (access & DESC_READABLE);
    return !require_writable || (access & DESC_WRITABLE);
}

static bool dpmi_rights_valid(uint8_t access, uint8_t flags)
{
    if (!(access & DESC_SEGMENT) ||
        (access & DESC_DPL_MASK) != DESC_DPL3 ||
        (flags & 0x20))
        return false;

    /* Absent descriptors may carry arbitrary type bits (DPMI 0009h).
     * Present code must be readable and non-conforming. */
    if ((access & (DESC_PRESENT | DESC_CODE)) == (DESC_PRESENT | DESC_CODE) &&
        ((access & (DESC_READABLE | 0x04)) != DESC_READABLE))
        return false;
    return true;
}

static uint16_t dpmi_next_general_index(uint16_t index)
{
    index++;
    return index < DPMI_MAX_DESCRIPTORS
         ? index : DPMI_SPECIFIC_DESCRIPTOR_COUNT;
}

static bool dpmi_descriptor_run_free(const dpmi_state_t *dpmi,
                                     uint16_t start, uint16_t count)
{
    if (!count || start < DPMI_SPECIFIC_DESCRIPTOR_COUNT ||
        start >= DPMI_MAX_DESCRIPTORS ||
        count > DPMI_MAX_DESCRIPTORS - start)
        return false;

    for (uint16_t i = 0; i < count; i++)
        if (dpmi->descriptor_state[start + i] != DPMI_DESC_FREE)
            return false;
    return true;
}

static bool dpmi_find_descriptor_run(const dpmi_state_t *dpmi,
                                     uint16_t count, uint16_t *start)
{
    if (!count || count > DPMI_MAX_DESCRIPTORS -
                          DPMI_SPECIFIC_DESCRIPTOR_COUNT)
        return false;

    uint16_t found = 0;
    uint16_t candidate = DPMI_SPECIFIC_DESCRIPTOR_COUNT;
    for (uint16_t i = DPMI_SPECIFIC_DESCRIPTOR_COUNT;
         i < DPMI_MAX_DESCRIPTORS; i++) {
        if (dpmi->descriptor_state[i] == DPMI_DESC_FREE) {
            if (!found) candidate = i;
            if (++found == count) {
                if (start) *start = candidate;
                return true;
            }
        } else {
            found = 0;
        }
    }
    return false;
}

static bool dpmi_claim_descriptor_run(dpmi_state_t *dpmi, uint16_t start,
                                      uint16_t count, uint8_t state)
{
    if (!dpmi_descriptor_run_free(dpmi, start, count)) return false;

    for (uint16_t i = 0; i < count; i++) {
        dpmi_build_desc(&dpmi->ldt[start + i], 0, 0,
                        DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                        DESC_WRITABLE,
                        0);
        dpmi->descriptor_state[start + i] = state;
    }
    dpmi->next_free_index = dpmi_next_general_index(start + count - 1u);
    return true;
}

static uint16_t dpmi_alloc_descriptor_run(dpmi_state_t *dpmi,
                                          uint16_t count, uint8_t state)
{
    uint16_t start;
    if (!dpmi_find_descriptor_run(dpmi, count, &start) ||
        !dpmi_claim_descriptor_run(dpmi, start, count, state))
        return 0;
    return dpmi_index_to_sel(start);
}

static void dpmi_release_descriptor(dpmi_state_t *dpmi, uint16_t index)
{
    if (index >= DPMI_MAX_DESCRIPTORS)
        return;

    dpmi_zero(&dpmi->ldt[index], sizeof(dpmi_descriptor_t));
    dpmi->descriptor_state[index] = DPMI_DESC_FREE;
    if (index >= DPMI_SPECIFIC_DESCRIPTOR_COUNT &&
        (dpmi->next_free_index < DPMI_SPECIFIC_DESCRIPTOR_COUNT ||
         dpmi->next_free_index >= DPMI_MAX_DESCRIPTORS ||
         index < dpmi->next_free_index))
        dpmi->next_free_index = index;
}

static void dpmi_release_descriptor_run(dpmi_state_t *dpmi, uint16_t start,
                                        uint16_t count)
{
    if (start >= DPMI_MAX_DESCRIPTORS ||
        count > DPMI_MAX_DESCRIPTORS - start)
        return;
    for (uint16_t i = 0; i < count; i++)
        dpmi_release_descriptor(dpmi, start + i);
}

static uint16_t dpmi_alloc_descriptor(dpmi_state_t *dpmi)
{
    uint16_t start = dpmi->next_free_index;
    if (start < DPMI_SPECIFIC_DESCRIPTOR_COUNT ||
        start >= DPMI_MAX_DESCRIPTORS)
        start = DPMI_SPECIFIC_DESCRIPTOR_COUNT;
    uint16_t i = start;

    do {
        if (dpmi->descriptor_state[i] == DPMI_DESC_FREE) {
            dpmi_descriptor_t *d = &dpmi->ldt[i];
            dpmi_build_desc(d, 0, 0,
                            DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                            DESC_WRITABLE,
                            0);
            dpmi->descriptor_state[i] = DPMI_DESC_MUTABLE;
            dpmi->next_free_index = dpmi_next_general_index(i);
            return dpmi_index_to_sel(i);
        }
        i = dpmi_next_general_index(i);
    } while (i != start);

    /* No free descriptors */
    serial_puts("[DPMI] Out of LDT descriptors!\n");
    return 0;
}

#define DPMI_DOS_PARAGRAPHS_PER_DESCRIPTOR 0x1000u

static uint16_t dpmi_dos_descriptor_count(bool is_32bit,
                                          uint16_t paragraphs)
{
    if (is_32bit) return 1;
    uint32_t count = ((uint32_t)paragraphs +
                      DPMI_DOS_PARAGRAPHS_PER_DESCRIPTOR - 1u) /
                     DPMI_DOS_PARAGRAPHS_PER_DESCRIPTOR;
    return count ? (uint16_t)count : 1;
}

static bool dpmi_dos_block_valid(const dpmi_state_t *dpmi, uint16_t index)
{
    if (index >= DPMI_MAX_DESCRIPTORS) return false;
    const dpmi_dos_block_t *block = &dpmi->dos_blocks[index];
    if (!block->allocated || !block->descriptor_count ||
        block->descriptor_count > DPMI_MAX_DESCRIPTORS - index)
        return false;

    for (uint16_t i = 0; i < block->descriptor_count; i++)
        if (dpmi->descriptor_state[index + i] != DPMI_DESC_DOS_MEMORY)
            return false;
    return true;
}

static void dpmi_configure_dos_descriptors(dpmi_state_t *dpmi,
                                           uint16_t start,
                                           uint16_t descriptor_count,
                                           uint16_t segment,
                                           uint16_t paragraphs)
{
    uint32_t block_base = (uint32_t)segment << 4;
    uint32_t block_size = (uint32_t)paragraphs << 4;
    uint8_t access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                     DESC_WRITABLE;
    uint8_t flags = dpmi->is_32bit ? DESC_32BIT : 0;

    for (uint16_t i = 0; i < descriptor_count; i++) {
        uint32_t offset = (uint32_t)i << 16;
        uint32_t remaining = block_size > offset ? block_size - offset : 0;
        uint32_t chunk = remaining > 0x10000u ? 0x10000u : remaining;
        uint32_t limit = i == 0
                       ? (block_size ? block_size - 1u : 0)
                       : (chunk ? chunk - 1u : 0);
        dpmi_build_desc(&dpmi->ldt[start + i], block_base + offset,
                        limit, access, flags);
        dpmi->descriptor_state[start + i] = DPMI_DESC_DOS_MEMORY;
    }
}

static uint16_t dpmi_dos_descriptor_capacity(const dpmi_state_t *dpmi,
                                             uint16_t start,
                                             uint16_t current_count)
{
    uint16_t count = current_count;
    while (count < 16u && start + count < DPMI_MAX_DESCRIPTORS &&
           dpmi->descriptor_state[start + count] == DPMI_DESC_FREE)
        count++;
    return count;
}

static uint16_t dpmi_dos_max_paragraphs(const dpmi_state_t *dpmi,
                                        uint16_t start,
                                        uint16_t current_count)
{
    if (dpmi->is_32bit) return 0xFFFFu;
    uint32_t count = dpmi_dos_descriptor_capacity(dpmi, start,
                                                  current_count);
    uint32_t paragraphs = count * DPMI_DOS_PARAGRAPHS_PER_DESCRIPTOR;
    return paragraphs > 0xFFFFu ? 0xFFFFu : (uint16_t)paragraphs;
}

static uint16_t dpmi_dos_error(int result)
{
    if (result == DOS_MEM_ERR_BLOCK) return 0x0009;
    if (result == DOS_MEM_ERR_NOMEM) return 0x0008;
    return 0x0007;
}

static bool dpmi_selector_in_run(uint16_t selector, uint16_t start,
                                 uint16_t count)
{
    if (!(selector & 0x04u)) return false;
    uint16_t index = dpmi_sel_to_index(selector);
    return index >= start && (uint32_t)index < (uint32_t)start + count;
}

static void dpmi_clear_freed_data_selectors(cpu8086_state_t *cpu,
                                            uint16_t start, uint16_t count)
{
    if (dpmi_selector_in_run(cpu->ds, start, count)) cpu8086_cache_segment(cpu, 3, 0, NULL);
    if (dpmi_selector_in_run(cpu->es, start, count)) cpu8086_cache_segment(cpu, 0, 0, NULL);
    if (dpmi_selector_in_run(cpu->fs, start, count)) cpu8086_cache_segment(cpu, 4, 0, NULL);
    if (dpmi_selector_in_run(cpu->gs, start, count)) cpu8086_cache_segment(cpu, 5, 0, NULL);
}

uint16_t dpmi_segment_selector(dos_vm_t *vm, uint16_t segment)
{
    dpmi_state_t *dpmi = &vm->dpmi;
    for (uint16_t i = 0; i < DPMI_MAX_DESCRIPTORS; i++) {
        if (dpmi->descriptor_state[i] == DPMI_DESC_RM_ALIAS &&
            dpmi_desc_get_base(&dpmi->ldt[i]) == ((uint32_t)segment << 4))
            return dpmi_index_to_sel(i);
    }
    uint16_t selector = dpmi_alloc_descriptor(dpmi);
    if (!selector) return 0;
    uint16_t index = dpmi_sel_to_index(selector);
    dpmi_build_desc(&dpmi->ldt[index], (uint32_t)segment << 4, 0xFFFF,
                    DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT | DESC_WRITABLE, 0);
    dpmi->descriptor_state[index] = DPMI_DESC_RM_ALIAS;
    dos_native_sync_ldt(vm);
    return selector;
}

static uint16_t dpmi_host_code_selector(dpmi_state_t *dpmi)
{
    uint16_t idx;
    if (dpmi_selector_allocated(dpmi, dpmi->sel_host_code, &idx) &&
        dpmi->descriptor_state[idx] == DPMI_DESC_HOST) {
        if (dpmi->is_32bit)
            dpmi->ldt[idx].flags_lim |= DESC_32BIT;
        else
            dpmi->ldt[idx].flags_lim &= ~DESC_32BIT;
        return dpmi->sel_host_code;
    }

    uint16_t sel = dpmi_alloc_descriptor(dpmi);
    if (!sel)
        return 0;

    idx = dpmi_sel_to_index(sel);
    dpmi_build_desc(&dpmi->ldt[idx], (uint32_t)DPMI_ENTRY_SEG << 4,
                    0xFFFF,
                    DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                    DESC_CODE | DESC_READABLE,
                    dpmi->is_32bit ? DESC_32BIT : 0);
    dpmi->descriptor_state[idx] = DPMI_DESC_HOST;
    dpmi->sel_host_code = sel;
    return sel;
}

uint16_t dpmi_get_host_code_selector(dos_vm_t *vm)
{
    return vm ? dpmi_host_code_selector(&vm->dpmi) : 0;
}

uint16_t dpmi_get_exception_stack_selector(dos_vm_t *vm)
{
    if (!vm || !vm->mem ||
        dpmi_memory_limit(vm) < DPMI_EXT_BASE + DPMI_HOST_STACK_AREA)
        return 0;

    dpmi_state_t *dpmi = &vm->dpmi;
    uint16_t index;
    uint16_t selector = dpmi->sel_exception_stack;
    if (!dpmi_selector_allocated(dpmi, selector, &index) ||
        dpmi->descriptor_state[index] != DPMI_DESC_HOST) {
        selector = dpmi_alloc_descriptor(dpmi);
        if (!selector) return 0;
        index = dpmi_sel_to_index(selector);
        dpmi->sel_exception_stack = selector;
        dpmi->descriptor_state[index] = DPMI_DESC_HOST;
    }
    dpmi_build_desc(&dpmi->ldt[index],
                    dpmi_ext_limit(vm),
                    DPMI_EXCEPTION_STACK_SIZE - 1u,
                    DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT | DESC_WRITABLE,
                    dpmi->is_32bit ? DESC_32BIT : 0);
    return selector;
}

bool dpmi_raw_mode_switch(dos_vm_t *vm, uint8_t private_frame_bytes)
{
    if (!vm || !vm->cpu)
        return false;

    cpu8086_state_t *cpu = vm->cpu;
    uint16_t next_ds = cpu->ax;
    uint16_t next_es = cpu->cx;
    uint16_t next_ss = cpu->dx;
    uint16_t next_cs = cpu->si;
    dpmi_stack_t outgoing = { cpu->ss, cpu->esp };
    if (cpu_stack_addr32(cpu))
        outgoing.esp += private_frame_bytes;
    else
        outgoing.esp = (outgoing.esp & 0xFFFF0000u) |
                       (uint16_t)(cpu->sp + private_frame_bytes);
    uint32_t flags = private_frame_bytes ? vm->software_int_return_flags
                                         : cpu8086_flags_image(cpu);
    if (cpu->protected_mode && vm->dpmi.active)
        flags = (flags & ~FLAG_IF) |
                (vm->dpmi.virtual_interrupts_enabled ? FLAG_IF : 0);

    if (!cpu->protected_mode) {
        uint32_t next_sp = vm->dpmi.is_32bit ? cpu->ebx : cpu->bx;
        uint32_t next_ip = vm->dpmi.is_32bit ? cpu->edi : cpu->di;
        uint16_t code_idx;

        if (!dpmi_selector_is_code(&vm->dpmi, next_cs) ||
            !dpmi_selector_is_data(&vm->dpmi, next_ss, true) ||
            !dpmi_selector_is_data(&vm->dpmi, next_ds, false) ||
            !dpmi_selector_is_data(&vm->dpmi, next_es, false) ||
            !dpmi_selector_allocated(&vm->dpmi, next_cs, &code_idx) ||
            next_ip > dpmi_desc_get_limit(&vm->dpmi.ldt[code_idx])) {
            serial_puts("[DPMI] Raw RM->PM switch rejected invalid selectors\n");
            return false;
        }

        vm->dpmi.real_mode_stack = (dpmi_stack_t){ outgoing.ss,
                                                   (uint16_t)outgoing.esp };
        vm->dpmi.real_mode_paging = dpmi_current_paging(cpu);
        vm->dpmi.virtual_interrupts_enabled = (flags & FLAG_IF) != 0;
        cpu->eflags = flags | FLAGS_FIXED | FLAG_IF;
        cpu->ds = next_ds;
        cpu->es = next_es;
        cpu->ss = next_ss;
        cpu->esp = next_sp;
        cpu->cs = next_cs;
        cpu->eip = next_ip;
        cpu->fs = 0;
        cpu->gs = 0;
        cpu->protected_mode = true;
        cpu->pm_cs_loaded = true;
        cpu8086_use_host_ldt(cpu);
        dpmi_apply_paging(cpu, vm->dpmi.suspended_paging);
        cpu8086_sync_cs(cpu);
        cpu8086_sync_data(cpu);
        cpu->halted = false;
        serial_puts("[DPMI] Raw switch RM->PM\n");
        return true;
    }

    vm->dpmi.suspended_stack = outgoing;
    vm->dpmi.suspended_paging = dpmi_current_paging(cpu);
    cpu->eflags = flags | FLAGS_FIXED;
    cpu->ds = next_ds;
    cpu->es = next_es;
    cpu->ss = next_ss;
    cpu->esp = cpu->bx;
    cpu->cs = next_cs;
    cpu->eip = cpu->di;
    cpu->fs = 0;
    cpu->gs = 0;
    cpu->protected_mode = false;
    cpu->pm_cs_loaded = false;
    cpu->op_size_32 = false;
    cpu->addr_size_32 = false;
    dpmi_apply_paging(cpu, vm->dpmi.real_mode_paging);
    cpu8086_reset_real_cs(cpu, cpu->cs);
    cpu8086_sync_data(cpu);
    cpu->halted = false;
    serial_puts("[DPMI] Raw switch PM->RM\n");
    return true;
}

/* ══════════════════════════════════════════════════════════════════
 * 4. dpmi_enter_protected_mode -- Real→protected mode switch
 * ══════════════════════════════════════════════════════════════════ */

void dpmi_enter_protected_mode(dos_vm_t *vm)
{
    if (!vm || !vm->cpu || !vm->mem) return;

    bool exec_client = dos_exec_begin_dpmi_client(vm);
    cpu8086_state_t *cpu = vm->cpu;
    dpmi_state_t *dpmi = &vm->dpmi;
    uint32_t return_flags = vm->software_int_frame_bytes
                          ? vm->software_int_return_flags : cpu->eflags;
    uint16_t error = 0;
    uint16_t first_index = 0;
    uint16_t descriptor_count = 0;
    bool descriptors_claimed = false;

    if (dpmi->active) {
        cpu->ax = 0x8011;
        cpu->eflags = return_flags | FLAG_CF | FLAGS_FIXED;
        return;
    }
    if (cpu->ax & ~1u) {
        error = 0x8021;
        goto fail;
    }

    bool is_32bit = (cpu->ax & 1u) != 0;

    /* Per DPMI spec, initial selectors map the client's real-mode segments.
     * cpu->cs is F000 (the stub), so read the CALLER's CS from the stack.
     * DS/SS are the client's original values (unchanged by CALL FAR). */
    uint32_t stack_linear_pre = ((uint32_t)cpu->ss << 4) + cpu->sp;
    uint32_t psp_base = (uint32_t)vm->current_psp << 4;
    if (stack_linear_pre > vm->total_mem_size ||
        10u > vm->total_mem_size - stack_linear_pre ||
        psp_base > vm->total_mem_size ||
        0x2Eu > vm->total_mem_size - psp_base) {
        error = 0x8021;
        goto fail;
    }

    uint16_t caller_ip = dos_mem_read16(vm, stack_linear_pre + 6);
    uint16_t caller_cs_val = dos_mem_read16(vm, stack_linear_pre + 8);
    uint16_t environment_segment = dos_mem_read16(vm, psp_base + 0x2C);
    uint32_t cs_base = (uint32_t)caller_cs_val << 4;
    uint32_t ds_base = (uint32_t)cpu->ds << 4;
    uint32_t ss_base = (uint32_t)cpu->ss << 4;
    uint32_t environment_base = (uint32_t)environment_segment << 4;
    if (cs_base >= vm->total_mem_size || ds_base >= vm->total_mem_size ||
        ss_base >= vm->total_mem_size ||
        (environment_segment && environment_base >= vm->total_mem_size)) {
        error = 0x8021;
        goto fail;
    }

    descriptor_count = environment_segment ? 5u : 4u;
    uint16_t first_selector = dpmi_alloc_descriptor_run(
        dpmi, descriptor_count, DPMI_DESC_MUTABLE);
    if (!first_selector) {
        error = 0x8011;
        goto fail;
    }
    first_index = dpmi_sel_to_index(first_selector);
    descriptors_claimed = true;

    dpmi->sel_code = first_selector;
    dpmi->sel_data = first_selector + DPMI_SEL_INC;
    dpmi->sel_stack = first_selector + 2u * DPMI_SEL_INC;
    dpmi->sel_psp = first_selector + 3u * DPMI_SEL_INC;
    dpmi->sel_env = environment_segment
                  ? first_selector + 4u * DPMI_SEL_INC : 0;

    uint8_t code_access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                          DESC_CODE | DESC_READABLE;
    uint8_t data_access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                          DESC_WRITABLE;
    uint8_t data_flags = is_32bit ? DESC_32BIT : 0;
    dpmi_build_desc(&dpmi->ldt[first_index], cs_base, 0xFFFF,
                    code_access, 0);
    dpmi_build_desc(&dpmi->ldt[first_index + 1u], ds_base, 0xFFFF,
                    data_access, data_flags);
    dpmi_build_desc(&dpmi->ldt[first_index + 2u], ss_base, 0xFFFF,
                    data_access, data_flags);
    dpmi_build_desc(&dpmi->ldt[first_index + 3u], psp_base, 0xFF,
                    data_access, 0);
    dpmi->descriptor_state[first_index + 3u] = DPMI_DESC_CLIENT_SYSTEM;
    if (environment_segment) {
        dpmi_build_desc(&dpmi->ldt[first_index + 4u], environment_base,
                        0xFFFF, data_access, 0);
        dpmi->descriptor_state[first_index + 4u] =
            DPMI_DESC_CLIENT_SYSTEM;
    }

    /* Reserve host code and a resident exception stack transactionally.
     * Default vectors and exception recovery must survive LDT exhaustion. */
    dpmi->is_32bit = is_32bit;
    if (!dpmi_host_code_selector(dpmi) ||
        !dpmi_get_exception_stack_selector(vm)) {
        error = 0x8011;
        goto fail;
    }
    if (environment_segment)
        dos_mem_write16(vm, psp_base + 0x2C, dpmi->sel_env);

    /* Read caller's return address from the stack.
     * Stack layout (16-bit real mode, growing down):
     *   SP+0: INT return IP (stub+2)
     *   SP+2: INT return CS (F000)
     *   SP+4: FLAGS
     *   SP+6: caller IP (DOS4GW code after CALL FAR)
     *   SP+8: caller CS
     * Skip both frames. EIP/ESP are offsets within their segments. */
    uint16_t orig_sp = (uint16_t)(cpu->sp + 10u);

    dpmi->saved_cs = cpu->cs;
    dpmi->saved_ip = cpu->ip;
    dpmi->saved_ss = cpu->ss;
    dpmi->saved_sp = cpu->sp;
    dpmi->saved_ds = cpu->ds;
    dpmi->saved_es = cpu->es;
    dpmi->active = true;
    dpmi->owner_psp = vm->current_psp;
    dpmi->is_32bit = is_32bit;
    dpmi->virtual_interrupts_enabled = (return_flags & FLAG_IF) != 0;

    serial_puts("[DPMI] Mode switch: ");
    serial_puts(is_32bit ? "32-bit" : "16-bit");
    serial_puts(" client\n");

    /* Switch CPU to protected mode (USE16) */
    dpmi->real_mode_paging = dpmi_current_paging(cpu);
    cpu8086_use_host_ldt(cpu);
    cpu->protected_mode = true;
    cpu->pm_cs_loaded   = true;
    cpu->op_size_32     = false;
    cpu->addr_size_32   = false;
    cpu->cr0           |= 1;
    dpmi->suspended_paging = dpmi_current_paging(cpu);

    /* Set segment registers to PM selectors */
    cpu->cs = dpmi->sel_code;
    cpu8086_sync_cs(cpu);
    cpu->ds = dpmi->sel_data;
    cpu->es = dpmi->sel_psp;
    cpu->ss = dpmi->sel_stack;
    cpu->fs = 0;
    cpu->gs = 0;
    cpu8086_sync_data(cpu);

    /* EIP = offset within CS, ESP = offset within SS */
    cpu->eip = caller_ip;
    if (is_32bit)
        cpu->esp = orig_sp;
    else
        cpu->sp = orig_sp;
    /* This host virtualizes the client IF and leaves the physical IF set. */
    cpu->eflags = (return_flags & ~FLAG_CF) | FLAGS_FIXED | FLAG_IF;

    serial_puts("[DPMI] CS=");
    serial_puthex(cpu->cs, 4);
    serial_puts(" DS=");
    serial_puthex(cpu->ds, 4);
    serial_puts(" SS=");
    serial_puthex(cpu->ss, 4);
    serial_puts(" EIP=");
    serial_puthex(cpu->eip, 8);
    serial_puts(" ESP=");
    serial_puthex(cpu->esp, 8);
    serial_puts("\n  cs_base=");
    serial_puthex(cs_base, 8);
    serial_puts(" caller_ip=");
    serial_puthex(caller_ip, 4);
    serial_puts(" bytes@CS:EIP:");
    for (int i = 0; i < 16; i++) {
        serial_puts(" ");
        uint32_t address = cs_base + caller_ip + (uint32_t)i;
        serial_puthex(address < vm->total_mem_size ? vm->mem[address] : 0, 2);
    }
    serial_puts("\n");
    return;

fail:
    if (descriptors_claimed)
        dpmi_release_descriptor_run(dpmi, first_index, descriptor_count);
    dpmi->sel_code = 0;
    dpmi->sel_data = 0;
    dpmi->sel_stack = 0;
    dpmi->sel_psp = 0;
    dpmi->sel_env = 0;
    if (dpmi->sel_host_code) {
        uint16_t host_index = dpmi_sel_to_index(dpmi->sel_host_code);
        if (host_index < DPMI_MAX_DESCRIPTORS &&
            dpmi->descriptor_state[host_index] == DPMI_DESC_HOST)
            dpmi_release_descriptor(dpmi, host_index);
        dpmi->sel_host_code = 0;
    }
    if (dpmi->sel_exception_stack) {
        uint16_t index = dpmi_sel_to_index(dpmi->sel_exception_stack);
        if (index < DPMI_MAX_DESCRIPTORS &&
            dpmi->descriptor_state[index] == DPMI_DESC_HOST)
            dpmi_release_descriptor(dpmi, index);
        dpmi->sel_exception_stack = 0;
    }
    dpmi->active = false;
    dpmi->is_32bit = false;
    if (exec_client) dos_exec_abort_dpmi_client(vm);
    cpu->ax = error ? error : 0x8021;
    cpu->eflags = return_flags | FLAG_CF | FLAGS_FIXED;
}

/* ══════════════════════════════════════════════════════════════════
 * 6. dos_int31_dpmi -- INT 31h DPMI services dispatcher
 * ══════════════════════════════════════════════════════════════════ */

typedef enum {
    DPMI_RM_CALL_INTERRUPT,
    DPMI_RM_CALL_FAR,
    DPMI_RM_CALL_IRET
} dpmi_rm_call_kind_t;

static bool dpmi_range_valid(const dos_vm_t *vm, uint32_t address,
                             uint32_t size)
{
    return vm && vm->mem && address <= vm->total_mem_size &&
           size <= vm->total_mem_size - address;
}

/* Fixed-size DPMI ABI records (largest: 88-byte extended exception frame).
 * Snapshot both translations before access, including records that straddle
 * non-contiguous pages or alias their own paging structures. */
typedef struct {
    dos_page_translation_t translation;
    uint32_t size;
    bool accessed, dirty;
} dpmi_buffer_page_t;

typedef struct {
    dpmi_buffer_page_t pages[2];
    uint32_t size;
    uint8_t count;
    bool writable;
} dpmi_client_buffer_t;

typedef struct {
    uint32_t error, linear;
    uint8_t vector;
} dpmi_buffer_fault_t;

static bool dpmi_buffer_fault(dpmi_buffer_fault_t *fault, uint8_t vector,
                               uint32_t error, uint32_t linear)
{
    if (fault) *fault = (dpmi_buffer_fault_t){ .vector = vector, .error = error, .linear = linear };
    return false;
}

static bool dpmi_client_region_probe(dos_vm_t *vm, uint16_t selector, uint32_t offset,
                                      bool writable, uint32_t *linear, uint64_t *available,
                                      dpmi_buffer_fault_t *fault)
{
    dpmi_descriptor_ref_t reference;
    dos_page_fault_t page_fault = {0};
    dpmi_buffer_fault(fault, 13, selector & ~3u, 0);
    if (!vm || !vm->cpu || !vm->mem ||
        !dpmi_lookup_descriptor(vm, selector, &reference, &page_fault)) {
        if (page_fault.raised)
            return dpmi_buffer_fault(fault, 14, page_fault.error, page_fault.linear);
        return false;
    }
    dpmi_descriptor_t descriptor = reference.descriptor;
    uint8_t access = descriptor.access;
    bool code = (access & DESC_CODE) != 0;
    bool conforming = code && (access & 4u);
    if (!(access & DESC_SEGMENT) ||
        (!conforming && (access & DESC_DPL_MASK) != DESC_DPL3) ||
        (code && (writable || !(access & DESC_READABLE))) ||
        (!code && writable && !(access & DESC_WRITABLE))) return false;
    if (!(access & DESC_PRESENT)) return dpmi_buffer_fault(fault, 11, selector & ~3u, 0);
    dpmi_buffer_fault(fault, 13, 0, 0);
    uint32_t limit = dpmi_desc_get_limit(&descriptor);
    if (!code && (access & 4u)) {
        uint32_t top = descriptor.flags_lim & DESC_32BIT ? UINT32_MAX : 0xFFFFu;
        if (offset <= limit || offset > top) return false;
        limit = top;
    } else if (offset > limit) return false;
    uint32_t base = dpmi_desc_get_base(&descriptor);
    if ((uint64_t)base + offset > UINT32_MAX) return false;
    *linear = base + offset;
    *available = (uint64_t)limit - offset + 1u;
    uint64_t linear_available = (uint64_t)UINT32_MAX - *linear + 1u;
    if (*available > linear_available) *available = linear_available;
    return true;
}

static bool dpmi_linear_buffer_probe(dos_vm_t *vm, uint32_t linear, uint32_t size,
                                      unsigned access, dpmi_client_buffer_t *buffer,
                                      dpmi_buffer_fault_t *fault)
{
    dpmi_buffer_fault(fault, 13, 0, linear);
    if (!vm || !vm->cpu || !vm->mem || !buffer || !size ||
        size > DPMI_EXCEPTION_EXT_FRAME_SIZE ||
        (uint64_t)linear + size - 1u > UINT32_MAX) return false;

    dpmi_client_buffer_t result = { .size = size, .writable = (access & DOS_PAGE_WRITE) != 0 };
    while (size) {
        dpmi_buffer_page_t *page = &result.pages[result.count++];
        page->size = DPMI_EXT_PAGE_SIZE - (linear & 0xFFFu);
        if (page->size > size) page->size = size;
        dos_page_fault_t page_fault = {0};
        if (!dos_page_probe(vm, linear, access, &page->translation, &page_fault))
            return dpmi_buffer_fault(fault, 14, page_fault.error, page_fault.linear);
        if ((page->translation.pde != UINT32_MAX &&
             (!dpmi_range_valid(vm, page->translation.pde, 4u) ||
              !dpmi_range_valid(vm, page->translation.pte, 4u))) ||
            !dpmi_range_valid(vm, page->translation.physical, page->size)) return false;
        linear += page->size;
        size -= page->size;
    }
    *buffer = result;
    return true;
}

static bool dpmi_linear_buffer(dos_vm_t *vm, uint32_t linear, uint32_t size,
                                unsigned access, dpmi_client_buffer_t *buffer)
{
    return dpmi_linear_buffer_probe(vm, linear, size, access, buffer, NULL);
}

static bool dpmi_client_buffer_probe(dos_vm_t *vm, uint16_t selector,
                                      uint32_t offset, uint32_t size, bool writable,
                                      dpmi_client_buffer_t *buffer, dpmi_buffer_fault_t *fault)
{
    uint32_t linear;
    uint64_t available;
    dpmi_buffer_fault(fault, 13, 0, 0);
    return buffer && size && size <= 64u &&
           dpmi_client_region_probe(vm, selector, offset, writable, &linear, &available, fault) &&
           size <= available &&
           dpmi_linear_buffer_probe(vm, linear, size, DOS_PAGE_USER |
                                     (writable ? DOS_PAGE_WRITE : DOS_PAGE_READ), buffer, fault);
}

static bool dpmi_client_buffer(dos_vm_t *vm, uint16_t selector,
                               uint32_t offset, uint32_t size, bool writable,
                               dpmi_client_buffer_t *buffer)
{
    return dpmi_client_buffer_probe(vm, selector, offset, size, writable, buffer, NULL);
}

static bool dpmi_service_buffer(dos_vm_t *vm, uint16_t selector,
                                 uint32_t offset, uint32_t size, bool writable,
                                 dpmi_client_buffer_t *buffer)
{
    dpmi_buffer_fault_t fault;
    while (!dpmi_client_buffer_probe(vm, selector, offset, size, writable, buffer, &fault)) {
        if (!dpmi_service_exception(vm, fault.vector, fault.error, fault.linear)) return false;
    }
    return vm->cpu->running;
}

static bool dpmi_buffer_commit(dos_vm_t *vm, dpmi_client_buffer_t *buffer,
                               uint32_t offset, uint32_t size, bool write)
{
    if (!buffer || offset > buffer->size || size > buffer->size - offset ||
        (write && !buffer->writable)) return false;
    uint32_t skip = offset, remaining = size;
    for (unsigned i = 0; i < buffer->count && remaining; i++) {
        dpmi_buffer_page_t *page = &buffer->pages[i];
        if (skip >= page->size) { skip -= page->size; continue; }
        uint32_t part = page->size - skip;
        if (part > remaining) part = remaining;
        if (!page->accessed || (write && !page->dirty)) {
            dos_page_commit(vm, &page->translation, write);
            page->accessed = true;
            if (write) page->dirty = true;
        }
        remaining -= part;
        skip = 0;
    }
    return remaining == 0;
}

static bool dpmi_buffer_transfer(dos_vm_t *vm, dpmi_client_buffer_t *buffer,
                                 uint32_t offset, void *data, uint32_t size,
                                 bool write)
{
    if (!data || !dpmi_buffer_commit(vm, buffer, offset, size, write)) return false;
    uint8_t *bytes = data;
    for (unsigned i = 0; i < buffer->count && size; i++) {
        dpmi_buffer_page_t *page = &buffer->pages[i];
        if (offset >= page->size) { offset -= page->size; continue; }
        uint32_t part = page->size - offset;
        if (part > size) part = size;
        for (uint32_t n = 0; n < part; n++) {
            uint32_t address = page->translation.physical + offset + n;
            if (write) dos_mem_write8(vm, address, bytes[n]);
            else bytes[n] = dos_mem_read8(vm, address);
        }
        bytes += part;
        size -= part;
        offset = 0;
    }
    return size == 0;
}

static uint32_t dpmi_buffer_load(dos_vm_t *vm, dpmi_client_buffer_t *buffer,
                                  uint32_t offset, unsigned width)
{
    uint32_t value = 0;
    if (width <= sizeof(value))
        (void)dpmi_buffer_transfer(vm, buffer, offset, &value, width, false);
    return value;
}

static void dpmi_buffer_store(dos_vm_t *vm, dpmi_client_buffer_t *buffer,
                               uint32_t offset, uint32_t value, unsigned width)
{
    if (width <= sizeof(value))
        (void)dpmi_buffer_transfer(vm, buffer, offset, &value, width, true);
}

static bool dpmi_code_target_probe(dos_vm_t *vm, uint16_t selector,
                                    uint32_t offset, dpmi_buffer_fault_t *fault)
{
    uint16_t index;
    dpmi_buffer_fault(fault, 13, selector & ~3u, 0);
    if (!vm || !vm->cpu || !vm->mem ||
        !dpmi_selector_allocated(&vm->dpmi, selector, &index)) return false;
    const dpmi_descriptor_t *descriptor = &vm->dpmi.ldt[index];
    uint8_t access = descriptor->access;
    uint8_t required = DESC_DPL3 | DESC_SEGMENT | DESC_CODE | DESC_READABLE;
    if ((access & required) != required || (access & 4u)) return false;
    if (!(access & DESC_PRESENT)) return dpmi_buffer_fault(fault, 11, selector & ~3u, 0);
    dpmi_buffer_fault(fault, 13, 0, 0);
    if (offset > dpmi_desc_get_limit(descriptor)) return false;

    uint64_t linear = (uint64_t)dpmi_desc_get_base(descriptor) + offset;
    dpmi_client_buffer_t code;
    return linear <= UINT32_MAX &&
           dpmi_linear_buffer_probe(vm, (uint32_t)linear, 1u, DOS_PAGE_USER, &code, fault);
}

static bool dpmi_code_target(dos_vm_t *vm, uint16_t selector, uint32_t offset)
{
    return dpmi_code_target_probe(vm, selector, offset, NULL);
}

static bool dpmi_stack_descriptor(dos_vm_t *vm, uint16_t selector,
                                   dpmi_descriptor_t *descriptor)
{
    if (!vm || !vm->cpu || (selector & 3u) != 3u) return false;
    /* Nested handlers use the loaded SS, not a descriptor that the client
     * may have edited since loading it. Dormant/new stacks load afresh. */
    if (vm->cpu->protected_mode && selector == vm->cpu->ss) {
        if (!vm->cpu->ss_cache.valid) return false;
        *descriptor = vm->cpu->ss_cache.descriptor;
    } else if (!dpmi_guest_descriptor(vm, selector, descriptor)) return false;
    uint8_t required = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT | DESC_WRITABLE;
    return (descriptor->access & required) == required &&
           !(descriptor->access & DESC_CODE);
}

static bool dpmi_stack_region(const dpmi_descriptor_t *descriptor, uint32_t offset,
                               uint32_t size, uint32_t *linear)
{
    if (!size) return false;
    uint64_t last = (uint64_t)offset + size - 1u;
    uint32_t top = descriptor->flags_lim & DESC_32BIT ? UINT32_MAX : 0xFFFFu;
    uint32_t limit = dpmi_desc_get_limit(descriptor);
    if (last > top || ((descriptor->access & 4u) ? offset <= limit : last > limit))
        return false;
    uint32_t base = dpmi_desc_get_base(descriptor);
    if ((uint64_t)base + last > UINT32_MAX) return false;
    *linear = base + offset;
    return true;
}

static bool dpmi_stack_buffer(dos_vm_t *vm, uint16_t selector, uint32_t offset,
                               uint32_t size, bool write, dpmi_client_buffer_t *buffer)
{
    dpmi_descriptor_t descriptor;
    uint32_t linear;
    return dpmi_stack_descriptor(vm, selector, &descriptor) &&
           dpmi_stack_region(&descriptor, offset, size, &linear) &&
           dpmi_linear_buffer(vm, linear, size, DOS_PAGE_USER |
                               (write ? DOS_PAGE_WRITE : DOS_PAGE_READ), buffer);
}

bool dpmi_stack_span(dos_vm_t *vm, uint16_t selector, uint32_t offset,
                     uint32_t size, uint32_t *linear)
{
    dpmi_descriptor_t descriptor;
    dpmi_client_buffer_t buffer;
    uint32_t address;
    if (!dpmi_stack_descriptor(vm, selector, &descriptor) ||
        !dpmi_stack_region(&descriptor, offset, size, &address) ||
        !dpmi_linear_buffer(vm, address, size, DOS_PAGE_USER | DOS_PAGE_WRITE, &buffer))
        return false;
    if (linear) *linear = address;
    return true;
}

bool dpmi_read_stack_frame(dos_vm_t *vm, void *data, uint32_t size)
{
    dpmi_client_buffer_t buffer;
    return data && vm && vm->cpu &&
           dpmi_stack_buffer(vm, vm->cpu->ss, cpu_stack_offset(vm->cpu), size, false, &buffer) &&
           dpmi_buffer_transfer(vm, &buffer, 0, data, size, false);
}

typedef struct {
    dpmi_client_buffer_t buffer;
    dpmi_descriptor_t descriptor;
    dpmi_stack_t cursor;
    uint8_t bytes[DPMI_EXCEPTION_EXT_FRAME_SIZE];
} dpmi_stack_frame_t;

static bool dpmi_prepare_stack_frame(dos_vm_t *vm, const dpmi_stack_t *stack,
                                      uint32_t size, dpmi_stack_frame_t *frame)
{
    dpmi_descriptor_t descriptor;
    if (!stack || !frame || !size || size > sizeof(frame->bytes) ||
        !dpmi_stack_descriptor(vm, stack->ss, &descriptor)) return false;
    uint32_t mask = descriptor.flags_lim & DESC_32BIT ? UINT32_MAX : 0xFFFFu;
    uint32_t top = stack->esp & mask, linear;
    dpmi_client_buffer_t buffer;
    if (top < size || !dpmi_stack_region(&descriptor, top - size, size, &linear) ||
        !dpmi_linear_buffer(vm, linear, size, DOS_PAGE_USER | DOS_PAGE_WRITE, &buffer))
        return false;
    frame->buffer = buffer;
    frame->descriptor = descriptor;
    frame->cursor = (dpmi_stack_t){ stack->ss, (stack->esp & ~mask) | (top - size) };
    return true;
}

static bool dpmi_store_stack_frame(dos_vm_t *vm, dpmi_stack_frame_t *frame)
{
    return dpmi_buffer_transfer(vm, &frame->buffer, 0, frame->bytes,
                                 frame->buffer.size, true);
}

static void dpmi_enter_stack_frame(dos_vm_t *vm, const dpmi_stack_frame_t *frame)
{
    /* Keep the admitted descriptor even when the payload aliases a table.
     * Callers publish this cursor only after all output records are stored. */
    cpu8086_cache_segment(vm->cpu, 2, frame->cursor.ss, &frame->descriptor);
    vm->cpu->esp = frame->cursor.esp;
}

bool dpmi_push_stack_frame(dos_vm_t *vm, const dpmi_stack_t *stack,
                            const uint32_t *fields, unsigned count)
{
    if (!fields || !count || count > 16u || !vm) return false;
    unsigned width = vm->dpmi.is_32bit ? 4u : 2u;
    dpmi_stack_frame_t frame;
    if (!dpmi_prepare_stack_frame(vm, stack, count * width, &frame)) return false;
    for (unsigned i = 0; i < count; i++)
        for (unsigned b = 0; b < width; b++) frame.bytes[i * width + b] = fields[i] >> (8u * b);
    if (!dpmi_store_stack_frame(vm, &frame)) return false;
    dpmi_enter_stack_frame(vm, &frame);
    return true;
}

bool dpmi_push_stack_bytes(dos_vm_t *vm, const dpmi_stack_t *stack,
                            const uint8_t *bytes, uint32_t size)
{
    dpmi_stack_frame_t frame;
    if (!bytes || !dpmi_prepare_stack_frame(vm, stack, size, &frame)) return false;
    for (uint32_t i = 0; i < size; i++) frame.bytes[i] = bytes[i];
    if (!dpmi_store_stack_frame(vm, &frame)) return false;
    dpmi_enter_stack_frame(vm, &frame);
    return true;
}

bool dpmi_locked_stack_top(dos_vm_t *vm, uint32_t size, dpmi_stack_t *stack)
{
    if (!vm || !vm->cpu || !stack) return false;
    dpmi_state_t *dpmi = &vm->dpmi;
    dpmi_stack_t selected;
    if (dpmi->exception_depth || dpmi->callback_depth ||
        dpmi->interrupt_depth || dpmi->control_depth) {
        selected = vm->cpu->protected_mode
                 ? (dpmi_stack_t){ vm->cpu->ss, vm->cpu->esp }
                 : dpmi->suspended_stack;
    } else {
        selected.ss = dpmi_get_exception_stack_selector(vm);
        selected.esp = DPMI_EXCEPTION_STACK_SIZE;
    }
    dpmi_descriptor_t descriptor;
    if (!dpmi_stack_descriptor(vm, selected.ss, &descriptor)) return false;
    uint32_t top = descriptor.flags_lim & DESC_32BIT
                 ? selected.esp : (uint16_t)selected.esp;
    if (top < size || !dpmi_stack_span(vm, selected.ss, top - size, size, NULL))
        return false;
    *stack = selected;
    return true;
}

bool dpmi_has_real_timer_handler(const dos_vm_t *vm)
{
    if (!vm || !vm->dpmi.active) return false;
    const dpmi_state_t *dpmi = &vm->dpmi;
    return dpmi->pm_vectors[0x1C].sel &&
           !(dpmi->pm_vectors[0x1C].sel == dpmi->sel_host_code &&
             dpmi->pm_vectors[0x1C].off == DPMI_PM_REFLECT_BASE_OFF +
                                         0x1Cu * DPMI_PM_REFLECT_STUB_SIZE);
}

bool dpmi_owns_real_interrupt(const dos_vm_t *vm, uint8_t vector)
{
    if (!vm || !vm->dpmi.active) return false;
    /* The DOS extensions define default Ignore/Fail for protected clients,
     * even when a real-mode Ctrl+C/critical-error vector is installed. */
    return vector == 0x23 || vector == 0x24 ||
           (vector == 0x1C && dpmi_has_real_timer_handler(vm));
}

static bool dpmi_prepare_control_frame(dos_vm_t *vm, const dpmi_stack_t *stack,
                                        uint16_t host_cs, uint32_t return_off,
                                        uint32_t flags, const uint16_t *dos_tail,
                                        dpmi_stack_frame_t *frame)
{
    unsigned width = vm->dpmi.is_32bit ? 4u : 2u;
    unsigned size = 3u * width + (dos_tail ? 24u : 0u);
    if (!dpmi_prepare_stack_frame(vm, stack, size, frame)) return false;
    const uint32_t fields[] = { return_off, host_cs, flags };
    for (unsigned i = 0; i < 3; i++)
        for (unsigned b = 0; b < width; b++) frame->bytes[i * width + b] = fields[i] >> (8u * b);
    if (dos_tail) for (unsigned i = 0; i < 12; i++)
        for (unsigned b = 0; b < 2; b++) frame->bytes[3u * width + i * 2u + b] = dos_tail[i] >> (8u * b);
    return true;
}

bool dpmi_reflect_timer(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || cpu->protected_mode || !dpmi_has_real_timer_handler(vm))
        return false;
    dpmi_state_t *dpmi = &vm->dpmi;
    uint16_t selector = dpmi->pm_vectors[0x1C].sel;
    uint32_t offset = dpmi->pm_vectors[0x1C].off;
    cpu8086_state_t saved = *cpu;
    dpmi_apply_paging(cpu, dpmi->suspended_paging);
    uint16_t host_cs = dpmi_get_host_code_selector(vm);
    dpmi_stack_t stack;
    dpmi_stack_frame_t frame;
    uint32_t flags = vm->software_int_frame_bytes
                   ? vm->software_int_return_flags : cpu->eflags;
    if (!host_cs || !dpmi_code_target(vm, selector, offset) ||
        dpmi->control_depth >= DPMI_MAX_CALLBACKS ||
        !dpmi_locked_stack_top(vm, dpmi->is_32bit ? 12u : 6u, &stack) ||
        !dpmi_prepare_control_frame(vm, &stack, host_cs, DPMI_CONTROL_RETURN_OFF,
                                     flags, NULL, &frame) ||
        !dpmi_store_stack_frame(vm, &frame)) {
        *cpu = saved;
        return false;
    }

    dpmi_stack_t saved_real_stack = dpmi->real_mode_stack;
    dpmi_paging_t saved_real_paging = dpmi->real_mode_paging;
    bool saved_virtual_if = dpmi->virtual_interrupts_enabled;
    dpmi->real_mode_stack = (dpmi_stack_t){ cpu->ss, cpu->sp };
    dpmi->real_mode_paging = dpmi_current_paging(&saved);
    dpmi->control_depth++;
    dpmi->virtual_interrupts_enabled = (flags & FLAG_IF) != 0;
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->cpl = 3;
    dpmi_enter_stack_frame(vm, &frame);
    cpu->ds = cpu->es = cpu->fs = cpu->gs = 0;
    for (unsigned s = 0; s < 6; s++) if (s != 1 && s != 2)
        cpu8086_cache_segment(cpu, s, 0, NULL);
    cpu->eflags = (flags & ~FLAG_TF) | FLAGS_FIXED | FLAG_IF;
    cpu->cs = selector;
    cpu->eip = offset;
    cpu->halted = false;
    cpu8086_sync_cs(cpu);
    vm->native_dispatch_depth++;
    bool returned = cpu8086_run_until(vm, true, host_cs,
                                      DPMI_CONTROL_RETURN_OFF);
    vm->native_dispatch_depth--;
    bool valid = returned && cpu->ss == stack.ss && cpu->esp == stack.esp;
    saved.eax = cpu->eax; saved.ebx = cpu->ebx;
    saved.ecx = cpu->ecx; saved.edx = cpu->edx;
    saved.esi = cpu->esi; saved.edi = cpu->edi; saved.ebp = cpu->ebp;
    saved.insn_count = cpu->insn_count;
    if (!cpu->running) {
        saved.running = false;
        saved.exit_code = cpu->exit_code;
    }
    *cpu = saved;
    dpmi->control_depth--;
    dpmi->real_mode_stack = saved_real_stack;
    dpmi->real_mode_paging = saved_real_paging;
    dpmi->virtual_interrupts_enabled = saved_virtual_if;
    return valid;
}

static bool dpmi_invoke_control_break(dos_vm_t *vm, bool reflected)
{
    dpmi_state_t *dpmi = &vm->dpmi;
    cpu8086_state_t *cpu = vm->cpu;
    uint16_t selector = dpmi->pm_vectors[0x23].sel;
    uint32_t offset = dpmi->pm_vectors[0x23].off;
    if (!selector || (selector == dpmi->sel_host_code &&
        offset == DPMI_PM_REFLECT_BASE_OFF + 0x23u * DPMI_PM_REFLECT_STUB_SIZE))
        return true;

    cpu8086_state_t saved = *cpu;
    uint32_t flags = reflected ? vm->software_int_return_flags : saved.eflags & ~FLAG_CF;
    flags = (flags & ~FLAG_TF) | FLAGS_FIXED;
    if (!cpu->protected_mode) dpmi_apply_paging(cpu, dpmi->suspended_paging);
    uint16_t host_cs = dpmi_get_host_code_selector(vm);
    dpmi_stack_t stack;
    dpmi_stack_frame_t frame;
    if (!host_cs || !dpmi_code_target(vm, selector, offset) ||
        dpmi->control_depth >= DPMI_MAX_CALLBACKS ||
        !dpmi_locked_stack_top(vm, dpmi->is_32bit ? 12u : 6u, &stack) ||
        !dpmi_prepare_control_frame(vm, &stack, host_cs, DPMI_CONTROL_RETURN_OFF,
                                     flags, NULL, &frame) ||
        !dpmi_store_stack_frame(vm, &frame)) {
        *cpu = saved;
        return false;
    }

    dpmi_stack_t saved_real_stack = dpmi->real_mode_stack;
    dpmi_paging_t saved_real_paging = dpmi->real_mode_paging;
    if (!cpu->protected_mode) {
        dpmi->real_mode_stack = (dpmi_stack_t){ cpu->ss, cpu->sp };
        dpmi->real_mode_paging = dpmi_current_paging(&saved);
    }
    bool saved_virtual_if = dpmi->virtual_interrupts_enabled;
    dpmi->control_depth++;
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->cpl = 3;
    dpmi_enter_stack_frame(vm, &frame);
    if (!saved.protected_mode) {
        cpu->ds = cpu->es = cpu->fs = cpu->gs = 0;
        for (unsigned s = 0; s < 6; s++) if (s != 1 && s != 2)
            cpu8086_cache_segment(cpu, s, 0, NULL);
    }
    cpu->eflags = flags | FLAG_IF;
    dpmi->virtual_interrupts_enabled = reflected && (flags & FLAG_IF);
    cpu->cs = selector;
    cpu->eip = offset;
    cpu->halted = false;
    cpu8086_sync_cs(cpu);
    vm->native_dispatch_depth++;
    bool returned = cpu8086_run_until(vm, true, host_cs,
                                      DPMI_CONTROL_RETURN_OFF);
    vm->native_dispatch_depth--;
    /* DOS permits RETF as well as IRET for Ctrl+C. DPMI ignores CF in
     * either case; the leftover FLAGS word/dword is host-owned. */
    uint32_t far_sp = stack.esp;
    unsigned width = dpmi->is_32bit ? 4u : 2u;
    if (cpu_stack_addr32(cpu)) far_sp -= width;
    else far_sp = (far_sp & 0xFFFF0000u) | (uint16_t)(far_sp - width);
    bool valid = returned && cpu->ss == stack.ss &&
                 (cpu->esp == stack.esp || cpu->esp == far_sp);
    if (reflected && valid) {
        saved.eax = cpu->eax; saved.ebx = cpu->ebx;
        saved.ecx = cpu->ecx; saved.edx = cpu->edx;
        saved.esi = cpu->esi; saved.edi = cpu->edi; saved.ebp = cpu->ebp;
    }
    bool running = cpu->running;
    int32_t exit_code = cpu->exit_code;
    uint64_t instructions = cpu->insn_count;
    *cpu = saved;
    cpu->insn_count = instructions;
    if (!running) {
        cpu->running = false;
        cpu->exit_code = exit_code;
    }
    dpmi->control_depth--;
    dpmi->real_mode_stack = saved_real_stack;
    dpmi->real_mode_paging = saved_real_paging;
    dpmi->virtual_interrupts_enabled = saved_virtual_if;
    return valid;
}

bool dpmi_control_break(dos_vm_t *vm)
{
    return dpmi_invoke_control_break(vm, false);
}

static uint8_t dpmi_invoke_critical_error(dos_vm_t *vm, bool reflected)
{
    dpmi_state_t *dpmi = &vm->dpmi;
    cpu8086_state_t *cpu = vm->cpu;
    uint16_t selector = dpmi->pm_vectors[0x24].sel;
    uint32_t offset = dpmi->pm_vectors[0x24].off;
    if (!selector || (selector == dpmi->sel_host_code &&
        offset == DPMI_PM_REFLECT_BASE_OFF + 0x24u * DPMI_PM_REFLECT_STUB_SIZE))
        return 3u;

    cpu8086_state_t saved = *cpu;
    uint16_t real_frame[15];
    /* Snapshot the DOS words with the loaded real SS and its address space,
     * before switching to the suspended protected handler's page tables. */
    if (cpu->protected_mode || !cpu8086_read_stack_words(cpu, real_frame, 15)) {
        *cpu = saved;
        serial_puts("[DPMI] Invalid real INT 24h frame; failing request\n");
        return 3u;
    }
    dpmi_apply_paging(cpu, dpmi->suspended_paging);
    uint16_t host_cs = dpmi_get_host_code_selector(vm);
    dpmi_stack_t stack;
    dpmi_stack_frame_t frame;
    if (!host_cs ||
        !dpmi_code_target(vm, selector, offset) ||
        dpmi->control_depth >= DPMI_MAX_CALLBACKS ||
        !dpmi_locked_stack_top(vm, 24u + (dpmi->is_32bit ? 12u : 6u), &stack) ||
        !dpmi_prepare_control_frame(vm, &stack, host_cs, DPMI_CONTROL_RETURN_OFF,
                                     real_frame[2], real_frame + 3, &frame) ||
        !dpmi_store_stack_frame(vm, &frame)) {
        *cpu = saved;
        serial_puts("[DPMI] Invalid INT 24h vector or frame; failing request\n");
        return 3u;
    }

    dpmi_stack_t saved_real_stack = dpmi->real_mode_stack;
    dpmi_paging_t saved_real_paging = dpmi->real_mode_paging;
    dpmi->real_mode_stack = (dpmi_stack_t){ cpu->ss, cpu->sp };
    dpmi->real_mode_paging = dpmi_current_paging(&saved);
    bool saved_virtual_if = dpmi->virtual_interrupts_enabled;
    dpmi->control_depth++;
    /* Only the handler IRET expands for a 32-bit client. The nine saved
     * DOS registers and caller IRET remain words with real-mode segments. */
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->cpl = 3;
    dpmi_enter_stack_frame(vm, &frame);
    uint32_t return_sp = cpu_stack_addr32(cpu) ? stack.esp - 24u
                      : (stack.esp & 0xFFFF0000u) | (uint16_t)(stack.esp - 24u);
    cpu->ds = cpu->es = cpu->fs = cpu->gs = 0;
    for (unsigned s = 0; s < 6; s++) if (s != 1 && s != 2)
        cpu8086_cache_segment(cpu, s, 0, NULL);
    uint32_t flags = reflected ? real_frame[2] : saved.eflags;
    cpu->eflags = (flags & ~FLAG_TF) | FLAGS_FIXED | FLAG_IF;
    dpmi->virtual_interrupts_enabled = reflected && (flags & FLAG_IF);
    cpu->ebp = cpu_stack_offset(cpu);
    cpu->cs = selector;
    cpu->eip = offset;
    cpu->halted = false;
    cpu8086_sync_cs(cpu);
    vm->native_dispatch_depth++;
    bool returned = cpu8086_run_until(vm, true, host_cs,
                                      DPMI_CONTROL_RETURN_OFF);
    vm->native_dispatch_depth--;
    bool valid = returned && cpu->ss == stack.ss && cpu->esp == return_sp;
    uint8_t action = cpu->al;
    bool running = cpu->running;
    int32_t exit_code = cpu->exit_code;
    uint64_t instructions = cpu->insn_count;
    *cpu = saved;
    cpu->insn_count = instructions;
    if (!running) {
        cpu->running = false;
        cpu->exit_code = exit_code;
    }
    dpmi->control_depth--;
    dpmi->real_mode_stack = saved_real_stack;
    dpmi->real_mode_paging = saved_real_paging;
    dpmi->virtual_interrupts_enabled = saved_virtual_if;
    if (!valid) {
        serial_puts("[DPMI] Invalid INT 24h return; failing device request\n");
        return 3u;
    }
    /* DPMI translates Abort (including DOS's unknown-response policy) to
     * Fail. Only Ignore and Retry retain their real-mode meanings. */
    return action < 2u ? action : 3u;
}

uint8_t dpmi_critical_error(dos_vm_t *vm)
{
    return dpmi_invoke_critical_error(vm, false);
}

bool dpmi_reflect_dos_interrupt(dos_vm_t *vm, uint8_t vector)
{
    if (!vm || !vm->cpu || vm->cpu->protected_mode || !vm->dpmi.active) return false;
    if (vector == 0x23) return dpmi_invoke_control_break(vm, true);
    if (vector != 0x24) return false;
    uint8_t action = dpmi_invoke_critical_error(vm, true);
    vm->cpu->al = action;
    return true;
}

static void dpmi_read_rm_regs(dos_vm_t *vm, uint32_t address,
                              dpmi_rm_regs_t *regs)
{
    uint8_t *bytes = (uint8_t *)regs;
    for (uint64_t i = 0; i < sizeof(*regs); i++)
        bytes[i] = dos_mem_read8(vm, address + (uint32_t)i);
}

static void dpmi_write_rm_regs(dos_vm_t *vm, uint32_t address,
                               const dpmi_rm_regs_t *regs)
{
    const uint8_t *bytes = (const uint8_t *)regs;
    for (uint64_t i = 0; i < sizeof(*regs); i++)
        dos_mem_write8(vm, address + (uint32_t)i, bytes[i]);
}

dpmi_service_result_t dpmi_callback_enter(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    dpmi_state_t *dpmi = vm ? &vm->dpmi : NULL;
    if (!cpu || !dpmi || cpu->protected_mode ||
        cpu->cs != DPMI_ENTRY_SEG || cpu->ip < 2u ||
        dpmi->callback_depth >= DPMI_MAX_CALLBACKS)
        return DPMI_SERVICE_INVALID;

    uint8_t slot = dos_mem_read8(vm, dos_linear(cpu->cs, cpu->ip));
    if (slot >= DPMI_MAX_CALLBACKS)
        return DPMI_SERVICE_INVALID;

    dpmi_callback_t *callback = &dpmi->callbacks[slot];
    if (!callback->active || callback->rm_seg != cpu->cs ||
        callback->rm_off + 2u != cpu->ip)
        return DPMI_SERVICE_INVALID;

    cpu8086_state_t saved = *cpu;
    uint64_t generation = callback->generation;
    uint8_t entry_depth = dpmi->callback_depth;
    bool frame_consumed = false;
    uint16_t private_frame[3];
    if (!cpu8086_read_stack_words(cpu, private_frame, 3)) goto invalid;
    dpmi_client_buffer_t regs_buffer;
    dpmi_descriptor_t regs_descriptor;
    for (;;) {
        /* Both admissions must be repeated after a repair. No record,
         * locked-stack frame or callback depth is published until both pass. */
        saved = *cpu;
        dpmi_apply_paging(cpu, dpmi->suspended_paging);
        dpmi_buffer_fault_t fault;
        if (dpmi_code_target_probe(vm, callback->pm_sel, callback->pm_off, &fault) &&
            dpmi_client_buffer_probe(vm, callback->rm_regs_sel, callback->rm_regs_off,
                                       sizeof(dpmi_rm_regs_t), true, &regs_buffer, &fault)) break;
        *cpu = saved;
        if (!frame_consumed) {
            cpu_stack_adjust(cpu, 6u);
            cpu->flags = private_frame[2] | FLAGS_FIXED;
            vm->software_int_frame_bytes = 0;
            frame_consumed = true;
        }
        if (!dpmi_real_service_exception(vm, fault.vector, fault.error, fault.linear))
            return DPMI_SERVICE_INTERRUPTED;
        cpu = vm->cpu;
        /* A handler may free and reuse this slot while entry is suspended.
         * The same real far pointer must not retarget the pending callback. */
        if (!callback->active || callback->generation != generation ||
            dpmi->callback_depth != entry_depth) return DPMI_SERVICE_INVALID;
    }
    if (!dpmi_guest_descriptor(vm, callback->rm_regs_sel, &regs_descriptor))
        goto invalid;

    dpmi_stack_t stack;
    dpmi_stack_frame_t frame;
    uint16_t host_code_sel = dpmi_get_host_code_selector(vm);
    uint16_t rm_stack_index;
    if (!host_code_sel ||
        !dpmi_locked_stack_top(vm, dpmi->is_32bit ? 12u : 6u, &stack) ||
        !dpmi_selector_allocated(dpmi, callback->rm_stack_sel,
                                 &rm_stack_index) ||
        dpmi->descriptor_state[rm_stack_index] != DPMI_DESC_HOST ||
        !dpmi_prepare_control_frame(vm, &stack, host_code_sel, DPMI_CALLBACK_RETURN_OFF,
                                     FLAGS_FIXED, NULL, &frame))
        goto invalid;

    uint16_t caller_sp = (uint16_t)(cpu->sp + (frame_consumed ? 0u : 6u));
    dpmi_rm_regs_t regs;
    dpmi_zero(&regs, sizeof(regs));
    regs.eax = cpu->eax;
    regs.ebx = cpu->ebx;
    regs.ecx = cpu->ecx;
    regs.edx = cpu->edx;
    regs.esi = cpu->esi;
    regs.edi = cpu->edi;
    regs.ebp = cpu->ebp;
    regs.flags = frame_consumed ? cpu->flags : private_frame[2];
    regs.es = cpu->es;
    regs.ds = cpu->ds;
    regs.fs = cpu->fs;
    regs.gs = cpu->gs;
    regs.ip = private_frame[0];
    regs.cs = private_frame[1];
    regs.sp = caller_sp;
    regs.ss = cpu->ss;
    /* Either output can alias the other's paging metadata. Commit both
     * admitted snapshots' A/D bits before writing either payload. */
    if (!dpmi_buffer_commit(vm, &regs_buffer, 0, sizeof(regs), true) ||
        !dpmi_buffer_commit(vm, &frame.buffer, 0, frame.buffer.size, true) ||
        !dpmi_buffer_transfer(vm, &regs_buffer, 0, &regs, sizeof(regs), true) ||
        !dpmi_store_stack_frame(vm, &frame)) goto invalid;

    dpmi_build_desc(&dpmi->ldt[rm_stack_index], (uint32_t)cpu->ss << 4,
                    0xFFFFu,
                    DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                    DESC_WRITABLE,
                    0);
    dpmi->descriptor_state[rm_stack_index] = DPMI_DESC_HOST;

    uint8_t depth = dpmi->callback_depth;
    dpmi->callback_slots[depth] = slot;
    dpmi->callback_virtual_interrupts[depth] = dpmi->virtual_interrupts_enabled;
    dpmi->callback_real_stacks[depth] = dpmi->real_mode_stack;
    dpmi->callback_real_paging[depth] = dpmi->real_mode_paging;
    dpmi->real_mode_stack = (dpmi_stack_t){ cpu->ss, caller_sp };
    dpmi->real_mode_paging = dpmi_current_paging(&saved);
    dpmi->callback_depth++;

    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->cpl = 3;
    dpmi_enter_stack_frame(vm, &frame);
    cpu->eflags = FLAGS_FIXED;
    dpmi->virtual_interrupts_enabled = false;
    serial_puts("[DPMI] Callback enter slot=");
    serial_putdec(slot);
    serial_puts(" SS:SP=");
    serial_puthex(cpu->ss, 4);
    serial_puts(":");
    serial_puthex(cpu_stack_offset(cpu), 8);
    serial_puts(" frame=");
    serial_puthex(dpmi_buffer_load(vm, &frame.buffer, 0, dpmi->is_32bit ? 4u : 2u), 8);
    serial_puts("\n");

    cpu->ds = callback->rm_stack_sel;
    cpu->esi = caller_sp;
    cpu->es = callback->rm_regs_sel;
    cpu->edi = callback->rm_regs_off;
    cpu->fs = 0;
    cpu->gs = 0;
    cpu->cs = callback->pm_sel;
    cpu->eip = callback->pm_off;
    cpu->halted = false;
    cpu8086_sync_cs(cpu);
    cpu8086_cache_segment(cpu, 0, callback->rm_regs_sel, &regs_descriptor);
    cpu8086_cache_segment(cpu, 3, callback->rm_stack_sel, &dpmi->ldt[rm_stack_index]);
    cpu8086_cache_segment(cpu, 4, 0, NULL);
    cpu8086_cache_segment(cpu, 5, 0, NULL);
    return DPMI_SERVICE_COMPLETE;

invalid:
    *cpu = saved;
    return DPMI_SERVICE_INVALID;
}

dpmi_service_result_t dpmi_callback_return(dos_vm_t *vm, bool discard_private_int_frame)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    dpmi_state_t *dpmi = vm ? &vm->dpmi : NULL;
    if (!cpu || !dpmi || !cpu->protected_mode ||
        !dpmi->callback_depth || dpmi->callback_depth > DPMI_MAX_CALLBACKS ||
        cpu->cs != dpmi->sel_host_code ||
        cpu->eip != DPMI_CALLBACK_RETURN_OFF + 2u)
        return DPMI_SERVICE_INVALID;

    uint8_t depth = dpmi->callback_depth;
    uint8_t slot = dpmi->callback_slots[depth - 1u];
    if (slot >= DPMI_MAX_CALLBACKS || !dpmi->callbacks[slot].active)
        return DPMI_SERVICE_INVALID;

    if (discard_private_int_frame) {
        uint32_t frame_size = dpmi->is_32bit ? 12u : 6u;
        dpmi_client_buffer_t private_frame;
        if (!dpmi_stack_buffer(vm, cpu->ss, cpu_stack_offset(cpu),
                               frame_size, false, &private_frame))
            return DPMI_SERVICE_INVALID;
    }

    /* IRET selected this record and real-mode continuation already. A
     * repair handler can nest callbacks or edit ES:EDI without retargeting
     * the pending copy. Do not pop a callback until the whole record is read. */
    uint16_t regs_selector = cpu->es;
    uint32_t regs_offset = dpmi->is_32bit ? cpu->edi : cpu->di;
    dpmi_paging_t real_paging = dpmi->real_mode_paging;
    dpmi_client_buffer_t regs_buffer;
    dpmi_buffer_fault_t fault;
    while (!dpmi_client_buffer_probe(vm, regs_selector, regs_offset,
                                      sizeof(dpmi_rm_regs_t), false,
                                      &regs_buffer, &fault)) {
        if (!dpmi_service_exception(vm, fault.vector, fault.error, fault.linear))
            return DPMI_SERVICE_INTERRUPTED;
        cpu = vm->cpu;
        if (dpmi->callback_depth != depth || dpmi->callback_slots[depth - 1u] != slot ||
            !dpmi->callbacks[slot].active)
            return DPMI_SERVICE_INTERRUPTED;
    }
    dpmi_rm_regs_t regs;
    if (!dpmi_buffer_transfer(vm, &regs_buffer, 0, &regs, sizeof(regs), false))
        return DPMI_SERVICE_INVALID;
    dpmi->suspended_paging = dpmi_current_paging(cpu);
    dpmi->callback_depth--;
    dpmi->real_mode_stack = dpmi->callback_real_stacks[dpmi->callback_depth];
    dpmi->real_mode_paging = dpmi->callback_real_paging[dpmi->callback_depth];
    dpmi->virtual_interrupts_enabled =
        dpmi->callback_virtual_interrupts[dpmi->callback_depth];

    cpu->eax = regs.eax;
    cpu->ebx = regs.ebx;
    cpu->ecx = regs.ecx;
    cpu->edx = regs.edx;
    cpu->esi = regs.esi;
    cpu->edi = regs.edi;
    cpu->ebp = regs.ebp;
    cpu->es = regs.es;
    cpu->ds = regs.ds;
    cpu->fs = regs.fs;
    cpu->gs = regs.gs;
    cpu->ss = regs.ss;
    cpu->esp = regs.sp;
    cpu->cs = regs.cs;
    cpu->eip = regs.ip;
    cpu->eflags = (regs.flags & 0x0FFFu) | FLAGS_FIXED;
    cpu->protected_mode = false;
    cpu->pm_cs_loaded = false;
    cpu->op_size_32 = false;
    cpu->addr_size_32 = false;
    dpmi_apply_paging(cpu, real_paging);
    cpu8086_reset_real_cs(cpu, cpu->cs);
    cpu8086_sync_data(cpu);
    cpu->halted = false;
    return DPMI_SERVICE_COMPLETE;
}

/* Real frames and each DOS I/O chunk fit in 64 KiB. Linear misalignment
 * can add a seventeenth page; the complete request may contain more chunks. */
typedef struct {
    dpmi_buffer_page_t pages[0x10000u / DPMI_EXT_PAGE_SIZE + 1u];
    unsigned count;
} dpmi_page_span_t;

static bool dpmi_page_span_probe_fault(dos_vm_t *vm, uint32_t linear, uint32_t size,
                                        unsigned access, dpmi_page_span_t *buffer,
                                        dpmi_buffer_fault_t *fault)
{
    dpmi_buffer_fault(fault, 13, 0, linear);
    *buffer = (dpmi_page_span_t){0};
    if (size > 0x10000u || (size && (uint64_t)linear + size - 1u > UINT32_MAX)) return false;
    while (size) {
        if (buffer->count >= sizeof(buffer->pages) / sizeof(buffer->pages[0])) return false;
        dpmi_buffer_page_t *page = &buffer->pages[buffer->count++];
        page->size = DPMI_EXT_PAGE_SIZE - (linear & 0xFFFu);
        if (page->size > size) page->size = size;
        dos_page_fault_t page_fault = {0};
        if (!dos_page_probe(vm, linear, access, &page->translation, &page_fault))
            return dpmi_buffer_fault(fault, 14, page_fault.error, page_fault.linear);
        if ((page->translation.pde != UINT32_MAX &&
             (!dpmi_range_valid(vm, page->translation.pde, 4u) ||
              !dpmi_range_valid(vm, page->translation.pte, 4u))) ||
            !dpmi_range_valid(vm, page->translation.physical, page->size)) return false;
        linear += page->size;
        size -= page->size;
    }
    return true;
}

static bool dpmi_page_span_probe(dos_vm_t *vm, uint32_t linear, uint32_t size,
                                  unsigned access, dpmi_page_span_t *buffer)
{
    return dpmi_page_span_probe_fault(vm, linear, size, access, buffer, NULL);
}

static bool dpmi_argument_span(const cpu8086_state_t *cpu, uint32_t offset,
                                uint32_t size, uint32_t *linear)
{
    if (!size) { *linear = 0; return true; }
    const dpmi_descriptor_t *d = &cpu->ss_cache.descriptor;
    uint8_t access = d->access;
    if (!cpu->ss_cache.valid || (access & (DESC_PRESENT | DESC_SEGMENT | DESC_CODE | DESC_WRITABLE)) !=
        (DESC_PRESENT | DESC_SEGMENT | DESC_WRITABLE)) return false;
    uint32_t top = d->flags_lim & DESC_32BIT ? UINT32_MAX : 0xFFFFu;
    uint32_t base = dpmi_desc_get_base(d), limit = dpmi_desc_get_limit(d);
    uint64_t last = (uint64_t)offset + size - 1u;
    if (last > top || (access & 4u ? offset <= limit : last > limit) ||
        (uint64_t)base + last > UINT32_MAX) return false;
    *linear = base + offset;
    return true;
}

static void dpmi_page_span_commit(dos_vm_t *vm, const dpmi_page_span_t *buffer, bool write)
{
    for (unsigned i = 0; i < buffer->count; i++)
        dos_page_commit(vm, &buffer->pages[i].translation, write);
}

static void dpmi_page_span_transfer(dos_vm_t *vm, const dpmi_page_span_t *buffer,
                                     uint8_t *bytes, bool write)
{
    for (unsigned i = 0; i < buffer->count; i++) {
        const dpmi_buffer_page_t *page = &buffer->pages[i];
        for (uint32_t n = 0; n < page->size; n++) {
            uint32_t physical = page->translation.physical + n;
            if (write) dos_mem_write8(vm, physical, *bytes++);
            else *bytes++ = dos_mem_read8(vm, physical);
        }
    }
}

static uint16_t dpmi_simulate_rm_call(dos_vm_t *vm, dpmi_rm_regs_t *regs,
                                  uint16_t copy_words,
                                  dpmi_rm_call_kind_t kind,
                                  uint8_t interrupt_number)
{
    cpu8086_state_t *cpu = vm->cpu;
    cpu8086_state_t protected_state = *cpu;
    uint32_t source_stack = cpu_stack_offset(cpu);
    /* Native gates keep their frame on the host stack; interpreted INT 31h
     * leaves a private IRET below the caller's actual argument words. */
    if (copy_words)
        source_stack = (source_stack + vm->software_int_frame_bytes) &
                       (cpu_stack_addr32(cpu) ? UINT32_MAX : 0xFFFFu);
    uint32_t copied_bytes = (uint32_t)copy_words * 2u;
    uint32_t frame_bytes = kind == DPMI_RM_CALL_FAR ? 4u : 6u;

    uint16_t real_ss = regs->ss;
    uint16_t real_sp = regs->sp;
    if (!real_ss && !real_sp) {
        real_ss = vm->dpmi.real_mode_stack.ss;
        real_sp = (uint16_t)vm->dpmi.real_mode_stack.esp;
    }

    if (copied_bytes + frame_bytes > real_sp)
        return 0x8021;

    uint16_t arguments_sp = (uint16_t)(real_sp - copied_bytes);
    uint16_t stack_low = (uint16_t)(arguments_sp - frame_bytes);
    uint32_t source_linear;
    dpmi_page_span_t source, destination, vector = {0}, target = {0};
    if (!dpmi_argument_span(cpu, source_stack, copied_bytes, &source_linear)) return 0x8021;
    if (!dpmi_page_span_probe(vm, source_linear, copied_bytes, DOS_PAGE_USER, &source)) return 0x8012;
    /* Probe both address spaces without publishing a mode switch. No guest
     * payload or A/D metadata is changed until the complete call is admitted. */
    dpmi_apply_paging(cpu, vm->dpmi.real_mode_paging);
    uint16_t target_cs = regs->cs;
    uint16_t target_ip = regs->ip;
    bool host_service = false;
    uint16_t admission_error = 0;
    if (kind == DPMI_RM_CALL_INTERRUPT) {
        host_service = dpmi_owns_real_interrupt(vm, interrupt_number);
        if (!host_service) {
            uint8_t bytes[4];
            if (!dpmi_page_span_probe(vm, interrupt_number * 4u, 4u, DOS_PAGE_READ, &vector)) {
                admission_error = 0x8012;
                goto admission_done;
            }
            dpmi_page_span_transfer(vm, &vector, bytes, false);
            target_ip = bytes[0] | ((uint16_t)bytes[1] << 8);
            target_cs = bytes[2] | ((uint16_t)bytes[3] << 8);
        }
    }
    if (!host_service) {
        uint32_t linear = dos_linear(target_cs, target_ip);
        if (!dpmi_page_span_probe(vm, linear, 1u, DOS_PAGE_READ, &target)) {
            admission_error = 0x8021;
            goto admission_done;
        }
        if (kind == DPMI_RM_CALL_INTERRUPT &&
            dos_rm_host_vector(vm, interrupt_number, target_cs, target_ip)) {
            dpmi_page_span_t stub;
            uint8_t bytes[3];
            if (dpmi_page_span_probe(vm, linear, 3u, DOS_PAGE_READ, &stub)) {
                dpmi_page_span_transfer(vm, &stub, bytes, false);
                if (bytes[0] == 0xCD && bytes[1] == DOS_RM_SERVICE_INT && bytes[2] == 0xCF) {
                    target = stub;
                    host_service = true;
                }
            }
        }
    }
    if (host_service) {
        target_cs = DPMI_ENTRY_SEG;
        target_ip = DPMI_RM_RETURN_OFF;
    }
    if (!dpmi_page_span_probe(vm, dos_linear(real_ss, stack_low),
                                  frame_bytes + copied_bytes, DOS_PAGE_WRITE, &destination))
        admission_error = 0x8012;
admission_done:
    dpmi_apply_paging(cpu, dpmi_current_paging(&protected_state));
    if (admission_error) return admission_error;

    uint8_t frame[6], *image = frame;
    unsigned pages = (frame_bytes + copied_bytes + 4095u) / 4096u;
    if (copied_bytes) {
        image = dos_host_alloc_pages(pages);
        if (!image) return 0x8013;
    }
    dpmi_page_span_commit(vm, &vector, false);
    dpmi_page_span_commit(vm, &target, false);
    dpmi_page_span_commit(vm, &source, false);
    dpmi_page_span_commit(vm, &destination, true);
    /* Snapshot all arguments before any stores: virtual ranges can alias
     * through arbitrary physical page permutations, not just +/- offsets. */
    dpmi_page_span_transfer(vm, &source, image + frame_bytes, false);
    uint16_t fields[] = { DPMI_RM_RETURN_OFF, DPMI_ENTRY_SEG, regs->flags | FLAGS_FIXED };
    for (unsigned i = 0; i < frame_bytes; i++) image[i] = (uint8_t)(fields[i / 2u] >> (8u * (i & 1u)));
    dpmi_page_span_transfer(vm, &destination, image, true);
    if (copied_bytes) dos_host_free_pages(image, pages);

    dpmi_stack_t previous_stack = vm->dpmi.suspended_stack;
    dpmi_stack_t previous_real_stack = vm->dpmi.real_mode_stack;
    dpmi_paging_t previous_paging = vm->dpmi.suspended_paging;
    dpmi_paging_t previous_real_paging = vm->dpmi.real_mode_paging;
    bool previous_virtual_if = vm->dpmi.virtual_interrupts_enabled;
    vm->dpmi.suspended_stack = (dpmi_stack_t){ cpu->ss, cpu->esp };
    vm->dpmi.suspended_paging = dpmi_current_paging(cpu);
    cpu->eax = regs->eax;
    cpu->ebx = regs->ebx;
    cpu->ecx = regs->ecx;
    cpu->edx = regs->edx;
    cpu->esi = regs->esi;
    cpu->edi = regs->edi;
    cpu->ebp = regs->ebp;
    cpu->ds = regs->ds;
    cpu->es = regs->es;
    cpu->fs = regs->fs;
    cpu->gs = regs->gs;
    cpu->ss = real_ss;
    cpu->esp = stack_low;
    cpu->cs = target_cs;
    cpu8086_reset_real_cs(cpu, target_cs);
    cpu->eip = target_ip;
    cpu->eflags = (uint32_t)regs->flags | FLAGS_FIXED;
    cpu->protected_mode = false;
    cpu->pm_cs_loaded = false;
    cpu->op_size_32 = false;
    cpu->addr_size_32 = false;
    dpmi_apply_paging(cpu, vm->dpmi.real_mode_paging);
    cpu->halted = false;
    cpu8086_sync_data(cpu);

    if (kind != DPMI_RM_CALL_FAR) cpu->flags &= ~(FLAG_IF | FLAG_TF);
    if (host_service) cpu8086_real_host_interrupt(cpu, interrupt_number, regs->flags | FLAGS_FIXED);

    bool reached = cpu8086_run_until_real(vm, DPMI_ENTRY_SEG,
                                          DPMI_RM_RETURN_OFF);
    reached = reached && cpu->ss == real_ss && cpu_stack_offset(cpu) == arguments_sp;
    bool nested_running = cpu->running;
    int32_t nested_exit_code = cpu->exit_code;
    uint64_t instruction_count = cpu->insn_count;

    regs->eax = cpu->eax;
    regs->ebx = cpu->ebx;
    regs->ecx = cpu->ecx;
    regs->edx = cpu->edx;
    regs->esi = cpu->esi;
    regs->edi = cpu->edi;
    regs->ebp = cpu->ebp;
    regs->flags = cpu->flags;
    regs->ds = cpu->ds;
    regs->es = cpu->es;
    regs->fs = cpu->fs;
    regs->gs = cpu->gs;

    *cpu = protected_state;
    vm->dpmi.suspended_stack = previous_stack;
    vm->dpmi.real_mode_stack = previous_real_stack;
    vm->dpmi.suspended_paging = previous_paging;
    vm->dpmi.real_mode_paging = previous_real_paging;
    vm->dpmi.virtual_interrupts_enabled = previous_virtual_if;
    cpu->insn_count = instruction_count;
    if (!nested_running) {
        cpu->running = false;
        cpu->exit_code = nested_exit_code;
    }
    return reached ? 0 : 0x8021;
}

static bool dpmi_io_buffer_probe(dos_vm_t *vm, uint16_t selector,
                                   uint32_t offset, uint32_t size, bool writable,
                                   dpmi_buffer_fault_t *fault)
{
    if (!size) return true;
    uint32_t linear;
    uint64_t available;
    if (!dpmi_client_region_probe(vm, selector, offset, writable, &linear, &available, fault) ||
        size > available) return false;
    dpmi_page_span_t span;
    while (size) {
        uint32_t part = size > 0x10000u ? 0x10000u : size;
        if (!dpmi_page_span_probe_fault(vm, linear, part,
            DOS_PAGE_USER | (writable ? DOS_PAGE_WRITE : DOS_PAGE_READ), &span, fault)) return false;
        linear += part;
        size -= part;
    }
    return true;
}

static bool dpmi_io_buffer_valid(dos_vm_t *vm, uint16_t selector,
                                   uint32_t offset, uint32_t size, bool writable)
{
    return dpmi_io_buffer_probe(vm, selector, offset, size, writable, NULL);
}

static bool dpmi_service_io_buffer(dos_vm_t *vm, uint16_t selector,
                                     uint32_t offset, uint32_t size, bool writable)
{
    dpmi_buffer_fault_t fault;
    while (!dpmi_io_buffer_probe(vm, selector, offset, size, writable, &fault)) {
        if (!dpmi_service_exception(vm, fault.vector, fault.error, fault.linear)) return false;
    }
    return vm->cpu->running;
}

/* Opaque 0305h record: dormant stack and address-space state.
 * Raw switches supply CS:IP and segments explicitly. Continuations and
 * nesting ownership stay on the host stack, never in a guest-owned buffer. */
typedef struct __attribute__((packed)) {
    uint32_t signature, esp;
    uint16_t ss;
    uint8_t protected_mode, reserved;
    uint32_t cr0, cr3;
} dpmi_saved_state_t;

dpmi_service_result_t dpmi_save_restore_state(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !vm->mem || !vm->dpmi.active || cpu->al > 1u)
        return DPMI_SERVICE_INVALID;
    bool save = cpu->al == 0;
    bool protected_mode = cpu->protected_mode;
    uint16_t selector = cpu->es;
    uint32_t offset = protected_mode && vm->dpmi.is_32bit
                    ? cpu->edi : cpu->di;
    dpmi_stack_t *other = protected_mode ? &vm->dpmi.real_mode_stack
                                         : &vm->dpmi.suspended_stack;
    dpmi_paging_t *paging = protected_mode ? &vm->dpmi.real_mode_paging
                                           : &vm->dpmi.suspended_paging;
    /* A repair handler can itself save/restore or switch modes. Capture the
     * original operation and save payload before yielding to that handler. */
    dpmi_saved_state_t state = { 0x32535044u, other->esp, other->ss,
                                 !protected_mode, 0, paging->cr0, paging->cr3 };
    dpmi_client_buffer_t buffer;
    if (protected_mode) {
        dpmi_buffer_fault_t fault;
        while (!dpmi_client_buffer_probe(vm, selector, offset, sizeof(state),
                                         save, &buffer, &fault)) {
            if (!dpmi_service_exception(vm, fault.vector, fault.error, fault.linear))
                return DPMI_SERVICE_INTERRUPTED;
        }
    } else {
        unsigned access = (save ? DOS_PAGE_WRITE : DOS_PAGE_READ) |
                          (cpu8086_cpl(cpu) == 3u ? DOS_PAGE_USER : 0u);
        if (offset > 0x10000u - sizeof(dpmi_saved_state_t) ||
            !dpmi_linear_buffer(vm, dos_linear(selector, (uint16_t)offset),
                                 sizeof(dpmi_saved_state_t), access, &buffer))
            return DPMI_SERVICE_INVALID;
    }

    (void)dpmi_buffer_transfer(vm, &buffer, 0, &state, sizeof(state), save);
    if (!save) {
        if (state.signature != 0x32535044u || state.reserved ||
            state.protected_mode != !protected_mode ||
            (state.cr0 & 1u) != (unsigned)state.protected_mode ||
            (!state.protected_mode && (state.esp > 0xFFFFu || (state.cr0 & DOS_CR0_PG))))
            return DPMI_SERVICE_INVALID;
        /* Validate the cursor's descriptor/range when consuming it, just
         * like a suspended selector that a client might later free. */
        *other = (dpmi_stack_t){ state.ss, state.esp };
        *paging = (dpmi_paging_t){ state.cr0, state.cr3 };
    }
    return DPMI_SERVICE_COMPLETE;
}

static bool dpmi_copy_io_buffer(dos_vm_t *vm, uint16_t selector,
                                uint32_t offset, uint32_t real_address,
                                uint32_t size, bool to_real, uint8_t *scratch)
{
    if (!size) return true;
    uint32_t linear;
    uint64_t available;
    dpmi_page_span_t span;
    if (!scratch || size > 0x10000u || !dpmi_range_valid(vm, real_address, size)) return false;
    /* Freeze completed real-mode output before running a repair handler:
     * nested DOS calls or physical aliases can overwrite the bounce buffer. */
    if (!to_real)
        for (uint32_t i = 0; i < size; i++) scratch[i] = dos_mem_read8(vm, real_address + i);
    for (;;) {
        dpmi_buffer_fault_t fault;
        if (dpmi_client_region_probe(vm, selector, offset, !to_real, &linear, &available, &fault) &&
            size <= available && dpmi_page_span_probe_fault(vm, linear, size,
                DOS_PAGE_USER | (to_real ? DOS_PAGE_READ : DOS_PAGE_WRITE), &span, &fault)) break;
        if (!dpmi_service_exception(vm, fault.vector, fault.error, fault.linear)) return false;
    }
    /* The conventional allocation belongs to this host call. Capture the
     * protected translations and snapshot a whole chunk before writing:
     * physical aliases may overlap the payload or its own page tables. */
    dpmi_page_span_commit(vm, &span, !to_real);
    if (to_real) {
        dpmi_page_span_transfer(vm, &span, scratch, false);
        for (uint32_t i = 0; i < size; i++)
            dos_mem_write8(vm, real_address + i, scratch[i]);
    } else {
        dpmi_page_span_transfer(vm, &span, scratch, true);
    }
    return true;
}

uint16_t dpmi_dos_file_io(dos_vm_t *vm, uint8_t read_terminator)
{
    enum { IO_BUFFER_MAX = 0xFFF0u, IO_STACK_PARAGRAPHS = 0x100u };
    cpu8086_state_t *cpu = vm->cpu;
    cpu8086_state_t request = *cpu;
    bool read = request.ah == 0x3Fu;
    uint32_t count = vm->dpmi.is_32bit ? request.ecx : request.cx;
    uint32_t offset = vm->dpmi.is_32bit ? request.edx : request.dx;
    if (!dpmi_service_io_buffer(vm, request.ds, offset, count, read)) return 0;

    uint32_t capacity = count < IO_BUFFER_MAX ? count : IO_BUFFER_MAX;
    uint16_t paragraphs = (uint16_t)((capacity + 15u) / 16u);
    uint16_t largest = 0;
    uint16_t segment = dos_mem_alloc(vm, paragraphs + IO_STACK_PARAGRAPHS,
                                     &largest);
    if (!segment && largest > IO_STACK_PARAGRAPHS) {
        paragraphs = largest - IO_STACK_PARAGRAPHS;
        segment = dos_mem_alloc(vm, largest, NULL);
    }
    if (!segment) return DOS_ERR_NOT_ENOUGH_MEMORY;
    capacity = (uint32_t)paragraphs * 16u;
    unsigned scratch_pages = (capacity + 4095u) / 4096u;
    uint8_t *scratch = scratch_pages ? dos_host_alloc_pages(scratch_pages) : NULL;
    if (scratch_pages && !scratch) {
        dos_mem_free(vm, segment);
        return DOS_ERR_NOT_ENOUGH_MEMORY;
    }
    uint32_t real_address = dos_linear(segment, 0);
    dpmi_rm_regs_t regs = {
        .edi = request.edi, .esi = request.esi, .ebp = request.ebp,
        .ebx = request.ebx, .edx = request.edx, .ecx = request.ecx,
        .eax = request.eax, .flags = request.flags,
        .ds = segment, .ss = (uint16_t)(segment + paragraphs),
        .sp = IO_STACK_PARAGRAPHS * 16u - 2u
    };
    uint32_t transferred = 0;
    uint16_t error = 0;
    bool cancelled = false;
    do {
        uint32_t part = count - transferred;
        if (part > capacity) part = capacity;
        uint32_t position = offset + transferred;
        if (!dpmi_service_io_buffer(vm, request.ds, position, part, read)) {
            cancelled = true;
            break;
        }
        /* Initializing read buffers preserves untouched bytes on Ignore
         * and partial failures, for which DOS does not return a byte count. */
        if (!dpmi_copy_io_buffer(vm, request.ds, position, real_address, part, true, scratch)) {
            cancelled = true;
            break;
        }
        regs.eax = (regs.eax & 0xFFFF0000u) | request.ax;
        regs.ebx = (regs.ebx & 0xFFFF0000u) | request.bx;
        regs.ecx = (regs.ecx & 0xFFFF0000u) | part;
        regs.edx &= 0xFFFF0000u;
        regs.ds = segment;
        if (dpmi_simulate_rm_call(vm, &regs, 0,
                                   DPMI_RM_CALL_INTERRUPT, 0x21u)) {
            error = DOS_ERR_INVALID_DATA;
            break;
        }
        if (!cpu->running) { cancelled = true; break; }
        uint32_t completed = regs.eax & 0xFFFFu;
        bool failed = (regs.flags & FLAG_CF) != 0;
        if (!failed && completed > part) {
            error = DOS_ERR_INVALID_DATA;
            break;
        }
        if (read) {
            uint32_t copied = failed ? part : completed;
            if (!dpmi_copy_io_buffer(vm, request.ds, position, real_address,
                                     copied, false, scratch)) {
                cancelled = true;
                break;
            }
        }
        if (failed) break;
        transferred += completed;
        if (completed < part) break;
        if (completed && read_terminator) {
            uint8_t last = scratch[completed - 1u];
            if (last == read_terminator ||
                (read_terminator == '\r' && last == 0x1Au))
                break;
        }
    } while (transferred < count && cpu->running);

    if (scratch) dos_host_free_pages(scratch, scratch_pages);
    if (dos_mem_free(vm, segment) != DOS_MEM_OK && !error)
        error = DOS_ERR_INVALID_BLOCK;
    if (cancelled || !cpu->running) return 0;
    if (error) return error;

    cpu->eax = regs.eax;
    if (!(regs.flags & FLAG_CF)) {
        if (vm->dpmi.is_32bit) cpu->eax = transferred;
        else cpu->ax = (uint16_t)transferred;
    }
    cpu->ebx = regs.ebx;
    cpu->esi = regs.esi;
    cpu->edi = regs.edi;
    cpu->ebp = regs.ebp;
    const uint16_t status_flags = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
    cpu->flags = (cpu->flags & ~status_flags) | (regs.flags & status_flags);
    return 0;
}

static bool dpmi_dos_string_size_probe(dos_vm_t *vm, uint16_t selector,
                                        uint32_t offset, uint32_t *size,
                                        dpmi_buffer_fault_t *fault)
{
    uint32_t linear;
    uint64_t remaining;
    if (!dpmi_client_region_probe(vm, selector, offset, false, &linear, &remaining, fault)) return false;
    if (!vm->dpmi.is_32bit) {
        if (offset > 0xFFFFu) return false;
        if (remaining > 0x10000u - offset) remaining = 0x10000u - offset;
    }
    uint32_t length = 0;
    while (remaining) {
        dpmi_page_span_t span;
        if (!dpmi_page_span_probe_fault(vm, linear, 1u, DOS_PAGE_USER, &span, fault)) return false;
        uint32_t address = span.pages[0].translation.physical;
        uint32_t part = DPMI_EXT_PAGE_SIZE - (linear & 0xFFFu);
        if (part > remaining) part = (uint32_t)remaining;
        if (part > vm->total_mem_size - address) part = vm->total_mem_size - address;
        dpmi_page_span_commit(vm, &span, false);
        for (uint32_t i = 0; i < part; i++) {
            if (dos_mem_read8(vm, address + i) == '$') {
                if (i >= UINT32_MAX - length) return false;
                *size = length + i + 1u;
                return true;
            }
        }
        if (part > UINT32_MAX - length) return false;
        linear += part;
        length += part;
        remaining -= part;
    }
    return false;
}

static bool dpmi_dos_string_size(dos_vm_t *vm, uint16_t selector,
                                  uint32_t offset, uint32_t *size)
{
    dpmi_buffer_fault_t fault;
    while (!dpmi_dos_string_size_probe(vm, selector, offset, size, &fault)) {
        if (!dpmi_service_exception(vm, fault.vector, fault.error, fault.linear)) return false;
    }
    return vm->cpu->running;
}

uint16_t dpmi_dos_console(dos_vm_t *vm)
{
    enum { CONSOLE_BUFFER_MAX = 0xFFF0u, CONSOLE_STACK_PARAGRAPHS = 0x100u };
    cpu8086_state_t *cpu = vm->cpu;
    cpu8086_state_t request = *cpu;
    bool string = request.ah == 9u;
    bool line = request.ah == 0x0Au || request.ax == 0x0C0Au;
    bool copy_back = false;
    uint32_t offset = vm->dpmi.is_32bit ? request.edx : request.dx;
    uint32_t size = 0;
    if (string) {
        if (!dpmi_dos_string_size(vm, request.ds, offset, &size))
            return 0;
    } else if (line) {
        dpmi_client_buffer_t header;
        if (!dpmi_service_buffer(vm, request.ds, offset, 1u, false, &header)) return 0;
        uint8_t maximum = (uint8_t)dpmi_buffer_load(vm, &header, 0, 1u);
        size = maximum ? (uint32_t)maximum + 2u : 1u;
        copy_back = maximum != 0;
        for (;;) {
            dpmi_buffer_fault_t fault = { .vector = 13 };
            if ((vm->dpmi.is_32bit || size <= 0x10000u - offset) &&
                dpmi_io_buffer_probe(vm, request.ds, offset, size, copy_back, &fault)) break;
            if (!dpmi_service_exception(vm, fault.vector, fault.error, fault.linear)) return 0;
        }
    }

    uint32_t capacity = size < CONSOLE_BUFFER_MAX ? size : CONSOLE_BUFFER_MAX;
    uint16_t paragraphs = (uint16_t)((capacity + 15u) / 16u);
    uint16_t largest = 0;
    uint16_t segment = dos_mem_alloc(vm, paragraphs + CONSOLE_STACK_PARAGRAPHS,
                                     &largest);
    /* Strings can be split; an input line must fit in a single call so that
     * editing, flushing, and Ctrl-C retain the real DOS service contract. */
    if (!segment && string && largest > CONSOLE_STACK_PARAGRAPHS) {
        paragraphs = largest - CONSOLE_STACK_PARAGRAPHS;
        segment = dos_mem_alloc(vm, largest, NULL);
    }
    if (!segment) return DOS_ERR_NOT_ENOUGH_MEMORY;
    capacity = (uint32_t)paragraphs * 16u;
    unsigned scratch_pages = (capacity + 4095u) / 4096u;
    uint8_t *scratch = scratch_pages ? dos_host_alloc_pages(scratch_pages) : NULL;
    if (scratch_pages && !scratch) {
        dos_mem_free(vm, segment);
        return DOS_ERR_NOT_ENOUGH_MEMORY;
    }
    uint32_t real_address = dos_linear(segment, 0);
    dpmi_rm_regs_t regs = {
        .edi = request.edi, .esi = request.esi, .ebp = request.ebp,
        .ebx = request.ebx, .edx = request.edx, .ecx = request.ecx,
        .eax = request.eax, .flags = request.flags,
        .ss = (uint16_t)(segment + paragraphs),
        .sp = CONSOLE_STACK_PARAGRAPHS * 16u - 2u
    };
    uint32_t consumed = 0;
    uint16_t error = 0;
    bool cancelled = false;
    do {
        uint32_t part = size;
        if (string) {
            part = size - 1u - consumed;
            if (part >= capacity) part = capacity - 1u;
            regs.eax = (regs.eax & 0xFFFF0000u) | request.ax;
        }
        if (size) {
            if (!dpmi_service_io_buffer(vm, request.ds, offset + consumed,
                                      part, copy_back)) {
                cancelled = true;
                break;
            }
            if (!dpmi_copy_io_buffer(vm, request.ds, offset + consumed,
                                     real_address, part, true, scratch)) {
                cancelled = true;
                break;
            }
            if (string) dos_mem_write8(vm, real_address + part, '$');
            regs.edx &= 0xFFFF0000u;
            regs.ds = segment;
        }
        if (dpmi_simulate_rm_call(vm, &regs, 0,
                                   DPMI_RM_CALL_INTERRUPT, 0x21u)) {
            error = DOS_ERR_INVALID_DATA;
            break;
        }
        if (!cpu->running) { cancelled = true; break; }
        if (copy_back) {
            if (!dpmi_copy_io_buffer(vm, request.ds, offset, real_address, size, false, scratch)) {
                cancelled = true;
                break;
            }
        }
        consumed += part;
        /* Legacy output does not define CF as a success indicator. Stop on
         * an error/nonstandard AX result, not on a caller's preserved CF. */
        if (!string || (regs.eax & 0xFFFFu) != 0x0924u) break;
    } while (consumed < size - 1u && cpu->running);

    if (scratch) dos_host_free_pages(scratch, scratch_pages);
    if (dos_mem_free(vm, segment) != DOS_MEM_OK && !error)
        error = DOS_ERR_INVALID_BLOCK;
    if (cancelled || !cpu->running) return 0;
    if (error) return error;
    cpu->eax = regs.eax;
    cpu->ebx = regs.ebx;
    cpu->ecx = regs.ecx;
    cpu->edx = size ? (regs.edx & 0xFFFF0000u) | request.dx : regs.edx;
    cpu->esi = regs.esi;
    cpu->edi = regs.edi;
    cpu->ebp = regs.ebp;
    const uint16_t status_flags = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
    cpu->flags = (cpu->flags & ~status_flags) | (regs.flags & status_flags);
    return 0;
}

static bool dpmi_default_return_flags_buffer(dos_vm_t *vm,
                                             uint8_t private_frame_bytes,
                                             dpmi_client_buffer_t *buffer)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !buffer || !cpu->protected_mode || !vm->dpmi.active)
        return false;

    uint32_t delta = private_frame_bytes + (vm->dpmi.is_32bit ? 8u : 4u);
    uint32_t offset = cpu_stack_offset(cpu);
    if (cpu_stack_addr32(cpu)) {
        if (offset > UINT32_MAX - delta)
            return false;
        offset += delta;
    } else {
        offset = (uint16_t)(offset + delta);
    }

    return dpmi_stack_buffer(vm, cpu->ss, offset,
                              vm->dpmi.is_32bit ? 4u : 2u, true, buffer);
}

static bool dpmi_reflect_real_interrupt(dos_vm_t *vm, uint8_t int_num,
                                        uint32_t flags,
                                        uint32_t *reflected_flags)
{
    cpu8086_state_t *cpu = vm->cpu;
    dpmi_rm_regs_t regs;
    dpmi_zero(&regs, sizeof(regs));
    regs.eax = cpu->eax;
    regs.ebx = cpu->ebx;
    regs.ecx = cpu->ecx;
    regs.edx = cpu->edx;
    regs.esi = cpu->esi;
    regs.edi = cpu->edi;
    regs.ebp = cpu->ebp;
    regs.flags = (uint16_t)((flags & ~FLAG_IF) |
                            (vm->dpmi.virtual_interrupts_enabled ? FLAG_IF : 0));
    dpmi_rm_call_kind_t kind = DPMI_RM_CALL_INTERRUPT;
    if (int_num == 0x1Cu) {
        /* Chaining the default PM timer calls the saved real IVT handler,
         * not another INT 1Ch. A nested tick inside that handler must still
         * be reflected normally, so no global recursion-disable flag. */
        regs.ip = dos_mem_read16(vm, 0x1Cu * 4u);
        regs.cs = dos_mem_read16(vm, 0x1Cu * 4u + 2u);
        if ((!regs.cs && !regs.ip) || regs.cs >= DOS_ROM_BASE >> 4) {
            *reflected_flags = flags;
            return true;
        }
        kind = DPMI_RM_CALL_IRET;
    }
    if (dpmi_simulate_rm_call(vm, &regs, 0, kind, int_num))
        return false;
    cpu->eax = regs.eax;
    cpu->ebx = regs.ebx;
    cpu->ecx = regs.ecx;
    cpu->edx = regs.edx;
    cpu->esi = regs.esi;
    cpu->edi = regs.edi;
    cpu->ebp = regs.ebp;
    *reflected_flags = regs.flags;
    return true;
}

bool dpmi_dispatch_default_exception(dos_vm_t *vm, uint8_t vector,
                                     uint8_t private_frame_bytes)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !cpu->protected_mode || !vm->dpmi.active ||
        !vm->dpmi.exception_depth || vector >= 32u)
        return false;

    if (vm->dpmi.exception_context[vm->dpmi.exception_depth - 1u].real)
        return dpmi_dispatch_default_real_exception(vm, vector, private_frame_bytes);

    uint32_t width = vm->dpmi.is_32bit ? 4u : 2u;
    uint32_t offset = cpu_stack_offset(cpu);
    if (cpu_stack_addr32(cpu)) {
        if (offset > UINT32_MAX - private_frame_bytes) return false;
        offset += private_frame_bytes;
    } else {
        offset = (uint16_t)(offset + private_frame_bytes);
    }
    dpmi_client_buffer_t frame;
    if (!dpmi_stack_buffer(vm, cpu->ss, offset, 8u * width, true, &frame))
        return false;
    uint32_t return_off = dpmi_buffer_load(vm, &frame, 0, width);
    if (return_off != DPMI_EXCEPTION_RETURN_OFF ||
        dpmi_buffer_load(vm, &frame, width, 2) != vm->dpmi.sel_host_code)
        return false;

    /* The DPMI default #DE policy requires an installed DOS handler. Do
     * not ask the CPU's raw INT path to treat 0000:0000 as a missing vector. */
    uint32_t real_divide = vector ? 0 : dos_mem_read32(vm, 0);
    bool missing_divide = !vector && (!real_divide ||
        (real_divide == 0xF0000000u && dos_mem_read8(vm, DOS_ROM_BASE) == 0xCF));
    if (vector == 6u || vector >= 8u || missing_divide) {
        uint32_t eip = dpmi_buffer_load(vm, &frame, 3u * width, width);
        serial_puts("[DPMI] Unhandled processor exception ");
        serial_puthex(vector, 2);
        serial_puts(" at ");
        serial_puthex(dpmi_buffer_load(vm, &frame, 4u * width, 2), 4);
        serial_puts(":"); serial_puthex(eip, 8); serial_puts("\n");
        cpu->running = false;
        cpu->exit_code = -1;
        return true;
    }

    uint32_t flags = dpmi_buffer_load(vm, &frame, 5u * width, width);
    uint32_t reflected_flags;
    if (!dpmi_reflect_real_interrupt(vm, vector, flags, &reflected_flags))
        return !cpu->running;
    if (!dpmi_stack_buffer(vm, cpu->ss, offset, 8u * width, true, &frame))
        return false;
    flags = (flags & 0xFFFF0000u) | reflected_flags | FLAGS_FIXED;
    dpmi_buffer_store(vm, &frame, 5u * width, flags, width);
    return true;
}

bool dpmi_dispatch_default_interrupt(dos_vm_t *vm, uint8_t int_num,
                                     uint8_t private_frame_bytes)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    dpmi_client_buffer_t return_flags;
    if (!cpu || !cpu->protected_mode || !vm->dpmi.active ||
        !dpmi_default_return_flags_buffer(vm, private_frame_bytes,
                                          &return_flags))
        return false;

    uint32_t reflected_flags;
    if (int_num == 0x24u) {
        cpu->al = 3u;
        reflected_flags = cpu->eflags;
    } else if (int_num == 0x23u) {
        /* The default protected Ctrl-C handler ignores the notification;
         * chaining here must not fall back to the real-mode abort handler. */
        reflected_flags = cpu->eflags;
    } else if (dos_int_has_pm_translator(int_num)) {
        dos_int_dispatch(vm, int_num);
        reflected_flags = cpu->eflags;
    } else if (!dpmi_reflect_real_interrupt(vm, int_num, cpu->flags,
                                            &reflected_flags))
        return false;

    /* Segment registers and SP never cross the DPMI reflection boundary.
     * IF remains virtualized; return the flags that user mode can observe. */
    const uint32_t reflected_mask = FLAG_CF | FLAG_PF | FLAG_AF |
                                    FLAG_ZF | FLAG_SF | FLAG_TF |
                                    FLAG_DF | FLAG_OF;
    if (!dpmi_default_return_flags_buffer(vm, private_frame_bytes, &return_flags))
        return false;
    uint32_t width = vm->dpmi.is_32bit ? 4u : 2u;
    uint32_t saved = dpmi_buffer_load(vm, &return_flags, 0, width);
    saved = (saved & ~reflected_mask) |
            (reflected_flags & reflected_mask) | FLAGS_FIXED;
    dpmi_buffer_store(vm, &return_flags, 0, saved, width);
    cpu->eflags = (cpu->eflags & ~reflected_mask) |
                  (reflected_flags & reflected_mask) |
                  FLAGS_FIXED | FLAG_IF;
    return true;
}

void dos_int31_dpmi(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    dpmi_state_t *dpmi = &vm->dpmi;
    uint16_t func = cpu->ax;

    switch (func) {

    /* ── AX=0000h: Allocate LDT Descriptors ────────────────────── */
    case 0x0000: {
        uint16_t count = cpu->cx;
        if (count == 0) count = 1;
        uint16_t first_sel = dpmi_alloc_descriptor_run(
            dpmi, count, DPMI_DESC_MUTABLE);

        if (first_sel) {
            cpu->ax = first_sel;
            cpu->eflags &= ~FLAG_CF;
        } else {
            serial_puts("[DPMI] 0000h: Cannot allocate ");
            serial_putdec(count);
            serial_puts(" descriptors\n");
            cpu->ax = 0x8011;  /* descriptor unavailable */
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0001h: Free LDT Descriptor ─────────────────────────── */
    case 0x0001: {
        uint16_t sel = cpu->bx;
        uint16_t idx;
        if (dpmi_selector_allocated(dpmi, sel, &idx) &&
            dpmi->descriptor_state[idx] == DPMI_DESC_MUTABLE) {
            dpmi_release_descriptor(dpmi, idx);
            dpmi_clear_freed_data_selectors(cpu, idx, 1);
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8022;  /* invalid selector */
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0003h: Get Selector Increment ──────────────────────── */
    /* AX=0002h: Segment to Descriptor */
    case 0x0002: {
        uint16_t sel = dpmi_segment_selector(vm, cpu->bx);
        if (!sel) {
            cpu->ax = 0x8011;  /* descriptor unavailable */
            cpu->eflags |= FLAG_CF;
            break;
        }

        cpu->ax = sel;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    case 0x0003:
        cpu->ax = DPMI_SEL_INC;
        cpu->eflags &= ~FLAG_CF;
        break;

    /* ── AX=0006h: Get Segment Base Address ────────────────────── */
    case 0x0006: {
        uint16_t sel = cpu->bx;
        uint16_t idx;
        if (dpmi_selector_allocated(dpmi, sel, &idx)) {
            uint32_t base = dpmi_desc_get_base(&dpmi->ldt[idx]);
            cpu->cx = (uint16_t)(base >> 16);
            cpu->dx = (uint16_t)(base & 0xFFFF);
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8022;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0007h: Set Segment Base Address ────────────────────── */
    case 0x0007: {
        uint16_t sel = cpu->bx;
        uint16_t idx;
        if (dpmi_selector_mutable(dpmi, sel, &idx)) {
            uint32_t base = ((uint32_t)cpu->cx << 16) | cpu->dx;
            dpmi_desc_set_base(&dpmi->ldt[idx], base);
            serial_puts("[DPMI] SetBase sel=0x");
            serial_puthex(sel, 4);
            serial_puts(" base=0x"); serial_puthex(base, 8);
            serial_puts("\n");
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8022;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0008h: Set Segment Limit ───────────────────────────── */
    case 0x0008: {
        uint16_t sel = cpu->bx;
        uint16_t idx;
        uint32_t limit = ((uint32_t)cpu->cx << 16) | cpu->dx;
        if (dpmi_selector_mutable(dpmi, sel, &idx) &&
            dpmi_limit_is_representable(limit)) {
            dpmi_desc_set_limit(&dpmi->ldt[idx], limit);
            serial_puts("[DPMI] SetLimit sel=0x");
            serial_puthex(sel, 4);
            serial_puts(" limit=0x"); serial_puthex(limit, 8);
            serial_puts("\n");
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = dpmi_selector_mutable(dpmi, sel, NULL)
                    ? 0x8021 : 0x8022;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0009h: Set Descriptor Access Rights ────────────────── */
    case 0x0009: {
        uint16_t sel = cpu->bx;
        uint16_t idx;
        if (dpmi_selector_mutable(dpmi, sel, &idx) &&
            dpmi_rights_valid(cpu->cl, cpu->ch)) {
            dpmi_descriptor_t *d = &dpmi->ldt[idx];
            uint8_t new_acc = cpu->cl;
            d->access = new_acc;
            /* Preserve limit bits and replace only AVL/L/D/G. */
            d->flags_lim = (d->flags_lim & 0x0F) | (cpu->ch & 0xF0);
            serial_puts("[DPMI] SetAccess sel=0x");
            serial_puthex(sel, 4);
            serial_puts(" acc=0x"); serial_puthex(new_acc, 2);
            serial_puts(" flags=0x"); serial_puthex(cpu->ch, 2);
            serial_puts((new_acc & DESC_CODE) ? " CODE" : " DATA");
            serial_puts("\n");
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = dpmi_selector_mutable(dpmi, sel, NULL) ? 0x8021 : 0x8022;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=000Ah: Create Alias Descriptor ─────────────────────── */
    case 0x000A: {
        uint16_t src_sel = cpu->bx;
        uint16_t src_idx;
        if (!dpmi_selector_allocated(dpmi, src_sel, &src_idx)) {
            cpu->ax = 0x8022;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint16_t new_sel = dpmi_alloc_descriptor(dpmi);
        if (!new_sel) {
            cpu->ax = 0x8011;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint16_t new_idx = dpmi_sel_to_index(new_sel);
        dpmi_descriptor_t *src = &dpmi->ldt[src_idx];
        dpmi_descriptor_t *dst = &dpmi->ldt[new_idx];

        /* Copy base and limit from source */
        uint32_t base = dpmi_desc_get_base(src);
        uint32_t limit = dpmi_desc_get_limit(src);

        /* Make it a data segment (clear code bit, set writable) */
        uint8_t access = (src->access & ~DESC_CODE) | DESC_WRITABLE;
        access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT | DESC_WRITABLE;
        uint8_t flags = src->flags_lim & 0xF0;  /* preserve G and D bits */

        dpmi_build_desc(dst, base, limit, access, flags);

        cpu->ax = new_sel;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=000Bh: Get Descriptor ────────────────────────────────── */
    case 0x000B: {
        uint16_t sel = cpu->bx;
        uint16_t buffer_sel = cpu->es;
        uint32_t offset = dpmi->is_32bit ? cpu->edi : cpu->di;
        uint16_t idx;
        if (!dpmi_selector_allocated(dpmi, sel, &idx)) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8022;
            break;
        }
        dpmi_client_buffer_t buffer;
        if (!dpmi_service_buffer(vm, buffer_sel, offset,
                                  sizeof(dpmi_descriptor_t), true, &buffer)) break;
        /* A repair handler can free the originally requested descriptor. */
        if (!dpmi_selector_allocated(dpmi, sel, &idx)) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8022;
            break;
        }
        dpmi_descriptor_t descriptor = dpmi->ldt[idx];
        (void)dpmi_buffer_transfer(vm, &buffer, 0, &descriptor, sizeof(descriptor), true);
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=000Ch: Set Descriptor ────────────────────────────────── */
    case 0x000C: {
        uint16_t sel = cpu->bx;
        uint16_t buffer_sel = cpu->es;
        uint32_t offset = dpmi->is_32bit ? cpu->edi : cpu->di;
        uint16_t idx;
        if (!dpmi_selector_mutable(dpmi, sel, &idx)) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8022;
            break;
        }
        dpmi_client_buffer_t buffer;
        if (!dpmi_service_buffer(vm, buffer_sel, offset,
                                  sizeof(dpmi_descriptor_t), false, &buffer)) break;
        if (!dpmi_selector_mutable(dpmi, sel, &idx)) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8022;
            break;
        }
        dpmi_descriptor_t next;
        (void)dpmi_buffer_transfer(vm, &buffer, 0, &next, sizeof(next), false);
        if (!dpmi_rights_valid(next.access, next.flags_lim)) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8021;
            break;
        }
        dpmi->ldt[idx] = next;
        dpmi_descriptor_t *d = &dpmi->ldt[idx];

        /* Log descriptor details for debugging */
        uint32_t base = dpmi_desc_get_base(d);
        uint32_t limit = dpmi_desc_get_limit(d);
        bool is_code = (d->access & DESC_CODE) != 0;
        bool is_32 = (d->flags_lim & 0x40) != 0;
        serial_puts("[DPMI] SetDesc sel=");
        serial_puthex(sel, 4);
        serial_puts(" base=");
        serial_puthex(base, 8);
        serial_puts(" lim=");
        serial_puthex(limit, 8);
        serial_puts(is_code ? " CODE" : " DATA");
        serial_puts(is_32 ? " 32" : " 16");
        serial_puts(" acc=");
        serial_puthex(d->access, 2);
        serial_puts("\n");

        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* -- AX=000Dh: Allocate Specific LDT Descriptor --------------- */
    case 0x000D: {
        uint16_t sel = cpu->bx;
        uint16_t idx = dpmi_sel_to_index(sel);

        if (!(sel & 0x04u) || idx >= DPMI_MAX_DESCRIPTORS) {
            cpu->ax = 0x8022;  /* references GDT or beyond LDT */
            cpu->eflags |= FLAG_CF;
        } else if (dpmi->descriptor_state[idx] != DPMI_DESC_FREE) {
            cpu->ax = 0x8011;  /* descriptor is in use */
            cpu->eflags |= FLAG_CF;
        } else {
            dpmi_build_desc(&dpmi->ldt[idx], 0, 0,
                            DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                            DESC_WRITABLE,
                            0);
            dpmi->descriptor_state[idx] = DPMI_DESC_MUTABLE;
            if (idx >= DPMI_SPECIFIC_DESCRIPTOR_COUNT &&
                idx == dpmi->next_free_index)
                dpmi->next_free_index = dpmi_next_general_index(idx);
            cpu->eflags &= ~FLAG_CF;
        }
        break;
    }

    /* ── AX=0305h: Get State Save/Restore Addresses ──────────────── */
    case 0x0305: {
        uint16_t host_sel = dpmi_host_code_selector(dpmi);
        if (!host_sel) {
            cpu->ax = 0x8011;
            cpu->eflags |= FLAG_CF;
            break;
        }

        cpu->ax  = sizeof(dpmi_saved_state_t);
        cpu->bx  = DPMI_ENTRY_SEG;      /* RM seg */
        cpu->cx  = DPMI_SAVE_STATE_OFF; /* RM off */
        cpu->esi = host_sel;            /* PM code selector */
        cpu->edi = DPMI_SAVE_STATE_OFF; /* PM off */
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0306h: Get Raw Mode Switch Addresses ─────────────────── */
    case 0x0306: {
        uint16_t host_sel = dpmi_host_code_selector(dpmi);
        if (!host_sel) {
            cpu->ax = 0x8011;
            cpu->eflags |= FLAG_CF;
            break;
        }

        cpu->bx  = DPMI_ENTRY_SEG;      /* RM→PM seg */
        cpu->cx  = DPMI_RAW_SWITCH_OFF;  /* RM->PM off */
        cpu->esi = host_sel;            /* PM→RM selector */
        cpu->edi = DPMI_RAW_SWITCH_OFF;  /* PM->RM off */
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0A00h: Get Vendor-Specific API Entry Point ───────────── */
    case 0x0A00:
        /* No vendor extensions — return error (carry set) */
        cpu->eflags |= FLAG_CF;
        cpu->ax = 0x8001;  /* unsupported */
        break;

    /* ── AX=0100h: Allocate DOS Memory Block ───────────────────── */
    case 0x0100: {
        uint16_t paragraphs = cpu->bx;
        uint16_t largest = 0;
        int memory_status = dos_mem_largest_available(vm, &largest);
        if (memory_status != DOS_MEM_OK) {
            cpu->ax = dpmi_dos_error(memory_status);
            cpu->bx = 0;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint16_t allocation_largest = 0;
        uint16_t owner = dpmi->owner_psp ? dpmi->owner_psp : vm->current_psp;
        uint16_t seg = dos_mem_alloc_owned(vm, paragraphs, &allocation_largest, owner);
        if (!seg) {
            cpu->ax = 0x0008;  /* insufficient memory */
            cpu->bx = allocation_largest;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint16_t count = dpmi_dos_descriptor_count(dpmi->is_32bit,
                                                   paragraphs);
        uint16_t sel = dpmi_alloc_descriptor_run(
            dpmi, count, DPMI_DESC_DOS_MEMORY);
        if (!sel) {
            memory_status = dos_mem_free(vm, seg);
            cpu->ax = memory_status == DOS_MEM_OK
                    ? 0x8011 : dpmi_dos_error(memory_status);
            cpu->bx = largest;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint16_t idx = dpmi_sel_to_index(sel);
        dpmi_configure_dos_descriptors(dpmi, idx, count, seg, paragraphs);
        dpmi_dos_block_t *block = &dpmi->dos_blocks[idx];
        dpmi_zero(block, sizeof(*block));
        block->segment = seg;
        block->paragraphs = paragraphs;
        block->descriptor_count = count;
        block->allocated = true;

        cpu->ax = seg;
        cpu->dx = sel;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0101h: Free DOS Memory Block ───────────────────────── */
    case 0x0101: {
        uint16_t sel = cpu->dx;
        uint16_t idx;

        if (!dpmi_selector_allocated(dpmi, sel, &idx)) {
            cpu->ax = 0x8022;
            cpu->eflags |= FLAG_CF;
            break;
        }
        if (!dpmi_dos_block_valid(dpmi, idx)) {
            cpu->ax = 0x0009;
            cpu->eflags |= FLAG_CF;
            break;
        }

        dpmi_dos_block_t *block = &dpmi->dos_blocks[idx];
        int result = dos_mem_free(vm, block->segment);
        if (result != DOS_MEM_OK) {
            cpu->ax = dpmi_dos_error(result);
            cpu->eflags |= FLAG_CF;
            break;
        }

        dpmi_clear_freed_data_selectors(cpu, idx,
                                        block->descriptor_count);
        dpmi_release_descriptor_run(dpmi, idx,
                                    block->descriptor_count);
        dpmi_zero(block, sizeof(*block));
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0102h: Resize DOS Memory Block ─────────────────────── */
    case 0x0102: {
        uint16_t sel = cpu->dx;
        uint16_t new_paragraphs = cpu->bx;
        uint16_t idx;

        if (!dpmi_selector_allocated(dpmi, sel, &idx)) {
            cpu->ax = 0x8022;
            cpu->bx = 0;
            cpu->eflags |= FLAG_CF;
            break;
        }
        if (!dpmi_dos_block_valid(dpmi, idx)) {
            cpu->ax = 0x0009;
            cpu->bx = 0;
            cpu->eflags |= FLAG_CF;
            break;
        }

        dpmi_dos_block_t *block = &dpmi->dos_blocks[idx];
        uint16_t current_paragraphs = 0;
        uint16_t memory_max = 0;
        int result = dos_mem_query_block(vm, block->segment,
                                         &current_paragraphs, &memory_max);
        if (result != DOS_MEM_OK) {
            cpu->ax = dpmi_dos_error(result);
            cpu->bx = memory_max;
            cpu->eflags |= FLAG_CF;
            break;
        }
        if (current_paragraphs != block->paragraphs) {
            cpu->ax = 0x0009;
            cpu->bx = memory_max;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint16_t descriptor_max = dpmi_dos_max_paragraphs(
            dpmi, idx, block->descriptor_count);
        uint16_t maximum = memory_max < descriptor_max
                         ? memory_max : descriptor_max;
        if (new_paragraphs > memory_max) {
            cpu->ax = 0x0008;
            cpu->bx = maximum;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint16_t old_count = block->descriptor_count;
        uint16_t new_count = dpmi_dos_descriptor_count(
            dpmi->is_32bit, new_paragraphs);
        uint16_t claimed = 0;
        if (new_count > old_count) {
            claimed = (uint16_t)(new_count - old_count);
            if (!dpmi_claim_descriptor_run(dpmi, idx + old_count,
                                           claimed,
                                           DPMI_DESC_DOS_MEMORY)) {
                cpu->ax = 0x8011;
                cpu->bx = maximum;
                cpu->eflags |= FLAG_CF;
                break;
            }
        }

        uint16_t resize_max = 0;
        result = dos_mem_resize(vm, block->segment, new_paragraphs,
                                &resize_max);
        if (result != DOS_MEM_OK) {
            if (claimed)
                dpmi_release_descriptor_run(dpmi, idx + old_count,
                                            claimed);
            cpu->ax = dpmi_dos_error(result);
            cpu->bx = resize_max < descriptor_max
                    ? resize_max : descriptor_max;
            cpu->eflags |= FLAG_CF;
            break;
        }

        if (new_count < old_count) {
            uint16_t released = (uint16_t)(old_count - new_count);
            dpmi_clear_freed_data_selectors(cpu, idx + new_count, released);
            dpmi_release_descriptor_run(dpmi, idx + new_count, released);
        }
        dpmi_configure_dos_descriptors(dpmi, idx, new_count,
                                       block->segment, new_paragraphs);
        block->paragraphs = new_paragraphs;
        block->descriptor_count = new_count;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0200h: Get Real-Mode Interrupt Vector ──────────────── */
    case 0x0200: {
        uint8_t int_num = cpu->bl;
        uint32_t ivt_addr = (uint32_t)int_num * 4;
        cpu->dx = dos_mem_read16(vm, ivt_addr);       /* offset */
        cpu->cx = dos_mem_read16(vm, ivt_addr + 2);   /* segment */
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0201h: Set Real-Mode Interrupt Vector ──────────────── */
    case 0x0201: {
        uint8_t int_num = cpu->bl;
        uint32_t ivt_addr = (uint32_t)int_num * 4;
        dos_mem_write16(vm, ivt_addr, cpu->dx);         /* offset */
        dos_mem_write16(vm, ivt_addr + 2, cpu->cx);     /* segment */
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0202h: Get Processor Exception Handler Vector ────────── */
    case 0x0202:
    case 0x0210:
    case 0x0211: {
        uint8_t exc_num = cpu->bl;
        if (exc_num > 31) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8021;
            break;
        }
        uint16_t selector = func == 0x0211 ? dpmi->real_exception_vectors[exc_num].sel
                                           : dpmi->exception_vectors[exc_num].sel;
        uint32_t offset = func == 0x0211 ? dpmi->real_exception_vectors[exc_num].off
                                        : dpmi->exception_vectors[exc_num].off;
        if (!selector) {
            selector = dpmi_get_host_code_selector(vm);
            if (!selector) {
                cpu->eflags |= FLAG_CF;
                cpu->ax = 0x8011;
                break;
            }
            offset = DPMI_PM_EXCEPTION_BASE_OFF +
                     exc_num * DPMI_PM_EXCEPTION_STUB_SIZE;
        }
        cpu->cx = selector;
        if (dpmi->is_32bit) cpu->edx = offset;
        else cpu->dx = (uint16_t)offset;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0203h: Set Processor Exception Handler Vector ────────── */
    case 0x0203:
    case 0x0212:
    case 0x0213: {
        uint8_t exc_num = cpu->bl;
        if (exc_num > 31) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8021;
            break;
        }
        if (!dpmi_selector_is_code(dpmi, cpu->cx)) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8022;
            break;
        }
        if (func == 0x0213) {
            dpmi->real_exception_vectors[exc_num].sel = cpu->cx;
            dpmi->real_exception_vectors[exc_num].off = dpmi->is_32bit ? cpu->edx : cpu->dx;
        } else {
            dpmi->exception_vectors[exc_num].sel = cpu->cx;
            dpmi->exception_vectors[exc_num].off = dpmi->is_32bit ? cpu->edx : cpu->dx;
        }
        /* An old-format owner can still chain to an earlier 0212h owner. */
        if (func == 0x0212) dpmi->exception_extended[exc_num] = true;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0204h: Get Protected-Mode Interrupt Vector ─────────── */
    case 0x0204: {
        uint8_t int_num = cpu->bl;
        if (dpmi->pm_vectors[int_num].sel) {
            cpu->cx = dpmi->pm_vectors[int_num].sel;
            cpu->edx = dpmi->pm_vectors[int_num].off;
        } else {
            uint16_t host_selector = dpmi_get_host_code_selector(vm);
            if (!host_selector) {
                cpu->ax = 0x8011;
                cpu->eflags |= FLAG_CF;
                break;
            }
            cpu->cx = host_selector;
            cpu->edx = DPMI_PM_REFLECT_BASE_OFF +
                       int_num * DPMI_PM_REFLECT_STUB_SIZE;
        }
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0205h: Set Protected-Mode Interrupt Vector ─────────── */
    case 0x0205: {
        uint8_t int_num = cpu->bl;
        if (!dpmi_selector_is_code(dpmi, cpu->cx)) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8022;
            break;
        }
        dpmi->pm_vectors[int_num].sel = cpu->cx;
        dpmi->pm_vectors[int_num].off = cpu->edx;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0300h: Simulate Real-Mode Interrupt ────────────────── */
    /* This is CRITICAL: DOOM does ALL file I/O through this call.
     * It builds a dpmi_rm_regs_t with e.g. AH=3Dh (open file),
     * DS:DX pointing to the filename, and calls INT 31h/0300h
     * with BL=0x21. We load those regs, call dos_int_dispatch in
     * real mode, then copy the results back. */
    case 0x0300:
    case 0x0301:
    case 0x0302: {
        uint32_t regs_offset = dpmi->is_32bit ? cpu->edi : cpu->di;
        uint16_t regs_selector = cpu->es;
        dpmi_client_buffer_t regs_buffer;
        uint8_t interrupt_number = cpu->bl;
        uint16_t copy_words = cpu->cx;
        if (cpu->bh & 0xFEu) {
            cpu->ax = 0x8021;
            cpu->eflags |= FLAG_CF;
            break;
        }
        if (!dpmi_service_buffer(vm, regs_selector, regs_offset,
                                  sizeof(dpmi_rm_regs_t), false, &regs_buffer)) break;

        dpmi_rm_regs_t regs;
        (void)dpmi_buffer_transfer(vm, &regs_buffer, 0, &regs, sizeof(regs), false);

        dpmi_rm_call_kind_t kind = DPMI_RM_CALL_INTERRUPT;
        if (func == 0x0301)
            kind = DPMI_RM_CALL_FAR;
        else if (func == 0x0302)
            kind = DPMI_RM_CALL_IRET;

        uint16_t error = dpmi_simulate_rm_call(vm, &regs, copy_words,
                                               kind, interrupt_number);
        if (!cpu->running) break;
        /* A nested callback can edit descriptors or page tables. Resolve
         * the caller's output again instead of writing to a stale mapping. */
        if (!dpmi_service_buffer(vm, regs_selector, regs_offset,
                                  sizeof(regs), true, &regs_buffer)) break;
        (void)dpmi_buffer_transfer(vm, &regs_buffer, 0, &regs, sizeof(regs), true);

        if (!error) {
            cpu->eflags &= ~FLAG_CF;
        } else if (cpu->running) {
            cpu->ax = error;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    case 0x0303: {
        /* Capture both pointers before a handler can change DS/ES or GPRs.
         * A later record repair may invalidate the earlier code admission. */
        uint16_t pm_selector = cpu->ds, regs_selector = cpu->es;
        uint32_t pm_offset = dpmi->is_32bit ? cpu->esi : cpu->si;
        uint32_t regs_offset = dpmi->is_32bit ? cpu->edi : cpu->di;
        dpmi_client_buffer_t regs_buffer;
        dpmi_buffer_fault_t fault;
        while (!dpmi_code_target_probe(vm, pm_selector, pm_offset, &fault) ||
               !dpmi_client_buffer_probe(vm, regs_selector, regs_offset,
                                          sizeof(dpmi_rm_regs_t), true, &regs_buffer, &fault)) {
            if (!dpmi_service_exception(vm, fault.vector, fault.error, fault.linear)) return;
            cpu = vm->cpu;
        }

        int slot = -1;
        for (int i = 0; i < DPMI_MAX_CALLBACKS; i++) {
            if (!dpmi->callbacks[i].active) {
                slot = i;
                break;
            }
        }

        if (slot < 0 || dpmi->callback_generation == UINT64_MAX) {
            cpu->ax = 0x8015;  /* callback unavailable */
            cpu->eflags |= FLAG_CF;
            break;
        }

        /* Nothing client-visible is reserved until admission completes.
         * Do not retain newly created host descriptors on resource failure. */
        uint16_t cb_off = DPMI_CALLBACK_BASE_OFF +
                          (uint16_t)(slot * DPMI_CALLBACK_STUB_SIZE);
        uint32_t cb_addr = dos_linear(DPMI_ENTRY_SEG, cb_off);
        if (!dpmi_range_valid(vm, cb_addr, DPMI_CALLBACK_STUB_SIZE)) {
            cpu->ax = 0x8015;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint16_t old_stack = dpmi->sel_exception_stack, old_code = dpmi->sel_host_code;
        uint16_t stack_index = 0, code_index = 0;
        bool had_stack = dpmi_selector_allocated(dpmi, old_stack, &stack_index) &&
                         dpmi->descriptor_state[stack_index] == DPMI_DESC_HOST;
        bool had_code = dpmi_selector_allocated(dpmi, old_code, &code_index) &&
                        dpmi->descriptor_state[code_index] == DPMI_DESC_HOST;
        dpmi_descriptor_t stack_before = had_stack ? dpmi->ldt[stack_index] : (dpmi_descriptor_t){0};
        dpmi_descriptor_t code_before = had_code ? dpmi->ldt[code_index] : (dpmi_descriptor_t){0};
        uint16_t next_free = dpmi->next_free_index;
        uint16_t host_stack = dpmi_get_exception_stack_selector(vm);
        uint16_t host_code = host_stack ? dpmi_get_host_code_selector(vm) : 0;
        uint16_t rm_stack_sel = host_code ? dpmi_alloc_descriptor(dpmi) : 0;
        if (!rm_stack_sel) {
            if (had_stack) dpmi->ldt[stack_index] = stack_before;
            else if (host_stack) dpmi_release_descriptor(dpmi, dpmi_sel_to_index(host_stack));
            if (had_code) dpmi->ldt[code_index] = code_before;
            else if (host_code) dpmi_release_descriptor(dpmi, dpmi_sel_to_index(host_code));
            dpmi->sel_exception_stack = old_stack;
            dpmi->sel_host_code = old_code;
            dpmi->next_free_index = next_free;
            cpu->ax = 0x8015;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint16_t rm_stack_index = dpmi_sel_to_index(rm_stack_sel);
        dpmi_build_desc(&dpmi->ldt[rm_stack_index], 0, 0xFFFFu,
                        DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                        DESC_WRITABLE,
                        0);
        dpmi->descriptor_state[rm_stack_index] = DPMI_DESC_HOST;

        /* Callback stubs have a dedicated range and cannot overwrite host
         * entry, state, mode-switch, or exception-return stubs. */
        dpmi_callback_t *callback = &dpmi->callbacks[slot];
        dpmi_zero(callback, sizeof(*callback));
        callback->rm_seg = DPMI_ENTRY_SEG;
        callback->rm_off = cb_off;
        callback->pm_sel = pm_selector;
        callback->pm_off = pm_offset;
        callback->rm_regs_sel = regs_selector;
        callback->rm_regs_off = regs_offset;
        callback->rm_stack_sel = rm_stack_sel;
        callback->generation = ++dpmi->callback_generation;

        /* INT FBh enters the host; the slot byte identifies this callback.
         * HLT catches a client that returns without setting RM CS:IP. */
        vm->mem[cb_addr] = 0xCD;
        vm->mem[cb_addr + 1u] = DPMI_CALLBACK_ENTRY_INT;
        vm->mem[cb_addr + 2u] = (uint8_t)slot;
        vm->mem[cb_addr + 3u] = 0xF4;
        callback->active = true;

        cpu->cx = DPMI_ENTRY_SEG;
        cpu->dx = cb_off;
        cpu->eflags &= ~FLAG_CF;

        serial_puts("[DPMI] Callback allocated: slot ");
        serial_putdec((uint64_t)slot);
        serial_puts("\n");
        break;
    }

    /* ── AX=0304h: Free Real-Mode Callback ─────────────────────── */
    case 0x0304: {
        uint16_t seg = cpu->cx;
        uint16_t off = cpu->dx;
        bool found = false;

        for (int i = 0; i < DPMI_MAX_CALLBACKS; i++) {
            if (dpmi->callbacks[i].active &&
                dpmi->callbacks[i].rm_seg == seg &&
                dpmi->callbacks[i].rm_off == off) {
                bool executing = false;
                for (uint8_t depth = 0; depth < dpmi->callback_depth; depth++) {
                    if (dpmi->callback_slots[depth] == (uint8_t)i) {
                        executing = true;
                        break;
                    }
                }
                if (executing) break;

                uint16_t index;
                if (dpmi_selector_allocated(dpmi,
                                             dpmi->callbacks[i].rm_stack_sel,
                                             &index)) {
                    dpmi_release_descriptor(dpmi, index);
                }
                uint32_t stub = dos_linear(seg, off);
                if (dpmi_range_valid(vm, stub, DPMI_CALLBACK_STUB_SIZE)) {
                    for (uint32_t byte = 0;
                         byte < DPMI_CALLBACK_STUB_SIZE; byte++)
                        vm->mem[stub + byte] = 0xF4;
                }
                dpmi_zero(&dpmi->callbacks[i],
                          sizeof(dpmi->callbacks[i]));
                found = true;
                break;
            }
        }

        if (found) {
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8024;  /* invalid callback address */
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0400h: Get DPMI Version ────────────────────────────── */
    case 0x0400:
        cpu->ah = 0x00;    /* major version */
        cpu->al = 0x5A;    /* minor version (90 = 0x5A) */
        cpu->bx = 0x0005;  /* flags: 32-bit, reflect to real mode */
        cpu->cl = 0x05;    /* processor type: 586 */
        dos_io_get_pic_bases(vm, &cpu->dh, &cpu->dl);
        cpu->eflags &= ~FLAG_CF;
        break;

    /* ── AX=0500h: Get Free Memory Information ─────────────────── */
    case 0x0500: {
        dpmi_client_buffer_t info_buffer;
        uint32_t info_offset = dpmi->is_32bit ? cpu->edi : cpu->di;
        if (!dpmi_service_buffer(vm, cpu->es, info_offset, 48u,
                                  true, &info_buffer)) break;

        uint32_t total_pages = dpmi_ext_total_pages(vm);
        uint32_t free_pages = dpmi_ext_free_page_count(vm);
        uint32_t largest_pages = dpmi_ext_largest_free_page_count(vm);
        uint32_t allocated_pages = total_pages - free_pages;

        uint32_t info[12] = {
            largest_pages * DPMI_EXT_PAGE_SIZE, largest_pages, largest_pages,
            total_pages, allocated_pages, free_pages, total_pages, free_pages,
            0, UINT32_MAX, UINT32_MAX, UINT32_MAX /* no paging file */
        };
        (void)dpmi_buffer_transfer(vm, &info_buffer, 0, info, sizeof(info), true);

        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0501h: Allocate Memory Block ───────────────────────── */
    case 0x0501: {
        uint32_t size = ((uint32_t)cpu->bx << 16) | cpu->cx;
        if (size == 0) size = 1;
        if (size > UINT32_MAX - (DPMI_EXT_PAGE_SIZE - 1u)) {
            cpu->ax = 0x8012;
            cpu->eflags |= FLAG_CF;
            break;
        }
        uint32_t page_count =
            (size + DPMI_EXT_PAGE_SIZE - 1u) / DPMI_EXT_PAGE_SIZE;

        uint32_t addr = dpmi_ext_alloc_pages(vm, page_count, false);
        if (!addr) {
            serial_puts("[DPMI] 0501h: Out of memory (need ");
            serial_puthex(size, 8);
            serial_puts(")\n");
            cpu->ax = 0x8012;  /* linear memory unavailable */
            cpu->eflags |= FLAG_CF;
            break;
        }

        /* Find a free mem_block slot */
        int slot = -1;
        for (int i = 0; i < DPMI_MAX_MEM_BLOCKS; i++) {
            if (!dpmi->mem_blocks[i].allocated) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            dpmi_ext_free_pages(vm, addr, page_count);
            cpu->ax = 0x8012;
            cpu->eflags |= FLAG_CF;
            break;
        }

        if (!dpmi->next_handle) dpmi->next_handle = 1;
        uint32_t handle = dpmi->next_handle++;

        dpmi->mem_blocks[slot].base      = addr;
        dpmi->mem_blocks[slot].size      = page_count * DPMI_EXT_PAGE_SIZE;
        dpmi->mem_blocks[slot].handle    = handle;
        dpmi->mem_blocks[slot].allocated = true;

        /* Return BX:CX = linear address, SI:DI = handle */
        cpu->bx = (uint16_t)(addr >> 16);
        cpu->cx = (uint16_t)(addr & 0xFFFF);
        cpu->si = (uint16_t)(handle >> 16);
        cpu->di = (uint16_t)(handle & 0xFFFF);

        serial_puts("[DPMI] Alloc ");
        serial_puthex(page_count * DPMI_EXT_PAGE_SIZE, 8);
        serial_puts(" bytes at ");
        serial_puthex(addr, 8);
        serial_puts(" handle=");
        serial_puthex(handle, 8);
        serial_puts("\n");

        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0502h: Free Memory Block ───────────────────────────── */
    case 0x0502: {
        uint32_t handle = ((uint32_t)cpu->si << 16) | cpu->di;
        bool found = false;

        for (int i = 0; i < DPMI_MAX_MEM_BLOCKS; i++) {
            if (dpmi->mem_blocks[i].allocated &&
                dpmi->mem_blocks[i].handle == handle) {
                uint32_t pages = dpmi->mem_blocks[i].size /
                                 DPMI_EXT_PAGE_SIZE;
                found = dpmi_ext_free_pages(
                    vm, dpmi->mem_blocks[i].base, pages);
                if (found)
                    dpmi_zero(&dpmi->mem_blocks[i],
                              sizeof(dpmi->mem_blocks[i]));
                break;
            }
        }

        if (found) {
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8023;  /* invalid handle */
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* ── AX=0503h: Resize Memory Block ─────────────────────────── */
    case 0x0503: {
        uint32_t handle   = ((uint32_t)cpu->si << 16) | cpu->di;
        uint32_t new_size = ((uint32_t)cpu->bx << 16) | cpu->cx;
        if (new_size == 0) new_size = 1;
        if (new_size > UINT32_MAX - (DPMI_EXT_PAGE_SIZE - 1u)) {
            cpu->ax = 0x8012;
            cpu->eflags |= FLAG_CF;
            break;
        }
        uint32_t new_pages =
            (new_size + DPMI_EXT_PAGE_SIZE - 1u) / DPMI_EXT_PAGE_SIZE;
        new_size = new_pages * DPMI_EXT_PAGE_SIZE;

        int slot = -1;
        for (int i = 0; i < DPMI_MAX_MEM_BLOCKS; i++) {
            if (dpmi->mem_blocks[i].allocated &&
                dpmi->mem_blocks[i].handle == handle) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            cpu->ax = 0x8023;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint32_t old_base = dpmi->mem_blocks[slot].base;
        uint32_t old_size = dpmi->mem_blocks[slot].size;
        uint32_t old_pages = old_size / DPMI_EXT_PAGE_SIZE;
        uint32_t new_base = old_base;
        bool resized = false;

        if (new_pages == old_pages) {
            resized = true;
        } else if (new_pages < old_pages) {
            resized = dpmi_ext_free_pages(
                vm, old_base + new_pages * DPMI_EXT_PAGE_SIZE,
                old_pages - new_pages);
        } else {
            uint32_t first = (old_base - DPMI_EXT_BASE) /
                             DPMI_EXT_PAGE_SIZE;
            uint32_t extra = new_pages - old_pages;
            if (dpmi_ext_claim_pages(vm, first + old_pages, extra)) {
                resized = true;
            } else {
                new_base = dpmi_ext_alloc_pages(vm, new_pages, false);
                if (new_base) {
                    for (uint32_t i = 0; i < old_size; i++)
                        dos_mem_write8(vm, new_base + i,
                                       dos_mem_read8(vm, old_base + i));
                    if (dpmi_ext_free_pages(vm, old_base, old_pages)) {
                        resized = true;
                    } else {
                        dpmi_ext_free_pages(vm, new_base, new_pages);
                        new_base = old_base;
                    }
                }
            }
        }

        if (resized) {
            dpmi->mem_blocks[slot].base = new_base;
            dpmi->mem_blocks[slot].size = new_size;
            cpu->bx = (uint16_t)(new_base >> 16);
            cpu->cx = (uint16_t)(new_base & 0xFFFF);
            cpu->si = (uint16_t)(handle >> 16);
            cpu->di = (uint16_t)(handle & 0xFFFF);
            cpu->eflags &= ~FLAG_CF;
        } else {
            cpu->ax = 0x8012;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    /* AX=0600h-0603h: page locking and conventional-memory paging hints.
     * The DOS address space is resident and has no backing store, so DPMI
     * explicitly permits these services to succeed as no-ops. */
    case 0x0600:
    case 0x0601:
    case 0x0602:
    case 0x0603:
        cpu->eflags &= ~FLAG_CF;
        break;

    /* AX=0604h: Get Page Size */
    case 0x0604:
        cpu->bx = 0;
        cpu->cx = DPMI_EXT_PAGE_SIZE;
        cpu->eflags &= ~FLAG_CF;
        break;

    /* AX=0702h-0703h: demand-page candidate and discard hints. With no
     * demand paging, retaining the resident contents is a valid no-op. */
    case 0x0702:
    case 0x0703:
        cpu->eflags &= ~FLAG_CF;
        break;

    /* ── AX=0800h: Physical Address Mapping ────────────────────── */
    case 0x0800: {
        uint32_t phys = ((uint32_t)cpu->bx << 16) | cpu->cx;
        uint32_t size = ((uint32_t)cpu->si << 16) | cpu->di;
        if (!size || phys > vm->total_mem_size ||
            size > vm->total_mem_size - phys) {
            cpu->ax = 0x8021; /* invalid value */
            cpu->eflags |= FLAG_CF;
            break;
        }
        /* The private DOS CR3 already identity-maps the emulated physical
         * aperture, including VBE video memory. */
        cpu->bx = (uint16_t)(phys >> 16);
        cpu->cx = (uint16_t)(phys & 0xFFFF);
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* AX=0801h: Free Physical Address Mapping. Identity mappings are owned
     * by the VM, so releasing a valid mapping is an intentional no-op. */
    case 0x0801: {
        uint32_t linear = ((uint32_t)cpu->bx << 16) | cpu->cx;
        if (linear >= vm->total_mem_size) {
            cpu->ax = 0x8021;
            cpu->eflags |= FLAG_CF;
        } else {
            cpu->eflags &= ~FLAG_CF;
        }
        break;
    }

    /* AX=0900h-0902h: Virtual Interrupt State. AH is deliberately left
     * unchanged so the returned AX can be invoked to restore the old state. */
    case 0x0900:
    case 0x0901:
    case 0x0902: {
        bool previous = dpmi->virtual_interrupts_enabled;
        if (func == 0x0900)
            dpmi->virtual_interrupts_enabled = false;
        else if (func == 0x0901)
            dpmi->virtual_interrupts_enabled = true;
        cpu->al = previous ? 1u : 0u;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── Default: unhandled DPMI function ──────────────────────── */
    default:
        serial_puts("[DPMI] Unhandled INT 31h AX=");
        serial_puthex(func, 4);
        serial_puts("\n");
        cpu->ax = 0x8001;
        cpu->eflags |= FLAG_CF;
        break;
    }
}

static int dpmi_state_service_selftest(dos_vm_t *vm, uint16_t data_sel)
{
    cpu8086_state_t *cpu = vm->cpu;
    cpu8086_state_t original = *cpu;
    dpmi_descriptor_t *data = &vm->dpmi.ldt[dpmi_sel_to_index(data_sel)];
    dpmi_descriptor_t original_data = *data;
    dpmi_stack_t original_pm = vm->dpmi.suspended_stack;
    dpmi_stack_t original_rm = vm->dpmi.real_mode_stack;
    dpmi_paging_t original_pm_paging = vm->dpmi.suspended_paging;
    dpmi_paging_t original_rm_paging = vm->dpmi.real_mode_paging;
    bool original_width = vm->dpmi.is_32bit;
    int checks = 0, failures = 0;
#define STATE_CHECK(condition) do { checks++; if (!(condition)) failures++; } while (0)
    dpmi_desc_set_limit(data, 0x1FFFFu);
    for (unsigned width = 0; width < 2; width++) {
        vm->dpmi.is_32bit = width != 0;
        for (unsigned pm = 0; pm < 2; pm++) {
            *cpu = original;
            cpu->protected_mode = pm != 0;
            cpu->es = pm ? data_sel : 0x1000;
            cpu->edi = pm && width ? 0x11000u : 0xA5A51000u;
            cpu->eflags = FLAGS_FIXED | FLAG_CF | FLAG_DF | FLAG_IF;
            dpmi_stack_t *other = pm ? &vm->dpmi.real_mode_stack
                                     : &vm->dpmi.suspended_stack;
            dpmi_paging_t *paging = pm ? &vm->dpmi.real_mode_paging
                                       : &vm->dpmi.suspended_paging;
            dpmi_stack_t expected = { pm ? 0x2345u : data_sel,
                                       pm ? 0x8000u : 0xA5A58000u };
            dpmi_paging_t expected_paging = {
                pm ? 0x10u : DOS_CR0_PG | DOS_CR0_WP | 0x11u, 0xABCDE018u
            };
            *other = expected;
            *paging = expected_paging;
            for (unsigned operation = 0; operation < 2; operation++) {
                cpu->al = operation;
                cpu8086_state_t before = *cpu;
                STATE_CHECK(dpmi_save_restore_state(vm) == DPMI_SERVICE_COMPLETE);
                bool unchanged = true;
                for (unsigned i = 0; i < sizeof(*cpu); i++)
                    if (((uint8_t *)cpu)[i] != ((uint8_t *)&before)[i])
                        unchanged = false;
                STATE_CHECK(unchanged);
                STATE_CHECK(other->ss == expected.ss && other->esp == expected.esp);
                STATE_CHECK(paging->cr0 == expected_paging.cr0 && paging->cr3 == expected_paging.cr3);
                if (!operation) {
                    *other = (dpmi_stack_t){ 0x4444, 0x5555 };
                    *paging = (dpmi_paging_t){ pm ? 0u : 1u, 0x60000 };
                }
            }
            uint32_t address = pm ? dpmi_translate(vm, data_sel,
                                                   width ? 0x11000u : 0x1000u)
                                  : 0x11000u;
            uint8_t mode = dos_mem_read8(vm, address + 10u);
            dos_mem_write8(vm, address + 10u, mode ^ 1u);
            STATE_CHECK(dpmi_save_restore_state(vm) == DPMI_SERVICE_INVALID);
            STATE_CHECK(other->ss == expected.ss && other->esp == expected.esp);
            dos_mem_write8(vm, address + 10u, mode);
            dos_mem_write32(vm, address + 12u, expected_paging.cr0 ^ 1u);
            STATE_CHECK(dpmi_save_restore_state(vm) == DPMI_SERVICE_INVALID);
            STATE_CHECK(other->ss == expected.ss && other->esp == expected.esp &&
                        paging->cr0 == expected_paging.cr0 && paging->cr3 == expected_paging.cr3);
            if (pm) {
                dos_mem_write32(vm, address + 12u, expected_paging.cr0 | DOS_CR0_PG);
                STATE_CHECK(dpmi_save_restore_state(vm) == DPMI_SERVICE_INVALID);
                STATE_CHECK(paging->cr0 == expected_paging.cr0 && paging->cr3 == expected_paging.cr3);
            }
            dos_mem_write32(vm, address + 12u, expected_paging.cr0);
            dos_mem_write8(vm, address, 0);
            STATE_CHECK(dpmi_save_restore_state(vm) == DPMI_SERVICE_INVALID);
            STATE_CHECK(other->ss == expected.ss && other->esp == expected.esp);
            cpu->al = 2;
            STATE_CHECK(dpmi_save_restore_state(vm) == DPMI_SERVICE_INVALID);
            cpu->al = 0;
            if (pm) {
                /* Raw admission must not run a handler; the recovery matrix
                 * below exercises real delivery and continuation ownership. */
                dpmi_client_buffer_t rejected;
                uint8_t access = data->access;
                data->access &= (uint8_t)~DESC_WRITABLE;
                STATE_CHECK(!dpmi_client_buffer(vm, cpu->es, width ? cpu->edi : cpu->di,
                                                 sizeof(dpmi_saved_state_t), true, &rejected));
                data->access = access & (uint8_t)~DESC_PRESENT;
                STATE_CHECK(!dpmi_client_buffer(vm, cpu->es, width ? cpu->edi : cpu->di,
                                                 sizeof(dpmi_saved_state_t), true, &rejected));
                data->access = access;
                dpmi_desc_set_limit(data, 0x1005u);
                cpu->edi = 0x1000u;
                STATE_CHECK(!dpmi_client_buffer(vm, cpu->es, cpu->di,
                                                 sizeof(dpmi_saved_state_t), true, &rejected));
                dpmi_desc_set_limit(data, 0x1FFFFu);
                cpu->es = 0;
                STATE_CHECK(!dpmi_client_buffer(vm, cpu->es, cpu->di,
                                                 sizeof(dpmi_saved_state_t), true, &rejected));
            } else {
                cpu->di = 0xFFFA;
                STATE_CHECK(dpmi_save_restore_state(vm) == DPMI_SERVICE_INVALID);
            }
        }
    }
    *cpu = original;
    *data = original_data;
    vm->dpmi.suspended_stack = original_pm;
    vm->dpmi.real_mode_stack = original_rm;
    vm->dpmi.suspended_paging = original_pm_paging;
    vm->dpmi.real_mode_paging = original_rm_paging;
    vm->dpmi.is_32bit = original_width;
    serial_puts("[DPMI-STATE-TEST] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef STATE_CHECK
    return failures;
}

static int dpmi_client_buffer_selftest(dos_vm_t *vm, uint16_t data_sel)
{
    cpu8086_state_t *cpu = vm->cpu;
    cpu8086_state_t original = *cpu;
    dpmi_descriptor_t *data = &vm->dpmi.ldt[dpmi_sel_to_index(data_sel)];
    dpmi_descriptor_t original_data = *data;
    bool original_width = vm->dpmi.is_32bit;
    int checks = 0, failures = 0;
#define BUFFER_CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    serial_puts("[DPMI-BUFFER-TEST] failed check="); serial_putdec(checks); \
    serial_puts("\n"); } } while (0)
    cpu->cr0 = 1;
    cpu8086_use_host_ldt(cpu);
    cpu->gdtr.base = 0x40000;
    cpu->gdtr.limit = 15;
    dpmi_client_buffer_t buffer = { 0 };

    /* GDT and LDT records obey the same data-access contract. */
    for (unsigned table = 0; table < 2; table++) {
        uint16_t selector = table ? 0x0Bu : data_sel;
        for (unsigned access = 0; access < 256; access++) {
            dpmi_build_desc(data, 0x20000, 0xFFFF, access, 0);
            for (unsigned i = 0; i < sizeof(*data); i++)
                dos_mem_write8(vm, 0x40008u + i, ((uint8_t *)data)[i]);
            bool code = (access & 8u) != 0;
            bool allowed = (access & 0x90u) == 0x90u &&
                           ((access & 0x60u) == 0x60u || (code && (access & 4u)));
            /* Expand-down data needs an offset above its limit. */
            uint32_t offset = !code && (access & 4u) ? 0x10000u : 0x100u;
            if (!code && (access & 4u)) allowed = false;
            BUFFER_CHECK(dpmi_client_buffer(vm, selector, offset, 50, false,
                                              &buffer) ==
                         (allowed && (!code || (access & 2u))));
            BUFFER_CHECK(dpmi_client_buffer(vm, selector, offset, 50, true,
                                              &buffer) ==
                         (allowed && !code && (access & 2u)));
        }
    }
    dpmi_build_desc(data, 0x20000, 0xFFFF, 0xF2, 0);
    BUFFER_CHECK(dpmi_client_buffer(vm, data_sel & ~3u, 0xFFCE, 50, true, &buffer));
    BUFFER_CHECK(!dpmi_client_buffer(vm, data_sel, 0xFFCF, 50, true, &buffer));
    BUFFER_CHECK(!dpmi_client_buffer(vm, 0, 0, 50, true, &buffer));
    BUFFER_CHECK(!dpmi_client_buffer(vm, data_sel, 0, 65, true, &buffer));
    dpmi_build_desc(data, 0, 0x1000, 0xF6, 0);
    BUFFER_CHECK(dpmi_client_buffer(vm, data_sel, 0x1001, 50, true, &buffer));
    BUFFER_CHECK(!dpmi_client_buffer(vm, data_sel, 0x1000, 50, false, &buffer));
    BUFFER_CHECK(!dpmi_client_buffer(vm, data_sel, 0xFFCF, 50, false, &buffer));
    data->flags_lim |= DESC_32BIT;
    BUFFER_CHECK(dpmi_client_buffer(vm, data_sel, 0xFFCF, 50, true, &buffer));
    dpmi_build_desc(data, vm->total_mem_size, 0xFFFF, 0xF2, 0);
    BUFFER_CHECK(!dpmi_client_buffer(vm, data_sel, 0, 50, true, &buffer));
    dpmi_desc_set_base(data, UINT32_MAX - 10u);
    BUFFER_CHECK(!dpmi_client_buffer(vm, data_sel, 0, 50, false, &buffer));

    const uint32_t linears[] = { 0x6FF0u, 0x403FFFF0u };
    for (unsigned test = 0; test < 2; test++) {
        uint32_t linear = linears[test], next = linear + 16u;
        dpmi_zero(vm->mem + 0x10000, 0x3000);
        dpmi_build_desc(data, 0, UINT32_MAX, 0xF2, DESC_32BIT);
        cpu->cr0 = 0x80000001u;
        cpu->cr3 = 0x10000;
        uint32_t pde1 = 0x10000u + (linear >> 22) * 4u;
        uint32_t pde2 = 0x10000u + (next >> 22) * 4u;
        uint32_t pte1 = 0x11000u + ((linear >> 12) & 0x3FFu) * 4u;
        uint32_t pte2 = (test ? 0x12000u : 0x11000u) +
                        ((next >> 12) & 0x3FFu) * 4u;
        dos_mem_write32(vm, pde1, 0x11007);
        dos_mem_write32(vm, pde2, test ? 0x12007 : 0x11007);
        dos_mem_write32(vm, pte1, 0x20007);
        dos_mem_write32(vm, pte2, 0x24007);
        uint8_t expected[50], result[50];
        for (unsigned i = 0; i < sizeof(expected); i++) expected[i] = i + 1u;
        vm->mem[0x20FEF] = vm->mem[0x21000] = vm->mem[0x23FFF] = vm->mem[0x24022] = 0xA5;
        BUFFER_CHECK(dpmi_client_buffer(vm, data_sel, linear, 50, true, &buffer));
        BUFFER_CHECK(buffer.count == 2 && buffer.pages[0].translation.physical == 0x20FF0 &&
                     buffer.pages[1].translation.physical == 0x24000);
        BUFFER_CHECK(!(dos_mem_read32(vm, pte1) & 0x60u) &&
                     !(dos_mem_read32(vm, pte2) & 0x60u));
        BUFFER_CHECK(dpmi_buffer_transfer(vm, &buffer, 0, result, 1, false));
        BUFFER_CHECK((dos_mem_read32(vm, pte1) & 0x60u) == 0x20u &&
                     !(dos_mem_read32(vm, pte2) & 0x60u));
        BUFFER_CHECK(dpmi_buffer_transfer(vm, &buffer, 0, expected, 50, true));
        BUFFER_CHECK(dpmi_buffer_transfer(vm, &buffer, 0, result, 50, false));
        for (unsigned i = 0; i < sizeof(expected); i++)
            BUFFER_CHECK(result[i] == expected[i]);
        BUFFER_CHECK((dos_mem_read32(vm, pte1) & 0x60u) == 0x60u &&
                     (dos_mem_read32(vm, pte2) & 0x60u) == 0x60u &&
                     (dos_mem_read32(vm, pde1) & 0x20u) &&
                     (dos_mem_read32(vm, pde2) & 0x20u));
        BUFFER_CHECK(vm->mem[0x20FEF] == 0xA5 && vm->mem[0x21000] == 0xA5 &&
                     vm->mem[0x23FFF] == 0xA5 && vm->mem[0x24022] == 0xA5);
        BUFFER_CHECK(!dpmi_buffer_transfer(vm, &buffer, 49, expected, 2, true));

        cpu->protected_mode = true;
        cpu->es = data_sel;
        cpu->edi = linear;
        vm->dpmi.is_32bit = true;
        cpu->ax = 0x0500;
        dos_int31_dpmi(vm);
        BUFFER_CHECK(!(cpu->eflags & FLAG_CF));
        BUFFER_CHECK(dpmi_buffer_load(vm, &buffer, 44, 4) == UINT32_MAX);
        dpmi_stack_t saved_rm = vm->dpmi.real_mode_stack;
        vm->dpmi.real_mode_stack = (dpmi_stack_t){ 0x1234, 0x5678 };
        cpu->edi = linear + 12u; /* State record starts at offset FFCh. */
        cpu->al = 0;
        BUFFER_CHECK(dpmi_save_restore_state(vm) == DPMI_SERVICE_COMPLETE);
        vm->dpmi.real_mode_stack = (dpmi_stack_t){ 0x2345, 0x6789 };
        cpu->al = 1;
        dos_mem_write32(vm, pte2, 0x24006);
        dpmi_client_buffer_t rejected;
        BUFFER_CHECK(!dpmi_client_buffer(vm, cpu->es, cpu->edi,
                                          sizeof(dpmi_saved_state_t), false, &rejected));
        BUFFER_CHECK(vm->dpmi.real_mode_stack.ss == 0x2345 &&
                     vm->dpmi.real_mode_stack.esp == 0x6789);
        dos_mem_write32(vm, pte2, 0x24007);
        BUFFER_CHECK(dpmi_save_restore_state(vm) == DPMI_SERVICE_COMPLETE);
        BUFFER_CHECK(vm->dpmi.real_mode_stack.ss == 0x1234 &&
                     vm->dpmi.real_mode_stack.esp == 0x5678);
        vm->dpmi.real_mode_stack = saved_rm;
        cpu->edi = linear;

        dpmi_rm_regs_t regs = { .cs = 0x3000, .flags = FLAGS_FIXED };
        vm->mem[0x30000] = 0xB8; /* MOV AX,CAFE; RETF */
        vm->mem[0x30001] = 0xFE;
        vm->mem[0x30002] = 0xCA;
        vm->mem[0x30003] = 0xCB;
        BUFFER_CHECK(dpmi_buffer_transfer(vm, &buffer, 0, &regs, sizeof(regs), true));
        cpu->ax = 0x0301;
        cpu->bx = cpu->cx = 0;
        cpu->running = true;
        dos_int31_dpmi(vm);
        BUFFER_CHECK(!(cpu->eflags & FLAG_CF) && cpu->running);
        BUFFER_CHECK(cpu->cr0 == 0x80000001u && cpu->cr3 == 0x10000u);
        BUFFER_CHECK(dpmi_buffer_transfer(vm, &buffer, 0, &regs, sizeof(regs), false));
        BUFFER_CHECK(regs.eax == 0xCAFE && regs.cs == 0x3000 && regs.ss == 0 && regs.sp == 0);

        /* Reject absent/supervisor/read-only mappings at either level.
         * Preflight failure cannot dirty the first page of a split record. */
        uint32_t entries[] = { pde1, pde2, pte1, pte2 };
        for (unsigned entry = 0; entry < 4; entry++) {
            uint32_t address = entries[entry];
            uint32_t value = dos_mem_read32(vm, address);
            for (unsigned bit = 0; bit < 3; bit++) {
                dos_mem_write32(vm, address, value & ~(1u << bit));
                buffer.size = 0x12345678;
                BUFFER_CHECK(!dpmi_client_buffer(vm, data_sel, linear, 50, true, &buffer));
                BUFFER_CHECK(buffer.size == 0x12345678);
                BUFFER_CHECK(dpmi_client_buffer(vm, data_sel, linear, 50, false,
                                                 &buffer) == (bit == 1));
                BUFFER_CHECK(dos_mem_read32(vm, address) == (value & ~(1u << bit)));
                dos_mem_write32(vm, address, value);
            }
        }
        dos_mem_write32(vm, pte2, vm->total_mem_size | 7u);
        BUFFER_CHECK(!dpmi_client_buffer(vm, data_sel, linear, 50, false, &buffer));
        dos_mem_write32(vm, pte2, 0x24007);
        dos_mem_write32(vm, pde2, vm->total_mem_size | 7u);
        BUFFER_CHECK(!dpmi_client_buffer(vm, data_sel, linear, 50, false, &buffer));
        cpu->cr3 = vm->total_mem_size;
        BUFFER_CHECK(!dpmi_client_buffer(vm, data_sel, linear, 50, false, &buffer));
    }

    /* The first physical span includes its own PTE. Resolve the whole
     * record before writing it, without translating the next byte again. */
    dpmi_zero(vm->mem + 0x10000, 0x3000);
    cpu->cr3 = 0x10000;
    dos_mem_write32(vm, 0x10000, 0x11007);
    dos_mem_write32(vm, 0x10004, 0x12007);
    dos_mem_write32(vm, 0x11FFC, 0x11007);
    dos_mem_write32(vm, 0x12000, 0x24007);
    uint8_t alias_record[50];
    for (unsigned i = 0; i < sizeof(alias_record); i++) alias_record[i] = 0x80u + i;
    BUFFER_CHECK(dpmi_client_buffer(vm, data_sel, 0x3FFFF0, 50, true, &buffer));
    BUFFER_CHECK(dpmi_buffer_transfer(vm, &buffer, 0, alias_record, 50, true));
    for (unsigned i = 0; i < sizeof(alias_record); i++)
        BUFFER_CHECK(dos_mem_read8(vm, i < 16 ? 0x11FF0 + i : 0x24000 + i - 16) == alias_record[i]);

    cpu->cr0 = 1;
    dpmi_build_desc(data, 0, 0x1FFFF, 0xF2, 0);
    cpu->es = data_sel;
    for (unsigned width = 0; width < 2; width++) {
        vm->dpmi.is_32bit = width != 0;
        cpu->edi = width ? 0x11000 : 0xA5A51000;
        cpu->ax = 0x0500;
        cpu->eflags = FLAG_CF | FLAGS_FIXED;
        dos_int31_dpmi(vm);
        BUFFER_CHECK(!(cpu->eflags & FLAG_CF));
        BUFFER_CHECK(dos_mem_read32(vm, (width ? 0x11000 : 0x1000) + 0x2C) == UINT32_MAX);
    }
    *cpu = original;
    *data = original_data;
    vm->dpmi.is_32bit = original_width;
    serial_puts("[DPMI-BUFFER-TEST] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef BUFFER_CHECK
    return failures;
}

static int dpmi_callback_buffer_selftest(dos_vm_t *vm, uint16_t data_sel,
                                        uint16_t stack_sel)
{
    cpu8086_state_t *cpu = vm->cpu;
    cpu8086_state_t original = *cpu;
    dpmi_state_t *dpmi = &vm->dpmi;
    dpmi_descriptor_t *data = &dpmi->ldt[dpmi_sel_to_index(data_sel)];
    dpmi_descriptor_t *stack = &dpmi->ldt[dpmi_sel_to_index(stack_sel)];
    dpmi_descriptor_t original_data = *data, original_stack = *stack;
    dpmi_stack_t original_rm = dpmi->real_mode_stack;
    dpmi_stack_t original_saved = dpmi->callback_real_stacks[0];
    dpmi_callback_t original_callback = dpmi->callbacks[0];
    bool original_width = dpmi->is_32bit, original_vif = dpmi->virtual_interrupts_enabled;
    bool original_saved_vif = dpmi->callback_virtual_interrupts[0];
    uint8_t original_slot = dpmi->callback_slots[0];
    int checks = 0, failures = 0;
#define CALLBACK_CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    serial_puts("[DPMI-CBBUFFER-TEST] failed check="); serial_putdec(checks); \
    serial_puts("\n"); } } while (0)
    dpmi_rm_regs_t regs = {
        .eax = 0x12345678, .ebx = 0x87654321, .ecx = 0xA5A51234, .edx = 0x5A5A5678,
        .esi = 0x11223344, .edi = 0x55667788, .ebp = 0xDEADBEEF,
        .flags = FLAGS_FIXED | FLAG_CF | FLAG_IF | FLAG_DF,
        .cs = 0x2000, .ip = 0x100, .ss = 0x2345, .sp = 0x8000,
        .ds = 0, .es = 0xFFFF, .fs = 0x1234, .gs = 0
    };
    dpmi_write_rm_regs(vm, 0x6000, &regs);
    for (unsigned width = 0; width < 2; width++) {
        for (unsigned stack32 = 0; stack32 < 2; stack32++) {
            for (unsigned fault = 0; fault < 5; fault++) {
                *cpu = original;
                cpu->protected_mode = true;
                cpu->cr0 = 1;
                cpu8086_use_host_ldt(cpu);
                cpu->cs = dpmi->sel_host_code;
                cpu->eip = DPMI_CALLBACK_RETURN_OFF + 2u;
                cpu->es = data_sel;
                cpu->edi = width ? 0x6000 : 0xA5A56000;
                cpu->ss = stack_sel;
                cpu->esp = stack32 ? 0x5000 : 0xA5A55000;
                dpmi->is_32bit = width != 0;
                dpmi->callback_depth = 1;
                dpmi->callback_slots[0] = 0;
                dpmi->callbacks[0].active = true;
                dpmi->callbacks[0].rm_regs_off = 0x6100; /* different input record */
                dpmi->callback_real_stacks[0] = (dpmi_stack_t){ 0x2222, 0x7777 };
                dpmi->callback_virtual_interrupts[0] = true;
                dpmi->real_mode_stack = (dpmi_stack_t){ 0x3333, 0x8888 };
                dpmi->virtual_interrupts_enabled = false;
                dpmi_build_desc(data, 0, 0xFFFF, 0xF0, 0); /* read-only output */
                dpmi_build_desc(stack, 0, 0xFFFF, 0xF2, stack32 ? DESC_32BIT : 0);
                if (fault == 1) cpu->es = 0;
                if (fault == 2) data->access &= ~DESC_PRESENT;
                if (fault == 3) dpmi_desc_set_limit(data, 0x6001);
                if (fault == 4) stack->access &= ~DESC_WRITABLE;
                cpu8086_sync_cs(cpu);
                cpu8086_sync_segment(cpu, 2);
                cpu8086_state_t before = *cpu;
                if (fault && fault < 4) {
                    dpmi_client_buffer_t buffer;
                    CALLBACK_CHECK(!dpmi_client_buffer(vm, cpu->es, cpu->di,
                                                        sizeof(regs), false, &buffer));
                } else CALLBACK_CHECK(dpmi_callback_return(vm, true) ==
                                      (fault ? DPMI_SERVICE_INVALID : DPMI_SERVICE_COMPLETE));
                if (fault) {
                    bool unchanged = true;
                    for (unsigned i = 0; i < sizeof(*cpu); i++)
                        if (((uint8_t *)cpu)[i] != ((uint8_t *)&before)[i]) unchanged = false;
                    CALLBACK_CHECK(unchanged && dpmi->callback_depth == 1 &&
                                   dpmi->real_mode_stack.ss == 0x3333 &&
                                   dpmi->real_mode_stack.esp == 0x8888 &&
                                   !dpmi->virtual_interrupts_enabled);
                } else {
                    CALLBACK_CHECK(!cpu->protected_mode && !(cpu->cr0 & 1u) &&
                                   cpu->cs == regs.cs && cpu->eip == regs.ip &&
                                   cpu->ss == regs.ss && cpu->esp == regs.sp);
                    CALLBACK_CHECK(cpu->eax == regs.eax && cpu->ebx == regs.ebx &&
                                   cpu->ecx == regs.ecx && cpu->edx == regs.edx &&
                                   cpu->esi == regs.esi && cpu->edi == regs.edi && cpu->ebp == regs.ebp);
                    CALLBACK_CHECK(cpu->ds == regs.ds && cpu->es == regs.es &&
                                   cpu->fs == regs.fs && cpu->gs == regs.gs && cpu->flags == regs.flags);
                    CALLBACK_CHECK(!dpmi->callback_depth && dpmi->real_mode_stack.ss == 0x2222 &&
                                   dpmi->real_mode_stack.esp == 0x7777 && dpmi->virtual_interrupts_enabled);
                }
            }
        }
    }
    *cpu = original;
    *data = original_data;
    *stack = original_stack;
    dpmi->callback_depth = 0;
    dpmi->callbacks[0] = original_callback;
    dpmi->callback_slots[0] = original_slot;
    dpmi->callback_real_stacks[0] = original_saved;
    dpmi->callback_virtual_interrupts[0] = original_saved_vif;
    dpmi->real_mode_stack = original_rm;
    dpmi->is_32bit = original_width;
    dpmi->virtual_interrupts_enabled = original_vif;
    serial_puts("[DPMI-CBBUFFER-TEST] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef CALLBACK_CHECK
    return failures;
}

typedef struct {
    uint16_t code, buffer, data, stack;
} dpmi_paging_fixture_t;

static dpmi_paging_fixture_t dpmi_paging_test_prepare(dos_vm_t *vm, unsigned width)
{
    cpu8086_state_t *cpu = vm->cpu;
    dpmi_zero(vm->mem, vm->total_mem_size);
    cpu8086_init(cpu, vm);
    dpmi_init(vm);
    vm->emulate_cpu = true;
    vm->step_limit = 512;
    vm->step_count = 0;
    vm->step_limit_reached = vm->process_terminated = false;
    vm->interpreter_stop_active = vm->interpreter_stop_reached = false;
    vm->timer_irq_pending = false;
    vm->start_ticks = vm->last_timer_tick = idt_get_ticks();
    vm->dpmi.active = true;
    vm->dpmi.is_32bit = width != 0;
    vm->dpmi.virtual_interrupts_enabled = false;
    dpmi_paging_fixture_t fixture = {
        dpmi_alloc_descriptor(&vm->dpmi), dpmi_alloc_descriptor(&vm->dpmi),
        dpmi_alloc_descriptor(&vm->dpmi), dpmi_alloc_descriptor(&vm->dpmi)
    };
    uint8_t flags = width ? DESC_32BIT : 0;
    dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(fixture.code)], 0x40000000, 0xFFFF, 0xFA, flags);
    dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(fixture.buffer)], 0x6000, 0xFFFF, 0xF2, flags);
    dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(fixture.data)], 0, 0xFFFF, 0xF2, flags);
    dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(fixture.stack)], 0, 0xFFFF, 0xF2, flags);
    for (unsigned context = 0; context < 2; context++) {
        uint32_t directory = 0x10000u + context * 0x4000u;
        uint32_t low = directory + 0x1000, high = directory + 0x2000;
        uint32_t physical = 0x20000u + context * 0x8000u;
        dos_mem_write32(vm, directory, low | 7u);
        dos_mem_write32(vm, directory + 0x400, high | 7u);
        for (unsigned i = 0; i < 512; i++) dos_mem_write32(vm, low + i * 4u, i * 4096u | 7u);
        dos_mem_write32(vm, high, physical | 7u);
        dos_mem_write32(vm, high + 4, (physical + 0x4000) | 7u);
        dos_mem_write32(vm, high + 8, (physical + 0x6000) | 7u);
        dos_mem_write32(vm, low + 24, (physical + 0x4000) | 7u);
        dos_mem_write32(vm, low + 28, (physical + 0x6000) | 7u);
        vm->mem[physical] = 0x90;
    }
    vm->dpmi.suspended_paging = (dpmi_paging_t){ DOS_CR0_PG | DOS_CR0_WP | 0x11u, 0x10018 };
    vm->dpmi.real_mode_paging = (dpmi_paging_t){ 0x10, 0x1C018 };
    cpu->cr0 = DOS_CR0_WP | 0x10u;
    cpu->cr3 = 0x18008;
    cpu->ss = 0;
    cpu->esp = 0x8000;
    cpu->eflags = FLAGS_FIXED | FLAG_CF | FLAG_IF;
    return fixture;
}

static int dpmi_callback_entry_recovery_selftest(void)
{
    enum { OK, REG_NP, REG_TYPE, REG_LIMIT, REG_PAGE, REG_RO, REG_SUP,
           CODE_NP, CODE_TYPE, CODE_LIMIT, CODE_PAGE, CODE_SUP, REVALIDATE,
           REPEATED, EDITED_REGS, REDIRECT_SAME, REDIRECT_OTHER, FREED, REUSED,
           EXITED, UNHANDLED, QUOTA, NESTED, REMAPPED, POLICIES };
    const unsigned memory_pages = 512;
    unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (vm) dos_host_free_pages(vm, vm_pages);
        if (memory) dos_host_free_pages(memory, memory_pages);
        return 1;
    }
    dpmi_zero(vm, vm_pages * 4096u);
    cpu8086_state_t cpu;
    vm->cpu = &cpu; vm->mem = memory; vm->total_mem_size = memory_pages * 4096u;
    unsigned checks = 0, recovered = 0;
    int failures = 0;
#define CB_ENTRY_CHECK(condition) do { \
    if (!(condition)) { \
        if (failures < 24) { serial_puts("[DPMI-CALLBACK-ENTRY] failed check="); serial_putdec(checks); \
            serial_puts(" policy/width/stack/path="); serial_putdec(policy); serial_puts("/"); \
            serial_putdec(width32); serial_putdec(stack32); serial_putdec(path); serial_puts("\n"); } \
        failures++; \
    } \
    checks++; \
} while (0)
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned path = 0; path < 3; path++)
    for (unsigned policy = 0; policy < POLICIES; policy++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        vm->step_limit = policy == QUOTA ? 128 : 4096;
        vm->software_int_frame_bytes = 0;
        dpmi_desc_set_limit(&vm->dpmi.ldt[dpmi_sel_to_index(f.data)], UINT32_MAX);
        vm->dpmi.ldt[dpmi_sel_to_index(f.stack)].flags_lim = stack32 ? DESC_32BIT : 0;
        dpmi_descriptor_t *bd = &vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)];
        dpmi_desc_set_base(bd, 0x6FFC);
        dos_mem_write32(vm, 0x1101C, 0x2A007);
        uint16_t target = dpmi_alloc_descriptor(&vm->dpmi);
        dpmi_descriptor_t *cd = &vm->dpmi.ldt[dpmi_sel_to_index(target)];
        dpmi_build_desc(cd, 0x40002000, 0xFFFF, 0xFA, width32 ? DESC_32BIT : 0);
        dos_mem_write32(vm, 0x12008, 0x30007);
        uint16_t alias = dpmi_alloc_descriptor(&vm->dpmi);
        dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(alias)], 0, 0xFFFF, 0xF2, 0);
        vm->dpmi.descriptor_state[dpmi_sel_to_index(alias)] = DPMI_DESC_HOST;
        vm->dpmi.callbacks[0] = (dpmi_callback_t){ .rm_seg = DPMI_ENTRY_SEG,
            .rm_off = DPMI_CALLBACK_BASE_OFF, .pm_sel = target, .pm_off = 0x20,
            .rm_regs_sel = f.buffer, .rm_regs_off = 0, .rm_stack_sel = alias,
            .generation = 1, .active = true };
        vm->dpmi.callback_generation = 1;
        dpmi_get_host_code_selector(vm);
        dpmi_get_exception_stack_selector(vm);
        vm->dpmi.exception_depth = 1;
        vm->dpmi.suspended_stack = (dpmi_stack_t){ f.stack, stack32 ? 0x9000 : 0xBEEF9000 };
        vm->dpmi.virtual_interrupts_enabled = true;
        uint32_t stub = dos_linear(DPMI_ENTRY_SEG, DPMI_CALLBACK_BASE_OFF);
        memory[stub] = 0xCD; memory[stub + 1] = DPMI_CALLBACK_ENTRY_INT;
        memory[stub + 2] = 0; memory[stub + 3] = 0xF4;
        cpu8086_load_real_cs(&cpu, DPMI_ENTRY_SEG);
        cpu.eip = DPMI_CALLBACK_BASE_OFF + (path == 2 ? 0 : 2);
        cpu.esp = 0xCAFE8000u + (path == 2 ? 6u : 0u);
        cpu.eax = 0x12345678; cpu.ebx = 0x23456789; cpu.ecx = 0x3456789A;
        cpu.edx = 0x456789AB; cpu.esi = 0x56789ABC; cpu.edi = 0x6789ABCD; cpu.ebp = 0x789ABCDE;
        cpu8086_load_real_segment(&cpu, 3, 0x2222);
        cpu8086_load_real_segment(&cpu, 0, 0x3333);
        cpu8086_load_real_segment(&cpu, 4, 0x4444);
        cpu8086_load_real_segment(&cpu, 5, 0x5555);
        dos_mem_write16(vm, 0x8000, DPMI_CALLBACK_BASE_OFF + 2);
        dos_mem_write16(vm, 0x8002, DPMI_ENTRY_SEG);
        dos_mem_write16(vm, 0x8004, FLAGS_FIXED | FLAG_IF | FLAG_CF);
        for (unsigned i = 0; i < 50; i++) {
            memory[i < 4 ? 0x24FFC + i : 0x2A000 + i - 4] = 0xA5;
            memory[i < 4 ? 0x2CFFC + i : 0x2E000 + i - 4] = 0x5A;
        }
        memory[0x24FFB] = memory[0x2A02E] = 0x39;
        bool code_fault = policy >= CODE_NP && policy <= CODE_SUP;
        unsigned vector = policy == REG_NP || policy == CODE_NP ? 11 :
            policy == REG_TYPE || policy == REG_LIMIT || policy == CODE_TYPE || policy == CODE_LIMIT ? 13 : 14;
        unsigned error = policy == REG_NP || policy == REG_TYPE ? f.buffer & ~3u :
            policy == CODE_NP || policy == CODE_TYPE ? target & ~3u : vector == 13 ? 0 :
            (code_fault ? 4u : 6u) | (policy == REG_RO || policy == REG_SUP || policy == CODE_SUP);
        if (policy == REG_NP) bd->access &= ~DESC_PRESENT;
        else if (policy == REG_TYPE) bd->access = 0xF0;
        else if (policy == REG_LIMIT) dpmi_desc_set_limit(bd, 48);
        else if (policy == CODE_NP) cd->access &= ~DESC_PRESENT;
        else if (policy == CODE_TYPE) cd->access = 0xF2;
        else if (policy == CODE_LIMIT) dpmi_desc_set_limit(cd, 0x1F);
        else if (code_fault) dos_mem_write32(vm, 0x12008, policy == CODE_PAGE ? 0x30006 : 0x30003);
        else if (policy != OK) dos_mem_write32(vm, 0x1101C,
            policy == REG_RO ? 0x2A005 : policy == REG_SUP ? 0x2A003 : 0x2A006);
        uint8_t *out;
#define CE_BYTE(value) (*out++ = (uint8_t)(value))
#define CE_WORD(value) do { uint16_t x_ = (value); CE_BYTE(x_); CE_BYTE(x_ >> 8); } while (0)
#define CE_DWORD(value) do { uint32_t x_ = (value); for (unsigned b_ = 0; b_ < 4; b_++) CE_BYTE(x_ >> (b_ * 8u)); } while (0)
#define CE_OP32() do { if (!width32) CE_BYTE(0x66); } while (0)
#define CE_OP16() do { if (width32) CE_BYTE(0x66); } while (0)
#define CE_ADDR32() do { if (!width32) CE_BYTE(0x67); } while (0)
#define CE_STORE(address, value) do { CE_OP32(); CE_ADDR32(); CE_BYTE(0xC7); CE_BYTE(0x05); CE_DWORD(address); CE_DWORD(value); } while (0)
#define CE_FRAME(offset, value) do { CE_BYTE(0x36); CE_OP32(); CE_ADDR32(); CE_BYTE(0xC7); CE_BYTE(0x45); CE_BYTE(offset); CE_DWORD(value); } while (0)
        for (unsigned v = 11; v <= 14; v++) if (policy != UNHANDLED) {
            vm->dpmi.real_exception_vectors[v].sel = f.code;
            vm->dpmi.real_exception_vectors[v].off = 0x100;
        }
        vm->dpmi.exception_vectors[3].sel = f.code;
        vm->dpmi.exception_vectors[3].off = 0x700;
        vm->dpmi.exception_extended[3] = true;
        out = memory + 0x20700;
        CE_OP32(); CE_ADDR32(); CE_BYTE(0xFF); CE_BYTE(0x05); CE_DWORD(0xA004);
        CE_OP32(); CE_BYTE(0x83); CE_BYTE(0xC4); CE_BYTE(32); CE_BYTE(0xCB);
        out = memory + 0x20100;
        CE_OP32(); CE_BYTE(0x60);
        CE_OP32(); CE_BYTE(0x89); CE_BYTE(0xE5);
        if (!stack32) { CE_OP32(); CE_BYTE(0x81); CE_BYTE(0xE5); CE_DWORD(0xFFFF); }
        CE_OP16(); CE_BYTE(0xB8); CE_WORD(f.data); CE_BYTE(0x8E); CE_BYTE(0xD8);
        CE_OP32(); CE_ADDR32(); CE_BYTE(0xFF); CE_BYTE(0x05); CE_DWORD(0xA000);
        for (unsigned i = 0; i < DPMI_EXCEPTION_EXT_FRAME_SIZE; i += 4) {
            CE_BYTE(0x36); CE_OP32(); CE_ADDR32(); CE_BYTE(0x8B); CE_BYTE(0x45); CE_BYTE(32u + i);
            CE_OP32(); CE_ADDR32(); CE_BYTE(0xA3); CE_DWORD(0xA040 + i);
        }
        if (policy == NESTED) CE_BYTE(0xCC);
        if (policy == EXITED) { CE_OP16(); CE_BYTE(0xB8); CE_WORD(0x4C2A); CE_BYTE(0xCD); CE_BYTE(0x21); }
        if (policy == QUOTA) { CE_BYTE(0xEB); CE_BYTE(0xFE); }
        uint8_t *retry_skip = NULL;
        if (policy == REPEATED) {
            CE_OP32(); CE_ADDR32(); CE_BYTE(0x83); CE_BYTE(0x3D); CE_DWORD(0xA000); CE_BYTE(1);
            CE_BYTE(0x0F); CE_BYTE(0x84); retry_skip = out;
            if (width32) CE_DWORD(0); else CE_WORD(0);
        }
        CE_STORE(0x1101C, policy == REMAPPED ? 0x2E007 : 0x2A007);
        CE_STORE(0x12008, 0x30007);
        if (policy == REMAPPED) CE_STORE(0x11018, 0x2C007);
        if (policy == REG_NP || policy == REG_TYPE || policy == REG_LIMIT ||
            policy == CODE_NP || policy == CODE_TYPE || policy == CODE_LIMIT || policy == REVALIDATE) {
            uint16_t sel = code_fault || policy == REVALIDATE ? target : f.buffer;
            CE_OP16(); CE_BYTE(0xBB); CE_WORD(sel);
            CE_OP16(); CE_BYTE(0xB8); CE_WORD(9);
            CE_OP16(); CE_BYTE(0xB9); CE_WORD((width32 ? 0x4000 : 0) | (sel == target ? 0xFA : 0xF2));
            CE_BYTE(0xCD); CE_BYTE(0x31);
            CE_OP16(); CE_BYTE(0xB8); CE_WORD(8);
            CE_OP16(); CE_BYTE(0xB9); CE_WORD(0);
            CE_OP16(); CE_BYTE(0xBA); CE_WORD(0xFFFF); CE_BYTE(0xCD); CE_BYTE(0x31);
        }
        if (policy == REVALIDATE) {
            CE_OP32(); CE_ADDR32(); CE_BYTE(0x83); CE_BYTE(0x3D); CE_DWORD(0xA000); CE_BYTE(1);
            CE_BYTE(0x75); uint8_t *skip = out++;
            CE_OP16(); CE_BYTE(0xB8); CE_WORD(9);
            CE_OP16(); CE_BYTE(0xB9); CE_WORD((width32 ? 0x4000 : 0) | 0x7A); CE_BYTE(0xCD); CE_BYTE(0x31);
            *skip = out - skip - 1u;
        }
        if (policy == EDITED_REGS) {
            CE_FRAME(28, 0x0BADCAFE); CE_FRAME(4, 0x11112222); CE_FRAME(0, 0x33334444);
            CE_FRAME(32 + 0x38, 0xABCD8123); CE_FRAME(32 + 0x3C, 0x1234);
            CE_FRAME(32 + 0x40, 0x2345); CE_FRAME(32 + 0x44, 0x3456);
            CE_FRAME(32 + 0x48, 0x4567); CE_FRAME(32 + 0x4C, 0x5678);
            CE_FRAME(32 + 0x34, FLAG_VM | FLAG_DF | FLAG_CF | FLAG_IF | FLAGS_FIXED);
        }
        if (policy == REDIRECT_SAME || policy == REDIRECT_OTHER) {
            CE_FRAME(32 + 0x30, ((DPMI_EXCEPTION_HOST | DPMI_EXCEPTION_REDIRECT) << 16) |
                     (policy == REDIRECT_SAME ? DPMI_ENTRY_SEG : 0x1234));
            if (policy == REDIRECT_OTHER) CE_FRAME(32 + 0x2C, 0x5678);
            CE_FRAME(32 + 0x34, FLAG_VM | FLAG_DF | FLAGS_FIXED);
        }
        if (policy == FREED || policy == REUSED) {
            CE_OP16(); CE_BYTE(0xB8); CE_WORD(0x0304);
            CE_OP16(); CE_BYTE(0xB9); CE_WORD(DPMI_ENTRY_SEG);
            CE_OP16(); CE_BYTE(0xBA); CE_WORD(DPMI_CALLBACK_BASE_OFF); CE_BYTE(0xCD); CE_BYTE(0x31);
            if (policy == REUSED) {
                CE_OP16(); CE_BYTE(0xB8); CE_WORD(target); CE_BYTE(0x8E); CE_BYTE(0xD8);
                CE_OP16(); CE_BYTE(0xB8); CE_WORD(f.buffer); CE_BYTE(0x8E); CE_BYTE(0xC0);
                CE_OP32(); CE_BYTE(0xBE); CE_DWORD(0x20); CE_OP32(); CE_BYTE(0xBF); CE_DWORD(0);
                CE_OP16(); CE_BYTE(0xB8); CE_WORD(0x0303); CE_BYTE(0xCD); CE_BYTE(0x31);
            }
        }
        if (retry_skip) {
            uint32_t distance = out - retry_skip - (width32 ? 4u : 2u);
            for (unsigned i = 0; i < (width32 ? 4u : 2u); i++) retry_skip[i] = distance >> (8u * i);
        }
        CE_OP32(); CE_BYTE(0x61);
        if (stack32 != width32) CE_BYTE(0x66);
        CE_BYTE(0x83); CE_BYTE(0xC4); CE_BYTE(32); CE_BYTE(0xCB);
        CB_ENTRY_CHECK(out < memory + 0x20700);
#undef CE_FRAME
#undef CE_STORE
#undef CE_ADDR32
#undef CE_OP16
#undef CE_OP32
#undef CE_DWORD
#undef CE_WORD
#undef CE_BYTE
        cpu8086_state_t before = cpu;
        dpmi_paging_t real_paging = vm->dpmi.real_mode_paging;
        dpmi_service_result_t result = DPMI_SERVICE_INVALID;
        if (!path) result = dpmi_callback_enter(vm);
        else if (path == 1) cpu8086_real_host_interrupt(&cpu, DPMI_CALLBACK_ENTRY_INT, cpu.eflags);
        else (void)cpu8086_run_one(vm);
        bool stopped = policy == EXITED || policy == UNHANDLED || policy == QUOTA;
        bool replaced = policy == FREED || policy == REUSED;
        bool cancelled = policy == REDIRECT_SAME || policy == REDIRECT_OTHER;
        bool completed = !stopped && !replaced && !cancelled;
        if (!path) CB_ENTRY_CHECK(result == (completed ? DPMI_SERVICE_COMPLETE :
                                 replaced ? DPMI_SERVICE_INVALID : DPMI_SERVICE_INTERRUPTED));
        CB_ENTRY_CHECK(!vm->dpmi.host_wait && !vm->native_dispatch_depth &&
                       !vm->interpreter_stop_signal && !vm->software_int_frame_bytes);
        CB_ENTRY_CHECK(vm->dpmi.exception_depth == 1 && vm->dpmi.callback_depth == completed);
        CB_ENTRY_CHECK(dos_mem_read32(vm, 0xA000) == (policy == OK || policy == UNHANDLED ? 0 :
                       policy == REVALIDATE || policy == REPEATED ? 2 : 1));
        CB_ENTRY_CHECK(dos_mem_read32(vm, 0xA004) == (policy == NESTED));
        if (stopped || (replaced && path)) {
            CB_ENTRY_CHECK(!cpu.running && (policy == QUOTA ? vm->step_limit_reached :
                           cpu.exit_code == (policy == EXITED ? 42 : -1)));
        } else if (!completed) {
            CB_ENTRY_CHECK(cpu.running && !cpu.protected_mode && cpu.esp == 0xCAFE8006 && cpu.ss == 0);
            CB_ENTRY_CHECK(cpu.cs == (policy == REDIRECT_OTHER ? 0x1234 : DPMI_ENTRY_SEG) &&
                           cpu.eip == (policy == REDIRECT_OTHER ? 0x5678 : DPMI_CALLBACK_BASE_OFF + 2));
            if (cancelled) CB_ENTRY_CHECK(cpu.flags == (FLAG_DF | FLAGS_FIXED));
        }
        if (policy != OK && policy != UNHANDLED) {
            if (policy == REVALIDATE) { vector = 11; error = target & ~3u; }
            CB_ENTRY_CHECK(dos_mem_read32(vm, 0xA040 + 0x28) == error &&
                           dos_mem_read32(vm, 0xA040 + 0x2C) == DPMI_CALLBACK_BASE_OFF + 2 &&
                           dos_mem_read32(vm, 0xA040 + 0x30) == (DPMI_ENTRY_SEG | (DPMI_EXCEPTION_HOST << 16)));
            CB_ENTRY_CHECK(dos_mem_read32(vm, 0xA040 + 0x38) == 0xCAFE8006 &&
                           dos_mem_read32(vm, 0xA040 + 0x34) == (FLAG_VM | FLAGS_FIXED | FLAG_CF | FLAG_IF));
            if (vector == 14) {
                uint32_t expected_pte = code_fault ? policy == CODE_SUP ? 3 : 6 :
                    policy == REG_RO ? 5 : policy == REG_SUP ? 3 : 6;
                CB_ENTRY_CHECK(dos_mem_read32(vm, 0xA040 + 0x50) == (code_fault ? 0x40002020 : 0x7000) &&
                               dos_mem_read32(vm, 0xA040 + 0x54) == expected_pte);
            }
        }
        if (completed) {
            dpmi_rm_regs_t record;
            bool remap = policy == REMAPPED;
            for (unsigned i = 0; i < sizeof(record); i++) ((uint8_t *)&record)[i] =
                memory[i < 4 ? (remap ? 0x2CFFC : 0x24FFC) + i : (remap ? 0x2E000 : 0x2A000) + i - 4];
            bool edited = policy == EDITED_REGS;
            CB_ENTRY_CHECK(record.eax == (edited ? 0x0BADCAFE : before.eax) && record.ebx == before.ebx &&
                           record.esi == (edited ? 0x11112222 : before.esi) && record.edi == (edited ? 0x33334444 : before.edi));
            CB_ENTRY_CHECK(record.ip == DPMI_CALLBACK_BASE_OFF + 2 && record.cs == DPMI_ENTRY_SEG &&
                           record.sp == (edited ? 0x8123 : 0x8006) && record.ss == (edited ? 0x1234 : 0));
            CB_ENTRY_CHECK(record.ds == (edited ? 0x3456 : before.ds) && record.es == (edited ? 0x2345 : before.es) &&
                           record.fs == (edited ? 0x4567 : before.fs) && record.gs == (edited ? 0x5678 : before.gs) &&
                           record.flags == (FLAGS_FIXED | FLAG_CF | FLAG_IF | (edited ? FLAG_DF : 0)));
            CB_ENTRY_CHECK(cpu.protected_mode && cpu.cs == target && cpu.eip == 0x20 &&
                           cpu.ss == f.stack && cpu.esp == vm->dpmi.suspended_stack.esp - (width32 ? 12u : 6u) &&
                           cpu.ds == alias && cpu.esi == record.sp && cpu.es == f.buffer && !cpu.edi);
            CB_ENTRY_CHECK(cpu.cr3 == 0x10018 && vm->dpmi.real_mode_paging.cr3 == before.cr3 &&
                           vm->dpmi.real_mode_paging.cr0 == before.cr0 &&
                           dpmi_desc_get_base(&cpu.ds_cache.descriptor) == (uint32_t)record.ss << 4);
            if (policy != OK) recovered++;
        } else {
            bool intact = true;
            for (unsigned i = 0; i < 50; i++) if (memory[i < 4 ? 0x24FFC + i : 0x2A000 + i - 4] != 0xA5) intact = false;
            CB_ENTRY_CHECK(intact && vm->dpmi.real_mode_paging.cr0 == real_paging.cr0 &&
                           vm->dpmi.real_mode_paging.cr3 == real_paging.cr3);
        }
        CB_ENTRY_CHECK(memory[0x24FFB] == 0x39 && memory[0x2A02E] == 0x39);
        if (replaced) CB_ENTRY_CHECK(vm->dpmi.callbacks[0].active == (policy == REUSED) &&
                       vm->dpmi.callback_generation == (policy == REUSED ? 2u : 1u));
    }
    serial_puts("[DPMI-CALLBACK-ENTRY] checks="); serial_putdec(checks);
    serial_puts(" recovered="); serial_putdec(recovered);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef CB_ENTRY_CHECK
    dos_host_free_pages(memory, memory_pages); dos_host_free_pages(vm, vm_pages);
    return failures;
}

static int dpmi_callback_registration_selftest(void)
{
    enum {
        REG_OK, REG_PAGE, REG_READONLY, REG_SUPERVISOR, REG_TYPE, REG_NP, REG_LIMIT,
        CODE_TYPE, CODE_NP, CODE_LIMIT, CODE_PAGE, CODE_SUPERVISOR, CODE_BACKING,
        REG_BACKING, REPEATED, NESTED_BP, CHANGED_ARGS, NESTED_REG, CANCELLED,
        EXITED, QUOTA, UNHANDLED, REVALIDATE, FULL_CALLBACKS, GENERATION_EXHAUSTED, NO_DESCRIPTORS,
        ONE_DESCRIPTOR, TWO_DESCRIPTORS, HOSTS_ONLY, SHORT_STUB, CODE_WRAP,
        CODE_READONLY, REG_REMAP, REG_POLICIES
    };
    const unsigned memory_pages = 512;
    unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (vm) dos_host_free_pages(vm, vm_pages);
        if (memory) dos_host_free_pages(memory, memory_pages);
        return 1;
    }
    dpmi_zero(vm, vm_pages * 4096u);
    cpu8086_state_t cpu;
    vm->cpu = &cpu; vm->mem = memory;
    unsigned checks = 0, recovered = 0;
    int failures = 0;
#define REG_CHECK(condition) do { \
    if (!(condition)) { \
        if (failures < 24) { serial_puts("[DPMI-CALLBACK-REGISTER] failed check="); serial_putdec(checks); \
            serial_puts(" policy/width/stack/high/int="); serial_putdec(policy); serial_puts("/"); \
            serial_putdec(width32); serial_putdec(stack32); serial_putdec(high); serial_putdec(interpreted); \
            serial_puts("\n"); } \
        failures++; \
    } \
    checks++; \
} while (0)
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned interpreted = 0; interpreted < 2; interpreted++)
    for (unsigned policy = 0; policy < REG_POLICIES; policy++) {
        vm->total_mem_size = memory_pages * 4096u;
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        vm->step_limit = policy == QUOTA ? 128 : 4096;
        unsigned width = width32 ? 4u : 2u;
        uint32_t linear = high ? 0x3FFFFC : 0x6FFC;
        uint32_t first = high ? 0x11FFC : 0x11018, second = high ? 0x13000 : 0x1101C;
        if (high) dos_mem_write32(vm, 0x10004, 0x13007);
        dos_mem_write32(vm, first, 0x24007); dos_mem_write32(vm, second, 0x2A007);
        uint16_t target = dpmi_alloc_descriptor(&vm->dpmi);
        dpmi_descriptor_t *code = &vm->dpmi.ldt[dpmi_sel_to_index(target)];
        dpmi_build_desc(code, 0x40002000, 0x20, 0xFA, width32 ? DESC_32BIT : 0);
        dpmi_descriptor_t *buffer = &vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)];
        dpmi_desc_set_base(buffer, linear);
        dpmi_desc_set_limit(buffer, sizeof(dpmi_rm_regs_t) - 1u);
        dpmi_desc_set_limit(&vm->dpmi.ldt[dpmi_sel_to_index(f.data)], UINT32_MAX);
        vm->dpmi.ldt[dpmi_sel_to_index(f.stack)].flags_lim = stack32 ? DESC_32BIT : 0;
        bool resource_error = policy >= FULL_CALLBACKS && policy <= SHORT_STUB;
        bool fresh_hosts = policy >= NO_DESCRIPTORS && policy <= TWO_DESCRIPTORS;
        if (!fresh_hosts && policy != SHORT_STUB) {
            dpmi_get_exception_stack_selector(vm); dpmi_get_host_code_selector(vm);
        }
        for (unsigned i = 0; i < sizeof(dpmi_rm_regs_t); i++) {
            memory[i < 4 ? 0x24FFC + i : 0x2A000 + i - 4u] = 0xA5;
            memory[i < 4 ? 0x2CFFC + i : 0x2E000 + i - 4u] = 0x5A;
        }
        memory[0x24FFB] = memory[0x25000] = memory[0x29FFF] = memory[0x2A02E] = 0x39;
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cs = f.code; cpu.ds = target; cpu.es = f.buffer; cpu.ss = f.stack;
        cpu.esp = stack32 ? 0x8F00 : 0xBEEF8F00; cpu.eip = interpreted ? 0 : 15;
        cpu.eax = 0xA5A50303; cpu.ebx = 0xABCDEF01; cpu.ecx = 0x11223344;
        cpu.edx = 0x55667788; cpu.esi = width32 ? 0x20 : 0x98760020;
        cpu.edi = width32 ? 0 : 0x12340000; cpu.ebp = 0xCAFEBABE;
        cpu.eflags = FLAGS_FIXED | FLAG_CF | FLAG_DF | FLAG_IF;
        cpu8086_sync_cs(&cpu); cpu8086_sync_data(&cpu);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        vm->dpmi.virtual_interrupts_enabled = true;
        for (unsigned i = 0; i < 13; i++) memory[0x20000 + i] = 0x3E;
        memory[0x2000D] = 0xCD; memory[0x2000E] = 0x31;
        memory[0x2000F] = memory[0x20080] = 0xF4;
        uint8_t *out;
#define RG_BYTE(value) (*out++ = (uint8_t)(value))
#define RG_WORD(value) do { uint16_t x_ = (value); RG_BYTE(x_); RG_BYTE(x_ >> 8); } while (0)
#define RG_DWORD(value) do { uint32_t x_ = (value); for (unsigned b_ = 0; b_ < 4; b_++) RG_BYTE(x_ >> (b_ * 8u)); } while (0)
#define RG_OP32() do { if (!width32) RG_BYTE(0x66); } while (0)
#define RG_OP16() do { if (width32) RG_BYTE(0x66); } while (0)
#define RG_ADDR32() do { if (!width32) RG_BYTE(0x67); } while (0)
        for (unsigned vector = 11; vector <= 14; vector++) {
            unsigned off = 0x100 + (vector - 11u) * 0x60;
            if (policy != UNHANDLED) {
                vm->dpmi.exception_vectors[vector].sel = f.code;
                vm->dpmi.exception_vectors[vector].off = off;
            }
            out = memory + 0x20000 + off;
            RG_BYTE(0x1E); RG_BYTE(0x06); RG_OP32(); RG_BYTE(0x60);
            RG_OP16(); RG_BYTE(0xB8); RG_WORD(f.data); RG_BYTE(0x8E); RG_BYTE(0xD8);
            RG_ADDR32(); RG_BYTE(0xC6); RG_BYTE(0x05); RG_DWORD(0x8010); RG_BYTE(vector);
            RG_BYTE(0xE9);
            if (width32) { RG_DWORD(0x20300 - (uint32_t)(out - memory) - 4u); }
            else { RG_WORD(0x20300 - (uint32_t)(out - memory) - 2u); }
        }
        vm->dpmi.exception_vectors[3].sel = f.code; vm->dpmi.exception_vectors[3].off = 0x700;
        out = memory + 0x20300;
        RG_OP32(); RG_BYTE(0x89); RG_BYTE(0xE5);
        if (!stack32) { RG_OP32(); RG_BYTE(0x81); RG_BYTE(0xE5); RG_DWORD(0xFFFF); }
        RG_OP32(); RG_ADDR32(); RG_BYTE(0xFF); RG_BYTE(0x05); RG_DWORD(0x8000);
        const unsigned fields[] = { 2, 3, 4, 6, 7 };
        for (unsigned i = 0; i < 5; i++) {
            RG_BYTE(0x36); RG_ADDR32(); RG_BYTE(0x8B); RG_BYTE(0x45); RG_BYTE(32u + (2u + fields[i]) * width);
            RG_ADDR32(); RG_BYTE(0xA3); RG_DWORD(0x8020 + i * 4u);
        }
        if (policy == NESTED_BP) RG_BYTE(0xCC);
        if (policy == EXITED) { RG_OP16(); RG_BYTE(0xB8); RG_WORD(0x4C2A); RG_BYTE(0xCD); RG_BYTE(0x21); }
        if (policy == QUOTA) { RG_BYTE(0xEB); RG_BYTE(0xFE); }
        uint8_t *skip = NULL, *begin = NULL;
        if (policy == REPEATED) {
            RG_OP32(); RG_ADDR32(); RG_BYTE(0x83); RG_BYTE(0x3D); RG_DWORD(0x8000); RG_BYTE(1);
            RG_BYTE(0x74); skip = out++; begin = out;
        }
        RG_OP32(); RG_ADDR32(); RG_BYTE(0xC7); RG_BYTE(0x05); RG_DWORD(second);
        RG_DWORD(policy == REG_REMAP ? 0x2E007 : 0x2A007);
        RG_OP32(); RG_ADDR32(); RG_BYTE(0xC7); RG_BYTE(0x05); RG_DWORD(0x12008); RG_DWORD(0x26005);
        if (policy == REG_REMAP) {
            RG_OP32(); RG_ADDR32(); RG_BYTE(0xC7); RG_BYTE(0x05); RG_DWORD(first); RG_DWORD(0x2C007);
        }
        if (policy >= REG_TYPE && policy <= CODE_LIMIT) {
            bool code_fault = policy >= CODE_TYPE;
            bool limit_fault = policy == REG_LIMIT || policy == CODE_LIMIT;
            RG_OP16(); RG_BYTE(0xBB); RG_WORD(code_fault ? target : f.buffer);
            RG_OP16(); RG_BYTE(0xB8); RG_WORD(limit_fault ? 8 : 9);
            RG_OP16(); RG_BYTE(0xB9); RG_WORD(limit_fault ? 0 :
                (width32 ? 0x4000 : 0) | (code_fault ? 0xFA : 0xF2));
            if (limit_fault) { RG_OP16(); RG_BYTE(0xBA); RG_WORD(0xFFFF); }
            RG_BYTE(0xCD); RG_BYTE(0x31);
        }
        if (policy == CODE_WRAP) {
            RG_OP16(); RG_BYTE(0xBB); RG_WORD(target);
            RG_OP16(); RG_BYTE(0xB9); RG_WORD(0x4000);
            RG_OP16(); RG_BYTE(0xBA); RG_WORD(0x2000);
            RG_OP16(); RG_BYTE(0xB8); RG_WORD(7); RG_BYTE(0xCD); RG_BYTE(0x31);
        }
        if (policy == REVALIDATE) {
            RG_OP16(); RG_BYTE(0xBA); RG_WORD(0x1F);
            RG_OP32(); RG_ADDR32(); RG_BYTE(0x83); RG_BYTE(0x3D); RG_DWORD(0x8000); RG_BYTE(1);
            RG_BYTE(0x74); RG_BYTE(width32 ? 4 : 3);
            RG_OP16(); RG_BYTE(0xBA); RG_WORD(0x20);
            RG_OP16(); RG_BYTE(0xBB); RG_WORD(target); RG_OP16(); RG_BYTE(0xB9); RG_WORD(0);
            RG_OP16(); RG_BYTE(0xB8); RG_WORD(8); RG_BYTE(0xCD); RG_BYTE(0x31);
        }
        if (skip) *skip = (uint8_t)(out - begin);
        if (policy == NESTED_REG) {
            RG_OP16(); RG_BYTE(0xB8); RG_WORD(f.data); RG_BYTE(0x8E); RG_BYTE(0xC0);
            RG_OP16(); RG_BYTE(0xB8); RG_WORD(f.code); RG_BYTE(0x8E); RG_BYTE(0xD8);
            RG_OP32(); RG_BYTE(0xBE); RG_DWORD(0x700); RG_OP32(); RG_BYTE(0xBF); RG_DWORD(0x9000);
            RG_OP16(); RG_BYTE(0xB8); RG_WORD(0x0303); RG_BYTE(0xCD); RG_BYTE(0x31);
            RG_OP16(); RG_BYTE(0xB8); RG_WORD(f.data); RG_BYTE(0x8E); RG_BYTE(0xD8);
            RG_OP16(); RG_ADDR32(); RG_BYTE(0x89); RG_BYTE(0x0D); RG_DWORD(0x8034);
            RG_OP16(); RG_ADDR32(); RG_BYTE(0x89); RG_BYTE(0x15); RG_DWORD(0x8036);
        }
        if (policy == CHANGED_ARGS) {
            for (unsigned i = 0; i < 2; i++) {
                RG_BYTE(0x36); RG_OP16(); RG_ADDR32(); RG_BYTE(0xC7); RG_BYTE(0x45);
                RG_BYTE(32u + i * width); RG_WORD(f.data);
                RG_BYTE(0x36); RG_OP32(); RG_ADDR32(); RG_BYTE(0xC7); RG_BYTE(0x45);
                RG_BYTE(i * 4u); RG_DWORD(i ? 0x89ABCDEF : 0x12345678);
            }
        }
        if (policy == CANCELLED) {
            RG_BYTE(0x36); RG_ADDR32(); RG_BYTE(0xC7); RG_BYTE(0x45); RG_BYTE(32u + 5u * width);
            if (width32) { RG_DWORD(0x80); } else { RG_WORD(0x80); }
        }
        RG_BYTE(0x36); RG_ADDR32(); RG_BYTE(0x81); RG_BYTE(0x65); RG_BYTE(32u + 7u * width);
        if (width32) { RG_DWORD(~(uint32_t)FLAG_DF); } else { RG_WORD(~FLAG_DF); }
        RG_OP32(); RG_BYTE(0x61); RG_BYTE(0x07); RG_BYTE(0x1F); RG_BYTE(0xCB);
        REG_CHECK(out < memory + 0x20700);
        out = memory + 0x20700;
        RG_OP32(); RG_ADDR32(); RG_BYTE(0xFF); RG_BYTE(0x05); RG_DWORD(0x8004); RG_BYTE(0xCB);
#undef RG_ADDR32
#undef RG_OP16
#undef RG_OP32
#undef RG_DWORD
#undef RG_WORD
#undef RG_BYTE
        bool faulted = !resource_error && policy != REG_OK && policy != CODE_READONLY;
        bool code_fault = (policy >= CODE_TYPE && policy <= CODE_BACKING) || policy == CODE_WRAP;
        if (faulted && !code_fault && !(policy >= REG_TYPE && policy <= REG_LIMIT))
            dos_mem_write32(vm, second, policy == REG_READONLY ? 0x2A005 : policy == REG_SUPERVISOR ? 0x2A003 :
                                        policy == REG_BACKING ? vm->total_mem_size | 7u : 0x2A006);
        if (policy == REG_TYPE) buffer->access = 0xF0;
        if (policy == REG_NP) buffer->access &= ~DESC_PRESENT;
        if (policy == REG_LIMIT) dpmi_desc_set_limit(buffer, sizeof(dpmi_rm_regs_t) - 2u);
        if (policy == CODE_TYPE) code->access = 0xF2;
        if (policy == CODE_NP) code->access &= ~DESC_PRESENT;
        if (policy == CODE_LIMIT) dpmi_desc_set_limit(code, 0x1F);
        if (policy == CODE_WRAP) dpmi_desc_set_base(code, UINT32_MAX);
        if (policy == CODE_PAGE || policy == CODE_SUPERVISOR || policy == CODE_BACKING || policy == CODE_READONLY)
            dos_mem_write32(vm, 0x12008, policy == CODE_PAGE ? 0x26004 : policy == CODE_SUPERVISOR ? 0x26001 :
                                        policy == CODE_BACKING ? vm->total_mem_size | 5u : 0x26005);
        if (policy == FULL_CALLBACKS)
            for (unsigned i = 0; i < DPMI_MAX_CALLBACKS; i++) vm->dpmi.callbacks[i].active = true;
        if (policy == GENERATION_EXHAUSTED) vm->dpmi.callback_generation = UINT64_MAX;
        if (fresh_hosts || policy == HOSTS_ONLY) {
            unsigned available = fresh_hosts ? policy - NO_DESCRIPTORS : 0;
            for (unsigned i = 0; i < DPMI_MAX_DESCRIPTORS - available; i++)
                if (vm->dpmi.descriptor_state[i] == DPMI_DESC_FREE)
                    vm->dpmi.descriptor_state[i] = DPMI_DESC_HOST;
        }
        if (policy == SHORT_STUB) vm->total_mem_size = dos_linear(DPMI_ENTRY_SEG, DPMI_CALLBACK_BASE_OFF) + 3u;
        uint8_t states[DPMI_MAX_DESCRIPTORS];
        for (unsigned i = 0; i < DPMI_MAX_DESCRIPTORS; i++) states[i] = vm->dpmi.descriptor_state[i];
        uint16_t old_stack = vm->dpmi.sel_exception_stack, old_code = vm->dpmi.sel_host_code;
        uint16_t next_free = vm->dpmi.next_free_index;
        cpu8086_state_t before = cpu;
        if (interpreted) (void)cpu8086_run_one(vm); else dos_int31_dpmi(vm);
        bool cancelled = policy == CANCELLED, stopped = policy == EXITED || policy == QUOTA || policy == UNHANDLED;
        bool completed = !cancelled && !stopped && !resource_error;
        unsigned expected_faults = policy == UNHANDLED ? 0 : policy == REPEATED || policy == REVALIDATE ? 2u : faulted;
        REG_CHECK(dos_mem_read32(vm, 0x8000) == expected_faults && dos_mem_read32(vm, 0x8004) == (policy == NESTED_BP));
        REG_CHECK(!vm->dpmi.host_wait && !vm->interpreter_stop_signal && !vm->software_int_frame_bytes && !vm->native_dispatch_depth);
        unsigned active = 0;
        for (unsigned i = 0; i < DPMI_MAX_CALLBACKS; i++) if (vm->dpmi.callbacks[i].active) active++;
        REG_CHECK(active == (policy == FULL_CALLBACKS ? DPMI_MAX_CALLBACKS : completed ? 1u + (policy == NESTED_REG) : 0u));
        REG_CHECK(!vm->dpmi.callback_depth);
        if (stopped) {
            REG_CHECK(!cpu.running && (policy == QUOTA ? vm->step_limit_reached : cpu.exit_code == (policy == EXITED ? 42 : -1)));
        } else {
            REG_CHECK(cpu.running && !vm->dpmi.exception_depth && cpu.cs == before.cs &&
                      cpu.eip == (cancelled ? 0x80u : 15u) && cpu.ss == before.ss && cpu.esp == before.esp);
            REG_CHECK(cpu.eax == (resource_error ? (before.eax & 0xFFFF0000u) | 0x8015u : before.eax) &&
                      cpu.ebx == before.ebx && cpu.ebp == before.ebp);
            REG_CHECK(cpu.ds == (policy == CHANGED_ARGS ? f.data : before.ds) && cpu.es == (policy == CHANGED_ARGS ? f.data : before.es) &&
                      cpu.esi == (policy == CHANGED_ARGS ? 0x89ABCDEF : before.esi) && cpu.edi == (policy == CHANGED_ARGS ? 0x12345678 : before.edi));
            REG_CHECK(!!(cpu.flags & FLAG_CF) == !completed && !!(cpu.flags & FLAG_DF) == !faulted);
            REG_CHECK(cpu.cr0 == before.cr0 && cpu.cr3 == before.cr3);
            if (completed) {
                unsigned slot = policy == NESTED_REG;
                dpmi_callback_t *cb = &vm->dpmi.callbacks[slot];
                REG_CHECK(cpu.cx == DPMI_ENTRY_SEG && cpu.dx == DPMI_CALLBACK_BASE_OFF + slot * DPMI_CALLBACK_STUB_SIZE &&
                          (cpu.ecx & 0xFFFF0000u) == (before.ecx & 0xFFFF0000u) && (cpu.edx & 0xFFFF0000u) == (before.edx & 0xFFFF0000u));
                REG_CHECK(cb->pm_sel == target && cb->pm_off == 0x20 && cb->rm_regs_sel == f.buffer && cb->rm_regs_off == 0 &&
                          vm->dpmi.descriptor_state[dpmi_sel_to_index(cb->rm_stack_sel)] == DPMI_DESC_HOST &&
                          cb->generation == slot + 1u && vm->dpmi.callback_generation == slot + 1u);
                if (faulted) recovered++;
            } else REG_CHECK(cpu.ecx == before.ecx && cpu.edx == before.edx);
        }
        if (faulted && !stopped) {
            unsigned vector = policy == REG_NP || policy == CODE_NP ? 11 :
                (policy >= REG_TYPE && policy <= CODE_LIMIT) || policy == CODE_BACKING || policy == REG_BACKING ||
                policy == REVALIDATE || policy == CODE_WRAP ? 13 : 14;
            unsigned error = policy == REG_TYPE || policy == REG_NP ? f.buffer & ~3u :
                policy == CODE_TYPE || policy == CODE_NP ? target & ~3u : vector != 14 ? 0 :
                (code_fault ? 4u : 6u) | (policy == CODE_SUPERVISOR || policy == REG_READONLY || policy == REG_SUPERVISOR);
            REG_CHECK(memory[0x8010] == vector && dos_mem_read16(vm, 0x8020) == error &&
                      dos_mem_read16(vm, 0x8024) == 15 && dos_mem_read16(vm, 0x8028) == before.cs &&
                      dos_mem_read16(vm, 0x802C) == before.sp && dos_mem_read16(vm, 0x8030) == before.ss);
            if (width32) REG_CHECK(dos_mem_read32(vm, 0x802C) == before.esp);
            if (vector == 14) REG_CHECK(cpu.cr2 == (code_fault ? 0x40002020u : linear + 4u));
        }
        bool intact = true;
        for (unsigned i = 0; i < sizeof(dpmi_rm_regs_t); i++)
            if (memory[i < 4 ? 0x24FFC + i : 0x2A000 + i - 4u] != 0xA5 ||
                memory[i < 4 ? 0x2CFFC + i : 0x2E000 + i - 4u] != 0x5A) intact = false;
        REG_CHECK(intact && memory[0x24FFB] == 0x39 && memory[0x25000] == 0x39 && memory[0x29FFF] == 0x39 && memory[0x2A02E] == 0x39);
        REG_CHECK(!(dos_mem_read32(vm, first) & 0x60u) && !(dos_mem_read32(vm, second) & 0x60u) && !(dos_mem_read32(vm, 0x12008) & 0x60u));
        if (resource_error) {
            bool unchanged = true;
            for (unsigned i = 0; i < DPMI_MAX_DESCRIPTORS; i++) if (states[i] != vm->dpmi.descriptor_state[i]) unchanged = false;
            REG_CHECK(unchanged && vm->dpmi.sel_exception_stack == old_stack && vm->dpmi.sel_host_code == old_code && vm->dpmi.next_free_index == next_free);
        }
        if (policy == NESTED_REG) REG_CHECK(vm->dpmi.callbacks[0].pm_sel == f.code && vm->dpmi.callbacks[0].rm_regs_sel == f.data &&
                                           dos_mem_read16(vm, 0x8034) == DPMI_ENTRY_SEG && dos_mem_read16(vm, 0x8036) == DPMI_CALLBACK_BASE_OFF);
        if (cancelled) {
            cpu.ax = 0x0303; cpu.eip = 15;
            dos_int31_dpmi(vm);
            REG_CHECK(!(cpu.flags & FLAG_CF) && vm->dpmi.callbacks[0].active && dos_mem_read32(vm, 0x8000) == 1);
        }
        if (completed || cancelled) {
            for (unsigned i = 0; i < DPMI_MAX_CALLBACKS; i++) if (vm->dpmi.callbacks[i].active) {
                uint16_t index = dpmi_sel_to_index(vm->dpmi.callbacks[i].rm_stack_sel);
                uint32_t stub = dos_linear(vm->dpmi.callbacks[i].rm_seg, vm->dpmi.callbacks[i].rm_off);
                cpu.ax = 0x0304; cpu.cx = vm->dpmi.callbacks[i].rm_seg; cpu.dx = vm->dpmi.callbacks[i].rm_off;
                dos_int31_dpmi(vm);
                REG_CHECK(!(cpu.flags & FLAG_CF) && !vm->dpmi.callbacks[i].active && vm->dpmi.descriptor_state[index] == DPMI_DESC_FREE &&
                          memory[stub] == 0xF4 && memory[stub + 1] == 0xF4 && memory[stub + 2] == 0xF4 && memory[stub + 3] == 0xF4);
            }
        }
    }
    serial_puts("[DPMI-CALLBACK-REGISTER] checks="); serial_putdec(checks);
    serial_puts(" recovered="); serial_putdec(recovered);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef REG_CHECK
    dos_host_free_pages(memory, memory_pages); dos_host_free_pages(vm, vm_pages);
    return failures;
}

static int dpmi_callback_recovery_selftest(void)
{
    const unsigned memory_pages = 512;
    unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (vm) dos_host_free_pages(vm, vm_pages);
        if (memory) dos_host_free_pages(memory, memory_pages);
        return 1;
    }
    dpmi_zero(vm, vm_pages * 4096u);
    cpu8086_state_t cpu;
    vm->cpu = &cpu; vm->mem = memory; vm->total_mem_size = memory_pages * 4096u;
    unsigned checks = 0, recovered = 0;
    int failures = 0;
#define CALLBACK_RECOVERY_CHECK(condition) do { \
    if (!(condition)) { \
        if (failures < 24) { serial_puts("[DPMI-CALLBACK-RECOVERY] failed check="); serial_putdec(checks); \
            serial_puts(" policy/width/stack/high/int="); serial_putdec(policy); serial_puts("/"); \
            serial_putdec(width32); serial_putdec(stack32); serial_putdec(high); serial_putdec(interpreted); \
            serial_puts("\n"); } \
        failures++; \
    } \
    checks++; \
} while (0)
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned interpreted = 0; interpreted < 2; interpreted++)
    for (unsigned policy = 0; policy < 19; policy++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        vm->step_limit = policy == 12 ? 128 : 4096;
        unsigned width = width32 ? 4u : 2u;
        uint32_t linear = high ? 0x3FFFFC : 0x6FFC;
        uint32_t first = high ? 0x11FFC : 0x11018, second = high ? 0x13000 : 0x1101C;
        if (high) dos_mem_write32(vm, 0x10004, 0x13007);
        dos_mem_write32(vm, first, 0x24005); dos_mem_write32(vm, second, 0x26005);
        dpmi_descriptor_t *bd = &vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)];
        dpmi_desc_set_base(bd, linear);
        bd->access = 0xF0;
        dpmi_desc_set_limit(&vm->dpmi.ldt[dpmi_sel_to_index(f.data)], UINT32_MAX);
        vm->dpmi.ldt[dpmi_sel_to_index(f.stack)].flags_lim = stack32 ? DESC_32BIT : 0;
        uint16_t host_cs = dpmi_get_host_code_selector(vm);
        dpmi_get_exception_stack_selector(vm);
        dpmi_rm_regs_t regs = { .eax = 0x12345678, .ebx = 0x87654321, .ecx = 0xA5A51234,
            .edx = 0x5A5A5678, .esi = 0x11223344, .edi = 0x55667788, .ebp = 0xDEADBEEF,
            .flags = FLAGS_FIXED | FLAG_CF | FLAG_IF | FLAG_DF, .cs = 0x2345, .ip = 0x100,
            .ss = 0x3456, .sp = 0x8000, .ds = 0, .es = 0xFFFF, .fs = 0x1234, .gs = 0 };
        dpmi_rm_regs_t alternate = regs;
        alternate.eax = 0x0BADCAFE;
        dpmi_saved_state_t nested = { 0x32535044u, 0xABCD, 0x6789, 0, 0, 0x10, 0xBCDEF018 };
        for (unsigned i = 0; i < sizeof(regs); i++) {
            memory[i < 4 ? 0x24FFC + i : 0x26000 + i - 4u] = ((uint8_t *)&regs)[i];
            memory[i < 4 ? 0x2CFFC + i : 0x2E000 + i - 4u] = ((uint8_t *)&alternate)[i];
            memory[0x9200 + i] = ((uint8_t *)&alternate)[i];
        }
        for (unsigned i = 0; i < sizeof(nested); i++) memory[0x9060 + i] = ((uint8_t *)&nested)[i];
        memory[0x24FFB] = memory[0x25000] = memory[0x25FFF] = memory[0x2602E] = 0x39;
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cs = host_cs; cpu.ds = f.data; cpu.es = f.buffer; cpu.ss = f.stack;
        cpu.esp = stack32 ? 0x8F00 : 0xBEEF8F00;
        cpu.eip = DPMI_CALLBACK_RETURN_OFF + (interpreted ? 0u : 2u);
        cpu.eax = 0xA5A5BC00; cpu.ebx = 0xABCDEF01; cpu.ecx = 0x11223344;
        cpu.edx = 0x55667788; cpu.esi = 0x98765432; cpu.edi = width32 ? 0 : 0x12340000;
        cpu.ebp = 0xCAFEBABE; cpu.eflags = FLAGS_FIXED | FLAG_CF | FLAG_DF | FLAG_IF;
        cpu8086_sync_cs(&cpu); cpu8086_sync_data(&cpu);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        vm->dpmi.callback_depth = 2;
        vm->dpmi.callback_slots[0] = 1;
        vm->dpmi.callback_slots[1] = 0;
        vm->dpmi.callbacks[0] = (dpmi_callback_t){ .active = true, .rm_seg = DPMI_ENTRY_SEG,
            .rm_off = DPMI_CALLBACK_BASE_OFF, .rm_regs_sel = f.data, .rm_regs_off = 0x9200 };
        vm->dpmi.callbacks[1].active = true;
        dpmi_stack_t caller = { 0x4444, 0x5555 }, original = { 0x1234, 0x5678 };
        dpmi_paging_t caller_paging = { 0x10, 0xDEFA0018 }, real_paging = { 0x10, 0xABCDE018 };
        vm->dpmi.callback_real_stacks[1] = caller;
        vm->dpmi.callback_real_paging[1] = caller_paging;
        vm->dpmi.callback_virtual_interrupts[1] = true;
        vm->dpmi.real_mode_stack = original;
        vm->dpmi.real_mode_paging = real_paging;
        vm->dpmi.virtual_interrupts_enabled = false;
        if (policy == 9) {
            cpu8086_state_t saved = cpu;
            cpu.ds = f.code; cpu.esi = 0x700; cpu.es = f.data; cpu.edi = 0x9080; cpu.ax = 0x0303;
            dos_int31_dpmi(vm);
            CALLBACK_RECOVERY_CHECK(!(cpu.flags & FLAG_CF) && vm->dpmi.callbacks[2].active);
            dpmi_rm_regs_t call = { .cs = cpu.cx, .ip = cpu.dx, .flags = FLAGS_FIXED };
            dpmi_write_rm_regs(vm, 0x9000, &call);
            cpu = saved;
        }
        memory[0x20080] = 0xF4;
        uint8_t *out;
#define CB_BYTE(value) (*out++ = (uint8_t)(value))
#define CB_WORD(value) do { uint16_t x_ = (value); CB_BYTE(x_); CB_BYTE(x_ >> 8); } while (0)
#define CB_DWORD(value) do { uint32_t x_ = (value); for (unsigned b_ = 0; b_ < 4; b_++) CB_BYTE(x_ >> (b_ * 8u)); } while (0)
#define CB_OP32() do { if (!width32) CB_BYTE(0x66); } while (0)
#define CB_OP16() do { if (width32) CB_BYTE(0x66); } while (0)
#define CB_ADDR32() do { if (!width32) CB_BYTE(0x67); } while (0)
        for (unsigned vector = 11; vector <= 14; vector++) {
            unsigned off = 0x100 + (vector - 11u) * 0x40;
            if (policy != 13) {
                vm->dpmi.exception_vectors[vector].sel = f.code;
                vm->dpmi.exception_vectors[vector].off = off;
            }
            out = memory + 0x20000 + off;
            CB_ADDR32(); CB_BYTE(0xC6); CB_BYTE(0x05); CB_DWORD(0x8010); CB_BYTE(vector);
            CB_BYTE(0xE9);
            if (width32) { CB_DWORD(0x20300 - (uint32_t)(out - memory) - 4u); }
            else { CB_WORD(0x20300 - (uint32_t)(out - memory) - 2u); }
        }
        vm->dpmi.exception_vectors[3].sel = f.code;
        vm->dpmi.exception_vectors[3].off = 0x600;
        out = memory + 0x20300;
        CB_OP32(); CB_BYTE(0x60); CB_OP32(); CB_BYTE(0x89); CB_BYTE(0xE5);
        if (!stack32) { CB_OP32(); CB_BYTE(0x81); CB_BYTE(0xE5); CB_DWORD(0xFFFF); }
        CB_OP32(); CB_ADDR32(); CB_BYTE(0xFF); CB_BYTE(0x05); CB_DWORD(0x8000);
        const unsigned fields[] = { 2, 3, 4, 6, 7 };
        for (unsigned i = 0; i < 5; i++) {
            CB_BYTE(0x36); CB_ADDR32(); CB_BYTE(0x8B); CB_BYTE(0x45); CB_BYTE(32u + fields[i] * width);
            CB_ADDR32(); CB_BYTE(0xA3); CB_DWORD(0x8020 + i * 4u);
        }
        if (policy == 8) CB_BYTE(0xCC);
        if (policy == 11) { CB_OP16(); CB_BYTE(0xB8); CB_WORD(0x4C2A); CB_BYTE(0xCD); CB_BYTE(0x21); }
        if (policy == 12) { CB_BYTE(0xEB); CB_BYTE(0xFE); }
        uint8_t *skip = NULL, *begin = NULL;
        if (policy == 7) {
            CB_OP32(); CB_ADDR32(); CB_BYTE(0x83); CB_BYTE(0x3D); CB_DWORD(0x8000); CB_BYTE(1);
            CB_BYTE(0x74); skip = out++; begin = out;
        }
        CB_OP32(); CB_ADDR32(); CB_BYTE(0xC7); CB_BYTE(0x05); CB_DWORD(second);
        CB_DWORD(policy == 16 ? 0x2E005 : 0x26005);
        if (policy == 16) {
            CB_OP32(); CB_ADDR32(); CB_BYTE(0xC7); CB_BYTE(0x05); CB_DWORD(first); CB_DWORD(0x2C005);
        }
        if (policy >= 4 && policy <= 6) {
            CB_OP16(); CB_BYTE(0xBB); CB_WORD(f.buffer);
            CB_OP16(); CB_BYTE(0xB8); CB_WORD(policy == 6 ? 8 : 9);
            CB_OP16(); CB_BYTE(0xB9); CB_WORD(policy == 6 ? 0 : width32 ? 0x40F0 : 0xF0);
            if (policy == 6) { CB_OP16(); CB_BYTE(0xBA); CB_WORD(0xFFFF); }
            CB_BYTE(0xCD); CB_BYTE(0x31);
        }
        if (skip) *skip = (uint8_t)(out - begin);
        if (policy == 9) {
            CB_OP16(); CB_BYTE(0xB8); CB_WORD(0x0304);
            CB_OP16(); CB_BYTE(0xB9); CB_WORD(DPMI_ENTRY_SEG);
            CB_OP16(); CB_BYTE(0xBA); CB_WORD(DPMI_CALLBACK_BASE_OFF);
            CB_BYTE(0xCD); CB_BYTE(0x31);
            CB_OP16(); CB_ADDR32(); CB_BYTE(0xA3); CB_DWORD(0x8012);
            CB_BYTE(0x06); CB_OP16(); CB_BYTE(0xB8); CB_WORD(f.data); CB_BYTE(0x8E); CB_BYTE(0xC0);
            CB_OP32(); CB_BYTE(0xBF); CB_DWORD(0x9000);
            CB_OP16(); CB_BYTE(0xBB); CB_WORD(0); CB_OP16(); CB_BYTE(0xB9); CB_WORD(0);
            CB_OP16(); CB_BYTE(0xB8); CB_WORD(0x0301); CB_BYTE(0xCD); CB_BYTE(0x31); CB_BYTE(0x07);
        }
        if (policy == 14) {
            CB_BYTE(0x36); CB_OP32(); CB_ADDR32(); CB_BYTE(0xC7); CB_BYTE(0x45); CB_BYTE(28); CB_DWORD(0x0BAD0000);
            CB_BYTE(0x36); CB_OP32(); CB_ADDR32(); CB_BYTE(0xC7); CB_BYTE(0x45); CB_BYTE(0); CB_DWORD(0x9200);
            CB_OP16(); CB_BYTE(0xB8); CB_WORD(f.data); CB_BYTE(0x8E); CB_BYTE(0xC0);
        }
        if (policy == 15) {
            CB_OP32(); CB_ADDR32(); CB_BYTE(0xC7); CB_BYTE(0x05); CB_DWORD(0x26018); CB_DWORD(0x76543210);
        }
        if (policy == 17) {
            CB_BYTE(0x06); CB_OP16(); CB_BYTE(0xB8); CB_WORD(f.data); CB_BYTE(0x8E); CB_BYTE(0xC0);
            CB_OP32(); CB_BYTE(0xBF); CB_DWORD(0x9060); CB_BYTE(0xB0); CB_BYTE(1);
            CB_BYTE(0x9A);
            if (width32) { CB_DWORD(DPMI_SAVE_STATE_OFF); } else { CB_WORD(DPMI_SAVE_STATE_OFF); }
            CB_WORD(host_cs); CB_BYTE(0x07);
        }
        if (policy == 10) {
            CB_BYTE(0x36); CB_ADDR32(); CB_BYTE(0xC7); CB_BYTE(0x45); CB_BYTE(32u + 3u * width);
            if (width32) { CB_DWORD(0x80); } else { CB_WORD(0x80); }
            CB_BYTE(0x36); CB_ADDR32(); CB_BYTE(0xC7); CB_BYTE(0x45); CB_BYTE(32u + 4u * width);
            if (width32) { CB_DWORD(f.code); } else { CB_WORD(f.code); }
        }
        CB_OP32(); CB_BYTE(0x61); CB_BYTE(0xCB);
        CALLBACK_RECOVERY_CHECK(out < memory + 0x20600);
        out = memory + 0x20600;
        CB_OP32(); CB_ADDR32(); CB_BYTE(0xFF); CB_BYTE(0x05); CB_DWORD(0x8004); CB_BYTE(0xCB);
        out = memory + 0x20700;
        CB_OP16(); CB_ADDR32(); CB_BYTE(0x8B); CB_BYTE(0x06);
        CB_BYTE(0x26); CB_OP16(); CB_ADDR32(); CB_BYTE(0x89); CB_BYTE(0x47); CB_BYTE(42);
        CB_OP16(); CB_ADDR32(); CB_BYTE(0x8B); CB_BYTE(0x46); CB_BYTE(2);
        CB_BYTE(0x26); CB_OP16(); CB_ADDR32(); CB_BYTE(0x89); CB_BYTE(0x47); CB_BYTE(44);
        CB_BYTE(0x26); CB_OP16(); CB_ADDR32(); CB_BYTE(0x83); CB_BYTE(0x47); CB_BYTE(46); CB_BYTE(4);
        CB_OP16(); CB_BYTE(0xB8); CB_WORD(f.data); CB_BYTE(0x8E); CB_BYTE(0xD8);
        CB_OP32(); CB_ADDR32(); CB_BYTE(0xFF); CB_BYTE(0x05); CB_DWORD(0x8008); CB_BYTE(0xCF);
#undef CB_ADDR32
#undef CB_OP16
#undef CB_OP32
#undef CB_DWORD
#undef CB_WORD
#undef CB_BYTE
        if (policy && (policy < 4 || policy > 6))
            dos_mem_write32(vm, second, policy == 2 ? 0x26005 : policy == 3 ? 0x26001 :
                                        policy == 18 ? vm->total_mem_size | 5u : 0x26004);
        if (policy == 4) bd->access = 0xF8;
        if (policy == 5) bd->access &= ~DESC_PRESENT;
        if (policy == 6) dpmi_desc_set_limit(bd, sizeof(regs) - 2u);
        cpu8086_state_t before = cpu;
        dpmi_service_result_t result = DPMI_SERVICE_COMPLETE;
        if (interpreted) (void)cpu8086_run_one(vm); else result = dpmi_callback_return(vm, false);
        bool cancelled = policy == 10, stopped = policy == 11 || policy == 12 || policy == 13;
        bool completed = !cancelled && !stopped, faulted = policy != 0 && policy != 2;
        if (!interpreted) CALLBACK_RECOVERY_CHECK(result == (completed ? DPMI_SERVICE_COMPLETE : DPMI_SERVICE_INTERRUPTED));
        CALLBACK_RECOVERY_CHECK(!vm->dpmi.host_wait && !vm->interpreter_stop_signal &&
                                 !vm->software_int_frame_bytes && !vm->native_dispatch_depth);
        CALLBACK_RECOVERY_CHECK(dos_mem_read32(vm, 0x8000) == (policy == 13 ? 0u : policy == 7 ? 2u : faulted) &&
                                 dos_mem_read32(vm, 0x8004) == (policy == 8));
        if (stopped) {
            CALLBACK_RECOVERY_CHECK(!cpu.running && (policy == 12 ? vm->step_limit_reached :
                                                      cpu.exit_code == (policy == 11 ? 42 : -1)));
        } else {
            CALLBACK_RECOVERY_CHECK(cpu.running && !vm->dpmi.exception_depth &&
                                     vm->dpmi.callbacks[0].active && vm->dpmi.callbacks[1].active);
            CALLBACK_RECOVERY_CHECK(vm->dpmi.callback_depth == (completed ? 1u : 2u) &&
                                     vm->dpmi.callback_slots[0] == 1 && vm->dpmi.callback_slots[1] == 0 &&
                                     vm->dpmi.virtual_interrupts_enabled == completed);
            CALLBACK_RECOVERY_CHECK(vm->dpmi.real_mode_stack.ss == (completed ? caller.ss : original.ss) &&
                                     vm->dpmi.real_mode_stack.esp == (completed ? caller.esp : original.esp) &&
                                     vm->dpmi.real_mode_paging.cr3 == (completed ? caller_paging.cr3 : real_paging.cr3));
            if (faulted && completed) recovered++;
        }
        if (faulted && !stopped) {
            unsigned vector = policy == 5 ? 11 : policy == 4 || policy == 6 || policy == 18 ? 13 : 14;
            unsigned error = policy == 4 || policy == 5 ? f.buffer & ~3u : policy == 6 || policy == 18 ? 0 :
                             4u | (policy == 3);
            CALLBACK_RECOVERY_CHECK(memory[0x8010] == vector && dos_mem_read16(vm, 0x8020) == error &&
                                     dos_mem_read16(vm, 0x8024) == DPMI_CALLBACK_RETURN_OFF + 2u &&
                                     dos_mem_read16(vm, 0x8028) == host_cs && dos_mem_read16(vm, 0x802C) == before.sp &&
                                     dos_mem_read16(vm, 0x8030) == before.ss && (vector != 14 || cpu.cr2 == linear + 4u));
            if (width32) CALLBACK_RECOVERY_CHECK(dos_mem_read32(vm, 0x802C) == before.esp);
        }
        dpmi_rm_regs_t expected = policy == 16 ? alternate : regs;
        if (policy == 15) expected.eax = 0x76543210;
        bool intact = true;
        for (unsigned i = 0; i < sizeof(regs); i++) {
            uint8_t old = ((uint8_t *)(policy == 15 ? &expected : &regs))[i];
            if (memory[i < 4 ? 0x24FFC + i : 0x26000 + i - 4u] != old ||
                memory[i < 4 ? 0x2CFFC + i : 0x2E000 + i - 4u] != ((uint8_t *)&alternate)[i] ||
                memory[0x9200 + i] != ((uint8_t *)&alternate)[i]) intact = false;
        }
        CALLBACK_RECOVERY_CHECK(intact && memory[0x24FFB] == 0x39 && memory[0x25000] == 0x39 &&
                                 memory[0x25FFF] == 0x39 && memory[0x2602E] == 0x39);
        CALLBACK_RECOVERY_CHECK((dos_mem_read32(vm, first) & 0x60u) == (completed ? 0x20u : 0u) &&
                                 (dos_mem_read32(vm, second) & 0x60u) == (completed ? 0x20u : 0u));
        if (policy == 9) CALLBACK_RECOVERY_CHECK(dos_mem_read32(vm, 0x8008) == 1 &&
                                                 dos_mem_read16(vm, 0x8012) == 0x8024 && vm->dpmi.callbacks[2].active);
        if (cancelled) {
            CALLBACK_RECOVERY_CHECK(cpu.protected_mode && cpu.cs == f.code && cpu.eip == 0x80 &&
                                     cpu.ss == before.ss && cpu.esp == before.esp && cpu.edi == before.edi &&
                                     cpu.cr0 == before.cr0 && cpu.cr3 == before.cr3);
            cpu.cs = host_cs; cpu.eip = DPMI_CALLBACK_RETURN_OFF + (interpreted ? 0u : 2u);
            cpu8086_sync_cs(&cpu);
            if (interpreted) (void)cpu8086_run_one(vm);
            else CALLBACK_RECOVERY_CHECK(dpmi_callback_return(vm, false) == DPMI_SERVICE_COMPLETE);
            CALLBACK_RECOVERY_CHECK(cpu.running && vm->dpmi.callback_depth == 1 && dos_mem_read32(vm, 0x8000) == 1);
        }
        if (completed || cancelled) {
            CALLBACK_RECOVERY_CHECK(!cpu.protected_mode && cpu.cs == expected.cs && cpu.eip == expected.ip &&
                                     cpu.ss == expected.ss && cpu.esp == expected.sp && cpu.flags == expected.flags);
            CALLBACK_RECOVERY_CHECK(cpu.eax == expected.eax && cpu.ebx == expected.ebx && cpu.ecx == expected.ecx &&
                                     cpu.edx == expected.edx && cpu.esi == expected.esi && cpu.edi == expected.edi &&
                                     cpu.ebp == expected.ebp && cpu.ds == expected.ds && cpu.es == expected.es &&
                                     cpu.fs == expected.fs && cpu.gs == expected.gs);
            CALLBACK_RECOVERY_CHECK(cpu.cr0 == real_paging.cr0 && cpu.cr3 == real_paging.cr3 &&
                                     vm->dpmi.suspended_paging.cr0 == before.cr0 && vm->dpmi.suspended_paging.cr3 == before.cr3);
        }
    }
    serial_puts("[DPMI-CALLBACK-RECOVERY] checks="); serial_putdec(checks);
    serial_puts(" recovered="); serial_putdec(recovered);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef CALLBACK_RECOVERY_CHECK
    dos_host_free_pages(memory, memory_pages); dos_host_free_pages(vm, vm_pages);
    return failures;
}

static int dpmi_state_recovery_selftest(void)
{
    const unsigned memory_pages = 512;
    unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (vm) dos_host_free_pages(vm, vm_pages);
        if (memory) dos_host_free_pages(memory, memory_pages);
        return 1;
    }
    dpmi_zero(vm, vm_pages * 4096u);
    cpu8086_state_t cpu;
    vm->cpu = &cpu; vm->mem = memory; vm->total_mem_size = memory_pages * 4096u;
    unsigned checks = 0, recovered = 0;
    int failures = 0;
#define STATE_RECOVERY_CHECK(condition) do { \
    if (!(condition)) { \
        if (failures < 24) { serial_puts("[DPMI-STATE-RECOVERY] failed check="); serial_putdec(checks); \
            serial_puts(" operation/policy="); serial_putdec(restore); serial_puts("/"); serial_putdec(policy); \
            serial_puts(" width/stack/high/int="); serial_putdec(width32); serial_putdec(stack32); \
            serial_putdec(high); serial_putdec(interpreted); serial_puts("\n"); } \
        failures++; \
    } \
    checks++; \
} while (0)
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned interpreted = 0; interpreted < 2; interpreted++)
    for (unsigned restore = 0; restore < 2; restore++)
    for (unsigned policy = 0; policy < 18; policy++) {
        if (policy == 16 && !restore) continue;
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        vm->step_limit = policy == 12 ? 128 : 2048;
        unsigned width = width32 ? 4u : 2u;
        uint32_t linear = high ? 0x3FFFFC : 0x6FFC;
        uint32_t first = high ? 0x11FFC : 0x11018, second = high ? 0x13000 : 0x1101C;
        if (high) dos_mem_write32(vm, 0x10004, 0x13007);
        dos_mem_write32(vm, first, 0x24007); dos_mem_write32(vm, second, 0x26007);
        dpmi_descriptor_t *bd = &vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)];
        dpmi_desc_set_base(bd, linear);
        dpmi_desc_set_limit(&vm->dpmi.ldt[dpmi_sel_to_index(f.data)], UINT32_MAX);
        vm->dpmi.ldt[dpmi_sel_to_index(f.stack)].flags_lim = stack32 ? DESC_32BIT : 0;
        uint16_t host_cs = dpmi_get_host_code_selector(vm);
        dpmi_get_exception_stack_selector(vm);
        dpmi_saved_state_t state = { 0x32535044u, 0x5678, 0x2345, 0, 0, 0x10, 0xABCDE018 };
        dpmi_saved_state_t nested = { 0x32535044u, 0xABCD, 0x6789, 0, 0, 0x10, 0xBCDEF018 };
        dpmi_stack_t initial = restore ? (dpmi_stack_t){ 0x4444, 0x5555 } : (dpmi_stack_t){ state.ss, state.esp };
        dpmi_paging_t initial_paging = { 0x10, restore ? 0x70018 : state.cr3 };
        vm->dpmi.real_mode_stack = initial; vm->dpmi.real_mode_paging = initial_paging;
        for (unsigned i = 0; i < sizeof(state); i++) {
            memory[i < 4 ? 0x24FFC + i : 0x26000 + i - 4u] = restore ? ((uint8_t *)&state)[i] : 0xA5;
            memory[i < 4 ? 0x2CFFC + i : 0x2E000 + i - 4u] = restore ? ((uint8_t *)&state)[i] : 0xA5;
            memory[0x9020 + i] = ((uint8_t *)&nested)[i];
        }
        memory[0x24FFB] = memory[0x25000] = memory[0x25FFF] = memory[0x26010] = 0x39;
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cs = host_cs; cpu.ds = f.data; cpu.es = f.buffer; cpu.ss = f.stack;
        cpu.esp = stack32 ? 0x8F00 : 0xBEEF8F00;
        cpu.eip = DPMI_SAVE_STATE_OFF + (interpreted ? 0u : 2u);
        cpu.eax = 0xA5A5BC00u | restore; cpu.ebx = 0xABCDEF01; cpu.ecx = 0x11223344;
        cpu.edx = 0x55667788; cpu.esi = 0x98765432; cpu.edi = width32 ? 0 : 0x12340000;
        cpu.ebp = 0xCAFEBABE; cpu.eflags = FLAGS_FIXED | FLAG_CF | FLAG_DF | FLAG_IF;
        cpu8086_sync_cs(&cpu); cpu8086_sync_data(&cpu);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        vm->dpmi.virtual_interrupts_enabled = true;
        if (width32) { dos_mem_write32(vm, 0x8F00, 15); dos_mem_write32(vm, 0x8F04, f.code); }
        else { dos_mem_write16(vm, 0x8F00, 15); dos_mem_write16(vm, 0x8F02, f.code); }
        memory[0x2000F] = memory[0x20080] = 0xF4;
        uint8_t *out;
#define ST_BYTE(value) (*out++ = (uint8_t)(value))
#define ST_WORD(value) do { uint16_t x_ = (value); ST_BYTE(x_); ST_BYTE(x_ >> 8); } while (0)
#define ST_DWORD(value) do { uint32_t x_ = (value); for (unsigned b_ = 0; b_ < 4; b_++) ST_BYTE(x_ >> (b_ * 8u)); } while (0)
#define ST_OP32() do { if (!width32) ST_BYTE(0x66); } while (0)
#define ST_OP16() do { if (width32) ST_BYTE(0x66); } while (0)
#define ST_ADDR32() do { if (!width32) ST_BYTE(0x67); } while (0)
#define ST_FAR_STATE() do { ST_BYTE(0x9A); if (width32) { ST_DWORD(DPMI_SAVE_STATE_OFF); } \
                           else { ST_WORD(DPMI_SAVE_STATE_OFF); } ST_WORD(host_cs); } while (0)
        for (unsigned vector = 11; vector <= 14; vector++) {
            unsigned off = 0x100 + (vector - 11u) * 0x40;
            if (policy != 13) {
                vm->dpmi.exception_vectors[vector].sel = f.code;
                vm->dpmi.exception_vectors[vector].off = off;
            }
            out = memory + 0x20000 + off;
            ST_ADDR32(); ST_BYTE(0xC6); ST_BYTE(0x05); ST_DWORD(0x8010); ST_BYTE(vector);
            ST_BYTE(0xE9);
            if (width32) { ST_DWORD(0x20300 - (uint32_t)(out - memory) - 4u); }
            else { ST_WORD(0x20300 - (uint32_t)(out - memory) - 2u); }
        }
        vm->dpmi.exception_vectors[3].sel = f.code;
        vm->dpmi.exception_vectors[3].off = 0x600;
        out = memory + 0x20300;
        ST_OP32(); ST_BYTE(0x60); ST_OP32(); ST_BYTE(0x89); ST_BYTE(0xE5);
        ST_OP32(); ST_ADDR32(); ST_BYTE(0xFF); ST_BYTE(0x05); ST_DWORD(0x8000);
        const unsigned fields[] = { 2, 3, 4, 6, 7 };
        for (unsigned i = 0; i < 5; i++) {
            ST_BYTE(0x36); ST_ADDR32(); ST_BYTE(0x8B); ST_BYTE(0x45); ST_BYTE(32u + fields[i] * width);
            ST_ADDR32(); ST_BYTE(0xA3); ST_DWORD(0x8020 + i * 4u);
        }
        if (policy == 8) ST_BYTE(0xCC);
        if (policy == 11) { ST_OP16(); ST_BYTE(0xB8); ST_WORD(0x4C2A); ST_BYTE(0xCD); ST_BYTE(0x21); }
        if (policy == 12) { ST_BYTE(0xEB); ST_BYTE(0xFE); }
        uint8_t *skip = NULL, *begin = NULL;
        if (policy == 7) {
            ST_OP32(); ST_ADDR32(); ST_BYTE(0x83); ST_BYTE(0x3D); ST_DWORD(0x8000); ST_BYTE(1);
            ST_BYTE(0x74); skip = out++; begin = out;
        }
        ST_OP32(); ST_ADDR32(); ST_BYTE(0xC7); ST_BYTE(0x05); ST_DWORD(second);
        ST_DWORD(policy == 17 ? 0x2E007 : 0x26007);
        if (policy == 17) {
            ST_OP32(); ST_ADDR32(); ST_BYTE(0xC7); ST_BYTE(0x05); ST_DWORD(first); ST_DWORD(0x2C007);
        }
        if (policy >= 4 && policy <= 6) {
            ST_OP16(); ST_BYTE(0xBB); ST_WORD(f.buffer);
            ST_OP16(); ST_BYTE(0xB8); ST_WORD(policy == 6 ? 8 : 9);
            ST_OP16(); ST_BYTE(0xB9); ST_WORD(policy == 6 ? 0 : width32 ? 0x40F2 : 0xF2);
            if (policy == 6) { ST_OP16(); ST_BYTE(0xBA); ST_WORD(0xFFFF); }
            ST_BYTE(0xCD); ST_BYTE(0x31);
        }
        if (skip) *skip = (uint8_t)(out - begin);
        if (policy == 9) {
            ST_BYTE(0x06); ST_OP16(); ST_BYTE(0xB8); ST_WORD(f.data); ST_BYTE(0x8E); ST_BYTE(0xC0);
            ST_OP32(); ST_BYTE(0xBF); ST_DWORD(0x9000); ST_BYTE(0xB0); ST_BYTE(0); ST_FAR_STATE();
            ST_OP32(); ST_BYTE(0xBF); ST_DWORD(0x9020); ST_BYTE(0xB0); ST_BYTE(1); ST_FAR_STATE();
            ST_BYTE(0x07);
        }
        if (policy == 14) {
            ST_BYTE(0x36); ST_OP32(); ST_ADDR32(); ST_BYTE(0xC7); ST_BYTE(0x45); ST_BYTE(28);
            ST_DWORD(0x0BAD0000u | !restore);
            ST_BYTE(0x36); ST_OP32(); ST_ADDR32(); ST_BYTE(0xC7); ST_BYTE(0x45); ST_BYTE(0); ST_DWORD(0x9000);
            ST_OP16(); ST_BYTE(0xB8); ST_WORD(f.data); ST_BYTE(0x8E); ST_BYTE(0xC0);
        }
        if (policy == 15 || policy == 16) {
            ST_OP32(); ST_ADDR32(); ST_BYTE(0xC7); ST_BYTE(0x05);
            ST_DWORD(policy == 15 ? 0x26000 : 0x24FFC); ST_DWORD(policy == 15 ? 0x7654 : 0);
        }
        if (policy == 10) {
            ST_BYTE(0x36); ST_ADDR32(); ST_BYTE(0xC7); ST_BYTE(0x45); ST_BYTE(32u + 3u * width);
            if (width32) { ST_DWORD(0x80); } else { ST_WORD(0x80); }
            ST_BYTE(0x36); ST_ADDR32(); ST_BYTE(0xC7); ST_BYTE(0x45); ST_BYTE(32u + 4u * width);
            if (width32) { ST_DWORD(f.code); } else { ST_WORD(f.code); }
        }
        ST_BYTE(0x36); ST_ADDR32(); ST_BYTE(0x81); ST_BYTE(0x65); ST_BYTE(32u + 5u * width);
        if (width32) { ST_DWORD(~(uint32_t)FLAG_DF); } else { ST_WORD(~FLAG_DF); }
        ST_OP32(); ST_BYTE(0x61); ST_BYTE(0xCB);
        STATE_RECOVERY_CHECK(out < memory + 0x20600);
        out = memory + 0x20600;
        ST_OP32(); ST_ADDR32(); ST_BYTE(0xFF); ST_BYTE(0x05); ST_DWORD(0x8004); ST_BYTE(0xCB);
#undef ST_FAR_STATE
#undef ST_ADDR32
#undef ST_OP16
#undef ST_OP32
#undef ST_DWORD
#undef ST_WORD
#undef ST_BYTE
        if (policy && (policy < 4 || policy > 6))
            dos_mem_write32(vm, second, policy == 2 ? 0x26005 : policy == 3 ? 0x26003 : 0x26006);
        if (policy == 4) bd->access = 0xF8;
        if (policy == 5) bd->access &= ~DESC_PRESENT;
        if (policy == 6) dpmi_desc_set_limit(bd, sizeof(state) - 2u);
        cpu8086_state_t before = cpu;
        dpmi_service_result_t result = DPMI_SERVICE_COMPLETE;
        if (interpreted) (void)cpu8086_run_one(vm); else result = dpmi_save_restore_state(vm);
        bool cancelled = policy == 10, stopped = policy == 11 || policy == 12 || policy == 13;
        bool invalid = policy == 16, completed = !cancelled && !stopped && !invalid;
        bool faulted = policy != 0 && !(restore && policy == 2);
        if (!interpreted) STATE_RECOVERY_CHECK(result == (invalid ? DPMI_SERVICE_INVALID :
                                               cancelled || stopped ? DPMI_SERVICE_INTERRUPTED : DPMI_SERVICE_COMPLETE));
        STATE_RECOVERY_CHECK(!vm->dpmi.host_wait && !vm->interpreter_stop_signal &&
                             !vm->software_int_frame_bytes && !vm->native_dispatch_depth);
        STATE_RECOVERY_CHECK(dos_mem_read32(vm, 0x8000) == (policy == 13 ? 0u : policy == 7 ? 2u : faulted) &&
                             dos_mem_read32(vm, 0x8004) == (policy == 8));
        if (stopped || (invalid && interpreted)) {
            STATE_RECOVERY_CHECK(!cpu.running && (policy == 12 ? vm->step_limit_reached :
                                                  cpu.exit_code == (policy == 11 ? 42 : -1)));
        } else {
            STATE_RECOVERY_CHECK(cpu.running && !vm->dpmi.exception_depth && cpu.cs == (cancelled ? f.code : host_cs) &&
                                 cpu.eip == (cancelled ? 0x80u : DPMI_SAVE_STATE_OFF + 2u) &&
                                 cpu.ss == before.ss && cpu.esp == before.esp);
            STATE_RECOVERY_CHECK(cpu.eax == (policy == 14 ? 0x0BAD0000u | !restore : before.eax) &&
                                 cpu.edi == (policy == 14 ? 0x9000u : before.edi) && cpu.es == (policy == 14 ? f.data : before.es) &&
                                 cpu.ebx == before.ebx && cpu.ecx == before.ecx && cpu.edx == before.edx &&
                                 cpu.esi == before.esi && cpu.ebp == before.ebp &&
                                 (cpu.flags & FLAG_CF) && !!(cpu.flags & FLAG_DF) == !faulted);
            if (faulted && completed) recovered++;
        }
        if (faulted && !stopped) {
            unsigned vector = policy == 5 ? 11 : policy == 4 || policy == 6 ? 13 : 14;
            unsigned error = policy == 4 || policy == 5 ? f.buffer & ~3u : policy == 6 ? 0 :
                             4u | (restore ? 0u : 2u) | (policy == 2 || policy == 3);
            STATE_RECOVERY_CHECK(memory[0x8010] == vector && dos_mem_read16(vm, 0x8020) == error &&
                                 dos_mem_read16(vm, 0x8024) == DPMI_SAVE_STATE_OFF + 2u &&
                                 dos_mem_read16(vm, 0x8028) == host_cs && dos_mem_read16(vm, 0x802C) == before.sp &&
                                 dos_mem_read16(vm, 0x8030) == before.ss && (vector != 14 || cpu.cr2 == linear + 4u));
        }
        dpmi_saved_state_t expected = state;
        if (restore && policy == 15) expected.esp = 0x7654;
        if (invalid) expected.signature = 0;
        bool intact = true;
        for (unsigned i = 0; i < sizeof(state); i++) {
            uint8_t old = restore ? ((uint8_t *)&expected)[i] : completed && policy != 17 ? ((uint8_t *)&state)[i] : 0xA5;
            uint8_t alternate = restore ? ((uint8_t *)&state)[i] : completed && policy == 17 ? ((uint8_t *)&state)[i] : 0xA5;
            if (memory[i < 4 ? 0x24FFC + i : 0x26000 + i - 4u] != old ||
                memory[i < 4 ? 0x2CFFC + i : 0x2E000 + i - 4u] != alternate) intact = false;
        }
        STATE_RECOVERY_CHECK(intact && memory[0x24FFB] == 0x39 && memory[0x25000] == 0x39 &&
                             memory[0x25FFF] == 0x39 && memory[0x26010] == 0x39);
        uint32_t access_bits = completed || invalid ? restore ? 0x20u : 0x60u : 0u;
        STATE_RECOVERY_CHECK((dos_mem_read32(vm, first) & 0x60u) == access_bits &&
                             (dos_mem_read32(vm, second) & 0x60u) == access_bits);
        dpmi_stack_t wanted = restore && completed ? (dpmi_stack_t){ expected.ss, expected.esp } :
                               policy == 9 ? (dpmi_stack_t){ nested.ss, nested.esp } : initial;
        dpmi_paging_t wanted_paging = restore && completed ? (dpmi_paging_t){ expected.cr0, expected.cr3 } :
                                      policy == 9 ? (dpmi_paging_t){ nested.cr0, nested.cr3 } : initial_paging;
        STATE_RECOVERY_CHECK(vm->dpmi.real_mode_stack.ss == wanted.ss && vm->dpmi.real_mode_stack.esp == wanted.esp &&
                             vm->dpmi.real_mode_paging.cr0 == wanted_paging.cr0 && vm->dpmi.real_mode_paging.cr3 == wanted_paging.cr3);
        STATE_RECOVERY_CHECK(cpu.cr0 == before.cr0 && cpu.cr3 == before.cr3 &&
                             vm->dpmi.suspended_paging.cr0 == before.cr0 && vm->dpmi.suspended_paging.cr3 == before.cr3);
        if (policy == 9) {
            dpmi_saved_state_t entry = { 0x32535044u, initial.esp, initial.ss, 0, 0, initial_paging.cr0, initial_paging.cr3 };
            for (unsigned i = 0; i < sizeof(entry); i++) STATE_RECOVERY_CHECK(memory[0x9000 + i] == ((uint8_t *)&entry)[i]);
        }
        if (completed) {
            (void)cpu8086_run_one(vm);
            STATE_RECOVERY_CHECK(cpu.running && cpu.cs == f.code && cpu.eip == 15 && cpu.ss == before.ss &&
                                 cpu.esp == before.esp + 2u * width && !vm->software_int_frame_bytes);
        }
    }
    serial_puts("[DPMI-STATE-RECOVERY] checks="); serial_putdec(checks);
    serial_puts(" recovered="); serial_putdec(recovered);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef STATE_RECOVERY_CHECK
    dos_host_free_pages(memory, memory_pages); dos_host_free_pages(vm, vm_pages);
    return failures;
}

static int dpmi_service_buffer_selftest(void)
{
    const unsigned memory_pages = 512;
    unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (vm) dos_host_free_pages(vm, vm_pages);
        if (memory) dos_host_free_pages(memory, memory_pages);
        return 1;
    }
    dpmi_zero(vm, vm_pages * 4096u);
    cpu8086_state_t cpu;
    vm->cpu = &cpu; vm->mem = memory; vm->total_mem_size = memory_pages * 4096u;
    unsigned checks = 0, recovered = 0;
    int failures = 0;
#define SERVICE_CHECK(condition) do { \
    if (!(condition)) { \
        if (failures < 24) { serial_puts("[DPMI-SERVICE-BUFFER] failed check="); serial_putdec(checks); serial_puts("\n"); } \
        failures++; \
    } \
    checks++; \
} while (0)
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned write = 0; write < 2; write++)
    for (unsigned policy = 0; policy < 13; policy++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        dpmi_descriptor_t *d = &vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)];
        uint32_t linear = high ? 0x3FFFF0 : 0x6FF0;
        uint32_t first = high ? 0x11FFC : 0x11018, second = high ? 0x13000 : 0x1101C;
        if (high) dos_mem_write32(vm, 0x10004, 0x13007);
        dos_mem_write32(vm, first, 0x24007); dos_mem_write32(vm, second, 0x26007);
        dpmi_desc_set_base(d, linear);
        uint16_t selector = f.buffer;
        uint8_t expected_vector = 13;
        uint32_t expected_error = 0, expected_linear = 0;
        bool allowed = policy == 0 || (policy == 2 && !write) || (policy == 9 && !write);
        if (policy >= 1 && policy <= 3) {
            dos_mem_write32(vm, second, 0x26007u & ~(1u << (policy - 1u)));
            expected_vector = 14; expected_error = (write ? 2u : 0u) | 4u | (policy != 1);
            expected_linear = linear + 16u;
        }
        switch (policy) {
        case 4: selector = 0; break;
        case 5: d->access &= ~DESC_PRESENT; expected_vector = 11; expected_error = selector & ~3u; break;
        case 6: d->access = 0xF8; expected_error = selector & ~3u; break;
        case 7: dpmi_desc_set_limit(d, 48); break;
        case 8: d->access |= 4u; break;
        case 9: d->access &= ~DESC_WRITABLE; expected_error = selector & ~3u; break;
        case 10: dos_mem_write32(vm, second, vm->total_mem_size | 7u); break;
        case 11: dpmi_desc_set_base(d, 0xFFFFFFF0); break;
        case 12: selector = 0xFFF7; expected_error = selector & ~3u; break;
        }
        dpmi_client_buffer_t buffer = { .size = 0xA5A5 };
        dpmi_buffer_fault_t fault;
        SERVICE_CHECK(dpmi_client_buffer_probe(vm, selector, 0, 50, write, &buffer, &fault) == allowed);
        SERVICE_CHECK(allowed || (fault.vector == expected_vector && fault.error == expected_error &&
                      (fault.vector != 14 || fault.linear == expected_linear) && buffer.size == 0xA5A5));
        SERVICE_CHECK(dos_mem_read32(vm, first) == 0x24007);
    }

    /* Actual host calls and decoded INTs, with a retained real-mode side
     * effect, nested CPU/service exceptions and edited-return cancellation. */
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned interpreted = 0; interpreted < 2; interpreted++)
    for (unsigned kind = 0; kind < 3; kind++)
    for (unsigned policy = 0; policy < 17; policy++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        vm->software_int_frame_bytes = 0;
        vm->software_int_return_flags = 0;
        vm->step_limit = policy == 13 ? 128 : 2048;
        unsigned width = width32 ? 4u : 2u;
        uint32_t first = high ? 0x11FFC : 0x11018, second = high ? 0x13000 : 0x1101C;
        uint32_t linear = high ? 0x3FFFF0 : 0x6FF0;
        if (high) dos_mem_write32(vm, 0x10004, 0x13007);
        dos_mem_write32(vm, first, 0x24007); dos_mem_write32(vm, second, 0x26007);
        dpmi_desc_set_base(&vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)], linear);
        dpmi_desc_set_limit(&vm->dpmi.ldt[dpmi_sel_to_index(f.data)], UINT32_MAX);
        vm->dpmi.ldt[dpmi_sel_to_index(f.stack)].flags_lim = stack32 ? DESC_32BIT : 0;
        uint16_t alternate = dpmi_alloc_descriptor(&vm->dpmi);
        dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(alternate)], 0, 0xFFFF, 0xF2,
                          stack32 ? DESC_32BIT : 0);
        uint16_t alternate_cs = dpmi_alloc_descriptor(&vm->dpmi);
        vm->dpmi.ldt[dpmi_sel_to_index(alternate_cs)] = vm->dpmi.ldt[dpmi_sel_to_index(f.code)];
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cs = f.code; cpu.ds = f.data; cpu.es = f.buffer; cpu.ss = f.stack;
        cpu.esp = stack32 ? 0x8F00 : 0xBEEF8F00;
        cpu.eip = interpreted ? 0 : 15;
        cpu.eax = 0xA5A50300u + kind; cpu.ebx = 0xABCD0060;
        cpu.ecx = 0; cpu.edx = 0x11223344; cpu.esi = 0x55667788; cpu.edi = 0;
        cpu.ebp = 0xCAFEBABE; cpu.eflags = FLAGS_FIXED | FLAG_CF | FLAG_IF | FLAG_DF;
        cpu8086_sync_cs(&cpu); cpu8086_sync_data(&cpu);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        vm->dpmi.virtual_interrupts_enabled = true;
        for (unsigned v = 11; v <= 14; v++) {
            vm->dpmi.exception_vectors[v].sel = f.code;
            vm->dpmi.exception_vectors[v].off = 0x100;
        }
        vm->dpmi.exception_vectors[3].sel = f.code;
        vm->dpmi.exception_vectors[3].off = 0x400;
        dpmi_get_exception_stack_selector(vm);
        for (unsigned i = 0; i < 13; i++) memory[0x20000 + i] = 0x3E;
        memory[0x2000D] = 0xCD; memory[0x2000E] = 0x31;
        memory[0x2000F] = memory[0x20080] = 0xF4;
        dpmi_rm_regs_t input = { .eax = 0x87654321, .flags = FLAGS_FIXED | FLAG_IF,
                                 .cs = 0x3000 };
        for (unsigned i = 0; i < sizeof(input); i++)
            memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u] = ((uint8_t *)&input)[i];
        dos_mem_write16(vm, 0x60 * 4u, 0); dos_mem_write16(vm, 0x60 * 4u + 2, 0x3000);
        dpmi_rm_regs_t inner = input; inner.ip = 0x100;
        for (unsigned i = 0; i < sizeof(inner); i++) memory[0x9000 + i] = ((uint8_t *)&inner)[i];
        uint8_t *out = memory + 0x30000;
#define SERVICE_BYTE(value) (*out++ = (uint8_t)(value))
#define SERVICE_WORD(value) do { uint16_t x_ = (value); SERVICE_BYTE(x_); SERVICE_BYTE(x_ >> 8); } while (0)
#define SERVICE_DWORD(value) do { uint32_t x_ = (value); for (unsigned b_ = 0; b_ < 4; b_++) SERVICE_BYTE(x_ >> (b_ * 8u)); } while (0)
#define SERVICE_OP32() do { if (!width32) SERVICE_BYTE(0x66); } while (0)
#define SERVICE_ADDR32() do { if (!width32) SERVICE_BYTE(0x67); } while (0)
        SERVICE_BYTE(0x66); SERVICE_BYTE(0xFF); SERVICE_BYTE(0x06); SERVICE_WORD(0x8008);
        if (policy >= 3 && policy <= 13) {
            SERVICE_BYTE(0xB8); SERVICE_WORD(second >> 4);
            SERVICE_BYTE(0x8E); SERVICE_BYTE(0xD8);
            SERVICE_BYTE(0x66); SERVICE_BYTE(0xC7); SERVICE_BYTE(0x06); SERVICE_WORD(second & 15u);
            SERVICE_DWORD(policy == 4 ? 0x26005 : policy == 5 ? 0x26003 : 0x26006);
        }
        SERVICE_BYTE(0x66); SERVICE_BYTE(0xB8); SERVICE_DWORD(0x12345678);
        SERVICE_BYTE(kind == 1 ? 0xCB : 0xCF);
        out = memory + 0x30100;
        SERVICE_BYTE(0x66); SERVICE_BYTE(0xFF); SERVICE_BYTE(0x06); SERVICE_WORD(0x800C);
        SERVICE_BYTE(0xCB);
        out = memory + 0x20100;
        SERVICE_OP32(); SERVICE_BYTE(0x60); /* PUSHAD retains all client GPRs. */
        SERVICE_OP32(); SERVICE_BYTE(0x89); SERVICE_BYTE(0xE5);
        SERVICE_OP32(); SERVICE_ADDR32(); SERVICE_BYTE(0xFF); SERVICE_BYTE(0x05); SERVICE_DWORD(0x8000);
        const unsigned fields[] = { 2, 3, 6, 7 };
        for (unsigned i = 0; i < 4; i++) {
            SERVICE_BYTE(0x36); SERVICE_ADDR32(); SERVICE_BYTE(0x8B); SERVICE_BYTE(0x45);
            SERVICE_BYTE(32u + fields[i] * width);
            SERVICE_ADDR32(); SERVICE_BYTE(0xA3); SERVICE_DWORD(0x8020 + i * 4u);
        }
        if (policy == 7) SERVICE_BYTE(0xCC);
        if (policy == 8) {
            SERVICE_OP32(); SERVICE_ADDR32(); SERVICE_BYTE(0x83); SERVICE_BYTE(0x3D);
            SERVICE_DWORD(0x8000); SERVICE_BYTE(1);
            SERVICE_BYTE(0x75); uint8_t *skip = out++; uint8_t *begin = out;
            if (width32) SERVICE_BYTE(0x66);
            SERVICE_BYTE(0xB8); SERVICE_WORD(f.data); SERVICE_BYTE(0x8E); SERVICE_BYTE(0xC0);
            SERVICE_OP32(); SERVICE_BYTE(0xBF); SERVICE_DWORD(0x9000);
            SERVICE_OP32(); SERVICE_BYTE(0x31); SERVICE_BYTE(0xC9);
            if (width32) SERVICE_BYTE(0x66);
            SERVICE_BYTE(0xB8); SERVICE_WORD(0x0301); SERVICE_BYTE(0xCD); SERVICE_BYTE(0x31);
            if (width32) SERVICE_BYTE(0x66);
            SERVICE_BYTE(0xB8); SERVICE_WORD(f.buffer); SERVICE_BYTE(0x8E); SERVICE_BYTE(0xC0);
            *skip = (uint8_t)(out - begin);
        }
        if (policy == 12) {
            if (width32) SERVICE_BYTE(0x66);
            SERVICE_BYTE(0xB8); SERVICE_WORD(0x4C2A); SERVICE_BYTE(0xCD); SERVICE_BYTE(0x21);
        }
        if (policy == 13) { SERVICE_BYTE(0xEB); SERVICE_BYTE(0xFE); }
        uint8_t *skip_repair = NULL, *repair_begin = NULL;
        if (policy == 6) {
            SERVICE_OP32(); SERVICE_ADDR32(); SERVICE_BYTE(0x83); SERVICE_BYTE(0x3D);
            SERVICE_DWORD(0x8000); SERVICE_BYTE(1); SERVICE_BYTE(0x74);
            skip_repair = out++; repair_begin = out;
        }
        SERVICE_OP32(); SERVICE_ADDR32(); SERVICE_BYTE(0xC7); SERVICE_BYTE(0x05);
        SERVICE_DWORD(second); SERVICE_DWORD(0x26007);
        SERVICE_OP32(); SERVICE_ADDR32(); SERVICE_BYTE(0xC7); SERVICE_BYTE(0x05);
        SERVICE_DWORD(0x11024); SERVICE_DWORD(0x9007);
        if (policy >= 14) {
            if (width32) SERVICE_BYTE(0x66);
            SERVICE_BYTE(0xBB); SERVICE_WORD(f.buffer);
            if (width32) SERVICE_BYTE(0x66);
            SERVICE_BYTE(0xB8); SERVICE_WORD(policy == 16 ? 8 : 9);
            if (width32) SERVICE_BYTE(0x66);
            SERVICE_BYTE(0xB9); SERVICE_WORD(policy == 16 ? 0 : width32 ? 0x40F2 : 0x00F2);
            if (policy == 16) {
                if (width32) SERVICE_BYTE(0x66);
                SERVICE_BYTE(0xBA); SERVICE_WORD(0xFFFF);
            }
            SERVICE_BYTE(0xCD); SERVICE_BYTE(0x31);
        }
        if (skip_repair) *skip_repair = (uint8_t)(out - repair_begin);
        if (policy >= 9 && policy <= 11) {
            SERVICE_BYTE(0x36); SERVICE_ADDR32(); SERVICE_BYTE(0xC7); SERVICE_BYTE(0x45);
            SERVICE_BYTE(32u + (policy == 9 ? 3u : policy == 10 ? 4u : 6u) * width);
            uint32_t value = policy == 9 ? 0x80 : policy == 10 ? alternate_cs : 0x7000;
            if (width32) { SERVICE_DWORD(value); } else { SERVICE_WORD(value); }
            if (policy == 11) {
                SERVICE_BYTE(0x36); SERVICE_ADDR32(); SERVICE_BYTE(0xC7); SERVICE_BYTE(0x45);
                SERVICE_BYTE(32u + 7u * width);
                if (width32) { SERVICE_DWORD(alternate); } else { SERVICE_WORD(alternate); }
            }
        }
        SERVICE_OP32(); SERVICE_BYTE(0x61); SERVICE_BYTE(0xCB);
        SERVICE_CHECK(out < memory + 0x20400);
        out = memory + 0x20400;
        SERVICE_OP32(); SERVICE_ADDR32(); SERVICE_BYTE(0xFF); SERVICE_BYTE(0x05); SERVICE_DWORD(0x8004);
        SERVICE_BYTE(0xCB);
#undef SERVICE_ADDR32
#undef SERVICE_OP32
#undef SERVICE_DWORD
#undef SERVICE_WORD
#undef SERVICE_BYTE
        if (policy == 1) dos_mem_write32(vm, second, 0x26006);
        if (policy == 2) dos_mem_write32(vm, second, 0x26005);
        if (policy == 8) dos_mem_write32(vm, 0x11024, 0x9005);
        if (policy == 14) vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)].access &= ~DESC_WRITABLE;
        if (policy == 15) vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)].access &= ~DESC_PRESENT;
        if (policy == 16) dpmi_desc_set_limit(&vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)], 48);
        cpu8086_state_t before = cpu;
        if (interpreted) (void)cpu8086_run_one(vm); else dos_int31_dpmi(vm);
        bool cancelled = policy == 9 || policy == 10, stopped = policy == 12 || policy == 13;
        uint32_t calls = dos_mem_read32(vm, 0x8008), faults = dos_mem_read32(vm, 0x8000);
        SERVICE_CHECK(calls == 1 && faults == (policy == 0 ? 0u : policy == 6 || policy == 8 ? 2u : 1u));
        SERVICE_CHECK(!vm->dpmi.host_wait && !vm->interpreter_stop_signal && !vm->interpreter_stop_active &&
                      !vm->software_int_frame_bytes && !vm->native_dispatch_depth);
        if (stopped) {
            SERVICE_CHECK(!cpu.running && (policy == 12 ? cpu.exit_code == 42 : vm->step_limit_reached));
        } else {
            SERVICE_CHECK(cpu.running && !vm->dpmi.exception_depth && cpu.protected_mode &&
                          cpu.cs == (policy == 10 ? alternate_cs : before.cs) && cpu.eip == (policy == 9 ? 0x80 : 15));
            uint32_t esp = policy == 11 ? (width32 ? 0x7000 : (before.esp & 0xFFFF0000u) | 0x7000) : before.esp;
            SERVICE_CHECK(cpu.ss == (policy == 11 ? alternate : before.ss) && cpu.esp == esp &&
                          cpu.eax == before.eax && cpu.ecx == before.ecx && cpu.ebx == before.ebx &&
                          cpu.edx == before.edx && cpu.esi == before.esi && cpu.edi == before.edi && cpu.ebp == before.ebp);
            SERVICE_CHECK((cpu.flags & FLAG_CF) == (cancelled ? FLAG_CF : 0) &&
                          (cpu.flags & FLAG_DF) && vm->dpmi.virtual_interrupts_enabled);
            if (!cancelled && policy != 0) recovered++;
        }
        dpmi_rm_regs_t output;
        for (unsigned i = 0; i < sizeof(output); i++)
            ((uint8_t *)&output)[i] = memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u];
        SERVICE_CHECK(output.eax == (cancelled || stopped ? input.eax : 0x12345678) &&
                      dos_mem_read32(vm, 0x8004) == (policy == 7) && dos_mem_read32(vm, 0x800C) == (policy == 8));
        if (policy && !stopped && policy != 8) {
            uint16_t error = policy == 2 || policy == 4 || policy == 5 ? 7 : policy == 14 || policy == 15 ? f.buffer & ~3u :
                             policy == 16 ? 0 : policy == 1 ? 4 : 6;
            SERVICE_CHECK(dos_mem_read16(vm, 0x8020) == error && dos_mem_read16(vm, 0x8024) == 15 &&
                          dos_mem_read16(vm, 0x8028) == (policy == 11 ? 0x8F00 : before.sp) &&
                          dos_mem_read16(vm, 0x802C) == before.ss);
        }
    }
    /* The private return itself can cross the periodic IRQ polling boundary.
     * Resume the host first; the latched IRQ must remain deliverable afterward. */
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        vm->dpmi.ldt[dpmi_sel_to_index(f.stack)].flags_lim = stack32 ? DESC_32BIT : 0;
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cs = f.code; cpu.ds = f.data; cpu.es = f.buffer; cpu.ss = f.stack;
        cpu.esp = stack32 ? 0x8F00 : 0xBEEF8F00; cpu.eip = 15;
        cpu8086_sync_cs(&cpu); cpu8086_sync_data(&cpu);
        vm->dpmi.virtual_interrupts_enabled = true;
        vm->dpmi.exception_vectors[13].sel = f.code;
        vm->dpmi.exception_vectors[13].off = 0x80;
        vm->dpmi.pm_vectors[8].sel = f.code;
        vm->dpmi.pm_vectors[8].off = 0x90;
        memory[0x20080] = 0xCB; memory[0x20090] = 0xF4;
        dpmi_host_wait_t wait = { .psp = vm->current_psp };
        SERVICE_CHECK(cpu_deliver_exception(vm, 13, 15, 0, true));
        vm->dpmi.host_wait = &wait;
        SERVICE_CHECK(cpu8086_run_one(vm) && cpu.cs == vm->dpmi.sel_host_code &&
                      cpu.eip == DPMI_EXCEPTION_RETURN_OFF);
        cpu.insn_count = 16383;
        vm->timer_irq_pending = true;
        SERVICE_CHECK(cpu8086_run_until_signal(vm, &wait.returned));
        SERVICE_CHECK(wait.returned && cpu.running && cpu.cs == f.code && cpu.eip == 15 &&
                      !vm->dpmi.exception_depth && vm->timer_irq_pending && cpu.insn_count == 16384);
        vm->dpmi.host_wait = NULL;
        SERVICE_CHECK(cpu8086_service_interrupts(vm) && !vm->timer_irq_pending &&
                      cpu.cs == f.code && cpu.eip == 0x90);
    }
    dos_host_free_pages(memory, memory_pages); dos_host_free_pages(vm, vm_pages);
    serial_puts("[DPMI-SERVICE-BUFFER] checks="); serial_putdec(checks);
    serial_puts(" recovered="); serial_putdec(recovered);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef SERVICE_CHECK
    return failures;
}

static int dpmi_host_entry_frame_selftest(void)
{
    const unsigned memory_pages = 512;
    unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (vm) dos_host_free_pages(vm, vm_pages);
        if (memory) dos_host_free_pages(memory, memory_pages);
        return 1;
    }
    dpmi_zero(vm, vm_pages * 4096u);
    cpu8086_state_t cpu;
    vm->cpu = &cpu;
    vm->mem = memory;
    vm->total_mem_size = memory_pages * 4096u;
    unsigned checks = 0;
    int failures = 0;
#define ENTRY_FRAME_CHECK(condition) do { \
    if (!(condition)) { \
        if (failures < 24) { serial_puts("[DPMI-ENTRY-FRAME] failed check="); serial_putdec(checks); serial_puts("\n"); } \
        failures++; \
    } \
    checks++; \
} while (0)
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned kind = 0; kind < 4; kind++)
    for (unsigned policy = 0; policy < 14; policy++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        unsigned width = width32 ? 4u : 2u, size = 3u * width + (kind == 3 ? 24u : 0u);
        uint32_t linear = high ? 0x7FFFFF : 0x8FFFF;
        uint32_t top = stack32 ? 0x9000 : 0xCAFE9000;
        uint32_t first = high ? 0x13FFC : 0x1123C, second = high ? 0x15000 : first + 4u;
        uint32_t pde = high ? 0x10008 : 0x10000;
        if (high) {
            dos_mem_write32(vm, 0x10004, 0x13007);
            dos_mem_write32(vm, 0x10008, 0x15007);
        }
        dos_mem_write32(vm, first, 0x40007);
        dos_mem_write32(vm, second, 0x60007);
        dpmi_descriptor_t *sd = &vm->dpmi.ldt[dpmi_sel_to_index(f.stack)];
        dpmi_build_desc(sd, linear - (0x9000u - size), 0xFFFF, 0xF2, stack32 ? DESC_32BIT : 0);
        uint16_t host = dpmi_get_host_code_selector(vm);
        vm->dpmi.exception_depth = 1;
        vm->dpmi.suspended_stack = (dpmi_stack_t){ f.stack, top };
        vm->dpmi.virtual_interrupts_enabled = true;
        for (unsigned i = 0; i < 15; i++)
            dos_mem_write16(vm, 0x8000 + i * 2u, i == 2 ? FLAGS_FIXED | FLAG_IF | FLAG_CF : 0x5100u + i);
        for (unsigned i = 0; i < 50; i++) memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u] = 0xA5;
        for (unsigned i = 0; i < size; i++) memory[i ? 0x60000 + i - 1u : 0x40FFF] = 0xA5;
        memory[0x40FFE] = memory[0x60000 + size - 1u] = 0xC7;
        if (!kind) {
            uint16_t alias = dpmi_alloc_descriptor(&vm->dpmi);
            dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(alias)], 0, 0xFFFF, 0xF2, 0);
            vm->dpmi.descriptor_state[dpmi_sel_to_index(alias)] = DPMI_DESC_HOST;
            vm->dpmi.callbacks[0] = (dpmi_callback_t){
                .rm_seg = DPMI_ENTRY_SEG, .rm_off = DPMI_CALLBACK_BASE_OFF,
                .pm_sel = f.code, .rm_regs_sel = f.buffer, .rm_regs_off = 0xFF0,
                .rm_stack_sel = alias, .active = true
            };
            cpu8086_load_real_cs(&cpu, DPMI_ENTRY_SEG);
            cpu.eip = DPMI_CALLBACK_BASE_OFF + 2;
            memory[dos_linear(cpu.cs, cpu.ip)] = 0;
            memory[0x20000] = 0xCF;
        } else {
            unsigned vector = kind == 1 ? 0x1C : kind == 2 ? 0x23 : 0x24;
            vm->dpmi.pm_vectors[vector].sel = f.code;
            vm->dpmi.pm_vectors[vector].off = 0;
            memory[0x20000] = 0xB0; memory[0x20001] = 1; memory[0x20002] = 0xCF;
        }
        if (policy == 13 && kind == 2) {
            cpu.protected_mode = cpu.pm_cs_loaded = true;
            cpu.cpl = 3; cpu.cs = f.code; cpu.ss = f.stack; cpu.esp = top;
            cpu8086_sync_cs(&cpu);
            cpu8086_sync_data(&cpu);
            dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        }
        switch (policy) {
        case 1: dos_mem_write32(vm, second, 0x60006); break;
        case 2: dos_mem_write32(vm, second, 0x60005); break;
        case 3: dos_mem_write32(vm, second, 0x60003); break;
        case 4: dos_mem_write32(vm, pde, dos_mem_read32(vm, pde) & ~1u); break;
        case 5: dos_mem_write32(vm, pde, dos_mem_read32(vm, pde) & ~2u); break;
        case 6: dos_mem_write32(vm, pde, dos_mem_read32(vm, pde) & ~4u); break;
        case 7: dos_mem_write32(vm, second, vm->total_mem_size | 7u); break;
        case 8: dpmi_desc_set_limit(sd, 0x8FFEu); break;
        case 9: sd->access |= 4u; dpmi_desc_set_limit(sd, 0x9000u - size); break;
        case 10: sd->access |= 4u; dpmi_desc_set_limit(sd, 0x9000u - size - 1u); break;
        case 11: sd->access &= ~DESC_PRESENT; break;
        case 12: sd->access |= DESC_CODE; break;
        case 13: sd->access &= ~DESC_WRITABLE; break;
        }
        cpu8086_state_t saved = cpu;
        uint32_t first_pte = dos_mem_read32(vm, first), second_pte = dos_mem_read32(vm, second);
        uint32_t directory = dos_mem_read32(vm, pde);
        bool valid = policy == 0 || policy == 10 || (policy == 13 && kind == 2);
        bool buffer_fault = !kind && !high && policy >= 4 && policy <= 6;
        unsigned result = kind == 0 ? dpmi_callback_enter(vm) : kind == 1 ? dpmi_reflect_timer(vm)
                        : kind == 2 ? dpmi_control_break(vm) : dpmi_critical_error(vm);
        ENTRY_FRAME_CHECK(result == (buffer_fault ? DPMI_SERVICE_INTERRUPTED : valid ? 1u : kind == 3 ? 3u : 0u));
        uint32_t flags = !kind ? FLAGS_FIXED : kind == 2 ? (saved.eflags & ~(FLAG_CF | FLAG_TF)) | FLAGS_FIXED
                         : kind == 3 ? FLAGS_FIXED | FLAG_IF | FLAG_CF : saved.eflags;
        const uint32_t values[] = { !kind ? DPMI_CALLBACK_RETURN_OFF : DPMI_CONTROL_RETURN_OFF, host, flags };
        bool intact = memory[0x40FFE] == 0xC7 && memory[0x60000 + size - 1u] == 0xC7;
        for (unsigned i = 0; i < size; i++) {
            uint8_t value = i < 3u * width ? (uint8_t)(values[i / width] >> ((i % width) * 8u))
                : (uint8_t)((0x5103u + (i - 3u * width) / 2u) >> ((i & 1u) * 8u));
            if (memory[i ? 0x60000 + i - 1u : 0x40FFF] != (valid ? value : 0xA5)) intact = false;
        }
        ENTRY_FRAME_CHECK(intact);
        if (!valid) {
            if (buffer_fault) {
                saved.running = false;
                saved.exit_code = -1;
                saved.esp += 6u;
                saved.flags = FLAGS_FIXED | FLAG_IF | FLAG_CF;
                saved.cr2 = 0x6FF0;
            }
            intact = true;
            for (unsigned i = 0; i < sizeof(cpu); i++)
                if (((uint8_t *)&cpu)[i] != ((uint8_t *)&saved)[i]) intact = false;
            for (unsigned i = 0; i < 50; i++)
                if (memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u] != 0xA5) intact = false;
            ENTRY_FRAME_CHECK(intact && !vm->dpmi.callback_depth && !vm->dpmi.control_depth &&
                              vm->dpmi.exception_depth == 1 && vm->dpmi.virtual_interrupts_enabled);
            ENTRY_FRAME_CHECK(dos_mem_read32(vm, first) == first_pte &&
                              dos_mem_read32(vm, second) == second_pte && dos_mem_read32(vm, pde) == directory);
        } else {
            ENTRY_FRAME_CHECK((dos_mem_read32(vm, first) & 0x60u) == 0x60u &&
                              (dos_mem_read32(vm, second) & 0x60u) == 0x60u);
            if (!kind) {
                ENTRY_FRAME_CHECK(cpu.ss == f.stack && cpu.esp == top - size &&
                                  cpu_stack_addr32(&cpu) == (stack32 != 0) &&
                                  vm->dpmi.callback_depth == 1 && !vm->dpmi.virtual_interrupts_enabled);
                ENTRY_FRAME_CHECK(cpu8086_run_one(vm) && cpu.eip == DPMI_CALLBACK_RETURN_OFF);
                ENTRY_FRAME_CHECK(cpu8086_run_one(vm) && !cpu.protected_mode &&
                                  cpu.cs == 0x5101 && cpu.eip == 0x5100 && cpu.esp == 0x8006 &&
                                  !vm->dpmi.callback_depth && vm->dpmi.virtual_interrupts_enabled);
            } else {
                ENTRY_FRAME_CHECK(cpu.running && !vm->step_limit_reached && cpu.insn_count > saved.insn_count &&
                                  cpu.ss == saved.ss && cpu.esp == saved.esp &&
                                  cpu.protected_mode == saved.protected_mode && cpu.cr3 == saved.cr3 &&
                                  !vm->dpmi.control_depth && vm->dpmi.virtual_interrupts_enabled);
            }
        }
    }

    /* Capture both outputs before either can overwrite the other's PTEs.
     * The third case forms a cycle, so ordering the payloads is not enough. */
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned alias_kind = 0; alias_kind < 3; alias_kind++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        unsigned width = width32 ? 4u : 2u, size = width * 3u;
        uint32_t frame_linear = alias_kind ? 0x8F018 : 0x8FFFF;
        uint32_t regs_offset = alias_kind == 1 ? 0xFF0 : 0x23C;
        uint32_t first_physical = alias_kind ? 0x11018 : 0x40FFF;
        dos_mem_write32(vm, 0x1123C, alias_kind ? 0x11007 : 0x40007);
        dos_mem_write32(vm, 0x11240, 0x60007);
        if (alias_kind != 1) dos_mem_write32(vm, 0x11018, 0x11007);
        dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(f.stack)],
            frame_linear - (0x9000u - size), 0xFFFF, 0xF2, DESC_32BIT);
        vm->dpmi.exception_depth = 1;
        vm->dpmi.suspended_stack = (dpmi_stack_t){ f.stack, 0x9000 };
        uint16_t alias = dpmi_alloc_descriptor(&vm->dpmi);
        dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(alias)], 0, 0xFFFF, 0xF2, 0);
        vm->dpmi.descriptor_state[dpmi_sel_to_index(alias)] = DPMI_DESC_HOST;
        vm->dpmi.callbacks[0] = (dpmi_callback_t){
            .rm_seg = DPMI_ENTRY_SEG, .rm_off = DPMI_CALLBACK_BASE_OFF,
            .pm_sel = f.code, .rm_regs_sel = f.buffer, .rm_regs_off = regs_offset,
            .rm_stack_sel = alias, .active = true
        };
        cpu8086_load_real_cs(&cpu, DPMI_ENTRY_SEG);
        cpu.eip = DPMI_CALLBACK_BASE_OFF + 2;
        memory[dos_linear(cpu.cs, cpu.ip)] = 0;
        cpu.eax = 0x81828384; cpu.ebx = 0x91929394; cpu.ecx = 0xA1A2A3A4;
        cpu.edx = 0xB1B2B3B4; cpu.esi = 0xC1C2C3C4; cpu.edi = 0xD1D2D3D4; cpu.ebp = 0xE1E2E3E4;
        dos_mem_write16(vm, 0x8000, 0x1234); dos_mem_write16(vm, 0x8002, 0x2345);
        dos_mem_write16(vm, 0x8004, FLAGS_FIXED | FLAG_IF | FLAG_CF);
        dpmi_rm_regs_t expected = { .edi = cpu.edi, .esi = cpu.esi, .ebp = cpu.ebp,
            .ebx = cpu.ebx, .edx = cpu.edx, .ecx = cpu.ecx, .eax = cpu.eax,
            .flags = FLAGS_FIXED | FLAG_IF | FLAG_CF, .es = cpu.es, .ds = cpu.ds,
            .fs = cpu.fs, .gs = cpu.gs, .ip = 0x1234, .cs = 0x2345, .sp = 0x8006, .ss = 0 };
        ENTRY_FRAME_CHECK(dpmi_callback_enter(vm) && cpu.protected_mode && cpu.esp == 0x9000u - size);
        bool intact = true;
        for (unsigned i = 0; i < sizeof(expected); i++) {
            uint32_t at = alias_kind == 1 ? (i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u) : 0x1123C + i;
            if (memory[at] != ((uint8_t *)&expected)[i]) intact = false;
        }
        ENTRY_FRAME_CHECK(intact);
        const uint32_t fields[] = { DPMI_CALLBACK_RETURN_OFF, vm->dpmi.sel_host_code, FLAGS_FIXED };
        intact = true;
        for (unsigned i = 0; i < size; i++) {
            uint32_t at = alias_kind ? first_physical + i : i ? 0x60000 + i - 1u : first_physical;
            if (memory[at] != (uint8_t)(fields[i / width] >> ((i % width) * 8u))) intact = false;
        }
        ENTRY_FRAME_CHECK(intact && cpu.ss_cache.valid &&
                          dpmi_desc_get_base(&cpu.ss_cache.descriptor) == frame_linear - (0x9000u - size));
    }

    /* Callback input is a real stack, not a flat host pointer. Resolve it
     * in the real paging context before entering the protected context. */
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned policy = 0; policy < 7; policy++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        uint16_t alias = dpmi_alloc_descriptor(&vm->dpmi);
        dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(alias)], 0, 0xFFFF, 0xF2, 0);
        vm->dpmi.descriptor_state[dpmi_sel_to_index(alias)] = DPMI_DESC_HOST;
        vm->dpmi.callbacks[0] = (dpmi_callback_t){
            .rm_seg = DPMI_ENTRY_SEG, .rm_off = DPMI_CALLBACK_BASE_OFF,
            .pm_sel = f.code, .rm_regs_sel = f.buffer, .rm_regs_off = 0xFF0,
            .rm_stack_sel = alias, .active = true
        };
        cpu8086_load_real_cs(&cpu, DPMI_ENTRY_SEG);
        cpu.eip = DPMI_CALLBACK_BASE_OFF + 2;
        memory[dos_linear(cpu.cs, cpu.ip)] = 0;
        cpu8086_load_real_segment(&cpu, 2, 0x7000);
        if (stack32) cpu.ss_cache.descriptor.flags_lim |= DESC_32BIT;
        cpu.esp = stack32 ? 0x8FFE : 0xABCD8FFE;
        cpu.cr0 |= DOS_CR0_PG;
        dos_mem_write32(vm, 0x18000, 0x19007);
        dos_mem_write32(vm, 0x191E0, 0x90007);
        dos_mem_write32(vm, 0x191E4, 0x92007);
        dos_mem_write16(vm, 0x90FFE, 0x1234);
        dos_mem_write16(vm, 0x92000, 0x2345);
        dos_mem_write16(vm, 0x92002, FLAGS_FIXED | FLAG_IF);
        for (unsigned i = 0; i < 50; i++) memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u] = 0xA5;
        if (policy == 1) dos_mem_write32(vm, 0x191E4, 0x92006);
        if (policy == 2) dos_mem_write32(vm, 0x191E4, 0x92005);
        if (policy == 3) dpmi_desc_set_limit(&cpu.ss_cache.descriptor, 0x9002);
        if (policy == 4) cpu.ss_cache.valid = false;
        if (policy == 5) dos_mem_write32(vm, 0x191E4, vm->total_mem_size | 7u);
        if (policy == 6) dos_mem_write32(vm, 0x18000, 0x19006);
        cpu8086_state_t saved = cpu;
        bool valid = policy == 0 || policy == 2;
        ENTRY_FRAME_CHECK(dpmi_callback_enter(vm) == valid);
        bool intact = true;
        if (valid) {
            dpmi_rm_regs_t regs;
            for (unsigned i = 0; i < sizeof(regs); i++)
                ((uint8_t *)&regs)[i] = memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u];
            ENTRY_FRAME_CHECK(regs.ip == 0x1234 && regs.cs == 0x2345 && regs.sp == 0x9004 &&
                              regs.ss == 0x7000 && regs.flags == (FLAGS_FIXED | FLAG_IF) &&
                              vm->dpmi.real_mode_paging.cr3 == saved.cr3 && cpu.cr3 == 0x10018);
        } else {
            for (unsigned i = 0; i < sizeof(cpu); i++)
                if (((uint8_t *)&cpu)[i] != ((uint8_t *)&saved)[i]) intact = false;
            for (unsigned i = 0; i < 50; i++)
                if (memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u] != 0xA5) intact = false;
            ENTRY_FRAME_CHECK(intact && !vm->dpmi.callback_depth);
        }
    }
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned policy = 0; policy < 7; policy++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        unsigned size = width32 ? 12u : 6u;
        uint32_t linear = high ? 0x7FFFFF : 0x8FFFF;
        uint32_t first = high ? 0x13FFC : 0x1123C, second = high ? 0x15000 : first + 4u;
        if (high) {
            dos_mem_write32(vm, 0x10004, 0x13007);
            dos_mem_write32(vm, 0x10008, 0x15007);
        }
        dos_mem_write32(vm, first, 0x40005);
        dos_mem_write32(vm, second, 0x60005);
        dpmi_descriptor_t *sd = &vm->dpmi.ldt[dpmi_sel_to_index(f.stack)];
        dpmi_build_desc(sd, linear - 0x8000u, 0xFFFF, 0xF2, stack32 ? DESC_32BIT : 0);
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cpl = 3; cpu.cs = dpmi_get_host_code_selector(vm);
        cpu.eip = DPMI_CALLBACK_RETURN_OFF + 2;
        cpu.ss = f.stack; cpu.esp = stack32 ? 0x8000 : 0xCAFE8000;
        cpu.es = f.buffer; cpu.edi = width32 ? 0xFF0 : 0xABCD0FF0;
        cpu8086_sync_cs(&cpu);
        cpu8086_sync_data(&cpu);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        vm->dpmi.callback_depth = 1;
        vm->dpmi.callbacks[0].active = true;
        dpmi_rm_regs_t regs = { .eax = 0x12345678, .cs = 0x2345, .ip = 0x1234,
            .ss = 0x3456, .sp = 0x9876, .flags = FLAGS_FIXED | FLAG_IF };
        for (unsigned i = 0; i < sizeof(regs); i++)
            memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u] = ((uint8_t *)&regs)[i];
        if (policy == 1) dos_mem_write32(vm, second, 0x60004);
        if (policy == 2) dos_mem_write32(vm, second, 0x60001);
        if (policy == 3) dos_mem_write32(vm, second, vm->total_mem_size | 5u);
        if (policy == 4) dpmi_desc_set_limit(&cpu.ss_cache.descriptor, 0x8000u + size - 2u);
        if (policy == 5) cpu.ss_cache.descriptor.access &= ~DESC_WRITABLE;
        if (policy == 6) sd->access &= ~DESC_PRESENT;
        cpu8086_state_t saved = cpu;
        bool valid = policy == 0 || policy == 6;
        ENTRY_FRAME_CHECK(dpmi_callback_return(vm, true) ==
                          (valid ? DPMI_SERVICE_COMPLETE : DPMI_SERVICE_INVALID));
        if (valid) {
            ENTRY_FRAME_CHECK(!cpu.protected_mode && cpu.cs == regs.cs && cpu.eip == regs.ip &&
                              cpu.ss == regs.ss && cpu.esp == regs.sp && cpu.eax == regs.eax &&
                              !vm->dpmi.callback_depth);
        } else {
            bool intact = true;
            for (unsigned i = 0; i < sizeof(cpu); i++)
                if (((uint8_t *)&cpu)[i] != ((uint8_t *)&saved)[i]) intact = false;
            ENTRY_FRAME_CHECK(intact && vm->dpmi.callback_depth == 1);
        }
    }
    serial_puts("[DPMI-ENTRY-FRAME] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
#undef ENTRY_FRAME_CHECK
    dos_host_free_pages(memory, memory_pages);
    dos_host_free_pages(vm, vm_pages);
    return failures;
}

static bool dpmi_descriptor_unchanged(const dpmi_descriptor_t *left,
                                       const dpmi_descriptor_t *right);

static int dpmi_record_service_selftest(void)
{
    const unsigned memory_pages = 512;
    unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (vm) dos_host_free_pages(vm, vm_pages);
        if (memory) dos_host_free_pages(memory, memory_pages);
        return 1;
    }
    dpmi_zero(vm, vm_pages * 4096u);
    cpu8086_state_t cpu;
    vm->cpu = &cpu; vm->mem = memory; vm->total_mem_size = memory_pages * 4096u;
    unsigned checks = 0, recovered = 0;
    int failures = 0;
#define RECORD_SERVICE_CHECK(condition) do { \
    if (!(condition)) { \
        if (failures < 24) { serial_puts("[DPMI-RECORD-SERVICE] failed check="); serial_putdec(checks); \
            serial_puts(" kind/policy="); serial_putdec(kind); serial_puts("/"); serial_putdec(policy); \
            serial_puts(" width/stack/high/int="); serial_putdec(width32); serial_putdec(stack32); \
            serial_putdec(high); serial_putdec(interpreted); serial_puts("\n"); } \
        failures++; \
    } \
    checks++; \
} while (0)
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned interpreted = 0; interpreted < 2; interpreted++)
    for (unsigned kind = 0; kind < 3; kind++)
    for (unsigned policy = 0; policy < 20; policy++) {
        bool input = kind == 1;
        if ((input && policy == 2) || (kind == 2 && (policy == 13 || policy == 14 || policy == 18)) ||
            ((policy == 16 || policy == 19) && !input)) continue;
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        vm->step_limit = policy == 12 ? 128 : 2048;
        unsigned size = kind == 2 ? 48u : 8u, width = width32 ? 4u : 2u;
        uint32_t linear = high ? 0x3FFFFC : 0x6FFC;
        uint32_t first = high ? 0x11FFC : 0x11018, second = high ? 0x13000 : 0x1101C;
        if (high) dos_mem_write32(vm, 0x10004, 0x13007);
        dos_mem_write32(vm, first, 0x24007); dos_mem_write32(vm, second, 0x26007);
        dpmi_descriptor_t *bd = &vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)];
        dpmi_desc_set_base(bd, linear);
        dpmi_desc_set_limit(&vm->dpmi.ldt[dpmi_sel_to_index(f.data)], UINT32_MAX);
        vm->dpmi.ldt[dpmi_sel_to_index(f.stack)].flags_lim = stack32 ? DESC_32BIT : 0;
        uint16_t target_sel = dpmi_alloc_descriptor(&vm->dpmi), other_sel = dpmi_alloc_descriptor(&vm->dpmi);
        dpmi_descriptor_t *target = &vm->dpmi.ldt[dpmi_sel_to_index(target_sel)];
        dpmi_build_desc(target, 0x12340000, 0xFFFF, 0xF2, DESC_32BIT);
        dpmi_descriptor_t original = *target, incoming;
        dpmi_build_desc(&incoming, 0x56780000, 0x1FFFF, policy == 16 ? 0xB2 : 0xF2, DESC_32BIT);
        for (unsigned i = 0; i < size; i++)
            memory[i < 4 ? 0x24FFC + i : 0x26000 + i - 4u] = input ? ((uint8_t *)&incoming)[i] : 0xA5;
        memory[0x24FFB] = memory[0x25000] = memory[0x25FFF] = memory[0x26000 + size - 4u] = 0x39;
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cs = f.code; cpu.ds = f.data; cpu.es = f.buffer; cpu.ss = f.stack;
        cpu.esp = stack32 ? 0x8F00 : 0xBEEF8F00; cpu.eip = interpreted ? 0 : 15;
        const uint16_t functions[] = { 0x000B, 0x000C, 0x0500 };
        cpu.eax = 0xA5A50000u | functions[kind]; cpu.ebx = 0xABCD0000u | (policy == 18 ? 0 : target_sel);
        cpu.ecx = 0x11223344; cpu.edx = 0x55667788; cpu.esi = 0x98765432;
        cpu.edi = width32 ? 0 : 0x12340000; cpu.ebp = 0xCAFEBABE;
        cpu.eflags = FLAGS_FIXED | FLAG_CF | FLAG_DF | FLAG_IF;
        cpu8086_sync_cs(&cpu); cpu8086_sync_data(&cpu);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        vm->dpmi.virtual_interrupts_enabled = true;
        dpmi_get_exception_stack_selector(vm);
        for (unsigned i = 0; i < 13; i++) memory[0x20000 + i] = 0x3E;
        memory[0x2000D] = 0xCD; memory[0x2000E] = 0x31;
        memory[0x2000F] = memory[0x20080] = 0xF4;
        uint8_t *out;
#define RS_BYTE(value) (*out++ = (uint8_t)(value))
#define RS_WORD(value) do { uint16_t x_ = (value); RS_BYTE(x_); RS_BYTE(x_ >> 8); } while (0)
#define RS_DWORD(value) do { uint32_t x_ = (value); for (unsigned b_ = 0; b_ < 4; b_++) RS_BYTE(x_ >> (b_ * 8u)); } while (0)
#define RS_OP32() do { if (!width32) RS_BYTE(0x66); } while (0)
#define RS_ADDR32() do { if (!width32) RS_BYTE(0x67); } while (0)
        for (unsigned vector = 11; vector <= 14; vector++) {
            unsigned off = 0x100 + (vector - 11u) * 0x40;
            if (policy != 17) {
                vm->dpmi.exception_vectors[vector].sel = f.code;
                vm->dpmi.exception_vectors[vector].off = off;
            }
            out = memory + 0x20000 + off;
            RS_ADDR32(); RS_BYTE(0xC6); RS_BYTE(0x05); RS_DWORD(0x8010); RS_BYTE(vector);
            RS_BYTE(0xE9);
            if (width32) { RS_DWORD(0x20300 - (uint32_t)(out - memory) - 4u); }
            else { RS_WORD(0x20300 - (uint32_t)(out - memory) - 2u); }
        }
        vm->dpmi.exception_vectors[3].sel = f.code; vm->dpmi.exception_vectors[3].off = 0x600;
        out = memory + 0x20300;
        RS_OP32(); RS_BYTE(0x60); RS_OP32(); RS_BYTE(0x89); RS_BYTE(0xE5);
        RS_OP32(); RS_ADDR32(); RS_BYTE(0xFF); RS_BYTE(0x05); RS_DWORD(0x8000);
        const unsigned fields[] = { 2, 3, 6, 7 };
        for (unsigned i = 0; i < 4; i++) {
            RS_BYTE(0x36); RS_ADDR32(); RS_BYTE(0x8B); RS_BYTE(0x45); RS_BYTE(32u + fields[i] * width);
            RS_ADDR32(); RS_BYTE(0xA3); RS_DWORD(0x8020 + i * 4u);
        }
        if (policy == 8) RS_BYTE(0xCC);
        if (policy == 11) {
            if (width32) RS_BYTE(0x66);
            RS_BYTE(0xB8); RS_WORD(0x4C2A); RS_BYTE(0xCD); RS_BYTE(0x21);
        }
        if (policy == 12) { RS_BYTE(0xEB); RS_BYTE(0xFE); }
        uint8_t *skip = NULL, *begin = NULL;
        if (policy == 7) {
            RS_OP32(); RS_ADDR32(); RS_BYTE(0x83); RS_BYTE(0x3D); RS_DWORD(0x8000); RS_BYTE(1);
            RS_BYTE(0x74); skip = out++; begin = out;
        }
        RS_OP32(); RS_ADDR32(); RS_BYTE(0xC7); RS_BYTE(0x05); RS_DWORD(second); RS_DWORD(0x26007);
        if (policy >= 4 && policy <= 6) {
            if (width32) RS_BYTE(0x66);
            RS_BYTE(0xBB); RS_WORD(f.buffer);
            if (width32) RS_BYTE(0x66);
            RS_BYTE(0xB8); RS_WORD(policy == 6 ? 8 : 9);
            if (width32) RS_BYTE(0x66);
            RS_BYTE(0xB9); RS_WORD(policy == 6 ? 0 : width32 ? 0x40F2 : 0xF2);
            if (policy == 6) {
                if (width32) RS_BYTE(0x66);
                RS_BYTE(0xBA); RS_WORD(0xFFFF);
            }
            RS_BYTE(0xCD); RS_BYTE(0x31);
        }
        if (skip) *skip = (uint8_t)(out - begin);
        if (policy == 9) {
            RS_BYTE(0x06);
            if (width32) RS_BYTE(0x66);
            RS_BYTE(0xB8); RS_WORD(f.data); RS_BYTE(0x8E); RS_BYTE(0xC0);
            RS_OP32(); RS_BYTE(0xBF); RS_DWORD(0x9000);
            if (width32) RS_BYTE(0x66);
            RS_BYTE(0xBB); RS_WORD(target_sel);
            if (width32) RS_BYTE(0x66);
            RS_BYTE(0xB8); RS_WORD(0x000B); RS_BYTE(0xCD); RS_BYTE(0x31);
            RS_ADDR32(); RS_BYTE(0xA3); RS_DWORD(0x8030);
            RS_BYTE(0x9C); RS_BYTE(0x58); RS_ADDR32(); RS_BYTE(0xA3); RS_DWORD(0x8034);
            RS_BYTE(0x07);
        }
        if (policy == 13 || policy == 14) {
            if (width32) RS_BYTE(0x66);
            RS_BYTE(0xBB); RS_WORD(target_sel);
            if (width32) RS_BYTE(0x66);
            RS_BYTE(0xB8); RS_WORD(policy == 13 ? 1 : 7);
            if (policy == 14) {
                if (width32) RS_BYTE(0x66);
                RS_BYTE(0xB9); RS_WORD(0xBEEF);
                if (width32) RS_BYTE(0x66);
                RS_BYTE(0xBA); RS_WORD(0x1000);
            }
            RS_BYTE(0xCD); RS_BYTE(0x31);
        }
        if (policy == 15) {
            RS_BYTE(0x36); RS_OP32(); RS_ADDR32(); RS_BYTE(0xC7); RS_BYTE(0x45); RS_BYTE(16);
            RS_DWORD(0xBEEF0000u | other_sel);
            RS_BYTE(0x36); RS_OP32(); RS_ADDR32(); RS_BYTE(0xC7); RS_BYTE(0x45); RS_BYTE(0);
            RS_DWORD(0x0BAD9000);
            if (width32) RS_BYTE(0x66);
            RS_BYTE(0xB8); RS_WORD(f.data); RS_BYTE(0x8E); RS_BYTE(0xC0);
        }
        if (policy == 10) {
            RS_BYTE(0x36); RS_ADDR32(); RS_BYTE(0xC7); RS_BYTE(0x45); RS_BYTE(32u + 3u * width);
            if (width32) { RS_DWORD(0x80); } else { RS_WORD(0x80); }
        }
        RS_BYTE(0x36); RS_ADDR32(); RS_BYTE(0x81); RS_BYTE(0x65); RS_BYTE(32u + 5u * width);
        if (width32) { RS_DWORD(~(uint32_t)FLAG_DF); } else { RS_WORD(~FLAG_DF); }
        RS_OP32(); RS_BYTE(0x61); RS_BYTE(0xCB);
        RECORD_SERVICE_CHECK(out < memory + 0x20600);
        out = memory + 0x20600;
        RS_OP32(); RS_ADDR32(); RS_BYTE(0xFF); RS_BYTE(0x05); RS_DWORD(0x8004); RS_BYTE(0xCB);
#undef RS_ADDR32
#undef RS_OP32
#undef RS_DWORD
#undef RS_WORD
#undef RS_BYTE
        if (policy && (policy < 4 || policy > 6))
            dos_mem_write32(vm, second, policy == 2 ? 0x26005 : policy == 3 ? 0x26003 : 0x26006);
        if (policy == 4) bd->access = 0xF8;
        if (policy == 5) bd->access &= ~DESC_PRESENT;
        if (policy == 6) dpmi_desc_set_limit(bd, size - 2u);
        if (policy == 19) vm->dpmi.descriptor_state[dpmi_sel_to_index(target_sel)] = DPMI_DESC_HOST;
        cpu8086_state_t before = cpu;
        if (interpreted) (void)cpu8086_run_one(vm); else dos_int31_dpmi(vm);
        bool cancelled = policy == 10, stopped = policy == 11 || policy == 12 || policy == 17;
        bool service_error = policy == 13 || policy == 16 || policy == 18 || policy == 19;
        bool faulted = policy != 0 && policy != 18 && policy != 19;
        bool completed = !cancelled && !stopped && !service_error;
        RECORD_SERVICE_CHECK(!vm->dpmi.host_wait && !vm->interpreter_stop_signal &&
                             !vm->software_int_frame_bytes && !vm->native_dispatch_depth);
        RECORD_SERVICE_CHECK(dos_mem_read32(vm, 0x8000) == (policy == 17 ? 0u : policy == 7 ? 2u : faulted) &&
                             dos_mem_read32(vm, 0x8004) == (policy == 8));
        if (stopped) {
            RECORD_SERVICE_CHECK(!cpu.running && (policy == 12 ? vm->step_limit_reached :
                                                  cpu.exit_code == (policy == 11 ? 42 : -1)));
        } else {
            RECORD_SERVICE_CHECK(cpu.running && !vm->dpmi.exception_depth && cpu.cs == before.cs &&
                                 cpu.eip == (cancelled ? 0x80u : 15u) && cpu.ss == before.ss && cpu.esp == before.esp);
            RECORD_SERVICE_CHECK(cpu.eax == (service_error ? (before.eax & 0xFFFF0000u) |
                                 (policy == 16 ? 0x8021u : 0x8022u) : before.eax) &&
                                 cpu.ebx == (policy == 15 ? 0xBEEF0000u | other_sel : before.ebx) &&
                                 cpu.edi == (policy == 15 ? 0x0BAD9000u : before.edi) &&
                                 cpu.es == (policy == 15 ? f.data : before.es) && cpu.ecx == before.ecx &&
                                 cpu.edx == before.edx && cpu.esi == before.esi && cpu.ebp == before.ebp);
            RECORD_SERVICE_CHECK(!!(cpu.flags & FLAG_CF) == (cancelled || service_error) &&
                                 !!(cpu.flags & FLAG_DF) == !faulted);
            if (faulted && completed) recovered++;
        }
        if (faulted && !stopped) {
            unsigned vector = policy == 5 ? 11 : policy == 4 || policy == 6 ? 13 : 14;
            unsigned error = policy == 4 || policy == 5 ? f.buffer & ~3u : policy == 6 ? 0 :
                             4u | (input ? 0u : 2u) | (policy == 2 || policy == 3);
            RECORD_SERVICE_CHECK(memory[0x8010] == vector && dos_mem_read16(vm, 0x8020) == error &&
                                 dos_mem_read16(vm, 0x8024) == 15 && dos_mem_read16(vm, 0x8028) == before.sp &&
                                 dos_mem_read16(vm, 0x802C) == before.ss && (vector != 14 || cpu.cr2 == linear + 4u));
        }
        dpmi_descriptor_t expected_descriptor = original;
        if (policy == 13) dpmi_zero(&expected_descriptor, sizeof(expected_descriptor));
        else if (input && completed) expected_descriptor = incoming;
        else if (policy == 14) dpmi_desc_set_base(&expected_descriptor, 0xBEEF1000);
        RECORD_SERVICE_CHECK(dpmi_descriptor_unchanged(target, &expected_descriptor));
        uint32_t total = dpmi_ext_total_pages(vm), free = dpmi_ext_free_page_count(vm), largest = dpmi_ext_largest_free_page_count(vm);
        uint32_t info[12] = { largest * DPMI_EXT_PAGE_SIZE, largest, largest, total, total - free,
                             free, total, free, 0, UINT32_MAX, UINT32_MAX, UINT32_MAX };
        bool intact = true;
        for (unsigned i = 0; i < size; i++) {
            uint8_t expected = input ? ((uint8_t *)&incoming)[i] : !completed ? 0xA5 :
                               kind == 2 ? ((uint8_t *)info)[i] : ((uint8_t *)&expected_descriptor)[i];
            if (memory[i < 4 ? 0x24FFC + i : 0x26000 + i - 4u] != expected) intact = false;
        }
        RECORD_SERVICE_CHECK(intact && memory[0x24FFB] == 0x39 && memory[0x25000] == 0x39 &&
                             memory[0x25FFF] == 0x39 && memory[0x26000 + size - 4u] == 0x39);
        if (policy == 9) {
            RECORD_SERVICE_CHECK(dos_mem_read16(vm, 0x8030) == 0x000B && !(dos_mem_read16(vm, 0x8034) & FLAG_CF));
            for (unsigned i = 0; i < sizeof(original); i++)
                RECORD_SERVICE_CHECK(memory[0x9000 + i] == ((uint8_t *)&original)[i]);
        }
    }
    serial_puts("[DPMI-RECORD-SERVICE] checks="); serial_putdec(checks);
    serial_puts(" recovered="); serial_putdec(recovered);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef RECORD_SERVICE_CHECK
    dos_host_free_pages(memory, memory_pages); dos_host_free_pages(vm, vm_pages);
    return failures;
}

static int dpmi_record_buffer_selftest(void)
{
    const unsigned memory_pages = 512;
    unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (vm) dos_host_free_pages(vm, vm_pages);
        if (memory) dos_host_free_pages(memory, memory_pages);
        return 1;
    }
    dpmi_zero(vm, vm_pages * 4096u);
    cpu8086_state_t cpu;
    vm->cpu = &cpu;
    vm->mem = memory;
    unsigned checks = 0;
    int failures = 0;
#define RECORD_CHECK(condition) do { \
    if (!(condition)) { \
        if (failures < 16) { serial_puts("[DPMI-RECORD-BUFFER] failed check="); serial_putdec(checks); serial_puts("\n"); } \
        failures++; \
    } \
    checks++; \
} while (0)
    for (unsigned width = 0; width < 2; width++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned set = 0; set < 2; set++)
    for (unsigned policy = 0; policy < 19; policy++) {
        vm->total_mem_size = memory_pages * 4096u;
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cpl = 3;
        dpmi_descriptor_t *bd = &vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)];
        uint32_t base = high ? 0x40001000 : 0x6000;
        uint32_t offset = 0xFFC;
        if (policy == 12) {
            base = high ? 0x40000000 : 0x8000;
            offset = width ? 0x1FFFC : 0xFFFC;
        }
        dpmi_build_desc(bd, base, 0x2FFFF, 0xF2, width ? DESC_32BIT : 0);
        uint32_t linear = base + offset;
        uint32_t directory = high ? 0x10400 : 0x10000;
        uint32_t first = (high ? 0x12000 : 0x11000) + ((linear >> 12) & 0x3FFu) * 4u;
        dos_mem_write32(vm, first, 0x24007);
        dos_mem_write32(vm, first + 4u, 0x26007);
        cpu.es = f.buffer;
        cpu.ebx = 0x5A5A0000u | (policy == 11 ? f.buffer : f.data);
        cpu.edi = width ? offset : 0xA5A50000u | offset;
        dpmi_descriptor_t *target = &vm->dpmi.ldt[dpmi_sel_to_index(cpu.bx)];
        dpmi_descriptor_t incoming;
        dpmi_build_desc(&incoming, 0x45678000, 0xFFFF, 0xF2, DESC_32BIT);
        bool accepted = policy == 0 || policy == 11 || policy == 12 || policy == 13 ||
                        policy == 15 || policy == 16 || (set && (policy == 1 || policy == 2)) ||
                        (!set && (policy == 9 || policy == 10));
        switch (policy) {
        case 1: bd->access = 0xF0; break;
        case 2: dos_mem_write32(vm, first + 4u, 0x26005); break;
        case 3: dos_mem_write32(vm, first + 4u, 0x26003); break;
        case 4: dos_mem_write32(vm, first + 4u, 0); break;
        case 5: dos_mem_write32(vm, first + 4u, vm->total_mem_size | 7u); break;
        case 6: dpmi_desc_set_limit(bd, offset + 6u); break;
        case 7: cpu.es = 0; break;
        case 8: cpu.bx = 0; break;
        case 9: vm->dpmi.descriptor_state[dpmi_sel_to_index(cpu.bx)] = DPMI_DESC_HOST; break;
        case 10: incoming.access = 0xB2; break;
        case 13: target->access &= ~DESC_PRESENT; break;
        case 14: vm->total_mem_size = 0x26002; break;
        case 15: dpmi_desc_set_limit(bd, offset + 7u); break;
        case 16: bd->access |= 4u; dpmi_desc_set_limit(bd, offset - 1u); break;
        case 17: dpmi_desc_set_base(bd, UINT32_MAX - offset + 1u); break;
        case 18: bd->access &= ~DESC_PRESENT; break;
        }
        dpmi_descriptor_t original = *target;
        for (unsigned i = 0; i < sizeof(incoming); i++) {
            memory[i < 4 ? 0x24FFC + i : 0x26000 + i - 4u] = set ? ((uint8_t *)&incoming)[i] : 0xA5;
            if (!high) memory[linear + i] = 0x6C;
        }
        memory[0x24FFB] = memory[0x25000] = memory[0x25FFF] = memory[0x26004] = 0x39;
        uint32_t old_directory = dos_mem_read32(vm, directory);
        uint32_t old_first = dos_mem_read32(vm, first), old_second = dos_mem_read32(vm, first + 4u);
        cpu.eax = 0xACED0000u | (set ? 0x000C : 0x000B);
        cpu.eflags = FLAGS_FIXED | FLAG_IF | FLAG_DF | FLAG_CF;
        cpu8086_state_t expected_cpu = cpu;
        bool pointer_fault = !accepted && policy != 8 && !(set && (policy == 9 || policy == 10));
        if (accepted) expected_cpu.eflags &= ~FLAG_CF;
        else if (!pointer_fault) expected_cpu.ax = set && policy == 10 ? 0x8021 : 0x8022;
        if (pointer_fault) {
            /* Raw admission stays side-effect free. Real handler delivery
             * and service cancellation are exercised by the service matrix. */
            dpmi_client_buffer_t buffer;
            dpmi_buffer_fault_t fault;
            RECORD_CHECK(!dpmi_client_buffer_probe(vm, cpu.es, offset, sizeof(incoming),
                                                   !set, &buffer, &fault));
            RECORD_CHECK(fault.vector == 11 || fault.vector == 13 || fault.vector == 14);
        } else dos_int31_dpmi(vm);
        bool same_cpu = true, same_payload = true, same_descriptor = true;
        dpmi_descriptor_t expected_descriptor = set && accepted ? incoming : original;
        for (unsigned i = 0; i < sizeof(cpu); i++)
            if (((uint8_t *)&cpu)[i] != ((uint8_t *)&expected_cpu)[i]) same_cpu = false;
        for (unsigned i = 0; i < sizeof(incoming); i++) {
            uint8_t expected = set ? ((uint8_t *)&incoming)[i] : accepted ? ((uint8_t *)&original)[i] : 0xA5;
            if (memory[i < 4 ? 0x24FFC + i : 0x26000 + i - 4u] != expected ||
                (!high && memory[linear + i] != 0x6C)) same_payload = false;
            if (((uint8_t *)target)[i] != ((uint8_t *)&expected_descriptor)[i]) same_descriptor = false;
        }
        RECORD_CHECK(same_cpu);
        RECORD_CHECK(same_payload);
        RECORD_CHECK(same_descriptor);
        RECORD_CHECK(memory[0x24FFB] == 0x39 && memory[0x25000] == 0x39 &&
                     memory[0x25FFF] == 0x39 && memory[0x26004] == 0x39);
        bool accessed = accepted || (set && policy == 10);
        uint32_t bits = accessed ? set ? 0x20 : 0x60 : 0;
        RECORD_CHECK(dos_mem_read32(vm, directory) == (old_directory | (accessed ? 0x20u : 0u)));
        RECORD_CHECK(dos_mem_read32(vm, first) == (old_first | bits) &&
                     dos_mem_read32(vm, first + 4u) == (old_second | bits));
    }

    /* A descriptor output may overwrite the PTE that translated its first
     * half. The second half is already admitted, across a PDE boundary. */
    vm->total_mem_size = memory_pages * 4096u;
    dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, 1);
    dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
    dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)], 0, UINT32_MAX, 0xF2, DESC_32BIT);
    dos_mem_write32(vm, 0x10004, 0x12007);
    dos_mem_write32(vm, 0x11FFC, 0x11007);
    dos_mem_write32(vm, 0x12000, 0x24007);
    cpu.es = f.buffer; cpu.bx = f.data; cpu.edi = 0x3FFFFC; cpu.ax = 0x000B;
    dpmi_descriptor_t descriptor = vm->dpmi.ldt[dpmi_sel_to_index(f.data)];
    dos_int31_dpmi(vm);
    RECORD_CHECK(!(cpu.eflags & FLAG_CF));
    for (unsigned i = 0; i < sizeof(descriptor); i++)
        RECORD_CHECK(memory[i < 4 ? 0x11FFC + i : 0x24000 + i - 4u] == ((uint8_t *)&descriptor)[i]);

    for (unsigned width = 0; width < 2; width++)
    for (unsigned v86 = 0; v86 < 2; v86++)
    for (unsigned restore = 0; restore < 2; restore++)
    for (unsigned policy = 0; policy < 16; policy++) {
        vm->total_mem_size = memory_pages * 4096u;
        f = dpmi_paging_test_prepare(vm, width);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        cpu.eflags = FLAGS_FIXED | FLAG_CF | FLAG_DF | FLAG_IF | (v86 ? FLAG_VM : 0);
        cpu.es = 0x600; cpu.edi = 0xABCD0FFC; cpu.al = restore;
        dpmi_saved_state_t state = { 0x32535044u, 0xA5A58000, f.stack, 1, 0, DOS_CR0_PG | 0x11u, 0x14018 };
        vm->dpmi.suspended_stack = (dpmi_stack_t){ state.ss, state.esp };
        vm->dpmi.suspended_paging = (dpmi_paging_t){ state.cr0, state.cr3 };
        bool accepted = policy == 0 || policy == 8 || policy == 13 ||
                        ((policy == 1 || policy == 5) && restore) ||
                        (policy == 2 && !v86) || (policy == 14 && (restore || !v86)) ||
                        ((policy == 9 || policy == 10 || policy == 11 || policy == 12) && !restore);
        switch (policy) {
        case 1: dos_mem_write32(vm, 0x1101C, 0x26005); break;
        case 2: dos_mem_write32(vm, 0x1101C, 0x26003); break;
        case 3: dos_mem_write32(vm, 0x1101C, 0); break;
        case 4: dos_mem_write32(vm, 0x1101C, vm->total_mem_size | 7u); break;
        case 5: dos_mem_write32(vm, 0x10000, 0x11005); break;
        case 6: dos_mem_write32(vm, 0x10000, 0); break;
        case 7: cpu.di = 0xFFED; break;
        case 8: cpu.es = 0; cpu.di = 0xFFEC; break;
        case 9: if (restore) state.signature ^= 1u; break;
        case 10: if (restore) state.reserved = 1; break;
        case 11: if (restore) state.protected_mode = 0; break;
        case 12: if (restore) state.cr0 &= ~1u; break;
        case 13: cpu.cr0 = 0x10; cpu.eflags &= ~FLAG_VM; break;
        case 14: cpu.cr0 &= ~DOS_CR0_WP; dos_mem_write32(vm, 0x1101C, 0x26005); break;
        case 15: vm->total_mem_size = 0x2600F; break;
        }
        bool identity = policy == 8 || policy == 13;
        uint32_t at = dos_linear(cpu.es, cpu.di);
        for (unsigned i = 0; i < sizeof(state); i++) {
            memory[at + i] = 0x6C;
            memory[identity ? at + i : i < 4 ? 0x24FFC + i : 0x26000 + i - 4u] =
                restore ? ((uint8_t *)&state)[i] : 0xA5;
        }
        if (restore) {
            vm->dpmi.suspended_stack = (dpmi_stack_t){ 0x4444, 0x5555 };
            vm->dpmi.suspended_paging = (dpmi_paging_t){ 1, 0x18000 };
        }
        dpmi_stack_t previous_stack = vm->dpmi.suspended_stack;
        dpmi_paging_t previous_paging = vm->dpmi.suspended_paging;
        uint32_t entries[] = { 0x10000, 0x11018, 0x1101C, 0x1103C };
        uint32_t old_entries[4];
        for (unsigned i = 0; i < 4; i++) old_entries[i] = dos_mem_read32(vm, entries[i]);
        cpu8086_state_t before = cpu;
        RECORD_CHECK(dpmi_save_restore_state(vm) == (accepted ? DPMI_SERVICE_COMPLETE : DPMI_SERVICE_INVALID));
        bool same_cpu = true, same_payload = true, same_ad = true;
        for (unsigned i = 0; i < sizeof(cpu); i++)
            if (((uint8_t *)&cpu)[i] != ((uint8_t *)&before)[i]) same_cpu = false;
        for (unsigned i = 0; i < sizeof(state); i++) {
            uint8_t expected = restore || accepted ? ((uint8_t *)&state)[i] : 0xA5;
            if (memory[identity ? at + i : i < 4 ? 0x24FFC + i : 0x26000 + i - 4u] != expected ||
                (!identity && memory[at + i] != 0x6C)) same_payload = false;
        }
        bool accessed = accepted || (restore && policy >= 9 && policy <= 12);
        for (unsigned i = 0; i < 4; i++) {
            uint32_t bits = !accessed || policy == 13 ? 0 : i == 0 ? 0x20 : restore ? 0x20 : 0x60;
            if ((policy == 8 && (i == 1 || i == 2)) || (policy != 8 && i == 3)) bits = 0;
            if (dos_mem_read32(vm, entries[i]) != (old_entries[i] | bits)) same_ad = false;
        }
        RECORD_CHECK(same_cpu);
        RECORD_CHECK(same_payload);
        RECORD_CHECK(same_ad);
        RECORD_CHECK(vm->dpmi.suspended_stack.ss == (accepted && restore ? state.ss : previous_stack.ss) &&
                     vm->dpmi.suspended_stack.esp == (accepted && restore ? state.esp : previous_stack.esp));
        RECORD_CHECK(vm->dpmi.suspended_paging.cr0 == (accepted && restore ? state.cr0 : previous_paging.cr0) &&
                     vm->dpmi.suspended_paging.cr3 == (accepted && restore ? state.cr3 : previous_paging.cr3));
    }
    dos_host_free_pages(memory, memory_pages);
    dos_host_free_pages(vm, vm_pages);
    serial_puts("[DPMI-RECORD-BUFFER] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef RECORD_CHECK
    return failures + dpmi_record_service_selftest();
}

static int dpmi_io_service_selftest(void)
{
    const unsigned memory_pages = 512;
    unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (vm) dos_host_free_pages(vm, vm_pages);
        if (memory) dos_host_free_pages(memory, memory_pages);
        return 1;
    }
    dpmi_zero(vm, vm_pages * 4096u);
    cpu8086_state_t cpu;
    vm->cpu = &cpu; vm->mem = memory; vm->total_mem_size = memory_pages * 4096u;
    unsigned checks = 0, recovered = 0;
    int failures = 0;
#define IO_SERVICE_CHECK(condition) do { \
    if (!(condition)) { \
        if (failures < 24) { serial_puts("[DPMI-IO-SERVICE] failed check="); serial_putdec(checks); \
            serial_puts(" kind="); serial_putdec(kind); serial_puts(" policy="); serial_putdec(policy); \
            serial_puts(" width/stack/high/int="); serial_putdec(width32); serial_putdec(stack32); \
            serial_putdec(high); serial_putdec(interpreted); serial_puts("\n"); } \
        failures++; \
    } \
    checks++; \
} while (0)
    for (unsigned width32 = 0; width32 < 2; width32++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned interpreted = 0; interpreted < 2; interpreted++)
    for (unsigned kind = 0; kind < 5; kind++)
    for (unsigned policy = 0; policy < 22; policy++) {
        bool output = kind == 0 || kind >= 3;
        bool large = policy >= 18;
        if (large && (kind >= 2 || (policy == 19 && kind != 0))) continue;
        if (!output && ((policy >= 3 && policy <= 10) || (policy >= 14 && !large))) continue;
        if (!output && policy == 1) continue;
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width32);
        dos_mem_init(vm); vm->current_psp = 0x50;
        dos_api_init(vm);
        /* Keep the fixture's code, tables and counters out of the DOS pool. */
        IO_SERVICE_CHECK(dos_mem_alloc(vm, large ? 0x9700 : 0x7000, NULL) != 0);
        uint16_t before_free = 0, after_free = 0;
        IO_SERVICE_CHECK(!dos_mem_alloc(vm, 0xFFFF, &before_free) && before_free);
        uint32_t capacity = large ? ((uint32_t)before_free - 0x100u) * 16u : 32u;
        uint32_t count = large ? capacity + 32u : 32u;
        vm->sft[0].open_mode = 2; /* read/write handle for the real INT hook */
        vm->sft[0].device_kind = DOS_DEVICE_NUL;
        vm->software_int_frame_bytes = 0;
        vm->step_limit = policy == 10 ? 128 : 2048;
        unsigned width = width32 ? 4u : 2u;
        uint32_t linear = large ? (high ? 0x3FF000u : 0x200000u) + ((0x1000u - (capacity & 0xFFFu)) & 0xFFFu) :
                                  high ? 0x3FFFF0 : 0x6FF0;
        uint32_t first = high ? 0x11FFC : large ? 0x11800 : 0x11018;
        uint32_t second = high ? 0x13000 : 0x1101C;
        if (high) dos_mem_write32(vm, 0x10004, 0x13007);
        unsigned tail_page = ((linear & 0xFFFu) + capacity) / 4096u;
        uint32_t tail_entry = high ? 0x13000 + (tail_page - 1u) * 4u : first + tail_page * 4u;
        uint32_t tail_physical = 0x30000 + tail_page * 4096u;
        if (large) {
            unsigned pages = ((linear & 0xFFFu) + count + 4095u) / 4096u;
            for (unsigned p = 0; p < pages; p++)
                dos_mem_write32(vm, high && p ? 0x13000 + (p - 1u) * 4u : first + p * 4u,
                                (0x30000 + p * 4096u) | 7u);
        } else {
            dos_mem_write32(vm, first, 0x24007); dos_mem_write32(vm, second, 0x26007);
        }
        dpmi_descriptor_t *d = &vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)];
        dpmi_desc_set_base(d, linear);
        dpmi_desc_set_limit(&vm->dpmi.ldt[dpmi_sel_to_index(f.data)], UINT32_MAX);
        vm->dpmi.ldt[dpmi_sel_to_index(f.stack)].flags_lim = stack32 ? DESC_32BIT : 0;
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cs = f.code; cpu.ss = f.stack; cpu.ds = f.buffer; cpu.es = f.data;
        cpu.esp = stack32 ? 0x8F00 : 0xBEEF8F00; cpu.eip = interpreted ? 0 : 15;
        const uint16_t functions[] = { 0x3F00, 0x4000, 0x0900, 0x0A00, 0x0C0A };
        cpu.eax = 0xA5A50000 | functions[kind]; cpu.ebx = 0xABCD0000;
        cpu.ecx = width32 ? count : 0xABCD0000 | count; cpu.edx = width32 ? 0 : 0xABCD0000;
        cpu.esi = 0x12345678; cpu.edi = 0x98765432; cpu.ebp = 0xCAFEBABE;
        cpu.eflags = FLAGS_FIXED | FLAG_CF | FLAG_IF | FLAG_DF;
        cpu8086_sync_cs(&cpu); cpu8086_sync_data(&cpu);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        vm->dpmi.virtual_interrupts_enabled = true;
        dpmi_get_exception_stack_selector(vm);
        for (unsigned i = 0; i < 32; i++) {
            memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u] = 0xA5;
            memory[i < 16 ? 0x28FF0 + i : 0x2A000 + i - 16u] = 0xC7;
        }
        if (large)
            for (unsigned i = 0; i < count; i++) memory[0x30000 + (linear & 0xFFFu) + i] = 0xA5;
        if (kind == 2) memory[0x2600F] = '$';
        if (kind >= 3) { memory[0x24FF0] = 30; memory[0x24FF1] = 0; }
        for (unsigned i = 0; i < 13; i++) memory[0x20000 + i] = 0x3E;
        memory[0x2000D] = 0xCD; memory[0x2000E] = 0x21;
        memory[0x2000F] = memory[0x20080] = 0xF4;
        uint8_t *out;
#define IOS_BYTE(value) (*out++ = (uint8_t)(value))
#define IOS_WORD(value) do { uint16_t x_ = (value); IOS_BYTE(x_); IOS_BYTE(x_ >> 8); } while (0)
#define IOS_DWORD(value) do { uint32_t x_ = (value); for (unsigned b_ = 0; b_ < 4; b_++) IOS_BYTE(x_ >> (b_ * 8u)); } while (0)
#define IOS_OP32() do { if (!width32) IOS_BYTE(0x66); } while (0)
#define IOS_ADDR32() do { if (!width32) IOS_BYTE(0x67); } while (0)
        /* Real driver: an observable invocation, a result, then a revoked
         * output page. Never call it again just to repair the destination. */
        out = memory + 0x90100;
        IOS_BYTE(0x2E); IOS_BYTE(0xFF); IOS_BYTE(0x06); IOS_WORD(0x200);
        IOS_BYTE(0x2E); IOS_BYTE(0x01); IOS_BYTE(0x0E); IOS_WORD(0x208); /* ADD CS:bytes,CX */
        IOS_BYTE(0x8C); IOS_BYTE(0xD8); /* MOV AX,DS */
        IOS_BYTE(0x2E); IOS_BYTE(0xA3); IOS_WORD(0x204);
        uint8_t *inner_skip = NULL, *inner_begin = NULL;
        if (policy == 17) {
            IOS_BYTE(0x83); IOS_BYTE(0xF9); IOS_BYTE(1); IOS_BYTE(0x74);
            inner_skip = out++; inner_begin = out;
        }
        if (output) {
            IOS_BYTE(0x89); IOS_BYTE(0xD3);
            IOS_BYTE(0xC7); IOS_BYTE(0x07); IOS_WORD(kind >= 3 ? 0x011E : 0x1234);
            IOS_BYTE(0xC7); IOS_BYTE(0x47); IOS_BYTE(30); IOS_WORD(0x5678);
        }
        bool after_call_fault = (policy >= 3 && policy <= 10) || policy >= 14;
        uint32_t bad_entry = large ? tail_entry : policy == 15 ? first : second;
        uint32_t bad_value = (large ? tail_physical : policy == 15 ? 0x24000u : 0x26000u) |
                             (policy == 4 ? 5u : policy == 5 ? 3u : 6u);
        if (after_call_fault) {
            uint8_t *later_skip = NULL, *later_begin = NULL;
            if (large) {
                IOS_BYTE(0x2E); IOS_BYTE(0x83); IOS_BYTE(0x3E); IOS_WORD(0x200); IOS_BYTE(policy == 19 ? 2 : 1);
                IOS_BYTE(0x75); later_skip = out++; later_begin = out;
            }
            IOS_BYTE(0x1E); IOS_BYTE(0xB8); IOS_WORD(bad_entry >> 4);
            IOS_BYTE(0x8E); IOS_BYTE(0xD8);
            IOS_BYTE(0x66); IOS_BYTE(0xC7); IOS_BYTE(0x06); IOS_WORD(bad_entry & 15u); IOS_DWORD(bad_value);
            IOS_BYTE(0x1F);
            if (later_skip) *later_skip = (uint8_t)(out - later_begin);
        }
        if (inner_skip) *inner_skip = (uint8_t)(out - inner_begin);
        IOS_BYTE(0x55); IOS_BYTE(0x89); IOS_BYTE(0xE5);
        IOS_BYTE(0x83); IOS_BYTE(policy == 16 ? 0x4E : 0x66); IOS_BYTE(6); IOS_BYTE(policy == 16 ? 1 : 0xFE);
        IOS_BYTE(0x5D);
        if (policy == 17 || large) { IOS_BYTE(0x89); IOS_BYTE(0xC8); }
        else {
            IOS_BYTE(0xB8); IOS_WORD(policy == 16 ? 5 : kind == 2 ? 0x0924 : policy == 15 ? 2 : 32);
        }
        IOS_BYTE(0xCF);
        IO_SERVICE_CHECK(out < memory + 0x90200);
        dos_mem_write32(vm, 0x21 * 4u, 0x90000100);

        for (unsigned vector = 11; vector <= 14; vector++) {
            unsigned off = 0x100 + (vector - 11u) * 0x40;
            vm->dpmi.exception_vectors[vector].sel = f.code;
            vm->dpmi.exception_vectors[vector].off = off;
            out = memory + 0x20000 + off;
            IOS_BYTE(0x26); IOS_ADDR32(); IOS_BYTE(0xC6); IOS_BYTE(0x05); IOS_DWORD(0x8010); IOS_BYTE(vector);
            IOS_BYTE(0xE9);
            if (width32) { IOS_DWORD(0x20300 - (uint32_t)(out - memory) - 4u); }
            else { IOS_WORD(0x20300 - (uint32_t)(out - memory) - 2u); }
        }
        vm->dpmi.exception_vectors[3].sel = f.code;
        vm->dpmi.exception_vectors[3].off = 0x600;
        out = memory + 0x20300;
        IOS_OP32(); IOS_BYTE(0x60); IOS_OP32(); IOS_BYTE(0x89); IOS_BYTE(0xE5);
        IOS_BYTE(0x26); IOS_OP32(); IOS_ADDR32(); IOS_BYTE(0xFF); IOS_BYTE(0x05); IOS_DWORD(0x8000);
        const unsigned fields[] = { 2, 3, 6, 7 };
        for (unsigned i = 0; i < 4; i++) {
            IOS_BYTE(0x36); IOS_ADDR32(); IOS_BYTE(0x8B); IOS_BYTE(0x45); IOS_BYTE(32u + fields[i] * width);
            IOS_BYTE(0x26); IOS_ADDR32(); IOS_BYTE(0xA3); IOS_DWORD(0x8020 + i * 4u);
        }
        if (large) {
            IOS_BYTE(0x26); IOS_ADDR32(); IOS_BYTE(0xA1); IOS_DWORD(0x90200);
            IOS_BYTE(0x26); IOS_ADDR32(); IOS_BYTE(0xA3); IOS_DWORD(0x8038);
            IOS_BYTE(0x26); IOS_ADDR32(); IOS_BYTE(0xA1); IOS_DWORD(0x90208);
            IOS_BYTE(0x26); IOS_ADDR32(); IOS_BYTE(0xA3); IOS_DWORD(0x803C);
        }
        if (policy == 7) IOS_BYTE(0xCC);
        if (policy == 9 || policy == 21) {
            if (width32) IOS_BYTE(0x66);
            IOS_BYTE(0xB8); IOS_WORD(0x4C2A); IOS_BYTE(0xCD); IOS_BYTE(0x21);
        }
        if (policy == 10) { IOS_BYTE(0xEB); IOS_BYTE(0xFE); }
        if (policy == 17) {
            IOS_BYTE(0x1E);
            if (width32) IOS_BYTE(0x66);
            IOS_BYTE(0xB8); IOS_WORD(f.data); IOS_BYTE(0x8E); IOS_BYTE(0xD8);
            IOS_OP32(); IOS_BYTE(0xBA); IOS_DWORD(0x8040);
            IOS_OP32(); IOS_BYTE(0xB9); IOS_DWORD(1);
            IOS_OP32(); IOS_BYTE(0xBB); IOS_DWORD(0);
            if (width32) IOS_BYTE(0x66);
            IOS_BYTE(0xB8); IOS_WORD(0x4000); IOS_BYTE(0xCD); IOS_BYTE(0x21);
            IOS_BYTE(0x26); IOS_ADDR32(); IOS_BYTE(0xA3); IOS_DWORD(0x8030);
            IOS_BYTE(0x9C); IOS_BYTE(0x58);
            IOS_BYTE(0x26); IOS_ADDR32(); IOS_BYTE(0xA3); IOS_DWORD(0x8034);
            IOS_BYTE(0x1F);
        }
        uint8_t *skip = NULL, *begin = NULL;
        if (policy == 6) {
            IOS_BYTE(0x26); IOS_OP32(); IOS_ADDR32(); IOS_BYTE(0x83); IOS_BYTE(0x3D); IOS_DWORD(0x8000); IOS_BYTE(1);
            IOS_BYTE(0x74); skip = out++; begin = out;
        }
        for (unsigned i = 0; i < (large ? 1u : 2u); i++) {
            IOS_BYTE(0x26); IOS_OP32(); IOS_ADDR32(); IOS_BYTE(0xC7); IOS_BYTE(0x05);
            IOS_DWORD(large ? tail_entry : i ? second : first);
            IOS_DWORD((large ? tail_physical : policy == 14 ? i ? 0x2A000u : 0x28000u : i ? 0x26000u : 0x24000u) | 7u);
        }
        if (policy >= 11 && policy <= 13) {
            if (width32) IOS_BYTE(0x66);
            IOS_BYTE(0xBB); IOS_WORD(f.buffer);
            if (width32) IOS_BYTE(0x66);
            IOS_BYTE(0xB8); IOS_WORD(policy == 13 ? 8 : 9);
            if (width32) IOS_BYTE(0x66);
            IOS_BYTE(0xB9); IOS_WORD(policy == 13 ? 0 : width32 ? 0x40F2 : 0xF2);
            if (policy == 13) {
                if (width32) IOS_BYTE(0x66);
                IOS_BYTE(0xBA); IOS_WORD(0xFFFF);
            }
            IOS_BYTE(0xCD); IOS_BYTE(0x31);
        }
        if (policy == 14) {
            IOS_BYTE(0x26); IOS_OP32(); IOS_ADDR32(); IOS_BYTE(0x0F); IOS_BYTE(0xB7); IOS_BYTE(0x05); IOS_DWORD(0x90204);
            IOS_OP32(); IOS_BYTE(0xC1); IOS_BYTE(0xE0); IOS_BYTE(4);
            IOS_BYTE(0x26); IOS_ADDR32(); IOS_BYTE(0xC6); IOS_BYTE(0x00); IOS_BYTE(0xEE);
        }
        if (skip) *skip = (uint8_t)(out - begin);
        if (policy == 8 || policy == 20) {
            IOS_BYTE(0x36); IOS_ADDR32(); IOS_BYTE(0xC7); IOS_BYTE(0x45); IOS_BYTE(32u + 3u * width);
            if (width32) { IOS_DWORD(0x80); } else { IOS_WORD(0x80); }
        }
        /* Set the interrupted DF clear, even when the service later publishes
         * its status. Repeated faults do not toggle it back on. */
        IOS_BYTE(0x36); IOS_ADDR32(); IOS_BYTE(0x81); IOS_BYTE(0x65); IOS_BYTE(32u + 5u * width);
        if (width32) { IOS_DWORD(~(uint32_t)FLAG_DF); } else { IOS_WORD(~FLAG_DF); }
        IOS_OP32(); IOS_BYTE(0x61); IOS_BYTE(0xCB);
        IO_SERVICE_CHECK(out < memory + 0x20600);
        out = memory + 0x20600;
        IOS_BYTE(0x26); IOS_OP32(); IOS_ADDR32(); IOS_BYTE(0xFF); IOS_BYTE(0x05); IOS_DWORD(0x8004); IOS_BYTE(0xCB);
#undef IOS_ADDR32
#undef IOS_OP32
#undef IOS_DWORD
#undef IOS_WORD
#undef IOS_BYTE
        if (policy <= 2) dos_mem_write32(vm, second, 0x26000u | (policy == 0 ? 6 : policy == 1 ? 5 : 3));
        if (policy == 11) d->access = 0xF8;
        if (policy == 12) d->access &= ~DESC_PRESENT;
        if (policy == 13) dpmi_desc_set_limit(d, 15);
        cpu8086_state_t before = cpu;
        if (interpreted) (void)cpu8086_run_one(vm); else dos_int21_dispatch(vm);
        bool cancelled = policy == 8 || policy == 20, stopped = policy == 9 || policy == 10 || policy == 21;
        IO_SERVICE_CHECK(dos_mem_read16(vm, 0x90200) == (policy == 17 || policy == 18 || policy == 19 ? 2 : 1) &&
                         dos_mem_read32(vm, 0x8000) == (policy == 6 ? 2u : 1u));
        IO_SERVICE_CHECK(!vm->dpmi.host_wait && !vm->interpreter_stop_signal &&
                         !vm->software_int_frame_bytes && !vm->native_dispatch_depth);
        if (stopped) {
            IO_SERVICE_CHECK(!cpu.running && (policy == 10 ? vm->step_limit_reached : cpu.exit_code == 42));
        } else {
            uint32_t result = large ? count : policy == 16 ? 5 : kind == 2 ? 0x0924 : policy == 15 ? 2 : 32;
            IO_SERVICE_CHECK(cpu.running && !vm->dpmi.exception_depth && cpu.cs == f.code &&
                             cpu.eip == (cancelled ? 0x80u : 15u) && cpu.ss == before.ss && cpu.esp == before.esp);
            IO_SERVICE_CHECK(cpu.ax == (cancelled ? before.ax : result) && cpu.ebx == before.ebx &&
                             cpu.ecx == before.ecx && cpu.edx == before.edx && cpu.esi == before.esi &&
                             cpu.edi == before.edi && cpu.ebp == before.ebp && !(cpu.flags & FLAG_DF) &&
                             !!(cpu.flags & FLAG_CF) == (cancelled || policy == 16));
            IO_SERVICE_CHECK(!dos_mem_alloc(vm, 0xFFFF, &after_free) && before_free == after_free);
            if (!cancelled) recovered++;
        }
        unsigned vector = policy == 12 ? 11 : policy == 11 || policy == 13 ? 13 : 14;
        uint32_t error = policy == 11 || policy == 12 ? f.buffer & ~3u : policy == 13 ? 0 :
                         4u | (output ? 2u : 0u) | (policy == 1 || policy == 2 || policy == 4 || policy == 5);
        IO_SERVICE_CHECK(memory[0x8010] == vector && dos_mem_read16(vm, 0x8020) == error &&
                         dos_mem_read16(vm, 0x8024) == 15 && dos_mem_read16(vm, 0x8028) == before.sp &&
                         dos_mem_read16(vm, 0x802C) == before.ss);
        IO_SERVICE_CHECK(vector != 14 || cpu.cr2 == linear + (large ? capacity : policy == 15 ? 0u : 16u));
        bool unchanged = true;
        for (unsigned i = 0; i < 32; i++) {
            uint8_t initial = kind == 2 && i == 31 ? '$' : kind >= 3 && i < 2 ? i ? 0 : 30 : 0xA5;
            uint8_t value = i < 2 ? (uint8_t)((kind >= 3 ? 0x011Eu : 0x1234u) >> (i * 8u)) :
                            i >= 30 ? (uint8_t)(0x5678u >> ((i - 30u) * 8u)) : initial;
            /* Only AH=3Fh returns its byte count in AX. Line input has a
             * buffer result, even if a real hook leaves AX equal to two. */
            bool copied = !large && output && !cancelled && !stopped && (policy != 15 || kind != 0 || i < 2);
            uint8_t old_expected = copied && policy != 14 ? value : initial;
            uint8_t new_expected = copied && policy == 14 ? value : 0xC7;
            if (memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u] != old_expected ||
                memory[i < 16 ? 0x28FF0 + i : 0x2A000 + i - 16u] != new_expected) unchanged = false;
        }
        IO_SERVICE_CHECK(unchanged && dos_mem_read32(vm, 0x8004) == (policy == 7));
        if (policy == 17)
            IO_SERVICE_CHECK(dos_mem_read16(vm, 0x8030) == 1 && !(dos_mem_read16(vm, 0x8034) & FLAG_CF));
        if (large) {
            uint32_t completed = cancelled || stopped ? capacity : count;
            IO_SERVICE_CHECK(dos_mem_read16(vm, 0x90208) == completed &&
                             dos_mem_read16(vm, 0x8038) == (policy == 19 ? 2 : 1) &&
                             dos_mem_read16(vm, 0x803C) == (policy == 19 ? count : capacity));
            bool equal = true;
            for (unsigned i = 0; i < count; i++) {
                unsigned at = i < capacity ? i : i - capacity;
                uint8_t expected = output && i < completed && (at < 2 || (at >= 30 && at < 32)) ?
                    (uint8_t)((at < 2 ? 0x1234u : 0x5678u) >> ((at & 1u) * 8u)) : 0xA5;
                if (memory[0x30000 + (linear & 0xFFFu) + i] != expected) equal = false;
            }
            IO_SERVICE_CHECK(equal);
        }
    }
    serial_puts("[DPMI-IO-SERVICE] checks="); serial_putdec(checks);
    serial_puts(" recovered="); serial_putdec(recovered);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef IO_SERVICE_CHECK
    dos_host_free_pages(memory, memory_pages); dos_host_free_pages(vm, vm_pages);
    return failures;
}

static int dpmi_io_buffer_selftest(void)
{
    const unsigned memory_pages = 512, scratch_pages = 16;
    unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(memory_pages);
    uint8_t *scratch = dos_host_alloc_pages(scratch_pages);
    if (!vm || !memory || !scratch) {
        if (vm) dos_host_free_pages(vm, vm_pages);
        if (memory) dos_host_free_pages(memory, memory_pages);
        if (scratch) dos_host_free_pages(scratch, scratch_pages);
        return 1;
    }
    dpmi_zero(vm, vm_pages * 4096u);
    cpu8086_state_t cpu;
    vm->cpu = &cpu;
    vm->mem = memory;
    vm->total_mem_size = memory_pages * 4096u;
    unsigned checks = 0;
    int failures = 0;
#define IO_CHECK(condition) do { \
    if (!(condition)) { \
        if (failures < 16) { serial_puts("[DPMI-IO-BUFFER] failed check="); serial_putdec(checks); serial_puts("\n"); } \
        failures++; \
    } \
    checks++; \
} while (0)
    for (unsigned width = 0; width < 2; width++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned to_real = 0; to_real < 2; to_real++)
    for (unsigned policy = 0; policy < 14; policy++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width);
        dpmi_descriptor_t *d = &vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)];
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        uint32_t directory = high ? 0x10400 : 0x10000;
        uint32_t first = high ? 0x12004 : 0x11018;
        if (high) dpmi_desc_set_base(d, 0x40001000);
        bool accepted = policy == 0 || policy == 12 ||
                        (to_real && (policy == 1 || policy == 2 || policy == 11));
        switch (policy) {
        case 1: d->access = 0xF0; break;
        case 2: dos_mem_write32(vm, first + 4u, 0x26005); break;
        case 3: dos_mem_write32(vm, first + 4u, 0x26003); break;
        case 4: dos_mem_write32(vm, first + 4u, 0); break;
        case 5: dos_mem_write32(vm, first + 4u, vm->total_mem_size | 7u); break;
        case 6: dos_mem_write32(vm, directory, dos_mem_read32(vm, directory) & ~4u); break;
        case 7: dpmi_desc_set_limit(d, 0xFFF); break;
        case 8: d->access |= 4u; break;
        case 9: d->access &= ~DESC_PRESENT; break;
        case 10: d->access = 0xF8; break;
        case 11: d->access = 0x9E; break; /* readable conforming code, DPL 0 */
        case 12: d->access |= 4u; dpmi_desc_set_limit(d, 0xFEF); break;
        case 13: dpmi_desc_set_base(d, 0xFFFFF100); break;
        }
        for (unsigned i = 0; i < 32; i++) {
            memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u] = to_real ? (uint8_t)(i + 1u) : 0xA5;
            memory[0x90003 + i] = to_real ? 0xA5 : (uint8_t)(i + 1u);
            memory[0x6FF0 + i] = 0x6C;
        }
        uint32_t old_directory = dos_mem_read32(vm, directory);
        uint32_t old_first = dos_mem_read32(vm, first);
        IO_CHECK(dpmi_io_buffer_valid(vm, f.buffer, 0xFF0, 32, !to_real) == accepted);
        IO_CHECK(dos_mem_read32(vm, directory) == old_directory && dos_mem_read32(vm, first) == old_first);
        IO_CHECK(dpmi_copy_io_buffer(vm, f.buffer, 0xFF0, 0x90003, 32, to_real, scratch) == accepted);
        bool equal = true;
        for (unsigned i = 0; i < 32; i++) {
            uint32_t destination = to_real ? 0x90003 + i : i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u;
            if (memory[destination] != (accepted ? i + 1u : 0xA5u) || memory[0x6FF0 + i] != 0x6C) equal = false;
        }
        IO_CHECK(equal);
        IO_CHECK(dos_mem_read32(vm, directory) == (old_directory | (accepted ? 0x20u : 0u)) &&
                  dos_mem_read32(vm, first) == (old_first | (accepted ? to_real ? 0x20u : 0x60u : 0u)));
    }

    /* Whole chunks can overlap, including a permutation of seventeen pages. */
    for (unsigned to_real = 0; to_real < 2; to_real++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, 1);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        dpmi_desc_set_base(&vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)], 0x40000003);
        for (unsigned p = 0; p < 17; p++)
            dos_mem_write32(vm, 0x12000 + p * 4u, (0x40000 + ((p + 1u) % 17u) * 4096u) | 7u);
        for (unsigned i = 0; i < 0x10000; i++) {
            uint32_t linear = i + 3u;
            uint32_t physical = 0x40000 + ((linear / 4096u + 1u) % 17u) * 4096u + (linear & 0xFFFu);
            memory[to_real ? physical : 0x40003 + i] = (uint8_t)(i * 13u + 0x57u);
        }
        IO_CHECK(dpmi_copy_io_buffer(vm, f.buffer, 0, 0x40003, 0x10000, to_real, scratch));
        bool equal = true;
        for (unsigned i = 0; i < 0x10000; i++) {
            uint32_t linear = i + 3u;
            uint32_t physical = 0x40000 + ((linear / 4096u + 1u) % 17u) * 4096u + (linear & 0xFFFu);
            if (memory[to_real ? 0x40003 + i : physical] != (uint8_t)(i * 13u + 0x57u)) equal = false;
        }
        IO_CHECK(equal);
    }
    /* Payload stores may replace the PTE needed for a later page in this copy. */
    for (unsigned to_real = 0; to_real < 2; to_real++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, 1);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        dpmi_desc_set_base(&vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)], 0x40000000);
        dos_mem_write32(vm, 0x12000, (to_real ? 0x50000u : 0x12000u) | 7u);
        dos_mem_write32(vm, 0x12004, 0x60007);
        for (unsigned i = 0; i < 0x1010; i++)
            memory[to_real && i >= 4096 ? 0x60000 + i - 4096u : 0x50000 + i] = (uint8_t)(i * 7u + 0x31u);
        IO_CHECK(dpmi_copy_io_buffer(vm, f.buffer, 0, to_real ? 0x12000 : 0x50000, 0x1010, to_real, scratch));
        bool equal = true;
        for (unsigned i = 0; i < 0x1010; i++)
            if (memory[!to_real && i >= 4096 ? 0x60000 + i - 4096u : 0x12000 + i] != (uint8_t)(i * 7u + 0x31u)) equal = false;
        IO_CHECK(equal);
    }

    for (unsigned absent = 0; absent < 2; absent++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, 1);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)], 0x40000003, 0x30000, 0xF2, DESC_32BIT);
        for (unsigned p = 0; p < 33; p++)
            dos_mem_write32(vm, 0x12000 + p * 4u, absent && p == 32 ? 0 : (0x40000 + p * 4096u) | 7u);
        IO_CHECK(dpmi_io_buffer_valid(vm, f.buffer, 0, 0x20023, true) == !absent);
        IO_CHECK(dos_mem_read32(vm, 0x10400) == 0x12007 && dos_mem_read32(vm, 0x12000) == 0x40007);
        IO_CHECK(dpmi_io_buffer_valid(vm, 0, UINT32_MAX, 0, true) &&
                  dpmi_copy_io_buffer(vm, 0, UINT32_MAX, UINT32_MAX, 0, false, NULL));
        IO_CHECK(!dpmi_copy_io_buffer(vm, f.buffer, 0, 0x90000, 0x10001, false, scratch) &&
                  !dpmi_io_buffer_valid(vm, f.buffer, 0x2FFFF, 3, true));
    }

    for (unsigned width = 0; width < 2; width++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned scenario = 0; scenario < 10; scenario++) {
        vm->total_mem_size = memory_pages * 4096u;
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        dpmi_descriptor_t *d = &vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)];
        if (high) dpmi_desc_set_base(d, 0x40001000);
        uint32_t first = high ? 0x12004 : 0x11018;
        uint32_t offset = 0xFF0, expected = 19;
        for (unsigned i = 0; i < 32; i++) memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u] = 'Q';
        memory[0x26002] = '$';
        if (scenario == 0) dpmi_desc_set_limit(d, 0x1002);
        if (scenario == 1) { memory[0x24FFF] = '$'; dos_mem_write32(vm, first + 4u, 0); expected = 16; }
        if (scenario == 2) { dos_mem_write32(vm, first + 4u, 0); expected = 0; }
        if (scenario == 3) { dpmi_desc_set_limit(d, 0x1001); expected = 0; }
        if (scenario == 4) d->access = 0xF0;
        if (scenario == 5) { dos_mem_write32(vm, first, 0x24003); expected = 0; }
        if (scenario == 6) d->access = 0x9E;
        if (scenario == 7) {
            offset = width ? 0x1FFFFu : 0xFFFFu;
            dpmi_build_desc(d, width ? 0x70000 : 0x80000, 0x20000, 0xF0, width ? DESC_32BIT : 0);
            memory[0x8FFFF] = '$'; expected = 1;
        }
        if (scenario >= 8) {
            dos_mem_write32(vm, first, 0x1FF007);
            vm->total_mem_size -= 13;
            memory[0x1FFFF0] = memory[0x1FFFF1] = 'Q';
            memory[0x1FFFF2] = scenario == 8 ? '$' : 'Q';
            expected = scenario == 8 ? 3u : 0u;
        }
        uint32_t size = 0xDEADBEEF;
        IO_CHECK(dpmi_dos_string_size(vm, f.buffer, offset, &size) == (expected != 0));
        IO_CHECK(size == (expected ? expected : 0xDEADBEEFu));
        if (scenario == 7 && !width) {
            memory[0x8FFFF] = 'Q'; memory[0x90000] = '$';
            IO_CHECK(!dpmi_dos_string_size(vm, f.buffer, offset, &size));
        }
    }
    vm->total_mem_size = memory_pages * 4096u;
    /* Real INT 21h hooks run between the protected copy-in and copy-out.
     * Their page-table edits must be observed without changing the source
     * cursor or copying a short read's untouched tail to a new mapping. */
    for (unsigned width = 0; width < 2; width++)
    for (unsigned high = 0; high < 2; high++)
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width);
        dpmi_descriptor_t *d = &vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)];
        if (high) dpmi_desc_set_base(d, 0x40001000);
        dos_mem_init(vm);
        vm->current_psp = 0x50;
        uint16_t before = 0, after = 0;
        IO_CHECK(!dos_mem_alloc(vm, 0xFFFF, &before) && before);
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cpl = 3;
        cpu.cs = f.code; cpu.ss = f.stack; cpu.ds = f.buffer; cpu.esp = 0x8000;
        cpu8086_sync_cs(&cpu);
        cpu8086_sync_data(&cpu);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        cpu.eax = 0xA5A53F00; cpu.ecx = width ? 32u : 0xABCD0020u;
        cpu.edx = width ? 0xFF0u : 0xABCD0FF0u;
        for (unsigned i = 0; i < 32; i++) {
            memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u] = 0xA5;
            memory[i < 16 ? 0x28FF0 + i : 0x2A000 + i - 16u] = 0xC7;
        }
        uint32_t first = high ? 0x12004 : 0x11018;
        if (scenario == 2) dos_mem_write32(vm, first + 4u, 0);
        dos_mem_write32(vm, 0x21 * 4u, 0x90000100);
        const uint8_t hook[] = { 0x2E, 0xFF, 0x06, 0, 2, 0x89, 0xD3, 0xC7, 7, 0x34, 0x12 };
        unsigned n = 0;
        for (unsigned i = 0; i < sizeof(hook); i++) memory[0x90100 + n++] = hook[i];
        if (scenario >= 3) {
            const uint8_t remap[] = { 0x1E, 0xB8, 0, 0, 0x8E, 0xD8,
                0x66, 0xC7, 0x06, 0, 0, 7, 0x80, 2, 0,
                0x66, 0xC7, 0x06, 0, 0, 7, 0xA0, 2, 0, 0x1F };
            uint32_t start = 0x90100 + n;
            for (unsigned i = 0; i < sizeof(remap); i++) memory[0x90100 + n++] = remap[i];
            dos_mem_write16(vm, start + 2, (uint16_t)(first >> 4));
            dos_mem_write16(vm, start + 9, (uint16_t)(first & 15u));
            dos_mem_write16(vm, start + 18, (uint16_t)((first & 15u) + 4u));
            if (scenario == 4) memory[start + 20] = 5; /* newly read-only tail */
        }
        const uint8_t result[] = { 0x55, 0x89, 0xE5, 0x83, 0x66, 6, 0xFE, 0x5D, 0xB8, 2, 0, 0xCF };
        uint32_t end = 0x90100 + n;
        for (unsigned i = 0; i < sizeof(result); i++) memory[0x90100 + n++] = result[i];
        if (scenario == 1) { memory[end + 4] = 0x4E; memory[end + 6] = 1; memory[end + 9] = 5; }
        if (scenario == 4) memory[end + 9] = 32;
        uint16_t error = dpmi_dos_file_io(vm, 0);
        bool stopped = scenario == 2 || scenario == 4;
        IO_CHECK(error == 0);
        IO_CHECK(dos_mem_read16(vm, 0x90200) == (scenario == 2 ? 0 : 1) &&
                  (stopped ? !cpu.running : cpu.running && cpu.protected_mode &&
                   cpu.cr3 == 0x10018 && cpu.esp == 0x8000));
        IO_CHECK(dos_mem_read16(vm, 0x24FF0) == (scenario < 2 ? 0x1234 : 0xA5A5) &&
                  dos_mem_read16(vm, 0x28FF0) == (scenario == 3 ? 0x1234 : 0xC7C7));
        bool unchanged_tail = true;
        for (unsigned i = 2; i < 32; i++)
            if (memory[i < 16 ? 0x24FF0 + i : 0x26000 + i - 16u] != 0xA5 ||
                memory[i < 16 ? 0x28FF0 + i : 0x2A000 + i - 16u] != 0xC7) unchanged_tail = false;
        IO_CHECK(unchanged_tail && !dos_mem_alloc(vm, 0xFFFF, &after) && before == after);
        if (!stopped) IO_CHECK(cpu.ax == (scenario == 1 ? 5 : 2) &&
                             ((cpu.flags & FLAG_CF) != 0) == (scenario == 1));
    }
    for (unsigned width = 0; width < 2; width++)
    for (unsigned flush = 0; flush < 2; flush++)
    for (unsigned nonzero = 0; nonzero < 2; nonzero++) {
        dpmi_paging_fixture_t f = dpmi_paging_test_prepare(vm, width);
        uint32_t offset = width ? 0x1FFFF : 0xFFFF;
        dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(f.buffer)],
            width ? 0x70000 : 0x80000, offset, 0xF0, width ? DESC_32BIT : 0);
        dos_mem_init(vm);
        vm->current_psp = 0x50;
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cpl = 3; cpu.cs = f.code; cpu.ss = f.stack; cpu.ds = f.buffer; cpu.esp = 0x8000;
        cpu8086_sync_cs(&cpu);
        cpu8086_sync_data(&cpu);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        cpu.ax = flush ? 0x0C0A : 0x0A00;
        cpu.edx = width ? offset : 0xABCD0000u | offset;
        memory[0x8FFFF] = nonzero ? 8 : 0;
        memory[0x90000] = 0xC7;
        const uint8_t hook[] = { 0x2E, 0xFF, 0x06, 0, 2, 0xCF };
        for (unsigned i = 0; i < sizeof(hook); i++) memory[0x90100 + i] = hook[i];
        dos_mem_write32(vm, 0x21 * 4u, 0x90000100);
        IO_CHECK(dpmi_dos_console(vm) == 0 && cpu.running == !nonzero);
        IO_CHECK(dos_mem_read16(vm, 0x90200) == (nonzero ? 0 : 1) &&
                  memory[0x8FFFF] == (nonzero ? 8 : 0) && memory[0x90000] == 0xC7 &&
                  cpu.edx == (width ? offset : 0xABCD0000u | offset));
    }
    serial_puts("[DPMI-IO-BUFFER] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
#undef IO_CHECK
    dos_host_free_pages(scratch, scratch_pages);
    dos_host_free_pages(memory, memory_pages);
    dos_host_free_pages(vm, vm_pages);
    return failures + dpmi_io_service_selftest();
}

static int dpmi_paging_reentry_selftest(void)
{
    const uint64_t memory_pages = 512;
    uint64_t vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = (dos_vm_t *)dos_host_alloc_pages(vm_pages);
    uint8_t *memory = (uint8_t *)dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (memory) dos_host_free_pages(memory, memory_pages);
        if (vm) dos_host_free_pages(vm, vm_pages);
        return 1;
    }
    dpmi_zero(vm, vm_pages * 4096u);
    cpu8086_state_t cpu;
    vm->cpu = &cpu;
    vm->mem = memory;
    vm->total_mem_size = memory_pages * 4096u;
    unsigned checks = 0;
    int failures = 0;
#define REENTRY_CHECK(condition) do { \
    if (!(condition)) { \
        if (failures < 12) { serial_puts("[DPMI-PAGING-STATE] failed check="); serial_putdec(checks); serial_puts("\n"); } \
        failures++; \
    } \
    checks++; \
} while (0)
    for (unsigned width = 0; width < 2; width++)
    for (unsigned context = 0; context < 2; context++)
    for (unsigned high_buffer = 0; high_buffer < 2; high_buffer++)
    for (unsigned scenario = 0; scenario < 6; scenario++) {
        dpmi_paging_fixture_t fixture = dpmi_paging_test_prepare(vm, width);
        uint32_t directory = 0x10000u + context * 0x4000u;
        uint32_t physical = 0x20000u + context * 0x8000u;
        uint32_t buffer_entry = directory + (high_buffer ? 0x2008u : 0x101Cu);
        vm->dpmi.suspended_paging.cr3 = directory | 0x18u;
        if (high_buffer) dpmi_desc_set_base(&vm->dpmi.ldt[dpmi_sel_to_index(fixture.buffer)], 0x40001000);
        uint16_t alias = dpmi_alloc_descriptor(&vm->dpmi);
        dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(alias)], 0, 0xFFFF, 0xF2, 0);
        vm->dpmi.descriptor_state[dpmi_sel_to_index(alias)] = DPMI_DESC_HOST;
        vm->dpmi.callbacks[0] = (dpmi_callback_t){
            .rm_seg = DPMI_ENTRY_SEG, .rm_off = DPMI_CALLBACK_BASE_OFF,
            .pm_sel = fixture.code, .rm_regs_sel = fixture.buffer,
            .rm_regs_off = 0xFF0, .rm_stack_sel = alias, .active = true
        };
        memory[dos_linear(DPMI_ENTRY_SEG, DPMI_CALLBACK_BASE_OFF + 2)] = 0;
        cpu.cs = DPMI_ENTRY_SEG;
        cpu.eip = DPMI_CALLBACK_BASE_OFF + 2;
        dos_mem_write16(vm, 0x8000, 0x1234);
        dos_mem_write16(vm, 0x8002, 0x2345);
        dos_mem_write16(vm, 0x8004, FLAGS_FIXED | FLAG_CF | FLAG_IF);
        for (unsigned i = 0; i < sizeof(dpmi_rm_regs_t); i++) {
            memory[physical + (i < 16 ? 0x4FF0u + i : 0x6000u + i - 16u)] = 0xA5;
            memory[0x6FF0 + i] = 0x6C;
        }
        if (scenario == 1) dos_mem_write32(vm, directory + 0x2000, physical | 6u);
        if (scenario == 2) dos_mem_write32(vm, directory + 0x2000, physical | 3u);
        if (scenario >= 3) dos_mem_write32(vm, buffer_entry,
            dos_mem_read32(vm, buffer_entry) & ~(1u << (scenario - 3u)));
        cpu8086_state_t original = cpu;
        dpmi_paging_t original_real = vm->dpmi.real_mode_paging;
        dpmi_service_result_t result = dpmi_callback_enter(vm);
        bool entered = result == DPMI_SERVICE_COMPLETE;
        REENTRY_CHECK(result == (scenario ? DPMI_SERVICE_INTERRUPTED : DPMI_SERVICE_COMPLETE));
        if (entered) {
            REENTRY_CHECK(cpu.protected_mode && cpu.cr0 == vm->dpmi.suspended_paging.cr0 &&
                          cpu.cr3 == (directory | 0x18u) && vm->dpmi.callback_depth == 1 &&
                          vm->dpmi.real_mode_paging.cr0 == original.cr0 &&
                          vm->dpmi.real_mode_paging.cr3 == original.cr3);
            REENTRY_CHECK(cpu8086_run_one(vm) && cpu.eip == 1);
            dpmi_rm_regs_t regs;
            for (unsigned i = 0; i < sizeof(regs); i++)
                ((uint8_t *)&regs)[i] = memory[physical + (i < 16 ? 0x4FF0u + i : 0x6000u + i - 16u)];
            REENTRY_CHECK(regs.ip == 0x1234 && regs.cs == 0x2345 && regs.sp == 0x8006 &&
                          regs.ss == 0 && regs.eax == original.eax);
            cpu.cs = vm->dpmi.sel_host_code;
            cpu.eip = DPMI_CALLBACK_RETURN_OFF + 2;
            REENTRY_CHECK(dpmi_callback_return(vm, false) == DPMI_SERVICE_COMPLETE && !cpu.protected_mode &&
                          cpu.cr0 == original.cr0 && cpu.cr3 == original.cr3 &&
                          cpu.cs == 0x2345 && cpu.eip == 0x1234 && !vm->dpmi.callback_depth);
        } else {
            /* With no real exception handler, failed admission terminates
             * the client after consuming the private INT frame once. */
            original.running = false;
            original.exit_code = -1;
            original.esp += 6u;
            original.flags = FLAGS_FIXED | FLAG_CF | FLAG_IF;
            original.cr2 = scenario < 3 ? 0x40000000u : high_buffer ? 0x40002000u : 0x7000u;
            bool unchanged = true;
            for (unsigned i = 0; i < sizeof(cpu); i++)
                if (((uint8_t *)&cpu)[i] != ((uint8_t *)&original)[i]) unchanged = false;
            for (unsigned i = 0; i < sizeof(dpmi_rm_regs_t); i++)
                if (memory[physical + (i < 16 ? 0x4FF0u + i : 0x6000u + i - 16u)] != 0xA5) unchanged = false;
            REENTRY_CHECK(unchanged && !vm->dpmi.callback_depth);
        }
        bool untouched_alias = true;
        for (unsigned i = 0; i < sizeof(dpmi_rm_regs_t); i++)
            if (memory[0x6FF0 + i] != 0x6C) untouched_alias = false;
        REENTRY_CHECK(untouched_alias && vm->dpmi.real_mode_paging.cr0 == original_real.cr0 &&
                      vm->dpmi.real_mode_paging.cr3 == original_real.cr3);
    }

    for (unsigned width = 0; width < 2; width++)
    for (unsigned context = 0; context < 2; context++)
    for (unsigned kind = 0; kind < 3; kind++)
    for (unsigned denied = 0; denied < 3; denied++) {
        dpmi_paging_fixture_t fixture = dpmi_paging_test_prepare(vm, width);
        uint32_t directory = 0x10000u + context * 0x4000u;
        uint32_t physical = 0x20000u + context * 0x8000u;
        vm->dpmi.suspended_paging.cr3 = directory | 0x18u;
        /* A legal data-segment store proves which mapped handler ran. */
        const uint8_t code16[] = { 0xB8, 0, 0, 0x8E, 0xD8,
            0x66, 0xC7, 0x06, 0, 0x30, 0xFE, 0xCA, 0, 0, 0xB0, 1, 0xCF };
        const uint8_t code32[] = { 0x66, 0xB8, 0, 0, 0x8E, 0xD8,
            0xC7, 0x05, 0, 0x30, 0, 0, 0xFE, 0xCA, 0, 0, 0xB0, 1, 0xCF };
        unsigned size = width ? sizeof(code32) : sizeof(code16);
        for (unsigned i = 0; i < size; i++) memory[physical + i] = width ? code32[i] : code16[i];
        dos_mem_write16(vm, physical + (width ? 2u : 1u), fixture.data);
        unsigned vector = kind == 0 ? 0x1C : kind == 1 ? 0x23 : 0x24;
        vm->dpmi.pm_vectors[vector].sel = fixture.code;
        vm->dpmi.pm_vectors[vector].off = 0;
        if (denied) dos_mem_write32(vm, directory + 0x2000, physical | (denied == 1 ? 6u : 3u));
        cpu8086_state_t original = cpu;
        dpmi_paging_t original_real = vm->dpmi.real_mode_paging;
        unsigned result = kind == 0 ? dpmi_reflect_timer(vm) : kind == 1 ? dpmi_control_break(vm)
                                                                                   : dpmi_critical_error(vm);
        REENTRY_CHECK(result == (denied ? kind == 2 ? 3u : 0u : 1u));
        REENTRY_CHECK(!cpu.protected_mode && cpu.cr0 == original.cr0 && cpu.cr3 == original.cr3 &&
                      cpu.ss == original.ss && cpu.esp == original.esp && !vm->dpmi.control_depth &&
                      vm->dpmi.real_mode_paging.cr0 == original_real.cr0 &&
                      vm->dpmi.real_mode_paging.cr3 == original_real.cr3);
        REENTRY_CHECK(dos_mem_read32(vm, 0x3000) == (denied ? 0u : 0xCAFEu) &&
                      (denied ? cpu.insn_count == 0 : cpu.insn_count > 0));
    }

    for (unsigned width = 0; width < 2; width++)
    for (unsigned stack32 = 0; stack32 < 2; stack32++)
    for (unsigned scenario = 0; scenario < 9; scenario++) {
        dpmi_paging_fixture_t fixture = dpmi_paging_test_prepare(vm, width);
        vm->dpmi.pm_vectors[0x24].sel = fixture.code;
        vm->dpmi.pm_vectors[0x24].off = 0;
        memory[0x20000] = 0xB0; memory[0x20001] = 1; memory[0x20002] = 0xCF;
        vm->dpmi.control_depth = 1;
        vm->dpmi.suspended_stack = (dpmi_stack_t){ fixture.stack, 0x8004 };
        cpu8086_load_real_segment(&cpu, 2, 0x2000);
        dpmi_desc_set_base(&cpu.ss_cache.descriptor, 0x40000);
        if (stack32) cpu.ss_cache.descriptor.flags_lim |= DESC_32BIT;
        dpmi_desc_set_limit(&cpu.ss_cache.descriptor, stack32 ? 0x1FFFF : 0xFFFF);
        cpu.esp = stack32 ? 0x10FF8 : 0xABCD0FF8;
        uint32_t linear = 0x40000 + cpu_stack_offset(&cpu);
        uint32_t entry = 0x19000 + (linear >> 12) * 4u;
        dos_mem_write32(vm, 0x18000, 0x19007);
        for (unsigned i = 0; i < 512; i++) dos_mem_write32(vm, 0x19000 + i * 4u, i * 4096u | 7u);
        dos_mem_write32(vm, entry, 0x80007);
        dos_mem_write32(vm, entry + 4, 0x82007);
        cpu.cr0 |= DOS_CR0_PG;
        cpu.cr3 = 0x18008;
        for (unsigned i = 0; i < 15; i++) {
            uint32_t physical = i < 4 ? 0x80FF8 + i * 2u : 0x82000 + (i - 4u) * 2u;
            dos_mem_write16(vm, physical, i == 2 ? FLAGS_FIXED | FLAG_IF : 0x5100 + i);
        }
        /* The PM destination straddles two separately mapped pages. Denial
         * of the second page must not leave a partial frame in the first. */
        for (unsigned i = 0; i < 36; i++) {
            uint32_t at = 0x8004 - 36u + i;
            memory[at < 0x8000 ? 0x26000 + (at & 0xFFF) : at] = 0xA5;
        }
        if (scenario == 1) dos_mem_write32(vm, entry + 4, 0x82006);
        if (scenario == 2) dpmi_desc_set_limit(&cpu.ss_cache.descriptor, cpu_stack_offset(&cpu) + 27u);
        if (scenario == 3) cpu.ss_cache.valid = false;
        if (scenario >= 4 && scenario <= 6)
            dos_mem_write32(vm, 0x11020, 0x8000 | (scenario == 4 ? 6u : scenario == 5 ? 5u : 3u));
        if (scenario == 7) dos_mem_write32(vm, entry + 4, vm->total_mem_size | 7u);
        /* Read-only real source pages are legal; only the PM frame needs RW. */
        if (scenario == 8) dos_mem_write32(vm, entry + 4, 0x82005);
        cpu8086_state_t original = cpu;
        unsigned result = dpmi_critical_error(vm);
        bool valid = scenario == 0 || scenario == 8;
        REENTRY_CHECK(result == (valid ? 1u : 3u));
        REENTRY_CHECK(cpu.cr0 == original.cr0 && cpu.cr3 == original.cr3 &&
                      cpu.cr2 == original.cr2 && cpu.esp == original.esp && cpu.ss == original.ss &&
                      !cpu.protected_mode && !cpu.delivery_fault && vm->dpmi.control_depth == 1);
        bool intact = true;
        for (unsigned i = 0; i < 24; i++) {
            uint32_t at = 0x8004 - 24u + i;
            uint8_t expected = valid ? (uint8_t)((0x5103u + i / 2u) >> ((i & 1u) * 8u)) : 0xA5;
            if (memory[at < 0x8000 ? 0x26000 + (at & 0xFFF) : at] != expected) intact = false;
        }
        REENTRY_CHECK(intact && (valid ? cpu.insn_count > 0 : cpu.insn_count == 0));
    }

    for (unsigned width = 0; width < 2; width++) {
        dpmi_paging_fixture_t fixture = dpmi_paging_test_prepare(vm, width);
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cs = fixture.code;
        cpu.ss = fixture.stack;
        cpu.esp = 0x9000;
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        dpmi_paging_t first = dpmi_current_paging(&cpu);
        dpmi_paging_t real = vm->dpmi.real_mode_paging;
        for (unsigned trip = 0; trip < 2; trip++) {
            if (trip) dpmi_apply_paging(&cpu, (dpmi_paging_t){ DOS_CR0_PG | 0x11u, 0x14008 });
            cpu.ax = cpu.cx = cpu.dx = 0;
            cpu.bx = 0x8000; cpu.si = 0x3000; cpu.di = 0x100;
            REENTRY_CHECK(dpmi_raw_mode_switch(vm, 0) && !cpu.protected_mode &&
                          cpu.cr0 == real.cr0 && cpu.cr3 == real.cr3);
            memory[0x30100] = 0x90;
            REENTRY_CHECK(cpu8086_run_one(vm) && cpu.eip == 0x101);
            cpu.es = 0; cpu.edi = 0x5000; cpu.al = trip;
            cpu8086_state_t before = cpu;
            REENTRY_CHECK(dpmi_save_restore_state(vm) == DPMI_SERVICE_COMPLETE &&
                          cpu.cr0 == before.cr0 && cpu.cr3 == before.cr3 &&
                          vm->dpmi.suspended_paging.cr0 == first.cr0 && vm->dpmi.suspended_paging.cr3 == first.cr3);
            cpu.ax = cpu.cx = fixture.data; cpu.dx = fixture.stack;
            cpu.ebx = 0x9000; cpu.si = fixture.code; cpu.edi = 0;
            REENTRY_CHECK(dpmi_raw_mode_switch(vm, 0) && cpu.protected_mode &&
                          cpu.cr0 == first.cr0 && cpu.cr3 == first.cr3);
            REENTRY_CHECK(cpu8086_run_one(vm) && cpu.eip == 1);
        }
    }

    /* Ordinary 0301h owns its save/restore automatically, including a
     * protected callback which makes another real-mode FAR call. */
    for (unsigned width = 0; width < 2; width++)
    for (unsigned context = 0; context < 2; context++) {
        dpmi_paging_fixture_t fixture = dpmi_paging_test_prepare(vm, width);
        uint32_t directory = 0x10000u + context * 0x4000u;
        uint32_t physical = 0x20000u + context * 0x8000u;
        dpmi_desc_set_base(&vm->dpmi.ldt[dpmi_sel_to_index(fixture.buffer)], 0x40001000);
        vm->dpmi.ldt[dpmi_sel_to_index(fixture.code)].flags_lim &= (uint8_t)~DESC_32BIT;
        const uint8_t nested[] = { 0x06, 0x1E, 0x66, 0x60, /* ES,DS,PUSHAD */
            0xB8, 0, 0, 0x8E, 0xC0, 0x66, 0xBF, 0, 0x42, 0, 0,
            0xB8, 1, 3, 0x31, 0xC9, 0x31, 0xDB, 0xCD, 0x31,
            0x66, 0x61, 0x1F, 0x07 }; /* POPAD,DS,ES */
        const uint8_t callback[] = {
            0x8B, 0x04, 0x26, 0x89, 0x45, 0x2A,
            0x8B, 0x44, 0x02, 0x26, 0x89, 0x45, 0x2C,
            0x26, 0x83, 0x45, 0x2E, 4,
            0x26, 0xC7, 0x45, 0x1C, 0xFE, 0xCA
        };
        unsigned pos = 0;
        for (unsigned i = 0; i < sizeof(nested); i++) memory[physical + pos++] = nested[i];
        dos_mem_write16(vm, physical + 5, fixture.data);
        for (unsigned i = 0; i < sizeof(callback); i++) memory[physical + pos++] = callback[i];
        if (width) memory[physical + pos++] = 0x66;
        memory[physical + pos++] = 0xCF;
        memory[0x30100] = 0xB8; memory[0x30101] = 0x34; memory[0x30102] = 0x12; memory[0x30103] = 0xCB;
        dpmi_rm_regs_t leaf = { .cs = 0x3000, .ip = 0x100, .flags = FLAGS_FIXED };
        dpmi_write_rm_regs(vm, 0x4200, &leaf);
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        dpmi_apply_paging(&cpu, (dpmi_paging_t){ DOS_CR0_PG | DOS_CR0_WP | 0x11u, directory | 0x18u });
        cpu.cs = fixture.code; cpu.ss = fixture.stack; cpu.esp = 0x9000;
        cpu.ax = 0x0303; cpu.ds = fixture.code; cpu.esi = 0;
        cpu.es = fixture.buffer; cpu.edi = 0xFF0;
        dos_int31_dpmi(vm);
        REENTRY_CHECK(!(cpu.eflags & FLAG_CF));
        dpmi_rm_regs_t outer = { .cs = cpu.cx, .ip = cpu.dx, .flags = FLAGS_FIXED };
        dpmi_write_rm_regs(vm, 0x4000, &outer);
        cpu.ax = 0x0301; cpu.bx = cpu.cx = 0; cpu.es = fixture.data; cpu.edi = 0x4000;
        dpmi_paging_t current = dpmi_current_paging(&cpu);
        vm->dpmi.suspended_paging = (dpmi_paging_t){ 0x11, 0x1B000 };
        dpmi_paging_t previous = vm->dpmi.suspended_paging, real = vm->dpmi.real_mode_paging;
        dos_int31_dpmi(vm);
        dpmi_read_rm_regs(vm, 0x4000, &outer);
        dpmi_read_rm_regs(vm, 0x4200, &leaf);
        REENTRY_CHECK(!(cpu.eflags & FLAG_CF) && cpu.running && !vm->step_limit_reached &&
                      (uint16_t)outer.eax == 0xCAFE && (uint16_t)leaf.eax == 0x1234);
        REENTRY_CHECK(cpu.protected_mode && cpu.cr0 == current.cr0 && cpu.cr3 == current.cr3 &&
                      vm->dpmi.suspended_paging.cr0 == previous.cr0 && vm->dpmi.suspended_paging.cr3 == previous.cr3 &&
                      vm->dpmi.real_mode_paging.cr0 == real.cr0 && vm->dpmi.real_mode_paging.cr3 == real.cr3 &&
                      !vm->dpmi.callback_depth && !vm->dpmi.control_depth);
    }
    serial_puts("[DPMI-PAGING-STATE] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec((uint64_t)failures); serial_puts("\n");
#undef REENTRY_CHECK
    dos_host_free_pages(memory, memory_pages);
    dos_host_free_pages(vm, vm_pages);
    return failures;
}

static int dpmi_rm_arguments_selftest(void)
{
    const unsigned memory_pages = 512;
    unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (vm) dos_host_free_pages(vm, vm_pages);
        if (memory) dos_host_free_pages(memory, memory_pages);
        return 1;
    }
    dpmi_zero(vm, vm_pages * 4096u);
    cpu8086_state_t cpu;
    vm->cpu = &cpu;
    vm->mem = memory;
    vm->total_mem_size = memory_pages * 4096u;
    unsigned checks = 0;
    int failures = 0;
#define ARG_CHECK(condition) do { \
    if (!(condition)) { \
        if (failures < 16) { serial_puts("[DPMI-RM-ARGUMENTS] failed check="); serial_putdec(checks); serial_puts("\n"); } \
        failures++; \
    } \
    checks++; \
} while (0)
    const unsigned lengths[] = { 0, 2, 0x2010, 0xFFF8 };
    for (unsigned width = 0; width < 2; width++)
    for (unsigned kind = 0; kind < 3; kind++)
    for (unsigned size_case = 0; size_case < 4; size_case++)
    for (unsigned alias = 0; alias < 2; alias++) {
        dpmi_paging_fixture_t fixture = dpmi_paging_test_prepare(vm, width);
        unsigned size = lengths[size_case];
        unsigned frame = kind == DPMI_RM_CALL_FAR ? 4u : 6u;
        uint32_t offset = size_case == 3 ? 0u : 0xFF0u;
        dpmi_descriptor_t *ss = &vm->dpmi.ldt[dpmi_sel_to_index(fixture.stack)];
        dpmi_desc_set_base(ss, 0x40000003);
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cpl = 3;
        cpu.cs = fixture.code;
        cpu.ss = fixture.stack;
        cpu.esp = width ? offset : 0xABCD0000u | offset;
        cpu8086_sync_cs(&cpu);
        cpu8086_sync_data(&cpu);
        /* A loaded SS remains authoritative even if its LDT entry changes. */
        dpmi_desc_set_base(ss, 0xDEAD0000);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        vm->dpmi.real_mode_paging = (dpmi_paging_t){ DOS_CR0_PG | DOS_CR0_WP | 0x10u, 0x14018 };
        uint32_t destination_pages[17];
        for (unsigned page = 0; page < 17; page++) {
            dos_mem_write32(vm, 0x12000 + page * 4u, (0x40000 + page * 0x2000) | 7u);
            destination_pages[page] = alias ? 0x40000 + ((page + 1u) % 17u) * 0x2000 :
                                             0x100000 + page * 0x2000;
            dos_mem_write32(vm, 0x15000 + (0x60 + page) * 4u, destination_pages[page] | 7u);
        }
        for (unsigned i = 0; i < size; i++) {
            uint32_t linear = 3u + offset + i;
            memory[0x40000 + (linear / 4096u) * 0x2000 + (linear & 0xFFFu)] = (uint8_t)(i * 13u + 0x57u);
        }
        dpmi_rm_regs_t regs = { .flags = FLAGS_FIXED, .ss = 0x6001,
            .sp = size_case == 3 ? 0xFFFE : 0x3000, .cs = 0x9000, .ip = 0x100 };
        uint32_t argument_linear = 0x10u + regs.sp - size;
        /* Return AX and the observed entry SP, without removing arguments. */
        const uint8_t program[] = { 0x89, 0xE3, 0xB8, 0xFE, 0xCA, 0xCB };
        for (unsigned i = 0; i < sizeof(program); i++) memory[0x90100 + i] = program[i];
        memory[0x90105] = kind == DPMI_RM_CALL_FAR ? 0xCB : 0xCF;
        dos_mem_write32(vm, 0x60 * 4u, 0x90000100);
        cpu8086_state_t before = cpu;
        uint16_t result = dpmi_simulate_rm_call(vm, &regs, size / 2u, kind, 0x60);
        ARG_CHECK(!result && (uint16_t)regs.eax == 0xCAFE &&
                  (uint16_t)regs.ebx == (uint16_t)(regs.sp - size - frame));
        ARG_CHECK(cpu.running && cpu.protected_mode && cpu.esp == before.esp &&
                  cpu.ss == before.ss && cpu.cr0 == before.cr0 && cpu.cr3 == before.cr3 &&
                  dpmi_desc_get_base(&cpu.ss_cache.descriptor) == 0x40000003);
        bool equal = true;
        for (unsigned i = 0; i < size; i++) {
            uint32_t linear = argument_linear + i;
            if (memory[destination_pages[linear / 4096u] + (linear & 0xFFFu)] != (uint8_t)(i * 13u + 0x57u))
                equal = false;
        }
        ARG_CHECK(equal);
        ARG_CHECK((dos_mem_read32(vm, 0x14000) & 0x20u) &&
                  (dos_mem_read32(vm, 0x15000 + (0x60 + (argument_linear - frame) / 4096u) * 4u) & 0x60u) == 0x60u);
    }

    for (unsigned width = 0; width < 2; width++)
    for (unsigned policy = 0; policy < 4; policy++) {
        dpmi_paging_fixture_t fixture = dpmi_paging_test_prepare(vm, width);
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cpl = 3;
        cpu.cs = fixture.code;
        cpu.ss = fixture.stack;
        cpu.esp = 0x6FF0;
        cpu8086_sync_cs(&cpu);
        cpu8086_sync_data(&cpu);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        vm->dpmi.real_mode_paging = (dpmi_paging_t){ DOS_CR0_PG | DOS_CR0_WP | 0x10u, 0x14018 };
        dpmi_rm_regs_t regs = { .flags = FLAGS_FIXED, .ss = 0x8000, .sp = 0x1010,
                                .cs = 0x9000, .ip = 0x100 };
        memory[0x90100] = 0xCB;
        dos_mem_write16(vm, 0x24FF0, 0xBEEF);
        dos_mem_write16(vm, 0x2600E, 0xCAFE);
        if (policy == 0) dos_mem_write32(vm, 0x1101C, 0x26005); /* source reads need no PTE.RW */
        if (policy == 1) {
            vm->dpmi.real_mode_paging.cr0 &= ~DOS_CR0_WP;
            dos_mem_write32(vm, 0x15204, 0x81001); /* supervisor writes with WP=0 */
        }
        if (policy == 2) {
            cpu.ss_cache.descriptor.access |= 4u;
            dpmi_desc_set_limit(&cpu.ss_cache.descriptor, 0x6FEF);
        }
        uint32_t arguments = 0x80FF0;
        if (policy == 3) {
            regs.ss = regs.sp = 0;
            arguments = dos_linear(vm->dpmi.real_mode_stack.ss,
                                    (uint16_t)(vm->dpmi.real_mode_stack.esp - 32u));
        }
        ARG_CHECK(!dpmi_simulate_rm_call(vm, &regs, 16, DPMI_RM_CALL_FAR, 0));
        ARG_CHECK(dos_mem_read16(vm, arguments) == 0xBEEF &&
                  dos_mem_read16(vm, arguments + 30u) == 0xCAFE);
        ARG_CHECK(cpu.running && cpu.protected_mode && cpu.esp == 0x6FF0 && cpu.cr3 == 0x10018);
    }

    /* All admission failures leave the CPU, stack payload and A/D bits alone. */
    for (unsigned width = 0; width < 2; width++)
    for (unsigned scenario = 0; scenario < 18; scenario++) {
        dpmi_paging_fixture_t fixture = dpmi_paging_test_prepare(vm, width);
        cpu.protected_mode = cpu.pm_cs_loaded = true;
        cpu.cpl = 3;
        cpu.cs = fixture.code;
        cpu.ss = fixture.stack;
        cpu.esp = 0x6FF0;
        cpu8086_sync_cs(&cpu);
        cpu8086_sync_data(&cpu);
        dpmi_apply_paging(&cpu, vm->dpmi.suspended_paging);
        vm->dpmi.real_mode_paging = (dpmi_paging_t){ DOS_CR0_PG | DOS_CR0_WP | 0x10u, 0x14018 };
        dpmi_rm_regs_t regs = { .flags = FLAGS_FIXED, .ss = 0x8000, .sp = 0x1010,
                                .cs = 0x9000, .ip = 0x100 };
        memory[0x90100] = 0xCB;
        uint16_t words = 16, expected = 0x8012;
        switch (scenario) {
        case 0: dos_mem_write32(vm, 0x1101C, 0); break; /* last source page */
        case 1: dos_mem_write32(vm, 0x15204, 0); break; /* last destination page */
        case 2: dos_mem_write32(vm, 0x1101C, 0x26003); break; /* source supervisor */
        case 3: dos_mem_write32(vm, 0x15204, 0x81005); break; /* destination read-only */
        case 4: dos_mem_write32(vm, 0x1101C, vm->total_mem_size | 7u); break;
        case 5: dos_mem_write32(vm, 0x15204, vm->total_mem_size | 7u); break;
        case 6: cpu.ss_cache.valid = false; expected = 0x8021; break;
        case 7: dpmi_desc_set_limit(&cpu.ss_cache.descriptor, 0x6FFF); expected = 0x8021; break;
        case 8: cpu.ss_cache.descriptor.access |= DESC_CODE; expected = 0x8021; break;
        case 9: cpu.ss_cache.descriptor.access &= ~DESC_WRITABLE; expected = 0x8021; break;
        case 10: cpu.ss_cache.descriptor.access |= 4u; expected = 0x8021; break; /* expand-down */
        case 11: dpmi_desc_set_base(&cpu.ss_cache.descriptor, 0xFFFFF000); expected = 0x8021; break;
        case 12: words = 0xFFFF; expected = 0x8021; break;
        case 13: regs.sp = 35; expected = 0x8021; break;
        case 14: dos_mem_write32(vm, 0x15240, 0); expected = 0x8021; break; /* code absent */
        case 15: dos_mem_write32(vm, 0x14000, 0); expected = 0x8021; break; /* code lookup fails first */
        case 16: cpu.esp = width ? 0xFFFFFFF0u : 0xFFF0u; expected = 0x8021; break;
        case 17: dos_mem_write32(vm, 0x15200, 0); break; /* first destination page */
        }
        cpu8086_state_t before = cpu;
        dpmi_rm_regs_t original = regs;
        uint32_t source_entry = dos_mem_read32(vm, 0x11018);
        uint32_t dest_entry = dos_mem_read32(vm, 0x15200);
        uint32_t source_directory = dos_mem_read32(vm, 0x10000);
        uint32_t real_directory = dos_mem_read32(vm, 0x14000);
        dos_mem_write32(vm, 0x80FEC, 0xA5B6C7D8);
        ARG_CHECK(dpmi_simulate_rm_call(vm, &regs, words, DPMI_RM_CALL_FAR, 0) == expected);
        bool same = true;
        for (unsigned i = 0; i < sizeof(cpu); i++)
            if (((uint8_t *)&cpu)[i] != ((uint8_t *)&before)[i]) same = false;
        for (unsigned i = 0; i < sizeof(regs); i++)
            if (((uint8_t *)&regs)[i] != ((uint8_t *)&original)[i]) same = false;
        ARG_CHECK(same && dos_mem_read32(vm, 0x80FEC) == 0xA5B6C7D8);
        ARG_CHECK(dos_mem_read32(vm, 0x11018) == source_entry &&
                  dos_mem_read32(vm, 0x15200) == dest_entry &&
                  dos_mem_read32(vm, 0x10000) == source_directory &&
                  dos_mem_read32(vm, 0x14000) == real_directory);
    }
    dos_host_free_pages(memory, memory_pages);
    dos_host_free_pages(vm, vm_pages);
    serial_puts("[DPMI-RM-ARGUMENTS] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef ARG_CHECK
    return failures;
}

static int dpmi_rm_call_selftest(void)
{

    const uint64_t memory_pages = 512;
    uint64_t vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = (dos_vm_t *)dos_host_alloc_pages(vm_pages);
    uint8_t *memory = (uint8_t *)dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (memory) dos_host_free_pages(memory, memory_pages);
        if (vm) dos_host_free_pages(vm, vm_pages);
        return 1;
    }

    dpmi_zero(vm, vm_pages * 4096u);
    dpmi_zero(memory, memory_pages * 4096u);
    cpu8086_state_t cpu;
    dpmi_zero(&cpu, sizeof(cpu));
    vm->cpu = &cpu;
    vm->mem = memory;
    vm->total_mem_size = (uint32_t)(memory_pages * 4096u);
    cpu8086_init(&cpu, vm);
    dpmi_init(vm);
    vm->dpmi.active = true;
    vm->dpmi.is_32bit = false;

    uint16_t code_sel = dpmi_alloc_descriptor(&vm->dpmi);
    uint16_t data_sel = dpmi_alloc_descriptor(&vm->dpmi);
    uint16_t stack_sel = dpmi_alloc_descriptor(&vm->dpmi);
    int failures = 0;
    if (!code_sel || !data_sel || !stack_sel) {
        failures++;
        goto done;
    }

    uint8_t code_access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                          DESC_CODE | DESC_READABLE;
    uint8_t data_access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                          DESC_WRITABLE;
    dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(code_sel)],
                    0, 0xFFFF, code_access, 0);
    dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(data_sel)],
                    0, 0xFFFF, data_access, 0);
    dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(stack_sel)],
                    0, 0xFFFF, data_access, 0);

    const uint32_t regs_addr = 0x3000;
    const uint32_t pm_stack = 0x5000;
    const uint16_t rm_segment = 0x2000;
    const uint16_t far_offset = 0x0100;
    const uint16_t iret_offset = 0x0120;
    const uint16_t int_offset = 0x0140;

    /* Inspect the copied argument without removing caller-owned words. */
    uint32_t far_code = dos_linear(rm_segment, far_offset);
    const uint8_t far_program[] = {
        0x55, 0x89, 0xE5, 0x36, 0x8B, 0x5E, 0x06, 0x5D,
        0xB8, 0x34, 0x12, 0xCB
    };
    for (unsigned i = 0; i < sizeof(far_program); i++)
        memory[far_code + i] = far_program[i];
    dos_mem_write16(vm, pm_stack, 0xBEEF);

    dpmi_rm_regs_t regs;
    dpmi_zero(&regs, sizeof(regs));
    regs.flags = FLAGS_FIXED;
    regs.cs = rm_segment;
    regs.ip = far_offset;
    dpmi_write_rm_regs(vm, regs_addr, &regs);

    cpu.protected_mode = true;
    cpu.pm_cs_loaded = true;
    cpu.cr0 |= 1u;
    cpu.cs = code_sel;
    cpu.ds = data_sel;
    cpu.es = data_sel;
    cpu.ss = stack_sel;
    cpu.esp = pm_stack;
    cpu.cpl = 3;
    cpu8086_sync_cs(&cpu);
    cpu8086_sync_data(&cpu);
    cpu.eflags = FLAGS_FIXED;
    cpu.running = true;
    cpu.ax = 0x0301;
    cpu.cx = 1;
    cpu.edi = regs_addr;
    dos_int31_dpmi(vm);
    dpmi_read_rm_regs(vm, regs_addr, &regs);
    if ((cpu.eflags & FLAG_CF) || (uint16_t)regs.eax != 0x1234 ||
        regs.ebx != 0xBEEF || regs.cs != rm_segment ||
        regs.ip != far_offset || regs.ss != 0 || regs.sp != 0 ||
        !cpu.protected_mode || !cpu.pm_cs_loaded ||
        cpu.ss != stack_sel || cpu.esp != pm_stack) {
        serial_puts("[DPMI-TEST] simulated RM FAR call mismatch\n");
        failures++;
    }

    uint32_t iret_code = dos_linear(rm_segment, iret_offset);
    memory[iret_code] = 0xB8;
    memory[iret_code + 1u] = 0x78;
    memory[iret_code + 2u] = 0x56;
    memory[iret_code + 3u] = 0xCF;
    dpmi_zero(&regs, sizeof(regs));
    regs.flags = FLAGS_FIXED | FLAG_IF;
    regs.cs = rm_segment;
    regs.ip = iret_offset;
    dpmi_write_rm_regs(vm, regs_addr, &regs);

    cpu.ax = 0x0302;
    cpu.cx = 0;
    cpu.edi = regs_addr;
    cpu.running = true;
    dos_int31_dpmi(vm);
    dpmi_read_rm_regs(vm, regs_addr, &regs);
    if ((cpu.eflags & FLAG_CF) || (uint16_t)regs.eax != 0x5678 ||
        !(regs.flags & FLAG_IF) || regs.cs != rm_segment ||
        regs.ip != iret_offset || regs.ss != 0 || regs.sp != 0) {
        serial_puts("[DPMI-TEST] simulated RM IRET call mismatch\n");
        failures++;
    }

    uint32_t int_code = dos_linear(rm_segment, int_offset);
    memory[int_code] = 0xB8;
    memory[int_code + 1u] = 0xBC;
    memory[int_code + 2u] = 0x9A;
    memory[int_code + 3u] = 0xCF;
    dos_mem_write16(vm, 0x60u * 4u, int_offset);
    dos_mem_write16(vm, 0x60u * 4u + 2u, rm_segment);
    dpmi_zero(&regs, sizeof(regs));
    regs.flags = FLAGS_FIXED | FLAG_IF;
    regs.cs = 0xAAAA;
    regs.ip = 0xBBBB;
    dpmi_write_rm_regs(vm, regs_addr, &regs);

    cpu.ax = 0x0300;
    cpu.bl = 0x60;
    cpu.bh = 0;
    cpu.cx = 0;
    cpu.edi = regs_addr;
    cpu.running = true;
    dos_int31_dpmi(vm);
    dpmi_read_rm_regs(vm, regs_addr, &regs);
    if ((cpu.eflags & FLAG_CF) || (uint16_t)regs.eax != 0x9ABC ||
        regs.cs != 0xAAAA || regs.ip != 0xBBBB ||
        regs.ss != 0 || regs.sp != 0) {
        serial_puts("[DPMI-TEST] simulated RM interrupt mismatch\n");
        failures++;
    }

    cpu.ax = 0x0301;
    cpu.bh = 0x02;
    cpu.cx = 0;
    cpu.edi = regs_addr;
    cpu.eflags &= ~FLAG_CF;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8021) {
        serial_puts("[DPMI-TEST] RM call reserved-field validation mismatch\n");
        failures++;
    }

    const uint32_t callback_code = 0x1800;
    const uint32_t callback_regs_addr = 0x3100;
    const uint8_t callback_program[] = {
        0x8B, 0x04,                         /* MOV AX,[SI] */
        0x26, 0x89, 0x45, 0x2A,             /* MOV ES:[DI+2A],AX */
        0x8B, 0x44, 0x02,                   /* MOV AX,[SI+2] */
        0x26, 0x89, 0x45, 0x2C,             /* MOV ES:[DI+2C],AX */
        0x26, 0x83, 0x45, 0x2E, 0x04,       /* ADD ES:[DI+2E],4 */
        0x26, 0xC7, 0x45, 0x1C, 0xFE, 0xCA, /* MOV ES:[DI+1C],CAFE */
        0xCF                                /* IRET */
    };
    for (uint32_t i = 0; i < sizeof(callback_program); i++)
        memory[callback_code + i] = callback_program[i];

    cpu.ax = 0x0303;
    cpu.ds = code_sel;
    cpu.esi = callback_code;
    cpu.es = data_sel;
    cpu.edi = callback_regs_addr;
    cpu.eflags = FLAG_CF | FLAGS_FIXED;
    dos_int31_dpmi(vm);
    uint16_t callback_seg = cpu.cx;
    uint16_t callback_off = cpu.dx;
    uint16_t callback_stack_sel = vm->dpmi.callbacks[0].rm_stack_sel;
    if ((cpu.eflags & FLAG_CF) || callback_seg != DPMI_ENTRY_SEG ||
        callback_off != DPMI_CALLBACK_BASE_OFF || !callback_stack_sel) {
        serial_puts("[DPMI-TEST] callback allocation mismatch\n");
        failures++;
    }

    dpmi_zero(&regs, sizeof(regs));
    regs.flags = FLAGS_FIXED | FLAG_IF;
    regs.cs = callback_seg;
    regs.ip = callback_off;
    dpmi_write_rm_regs(vm, regs_addr, &regs);

    cpu.ax = 0x0301;
    cpu.bh = 0;
    cpu.cx = 0;
    cpu.es = data_sel;
    cpu.edi = regs_addr;
    cpu.running = true;
    dos_int31_dpmi(vm);
    dpmi_read_rm_regs(vm, regs_addr, &regs);
    dpmi_rm_regs_t callback_regs;
    dpmi_read_rm_regs(vm, callback_regs_addr, &callback_regs);
    if ((cpu.eflags & FLAG_CF) || (uint16_t)regs.eax != 0xCAFE ||
        callback_regs.cs != DPMI_ENTRY_SEG ||
        callback_regs.ip != DPMI_RM_RETURN_OFF ||
        callback_regs.ss != DPMI_ENTRY_SEG ||
        callback_regs.sp != DPMI_RM_STACK_TOP ||
        vm->dpmi.callback_depth != 0 || !cpu.protected_mode) {
        serial_puts("[DPMI-TEST] callback round-trip mismatch\n");
        failures++;
    }

    cpu.ax = 0x0304;
    cpu.cx = callback_seg;
    cpu.dx = callback_off;
    cpu.eflags |= FLAG_CF;
    dos_int31_dpmi(vm);
    uint16_t callback_stack_index = dpmi_sel_to_index(callback_stack_sel);
    if ((cpu.eflags & FLAG_CF) || vm->dpmi.callbacks[0].active ||
        vm->dpmi.descriptor_state[callback_stack_index] != DPMI_DESC_FREE) {
        serial_puts("[DPMI-TEST] callback free mismatch\n");
        failures++;
    }

    dpmi_buffer_fault_t callback_fault;
    if (dpmi_code_target_probe(vm, data_sel, callback_code, &callback_fault) ||
        callback_fault.vector != 13 || callback_fault.error != (data_sel & ~3u)) {
        serial_puts("[DPMI-TEST] callback target validation mismatch\n");
        failures++;
    }

    failures += dpmi_state_service_selftest(vm, data_sel);
    failures += dpmi_state_recovery_selftest();
    failures += dpmi_client_buffer_selftest(vm, data_sel);
    failures += dpmi_service_buffer_selftest();
    failures += dpmi_callback_buffer_selftest(vm, data_sel, stack_sel);
    failures += dpmi_callback_registration_selftest();
    failures += dpmi_callback_recovery_selftest();

done:
    dos_host_free_pages(memory, memory_pages);
    dos_host_free_pages(vm, vm_pages);
    return failures;
}

static int dpmi_dos_memory_selftest(void)
{

    const uint64_t vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    const uint64_t memory_pages = (DOS_CONV_TOP + 4095u) / 4096u;
    dos_vm_t *vm = (dos_vm_t *)dos_host_alloc_pages(vm_pages);
    uint8_t *memory = (uint8_t *)dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (memory) dos_host_free_pages(memory, memory_pages);
        if (vm) dos_host_free_pages(vm, vm_pages);
        return 1;
    }

    dpmi_zero(vm, vm_pages * 4096u);
    dpmi_zero(memory, memory_pages * 4096u);
    cpu8086_state_t cpu;
    dpmi_zero(&cpu, sizeof(cpu));
    vm->cpu = &cpu;
    vm->mem = memory;
    vm->total_mem_size = DOS_CONV_TOP;
    vm->system_mem_size = DOS_CONV_TOP;
    vm->current_psp = 0x1234;
    cpu8086_init(&cpu, vm);
    dos_mem_init(vm);
    dpmi_init(vm);
    vm->dpmi.active = true;
    vm->dpmi.is_32bit = false;

    int failures = 0;
    cpu.ax = 0x0100;
    cpu.bx = 0x2001;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    uint16_t segment = cpu.ax;
    uint16_t selector = cpu.dx;
    uint16_t index = dpmi_sel_to_index(selector);
    uint32_t base = (uint32_t)segment << 4;
    dpmi_dos_block_t *block = index < DPMI_MAX_DESCRIPTORS
                            ? &vm->dpmi.dos_blocks[index] : NULL;
    if ((cpu.eflags & FLAG_CF) || !segment ||
        index < DPMI_SPECIFIC_DESCRIPTOR_COUNT || !block ||
        !block->allocated || block->segment != segment ||
        block->paragraphs != 0x2001 || block->descriptor_count != 3 ||
        dpmi_desc_get_base(&vm->dpmi.ldt[index]) != base ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[index]) != 0x2000Fu ||
        dpmi_desc_get_base(&vm->dpmi.ldt[index + 1u]) != base + 0x10000u ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[index + 1u]) != 0xFFFFu ||
        dpmi_desc_get_base(&vm->dpmi.ldt[index + 2u]) != base + 0x20000u ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[index + 2u]) != 0x000Fu ||
        (vm->dpmi.ldt[index].flags_lim & DESC_32BIT) ||
        vm->dpmi.descriptor_state[index] != DPMI_DESC_DOS_MEMORY ||
        vm->dpmi.descriptor_state[index + 1u] != DPMI_DESC_DOS_MEMORY ||
        vm->dpmi.descriptor_state[index + 2u] != DPMI_DESC_DOS_MEMORY)
        failures++;

    dpmi_descriptor_t original = vm->dpmi.ldt[index];
    const uint16_t modification_functions[] = {
        0x0007, 0x0008, 0x0009, 0x000C
    };
    for (unsigned i = 0;
         i < sizeof(modification_functions) / sizeof(modification_functions[0]);
         i++) {
        cpu.ax = modification_functions[i];
        cpu.bx = selector;
        cpu.cx = 0x4092;
        cpu.dx = 0x3456;
        cpu.eflags = 0;
        dos_int31_dpmi(vm);
        if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8022)
            failures++;
    }
    for (unsigned i = 0; i < sizeof(original); i++)
        if (((const uint8_t *)&vm->dpmi.ldt[index])[i] !=
            ((const uint8_t *)&original)[i])
            failures++;

    cpu.ds = selector + DPMI_SEL_INC;
    cpu.es = selector + 2u * DPMI_SEL_INC;
    cpu.fs = selector + DPMI_SEL_INC;
    cpu.gs = selector + 2u * DPMI_SEL_INC;
    cpu.ax = 0x0102;
    cpu.bx = 0x0800;
    cpu.dx = selector;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) || !block ||
        block->paragraphs != 0x0800 || block->descriptor_count != 1 ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[index]) != 0x7FFFu ||
        vm->dpmi.descriptor_state[index + 1u] != DPMI_DESC_FREE ||
        vm->dpmi.descriptor_state[index + 2u] != DPMI_DESC_FREE ||
        cpu.ds || cpu.es || cpu.fs || cpu.gs)
        failures++;

    uint16_t blocked_selector = dpmi_index_to_sel(index + 1u);
    cpu.ax = 0x000D;
    cpu.bx = blocked_selector;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu.eflags & FLAG_CF) failures++;

    cpu.ax = 0x0102;
    cpu.bx = 0x1001;
    cpu.dx = selector;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    uint16_t current = 0;
    uint16_t available = 0;
    int query = dos_mem_query_block(vm, segment, &current, &available);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8011 ||
        cpu.bx != 0x1000 || query != DOS_MEM_OK || current != 0x0800 ||
        !block || block->paragraphs != 0x0800 ||
        block->descriptor_count != 1 ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[index]) != 0x7FFFu ||
        vm->dpmi.descriptor_state[index + 1u] != DPMI_DESC_MUTABLE)
        failures++;

    cpu.ax = 0x0001;
    cpu.bx = blocked_selector;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu.eflags & FLAG_CF) failures++;

    cpu.ax = 0x0102;
    cpu.bx = 0x1001;
    cpu.dx = selector;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) || !block ||
        block->paragraphs != 0x1001 || block->descriptor_count != 2 ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[index]) != 0x1000Fu ||
        dpmi_desc_get_base(&vm->dpmi.ldt[index + 1u]) != base + 0x10000u ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[index + 1u]) != 0x000Fu)
        failures++;

    uint16_t guard = dos_mem_alloc(vm, 0x10, NULL);
    cpu.ax = 0x0102;
    cpu.bx = 0x2000;
    cpu.dx = selector;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    query = dos_mem_query_block(vm, segment, &current, &available);
    if (!guard || !(cpu.eflags & FLAG_CF) || cpu.ax != 0x0008 ||
        cpu.bx != 0x1001 || query != DOS_MEM_OK || current != 0x1001 ||
        !block || block->paragraphs != 0x1001 ||
        block->descriptor_count != 2)
        failures++;
    if (guard && dos_mem_free(vm, guard) != DOS_MEM_OK) failures++;

    cpu.ds = selector;
    cpu.es = selector + DPMI_SEL_INC;
    cpu.ax = 0x0101;
    cpu.dx = selector & (uint16_t)~3u;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) || !block || block->allocated ||
        vm->dpmi.descriptor_state[index] != DPMI_DESC_FREE ||
        vm->dpmi.descriptor_state[index + 1u] != DPMI_DESC_FREE ||
        cpu.ds || cpu.es ||
        dos_mem_query_block(vm, segment, &current, &available) !=
            DOS_MEM_ERR_BLOCK)
        failures++;

    cpu.ax = 0x0101;
    cpu.dx = 0x0008;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8022) failures++;

    dpmi_init(vm);
    vm->dpmi.active = true;
    vm->dpmi.is_32bit = true;
    cpu.ax = 0x0100;
    cpu.bx = 0x3001;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    segment = cpu.ax;
    selector = cpu.dx;
    index = dpmi_sel_to_index(selector);
    block = index < DPMI_MAX_DESCRIPTORS
          ? &vm->dpmi.dos_blocks[index] : NULL;
    base = (uint32_t)segment << 4;
    if ((cpu.eflags & FLAG_CF) || !block ||
        block->descriptor_count != 1 || block->paragraphs != 0x3001 ||
        dpmi_desc_get_base(&vm->dpmi.ldt[index]) != base ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[index]) != 0x3000Fu ||
        !(vm->dpmi.ldt[index].flags_lim & DESC_32BIT))
        failures++;

    cpu.ax = 0x0102;
    cpu.bx = 0x4001;
    cpu.dx = selector;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) || !block ||
        block->descriptor_count != 1 || block->paragraphs != 0x4001 ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[index]) != 0x4000Fu)
        failures++;

    cpu.ax = 0x0101;
    cpu.dx = selector;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu.eflags & FLAG_CF) failures++;

    uint16_t largest_before = 0;
    uint16_t largest_after = 0;
    if (dos_mem_largest_available(vm, &largest_before) != DOS_MEM_OK)
        failures++;
    for (uint16_t i = DPMI_SPECIFIC_DESCRIPTOR_COUNT;
         i < DPMI_MAX_DESCRIPTORS; i++)
        vm->dpmi.descriptor_state[i] = DPMI_DESC_MUTABLE;
    cpu.ax = 0x0100;
    cpu.bx = 0x20;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8011 ||
        cpu.bx != largest_before ||
        dos_mem_largest_available(vm, &largest_after) != DOS_MEM_OK ||
        largest_after != largest_before)
        failures++;

    dpmi_init(vm);
    vm->dpmi.active = true;
    vm->dpmi.is_32bit = true;
    cpu.ax = 0x0000;
    cpu.cx = 1;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    uint16_t ordinary_selector = cpu.ax;
    cpu.ax = 0x0102;
    cpu.bx = 1;
    cpu.dx = ordinary_selector;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x0009) failures++;

    dos_mcb_t *root = (dos_mcb_t *)(vm->mem +
                                     ((uint32_t)vm->first_mcb << 4));
    root->type = 'X';
    cpu.ax = 0x0100;
    cpu.bx = 1;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x0007 || cpu.bx != 0)
        failures++;

    dos_host_free_pages(memory, memory_pages);
    dos_host_free_pages(vm, vm_pages);
    return failures;
}

enum {
    DPMI_ENTRY_TEST_PSP = 0x2000,
    DPMI_ENTRY_TEST_ENV = 0x2100,
    DPMI_ENTRY_TEST_DS = 0x2200,
    DPMI_ENTRY_TEST_SS = 0x2300,
    DPMI_ENTRY_TEST_CS = 0x2400,
    DPMI_ENTRY_TEST_ES = 0x2500,
    DPMI_ENTRY_TEST_SP = 0x0100,
    DPMI_ENTRY_TEST_IP = 0x3456
};

static void dpmi_prepare_entry_selftest(dos_vm_t *vm,
                                        cpu8086_state_t *cpu,
                                        uint16_t mode_flags)
{
    cpu8086_init(cpu, vm);
    vm->cpu = cpu;
    vm->current_psp = DPMI_ENTRY_TEST_PSP;
    dpmi_descriptor_t prior_ldt = { .limit_lo = 0xFF, .base_mid = 7, .access = 0x82 };
    cpu8086_cache_ldtr(cpu, 0x28, &prior_ldt);
    vm->software_int_frame_bytes = 6;
    vm->software_int_return_flags = FLAGS_FIXED | FLAG_IF | FLAG_CF;

    cpu->eax = 0xA5A50000u | mode_flags;
    cpu->ebx = 0x11223344u;
    cpu->ecx = 0x22334455u;
    cpu->edx = 0x33445566u;
    cpu->ebp = 0x44556677u;
    cpu->esi = 0x55667788u;
    cpu->edi = 0x66778899u;
    cpu->esp = 0xCAFE0000u | DPMI_ENTRY_TEST_SP;
    cpu->cs = DPMI_ENTRY_SEG;
    cpu->eip = DPMI_ENTRY_OFF + 2u;
    cpu->ds = DPMI_ENTRY_TEST_DS;
    cpu->es = DPMI_ENTRY_TEST_ES;
    cpu->ss = DPMI_ENTRY_TEST_SS;
    cpu->fs = 0x1111;
    cpu->gs = 0x2222;

    uint32_t stack = ((uint32_t)DPMI_ENTRY_TEST_SS << 4) +
                     DPMI_ENTRY_TEST_SP;
    dos_mem_write16(vm, stack + 6u, DPMI_ENTRY_TEST_IP);
    dos_mem_write16(vm, stack + 8u, DPMI_ENTRY_TEST_CS);
    dos_mem_write16(vm, ((uint32_t)DPMI_ENTRY_TEST_PSP << 4) + 0x2Cu,
                    DPMI_ENTRY_TEST_ENV);
}

static bool dpmi_descriptor_unchanged(const dpmi_descriptor_t *left,
                                      const dpmi_descriptor_t *right)
{
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;
    for (unsigned i = 0; i < sizeof(*left); i++)
        if (a[i] != b[i]) return false;
    return true;
}

static int dpmi_entry_selftest(void)
{

    const uint64_t vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    const uint64_t memory_pages = 512;
    dos_vm_t *vm = (dos_vm_t *)dos_host_alloc_pages(vm_pages);
    uint8_t *memory = (uint8_t *)dos_host_alloc_pages(memory_pages);
    if (!vm || !memory) {
        if (memory) dos_host_free_pages(memory, memory_pages);
        if (vm) dos_host_free_pages(vm, vm_pages);
        return 1;
    }

    dpmi_zero(vm, vm_pages * 4096u);
    dpmi_zero(memory, memory_pages * 4096u);
    cpu8086_state_t cpu;
    dpmi_zero(&cpu, sizeof(cpu));
    vm->cpu = &cpu;
    vm->mem = memory;
    vm->total_mem_size = memory_pages * 4096u;
    vm->system_mem_size = vm->total_mem_size;

    int failures = 0;
    dpmi_init(vm);
    dpmi_prepare_entry_selftest(vm, &cpu, 1);
    dpmi_enter_protected_mode(vm);

    uint16_t code_index = dpmi_sel_to_index(vm->dpmi.sel_code);
    uint16_t data_index = dpmi_sel_to_index(vm->dpmi.sel_data);
    uint16_t stack_index = dpmi_sel_to_index(vm->dpmi.sel_stack);
    uint16_t psp_index = dpmi_sel_to_index(vm->dpmi.sel_psp);
    uint16_t env_index = dpmi_sel_to_index(vm->dpmi.sel_env);
    uint16_t host_index = dpmi_sel_to_index(vm->dpmi.sel_host_code);
    uint32_t psp_base = (uint32_t)DPMI_ENTRY_TEST_PSP << 4;
    uint16_t converted_environment = dos_mem_read16(vm, psp_base + 0x2Cu);

    if (!vm->dpmi.active || !vm->dpmi.is_32bit || !cpu.protected_mode ||
        !cpu.host_ldt || cpu.ldtr || cpu.ldt_cache.valid ||
        !cpu.pm_cs_loaded || !(cpu.cr0 & 1u) || cpu.op_size_32 ||
        cpu.addr_size_32 || cpu.cs != vm->dpmi.sel_code ||
        cpu.ds != vm->dpmi.sel_data || cpu.ss != vm->dpmi.sel_stack ||
        cpu.es != vm->dpmi.sel_psp || cpu.fs || cpu.gs ||
        cpu.eip != DPMI_ENTRY_TEST_IP ||
        cpu.esp != DPMI_ENTRY_TEST_SP + 10u ||
        cpu.eax != 0xA5A50001u || cpu.ebx != 0x11223344u ||
        cpu.ecx != 0x22334455u || cpu.edx != 0x33445566u ||
        cpu.ebp != 0x44556677u || cpu.esi != 0x55667788u ||
        cpu.edi != 0x66778899u || (cpu.eflags & FLAG_CF) ||
        !(cpu.eflags & FLAG_IF) || !cpu_stack_addr32(&cpu))
        failures++;

    if (code_index < DPMI_SPECIFIC_DESCRIPTOR_COUNT ||
        vm->dpmi.sel_data != vm->dpmi.sel_code + DPMI_SEL_INC ||
        vm->dpmi.sel_stack != vm->dpmi.sel_data + DPMI_SEL_INC ||
        vm->dpmi.sel_psp != vm->dpmi.sel_stack + DPMI_SEL_INC ||
        vm->dpmi.sel_env != vm->dpmi.sel_psp + DPMI_SEL_INC ||
        dpmi_desc_get_base(&vm->dpmi.ldt[code_index]) !=
            ((uint32_t)DPMI_ENTRY_TEST_CS << 4) ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[code_index]) != 0xFFFFu ||
        (vm->dpmi.ldt[code_index].flags_lim & DESC_32BIT) ||
        dpmi_desc_get_base(&vm->dpmi.ldt[data_index]) !=
            ((uint32_t)DPMI_ENTRY_TEST_DS << 4) ||
        !(vm->dpmi.ldt[data_index].flags_lim & DESC_32BIT) ||
        dpmi_desc_get_base(&vm->dpmi.ldt[stack_index]) !=
            ((uint32_t)DPMI_ENTRY_TEST_SS << 4) ||
        !(vm->dpmi.ldt[stack_index].flags_lim & DESC_32BIT) ||
        dpmi_desc_get_base(&vm->dpmi.ldt[psp_index]) != psp_base ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[psp_index]) != 0xFFu ||
        dpmi_desc_get_base(&vm->dpmi.ldt[env_index]) !=
            ((uint32_t)DPMI_ENTRY_TEST_ENV << 4) ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[env_index]) != 0xFFFFu ||
        converted_environment != vm->dpmi.sel_env ||
        vm->dpmi.descriptor_state[code_index] != DPMI_DESC_MUTABLE ||
        vm->dpmi.descriptor_state[data_index] != DPMI_DESC_MUTABLE ||
        vm->dpmi.descriptor_state[stack_index] != DPMI_DESC_MUTABLE ||
        vm->dpmi.descriptor_state[psp_index] != DPMI_DESC_CLIENT_SYSTEM ||
        vm->dpmi.descriptor_state[env_index] != DPMI_DESC_CLIENT_SYSTEM)
        failures++;

    bool host_descriptor_valid = vm->dpmi.sel_host_code &&
                                 host_index < DPMI_MAX_DESCRIPTORS;
    dpmi_descriptor_t *host_descriptor = host_descriptor_valid
                                             ? &vm->dpmi.ldt[host_index]
                                             : NULL;
    uint8_t host_access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                          DESC_CODE | DESC_READABLE;
    if (!host_descriptor_valid ||
        vm->dpmi.descriptor_state[host_index] != DPMI_DESC_HOST ||
        dpmi_desc_get_base(host_descriptor) !=
            ((uint32_t)DPMI_ENTRY_SEG << 4) ||
        dpmi_desc_get_limit(host_descriptor) != 0xFFFFu ||
        (host_descriptor->access & host_access) != host_access ||
        !(host_descriptor->flags_lim & DESC_32BIT)) {
        serial_puts("[DPMI-TEST] 32-bit host descriptor mismatch\n");
        failures++;
    }

    /* 0204h remains available after every client descriptor is occupied.
     * Low software vectors return through STI/IRET; all others use IRET. */
    uint8_t saved_descriptor_state[DPMI_MAX_DESCRIPTORS];
    for (unsigned i = 0; i < DPMI_MAX_DESCRIPTORS; i++) {
        saved_descriptor_state[i] = vm->dpmi.descriptor_state[i];
        if (vm->dpmi.descriptor_state[i] == DPMI_DESC_FREE)
            vm->dpmi.descriptor_state[i] = DPMI_DESC_MUTABLE;
    }
    uint16_t exception_stack = dpmi_get_exception_stack_selector(vm);
    uint16_t exception_index = dpmi_sel_to_index(exception_stack);
    if (!exception_stack || exception_stack != vm->dpmi.sel_exception_stack ||
        vm->dpmi.descriptor_state[exception_index] != DPMI_DESC_HOST ||
        dpmi_desc_get_base(&vm->dpmi.ldt[exception_index]) !=
            vm->system_mem_size - DPMI_EXCEPTION_STACK_SIZE ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[exception_index]) !=
            DPMI_EXCEPTION_STACK_SIZE - 1u ||
        !(vm->dpmi.ldt[exception_index].flags_lim & DESC_32BIT)) failures++;

    cpu.ax = 0x0204;
    cpu.bl = 0x07;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    uint32_t low_reflector = dos_linear(
        DPMI_ENTRY_SEG,
        DPMI_PM_REFLECT_BASE_OFF + 7u * DPMI_PM_REFLECT_STUB_SIZE);
    if ((cpu.eflags & FLAG_CF) || cpu.cx != vm->dpmi.sel_host_code ||
        cpu.edx != DPMI_PM_REFLECT_BASE_OFF +
                   7u * DPMI_PM_REFLECT_STUB_SIZE ||
        dos_mem_read8(vm, low_reflector) != 0xCD ||
        dos_mem_read8(vm, low_reflector + 1u) !=
            DPMI_DEFAULT_REFLECT_INT ||
        dos_mem_read8(vm, low_reflector + 2u) != 0xFB ||
        dos_mem_read8(vm, low_reflector + 3u) != 0xCF) {
        serial_puts("[DPMI-TEST] low default vector mismatch\n");
        failures++;
    }

    cpu.ax = 0x0204;
    cpu.bl = 0x33;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    uint32_t regular_reflector = dos_linear(
        DPMI_ENTRY_SEG,
        DPMI_PM_REFLECT_BASE_OFF + 0x33u * DPMI_PM_REFLECT_STUB_SIZE);
    if ((cpu.eflags & FLAG_CF) || cpu.cx != vm->dpmi.sel_host_code ||
        cpu.edx != DPMI_PM_REFLECT_BASE_OFF +
                   0x33u * DPMI_PM_REFLECT_STUB_SIZE ||
        dos_mem_read8(vm, regular_reflector) != 0xCD ||
        dos_mem_read8(vm, regular_reflector + 1u) !=
            DPMI_DEFAULT_REFLECT_INT ||
        dos_mem_read8(vm, regular_reflector + 2u) != 0xCF ||
        dos_mem_read8(vm, regular_reflector + 3u) != 0x90) {
        serial_puts("[DPMI-TEST] regular default vector mismatch\n");
        failures++;
    }

    uint32_t hardware_reflector = dos_linear(
        DPMI_ENTRY_SEG,
        DPMI_PM_HW_REFLECT_BASE_OFF +
            0x33u * DPMI_PM_REFLECT_STUB_SIZE);
    if (dos_mem_read8(vm, hardware_reflector) != 0xCD ||
        dos_mem_read8(vm, hardware_reflector + 1u) !=
            DPMI_DEFAULT_REFLECT_INT ||
        dos_mem_read8(vm, hardware_reflector + 2u) != 0xFB ||
        dos_mem_read8(vm, hardware_reflector + 3u) != 0xCF) {
        serial_puts("[DPMI-TEST] hardware default vector mismatch\n");
        failures++;
    }

    for (unsigned i = 0; i < DPMI_MAX_DESCRIPTORS; i++)
        vm->dpmi.descriptor_state[i] = saved_descriptor_state[i];

    if (vm->dpmi.saved_cs != DPMI_ENTRY_SEG ||
        vm->dpmi.saved_ip != DPMI_ENTRY_OFF + 2u ||
        vm->dpmi.saved_ss != DPMI_ENTRY_TEST_SS ||
        vm->dpmi.saved_sp != DPMI_ENTRY_TEST_SP ||
        vm->dpmi.saved_ds != DPMI_ENTRY_TEST_DS ||
        vm->dpmi.saved_es != DPMI_ENTRY_TEST_ES)
        failures++;

    const uint16_t immutable_selectors[] = {
        vm->dpmi.sel_psp, vm->dpmi.sel_env, vm->dpmi.sel_exception_stack
    };
    const uint16_t modification_functions[] = {
        0x0001, 0x0007, 0x0008, 0x0009, 0x000C
    };
    for (unsigned selector_no = 0;
         selector_no < sizeof(immutable_selectors) /
                       sizeof(immutable_selectors[0]); selector_no++) {
        uint16_t selector = immutable_selectors[selector_no];
        uint16_t index = dpmi_sel_to_index(selector);
        dpmi_descriptor_t original = vm->dpmi.ldt[index];
        for (unsigned function_no = 0;
             function_no < sizeof(modification_functions) /
                           sizeof(modification_functions[0]); function_no++) {
            cpu.ax = modification_functions[function_no];
            cpu.bx = selector;
            cpu.cx = 0x4092;
            cpu.dx = 0x3456;
            cpu.eflags = 0;
            dos_int31_dpmi(vm);
            if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8022)
                failures++;
        }
        if (!dpmi_descriptor_unchanged(&vm->dpmi.ldt[index], &original) ||
            vm->dpmi.descriptor_state[index] !=
                (selector == vm->dpmi.sel_exception_stack ? DPMI_DESC_HOST
                                                        : DPMI_DESC_CLIENT_SYSTEM))
            failures++;
    }

    cpu.ax = 0x0007;
    cpu.bx = vm->dpmi.sel_data;
    cpu.cx = DPMI_ENTRY_TEST_DS >> 12;
    cpu.dx = (uint16_t)(DPMI_ENTRY_TEST_DS << 4);
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu.eflags & FLAG_CF) failures++;

    dpmi_init(vm);
    dpmi_prepare_entry_selftest(vm, &cpu, 0);
    dpmi_enter_protected_mode(vm);
    data_index = dpmi_sel_to_index(vm->dpmi.sel_data);
    stack_index = dpmi_sel_to_index(vm->dpmi.sel_stack);
    host_index = dpmi_sel_to_index(vm->dpmi.sel_host_code);
    host_descriptor_valid = vm->dpmi.sel_host_code &&
                            host_index < DPMI_MAX_DESCRIPTORS;
    if (!vm->dpmi.active || vm->dpmi.is_32bit || !cpu.protected_mode ||
        !cpu.host_ldt || cpu.ldtr || cpu.ldt_cache.valid ||
        cpu.eax != 0xA5A50000u || cpu.esp != 0xCAFE010Au ||
        cpu_stack_addr32(&cpu) ||
        (vm->dpmi.ldt[data_index].flags_lim & DESC_32BIT) ||
        (vm->dpmi.ldt[stack_index].flags_lim & DESC_32BIT) ||
        !host_descriptor_valid ||
        vm->dpmi.descriptor_state[host_index] != DPMI_DESC_HOST ||
        (vm->dpmi.ldt[host_index].flags_lim & DESC_32BIT) ||
        cpu.es != vm->dpmi.sel_psp || cpu.fs || cpu.gs ||
        dos_mem_read16(vm, psp_base + 0x2Cu) != vm->dpmi.sel_env) {
        serial_puts("[DPMI-TEST] 16-bit host descriptor mismatch\n");
        failures++;
    }

    dpmi_init(vm);
    dpmi_prepare_entry_selftest(vm, &cpu, 2);
    dpmi_enter_protected_mode(vm);
    if (vm->dpmi.active || cpu.protected_mode || cpu.ax != 0x8021 ||
        cpu.host_ldt || cpu.ldtr != 0x28 || !cpu.ldt_cache.valid ||
        cpu.ldt_cache.descriptor.base_mid != 7 ||
        !(cpu.eflags & FLAG_CF) || !(cpu.eflags & FLAG_IF) ||
        cpu.cs != DPMI_ENTRY_SEG || cpu.eip != DPMI_ENTRY_OFF + 2u ||
        cpu.ss != DPMI_ENTRY_TEST_SS || cpu.sp != DPMI_ENTRY_TEST_SP ||
        dos_mem_read16(vm, psp_base + 0x2Cu) != DPMI_ENTRY_TEST_ENV) {
        serial_puts("[DPMI-TEST] failed entry retained host descriptor\n");
        failures++;
    }
    for (unsigned i = 0; i < DPMI_MAX_DESCRIPTORS; i++)
        if (vm->dpmi.descriptor_state[i] != DPMI_DESC_FREE) failures++;

    dpmi_init(vm);
    dpmi_prepare_entry_selftest(vm, &cpu, 1);
    for (unsigned i = DPMI_SPECIFIC_DESCRIPTOR_COUNT;
         i < DPMI_MAX_DESCRIPTORS; i++)
        vm->dpmi.descriptor_state[i] = DPMI_DESC_MUTABLE;
    dpmi_enter_protected_mode(vm);
    if (vm->dpmi.active || cpu.protected_mode || cpu.ax != 0x8011 ||
        !(cpu.eflags & FLAG_CF) || !(cpu.eflags & FLAG_IF) ||
        vm->dpmi.sel_code || vm->dpmi.sel_data || vm->dpmi.sel_stack ||
        vm->dpmi.sel_psp || vm->dpmi.sel_env || vm->dpmi.sel_host_code ||
        dos_mem_read16(vm, psp_base + 0x2Cu) != DPMI_ENTRY_TEST_ENV)
        failures++;
    for (unsigned i = DPMI_SPECIFIC_DESCRIPTOR_COUNT;
         i < DPMI_MAX_DESCRIPTORS; i++)
        if (vm->dpmi.descriptor_state[i] != DPMI_DESC_MUTABLE) failures++;

    /* Entry needs five client descriptors plus both host descriptors.
     * Failure after any partial allocation must leave no retained slot. */
    for (unsigned available = 1; available < 7; available++) {
        dpmi_init(vm);
        dpmi_prepare_entry_selftest(vm, &cpu, 1);
        for (unsigned i = DPMI_SPECIFIC_DESCRIPTOR_COUNT + available;
             i < DPMI_MAX_DESCRIPTORS; i++)
            vm->dpmi.descriptor_state[i] = DPMI_DESC_MUTABLE;
        dpmi_enter_protected_mode(vm);
        if (vm->dpmi.active || cpu.protected_mode || cpu.ax != 0x8011 ||
            !(cpu.eflags & FLAG_CF) || vm->dpmi.sel_host_code ||
            vm->dpmi.sel_exception_stack ||
            dos_mem_read16(vm, psp_base + 0x2Cu) != DPMI_ENTRY_TEST_ENV)
            failures++;
        for (unsigned i = DPMI_SPECIFIC_DESCRIPTOR_COUNT;
             i < DPMI_MAX_DESCRIPTORS; i++)
            if (vm->dpmi.descriptor_state[i] !=
                (i < DPMI_SPECIFIC_DESCRIPTOR_COUNT + available
                    ? DPMI_DESC_FREE : DPMI_DESC_MUTABLE)) failures++;
    }
    dpmi_init(vm);
    dpmi_prepare_entry_selftest(vm, &cpu, 1);
    vm->system_mem_size = DPMI_EXT_BASE + DPMI_EXCEPTION_STACK_SIZE - 1u;
    dpmi_enter_protected_mode(vm);
    if (vm->dpmi.active || cpu.protected_mode || cpu.ax != 0x8011 ||
        !(cpu.eflags & FLAG_CF) || vm->dpmi.sel_host_code ||
        vm->dpmi.sel_exception_stack) failures++;
    for (unsigned i = 0; i < DPMI_MAX_DESCRIPTORS; i++)
        if (vm->dpmi.descriptor_state[i] != DPMI_DESC_FREE) failures++;

    dos_host_free_pages(memory, memory_pages);
    dos_host_free_pages(vm, vm_pages);
    return failures;
}

static int dpmi_page_interrupt_service_selftest(dos_vm_t *vm,
                                                cpu8086_state_t *cpu)
{
    static const uint16_t resident_noops[] = {
        0x0600, 0x0601, 0x0602, 0x0603, 0x0702, 0x0703
    };
    const uint32_t success_flags = FLAGS_FIXED | FLAG_ZF;
    int failures = 0;

    vm->dpmi.virtual_interrupts_enabled = true;
    for (unsigned i = 0;
         i < sizeof(resident_noops) / sizeof(resident_noops[0]); i++) {
        uint32_t expected_eax = 0xA5A50000u | resident_noops[i];
        cpu->eax = expected_eax;
        cpu->ebx = 0xB6B61234u;
        cpu->ecx = 0xC7C75678u;
        cpu->edx = 0xD8D89ABCu;
        cpu->esi = 0xE9E9DEF0u;
        cpu->edi = 0xFAFA2468u;
        cpu->eflags = success_flags | FLAG_CF;
        dos_int31_dpmi(vm);
        if (cpu->eax != expected_eax || cpu->ebx != 0xB6B61234u ||
            cpu->ecx != 0xC7C75678u || cpu->edx != 0xD8D89ABCu ||
            cpu->esi != 0xE9E9DEF0u || cpu->edi != 0xFAFA2468u ||
            cpu->eflags != success_flags)
            failures++;
    }

    cpu->eax = 0xA5A50604u;
    cpu->ebx = 0xB6B61234u;
    cpu->ecx = 0xC7C75678u;
    cpu->edx = 0xD8D89ABCu;
    cpu->eflags = success_flags | FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu->eax != 0xA5A50604u || cpu->ebx != 0xB6B60000u ||
        cpu->ecx != (0xC7C70000u | DPMI_EXT_PAGE_SIZE) ||
        cpu->edx != 0xD8D89ABCu || cpu->eflags != success_flags)
        failures++;

    cpu->ebx = 0xB6B61234u;
    cpu->eax = 0xA5A50902u;
    cpu->eflags = success_flags | FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu->eax != 0xA5A50901u || cpu->ebx != 0xB6B61234u ||
        !vm->dpmi.virtual_interrupts_enabled ||
        cpu->eflags != success_flags)
        failures++;

    cpu->eax = 0xA5A50900u;
    cpu->eflags = success_flags | FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu->eax != 0xA5A50901u ||
        vm->dpmi.virtual_interrupts_enabled ||
        cpu->eflags != success_flags)
        failures++;

    cpu->eax = 0xA5A50902u;
    cpu->eflags = success_flags | FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu->eax != 0xA5A50900u ||
        vm->dpmi.virtual_interrupts_enabled ||
        cpu->eflags != success_flags)
        failures++;

    cpu->eax = 0xA5A50901u;
    cpu->eflags = success_flags | FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu->eax != 0xA5A50900u ||
        !vm->dpmi.virtual_interrupts_enabled ||
        cpu->eflags != success_flags)
        failures++;

    cpu->eax = 0xA5A57FFFu;
    cpu->eflags = success_flags;
    dos_int31_dpmi(vm);
    if (cpu->eax != 0xA5A58001u ||
        cpu->eflags != (success_flags | FLAG_CF))
        failures++;

    return failures;
}

int dpmi_selftest(void)
{

    uint64_t pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = (dos_vm_t *)dos_host_alloc_pages(pages);
    if (!vm) return 1;

    dpmi_zero(vm, pages * 4096u);
    cpu8086_state_t cpu;
    dpmi_zero(&cpu, sizeof(cpu));
    vm->cpu = &cpu;
    cpu.vm = vm;
    /* This descriptor-only fixture has no physical backing, but still
     * needs the same dormant-mode defaults as a bootstrapped DPMI host. */
    dpmi_init(vm);
    vm->total_mem_size = DOS_VM_ADDRESS_SPACE_SIZE;
    vm->system_mem_size = DOS_TOTAL_MEM;

    int entry_failures = dpmi_entry_selftest();
    entry_failures += dos_exec_dpmi_selftest();
    int rm_call_failures = dpmi_rm_call_selftest();
    rm_call_failures += dpmi_rm_arguments_selftest();
    rm_call_failures += dpmi_paging_reentry_selftest();
    rm_call_failures += dpmi_host_entry_frame_selftest();
    rm_call_failures += dpmi_callback_entry_recovery_selftest();
    int dos_memory_failures = dpmi_dos_memory_selftest();
    dos_memory_failures += dpmi_record_buffer_selftest();
    dos_memory_failures += dpmi_io_buffer_selftest();
    int service_failures = dpmi_page_interrupt_service_selftest(vm, &cpu);
    int failures = entry_failures + rm_call_failures +
                   dos_memory_failures + service_failures;
    if (entry_failures || rm_call_failures || dos_memory_failures ||
        service_failures) {
        serial_puts("[DPMI-TEST] entry/rm/memory/services=");
        serial_putdec(entry_failures);
        serial_puts("/");
        serial_putdec(rm_call_failures);
        serial_puts("/");
        serial_putdec(dos_memory_failures);
        serial_puts("/");
        serial_putdec(service_failures);
        serial_puts("\n");
    }
    int descriptor_failures_start = failures;

    unsigned rights_failures = 0;
    for (unsigned access = 0; access < 256; access++)
    for (unsigned flags = 0; flags < 256; flags++) {
        bool expected = !(flags & 0x20u) &&
            ((access >= 0x70u && access <= 0x7Fu) ||
             (access >= 0xF0u && access <= 0xF7u) || access == 0xFAu || access == 0xFBu);
        if (dpmi_rights_valid(access, flags) != expected) rights_failures++;
    }
    failures += rights_failures;
    serial_puts("[DPMI-RIGHTS] checks=65536 failures=");
    serial_putdec(rights_failures); serial_puts("\n");

    /* 000Dh may claim any free LDT entry, while ordinary allocation must
     * preserve the first 16 entries exclusively for specific requests. */
    const uint16_t specific_sel = 0x0004;
    const uint16_t specific_idx = dpmi_sel_to_index(specific_sel);
    cpu.ax = 0x000D;
    cpu.bx = specific_sel;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    uint8_t specific_access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                              DESC_WRITABLE;
    if ((cpu.eflags & FLAG_CF) ||
        vm->dpmi.descriptor_state[specific_idx] != DPMI_DESC_MUTABLE ||
        dpmi_desc_get_base(&vm->dpmi.ldt[specific_idx]) != 0 ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[specific_idx]) != 0 ||
        (vm->dpmi.ldt[specific_idx].access & specific_access) !=
            specific_access)
        failures++;

    /* The same LDT index with different RPL bits is still occupied. */
    cpu.ax = 0x000D;
    cpu.bx = specific_sel | 3u;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8011)
        failures++;

    cpu.ax = 0x000D;
    cpu.bx = 0x0008;  /* GDT selector */
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8022)
        failures++;

    cpu.ax = 0x000D;
    cpu.bx = (uint16_t)((DPMI_MAX_DESCRIPTORS << 3) | 0x04u);
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8022)
        failures++;

    const uint16_t high_specific_sel =
        (uint16_t)(((DPMI_MAX_DESCRIPTORS - 1u) << 3) | 0x04u);
    cpu.ax = 0x000D;
    cpu.bx = high_specific_sel;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    uint16_t high_specific_idx = dpmi_sel_to_index(high_specific_sel);
    if ((cpu.eflags & FLAG_CF) ||
        vm->dpmi.descriptor_state[high_specific_idx] != DPMI_DESC_MUTABLE)
        failures++;

    cpu.ax = 0x0001;
    cpu.bx = high_specific_sel | 3u;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) ||
        vm->dpmi.descriptor_state[high_specific_idx] != DPMI_DESC_FREE)
        failures++;

    cpu.ax = 0x0001;
    cpu.bx = specific_sel | 3u;
    cpu.eflags = FLAG_CF;
    cpu.ds = specific_sel;
    cpu.es = specific_sel | 1u;
    cpu.fs = specific_sel | 2u;
    cpu.gs = specific_sel | 3u;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) ||
        vm->dpmi.descriptor_state[specific_idx] != DPMI_DESC_FREE ||
        cpu.ds || cpu.es || cpu.fs || cpu.gs)
        failures++;

    cpu.ax = 0x0001;
    cpu.ds = cpu.es = cpu.fs = cpu.gs = 0x30;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8022 ||
        cpu.ds != 0x30 || cpu.es != 0x30 || cpu.fs != 0x30 || cpu.gs != 0x30)
        failures++;
    cpu.ds = cpu.es = cpu.fs = cpu.gs = 0;

    cpu.ax = 0x0000;
    cpu.cx = 1;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    uint16_t general_sel = cpu.ax;
    uint16_t general_idx = dpmi_sel_to_index(general_sel);
    if ((cpu.eflags & FLAG_CF) ||
        general_idx < DPMI_SPECIFIC_DESCRIPTOR_COUNT ||
        general_idx >= DPMI_MAX_DESCRIPTORS)
        failures++;

    cpu.ax = 0x0001;
    cpu.bx = general_sel;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) ||
        vm->dpmi.descriptor_state[general_idx] != DPMI_DESC_FREE)
        failures++;

    cpu.ax = 0x0002;
    cpu.bx = 0x1234;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);

    uint16_t sel = cpu.ax;
    uint16_t idx = dpmi_sel_to_index(sel);
    if ((cpu.eflags & FLAG_CF) || (sel & 0x07) != 0x07 ||
        idx < DPMI_SPECIFIC_DESCRIPTOR_COUNT ||
        idx >= DPMI_MAX_DESCRIPTORS) {
        failures++;
    } else {
        dpmi_descriptor_t *desc = &vm->dpmi.ldt[idx];
        uint8_t required = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                           DESC_WRITABLE;
        if (dpmi_desc_get_base(desc) != 0x12340 ||
            dpmi_desc_get_limit(desc) != 0xFFFF ||
            (desc->access & required) != required ||
            (desc->access & DESC_CODE) ||
            vm->dpmi.descriptor_state[idx] != DPMI_DESC_RM_ALIAS)
            failures++;
    }

    /* Repeated 0002h calls must return the same immutable selector. */
    cpu.ax = 0x0002;
    cpu.bx = 0x1234;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) || cpu.ax != sel)
        failures++;

    /* Vector setters accept only present, readable code selectors. */
    cpu.ax = 0x0000;
    cpu.cx = 2;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    uint16_t exception_sel = cpu.ax;
    uint16_t interrupt_sel = exception_sel + DPMI_SEL_INC;
    if (cpu.eflags & FLAG_CF) failures++;

    cpu.ax = 0x0009;
    cpu.bx = exception_sel;
    cpu.cx = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
             DESC_CODE | DESC_READABLE;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu.eflags & FLAG_CF) failures++;

    cpu.ax = 0x0009;
    cpu.bx = interrupt_sel;
    cpu.cx = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
             DESC_CODE | DESC_READABLE;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu.eflags & FLAG_CF) failures++;

    /* Exception handlers and interrupt vectors with the same number must not
     * overwrite each other (DPMI 0202h-0205h). These offsets use the USE32 ABI. */
    vm->dpmi.is_32bit = true;
    cpu.ax = 0x0203;
    cpu.bl = 0x0D;
    cpu.cx = exception_sel;
    cpu.edx = 0x11223344;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu.eflags & FLAG_CF) failures++;

    cpu.ax = 0x0205;
    cpu.bl = 0x0D;
    cpu.cx = interrupt_sel;
    cpu.edx = 0x55667788;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu.eflags & FLAG_CF) failures++;

    cpu.ax = 0x0202;
    cpu.bl = 0x0D;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) || cpu.cx != exception_sel ||
        cpu.edx != 0x11223344)
        failures++;

    cpu.ax = 0x0204;
    cpu.bl = 0x0D;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) || cpu.cx != interrupt_sel ||
        cpu.edx != 0x55667788)
        failures++;

    cpu.ax = 0x0203;
    cpu.bl = 0x0D;
    cpu.cx = 0xFFF7;
    cpu.edx = 0xDEADBEEF;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8022)
        failures++;

    cpu.ax = 0x0203;
    cpu.bl = 0x20;
    cpu.cx = exception_sel;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8021)
        failures++;

    cpu.ax = 0x0205;
    cpu.bl = 0x0D;
    cpu.cx = 0xFFF7;
    cpu.edx = 0xDEADBEEF;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8022)
        failures++;

    cpu.ax = 0x0202;
    cpu.bl = 0x0D;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) || cpu.cx != exception_sel ||
        cpu.edx != 0x11223344)
        failures++;

    cpu.ax = 0x0204;
    cpu.bl = 0x0D;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) || cpu.cx != interrupt_sel ||
        cpu.edx != 0x55667788)
        failures++;

    cpu.ax = 0x0001;
    cpu.bx = sel;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8022 ||
        vm->dpmi.descriptor_state[idx] != DPMI_DESC_RM_ALIAS)
        failures++;

    /* Present is descriptor content, not allocator metadata. */
    vm->dpmi.is_32bit = false;
    cpu.ax = 0x0000;
    cpu.cx = 1;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    uint16_t mutable_sel = cpu.ax;
    uint16_t mutable_idx = dpmi_sel_to_index(mutable_sel);
    if ((cpu.eflags & FLAG_CF) || mutable_idx >= DPMI_MAX_DESCRIPTORS ||
        vm->dpmi.descriptor_state[mutable_idx] != DPMI_DESC_MUTABLE ||
        dpmi_desc_get_base(&vm->dpmi.ldt[mutable_idx]) != 0 ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[mutable_idx]) != 0)
        failures++;

    cpu.ax = 0x0008;
    cpu.bx = mutable_sel;
    cpu.cx = 0x013F;
    cpu.dx = 0xFFFF;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[mutable_idx]) != 0x013FFFFFU ||
        !(vm->dpmi.ldt[mutable_idx].flags_lim & DESC_GRANULARITY))
        failures++;

    cpu.ax = 0x0008;
    cpu.bx = mutable_sel;
    cpu.cx = 0x0014;
    cpu.dx = 0x0000;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8021 ||
        dpmi_desc_get_limit(&vm->dpmi.ldt[mutable_idx]) != 0x013FFFFFU)
        failures++;

    cpu.ax = 0x0009;
    cpu.bx = mutable_sel;
    cpu.cx = DESC_DPL3 | DESC_SEGMENT | DESC_WRITABLE;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) ||
        (vm->dpmi.ldt[mutable_idx].access & DESC_PRESENT) ||
        vm->dpmi.descriptor_state[mutable_idx] != DPMI_DESC_MUTABLE)
        failures++;

    cpu.ax = 0x0006;
    cpu.bx = mutable_sel;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu.eflags & FLAG_CF) failures++;

    cpu.ax = 0x0001;
    cpu.bx = mutable_sel;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) ||
        vm->dpmi.descriptor_state[mutable_idx] != DPMI_DESC_FREE)
        failures++;

    vm->dpmi.is_32bit = true;
    cpu.ax = 0x0305;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    uint16_t host_sel = cpu.si;
    uint16_t host_idx = dpmi_sel_to_index(host_sel);
    if ((cpu.eflags & FLAG_CF) || cpu.ax != sizeof(dpmi_saved_state_t) ||
        cpu.bx != DPMI_ENTRY_SEG || cpu.cx != DPMI_SAVE_STATE_OFF ||
        cpu.edi != DPMI_SAVE_STATE_OFF ||
        host_idx >= DPMI_MAX_DESCRIPTORS ||
        vm->dpmi.descriptor_state[host_idx] != DPMI_DESC_HOST ||
        dpmi_desc_get_base(&vm->dpmi.ldt[host_idx]) !=
            ((uint32_t)DPMI_ENTRY_SEG << 4) ||
        !dpmi_selector_is_code(&vm->dpmi, host_sel) ||
        !(vm->dpmi.ldt[host_idx].flags_lim & DESC_32BIT))
        failures++;

    cpu.ax = 0x0306;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) || cpu.bx != DPMI_ENTRY_SEG ||
        cpu.cx != DPMI_RAW_SWITCH_OFF || cpu.si != host_sel ||
        cpu.edi != DPMI_RAW_SWITCH_OFF)
        failures++;

    cpu.ax = 0x0007;
    cpu.bx = host_sel;
    cpu.cx = 0;
    cpu.dx = 0;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8022 ||
        vm->dpmi.descriptor_state[host_idx] != DPMI_DESC_HOST)
        failures++;

    /* VBE linear framebuffers are physical mappings outside allocatable
     * DOS memory but inside the VM's guest-physical aperture. */
    cpu.ax = 0x0800;
    cpu.bx = (uint16_t)(DOS_VBE_FB_BASE >> 16);
    cpu.cx = (uint16_t)DOS_VBE_FB_BASE;
    cpu.si = 0;
    cpu.di = 4096;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if ((cpu.eflags & FLAG_CF) ||
        cpu.bx != (uint16_t)(DOS_VBE_FB_BASE >> 16) ||
        cpu.cx != (uint16_t)DOS_VBE_FB_BASE)
        failures++;

    cpu.ax = 0x0801;
    cpu.bx = (uint16_t)(DOS_VBE_FB_BASE >> 16);
    cpu.cx = (uint16_t)DOS_VBE_FB_BASE;
    cpu.eflags = FLAG_CF;
    dos_int31_dpmi(vm);
    if (cpu.eflags & FLAG_CF) failures++;

    cpu.ax = 0x0800;
    cpu.bx = (uint16_t)((DOS_VM_ADDRESS_SPACE_SIZE - 4096u) >> 16);
    cpu.cx = (uint16_t)(DOS_VM_ADDRESS_SPACE_SIZE - 4096u);
    cpu.si = 0;
    cpu.di = 8192;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8021)
        failures++;

    cpu.ax = 0x0001;
    cpu.bx = host_sel;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8022 ||
        vm->dpmi.descriptor_state[host_idx] != DPMI_DESC_HOST)
        failures++;

    uint16_t raw_data_sel = dpmi_alloc_descriptor(&vm->dpmi);
    uint16_t raw_stack_sel = dpmi_alloc_descriptor(&vm->dpmi);
    if (!raw_data_sel || !raw_stack_sel) {
        failures++;
    } else {
        uint8_t data_access = DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                              DESC_WRITABLE;
        dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(raw_data_sel)],
                        0, 0xFFFF, data_access, 0);
        dpmi_build_desc(&vm->dpmi.ldt[dpmi_sel_to_index(raw_stack_sel)],
                        0, 0xFFFF, data_access, 0);
        dpmi_desc_set_limit(&vm->dpmi.ldt[dpmi_sel_to_index(exception_sel)],
                            0xFFFF);

        vm->dpmi.is_32bit = false;
        cpu.protected_mode = false;
        cpu.pm_cs_loaded = false;
        cpu.cr0 &= ~1u;
        cpu.ax = raw_data_sel;
        cpu.cx = raw_data_sel;
        cpu.dx = raw_stack_sel;
        cpu.ebx = 0x1234;
        cpu.esi = exception_sel;
        cpu.edi = 0x5678;
        cpu.fs = 0x1111;
        cpu.gs = 0x2222;
        if (!dpmi_raw_mode_switch(vm, 0) || !cpu.protected_mode ||
            !cpu.pm_cs_loaded || !(cpu.cr0 & 1u) ||
            cpu.ds != raw_data_sel || cpu.es != raw_data_sel ||
            cpu.ss != raw_stack_sel || cpu.esp != 0x1234 ||
            cpu.cs != exception_sel || cpu.eip != 0x5678 ||
            cpu.fs != 0 || cpu.gs != 0)
            failures++;

        cpu.ax = 0x1111;
        cpu.cx = 0x2222;
        cpu.dx = 0x3333;
        cpu.ebx = 0x4444;
        cpu.esi = 0x5555;
        cpu.edi = 0x6666;
        cpu.op_size_32 = true;
        cpu.addr_size_32 = true;
        if (!dpmi_raw_mode_switch(vm, 0) || cpu.protected_mode ||
            cpu.pm_cs_loaded || (cpu.cr0 & 1u) || cpu.ds != 0x1111 ||
            cpu.es != 0x2222 || cpu.ss != 0x3333 || cpu.sp != 0x4444 ||
            cpu.cs != 0x5555 || cpu.ip != 0x6666 || cpu.op_size_32 ||
            cpu.addr_size_32 || cpu.fs != 0 || cpu.gs != 0)
            failures++;

        cpu.protected_mode = false;
        cpu.ax = raw_data_sel;
        cpu.cx = raw_data_sel;
        cpu.dx = raw_stack_sel;
        cpu.esi = 0xFFF7;
        cpu.edi = 0;
        if (dpmi_raw_mode_switch(vm, 0) || cpu.protected_mode)
            failures++;
    }

    for (unsigned i = 0; i < DPMI_MAX_DESCRIPTORS; i++)
        vm->dpmi.descriptor_state[i] = DPMI_DESC_MUTABLE;
    cpu.ax = 0x0002;
    cpu.bx = 0x2000;
    cpu.eflags = 0;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8011)
        failures++;

    if (failures != descriptor_failures_start) {
        serial_puts("[DPMI-TEST] descriptor/service body failures=");
        serial_putdec(failures - descriptor_failures_start);
        serial_puts("\n");
    }
    dos_host_free_pages(vm, pages);
    return failures;
}
