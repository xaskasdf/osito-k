#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../boot/boot_efi.c"

EFI_BOOT_SERVICES *BS;
EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES services;
static void *pages[128];
static unsigned page_count, page_fail_at, pool_count, exit_calls, map_calls;
static UINTN generation, required;
static EFI_STATUS map_error, exit_error;
static BOOLEAN fresh_map, exited, retry_once, grow_on_retry, map_fails_on_retry;

UINTN Print(const CHAR16 *fmt, ...)
{
    (void)fmt;
    assert(!exit_calls);
    return 0;
}

static EFI_STATUS EFIAPI allocate_pages(EFI_ALLOCATE_TYPE type, EFI_MEMORY_TYPE memory,
                                        UINTN count, EFI_PHYSICAL_ADDRESS *address)
{
    assert(!exited && type == AllocateMaxAddress && memory == EfiLoaderData);
    assert(count == 1 && *address == 0xffffffffULL);
    if (page_count == page_fail_at)
        return EFI_OUT_OF_RESOURCES;
    assert(page_count < sizeof(pages) / sizeof(pages[0]));
    void *page = aligned_alloc(4096, 4096);
    assert(page);
    memset(page, 0xa5, 4096);
    pages[page_count++] = page;
    *address = (UINT64)page;
    generation++;
    fresh_map = FALSE;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI allocate_pool(EFI_MEMORY_TYPE type, UINTN size, void **buffer)
{
    assert(!exited && type == EfiLoaderData);
    *buffer = malloc(size);
    assert(*buffer);
    pool_count++;
    generation++;
    fresh_map = FALSE;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI free_pool(void *buffer)
{
    assert(!exited && buffer && pool_count);
    free(buffer);
    pool_count--;
    generation++;
    fresh_map = FALSE;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI memory_map(UINTN *size, EFI_MEMORY_DESCRIPTOR *buffer,
                                   UINTN *key, UINTN *stride, UINT32 *version)
{
    assert(!exited);
    map_calls++;
    if (EFI_ERROR(map_error))
        return map_error;
    *stride = 48;
    *version = 1;
    if (*size < required) {
        *size = required;
        return EFI_BUFFER_TOO_SMALL;
    }
    assert(buffer);
    memset(buffer, 0, required);
    buffer->Type = EfiConventionalMemory;
    buffer->PhysicalStart = 0x100000;
    buffer->NumberOfPages = 1024;
    *size = required;
    *key = generation;
    fresh_map = TRUE;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI exit_services(EFI_HANDLE image, UINTN key)
{
    assert(!exited && image == (EFI_HANDLE)&services);
    assert(fresh_map && key == generation);
    fresh_map = FALSE;
    exit_calls++;
    if ((retry_once && exit_calls == 1) || exit_error == EFI_INVALID_PARAMETER) {
        generation++;
        if (grow_on_retry)
            required = 48 * 40;
        if (map_fails_on_retry)
            map_error = EFI_DEVICE_ERROR;
        return EFI_INVALID_PARAMETER;
    }
    if (EFI_ERROR(exit_error))
        return exit_error;
    exited = TRUE;
    return EFI_SUCCESS;
}

static void reset(void)
{
    for (unsigned i = 0; i < page_count; i++)
        free(pages[i]);
    if (mmap_buf) {
        free(mmap_buf);
        pool_count--;
    }
    assert(!pool_count);
    mmap_buf = NULL;
    mmap_capacity = mmap_size = mmap_key = mmap_desc_size = 0;
    page_count = exit_calls = map_calls = 0;
    page_fail_at = 128;
    required = 96;
    generation = 1;
    map_error = exit_error = EFI_SUCCESS;
    fresh_map = exited = retry_once = grow_on_retry = map_fails_on_retry = FALSE;
    memset(&services, 0, sizeof(services));
    BS = &services;
    BS->AllocatePages = allocate_pages;
    BS->AllocatePool = allocate_pool;
    BS->FreePool = free_pool;
    BS->GetMemoryMap = memory_map;
    BS->ExitBootServices = exit_services;
}

static UINT64 walk(UINT64 root, UINT64 phys, UINT64 *flags)
{
    UINT64 entry = ((UINT64 *)root)[256];
    assert(entry & 1);
    entry = ((UINT64 *)(entry & BOOT_PT_MASK))[phys >> 30];
    if (!(entry & 1))
        return ~(UINT64)0;
    assert(!(entry & BOOT_PTE_LARGE));
    entry = ((UINT64 *)(entry & BOOT_PT_MASK))[(phys >> 21) & 511];
    if (!(entry & 1))
        return ~(UINT64)0;
    if (entry & BOOT_PTE_LARGE) {
        *flags = entry & (BOOT_2M - 1);
        return (entry & ~(BOOT_2M - 1)) | (phys & (BOOT_2M - 1));
    }
    entry = ((UINT64 *)(entry & BOOT_PT_MASK))[(phys >> 12) & 511];
    if (!(entry & 1))
        return ~(UINT64)0;
    *flags = entry & 4095;
    return (entry & BOOT_PT_MASK) | (phys & 4095);
}

static void test_high_framebuffer(void)
{
    reset();
    UINT64 firmware[512] = {0};
    firmware[0] = 0x12345003;
    firmware[255] = 0x23456003;
    EFI_MEMORY_DESCRIPTOR map[3] = {
        {.Type = EfiConventionalMemory, .PhysicalStart = 0, .NumberOfPages = (2ULL << 30) / 4096},
        {.Type = EfiConventionalMemory, .PhysicalStart = 4ULL << 30, .NumberOfPages = (30ULL << 30) / 4096},
        {.Type = EfiMemoryMappedIO, .PhysicalStart = 128ULL << 30, .NumberOfPages = 1024}
    };
    UINT64 root = 0, fb = 0x4000000000ULL, bytes = 1920 * 1080 * 4, flags;
    assert(boot_prepare_paging(firmware, map, sizeof(map), sizeof(map[0]),
                               fb, bytes, 0xfee00000, &root) == EFI_SUCCESS);
    assert(((UINT64 *)root)[0] == firmware[0] && ((UINT64 *)root)[255] == firmware[255]);
    assert(!firmware[256]);
    assert(page_count < 40);
    assert(walk(root, 32ULL << 30, &flags) == (32ULL << 30));
    assert(walk(root, 64ULL << 30, &flags) == ~(UINT64)0);
    assert(walk(root, 128ULL << 30, &flags) == ~(UINT64)0);
    assert(walk(root, 0xfee00000, &flags) == 0xfee00000);
    assert((flags & BOOT_PTE_UC) == BOOT_PTE_UC);
    assert(walk(root, 0xfee01000, &flags) == ~(UINT64)0);
    for (UINT64 at = fb; at < fb + bytes; at += 4096) {
        assert(walk(root, at, &flags) == at);
        assert((flags & BOOT_PTE_UC) == BOOT_PTE_UC);
    }
    assert(walk(root, fb + bytes - 1, &flags) == fb + bytes - 1);
    assert(walk(root, fb + ((bytes + 4095) & ~4095ULL), &flags) == ~(UINT64)0);
    puts("PASS sparse 32-GiB RAM and complete framebuffer at 256 GiB");
}

static void test_mapping_edges(void)
{
    reset();
    UINT64 *pdpt;
    assert(boot_alloc_table(&pdpt) == EFI_SUCCESS);
    assert(boot_map_range(pdpt, 0, BOOT_2M, 0) == EFI_SUCCESS);
    assert(boot_map_range(pdpt, 4096 + 3, 4100, BOOT_PTE_UC) == EFI_SUCCESS);
    UINT64 pd_entry = pdpt[0];
    UINT64 *pd = (UINT64 *)(pd_entry & BOOT_PT_MASK);
    assert(!(pd[0] & BOOT_PTE_LARGE));
    UINT64 *pt = (UINT64 *)(pd[0] & BOOT_PT_MASK);
    assert((pt[0] & BOOT_PTE_UC) == 0);
    assert((pt[1] & BOOT_PTE_UC) == BOOT_PTE_UC);
    assert((pt[2] & BOOT_PTE_UC) == BOOT_PTE_UC);
    assert((pt[3] & BOOT_PTE_UC) == 0);
    assert(boot_map_range(pdpt, (257ULL << 30) - 4096, 3 * 4096, BOOT_PTE_UC) == EFI_SUCCESS);
    assert(pdpt[256] & 1);
    assert(pdpt[257] & 1);
    assert(boot_map_range(pdpt, BOOT_DIRECT_SIZE - 4096, 4096, BOOT_PTE_UC) == EFI_SUCCESS);
    assert(boot_map_range(pdpt, BOOT_DIRECT_SIZE - 4096, 4097, 0) == EFI_UNSUPPORTED);
    assert(boot_map_range(pdpt, 0, 0, 0) == EFI_UNSUPPORTED);
    assert(boot_map_range(pdpt, ~(UINT64)0, 2, 0) == EFI_UNSUPPORTED);
    page_fail_at = page_count;
    assert(boot_map_range(pdpt, 5ULL << 30, 4096, 0) == EFI_OUT_OF_RESOURCES);
    puts("PASS unaligned framebuffer, page splitting, boundaries and allocation failure");
}

static void test_exit_contract(void)
{
    reset();
    boot_info_t info = {0};
    assert(exit_boot_services(&services, &info) == EFI_SUCCESS);
    assert(exited && exit_calls == 1 && pool_count == 1 && map_calls == 2);
    assert(info.mmap_addr == (UINT64)mmap_buf && info.mmap_size == 96);
    assert(info.mmap_desc_size == 48 && info.mmap_desc_ver == 1);
    reset();
    assert(get_memory_map() == EFI_SUCCESS);
    void *original = mmap_buf;
    retry_once = TRUE;
    assert(exit_boot_services(&services, &info) == EFI_SUCCESS);
    assert(exit_calls == 2 && pool_count == 1 && mmap_buf == original);
    reset();
    retry_once = grow_on_retry = TRUE;
    assert(exit_boot_services(&services, &info) == EFI_SUCCESS);
    assert(exit_calls == 2 && pool_count == 1 && info.mmap_size == 48 * 40);
    reset();
    map_error = EFI_DEVICE_ERROR;
    assert(exit_boot_services(&services, &info) == EFI_DEVICE_ERROR && !exit_calls);
    reset();
    retry_once = map_fails_on_retry = TRUE;
    assert(exit_boot_services(&services, &info) == EFI_DEVICE_ERROR && exit_calls == 1);
    reset();
    exit_error = EFI_DEVICE_ERROR;
    assert(exit_boot_services(&services, &info) == EFI_DEVICE_ERROR && exit_calls == 1);
    reset();
    exit_error = EFI_INVALID_PARAMETER;
    assert(exit_boot_services(&services, &info) == EFI_INVALID_PARAMETER && exit_calls == 4);
    puts("PASS fresh MapKey, bounded retries, map growth and error propagation");
}

static void test_trace(void)
{
    unsigned pixels[256 * 24];
    memset(pixels, 0xa5, sizeof(pixels));
    boot_trace((UINT64)pixels, 240, 24, 256, 0xe1, EFI_DEVICE_ERROR);
    assert(pixels[0] == 0xa5a5a5a5u);
    assert(pixels[4 * 256 + 12] == 0x00202020u);
    for (unsigned y = 0; y < 24; y++)
        for (unsigned x = 240; x < 256; x++)
            assert(pixels[y * 256 + x] == 0xa5a5a5a5u);
    boot_trace(0, 240, 24, 256, 0xff, 0);
    puts("PASS framebuffer checkpoints respect extent and padded stride");
}

int main(void)
{
    test_high_framebuffer();
    test_mapping_edges();
    test_exit_contract();
    test_trace();
    reset();
    puts("PASS EFI handoff contract");
    return 0;
}
