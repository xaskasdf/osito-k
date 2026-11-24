#ifndef OSITO_KEXEC_H
#define OSITO_KEXEC_H
#include "../include/boot_info.h"
void kexec_command(const char *filename);
void kexec_boot_reserve(const boot_info_t *, uint64_t physical_rsp);
void kexec_boot_release(boot_info_t *);
#endif
