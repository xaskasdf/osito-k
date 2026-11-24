#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#define sched_yield startup_test_sched_yield
#include "../win32/crt_startup.c"
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned checks, kernel_allocations, allocation_step, fail_step;
static int error_value, new_mode, search_index;
static DWORD pid = 5;
int g_compat32_mode;
static const char *command;
static WCHAR wide_command[CRT_ARGS_MAX_UNITS];
static int use_wide;
static uint64_t env_cell = 0x123456;
static struct { void *pointer; size_t bytes; } user_allocations[64];
static const char *names[] = {"zeta.arg", ".", "Beta.arg", "..", "alpha.arg"};

void *kmalloc(uint64_t size)
{
    if (++allocation_step == fail_step) return NULL;
    void *p = malloc(size);
    if (p) ++kernel_allocations;
    return p;
}
void kfree(void *p) { if (p) { CHECK(kernel_allocations); --kernel_allocations; free(p); } }
void sched_yield(void) { CHECK(0); }
DWORD win32_current_process_id(void) { return pid; }
const char *win32_current_command_line(void) { return command; }
const WCHAR *win32_current_command_line_w(void) { return use_wide ? wide_command : NULL; }
int *WINAPI crt_errno(void) { return &error_value; }
int WINAPI _set_new_mode(int value) { int old = new_mode; new_mode = value; return old; }
char ***WINAPI crt_p_environ(void) { return (char ***)&env_cell; }
WCHAR ***WINAPI crt_p_wenviron(void) { return (WCHAR ***)&env_cell; }
PVOID WINAPI crt_malloc(SIZE_T bytes)
{
    if (++allocation_step == fail_step) return NULL;
    size_t mapped = (bytes + 4095) & ~(size_t)4095;
    void *p = mmap(NULL, mapped, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | (g_compat32_mode ? MAP_32BIT : 0), -1, 0);
    if (p == MAP_FAILED) return NULL;
    for (unsigned i = 0; i < 64; ++i) if (!user_allocations[i].pointer) {
        user_allocations[i].pointer = p; user_allocations[i].bytes = mapped;
        return p;
    }
    CHECK(0); return NULL;
}
void WINAPI crt_free(PVOID p)
{
    for (unsigned i = 0; i < 64; ++i) if (user_allocations[i].pointer == p) {
        CHECK(munmap(p, user_allocations[i].bytes) == 0);
        user_allocations[i].pointer = NULL; return;
    }
    CHECK(0);
}
int WINAPI crt_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
int WINAPI crt_wcsicmp(const WCHAR *a, const WCHAR *b)
{
    while (*a || *b) {
        unsigned x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
DWORD WINAPI GetModuleFileNameA(HANDLE h, PSTR text, DWORD capacity)
{
    (void)h; CHECK(capacity > 24); strcpy(text, "C:\\Program Files\\app.exe");
    return strlen(text);
}
DWORD WINAPI GetModuleFileNameW(HANDLE h, PWSTR text, DWORD capacity)
{
    char name[100]; unsigned n = GetModuleFileNameA(h, name, capacity);
    for (unsigned i = 0; i <= n; ++i) text[i] = (uint8_t)name[i];
    return n;
}
BOOL WINAPI FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data)
{
    CHECK(h == (HANDLE)1);
    if (search_index == 5) return FALSE;
    strcpy(data->cFileName, names[search_index++]); return TRUE;
}
BOOL WINAPI FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data)
{
    WIN32_FIND_DATAA a;
    if (!FindNextFileA(h, &a)) return FALSE;
    unsigned i = 0;
    do { data->cFileName[i] = a.cFileName[i]; } while (a.cFileName[i++]);
    return TRUE;
}
HANDLE WINAPI FindFirstFileA(PCSTR name, LPWIN32_FIND_DATAA data)
{
    if (strstr(name, "none")) return INVALID_HANDLE_VALUE;
    search_index = 0; FindNextFileA((HANDLE)1, data); return (HANDLE)1;
}
HANDLE WINAPI FindFirstFileW(PCWSTR name, LPWIN32_FIND_DATAW data)
{
    if (name[0] == 'n') return INVALID_HANDLE_VALUE;
    search_index = 0; FindNextFileW((HANDLE)1, data); return (HANDLE)1;
}
BOOL WINAPI FindClose(HANDLE h) { CHECK(h == (HANDLE)1); return TRUE; }

