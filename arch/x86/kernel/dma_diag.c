/* Read-only PCIe AER / Intel VT-d evidence. Architectural register layouts:
 * PCIe AER, ACPI DMAR, Intel VT-d VER/CAP/ECAP/GSTS/FSTS/FRCD.
 * Capture never acknowledges sticky state or walks hardware translation tables. */
#include "../include/dma_diag.h"
#include "../include/boot_info.h"
#include "../include/paging.h"

extern boot_info_t saved_boot_info;
extern int paging_map_mmio(uint64_t phys, uint64_t size);
extern uint64_t idt_get_monotonic_ns(void);

#define TABLE_LIMIT 65536U
#define ROOT_ENTRIES 256U
#define SUBTABLE_LIMIT 256U
#define UNIT_LIMIT 32U
#define PHYS_LIMIT (1ULL << 39)

#ifdef DMA_DIAG_HOST_TEST
extern const void *dma_diag_test_map(uint64_t phys, unsigned size);
extern uint32_t dma_diag_test_read32(uint64_t phys);
extern uint64_t dma_diag_test_read64(uint64_t phys);
#define read32(a) dma_diag_test_read32(a)
#define read64(a) dma_diag_test_read64(a)
#else
#define read32(a) mmio_read32(PHYS_TO_VIRT(a))
#define read64(a) mmio_read64(PHYS_TO_VIRT(a))
#endif

static uint16_t le16(const void *p) { uint16_t n; memcpy(&n, p, 2); return n; }
static uint32_t le32(const void *p) { uint32_t n; memcpy(&n, p, 4); return n; }
static uint64_t le64(const void *p) { uint64_t n; memcpy(&n, p, 8); return n; }

static bool range_ok(uint64_t a, uint64_t n)
{
    return a >= 4096 && n && n <= TABLE_LIMIT && a < PHYS_LIMIT && n <= PHYS_LIMIT - a;
}

/* Firmware tables must reside in retained memory. A firmware-nominated MMIO
 * hole is allowed, but overlapping RAM/ACPI/runtime ranges are not. */
static bool firmware_range(uint64_t a, unsigned n, bool table)
{
    const boot_info_t *b = &saved_boot_info;
    if (!range_ok(a, n) || b->magic != BOOT_INFO_MAGIC || !b->mmap_addr ||
        b->mmap_desc_ver != 1 || b->mmap_desc_size < 40 || b->mmap_desc_size > 256 ||
        !b->mmap_size || b->mmap_size > 8192 || b->mmap_size % b->mmap_desc_size)
        return false;
    uint64_t covered = a;
    for (unsigned pass = 0; pass < 256; pass++) {
        uint64_t next = covered;
        for (uint64_t i = 0; i < b->mmap_size; i += b->mmap_desc_size) {
            const uint8_t *d = (const uint8_t *)(uintptr_t)b->mmap_addr + i;
            uint32_t type = le32(d);
            uint64_t start = le64(d + 8), pages = le64(d + 24);
            if ((start & 4095) || !pages || pages > (UINT64_MAX - start) / 4096)
                return false;
            uint64_t end = start + pages * 4096;
            if (a >= end || a + n <= start) continue;
            bool allowed = table ? (type == 0 || type == 6 || type == 9 || type == 10)
                                 : (type == 0 || type == 11);
            if (!allowed) return false;
            if (start <= covered && end > next) next = end;
        }
        if (!table || next >= a + n) return true;
        if (next == covered) return false;
        covered = next;
    }
    return false;
}

static const uint8_t *map_phys(uint64_t a, unsigned n, bool table)
{
    if (!firmware_range(a, n, table)) return NULL;
#ifdef DMA_DIAG_HOST_TEST
    return dma_diag_test_map(a, n);
#else
    uint64_t start = a & ~4095ULL, end = (a + n + 4095) & ~4095ULL;
    if (paging_map_mmio(start, end - start)) return NULL;
    for (uint64_t p = start; p < end; p += 4096) {
        uint64_t va = (uint64_t)PHYS_TO_VIRT(p);
        if (paging_translate_in_cr3(paging_get_kernel_cr3(), va) != p) return NULL;
        __asm__ volatile ("invlpg (%0)" :: "r"(va) : "memory");
    }
    return PHYS_TO_VIRT(a);
#endif
}

