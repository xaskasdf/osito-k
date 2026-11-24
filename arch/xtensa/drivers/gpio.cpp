/*
 * OsitoK - GPIO driver
 *
 * GPIO 0-15 use the normal GPIO peripheral (0x60000300).
 * GPIO 16 is special — routed through the RTC subsystem.
 *
 * Each pin's IOMUX register must be set to the correct function
 * to route it as GPIO. The function number varies per pin:
 *   Function 0: GPIO 0, 2, 4, 5
 *   Function 3: GPIO 1, 3, 6-15
 */

#include "drivers/gpio.h"

extern "C" {

/* IOMUX offset and GPIO function for pins 0-15 */
struct pin_mux {
    uint8_t iomux_off;
    uint8_t gpio_func;
};

static const pin_mux pin_map[16] = {
    { 0x34, 0 },  /*  0: D3 (flash button) */
    { 0x18, 3 },  /*  1: TX (UART0) */
    { 0x38, 0 },  /*  2: D4 (onboard LED) */
    { 0x14, 3 },  /*  3: RX (UART0) */
    { 0x3C, 0 },  /*  4: D2 */
    { 0x40, 0 },  /*  5: D1 */
    { 0x1C, 3 },  /*  6: SPI CLK  (flash!) */
    { 0x20, 3 },  /*  7: SPI MISO (flash!) */
    { 0x24, 3 },  /*  8: SPI MOSI (flash!) */
    { 0x28, 3 },  /*  9: SPI HD   (flash!) */
    { 0x2C, 3 },  /* 10: SPI WP   (flash!) */
    { 0x30, 3 },  /* 11: SPI CS   (flash!) */
    { 0x04, 3 },  /* 12: D6 */
    { 0x08, 3 },  /* 13: D7 */
    { 0x0C, 3 },  /* 14: D5 */
    { 0x10, 3 },  /* 15: D8 (boot select) */
};

/* ====== GPIO16 (RTC) helpers ====== */

static void gpio16_mode(uint8_t mode)
{
    RTC_GPIO_CONF &= ~1;   /* select GPIO function */
    if (mode == GPIO_MODE_OUTPUT)
        RTC_GPIO_ENABLE |= 1;
    else
        RTC_GPIO_ENABLE &= ~1;
}

static void gpio16_write(uint8_t val)
{
    if (val)
        RTC_GPIO_OUT |= 1;
    else
        RTC_GPIO_OUT &= ~1;
}

static uint8_t gpio16_read(void)
{
    return RTC_GPIO_IN & 1;
}

/* ====== Public API ====== */

void gpio_mode(uint8_t pin, uint8_t mode)
{
    if (pin > 16) return;

    if (pin == 16) {
        gpio16_mode(mode);
        return;
    }

    /* Set IOMUX to GPIO function */
    volatile uint32_t *iomux =
        (volatile uint32_t *)(IOMUX_BASE + pin_map[pin].iomux_off);
    iomux_set_function(iomux, pin_map[pin].gpio_func << IOMUX_FUNC_SHIFT);

    /* Set direction */
    if (mode == GPIO_MODE_OUTPUT)
        GPIO_ENABLE_W1TS = (1 << pin);
    else
        GPIO_ENABLE_W1TC = (1 << pin);
}

void gpio_write(uint8_t pin, uint8_t val)
{
    if (pin > 16) return;

    if (pin == 16) {
        gpio16_write(val);
        return;
    }

    if (val)
        GPIO_OUT_W1TS = (1 << pin);
    else
        GPIO_OUT_W1TC = (1 << pin);
}

uint8_t gpio_read(uint8_t pin)
{
    if (pin > 16) return 0;

    if (pin == 16)
        return gpio16_read();

    return (GPIO_IN >> pin) & 1;
}

void gpio_toggle(uint8_t pin)
{
    gpio_write(pin, !gpio_read(pin));
}

void gpio_pullup(uint8_t pin, bool enable)
{
    if (pin > 15) return;  /* GPIO16 has no IOMUX pull-up */

    volatile uint32_t *iomux =
        (volatile uint32_t *)(IOMUX_BASE + pin_map[pin].iomux_off);
    iomux_set_pullup(iomux, enable);
}

} /* extern "C" */
