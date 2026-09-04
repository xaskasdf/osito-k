/* AMD64 context capture/restore and software-exception import entries. */

    .text

    .set CTX_FLAGS,   0x030
    .set CTX_MXCSR,   0x034
    .set CTX_SEGCS,   0x038
    .set CTX_SEGDS,   0x03A
    .set CTX_SEGES,   0x03C
    .set CTX_SEGFS,   0x03E
    .set CTX_SEGGS,   0x040
    .set CTX_SEGSS,   0x042
    .set CTX_EFLAGS,  0x044
    .set CTX_RAX,     0x078
    .set CTX_RCX,     0x080
    .set CTX_RDX,     0x088
    .set CTX_RBX,     0x090
    .set CTX_RSP,     0x098
    .set CTX_RBP,     0x0A0
    .set CTX_RSI,     0x0A8
    .set CTX_RDI,     0x0B0
    .set CTX_R8,      0x0B8
    .set CTX_R9,      0x0C0
    .set CTX_R10,     0x0C8
    .set CTX_R11,     0x0D0
    .set CTX_R12,     0x0D8
    .set CTX_R13,     0x0E0
    .set CTX_R14,     0x0E8
    .set CTX_R15,     0x0F0
    .set CTX_RIP,     0x0F8
    .set CTX_XMM0,    0x1A0

    .set CONTEXT_FULL_AMD64, 0x00100007
    .set ENTRY_FRAME_SIZE,   0x518
    .set ENTRY_CONTEXT,      0x030
    .set ENTRY_ARGUMENT5,    ENTRY_CONTEXT + 0x4D0
    .set ENTRY_ARGUMENT6,    ENTRY_CONTEXT + 0x4D8

/* R10 points at a context whose integer and XMM registers are populated. */
    .macro finish_capture
    movq $0, 0x000(%r10)
    movq $0, 0x008(%r10)
    movq $0, 0x010(%r10)
    movq $0, 0x018(%r10)
    movq $0, 0x020(%r10)
    movq $0, 0x028(%r10)
    movl $CONTEXT_FULL_AMD64, CTX_FLAGS(%r10)
    stmxcsr CTX_MXCSR(%r10)
    movw %cs, %ax
    movw %ax, CTX_SEGCS(%r10)
    movw %ds, %ax
    movw %ax, CTX_SEGDS(%r10)
    movw %es, %ax
    movw %ax, CTX_SEGES(%r10)
    movw %fs, %ax
    movw %ax, CTX_SEGFS(%r10)
    movw %gs, %ax
    movw %ax, CTX_SEGGS(%r10)
    movw %ss, %ax
    movw %ax, CTX_SEGSS(%r10)
    pushfq
    popq %rax
    movl %eax, CTX_EFLAGS(%r10)

    xorq %rax, %rax
    movq %rax, 0x048(%r10)
    movq %rax, 0x050(%r10)
    movq %rax, 0x058(%r10)
    movq %rax, 0x060(%r10)
    movq %rax, 0x068(%r10)
    movq %rax, 0x070(%r10)

    fxsave64 0x100(%r10)
    cld
    leaq 0x2A0(%r10), %rdi
    movl $70, %ecx
    rep stosq
    movq CTX_RDI(%r10), %rdi
    .endm

