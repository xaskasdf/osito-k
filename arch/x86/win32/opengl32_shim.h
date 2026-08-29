/* OsitoK Windows compatibility layer - opengl32.dll query shim. */
#ifndef OPENGL32_SHIM_H
#define OPENGL32_SHIM_H

#include "nttypes.h"

void opengl32_shim_init(void);
PVOID opengl32_resolve(const char *func_name, USHORT ordinal,
                       BOOL by_ordinal);

#endif /* OPENGL32_SHIM_H */
