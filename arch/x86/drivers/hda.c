/*
 * OsitoK x86-64 — Intel HDA (High Definition Audio) Driver
 *
 * Supports ICH6/ICH9 (QEMU), AMD Starship/Matisse, NVIDIA GA102.
 * Controller init, codec discovery, CORB/RIRB command interface,
 * output stream setup, and PCM playback via polling.
 */

#include "hda.h"

/* ── External Declarations ─────────────────────────────────────── */

extern void  serial_puts(const char *s);
extern void  serial_putc(char c);
extern void  serial_puthex(uint64_t val, int digits);
extern void  serial_putdec(uint64_t val);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func);
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);

/* ── Driver State ──────────────────────────────────────────────── */

typedef struct {
    volatile void *bar0;

    /* CORB/RIRB */
    uint32_t *corb;
    uint64_t *rirb;
    uint16_t  corb_wp;
    uint16_t  rirb_rp;

    /* Codec info */
    uint8_t   codec_addr;
    uint16_t  codec_vendor;
    uint16_t  codec_device;
    uint8_t   afg_nid;
    uint8_t   dac_nid;
    uint8_t   pin_nid;

    /* Stream */
    uint32_t  out_stream_off;
    uint8_t   num_iss;
    uint8_t   num_oss;
    uint8_t   stream_id;

    /* Detected format */
    uint32_t  sample_rate;
    uint8_t   bits_per_sample;
    uint16_t  fmt_reg;

    /* DMA */
    hda_bdl_entry_t *bdl;
    int16_t  *audio_buf;
    uint32_t  buf_size;

    bool      initialized;
    bool      playing;
} hda_state_t;

static hda_state_t hda;

/* ── MMIO Accessors ────────────────────────────────────────────── */

static inline uint8_t hda_read8(uint32_t off) {
    return *(volatile uint8_t *)((uint64_t)hda.bar0 + off);
}
static inline uint16_t hda_read16(uint32_t off) {
    return *(volatile uint16_t *)((uint64_t)hda.bar0 + off);
}
static inline uint32_t hda_read32(uint32_t off) {
    return mmio_read32((volatile void *)((uint64_t)hda.bar0 + off));
}
static inline void hda_write8(uint32_t off, uint8_t val) {
    *(volatile uint8_t *)((uint64_t)hda.bar0 + off) = val;
}
static inline void hda_write16(uint32_t off, uint16_t val) {
    *(volatile uint16_t *)((uint64_t)hda.bar0 + off) = val;
}
static inline void hda_write32(uint32_t off, uint32_t val) {
    mmio_write32((volatile void *)((uint64_t)hda.bar0 + off), val);
}

static inline void spin_delay(uint32_t iters) {
    for (volatile uint32_t i = 0; i < iters; i++)
        __asm__ volatile ("pause");
}

/* ── CORB/RIRB Init ───────────────────────────────────────────── */

