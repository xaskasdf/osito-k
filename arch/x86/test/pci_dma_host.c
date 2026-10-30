#include "../include/types.h"
extern void pci_test_outl(uint16_t port, uint32_t value);
extern uint32_t pci_test_inl(uint16_t port);
#define outl pci_test_outl
#define inl pci_test_inl
#define PCI_HOST_TEST 1
#include "../kernel/pci.c"

extern int printf(const char *, ...);
extern void abort(void);
static unsigned checks, failures, violations, writes, ndev, selected;
static bool irq_enabled = true, reenter, quiescing;
static int blocked, vanish, corrupt, change_path, drop_parent;
static struct { uint32_t bdf, cfg[64]; } mock[64];
static uint32_t write_order[64];
static uint8_t mmconfig[4U << 20] __attribute__((aligned(4096)));
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

uint64_t pci_test_irq_save(void)
{
    uint64_t flags = irq_enabled ? 512 : 0;
    irq_enabled = false;
    return flags;
}
void pci_test_irq_restore(uint64_t flags)
{
    if (pci_io_lock) violations++;
    irq_enabled = !!(flags & 512);
}
static int selected_device(void)
{
    for (unsigned i = 0; i < ndev; i++)
        if ((selected & 0xffff00) == mock[i].bdf) return i;
    return -1;
}
void pci_test_outl(uint16_t port, uint32_t value)
{
    if (irq_enabled || !pci_io_lock) violations++;
    if (port != 0xcf8) { violations++; return; }
    selected = value;
}
uint32_t pci_test_inl(uint16_t port)
{
    if (irq_enabled || !pci_io_lock || port != 0xcfc) violations++;
    int idx = selected_device();
    return idx < 0 ? UINT32_MAX : mock[idx].cfg[(selected & 255) / 4];
}
void pci_test_outw(uint16_t port, uint16_t value)
{
    int idx = selected_device();
    if (irq_enabled || !pci_io_lock || port != 0xcfc || (selected & 255) != 4 || idx < 0)
        abort();
    uint32_t *cfg = mock[idx].cfg;
    if ((!quiescing && ((cfg[2] >> 16) != 0x0604 || value != (uint16_t)(cfg[1] | 4))) ||
        (quiescing && ((cfg[2] >> 24) == 6 || value != (uint16_t)(cfg[1] & ~4U))) || writes == 64)
        violations++;
    write_order[writes++] = mock[idx].bdf;
    if (idx != blocked) cfg[1] = (cfg[1] & 0xffff0000) | value;
    if (idx == vanish) cfg[0] = UINT32_MAX;
    if (idx == corrupt) cfg[1] &= ~2U;
    if (writes == 1 && change_path >= 0) mock[change_path].cfg[6] ^= 0x10000;
    if (writes == 2 && drop_parent >= 0) mock[drop_parent].cfg[1] &= ~4U;
    if (reenter) {
        pci_dma_path_t nested;
        CHECK(pci_prepare_dma_path(&pci_devices[0], &nested) != NULL);
        CHECK(!nested.ready && !strcmp(nested.error, "PCI DMA path preparation busy"));
    }
}
static void reset(void)
{
    CHECK(!pci_dma_lock && !pci_io_lock && irq_enabled);
    memset(mock, 0, sizeof(mock));
    memset(pci_devices, 0, sizeof(pci_devices));
    pci_device_count = ndev = writes = violations = 0;
    ecam_base = ecam_segment = ecam_start_bus = ecam_end_bus = 0;
    blocked = vanish = corrupt = change_path = drop_parent = -1;
    reenter = quiescing = false;
}
static unsigned add(unsigned bus, unsigned dev, unsigned func, bool bridge,
                    unsigned secondary, unsigned subordinate)
{
    unsigned i = ndev++;
    if (i >= 64) abort();
    pci_devices[i] = (pci_dev_t){.bus = bus, .dev = dev, .func = func,
        .vendor_id = 0x1234, .device_id = 0x5000 + i,
        .class_code = bridge ? 6 : 2, .subclass = bridge ? 4 : 0};
    pci_device_count = ndev;
    mock[i].bdf = (bus << 16) | (dev << 11) | (func << 8);
    mock[i].cfg[0] = 0x1234 | ((0x5000 + i) << 16);
    mock[i].cfg[1] = 0xf9100543; /* W1C status and unrelated Command bits. BME off. */
    mock[i].cfg[2] = bridge ? 0x06040000 : 0x02000000;
    mock[i].cfg[3] = bridge ? 0x10000 : 0;
    if (bridge) mock[i].cfg[6] = bus | (secondary << 8) | (subordinate << 16);
    return i;
}
static void chain(void)
{
    reset();
    add(3, 0, 0, false, 0, 0); /* endpoint */
    add(2, 2, 1, true, 3, 3);  /* nearest bridge */
    add(0, 4, 2, true, 2, 6);  /* root bridge */
    add(3, 1, 0, false, 0, 0); /* another child, never modified */
    mock[3].cfg[1] |= 4;
}
static void test_success(void)
{
    pci_dma_path_t r;
    chain(); reenter = true;
    CHECK(pci_prepare_dma_path(&pci_devices[0], &r) == NULL);
    CHECK(r.ready && r.attempted && r.count == 2 && r.segment == 0);
    CHECK(writes == 2 && write_order[0] == mock[2].bdf && write_order[1] == mock[1].bdf);
    CHECK(mock[0].cfg[1] == 0xf9100543 && mock[3].cfg[1] == 0xf9100547);
    for (unsigned i = 0; i < 2; i++) {
        CHECK(r.bridge[i].before == 0x543 && r.bridge[i].after == 0x547);
        CHECK(r.bridge[i].written && r.bridge[i].verified);
        CHECK(mock[i + 1].cfg[1] == 0xf9100547);
    }
    CHECK(!violations);
    CHECK(pci_prepare_dma_path(&pci_devices[0], &r) == NULL && writes == 2);
    CHECK(!r.bridge[0].written && !r.bridge[1].written && r.ready);
    mock[3].cfg[1] &= ~4U;
    CHECK(pci_prepare_dma_path(&pci_devices[3], &r) == NULL && writes == 2);
    CHECK(r.count == 2 && r.ready && !violations);
    /* A new client/stop does not acquire ownership of bridge BME to clear it. */
    CHECK((mock[1].cfg[1] & 4) && (mock[2].cfg[1] & 4));

    reset(); add(0, 7, 0, false, 0, 0);
    CHECK(pci_prepare_dma_path(&pci_devices[0], &r) == NULL);
    CHECK(r.ready && r.count == 0 && writes == 0);
    chain();
    memset(mmconfig, 0xff, sizeof(mmconfig));
    for (unsigned i = 0; i < ndev; i++) {
        const pci_dev_t *p = &pci_devices[i];
        memcpy(mmconfig + ((unsigned)p->bus << 20) + ((unsigned)p->dev << 15) +
               ((unsigned)p->func << 12), mock[i].cfg, sizeof(mock[i].cfg));
    }
    ecam_base = (uint64_t)mmconfig; ecam_end_bus = 3; ecam_segment = 7;
    CHECK(pci_prepare_dma_path(&pci_devices[0], &r) == NULL && r.segment == 7);
    CHECK(r.ready && r.count == 2 && !writes && !violations);
    CHECK(pci_cfg_read32(0, 4, 2, 4) == 0xf9100547);
    CHECK(pci_cfg_read32(2, 2, 1, 4) == 0xf9100547);
    CHECK(pci_cfg_read32(3, 0, 0, 4) == 0xf9100543);
    reset(); add(8, 0, 0, false, 0, 0);
    memcpy(mmconfig, mock[0].cfg, sizeof(mock[0].cfg));
    ecam_base = (uint64_t)mmconfig; ecam_start_bus = ecam_end_bus = 8; ecam_segment = 2;
    CHECK(pci_prepare_dma_path(&pci_devices[0], &r) != NULL && !r.ready && !writes);
}
static void test_reject(void)
{
    pci_dma_path_t r;
    for (unsigned failure = 0; failure < 19; failure++) {
        chain();
        pci_dev_t *d = &pci_devices[0];
        switch (failure) {
        case 0: d = NULL; break;
        case 1: d->dev = 32; break;
        case 2: d->func = 8; break;
        case 3: d->vendor_id = 0xffff; break;
        case 4: pci_device_count = MAX_PCI_DEVICES; break;
        case 5: pci_devices[3] = *d; break;
        case 6: mock[0].cfg[1] |= 4; break;
        case 7: mock[0].cfg[0] = UINT32_MAX; break;
        case 8: mock[0].cfg[3] = 0x10000; break;
        case 9: mock[1].cfg[1] &= ~2U; break;
        case 10: mock[1].cfg[15] |= 0x40U << 16; break;
        case 11: mock[1].cfg[0] ^= 0x10000; break;
        case 12: mock[1].cfg[2] ^= 0x10000; break;
        case 13: mock[1].cfg[6] = 0x020302; break;
        case 14: mock[2].cfg[6] = 0x020200; break; /* doesn't cover leaf */
        case 15: add(0, 8, 1, true, 1, 3); break; /* overlapping sibling range */
        case 16: add(0, 8, 2, true, 3, 3); break; /* two direct parents */
        case 17: pci_device_count = 2; break; /* root missing */
        case 18: mock[1].cfg[6] = 0x040402; break; /* direct parent missing */
        }
        CHECK(pci_prepare_dma_path(d, &r) != NULL);
        CHECK(r.attempted && !r.ready && r.error && !writes && !violations && !pci_dma_lock);
    }
    chain();
    pci_dev_t absent = pci_devices[0]; absent.func = 1;
    CHECK(pci_prepare_dma_path(&absent, &r) != NULL && !writes);
    CHECK(pci_prepare_dma_path(&pci_devices[0], NULL) != NULL && !writes);
    /* Cycle that otherwise contains the leaf bus. */
    reset(); add(5, 0, 0, false, 0, 0);
    add(3, 0, 0, true, 5, 5); add(5, 1, 0, true, 3, 5);
    CHECK(pci_prepare_dma_path(&pci_devices[0], &r) != NULL && !writes && !r.ready);
    for (unsigned depth = PCI_DMA_PATH_MAX; depth <= PCI_DMA_PATH_MAX + 1; depth++) {
        reset(); add(depth, 0, 0, false, 0, 0);
        for (unsigned bus = 0; bus < depth; bus++) add(bus, 1, 0, true, bus + 1, depth);
        const char *error = pci_prepare_dma_path(&pci_devices[0], &r);
        CHECK((depth == PCI_DMA_PATH_MAX) == (error == NULL));
        CHECK(depth == PCI_DMA_PATH_MAX ? writes == depth && r.ready : !writes && !r.ready);
        CHECK(!violations);
    }
}
static void test_partial(void)
{
    pci_dma_path_t r;
    for (unsigned failure = 0; failure < 6; failure++) {
        chain();
        switch (failure) {
        case 0: blocked = 2; break;
        case 1: blocked = 1; break;
        case 2: vanish = 1; break;
        case 3: corrupt = 1; break;
        case 4: change_path = 1; break;
        case 5: drop_parent = 2; break;
        }
        CHECK(pci_prepare_dma_path(&pci_devices[0], &r) != NULL && !r.ready);
        CHECK(!(mock[0].cfg[1] & 4) && !violations && !pci_dma_lock);
        CHECK(mock[3].cfg[1] == 0xf9100547);
        if (failure != 0 && failure != 5) CHECK(mock[2].cfg[1] == 0xf9100547);
        CHECK((mock[1].cfg[1] >> 16) == 0xf910 && (mock[2].cfg[1] >> 16) == 0xf910);
    }
    chain(); blocked = 1;
    CHECK(pci_prepare_dma_path(&pci_devices[0], &r) != NULL);
    blocked = -1;
    CHECK(pci_prepare_dma_path(&pci_devices[0], &r) == NULL && r.ready && writes == 3);
    CHECK(!r.bridge[1].written && r.bridge[0].written && !violations);
}
static void test_kexec(void)
{
    chain(); quiescing = true;
    for (unsigned i = 0; i < ndev; i++) mock[i].cfg[1] |= 4;
    CHECK(pci_kexec_quiesce());
    CHECK(writes == 2 && write_order[0] == mock[0].bdf && write_order[1] == mock[3].bdf);
    CHECK(mock[0].cfg[1] == 0xf9100543 && mock[3].cfg[1] == 0xf9100543);
    CHECK(mock[1].cfg[1] == 0xf9100547 && mock[2].cfg[1] == 0xf9100547);
    CHECK(!violations && irq_enabled);
    for (unsigned fault = 0; fault < 7; fault++) {
        reset(); add(0, 4, 0, false, 0, 0); quiescing = true;
        mock[0].cfg[1] |= 4;
        mock[0].cfg[0x34 / 4] = 0x40;
        mock[0].cfg[0x40 / 4] = 0x10; /* PCI Express capability. */
        switch (fault) {
        case 0: blocked = 0; break;
        case 1: mock[0].cfg[0x40 / 4] = 0x4009; break; /* capability cycle */
        case 2: mock[0].cfg[0x34 / 4] = 0x42; break;
        case 3: mock[0].cfg[0x34 / 4] = 0x20; break;
        case 4: mock[0].cfg[0x48 / 4] = 0x00200000; break; /* pending never drains */
        case 5: mock[0].cfg[1] = UINT32_MAX; break;
        case 6: pci_device_count = MAX_PCI_DEVICES; break;
        }
        CHECK(!pci_kexec_quiesce());
        CHECK(!violations && !pci_io_lock && irq_enabled);
    }
    reset(); add(0, 4, 0, false, 0, 0); quiescing = true;
    mock[0].cfg[0x34 / 4] = 0x40; mock[0].cfg[0x40 / 4] = 0x10;
    CHECK(pci_kexec_quiesce() && !violations);
}

int main(void)
{
    test_success(); test_reject(); test_partial(); test_kexec();
    CHECK(!pci_dma_lock && !pci_io_lock && irq_enabled);
    printf("PCI DMA: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
