#ifndef OSITOK_THERMAL_H
#define OSITOK_THERMAL_H

#include "../include/types.h"

typedef struct {
    uint64_t raw;
    bool readable, valid;
    uint8_t delta_c;
} thermal_sensor_t;

typedef struct {
    bool intel, msr, dts, package, hwp, est, tm;
    bool target_valid, misc_valid, hwp_valid, hwp_request_valid;
    uint8_t target_c;
    uint32_t apic_id;
    uint64_t target_raw, misc, hwp_enable, hwp_request;
    thermal_sensor_t core, pkg;
} thermal_snapshot_t;

void thermal_read(thermal_snapshot_t *snapshot);
void thermal_dump(void);
void thermal_sample_dump(void);
void thermal_msr_selftest(void);

#endif
