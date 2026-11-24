#define DMA_DIAG_HOST_TEST 1
#include "../kernel/dma_diag.c"

extern int printf(const char *format, ...);
extern void abort(void);
boot_info_t saved_boot_info;

#define ACPI_BASE 0x200000ULL
#define UNIT_BASE 0xfed90000ULL
static uint8_t acpi[65536], registers[24576], memory_map[120];
static uint32_t config[2][1024];
static pci_dev_t endpoint = {.bus = 0x52, .vendor_id = 0x8086, .device_id = 0x2723,
                             .class_code = 2, .subclass = 0x80};
static pci_dev_t bridge = {.bus = 0, .dev = 0x1d, .func = 6,
                           .vendor_id = 0x8086, .device_id = 0xa336, .class_code = 6, .subclass = 4};
static unsigned checks, failures, maps, mmio_reads, cfg_reads, mapped_end;
static bool no_extended, fail_map, absent, bad_parent, cycle, changing_aer;
static int unstable_slot;
static unsigned unstable_reads;
static uint64_t now;
static dma_diag_t report;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = v >> (8 * i);
}
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, v); put32(p + 4, v >> 32);
}
static void seal(uint8_t *p, unsigned n, unsigned at)
{
    p[at] = 0;
    uint8_t sum = 0;
    for (unsigned i = 0; i < n; i++) sum += p[i];
    p[at] = 0 - sum;
}
static void seal_dmar(unsigned size)
{
    put32(acpi + 0x2004, size); seal(acpi + 0x2000, size, 9);
}
static void memory_descriptor(unsigned i, uint32_t type, uint64_t base, uint64_t pages)
{
    uint8_t *p = memory_map + i * 40;
    put32(p, type); put64(p + 8, base); put64(p + 24, pages);
}
static void fixture(void)
{
    memset(acpi, 0, sizeof(acpi)); memset(registers, 0, sizeof(registers));
    memset(config, 0, sizeof(config)); memset(memory_map, 0, sizeof(memory_map));
    memset(&saved_boot_info, 0, sizeof(saved_boot_info));
    maps = mmio_reads = cfg_reads = mapped_end = unstable_reads = 0;
    no_extended = fail_map = absent = bad_parent = cycle = changing_aer = false;
    unstable_slot = -1; now = 0;
    memory_descriptor(0, 9, ACPI_BASE, 16);
    memory_descriptor(1, 0, UNIT_BASE, 6);
    memory_descriptor(2, 7, 0x10000000, 4096);
    saved_boot_info.magic = BOOT_INFO_MAGIC;
    saved_boot_info.mmap_addr = (uint64_t)memory_map;
    saved_boot_info.mmap_size = sizeof(memory_map);
    saved_boot_info.mmap_desc_size = 40;
    saved_boot_info.mmap_desc_ver = 1;
    saved_boot_info.acpi_rsdp = ACPI_BASE;
    memcpy(acpi, "RSD PTR ", 8); acpi[15] = 2;
    put32(acpi + 20, 36); put64(acpi + 24, ACPI_BASE + 4096);
    seal(acpi, 20, 8); seal(acpi, 36, 32);
    memcpy(acpi + 4096, "XSDT", 4); put32(acpi + 4100, 44);
    put64(acpi + 4096 + 36, ACPI_BASE + 8192); seal(acpi + 4096, 44, 9);
    uint8_t *d = acpi + 8192;
    memcpy(d, "DMAR", 4); d[8] = 1; d[36] = 47; d[37] = 1;
    d[50] = 16; d[52] = 1; put64(d + 56, UNIT_BASE); seal_dmar(64);
    put32(registers, 0x10);
    put64(registers + 8, (255ULL << 40) | (32U << 24) | 0x460);
    put64(registers + 16, 1);
    put32(registers + 0x1c, 1U << 31); put64(registers + 0x20, 0x12345000);
    put32(registers + 0x64, 1); put32(registers + 0x68, 0x10000000);
    put32(registers + 0x6c, 0x11ffffff);
    for (unsigned i = 0; i < 2; i++) {
        config[i][0] = i ? 0xa3368086 : 0x27238086;
        config[i][1] = 0x100006;
        config[i][2] = i ? 0x06040000 : 0x02800000;
        config[i][3] = i ? 0x00010000 : 0;
        config[i][0x34 / 4] = 0x40;
        config[i][0x40 / 4] = i ? 0x00420010 : 0x00020010;
        config[i][0x48 / 4] = 0x190c10;
        config[i][0x50 / 4] = 0x10120142;
        config[i][0x100 / 4] = 0x00010001;
        config[i][0x108 / 4] = 0x100000;
        config[i][0x10c / 4] = 0x4000;
        config[i][0x114 / 4] = 0x2000;
    }
    config[1][0x18 / 4] = 0x00525200;
}