static int hda_corb_rirb_init(void)
{
    /* Allocate CORB: 256 entries x 4 bytes = 1KB */
    hda.corb = (uint32_t *)mem_alloc_aligned(HDA_CORB_ENTRIES * 4, 4096);
    if (!hda.corb) return -1;
    memset(hda.corb, 0, HDA_CORB_ENTRIES * 4);

    /* Allocate RIRB: 256 entries x 8 bytes = 2KB */
    hda.rirb = (uint64_t *)mem_alloc_aligned(HDA_RIRB_ENTRIES * 8, 4096);
    if (!hda.rirb) return -1;
    memset(hda.rirb, 0, HDA_RIRB_ENTRIES * 8);

    /* Stop CORB/RIRB if running */
    hda_write8(HDA_CORBCTL, 0);
    hda_write8(HDA_RIRBCTL, 0);
    spin_delay(200);

    /* Set CORB size: prefer 256 entries (cap bit 2), else 16 (bit 1) */
    uint8_t corbsize = hda_read8(HDA_CORBSIZE);
    uint8_t corbcap = (corbsize >> 4) & 0xF;
    if (corbcap & 0x4)
        hda_write8(HDA_CORBSIZE, (corbsize & 0xFC) | 0x02);
    else if (corbcap & 0x2)
        hda_write8(HDA_CORBSIZE, (corbsize & 0xFC) | 0x01);

    /* Set CORB base address */
    hda_write32(HDA_CORBLBASE, (uint32_t)(uint64_t)hda.corb);
    hda_write32(HDA_CORBUBASE, (uint32_t)((uint64_t)hda.corb >> 32));

    /* Reset CORB read pointer: set bit 15, wait, clear, wait */
    hda_write16(HDA_CORBRP, (1 << 15));
    for (int i = 0; i < 200; i++) {
        if (hda_read16(HDA_CORBRP) & (1 << 15)) break;
        spin_delay(100);
    }
    hda_write16(HDA_CORBRP, 0);
    for (int i = 0; i < 200; i++) {
        if (!(hda_read16(HDA_CORBRP) & (1 << 15))) break;
        spin_delay(100);
    }

    /* CORB write pointer to 0 */
    hda_write16(HDA_CORBWP, 0);
    hda.corb_wp = 0;

    /* Set RIRB size (same logic) */
    uint8_t rirbsize = hda_read8(HDA_RIRBSIZE);
    uint8_t rirbcap = (rirbsize >> 4) & 0xF;
    if (rirbcap & 0x4)
        hda_write8(HDA_RIRBSIZE, (rirbsize & 0xFC) | 0x02);
    else if (rirbcap & 0x2)
        hda_write8(HDA_RIRBSIZE, (rirbsize & 0xFC) | 0x01);

    /* Set RIRB base address */
    hda_write32(HDA_RIRBLBASE, (uint32_t)(uint64_t)hda.rirb);
    hda_write32(HDA_RIRBUBASE, (uint32_t)((uint64_t)hda.rirb >> 32));

    /* Reset RIRB write pointer */
    hda_write16(HDA_RIRBWP, (1 << 15));
    spin_delay(200);

    /* Start CORB + RIRB DMA */
    hda_write8(HDA_CORBCTL, HDA_CORBCTL_RUN);
    hda_write8(HDA_RIRBCTL, HDA_RIRBCTL_RUN);
    spin_delay(500);

    /* Verify running */
    if (!(hda_read8(HDA_CORBCTL) & HDA_CORBCTL_RUN)) {
        serial_puts("[HDA] CORB failed to start\n");
        return -1;
    }
    if (!(hda_read8(HDA_RIRBCTL) & HDA_RIRBCTL_RUN)) {
        serial_puts("[HDA] RIRB failed to start\n");
        return -1;
    }

    serial_puts("[HDA] CORB/RIRB running\n");
    return 0;
}

/* ── Verb Send/Receive ─────────────────────────────────────────── */

/*
 * Immediate Command Interface (ICO/IRI/ICS at 0x60/0x64/0x68).
 * Simpler than CORB/RIRB: write verb, poll status, read response.
 * Falls back from CORB/RIRB which has DMA issues on some controllers.
 */
#define HDA_ICO     0x60    /* Immediate Command Output (32-bit) */
#define HDA_IRI     0x64    /* Immediate Response Input (32-bit) */
#define HDA_ICS     0x68    /* Immediate Command Status (16-bit) */
#define HDA_ICS_ICB (1 << 0)   /* Immediate Command Busy */
#define HDA_ICS_IRV (1 << 1)   /* Immediate Result Valid */

