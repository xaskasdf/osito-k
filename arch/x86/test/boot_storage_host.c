#include "../kernel/boot_diag.c"
extern int printf(const char *, ...);

static char log_data[32768];
static uint32_t log_length, read_chunk = 7, failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); failures++; \
} } while (0)

uint64_t klog_total_bytes(void) { return log_length; }
uint32_t klog_read_since(uint64_t *cursor, char *buffer, uint32_t limit)
{
    uint32_t count = log_length - (uint32_t)*cursor;
    if (count > limit) count = limit;
    if (count > read_chunk) count = read_chunk;
    memcpy(buffer, log_data + *cursor, count);
    *cursor += count;
    return count;
}

static void append(const char *s)
{
    uint32_t n = strlen(s);
    CHECK(n < sizeof(log_data) - log_length);
    memcpy(log_data + log_length, s, n);
    log_length += n;
}

int main(void)
{
    boot_diag_capture_storage();
    CHECK(boot_diag_storage_count() == 0 && boot_diag_storage_line(0) == NULL);
    append("[MEMSTAT] ignored\r\n[PCI] xHCI controller\r\n[xHCI] Bulk error code=5\n");
    append("[USB-STOR] INQUIRY failed\n[KERN] OsitoFS not found on any block device\n");
    boot_diag_capture_storage();
    CHECK(boot_diag_storage_count() == 4 && boot_diag_storage_dropped() == 0);
    CHECK(!strcmp(boot_diag_storage_line(0), "[PCI] xHCI controller"));
    CHECK(!strcmp(boot_diag_storage_line(3), "[KERN] OsitoFS not found on any block device"));
    CHECK(boot_diag_storage_line(4) == NULL);
    memset(log_data, 0, sizeof(log_data)); log_length = 0;
    CHECK(!strcmp(boot_diag_storage_line(1), "[xHCI] Bulk error code=5"));

    for (unsigned i = 0; i < 70; i++) append("[GPT] Reading GPT header...\n");
    append("[OsitoFS v3] mounted\n");
    boot_diag_capture_storage();
    CHECK(boot_diag_storage_count() == 64 && boot_diag_storage_dropped() == 7);
    CHECK(!strcmp(boot_diag_storage_line(63), "[OsitoFS v3] mounted"));
    log_length = 0;
    append("[xHCI] ");
    for (unsigned i = 0; i < 600; i++) append("x");
    append("\n[BLKDEV] registered");
    read_chunk = 512;
    boot_diag_capture_storage();
    CHECK(boot_diag_storage_count() == 2 && boot_diag_storage_dropped() == 0);
    CHECK(strlen(boot_diag_storage_line(0)) == 127);
    CHECK(!strcmp(boot_diag_storage_line(0) + 124, "..."));
    CHECK(!strcmp(boot_diag_storage_line(1), "[BLKDEV] registered"));
    printf("boot storage snapshot failures: %u\n", failures);
    return failures != 0;
}
