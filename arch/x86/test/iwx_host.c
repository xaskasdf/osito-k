#define IWX_HOST_TEST 1
#include "../drivers/iwx.c"
#include "../drivers/iwx_fw.c"
#include "../drivers/iwx_scan.c"
#include "../drivers/iwx_link.c"
#include "../drivers/iwx_data.c"
#define DMA_DIAG_HOST_TEST 1
#define le32 diag_le32
#include "../kernel/dma_diag.c"
#undef le32

boot_info_t saved_boot_info;
const void *dma_diag_test_map(uint64_t a, unsigned n) { (void)a; (void)n; return NULL; }
uint32_t dma_diag_test_read32(uint64_t a) { (void)a; return UINT32_MAX; }
uint64_t dma_diag_test_read64(uint64_t a) { (void)a; return UINT64_MAX; }
uint16_t pci_diag_segment(uint8_t b) { (void)b; return 0; }
int pci_diag_parent(const pci_dev_t *d, pci_dev_t *p) { (void)d; (void)p; return 0; }
bool pci_diag_read32(uint8_t b, uint8_t d, uint8_t f, uint16_t off, uint32_t *v)
{
    if (!v || (off & 3) || off > 252) return false;
    *v = pci_cfg_read32(b, d, f, off);
    return true;
}

extern int printf(const char *format, ...);
extern void abort(void);
extern void *memmove(void *dest, const void *source, size_t size);

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static struct {
    pci_dev_t dev;
    uint32_t pci[64], csr[2048], hpm, wprot, prph_addr, prph_write;
    uint64_t now, bar;
    unsigned config_writes, writes, reads, maps, violations;
    bool absent, map_fail, unmapped, no_time, power_fail, clock_fail;
    bool owner_fail, prepare_needed, stop_fail, rfkill, removed;
    bool dma_latched, msi_latched, power_reset;
    bool allow_dma, started, no_alive, no_rfh, bad_status, bad_length, bad_id;
    bool bad_cursor, duplicate_id, sw_error, byte_writes, stop_on_launch;
    bool removed_on_launch, block_bme_start, no_dma_mapping, request_only;
    bool wrong_queue, paging, bme_stuck_on_stop, block_bme_stop;
    bool diag_clock_fail, diag_sentinel, corrupt_caps, path_fail, path_ready, mac_release_stuck;
    unsigned path_calls;
    uint32_t boot_cause, boot_fh;
    unsigned diag_reads, trace_error_at;
    unsigned allocations, frees, fail_at, next_page, live_pages, offered;
    unsigned notification_version, reported_version, injected;
    unsigned command_calls, command_fault, fault_command, nvm_version;
    unsigned init_sequence, init_payload;
    bool init_after, command_large;
    bool scheduler_off, spawn_fail, rx_owned[IWX_RX_COUNT];
    bool scan_mode, scan_early, abort_complete;
    bool link_mode, session_start, tx_reverse;
    unsigned tx_calls, tx_fault, link_fault;
    unsigned tx_queue, tx_write_pointer, tx_cursor, tx_queue_flags;
    unsigned key_calls, key_fault, key_fault_at;
    bool eapol_hold, rng_fail;
    unsigned scan_fault, mcc_changes, radio_calls;
    uint32_t last_mcc_query;
    unsigned spawn_calls, rx_doorbells, rx_producer;
    void (*worker)(void);
} hw;
static uint8_t dma_memory[4 * 1024 * 1024] __attribute__((aligned(4096)));
int random_secure_supported(void) { return !hw.rng_fail; }
int random_secure_word(uint64_t *word) { *word = 0x1a2b3c4d5e6f7081ULL; return 1; }
extern unsigned iwx_test_wrap(const uint8_t key[16], const uint8_t *p, unsigned n, uint8_t *out);
extern void iwx_test_hmac(const uint8_t key[16], const uint8_t *p, unsigned n, uint8_t mac[20]);
static unsigned allocation_pages[1024];
static void queue_fw_packet(uint8_t opcode, uint8_t group, uint8_t index, uint8_t queue,
                            const uint8_t *data, unsigned length);
#define DMA_BASE 0x10000000ULL

void *iwx_test_dma_ptr(uint64_t phys)
{
    if (phys < DMA_BASE || phys >= DMA_BASE + sizeof(dma_memory)) abort();
    return dma_memory + (phys - DMA_BASE);
}
void *mem_alloc_pages(uint64_t pages)
{
    hw.allocations++;
    if (hw.allocations == hw.fail_at || hw.next_page + pages > 1024) return NULL;
    unsigned start = hw.next_page;
    hw.next_page += pages;
    allocation_pages[start] = pages;
    hw.live_pages += pages;
    return (void *)(DMA_BASE + (uint64_t)start * 4096);
}
void mem_free_pages(void *address, uint64_t pages)
{
    uint64_t phys = (uint64_t)address;
    if (phys < DMA_BASE || phys >= DMA_BASE + sizeof(dma_memory) || (phys & 4095)) abort();
    unsigned slot = (phys - DMA_BASE) / 4096;
    if (allocation_pages[slot] != pages) abort();
    if (transport.exposed && (hw.started || (hw.pci[1] & 4))) hw.violations++;
    allocation_pages[slot] = 0;
    hw.live_pages -= pages;
    hw.frees++;
}

static void reset_mock(void)
{
    CHECK(hw.live_pages == 0 && transport.count == 0 && iwx_retained_bytes() == 0 && !runtime.active);
    memset(&hw, 0, sizeof(hw));
    memset(&runtime, 0, sizeof(runtime));
    memset(allocation_pages, 0, sizeof(allocation_pages));
    memset(dma_memory, 0xa5, sizeof(dma_memory));
    probe_lock = 0;
    hw.dev = (pci_dev_t){.bus = 3, .vendor_id = 0x8086, .device_id = 0x2723,
                         .class_code = 2, .subclass = 0x80};
    hw.bar = 0xfec00000;
    hw.pci[0] = 0x27238086;
    hw.pci[1] = 0x00100007; /* Capability list, BIOS bus mastering. */
    hw.pci[4] = (uint32_t)hw.bar | 4;
    hw.pci[0x34 / 4] = 0x40;
    hw.pci[0x40 / 4] = 0x00004801; /* PM -> MSI */
    hw.pci[0x44 / 4] = 0x00008003; /* D3hot, PME status */
    hw.pci[0x48 / 4] = 0x00015805; /* Enabled MSI -> MSI-X */
    hw.pci[0x58 / 4] = 0x80006011; /* Enabled MSI-X -> PCIe */
    hw.pci[0x60 / 4] = 0x00020010; /* PCIe v2 endpoint */
    hw.pci[0x68 / 4] = 0x000a0810; /* Device control + W1C error status */
    hw.pci[0x70 / 4] = 0x10110042; /* Link control + status */
    hw.csr[IWX_HW_REV / 4] = 0x350;
    hw.csr[IWX_HW_RF_ID / 4] = 0x10a100;
    hw.csr[IWX_GP_CNTRL / 4] = IWX_RF_ENABLED;
    hw.notification_version = hw.reported_version = 6;
    hw.tx_queue = 7;
    hw.tx_queue_flags = 1; /* Captured CC API-77 allocation response on the P1. */
}

const pci_dev_t *pci_get_wlan(void) { return hw.absent ? NULL : &hw.dev; }
bool sched_is_enabled(void) { return !hw.scheduler_off; }
int sched_spawn(const char *name, void (*entry)(void))
{
    CHECK(!strcmp(name, "wlan-rx") && probe_lock && !runtime.active && entry);
    unsigned reads = hw.reads, writes = hw.writes;
    iwx_status_t s;
    CHECK(iwx_poll() == 0 && iwx_get_status(&s) == -1);
    CHECK(reads == hw.reads && writes == hw.writes);
    hw.spawn_calls++;
    if (hw.spawn_fail) return -1;
    hw.worker = entry;
    return 42;
}
int sched_sleep_ticks(uint64_t ticks) { CHECK(ticks == 1); return 0; }
void sched_yield(void)
{
#ifdef IWX_NET_HOST_TEST
    extern void iwx_test_net_yield(void);
    iwx_test_net_yield();
#endif
}
const char *pci_prepare_dma_path(const pci_dev_t *d, pci_dma_path_t *r)
{
    if (d != &hw.dev || transport.exposed || (hw.pci[1] & 4) ||
        !transport.context || !hw.live_pages || hw.started) hw.violations++;
    hw.path_calls++;
    r->attempted = true;
    if (hw.path_fail) return r->error = "mock bridge BME did not latch";
    hw.path_ready = r->ready = true;
    return NULL;
}
uint16_t pci_cfg_read16(uint8_t b, uint8_t d, uint8_t f, uint16_t reg)
{
    (void)b; (void)d; (void)f;
    if ((reg & 1) || reg > 254) abort();
    return hw.pci[reg / 4] >> ((reg & 2) * 8);
}
uint32_t pci_cfg_read32(uint8_t b, uint8_t d, uint8_t f, uint16_t reg)
{
    (void)b; (void)d; (void)f;
    if ((reg & 3) || reg > 252) abort();
    return hw.pci[reg / 4];
}

static void inject_trace_error(unsigned step)
{
    if (hw.trace_error_at != step) return;
    hw.csr[IWX_INT / 4] |= 1U << 29;
    hw.pci[0x68 / 4] |= 1U << 16;
}

void pci_cfg_write16(uint8_t b, uint8_t d, uint8_t f, uint16_t reg, uint16_t v)
{
    (void)b; (void)d; (void)f;
    hw.config_writes++;
    if ((reg & 1) || reg > 254) abort();
    if (reg == 6 || (reg == 0x44 && (v & 0x8000))) hw.violations++;
    if (reg == 4 && (v & 4) && !hw.allow_dma) hw.violations++;
    if (reg == 4 && (v & 4) && hw.allow_dma && !hw.path_ready && !hw.dma_latched)
        hw.violations++;
    if (reg == 4 && (v & 4) && hw.block_bme_start) v &= ~4U;
    if (reg == 4 && hw.block_bme_stop) v |= 4;
    if (reg == 4 && hw.dma_latched) v |= 4;
    if (reg == 0x4a && hw.msi_latched) v |= 1;
    if (reg == 0x44 && hw.power_fail) return;
    if (reg == 0x44 && hw.power_reset) hw.pci[1] &= 0xffff0000;
    uint32_t shift = (reg & 2) * 8;
    hw.pci[reg / 4] = (hw.pci[reg / 4] & ~(0xffffU << shift)) | ((uint32_t)v << shift);
    if (reg == 4 && (v & 4) && hw.allow_dma) inject_trace_error(1);
}
int paging_map_mmio(uint64_t phys, uint64_t size)
{
    hw.maps++;
    if (phys != hw.bar || size != 8192) hw.violations++;
    return hw.map_fail ? -1 : 0;
}
uint64_t paging_get_kernel_cr3(void) { return 4096; }
uint64_t paging_translate_in_cr3(uint64_t cr3, uint64_t va)
{
    uint64_t phys = va - (uint64_t)PHYS_TO_VIRT(0);
    if (cr3 != 4096) hw.violations++;
    if (phys >= DMA_BASE && phys < DMA_BASE + sizeof(dma_memory))
        return hw.no_dma_mapping ? UINT64_MAX : phys;
    if (phys != hw.bar && phys != hw.bar + 4096) hw.violations++;
    return hw.unmapped ? UINT64_MAX : phys;
}
uint64_t idt_get_monotonic_ns(void)
{
    if (!hw.no_time) hw.now += 10000;
    return hw.now;
}
uint32_t iwx_test_read(uint64_t addr)
{
    hw.reads++;
    if (addr < hw.bar || addr >= hw.bar + 8192 || (addr & 3)) abort();
    unsigned reg = addr - hw.bar;
    if (((hw.pci[1] & 4) && !hw.allow_dma) || (hw.pci[0x44 / 4] & 3)) hw.violations++;
    if (hw.removed) return 0xffffffff;
    if (reg == IWX_PRPH_RDAT) {
        if (hw.prph_addr == 0x03440) return hw.hpm;
        if (hw.prph_addr == 0x04d00) return hw.wprot;
        switch (hw.prph_addr) {
        /* AX200 bus addresses, independent of the driver's logical constants. */
        case 0x09820: case 0x09800: case 0x09808:
        case 0x01eb0: case 0x01eb8:
        case 0x01e30: case 0x01e34:
        case 0x038c0: case 0x038c4: case 0x05c40:
            if (!(hw.csr[IWX_GP_CNTRL / 4] & IWX_MAC_ACCESS) ||
                !(hw.csr[IWX_GP_CNTRL / 4] & IWX_CLOCK_READY)) hw.violations++;
            hw.diag_reads++;
            return hw.diag_sentinel ? 0xa5a5a5a0 : 0x51000000 | (hw.prph_addr & 0xffff);
        default: hw.violations++;
        }
    }
    return hw.csr[reg / 4];
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = v >> (i * 8);
}

static void inject_alive(void)
{
    if (transport.streaming) {
        CHECK(hw.rx_owned[2]);
        hw.rx_owned[2] = false;
        hw.rx_producer = hw.duplicate_id ? 2 : 1;
    }
    uint8_t *buf = dma_ptr(transport.rx[2]); /* Completion order need not equal buffer ID. */
    unsigned version = hw.reported_version;
    unsigned payload = version == 4 ? 116 : version == 5 ? 128 : 144;
    memset(buf, 0, payload + 8);
    put_u32(buf, (hw.bad_length ? 0x3fff : payload + 4) | (hw.wrong_queue ? 0x10000 : 0));
    buf[4] = 1;
    buf[7] = hw.request_only ? 0 : 0x80;
    buf[8] = hw.bad_status ? 0xad : 0xfe;
    buf[9] = hw.bad_status ? 0xde : 0xca;
    put_u32(buf + 12, 0x11223344);
    put_u32(buf + 16, 0x55667788);
    put_u32(buf + 108, 0x778899aa);
    put_u32(buf + 112, 0xbbccddee);
    put_u32(buf + ((payload + 8 + 63) & ~63), IWX_RX_INVALID);
    uint32_t *used = dma_ptr(transport.used_ring);
    used[0] = hw.bad_id ? 4095 : 3;
    used[1] = 3;
    *(uint16_t *)dma_ptr(transport.status) = hw.bad_cursor ? 4095 :
                                          hw.duplicate_id ? 2 : 1;
    hw.injected++;
}

static unsigned mock_packet(uint8_t *p, uint8_t opcode, uint8_t group,
                             uint8_t index, uint8_t queue, unsigned size)
{
    memset(p, 0, 8 + size);
    put_u32(p, size + 4);
    p[4] = opcode; p[5] = group; p[6] = index; p[7] = queue;
    return (8 + size + 63) & ~63U;
}

static unsigned mock_rx_slot(void)
{
    for (unsigned i = 0; i < IWX_RX_COUNT; i++) if (hw.rx_owned[i]) return i;
    abort();
    return 0;
}

static void check_radio_command(const uint8_t *h, unsigned payload)
{
    const uint8_t *p = h + 8;
    hw.radio_calls++;
    if (h[1] == 1 && h[0] == 0x98) CHECK(payload == 4 && rx_le32(p) == 3);
    else if (h[1] == 1 && h[0] == 0x9b) CHECK(payload == 8 && rx_le32(p) == 3 && !rx_le32(p + 4));
    else if (h[1] == 2 && h[0] == 1) CHECK(payload == 8 && rx_le32(p) == 1 && !rx_le32(p + 4));
    else if (h[1] == 5 && h[0] == 0) {
        CHECK(payload == 4 && !rx_le32(p));
        CHECK(iwx_radio_capability(&runtime.profile, 12));
    } else if (h[1] == 1 && h[0] == 0xee) {
        CHECK(payload == 32 && rx_le32(p) == 1);
        CHECK(hw.pci[(0x60 + 0x28) / 4] & 0x400);
    } else if (h[1] == 4 && h[0] == 4) {
        CHECK(payload == 20);
        for (unsigned i = 0; i < payload; i++) CHECK(!p[i]);
    } else if (h[1] == 1 && h[0] == 0x77) CHECK(payload == 4 && !rx_le32(p));
    else if (h[1] == 1 && h[0] == 0xc8) {
        uint32_t expected = runtime.mcc_pending ?
            runtime.pending_mcc | ((uint32_t)runtime.pending_mcc_source << 16) : 0x105a5a;
        CHECK(payload == 28 && rx_le32(p) == expected);
        hw.last_mcc_query = rx_le32(p);
        for (unsigned i = 4; i < payload; i++) CHECK(!p[i]);
    } else if (h[1] == 1 && h[0] == 0x0c) {
        CHECK(payload == 12 && !rx_le32(p) && rx_le32(p + 4) == 3 && rx_le32(p + 8) == 3);
        CHECK(runtime.scan.regulatory.valid);
    } else if (h[1] == 1 && h[0] == 0xd2) {
        CHECK(payload == 60);
        for (unsigned i = 0; i < payload; i++) CHECK(!p[i]);
    } else if (h[1] == 1 && h[0] == 0x0d) {
        CHECK(payload == 1940 && runtime.scan.state == IWX_SCAN_RUNNING);
        CHECK(!rx_le32(p) && rx_le32(p + 4) == 6 && p[8] == 0x86 && p[9] == 8);
        CHECK(p[17] == 0 && p[45] == 51 && p[586] == 1);
    } else if (h[1] == 1 && h[0] == 0x0e) {
        CHECK(payload == 8 && !rx_le32(p) && !rx_le32(p + 4));
        CHECK(runtime.scan.state == IWX_SCAN_ABORTING);
    } else if (hw.link_mode && h[1] == 1 && h[0] == 8) {
        CHECK(payload == 32 && rx_le32(p) == 0 && rx_le32(p + 4) == 1);
        CHECK(rx_le32(p + 8) == 1 && p[12] == 1 && !p[13]);
        for (unsigned i = 14; i < payload; i++) CHECK(!p[i]);
    } else if (hw.link_mode && h[1] == 5 && h[0] == 8) {
        CHECK(payload == 32 && h[7] == 2 && rx_le32(p + 4) == 0x1406);
        for (unsigned i = 8; i < payload; i++) CHECK(!p[i]);
    } else if (hw.link_mode && h[1] == 1 && h[0] == 0x28) {
        CHECK(payload == 148 && rx_le32(p) == 0 && rx_le32(p + 8) == 5);
        CHECK(rx_le32(p + 4) == (runtime.link.status.state == IWX_LINK_CONFIGURE ? 2U : 1U));
        CHECK(!memcmp(p + 16, runtime.status.nvm.mac, 6));
        CHECK(!memcmp(p + 24, "\x02\x01\x02\x03\x04\x05", 6));
        CHECK(rx_le32(p + 52) == 68 && !rx_le32(p + 56));
        CHECK(rx_le32(p + 100) == (runtime.link.status.state == IWX_LINK_CONFIGURE ? 1U : 0U));
        CHECK(rx_le32(p + 136) == (runtime.link.status.state == IWX_LINK_CONFIGURE ? 35U : 0U));
    } else if (hw.link_mode && h[1] == 1 && h[0] == 0x2b) {
        CHECK(payload == 28 && !rx_le32(p) && rx_le32(p + 4) == 1 && !rx_le32(p + 8));
        CHECK(rx_le32(p + 12) == UINT32_MAX && rx_le32(p + 16) == UINT32_MAX);
        CHECK(!rx_le32(p + 20) && !rx_le32(p + 24));
    } else if (hw.link_mode && h[1] == 1 && h[0] == 0x18) {
        CHECK(payload == 48 && !memcmp(p + 8, "\x02\x01\x02\x03\x04\x05", 6));
        CHECK(rx_le32(p + 24) == 0x3c000000 && !p[16] && !p[35]);
    } else if (hw.link_mode && h[1] == 1 && h[0] == 0x17) {
        hw.key_calls++;
        CHECK(payload == 76 && !p[0] && p[1] < 5 && !runtime.link.status.port_open);
        CHECK(runtime.wpa.install_pending && runtime.wpa.keys.generation);
        bool tkip = p[1] && runtime.wpa.group_cipher == WPA2_CIPHER_TKIP;
        CHECK(rx_le16(p + 2) == (p[1] ? ((tkip ? 0x400b : 0x400a) | ((p[1] - 1) << 8)) : 0xa));
        CHECK(!memcmp(p + 4, p[1] ? runtime.wpa.keys.group : runtime.wpa.keys.pairwise, 16));
        if (tkip) {
            CHECK(!memcmp(p + 20, runtime.wpa.keys.group + 24, 8));
            CHECK(!memcmp(p + 28, runtime.wpa.keys.group + 16, 8));
            CHECK(!memcmp(p + 52, runtime.wpa.keys.group + 16, 8));
            CHECK(!memcmp(p + 60, runtime.wpa.keys.group + 24, 8));
            for (unsigned i = 36; i < 52; i++) CHECK(!p[i]);
            for (unsigned i = 68; i < payload; i++) CHECK(!p[i]);
        } else for (unsigned i = 20; i < payload; i++) CHECK(!p[i]);
    } else if (hw.link_mode && h[1] == 5 && h[0] == 0x17) {
        CHECK(payload == 36 && !rx_le32(p) && rx_le32(p + 4) == 1 && p[8] == 15);
        CHECK(!rx_le32(p + 12) && rx_le32(p + 16) == 5);
        CHECK(rx_le32(p + 20) == transport.tx_byte_count && !rx_le32(p + 24));
        CHECK(rx_le32(p + 28) == transport.tx_descriptors && !rx_le32(p + 32));
    } else if (hw.link_mode && h[1] == 3 && h[0] == 5) {
        CHECK(payload == 24 && !rx_le32(p) && rx_le32(p + 4) == 1 && !rx_le32(p + 8));
        CHECK(rx_le32(p + 12) == 3000 && !rx_le32(p + 16) && !rx_le32(p + 20));
    } else CHECK(false);
}

