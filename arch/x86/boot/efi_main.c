/*
 * OsitoK x86-64 — UEFI Entry Point
 *
 * Boot flow:
 *   UEFI -> efi_main() -> GOP init -> serial init -> memory map ->
 *   ExitBootServices() -> kernel_entry()
 *
 * Built with gnu-efi.
 */

#include <efi.h>
#include <efilib.h>

/* boot_info_t definition (must match include/boot_info.h) */
typedef struct {
    unsigned int magic;
    unsigned int version;
    unsigned long long fb_base;
    unsigned int fb_width;
    unsigned int fb_height;
    unsigned int fb_pitch;
    unsigned long long mmap_addr;
    unsigned long long mmap_size;
    unsigned long long mmap_desc_size;
    unsigned int mmap_desc_ver;
    unsigned int _pad0;
    unsigned long long acpi_rsdp;
    unsigned long long kernel_phys_base;
    unsigned long long kernel_size;
} boot_info_t;

extern void kernel_entry(boot_info_t *info);

/* ── GOP Framebuffer Setup ───────────────────────────────────── */

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
static unsigned int *gop_fb_base;
static unsigned int  gop_fb_width;
static unsigned int  gop_fb_height;
static unsigned int  gop_fb_pitch;

static EFI_STATUS init_gop(EFI_SYSTEM_TABLE *ST)
{
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status;

    status = uefi_call_wrapper(BS->LocateProtocol, 3, &gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(status)) {
        Print(L"GOP not available\r\n");
        return status;
    }

    gop_fb_base   = (unsigned int *)(unsigned long long)gop->Mode->FrameBufferBase;
    gop_fb_width  = gop->Mode->Info->HorizontalResolution;
    gop_fb_height = gop->Mode->Info->VerticalResolution;
    gop_fb_pitch  = gop->Mode->Info->PixelsPerScanLine;

    Print(L"GOP: %dx%d, pitch=%d, fb=0x%lx\r\n",
          gop_fb_width, gop_fb_height, gop_fb_pitch,
          gop->Mode->FrameBufferBase);

    return EFI_SUCCESS;
}

/* ── Memory Map ──────────────────────────────────────────────── */

static UINT8          *mmap_buf;
static UINTN           mmap_size;
static UINTN           mmap_key;
static UINTN           mmap_desc_size;
static UINT32          mmap_desc_ver;

static EFI_STATUS get_memory_map(EFI_SYSTEM_TABLE *ST)
{
    EFI_STATUS status;

    /* First call to get required buffer size */
    mmap_size = 0;
    status = uefi_call_wrapper(BS->GetMemoryMap, 5,
                               &mmap_size, NULL, &mmap_key,
                               &mmap_desc_size, &mmap_desc_ver);
    if (status != EFI_BUFFER_TOO_SMALL)
        return status;

    /* Allocate extra for the allocation itself */
    mmap_size += 2 * mmap_desc_size;
    status = uefi_call_wrapper(BS->AllocatePool, 3,
                               EfiLoaderData, mmap_size, (void **)&mmap_buf);
    if (EFI_ERROR(status)) return status;

    /* Get actual memory map */
    status = uefi_call_wrapper(BS->GetMemoryMap, 5,
                               &mmap_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf,
                               &mmap_key, &mmap_desc_size, &mmap_desc_ver);
    return status;
}

/* ── ACPI RSDP from EFI System Table ─────────────────────────── */

/* Exported to kernel — smp.c reads this */
unsigned long long efi_acpi_rsdp;

static int guid_eq(EFI_GUID *a, EFI_GUID *b)
{
    unsigned char *p = (unsigned char *)a;
    unsigned char *q = (unsigned char *)b;
    for (int i = 0; i < 16; i++)
        if (p[i] != q[i]) return 0;
    return 1;
}

