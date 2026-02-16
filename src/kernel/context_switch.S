/*
 * OsitoK - Context switch and exception entry/exit
 *
 * Xtensa LX106, CALL0 ABI (no register windows).
 *
 * When a level-1 exception/interrupt fires:
 *   - EPC1 = interrupted PC
 *   - PS.EXCM = 1 (exception mode, interrupts masked)
 *   - CPU jumps to UserExceptionVector (VECBASE+0x50) if PS.UM=1
 *     or KernelExceptionVector (VECBASE+0x30) if PS.UM=0
 *   - The vector `j _xt_user_exc` does NOT clobber any register
 *
 * Context frame (80 bytes, on task stack):
 *   a0-a15 (16 regs * 4 = 64 bytes)
 *   PS, SAR, EPC1, pad (4 * 4 = 16 bytes)
 *   Total = 80 bytes
 *
 * IMPORTANT: This code MUST be in IRAM.
 */

/*
 * Context frame offsets — must match CTX_* defines in task.h
 */
#define CTX_A0    0x00
#define CTX_A1    0x04
#define CTX_A2    0x08
#define CTX_A3    0x0C
#define CTX_A4    0x10
#define CTX_A5    0x14
#define CTX_A6    0x18
#define CTX_A7    0x1C
#define CTX_A8    0x20
#define CTX_A9    0x24
#define CTX_A10   0x28
#define CTX_A11   0x2C
#define CTX_A12   0x30
#define CTX_A13   0x34
#define CTX_A14   0x38
#define CTX_A15   0x3C
#define CTX_PS    0x40
#define CTX_SAR   0x44
#define CTX_EPC1  0x48
#define CTX_SIZE  80

    .section .iram0.text, "ax"

/* ================================================================
 * _xt_user_exc — Level-1 exception/interrupt entry point
 *
 * Jumped to from vector table (VECBASE+0x30 or VECBASE+0x50).
 * The `j` instruction does NOT clobber any register.
 *
 * At entry:
 *   - EPC1 = interrupted PC
 *   - PS.EXCM = 1 (exception mode, interrupts masked)
 *   - All general registers contain interrupted task's values
 *
 * Strategy:
 *   1. Allocate context frame on task stack
 *   2. Save all registers + PS + SAR + EPC1
 *   3. Save SP to current_task->sp
 *   4. Switch to ISR stack
 *   5. Call C handler (os_exception_handler)
 *   6. Load (possibly new) current_task->sp
 *   7. Restore context
 *   8. Return from exception (rfe)
 * ================================================================ */

    .global  _xt_user_exc
    .type    _xt_user_exc, @function
    .literal_position
    .align   4

_xt_user_exc:
    /* Allocate context frame on current task stack */
    addi    a1, a1, -CTX_SIZE

    /* Save all general registers */
    s32i    a0, a1, CTX_A0
    s32i    a2, a1, CTX_A2
    s32i    a3, a1, CTX_A3
    s32i    a4, a1, CTX_A4
    s32i    a5, a1, CTX_A5
    s32i    a6, a1, CTX_A6
    s32i    a7, a1, CTX_A7
    s32i    a8, a1, CTX_A8
    s32i    a9, a1, CTX_A9
    s32i    a10, a1, CTX_A10
    s32i    a11, a1, CTX_A11
    s32i    a12, a1, CTX_A12
    s32i    a13, a1, CTX_A13
    s32i    a14, a1, CTX_A14
    s32i    a15, a1, CTX_A15

    /* Save the stack pointer (a1) — the value BEFORE we subtracted */
    addi    a2, a1, CTX_SIZE
    s32i    a2, a1, CTX_A1

    /* Save special registers */
    rsr     a2, ps
    s32i    a2, a1, CTX_PS

    rsr     a2, sar
    s32i    a2, a1, CTX_SAR

    rsr     a2, epc1
    s32i    a2, a1, CTX_EPC1

    /*
     * Save current SP to current_task->sp (offset 0 in TCB)
     */
    movi    a3, current_task
    l32i    a3, a3, 0           /* a3 = current_task pointer */
    s32i    a1, a3, 0           /* current_task->sp = a1 */

    /*
     * Switch to ISR stack
     */
    movi    a1, _isr_stack_top

    /*
     * Call the C exception dispatcher
     * This checks EXCCAUSE and dispatches to the right handler.
     * If it's a timer tick, it calls schedule().
     */
    call0   os_exception_handler

    /*
     * After os_exception_handler returns, current_task may point to a
     * different task (if schedule() switched). Load its SP.
     */
    movi    a3, current_task
    l32i    a3, a3, 0           /* a3 = (possibly new) current_task */
    l32i    a1, a3, 0           /* a1 = new task's saved SP */

    /* Jump over literal pool to the restore code */
    j       _xt_do_restore