static void inject_command(uint32_t doorbell)
{
    unsigned index = hw.command_calls++ & 255;
    unsigned fault = hw.command_calls == hw.fault_command ? hw.command_fault : 0;
    const uint8_t *t = (uint8_t *)dma_ptr(transport.command) + index * 256;
    const uint8_t *h = dma_ptr(transport.command_data);
    unsigned payload = h[4] | ((unsigned)h[5] << 8), total = payload + 8;
    CHECK(hw.started && transport.exposed && (hw.pci[1] & 4) && probe_lock);
    CHECK(!(hw.csr[IWX_GP_CNTRL / 4] & IWX_MAC_ACCESS));
    CHECK(doorbell == ((index + 1) & 255) && h[2] == index && h[3] == 0 && h[6] == 0);
    CHECK(h[7] == ((hw.link_mode && h[1] == 5 && h[0] == 8) ? 2 : 0));
    CHECK((t[0] | t[1] << 8) == (total > 20 ? 2 : 1));
    CHECK((unsigned)(t[2] | t[3] << 8) == (total > 20 ? 20 : total));
    CHECK(rx_le32(t + 4) == transport.command_data && rx_le32(t + 8) == 0);
    if (total > 20) {
        CHECK((unsigned)(t[12] | t[13] << 8) == total - 20);
        CHECK(rx_le32(t + 14) == transport.command_data + 20 && rx_le32(t + 18) == 0);
    }
    if (!hw.command_large) {
        static const uint8_t groups[] = {2, 12, 12}, codes[] = {3, 0, 2};
        CHECK(index < 3 && payload == 4);
        if (index < 3) CHECK(h[0] == codes[index] && h[1] == groups[index]);
        CHECK(rx_le32(h + 8) == (index == 0 ? 2U : 0U));
    }
    if (hw.scan_mode) check_radio_command(h, payload);
    if (fault == 1) return;
    if (fault == 22) { hw.rfkill = true; hw.csr[IWX_GP_CNTRL / 4] &= ~IWX_RF_ENABLED; }
    if (fault == 23) hw.csr[IWX_INT / 4] |= IWX_INT_HW_ERR;
    if (fault == 24) hw.csr[IWX_INT / 4] |= IWX_INT_SW_ERR;
    if (fault == 25) { hw.no_time = true; return; }
    if (fault == 29) hw.mac_release_stuck = true;
    if (fault == 30) hw.diag_clock_fail = true;
    unsigned slot = transport.streaming ? mock_rx_slot() : index < 2 ? index : index + 1;
    uint8_t *buf = dma_ptr(transport.rx[slot]);
    bool init = index == 1 && !hw.command_large && fault != 13;
    unsigned off = 0;
    if (hw.scan_mode && hw.scan_early && h[1] == 1 && h[0] == 0x0d) {
        off += mock_packet(buf, 0x0f, 0, 0, 0x80, 16);
        buf[14] = 1;
    }
    if ((init && !hw.init_after) || fault == 16)
        off += mock_packet(buf, 4, 0, hw.init_sequence,
                           (hw.init_sequence >> 8) & (fault == 15 ? 0x7f : 0xff), hw.init_payload);
    unsigned size = index == 2 && !hw.command_large ? (hw.nvm_version == 4 ? 468 : 128) : 0;
    bool mcc = hw.scan_mode && h[1] == 1 && h[0] == 0xc8;
    bool scan_abort = hw.scan_mode && h[1] == 1 && h[0] == 0x0e;
    if (mcc) size = hw.scan_fault == 1 ? 20 : 224;
    if (scan_abort) size = hw.scan_fault == 4 ? 0 : 4;
    bool binding = hw.link_mode && h[1] == 1 && h[0] == 0x2b;
    bool station = hw.link_mode && h[1] == 1 && h[0] == 0x18;
    bool txq = hw.link_mode && h[1] == 5 && h[0] == 0x17;
    bool session = hw.link_mode && h[1] == 3 && h[0] == 5;
    bool key = hw.link_mode && h[1] == 1 && h[0] == 0x17;
    unsigned key_fault = !hw.key_fault_at || hw.key_calls == hw.key_fault_at ? hw.key_fault : 0;
    if (key && key_fault == 3) return;
    if (key) size = key_fault == 1 ? 3 : 4;
    if (binding || station) size = hw.link_fault == 1 ? 0 : 4;
    if (txq) size = hw.link_fault == 3 ? 7 : 8;
    if (fault == 9) size = 513;
    if (fault == 17) size--;
    uint8_t *response = buf + off;
    off += mock_packet(response, h[0], h[1], index, 0, size);
    switch (fault) {
    case 2: response[6]++; break;
    case 3: response[5]++; break;
    case 4: response[4]++; break;
    case 5: response[7] = 1; break;
    case 6: response[5] |= 0x40; break;
    case 7: response[4] = 2; response[5] = 0; response[7] = 0x80; break;
    case 10: put_u32(response, 0x3fff); break;
    case 11: put_u32(response, (size + 4) | 0x10000); break;
    case 12: response[7] = 0x80; break;
    }
    if (station) put_u32(response + 8, hw.link_fault == 2 ? 2 : 1);
    if (key) put_u32(response + 8, key_fault == 2 ? 2 : 1);
    if (key && key_fault == 4) {
        unsigned where = off;
        off += mock_packet(buf + off, 0xc1, 0, 0, 0x80, 74);
        uint8_t *mpdu = buf + where + 8;
        mpdu[0] = 26; mpdu[12] = 3; mpdu[34] = 1; mpdu[48] = 0xc0;
        memcpy(mpdu + 52, runtime.link.local, 6);
        memcpy(mpdu + 58, runtime.link.ap.bssid, 6); memcpy(mpdu + 64, mpdu + 58, 6);
        mpdu[72] = 3;
    }
    if (binding) put_u32(response + 8, hw.link_fault == 2 ? 1 : 0);
    if (txq) {
        unsigned q = hw.link_fault == 4 ? 0 : hw.link_fault == 5 ? 512 : hw.tx_queue;
        response[8] = q; response[9] = q >> 8;
        response[10] = hw.tx_queue_flags;
        response[11] = hw.tx_queue_flags >> 8;
        response[12] = hw.tx_write_pointer; response[13] = hw.tx_write_pointer >> 8;
        hw.tx_cursor = hw.tx_write_pointer & 4095;
        if (hw.link_fault == 6) response[5] |= 0x40; /* Header CMD_FAILED. */
        if (hw.link_fault == 7) response[6]++; /* Wrong echoed command index. */
    }
    if (session && hw.session_start) {
        unsigned where = off;
        off += mock_packet(buf + off, 0xfb, 3, 0, 0x80, hw.link_fault == 8 ? 15 : 16);
        put_u32(buf + where + 12, hw.link_fault == 9 ? 0 : 1);
        put_u32(buf + where + 16, 1);
    }
    if (index == 2 && !hw.command_large) {
        uint8_t *p = response + 8;
        p[4] = 0x34; p[5] = 0x12; p[6] = 7; p[7] = 2;
        put_u32(p + 8, 0x1f);
        put_u32(p + 12, fault == 19 ? 4 : 3);
        put_u32(p + 16, fault == 20 ? 0 : 3);
        put_u32(p + 20, fault == 21 ? 2 : 1);
        if (hw.nvm_version == 4) {
            put_u32(p + 24, fault == 18 ? 111 : 51);
            for (unsigned i = 0; i < 51; i++) put_u32(p + 28 + i * 4, 0x701 + i * 2);
        } else {
            for (unsigned i = 0; i < 51; i++) {
                p[24 + 2 * i] = 1 + i * 2; p[25 + 2 * i] = 7;
            }
        }
    }
    if (mcc) {
        uint8_t *p = response + 8;
        put_u32(p, hw.scan_fault == 2 ? 2 : 0);
        put_u32(p + 4, h[10] == 0x10 ? 0x434c : (uint16_t)rx_le32(h + 8));
        p[12] = 0x10;
        put_u32(p + 16, 51);
        if (size == 224) for (unsigned i = 0; i < 51; i++) put_u32(p + 20 + i * 4,
            hw.link_mode && hw.link_fault != 10 ? 9 : 1);
        if (hw.mcc_changes) {
            hw.mcc_changes--;
            unsigned where = off;
            off += mock_packet(buf + off, 0xc9, 0, 0, 0x80, 4);
            put_u32(buf + where + 8, 0x054152);
        }
    }
    if (scan_abort) {
        if (size == 4) put_u32(response + 8, hw.scan_fault == 3 ? 2 : hw.scan_fault == 5 ? 3 : 0);
        if (hw.abort_complete) {
            unsigned where = off;
            off += mock_packet(buf + off, 0x0f, 0, 0, 0x80, 16);
            buf[where + 14] = 2;
        }
    }
    if (fault == 8) off += mock_packet(buf + off, h[0], h[1], index, 0, 0);
    if ((init && hw.init_after) || fault == 14)
        off += mock_packet(buf + off, 4, 0, hw.init_sequence, hw.init_sequence >> 8, hw.init_payload);
    put_u32(buf + off, IWX_RX_INVALID);
    uint32_t *used = dma_ptr(transport.used_ring);
    if (transport.streaming) {
        hw.rx_owned[slot] = false;
        used[hw.rx_producer] = fault == 26 ? 3 : slot + 1;
        hw.rx_producer = (hw.rx_producer + 1) & (IWX_RX_COUNT - 1);
        *(uint16_t *)dma_ptr(transport.status) = hw.rx_producer;
    } else {
        used[transport.read] = fault == 26 ? 3 : slot + 1;
        *(uint16_t *)dma_ptr(transport.status) = transport.read + 1;
    }
    hw.csr[IWX_INT / 4] |= 1U << 31;
}

static void inject_management(uint32_t doorbell)
{
    hw.tx_calls++;
    const uint8_t *p = dma_ptr(transport.tx_page);
    const uint8_t *t = (const uint8_t *)dma_ptr(transport.tx_descriptors) + transport.tx_index * 256;
    CHECK(hw.link_mode && transport.tx_pending && transport.tx_enabled && probe_lock);
    unsigned index = hw.tx_cursor & 255, tag = hw.tx_queue & 31;
    CHECK(transport.tx_index == index && transport.tx_queue == hw.tx_queue);
    CHECK((doorbell >> 16) == hw.tx_queue && (doorbell & 0xffff) == ((index + 1) & 255));
    CHECK(p[0] == 0x1c && !p[1] && p[2] == index && p[3] == tag);
    bool data = p[24] == 8;
    bool deauth = p[24] == 0xc0 && runtime.wpa.countermeasures;
    CHECK(data || deauth || p[24] == (runtime.link.status.state == IWX_LINK_AUTH ? 0xb0 : 0));
    CHECK(rx_le16(p + 6) == 0x0c00 && rx_le32(p + 8) == ((p[25] & 0x40) ? 1U : 3U));
    uint32_t tx_rate = rx_le32(p + 20);
    bool normal_data = data && !(p[70] == 0x88 && p[71] == 0x8e);
    if (normal_data) {
        CHECK((tx_rate & ~0x107U) == 0x4000);
        static const unsigned legacy[] = {2,4,11,22,12,18,24,36,48,72,96,108};
        unsigned at = tx_rate & 7;
        if (tx_rate & 0x100) at += 4;
        CHECK(at < sizeof(legacy) / sizeof(legacy[0]));
        bool offered = false;
        for (unsigned i = 0; i < runtime.link.nrates; i++)
            if ((runtime.link.rates[i] & 127) == legacy[at]) offered = true;
        CHECK(offered);
    } else CHECK(tx_rate == 0x4000); /* Management and EAPOL stay conservative. */
    CHECK(rx_le16(t) == 3 && rx_le16(t + 2) == 20 && rx_le16(t + 12) == 28);
    CHECK(rx_le32(t + 4) == transport.tx_page && rx_le32(t + 14) == transport.tx_page + 20);
    CHECK(rx_le32(t + 24) == transport.tx_page + 64 && rx_le16(t + 22) == rx_le16(p + 4) - 24);
    CHECK(((uint16_t *)dma_ptr(transport.tx_byte_count))[transport.tx_index] == (rx_le16(p + 4) + 3) / 4);
    uint8_t completion[48] = {1}, mpdu[78] = {30};
    unsigned completed_queue = hw.tx_queue ^ (hw.tx_fault == 8 ? 32 : 0);
    completion[36] = completed_queue; completion[37] = completed_queue >> 8;
    completion[40] = hw.tx_fault == 9 ? 0x83 : 1;
    hw.tx_cursor = (hw.tx_cursor + 1) & 4095;
    put_u32(completion + 44, hw.tx_fault == 2 ? hw.tx_cursor + 1 : hw.tx_cursor);
    if (hw.tx_fault == 7) completion[0] = 2;
    if (deauth) {
        CHECK(rx_le16(p + 4) == 26 && p[64] == 14 && !p[65] && !runtime.link.status.port_open);
        queue_fw_packet(0x1c, 0, index, tag, completion, sizeof(completion));
        return;
    }
    if (data) {
        if (transport.tx_data) {
            CHECK(!transport.tx_wpa_token && runtime.link.status.port_open);
            CHECK(!memcmp(p + 64, "\xaa\xaa\x03\x00\x00\x00", 6));
            CHECK(p[70] != 0x88 || p[71] != 0x8e);
            CHECK(!!(p[25] & 0x40) == runtime.link.status.rsn);
#ifdef IWX_NET_HOST_TEST
            extern void iwx_test_net_tx(const uint8_t *page);
            iwx_test_net_tx(p);
#endif
        } else {
            CHECK(transport.tx_wpa_token && runtime.wpa.inflight_tx == transport.tx_wpa_token);
            CHECK(!memcmp(p + 64, "\xaa\xaa\x03\x00\x00\x00\x88\x8e", 8));
        }
        if (!hw.eapol_hold)
            queue_fw_packet(0x1c, 0, index, tag, completion, sizeof(completion));
        return;
    }
    mpdu[12] = 3; mpdu[34] = 1;
    mpdu[48] = p[24] == 0xb0 ? 0xb0 : 0x10;
    memcpy(mpdu + 52, runtime.status.nvm.mac, 6);
    memcpy(mpdu + 58, "\x02\x01\x02\x03\x04\x05", 6);
    memcpy(mpdu + 64, mpdu + 58, 6);
    if (mpdu[48] == 0xb0) { mpdu[74] = 2; mpdu[76] = hw.tx_fault == 11 ? 13 : 0; }
    else { mpdu[72] = runtime.link.status.rsn ? 17 : 1; mpdu[74] = hw.tx_fault == 11 ? 17 : 0; mpdu[76] = 35; mpdu[77] = 0xc0; }
    if (hw.tx_fault == 10) mpdu[58] ^= 2;
    if (hw.tx_fault == 14) { hw.rfkill = true; hw.csr[IWX_GP_CNTRL / 4] &= ~IWX_RF_ENABLED; }
    if (hw.tx_fault == 15) {
        uint8_t mcc[4] = {0x52,0x41,5,0};
        queue_fw_packet(0xc9, 0, 0, 0x80, mcc, sizeof(mcc));
    }
    bool reply = hw.tx_fault != 13;
    if (hw.tx_reverse && reply) queue_fw_packet(0xc1, 0, 0, 0x80, mpdu, sizeof(mpdu));
    if (hw.tx_fault != 1) {
        queue_fw_packet(0x1c, 0, index + (hw.tx_fault == 5),
                        tag ^ (hw.tx_fault == 4 ? 1 : 0), completion, hw.tx_fault == 6 ? 47 : 48);
        if (hw.tx_fault == 3) queue_fw_packet(0x1c, 0, index, tag, completion, 48);
    }
    if (!hw.tx_reverse && reply) queue_fw_packet(0xc1, 0, 0, 0x80, mpdu, sizeof(mpdu));
}

void iwx_test_write8(uint64_t addr, uint8_t v)
{
    if (addr == hw.bar + IWX_INT_COALESCING && v == 0x40) hw.byte_writes = true;
    else if (addr != hw.bar + IWX_INT_PERIODIC || v != 0) hw.violations++;
}

