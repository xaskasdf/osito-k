/* Exercise the production startup against a controller left running by a
 * previous kernel. No audio device or host speakers are used. */
#include "../include/types.h"
static uint8_t hda_read8(uint32_t off);
static uint16_t hda_read16(uint32_t off);
static uint32_t hda_read32(uint32_t off);
static void hda_write8(uint32_t off, uint8_t value);
static void hda_write16(uint32_t off, uint16_t value);
static void hda_write32(uint32_t off, uint32_t value);
#define HDA_HOST_TEST 1
#include "../drivers/hda.c"

extern int printf(const char *, ...);
extern void abort(void);

#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)

static unsigned checks, failures, violations, reset_count, enable_count;
static unsigned allocations, fail_allocation, stream_count, command_count;
static uint8_t registers[4096];
static uint8_t dma_memory[4][HDA_BUF_SIZE * HDA_BDL_ENTRIES]
    __attribute__((aligned(4096)));
static uint16_t pci_command, codec_mask;
static uint64_t clock_ns, reset_started, reset_finished;
static bool stuck_assert, stuck_release, stuck_corb, stuck_rirb, stuck_clock;
static bool refuse_bme_off, refuse_bme_on, no_decode, bad_codec, bad_route;
static bool require_bme_for_ring_run;
static int stuck_stream;

static uint32_t hda_test_register(uint32_t off, unsigned width)
{
    if (off + width > sizeof(registers)) abort();
    uint32_t result = 0;
    for (unsigned i = 0; i < width; i++)
        result |= (uint32_t)registers[off + i] << (i * 8);
    return result;
}

static void hda_test_put(uint32_t off, uint32_t value, unsigned width)
{
    if (off + width > sizeof(registers)) abort();
    for (unsigned i = 0; i < width; i++)
        registers[off + i] = value >> (i * 8);
}

static bool hda_test_dma_safe(void)
{
    if (!reset_count) return false;
    for (unsigned i = 0; i < stream_count; i++)
        if (hda_test_register(HDA_SD_BASE + i * HDA_SD_SIZE, 2) &
            HDA_SD_CTL_RUN) return false;
    if (hda_test_register(HDA_DPLBASE, 4) & 1) return false;
    if ((registers[HDA_CORBCTL] & HDA_CORBCTL_RUN) &&
        (!hda.corb_phys || hda_test_register(HDA_CORBLBASE, 4) !=
                           (uint32_t)hda.corb_phys)) return false;
    if ((registers[HDA_RIRBCTL] & HDA_RIRBCTL_RUN) &&
        (!hda.rirb_phys || hda_test_register(HDA_RIRBLBASE, 4) !=
                           (uint32_t)hda.rirb_phys)) return false;
    return true;
}

static uint32_t hda_test_codec_response(uint32_t command)
{
    unsigned nid = (command >> 20) & 0xff;
    unsigned verb = (command >> 8) & 0xfff;
    unsigned param = command & 0xff;
    if (verb == HDA_VERB_GET_PARAM) {
        switch (param) {
        case HDA_PARAM_VENDOR_ID: return 0x8086280b;
        case HDA_PARAM_NODE_COUNT:
            return nid == 0 ? 0x10001 : 0x20002;
        case HDA_PARAM_FUNC_GRP_TYPE: return 1;
        case HDA_PARAM_AUDIO_WIDGET:
            return nid == 2 ? HDA_WCAP_STEREO :
                (HDA_WIDGET_PIN << 20) | HDA_WCAP_CONN_LIST;
        case HDA_PARAM_PCM_RATES: return HDA_RATE_48KHZ | HDA_BITS_16;
        case HDA_PARAM_STREAM_FMTS: return HDA_STREAM_FMT_PCM;
        case HDA_PARAM_PIN_CAP: return HDA_PINCAP_OUT;
        case HDA_PARAM_CONN_LIST_LEN: return 1;
        default: return 0;
        }
    }
    if (verb == HDA_VERB_GET_CONFIG)
        return (2U << 30) | (HDA_DEVICE_SPEAKER << 20);
    if (verb == HDA_VERB_GET_CONN_LIST) return 2;
    return 0;
}

static uint8_t hda_read8(uint32_t off)
{
    return hda_test_register(off, 1);
}

static uint16_t hda_read16(uint32_t off)
{
    if (off == HDA_STATESTS && reset_finished &&
        clock_ns - reset_finished < 521000) violations++;
    return hda_test_register(off, 2);
}

static uint32_t hda_read32(uint32_t off)
{
    return hda_test_register(off, 4);
}

static void hda_write8(uint32_t off, uint8_t value)
{
    if (require_bme_for_ring_run && !(pci_command & 4) &&
        (off == HDA_CORBCTL || off == HDA_RIRBCTL) && (value & 2))
        return;
    if (off == HDA_CORBCTL && stuck_corb && !(value & HDA_CORBCTL_RUN))
        return;
    if (off == HDA_RIRBCTL && stuck_rirb && !(value & HDA_RIRBCTL_RUN))
        return;
    hda_test_put(off, value, 1);
}

