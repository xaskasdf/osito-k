/*
 * OsitoK - EMS 4.0 and VCPI 1.0 services (INT 67h)
 *
 * EMS, VCPI, and DPMI allocate from one physical page pool. This keeps
 * ownership explicit and prevents overlapping extended-memory allocations.
 */

#include "cpu8086.h"
#include "dos_dpmi.h"

extern void *mem_alloc_pages(uint64_t count);
extern void mem_free_pages(void *addr, uint64_t count);

#define EMS_PAGE_FRAME_SEG       0xE000u
#define EMS_VERSION              0x40u
#define EMS_MAX_HANDLES          64u
#define EMS_PAGES_PER_LOGICAL    (DOS_EMS_PAGE_SIZE / DPMI_EXT_PAGE_SIZE)

#define EMS_ERR_INVALID_HANDLE   0x83u
#define EMS_ERR_UNSUPPORTED      0x84u
#define EMS_ERR_NO_HANDLES       0x85u
#define EMS_ERR_TOO_MANY_PAGES   0x87u
#define EMS_ERR_OUT_OF_PAGES     0x88u
#define EMS_ERR_ZERO_PAGES       0x89u
#define EMS_ERR_LOGICAL_PAGE     0x8Au
#define EMS_ERR_PHYSICAL_PAGE    0x8Bu

#define VCPI_VERSION_MAJOR       1u
#define VCPI_VERSION_MINOR       0u
#define VCPI_PM_ENTRY_OFF        0x0180u
#define VCPI_PTE_FLAGS           0x00000007u
#define VCPI_ERR_UNSUPPORTED     0x8Fu

typedef struct {
    uint32_t *pages;
    uint16_t page_count;
    uint16_t table_pages;
    bool allocated;
} dos_ems_handle_t;

struct dos_vcpi_state {
    dos_ems_handle_t handles[EMS_MAX_HANDLES + 1u];
    uint8_t vcpi_page_bitmap[DPMI_EXT_BITMAP_SIZE];
    uint16_t frame_handles[DOS_EMS_FRAME_PAGES];
    uint16_t frame_logical[DOS_EMS_FRAME_PAGES];
    uint8_t next_handle;
    uint8_t pic_master;
    uint8_t pic_slave;
    uint32_t server_cr0;
    uint32_t server_cr3;
    uint32_t server_gdt_base;
    uint32_t server_idt_base;
    uint16_t server_gdt_limit;
    uint16_t server_idt_limit;
    uint16_t server_ldtr;
    uint16_t server_tr;
    bool server_dpmi_active;
    bool server_dpmi_is_32bit;
    bool server_context_valid;
};

static void vcpi_zero(void *dst, uint64_t len)
{
    uint8_t *p = (uint8_t *)dst;
    for (uint64_t i = 0; i < len; i++) p[i] = 0;
}

static bool vcpi_guest_range(const dos_vm_t *vm, uint32_t address,
                             uint32_t size)
{
    return vm && address <= vm->total_mem_size &&
           size <= vm->total_mem_size - address;
}

static struct dos_vcpi_state *vcpi_get_state(dos_vm_t *vm, bool create)
{
    if (!vm || vm->vcpi || !create)
        return vm ? vm->vcpi : NULL;

    uint64_t pages = (sizeof(struct dos_vcpi_state) + 4095u) / 4096u;
    struct dos_vcpi_state *state =
        (struct dos_vcpi_state *)mem_alloc_pages(pages);
    if (!state) return NULL;

    vcpi_zero(state, pages * 4096u);
    state->next_handle = 1;
    state->pic_master = 0x08;
    state->pic_slave = 0x70;
    vm->vcpi = state;
    return state;
}

static bool vcpi_page_index(const dos_vm_t *vm, uint32_t address,
                            uint32_t *index)
{
    if (!vm || address < DPMI_EXT_BASE ||
        (address & (DPMI_EXT_PAGE_SIZE - 1u)))
        return false;
    uint32_t page = (address - DPMI_EXT_BASE) / DPMI_EXT_PAGE_SIZE;
    if (page >= dpmi_ext_total_pages(vm)) return false;
    if (index) *index = page;
    return true;
}

static bool vcpi_page_owned(const dos_vm_t *vm,
                            const struct dos_vcpi_state *state,
                            uint32_t address)
{
    uint32_t page;
    if (!state || !vcpi_page_index(vm, address, &page)) return false;
    return (state->vcpi_page_bitmap[page >> 3] &
            (uint8_t)(1u << (page & 7u))) != 0;
}

static void vcpi_set_page_owned(const dos_vm_t *vm,
                                struct dos_vcpi_state *state,
                                uint32_t address, bool owned)
{
    uint32_t page;
    if (!state || !vcpi_page_index(vm, address, &page)) return;
    uint8_t mask = (uint8_t)(1u << (page & 7u));
    if (owned)
        state->vcpi_page_bitmap[page >> 3] |= mask;
    else
        state->vcpi_page_bitmap[page >> 3] &= (uint8_t)~mask;
}

static dos_ems_handle_t *ems_get_handle(struct dos_vcpi_state *state,
                                        uint16_t handle)
{
    if (!state || handle == 0 || handle > EMS_MAX_HANDLES ||
        !state->handles[handle].allocated)
        return NULL;
    return &state->handles[handle];
}

