#include "dos_time.h"
#include "dos_types.h"

#define DOS_MIN_UNIX_TIME 315532800ULL
#define DOS_MAX_UNIX_TIME 4354819198ULL
#define DOS_CLOCK_MAX_UNIX_TIME 4102444799ULL

extern uint64_t idt_get_ticks(void);
extern uint64_t ntp_get_utc_100ns(void);

static bool dos_is_leap_year(unsigned year)
{
    return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

static unsigned dos_days_in_month(unsigned year, unsigned month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < 1U || month > 12U) return 0;
    if (month == 2U && dos_is_leap_year(year)) return 29U;
    return days[month - 1U];
}

static void dos_civil_from_days(int64_t days, int *year,
                                unsigned *month, unsigned *day)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned day_of_era = (unsigned)(days - era * 146097);
    unsigned year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U -
         day_of_era / 146096U) / 365U;
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

static int64_t dos_days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2U;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned year_of_era = (unsigned)(year - era * 400);
    unsigned month_prime = month > 2U ? month - 3U : month + 9U;
    unsigned day_of_year = (153U * month_prime + 2U) / 5U + day - 1U;
    unsigned day_of_era = year_of_era * 365U + year_of_era / 4U -
                          year_of_era / 100U + day_of_year;
    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

bool dos_unix_to_calendar(uint64_t unix_time, dos_calendar_t *calendar)
{
    if (!calendar || unix_time > 253402300799ULL) return false;
    uint64_t unix_days = unix_time / 86400ULL;
    uint32_t seconds_of_day = (uint32_t)(unix_time % 86400ULL);
    int year;
    unsigned month;
    unsigned day;
    dos_civil_from_days((int64_t)unix_days, &year, &month, &day);
    if (year < 1970 || year > 9999) return false;
    calendar->year = (uint16_t)year;
    calendar->month = (uint8_t)month;
    calendar->day = (uint8_t)day;
    calendar->hour = (uint8_t)(seconds_of_day / 3600U);
    calendar->minute = (uint8_t)((seconds_of_day % 3600U) / 60U);
    calendar->second = (uint8_t)(seconds_of_day % 60U);
    return true;
}

bool dos_calendar_to_unix(const dos_calendar_t *calendar,
                          uint64_t *unix_time)
{
    if (!calendar || !unix_time || calendar->year < 1970U ||
        calendar->year > 9999U || calendar->month < 1U ||
        calendar->month > 12U || calendar->day < 1U ||
        calendar->day > dos_days_in_month(calendar->year, calendar->month) ||
        calendar->hour > 23U || calendar->minute > 59U ||
        calendar->second > 59U)
        return false;

    int64_t days = dos_days_from_civil(calendar->year, calendar->month,
                                       calendar->day);
    if (days < 0) return false;
    *unix_time = (uint64_t)days * 86400ULL +
                 (uint64_t)calendar->hour * 3600ULL +
                 (uint64_t)calendar->minute * 60ULL + calendar->second;
    return true;
}

bool dos_calendar_day_of_week(const dos_calendar_t *calendar,
                              uint8_t *day_of_week)
{
    uint64_t unix_time;
    if (!day_of_week || !dos_calendar_to_unix(calendar, &unix_time))
        return false;

    /* 1970-01-01 was Thursday; DOS numbers Sunday as zero. */
    *day_of_week = (uint8_t)((unix_time / 86400ULL + 4ULL) % 7ULL);
    return true;
}

static uint64_t dos_clock_centiseconds(const dos_vm_t *vm)
{
    uint64_t value;
    if (vm->clock_override) {
        uint64_t now = idt_get_ticks();
        uint64_t elapsed = now >= vm->clock_base_ticks
            ? now - vm->clock_base_ticks : 0;
        value = vm->clock_base_centiseconds + elapsed;
    } else {
        value = ntp_get_utc_100ns() / 100000ULL;
    }

    const uint64_t minimum = DOS_MIN_UNIX_TIME * 100ULL;
    const uint64_t maximum = DOS_CLOCK_MAX_UNIX_TIME * 100ULL + 99ULL;
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

bool dos_clock_get(const struct dos_vm *vm, dos_calendar_t *calendar,
                   uint8_t *hundredths)
{
    if (!vm || !calendar || !hundredths) return false;
    uint64_t centiseconds = dos_clock_centiseconds(vm);
    *hundredths = (uint8_t)(centiseconds % 100ULL);
    return dos_unix_to_calendar(centiseconds / 100ULL, calendar);
}

bool dos_clock_set(struct dos_vm *vm, const dos_calendar_t *calendar,
                   uint8_t hundredths)
{
    uint64_t unix_time;
    if (!vm || !calendar || calendar->year < 1980U ||
        calendar->year > 2099U || hundredths > 99U ||
        !dos_calendar_to_unix(calendar, &unix_time))
        return false;

    vm->clock_base_centiseconds = unix_time * 100ULL + hundredths;
    vm->clock_base_ticks = idt_get_ticks();
    vm->clock_override = true;
    return true;
}

bool dos_pack_datetime(uint64_t unix_time, uint16_t *date, uint16_t *time)
{
    if (!date || !time) return false;
    if (unix_time < DOS_MIN_UNIX_TIME) unix_time = DOS_MIN_UNIX_TIME;
    if (unix_time > DOS_MAX_UNIX_TIME) unix_time = DOS_MAX_UNIX_TIME;

    dos_calendar_t calendar;
    if (!dos_unix_to_calendar(unix_time, &calendar) ||
        calendar.year < 1980U || calendar.year > 2107U)
        return false;
    *date = (uint16_t)(((calendar.year - 1980U) << 9) |
                       ((uint16_t)calendar.month << 5) | calendar.day);
    *time = (uint16_t)(((uint16_t)calendar.hour << 11) |
                       ((uint16_t)calendar.minute << 5) |
                       (calendar.second / 2U));
    return true;
}

bool dos_unpack_datetime(uint16_t date, uint16_t time,
                         uint64_t *unix_time)
{
    dos_calendar_t calendar;
    calendar.year = (uint16_t)(1980U + ((date >> 9) & 0x7FU));
    calendar.month = (uint8_t)((date >> 5) & 0x0FU);
    calendar.day = (uint8_t)(date & 0x1FU);
    calendar.hour = (uint8_t)((time >> 11) & 0x1FU);
    calendar.minute = (uint8_t)((time >> 5) & 0x3FU);
    calendar.second = (uint8_t)((time & 0x1FU) * 2U);
    return dos_calendar_to_unix(&calendar, unix_time);
}