/* Capture an import caller into the context embedded in this entry frame. */
    .macro capture_entry_context
    movq %rax, ENTRY_CONTEXT+CTX_RAX(%rsp)
    movq %r10, ENTRY_CONTEXT+CTX_R10(%rsp)
    leaq ENTRY_CONTEXT(%rsp), %r10
    movq %rcx, CTX_RCX(%r10)
    movq %rdx, CTX_RDX(%r10)
    movq %rbx, CTX_RBX(%r10)
    movq %rbp, CTX_RBP(%r10)
    movq %rsi, CTX_RSI(%r10)
    movq %rdi, CTX_RDI(%r10)
    movq %r8,  CTX_R8(%r10)
    movq %r9,  CTX_R9(%r10)
    movq %r11, CTX_R11(%r10)
    movq %r12, CTX_R12(%r10)
    movq %r13, CTX_R13(%r10)
    movq %r14, CTX_R14(%r10)
    movq %r15, CTX_R15(%r10)
    leaq ENTRY_FRAME_SIZE+8(%rsp), %rax
    movq %rax, CTX_RSP(%r10)
    movq ENTRY_FRAME_SIZE(%rsp), %rax
    movq %rax, CTX_RIP(%r10)
    movdqu %xmm0,  CTX_XMM0+0x00(%r10)
    movdqu %xmm1,  CTX_XMM0+0x10(%r10)
    movdqu %xmm2,  CTX_XMM0+0x20(%r10)
    movdqu %xmm3,  CTX_XMM0+0x30(%r10)
    movdqu %xmm4,  CTX_XMM0+0x40(%r10)
    movdqu %xmm5,  CTX_XMM0+0x50(%r10)
    movdqu %xmm6,  CTX_XMM0+0x60(%r10)
    movdqu %xmm7,  CTX_XMM0+0x70(%r10)
    movdqu %xmm8,  CTX_XMM0+0x80(%r10)
    movdqu %xmm9,  CTX_XMM0+0x90(%r10)
    movdqu %xmm10, CTX_XMM0+0xA0(%r10)
    movdqu %xmm11, CTX_XMM0+0xB0(%r10)
    movdqu %xmm12, CTX_XMM0+0xC0(%r10)
    movdqu %xmm13, CTX_XMM0+0xD0(%r10)
    movdqu %xmm14, CTX_XMM0+0xE0(%r10)
    movdqu %xmm15, CTX_XMM0+0xF0(%r10)
    finish_capture
    .endm

    .macro restore_entry_nonvolatiles
    leaq ENTRY_CONTEXT(%rsp), %r10
    ldmxcsr CTX_MXCSR(%r10)
    movdqu CTX_XMM0+0x60(%r10), %xmm6
    movdqu CTX_XMM0+0x70(%r10), %xmm7
    movdqu CTX_XMM0+0x80(%r10), %xmm8
    movdqu CTX_XMM0+0x90(%r10), %xmm9
    movdqu CTX_XMM0+0xA0(%r10), %xmm10
    movdqu CTX_XMM0+0xB0(%r10), %xmm11
    movdqu CTX_XMM0+0xC0(%r10), %xmm12
    movdqu CTX_XMM0+0xD0(%r10), %xmm13
    movdqu CTX_XMM0+0xE0(%r10), %xmm14
    movdqu CTX_XMM0+0xF0(%r10), %xmm15
    movq CTX_RBX(%r10), %rbx
    movq CTX_RBP(%r10), %rbp
    movq CTX_RSI(%r10), %rsi
    movq CTX_RDI(%r10), %rdi
    movq CTX_R12(%r10), %r12
    movq CTX_R13(%r10), %r13
    movq CTX_R14(%r10), %r14
    movq CTX_R15(%r10), %r15
    .endm

    .globl RaiseException
    .type RaiseException, @function
RaiseException:
    subq $ENTRY_FRAME_SIZE, %rsp
    capture_entry_context
    movq CTX_RCX(%r10), %rcx
    movq CTX_RDX(%r10), %rdx
    movq CTX_R8(%r10), %r8
    movq CTX_R9(%r10), %r9
    movq %r10, 0x20(%rsp)
    call win32_raise_exception_impl
    restore_entry_nonvolatiles
    addq $ENTRY_FRAME_SIZE, %rsp
    retq
    .size RaiseException, .-RaiseException

    .globl win32_debug_break_entry
    .type win32_debug_break_entry, @function
win32_debug_break_entry:
    subq $ENTRY_FRAME_SIZE, %rsp
    capture_entry_context
    movl $0x80000003, %ecx
    xorl %edx, %edx
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    movq %r10, 0x20(%rsp)
    call win32_raise_exception_impl
    restore_entry_nonvolatiles
    addq $ENTRY_FRAME_SIZE, %rsp
    retq
    .size win32_debug_break_entry, .-win32_debug_break_entry

    .globl RtlRaiseException
    .type RtlRaiseException, @function
RtlRaiseException:
    subq $ENTRY_FRAME_SIZE, %rsp
    capture_entry_context
    movq CTX_RCX(%r10), %rcx
    movq %r10, %rdx
    call win32_rtl_raise_exception_impl
    restore_entry_nonvolatiles
    addq $ENTRY_FRAME_SIZE, %rsp
    retq
    .size RtlRaiseException, .-RtlRaiseException

    .globl RtlRaiseStatus
    .type RtlRaiseStatus, @function