static uint16_t ems_find_handle(struct dos_vcpi_state *state)
{
    if (!state) return 0;
    uint16_t start = state->next_handle;
    if (start == 0 || start > EMS_MAX_HANDLES) start = 1;
    for (uint16_t i = 0; i < EMS_MAX_HANDLES; i++) {
        uint16_t handle = (uint16_t)(1u +
            ((uint16_t)(start - 1u + i) % EMS_MAX_HANDLES));
        if (!state->handles[handle].allocated) {
            state->next_handle = handle == EMS_MAX_HANDLES
                               ? 1 : (uint8_t)(handle + 1u);
            return handle;
        }
    }
    return 0;
}

static void ems_unmap_frame(dos_vm_t *vm,
                            struct dos_vcpi_state *state,
                            unsigned frame)
{
    if (!vm || frame >= DOS_EMS_FRAME_PAGES) return;
    vm->ems_frame_bases[frame] = 0;
    if (state) {
        state->frame_handles[frame] = 0;
        state->frame_logical[frame] = 0;
    }
    dos_native_ems_map_frame(vm, frame, 0);
}

static void ems_release_handle(dos_vm_t *vm,
                               struct dos_vcpi_state *state,
                               uint16_t handle)
{
    dos_ems_handle_t *entry = ems_get_handle(state, handle);
    if (!entry) return;
    for (unsigned frame = 0; frame < DOS_EMS_FRAME_PAGES; frame++) {
        if (state->frame_handles[frame] == handle)
            ems_unmap_frame(vm, state, frame);
    }
    for (uint16_t page = 0; page < entry->page_count; page++)
        (void)dpmi_ext_free_pages(vm, entry->pages[page],
                                  EMS_PAGES_PER_LOGICAL);
    if (entry->pages)
        mem_free_pages(entry->pages, entry->table_pages);
    vcpi_zero(entry, sizeof(*entry));
    if (handle < state->next_handle)
        state->next_handle = (uint8_t)handle;
}

static void vcpi_write_descriptor(dos_vm_t *vm, uint32_t address,
                                  uint32_t base, uint32_t limit,
                                  uint8_t access, uint8_t flags)
{
    dos_mem_write16(vm, address, (uint16_t)(limit & 0xFFFFu));
    dos_mem_write16(vm, address + 2u, (uint16_t)(base & 0xFFFFu));
    dos_mem_write8(vm, address + 4u, (uint8_t)(base >> 16));
    dos_mem_write8(vm, address + 5u, access);
    dos_mem_write8(vm, address + 6u,
                   (uint8_t)((flags & 0xF0u) | ((limit >> 16) & 0x0Fu)));
    dos_mem_write8(vm, address + 7u, (uint8_t)(base >> 24));
}

static void ems_allocate(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t logical_pages = cpu->bx;
    uint32_t total_pages = dpmi_ext_total_pages(vm) /
                           EMS_PAGES_PER_LOGICAL;
    if (!logical_pages) {
        cpu->ah = EMS_ERR_ZERO_PAGES;
        return;
    }
    if (logical_pages > total_pages) {
        cpu->ah = EMS_ERR_TOO_MANY_PAGES;
        return;
    }
    if (dpmi_ext_free_page_count(vm) <
        logical_pages * EMS_PAGES_PER_LOGICAL) {
        cpu->ah = EMS_ERR_OUT_OF_PAGES;
        return;
    }

    struct dos_vcpi_state *state = vcpi_get_state(vm, true);
    if (!state) {
        cpu->ah = EMS_ERR_OUT_OF_PAGES;
        return;
    }
    uint16_t handle = ems_find_handle(state);
    if (!handle) {
        cpu->ah = EMS_ERR_NO_HANDLES;
        return;
    }

    uint64_t table_pages =
        (logical_pages * sizeof(uint32_t) + 4095u) / 4096u;
    uint32_t *pages = (uint32_t *)mem_alloc_pages(table_pages);
    if (!pages) {
        cpu->ah = EMS_ERR_OUT_OF_PAGES;
        return;
    }
    vcpi_zero(pages, table_pages * 4096u);

    uint32_t allocated = 0;
    while (allocated < logical_pages) {
        uint32_t base = dpmi_ext_alloc_pages(vm,
                                             EMS_PAGES_PER_LOGICAL, true);
        if (!base) break;
        pages[allocated++] = base;
    }
    if (allocated != logical_pages) {
        for (uint32_t i = 0; i < allocated; i++)
            (void)dpmi_ext_free_pages(vm, pages[i],
                                      EMS_PAGES_PER_LOGICAL);
        mem_free_pages(pages, table_pages);
        cpu->ah = EMS_ERR_OUT_OF_PAGES;
        return;
    }

    dos_ems_handle_t *entry = &state->handles[handle];
    entry->pages = pages;
    entry->page_count = (uint16_t)logical_pages;
    entry->table_pages = (uint16_t)table_pages;
    entry->allocated = true;
    cpu->dx = handle;
    cpu->ah = 0;
}

