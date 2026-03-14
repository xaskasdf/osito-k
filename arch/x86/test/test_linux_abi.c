/*
 * test_linux_abi.c — Linux ABI compatibility test for OsitoK
 *
 * Tests core syscalls and C library functionality to verify that
 * OsitoK's Linux syscall layer works with real glibc/musl binaries.
 *
 * Build (glibc static):
 *   gcc -static -o test_linux_abi.elf test_linux_abi.c
 *
 * Build (musl static):
 *   musl-gcc -static -o test_linux_abi.elf test_linux_abi.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do { \
    if (cond) { \
        printf("  PASS: %s\n", name); \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s\n", name); \
        tests_failed++; \
    } \
} while(0)

/* 1. Basic I/O */
static void test_stdio(void)
{
    printf("[TEST] stdio\n");
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "hello %d %s", 42, "world");
    TEST("snprintf format", n == 14 && strcmp(buf, "hello 42 world") == 0);
    TEST("strlen", strlen("osito") == 5);
    TEST("strcmp equal", strcmp("abc", "abc") == 0);
    TEST("strcmp less", strcmp("abc", "abd") < 0);
}

/* 2. Memory allocation */
static void test_malloc(void)
{
    printf("[TEST] malloc/free\n");
    void *p = malloc(4096);
    TEST("malloc non-null", p != NULL);
    if (p) {
        memset(p, 0xAA, 4096);
        TEST("memset+check", ((unsigned char *)p)[2048] == 0xAA);
        free(p);
        TEST("free (no crash)", 1);
    }
}

/* 3. mmap */
static void test_mmap(void)
{
    printf("[TEST] mmap/munmap\n");
    void *m = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    TEST("mmap non-null", m != MAP_FAILED);
    if (m != MAP_FAILED) {
        memset(m, 0x55, 4096);
        TEST("mmap write+read", ((unsigned char *)m)[100] == 0x55);
        int r = munmap(m, 4096);
        TEST("munmap", r == 0);
    }
}

/* 4. uname */
static void test_uname(void)
{
    printf("[TEST] uname\n");
    struct utsname u;
    int r = uname(&u);
    TEST("uname returns 0", r == 0);
    printf("  sysname:  %s\n", u.sysname);
    printf("  nodename: %s\n", u.nodename);
    printf("  release:  %s\n", u.release);
    printf("  version:  %s\n", u.version);
    printf("  machine:  %s\n", u.machine);
    TEST("machine is x86_64", strcmp(u.machine, "x86_64") == 0);
}

/* 5. getpid/getuid */
static void test_ids(void)
{
    printf("[TEST] process IDs\n");
    pid_t pid = getpid();
    uid_t uid = getuid();
    gid_t gid = getgid();
    TEST("getpid > 0", pid > 0);
    printf("  pid=%d uid=%d gid=%d\n", pid, uid, gid);
}

/* 6. time */
static void test_time(void)
{
    printf("[TEST] time\n");
    time_t t = time(NULL);
    TEST("time returns value", t >= 0);
    struct timespec ts;
    int r = clock_gettime(CLOCK_MONOTONIC, &ts);
    TEST("clock_gettime", r == 0);
    printf("  time=%ld clock=%ld.%09ld\n", (long)t, (long)ts.tv_sec, ts.tv_nsec);
}

/* 7. File I/O */
static void test_file_io(void)
{
    printf("[TEST] file I/O\n");
    /* Try writing to stdout directly via write() */
    const char msg[] = "  write() works\n";
    ssize_t w = write(1, msg, sizeof(msg) - 1);
    TEST("write to stdout", w == (ssize_t)(sizeof(msg) - 1));

    /* getcwd */
    char cwd[256];
    char *p = getcwd(cwd, sizeof(cwd));
    TEST("getcwd", p != NULL);
    if (p) printf("  cwd: %s\n", cwd);
}

/* 8. Environment */
static void test_env(void)
{
    printf("[TEST] environment\n");
    /* Just verify getenv doesn't crash */
    char *path = getenv("PATH");
    printf("  PATH=%s\n", path ? path : "(null)");
    TEST("getenv no crash", 1);
}

/* 9. Argv */
static void test_argv(int argc, char **argv)
{
    printf("[TEST] argc/argv\n");
    TEST("argc >= 1", argc >= 1);
    TEST("argv[0] non-null", argv[0] != NULL);
    printf("  argc=%d argv[0]=%s\n", argc, argv[0] ? argv[0] : "(null)");
}

int main(int argc, char **argv)
{
    printf("=== OsitoK Linux ABI Test ===\n\n");

    test_argv(argc, argv);
    test_stdio();
    test_malloc();
    test_mmap();
    test_uname();
    test_ids();
    test_time();
    test_file_io();
    test_env();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
