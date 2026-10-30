/* XMS control entry, VM-owned handles and physical memory reservations. */
#include "cpu8086.h"
#include "dos_hostmem.h"
#include "dos_io.h"
#include "dos_xms.h"

extern void *memmove(void *dst, const void *src, uint64_t length);

typedef struct {
    uint32_t base, kilobytes;
    uint16_t pins;
    uint8_t locks;
    bool used;
} xms_block_t;

struct dos_xms_state {
    xms_block_t *blocks;
    uint32_t table_pages, capacity, used, local_a20;
    uint16_t hma_pages;
    bool global_a20, hma_used, bios_hooked;
    uint16_t umb_owner[DOS_XMS_UMB_SIZE / 16u];
};

#define XMS_STATE_PAGES ((sizeof(struct dos_xms_state) + 4095u) / 4096u)
#define XMS_COPY_QUANTUM 8192u
#define XMS_UMB_DOS_OWNER 0x8000u

uint16_t dos_xms_umb_claim_dos(dos_vm_t *vm)
{
    if (!vm || !vm->xms ||
        vm->total_mem_size < DOS_XMS_UMB_BASE + DOS_XMS_UMB_SIZE) return 0;
    uint16_t *owners = vm->xms->umb_owner;
    const unsigned count = DOS_XMS_UMB_SIZE / 16u;
    unsigned last = 0;
    for (unsigned i = 0; i < count;) {
        if (owners[i]) {
            if (owners[i] & XMS_UMB_DOS_OWNER) last = i + 1u;
            i++;
            continue;
        }
        unsigned first = i;
        while (i < count && !owners[i]) i++;
        /* The end of each reservation remains trusted after MCB splitting. */
        for (unsigned p = first; p < i; p++) owners[p] = XMS_UMB_DOS_OWNER | i;
        last = i;
    }
    return last ? (uint16_t)((DOS_XMS_UMB_BASE >> 4) + last) : 0;
}

void dos_xms_umb_release_dos(dos_vm_t *vm)
{
    if (!vm || !vm->xms) return;
    for (unsigned i = 0; i < DOS_XMS_UMB_SIZE / 16u; i++)
        if (vm->xms->umb_owner[i] & XMS_UMB_DOS_OWNER)
            vm->xms->umb_owner[i] = 0;
}

uint16_t dos_xms_umb_dos_next(const dos_vm_t *vm, uint16_t segment)
{
    if (!vm || !vm->xms) return 0;
    unsigned first = segment > (DOS_XMS_UMB_BASE >> 4)
        ? segment - (DOS_XMS_UMB_BASE >> 4) : 0;
    for (unsigned i = first; i < DOS_XMS_UMB_SIZE / 16u; i++)
        if (vm->xms->umb_owner[i] & XMS_UMB_DOS_OWNER)
            return (uint16_t)((DOS_XMS_UMB_BASE >> 4) + i);
    return 0;
}

uint16_t dos_xms_umb_dos_end(const dos_vm_t *vm, uint16_t segment)
{
    unsigned index = (unsigned)segment - (DOS_XMS_UMB_BASE >> 4);
    if (!vm || !vm->xms || index >= DOS_XMS_UMB_SIZE / 16u) return 0;
    unsigned owner = vm->xms->umb_owner[index];
    if (!(owner & XMS_UMB_DOS_OWNER)) return 0;
    return (uint16_t)((DOS_XMS_UMB_BASE >> 4) + (owner & ~XMS_UMB_DOS_OWNER));
}

static void xms_result(cpu8086_state_t *cpu, uint8_t error)
{
    cpu->ax = error ? 0 : 1;
    cpu->bl = error;
}

static xms_block_t *xms_block(dos_vm_t *vm, uint16_t handle)
{
    struct dos_xms_state *xms = vm->xms;
    if (!handle || handle >= xms->capacity || !xms->blocks[handle].used) return NULL;
    return &xms->blocks[handle];
}

static uint32_t xms_pages(uint32_t kilobytes)
{
    return (kilobytes + 3u) / 4u;
}

