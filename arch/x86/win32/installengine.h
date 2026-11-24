#ifndef OSITO_INSTALLENGINE_H
#define OSITO_INSTALLENGINE_H
#include "nttypes.h"
void installengine_init(void);
/* Called after all owner threads have stopped, before guest heap teardown. */
void installengine_release_process(DWORD owner);
#endif
