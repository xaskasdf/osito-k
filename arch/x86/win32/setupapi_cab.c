/* SetupAPI cabinet callbacks use external runtime payloads and normal Win32
 * filesystem paths, including the caller's WOW64 redirection policy. */
#include "cab.h"
#include "setupapi_cab.h"
#include "kernel32_shim.h"
#include "ntdll_shim.h"
#include "compat32.h"
#include "wintime.h"

extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);
#define SETUP_CAB_PATH 260U
typedef struct {
    BYTE file_info[544], cabinet_info[32], paths[24];
    WCHAR cabinet_path[SETUP_CAB_PATH], full_path[SETUP_CAB_PATH];
    WCHAR target[SETUP_CAB_PATH], decoded_name[SETUP_CAB_PATH];
    BYTE source[SETUP_CAB_PATH * 2], name[SETUP_CAB_PATH * 2];
    BYTE directory[SETUP_CAB_PATH * 2], next[SETUP_CAB_PATH * 2];
    BYTE disk[SETUP_CAB_PATH * 2];
} SETUP_CAB_MEMORY;
typedef struct {
    PVOID callback, context;
    BOOL wide, compat;
    SETUP_CAB_MEMORY *memory;
} SETUP_CAB_STATE;

/* Copy and bound the caller's path before opening files or invoking callbacks. */
static DWORD setup_cab_string(PCVOID input, BOOL wide, BOOL compat, WCHAR *out)
{
    if (!input) return 87;
    unsigned step = wide ? 2U : 1U, length = 0;
    for (; length < SETUP_CAB_PATH; length++) {
        const BYTE *p = (const BYTE *)input + length * step;
        if (!win32_user_range_readable(p, step, compat)) return 87;
        if (wide ? !*(const WCHAR *)p : !*p) break;
    }
    if (length == SETUP_CAB_PATH) return 206;
    if (wide) memcpy(out, input, (length + 1U) * 2U);
    else if (!MultiByteToWideChar(0, 0, input, (int)length + 1, out, SETUP_CAB_PATH))
        return GetLastError();
    return 0;
}

static UINT setup_cab_notify(SETUP_CAB_STATE *s, UINT message, PVOID p1, PVOID p2)
{
    if (!win32_user_range_executable(s->callback, 1, s->compat)) return 87;
    if (s->compat) {
        uint32_t args[4] = {(uint32_t)(ULONG_PTR)s->context, message,
                            (uint32_t)(ULONG_PTR)p1, (uint32_t)(ULONG_PTR)p2};
        uint32_t stack = compat32_current_user_stack_top();
        return stack ? compat32_callback_args_on_stack((uint32_t)(ULONG_PTR)s->callback, 4, args, stack)
                     : compat32_callback_args((uint32_t)(ULONG_PTR)s->callback, 4, args);
    }
    typedef UINT (WINAPI *CALLBACK)(PVOID, UINT, ULONG_PTR, ULONG_PTR);
    return ((CALLBACK)s->callback)(s->context, message, (ULONG_PTR)p1, (ULONG_PTR)p2);
}

