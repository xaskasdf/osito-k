/*
 * OsitoK x86-64 — USB Mass Storage Driver (Bulk-Only Transport)
 *
 * Reads sectors from USB flash drives via xHCI bulk transfers.
 * Implements BBB (Bulk-Only Transport) protocol:
 *   CBW (Command Block Wrapper) → Data → CSW (Command Status Wrapper)
 * SCSI commands: READ/WRITE(10), INQUIRY, READ CAPACITY, MODE SENSE,
 * REQUEST SENSE and SYNCHRONIZE CACHE(10).
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);

/* xHCI bulk transfer (from xhci.c) */
extern int xhci_bulk_out(int dev_idx, const void *data, uint32_t len)
    __attribute__((weak));
extern int xhci_bulk_in(int dev_idx, void *data, uint32_t len, uint32_t *actual)
    __attribute__((weak));
extern int xhci_bulk_clear_halt(int dev_idx, bool data_in)
    __attribute__((weak));

/* ── USB Mass Storage BBB Protocol ───────────────────────────── */

#define CBW_SIGNATURE   0x43425355  /* "USBC" */
#define CSW_SIGNATURE   0x53425355  /* "USBS" */

typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;        /* 0x80 = Data-In (device → host) */
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;      /* SCSI command length (6-16) */
    uint8_t  CBWCB[16];         /* SCSI CDB */
} cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;        /* 0=OK, 1=Failed, 2=Phase Error */
} csw_t;

/* SCSI Commands */
#define SCSI_INQUIRY              0x12
#define SCSI_READ_CAPACITY        0x25
#define SCSI_READ_10              0x28
#define SCSI_WRITE_10             0x2A
#define SCSI_SYNCHRONIZE_CACHE_10 0x35
#define USB_WRITE_DIAG_RECENT     8
#define USB_BULK_STALL           (-6) /* xHCI Stall Error completion */

typedef struct {
    bool valid;
    uint8_t response, key, asc, ascq;
} usb_scsi_sense_t;

enum { USB_CACHE_UNKNOWN, USB_CACHE_THROUGH, USB_CACHE_BACK };

/* ── Driver State ────────────────────────────────────────────── */

static struct {
    bool     ready;
    int      dev_idx;        /* xHCI device index */
    uint32_t block_count;    /* Total LBAs */
    uint32_t block_size;     /* Bytes per block (usually 512) */
    uint32_t cbw_tag;        /* Incrementing tag for CBW/CSW matching */
    char     vendor[9];
    char     product[17];
    uint8_t  cache_mode;
    bool     fua;
    bool     write_protected;
    bool     needs_flush;
    bool     sync_unsupported;
    bool     transport_failed;
    bool     short_residue_warned;
} usb_disk;

static volatile uint32_t usb_io_lock;

