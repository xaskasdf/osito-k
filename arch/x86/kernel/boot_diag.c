/*
 * OsitoK x86-64 — persistent boot diagnostics.
 *
 * Captures the serial-backed klog to OsitoFS once storage is mounted.
 * Designed for real hardware where no UART is available after a failed boot.
 */

#include "../include/types.h"

#define BOOT_DIAG_SLOTS        4
#define BOOT_DIAG_CRASH_SLOTS  16
#define BOOT_DIAG_MAX_LOG      (4 * 1024 * 1024)
#define BOOT_DIAG_CHUNK        4096

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);

extern bool osfs2_is_mounted(void);
extern void *osfs2_find(const char *name);
extern void *osfs2_create(const char *name, uint64_t size);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
extern int   osfs2_write_data(void *file, uint64_t offset, const void *buf, uint64_t len);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern int   osfs2_delete(const char *name);
extern int   osfs2_truncate(void *file, uint64_t size);
extern uint64_t osfs2_file_size(void *file);
extern int disk_flush(void);
extern void usb_storage_write_diag_flush(const char *reason) __attribute__((weak));

extern uint32_t klog_read_since(uint64_t *cursor, char *buf, uint32_t max_len);
extern uint64_t klog_total_bytes(void);

static bool boot_diag_ready;
static bool boot_diag_failed;
static bool boot_diag_truncated;
static int  boot_diag_slot_id = -1;
static uint32_t boot_diag_crash_next;
static uint64_t boot_diag_cursor;
static uint64_t boot_diag_log_bytes;
static uint64_t boot_diag_last_hb_ticks;
static uint64_t boot_diag_last_auto_ticks;
static bool boot_diag_flushing;
static void *boot_diag_log_file;
static char boot_diag_log_name[32];
static char boot_diag_chunk[BOOT_DIAG_CHUNK];

void boot_diag_flush(const char *reason);

static char *bd_append_str(char *p, char *end, const char *s)
{
    while (*s && p < end) *p++ = *s++;
    return p;
}

static char *bd_append_dec(char *p, char *end, uint64_t v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) {
        if (p < end) *p++ = '0';
        return p;
    }
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n-- > 0 && p < end) *p++ = tmp[n];
    return p;
}

static void bd_set_boot_name(char *out, int slot)
{
    strcpy(out, "diag/boot0.log");
    out[9] = (char)('0' + (slot & 3));
}

void boot_diag_format_crash_name(char *out, int slot, uint32_t idx, const char *ext)
{
    strcpy(out, "diag/cr0_00.");
    out[7] = (char)('0' + (slot & 3));
    out[9] = (char)('0' + ((idx / 10) % 10));
    out[10] = (char)('0' + (idx % 10));
    out[12] = ext[0];
    out[13] = ext[1];
    out[14] = ext[2];
    out[15] = '\0';
}

static int bd_read_index(void)
{
    void *f = osfs2_find("diag/boot.idx");
    char buf[16];
    uint64_t sz;
    int v = 0;

    if (!f) return 0;
    sz = osfs2_file_size(f);
    if (sz == 0 || sz >= sizeof(buf)) return 0;
    memset(buf, 0, sizeof(buf));
    if (osfs2_read(f, 0, buf, sz) < 0) return 0;
    for (uint64_t i = 0; i < sz; i++) {
        if (buf[i] < '0' || buf[i] > '9') break;
        v = v * 10 + (buf[i] - '0');
    }
    return v & 3;
}

static void bd_write_small(const char *name, const char *buf, uint64_t len)
{
    void *f;

    f = osfs2_find(name);
    if (f && osfs2_write(f, 0, buf, len) == 0 &&
        osfs2_truncate(f, len) == 0)
        return;

    osfs2_delete(name);
    f = osfs2_create(name, len);
    if (f)
        osfs2_write(f, 0, buf, len);
}

