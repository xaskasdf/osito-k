/*
 * OsitoK - ESP8266 IOMUX (pin mux) register definitions
 *
 * Each GPIO pin has an IOMUX register that selects its function.
 * The IOMUX base is at 0x60000800.
 */
#ifndef ESP8266_IOMUX_H
#define ESP8266_IOMUX_H

#include "kernel/types.h"
#include "hw/esp8266_regs.h"

#define IOMUX_BASE        0x60000800

/* IOMUX config register (affects SPI flash pins) */
#define IOMUX_CONF        REG32(IOMUX_BASE + 0x00)

/* Individual pin registers.
 * Note: pin register offsets are NOT sequential with GPIO numbers.
 */
#define IOMUX_GPIO0       REG32(IOMUX_BASE + 0x34)  /* GPIO0 */
#define IOMUX_GPIO1       REG32(IOMUX_BASE + 0x18)  /* GPIO1 / U0TXD */
#define IOMUX_GPIO2       REG32(IOMUX_BASE + 0x38)  /* GPIO2 / U1TXD */
#define IOMUX_GPIO3       REG32(IOMUX_BASE + 0x14)  /* GPIO3 / U0RXD */
#define IOMUX_GPIO4       REG32(IOMUX_BASE + 0x3C)  /* GPIO4 */
#define IOMUX_GPIO5       REG32(IOMUX_BASE + 0x40)  /* GPIO5 */
#define IOMUX_GPIO6       REG32(IOMUX_BASE + 0x1C)  /* GPIO6 / SPI CLK */
#define IOMUX_GPIO7       REG32(IOMUX_BASE + 0x20)  /* GPIO7 / SPI MISO */
#define IOMUX_GPIO8       REG32(IOMUX_BASE + 0x24)  /* GPIO8 / SPI MOSI */
#define IOMUX_GPIO9       REG32(IOMUX_BASE + 0x28)  /* GPIO9 */
#define IOMUX_GPIO10      REG32(IOMUX_BASE + 0x2C)  /* GPIO10 */
#define IOMUX_GPIO11      REG32(IOMUX_BASE + 0x30)  /* GPIO11 / SPI CS */
#define IOMUX_GPIO12      REG32(IOMUX_BASE + 0x04)  /* GPIO12 / MTDI */
#define IOMUX_GPIO13      REG32(IOMUX_BASE + 0x08)  /* GPIO13 / MTCK */
#define IOMUX_GPIO14      REG32(IOMUX_BASE + 0x0C)  /* GPIO14 / MTMS */
#define IOMUX_GPIO15      REG32(IOMUX_BASE + 0x10)  /* GPIO15 / MTDO */

/* IOMUX function select bits */
#define IOMUX_FUNC_SHIFT  4
#define IOMUX_FUNC_MASK   (0x3 << IOMUX_FUNC_SHIFT)

/* Pull-up enable */
#define IOMUX_PULLUP      (1 << 7)

/* Function numbers for UART0 pins:
 *   GPIO1 (U0TXD): function 0
 *   GPIO3 (U0RXD): function 0
 */
#define IOMUX_FUNC0       (0 << IOMUX_FUNC_SHIFT)
#define IOMUX_FUNC1       (1 << IOMUX_FUNC_SHIFT)
#define IOMUX_FUNC2       (2 << IOMUX_FUNC_SHIFT)
#define IOMUX_FUNC3       (3 << IOMUX_FUNC_SHIFT)

/* Configure a pin to a specific function */
INLINE void iomux_set_function(volatile uint32_t *pin_reg, uint32_t func) {
    uint32_t val = *pin_reg;
    val &= ~IOMUX_FUNC_MASK;
    val |= (func & IOMUX_FUNC_MASK);
    *pin_reg = val;
}

/* Enable/disable pull-up on a pin */
INLINE void iomux_set_pullup(volatile uint32_t *pin_reg, bool enable) {
    if (enable)
        *pin_reg |= IOMUX_PULLUP;
    else
        *pin_reg &= ~IOMUX_PULLUP;
}

#endif /* ESP8266_IOMUX_H */