void iwx_test_write(uint64_t addr, uint32_t v)
{
    hw.writes++;
    if (addr < hw.bar || addr >= hw.bar + 8192 || (addr & 3)) abort();
    unsigned reg = addr - hw.bar;
    if (((hw.pci[1] & 4) && !hw.allow_dma) || (hw.pci[0x44 / 4] & 3)) hw.violations++;
    if (reg == IWX_INT_MASK && v) hw.violations++;
    if (reg == IWX_INT_COALESCING) hw.violations++;
    if (reg == IWX_HBUS_TARG_WRPTR) {
        if (v >> 16) inject_management(v); else inject_command(v);
        return;
    }
    if (reg == IWX_INT || reg == IWX_FH_INT) {
        if (hw.started && ((hw.csr[IWX_INT / 4] & (IWX_INT_HW_ERR | IWX_INT_SW_ERR)) ||
                           (hw.csr[IWX_FH_INT / 4] & IWX_FH_ERROR))) hw.violations++;
        hw.csr[reg / 4] &= ~v; /* W1C; not ordinary read/write RAM. */
        return;
    }
    if (reg == IWX_RX_WIDX) {
        if (!hw.started || !transport.exposed || !(hw.pci[1] & 4)) hw.violations++;
        uint64_t *free_ring = dma_ptr(transport.free_ring);
        if (transport.streaming) {
            CHECK(v < 512 && !(v & 7) && probe_lock);
            for (unsigned i = hw.offered; i != v; i = (i + 1) & 511) {
                unsigned id = free_ring[i] & 0xfff;
                CHECK(id && id <= 504);
                if (!id || id > 504) abort();
                CHECK(free_ring[i] == (transport.rx[id - 1] | id) && !hw.rx_owned[id - 1]);
                CHECK(transport.owned[id - 1]);
                uint32_t *buf = dma_ptr(transport.rx[id - 1]);
                for (unsigned p = 0; p < 1024; p += 16) CHECK(buf[p] == IWX_RX_INVALID);
                hw.rx_owned[id - 1] = true;
            }
            hw.offered = v;
            hw.rx_doorbells++;
            if (!hw.injected && v == 504 && !hw.no_alive) inject_alive();
        } else {
            if (!hw.offered && v != 8) hw.violations++;
            hw.offered = v;
            for (unsigned i = 0; i < v; i++)
                if (free_ring[i] != (transport.rx[i] | (i + 1))) hw.violations++;
            if (v > 8 && !hw.no_alive) inject_alive();
        }
    }
    if (reg == IWX_PRPH_RADDR || reg == IWX_PRPH_WADDR) {
        CHECK((v & 0xfff00000U) == 0x03000000U);
        if (reg == IWX_PRPH_RADDR) hw.prph_addr = v & 0xfffff;
        else hw.prph_write = v & 0xfffff;
    }
    if (reg == IWX_PRPH_WDAT) {
        if (hw.prph_write == 0x05c44 && v == 1) {
            if (!hw.allow_dma || !hw.byte_writes || !transport.exposed ||
                !(hw.pci[1] & 4) || (hw.pci[0x48 / 4] & 0x10000) ||
                (hw.pci[0x58 / 4] & 0x80000000) || !(hw.pci[1] & 0x400)) hw.violations++;
            if (hw.csr[IWX_CTXT_INFO_BA / 4] != transport.context ||
                hw.csr[IWX_CTXT_INFO_BA / 4 + 1]) hw.violations++;
            iwx_context_t *c = dma_ptr(transport.context);
            if (c->size_dwords != 448 || c->control_flags != 0x990 ||
                c->free_rbd != transport.free_ring || c->used_rbd != transport.used_ring ||
                c->status != transport.status || c->cmd_queue_size != 5 ||
                c->cmd_queue != transport.command || c->mac_id != 0x350) hw.violations++;
            if (*(uint32_t *)dma_ptr(c->lmac[0]) != 0x44332211 ||
                *(uint32_t *)dma_ptr(c->umac[0]) != 0x88776655) hw.violations++;
            if (hw.paging) {
                if (!c->paging[0] || !c->paging[1] || c->paging[2] ||
                    *(uint32_t *)dma_ptr(c->paging[0]) != 0x12345678 ||
                    *(uint32_t *)dma_ptr(c->paging[1]) != 0xabcdef01) hw.violations++;
            } else if (c->paging[0]) hw.violations++;
            if (hw.csr[IWX_LTR_LONG / 4] != 0x88fa88fa) hw.violations++;
            hw.started = true;
            inject_trace_error(3);
            if (!hw.no_rfh) hw.csr[IWX_INT / 4] |= IWX_INT_ALIVE;
            if (hw.sw_error) hw.csr[IWX_INT / 4] |= IWX_INT_SW_ERR;
            hw.csr[IWX_INT / 4] |= hw.boot_cause;
            hw.csr[IWX_FH_INT / 4] |= hw.boot_fh;
            if (hw.corrupt_caps) hw.pci[0x60 / 4] |= 0x6000;
            if (hw.stop_on_launch) hw.stop_fail = true;
            if (hw.removed_on_launch) hw.removed = true;
            if (hw.bme_stuck_on_stop) hw.block_bme_stop = true;
        } else if (hw.prph_write != 0x03440 || (hw.wprot & IWX_WFPM_ACCESS))
            hw.violations++;
        else hw.hpm = v;
    }
    if (reg == IWX_HW_IF_CONFIG && (hw.owner_fail ||
        (hw.prepare_needed && !(v & IWX_PREPARE)))) v &= ~IWX_NIC_READY;
    if (reg == IWX_GP_CNTRL) {
        if (hw.started && hw.mac_release_stuck) v |= IWX_MAC_ACCESS;
        v &= ~(IWX_CLOCK_READY | IWX_RF_ENABLED);
        if ((v & IWX_INIT_DONE) && !hw.clock_fail) v |= IWX_CLOCK_READY;
        if (!hw.rfkill) v |= IWX_RF_ENABLED;
        if (hw.started && hw.diag_clock_fail && (v & IWX_MAC_ACCESS)) v &= ~IWX_CLOCK_READY;
    }
    if (reg == IWX_RESET) {
        if ((v & IWX_STOP_MASTER) && !hw.stop_fail) v |= IWX_MASTER_DISABLED;
        if (v & IWX_SW_RESET) {
            hw.csr[IWX_INT / 4] = hw.csr[IWX_FH_INT / 4] = 0;
            hw.csr[IWX_CTXT_INFO_BA / 4] = 0;
            hw.csr[IWX_CTXT_INFO_BA / 4 + 1] = 0;
            hw.csr[IWX_GP_CNTRL / 4] &= ~(IWX_CLOCK_READY | IWX_INIT_DONE | IWX_MAC_ACCESS);
            v = 0;
            hw.started = false;
        }
    }
    hw.csr[reg / 4] = v;
    if (reg == IWX_CTXT_INFO_BA + 4) inject_trace_error(2);
    if (reg == IWX_GP_CNTRL && hw.started && !(v & IWX_MAC_ACCESS)) inject_trace_error(4);
}

static void test_probe(void)
{
    iwx_probe_report_t r;
    reset_mock();
    CHECK(iwx_probe(&r) == 0);
    CHECK(r.clock_ready && r.rfkill_known && !r.rfkill && r.stopped);
    CHECK(r.command_before == 7 && r.command_after == 0x403);
    CHECK(!(hw.csr[IWX_GP_CNTRL / 4] & IWX_INIT_DONE));
    CHECK(!(hw.pci[0x48 / 4] & 0x10000));
    CHECK((hw.pci[0x58 / 4] >> 16) == 0x4000);
    CHECK(hw.violations == 0);
    CHECK(iwx_probe(&r) == 0 && r.stopped); /* Repeatable; no allocated DMA. */

    reset_mock(); hw.absent = true;
    CHECK(iwx_probe(&r) == -1 && hw.config_writes == 0 && hw.reads == 0);
    reset_mock(); hw.dev.device_id = 0x51f0;
    CHECK(iwx_probe(&r) == -1 && hw.config_writes == 0);
    reset_mock(); hw.pci[0] = 0xffffffff;
    CHECK(iwx_probe(&r) == -1 && hw.config_writes == 0);
    reset_mock(); probe_lock = 1;
    CHECK(iwx_probe(&r) == -1 && hw.config_writes == 0 && probe_lock == 1);

    for (unsigned ptr = 0; ptr < 256; ptr++) {
        if (ptr == 0 || (ptr >= 0x40 && !(ptr & 3))) continue;
        reset_mock(); hw.pci[0x34 / 4] = ptr;
        CHECK(iwx_probe(&r) == -1 && hw.config_writes == 0);
    }
    reset_mock(); hw.pci[0x58 / 4] = 0x80004011; /* Loop back to PM. */
    CHECK(iwx_probe(&r) == -1 && hw.config_writes == 0);
    reset_mock(); hw.pci[0x48 / 4] = 0x5801; /* Duplicate PM. */
    CHECK(iwx_probe(&r) == -1 && hw.config_writes == 0);
    reset_mock(); hw.power_fail = true;
    CHECK(iwx_probe(&r) == -1 && hw.reads == 0 && !(hw.pci[1] & 4));
    reset_mock(); hw.power_reset = true;
    CHECK(iwx_probe(&r) == 0 && r.command_after == 0x402 && hw.violations == 0);
    reset_mock(); hw.dma_latched = true;
    CHECK(iwx_probe(&r) == -1 && hw.reads == 0);
    reset_mock(); hw.msi_latched = true;
    CHECK(iwx_probe(&r) == -1 && hw.reads == 0 && !(hw.pci[1] & 4));

    for (unsigned type = 0; type < 8; type++) {
        if (type == 0 || type == 4) continue;
        reset_mock(); hw.pci[4] = (uint32_t)hw.bar | type;
        CHECK(iwx_probe(&r) == -1 && hw.maps == 0);
    }
    reset_mock(); hw.pci[4] = 4; hw.pci[5] = 0x40; hw.bar = 0x4000000000ULL;
    CHECK(iwx_probe(&r) == 0 && r.bar0 == hw.bar && hw.violations == 0);
    reset_mock(); hw.pci[4] = 0;
    CHECK(iwx_probe(&r) == -1 && hw.maps == 0);
    reset_mock(); hw.map_fail = true;
    CHECK(iwx_probe(&r) == -1 && hw.reads == 0);
    reset_mock(); hw.unmapped = true;
    CHECK(iwx_probe(&r) == -1 && hw.reads == 0);
    reset_mock(); hw.removed = true;
    CHECK(iwx_probe(&r) == -1 && !r.stopped && hw.writes == 0);
    reset_mock(); hw.csr[IWX_HW_REV / 4] = 0xa5a5a5a0;
    CHECK(iwx_probe(&r) == -1 && hw.writes == 0);
    reset_mock(); hw.prepare_needed = true;
    CHECK(iwx_probe(&r) == 0 && r.stopped && hw.violations == 0);
    reset_mock(); hw.owner_fail = true;
    CHECK(iwx_probe(&r) == -1 && r.stopped && !r.rfkill_known);
    CHECK(strcmp(r.stage, "NIC ownership") == 0);
    reset_mock(); hw.clock_fail = true;
    CHECK(iwx_probe(&r) == -1 && r.stopped && !r.clock_ready && !r.rfkill_known);
    CHECK(strcmp(r.stage, "MAC clock") == 0);
    reset_mock(); hw.rfkill = true;
    CHECK(iwx_probe(&r) == 0 && r.rfkill_known && r.rfkill && r.stopped);
    reset_mock(); hw.no_time = true;
    CHECK(iwx_probe(&r) == -1 && hw.reads == 0);
    reset_mock(); hw.stop_fail = true;
    CHECK(iwx_probe(&r) == -1 && r.clock_ready && !r.stopped);
    CHECK(strcmp(r.stage, "device stop") == 0 && !(hw.pci[1] & 4));
    reset_mock(); hw.hpm = IWX_PERSISTENCE;
    CHECK(iwx_probe(&r) == 0 && hw.hpm == 0 && hw.violations == 0);
    reset_mock(); hw.hpm = IWX_PERSISTENCE; hw.wprot = IWX_WFPM_ACCESS;
    CHECK(iwx_probe(&r) == -1 && hw.hpm == IWX_PERSISTENCE && hw.violations == 0);
    CHECK(strcmp(r.stage, "firmware persistence") == 0);
    reset_mock(); hw.hpm = 0xa5a5a5a0;
    CHECK(iwx_probe(&r) == 0 && r.stopped);
    CHECK(iwx_probe(NULL) == -1);
}

static uint8_t fw[8192];
static unsigned fwsize;
static void fw_tlv(unsigned type, unsigned bytes, const uint32_t *data)
{
    put_u32(fw + fwsize, type);
    put_u32(fw + fwsize + 4, bytes);
    memcpy(fw + fwsize + 8, data, bytes);
    fwsize += 8 + bytes;
}
static void boot_fixture(unsigned version)
{
    reset_mock();
    hw.allow_dma = true;
    hw.reported_version = version;
    memset(fw, 0, sizeof(fw));
    fwsize = 88;
    put_u32(fw + 4, IWX_TLV_MAGIC);
    uint32_t ver[] = {77, 0, 0}, phy = 0x330018, cpus = 2;
    uint32_t cmd = (version << 24) | 1;
    uint32_t lmac[] = {0x1000, 0x44332211}, umac[] = {0x8000, 0x88776655};
    uint32_t separator = IWX_CPU_SEPARATOR;
    fw_tlv(IWX_TLV_FW_VERSION, 12, ver);
    fw_tlv(IWX_TLV_PHY_SKU, 4, &phy);
    fw_tlv(IWX_TLV_NUM_CPU, 4, &cpus);
    fw_tlv(IWX_TLV_CMD_VERSIONS, 4, &cmd);
    fw_tlv(IWX_TLV_SEC_RT, 8, lmac);
    fw_tlv(IWX_TLV_SEC_RT, 4, &separator);
    fw_tlv(IWX_TLV_SEC_RT, 8, umac);
}

static void paging_fixture(void)
{
    boot_fixture(6);
    hw.paging = true;
    uint32_t size = 4096, separator = IWX_PAGING_SEPARATOR;
    uint32_t css[] = {0, 0x12345678};
    uint32_t page[1025] = {0x1000000, 0xabcdef01};
    fw_tlv(IWX_TLV_PAGING, 4, &size);
    fw_tlv(IWX_TLV_SEC_RT, 4, &separator);
    fw_tlv(IWX_TLV_SEC_RT, sizeof(css), css);
    fw_tlv(IWX_TLV_SEC_RT, sizeof(page), page);
}

static void test_boot(void)
{
    iwx_probe_report_t r;
    boot_fixture(6); hw.path_fail = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && hw.path_calls == 1);
    CHECK(r.dma_path.attempted && !r.dma_path.ready && r.error == r.dma_path.error);
    CHECK(!strcmp(r.stage, "PCI DMA path preparation") && !transport.exposed);
    CHECK(r.stopped && !hw.live_pages && !hw.violations && !(hw.pci[1] & 4));
    CHECK(!hw.started && !r.fault.captured && hw.allocations == hw.frees);
    for (unsigned version = 4; version <= 7; version++) {
        boot_fixture(version);
        CHECK(iwx_fw_boot(fw, fwsize, &r) == 0);
        CHECK(r.alive && r.rfh_ready && r.stopped && !r.retained_bytes && !r.stop_error);
        CHECK(r.alive_version == version && r.alive_status == 0xcafe && r.rx_packets == 1);
        CHECK(r.lmac_major == 0x11223344 && r.lmac_minor == 0x55667788);
        CHECK(r.umac_major == 0x778899aa && r.umac_minor == 0xbbccddee);
        CHECK(!hw.live_pages && !(hw.pci[1] & 4) && !hw.violations);
        CHECK(hw.allocations == hw.frees && hw.allocations == 519);
        CHECK(r.dma_path.ready && hw.path_calls == 1 && hw.path_ready);
        for (int repeat = 0; repeat < 3; repeat++) {
            hw.next_page = 0; hw.offered = 0;
            CHECK(iwx_fw_boot(fw, fwsize, &r) == 0 && r.alive && !hw.violations);
            CHECK(hw.allocations == hw.frees && !hw.live_pages);
        }
    }
    for (unsigned fail = 1; fail <= 519; fail++) {
        boot_fixture(6); hw.fail_at = fail;
        CHECK(iwx_fw_boot(fw, fwsize, &r) == -1);
        CHECK(!hw.live_pages && !hw.violations && !r.alive && !transport.exposed);
        CHECK(!hw.path_calls);
    }
    boot_fixture(6); hw.no_dma_mapping = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && !hw.live_pages && !hw.violations);
    boot_fixture(6); hw.rfkill = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && !hw.allocations && r.stopped);
    boot_fixture(6); hw.absent = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && !hw.allocations && !hw.config_writes);
    boot_fixture(3);
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && !hw.allocations && !hw.config_writes);
    for (unsigned failure = 0; failure < 13; failure++) {
        boot_fixture(6);
        switch (failure) {
        case 0: hw.no_rfh = true; break;
        case 1: hw.no_alive = true; break;
        case 2: hw.bad_status = true; break;
        case 3: hw.bad_length = true; break;
        case 4: hw.bad_id = true; break;
        case 5: hw.bad_cursor = true; break;
        case 6: hw.duplicate_id = true; break;
        case 7: hw.sw_error = true; break;
        case 8: hw.reported_version = 4; break;
        case 9: hw.block_bme_start = true; break;
        case 10: hw.clock_fail = true; break;
        case 11: hw.request_only = true; break;
        case 12: hw.wrong_queue = true; break;
        }
        CHECK(iwx_fw_boot(fw, fwsize, &r) == -1);
        CHECK(r.stopped && !hw.live_pages && !hw.violations && !(hw.pci[1] & 4));
        if (failure == 1) CHECK(r.rfh_ready && !r.alive);
    }
    boot_fixture(6); hw.stop_on_launch = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && r.alive && !r.stopped);
    CHECK(r.retained_bytes && hw.live_pages && !hw.frees && !(hw.pci[1] & 4));
    CHECK(iwx_retained_bytes() == r.retained_bytes && !hw.violations);
    unsigned allocations = hw.allocations, writes = hw.writes;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && hw.allocations == allocations && hw.writes == writes);
    CHECK(iwx_probe(&r) == -1 && hw.writes == writes);
    CHECK(iwx_handoff_begin() == -1 && !probe_lock && hw.live_pages);
    hw.stop_fail = false;
    CHECK(iwx_stop(&r) == 0 && r.stopped && !hw.live_pages && !iwx_retained_bytes());
    CHECK(!hw.violations && hw.frees == hw.allocations);

    paging_fixture();
    CHECK(iwx_fw_boot(fw, fwsize, &r) == 0 && !hw.violations);
    CHECK(hw.allocations == 521 && hw.frees == 521 && !hw.live_pages);
    for (unsigned fail = 520; fail <= 521; fail++) {
        paging_fixture(); hw.fail_at = fail;
        CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && !hw.live_pages && !hw.violations);
    }
    paging_fixture(); hw.stop_on_launch = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && r.alive && !hw.frees);
    CHECK(transport.count == 521 && r.retained_bytes == (uint64_t)hw.live_pages * 4096);
    hw.stop_fail = false;
    CHECK(iwx_stop(&r) == 0 && hw.frees == 521 && !hw.violations);

    boot_fixture(6); hw.bme_stuck_on_stop = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && r.alive && !r.stopped);
    CHECK(r.retained_bytes && !hw.frees && !hw.violations && (hw.pci[1] & 4));
    hw.block_bme_stop = false;
    CHECK(iwx_stop(&r) == 0 && !hw.live_pages && !hw.violations);

    boot_fixture(6); hw.removed_on_launch = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && r.retained_bytes && !hw.frees);
    CHECK(r.error && r.stop_error && !r.stopped);
    hw.pci[0] = 0xffffffff;
    CHECK(iwx_stop(&r) == -1 && !hw.frees);
    hw.pci[0] = 0x27238086;
    hw.removed = false;
    CHECK(iwx_stop(&r) == 0 && !hw.live_pages && !hw.violations);

    boot_fixture(6); hw.bad_status = true; hw.stop_on_launch = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && !r.alive && r.retained_bytes);
    CHECK(r.error && r.stop_error && strcmp(r.error, r.stop_error));
    hw.stop_fail = false;
    CHECK(iwx_handoff_begin() == 0 && !hw.live_pages && probe_lock);
    iwx_handoff_end();

    boot_fixture(6);
    CHECK(iwx_handoff_begin() == 0 && probe_lock);
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && !hw.allocations && !hw.config_writes);
    CHECK(iwx_probe(&r) == -1 && iwx_stop(&r) == -1);
    iwx_handoff_end();
    CHECK(!probe_lock && iwx_fw_boot(fw, fwsize, &r) == 0 && !hw.live_pages);
    CHECK(iwx_stop(&r) == 0 && r.stopped);
    CHECK(iwx_fw_boot(fw, fwsize, NULL) == -1 && iwx_stop(NULL) == -1);
}

static void test_boot_snapshot(void)
{
    iwx_probe_report_t r;
    boot_fixture(6);
    CHECK(iwx_fw_boot(fw, fwsize, &r) == 0 && !r.fault.captured && !hw.diag_reads);
    boot_fixture(6); hw.fail_at = 1;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && !r.fault.captured && !hw.diag_reads);

    for (unsigned error = 0; error < 4; error++) {
        boot_fixture(6); hw.no_rfh = true;
        hw.boot_cause = error == 0 ? IWX_INT_HW_ERR : error == 1 ? IWX_INT_SW_ERR : 0;
        hw.boot_fh = error >= 2 ? IWX_FH_ERROR : 0;
        if (error == 3) hw.boot_cause = IWX_INT_HW_ERR | IWX_INT_SW_ERR;
        CHECK(iwx_fw_boot(fw, fwsize, &r) == -1);
        CHECK(strcmp(r.error, error == 1 ? "firmware software error interrupt" :
                                         "hardware/DMA error interrupt") == 0);
        const iwx_boot_snapshot_t *s = &r.fault;
        CHECK(s->captured && s->pci_valid && s->csr_valid && s->pcie_valid);
        CHECK(s->interrupt == hw.boot_cause && s->fh_interrupt == hw.boot_fh);
        CHECK((s->pci_command_status & 0xffff) == 0x407 && r.command_after == 0x403);
        CHECK(s->pcie_device_control == 0x0810 && s->pcie_device_status == 0x000a);
        CHECK(s->pcie_link_control == 0x0042 && s->pcie_link_status == 0x1011);
        CHECK(hw.pci[0x68 / 4] == 0x000a0810 && hw.pci[0x70 / 4] == 0x10110042);
        CHECK(s->context_expected == DMA_BASE && s->context_observed == DMA_BASE);
        CHECK(s->context_mac == 0x350 && s->context_dwords == 448 && s->context_flags == 0x990);
        CHECK(s->free_ring == DMA_BASE + 4096 && s->used_ring == DMA_BASE + 8192);
        CHECK(s->status == DMA_BASE + 12288 && s->lmac && s->umac && !s->paging);
        CHECK(s->dma_bytes == 534 * 4096 && !s->rx_read && !s->rx_producer);
        CHECK(s->prph_count == IWX_BOOT_PRPH_SAMPLES && !s->prph_error);
        for (unsigned i = 0; i < s->prph_count; i++)
            CHECK(s->prph[i].valid && s->prph[i].value == (0x51000000 | (s->prph[i].address & 0xffff)));
        CHECK(!hw.csr[IWX_INT / 4] && !hw.csr[IWX_FH_INT / 4] && !hw.csr[IWX_CTXT_INFO_BA / 4]);
        CHECK(r.stopped && !r.stop_error && !hw.live_pages && !hw.violations);
        unsigned reads = hw.reads, writes = hw.writes;
        snapshot_transport(&r, NULL);
        CHECK(hw.reads == reads && hw.writes == writes && s->context_expected == DMA_BASE);
    }
    boot_fixture(6); hw.boot_cause = IWX_INT_HW_ERR; hw.diag_clock_fail = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1);
    CHECK(!strcmp(r.error, "hardware/DMA error interrupt") && r.fault.prph_error);
    CHECK(!strcmp(r.fault.prph_error, "register wait timed out") && !r.fault.prph_count);
    CHECK(r.stopped && !hw.live_pages && !hw.violations && hw.reads < POLL_LIMIT);
    CHECK(!(hw.csr[IWX_GP_CNTRL / 4] & IWX_MAC_ACCESS));

    boot_fixture(6); hw.boot_cause = IWX_INT_HW_ERR; hw.diag_sentinel = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && r.fault.prph_count == IWX_BOOT_PRPH_SAMPLES);
    for (unsigned i = 0; i < r.fault.prph_count; i++) CHECK(!r.fault.prph[i].valid);
    CHECK(r.stopped && !hw.live_pages && !hw.violations);

    boot_fixture(6); hw.boot_cause = IWX_INT_HW_ERR; hw.corrupt_caps = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && !r.fault.pcie_valid && r.fault.pci_error);
    CHECK(r.fault.csr_valid && r.fault.prph_count == IWX_BOOT_PRPH_SAMPLES);
    CHECK(r.stopped && !hw.live_pages && !hw.violations);

    boot_fixture(6); hw.removed_on_launch = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && r.fault.captured && !r.fault.csr_valid);
    CHECK(r.fault.prph_error && !r.fault.prph_count && !hw.diag_reads && !hw.frees);
    CHECK(!strcmp(r.error, "MMIO inaccessible"));
    hw.removed = false;
    CHECK(iwx_stop(&r) == 0 && !hw.live_pages && !r.fault.captured && !hw.violations);

    boot_fixture(6); hw.boot_cause = IWX_INT_HW_ERR; hw.stop_on_launch = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && r.fault.interrupt & IWX_INT_HW_ERR);
    CHECK(r.fault.captured && r.stop_error && r.retained_bytes && !hw.frees);
    CHECK(!strcmp(r.error, "hardware/DMA error interrupt"));
    hw.stop_fail = false;
    CHECK(iwx_stop(&r) == 0 && !hw.live_pages && !hw.violations);
}

