/* Do not interpose freestanding libc shims into the host sanitizer runtime. */
#define strncmp kexec_test_strncmp
#define memmove kexec_test_memmove
#include "../kernel/memory.c"

extern int printf(const char *, ...);
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
void serial_puts(const char *s) { (void)s; }
void serial_puthex(uint64_t n, int digits) { (void)n; (void)digits; }
void serial_putdec(uint64_t n) { (void)n; }
void fb_puts(const char *s) { (void)s; }
void fb_putdec(uint64_t n) { (void)n; }

int main(void)
{
    efi_memory_descriptor_t map[] = {
        {.type = 7, .physical_start = 0x100000, .number_of_pages = 256},
        {.type = 2, .physical_start = 0x200000, .number_of_pages = 256}
    };
    mem_init(map, sizeof(map), sizeof(map[0]));
    CHECK(free_pages == 256 && total_pages == 256);
    CHECK(mem_reserve_boot_stack_region(0x200000, 0x10000));
    CHECK(free_pages == 256 && !bitmap_test(0x200000 / PAGE_SIZE));
    CHECK(identity_boot_stack_base == 0x200000 && identity_boot_stack_limit == 0x210000);
    CHECK(mem_reserve_boot_stack_region(0x100000, 0x10000));
    CHECK(free_pages == 240 && !bitmap_test(0x100000 / PAGE_SIZE));
    CHECK(mem_reserve_boot_stack_region(0x100000, 0x10000) && free_pages == 240);
    CHECK(mem_identity_reservation_conflict_end(0x108000, 0x1000) == 0x110000);
    CHECK(!mem_identity_reservation_conflict_end(0x110000, 0x1000));
    CHECK(!mem_reserve_boot_stack_region(0x100001, 0x10000));
    CHECK(!mem_reserve_boot_stack_region(0x100000, 0x10001));
    CHECK(!mem_reserve_boot_stack_region(0x100000, 0));
    CHECK(!mem_reserve_boot_stack_region(MAX_PHYS_PAGES * PAGE_SIZE, PAGE_SIZE));
    CHECK(!mem_reserve_boot_stack_region(0x100000, UINT64_MAX - 4095));
    CHECK(identity_boot_stack_base == 0x100000 && identity_boot_stack_limit == 0x110000);
    mem_reserve_kernel(0x210000, PAGE_SIZE); /* Already-reserved EFI LoaderData. */
    CHECK(free_pages == 240);
    mem_reserve_kernel(0x110000, PAGE_SIZE);
    CHECK(free_pages == 239 && !bitmap_test(0x110000 / PAGE_SIZE));
    printf("kexec boot reservations: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
