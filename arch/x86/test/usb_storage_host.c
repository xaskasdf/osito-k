#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define OSITOK_X86_TYPES_H
#define USB_STORAGE_HOST_TEST
#include "../drivers/usb_storage.c"

static _Thread_local uint64_t irq_flags = 1ULL << 9;
static _Thread_local uint64_t test_clock;
static _Thread_local unsigned clock_calls, regress_clock_at;
static bool report_output;
static uint64_t report_flags, test_tsc_hz = 1000000000;
static bool concurrent;
uint64_t usb_storage_test_clock(void)
{
    test_clock += 10;
    return ++clock_calls == regress_clock_at ? 0 : test_clock;
}
uint64_t idt_get_tsc_freq(void) { return test_tsc_hz; }
uint64_t usb_storage_test_irq_save(void)
{ uint64_t flags = irq_flags; irq_flags &= ~(1ULL << 9); return flags; }
void usb_storage_test_irq_restore(uint64_t flags) { irq_flags = flags; }

static unsigned failures, checks;
#define CHECK(x) do { __atomic_add_fetch(&checks, 1, __ATOMIC_RELAXED); if (!(x)) { \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED); \
} } while (0)

static _Thread_local unsigned checkpoints;
void sched_io_checkpoint(void)
{
    CHECK(irq_flags & (1ULL << 9));
    /* The next task may have acquired the lock already on another CPU. In
     * sequential checks, ownership must be released before this callback. */
    if (!concurrent) CHECK(!usb_io_lock);
    checkpoints++;
}

static struct {
    cbw_t cbw;
    unsigned phase, commands[256], status, residue;
    unsigned key, asc, ascq, transferred;
    bool wce, fua, wp, mode10_only, no_modes, reject_sync, reject_fua;
    bool descriptor_sense, stall_data, stall_csw, sense_failure;
    bool malformed_mode, short_csw, bad_tag, bad_residue, medium_error;
    bool csw_timeout, phase_error, short_sense, deferred_sense;
    unsigned sync_key, sync_asc, sync_ascq;
    unsigned fua_writes, plain_writes, clears;
    unsigned zero_residue_op, short_op, short_length, csw_zlps, csw_reads, csw_stalls;
    bool data_csw, early_bad_tag, early_missing_data, bad_signature, clear_fail;
    bool csw_stall_first;
} mock;
static char logs[32768];

void serial_puts(const char *s)
{
    if (report_output) {
        CHECK(!usb_io_lock);
        CHECK(irq_flags == report_flags);
    }
    size_t at = strlen(logs), n = strlen(s);
    if (at + n < sizeof(logs)) memcpy(logs + at, s, n + 1);
}
void serial_putdec(uint64_t v)
{
    char s[32]; snprintf(s, sizeof(s), "%llu", (unsigned long long)v); serial_puts(s);
}
void serial_puthex(uint64_t v, int n)
{
    char s[32]; snprintf(s, sizeof(s), "%0*llX", n, (unsigned long long)v); serial_puts(s);
}

static void reject(unsigned key, unsigned asc)
{
    mock.status = 1;
    mock.key = key;
    mock.asc = asc;
    mock.ascq = 0;
}

int xhci_bulk_out(int dev, const void *data, uint32_t len)
{
    CHECK(!(irq_flags & (1ULL << 9)));
    if (concurrent) sched_yield();
    CHECK(dev == 2);
    if (!mock.phase) {
        CHECK(len == sizeof(cbw_t));
        if (len != sizeof(cbw_t)) return -1;
        memcpy(&mock.cbw, data, len);
        CHECK(mock.cbw.dCBWSignature == CBW_SIGNATURE);
        unsigned op = mock.cbw.CBWCB[0];
        mock.commands[op]++;
        mock.status = mock.residue = mock.transferred = 0;
        mock.phase = mock.cbw.dCBWDataTransferLength ? 1 : 2;
        if ((op == 0x1a && mock.mode10_only) ||
            ((op == 0x1a || op == 0x5a) && mock.no_modes)) reject(5, 0x24);
        if (op == 0x35 && mock.reject_sync) {
            reject(mock.sync_key ? mock.sync_key : 5, mock.sync_asc ? mock.sync_asc : 0x24);
            mock.ascq = mock.sync_ascq;
        }
        if (op == 0x2a) {
            if (mock.cbw.CBWCB[1] & 8) {
                mock.fua_writes++;
                if (mock.reject_fua) reject(5, 0x24);
            } else mock.plain_writes++;
            if (mock.medium_error) reject(3, 0x0c);
        }
        return 0;
    }
    CHECK(mock.phase == 1 && !(mock.cbw.bmCBWFlags & 0x80));
    CHECK(len == mock.cbw.dCBWDataTransferLength);
    mock.transferred = len;
    mock.phase = 2;
    return 0;
}

