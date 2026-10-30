/* Run the unchanged PE probe against production GDI, using the existing
 * surface test's kernel adapters. The Windows PE output is the oracle. */
#define GDI_HOST_USER_MEMORY
#define main surface_lifetime_main
#include "gdi_surface_lifetime_host.c"
#undef main
#undef CHECK

extern long write(int, const void *, unsigned long);
extern void exit(int) __attribute__((noreturn));
static char *host_command;
DWORD WINAPI GetLastError(void) { return host_last_error; }
HANDLE WINAPI LoadLibraryA(PCSTR name) { (void)name; return (HANDLE)1; }
PVOID WINAPI GetProcAddress(HANDLE module, PCSTR name)
{
    (void)module;
    return gdi32_resolve(name, 0, FALSE);
}
HANDLE WINAPI GetStdHandle(DWORD which) { (void)which; return (HANDLE)1; }
BOOL WINAPI WriteFile(HANDLE file, PCVOID data, DWORD size, DWORD *done, PVOID overlap)
{
    (void)file; (void)overlap;
    long count = write(1, data, size);
    if (done) *done = count > 0 ? (DWORD)count : 0;
    return count == size;
}
PCSTR WINAPI GetCommandLineA(void) { return host_command; }
void WINAPI ExitProcess(UINT code) { exit(code); }

#define EXTPEN_HOST
#ifdef GDI_HOST_ARC
#include "arc_pe.c"
#define gdi_test_entry arc_entry
#elif defined(GDI_HOST_POLYGON)
#include "polygon_draw_pe.c"
#define gdi_test_entry polygon_draw_entry
#else
#include "extpen_pe.c"
#define gdi_test_entry extpen_entry
#endif

int main(int argc, char **argv)
{
    host_command = argc > 1 ? argv[1] : "extpen";
    gdi_test_entry();
    return 1;
}
