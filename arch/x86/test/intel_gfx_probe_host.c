/* Exercise production discovery against read-only synthetic PCI/MMIO.
 * No source seam removes privileged operations: successful init is not called
 * on the host. Its rejection paths and the actual memory probe are tested. */
#include "../include/types.h"
extern int printf(const char *, ...);
extern void abort(void);
static uint32_t probe_read32(volatile void *);
static uint64_t probe_read64(volatile void *);
#define mmio_read32 probe_read32
#define mmio_read64 probe_read64
#include "../drivers/intel_gfx.c"

static unsigned checks, reads32, reads64, config_reads, maps;
static unsigned fail_read;
static uint16_t segment;
static uint8_t aperture[16 * 1024 * 1024] __attribute__((aligned(4096)));
static uint32_t host_config[64], gpu_config[64];
static uint64_t ptes[2];
static uint64_t fences[32];
static unsigned fence_reads, dma_prepares;
static dma_diag_t dma_model;
static uint32_t change_register,change_mask,change_on_read,change_reads;
static uint32_t captured_reads[256],captured_count;
static bool capture_reads;
#define CHECK(x) do { ++checks; if (!(x)) { \
    printf("FAIL probe line %d: %s\n", __LINE__, #x); abort(); \
} } while (0)

void serial_puts(const char *s) { (void)s; }
void serial_puthex(uint64_t v, int n) { (void)v; (void)n; }
void serial_putdec(uint64_t v) { (void)v; }
void fb_puts(const char *s) { (void)s; }
void fb_putdec(uint64_t v) { (void)v; }

void dma_diag_prepare(const pci_dev_t *device, dma_diag_t *report)
{
    CHECK(device->bus == 0 && device->dev == 2 && !device->func);
    CHECK(device->vendor_id == 0x8086 && device->device_id == 0x3e9b);
    CHECK(device->bar[0] == 0x6040000000ULL && device->bar[2] == 0x4000000000ULL);
    dma_prepares++;
    *report = dma_model;
}

bool pci_diag_read32(uint8_t bus, uint8_t dev, uint8_t func,
                     uint16_t reg, uint32_t *out)
{
    CHECK(bus == 0 && (dev == 0 || dev == 2) && func == 0);
    CHECK(reg < 256 && !(reg & 3));
    if (++config_reads == fail_read) return false;
    *out = (dev == 0 ? host_config : gpu_config)[reg / 4];
    return true;
}

uint16_t pci_diag_segment(uint8_t bus)
{
    CHECK(bus == 0);
    return segment;
}

int paging_map_mmio(uint64_t phys, uint64_t bytes)
{
    CHECK(phys == 0x6040000000ULL && bytes == sizeof(aperture));
    maps++;
    return -1; /* Test that a failed mapping prevents ALL MMIO access. */
}

static uint32_t probe_read32(volatile void *address)
{
    uint64_t offset = (uint64_t)address - (uint64_t)aperture;
    CHECK(offset < sizeof(aperture) - 3 && !(offset & 3));
    reads32++;
    if (capture_reads) {
        CHECK(captured_count<256); captured_reads[captured_count++]=(uint32_t)offset;
    }
    uint32_t value; memcpy(&value,aperture+offset,sizeof(value));
    if (offset==change_register && ++change_reads>=change_on_read) value^=change_mask;
    return value;
}

static uint64_t probe_read64(volatile void *address)
{
    uint64_t offset = (uint64_t)address - (uint64_t)aperture;
    if (offset >= 0x100000 && offset < 0x100100) {
        CHECK(!(offset & 7));
        fence_reads++;
        return fences[(offset - 0x100000) / 8];
    }
    CHECK(offset == 0x800000 || offset == 0x803f40);
    CHECK(++reads64 <= 2);
    return ptes[offset != 0x800000];
}