const void *dma_diag_test_map(uint64_t a, unsigned n)
{
    maps++;
    if (fail_map) return NULL;
    if (a >= ACPI_BASE && a - ACPI_BASE + n <= sizeof(acpi)) return acpi + (a - ACPI_BASE);
    if (a == UNIT_BASE && n <= sizeof(registers)) {
        if (n > mapped_end) mapped_end = n;
        return registers;
    }
    abort();
}
uint32_t dma_diag_test_read32(uint64_t a)
{
    mmio_reads++;
    if (a < UNIT_BASE || (a & 3) || a - UNIT_BASE + 4 > mapped_end) abort();
    return le32(registers + (a - UNIT_BASE));
}
uint64_t dma_diag_test_read64(uint64_t a)
{
    mmio_reads++;
    if (a < UNIT_BASE || (a & 7) || a - UNIT_BASE + 8 > mapped_end) abort();
    uint64_t v = le64(registers + (a - UNIT_BASE));
    if (unstable_slot >= 0 && a == UNIT_BASE + 0x208 + (unsigned)unstable_slot * 16)
        v ^= (uint64_t)(unstable_reads++ & 1) << 62;
    return v;
}
uint64_t idt_get_monotonic_ns(void) { return now += 1000; }
uint16_t pci_diag_segment(uint8_t b) { (void)b; return 0; }
bool pci_diag_read32(uint8_t b, uint8_t d, uint8_t f, uint16_t off, uint32_t *v)
{
    cfg_reads++;
    if (!v || d > 31 || f > 7 || (off & 3) || off > 4092) abort();
    if (off >= 256 && no_extended) return false;
    unsigned slot;
    if (b == 0x52 && d == 0 && f == 0) slot = 0;
    else if (b == 0 && d == 0x1d && f == 6) slot = 1;
    else { *v = UINT32_MAX; return true; }
    *v = absent && !slot ? UINT32_MAX : config[slot][off / 4];
    if (changing_aer && !slot && off == 0x104) config[slot][off / 4] ^= 0x4000;
    return true;
}
int pci_diag_parent(const pci_dev_t *c, pci_dev_t *p)
{
    if (bad_parent) return -1;
    if (c->bus == 0x52) { *p = bridge; return 1; }
    if (cycle) { *p = endpoint; return 1; }
    return 0;
}

