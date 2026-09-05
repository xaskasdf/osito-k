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

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* DOS INT dispatch (for real-mode interrupt simulation) */
extern void dos_int21_dispatch(dos_vm_t *vm);
extern void dos_int10_video(dos_vm_t *vm);
extern void dos_int16_keyboard(dos_vm_t *vm);
extern void dos_int1a_timer(dos_vm_t *vm);
extern void dos_int_dispatch(dos_vm_t *vm, uint8_t int_num);

/* ── Helper: zero memory (no libc) ─────────────────────────────── */

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
    if (total <= DPMI_EXT_BASE + DPMI_CALLBACK_STACK_AREA)
        return DPMI_EXT_BASE;
    return total - DPMI_CALLBACK_STACK_AREA;
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

    /* 0305h is a FAR-call target and may return immediately because AX=0.
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
        vm->mem[save_stub] = 0xCB;                 /* RETF */
        vm->mem[ms_stub] = 0xCD;                   /* INT imm8 */
        vm->mem[ms_stub + 1] = DPMI_RAW_SWITCH_INT;
    }
    if (exception_stub + 3 <= vm->total_mem_size) {
        vm->mem[exception_stub] = 0xCD;             /* INT imm8 */
        vm->mem[exception_stub + 1] = DPMI_EXCEPTION_RETURN_INT;
        vm->mem[exception_stub + 2] = 0xF4;         /* must not return */
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

    serial_puts("[DPMI] Host initialized, entry at F000:0100,"
                " save F000:0110, mode-switch F000:0118,"
                " exception-return F000:0120, callback-return F000:0140,"
                " reflectors F000:1000/1400\n");
}

/* ══════════════════════════════════════════════════════════════════
 * 2. dpmi_translate -- Selector:offset to linear address
 * ══════════════════════════════════════════════════════════════════ */

static bool dpmi_descriptor_at(dos_vm_t *vm, uint32_t address,
                               dpmi_descriptor_t *descriptor)
{
    if (!vm || !vm->mem || !descriptor ||
        address > vm->total_mem_size ||
        sizeof(*descriptor) > vm->total_mem_size - address)
        return false;

    uint8_t *bytes = (uint8_t *)descriptor;
    for (unsigned i = 0; i < sizeof(*descriptor); i++)
        bytes[i] = dos_mem_read8(vm, address + i);
    return true;
}

static bool dpmi_gdt_descriptor(dos_vm_t *vm, uint16_t selector,
                                dpmi_descriptor_t *descriptor)
{
    if (!vm || !vm->cpu || (selector & 0x04u) ||
        (selector >> 3) == 0)
        return false;

    uint32_t offset = (uint32_t)(selector & 0xFFF8u);
    if (!vm->cpu->gdtr.base || offset + 7u > vm->cpu->gdtr.limit)
        return false;
    return dpmi_descriptor_at(vm, vm->cpu->gdtr.base + offset,
                              descriptor);
}

