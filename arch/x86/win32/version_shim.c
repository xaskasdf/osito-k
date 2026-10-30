/* version.dll support backed by native PE RT_VERSION resources. */

#include "version_shim.h"
#include "kernel32_shim.h"
#include "pe.h"
#include "win32_abi.h"
#include "dllloader.h"
#include "ntprocess.h"

extern void serial_puts(const char *s);

#define ERROR_RESOURCE_DATA_NOT_FOUND 1812
#define ERROR_INSUFFICIENT_BUFFER      122
#define ERROR_BAD_EXE_FORMAT           193
#define ERROR_INVALID_PARAMETER         87
#define ERROR_NOT_ENOUGH_MEMORY           8

#define OPEN_EXISTING                  3U
#define FILE_BEGIN                     0U
#define VERSION_RESOURCE_TYPE          16U
#define VERSION_MAX_SECTIONS           96U
#define VERSION_MAX_RESOURCE_SIZE      (16U * 1024U * 1024U)

#define VS_FFI_SIGNATURE       0xFEEF04BDU
#define VS_FFI_STRUCVERSION    0x00010000U
#define VOS_NT_WINDOWS32       0x00040004U
#define VFT_DLL                0x00000002U

typedef struct _VERSION_FIXED_FILE_INFO {
    DWORD dwSignature;
    DWORD dwStrucVersion;
    DWORD dwFileVersionMS;
    DWORD dwFileVersionLS;
    DWORD dwProductVersionMS;
    DWORD dwProductVersionLS;
    DWORD dwFileFlagsMask;
    DWORD dwFileFlags;
    DWORD dwFileOS;
    DWORD dwFileType;
    DWORD dwFileSubtype;
    DWORD dwFileDateMS;
    DWORD dwFileDateLS;
} VERSION_FIXED_FILE_INFO;

typedef struct _VERSION_BLOB {
    VERSION_FIXED_FILE_INFO fixed;
    WORD translation[2];
    WCHAR file_version[13];
    WCHAR product_version[13];
} VERSION_BLOB;

typedef struct _VERSION_RESOURCE_DIRECTORY {
    DWORD Characteristics;
    DWORD TimeDateStamp;
    WORD MajorVersion;
    WORD MinorVersion;
    WORD NumberOfNamedEntries;
    WORD NumberOfIdEntries;
} VERSION_RESOURCE_DIRECTORY;

typedef struct _VERSION_RESOURCE_ENTRY {
    DWORD Name;
    DWORD OffsetToData;
} VERSION_RESOURCE_ENTRY;

typedef struct _VERSION_RESOURCE_DATA_ENTRY {
    DWORD OffsetToData;
    DWORD Size;
    DWORD CodePage;
    DWORD Reserved;
} VERSION_RESOURCE_DATA_ENTRY;

typedef struct _VERSION_RESOURCE_LOCATION {
    DWORD file_offset;
    DWORD size;
} VERSION_RESOURCE_LOCATION;

typedef struct _VERSION_NT_PREFIX {
    DWORD Signature;
    IMAGE_FILE_HEADER FileHeader;
} VERSION_NT_PREFIX;

typedef struct _VERSION_NODE {
    const BYTE *start;
    const BYTE *end;
    const WCHAR *key;
    const BYTE *value;
    const BYTE *children;
    WORD value_length;
    WORD type;
} VERSION_NODE;

_Static_assert(sizeof(VERSION_RESOURCE_DIRECTORY) == 16,
               "PE resource directory layout");
_Static_assert(sizeof(VERSION_RESOURCE_ENTRY) == 8,
               "PE resource entry layout");
_Static_assert(sizeof(VERSION_RESOURCE_DATA_ENTRY) == 16,
               "PE resource data entry layout");
_Static_assert(sizeof(VERSION_NT_PREFIX) == 24, "PE NT prefix layout");

static const VERSION_BLOB kernel_version = {
    {
        VS_FFI_SIGNATURE,
        VS_FFI_STRUCVERSION,
        (WIN32_NT_VERSION_MAJOR << 16) | WIN32_NT_VERSION_MINOR,
        (WIN32_NT_VERSION_BUILD << 16),
        (WIN32_NT_VERSION_MAJOR << 16) | WIN32_NT_VERSION_MINOR,
        (WIN32_NT_VERSION_BUILD << 16),
        0x0000003FU,
        0,
        VOS_NT_WINDOWS32,
        VFT_DLL,
        0,
        0,
        0
    },
    { 0x0409, 1200 },
    { '1', '0', '.', '0', '.', '1', '9', '0', '4', '5', '.', '0', 0 },
    { '1', '0', '.', '0', '.', '1', '9', '0', '4', '5', '.', '0', 0 }
};

