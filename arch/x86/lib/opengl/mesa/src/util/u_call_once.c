/*
 * Copyright 2022 Yonggang Luo
 * SPDX-License-Identifier: MIT
 */

#include "u_call_once.h"

struct util_call_once_context_t
{
   const void *data;
   util_call_once_data_func func;
};

/* OsitoK W4.7-fix: ELF loader doesn't init %fs for these test apps;
 * thread_local accesses fault on %fs:-8. Single-threaded so plain
 * static works. */
#ifdef __OSITO_K__
static struct util_call_once_context_t call_once_context;
#else
static thread_local struct util_call_once_context_t call_once_context;
#endif

static void
util_call_once_data_slow_once(void)
{
   struct util_call_once_context_t *once_context = &call_once_context;
   once_context->func(once_context->data);
}

void
util_call_once_data_slow(once_flag *once, util_call_once_data_func func, const void *data)
{
   struct util_call_once_context_t *once_context = &call_once_context;
   once_context->data = data;
   once_context->func = func;
   call_once(once, util_call_once_data_slow_once);
}
