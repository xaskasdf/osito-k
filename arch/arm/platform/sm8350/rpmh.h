/*
 * rpmh.h -- votes to the AOP through the apps RSC, and the command DB (rpmh.c)
 */

#ifndef OSITO_SM8350_RPMH_H
#define OSITO_SM8350_RPMH_H

#include <stdint.h>

#define RPMH_ERR_VERSION    (-1)    /* RSC isn't v2, or no command DB */
#define RPMH_ERR_NO_RES     (-2)    /* no such resource (or not a VRM) */
#define RPMH_ERR_BUSY       (-3)    /* both AMCs busy */
#define RPMH_ERR_TIMEOUT    (-4)
#define RPMH_ERR_NACK       (-5)    /* sent, but not acknowledged */

/* PMIC5 LDO modes (Linux qcom-rpmh-regulator.c) */
#define RPMH_LDO_LPM        4
#define RPMH_LDO_HPM        7

/* A resource's RPMh address from the command DB ("ldoc3"); 0 if found */
int cmd_db_addr(const char *name, uint32_t *addr);

/* One active-only write, waiting for the AOP's acknowledgement */
int rpmh_write(uint32_t addr, uint32_t data);

/* A regulator vote: voltage (mV) and mode, then enable; off sends only the
 * disable. Refuses resources the command DB doesn't list as VRMs. */
int rpmh_vrm_vote(const char *name, uint32_t mv, uint32_t mode, int on);

const char *rpmh_err(int err);

#endif