int xhci_bulk_clear_halt(int dev, bool in)
{
    CHECK(!(irq_flags & (1ULL << 9)));
    CHECK(dev == 2 && in);
    mock.clears++;
    return mock.clear_fail ? -1 : 0;
}

int xhci_bulk_in(int dev, void *data, uint32_t len, uint32_t *actual)
{
    CHECK(!(irq_flags & (1ULL << 9)));
    if (concurrent) sched_yield();
    CHECK(dev == 2);
    *actual = 0;
    if (mock.phase == 1) {
        CHECK(mock.cbw.bmCBWFlags & 0x80);
        memset(data, 0, len);
        unsigned op = mock.cbw.CBWCB[0], n = 0;
        uint8_t reply[256] = {0};
        if (mock.status) {
            mock.residue = len;
            mock.phase = 2;
            if (mock.stall_data) return -6;
            if (mock.data_csw) {
                csw_t csw = {CSW_SIGNATURE, mock.cbw.dCBWTag + mock.early_bad_tag,
                             mock.early_missing_data ? 0 : len,
                             mock.early_missing_data ? 0 : 1};
                CHECK(len >= sizeof(csw));
                memcpy(data, &csw, sizeof(csw));
                *actual = sizeof(csw);
                mock.phase = 0;
            }
            return 0;
        }
        if (op == 0x12) {
            n = 36;
            reply[4] = 31;
            memcpy(reply + 8, "TEST    CACHE DISK      ", 24);
        } else if (op == 0x25) {
            n = 8;
            reply[2] = 0xff; reply[3] = 0xff; reply[6] = 2;
        } else if (op == 0x1a || op == 0x5a) {
            unsigned header = op == 0x1a ? 4 : 8;
            unsigned page = header + 8;
            n = page + 20;
            if (op == 0x1a) { reply[0] = n - 1; reply[3] = 8; }
            else { reply[1] = n - 2; reply[7] = 8; }
            reply[op == 0x5a ? 3 : 2] =
                (mock.fua ? 0x10 : 0) | (mock.wp ? 0x80 : 0);
            reply[page] = 8;
            reply[page + 1] = 18;
            reply[page + 2] = mock.wce ? 4 : 0;
            if (mock.malformed_mode) reply[page + 1] = 250;
        } else if (op == 3) {
            n = 18;
            if (mock.descriptor_sense) {
                reply[0] = 0x72; reply[1] = mock.key;
                reply[2] = mock.asc; reply[3] = mock.ascq;
                reply[7] = 0; n = 8;
            } else {
                reply[0] = 0x70; reply[2] = mock.key; reply[7] = 10;
                reply[12] = mock.asc; reply[13] = mock.ascq;
            }
            if (mock.sense_failure) mock.status = 1;
            if (mock.deferred_sense) reply[0]++;
            if (mock.short_sense) n = 4;
        } else if (op == 0x28) {
            memset(data, 0xa5, len);
            n = len;
        }
        if (n > len) n = len;
        if (op == mock.short_op && n > mock.short_length) n = mock.short_length;
        if (op != 0x28) memcpy(data, reply, n);
        *actual = mock.transferred = n;
        mock.residue = len - n;
        mock.phase = 2;
        return 0;
    }
    CHECK(mock.phase == 2 && len == sizeof(csw_t));
    mock.csw_reads++;
    if (mock.csw_stall_first && mock.stall_csw) { mock.stall_csw = false; return -6; }
    if (mock.csw_zlps) { mock.csw_zlps--; return 0; }
    if (mock.csw_timeout) return -1;
    if (mock.stall_csw) { mock.stall_csw = false; return -6; }
    if (mock.csw_stalls) { mock.csw_stalls--; return -6; }
    csw_t csw = {mock.bad_signature ? 0 : CSW_SIGNATURE, mock.cbw.dCBWTag + (mock.bad_tag ? 1 : 0),
                 mock.bad_residue ? mock.cbw.dCBWDataTransferLength + 1 :
                 mock.cbw.CBWCB[0] == mock.zero_residue_op ? 0 : mock.residue,
                 mock.phase_error ? 2 : mock.status};
    *actual = mock.short_csw ? 12 : sizeof(csw);
    memcpy(data, &csw, *actual);
    mock.phase = 0;
    return 0;
}

