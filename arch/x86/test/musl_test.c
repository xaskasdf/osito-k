/*
 * musl_test.c — test musl libc on OsitoK
 *
 * Compiled with musl-gcc: exercises stdio, string, memory, and time functions
 * that rely on the new X-MUSL syscalls (arch_prctl, set_tid_address,
 * clock_gettime, getrandom, etc.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
    int pass = 0, fail = 0;

    printf("=== musl libc test ===\n");

    /* T1: printf works (proves musl stdio init) */
    printf("T1 printf: PASS\n");
    pass++;

    /* T2: strlen/strcmp */
    const char *s = "OsitoK";
    if (strlen(s) == 6 && strcmp(s, "OsitoK") == 0) {
        printf("T2 string ops: PASS\n");
        pass++;
    } else {
        printf("T2 string ops: FAIL\n");
        fail++;
    }

    /* T3: malloc/free */
    char *p = malloc(128);
    if (p) {
        strcpy(p, "Hello from musl heap!");
        if (strcmp(p, "Hello from musl heap!") == 0) {
            printf("T3 malloc: PASS (%s)\n", p);
            pass++;
        } else {
            printf("T3 malloc: FAIL\n");
            fail++;
        }
        free(p);
    } else {
        printf("T3 malloc: FAIL (NULL)\n");
        fail++;
    }

    /* T4: mmap anonymous */
    void *mp = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mp != MAP_FAILED) {
        int *ip = (int *)mp;
        ip[0] = 0x12345678;
        if (ip[0] == 0x12345678) {
            printf("T4 mmap: PASS (addr=%p)\n", mp);
            pass++;
        } else {
            printf("T4 mmap: FAIL (readback)\n");
            fail++;
        }
        munmap(mp, 4096);
    } else {
        printf("T4 mmap: FAIL (MAP_FAILED)\n");
        fail++;
    }

    /* T5: getpid */
    pid_t pid = getpid();
    if (pid > 0) {
        printf("T5 getpid: PASS (pid=%d)\n", pid);
        pass++;
    } else {
        printf("T5 getpid: FAIL (pid=%d)\n", pid);
        fail++;
    }

    /* T6: clock_gettime */
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        printf("T6 clock_gettime: PASS (sec=%ld ns=%ld)\n",
               (long)ts.tv_sec, (long)ts.tv_nsec);
        pass++;
    } else {
        printf("T6 clock_gettime: FAIL\n");
        fail++;
    }

    /* T7: open/read/close /dev/zero */
    int fd = open("/dev/zero", O_RDONLY);
    if (fd >= 0) {
        char buf[16];
        buf[0] = 0xFF;
        ssize_t n = read(fd, buf, 16);
        close(fd);
        if (n == 16 && buf[0] == 0) {
            printf("T7 /dev/zero: PASS\n");
            pass++;
        } else {
            printf("T7 /dev/zero: FAIL (n=%zd b0=%d)\n", n, buf[0]);
            fail++;
        }
    } else {
        printf("T7 /dev/zero open: FAIL\n");
        fail++;
    }

    /* T8: snprintf */
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "answer=%d pi=%.2f", 42, 3.14);
    if (len > 0 && strstr(buf, "answer=42") != NULL) {
        printf("T8 snprintf: PASS (%s)\n", buf);
        pass++;
    } else {
        printf("T8 snprintf: FAIL (%s)\n", buf);
        fail++;
    }

    /* T9: argc/argv */
    if (argc >= 1) {
        printf("T9 argc/argv: PASS (argc=%d argv[0]=%s)\n",
               argc, argv[0] ? argv[0] : "(null)");
        pass++;
    } else {
        printf("T9 argc/argv: FAIL\n");
        fail++;
    }

    printf("=== %d/%d passed ===\n", pass, pass + fail);
    return fail;
}