static void test_snapshot_pcie(void)
{
    for (unsigned kind = 0; kind < 8; kind++) {
        reset_mock();
        iwx_boot_snapshot_t s = {.pci_command_status = hw.pci[1]};
        switch (kind) {
        case 0: s.pci_command_status &= ~(1U << 20); break;
        case 1: hw.pci[0x34 / 4] = 0x3c; break;
        case 2: hw.pci[0x34 / 4] = 0x41; break;
        case 3: hw.pci[0x60 / 4] |= 0x6000; break;
        case 4: hw.pci[0x34 / 4] = 0xfc; hw.pci[0xfc / 4] = 0x10; break;
        case 5: hw.pci[0x60 / 4] = 0x00007010; hw.pci[0x70 / 4] = 0x10; break;
        case 6: hw.pci[0x60 / 4] = 0x00920010; break;
        case 7: hw.pci[0x68 / 4] = 0xffffffff; break;
        }
        snapshot_pcie(&hw.dev, &s);
        CHECK(!s.pcie_valid && s.pci_error && !hw.config_writes && !hw.writes);
    }
}

static void test_boot_trace(void)
{
    static const char *const phases[] = {
        "before-prepare", "before-card-reset", "after-prepare", "dma-ready", "before-bme",
        "after-bme", "after-context", "before-cpu-run", "after-cpu-run",
        "after-mac-release", "first-poll", "rfh-alive-interrupt",
        "alive-confirmed", "before-stop"
    };
    iwx_probe_report_t r;
    boot_fixture(6);
    CHECK(iwx_fw_boot(fw, fwsize, &r) == 0);
    CHECK(r.trace_count == sizeof(phases) / sizeof(phases[0]) && !r.trace_dropped);
    CHECK(r.poll_count == 1 && r.stopped && !hw.live_pages && !hw.violations);
    for (unsigned i = 0; i < r.trace_count; i++) {
        const iwx_boot_trace_sample_t *t = &r.trace[i];
        CHECK(!strcmp(t->phase, phases[i]) && t->pci_valid && t->pcie_valid && t->pm_valid);
        CHECK(t->csr_sampled == (i != 0) && t->csr_valid == (i != 0) && !t->note);
        CHECK(t->pcie_device_status == 0xa && t->pcie_device_control == 0x810);
        CHECK(t->pcie_link_control == 0x42 && t->pcie_link_status == 0x1011);
        CHECK(i == 0 || t->elapsed_us >= r.trace[i - 1].elapsed_us);
    }
    CHECK(r.trace[0].pm_control == 0x8003 && r.trace[1].pm_control == 0);
    CHECK(r.trace[0].pci_command_status == 0x100007);
    CHECK(!(r.trace[4].pci_command_status & 4) && (r.trace[5].pci_command_status & 4));
    CHECK(r.trace[5].context_expected == DMA_BASE && !r.trace[5].context_observed);
    CHECK(r.trace[6].context_observed == DMA_BASE && r.trace[6].dma_bytes == 534 * 4096);
    CHECK(r.trace[8].gp_cntrl & IWX_MAC_ACCESS);
    CHECK(!(r.trace[9].gp_cntrl & IWX_MAC_ACCESS));
    CHECK(r.trace[13].rx_producer == 1 && r.trace[13].rx_read == 1);
    CHECK(!hw.csr[IWX_CTXT_INFO_BA / 4] && r.trace[13].context_observed == DMA_BASE);
    CHECK(r.platform.prepared && r.platform.sample[0].captured && r.platform.sample[1].captured);
    CHECK(r.platform.sample[0].aer[0].command_status == 0x100007);
    CHECK(r.platform.sample[1].aer[0].command_status == 0x100407);
    CHECK(!r.platform.iommu_ready && r.platform.dmar_note);

    const unsigned first[] = {0, 5, 6, 8, 9};
    for (unsigned step = 1; step <= 4; step++) {
        boot_fixture(6); hw.trace_error_at = step; hw.no_rfh = true;
        CHECK(iwx_fw_boot(fw, fwsize, &r) == -1);
        CHECK(!strcmp(r.error, "hardware/DMA error interrupt"));
        CHECK(r.trace_count == 12 && !r.trace_dropped && r.poll_count == 1);
        for (unsigned i = 0; i < r.trace_count; i++) {
            CHECK(!!(r.trace[i].interrupt & (1U << 29)) == (i >= first[step]));
            CHECK(r.trace[i].pcie_device_status == (i >= first[step] ? 0xb : 0xa));
        }
        CHECK(!strcmp(r.trace[11].phase, "before-stop"));
        CHECK(r.fault.interrupt & (1U << 29));
        CHECK(r.stopped && !hw.live_pages && !hw.violations && !r.retained_bytes);
    }

    boot_fixture(6); hw.no_rfh = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && r.poll_count > 1);
    CHECK(r.trace_count == 12 && !r.trace_dropped && !hw.violations);
    boot_fixture(6); hw.fail_at = 1;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && r.trace_count == 4 && !r.poll_count);
    CHECK(r.trace[3].dma_bytes == 19 * 4096 && !r.trace[3].context_expected && !hw.live_pages);
    boot_fixture(6); hw.removed_on_launch = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && r.trace_count == 11 && !r.poll_count);
    CHECK(r.trace[8].csr_sampled && !r.trace[8].csr_valid && r.trace[8].note);
    CHECK(r.retained_bytes && !hw.frees && !hw.violations);
    hw.removed = false;
    CHECK(iwx_stop(&r) == 0 && !r.trace_count && !hw.live_pages);
    boot_fixture(6); hw.corrupt_caps = true; hw.boot_cause = IWX_INT_HW_ERR;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && r.trace[7].pcie_valid);
    CHECK(!r.trace[8].pcie_valid && r.trace[8].note && r.trace[8].csr_valid);
    CHECK(r.stopped && !hw.live_pages && !hw.violations);

    boot_fixture(6); hw.csr[IWX_INT / 4] = 1U << 29;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == 0);
    CHECK(r.trace[1].interrupt == (1U << 29) && !r.trace[2].interrupt);
    CHECK(!hw.violations && r.stopped && !hw.live_pages);
    boot_fixture(6); hw.absent = true;
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && !r.trace_count && !hw.reads);
    boot_fixture(3);
    CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && !r.trace_count && !hw.reads);

    reset_mock(); hw.allow_dma = true;
    memset(&r, 0, sizeof(r)); r.bar0 = hw.bar; r.error = "original";
    trace_checkpoint(&hw.dev, &r, "pci-only", false);
    CHECK(r.trace_count == 1 && !hw.reads && !hw.writes && !hw.config_writes && !hw.allocations);
    trace_checkpoint(&hw.dev, &r, "d3-skip-mmio", true);
    CHECK(!r.trace[1].csr_sampled && r.trace[1].note && !hw.reads);
    hw.pci[0x44 / 4] = 0;
    hw.pci[4] += 4096;
    trace_checkpoint(&hw.dev, &r, "bar-changed", true);
    CHECK(!r.trace[2].csr_sampled && r.trace[2].note && !hw.reads);
    hw.pci[4] -= 4096;
    hw.pci[0] = 0xffffffff;
    trace_checkpoint(&hw.dev, &r, "removed", true);
    CHECK(!r.trace[3].pci_valid && !r.trace[3].csr_sampled && !hw.reads);
    hw.pci[0] = 0x27238086;
    while (r.trace_count < IWX_BOOT_TRACE_SAMPLES)
        trace_checkpoint(&hw.dev, &r, "capacity", false);
    unsigned reads = hw.reads;
    trace_checkpoint(&hw.dev, &r, "overflow", true);
    CHECK(r.trace_count == IWX_BOOT_TRACE_SAMPLES && r.trace_dropped == 1 && hw.reads == reads);
    CHECK(!strcmp(r.error, "original") && !hw.config_writes && !hw.writes && !hw.allocations);
    CHECK(!hw.diag_reads && !hw.violations);
}

typedef struct {
    const char *phase[64];
    unsigned count, fail_at;
    bool reenter;
} observer_test_t;

static bool observe_boot(void *arg, const char *phase, const iwx_probe_report_t *r)
{
    observer_test_t *o = arg;
    CHECK(o->count < 64 && r->progress_count == o->count + 1);
    CHECK(probe_lock && !(hw.csr[IWX_GP_CNTRL / 4] & IWX_MAC_ACCESS));
    o->phase[o->count++] = phase;
    if (!strcmp(phase, "wlan-bridge-enable"))
        CHECK(hw.live_pages && !transport.exposed && !(hw.pci[1] & 4));
    if (!strcmp(phase, "wlan-bridge-ready")) CHECK(hw.path_ready && !hw.started);
    if (!strcmp(phase, "wlan-wait-alive")) CHECK(hw.started && transport.exposed);
    if (!strcmp(phase, "wlan-dma-release")) CHECK(r->stopped && !hw.started && !(hw.pci[1] & 4));
    if (o->reenter) {
        unsigned writes = hw.writes, cfg = hw.config_writes, alloc = hw.allocations;
        iwx_probe_report_t nested;
        CHECK(iwx_probe(&nested) == -1 && iwx_stop(&nested) == -1);
        CHECK(iwx_handoff_begin() == -1 && probe_lock);
        CHECK(writes == hw.writes && cfg == hw.config_writes && alloc == hw.allocations);
    }
    /* Model a slow filesystem callback without an extra register access. */
    hw.now += 1000000;
    return o->count != o->fail_at;
}

static void test_persistent_progress(void)
{
    static const char *const expected[] = {
        "wlan-fw-validate", "wlan-platform-prepare", "wlan-card-prepare",
        "wlan-pci-ready", "wlan-mmio-ready", "wlan-card-ready", "wlan-dma-allocate",
        "wlan-bridge-enable", "wlan-bridge-ready", "wlan-launch", "wlan-wait-alive",
        "wlan-rfh-ready", "wlan-rx-first", "wlan-alive", "wlan-cleanup",
        "wlan-platform-snapshot", "wlan-dma-stop", "wlan-dma-release", "wlan-finished"
    };
    iwx_probe_report_t r;
    boot_fixture(6);
    CHECK(iwx_fw_boot(fw, fwsize, &r) == 0 && !r.progress_count && !r.progress_failed_at);
    unsigned writes = hw.writes, reads = hw.reads, cfg = hw.config_writes;
    boot_fixture(6);
    observer_test_t o = {.reenter = true};
    CHECK(iwx_fw_boot_traced(fw, fwsize, &r, observe_boot, &o) == 0);
    CHECK(o.count == sizeof(expected) / sizeof(expected[0]) && !r.progress_deferred);
    for (unsigned i = 0; i < o.count; i++) CHECK(!strcmp(o.phase[i], expected[i]));
    CHECK(!hw.live_pages && !hw.violations && !probe_lock && r.alive && r.stopped);
    CHECK(writes == hw.writes && reads == hw.reads && cfg == hw.config_writes);

    for (unsigned fail = 1; fail <= sizeof(expected) / sizeof(expected[0]); fail++) {
        boot_fixture(6);
        o = (observer_test_t){.fail_at = fail};
        CHECK(iwx_fw_boot_traced(fw, fwsize, &r, observe_boot, &o) == -1);
        CHECK(o.count == fail && !strcmp(r.progress_failed_at, expected[fail - 1]));
        CHECK(!strcmp(r.error, "persistent progress checkpoint failed"));
        CHECK(!hw.live_pages && !hw.violations && !probe_lock);
        if (fail <= 3) CHECK(!hw.config_writes && !hw.allocations);
        if (fail <= 10) CHECK(!hw.started && !r.alive);
    }

    boot_fixture(6); hw.boot_cause = IWX_INT_HW_ERR;
    o = (observer_test_t){.fail_at = 12}; /* cleanup observer fails after hardware error */
    CHECK(iwx_fw_boot_traced(fw, fwsize, &r, observe_boot, &o) == -1);
    CHECK(!strcmp(r.error, "hardware/DMA error interrupt") && r.progress_failed_at);
    CHECK(r.fault.captured && r.stopped && !hw.live_pages && !hw.violations);

    boot_fixture(6); hw.no_rfh = true;
    o = (observer_test_t){0};
    CHECK(iwx_fw_boot_traced(fw, fwsize, &r, observe_boot, &o) == -1);
    CHECK(r.poll_count > 1 && o.count < 20 && !r.progress_failed_at && !hw.live_pages);
    boot_fixture(6); hw.no_alive = true;
    o = (observer_test_t){0};
    CHECK(iwx_fw_boot_traced(fw, fwsize, &r, observe_boot, &o) == -1);
    CHECK(r.rfh_ready && !r.alive && o.count < 21 && !hw.live_pages && !hw.violations);

    boot_fixture(6); hw.block_bme_start = true;
    o = (observer_test_t){0};
    CHECK(iwx_fw_boot_traced(fw, fwsize, &r, observe_boot, &o) == -1);
    CHECK(r.progress_deferred && r.stopped && !hw.live_pages && !hw.violations);
    CHECK(!strcmp(o.phase[o.count - 1], "wlan-finished"));
    boot_fixture(6); hw.mac_release_stuck = true;
    o = (observer_test_t){0};
    CHECK(iwx_fw_boot_traced(fw, fwsize, &r, observe_boot, &o) == 0);
    CHECK(r.progress_deferred && r.stopped && !hw.live_pages && !hw.violations);
    CHECK(!strcmp(o.phase[o.count - 3], "wlan-launch"));
    boot_fixture(6); hw.removed_on_launch = true;
    o = (observer_test_t){0};
    CHECK(iwx_fw_boot_traced(fw, fwsize, &r, observe_boot, &o) == -1);
    CHECK(r.progress_deferred && r.retained_bytes && !hw.frees && !hw.violations);
    CHECK(!strcmp(o.phase[o.count - 1], "wlan-launch"));
    hw.removed = false;
    CHECK(iwx_stop(&r) == 0 && !hw.live_pages);

    boot_fixture(6); hw.stop_on_launch = true;
    o = (observer_test_t){.fail_at = 11};
    CHECK(iwx_fw_boot_traced(fw, fwsize, &r, observe_boot, &o) == -1);
    CHECK(r.progress_failed_at && r.retained_bytes && !hw.frees && !hw.violations);
    hw.stop_fail = false;
    CHECK(iwx_stop(&r) == 0 && !hw.live_pages);

    boot_fixture(6); probe_lock = 1;
    o = (observer_test_t){0};
    CHECK(iwx_fw_boot_traced(fw, fwsize, &r, observe_boot, &o) == -1 && !o.count);
    probe_lock = 0;
    CHECK(iwx_fw_boot_traced(fw, fwsize, NULL, observe_boot, &o) == -1 && !o.count);
    memset(&r, 0, sizeof(r));
    r.progress_count = 64;
    boot_progress_t p = {.emit = observe_boot, .arg = &o};
    CHECK(!progress_mark(&p, &r, "limit") && r.progress_failed_at && !o.count);
}

static void nvm_fixture(unsigned version)
{
    boot_fixture(6);
    for (unsigned p = 88; p < fwsize; p += 8 + rx_le32(fw + p + 4)) {
        if (rx_le32(fw + p) != IWX_TLV_CMD_VERSIONS) continue;
        memmove(fw + p + 20, fw + p + 12, fwsize - p - 12);
        put_u32(fw + p + 4, 12);
        put_u32(fw + p + 12, (version << 24) | 0x00010c02);
        put_u32(fw + p + 16, 0x00010c00);
        fwsize += 8;
        break;
    }
    uint32_t api[] = {1, version == 4 ? 1U << 16 : 0};
    fw_tlv(IWX_TLV_API_CHANGES, sizeof(api), api);
    hw.nvm_version = version;
    hw.init_sequence = 0x8000;
    hw.csr[IWX_MAC_STRAP0 / 4] = 0x00112233;
    hw.csr[IWX_MAC_STRAP1 / 4] = 0x4455;
}