static void fault(unsigned i, uint16_t sid, unsigned reason)
{
    put64(registers + 0x200 + i * 16, 0x10901000);
    put64(registers + 0x208 + i * 16, (1ULL << 63) | ((uint64_t)reason << 32) | sid);
}
static void test_capture(void)
{
    fixture();
    uint8_t old_regs[sizeof(registers)];
    uint32_t old_pci[2][1024];
    memcpy(old_regs, registers, sizeof(registers)); memcpy(old_pci, config, sizeof(config));
    dma_diag_prepare(&endpoint, &report);
    CHECK(report.prepared && report.iommu_ready && !report.dmar_note && !report.path_note);
    CHECK(report.path_count == 2 && report.requester == 0x5200 && report.segment == 0);
    CHECK(report.base == UNIT_BASE && report.fault_regs == 256 && report.fault_offset == 512);
    CHECK(report.sample[0].aer[0].valid && report.sample[0].aer[1].valid);
    CHECK(!report.sample[0].aer[0].root && report.sample[0].aer[1].root);
    CHECK(report.sample[0].iommu.valid && report.sample[0].iommu.checked == 256);
    CHECK(!memcmp(old_regs, registers, sizeof(registers)) && !memcmp(old_pci, config, sizeof(config)));
    unsigned mappings = maps;
    config[0][0x104 / 4] = 0x4000; config[0][0x118 / 4] = 14;
    config[0][0x11c / 4] = 0x20000001;
    config[1][0x130 / 4] = 0x44; config[1][0x134 / 4] = 0x52000000;
    put32(registers + 0x34, 2); fault(255, 0x5200, 6); fault(10, 0x1234, 5);
    memcpy(old_regs, registers, sizeof(registers)); memcpy(old_pci, config, sizeof(config));
    dma_diag_capture(&report, 1);
    CHECK(maps == mappings && report.sample[1].captured);
    CHECK(report.sample[0].aer[0].unc_status == 0 && report.sample[1].aer[0].unc_status == 0x4000);
    CHECK(report.sample[1].aer[0].header[0] == 0x20000001);
    CHECK(report.sample[1].aer[1].root_status == 0x44 && report.sample[1].aer[1].source == 0x52000000);
    const dma_iommu_sample_t *s = &report.sample[1].iommu;
    CHECK(s->valid && s->active == 2 && s->stored == 2 && s->target == 1 && !s->unstable);
    CHECK(s->faults[1].index == 255 && s->faults[1].target && (s->faults[1].high >> 32 & 255) == 6);
    CHECK(s->root == 0x12345000 && s->gsts == (1U << 31) && s->pmen == 1);
    CHECK(!memcmp(old_regs, registers, sizeof(registers)) && !memcmp(old_pci, config, sizeof(config)));
    unsigned reads = mmio_reads + cfg_reads;
    dma_diag_capture(&report, 0); dma_diag_capture(&report, 1); dma_diag_capture(&report, 2);
    CHECK(reads == mmio_reads + cfg_reads);

    fixture(); for (unsigned i = 0; i < 256; i++) fault(i, i == 255 ? 0x5200 : 0x100, 2);
    dma_diag_prepare(&endpoint, &report); s = &report.sample[0].iommu;
    CHECK(s->active == 256 && s->stored == 16 && s->target == 1 && s->faults[0].index == 255);
    fixture(); fault(5, 0x5200, 6); unstable_slot = 5;
    dma_diag_prepare(&endpoint, &report);
    CHECK(report.sample[0].iommu.valid && report.sample[0].iommu.unstable == 1);
    CHECK(!report.sample[0].iommu.active && !report.sample[0].iommu.stored);
    fixture(); dma_diag_prepare(&endpoint, &report);
    put32(registers, 0xffffffff); reads = mmio_reads;
    dma_diag_capture(&report, 1);
    CHECK(!report.sample[1].iommu.valid && report.sample[1].iommu.note && mmio_reads == reads + 1);
    fixture(); put32(registers + 0x1c, 0); dma_diag_prepare(&endpoint, &report);
    CHECK(report.sample[0].iommu.valid && !report.sample[0].iommu.gsts);
    fixture(); dma_diag_prepare(NULL, &report); CHECK(!report.prepared && !maps && !cfg_reads);
    absent = true; dma_diag_prepare(&endpoint, &report); CHECK(!report.prepared && !maps);
}