RtlRaiseStatus:
    subq $ENTRY_FRAME_SIZE, %rsp
    capture_entry_context
    movq CTX_RCX(%r10), %rcx
    movq %r10, %rdx
    call win32_rtl_raise_status_impl
    restore_entry_nonvolatiles
    addq $ENTRY_FRAME_SIZE, %rsp
    retq
    .size RtlRaiseStatus, .-RtlRaiseStatus

    .globl RtlUnwind
    .type RtlUnwind, @function
RtlUnwind:
    subq $ENTRY_FRAME_SIZE, %rsp
    capture_entry_context
    movq $0, ENTRY_ARGUMENT5(%rsp)
    movq $0, ENTRY_ARGUMENT6(%rsp)
    movq %r10, %rcx
    xorl %edx, %edx
    call win32_rtl_unwind_entry_impl
    restore_entry_nonvolatiles
    addq $ENTRY_FRAME_SIZE, %rsp
    retq
    .size RtlUnwind, .-RtlUnwind

    .globl RtlUnwindEx
    .type RtlUnwindEx, @function
RtlUnwindEx:
    subq $ENTRY_FRAME_SIZE, %rsp
    capture_entry_context
    movq ENTRY_FRAME_SIZE+0x28(%rsp), %rax
    movq %rax, ENTRY_ARGUMENT5(%rsp)
    movq ENTRY_FRAME_SIZE+0x30(%rsp), %rax
    movq %rax, ENTRY_ARGUMENT6(%rsp)
    movq %r10, %rcx
    movl $1, %edx
    call win32_rtl_unwind_entry_impl
    restore_entry_nonvolatiles
    addq $ENTRY_FRAME_SIZE, %rsp
    retq
    .size RtlUnwindEx, .-RtlUnwindEx

    .globl RtlCaptureContext
    .type RtlCaptureContext, @function
RtlCaptureContext:
    testq %rcx, %rcx
    jz .Lcapture_return
    movq %rax, CTX_RAX(%rcx)
    movq %r10, CTX_R10(%rcx)
    movq %rcx, %r10
    movq %rcx, CTX_RCX(%r10)
    movq %rdx, CTX_RDX(%r10)
    movq %rbx, CTX_RBX(%r10)
    leaq 8(%rsp), %rax
    movq %rax, CTX_RSP(%r10)
    movq %rbp, CTX_RBP(%r10)
    movq %rsi, CTX_RSI(%r10)
    movq %rdi, CTX_RDI(%r10)
    movq %r8,  CTX_R8(%r10)
    movq %r9,  CTX_R9(%r10)
    movq %r11, CTX_R11(%r10)
    movq %r12, CTX_R12(%r10)
    movq %r13, CTX_R13(%r10)
    movq %r14, CTX_R14(%r10)
    movq %r15, CTX_R15(%r10)
    movq (%rsp), %rax
    movq %rax, CTX_RIP(%r10)
    movdqu %xmm0,  CTX_XMM0+0x00(%r10)
    movdqu %xmm1,  CTX_XMM0+0x10(%r10)
    movdqu %xmm2,  CTX_XMM0+0x20(%r10)
    movdqu %xmm3,  CTX_XMM0+0x30(%r10)
    movdqu %xmm4,  CTX_XMM0+0x40(%r10)
    movdqu %xmm5,  CTX_XMM0+0x50(%r10)
    movdqu %xmm6,  CTX_XMM0+0x60(%r10)
    movdqu %xmm7,  CTX_XMM0+0x70(%r10)
    movdqu %xmm8,  CTX_XMM0+0x80(%r10)
    movdqu %xmm9,  CTX_XMM0+0x90(%r10)
    movdqu %xmm10, CTX_XMM0+0xA0(%r10)
    movdqu %xmm11, CTX_XMM0+0xB0(%r10)
    movdqu %xmm12, CTX_XMM0+0xC0(%r10)
    movdqu %xmm13, CTX_XMM0+0xD0(%r10)
    movdqu %xmm14, CTX_XMM0+0xE0(%r10)
    movdqu %xmm15, CTX_XMM0+0xF0(%r10)
    finish_capture
.Lcapture_return:
    retq
    .size RtlCaptureContext, .-RtlCaptureContext

    .globl win32_restore_context_asm
    .type win32_restore_context_asm, @function
