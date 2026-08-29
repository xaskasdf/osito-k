#ifndef OSITOFS3_H
#define OSITOFS3_H

#include "../include/types.h"

typedef struct {
    uint32_t directory_inode;
    uint32_t next_inode;
    uint32_t generation;
} osfs3_dir_cursor_t;

int      osfs3_mount(uint64_t part_offset);
bool     osfs3_is_mounted(void);

uint32_t osfs3_resolve_path(const char *path);
uint32_t osfs3_resolve_path_ci(const char *path);
bool     osfs3_directory_exists_ci(const char *path);
bool     osfs3_directory_has_children_ci(const char *path);
bool     osfs3_is_dir(uint32_t ino);
void     osfs3_list_dir(uint32_t dir_ino);
int      osfs3_dir_cursor_open_ci(const char *path,
                                  osfs3_dir_cursor_t *cursor);
int      osfs3_dir_cursor_next(osfs3_dir_cursor_t *cursor,
                               char *name, uint32_t name_capacity,
                               bool *is_directory, uint64_t *size,
                               uint32_t *inode_index);

void    *osfs3_find(const char *path);
void    *osfs3_find_ci(const char *path);
void    *osfs3_get_file(int inode_index);
void    *osfs3_file_at(uint32_t ordinal);
int      osfs3_find_first(const char *pattern, int start_inode);

int      osfs3_file_retain(void *file);
void     osfs3_file_release(void *file);
uint64_t osfs3_file_size(const void *file);
const char *osfs3_file_name(const void *file);
uint32_t osfs3_file_ctime(const void *file);
uint32_t osfs3_file_mtime(const void *file);
uint64_t osfs3_file_revision(const void *file);
uint64_t osfs3_file_byte_offset(const void *file);

int      osfs3_read(uint32_t ino, uint64_t offset, void *buf, uint64_t len);
uint64_t osfs3_get_size(uint32_t ino);
int      osfs3_read_file(const void *file, uint64_t offset, void *buf,
                         uint64_t len);
int      osfs3_read_file_block(const void *file, uint32_t block_index,
                               void *buf);
void    *osfs3_create(const char *path, uint64_t size);
int      osfs3_write(void *file, uint64_t offset, const void *buf,
                     uint64_t len);
int      osfs3_truncate(void *file, uint64_t size);
int      osfs3_rename(const char *from, const char *to, bool replace);
int      osfs3_delete(const char *path);
int      osfs3_mkdir(const char *path);
int      osfs3_rmdir(const char *path);

uint32_t osfs3_file_count(void);
uint32_t osfs3_free_blocks(void);
uint32_t osfs3_total_blocks(void);
uint32_t osfs3_max_files(void);
uint32_t osfs3_block_size(void);
const char *osfs3_label(void);

#endif /* OSITOFS3_H */
