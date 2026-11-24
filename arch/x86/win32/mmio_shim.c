/* Multimedia file readers used by external legacy runtime installers. */
#include "mmio_shim.h"
#include "kernel32_shim.h"
#include "compat32.h"
#include "../kernel/smp.h"

#define MMIO_SLOTS 128
#define MMIO_RIFF 0x46464952U
#define MMIO_LIST 0x5453494cU
#define MMIO_MEM  0x204d454dU
#define MMIO_DOS  0x20534f44U
#define MMIO_NOT_FOUND 265U
typedef struct {
    DWORD id, owner, references, io_busy;
    BOOL used, closed, keep_file, memory;
    HANDLE file;
    BYTE *buffer;
    LONG size, position;
} MMIO_STREAM;
static MMIO_STREAM mmio_streams[MMIO_SLOTS];
static spinlock_t mmio_table_lock = SPINLOCK_INIT;
static DWORD mmio_next_id = 0x72000000;

static uint64_t mmio_lock(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&mmio_table_lock); return flags;
}
static void mmio_unlock(uint64_t flags)
{
    spin_unlock(&mmio_table_lock);
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
}
static MMIO_STREAM *mmio_reference(HANDLE handle)
{
    DWORD owner = GetCurrentProcessId(); MMIO_STREAM *stream = NULL;
    uint64_t flags = mmio_lock();
    for (unsigned i = 0; i < MMIO_SLOTS; i++) {
        MMIO_STREAM *p = &mmio_streams[i];
        if (p->used && !p->closed && p->id == (ULONG_PTR)handle && p->owner == owner) {
            p->references++; stream = p; break;
        }
    }
    mmio_unlock(flags);
    /* Serialize a seek/read sequence without holding an IRQ-disabled lock
     * during file I/O. The reference prevents recycling while waiting. */
    if (stream) while (__atomic_exchange_n(&stream->io_busy, 1, __ATOMIC_ACQUIRE)) Sleep(0);
    return stream;
}
static void mmio_dereference(MMIO_STREAM *stream)
{
    HANDLE close = NULL;
    uint64_t flags = mmio_lock();
    __atomic_store_n(&stream->io_busy, 0, __ATOMIC_RELEASE);
    if (!--stream->references) {
        if (!stream->memory && !stream->keep_file) close = stream->file;
        memset(stream, 0, sizeof(*stream));
    }
    mmio_unlock(flags);
    if (close) CloseHandle(close);
}
static void mmio_set_error(PVOID info, DWORD error)
{
    if (!info) return;
    unsigned offset = g_compat32_mode ? 12 : 16;
    if (win32_user_range_writable((BYTE *)info + offset, 4, g_compat32_mode != 0))
        memcpy((BYTE *)info + offset, &error, 4);
}
static HANDLE mmio_open(PCVOID name, BOOL wide, PVOID info, DWORD flags)
{
    BOOL compat = g_compat32_mode != 0; unsigned step = compat ? 4 : 8;
    DWORD code = 0; ULONG_PTR procedure = 0, buffer = 0; LONG capacity = 0;
    if (info) {
        if (!win32_user_range_readable(info, 44 + 7 * step, compat)) return NULL;
        memcpy(&code, (BYTE *)info + 4, 4); memcpy(&procedure, (BYTE *)info + 8, step);
        memcpy(&capacity, (BYTE *)info + 12 + 2 * step, 4);
        memcpy(&buffer, (BYTE *)info + 16 + 2 * step, step);
    }
    /* Buffered pointer mutation, writable/custom storage and temporary-name
     * operations need separate contracts; never pretend to perform them. */
    if ((flags & ~0x10070U) || procedure ||
        (code && code != MMIO_MEM && code != MMIO_DOS) ||
        (code != MMIO_MEM && buffer)) {
        mmio_set_error(info, 8); return NULL;
    }
    BOOL memory = code == MMIO_MEM;
    HANDLE file = NULL; DWORD size = 0, error = 0;
    if (memory) {
        if (name || capacity < 0 || !win32_user_range_readable((PVOID)buffer, capacity, compat)) {
            mmio_set_error(info, 11); return NULL;
        }
        size = (DWORD)capacity;
    } else {
        if (!name) { mmio_set_error(info, 11); return NULL; }
        DWORD sharing;
        switch (flags & 0x70) {
        case 0: case 0x40: sharing = 3; break;
        case 0x10: sharing = 0; break;
        case 0x20: sharing = 1; break;
        case 0x30: sharing = 2; break;
        default: mmio_set_error(info, 11); return NULL;
        }
        file = wide ? CreateFileW(name, GENERIC_READ, sharing, NULL, 3, FILE_ATTRIBUTE_NORMAL, NULL) :
                      CreateFileA(name, GENERIC_READ, sharing, NULL, 3, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            DWORD last = GetLastError();
            error = last == 2 ? 257 : last == 3 ? 267 : last == 5 ? 268 : last == 32 ? 269 : 272;
            mmio_set_error(info, error); return NULL;
        }
        DWORD high = 0; size = GetFileSize(file, &high);
        if (high || size > 0x7fffffffU) { CloseHandle(file); mmio_set_error(info, 272); return NULL; }
    }
    HANDLE result = NULL; uint64_t irq = mmio_lock();
    for (unsigned i = 0; i < MMIO_SLOTS; i++) if (!mmio_streams[i].used) {
        MMIO_STREAM *stream = &mmio_streams[i]; memset(stream, 0, sizeof(*stream));
        if (++mmio_next_id >= 0x7fffffff) mmio_next_id = 0x72000001;
        stream->id = mmio_next_id; stream->owner = GetCurrentProcessId();
        stream->used = TRUE; stream->references = 1;
        stream->file = file; stream->memory = memory; stream->size = (LONG)size;
        stream->buffer = (BYTE *)buffer; result = (HANDLE)(ULONG_PTR)stream->id;
        break;
    }
    mmio_unlock(irq);
    if (!result && file) CloseHandle(file);
    mmio_set_error(info, result ? 0 : 258); return result;
}
HANDLE WINAPI shim_mmioOpenA(PSTR name, PVOID info, DWORD flags) { return mmio_open(name, FALSE, info, flags); }
HANDLE WINAPI shim_mmioOpenW(PWSTR name, PVOID info, DWORD flags) { return mmio_open(name, TRUE, info, flags); }