win32_restore_context_asm:
    testq %rcx, %rcx
    jz .Lrestore_return
    movq %rcx, %r10
    fxrstor64 0x100(%r10)

    movl CTX_EFLAGS(%r10), %eax
    pushq %rax
    popfq
    movq CTX_RSP(%r10), %r11
    subq $8, %r11
    movq CTX_RIP(%r10), %rax
    movq %rax, (%r11)
    movq %r11, %rsp

    movq CTX_RAX(%r10), %rax
    movq CTX_RCX(%r10), %rcx
    movq CTX_RDX(%r10), %rdx
    movq CTX_RBX(%r10), %rbx
    movq CTX_RBP(%r10), %rbp
    movq CTX_RSI(%r10), %rsi
    movq CTX_RDI(%r10), %rdi
    movq CTX_R8(%r10),  %r8
    movq CTX_R9(%r10),  %r9
    movq CTX_R12(%r10), %r12
    movq CTX_R13(%r10), %r13
    movq CTX_R14(%r10), %r14
    movq CTX_R15(%r10), %r15
    movq CTX_R11(%r10), %r11
    movq CTX_R10(%r10), %r10
    retq
.Lrestore_return:
    retq
    .size win32_restore_context_asm, .-win32_restore_context_asm

/* Kernel-side calls into PE unwind code need their own AMD64 metadata. A
 * nested RtlUnwindEx can then identify the wrapper and recover the dispatcher
 * that was active before the collision. */
    .p2align 4
    .globl win32_unwind64_wrapper_region_start
win32_unwind64_wrapper_region_start:

    .globl win32_unwind64_call_language_handler
    .globl win32_unwind64_call_language_handler_end
    .type win32_unwind64_call_language_handler, @function
win32_unwind64_call_language_handler:
    subq $0x28, %rsp
    movq %r9, 0x20(%rsp)
    callq *0x50(%rsp)
    nop
    addq $0x28, %rsp
    retq
win32_unwind64_call_language_handler_end:
    .size win32_unwind64_call_language_handler, \
          .-win32_unwind64_call_language_handler

    .p2align 4
    .globl win32_unwind64_call_exception_handler
    .globl win32_unwind64_call_exception_handler_end
    .type win32_unwind64_call_exception_handler, @function
win32_unwind64_call_exception_handler:
    subq $0x38, %rsp
    movq %r9, 0x20(%rsp)
    movq 0x68(%rsp), %rax
    movq %rax, 0x28(%rsp)
    callq *0x60(%rsp)
    nop
    addq $0x38, %rsp
    retq
win32_unwind64_call_exception_handler_end:
    .size win32_unwind64_call_exception_handler, \
          .-win32_unwind64_call_exception_handler

    .p2align 4
    .globl win32_unwind64_call_exception_filter
    .globl win32_unwind64_call_exception_filter_end
    .type win32_unwind64_call_exception_filter, @function
win32_unwind64_call_exception_filter:
    subq $0x28, %rsp
    movq %r9, 0x20(%rsp)
    movq %rcx, %rax
    movq %rdx, %rcx
    movq %r8, %rdx
    callq *%rax
    nop
    addq $0x28, %rsp
    retq
win32_unwind64_call_exception_filter_end:
    .size win32_unwind64_call_exception_filter, \
          .-win32_unwind64_call_exception_filter

    .p2align 4
    .globl win32_unwind64_call_termination_handler
    .globl win32_unwind64_call_termination_handler_end
    .type win32_unwind64_call_termination_handler, @function
win32_unwind64_call_termination_handler:
    subq $0x38, %rsp
    movq %r9, 0x20(%rsp)
    movq %rcx, %rax
    movl %edx, %ecx
    movq %r8, %rdx
    callq *%rax
    nop
    addq $0x38, %rsp
    retq
win32_unwind64_call_termination_handler_end:
    .size win32_unwind64_call_termination_handler, \
          .-win32_unwind64_call_termination_handler

/* Invoke an unwind-consolidation callback from a synthetic machine frame.
 * Unwinding through the callback recovers the supplied target context and
 * therefore skips the RtlRestoreContext implementation frames. */
    .p2align 4
    .globl win32_unwind64_call_consolidate
    .globl win32_unwind64_call_consolidate_end
    .type win32_unwind64_call_consolidate, @function
