/* Actual window selection, using synthetic PTE reads; no privileged calls. */
#include "../include/types.h"
extern int printf(const char *, ...);
extern void abort(void);
static uint64_t test_window_read(volatile void *);
#define mmio_read64 test_window_read
#define INTEL_BCS_DIAGNOSTIC 1
#include "../drivers/intel_gfx_memory.c"
static uint64_t table[65536];
static unsigned checks, reads;
static uint32_t change_page;
static bool executor_idle=true;
bool intel_gpu_executor_quiescent(void) { return executor_idle; }
/* Clang's function sanitizer retains the unused clock callback. It must
 * never execute in this selector-only host test. */
uint64_t idt_get_monotonic_ns(void) { abort(); return 0; }
/* Sanitizer instrumentation may retain the diagnostic allocator vtable.
 * Selector-only tests must never allocate or access real page tables. */
void *mem_alloc_pages(uint64_t count) { (void)count; abort(); return 0; }
void mem_free_pages(void *p, uint64_t count) { (void)p; (void)count; abort(); }
uint64_t paging_get_kernel_cr3(void) { abort(); return 0; }
uint64_t paging_translate_in_cr3(uint64_t cr3, uint64_t va)
{ (void)cr3; (void)va; abort(); return 0; }
int paging_query_mapping_in_cr3(uint64_t cr3, uint64_t va, uint64_t *flags, uint64_t *size)
{ (void)cr3; (void)va; (void)flags; (void)size; abort(); return -1; }
#define CHECK(x) do { checks++; if (!(x)) { \
    printf("FAIL GGTT window %d: %s\n", __LINE__, #x); abort(); \
} } while (0)
static uint64_t test_window_read(volatile void *address)
{
    uint64_t offset = (uint64_t)address - (uint64_t)native_memory.mmio - INTEL_GGTT_MMIO_OFFSET;
    CHECK(!(offset & 7) && offset / 8 < 65536);
    uint32_t page = (uint32_t)(offset / 8);
    reads++;
    uint64_t value = table[page];
    if (page == change_page) table[page] ^= 0x1000;
    return value;
}
int main(void)
{
    CHECK(intel_gfx_kexec_ready());
    executor_idle=false;
    CHECK(!intel_gfx_kexec_ready());
    executor_idle=true;
    bcs_memory.engine.referenced = true;
    CHECK(!intel_gfx_kexec_ready());
    bcs_memory.engine.completed = true;
    CHECK(!intel_gfx_kexec_ready()); /* a completed token is not retirement */
    bcs_memory.engine.retired = true;
    CHECK(intel_gfx_kexec_ready());
    bcs_memory.engine.retired = false;
    bcs_memory.engine.referenced = false;
    CHECK(intel_gfx_kexec_ready());
    rcs_memory.engine.referenced=true;
    CHECK(!intel_gfx_kexec_ready());
    rcs_memory.engine.completed=true;
    CHECK(!intel_gfx_kexec_ready());
    rcs_memory.engine.retired=true;
    rcs_memory.ram.allocated_pages=33;
    CHECK(!intel_gfx_kexec_ready()); /* retired engine still has owned mappings/RAM */
    rcs_memory.ram.freed_pages=33;
    CHECK(intel_gfx_kexec_ready());
    rcs_memory.cache.changed=1;
    CHECK(!intel_gfx_kexec_ready());
    rcs_memory.cache.changed=0; rcs_memory.pat.changed=true;
    CHECK(!intel_gfx_kexec_ready());
    rcs_memory.pat.changed=false;
    CHECK(intel_gfx_kexec_ready());
    intel_gfx_state_t d = {0};
    d.memory.aperture_bytes = 0x10000000;
    d.memory.gpu_va_bytes = 0x100000000ULL;
    d.memory.ggtt_bytes = 0x800000;
    d.gop_pitch_bytes = 7680; d.gop_height = 1080;
    native_memory.device = &d;
    /* Fake MMIO base is only converted to an offset, never dereferenced. */
    native_memory.mmio = (volatile uint8_t *)0x10000000ULL;
    change_page = UINT32_MAX;
    CHECK(ggtt_test_find_window());
    CHECK(native_memory.first == 0xfc00 && reads == 1024);
    /* One unknown entry excludes that complete CPU 2 MiB window. */
    table[0xfc01] = UINT64_MAX; reads = 0;
    CHECK(ggtt_test_find_window()); CHECK(native_memory.first == 0xfa00);
    table[0xfc01] = 0; change_page = 0xfc02;
    CHECK(ggtt_test_find_window()); CHECK(native_memory.first == 0xfa00);
    change_page = UINT32_MAX;
    for (unsigned i = 0; i < 65536; i++) table[i] = 0x12345001;
    native_memory.first = 99;
    CHECK(!ggtt_test_find_window()); CHECK(native_memory.first == 99);
    /* Present firmware entries survive; a single hole may be used. */
    table[0xf600] = 0x1000;
    CHECK(ggtt_test_find_window()); CHECK(native_memory.first == 0xf600);
    CHECK(table[0xf600] == 0x1000 && table[0xf601] == 0x12345001);
    d.gop_ggtt_offset = 0xf600000;
    native_memory.first = 99;
    CHECK(!ggtt_test_find_window()); CHECK(native_memory.first == 99);
    /* Low addresses remain reserved even when every high window is used. */
    d.gop_ggtt_offset = 0; table[0xf600] = 1; table[0x2000] = 0;
    CHECK(!ggtt_test_find_window());
    printf("GGTT window: %u checks passed\n", checks);
    return 0;
}
