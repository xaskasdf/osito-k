#ifndef OSITO_SSHD_H
#define OSITO_SSHD_H
#include "../include/types.h"
void sshd_command(int argc, char **argv);
const char *sshd_ensure_running(uint16_t port);
#endif
