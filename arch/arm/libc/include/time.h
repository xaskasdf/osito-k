/* time.h -- Osito-K AArch64 libc. There is no RTC: CLOCK_REALTIME counts
 * from the phone's boot, as CLOCK_MONOTONIC does. */
#ifndef _TIME_H
#define _TIME_H

#include <sys/types.h>

#define CLOCK_REALTIME      0
#define CLOCK_MONOTONIC     1
#define CLOCKS_PER_SEC      1000000L

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

int clock_gettime(int clk, struct timespec *ts);
time_t time(time_t *t);
clock_t clock(void);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif
