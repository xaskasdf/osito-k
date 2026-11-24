/*
 * OsitoK - ADC driver
 *
 * Reads the ESP8266 SAR ADC (TOUT / A0) without SDK.
 *
 * Without full PHY init (register_chipv6_phy), the SAR ADC has a
 * very compressed 11-bit range. Measured calibration on Wemos D1:
 *   A0=GND  → 11-bit ~402
 *   A0=3.3V → 11-bit ~472
 *
 * We map this 70-unit range linearly to 0-1023.
 */

#include "drivers/adc.h"
#include "drivers/uart.h"

extern "C" {

#define SAR(n)  REG32(0x60000D00 + (n) * 4)
#define ADC_CLK_DIV 8

/* Calibration: 11-bit values at known voltages on A0 */
#define ADC_CAL_ZERO  398   /* 11-bit at A0=GND (with margin) */
#define ADC_CAL_MAX   474   /* 11-bit at A0=3.3V (with margin) */
#define ADC_CAL_SPAN  (ADC_CAL_MAX - ADC_CAL_ZERO)  /* 76 */

void adc_init(void)
{
    /* Connect SAR clock to PLL 80MHz (bits 25-26 of IO_RTC_4) */
    REG32(0x60000710) |= 0x06000000u;

    /* ROM SAR init: configures internal I2C bus for SAR ADC */
    rom_sar_init();

    /* RF I2C config */
    rom_i2c_writeReg_Mask(98, 1, 3, 7, 4, 15);

    /* Enable peripheral clocks in DPORT */
    REG32(0x3FF00018) |= 0x038f0000u;

    /* HDRF analog config */
    REG32(0x600005e8) |= 0x01800000u;

    /* Configure SAR clock divider and timing */
    SAR(20) = (SAR(20) & 0xFFFF00FFu) | ((uint32_t)ADC_CLK_DIV << 8);

    SAR(21) = (SAR(21) & 0xFF000000u) |
              (ADC_CLK_DIV * 5 +
               ((ADC_CLK_DIV - 1) << 16) +
               ((ADC_CLK_DIV - 1) << 8) - 1);

    SAR(22) = (SAR(22) & 0xFF000000u) |
              (ADC_CLK_DIV * 11 +
               ((ADC_CLK_DIV * 3 - 1) << 8) +
               ((ADC_CLK_DIV * 11 - 1) << 16) - 1);

    /* Clear win_cnt bits [4:2] — single sample */
    SAR(20) &= 0xFFFFFFE1u;

    /* Enable SAR conversion engine (en_test bit) */
    rom_i2c_writeReg_Mask(108, 2, 0, 5, 5, 1);

    /* TOUT mode on (bit 21) */
    SAR(23) |= (1u << 21);

    /* Wait idle */
    while ((SAR(20) >> 24) & 0x07)
        ;

    /* Dummy conversion to prime */
    SAR(20) |= (1u << 1);
    ets_delay_us(100);
    while ((SAR(20) >> 24) & 0x07)
        ;
}

/*
 * Read one raw 11-bit sample from the SAR.
 */
static uint16_t adc_read_11bit(void)
{
    /* Trigger: clear bit 1, then set it (rising edge starts conversion) */
    SAR(20) &= 0xFFFFFFE1u;
    SAR(20) |= (1u << 1);
    ets_delay_us(100);

    uint32_t timeout = 5000;
    while (((SAR(20) >> 24) & 0x07) && --timeout)
        ;

    return (~SAR(32)) & 0x7FFu;
}

uint16_t adc_read(void)
{
    uint16_t raw = adc_read_11bit();

    /*
     * Calibrated linear mapping:
     *   11-bit 398 (A0=GND) → 0
     *   11-bit 474 (A0=3.3V) → 1023
     *   scale = (raw - 398) * 1023 / 76 ≈ (raw - 398) * 863 >> 6
     */
    int32_t v = (int32_t)raw - ADC_CAL_ZERO;
    if (v < 0) v = 0;
    uint32_t scaled = ((uint32_t)v * 863u) >> 6;
    if (scaled > 1023) scaled = 1023;

    return (uint16_t)scaled;
}

void adc_debug(void)
{
    uart_puts("11bit x5: ");
    for (int i = 0; i < 5; i++) {
        if (i) uart_puts(" ");
        uart_put_dec(adc_read_11bit());
    }
    uart_puts("\n");

    uart_puts("scaled x5: ");
    for (int i = 0; i < 5; i++) {
        if (i) uart_puts(" ");
        uart_put_dec(adc_read());
    }
    uart_puts("\n");
}

} /* extern "C" */
