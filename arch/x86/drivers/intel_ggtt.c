#include "intel_ggtt.h"

/* Intel Kaby Lake PRM, vol 2c: GGC/BDSM/BGSM/GTTMMADR/GMADR; vol 2d:
 * MSAC; vol 5: 64-bit GGTT PTE format. See the Intel memory discovery section
 * in docs/win32-graphics-latency.md.
 * This is the Gen9 client layout, not the server 46-bit PTE format. */
enum intel_ggtt_status intel_ggtt_decode(const intel_ggtt_config_t *c,
                                        intel_ggtt_layout_t *out)
{
    if (!c || !out) return INTEL_GGTT_BAD_ARGUMENT;
    if ((c->host_id & 0xffff) != 0x8086 ||
        (c->igd_id & 0xffff) != 0x8086)
        return INTEL_GGTT_BAD_DEVICE;
    if (!(c->command & 2)) return INTEL_GGTT_MEMORY_DISABLED;

    intel_ggtt_layout_t v = {0};
    v.mmio_base = ((uint64_t)c->bar0_hi << 32) | (c->bar0_lo & ~15u);
    v.aperture_base = ((uint64_t)c->bar2_hi << 32) | (c->bar2_lo & ~15u);
    if ((c->bar0_lo & 15) != 4 || (c->bar2_lo & 15) != 12 ||
        !v.mmio_base || !v.aperture_base ||
        v.mmio_base >= INTEL_GGTT_PHYS_LIMIT ||
        v.aperture_base >= INTEL_GGTT_PHYS_LIMIT ||
        (v.mmio_base & (INTEL_GGTT_BAR_BYTES - 1)))
        return INTEL_GGTT_BAD_BAR;

    /* Only these MSAC.APSZ encodings are legal; do not silently round up. */
    unsigned aps = c->msac & 31;
    if (aps != 0 && aps != 1 && aps != 3 && aps != 7 && aps != 15 && aps != 31)
        return INTEL_GGTT_BAD_APERTURE;
    v.aperture_bytes = ((uint64_t)aps + 1) * 128 * 1024 * 1024;
    if ((v.aperture_base & (v.aperture_bytes - 1)) ||
        v.aperture_bytes > INTEL_GGTT_PHYS_LIMIT - v.aperture_base ||
        (v.mmio_base < v.aperture_base + v.aperture_bytes &&
         v.aperture_base < v.mmio_base + INTEL_GGTT_BAR_BYTES))
        return INTEL_GGTT_BAD_BAR;

    unsigned gms = (c->ggc >> 8) & 255;
    unsigned ggms = (c->ggc >> 6) & 3;
    if (c->ggc & 0x38) return INTEL_GGTT_BAD_GGC;
    if (gms <= 0x10)
        v.stolen_bytes = (uint64_t)gms * 32 * 1024 * 1024;
    else if (gms == 0x20 || gms == 0x30 || gms == 0x40)
        v.stolen_bytes = (uint64_t)gms * 32 * 1024 * 1024;
    else if (gms >= 0xf0 && gms <= 0xfe)
        v.stolen_bytes = (uint64_t)(gms - 0xf0 + 1) * 4 * 1024 * 1024;
    else
        return INTEL_GGTT_BAD_GGC;
    if (!ggms) return INTEL_GGTT_DISABLED;
    v.ggtt_bytes = (1ULL << ggms) * 1024 * 1024;
    v.gpu_va_bytes = v.ggtt_bytes / 8 * 4096;
    v.ggtt_mmio_base = v.mmio_base + INTEL_GGTT_MMIO_OFFSET;

    /* These registers have a 1 MB-aligned address and lock bit 0. Reject
     * inaccessible/all-ones config and layouts inconsistent with firmware's
     * contiguous GSM, DSM and top-of-low-DRAM definitions. */
    if ((c->bdsm | c->bgsm | c->tolud) & 0x000ffffe)
        return INTEL_GGTT_BAD_STOLEN;
    v.stolen_base = c->bdsm & 0xfff00000u;
    v.table_stolen_base = c->bgsm & 0xfff00000u;
    uint64_t top = c->tolud & 0xfff00000u;
    if (!top || !v.stolen_base || !v.table_stolen_base ||
        v.stolen_base + v.stolen_bytes != top ||
        v.table_stolen_base + v.ggtt_bytes != v.stolen_base ||
        v.mmio_base < top || v.aperture_base < top)
        return INTEL_GGTT_BAD_STOLEN;

    *out = v;
    return INTEL_GGTT_OK;
}

bool intel_ggtt_aperture_offset(const intel_ggtt_layout_t *v, uint64_t cpu_phys,
                               uint64_t bytes, uint64_t *gpu_offset)
{
    if (!v || !gpu_offset || !bytes || cpu_phys < v->aperture_base)
        return false;
    uint64_t offset = cpu_phys - v->aperture_base;
    if (offset >= v->aperture_bytes || bytes > v->aperture_bytes - offset ||
        offset >= v->gpu_va_bytes || bytes > v->gpu_va_bytes - offset)
        return false;
    *gpu_offset = offset;
    return true;
}

bool intel_ggtt_pte_offset(const intel_ggtt_layout_t *v, uint64_t gpu_address,
                          uint32_t *bar0_offset)
{
    if (!v || !bar0_offset || gpu_address >= v->gpu_va_bytes)
        return false;
    uint64_t offset = (gpu_address >> 12) * 8;
    if (v->ggtt_bytes < 8 || offset > v->ggtt_bytes - 8 ||
        offset > INTEL_GGTT_BAR_BYTES - INTEL_GGTT_MMIO_OFFSET - 8)
        return false;
    *bar0_offset = (uint32_t)(INTEL_GGTT_MMIO_OFFSET + offset);
    return true;
}

bool intel_ggtt_decode_pte(uint64_t pte, uint64_t gpu_address,
                         uint64_t *backing_phys)
{
    if (!backing_phys || !(pte & 1)) return false;
    *backing_phys = (pte & ((INTEL_GGTT_PHYS_LIMIT - 1) & ~4095ULL)) |
                    (gpu_address & 4095);
    return true;
}
