/* PE certificate-table editing. Certificates remain opaque WIN_CERTIFICATE
 * records; trust/signature verification belongs to WinVerifyTrust. */
#include "imagehlp_shim.h"
#include "kernel32_shim.h"
#include "ntdll_shim.h"
#include "handle.h"
#include "compat32.h"

extern HANDLE_TABLE g_handle_table;
extern DWORD win32_current_process_id(void);

typedef struct {
    HANDLE handle;
    ULONGLONG size;
    LONGLONG position;
    DWORD checksum, directory, certificate, certificate_size;
} image_file_t;

static DWORD image_u32(const BYTE *p)
{
    return (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24;
}

static void image_put_u32(BYTE *p, DWORD value)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (BYTE)(value >> (8 * i));
}

static BOOL image_seek(image_file_t *file, LONGLONG position)
{
    FILE_POSITION_INFORMATION info;
    IO_STATUS_BLOCK io;
    info.CurrentByteOffset.QuadPart = position;
    return NT_SUCCESS(NtSetInformationFile(file->handle, &io, &info,
        sizeof(info), FilePositionInformation));
}

static BOOL image_read(image_file_t *file, ULONGLONG offset, PVOID buffer, DWORD bytes)
{
    if (offset > file->size || bytes > file->size - offset) return FALSE;
    LARGE_INTEGER at;
    IO_STATUS_BLOCK io;
    at.QuadPart = (LONGLONG)offset;
    return !bytes || (NT_SUCCESS(NtReadFile(file->handle, NULL, NULL, NULL,
        &io, buffer, bytes, &at, NULL)) && io.Information == bytes);
}

static BOOL image_write(image_file_t *file, ULONGLONG offset, PCVOID buffer, DWORD bytes)
{
    LARGE_INTEGER at;
    IO_STATUS_BLOCK io;
    at.QuadPart = (LONGLONG)offset;
    return !bytes || (NT_SUCCESS(NtWriteFile(file->handle, NULL, NULL, NULL,
        &io, (PVOID)buffer, bytes, &at, NULL)) && io.Information == bytes);
}

static BOOL image_resize(image_file_t *file, ULONGLONG size)
{
    LARGE_INTEGER value;
    IO_STATUS_BLOCK io;
    value.QuadPart = (LONGLONG)size;
    if (!NT_SUCCESS(NtSetInformationFile(file->handle, &io, &value,
        sizeof(value), FileEndOfFileInformation))) return FALSE;
    file->size = size;
    return TRUE;
}

static BOOL image_open(HANDLE handle, BOOL write, image_file_t *file)
{
    HANDLE_OBJECT_SNAPSHOT snapshot;
    DWORD owner = win32_current_process_id();
    ACCESS_MASK required = FILE_READ_DATA | (write ? FILE_WRITE_DATA : 0);
    if (!owner) owner = 1;
    if (!NT_SUCCESS(handle_snapshot_for_process(&g_handle_table, handle,
            owner, &snapshot)) || snapshot.type != OBJ_TYPE_FILE ||
        (snapshot.access & required) != required ||
        !(snapshot.file_flags & FILE_OBJ_DISK_FILE) ||
        (snapshot.file_flags & FILE_OBJ_DIRECTORY)) return FALSE;
    FILE_STANDARD_INFORMATION standard;
    FILE_POSITION_INFORMATION position;
    IO_STATUS_BLOCK io;
    if (!NT_SUCCESS(NtQueryInformationFile(handle, &io, &standard,
            sizeof(standard), FileStandardInformation)) ||
        standard.EndOfFile.QuadPart < 0 || standard.EndOfFile.QuadPart > 0xffffffffLL ||
        !NT_SUCCESS(NtQueryInformationFile(handle, &io, &position,
            sizeof(position), FilePositionInformation))) return FALSE;
    *file = (image_file_t){.handle=handle, .size=(ULONGLONG)standard.EndOfFile.QuadPart,
                          .position=position.CurrentByteOffset.QuadPart};
    return TRUE;
}

