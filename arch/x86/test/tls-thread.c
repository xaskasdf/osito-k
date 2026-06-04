/* tls-thread.c — per-thread TLS contract check (the cc1-vs-xino-cod delta).
 *
 * cc1 (works) is single-threaded; xino-cod links libstdc++ built with
 * --enable-threads=posix and may spawn worker threads. The kernel's
 * clone(CLONE_THREAD|CLONE_SETTLS) path sets the new thread's fs_base from
 * the r8 `tls` arg; if a thread ever runs user code (or writes errno)
 * before/without its FS base being applied, it faults at fs:[0].
 *
 * Each worker: sets a __thread var, forces an errno write (close(-1)),
 * then prints its id. A crash on any worker means thread TLS is broken;
 * the main thread continuing fine while a worker dies pinpoints the
 * clone-thread FS path.
 *
 * Build (Mac, gcc14 musl cross):
 *   ~/ok-ported/toolchain/gcc14-install-v6/bin/x86_64-linux-musl-gcc \
 *     -static -no-pie -O2 -pthread tls-thread.c -o tls-thread
 */
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

__thread int tls_id;

static void *worker(void *arg)
{
    tls_id = (int)(long)arg;          /* per-thread TLS write */
    errno = 0;
    close(-1);                        /* errno write → fs:[0] read */
    char c = (char)('0' + (tls_id & 7));
    write(1, &c, 1);
    return (void *)(long)(tls_id + errno);
}

int main(void)
{
    pthread_t t[4];
    for (long i = 0; i < 4; i++)
        pthread_create(&t[i], 0, worker, (void *)i);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], 0);
    write(1, "\nthreads ok\n", 12);
    return 0;
}
