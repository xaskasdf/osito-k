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

/* Forward declarations for kernel functions */
extern void serial_init(void);
extern void serial_puts(const char *s);
extern void serial_puthex(unsigned long long val, int digits);

extern void fb_init(unsigned int *base, unsigned int w, unsigned int h, unsigned int pitch);
extern void fb_clear(void);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, unsigned int color);

extern void kernel_entry(void *memory_map, unsigned long long map_size,
                         unsigned long long desc_size, unsigned long long desc_version);

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

    /* Initialize our serial driver */
    serial_init();
    serial_puts("\r\n[OsitoK] Serial initialized (COM1 115200)\r\n");

    /* Initialize our framebuffer driver */
    fb_init(gop_fb_base, gop_fb_width, gop_fb_height, gop_fb_pitch);
    fb_clear();

    /* Hand off to kernel */
    kernel_entry(mmap_buf, (unsigned long long)mmap_size,
                 (unsigned long long)mmap_desc_size,
                 (unsigned long long)mmap_desc_ver);

    /* Should never reach here */
halt:
    for (;;) __asm__ volatile ("hlt");
    return EFI_SUCCESS;
}
