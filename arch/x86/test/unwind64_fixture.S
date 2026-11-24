    .text

    .globl unwind64_fixture
    .globl unwind64_fixture_after_push
    .globl unwind64_fixture_body
    .globl unwind64_fixture_epilog
    .seh_proc unwind64_fixture
unwind64_fixture:
    pushq %rbx
    .seh_pushreg %rbx
unwind64_fixture_after_push:
    subq $0x30, %rsp
    .seh_stackalloc 0x30
    movq %rsi, 0x20(%rsp)
    .seh_savereg %rsi, 0x20
    .seh_endprologue
unwind64_fixture_body:
    xorl %eax, %eax
    movq 0x20(%rsp), %rsi
unwind64_fixture_epilog:
    addq $0x30, %rsp
    popq %rbx
    retq
    .seh_endproc

    .globl unwind64_frame_fixture
    .globl unwind64_frame_fixture_body
    .seh_proc unwind64_frame_fixture
unwind64_frame_fixture:
    pushq %rbp
    .seh_pushreg %rbp
    subq $0x40, %rsp
    .seh_stackalloc 0x40
    leaq 0x20(%rsp), %rbp
    .seh_setframe %rbp, 0x20
    .seh_endprologue
unwind64_frame_fixture_body:
    xorl %eax, %eax
    leaq -0x20(%rbp), %rsp
    addq $0x40, %rsp
    popq %rbp
    retq
    .seh_endproc

    /* Deliberately has no .seh_proc entry; the C test registers it as JIT
       code through RtlAddFunctionTable. */
    .globl unwind64_dynamic_probe
unwind64_dynamic_probe:
    retq

    .globl unwind64_return_target
unwind64_return_target:
    retq

    .globl unwind64_dispatch_probe
    .globl unwind64_dispatch_fault
    .globl unwind64_dispatch_resume
    .seh_proc unwind64_dispatch_probe
unwind64_dispatch_probe:
    subq $0x28, %rsp
    .seh_stackalloc 0x28
    .seh_handler unwind64_test_personality, @except
    .seh_endprologue
unwind64_dispatch_fault:
    ud2
unwind64_dispatch_resume:
    movl $0x64, %eax
    addq $0x28, %rsp
    retq
    .seh_endproc

    .globl unwind64_debug_break_probe
    .globl unwind64_debug_break_resume
    .seh_proc unwind64_debug_break_probe
unwind64_debug_break_probe:
    subq $0x28, %rsp
    .seh_stackalloc 0x28
    .seh_endprologue
    callq *__imp_DebugBreak(%rip)
unwind64_debug_break_resume:
    movl $0xDB, %eax
    addq $0x28, %rsp
    retq
    .seh_endproc

    .globl unwind64_longjump_probe
    .globl unwind64_longjump_resume
    .seh_proc unwind64_longjump_probe