static DWORD setup_cab_copy_file(SETUP_CAB_STATE *s, cab_archive_t *cab,
                                  int index, const cab_file_info_t *info,
                                  BOOL *created)
{
    unsigned pointer_size = s->compat ? 4U : 8U;
    *created = FALSE;
    DWORD error = setup_cab_string(s->memory->file_info + pointer_size + 14U,
                                   s->wide, s->compat, s->memory->target);
    if (error) return error;
    if (!s->memory->target[0]) return 3;
    HANDLE output = CreateFileW(s->memory->target, GENERIC_WRITE, FILE_SHARE_READ,
                                 NULL, 2, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) return GetLastError();
    *created = TRUE;
    uint32_t size = 0;
    BYTE *data = cab_extract_index(cab, index, &size);
    if (!data) error = 13;
    for (DWORD offset = 0; !error && offset < size;) {
        DWORD written = 0, chunk = size - offset;
        if (chunk > 65536U) chunk = 65536U;
        if (!WriteFile(output, data + offset, chunk, &written, NULL) || !written)
            error = GetLastError() ? GetLastError() : 29;
        else offset += written;
    }
    if (data) kfree(data);
    if (!error) {
        FILE_BASIC_INFORMATION basic = {0};
        WINTIME_CALENDAR calendar = {0};
        calendar.year = 1980 + (info->date >> 9);
        calendar.month = (info->date >> 5) & 15;
        calendar.day = info->date & 31;
        calendar.hour = info->time >> 11;
        calendar.minute = (info->time >> 5) & 63;
        calendar.second = (info->time & 31) * 2;
        uint64_t timestamp;
        if (!wintime_calendar_to_filetime(&calendar, &timestamp))
            basic.LastWriteTime.QuadPart = (LONGLONG)timestamp;
        /* Extracted files are newly archived even if the CAB omitted that bit. */
        basic.FileAttributes = (info->attributes & 0x07U) | FILE_ATTRIBUTE_ARCHIVE;
        IO_STATUS_BLOCK iosb;
        NTSTATUS status = NtSetInformationFile(output, &iosb, &basic,
                                               sizeof(basic), FileBasicInformation);
        if (!NT_SUCCESS(status)) error = RtlNtStatusToDosError(status);
    }
    CloseHandle(output);
    return error;
}

