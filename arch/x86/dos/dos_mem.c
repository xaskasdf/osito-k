/*
 * OsitoK DOS Memory Control Block (MCB) manager.
 *
 * Implements the conventional and upper-memory arenas used by INT 21h functions
 * 48h, 49h, and 4Ah. Every mutation first validates the complete chain so
 * a damaged guest MCB cannot make the kernel walk outside emulated memory.
 */

#include "cpu8086.h"
#include "dos_hostmem.h"
#include "dos_mem.h"
#include "dos_xms.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

#ifndef DOS_DIAGNOSTICS
#define DOS_DIAGNOSTICS 0
#endif

_Static_assert(DOS_TRANSFER_BASE >= DOS_VRAM_BASE + DOS_VRAM_SIZE,
               "DOS transfer window overlaps VGA");
_Static_assert(DOS_TRANSFER_BASE + DOS_TRANSFER_SIZE <= DOS_XMS_UMB_BASE &&
               DOS_TRANSFER_BASE + DOS_TRANSFER_SIZE <= DOS_EMS_PAGE_FRAME_BASE &&
               DOS_TRANSFER_BASE + DOS_TRANSFER_SIZE <= DOS_ROM_BASE,
               "DOS transfer window overlaps guest UMB, EMS or host ROM");

typedef struct dos_transfer_reservation {
    struct dos_transfer_reservation *next;
    uint64_t generation;
    uint32_t base, size, pages;
} dos_transfer_reservation_t;

/* Metadata and copy snapshots are kernel-owned. Only payload and the real
 * stack occupy the VM's reserved window; guest MCB policy cannot allocate it. */
bool dos_transfer_acquire(dos_vm_t *vm, uint32_t requested, uint32_t minimum,
                           dos_transfer_buffer_t *buffer)
{
    if (!buffer) return false;
    *buffer = (dos_transfer_buffer_t){0};
    if (!vm || !vm->mem ||
        vm->total_mem_size < DOS_TRANSFER_BASE + DOS_TRANSFER_SIZE ||
        vm->transfer_generation == UINT64_MAX || minimum > requested ||
        minimum > DOS_TRANSFER_BUFFER_MAX) return false;
    if (requested > DOS_TRANSFER_BUFFER_MAX) requested = DOS_TRANSFER_BUFFER_MAX;
    uint32_t wanted = ((requested + 15u) & ~15u) + DOS_TRANSFER_STACK_SIZE;
    uint32_t required = ((minimum + 15u) & ~15u) + DOS_TRANSFER_STACK_SIZE;
    uint32_t cursor = DOS_TRANSFER_BASE, base = 0, space = 0;
    dos_transfer_reservation_t *entry = vm->transfers;
    for (;;) {
        uint32_t end = entry ? entry->base : DOS_TRANSFER_BASE + DOS_TRANSFER_SIZE;
        uint32_t gap = end - cursor;
        if (gap >= required && gap > space) { base = cursor; space = gap; }
        if (gap >= wanted || !entry) break;
        cursor = entry->base + entry->size;
        entry = entry->next;
    }
    if (!space) return false;
    uint32_t size = space < wanted ? space : wanted;
    uint32_t capacity = size - DOS_TRANSFER_STACK_SIZE;
    uint32_t pages = (sizeof(*entry) + capacity + 4095u) / 4096u;
    entry = dos_host_alloc_pages(pages);
    if (!entry) return false;
    entry->generation = ++vm->transfer_generation;
    entry->base = base;
    entry->size = size;
    entry->pages = pages;
    dos_transfer_reservation_t **link = &vm->transfers;
    while (*link && (*link)->base < base) link = &(*link)->next;
    entry->next = *link;
    *link = entry;
    for (uint32_t i = 0; i < size; i++) dos_mem_write8(vm, base + i, 0);
    *buffer = (dos_transfer_buffer_t){ entry->generation, capacity,
        base >> 4, (base + capacity) >> 4, (uint8_t *)(entry + 1) };
    return true;
}

static void dos_transfer_destroy(dos_vm_t *vm, dos_transfer_reservation_t *entry)
{
    if (vm->mem && entry->base <= vm->total_mem_size &&
        entry->size <= vm->total_mem_size - entry->base)
        for (uint32_t i = 0; i < entry->size; i++) dos_mem_write8(vm, entry->base + i, 0);
    dos_host_free_pages(entry, entry->pages);
}

bool dos_transfer_release(dos_vm_t *vm, uint64_t generation)
{
    if (!vm || !generation) return false;
    dos_transfer_reservation_t **link = &vm->transfers;
    while (*link && (*link)->generation != generation) link = &(*link)->next;
    if (!*link) return false;
    dos_transfer_reservation_t *entry = *link;
    *link = entry->next;
    dos_transfer_destroy(vm, entry);
    return true;
}

/* An EXEC snapshot preserves earlier calls, including borrowed clients.
 * Generation values also make release after a non-local unwind harmless. */
void dos_transfer_release_after(dos_vm_t *vm, uint64_t generation)
{
    if (!vm) return;
    dos_transfer_reservation_t **link = &vm->transfers;
    while (*link) {
        dos_transfer_reservation_t *entry = *link;
        if (entry->generation <= generation) { link = &entry->next; continue; }
        *link = entry->next;
        dos_transfer_destroy(vm, entry);
    }
}