static bool checksum(const uint8_t *p, unsigned n)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i < n; i++) sum += p[i];
    return sum == 0;
}

static const uint8_t *sdt(uint64_t a, const char *signature, unsigned minimum)
{
    const uint8_t *p = map_phys(a, 36, true);
    if (!p || memcmp(p, signature, 4)) return NULL;
    unsigned n = le32(p + 4);
    if (n < minimum || n > TABLE_LIMIT) return NULL;
    p = map_phys(a, n, true);
    return p && checksum(p, n) ? p : NULL;
}

static const uint8_t *find_dmar(dma_diag_t *r)
{
    const uint8_t *p = map_phys(saved_boot_info.acpi_rsdp, 20, true);
    r->dmar_note = "RSDP absent, unsafe or invalid";
    if (!p || memcmp(p, "RSD PTR ", 8) || !checksum(p, 20)) return NULL;
    uint64_t root = le32(p + 16);
    unsigned stride = 4;
    if (p[15] >= 2) {
        p = map_phys(saved_boot_info.acpi_rsdp, 36, true);
        if (!p) return NULL;
        unsigned length = le32(p + 20);
        if (length < 36 || length > 4096) return NULL;
        p = map_phys(saved_boot_info.acpi_rsdp, length, true);
        if (!p || !checksum(p, length)) return NULL;
        if (le64(p + 24)) { root = le64(p + 24); stride = 8; }
    }
    p = sdt(root, stride == 8 ? "XSDT" : "RSDT", 36);
    r->dmar_note = "ACPI root invalid or exceeds diagnostic bounds";
    if (!p) return NULL;
    unsigned n = le32(p + 4) - 36;
    if (n % stride || n / stride > ROOT_ENTRIES) return NULL;
    uint64_t address = 0;
    for (unsigned i = 0; i < n; i += stride) {
        uint64_t a = stride == 8 ? le64(p + 36 + i) : le32(p + 36 + i);
        const uint8_t *header = map_phys(a, 36, true);
        if (!header) { r->dmar_note = "unsafe ACPI child address"; return NULL; }
        if (memcmp(header, "DMAR", 4)) continue;
        if (address) { r->dmar_note = "multiple DMAR tables"; return NULL; }
        address = a;
    }
    r->dmar_note = "DMAR not present";
    if (!address) return NULL;
    p = sdt(address, "DMAR", 48);
    r->dmar_note = "DMAR length/checksum invalid";
    if (!p || p[36] > 63) return NULL;
    r->table = address;
    r->table_length = le32(p + 4);
    r->host_width = p[36] + 1;
    r->table_flags = p[37];
    return p;
}

static bool cfg(const pci_dev_t *d, unsigned off, uint32_t *v)
{
    return off <= 4092 && pci_diag_read32(d->bus, d->dev, d->func, off, v);
}

static bool identity(const pci_dev_t *d, uint32_t *id)
{
    return cfg(d, 0, id) && *id != UINT32_MAX && (*id & 65535) &&
           *id == (d->vendor_id | ((uint32_t)d->device_id << 16));
}

