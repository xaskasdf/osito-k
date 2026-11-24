/* W4.8 — minimal signal.h shim (C build only — C++ build uses host signal.h). */
#ifndef OSITO_SIGNAL_H
#define OSITO_SIGNAL_H 1
#ifdef __cplusplus
#include_next <signal.h>
#else

typedef int sig_atomic_t;
typedef unsigned long sigset_t;

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGABRT   6
#define SIGFPE    8
#define SIGKILL   9
#define SIGSEGV   11
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

struct sigaction {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, void *, void *);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

extern int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
extern int sigemptyset(sigset_t *set);
extern int sigfillset(sigset_t *set);
extern int sigaddset(sigset_t *set, int sig);
extern void (*signal(int sig, void (*handler)(int)))(int);

#endif /* __cplusplus */
#endif
