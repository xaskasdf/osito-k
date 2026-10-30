#ifndef OSITOK_BOOT_PAGING_H
#define OSITOK_BOOT_PAGING_H

/* The kernel's bootstrap direct map occupies one PML4 slot (512 GiB).
 * Populate RAM and the actual GOP aperture, not the holes between them. */
#define BOOT_DIRECT_BASE  0xffff800000000000ULL
#define BOOT_DIRECT_SIZE (1ULL << 39)
#define BOOT_PT_MASK     0x000ffffffffff000ULL
#define BOOT_PTE_RW      0x003ULL
#define BOOT_PTE_LARGE   0x080ULL
#define BOOT_PTE_UC      0x018ULL
#define BOOT_2M          (1ULL << 21)

static EFI_STATUS boot_alloc_table(UINT64 **table)
{
    EFI_PHYSICAL_ADDRESS address = 0xffffffffULL;
    EFI_STATUS status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress,
                                         EfiLoaderData, 1, &address);
    if (EFI_ERROR(status))
        return status;
    *table = (UINT64 *)address;
    for (UINTN i = 0; i < 512; i++)
        (*table)[i] = 0;
    return EFI_SUCCESS;
}

static EFI_STATUS boot_next_table(UINT64 *entry, UINT64 **table)
{
    if (*entry & 1) {
        *table = (UINT64 *)(*entry & BOOT_PT_MASK);
        return EFI_SUCCESS;
    }
    EFI_STATUS status = boot_alloc_table(table);
    if (!EFI_ERROR(status))
        *entry = (UINT64)*table | BOOT_PTE_RW;
    return status;
}

static EFI_STATUS boot_map_range(UINT64 *pdpt, UINT64 base, UINT64 size,
                                 UINT64 cache_flags)
{
    if (!size || base >= BOOT_DIRECT_SIZE || size > BOOT_DIRECT_SIZE - base)
        return EFI_UNSUPPORTED;
    UINT64 end = (base + size + 4095) & ~4095ULL;
    base &= ~4095ULL;
    while (base < end) {
        UINT64 *pd;
        EFI_STATUS status = boot_next_table(&pdpt[base >> 30], &pd);
        if (EFI_ERROR(status))
            return status;
        UINT64 *entry = &pd[(base >> 21) & 511];
        if (!(base & (BOOT_2M - 1)) && end - base >= BOOT_2M &&
            (!(*entry & 1) || (*entry & BOOT_PTE_LARGE))) {
            *entry = base | BOOT_PTE_RW | BOOT_PTE_LARGE | cache_flags;
            base += BOOT_2M;
            continue;
        }
        UINT64 *pt;
        if (*entry & BOOT_PTE_LARGE) {
            status = boot_alloc_table(&pt);
            if (EFI_ERROR(status))
                return status;
            UINT64 phys = *entry & ~(BOOT_2M - 1);
            UINT64 flags = *entry & (BOOT_PTE_RW | BOOT_PTE_UC);
            for (UINTN i = 0; i < 512; i++)
                pt[i] = (phys + i * 4096) | flags;
            *entry = (UINT64)pt | BOOT_PTE_RW;
        } else {
            status = boot_next_table(entry, &pt);
            if (EFI_ERROR(status))
                return status;
        }
        pt[(base >> 12) & 511] = base | BOOT_PTE_RW | cache_flags;
        base += 4096;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS boot_prepare_paging(const UINT64 *firmware_pml4,
                                      const void *map, UINTN map_size, UINTN stride,
                                      UINT64 fb_base, UINT64 fb_size,
                                      UINT64 apic_base, UINT64 *cr3)
{
    if (!map || !map_size || stride < sizeof(EFI_MEMORY_DESCRIPTOR) || map_size % stride)
        return EFI_INVALID_PARAMETER;
    UINT64 *root, *pdpt;
    EFI_STATUS status = boot_alloc_table(&root);
    if (EFI_ERROR(status))
        return status;
    status = boot_alloc_table(&pdpt);
    if (EFI_ERROR(status))
        return status;
    for (UINTN i = 0; i < 512; i++)
        root[i] = firmware_pml4[i];
    root[256] = (UINT64)pdpt | BOOT_PTE_RW;

    for (UINTN offset = 0; offset < map_size; offset += stride) {
        const EFI_MEMORY_DESCRIPTOR *d = (const void *)((const UINT8 *)map + offset);
        if ((d->Type < EfiLoaderCode || d->Type > EfiConventionalMemory) &&
            d->Type != EfiACPIReclaimMemory && d->Type != EfiACPIMemoryNVS)
            continue;
        if (!d->NumberOfPages)
            continue;
        if (d->NumberOfPages > BOOT_DIRECT_SIZE / 4096)
            return EFI_UNSUPPORTED;
        status = boot_map_range(pdpt, d->PhysicalStart, d->NumberOfPages * 4096, 0);
        if (EFI_ERROR(status))
            return status;
    }
    status = boot_map_range(pdpt, fb_base, fb_size, BOOT_PTE_UC);
    if (EFI_ERROR(status))
        return status;
    /* idt_init touches the local APIC before paging_init takes ownership. */
    status = boot_map_range(pdpt, apic_base, 4096, BOOT_PTE_UC);
    if (!EFI_ERROR(status))
        *cr3 = (UINT64)root;
    return status;
}

#endif
