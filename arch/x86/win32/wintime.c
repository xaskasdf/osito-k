#include "wintime.h"

#ifndef WINTIME_HOST_TEST
extern uint64_t ntp_get_utc_100ns(void);
#else
extern uint64_t ntp_get_utc_100ns(void);
#endif

#define WINTIME_SECONDS_PER_DAY 86400ULL

static int wintime_is_leap_year(int year)
{
    return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}

static int wintime_days_in_month(int year, int month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < 1 || month > 12)
        return 0;
    if (month == 2 && wintime_is_leap_year(year))
        return 29;
    return days[month - 1];
}

/* Days since 1970-01-01. The civil-date algorithms are valid for the full
 * SYSTEMTIME range and avoid tables tied to a particular build year. */
static int64_t wintime_days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned year_of_era = (unsigned)(year - era * 400);
    unsigned adjusted_month = (unsigned)((int)month + (month > 2 ? -3 : 9));
    unsigned day_of_year =
        (153U * adjusted_month + 2U) / 5U
        + day - 1U;
    unsigned day_of_era = year_of_era * 365U + year_of_era / 4U
                         - year_of_era / 100U + day_of_year;
    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static void wintime_civil_from_days(int64_t days, int *year,
                                    unsigned *month, unsigned *day)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned day_of_era = (unsigned)(days - era * 146097);
    unsigned year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U
         - day_of_era / 146096U) / 365U;
    int y = (int)year_of_era + (int)(era * 400);
    unsigned day_of_year = day_of_era -
        (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    unsigned month_prime = (5U * day_of_year + 2U) / 153U;
    unsigned d = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
    unsigned m = month_prime < 10U ? month_prime + 3U : month_prime - 9U;
    y += m <= 2U;
    *year = y;
    *month = m;
    *day = d;
}

static uint16_t wintime_day_of_year(int year, int month, int day)
{
    uint16_t result = 0;
    for (int current = 1; current < month; current++)
        result = (uint16_t)(result + wintime_days_in_month(year, current));
    return (uint16_t)(result + day - 1);
}

uint64_t wintime_now_filetime(void)
{
    uint64_t unix_time = ntp_get_utc_100ns();
    if (!unix_time || unix_time > UINT64_MAX - WINTIME_UNIX_EPOCH_FILETIME)
        return 0;
    return WINTIME_UNIX_EPOCH_FILETIME + unix_time;
}

int64_t wintime_now_unix_seconds(void)
{
    uint64_t unix_time = ntp_get_utc_100ns();
    return unix_time ? (int64_t)(unix_time / WINTIME_TICKS_PER_SECOND) : -1;
}

int wintime_filetime_to_calendar(uint64_t filetime,
                                 WINTIME_CALENDAR *calendar)
{
    if (!calendar)
        return -1;

    uint64_t total_seconds = filetime / WINTIME_TICKS_PER_SECOND;
    uint64_t days_since_1601 = total_seconds / WINTIME_SECONDS_PER_DAY;
    uint64_t second_of_day = total_seconds % WINTIME_SECONDS_PER_DAY;
    int64_t days_since_unix = (int64_t)days_since_1601 - 134774;
    int year;
    unsigned month;
    unsigned day;
    wintime_civil_from_days(days_since_unix, &year, &month, &day);
    if (year < 1601 || year > 30827)
        return -1;

    int64_t weekday = (days_since_unix + 4) % 7;
    if (weekday < 0)
        weekday += 7;

    calendar->year = (uint16_t)year;
    calendar->month = (uint16_t)month;
    calendar->day_of_week = (uint16_t)weekday;
    calendar->day = (uint16_t)day;
    calendar->hour = (uint16_t)(second_of_day / 3600ULL);
    calendar->minute = (uint16_t)((second_of_day % 3600ULL) / 60ULL);
    calendar->second = (uint16_t)(second_of_day % 60ULL);
    calendar->millisecond = (uint16_t)
        ((filetime % WINTIME_TICKS_PER_SECOND) / 10000ULL);
    calendar->day_of_year = wintime_day_of_year(year, (int)month, (int)day);
    return 0;
}

int wintime_calendar_to_filetime(const WINTIME_CALENDAR *calendar,
                                 uint64_t *filetime)
{
    if (!calendar || !filetime || calendar->year < 1601 ||
        calendar->year > 30827 || calendar->month < 1 ||
        calendar->month > 12 || calendar->day < 1 ||
        calendar->day > wintime_days_in_month(calendar->year,
                                               calendar->month) ||
        calendar->hour > 23 || calendar->minute > 59 ||
        calendar->second > 59 || calendar->millisecond > 999)
        return -1;

    int64_t days_since_1601 =
        wintime_days_from_civil(calendar->year, calendar->month,
                                calendar->day) -
        wintime_days_from_civil(1601, 1, 1);
    if (days_since_1601 < 0)
        return -1;

    uint64_t seconds = (uint64_t)days_since_1601 * WINTIME_SECONDS_PER_DAY
                     + (uint64_t)calendar->hour * 3600ULL
                     + (uint64_t)calendar->minute * 60ULL
                     + calendar->second;
    if (seconds > UINT64_MAX / WINTIME_TICKS_PER_SECOND)
        return -1;
    uint64_t ticks = seconds * WINTIME_TICKS_PER_SECOND;
    uint64_t milliseconds = (uint64_t)calendar->millisecond * 10000ULL;
    if (ticks > UINT64_MAX - milliseconds)
        return -1;
    *filetime = ticks + milliseconds;
    return 0;
}

int wintime_unix_to_calendar(int64_t unix_seconds,
                             WINTIME_CALENDAR *calendar)
{
    if (!calendar || unix_seconds < -11644473600LL)
        return -1;
    uint64_t filetime;
    if (unix_seconds >= 0) {
        uint64_t seconds = (uint64_t)unix_seconds;
        if (seconds >
            (UINT64_MAX - WINTIME_UNIX_EPOCH_FILETIME) /
                WINTIME_TICKS_PER_SECOND)
            return -1;
        filetime = WINTIME_UNIX_EPOCH_FILETIME +
                   seconds * WINTIME_TICKS_PER_SECOND;
    } else {
        uint64_t before_epoch = (uint64_t)(-unix_seconds) *
                                WINTIME_TICKS_PER_SECOND;
        if (before_epoch > WINTIME_UNIX_EPOCH_FILETIME)
            return -1;
        filetime = WINTIME_UNIX_EPOCH_FILETIME - before_epoch;
    }
    return wintime_filetime_to_calendar(filetime, calendar);
}

int wintime_calendar_to_unix(const WINTIME_CALENDAR *calendar,
                             int64_t *unix_seconds)
{
    uint64_t filetime;
    if (!unix_seconds || wintime_calendar_to_filetime(calendar, &filetime) < 0)
        return -1;
    if (filetime >= WINTIME_UNIX_EPOCH_FILETIME) {
        *unix_seconds = (int64_t)
            ((filetime - WINTIME_UNIX_EPOCH_FILETIME) /
             WINTIME_TICKS_PER_SECOND);
    } else {
        *unix_seconds = -(int64_t)
            ((WINTIME_UNIX_EPOCH_FILETIME - filetime) /
             WINTIME_TICKS_PER_SECOND);
    }
    return 0;
}