static void bd_write_index(int next_slot)
{
    char buf[4];
    buf[0] = (char)('0' + (next_slot & 3));
    buf[1] = '\n';
    buf[2] = '\0';
    bd_write_small("diag/boot.idx", buf, 2);
}

static void bd_delete_slot_crashes(int slot)
{
    char name[32];
    for (uint32_t i = 0; i < BOOT_DIAG_CRASH_SLOTS; i++) {
        boot_diag_format_crash_name(name, slot, i, "bin");
        osfs2_delete(name);
        boot_diag_format_crash_name(name, slot, i, "txt");
        osfs2_delete(name);
    }
}

static int bd_append_log(const char *buf, uint32_t len)
{
    static const char trunc_msg[] = "\n[BDIAG] log truncated at 4194304 bytes\n";
    uint32_t trunc_len = (uint32_t)(sizeof(trunc_msg) - 1);

    if (!boot_diag_ready || boot_diag_failed || boot_diag_truncated || len == 0)
        return 0;

    if (boot_diag_log_bytes + len > BOOT_DIAG_MAX_LOG) {
        uint64_t remaining = BOOT_DIAG_MAX_LOG - boot_diag_log_bytes;
        if (remaining > trunc_len) {
            uint32_t room = (uint32_t)(remaining - trunc_len);
            if (room && osfs2_write_data(boot_diag_log_file, boot_diag_log_bytes, buf, room) == 0)
                boot_diag_log_bytes += room;
            if (osfs2_write_data(boot_diag_log_file, boot_diag_log_bytes, trunc_msg, trunc_len) == 0)
                boot_diag_log_bytes += trunc_len;
        }
        boot_diag_truncated = true;
        return 0;
    }

    if (osfs2_write_data(boot_diag_log_file, boot_diag_log_bytes, buf, len) < 0) {
        boot_diag_failed = true;
        return -1;
    }
    boot_diag_log_bytes += len;
    return 0;
}

static void bd_append_marker(const char *reason)
{
    char line[160];
    char *p = line;
    char *end = line + sizeof(line) - 1;

    p = bd_append_str(p, end, "\n[BDIAG] tick=");
    p = bd_append_dec(p, end, idt_get_ticks());
    p = bd_append_str(p, end, " reason=");
    p = bd_append_str(p, end, reason ? reason : "(none)");
    p = bd_append_str(p, end, " klog_total=");
    p = bd_append_dec(p, end, klog_total_bytes());
    p = bd_append_str(p, end, "\n");
    *p = '\0';
    bd_append_log(line, (uint32_t)(p - line));
}

static void bd_write_latest(const char *reason)
{
    char buf[256];
    char *p = buf;
    char *end = buf + sizeof(buf) - 1;

    p = bd_append_str(p, end, "slot=");
    p = bd_append_dec(p, end, (uint64_t)boot_diag_slot_id);
    p = bd_append_str(p, end, "\nlog=");
    p = bd_append_str(p, end, boot_diag_log_name);
    p = bd_append_str(p, end, "\nreason=");
    p = bd_append_str(p, end, reason ? reason : "(none)");
    p = bd_append_str(p, end, "\nticks=");
    p = bd_append_dec(p, end, idt_get_ticks());
    p = bd_append_str(p, end, "\nbytes=");
    p = bd_append_dec(p, end, boot_diag_log_bytes);
    p = bd_append_str(p, end, "\ntruncated=");
    p = bd_append_dec(p, end, boot_diag_truncated ? 1 : 0);
    p = bd_append_str(p, end, "\n");
    *p = '\0';

    bd_write_small("diag/latest.txt", buf, (uint64_t)(p - buf));
}