static WCHAR version_ascii_lower(WCHAR ch)
{
    if (ch >= 'A' && ch <= 'Z') return (WCHAR)(ch + ('a' - 'A'));
    return ch;
}

static BOOL version_wstr_eq_ascii(PCWSTR value, const char *ascii)
{
    if (!value || !ascii) return FALSE;
    while (*value && *ascii) {
        if (version_ascii_lower(*value) !=
            version_ascii_lower((WCHAR)(unsigned char)*ascii))
            return FALSE;
        value++;
        ascii++;
    }
    return *value == 0 && *ascii == 0;
}

static BOOL version_wstr_ends_ascii(PCWSTR value, const char *suffix)
{
    SIZE_T value_len = 0;
    SIZE_T suffix_len = 0;

    if (!value || !suffix) return FALSE;
    while (value[value_len]) value_len++;
    while (suffix[suffix_len]) suffix_len++;
    if (suffix_len > value_len) return FALSE;
    return version_wstr_eq_ascii(value + value_len - suffix_len, suffix);
}

static BOOL version_is_kernel_module(PCWSTR filename)
{
    return version_wstr_ends_ascii(filename, "kernel32.dll") ||
           version_wstr_ends_ascii(filename, "kernelbase.dll");
}

static void version_copy(void *dst, const void *src, SIZE_T length)
{
    BYTE *out = (BYTE *)dst;
    const BYTE *in = (const BYTE *)src;
    while (length--) *out++ = *in++;
}

static PWSTR version_ansi_to_wide(PCSTR value)
{
    if (!value) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    int count = MultiByteToWideChar(0, 0, value, -1, NULL, 0);
    if (count <= 0)
        return NULL;

    PWSTR wide = LocalAlloc(0, (SIZE_T)count * sizeof(WCHAR));
    if (!wide) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (!MultiByteToWideChar(0, 0, value, -1, wide, count)) {
        DWORD error = GetLastError();
        LocalFree(wide);
        SetLastError(error);
        return NULL;
    }
    return wide;
}

static void version_store_pointer(PVOID output, PVOID value)
{
    if (!output) return;
    if (g_compat32_mode)
        *(DWORD *)output = (DWORD)(ULONG_PTR)value;
    else
        *(PVOID *)output = value;
}

static BOOL version_range_valid(DWORD offset, DWORD length, DWORD limit)
{
    return offset <= limit && length <= limit - offset;
}

static BOOL version_read_at(HANDLE file, DWORD offset, PVOID buffer,
                            DWORD length)
{
    BYTE *out = (BYTE *)buffer;

    if (offset > 0x7FFFFFFFU ||
        SetFilePointer(file, (LONG)offset, NULL, FILE_BEGIN) != offset)
        return FALSE;

    while (length != 0) {
        DWORD read = 0;
        if (!ReadFile(file, out, length, &read, NULL) || read == 0)
            return FALSE;
        out += read;
        length -= read;
    }
    return TRUE;
}

static HANDLE version_open_file(PCWSTR filename, DWORD *file_size)
{
    DWORD high = 0;
    /* Selection has already resolved WOW64 and the CWD. Open that physical
     * file, restoring the thread setting before parsing any resources. */
    BOOL previous = nt_thread_fs_redirection(1);
    HANDLE file = CreateFileW(filename, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              NULL);
    DWORD error = GetLastError();
    nt_thread_fs_redirection(previous);
    SetLastError(error);
    if (file == INVALID_HANDLE_VALUE)
        return file;

    DWORD low = GetFileSize(file, &high);
    if (high != 0 || low == 0xFFFFFFFFU || low > 0x7FFFFFFFU) {
        CloseHandle(file);
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return INVALID_HANDLE_VALUE;
    }
    *file_size = low;
    return file;
}

static BOOL version_rva_to_file(const IMAGE_SECTION_HEADER *sections,
                                WORD section_count, DWORD size_of_headers,
                                DWORD file_size, DWORD rva, DWORD length,
                                DWORD *file_offset)
{
    if (rva < size_of_headers && version_range_valid(rva, length, file_size)) {
        *file_offset = rva;
        return TRUE;
    }

    for (WORD i = 0; i < section_count; i++) {
        DWORD section_rva = sections[i].VirtualAddress;
        DWORD raw_size = sections[i].SizeOfRawData;
        if (rva < section_rva)
            continue;

        DWORD delta = rva - section_rva;
        if (!version_range_valid(delta, length, raw_size))
            continue;
        if (delta > 0xFFFFFFFFU - sections[i].PointerToRawData)
            return FALSE;
        DWORD raw_offset = sections[i].PointerToRawData + delta;
        if (!version_range_valid(raw_offset, length, file_size))
            return FALSE;

        *file_offset = raw_offset;
        return TRUE;
    }
    return FALSE;
}

