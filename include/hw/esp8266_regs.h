/*
 * OsitoK - ESP8266 hardware register definitions
 *
 * References:
 *   - ESP8266 Technical Reference Manual (Espressif)
 *   - esp8266-re / nosdk8266 projects
 */
#ifndef ESP8266_REGS_H
#define ESP8266_REGS_H

#include "kernel/types.h"

/* Volatile register access macros */
#define REG32(addr)       (*(volatile uint32_t *)(addr))
#define REG16(addr)       (*(volatile uint16_t *)(addr))
#define REG8(addr)        (*(volatile uint8_t  *)(addr))

/* ======== DPORT registers (0x3FF00000) ======== */
#define DPORT_BASE        0x3FF00000

/* Edge interrupt enable (bit 0 = timer FRC1, bit 1 = timer FRC2) */
#define DPORT_EDGE_INT_ENABLE  REG32(DPORT_BASE + 0x004)
#define DPORT_EDGE_INT_TIMER1  (1 << 1)

/* CPU clock: bit 0 = CPU at 160MHz when set */
#define DPORT_CPU_CLK     REG32(DPORT_BASE + 0x014)

/* ======== UART0 registers (0x60000000) ======== */
#define UART0_BASE        0x60000000

#define UART0_FIFO        REG32(UART0_BASE + 0x00)
#define UART0_INT_RAW     REG32(UART0_BASE + 0x04)
#define UART0_INT_ST      REG32(UART0_BASE + 0x08)
#define UART0_INT_ENA     REG32(UART0_BASE + 0x0C)
#define UART0_INT_CLR     REG32(UART0_BASE + 0x10)
#define UART0_CLKDIV      REG32(UART0_BASE + 0x14)
#define UART0_AUTOBAUD    REG32(UART0_BASE + 0x18)
#define UART0_STATUS      REG32(UART0_BASE + 0x1C)
#define UART0_CONF0       REG32(UART0_BASE + 0x20)
#define UART0_CONF1       REG32(UART0_BASE + 0x24)
#define UART0_LOWPULSE    REG32(UART0_BASE + 0x28)
#define UART0_HIGHPULSE   REG32(UART0_BASE + 0x2C)
#define UART0_RXD_CNT     REG32(UART0_BASE + 0x30)

/* UART interrupt bits */
#define UART_RXFIFO_FULL_INT   (1 << 0)
#define UART_TXFIFO_EMPTY_INT  (1 << 1)
#define UART_RXFIFO_OVF_INT    (1 << 4)
#define UART_FRM_ERR_INT       (1 << 3)
#define UART_RXFIFO_TOUT_INT   (1 << 8)

/* UART CONF0 bits */
#define UART_CONF0_8N1    0x0000001C  /* 8 data bits, no parity, 1 stop bit */
#define UART_TXFIFO_RST   (1 << 18)
#define UART_RXFIFO_RST   (1 << 17)

/* UART CONF1 bits */
#define UART_RX_TOUT_EN   (1 << 31)

/* UART STATUS field masks */
#define UART_TXFIFO_CNT_MASK   0x000000FF
#define UART_RXFIFO_CNT_MASK   0x000000FF
#define UART_TXFIFO_CNT_SHIFT  16
#define UART_RXFIFO_CNT_SHIFT  0

/* ======== Timer (FRC1/FRC2) registers (0x60000600) ======== */
#define TIMER_BASE        0x60000600

/* FRC1 (Timer 1) */
#define FRC1_LOAD         REG32(TIMER_BASE + 0x00)
#define FRC1_COUNT        REG32(TIMER_BASE + 0x04)
#define FRC1_CTRL         REG32(TIMER_BASE + 0x08)
#define FRC1_INT_CLR      REG32(TIMER_BASE + 0x0C)