void boot_diag_init(void)
{
    int slot;

    if (boot_diag_ready || !osfs2_is_mounted())
        return;

    slot = bd_read_index();
    boot_diag_slot_id = slot;
    bd_write_index((slot + 1) & 3);
    bd_set_boot_name(boot_diag_log_name, slot);

    osfs2_delete(boot_diag_log_name);
    bd_delete_slot_crashes(slot);
    boot_diag_log_file = osfs2_create(boot_diag_log_name, BOOT_DIAG_MAX_LOG);
    if (!boot_diag_log_file) {
        boot_diag_failed = true;
        serial_puts("[BDIAG] failed to create ");
        serial_puts(boot_diag_log_name);
        serial_puts("\n");
        return;
    }
    osfs2_truncate(boot_diag_log_file, 0);

    boot_diag_cursor = 0;
    boot_diag_log_bytes = 0;
    boot_diag_truncated = false;
    boot_diag_failed = false;
    boot_diag_crash_next = 0;
    boot_diag_last_hb_ticks = 0;
    boot_diag_last_auto_ticks = 0;
    boot_diag_flushing = false;
    boot_diag_ready = true;

    serial_puts("[BDIAG] boot diagnostics active: ");
    serial_puts(boot_diag_log_name);
    serial_puts("\n");
    boot_diag_flush("fs-mounted");
}

void boot_diag_flush(const char *reason)
{
    uint64_t target;
    uint32_t n;

    if (!boot_diag_ready || boot_diag_failed)
        return;
    if (boot_diag_flushing)
        return;
    boot_diag_flushing = true;

    if (usb_storage_write_diag_flush)
        usb_storage_write_diag_flush(reason);

    target = klog_total_bytes();
    bd_append_marker(reason);
    while (boot_diag_cursor < target && !boot_diag_truncated && !boot_diag_failed) {
        uint64_t remaining = target - boot_diag_cursor;
        uint32_t want = remaining > BOOT_DIAG_CHUNK ? BOOT_DIAG_CHUNK
                                                    : (uint32_t)remaining;
        n = klog_read_since(&boot_diag_cursor, boot_diag_chunk, want);
        if (!n)
            break;
        if (n)
            bd_append_log(boot_diag_chunk, n);
    }

    bd_write_latest(reason);
    osfs2_truncate(boot_diag_log_file, boot_diag_log_bytes);
    disk_flush();
    boot_diag_flushing = false;
}

void boot_diag_maybe_flush(const char *reason, uint64_t min_bytes,
                           uint64_t min_ticks)
{
    uint64_t total;
    uint64_t pending;
    uint64_t now;

    if (!boot_diag_ready || boot_diag_failed || boot_diag_truncated ||
        boot_diag_flushing)
        return;

    total = klog_total_bytes();
    if (total <= boot_diag_cursor)
        return;
    pending = total - boot_diag_cursor;
    now = idt_get_ticks();

    if (min_bytes && pending >= min_bytes) {
        boot_diag_last_auto_ticks = now;
        boot_diag_flush(reason ? reason : "auto");
        return;
    }

    if (min_ticks && pending && now - boot_diag_last_auto_ticks >= min_ticks) {
        boot_diag_last_auto_ticks = now;
        boot_diag_flush(reason ? reason : "auto");
    }
}

void boot_diag_mark(const char *reason)
{
    if (reason) {
        serial_puts("[BDIAG] ");
        serial_puts(reason);
        serial_puts("\n");
    }
    boot_diag_flush(reason);
}

void boot_diag_heartbeat(const char *reason)
{
    uint64_t now = idt_get_ticks();

    if (!boot_diag_ready)
        return;
    if (boot_diag_last_hb_ticks && now - boot_diag_last_hb_ticks < 100)
        return;
    boot_diag_last_hb_ticks = now;
    boot_diag_mark(reason ? reason : "heartbeat");
}

int boot_diag_slot(void)
{
    return boot_diag_slot_id < 0 ? 0 : boot_diag_slot_id;
}

uint32_t boot_diag_next_crash_index(void)
{
    uint32_t idx = boot_diag_crash_next % BOOT_DIAG_CRASH_SLOTS;
    boot_diag_crash_next++;
    return idx;
}
