/*
 * OsitoK x86-64 — UEFI Bootloader
 *
 * Standalone boot.efi that loads kernel.elf from the ESP and jumps to it.
 * PXE builds define OK_PXE_EMBED_KERNEL and embed kernel.elf directly.
 * The kernel is compiled WITHOUT EFI restrictions (-fno-pie, jump tables OK).
 *
 * Boot flow:
 *   UEFI → boot.efi → load kernel.elf → GOP → ACPI → memory map →
 *   ExitBootServices() → jump to kernel_entry(boot_info_t*)
 *
 * Built with gnu-efi. ~300 lines.
 */

#include <efi.h>
#include <efilib.h>
#include "boot_paging.h"
#include "../include/boot_trace.h"

/* ── Boot info passed to kernel ─────────────────────────────── */

#define BOOT_INFO_MAGIC        0x4F53544B  /* 'OSTK' */
#define BOOT_INFO_VERSION      2
#define BOOT_MAX_DISPLAY_MODES 16

typedef struct {
    UINT32 width;
    UINT32 height;
    UINT32 pitch;
    UINT32 pixel_format;   /* 0=RGBX, 1=BGRX */
} boot_display_mode_t;

typedef struct {
    UINT32 magic;
    UINT32 version;

    UINT64 fb_base;
    UINT32 fb_width;
    UINT32 fb_height;
    UINT32 fb_pitch;

    UINT64 mmap_addr;
    UINT64 mmap_size;
    UINT64 mmap_desc_size;
    UINT32 mmap_desc_ver;
    UINT32 _pad0;

    UINT64 acpi_rsdp;
    UINT64 kernel_phys_base;
    UINT64 kernel_size;

    UINT32 display_mode_count;
    UINT32 display_current_mode;
    boot_display_mode_t display_modes[BOOT_MAX_DISPLAY_MODES];
} boot_info_t;

/* ── Minimal ELF64 defs (self-contained, no elf.h) ──────────── */

#define ELF_MAGIC     0x464C457F  /* "\x7fELF" */
#define PT_LOAD       1

typedef struct {
    UINT32 e_ident_mag;      /* 0x7f 'E' 'L' 'F' */
    UINT8  e_ident_rest[12]; /* class, data, version, os, pad */
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} elf64_phdr_t;

/* ── GOP Framebuffer ─────────────────────────────────────────── */

static UINT64  gop_fb_base;
static UINT32  gop_fb_width;
static UINT32  gop_fb_height;
static UINT32  gop_fb_pitch;

/* Mode table filled during enumeration — passed to kernel via boot_info */
static boot_display_mode_t gop_modes[BOOT_MAX_DISPLAY_MODES];
static UINT32 gop_mode_count;
static UINT32 gop_selected_mode;

static BOOLEAN gop_mode_usable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
                               UINTN size)
{
    if (!info || size < sizeof(*info) || !info->HorizontalResolution ||
        !info->VerticalResolution ||
        info->PixelsPerScanLine < info->HorizontalResolution ||
        (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
         info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor))
        return FALSE;

    UINT64 row_bytes = (UINT64)info->PixelsPerScanLine * sizeof(UINT32);
    return info->VerticalResolution <= (~(UINT64)0) / row_bytes;
}

static BOOLEAN gop_framebuffer_usable(const EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    if (!gop || !gop->Mode || gop->Mode->Mode >= gop->Mode->MaxMode ||
        !gop_mode_usable(gop->Mode->Info, gop->Mode->SizeOfInfo))
        return FALSE;

    UINT64 bytes = (UINT64)gop->Mode->Info->PixelsPerScanLine *
                   gop->Mode->Info->VerticalResolution * sizeof(UINT32);
    return gop->Mode->FrameBufferBase && gop->Mode->FrameBufferSize >= bytes &&
           gop->Mode->FrameBufferBase <= (~(UINT64)0) - bytes;
}