static void reset(void)
{
    memset(&mock, 0, sizeof(mock));
    memset(logs, 0, sizeof(logs));
    memset(&usb_perf, 0, sizeof(usb_perf));
}

static void test_performance_counters(void)
{
    uint8_t data[192 * 512];
    reset(); mock.wce = true;
    CHECK(!usb_storage_init(2));
    CHECK(usb_perf.op[USB_PERF_INIT].calls == 1);
    memset(&usb_perf, 0, sizeof(usb_perf));
    CHECK(!usb_storage_read(8, 192, data));
    CHECK(usb_perf.op[USB_PERF_READ].calls == 1);
    CHECK(usb_perf.op[USB_PERF_READ].failures == 0);
    CHECK(usb_perf.op[USB_PERF_READ].ok_bytes == sizeof(data));
    CHECK(usb_perf.op[USB_PERF_READ].requested_bytes == sizeof(data));
    CHECK(usb_perf.op[USB_PERF_READ].wait_ticks == 10);
    CHECK(usb_perf.op[USB_PERF_READ].hold_ticks == 130);
    CHECK(usb_perf.op[USB_PERF_READ].max_hold_ticks == 130);
    CHECK(!usb_perf.op[USB_PERF_READ].entry_irq_off);
    CHECK(usb_perf.read_sizes[6] == 1);
    for (unsigned i = 0; i < USB_PERF_PHASES; i++) {
        CHECK(usb_perf.phase[i].calls == 2);
        CHECK(usb_perf.phase[i].ticks == 20 && usb_perf.phase[i].max_ticks == 10);
        CHECK(!usb_perf.phase[i].failures && !usb_perf.phase[i].bad_clock);
    }
    CHECK(usb_perf.phase[USB_PERF_CBW].ok_bytes == 62);
    CHECK(usb_perf.phase[USB_PERF_CSW].ok_bytes == 26);
    CHECK(usb_perf.phase[USB_PERF_DATA].ok_bytes == sizeof(data));

    irq_flags = 0;
    CHECK(!usb_storage_read(8, 1, data));
    CHECK(usb_perf.op[USB_PERF_READ].entry_irq_off == 1);
    CHECK(usb_perf.op[USB_PERF_READ].hold_ticks == 200);
    CHECK(usb_perf.read_sizes[0] == 1 && !irq_flags);
    irq_flags = 1ULL << 9;
    CHECK(!usb_storage_write(8, 1, data));
    CHECK(!usb_storage_flush());
    CHECK(usb_perf.op[USB_PERF_WRITE].ok_bytes == 512 && usb_perf.write_sizes[0] == 1);
    CHECK(usb_perf.op[USB_PERF_FLUSH].calls == 1);
    CHECK(usb_perf.op[USB_PERF_FLUSH].hold_ticks == 50); /* No data phase. */
    CHECK(!usb_storage_flush());
    CHECK(usb_perf.op[USB_PERF_FLUSH].hold_ticks == 60); /* Clean cache: no command. */

    /* Failure and a partially filled destination are not a successful read. */
    mock.short_op = 0x28; mock.short_length = 511;
    uint64_t ok = usb_perf.op[USB_PERF_READ].ok_bytes;
    CHECK(usb_storage_read(8, 1, data) < 0);
    CHECK(usb_perf.op[USB_PERF_READ].ok_bytes == ok);
    CHECK(usb_perf.op[USB_PERF_READ].failures == 1);
    mock.short_op = 0; mock.csw_timeout = true;
    CHECK(usb_storage_read(8, 1, data) < 0);
    CHECK(usb_perf.op[USB_PERF_READ].failures == 2);
    CHECK(usb_perf.phase[USB_PERF_CSW].failures == 1);

    usb_perf_t saved = usb_perf;
    unsigned commands[256];
    memcpy(commands, mock.commands, sizeof(commands));
    for (unsigned disabled = 0; disabled < 2; disabled++) {
        report_flags = irq_flags = disabled ? 0 : 1ULL << 9;
        logs[0] = 0;
        report_output = true;
        usb_storage_perf_report();
        report_output = false;
        CHECK(!memcmp(&saved, &usb_perf, sizeof(saved)));
        CHECK(!memcmp(commands, mock.commands, sizeof(commands)));
        CHECK(strstr(logs, "[USB-PERF] cumulative tsc_hz=1000000000 now=") != NULL);
        CHECK(strstr(logs, "op=read calls=4 failures=2") != NULL);
        CHECK(strstr(logs, "bounds_bytes=512/4096/8192/16384/32768/65536/more") != NULL);
    }
    irq_flags = 1ULL << 9;
    test_tsc_hz = 0;
    logs[0] = 0;
    usb_storage_perf_report();
    CHECK(strstr(logs, "cumulative tsc_hz=0 ") != NULL);
    test_tsc_hz = 1000000000;

    reset();
    CHECK(!usb_storage_init(2));
    memset(&usb_perf, 0, sizeof(usb_perf));
    static const unsigned counts[] = {1, 2, 8, 9, 16, 17, 32, 33, 64, 65, 128, 129};
    static const unsigned bins[] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6};
    uint64_t expected[USB_PERF_SIZES] = {0};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        CHECK(!usb_storage_read(8, counts[i], data));
        expected[bins[i]]++;
        CHECK(!memcmp(usb_perf.read_sizes, expected, sizeof(expected)));
    }
    uint64_t calls = usb_perf.phase[USB_PERF_CBW].calls;
    CHECK(usb_storage_read(8, 1, NULL) < 0);
    CHECK(!usb_storage_read(8, 0, NULL));
    CHECK(usb_perf.phase[USB_PERF_CBW].calls == calls);

    saved = usb_perf;
    usb_perf_record(USB_PERF_READ, 20, 10, 30, irq_flags, 0, -1);
    usb_perf_record(USB_PERF_READ, 10, 20, 15, irq_flags, 0, -1);
    CHECK(usb_perf.op[USB_PERF_READ].bad_clock == 2);
    CHECK(usb_perf.op[USB_PERF_READ].hold_ticks == saved.op[USB_PERF_READ].hold_ticks);
    CHECK(usb_perf.op[USB_PERF_READ].wait_ticks == saved.op[USB_PERF_READ].wait_ticks);
    regress_clock_at = clock_calls + 4; /* End of next CBW, not its API call. */
    CHECK(!usb_storage_read(8, 1, data));
    CHECK(usb_perf.phase[USB_PERF_CBW].bad_clock == 1);
    CHECK(usb_perf.phase[USB_PERF_CBW].ticks == saved.phase[USB_PERF_CBW].ticks);
    regress_clock_at = 0;
}

