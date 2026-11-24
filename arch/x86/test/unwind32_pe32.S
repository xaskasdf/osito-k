/* Freestanding PE32 contract test for i386 registration-chain unwind. */

    .set RETVAL_TARGET, 0x13579bdf
    .set RETVAL_NULL_IP, 0x2468ace0
    .set RETVAL_COLLISION, 0xa1b2c3d4
    .set REG_RESTORE_EAX, 0x0a1b2c3d
    .set REG_RESTORE_ECX, 0x10293847
    .set REG_RESTORE_EDX, 0x56473829
    .set REG_CAPTURE_EAX, 0x89abcdef
    .set REG_CAPTURE_ECX, 0x76543210
    .set REG_CAPTURE_EDX, 0xfedcba98
    .set REG_LONGJUMP_EAX, 0x10203040
    .set REG_LONGJUMP_ECX, 0x50607080
    .set REG_LONGJUMP_EDX, 0x90a0b0c0
    .set REG_EBX, 0x11223344
    .set REG_ESI, 0x55667788
    .set REG_EDI, 0x99aabbcc
    .set REG_ECX, 0x31415926
    .set REG_EDX, 0x27182818
    .set STATUS_UNWIND, 0xc0000027
    .set STATUS_LONGJUMP, 0x80000026
    .set TEST_EXCEPTION, 0xe0423201
    .set CONTEXT32_FULL, 0x00010007

    .text
    .globl _mainCRTStartup

_mainCRTStartup:
    calll _exercise_target_ip
    testl %eax, %eax
    jnz .Lexit
    calll _exercise_null_target_ip
    testl %eax, %eax
    jnz .Lexit
    calll _exercise_eh3_finally
    testl %eax, %eax
    jnz .Lexit
    calll _exercise_capture_context
    testl %eax, %eax
    jnz .Lexit
    calll _exercise_restore_context
    testl %eax, %eax
    jnz .Lexit
    calll _exercise_restore_longjump
    testl %eax, %eax
    jnz .Lexit
    calll _exercise_collided_unwind
    testl %eax, %eax
    jnz .Lexit
    calll _exercise_exit_unwind
.Lexit:
    pushl %eax
    calll *__imp__ExitProcess@4
    int3

_exercise_target_ip:
    pushl %ebp
    movl %esp, %ebp
    subl $0x80, %esp

    movl %ebx, _saved_ebx
    movl %esi, _saved_esi
    movl %edi, _saved_edi

    cld
    leal -0x60(%ebp), %edi
    xorl %eax, %eax
    movl $20, %ecx
    rep stosl
    movl $TEST_EXCEPTION, -0x60(%ebp)
    movl $_after_target_call, -0x54(%ebp)

    movl %fs:0, %eax
    movl %eax, -0x08(%ebp)
    movl $_outer_handler, -0x04(%ebp)
    leal -0x08(%ebp), %eax
    movl %eax, _outer_frame
    movl %eax, -0x10(%ebp)
    movl $_inner_handler, -0x0c(%ebp)
    leal -0x10(%ebp), %eax
    movl %eax, _inner_frame
    movl %eax, %fs:0

    leal -0x60(%ebp), %eax
    movl %eax, _record_address
    movl $_after_target_call, _expected_return_eip
    movl %ebp, _expected_ebp
    movl %esp, _expected_esp
    movl $0, _inner_marker
    movl $0, _outer_marker
    movl $0, _handler_failure

    movl $REG_EBX, %ebx
    movl $REG_ESI, %esi
    movl $REG_EDI, %edi
    movl $REG_ECX, %ecx
    movl $REG_EDX, %edx
    stc

    pushl $RETVAL_TARGET
    leal -0x60(%ebp), %eax
    pushl %eax
    pushl $_target_continue
    leal -0x08(%ebp), %eax
    pushl %eax
    calll *__imp__RtlUnwind@16

_after_target_call:
    movl $60, %eax
    jmp .Ltarget_cleanup

_target_continue:
    jnc .Ltarget_bad_eflags
    cmpl $RETVAL_TARGET, %eax
    jne .Ltarget_bad_eax
    cmpl $REG_EBX, %ebx
    jne .Ltarget_bad_ebx
    cmpl $REG_ESI, %esi
    jne .Ltarget_bad_esi
    cmpl $REG_EDI, %edi
    jne .Ltarget_bad_edi
    cmpl $REG_ECX, %ecx
    jne .Ltarget_bad_volatile
    cmpl $REG_EDX, %edx
    jne .Ltarget_bad_volatile
    cmpl _expected_ebp, %ebp
    jne .Ltarget_bad_ebp
    cmpl _expected_esp, %esp
    jne .Ltarget_bad_esp
    movl %fs:0, %edx
    cmpl _outer_frame, %edx
    jne .Ltarget_bad_chain
    cmpl $1, _inner_marker
    jne .Ltarget_bad_handler
    cmpl $0, _outer_marker
    jne .Ltarget_bad_outer
    cmpl $0, _handler_failure
    jne .Ltarget_handler_failure
    movl _record_address, %edx
    testl $0x02, 4(%edx)
    jz .Ltarget_bad_flags
    testl $0x04, 4(%edx)
    jnz .Ltarget_bad_flags
    xorl %eax, %eax
    jmp .Ltarget_cleanup