static EFI_STATUS init_gop(void)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status = EFI_NOT_FOUND;

    gop_fb_base = 0;
    gop_fb_width = gop_fb_height = gop_fb_pitch = 0;
    gop_mode_count = gop_selected_mode = 0;

    /* Prefer the firmware console's adapter on systems with multiple GPUs. */
    if (ST->ConsoleOutHandle)
        status = uefi_call_wrapper(BS->HandleProtocol, 3, ST->ConsoleOutHandle,
                                   &gop_guid, (void **)&gop);
    if (EFI_ERROR(status) || !gop)
        status = uefi_call_wrapper(BS->LocateProtocol, 3, &gop_guid, NULL,
                                   (void **)&gop);
    if (EFI_ERROR(status) || !gop) {
        Print(L"GOP not available\r\n");
        return EFI_ERROR(status) ? status : EFI_NOT_FOUND;
    }

    if (!gop->Mode || !gop->Mode->MaxMode)
        return EFI_UNSUPPORTED;

    /* A valid firmware mode already drives the connected display. Only try a
     * mode change when it cannot provide the kernel's linear framebuffer. */
    if (!gop_framebuffer_usable(gop)) {
        status = EFI_UNSUPPORTED;
        for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
            UINTN info_size = 0;
            EFI_STATUS query = uefi_call_wrapper(gop->QueryMode, 4, gop, i,
                                                 &info_size, &info);
            BOOLEAN usable = !EFI_ERROR(query) && gop_mode_usable(info, info_size);
            if (info)
                uefi_call_wrapper(BS->FreePool, 1, info);
            if (!usable)
                continue;

            Print(L"GOP: activating mode %d (no usable framebuffer)\r\n", i);
            status = uefi_call_wrapper(gop->SetMode, 2, gop, i);
            Print(L"GOP: SetMode(%d) returned %r\r\n", i, status);
            if (!EFI_ERROR(status)) {
                if (gop_framebuffer_usable(gop))
                    break;
                status = EFI_DEVICE_ERROR;
            }
        }
        if (EFI_ERROR(status) || !gop_framebuffer_usable(gop)) {
            Print(L"GOP: no usable linear framebuffer (%r)\r\n", status);
            return status;
        }
    }

    UINT32 active_mode = gop->Mode->Mode;
    const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *current = gop->Mode->Info;
    gop_fb_base   = gop->Mode->FrameBufferBase;
    gop_fb_width  = current->HorizontalResolution;
    gop_fb_height = current->VerticalResolution;
    gop_fb_pitch  = current->PixelsPerScanLine;

    /* The handoff uses compact table indices, not firmware mode IDs. Reserve
     * index zero for the active mode even when the table fills up. */
    gop_modes[gop_mode_count++] = (boot_display_mode_t){
        gop_fb_width, gop_fb_height, gop_fb_pitch, (UINT32)current->PixelFormat
    };
    Print(L"GOP: keeping active mode %d (%dx%d)\r\n",
          active_mode, gop_fb_width, gop_fb_height);

    for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
        if (i == active_mode)
            continue;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN info_size = 0;
        EFI_STATUS query = uefi_call_wrapper(gop->QueryMode, 4, gop, i,
                                             &info_size, &info);
        if (!EFI_ERROR(query) && gop_mode_usable(info, info_size)) {
            if (gop_mode_count < BOOT_MAX_DISPLAY_MODES) {
                gop_modes[gop_mode_count++] = (boot_display_mode_t){
                    info->HorizontalResolution, info->VerticalResolution,
                    info->PixelsPerScanLine, (UINT32)info->PixelFormat
                };
            }
            Print(L"  GOP mode %d: %dx%d pf=%d\r\n", i,
                  info->HorizontalResolution, info->VerticalResolution,
                  info->PixelFormat);
        }
        if (info)
            uefi_call_wrapper(BS->FreePool, 1, info);
    }

    Print(L"GOP: %dx%d pitch=%d fb=0x%lx (%d modes found)\r\n",
          gop_fb_width, gop_fb_height, gop_fb_pitch, gop_fb_base, gop_mode_count);

    return EFI_SUCCESS;
}

/* ── ACPI RSDP ───────────────────────────────────────────────── */

