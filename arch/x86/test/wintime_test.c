#include <stdint.h>
#include <stdio.h>

#define WINTIME_HOST_TEST 1
#include "../win32/wintime.h"

static uint64_t fake_utc_100ns;

uint64_t ntp_get_utc_100ns(void)
{
    return fake_utc_100ns;
}

static int failures;

#define CHECK(condition) do {                                             \
    if (!(condition)) {                                                   \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                #condition);                                              \
        failures++;                                                       \
    }                                                                     \
} while (0)

static void check_epoch_boundaries(void)
{
    WINTIME_CALENDAR calendar;
    CHECK(wintime_filetime_to_calendar(0, &calendar) == 0);
    CHECK(calendar.year == 1601 && calendar.month == 1 && calendar.day == 1);
    CHECK(calendar.day_of_week == 1); /* Monday */

    CHECK(wintime_filetime_to_calendar(WINTIME_UNIX_EPOCH_FILETIME,
                                       &calendar) == 0);
    CHECK(calendar.year == 1970 && calendar.month == 1 && calendar.day == 1);
    CHECK(calendar.day_of_week == 4); /* Thursday */
}

static void check_roundtrip(void)
{
    WINTIME_CALENDAR input = {
        .year = 2024, .month = 2, .day = 29,
        .hour = 23, .minute = 59, .second = 59, .millisecond = 999
    };
    WINTIME_CALENDAR output;
    uint64_t filetime;
    int64_t unix_seconds;
    CHECK(wintime_calendar_to_filetime(&input, &filetime) == 0);
    CHECK(wintime_filetime_to_calendar(filetime, &output) == 0);
    CHECK(output.year == input.year && output.month == input.month);
    CHECK(output.day == input.day && output.hour == input.hour);
    CHECK(output.minute == input.minute && output.second == input.second);
    CHECK(output.millisecond == input.millisecond);
    CHECK(output.day_of_year == 59);
    CHECK(wintime_calendar_to_unix(&input, &unix_seconds) == 0);
    CHECK(unix_seconds == 1709251199LL);
}

static void check_validation(void)
{
    WINTIME_CALENDAR invalid = {
        .year = 2100, .month = 2, .day = 29
    };
    uint64_t filetime;
    CHECK(wintime_calendar_to_filetime(&invalid, &filetime) < 0);
    invalid.year = 2000;
    CHECK(wintime_calendar_to_filetime(&invalid, &filetime) == 0);
    invalid.month = 13;
    CHECK(wintime_calendar_to_filetime(&invalid, &filetime) < 0);
}

static void check_live_source(void)
{
    fake_utc_100ns = 1704067200ULL * WINTIME_TICKS_PER_SECOND;
    CHECK(wintime_now_unix_seconds() == 1704067200LL);
    CHECK(wintime_now_filetime() == WINTIME_UNIX_EPOCH_FILETIME +
                                    fake_utc_100ns);
}

int main(void)
{
    check_epoch_boundaries();
    check_roundtrip();
    check_validation();
    check_live_source();
    if (failures)
        return 1;
    puts("wintime: all tests passed");
    return 0;
}
