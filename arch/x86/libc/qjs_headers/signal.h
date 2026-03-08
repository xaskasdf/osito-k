/* OsitoK shim — signal.h */
#ifndef _SIGNAL_H
#define _SIGNAL_H
typedef void (*sighandler_t)(int);
typedef unsigned long sigset_t;
#define SIGINT  2
#define SIGTERM 15
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
sighandler_t signal(int sig, sighandler_t handler);
#endif
