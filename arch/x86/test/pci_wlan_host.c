#include "../include/types.h"
extern void pci_test_outl(uint16_t port, uint32_t value);
extern uint32_t pci_test_inl(uint16_t port);
#define outl pci_test_outl
#define inl pci_test_inl
#define PCI_HOST_TEST 1
#include "../kernel/pci.c"

extern int printf(const char *format, ...);
extern void abort(void);
static unsigned checks, failures, violations, selections, writes;
static uint32_t selected, config[64];
static volatile uint32_t ecam[1024];
static bool irq_enabled = true;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

void serial_puts(const char *s) { (void)s; }
void serial_puthex(uint64_t v, int n) { (void)v; (void)n; }
void serial_putdec(uint64_t v) { (void)v; }
const char *intel_gfx_device_name(uint16_t v) { (void)v; return "mock"; }
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
void pci_test_outl(uint16_t port, uint32_t value)
{
    if (irq_enabled || !pci_io_lock) violations++;
    if (port == 0xcf8) { selected = value; selections++; }
    else if (port == 0xcfc) { config[(selected & 0xfc) / 4] = value; writes++; }
    else abort();
}
uint32_t pci_test_inl(uint16_t port)
{
    if (irq_enabled || !pci_io_lock || port != 0xcfc) violations++;
    return config[(selected & 0xfc) / 4];
}
void pci_test_outw(uint16_t port, uint16_t value)
{
    if (irq_enabled || !pci_io_lock || (port != 0xcfc && port != 0xcfe)) violations++;
    unsigned idx = (selected & 0xfc) / 4;
    unsigned shift = (port - 0xcfc) * 8;
    config[idx] = (config[idx] & ~(0xffffU << shift)) | ((uint32_t)value << shift);
    writes++;
}
void pci_test_outb(uint16_t port, uint8_t value)
{
    if (irq_enabled || !pci_io_lock || port < 0xcfc || port > 0xcff) violations++;
    unsigned idx = (selected & 0xfc) / 4, shift = (port - 0xcfc) * 8;
    config[idx] = (config[idx] & ~(255U << shift)) | ((uint32_t)value << shift);
    writes++;
}