/* FRC1_CTRL bits */
#define FRC1_CTRL_EN      (1 << 7)     /* Timer enable */
#define FRC1_CTRL_AUTOLOAD (1 << 6)    /* Auto-reload on alarm */
#define FRC1_CTRL_DIV1    (0 << 2)     /* Prescaler /1 */
#define FRC1_CTRL_DIV16   (1 << 2)     /* Prescaler /16 */
#define FRC1_CTRL_DIV256  (3 << 2)     /* Prescaler /256 */
#define FRC1_CTRL_INT_EDGE (0 << 0)    /* Edge interrupt */
#define FRC1_CTRL_INT_LEVEL (1 << 0)   /* Level interrupt */

/* FRC2 (Timer 2) */
#define FRC2_LOAD         REG32(TIMER_BASE + 0x20)
#define FRC2_COUNT        REG32(TIMER_BASE + 0x24)
#define FRC2_CTRL         REG32(TIMER_BASE + 0x28)
#define FRC2_INT_CLR      REG32(TIMER_BASE + 0x2C)
#define FRC2_ALARM        REG32(TIMER_BASE + 0x30)

/* ======== GPIO registers (0x60000300) ======== */
#define GPIO_BASE         0x60000300

#define GPIO_OUT          REG32(GPIO_BASE + 0x00)
#define GPIO_OUT_W1TS     REG32(GPIO_BASE + 0x04)  /* Set bits */
#define GPIO_OUT_W1TC     REG32(GPIO_BASE + 0x08)  /* Clear bits */
#define GPIO_ENABLE       REG32(GPIO_BASE + 0x0C)
#define GPIO_ENABLE_W1TS  REG32(GPIO_BASE + 0x10)
#define GPIO_ENABLE_W1TC  REG32(GPIO_BASE + 0x14)
#define GPIO_IN           REG32(GPIO_BASE + 0x18)
#define GPIO_STATUS       REG32(GPIO_BASE + 0x1C)  /* Interrupt status */
#define GPIO_STATUS_W1TC  REG32(GPIO_BASE + 0x24)  /* Clear interrupt */

/* GPIO pin configuration registers (one per pin, 0-15) */
#define GPIO_PIN(n)       REG32(GPIO_BASE + 0x28 + (n) * 4)

/* ======== WDT (Watchdog Timer) registers (0x60000900) ======== */
#define WDT_BASE          0x60000900

#define WDT_CTRL          REG32(WDT_BASE + 0x00)
#define WDT_REG1          REG32(WDT_BASE + 0x04)
#define WDT_REG2          REG32(WDT_BASE + 0x08)
#define WDT_FEED          REG32(WDT_BASE + 0x14)

/* ======== RTC registers (0x60000700) ======== */
#define RTC_BASE          0x60000700

#define RTC_CNTL          REG32(RTC_BASE + 0x00)
#define RTC_GPIO_OUT      REG32(RTC_BASE + 0x68)
#define RTC_GPIO_ENABLE   REG32(RTC_BASE + 0x74)
#define RTC_GPIO_IN       REG32(RTC_BASE + 0x8C)
#define RTC_GPIO_CONF     REG32(RTC_BASE + 0x90)

/* ======== Xtensa interrupt numbers ======== */
#define INUM_SLC       1   /* SLC */
#define INUM_SPI       2   /* SPI */
#define INUM_GPIO      4   /* GPIO */
#define INUM_UART      5   /* UART */
#define INUM_MAX       6   /* ??? */
#define INUM_SOFT      7   /* Software interrupt */
#define INUM_WDT       8   /* WDT */
#define INUM_TIMER_FRC1 9  /* FRC1 timer */

/* Interrupt enable/disable macros */
#define INT_ENABLE(n)   do { uint32_t m = 1 << (n); \
    __asm__ volatile("rsr.intenable a2; or a2, a2, %0; wsr.intenable a2" :: "r"(m) : "a2"); \
} while(0)

#define INT_DISABLE(n)  do { uint32_t m = ~(1 << (n)); \
    __asm__ volatile("rsr.intenable a2; and a2, a2, %0; wsr.intenable a2" :: "r"(m) : "a2"); \
} while(0)

#endif /* ESP8266_REGS_H */
