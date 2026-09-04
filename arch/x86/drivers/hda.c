/*
 * OsitoK x86-64 — Intel HDA (High Definition Audio) Driver
 *
 * Supports ICH6/ICH9 (QEMU), AMD Starship/Matisse, NVIDIA GA102.
 * Controller init, codec discovery, CORB/RIRB command interface,
 * output stream setup, and PCM playback via polling.
 */

#include "hda.h"
#include "paging.h"

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

#define HDA_MAX_CODEC_NODES 128U
#define HDA_MAX_ROUTE_NODES 16U

typedef struct {
    uint8_t nid;
    uint8_t connection_index;
    uint8_t connection_count;
} hda_route_step_t;

typedef struct {
    volatile void *bar0;

    /* CORB/RIRB */
    uint64_t  corb_phys;
    uint32_t *corb;
    uint64_t  rirb_phys;
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
    uint16_t  widget_start;
    uint16_t  widget_count;
    uint32_t  widget_caps[HDA_MAX_CODEC_NODES];
    uint8_t   widget_valid[HDA_MAX_CODEC_NODES];
    uint32_t  pin_caps;
    uint32_t  pin_config;
    hda_route_step_t route[HDA_MAX_ROUTE_NODES];
    uint8_t   route_len;

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
    uint64_t  bdl_phys;
    hda_bdl_entry_t *bdl;
    uint64_t  audio_buf_phys;
    int16_t  *audio_buf;
    uint32_t  buf_size;
    uint32_t  block_bytes;
    uint32_t  stream_faults;
    uint32_t  stream_start_failures;

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

static uint32_t hda_stream_ctl_read(uint32_t stream)
{
    return hda_read16(stream + HDA_SD_CTL) |
           ((uint32_t)hda_read8(stream + HDA_SD_CTL + 2U) << 16);
}

static void hda_stream_ctl_write(uint32_t stream, uint32_t value)
{
    hda_write16(stream + HDA_SD_CTL, (uint16_t)value);
    hda_write8(stream + HDA_SD_CTL + 2U, (uint8_t)(value >> 16));
}

static bool hda_log_event(uint32_t count)
{
    return count <= 4U || (count & (count - 1U)) == 0;
}

static void hda_report_stream_fault(uint8_t status, uint32_t control,
                                    uint32_t position)
{
    uint32_t count = ++hda.stream_faults;
    if (!hda_log_event(count))
        return;

    serial_puts("[HDA] output stream stalled; rearming (status=");
    serial_puthex(status, 2);
    serial_puts(" control=");
    serial_puthex(control, 6);
    serial_puts(" position=");
    serial_putdec(position);
    serial_puts(", fault=");
    serial_putdec(count);
    serial_puts(")\n");
}

static void hda_report_stream_start_failure(uint8_t status,
                                            uint32_t control)
{
    uint32_t count = ++hda.stream_start_failures;
    if (!hda_log_event(count))
        return;

    serial_puts("[HDA] output stream failed to start (status=");
    serial_puthex(status, 2);
    serial_puts(" control=");
    serial_puthex(control, 6);
    serial_puts(", failure=");
    serial_putdec(count);
    serial_puts("); retrying on next block\n");
}

static inline void spin_delay(uint32_t iters) {
    for (volatile uint32_t i = 0; i < iters; i++)
        __asm__ volatile ("pause");
}

static int hda_stream_reset(uint32_t stream)
{
    uint32_t ctl = hda_stream_ctl_read(stream) & ~HDA_SD_CTL_RUN;
    hda_stream_ctl_write(stream, ctl);
    for (uint32_t i = 0; i < 10000U; i++) {
        if (!(hda_stream_ctl_read(stream) & HDA_SD_CTL_RUN))
            break;
        spin_delay(100);
    }
    if (hda_stream_ctl_read(stream) & HDA_SD_CTL_RUN) {
        serial_puts("[HDA] Stream failed to stop\n");
        return -1;
    }

    hda_stream_ctl_write(stream, ctl | HDA_SD_CTL_SRST);
    for (uint32_t i = 0; i < 10000U; i++) {
        if (hda_stream_ctl_read(stream) & HDA_SD_CTL_SRST)
            break;
        spin_delay(100);
    }
    if (!(hda_stream_ctl_read(stream) & HDA_SD_CTL_SRST)) {
        serial_puts("[HDA] Stream reset assertion timed out\n");
        return -1;
    }

    hda_stream_ctl_write(stream, ctl & ~HDA_SD_CTL_SRST);
    for (uint32_t i = 0; i < 10000U; i++) {
        if (!(hda_stream_ctl_read(stream) & HDA_SD_CTL_SRST))
            return 0;
        spin_delay(100);
    }
    serial_puts("[HDA] Stream reset deassertion timed out\n");
    return -1;
}

/* ── CORB/RIRB Init ───────────────────────────────────────────── */

static int hda_corb_rirb_init(void)
{
    /* Allocate CORB: 256 entries x 4 bytes = 1KB */
    hda.corb_phys = (uint64_t)(uintptr_t)mem_alloc_aligned(
        HDA_CORB_ENTRIES * 4, 4096);
    if (!hda.corb_phys) return -1;
    hda.corb = (uint32_t *)PHYS_TO_VIRT(hda.corb_phys);
    memset(hda.corb, 0, HDA_CORB_ENTRIES * 4);

    /* Allocate RIRB: 256 entries x 8 bytes = 2KB */
    hda.rirb_phys = (uint64_t)(uintptr_t)mem_alloc_aligned(
        HDA_RIRB_ENTRIES * 8, 4096);
    if (!hda.rirb_phys) return -1;
    hda.rirb = (uint64_t *)PHYS_TO_VIRT(hda.rirb_phys);
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
    hda_write32(HDA_CORBLBASE, (uint32_t)hda.corb_phys);
    hda_write32(HDA_CORBUBASE, (uint32_t)(hda.corb_phys >> 32));

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
    hda_write32(HDA_RIRBLBASE, (uint32_t)hda.rirb_phys);
    hda_write32(HDA_RIRBUBASE, (uint32_t)(hda.rirb_phys >> 32));

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
static int hda_cmd_exec(uint8_t nid, uint32_t verb, uint32_t parm,
                        uint32_t *response)
{
    uint32_t cmd = ((uint32_t)hda.codec_addr << 28) |
                   ((uint32_t)nid << 20) |
                   ((verb & 0xFFF) << 8) |
                   (parm & 0xFFFF);
    return hda_send_verb(cmd, response);
}

static int hda_detect_format(void);

/* ── Codec Discovery ───────────────────────────────────────────── */

static int hda_get_subnodes(uint8_t nid, uint16_t *start_out,
                            uint16_t *count_out)
{
    uint32_t response;
    if (hda_cmd_exec(nid, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT,
                     &response) < 0)
        return -1;

    uint16_t start = (uint16_t)((response >> 16) & 0x7FFFU);
    uint16_t count = (uint16_t)(response & 0x7FFFU);
    if (!count || start >= HDA_MAX_CODEC_NODES ||
        count > HDA_MAX_CODEC_NODES ||
        (uint32_t)start + count > HDA_MAX_CODEC_NODES)
        return -1;

    *start_out = start;
    *count_out = count;
    return 0;
}

static bool hda_widget_is_valid(uint8_t nid)
{
    uint16_t value = nid;
    return value >= hda.widget_start &&
           value < hda.widget_start + hda.widget_count &&
           hda.widget_valid[nid] != 0;
}

static uint8_t hda_widget_type(uint8_t nid)
{
    return (uint8_t)((hda.widget_caps[nid] >> 20) & 0x0FU);
}

static int hda_read_format_caps(uint8_t nid, uint32_t *pcm_out,
                                uint32_t *formats_out)
{
    uint8_t caps_nid = nid;
    if (nid != hda.afg_nid &&
        (!(hda.widget_caps[nid] & HDA_WCAP_FORMAT_OVRD)))
        caps_nid = hda.afg_nid;

    uint32_t pcm;
    uint32_t formats;
    if (hda_cmd_exec(caps_nid, HDA_VERB_GET_PARAM, HDA_PARAM_PCM_RATES,
                     &pcm) < 0 ||
        hda_cmd_exec(caps_nid, HDA_VERB_GET_PARAM, HDA_PARAM_STREAM_FMTS,
                     &formats) < 0)
        return -1;

    *pcm_out = pcm;
    *formats_out = formats;
    return 0;
}

static bool hda_dac_supports_output(uint8_t nid)
{
    uint32_t pcm;
    uint32_t formats;
    uint32_t wcaps = hda.widget_caps[nid];

    if (hda_widget_type(nid) != HDA_WIDGET_AUD_OUT ||
        !(wcaps & HDA_WCAP_STEREO) || (wcaps & HDA_WCAP_DIGITAL) ||
        hda_read_format_caps(nid, &pcm, &formats) < 0)
        return false;

    return (formats & HDA_STREAM_FMT_PCM) &&
           (pcm & HDA_RATE_48KHZ) && (pcm & HDA_BITS_16);
}

static int hda_get_connections(uint8_t nid, uint8_t *connections,
                               uint32_t capacity)
{
    uint32_t list_info;
    if (!(hda.widget_caps[nid] & HDA_WCAP_CONN_LIST))
        return 0;
    if (hda_cmd_exec(nid, HDA_VERB_GET_PARAM, HDA_PARAM_CONN_LIST_LEN,
                     &list_info) < 0)
        return -1;

    uint32_t raw_count = HDA_CONN_LIST_LEN(list_info);
    if (!raw_count)
        return 0;

    bool long_form = (list_info & HDA_CONN_LIST_LONG) != 0;
    uint32_t shift = long_form ? 16U : 8U;
    uint32_t entries_per_response = long_form ? 2U : 4U;
    uint32_t value_mask = long_form ? 0x7FFFU : 0x7FU;
    uint32_t range_mask = long_form ? 0x8000U : 0x80U;
    uint32_t response = 0;
    uint32_t expanded_count = 0;
    uint16_t previous = 0;

    for (uint32_t raw_index = 0; raw_index < raw_count; raw_index++) {
        if ((raw_index % entries_per_response) == 0 &&
            hda_cmd_exec(nid, HDA_VERB_GET_CONN_LIST, raw_index,
                         &response) < 0)
            return -1;

        uint32_t entry = response & (value_mask | range_mask);
        response >>= shift;
        uint16_t value = (uint16_t)(entry & value_mask);
        bool range = raw_count > 1 && (entry & range_mask) != 0;

        if (!value || value >= HDA_MAX_CODEC_NODES)
            return -1;

        if (range) {
            if (!previous || previous >= value)
                return -1;
            for (uint16_t expanded = previous + 1; expanded <= value;
                 expanded++) {
                if (expanded_count >= capacity)
                    return -1;
                connections[expanded_count++] = (uint8_t)expanded;
            }
        } else {
            if (expanded_count >= capacity)
                return -1;
            connections[expanded_count++] = (uint8_t)value;
        }
        previous = value;
    }

    return (int)expanded_count;
}

static bool hda_find_route(uint8_t nid, uint8_t depth,
                           uint8_t visited[HDA_MAX_CODEC_NODES],
                           hda_route_step_t route[HDA_MAX_ROUTE_NODES],
                           uint8_t *route_len)
{
    if (depth >= HDA_MAX_ROUTE_NODES || !hda_widget_is_valid(nid) ||
        visited[nid])
        return false;

    visited[nid] = 1;
    uint8_t type = hda_widget_type(nid);
    route[depth].nid = nid;
    route[depth].connection_index = 0;
    route[depth].connection_count = 0;

    if (type == HDA_WIDGET_AUD_OUT) {
        if (hda_dac_supports_output(nid)) {
            *route_len = depth + 1;
            return true;
        }
        visited[nid] = 0;
        return false;
    }

    if (type == HDA_WIDGET_AUD_IN ||
        (type == HDA_WIDGET_PIN && depth != 0) ||
        !(hda.widget_caps[nid] & HDA_WCAP_CONN_LIST) ||
        depth + 1 >= HDA_MAX_ROUTE_NODES) {
        visited[nid] = 0;
        return false;
    }

    uint8_t connections[HDA_MAX_CODEC_NODES];
    int count = hda_get_connections(nid, connections,
                                    HDA_MAX_CODEC_NODES);
    if (count <= 0) {
        visited[nid] = 0;
        return false;
    }

    for (int index = 0; index < count; index++) {
        uint8_t next = connections[index];
        if (!hda_widget_is_valid(next) || visited[next])
            continue;
        if ((hda.widget_caps[nid] & HDA_WCAP_IN_AMP) && index > 0x0F)
            continue;

        route[depth].connection_index = (uint8_t)index;
        route[depth].connection_count = (uint8_t)count;
        if (hda_find_route(next, depth + 1, visited, route, route_len))
            return true;
    }

    visited[nid] = 0;
    return false;
}

static int hda_pin_score(uint32_t wcaps, uint32_t pin_caps,
                         uint32_t config)
{
    if (!(pin_caps & HDA_PINCAP_OUT) || (wcaps & HDA_WCAP_DIGITAL) ||
        HDA_DEFCFG_PORT(config) == HDA_DEFCFG_PORT_NONE)
        return -1;

    int score;
    switch (HDA_DEFCFG_DEVICE(config)) {
    case HDA_DEVICE_SPEAKER:
        score = 500;
        break;
    case HDA_DEVICE_LINE_OUT:
        score = 400;
        break;
    case HDA_DEVICE_HEADPHONE:
        score = 300;
        break;
    case HDA_DEVICE_SPDIF_OUT:
    case HDA_DEVICE_DIGITAL_OUT:
        return -1;
    default:
        score = 100;
        break;
    }

    /* Prefer fixed/internal outputs when no jack-sense policy exists yet. */
    if (HDA_DEFCFG_PORT(config) == 2)
        score += 40;
    else if (HDA_DEFCFG_PORT(config) == 3)
        score += 30;
    else
        score += 20;
    return score;
}

static int hda_select_output_route(void)
{
    int best_score = -1;
    uint8_t best_len = 0;

    for (uint16_t value = hda.widget_start;
         value < hda.widget_start + hda.widget_count; value++) {
        uint8_t nid = (uint8_t)value;
        if (!hda.widget_valid[nid] ||
            hda_widget_type(nid) != HDA_WIDGET_PIN)
            continue;

        uint32_t pin_caps;
        uint32_t config;
        if (hda_cmd_exec(nid, HDA_VERB_GET_PARAM, HDA_PARAM_PIN_CAP,
                         &pin_caps) < 0 ||
            hda_cmd_exec(nid, HDA_VERB_GET_CONFIG, 0, &config) < 0)
            continue;

        int score = hda_pin_score(hda.widget_caps[nid], pin_caps, config);
        if (score < 0)
            continue;

        uint8_t visited[HDA_MAX_CODEC_NODES];
        hda_route_step_t candidate[HDA_MAX_ROUTE_NODES];
        uint8_t candidate_len = 0;
        memset(visited, 0, sizeof(visited));
        memset(candidate, 0, sizeof(candidate));
        if (!hda_find_route(nid, 0, visited, candidate, &candidate_len))
            continue;

        if (score > best_score ||
            (score == best_score && candidate_len < best_len)) {
            best_score = score;
            best_len = candidate_len;
            hda.pin_nid = nid;
            hda.dac_nid = candidate[candidate_len - 1].nid;
            hda.pin_caps = pin_caps;
            hda.pin_config = config;
            hda.route_len = candidate_len;
            memcpy(hda.route, candidate,
                   candidate_len * sizeof(candidate[0]));
        }
    }

    if (best_score < 0) {
        serial_puts("[HDA] No connected analog PCM output route\n");
        return -1;
    }

    serial_puts("[HDA] Output route: ");
    for (uint8_t index = hda.route_len; index > 0; index--) {
        serial_puts("NID ");
        serial_putdec(hda.route[index - 1].nid);
        if (index > 1)
            serial_puts(" -> ");
    }
    serial_puts(" (DAC to pin)\n");
    return 0;
}

static int hda_codec_init(void)
{
    /* Get codec vendor/device ID from root node (NID 0) */
    uint32_t vendor;
    if (hda_cmd_exec(0x00, HDA_VERB_GET_PARAM, HDA_PARAM_VENDOR_ID,
                     &vendor) < 0) {
        serial_puts("[HDA] Failed to read codec identity\n");
        return -1;
    }
    hda.codec_vendor = (vendor >> 16) & 0xFFFF;
    hda.codec_device = vendor & 0xFFFF;

    serial_puts("[HDA] Codec: ");
    serial_puthex(hda.codec_vendor, 4);
    serial_puts(":");
    serial_puthex(hda.codec_device, 4);
    serial_puts("\n");

    /* Get subordinate node count from root */
    uint16_t start_nid;
    uint16_t num_nodes;
    if (hda_get_subnodes(0x00, &start_nid, &num_nodes) < 0) {
        serial_puts("[HDA] Invalid root node range\n");
        return -1;
    }

    /* Find Audio Function Group (type 0x01) */
    for (uint16_t i = 0; i < num_nodes; i++) {
        uint8_t nid = (uint8_t)(start_nid + i);
        uint32_t fgt;
        if (hda_cmd_exec(nid, HDA_VERB_GET_PARAM,
                         HDA_PARAM_FUNC_GRP_TYPE, &fgt) < 0)
            continue;
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
    if (hda_cmd_exec(hda.afg_nid, HDA_VERB_SET_POWER, 0x00, NULL) < 0) {
        serial_puts("[HDA] Failed to power audio function group\n");
        return -1;
    }

    /* Enumerate AFG widgets */
    if (hda_get_subnodes(hda.afg_nid, &start_nid, &num_nodes) < 0) {
        serial_puts("[HDA] Invalid widget node range\n");
        return -1;
    }
    hda.widget_start = start_nid;
    hda.widget_count = num_nodes;

    serial_puts("[HDA] Widgets: ");
    serial_putdec(num_nodes);
    serial_puts(" (NID ");
    serial_putdec(start_nid);
    serial_puts("-");
    serial_putdec(start_nid + num_nodes - 1);
    serial_puts(")\n");

    for (uint16_t i = 0; i < num_nodes; i++) {
        uint8_t nid = (uint8_t)(start_nid + i);
        uint32_t wcaps;
        if (hda_cmd_exec(nid, HDA_VERB_GET_PARAM,
                         HDA_PARAM_AUDIO_WIDGET, &wcaps) < 0)
            continue;
        hda.widget_caps[nid] = wcaps;
        hda.widget_valid[nid] = 1;
    }

    if (hda_select_output_route() < 0)
        return -1;

    if (hda_detect_format() < 0)
        return -1;

    return 0;
}

/* ── Format Auto-Detection ─────────────────────────────────────── */

static int hda_detect_format(void)
{
    uint32_t pcm;
    uint32_t formats;
    if (hda_read_format_caps(hda.dac_nid, &pcm, &formats) < 0) {
        serial_puts("[HDA] Failed to read DAC format capabilities\n");
        return -1;
    }

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

    /* The public API consumes interleaved signed PCM16 frames. Selecting a
     * wider or faster codec format here would over-read every caller buffer. */
    if (!(formats & HDA_STREAM_FMT_PCM) ||
        !(pcm & HDA_RATE_48KHZ) || !(pcm & HDA_BITS_16)) {
        serial_puts("[HDA] Codec lacks required 48kHz/16-bit PCM output\n");
        return -1;
    }

    hda.sample_rate = HDA_SAMPLE_RATE;
    hda.bits_per_sample = 16;
    hda.fmt_reg = HDA_FMT_DEFAULT;

    serial_puts("[HDA] Selected: ");
    serial_putdec(hda.sample_rate / 1000);
    serial_puts("kHz/");
    serial_putdec(hda.bits_per_sample);
    serial_puts("bit stereo (FMT=0x");
    serial_puthex(hda.fmt_reg, 4);
    serial_puts(")\n");
    return 0;
}

/* ── Stream Setup ──────────────────────────────────────────────── */

static int hda_stream_init(void)
{
    uint32_t sd = hda.out_stream_off;

    if (hda_stream_reset(sd) < 0)
        return -1;

    /* Clear status bits */
    hda_write8(sd + HDA_SD_STS,
               HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE);

    /* Two one-second-capacity blocks allow the producer to refill the block
     * that HDA is not currently consuming. Normal streaming uses much smaller
     * blocks (typically 10 ms). */
    hda.buf_size = HDA_BUF_SIZE * HDA_BDL_ENTRIES;
    hda.audio_buf_phys = (uint64_t)(uintptr_t)mem_alloc_aligned(
        hda.buf_size, 4096);
    if (!hda.audio_buf_phys) {
        serial_puts("[HDA] Failed to allocate audio buffer\n");
        return -1;
    }
    hda.audio_buf = (int16_t *)PHYS_TO_VIRT(hda.audio_buf_phys);
    memset(hda.audio_buf, 0, hda.buf_size);

    /* Allocate BDL (128-byte aligned per spec) */
    hda.bdl_phys = (uint64_t)(uintptr_t)mem_alloc_aligned(
        HDA_BDL_ENTRIES * sizeof(hda_bdl_entry_t), 128);
    if (!hda.bdl_phys) {
        serial_puts("[HDA] Failed to allocate BDL\n");
        return -1;
    }
    hda.bdl = (hda_bdl_entry_t *)PHYS_TO_VIRT(hda.bdl_phys);
    memset(hda.bdl, 0, HDA_BDL_ENTRIES * sizeof(hda_bdl_entry_t));

    uint32_t initial_block = hda.buf_size / HDA_BDL_ENTRIES;
    for (uint32_t i = 0; i < HDA_BDL_ENTRIES; i++) {
        hda.bdl[i].addr = hda.audio_buf_phys +
                          (uint64_t)i * initial_block;
        hda.bdl[i].length = initial_block;
        hda.bdl[i].ioc = 0;
    }

    /* Configure stream descriptor */
    uint32_t ctl = (uint32_t)hda.stream_id << 20;
    hda_stream_ctl_write(sd, ctl);

    hda_write32(sd + HDA_SD_CBL, hda.buf_size);
    hda_write16(sd + HDA_SD_LVI, HDA_BDL_ENTRIES - 1);
    hda_write16(sd + HDA_SD_FMT, hda.fmt_reg);

    hda_write32(sd + HDA_SD_BDLPL, (uint32_t)hda.bdl_phys);
    hda_write32(sd + HDA_SD_BDLPU, (uint32_t)(hda.bdl_phys >> 32));

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

static int hda_unmute_amp(uint8_t nid, bool input, uint8_t index)
{
    uint32_t wcaps = hda.widget_caps[nid];
    uint32_t required_cap = input ? HDA_WCAP_IN_AMP : HDA_WCAP_OUT_AMP;
    if (!(wcaps & required_cap))
        return 0;

    uint8_t cap_nid = (wcaps & HDA_WCAP_AMP_OVRD) ? nid : hda.afg_nid;
    uint32_t amp_caps;
    if (hda_cmd_exec(cap_nid, HDA_VERB_GET_PARAM,
                     input ? HDA_PARAM_AMP_IN_CAP : HDA_PARAM_AMP_OUT_CAP,
                     &amp_caps) < 0)
        return -1;

    uint8_t zero_db = (uint8_t)(amp_caps & 0x7FU);
    uint8_t steps = (uint8_t)((amp_caps >> 8) & 0x7FU);
    if (zero_db > steps)
        zero_db = steps;

    uint32_t payload = HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
                       HDA_AMP_GAIN(zero_db);
    if (input)
        payload |= HDA_AMP_SET_IN | HDA_AMP_INDEX(index);
    else
        payload |= HDA_AMP_SET_OUT;

    return hda_cmd_exec(nid, HDA_VERB_SET_AMP, payload, NULL);
}

static int hda_setup_output(void)
{
    /* Power the selected path from converter to pin. */
    for (uint8_t index = hda.route_len; index > 0; index--) {
        uint8_t nid = hda.route[index - 1].nid;
        if ((hda.widget_caps[nid] & HDA_WCAP_POWER) &&
            hda_cmd_exec(nid, HDA_VERB_SET_POWER, 0x00, NULL) < 0)
            return -1;
    }
    spin_delay(1000);

    /* Program each downstream widget to consume the next route node. */
    for (uint8_t index = 0; index + 1 < hda.route_len; index++) {
        hda_route_step_t *step = &hda.route[index];
        if (step->connection_count > 1 &&
            hda_widget_type(step->nid) != HDA_WIDGET_AUD_MIX &&
            hda_cmd_exec(step->nid, HDA_VERB_SET_CONN_SEL,
                         step->connection_index, NULL) < 0)
            return -1;
        if (hda_unmute_amp(step->nid, true,
                           step->connection_index) < 0)
            return -1;
    }

    /* Configure DAC converter: stream ID + channel 0 */
    if (hda_cmd_exec(hda.dac_nid, HDA_VERB_SET_CONV_CTRL,
                     hda.stream_id << 4, NULL) < 0)
        return -1;

    /* Set DAC format to match stream */
    if (hda_cmd_exec(hda.dac_nid, HDA_VERB_SET_STREAM_FMT,
                     hda.fmt_reg, NULL) < 0)
        return -1;

    /* Enable output on pin */
    uint32_t pin_control = HDA_PIN_OUT_EN;
    if (HDA_DEFCFG_DEVICE(hda.pin_config) == HDA_DEVICE_HEADPHONE &&
        (hda.pin_caps & HDA_PINCAP_HP_DRV))
        pin_control |= HDA_PIN_HP_EN;
    if (hda_cmd_exec(hda.pin_nid, HDA_VERB_SET_PIN_CTRL,
                     pin_control, NULL) < 0)
        return -1;

    for (uint8_t index = hda.route_len; index > 0; index--) {
        if (hda_unmute_amp(hda.route[index - 1].nid, false, 0) < 0)
            return -1;
    }

    /* Enable EAPD (external amplifier) if supported */
    if ((hda.pin_caps & HDA_PINCAP_EAPD) &&
        hda_cmd_exec(hda.pin_nid, HDA_VERB_SET_EAPD,
                     HDA_EAPD_ENABLE, NULL) < 0)
        return -1;

    serial_puts("[HDA] Output route configured\n");
    return 0;
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
    hda.bar0 = (volatile void *)PHYS_TO_VIRT(bar0_phys);

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
    if (hda_setup_output() < 0) {
        serial_puts("[HDA] Failed to configure codec output route\n");
        return -1;
    }

    hda.initialized = true;
    serial_puts("[HDA] Audio initialized OK\n");
    return 0;
}

void hda_play_buffer(const int16_t *samples, uint32_t num_frames)
{
    if (!hda.initialized || !samples || !num_frames) return;

    uint32_t sd = hda.out_stream_off;
    uint32_t max_block = hda.buf_size / HDA_BDL_ENTRIES;
    uint32_t max_frames = max_block / (sizeof(int16_t) * 2);
    if (num_frames > max_frames)
        num_frames = max_frames;
    uint32_t bytes = num_frames * sizeof(int16_t) * 2;

    if (hda.playing) {
        uint8_t status = hda_read8(sd + HDA_SD_STS);
        uint32_t control = hda_stream_ctl_read(sd);
        uint32_t position = hda_read32(sd + HDA_SD_LPIB);
        uint8_t errors = status & (HDA_SD_STS_FIFOE | HDA_SD_STS_DESE);

        /* Both stream errors stop DMA. RUN can also disappear without a
         * latched status bit after controller-level recovery. Never keep
         * feeding a software-only notion of a running stream. */
        if (errors || !(control & HDA_SD_CTL_RUN)) {
            hda_report_stream_fault(status, control, position);
            if (status & (HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE |
                          HDA_SD_STS_DESE)) {
                hda_write8(sd + HDA_SD_STS,
                           status & (HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE |
                                     HDA_SD_STS_DESE));
            }
            hda.playing = false;
            hda.block_bytes = 0;
        }
    }

    if (hda.playing && hda.block_bytes == bytes) {
        uint32_t position = hda_read32(sd + HDA_SD_LPIB);
        uint32_t current_slot = position >= bytes ? 1U : 0U;
        uint32_t target_slot = current_slot ^ 1U;
        memcpy((uint8_t *)hda.audio_buf + target_slot * bytes,
               samples, bytes);
        wmb();
        return;
    }

    uint32_t ctl = hda_stream_ctl_read(sd);
    ctl &= ~HDA_SD_CTL_RUN;
    hda_stream_ctl_write(sd, ctl);
    spin_delay(200);

    memcpy(hda.audio_buf, samples, bytes);
    memcpy((uint8_t *)hda.audio_buf + bytes, samples, bytes);
    wmb();

    for (uint32_t i = 0; i < HDA_BDL_ENTRIES; i++) {
        hda.bdl[i].addr = hda.audio_buf_phys + (uint64_t)i * bytes;
        hda.bdl[i].length = bytes;
        hda.bdl[i].ioc = 0;
    }
    hda.block_bytes = bytes;
    hda_write32(sd + HDA_SD_CBL, bytes * HDA_BDL_ENTRIES);
    hda_write16(sd + HDA_SD_LVI, HDA_BDL_ENTRIES - 1);

    /* Clear status */
    hda_write8(sd + HDA_SD_STS,
               HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE);

    /* Set stream tag */
    ctl = (uint32_t)hda.stream_id << 20;
    hda_stream_ctl_write(sd, ctl);

    /* Re-set format (some controllers need this after stop) */
    hda_write16(sd + HDA_SD_FMT, hda.fmt_reg);

    /* Start stream */
    ctl = hda_stream_ctl_read(sd);
    ctl |= HDA_SD_CTL_RUN;
    hda_stream_ctl_write(sd, ctl);

    ctl = hda_stream_ctl_read(sd);
    if (!(ctl & HDA_SD_CTL_RUN)) {
        hda.playing = false;
        hda.block_bytes = 0;
        hda_report_stream_start_failure(hda_read8(sd + HDA_SD_STS), ctl);
        return;
    }

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
    uint32_t ctl = hda_stream_ctl_read(sd);
    ctl &= ~HDA_SD_CTL_RUN;
    hda_stream_ctl_write(sd, ctl);
    hda.playing = false;
    hda.block_bytes = 0;
}

bool hda_is_ready(void)
{
    return hda.initialized;
}
