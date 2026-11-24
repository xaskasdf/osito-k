/* Prepare without touching live/final kernel memory; commit through a private
 * stack, GDT, IDT and CR3. All allocations come from the physical allocator. */
#include "kexec.h"
#include "kexec_image.h"
#include "../include/paging.h"
#include "../include/pci.h"
#include "../drivers/iwx.h"
#include "../drivers/hda.h"
#include "../drivers/intel_gfx.h"
#include "../drivers/intel_gfx_driver.h"
#include "smp.h"

extern boot_info_t saved_boot_info;
extern void *kmalloc(uint64_t);
extern void kfree(void *);
extern void *mem_alloc_aligned_high_below4g(uint64_t, uint64_t);
extern void mem_free_pages(void *, uint64_t);
extern uint64_t mem_get_highest_address(void);
extern void *osfs2_find(const char *);
extern uint64_t osfs2_file_size(void *);
extern int osfs2_read(void *, uint64_t, void *, uint64_t);
extern int disk_flush(void);
extern void sh_puts(const char *);
extern void serial_puts(const char *);
extern uint64_t idt_get_monotonic_ns(void);
extern volatile uint32_t *idt_get_apic_base(void);
extern unsigned char kexec_trampoline[], kexec_trampoline_end[], kexec_trampoline_fault[];

#define PAGE 4096ULL
#define MAX_ALLOCS 1024
#define STACK_BYTES (1024 * 1024ULL)
#define PT_MASK 0x000ffffffffff000ULL
#define MAP_LIMIT (1ULL << 39)

typedef struct { uint64_t source, destination, filesz, memsz; } copy_segment_t;
typedef struct __attribute__((packed)) { uint16_t limit; uint64_t base; } descriptor_t;
typedef struct {
    uint64_t cr3, stack, entry, info, segments, count;
    descriptor_t gdt, idt;
    boot_info_t boot;
    copy_segment_t copy[KEXEC_MAX_SEGMENTS];
} handoff_t;
_Static_assert(__builtin_offsetof(handoff_t, gdt) == 48, "trampoline GDTR ABI");
_Static_assert(__builtin_offsetof(handoff_t, idt) == 58, "trampoline IDTR ABI");
_Static_assert(sizeof(handoff_t) < PAGE, "handoff header page");

typedef struct {
    kexec_image_t image;
    struct { uint64_t address, bytes; } allocation[MAX_ALLOCS];
    unsigned count;
    uint64_t data, data_size, stack, root, pdpt;
    const char *error;
} load_state_t;

static void print_decimal(uint64_t number)
{
    char text[24]; unsigned i = sizeof(text) - 1;
    text[i] = 0;
    do { text[--i] = '0' + number % 10; number /= 10; } while (number);
    sh_puts(text + i);
}

static void *allocate(load_state_t *s, uint64_t bytes)
{
    if (!bytes || bytes > UINT64_MAX - PAGE + 1 || s->count == MAX_ALLOCS) return NULL;
    bytes = (bytes + PAGE - 1) & ~(PAGE - 1);
    void *physical = mem_alloc_aligned_high_below4g(bytes, PAGE);
    uint64_t address = (uint64_t)physical;
    if (!physical) return NULL;
    if (kexec_overlap(address, bytes, s->image.low, s->image.high - s->image.low)) {
        mem_free_pages(physical, bytes / PAGE);
        s->error = "allocated staging overlaps destination; no safe allocation";
        return NULL;
    }
    s->allocation[s->count].address = address;
    s->allocation[s->count++].bytes = bytes;
    void *v = PHYS_TO_VIRT(address);
    memset(v, 0, bytes);
    return v;
}

