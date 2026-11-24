/*
 * Minimal DPMI 0.9 client used to validate the interpreted USE16 to native
 * USE32 transition, segment preservation/invalidation, and raw callbacks.
 * Descriptors allocated by native INT 31h calls must reach the private LDT.
 */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds
    movw %cs, saved_real_cs
    movw %ss, saved_real_ss
    pushfw
    popw %dx
    testw $0x0200, %dx
    jz real_initial_interrupt_fail
    movw 0x2c, %ax
    movw %ax, saved_real_env
    movw %ax, %dx
    shlw $4, %dx
    movw %dx, environment_base_lo
    shrw $12, %ax
    movw %ax, environment_base_hi

    /* The COM loader grants the client the remaining conventional arena.
     * Move the stack into the retained prefix and release the tail before
     * exercising DPMI DOS-memory allocation. */
    movw $0x1ff0, %sp
    movw %cs, %ax
    movw %ax, %es
    movw $0x0200, %bx
    movb $0x4a, %ah
    int $0x21
    jc real_mode_fail
    pushfw
    popw %dx
    testw $0x0200, %dx
    jz real_resize_interrupt_fail

    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz real_mode_fail

    movw %di, dpmi_entry
    movw %es, dpmi_entry + 2
    pushfw
    popw %dx
    testw $0x0200, %dx
    jz real_interrupt_fail
    movw $1, %ax
    lcall *dpmi_entry

pm16_entry:
    jc pm16_contract_fail
    cmpw $1, %ax
    jne pm16_contract_fail
    movw %cs, initial_code_selector
    movw %ds, initial_data_selector
    movw %ss, initial_stack_selector
    movw %es, initial_psp_selector
    movw %fs, %ax
    testw %ax, %ax
    jnz pm16_contract_fail
    movw %gs, %ax
    testw %ax, %ax
    jnz pm16_contract_fail

    movw $0x0902, %ax
    int $0x31
    jc pm16_interrupt_fail
    cmpw $0x0901, %ax
    jne pm16_interrupt_fail

    movw %cs, %bx
    movw $0x0006, %ax
    int $0x31
    jc pm16_fail
    movw %cx, code_base_hi
    movw %dx, code_base_lo

    movw initial_psp_selector, %bx
    movw $0x0006, %ax
    int $0x31
    jc pm16_contract_fail
    cmpw code_base_hi, %cx
    jne pm16_contract_fail
    cmpw code_base_lo, %dx
    jne pm16_contract_fail

    movw %es:0x2c, %ax
    movw %ax, initial_environment_selector
    cmpw $0, saved_real_env
    je 1f
    testw %ax, %ax
    jz pm16_contract_fail
    movw %ax, %bx
    movw $0x0006, %ax
    int $0x31
    jc pm16_contract_fail
    cmpw environment_base_hi, %cx
    jne pm16_contract_fail
    cmpw environment_base_lo, %dx
    jne pm16_contract_fail
    jmp 2f
1:
    testw %ax, %ax
    jnz pm16_contract_fail
2:

    /* A USE32 client starts in a USE16 CS with Big DS and SS. Function
     * 000Bh already uses a 48-bit ES:EDI pointer at this stage. */
    movw initial_data_selector, %ax
    movw %ax, %es
    movl $initial_descriptor, %edi

    movw initial_code_selector, %bx
    movw $0x000b, %ax
    int $0x31
    jc pm16_contract_fail
    testb $0x40, initial_descriptor + 6
    jnz pm16_contract_fail

    movw initial_data_selector, %bx
    movw $0x000b, %ax
    int $0x31
    jc pm16_contract_fail
    testb $0x40, initial_descriptor + 6
    jz pm16_contract_fail

    movw initial_stack_selector, %bx
    movw $0x000b, %ax
    int $0x31
    jc pm16_contract_fail
    testb $0x40, initial_descriptor + 6
    jz pm16_contract_fail

    movw initial_psp_selector, %bx
    movw $0x000b, %ax
    int $0x31
    jc pm16_contract_fail
    cmpw $0x00ff, initial_descriptor
    jne pm16_contract_fail
    testb $0x0f, initial_descriptor + 6
    jnz pm16_contract_fail

    /* PSP and environment descriptors are host-owned. */
    movw initial_psp_selector, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x0007, %ax
    int $0x31
    jnc pm16_contract_fail
    cmpw $0x8022, %ax
    jne pm16_contract_fail

    movw initial_environment_selector, %bx
    testw %bx, %bx
    jz 3f
    movw $0x0007, %ax
    int $0x31
    jnc pm16_contract_fail
    cmpw $0x8022, %ax
    jne pm16_contract_fail