static void test_nvm(void)
{
    iwx_probe_report_t r;
    iwx_nvm_report_t n;
    static const uint8_t mac[] = {0, 0x11, 0x22, 0x33, 0x44, 0x55};
    for (unsigned version = 3; version <= 4; version++) {
        for (unsigned order = 0; order < 2; order++) {
            nvm_fixture(version); hw.init_after = order != 0;
            CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, NULL, NULL) == 0);
            CHECK(r.alive && r.stopped && !r.error && n.valid && n.init_complete);
            CHECK(n.commands_sent == 3 && n.commands_completed == 3 && n.command_polls == 4);
            CHECK(n.response_version == version && n.response_bytes == (version == 4 ? 468 : 128));
            CHECK(n.nvm_version == 0x1234 && n.board_type == 7 && n.n_hw_addrs == 2);
            CHECK(n.mac_sku == 0x1f && n.tx_chains == 3 && n.rx_chains == 3 && n.lar_enabled == 1);
            CHECK(n.n_channels == 51 && !memcmp(n.mac, mac, 6) && !n.mac_from_otp);
            CHECK(n.channel_profile[0] == 0x701 && n.channel_profile[50] == 0x765);
            CHECK(!hw.live_pages && !hw.violations && !transport.nvm && !probe_lock);
        }
    }
    /* A second session may reuse physical pages, but never command/RX state. */
    hw.next_page = hw.offered = hw.command_calls = 0;
    CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, NULL, NULL) == 0 && n.valid);
    CHECK(n.commands_completed == 3 && !hw.live_pages && !hw.violations);

    /* INIT_COMPLETE is an event, not an echoed command index or an empty struct. */
    static const unsigned sequences[] = {0x8001, 0x8100, 0xffff};
    for (unsigned i = 0; i < sizeof(sequences) / sizeof(sequences[0]); i++) {
        for (unsigned order = 0; order < 2; order++) {
            for (unsigned length = 0; length <= 4; length += 4) {
                nvm_fixture(4);
                hw.init_sequence = sequences[i]; hw.init_payload = length; hw.init_after = order != 0;
                CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, NULL, NULL) == 0);
                CHECK(n.valid && n.init_complete && n.commands_completed == 3);
                CHECK(n.init_notifications == 1 && n.init_sequence == sequences[i] &&
                      n.init_payload_bytes == length);
                CHECK(r.stopped && !hw.live_pages && !hw.violations);
            }
        }
    }

    nvm_fixture(4);
    hw.csr[IWX_MAC_STRAP0 / 4] = hw.csr[IWX_MAC_STRAP1 / 4] = 0;
    hw.csr[IWX_MAC_OTP0 / 4] = 0x00112233;
    hw.csr[IWX_MAC_OTP1 / 4] = 0x4455;
    CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, NULL, NULL) == 0 && n.mac_from_otp);

    for (unsigned fault = 1; fault <= 30; fault++) {
        nvm_fixture(4);
        hw.command_fault = fault;
        hw.fault_command = (fault >= 13 && fault <= 15) ? 2 :
                           (fault >= 17 && fault <= 21) || fault >= 27 ? 3 : 1;
        if (fault == 27) hw.csr[IWX_MAC_STRAP0 / 4] = hw.csr[IWX_MAC_STRAP1 / 4] = 0;
        if (fault == 28) { /* A firmware can acknowledge, yet fail to stop DMA. */
            hw.stop_on_launch = true;
        }
        CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, NULL, NULL) == -1 && r.error);
        CHECK(fault == 28 || !n.valid);
        CHECK(!hw.violations && !probe_lock && !transport.nvm);
        if (fault == 25 || fault == 28) {
            CHECK(r.retained_bytes && !hw.frees);
            hw.no_time = hw.stop_fail = false;
            CHECK(iwx_stop(&r) == 0);
        } else CHECK(r.stopped && !r.retained_bytes);
        CHECK(!hw.live_pages && !iwx_retained_bytes());
    }
    /* Ordinary ALIVE diagnostics must not require or use the NVM ABI. */
    boot_fixture(6);
    CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, NULL, NULL) == -1 && !hw.allocations && !hw.writes);
    CHECK(iwx_fw_boot(fw, fwsize, &r) == 0 && !hw.command_calls);
    nvm_fixture(5);
    CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, NULL, NULL) == -1 && !hw.allocations && !hw.writes);
    nvm_fixture(4);
    fw[fwsize - 2] = 0; /* API bit 48 no longer agrees with response v4. */
    CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, NULL, NULL) == -1 && !hw.writes);
    nvm_fixture(4); hw.absent = true;
    CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, NULL, NULL) == -1 && !hw.allocations);
    nvm_fixture(4);
    CHECK(iwx_fw_nvm(fw, fwsize, NULL, &n, NULL, NULL) == -1 && !n.valid);
    CHECK(iwx_fw_nvm(fw, fwsize, &r, NULL, NULL, NULL) == -1 && !hw.writes);
    probe_lock = 1;
    CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, NULL, NULL) == -1 && !hw.writes);
    probe_lock = 0;

    for (unsigned fail = 1; fail <= 520; fail++) {
        nvm_fixture(4); hw.fail_at = fail;
        CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, NULL, NULL) == -1);
        CHECK(!hw.live_pages && !hw.command_calls && !hw.violations);
    }
    nvm_fixture(4);
    observer_test_t o = {.reenter = true};
    CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, observe_boot, &o) == 0 && n.valid);
    CHECK(o.count == 25 && !r.progress_deferred && !hw.violations);
    unsigned count = o.count;
    for (unsigned fail = 1; fail <= count; fail++) {
        nvm_fixture(4); o = (observer_test_t){.fail_at = fail};
        CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, observe_boot, &o) == -1);
        CHECK(o.count == fail && r.progress_failed_at && !hw.live_pages && !hw.violations);
    }
}

static void test_nvm_decode(void)
{
    uint8_t p[468] = {0};
    put_u32(p + 12, 3); put_u32(p + 16, 1); put_u32(p + 24, 110);
    put_u32(p + 28 + 109 * 4, 0x80000555);
    iwx_nvm_report_t n = {.response_version = 4};
    CHECK(decode_nvm(&n, p, sizeof(p)) && n.n_channels == 110);
    CHECK(n.channel_profile[109] == 0x80000555 && !n.valid);
    for (unsigned size = 0; size < 600; size++) {
        CHECK(decode_nvm(&n, p, size) == (size == sizeof(p)));
    }
    put_u32(p + 24, 0); CHECK(!decode_nvm(&n, p, sizeof(p)));
    put_u32(p + 24, 111); CHECK(!decode_nvm(&n, p, sizeof(p)));
    put_u32(p + 24, UINT32_MAX); CHECK(!decode_nvm(&n, p, sizeof(p)));
    uint8_t invalid[][6] = {{0}, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
                            {1, 2, 3, 4, 5, 6}, {2, 0xcc, 0xaa, 0xff, 0xee, 0}};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) CHECK(!valid_mac(invalid[i]));
}

static void test_notification_sequences(void)
{
    CHECK(!hw.live_pages && !transport.count && !runtime.active);
    uint8_t buf[4096];
    iwx_nvm_report_t n;
    iwx_probe_report_t r;
    static const uint8_t codes[] = {IWX_INIT_COMPLETE, IWX_MCC_CHANGED,
        IWX_SCAN_COMPLETE_NOTIF, IWX_SESSION_NOTIFICATION, IWX_RX_MPDU};
    static const unsigned sizes[] = {4, 4, 16, 16, 1};
    transport.nvm = &n;
    transport.command_pending = transport.init_waiting = true;
    transport.response[0] = 0xa5;
    transport.response_size = 7;
    runtime.active = true;
    for (unsigned sequence = 0; sequence <= 0xffff; sequence++) {
        bool event = (sequence & 0x8000) != 0;
        for (unsigned kind = 0; kind < sizeof(codes); kind++) {
            r = (iwx_probe_report_t){0};
            n = (iwx_nvm_report_t){.commands_sent = 2, .commands_completed = 1};
            transport.command_done = false;
            runtime.scan = (iwx_scan_status_t){.state = IWX_SCAN_RUNNING};
            runtime.scan.regulatory.valid = true;
            runtime.link = (iwx_link_t){0};
            runtime.link.status.state = kind == 4 ? IWX_LINK_AUTH : IWX_LINK_SESSION;
            unsigned end = mock_packet(buf, codes[kind], kind == 3 ? IWX_MAC_GROUP : 0,
                                       sequence, sequence >> 8, sizes[kind]);
            if (kind == 2) buf[14] = 1;
            if (kind == 3) { put_u32(buf + 12, 1); put_u32(buf + 16, 1); }
            put_u32(buf + end, IWX_RX_INVALID);
            CHECK(parse_rx(buf, &r) == event);
            CHECK(!transport.command_done && n.commands_completed == 1 &&
                  transport.response_size == 7 && transport.response[0] == 0xa5);
            if (kind == 0) {
                CHECK(n.init_complete == event && n.init_notifications == 1);
                CHECK(n.init_sequence == sequence && n.init_payload_bytes == sizes[kind]);
            } else if (event) {
                if (kind == 1) CHECK(!runtime.scan.regulatory.valid);
                if (kind == 2) CHECK(runtime.scan.state == IWX_SCAN_COMPLETE);
                if (kind == 3) CHECK(runtime.link.session_started);
                if (kind == 4) CHECK(runtime.scan.malformed == 1 && runtime.link.status.rx_malformed == 1);
            }
        }
    }
    runtime.active = false;
    for (unsigned bad = 0; bad < 6; bad++) {
        r = (iwx_probe_report_t){0};
        n = (iwx_nvm_report_t){.commands_sent = bad < 2 ? bad : 2, .init_complete = bad == 2};
        transport.init_waiting = bad != 3;
        unsigned end = mock_packet(buf, IWX_INIT_COMPLETE, 0, 0xff, 0xff, 4);
        if (bad == 4) put_u32(buf, 0x3fff); /* Extent beyond DMA buffer. */
        if (bad == 5) put_u32(buf, 0x10008); /* Actual unconfigured RX queue. */
        put_u32(buf + end, IWX_RX_INVALID);
        CHECK(!parse_rx(buf, &r) && r.error);
        CHECK(!transport.command_done && !n.commands_completed);
    }
    static const unsigned lengths[] = {0, 1, 4, 16, 4088};
    transport.init_waiting = true;
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        r = (iwx_probe_report_t){0};
        n = (iwx_nvm_report_t){.commands_sent = 2};
        unsigned end = mock_packet(buf, IWX_INIT_COMPLETE, 0, 0xff, 0xff, lengths[i]);
        if (end < sizeof(buf)) put_u32(buf + end, IWX_RX_INVALID);
        CHECK(parse_rx(buf, &r) && n.init_complete && n.init_payload_bytes == lengths[i]);
        CHECK(!transport.command_done && !n.commands_completed);
    }
    memset(&transport, 0, sizeof(transport));
    memset(&runtime, 0, sizeof(runtime));
}

static void test_command_bounds(void)
{
    static uint8_t bytes[4096];
    for (unsigned size = 0; size <= 4089; size++) {
        if (size > 22 && size < 4087) continue;
        nvm_fixture(4);
        hw.command_large = true;
        iwx_fw_plan_t p;
        CHECK(iwx_fw_plan(fw, fwsize, &p) == NULL);
        iwx_probe_report_t r = {.alive = true, .rfh_ready = true, .bar0 = hw.bar};
        iwx_nvm_report_t n = {0};
        transport.nvm = &n;
        CHECK(alloc_transport(fw, &p, &r));
        transport.exposed = hw.started = hw.path_ready = true;
        transport.bar0 = hw.bar; transport.device = hw.dev;
        hw.pci[0x44 / 4] = 0; hw.pci[1] |= 4; probe_lock = 1;
        hw.csr[IWX_GP_CNTRL / 4] = IWX_RF_ENABLED | IWX_CLOCK_READY | IWX_INIT_DONE;
        CHECK(send_command(&r, 2, 3, bytes, size) == (size <= 4088));
        if (size > 4088) CHECK(!hw.command_calls);
        if (!size) {
            unsigned calls = hw.command_calls;
            CHECK(!send_command(&r, 2, 3, NULL, 4));
            CHECK(!send_command(&r, 0, 3, bytes, 4));
            transport.command_index = 256;
            CHECK(!send_command(&r, 2, 3, bytes, 4));
            transport.command_index = 0; transport.command_pending = true;
            CHECK(!send_command(&r, 2, 3, bytes, 4) && calls == hw.command_calls);
        }
        finish_transport(&hw.dev, &r, true, NULL);
        probe_lock = 0;
        CHECK(r.stopped && !hw.live_pages && !hw.violations);
    }
}

static void queue_notifications(unsigned count)
{
    uint32_t *used = dma_ptr(transport.used_ring);
    for (unsigned i = 0; i < count; i++) {
        unsigned slot = mock_rx_slot();
        hw.rx_owned[slot] = false;
        uint8_t *p = dma_ptr(transport.rx[slot]);
        unsigned off = mock_packet(p, 0x77, 2, 0, 0x80, 0);
        put_u32(p + off, IWX_RX_INVALID);
        used[hw.rx_producer] = slot + 1;
        hw.rx_producer = (hw.rx_producer + 1) & 511;
    }
    *(uint16_t *)dma_ptr(transport.status) = hw.rx_producer;
    hw.csr[IWX_INT / 4] |= 1U << 31;
}

static void check_rx_accounting(void)
{
    unsigned owned = 0;
    for (unsigned i = 0; i < 512; i++) {
        CHECK(hw.rx_owned[i] == transport.owned[i]);
        if (transport.owned[i]) owned++;
    }
    CHECK(owned == transport.outstanding && owned <= 504 && owned >= 497);
    CHECK(transport.read == transport.completed % 512);
    CHECK(transport.reposted == (transport.completed & ~7ULL));
    CHECK(transport.free_write == (504 + transport.completed) % 512);
    CHECK(transport.published == (504 + transport.reposted) % 512);
}

static void test_runtime_lifecycle(void)
{
    iwx_probe_report_t r;
    iwx_nvm_report_t n;
    iwx_status_t s;
    for (unsigned version = 3; version <= 4; version++) {
        nvm_fixture(version);
        CHECK(iwx_start(fw, fwsize, &r, &n, NULL, NULL) == 0);
        CHECK(r.alive && n.valid && !r.stopped && !r.error && runtime.active);
        CHECK((r.command_after & 0x406) == 0x406);
        CHECK(hw.spawn_calls == 1 && hw.worker == rx_worker && hw.live_pages && !hw.frees);
        CHECK(!iwx_retained_bytes() && transport.nvm != &n);
        memset(&r, 0xa5, sizeof(r)); memset(&n, 0xa5, sizeof(n));
        unsigned reads = hw.reads, writes = hw.writes;
        CHECK(iwx_get_status(&s) == 0 && s.state == IWX_ACTIVE && s.worker_started);
        CHECK(s.generation == 1 && s.nvm.valid && s.nvm.response_version == version);
        CHECK(s.dma_bytes == hw.live_pages * 4096 && s.rx_buffers == 4 && !s.rx_reposted);
        CHECK(hw.reads == reads && hw.writes == writes);
        CHECK(iwx_probe(&r) == -1 && r.error && !r.retained_bytes);
        CHECK(iwx_start(fw, fwsize, &r, &n, NULL, NULL) == -1);
        CHECK(iwx_fw_nvm(fw, fwsize, &r, &n, NULL, NULL) == -1);
        CHECK(iwx_fw_boot(fw, fwsize, &r) == -1 && hw.reads == reads && hw.writes == writes);
        CHECK(iwx_poll() == 0 && runtime.status.nvm.valid);
        check_rx_accounting();
        CHECK(iwx_stop(&r) == 0 && r.stopped && !hw.live_pages && !runtime.active);
        CHECK(iwx_get_status(&s) == 0 && s.state == IWX_OFF && !s.dma_bytes);
        reads = hw.reads; writes = hw.writes;
        CHECK(iwx_poll() == 0 && iwx_stop(&r) == 0);
        CHECK(hw.reads == reads && hw.writes == writes && !hw.violations);

        /* Stop/start reuses the single worker, not caller-owned reports. */
        hw.next_page = hw.offered = hw.command_calls = hw.injected = hw.rx_producer = 0;
        memset(hw.rx_owned, 0, sizeof(hw.rx_owned));
        CHECK(iwx_start(fw, fwsize, &r, &n, NULL, NULL) == 0);
        CHECK(hw.spawn_calls == 1 && runtime.status.generation == 2);
        CHECK(iwx_handoff_begin() == 0 && probe_lock && !hw.live_pages && !runtime.active);
        reads = hw.reads; writes = hw.writes;
        CHECK(iwx_poll() == 0 && iwx_get_status(&s) == -1);
        CHECK(iwx_start(fw, fwsize, &r, &n, NULL, NULL) == -1);
        CHECK(iwx_stop(&r) == -1 && reads == hw.reads && writes == hw.writes);
        iwx_handoff_end();
        CHECK(iwx_get_status(&s) == 0 && s.state == IWX_OFF && !s.dma_bytes && !hw.violations);
    }
    nvm_fixture(4); hw.scheduler_off = true;
    CHECK(iwx_start(fw, fwsize, &r, &n, NULL, NULL) == -1);
    CHECK(r.error && !hw.writes && !hw.allocations && !hw.spawn_calls);
    nvm_fixture(4); hw.spawn_fail = true;
    CHECK(iwx_start(fw, fwsize, &r, &n, NULL, NULL) == -1);
    CHECK(r.error && r.stopped && !hw.live_pages && hw.spawn_calls == 1);
    CHECK(iwx_get_status(&s) == 0 && s.state == IWX_FAILED && !s.worker_started && s.error);
    CHECK(iwx_start(fw, fwsize, NULL, &n, NULL, NULL) == -1 && !n.valid);
    CHECK(iwx_start(fw, fwsize, &r, NULL, NULL, NULL) == -1 && r.error);
    CHECK(iwx_get_status(NULL) == -1);
    const unsigned fails[] = {1, 6, 7, 519, 520};
    for (unsigned i = 0; i < sizeof(fails) / sizeof(fails[0]); i++) {
        nvm_fixture(4); hw.fail_at = fails[i];
        CHECK(iwx_start(fw, fwsize, &r, &n, NULL, NULL) == -1);
        CHECK(!hw.live_pages && !hw.spawn_calls && !hw.violations);
    }
    nvm_fixture(4);
    observer_test_t o = {.reenter = true};
    CHECK(iwx_start(fw, fwsize, &r, &n, observe_boot, &o) == 0);
    CHECK(!r.progress_deferred && !hw.violations && o.count);
    unsigned count = o.count;
    CHECK(iwx_stop(&r) == 0);
    for (unsigned fail = 1; fail <= count; fail++) {
        nvm_fixture(4); o = (observer_test_t){.fail_at = fail};
        CHECK(iwx_start(fw, fwsize, &r, &n, observe_boot, &o) == -1);
        CHECK(o.count == fail && r.progress_failed_at && !hw.live_pages && !hw.spawn_calls);
        CHECK(!hw.violations && !runtime.active);
    }
}

static void test_runtime_rx(void)
{
    iwx_probe_report_t r;
    iwx_nvm_report_t n;
    iwx_status_t s;
    nvm_fixture(4);
    CHECK(iwx_start(fw, fwsize, &r, &n, NULL, NULL) == 0);
    unsigned pages = hw.live_pages, allocations = hw.allocations;
    queue_notifications(70);
    CHECK(iwx_poll() == 64 && transport.completed == 68);
    CHECK(iwx_poll() == 6 && transport.completed == 74);
    CHECK(iwx_poll() == 0);
    check_rx_accounting();
    /* Several wraps of both independent rings, partial batches and a full
     * outstanding set. No allocations/frees on the runtime receive path. */
    for (unsigned i = 0; i < 20; i++) {
        unsigned count = i % 3 == 0 ? transport.outstanding : i % 3 == 1 ? 1 : 300;
        queue_notifications(count);
        unsigned total = 0;
        int processed;
        while ((processed = iwx_poll()) > 0) { CHECK(processed <= 64); total += processed; }
        CHECK(processed == 0 && total == count);
        check_rx_accounting();
    }
    CHECK(transport.completed > 512 * 8 && !hw.violations);
    CHECK(hw.live_pages == pages && hw.allocations == allocations && !hw.frees);
    CHECK(iwx_get_status(&s) == 0 && s.rx_buffers == transport.completed && s.rx_packets > 4096);
    CHECK(s.rx_reposted == transport.reposted && s.rx_pending < 8 && s.state == IWX_ACTIVE);
    CHECK(iwx_stop(&r) == 0 && !hw.live_pages && !hw.violations);
}

