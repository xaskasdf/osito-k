/*
 * paging.h -- what code ported from x86 expects of its paging header
 *
 * The inference stack (tensor.c, tensor_arena.c, tokenizer_trie.c) comes
 * from arch/x86, where physical memory is reached through a direct map.
 * Here the map is flat (VA == PA), so the conversions are casts.
 */

#ifndef OSITO_ARM_PAGING_H
#define OSITO_ARM_PAGING_H

#include "types.h"

#define PHYS_TO_VIRT(p)     ((void *)(uintptr_t)(p))
#define VIRT_TO_PHYS(v)     ((uint64_t)(uintptr_t)(v))

#endif
