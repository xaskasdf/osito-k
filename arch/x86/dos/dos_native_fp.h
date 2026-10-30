#ifndef OSITOK_DOS_NATIVE_FP_H
#define OSITOK_DOS_NATIVE_FP_H

/* Matches the kernel's XCR0=7 policy, with a FXSAVE fallback. */
#define DOS_NATIVE_FP_BYTES 1024
#define DOS_NATIVE_FP_CR0_OFFSET 1056
#define DOS_NATIVE_FP_COMPAT_OFFSET 1064
#define DOS_NATIVE_FP_FRAME_BYTES 1088

#ifndef __ASSEMBLER__
#include "../include/types.h"

typedef struct dos_native_fp_state {
    uint8_t image[DOS_NATIVE_FP_BYTES] __attribute__((aligned(64)));
    uint8_t legacy_env[32];
    uint64_t return_cr0;
    uint32_t return_compat;
    uint64_t host_cr0, host_efer, host_sysenter_cs;
    uint32_t host_mxcsr;
    uint16_t host_control;
    bool host_saved, host_sep;
    uint16_t fcs, fds;
    bool ferr, irq_pending, ignne;
} dos_native_fp_state_t;

_Static_assert(sizeof(dos_native_fp_state_t) <= 4096, "DOS FP backing page");
_Static_assert(__builtin_offsetof(dos_native_fp_state_t, image) == 0,
               "DOS assembly FP image offset");
_Static_assert(__builtin_offsetof(dos_native_fp_state_t, return_cr0) ==
               DOS_NATIVE_FP_CR0_OFFSET, "DOS assembly CR0 offset");
_Static_assert(__builtin_offsetof(dos_native_fp_state_t, return_compat) ==
               DOS_NATIVE_FP_COMPAT_OFFSET, "DOS assembly FP format offset");
struct dos_vm;
void dos_native_fp_return(struct dos_vm *vm, void *image);
#endif
#endif