static void test_runtime_errors(void)
{
    iwx_probe_report_t r;
    iwx_nvm_report_t n;
    iwx_status_t s;
    for (unsigned fault = 1; fault <= 20; fault++) {
        nvm_fixture(4);
        CHECK(iwx_start(fw, fwsize, &r, &n, NULL, NULL) == 0);
        unsigned old = hw.rx_producer;
        queue_notifications(fault == 8 ? 70 : fault == 7 ? 2 : 1);
        uint32_t *used = dma_ptr(transport.used_ring);
        uint8_t *p = dma_ptr(transport.rx[used[old] - 1]);
        switch (fault) {
        case 1: *(uint16_t *)dma_ptr(transport.status) = 512; break;
        case 2: *(uint16_t *)dma_ptr(transport.status) = (transport.read + 501) & 511; break;
        case 3: used[old] = 0; break;
        case 4: used[old] = 505; break;
        case 5: used[old] = 4095; break;
        case 6: used[old] = 3; break; /* ALIVE buffer not yet reposted. */
        case 7: used[(old + 1) & 511] = used[old]; break;
        case 8:
            used[(old + 64) & 511] = used[old];
            CHECK(iwx_poll() == 64); /* Duplicate falls in an older backlog, after reoffer. */
            break;
        case 9: put_u32(p, 0x3fff); break;
        case 10: put_u32(p, 0x10004); break;
        case 11: p[4] = 2; p[5] = 0; break;
        case 12: p[7] = 0; break;
        case 13: p[4] = 1; p[5] = 0; break;
        case 14: hw.removed = true; break;
        case 15: hw.csr[IWX_INT / 4] |= IWX_INT_SW_ERR; break;
        case 16: hw.csr[IWX_INT / 4] |= IWX_INT_HW_ERR; break;
        case 17: hw.csr[IWX_FH_INT / 4] |= IWX_FH_ERROR; break;
        case 18: hw.rfkill = true; hw.csr[IWX_GP_CNTRL / 4] &= ~IWX_RF_ENABLED; break;
        case 19: hw.csr[IWX_INT / 4] |= IWX_INT_CT_KILL; break;
        case 20: p[4] = 4; p[5] = 0; break;
        }
        CHECK(iwx_poll() == -1 && !runtime.active);
        CHECK(iwx_get_status(&s) == 0 && s.error);
        unsigned reads = hw.reads, writes = hw.writes;
        CHECK(iwx_poll() == 0 && reads == hw.reads && writes == hw.writes);
        if (fault == 14) {
            CHECK(s.state == IWX_QUARANTINED && s.dma_bytes && iwx_retained_bytes() && !hw.frees);
            hw.removed = false;
            CHECK(iwx_stop(&r) == 0);
        } else CHECK(s.state == IWX_FAILED && !s.dma_bytes && !iwx_retained_bytes());
        CHECK(!hw.live_pages && !hw.violations);
    }
    /* Active stop failure must publish quarantine, including to the kexec
     * guard. A second stop is allowed, but no more RX/commands may run. */
    for (unsigned handoff = 0; handoff < 2; handoff++) {
        nvm_fixture(4);
        CHECK(iwx_start(fw, fwsize, &r, &n, NULL, NULL) == 0);
        hw.stop_fail = true;
        CHECK((handoff ? iwx_handoff_begin() : iwx_stop(&r)) == -1);
        CHECK(!probe_lock && !runtime.active && hw.live_pages && !hw.frees);
        CHECK(iwx_get_status(&s) == 0 && s.state == IWX_QUARANTINED && s.stop_error);
        CHECK(iwx_retained_bytes() == s.dma_bytes && !transport.nvm);
        unsigned reads = hw.reads, writes = hw.writes;
        CHECK(iwx_poll() == 0 && iwx_start(fw, fwsize, &r, &n, NULL, NULL) == -1);
        CHECK(reads == hw.reads && writes == hw.writes);
        hw.stop_fail = false;
        CHECK(iwx_stop(&r) == 0 && !hw.live_pages && !iwx_retained_bytes() && !hw.violations);
    }
}

static void test_runtime_commands(void)
{
    iwx_probe_report_t r;
    iwx_nvm_report_t n;
    iwx_status_t s;
    nvm_fixture(4);
    CHECK(iwx_start(fw, fwsize, &r, &n, NULL, NULL) == 0);
    hw.command_large = true;
    uint32_t payload = 0;
    probe_lock = 1;
    for (unsigned i = 0; i < 600; i++) {
        CHECK(send_command(&runtime.report, 12, 2, &payload, sizeof(payload)));
        CHECK(!transport.command_pending && runtime.status.nvm.commands_completed == 4 + i);
    }
    CHECK(transport.command_index == (603 & 255) && hw.command_calls == 603 && !hw.violations);
    hw.command_fault = 1; hw.fault_command = 604;
    CHECK(!send_command(&runtime.report, 12, 2, &payload, sizeof(payload)));
    CHECK(runtime.report.error && transport.command_pending && hw.command_calls == 604);
    CHECK(!send_command(&runtime.report, 12, 2, &payload, sizeof(payload)) && hw.command_calls == 604);
    probe_lock = 0;
    CHECK(iwx_poll() == -1 && !runtime.active && !hw.live_pages && !hw.violations);
    CHECK(iwx_get_status(&s) == 0 && s.state == IWX_FAILED && s.error);
    CHECK(!strcmp(s.error, "firmware command response timed out"));
}

static void scan_fixture(void)
{
    nvm_fixture(4);
    static const uint32_t versions[] = {
        0x00010198, 0x0006019b, 0x00020201, 0x00010500, 0x00010404,
        0x00060177, 0x050101c8, 0x0005010c, 0x000f010d, 0x0001010e,
        0x000401d2, 0x046300c1, 0x000301ee,
        0x00040108, 0x00020508, 0x00050128, 0x0102012b, 0x020c0118,
        0x0709011c, 0x02030517, 0x00010305, 0x026303fb, 0x00030117
    };
    for (unsigned p = 88; p < fwsize; p += 8 + rx_le32(fw + p + 4)) {
        unsigned type = rx_le32(fw + p);
        if (type == IWX_TLV_CMD_VERSIONS) {
            unsigned end = p + 8 + rx_le32(fw + p + 4);
            memmove(fw + end + sizeof(versions), fw + end, fwsize - end);
            memcpy(fw + end, versions, sizeof(versions));
            put_u32(fw + p + 4, 12 + sizeof(versions));
            fwsize += sizeof(versions);
        } else if (type == IWX_TLV_API_CHANGES) put_u32(fw + p + 12, 0x05010000);
    }
    uint32_t api[] = {0, 1U << 9}, cap0[] = {0, 6}, cap2[] = {2, (1U << 10) | (1U << 25)};
    uint32_t channels = 67;
    fw_tlv(IWX_TLV_API_CHANGES, sizeof(api), api);
    fw_tlv(IWX_TLV_CAPABILITIES, sizeof(cap0), cap0);
    uint32_t cap1[] = {1, (1U << 16) | (1U << 22)};
    fw_tlv(IWX_TLV_CAPABILITIES, sizeof(cap1), cap1);
    fw_tlv(IWX_TLV_CAPABILITIES, sizeof(cap2), cap2);
    fw_tlv(IWX_TLV_SCAN_CHANNELS, 4, &channels);
    iwx_probe_report_t r;
    iwx_nvm_report_t n;
    CHECK(iwx_start(fw, fwsize, &r, &n, NULL, NULL) == 0);
    CHECK(!iwx_scan_profile_check(&runtime.profile, &n));
    memset(fw, 0xa5, sizeof(fw)); /* Metadata survives the firmware input. */
    hw.command_large = hw.scan_mode = true;
}

static void queue_fw_packet(uint8_t opcode, uint8_t group, uint8_t index, uint8_t queue,
                            const uint8_t *data, unsigned length)
{
    unsigned slot = mock_rx_slot();
    hw.rx_owned[slot] = false;
    uint8_t *p = dma_ptr(transport.rx[slot]);
    unsigned end = mock_packet(p, opcode, group, index, queue, length);
    if (length) memcpy(p + 8, data, length);
    put_u32(p + end, IWX_RX_INVALID);
    uint32_t *used = dma_ptr(transport.used_ring);
    used[hw.rx_producer] = slot + 1;
    hw.rx_producer = (hw.rx_producer + 1) & 511;
    *(uint16_t *)dma_ptr(transport.status) = hw.rx_producer;
    hw.csr[IWX_INT / 4] |= 1U << 31;
}

static void queue_scan_notification(uint8_t opcode, uint8_t group, uint8_t queue,
                                     const uint8_t *data, unsigned length)
{
    queue_fw_packet(opcode, group, 0, queue, data, length);
}

static void scan_done(unsigned status, unsigned uid)
{
    uint8_t complete[16] = {0};
    put_u32(complete, uid); complete[6] = status;
    queue_scan_notification(0x0f, 0, 0x80, complete, sizeof(complete));
}

static void test_scan_lifecycle(void)
{
    iwx_probe_report_t r;
    iwx_scan_status_t s;
    iwx_bss_t b[2];
    reset_mock();
    CHECK(iwx_scan_start() && iwx_scan_abort() && !hw.writes && !hw.reads);
    CHECK(iwx_scan_results(&s, b, 2) == 0 && s.state == IWX_SCAN_IDLE);
    scan_fixture();
    unsigned seen = runtime.profile.seen;
    runtime.profile.seen = 0;
    CHECK(iwx_scan_start() && runtime.active && !hw.radio_calls);
    runtime.profile.seen = seen;
    probe_lock = 1;
    CHECK(iwx_scan_start() && iwx_scan_abort() && iwx_scan_results(&s, b, 2) == -1);
    probe_lock = 0;
    CHECK(iwx_scan_start() == NULL && hw.radio_calls == 9 && runtime.scan.state == IWX_SCAN_RUNNING);
    CHECK(iwx_scan_start() && hw.radio_calls == 9);
    CHECK(iwx_scan_results(&s, b, 2) == 0 && s.generation == 1 && s.regulatory.valid);
    uint8_t mpdu[90] = {0};
    mpdu[0] = 42; mpdu[12] = 3; mpdu[32] = 45; mpdu[34] = 6; mpdu[48] = 0x80;
    memcpy(mpdu + 64, "\x02\x11\x22\x33\x44\x55", 6);
    mpdu[80] = 100; mpdu[82] = 1; mpdu[85] = 4; memcpy(mpdu + 86, "test", 4);
    queue_scan_notification(0xc1, 0, 0x80, mpdu, sizeof(mpdu));
    CHECK(iwx_poll() == 1 && runtime.scan.count == 1);
    CHECK(iwx_scan_results(&s, b, 2) == 1 && b[0].signal_dbm == -45 && b[0].ssid_len == 4);
    queue_scan_notification(0xc1, 0, 0x80, mpdu, 40);
    CHECK(iwx_poll() == 1 && runtime.scan.malformed == 1 && runtime.active);
    scan_done(1, 7);
    CHECK(iwx_poll() == 1 && runtime.scan.state == IWX_SCAN_RUNNING);
    scan_done(1, 0);
    CHECK(iwx_poll() == 1 && runtime.scan.state == IWX_SCAN_COMPLETE);
    scan_done(1, 0);
    CHECK(iwx_poll() == 1 && runtime.scan.state == IWX_SCAN_COMPLETE && runtime.scan.ignored == 2);
    hw.scan_early = true;
    CHECK(!iwx_scan_start() && hw.radio_calls == 11 && runtime.scan.state == IWX_SCAN_COMPLETE);
    CHECK(runtime.scan.generation == 2 && runtime.scan.count == 0);
    hw.scan_early = false;
    CHECK(!iwx_scan_start() && hw.radio_calls == 13);
    hw.abort_complete = true;
    CHECK(!iwx_scan_abort() && runtime.scan.state == IWX_SCAN_ABORTED && runtime.active);
    CHECK(iwx_scan_abort() && !runtime.report.error);
    CHECK(!iwx_scan_start() && runtime.scan.generation == 4);
    CHECK(iwx_stop(&r) == 0 && runtime.scan.state == IWX_SCAN_ABORTED && !hw.violations && !hw.live_pages);
    CHECK(!runtime.scan.radio_ready && !runtime.scan.regulatory.valid);
    CHECK(iwx_scan_results(NULL, NULL, 0) == -1 && iwx_scan_results(&s, NULL, 1) == -1);

    scan_fixture();
    runtime.profile.capa[0] |= 1U << 12; /* Optional, absent on the real CC image. */
    hw.pci[(0x60 + 0x28) / 4] |= 0x400; /* PCIe LTR enabled by firmware. */
    CHECK(!iwx_scan_start() && hw.radio_calls == 11);
    CHECK(iwx_stop(&r) == 0 && !hw.violations);
}

static void test_scan_failures(void)
{
    iwx_probe_report_t r;
    for (unsigned command = 4; command <= 12; command++) {
        scan_fixture();
        hw.fault_command = command; hw.command_fault = 6;
        CHECK(iwx_scan_start() != NULL && !runtime.active && !hw.live_pages && !hw.violations);
        CHECK(runtime.scan.state == IWX_SCAN_FAILED && runtime.status.error);
    }
    for (unsigned fault = 1; fault <= 5; fault++) {
        scan_fixture();
        hw.scan_fault = fault;
        if (fault <= 2) CHECK(iwx_scan_start() && !runtime.active && !hw.live_pages);
        else {
            CHECK(!iwx_scan_start());
            if (fault == 3) {
                CHECK(!iwx_scan_abort() && runtime.scan.state == IWX_SCAN_ABORTED && runtime.active);
                CHECK(!iwx_stop(&r));
            } else CHECK(iwx_scan_abort() && !runtime.active && !hw.live_pages);
        }
        CHECK(!hw.violations);
    }
    for (unsigned changes = 1; changes <= 2; changes++) {
        scan_fixture(); hw.mcc_changes = changes;
        if (changes == 1) {
            CHECK(!iwx_scan_start() && runtime.regulatory_revision == 1 && runtime.scan.regulatory.valid);
            CHECK(hw.last_mcc_query == 0x054152 && runtime.scan.regulatory.mcc == 0x4152 && !runtime.mcc_pending);
            CHECK(!iwx_stop(&r));
        } else CHECK(iwx_scan_start() && !runtime.active && !hw.live_pages);
    }
    scan_fixture(); CHECK(!iwx_scan_start());
    uint8_t change[4] = {0x52, 0x41, 5, 0};
    hw.abort_complete = true;
    queue_scan_notification(0xc9, 0, 0x80, change, sizeof(change));
    CHECK(iwx_poll() > 0 && runtime.scan.state == IWX_SCAN_ABORTED && !runtime.scan.regulatory.valid);
    CHECK(runtime.scan.error && !iwx_scan_start() && runtime.scan.regulatory.valid);
    CHECK(hw.last_mcc_query == 0x054152 && runtime.scan.regulatory.mcc == 0x4152 && !runtime.mcc_pending);
    CHECK(!iwx_stop(&r));
    scan_fixture(); CHECK(!iwx_scan_start());
    hw.now += 30000000001ULL;
    CHECK(iwx_poll() >= 0 && runtime.scan.state == IWX_SCAN_ABORTING);
    CHECK(!iwx_scan_abort()); /* Does not restart the abort deadline. */
    hw.stop_fail = true;
    hw.now += 2000000001ULL;
    CHECK(iwx_poll() == -1 && runtime.status.state == IWX_QUARANTINED && hw.live_pages);
    CHECK(runtime.scan.state == IWX_SCAN_FAILED && iwx_retained_bytes());
    CHECK(iwx_scan_start() && iwx_handoff_begin() == -1);
    hw.stop_fail = false;
    CHECK(!iwx_stop(&r) && !hw.live_pages && !hw.violations);

    for (unsigned bad = 0; bad < 4; bad++) {
        scan_fixture(); CHECK(!iwx_scan_start());
        uint8_t notification[16] = {0}; notification[6] = bad == 0 ? 99 : 1;
        queue_scan_notification(bad == 3 ? 0xc9 : 0x0f, 0, bad == 1 ? 0x01 : 0x80,
                                notification, bad == 2 ? 15 : 16);
        CHECK(iwx_poll() == -1 && !runtime.active && !hw.live_pages && !hw.violations);
    }
}

static void link_fixture(void)
{
    scan_fixture();
    hw.link_mode = hw.session_start = true;
    CHECK(!iwx_scan_start());
    uint8_t beacon[112] = {64};
    beacon[12] = 3; beacon[34] = 1; beacon[32] = 45;
    beacon[48] = 0x80;
    memset(beacon + 52, 255, 6);
    memcpy(beacon + 58, "\x02\x01\x02\x03\x04\x05", 6);
    memcpy(beacon + 64, beacon + 58, 6);
    beacon[80] = 100; beacon[82] = 0x21; beacon[83] = 4;
    const uint8_t ies[] = {0,4,'t','e','s','t',1,8,0x82,0x84,0x8b,0x96,12,18,24,36,
        50,4,48,72,96,108,5,4,0,2,0,0};
    memcpy(beacon + 84, ies, sizeof(ies));
    queue_scan_notification(0xc1, 0, 0x80, beacon, sizeof(beacon));
    scan_done(1, 0);
    CHECK(iwx_poll() > 0 && runtime.scan.count == 1 && runtime.scan.state == IWX_SCAN_COMPLETE);
    CHECK(runtime.bss[0].rates_len == 12 && runtime.bss[0].dtim_period == 2);
}

static void test_link_lifecycle(void)
{
    iwx_probe_report_t r;
    iwx_link_status_t s;
    static const uint8_t bssid[] = {2,1,2,3,4,5};
    reset_mock();
    CHECK(iwx_associate(bssid, false) && !hw.writes && !hw.reads);
    CHECK(!iwx_link_status(&s) && s.state == IWX_LINK_IDLE && !s.port_open);
    CHECK(iwx_link_status(NULL) == -1);
    for (unsigned reverse = 0; reverse < 2; reverse++) {
        link_fixture();
        probe_lock = 1;
        CHECK(iwx_associate(bssid, false) && iwx_link_status(&s) == -1);
        probe_lock = 0;
        unsigned commands = hw.command_calls, allocs = hw.allocations;
        uint8_t unknown[6] = {2,1,2,3,4,6};
        CHECK(iwx_associate(unknown, false) && hw.command_calls == commands);
        CHECK(iwx_associate(NULL, false) && hw.allocations == allocs);
        hw.tx_reverse = reverse;
        const char *err = iwx_associate(bssid, false);
        if (err) printf("link start: %s\n", err);
        CHECK(!err && runtime.active && transport.tx_pending);
        CHECK(runtime.link.status.state == IWX_LINK_AUTH && hw.tx_calls == 1);
        CHECK(iwx_associate(bssid, false) && iwx_scan_start());
        CHECK(iwx_poll() > 0 && runtime.link.status.state == IWX_LINK_ASSOC && hw.tx_calls == 2);
        CHECK(iwx_poll() > 0 && runtime.link.status.state == IWX_LINK_ASSOCIATED);
        CHECK(!iwx_link_status(&s) && s.aid == 35 && s.tx_sent == 2 && s.tx_completed == 2 && !s.port_open);
        CHECK(!transport.tx_pending && transport.tx_queue == 7 && transport.tx_index == 2);
        CHECK(hw.allocations == allocs + 3 && runtime.active && !hw.violations);
        /* TX header index and hardware cursor wrap independently of RX and
         * command indices. An AP response alone does not recycle TX memory. */
        for (unsigned i = 0; i < 140; i++) {
            runtime.link.status.state = IWX_LINK_AUTH;
            runtime.link.request_sent = false;
            runtime.link.deadline = hw.now + 1000000000ULL;
            CHECK(iwx_poll() >= 0 && transport.tx_pending);
            CHECK(iwx_poll() > 0 && iwx_poll() > 0);
            CHECK(runtime.link.status.state == IWX_LINK_ASSOCIATED && !transport.tx_pending);
            if (!runtime.active) break;
        }
        CHECK(transport.tx_index == (282 & 255) && runtime.link.status.tx_completed == 282);
        uint8_t ended[16] = {0}; ended[4] = 1;
        if (runtime.active) {
            queue_scan_notification(0xfb, 3, 0x80, ended, sizeof(ended));
            CHECK(iwx_poll() > 0 && runtime.active && runtime.link.status.state == IWX_LINK_ASSOCIATED);
        }
        CHECK(!iwx_stop(&r) && !hw.live_pages && !hw.violations && runtime.link.status.state == IWX_LINK_STOPPED);
    }
}

static void test_link_failures(void)
{
    static const uint8_t bssid[] = {2,1,2,3,4,5};
    iwx_probe_report_t r;
    for (unsigned command = 1; command <= 8; command++) {
        link_fixture();
        hw.fault_command = hw.command_calls + command;
        hw.command_fault = 6;
        CHECK(iwx_associate(bssid, false) && !runtime.active && !hw.live_pages && !hw.violations);
    }
    for (unsigned fault = 1; fault <= 10; fault++) {
        link_fixture(); hw.link_fault = fault;
        CHECK(iwx_associate(bssid, false));
        if (fault >= 3 && fault <= 5) {
            iwx_link_status_t status = {0};
            CHECK(!iwx_link_status(&status) && status.queue_response);
            CHECK(status.queue_bytes == (fault == 3 ? 7U : 8U));
            CHECK(status.queue_id == (fault == 3 || fault == 4 ? 0 : fault == 5 ? 512 : 7));
            CHECK(status.queue_flags == (fault == 3 ? 0 : hw.tx_queue_flags));
            CHECK(!status.tx_sent && !hw.tx_calls && !status.port_open);
        }
        if (fault == 10) {
            CHECK(runtime.active && !hw.tx_calls && runtime.link.status.state == IWX_LINK_IDLE);
            CHECK(!iwx_stop(&r));
        } else CHECK(!runtime.active && !hw.live_pages);
        CHECK(!hw.violations);
    }
    for (unsigned alloc = 1; alloc <= 3; alloc++) {
        link_fixture(); hw.fail_at = hw.allocations + alloc;
        CHECK(iwx_associate(bssid, false) && !runtime.active && !hw.live_pages && !hw.violations);
    }
    for (unsigned fault = 1; fault <= 15; fault++) {
        if (fault == 12) continue;
        link_fixture(); hw.tx_fault = fault;
        CHECK(!iwx_associate(bssid, false));
        iwx_poll();
        if (runtime.active) { hw.now += 2000000000ULL; iwx_poll(); }
        CHECK(!runtime.active && !hw.live_pages && !hw.violations);
        CHECK(runtime.link.status.state == IWX_LINK_FAILED && !runtime.link.status.port_open);
    }
    link_fixture(); hw.session_start = false;
    CHECK(!iwx_associate(bssid, false) && !hw.tx_calls && runtime.link.status.state == IWX_LINK_SESSION);
    hw.now += 2000000000ULL;
    CHECK(iwx_poll() == -1 && !hw.live_pages && !hw.violations);

    link_fixture(); hw.tx_fault = 1;
    CHECK(!iwx_associate(bssid, false));
    CHECK(iwx_poll() > 0 && transport.tx_pending && runtime.link.status.state == IWX_LINK_ASSOC);
    unsigned pinned = hw.live_pages;
    hw.stop_fail = true; hw.now += 2000000000ULL;
    CHECK(iwx_poll() == -1 && iwx_retained_bytes() && hw.live_pages == pinned);
    CHECK(transport.tx_page && transport.tx_descriptors && iwx_handoff_begin() == -1);
    CHECK(iwx_associate(bssid, false));
    hw.stop_fail = false;
    CHECK(!iwx_stop(&r) && !hw.live_pages && !iwx_retained_bytes() && !hw.violations);

    link_fixture();
    CHECK(!iwx_associate(bssid, false));
    CHECK(!iwx_handoff_begin() && !hw.live_pages && !runtime.active);
    CHECK(iwx_associate(bssid, false)); iwx_handoff_end();
    CHECK(!hw.violations);
}