/* Resolve firmware PCI paths without trusting bus numbers as MMIO pointers. */
static int scope_matches(const uint8_t *s, const pci_dev_t *target, uint16_t segment)
{
    unsigned n = s[1];
    if (n < 8 || (n & 1)) return -1;
    if (s[0] != 1 && s[0] != 2) return 0;
    uint8_t bus = s[5];
    uint64_t visited[4] = {0};
    for (unsigned i = 6; i < n; i += 2) {
        uint8_t dev = s[i], fun = s[i + 1];
        uint32_t id, header, buses, class;
        if (dev > 31 || fun > 7 || pci_diag_segment(bus) != segment) return -1;
        if (visited[bus / 64] & (1ULL << (bus % 64))) return -1;
        visited[bus / 64] |= 1ULL << (bus % 64);
        if (!pci_diag_read32(bus, dev, fun, 0, &id) || id == UINT32_MAX || !(id & 65535))
            return 0; /* A firmware scope may describe an absent device. */
        bool last = i + 2 == n;
        if (last && s[0] == 1)
            return bus == target->bus && dev == target->dev && fun == target->func;
        if (!pci_diag_read32(bus, dev, fun, 0x0c, &header) ||
            !pci_diag_read32(bus, dev, fun, 8, &class) ||
            ((header >> 16) & 0x7f) != 1 || (class >> 16) != 0x0604 ||
            !pci_diag_read32(bus, dev, fun, 0x18, &buses)) return -1;
        unsigned secondary = (buses >> 8) & 255, subordinate = (buses >> 16) & 255;
        if ((buses & 255) != bus || !secondary || secondary == bus || secondary > subordinate)
            return -1;
        if (last) return target->bus >= secondary && target->bus <= subordinate;
        bus = secondary;
    }
    return -1;
}

static void prepare_dmar(dma_diag_t *r)
{
    const uint8_t *p = find_dmar(r);
    if (!p) return;
    const uint8_t *explicit_unit = NULL, *fallback = NULL;
    unsigned explicit_count = 0, all_count = 0, records = 0;
    r->dmar_note = "malformed or excessive DMAR subtables/scopes";
    for (unsigned at = 48; at < r->table_length;) {
        if (++records > SUBTABLE_LIMIT || r->table_length - at < 4) return;
        const uint8_t *u = p + at;
        unsigned length = le16(u + 2);
        if (length < 4 || length > r->table_length - at) return;
        if (le16(u) == 0) {
            if (length < 16 || ++r->units_seen > UNIT_LIMIT) return;
            bool matches = false;
            for (unsigned off = 16; off < length;) {
                if (length - off < 8 || u[off + 1] < 8 || (u[off + 1] & 1) ||
                    u[off + 1] > length - off) return;
                if (le16(u + 6) == r->segment) {
                    int match = scope_matches(u + off, &r->path[0], r->segment);
                    if (match < 0) return;
                    matches |= match != 0;
                }
                off += u[off + 1];
            }
            if (le16(u + 6) == r->segment) {
                if (matches && !(u[4] & 1)) { explicit_unit = u; explicit_count++; }
                if (u[4] & 1) { fallback = u; all_count++; }
            }
        }
        at += length;
    }
    r->dmar_note = "ambiguous DMAR device ownership";
    if (explicit_count > 1 || all_count > 1) return;
    const uint8_t *u = explicit_count ? explicit_unit : fallback;
    r->dmar_note = "no applicable DRHD for PCI requester";
    if (!u) return;
    r->base = le64(u + 8); r->unit_flags = u[4]; r->unit_size = u[5];
    r->dmar_note = "unsafe/unmappable DRHD register range";
    if (r->base < 0x100000 || (r->base & 4095) || r->unit_size > 4 ||
        !map_phys(r->base, 4096, false)) return;
    r->version = read32(r->base);
    r->cap = read64(r->base + 8); r->ecap = read64(r->base + 16);
    r->dmar_note = "DRHD version/capabilities unavailable or unsupported";
    if (!r->version || r->version == UINT32_MAX || !(r->version & 0xf0) ||
        r->cap == UINT64_MAX || !r->cap || r->ecap == UINT64_MAX) return;
    r->fault_regs = ((r->cap >> 40) & 255) + 1;
    r->fault_offset = ((r->cap >> 24) & 1023) * 16;
    unsigned end = r->fault_offset + r->fault_regs * 16;
    r->dmar_note = "invalid or unmappable DRHD fault register span";
    /* Older firmware may understate the span; CAP still bounds FRCD to 20 KiB. */
    if (r->fault_offset < 0x100 || end > 20480 || !map_phys(r->base, end, false)) return;
    r->iommu_ready = true;
    r->dmar_note = NULL;
}