static void ems_map(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    unsigned frame = cpu->al;
    if (frame >= DOS_EMS_FRAME_PAGES) {
        cpu->ah = EMS_ERR_PHYSICAL_PAGE;
        return;
    }
    struct dos_vcpi_state *state = vcpi_get_state(vm, false);
    if (cpu->bx == 0xFFFFu) {
        ems_unmap_frame(vm, state, frame);
        cpu->ah = 0;
        return;
    }
    dos_ems_handle_t *entry = ems_get_handle(state, cpu->dx);
    if (!entry) {
        cpu->ah = EMS_ERR_INVALID_HANDLE;
        return;
    }
    if (cpu->bx >= entry->page_count) {
        cpu->ah = EMS_ERR_LOGICAL_PAGE;
        return;
    }
    uint32_t backing = entry->pages[cpu->bx];
    vm->ems_frame_bases[frame] = backing;
    state->frame_handles[frame] = cpu->dx;
    state->frame_logical[frame] = cpu->bx;
    dos_native_ems_map_frame(vm, frame, backing);
    cpu->ah = 0;
}

static void ems_dispatch(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    switch (cpu->ah) {
    case 0x40:
        cpu->ah = 0;
        break;
    case 0x41:
        cpu->bx = EMS_PAGE_FRAME_SEG;
        cpu->ah = 0;
        break;
    case 0x42:
        cpu->bx = (uint16_t)(dpmi_ext_free_page_count(vm) /
                             EMS_PAGES_PER_LOGICAL);
        cpu->dx = (uint16_t)(dpmi_ext_total_pages(vm) /
                             EMS_PAGES_PER_LOGICAL);
        cpu->ah = 0;
        break;
    case 0x43:
        ems_allocate(vm);
        break;
    case 0x44:
        ems_map(vm);
        break;
    case 0x45: {
        struct dos_vcpi_state *state = vcpi_get_state(vm, false);
        if (!ems_get_handle(state, cpu->dx)) {
            cpu->ah = EMS_ERR_INVALID_HANDLE;
            break;
        }
        ems_release_handle(vm, state, cpu->dx);
        cpu->ah = 0;
        break;
    }
    case 0x46:
        cpu->al = EMS_VERSION;
        cpu->ah = 0;
        break;
    default:
        cpu->ah = EMS_ERR_UNSUPPORTED;
        break;
    }
}

static uint32_t vcpi_first_megabyte_page(dos_vm_t *vm, uint16_t page)
{
    uint32_t address = (uint32_t)page * DPMI_EXT_PAGE_SIZE;
    uint32_t frame_offset = address - DOS_EMS_PAGE_FRAME_BASE;
    if (frame_offset < DOS_EMS_FRAME_PAGES * DOS_EMS_PAGE_SIZE) {
        uint32_t backing = vm->ems_frame_bases[frame_offset /
                                                DOS_EMS_PAGE_SIZE];
        if (backing)
            return backing + (frame_offset & (DOS_EMS_PAGE_SIZE - 1u));
    }
    return address;
}

static void vcpi_detect(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    cpu->ah = 0;
    cpu->bh = VCPI_VERSION_MAJOR;
    cpu->bl = VCPI_VERSION_MINOR;
}

static void vcpi_get_pm_interface(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t page_table = dos_linear(cpu->es, cpu->di);
    uint32_t descriptors = dos_linear(cpu->ds, cpu->si);
    if (!vcpi_guest_range(vm, page_table, 4096u) ||
        !vcpi_guest_range(vm, descriptors, 24u)) {
        cpu->ah = EMS_ERR_OUT_OF_PAGES;
        return;
    }

    for (unsigned i = 0; i < 1024u; i++)
        dos_mem_write32(vm, page_table + i * 4u, 0);
    for (unsigned page = 0; page < 256u; page++) {
        uint32_t physical = vcpi_first_megabyte_page(vm, (uint16_t)page);
        dos_mem_write32(vm, page_table + page * 4u,
                        physical | VCPI_PTE_FLAGS);
    }

    vcpi_write_descriptor(vm, descriptors,
        (uint32_t)DPMI_ENTRY_SEG << 4, 0xFFFFu,
        DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
        DESC_CODE | DESC_READABLE, DESC_32BIT);
    vcpi_write_descriptor(vm, descriptors + 8u,
        0, 0xFFFFFu,
        DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT | DESC_WRITABLE,
        DESC_GRANULARITY | DESC_32BIT);
    for (unsigned i = 0; i < 8u; i++)
        dos_mem_write8(vm, descriptors + 16u + i, 0);

    uint32_t entry = ((uint32_t)DPMI_ENTRY_SEG << 4) + VCPI_PM_ENTRY_OFF;
    if (vcpi_guest_range(vm, entry, 3u)) {
        vm->mem[entry] = 0xCD;
        vm->mem[entry + 1u] = 0x67;
        vm->mem[entry + 2u] = 0xCB;
    }
    cpu->di = (uint16_t)(cpu->di + 256u * 4u);
    cpu->ebx = VCPI_PM_ENTRY_OFF;
    cpu->ah = 0;
}

static void vcpi_max_phys_addr(dos_vm_t *vm)
{
    uint32_t pages = dpmi_ext_total_pages(vm);
    vm->cpu->edx = pages
                 ? DPMI_EXT_BASE + (pages - 1u) * DPMI_EXT_PAGE_SIZE
                 : 0;
    vm->cpu->ah = 0;
}