static void test_link_queue_contract(void)
{
    static const unsigned queues[] = {1, 31, 32, 127, 128, 255, 256, 511};
    static const unsigned cursors[] = {1, 254, 255, 256, 0x1234, 0xffff};
    static const uint8_t bssid[] = {2,1,2,3,4,5};
    for (unsigned q = 0; q < sizeof(queues) / sizeof(queues[0]); q++)
        for (unsigned i = 0; i < sizeof(cursors) / sizeof(cursors[0]); i++) {
            link_fixture();
            hw.tx_queue = queues[q]; hw.tx_write_pointer = cursors[i];
            hw.tx_reverse = i & 1;
            const char *err = iwx_associate(bssid, false);
            CHECK(!err && runtime.active && transport.tx_pending);
            if (!err) {
                CHECK(transport.tx_queue == queues[q] && transport.tx_index == (cursors[i] & 255));
                iwx_link_status_t status = {0};
                CHECK(!iwx_link_status(&status) && status.queue_response && status.queue_bytes == 8);
                CHECK(status.queue_id == queues[q] && status.queue_flags == hw.tx_queue_flags && !status.queue_reserved);
                CHECK(status.queue_write_pointer == cursors[i]);
                CHECK(iwx_poll() > 0 && iwx_poll() > 0);
                CHECK(runtime.link.status.state == IWX_LINK_ASSOCIATED);
                CHECK(runtime.link.status.tx_completed == 2 && !transport.tx_pending);
                CHECK(transport.tx_index == ((cursors[i] + 2) & 255));
            }
            iwx_probe_report_t r;
            CHECK(!iwx_stop(&r) && !hw.live_pages && !hw.violations);
        }
    link_fixture(); hw.tx_queue = 511; hw.tx_write_pointer = 255; hw.tx_fault = 8;
    CHECK(!iwx_associate(bssid, false));
    CHECK(iwx_poll() == -1 && !runtime.active && !hw.live_pages && !hw.violations);
}

static void test_link_queue_flags(void)
{
    static const unsigned flags[] = {0, 1, 2, 0x100, 0x8000, 0xffff};
    static const uint8_t bssid[] = {2,1,2,3,4,5};
    for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        for (unsigned bad = 0; bad < 3; bad++) {
            link_fixture();
            hw.tx_queue = 1; hw.tx_queue_flags = flags[i];
            hw.link_fault = bad ? bad + 5 : 0;
            const char *err = iwx_associate(bssid, false);
            if (bad) {
                CHECK(err && !runtime.active && !hw.live_pages && !hw.tx_calls);
                CHECK(err && !strcmp(err, bad == 1 ? "firmware rejected command" :
                                     "unexpected firmware command response sequence/ID"));
                CHECK(!runtime.link.status.queue_response && !runtime.link.status.port_open);
            } else {
                /* i == 1 replays 8/1/1/0/0, captured before any authentication TX. */
                CHECK(!err && runtime.active && transport.tx_pending);
                iwx_link_status_t s = {0};
                CHECK(!iwx_link_status(&s) && s.queue_response && s.queue_bytes == 8);
                CHECK(s.queue_id == 1 && s.queue_flags == flags[i] && !s.queue_write_pointer && !s.queue_reserved);
                if (!err) {
                    CHECK(iwx_poll() > 0 && iwx_poll() > 0);
                    CHECK(runtime.link.status.state == IWX_LINK_ASSOCIATED);
                    CHECK(runtime.link.status.tx_completed == 2 && !runtime.link.status.port_open);
                }
            }
            iwx_probe_report_t r;
            CHECK(!iwx_stop(&r) && !hw.live_pages && !hw.violations);
        }
    }
}

static bool security_tkip;
static void eapol_put16(uint8_t *p, unsigned n) { p[0] = n >> 8; p[1] = n; }
static void eapol_base(uint8_t *p, unsigned info, unsigned replay)
{
    memset(p, 0, WPA2_EAPOL_MAX);
    p[0] = 2; p[1] = 3; p[4] = 2;
    eapol_put16(p + 2, 95); eapol_put16(p + 5, info); eapol_put16(p + 7, 16);
    p[16] = replay;
    if (info & 8) for (unsigned i = 0; i < 32; i++) p[17 + i] = i + 8;
}

static unsigned eapol_message(uint8_t *p, bool pairwise, unsigned replay, unsigned id, unsigned seed)
{
    eapol_base(p, pairwise ? 0x13ca : 0x1382, replay);
    uint8_t plain[80] = {0}; unsigned n = 0;
    if (pairwise) {
        plain[0] = 48; plain[1] = runtime.wpa.ap_rsn_len;
        memcpy(plain + 2, runtime.wpa.ap_rsn, runtime.wpa.ap_rsn_len); n = 2 + runtime.wpa.ap_rsn_len;
    } else eapol_put16(p + 7, runtime.wpa.group_len);
    uint8_t kde[8] = {221,6 + runtime.wpa.group_len,0,15,172,1,id,0};
    memcpy(plain + n, kde, sizeof(kde)); n += sizeof(kde);
    for (unsigned i = 0; i < runtime.wpa.group_len; i++) plain[n++] = i + seed;
    if (n & 7) { plain[n++] = 221; while (n & 7) plain[n++] = 0; }
    unsigned size = iwx_test_wrap(runtime.wpa.ptk + 16, plain, n, p + 99);
    eapol_put16(p + 97, size); eapol_put16(p + 2, 95 + size); p[65] = 47;
    uint8_t mac[20]; iwx_test_hmac(runtime.wpa.ptk, p, 99 + size, mac);
    memcpy(p + 81, mac, 16);
    return 99 + size;
}

static void queue_eapol(const uint8_t *p, unsigned n, bool protect, unsigned pn)
{
    uint8_t mpdu[48 + 24 + 8 + 8 + WPA2_EAPOL_MAX] = {0};
    unsigned body = protect ? 80 : 72;
    mpdu[0] = body + 8 + n - 48; mpdu[1] = (body + 8 + n - 48) >> 8;
    put_u32(mpdu + 12, protect ? 0xa43 : 3); mpdu[34] = 1;
    mpdu[48] = 8; mpdu[49] = protect ? 0x42 : 2;
    memcpy(mpdu + 52, runtime.link.local, 6); memcpy(mpdu + 58, runtime.link.ap.bssid, 6);
    memcpy(mpdu + 64, runtime.link.ap.bssid, 6);
    if (protect) { mpdu[72] = pn; mpdu[73] = pn >> 8; mpdu[75] = 0x20; }
    memcpy(mpdu + body, "\xaa\xaa\x03\x00\x00\x00\x88\x8e", 8);
    memcpy(mpdu + body + 8, p, n);
    queue_fw_packet(0xc1, 0, 0, 0x80, mpdu, body + 8 + n);
}

static void eapol_complete(void)
{
    uint8_t completed[48] = {1};
    completed[36] = hw.tx_queue; completed[37] = hw.tx_queue >> 8; completed[40] = 1;
    put_u32(completed + 44, hw.tx_cursor);
    queue_fw_packet(0x1c, 0, transport.tx_index, hw.tx_queue & 31, completed, sizeof(completed));
}

static bool bytes_zero(const void *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++) if (((const uint8_t *)p)[i]) return false;
    return true;
}

static void security_fixture(void)
{
    link_fixture();
    runtime.bss[0].rsn = true; runtime.bss[0].capability |= 16;
    runtime.bss[0].rsn_len = 20;
    memcpy(runtime.bss[0].rsn_ie, wpa2_assoc_rsn + 2, 20);
    if (security_tkip) {
        runtime.bss[0].rsn_ie[5] = 2;
        runtime.profile.api[0] |= 1U << 29;
        runtime.profile.api[1] |= 1U << 9;
    }
}

static void security_start(bool early)
{
    security_fixture();
    uint8_t pmk[32]; memset(pmk, 0x24, sizeof(pmk));
    CHECK(!iwx_connect_psk(runtime.bss[0].bssid, pmk));
    CHECK(runtime.wpa.state == WPA2_WAIT_M1 && !runtime.link.status.port_open);
    CHECK(iwx_poll() > 0 && runtime.link.status.state == IWX_LINK_ASSOC);
    if (early) {
        uint8_t m1[WPA2_EAPOL_MAX]; eapol_base(m1, 0x8a, 1); queue_eapol(m1, 99, false, 0);
    }
    CHECK(iwx_poll() > 0 && runtime.link.status.state == IWX_LINK_ASSOCIATED);
    CHECK(runtime.eapol_count == (early ? 1U : 0U));
}

static void security_m1(bool early, bool hold)
{
    security_start(early);
    uint8_t m1[WPA2_EAPOL_MAX]; eapol_base(m1, 0x8a, 1);
    if (!early) queue_eapol(m1, 99, false, 0);
    hw.eapol_hold = hold;
    CHECK(iwx_poll() >= 0 && runtime.active && runtime.wpa.state == WPA2_WAIT_M3 && transport.tx_pending);
    CHECK(!runtime.link.status.port_open && runtime.wpa.inflight_tx);
    if (!hold) CHECK(iwx_poll() > 0 && !transport.tx_pending);
}

static void test_security_lifecycle(void)
{
    iwx_probe_report_t r;
    uint8_t frame[WPA2_EAPOL_MAX];
    for (unsigned order = 0; order < 3; order++) {
        security_m1(order == 2, order == 1);
        unsigned n = eapol_message(frame, true, 2, 1, 10);
        queue_eapol(frame, n, false, 0);
        CHECK(iwx_poll() > 0 && runtime.active && !runtime.link.status.port_open);
        if (order == 1) {
            CHECK(!hw.key_calls && runtime.wpa.install_pending);
            hw.eapol_hold = false; eapol_complete();
            CHECK(iwx_poll() > 0 && runtime.active);
        }
        CHECK(hw.key_calls == 2 && runtime.link.status.key_installs == 1 && runtime.wpa.state == WPA2_WAIT_M4_TX);
        CHECK(transport.tx_pending && !runtime.link.status.port_open);
        CHECK(bytes_zero(dma_ptr(transport.command_data), 4096));
        CHECK(data_pn(runtime.wpa.keys.rsc) == 0 && data_pn((uint8_t *)&runtime.data.pn[2]) == 47);
        CHECK(iwx_poll() > 0 && runtime.link.status.port_open && runtime.wpa.state == WPA2_ESTABLISHED);
        /* Duplicate M3 resends M4, never resets RX PN or reinstalls a key. */
        runtime.data.pn[2] = 80;
        queue_eapol(frame, n, false, 0);
        CHECK(iwx_poll() > 0 && hw.key_calls == 2 && runtime.data.pn[2] == 80);
        CHECK(iwx_poll() > 0 && runtime.link.status.port_open);
        n = eapol_message(frame, false, 3, 2, 30);
        queue_eapol(frame, n, true, 1);
        CHECK(iwx_poll() > 0 && hw.key_calls == 3 && !runtime.link.status.port_open);
        CHECK(iwx_link_up()); /* GTK rotation is not a new IP network. */
        CHECK(runtime.wpa.state == WPA2_WAIT_GROUP_TX && runtime.data.pn[0] == 1 && runtime.data.pn[3] == 47);
        CHECK(((uint8_t *)dma_ptr(transport.tx_page))[25] == 0x41);
        CHECK(iwx_poll() > 0 && runtime.link.status.port_open);
        queue_eapol(frame, n, true, 1); /* ciphertext replay before WPA processing */
        unsigned accepted = runtime.link.status.eapol_received;
        CHECK(iwx_poll() > 0 && runtime.link.status.eapol_received == accepted && hw.key_calls == 3);
        queue_eapol(frame, n, true, 2);
        CHECK(iwx_poll() > 0 && hw.key_calls == 3 && runtime.data.pn[3] == 47);
        CHECK(iwx_poll() > 0 && runtime.link.status.port_open);
        uint64_t command_page = transport.command_data, tx_page = transport.tx_page;
        CHECK(!iwx_stop(&r) && !hw.live_pages && !hw.violations);
        CHECK(bytes_zero(&runtime.wpa, sizeof(runtime.wpa)) && bytes_zero(&runtime.data, sizeof(runtime.data)));
        CHECK(bytes_zero(dma_ptr(command_page), 4096) && bytes_zero(dma_ptr(tx_page), 4096));
    }
}

static void queue_tkip_group(unsigned pn, bool bad_mic)
{
    uint8_t mpdu[96] = {48};
    put_u32(mpdu + 12, bad_mic ? 0xb2f : 0xb6f); mpdu[34] = runtime.link.ap.channel;
    mpdu[48] = 8; mpdu[49] = 0x42;
    memset(mpdu + 52, 255, 6); memcpy(mpdu + 58, runtime.link.ap.bssid, 6);
    memcpy(mpdu + 64, runtime.link.ap.bssid, 6);
    mpdu[72] = pn >> 8; mpdu[73] = ((pn >> 8) | 0x20) & 0x7f; mpdu[74] = pn; mpdu[75] = 0x60;
    memcpy(mpdu + 80, "\xaa\xaa\x03\x00\x00\x00\x08\x06", 8);
    unsigned slot = mock_rx_slot();
    queue_fw_packet(0xc1, 0, 0, 0x80, mpdu, sizeof(mpdu));
    uint8_t *header = dma_ptr(transport.rx[slot]);
    put_u32(header, rx_le32(header) | IWX_RX_RADA);
}

static void test_tkip_lifecycle(void)
{
    security_tkip = true;
    test_security_lifecycle();
    iwx_probe_report_t r; uint8_t pmk[32] = {1};
    for (unsigned missing = 0; missing < 2; missing++) {
        security_fixture();
        runtime.profile.api[missing] &= ~(1U << (missing ? 9 : 29));
        CHECK(iwx_connect_psk(runtime.bss[0].bssid, pmk) && !hw.tx_calls && !hw.key_calls);
        CHECK(!iwx_stop(&r) && !hw.live_pages && !hw.violations);
    }
    security_m1(false, false);
    uint8_t frame[WPA2_EAPOL_MAX];
    unsigned n = eapol_message(frame, true, 2, 1, 10);
    queue_eapol(frame, n, false, 0);
    CHECK(iwx_poll() > 0 && iwx_poll() > 0 && iwx_link_up());
    queue_tkip_group(48, false);
    CHECK(iwx_poll() > 0 && runtime.data.pn[2] == 48 && runtime.link.status.data_rx == 1);
    queue_tkip_group(48, true); /* Replay is rejected before the Michael report. */
    CHECK(iwx_poll() > 0 && !michael.have_first && !runtime.wpa.mic_reports);
    queue_tkip_group(49, true);
    CHECK(iwx_poll() > 0 && michael.have_first && runtime.wpa.inflight_kind == WPA2_TX_MIC_REPORT);
    CHECK(runtime.data.pn[2] == 48 && runtime.link.status.data_rx == 1 && iwx_link_up());
    CHECK(iwx_poll() > 0 && runtime.link.status.port_open);
    queue_tkip_group(50, true);
    CHECK(iwx_poll() > 0 && runtime.wpa.countermeasures && !iwx_link_up());
    CHECK(!runtime.link.status.port_open && michael.blocked_until > hw.now);
    CHECK(iwx_poll() > 0 && runtime.tkip_deauth_sent);
    CHECK(iwx_poll() == -1 && !runtime.active && !hw.live_pages && !hw.violations);
    CHECK(bytes_zero(&runtime.wpa, sizeof(runtime.wpa)) && bytes_zero(&runtime.data, sizeof(runtime.data)));
    uint64_t until = michael.blocked_until;
    CHECK(until && !iwx_stop(&r) && michael.blocked_until == until);
    security_fixture();
    CHECK(iwx_connect_psk(runtime.bss[0].bssid, pmk) && !hw.tx_calls);
    hw.now = until;
    CHECK(!iwx_connect_psk(runtime.bss[0].bssid, pmk) && hw.tx_calls == 1);
    CHECK(!iwx_stop(&r) && !hw.live_pages && !hw.violations);
    memset(&michael, 0, sizeof(michael)); security_tkip = false;
}

static void test_security_failures(void)
{
    iwx_probe_report_t r;
    uint8_t frame[WPA2_EAPOL_MAX], pmk[32] = {1};
    security_fixture();
    CHECK(iwx_connect_psk(runtime.bss[0].bssid, NULL));
    runtime.profile.command[IWX_ABI_STA_KEY] = 2;
    CHECK(iwx_connect_psk(runtime.bss[0].bssid, pmk) && !hw.tx_calls);
    runtime.profile.command[IWX_ABI_STA_KEY] = 3; hw.rng_fail = true;
    CHECK(iwx_connect_psk(runtime.bss[0].bssid, pmk) && !hw.tx_calls && runtime.link.status.state == IWX_LINK_IDLE);
    CHECK(bytes_zero(&runtime.wpa, sizeof(runtime.wpa)) && !iwx_stop(&r));
    for (unsigned fault = 1; fault <= 4; fault++) for (unsigned at = 1; at <= 2; at++) {
        security_m1(false, false);
        hw.key_fault = fault; hw.key_fault_at = at;
        unsigned n = eapol_message(frame, true, 2, 1, 10);
        queue_eapol(frame, n, false, 0);
        CHECK(iwx_poll() == -1 && !runtime.active && !hw.live_pages && !hw.violations);
        CHECK(!runtime.link.status.port_open && bytes_zero(&runtime.wpa, sizeof(runtime.wpa)));
    }
    for (unsigned timeout = 0; timeout < 2; timeout++) {
        security_m1(false, false);
        unsigned n = eapol_message(frame, true, 2, 1, 10);
        hw.eapol_hold = timeout;
        hw.tx_fault = timeout ? 0 : 9;
        queue_eapol(frame, n, false, 0);
        CHECK(iwx_poll() > 0 && hw.key_calls == 2 && !runtime.link.status.port_open);
        if (timeout) hw.now += 2000000000ULL;
        CHECK(iwx_poll() == -1 && !runtime.active && !hw.live_pages && !hw.violations);
        CHECK(bytes_zero(&runtime.wpa, sizeof(runtime.wpa)) && !runtime.link.status.port_open);
    }
    security_m1(false, false);
    unsigned n = eapol_message(frame, true, 2, 1, 10);
    frame[81] ^= 1; queue_eapol(frame, n, false, 0);
    CHECK(iwx_poll() > 0 && !hw.key_calls && runtime.active && runtime.wpa.state == WPA2_WAIT_M3);
    hw.now += WPA2_TIMEOUT_NS;
    CHECK(iwx_poll() == -1 && !hw.live_pages && bytes_zero(&runtime.wpa, sizeof(runtime.wpa)));

    security_m1(false, true);
    CHECK(!iwx_handoff_begin() && !runtime.active && !hw.live_pages);
    CHECK(bytes_zero(&runtime.wpa, sizeof(runtime.wpa))); iwx_handoff_end();

    security_m1(false, false); hw.key_fault = 3; hw.stop_fail = true;
    n = eapol_message(frame, true, 2, 1, 10); queue_eapol(frame, n, false, 0);
    uint64_t page = transport.command_data;
    CHECK(iwx_poll() == -1 && iwx_retained_bytes() && !runtime.active);
    CHECK(bytes_zero(&runtime.wpa, sizeof(runtime.wpa)) && !bytes_zero(dma_ptr(page), 4096));
    hw.stop_fail = false;
    CHECK(!iwx_stop(&r) && !hw.live_pages && !hw.violations && bytes_zero(dma_ptr(page), 4096));

    security_start(false); eapol_base(frame, 0x8a, 1);
    for (unsigned i = 0; i < 12; i++) queue_eapol(frame, 99, false, 0);
    CHECK(iwx_poll() > 0 && runtime.link.status.eapol_dropped == 8 && !runtime.eapol_count && runtime.active);
    CHECK(runtime.link.status.eapol_ingress_dropped == 8 && !runtime.link.status.eapol_last_drop.reason);
    CHECK(bytes_zero(runtime.eapol, sizeof(runtime.eapol)));
    CHECK(!iwx_stop(&r) && !hw.live_pages && !hw.violations);
}

