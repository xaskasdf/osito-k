/*
 * OsitoK — DOS INT Handler Stubs for Native 32-bit Execution
 *
 * When a DPMI client enters 32-bit protected mode, its DOS, BIOS, DPMI,
 * and mouse interrupts trap here through the IDT. We save the full register
 * state, call the C dispatcher, restore registers, and IRETQ back.
 *
 * Uses IST2 (separate from Win32's IST1) to avoid stack conflicts.
 * Pattern follows int2e_stub.S exactly.
 *
 * DOS INTs pass arguments in registers (AH=function, DS:DX=buffer, etc.)
 * so we pass the entire saved register frame to the C handler.
 */

.code64
.text

/*
 * Common handler body — entered with the interrupt number pushed by the
 * vector stub immediately above the CPU's IRET frame.
 * Saves all GPRs, calls dos_int_native_dispatch(int_num, regs_ptr), restores.
 */
dos_int_common:
    /* CPU loaded RSP from TSS.IST2 and pushed a 40-byte IRET frame. The
     * vector stub then pushed one qword without clobbering a guest GPR. */

    /* Save all registers */
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    /* Save segment registers (as 64-bit pushes, only low 16 bits matter) */
    xor %rax, %rax
    mov %ds, %ax
    push %rax
    mov %es, %ax
    push %rax

    /* Save old IST2 on stack (for reentrancy) */
    movq tss_ist2_ptr(%rip), %r11
    pushq (%r11)

    /* Update IST2 below current RSP for nested INTs */
    movq %rsp, %r10
    subq $16384, %r10
    movq %r10, (%r11)

    /*
     * Stack layout after the common saves:
     *   [RSP +   0]:  saved old IST2
     *   [RSP +   8]:  ES
     *   [RSP +  16]:  DS
     *   [RSP +  24]:  R15
     *   [RSP +  32]:  R14
     *   ...
     *   [RSP + 136]:  RAX
     *   [RSP + 144]:  RBX  (not used, RAX is at 136)
     *   Actually let me recalculate...
     *   [RSP +   0]:  saved IST2
     *   [RSP +   8]:  ES
     *   [RSP +  16]:  DS
     *   [RSP +  24]:  R15
     *   [RSP +  32]:  R14
     *   [RSP +  40]:  R13
     *   [RSP +  48]:  R12
     *   [RSP +  56]:  R11
     *   [RSP +  64]:  R10
     *   [RSP +  72]:  R9
     *   [RSP +  80]:  R8
     *   [RSP +  88]:  RBP
     *   [RSP +  96]:  guest RDI
     *   [RSP + 104]:  RSI
     *   [RSP + 112]:  RDX
     *   [RSP + 120]:  RCX
     *   [RSP + 128]:  RBX
     *   [RSP + 136]:  RAX
     *   [RSP + 144]:  interrupt number
     *   [RSP + 152]:  IRET RIP
     *   [RSP + 160]:  IRET CS
     *   [RSP + 168]:  IRET RFLAGS
     *   [RSP + 176]:  IRET RSP
     *   [RSP + 184]:  IRET SS
     */

    /* Restore 64-bit data segments for kernel C code */
    movw $0x30, %ax         /* GDT_SEL_DATA64 = 0x30 */
    mov %ax, %ds
    mov %ax, %es

    /* Call C dispatcher:
     *   RDI = interrupt number
     *   RSI = pointer to saved register frame on stack
     * The C function reads/writes registers via the frame pointer. */
    movq 144(%rsp), %rdi    /* vector pushed by the entry stub */
    leaq 8(%rsp), %rsi      /* pointer to ES on stack = start of regs frame */

    mov %rsp, %rbp
    and $~0xF, %rsp          /* Align stack to 16 bytes */
    call dos_int_native_dispatch
    mov %rbp, %rsp

    /* Restore IST2 */
    movq tss_ist2_ptr(%rip), %r11
    popq (%r11)

    /* Restore segment registers */
    pop %rax
    mov %ax, %es
    pop %rax
    mov %ax, %ds

    /* Restore GPRs */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    addq $8, %rsp           /* discard interrupt number */
    iretq

/*
 * Individual INT stubs push the vector without changing guest registers.
 */

.globl dos_int08_stub
dos_int08_stub:             /* Timer (IRQ 0) */
    pushq $0x08
    jmp dos_int_common

.globl dos_int10_stub
dos_int10_stub:             /* BIOS Video */
    pushq $0x10
    jmp dos_int_common

.globl dos_int16_stub
dos_int16_stub:             /* BIOS Keyboard */
    pushq $0x16
    jmp dos_int_common

.globl dos_int20_stub
dos_int20_stub:             /* DOS Terminate */
    pushq $0x20
    jmp dos_int_common

.globl dos_int21_stub
dos_int21_stub:             /* DOS API */
    pushq $0x21
    jmp dos_int_common

.globl dos_int2f_stub
dos_int2f_stub:             /* DOS Multiplex */
    pushq $0x2F
    jmp dos_int_common

.globl dos_int31_stub
dos_int31_stub:             /* DPMI */
    pushq $0x31
    jmp dos_int_common

.globl dos_int33_stub
dos_int33_stub:             /* Mouse */
    pushq $0x33
    jmp dos_int_common

.globl dos_int67_stub
dos_int67_stub:             /* EMS / VCPI */
    pushq $0x67
    jmp dos_int_common

.globl dos_intf9_stub
dos_intf9_stub:             /* DPMI default protected-mode reflector */
    pushq $0xF9
    jmp dos_int_common

.globl dos_intfa_stub
dos_intfa_stub:             /* DPMI real-mode callback return */
    pushq $0xFA
    jmp dos_int_common

.globl dos_intfc_stub
dos_intfc_stub:             /* DPMI raw PM-to-RM switch */
    pushq $0xFC
    jmp dos_int_common

.globl dos_intfd_stub
dos_intfd_stub:             /* DPMI processor-exception return */
    pushq $0xFD
    jmp dos_int_common

.globl dos_exc01_stub
dos_exc01_stub:             /* #DB, distinguished from software INT 01h */
    pushq $0x101
    jmp dos_int_common

.section .note.GNU-stack,"",%progbits