static void test_acpi_guards(void)
{
    for (unsigned which = 0; which < 17; which++) {
        fixture(); uint8_t *d = acpi + 8192;
        switch (which) {
        case 0: acpi[8]++; break;
        case 1: acpi[32]++; break;
        case 2: acpi[4096 + 9]++; break;
        case 3: d[9]++; break;
        case 4: put32(d + 4, 65537); break;
        case 5: put32(acpi + 20, 35); break;
        case 6: saved_boot_info.mmap_size--; break;
        case 7: memory_descriptor(0, 7, ACPI_BASE, 16); break;
        case 8: d[50] = 0; seal_dmar(64); break;
        case 9: d[50] = 15; seal_dmar(64); break;
        case 10: put64(d + 56, UNIT_BASE + 1); seal_dmar(64); break;
        case 11: put64(d + 56, 0x10000000); seal_dmar(64); break;
        case 12: put64(d + 56, PHYS_LIMIT); seal_dmar(64); break;
        case 13: d[52] = 0; seal_dmar(64); break;
        case 14: d[54] = 1; seal_dmar(64); break;
        case 15: memcpy(d + 64, d + 48, 16); seal_dmar(80); break;
        case 16: fail_map = true; break;
        }
        dma_diag_prepare(&endpoint, &report);
        CHECK(report.prepared && !report.iommu_ready && report.dmar_note && !mmio_reads);
        CHECK(report.sample[0].aer[0].valid);
    }
    fixture(); put64(registers + 8, UINT64_MAX);
    dma_diag_prepare(&endpoint, &report); CHECK(!report.iommu_ready && report.dmar_note);
    fixture(); put64(registers + 8, 0x400);
    dma_diag_prepare(&endpoint, &report); CHECK(!report.iommu_ready && report.dmar_note);
    fixture();
    acpi[15] = 0; put32(acpi + 16, ACPI_BASE + 4096); seal(acpi, 20, 8);
    memcpy(acpi + 4096, "RSDT", 4); put32(acpi + 4100, 40);
    put32(acpi + 4096 + 36, ACPI_BASE + 8192); seal(acpi + 4096, 40, 9);
    dma_diag_prepare(&endpoint, &report); CHECK(report.iommu_ready);
    fixture(); memset(acpi + 8192, 0, 4);
    dma_diag_prepare(&endpoint, &report); CHECK(!report.iommu_ready && !strcmp(report.dmar_note, "DMAR not present"));
}

static void test_scopes(void)
{
    for (unsigned which = 0; which < 7; which++) {
        fixture(); uint8_t *d = acpi + 8192;
        d[52] = 0; d[50] = 24;
        d[64] = 1; d[65] = 8; d[69] = 0x52;
        switch (which) {
        case 0: break;
        case 1: d[64] = 2; d[69] = 0; d[70] = 0x1d; d[71] = 6; break;
        case 2: d[50] = 26; d[65] = 10; d[69] = 0; d[70] = 0x1d; d[71] = 6; break;
        case 3: d[65] = 7; break;
        case 4: d[70] = 32; break;
        case 5: d[70] = 1; break;
        case 6: d[64] = 2; d[69] = 0; d[70] = 0x1d; d[71] = 6; config[1][6] = 0; break;
        }
        seal_dmar(48 + d[50]);
        dma_diag_prepare(&endpoint, &report);
        CHECK(report.iommu_ready == (which < 3));
        if (which >= 3) CHECK(report.dmar_note && !mmio_reads);
    }
    fixture(); uint8_t *d = acpi + 8192;
    memcpy(d + 64, d + 48, 16); put64(d + 56, UNIT_BASE + 4096);
    d[66] = 24; d[68] = 0; d[80] = 1; d[81] = 8; d[85] = 0x52;
    seal_dmar(88); dma_diag_prepare(&endpoint, &report);
    CHECK(report.iommu_ready && report.base == UNIT_BASE && report.units_seen == 2);
}