static BOOL version_read_resource_entry(HANDLE file, DWORD resource_base,
                                        DWORD resource_size,
                                        DWORD directory_offset,
                                        DWORD desired_id, const char *desired_name, BOOL first,
                                        VERSION_RESOURCE_ENTRY *result)
{
    VERSION_RESOURCE_DIRECTORY directory;
    if (!version_range_valid(directory_offset, sizeof(directory),
                             resource_size) ||
        !version_read_at(file, resource_base + directory_offset, &directory,
                         sizeof(directory)))
        return FALSE;

    DWORD count = (DWORD)directory.NumberOfNamedEntries +
                  (DWORD)directory.NumberOfIdEntries;
    DWORD entries_offset = directory_offset + sizeof(directory);
    if (count == 0 || count > 4096U ||
        !version_range_valid(entries_offset,
                             count * sizeof(VERSION_RESOURCE_ENTRY),
                             resource_size))
        return FALSE;

    for (DWORD i = 0; i < count; i++) {
        VERSION_RESOURCE_ENTRY entry;
        DWORD entry_offset = resource_base + entries_offset +
                             i * sizeof(VERSION_RESOURCE_ENTRY);
        if (!version_read_at(file, entry_offset, &entry, sizeof(entry)))
            return FALSE;
        BOOL named = FALSE;
        if (desired_name && (entry.Name & 0x80000000U)) {
            DWORD name_offset = entry.Name & 0x7FFFFFFFU;
            WORD chars;
            if (version_range_valid(name_offset, sizeof(chars), resource_size) &&
                version_read_at(file, resource_base + name_offset, &chars, sizeof(chars)) &&
                chars == 3 && version_range_valid(name_offset + 2, 6, resource_size)) {
                WCHAR name[4] = {0};
                named = version_read_at(file, resource_base + name_offset + 2, name, 6) &&
                        version_wstr_eq_ascii(name, desired_name);
            }
        }
        if (first || named || (!desired_name && !(entry.Name & 0x80000000U) &&
                      (entry.Name & 0xFFFFU) == desired_id)) {
            *result = entry;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL version_locate_resource(HANDLE file, DWORD file_size, const char *named_type,
                                    VERSION_RESOURCE_LOCATION *location)
{
    IMAGE_DOS_HEADER dos;
    VERSION_NT_PREFIX nt;
    union {
        IMAGE_OPTIONAL_HEADER32 header32;
        IMAGE_OPTIONAL_HEADER64 header64;
        BYTE bytes[sizeof(IMAGE_OPTIONAL_HEADER64)];
    } optional;
    IMAGE_SECTION_HEADER sections[VERSION_MAX_SECTIONS];
    DWORD resource_rva = 0;
    DWORD resource_size = 0;
    DWORD size_of_headers = 0;

    if (file_size < sizeof(dos) ||
        !version_read_at(file, 0, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        !version_range_valid((DWORD)dos.e_lfanew, sizeof(nt), file_size) ||
        !version_read_at(file, (DWORD)dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.NumberOfSections == 0 ||
        nt.FileHeader.NumberOfSections > VERSION_MAX_SECTIONS ||
        nt.FileHeader.SizeOfOptionalHeader > sizeof(optional)) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return FALSE;
    }

    DWORD optional_offset = (DWORD)dos.e_lfanew + sizeof(nt);
    if (!version_range_valid(optional_offset,
                             nt.FileHeader.SizeOfOptionalHeader, file_size) ||
        !version_read_at(file, optional_offset, optional.bytes,
                         nt.FileHeader.SizeOfOptionalHeader)) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return FALSE;
    }

    if (optional.header32.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        nt.FileHeader.SizeOfOptionalHeader >=
            sizeof(IMAGE_OPTIONAL_HEADER32) &&
        optional.header32.NumberOfRvaAndSizes >
            IMAGE_DIRECTORY_ENTRY_RESOURCE) {
        resource_rva = optional.header32
                           .DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE]
                           .VirtualAddress;
        resource_size = optional.header32
                            .DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE]
                            .Size;
        size_of_headers = optional.header32.SizeOfHeaders;
    } else if (optional.header64.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
               nt.FileHeader.SizeOfOptionalHeader >=
                   sizeof(IMAGE_OPTIONAL_HEADER64) &&
               optional.header64.NumberOfRvaAndSizes >
                   IMAGE_DIRECTORY_ENTRY_RESOURCE) {
        resource_rva = optional.header64
                           .DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE]
                           .VirtualAddress;
        resource_size = optional.header64
                            .DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE]
                            .Size;
        size_of_headers = optional.header64.SizeOfHeaders;
    } else {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return FALSE;
    }

    if (resource_rva == 0 || resource_size < sizeof(VERSION_RESOURCE_DIRECTORY)) {
        SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return FALSE;
    }

    DWORD section_offset = optional_offset + nt.FileHeader.SizeOfOptionalHeader;
    DWORD section_bytes =
        (DWORD)nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (!version_range_valid(section_offset, section_bytes, file_size) ||
        !version_read_at(file, section_offset, sections, section_bytes)) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return FALSE;
    }

    DWORD resource_base = 0;
    if (!version_rva_to_file(sections, nt.FileHeader.NumberOfSections,
                             size_of_headers, file_size, resource_rva,
                             sizeof(VERSION_RESOURCE_DIRECTORY),
                             &resource_base) ||
        !version_range_valid(resource_base, resource_size, file_size)) {
        SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return FALSE;
    }

    VERSION_RESOURCE_ENTRY type_entry;
    VERSION_RESOURCE_ENTRY name_entry;
    VERSION_RESOURCE_ENTRY language_entry;
    if (!version_read_resource_entry(file, resource_base, resource_size, 0,
                                     VERSION_RESOURCE_TYPE, named_type, FALSE,
                                     &type_entry) ||
        !(type_entry.OffsetToData & 0x80000000U) ||
        !version_read_resource_entry(
            file, resource_base, resource_size,
            type_entry.OffsetToData & 0x7FFFFFFFU, 0, NULL, TRUE, &name_entry) ||
        !(name_entry.OffsetToData & 0x80000000U) ||
        !version_read_resource_entry(
            file, resource_base, resource_size,
            name_entry.OffsetToData & 0x7FFFFFFFU, 0, NULL, TRUE,
            &language_entry) ||
        (language_entry.OffsetToData & 0x80000000U)) {
        SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return FALSE;
    }

    DWORD data_entry_offset = language_entry.OffsetToData & 0x7FFFFFFFU;
    VERSION_RESOURCE_DATA_ENTRY data_entry;
    if (!version_range_valid(data_entry_offset, sizeof(data_entry),
                             resource_size) ||
        !version_read_at(file, resource_base + data_entry_offset, &data_entry,
                         sizeof(data_entry)) ||
        data_entry.Size == 0 ||
        data_entry.Size > VERSION_MAX_RESOURCE_SIZE ||
        !version_rva_to_file(sections, nt.FileHeader.NumberOfSections,
                             size_of_headers, file_size,
                             data_entry.OffsetToData, data_entry.Size,
                             &location->file_offset)) {
        SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return FALSE;
    }

    location->size = data_entry.Size;
    return TRUE;
}

