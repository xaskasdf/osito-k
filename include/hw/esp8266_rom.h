/*
 * OsitoK - ESP8266 ROM function prototypes
 *
 * These functions live in mask ROM and are linked via rom_functions.ld.
 * We declare them here so C/C++ code can call them.
 */
#ifndef ESP8266_ROM_H
#define ESP8266_ROM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "kernel/types.h"

/* printf (ROM implementation, limited format support) */
int ets_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Memory functions */
void *ets_memset(void *dst, int c, size_t n);
void *ets_memcpy(void *dst, const void *src, size_t n);
void *ets_memmove(void *dst, const void *src, size_t n);

/* String functions */
size_t ets_strlen(const char *s);
int ets_strcmp(const char *s1, const char *s2);
int ets_strncmp(const char *s1, const char *s2, size_t n);
char *ets_strcpy(char *dst, const char *src);
char *ets_strncpy(char *dst, const char *src, size_t n);
char *ets_strstr(const char *haystack, const char *needle);

/* Cache (flash-mapped memory) control */
void Cache_Read_Enable(uint32_t odd_even, uint32_t mb_count, uint32_t autoload);
void Cache_Read_Disable(void);

/* Delay */
void ets_delay_us(uint32_t us);

/* Character output */
void ets_install_putc1(void (*putc)(char));

/* Interrupt management */
void ets_isr_attach(int inum, void (*handler)(void *), void *arg);
void ets_isr_unmask(uint32_t mask);
void ets_isr_mask(uint32_t mask);
void ets_intr_lock(void);
void ets_intr_unlock(void);

/* UART baud rate */
void uart_div_modify(int uart_no, uint32_t div);

/* System reset */
void software_reset(void) __attribute__((noreturn));

/* WDT */
void ets_wdt_disable(void);
void ets_wdt_enable(void);

/* RTC SAR ADC I2C master (used for analog read) */
uint8_t rom_i2c_readReg(uint8_t block, uint8_t host_id, uint8_t reg_add);
void rom_i2c_writeReg(uint8_t block, uint8_t host_id, uint8_t reg_add, uint8_t data);
uint8_t rom_i2c_readReg_Mask(uint8_t block, uint8_t host_id, uint8_t reg_add,
                              uint8_t Msb, uint8_t Lsb);
void rom_i2c_writeReg_Mask(uint8_t block, uint8_t host_id, uint8_t reg_add,
                             uint8_t Msb, uint8_t Lsb, uint8_t indata);

/* SAR ADC initialization (configures internal I2C bus for SAR) */
void rom_sar_init(void);

/* SPI flash */
int SPIRead(uint32_t addr, void *dst, uint32_t size);
int SPIWrite(uint32_t addr, const void *src, uint32_t size);
int SPIEraseSector(int sector);

#ifdef __cplusplus
}
#endif

#endif /* ESP8266_ROM_H */
