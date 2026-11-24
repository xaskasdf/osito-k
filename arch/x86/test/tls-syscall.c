/* tls-syscall.c — reproduce xino-cod's exact failing TLS path.
 *
 * xino-cod crashes at `mov rax, fs:[0]` (CR2=0) right after calling
 * syscall(530) (kernel inference). The fs:[0] read is musl writing errno:
 * a raw syscall that returns negative makes the musl wrapper do
 *   errno = -ret;  // → __errno_location() → __pthread_self() → fs:[0]
 * so if the live FS base was lost across the syscall, errno write faults.
 *
 * This binary isolates that path with NO C++/replxx/network noise:
 *   1. basic __thread access (TLS up?)
 *   2. errno write via a deliberately-failing ordinary syscall (close(-1))
 *   3. errno write via syscall(530) inference (the exact xino-cod trigger);
 *      530 returns negative here (no/!=expected args) → musl writes errno.
 *   4. repeat after a sched_yield-heavy call so any mid-syscall FS clobber
 *      has a chance to surface.
 *
 * Build (Mac, gcc14 musl cross):
 *   ~/ok-ported/toolchain/gcc14-install-v6/bin/x86_64-linux-musl-gcc \
 *     -static -no-pie -O2 tls-syscall.c -o tls-syscall
 *
 * Expected on OsitoK (TLS contract intact): prints
 *   tls ok
 *   close errno=<n>
 *   inf errno=<n>
 *   done
 * A crash at any `errno =` line means the FS base was lost by that point.
 */
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>

static void puts2(const char *s) { write(1, s, strlen(s)); }

static void put_errno(const char *tag)
{
    /* read errno (TLS) — this is the fs:[0]-based access that faults */
    int e = errno;
    char buf[64];
    int n = 0;
    while (*tag) buf[n++] = *tag++;
    buf[n++] = '=';
    /* tiny itoa */
    char tmp[12]; int t = 0;
    if (e < 0) { buf[n++] = '-'; e = -e; }
    if (e == 0) tmp[t++] = '0';
    while (e) { tmp[t++] = (char)('0' + e % 10); e /= 10; }
    while (t) buf[n++] = tmp[--t];
    buf[n++] = '\n';
    write(1, buf, n);
}

int main(void)
{
    /* 1. basic TLS */
    static __thread int marker;
    marker = 7;
    if (marker == 7) puts2("tls ok\n");

    /* 2. errno via ordinary failing syscall */
    errno = 0;
    if (close(-1) < 0) put_errno("close errno");

    /* 3. errno via syscall(530) inference — the exact xino-cod trigger.
     * Pass benign/garbage args; the kernel returns a negative errno
     * (e.g. -EINVAL/-EFAULT/-ENODEV) without running a real generation. */
    errno = 0;
    long r = syscall(530, (long)0, (long)0, (long)0, (long)0, (long)0);
    if (r < 0) {
        errno = (int)-r;
        put_errno("inf errno");
    }

    /* 4. a second round-trip to catch a delayed/yield-path FS clobber */
    errno = 0;
    if (close(-1) < 0) put_errno("close2 errno");

    puts2("done\n");
    return 0;
}