static uint16_t dos_mcb_limit(const dos_vm_t *vm)
{
    if (!vm || !vm->mem) return 0;
    uint32_t limit = vm->total_mem_size;
    if (limit > DOS_CONV_TOP) limit = DOS_CONV_TOP;
    /* Reserve the bridge before a COM can own all conventional memory. */
    if (vm->total_mem_size >= DOS_XMS_UMB_BASE + DOS_XMS_UMB_SIZE) limit -= 16u;
    return (uint16_t)(limit >> 4);
}

static bool dos_mcb_has_umb(const dos_vm_t *vm)
{
    return vm && vm->uppermem_root == dos_mcb_limit(vm) && vm->uppermem_end;
}

static bool dos_mcb_is_bridge(const dos_vm_t *vm, uint16_t segment)
{
    if (!dos_mcb_has_umb(vm)) return false;
    if (segment == vm->uppermem_root) return true;
    uint16_t end = dos_xms_umb_dos_end(vm, segment);
    return end && end < vm->uppermem_end && segment == end - 1u;
}

static void dos_mcb_clear(dos_mcb_t *mcb)
{
    mcb->owner = 0;
    for (int i = 0; i < 3; i++) mcb->reserved[i] = 0;
    for (int i = 0; i < 8; i++) mcb->name[i] = 0;
}

static int dos_mcb_decode(dos_vm_t *vm, uint16_t segment,
                          dos_mcb_t **out, uint16_t *next_segment)
{
    uint16_t limit = dos_mcb_limit(vm);
    if (!limit || segment < vm->first_mcb)
        return -1;

    uint32_t address = (uint32_t)segment << 4;
    if (address > vm->total_mem_size ||
        sizeof(dos_mcb_t) > vm->total_mem_size - address)
        return -1;

    dos_mcb_t *mcb = (dos_mcb_t *)(vm->mem + address);
    if (mcb->type != 'M' && mcb->type != 'Z') return -1;

    uint32_t next = (uint32_t)segment + 1u + mcb->size;
    uint8_t last_type = 'Z';
    if (segment < limit) {
        if (vm->uppermem_link) last_type = 'M';
    } else {
        if (!dos_mcb_has_umb(vm)) return -1;
        if (dos_mcb_is_bridge(vm, segment)) {
            uint16_t destination = dos_xms_umb_dos_next(vm, segment + 1u);
            if (!destination || next != destination ||
                mcb->type != 'M' || mcb->owner != 8u) return -1;
            if (out) *out = mcb;
            if (next_segment) *next_segment = destination;
            return 0;
        }
        limit = dos_xms_umb_dos_end(vm, segment);
        if (!limit) return -1;
        if (limit < vm->uppermem_end) { limit--; last_type = 'M'; }
    }
    if (next > limit || mcb->type != (next == limit ? last_type : 'M')) return -1;

    if (out) *out = mcb;
    if (next_segment) *next_segment = (uint16_t)next;
    return 0;
}

static int dos_mcb_validate_range(dos_vm_t *vm, uint16_t segment, uint16_t limit)
{
    if (!segment || segment >= limit) return -1;
    uint32_t max_blocks = (uint32_t)limit - segment;

    for (uint32_t count = 0; count <= max_blocks; count++) {
        dos_mcb_t *mcb;
        uint16_t next;
        if (dos_mcb_decode(vm, segment, &mcb, &next) < 0) return -1;
        if (next >= limit) return next == limit ? 0 : -1;
        if (mcb->type == 'Z') return -1;
        segment = next;
    }
    return -1;
}

static int dos_mcb_validate(dos_vm_t *vm)
{
    if (!vm || (vm->uppermem_link && !dos_mcb_has_umb(vm)) ||
        dos_mcb_validate_range(vm, vm->first_mcb, dos_mcb_limit(vm)) < 0)
        return -1;
    if (dos_mcb_has_umb(vm))
        return dos_mcb_validate_range(vm, vm->uppermem_root, vm->uppermem_end);
    return 0;
}

static int dos_mcb_find_validated(dos_vm_t *vm, uint16_t target,
                                  dos_mcb_t **out, uint16_t *previous)
{
    uint16_t segment = dos_mcb_has_umb(vm) && target >= vm->uppermem_root
        ? vm->uppermem_root : vm->first_mcb;
    uint16_t prev = 0;
    for (;;) {
        dos_mcb_t *mcb;
        uint16_t next;
        if (dos_mcb_decode(vm, segment, &mcb, &next) < 0) return -1;
        if (segment == target) {
            if (out) *out = mcb;
            if (previous) *previous = prev;
            return 0;
        }
        if (mcb->type == 'Z') return -1;
        prev = segment;
        segment = next;
    }
}

static int dos_mcb_coalesce_forward(dos_vm_t *vm, uint16_t segment,
                                    dos_mcb_t *mcb)
{
    while (mcb->type == 'M') {
        uint16_t next_segment = (uint16_t)(segment + 1u + mcb->size);
        dos_mcb_t *next;
        if (dos_mcb_decode(vm, next_segment, &next, NULL) < 0) return -1;
        if (next->owner != 0) break;

        uint32_t combined = (uint32_t)mcb->size + 1u + next->size;
        if (combined > 0xFFFFu) return -1;
        mcb->size = (uint16_t)combined;
        mcb->type = next->type;
    }
    return 0;
}

