/*
 * OsitoK v0.1 - Master include header
 * Bare-metal preemptive kernel for ESP8266
 */
#ifndef OSITO_H
#define OSITO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "kernel/types.h"
#include "kernel/config.h"
#include "hw/esp8266_regs.h"
#include "hw/esp8266_iomux.h"
#include "hw/esp8266_rom.h"

/* Version */
#define OSITO_VERSION_MAJOR  0
#define OSITO_VERSION_MINOR  1
#define OSITO_VERSION_STRING "0.1"

#ifdef __cplusplus
}
#endif

#endif /* OSITO_H */