static void vcpi_free_page_count(dos_vm_t *vm)
{
    vm->cpu->edx = dpmi_ext_free_page_count(vm);
    vm->cpu->ah = 0;
}

static void vcpi_alloc_page(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    struct dos_vcpi_state *state = vcpi_get_state(vm, true);
    if (!state) {
        cpu->ah = EMS_ERR_OUT_OF_PAGES;
        return;
    }
    uint32_t page = dpmi_ext_alloc_pages(vm, 1, true);
    if (!page) {
        cpu->ah = EMS_ERR_OUT_OF_PAGES;
        return;
    }
    vcpi_set_page_owned(vm, state, page, true);
    cpu->edx = page;
    cpu->ah = 0;
}

static void vcpi_free_page(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    struct dos_vcpi_state *state = vcpi_get_state(vm, false);
    if (!vcpi_page_owned(vm, state, cpu->edx) ||
        !dpmi_ext_free_pages(vm, cpu->edx, 1)) {
        cpu->ah = EMS_ERR_LOGICAL_PAGE;
        return;
    }
    vcpi_set_page_owned(vm, state, cpu->edx, false);
    cpu->ah = 0;
}

static void vcpi_get_phys_addr(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (cpu->cx >= 256u) {
        cpu->ah = EMS_ERR_LOGICAL_PAGE;
        return;
    }
    cpu->edx = vcpi_first_megabyte_page(vm, cpu->cx);
    cpu->ah = 0;
}

static void vcpi_read_cr0(dos_vm_t *vm)
{
    vm->cpu->ebx = vm->cpu->cr0;
    vm->cpu->ah = 0;
}

static void vcpi_read_debug_regs(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t buffer = dos_addr(vm, cpu->es, cpu->di);
    if (!vcpi_guest_range(vm, buffer, 32u)) {
        cpu->ah = EMS_ERR_LOGICAL_PAGE;
        return;
    }
    for (unsigned i = 0; i < 8u; i++)
        dos_mem_write32(vm, buffer + i * 4u,
                        (i == 4u || i == 5u) ? 0 : cpu->dr[i]);
    cpu->ah = 0;
}

static void vcpi_load_debug_regs(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t buffer = dos_addr(vm, cpu->es, cpu->di);
    if (!vcpi_guest_range(vm, buffer, 32u)) {
        cpu->ah = EMS_ERR_LOGICAL_PAGE;
        return;
    }
    for (unsigned i = 0; i < 8u; i++) {
        if (i != 4u && i != 5u)
            cpu->dr[i] = dos_mem_read32(vm, buffer + i * 4u);
    }
    cpu->ah = 0;
}

static void vcpi_get_pic_mappings(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    struct dos_vcpi_state *state = vcpi_get_state(vm, true);
    if (!state) {
        cpu->ah = EMS_ERR_OUT_OF_PAGES;
        return;
    }
    cpu->bx = state->pic_master;
    cpu->cx = state->pic_slave;
    cpu->ah = 0;
}

static void vcpi_set_pic_mappings(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (cpu->bx > 0xFFu || cpu->cx > 0xFFu) {
        cpu->ah = EMS_ERR_LOGICAL_PAGE;
        return;
    }
    struct dos_vcpi_state *state = vcpi_get_state(vm, true);
    if (!state) {
        cpu->ah = EMS_ERR_OUT_OF_PAGES;
        return;
    }
    state->pic_master = (uint8_t)cpu->bx;
    state->pic_slave = (uint8_t)cpu->cx;
    cpu->ah = 0;
}

static bool vcpi_gdt_descriptor(dos_vm_t *vm, uint32_t gdt_base,
                                uint16_t gdt_limit, uint16_t selector,
                                dpmi_descriptor_t *descriptor)
{
    if (!descriptor || (selector & 0x04u) || (selector >> 3) == 0)
        return false;
    uint32_t offset = (uint32_t)(selector & 0xFFF8u);
    if (offset + 7u > gdt_limit ||
        !vcpi_guest_range(vm, gdt_base + offset, 8u))
        return false;

    uint8_t *bytes = (uint8_t *)descriptor;
    for (unsigned i = 0; i < sizeof(*descriptor); i++)
        bytes[i] = dos_mem_read8(vm, gdt_base + offset + i);
    return true;
}

static bool vcpi_code_selector_valid(dos_vm_t *vm, uint32_t gdt_base,
                                     uint16_t gdt_limit, uint16_t selector,
                                     uint32_t eip)
{
    dpmi_descriptor_t descriptor;
    if (!vcpi_gdt_descriptor(vm, gdt_base, gdt_limit, selector,
                             &descriptor))
        return false;
    return (descriptor.access &
            (DESC_PRESENT | DESC_SEGMENT | DESC_CODE)) ==
           (DESC_PRESENT | DESC_SEGMENT | DESC_CODE) &&
           eip <= dpmi_desc_get_limit(&descriptor);
}

static bool vcpi_system_selector_valid(dos_vm_t *vm, uint32_t gdt_base,
                                       uint16_t gdt_limit,
                                       uint16_t selector, bool tss)
{
    if (!selector) return true;
    dpmi_descriptor_t descriptor;
    if (!vcpi_gdt_descriptor(vm, gdt_base, gdt_limit, selector,
                             &descriptor) ||
        !(descriptor.access & DESC_PRESENT) ||
        (descriptor.access & DESC_SEGMENT))
        return false;

    uint8_t type = descriptor.access & 0x0Fu;
    return tss ? (type == 0x09u || type == 0x0Bu) : type == 0x02u;
}

