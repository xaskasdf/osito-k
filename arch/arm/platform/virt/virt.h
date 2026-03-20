/*
 * virt.h -- QEMU virt machine MMIO addresses and helpers
 *
 * Reference: QEMU hw/arm/virt.c memory map
 */

#ifndef OSITO_VIRT_H
#define OSITO_VIRT_H

#include <stdint.h>

/* ========================================================================
 * PL011 UART (ARM PrimeCell UART)
 * ======================================================================== */
#define PL011_BASE              0x09000000UL

/* Register offsets */
#define PL011_DR                0x000   /* Data register */
#define PL011_RSR               0x004   /* Receive status */
#define PL011_FR                0x018   /* Flag register */
#define PL011_IBRD              0x024   /* Integer baud rate divisor */
#define PL011_FBRD              0x028   /* Fractional baud rate divisor */
#define PL011_LCR_H             0x02C   /* Line control register */
#define PL011_CR                0x030   /* Control register */
#define PL011_IFLS              0x034   /* Interrupt FIFO level select */
#define PL011_IMSC              0x038   /* Interrupt mask set/clear */
#define PL011_RIS               0x03C   /* Raw interrupt status */
#define PL011_MIS               0x040   /* Masked interrupt status */
#define PL011_ICR               0x044   /* Interrupt clear */

/* Flag register bits */
#define PL011_FR_TXFF           (1 << 5)    /* TX FIFO full */
#define PL011_FR_RXFE           (1 << 4)    /* RX FIFO empty */
#define PL011_FR_BUSY           (1 << 3)    /* UART busy */

/* Control register bits */
#define PL011_CR_UARTEN         (1 << 0)    /* UART enable */
#define PL011_CR_TXE            (1 << 8)    /* TX enable */
#define PL011_CR_RXE            (1 << 9)    /* RX enable */

/* ========================================================================
 * GICv3 (QEMU virt)
 * ======================================================================== */
#define GICD_BASE               0x08000000UL
#define GICR_BASE               0x080A0000UL

/* ========================================================================
 * Memory
 * ======================================================================== */
#define RAM_BASE                0x40000000UL
#define RAM_SIZE                0x20000000UL     /* 512 MB */

/* ========================================================================
 * MMIO helpers
 * ======================================================================== */

static inline uint32_t mmio_read32(uintptr_t addr) {
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write32(uintptr_t addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}

static inline uint64_t mmio_read64(uintptr_t addr) {
    return *(volatile uint64_t *)addr;
}

/* Barriers */
#define dsb()   __asm__ volatile("dsb sy" ::: "memory")
#define dmb()   __asm__ volatile("dmb sy" ::: "memory")
#define isb()   __asm__ volatile("isb"    ::: "memory")
#define wfe()   __asm__ volatile("wfe")
#define wfi()   __asm__ volatile("wfi")

#endif /* OSITO_VIRT_H */
