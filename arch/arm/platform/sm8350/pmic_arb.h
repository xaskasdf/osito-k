/*
 * pmic_arb.h -- PMIC register access over the SPMI PMIC arbiter (pmic_arb.c)
 */

#ifndef OSITO_SM8350_PMIC_ARB_H
#define OSITO_SM8350_PMIC_ARB_H

#include <stdint.h>

#define PMIC_ERR_VERSION    (-1)    /* not a v5 arbiter */
#define PMIC_ERR_NO_CHANNEL (-2)    /* no APID (for writes: none owned by EE 0) */
#define PMIC_ERR_STATUS     (-3)    /* the arbiter reported failure / denied */
#define PMIC_ERR_TIMEOUT    (-4)

/* addr = peripheral << 8 | register, as in the device tree (0x7148) */
int pmic_read(uint8_t sid, uint16_t addr, uint8_t *val);
int pmic_write(uint8_t sid, uint16_t addr, uint8_t val);
uint32_t pmic_arb_version(void);
void pmic_list(uint8_t sid);    /* every peripheral it maps on that PMIC */

#endif
