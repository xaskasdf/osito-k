/* PE32 contract test for the kernel32 private-profile API. */

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef void *PVOID;
typedef PVOID HANDLE;

#define WINAPI __attribute__((stdcall))
#define DLLIMPORT __declspec(dllimport)

#define GENERIC_WRITE 0x40000000UL
#define FILE_SHARE_READ 1U
#define FILE_SHARE_WRITE 2U
#define CREATE_ALWAYS 2U
#define FILE_ATTRIBUTE_NORMAL 0x80U
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
DLLIMPORT DWORD WINAPI GetPrivateProfileStringA(
    const char *section, const char *key, const char *fallback,
    char *buffer, DWORD size, const char *file);
DLLIMPORT DWORD WINAPI GetPrivateProfileStringW(
    const WORD *section, const WORD *key, const WORD *fallback,
    WORD *buffer, DWORD size, const WORD *file);
DLLIMPORT BOOL WINAPI WritePrivateProfileStringA(
    const char *section, const char *key, const char *value,
    const char *file);
DLLIMPORT BOOL WINAPI WritePrivateProfileStringW(
    const WORD *section, const WORD *key, const WORD *value,
    const WORD *file);
DLLIMPORT UINT WINAPI GetPrivateProfileIntA(
    const char *section, const char *key, int fallback, const char *file);
DLLIMPORT DWORD WINAPI GetPrivateProfileSectionNamesA(
    char *buffer, DWORD size, const char *file);
DLLIMPORT DWORD WINAPI GetPrivateProfileSectionA(
    const char *section, char *buffer, DWORD size, const char *file);
DLLIMPORT BOOL WINAPI WritePrivateProfileSectionA(
    const char *section, const char *strings, const char *file);

void *memset(void *destination, int value, unsigned int size)
{
    BYTE *bytes = (BYTE *)destination;
    for (unsigned int i = 0; i < size; i++) bytes[i] = (BYTE)value;
    return destination;
}

static DWORD text_length(const char *text)
{
    DWORD length = 0;
    while (text[length]) length++;
    return length;
}

static BOOL text_equal(const char *left, const char *right)
{
    while (*left && *right) {
        if (*left++ != *right++) return 0;
    }
    return *left == *right;
}

static BOOL bytes_equal(const char *left, const char *right, DWORD count)
{
    for (DWORD i = 0; i < count; i++) {
        if (left[i] != right[i]) return 0;
    }
    return 1;
}

static void print(const char *text)
{
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, text_length(text),
              &written, (PVOID)0);
}

static void fail(UINT code)
{
    char message[] = "PROFILE TEST FAIL 00\r\n";
    message[18] = (char)('0' + (code / 10));
    message[19] = (char)('0' + (code % 10));
    print(message);
    ExitProcess(code);
    for (;;) { }
}

static BOOL reset_profile(const char *file)
{
    HANDLE handle = CreateFileA(file, GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                (PVOID)0, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, (HANDLE)0);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    BOOL closed = CloseHandle(handle);
    BOOL flushed = WritePrivateProfileStringA(
        (const char *)0, (const char *)0, (const char *)0, file);
    return closed && flushed;
}