static void dos_mcb_split(dos_vm_t *vm, uint16_t segment, dos_mcb_t *mcb,
                          uint16_t paragraphs)
{
    uint16_t old_size = mcb->size;
    uint8_t old_type = mcb->type;
    uint16_t free_segment = (uint16_t)(segment + 1u + paragraphs);
    dos_mcb_t *free_mcb = (dos_mcb_t *)(vm->mem +
                                        ((uint32_t)free_segment << 4));

    free_mcb->type = old_type;
    free_mcb->size = (uint16_t)(old_size - paragraphs - 1u);
    dos_mcb_clear(free_mcb);
    mcb->type = 'M';
    mcb->size = paragraphs;
}

static void dos_mcb_split_high(dos_vm_t *vm, uint16_t segment,
                               dos_mcb_t *mcb, uint16_t paragraphs,
                               dos_mcb_t **allocated,
                               uint16_t *allocated_segment)
{
    uint16_t old_size = mcb->size;
    uint8_t old_type = mcb->type;
    uint16_t high_segment = (uint16_t)(segment + old_size - paragraphs);
    dos_mcb_t *high = (dos_mcb_t *)(vm->mem +
                                     ((uint32_t)high_segment << 4));

    mcb->type = 'M';
    mcb->size = (uint16_t)(old_size - paragraphs - 1u);
    dos_mcb_clear(mcb);

    high->type = old_type;
    high->size = paragraphs;
    dos_mcb_clear(high);
    *allocated = high;
    *allocated_segment = high_segment;
}

void dos_mem_init(dos_vm_t *vm)
{
    if (!vm || !vm->mem) return;
    dos_xms_umb_release_dos(vm);
    vm->allocation_strategy = DOS_ALLOC_FIRST_FIT;
    vm->uppermem_link = 0;
    vm->uppermem_root = 0xFFFFu;
    vm->uppermem_end = 0;
    vm->first_mcb = DOS_CONV_BASE / 16;
    uint16_t limit = dos_mcb_limit(vm);
    if (limit <= vm->first_mcb) {
        vm->first_mcb = 0;
        return;
    }

    dos_mcb_t *mcb = (dos_mcb_t *)(vm->mem +
                                    ((uint32_t)vm->first_mcb << 4));
    mcb->type = 'Z';
    mcb->size = (uint16_t)(limit - vm->first_mcb - 1u);
    dos_mcb_clear(mcb);
}

static void dos_mcb_bridge(dos_vm_t *vm, uint16_t segment, uint16_t destination)
{
    dos_mcb_t *mcb = (dos_mcb_t *)(vm->mem + ((uint32_t)segment << 4));
    dos_mcb_clear(mcb);
    mcb->type = 'M';
    mcb->owner = 8;
    mcb->size = destination - segment - 1u;
    mcb->name[0] = 'S'; mcb->name[1] = 'C';
}

int dos_mem_set_umb_link(dos_vm_t *vm, uint16_t linked)
{
    if (linked > 1) return DOS_MEM_ERR_UNAVAILABLE;
    if (dos_mcb_validate(vm) < 0) return DOS_MEM_ERR_ARENA;
    if (!dos_mcb_has_umb(vm)) {
        uint16_t end = dos_xms_umb_claim_dos(vm);
        if (!end) return DOS_MEM_ERR_UNAVAILABLE;
        vm->uppermem_root = dos_mcb_limit(vm);
        vm->uppermem_end = end;
        uint16_t start = dos_xms_umb_dos_next(vm, vm->uppermem_root);
        dos_mcb_bridge(vm, vm->uppermem_root, start);
        while (start) {
            uint16_t region_end = dos_xms_umb_dos_end(vm, start);
            uint16_t next = dos_xms_umb_dos_next(vm, region_end);
            uint16_t free_end = region_end - (next ? 1u : 0u);
            if (start < free_end) {
                dos_mcb_t *mcb = (dos_mcb_t *)(vm->mem + ((uint32_t)start << 4));
                dos_mcb_clear(mcb);
                mcb->type = next ? 'M' : 'Z';
                mcb->size = free_end - start - 1u;
            }
            if (next) dos_mcb_bridge(vm, free_end, next);
            start = next;
        }
    }
    uint16_t segment = vm->first_mcb;
    for (;;) {
        dos_mcb_t *mcb;
        uint16_t next;
        if (dos_mcb_decode(vm, segment, &mcb, &next) < 0) return DOS_MEM_ERR_ARENA;
        if (next == dos_mcb_limit(vm)) {
            mcb->type = linked ? 'M' : 'Z';
            vm->uppermem_link = linked;
            return dos_mcb_validate(vm) < 0 ? DOS_MEM_ERR_ARENA : DOS_MEM_OK;
        }
        segment = next;
    }
}

typedef struct { uint16_t start, end; } dos_mcb_range_t;

