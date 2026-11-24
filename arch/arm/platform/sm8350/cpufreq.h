/*
 * cpufreq.h -- frequency domains for thermal.c (cpufreq.c)
 */

#ifndef OSITO_SM8350_CPUFREQ_H
#define OSITO_SM8350_CPUFREQ_H

#include <stdint.h>

/* 0 = cpu0-3 (A55), 1 = cpu4-6 (A78), 2 = cpu7 (X1) */
#define CPUFREQ_DOMAINS 3

/* Move a domain's thermal cap one level down (dir < 0, below what it's set
 * to now) or up (dir > 0). Returns the new cap level, -1 for a domain that
 * can't be capped. Domains 1 and 2 share a voltage rail: cap them together. */
int      cpufreq_cap_step(int dom, int dir);
int      cpufreq_capped(int dom);       /* running below the level asked for */
uint32_t cpufreq_cap_khz(int dom);      /* what it's set to run at */
uint32_t cpufreq_now_khz(int dom);      /* what the hardware reports */

#endif
