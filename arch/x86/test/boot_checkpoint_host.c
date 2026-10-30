#include "../kernel/boot_diag.c"
extern int printf(const char *, ...);
extern char *strstr(const char *, const char *);

static unsigned checks, failures, flushes;
static char log_data[8192], disk_data[BOOT_DIAG_MAX_LOG];
static uint32_t log_length;
static uint64_t disk_size;
static bool fail_write, fail_size, fail_flush, short_read, reenter;
static bool fs_mounted, fs_v3, diag_dir, fail_mkdir;
static unsigned mkdir_calls, creates, deletes;
static int log_token, latest_token;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)

void serial_puts(const char *s)
{
    size_t n = strlen(s);
    CHECK(n < sizeof(log_data) - log_length);
    memcpy(log_data + log_length, s, n);
    log_length += n;
}
void serial_putdec(uint64_t n) { (void)n; }
uint64_t idt_get_ticks(void) { return 42; }
uint64_t klog_total_bytes(void) { return log_length; }
uint32_t klog_read_since(uint64_t *cursor, char *buf, uint32_t max)
{
    if (short_read) return 0;
    uint32_t n = log_length - *cursor;
    if (n > max) n = max;
    memcpy(buf, log_data + *cursor, n);
    *cursor += n;
    return n;
}
bool osfs2_is_mounted(void) { return fs_mounted; }
bool osfs3_is_mounted(void) { return fs_v3; }
bool osfs3_directory_exists_ci(const char *name) { CHECK(!strcmp(name, "diag")); return diag_dir; }
int osfs3_mkdir(const char *name)
{
    CHECK(!strcmp(name, "diag")); mkdir_calls++;
    if (fail_mkdir) return -1;
    diag_dir = true; return 0;
}
void *osfs2_find(const char *name) { return !strcmp(name, "diag/latest.txt") ? &latest_token : NULL; }
uint64_t osfs2_file_size(void *file) { (void)file; return 0; }
int osfs2_read(void *file, uint64_t at, void *out, uint64_t n)
{ (void)file; (void)at; (void)out; (void)n; return -1; }
void *osfs2_create(const char *name, uint64_t size)
{
    (void)name; creates++; CHECK(!fs_v3 || diag_dir);
    return size == BOOT_DIAG_MAX_LOG ? &log_token : &latest_token;
}
int osfs2_delete(const char *name) { (void)name; deletes++; return 0; }
int osfs2_truncate(void *f, uint64_t size) { (void)size; CHECK(f == &latest_token); return 0; }
int osfs2_write(void *f, uint64_t at, const void *data, uint64_t len)
{
    (void)at; (void)data; (void)len;
    CHECK(f == &latest_token);
    return 0;
}
int osfs2_write_data(void *f, uint64_t at, const void *data, uint64_t len)
{
    CHECK(f == &log_token && at + len <= sizeof(disk_data));
    if (reenter) CHECK(!boot_diag_checkpoint("nested"));
    if (fail_write) return -1;
    memcpy(disk_data + at, data, len);
    return 0;
}
int osfs2_set_size_reserved(void *f, uint64_t size)
{
    CHECK(f == &log_token);
    if (fail_size) return -1;
    disk_size = size;
    return 0;
}
int disk_flush(void) { flushes++; return fail_flush ? -1 : 0; }

static void reset(void)
{
    memset(log_data, 0, sizeof(log_data));
    memset(disk_data, 0, sizeof(disk_data));
    log_length = flushes = 0;
    disk_size = 0;
    fail_write = fail_size = fail_flush = short_read = reenter = false;
    fs_mounted = fs_v3 = true; diag_dir = fail_mkdir = false;
    mkdir_calls = creates = deletes = 0;
    boot_diag_ready = true;
    boot_diag_failed = boot_diag_truncated = boot_diag_flushing = false;
    boot_diag_last_flush_ok = true; /* Must not allow stale success. */
    boot_diag_cursor = boot_diag_log_bytes = 0;
    boot_diag_slot_id = 0;
    boot_diag_log_file = &log_token;
    strcpy(boot_diag_log_name, "diag/boot0.log");
}

int main(void)
{
    reset(); boot_diag_ready = false;
    boot_diag_init();
    CHECK(boot_diag_ready && !boot_diag_failed && mkdir_calls == 1 && creates == 2);
    CHECK(flushes == 1 && disk_size > 0);
    boot_diag_init(); CHECK(mkdir_calls == 1 && creates == 2 && flushes == 1);
    reset(); boot_diag_ready = false; diag_dir = true;
    boot_diag_init(); CHECK(boot_diag_ready && !mkdir_calls && creates == 2);
    reset(); boot_diag_ready = false; fail_mkdir = true;
    boot_diag_init(); CHECK(!boot_diag_ready && boot_diag_failed && !creates && !deletes && !flushes);
    reset(); boot_diag_ready = fs_mounted = false;
    boot_diag_init(); CHECK(!boot_diag_ready && !mkdir_calls && !creates && !deletes);
    reset(); boot_diag_ready = fs_v3 = false;
    boot_diag_init(); CHECK(boot_diag_ready && !mkdir_calls && creates == 2);
    reset();
    serial_puts("before-command\n");
    CHECK(boot_diag_checkpoint("wlan-file-read") && flushes == 1 && disk_size > 0);
    CHECK(strstr(disk_data, "before-command\n") != NULL);
    CHECK(strstr(disk_data, "[BDIAG] wlan-file-read\n") != NULL);
    uint64_t prior = disk_size;
    CHECK(boot_diag_checkpoint("wlan-file-ready") && flushes == 2 && disk_size > prior);
    CHECK(strstr(disk_data, "[BDIAG] wlan-file-ready\n") != NULL);
    for (unsigned kind = 0; kind < 9; kind++) {
        reset();
        switch (kind) {
        case 0: boot_diag_ready = false; break;
        case 1: boot_diag_failed = true; break;
        case 2: boot_diag_truncated = true; break;
        case 3: boot_diag_flushing = true; break;
        case 4: fail_write = true; break;
        case 5: fail_size = true; break;
        case 6: fail_flush = true; break;
        case 7: short_read = true; break;
        case 8: boot_diag_log_bytes = BOOT_DIAG_MAX_LOG; break;
        }
        CHECK(!boot_diag_checkpoint("must-not-succeed"));
        if (kind < 4) CHECK(!flushes && !log_length);
        else CHECK(!boot_diag_flushing);
    }
    reset();
    CHECK(!boot_diag_checkpoint(NULL) && !flushes);
    reenter = true;
    CHECK(boot_diag_checkpoint("outer") && flushes == 1);
    CHECK(strstr(disk_data, "nested") == NULL);
    reset(); fail_flush = true;
    CHECK(!boot_diag_checkpoint("failed-flush"));
    fail_flush = false;
    CHECK(boot_diag_checkpoint("retry") && flushes == 2);
    printf("boot checkpoint persistence: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