win32_unwind64_call_consolidate:
    pushq %rbp
    movq %rsp, %rbp
    subq $0x28, %rsp
    movq CTX_RIP(%rcx), %rax
    movq %rax, 0x00(%rsp)
    movq CTX_RSP(%rcx), %rax
    movq %rax, 0x18(%rsp)
.Lconsolidate_machine_frame_done:
    subq $0x108, %rsp
.Lconsolidate_allocation_done:
    movq CTX_RBP(%rcx), %rax
    movq %rax, 0x100(%rsp)
.Lconsolidate_rbp_saved:
    movq CTX_RBX(%rcx), %rax
    movq %rax, 0x20(%rsp)
.Lconsolidate_rbx_saved:
    movq CTX_RSI(%rcx), %rax
    movq %rax, 0x28(%rsp)
.Lconsolidate_rsi_saved:
    movq CTX_RDI(%rcx), %rax
    movq %rax, 0x30(%rsp)
.Lconsolidate_rdi_saved:
    movq CTX_R12(%rcx), %rax
    movq %rax, 0x38(%rsp)
.Lconsolidate_r12_saved:
    movq CTX_R13(%rcx), %rax
    movq %rax, 0x40(%rsp)
.Lconsolidate_r13_saved:
    movq CTX_R14(%rcx), %rax
    movq %rax, 0x48(%rsp)
.Lconsolidate_r14_saved:
    movq CTX_R15(%rcx), %rax
    movq %rax, 0x50(%rsp)
.Lconsolidate_r15_saved:
    pushq %rsi
    pushq %rdi
    leaq CTX_XMM0+0x60(%rcx), %rsi
    leaq 0x70(%rsp), %rdi
    movl $20, %ecx
    cld
    rep movsq
    popq %rdi
    popq %rsi
.Lconsolidate_xmm_saved:
.Lconsolidate_prolog_end:
    movq %r8, %rcx
    callq *%rdx
    nop
    leaq 0(%rbp), %rsp
    popq %rbp
    retq
win32_unwind64_call_consolidate_end:
    .size win32_unwind64_call_consolidate, \
          .-win32_unwind64_call_consolidate

    .p2align 2
    .globl win32_unwind64_language_runtime_function
win32_unwind64_language_runtime_function:
    .long win32_unwind64_call_language_handler - \
          win32_unwind64_wrapper_region_start
    .long win32_unwind64_call_language_handler_end - \
          win32_unwind64_wrapper_region_start
    .long win32_unwind64_language_unwind_info - \
          win32_unwind64_wrapper_region_start

    .globl win32_unwind64_exception_runtime_function
win32_unwind64_exception_runtime_function:
    .long win32_unwind64_call_exception_handler - \
          win32_unwind64_wrapper_region_start
    .long win32_unwind64_call_exception_handler_end - \
          win32_unwind64_wrapper_region_start
    .long win32_unwind64_exception_unwind_info - \
          win32_unwind64_wrapper_region_start

    .globl win32_unwind64_filter_runtime_function
win32_unwind64_filter_runtime_function:
    .long win32_unwind64_call_exception_filter - \
          win32_unwind64_wrapper_region_start
    .long win32_unwind64_call_exception_filter_end - \
          win32_unwind64_wrapper_region_start
    .long win32_unwind64_filter_unwind_info - \
          win32_unwind64_wrapper_region_start

    .globl win32_unwind64_termination_runtime_function
win32_unwind64_termination_runtime_function:
    .long win32_unwind64_call_termination_handler - \
          win32_unwind64_wrapper_region_start
    .long win32_unwind64_call_termination_handler_end - \
          win32_unwind64_wrapper_region_start
    .long win32_unwind64_termination_unwind_info - \
          win32_unwind64_wrapper_region_start

    .globl win32_unwind64_consolidate_runtime_function
win32_unwind64_consolidate_runtime_function:
    .long win32_unwind64_call_consolidate - \
          win32_unwind64_wrapper_region_start
    .long win32_unwind64_call_consolidate_end - \
          win32_unwind64_wrapper_region_start
    .long win32_unwind64_consolidate_unwind_info - \
          win32_unwind64_wrapper_region_start

win32_unwind64_language_unwind_info:
    .byte 0x11, 0x04, 0x01, 0x00
    .byte 0x04, 0x42, 0x00, 0x00
    .long win32_unwind64_collision_handler_thunk - \
          win32_unwind64_wrapper_region_start