DWORD WINAPI shim_GetFileVersionInfoSizeW(PCWSTR filename, DWORD *handle)
{
    if (handle) *handle = 0;
    if (!filename) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (version_is_kernel_module(filename)) {
        SetLastError(0);
        serial_puts("[VERSION] kernel version 10.0.19045.0\n");
        return (DWORD)sizeof(kernel_version);
    }

    return shim_GetFileVersionInfoSizeExW(3, filename, handle);
}

DWORD WINAPI shim_GetFileVersionInfoSizeA(PCSTR filename, DWORD *handle)
{
    if (handle) *handle = 0;
    PWSTR wide = version_ansi_to_wide(filename);
    if (!wide)
        return 0;

    DWORD result = shim_GetFileVersionInfoSizeW(wide, handle);
    DWORD error = GetLastError();
    LocalFree(wide);
    SetLastError(error);
    return result;
}

BOOL WINAPI shim_GetFileVersionInfoW(PCWSTR filename, DWORD handle,
                                     DWORD length, PVOID data)
{
    (void)handle;
    if (!filename) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!version_is_kernel_module(filename)) {
        return shim_GetFileVersionInfoExW(3, filename, handle, length, data);
    }
    if (!data || length < sizeof(kernel_version)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    version_copy(data, &kernel_version, sizeof(kernel_version));
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI shim_GetFileVersionInfoA(PCSTR filename, DWORD handle,
                                     DWORD length, PVOID data)
{
    PWSTR wide = version_ansi_to_wide(filename);
    if (!wide)
        return FALSE;

    BOOL result = shim_GetFileVersionInfoW(wide, handle, length, data);
    DWORD error = GetLastError();
    LocalFree(wide);
    SetLastError(error);
    return result;
}

static BOOL version_query_string(const VERSION_BLOB *blob, PCWSTR sub_block,
                                 PVOID *buffer, UINT *length)
{
    if (version_wstr_ends_ascii(sub_block, "\\FileVersion")) {
        *buffer = (PVOID)blob->file_version;
        *length = (UINT)(sizeof(blob->file_version) / sizeof(WCHAR));
        return TRUE;
    }
    if (version_wstr_ends_ascii(sub_block, "\\ProductVersion")) {
        *buffer = (PVOID)blob->product_version;
        *length = (UINT)(sizeof(blob->product_version) / sizeof(WCHAR));
        return TRUE;
    }
    SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
    return FALSE;
}

static WORD version_read_u16(const BYTE *value)
{
    return (WORD)((WORD)value[0] | ((WORD)value[1] << 8));
}

static DWORD version_read_u32(const BYTE *value)
{
    return (DWORD)value[0] | ((DWORD)value[1] << 8) |
           ((DWORD)value[2] << 16) | ((DWORD)value[3] << 24);
}

static const BYTE *version_align4(const BYTE *value)
{
    return (const BYTE *)(((ULONG_PTR)value + 3U) & ~(ULONG_PTR)3U);
}

static BOOL version_parse_node(const BYTE *start, const BYTE *container_end,
                               VERSION_NODE *node)
{
    if (!start || !container_end || container_end < start ||
        (SIZE_T)(container_end - start) < 6U)
        return FALSE;

    WORD total_length = version_read_u16(start);
    WORD value_length = version_read_u16(start + 2);
    WORD type = version_read_u16(start + 4);
    if (total_length < 6U ||
        (SIZE_T)total_length > (SIZE_T)(container_end - start))
        return FALSE;

    const BYTE *end = start + total_length;
    const BYTE *cursor = start + 6;
    const BYTE *key_start = cursor;
    while ((SIZE_T)(end - cursor) >= sizeof(WCHAR) &&
           version_read_u16(cursor) != 0)
        cursor += sizeof(WCHAR);
    if ((SIZE_T)(end - cursor) < sizeof(WCHAR))
        return FALSE;

    cursor = version_align4(cursor + sizeof(WCHAR));
    if (cursor > end)
        return FALSE;

    SIZE_T value_bytes = type == 1 ? (SIZE_T)value_length * sizeof(WCHAR)
                                      : (SIZE_T)value_length;
    if (value_bytes > (SIZE_T)(end - cursor))
        return FALSE;

    const BYTE *children = version_align4(cursor + value_bytes);
    if (children > end)
        children = end;

    node->start = start;
    node->end = end;
    node->key = (const WCHAR *)key_start;
    node->value = cursor;
    node->children = children;
    node->value_length = value_length;
    node->type = type;
    return TRUE;
}

static BOOL version_key_equals(const WCHAR *key, const WCHAR *segment,
                               SIZE_T segment_length)
{
    for (SIZE_T i = 0; i < segment_length; i++) {
        if (!key[i] || version_ascii_lower(key[i]) !=
                           version_ascii_lower(segment[i]))
            return FALSE;
    }
    return key[segment_length] == 0;
}

static BOOL version_find_child(const VERSION_NODE *parent,
                               const WCHAR *segment, SIZE_T segment_length,
                               VERSION_NODE *result)
{
    const BYTE *cursor = parent->children;
    while ((SIZE_T)(parent->end - cursor) >= 6U) {
        VERSION_NODE child;
        if (!version_parse_node(cursor, parent->end, &child))
            return FALSE;
        if (version_key_equals(child.key, segment, segment_length)) {
            *result = child;
            return TRUE;
        }

        const BYTE *next = version_align4(child.end);
        if (next <= cursor || next > parent->end)
            return FALSE;
        cursor = next;
    }
    return FALSE;
}

static BOOL version_query_native(PCVOID block, PCWSTR sub_block,
                                 PVOID *buffer, UINT *length, BOOL *text)
{
    const BYTE *root_start = (const BYTE *)block;
    WORD root_length = version_read_u16(root_start);
    if (root_length < 6U)
        return FALSE;

    VERSION_NODE current;
    if (!version_parse_node(root_start, root_start + root_length, &current) ||
        !version_wstr_eq_ascii(current.key, "VS_VERSION_INFO"))
        return FALSE;

    const WCHAR *cursor = sub_block;
    while (*cursor == '\\')
        cursor++;

    while (*cursor) {
        const WCHAR *segment = cursor;
        while (*cursor && *cursor != '\\')
            cursor++;
        SIZE_T segment_length = (SIZE_T)(cursor - segment);
        if (segment_length == 0 ||
            !version_find_child(&current, segment, segment_length, &current))
            return FALSE;
        while (*cursor == '\\')
            cursor++;
    }

    *buffer = (PVOID)current.value;
    *length = (UINT)current.value_length;
    if (text) *text = current.type == 1;
    return TRUE;
}

static BOOL version_query_value(PCVOID block, PCWSTR sub_block,
                                PVOID *buffer, UINT *length)
{
    if (version_read_u32((const BYTE *)block) == VS_FFI_SIGNATURE) {
        const VERSION_BLOB *blob = (const VERSION_BLOB *)block;
        if (version_wstr_eq_ascii(sub_block, "\\")) {
            *buffer = (PVOID)&blob->fixed;
            *length = (UINT)sizeof(blob->fixed);
            SetLastError(0);
            return TRUE;
        }
        if (version_wstr_eq_ascii(sub_block,
                                  "\\VarFileInfo\\Translation")) {
            *buffer = (PVOID)blob->translation;
            *length = (UINT)sizeof(blob->translation);
            SetLastError(0);
            return TRUE;
        }
        if (version_query_string(blob, sub_block, buffer, length)) {
            SetLastError(0);
            return TRUE;
        }
    } else if (version_query_native(block, sub_block, buffer, length, NULL)) {
        SetLastError(0);
        return TRUE;
    }

    SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
    return FALSE;
}

BOOL WINAPI shim_VerQueryValueW(PCVOID block, PCWSTR sub_block,
                                PVOID *buffer, UINT *length)
{
    if (buffer) version_store_pointer(buffer, NULL);
    if (length) *length = 0;
    if (!block || !sub_block || !buffer || !length) {
        SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return FALSE;
    }

    PVOID value = NULL;
    UINT value_length = 0;
    if (!version_query_value(block, sub_block, &value, &value_length))
        return FALSE;
    version_store_pointer(buffer, value);
    *length = value_length;
    return TRUE;
}

BOOL WINAPI shim_VerQueryValueA(PCVOID block, PCSTR sub_block,
                                PVOID *buffer, UINT *length)
{
    if (buffer) version_store_pointer(buffer, NULL);
    if (length) *length = 0;
    if (!block || !sub_block || !buffer || !length) {
        SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return FALSE;
    }

    PWSTR wide = version_ansi_to_wide(sub_block);
    if (!wide)
        return FALSE;

    PVOID value = NULL;
    UINT value_length = 0;
    BOOL text = FALSE;
    BOOL result;
    if (version_read_u32(block) == VS_FFI_SIGNATURE)
        result = version_query_value(block, wide, &value, &value_length);
    else {
        result = version_query_native(block, wide, &value, &value_length, &text);
        SetLastError(result ? 0 : ERROR_RESOURCE_DATA_NOT_FOUND);
    }
    if (result && text && value_length) {
        DWORD root = version_read_u16(block);
        DWORD aligned_root = (root + 3) & ~3U;
        SIZE_T offset = (const BYTE *)value - (const BYTE *)block;
        /* Ex buffers reserve a second resource-sized area for ANSI queries.
         * Only use it when the loader's trailer proves that layout. */
        if (offset > root || value_length > (root - offset) / sizeof(WCHAR) ||
            version_read_u32((const BYTE *)block + aligned_root) != 0x58324546U) {
            result = FALSE;
            SetLastError(ERROR_INVALID_PARAMETER);
        } else {
            PSTR ansi = (PSTR)block + aligned_root + 4 + offset;
            int converted = WideCharToMultiByte(0, 0, value, (int)value_length,
                                                 ansi, (int)(root - offset), NULL, NULL);
            if (!converted) result = FALSE;
            else { value = ansi; value_length = (UINT)converted; }
        }
    }
    DWORD error = GetLastError();
    LocalFree(wide);
    if (result) {
        version_store_pointer(buffer, value);
        *length = value_length;
    }
    SetLastError(error);
    return result;
}

extern DWORD WINAPI GetUserDefaultLCID(void);
extern int WINAPI GetLocaleInfoW(DWORD locale, DWORD type, PWSTR data, int chars);

/* Read resources as data, without executing or registering the queried image. */
static BYTE *version_file_resource(PCWSTR filename, const char *type, DWORD *size)
{
    DWORD file_size = 0;
    HANDLE file = version_open_file(filename, &file_size);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    VERSION_RESOURCE_LOCATION location;
    BYTE *data = NULL;
    if (version_locate_resource(file, file_size, type, &location)) {
        data = LocalAlloc(0, location.size);
        if (!data) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        else if (!version_read_at(file, location.file_offset, data, location.size)) {
            LocalFree(data); data = NULL;
        } else *size = location.size;
    } else if (GetLastError() == ERROR_RESOURCE_DATA_NOT_FOUND) SetLastError(1813);
    DWORD error = GetLastError();
    CloseHandle(file); SetLastError(error);
    return data;
}

static BOOL version_mui_header(const BYTE *data, DWORD size, unsigned kind)
{
    return data && size >= 136 && version_read_u32(data) == 0xFECDFECDU &&
           version_read_u32(data + 4) >= 136 && version_read_u32(data + 4) <= size &&
           version_read_u32(data + 8) == 0x10000 &&
           (version_read_u32(data + 16) & 3) == kind;
}

static BOOL version_mui_language(const BYTE *data, DWORD size, unsigned field,
                                  WCHAR language[85])
{
    DWORD offset = version_read_u32(data + field), bytes = version_read_u32(data + field + 4);
    if (offset < 136 || bytes < 4 || bytes > 170 || (bytes & 1) ||
        !version_range_valid(offset, bytes, size)) return FALSE;
    unsigned count = bytes / 2;
    for (unsigned i = 0; i < count; i++) {
        WCHAR ch = version_read_u16(data + offset + i * 2);
        language[i] = ch;
        if (!ch) return i > 0 && i == count - 1;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-')) return FALSE;
    }
    return FALSE;
}

static BYTE *version_mui_resource(PCWSTR filename, PCWSTR language,
                                   const BYTE *neutral, DWORD *size)
{
    SIZE_T length = 0, prefix = 0, lang = 0;
    while (filename[length]) {
        if (length >= 32760) return NULL;
        if (filename[length] == '/' || filename[length] == '\\') prefix = length + 1;
        length++;
    }
    while (language[lang]) { if (lang >= 84) return NULL; lang++; }
    PWSTR path = LocalAlloc(0, (length + lang + 7) * sizeof(WCHAR));
    if (!path) return NULL;
    SIZE_T n = 0;
    for (SIZE_T i = 0; i < prefix; i++) path[n++] = filename[i];
    for (SIZE_T i = 0; i < lang; i++) path[n++] = language[i];
    path[n++] = '/';
    for (SIZE_T i = prefix; i < length; i++) path[n++] = filename[i];
    path[n++] = '.'; path[n++] = 'm'; path[n++] = 'u'; path[n++] = 'i'; path[n] = 0;
    DWORD config_size = 0;
    BYTE *config = version_file_resource(path, "MUI", &config_size), *result = NULL;
    WCHAR declared[85];
    BOOL match = version_mui_header(config, config_size, 2) &&
                 version_mui_language(config, config_size, 116, declared);
    if (match) {
        for (unsigned i = 44; i < 60; i++) if (neutral[i] != config[i]) match = FALSE;
        for (SIZE_T i = 0; i <= lang; i++)
            if (version_ascii_lower(declared[i]) != version_ascii_lower(language[i])) { match = FALSE; break; }
    }
    if (match) result = version_file_resource(path, NULL, size);
    if (config) LocalFree(config);
    LocalFree(path);
    return result;
}

static BYTE *version_selected_resource(DWORD flags, PCWSTR filename, DWORD *size)
{
    if (!filename || !*filename) { SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND); return NULL; }
    char requested[260], physical[260];
    WCHAR resolved[261];
    if (!WideCharToMultiByte(65001, 0, filename, -1, requested,
                             sizeof(requested), NULL, NULL)) return NULL;
    if (!dll_resolve_data_path(requested, physical)) return NULL;
    /* A leading separator keeps the resolved volume-relative path independent
     * of the current directory, including for its sibling MUI resources. */
    resolved[0] = '\\';
    if (!MultiByteToWideChar(65001, 0, physical, -1, resolved + 1, 260)) return NULL;
    filename = resolved;
    DWORD neutral_size = 0;
    BYTE *neutral = version_file_resource(filename, NULL, &neutral_size);
    DWORD error = GetLastError();
    if (flags & 1) {
        DWORD config_size = 0;
        BYTE *config = version_file_resource(filename, "MUI", &config_size);
        if (version_mui_header(config, config_size, 1)) {
            WCHAR language[85], fallback[85];
            BYTE *localized = NULL;
            DWORD localized_size = 0;
            if (GetLocaleInfoW(GetUserDefaultLCID(), 0x5c, language, 85) > 1)
                localized = version_mui_resource(filename, language, config, &localized_size);
            if (!localized && version_mui_language(config, config_size, 124, fallback))
                localized = version_mui_resource(filename, fallback, config, &localized_size);
            if (localized) {
                /* NEUTRAL selects the main file's fixed information, while
                 * LOCALIZED alone selects the complete language resource. */
                if ((flags & 2) && neutral && neutral_size >= 92 && localized_size >= 92 &&
                    version_read_u32(neutral + 40) == VS_FFI_SIGNATURE &&
                    version_read_u32(localized + 40) == VS_FFI_SIGNATURE)
                    version_copy(localized + 40, neutral + 40, 52);
                if (neutral) LocalFree(neutral);
                neutral = localized; neutral_size = localized_size;
            }
        }
        if (config) LocalFree(config);
    }
    if (!neutral) { SetLastError(error); return NULL; }
    DWORD root = neutral_size >= 6 ? version_read_u16(neutral) : 0;
    if (root < 6 || root > neutral_size) {
        LocalFree(neutral); SetLastError(13); return NULL;
    }
    *size = root;
    SetLastError(0);
    return neutral;
}