.Ltarget_bad_eax:
    movl $61, %eax
    jmp .Ltarget_cleanup
.Ltarget_bad_ebx:
    movl $62, %eax
    jmp .Ltarget_cleanup
.Ltarget_bad_esi:
    movl $63, %eax
    jmp .Ltarget_cleanup
.Ltarget_bad_edi:
    movl $64, %eax
    jmp .Ltarget_cleanup
.Ltarget_bad_ebp:
    movl $65, %eax
    jmp .Ltarget_cleanup
.Ltarget_bad_esp:
    movl $66, %eax
    jmp .Ltarget_cleanup
.Ltarget_bad_chain:
    movl $67, %eax
    jmp .Ltarget_cleanup
.Ltarget_bad_handler:
    movl $68, %eax
    jmp .Ltarget_cleanup
.Ltarget_bad_outer:
    movl $69, %eax
    jmp .Ltarget_cleanup
.Ltarget_handler_failure:
    movl _handler_failure, %eax
    jmp .Ltarget_cleanup
.Ltarget_bad_flags:
    movl $70, %eax
    jmp .Ltarget_cleanup
.Ltarget_bad_eflags:
    movl $77, %eax
    jmp .Ltarget_cleanup
.Ltarget_bad_volatile:
    movl $78, %eax

.Ltarget_cleanup:
    movl _outer_frame, %edx
    movl (%edx), %ecx
    movl %ecx, %fs:0
    movl _saved_ebx, %ebx
    movl _saved_esi, %esi
    movl _saved_edi, %edi
    movl %ebp, %esp
    popl %ebp
    retl

_exercise_null_target_ip:
    pushl %ebp
    movl %esp, %ebp
    subl $0x80, %esp

    movl %ebx, _saved_ebx
    movl %esi, _saved_esi
    movl %edi, _saved_edi

    movl %fs:0, %eax
    movl %eax, -0x08(%ebp)
    movl $_outer_handler, -0x04(%ebp)
    leal -0x08(%ebp), %eax
    movl %eax, _outer_frame
    movl %eax, -0x10(%ebp)
    movl $_inner_handler, -0x0c(%ebp)
    leal -0x10(%ebp), %eax
    movl %eax, _inner_frame
    movl %eax, %fs:0

    movl $0, _record_address
    movl $_after_null_target_call, _expected_return_eip
    movl %ebp, _expected_ebp
    movl %esp, _expected_esp
    movl $0, _inner_marker
    movl $0, _outer_marker
    movl $0, _handler_failure

    movl $REG_EBX, %ebx
    movl $REG_ESI, %esi
    movl $REG_EDI, %edi
    movl $REG_ECX, %ecx
    movl $REG_EDX, %edx
    stc

    pushl $RETVAL_NULL_IP
    pushl $0
    pushl $0
    leal -0x08(%ebp), %eax
    pushl %eax
    calll *__imp__RtlUnwind@16

_after_null_target_call:
    jnc .Lnull_bad_eflags
    cmpl $RETVAL_NULL_IP, %eax
    jne .Lnull_bad_eax
    cmpl $REG_EBX, %ebx
    jne .Lnull_bad_regs
    cmpl $REG_ESI, %esi
    jne .Lnull_bad_regs
    cmpl $REG_EDI, %edi
    jne .Lnull_bad_regs
    cmpl $REG_ECX, %ecx
    jne .Lnull_bad_volatile
    cmpl $REG_EDX, %edx
    jne .Lnull_bad_volatile
    cmpl _expected_ebp, %ebp
    jne .Lnull_bad_regs
    cmpl _expected_esp, %esp
    jne .Lnull_bad_stack
    movl %fs:0, %edx
    cmpl _outer_frame, %edx
    jne .Lnull_bad_chain
    cmpl $1, _inner_marker
    jne .Lnull_bad_handler
    cmpl $0, _outer_marker
    jne .Lnull_bad_outer
    cmpl $0, _handler_failure
    jne .Lnull_handler_failure
    xorl %eax, %eax
    jmp .Lnull_cleanup

.Lnull_bad_eax:
    movl $71, %eax
    jmp .Lnull_cleanup
.Lnull_bad_regs:
    movl $72, %eax
    jmp .Lnull_cleanup
.Lnull_bad_stack:
    movl $73, %eax
    jmp .Lnull_cleanup
.Lnull_bad_chain:
    movl $74, %eax
    jmp .Lnull_cleanup
.Lnull_bad_handler:
    movl $75, %eax
    jmp .Lnull_cleanup
.Lnull_bad_outer:
    movl $76, %eax
    jmp .Lnull_cleanup
.Lnull_handler_failure:
    movl _handler_failure, %eax
    jmp .Lnull_cleanup
.Lnull_bad_eflags:
    movl $79, %eax
    jmp .Lnull_cleanup
.Lnull_bad_volatile:
    movl $80, %eax

