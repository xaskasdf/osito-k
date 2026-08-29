/* version.dll support backed by native PE RT_VERSION resources. */

#include "version_shim.h"
#include "kernel32_shim.h"
#include "pe.h"
#include "win32_abi.h"

extern void serial_puts(const char *s);

#define ERROR_RESOURCE_DATA_NOT_FOUND 1812
#define ERROR_INSUFFICIENT_BUFFER      122
#define ERROR_BAD_EXE_FORMAT           193

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
        (10U << 16),
        (19045U << 16),
        (10U << 16),
        (19045U << 16),
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
    HANDLE file = CreateFileW(filename, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              NULL);
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
                                        DWORD desired_id, BOOL first,
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
        if (first || (!(entry.Name & 0x80000000U) &&
                      (entry.Name & 0xFFFFU) == desired_id)) {
            *result = entry;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL version_locate_resource(HANDLE file, DWORD file_size,
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
                                     VERSION_RESOURCE_TYPE, FALSE,
                                     &type_entry) ||
        !(type_entry.OffsetToData & 0x80000000U) ||
        !version_read_resource_entry(
            file, resource_base, resource_size,
            type_entry.OffsetToData & 0x7FFFFFFFU, 0, TRUE, &name_entry) ||
        !(name_entry.OffsetToData & 0x80000000U) ||
        !version_read_resource_entry(
            file, resource_base, resource_size,
            name_entry.OffsetToData & 0x7FFFFFFFU, 0, TRUE,
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
    if (version_is_kernel_module(filename)) {
        SetLastError(0);
        serial_puts("[VERSION] kernel version 10.0.19045.0\n");
        return (DWORD)sizeof(kernel_version);
    }

    DWORD file_size = 0;
    HANDLE file = version_open_file(filename, &file_size);
    if (file == INVALID_HANDLE_VALUE)
        return 0;

    VERSION_RESOURCE_LOCATION location;
    BOOL found = version_locate_resource(file, file_size, &location);
    DWORD error = GetLastError();
    CloseHandle(file);
    if (!found) {
        SetLastError(error);
        serial_puts("[VERSION] PE has no usable RT_VERSION resource\n");
        return 0;
    }

    SetLastError(0);
    serial_puts("[VERSION] PE RT_VERSION resource found\n");
    return location.size;
}

BOOL WINAPI shim_GetFileVersionInfoW(PCWSTR filename, DWORD handle,
                                     DWORD length, PVOID data)
{
    (void)handle;
    if (!version_is_kernel_module(filename)) {
        DWORD file_size = 0;
        HANDLE file = version_open_file(filename, &file_size);
        if (file == INVALID_HANDLE_VALUE)
            return FALSE;

        VERSION_RESOURCE_LOCATION location;
        BOOL found = version_locate_resource(file, file_size, &location);
        DWORD error = GetLastError();
        if (!found) {
            CloseHandle(file);
            SetLastError(error);
            return FALSE;
        }
        if (!data || length < location.size) {
            CloseHandle(file);
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }

        BOOL read = version_read_at(file, location.file_offset, data,
                                    location.size);
        error = GetLastError();
        CloseHandle(file);
        if (!read) {
            SetLastError(error);
            return FALSE;
        }
        SetLastError(0);
        return TRUE;
    }
    if (!data || length < sizeof(kernel_version)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    version_copy(data, &kernel_version, sizeof(kernel_version));
    SetLastError(0);
    return TRUE;
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
                                 PVOID *buffer, UINT *length)
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
    return TRUE;
}

BOOL WINAPI shim_VerQueryValueW(PCVOID block, PCWSTR sub_block,
                                PVOID *buffer, UINT *length)
{
    if (buffer) *buffer = NULL;
    if (length) *length = 0;

    if (!block || !sub_block || !buffer || !length) {
        SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return FALSE;
    }

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
    } else if (version_query_native(block, sub_block, buffer, length)) {
        SetLastError(0);
        return TRUE;
    }

    SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
    return FALSE;
}

static const WIN32_EXPORT version_exports[] = {
    { "GetFileVersionInfoSizeW", (PVOID)shim_GetFileVersionInfoSizeW,
      2, CC_STDCALL },
    { "GetFileVersionInfoW", (PVOID)shim_GetFileVersionInfoW,
      4, CC_STDCALL },
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
