/*
 * OsitoK - ESP8266 ROM function addresses
 *
 * These functions are burned into the ESP8266 mask ROM and available
 * at fixed addresses. We PROVIDE() them so code can call them without
 * needing the SDK.
 *
 * Source: Espressif ESP8266 ROM function table
 */

/* ets_printf - ROM printf (limited, but works for early boot) */
PROVIDE ( ets_printf = 0x400024cc );

/* ets_memset / ets_memcpy / ets_memmove */
PROVIDE ( ets_memset = 0x400018a4 );
PROVIDE ( ets_memcpy = 0x400018b4 );
PROVIDE ( ets_memmove = 0x400018c4 );

/* ets_strlen / ets_strcmp / ets_strncmp / ets_strcpy / ets_strncpy */
PROVIDE ( ets_strlen = 0x400020c0 );
PROVIDE ( ets_strcmp = 0x40002aa8 );
PROVIDE ( ets_strncmp = 0x40002ab8 );
PROVIDE ( ets_strcpy = 0x40002a88 );
PROVIDE ( ets_strncpy = 0x40002a98 );
PROVIDE ( ets_strstr = 0x40002ac8 );

/* Cache_Read_Enable - enables flash-mapped memory at 0x40200000+ */
PROVIDE ( Cache_Read_Enable = 0x40004678 );

/* Cache_Read_Disable */
PROVIDE ( Cache_Read_Disable = 0x400047f0 );

/* ets_delay_us - busy-wait delay in microseconds */
PROVIDE ( ets_delay_us = 0x40002ecc );

/* ets_install_putc1 - set character output function */
PROVIDE ( ets_install_putc1 = 0x4000258c );

/* ets_isr_attach - attach interrupt handler */
PROVIDE ( ets_isr_attach = 0x4000259c );

/* ets_isr_unmask / ets_isr_mask */
PROVIDE ( ets_isr_unmask = 0x400025a0 );
PROVIDE ( ets_isr_mask = 0x400025a4 );

/* ets_intr_lock / ets_intr_unlock */
PROVIDE ( ets_intr_lock = 0x400025a8 );
PROVIDE ( ets_intr_unlock = 0x400025ac );

/* SPIRead / SPIWrite - SPI flash access */
PROVIDE ( SPIRead = 0x40004b1c );
PROVIDE ( SPIWrite = 0x40004a4c );
PROVIDE ( SPIEraseSector = 0x40004a00 );

/* uart_div_modify - change UART baud rate */
PROVIDE ( uart_div_modify = 0x400039d8 );

/* software_reset */
PROVIDE ( software_reset = 0x4000278c );

/* ets_wdt_disable / ets_wdt_enable */
PROVIDE ( ets_wdt_disable = 0x400030f0 );
PROVIDE ( ets_wdt_enable = 0x40002fa0 );

/* mem* from ROM (C standard library) */
PROVIDE ( memset = 0x400018a4 );
PROVIDE ( memcpy = 0x400018b4 );
PROVIDE ( memmove = 0x400018c4 );
PROVIDE ( memcmp = 0x400018d4 );

/* rom_i2c_readReg / rom_i2c_writeReg / _Mask variants - RTC SAR ADC I2C master */
PROVIDE ( rom_i2c_readReg = 0x40007268 );
PROVIDE ( rom_i2c_writeReg = 0x400072d8 );
PROVIDE ( rom_i2c_readReg_Mask = 0x40007208 );
PROVIDE ( rom_i2c_writeReg_Mask = 0x40007258 );

/* rom_sar_init - initializes SAR ADC I2C bus configuration */
PROVIDE ( rom_sar_init = 0x40006738 );

/* str* from ROM */
PROVIDE ( strlen = 0x400020c0 );
PROVIDE ( strcmp = 0x40002aa8 );
PROVIDE ( strncmp = 0x40002ab8 );
PROVIDE ( strcpy = 0x40002a88 );
PROVIDE ( strncpy = 0x40002a98 );