.Lnull_cleanup:
    movl _outer_frame, %edx
    movl (%edx), %ecx
    movl %ecx, %fs:0
    movl _saved_ebx, %ebx
    movl _saved_esi, %esi
    movl _saved_edi, %edi
    movl %ebp, %esp
    popl %ebp
    retl

_exercise_eh3_finally:
    pushl %ebp
    movl %esp, %ebp
    subl $0x80, %esp

    movl %ebx, _saved_ebx
    movl %esi, _saved_esi
    movl %edi, _saved_edi

    movl %fs:0, %eax
    movl %eax, -0x08(%ebp)
    movl $_outer_handler, -0x04(%ebp)
    leal -0x08(%ebp), %eax
    movl %eax, _outer_frame
    movl %eax, -0x18(%ebp)
    movl $_eh3_registration_handler, -0x14(%ebp)
    movl $_eh3_scope_table, -0x10(%ebp)
    movl $0, -0x0c(%ebp)
    leal -0x18(%ebp), %eax
    movl %eax, _inner_frame
    movl %eax, %fs:0

    movl %ebp, _expected_ebp
    movl %esp, _expected_esp
    movl $0, _outer_marker
    movl $0, _finally_marker
    movl $0, _handler_failure

    movl $REG_EBX, %ebx
    movl $REG_ESI, %esi
    movl $REG_EDI, %edi

    pushl $RETVAL_TARGET
    pushl $0
    pushl $_eh3_continue
    leal -0x08(%ebp), %eax
    pushl %eax
    calll *__imp__RtlUnwind@16

.Leh3_returned:
    movl $81, %eax
    jmp .Leh3_cleanup

_eh3_continue:
    cmpl $RETVAL_TARGET, %eax
    jne .Leh3_bad_eax
    cmpl $REG_EBX, %ebx
    jne .Leh3_bad_regs
    cmpl $REG_ESI, %esi
    jne .Leh3_bad_regs
    cmpl $REG_EDI, %edi
    jne .Leh3_bad_regs
    cmpl _expected_ebp, %ebp
    jne .Leh3_bad_regs
    cmpl _expected_esp, %esp
    jne .Leh3_bad_stack
    movl %fs:0, %edx
    cmpl _outer_frame, %edx
    jne .Leh3_bad_chain
    cmpl $1, _finally_marker
    jne .Leh3_bad_finally
    cmpl $0, _outer_marker
    jne .Leh3_bad_outer
    movl _inner_frame, %edx
    cmpl $-1, 12(%edx)
    jne .Leh3_bad_level
    cmpl $0, _handler_failure
    jne .Leh3_handler_failure
    xorl %eax, %eax
    jmp .Leh3_cleanup

.Leh3_bad_eax:
    movl $82, %eax
    jmp .Leh3_cleanup
.Leh3_bad_regs:
    movl $83, %eax
    jmp .Leh3_cleanup
.Leh3_bad_stack:
    movl $84, %eax
    jmp .Leh3_cleanup
.Leh3_bad_chain:
    movl $85, %eax
    jmp .Leh3_cleanup
.Leh3_bad_finally:
    movl $86, %eax
    jmp .Leh3_cleanup
.Leh3_bad_outer:
    movl $87, %eax
    jmp .Leh3_cleanup
.Leh3_bad_level:
    movl $88, %eax
    jmp .Leh3_cleanup
.Leh3_handler_failure:
    movl _handler_failure, %eax

.Leh3_cleanup:
    movl _outer_frame, %edx
    movl (%edx), %ecx
    movl %ecx, %fs:0
    movl _saved_ebx, %ebx
    movl _saved_esi, %esi
    movl _saved_edi, %edi
    movl %ebp, %esp
    popl %ebp
    retl

_eh3_registration_handler:
    pushl %ebp
    movl %esp, %ebp
    pushl 20(%ebp)
    pushl 16(%ebp)
    pushl 12(%ebp)
    pushl 8(%ebp)
    calll __except_handler3
    addl $16, %esp
    popl %ebp
    retl

_eh3_finally:
    cmpl _outer_frame, %ebp
    jne .Leh3_finally_bad_ebp
    movl $1, _finally_marker
    retl
.Leh3_finally_bad_ebp:
    movl $89, _handler_failure
    retl

_exercise_capture_context:
    pushl %ebp
    movl %esp, %ebp
    subl $0x300, %esp

    movl %ebx, _saved_ebx
    movl %esi, _saved_esi
    movl %edi, _saved_edi

    leal -0x2e0(%ebp), %eax
    movl %eax, -0x300(%ebp)
    cld
    movl %eax, %edi
    xorl %eax, %eax
    movl $179, %ecx
    rep stosl

    movl $REG_EBX, %ebx
    movl $REG_ESI, %esi
    movl $REG_EDI, %edi
    movl $REG_CAPTURE_ECX, %ecx
    movl $REG_CAPTURE_EDX, %edx
    movl $REG_CAPTURE_EAX, %eax
    stc
    pushl -0x300(%ebp)
    calll *__imp__RtlCaptureContext@4