3:

    /* Function 000Dh owns the low LDT reserve. Input RPL bits are ignored,
     * and ordinary allocation must not reuse a released reserved entry. */
    movw $0x0004, %bx
    movw $0x000d, %ax
    int $0x31
    jc pm16_fail

    movw $0x0007, %bx
    movw $0x000d, %ax
    int $0x31
    jnc pm16_fail
    cmpw $0x8011, %ax
    jne pm16_fail

    movw $0x0007, %bx
    movw $0x0001, %ax
    int $0x31
    jc pm16_fail

    movw $1, %cx
    movw $0x0000, %ax
    int $0x31
    jc pm16_fail
    cmpw $0x0087, %ax
    jb pm16_fail
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

    movw $0x0902, %ax
    int $0x31
    jc pm16_interrupt_fail
    cmpw $0x0901, %ax
    jne pm16_interrupt_fail

    movw initial_data_selector, %ax
    movw %ax, %fs
    movw %ax, %gs
    ljmp *native_pointer

real_mode_fail:
    movw $real_fail_message, %dx
    movb $0x09, %ah
    int $0x21
    movw $0x4c7e, %ax
    int $0x21

real_interrupt_fail:
    movw $0x4c76, %ax
    int $0x21

real_initial_interrupt_fail:
    movw $0x4c74, %ax
    int $0x21

real_resize_interrupt_fail:
    movw $0x4c73, %ax
    int $0x21

pm16_fail:
    movw $0x4c7f, %ax
    int $0x21

pm16_contract_fail:
    movw $0x4c78, %ax
    int $0x21

pm16_interrupt_fail:
    movw $0x4c75, %ax
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
saved_real_ss:
    .word 0
saved_real_env:
    .word 0
environment_base_hi:
    .word 0
environment_base_lo:
    .word 0
code_base_hi:
    .word 0
code_base_lo:
    .word 0
initial_code_selector:
    .word 0
initial_data_selector:
    .word 0
initial_stack_selector:
    .word 0
initial_psp_selector:
    .word 0
initial_environment_selector:
    .word 0
initial_descriptor:
    .space 8
real_fail_message:
    .ascii "DPMI host unavailable\r\n$"

    .code32