static uint16_t xms_new_handle(dos_vm_t *vm)
{
    struct dos_xms_state *xms = vm->xms;
    for (uint32_t i = 1; i < xms->capacity; i++)
        if (!xms->blocks[i].used) return (uint16_t)i;
    if (xms->capacity == 65536u) return 0;
    uint32_t pages = xms->table_pages ? xms->table_pages * 2u : 1u;
    uint32_t max_pages = (65536u * sizeof(xms_block_t) + 4095u) / 4096u;
    if (pages > max_pages) pages = max_pages;
    xms_block_t *blocks = dos_host_alloc_pages(pages);
    if (!blocks) return 0;
    memset(blocks, 0, pages * 4096u);
    if (xms->blocks) memcpy(blocks, xms->blocks, xms->capacity * sizeof(*blocks));
    dos_host_free_pages(xms->blocks, xms->table_pages);
    uint16_t result = xms->capacity ? (uint16_t)xms->capacity : 1;
    xms->blocks = blocks;
    xms->table_pages = pages;
    xms->capacity = pages * 4096u / sizeof(*blocks);
    if (xms->capacity > 65536u) xms->capacity = 65536u;
    return result;
}

static bool xms_vdisk_signature(dos_vm_t *vm, uint32_t address)
{
    const char signature[] = "VDISK";
    if (!dos_phys_guest_span(vm, address, 5, NULL)) return false;
    for (unsigned i = 0; i < 5; i++)
        if (dos_phys_read8(vm, address + i) != (uint8_t)signature[i]) return false;
    return true;
}

static bool xms_vdisk(dos_vm_t *vm)
{
    uint32_t vector = dos_mem_read32(vm, 0x19u * 4u);
    uint32_t address = dos_linear(vector >> 16, vector & 0xFFFFu);
    return xms_vdisk_signature(vm, address + 0x13u) ||
           xms_vdisk_signature(vm, DOS_XMS_HMA_BASE + 0x13u);
}

bool dos_xms_init(dos_vm_t *vm)
{
    if (!vm || !vm->mem || !vm->io) return false;
    if (vm->xms) return true;
    if (DOS_ROM_BASE + DOS_XMS_ENTRY_OFF + 8u > vm->total_mem_size || xms_vdisk(vm))
        return false;
    struct dos_xms_state *xms = dos_host_alloc_pages(XMS_STATE_PAGES);
    if (!xms) return false;
    memset(xms, 0, XMS_STATE_PAGES * 4096u);
    uint32_t pages = dpmi_ext_total_pages(vm);
    if (pages > DOS_XMS_HMA_SIZE / 4096u) pages = DOS_XMS_HMA_SIZE / 4096u;
    if (pages && !dpmi_ext_reserve_pages(vm, DOS_XMS_HMA_BASE, pages)) {
        dos_host_free_pages(xms, XMS_STATE_PAGES);
        return false;
    }
    xms->hma_pages = (uint16_t)pages;
    vm->xms = xms;
    if (!xms_new_handle(vm)) {
        dos_xms_cleanup(vm);
        return false;
    }
    /* Five patchable bytes implement the specified XMS far-hook chain. */
    const uint8_t entry[] = { 0xEB, 3, 0x90, 0x90, 0x90, 0xCD, DOS_RM_SERVICE_INT, 0xCB };
    memcpy(vm->mem + DOS_ROM_BASE + DOS_XMS_ENTRY_OFF, entry, sizeof(entry));
    return true;
}

void dos_xms_cleanup(dos_vm_t *vm)
{
    if (!vm || !vm->xms) return;
    struct dos_xms_state *xms = vm->xms;
    for (uint32_t i = 1; i < xms->capacity; i++)
        if (xms->blocks[i].used && xms->blocks[i].kilobytes)
            dpmi_ext_free_pages(vm, xms->blocks[i].base, xms_pages(xms->blocks[i].kilobytes));
    if (xms->hma_pages) dpmi_ext_free_pages(vm, DOS_XMS_HMA_BASE, xms->hma_pages);
    dos_host_free_pages(xms->blocks, xms->table_pages);
    dos_host_free_pages(xms, XMS_STATE_PAGES);
    vm->xms = NULL;
}

bool dos_xms_multiplex(dos_vm_t *vm)
{
    if (!vm->xms) return false;
    cpu8086_state_t *cpu = vm->cpu;
    if (cpu->ax == 0x4300) cpu->al = 0x80;
    else if (cpu->ax == 0x4310) {
        cpu->es = DOS_ROM_BASE >> 4;
        cpu8086_sync_segment(cpu, 0);
        cpu->bx = DOS_XMS_ENTRY_OFF;
    } else return false;
    return true;
}

bool dos_xms_entry_source(const dos_vm_t *vm)
{
    const cpu8086_state_t *cpu = vm ? vm->cpu : NULL;
    return vm && vm->xms && cpu && !cpu->protected_mode &&
           (uint64_t)dpmi_desc_get_base(&cpu->cs_cache.descriptor) + cpu->eip ==
               DOS_ROM_BASE + DOS_XMS_ENTRY_OFF + 7u;
}