static unsigned dos_mcb_search_ranges(const dos_vm_t *vm, dos_mcb_range_t ranges[2])
{
    unsigned count = 0;
    bool high = vm->uppermem_link && dos_mcb_has_umb(vm) &&
        (vm->allocation_strategy & (DOS_ALLOC_UMB_ONLY | DOS_ALLOC_UMB_FIRST));
    if (high) ranges[count++] = (dos_mcb_range_t){vm->uppermem_root, vm->uppermem_end};
    if (!high || !(vm->allocation_strategy & DOS_ALLOC_UMB_ONLY))
        ranges[count++] = (dos_mcb_range_t){vm->first_mcb, dos_mcb_limit(vm)};
    return count;
}

uint16_t dos_mem_alloc(dos_vm_t *vm, uint16_t paragraphs, uint16_t *largest)
{
    return dos_mem_alloc_owned(vm, paragraphs, largest, vm ? vm->current_psp : 0);
}

uint16_t dos_mem_alloc_owned(dos_vm_t *vm, uint16_t paragraphs,
                             uint16_t *largest, uint16_t owner)
{
    uint16_t segment = 0;
    (void)dos_mem_alloc_checked(vm, paragraphs, owner, &segment, largest);
    return segment;
}

int dos_mem_alloc_checked(dos_vm_t *vm, uint16_t paragraphs, uint16_t owner,
                          uint16_t *result, uint16_t *largest)
{
    if (result) *result = 0;
    if (largest) *largest = 0;
    if (dos_mcb_validate(vm) < 0) return DOS_MEM_ERR_ARENA;

    uint16_t best_free = 0;
    dos_mcb_t *selected = NULL;
    uint16_t selected_segment = 0;
    uint8_t fit = vm->allocation_strategy & DOS_ALLOC_FIT_MASK;
    if (fit > DOS_ALLOC_LAST_FIT) fit = DOS_ALLOC_FIRST_FIT;

    dos_mcb_range_t ranges[2];
    unsigned count = dos_mcb_search_ranges(vm, ranges);
    for (unsigned r = 0; r < count && !selected; r++) {
        uint16_t segment = ranges[r].start;
        while (segment < ranges[r].end) {
            dos_mcb_t *mcb;
            uint16_t next;
            if (dos_mcb_decode(vm, segment, &mcb, &next) < 0) return DOS_MEM_ERR_ARENA;

            if (mcb->owner == 0) {
                if (dos_mcb_coalesce_forward(vm, segment, mcb) < 0) return DOS_MEM_ERR_ARENA;
                if (mcb->size > best_free) best_free = mcb->size;
                if (mcb->size >= paragraphs) {
                    if (fit == DOS_ALLOC_FIRST_FIT ||
                        fit == DOS_ALLOC_LAST_FIT || !selected ||
                        mcb->size < selected->size) {
                        selected = mcb;
                        selected_segment = segment;
                    }
                    if (fit == DOS_ALLOC_FIRST_FIT ||
                        (fit == DOS_ALLOC_BEST_FIT &&
                         mcb->size == paragraphs))
                        break;
                }
            }

            if (mcb->type == 'Z') break;
            segment = (uint16_t)(segment + 1u + mcb->size);
        }
    }

    if (!selected) {
        if (largest) *largest = best_free;
        return DOS_MEM_ERR_NOMEM;
    }

    /* MS-DOS permits a zero-paragraph block; it still consumes an MCB. */
    if (selected->size > paragraphs) {
        if (fit == DOS_ALLOC_LAST_FIT) {
            dos_mcb_split_high(vm, selected_segment, selected, paragraphs,
                               &selected, &selected_segment);
        } else {
            dos_mcb_split(vm, selected_segment, selected, paragraphs);
        }
    }
    selected->owner = owner ? owner : (uint16_t)(selected_segment + 1u);

#if DOS_DIAGNOSTICS
    serial_puts("[MCB] alloc mcb=");
    serial_puthex(selected_segment, 4);
    serial_puts(" owner=");
    serial_puthex(selected->owner, 4);
    serial_puts(" size=");
    serial_puthex(selected->size, 4);
    serial_puts("\n");
#endif
    if (result) *result = (uint16_t)(selected_segment + 1u);
    return DOS_MEM_OK;
}

int dos_mem_free(dos_vm_t *vm, uint16_t segment)
{
    if (!segment) return DOS_MEM_ERR_BLOCK;
    if (dos_mcb_validate(vm) < 0) return DOS_MEM_ERR_ARENA;

    uint16_t mcb_segment = (uint16_t)(segment - 1u);
    uint16_t previous = 0;
    dos_mcb_t *mcb;
    if (dos_mcb_find_validated(vm, mcb_segment, &mcb, &previous) < 0 ||
        mcb->owner == 0 || dos_mcb_is_bridge(vm, mcb_segment))
        return DOS_MEM_ERR_BLOCK;

    dos_mcb_clear(mcb);
    if (dos_mcb_coalesce_forward(vm, mcb_segment, mcb) < 0)
        return DOS_MEM_ERR_ARENA;

    if (previous) {
        dos_mcb_t *prev;
        if (dos_mcb_decode(vm, previous, &prev, NULL) < 0)
            return DOS_MEM_ERR_ARENA;
        if (prev->owner == 0 &&
            dos_mcb_coalesce_forward(vm, previous, prev) < 0)
            return DOS_MEM_ERR_ARENA;
    }
    return dos_mcb_validate(vm) == 0 ? DOS_MEM_OK : DOS_MEM_ERR_ARENA;
}

