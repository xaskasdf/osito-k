#include "../kernel/klog.c"

extern int printf(const char *format, ...);
extern void *malloc(size_t size);
extern void free(void *ptr);
static unsigned checks, failures;
static bool fail_alloc;
void *kmalloc(uint64_t size) { return fail_alloc ? NULL : malloc(size); }
#define CHECK(test) do { checks++; if (!(test)) { \
    printf("FAIL line %d: %s\n", __LINE__, #test); failures++; \
} } while (0)

static char sample(uint64_t index) { return (char)(1 + index % 251); }
static void append(uint32_t count)
{
    uint64_t start = klog_total_bytes();
    for (uint32_t i = 0; i < count; i++) klog_putc(sample(start + i));
}
static void suffix(uint32_t maximum)
{
    char *out = malloc((size_t)maximum + 2);
    CHECK(out != NULL);
    if (!out) return;
    memset(out, 0x5a, (size_t)maximum + 2);
    uint64_t total = klog_total_bytes();
    uint32_t expected = total < KLOG_SIZE ? (uint32_t)total : KLOG_SIZE;
    if (expected > maximum) expected = maximum;
    uint32_t actual = klog_read(out + 1, maximum);
    CHECK(actual == expected);
    CHECK(out[0] == 0x5a && out[actual + 1] == 0x5a);
    bool matches = true;
    for (uint32_t i = 0; i < actual; i++)
        if (out[i + 1] != sample(total - actual + i)) matches = false;
    CHECK(matches);
    free(out);
}
int main(void)
{
    char out[128];
    CHECK(!klog_read(out, sizeof(out)));
    fail_alloc = true; klog_init();
    CHECK(!klog_read(out, sizeof(out)));
    fail_alloc = false; klog_init();
    suffix(64);
    append(8192);
    suffix(0); suffix(1); suffix(64); suffix(8192); suffix(9000);
    CHECK(!klog_read(NULL, 8));
    append(KLOG_SIZE - 8192);
    suffix(1); suffix(64); suffix(KLOG_SIZE); suffix(KLOG_SIZE + 64);
    append(32769);
    suffix(64); suffix(512 * 1024); suffix(KLOG_SIZE + 64);
    uint64_t cursor = 0, oldest = klog_total_bytes() - KLOG_SIZE;
    CHECK(klog_read_since(&cursor, out, sizeof(out)) == sizeof(out));
    CHECK(cursor == oldest + sizeof(out));
    bool matches = true;
    for (unsigned i = 0; i < sizeof(out); i++)
        if (out[i] != sample(oldest + i)) matches = false;
    CHECK(matches);
    cursor = klog_total_bytes();
    CHECK(!klog_read_since(&cursor, out, sizeof(out)));
    free(klog_buf);
    printf("Kernel log suffix: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