bool dos_xms_bios_hooked(const dos_vm_t *vm)
{
    return vm && vm->xms && vm->xms->bios_hooked;
}

static uint8_t xms_a20(dos_vm_t *vm, uint8_t function)
{
    struct dos_xms_state *xms = vm->xms;
    if (function == 3) xms->global_a20 = true;
    if (function == 4) xms->global_a20 = false;
    if (function == 5) {
        if (xms->local_a20 == UINT32_MAX) return 0x82;
        xms->local_a20++;
    }
    bool unmatched = function == 6 && !xms->local_a20;
    if (function == 6 && xms->local_a20) xms->local_a20--;
    bool enabled = xms->global_a20 || xms->local_a20;
    if (!dos_io_set_a20(vm, enabled)) return 0x82;
    if (unmatched) return 0x82;
    if (!enabled && dos_a20_enabled(vm)) return 0x94;
    if (enabled && !dos_a20_enabled(vm)) return 0x82;
    return (function == 4 || function == 6) && enabled ? 0x94 : 0;
}

static uint8_t xms_allocate(dos_vm_t *vm, uint32_t kilobytes, uint16_t *handle)
{
    *handle = 0;
    if (kilobytes > dpmi_ext_total_pages(vm) * 4u) return 0xA0;
    uint16_t id = xms_new_handle(vm);
    if (!id) return 0xA1;
    uint32_t base = kilobytes ? dpmi_ext_alloc_pages(vm, xms_pages(kilobytes), true) : 0;
    if (kilobytes && !base) return 0xA0;
    vm->xms->blocks[id] = (xms_block_t){ .base = base, .kilobytes = kilobytes, .used = true };
    vm->xms->used++;
    *handle = id;
    return 0;
}

static uint8_t xms_resize(dos_vm_t *vm, uint16_t handle, uint32_t kilobytes)
{
    xms_block_t *block = xms_block(vm, handle);
    if (!block) return 0xA2;
    if (block->locks || block->pins) return 0xAB;
    if (kilobytes > dpmi_ext_total_pages(vm) * 4u) return 0xA0;
    uint32_t old_pages = xms_pages(block->kilobytes), pages = xms_pages(kilobytes);
    if (old_pages && dpmi_ext_resize_pages(vm, block->base, old_pages, pages)) {
        if (pages && kilobytes != block->kilobytes) {
            uint32_t first = kilobytes < block->kilobytes ? kilobytes : block->kilobytes;
            memset(vm->mem + block->base + first * 1024u, 0, pages * 4096u - first * 1024u);
        }
        if (!pages) block->base = 0;
        block->kilobytes = kilobytes;
        return 0;
    }
    uint32_t base = pages ? dpmi_ext_alloc_pages(vm, pages, true) : 0;
    if (pages && !base) return 0xA0;
    uint32_t copy = kilobytes < block->kilobytes ? kilobytes : block->kilobytes;
    if (copy) memcpy(vm->mem + base, vm->mem + block->base, copy * 1024u);
    if (old_pages) dpmi_ext_free_pages(vm, block->base, old_pages);
    block->base = base;
    block->kilobytes = kilobytes;
    return 0;
}

static uint8_t xms_move_endpoint(dos_vm_t *vm, uint16_t handle, uint32_t offset,
                                 uint32_t length, bool destination, uint32_t *address,
                                 uint8_t **direct)
{
    uint32_t limit;
    if (handle) {
        xms_block_t *block = xms_block(vm, handle);
        if (!block) return destination ? 0xA5 : 0xA3;
        limit = block->kilobytes * 1024u;
        if (offset > limit) return destination ? 0xA6 : 0xA4;
        if (length > limit - offset) return 0xA7;
        *address = block->base + offset;
    } else {
        *address = dos_linear(offset >> 16, offset & 0xFFFFu);
        limit = DOS_XMS_HMA_BASE + DOS_XMS_HMA_SIZE - 16u;
        if (*address > limit || length > limit - *address) return 0xA7;
    }
    return dos_phys_guest_span(vm, *address, length, direct) ? 0 : 0xA7;
}

static bool xms_interrupt_window(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    if (!cpu->running || !(vm->software_int_return_flags & FLAG_IF)) return cpu->running;
    cpu8086_state_t saved = *cpu;
    cpu->flags |= FLAG_IF;
    if (cpu8086_service_interrupts(vm) &&
        !cpu8086_run_until(vm, saved.protected_mode, saved.cs, saved.eip)) return false;
    if (!cpu->running) return false;
    uint64_t count = cpu->insn_count;
    *cpu = saved;
    cpu->insn_count = count;
    return true;
}