bool dpmi_guest_descriptor(dos_vm_t *vm, uint16_t selector,
                           dpmi_descriptor_t *descriptor)
{
    if (!vm || !vm->cpu || !descriptor || (selector & ~3u) == 0)
        return false;

    if (!(selector & 0x04u))
        return dpmi_gdt_descriptor(vm, selector, descriptor);

    uint16_t index = selector >> 3;
    if (!vm->cpu->ldtr) {
        if (index >= DPMI_MAX_DESCRIPTORS ||
            vm->dpmi.descriptor_state[index] == DPMI_DESC_FREE)
            return false;
        *descriptor = vm->dpmi.ldt[index];
        return true;
    }

    dpmi_descriptor_t ldt_descriptor;
    if (!dpmi_gdt_descriptor(vm, vm->cpu->ldtr, &ldt_descriptor) ||
        !(ldt_descriptor.access & DESC_PRESENT) ||
        (ldt_descriptor.access & DESC_SEGMENT) ||
        (ldt_descriptor.access & 0x0Fu) != 0x02u)
        return false;

    uint32_t offset = (uint32_t)index * sizeof(dpmi_descriptor_t);
    uint32_t limit = dpmi_desc_get_limit(&ldt_descriptor);
    if (offset + sizeof(dpmi_descriptor_t) - 1u > limit)
        return false;
    return dpmi_descriptor_at(vm,
                              dpmi_desc_get_base(&ldt_descriptor) + offset,
                              descriptor);
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

    /* DPMI only permits readable, non-conforming code descriptors. */
    if ((access & DESC_CODE) &&
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
    if (dpmi_selector_in_run(cpu->ds, start, count)) cpu->ds = 0;
    if (dpmi_selector_in_run(cpu->es, start, count)) cpu->es = 0;
    if (dpmi_selector_in_run(cpu->fs, start, count)) cpu->fs = 0;
    if (dpmi_selector_in_run(cpu->gs, start, count)) cpu->gs = 0;
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

bool dpmi_raw_mode_switch(dos_vm_t *vm)
{
    if (!vm || !vm->cpu)
        return false;

    cpu8086_state_t *cpu = vm->cpu;
    uint16_t next_ds = cpu->ax;
    uint16_t next_es = cpu->cx;
    uint16_t next_ss = cpu->dx;
    uint16_t next_cs = cpu->si;

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
        cpu->cr0 |= 1u;
        cpu->halted = false;
        serial_puts("[DPMI] Raw switch RM->PM\n");
        return true;
    }

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
    cpu->cr0 &= ~1u;
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

    /* 0204h is specified as always successful. Reserve the host code
     * selector while entry is still transactional so every default vector
     * remains available even if the client later exhausts the LDT. */
    dpmi->is_32bit = is_32bit;
    if (!dpmi_host_code_selector(dpmi)) {
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
    dpmi->is_32bit = is_32bit;
    dpmi->virtual_interrupts_enabled = (return_flags & FLAG_IF) != 0;

    serial_puts("[DPMI] Mode switch: ");
    serial_puts(is_32bit ? "32-bit" : "16-bit");
    serial_puts(" client\n");

    /* Switch CPU to protected mode (USE16) */
    cpu->protected_mode = true;
    cpu->pm_cs_loaded   = true;
    cpu->op_size_32     = false;
    cpu->addr_size_32   = false;
    cpu->cr0           |= 1;

    /* Set segment registers to PM selectors */
    cpu->cs = dpmi->sel_code;
    cpu->ds = dpmi->sel_data;
    cpu->es = dpmi->sel_psp;
    cpu->ss = dpmi->sel_stack;
    cpu->fs = 0;
    cpu->gs = 0;

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
    dpmi->active = false;
    dpmi->is_32bit = false;
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

static bool dpmi_client_buffer(dos_vm_t *vm, uint16_t selector,
                               uint32_t offset, uint32_t size,
                               uint32_t *linear)
{
    if (!size || (selector & ~3u) == 0)
        return false;

    if (selector & 0x04) {
        uint16_t index;
        if (!dpmi_selector_is_data(&vm->dpmi, selector, true) ||
            !dpmi_selector_allocated(&vm->dpmi, selector, &index))
            return false;

        uint32_t limit = dpmi_desc_get_limit(&vm->dpmi.ldt[index]);
        if (offset > limit || size - 1u > limit - offset)
            return false;
    }

    uint32_t address = dpmi_translate(vm, selector, offset);
    if (!dpmi_range_valid(vm, address, size))
        return false;
    if (linear) *linear = address;
    return true;
}

static bool dpmi_code_target(dos_vm_t *vm, uint16_t selector,
                             uint32_t offset)
{
    uint16_t index;
    if (!dpmi_selector_is_code(&vm->dpmi, selector) ||
        !dpmi_selector_allocated(&vm->dpmi, selector, &index) ||
        offset > dpmi_desc_get_limit(&vm->dpmi.ldt[index]))
        return false;

    return dpmi_range_valid(vm, dpmi_translate(vm, selector, offset), 1);
}

static uint16_t dpmi_callback_stack_selector(dos_vm_t *vm)
{
    dpmi_state_t *dpmi = &vm->dpmi;
    uint16_t index;
    uint32_t base = dpmi_ext_limit(vm);
    if (base < DPMI_EXT_BASE ||
        !dpmi_range_valid(vm, base, DPMI_CALLBACK_STACK_AREA))
        return 0;

    if (dpmi_selector_allocated(dpmi, dpmi->callback_stack_sel, &index) &&
        dpmi->descriptor_state[index] == DPMI_DESC_HOST) {
        dpmi_desc_set_base(&dpmi->ldt[index], base);
        if (dpmi->is_32bit)
            dpmi->ldt[index].flags_lim |= DESC_32BIT;
        else
            dpmi->ldt[index].flags_lim &= ~DESC_32BIT;
        return dpmi->callback_stack_sel;
    }

    uint16_t selector = dpmi_alloc_descriptor(dpmi);
    if (!selector) return 0;

    index = dpmi_sel_to_index(selector);
    dpmi_build_desc(&dpmi->ldt[index], base,
                    DPMI_CALLBACK_STACK_AREA - 1u,
                    DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                    DESC_WRITABLE,
                    dpmi->is_32bit ? DESC_32BIT : 0);
    dpmi->descriptor_state[index] = DPMI_DESC_HOST;
    dpmi->callback_stack_sel = selector;
    return selector;
}

bool dpmi_control_break(dos_vm_t *vm)
{
    dpmi_state_t *dpmi = &vm->dpmi;
    cpu8086_state_t *cpu = vm->cpu;
    uint16_t selector = dpmi->pm_vectors[0x23].sel;
    uint32_t offset = dpmi->pm_vectors[0x23].off;
    if (!selector || (selector == dpmi->sel_host_code &&
        offset == DPMI_PM_REFLECT_BASE_OFF + 0x23u * DPMI_PM_REFLECT_STUB_SIZE))
        return true;

    uint16_t host_cs = dpmi_get_host_code_selector(vm);
    uint16_t host_ss = dpmi_callback_stack_selector(vm);
    if (!host_cs || !host_ss || !dpmi_code_target(vm, selector, offset) ||
        dpmi->callback_depth >= DPMI_MAX_CALLBACKS)
        return false;

    cpu8086_state_t saved = *cpu;
    bool saved_virtual_if = dpmi->virtual_interrupts_enabled;
    uint32_t top = (++dpmi->callback_depth) * DPMI_CALLBACK_STACK_SIZE - 16u;
    /* Use the same resident stack pool as real-mode callbacks. This also
     * covers INT 23h reflected from a simulated real-mode DOS service. */
    cpu->protected_mode = true;
    cpu->cr0 |= 1u;
    cpu->ss = host_ss;
    cpu->esp = top;
    if (!saved.protected_mode)
        cpu->ds = cpu->es = cpu->fs = cpu->gs = 0;
    cpu->eflags = (saved.eflags & ~(FLAG_CF | FLAG_TF)) | FLAGS_FIXED | FLAG_IF;
    if (dpmi->is_32bit) {
        cpu_push32(cpu, cpu->eflags);
        cpu_push32(cpu, host_cs);
        cpu_push32(cpu, DPMI_CONTROL_RETURN_OFF);
    } else {
        cpu_push16(cpu, cpu->flags);
        cpu_push16(cpu, host_cs);
        cpu_push16(cpu, DPMI_CONTROL_RETURN_OFF);
    }
    cpu->cs = selector;
    cpu->eip = offset;
    cpu->halted = false;
    cpu8086_sync_cs(cpu);
    vm->native_dispatch_depth++;
    bool returned = cpu8086_run_until(vm, true, host_cs,
                                      DPMI_CONTROL_RETURN_OFF);
    vm->native_dispatch_depth--;
    bool valid = returned && cpu->ss == host_ss && cpu_stack_offset(cpu) == top;
    bool running = cpu->running;
    int32_t exit_code = cpu->exit_code;
    uint64_t instructions = cpu->insn_count;
    *cpu = saved;
    cpu->insn_count = instructions;
    if (!running) {
        cpu->running = false;
        cpu->exit_code = exit_code;
    }
    dpmi->callback_depth--;
    dpmi->virtual_interrupts_enabled = saved_virtual_if;
    return valid;
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

bool dpmi_callback_enter(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    dpmi_state_t *dpmi = vm ? &vm->dpmi : NULL;
    if (!cpu || !dpmi || cpu->protected_mode ||
        cpu->cs != DPMI_ENTRY_SEG || cpu->ip < 2u ||
        dpmi->callback_depth >= DPMI_MAX_CALLBACKS)
        return false;

    uint32_t private_frame = dos_linear(cpu->ss, cpu->sp);
    if (!dpmi_range_valid(vm, private_frame, 6u))
        return false;

    uint8_t slot = dos_mem_read8(vm, dos_linear(cpu->cs, cpu->ip));
    if (slot >= DPMI_MAX_CALLBACKS)
        return false;

    dpmi_callback_t *callback = &dpmi->callbacks[slot];
    if (!callback->active || callback->rm_seg != cpu->cs ||
        callback->rm_off + 2u != cpu->ip ||
        !dpmi_code_target(vm, callback->pm_sel, callback->pm_off))
        return false;

    uint32_t regs_address;
    if (!dpmi_client_buffer(vm, callback->rm_regs_sel,
                            callback->rm_regs_off,
                            sizeof(dpmi_rm_regs_t), &regs_address))
        return false;

    uint16_t host_stack_sel = dpmi_callback_stack_selector(vm);
    uint16_t host_code_sel = dpmi_get_host_code_selector(vm);
    uint16_t rm_stack_index;
    if (!host_stack_sel || !host_code_sel ||
        !dpmi_selector_allocated(dpmi, callback->rm_stack_sel,
                                 &rm_stack_index) ||
        dpmi->descriptor_state[rm_stack_index] != DPMI_DESC_HOST)
        return false;

    uint16_t caller_sp = (uint16_t)(cpu->sp + 6u);
    dpmi_rm_regs_t regs;
    dpmi_zero(&regs, sizeof(regs));
    regs.eax = cpu->eax;
    regs.ebx = cpu->ebx;
    regs.ecx = cpu->ecx;
    regs.edx = cpu->edx;
    regs.esi = cpu->esi;
    regs.edi = cpu->edi;
    regs.ebp = cpu->ebp;
    regs.flags = dos_mem_read16(vm, private_frame + 4u);
    regs.es = cpu->es;
    regs.ds = cpu->ds;
    regs.fs = cpu->fs;
    regs.gs = cpu->gs;
    regs.ip = dos_mem_read16(vm, private_frame);
    regs.cs = dos_mem_read16(vm, private_frame + 2u);
    regs.sp = caller_sp;
    regs.ss = cpu->ss;
    dpmi_write_rm_regs(vm, regs_address, &regs);

    dpmi_build_desc(&dpmi->ldt[rm_stack_index], (uint32_t)cpu->ss << 4,
                    0xFFFFu,
                    DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                    DESC_WRITABLE,
                    0);
    dpmi->descriptor_state[rm_stack_index] = DPMI_DESC_HOST;

    cpu_stack_adjust(cpu, 6); /* discard the private real-mode INT frame */
    uint8_t depth = dpmi->callback_depth;
    dpmi->callback_slots[depth] = slot;
    dpmi->callback_depth++;

    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->cr0 |= 1u;
    cpu->ss = host_stack_sel;
    cpu->esp = (uint32_t)(depth + 1u) * DPMI_CALLBACK_STACK_SIZE;
    cpu->eflags = FLAGS_FIXED;
    if (dpmi->is_32bit) {
        cpu_push32(cpu, cpu->eflags);
        cpu_push32(cpu, host_code_sel);
        cpu_push32(cpu, DPMI_CALLBACK_RETURN_OFF);
    } else {
        cpu_push16(cpu, cpu->flags);
        cpu_push16(cpu, host_code_sel);
        cpu_push16(cpu, DPMI_CALLBACK_RETURN_OFF);
    }

    uint32_t callback_frame = dos_addr(vm, cpu->ss,
                                       cpu_stack_offset(cpu));
    serial_puts("[DPMI] Callback enter slot=");
    serial_putdec(slot);
    serial_puts(" SS:SP=");
    serial_puthex(cpu->ss, 4);
    serial_puts(":");
    serial_puthex(cpu_stack_offset(cpu), 8);
    serial_puts(" frame=");
    serial_puthex(dos_mem_read32(vm, callback_frame), 8);
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
    return true;
}

bool dpmi_callback_return(dos_vm_t *vm, bool discard_private_int_frame)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    dpmi_state_t *dpmi = vm ? &vm->dpmi : NULL;
    if (!cpu || !dpmi || !cpu->protected_mode ||
        !dpmi->callback_depth || cpu->cs != dpmi->sel_host_code ||
        cpu->eip != DPMI_CALLBACK_RETURN_OFF + 2u)
        return false;

    uint8_t slot = dpmi->callback_slots[dpmi->callback_depth - 1u];
    if (slot >= DPMI_MAX_CALLBACKS || !dpmi->callbacks[slot].active)
        return false;

    uint32_t regs_offset = dpmi->is_32bit ? cpu->edi : cpu->di;
    uint32_t regs_address;
    if (!dpmi_client_buffer(vm, cpu->es, regs_offset,
                            sizeof(dpmi_rm_regs_t), &regs_address))
        return false;

    if (discard_private_int_frame) {
        uint32_t frame_size = dpmi->is_32bit ? 12u : 6u;
        uint32_t frame = dos_addr(vm, cpu->ss, cpu_stack_offset(cpu));
        if (!dpmi_range_valid(vm, frame, frame_size))
            return false;
        cpu_stack_adjust(cpu, (int32_t)frame_size);
    }

    dpmi_rm_regs_t regs;
    dpmi_read_rm_regs(vm, regs_address, &regs);
    dpmi->callback_depth--;

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
    cpu->cr0 &= ~1u;
    cpu->halted = false;
    return true;
}

static bool dpmi_copy_pm_stack_words(dos_vm_t *vm, uint16_t source_sel,
                                     uint32_t source_off,
                                     uint16_t destination_seg,
                                     uint16_t destination_off,
                                     uint16_t words)
{
    if (!words) return true;

    uint32_t bytes = (uint32_t)words * 2u;
    uint32_t source = dos_addr(vm, source_sel, source_off);
    uint32_t destination = dos_linear(destination_seg, destination_off);
    if (!dpmi_range_valid(vm, source, bytes) ||
        !dpmi_range_valid(vm, destination, bytes))
        return false;

    bool backwards = destination > source && destination < source + bytes;
    for (uint32_t n = 0; n < words; n++) {
        uint32_t i = backwards ? (uint32_t)words - 1u - n : n;
        uint32_t src = dos_addr(vm, source_sel, source_off + i * 2u);
        uint32_t dst = dos_linear(destination_seg,
                                  (uint16_t)(destination_off + i * 2u));
        if (!dpmi_range_valid(vm, src, 2) ||
            !dpmi_range_valid(vm, dst, 2))
            return false;
        dos_mem_write16(vm, dst, dos_mem_read16(vm, src));
    }
    return true;
}

static bool dpmi_simulate_rm_call(dos_vm_t *vm, dpmi_rm_regs_t *regs,
                                  uint16_t copy_words,
                                  dpmi_rm_call_kind_t kind,
                                  uint8_t interrupt_number)
{
    cpu8086_state_t *cpu = vm->cpu;
    cpu8086_state_t protected_state = *cpu;
    uint32_t source_stack = cpu_stack_offset(cpu);
    uint32_t copied_bytes = (uint32_t)copy_words * 2u;
    uint32_t frame_bytes = kind == DPMI_RM_CALL_FAR ? 4u : 6u;
    if (kind == DPMI_RM_CALL_INTERRUPT)
        frame_bytes = 10u;

    uint16_t real_ss = regs->ss;
    uint16_t real_sp = regs->sp;
    if (!real_ss && !real_sp) {
        real_ss = DPMI_ENTRY_SEG;
        real_sp = DPMI_RM_STACK_TOP;
    }

    if (copied_bytes + frame_bytes > real_sp)
        return false;

    uint16_t arguments_sp = (uint16_t)(real_sp - copied_bytes);
    uint16_t stack_low = (uint16_t)(arguments_sp - frame_bytes);
    if (!dpmi_range_valid(vm, dos_linear(real_ss, stack_low),
                          copied_bytes + frame_bytes) ||
        !dpmi_copy_pm_stack_words(vm, protected_state.ss, source_stack,
                                  real_ss, arguments_sp, copy_words))
        return false;

    uint16_t target_cs = regs->cs;
    uint16_t target_ip = regs->ip;
    bool direct_interrupt = false;
    if (kind == DPMI_RM_CALL_INTERRUPT) {
        uint32_t vector = (uint32_t)interrupt_number * 4u;
        target_ip = dos_mem_read16(vm, vector);
        target_cs = dos_mem_read16(vm, vector + 2u);
        direct_interrupt = (target_cs || target_ip) &&
                           target_cs < (DOS_ROM_BASE >> 4);

        if (!direct_interrupt) {
            uint32_t stub = dos_linear(DPMI_ENTRY_SEG,
                                       DPMI_RM_INT_STUB_OFF);
            if (!dpmi_range_valid(vm, stub, 3))
                return false;
            vm->mem[stub] = 0xCD;
            vm->mem[stub + 1u] = interrupt_number;
            vm->mem[stub + 2u] = 0xCB;
            target_cs = DPMI_ENTRY_SEG;
            target_ip = DPMI_RM_INT_STUB_OFF;
        }
    }

    if (!dpmi_range_valid(vm, dos_linear(target_cs, target_ip), 1))
        return false;

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
    cpu->esp = arguments_sp;
    cpu->cs = target_cs;
    cpu->eip = target_ip;
    cpu->eflags = (uint32_t)regs->flags | FLAGS_FIXED;
    cpu->protected_mode = false;
    cpu->pm_cs_loaded = false;
    cpu->op_size_32 = false;
    cpu->addr_size_32 = false;
    cpu->cr0 &= ~1u;
    cpu->halted = false;

    if (kind == DPMI_RM_CALL_FAR ||
        (kind == DPMI_RM_CALL_INTERRUPT && !direct_interrupt)) {
        cpu_push16(cpu, DPMI_ENTRY_SEG);
        cpu_push16(cpu, DPMI_RM_RETURN_OFF);
    } else {
        cpu_push16(cpu, cpu->flags | FLAGS_FIXED);
        cpu_push16(cpu, DPMI_ENTRY_SEG);
        cpu_push16(cpu, DPMI_RM_RETURN_OFF);
        cpu->flags &= ~(FLAG_IF | FLAG_TF);
    }

    bool reached = cpu8086_run_until_real(vm, DPMI_ENTRY_SEG,
                                          DPMI_RM_RETURN_OFF);
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
    cpu->insn_count = instruction_count;
    if (!nested_running) {
        cpu->running = false;
        cpu->exit_code = nested_exit_code;
    }
    return reached;
}

static bool dpmi_default_return_flags_address(dos_vm_t *vm,
                                              uint8_t private_frame_bytes,
                                              uint32_t *address)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    if (!cpu || !address || !cpu->protected_mode || !vm->dpmi.active)
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

    return dpmi_client_buffer(vm, cpu->ss, offset,
                              vm->dpmi.is_32bit ? 4u : 2u, address);
}

bool dpmi_dispatch_default_interrupt(dos_vm_t *vm, uint8_t int_num,
                                     uint8_t private_frame_bytes)
{
    cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    uint32_t return_flags_address;
    if (!cpu || !cpu->protected_mode || !vm->dpmi.active ||
        !dpmi_default_return_flags_address(vm, private_frame_bytes,
                                           &return_flags_address))
        return false;

    uint32_t reflected_flags;
    if (int_num == 0x23u) {
        /* The default protected Ctrl-C handler ignores the notification;
         * chaining here must not fall back to the real-mode abort handler. */
        reflected_flags = cpu->eflags;
    } else if (dos_int_has_pm_translator(int_num)) {
        dos_int_dispatch(vm, int_num);
        reflected_flags = cpu->eflags;
    } else {
        dpmi_rm_regs_t regs;
        dpmi_zero(&regs, sizeof(regs));
        regs.eax = cpu->eax;
        regs.ebx = cpu->ebx;
        regs.ecx = cpu->ecx;
        regs.edx = cpu->edx;
        regs.esi = cpu->esi;
        regs.edi = cpu->edi;
        regs.ebp = cpu->ebp;
        regs.flags = cpu->flags;

        if (!dpmi_simulate_rm_call(vm, &regs, 0,
                                   DPMI_RM_CALL_INTERRUPT, int_num))
            return false;

        cpu->eax = regs.eax;
        cpu->ebx = regs.ebx;
        cpu->ecx = regs.ecx;
        cpu->edx = regs.edx;
        cpu->esi = regs.esi;
        cpu->edi = regs.edi;
        cpu->ebp = regs.ebp;
        reflected_flags = regs.flags;
    }

    /* Segment registers and SP never cross the DPMI reflection boundary.
     * IF remains virtualized; return the flags that user mode can observe. */
    const uint32_t reflected_mask = FLAG_CF | FLAG_PF | FLAG_AF |
                                    FLAG_ZF | FLAG_SF | FLAG_TF |
                                    FLAG_DF | FLAG_OF;
    if (vm->dpmi.is_32bit) {
        uint32_t saved = dos_mem_read32(vm, return_flags_address);
        saved = (saved & ~reflected_mask) |
                (reflected_flags & reflected_mask) | FLAGS_FIXED;
        dos_mem_write32(vm, return_flags_address, saved);
    } else {
        uint16_t saved = dos_mem_read16(vm, return_flags_address);
        saved = (uint16_t)((saved & ~reflected_mask) |
                           (reflected_flags & reflected_mask) | FLAGS_FIXED);
        dos_mem_write16(vm, return_flags_address, saved);
    }
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
        uint16_t idx;
        if (!dpmi_selector_allocated(dpmi, sel, &idx)) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8022;
            break;
        }
        /* Copy 8-byte descriptor to ES:EDI (or ES:DI in 16-bit) */
        uint32_t dst = dpmi_translate(vm, cpu->es,
                       dpmi->is_32bit ? cpu->edi : cpu->di);
        dpmi_descriptor_t *d = &dpmi->ldt[idx];
        for (int i = 0; i < 8; i++)
            dos_mem_write8(vm, dst + i, ((uint8_t *)d)[i]);
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=000Ch: Set Descriptor ────────────────────────────────── */
    case 0x000C: {
        uint16_t sel = cpu->bx;
        uint16_t idx;
        if (!dpmi_selector_mutable(dpmi, sel, &idx)) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8022;
            break;
        }
        /* Read 8-byte descriptor from ES:EDI (or ES:DI in 16-bit) */
        uint32_t src = dpmi_translate(vm, cpu->es,
                       dpmi->is_32bit ? cpu->edi : cpu->di);
        dpmi_descriptor_t next;
        for (int i = 0; i < 8; i++)
            ((uint8_t *)&next)[i] = dos_mem_read8(vm, src + i);
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

        /* AX=0 means there is no private state buffer. The same RETF byte is
         * exposed through a real-mode segment and an immutable PM selector. */
        cpu->ax  = 0;                   /* state buffer size = 0 */
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
        uint16_t seg = dos_mem_alloc(vm, paragraphs, &allocation_largest);
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
    case 0x0202: {
        uint8_t exc_num = cpu->bl;
        if (exc_num > 31) {
            cpu->eflags |= FLAG_CF;
            cpu->ax = 0x8021;
            break;
        }
        cpu->cx  = dpmi->exception_vectors[exc_num].sel;
        cpu->edx = dpmi->exception_vectors[exc_num].off;
        cpu->eflags &= ~FLAG_CF;
        break;
    }

    /* ── AX=0203h: Set Processor Exception Handler Vector ────────── */
    case 0x0203: {
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
        dpmi->exception_vectors[exc_num].sel = cpu->cx;
        dpmi->exception_vectors[exc_num].off = cpu->edx;
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
        uint32_t regs_addr;
        if (cpu->bh & 0xFEu) {
            cpu->ax = 0x8021;
            cpu->eflags |= FLAG_CF;
            break;
        }
        if (!dpmi_client_buffer(vm, cpu->es, regs_offset,
                                sizeof(dpmi_rm_regs_t), &regs_addr)) {
            cpu->ax = 0x8022;
            cpu->eflags |= FLAG_CF;
            break;
        }

        dpmi_rm_regs_t regs;
        dpmi_read_rm_regs(vm, regs_addr, &regs);

        dpmi_rm_call_kind_t kind = DPMI_RM_CALL_INTERRUPT;
        if (func == 0x0301)
            kind = DPMI_RM_CALL_FAR;
        else if (func == 0x0302)
            kind = DPMI_RM_CALL_IRET;

        uint8_t interrupt_number = cpu->bl;
        uint16_t copy_words = cpu->cx;
        bool completed = dpmi_simulate_rm_call(vm, &regs, copy_words,
                                               kind, interrupt_number);
        dpmi_write_rm_regs(vm, regs_addr, &regs);

        if (completed) {
            cpu->eflags &= ~FLAG_CF;
        } else if (cpu->running) {
            cpu->ax = 0x8021;
            cpu->eflags |= FLAG_CF;
        }
        break;
    }

    case 0x0303: {
        /* DS:ESI = PM procedure, ES:EDI = RM register structure */
        uint32_t pm_offset = dpmi->is_32bit ? cpu->esi : cpu->si;
        uint32_t regs_offset = dpmi->is_32bit ? cpu->edi : cpu->di;
        if (!dpmi_code_target(vm, cpu->ds, pm_offset) ||
            !dpmi_client_buffer(vm, cpu->es, regs_offset,
                                sizeof(dpmi_rm_regs_t), NULL) ||
            !dpmi_callback_stack_selector(vm) ||
            !dpmi_get_host_code_selector(vm)) {
            cpu->ax = 0x8015;
            cpu->eflags |= FLAG_CF;
            break;
        }

        int slot = -1;
        for (int i = 0; i < DPMI_MAX_CALLBACKS; i++) {
            if (!dpmi->callbacks[i].active) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            cpu->ax = 0x8015;  /* callback unavailable */
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint16_t rm_stack_sel = dpmi_alloc_descriptor(dpmi);
        if (!rm_stack_sel) {
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
        uint16_t cb_off = DPMI_CALLBACK_BASE_OFF +
                          (uint16_t)(slot * DPMI_CALLBACK_STUB_SIZE);
        uint32_t cb_addr = dos_linear(DPMI_ENTRY_SEG, cb_off);
        if (!dpmi_range_valid(vm, cb_addr, DPMI_CALLBACK_STUB_SIZE)) {
            dpmi_release_descriptor(dpmi, rm_stack_index);
            cpu->ax = 0x8015;
            cpu->eflags |= FLAG_CF;
            break;
        }

        dpmi_callback_t *callback = &dpmi->callbacks[slot];
        dpmi_zero(callback, sizeof(*callback));
        callback->rm_seg = DPMI_ENTRY_SEG;
        callback->rm_off = cb_off;
        callback->pm_sel = cpu->ds;
        callback->pm_off = pm_offset;
        callback->rm_regs_sel = cpu->es;
        callback->rm_regs_off = regs_offset;
        callback->rm_stack_sel = rm_stack_sel;

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
        uint32_t info_addr;
        if (!dpmi_client_buffer(vm, cpu->es, cpu->edi, 48u,
                                &info_addr)) {
            cpu->ax = 0x8022;
            cpu->eflags |= FLAG_CF;
            break;
        }

        uint32_t total_pages = dpmi_ext_total_pages(vm);
        uint32_t free_pages = dpmi_ext_free_page_count(vm);
        uint32_t largest_pages = dpmi_ext_largest_free_page_count(vm);
        uint32_t allocated_pages = total_pages - free_pages;

        dos_mem_write32(vm, info_addr + 0x00,
                        largest_pages * DPMI_EXT_PAGE_SIZE);
        dos_mem_write32(vm, info_addr + 0x04, largest_pages);
        dos_mem_write32(vm, info_addr + 0x08, largest_pages);
        dos_mem_write32(vm, info_addr + 0x0C, total_pages);
        dos_mem_write32(vm, info_addr + 0x10, allocated_pages);
        dos_mem_write32(vm, info_addr + 0x14, free_pages);
        dos_mem_write32(vm, info_addr + 0x18, total_pages);
        dos_mem_write32(vm, info_addr + 0x1C, free_pages);
        dos_mem_write32(vm, info_addr + 0x20, 0); /* no paging file */
        for (uint32_t offset = 0x24; offset < 0x30; offset += 4)
            dos_mem_write32(vm, info_addr + offset, 0xFFFFFFFFu);

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

    /* POP return IP/CS and the copied argument, rebuild the FAR frame,
     * return AX/BX as evidence that the procedure really executed. */
    uint32_t far_code = dos_linear(rm_segment, far_offset);
    const uint8_t far_program[] = {
        0x5A, 0x59, 0x5B, 0x51, 0x52, 0xB8, 0x34, 0x12, 0xCB
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

    cpu.ax = 0x0303;
    cpu.ds = data_sel; /* writable data is not an executable callback */
    cpu.esi = callback_code;
    cpu.es = data_sel;
    cpu.edi = callback_regs_addr;
    cpu.eflags &= ~FLAG_CF;
    dos_int31_dpmi(vm);
    if (!(cpu.eflags & FLAG_CF) || cpu.ax != 0x8015) {
        serial_puts("[DPMI-TEST] callback target validation mismatch\n");
        failures++;
    }

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
    const uint64_t memory_pages = 256;
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
        vm->dpmi.sel_psp, vm->dpmi.sel_env
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
            vm->dpmi.descriptor_state[index] != DPMI_DESC_CLIENT_SYSTEM)
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
    vm->total_mem_size = DOS_VM_ADDRESS_SPACE_SIZE;
    vm->system_mem_size = DOS_TOTAL_MEM;

    int entry_failures = dpmi_entry_selftest();
    int rm_call_failures = dpmi_rm_call_selftest();
    int dos_memory_failures = dpmi_dos_memory_selftest();
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
     * overwrite each other (DPMI 0202h-0205h). */
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
    if ((cpu.eflags & FLAG_CF) || cpu.ax != 0 ||
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
        if (!dpmi_raw_mode_switch(vm) || !cpu.protected_mode ||
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
        if (!dpmi_raw_mode_switch(vm) || cpu.protected_mode ||
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
        if (dpmi_raw_mode_switch(vm) || cpu.protected_mode)
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