static void check_arguments(const char *line, unsigned width, int wildcard,
                             const char **expected, unsigned count)
{
    command = line;
    for (unsigned i = 0; i <= strlen(line); ++i) wide_command[i] = (uint8_t)line[i];
    uint64_t argv = 0xcccccccc00000000ULL, env = argv;
    unsigned pointer_bytes = g_compat32_mode ? 4 : 8;
    int argc = -1, startup = 1;
    CHECK(!crt_startup_args(&argc, &argv, &env, wildcard, &startup, width));
    CHECK(argc == (int)count && new_mode == 1);
    if (pointer_bytes == 4) {
        CHECK(argv >> 32 == 0xcccccccc && env >> 32 == 0xcccccccc);
        argv &= UINT32_MAX; env &= UINT32_MAX;
    }
    CHECK(env == env_cell);
    for (unsigned i = 0; i <= count; ++i) {
        uint64_t pointer = 0;
        memcpy(&pointer, (const char *)(uintptr_t)argv + i * pointer_bytes, pointer_bytes);
        if (i == count) { CHECK(!pointer); break; }
        CHECK(pointer);
        for (unsigned j = 0; j <= strlen(expected[i]); ++j)
            CHECK(UNIT((const void *)(uintptr_t)pointer, j, width) == (uint8_t)expected[i][j]);
    }
    uint64_t again = 0;
    CHECK(!crt_startup_args(&argc, &again, &env, wildcard, NULL, width));
    CHECK(again == argv); /* Stable argv storage across repeated startup calls. */
    crt_startup_release_process(pid);
    CHECK(!kernel_allocations && !crt_startup_cache);
    for (unsigned i = 0; i < 64; ++i)
        if (user_allocations[i].pointer) crt_free(user_allocations[i].pointer);
    ++pid;
}

int main(void)
{
    const char *basic[] = {"C:\\Program Files\\p.exe", "first", "a b", "", "second"};
    const char *glob[] = {"p", "dir\\alpha.arg", "dir\\Beta.arg", "dir\\zeta.arg", "*.arg", "none?"};
    const char *empty[] = {"C:\\Program Files\\app.exe"};
    for (unsigned bits = 0; bits < 2; ++bits) {
        g_compat32_mode = bits;
        for (unsigned width = 1; width <= 2; ++width) {
            use_wide = width == 2;
            check_arguments("\"C:\\Program Files\\p.exe\" first \"a b\" \"\" second", width, 0, basic, 5);
            check_arguments("p dir\\*.arg \"*.arg\" none?", width, 1, glob, 6);
            check_arguments("", width, 0, empty, 1);
            /* Fail each allocation in turn: no partial cache survives. */
            command = "p a b"; use_wide = 0;
            for (unsigned failure = 1; failure <= 10; ++failure) {
                fail_step = failure; allocation_step = 0;
                uint64_t argv = 0, env = 0; int argc = 0;
                int result = crt_startup_args(&argc, &argv, &env, 0, NULL, width);
                if (result) CHECK(!crt_startup_cache && !kernel_allocations);
                else crt_startup_release_process(pid);
                for (unsigned i = 0; i < 64; ++i)
                    if (user_allocations[i].pointer) crt_free(user_allocations[i].pointer);
                CHECK(!kernel_allocations);
                ++pid;
            }
            fail_step = 0;
        }
    }
    printf("CRT STARTUP HOST PASS checks=%u\n", checks);
    return 0;
}
