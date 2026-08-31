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

/* Keep enough desktop space for modern windowed applications while bounding
 * the compositor cost on firmware that exposes very large GOP modes. */
#define GOP_PREFER_W 1280
#define GOP_PREFER_H  800

static EFI_STATUS init_gop(void)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status;

    status = uefi_call_wrapper(BS->LocateProtocol, 3, &gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(status)) {
        Print(L"GOP not available\r\n");
        return status;
    }

    /* Enumerate all modes — collect valid BGRX/RGBX modes */
    UINT32 best_mode = gop->Mode->Mode;
    UINT32 best_w = 0, best_h = 0;
    gop_mode_count = 0;

    for (UINT32 i = 0; i < gop->Mode->MaxMode && gop_mode_count < BOOT_MAX_DISPLAY_MODES; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN info_size = 0;
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, i, &info_size, &info)))
            continue;
        /* Only RGBX (0) and BGRX (1) — skip BltOnly (3) and bitmask (2) */
        if (info->PixelFormat > PixelBlueGreenRedReserved8BitPerColor)
            continue;

        UINT32 w = info->HorizontalResolution;
        UINT32 h = info->VerticalResolution;
        UINT32 pf = (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) ? 1 : 0;

        gop_modes[gop_mode_count].width        = w;
        gop_modes[gop_mode_count].height       = h;
        gop_modes[gop_mode_count].pitch        = info->PixelsPerScanLine;
        gop_modes[gop_mode_count].pixel_format = pf;
        gop_mode_count++;

        /* Select: prefer exact GOP_PREFER_WxGOP_PREFER_H;
         * fallback to largest resolution that fits within those bounds */
        if (w <= GOP_PREFER_W && h <= GOP_PREFER_H) {
            if (w * h > best_w * best_h) {
                best_mode = i;
                best_w = w;
                best_h = h;
            }
        }
        Print(L"  GOP mode %d: %dx%d pf=%d\r\n", i, w, h, pf);
    }

    /* Switch to the selected mode if different from current */
    if (best_mode != gop->Mode->Mode) {
        Print(L"GOP: switching to mode %d (%dx%d)\r\n", best_mode, best_w, best_h);
        uefi_call_wrapper(gop->SetMode, 2, gop, best_mode);
    }
    gop_selected_mode = best_mode;

    /* Read final active mode parameters */
    gop_fb_base   = gop->Mode->FrameBufferBase;
    gop_fb_width  = gop->Mode->Info->HorizontalResolution;
    gop_fb_height = gop->Mode->Info->VerticalResolution;
    gop_fb_pitch  = gop->Mode->Info->PixelsPerScanLine;

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
static UINTN   mmap_size;
static UINTN   mmap_key;
static UINTN   mmap_desc_size;
static UINT32  mmap_desc_ver;

