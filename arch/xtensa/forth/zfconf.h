/*
 * zForth configuration for OsitoK
 *
 * 32-bit integer cells, 2KB dictionary, 16-deep stacks.
 * No trace, no typed memory access (save code size).
 */
#ifndef ZFCONF_H
#define ZFCONF_H

#include <stdint.h>

typedef int32_t  zf_cell;
typedef int32_t  zf_int;
typedef uint32_t zf_addr;

#define ZF_CELL_FMT     "%d"
#define ZF_ADDR_FMT     "%u"

#define ZF_DICT_SIZE              2048
#define ZF_DSTACK_SIZE            16
#define ZF_RSTACK_SIZE            16

#define ZF_ENABLE_TRACE           0
#define ZF_ENABLE_BOUNDARY_CHECKS 1
#define ZF_ENABLE_BOOTSTRAP       1
#define ZF_ENABLE_TYPED_MEM_ACCESS 0

#endif /* ZFCONF_H */