static void test_cache(void)
{
    uint8_t block[512] = {0};
    reset(); mock.reject_sync = mock.reject_fua = true;
    CHECK(usb_storage_init(2) == 0);
    CHECK(mock.commands[0x1a] > 0);
    CHECK(usb_storage_write(8, 1, block) == 0);
    CHECK(usb_storage_flush() == 0);
    CHECK(mock.fua_writes == 0 && mock.plain_writes == 1);
    CHECK(mock.commands[0x35] == 0);

    reset(); mock.wce = true;
    CHECK(usb_storage_init(2) == 0);
    CHECK(usb_storage_write(8, 1, block) == 0);
    CHECK(usb_storage_flush() == 0);
    CHECK(mock.fua_writes == 0 && mock.plain_writes == 1);
    CHECK(mock.commands[0x35] == 1);

    reset(); mock.wce = mock.fua = mock.reject_sync = true;
    CHECK(usb_storage_init(2) == 0);
    CHECK(usb_storage_write(8, 1, block) == 0);
    CHECK(usb_storage_flush() == 0);
    CHECK(mock.fua_writes == 1 && mock.plain_writes == 0);
    CHECK(mock.commands[0x35] == 0);

    reset(); mock.wce = mock.fua = mock.reject_fua = true;
    CHECK(usb_storage_init(2) == 0);
    CHECK(usb_storage_write(8, 1, block) == 0);
    CHECK(usb_storage_flush() == 0);
    CHECK(mock.fua_writes == 1 && mock.plain_writes == 1);
    CHECK(mock.commands[0x35] == 1);
    CHECK(usb_storage_write(9, 1, block) == 0);
    CHECK(mock.fua_writes == 1 && mock.plain_writes == 2);

    reset(); mock.mode10_only = mock.reject_sync = mock.stall_data = true;
    CHECK(usb_storage_init(2) == 0);
    CHECK(mock.commands[0x5a] > 0 && mock.clears > 0);
    CHECK(usb_storage_write(8, 1, block) == 0);
    CHECK(usb_storage_flush() == 0);

    reset(); mock.wp = true;
    CHECK(usb_storage_init(2) == 0);
    CHECK(usb_storage_write(8, 1, block) < 0);
    CHECK(mock.commands[0x2a] == 0);
}

