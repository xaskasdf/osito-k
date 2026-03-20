/*
 * start.S -- Osito-K AArch64 entry point (shared by all platforms)
 *
 * Entry conditions (common):
 *   x0 = DTB physical address (or 0)
 *   EL1, interrupts disabled
 *
 * SM8350 (ABL): caches ON, MMU may be ON (identity mapped),
 *   UART at 0x98C000 already configured. Actual load ~0xA0080000.
 * QEMU virt: -kernel loads Image at RAM base (0x40000000).
 *
 * All code must be position-independent (ADRP/ADR, no stored pointers).
 * Stack address provided by linker script symbol _stack_top.
 */

.section .text.boot
.globl _start

/*
 * ARM64 Image header (required by ABL)
 * Must be at offset 0 of the binary.
 */
_start:
    b       _entry                  // code0: branch to entry
    .word   0                       // code1: reserved
    .quad   0x0                     // text_offset: image load offset
    .quad   _image_end - _start     // image_size
    .quad   0xa                     // flags: LE, 4KB pages, anywhere
    .quad   0                       // res2
    .quad   0                       // res3
    .quad   0                       // res4
    .ascii  "ARM\x64"              // magic
    .word   0                       // res5 (PE offset)

/*
 * Actual entry point
 */
_entry:
    /* Save DTB address (callee-saved register) */
    mov     x19, x0

    /* Verify EL1 */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #1
    b.ne    _hang

    /* Set up exception vectors */
    adr     x0, _vectors
    msr     VBAR_EL1, x0
    isb

    /* Stack: provided by linker script (_stack_top) */
    ldr     x0, =_stack_top
    mov     sp, x0

    /* Clear BSS */
    adr     x0, __bss_start
    adr     x1, __bss_end
1:  cmp     x0, x1
    b.ge    2f
    str     xzr, [x0], #8
    b       1b
2:

    /* Jump to C kernel entry */
    mov     x0, x19                 // x0 = DTB
    bl      kernel_main

    /* Should not return */
_hang:
    wfe
    b       _hang

/*
 * Exception vector table
 * 4 entries x 4 types = 16 vectors, each 0x80 bytes apart.
 *
 * For Osito-K scheduler: IRQ handler must save full context frame
 * (x0-x30, SP, ELR_EL1, SPSR_EL1) then call scheduler, restore
 * next task's context, and eret. See aarch64.h for context struct.
 *
 * This is the minimal crash-catching version. The full scheduler
 * version (like ESP8266's context_switch.S) goes here.
 */
.balign 0x800
.globl _vectors
_vectors:
    /* Current EL with SP0 */
    b       _exception_sync
    .balign 0x80
    b       _exception_irq
    .balign 0x80
    b       _hang                   // FIQ
    .balign 0x80
    b       _exception_serror
    .balign 0x80

    /* Current EL with SPx */
    b       _exception_sync
    .balign 0x80
    b       _exception_irq
    .balign 0x80
    b       _hang                   // FIQ
    .balign 0x80
    b       _exception_serror
    .balign 0x80

    /* Lower EL AArch64 */
    b       _hang
    .balign 0x80
    b       _hang
    .balign 0x80
    b       _hang
    .balign 0x80
    b       _hang
    .balign 0x80

    /* Lower EL AArch32 */
    b       _hang
    .balign 0x80
    b       _hang
    .balign 0x80
    b       _hang
    .balign 0x80
    b       _hang
    .balign 0x80

_exception_sync:
    /* Save x0-x3 on stack before checking ESR (preserves syscall args) */
    stp     x0, x1, [sp, #-16]!
    stp     x2, x3, [sp, #-16]!

    /* Check exception class — SVC from AArch64? */
    mrs     x0, ESR_EL1
    lsr     x1, x0, #26            // EC = bits[31:26]
    cmp     x1, #0x15              // EC=0x15 = SVC from AArch64
    b.eq    _svc_entry

    /* Not SVC: generic exception handler (crash) */
    mrs     x1, ELR_EL1
    mrs     x2, FAR_EL1
    bl      exception_handler
    ldp     x2, x3, [sp], #16
    ldp     x0, x1, [sp], #16
    b       _hang

_svc_entry:
    /* Restore x0-x3 (the real syscall args) before saving full frame */
    ldp     x2, x3, [sp], #16
    ldp     x0, x1, [sp], #16

    /* Save full context frame (272 bytes, same layout as IRQ) */
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]
    add     x0, sp, #272
    str     x0, [sp, #248]         // saved SP
    mrs     x0, ELR_EL1
    mrs     x1, SPSR_EL1
    stp     x0, x1, [sp, #256]

    /* Call C handler: svc_handler(frame) where frame = sp */
    mov     x0, sp
    bl      svc_handler

    /* Restore context and return to userspace */
    ldp     x0, x1, [sp, #256]
    msr     ELR_EL1, x0
    msr     SPSR_EL1, x1
    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]
    add     sp, sp, #272
    eret

_exception_irq:
    b       irq_vector_entry

_exception_serror:
    stp     x0, x1, [sp, #-16]!
    mrs     x0, ESR_EL1
    bl      serror_handler
    ldp     x0, x1, [sp], #16
    b       _hang

/* BSS symbols provided by linker script */