unwind64_longjump_probe:
    pushq %rbx
    .seh_pushreg %rbx
    pushq %rbp
    .seh_pushreg %rbp
    pushq %rsi
    .seh_pushreg %rsi
    pushq %rdi
    .seh_pushreg %rdi
    pushq %r12
    .seh_pushreg %r12
    pushq %r13
    .seh_pushreg %r13
    pushq %r14
    .seh_pushreg %r14
    pushq %r15
    .seh_pushreg %r15
    subq $0xF8, %rsp
    .seh_stackalloc 0xF8
    movdqu %xmm6, 0x20(%rsp)
    .seh_savexmm %xmm6, 0x20
    movdqu %xmm7, 0x30(%rsp)
    .seh_savexmm %xmm7, 0x30
    movdqu %xmm8, 0x40(%rsp)
    .seh_savexmm %xmm8, 0x40
    movdqu %xmm9, 0x50(%rsp)
    .seh_savexmm %xmm9, 0x50
    movdqu %xmm10, 0x60(%rsp)
    .seh_savexmm %xmm10, 0x60
    movdqu %xmm11, 0x70(%rsp)
    .seh_savexmm %xmm11, 0x70
    movdqu %xmm12, 0x80(%rsp)
    .seh_savexmm %xmm12, 0x80
    movdqu %xmm13, 0x90(%rsp)
    .seh_savexmm %xmm13, 0x90
    movdqu %xmm14, 0xA0(%rsp)
    .seh_savexmm %xmm14, 0xA0
    movdqu %xmm15, 0xB0(%rsp)
    .seh_savexmm %xmm15, 0xB0
    .seh_endprologue
    movq %rcx, 0xC0(%rsp)
    movq %rdx, 0xC8(%rsp)
    movq %r8,  0xD0(%rsp)
    movq %r9,  0xD8(%rsp)
    stmxcsr 0xE0(%rsp)
    fnstcw 0xE4(%rsp)

    callq *__imp_RtlCaptureContext(%rip)
    movq 0xD0(%rsp), %r8
    movq %rsp, 0x10(%r8)
    leaq unwind64_longjump_resume(%rip), %rax
    movq %rax, 0x50(%r8)
    movq 0xC0(%rsp), %rcx
    movq 0xC8(%rsp), %rdx
    callq *__imp_RtlRestoreContext(%rip)
    movl $1, %eax
    jmp .Llongjump_cleanup

unwind64_longjump_resume:
    movq 0xD8(%rsp), %r10
    movq %rbx, 0x00(%r10)
    movq %rbp, 0x08(%r10)
    movq %rsi, 0x10(%r10)
    movq %rdi, 0x18(%r10)
    movq %r12, 0x20(%r10)
    movq %r13, 0x28(%r10)
    movq %r14, 0x30(%r10)
    movq %r15, 0x38(%r10)
    stmxcsr 0x40(%r10)
    fnstcw 0x44(%r10)
    movdqu %xmm6, 0x50(%r10)
    movdqu %xmm7, 0x60(%r10)
    movdqu %xmm8, 0x70(%r10)
    movdqu %xmm9, 0x80(%r10)
    movdqu %xmm10, 0x90(%r10)
    movdqu %xmm11, 0xA0(%r10)
    movdqu %xmm12, 0xB0(%r10)
    movdqu %xmm13, 0xC0(%r10)
    movdqu %xmm14, 0xD0(%r10)
    movdqu %xmm15, 0xE0(%r10)
    xorl %eax, %eax

.Llongjump_cleanup:
    ldmxcsr 0xE0(%rsp)
    fldcw 0xE4(%rsp)
    movdqu 0x20(%rsp), %xmm6
    movdqu 0x30(%rsp), %xmm7
    movdqu 0x40(%rsp), %xmm8
    movdqu 0x50(%rsp), %xmm9
    movdqu 0x60(%rsp), %xmm10
    movdqu 0x70(%rsp), %xmm11
    movdqu 0x80(%rsp), %xmm12
    movdqu 0x90(%rsp), %xmm13
    movdqu 0xA0(%rsp), %xmm14
    movdqu 0xB0(%rsp), %xmm15
    addq $0xF8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    retq
    .seh_endproc

    .globl unwind64_capture_state_probe
    .seh_proc unwind64_capture_state_probe
