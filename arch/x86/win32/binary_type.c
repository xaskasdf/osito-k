/* Executable classification without loading code, imports, or dependencies. */
#include "binary_type.h"
#include "kernel32_shim.h"
#include "compat32.h"

static uint16_t binary_u16(const BYTE *p)
{ return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1]<<8); }
static uint32_t binary_u32(const BYTE *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static int binary_power_two(uint32_t n) { return n && !(n & (n-1)); }
static uint64_t binary_align(uint64_t value, uint32_t alignment)
{ return (value+alignment-1) & ~((uint64_t)alignment-1); }

static DWORD binary_read(HANDLE file, uint64_t offset, BYTE *buffer,
                          DWORD capacity, DWORD *received)
{
    LONG high = (LONG)(offset>>32);
    SetLastError(0);
    DWORD low = SetFilePointer(file, (LONG)offset, &high, 0);
    if (low == 0xffffffffU && GetLastError()) return GetLastError();
    memset(buffer, 0, capacity); *received = 0;
    while (*received < capacity) {
        DWORD count = 0;
        if (!ReadFile(file, buffer+*received, capacity-*received, &count, NULL))
            return GetLastError() ? GetLastError() : 30;
        if (!count) break;
        *received += count;
    }
    return 0;
}

static int binary_suffix(PCWSTR path, DWORD length, const char *suffix)
{
    if (length < 4 || path[length-4] != '.') return 0;
    for (unsigned i = 0; i < 3; i++) {
        WCHAR c = path[length-3+i];
        if (c >= 'A' && c <= 'Z') c += 'a'-'A';
        if (c != (WCHAR)suffix[i]) return 0;
    }
    return 1;
}

static DWORD binary_pe(HANDLE file, uint32_t offset, const BYTE header[264],
                        DWORD received, DWORD *type)
{
    uint16_t machine = binary_u16(header+4), sections = binary_u16(header+6);
    uint16_t optional_size = binary_u16(header+20), characteristics = binary_u16(header+22);
    const BYTE *optional = header+24;
    uint16_t magic = binary_u16(optional), subsystem = binary_u16(optional+68);
    if (!(characteristics & 2) || (characteristics & 0x2000) || !sections || sections > 96)
        return 193;
    if (!((machine == 0x14c && magic == 0x10b) || (machine == 0x8664 && magic == 0x20b)))
        return 193;
    if (received < (magic == 0x20b ? 264U : 248U) || optional_size < (magic == 0x20b ? 112U : 96U))
        return 193;
    uint32_t section_align = binary_u32(optional+32), file_align = binary_u32(optional+36);
    uint32_t image_size = binary_u32(optional+56), headers_size = binary_u32(optional+60);
    if (!binary_power_two(section_align) || !binary_power_two(file_align) || file_align > section_align ||
        (section_align < 4096 ? file_align != section_align : file_align < 512 || file_align > 65536) ||
        !image_size || image_size > 0x7fffffffU || !headers_size || headers_size > image_size)
        return 193;
    uint64_t end = binary_align(headers_size, section_align);
    uint64_t table = (uint64_t)offset+24+optional_size;
    DWORD file_high = 0;
    SetLastError(0);
    DWORD file_low = GetFileSize(file, &file_high);
    if (file_low == 0xffffffffU && GetLastError()) return GetLastError();
    uint64_t file_size = file_low | (uint64_t)file_high<<32;
    for (unsigned i = 0; i < sections; i++) {
        BYTE section[40]; DWORD bytes = 0;
        DWORD error = binary_read(file, table+(uint64_t)i*40, section, sizeof(section), &bytes);
        if (error) return error;
        /* Windows can classify a truncated image when its meaningful header
         * fields are present; the rest of the header page reads as zero. */
        uint32_t virtual_size = binary_u32(section+8), address = binary_u32(section+12);
        uint32_t raw_size = binary_u32(section+16), raw_offset = binary_u32(section+20);
        if (i+1 == sections && (uint64_t)raw_offset+raw_size > file_size) return 193;
        if (address % section_align || address < end ||
            (section_align < 4096 && raw_size && raw_offset != address)) return 193;
        uint64_t extent = virtual_size ? virtual_size : raw_size;
        end = binary_align((uint64_t)address+extent, section_align);
        if (end > image_size) return 193;
    }
    /* WOW64 classifies a PE64 image before considering its subsystem. */
    *type = magic == 0x20b && g_compat32_mode ? 6 : subsystem == 7 ? 4 : magic == 0x20b ? 6 : 0;
    return 0;
}

BOOL WINAPI GetBinaryTypeW(PCWSTR path, DWORD *type)
{
    DWORD saved = GetLastError(), length = 0, result = 0, error = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    if (!path) { error = 123; goto done; }
    BOOL compat = g_compat32_mode != 0;
    for (;;) {
        if (!win32_user_range_readable(path+length, 2, compat)) { error = 87; goto done; }
        if (!path[length]) break;
        if (++length == 32767) { error = 206; goto done; }
    }
    if (!length) { error = 123; goto done; }
    if (!type || !win32_user_range_writable(type, 4, compat)) { error = 87; goto done; }
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == 0xffffffffU) { error = GetLastError() ? GetLastError() : 2; goto done; }
    if (attributes & 0x10) { error = 5; goto done; }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                       NULL, 3, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) { error = GetLastError() ? GetLastError() : 5; goto done; }
    if (GetFileType(file) != 1) { error = 5; goto done; }
    BYTE dos[64], header[264]; DWORD received = 0;
    error = binary_read(file, 0, dos, sizeof(dos), &received);
    if (error) goto done;
    if (!received) { error = 193; goto done; }
    if (received >= 2 && binary_u16(dos) == 0x5a4d) {
        result = 1;
        if (received < sizeof(dos)) goto done;
        uint32_t offset = binary_u32(dos+60);
        if (offset < sizeof(dos) || offset > 0x7fffffffU) goto done;
        error = binary_read(file, offset, header, sizeof(header), &received);
        if (error) goto done;
        if (received >= 64 && binary_u16(header) == 0x454e && header[0x36] == 2) result = 2;
        if (received >= 248 && binary_u32(header) == 0x4550) {
            if (binary_u16(header+24) == 0x20b && received < 264) goto done;
            error = binary_pe(file, offset, header, received, &result);
        }
    } else if (binary_suffix(path, length, "exe") || binary_suffix(path, length, "com")) result = 1;
    else if (binary_suffix(path, length, "pif")) result = 3;
    else error = 193;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (error) { SetLastError(error); return FALSE; }
    *type = result; SetLastError(saved); return TRUE;
}
