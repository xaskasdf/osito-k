/*
 * vfs_test.c — test VFS layer (/dev and /proc virtual files)
 */

extern int printf(const char *fmt, ...);
extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern long read(int fd, void *buf, unsigned long count);
extern long write(int fd, const void *buf, unsigned long count);

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

int main(void)
{
    int pass = 0, fail = 0;
    char buf[256];
    long n;

    printf("=== VFS test ===\n");

    /* T1: /dev/null read → EOF */
    int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
        n = read(fd, buf, 64);
        if (n == 0) { printf("T1 /dev/null read=EOF: PASS\n"); pass++; }
        else { printf("T1 /dev/null read: FAIL (n=%ld)\n", n); fail++; }
        close(fd);
    } else { printf("T1 /dev/null open: FAIL\n"); fail++; }

    /* T2: /dev/null write → discarded */
    fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
        n = write(fd, "hello", 5);
        if (n == 5) { printf("T2 /dev/null write: PASS\n"); pass++; }
        else { printf("T2 /dev/null write: FAIL (n=%ld)\n", n); fail++; }
        close(fd);
    } else { printf("T2 /dev/null open: FAIL\n"); fail++; }

    /* T3: /dev/zero read → zeros */
    fd = open("/dev/zero", O_RDONLY);
    if (fd >= 0) {
        buf[0] = 0xFF; buf[1] = 0xFF;
        n = read(fd, buf, 16);
        if (n == 16 && buf[0] == 0 && buf[1] == 0) {
            printf("T3 /dev/zero: PASS\n"); pass++;
        } else {
            printf("T3 /dev/zero: FAIL (n=%ld, b0=%d)\n", n, buf[0]); fail++;
        }
        close(fd);
    } else { printf("T3 /dev/zero open: FAIL\n"); fail++; }

    /* T4: /dev/urandom read → non-zero bytes (likely) */
    fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        for (int i = 0; i < 32; i++) buf[i] = 0;
        n = read(fd, buf, 32);
        int nonzero = 0;
        for (int i = 0; i < 32; i++) if (buf[i] != 0) nonzero++;
        if (n == 32 && nonzero > 0) {
            printf("T4 /dev/urandom: PASS (nonzero=%d/32)\n", nonzero); pass++;
        } else {
            printf("T4 /dev/urandom: FAIL (n=%ld nz=%d)\n", n, nonzero); fail++;
        }
        close(fd);
    } else { printf("T4 /dev/urandom open: FAIL\n"); fail++; }

    /* T5: /proc/self/status */
    fd = open("/proc/self/status", O_RDONLY);
    if (fd >= 0) {
        n = read(fd, buf, 255);
        if (n > 0) {
            buf[n] = 0;
            printf("T5 /proc/self/status: PASS (%ld bytes)\n", n);
            printf("  %s", buf);
            pass++;
        } else {
            printf("T5 /proc/self/status: FAIL (n=%ld)\n", n); fail++;
        }
        close(fd);
    } else { printf("T5 /proc/self/status open: FAIL\n"); fail++; }

    /* T6: /dev/console write → should print */
    fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        const char *msg = "T6 /dev/console: ";
        n = write(fd, msg, 17);
        if (n == 17) { printf("PASS\n"); pass++; }
        else { printf("FAIL\n"); fail++; }
        close(fd);
    } else { printf("T6 /dev/console open: FAIL\n"); fail++; }

    /* T7: nonexistent /dev path */
    fd = open("/dev/nonexistent", O_RDONLY);
    if (fd < 0) { printf("T7 /dev/nonexistent: PASS (ENOENT)\n"); pass++; }
    else { printf("T7 /dev/nonexistent: FAIL (fd=%d)\n", fd); close(fd); fail++; }

    printf("=== %d/%d passed ===\n", pass, pass + fail);
    return fail;
}