static int hda_send_verb(uint32_t verb, uint32_t *response)
{
    /* Wait for not busy */
    for (int i = 0; i < 1000; i++) {
        if (!(hda_read16(HDA_ICS) & HDA_ICS_ICB)) break;
        spin_delay(100);
    }
    if (hda_read16(HDA_ICS) & HDA_ICS_ICB) {
        serial_puts("[HDA] ICI busy timeout\n");
        return -1;
    }

    /* Clear IRV by writing 1 */
    hda_write16(HDA_ICS, HDA_ICS_IRV);

    /* Write verb to ICO — this triggers processing */
    hda_write32(HDA_ICO, verb);

    /* Set ICB to start command */
    hda_write16(HDA_ICS, HDA_ICS_ICB);

    /* Poll for completion: ICB clears and IRV sets */
    for (int timeout = 0; timeout < 2000000; timeout++) {
        uint16_t ics = hda_read16(HDA_ICS);
        if (!(ics & HDA_ICS_ICB) && (ics & HDA_ICS_IRV)) {
            if (response)
                *response = hda_read32(HDA_IRI);
            return 0;
        }
        __asm__ volatile ("pause");
    }

    serial_puts("[HDA] Verb timeout, ICS=");
    serial_puthex(hda_read16(HDA_ICS), 4);
    serial_puts("\n");
    return -1;
}

/*
 * Unified verb helper.
 * 12-bit verbs: verb=0xF00 etc, parm=8-bit.
 * 4-bit verbs:  verb=0x200/0x300 etc, parm=16-bit.
 * Encoding: (CAd << 28) | (NID << 20) | (verb << 8) | parm
 */
static uint32_t hda_cmd(uint8_t nid, uint32_t verb, uint32_t parm)
{
    uint32_t cmd = ((uint32_t)hda.codec_addr << 28) |
                   ((uint32_t)nid << 20) |
                   ((verb & 0xFFF) << 8) |
                   (parm & 0xFFFF);
    uint32_t resp = 0;
    hda_send_verb(cmd, &resp);
    return resp;
}

static void hda_detect_format(void);

/* ── Codec Discovery ───────────────────────────────────────────── */

static int hda_codec_init(void)
{
    /* Get codec vendor/device ID from root node (NID 0) */
    uint32_t vendor = hda_cmd(0x00, HDA_VERB_GET_PARAM, HDA_PARAM_VENDOR_ID);
    hda.codec_vendor = (vendor >> 16) & 0xFFFF;
    hda.codec_device = vendor & 0xFFFF;

    serial_puts("[HDA] Codec: ");
    serial_puthex(hda.codec_vendor, 4);
    serial_puts(":");
    serial_puthex(hda.codec_device, 4);
    serial_puts("\n");

    /* Get subordinate node count from root */
    uint32_t node_count = hda_cmd(0x00, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT);
    uint8_t start_nid = (node_count >> 16) & 0xFF;
    uint8_t num_nodes = node_count & 0xFF;

    /* Find Audio Function Group (type 0x01) */
    for (int i = 0; i < num_nodes; i++) {
        uint8_t nid = start_nid + i;
        uint32_t fgt = hda_cmd(nid, HDA_VERB_GET_PARAM, HDA_PARAM_FUNC_GRP_TYPE);
        if ((fgt & 0xFF) == 0x01) {
            hda.afg_nid = nid;
            serial_puts("[HDA] AFG at NID ");
            serial_putdec(nid);
            serial_puts("\n");
            break;
        }
    }
    if (hda.afg_nid == 0) {
        serial_puts("[HDA] No AFG found\n");
        return -1;
    }

    /* Power up AFG */
    hda_cmd(hda.afg_nid, HDA_VERB_SET_POWER, 0x00);  /* D0 */

    /* Enumerate AFG widgets */
    node_count = hda_cmd(hda.afg_nid, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT);
    start_nid = (node_count >> 16) & 0xFF;
    num_nodes = node_count & 0xFF;

    serial_puts("[HDA] Widgets: ");
    serial_putdec(num_nodes);
    serial_puts(" (NID ");
    serial_putdec(start_nid);
    serial_puts("-");
    serial_putdec(start_nid + num_nodes - 1);
    serial_puts(")\n");

    /* Walk widgets: find first DAC and first output-capable pin */
    for (int i = 0; i < num_nodes; i++) {
        uint8_t nid = start_nid + i;
        uint32_t wcap = hda_cmd(nid, HDA_VERB_GET_PARAM, HDA_PARAM_AUDIO_WIDGET);
        uint8_t wtype = (wcap >> 20) & 0xF;

        if (wtype == HDA_WIDGET_AUD_OUT && hda.dac_nid == 0) {
            hda.dac_nid = nid;
            serial_puts("[HDA] DAC at NID ");
            serial_putdec(nid);
            serial_puts("\n");
        }

        if (wtype == HDA_WIDGET_PIN && hda.pin_nid == 0) {
            uint32_t pincap = hda_cmd(nid, HDA_VERB_GET_PARAM, HDA_PARAM_PIN_CAP);
            if (pincap & (1 << 4)) {  /* Output capable */
                hda.pin_nid = nid;
                serial_puts("[HDA] Output pin at NID ");
                serial_putdec(nid);
                serial_puts("\n");
            }
        }
    }

    if (hda.dac_nid == 0 || hda.pin_nid == 0) {
        serial_puts("[HDA] Missing DAC or output pin\n");
        return -1;
    }

    /* Detect best format from DAC capabilities */
    hda_detect_format();

    return 0;
}