static void probe_reset(void)
{
    memset(&igfx, 0, sizeof(igfx));
    memset(host_config, 0, sizeof(host_config));
    memset(gpu_config, 0, sizeof(gpu_config));
    memset(aperture+0x40000,0,0x40000);
    *(uint32_t *)(aperture+0x70180)=0x84000000; /* Incomplete historical P1 sample. */
    change_register=UINT32_MAX; change_mask=change_on_read=change_reads=0;
    capture_reads=false; captured_count=0;
    reads32 = reads64 = config_reads = maps = fail_read = segment = 0;
    fence_reads = dma_prepares = 0;
    memset(fences, 0, sizeof(fences));
    memset(&dma_model, 0, sizeof(dma_model));
    dma_model.prepared = dma_model.iommu_ready = true;
    dma_model.sample[0].captured = dma_model.sample[0].iommu.valid = true;
    igfx.vendor_id = 0x8086;
    igfx.device_id = 0x3e9b;
    igfx.dev = 2;
    igfx.bar0_phys = 0x6040000000ULL;
    igfx.bar2_phys = igfx.gop_fb_phys = 0x4000000000ULL;
    igfx.gop_width = 1920;
    igfx.gop_height = 1080;
    igfx.gop_pitch_bytes = 7680;
    igfx_mmio = aperture;
    host_config[0] = 0x3ec48086;
    host_config[0x50 / 4] = 0x0281;
    host_config[0xb0 / 4] = 0xbc000001;
    host_config[0xb4 / 4] = 0xbbc00001;
    host_config[0xbc / 4] = 0xc0000001;
    gpu_config[0] = 0x3e9b8086;
    gpu_config[1] = 2; /* CPU memory decoding only; probe must not enable DMA. */
    gpu_config[0x10 / 4] = 0x40000004;
    gpu_config[0x14 / 4] = 0x60;
    gpu_config[0x18 / 4] = 12;
    gpu_config[0x1c / 4] = 0x40;
    gpu_config[0x60 / 4] = 0x00010000;
    ptes[0] = 0xbc000001;
    ptes[1] = 0xbc7e8001;
}

static void probe_check(void)
{
    uint32_t h[64], g[64];
    memcpy(h, host_config, sizeof(h));
    memcpy(g, gpu_config, sizeof(g));
    igfx_probe_memory();
    CHECK(!memcmp(h, host_config, sizeof(h)));
    CHECK(!memcmp(g, gpu_config, sizeof(g)));
    CHECK(!igfx.ready && !igfx.gop_scanout_retained);
}

