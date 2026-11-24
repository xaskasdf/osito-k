/* VC6-style registration and metadata, also executed by native Windows. */
    .text
    .globl _exercise_catch
_exercise_catch:
    pushl %ebp
    movl %esp, %ebp
    pushl $-1
    pushl $_catch_frame_handler
    pushl %fs:0
    movl %esp, %fs:0
    subl $48, %esp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %esp, -16(%ebp)
    movl $0x6ac03519, -44(%ebp)
    movl $0xc10357ab, -24(%ebp)
    movl $0xcccccccc, -40(%ebp)
    movl $0xcccccccc, -36(%ebp)
    movl $0xcccccccc, -32(%ebp)
    movl $0xcccccccc, -28(%ebp)
    movl $0, -4(%ebp)
    calll _raise_from_test
    int3
_catch_continue:
    movl -12(%ebp), %ecx
    movl %ecx, %fs:0
    movl -16(%ebp), %esp
    popl %edi
    popl %esi
    popl %ebx
    movl %ebp, %esp
    popl %ebp
    retl
_catch_funclet:
    pushl -24(%ebp)
    pushl -44(%ebp)
    leal -40(%ebp), %eax
    pushl %eax
    calll _verify_catch
    addl $12, %esp
    movl $_catch_continue, %eax
    retl
_catch_frame_handler:
    movl $_func_info, %eax
    jmp ___CxxFrameHandler

    .globl _throw_payload
_throw_payload:
    pushl $_throw_info
    pushl $_catch_payload
    calll *__imp___CxxThrowException@8
    int3

    .data
    .p2align 2
_func_info:
    .long 0x19930520, 2, _unwind_map, 1, _try_map, 0, 0, 0, 0
_unwind_map:
    .long -1, 0, -1, 0
_try_map:
    .long 0, 0, 1, 1, _handler_type
_handler_type:
    .globl _catch_adjectives
_catch_adjectives:
    .long 0
    .long _type_info, -40, _catch_funclet
_type_info:
    .long 0, 0
    .globl _catch_type_name
_catch_type_name:
    .space 64
    .p2align 2
_throw_info:
    .long 0, 0, 0, _catchable_array
_catchable_array:
    .long 1, _catchable_type
_catchable_type:
    .globl _catch_properties
_catch_properties:
    .long 1
    .long _type_info, 0, -1, 0
    .globl _catch_size
_catch_size:
    .long 4, 0