static UINT64 acpi_rsdp_addr;

static int guid_eq(EFI_GUID *a, EFI_GUID *b)
{
    UINT8 *p = (UINT8 *)a;
    UINT8 *q = (UINT8 *)b;
    for (int i = 0; i < 16; i++)
        if (p[i] != q[i]) return 0;
    return 1;
}

static void find_acpi_rsdp(EFI_SYSTEM_TABLE *ST)
{
    EFI_GUID acpi20 = { 0x8868E871, 0xE4F1, 0x11D3,
        { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } };
    EFI_GUID acpi10 = { 0xEB9D2D30, 0x2D88, 0x11D3,
        { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (guid_eq(&t->VendorGuid, &acpi20)) {
            acpi_rsdp_addr = (UINT64)t->VendorTable;
            Print(L"ACPI 2.0 RSDP at 0x%lx\r\n", acpi_rsdp_addr);
            return;
        }
    }
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (guid_eq(&t->VendorGuid, &acpi10)) {
            acpi_rsdp_addr = (UINT64)t->VendorTable;
            Print(L"ACPI 1.0 RSDP at 0x%lx\r\n", acpi_rsdp_addr);
            return;
        }
    }
    acpi_rsdp_addr = 0;
    Print(L"ACPI RSDP not found (%d config tables)\r\n", ST->NumberOfTableEntries);
}

/* ── Load kernel ELF from ESP ────────────────────────────────── */

static UINT64 kernel_phys_lo;
static UINT64 kernel_phys_hi;
static UINT64 kernel_entry_addr;

#ifdef OK_PXE_EMBED_KERNEL
extern const UINT8 ok_pxe_kernel_elf_start[];
extern const UINT8 ok_pxe_kernel_elf_end[];

static EFI_STATUS load_kernel_elf_from_memory(const UINT8 *image, UINTN image_size)
{
    if (!image || image_size < sizeof(elf64_ehdr_t))
        return EFI_INVALID_PARAMETER;

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)image;
    if (ehdr->e_ident_mag != ELF_MAGIC) {
        Print(L"Invalid embedded ELF magic (expected 0x%x got 0x%x)\r\n",
              ELF_MAGIC, ehdr->e_ident_mag);
        return EFI_INVALID_PARAMETER;
    }

    UINT64 ph_end = ehdr->e_phoff + ((UINT64)ehdr->e_phnum * ehdr->e_phentsize);
    if (ehdr->e_phentsize < sizeof(elf64_phdr_t) || ph_end > image_size) {
        Print(L"Invalid embedded ELF program header table\r\n");
        return EFI_INVALID_PARAMETER;
    }

    Print(L"embedded kernel.elf: entry=0x%lx, %d phdrs\r\n",
          ehdr->e_entry, ehdr->e_phnum);

    kernel_entry_addr = ehdr->e_entry;
    kernel_phys_lo = ~0ULL;
    kernel_phys_hi = 0;

    for (UINT16 i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *phdr =
            (const elf64_phdr_t *)(image + ehdr->e_phoff + i * ehdr->e_phentsize);

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0)
            continue;
        if (phdr->p_offset + phdr->p_filesz > image_size) {
            Print(L"  embedded LOAD %d exceeds image size\r\n", i);
            return EFI_INVALID_PARAMETER;
        }

        Print(L"  LOAD: vaddr=0x%lx filesz=0x%lx memsz=0x%lx\r\n",
              phdr->p_vaddr, phdr->p_filesz, phdr->p_memsz);

        UINT64 seg_base = phdr->p_paddr;
        UINT64 seg_end  = seg_base + phdr->p_memsz;
        UINT64 pages = (seg_end - (seg_base & ~0xFFFULL) + 0xFFF) >> 12;

        EFI_PHYSICAL_ADDRESS alloc_addr = seg_base & ~0xFFFULL;
        EFI_STATUS status = uefi_call_wrapper(BS->AllocatePages, 4,
                                              AllocateAddress, EfiLoaderData,
                                              pages, &alloc_addr);
        if (EFI_ERROR(status)) {
            Print(L"  AllocatePages at 0x%lx (%d pages) failed: %r\r\n",
                  alloc_addr, pages, status);
            return status;
        }

        UINT8 *dst = (UINT8 *)seg_base;
        for (UINT64 j = 0; j < phdr->p_memsz; j++)
            dst[j] = 0;
        for (UINT64 j = 0; j < phdr->p_filesz; j++)
            dst[j] = image[phdr->p_offset + j];

        if (seg_base < kernel_phys_lo) kernel_phys_lo = seg_base;
        if (seg_end  > kernel_phys_hi) kernel_phys_hi = seg_end;
    }

    Print(L"Kernel loaded: 0x%lx-0x%lx (%d KB)\r\n",
          kernel_phys_lo, kernel_phys_hi,
          (kernel_phys_hi - kernel_phys_lo) / 1024);

    return EFI_SUCCESS;
}

