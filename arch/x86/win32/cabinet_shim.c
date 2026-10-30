/* FDI consumes application I/O callbacks, including virtual files pointing
 * inside a bootstrapper EXE. No filesystem path or installer payload is
 * substituted for those callbacks. CAB decompression is shared with SetupAPI. */
#include "cabinet_shim.h"
#include "cab.h"
#include "compat32.h"
#include "kernel32_shim.h"

extern void *kmalloc(uint64_t bytes);
extern void kfree(void *pointer);
extern DWORD win32_current_process_id(void);

#define FDI_CONTEXTS 128
#define FDI_MAX_CABINET (128U * 1024U * 1024U)
#define FDI_IO_SIZE 32768U
enum { FDI_FREE, FDI_BUSY, FDI_IDLE };
enum { FDI_ALLOC, FDI_FREE_MEM, FDI_OPEN, FDI_READ, FDI_WRITE, FDI_CLOSE, FDI_SEEK };
typedef struct { int operation, type, error; } FDI_ERF;
typedef struct {
    LONG bytes;
    PVOID name, disk, path, user;
    LONG_PTR file;
    USHORT date, time, attributes, set, number, folder;
    int error;
} FDI_NOTICE64;
typedef struct {
    LONG bytes;
    DWORD name, disk, path, user, file;
    USHORT date, time, attributes, set, number, folder;
    int error;
} FDI_NOTICE32;
_Static_assert(sizeof(FDI_NOTICE32) == 40, "FDINOTIFICATION PE32");
_Static_assert(sizeof(FDI_NOTICE64) == 64, "FDINOTIFICATION PE64");
typedef struct {
    BYTE notice[64];
    char path[260], name[260], full_path[520], empty[1];
    BYTE io[FDI_IO_SIZE];
} FDI_MEMORY;
typedef struct {
    DWORD state, handle, owner;
    BOOL compat;
    PVOID callbacks[7];
    FDI_ERF *error;
    FDI_MEMORY *memory;
    BYTE *cabinet_data, *file_data;
    cab_archive_t *archive;
    LONG_PTR input, output;
} FDI_CONTEXT;
static FDI_CONTEXT fdi_contexts[FDI_CONTEXTS];
static DWORD fdi_next_handle = 0x0FD10000U;

static void fdi_set_error(FDI_CONTEXT *context, int operation)
{
    if (win32_user_range_writable(context->error, sizeof(*context->error), context->compat))
        *context->error = (FDI_ERF){operation, 0, operation != 0};
}

static ULONG_PTR fdi_call(FDI_CONTEXT *context, unsigned callback,
                           ULONG_PTR a, ULONG_PTR b, ULONG_PTR c)
{
    PVOID function = context->callbacks[callback];
    if (!win32_user_range_executable(function, 1, context->compat)) return (ULONG_PTR)-1;
    unsigned count = callback == FDI_ALLOC || callback == FDI_FREE_MEM ||
                     callback == FDI_CLOSE ? 1 : 3;
    if (context->compat) {
        uint32_t arguments[3] = {(uint32_t)a, (uint32_t)b, (uint32_t)c};
        uint32_t stack = compat32_current_user_stack_top();
        return stack ? compat32_callback_args_on_stack((uint32_t)(ULONG_PTR)function,
                           (int)count, arguments, stack)
                     : compat32_callback_args((uint32_t)(ULONG_PTR)function,
                           (int)count, arguments);
    }
    if (count == 1) {
        typedef ULONG_PTR (WINAPI *CALL1)(ULONG_PTR);
        return ((CALL1)function)(a);
    }
    typedef ULONG_PTR (WINAPI *CALL3)(ULONG_PTR, ULONG_PTR, ULONG_PTR);
    return ((CALL3)function)(a, b, c);
}

static LONG_PTR fdi_signed_result(FDI_CONTEXT *context, ULONG_PTR result)
{
    return context->compat ? (LONG_PTR)(LONG)(DWORD)result : (LONG_PTR)result;
}

