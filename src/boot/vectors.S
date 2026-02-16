/*
 * OsitoK - Exception/Interrupt vector table
 *
 * ESP8266/LX106 vector layout (from VECBASE):
 *   Offset 0x00: (unused on CALL0 ABI — window vectors base)
 *   Offset 0x10: Level-2 / Debug exception
 *   Offset 0x20: Level-3 / NMI (hardware WDT)
 *   Offset 0x30: KernelExceptionVector (PS.UM=0)
 *   Offset 0x50: UserExceptionVector   (PS.UM=1)
 *   Offset 0x70: DoubleExceptionVector
 *
 * Confirmed from ESP8266 core-isa.h:
 *   XCHAL_KERNEL_VECOFS    = 0x30
 *   XCHAL_USER_VECOFS      = 0x50
 *   XCHAL_DOUBLEEXC_VECOFS = 0x70
 *   XCHAL_INTLEVEL2_VECOFS = 0x10  (debug)
 *   XCHAL_INTLEVEL3_VECOFS = 0x20  (NMI)
 */

    .section .vectors, "ax"
    .global _vecbase
    .global _xt_user_exc

    .literal_position
    .align 256           /* VECBASE must be 256-byte aligned */

/*
 * Offset 0x00: Window vectors base (unused in CALL0 ABI)
 * We put a jump to _start here as a convenience.
 */
_vecbase:
    j       _start

/* -----------------------------------------------------------
 * Offset 0x10: Level-2 / Debug exception vector
 * Not used in normal operation.
 * ----------------------------------------------------------- */
    .org _vecbase + 0x10
    break   1, 15
    j       . - 3           /* spin if debug exception fires */

/* -----------------------------------------------------------
 * Offset 0x20: NMI vector (Level-3 on ESP8266 LX106)
 *
 * The hardware WDT fires an NMI after ~50-60ms if not fed.
 * Save a0 in EXCSAVE3, then jump to handler.
 * ----------------------------------------------------------- */
    .org _vecbase + 0x20
    wsr     a0, excsave3
    j       _nmi_handler

/* -----------------------------------------------------------
 * Offset 0x30: KernelExceptionVector (fires when PS.UM=0)
 *
 * Level-1 exceptions/interrupts when running in kernel mode.
 * Use the same handler as UserExceptionVector.
 * ----------------------------------------------------------- */
    .org _vecbase + 0x30
    j       _xt_user_exc

/* -----------------------------------------------------------
 * Offset 0x50: UserExceptionVector (fires when PS.UM=1)
 *
 * Level-1 exceptions/interrupts when running in user mode.
 * This is the PRIMARY interrupt entry point for our kernel.
 * ----------------------------------------------------------- */
    .org _vecbase + 0x50
    j       _xt_user_exc

/* -----------------------------------------------------------
 * Offset 0x70: DoubleExceptionVector
 *
 * Fires when an exception occurs inside an exception handler
 * (while PS.EXCM=1). Fatal — just spin.
 * ----------------------------------------------------------- */
    .org _vecbase + 0x70
_double_exc:
    j       _double_exc     /* spin — this is a fatal error */

/* -----------------------------------------------------------
 * NMI handler (placed after all vectors)
 *
 * The WDT is fed proactively from main code (uart_putc etc).
 * This NMI handler is a safety net: just return cleanly.
 * ----------------------------------------------------------- */
    .align 4
    .literal_position
_nmi_handler:
    rsr     a0, excsave3
    rfi     3
