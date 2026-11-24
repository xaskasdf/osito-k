/* PE32 contract test for persistent Win32 file timestamps. */

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef unsigned long long ULONGLONG;
typedef int BOOL;
typedef void *PVOID;
typedef PVOID HANDLE;

typedef struct {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;

typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION;

typedef struct {
    ULONGLONG VolumeSerialNumber;
    BYTE FileId[16];
} FILE_ID_INFO;

#define WINAPI __attribute__((stdcall))
#define DLLIMPORT __declspec(dllimport)

#define GENERIC_READ 0x80000000UL
#define GENERIC_WRITE 0x40000000UL
#define FILE_WRITE_ATTRIBUTES 0x00000100UL
#define FILE_SHARE_READ 1U
#define FILE_SHARE_WRITE 2U
#define CREATE_ALWAYS 2U
#define OPEN_EXISTING 3U
#define FILE_ATTRIBUTE_NORMAL 0x80U
#define GET_FILEEX_INFO_STANDARD 0U
#define ERROR_ACCESS_DENIED 5U
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

DLLIMPORT void WINAPI ExitProcess(UINT code);
DLLIMPORT HANDLE WINAPI GetStdHandle(DWORD which);
DLLIMPORT HANDLE WINAPI CreateFileA(const char *name, DWORD access,
                                    DWORD share, PVOID security,
                                    DWORD disposition, DWORD attributes,
                                    HANDLE template_file);
DLLIMPORT BOOL WINAPI WriteFile(HANDLE file, const void *buffer, DWORD size,
                                DWORD *written, PVOID overlapped);
DLLIMPORT BOOL WINAPI CloseHandle(HANDLE handle);
DLLIMPORT BOOL WINAPI DeleteFileA(const char *name);
DLLIMPORT BOOL WINAPI MoveFileA(const char *old_name, const char *new_name);
DLLIMPORT DWORD WINAPI GetLastError(void);
DLLIMPORT BOOL WINAPI GetFileTime(HANDLE file, FILETIME *creation,
                                  FILETIME *access, FILETIME *write);
DLLIMPORT BOOL WINAPI SetFileTime(HANDLE file, const FILETIME *creation,
                                  const FILETIME *access,
                                  const FILETIME *write);
DLLIMPORT BOOL WINAPI GetFileAttributesExW(
    const WORD *name, DWORD info_level, WIN32_FILE_ATTRIBUTE_DATA *data);
DLLIMPORT BOOL WINAPI GetFileAttributesExA(
    const char *name, DWORD info_level, WIN32_FILE_ATTRIBUTE_DATA *data);
DLLIMPORT BOOL WINAPI GetFileInformationByHandle(
    HANDLE file, BY_HANDLE_FILE_INFORMATION *information);
DLLIMPORT BOOL WINAPI GetFileInformationByHandleEx(
    HANDLE file, DWORD info_class, PVOID information, DWORD information_size);
DLLIMPORT BOOL WINAPI GetVolumeInformationA(
    const char *root_path, char *volume_name, DWORD volume_name_size,
    DWORD *volume_serial, DWORD *maximum_component_length,
    DWORD *filesystem_flags, char *filesystem_name,
    DWORD filesystem_name_size);

static DWORD text_length(const char *text)
{
    DWORD length = 0;
    while (text[length]) length++;
    return length;
}

static void print(const char *text)
{
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, text_length(text),
              &written, (PVOID)0);
}

static void fail(UINT code)
{
    char message[] = "FILE TIME TEST FAIL 00\r\n";
    message[20] = (char)('0' + (code / 10));
    message[21] = (char)('0' + (code % 10));
    print(message);
    ExitProcess(code);
    for (;;) { }
}

static FILETIME filetime_from_unix(DWORD seconds)
{
    const ULONGLONG epoch = 11644473600ULL;
    ULONGLONG value = (epoch + seconds) * 10000000ULL;
    FILETIME result;
    result.dwLowDateTime = (DWORD)value;
    result.dwHighDateTime = (DWORD)(value >> 32);
    return result;
}

static BOOL filetime_equal(const FILETIME *left, const FILETIME *right)
{
    return left->dwLowDateTime == right->dwLowDateTime &&
           left->dwHighDateTime == right->dwHighDateTime;
}

static ULONGLONG file_id_low(const FILE_ID_INFO *information)
{
    ULONGLONG value = 0;
    for (UINT i = 0; i < 8; i++)
        value |= (ULONGLONG)information->FileId[i] << (i * 8);
    return value;
}

