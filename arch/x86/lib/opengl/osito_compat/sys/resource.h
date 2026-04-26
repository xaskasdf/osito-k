/* OsitoK no-op shim for <sys/resource.h>.
 *
 * Mesa util/u_queue.c (and a few other Linux paths) gate a getpriority/
 * setpriority section on `#if defined(__linux__)`. gcc:12 in our build
 * docker auto-defines __linux__, so the section fires and references this
 * header.  The functions are wrapped at use-site behind another check
 * we never trigger; the references just need to compile.
 *
 * This file exists in osito_compat/sys/ so the existing -I forest picks
 * it up before any system header.  W4.7 T0 (TLS fix) re-builds util/
 * objects, exposing this previously-cached compile unit.
 */
#ifndef _OSITO_SYS_RESOURCE_H
#define _OSITO_SYS_RESOURCE_H 1

/* Glibc constants we map to no-ops. */
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

#define RLIMIT_CPU       0
#define RLIMIT_FSIZE     1
#define RLIMIT_DATA      2
#define RLIMIT_STACK     3
#define RLIMIT_CORE      4
#define RLIMIT_RSS       5
#define RLIMIT_NOFILE    7
#define RLIMIT_AS        9

typedef unsigned long rlim_t;
struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

static inline int getpriority(int which, int who) {
    (void)which; (void)who; return 0;
}
static inline int setpriority(int which, int who, int prio) {
    (void)which; (void)who; (void)prio; return 0;
}
static inline int getrlimit(int resource, struct rlimit *rlim) {
    (void)resource; if (rlim) { rlim->rlim_cur = (rlim_t)-1; rlim->rlim_max = (rlim_t)-1; }
    return 0;
}
static inline int setrlimit(int resource, const struct rlimit *rlim) {
    (void)resource; (void)rlim; return 0;
}

#endif /* _OSITO_SYS_RESOURCE_H */