static void capture_aer(const pci_dev_t *d, dma_aer_sample_t *s)
{
    s->note = "PCI function disappeared/changed";
    if (!identity(d, &s->id) || !cfg(d, 4, &s->command_status)) return;
    s->note = "PCIe capability absent";
    if (!(s->command_status & (1U << 20))) return;
    uint32_t h;
    if (!cfg(d, 0x34, &h)) return;
    unsigned pos = h & 255, pcie = 0, type = 0;
    uint64_t visited = 0;
    s->note = "invalid conventional PCI capability chain";
    while (pos) {
        if (pos < 64 || pos > 252 || (pos & 3)) return;
        uint64_t bit = 1ULL << (pos / 4 - 16);
        if (visited & bit) return;
        visited |= bit;
        if (!cfg(d, pos, &h) || h == UINT32_MAX) return;
        if ((h & 255) == 0x10) {
            if (pcie || pos > 236 || !((h >> 16) & 15)) return;
            pcie = pos; type = (h >> 20) & 15;
        }
        pos = (h >> 8) & 255;
    }
    s->note = "PCIe link capability absent/unsupported";
    if (!pcie || (type != 0 && type != 1 && type != 4 && type != 5 && type != 6)) return;
    if (!cfg(d, pcie + 8, &s->device) || !cfg(d, pcie + 16, &s->link) ||
        s->device == UINT32_MAX || s->link == UINT32_MAX) return;
    s->pcie_valid = true; s->root = type == 4;
    s->note = "extended PCI configuration unavailable";
    if (!cfg(d, 256, &h) || h == UINT32_MAX) return;
    s->note = "AER capability absent";
    if (!h) return;
    uint64_t seen[16] = {0};
    pos = 256;
    unsigned aer = 0;
    s->note = "invalid extended PCI capability chain";
    while (pos) {
        if (pos < 256 || pos > 4092 || (pos & 3)) return;
        unsigned slot = pos / 4;
        if (seen[slot / 64] & (1ULL << (slot % 64))) return;
        seen[slot / 64] |= 1ULL << (slot % 64);
        if (!cfg(d, pos, &h) || !h || h == UINT32_MAX || !((h >> 16) & 15)) return;
        if ((h & 65535) == 1) {
            if (aer) return;
            aer = pos;
        }
        pos = h >> 20;
    }
    s->note = "AER capability absent";
    if (!aer) return;
    unsigned bytes = s->root ? 56 : 44;
    s->note = "overlapping/truncated AER capability";
    if (aer > 4096 - bytes) return;
    for (unsigned off = aer + 4; off < aer + bytes; off += 4)
        if (seen[off / 256] & (1ULL << ((off / 4) % 64))) return;
    s->aer_offset = aer;
    s->note = "AER registers inaccessible";
    uint32_t *fields[] = {&s->unc_status, &s->unc_mask, &s->severity,
        &s->cor_status, &s->cor_mask, &s->control,
        &s->header[0], &s->header[1], &s->header[2], &s->header[3],
        &s->root_command, &s->root_status, &s->source};
    for (unsigned i = 0; i < (s->root ? 13U : 10U); i++)
        if (!cfg(d, aer + 4 + i * 4, fields[i])) return;
    if (s->unc_status == UINT32_MAX || s->cor_status == UINT32_MAX || s->control == UINT32_MAX)
        return;
    uint32_t id, unc, cor;
    if (!identity(d, &id) || !cfg(d, aer + 4, &unc) || !cfg(d, aer + 16, &cor) ||
        unc == UINT32_MAX || cor == UINT32_MAX) return;
    s->changed = unc != s->unc_status || cor != s->cor_status;
    s->valid = true; s->note = NULL;
}

