/* OsitoK shim — pthread.h
 * Mesa's src/c11/threads.h takes the HAVE_PTHREAD=1 branch when the macro
 * is set; that branch #include's <pthread.h>.
 *
 * C mode (W4.0/W4.1): empty file — mesa_compat.h already supplies the
 * typedefs/functions Mesa needs.
 *
 * C++ mode (W4.2+): delegate to the hosted gcc:12 glibc <pthread.h>
 * via include_next so PTHREAD_ONCE_INIT etc. are available.
 */
#ifndef OSITO_PTHREAD_H
#define OSITO_PTHREAD_H 1
#ifdef __cplusplus
#  include_next <pthread.h>
#endif
#endif