static void test_aer_guards(void)
{
    for (unsigned which = 0; which < 8; which++) {
        fixture();
        switch (which) {
        case 0: no_extended = true; break;
        case 1: config[0][0x100 / 4] |= 0x10000000; break;
        case 2: config[0][0x100 / 4] = 0xffc10002; config[0][0xffc / 4] = 0x10001; break;
        case 3: config[0][0x100 / 4] |= 0x12000000; config[0][0x120 / 4] = 0x10002; break;
        case 4: config[0][0x40 / 4] |= 0x4000; break;
        case 5: config[0][0x100 / 4] = 0; break;
        case 6: config[0][0x100 / 4] = UINT32_MAX; break;
        case 7: config[0][0x104 / 4] = UINT32_MAX; break;
        }
        dma_diag_prepare(&endpoint, &report);
        CHECK(!report.sample[0].aer[0].valid && report.sample[0].aer[0].note);
        CHECK(report.iommu_ready && cfg_reads < 200);
    }
    fixture(); changing_aer = true; dma_diag_prepare(&endpoint, &report);
    CHECK(report.sample[0].aer[0].valid && report.sample[0].aer[0].changed);
    fixture(); bad_parent = true; dma_diag_prepare(&endpoint, &report);
    CHECK(report.path_count == 1 && report.path_note && report.iommu_ready);
    fixture(); cycle = true; dma_diag_prepare(&endpoint, &report);
    CHECK(report.path_count == 2 && report.path_note);
    fixture(); dma_diag_prepare(&endpoint, &report); absent = true;
    dma_diag_capture(&report, 1); CHECK(!report.sample[1].aer[0].valid);
    CHECK(report.sample[0].aer[0].valid);
}

static void test_bounds(void)
{
    fixture();
    CHECK(!firmware_range(UINT64_MAX - 4095, 8192, false));
    CHECK(!firmware_range(UNIT_BASE, 65537, false));
    CHECK(!firmware_range(0, 36, true));
    CHECK(!firmware_range(0x10000000, 4096, false));
    CHECK(firmware_range(UNIT_BASE + sizeof(registers), 4096, false));
    memory_descriptor(1, 7, UNIT_BASE + 4096, 1);
    CHECK(!firmware_range(UNIT_BASE, 4608, false));
    fixture(); saved_boot_info.mmap_desc_size = 0;
    CHECK(!firmware_range(UNIT_BASE, 4096, false));
    fixture(); put64(memory_map + 24, UINT64_MAX);
    CHECK(!firmware_range(UNIT_BASE, 4096, false));
    fixture(); put32(acpi + 4100, 36 + 257 * 8); seal(acpi + 4096, 36 + 257 * 8, 9);
    dma_diag_prepare(&endpoint, &report); CHECK(!report.iommu_ready && !mmio_reads);
    fixture(); put32(acpi + 4100, 52); put64(acpi + 4096 + 44, ACPI_BASE + 8192);
    seal(acpi + 4096, 52, 9); dma_diag_prepare(&endpoint, &report);
    CHECK(!report.iommu_ready && !mmio_reads && !strcmp(report.dmar_note, "multiple DMAR tables"));
    fixture(); uint8_t *d = acpi + 8192;
    for (unsigned i = 0; i < 33; i++) {
        memcpy(d + 48 + i * 16, d + 48, 16);
        d[48 + i * 16 + 6] = 1;
    }
    seal_dmar(48 + 33 * 16); dma_diag_prepare(&endpoint, &report);
    CHECK(!report.iommu_ready && !mmio_reads && report.units_seen == 33);
    fixture(); dma_diag_prepare(&endpoint, &report);
    put32(registers + 0x34, UINT32_MAX); dma_diag_capture(&report, 1);
    CHECK(!report.sample[1].iommu.valid && !report.sample[1].iommu.checked);
    fixture(); dma_diag_prepare(&endpoint, &report);
    put64(registers + 0x208, UINT64_MAX); dma_diag_capture(&report, 1);
    CHECK(!report.sample[1].iommu.valid && report.sample[1].iommu.checked == 1);
    fixture();
    for (unsigned off = 0x100; off < 0x1000; off += 4)
        config[0][off / 4] = ((off == 0xffc ? 0 : off + 4) << 20) | 0x10002;
    dma_diag_prepare(&endpoint, &report);
    CHECK(!report.sample[0].aer[0].valid && cfg_reads < 1100);
}

int main(void)
{
    test_capture(); test_acpi_guards(); test_scopes(); test_aer_guards();
    test_bounds();
    printf("DMA/AER diagnostics: %u checks, %u failures; report=%u bytes\n",
           checks, failures, (unsigned)sizeof(dma_diag_t));
    return failures ? 1 : 0;
}
