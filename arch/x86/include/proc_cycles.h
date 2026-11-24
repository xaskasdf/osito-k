#ifndef OSITO_PROC_CYCLES_H
#define OSITO_PROC_CYCLES_H

#include "types.h"

/* BSP task runtime in TSC ticks, including the current unswitched slice. */
bool proc_query_cpu_cycles(uint32_t pid, uint64_t *cycles);

#endif