static FDI_CONTEXT *fdi_acquire(PVOID handle)
{
    ULONG_PTR id = (ULONG_PTR)handle;
    DWORD owner = win32_current_process_id();
    if (!id || id > 0xffffffffU) return NULL;
    for (unsigned i = 0; i < FDI_CONTEXTS; i++) {
        FDI_CONTEXT *context = &fdi_contexts[i];
        if (__atomic_load_n(&context->handle, __ATOMIC_ACQUIRE) != id ||
            __atomic_load_n(&context->owner, __ATOMIC_ACQUIRE) != owner) continue;
        DWORD expected = FDI_IDLE;
        if (!__atomic_compare_exchange_n(&context->state, &expected, FDI_BUSY,
                FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return NULL;
        if (context->handle == id && context->owner == owner) return context;
        __atomic_store_n(&context->state, FDI_IDLE, __ATOMIC_RELEASE);
    }
    return NULL;
}

static BOOL fdi_work_valid(FDI_CONTEXT *context)
{
    return win32_user_range_writable(context->memory, sizeof(*context->memory), context->compat);
}

static int fdi_string(FDI_CONTEXT *context, PCSTR source, char *destination,
                       unsigned capacity)
{
    if (!source) return -1;
    for (unsigned i = 0; i < capacity; i++) {
        if (!win32_user_range_readable(source + i, 1, context->compat)) return -1;
        destination[i] = source[i];
        if (!source[i]) return (int)i;
    }
    return -1;
}

static LONG_PTR fdi_notify(FDI_CONTEXT *context, PVOID callback, int kind,
                            FDI_NOTICE64 *notice)
{
    if (!fdi_work_valid(context) ||
        !win32_user_range_executable(callback, 1, context->compat)) return -1;
    if (context->compat) {
        FDI_NOTICE32 *out = (void *)context->memory->notice;
        *out = (FDI_NOTICE32){notice->bytes, (DWORD)(ULONG_PTR)notice->name,
            (DWORD)(ULONG_PTR)notice->disk, (DWORD)(ULONG_PTR)notice->path,
            (DWORD)(ULONG_PTR)notice->user, (DWORD)notice->file,
            notice->date, notice->time, notice->attributes, notice->set,
            notice->number, notice->folder, notice->error};
        uint32_t arguments[2] = {(uint32_t)kind, (uint32_t)(ULONG_PTR)out};
        uint32_t stack = compat32_current_user_stack_top();
        LONG result = (LONG)(stack ? compat32_callback_args_on_stack(
            (uint32_t)(ULONG_PTR)callback, 2, arguments, stack) :
            compat32_callback_args((uint32_t)(ULONG_PTR)callback, 2, arguments));
        if (!fdi_work_valid(context)) return -1;
        notice->bytes = out->bytes;
        notice->folder = out->folder;
        return result;
    }
    FDI_NOTICE64 *out = (void *)context->memory->notice;
    *out = *notice;
    typedef LONG_PTR (WINAPI *NOTIFY)(int, FDI_NOTICE64 *);
    LONG_PTR result = ((NOTIFY)callback)(kind, out);
    if (!fdi_work_valid(context)) return -1;
    notice->bytes = out->bytes;
    notice->folder = out->folder;
    return result;
}

static void fdi_discard_kernel_data(FDI_CONTEXT *context)
{
    if (context->file_data) { kfree(context->file_data); context->file_data = NULL; }
    if (context->archive) { cab_close(context->archive); context->archive = NULL; }
}

static void fdi_finish_copy(FDI_CONTEXT *context)
{
    if (context->output) {
        LONG_PTR output = context->output;
        context->output = 0;
        fdi_call(context, FDI_CLOSE, (ULONG_PTR)output, 0, 0);
    }
    if (context->input != -1) {
        LONG_PTR input = context->input;
        context->input = -1;
        fdi_call(context, FDI_CLOSE, (ULONG_PTR)input, 0, 0);
    }
    fdi_discard_kernel_data(context);
    if (context->cabinet_data) {
        BYTE *data = context->cabinet_data;
        context->cabinet_data = NULL;
        fdi_call(context, FDI_FREE_MEM, (ULONG_PTR)data, 0, 0);
    }
}

static PVOID WINAPI FDICreate(PVOID allocate, PVOID release, PVOID open,
                               PVOID read, PVOID write, PVOID close,
                               PVOID seek, int cpu, FDI_ERF *error)
{
    (void)cpu; /* Only meaningful to the historical 16-bit implementation. */
    BOOL compat = g_compat32_mode != 0;
    if (!win32_user_range_writable(error, sizeof(*error), compat)) return NULL;
    *error = (FDI_ERF){0};
    PVOID callbacks[7] = {allocate, release, open, read, write, close, seek};
    for (unsigned i = 0; i < 7; i++)
        if (!win32_user_range_executable(callbacks[i], 1, compat)) {
            *error = (FDI_ERF){11, 0, TRUE};
            return NULL;
        }
    FDI_CONTEXT *context = NULL;
    for (unsigned i = 0; i < FDI_CONTEXTS; i++) {
        DWORD expected = FDI_FREE;
        if (__atomic_compare_exchange_n(&fdi_contexts[i].state, &expected,
                FDI_BUSY, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            context = &fdi_contexts[i]; break;
        }
    }
    if (!context) { *error = (FDI_ERF){5, 0, TRUE}; return NULL; }
    DWORD id = __atomic_add_fetch(&fdi_next_handle, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&context->handle, id, __ATOMIC_RELEASE);
    __atomic_store_n(&context->owner, win32_current_process_id(), __ATOMIC_RELEASE);
    context->compat = compat;
    context->error = error;
    context->memory = NULL;
    context->cabinet_data = context->file_data = NULL;
    context->archive = NULL;
    context->input = -1;
    context->output = 0;
    memcpy(context->callbacks, callbacks, sizeof(callbacks));
    context->memory = (PVOID)fdi_call(context, FDI_ALLOC, sizeof(FDI_MEMORY), 0, 0);
    if (!context->memory || !fdi_work_valid(context)) {
        fdi_set_error(context, 5);
        if (context->memory)
            fdi_call(context, FDI_FREE_MEM, (ULONG_PTR)context->memory, 0, 0);
        context->memory = NULL;
        __atomic_store_n(&context->state, FDI_FREE, __ATOMIC_RELEASE);
        return NULL;
    }
    memset(context->memory, 0, sizeof(*context->memory));
    __atomic_store_n(&context->state, FDI_IDLE, __ATOMIC_RELEASE);
    return (PVOID)(ULONG_PTR)id;
}

static DWORD fdi_u32(const BYTE *data)
{
    return (DWORD)data[0] | ((DWORD)data[1] << 8) |
           ((DWORD)data[2] << 16) | ((DWORD)data[3] << 24);
}

static int fdi_load_cabinet(FDI_CONTEXT *context)
{
    context->input = fdi_signed_result(context, fdi_call(context, FDI_OPEN,
        (ULONG_PTR)context->memory->full_path, 0x8000, 0x100));
    if (context->input == -1) return 1;
    if (!fdi_work_valid(context)) return 2;
    DWORD read = (DWORD)fdi_call(context, FDI_READ, (ULONG_PTR)context->input,
                                 (ULONG_PTR)context->memory->io, 36);
    if (read != 36 || !fdi_work_valid(context)) return 2;
    BYTE header[36];
    memcpy(header, context->memory->io, sizeof(header));
    if (memcmp(header, "MSCF", 4)) return 2;
    if (header[24] != 3 || header[25] != 1) return 3;
    DWORD length = fdi_u32(header + 8);
    if (length < 36) return 4;
    if (length > FDI_MAX_CABINET) return 5;
    context->cabinet_data = (PVOID)fdi_call(context, FDI_ALLOC, length, 0, 0);
    if (!win32_user_range_writable(context->cabinet_data, length, context->compat)) return 5;
    memcpy(context->cabinet_data, header, sizeof(header));
    DWORD offset = 36;
    while (offset < length) {
        DWORD chunk = length - offset;
        if (chunk > FDI_IO_SIZE) chunk = FDI_IO_SIZE;
        read = (DWORD)fdi_call(context, FDI_READ, (ULONG_PTR)context->input,
            (ULONG_PTR)(context->cabinet_data + offset), chunk);
        if (read > chunk) return 4;
        if (!read) break;
        offset += read;
    }
    if (!win32_user_range_readable(context->cabinet_data, offset, context->compat)) return 4;
    context->archive = cab_open(context->cabinet_data, offset);
    return context->archive ? 0 : 4;
}

static BOOL WINAPI FDICopy(PVOID handle, PSTR cabinet, PSTR path, int flags,
                            PVOID notify, PVOID decrypt, PVOID user)
{
    FDI_CONTEXT *context = fdi_acquire(handle);
    if (!context) return FALSE;
    int error = 0;
    fdi_set_error(context, 0);
    if (flags || decrypt || !fdi_work_valid(context) ||
        !win32_user_range_executable(notify, 1, context->compat)) { error = 11; goto done; }
    int path_length = fdi_string(context, path, context->memory->path, 260);
    int name_length = fdi_string(context, cabinet, context->memory->name, 260);
    if (path_length < 0 || name_length < 0) { error = 1; goto done; }
    memcpy(context->memory->full_path, context->memory->path, (unsigned)path_length);
    memcpy(context->memory->full_path + path_length, context->memory->name, (unsigned)name_length + 1);
    error = fdi_load_cabinet(context);
    if (error) goto done;
    cab_info_t info;
    cab_get_info(context->archive, &info);
    FDI_NOTICE64 notice = {.user = user, .set = info.set_id, .number = info.number};
    notice.name = (info.flags & 2) ? (PVOID)info.next_cabinet : context->memory->empty;
    notice.disk = (info.flags & 2) ? (PVOID)info.next_disk : context->memory->empty;
    notice.path = context->memory->path;
    if (fdi_notify(context, notify, 0, &notice) == -1) { error = 11; goto done; }

    int total = cab_file_count(context->archive);
    DWORD offset = fdi_u32(context->cabinet_data + 16);
    notice = (FDI_NOTICE64){.user = user, .set = info.set_id,
                            .bytes = (LONG)offset, .folder = (USHORT)total};
    if (fdi_notify(context, notify, 5, &notice) == -1) { error = 11; goto done; }
    if (!notice.folder) goto done;
    /* A supplied enumeration cursor must describe an actual CFFILE boundary.
     * This bounds callback-directed skips even for untrusted input. */
    int first = 0;
    while (first < total && offset != (DWORD)notice.bytes) {
        const char *name = cab_file_name(context->archive, first++);
        offset += 16U + (DWORD)strlen(name) + 1U;
    }
    if (first == total || notice.folder != total - first) { error = 4; goto done; }
    for (int index = first; index < total; index++) {
        cab_file_info_t file;
        if (cab_file_info(context->archive, index, &file)) { error = 4; break; }
        if (file.folder >= 0xfffdU) { error = 10; break; } /* Split cabinets not yet supported. */
        notice = (FDI_NOTICE64){.bytes = (LONG)file.size, .name = (PVOID)file.name,
            .user = user, .date = file.date, .time = file.time,
            .attributes = file.attributes, .folder = file.folder};
        LONG_PTR target = fdi_notify(context, notify, 2, &notice);
        if (target == -1) { error = 11; break; }
        if (target) {
            context->output = target;
            if (file.compression != 0 && file.compression != 1 && file.compression != 3) {
                error = 6; break;
            }
            uint32_t size = 0;
            context->file_data = cab_extract_index(context->archive, index, &size);
            if (!context->file_data || size != file.size) { error = 4; break; }
            for (DWORD written = 0; written < size;) {
                DWORD chunk = size - written;
                if (chunk > FDI_IO_SIZE) chunk = FDI_IO_SIZE;
                if (!fdi_work_valid(context)) { error = 8; break; }
                memcpy(context->memory->io, context->file_data + written, chunk);
                DWORD got = (DWORD)fdi_call(context, FDI_WRITE, (ULONG_PTR)target,
                    (ULONG_PTR)context->memory->io, chunk);
                if (got != chunk) { error = 8; break; }
                written += chunk;
            }
            kfree(context->file_data); context->file_data = NULL;
            if (error) break;
            notice.bytes = (file.attributes & 0x40) != 0;
            notice.attributes = file.attributes & ~0x40U;
            notice.file = target;
            context->output = 0; /* CLOSE_FILE_INFO owns closing, including on failure. */
            LONG_PTR result = fdi_notify(context, notify, 3, &notice);
            if (result == -1 || result == 0) { error = result == -1 ? 11 : 8; break; }
        }
        offset += 16U + (DWORD)strlen(file.name) + 1U;
    }
    if (!error) {
        notice = (FDI_NOTICE64){.user = user, .set = info.set_id, .bytes = (LONG)offset};
        if (fdi_notify(context, notify, 5, &notice) == -1) error = 11;
    }
done:
    fdi_finish_copy(context);
    fdi_set_error(context, error);
    __atomic_store_n(&context->state, FDI_IDLE, __ATOMIC_RELEASE);
    return error == 0;
}

static BOOL WINAPI FDIDestroy(PVOID handle)
{
    FDI_CONTEXT *context = fdi_acquire(handle);
    if (!context) return FALSE;
    FDI_MEMORY *memory = context->memory;
    context->memory = NULL;
    fdi_call(context, FDI_FREE_MEM, (ULONG_PTR)memory, 0, 0);
    __atomic_store_n(&context->state, FDI_FREE, __ATOMIC_RELEASE);
    return TRUE;
}

void cabinet_release_process(DWORD owner)
{
    /* Called after owned workers stop, before their VM disappears. Do not
     * execute allocator or file callbacks from a terminated application. */
    for (unsigned i = 0; i < FDI_CONTEXTS; i++) {
        FDI_CONTEXT *context = &fdi_contexts[i];
        if (__atomic_load_n(&context->state, __ATOMIC_ACQUIRE) == FDI_FREE ||
            __atomic_load_n(&context->owner, __ATOMIC_ACQUIRE) != owner) continue;
        fdi_discard_kernel_data(context);
        context->cabinet_data = NULL;
        context->memory = NULL;
        __atomic_store_n(&context->state, FDI_FREE, __ATOMIC_RELEASE);
    }
}

static const WIN32_EXPORT cabinet_exports[] = {
    WX_CDL("FDICreate", FDICreate, 9),
    WX_CDL("FDICopy", FDICopy, 7),
    WX_CDL("FDIDestroy", FDIDestroy, 1),
    {NULL, NULL, 0, CC_CDECL}
};
const WIN32_EXPORT *cabinet_abi_table(int *count)
{
    *count = sizeof(cabinet_exports) / sizeof(cabinet_exports[0]);
    return cabinet_exports;
}
PVOID cabinet_resolve(const char *name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) {
        if (ordinal == 20) return (PVOID)FDICreate;
        if (ordinal == 22) return (PVOID)FDICopy;
        if (ordinal == 23) return (PVOID)FDIDestroy;
        return NULL;
    }
    if (name) for (unsigned i = 0; cabinet_exports[i].name; i++)
        if (!strcmp(name, cabinet_exports[i].name)) return cabinet_exports[i].func;
    return NULL;
}
