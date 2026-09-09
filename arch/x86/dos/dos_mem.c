/*
 * OsitoK DOS Memory Control Block (MCB) manager.
 *
 * Implements the conventional-memory arena used by INT 21h functions
 * 48h, 49h, and 4Ah. Every mutation first validates the complete chain so
 * a damaged guest MCB cannot make the kernel walk outside emulated memory.
 */

#include "cpu8086.h"
#include "dos_hostmem.h"
#include "dos_mem.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

#ifndef DOS_DIAGNOSTICS
#define DOS_DIAGNOSTICS 0
#endif

static uint16_t dos_mcb_limit(const dos_vm_t *vm)
{
    if (!vm || !vm->mem) return 0;
    uint32_t limit = vm->total_mem_size;
    if (limit > DOS_CONV_TOP) limit = DOS_CONV_TOP;
    return (uint16_t)(limit >> 4);
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
    if (!limit || segment < vm->first_mcb || segment >= limit)
        return -1;

    uint32_t address = (uint32_t)segment << 4;
    if (address > vm->total_mem_size ||
        sizeof(dos_mcb_t) > vm->total_mem_size - address)
        return -1;

    dos_mcb_t *mcb = (dos_mcb_t *)(vm->mem + address);
    if (mcb->type != 'M' && mcb->type != 'Z') return -1;

    uint32_t next = (uint32_t)segment + 1u + mcb->size;
    if (next > limit || (mcb->type == 'M' && next >= limit))
        return -1;

    if (out) *out = mcb;
    if (next_segment) *next_segment = (uint16_t)next;
    return 0;
}

static int dos_mcb_validate(dos_vm_t *vm)
{
    uint16_t limit = dos_mcb_limit(vm);
    uint16_t segment = vm ? vm->first_mcb : 0;
    if (!segment || segment >= limit) return -1;
    uint32_t max_blocks = (uint32_t)limit - segment;

    for (uint32_t count = 0; count <= max_blocks; count++) {
        dos_mcb_t *mcb;
        uint16_t next;
        if (dos_mcb_decode(vm, segment, &mcb, &next) < 0) return -1;
        if (mcb->type == 'Z') return next == limit ? 0 : -1;
        segment = next;
    }
    return -1;
}

static int dos_mcb_find_validated(dos_vm_t *vm, uint16_t target,
                                  dos_mcb_t **out, uint16_t *previous)
{
    uint16_t segment = vm->first_mcb;
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
    vm->allocation_strategy = DOS_ALLOC_FIRST_FIT;
    vm->uppermem_link = 0;
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

uint16_t dos_mem_alloc(dos_vm_t *vm, uint16_t paragraphs, uint16_t *largest)
{
    return dos_mem_alloc_owned(vm, paragraphs, largest, vm ? vm->current_psp : 0);
}

uint16_t dos_mem_alloc_owned(dos_vm_t *vm, uint16_t paragraphs,
                             uint16_t *largest, uint16_t owner)
{
    if (largest) *largest = 0;
    if (dos_mcb_validate(vm) < 0) return 0;

    uint16_t segment = vm->first_mcb;
    uint16_t best_free = 0;
    dos_mcb_t *selected = NULL;
    uint16_t selected_segment = 0;
    uint8_t fit = vm->allocation_strategy & DOS_ALLOC_FIT_MASK;
    if (fit > DOS_ALLOC_LAST_FIT) fit = DOS_ALLOC_FIRST_FIT;

    for (;;) {
        dos_mcb_t *mcb;
        uint16_t next;
        if (dos_mcb_decode(vm, segment, &mcb, &next) < 0) return 0;

        if (mcb->owner == 0) {
            if (dos_mcb_coalesce_forward(vm, segment, mcb) < 0) return 0;
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

    if (!selected) {
        if (largest) *largest = best_free;
        return 0;
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
    return (uint16_t)(selected_segment + 1u);
}

int dos_mem_free(dos_vm_t *vm, uint16_t segment)
{
    if (!segment) return DOS_MEM_ERR_BLOCK;
    if (dos_mcb_validate(vm) < 0) return DOS_MEM_ERR_ARENA;

    uint16_t mcb_segment = (uint16_t)(segment - 1u);
    uint16_t previous = 0;
    dos_mcb_t *mcb;
    if (dos_mcb_find_validated(vm, mcb_segment, &mcb, &previous) < 0 ||
        mcb->owner == 0)
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
        mcb->owner == 0)
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

    uint32_t run = 0;
    uint32_t best = 0;
    uint16_t segment = vm->first_mcb;
    for (;;) {
        dos_mcb_t *mcb;
        uint16_t next;
        if (dos_mcb_decode(vm, segment, &mcb, &next) < 0)
            return DOS_MEM_ERR_ARENA;

        if (mcb->owner == 0) {
            if (run) run++;
            run += mcb->size;
            if (run > best) best = run;
        } else {
            run = 0;
        }

        if (mcb->type == 'Z') break;
        segment = next;
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
    uint16_t segment = vm->first_mcb;
    for (;;) {
        dos_mcb_t *mcb;
        uint16_t next;
        if (dos_mcb_decode(vm, segment, &mcb, &next) < 0) return -1;
        uint8_t type = mcb->type;
        if (mcb->owner == owner) dos_mcb_clear(mcb);
        if (type == 'Z') break;
        segment = next;
    }

    segment = vm->first_mcb;
    for (;;) {
        dos_mcb_t *mcb;
        if (dos_mcb_decode(vm, segment, &mcb, NULL) < 0) return -1;
        if (mcb->owner == 0 && dos_mcb_coalesce_forward(vm, segment, mcb) < 0)
            return -1;
        if (mcb->type == 'Z') break;
        segment = (uint16_t)(segment + 1u + mcb->size);
    }
    return dos_mcb_validate(vm);
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
    return failures;
}
