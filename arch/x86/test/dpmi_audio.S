/*
 * Native DPMI Sound Blaster contract test.
 *
 * The USE32 half performs real IN/OUT instructions at CPL3. OsitoK must
 * route their #GP faults through the virtual DOS I/O bus, stream the DMA
 * buffer through the shared PCM scheduler, and reflect IRQ5 to vector 0Dh.
 */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds
    movw %cs, saved_real_cs

    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz real_mode_fail

    movw %di, dpmi_entry
    movw %es, dpmi_entry + 2
    movw $1, %ax
    lcall *dpmi_entry

pm16_entry:
    movw %cs, %bx
    movw $0x0006, %ax
    int $0x31
    jc pm16_fail
    movw %cx, code_base_hi
    movw %dx, code_base_lo

    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc pm16_fail
    movw %ax, native_selector
    movw %ax, native_pointer + 2

    movw %ax, %bx
    movw code_base_hi, %cx
    movw code_base_lo, %dx
    movw $0x0007, %ax
    int $0x31
    jc pm16_fail

    movw native_selector, %bx
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $0x0008, %ax
    int $0x31
    jc pm16_fail

    movw native_selector, %bx
    movw $0x40fa, %cx
    movw $0x0009, %ax
    int $0x31
    jc pm16_fail
    ljmp *native_pointer

real_mode_fail:
    movw $0x4c70, %ax
    int $0x21

pm16_fail:
    movw $0x4c71, %ax
    int $0x21

    .align 2
dpmi_entry:
    .word 0, 0
native_pointer:
    .word native32_entry, 0
native_selector:
    .word 0
saved_real_cs:
    .word 0
code_base_hi:
    .word 0
code_base_lo:
    .word 0

    .code32
native32_entry:
    /* Install the protected-mode handler for DOS IRQ5 (INT 0Dh). */
    movw %cs, %cx
    movl $native_audio_irq, %edx
    movb $0x0d, %bl
    movl $0x0205, %eax
    int $0x31
    jc native_vector_fail

    /* Reset DSP and require the conventional 0xAA acknowledgement. */
    movw $0x226, %dx
    movb $1, %al
    outb %al, %dx
    xorb %al, %al
    outb %al, %dx
    movl $100000, %ecx
reset_poll:
    movw $0x22e, %dx
    inb %dx, %al
    testb $0x80, %al
    jnz reset_ready
    loop reset_poll
    jmp native_dsp_fail
reset_ready:
    movw $0x22a, %dx
    inb %dx, %al
    cmpb $0xaa, %al
    jne native_dsp_fail

    /* Program DMA channel 1 for the buffer's guest physical address. */
    movzwl saved_real_cs, %ebx
    shll $4, %ebx
    addl $sample_buffer, %ebx

    movw $0x0a, %dx
    movb $0x05, %al
    outb %al, %dx
    movw $0x0c, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x02, %dx
    movb %bl, %al
    outb %al, %dx
    movb %bh, %al
    outb %al, %dx
    movl %ebx, %eax
    shrl $16, %eax
    movw $0x83, %dx
    outb %al, %dx
    movw $0x03, %dx
    movb $0xff, %al
    outb %al, %dx
    movb $0x0f, %al
    outb %al, %dx
    movw $0x0b, %dx
    movb $0x49, %al
    outb %al, %dx
    movw $0x0a, %dx
    movb $0x01, %al
    outb %al, %dx

    /* 48 kHz, unsigned mono, single-cycle 4096-byte transfer. */
    movw $0x22c, %dx
    movb $0xd1, %al
    outb %al, %dx
    movb $0x41, %al
    outb %al, %dx
    movb $0xbb, %al
    outb %al, %dx
    movb $0x80, %al
    outb %al, %dx
    movb $0xc0, %al
    outb %al, %dx
    xorb %al, %al
    outb %al, %dx
    movb $0xff, %al
    outb %al, %dx
    movb $0x0f, %al
    outb %al, %dx

    movl $200000000, %ecx
irq_wait:
    cmpb $1, audio_irq_seen
    je native_success
    pause
    loop irq_wait
    jmp native_irq_fail

native_success:
    movl $0x0902, %eax
    int $0x31
    jc native_irq_fail
    cmpw $0x0901, %ax
    jne native_irq_fail

    /* Terminal count is latched in status and clears on the first read. */
    movw $0x08, %dx
    inb %dx, %al
    testb $0x02, %al
    jz native_dma_fail
    inb %dx, %al
    testb $0x02, %al
    jnz native_dma_fail

    /* A single-cycle transfer leaves address one past the buffer and the
     * current count wrapped to FFFF. */
    addw $4096, %bx
    movw $0x0c, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x02, %dx
    inb %dx, %al
    cmpb %bl, %al
    jne native_dma_fail
    inb %dx, %al
    cmpb %bh, %al
    jne native_dma_fail

    movw $0x0c, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x03, %dx
    inb %dx, %al
    cmpb $0xff, %al
    jne native_dma_fail
    inb %dx, %al
    cmpb $0xff, %al
    jne native_dma_fail

    movl $0x4c2a, %eax
    int $0x21
native_vector_fail:
    movl $0x4c72, %eax
    int $0x21
native_dsp_fail:
    movl $0x4c73, %eax
    int $0x21
native_irq_fail:
    movl $0x4c74, %eax
    int $0x21
native_dma_fail:
    movl $0x4c75, %eax
    int $0x21

native_audio_irq:
    pushal
    movw $0x22e, %dx
    inb %dx, %al
    movb $1, audio_irq_seen
    popal
    sti
    iretl

audio_irq_seen:
    .byte 0
    .align 16
sample_buffer:
    .fill 4096, 1, 0xe0
