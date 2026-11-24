/*
 * arch/x86/include/io_predict.h — speculative I/O prefetch
 *
 * Records per-process file-open sequences: "process P opened A, then B".
 * On subsequent opens of A, prefetches B on an AP via smp_submit_ff so
 * when the process actually opens B its metadata + first pages are
 * already warm in kernel memory.
 *
 * Benefit example: TCC compiling foo.c always opens foo.h, stdio.h, ...
 * With the pattern cached, by the time TCC emits `#include <stdio.h>`
 * stdio.h has been prefetched on another core.
 */
#ifndef OSITOK_IO_PREDICT_H
#define OSITOK_IO_PREDICT_H

#include "types.h"
#include "stdint.h"

/* Called on every successful vfs_find() — records transitions and
 * dispatches prefetch for predicted next files. */
void io_predict_observe(const char *opened_path);

/* Reset the pattern table. */
void io_predict_reset(void);

/* Shell: dump top patterns. */
void io_predict_stats(void);

/* Enable/disable (default: enabled). */
extern bool io_predict_enabled;

#endif /* OSITOK_IO_PREDICT_H */
