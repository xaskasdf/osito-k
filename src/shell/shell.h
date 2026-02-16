/*
 * OsitoK - Minimal shell header
 */
#ifndef OSITO_SHELL_H
#define OSITO_SHELL_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shell task function (to be used with task_create) */
void shell_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_SHELL_H */