static int dos_mem_block_info(dos_vm_t *vm, uint16_t segment,
                              dos_mcb_t **block, uint16_t *current_size,
                              uint16_t *max_avail)
{
    if (current_size) *current_size = 0;
    if (max_avail) *max_avail = 0;
    if (!segment) return DOS_MEM_ERR_BLOCK;
    if (dos_mcb_validate(vm) < 0) return DOS_MEM_ERR_ARENA;

    uint16_t mcb_segment = (uint16_t)(segment - 1u);
    dos_mcb_t *mcb;
    if (dos_mcb_find_validated(vm, mcb_segment, &mcb, NULL) < 0 ||
        mcb->owner == 0 || dos_mcb_is_bridge(vm, mcb_segment))
        return DOS_MEM_ERR_BLOCK;

    uint32_t available = mcb->size;
    uint16_t scan_segment = mcb_segment;
    dos_mcb_t *scan = mcb;
    while (scan->type == 'M') {
        uint16_t next_segment = (uint16_t)(scan_segment + 1u + scan->size);
        dos_mcb_t *next;
        if (dos_mcb_decode(vm, next_segment, &next, NULL) < 0)
            return DOS_MEM_ERR_ARENA;
        if (next->owner != 0) break;
        available += 1u + next->size;
        if (available > 0xFFFFu) return DOS_MEM_ERR_ARENA;
        scan_segment = next_segment;
        scan = next;
    }

    if (block) *block = mcb;
    if (current_size) *current_size = mcb->size;
    if (max_avail) *max_avail = (uint16_t)available;
    return DOS_MEM_OK;
}

int dos_mem_query_block(dos_vm_t *vm, uint16_t segment,
                        uint16_t *current_size, uint16_t *max_avail)
{
    return dos_mem_block_info(vm, segment, NULL, current_size, max_avail);
}

int dos_mem_largest_available(dos_vm_t *vm, uint16_t *largest)
{
    if (largest) *largest = 0;
    if (dos_mcb_validate(vm) < 0) return DOS_MEM_ERR_ARENA;

    uint32_t best = 0;
    dos_mcb_range_t ranges[2];
    unsigned count = dos_mcb_search_ranges(vm, ranges);
    for (unsigned r = 0; r < count; r++) {
        uint32_t run = 0;
        bool free_run = false;
        uint16_t segment = ranges[r].start;
        while (segment < ranges[r].end) {
            dos_mcb_t *mcb;
            uint16_t next;
            if (dos_mcb_decode(vm, segment, &mcb, &next) < 0)
                return DOS_MEM_ERR_ARENA;

            if (mcb->owner == 0) {
                if (free_run) run++;
                run += mcb->size;
                free_run = true;
                if (run > best) best = run;
            } else {
                run = 0;
                free_run = false;
            }

            if (mcb->type == 'Z') break;
            segment = next;
        }
    }

    if (best > 0xFFFFu) return DOS_MEM_ERR_ARENA;
    if (largest) *largest = (uint16_t)best;
    return DOS_MEM_OK;
}

int dos_mem_resize(dos_vm_t *vm, uint16_t segment, uint16_t new_size,
                   uint16_t *max_avail)
{
    dos_mcb_t *mcb;
    uint16_t current_size;
    uint16_t available;
    int result = dos_mem_block_info(vm, segment, &mcb, &current_size,
                                    &available);
    if (result != DOS_MEM_OK) return result;
    if (max_avail) *max_avail = available;

    if (new_size > available)
        return DOS_MEM_ERR_NOMEM;

    uint16_t mcb_segment = (uint16_t)(segment - 1u);
    if (new_size > current_size &&
        dos_mcb_coalesce_forward(vm, mcb_segment, mcb) < 0)
        return DOS_MEM_ERR_ARENA;

    if (new_size < mcb->size) {
        dos_mcb_split(vm, mcb_segment, mcb, new_size);
        uint16_t free_segment = (uint16_t)(mcb_segment + 1u + new_size);
        dos_mcb_t *free_mcb;
        if (dos_mcb_decode(vm, free_segment, &free_mcb, NULL) < 0 ||
            dos_mcb_coalesce_forward(vm, free_segment, free_mcb) < 0)
            return DOS_MEM_ERR_ARENA;
    }

#if DOS_DIAGNOSTICS
    serial_puts("[MCB] resize mcb=");
    serial_puthex(mcb_segment, 4);
    serial_puts(" size=");
    serial_puthex(mcb->size, 4);
    serial_puts("\n");
#endif
    return dos_mcb_validate(vm) == 0 ? DOS_MEM_OK : DOS_MEM_ERR_ARENA;
}