static void vcpi_switch_to_pm(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint32_t block = cpu->esi;
    if (!vcpi_guest_range(vm, block, 0x16u)) {
        cpu->ah = EMS_ERR_PHYSICAL_PAGE;
        return;
    }
    uint32_t new_cr3 = dos_mem_read32(vm, block);
    uint32_t gdtr_ptr = dos_mem_read32(vm, block + 4u);
    uint32_t idtr_ptr = dos_mem_read32(vm, block + 8u);
    uint16_t new_ldt = dos_mem_read16(vm, block + 0x0Cu);
    uint16_t new_tr = dos_mem_read16(vm, block + 0x0Eu);
    uint32_t new_eip = dos_mem_read32(vm, block + 0x10u);
    uint16_t new_cs = dos_mem_read16(vm, block + 0x14u);
    if (!vcpi_guest_range(vm, gdtr_ptr, 6u) ||
        !vcpi_guest_range(vm, idtr_ptr, 6u) ||
        (new_cr3 && ((new_cr3 & (DPMI_EXT_PAGE_SIZE - 1u)) ||
                     !vcpi_guest_range(vm, new_cr3,
                                       DPMI_EXT_PAGE_SIZE)))) {
        cpu->ah = EMS_ERR_PHYSICAL_PAGE;
        return;
    }
    uint16_t gdt_limit = dos_mem_read16(vm, gdtr_ptr);
    uint32_t gdt_base = dos_mem_read32(vm, gdtr_ptr + 2u);
    uint16_t idt_limit = dos_mem_read16(vm, idtr_ptr);
    uint32_t idt_base = dos_mem_read32(vm, idtr_ptr + 2u);
    if (!vcpi_code_selector_valid(vm, gdt_base, gdt_limit, new_cs,
                                  new_eip) ||
        !vcpi_system_selector_valid(vm, gdt_base, gdt_limit, new_ldt,
                                    false) ||
        !vcpi_system_selector_valid(vm, gdt_base, gdt_limit, new_tr,
                                    true) ||
        !vcpi_guest_range(vm, idt_base, (uint32_t)idt_limit + 1u)) {
        cpu->ah = EMS_ERR_LOGICAL_PAGE;
        return;
    }

    struct dos_vcpi_state *state = vcpi_get_state(vm, true);
    if (!state) {
        cpu->ah = EMS_ERR_OUT_OF_PAGES;
        return;
    }
    state->server_cr0 = cpu->cr0;
    state->server_cr3 = cpu->cr3;
    state->server_gdt_limit = cpu->gdtr.limit;
    state->server_gdt_base = cpu->gdtr.base;
    state->server_idt_limit = cpu->idtr.limit;
    state->server_idt_base = cpu->idtr.base;
    state->server_ldtr = cpu->ldtr;
    state->server_tr = cpu->tr;
    state->server_dpmi_active = vm->dpmi.active;
    state->server_dpmi_is_32bit = vm->dpmi.is_32bit;
    state->server_context_valid = true;

    cpu->cr3 = new_cr3;
    cpu->gdtr.limit = gdt_limit;
    cpu->gdtr.base = gdt_base;
    cpu->idtr.limit = idt_limit;
    cpu->idtr.base = idt_base;
    cpu->protected_mode = true;
    cpu->pm_cs_loaded = true;
    cpu->cr0 = (cpu->cr0 | 1u) & ~0x80000000u;
    if (new_cr3) cpu->cr0 |= 0x80000000u;
    cpu->ldtr = new_ldt;
    cpu->tr = new_tr;
    cpu->cs = new_cs;
    cpu->eip = new_eip;
    cpu->ds = 0;
    cpu->es = 0;
    cpu->fs = 0;
    cpu->gs = 0;
    cpu->eflags = (cpu->eflags & ~(FLAG_VM | FLAG_NT)) |
                  FLAG_IOPL_MASK | FLAGS_FIXED;
    vm->dpmi.active = true;
    vm->dpmi.is_32bit = true;
    cpu8086_sync_cs(cpu);
    cpu->ah = 0;
}

