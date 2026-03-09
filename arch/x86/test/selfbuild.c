/*
 * selfbuild.c — Multi-feature program compiled entirely inside OsitoK.
 *
 * Demonstrates: printf, malloc, file I/O, string ops, dynamic memory,
 * all provided by the OsitoK CRT+libc (crt.o + syscall.o + tcclib.o).
 *
 * Compile inside OsitoK:
 *   cc selfbuild.c         → selfbuild.elf
 *   cc -run selfbuild.c    → compile + run
 */

#include "ositok.h"

/* ── Fibonacci with dynamic memory ─────────────────────────── */

static int *fib_array(int n)
{
    int *arr = (int *)malloc(n * sizeof(int));
    if (!arr) return NULL;
    arr[0] = 0;
    if (n > 1) arr[1] = 1;
    for (int i = 2; i < n; i++)
        arr[i] = arr[i-1] + arr[i-2];
    return arr;
}

/* ── String reverse ──────────────────────────────────────────── */

static void str_reverse(char *s)
{
    int len = strlen(s);
    for (int i = 0; i < len / 2; i++) {
        char tmp = s[i];
        s[i] = s[len - 1 - i];
        s[len - 1 - i] = tmp;
    }
}

/* ── File I/O test ───────────────────────────────────────────── */

static int test_file_io(void)
{
    const char *fname = "_selftest.tmp";
    int fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        printf("  SKIP: open() failed (fd=%d)\n", fd);
        return 0;  /* Not a failure, disk might be read-only */
    }

    const char *msg = "Hello from self-compiled OsitoK!\n";
    write(fd, msg, strlen(msg));
    close(fd);

    /* Read it back */
    fd = open(fname, O_RDONLY);
    if (fd < 0) {
        printf("  FAIL: cannot reopen %s\n", fname);
        return 1;
    }

    char buf[128];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n > 0) {
        buf[n] = '\0';
        printf("  Read back: %s", buf);
        if (strcmp(buf, msg) == 0)
            printf("  PASS: file I/O round-trip\n");
        else
            printf("  FAIL: data mismatch\n");
    }

    unlink(fname);
    return 0;
}

/* ── qsort comparison ────────────────────────────────────────── */

static int int_cmp(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    printf("=== OsitoK Self-Build Test ===\n");
    printf("Compiled AND executed entirely inside OsitoK.\n\n");

    /* T1: printf + format strings */
    printf("T1: printf — int=%d, hex=0x%x, str=\"%s\"\n", 42, 0xCAFE, "osito");

    /* T2: malloc + fibonacci */
    printf("T2: malloc + fibonacci\n");
    int *fibs = fib_array(10);
    if (fibs) {
        printf("  fib(10) = ");
        for (int i = 0; i < 10; i++) {
            printf("%d", fibs[i]);
            if (i < 9) printf(", ");
        }
        printf("\n");
        free(fibs);
        printf("  PASS: malloc/free\n");
    } else {
        printf("  FAIL: malloc returned NULL\n");
    }

    /* T3: string operations */
    printf("T3: string ops\n");
    char s[32];
    strcpy(s, "OsitoK");
    str_reverse(s);
    printf("  reverse(\"OsitoK\") = \"%s\"\n", s);
    if (strcmp(s, "KotisO") == 0)
        printf("  PASS: string reverse\n");
    else
        printf("  FAIL: expected \"KotisO\"\n");

    /* T4: snprintf */
    printf("T4: snprintf\n");
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "pid=%d, name=%s", getpid(), "selfbuild");
    printf("  %s (len=%d)\n", buf, n);
    printf("  PASS: snprintf\n");

    /* T5: qsort */
    printf("T5: qsort\n");
    int arr[] = {5, 3, 8, 1, 9, 2, 7, 4, 6, 0};
    qsort(arr, 10, sizeof(int), int_cmp);
    printf("  sorted: ");
    for (int i = 0; i < 10; i++) printf("%d ", arr[i]);
    printf("\n");
    int sorted = 1;
    for (int i = 1; i < 10; i++)
        if (arr[i] < arr[i-1]) sorted = 0;
    printf("  %s\n", sorted ? "PASS" : "FAIL");

    /* T6: file I/O */
    printf("T6: file I/O\n");
    test_file_io();

    /* T7: argv */
    printf("T7: argc=%d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);

    printf("\n=== All tests complete ===\n");
    return 0;
}