win32_unwind64_exception_unwind_info:
    .byte 0x19, 0x04, 0x01, 0x00
    .byte 0x04, 0x62, 0x00, 0x00
    .long win32_unwind64_exception_bridge_thunk - \
          win32_unwind64_wrapper_region_start

win32_unwind64_filter_unwind_info:
    .byte 0x19, 0x04, 0x01, 0x00
    .byte 0x04, 0x42, 0x00, 0x00
    .long win32_unwind64_exception_bridge_thunk - \
          win32_unwind64_wrapper_region_start

win32_unwind64_termination_unwind_info:
    .byte 0x19, 0x04, 0x01, 0x00
    .byte 0x04, 0x62, 0x00, 0x00
    .long win32_unwind64_exception_bridge_thunk - \
          win32_unwind64_wrapper_region_start

/* 10 SAVE_XMM128, 8 SAVE_NONVOL, ALLOC_LARGE and PUSH_MACHFRAME. */
win32_unwind64_consolidate_unwind_info:
    .byte 0x01
    .byte .Lconsolidate_prolog_end - win32_unwind64_call_consolidate
    .byte 39
    .byte 0x00
    .byte .Lconsolidate_xmm_saved - win32_unwind64_call_consolidate, 0xF8
    .short 0x000F
    .byte .Lconsolidate_xmm_saved - win32_unwind64_call_consolidate, 0xE8
    .short 0x000E
    .byte .Lconsolidate_xmm_saved - win32_unwind64_call_consolidate, 0xD8
    .short 0x000D
    .byte .Lconsolidate_xmm_saved - win32_unwind64_call_consolidate, 0xC8
    .short 0x000C
    .byte .Lconsolidate_xmm_saved - win32_unwind64_call_consolidate, 0xB8
    .short 0x000B
    .byte .Lconsolidate_xmm_saved - win32_unwind64_call_consolidate, 0xA8
    .short 0x000A
    .byte .Lconsolidate_xmm_saved - win32_unwind64_call_consolidate, 0x98
    .short 0x0009
    .byte .Lconsolidate_xmm_saved - win32_unwind64_call_consolidate, 0x88
    .short 0x0008
    .byte .Lconsolidate_xmm_saved - win32_unwind64_call_consolidate, 0x78
    .short 0x0007
    .byte .Lconsolidate_xmm_saved - win32_unwind64_call_consolidate, 0x68
    .short 0x0006
    .byte .Lconsolidate_r15_saved - win32_unwind64_call_consolidate, 0xF4
    .short 0x000A
    .byte .Lconsolidate_r14_saved - win32_unwind64_call_consolidate, 0xE4
    .short 0x0009
    .byte .Lconsolidate_r13_saved - win32_unwind64_call_consolidate, 0xD4
    .short 0x0008
    .byte .Lconsolidate_r12_saved - win32_unwind64_call_consolidate, 0xC4
    .short 0x0007
    .byte .Lconsolidate_rdi_saved - win32_unwind64_call_consolidate, 0x74
    .short 0x0006
    .byte .Lconsolidate_rsi_saved - win32_unwind64_call_consolidate, 0x64
    .short 0x0005
    .byte .Lconsolidate_rbx_saved - win32_unwind64_call_consolidate, 0x34
    .short 0x0004
    .byte .Lconsolidate_rbp_saved - win32_unwind64_call_consolidate, 0x54
    .short 0x0020
    .byte .Lconsolidate_allocation_done - win32_unwind64_call_consolidate, 0x01
    .short 0x0021
    .byte .Lconsolidate_machine_frame_done - win32_unwind64_call_consolidate, 0x0A
    .short 0x0000

    .p2align 4
    .globl win32_unwind64_collision_handler_thunk
    .globl win32_unwind64_collision_handler_thunk_end
win32_unwind64_collision_handler_thunk:
    jmp win32_unwind64_collision_handler
win32_unwind64_collision_handler_thunk_end:

    .p2align 4
    .globl win32_unwind64_exception_bridge_thunk
    .globl win32_unwind64_exception_bridge_thunk_end
win32_unwind64_exception_bridge_thunk:
    jmp win32_unwind64_exception_bridge_handler
win32_unwind64_exception_bridge_thunk_end:

    .globl win32_unwind64_wrapper_region_end
win32_unwind64_wrapper_region_end:

    .section .note.GNU-stack,"",@progbits
