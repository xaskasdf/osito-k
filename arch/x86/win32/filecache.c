/* Buffered NT file reads. No cache lock or reserved slot survives disk I/O. */
#include "filecache.h"
#include "handle.h"
#include "../kernel/smp.h"

extern int osfs2_read(void *file, uint64_t offset, void *buf, uint64_t length);
extern uint64_t osfs2_file_revision(void *file);
extern void *memmove(void *dst, const void *src, uint64_t length);

typedef struct {
    uint64_t cookie;
    void *identity;
    uint64_t revision;
    uint64_t file_size;
    uint64_t offset;
    uint32_t length;
} NT_READ_KEY;

typedef struct {
    NT_READ_KEY key;
    uint64_t used;
    uint8_t data[NT_FILE_CACHE_BYTES];
} NT_READ_WINDOW;

static NT_READ_WINDOW g_read_windows[NT_FILE_CACHE_SLOTS];
static spinlock_t g_read_cache_lock = SPINLOCK_INIT;
static uint64_t g_read_cookie;
static uint64_t g_read_use;

#ifdef FILECACHE_HOST_TEST
static uint64_t read_cache_lock_irqsave(void);
static void read_cache_unlock_irqrestore(uint64_t flags);
#else
static uint64_t read_cache_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&g_read_cache_lock);
    return flags;
}

static void read_cache_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&g_read_cache_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}
#endif

void nt_file_cache_open(FILE_OBJECT *file)
{
    uint64_t flags = read_cache_lock_irqsave();
    /* Never recycle a cookie, even after a close racing with an old fill. */
    file->read_cache.cookie = g_read_cookie == UINT64_MAX ? 0 : ++g_read_cookie;
    file->read_cache.sequential = 0;
    file->read_cache.next_offset = 0;
    read_cache_unlock_irqrestore(flags);
}

void nt_file_cache_close(FILE_OBJECT *file)
{
    uint64_t flags = read_cache_lock_irqsave();
    uint64_t cookie = file->read_cache.cookie;
    file->read_cache.cookie = 0;
    file->read_cache.sequential = 0;
    for (uint32_t i = 0; i < NT_FILE_CACHE_SLOTS; i++)
        if (g_read_windows[i].key.cookie == cookie)
            g_read_windows[i].key.cookie = 0;
    read_cache_unlock_irqrestore(flags);
}

static int read_cache_key(FILE_OBJECT *file, uint64_t offset, uint32_t length,
                           uint64_t file_size, NT_READ_KEY *key)
{
    if (!file->read_cache.cookie || !file->osfs_file || !length ||
        length > NT_FILE_CACHE_BYTES || offset > file_size ||
        length > file_size - offset ||
        (file->create_options & FILE_NO_INTERMEDIATE_BUFFERING))
        return 0;
    *key = (NT_READ_KEY) {
        .cookie = file->read_cache.cookie,
        .identity = file->osfs_file,
        .revision = osfs2_file_revision(file->osfs_file),
        .file_size = file_size,
        .offset = offset,
        .length = length,
    };
    return !(key->revision & 1U);
}

static int read_cache_copy(const NT_READ_KEY *key, void *buffer)
{
    int found = 0;
    uint64_t flags = read_cache_lock_irqsave();
    for (uint32_t i = 0; i < NT_FILE_CACHE_SLOTS; i++) {
        NT_READ_WINDOW *window = &g_read_windows[i];
        NT_READ_KEY *saved = &window->key;
        if (saved->cookie != key->cookie || saved->identity != key->identity ||
            saved->revision != key->revision ||
            saved->file_size != key->file_size || key->offset < saved->offset)
            continue;
        uint64_t delta = key->offset - saved->offset;
        if (delta > saved->length || key->length > saved->length - delta)
            continue;
        if (buffer) {
            memcpy(buffer, window->data + delta, key->length);
            window->used = ++g_read_use;
        }
        found = 1;
        break;
    }
    read_cache_unlock_irqrestore(flags);
    return found;
}