static BOOL setup_cab_iterate(PCVOID path, DWORD reserved, PVOID callback,
                               PVOID context, BOOL wide)
{
    BOOL compat = g_compat32_mode != 0;
    if (reserved || !callback || !win32_user_range_executable(callback, 1, compat)) {
        SetLastError(87); return FALSE;
    }
    SETUP_CAB_STATE state = { .callback = callback, .context = context,
        .wide = wide, .compat = compat };
    SETUP_CAB_STATE *s = &state;
    s->memory = LocalAlloc(0x40, sizeof(*s->memory));
    if (!s->memory) { SetLastError(8); return FALSE; }
    DWORD error = setup_cab_string(path, wide, compat, s->memory->cabinet_path);
    HANDLE input = INVALID_HANDLE_VALUE;
    BYTE *data = NULL;
    cab_archive_t *cab = NULL;
    if (error) goto done;
    if (wide) memcpy(s->memory->source, s->memory->cabinet_path, sizeof(s->memory->cabinet_path));
    else if (!WideCharToMultiByte(0, 0, s->memory->cabinet_path, -1, (char *)s->memory->source,
                                 SETUP_CAB_PATH, NULL, NULL)) { error = GetLastError(); goto done; }
    input = CreateFileW(s->memory->cabinet_path, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, 3, FILE_ATTRIBUTE_NORMAL, NULL);
    if (input == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; }
    DWORD high = 0, size = GetFileSize(input, &high);
    if (high || size > 0x7fffffffU || size < 36) { error = 13; goto done; }
    data = kmalloc(size);
    if (!data) { error = 8; goto done; }
    for (DWORD offset = 0; offset < size;) {
        DWORD read = 0, chunk = size - offset;
        if (chunk > 65536U) chunk = 65536U;
        if (!ReadFile(input, data + offset, chunk, &read, NULL) || !read) {
            error = GetLastError() ? GetLastError() : 13; goto done;
        }
        offset += read;
    }
    CloseHandle(input); input = INVALID_HANDLE_VALUE;
    cab = cab_open(data, size);
    if (!cab) { error = 13; goto done; }
    cab_info_t ci;
    cab_get_info(cab, &ci);
    DWORD full_length = GetFullPathNameW(s->memory->cabinet_path, SETUP_CAB_PATH, s->memory->full_path, NULL);
    if (!full_length || full_length >= SETUP_CAB_PATH) { error = 206; goto done; }
    while (full_length && s->memory->full_path[full_length - 1] != '\\' && s->memory->full_path[full_length - 1] != '/')
        s->memory->full_path[--full_length] = 0;
    if (wide) memcpy(s->memory->directory, s->memory->full_path, sizeof(s->memory->full_path));
    else if (!WideCharToMultiByte(0, 0, s->memory->full_path, -1, (char *)s->memory->directory,
                                  SETUP_CAB_PATH, NULL, NULL)) { error = GetLastError(); goto done; }
    unsigned pointer_size = compat ? 4U : 8U;
    const char *media[2] = {ci.next_cabinet, ci.next_disk};
    BYTE *dest[2] = {s->memory->next, s->memory->disk};
    for (unsigned i = 0; i < 2; i++) {
        if (wide) {
            if (!MultiByteToWideChar(0, 0, media[i], -1, (PWSTR)dest[i], SETUP_CAB_PATH)) {
                error = 206; goto done;
            }
        } else {
            SIZE_T length = strlen(media[i]);
            if (length >= SETUP_CAB_PATH) { error = 206; goto done; }
            memcpy(dest[i], media[i], length + 1);
        }
    }
    ULONG_PTR pointers[3] = {(ULONG_PTR)s->memory->directory, (ULONG_PTR)s->memory->next, (ULONG_PTR)s->memory->disk};
    for (unsigned i = 0; i < 3; i++) memcpy(s->memory->cabinet_info + i * pointer_size, &pointers[i], pointer_size);
    memcpy(s->memory->cabinet_info + 3 * pointer_size, &ci.set_id, 2);
    memcpy(s->memory->cabinet_info + 3 * pointer_size + 2, &ci.number, 2);
    error = setup_cab_notify(s, 16, s->memory->cabinet_info, NULL);
    for (int index = 0; !error && index < cab_file_count(cab); index++) {
        cab_file_info_t info;
        if (cab_file_info(cab, index, &info)) { error = 13; break; }
        if (!MultiByteToWideChar((info.attributes & 0x80U) ? 65001U : 0U, 0,
            info.name, -1, s->memory->decoded_name, SETUP_CAB_PATH)) { error = 206; break; }
        if (wide) memcpy(s->memory->name, s->memory->decoded_name, sizeof(s->memory->decoded_name));
        else if (!WideCharToMultiByte(0, 0, s->memory->decoded_name, -1, (char *)s->memory->name,
                                      SETUP_CAB_PATH, NULL, NULL)) { error = 206; break; }
        ULONG_PTR name = (ULONG_PTR)s->memory->name;
        memcpy(s->memory->file_info, &name, pointer_size);
        memcpy(s->memory->file_info + pointer_size, &info.size, 4);
        memset(s->memory->file_info + pointer_size + 4, 0, 4);
        memcpy(s->memory->file_info + pointer_size + 8, &info.date, 2);
        memcpy(s->memory->file_info + pointer_size + 10, &info.time, 2);
        memcpy(s->memory->file_info + pointer_size + 12, &info.attributes, 2);
        UINT action = setup_cab_notify(s, 17, s->memory->file_info, s->memory->source);
        if (!action) { memcpy(&error, s->memory->file_info + pointer_size + 4, 4); break; }
        if (action == 2) continue;
        if (action != 1) { error = 995; break; }
        BOOL created;
        error = setup_cab_copy_file(s, cab, index, &info, &created);
        if (created) {
            ULONG_PTR target = (ULONG_PTR)(s->memory->file_info + pointer_size + 14U);
            ULONG_PTR source = (ULONG_PTR)s->memory->source;
            memcpy(s->memory->paths, &target, pointer_size);
            memcpy(s->memory->paths + pointer_size, &source, pointer_size);
            memcpy(s->memory->paths + pointer_size * 2, &error, 4);
            DWORD result = setup_cab_notify(s, 19, s->memory->paths, NULL);
            /* Cabinet extraction failures remove the incomplete target. A
             * callback's failure after successful extraction retains it. */
            if (error) DeleteFileW(s->memory->target);
            if (!error) error = result;
        }
    }
done:
    if (cab) cab_close(cab);
    if (data) kfree(data);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    LocalFree(s->memory);
    SetLastError(error);
    return error == 0;
}

BOOL WINAPI shim_SetupIterateCabinetA(PCSTR path, DWORD reserved, PVOID callback, PVOID context)
{
    return setup_cab_iterate(path, reserved, callback, context, FALSE);
}
BOOL WINAPI shim_SetupIterateCabinetW(PCWSTR path, DWORD reserved, PVOID callback, PVOID context)
{
    return setup_cab_iterate(path, reserved, callback, context, TRUE);
}
