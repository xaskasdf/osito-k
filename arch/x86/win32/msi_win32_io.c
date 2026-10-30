#include "msi_win32_io.h"
#include "kernel32_shim.h"
#include "advapi32_shim.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);

static uint32_t io_last_error(void)
{ uint32_t error = GetLastError(); return error ? error : 31; }
static HKEY io_root(uint32_t index)
{ return (HKEY)(ULONG_PTR)(LONG)(0x80000000U + index); }

uint32_t msi_win32_read_file(void *context, const uint16_t *path,
    uint8_t **data, uint32_t *size, int *present)
{
    (void)context;
    *data = NULL; *size = 0; *present = 0;
    HANDLE file = CreateFileW(path, 0x80000000U, 1, NULL, 3, 0x80, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        uint32_t error = io_last_error(); return error == 2 || error == 3 ? 0 : error;
    }
    *present = 1;
    DWORD high = 0, length = GetFileSize(file, &high), error = 0;
    if (high || length > 128U * 1024 * 1024) error = 8;
    uint8_t *buffer = !error ? kmalloc(length ? length : 1) : NULL;
    if (!error && !buffer) error = 8;
    DWORD offset = 0;
    while (!error && offset < length) {
        DWORD bytes = 0, wanted = length - offset;
        if (wanted > 65536) wanted = 65536;
        if (!ReadFile(file, buffer + offset, wanted, &bytes, NULL)) error = io_last_error();
        else if (!bytes || bytes > wanted) error = 38;
        else offset += bytes;
    }
    CloseHandle(file);
    if (error) { if (buffer) kfree(buffer); return error; }
    *data = buffer; *size = length; return 0;
}
static uint32_t io_create_file(void *context, const uint16_t *path,
    const uint8_t *data, uint32_t size, int *created)
{
    (void)context; *created = 0;
    /* Directory creation belongs to the execution plan. Do not create parents
     * opportunistically here, since this callback only journals a file. */
    HANDLE file = CreateFileW(path, 0x40000000U, 0, NULL, 1, 0x80, NULL);
    if (file == INVALID_HANDLE_VALUE) return io_last_error();
    *created = 1;
    uint32_t error = 0, offset = 0;
    while (!error && offset < size) {
        DWORD bytes = 0, wanted = size - offset;
        if (wanted > 65536) wanted = 65536;
        if (!WriteFile(file, data + offset, wanted, &bytes, NULL)) error = io_last_error();
        else if (!bytes || bytes > wanted) error = 29;
        else offset += bytes;
    }
    if (!CloseHandle(file) && !error) error = io_last_error();
    return error;
}
static uint32_t io_remove_file(void *context, const uint16_t *path)
{
    (void)context;
    if (DeleteFileW(path)) return 0;
    uint32_t error = io_last_error(); return error == 2 || error == 3 ? 0 : error;
}
static uint32_t io_read_registry(void *context, const msi_registry_value_t *value, msi_registry_before_t *before)
{
    (void)context; memset(before, 0, sizeof(*before));
    HKEY key = NULL;
    LONG error = RegOpenKeyExW(io_root(value->root), value->path, 0, KEY_READ | value->view, &key);
    if (error) return error == 2 ? 0 : (uint32_t)error;
    before->key_exists = 1;
    if (value->create_only) { RegCloseKey(key); return 0; }
    DWORD size = 0, type = 0;
    error = RegQueryValueExW(key, value->name, NULL, &type, NULL, &size);
    if (error == 2) { RegCloseKey(key); return 0; }
    if (!error && size > 1024U * 1024) error = 8;
    if (!error) {
        before->data = kmalloc(size ? size : 1);
        if (!before->data) error = 8;
        else {
            DWORD available = size;
            error = RegQueryValueExW(key, value->name, NULL, &type, before->data, &available);
            if (!error && available > size) error = 13;
            if (!error) { before->present = 1; before->size = available; before->type = type; }
        }
    }
    RegCloseKey(key); return (uint32_t)error;
}
static uint32_t io_write_registry(void *context, const msi_registry_value_t *value, int *changed)
{
    (void)context; *changed = 0;
    HKEY key = NULL; DWORD disposition = 0;
    LONG error = RegCreateKeyExW(io_root(value->root), value->path, 0, NULL, 0,
        KEY_READ | KEY_WRITE | value->view, NULL, &key, &disposition);
    if (error) return (uint32_t)error;
    if (disposition == REG_CREATED_NEW_KEY) *changed = 1;
    if (!value->create_only) {
        error = RegSetValueExW(key, value->name, 0, value->type, value->data, value->size);
        if (!error) *changed = 1;
    }
    RegCloseKey(key);
    return (uint32_t)error;
}
static uint32_t io_restore_registry(void *context, const msi_registry_value_t *value, const msi_registry_before_t *before)
{
    (void)context;
    HKEY key = NULL;
    LONG error = RegOpenKeyExW(io_root(value->root), value->path, 0, KEY_READ | KEY_WRITE | value->view, &key);
    if (error) return error == 2 && !before->present ? 0 : (uint32_t)error;
    if (!value->create_only) {
        if (before->present) error = RegSetValueExW(key, value->name, 0, before->type, before->data, before->size);
        else {
            error = RegDeleteValueW(key, value->name);
            if (error == 2) error = 0;
        }
    }
    DWORD subkeys = 0, values = 0;
    if (!error && !before->key_exists)
        error = RegQueryInfoKeyW(key, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
    RegCloseKey(key);
    if (!error && !before->key_exists && !subkeys && !values) {
        error = RegDeleteKeyExW(io_root(value->root), value->path, value->view, 0);
        if (error == 2) error = 0;
    }
    return (uint32_t)error;
}
static uint32_t io_flush(void *context)
{ (void)context; return (uint32_t)RegFlushKey(HKEY_LOCAL_MACHINE); }

const msi_apply_backend_t msi_win32_backend = {
    msi_win32_read_file, io_create_file, io_remove_file,
    io_read_registry, io_write_registry, io_restore_registry, io_flush, NULL
};
