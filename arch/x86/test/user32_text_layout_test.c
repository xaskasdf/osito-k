/* Production formatter with fixed-metric GDI mocks, for allocation/bounds QA.
 * Rendering and Windows compatibility are checked by gdi_text_pe.c instead. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../win32/user32_shim.h"
#include "../win32/gdi32_shim.h"
#include "../win32/kernel32_shim.h"

static unsigned allocations, fail_every, allocation_calls, draw_calls;
static DWORD background;

void *kmalloc(uint64_t bytes)
{
    if (fail_every && ++allocation_calls % fail_every == 0) return NULL;
    void *pointer = malloc((size_t)bytes);
    if (pointer) allocations++;
    return pointer;
}
void kfree(void *pointer)
{
    if (pointer) { assert(allocations); allocations--; free(pointer); }
}
void WINAPI SetLastError(DWORD error) { (void)error; }
BOOL WINAPI GetTextMetricsW(HDC dc, PVOID output)
{
    if (!dc || !output) return FALSE;
    *(GDI_TEXTMETRICW *)output = (GDI_TEXTMETRICW){
        .height=16, .ascent=12, .descent=4, .average_width=8, .maximum_width=8};
    return TRUE;
}
BOOL WINAPI GetTextExtentPoint32W(HDC dc, PCWSTR text, int count, PVOID output)
{
    if (!dc || !output || count < 0 || (count && !text)) return FALSE;
    assert(count <= INT_MAX / 8);
    ((LONG *)output)[0] = count * 8;
    ((LONG *)output)[1] = count ? 16 : 0;
    return TRUE;
}
DWORD WINAPI GetTextColor(HDC dc) { assert(dc); return 0x112233; }
DWORD WINAPI SetBkColor(HDC dc, DWORD color)
{
    assert(dc);
    DWORD old = background;
    background = color;
    return old;
}
BOOL WINAPI ExtTextOutW(HDC dc, int x, int y, UINT flags,
                          const GDI_RECT *rect, PCWSTR text, UINT count,
                          const int *spacing)
{
    (void)x; (void)y; (void)flags; (void)rect;
    assert(dc && count <= 8192);
    if (count) assert(text && spacing);
    draw_calls++;
    return TRUE;
}
int WINAPI MultiByteToWideChar(DWORD cp, DWORD flags, PCSTR text, int count,
                                 PWSTR output, int capacity)
{
    (void)cp; (void)flags;
    if (count == -1) count = (int)strlen(text) + 1;
    if (output) {
        if (capacity < count) return 0;
        for (int i = 0; i < count; i++) output[i] = (BYTE)text[i];
    }
    return count;
}
int WINAPI WideCharToMultiByte(DWORD cp, DWORD flags, PCWSTR text, int count,
                                 PSTR output, int capacity, PCSTR fallback,
                                 BOOL *used_fallback)
{
    (void)cp; (void)flags; (void)fallback; (void)used_fallback;
    if (count == -1) { count = 0; while (text[count++]); }
    if (output) {
        if (capacity < count) return 0;
        for (int i = 0; i < count; i++) output[i] = (char)text[i];
    }
    return count;
}

static unsigned random_value(void)
{
    static unsigned state = 0x7342B;
    state = state * 1664525u + 1013904223u;
    return state;
}

int main(void)
{
    static const char alphabet[] = "ABCxyz& \t\r\n\\.";
    static const UINT options[] = {
        0, 0x20, 0x10, 0x400, 0x820, 0x40, 0x2000, 0x25, 0x2A,
        0x100020, 0x200020, 0x8000, 0x4000, 0x40000, 0x18020,
        0x14020, 0x50020, 0x18000, 0x14000, 0x50000, 0xE0, 0x100
    };
    const RECT edges[] = {
        {0,0,320,160}, {-20,-10,1,3}, {INT_MIN,INT_MIN,INT_MAX,INT_MAX},
        {INT_MAX-2,INT_MAX-4,INT_MAX,INT_MAX}, {0,0,0,0}, {4,8,-4,-8}
    };
    for (unsigned iteration = 0; iteration < 20000; iteration++) {
        unsigned length = random_value() % 96;
        char *ansi = malloc(length + 4), *original = malloc(length + 1);
        WCHAR *wide = malloc((length + 4) * sizeof(WCHAR));
        assert(ansi && original && wide);
        for (unsigned i = 0; i < length; i++)
            wide[i] = ansi[i] = original[i] = alphabet[random_value() % (sizeof(alphabet)-1)];
        wide[length] = ansi[length] = original[length] = 0;
        UINT flags = options[random_value() % (sizeof(options)/sizeof(options[0]))];
        RECT rect = edges[random_value() % (sizeof(edges)/sizeof(edges[0]))];
        struct { UINT size; int tab, left, right; UINT drawn; } params = {
            20, (int)(random_value() % 10), (int)(random_value() % 16),
            (int)(random_value() % 16), 0};
        fail_every = iteration % 3 ? 0 : 7;
        int count = iteration & 1 ? -1 : (int)length;
        if (iteration & 2) DrawTextExA((HDC)1, ansi, count, &rect, flags, &params);
        else DrawTextExW((HDC)1, wide, count, &rect, flags, &params);
        assert(params.drawn <= length);
        assert(allocations == 0 && background == 0);
        if (!(flags & 0x10000)) {
            assert(memcmp(ansi, original, length+1) == 0);
            for (unsigned i = 0; i <= length; i++) assert(wide[i] == (BYTE)original[i]);
        }
        free(ansi); free(original); free(wide);
    }
    printf("[USER32-TEXT-LAYOUT] 20000 cases PASS, %u draw calls, allocations balanced\n", draw_calls);
    return 0;
}