native32_entry:
    movw %ds, native_data_selector
    movw %fs, %ax
    cmpw native_data_selector, %ax
    jne native_segment_fail
    movw %gs, %ax
    cmpw native_data_selector, %ax
    jne native_segment_fail
    cmpl $0x2468ace0, %fs:segment_marker
    jne native_segment_fail
    cmpl $0x2468ace0, %gs:segment_marker
    jne native_segment_fail

    movl $0x0400, %eax
    int $0x31
    jc native_fail
    cmpl $0x2468ace0, %fs:segment_marker
    jne native_segment_fail
    cmpl $0x2468ace0, %gs:segment_marker
    jne native_segment_fail

    /* Resident hosts may implement page services as no-ops, but the calls
     * and virtual interrupt state remain mandatory DPMI 0.9 contracts. */
    movl $0x0604, %eax
    int $0x31
    jc native_page_interrupt_fail
    testw %bx, %bx
    jnz native_page_interrupt_fail
    cmpw $0x1000, %cx
    jne native_page_interrupt_fail

    movl $0x0600, %eax
    int $0x31
    jc native_page_interrupt_fail
    movl $0x0601, %eax
    int $0x31
    jc native_page_interrupt_fail
    movl $0x0602, %eax
    int $0x31
    jc native_page_interrupt_fail
    movl $0x0603, %eax
    int $0x31
    jc native_page_interrupt_fail
    movl $0x0702, %eax
    int $0x31
    jc native_page_interrupt_fail
    movl $0x0703, %eax
    int $0x31
    jc native_page_interrupt_fail

    movl $0x0902, %eax
    int $0x31
    jc native_page_interrupt_fail
    cmpw $0x0901, %ax
    jne native_page_interrupt_fail
    movl $0x0900, %eax
    int $0x31
    jc native_page_interrupt_fail
    cmpw $0x0901, %ax
    jne native_page_interrupt_fail
    movl $0x0902, %eax
    int $0x31
    jc native_page_interrupt_fail
    cmpw $0x0900, %ax
    jne native_page_interrupt_fail
    movl $0x0901, %eax
    int $0x31
    jc native_page_interrupt_fail
    cmpw $0x0900, %ax
    jne native_page_interrupt_fail

    cli
    movl $0x0902, %eax
    int $0x31
    jc native_page_interrupt_fail
    cmpw $0x0900, %ax
    jne native_page_interrupt_fail
    sti
    movl $0x0902, %eax
    int $0x31
    jc native_page_interrupt_fail
    cmpw $0x0901, %ax
    jne native_page_interrupt_fail

    movl $0x7fff, %eax
    int $0x31
    jnc native_page_interrupt_fail
    cmpw $0x8001, %ax
    jne native_page_interrupt_fail

    /* Every vector initially exposes a unique, callable host reflector. Save
     * INT 33h's address so the client handler can chain to the translator. */
    movb $0x33, %bl
    movl $0x0204, %eax
    int $0x31
    jc native_pm_vector_fail
    cmpl $0x10cc, %edx
    jne native_pm_vector_fail
    movl %edx, old_pm33_pointer
    movw %cx, old_pm33_pointer + 4
    movw %cx, default_pm_selector

    movb $0x07, %bl
    movl $0x0204, %eax
    int $0x31
    jc native_pm_vector_fail
    cmpw default_pm_selector, %cx
    jne native_pm_vector_fail
    cmpl $0x101c, %edx
    jne native_pm_vector_fail

    /* Installed protected-mode vectors must preempt host-owned IDT gates.
     * The private vector numbers are ordinary client-visible vectors unless
     * execution came from the host's exact transition stub. */
    movw %cs, %cx
    movl $native_pmf9_handler, %edx
    movb $0xf9, %bl
    movl $0x0205, %eax
    int $0x31
    jc native_pm_vector_fail

    movb $0, native_pmf9_seen
    int $0xf9
    cmpb $1, native_pmf9_seen
    jne native_pm_vector_fail

    movw %cs, %cx
    movl $native_pmfa_handler, %edx
    movb $0xfa, %bl
    movl $0x0205, %eax
    int $0x31
    jc native_pm_vector_fail
    movw %cs, %cx
    movl $native_pmfc_handler, %edx
    movb $0xfc, %bl
    movl $0x0205, %eax
    int $0x31
    jc native_pm_vector_fail
    movw %cs, %cx
    movl $native_pmfd_handler, %edx
    movb $0xfd, %bl
    movl $0x0205, %eax
    int $0x31
    jc native_pm_vector_fail

    movb $0, native_pmfa_seen
    movb $0, native_pmfc_seen
    movb $0, native_pmfd_seen
    int $0xfa
    int $0xfc
    int $0xfd
    cmpb $1, native_pmfa_seen
    jne native_pm_vector_fail
    cmpb $1, native_pmfc_seen
    jne native_pm_vector_fail
    cmpb $1, native_pmfd_seen
    jne native_pm_vector_fail

    /* The custom INT 33h handler chains with FAR JMP to the old 0204h
     * address. Its internal INT F9h must bypass the client's F9 handler. */
    movw %cs, %cx
    movl $native_pm33_handler, %edx
    movb $0x33, %bl
    movl $0x0205, %eax
    int $0x31
    jc native_pm_vector_fail
    movb $0, native_pm33_seen
    movb $0, native_pmf9_seen
    xorl %eax, %eax
    int $0x33
    cmpb $1, native_pm33_seen
    jne native_pm_vector_fail
    cmpb $0, native_pmf9_seen
    jne native_pm_vector_fail
    cmpw $0xffff, %ax
    jne native_pm_vector_fail
    movl $0x0902, %eax
    int $0x31
    jc native_pm_vector_fail
    cmpw $0x0901, %ax
    jne native_pm_vector_fail

    movw %cs, %cx
    movl $native_pm60_handler, %edx
    movb $0x60, %bl
    movl $0x0205, %eax
    int $0x31
    jc native_pm_vector_fail
    movb $0, native_pm60_seen
    int $0x60
    cmpb $1, native_pm60_seen
    jne native_pm_vector_fail
    movl $0x0902, %eax
    int $0x31
    jc native_pm_vector_fail
    cmpw $0x0901, %ax
    jne native_pm_vector_fail

    movw %cs, %cx
    movl $native_pm07_handler, %edx
    movb $0x07, %bl
    movl $0x0205, %eax
    int $0x31
    jc native_pm_vector_fail
    movb $0, native_pm07_state
    int $0x07
    cmpb $1, native_pm07_state
    jne native_pm_vector_fail
    movl $0x0902, %eax
    int $0x31
    jc native_pm_vector_fail
    cmpw $0x0901, %ax
    jne native_pm_vector_fail

    /* A 32-bit client receives one descriptor even when the DOS block crosses
     * 64 KiB.  Exercise grow, free, native LDT synchronization, and ES
     * invalidation when its descriptor is released. */
    movw $0x1001, %bx
    movl $0x0100, %eax
    int $0x31
    jc native_dos_memory_fail
    movw %dx, dos_block_selector

    movw $0x1801, %bx
    movw dos_block_selector, %dx
    movl $0x0102, %eax
    int $0x31
    jc native_dos_memory_fail

    movw dos_block_selector, %ax
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %dx
    movl $0x0101, %eax
    int $0x31
    jc native_dos_memory_fail
    movw %es, %ax
    testw %ax, %ax
    jnz native_dos_memory_fail
    movw %fs, %ax
    testw %ax, %ax
    jnz native_segment_fail
    movw %gs, %ax
    testw %ax, %ax
    jnz native_segment_fail
    movw native_data_selector, %ax
    movw %ax, %es

    movw %cs, %cx
    movl $native_debug_handler, %edx
    movb $1, %bl
    movl $0x0203, %eax
    int $0x31
    jc native_exception_fail

    movb $0, native_debug_seen
    .byte 0xf1                  /* ICEBP: DPMI processor exception 1 */
