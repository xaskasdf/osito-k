#include <stdint.h>
#include <stddef.h>
__declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, unsigned long, unsigned long *, void *);
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) unsigned long __stdcall GetTempPathA(unsigned long, char *);
__declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
__declspec(dllimport) int __stdcall CreateDirectoryA(const char *, void *);
__declspec(dllimport) int __stdcall DeleteFileA(const char *);
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned);
#define LOAD(var, name) var = (void *)GetProcAddress(crt, name); if (!var) ++failures
#define CHECK(x) do { if (!(x)) ++failures; } while (0)

void crt_stdio_read_entry(void)
{
    unsigned failures = 0;
    void *crt = LoadLibraryA("msvcrt.dll");
    void *(__cdecl *open_file)(const char *, const char *);
    int (__cdecl *close_file)(void *), (__cdecl *eof)(void *), (__cdecl *error)(void *);
    int (__cdecl *put)(int, void *), (__cdecl *push)(int, void *);
    int (__cdecl *seek)(void *, long, int);
    size_t (__cdecl *read_file)(void *, size_t, size_t, void *);
    void (__cdecl *clear)(void *);
    void *(__cdecl *iob)(void);
    int (__cdecl *fileno)(void *);
    int (__cdecl *puts_file)(const char *, void *);
    LOAD(open_file, "fopen"); LOAD(close_file, "fclose"); LOAD(eof, "feof");
    LOAD(error, "ferror"); LOAD(put, "putc"); LOAD(read_file, "fread");
    LOAD(seek, "fseek"); LOAD(push, "ungetc"); LOAD(clear, "clearerr");
    LOAD(iob, "__iob_func"); LOAD(fileno, "_fileno"); LOAD(puts_file, "fputs");
    char path[300];
    unsigned n = GetTempPathA(260, path);
    if (!n || n >= 260) ++failures;
    if (!failures) {
        unsigned stride = sizeof(void *) == 4 ? 32 : 48;
        unsigned flag_offset = sizeof(void *) == 4 ? 12 : 24;
        unsigned char *standard = iob();
        CHECK(standard);
        if (standard) {
            for (unsigned i = 0; i < 3; ++i) CHECK(fileno(standard + i * stride) == (int)i);
            CHECK(puts_file("CRT STDOUT ALIVE\n", standard + stride) >= 0);
            CHECK(puts_file("CRT STDERR ALIVE\n", standard + 2 * stride) >= 0);
        }
        CreateDirectoryA(path, 0);
        unsigned long pid = GetCurrentProcessId();
        const char *stem = "osito-crt-read-";
        while (*stem) path[n++] = *stem++;
        for (int shift = 28; shift >= 0; shift -= 4)
            path[n++] = "0123456789abcdef"[(pid >> shift) & 15];
        path[n] = 0;
        void *file = open_file(path, "wb");
        CHECK(file);
        if (file) {
            for (int i = 0; i < 10; ++i) CHECK(put('a' + i, file) == 'a' + i);
            CHECK(!close_file(file));
        }
        file = open_file(path, "rb");
        CHECK(file);
        if (file) {
            unsigned char bytes[32];
            CHECK(read_file(bytes, 1, 10, file) == 10 && !eof(file) && !error(file));
            for (int i = 0; i < 10; ++i) CHECK(bytes[i] == 'a' + i);
            CHECK(read_file(bytes, 1, 1, file) == 0 && eof(file) && !error(file));
            CHECK(!seek(file, 0, 0) && !eof(file));
            CHECK(read_file(bytes, 1, 32, file) == 10 && eof(file) && !error(file));
            CHECK(*(int *)((unsigned char *)file + flag_offset) & 0x10);
            CHECK(!seek(file, 0, 0));
            CHECK(read_file(bytes, 3, 4, file) == 3 && eof(file) && !error(file));
            clear(file);
            CHECK(!eof(file) && !error(file));
            CHECK(push('Z', file) == 'Z');
            CHECK(read_file(bytes, 1, 2, file) == 1 && bytes[0] == 'Z' && eof(file));
            CHECK(read_file(bytes, 0, 3, file) == 0);
            CHECK(!close_file(file));
        }
        DeleteFileA(path);
    }
    const char *text = failures ? "CRT READ FAIL\r\n" : "CRT READ PASS\r\n";
    unsigned long written;
    WriteFile(GetStdHandle((unsigned long)-11), text, 15, &written, 0);
    ExitProcess(failures ? 1 : 0);
}
