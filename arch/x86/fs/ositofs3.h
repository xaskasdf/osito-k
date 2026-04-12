#ifndef OSITOFS3_H
#define OSITOFS3_H

#include "../include/types.h"

int      osfs3_mount(uint64_t part_offset);
uint32_t osfs3_resolve_path(const char *path);
int      osfs3_read(uint32_t ino, uint64_t offset, void *buf, uint64_t len);
uint64_t osfs3_get_size(uint32_t ino);
bool     osfs3_is_dir(uint32_t ino);
void     osfs3_list_dir(uint32_t dir_ino);
bool     osfs3_is_mounted(void);

#endif