_capture_context_resume:
    jnc .Lcapture_bad_return_flags
    movl -0x300(%ebp), %eax
    cmpl $CONTEXT32_FULL, 0(%eax)
    jne .Lcapture_bad_context_flags
    testl $1, 192(%eax)
    jz .Lcapture_bad_flags
    cmpl $REG_CAPTURE_EAX, 176(%eax)
    jne .Lcapture_bad_eax
    cmpl $REG_EBX, 164(%eax)
    jne .Lcapture_bad_integer
    cmpl $REG_ESI, 160(%eax)
    jne .Lcapture_bad_integer
    cmpl $REG_EDI, 156(%eax)
    jne .Lcapture_bad_integer
    cmpl $REG_CAPTURE_ECX, 172(%eax)
    jne .Lcapture_bad_integer
    cmpl $REG_CAPTURE_EDX, 168(%eax)
    jne .Lcapture_bad_integer
    cmpl %ebp, 180(%eax)
    jne .Lcapture_bad_ebp
    cmpl $_capture_context_resume, 184(%eax)
    jne .Lcapture_bad_eip
    cmpl %esp, 196(%eax)
    jne .Lcapture_bad_esp

    xorl %edx, %edx
    movw %cs, %dx
    cmpl %edx, 188(%eax)
    jne .Lcapture_bad_segments
    xorl %edx, %edx
    movw %ss, %dx
    cmpl %edx, 200(%eax)
    jne .Lcapture_bad_segments
    xorl %edx, %edx
    movw %ds, %dx
    cmpl %edx, 152(%eax)
    jne .Lcapture_bad_segments
    xorl %edx, %edx
    movw %es, %dx
    cmpl %edx, 148(%eax)
    jne .Lcapture_bad_segments
    xorl %edx, %edx
    movw %fs, %dx
    cmpl %edx, 144(%eax)
    jne .Lcapture_bad_segments
    xorl %edx, %edx
    movw %gs, %dx
    cmpl %edx, 140(%eax)
    jne .Lcapture_bad_segments

    xorl %eax, %eax
    jmp .Lcapture_cleanup

.Lcapture_bad_context_flags:
    movl $137, %eax
    jmp .Lcapture_cleanup
.Lcapture_bad_flags:
    movl $138, %eax
    jmp .Lcapture_cleanup
.Lcapture_bad_eax:
    movl $139, %eax
    jmp .Lcapture_cleanup
.Lcapture_bad_integer:
    movl $140, %eax
    jmp .Lcapture_cleanup
.Lcapture_bad_ebp:
    movl $141, %eax
    jmp .Lcapture_cleanup
.Lcapture_bad_eip:
    movl $142, %eax
    jmp .Lcapture_cleanup
.Lcapture_bad_esp:
    movl $143, %eax
    jmp .Lcapture_cleanup
.Lcapture_bad_segments:
    movl $144, %eax
    jmp .Lcapture_cleanup
.Lcapture_bad_return_flags:
    movl $145, %eax

.Lcapture_cleanup:
    movl _saved_ebx, %ebx
    movl _saved_esi, %esi
    movl _saved_edi, %edi
    movl %ebp, %esp
    popl %ebp
    retl

_exercise_restore_context:
    pushl %ebp
    movl %esp, %ebp
    subl $0x400, %esp

    movl %ebx, _saved_ebx
    movl %esi, _saved_esi
    movl %edi, _saved_edi
    movl %fs:0, %eax
    movl %eax, _restore_saved_chain

    /* Install a valid registration below the restored ESP. It must be
     * removed before control resumes or FS:[0] would point into dead stack. */
    leal -0x3e0(%ebp), %edx
    movl %eax, (%edx)
    movl $_outer_handler, 4(%edx)
    movl %edx, _restore_frame
    movl %edx, %fs:0

    cld
    leal -0x300(%ebp), %edi
    xorl %eax, %eax
    movl $179, %ecx
    rep stosl

    leal -0x300(%ebp), %eax
    movl $CONTEXT32_FULL, 0(%eax)
    movl $REG_EDI, 156(%eax)
    movl $REG_ESI, 160(%eax)
    movl $REG_EBX, 164(%eax)
    movl $REG_RESTORE_EDX, 168(%eax)
    movl $REG_RESTORE_ECX, 172(%eax)
    movl $REG_RESTORE_EAX, 176(%eax)
    movl %ebp, 180(%eax)
    movl $_restore_continue, 184(%eax)
    movl $0x00003003, 192(%eax)
    leal -0x20(%ebp), %edx
    movl %edx, 196(%eax)
    movl %edx, _expected_esp
    movl %ebp, _expected_ebp

    clc
    pushl $0
    pushl %eax
    calll *__imp__RtlRestoreContext
    addl $8, %esp
    movl $124, %eax
    jmp .Lrestore_cleanup