static void test_security_diagnostics(void)
{
    iwx_probe_report_t report;
    iwx_link_status_t status;
    uint8_t frame[WPA2_EAPOL_MAX];
    for (unsigned fault = 0; fault < 2; fault++) {
        security_start(false); eapol_base(frame, 0x8a, 1);
        unsigned n = 99;
        const char *reason = "unsupported EAPOL-Key descriptor";
        if (!fault) frame[4] = 254;
        else {
            reason = "invalid M1 key data"; n = 121;
            memcpy(frame + 99, "\xdd\x13\x00\x0f\xac\x04", 6);
            eapol_put16(frame + 97, 22); eapol_put16(frame + 2, 117);
        }
        for (unsigned attempt = 0; attempt < 6; attempt++) {
            queue_eapol(frame, n, false, 0);
            CHECK(iwx_poll() > 0 && runtime.active && !hw.key_calls && !iwx_link_up());
            CHECK(!iwx_link_status(&status) && !status.port_open && !status.eapol_received);
            CHECK(status.eapol_dropped == attempt + 1 && !status.eapol_ingress_dropped);
            CHECK(status.eapol_last_drop.reason && !strcmp(status.eapol_last_drop.reason, reason));
            CHECK(status.eapol_last_drop.state == WPA2_WAIT_M1 && status.eapol_last_drop.bytes == n);
            CHECK(status.eapol_last_drop.key_info == 0x8a && status.eapol_last_drop.data_bytes == n - 99);
        }
        CHECK(status.tx_sent == 2 && status.tx_completed == 2);
        wpa2_rx_diagnostic_t saved = status.eapol_last_drop;
        uint8_t deauth[74] = {26};
        deauth[12] = 3; deauth[34] = runtime.link.ap.channel; deauth[48] = 0xc0;
        memcpy(deauth + 52, runtime.link.local, 6);
        memcpy(deauth + 58, runtime.link.ap.bssid, 6); memcpy(deauth + 64, deauth + 58, 6);
        deauth[72] = 15;
        queue_fw_packet(0xc1, 0, 0, 0x80, deauth, sizeof(deauth));
        CHECK(iwx_poll() == -1 && !runtime.active && !hw.live_pages && !hw.violations);
        CHECK(bytes_zero(&runtime.wpa, sizeof(runtime.wpa)) && bytes_zero(runtime.eapol, sizeof(runtime.eapol)));
        CHECK(!iwx_link_status(&status) && status.state == IWX_LINK_FAILED && status.reason == 15);
        CHECK(!memcmp(&status.eapol_last_drop, &saved, sizeof(saved)));
        CHECK(!iwx_stop(&report) && !iwx_link_status(&status) && status.state == IWX_LINK_STOPPED);
        CHECK(!memcmp(&status.eapol_last_drop, &saved, sizeof(saved)));
    }
    security_start(false);
    CHECK(!iwx_link_status(&status) && !status.eapol_dropped && !status.eapol_ingress_dropped);
    CHECK(bytes_zero(&status.eapol_last_drop, sizeof(status.eapol_last_drop)));
    CHECK(!iwx_stop(&report) && !hw.live_pages && !hw.violations);
}

static void test_security_pmkid(void)
{
    iwx_probe_report_t report;
    iwx_link_status_t status = {0};
    uint8_t frame[WPA2_EAPOL_MAX], name[20], digest[20];
    for (unsigned complete = 0; complete < 2; complete++) {
        security_start(false); eapol_base(frame, 0x8a, 1); frame[0] = 1;
        memcpy(name, "PMK Name", 8); memcpy(name + 8, runtime.link.ap.bssid, 6);
        memcpy(name + 14, runtime.link.local, 6);
        wpa2_hmac_sha1(runtime.wpa.pmk, 32, name, sizeof(name), digest);
        memcpy(frame + 99, "\xdd\x14\x00\x0f\xac\x04", 6);
        memcpy(frame + 105, digest, 16); frame[105] ^= 1;
        eapol_put16(frame + 97, 22); eapol_put16(frame + 2, 117);
        for (unsigned attempt = 0; attempt < 2; attempt++) {
            queue_eapol(frame, 121, false, 0);
            CHECK(iwx_poll() > 0 && runtime.wpa.state == WPA2_WAIT_M3 && transport.tx_pending);
            CHECK(!iwx_link_status(&status) && !status.port_open && !status.key_installs);
            CHECK(status.pmkid_mismatches == attempt + 1 && status.eapol_received == attempt + 1);
            CHECK(!status.eapol_dropped && !status.eapol_last_drop.reason && !hw.key_calls);
            CHECK(status.tx_sent == attempt + 3);
            CHECK(iwx_poll() > 0 && !transport.tx_pending && !iwx_link_up());
        }
        if (complete) {
            unsigned n = eapol_message(frame, true, 2, 1, 10);
            queue_eapol(frame, n, false, 0);
            CHECK(iwx_poll() > 0 && hw.key_calls == 2 && !runtime.link.status.port_open);
            CHECK(iwx_poll() > 0 && runtime.wpa.state == WPA2_ESTABLISHED && iwx_link_up());
        } else {
            hw.now += WPA2_TIMEOUT_NS;
            CHECK(iwx_poll() == -1 && !runtime.active && !hw.key_calls && !iwx_link_up());
        }
        CHECK(!iwx_stop(&report) && !hw.live_pages && !hw.violations);
        CHECK(bytes_zero(&runtime.wpa, sizeof(runtime.wpa)));
        CHECK(!iwx_link_status(&status) && status.pmkid_mismatches == 2 && !status.port_open);
    }
    security_start(false);
    CHECK(!iwx_link_status(&status) && !status.pmkid_mismatches);
    CHECK(!iwx_stop(&report) && !hw.live_pages && !hw.violations);
}

static void test_security_trace(void)
{
    iwx_probe_report_t report;
    iwx_link_status_t s;
    const unsigned statuses[] = {1, 2, 0x83};
    for (unsigned k = 0; k < 3; k++) {
        security_m1(false, true);
        CHECK(!iwx_link_status(&s) && s.eapol_tx_count == 1 && s.rx_trace_count == 1);
        CHECK(s.rx_trace[0].reason == IWX_RX_ACCEPTED && s.rx_trace[0].key_info == 0x8a);
        CHECK(s.rx_trace[0].addresses == 7 && s.rx_trace[0].ether_type == 0x888e);
        const iwx_tx_sample_t *d = &s.eapol_tx[0];
        CHECK(!d->completed && d->dma_match && d->kind == WPA2_TX_M2);
        CHECK(d->frame_bytes == 153 && d->byte_count == 39 && d->command_flags == 3);
        CHECK(d->key_info == 0x10a && d->version == 2 && d->frame_control == 0x108);
        CHECK(d->tb_bytes[0] == 20 && d->tb_bytes[1] == 28 && d->tb_bytes[2] == 129);
        uint8_t completed[48] = {1,4,2,3};
        put_u32(completed + 4, 0x4000);
        completed[36] = hw.tx_queue; completed[37] = hw.tx_queue >> 8;
        completed[40] = statuses[k]; completed[41] = 0x12;
        put_u32(completed + 44, hw.tx_cursor);
        queue_fw_packet(0x1c, 0, transport.tx_index, hw.tx_queue & 31, completed, sizeof(completed));
        CHECK((iwx_poll() > 0) == (k != 2));
        CHECK(!iwx_link_status(&s) && !s.port_open && !s.key_installs);
        d = &s.eapol_tx[0];
        CHECK(d->completed && d->completed_ns >= d->submitted_ns && d->status == (0x1200 | statuses[k]));
        CHECK(d->ack_failures == 3 && d->rts_failures == 2 && d->bt_kills == 4 && d->completion_rate == 0x4000);
        iwx_tx_sample_t saved = *d;
        CHECK(!iwx_stop(&report) && !hw.live_pages && !hw.violations);
        CHECK(!iwx_link_status(&s) && !memcmp(&s.eapol_tx[0], &saved, sizeof(saved)));
    }
    security_start(false);
    uint8_t frame[WPA2_EAPOL_MAX]; eapol_base(frame, 0x8a, 1);
    CHECK(!iwx_link_status(&s) && !s.rx_trace_count && !s.eapol_tx_count);
    for (unsigned i = 0; i < 12; i++) {
        queue_eapol(frame, 99, false, 0);
        CHECK(iwx_poll() > 0 && transport.tx_pending);
        CHECK(iwx_poll() > 0 && !transport.tx_pending);
    }
    CHECK(!iwx_link_status(&s) && s.rx_trace_count == 8 && s.rx_trace_overwritten == 4);
    CHECK(s.eapol_tx_count == 8 && s.eapol_tx_overwritten == 4 && s.rx_reasons[IWX_RX_ACCEPTED] == 12);
    for (unsigned i = 0; i < 8; i++) CHECK(s.eapol_tx[i].completed && s.eapol_tx[i].dma_match);
    iwx_rx_sample_t beacon = {.frame_control = 0x80, .addresses = 2, .reason = IWX_RX_FORMAT};
    link_rx_trace(&runtime.link.status, &beacon);
    CHECK(runtime.link.status.rx_trace_overwritten == 4);
    runtime.link.status.port_open = true;
    iwx_rx_sample_t saved_handshake[IWX_LINK_TRACE_MAX];
    memcpy(saved_handshake, runtime.link.status.rx_trace, sizeof(saved_handshake));
    link_rx_trace(&runtime.link.status, &beacon);
    iwx_rx_sample_t traffic = {.frame_control = 0x4208, .addresses = 3, .reason = IWX_RX_ACCEPTED};
    link_rx_trace(&runtime.link.status, &traffic);
    CHECK(!runtime.link.status.data_trace_count);
    traffic.udp_source = 68; traffic.udp_destination = 67;
    link_rx_trace(&runtime.link.status, &traffic);
    traffic.udp_source = 67; traffic.udp_destination = 68;
    link_rx_trace(&runtime.link.status, &traffic);
    traffic.udp_source = traffic.udp_destination = 0; traffic.addresses |= 32;
    link_rx_trace(&runtime.link.status, &traffic);
    traffic.addresses &= ~32; traffic.reason = IWX_RX_REPLAY;
    for (unsigned i = 0; i < 10; i++) { traffic.pn = i; link_rx_trace(&runtime.link.status, &traffic); }
    CHECK(runtime.link.status.data_trace_count == 8 && runtime.link.status.data_trace_skipped == 5);
    CHECK(runtime.link.status.data_trace[0].udp_source == 68 && runtime.link.status.data_trace[7].pn == 4);
    CHECK(!memcmp(saved_handshake, runtime.link.status.rx_trace, sizeof(saved_handshake)));
    unsigned saturated = (unsigned)-1;
    link_diag_increment(&saturated); CHECK(saturated == (unsigned)-1);
    CHECK(!iwx_stop(&report) && !hw.live_pages && !hw.violations);
}

static void queue_ethernet(const uint8_t *e, unsigned n, bool protect, unsigned pn)
{
    uint8_t mpdu[48 + IWX_DATA_MAX + 8] = {0};
    unsigned body = protect ? 80 : 72;
    mpdu[0] = body + n - 54; mpdu[1] = (body + n - 54) >> 8;
    put_u32(mpdu + 12, protect ? 0xa43 : 3); mpdu[34] = 1;
    mpdu[48] = 8; mpdu[49] = protect ? 0x42 : 2;
    memcpy(mpdu + 52, e, 6); memcpy(mpdu + 58, runtime.link.ap.bssid, 6);
    memcpy(mpdu + 64, e + 6, 6);
    if (protect) { mpdu[72] = pn; mpdu[73] = pn >> 8; mpdu[75] = 0x20; }
    memcpy(mpdu + body, "\xaa\xaa\x03\x00\x00\x00", 6);
    memcpy(mpdu + body + 6, e + 12, n - 12);
    bool tkip = protect && (e[0] & 1) && runtime.wpa.group_cipher == WPA2_CIPHER_TKIP;
    if (protect && (e[0] & 1)) mpdu[75] |= 0x40; /* Installed GTK ID 1. */
    if (tkip) {
        put_u32(mpdu + 12, 0xb6f);
        mpdu[72] = pn >> 8; mpdu[73] = ((pn >> 8) | 0x20) & 0x7f; mpdu[74] = pn;
    }
    unsigned slot = mock_rx_slot();
    queue_fw_packet(0xc1, 0, 0, 0x80, mpdu, body + n - 6);
    if (tkip) {
        uint8_t *header = dma_ptr(transport.rx[slot]);
        put_u32(header, rx_le32(header) | IWX_RX_RADA);
    }
}

static void test_ethernet_queues(void)
{
    iwx_probe_report_t r;
    nic_link_state_t link;
    uint8_t tx[IWX_ETH_MAX] = {2}, rx[IWX_ETH_MAX], out[IWX_ETH_MAX + 8];
    uint32_t len;
    for (unsigned secure = 0; secure < 2; secure++) {
        if (secure) security_m1(false, false);
        else {
            link_fixture();
            CHECK(!iwx_connect_open(runtime.bss[0].bssid));
        }
        CHECK(!iwx_link_up() && !iwx_read_link(&link) && !link.up);
        CHECK(iwx_send(tx, sizeof(tx)) == -1 && iwx_recv(out, &len) == -1);
        if (secure) {
            uint8_t eapol[WPA2_EAPOL_MAX];
            unsigned n = eapol_message(eapol, true, 2, 1, 10);
            queue_eapol(eapol, n, false, 0);
        }
        CHECK(iwx_poll() > 0 && iwx_poll() > 0);
        CHECK(iwx_link_up() && !iwx_read_link(&link) && link.up);
        CHECK(!memcmp(link.mac, runtime.link.local, 6));
        uint64_t generation = link.generation;
        memcpy(tx + 6, link.mac, 6); tx[12] = 8; tx[13] = 0;
        for (unsigned i = 14; i < sizeof(tx); i++) tx[i] = i;
        CHECK(iwx_send(NULL, sizeof(tx)) == -1 && iwx_send(tx, 13) == -1);
        CHECK(iwx_send(tx, sizeof(tx) + 1) == -1);
        probe_lock = 1;
        CHECK(iwx_read_link(&link) == -1 && iwx_send(tx, sizeof(tx)) == -1);
        CHECK(iwx_recv(out, &len) == -1); probe_lock = 0;
        tx[6] ^= 2; CHECK(iwx_send(tx, sizeof(tx)) == -1); tx[6] ^= 2;
        tx[12] = 0x88; tx[13] = 0x8e;
        CHECK(iwx_send(tx, sizeof(tx)) == -1); tx[12] = 8; tx[13] = 0;
        for (unsigned wrap = 0; wrap < 18; wrap++) {
            for (unsigned i = 0; i < 16; i++) CHECK(!iwx_send(tx, sizeof(tx)));
            CHECK(runtime.tx_count == 16 && iwx_send(tx, sizeof(tx)) == -1);
            hw.eapol_hold = true;
            CHECK(iwx_poll() >= 0 && transport.tx_pending && runtime.tx_count == 15);
            CHECK(!memcmp((uint8_t *)dma_ptr(transport.tx_page) + 70, tx + 12, sizeof(tx) - 12));
            CHECK(iwx_poll() == 0 && runtime.tx_count == 15);
            hw.eapol_hold = false; eapol_complete();
            for (unsigned i = 0; i < 16; i++) CHECK(iwx_poll() > 0);
            CHECK(!transport.tx_pending && !runtime.tx_count && iwx_link_up());
        }
        hw.tx_fault = 9; /* Ordinary data loss must not fail the association. */
        CHECK(!iwx_send(tx, sizeof(tx)) && iwx_poll() >= 0 && iwx_poll() > 0);
        CHECK(iwx_link_up() && runtime.link.status.tx_failed == 1); hw.tx_fault = 0;
        memcpy(rx, tx, sizeof(rx)); memcpy(rx, link.mac, 6); memcpy(rx + 6, tx, 6);
        for (unsigned wrap = 0; wrap < 3; wrap++) {
            for (unsigned i = 0; i < 20; i++) queue_ethernet(rx, sizeof(rx), secure, wrap * 20 + i + 1);
            CHECK(iwx_poll() > 0 && runtime.rx_count == 16);
            CHECK(runtime.link.status.data_rx_dropped == (wrap + 1) * 4);
            for (unsigned i = 0; i < 16; i++) {
                memset(out, 0xcc, sizeof(out)); len = 0;
                CHECK(!iwx_recv(out + 4, &len) && len == sizeof(rx));
                CHECK(!memcmp(out + 4, rx, len) && out[0] == 0xcc && out[sizeof(out) - 1] == 0xcc);
            }
            len = 123; CHECK(iwx_recv(out, &len) == -1 && len == 123);
        }
        if (secure) {
            queue_ethernet(rx, sizeof(rx), true, 60); /* Replay. */
            queue_ethernet(rx, sizeof(rx), false, 0); /* Plaintext downgrade. */
            CHECK(iwx_poll() > 0 && !runtime.rx_count);
        }
        CHECK(!iwx_send(tx, sizeof(tx)));
        queue_ethernet(rx, sizeof(rx), secure, 61); CHECK(iwx_poll() > 0);
        CHECK(!iwx_stop(&r) && !iwx_link_up() && !hw.violations && !hw.live_pages);
        CHECK(!iwx_read_link(&link) && !link.up && link.generation == generation);
        CHECK(bytes_zero(runtime.tx_data, sizeof(runtime.tx_data)) && bytes_zero(runtime.rx_data, sizeof(runtime.rx_data)));
        CHECK(iwx_send(tx, sizeof(tx)) == -1 && iwx_recv(out, &len) == -1);
    }
}

static void test_network_identity(void)
{
    security_fixture();
    iwx_probe_report_t r;
    iwx_bss_t selected = runtime.bss[0];
    uint8_t pmk[32] = {1};
    CHECK(iwx_connect_network(NULL, pmk));
    selected.ssid_len = 33; CHECK(iwx_connect_network(&selected, pmk));
    selected = runtime.bss[0]; selected.channel++;
    CHECK(iwx_connect_network(&selected, pmk) && !hw.tx_calls);
    selected = runtime.bss[0]; selected.ssid[0] ^= 1;
    CHECK(iwx_connect_network(&selected, pmk) && !hw.tx_calls);
    selected = runtime.bss[0]; selected.rsn_ie[2] ^= 1;
    CHECK(iwx_connect_network(&selected, pmk) && !hw.tx_calls);
    selected = runtime.bss[0];
    CHECK(iwx_connect_network(&selected, NULL) && !hw.tx_calls);
    CHECK(!iwx_connect_network(&selected, pmk) && hw.tx_calls == 1);
    CHECK(!iwx_stop(&r) && !hw.live_pages && !hw.violations);
}

int main(void)
{
    test_link_queue_flags();
    if (failures) {
        printf("iwx TX queue flags: %u checks, %u failures\n", checks, failures);
        return 1;
    }
    test_probe();
    test_boot();
    test_boot_snapshot();
    test_snapshot_pcie();
    test_boot_trace();
    test_persistent_progress();
    test_nvm();
    test_nvm_decode();
    test_notification_sequences();
    test_command_bounds();
    test_runtime_lifecycle();
    test_runtime_rx();
    test_runtime_errors();
    test_runtime_commands();
    test_scan_lifecycle();
    test_scan_failures();
    test_link_lifecycle();
    test_link_failures();
    test_link_queue_contract();
    test_security_lifecycle();
    test_security_failures();
    test_security_diagnostics();
    test_security_pmkid();
    test_security_trace();
    test_ethernet_queues();
    test_network_identity();
    test_tkip_lifecycle();
    printf("iwx hardware/firmware transport: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