void mainCRTStartup(void)
{
    static const char file_name[] = "file-time-contract.tmp";
    static const char renamed_file_name[] = "file-time-renamed.tmp";
    static const WORD wide_file_name[] = {
        'f','i','l','e','-','t','i','m','e','-','c','o','n','t','r','a','c','t',
        '.','t','m','p',0
    };
    FILETIME creation = filetime_from_unix(1704067200U);
    FILETIME access = filetime_from_unix(1704153600U);
    FILETIME write = filetime_from_unix(1704240000U);
    FILETIME actual_creation;
    FILETIME actual_access;
    FILETIME actual_write;

    DeleteFileA(file_name);
    DeleteFileA(renamed_file_name);
    HANDLE file = CreateFileA(
        file_name, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, (PVOID)0, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, (HANDLE)0);
    if (file == INVALID_HANDLE_VALUE) fail(1);

    if (!SetFileTime(file, &creation, &access, &write)) fail(2);
    if (!GetFileTime(file, &actual_creation, &actual_access, &actual_write))
        fail(3);
    if (!filetime_equal(&actual_creation, &creation) ||
        !filetime_equal(&actual_access, &access) ||
        !filetime_equal(&actual_write, &write))
        fail(4);

    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW(wide_file_name, GET_FILEEX_INFO_STANDARD,
                              &attributes))
        fail(5);
    if (!filetime_equal(&attributes.ftCreationTime, &creation) ||
        !filetime_equal(&attributes.ftLastAccessTime, &access) ||
        !filetime_equal(&attributes.ftLastWriteTime, &write) ||
        attributes.nFileSizeHigh != 0 || attributes.nFileSizeLow != 0)
        fail(6);

    if (!GetFileAttributesExA(file_name, GET_FILEEX_INFO_STANDARD,
                              &attributes))
        fail(24);
    if (!filetime_equal(&attributes.ftCreationTime, &creation) ||
        !filetime_equal(&attributes.ftLastAccessTime, &access) ||
        !filetime_equal(&attributes.ftLastWriteTime, &write) ||
        attributes.nFileSizeHigh != 0 || attributes.nFileSizeLow != 0)
        fail(25);

    FILETIME suppress = {0xFFFFFFFFUL, 0xFFFFFFFFUL};
    if (!SetFileTime(file, (const FILETIME *)0, (const FILETIME *)0,
                     &suppress))
        fail(7);
    BYTE byte = 0x5A;
    DWORD written = 0;
    if (!WriteFile(file, &byte, 1, &written, (PVOID)0) || written != 1)
        fail(8);
    if (!GetFileTime(file, (FILETIME *)0, (FILETIME *)0, &actual_write))
        fail(9);
    if (!filetime_equal(&actual_write, &write)) fail(10);

    FILETIME restore = {0xFFFFFFFEUL, 0xFFFFFFFFUL};
    if (!SetFileTime(file, (const FILETIME *)0, (const FILETIME *)0,
                     &restore))
        fail(11);
    if (!WriteFile(file, &byte, 1, &written, (PVOID)0) || written != 1)
        fail(12);
    if (!GetFileTime(file, (FILETIME *)0, (FILETIME *)0, &actual_write))
        fail(13);
    if (filetime_equal(&actual_write, &write)) fail(14);

    if (!CloseHandle(file)) fail(15);
    file = CreateFileA(file_name, FILE_WRITE_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, (PVOID)0,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, (HANDLE)0);
    if (file == INVALID_HANDLE_VALUE) fail(16);
    if (!SetFileTime(file, &creation, &access, &write)) fail(17);
    if (!CloseHandle(file)) fail(18);

    file = CreateFileA(file_name, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, (PVOID)0,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, (HANDLE)0);
    if (file == INVALID_HANDLE_VALUE) fail(19);

    BY_HANDLE_FILE_INFORMATION before;
    FILE_ID_INFO before_id;
    if (!GetFileInformationByHandle(file, &before)) fail(26);
    if (!GetFileInformationByHandleEx(file, 18 /* FileIdInfo */, &before_id,
                                      sizeof(before_id)))
        fail(27);
    ULONGLONG stable_id = ((ULONGLONG)before.nFileIndexHigh << 32) |
                          before.nFileIndexLow;
    if (!stable_id || stable_id != file_id_low(&before_id) ||
        before.dwVolumeSerialNumber != (DWORD)before_id.VolumeSerialNumber)
        fail(28);

    char volume_name[32];
    char filesystem_name[16];
    DWORD volume_serial = 0;
    DWORD maximum_component_length = 0;
    DWORD filesystem_flags = 0;
    if (!GetVolumeInformationA((const char *)0, volume_name,
                               sizeof(volume_name), &volume_serial,
                               &maximum_component_length, &filesystem_flags,
                               filesystem_name, sizeof(filesystem_name)))
        fail(35);
    if (!volume_serial || volume_serial != before.dwVolumeSerialNumber ||
        volume_serial != (DWORD)before_id.VolumeSerialNumber)
        fail(36);
    if (!CloseHandle(file)) fail(29);
    if (!MoveFileA(file_name, renamed_file_name)) fail(30);

    file = CreateFileA(renamed_file_name, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, (PVOID)0,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, (HANDLE)0);
    if (file == INVALID_HANDLE_VALUE) fail(31);
    BY_HANDLE_FILE_INFORMATION after;
    FILE_ID_INFO after_id;
    if (!GetFileInformationByHandle(file, &after)) fail(32);
    if (!GetFileInformationByHandleEx(file, 18 /* FileIdInfo */, &after_id,
                                      sizeof(after_id)))
        fail(33);
    ULONGLONG reopened_id = ((ULONGLONG)after.nFileIndexHigh << 32) |
                            after.nFileIndexLow;
    if (reopened_id != stable_id || file_id_low(&after_id) != stable_id)
        fail(34);

    if (SetFileTime(file, &creation, &access, &write)) fail(20);
    if (GetLastError() != ERROR_ACCESS_DENIED) fail(21);
    if (!CloseHandle(file)) fail(22);
    if (!DeleteFileA(renamed_file_name)) fail(23);

    print("FILE TIME TEST PASS\r\n");
    ExitProcess(0);
}