_restore_continue:
    jnc .Lrestore_bad_flags
    cmpl $REG_RESTORE_EAX, %eax
    jne .Lrestore_bad_eax
    cmpl $REG_EBX, %ebx
    jne .Lrestore_bad_regs
    cmpl $REG_ESI, %esi
    jne .Lrestore_bad_regs
    cmpl $REG_EDI, %edi
    jne .Lrestore_bad_regs
    cmpl $REG_RESTORE_ECX, %ecx
    jne .Lrestore_bad_regs
    cmpl $REG_RESTORE_EDX, %edx
    jne .Lrestore_bad_regs
    cmpl _expected_ebp, %ebp
    jne .Lrestore_bad_control
    cmpl _expected_esp, %esp
    jne .Lrestore_bad_control
    movl %fs:0, %edx
    cmpl _restore_saved_chain, %edx
    jne .Lrestore_bad_chain
    pushfl
    popl %edx
    testl $0x00003000, %edx
    jnz .Lrestore_bad_privileged_flags

    /* A new gateway transition verifies that the restore consumed exactly
     * its own INT2E entry and left no pending control transfer behind. */
    calll *__imp__GetCurrentProcessId@0
    testl %eax, %eax
    jz .Lrestore_bad_gateway
    xorl %eax, %eax
    jmp .Lrestore_cleanup

.Lrestore_bad_eax:
    movl $125, %eax
    jmp .Lrestore_cleanup
.Lrestore_bad_regs:
    movl $126, %eax
    jmp .Lrestore_cleanup
.Lrestore_bad_control:
    movl $127, %eax
    jmp .Lrestore_cleanup
.Lrestore_bad_chain:
    movl $128, %eax
    jmp .Lrestore_cleanup
.Lrestore_bad_flags:
    movl $129, %eax
    jmp .Lrestore_cleanup
.Lrestore_bad_privileged_flags:
    movl $130, %eax
    jmp .Lrestore_cleanup
.Lrestore_bad_gateway:
    movl $131, %eax

.Lrestore_cleanup:
    movl _restore_saved_chain, %edx
    movl %edx, %fs:0
    movl _saved_ebx, %ebx
    movl _saved_esi, %esi
    movl _saved_edi, %edi
    movl %ebp, %esp
    popl %ebp
    retl

_exercise_restore_longjump:
    pushl %ebp
    movl %esp, %ebp
    subl $0x400, %esp

    movl %ebx, _saved_ebx
    movl %esi, _saved_esi
    movl %edi, _saved_edi

    cld
    leal -0x300(%ebp), %edi
    xorl %eax, %eax
    movl $179, %ecx
    rep stosl
    leal -0x360(%ebp), %edi
    movl $20, %ecx
    rep stosl
    leal -0x3a0(%ebp), %edi
    movl $16, %ecx
    rep stosl

    /* The CONTEXT deliberately carries invalid control/nonvolatile values;
     * STATUS_LONGJUMP must replace them from the i386 jump buffer. */
    leal -0x300(%ebp), %eax
    movl $CONTEXT32_FULL, 0(%eax)
    movl $1, 156(%eax)
    movl $2, 160(%eax)
    movl $3, 164(%eax)
    movl $REG_LONGJUMP_EDX, 168(%eax)
    movl $REG_LONGJUMP_ECX, 172(%eax)
    movl $REG_LONGJUMP_EAX, 176(%eax)
    movl $0, 180(%eax)
    movl $0, 184(%eax)
    movl $0x00000202, 192(%eax)
    movl $0, 196(%eax)

    leal -0x3a0(%ebp), %edx
    movl %ebp, 0(%edx)
    movl $REG_EBX, 4(%edx)
    movl $REG_EDI, 8(%edx)
    movl $REG_ESI, 12(%edx)
    leal -0x20(%ebp), %ecx
    movl %ecx, 16(%edx)
    movl $_restore_longjump_continue, 20(%edx)
    movl %ecx, _expected_esp
    movl %ebp, _expected_ebp

    leal -0x360(%ebp), %ecx
    movl $STATUS_LONGJUMP, 0(%ecx)
    movl $1, 16(%ecx)
    movl %edx, 20(%ecx)

    stc
    pushl %ecx
    pushl %eax
    calll *__imp__RtlRestoreContext
    addl $8, %esp
    movl $132, %eax
    jmp .Lrestore_longjump_cleanup

_restore_longjump_continue:
    jc .Lrestore_longjump_bad_flags
    cmpl $REG_LONGJUMP_EAX, %eax
    jne .Lrestore_longjump_bad_volatile
    cmpl $REG_LONGJUMP_ECX, %ecx
    jne .Lrestore_longjump_bad_volatile
    cmpl $REG_LONGJUMP_EDX, %edx
    jne .Lrestore_longjump_bad_volatile
    cmpl $REG_EBX, %ebx
    jne .Lrestore_longjump_bad_nonvolatile
    cmpl $REG_ESI, %esi
    jne .Lrestore_longjump_bad_nonvolatile
    cmpl $REG_EDI, %edi
    jne .Lrestore_longjump_bad_nonvolatile
    cmpl _expected_ebp, %ebp
    jne .Lrestore_longjump_bad_control
    cmpl _expected_esp, %esp
    jne .Lrestore_longjump_bad_control
    xorl %eax, %eax
    jmp .Lrestore_longjump_cleanup