/* ── Format Auto-Detection ─────────────────────────────────────── */

static void hda_detect_format(void)
{
    uint32_t pcm = hda_cmd(hda.dac_nid, HDA_VERB_GET_PARAM, HDA_PARAM_PCM_RATES);

    /* Log supported rates */
    serial_puts("[HDA] DAC rates:");
    if (pcm & HDA_RATE_192KHZ)  serial_puts(" 192k");
    if (pcm & HDA_RATE_1764KHZ) serial_puts(" 176.4k");
    if (pcm & HDA_RATE_96KHZ)   serial_puts(" 96k");
    if (pcm & HDA_RATE_882KHZ)  serial_puts(" 88.2k");
    if (pcm & HDA_RATE_48KHZ)   serial_puts(" 48k");
    if (pcm & HDA_RATE_441KHZ)  serial_puts(" 44.1k");
    if (pcm & HDA_RATE_32KHZ)   serial_puts(" 32k");
    if (pcm & HDA_RATE_22KHZ)   serial_puts(" 22k");
    if (pcm & HDA_RATE_16KHZ)   serial_puts(" 16k");
    if (pcm & HDA_RATE_11KHZ)   serial_puts(" 11k");
    if (pcm & HDA_RATE_8KHZ)    serial_puts(" 8k");
    serial_puts(" | bits:");
    if (pcm & HDA_BITS_32) serial_puts(" 32");
    if (pcm & HDA_BITS_24) serial_puts(" 24");
    if (pcm & HDA_BITS_20) serial_puts(" 20");
    if (pcm & HDA_BITS_16) serial_puts(" 16");
    if (pcm & HDA_BITS_8)  serial_puts(" 8");
    serial_puts("\n");

    /* Select best sample rate (prefer 48kHz family, highest first) */
    uint16_t fmt_rate;
    if (pcm & HDA_RATE_192KHZ) {
        hda.sample_rate = 192000;
        fmt_rate = HDA_FMT_BASE_48 | HDA_FMT_MUL_4;
    } else if (pcm & HDA_RATE_96KHZ) {
        hda.sample_rate = 96000;
        fmt_rate = HDA_FMT_BASE_48 | HDA_FMT_MUL_2;
    } else if (pcm & HDA_RATE_1764KHZ) {
        hda.sample_rate = 176400;
        fmt_rate = HDA_FMT_BASE_441 | HDA_FMT_MUL_4;
    } else if (pcm & HDA_RATE_882KHZ) {
        hda.sample_rate = 88200;
        fmt_rate = HDA_FMT_BASE_441 | HDA_FMT_MUL_2;
    } else if (pcm & HDA_RATE_48KHZ) {
        hda.sample_rate = 48000;
        fmt_rate = HDA_FMT_BASE_48 | HDA_FMT_MUL_1;
    } else if (pcm & HDA_RATE_441KHZ) {
        hda.sample_rate = 44100;
        fmt_rate = HDA_FMT_BASE_441 | HDA_FMT_MUL_1;
    } else {
        /* Fallback: assume 48kHz */
        hda.sample_rate = 48000;
        fmt_rate = HDA_FMT_BASE_48 | HDA_FMT_MUL_1;
    }

    /* Select best bit depth */
    uint16_t fmt_bits;
    if (pcm & HDA_BITS_32) {
        hda.bits_per_sample = 32;
        fmt_bits = HDA_FMT_32BIT;
    } else if (pcm & HDA_BITS_24) {
        hda.bits_per_sample = 24;
        fmt_bits = HDA_FMT_24BIT;
    } else if (pcm & HDA_BITS_20) {
        hda.bits_per_sample = 20;
        fmt_bits = HDA_FMT_20BIT;
    } else if (pcm & HDA_BITS_16) {
        hda.bits_per_sample = 16;
        fmt_bits = HDA_FMT_16BIT;
    } else {
        hda.bits_per_sample = 16;
        fmt_bits = HDA_FMT_16BIT;
    }

    /* Build FMT register: rate + bits + stereo */
    hda.fmt_reg = fmt_rate | HDA_FMT_DIV_1 | fmt_bits | HDA_FMT_STEREO;

    serial_puts("[HDA] Selected: ");
    serial_putdec(hda.sample_rate / 1000);
    serial_puts("kHz/");
    serial_putdec(hda.bits_per_sample);
    serial_puts("bit stereo (FMT=0x");
    serial_puthex(hda.fmt_reg, 4);
    serial_puts(")\n");
}

