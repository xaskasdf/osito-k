#ifndef OSITOK_X86_INTERRUPT_H
#define OSITOK_X86_INTERRUPT_H

#include "types.h"

enum x86_ist_index {
    X86_IST_NONE   = 0,
    X86_IST_COMPAT = 1,
    X86_IST_DEBUG  = 2,
    X86_IST_FAULT  = 3,
    X86_IST_IRQ    = 4,
    X86_IST_NMI    = 5,
    X86_IST_DF     = 6,
    X86_IST_MC     = 7,
};

/* Exact stack image consumed by isr_common's pop sequence and IRETQ. */
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} x86_interrupt_frame_t;

_Static_assert(sizeof(x86_interrupt_frame_t) == 176,
               "x86 interrupt frame size changed");
_Static_assert(__builtin_offsetof(x86_interrupt_frame_t, vector) == 120,
               "x86 interrupt vector offset changed");
_Static_assert(__builtin_offsetof(x86_interrupt_frame_t, rip) == 136,
               "x86 interrupt RIP offset changed");
_Static_assert(__builtin_offsetof(x86_interrupt_frame_t, rsp) == 160,
               "x86 interrupt RSP offset changed");

#endif