static bool map_range(load_state_t *s, uint64_t base, uint64_t bytes, uint64_t flags)
{
    if (!bytes || base >= MAP_LIMIT || bytes > MAP_LIMIT - base) return false;
    uint64_t end = (base + bytes + 0x1fffff) & ~0x1fffffULL;
    base &= ~0x1fffffULL;
    uint64_t *pdpt = PHYS_TO_VIRT(s->pdpt);
    for (; base < end; base += 0x200000) {
        uint64_t index = base >> 30;
        if (!(pdpt[index] & 1)) {
            void *page = allocate(s, PAGE);
            if (!page) return false;
            pdpt[index] = kvirt_to_phys(page) | 3;
        }
        uint64_t *pd = PHYS_TO_VIRT(pdpt[index] & PT_MASK);
        pd[(base >> 21) & 511] = base | 0x83 | flags;
    }
    return true;
}

static bool prepare(load_state_t *s, const uint8_t *elf)
{
    const boot_info_t *b = &saved_boot_info;
    if (!b->mmap_addr || !b->mmap_size || b->mmap_size > 65536 ||
        b->mmap_desc_size < sizeof(kexec_memory_descriptor_t) || b->mmap_size % b->mmap_desc_size) {
        s->error = "invalid saved UEFI memory map"; return false;
    }
    for (unsigned i = 0; i < s->image.count; i++) {
        kexec_load_t *l = &s->image.load[i];
        if (!kexec_destination_is_ram(b, l->physical, l->memsz)) {
            s->error = "kernel destination includes non-RAM or firmware-reserved memory"; return false;
        }
    }
    uint64_t map_bytes = (b->mmap_size + PAGE - 1) & ~(PAGE - 1);
    uint64_t payload = 4 * PAGE + map_bytes;
    s->data_size = payload + s->image.staging_bytes;
    handoff_t *h = allocate(s, s->data_size);
    void *stack = allocate(s, STACK_BYTES);
    uint64_t *root = allocate(s, PAGE), *pdpt = allocate(s, PAGE);
    if (!h || !stack || !root || !pdpt) return false;
    s->data = kvirt_to_phys(h); s->stack = kvirt_to_phys(stack);
    s->root = kvirt_to_phys(root); s->pdpt = kvirt_to_phys(pdpt);
    root[0] = root[256] = s->pdpt | 3;
    uint64_t top = mem_get_highest_address();
    if (top < KEXEC_PHYS_LIMIT) top = KEXEC_PHYS_LIMIT;
    if (!map_range(s, 0, top, 0)) return false;
    volatile uint32_t *apic = idt_get_apic_base();
    if (apic && !map_range(s, kvirt_to_phys((const void *)apic), PAGE, 0x18)) return false;
    if (b->fb_base && b->fb_height && b->fb_pitch) {
        uint64_t row = (uint64_t)b->fb_pitch * 4;
        if (b->fb_base >= MAP_LIMIT || b->fb_height > (MAP_LIMIT - b->fb_base) / row ||
            !map_range(s, b->fb_base, row * b->fb_height, 0x18)) {
            s->error = "cannot preserve GOP aperture"; return false;
        }
    }
    h->cr3 = s->root; h->stack = s->stack + STACK_BYTES;
    h->entry = s->image.entry;
    h->info = s->data + __builtin_offsetof(handoff_t, boot);
    h->segments = s->data + __builtin_offsetof(handoff_t, copy);
    h->count = s->image.count;
    h->boot = *b;
    h->boot.version = BOOT_INFO_VERSION;
    h->boot.kernel_phys_base = s->image.low;
    h->boot.kernel_size = s->image.high - s->image.low;
    h->boot.handoff_data_base = s->data;
    h->boot.handoff_data_size = s->data_size;
    h->boot.handoff_stack_base = s->stack;
    h->boot.handoff_stack_size = STACK_BYTES;
    h->boot.mmap_addr = s->data + 4 * PAGE;
    memcpy(PHYS_TO_VIRT(h->boot.mmap_addr), (const void *)(uintptr_t)b->mmap_addr, b->mmap_size);
    descriptor_t old_gdt;
    uint16_t cs;
    __asm__ volatile ("sgdt %0; mov %%cs,%1" : "=m"(old_gdt), "=r"(cs));
    if ((uint64_t)old_gdt.limit + 1 > PAGE || (cs & 3)) {
        s->error = "unsupported entry GDT"; return false;
    }
    h->gdt.limit = old_gdt.limit; h->gdt.base = s->data + PAGE;
    memcpy(PHYS_TO_VIRT(h->gdt.base), (const void *)old_gdt.base, old_gdt.limit + 1);
    uint64_t tramp_size = (uint64_t)(kexec_trampoline_end - kexec_trampoline);
    if (!tramp_size || tramp_size > PAGE) { s->error = "trampoline exceeds page"; return false; }
    memcpy(PHYS_TO_VIRT(s->data + 3 * PAGE), kexec_trampoline, tramp_size);
    uint64_t handler = s->data + 3 * PAGE + (kexec_trampoline_fault - kexec_trampoline);
    uint64_t *idt = PHYS_TO_VIRT(s->data + 2 * PAGE);
    for (unsigned i = 0; i < 256; i++) {
        idt[2 * i] = (handler & 0xffff) | ((uint64_t)cs << 16) |
                     (0x8eULL << 40) | (((handler >> 16) & 0xffff) << 48);
        idt[2 * i + 1] = handler >> 32;
    }
    h->idt.limit = PAGE - 1; h->idt.base = s->data + 2 * PAGE;
    for (unsigned i = 0; i < s->image.count; i++) {
        kexec_load_t *l = &s->image.load[i];
        h->copy[i] = (copy_segment_t){s->data + payload, l->physical, l->filesz, l->memsz};
        memcpy(PHYS_TO_VIRT(s->data + payload), elf + l->offset, l->filesz);
        payload += (l->filesz + PAGE - 1) & ~(PAGE - 1);
        sh_puts("  staged segment "); print_decimal(i + 1);
        sh_puts(": "); print_decimal(l->filesz); sh_puts(" file bytes, ");
        print_decimal(l->memsz - l->filesz); sh_puts(" zero-fill bytes\n");
    }
    return true;
}

