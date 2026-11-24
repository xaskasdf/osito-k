/*
 * elf.h -- loading static AArch64 programs (elf.c), run by process.c
 */

#ifndef OSITO_ARM_ELF_H
#define OSITO_ARM_ELF_H

#include "../include/types.h"

typedef struct {
    uint64_t entry;
    uint64_t image, image_pages;    /* every PT_LOAD, contiguous */
    uint64_t stack, stack_pages;
    uint64_t stack_top;             /* 16-byte aligned */
} elf_info_t;

/* Loads ET_EXEC (position-independent code only) or a static PIE (ET_DYN
 * with R_AARCH64_RELATIVE relocations) into fresh pages. 0 or -1. */
int  elf_load(const void *data, uint64_t size, elf_info_t *info);
void elf_unload(elf_info_t *info);

#endif