/* ── Stream Setup ──────────────────────────────────────────────── */

static int hda_stream_init(void)
{
    uint32_t sd = hda.out_stream_off;

    /* Stop stream if running */
    uint32_t ctl = hda_read32(sd + HDA_SD_CTL) & 0x00FFFFFF;
    ctl &= ~HDA_SD_CTL_RUN;
    hda_write32(sd + HDA_SD_CTL, ctl);
    spin_delay(200);

    /* Clear status bits */
    hda_write8(sd + HDA_SD_STS,
               HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE);

    /* Allocate audio buffer: 1 second at detected rate/depth/stereo */
    uint32_t bytes_per_sample = (hda.bits_per_sample + 7) / 8;  /* round up for 20-bit */
    hda.buf_size = hda.sample_rate * bytes_per_sample * 2;       /* stereo */
    hda.audio_buf = (int16_t *)mem_alloc_aligned(hda.buf_size, 4096);
    if (!hda.audio_buf) {
        serial_puts("[HDA] Failed to allocate audio buffer\n");
        return -1;
    }
    memset(hda.audio_buf, 0, hda.buf_size);

    /* Allocate BDL (128-byte aligned per spec) */
    hda.bdl = (hda_bdl_entry_t *)mem_alloc_aligned(
        HDA_BDL_ENTRIES * sizeof(hda_bdl_entry_t), 128);
    if (!hda.bdl) {
        serial_puts("[HDA] Failed to allocate BDL\n");
        return -1;
    }
    memset(hda.bdl, 0, HDA_BDL_ENTRIES * sizeof(hda_bdl_entry_t));

    /* BDL entry 0: entire audio buffer */
    hda.bdl[0].addr = (uint64_t)hda.audio_buf;
    hda.bdl[0].length = hda.buf_size;
    hda.bdl[0].ioc = 1;

    /* Configure stream descriptor */
    ctl = (uint32_t)hda.stream_id << 20;  /* Stream tag in bits 23:20 */
    hda_write32(sd + HDA_SD_CTL, ctl);

    hda_write32(sd + HDA_SD_CBL, hda.buf_size);
    hda_write16(sd + HDA_SD_LVI, 0);  /* 1 BDL entry: LVI = 0 */
    hda_write16(sd + HDA_SD_FMT, hda.fmt_reg);

    hda_write32(sd + HDA_SD_BDLPL, (uint32_t)(uint64_t)hda.bdl);
    hda_write32(sd + HDA_SD_BDLPU, (uint32_t)((uint64_t)hda.bdl >> 32));

    serial_puts("[HDA] Stream configured (");
    serial_putdec(hda.sample_rate / 1000);
    serial_puts("kHz/");
    serial_putdec(hda.bits_per_sample);
    serial_puts("bit/stereo, ");
    serial_putdec(hda.buf_size / 1024);
    serial_puts("KB buf)\n");
    return 0;
}

