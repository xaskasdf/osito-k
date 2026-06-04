/* hello-tls.c — minimal TLS contract check for static-musl on OsitoK.
 *
 * Exercises the single-threaded x86-64 Linux ABI TLS contract:
 *   musl _start → __init_tls → arch_prctl(ARCH_SET_FS, tcb)
 *   then any `__thread` access dereferences fs:<offset>.
 *
 * Build (Mac, gcc14 musl cross):
 *   ~/ok-ported/toolchain/gcc14-install-v6/bin/x86_64-linux-musl-gcc \
 *     -static -no-pie -O2 hello-tls.c -o hello-tls
 *
 * Expected on OsitoK: prints one letter A..H then a newline and exits 0.
 * If it crashes at `mov ...,fs:...` with CR2 small, the basic static-musl
 * TLS contract is broken (would also break cc1 — so this should PASS).
 */
#include <unistd.h>

__thread int counter = 42;

int main(void)
{
    counter++;
    char buf[2] = { (char)('A' + (counter & 7)), '\n' };
    write(1, buf, 2);
    return 0;
}