int main(void)
{
    probe_reset();
    probe_check();
    CHECK(config_reads == 12 && reads64 == 2 && !reads32);
    CHECK(igfx.memory_valid && igfx.gop_in_aperture && igfx.gop_sample_present);
    CHECK(igfx.fence_snapshot_valid && !igfx.fence_valid_mask && fence_reads == 64);
    igfx_probe_dma();
    CHECK(dma_prepares == 1 && igfx.dma_state_valid && igfx.dma_identity_confirmed);
    CHECK(igfx.memory.ggtt_bytes == 0x400000);
    CHECK(igfx.memory.aperture_bytes == 0x10000000);
    CHECK(igfx.gop_first_phys == 0xbc000000 && igfx.gop_last_phys == 0xbc7e8fff);
    CHECK(igfx_find_active_scanout() == -1);
    CHECK(igfx.memory_valid && !igfx.ready && !igfx.gop_scanout_retained);
    for (unsigned i = 1; i <= 12; i++) {
        probe_reset(); fail_read = i;
        probe_check();
        CHECK(config_reads == i && !reads64 && !igfx.memory_valid);
    }
    probe_reset(); igfx.device_id = 0x1912;
    probe_check(); CHECK(!config_reads && !reads64 && !igfx.memory_valid);
    probe_reset(); igfx.bus = 1;
    probe_check(); CHECK(!config_reads && !reads64);
    probe_reset(); segment = 7;
    probe_check(); CHECK(!config_reads && !reads64);
    probe_reset(); gpu_config[0x60 / 4] = 0x00020000;
    probe_check(); CHECK(!reads64 && !igfx.memory_valid);
    probe_reset(); host_config[0xb4 / 4] = 0;
    probe_check(); CHECK(!reads64 && !igfx.memory_valid);
    probe_reset(); gpu_config[0] = 0x3e928086;
    probe_check(); CHECK(!reads64 && !igfx.memory_valid);
    probe_reset(); igfx.bar0_phys += 0x1000000;
    probe_check(); CHECK(!reads64 && !igfx.memory_valid);
    probe_reset(); igfx.bar2_phys += 0x10000000;
    probe_check(); CHECK(!reads64 && !igfx.memory_valid);
    probe_reset(); igfx.gop_fb_phys--;
    probe_check(); CHECK(igfx.memory_valid && !igfx.gop_in_aperture && !reads64);
    probe_reset(); igfx.gop_fb_phys += 0x10000000 - 4096;
    probe_check(); CHECK(!igfx.gop_in_aperture && !reads64);
    probe_reset(); igfx.gop_height = 0;
    probe_check(); CHECK(!igfx.gop_in_aperture && !reads64);
    probe_reset(); igfx.gop_width = 1921;
    probe_check(); CHECK(!igfx.gop_in_aperture && !reads64);
    probe_reset(); ptes[0] = UINT64_MAX;
    probe_check(); CHECK(reads64 == 2 && !igfx.gop_sample_present);
    probe_reset(); ptes[1] = 0;
    probe_check(); CHECK(reads64 == 2 && !igfx.gop_sample_present);
    probe_reset(); fences[3] = (0x007ff000ULL << 32) | (63ULL << 32) | 1;
    probe_check(); CHECK(igfx.fence_snapshot_valid && igfx.fence_valid_mask == 8);
    probe_reset(); fences[0] = UINT64_MAX;
    probe_check(); CHECK(!igfx.fence_snapshot_valid);
    probe_reset(); fences[0] = (1ULL << 43) | 1;
    probe_check(); CHECK(!igfx.fence_snapshot_valid);
    probe_reset(); fences[0] = 0x100001;
    probe_check(); CHECK(!igfx.fence_snapshot_valid); /* reversed bounds */
    for (unsigned i = 0; i < 7; i++) {
        probe_reset(); igfx.memory_valid = true;
        switch (i) {
        case 0: dma_model.sample[0].iommu.gsts = 1u << 31; break;
        case 1: dma_model.sample[0].iommu.pmen = 1; break;
        case 2: dma_model.sample[0].iommu.pmen = 1u << 31; break;
        case 3: dma_model.sample[0].iommu.valid = false; break;
        case 4: dma_model.sample[0].iommu.changed = true; break;
        case 5: dma_model.iommu_ready = false; break;
        case 6: dma_model.path_note = "ambiguous topology"; break;
        }
        igfx_probe_dma();
        CHECK(dma_prepares == 1 && !igfx.dma_identity_confirmed);
        CHECK(igfx.dma_state_valid == (i < 3));
    }
    probe_reset(); igfx_probe_dma(); CHECK(!dma_prepares);

    /* No host-test seam in init: all these calls must return before CR3 and
     * the actual kernel mapping are used. Sanitizers catch an accidental read. */
    for (unsigned i = 0; i < 8; i++) {
        probe_reset();
        switch (i) {
        case 0: break; /* mapping fails */
        case 1: gpu_config[0] = UINT32_MAX; break;
        case 2: gpu_config[1] = 4; break;
        case 3: gpu_config[0x10 / 4] = 0x50000004; break;
        case 4: gpu_config[0x10 / 4] = 0x4000000c; break;
        case 5: fail_read = 1; break;
        case 6: fail_read = 3; break;
        case 7: fail_read = 4; break;
        }
        CHECK(intel_gfx_init(0x3e9b, 0, 2, 0, 0x6040000000ULL, 0x4000000000ULL,
                              0x4000000000ULL, 1920, 1080, 7680) == -1);
        CHECK(!reads64 && !reads32 && !igfx_mmio && !igfx.ready && !igfx.memory_valid);
        CHECK(maps == (i == 0 ? 1u : 0u));
    }
    printf("Intel probe: %u checks, 0 failures (synthetic PCI/MMIO)\n", checks);
    return 0;
}