static void hda_write16(uint32_t off, uint16_t value)
{
    if (off >= HDA_SD_BASE && (off - HDA_SD_BASE) % HDA_SD_SIZE == 0 &&
        (int)((off - HDA_SD_BASE) / HDA_SD_SIZE) == stuck_stream &&
        !(value & HDA_SD_CTL_RUN)) return;
    if (off == HDA_STATESTS) {
        hda_test_put(off, hda_test_register(off, 2) & ~value, 2);
        return;
    }
    if (off == HDA_ICS && value == HDA_ICS_ICB) {
        uint32_t command = hda_test_register(HDA_ICO, 4);
        command_count++;
        unsigned verb = (command >> 8) & 0xfff;
        if (bad_codec || (bad_route && verb == HDA_VERB_SET_PIN_CTRL)) {
            hda_test_put(off, HDA_ICS_ICB, 2);
            return;
        }
        hda_test_put(HDA_IRI, hda_test_codec_response(command), 4);
        hda_test_put(off, HDA_ICS_IRV, 2);
        return;
    }
    hda_test_put(off, value, 2);
}

static void hda_write32(uint32_t off, uint32_t value)
{
    if (off == HDA_GCTL && !(value & HDA_GCTL_CRST)) {
        if (registers[HDA_CORBCTL] & HDA_CORBCTL_RUN) violations++;
        if (registers[HDA_RIRBCTL] & HDA_RIRBCTL_RUN) violations++;
        for (unsigned i = 0; i < stream_count; i++)
            if (hda_test_register(HDA_SD_BASE + i * HDA_SD_SIZE, 2) &
                HDA_SD_CTL_RUN) violations++;
        if (pci_command & 4) violations++;
        if (stuck_assert) return;
        uint16_t cap = hda_test_register(HDA_GCAP, 2);
        memset(registers, 0, sizeof(registers));
        hda_test_put(HDA_GCAP, cap, 2);
        registers[HDA_CORBSIZE] = registers[HDA_RIRBSIZE] = 0x40;
        reset_started = clock_ns;
        reset_count++;
    } else if (off == HDA_GCTL && (value & HDA_GCTL_CRST)) {
        if (stuck_release) return;
        if (!reset_count || clock_ns - reset_started < 100000) violations++;
        reset_finished = clock_ns;
        hda_test_put(HDA_STATESTS, codec_mask, 2);
    }
    hda_test_put(off, value, 4);
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off)
{
    (void)bus; (void)dev; (void)func;
    if (off != 4) abort();
    return pci_command;
}

void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func,
                     uint16_t off, uint16_t value)
{
    (void)bus; (void)dev; (void)func;
    if (off != 4) abort();
    if (refuse_bme_off && !(value & 4)) value |= 4;
    if (refuse_bme_on) value &= ~4U;
    if (no_decode) value &= ~2U;
    if ((value & 4) && !(pci_command & 4)) {
        enable_count++;
        if (!hda_test_dma_safe()) violations++;
    }
    pci_command = value;
}

void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func)
{
    pci_cfg_write16(bus, dev, func, 4, pci_command | 6);
}

uint64_t idt_get_monotonic_ns(void)
{
    if (!stuck_clock) clock_ns += 1000;
    return clock_ns;
}

void *mem_alloc_aligned(uint64_t size, uint64_t alignment)
{
    if (++allocations == fail_allocation) return NULL;
    if (allocations > 4 || size > sizeof(dma_memory[0]) || alignment > 4096)
        abort();
    return (void *)(uintptr_t)VIRT_TO_PHYS(dma_memory[allocations - 1]);
}

void serial_puts(const char *s) { (void)s; }
void serial_putc(char c) { (void)c; }
void serial_puthex(uint64_t v, int digits) { (void)v; (void)digits; }
void serial_putdec(uint64_t v) { (void)v; }

