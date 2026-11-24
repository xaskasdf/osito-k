#ifndef NT64_SERVICE_H
#define NT64_SERVICE_H

/* OsitoK service IDs are private to its NTDLL build, not Windows build IDs.
 * The namespace is separate from the native ELF/Linux syscall numbers. */
#define NT64_SERVICE_BASE  0x4E540000
#define NT64_SERVICE_LIMIT 0x4E550000

#ifndef __ASSEMBLER__
#include "nttypes.h"

typedef struct {
    ULONGLONG r9, r8, rdx, r10;
    ULONGLONG r15, r14, r13, r12, rdi, rsi, rbp, rbx;
    ULONGLONG rip, rflags;
} NT64_SERVICE_FRAME;

ULONG nt64_service_number(PVOID target);
NTSTATUS nt64_service_dispatch(ULONG number, const NT64_SERVICE_FRAME *frame);
NTSTATUS nt64_service_invoke(PVOID target, const ULONG_PTR *args);
#endif
#endif