DWORD WINAPI shim_GetFileVersionInfoSizeExW(DWORD flags, PCWSTR filename, DWORD *handle)
{
    if (handle) *handle = 0;
    DWORD size = 0;
    BYTE *data = version_selected_resource(flags, filename, &size);
    if (!data) return 0;
    LocalFree(data);
    SetLastError(0);
    return ((size + 3) & ~3U) * 2 + 4;
}

BOOL WINAPI shim_GetFileVersionInfoExW(DWORD flags, PCWSTR filename, DWORD handle,
                                       DWORD length, PVOID output)
{
    (void)handle;
    if (length < 72) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    DWORD size = 0;
    BYTE *data = version_selected_resource(flags, filename, &size);
    if (!data) return FALSE;
    if (length < 92) {
        LocalFree(data); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE;
    }
    if (!output || size < 6) {
        LocalFree(data); SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    DWORD copied = ((length - 4) / 2) & ~3U;
    if (copied > size) copied = size;
    version_copy(output, data, copied);
    if (copied < version_read_u16(data)) *(WORD *)output = (WORD)copied;
    BYTE *trailer = (BYTE *)output + ((copied + 3) & ~3U);
    trailer[0] = 'F'; trailer[1] = 'E'; trailer[2] = '2'; trailer[3] = 'X';
    if ((flags & 3) == 3) {
        /* Native combined-mode retrieval overlays the fixed record after
         * truncation, even when it overlaps the ANSI conversion trailer. */
        if (length < 96 || size < 92) {
            LocalFree(data); SetLastError(13); return FALSE;
        }
        version_copy((BYTE *)output + 40, data + 40, 52);
    }
    LocalFree(data);
    SetLastError(0);
    return TRUE;
}

DWORD WINAPI shim_GetFileVersionInfoSizeExA(DWORD flags, PCSTR filename, DWORD *handle)
{
    if (handle) *handle = 0;
    if (!filename) { SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND); return 0; }
    PWSTR wide = version_ansi_to_wide(filename);
    if (!wide) return 0;
    DWORD result = shim_GetFileVersionInfoSizeExW(flags, wide, handle), error = GetLastError();
    LocalFree(wide); SetLastError(error); return result;
}