int main(void)
{
    config[1] = 0xa5a50007;
    CHECK(pci_cfg_read16(3, 2, 1, 4) == 7);
    CHECK(selected == 0x80031104);
    CHECK(pci_cfg_read16(3, 2, 1, 6) == 0xa5a5);
    pci_cfg_write16(3, 2, 1, 4, 0x403);
    CHECK(config[1] == 0xa5a50403); /* Status was not rewritten. */
    CHECK(irq_enabled && !pci_io_lock);
    irq_enabled = false;
    pci_cfg_write16(3, 2, 1, 6, 0x1234);
    CHECK(config[1] == 0x12340403 && !irq_enabled);
    irq_enabled = true;
    unsigned before = selections;
    CHECK(pci_cfg_read16(0, 0, 0, 3) == 0xffff);
    CHECK(pci_cfg_read16(0, 0, 0, 256) == 0xffff);
    pci_cfg_write16(0, 0, 0, 3, 0);
    pci_cfg_write16(0, 0, 0, 256, 0);
    CHECK(selections == before);
    pci_cfg_write32(3, 2, 1, 0x10, 0xfee00004);
    CHECK(pci_cfg_read32(3, 2, 1, 0x10) == 0xfee00004);

    ecam_base = (uint64_t)ecam;
    ecam_start_bus = ecam_end_bus = 8;
    ecam[1] = 0xc0c00007;
    before = selections;
    pci_cfg_write16(8, 0, 0, 4, 0x402);
    CHECK(ecam[1] == 0xc0c00402 && selections == before);
    CHECK(pci_cfg_read16(8, 0, 0, 6) == 0xc0c0);
    CHECK(pci_cfg_read32(8, 0, 0, 4) == 0xc0c00402);
    pci_cfg_write32(8, 0, 0, 0x10, 0x12340000);
    CHECK(ecam[4] == 0x12340000 && selections == before);
    CHECK(pci_cfg_read16(3, 0, 0, 4) == 0x403 && selections == before + 1);
    pci_cfg_write32(3, 0, 0, 0x10, 0xfee10004);
    CHECK(config[4] == 0xfee10004);
    uint32_t diagnostic = 0x13579bdf;
    before = selections;
    CHECK(!pci_diag_read32(3, 0, 0, 0x100, &diagnostic));
    CHECK(diagnostic == 0x13579bdf && selections == before);
    CHECK(!pci_diag_read32(8, 0, 0, 6, &diagnostic));
    CHECK(!pci_diag_read32(8, 32, 0, 4, &diagnostic));
    CHECK(!pci_diag_read32(8, 0, 8, 4, &diagnostic));
    CHECK(!pci_diag_read32(8, 0, 0, 4096, &diagnostic));
    CHECK(!pci_diag_read32(8, 0, 0, 4, NULL));
    ecam[1023] = 0xdeadbeef;
    CHECK(pci_diag_read32(8, 0, 0, 4092, &diagnostic) && diagnostic == 0xdeadbeef);
    CHECK(selections == before);
    ecam_segment = 2;
    CHECK(pci_diag_segment(8) == 2 && pci_diag_segment(3) == 0);
    ecam_segment = 0;
    ecam_base = 0;

    pci_dev_t child = {.bus = 8}, parent;
    pci_devices[0] = (pci_dev_t){.bus = 0, .dev = 0x1d, .func = 6,
        .vendor_id = 0x8086, .device_id = 0xa336, .class_code = 6, .subclass = 4};
    pci_device_count = 1;
    config[0] = 0xa3368086; config[3] = 0x10000; config[6] = 0x080800;
    unsigned old_writes = writes;
    CHECK(pci_diag_parent(&child, &parent) == 1 && parent.dev == 0x1d && parent.func == 6);
    child.bus = 9; CHECK(pci_diag_parent(&child, &parent) == 0);
    child.bus = 8; pci_devices[1] = pci_devices[0]; pci_device_count = 2;
    CHECK(pci_diag_parent(&child, &parent) == -1);
    pci_device_count = 1; config[6] = 0x070800;
    CHECK(pci_diag_parent(&child, &parent) == -1);
    config[6] = 0x080800; config[0] = UINT32_MAX;
    CHECK(pci_diag_parent(&child, &parent) == -1);
    pci_device_count = MAX_PCI_DEVICES;
    CHECK(pci_diag_parent(&child, &parent) == -1 && writes == old_writes);
    pci_device_count = 0; config[3] = config[6] = 0;

    pci_add_device(1, 0, 0, 0x1af4, 0x1041, 2, 0);
    pci_dev_t ethernet = *pci_get_nic();
    CHECK(pci_get_wlan() == NULL);
    pci_add_device(2, 0, 0, 0x8086, 0x2725, 2, 0x80); /* AX210 != AX200. */
    CHECK(pci_get_wlan() == NULL);
    pci_device_count = MAX_PCI_DEVICES;
    pci_add_device(3, 0, 0, 0x8086, 0x2723, 2, 0x80);
    CHECK(pci_get_wlan() && pci_get_wlan()->bus == 3);
    CHECK(pci_get_wlan()->bar[0] == 0xfee10000);
    CHECK(memcmp(pci_get_nic(), &ethernet, sizeof(ethernet)) == 0);
    pci_add_device(4, 0, 0, 0x8086, 0x2723, 2, 0x80);
    CHECK(pci_get_wlan()->bus == 3); /* Do not silently change selected radio. */
    config[1] = 0xaabbccdd;
    CHECK(pci_cfg_write8(3, 2, 1, 5, 0x12));
    CHECK(selected == 0x80031104 && config[1] == 0xaabb12dd);
    irq_enabled = false;
    CHECK(pci_cfg_write8(3, 2, 1, 7, 0x56));
    CHECK(config[1] == 0x56bb12dd && !irq_enabled);
    irq_enabled = true;
    before = selections;
    CHECK(!pci_cfg_write8(0, 32, 0, 4, 0));
    CHECK(!pci_cfg_write8(0, 0, 8, 4, 0));
    CHECK(!pci_cfg_write8(0, 0, 0, 256, 0));
    CHECK(!pci_cfg_write8(0, 0, 0, 4096, 0));
    CHECK(selections == before);
    ecam_base = (uint64_t)ecam;
    ecam_start_bus = ecam_end_bus = 8;
    ecam[1023] = 0xdeadbeef;
    CHECK(pci_cfg_write8(8, 0, 0, 4095, 0xab));
    CHECK(ecam[1023] == 0xabadbeef && selections == before);
    CHECK(violations == 0 && irq_enabled && !pci_io_lock);
    printf("PCI WLAN: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
