/*
 * arch/x86/kernel/tensor_arena.c — superpage-backed tensor allocator
 *
 * Uses existing mem_alloc_aligned_high() to get 2 MB-aligned physical
 * memory. The kernel's upper-half direct map (installed by paging_init)
 * already maps aligned RAM with PTE_LARGE (2 MB pages), so we just
 * expose the KERNEL_VBASE-offset view of the reservation.
 */

#include "../include/tensor_arena.h"
#include "../include/paging.h"

extern void *mem_alloc_aligned_high(uint64_t size, uint64_t alignment);
extern void  serial_puts(const char *s);
extern void  serial_putdec(uint64_t v);
extern void  serial_puthex(uint64_t v, int d);

tensor_arena_t g_tensor_arena;

int tensor_arena_init(tensor_arena_t *a, uint64_t size_mb)
{
    /* Round up to 2 MB multiple */
    uint64_t bytes = size_mb * 1024 * 1024;
    bytes = (bytes + TENSOR_SUPERPAGE_SIZE - 1) & ~(TENSOR_SUPERPAGE_SIZE - 1);

    /* Allocate 2 MB-aligned physically-contiguous region. mem_alloc_aligned_high
     * returns a physical address that is safely above the ELF load zone. */
    void *phys = mem_alloc_aligned_high(bytes, TENSOR_SUPERPAGE_SIZE);
    if (!phys) {
        serial_puts("[TENSOR-ARENA] FAIL: could not reserve ");
        serial_putdec(bytes / (1024 * 1024));
        serial_puts(" MB of 2MB-aligned memory\n");
        return -1;
    }

    a->phys_base  = (uint64_t)phys;
    a->virt_base  = PHYS_TO_VIRT(phys);
    a->total_size = bytes;
    a->used       = 0;
    a->peak_used  = 0;
    a->superpages = (uint32_t)(bytes / TENSOR_SUPERPAGE_SIZE);

    serial_puts("[TENSOR-ARENA] ");
    serial_putdec(bytes / (1024 * 1024));
    serial_puts(" MB on ");
    serial_putdec(a->superpages);
    serial_puts(" superpages: phys=0x");
    serial_puthex(a->phys_base, 12);
    serial_puts(" virt=0x");
    serial_puthex((uint64_t)a->virt_base, 16);
    serial_puts("\n");
    return 0;
}

void *tensor_arena_alloc(tensor_arena_t *a, uint64_t size, uint64_t align)
{
    if (!a->virt_base) return 0;
    if (align == 0) align = 64;
    uint64_t aligned = (a->used + align - 1) & ~(align - 1);
    if (aligned + size > a->total_size) {
        serial_puts("[TENSOR-ARENA] OOM: need ");
        serial_putdec(size);
        serial_puts(" B, have ");
        serial_putdec(a->total_size - aligned);
        serial_puts(" B free\n");
        return 0;
    }
    void *p = (char *)a->virt_base + aligned;
    a->used = aligned + size;
    if (a->used > a->peak_used) a->peak_used = (uint32_t)a->used;
    return p;
}

void tensor_arena_reset(tensor_arena_t *a)
{
    a->used = 0;
}

void tensor_arena_stats(const tensor_arena_t *a, const char *name)
{
    serial_puts("[TENSOR-ARENA] ");
    if (name) { serial_puts(name); serial_puts(": "); }
    serial_putdec(a->used / 1024);
    serial_puts(" KB used / ");
    serial_putdec(a->total_size / 1024);
    serial_puts(" KB total (peak ");
    serial_putdec(a->peak_used / 1024);
    serial_puts(" KB) ");
    serial_putdec(a->superpages);
    serial_puts(" superpages\n");
}
