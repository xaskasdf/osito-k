#ifndef OSITO_SHELL_REDIRECT_H
#define OSITO_SHELL_REDIRECT_H

#include "../fs/ositofs_metadata.h"

extern void *osfs2_find(const char *name);
extern void *osfs2_create(const char *name, uint64_t size);
extern int osfs2_get_mode(const void *file, uint16_t *mode);
extern uint64_t osfs2_file_size(void *file);
extern int disk_flush(void);

typedef struct {
    void *file;
    bool append;
} shell_output_t;

/* Prepare the target before running a command, including commands with no
 * output. Keep file identity; truncation is the filesystem's responsibility. */
static const char *shell_output_open(shell_output_t *out, const char *path,
                                     bool append)
{
    out->file = NULL;
    out->append = append;
    void *file = osfs2_find(path);
    if (!file) file = osfs2_create(path, 0);
    if (!file) return "cannot open/create target";
    uint16_t mode;
    if (osfs2_get_mode(file, &mode) < 0 || (mode & 0170000) != 0100000)
        return "target is not a regular file";
    if (!append && osfs2_truncate_ex(file, 0, 0) < 0)
        return "truncate failed";
    out->file = file;
    return NULL;
}

static const char *shell_output_finish(const shell_output_t *out,
                                       const void *data, uint32_t length)
{
    if (!out->file) return "target is not open";
    uint64_t offset = out->append ? osfs2_file_size(out->file) : 0;
    if (length && (offset > UINT64_MAX - length ||
        osfs2_write_ex(out->file, offset, data, length, 0) < 0))
        return "write failed";
    if (disk_flush() < 0) return "sync failed";
    return NULL;
}

/* Capture remains bounded, but exceeding the bound must not look successful. */
static bool shell_capture_append(char *buffer, uint32_t *used, uint32_t capacity,
                                  const char *data, size_t length)
{
    uint32_t limit = capacity ? capacity - 1 : 0;
    if (!buffer || *used > limit) return length == 0;
    size_t amount = limit - *used;
    if (amount > length) amount = length;
    if (amount) memcpy(buffer + *used, data, amount);
    *used += amount;
    return amount == length;
}

#endif