static LONG mmio_read(MMIO_STREAM *stream, PVOID buffer, LONG count)
{
    if (count < 0) return -1;
    if (stream->position >= stream->size) return 0;
    if (count > stream->size - stream->position) count = stream->size - stream->position;
    if (!count) return 0;
    if (stream->memory) {
        const void *source = stream->buffer + stream->position;
        if (!win32_user_range_readable(source, count, g_compat32_mode != 0)) return -1;
        memcpy(buffer, source, (SIZE_T)count); stream->position += count; return count;
    }
    if (SetFilePointer(stream->file, stream->position, NULL, 0) == 0xffffffffU) return -1;
    DWORD read = 0;
    if (!ReadFile(stream->file, buffer, count, &read, NULL)) return -1;
    stream->position += (LONG)read; return (LONG)read;
}
static LONG mmio_seek(MMIO_STREAM *stream, LONG offset, int origin)
{
    int64_t position;
    if (origin == 0) position = offset;
    else if (origin == 1) position = (int64_t)stream->position + offset;
    /* Unlike SetFilePointer, native MMIO subtracts SEEK_END's displacement. */
    else if (origin == 2) position = (int64_t)stream->size - offset;
    else return -1;
    if (position < 0 || position > 0x7fffffff || (stream->memory && position > stream->size)) return -1;
    stream->position = (LONG)position; return stream->position;
}
LONG WINAPI shim_mmioRead(HANDLE handle, PSTR buffer, LONG count)
{
    if (count < 0 || (count && !win32_user_range_writable(buffer, count, g_compat32_mode != 0))) return -1;
    MMIO_STREAM *stream = mmio_reference(handle); if (!stream) return -1;
    LONG result = mmio_read(stream, buffer, count); mmio_dereference(stream); return result;
}
LONG WINAPI shim_mmioSeek(HANDLE handle, LONG offset, int origin)
{
    MMIO_STREAM *stream = mmio_reference(handle); if (!stream) return -1;
    LONG result = mmio_seek(stream, offset, origin); mmio_dereference(stream); return result;
}
UINT WINAPI shim_mmioClose(HANDLE handle, UINT flags)
{
    if (flags & ~16U) return 11;
    MMIO_STREAM *stream = mmio_reference(handle); if (!stream) return 5;
    uint64_t irq = mmio_lock();
    if (stream->closed) { mmio_unlock(irq); mmio_dereference(stream); return 5; }
    stream->closed = TRUE; stream->keep_file = (flags & 16) != 0;
    stream->references--; /* Release the public handle's reference. */
    mmio_unlock(irq); mmio_dereference(stream); return 0;
}
UINT WINAPI shim_mmioDescend(HANDLE handle, OSITO_MMCKINFO *chunk,
                             const OSITO_MMCKINFO *parent, UINT flags)
{
    BOOL compat = g_compat32_mode != 0;
    if (!win32_user_range_writable(chunk, sizeof(*chunk), compat) ||
        (parent && !win32_user_range_readable(parent, sizeof(*parent), compat)) ||
        (flags != 0 && flags != 16 && flags != 32 && flags != 64)) return 11;
    MMIO_STREAM *stream = mmio_reference(handle); if (!stream) return 5;
    uint64_t limit = parent ? (uint64_t)parent->offset + parent->size : (DWORD)stream->size;
    if (limit > (DWORD)stream->size) limit = (DWORD)stream->size;
    DWORD wanted_id = flags == 32 ? MMIO_RIFF : flags == 64 ? MMIO_LIST : chunk->id;
    DWORD wanted_type = chunk->type; UINT result = MMIO_NOT_FOUND;
    for (unsigned scanned = 0; scanned < 65536 && (uint64_t)stream->position + 8 <= limit; scanned++) {
        OSITO_MMCKINFO found = {0}; DWORD header[2];
        if (mmio_read(stream, header, 8) != 8) break;
        found.id = header[0]; found.size = header[1]; found.offset = (DWORD)stream->position;
        uint64_t end = (uint64_t)found.offset + found.size;
        if (end > limit) break;
        if (found.id == MMIO_RIFF || found.id == MMIO_LIST) {
            if (found.size < 4 || mmio_read(stream, &found.type, 4) != 4) break;
        }
        if ((!flags || found.id == wanted_id) &&
            ((flags != 32 && flags != 64) || found.type == wanted_type)) {
            memcpy(chunk, &found, sizeof(found)); result = 0; break;
        }
        end += found.size & 1;
        if (end > 0x7fffffff || mmio_seek(stream, (LONG)end, 0) < 0) break;
    }
    mmio_dereference(stream); return result;
}
UINT WINAPI shim_mmioAscend(HANDLE handle, OSITO_MMCKINFO *chunk, UINT flags)
{
    if (!win32_user_range_readable(chunk, sizeof(*chunk), g_compat32_mode != 0) || flags || (chunk->flags & 0x10000000U)) return 11;
    MMIO_STREAM *stream = mmio_reference(handle); if (!stream) return 5;
    uint64_t end = (uint64_t)chunk->offset + chunk->size + (chunk->size & 1);
    UINT result = end <= 0x7fffffff && mmio_seek(stream, (LONG)end, 0) >= 0 ? 0 : 263;
    mmio_dereference(stream); return result;
}
void mmio_process_cleanup(DWORD owner)
{
    /* Owner threads are stopped. File handles and caller memory are reclaimed
     * by their existing owner-aware NT/heap reapers, not a foreign process. */
    uint64_t irq = mmio_lock();
    for (unsigned i = 0; i < MMIO_SLOTS; i++)
        if (mmio_streams[i].used && mmio_streams[i].owner == owner) memset(&mmio_streams[i], 0, sizeof(mmio_streams[i]));
    mmio_unlock(irq);
}