/* ── Output Pipeline ───────────────────────────────────────────── */

static void hda_setup_output(void)
{
    /* Configure DAC converter: stream ID + channel 0 */
    hda_cmd(hda.dac_nid, HDA_VERB_SET_CONV_CTRL,
            (hda.stream_id << 4) | 0);

    /* Set DAC format to match stream */
    hda_cmd(hda.dac_nid, HDA_VERB_SET_STREAM_FMT, hda.fmt_reg);

    /* Power up DAC */
    hda_cmd(hda.dac_nid, HDA_VERB_SET_POWER, 0x00);

    /* Enable output on pin */
    hda_cmd(hda.pin_nid, HDA_VERB_SET_PIN_CTRL, HDA_PIN_OUT_EN);

    /* Power up pin */
    hda_cmd(hda.pin_nid, HDA_VERB_SET_POWER, 0x00);

    /* Unmute DAC output amp: output + left + right + max gain */
    hda_cmd(hda.dac_nid, HDA_VERB_SET_AMP,
            HDA_AMP_SET_OUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
            HDA_AMP_GAIN(0x7F));

    /* Unmute pin output amp */
    hda_cmd(hda.pin_nid, HDA_VERB_SET_AMP,
            HDA_AMP_SET_OUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
            HDA_AMP_GAIN(0x7F));

    /* Enable EAPD (external amplifier) if supported */
    hda_cmd(hda.pin_nid, HDA_VERB_SET_EAPD, 0x02);
}

/* ── Sine Wave Table ───────────────────────────────────────────── */

/* 256 entries: sin(2*pi*i/256) * 32767, quarter-wave symmetric */
static const int16_t sine_table[256] = {
        0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
     6393,  7179,  7962,  8739,  9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    /* peak */
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
    32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683,
    27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868,
    18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278,  9512,  8739,  7962,  7179,
     6393,  5602,  4808,  4011,  3212,  2410,  1608,   804,
    /* zero crossing → negative half */
        0,  -804, -1608, -2410, -3212, -4011, -4808, -5602,
    -6393, -7179, -7962, -8739, -9512,-10278,-11039,-11793,
   -12539,-13279,-14010,-14732,-15446,-16151,-16846,-17530,
   -18204,-18868,-19519,-20159,-20787,-21403,-22005,-22594,
   -23170,-23731,-24279,-24811,-25329,-25832,-26319,-26790,
   -27245,-27683,-28105,-28510,-28898,-29268,-29621,-29956,
   -30273,-30571,-30852,-31113,-31356,-31580,-31785,-31971,
   -32137,-32285,-32412,-32521,-32609,-32678,-32728,-32757,
    /* trough */
   -32767,-32757,-32728,-32678,-32609,-32521,-32412,-32285,
   -32137,-31971,-31785,-31580,-31356,-31113,-30852,-30571,
   -30273,-29956,-29621,-29268,-28898,-28510,-28105,-27683,
   -27245,-26790,-26319,-25832,-25329,-24811,-24279,-23731,
   -23170,-22594,-22005,-21403,-20787,-20159,-19519,-18868,
   -18204,-17530,-16846,-16151,-15446,-14732,-14010,-13279,
   -12539,-11793,-11039,-10278, -9512, -8739, -7962, -7179,
    -6393, -5602, -4808, -4011, -3212, -2410, -1608,  -804,
};