static EFI_STATUS get_memory_map(void)
{
    EFI_STATUS status;

    mmap_size = 0;
    status = uefi_call_wrapper(BS->GetMemoryMap, 5,
                               &mmap_size, NULL, &mmap_key,
                               &mmap_desc_size, &mmap_desc_ver);
    if (status != EFI_BUFFER_TOO_SMALL)
        return status;

    mmap_size += 4 * mmap_desc_size;
    status = uefi_call_wrapper(BS->AllocatePool, 3,
                               EfiLoaderData, mmap_size, (void **)&mmap_buf);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(BS->GetMemoryMap, 5,
                               &mmap_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf,
                               &mmap_key, &mmap_desc_size, &mmap_desc_ver);
    return status;
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

    /* Step 4: Get memory map (LAST before ExitBootServices) */
    status = get_memory_map();
    if (EFI_ERROR(status)) {
        Print(L"FATAL: Cannot get memory map: %r\r\n", status);
        goto halt;
    }

    Print(L"Memory map: %d entries, desc_size=%d\r\n",
          mmap_size / mmap_desc_size, mmap_desc_size);

    /* Step 5: Fill boot_info */
    boot_info_t info;
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

    /* Step 5.5: Build a new PML4 that clones UEFI's mappings and adds
     * an upper-half direct map at 0xFFFF800000000000+ so the relocated
     * kernel can be entered at VA = KERNEL_VBASE + phys.
     *
     * We cannot patch UEFI's PML4 in place — it is marked read-only in
     * UEFI's own tables — so we allocate our own PML4 page, copy the
     * 512 entries, append PML4[256] → new PDPT (1GB huge pages 0..16 GB),
     * and switch CR3.
     *
     * This is a preparatory no-op as long as kernel_entry_addr still
     * points to the lower half; once kernel.ld relocates, the jump
     * below lands on the new PML4[256] entry and this path becomes
     * load-bearing. The kernel's own paging_init builds an equivalent
     * mirror (Fase 1) in its replacement CR3, so the handoff is
     * seamless. */
    {
        UINT64 old_cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(old_cr3));
        UINT64 *old_pml4 = (UINT64 *)(old_cr3 & ~0xFFFULL);

        EFI_PHYSICAL_ADDRESS my_pml4_phys = 0;
        EFI_STATUS st = uefi_call_wrapper(BS->AllocatePages, 4,
                                          AllocateAnyPages, EfiLoaderData,
                                          1, &my_pml4_phys);
        if (EFI_ERROR(st)) {
            Print(L"FATAL: AllocatePages PML4 failed: %r\r\n", st);
            goto halt;
        }

        EFI_PHYSICAL_ADDRESS pdpt_phys = 0;
        st = uefi_call_wrapper(BS->AllocatePages, 4,
                               AllocateAnyPages, EfiLoaderData,
                               1, &pdpt_phys);
        if (EFI_ERROR(st)) {
            Print(L"FATAL: AllocatePages PDPT failed: %r\r\n", st);
            goto halt;
        }

        UINT64 *my_pml4 = (UINT64 *)my_pml4_phys;
        UINT64 *pdpt    = (UINT64 *)pdpt_phys;

        for (int i = 0; i < 512; i++) my_pml4[i] = old_pml4[i];

        for (int i = 0; i < 512; i++) pdpt[i] = 0;
        /* 1GB huge pages covering 0..16 GB. PRESENT | WRITABLE | PS. */
        for (int gb = 0; gb < 16; gb++)
            pdpt[gb] = ((UINT64)gb << 30) | 0x83;

        my_pml4[256] = pdpt_phys | 0x3; /* PRESENT | WRITABLE */

        __asm__ volatile ("mov %0, %%cr3" : : "r"(my_pml4_phys) : "memory");

        Print(L"Upper-half PML4[256] -> PDPT 0x%lx (16 GB) via new PML4 0x%lx\r\n",
              pdpt_phys, my_pml4_phys);
    }

    Print(L"\r\nExiting boot services...\r\n");

    /* Step 6: ExitBootServices — point of no return */
    status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, mmap_key);
    if (EFI_ERROR(status)) {
        /* Memory map may have changed, retry once */
        get_memory_map();
        info.mmap_addr      = (UINT64)mmap_buf;
        info.mmap_size      = mmap_size;
        info.mmap_desc_size = mmap_desc_size;
        info.mmap_desc_ver  = mmap_desc_ver;
        status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, mmap_key);
        if (EFI_ERROR(status)) {
            Print(L"FATAL: ExitBootServices failed: %r\r\n", status);
            goto halt;
        }
    }

    /* === We are now bare-metal — no UEFI services === */

    /* Jump to kernel */
    typedef void (*kernel_entry_fn)(boot_info_t *);
    kernel_entry_fn entry = (kernel_entry_fn)kernel_entry_addr;
    entry(&info);

    /* Should never reach here */
halt:
    for (;;) __asm__ volatile ("hlt");
    return EFI_SUCCESS;
}