.Lrestore_longjump_bad_flags:
    movl $133, %eax
    jmp .Lrestore_longjump_cleanup
.Lrestore_longjump_bad_volatile:
    movl $134, %eax
    jmp .Lrestore_longjump_cleanup
.Lrestore_longjump_bad_nonvolatile:
    movl $135, %eax
    jmp .Lrestore_longjump_cleanup
.Lrestore_longjump_bad_control:
    movl $136, %eax

.Lrestore_longjump_cleanup:
    movl _saved_ebx, %ebx
    movl _saved_esi, %esi
    movl _saved_edi, %edi
    movl %ebp, %esp
    popl %ebp
    retl

_exercise_collided_unwind:
    pushl %ebp
    movl %esp, %ebp
    subl $0x40, %esp

    movl %ebx, _saved_ebx
    movl %esi, _saved_esi
    movl %edi, _saved_edi

    movl %fs:0, %eax
    movl %eax, -0x08(%ebp)
    movl $_outer_handler, -0x04(%ebp)
    leal -0x08(%ebp), %eax
    movl %eax, _outer_frame
    movl %eax, _collision_target_frame
    movl %eax, -0x10(%ebp)
    movl $_collision_inner_handler, -0x0c(%ebp)
    leal -0x10(%ebp), %eax
    movl %eax, _inner_frame
    movl %eax, %fs:0

    movl %ebp, _expected_ebp
    movl %esp, _expected_esp
    movl $0, _outer_marker
    movl $0, _handler_failure
    movl $0, _collision_handler_calls
    movl $0, _collision_first_target_seen
    movl $0, _collision_nested_returned

    movl $REG_EBX, %ebx
    movl $REG_ESI, %esi
    movl $REG_EDI, %edi

    pushl $RETVAL_TARGET
    pushl $0
    pushl $_collision_first_target
    leal -0x08(%ebp), %eax
    pushl %eax
    calll *__imp__RtlUnwind@16

_collision_first_target:
    movl $1, _collision_first_target_seen
    movl $110, %eax
    jmp .Lcollision_cleanup

_collision_second_target:
    cmpl $RETVAL_COLLISION, %eax
    jne .Lcollision_bad_eax
    cmpl _expected_ebp, %ebp
    jne .Lcollision_bad_context
    cmpl _expected_esp, %esp
    jne .Lcollision_bad_context
    cmpl $REG_EBX, %ebx
    jne .Lcollision_bad_context
    cmpl $REG_ESI, %esi
    jne .Lcollision_bad_context
    cmpl $REG_EDI, %edi
    jne .Lcollision_bad_context
    movl %fs:0, %edx
    cmpl _collision_target_frame, %edx
    jne .Lcollision_bad_chain
    cmpl $1, _collision_handler_calls
    jne .Lcollision_bad_calls
    cmpl $0, _collision_first_target_seen
    jne .Lcollision_bad_target
    cmpl $0, _collision_nested_returned
    jne .Lcollision_bad_return
    cmpl $0, _outer_marker
    jne .Lcollision_bad_outer
    cmpl $0, _handler_failure
    jne .Lcollision_handler_failure

    /* A fresh gateway call catches stale callback/INT2E ownership. */
    calll *__imp__GetCurrentProcessId@0
    testl %eax, %eax
    jz .Lcollision_bad_gateway
    xorl %eax, %eax
    jmp .Lcollision_cleanup

.Lcollision_bad_eax:
    movl $111, %eax
    jmp .Lcollision_cleanup
.Lcollision_bad_context:
    movl $112, %eax
    jmp .Lcollision_cleanup
.Lcollision_bad_chain:
    movl $113, %eax
    jmp .Lcollision_cleanup
.Lcollision_bad_calls:
    movl $114, %eax
    jmp .Lcollision_cleanup
.Lcollision_bad_target:
    movl $115, %eax
    jmp .Lcollision_cleanup
.Lcollision_bad_return:
    movl $116, %eax
    jmp .Lcollision_cleanup
.Lcollision_bad_outer:
    movl $117, %eax
    jmp .Lcollision_cleanup
.Lcollision_bad_gateway:
    movl $118, %eax
    jmp .Lcollision_cleanup
.Lcollision_handler_failure:
    movl _handler_failure, %eax

.Lcollision_cleanup:
    movl _collision_target_frame, %edx
    movl (%edx), %ecx
    movl %ecx, %fs:0
    movl _saved_ebx, %ebx
    movl _saved_esi, %esi
    movl _saved_edi, %edi
    movl %ebp, %esp
    popl %ebp
    retl

