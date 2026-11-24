#ifndef OSITO_SHELL_REMOTE_H
#define OSITO_SHELL_REMOTE_H
#include "../include/types.h"
typedef void (*shell_remote_output_fn)(void *, const char *, unsigned long);
int shell_remote_execute(char *, shell_remote_output_fn, void *);
bool shell_remote_capture(const char *, size_t);
bool shell_remote_active(void);
#endif
