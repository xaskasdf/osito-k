/*
 * thermal.h -- temperature sensors and the CPU thermal limit (thermal.c)
 */

#ifndef OSITO_SM8350_THERMAL_H
#define OSITO_SM8350_THERMAL_H

void thermal_start(void);   /* the limit's task; platform_start_tasks */
void thermal_summary(void); /* each cluster's MHz and hottest sensor, one line */

#endif