static void vcpi_switch_to_v86(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    struct dos_vcpi_state *state = vcpi_get_state(vm, false);
    if (!state || !state->server_context_valid) {
        cpu->ah = VCPI_ERR_UNSUPPORTED;
        return;
    }

    bool stack32 = cpu_stack_addr32(cpu);
    uint32_t stack_offset = stack32 ? cpu->esp : cpu->sp;
    if (!vm->native_dispatch_depth) {
        uint32_t frame_size = cpu->op_size_32 ? 12u : 6u;
        stack_offset = stack32 ? stack_offset + frame_size
                               : (uint16_t)(stack_offset + frame_size);
    }
    uint32_t block = dos_addr(vm, cpu->ss, stack_offset);
    if (block >= 0x100000u || !vcpi_guest_range(vm, block, 0x2Cu)) {
        cpu->ah = EMS_ERR_PHYSICAL_PAGE;
        return;
    }

    uint32_t new_eip = dos_mem_read32(vm, block + 0x08u);
    if (new_eip > 0xFFFFu) {
        cpu->ah = EMS_ERR_LOGICAL_PAGE;
        return;
    }

    const uint32_t v86_flags = FLAG_VM | FLAG_IOPL_MASK |
                               FLAG_IF | FLAGS_FIXED;
    dos_mem_write32(vm, block + 0x10u, v86_flags);

    cpu->eip = new_eip;
    cpu->cs = (uint16_t)dos_mem_read32(vm, block + 0x0Cu);
    cpu->esp = dos_mem_read32(vm, block + 0x14u);
    cpu->ss = (uint16_t)dos_mem_read32(vm, block + 0x18u);
    cpu->es = (uint16_t)dos_mem_read32(vm, block + 0x1Cu);
    cpu->ds = (uint16_t)dos_mem_read32(vm, block + 0x20u);
    cpu->fs = (uint16_t)dos_mem_read32(vm, block + 0x24u);
    cpu->gs = (uint16_t)dos_mem_read32(vm, block + 0x28u);
    cpu->eflags = v86_flags;
    cpu->cr0 = state->server_cr0 | 1u;
    cpu->cr3 = state->server_cr3;
    cpu->gdtr.limit = state->server_gdt_limit;
    cpu->gdtr.base = state->server_gdt_base;
    cpu->idtr.limit = state->server_idt_limit;
    cpu->idtr.base = state->server_idt_base;
    cpu->ldtr = state->server_ldtr;
    cpu->tr = state->server_tr;
    cpu->protected_mode = false;
    cpu->pm_cs_loaded = false;
    cpu->op_size_32 = false;
    cpu->addr_size_32 = false;
    cpu->halted = false;
    vm->dpmi.active = state->server_dpmi_active;
    vm->dpmi.is_32bit = state->server_dpmi_is_32bit;
}

static void vcpi_dispatch(dos_vm_t *vm)
{
    if (vm->cpu->protected_mode) {
        switch (vm->cpu->al) {
        case 0x03: vcpi_free_page_count(vm); break;
        case 0x04: vcpi_alloc_page(vm);      break;
        case 0x05: vcpi_free_page(vm);       break;
        case 0x0C: vcpi_switch_to_v86(vm);   break;
        default: vm->cpu->ah = VCPI_ERR_UNSUPPORTED; break;
        }
        return;
    }

    switch (vm->cpu->al) {
    case 0x00: vcpi_detect(vm);           break;
    case 0x01: vcpi_get_pm_interface(vm); break;
    case 0x02: vcpi_max_phys_addr(vm);    break;
    case 0x03: vcpi_free_page_count(vm);  break;
    case 0x04: vcpi_alloc_page(vm);       break;
    case 0x05: vcpi_free_page(vm);        break;
    case 0x06: vcpi_get_phys_addr(vm);    break;
    case 0x07: vcpi_read_cr0(vm);         break;
    case 0x08: vcpi_read_debug_regs(vm);  break;
    case 0x09: vcpi_load_debug_regs(vm);  break;
    case 0x0A: vcpi_get_pic_mappings(vm); break;
    case 0x0B: vcpi_set_pic_mappings(vm); break;
    case 0x0C: vcpi_switch_to_pm(vm);     break;
    default: vm->cpu->ah = VCPI_ERR_UNSUPPORTED; break;
    }
}

void dos_int67_dispatch(dos_vm_t *vm)
{
    if (!vm || !vm->cpu) return;
    if (vm->cpu->ah == 0xDEu)
        vcpi_dispatch(vm);
    else
        ems_dispatch(vm);
}

void dos_vcpi_cleanup(dos_vm_t *vm)
{
    if (!vm) return;
    struct dos_vcpi_state *state = vm->vcpi;
    for (unsigned frame = 0; frame < DOS_EMS_FRAME_PAGES; frame++)
        ems_unmap_frame(vm, state, frame);
    if (!state) return;

    for (uint16_t handle = 1; handle <= EMS_MAX_HANDLES; handle++)
        ems_release_handle(vm, state, handle);
    uint32_t total = dpmi_ext_total_pages(vm);
    for (uint32_t page = 0; page < total; page++) {
        uint8_t mask = (uint8_t)(1u << (page & 7u));
        if (state->vcpi_page_bitmap[page >> 3] & mask) {
            uint32_t address = DPMI_EXT_BASE +
                               page * DPMI_EXT_PAGE_SIZE;
            (void)dpmi_ext_free_pages(vm, address, 1);
        }
    }
    uint64_t pages = (sizeof(*state) + 4095u) / 4096u;
    mem_free_pages(state, pages);
    vm->vcpi = NULL;
}