static EFI_STATUS load_kernel_elf(EFI_HANDLE ImageHandle)
{
    (void)ImageHandle;
    UINTN image_size = (UINTN)(ok_pxe_kernel_elf_end - ok_pxe_kernel_elf_start);
    Print(L"PXE embedded kernel.elf: %d bytes\r\n", image_size);
    return load_kernel_elf_from_memory(ok_pxe_kernel_elf_start, image_size);
}
#else
static EFI_STATUS load_kernel_elf(EFI_HANDLE ImageHandle)
{
    EFI_STATUS status;
    EFI_GUID sfsp_guid = SIMPLE_FILE_SYSTEM_PROTOCOL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfsp;
    EFI_FILE *root, *file;
    UINTN num_handles;
    EFI_HANDLE *handles;

    (void)ImageHandle;

    /* Find all volumes with a file system */
    status = uefi_call_wrapper(BS->LocateHandleBuffer, 5,
                               ByProtocol, &sfsp_guid, NULL,
                               &num_handles, &handles);
    if (EFI_ERROR(status)) {
        Print(L"LocateHandleBuffer: %r\r\n", status);
        return status;
    }

    Print(L"Found %d file system(s)\r\n", num_handles);

    /* Try each volume for kernel.elf */
    file = NULL;
    for (UINTN h = 0; h < num_handles; h++) {
        status = uefi_call_wrapper(BS->HandleProtocol, 3,
                                   handles[h], &sfsp_guid, (void **)&sfsp);
        if (EFI_ERROR(status)) continue;

        status = uefi_call_wrapper(sfsp->OpenVolume, 2, sfsp, &root);
        if (EFI_ERROR(status)) continue;

        status = uefi_call_wrapper(root->Open, 5, root, &file,
                                   L"\\EFI\\BOOT\\kernel.elf",
                                   EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(status)) {
            Print(L"kernel.elf found on volume %d\r\n", h);
            break;
        }

        uefi_call_wrapper(root->Close, 1, root);
        root = NULL;
    }

    uefi_call_wrapper(BS->FreePool, 1, handles);

    if (file == NULL) {
        Print(L"kernel.elf not found on any volume\r\n");
        return EFI_NOT_FOUND;
    }

    /* Read ELF header */
    elf64_ehdr_t ehdr;
    UINTN sz = sizeof(ehdr);
    status = uefi_call_wrapper(file->Read, 3, file, &sz, &ehdr);
    Print(L"ELF read: status=%r sz=%d magic=0x%x\r\n", status, sz, ehdr.e_ident_mag);
    if (EFI_ERROR(status) || ehdr.e_ident_mag != ELF_MAGIC) {
        Print(L"Invalid ELF magic (expected 0x%x got 0x%x)\r\n",
              ELF_MAGIC, ehdr.e_ident_mag);
        return EFI_INVALID_PARAMETER;
    }

    Print(L"kernel.elf: entry=0x%lx, %d phdrs\r\n", ehdr.e_entry, ehdr.e_phnum);

    kernel_entry_addr = ehdr.e_entry;
    kernel_phys_lo = ~0ULL;
    kernel_phys_hi = 0;

    UINT64 load_start = ~0ULL;
    UINT64 load_end = 0;

    for (UINT16 i = 0; i < ehdr.e_phnum; i++) {
        elf64_phdr_t phdr;
        UINT64 phdr_off = ehdr.e_phoff + i * ehdr.e_phentsize;

        uefi_call_wrapper(file->SetPosition, 2, file, phdr_off);
        sz = sizeof(phdr);
        uefi_call_wrapper(file->Read, 3, file, &sz, &phdr);

        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0)
            continue;

        UINT64 seg_start = phdr.p_paddr & ~0xFFFULL;
        UINT64 seg_end = (phdr.p_paddr + phdr.p_memsz + 0xFFFULL) & ~0xFFFULL;
        if (seg_start < load_start) load_start = seg_start;
        if (seg_end > load_end) load_end = seg_end;
    }

    if (load_start == ~0ULL || load_end <= load_start)
        return EFI_LOAD_ERROR;

    UINT64 load_pages = (load_end - load_start) >> 12;
    EFI_PHYSICAL_ADDRESS load_addr = load_start;
    status = uefi_call_wrapper(BS->AllocatePages, 4,
                               AllocateAddress, EfiLoaderData,
                               load_pages, &load_addr);
    if (EFI_ERROR(status)) {
        Print(L"  AllocatePages kernel range 0x%lx-0x%lx (%d pages) failed: %r\r\n",
              load_start, load_end, load_pages, status);
        return status;
    }
    uefi_call_wrapper(BS->SetMem, 3, (VOID *)(UINTN)load_start,
                      (UINTN)(load_end - load_start), 0);

    /* Load each PT_LOAD segment */
    for (UINT16 i = 0; i < ehdr.e_phnum; i++) {
        elf64_phdr_t phdr;
        UINT64 phdr_off = ehdr.e_phoff + i * ehdr.e_phentsize;

        /* Seek and read program header */
        uefi_call_wrapper(file->SetPosition, 2, file, phdr_off);
        sz = sizeof(phdr);
        uefi_call_wrapper(file->Read, 3, file, &sz, &phdr);

        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0)
            continue;

        Print(L"  LOAD: vaddr=0x%lx filesz=0x%lx memsz=0x%lx\r\n",
              phdr.p_vaddr, phdr.p_filesz, phdr.p_memsz);

        UINT64 seg_base = phdr.p_paddr;
        UINT64 seg_end  = seg_base + phdr.p_memsz;

        /* Do not depend on firmware returning clean pages: NOBITS tails such
         * as .bss/.lbss must be zero before the kernel sees them. */
        UINT8 *dst = (UINT8 *)seg_base;
        for (UINT64 j = 0; j < phdr.p_memsz; j++)
            dst[j] = 0;

        /* Copy file data */
        if (phdr.p_filesz > 0) {
            uefi_call_wrapper(file->SetPosition, 2, file, phdr.p_offset);
            UINTN read_sz = (UINTN)phdr.p_filesz;
            uefi_call_wrapper(file->Read, 3, file, &read_sz, dst);
        }

        /* Track extent */
        if (seg_base < kernel_phys_lo) kernel_phys_lo = seg_base;
        if (seg_end  > kernel_phys_hi) kernel_phys_hi = seg_end;
    }

    uefi_call_wrapper(file->Close, 1, file);
    uefi_call_wrapper(root->Close, 1, root);

    Print(L"Kernel loaded: 0x%lx-0x%lx (%d KB)\r\n",
          kernel_phys_lo, kernel_phys_hi,
          (kernel_phys_hi - kernel_phys_lo) / 1024);

    return EFI_SUCCESS;
}
#endif