static void test_failures(void)
{
    uint8_t block[512] = {0};
    for (unsigned descriptor = 0; descriptor < 2; descriptor++) {
        reset(); mock.no_modes = mock.reject_sync = true;
        mock.descriptor_sense = descriptor;
        CHECK(usb_storage_init(2) == 0);
        CHECK(usb_storage_write(8, 1, block) == 0);
        CHECK(usb_storage_flush() == 0);
        CHECK(mock.fua_writes == 0);
        CHECK(usb_storage_flush() == 0);
        CHECK(mock.commands[0x35] == 1);
        CHECK(strstr(logs, "continuing without flush guarantee") != NULL);
        CHECK(usb_storage_read(8, 1, block) == 0 && block[0] == 0xa5);
    }
    reset(); mock.wce = mock.fua = mock.medium_error = true;
    CHECK(usb_storage_init(2) == 0);
    CHECK(usb_storage_write(8, 1, block) < 0);
    CHECK(mock.commands[0x2a] == 1 && mock.plain_writes == 0);

    reset(); mock.malformed_mode = mock.reject_sync = true;
    CHECK(usb_storage_init(2) == 0);
    CHECK(usb_storage_write(8, 1, block) == 0);
    CHECK(usb_storage_flush() == 0);

    reset(); mock.wce = mock.fua = mock.reject_fua = mock.sense_failure = true;
    CHECK(usb_storage_init(2) == 0);
    CHECK(usb_storage_write(8, 1, block) < 0);
    CHECK(mock.commands[0x2a] == 1);

    for (unsigned bad = 0; bad < 5; bad++) {
        reset();
        CHECK(usb_storage_init(2) == 0);
        mock.short_csw = bad == 0; mock.bad_tag = bad == 1; mock.bad_residue = bad == 2;
        mock.phase_error = bad == 3; mock.csw_timeout = bad == 4;
        CHECK(usb_storage_read(8, 1, block) < 0);
        CHECK(usb_storage_flush() < 0);
    }
    reset(); mock.short_csw = true;
    CHECK(usb_storage_init(2) < 0 && !usb_storage_is_ready());
    reset(); mock.stall_csw = true;
    CHECK(usb_storage_init(2) == 0 && mock.clears == 1);
    CHECK(usb_storage_read(65536, 1, block) < 0);
    CHECK(usb_storage_write(UINT64_MAX, 1, block) < 0);

    for (unsigned bad = 0; bad < 7; bad++) {
        reset(); mock.wce = mock.reject_sync = true;
        CHECK(usb_storage_init(2) == 0);
        CHECK(usb_storage_write(8, 1, block) == 0);
        mock.sync_key = bad == 0 ? 3 : 5;
        mock.sync_asc = bad == 1 ? 0x21 : 0x24;
        mock.sync_ascq = bad == 2 ? 1 : 0;
        mock.sense_failure = bad == 3;
        mock.short_sense = bad == 4;
        mock.deferred_sense = bad >= 5;
        mock.descriptor_sense = bad == 6;
        CHECK(usb_storage_flush() < 0);
        CHECK(!usb_disk.sync_unsupported);
        CHECK(strstr(logs, "continuing without flush guarantee") == NULL);
    }
    reset(); mock.wce = mock.reject_sync = true; mock.sync_asc = 0x20;
    CHECK(usb_storage_init(2) == 0);
    CHECK(usb_storage_write(8, 1, block) == 0);
    CHECK(usb_storage_flush() == 0);
    CHECK(usb_disk.sync_unsupported && usb_disk.needs_flush);
    size_t log_length = strlen(logs);
    CHECK(usb_storage_flush() == 0 && mock.commands[0x35] == 1);
    CHECK(strlen(logs) == log_length);
    reset();
    CHECK(usb_storage_init(2) == 0 && !usb_disk.sync_unsupported);
}