int dos_vcpi_selftest(void)
{
    uint64_t memory_pages = (DOS_TOTAL_MEM + 4095u) / 4096u;
    uint8_t *memory = (uint8_t *)mem_alloc_pages(memory_pages);
    if (!memory) return 1;
    vcpi_zero(memory, memory_pages * 4096u);

    dos_vm_t vm = {0};
    cpu8086_state_t cpu;
    vm.mem = memory;
    vm.total_mem_size = DOS_TOTAL_MEM;
    vm.cpu = &cpu;
    dpmi_init(&vm);
    cpu8086_init(&cpu, &vm);

    int failures = 0;
    uint32_t initial_free = dpmi_ext_free_page_count(&vm);
    cpu.ah = 0x43;
    cpu.bx = 2;
    dos_int67_dispatch(&vm);
    uint16_t handle = cpu.dx;
    struct dos_vcpi_state *state = vm.vcpi;
    dos_ems_handle_t *entry = ems_get_handle(state, handle);
    if (cpu.ah != 0 || !entry ||
        dpmi_ext_free_page_count(&vm) != initial_free - 8u)
        failures++;

    cpu.ah = 0x44;
    cpu.al = 0;
    cpu.bx = 0;
    cpu.dx = handle;
    dos_int67_dispatch(&vm);
    uint32_t backing = vm.ems_frame_bases[0];
    if (cpu.ah != 0 || !backing) {
        failures++;
    } else {
        dos_mem_write8(&vm, DOS_EMS_PAGE_FRAME_BASE + 123u, 0xA5u);
        memory[backing + 124u] = 0x5Au;
        if (memory[backing + 123u] != 0xA5u ||
            dos_mem_read8(&vm, DOS_EMS_PAGE_FRAME_BASE + 124u) != 0x5Au)
            failures++;
    }

    cpu.ax = 0xDE06;
    cpu.cx = 0x00E0;
    dos_int67_dispatch(&vm);
    if (cpu.ah != 0 || cpu.edx != backing) failures++;

    cpu.ah = 0x44;
    cpu.al = 0;
    cpu.bx = 0xFFFFu;
    dos_int67_dispatch(&vm);
    if (cpu.ah != 0 || vm.ems_frame_bases[0] != 0) failures++;

    cpu.ah = 0x45;
    cpu.dx = handle;
    dos_int67_dispatch(&vm);
    if (cpu.ah != 0 || dpmi_ext_free_page_count(&vm) != initial_free)
        failures++;

    cpu.ax = 0xDE04;
    dos_int67_dispatch(&vm);
    uint32_t vcpi_page = cpu.edx;
    uint32_t dpmi_page = dpmi_ext_alloc_pages(&vm, 1, false);
    if (cpu.ah != 0 || !vcpi_page || !dpmi_page ||
        vcpi_page == dpmi_page)
        failures++;

    cpu.ax = 0xDE05;
    cpu.edx = dpmi_page;
    dos_int67_dispatch(&vm);
    if (cpu.ah == 0) failures++;
    (void)dpmi_ext_free_pages(&vm, dpmi_page, 1);

    cpu.ax = 0xDE05;
    cpu.edx = vcpi_page;
    dos_int67_dispatch(&vm);
    if (cpu.ah != 0) failures++;
    cpu.ax = 0xDE05;
    cpu.edx = vcpi_page;
    dos_int67_dispatch(&vm);
    if (cpu.ah == 0) failures++;

    uint32_t debug_buffer = 0x7000u;
    for (unsigned i = 0; i < 8u; i++)
        dos_mem_write32(&vm, debug_buffer + i * 4u,
                        0x11000000u + i);
    cpu.es = 0x0700;
    cpu.di = 0;
    cpu.ax = 0xDE09;
    dos_int67_dispatch(&vm);
    for (unsigned i = 0; i < 8u; i++)
        dos_mem_write32(&vm, debug_buffer + i * 4u, 0);
    cpu.ax = 0xDE08;
    dos_int67_dispatch(&vm);
    if (cpu.ah != 0 || dos_mem_read32(&vm, debug_buffer) != 0x11000000u ||
        dos_mem_read32(&vm, debug_buffer + 16u) != 0 ||
        dos_mem_read32(&vm, debug_buffer + 24u) != 0x11000006u)
        failures++;

    cpu.es = 0x0800;
    cpu.di = 0;
    cpu.ds = 0x0900;
    cpu.si = 0;
    cpu.ax = 0xDE01;
    dos_int67_dispatch(&vm);
    if (cpu.ah != 0 || cpu.ebx != VCPI_PM_ENTRY_OFF || cpu.di != 0x0400u ||
        dos_mem_read32(&vm, 0x8000u) != VCPI_PTE_FLAGS ||
        (dos_mem_read8(&vm, 0x9005u) &
         (DESC_PRESENT | DESC_SEGMENT | DESC_CODE)) !=
        (DESC_PRESENT | DESC_SEGMENT | DESC_CODE))
        failures++;

    const uint32_t mode_block = 0xA000u;
    const uint32_t gdtr_ptr = 0xA100u;
    const uint32_t idtr_ptr = 0xA108u;
    const uint32_t gdt_base = 0xB000u;
    const uint32_t idt_base = 0xC000u;
    const uint32_t ldt_base = 0xD000u;
    const uint32_t v86_frame = 0xE000u;

    vcpi_write_descriptor(&vm, gdt_base + 0x08u,
                          0, 0xFFFFFu,
                          DESC_PRESENT | DESC_SEGMENT | DESC_CODE |
                          DESC_READABLE,
                          DESC_GRANULARITY | DESC_32BIT);
    vcpi_write_descriptor(&vm, gdt_base + 0x10u,
                          ldt_base, 0x00FFu,
                          DESC_PRESENT | 0x02u, 0);
    vcpi_write_descriptor(&vm, gdt_base + 0x18u,
                          0xD100u, 0x0067u,
                          DESC_PRESENT | 0x09u, 0);
    vcpi_write_descriptor(&vm, gdt_base + 0x20u,
                          0, 0xFFFFFu,
                          DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                          DESC_WRITABLE,
                          DESC_GRANULARITY | DESC_32BIT);
    vcpi_write_descriptor(&vm, ldt_base + 0x08u,
                          0x12340u, 0xFFFFu,
                          DESC_PRESENT | DESC_DPL3 | DESC_SEGMENT |
                          DESC_WRITABLE, 0);

    dos_mem_write16(&vm, gdtr_ptr, 0x0027u);
    dos_mem_write32(&vm, gdtr_ptr + 2u, gdt_base);
    dos_mem_write16(&vm, idtr_ptr, 0x00FFu);
    dos_mem_write32(&vm, idtr_ptr + 2u, idt_base);
    dos_mem_write32(&vm, mode_block, 0);
    dos_mem_write32(&vm, mode_block + 4u, gdtr_ptr);
    dos_mem_write32(&vm, mode_block + 8u, idtr_ptr);
    dos_mem_write16(&vm, mode_block + 0x0Cu, 0x0010u);
    dos_mem_write16(&vm, mode_block + 0x0Eu, 0x0018u);
    dos_mem_write32(&vm, mode_block + 0x10u, 0x1234u);
    dos_mem_write16(&vm, mode_block + 0x14u, 0x0008u);

    cpu.cr0 = 0;
    cpu.cr3 = 0;
    cpu.gdtr.limit = 0;
    cpu.gdtr.base = 0;
    cpu.idtr.limit = 0;
    cpu.idtr.base = 0;
    cpu.ldtr = 0;
    cpu.tr = 0;
    cpu.protected_mode = false;
    cpu.pm_cs_loaded = false;
    cpu.eflags = FLAG_VM | FLAG_IF | FLAGS_FIXED;
    cpu.esi = mode_block;
    cpu.ax = 0xDE0C;
    dos_int67_dispatch(&vm);
    if (cpu.ah != 0 || !cpu.protected_mode || !cpu.pm_cs_loaded ||
        !(cpu.cr0 & 1u) || (cpu.cr0 & 0x80000000u) ||
        cpu.cs != 0x0008u || cpu.eip != 0x1234u ||
        cpu.ldtr != 0x0010u || cpu.tr != 0x0018u ||
        cpu.gdtr.base != gdt_base || cpu.gdtr.limit != 0x0027u ||
        cpu.idtr.base != idt_base || cpu.idtr.limit != 0x00FFu ||
        (cpu.eflags & FLAG_VM) ||
        (cpu.eflags & FLAG_IOPL_MASK) != FLAG_IOPL_MASK ||
        dpmi_translate(&vm, 0x000Fu, 0x20u) != 0x12360u)
        failures++;

    dos_mem_write32(&vm, v86_frame + 0x08u, 0x3456u);
    dos_mem_write32(&vm, v86_frame + 0x0Cu, 0x1111u);
    dos_mem_write32(&vm, v86_frame + 0x10u, 0);
    dos_mem_write32(&vm, v86_frame + 0x14u, 0x5678u);
    dos_mem_write32(&vm, v86_frame + 0x18u, 0x2222u);
    dos_mem_write32(&vm, v86_frame + 0x1Cu, 0x3333u);
    dos_mem_write32(&vm, v86_frame + 0x20u, 0x4444u);
    dos_mem_write32(&vm, v86_frame + 0x24u, 0x5555u);
    dos_mem_write32(&vm, v86_frame + 0x28u, 0x6666u);
    cpu.ss = 0x0020u;
    cpu.esp = v86_frame;
    vm.native_dispatch_depth = 1;
    cpu.ax = 0xDE0C;
    dos_int67_dispatch(&vm);
    vm.native_dispatch_depth = 0;
    if (cpu.protected_mode || cpu.pm_cs_loaded || cpu.cr0 != 1u ||
        cpu.cr3 != 0 || cpu.cs != 0x1111u || cpu.eip != 0x3456u ||
        cpu.ss != 0x2222u || cpu.esp != 0x5678u ||
        cpu.es != 0x3333u || cpu.ds != 0x4444u ||
        cpu.fs != 0x5555u || cpu.gs != 0x6666u ||
        cpu.gdtr.base != 0 || cpu.gdtr.limit != 0 ||
        cpu.idtr.base != 0 || cpu.idtr.limit != 0 ||
        cpu.ldtr != 0 || cpu.tr != 0 ||
        cpu.eflags != (FLAG_VM | FLAG_IOPL_MASK |
                       FLAG_IF | FLAGS_FIXED) ||
        dos_mem_read32(&vm, v86_frame + 0x10u) != cpu.eflags)
        failures++;

    cpu.ax = 0xDE0D;
    dos_int67_dispatch(&vm);
    if (cpu.ah != VCPI_ERR_UNSUPPORTED) failures++;

    dos_vcpi_cleanup(&vm);
    if (vm.vcpi || dpmi_ext_free_page_count(&vm) != initial_free)
        failures++;
    mem_free_pages(memory, memory_pages);
    return failures;
}