static bool wait_icr(volatile uint32_t *apic)
{
    for (unsigned i = 0; i < 1000000; i++) {
        if (!(apic[0x300 / 4] & (1 << 12))) return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static void commit(load_state_t *s) __attribute__((noreturn));
static void commit(load_state_t *s)
{
    uint64_t data = s->data;
    volatile uint32_t *apic = idt_get_apic_base();
    __asm__ volatile ("cli" ::: "memory");
    /* Peers must stop before any final destination is overwritten. */
    if (apic) {
        apic[0x320 / 4] = 1 << 16;
        if (smp_cpu_count() > 1) {
            if (!wait_icr(apic)) goto failed;
            apic[0x310 / 4] = 0;
            apic[0x300 / 4] = 0x000cc500; /* INIT level/assert, all excluding self */
            if (!wait_icr(apic)) goto failed;
            uint64_t end = idt_get_monotonic_ns() + 10000000;
            while (idt_get_monotonic_ns() < end) __asm__ volatile ("pause");
            apic[0x300 / 4] = 0x000c8500; /* INIT deassert */
            if (!wait_icr(apic)) goto failed;
        }
    }
    if (!pci_kexec_quiesce()) goto failed;
    if (!hda_kexec_quiesce()) goto failed;
    void (*tramp)(void *) = (void (*)(void *))PHYS_TO_VIRT(data + 3 * PAGE);
    tramp(PHYS_TO_VIRT(data));
failed:
    sh_puts("kexec: handoff could not quiesce hardware; reboot required\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

void kexec_command(const char *arg)
{
    const char *name = arg && *arg ? arg : "kernel.elf";
    load_state_t *s = NULL;
    uint8_t *image = NULL;
    const char *error = NULL;
    bool wlan = false;
    if (smp_current_cpu() != 0) { error = "must run on BSP"; goto out; }
    intel_gfx_driver_snapshot_t gpu;
    if (intel_gfx_driver_snapshot(&gpu) != 0 || gpu.phase != INTEL_GFX_DRIVER_OFF) {
        error = "native GPU driver still owns resources; use gpu-native off"; goto out;
    }
    if (!intel_gfx_kexec_ready()) {
        error = "Intel GPU DMA retirement unconfirmed; reboot required"; goto out;
    }
    sh_puts("kexec: loading "); sh_puts(name); sh_puts(" from OsitoFS...\n");
    void *file = osfs2_find(name);
    if (!file) { error = "File not found"; goto out; }
    uint64_t size = osfs2_file_size(file);
    if (size < sizeof(kexec_elf_header_t) || size > 0x7fffffff) { error = "invalid file size"; goto out; }
    image = kmalloc(size);
    s = kmalloc(sizeof(*s));
    if (s) memset(s, 0, sizeof(*s));
    if (!image || !s) { error = "out of memory"; goto out; }
    if (osfs2_read(file, 0, image, size) < 0) { error = "read failed"; goto out; }
    error = kexec_parse_image(image, size, &s->image);
    if (error) goto out;
    if (!prepare(s, image)) { error = s->error ? s->error : "cannot allocate safe staging/page tables"; goto out; }
    kfree(image); image = NULL;
    if (disk_flush() < 0) { error = "filesystem flush failed"; goto out; }
    if (iwx_handoff_begin() != 0) { error = "WLAN busy or DMA shutdown unconfirmed; try wlan stop"; goto out; }
    wlan = true;
    sh_puts("kexec: staged without overwriting live memory; jumping to new kernel\n");
    commit(s);
out:
    if (wlan) iwx_handoff_end();
    if (s) {
        for (unsigned i = 0; i < s->count; i++)
            mem_free_pages((void *)s->allocation[i].address, s->allocation[i].bytes / PAGE);
        kfree(s);
    }
    if (image) kfree(image);
    if (error) { sh_puts("kexec: "); sh_puts(error); sh_puts("\n"); }
}

void kexec_boot_reserve(const boot_info_t *b, uint64_t rsp)
{
    extern void mem_reserve_kernel(uint64_t, uint64_t);
    extern bool mem_reserve_boot_stack_region(uint64_t, uint64_t);
    uint64_t data = b->handoff_data_base, dn = b->handoff_data_size;
    uint64_t stack = b->handoff_stack_base, sn = b->handoff_stack_size;
    bool valid = dn && sn && !((data | dn | stack | sn) & (PAGE - 1)) &&
        sn >= 65536 && rsp >= stack && rsp - stack < sn &&
        kexec_destination_is_ram(b, data, dn) && kexec_destination_is_ram(b, stack, sn) &&
        !kexec_overlap(data, dn, stack, sn) &&
        !kexec_overlap(data, dn, b->kernel_phys_base, b->kernel_size) &&
        !kexec_overlap(stack, sn, b->kernel_phys_base, b->kernel_size);
    if (!valid || !mem_reserve_boot_stack_region(stack, sn)) {
        serial_puts("[KEXEC] invalid handoff memory ownership; halted\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    /* EFI LoaderData reclaimed by the old kernel can back this allocation.
     * mem_init marks it reserved already; ownership validation above suffices. */
    mem_reserve_kernel(data, dn);
    serial_puts("[KEXEC] reserved transient data and entry stack\n");
}

void kexec_boot_release(boot_info_t *b)
{
    if (!b->handoff_data_size) return;
    /* main now uses its BSS copy; IDT/GDT and CR3 have been replaced. */
    mem_free_pages((void *)b->handoff_data_base, b->handoff_data_size / PAGE);
    b->handoff_data_base = b->handoff_data_size = 0;
    serial_puts("[KEXEC] released transient staging; entry stack retained\n");
}
