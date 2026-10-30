#define ACPI_RESET_HOST_TEST 1
#include "../kernel/acpi_reset.c"

extern int printf(const char *, ...);
static unsigned checks, failures, maps, writes, reads;
static uint64_t mapped;
static int map_error;
static uint16_t port, segment, pci_offset;
static uint8_t value, status, pci_dev, pci_func;
static bool pci_result = true;
static uint8_t fadt[4096], mmio[4096] __attribute__((aligned(4096)));
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

int paging_map_mmio(uint64_t address, uint64_t size)
{ maps++; mapped = address; CHECK(size == 4096); return map_error; }
uint16_t pci_diag_segment(uint8_t bus) { CHECK(bus == 0); return segment; }
bool pci_cfg_write8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint8_t v)
{
    CHECK(bus == 0); writes++; pci_dev = dev; pci_func = func;
    pci_offset = offset; value = v; return pci_result;
}
void acpi_reset_test_outb(uint16_t p, uint8_t v) { writes++; port = p; value = v; }
uint8_t acpi_reset_test_inb(uint16_t p) { CHECK(p == 0x64); reads++; return status; }

static void checksum(void)
{
    uint32_t length;
    memcpy(&length, fadt + 4, 4);
    fadt[9] = 0;
    for (uint32_t i = 0; i < length; i++) if (i != 9) fadt[9] -= fadt[i];
}
static void table(uint8_t space, uint64_t address)
{
    memset(fadt, 0, sizeof(fadt)); memcpy(fadt, "FACP", 4);
    uint32_t length = 244, flags = 1U << 10;
    memcpy(fadt + 4, &length, 4); fadt[8] = 6;
    memcpy(fadt + 112, &flags, 4);
    fadt[116] = space; fadt[117] = 8; fadt[119] = 1;
    memcpy(fadt + 120, &address, 8); fadt[128] = 6;
    checksum();
}
static void rejected(void)
{
    acpi_reset_register_t r = {1, 1, 1};
    unsigned before = maps;
    CHECK(acpi_reset_prepare(fadt, sizeof(fadt), &r) != NULL);
    CHECK(r.address == 0 && maps == before);
}

int main(void)
{
    acpi_reset_register_t r;
    table(1, 0xcf9);
    CHECK(!acpi_reset_prepare(fadt, sizeof(fadt), &r));
    CHECK(r.space == 1 && r.address == 0xcf9 && r.value == 6 && maps == 0);
    CHECK(acpi_reset_write(&r));
    CHECK(writes == 1 && port == 0xcf9 && value == 6);
    CHECK(acpi_reset_prepare(NULL, sizeof(fadt), &r) != NULL);
    CHECK(acpi_reset_prepare(fadt, sizeof(fadt), NULL) != NULL);
    for (size_t n = 0; n < 244; n++) CHECK(acpi_reset_prepare(fadt, n, &r) != NULL);
    CHECK(maps == 0 && writes == 1);
    fadt[9]++; rejected();
    table(1, 0xcf9); fadt[0] = 'X'; checksum(); rejected();
    table(1, 0xcf9); fadt[8] = 1; checksum(); rejected();
    table(1, 0xcf9); fadt[113] = 0; checksum(); rejected();
    for (unsigned width = 0; width < 256; width++) if (width != 8) {
        table(1, 0xcf9); fadt[117] = width; checksum(); rejected();
    }
    table(1, 0xcf9); fadt[118] = 1; checksum(); rejected();
    table(1, 0xcf9); fadt[119] = 2; checksum(); rejected();
    table(1, 0xcf9); fadt[119] = 0; checksum();
    CHECK(!acpi_reset_prepare(fadt, sizeof(fadt), &r));
    table(1, 65536); rejected();
    table(1, 0); rejected();
    table(3, 1); rejected();
    table(0, 1ULL << 47); rejected();
    table(0, UINT64_MAX); rejected();
    table(1, 0xcf9);
    uint32_t length = 128;
    memcpy(fadt + 4, &length, 4); checksum(); rejected();
    length = 4097; memcpy(fadt + 4, &length, 4); rejected();

    table(0, (uint64_t)&mmio[17]);
    memset(mmio, 0xa5, sizeof(mmio));
    CHECK(!acpi_reset_prepare(fadt, sizeof(fadt), &r));
    CHECK(mapped == (uint64_t)mmio && maps == 1);
    CHECK(acpi_reset_write(&r));
    CHECK(mmio[16] == 0xa5 && mmio[17] == 6 && mmio[18] == 0xa5);
    map_error = -1;
    CHECK(acpi_reset_prepare(fadt, sizeof(fadt), &r) != NULL && r.address == 0);
    map_error = 0;

    table(2, (31ULL << 32) | (7ULL << 16) | 0xab);
    CHECK(!acpi_reset_prepare(fadt, sizeof(fadt), &r));
    CHECK(acpi_reset_write(&r));
    CHECK(pci_dev == 31 && pci_func == 7 && pci_offset == 0xab && value == 6);
    pci_result = false; CHECK(!acpi_reset_write(&r)); pci_result = true;
    table(2, (1ULL << 32) | 4095);
    CHECK(!acpi_reset_prepare(fadt, sizeof(fadt), &r));
    CHECK(acpi_reset_write(&r) && pci_offset == 4095);
    table(2, 32ULL << 32); rejected();
    table(2, 8ULL << 16); rejected();
    table(2, (1ULL << 32) | 4096); rejected();
    table(2, (1ULL << 48) | 0xab); rejected();
    table(2, (1ULL << 32) | 0xab); segment = 1; rejected(); segment = 0;
    CHECK(!acpi_reset_write(NULL));
    CHECK(!acpi_reset_write(&(acpi_reset_register_t){0}));
    CHECK(!acpi_reset_write(&(acpi_reset_register_t){1, 3, 0}));

    unsigned before = writes;
    status = 2; reads = 0;
    CHECK(!acpi_reset_8042() && reads == 100000 && writes == before);
    status = 255; reads = 0;
    CHECK(!acpi_reset_8042() && reads == 1 && writes == before);
    status = 0; reads = 0;
    CHECK(acpi_reset_8042() && reads == 1 && writes == before + 1);
    CHECK(port == 0x64 && value == 0xfe);
    printf("ACPI reset: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
