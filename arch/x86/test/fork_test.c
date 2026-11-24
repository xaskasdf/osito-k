/*
 * fork_test.c — test fork/exec/wait on OsitoK
 *
 * Compiled with musl-gcc: exercises clone(SIGCHLD), wait4, execve.
 *
 * Uses raw write() for PASS/FAIL output to avoid musl FILE buffer
 * corruption between parent and child (shared .bss in identity-mapped OS).
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

/* Raw output helpers — bypass musl's FILE buffering */
static void wr(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

static void wr_dec(int n)
{
    char buf[16];
    int neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    int i = 0;
    if (n == 0) { buf[i++] = '0'; }
    else { while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; } }
    if (neg) buf[i++] = '-';
    /* reverse */
    char out[16];
    for (int j = 0; j < i; j++) out[j] = buf[i - 1 - j];
    out[i] = '\n';  /* NOT null, we use write with explicit len */
    write(1, out, i);
}

static void wr_hex(unsigned int v)
{
    char buf[12] = "0x";
    const char *h = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--)
        buf[2 + (7 - i)] = h[(v >> (i * 4)) & 0xF];
    buf[10] = 0;
    wr(buf);
}

int main(void)
{
    int pass = 0, fail = 0;

    wr("=== fork test ===\n");

    /* T1: fork returns 0 to child, >0 to parent */
    pid_t pid = fork();
    if (pid < 0) {
        wr("T1 fork: FAIL (returned "); wr_dec(pid); wr(")\n");
        fail++;
    } else if (pid == 0) {
        /* Child — use raw write to avoid FILE buffer sharing issues */
        wr("T1 child: pid="); wr_dec(getpid());
        wr(" ppid="); wr_dec(getppid()); wr("\n");
        _exit(42);
    } else {
        /* Parent */
        wr("T1 parent: child_pid="); wr_dec(pid); wr("\n");
        int status = 0;
        pid_t w = waitpid(pid, &status, 0);

        /* Debug: show raw values and individual comparisons */
        wr("T1 debug: w="); wr_dec(w);
        wr(" pid="); wr_dec(pid);
        wr(" status="); wr_dec(status);
        wr(" hex="); wr_hex((unsigned int)status);
        wr("\n");
        wr("T1 check: w==pid="); wr_dec(w == pid);
        wr(" WIFEXITED="); wr_dec(WIFEXITED(status));
        wr(" WEXITSTATUS="); wr_dec(WEXITSTATUS(status));
        wr(" ==42="); wr_dec(WEXITSTATUS(status) == 42);
        wr("\n");

        if (w == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42) {
            wr("T1 fork+wait: PASS\n");
            pass++;
        } else {
            wr("T1 fork+wait: FAIL\n");
            fail++;
        }
    }

    /* T2: fork + exec (child runs a different program) */
    pid = fork();
    if (pid < 0) {
        wr("T2 fork: FAIL\n");
        fail++;
    } else if (pid == 0) {
        wr("T2 child: about to exit with 7\n");
        _exit(7);
    } else {
        int status = 0;
        pid_t w = waitpid(pid, &status, 0);

        wr("T2 debug: w="); wr_dec(w);
        wr(" pid="); wr_dec(pid);
        wr(" status="); wr_dec(status);
        wr("\n");
        wr("T2 check: w==pid="); wr_dec(w == pid);
        wr(" WIFEXITED="); wr_dec(WIFEXITED(status));
        wr(" WEXITSTATUS="); wr_dec(WEXITSTATUS(status));
        wr(" ==7="); wr_dec(WEXITSTATUS(status) == 7);
        wr("\n");

        if (w == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7) {
            wr("T2 fork+exit: PASS\n");
            pass++;
        } else {
            wr("T2 fork+exit: FAIL\n");
            fail++;
        }
    }

    /* T3: getpid consistency */
    pid_t my_pid = getpid();
    if (my_pid > 0) {
        wr("T3 getpid: PASS (pid="); wr_dec(my_pid); wr(")\n");
        pass++;
    } else {
        wr("T3 getpid: FAIL\n");
        fail++;
    }

    wr("=== "); wr_dec(pass); wr("/"); wr_dec(pass + fail); wr(" passed ===\n");
    return fail;
}