int dos_mem_free_owner(dos_vm_t *vm, uint16_t owner)
{
    if (!owner || dos_mcb_validate(vm) < 0) return -1;

    /* Mark first so coalescing cannot hide a later block owned by the same
     * process. A second pass then rebuilds the maximal free spans. */
    dos_mcb_range_t ranges[2] = {
        {vm->first_mcb, dos_mcb_limit(vm)}, {vm->uppermem_root, vm->uppermem_end}
    };
    unsigned count = dos_mcb_has_umb(vm) ? 2u : 1u;
    for (unsigned r = 0; r < count; r++) {
        uint16_t segment = ranges[r].start;
        while (segment < ranges[r].end) {
            dos_mcb_t *mcb;
            uint16_t next;
            if (dos_mcb_decode(vm, segment, &mcb, &next) < 0) return -1;
            uint8_t type = mcb->type;
            if (mcb->owner == owner && !dos_mcb_is_bridge(vm, segment)) dos_mcb_clear(mcb);
            if (type == 'Z') break;
            segment = next;
        }
    }

    for (unsigned r = 0; r < count; r++) {
        uint16_t segment = ranges[r].start;
        while (segment < ranges[r].end) {
            dos_mcb_t *mcb;
            if (dos_mcb_decode(vm, segment, &mcb, NULL) < 0) return -1;
            if (mcb->owner == 0 && dos_mcb_coalesce_forward(vm, segment, mcb) < 0)
                return -1;
            if (mcb->type == 'Z') break;
            segment = (uint16_t)(segment + 1u + mcb->size);
        }
    }
    return dos_mcb_validate(vm);
}

static int dos_transfer_selftest(void)
{
    const unsigned pages = 256u;
    unsigned vm_pages = (sizeof(dos_vm_t) + 4095u) / 4096u;
    dos_vm_t *vm = dos_host_alloc_pages(vm_pages);
    uint8_t *memory = dos_host_alloc_pages(pages);
    if (!vm || !memory) {
        dos_host_free_pages(vm, vm_pages);
        dos_host_free_pages(memory, pages);
        return 1;
    }
    for (unsigned i = 0; i < vm_pages * 4096u; i++) ((uint8_t *)vm)[i] = 0;
    for (unsigned i = 0; i < pages * 4096u; i++) memory[i] = 0xA5;
    vm->mem = memory;
    vm->total_mem_size = pages * 4096u;
    dos_mem_init(vm);
    uint16_t largest = 0;
    int failures = 0;
    unsigned checks = 0;
#define TRANSFER_CHECK(test) do { if (!(test)) { \
    serial_puts("[DOS-TRANSFER] failed check="); serial_putdec(checks); serial_puts("\n"); \
    failures++; } checks++; } while (0)
    TRANSFER_CHECK(!dos_mem_alloc(vm, 0xFFFFu, &largest) && largest);
    uint16_t all = dos_mem_alloc(vm, largest, NULL);
    TRANSFER_CHECK(all && !dos_mem_alloc(vm, 1u, &largest) && !largest);
    if (!all) {
        dos_host_free_pages(memory, pages);
        dos_host_free_pages(vm, vm_pages);
        return failures + 1;
    }
    dos_mcb_t saved = *(dos_mcb_t *)(memory + ((uint32_t)(all - 1u) << 4));
    dos_transfer_buffer_t a, b, c;
    const uint32_t sizes[] = { 0, 1, 15, 16, 17, 257, 0x3FFF, 0x4000, 0x4001, UINT32_MAX };
    for (unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++) {
        uint32_t size = sizes[n] > DOS_TRANSFER_BUFFER_MAX ? DOS_TRANSFER_BUFFER_MAX : sizes[n];
        TRANSFER_CHECK(dos_transfer_acquire(vm, sizes[n], sizes[n] ? 1u : 0u, &a));
        if (!a.generation) continue;
        TRANSFER_CHECK(a.segment == DOS_TRANSFER_BASE >> 4 &&
                       a.capacity == ((size + 15u) & ~15u) && a.scratch &&
                       a.stack_segment == a.segment + a.capacity / 16u);
        bool zero = true;
        for (uint32_t i = 0; i < a.capacity + DOS_TRANSFER_STACK_SIZE; i++) {
            if (memory[DOS_TRANSFER_BASE + i]) zero = false;
            memory[DOS_TRANSFER_BASE + i] = 0x3C;
        }
        TRANSFER_CHECK(zero && dos_transfer_release(vm, a.generation));
        zero = true;
        for (uint32_t i = 0; i < a.capacity + DOS_TRANSFER_STACK_SIZE; i++)
            if (memory[DOS_TRANSFER_BASE + i]) zero = false;
        TRANSFER_CHECK(zero && !dos_transfer_release(vm, a.generation) && !vm->transfers);
    }
    TRANSFER_CHECK(!dos_transfer_acquire(vm, 1u, 2u, &a) && !a.generation);
    TRANSFER_CHECK(!dos_transfer_acquire(vm, UINT32_MAX, DOS_TRANSFER_BUFFER_MAX + 1u, &a));
    vm->total_mem_size = DOS_TRANSFER_BASE + DOS_TRANSFER_SIZE - 1u;
    TRANSFER_CHECK(!dos_transfer_acquire(vm, 1u, 1u, &a));
    vm->total_mem_size = pages * 4096u;

    /* Holes are reused by address; stale handles never release a newer call. */
    TRANSFER_CHECK(dos_transfer_acquire(vm, 35u, 35u, &a));
    TRANSFER_CHECK(dos_transfer_acquire(vm, 35u, 35u, &b));
    TRANSFER_CHECK(dos_transfer_acquire(vm, 35u, 35u, &c));
    if (a.generation && b.generation && c.generation) {
        memory[(uint32_t)a.segment << 4] = 0x12;
        memory[(uint32_t)c.segment << 4] = 0x34;
        uint64_t old = b.generation, mark = vm->transfer_generation;
        uint16_t hole = b.segment;
        TRANSFER_CHECK(dos_transfer_release(vm, old) && dos_transfer_acquire(vm, 35u, 35u, &b));
        TRANSFER_CHECK(b.segment == hole && b.generation > mark && !dos_transfer_release(vm, old));
        dos_transfer_release_after(vm, mark);
        TRANSFER_CHECK(!dos_transfer_release(vm, b.generation) &&
                       memory[(uint32_t)a.segment << 4] == 0x12 &&
                       memory[(uint32_t)c.segment << 4] == 0x34);
    }
    dos_transfer_release_after(vm, 0);
    dos_transfer_buffer_t d;
    TRANSFER_CHECK(dos_transfer_acquire(vm, 0x3000u, 1u, &a));
    TRANSFER_CHECK(dos_transfer_acquire(vm, DOS_TRANSFER_BUFFER_MAX, 1u, &b));
    TRANSFER_CHECK(dos_transfer_acquire(vm, DOS_TRANSFER_BUFFER_MAX, 1u, &c));
    TRANSFER_CHECK(dos_transfer_acquire(vm, DOS_TRANSFER_BUFFER_MAX, 1u, &d) && d.capacity == 0x1000u);
    TRANSFER_CHECK(!dos_transfer_acquire(vm, 0, 0, &a));
    dos_transfer_release_after(vm, 0);
    uint64_t mark = vm->transfer_generation;
    unsigned held = 0;
    while (held < 64u && dos_transfer_acquire(vm, 35u, 35u, &a)) held++;
    TRANSFER_CHECK(held == DOS_TRANSFER_SIZE / (48u + DOS_TRANSFER_STACK_SIZE));
    TRANSFER_CHECK(!dos_transfer_acquire(vm, 35u, 35u, &a));
    dos_transfer_release_after(vm, mark);
    TRANSFER_CHECK(!vm->transfers && dos_transfer_acquire(vm, DOS_TRANSFER_BUFFER_MAX, 1u, &a));
    dos_transfer_release_after(vm, 0);
    TRANSFER_CHECK(!vm->transfers && !dos_transfer_release(vm, a.generation));
    vm->transfer_generation = UINT64_MAX;
    TRANSFER_CHECK(!dos_transfer_acquire(vm, 1u, 1u, &a) && !vm->transfers);
    bool intact = true;
    for (unsigned i = 0; i < sizeof(saved); i++)
        if (memory[((uint32_t)(all - 1u) << 4) + i] != ((uint8_t *)&saved)[i]) intact = false;
    TRANSFER_CHECK(intact && memory[DOS_TRANSFER_BASE - 1u] == 0xA5 &&
                   memory[DOS_TRANSFER_BASE + DOS_TRANSFER_SIZE] == 0xA5);
    TRANSFER_CHECK(!dos_mem_alloc(vm, 1u, &largest) && !largest &&
                   dos_mem_free(vm, all) == DOS_MEM_OK);
    serial_puts("[DOS-TRANSFER] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef TRANSFER_CHECK
    dos_host_free_pages(memory, pages);
    dos_host_free_pages(vm, vm_pages);
    return failures;
}