static uint64_t usb_perf_clock(void)
{
#ifdef USB_STORAGE_HOST_TEST
    extern uint64_t usb_storage_test_clock(void);
    return usb_storage_test_clock();
#else
    uint32_t lo, hi;
    __asm__ volatile ("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
#endif
}

static uint64_t usb_io_lock_irqsave_timed(uint64_t *started)
{
    uint64_t flags;
#ifdef USB_STORAGE_HOST_TEST
    extern uint64_t usb_storage_test_irq_save(void);
    flags = usb_storage_test_irq_save();
#else
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    if (started) *started = usb_perf_clock();
    /* BOT has one command/tag stream, not one independent stream per endpoint.
     * Keep CBW, data, CSW, REQUEST SENSE and cache bookkeeping under one owner.
     * IRQ-save also prevents a local task switch while that owner is polling. */
    while (__atomic_exchange_n(&usb_io_lock, 1U, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
    return flags;
}

static uint64_t usb_io_lock_irqsave(void)
{
    return usb_io_lock_irqsave_timed(NULL);
}

static void usb_io_unlock_irqrestore(uint64_t flags)
{
    __atomic_store_n(&usb_io_lock, 0U, __ATOMIC_RELEASE);
#ifdef USB_STORAGE_HOST_TEST
    extern void usb_storage_test_irq_restore(uint64_t);
    usb_storage_test_irq_restore(flags);
#else
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
#endif
    extern void sched_io_checkpoint(void) __attribute__((weak));
    if ((flags & (1ULL << 9)) && sched_io_checkpoint)
        sched_io_checkpoint();
}

enum { USB_PERF_INIT, USB_PERF_READ, USB_PERF_WRITE, USB_PERF_FLUSH, USB_PERF_OPS };
enum { USB_PERF_CBW, USB_PERF_DATA, USB_PERF_CSW, USB_PERF_PHASES };
#define USB_PERF_SIZES 7

typedef struct {
    uint64_t calls, failures, requested_bytes, ok_bytes;
    uint64_t wait_ticks, hold_ticks, max_hold_ticks, entry_irq_off, bad_clock;
} usb_perf_op_t;

typedef struct {
    uint64_t calls, failures, requested_bytes, ok_bytes, ticks, max_ticks, bad_clock;
} usb_perf_phase_t;

typedef struct {
    usb_perf_op_t op[USB_PERF_OPS];
    usb_perf_phase_t phase[USB_PERF_PHASES];
    uint64_t read_sizes[USB_PERF_SIZES], write_sizes[USB_PERF_SIZES];
} usb_perf_t;

static usb_perf_t usb_perf;

/* All counters are protected by the BOT owner. TSC, not the masked timer IRQ,
 * measures the wait. Phase times are included in op hold times, not additive. */
static void usb_perf_record(unsigned op, uint64_t start, uint64_t acquired,
                            uint64_t end, uint64_t flags, uint64_t bytes, int result)
{
    usb_perf_op_t *s = &usb_perf.op[op];
    s->calls++;
    s->failures += result < 0;
    s->requested_bytes += bytes;
    if (result == 0) s->ok_bytes += bytes;
    s->entry_irq_off += !(flags & (1ULL << 9));
    if (acquired < start || end < acquired) s->bad_clock++;
    else {
        uint64_t hold = end - acquired;
        s->wait_ticks += acquired - start;
        s->hold_ticks += hold;
        if (hold > s->max_hold_ticks) s->max_hold_ticks = hold;
    }
    if (bytes && (op == USB_PERF_READ || op == USB_PERF_WRITE)) {
        static const uint32_t bounds[] = {512, 4096, 8192, 16384, 32768, 65536};
        unsigned bin = 0;
        while (bin < USB_PERF_SIZES - 1 && bytes > bounds[bin]) bin++;
        (op == USB_PERF_READ ? usb_perf.read_sizes : usb_perf.write_sizes)[bin]++;
    }
}

static int usb_perf_bulk(unsigned phase, void *data, uint32_t len, bool data_in,
                         uint32_t *actual)
{
    uint64_t start = usb_perf_clock();
    int result = data_in ? xhci_bulk_in(usb_disk.dev_idx, data, len, actual) :
                          xhci_bulk_out(usb_disk.dev_idx, data, len);
    uint64_t end = usb_perf_clock();
    usb_perf_phase_t *s = &usb_perf.phase[phase];
    s->calls++;
    s->failures += result < 0;
    s->requested_bytes += len;
    if (result == 0 && (!data_in || *actual <= len))
        s->ok_bytes += data_in ? *actual : len;
    if (end < start) s->bad_clock++;
    else {
        uint64_t elapsed = end - start;
        s->ticks += elapsed;
        if (elapsed > s->max_ticks) s->max_ticks = elapsed;
    }
    return result;
}

static void usb_perf_field(const char *name, uint64_t value)
{
    serial_puts(name);
    serial_putdec(value);
}

void usb_storage_perf_report(void)
{
    extern uint64_t idt_get_tsc_freq(void);
    usb_perf_t snapshot;
    uint64_t flags = usb_io_lock_irqsave();
    memcpy(&snapshot, &usb_perf, sizeof(snapshot));
    uint64_t now = usb_perf_clock();
    usb_io_unlock_irqrestore(flags);

    /* Output may block/yield over SSH; never print under the BOT owner. */
    usb_perf_field("[USB-PERF] cumulative tsc_hz=", idt_get_tsc_freq());
    usb_perf_field(" now=", now);
    serial_puts("\n");
    static const char *const ops[] = {"init", "read", "write", "flush"};
    for (unsigned i = 0; i < USB_PERF_OPS; i++) {
        const usb_perf_op_t *s = &snapshot.op[i];
        serial_puts("[USB-PERF] op="); serial_puts(ops[i]);
        usb_perf_field(" calls=", s->calls);
        usb_perf_field(" failures=", s->failures);
        usb_perf_field(" requested_bytes=", s->requested_bytes);
        usb_perf_field(" ok_bytes=", s->ok_bytes);
        usb_perf_field(" wait_ticks=", s->wait_ticks);
        usb_perf_field(" hold_ticks=", s->hold_ticks);
        usb_perf_field(" max_hold_ticks=", s->max_hold_ticks);
        usb_perf_field(" entry_irq_off=", s->entry_irq_off);
        usb_perf_field(" bad_clock=", s->bad_clock);
        serial_puts("\n");
    }
    static const char *const phases[] = {"cbw", "data", "csw"};
    for (unsigned i = 0; i < USB_PERF_PHASES; i++) {
        const usb_perf_phase_t *s = &snapshot.phase[i];
        serial_puts("[USB-PERF] phase="); serial_puts(phases[i]);
        usb_perf_field(" calls=", s->calls);
        usb_perf_field(" failures=", s->failures);
        usb_perf_field(" requested_bytes=", s->requested_bytes);
        usb_perf_field(" ok_bytes=", s->ok_bytes);
        usb_perf_field(" ticks=", s->ticks);
        usb_perf_field(" max_ticks=", s->max_ticks);
        usb_perf_field(" bad_clock=", s->bad_clock);
        serial_puts("\n");
    }
    for (unsigned dir = 0; dir < 2; dir++) {
        const uint64_t *sizes = dir ? snapshot.write_sizes : snapshot.read_sizes;
        serial_puts(dir ? "[USB-PERF] write_sizes=" : "[USB-PERF] read_sizes=");
        for (unsigned i = 0; i < USB_PERF_SIZES; i++) {
            if (i) serial_puts("/");
            serial_putdec(sizes[i]);
        }
        serial_puts(" bounds_bytes=512/4096/8192/16384/32768/65536/more\n");
    }
}

typedef struct {
    uint64_t lba;
    uint32_t count;
} usb_write_diag_entry_t;

static struct {
    uint64_t ops;
    uint64_t sectors;
    uint64_t first_lba;
    uint64_t last_lba;
    uint32_t first_count;
    uint32_t last_count;
    uint32_t recent_pos;
    uint32_t recent_count;
    usb_write_diag_entry_t recent[USB_WRITE_DIAG_RECENT];
} usb_write_diag;

static void usb_write_diag_record(uint64_t lba, uint32_t count)
{
    if (usb_write_diag.ops == 0) {
        usb_write_diag.first_lba = lba;
        usb_write_diag.first_count = count;
    }
    usb_write_diag.ops++;
    usb_write_diag.sectors += count;
    usb_write_diag.last_lba = lba;
    usb_write_diag.last_count = count;

    usb_write_diag.recent[usb_write_diag.recent_pos].lba = lba;
    usb_write_diag.recent[usb_write_diag.recent_pos].count = count;
    usb_write_diag.recent_pos =
        (usb_write_diag.recent_pos + 1) % USB_WRITE_DIAG_RECENT;
    if (usb_write_diag.recent_count < USB_WRITE_DIAG_RECENT)
        usb_write_diag.recent_count++;
}

static void usb_write_diag_reset(void)
{
    memset(&usb_write_diag, 0, sizeof(usb_write_diag));
}

static void usb_write_diag_print_summary(const char *reason)
{
    if (usb_write_diag.ops == 0)
        return;

    serial_puts("[USB-STOR] write summary");
    if (reason) {
        serial_puts(" reason=");
        serial_puts(reason);
    }
    serial_puts(" ops=");
    serial_putdec(usb_write_diag.ops);
    serial_puts(" sectors=");
    serial_putdec(usb_write_diag.sectors);
    serial_puts(" first=");
    serial_putdec(usb_write_diag.first_lba);
    serial_puts("+");
    serial_putdec(usb_write_diag.first_count);
    serial_puts(" last=");
    serial_putdec(usb_write_diag.last_lba);
    serial_puts("+");
    serial_putdec(usb_write_diag.last_count);
    serial_puts("\n");
}

void usb_storage_write_diag_flush(const char *reason)
{
    uint64_t flags = usb_io_lock_irqsave();
    usb_write_diag_print_summary(reason);
    usb_write_diag_reset();
    usb_io_unlock_irqrestore(flags);
}

static void usb_write_diag_print_recent(void)
{
    uint32_t n = usb_write_diag.recent_count;
    uint32_t start = (usb_write_diag.recent_pos + USB_WRITE_DIAG_RECENT - n) %
                     USB_WRITE_DIAG_RECENT;

    for (uint32_t i = 0; i < n; i++) {
        usb_write_diag_entry_t *e =
            &usb_write_diag.recent[(start + i) % USB_WRITE_DIAG_RECENT];
        serial_puts("[USB-STOR] recent write lba=");
        serial_putdec(e->lba);
        serial_puts(" count=");
        serial_putdec(e->count);
        serial_puts("\n");
    }
}

/* ── SCSI Command Helpers ────────────────────────────────────── */

/* Return 1 only for a validated command failure, not a transport failure.
 * The small DMA objects are aligned so they cannot cross a stack page. */
static int usb_bot_cmd(const uint8_t *cdb, uint8_t cdb_len,
                       void *data, uint32_t data_len, bool data_in,
                       uint32_t *actual_out)
{
    if (actual_out) *actual_out = 0;
    if (!xhci_bulk_out || !xhci_bulk_in || usb_disk.transport_failed ||
        !cdb || !cdb_len || cdb_len > 16 || (!data && data_len)) return -1;

    /* Build CBW */
    cbw_t cbw __attribute__((aligned(64)));
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = CBW_SIGNATURE;
    cbw.dCBWTag = ++usb_disk.cbw_tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = data_in ? 0x80 : 0x00;
    cbw.bCBWLUN = 0;
    cbw.bCBWCBLength = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);

    csw_t csw __attribute__((aligned(64)));
    memset(&csw, 0, sizeof(csw));
    uint32_t csw_actual = 0, received = data_len;
    bool stalled = false, got_csw = false;
    const char *stage = "cbw";
    int result = usb_perf_bulk(USB_PERF_CBW, &cbw, sizeof(cbw), false, NULL);
    if (result < 0) goto transport_error;

    /* Data phase (if any) */
    if (data_len > 0) {
        stage = "data";
        if (data_in) {
            received = 0;
            result = usb_perf_bulk(USB_PERF_DATA, data, data_len, true, &received);
        } else {
            result = usb_perf_bulk(USB_PERF_DATA, data, data_len, false, NULL);
        }
        if (result == USB_BULK_STALL && xhci_bulk_clear_halt &&
            xhci_bulk_clear_halt(usb_disk.dev_idx, data_in) == 0)
            stalled = true;
        else if (result < 0) goto transport_error;
        if (received > data_len) { stage = "data-length"; goto transport_error; }
        /* Some devices omit the data/ZLP before a failed command's CSW.
         * Recognize its envelope, then apply the same tag/status checks. */
        if (data_in && !stalled && received == sizeof(csw) && data_len > sizeof(csw)) {
            memcpy(&csw, data, sizeof(csw));
            if (csw.dCSWSignature == CSW_SIGNATURE) {
                got_csw = true;
                csw_actual = sizeof(csw);
                received = 0;
            } else memset(&csw, 0, sizeof(csw));
        }
    }

    /* Receive CSW */
    if (!got_csw) {
        bool skipped_zlp = false, cleared_stall = false;
        stage = "csw";
        for (unsigned attempt = 0; attempt < 3; attempt++) {
            csw_actual = 0;
            memset(&csw, 0, sizeof(csw));
            result = usb_perf_bulk(USB_PERF_CSW, &csw, sizeof(csw), true, &csw_actual);
            if (result == USB_BULK_STALL && !cleared_stall && xhci_bulk_clear_halt &&
                xhci_bulk_clear_halt(usb_disk.dev_idx, true) == 0) {
                cleared_stall = true;
                continue;
            }
            /* One surplus data terminator is not a malformed status wrapper. */
            if (result == 0 && csw_actual == 0 && !skipped_zlp) {
                skipped_zlp = true;
                continue;
            }
            break;
        }
        if (result < 0) goto transport_error;
        if (csw_actual != sizeof(csw)) { stage = "csw-length"; goto transport_error; }
    }

    if (csw.dCSWSignature != CSW_SIGNATURE || csw.dCSWTag != cbw.dCBWTag) {
        stage = "csw-identity";
        goto transport_error;
    }

    if (csw.bCSWStatus > 1 || csw.dCSWDataResidue > data_len) {
        stage = "csw-status/residue";
        goto transport_error;
    }

    if (csw.bCSWStatus != 0) {
        serial_puts("[USB-STOR] CSW status=");
        serial_putdec(csw.bCSWStatus);
        serial_puts(" residue=");
        serial_putdec(csw.dCSWDataResidue);
        serial_puts(" cdb[0]=0x");
        serial_puthex(cdb[0], 2);
        serial_puts("\n");

        return 1;
    }
    uint32_t relevant = data_len - csw.dCSWDataResidue;
    if (stalled && relevant != 0) { stage = "stall-status"; goto transport_error; }
    if (got_csw && relevant != 0) { stage = "missing-data"; goto transport_error; }
    /* USB's actual byte count is an upper bound, even when a device leaves
     * residue at zero for a short MODE SENSE response. Fixed-size SCSI calls
     * still reject any missing bytes in usb_scsi_cmd; never synthesize data. */
    if (data_in && relevant > received) {
        relevant = received;
        if (!usb_disk.short_residue_warned) {
            serial_puts("[USB-STOR] Short IN: CSW understates residue; using actual byte count\n");
            usb_disk.short_residue_warned = true;
        }
    }
    if (actual_out) *actual_out = relevant;
    return 0;

transport_error:
    /* Do not issue another CBW on an unsynchronized BOT stream. A complete
     * BOT reset/re-enumeration is needed for errors other than a clearable
     * data/CSW STALL; none of these may be classified as unsupported SCSI. */
    usb_disk.transport_failed = true;
    serial_puts("[USB-STOR] BOT cdb=0x"); serial_puthex(cdb[0], 2);
    serial_puts(" stage="); serial_puts(stage);
    serial_puts(" rc=");
    if (result < 0) { serial_puts("-"); serial_putdec(-(int64_t)result); }
    else serial_putdec(result);
    serial_puts(" want="); serial_putdec(data_len);
    serial_puts(" data="); serial_putdec(received);
    serial_puts(" csw="); serial_putdec(csw_actual); serial_puts("\n");
    serial_puts("[USB-STOR] CSW sig=0x"); serial_puthex(csw.dCSWSignature, 8);
    serial_puts(" tag="); serial_putdec(csw.dCSWTag);
    serial_puts(" expected="); serial_putdec(cbw.dCBWTag);
    serial_puts(" status="); serial_putdec(csw.bCSWStatus);
    serial_puts(" residue="); serial_putdec(csw.dCSWDataResidue); serial_puts("\n");
    serial_puts("[USB-STOR] BOT transport error; device needs reinitialization\n");
    return -1;
}

static int usb_scsi_cmd(const uint8_t *cdb, uint8_t cdb_len,
                        void *data, uint32_t data_len, bool data_in,
                        uint32_t *actual_out, usb_scsi_sense_t *sense_out)
{
    if (sense_out) memset(sense_out, 0, sizeof(*sense_out));
    uint32_t actual = 0;
    int result = usb_bot_cmd(cdb, cdb_len, data, data_len, data_in, &actual);
    if (actual_out) *actual_out = actual;
    if (result == 0) return actual_out || actual == data_len ? 0 : -1;
    if (result < 0) return -1;

    uint8_t cdb_sense[6] = {3, 0, 0, 0, 18, 0};
    uint8_t raw[18] __attribute__((aligned(32))) = {0};
    usb_scsi_sense_t sense = {0};
    if (usb_bot_cmd(cdb_sense, sizeof(cdb_sense), raw, sizeof(raw), true, &actual) == 0 &&
        actual >= 8) {
        sense.response = raw[0] & 0x7f;
        if ((sense.response == 0x70 || sense.response == 0x71) &&
            actual >= 14 && raw[7] >= 6) {
            sense.valid = true;
            sense.key = raw[2] & 15; sense.asc = raw[12]; sense.ascq = raw[13];
        } else if (sense.response == 0x72 || sense.response == 0x73) {
            sense.valid = true;
            sense.key = raw[1] & 15; sense.asc = raw[2]; sense.ascq = raw[3];
        }
    }
    if (sense.valid) {
        serial_puts("[USB-STOR] sense key=0x"); serial_puthex(sense.key, 1);
        serial_puts(" asc=0x"); serial_puthex(sense.asc, 2);
        serial_puts(" ascq=0x"); serial_puthex(sense.ascq, 2); serial_puts("\n");
    } else serial_puts("[USB-STOR] REQUEST SENSE unavailable or invalid\n");
    if (sense_out) *sense_out = sense;
    return -1;
}

static bool usb_scsi_unsupported(const usb_scsi_sense_t *sense)
{
    return sense->valid && (sense->response == 0x70 || sense->response == 0x72) &&
           sense->key == 5 && (sense->asc == 0x20 || sense->asc == 0x24) &&
           sense->ascq == 0;
}

static bool usb_parse_cache(const uint8_t *data, uint32_t actual, bool ten)
{
    uint32_t header = ten ? 8 : 4;
    if (actual < header) return false;
    uint32_t length = ten ? ((uint32_t)data[0] << 8 | data[1]) + 2 : data[0] + 1u;
    uint32_t descriptors = ten ? (uint32_t)data[6] << 8 | data[7] : data[3];
    if (length < header || descriptors > length - header) return false;
    if (length > actual) length = actual;
    uint8_t flags = data[ten ? 3 : 2];
    usb_disk.write_protected = !!(flags & 0x80);
    for (uint32_t at = header + descriptors; at + 2 <= length; ) {
        bool subpage = !!(data[at] & 0x40);
        uint32_t size;
        if (subpage) {
            if (length - at < 4) return false;
            size = 4 + ((uint32_t)data[at + 2] << 8 | data[at + 3]);
        } else size = 2 + data[at + 1];
        if (size > length - at) return false;
        if (!subpage && (data[at] & 0x3f) == 8 && size >= 20) {
            usb_disk.cache_mode = data[at + 2] & 4 ? USB_CACHE_BACK : USB_CACHE_THROUGH;
            usb_disk.fua = !!(flags & 0x10);
            return true;
        }
        at += size;
    }
    return false;
}

static void usb_storage_probe_cache(void)
{
    /* Query current values, with descriptors allowed. Some bridges support
     * only MODE SENSE(10), or expose page 08 only via the all-pages query. */
    uint8_t data[192] __attribute__((aligned(256)));
    for (unsigned page = 0; page < 2; page++) {
        for (unsigned ten = 0; ten < 2; ten++) {
            uint8_t cdb[10] = {ten ? 0x5a : 0x1a, 0, page ? 0x3f : 8};
            cdb[ten ? 8 : 4] = sizeof(data);
            uint32_t actual;
            usb_scsi_sense_t sense;
            memset(data, 0, sizeof(data));
            int result = usb_scsi_cmd(cdb, ten ? 10 : 6, data, sizeof(data), true,
                                      &actual, &sense);
            if (result == 0 && usb_parse_cache(data, actual, ten)) goto done;
            if (result < 0 && !usb_scsi_unsupported(&sense)) goto done;
        }
    }
done:
    serial_puts("[USB-STOR] write cache=");
    serial_puts(usb_disk.cache_mode == USB_CACHE_BACK ? "enabled" :
                usb_disk.cache_mode == USB_CACHE_THROUGH ? "disabled" : "unknown");
    serial_puts(usb_disk.fua ? " FUA=yes" : " FUA=no");
    serial_puts(usb_disk.write_protected ? " write-protected\n" : "\n");
}

/* ── Public API ──────────────────────────────────────────────── */

static int usb_storage_init_locked(int xhci_dev_idx)
{
    memset(&usb_disk, 0, sizeof(usb_disk));
    usb_write_diag_reset();
    usb_disk.dev_idx = xhci_dev_idx;

    /* INQUIRY — get device identity */
    uint8_t inq_cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t inq_data[36] __attribute__((aligned(64)));
    if (usb_scsi_cmd(inq_cdb, 6, inq_data, 36, true, NULL, NULL) < 0) {
        serial_puts("[USB-STOR] INQUIRY failed\n");
        return -1;
    }

    /* Extract vendor (8 bytes @ offset 8) and product (16 bytes @ offset 16) */
    memcpy(usb_disk.vendor, inq_data + 8, 8);
    usb_disk.vendor[8] = '\0';
    memcpy(usb_disk.product, inq_data + 16, 16);
    usb_disk.product[16] = '\0';

    /* READ CAPACITY — get disk size */
    uint8_t cap_cdb[10] = { SCSI_READ_CAPACITY, 0, 0,0,0,0, 0,0,0, 0 };
    uint8_t cap_data[8] __attribute__((aligned(8)));
    if (usb_scsi_cmd(cap_cdb, 10, cap_data, 8, true, NULL, NULL) < 0) {
        serial_puts("[USB-STOR] READ CAPACITY failed\n");
        return -1;
    }

    usb_disk.block_count = ((uint32_t)cap_data[0] << 24) | ((uint32_t)cap_data[1] << 16) |
                           ((uint32_t)cap_data[2] << 8) | cap_data[3];
    usb_disk.block_size  = ((uint32_t)cap_data[4] << 24) | ((uint32_t)cap_data[5] << 16) |
                           ((uint32_t)cap_data[6] << 8) | cap_data[7];
    if (usb_disk.block_count == UINT32_MAX || !usb_disk.block_size ||
        usb_disk.block_size > 8192 || (usb_disk.block_size & (usb_disk.block_size - 1))) {
        serial_puts("[USB-STOR] Unsupported READ CAPACITY geometry\n");
        return -1;
    }
    usb_disk.block_count++;  /* LBA is max LBA, count = max + 1 */
    usb_storage_probe_cache();
    if (usb_disk.transport_failed) return -1;

    usb_disk.ready = true;

    serial_puts("[USB-STOR] ");
    serial_puts(usb_disk.vendor);
    serial_puts(" ");
    serial_puts(usb_disk.product);
    serial_puts(": ");
    serial_putdec((uint64_t)usb_disk.block_count * usb_disk.block_size / (1024 * 1024));
    serial_puts(" MB (");
    serial_putdec(usb_disk.block_size);
    serial_puts(" bytes/sector)\n");
    return 0;
}

int usb_storage_init(int xhci_dev_idx)
{
    uint64_t start;
    uint64_t flags = usb_io_lock_irqsave_timed(&start);
    uint64_t acquired = usb_perf_clock();
    int result = usb_storage_init_locked(xhci_dev_idx);
    usb_perf_record(USB_PERF_INIT, start, acquired, usb_perf_clock(), flags, 0, result);
    usb_io_unlock_irqrestore(flags);
    return result;
}

static bool usb_storage_ready_locked(void) { return usb_disk.ready && !usb_disk.transport_failed; }
bool usb_storage_is_ready(void)
{
    uint64_t flags = usb_io_lock_irqsave();
    bool ready = usb_storage_ready_locked();
    usb_io_unlock_irqrestore(flags);
    return ready;
}
uint32_t usb_storage_block_size(void) { return usb_disk.block_size; }
uint32_t usb_storage_block_count(void) { return usb_disk.block_count; }

/* Read sectors from USB disk */
static int usb_storage_read_locked(uint64_t lba, uint32_t count, void *buf)
{
    if (!usb_storage_ready_locked() || (!buf && count) ||
        lba > usb_disk.block_count || count > usb_disk.block_count - lba) return -1;

    /* SCSI READ(10) — max 128 sectors per command */
    uint8_t *dst = (uint8_t *)buf;
    while (count > 0) {
        uint32_t chunk = (count > 128) ? 128 : count;
        uint32_t xfer = chunk * usb_disk.block_size;

        uint8_t cdb[10];
        memset(cdb, 0, 10);
        cdb[0] = SCSI_READ_10;
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)(lba);
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)(chunk);

        if (usb_scsi_cmd(cdb, 10, dst, xfer, true, NULL, NULL) < 0)
            return -1;

        dst += xfer;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

/* Write sectors to USB disk via SCSI WRITE(10). Same shape as READ(10)
 * but data goes Bulk-OUT (host→device). Without this the kernel can't
 * persist OsitoFS updates (crash reports, file creates, etc.) when the
 * mounted disk is the boot USB. */
static int usb_storage_write_locked(uint64_t lba, uint32_t count, const void *buf)
{
    if (!usb_storage_ready_locked()) {
        usb_write_diag_print_summary("not-ready");
        usb_write_diag_print_recent();
        usb_write_diag_reset();
        serial_puts("[USB-STOR] write: not ready\n");
        return -1;
    }
    if (usb_disk.write_protected || (!buf && count) ||
        lba > usb_disk.block_count || count > usb_disk.block_count - lba) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t chunk = (count > 128) ? 128 : count;
        uint32_t xfer  = chunk * usb_disk.block_size;

        uint8_t cdb[10];
        memset(cdb, 0, 10);
        cdb[0] = SCSI_WRITE_10;
        bool fua = usb_disk.cache_mode == USB_CACHE_BACK && usb_disk.fua;
        cdb[1] = fua ? 8 : 0;
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)(lba);
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)(chunk);

        usb_write_diag_record(lba, chunk);
        bool pending = usb_disk.needs_flush;
        if (usb_disk.cache_mode != USB_CACHE_THROUGH) usb_disk.needs_flush = true;
        usb_scsi_sense_t sense;
        int result = usb_scsi_cmd(cdb, 10, (void *)(uintptr_t)src, xfer, false, NULL, &sense);
        if (result < 0 && fua && usb_scsi_unsupported(&sense) && sense.asc == 0x24) {
            usb_disk.fua = false;
            fua = false;
            cdb[1] = 0;
            serial_puts("[USB-STOR] FUA rejected; retrying WRITE(10) without FUA\n");
            result = usb_scsi_cmd(cdb, 10, (void *)(uintptr_t)src, xfer, false, NULL, NULL);
        }
        if (result < 0) {
            usb_write_diag_print_summary("error");
            usb_write_diag_print_recent();
            usb_write_diag_reset();
            serial_puts("[USB-STOR] WRITE_10 SCSI failed lba=");
            serial_putdec(lba);
            serial_puts("\n");
            return -1;
        }
        if (fua) usb_disk.needs_flush = pending;

        src   += xfer;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

/* Flush pending cached writes. A specifically unsupported command degrades
 * to best-effort operation with a persistent per-device warning; it does
 * not turn media/transport errors into success or claim durable storage. */
static int usb_storage_flush_locked(void)
{
    if (!usb_storage_ready_locked()) return -1;
    if (usb_disk.cache_mode == USB_CACHE_THROUGH || !usb_disk.needs_flush ||
        usb_disk.sync_unsupported) return 0;
    uint8_t cdb[10] = { SCSI_SYNCHRONIZE_CACHE_10, 0, 0,0,0,0, 0,0,0, 0 };
    usb_scsi_sense_t sense;
    int result = usb_scsi_cmd(cdb, 10, NULL, 0, false, NULL, &sense);
    if (result == 0) usb_disk.needs_flush = false;
    else if (usb_scsi_unsupported(&sense)) {
        usb_disk.sync_unsupported = true;
        serial_puts("[USB-STOR] SYNCHRONIZE CACHE(10) unsupported; continuing without flush guarantee\n");
        return 0;
    }
    return result;
}

int usb_storage_read(uint64_t lba, uint32_t count, void *buf)
{
    uint64_t start;
    uint64_t flags = usb_io_lock_irqsave_timed(&start);
    uint64_t acquired = usb_perf_clock();
    int result = usb_storage_read_locked(lba, count, buf);
    usb_perf_record(USB_PERF_READ, start, acquired, usb_perf_clock(), flags,
                    (uint64_t)count * usb_disk.block_size, result);
    usb_io_unlock_irqrestore(flags);
    return result;
}

int usb_storage_write(uint64_t lba, uint32_t count, const void *buf)
{
    uint64_t start;
    uint64_t flags = usb_io_lock_irqsave_timed(&start);
    uint64_t acquired = usb_perf_clock();
    int result = usb_storage_write_locked(lba, count, buf);
    usb_perf_record(USB_PERF_WRITE, start, acquired, usb_perf_clock(), flags,
                    (uint64_t)count * usb_disk.block_size, result);
    usb_io_unlock_irqrestore(flags);
    return result;
}

int usb_storage_flush(void)
{
    uint64_t start;
    uint64_t flags = usb_io_lock_irqsave_timed(&start);
    uint64_t acquired = usb_perf_clock();
    int result = usb_storage_flush_locked();
    usb_perf_record(USB_PERF_FLUSH, start, acquired, usb_perf_clock(), flags, 0, result);
    usb_io_unlock_irqrestore(flags);
    return result;
}
