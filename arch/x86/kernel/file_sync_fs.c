#include "file_sync.h"
#include "../fs/ositofs3.h"
#include "random_secure.h"

extern int disk_flush(void);

static void *source, *staged, *previous;
static uint64_t source_revision, previous_revision, previous_id;
static char destination[FILE_SYNC_PATH], temporary[FILE_SYNC_PATH];
static uint16_t destination_mode;
static osfs3_dir_cursor_t cursor;

static int sync_stat(const char *path, unsigned *kind, uint64_t *size)
{
    if (!osfs3_is_mounted()) return SYNC_IO;
    uint32_t ino = osfs3_resolve_path_ci(path);
    if (!ino) return SYNC_MISSING;
    uint16_t mode;
    if (osfs3_get_mode(ino, &mode)) return SYNC_IO;
    if ((mode & 0170000) == 0040000) { *kind = 2; *size = 0; }
    else if ((mode & 0170000) == 0100000) {
        *kind = 1; *size = osfs3_get_size(ino);
    } else return SYNC_INVALID;
    return SYNC_OK;
}

static int sync_list(const char *path)
{
    return osfs3_dir_cursor_open_ci(path, &cursor) < 0 ? SYNC_IO : SYNC_OK;
}

static int sync_next(char *name, unsigned cap, unsigned *kind)
{
    bool directory;
    uint32_t generation = cursor.generation;
    int rc = osfs3_dir_cursor_next(&cursor, name, cap, &directory, NULL, NULL);
    if (cursor.generation != generation) return SYNC_CHANGED;
    if (rc < 0) return SYNC_IO;
    if (!rc) return SYNC_END;
    *kind = directory ? 2 : 1;
    return SYNC_OK;
}

static int sync_open(const char *path, uint64_t *size)
{
    source = osfs3_find_ci(path);
    if (!source) return SYNC_MISSING;
    if (osfs3_file_retain(source) < 0) { source = NULL; return SYNC_IO; }
    source_revision = osfs3_file_revision(source);
    if (source_revision & 1) {
        osfs3_file_release(source); source = NULL; return SYNC_CHANGED;
    }
    *size = osfs3_file_size(source);
    return SYNC_OK;
}

static int sync_read(uint64_t offset, void *p, unsigned n)
{
    if (!source || osfs3_file_revision(source) != source_revision)
        return SYNC_CHANGED;
    if (n && osfs3_read_file(source, offset, p, n) < 0) return SYNC_IO;
    return osfs3_file_revision(source) == source_revision ? SYNC_OK : SYNC_CHANGED;
}

static void sync_close(void)
{
    if (source) osfs3_file_release(source);
    source = NULL;
}

static void sync_abort(void)
{
    if (staged) {
        void *owned = staged;
        osfs3_file_release(staged); staged = NULL;
        if (osfs3_find_ci(temporary) == owned) osfs3_delete(temporary);
    }
    if (previous) osfs3_file_release(previous);
    previous = NULL;
    destination[0] = temporary[0] = 0;
}

static int sync_begin(const char *path, uint64_t size)
{
    unsigned kind;
    uint64_t old_size;
    int status = sync_stat(path, &kind, &old_size);
    if (status != SYNC_OK && status != SYNC_MISSING) return status;
    if (!status && kind != 1) return SYNC_INVALID;
    strcpy(destination, path);
    destination_mode = 0644;
    previous_id = previous_revision = 0;
    if (!status) {
        previous = osfs3_find_ci(path);
        if (!previous || osfs3_file_retain(previous) < 0) {
            previous = NULL; return SYNC_CHANGED;
        }
        previous_id = osfs3_file_id(previous);
        previous_revision = osfs3_file_revision(previous);
        if ((previous_revision & 1) ||
            osfs3_file_get_mode(previous, &destination_mode) < 0) {
            sync_abort(); return SYNC_CHANGED;
        }
    }
    unsigned parent = 0;
    for (unsigned i = 0; path[i]; i++) if (path[i] == '/') parent = i + 1;
    if (parent + 29 >= sizeof(temporary)) { sync_abort(); return SYNC_INVALID; }
    unsigned char random[8];
    if (random_secure_bytes(random, sizeof(random))) { sync_abort(); return SYNC_IO; }
    memcpy(temporary, path, parent);
    memcpy(temporary + parent, ".osync-", 7);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; i++) {
        temporary[parent + 7 + 2 * i] = hex[random[i] >> 4];
        temporary[parent + 8 + 2 * i] = hex[random[i] & 15];
    }
    strcpy(temporary + parent + 23, ".part");
    /* create is exclusive. Never truncate a stale or unrelated staging file. */
    staged = osfs3_create(temporary, size);
    if (!staged) { sync_abort(); return SYNC_IO; }
    if (osfs3_file_retain(staged) < 0) {
        staged = NULL; osfs3_delete(temporary); sync_abort(); return SYNC_IO;
    }
    if (osfs3_file_set_mode(staged, 0600) < 0) { sync_abort(); return SYNC_IO; }
    return SYNC_OK;
}

static int sync_write(uint64_t offset, const void *p, unsigned n)
{
    return osfs3_write_data(staged, offset, p, n) < 0 ? SYNC_IO : SYNC_OK;
}

static int sync_readback(uint64_t offset, void *p, unsigned n)
{
    return osfs3_read_file(staged, offset, p, n) < 0 ? SYNC_IO : SYNC_OK;
}

static int sync_commit(void)
{
    void *current = osfs3_find_ci(destination);
    if (current != previous || (previous &&
        (osfs3_file_id(previous) != previous_id ||
         osfs3_file_revision(previous) != previous_revision))) return SYNC_CHANGED;
    if (osfs3_file_set_mode(staged, destination_mode & 07777U) < 0 || disk_flush() < 0)
        return SYNC_IO;
    /* Replacement rejects busy targets. Release our own reference first. */
    if (previous) osfs3_file_release(previous);
    previous = NULL;
    int rc = osfs3_rename(temporary, destination, previous_id != 0);
    if (rc < 0) return rc == -3 ? SYNC_BUSY : SYNC_IO;
    osfs3_file_release(staged); staged = NULL;
    temporary[0] = destination[0] = 0;
    return disk_flush() < 0 ? SYNC_IO : SYNC_OK;
}

static int sync_mkdir(const char *path)
{
    unsigned kind; uint64_t size;
    int rc = sync_stat(path, &kind, &size);
    if (!rc) return kind == 2 ? SYNC_OK : SYNC_INVALID;
    if (rc != SYNC_MISSING) return rc;
    return osfs3_mkdir(path) < 0 ? SYNC_IO : SYNC_OK;
}

const file_sync_ops_t file_sync_osfs = {
    sync_stat, sync_list, sync_next, sync_open, sync_read, sync_close,
    sync_begin, sync_write, sync_readback, sync_commit, sync_abort, sync_mkdir
};