_collision_inner_handler:
    pushl %ebp
    movl %esp, %ebp
    cmpl $0, _collision_handler_calls
    jne .Lcollision_handler_reentered
    movl $1, _collision_handler_calls

    movl 8(%ebp), %eax
    testl %eax, %eax
    jz .Lcollision_handler_bad_record
    cmpl $STATUS_UNWIND, (%eax)
    jne .Lcollision_handler_bad_record
    testl $0x02, 4(%eax)
    jz .Lcollision_handler_bad_flags
    testl $0x04, 4(%eax)
    jnz .Lcollision_handler_bad_flags
    movl 12(%ebp), %edx
    cmpl _inner_frame, %edx
    jne .Lcollision_handler_bad_frame
    cmpl $0, 20(%ebp)
    je .Lcollision_handler_bad_dispatcher

    pushl $RETVAL_COLLISION
    pushl $0
    pushl $_collision_second_target
    pushl _collision_target_frame
    calll *__imp__RtlUnwind@16

    movl $1, _collision_nested_returned
    movl $116, _handler_failure
    jmp .Lcollision_handler_return

.Lcollision_handler_reentered:
    movl $119, _handler_failure
    jmp .Lcollision_handler_return
.Lcollision_handler_bad_record:
    movl $120, _handler_failure
    jmp .Lcollision_handler_return
.Lcollision_handler_bad_flags:
    movl $121, _handler_failure
    jmp .Lcollision_handler_return
.Lcollision_handler_bad_frame:
    movl $122, _handler_failure
    jmp .Lcollision_handler_return
.Lcollision_handler_bad_dispatcher:
    movl $123, _handler_failure
.Lcollision_handler_return:
    movl $1, %eax
    popl %ebp
    retl

_exercise_exit_unwind:
    pushl %ebp
    movl %esp, %ebp
    subl $0x80, %esp

    cld
    leal -0x60(%ebp), %edi
    xorl %eax, %eax
    movl $20, %ecx
    rep stosl
    movl $90, -0x60(%ebp)
    movl $_exit_unexpected, -0x54(%ebp)

    movl %fs:0, %eax
    movl %eax, -0x08(%ebp)
    movl $_exit_outer_handler, -0x04(%ebp)
    leal -0x08(%ebp), %eax
    movl %eax, _outer_frame
    movl %eax, -0x10(%ebp)
    movl $_exit_inner_handler, -0x0c(%ebp)
    leal -0x10(%ebp), %eax
    movl %eax, _inner_frame
    movl %eax, %fs:0

    leal -0x60(%ebp), %eax
    movl %eax, _record_address
    movl $_exit_unexpected, _expected_return_eip
    movl $0, _exit_sequence
    movl $0, _exit_failure

    pushl $RETVAL_TARGET
    leal -0x60(%ebp), %eax
    pushl %eax
    pushl $1
    pushl $0
    calll *__imp__RtlUnwind@16

_exit_unexpected:
    movl $99, %eax
    movl %ebp, %esp
    popl %ebp
    retl

_exit_inner_handler:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    cmpl _record_address, %eax
    jne .Lexit_inner_record
    testl $0x02, 4(%eax)
    jz .Lexit_inner_flags
    testl $0x04, 4(%eax)
    jz .Lexit_inner_flags
    movl 12(%ebp), %edx
    cmpl _inner_frame, %edx
    jne .Lexit_inner_frame
    movl 16(%ebp), %edx
    testl %edx, %edx
    jz .Lexit_inner_context
    cmpl $RETVAL_TARGET, 176(%edx)
    jne .Lexit_inner_context
    movl 184(%edx), %ecx
    cmpl _expected_return_eip, %ecx
    jne .Lexit_inner_context
    cmpl $0, 20(%ebp)
    je .Lexit_inner_dispatcher
    cmpl $0, _exit_sequence
    jne .Lexit_inner_order
    movl $1, _exit_sequence
    jmp .Lexit_inner_return

.Lexit_inner_record:
    movl $91, %edx
    jmp .Lexit_inner_fail
.Lexit_inner_flags:
    movl $92, %edx
    jmp .Lexit_inner_fail
.Lexit_inner_frame:
    movl $93, %edx
    jmp .Lexit_inner_fail
.Lexit_inner_context:
    movl $94, %edx
    jmp .Lexit_inner_fail
.Lexit_inner_dispatcher:
    movl $95, %edx
    jmp .Lexit_inner_fail
.Lexit_inner_order:
    movl $96, %edx
.Lexit_inner_fail:
    movl %edx, _exit_failure
    movl %edx, (%eax)
    movl $1, _exit_sequence
.Lexit_inner_return:
    movl $1, %eax
    popl %ebp
    retl

_exit_outer_handler:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    cmpl _record_address, %eax
    jne .Lexit_outer_record
    testl $0x02, 4(%eax)
    jz .Lexit_outer_flags
    testl $0x04, 4(%eax)
    jz .Lexit_outer_flags
    movl 12(%ebp), %edx
    cmpl _outer_frame, %edx
    jne .Lexit_outer_frame
    cmpl $1, _exit_sequence
    jne .Lexit_outer_order
    cmpl $0, _exit_failure
    jne .Lexit_outer_return
    movl $2, _exit_sequence
    movl $0, (%eax)
    jmp .Lexit_outer_return

.Lexit_outer_record:
    movl $97, %edx
    jmp .Lexit_outer_fail
.Lexit_outer_flags:
    movl $98, %edx
    jmp .Lexit_outer_fail
.Lexit_outer_frame:
    movl $100, %edx
    jmp .Lexit_outer_fail