unwind64_capture_state_probe:
    pushq %rbx
    .seh_pushreg %rbx
    pushq %rbp
    .seh_pushreg %rbp
    pushq %rsi
    .seh_pushreg %rsi
    pushq %rdi
    .seh_pushreg %rdi
    pushq %r12
    .seh_pushreg %r12
    pushq %r13
    .seh_pushreg %r13
    pushq %r14
    .seh_pushreg %r14
    pushq %r15
    .seh_pushreg %r15
    subq $0xF8, %rsp
    .seh_stackalloc 0xF8
    movdqu %xmm6, 0x20(%rsp)
    .seh_savexmm %xmm6, 0x20
    movdqu %xmm7, 0x30(%rsp)
    .seh_savexmm %xmm7, 0x30
    movdqu %xmm8, 0x40(%rsp)
    .seh_savexmm %xmm8, 0x40
    movdqu %xmm9, 0x50(%rsp)
    .seh_savexmm %xmm9, 0x50
    movdqu %xmm10, 0x60(%rsp)
    .seh_savexmm %xmm10, 0x60
    movdqu %xmm11, 0x70(%rsp)
    .seh_savexmm %xmm11, 0x70
    movdqu %xmm12, 0x80(%rsp)
    .seh_savexmm %xmm12, 0x80
    movdqu %xmm13, 0x90(%rsp)
    .seh_savexmm %xmm13, 0x90
    movdqu %xmm14, 0xA0(%rsp)
    .seh_savexmm %xmm14, 0xA0
    movdqu %xmm15, 0xB0(%rsp)
    .seh_savexmm %xmm15, 0xB0
    .seh_endprologue
    movq %rcx, 0xC0(%rsp)
    movq %rdx, 0xC8(%rsp)
    stmxcsr 0xE0(%rsp)
    fnstcw 0xE4(%rsp)

    movq %rdx, %r10
    movq 0x00(%r10), %rbx
    movq 0x08(%r10), %rbp
    movq 0x10(%r10), %rsi
    movq 0x18(%r10), %rdi
    movq 0x20(%r10), %r12
    movq 0x28(%r10), %r13
    movq 0x30(%r10), %r14
    movq 0x38(%r10), %r15
    ldmxcsr 0x40(%r10)
    fldcw 0x44(%r10)
    movdqu 0x50(%r10), %xmm6
    movdqu 0x60(%r10), %xmm7
    movdqu 0x70(%r10), %xmm8
    movdqu 0x80(%r10), %xmm9
    movdqu 0x90(%r10), %xmm10
    movdqu 0xA0(%r10), %xmm11
    movdqu 0xB0(%r10), %xmm12
    movdqu 0xC0(%r10), %xmm13
    movdqu 0xD0(%r10), %xmm14
    movdqu 0xE0(%r10), %xmm15
    movq 0xC0(%rsp), %rcx
    callq *__imp_RtlCaptureContext(%rip)
    xorl %eax, %eax

    ldmxcsr 0xE0(%rsp)
    fldcw 0xE4(%rsp)
    movdqu 0x20(%rsp), %xmm6
    movdqu 0x30(%rsp), %xmm7
    movdqu 0x40(%rsp), %xmm8
    movdqu 0x50(%rsp), %xmm9
    movdqu 0x60(%rsp), %xmm10
    movdqu 0x70(%rsp), %xmm11
    movdqu 0x80(%rsp), %xmm12
    movdqu 0x90(%rsp), %xmm13
    movdqu 0xA0(%rsp), %xmm14
    movdqu 0xB0(%rsp), %xmm15
    addq $0xF8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    retq
    .seh_endproc

    .globl unwind64_consolidate_probe
    .globl unwind64_consolidate_resume
    .globl unwind64_consolidate_wrong_target
    .seh_proc unwind64_consolidate_probe
unwind64_consolidate_probe:
    subq $0x38, %rsp
    .seh_stackalloc 0x38
    .seh_endprologue
    movq %rcx, 0x20(%rsp)
    movq %rdx, 0x28(%rsp)
    callq *__imp_RtlCaptureContext(%rip)
    movq 0x20(%rsp), %rax
    movq %rsp, 0x98(%rax)
    leaq unwind64_consolidate_wrong_target(%rip), %r8
    movq %r8, 0xF8(%rax)
    movq %rax, %rcx
    movq 0x28(%rsp), %rdx
    callq *__imp_RtlRestoreContext(%rip)
    movl $2, %eax
    jmp .Lconsolidate_probe_return

unwind64_consolidate_wrong_target:
    movl $3, %eax
    jmp .Lconsolidate_probe_return

unwind64_consolidate_resume:
    xorl %eax, %eax

.Lconsolidate_probe_return:
    addq $0x38, %rsp
    retq
    .seh_endproc
