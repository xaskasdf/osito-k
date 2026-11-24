/* OsitoK shim — Mesa's u_endian.h hits this branch on __APPLE__ host
 * compiles. We unconditionally set up little-endian for x86-64. */
#ifndef _OSITOK_MACHINE_ENDIAN_H_
#define _OSITOK_MACHINE_ENDIAN_H_

#define __DARWIN_LITTLE_ENDIAN  1234
#define __DARWIN_BIG_ENDIAN     4321
#define __DARWIN_BYTE_ORDER     __DARWIN_LITTLE_ENDIAN

#define LITTLE_ENDIAN           __DARWIN_LITTLE_ENDIAN
#define BIG_ENDIAN              __DARWIN_BIG_ENDIAN
#define BYTE_ORDER              __DARWIN_BYTE_ORDER

#endif