/* ── Memory Map ─────────────────────────────────────────────── */

static UINT8  *mmap_buf;
static UINTN   mmap_capacity;
static UINTN   mmap_size;
static UINTN   mmap_key;
static UINTN   mmap_desc_size;
static UINT32  mmap_desc_ver;

static EFI_STATUS get_memory_map(void)
{
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        mmap_size = mmap_capacity;
        EFI_STATUS status = uefi_call_wrapper(BS->GetMemoryMap, 5,
            &mmap_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf, &mmap_key,
            &mmap_desc_size, &mmap_desc_ver);
        if (status != EFI_BUFFER_TOO_SMALL) {
            if (!EFI_ERROR(status) && (!mmap_size ||
                mmap_desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
                mmap_size % mmap_desc_size))
                return EFI_DEVICE_ERROR;
            return status;
        }
        if (mmap_desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
            mmap_desc_size > (~(UINTN)0) / 16 ||
            mmap_size > (~(UINTN)0) - 16 * mmap_desc_size)
            return EFI_OUT_OF_RESOURCES;
        UINTN capacity = mmap_size + 16 * mmap_desc_size;
        UINT8 *buffer = NULL;
        status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                                   capacity, (void **)&buffer);
        if (EFI_ERROR(status))
            return status;
        if (mmap_buf)
            uefi_call_wrapper(BS->FreePool, 1, mmap_buf);
        mmap_buf = buffer;
        mmap_capacity = capacity;
    }
    return EFI_BUFFER_TOO_SMALL;
}

