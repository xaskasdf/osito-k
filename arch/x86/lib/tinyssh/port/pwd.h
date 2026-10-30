#ifndef OSITO_TSSH_PWD_H
#define OSITO_TSSH_PWD_H
#include <unistd.h>
struct passwd { uid_t pw_uid; };
struct passwd *tssh_getpwnam(const char *name);
#define getpwnam tssh_getpwnam
#endif
