/*
 * sm8350.h -- Hardware definitions for Qualcomm SM8350 (Snapdragon 888)
 * Target: ASUS ROG Phone 5 (I005DA)
 *
 * All addresses extracted from live device tree and verified on real hardware.
 * Source: sm8350-boot bootloader (tested 2026-03-03/04)
 */

#ifndef OSITO_SM8350_H
#define OSITO_SM8350_H

#include <stdint.h>

/* ========================================================================
 * Memory-Mapped I/O
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

/* ========================================================================
 * UART -- Qualcomm GENI Serial Engine (Console)
 *
 * ABL pre-initializes clocks, pinmux, baud=115200. DO NOT REINIT.
 *
 * Verified on real hardware:
 *   - TX FIFO polling may hang if FIFO doesn't drain
 *   - MUST use timeout in uart_putc (~100k iterations)
 *   - RX works fine for interactive input
 * ======================================================================== */
#define GENI_UART_BASE          0x0098C000UL

/* Register offsets */
#define SE_GENI_STATUS          0x040
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_IRQ_STATUS    0x640
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFOn        0x700
#define SE_GENI_RX_FIFOn        0x780
#define SE_GENI_TX_FIFO_STATUS  0x800
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GENI_TX_WATERMARK    0x80C
#define SE_IRQ_EN               0xE1C

/* FIFO status bits */
#define TX_FIFO_WC_MASK         0x0FFFFFFFUL
#define RX_FIFO_WC_MASK         0x0000007FUL
#define TX_FIFO_DEPTH           16          /* Words before full */

/* IRQ bits */
#define M_TX_FIFO_WATERMARK_EN  (1U << 30)
#define M_CMD_DONE_EN           (1U << 0)

/* ========================================================================
 * GICv3 -- ARM Generic Interrupt Controller v3
 * ======================================================================== */
#define GICD_BASE               0x17A00000UL
#define GICR_BASE               0x17A60000UL
#define GICR_STRIDE             0x00020000UL    /* Per-CPU */

/* GICD registers */
#define GICD_CTLR_OFF           0x000
#define GICD_TYPER_OFF          0x004
#define GICD_ISENABLER_OFF(n)   (0x100 + (n)*4)
#define GICD_ICENABLER_OFF(n)   (0x180 + (n)*4)
#define GICD_ISPENDR_OFF(n)     (0x200 + (n)*4)
#define GICD_IPRIORITYR_OFF(n)  (0x400 + (n)*4)
#define GICD_ICFGR_OFF(n)       (0xC00 + (n)*4)
#define GICD_IGROUPR_OFF(n)     (0x080 + (n)*4)
#define GICD_IGRPMODR_OFF(n)    (0xD00 + (n)*4)

/* GICR registers (per-CPU, SGI frame at offset 0x10000) */
#define GICR_WAKER_OFF          0x014
#define GICR_SGI_OFF            0x10000
#define GICR_SGI_ISENABLER0     (GICR_SGI_OFF + 0x100)
#define GICR_SGI_ICENABLER0     (GICR_SGI_OFF + 0x180)
#define GICR_SGI_IPRIORITYR(n)  (GICR_SGI_OFF + 0x400 + (n)*4)
#define GICR_SGI_IGROUPR0       (GICR_SGI_OFF + 0x080)
#define GICR_SGI_IGRPMODR0      (GICR_SGI_OFF + 0xD00)

/* ========================================================================
 * ARM Generic Timer -- 19.2 MHz (confirmed on device)
 * ======================================================================== */
#define TIMER_FREQ_HZ           19200000UL
#define TIMER_MS_TO_TICKS(ms)   ((TIMER_FREQ_HZ / 1000) * (ms))
#define TIMER_US_TO_TICKS(us)   ((TIMER_FREQ_HZ / 1000000) * (us))

/* Timer PPI interrupt IDs (GIC) */
#define TIMER_NS_PHYS_PPI       30      /* Non-secure physical: PPI 14 */
#define TIMER_SEC_PHYS_PPI      29      /* Secure physical: PPI 13 */
#define TIMER_VIRT_PPI          27      /* Virtual: PPI 11 */

/* Memory-mapped timer */
#define TIMER_MEM_BASE          0x17C20000UL
#define TIMER_FRAME0_BASE       0x17C21000UL

/* ========================================================================
 * Display -- Qualcomm MDSS/SDE
 *
 * ABL leaves splash framebuffer active. No MDSS init needed for basic
 * pixel output. Format confirmed ARGB8888 via color bar test.
 * ======================================================================== */
#define MDSS_BASE               0x0AE00000UL
#define MDSS_SIZE               0x84000UL
#define VBIF_BASE               0x0AEB0000UL

/* Splash framebuffer (verified on real hardware) */
#define SPLASH_FB_BASE          0x0E5000000ULL
#define SPLASH_FB_SIZE          0x02300000UL    /* 35 MB reserved */
#define DISPLAY_WIDTH           1080
#define DISPLAY_HEIGHT          2448
#define DISPLAY_BPP             4               /* ARGB8888 */
#define DISPLAY_STRIDE          (DISPLAY_WIDTH * DISPLAY_BPP)
#define DISPLAY_PIXEL(x, y)     (SPLASH_FB_BASE + (y) * DISPLAY_STRIDE + (x) * DISPLAY_BPP)

/* ========================================================================
 * UFS Storage
 * ======================================================================== */
#define UFSHC_BASE              0x01D84000UL
#define UFS_PHY_BASE            0x01D87000UL
#define UFS_ICE_BASE            0x01D88000UL

/* ========================================================================
 * USB -- DWC3 + Qualcomm wrapper
 * ======================================================================== */
#define USB0_BASE               0x0A600000UL    /* Side port (primary) */
#define USB1_BASE               0x0A800000UL    /* Bottom port */
#define USB0_HSPHY_BASE         0x088E3000UL
#define USB0_SSPHY_BASE         0x088E8000UL

/* ========================================================================
 * GPIO / TLMM
 * ======================================================================== */
#define TLMM_BASE               0x0F000000UL

/* ========================================================================
 * SPMI -- PMIC Arbiter (button input, regulators)
 *
 * WARNING: Direct observer probe causes data abort on this device.
 * Channel mapping not yet determined. Use with caution.
 * ======================================================================== */
#define SPMI_CORE_BASE          0x0C440000UL
#define SPMI_CHNLS_BASE         0x0C600000UL
#define SPMI_OBSRVR_BASE        0x0E600000UL

/* PON (power button + vol_down) */
#define PON_PERIPH_ID           0x13
#define PON_RT_STS_OFF          0x10
#define KPDPWR_ON_BIT           (1 << 0)
#define RESIN_ON_BIT            (1 << 1)

/* Vol Up: PMIC GPIO6 (periph 0x8D) */
#define VOLUP_GPIO_PERIPH_ID    0x8D
#define GPIO_RT_STS_OFF         0x10
#define GPIO_VAL_BIT            (1 << 0)

/* ========================================================================
 * Misc
 * ======================================================================== */
#define GCC_BASE                0x00100000UL
#define IPCC_BASE               0x00408000UL

/* ========================================================================
 * CPU Topology (from MIDR_EL1, verified)
 * ======================================================================== */
#define MIDR_PART_A55           0xD05
#define MIDR_PART_A78           0xD41
#define MIDR_PART_X1            0xD44

/* PSCI (power state coordination interface) */
#define PSCI_SYSTEM_RESET       0x84000009ULL
#define PSCI_CPU_ON             0xC4000003ULL
#define PSCI_CPU_OFF            0x84000002ULL

#endif /* OSITO_SM8350_H */