BOOL WINAPI shim_GetFileVersionInfoExA(DWORD flags, PCSTR filename, DWORD handle,
                                       DWORD length, PVOID data)
{
    if (length < 72) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    if (!filename) { SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND); return FALSE; }
    PWSTR wide = version_ansi_to_wide(filename);
    if (!wide) return FALSE;
    BOOL result = shim_GetFileVersionInfoExW(flags, wide, handle, length, data);
    DWORD error = GetLastError(); LocalFree(wide); SetLastError(error); return result;
}

static const WIN32_EXPORT version_exports[] = {
    { "GetFileVersionInfoSizeExA", (PVOID)shim_GetFileVersionInfoSizeExA, 3, CC_STDCALL },
    { "GetFileVersionInfoSizeExW", (PVOID)shim_GetFileVersionInfoSizeExW, 3, CC_STDCALL },
    { "GetFileVersionInfoExA", (PVOID)shim_GetFileVersionInfoExA, 5, CC_STDCALL },
    { "GetFileVersionInfoExW", (PVOID)shim_GetFileVersionInfoExW, 5, CC_STDCALL },
    { "GetFileVersionInfoSizeA", (PVOID)shim_GetFileVersionInfoSizeA,
      2, CC_STDCALL },
    { "GetFileVersionInfoSizeW", (PVOID)shim_GetFileVersionInfoSizeW,
      2, CC_STDCALL },
    { "GetFileVersionInfoA", (PVOID)shim_GetFileVersionInfoA,
      4, CC_STDCALL },
    { "GetFileVersionInfoW", (PVOID)shim_GetFileVersionInfoW,
      4, CC_STDCALL },
    { "VerQueryValueA", (PVOID)shim_VerQueryValueA, 4, CC_STDCALL },
    { "VerQueryValueW", (PVOID)shim_VerQueryValueW, 4, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *version_abi_table(int *count)
{
    *count = (int)(sizeof(version_exports) / sizeof(version_exports[0]));
    return version_exports;
}

static int version_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID version_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal) return NULL;
    for (int i = 0; version_exports[i].name; i++) {
        if (version_strcmp(func_name, version_exports[i].name) == 0)
            return version_exports[i].func;
    }
    return NULL;
}

PVOID version_shim_init(void)
{
    return (PVOID)version_exports;
}
