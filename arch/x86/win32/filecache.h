#ifndef NT_FILECACHE_H
#define NT_FILECACHE_H

#include "../include/types.h"

struct _FILE_OBJECT;

/* One immutable read window per open, in a bounded shared pool. */
#define NT_FILE_CACHE_BYTES 65536U
#define NT_FILE_CACHE_SLOTS 32U

typedef struct {
    uint64_t cookie;
    uint64_t next_offset;
    uint32_t sequential;
} NT_FILE_READ_STATE;

void nt_file_cache_open(struct _FILE_OBJECT *file);
void nt_file_cache_close(struct _FILE_OBJECT *file);

/* Caller holds the FILE_OBJECT I/O lock and enforces byte-range locks. Only
 * kernel buffers may be supplied. Allocation failure may fall back to length
 * bytes; read-ahead is optional and never changes the requested byte count. */
uint32_t nt_file_read_capacity(struct _FILE_OBJECT *file, uint64_t offset,
                              uint32_t length, uint64_t file_size);
int nt_file_cached_read(struct _FILE_OBJECT *file, uint64_t offset,
                        void *buffer, uint32_t length, uint32_t capacity,
                        uint64_t file_size);

#endif