static uint8_t xms_move(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint64_t address64 = (uint64_t)dpmi_desc_get_base(&cpu->ds_cache.descriptor) + cpu->si;
    if (!cpu->ds_cache.valid || address64 > UINT32_MAX || (uint32_t)cpu->si + 15u >
        dpmi_desc_get_limit(&cpu->ds_cache.descriptor) ||
        !dos_mem_guest_span(vm, (uint32_t)address64, 16, NULL)) return 0xA7;
    uint32_t address = (uint32_t)address64;
    uint32_t length = dos_mem_read32(vm, address);
    uint16_t source_id = dos_mem_read16(vm, address + 4u);
    uint32_t source_offset = dos_mem_read32(vm, address + 6u);
    uint16_t dest_id = dos_mem_read16(vm, address + 10u);
    uint32_t dest_offset = dos_mem_read32(vm, address + 12u);
    if (length & 1u) return 0xA7;
    uint32_t source, destination;
    uint8_t *src, *dst;
    uint8_t error = xms_move_endpoint(vm, source_id, source_offset, length, false, &source, &src);
    if (error) return error;
    error = xms_move_endpoint(vm, dest_id, dest_offset, length, true, &destination, &dst);
    if (error || !length) return error;
    if ((source_id && xms_block(vm, source_id)->pins == 0xFFFFu) ||
        (dest_id && xms_block(vm, dest_id)->pins == 0xFFFFu)) return 0xAB;
    if (source_id) xms_block(vm, source_id)->pins++;
    if (dest_id && dest_id != source_id) xms_block(vm, dest_id)->pins++;
    bool reverse = src && dst ? (uintptr_t)dst > (uintptr_t)src &&
                                   (uintptr_t)dst - (uintptr_t)src < length
                             : destination > source && destination - source < length;
    for (uint32_t done = 0; done < length;) {
        uint32_t part = length - done;
        if (part > XMS_COPY_QUANTUM) part = XMS_COPY_QUANTUM;
        uint32_t offset = reverse ? length - done - part : done;
        if (src && dst) memmove(dst + offset, src + offset, part);
        else for (uint32_t i = 0; i < part; i++) {
            uint32_t pos = offset + (reverse ? part - 1u - i : i);
            dos_phys_write8(vm, destination + pos, dos_phys_read8(vm, source + pos));
        }
        done += part;
        if (done < length && !xms_interrupt_window(vm)) { error = 0x8E; break; }
    }
    /* A reentrant call may have grown the handle table. Reacquire by ID. */
    if (source_id) xms_block(vm, source_id)->pins--;
    if (dest_id && dest_id != source_id) xms_block(vm, dest_id)->pins--;
    return error;
}

static uint16_t xms_umb_largest(dos_vm_t *vm)
{
    if (vm->total_mem_size < DOS_XMS_UMB_BASE + DOS_XMS_UMB_SIZE) return 0;
    uint16_t largest = 0, run = 0;
    for (unsigned i = 0; i < DOS_XMS_UMB_SIZE / 16u; i++) {
        run = vm->xms->umb_owner[i] ? 0 : run + 1u;
        if (run > largest) largest = run;
    }
    return largest;
}

static uint8_t xms_umb(dos_vm_t *vm, uint8_t function)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint16_t *owner = vm->xms->umb_owner;
    const uint32_t count = DOS_XMS_UMB_SIZE / 16u;
    if (function == 0x10) {
        uint16_t largest = xms_umb_largest(vm);
        uint32_t size = cpu->dx;
        if (!size || size > largest) {
            cpu->dx = largest;
            return largest ? 0xB0 : 0xB1;
        }
        uint32_t run = 0, first = 0;
        for (unsigned i = 0; i < count; i++) {
            run = owner[i] ? 0 : run + 1u;
            if (run == size) { first = i + 1u - size; break; }
        }
        for (unsigned i = 0; i < size; i++) owner[first + i] = first + 1u;
        cpu->bx = (DOS_XMS_UMB_BASE >> 4) + first;
        return 0;
    }
    uint32_t first = (uint32_t)cpu->dx - (DOS_XMS_UMB_BASE >> 4);
    if (first >= count || owner[first] != first + 1u) return 0xB2;
    uint32_t old = 0;
    while (first + old < count && owner[first + old] == first + 1u) old++;
    uint32_t size = function == 0x11 ? 0 : cpu->bx;
    if (size > old) {
        uint32_t available = old;
        while (first + available < count && !owner[first + available]) available++;
        if (size > available) {
            cpu->dx = (uint16_t)available;
            return 0xB0;
        }
    }
    for (uint32_t i = 0; i < (old > size ? old : size); i++)
        owner[first + i] = i < size ? first + 1u : 0;
    return 0;
}