/* ── Public API ────────────────────────────────────────────────── */

int hda_init(uint64_t bar0_phys, uint8_t bus, uint8_t dev, uint8_t func)
{
    memset(&hda, 0, sizeof(hda));
    hda.bar0 = (volatile void *)bar0_phys;

    /* Enable PCI bus master + memory space */
    pci_enable_bus_master(bus, dev, func);

    /* Disable all interrupts (we use polling) */
    hda_write32(HDA_INTCTL, 0);

    /* Read capabilities */
    uint16_t gcap = hda_read16(HDA_GCAP);
    uint8_t vmaj = hda_read8(HDA_VMAJ);
    uint8_t vmin = hda_read8(HDA_VMIN);

    hda.num_iss = (gcap >> 8) & 0xF;
    hda.num_oss = (gcap >> 12) & 0xF;

    /* First output stream descriptor: skip input streams */
    hda.out_stream_off = HDA_SD_BASE + hda.num_iss * HDA_SD_SIZE;
    hda.stream_id = 1;  /* Stream tag 1 (tag 0 = no stream) */

    serial_puts("[HDA] v");
    serial_putdec(vmaj);
    serial_puts(".");
    serial_putdec(vmin);
    serial_puts(", ISS=");
    serial_putdec(hda.num_iss);
    serial_puts(", OSS=");
    serial_putdec(hda.num_oss);
    serial_puts("\n");

    if (hda.num_oss == 0) {
        serial_puts("[HDA] No output streams available\n");
        return -1;
    }

    /* ── Controller Reset ── */

    /* Clear CRST → enter reset */
    hda_write32(HDA_GCTL, 0);
    for (int i = 0; i < 200; i++) {
        if (!(hda_read32(HDA_GCTL) & HDA_GCTL_CRST)) break;
        spin_delay(1000);
    }

    /* Codec PLL settle time */
    spin_delay(2000);

    /* Set CRST → exit reset */
    hda_write32(HDA_GCTL, HDA_GCTL_CRST);
    for (int i = 0; i < 200; i++) {
        if (hda_read32(HDA_GCTL) & HDA_GCTL_CRST) break;
        spin_delay(1000);
    }
    if (!(hda_read32(HDA_GCTL) & HDA_GCTL_CRST)) {
        serial_puts("[HDA] Controller reset failed\n");
        return -1;
    }

    /* Wait for codec detection (spec: 521us minimum) */
    spin_delay(5000);

    /* Check STATESTS for detected codecs */
    uint16_t statests = hda_read16(HDA_STATESTS);
    if (statests == 0) {
        serial_puts("[HDA] No codecs detected\n");
        return -1;
    }

    /* Find first codec address */
    for (int i = 0; i < 15; i++) {
        if (statests & (1 << i)) {
            hda.codec_addr = i;
            break;
        }
    }
    /* Clear STATESTS */
    hda_write16(HDA_STATESTS, statests);

    serial_puts("[HDA] Codec at address ");
    serial_putdec(hda.codec_addr);
    serial_puts("\n");

    /* ── Initialize subsystems ── */

    if (hda_corb_rirb_init() < 0) return -1;
    if (hda_codec_init() < 0) return -1;
    if (hda_stream_init() < 0) return -1;

    hda.initialized = true;
    serial_puts("[HDA] Audio initialized OK\n");
    return 0;
}

