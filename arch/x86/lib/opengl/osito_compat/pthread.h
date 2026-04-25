/* OsitoK shim — pthread.h
 * Mesa's src/c11/threads.h takes the HAVE_PTHREAD=1 branch when the macro
 * is set; that branch #include's <pthread.h>. We provide this empty file so
 * the include resolves; mesa_compat.h already supplies all the typedefs and
 * inline no-ops for the pthread surface we care about.
 */
#ifndef OSITO_PTHREAD_H
#define OSITO_PTHREAD_H 1
/* All pthread typedefs/functions live in mesa_compat.h (force-included). */
#endif