static BOOL image_header(image_file_t *file)
{
    BYTE dos[64], nt[24], optional[240];
    if (!image_read(file, 0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z') return FALSE;
    DWORD pe = image_u32(dos + 60);
    if (pe > 0x7fffffffU || !image_read(file, pe, nt, sizeof(nt)) ||
        image_u32(nt) != 0x4550 || !image_read(file, (ULONGLONG)pe + 24, optional, 2)) return FALSE;
    unsigned magic = optional[0] | (unsigned)optional[1] << 8;
    unsigned bytes = magic == 0x10b ? 224 : magic == 0x20b ? 240 : 0;
    if (!bytes || !image_read(file, (ULONGLONG)pe + 24, optional, bytes)) return FALSE;
    unsigned security = magic == 0x10b ? 128 : 144;
    file->checksum = pe + 24 + 64;
    file->directory = pe + 24 + security;
    file->certificate = image_u32(optional + security);
    file->certificate_size = image_u32(optional + security + 4);
    return TRUE;
}

static BOOL image_table_range(const image_file_t *file)
{
    return file->certificate <= file->size &&
           file->certificate_size <= file->size - file->certificate &&
           (!file->certificate || file->certificate >= file->directory + 8);
}

static BOOL image_finish(image_file_t *file, DWORD error, LONGLONG position)
{
    if (!image_seek(file, position) && !error) error = 87;
    SetLastError(error);
    return error == 0;
}

static BOOL image_rechecksum(image_file_t *file)
{
    BYTE buffer[4096];
    DWORD sum = 0;
    for (ULONGLONG at = 0; at < file->size;) {
        DWORD count = (DWORD)(file->size - at > sizeof(buffer) ? sizeof(buffer) : file->size - at);
        if (!image_read(file, at, buffer, count)) return FALSE;
        for (DWORD i = 0; i < count; i += 2) {
            DWORD low = at + i >= file->checksum && at + i < file->checksum + 4 ? 0 : buffer[i];
            DWORD high = i + 1 < count ? buffer[i + 1] : 0;
            if (at + i + 1 >= file->checksum && at + i + 1 < file->checksum + 4) high = 0;
            DWORD word = low | high << 8;
            sum = (sum & 0xffffU) + (sum >> 16) + word;
        }
        at += count;
    }
    sum = (sum & 0xffffU) + (sum >> 16);
    sum = (sum & 0xffffU) + (sum >> 16);
    image_put_u32(buffer, sum + (DWORD)file->size);
    return image_write(file, file->checksum, buffer, 4);
}

static BOOL image_directory(image_file_t *file, DWORD offset, DWORD size)
{
    BYTE entry[8];
    image_put_u32(entry, offset);image_put_u32(entry + 4, size);
    if (!image_write(file, file->directory, entry, sizeof(entry))) return FALSE;
    file->certificate = offset;file->certificate_size = size;
    return TRUE;
}

static BOOL image_move(image_file_t *file, ULONGLONG from, ULONGLONG to, ULONGLONG bytes)
{
    BYTE buffer[4096];
    BOOL backwards = to > from;
    ULONGLONG done = 0;
    while (done < bytes) {
        DWORD count = (DWORD)(bytes - done > sizeof(buffer) ? sizeof(buffer) : bytes - done);
        ULONGLONG at = backwards ? bytes - done - count : done;
        if (!image_read(file, from + at, buffer, count) ||
            !image_write(file, to + at, buffer, count)) return FALSE;
        done += count;
    }
    return TRUE;
}

BOOL WINAPI imagehlp_ImageEnumerateCertificates(HANDLE handle, WORD filter,
                                                DWORD *count, DWORD *indices,
                                                DWORD capacity)
{
    image_file_t file;
    if (!count || filter > 255 || !win32_user_range_writable(count, 4, g_compat32_mode) ||
        (indices && capacity && !win32_user_range_writable(indices, (ULONGLONG)capacity * 4, g_compat32_mode)) ||
        !image_open(handle, FALSE, &file)) { SetLastError(87);return FALSE; }
    if (!image_header(&file)) return image_finish(&file, 87, file.position);
    if (file.certificate && file.certificate_size >= 8 &&
        (ULONGLONG)file.certificate + file.certificate_size < file.size)
        return image_finish(&file, 193, file.position);
    *count = 0;
    if (!image_table_range(&file)) return image_finish(&file, 87, file.position);
    if (!file.certificate || file.certificate_size < 8) return image_finish(&file, 0, file.position);
    DWORD ordinal = 0;
    ULONGLONG at = 0;
    while (at + 8 <= file.certificate_size) {
        BYTE header[8];
        if (!image_read(&file, file.certificate + at, header, sizeof(header)))
            return image_finish(&file, 87, file.position);
        DWORD length = image_u32(header);
        if (!length) return image_finish(&file, 87, file.position);
        WORD type = header[6] | (WORD)header[7] << 8;
        if (filter == 255 || filter == type) {
            if (indices && *count < capacity) indices[*count] = ordinal;
            (*count)++;
        }
        at += ((ULONGLONG)length + 7) & ~7ULL;
        ordinal++;
    }
    return image_finish(&file, 0, file.position);
}

BOOL WINAPI imagehlp_ImageAddCertificate(HANDLE handle, PVOID certificate, DWORD *index)
{
    image_file_t file;
    if (!index || !win32_user_range_writable(index, 4, g_compat32_mode) ||
        !image_open(handle, TRUE, &file)) { SetLastError(87);return FALSE; }
    if (!image_header(&file)) return image_finish(&file, 87, file.position);
    *index = 0xffffffffU;
    if (!certificate || !win32_user_range_readable(certificate, 4, g_compat32_mode) ||
        !image_table_range(&file)) return image_finish(&file, 87, file.position);
    DWORD length = image_u32(certificate);
    ULONGLONG padded = ((ULONGLONG)length + 7) & ~7ULL;
    if (padded > 0xffffffffU ||
        (length && !win32_user_range_readable(certificate, length, g_compat32_mode)))
        return image_finish(&file, 87, file.position);
    *index = 0;
    for (ULONGLONG at = 0; file.certificate && at < file.certificate_size;) {
        BYTE header[8];
        if (!image_read(&file, file.certificate + at, header, sizeof(header)))
            return image_finish(&file, 87, file.position);
        DWORD item = image_u32(header);
        if (!item) return image_finish(&file, 87, file.position);
        at += ((ULONGLONG)item + 7) & ~7ULL;
        (*index)++;
    }
    ULONGLONG start = file.certificate ? file.certificate : (file.size + 7) & ~7ULL;
    ULONGLONG old_size = file.size;
    ULONGLONG insertion = start + file.certificate_size;
    ULONGLONG new_size = ((file.size > start ? file.size : start) + padded + 7) & ~7ULL;
    if ((start & 7) || new_size > 0xffffffffU ||
        file.certificate_size > 0xffffffffU - padded || insertion > new_size)
        return image_finish(&file, 87, file.position);
    if (!image_resize(&file, new_size)) return image_finish(&file, 87, file.position);
    if (file.certificate && insertion < old_size &&
        !image_move(&file, insertion, insertion + padded, old_size - insertion))
        return image_finish(&file, 87, file.position);
    if (!image_write(&file, insertion, certificate, length)) return image_finish(&file, 87, file.position);
    static const BYTE zero[8] = {0};
    if (!image_write(&file, insertion + length, zero, (DWORD)(padded - length)) ||
        !image_directory(&file, (DWORD)start, file.certificate_size + (DWORD)padded) ||
        !image_rechecksum(&file)) return image_finish(&file, 87, file.position);
    return image_finish(&file, 0, length || old_size != new_size ? (LONGLONG)new_size : file.position);
}

BOOL WINAPI imagehlp_ImageRemoveCertificate(HANDLE handle, DWORD index)
{
    image_file_t file;
    if (!image_open(handle, TRUE, &file)) { SetLastError(87);return FALSE; }
    if (!image_header(&file) || !image_table_range(&file) || !file.certificate ||
        (ULONGLONG)file.certificate + file.certificate_size != file.size)
        return image_finish(&file, 87, file.position);
    ULONGLONG at = 0;
    DWORD ordinal = 0;
    while (at + 8 < file.certificate_size) {
        BYTE header[8];
        if (!image_read(&file, file.certificate + at, header, sizeof(header)))
            return image_finish(&file, 87, file.position);
        ULONGLONG length = ((ULONGLONG)image_u32(header) + 7) & ~7ULL;
        if (ordinal == index) {
            if (!length) return image_finish(&file, 0, file.position);
            if (length > file.certificate_size - at) return image_finish(&file, 87, file.position);
            DWORD new_cert_size = file.certificate_size - (DWORD)length;
            if (!image_move(&file, file.certificate + at + length, file.certificate + at,
                    file.certificate_size - at - length) ||
                !image_resize(&file, file.size - length) ||
                !image_directory(&file, new_cert_size ? file.certificate : 0, new_cert_size) ||
                !image_rechecksum(&file)) return image_finish(&file, 87, file.position);
            return image_finish(&file, 0, (LONGLONG)file.size);
        }
        if (!length) break;
        at += length;ordinal++;
    }
    return image_finish(&file, 87, file.position);
}

static const WIN32_EXPORT imagehlp_exports[] = {
    {"ImageAddCertificate",(PVOID)imagehlp_ImageAddCertificate,3,CC_STDCALL},
    {"ImageEnumerateCertificates",(PVOID)imagehlp_ImageEnumerateCertificates,5,CC_STDCALL},
    {"ImageRemoveCertificate",(PVOID)imagehlp_ImageRemoveCertificate,2,CC_STDCALL},
};

const WIN32_EXPORT *imagehlp_abi_table(int *count)
{
    *count = sizeof(imagehlp_exports) / sizeof(imagehlp_exports[0]);
    return imagehlp_exports;
}

PVOID imagehlp_resolve(const char *name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (!name || by_ordinal) return NULL;
    for (unsigned i = 0; i < sizeof(imagehlp_exports) / sizeof(imagehlp_exports[0]); i++) {
        const char *a = name, *b = imagehlp_exports[i].name;
        while (*a && *a == *b) { a++;b++; }
        if (!*a && !*b) return imagehlp_exports[i].func;
    }
    return NULL;
}