void mainCRTStartup(void)
{
    static const char file_a[] = "profile-a.ini";
    static const char file_b[] = "profile-b.ini";
    static const char alpha_values[] =
        "Key=one\0Dup=first\0Dup=second\0Number=42\0\0";
    static const char expected_sections[] = "Alpha\0Beta\0\0";
    static const char expected_keys[] = "Key\0Dup\0Dup\0Number\0\0";
    static const char expected_section[] =
        "Key=one\0Dup=first\0Dup=second\0Number=42\0\0";
    static const WORD wide_section[] = {'W','i','d','e',0};
    static const WORD wide_key[] = {'K','e','y',0};
    static const WORD wide_value[] = {'v','a','l','u','e',0};
    static const WORD wide_file[] = {
        'p','r','o','f','i','l','e','-','b','.','i','n','i',0
    };
    char buffer[128] = {0};
    WORD wide_buffer[32] = {0};

    if (!reset_profile(file_a) || !reset_profile(file_b)) fail(18);

    if (!WritePrivateProfileSectionA("Alpha", alpha_values, file_a))
        fail(1);
    if (!WritePrivateProfileStringA("Beta", "Other", "two", file_a))
        fail(2);
    if (!WritePrivateProfileStringA("Alpha", "Key", "separate", file_b))
        fail(3);

    if (GetPrivateProfileStringA("Alpha", "Key", "bad", buffer,
                                 sizeof(buffer), file_a) != 3 ||
        !text_equal(buffer, "one"))
        fail(4);
    if (GetPrivateProfileStringA("Alpha", "Key", "bad", buffer,
                                 sizeof(buffer), file_b) != 8 ||
        !text_equal(buffer, "separate"))
        fail(5);
    if (GetPrivateProfileIntA("Alpha", "Number", -1, file_a) != 42U)
        fail(6);

    DWORD length = GetPrivateProfileSectionNamesA(buffer, sizeof(buffer),
                                                   file_a);
    if (length != sizeof(expected_sections) - 2 ||
        !bytes_equal(buffer, expected_sections,
                     sizeof(expected_sections) - 1))
        fail(7);
    length = GetPrivateProfileStringA("Alpha", (const char *)0, "", buffer,
                                      sizeof(buffer), file_a);
    if (length != sizeof(expected_keys) - 2 ||
        !bytes_equal(buffer, expected_keys, sizeof(expected_keys) - 1))
        fail(8);
    length = GetPrivateProfileSectionA("Alpha", buffer, sizeof(buffer),
                                       file_a);
    if (length != sizeof(expected_section) - 2 ||
        !bytes_equal(buffer, expected_section,
                     sizeof(expected_section) - 1))
        fail(9);

    for (DWORD i = 0; i < sizeof(buffer); i++) buffer[i] = (char)0x55;
    length = GetPrivateProfileSectionNamesA(buffer, 8, file_a);
    if (length != 6 || buffer[6] != 0 || buffer[7] != 0)
        fail(10);

    if (!WritePrivateProfileStringA("Alpha", "Dup", (const char *)0,
                                    file_a) ||
        GetPrivateProfileStringA("Alpha", "Dup", "bad", buffer,
                                 sizeof(buffer), file_a) != 6 ||
        !text_equal(buffer, "second"))
        fail(11);
    if (!WritePrivateProfileStringA("Beta", (const char *)0,
                                    (const char *)0, file_a))
        fail(12);
    if (!WritePrivateProfileStringA((const char *)0, (const char *)0,
                                    (const char *)0, file_a) ||
        GetPrivateProfileStringA("Alpha", "Dup", "bad", buffer,
                                 sizeof(buffer), file_a) != 6 ||
        !text_equal(buffer, "second"))
        fail(13);

    if (!WritePrivateProfileStringW(wide_section, wide_key, wide_value,
                                    wide_file) ||
        GetPrivateProfileStringW(wide_section, wide_key, (const WORD *)0,
                                 wide_buffer, 32, wide_file) != 5 ||
        wide_buffer[0] != 'v' || wide_buffer[4] != 'e' || wide_buffer[5])
        fail(14);

    static const char external[] =
        "[External]\r\nValue=changed\r\n[Empty]\r\n";
    HANDLE direct = CreateFileA(file_a, GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                (PVOID)0, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, (HANDLE)0);
    DWORD written = 0;
    if (direct == INVALID_HANDLE_VALUE ||
        !WriteFile(direct, external, sizeof(external) - 1, &written,
                   (PVOID)0) || written != sizeof(external) - 1 ||
        !CloseHandle(direct))
        fail(15);
    if (GetPrivateProfileStringA("External", "Value", "bad", buffer,
                                 sizeof(buffer), file_a) != 7 ||
        !text_equal(buffer, "changed"))
        fail(16);
    length = GetPrivateProfileSectionNamesA(buffer, sizeof(buffer), file_a);
    static const char external_sections[] = "External\0Empty\0\0";
    if (length != sizeof(external_sections) - 2 ||
        !bytes_equal(buffer, external_sections,
                     sizeof(external_sections) - 1))
        fail(17);

    print("PROFILE TEST PASS\r\n");
    ExitProcess(0);
}