static void read_cache_store(const NT_READ_KEY *key, const void *buffer)
{
    uint64_t flags = read_cache_lock_irqsave();
    uint32_t victim = 0;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < NT_FILE_CACHE_SLOTS; i++) {
        NT_READ_WINDOW *window = &g_read_windows[i];
        if (window->key.cookie == key->cookie) {
            victim = i;
            break;
        }
        if (!window->key.cookie) {
            victim = i;
            oldest = 0;
        } else if (window->used < oldest) {
            victim = i;
            oldest = window->used;
        }
    }
    NT_READ_WINDOW *window = &g_read_windows[victim];
    window->key = *key;
    memcpy(window->data, buffer, key->length);
    window->used = ++g_read_use;
    read_cache_unlock_irqrestore(flags);
}

static uint32_t read_ahead_length(FILE_OBJECT *file, const NT_READ_KEY *key,
                                   uint64_t *base)
{
    *base = key->offset;
    if ((file->create_options & FILE_RANDOM_ACCESS) ||
        (!(file->create_options & FILE_SEQUENTIAL_ONLY) &&
         (!file->read_cache.sequential ||
          file->read_cache.next_offset != key->offset)))
        return key->length;
    uint64_t start = key->offset & ~(uint64_t)(NT_FILE_CACHE_BYTES - 1);
    uint64_t available = key->file_size - start;
    uint32_t length = available < NT_FILE_CACHE_BYTES
        ? (uint32_t)available : NT_FILE_CACHE_BYTES;
    /* A request crossing a window stays a single ordinary read. */
    if (key->offset - start + key->length > length)
        return key->length;
    *base = start;
    return length;
}

uint32_t nt_file_read_capacity(FILE_OBJECT *file, uint64_t offset,
                              uint32_t length, uint64_t file_size)
{
    NT_READ_KEY key;
    if (!read_cache_key(file, offset, length, file_size, &key) ||
        read_cache_copy(&key, NULL))
        return length;
    uint64_t base;
    return read_ahead_length(file, &key, &base);
}

int nt_file_cached_read(FILE_OBJECT *file, uint64_t offset,
                        void *buffer, uint32_t length, uint32_t capacity,
                        uint64_t file_size)
{
    if (!buffer || capacity < length || length > 0x7FFFFFFFU)
        return -1;
    NT_READ_KEY key;
    int cacheable = read_cache_key(file, offset, length, file_size, &key);
    int result;
    if (cacheable && read_cache_copy(&key, buffer) &&
        osfs2_file_revision(key.identity) == key.revision) {
        result = (int)length;
    } else {
        uint64_t base = offset;
        uint32_t ahead = cacheable ? read_ahead_length(file, &key, &base) : length;
        if (cacheable && ahead > length && capacity >= ahead) {
            result = osfs2_read(key.identity, base, buffer, ahead);
            if (result == (int)ahead &&
                osfs2_file_revision(key.identity) == key.revision) {
                NT_READ_KEY filled = key;
                filled.offset = base;
                filled.length = ahead;
                read_cache_store(&filled, buffer);
                memmove(buffer, (uint8_t *)buffer + (offset - base), length);
                result = (int)length;
                goto done;
            }
            /* A speculative range can fail while the requested bytes remain
             * readable. Never turn a read-ahead failure into a user failure. */
        }
        if (cacheable)
            key.revision = osfs2_file_revision(key.identity);
        result = osfs2_read(file->osfs_file, offset, buffer, length);
        if (cacheable && !(key.revision & 1U) && result == (int)length &&
            osfs2_file_revision(key.identity) == key.revision)
            read_cache_store(&key, buffer);
    }
done:
    file->read_cache.sequential = length && result == (int)length &&
                                   offset <= UINT64_MAX - length;
    if (file->read_cache.sequential)
        file->read_cache.next_offset = offset + length;
    return result;
}