int dos_mem_selftest(void)
{
    const uint64_t pages = (DOS_CONV_TOP + 4095u) / 4096u;
    uint8_t *memory = (uint8_t *)dos_host_alloc_pages(pages);
    if (!memory) return 1;
    for (uint64_t i = 0; i < pages * 4096u; i++) memory[i] = 0;

    dos_vm_t vm;
    uint8_t *raw = (uint8_t *)&vm;
    for (uint64_t i = 0; i < sizeof(vm); i++) raw[i] = 0;
    vm.mem = memory;
    vm.total_mem_size = DOS_CONV_TOP;
    vm.current_psp = 0x1234;
    dos_mem_init(&vm);

    int failures = dos_mcb_validate(&vm) < 0;
    uint16_t initial_size = (uint16_t)(DOS_CONV_TOP / 16u -
                                       vm.first_mcb - 1u);

    uint16_t zero = dos_mem_alloc(&vm, 0, NULL);
    if (!zero || dos_mem_free(&vm, zero) < 0) failures++;

    uint16_t a = dos_mem_alloc(&vm, 0x100, NULL);
    uint16_t b = dos_mem_alloc(&vm, 0x080, NULL);
    uint16_t c = dos_mem_alloc(&vm, 0x040, NULL);
    if (!a || !b || !c || dos_mem_free(&vm, a) < 0 ||
        dos_mem_free(&vm, c) < 0 || dos_mem_free(&vm, b) < 0 ||
        dos_mcb_validate(&vm) < 0)
        failures++;
    dos_mcb_t *root = (dos_mcb_t *)(vm.mem +
                                     ((uint32_t)vm.first_mcb << 4));
    if (root->type != 'Z' || root->owner != 0 ||
        root->size != initial_size)
        failures++;

    a = dos_mem_alloc(&vm, 0x100, NULL);
    b = dos_mem_alloc(&vm, 0x100, NULL);
    uint16_t maximum = 0;
    if (!a || !b || dos_mem_free(&vm, b) < 0 ||
        dos_mem_resize(&vm, a, 0x180, &maximum) < 0 ||
        dos_mem_resize(&vm, a, 0x080, &maximum) < 0 ||
        dos_mem_resize(&vm, a, 0xFFFFu, &maximum) == 0 ||
        maximum != initial_size || dos_mem_free(&vm, a) < 0)
        failures++;

    /* First fit chooses the first suitable hole. */
    dos_mem_init(&vm);
    vm.current_psp = 0x1234;
    uint16_t first_hole = dos_mem_alloc(&vm, 0x40, NULL);
    uint16_t guard_a = dos_mem_alloc(&vm, 0x10, NULL);
    uint16_t second_hole = dos_mem_alloc(&vm, 0x20, NULL);
    uint16_t guard_b = dos_mem_alloc(&vm, 0x10, NULL);
    if (!first_hole || !guard_a || !second_hole || !guard_b ||
        dos_mem_free(&vm, first_hole) < 0 ||
        dos_mem_free(&vm, second_hole) < 0 ||
        dos_mem_alloc(&vm, 0x10, NULL) != first_hole)
        failures++;

    /* Best fit chooses the smallest suitable hole. */
    dos_mem_init(&vm);
    first_hole = dos_mem_alloc(&vm, 0x40, NULL);
    guard_a = dos_mem_alloc(&vm, 0x10, NULL);
    second_hole = dos_mem_alloc(&vm, 0x20, NULL);
    guard_b = dos_mem_alloc(&vm, 0x10, NULL);
    vm.allocation_strategy = DOS_ALLOC_BEST_FIT;
    if (!first_hole || !guard_a || !second_hole || !guard_b ||
        dos_mem_free(&vm, first_hole) < 0 ||
        dos_mem_free(&vm, second_hole) < 0 ||
        dos_mem_alloc(&vm, 0x10, NULL) != second_hole)
        failures++;

    /* Last fit splits the selected arena from its high end. */
    dos_mem_init(&vm);
    vm.allocation_strategy = DOS_ALLOC_LAST_FIT;
    uint16_t limit = dos_mcb_limit(&vm);
    uint16_t high = dos_mem_alloc(&vm, 0x10, NULL);
    dos_mcb_t *high_mcb = high ? (dos_mcb_t *)(vm.mem +
                                  ((uint32_t)(high - 1u) << 4)) : NULL;
    if (high != (uint16_t)(limit - 0x10u) || !high_mcb ||
        root->type != 'M' || root->owner != 0 ||
        root->size != (uint16_t)(initial_size - 0x11u) ||
        high_mcb->type != 'Z' || high_mcb->owner != vm.current_psp ||
        high_mcb->size != 0x10u || dos_mem_free(&vm, high) < 0)
        failures++;

    /* With no UMB arena, UMB-first still falls back to conventional RAM. */
    dos_mem_init(&vm);
    vm.allocation_strategy = DOS_ALLOC_UMB_FIRST | DOS_ALLOC_BEST_FIT;
    a = dos_mem_alloc(&vm, 0x10, NULL);
    if (a != (uint16_t)(vm.first_mcb + 1u) || dos_mem_free(&vm, a) < 0)
        failures++;

    dos_mem_init(&vm);
    vm.current_psp = 0;
    a = dos_mem_alloc(&vm, 0x10, NULL);
    dos_mcb_t *owned = a ? (dos_mcb_t *)(vm.mem +
                             ((uint32_t)(a - 1u) << 4)) : NULL;
    if (!a || !owned || owned->owner != a || dos_mem_free(&vm, a) < 0)
        failures++;

    dos_mem_init(&vm);
    vm.current_psp = 0x1234;
    a = dos_mem_alloc(&vm, 0x20, NULL);
    vm.current_psp = 0x5678;
    b = dos_mem_alloc(&vm, 0x30, NULL);
    c = dos_mem_alloc(&vm, 0x40, NULL);
    vm.current_psp = 0x1234;
    uint16_t parent_tail = dos_mem_alloc(&vm, 0x10, NULL);
    if (!a || !b || !c || !parent_tail ||
        dos_mem_free_owner(&vm, 0x5678) < 0 ||
        ((dos_mcb_t *)(vm.mem + ((uint32_t)(a - 1u) << 4)))->owner !=
            0x1234u ||
        ((dos_mcb_t *)(vm.mem +
                       ((uint32_t)(parent_tail - 1u) << 4)))->owner !=
            0x1234u ||
        dos_mcb_validate(&vm) < 0)
        failures++;

    dos_mem_init(&vm);
    root = (dos_mcb_t *)(vm.mem + ((uint32_t)vm.first_mcb << 4));
    root->type = 'X';
    maximum = 0xFFFF;
    if (dos_mem_alloc(&vm, 1, &maximum) != 0 || maximum != 0)
        failures++;
    root->type = 'Z';
    if (dos_mcb_validate(&vm) < 0 || dos_mem_free(&vm, 0) == 0)
        failures++;

    dos_host_free_pages(memory, pages);
    return failures + dos_transfer_selftest();
}