void dos_xms_dispatch(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    struct dos_xms_state *xms = vm->xms;
    if (!xms) { xms_result(cpu, 0x80); return; }
    uint8_t function = cpu->ah, error = 0;
    if (function) xms->bios_hooked = true;
    switch (function) {
    case 0:
        cpu->ax = 0x0300; cpu->bx = 0x0100;
        cpu->dx = xms->hma_pages == DOS_XMS_HMA_SIZE / 4096u;
        return;
    case 1:
        if (xms_vdisk(vm)) error = 0x81;
        else if (xms->hma_pages != DOS_XMS_HMA_SIZE / 4096u) error = 0x90;
        else if (xms->hma_used) error = 0x91;
        else xms->hma_used = true;
        break;
    case 2:
        if (xms->hma_pages != DOS_XMS_HMA_SIZE / 4096u) error = 0x90;
        else if (!xms->hma_used) error = 0x93;
        else xms->hma_used = false;
        break;
    case 3: case 4: case 5: case 6:
        error = xms_a20(vm, function);
        break;
    case 7:
        cpu->ax = dos_a20_enabled(vm); cpu->bl = 0;
        return;
    case 8: case 0x88: {
        uint32_t largest = dpmi_ext_largest_free_page_count(vm) * 4u;
        uint32_t total = dpmi_ext_free_page_count(vm) * 4u;
        if (function == 8) {
            cpu->ax = largest > 0xFFFFu ? 0xFFFFu : largest;
            cpu->dx = total > 0xFFFFu ? 0xFFFFu : total;
        } else {
            cpu->eax = largest; cpu->edx = total;
            uint32_t memory = vm->system_mem_size ? vm->system_mem_size : vm->total_mem_size;
            if (memory > vm->total_mem_size) memory = vm->total_mem_size;
            if (memory > DOS_TOTAL_MEM) memory = DOS_TOTAL_MEM;
            /* Reserved RAM still contributes to the highest physical address. */
            cpu->ecx = memory ? memory - 1u : 0;
        }
        cpu->bl = total ? 0 : 0xA0;
        return;
    }
    case 9: case 0x89: {
        uint16_t handle;
        error = xms_allocate(vm, function == 9 ? cpu->dx : cpu->edx, &handle);
        cpu->dx = handle;
        break;
    }
    case 0x0A: {
        xms_block_t *block = xms_block(vm, cpu->dx);
        if (!block) error = 0xA2;
        else if (block->locks || block->pins) error = 0xAB;
        else {
            if (block->kilobytes) dpmi_ext_free_pages(vm, block->base, xms_pages(block->kilobytes));
            *block = (xms_block_t){0};
            xms->used--;
        }
        break;
    }
    case 0x0B:
        error = xms_move(vm);
        break;
    case 0x0C: case 0x0D: case 0x0E: case 0x8E: {
        xms_block_t *block = xms_block(vm, cpu->dx);
        if (!block) { error = 0xA2; break; }
        if (function == 0x0C) {
            if (block->locks == 255u) { error = 0xAC; break; }
            block->locks++;
            cpu->ax = 1; cpu->bx = (uint16_t)block->base; cpu->dx = block->base >> 16;
            return;
        }
        if (function == 0x0D) {
            if (!block->locks) error = 0xAA;
            else block->locks--;
            break;
        }
        uint32_t free_handles = xms->capacity - 1u - xms->used;
        cpu->ax = 1; cpu->bh = block->locks;
        if (function == 0x0E) {
            cpu->bl = free_handles > 255u ? 255u : free_handles;
            cpu->dx = block->kilobytes > 0xFFFFu ? 0xFFFFu : block->kilobytes;
        } else {
            cpu->bl = 0; cpu->cx = free_handles; cpu->edx = block->kilobytes;
        }
        return;
    }
    case 0x0F: case 0x8F:
        error = xms_resize(vm, cpu->dx, function == 0x0F ? cpu->bx : cpu->ebx);
        break;
    case 0x10: case 0x11: case 0x12:
        error = xms_umb(vm, function);
        if (function == 0x10 && !error) { cpu->ax = 1; return; }
        break;
    default:
        error = 0x80;
    }
    xms_result(cpu, error);
}
