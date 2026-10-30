#ifndef OSITO_DRIVER_STORE_H
#define OSITO_DRIVER_STORE_H
#include "../include/types.h"
#define DRIVER_CONFIG_MAX 4096
/* 0 success, 1 missing, -1 invalid/unavailable. Never replace invalid configs. */
int driver_config_read(const char *id, void *data, unsigned capacity, unsigned *size);
int driver_config_write(const char *id, const void *data, unsigned size);
/* Bounded regular-file reads and atomic staged publication, used for firmware. */
int driver_file_read(const char *, void *, unsigned, unsigned *, bool private_file);
int driver_file_publish(const char *, const void *, unsigned, bool replace);
#endif