static void capture_iommu(const dma_diag_t *r, dma_iommu_sample_t *s)
{
    if (!r->iommu_ready) return;
    uint64_t b = r->base;
    s->note = "DRHD identity/capabilities changed or inaccessible";
    if (read32(b) != r->version || read64(b + 8) != r->cap || read64(b + 16) != r->ecap)
        return;
    s->gsts = read32(b + 0x1c); s->root = read64(b + 0x20);
    s->fsts = read32(b + 0x34);
    if (s->gsts == UINT32_MAX || s->fsts == UINT32_MAX || s->root == UINT64_MAX) return;
    if (r->cap & 0x60) s->pmen = read32(b + 0x64);
    if (r->cap & 0x20) { s->low_base = read32(b + 0x68); s->low_limit = read32(b + 0x6c); }
    if (r->cap & 0x40) { s->high_base = read64(b + 0x70); s->high_limit = read64(b + 0x78); }
    for (unsigned i = 0; i < r->fault_regs; i++) {
        uint64_t a = b + r->fault_offset + i * 16;
        uint64_t hi = read64(a + 8);
        s->checked++;
        if (hi == UINT64_MAX) { s->note = "FRCD inaccessible"; return; }
        if (!(hi & (1ULL << 63))) continue;
        uint64_t low = read64(a), again = read64(a + 8);
        if (again != hi || read64(a) != low) { s->unstable++; continue; }
        bool target = (uint16_t)hi == r->requester;
        s->active++; s->target += target;
        unsigned slot = s->stored;
        if (slot == DMA_DIAG_FAULTS) {
            if (target) {
                for (slot = 0; slot < DMA_DIAG_FAULTS; slot++)
                    if (!s->faults[slot].target) break;
            }
            if (slot == DMA_DIAG_FAULTS) continue;
        } else s->stored++;
        s->faults[slot] = (dma_fault_record_t){.low = low, .high = hi, .index = i, .target = target};
    }
    s->fsts_after = read32(b + 0x34);
    uint32_t gsts = read32(b + 0x1c);
    if (s->fsts_after == UINT32_MAX || gsts == UINT32_MAX) return;
    s->changed = s->fsts_after != s->fsts || gsts != s->gsts || read64(b + 0x20) != s->root;
    s->valid = true; s->note = NULL;
}

void dma_diag_capture(dma_diag_t *r, unsigned sample)
{
    if (!r || !r->prepared || sample >= 2 || r->sample[sample].captured) return;
    dma_diag_sample_t *s = &r->sample[sample];
    s->start_ns = idt_get_monotonic_ns();
    for (unsigned i = 0; i < r->path_count; i++) capture_aer(&r->path[i], &s->aer[i]);
    capture_iommu(r, &s->iommu);
    s->end_ns = idt_get_monotonic_ns();
    s->captured = true;
}

void dma_diag_prepare(const pci_dev_t *device, dma_diag_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    uint32_t id;
    if (!device || !identity(device, &id)) return;
    r->prepared = true;
    r->segment = pci_diag_segment(device->bus);
    r->requester = ((uint16_t)device->bus << 8) | (device->dev << 3) | device->func;
    r->path[0] = *device; r->path_count = 1;
    for (;;) {
        pci_dev_t p;
        int found = pci_diag_parent(&r->path[r->path_count - 1], &p);
        if (found < 0) { r->path_note = "PCI topology incomplete/ambiguous"; break; }
        if (!found) break;
        bool repeated = false;
        for (unsigned i = 0; i < r->path_count; i++)
            repeated |= r->path[i].bus == p.bus && r->path[i].dev == p.dev && r->path[i].func == p.func;
        if (repeated || r->path_count == DMA_DIAG_PATH) {
            r->path_note = "PCI ancestry cycle/depth limit"; break;
        }
        r->path[r->path_count++] = p;
    }
    prepare_dmar(r);
    dma_diag_capture(r, 0);
}
