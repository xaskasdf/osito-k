#ifndef OSITOK_DOS_TIME_H
#define OSITOK_DOS_TIME_H

#include "../include/types.h"

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} dos_calendar_t;

struct dos_vm;

bool dos_unix_to_calendar(uint64_t unix_time, dos_calendar_t *calendar);
bool dos_calendar_to_unix(const dos_calendar_t *calendar,
                          uint64_t *unix_time);
bool dos_calendar_day_of_week(const dos_calendar_t *calendar,
                              uint8_t *day_of_week);
bool dos_clock_get(const struct dos_vm *vm, dos_calendar_t *calendar,
                   uint8_t *hundredths);
bool dos_clock_set(struct dos_vm *vm, const dos_calendar_t *calendar,
                   uint8_t hundredths);
bool dos_pack_datetime(uint64_t unix_time, uint16_t *date,
                       uint16_t *time);
bool dos_unpack_datetime(uint16_t date, uint16_t time,
                         uint64_t *unix_time);

#endif