static void test_short_responses(void)
{
    uint8_t block[512] = {0};
    reset(); mock.wce = mock.fua = true; mock.zero_residue_op = 0x1a;
    CHECK(usb_storage_init(2) == 0);
    CHECK(usb_storage_is_ready());
    CHECK(usb_disk.cache_mode == USB_CACHE_BACK && usb_disk.fua);
    CHECK(usb_storage_read(8, 1, block) == 0);
    CHECK(usb_storage_write(8, 1, block) == 0 && mock.fua_writes == 1);
    CHECK(strstr(logs, "Short IN: CSW understates residue") != NULL);

    reset(); mock.mode10_only = mock.wce = true; mock.zero_residue_op = 0x5a;
    CHECK(usb_storage_init(2) == 0);
    CHECK(usb_disk.cache_mode == USB_CACHE_BACK && !usb_disk.fua);
    CHECK(usb_storage_write(8, 1, block) == 0);
    CHECK(usb_storage_flush() == 0 && mock.commands[0x35] == 1);

    reset(); mock.zero_residue_op = 0x28; mock.short_op = 0x28; mock.short_length = 511;
    CHECK(usb_storage_init(2) == 0);
    CHECK(usb_storage_read(8, 1, block) < 0);
    CHECK(usb_storage_is_ready()); /* Valid CSW, but a sector read must still be complete. */
    mock.short_op = 0;
    CHECK(usb_storage_read(8, 1, block) == 0);

    for (unsigned op = 0x12; op <= 0x25; op += 0x13) {
        reset(); mock.zero_residue_op = mock.short_op = op; mock.short_length = 7;
        CHECK(usb_storage_init(2) < 0 && !usb_storage_is_ready());
    }
    reset(); mock.csw_zlps = 1;
    CHECK(usb_storage_init(2) == 0 && mock.csw_zlps == 0);
    reset(); mock.csw_zlps = 2;
    CHECK(usb_storage_init(2) < 0 && usb_disk.transport_failed);
    CHECK(mock.csw_reads == 2);
    CHECK(strstr(logs, "BOT cdb=0x12 stage=csw-length rc=0 want=36 data=36 csw=0") != NULL);

    for (unsigned first = 0; first < 2; first++) {
        reset(); mock.csw_zlps = 1; mock.stall_csw = true; mock.csw_stall_first = first;
        CHECK(usb_storage_init(2) == 0);
        CHECK(mock.clears == 1 && mock.csw_reads == 5); /* INQUIRY 3, CAPACITY 1, MODE 1. */
    }
    reset(); mock.csw_stalls = 2;
    CHECK(usb_storage_init(2) < 0 && mock.clears == 1 && mock.csw_reads == 2);
    reset(); mock.stall_csw = mock.clear_fail = true;
    CHECK(usb_storage_init(2) < 0 && mock.clears == 1 && mock.csw_reads == 1);

    reset(); mock.mode10_only = mock.data_csw = true;
    CHECK(usb_storage_init(2) == 0);
    CHECK(mock.commands[3] == 1 && mock.commands[0x5a] == 1);
    CHECK(usb_storage_read(8, 1, block) == 0);
    for (unsigned bad = 0; bad < 2; bad++) {
        reset(); mock.mode10_only = mock.data_csw = true;
        mock.early_bad_tag = bad == 0; mock.early_missing_data = bad == 1;
        CHECK(usb_storage_init(2) < 0 && usb_disk.transport_failed);
        CHECK(mock.commands[3] == 0 && mock.csw_reads == 2);
        CHECK(strstr(logs, bad ? "stage=missing-data" : "stage=csw-identity") != NULL);
    }
    reset(); mock.bad_signature = true;
    CHECK(usb_storage_init(2) < 0 && usb_disk.transport_failed);
    CHECK(strstr(logs, "stage=csw-identity") != NULL);

    /* Every undersized mandatory response fails, regardless of a zero residue. */
    for (unsigned length = 0; length < sizeof(block); length++) {
        reset();
        CHECK(usb_storage_init(2) == 0);
        mock.zero_residue_op = mock.short_op = 0x28; mock.short_length = length;
        CHECK(usb_storage_read(8, 1, block) < 0 && usb_storage_is_ready());
    }
}