native_debug_resume:
    cmpb $1, native_debug_seen
    jne native_exception_fail
    cmpb $1, native_pmfd_seen
    jne native_pm_vector_fail

    movl $1, %ecx
    xorl %eax, %eax
    int $0x31
    jc native_fail

    movw %ax, %fs

    movw %ax, %gs
    movw %ax, %bx
    movl $0x0001, %eax
    int $0x31
    jc native_segment_fail
    movw %fs, %ax
    testw %ax, %ax
    jnz native_segment_fail
    movw %gs, %ax
    testw %ax, %ax
    jnz native_segment_fail

    /* Allocate a real-mode callback whose PM half executes natively after
     * the final raw switch. DS names the code target; ES names the register
     * structure that the host fills at callback time. */
    movw %ds, native_data_selector
    movw %cs, %ax
    movw %ax, %ds
    movw native_data_selector, %ax
    movw %ax, %es
    movl $native_callback, %esi
    movl $callback_regs, %edi
    movl $0x0303, %eax
    int $0x31
    jc native_callback_fail
    movw native_data_selector, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %dx, callback_far
    movw %cx, callback_far + 2

    movl $0x0306, %eax
    int $0x31
    jc native_fail
    movl %edi, raw_pm_pointer
    movw %si, raw_pm_pointer + 4

    movw saved_real_cs, %ax
    movw saved_real_cs, %cx
    movw saved_real_ss, %dx
    movl $0xff00, %ebx
    movw saved_real_cs, %si
    movl $raw_rm_entry, %edi
    ljmp *raw_pm_pointer

