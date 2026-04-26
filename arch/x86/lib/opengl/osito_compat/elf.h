/* W4.8 — minimal elf.h shim for u_cpu_detect (only AT_HWCAP/AT_HWCAP2). */
#ifndef OSITO_ELF_H
#define OSITO_ELF_H 1

#define AT_HWCAP    16
#define AT_HWCAP2   26
#define AT_NULL     0

typedef unsigned long Elf64_auxv_t;

#endif
