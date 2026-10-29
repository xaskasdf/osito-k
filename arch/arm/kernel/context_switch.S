/*
 * context_switch.S -- Full context save/restore for preemptive scheduling
 *
 * Context frame on stack (272 bytes = 34 x 8):
 *   [sp + 0]   x0
 *   [sp + 8]   x1
 *   ...
 *   [sp + 240] x30
 *   [sp + 248] saved_sp (SP before exception)
 *   [sp + 256] ELR_EL1
 *   [sp + 264] SPSR_EL1
 */

.section .text

.globl irq_vector_entry
irq_vector_entry:
    /* Allocate context frame (272 bytes) */
    sub     sp, sp, #272

    /* Save x0-x30 */
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]

    /* Save pre-exception SP */
    add     x0, sp, #272
    str     x0, [sp, #248]

    /* Save ELR_EL1 and SPSR_EL1 */
    mrs     x0, ELR_EL1
    mrs     x1, SPSR_EL1
    stp     x0, x1, [sp, #256]

    /* TPIDR_EL1 = this CPU's struct cpu_sched (task.h): current task at
     * offset 0, IRQ stack top at 8. x19/x20 survive the C call; the
     * interrupted values are already in the frame. */
    mrs     x20, TPIDR_EL1
    mov     x19, sp                 /* x19 = the interrupted context's frame */
    ldr     x1, [x20]               /* x1 = current task */
    cbz     x1, 1f                  /* Skip if no current task yet */
    str     x19, [x1]               /* current->sp = frame */
1:
    /* Run the handler on this CPU's IRQ stack: once schedule() makes the
     * interrupted task READY, another CPU may resume it on its own stack */
    ldr     x2, [x20, #8]
    mov     sp, x2
    bl      irq_handler

    /* Resume the (possibly different) current task */
    ldr     x1, [x20]
    mov     sp, x19                 /* No task yet: back to the same frame */
    cbz     x1, 2f
    ldr     x2, [x1]               /* x2 = new task's sp */
    mov     sp, x2
2:

    /* Restore ELR_EL1 and SPSR_EL1 */
    ldp     x0, x1, [sp, #256]
    msr     ELR_EL1, x0
    msr     SPSR_EL1, x1

    /* Restore x0-x30 */
    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]

    /* Deallocate frame */
    add     sp, sp, #272
    eret

/*
 * sched_start_asm -- Bootstrap first task without eret.
 * x0 = task SP (context frame).
 *
 * QEMU doesn't support eret outside exception context (EC=0 fault).
 * Workaround: switch stack, load callee-saved regs, enable IRQs,
 * then branch to trampoline directly.
 */
.globl sched_start_asm
sched_start_asm:
    mov     sp, x0

    /* Load x19 (func) and x20 (arg) from frame */
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]

    /* Pop the context frame */
    add     sp, sp, #272

    /* Enable IRQs (equivalent of SPSR.I=0 after eret) */
    msr     DAIFClr, #0x2

    /* Fall through to task_entry_trampoline */

/* Task entry trampoline — new tasks start here via eret */
/* x19 = func pointer, x20 = arg (callee-saved, survive eret) */
.globl task_entry_trampoline
task_entry_trampoline:
    mov     x0, x20             /* arg */
    blr     x19                 /* call func(arg) */
    bl      task_exit_handler   /* if func returns */
1:  wfe
    b       1b

/*
 * proc_exit_trampoline -- eret target after sys_exit.
 * Returns to C caller (proc_run's entry() call returns).
 */
.globl proc_exit_trampoline
proc_exit_trampoline:
    ret