static void find_acpi_rsdp(EFI_SYSTEM_TABLE *ST)
{
    /* ACPI 2.0 GUID: 8868E871-E4F1-11D3-BC22-0080C73C8881 */
    EFI_GUID acpi20_guid = { 0x8868E871, 0xE4F1, 0x11D3,
        { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } };
    /* ACPI 1.0 GUID: EB9D2D30-2D88-11D3-9A16-0090273FC14D */
    EFI_GUID acpi10_guid = { 0xEB9D2D30, 0x2D88, 0x11D3,
        { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

    UINTN i;
    for (i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (guid_eq(&t->VendorGuid, &acpi20_guid)) {
            efi_acpi_rsdp = (unsigned long long)t->VendorTable;
            Print(L"ACPI 2.0 RSDP at 0x%lx\r\n", efi_acpi_rsdp);
            return;
        }
    }
    for (i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (guid_eq(&t->VendorGuid, &acpi10_guid)) {
            efi_acpi_rsdp = (unsigned long long)t->VendorTable;
            Print(L"ACPI 1.0 RSDP at 0x%lx\r\n", efi_acpi_rsdp);
            return;
        }
    }
    efi_acpi_rsdp = 0;
    Print(L"ACPI RSDP: %d config tables (ST=%lx)\r\n",
          ST->NumberOfTableEntries, (unsigned long long)ST);
}

/* ── EFI Main ────────────────────────────────────────────────── */

EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS status;

    /* Initialize gnu-efi library */
    InitializeLib(ImageHandle, SystemTable);

    /* Clear screen */
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

    Print(L"OsitoK x86-64 — Bare-Metal AI OS\r\n");
    Print(L"=================================\r\n\r\n");

    /* Initialize GOP */
    status = init_gop(SystemTable);
    if (EFI_ERROR(status)) {
        Print(L"FATAL: Cannot initialize GOP framebuffer\r\n");
        goto halt;
    }

    /* Find ACPI RSDP (must be done BEFORE ExitBootServices)
     * Use gnu-efi global ST (initialized by InitializeLib) */
    find_acpi_rsdp(ST);

    /* Get memory map (must be done LAST before ExitBootServices) */
    status = get_memory_map(SystemTable);
    if (EFI_ERROR(status)) {
        Print(L"FATAL: Cannot get memory map: %r\r\n", status);
        goto halt;
    }

    Print(L"Memory map: %d entries, desc_size=%d\r\n",
          mmap_size / mmap_desc_size, mmap_desc_size);

    Print(L"\r\nExiting boot services...\r\n");

    /* === ExitBootServices — point of no return === */
    status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, mmap_key);
    if (EFI_ERROR(status)) {
        /* Memory map may have changed, retry once */
        get_memory_map(SystemTable);
        status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, mmap_key);
        if (EFI_ERROR(status)) {
            Print(L"FATAL: ExitBootServices failed: %r\r\n", status);
            goto halt;
        }
    }

    /* === We are now bare-metal === */
    /* No more UEFI Boot Services available */

    /* Fill boot_info and hand off to kernel */
    boot_info_t info;
    info.magic           = 0x4F53544B;
    info.version         = 1;
    info.fb_base         = (unsigned long long)gop_fb_base;
    info.fb_width        = gop_fb_width;
    info.fb_height       = gop_fb_height;
    info.fb_pitch        = gop_fb_pitch;
    info.mmap_addr       = (unsigned long long)mmap_buf;
    info.mmap_size       = (unsigned long long)mmap_size;
    info.mmap_desc_size  = (unsigned long long)mmap_desc_size;
    info.mmap_desc_ver   = mmap_desc_ver;
    info._pad0           = 0;
    info.acpi_rsdp       = efi_acpi_rsdp;
    info.kernel_phys_base = 0;  /* Legacy build: kernel is part of EFI binary */
    info.kernel_size      = 0;

    kernel_entry(&info);

    /* Should never reach here */
halt:
    for (;;) __asm__ volatile ("hlt");
    return EFI_SUCCESS;
}
