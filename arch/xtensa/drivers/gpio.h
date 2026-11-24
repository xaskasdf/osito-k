/*
 * OsitoK - GPIO driver
 *
 * Supports GPIO 0-16 with IOMUX auto-configuration.
 * GPIO16 uses RTC registers (no pull-up, no interrupt support).
 * GPIOs 6-11 are SPI flash — avoid unless you know what you're doing.
 *
 * Wemos D1 pin mapping:
 *   D0=GPIO16  D1=GPIO5   D2=GPIO4   D3=GPIO0
 *   D4=GPIO2   D5=GPIO14  D6=GPIO12  D7=GPIO13  D8=GPIO15
 */
#ifndef OSITO_GPIO_H
#define OSITO_GPIO_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GPIO_MODE_INPUT   0
#define GPIO_MODE_OUTPUT  1

#define GPIO_LOW   0
#define GPIO_HIGH  1

/* Configure pin as GPIO input or output (sets IOMUX automatically) */
void gpio_mode(uint8_t pin, uint8_t mode);

/* Set output value (0 or 1) */
void gpio_write(uint8_t pin, uint8_t val);

/* Read pin value (0 or 1) */
uint8_t gpio_read(uint8_t pin);

/* Toggle output */
void gpio_toggle(uint8_t pin);

/* Enable/disable internal pull-up (GPIO 0-15 only) */
void gpio_pullup(uint8_t pin, bool enable);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_GPIO_H */