.Lexit_outer_order:
    movl $101, %edx
.Lexit_outer_fail:
    movl %edx, _exit_failure
    movl %edx, (%eax)
.Lexit_outer_return:
    movl $1, %eax
    popl %ebp
    retl

_inner_handler:
    pushl %ebp
    movl %esp, %ebp
    movl $1, _inner_marker

    movl 8(%ebp), %eax
    testl %eax, %eax
    jz .Lhandler_bad_record
    movl _record_address, %edx
    testl %edx, %edx
    jz .Lhandler_local_record
    cmpl %edx, %eax
    jne .Lhandler_bad_record
    cmpl $TEST_EXCEPTION, (%eax)
    jne .Lhandler_bad_code
    jmp .Lhandler_record_flags
.Lhandler_local_record:
    cmpl $STATUS_UNWIND, (%eax)
    jne .Lhandler_bad_code
    movl 12(%eax), %edx
    cmpl _expected_return_eip, %edx
    jne .Lhandler_bad_address
.Lhandler_record_flags:
    testl $0x02, 4(%eax)
    jz .Lhandler_bad_flags
    testl $0x04, 4(%eax)
    jnz .Lhandler_bad_flags

    movl 12(%ebp), %eax
    cmpl _inner_frame, %eax
    jne .Lhandler_bad_frame
    movl 16(%ebp), %eax
    testl %eax, %eax
    jz .Lhandler_bad_context
    movl 184(%eax), %edx
    cmpl _expected_return_eip, %edx
    jne .Lhandler_bad_context_eip
    movl 196(%eax), %edx
    cmpl _expected_esp, %edx
    jne .Lhandler_bad_context_esp
    movl 180(%eax), %edx
    cmpl _expected_ebp, %edx
    jne .Lhandler_bad_context_ebp
    cmpl $REG_EBX, 164(%eax)
    jne .Lhandler_bad_context_regs
    cmpl $REG_ESI, 160(%eax)
    jne .Lhandler_bad_context_regs
    cmpl $REG_EDI, 156(%eax)
    jne .Lhandler_bad_context_regs
    cmpl $REG_ECX, 172(%eax)
    jne .Lhandler_bad_context_regs
    cmpl $REG_EDX, 168(%eax)
    jne .Lhandler_bad_context_regs
    movl _record_address, %edx
    testl %edx, %edx
    jz .Lhandler_expect_null_retval
    cmpl $RETVAL_TARGET, 176(%eax)
    jne .Lhandler_bad_context_eax
    jmp .Lhandler_dispatcher
.Lhandler_expect_null_retval:
    cmpl $RETVAL_NULL_IP, 176(%eax)
    jne .Lhandler_bad_context_eax
.Lhandler_dispatcher:
    cmpl $0, 20(%ebp)
    je .Lhandler_bad_dispatcher
    jmp .Lhandler_return

.Lhandler_bad_record:
    movl $20, _handler_failure
    jmp .Lhandler_return
.Lhandler_bad_code:
    movl $21, _handler_failure
    jmp .Lhandler_return
.Lhandler_bad_address:
    movl $22, _handler_failure
    jmp .Lhandler_return
.Lhandler_bad_flags:
    movl $23, _handler_failure
    jmp .Lhandler_return
.Lhandler_bad_frame:
    movl $24, _handler_failure
    jmp .Lhandler_return
.Lhandler_bad_context:
    movl $25, _handler_failure
    jmp .Lhandler_return
.Lhandler_bad_context_eip:
    movl $26, _handler_failure
    jmp .Lhandler_return
.Lhandler_bad_context_esp:
    movl $27, _handler_failure
    jmp .Lhandler_return
.Lhandler_bad_context_ebp:
    movl $28, _handler_failure
    jmp .Lhandler_return
.Lhandler_bad_context_regs:
    movl $29, _handler_failure
    jmp .Lhandler_return
.Lhandler_bad_context_eax:
    movl $30, _handler_failure
    jmp .Lhandler_return
.Lhandler_bad_dispatcher:
    movl $31, _handler_failure
.Lhandler_return:
    movl $1, %eax
    popl %ebp
    retl

_outer_handler:
    movl $1, _outer_marker
    movl $1, %eax
    retl

    .align 4
_eh3_scope_table:
    .long -1, 0, _eh3_finally

    .data
    .align 4
_outer_frame:
    .long 0
_inner_frame:
    .long 0
_record_address:
    .long 0
_expected_return_eip:
    .long 0
_expected_ebp:
    .long 0
_expected_esp:
    .long 0
_saved_ebx:
    .long 0
_saved_esi:
    .long 0
_saved_edi:
    .long 0
_inner_marker:
    .long 0
_outer_marker:
    .long 0
_handler_failure:
    .long 0
_finally_marker:
    .long 0
_collision_target_frame:
    .long 0
_collision_handler_calls:
    .long 0
_collision_first_target_seen:
    .long 0
_collision_nested_returned:
    .long 0
_exit_sequence:
    .long 0
_exit_failure:
    .long 0
_restore_saved_chain:
    .long 0
_restore_frame:
    .long 0
