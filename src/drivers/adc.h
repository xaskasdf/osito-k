/*
 * OsitoK - ADC driver for ESP8266
 *
 * The ESP8266 has a single 10-bit SAR ADC (TOUT / A0).
 * On Wemos D1, A0 has a voltage divider (220K/100K) giving ~0-3.2V range.
 * Reads 0-1023 via ROM I2C functions that control the RTC-SAR block.
 */
#ifndef OSITO_ADC_H
#define OSITO_ADC_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize ADC (configure SAR mux for TOUT pin) */
void adc_init(void);

/* Read ADC value (0-1023). Blocks ~200us for conversion. */
uint16_t adc_read(void);

/* Debug: dump I2C registers for SAR ADC (both hosts) */
void adc_debug(void);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_ADC_H */