native_fail:
    movl $0x4c7d, %eax
    int $0x21

native_segment_fail:
    movl $0x4c70, %eax
    int $0x21

native_exception_fail:
    movl $0x4c7c, %eax
    int $0x21

native_callback_fail:
    movl $0x4c7b, %eax
    int $0x21

native_dos_memory_fail:
    movl $0x4c79, %eax
    int $0x21

native_page_interrupt_fail:
    movl $0x4c77, %eax
    int $0x21

native_pm_vector_fail:
    movl $0x4c72, %eax
    int $0x21

native_pm33_handler:
    movb $1, native_pm33_seen
    ljmp *old_pm33_pointer

native_pmf9_handler:
    incb native_pmf9_seen
    iretl

native_pmfa_handler:
    incb native_pmfa_seen
    iretl

native_pmfc_handler:
    incb native_pmfc_seen
    iretl

native_pmfd_handler:
    incb native_pmfd_seen
    iretl

native_pm60_handler:
    movb $1, native_pm60_seen
    iretl

native_pm07_handler:
    pushal
    movl $0x0902, %eax
    int $0x31
    cmpw $0x0900, %ax
    jne 4f
    movb $1, native_pm07_state
    jmp 5f
4:
    movb $2, native_pm07_state
5:
    sti
    popal
    iretl

native_debug_handler:
    pushl %eax
    /* ICEBP is a trap: validate the next EIP without adjusting it. The
     * initial USE32 stack is Big, so address the old-style frame via ESP. */
    cmpl $native_debug_resume, %ss:16(%esp)
    jne native_exception_fail
    movb $1, native_debug_seen
    popl %eax
    lret

native_callback:
    /* DS:ESI points to the real-mode FAR return frame and ES:EDI points
     * to the 32h-byte DPMI register structure. Consume that FAR frame,
     * select its return address, and return a marker in real-mode EAX. */
    movw (%esi), %ax
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    movw %ax, %es:44(%edi)
    addw $4, %es:46(%edi)
    movl $0x1357cafe, %es:28(%edi)
    movw %fs, %ax
    testw %ax, %ax
    jnz callback_segment_fail
    movw %gs, %ax
    testw %ax, %ax
    jnz callback_segment_fail
    iretl

callback_segment_fail:
    movl $0xdead0070, %es:28(%edi)
    iretl

    .align 2
segment_marker:
    .long 0x2468ace0
raw_pm_pointer:
    .long 0
    .word 0
old_pm33_pointer:
    .long 0
    .word 0
default_pm_selector:
    .word 0
native_debug_seen:
    .byte 0
native_pm33_seen:
    .byte 0
native_pmf9_seen:
    .byte 0
native_pmfa_seen:
    .byte 0
native_pmfc_seen:
    .byte 0
native_pmfd_seen:
    .byte 0
native_pm60_seen:
    .byte 0
native_pm07_state:
    .byte 0
native_data_selector:
    .word 0
dos_block_selector:
    .word 0
    .align 2
callback_far:
    .word 0, 0
callback_regs:
    .space 50

    .code16
raw_rm_entry:
    lcall *callback_far
    cmpw $0xcafe, %ax
    jne raw_callback_fail
    cmpb $1, %cs:native_pmfa_seen
    jne raw_private_vector_fail
    cmpb $1, %cs:native_pmfc_seen
    jne raw_private_vector_fail
    cmpb $1, %cs:native_pmfd_seen
    jne raw_private_vector_fail
    movw $0x4c2a, %ax
    int $0x21

raw_callback_fail:
    movw $0x4c7a, %ax
    int $0x21

raw_private_vector_fail:
    movw $0x4c71, %ax
    int $0x21