static unsigned start_workers;
static void *storage_worker(void *arg)
{
    uintptr_t kind = (uintptr_t)arg;
    uint8_t block[512];
    memset(block, 0x5a, sizeof(block));
    while (!__atomic_load_n(&start_workers, __ATOMIC_ACQUIRE)) sched_yield();
    for (unsigned i = 0; i < 128; i++) {
        uint64_t flags = (i & 1) ? 0 : 1ULL << 9;
        irq_flags = flags;
        unsigned before = checkpoints;
        if (kind == 0) {
            CHECK(!usb_storage_read(8, 1, block)); CHECK(block[0] == 0xa5);
        } else if (kind == 1) CHECK(!usb_storage_write(9, 1, block));
        else if (kind == 2) CHECK(!usb_storage_flush());
        else usb_storage_write_diag_flush("concurrent");
        CHECK(irq_flags == flags);
        CHECK(checkpoints == before + !!(flags & (1ULL << 9)));
    }
    return NULL;
}

static void test_concurrent_io(void)
{
    reset(); mock.wce = mock.fua = mock.reject_fua = mock.reject_sync = true;
    CHECK(!usb_storage_init(2));
    concurrent = true;
    pthread_t threads[4];
    bool created[4];
    for (uintptr_t i = 0; i < 4; i++) {
        created[i] = !pthread_create(&threads[i], NULL, storage_worker, (void *)i);
        CHECK(created[i]);
    }
    __atomic_store_n(&start_workers, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 4; i++) if (created[i]) CHECK(!pthread_join(threads[i], NULL));
    concurrent = false;
    CHECK(usb_storage_is_ready() && !mock.phase);
    CHECK(mock.commands[0x28] == 128 && mock.plain_writes == 128 && mock.fua_writes == 1);
    CHECK(!usb_storage_flush() && usb_disk.sync_unsupported);
    CHECK(mock.commands[3] == 2); /* FUA fallback and unsupported flush retain their sense. */
    CHECK(usb_perf.op[USB_PERF_READ].calls == 128 && usb_perf.op[USB_PERF_READ].ok_bytes == 128 * 512);
    CHECK(usb_perf.op[USB_PERF_WRITE].calls == 128 && usb_perf.op[USB_PERF_WRITE].ok_bytes == 128 * 512);
    CHECK(usb_perf.op[USB_PERF_READ].entry_irq_off == 64);
    CHECK(usb_perf.op[USB_PERF_WRITE].entry_irq_off == 64);
    CHECK(irq_flags == (1ULL << 9));
}

int main(void)
{
    test_cache();
    test_failures();
    test_short_responses();
    test_performance_counters();
    test_concurrent_io();
    printf("USB storage: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
