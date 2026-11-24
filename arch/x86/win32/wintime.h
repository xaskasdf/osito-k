#ifndef OSITOK_WIN32_WINTIME_H
#define OSITOK_WIN32_WINTIME_H

#ifdef WINTIME_HOST_TEST
#include <stdint.h>
#else
#include "../include/types.h"
#endif

#define WINTIME_TICKS_PER_SECOND 10000000ULL
#define WINTIME_UNIX_EPOCH_FILETIME 116444736000000000ULL

typedef struct {
    uint16_t year;
    uint16_t month;
    uint16_t day_of_week;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
    uint16_t millisecond;
    uint16_t day_of_year;
} WINTIME_CALENDAR;

uint64_t wintime_now_filetime(void);
int64_t wintime_now_unix_seconds(void);
int wintime_filetime_to_calendar(uint64_t filetime,
                                 WINTIME_CALENDAR *calendar);
int wintime_calendar_to_filetime(const WINTIME_CALENDAR *calendar,
                                 uint64_t *filetime);
int wintime_unix_to_calendar(int64_t unix_seconds,
                             WINTIME_CALENDAR *calendar);
int wintime_calendar_to_unix(const WINTIME_CALENDAR *calendar,
                             int64_t *unix_seconds);

#endif
