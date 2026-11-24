/* Freestanding PE32 regression for the MSVC i386 EH4 registration ABI. */

    .text
    .globl _mainCRTStartup

_mainCRTStartup:
    pushl %ebp
    movl %esp, %ebp
    subl $8, %esp

    pushl $4
    pushl $0x3000
    pushl $4096
    pushl $0
    calll *__imp__VirtualAlloc@16
    movl %eax, _fault_page
    testl %eax, %eax
    jnz 1f
    pushl $20
    calll *__imp__ExitProcess@4

1:
    leal -4(%ebp), %ecx
    pushl %ecx
    pushl $1
    pushl $4096
    pushl %eax
    calll *__imp__VirtualProtect@16
    testl %eax, %eax
    jnz 2f
    pushl $21
    calll *__imp__ExitProcess@4

2:
    calll _exercise_eh4
    pushl %eax
    calll *__imp__ExitProcess@4
    int3

_exercise_eh4:
    pushl %ebp
    movl %esp, %ebp
    subl $0x30, %esp

    movl %fs:0, %eax
    movl %eax, -0x10(%ebp)
    movl $_eh4_registration_handler, -0x0c(%ebp)
    movl $_eh4_scope_table, %eax
    xorl _eh4_cookie, %eax
    movl %eax, -0x08(%ebp)
    movl $-2, -0x04(%ebp)
    leal -0x20(%ebp), %eax
    movl %eax, -0x18(%ebp)
    movl $0, -0x14(%ebp)
    movl _eh4_cookie, %eax
    xorl %ebp, %eax
    movl %eax, -0x1c(%ebp)

    movl $0, _filter_marker
    movl $0, _finally_marker
    movl $0, _handler_marker
    movl $0, _continuation_marker

    leal -0x10(%ebp), %eax
    movl %eax, %fs:0
    movl $1, -0x04(%ebp)
    movl _fault_page, %eax
    movl $0xbad0bad0, (%eax)

    movl $30, %eax
    jmp _eh4_return

_eh4_handler:
    movl $0x48414e44, _handler_marker

_eh4_continue:
    movl $0x434f4e54, _continuation_marker
    xorl %eax, %eax
    cmpl $0x53454834, _filter_marker
    je 3f
    movl $10, %eax
    jmp _eh4_return
3:
    cmpl $0x46494e41, _finally_marker
    je 4f
    movl $11, %eax
    jmp _eh4_return
4:
    cmpl $0x48414e44, _handler_marker
    je 5f
    movl $12, %eax
    jmp _eh4_return
5:
    cmpl $0x434f4e54, _continuation_marker
    je _eh4_return
    movl $13, %eax

_eh4_return:
    movl -0x10(%ebp), %edx
    movl %edx, %fs:0
    movl %ebp, %esp
    popl %ebp
    retl

_eh4_filter:
    movl -0x1c(%ebp), %eax
    xorl %ebp, %eax
    cmpl _eh4_cookie, %eax
    jne 6f
    movl $0x53454834, _filter_marker
    movl $1, %eax
    retl
6:
    movl $0, %eax
    retl

_eh4_finally:
    movl $0x46494e41, _finally_marker
    retl

_eh4_check_cookie:
    retl

_eh4_registration_handler:
    pushl %ebp
    movl %esp, %ebp
    pushl 0x14(%ebp)
    pushl 0x10(%ebp)
    pushl 0x0c(%ebp)
    pushl 0x08(%ebp)
    pushl $_eh4_check_cookie
    pushl $_eh4_cookie
    calll __except_handler4_common
    addl $24, %esp
    popl %ebp
    retl

    .data
    .align 4
_eh4_cookie:
    .long 0xbb40e64e

_eh4_scope_table:
    .long -2, 0, -0x1c, 0
    .long -2, _eh4_filter, _eh4_handler
    .long 0, 0, _eh4_finally

_fault_page:
    .long 0
_filter_marker:
    .long 0
_finally_marker:
    .long 0
_handler_marker:
    .long 0
_continuation_marker:
    .long 0