static EFI_STATUS exit_boot_services(EFI_HANDLE image, boot_info_t *info)
{
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        EFI_STATUS status = get_memory_map();
        if (EFI_ERROR(status))
            return status;
        info->mmap_addr = (UINT64)mmap_buf;
        info->mmap_size = mmap_size;
        info->mmap_desc_size = mmap_desc_size;
        info->mmap_desc_ver = mmap_desc_ver;
        /* No printing, protocol calls or allocations between these calls. */
        status = uefi_call_wrapper(BS->ExitBootServices, 2, image, mmap_key);
        if (status != EFI_INVALID_PARAMETER)
            return status;
    }
    return EFI_INVALID_PARAMETER;
}

/* SysV arguments: CR3, stack top, entry, boot_info. The low firmware mappings
 * retain this trampoline until the high-half kernel takes control. */
__attribute__((naked, noreturn, sysv_abi))
static void boot_enter_kernel(UINT64 cr3 __attribute__((unused)),
                              UINT64 stack __attribute__((unused)),
                              UINT64 entry __attribute__((unused)),
                              boot_info_t *info __attribute__((unused)))
{
    __asm__ volatile (
        "cli\n\t"
        "cld\n\t"
        "mov %rdi, %cr3\n\t"
        "mov %rsi, %rsp\n\t"
        "and $-16, %rsp\n\t"
        "xor %ebp, %ebp\n\t"
        "mov %rcx, %rdi\n\t"
        "call *%rdx\n\t"
        "1: cli\n\t"
        "hlt\n\t"
        "jmp 1b\n\t");
}

/* ── EFI Main ───────────────────────────────────────────────── */

