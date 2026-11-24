#include "../kernel/shell_redirect.h"

extern int printf(const char *format, ...);
extern void abort(void);

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static struct {
    char data[4096];
    uint64_t size;
    uint16_t mode;
    bool exists, create_fail, mode_fail, truncate_fail, write_fail, flush_fail;
    unsigned creates, truncates, writes, flushes;
} fs;

void *osfs2_find(const char *path)
{
    CHECK(!strcmp(path, "log"));
    return fs.exists ? &fs : NULL;
}
void *osfs2_create(const char *path, uint64_t size)
{
    CHECK(!strcmp(path, "log") && !size);
    fs.creates++;
    if (fs.create_fail) return NULL;
    fs.exists = true;
    fs.size = size;
    return &fs;
}
int osfs2_get_mode(const void *file, uint16_t *mode)
{
    CHECK(file == &fs);
    *mode = fs.mode;
    return fs.mode_fail ? -1 : 0;
}
uint64_t osfs2_file_size(void *file)
{
    CHECK(file == &fs);
    return fs.size;
}
int osfs2_truncate_ex(void *file, uint64_t size, uint32_t flags)
{
    CHECK(file == &fs && !size && !flags);
    fs.truncates++;
    if (fs.truncate_fail) return -1;
    fs.size = size;
    memset(fs.data, 0, sizeof(fs.data));
    return 0;
}
int osfs2_write_ex(void *file, uint64_t offset, const void *data,
                    uint64_t length, uint32_t flags)
{
    CHECK(file == &fs && length && !flags);
    fs.writes++;
    if (fs.write_fail) return -1;
    if (offset > sizeof(fs.data) || length > sizeof(fs.data) - offset) abort();
    memcpy(fs.data + offset, data, length);
    if (fs.size < offset + length) fs.size = offset + length;
    return 0; /* OsitoFS returns status, not a byte count. */
}
int disk_flush(void)
{
    fs.flushes++;
    return fs.flush_fail ? -1 : 0;
}

static void reset(bool exists)
{
    memset(&fs, 0, sizeof(fs));
    fs.exists = exists;
    fs.mode = 0100644;
    if (exists) {
        memcpy(fs.data, "old trailing data", 17);
        fs.size = 17;
    }
}

static void test_files(void)
{
    shell_output_t out;
    for (unsigned append = 0; append < 2; append++) {
        for (unsigned exists = 0; exists < 2; exists++) {
            for (unsigned empty = 0; empty < 2; empty++) {
                reset(exists);
                CHECK(!shell_output_open(&out, "log", append));
                CHECK(fs.creates == !exists && fs.truncates == !append);
                uint64_t prefix = append && exists ? 17 : 0;
                CHECK(fs.size == prefix); /* Truncated before command output. */
                CHECK(!shell_output_finish(&out, "new", empty ? 0 : 3));
                CHECK(fs.size == prefix + (empty ? 0 : 3));
                CHECK(fs.writes == !empty && fs.flushes == 1);
                if (prefix) CHECK(!memcmp(fs.data, "old trailing data", 17));
                if (!empty) CHECK(!memcmp(fs.data + prefix, "new", 3));
            }
        }
    }

    reset(false); fs.create_fail = true;
    CHECK(!strcmp(shell_output_open(&out, "log", false), "cannot open/create target"));
    CHECK(!out.file && !fs.truncates && !fs.writes && !fs.flushes);
    CHECK(!strcmp(shell_output_finish(&out, "x", 1), "target is not open"));
    for (unsigned append = 0; append < 2; append++) {
        reset(true); fs.mode = 0040755;
        CHECK(!strcmp(shell_output_open(&out, "log", append), "target is not a regular file"));
        CHECK(!out.file && !fs.truncates && !fs.writes && !fs.flushes);
    }
    reset(true); fs.mode_fail = true;
    CHECK(shell_output_open(&out, "log", false) && !out.file && fs.size == 17);
    reset(true); fs.truncate_fail = true;
    CHECK(!strcmp(shell_output_open(&out, "log", false), "truncate failed"));
    CHECK(!out.file && fs.size == 17 && !fs.writes && !fs.flushes);

    reset(true);
    CHECK(!shell_output_open(&out, "log", true));
    fs.write_fail = true;
    CHECK(!strcmp(shell_output_finish(&out, "x", 1), "write failed"));
    CHECK(fs.size == 17 && !fs.flushes);
    fs.write_fail = false;
    fs.size = UINT64_MAX;
    CHECK(!strcmp(shell_output_finish(&out, "x", 1), "write failed"));
    CHECK(fs.writes == 1 && !fs.flushes);

    for (unsigned length = 0; length < 2; length++) {
        reset(true);
        CHECK(!shell_output_open(&out, "log", false));
        fs.flush_fail = true;
        CHECK(!strcmp(shell_output_finish(&out, "x", length), "sync failed"));
        CHECK(fs.writes == length && fs.flushes == 1 && fs.size == length);
    }
}

static void test_capture(void)
{
    char buf[10];
    for (unsigned cap = 0; cap <= 8; cap++) {
        for (unsigned length = 0; length <= 10; length++) {
            memset(buf, 0xa5, sizeof(buf));
            uint32_t used = 0, limit = cap ? cap - 1 : 0;
            CHECK(shell_capture_append(buf, &used, cap, "0123456789", length) == (length <= limit));
            CHECK(used == (length < limit ? length : limit));
            CHECK(!memcmp(buf, "0123456789", used));
            for (unsigned i = used; i < sizeof(buf); i++) CHECK((uint8_t)buf[i] == 0xa5);
        }
    }
    uint32_t used = 0;
    CHECK(shell_capture_append(buf, &used, 8, "abc", 3));
    CHECK(shell_capture_append(buf, &used, 8, "defg", 4));
    CHECK(!shell_capture_append(buf, &used, 8, "h", 1));
    CHECK(used == 7 && !memcmp(buf, "abcdefg", 7));
    CHECK(!shell_capture_append(NULL, &used, 8, "x", 1));
    used = 9;
    CHECK(!shell_capture_append(buf, &used, 8, "x", 1) && used == 9);
}

int main(void)
{
    test_files();
    test_capture();
    printf("shell redirection: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