static void hda_test_reset(unsigned iss, unsigned oss, unsigned bss)
{
    memset(registers, 0, sizeof(registers));
    memset(dma_memory, 0x7f, sizeof(dma_memory));
    violations = allocations = fail_allocation = reset_count = enable_count = 0;
    command_count = 0;
    pci_command = 0x400; /* kexec disabled bus mastering, registers still live */
    codec_mask = 1;
    clock_ns = 1000;
    reset_started = reset_finished = 0;
    stuck_assert = stuck_release = stuck_corb = stuck_rirb = stuck_clock = false;
    refuse_bme_off = refuse_bme_on = no_decode = bad_codec = bad_route = false;
    require_bme_for_ring_run = false;
    stuck_stream = -1;
    stream_count = iss + oss + bss;
    hda_test_put(HDA_GCAP, (oss << 12) | (iss << 8) | (bss << 3) | 1, 2);
    hda_test_put(HDA_GCTL, HDA_GCTL_CRST, 4);
    hda_test_put(HDA_DPLBASE, 0xdead0001, 4);
    hda_test_put(HDA_CORBLBASE, 0xdead1000, 4);
    hda_test_put(HDA_RIRBLBASE, 0xdead2000, 4);
    registers[HDA_CORBCTL] = HDA_CORBCTL_RUN;
    registers[HDA_RIRBCTL] = HDA_RIRBCTL_RUN;
    for (unsigned i = 0; i < stream_count; i++) {
        hda_test_put(HDA_SD_BASE + i * HDA_SD_SIZE, HDA_SD_CTL_RUN, 2);
        hda_test_put(HDA_SD_BASE + i * HDA_SD_SIZE + HDA_SD_BDLPL,
                     0xdead3000 + 4096 * i, 4);
    }
}

static void hda_test_startup(bool success)
{
    CHECK((hda_init(0x100000, 0, 27, 0) == 0) == success);
    CHECK(hda_is_ready() == success);
    CHECK(violations == 0);
    CHECK((pci_command & 0x400) != 0); /* preserve unrelated command bits */
    if (!success && !refuse_bme_off) CHECK(!(pci_command & 4));
    if (success) {
        CHECK(enable_count == 1);
        CHECK(hda_test_dma_safe());
        CHECK(!hda.playing);
        CHECK(command_count > 10);
        unsigned dirty = 0;
        for (unsigned i = 0; i < hda.buf_size; i++) dirty |= dma_memory[2][i];
        CHECK(dirty == 0);
    }
}

int main(void)
{
    hda_test_reset(4, 4, 0); hda_test_startup(true);
    hda_test_reset(2, 3, 5); hda_test_startup(true);
    hda_test_reset(7, 9, 0); require_bme_for_ring_run = true;
    hda_test_startup(true);
    hda_test_reset(1, 1, 0); codec_mask = 0; hda_test_startup(false);
    hda_test_reset(2, 0, 2); hda_test_startup(false);
    hda_test_reset(1, 1, 0); stuck_assert = true; hda_test_startup(false);
    CHECK(reset_count == 0 && command_count == 0);
    hda_test_reset(1, 1, 0); stuck_release = true; hda_test_startup(false);
    hda_test_reset(1, 1, 0); stuck_corb = true; hda_test_startup(false);
    CHECK(reset_count == 0);
    hda_test_reset(1, 1, 0); stuck_rirb = true; hda_test_startup(false);
    CHECK(reset_count == 0);
    for (int i = 0; i < 5; i++) {
        hda_test_reset(1, 1, 3); stuck_stream = i; hda_test_startup(false);
        CHECK(reset_count == 0);
    }
    for (unsigned i = 1; i <= 4; i++) {
        hda_test_reset(1, 1, 0); fail_allocation = i; hda_test_startup(false);
    }
    hda_test_reset(1, 1, 0); bad_codec = true; hda_test_startup(false);
    hda_test_reset(1, 1, 0); bad_route = true; hda_test_startup(false);
    hda_test_reset(1, 1, 0); refuse_bme_on = true; hda_test_startup(false);
    hda_test_reset(1, 1, 0); refuse_bme_off = true; pci_command |= 4;
    hda_test_startup(false); CHECK(reset_count == 0 && allocations == 0);
    hda_test_reset(1, 1, 0); no_decode = true; hda_test_startup(false);
    CHECK(reset_count == 0 && allocations == 0);
    hda_test_reset(1, 1, 0); stuck_clock = true; hda_test_startup(false);
    CHECK(enable_count == 0);

    /* The handoff path sees the old live controller after PCI DMA is blocked.
     * Successful shutdown must leave link reset, not just a stopped output. */
    memset(&hda, 0, sizeof(hda));
    CHECK(hda_kexec_quiesce());
    for (unsigned fault = 0; fault < 6; fault++) {
        hda_test_reset(7, 9, 2);
        hda.bar0 = (void *)1;
        hda.initialized = hda.playing = true;
        if (fault == 1) stuck_corb = true;
        if (fault == 2) stuck_rirb = true;
        if (fault == 3) stuck_stream = 17;
        if (fault == 4) stuck_assert = true;
        if (fault == 5) stuck_clock = true;
        CHECK(hda_kexec_quiesce() == (fault == 0));
        CHECK(!hda.initialized && !hda.playing);
        CHECK(violations == 0 && !(pci_command & 4));
        if (fault == 0) {
            CHECK(!(hda_read32(HDA_GCTL) & HDA_GCTL_CRST));
            CHECK(clock_ns - reset_started >= 100000);
            CHECK(hda_test_dma_safe());
        }
    }
    printf("HDA startup: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