/* ================================================================
 * _xt_restore_context_and_rfe — Restore registers and return
 *
 * Also used as the entry point for sched_start() to load the
 * first task's context.
 *
 * Expects: a1 = pointer to context frame on task stack
 * ================================================================ */

    .global _xt_restore_context_and_rfe
    .type   _xt_restore_context_and_rfe, @function
    .literal_position
    .align  4

_xt_restore_context_and_rfe:
    /*
     * If called from sched_start(), load SP from current_task first
     */
    movi    a3, current_task
    l32i    a3, a3, 0
    l32i    a1, a3, 0           /* a1 = saved SP (context frame) */

_xt_do_restore:
    /* Restore special registers */
    l32i    a2, a1, CTX_EPC1
    wsr     a2, epc1

    l32i    a2, a1, CTX_SAR
    wsr     a2, sar

    l32i    a2, a1, CTX_PS
    wsr     a2, ps
    isync

    /* Restore general registers (a0, a2-a15) */
    l32i    a0, a1, CTX_A0
    /* skip a1 — we restore it last */
    l32i    a2, a1, CTX_A2
    l32i    a3, a1, CTX_A3
    l32i    a4, a1, CTX_A4
    l32i    a5, a1, CTX_A5
    l32i    a6, a1, CTX_A6
    l32i    a7, a1, CTX_A7
    l32i    a8, a1, CTX_A8
    l32i    a9, a1, CTX_A9
    l32i    a10, a1, CTX_A10
    l32i    a11, a1, CTX_A11
    l32i    a12, a1, CTX_A12
    l32i    a13, a1, CTX_A13
    l32i    a14, a1, CTX_A14
    l32i    a15, a1, CTX_A15

    /* Restore a1 (SP): pop the context frame */
    l32i    a1, a1, CTX_A1

    /* Return from exception */
    rfe

    .size _xt_user_exc, . - _xt_user_exc

/* ================================================================
 * _task_entry_trampoline — Assembly trampoline for new tasks
 *
 * When a new task is first scheduled via rfe, registers are loaded
 * from its initial context frame:
 *   a2 = task function pointer
 *   a3 = task argument
 *
 * We call the task function with the argument as its parameter.
 * If it returns, we call _task_exit_handler (C) to mark it dead.
 * ================================================================ */

    .global _task_entry_trampoline
    .type   _task_entry_trampoline, @function
    .literal_position
    .align  4

_task_entry_trampoline:
    /*
     * At this point (after rfe restored our initial context):
     *   a2 = task_func_t pointer (the real task function)
     *   a3 = void *arg
     *
     * CALL0 ABI: first arg in a2, so we need to:
     *   - Save the function pointer
     *   - Put arg into a2
     *   - call0 the function
     */
    mov     a4, a2          /* a4 = func */
    mov     a2, a3          /* a2 = arg (first argument) */
    callx0  a4              /* call func(arg) */

    /* If task function returns, call the C exit handler */
    call0   _task_exit_handler

    /* Should never reach here, but just in case */
.Ldead_loop:
    waiti   0
    j       .Ldead_loop

    .size _task_entry_trampoline, . - _task_entry_trampoline
