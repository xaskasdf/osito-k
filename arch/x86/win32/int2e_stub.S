/*
 * OsitoK — INT 0x2E Handler for Win32 Compat32 Dispatch
 *
 * When PE32 (i386) code runs in compatibility mode, IAT thunks use
 * INT 0x2E to transition from 32-bit to 64-bit kernel code:
 *
 *   MOV EAX, <thunk_index>    ; which Win32 function to call
 *   MOV ECX, <nargs>          ; argument count (for debug)
 *   INT 0x2E                  ; trap to 64-bit kernel
 *   RET <N*4>                 ; stdcall cleanup on return
 *
 * The 64-bit interrupt gate in the IDT switches the CPU to long mode.
 * We save registers, read the thunk index from EAX, compute a pointer
 * to the 32-bit caller's stack arguments, call compat32_dispatch(),
 * place the result in RAX, and IRETQ back to compat mode.
 *
 * Stack frame from compat mode through a 64-bit interrupt gate
 * (same privilege, no IST — in 64-bit mode the CPU always pushes SS:RSP):
 *
 *   [RSP + 32]: SS_old      (8 bytes, zero-extended from 32-bit)
 *   [RSP + 24]: RSP_old     (8 bytes, zero-extended from ESP)
 *   [RSP + 16]: RFLAGS_old  (8 bytes)
 *   [RSP +  8]: CS_old      (8 bytes, the 32-bit code selector)
 *   [RSP +  0]: RIP_old     (8 bytes, zero-extended from EIP)
 */

.code64
.text

.globl int2e_stub
int2e_stub:
    /*
     * CPU has pushed the interrupt frame (40 bytes).
     * RAX = thunk index, RCX = nargs (both set by the 32-bit thunk).
     *
     * Save callee-clobbered registers. We must preserve everything
     * except RAX (which carries the return value to 32-bit code).
     */
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

    /*
     * Stack layout at this point (10 saved regs × 8 = 80 bytes):
     *   [RSP +  0]: saved R11
     *   [RSP +  8]: saved R10
     *   [RSP + 16]: saved R9
     *   [RSP + 24]: saved R8
     *   [RSP + 32]: saved RBP
     *   [RSP + 40]: saved RDI
     *   [RSP + 48]: saved RSI
     *   [RSP + 56]: saved RDX
     *   [RSP + 64]: saved RCX
     *   [RSP + 72]: saved RBX
     * --- interrupt frame (pushed by CPU) ---
     *   [RSP + 80]: RIP_old
     *   [RSP + 88]: CS_old
     *   [RSP + 96]: RFLAGS_old
     *   [RSP + 104]: RSP_old  (the 32-bit ESP before INT)
     *   [RSP + 112]: SS_old
     */

    /* First SysV arg: RDI = thunk_idx (from EAX) */
    mov %eax, %edi

    /*
     * Second SysV arg: RSI = pointer to 32-bit stack arguments.
     *
     * At the moment of INT 0x2E, the 32-bit stack had:
     *   [ESP + 0]: return address (4 bytes, from CALL [IAT_entry])
     *   [ESP + 4]: arg1 (4 bytes)
     *   [ESP + 8]: arg2 ...
     *
     * RSP_old is in the interrupt frame at offset 104 from current RSP.
     * We want &args[0] = RSP_old + 4 (skip the 4-byte return address).
     */
    mov 104(%rsp), %rsi
    add $4, %rsi

    /* Align stack to 16 bytes (SysV ABI requirement for CALL) */
    mov %rsp, %rbp
    and $~0xF, %rsp

    /* Call compat32_dispatch(thunk_idx, stack_args) */
    call compat32_dispatch

    /* Restore stack (undo alignment) */
    mov %rbp, %rsp

    /*
     * RAX now holds the 64-bit return value from the shim.
     * After IRETQ, 32-bit code sees it as EAX (low 32 bits).
     */

    /* Restore saved registers (except RAX — it has the result) */
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

    /* Return to 32-bit compat mode */
    iretq