void hda_play_buffer(const int16_t *samples, uint32_t num_samples)
{
    if (!hda.initialized) return;

    uint32_t bps = (hda.bits_per_sample + 7) / 8;  /* bytes per sample */
    uint32_t bytes = num_samples * bps * 2;         /* stereo */
    if (bytes > hda.buf_size) bytes = hda.buf_size;

    /* Copy samples to DMA buffer */
    memcpy(hda.audio_buf, samples, bytes);
    if (bytes < hda.buf_size)
        memset((uint8_t *)hda.audio_buf + bytes, 0, hda.buf_size - bytes);

    wmb();

    /* Update BDL and CBL for actual size */
    uint32_t sd = hda.out_stream_off;
    hda.bdl[0].length = bytes;
    hda_write32(sd + HDA_SD_CBL, bytes);

    /* Stop stream before reconfiguring */
    uint32_t ctl = hda_read32(sd + HDA_SD_CTL) & 0x00FFFFFF;
    ctl &= ~HDA_SD_CTL_RUN;
    hda_write32(sd + HDA_SD_CTL, ctl);
    spin_delay(200);

    /* Clear status */
    hda_write8(sd + HDA_SD_STS,
               HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE);

    /* Set stream tag */
    ctl = (uint32_t)hda.stream_id << 20;
    hda_write32(sd + HDA_SD_CTL, ctl);

    /* Re-set format (some controllers need this after stop) */
    hda_write16(sd + HDA_SD_FMT, hda.fmt_reg);

    /* Configure codec output pipeline */
    hda_setup_output();

    /* Start stream */
    ctl = hda_read32(sd + HDA_SD_CTL) & 0x00FFFFFF;
    ctl |= HDA_SD_CTL_RUN;
    hda_write32(sd + HDA_SD_CTL, ctl);

    hda.playing = true;
}

void hda_play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!hda.initialized) return;

    uint32_t bps = (hda.bits_per_sample + 7) / 8;
    uint32_t frame_size = bps * 2;  /* stereo frame */
    uint32_t num_samples = hda.sample_rate * duration_ms / 1000;
    uint32_t max_samples = hda.buf_size / frame_size;
    if (num_samples > max_samples)
        num_samples = max_samples;

    /* Generate sine wave via phase accumulator */
    uint32_t phase = 0;
    uint32_t phase_inc = (freq_hz * 256) / hda.sample_rate;
    uint8_t *buf = (uint8_t *)hda.audio_buf;

    for (uint32_t i = 0; i < num_samples; i++) {
        int32_t val = sine_table[phase & 0xFF];
        val >>= 1;  /* 50% volume */

        if (hda.bits_per_sample <= 16) {
            int16_t s = (int16_t)val;
            int16_t *p = (int16_t *)(buf + i * frame_size);
            p[0] = s;  /* Left */
            p[1] = s;  /* Right */
        } else {
            /* Scale 16-bit sine to 32-bit range */
            int32_t s32 = val << 16;
            int32_t *p = (int32_t *)(buf + i * frame_size);
            p[0] = s32;  /* Left */
            p[1] = s32;  /* Right */
        }
        phase += phase_inc;
    }

    serial_puts("[HDA] Tone: ");
    serial_putdec(freq_hz);
    serial_puts(" Hz, ");
    serial_putdec(duration_ms);
    serial_puts(" ms @ ");
    serial_putdec(hda.sample_rate / 1000);
    serial_puts("kHz/");
    serial_putdec(hda.bits_per_sample);
    serial_puts("bit\n");

    hda_play_buffer(hda.audio_buf, num_samples);
}

void hda_stop(void)
{
    if (!hda.initialized) return;

    uint32_t sd = hda.out_stream_off;
    uint32_t ctl = hda_read32(sd + HDA_SD_CTL) & 0x00FFFFFF;
    ctl &= ~HDA_SD_CTL_RUN;
    hda_write32(sd + HDA_SD_CTL, ctl);
    hda.playing = false;
}

bool hda_is_ready(void)
{
    return hda.initialized;
}