EFI_STATUS
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS status;

    InitializeLib(ImageHandle, SystemTable);
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

    Print(L"OsitoK boot.efi — UEFI Bootloader\r\n");
    Print(L"==================================\r\n\r\n");

    /* Step 1: Load kernel ELF (must be before ExitBootServices — uses file I/O) */
    status = load_kernel_elf(ImageHandle);
    if (EFI_ERROR(status)) {
        Print(L"FATAL: Cannot load kernel.elf: %r\r\n", status);
        goto halt;
    }

    /* Step 2: Initialize GOP */
    status = init_gop();
    if (EFI_ERROR(status)) {
        Print(L"FATAL: Cannot initialize GOP framebuffer\r\n");
        goto halt;
    }

    /* Step 3: Find ACPI RSDP */
    find_acpi_rsdp(ST);

    /* Keep our stack and boot_info in loader-owned memory, not on the
     * firmware stack that the kernel will reclaim as BootServicesData. */
    EFI_PHYSICAL_ADDRESS stack_base = 0xffffffffULL;
    const UINTN stack_pages = 128;
    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress,
                               EfiLoaderData, stack_pages, &stack_base);
    if (EFI_ERROR(status)) {
        Print(L"FATAL: Cannot allocate kernel stack: %r\r\n", status);
        goto halt;
    }

    /* Preliminary map for building tables. Refresh again immediately at exit. */
    status = get_memory_map();
    if (EFI_ERROR(status)) {
        Print(L"FATAL: Cannot get memory map: %r\r\n", status);
        goto halt;
    }

    Print(L"Memory map: %d entries, desc_size=%d\r\n",
          mmap_size / mmap_desc_size, mmap_desc_size);

    /* Step 5: Fill boot_info */
    static boot_info_t info;
    info.magic           = BOOT_INFO_MAGIC;
    info.version         = BOOT_INFO_VERSION;
    info.fb_base         = gop_fb_base;
    info.fb_width        = gop_fb_width;
    info.fb_height       = gop_fb_height;
    info.fb_pitch        = gop_fb_pitch;
    info.mmap_addr       = (UINT64)mmap_buf;
    info.mmap_size       = mmap_size;
    info.mmap_desc_size  = mmap_desc_size;
    info.mmap_desc_ver   = mmap_desc_ver;
    info._pad0           = 0;
    info.acpi_rsdp       = acpi_rsdp_addr;
    info.kernel_phys_base = kernel_phys_lo;
    info.kernel_size      = kernel_phys_hi - kernel_phys_lo;
    info.display_mode_count   = gop_mode_count;
    info.display_current_mode = gop_selected_mode;
    for (UINT32 i = 0; i < gop_mode_count; i++)
        info.display_modes[i] = gop_modes[i];

    UINT64 firmware_cr3, kernel_cr3 = 0;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(firmware_cr3));
    UINT64 fb_bytes = (UINT64)gop_fb_pitch * gop_fb_height * sizeof(UINT32);
    UINT32 apic_lo, apic_hi;
    __asm__ volatile ("rdmsr" : "=a"(apic_lo), "=d"(apic_hi) : "c"(0x1b));
    UINT64 apic_base = (((UINT64)apic_hi << 32) | apic_lo) & 0xfffffffff000ULL;
    status = boot_prepare_paging((const UINT64 *)(firmware_cr3 & BOOT_PT_MASK),
        mmap_buf, mmap_size, mmap_desc_size, gop_fb_base, fb_bytes, apic_base, &kernel_cr3);
    if (EFI_ERROR(status)) {
        Print(L"FATAL: Cannot prepare RAM/GOP mappings: %r\r\n", status);
        goto halt;
    }
    Print(L"Handoff: CR3=0x%lx stack=0x%lx-0x%lx entry=0x%lx\r\n",
          kernel_cr3, stack_base, stack_base + stack_pages * 4096, kernel_entry_addr);
    Print(L"Handoff: GOP 0x%lx -> 0x%lx (%ld bytes); firmware CR3 unchanged\r\n",
          gop_fb_base, BOOT_DIRECT_BASE + gop_fb_base, fb_bytes);
    Print(L"Stages (bottom right): E0=exit pending, E1=exit OK, C1=kernel, C2=BSS OK\r\n");

    Print(L"\r\nExiting boot services...\r\n");

    boot_trace(gop_fb_base, gop_fb_width, gop_fb_height, gop_fb_pitch, 0xe0, 0);
    status = exit_boot_services(ImageHandle, &info);
    __asm__ volatile ("cli" ::: "memory");
    if (EFI_ERROR(status)) {
        /* Even a failed exit may have shut down the console protocol. */
        boot_trace(gop_fb_base, gop_fb_width, gop_fb_height, gop_fb_pitch, 0xef, status);
        goto halt;
    }
    boot_trace(gop_fb_base, gop_fb_width, gop_fb_height, gop_fb_pitch, 0xe1, 0);
    boot_enter_kernel(kernel_cr3, stack_base + stack_pages * 4096,
                      kernel_entry_addr, &info);

    /* Should never reach here */
halt:
    for (;;) __asm__ volatile ("cli; hlt");
    return EFI_SUCCESS;
}
