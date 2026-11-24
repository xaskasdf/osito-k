/*
 * OsitoK Windows Compatibility Layer — gdi32.dll Shim Implementation
 *
 * Real GDI Device Contexts and BitBlt for Win32 compat layer.
 * DCs back onto framebuffer (screen) or kmalloc'd pixel buffers (memory).
 */

#include "gdi32_shim.h"
#include "d3dkmt.h"
#include "d3dkmt_registry.h"
#include "compat32.h"
#include "dllloader.h"
#include "kernel32_shim.h"
#include "wow64.h"
#include "opengl32_shim.h"
#include "win32_abi.h"
#include "../fs/vfs.h"
#include "../include/compositor_surface.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern void *kmalloc(uint64_t size);
extern void *krealloc(void *ptr, uint64_t size);
extern void  kfree(void *ptr);
extern const uint8_t gui_font8x16[95][16];

/* Framebuffer access (weak — may not be linked) */
extern uint32_t *fb_get_base(void)   __attribute__((weak));
extern uint32_t  fb_get_width(void)  __attribute__((weak));
extern uint32_t  fb_get_height(void) __attribute__((weak));
extern uint32_t  fb_get_pitch(void)  __attribute__((weak));
extern bool fb_is_redirected(void) __attribute__((weak));
extern void fb_present_rows(uint32_t top, uint32_t bottom) __attribute__((weak));
extern uint32_t *display_get_back_buffer(void) __attribute__((weak));
extern uint32_t  display_get_width(void)  __attribute__((weak));
extern uint32_t  display_get_height(void) __attribute__((weak));
extern uint32_t  display_get_pitch(void) __attribute__((weak));
extern void display_force_refresh(void) __attribute__((weak));
extern HANDLE WINAPI GetDesktopWindow(void) __attribute__((weak));
extern BOOL WINAPI GetWindowRect(HANDLE window, GDI_RECT *rect)
    __attribute__((weak));
extern BOOL WINAPI ClientToScreen(HANDLE window, PVOID point)
    __attribute__((weak));
extern HDC WINAPI GetDCEx(HANDLE window, HANDLE region, DWORD flags)
    __attribute__((weak));
extern int WINAPI ReleaseDC(HANDLE window, HDC dc) __attribute__((weak));
extern BOOL user32_get_window_surface(HANDLE window, void **pixels, int *width,
                                      int *height, int *pitch)
    __attribute__((weak));
extern BOOL user32_get_window_dc_surface(HANDLE window, void **pixels, int *width,
                                         int *height, int *pitch, BOOL whole)
    __attribute__((weak));
extern BOOL user32_acquire_window_dc_surface(HANDLE window, BOOL whole,
                                             compositor_surface_ref_t *surface)
    __attribute__((weak));
extern HGDIOBJ user32_get_dc_visible_region(HANDLE window, DWORD flags,
                                             HGDIOBJ device_clip)
    __attribute__((weak));
extern void compositor_release_surface(compositor_surface_ref_t *surface)
    __attribute__((weak));
extern void user32_mark_window_dirty(HANDLE window) __attribute__((weak));
extern BOOL user32_get_current_display_mode(uint32_t *width, uint32_t *height,
                                             uint32_t *bpp, uint32_t *frequency)
    __attribute__((weak));
extern int ddraw_display_mode_active(void) __attribute__((weak));
extern void ddraw_get_display_mode(uint32_t *width, uint32_t *height,
                                   uint32_t *bpp) __attribute__((weak));

#define GDI_FALLBACK_SCREEN_WIDTH  640
#define GDI_FALLBACK_SCREEN_HEIGHT 480
#define GDI_FALLBACK_SCREEN_BPP    32

/* ── Local helpers ───────────────────────────────────────────── */

static void gdi_memcpy(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--) *d++ = *s++;
}

static void gdi_memmove(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    if (d <= s || d >= s + n) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
}

static void gdi_memset(void *p, int v, SIZE_T n)
{
    BYTE *d = (BYTE *)p;
    while (n--) *d++ = (BYTE)v;
}

#define GDI_FR_PRIVATE  0x10U
#define GDI_FR_NOT_ENUM 0x20U

typedef enum {
    GDI_FONT_RESOURCE_PATH,
    GDI_FONT_RESOURCE_MEMORY
} GDI_FONT_RESOURCE_KIND;

typedef struct _GDI_FONT_RESOURCE {
    struct _GDI_FONT_RESOURCE *next;
    GDI_FONT_RESOURCE_KIND kind;
    DWORD owner_pid;
    DWORD flags;
    DWORD references;
    DWORD face_count;
    char *path;
    BYTE *data;
    DWORD data_size;
} GDI_FONT_RESOURCE;

static GDI_FONT_RESOURCE *gdi_font_resources;
static volatile ULONG gdi_font_resource_lock;
static uint64_t gdi_font_resource_generation = 1;

static BOOL gdi_font_validate_file(const char *path, DWORD *face_count,
                                   DWORD *error);
static BOOL gdi_font_validate_memory(const void *data, DWORD size,
                                     DWORD *face_count);
static LONG dwrite_utf16_path_to_utf8(PCWSTR input, char *output,
                                      UINT capacity);
static void dwrite_release_system_collection_cache(DWORD process_id);
static void gdi_brush_release_process(DWORD process_id);
static void gdi_pen_release_process(DWORD process_id);
static void gdi_dc_release_process(DWORD process_id);
static void gdi_region_release_process(DWORD process_id);
static void gdi_palette_release_process(DWORD process_id);
static void gdi_font_release_process(DWORD process_id);

static void gdi_font_resources_lock(void)
{
    while (__atomic_exchange_n(&gdi_font_resource_lock, 1,
                               __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void gdi_font_resources_unlock(void)
{
    __atomic_store_n(&gdi_font_resource_lock, 0, __ATOMIC_RELEASE);
}

static DWORD gdi_font_resource_owner(DWORD flags)
{
    if (!(flags & GDI_FR_PRIVATE)) return 0;
    DWORD owner = GetCurrentProcessId();
    return owner ? owner : 1;
}

static char gdi_font_path_fold(char value)
{
    if (value == '/') return '\\';
    if (value >= 'A' && value <= 'Z') return value + ('a' - 'A');
    return value;
}

static BOOL gdi_font_path_equal(const char *left, const char *right)
{
    if (!left || !right) return FALSE;
    while (*left && *right &&
           gdi_font_path_fold(*left) == gdi_font_path_fold(*right)) {
        left++;
        right++;
    }
    return !*left && !*right;
}

static char *gdi_font_copy_path(const char *path)
{
    SIZE_T length = 0;
    while (path[length]) length++;
    char *copy = (char *)kmalloc(length + 1);
    if (!copy) return NULL;
    gdi_memcpy(copy, path, length + 1);
    return copy;
}

static BOOL gdi_font_normalize_path(PCSTR path, char normalized[260])
{
    if (!path || !*path || !win32_resolve_file_path(path, normalized) ||
        !normalized[0]) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    return TRUE;
}

static int gdi_font_resource_add_path(PCSTR path, DWORD flags, PVOID reserved)
{
    if (reserved || (flags & ~(GDI_FR_PRIVATE | GDI_FR_NOT_ENUM))) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    char normalized[260];
    if (!gdi_font_normalize_path(path, normalized)) return 0;

    DWORD faces = 0;
    DWORD error = 0;
    if (!gdi_font_validate_file(normalized, &faces, &error)) {
        SetLastError(error ? error : 11); /* ERROR_BAD_FORMAT */
        return 0;
    }

    DWORD owner = gdi_font_resource_owner(flags);
    gdi_font_resources_lock();
    for (GDI_FONT_RESOURCE *entry = gdi_font_resources; entry;
         entry = entry->next) {
        if (entry->kind != GDI_FONT_RESOURCE_PATH ||
            entry->owner_pid != owner || entry->flags != flags ||
            !gdi_font_path_equal(entry->path, normalized))
            continue;
        if (entry->references == UINT32_MAX) {
            gdi_font_resources_unlock();
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return 0;
        }
        entry->references++;
        DWORD existing_faces = entry->face_count;
        gdi_font_resources_unlock();
        SetLastError(0);
        return (int)existing_faces;
    }
    gdi_font_resources_unlock();

    GDI_FONT_RESOURCE *resource =
        (GDI_FONT_RESOURCE *)kmalloc(sizeof(*resource));
    char *stored_path = gdi_font_copy_path(normalized);
    if (!resource || !stored_path) {
        kfree(resource);
        kfree(stored_path);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    gdi_memset(resource, 0, sizeof(*resource));
    resource->kind = GDI_FONT_RESOURCE_PATH;
    resource->owner_pid = owner;
    resource->flags = flags;
    resource->references = 1;
    resource->face_count = faces;
    resource->path = stored_path;

    gdi_font_resources_lock();
    for (GDI_FONT_RESOURCE *entry = gdi_font_resources; entry;
         entry = entry->next) {
        if (entry->kind != GDI_FONT_RESOURCE_PATH ||
            entry->owner_pid != owner || entry->flags != flags ||
            !gdi_font_path_equal(entry->path, normalized))
            continue;
        if (entry->references == UINT32_MAX) {
            gdi_font_resources_unlock();
            kfree(resource->path);
            kfree(resource);
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return 0;
        }
        entry->references++;
        DWORD existing_faces = entry->face_count;
        gdi_font_resources_unlock();
        kfree(resource->path);
        kfree(resource);
        SetLastError(0);
        return (int)existing_faces;
    }
    resource->next = gdi_font_resources;
    gdi_font_resources = resource;
    gdi_font_resource_generation++;
    gdi_font_resources_unlock();
    SetLastError(0);
    return (int)faces;
}

static BOOL gdi_font_resource_remove_path(PCSTR path, DWORD flags,
                                          PVOID reserved)
{
    if (reserved || (flags & ~(GDI_FR_PRIVATE | GDI_FR_NOT_ENUM))) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    char normalized[260];
    if (!gdi_font_normalize_path(path, normalized)) return FALSE;
    DWORD owner = gdi_font_resource_owner(flags);

    GDI_FONT_RESOURCE *removed = NULL;
    gdi_font_resources_lock();
    GDI_FONT_RESOURCE **link = &gdi_font_resources;
    while (*link) {
        GDI_FONT_RESOURCE *entry = *link;
        if (entry->kind == GDI_FONT_RESOURCE_PATH &&
            entry->owner_pid == owner && entry->flags == flags &&
            gdi_font_path_equal(entry->path, normalized)) {
            if (--entry->references == 0) {
                *link = entry->next;
                removed = entry;
                gdi_font_resource_generation++;
            }
            gdi_font_resources_unlock();
            if (removed) {
                kfree(removed->path);
                kfree(removed);
            }
            SetLastError(0);
            return TRUE;
        }
        link = &entry->next;
    }
    gdi_font_resources_unlock();
    SetLastError(2); /* ERROR_FILE_NOT_FOUND */
    return FALSE;
}

static HANDLE gdi_font_resource_add_memory(PVOID data, DWORD size,
                                           PVOID reserved,
                                           DWORD *font_count)
{
    if (font_count) *font_count = 0;
    if (reserved || !data || !size || !font_count) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }

    DWORD faces = 0;
    if (!gdi_font_validate_memory(data, size, &faces)) {
        SetLastError(11); /* ERROR_BAD_FORMAT */
        return NULL;
    }

    GDI_FONT_RESOURCE *resource =
        (GDI_FONT_RESOURCE *)kmalloc(sizeof(*resource));
    BYTE *copy = (BYTE *)kmalloc(size);
    if (!resource || !copy) {
        kfree(resource);
        kfree(copy);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }
    gdi_memcpy(copy, data, size);
    gdi_memset(resource, 0, sizeof(*resource));
    resource->kind = GDI_FONT_RESOURCE_MEMORY;
    resource->owner_pid = GetCurrentProcessId();
    if (!resource->owner_pid) resource->owner_pid = 1;
    resource->references = 1;
    resource->face_count = faces;
    resource->data = copy;
    resource->data_size = size;

    gdi_font_resources_lock();
    resource->next = gdi_font_resources;
    gdi_font_resources = resource;
    gdi_font_resources_unlock();

    *font_count = faces;
    SetLastError(0);
    return (HANDLE)resource;
}

static BOOL gdi_font_resource_remove_memory(HANDLE handle)
{
    if (!handle) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    GDI_FONT_RESOURCE *removed = NULL;
    DWORD owner = GetCurrentProcessId();
    if (!owner) owner = 1;
    gdi_font_resources_lock();
    GDI_FONT_RESOURCE **link = &gdi_font_resources;
    while (*link) {
        GDI_FONT_RESOURCE *entry = *link;
        if (entry == (GDI_FONT_RESOURCE *)handle &&
            entry->kind == GDI_FONT_RESOURCE_MEMORY &&
            entry->owner_pid == owner) {
            *link = entry->next;
            removed = entry;
            break;
        }
        link = &entry->next;
    }
    gdi_font_resources_unlock();
    if (!removed) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    kfree(removed->data);
    kfree(removed);
    SetLastError(0);
    return TRUE;
}

static void gdi_font_resource_free_path_snapshot(char **paths, UINT count)
{
    if (!paths) return;
    for (UINT i = 0; i < count; i++) kfree(paths[i]);
    kfree(paths);
}

static uint64_t gdi_font_resource_current_generation(void)
{
    gdi_font_resources_lock();
    uint64_t generation = gdi_font_resource_generation;
    gdi_font_resources_unlock();
    return generation;
}

static BOOL gdi_font_resource_snapshot_paths(DWORD process_id,
                                             char ***paths_out,
                                             UINT *count_out,
                                             uint64_t *generation_out)
{
    if (!paths_out || !count_out || !generation_out) return FALSE;
    *paths_out = NULL;
    *count_out = 0;
    if (!process_id) process_id = 1;

    gdi_font_resources_lock();
    UINT count = 0;
    for (GDI_FONT_RESOURCE *entry = gdi_font_resources; entry;
         entry = entry->next) {
        if (entry->kind == GDI_FONT_RESOURCE_PATH &&
            !(entry->flags & GDI_FR_NOT_ENUM) &&
            (!entry->owner_pid || entry->owner_pid == process_id))
            count++;
    }

    char **paths = count
        ? (char **)kmalloc((uint64_t)count * sizeof(*paths)) : NULL;
    UINT copied = 0;
    BOOL complete = !count || paths != NULL;
    for (GDI_FONT_RESOURCE *entry = gdi_font_resources;
         complete && entry; entry = entry->next) {
        if (entry->kind != GDI_FONT_RESOURCE_PATH ||
            (entry->flags & GDI_FR_NOT_ENUM) ||
            (entry->owner_pid && entry->owner_pid != process_id))
            continue;
        if (!(paths[copied] = gdi_font_copy_path(entry->path))) {
            complete = FALSE;
            break;
        }
        copied++;
    }
    *generation_out = gdi_font_resource_generation;
    gdi_font_resources_unlock();

    if (!complete) {
        gdi_font_resource_free_path_snapshot(paths, copied);
        return FALSE;
    }
    *paths_out = paths;
    *count_out = count;
    return TRUE;
}

void gdi32_release_process(DWORD process_id)
{
    d3dkmt_release_process(process_id);
    gdi_dc_release_process(process_id);
    gdi_region_release_process(process_id);
    gdi_brush_release_process(process_id);
    gdi_palette_release_process(process_id);
    gdi_font_release_process(process_id);
    if (!process_id) return;
    GDI_FONT_RESOURCE *retired = NULL;
    BOOL catalog_changed = FALSE;

    gdi_font_resources_lock();
    GDI_FONT_RESOURCE **link = &gdi_font_resources;
    while (*link) {
        GDI_FONT_RESOURCE *entry = *link;
        if (entry->owner_pid != process_id) {
            link = &entry->next;
            continue;
        }
        *link = entry->next;
        if (entry->kind == GDI_FONT_RESOURCE_PATH)
            catalog_changed = TRUE;
        entry->next = retired;
        retired = entry;
    }
    if (catalog_changed) gdi_font_resource_generation++;
    gdi_font_resources_unlock();

    while (retired) {
        GDI_FONT_RESOURCE *next = retired->next;
        kfree(retired->path);
        kfree(retired->data);
        kfree(retired);
        retired = next;
    }
    dwrite_release_system_collection_cache(process_id);
}

/* ── GDI DC and Bitmap tables ────────────────────────────────── */

#define MAX_GDI_DCS     4096
#define MAX_GDI_BITMAPS 512
#define MAX_GDI_REGIONS 512
#define MAX_REGION_RECTS GDI_MAX_REGION_RECTS

typedef struct GDI_PAINT_CLIP {
    struct GDI_PAINT_CLIP *previous;
    GDI_RECT rect;
    GDI_REGION region;
    uint64_t token;
} GDI_PAINT_CLIP;

typedef struct GDI_SAVED_DC GDI_SAVED_DC;
typedef struct { LONG x, y; } GDI_POINT;

typedef struct {
    DWORD red, green, blue;
    UINT color_count;
    DWORD colors[256];
} GDI_PIXEL_FORMAT;

typedef struct {
    DWORD size;
    LONG width, height;
    USHORT planes, bpp;
    DWORD compression, image_size;
    LONG xppm, yppm;
    DWORD colors_used, colors_important;
} GDI_BITMAP_HEADER;

_Static_assert(sizeof(GDI_BITMAP_HEADER) == 40, "BITMAPINFOHEADER layout changed");

typedef struct {
    int      in_use;
    void    *surface;       /* pixel buffer (NULL = no bitmap selected yet) */
    int      width, height;
    int      bpp;
    int      pitch;         /* bytes per scanline */
    const GDI_PIXEL_FORMAT *format; /* NULL for the native device format */
    uint32_t text_color;
    uint32_t bk_color;
    uint32_t brush_color;
    uint32_t pen_color;
    int      poly_fill_mode, rop2, arc_direction;
    int      bk_mode;       /* TRANSPARENT=1, OPAQUE=2 */
    UINT     text_align;
    int      current_x, current_y;
    int      map_mode;
    int      stretch_mode;
    int      viewport_x, viewport_y;
    int      window_x, window_y;
    int      window_cx, window_cy, viewport_cx, viewport_cy;
    int      brush_x, brush_y;
    int      graphics_mode; /* GM_COMPATIBLE or GM_ADVANCED */
    float    world_transform[6];
    HGDIOBJ  prev_bitmap;   /* currently selected bitmap handle */
    HGDIOBJ  current_font;  /* currently selected font handle */
    HGDIOBJ  current_brush;
    HGDIOBJ  current_pen;
    HPALETTE current_palette;
    BOOL     palette_background;
    BOOL     clip_active;
    GDI_REGION clip_region;
    GDI_PAINT_CLIP *paint_clip;
    int      is_screen;     /* DC targets the display, not a window surface */
    int      screen_display; /* display back buffer rather than boot console */
    int      pooled_window; /* GetDC/BeginPaint DC, retained across ReleaseDC */
    BOOL     whole_window;
    ULONG_PTR owner_window; /* HWND associated with a pooled window DC */
    DWORD    cached_tid;   /* GetDCEx common DC; zero for the legacy pool */
    GDI_REGION *visible_region; /* device-space system clip, separate from app clip */
    HGDIOBJ  owned_clip;   /* GetDCEx region ownership ends with this borrow */
    HGDIOBJ  device_clip;  /* fixed device-space copy of the transferred region */
    DWORD    visible_flags;
    int      bottomup;      /* selected bitmap is a bottom-up DIB */
    DWORD    owner_pid;
    GDI_SAVED_DC *saved;
} GDI_DC;

struct GDI_SAVED_DC {
    GDI_SAVED_DC *previous;
    int level;
    GDI_DC dc;
};

static BOOL gdi_read_dc_pixel(const GDI_DC *dc, int x, int y,
                              uint32_t *color);
static BOOL gdi_write_dc_pixel(GDI_DC *dc, int x, int y, uint32_t color);
static void refresh_screen_dc(GDI_DC *dc);
static BOOL gdi_refresh_visible_region(GDI_DC *dc, GDI_REGION *region);

typedef struct {
    int      in_use;
    void    *pixels;
    int      width, height;
    int      bpp;
    int      pitch;
    int      bottomup;      /* DIB with positive biHeight (rows stored bottom-up) */
    int      virtual_alloc; /* pixels from VirtualAlloc (DIB section) */
    void    *mapping_base;  /* MapViewOfFile base for section-backed DIBs */
    GDI_PIXEL_FORMAT format;
    int      dib_section;
    GDI_BITMAP_HEADER dib_header;
    HANDLE   section;
    DWORD    section_offset;
} GDI_BITMAP;

static BOOL gdi_read_pixel(const BYTE *row, int x, int bpp,
                            const GDI_PIXEL_FORMAT *format, uint32_t *color);
static int gdi_get_dibits(GDI_BITMAP *bmp, UINT start, UINT count,
                           PVOID bits, PVOID info, UINT usage);

typedef struct {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    WCHAR lfFaceName[32];
} GDI_LOGFONTW;

typedef struct {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    char lfFaceName[32];
} GDI_LOGFONTA;

typedef struct GDI_FONT {
    struct GDI_FONT *next;
    HGDIOBJ handle;
    DWORD owner_pid;
    GDI_LOGFONTW logfont;
} GDI_FONT;

_Static_assert(sizeof(GDI_LOGFONTW) == 92, "LOGFONTW layout changed");
_Static_assert(sizeof(GDI_LOGFONTA) == 60, "LOGFONTA layout changed");

static GDI_DC     gdi_dcs[MAX_GDI_DCS];
static GDI_BITMAP gdi_bmps[MAX_GDI_BITMAPS];
#define FONT_BUCKETS 128u
static GDI_FONT *gdi_fonts[FONT_BUCKETS];
static ULONG next_font_id = 1;
static GDI_REGION gdi_regions[MAX_GDI_REGIONS];
static DWORD gdi_region_owners[MAX_GDI_REGIONS];
static volatile int gdi_region_lock;
static volatile int gdi_gamma_lock;
static int gdi_gamma_initialized;
static WORD gdi_gamma_ramp[3][256];
static UINT       gdi_blit_trace_count;

/* Handle encoding:
 *   HDC    = 0xDC000000 | index
 *   HBITMAP = 0xBB000000 | index
 * DC indices use 12 bits; bitmaps use the same decoder with a smaller bound.
 * Logical fonts use allocated records and full 24-bit object identifiers.
 */
#define DC_TAG     0xDC000000u
#define BMP_TAG    0xBB000000u
#define FONT_TAG   0xF0000000u
#define REGION_TAG 0xCC000000u
#define TAG_MASK   0xFF000000u
#define IDX_MASK   0x00000FFFu
#define STOCK_BITMAP ((HGDIOBJ)(ULONG_PTR)(BMP_TAG | 0xFEu))
#define STOCK_SYSTEM_FONT ((HGDIOBJ)(ULONG_PTR)0xAA00000Du)
#define OBJ_BITMAP 7
#define OBJ_FONT   6
#define OBJ_BRUSH  2
#define BRUSH_TAG  0xBE000000u
#define SYSTEM_BRUSH_TAG 0xBC000000u
#define STOCK_WHITE_BRUSH ((HGDIOBJ)(ULONG_PTR)0xAA000000u)
#define STOCK_BLACK_PEN ((HGDIOBJ)(ULONG_PTR)0xAA000007u)
#define PEN_TAG 0xEE000000u
#define STOCK_DEFAULT_PALETTE ((HPALETTE)(ULONG_PTR)0xAA00000Fu)
#define PALETTE_TAG 0xBD000000u
#define OBJ_PAL 5

typedef struct { BYTE red, green, blue, flags; } GDI_PALETTE_ENTRY;
typedef struct {
    WORD version, count;
    GDI_PALETTE_ENTRY entries[];
} GDI_LOGPALETTE;
typedef struct GDI_PALETTE {
    struct GDI_PALETTE *next;
    HPALETTE handle;
    DWORD owner_pid;
    UINT count;
    GDI_PALETTE_ENTRY entries[];
} GDI_PALETTE;

static GDI_PALETTE *gdi_palettes;
static ULONG next_palette_id = 1;
static const DWORD gdi_default_palette[20] = {
    0x000000, 0x000080, 0x008000, 0x008080, 0x800000,
    0x800080, 0x808000, 0xC0C0C0, 0xC0DCC0, 0xF0CAA6,
    0xF0FBFF, 0xA4A0A0, 0x808080, 0x0000FF, 0x00FF00,
    0x00FFFF, 0xFF0000, 0xFF00FF, 0xFFFF00, 0xFFFFFF,
};

typedef struct GDI_BRUSH {
    struct GDI_BRUSH *next;
    HGDIOBJ handle;
    DWORD owner_pid, color;
    UINT style, references;
    GDI_BITMAP pattern;
} GDI_BRUSH;

#define BRUSH_BUCKETS 128u
static GDI_BRUSH *gdi_brushes[BRUSH_BUCKETS];
static ULONG next_brush_id = 1;
static volatile ULONG gdi_brush_lock;
typedef struct GDI_PEN {
    struct GDI_PEN *next;
    HGDIOBJ handle;
    DWORD owner_pid, style, width, color, references;
    BOOL extended;
    DWORD brush_style, style_count, styles[16];
    ULONG_PTR hatch;
    GDI_BITMAP pattern;
} GDI_PEN;
static GDI_PEN *gdi_pens;
static ULONG next_pen_id = 1;
extern DWORD WINAPI GetSysColor(int index);

static uint64_t brushes_lock(void)
{
    uint64_t flags = 0;
#ifndef GDI_HOST_TEST
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(&gdi_brush_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
    return flags;
}

static void brushes_unlock(uint64_t flags)
{
    __atomic_store_n(&gdi_brush_lock, 0, __ATOMIC_RELEASE);
#ifndef GDI_HOST_TEST
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
#else
    (void)flags;
#endif
}

/* Palette storage shares the short, IRQ-safe GDI object lock with brushes.
 * DC use itself, like the existing bitmap table, is caller-synchronized. */
static GDI_PALETTE *palette_find_locked(HPALETTE handle)
{
    for (GDI_PALETTE *palette = gdi_palettes; palette; palette = palette->next)
        if (palette->handle == handle &&
            palette->owner_pid == GetCurrentProcessId()) return palette;
    return NULL;
}

static void gdi_palette_release_process(DWORD process_id)
{
    if (!process_id) return;
    GDI_PALETTE *retired = NULL;
    uint64_t flags = brushes_lock();
    GDI_PALETTE **link = &gdi_palettes;
    while (*link) {
        GDI_PALETTE *palette = *link;
        if (palette->owner_pid != process_id) {
            link = &palette->next;
            continue;
        }
        *link = palette->next;
        palette->next = retired;
        retired = palette;
    }
    brushes_unlock(flags);
    while (retired) {
        GDI_PALETTE *next = retired->next;
        kfree(retired);
        retired = next;
    }
}

static GDI_BRUSH *brush_find_locked(HGDIOBJ handle)
{
    for (GDI_BRUSH *brush = gdi_brushes[(ULONG_PTR)handle % BRUSH_BUCKETS];
         brush; brush = brush->next)
        if (brush->handle == handle) return brush;
    return NULL;
}

static GDI_PEN *gdi_pen_find_locked(HGDIOBJ handle)
{
    for (GDI_PEN *pen = gdi_pens; pen; pen = pen->next)
        if (pen->handle == handle) return pen;
    return NULL;
}

/* A NULL snapshot retains the object without copying its pattern to the stack. */
static BOOL gdi_pen_acquire(HGDIOBJ handle, const GDI_DC *dc, GDI_PEN *out)
{
    ULONG_PTR value = (ULONG_PTR)handle;
    if ((value & TAG_MASK) == 0xAA000000u) {
        UINT stock = value & 0xFFFFFFu;
        if (stock != 6 && stock != 7 && stock != 8 && stock != 19) return FALSE;
        if (out) {
            gdi_memset(out, 0, sizeof(*out));
            out->handle = handle;
            out->width = 1;
            out->style = stock == 8 ? 5 : 0;
            out->color = stock == 6 ? 0xFFFFFFu : stock == 19 && dc ? dc->pen_color : 0;
        }
        return TRUE;
    }
    uint64_t flags = brushes_lock();
    GDI_PEN *pen = gdi_pen_find_locked(handle);
    if (pen && pen->owner_pid != GetCurrentProcessId()) pen = NULL;
    if (pen) {
        pen->references++;
        if (out) *out = *pen;
    }
    brushes_unlock(flags);
    return pen != NULL;
}

static void gdi_pen_release(HGDIOBJ handle)
{
    if (((ULONG_PTR)handle & TAG_MASK) != PEN_TAG) return;
    uint64_t flags = brushes_lock();
    GDI_PEN *pen = gdi_pen_find_locked(handle);
    if (pen && pen->references) pen->references--;
    brushes_unlock(flags);
}

static void gdi_pen_release_process(DWORD process_id)
{
    GDI_PEN *retired = NULL;
    uint64_t flags = brushes_lock();
    GDI_PEN **link = &gdi_pens;
    while (*link) {
        GDI_PEN *pen = *link;
        if (pen->owner_pid != process_id) { link = &pen->next; continue; }
        *link = pen->next;
        pen->next = retired;
        retired = pen;
    }
    brushes_unlock(flags);
    while (retired) {
        GDI_PEN *next = retired->next;
        kfree(retired->pattern.pixels);
        kfree(retired);
        retired = next;
    }
}

/* Borrowed brush snapshots pin pattern pixels across a preemptible draw.
 * A NULL snapshot only retains the selected object. */
static BOOL brush_acquire(HGDIOBJ handle, const GDI_DC *dc, GDI_BRUSH *out)
{
    ULONG_PTR value = (ULONG_PTR)handle;
    if (out) gdi_memset(out, 0, sizeof(*out));
    if ((value & TAG_MASK) == 0xAA000000u) {
        UINT stock = (UINT)(value & ~TAG_MASK);
        static const DWORD colors[] = {
            0x00FFFFFFu, 0x00C0C0C0u, 0x00808080u, 0x00404040u, 0,
        };
        if (stock > 5 && stock != 18) return FALSE;
        if (out) {
            if (stock < 5) out->color = colors[stock];
            else if (stock == 5) out->style = 1; /* NULL_BRUSH */
            else out->color = dc ? dc->brush_color : 0xFFFFFFu;
        }
        return TRUE;
    }
    if ((value & TAG_MASK) == SYSTEM_BRUSH_TAG &&
        (value & ~TAG_MASK) < 31) {
        if (out) out->color = GetSysColor((int)(value & ~TAG_MASK));
        return TRUE;
    }
    if ((value & TAG_MASK) != BRUSH_TAG) return FALSE;
    uint64_t flags = brushes_lock();
    GDI_BRUSH *brush = brush_find_locked(handle);
    if (brush && brush->owner_pid != GetCurrentProcessId()) brush = NULL;
    if (brush) {
        brush->references++;
        if (out) *out = *brush;
    }
    brushes_unlock(flags);
    return brush != NULL;
}

static void brush_release(HGDIOBJ handle)
{
    uint64_t flags = brushes_lock();
    GDI_BRUSH *brush = brush_find_locked(handle);
    if (brush && brush->references) brush->references--;
    brushes_unlock(flags);
}

static void gdi_brush_release_process(DWORD process_id)
{
    if (!process_id) return;
    GDI_BRUSH *retired = NULL;
    uint64_t flags = brushes_lock();
    for (UINT bucket = 0; bucket < BRUSH_BUCKETS; bucket++) {
        GDI_BRUSH **link = &gdi_brushes[bucket];
        while (*link) {
            GDI_BRUSH *brush = *link;
            if (brush->owner_pid != process_id) {
                link = &brush->next;
                continue;
            }
            for (int i = 0; i < MAX_GDI_DCS; i++)
                if (gdi_dcs[i].current_brush == brush->handle)
                    gdi_dcs[i].current_brush = STOCK_WHITE_BRUSH;
            *link = brush->next;
            brush->next = retired;
            retired = brush;
        }
    }
    brushes_unlock(flags);
    while (retired) {
        GDI_BRUSH *next = retired->next;
        kfree(retired->pattern.pixels);
        kfree(retired);
        retired = next;
    }
}

#define GDI_BI_RGB            0u
#define GDI_BI_BITFIELDS      3u
#define GDI_BI_ALPHABITFIELDS 6u
#define GDI_DIB_RGB_COLORS    0u
#define GDI_CBM_INIT          0x04u

#define IS_DC_HANDLE(h)  (((ULONG_PTR)(h) & TAG_MASK) == DC_TAG)
#define IS_BMP_HANDLE(h) (((ULONG_PTR)(h) & TAG_MASK) == BMP_TAG)
#define IS_FONT_HANDLE(h) (((ULONG_PTR)(h) & TAG_MASK) == FONT_TAG)
#define IS_REGION_HANDLE(h) (((ULONG_PTR)(h) & TAG_MASK) == REGION_TAG)
#define IS_STOCK_FONT_HANDLE(h) \
    ((((ULONG_PTR)(h) & TAG_MASK) == 0xAA000000u) && \
     (((ULONG_PTR)(h) & IDX_MASK) >= 10u) && \
     (((ULONG_PTR)(h) & IDX_MASK) <= 17u) && \
     (((ULONG_PTR)(h) & IDX_MASK) != 15u))
#define DC_INDEX(h)      ((int)((ULONG_PTR)(h) & IDX_MASK))
#define BMP_INDEX(h)     ((int)((ULONG_PTR)(h) & IDX_MASK))
#define REGION_INDEX(h)  ((int)((ULONG_PTR)(h) & IDX_MASK))

BOOL gdi32_dc_display_name(HDC h,WCHAR name[32])
{
    if (!name || !IS_DC_HANDLE(h)) return FALSE;
    int idx=DC_INDEX(h);
    if (idx<0 || idx>=MAX_GDI_DCS || !gdi_dcs[idx].in_use ||
        gdi_dcs[idx].owner_pid!=GetCurrentProcessId()) return FALSE;
    /* GDI currently has one screen. Memory DCs retain its device identity
     * too, as on Windows; they do not acquire a presentation target. */
    static const char screen[]="\\\\.\\DISPLAY1";
    for (unsigned i=0;i<sizeof(screen);i++) name[i]=(WCHAR)screen[i];
    return TRUE;
}

static GDI_DC *dc_from_handle(HDC h)
{
    if (!IS_DC_HANDLE(h)) return NULL;
    int idx = DC_INDEX(h);
    if (idx < 0 || idx >= MAX_GDI_DCS) return NULL;
    if (!gdi_dcs[idx].in_use) return NULL;
    GDI_DC *dc = &gdi_dcs[idx];

    /* The display may start, resize, or rotate its back buffer while a screen
     * DC is retained. Never retain the redirected console as a screen target. */
    if (dc->is_screen)
        refresh_screen_dc(dc);

    /* A window DC survives ReleaseDC and may be retained by EGL for the
     * lifetime of a window.  Keep it attached to user32's current backing
     * store when that store is replaced by a resize. */
    if (dc->pooled_window && dc->owner_window && !dc->is_screen &&
        user32_get_window_dc_surface) {
        void *surface = NULL;
        int width = 0, height = 0, pitch = 0;
        if (user32_get_window_dc_surface((HANDLE)dc->owner_window, &surface,
                                         &width, &height, &pitch, dc->whole_window)) {
            if (dc->surface != surface || dc->width != width ||
                dc->height != height || dc->pitch != pitch) {
                static uint32_t refresh_trace_count;
                if (refresh_trace_count++ < 64) {
                    serial_puts("[GDI-DC-REFRESH] hwnd=0x");
                    serial_puthex(dc->owner_window, 8);
                    serial_puts(" size=");
                    serial_putdec((uint32_t)width);
                    serial_puts("x");
                    serial_putdec((uint32_t)height);
                    serial_puts(" pitch=");
                    serial_putdec((uint32_t)pitch);
                    serial_puts("\n");
                }
            }
            dc->surface = surface;
            dc->width = width;
            dc->height = height;
            dc->pitch = pitch;
            dc->is_screen = 0;
        } else {
            dc->surface = NULL;
            dc->width = 0;
            dc->height = 0;
            dc->pitch = 0;
        }
    }
    return dc;
}

/* A window draw owns its surface until all exits from the operation. The DC
 * handle itself remains pooled; replacing its cached target must not redirect
 * an in-flight copy. Memory bitmap/DC synchronization remains caller-owned. */
typedef struct {
    GDI_DC dc;
    GDI_PAINT_CLIP paint_clip;
    GDI_REGION visible_region;
    compositor_surface_ref_t surface;
    GDI_DC *original;
} GDI_DRAW_DC;

static void gdi_release_draw_dc(GDI_DRAW_DC *draw)
{
    gdi32_region_data_clear(&draw->dc.clip_region);
    gdi32_region_data_clear(&draw->visible_region);
    gdi32_region_data_clear(&draw->paint_clip.region);
    if (draw->surface.handle && compositor_release_surface)
        compositor_release_surface(&draw->surface);
}

/* GCC/Clang cleanup also covers validation errors and nested drawing calls. */
#define GDI_DRAW_SCOPE(name) \
    GDI_DRAW_DC name __attribute__((cleanup(gdi_release_draw_dc))) = {0}

static GDI_DC *gdi_acquire_draw_dc(HDC handle, GDI_DRAW_DC *draw)
{
    GDI_DC *dc = dc_from_handle(handle);
    if (!dc || !dc->pooled_window || !dc->owner_window ||
        (dc->is_screen && !dc->cached_tid))
        return dc;
    draw->dc = *dc;
    gdi32_region_data_retain(&draw->dc.clip_region);
    draw->original = dc;
    if (dc->visible_region) {
        gdi32_region_data_assign(&draw->visible_region, dc->visible_region);
        draw->dc.visible_region = &draw->visible_region;
        if (!gdi_refresh_visible_region(&draw->dc, &draw->visible_region)) return NULL;
    }
    if (dc->paint_clip) {
        draw->paint_clip = *dc->paint_clip;
        gdi32_region_data_retain(&draw->paint_clip.region);
        draw->paint_clip.previous = NULL;
        draw->dc.paint_clip = &draw->paint_clip;
    }
    if (!dc->is_screen && user32_acquire_window_dc_surface && compositor_release_surface) {
        if (!user32_acquire_window_dc_surface((HANDLE)draw->dc.owner_window,
                                               draw->dc.whole_window,
                                               &draw->surface)) return NULL;
        draw->dc.surface = draw->surface.pixels;
        draw->dc.width = (int)draw->surface.width;
        draw->dc.height = (int)draw->surface.height;
        draw->dc.pitch = (int)draw->surface.pitch * 4;
        draw->dc.bpp = 32;
        draw->dc.bottomup = 0;
        draw->dc.format = NULL;
    }
    /* Headless host shims without the owning API retain the borrowed path. */
    return &draw->dc;
}

static void gdi_draw_commit_position(GDI_DRAW_DC *draw, GDI_DC *dc, int x, int y)
{
    GDI_DC *target = draw->original ? draw->original : dc;
    if (target && target->in_use && target->owner_window == dc->owner_window) {
        target->current_x = x;
        target->current_y = y;
    }
}

static GDI_BITMAP *bmp_from_handle(HBITMAP h)
{
    if (!IS_BMP_HANDLE(h)) return NULL;
    int idx = BMP_INDEX(h);
    if (idx < 0 || idx >= MAX_GDI_BITMAPS) return NULL;
    if (!gdi_bmps[idx].in_use) return NULL;
    return &gdi_bmps[idx];
}

static GDI_FONT *gdi_font_find_locked(HGDIOBJ handle)
{
    for (GDI_FONT *font = gdi_fonts[(ULONG_PTR)handle % FONT_BUCKETS];
         font; font = font->next)
        if (font->handle == handle) return font;
    return NULL;
}

/* Copy metadata while protected: callers never borrow an allocation which
 * DeleteObject or process cleanup can free. DC use is caller-synchronized. */
static BOOL font_from_handle(HGDIOBJ handle, GDI_LOGFONTW *snapshot,
                             BOOL require_owner)
{
    if (!IS_FONT_HANDLE(handle)) return FALSE;
    DWORD owner = GetCurrentProcessId();
    uint64_t flags = brushes_lock();
    GDI_FONT *font = gdi_font_find_locked(handle);
    BOOL found = font && (!require_owner || font->owner_pid == owner);
    if (found && snapshot) *snapshot = font->logfont;
    brushes_unlock(flags);
    return found;
}

static void gdi_font_release_process(DWORD process_id)
{
    if (!process_id) return;
    GDI_FONT *retired = NULL;
    uint64_t flags = brushes_lock();
    for (UINT bucket = 0; bucket < FONT_BUCKETS; bucket++) {
        GDI_FONT **link = &gdi_fonts[bucket];
        while (*link) {
            GDI_FONT *font = *link;
            if (font->owner_pid != process_id) {
                link = &font->next;
                continue;
            }
            *link = font->next;
            font->next = retired;
            retired = font;
        }
    }
    brushes_unlock(flags);
    while (retired) {
        GDI_FONT *next = retired->next;
        kfree(retired);
        retired = next;
    }
}

static GDI_REGION *region_from_handle(HGDIOBJ h)
{
    if (!IS_REGION_HANDLE(h)) return NULL;
    int idx = REGION_INDEX(h);
    if (idx < 0 || idx >= MAX_GDI_REGIONS) return NULL;
    if (!gdi_regions[idx].in_use) return NULL;
    return &gdi_regions[idx];
}

static void regions_lock(void)
{
    while (__atomic_exchange_n(&gdi_region_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void regions_unlock(void)
{
    __atomic_store_n(&gdi_region_lock, 0, __ATOMIC_RELEASE);
}

void gdi32_region_data_retain(GDI_REGION *data)
{
    if (data && data->storage)
        __atomic_add_fetch(&data->storage->references, 1, __ATOMIC_RELAXED);
}

void gdi32_region_data_clear(GDI_REGION *data)
{
    if (!data) return;
    if (data->storage &&
        !__atomic_sub_fetch(&data->storage->references, 1, __ATOMIC_ACQ_REL))
        kfree(data->storage);
    gdi_memset(data, 0, sizeof(*data));
}

void gdi32_region_data_assign(GDI_REGION *destination, const GDI_REGION *source)
{
    if (destination == source) return;
    if (source->storage)
        __atomic_add_fetch(&source->storage->references, 1, __ATOMIC_RELAXED);
    gdi32_region_data_clear(destination);
    *destination = *source;
}

void gdi32_region_data_move(GDI_REGION *destination, GDI_REGION *source)
{
    if (destination == source) return;
    gdi32_region_data_clear(destination);
    *destination = *source;
    gdi_memset(source, 0, sizeof(*source));
}

/* The caller owns the mutable value. Handles are protected by regions_lock,
 * while USER32 values use its message lock; saved drawing snapshots retain
 * their backing and therefore never observe an in-place mutation. */
static BOOL region_reserve(GDI_REGION *region, UINT minimum)
{
    GDI_REGION_STORAGE *old = region->storage;
    if (!old && minimum <= MAX_REGION_RECTS) return TRUE;
    if (old && old->capacity >= minimum &&
        __atomic_load_n(&old->references, __ATOMIC_ACQUIRE) == 1) return TRUE;
    UINT capacity = old ? old->capacity : MAX_REGION_RECTS;
    while (capacity < minimum) {
        if (capacity > 0x7FFFFFFFu) { capacity = minimum; break; }
        capacity *= 2;
    }
    SIZE_T bytes;
    if (__builtin_mul_overflow((SIZE_T)capacity, sizeof(GDI_RECT), &bytes) ||
        __builtin_add_overflow(bytes, sizeof(*old), &bytes))
        return FALSE;
    GDI_REGION_STORAGE *storage = kmalloc(bytes);
    if (!storage) return FALSE;
    storage->references = 1;
    storage->capacity = capacity;
    gdi_memcpy(storage->rects, GDI_REGION_RECTS(region),
                 (SIZE_T)region->count * sizeof(GDI_RECT));
    region->storage = storage;
    if (old && !__atomic_sub_fetch(&old->references, 1, __ATOMIC_ACQ_REL))
        kfree(old);
    return TRUE;
}

static void gdi_region_release_process(DWORD process_id)
{
    if (!process_id) return;
    regions_lock();
    for (int i = 0; i < MAX_GDI_REGIONS; i++) {
        if (gdi_region_owners[i] != process_id) continue;
        gdi32_region_data_clear(&gdi_regions[i]);
        gdi_region_owners[i] = 0;
    }
    regions_unlock();
}

static BOOL gdi_refresh_visible_region(GDI_DC *dc, GDI_REGION *region)
{
    if (!dc->cached_tid || !user32_get_dc_visible_region) return TRUE;
    HGDIOBJ handle = user32_get_dc_visible_region((HANDLE)dc->owner_window,
                                                    dc->visible_flags, dc->device_clip);
    if (!handle) return FALSE;
    regions_lock();
    GDI_REGION *fresh = region_from_handle(handle);
    if (fresh) gdi32_region_data_assign(region, fresh);
    regions_unlock();
    DeleteObject(handle);
    return fresh != NULL;
}

static void gamma_lock(void)
{
    while (__atomic_exchange_n(&gdi_gamma_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void gamma_unlock(void)
{
    __atomic_store_n(&gdi_gamma_lock, 0, __ATOMIC_RELEASE);
}

static void gamma_initialize(void)
{
    if (gdi_gamma_initialized) return;
    for (int channel = 0; channel < 3; channel++)
        for (int value = 0; value < 256; value++)
            gdi_gamma_ramp[channel][value] = (WORD)(value * 257U);
    gdi_gamma_initialized = 1;
}

static int alloc_region(void)
{
    for (int i = 0; i < MAX_GDI_REGIONS; i++) {
        if (gdi_regions[i].in_use) continue;
        gdi_memset(&gdi_regions[i], 0, sizeof(gdi_regions[i]));
        gdi_regions[i].in_use = 1;
        gdi_region_owners[i] = GetCurrentProcessId();
        return i;
    }
    return -1;
}

static void gdi_stock_font_logfont(GDI_LOGFONTW *lf)
{
    static const char face[] = "System";
    gdi_memset(lf, 0, sizeof(*lf));
    lf->lfHeight = -16;
    lf->lfWeight = 400;
    lf->lfCharSet = 1; /* DEFAULT_CHARSET */
    for (int i = 0; face[i] && i < 31; i++)
        lf->lfFaceName[i] = (WCHAR)(BYTE)face[i];
}

static int alloc_dc(void)
{
    for (int i = 0; i < MAX_GDI_DCS; i++) {
        if (!gdi_dcs[i].in_use) {
            gdi_memset(&gdi_dcs[i], 0, sizeof(GDI_DC));
            gdi_dcs[i].in_use = 1;
            gdi_dcs[i].owner_pid = GetCurrentProcessId();
            gdi_dcs[i].text_color = 0x00000000u; /* COLORREF black */
            gdi_dcs[i].bk_color = 0x00FFFFFFu;   /* COLORREF white */
            gdi_dcs[i].bk_mode = 2; /* OPAQUE */
            gdi_dcs[i].map_mode = 1; /* MM_TEXT */
            gdi_dcs[i].window_cx = gdi_dcs[i].window_cy = 1;
            gdi_dcs[i].viewport_cx = gdi_dcs[i].viewport_cy = 1;
            gdi_dcs[i].stretch_mode = 1; /* BLACKONWHITE */
            gdi_dcs[i].graphics_mode = 1; /* GM_COMPATIBLE */
            gdi_dcs[i].world_transform[0] = 1.0f;
            gdi_dcs[i].world_transform[3] = 1.0f;
            gdi_dcs[i].current_font = STOCK_SYSTEM_FONT;
            gdi_dcs[i].current_brush = STOCK_WHITE_BRUSH;
            gdi_dcs[i].current_pen = STOCK_BLACK_PEN;
            gdi_dcs[i].poly_fill_mode = 1; /* ALTERNATE */
            gdi_dcs[i].rop2 = 13; /* R2_COPYPEN */
            gdi_dcs[i].arc_direction = 1; /* AD_COUNTERCLOCKWISE */
            gdi_dcs[i].current_palette = STOCK_DEFAULT_PALETTE;
            gdi_dcs[i].brush_color = 0x00FFFFFFu;
            return i;
        }
    }
    serial_puts("[GDI32] DC table exhausted capacity=");
    serial_putdec(MAX_GDI_DCS);
    serial_puts("\n");
    return -1;
}

static int alloc_bmp(void)
{
    for (int i = 0; i < MAX_GDI_BITMAPS; i++) {
        if (!gdi_bmps[i].in_use) {
            gdi_memset(&gdi_bmps[i], 0, sizeof(GDI_BITMAP));
            gdi_bmps[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static int bitmap_slots_used(void)
{
    int used = 0;
    for (int i = 0; i < MAX_GDI_BITMAPS; i++)
        if (gdi_bmps[i].in_use) used++;
    return used;
}

static void get_display_geometry(int *out_w, int *out_h, int *out_bpp)
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bpp = 0;

    if (user32_get_current_display_mode)
        user32_get_current_display_mode(&width, &height, &bpp, NULL);
    else if (ddraw_display_mode_active && ddraw_get_display_mode &&
             ddraw_display_mode_active())
        ddraw_get_display_mode(&width, &height, &bpp);

    if (!width && display_get_width) width = display_get_width();
    if (!height && display_get_height) height = display_get_height();
    if (!width && fb_get_width) width = fb_get_width();
    if (!height && fb_get_height) height = fb_get_height();

    if (!width) width = GDI_FALLBACK_SCREEN_WIDTH;
    if (!height) height = GDI_FALLBACK_SCREEN_HEIGHT;
    if (!bpp) bpp = GDI_FALLBACK_SCREEN_BPP;

    if (out_w) *out_w = (int)width;
    if (out_h) *out_h = (int)height;
    if (out_bpp) *out_bpp = (int)bpp;
}

static void refresh_screen_dc(GDI_DC *dc)
{
    uint32_t *base = NULL;
    uint32_t width = 0, height = 0, pitch = 0;
    int display = 0;

    if (display_get_back_buffer && display_get_width && display_get_height &&
        display_get_pitch) {
        base = display_get_back_buffer();
        if (base) {
            width = display_get_width();
            height = display_get_height();
            pitch = display_get_pitch();
            display = 1;
        }
    }
    /* fb_get_base/dimensions describe the terminal after fb_redirect(), not
     * the screen. The boot-console fallback is valid only before redirect. */
    if (!base && fb_is_redirected && !fb_is_redirected() && fb_get_base &&
        fb_get_width && fb_get_height && fb_get_pitch) {
        base = fb_get_base();
        width = fb_get_width();
        height = fb_get_height();
        pitch = fb_get_pitch();
    }

    /* Native surfaces stay 32-bit even in a 16-bit logical display mode.
     * Both providers use pixels/row; GDI uses bytes/row and signed offsets. */
    if (!base || !width || !height || pitch < width ||
        pitch > 0x7FFFFFFFu / sizeof(uint32_t) || height > 0x7FFFFFFFu ||
        (uint64_t)pitch * sizeof(uint32_t) * height > 0x7FFFFFFFu) {
        base = NULL;
        width = height = pitch = 0;
    }
    dc->surface = base;
    dc->width = (int)width;
    dc->height = (int)height;
    dc->pitch = (int)(pitch * sizeof(uint32_t));
    dc->bpp = 32;
    dc->format = NULL;
    dc->bottomup = 0;
    dc->screen_display = display;
}

static void mark_dc_dirty(GDI_DC *dc)
{
    if (!dc || !dc->surface) return;
    if (dc->is_screen) {
        if (dc->screen_display) {
            if (display_force_refresh) display_force_refresh();
        } else if (fb_present_rows) {
            fb_present_rows(0, (uint32_t)dc->height);
        }
    } else if (dc->pooled_window && dc->owner_window && user32_mark_window_dirty) {
        user32_mark_window_dirty((HANDLE)dc->owner_window);
    }
}

/* ── GetDeviceCaps ───────────────────────────────────────────── */

static int WINAPI ExtEscape_k32(HDC dc, int escape, int input_bytes,
                                  PCSTR input, int output_bytes, PSTR output)
{
    if (escape == 8 && (input_bytes < 4 || !input)) return 0; /* QUERYESCSUPPORT */
    if (!dc_from_handle(dc)) {
        SetLastError(6);
        return 0;
    }
    if ((input && input_bytes < 0) || (output && output_bytes < 0)) return -1;
    if ((input && input_bytes && !win32_user_range_readable(input, input_bytes, g_compat32_mode)) ||
        (output && output_bytes && !win32_user_range_writable(output, output_bytes, g_compat32_mode))) {
        SetLastError(998);
        return -1;
    }
    /* The current raster display/memory driver exposes no private escapes or
     * printer passthrough. Zero the marshalled output, as GDI does, and report
     * unsupported (zero), including when queried via QUERYESCSUPPORT. */
    if (output && output_bytes) gdi_memset(output, 0, (SIZE_T)output_bytes);
    return 0;
}

int WINAPI GetDeviceCaps(HDC hdc, int index)
{
    (void)hdc;
    /* NT reports the current logical display mode through the display DC. */
    int hw = 0, vh = 0, mode_bpp = 0;
    get_display_geometry(&hw, &vh, &mode_bpp);
    switch (index) {
    /* There is no EDID physical-size service yet. Use the same 96-DPI
     * virtual display as LOGPIXELS, rounded to whole millimeters. */
    case 4:         return (int)(((int64_t)hw * 254 + 480) / 960); /* HORZSIZE */
    case 6:         return (int)(((int64_t)vh * 254 + 480) / 960); /* VERTSIZE */
    case HORZRES:    return hw;
    case VERTRES:    return vh;
    case BITSPIXEL:  return mode_bpp;
    case PLANES:     return 1;
    case RASTERCAPS: return 0;
    case TECHNOLOGY: return 1; /* DT_RASDISPLAY */
    case LOGPIXELSX:
    case LOGPIXELSY: return 96;
    default:         return 0;
    }
}

/* ── DC creation / destruction ───────────────────────────────── */

BOOL WINAPI GetDeviceGammaRamp(HDC hdc, PVOID ramp)
{
    if (!dc_from_handle(hdc)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (!ramp) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    gamma_lock();
    gamma_initialize();
    gdi_memcpy(ramp, gdi_gamma_ramp, sizeof(gdi_gamma_ramp));
    gamma_unlock();
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI SetDeviceGammaRamp(HDC hdc, PCVOID ramp)
{
    if (!dc_from_handle(hdc)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (!ramp) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    gamma_lock();
    gamma_initialize();
    gdi_memcpy(gdi_gamma_ramp, ramp, sizeof(gdi_gamma_ramp));
    gamma_unlock();
    SetLastError(0);
    return TRUE;
}

HDC WINAPI CreateDCA(PCSTR lpszDriver, PCSTR lpszDevice,
                     PCSTR lpszOutput, PCVOID lpInitData)
{
    (void)lpszDriver; (void)lpszDevice;
    (void)lpszOutput; (void)lpInitData;
    serial_puts("[GDI32] CreateDCA\n");

    /* Allocate a screen DC */
    int idx = alloc_dc();
    if (idx < 0) return NULL;

    gdi_dcs[idx].is_screen = 1;
    refresh_screen_dc(&gdi_dcs[idx]);

    return (HDC)(ULONG_PTR)(DC_TAG | (unsigned)idx);
}

static HDC WINAPI CreateDCW_k32(PCWSTR driver, PCWSTR device,
                                 PCWSTR output, PCVOID init_data)
{
    (void)driver;
    (void)device;
    (void)output;
    return CreateDCA(NULL, NULL, NULL, init_data);
}

static BOOL WINAPI GetICMProfileW_stub(HDC hdc, DWORD *name_chars,
                                        PWSTR filename)
{
    if (!dc_from_handle(hdc) || !name_chars) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (filename && *name_chars)
        filename[0] = 0;
    *name_chars = 0;
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return FALSE;
}

HDC WINAPI CreateCompatibleDC(HDC hdc)
{
#ifndef OK_QUIET
    serial_puts("[GDI32] CreateCompatibleDC\n");
#endif

    int idx = alloc_dc();
    if (idx < 0) return NULL;

    /* Inherit dimensions from source DC (or screen defaults) */
    GDI_DC *src = dc_from_handle(hdc);
    if (src) {
        gdi_dcs[idx].width  = src->width;
        gdi_dcs[idx].height = src->height;
        gdi_dcs[idx].bpp    = src->bpp;
        gdi_dcs[idx].pitch  = src->pitch;
    } else {
        get_display_geometry(&gdi_dcs[idx].width, &gdi_dcs[idx].height,
                             &gdi_dcs[idx].bpp);
        gdi_dcs[idx].pitch = gdi_dcs[idx].width *
                             ((gdi_dcs[idx].bpp + 7) / 8);
    }
    /* surface = NULL — no bitmap selected yet */

    gdi_dcs[idx].prev_bitmap = STOCK_BITMAP;
    return (HDC)(ULONG_PTR)(DC_TAG | (unsigned)idx);
}

static void gdi_release_dc(GDI_DC *dc)
{
    if (dc->owned_clip) DeleteObject(dc->owned_clip);
    if (dc->device_clip) DeleteObject(dc->device_clip);
    gdi32_region_data_clear(dc->visible_region);
    kfree(dc->visible_region);
    gdi32_region_data_clear(&dc->clip_region);
    brush_release(dc->current_brush);
    GDI_SAVED_DC *saved = dc->saved;
    gdi_pen_release(dc->current_pen);
    while (saved) {
        GDI_SAVED_DC *previous = saved->previous;
        brush_release(saved->dc.current_brush);
        gdi_pen_release(saved->dc.current_pen);
        gdi32_region_data_clear(&saved->dc.clip_region);
        kfree(saved);
        saved = previous;
    }
    GDI_PAINT_CLIP *clip = dc->paint_clip;
    gdi_memset(dc, 0, sizeof(*dc));
    while (clip) {
        GDI_PAINT_CLIP *previous = clip->previous;
        gdi32_region_data_clear(&clip->region);
        kfree(clip);
        clip = previous;
    }
}

static void gdi_dc_release_process(DWORD process_id)
{
    if (!process_id) return;
    for (int i = 0; i < MAX_GDI_DCS; i++)
        if (gdi_dcs[i].in_use && gdi_dcs[i].owner_pid == process_id)
            gdi_release_dc(&gdi_dcs[i]);
    gdi_pen_release_process(process_id);
}

BOOL WINAPI DeleteDC(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc && dc->pooled_window)
        return FALSE;
    if (dc) gdi_release_dc(dc);
    return TRUE;
}

/* ── Screen DC helpers (called from user32 GetDC/ReleaseDC) ──── */

static HDC gdi_alloc_window_dc(HANDLE window, BOOL whole, BOOL cached)
{
    int idx = -1;
    for (int i = 0; !cached && i < MAX_GDI_DCS; i++) {
        if (gdi_dcs[i].in_use && gdi_dcs[i].pooled_window &&
            !gdi_dcs[i].cached_tid &&
            gdi_dcs[i].whole_window == whole &&
            gdi_dcs[i].owner_window == (ULONG_PTR)window) {
            idx = i;
            break;
        }
    }
    if (idx < 0) idx = alloc_dc();
    if (idx < 0) return NULL;

    GDI_DC *dc = &gdi_dcs[idx];

    int w = 0, h = 0, p = 0;
    void *base = NULL;
    int is_screen = !window || (GetDesktopWindow && window == GetDesktopWindow());

    /* A missing/empty window surface must never redirect its paint to the
     * screen. dc_from_handle will attach it when user32 creates the surface. */
    if (!is_screen && user32_get_window_dc_surface)
        user32_get_window_dc_surface(window, &base, &w, &h, &p, whole);

    dc->surface = base;
    dc->width   = w;
    dc->height  = h;
    dc->bpp     = 32;
    dc->format  = NULL;
    dc->pitch   = p;
    dc->is_screen = is_screen;
    dc->pooled_window = 1;
    dc->whole_window = whole;
    dc->owner_window = (ULONG_PTR)window;
    dc->cached_tid = cached ? GetCurrentThreadId() : 0;
    dc->current_font = STOCK_SYSTEM_FONT;
    if (is_screen) refresh_screen_dc(dc);

    return (HDC)(ULONG_PTR)(DC_TAG | (unsigned)idx);
}

HDC gdi32_alloc_window_dc(HANDLE window)
{
    return gdi_alloc_window_dc(window, FALSE, FALSE);
}

HDC gdi32_alloc_nonclient_dc(HANDLE window)
{
    return gdi_alloc_window_dc(window, TRUE, FALSE);
}

HDC gdi32_alloc_extended_dc(HANDLE window, BOOL whole,
                             HGDIOBJ visible, HGDIOBJ owned_clip,
                             HGDIOBJ device_clip, DWORD flags)
{
    GDI_REGION *copy = kmalloc(sizeof(*copy));
    if (!copy) return NULL;
    gdi_memset(copy, 0, sizeof(*copy));
    regions_lock();
    GDI_REGION *region = region_from_handle(visible);
    if (region) gdi32_region_data_assign(copy, region);
    regions_unlock();
    if (!region) { kfree(copy); return NULL; }
    HDC handle = gdi_alloc_window_dc(window, whole, TRUE);
    GDI_DC *dc = dc_from_handle(handle);
    if (!dc) { gdi32_region_data_clear(copy); kfree(copy); return NULL; }
    dc->visible_region = copy;
    dc->owned_clip = owned_clip;
    dc->device_clip = device_clip;
    dc->visible_flags = flags;
    return handle;
}

void gdi32_release_thread_dc(DWORD pid, DWORD tid)
{
    if (!pid || !tid) return;
    for (int i = 0; i < MAX_GDI_DCS; i++) {
        GDI_DC *dc = &gdi_dcs[i];
        if (dc->in_use && dc->owner_pid == pid && dc->cached_tid == tid)
            gdi_release_dc(dc);
    }
}

HDC gdi32_alloc_screen_dc(void)
{
    return gdi32_alloc_window_dc(NULL);
}

HANDLE gdi32_window_from_dc(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !dc->pooled_window) return NULL;
    return (HANDLE)dc->owner_window;
}

int gdi32_free_screen_dc(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !dc->pooled_window) return 0;
    if (dc->cached_tid) {
        if (dc->owner_pid != GetCurrentProcessId() ||
            dc->cached_tid != GetCurrentThreadId()) return 0;
        gdi_release_dc(dc);
        return 1;
    }
    /* ReleaseDC releases a pooled borrow; the DC remains associated with its
     * window so WindowFromDC stays deterministic across concurrent windows. */
    return 1;
}

BOOL gdi32_push_paint_clip(HDC hdc, const GDI_RECT *rect, uint64_t *token)
{
    if (!rect) return FALSE;
    GDI_REGION_SCOPE(region);
    if (rect->left < rect->right && rect->top < rect->bottom) {
        region.count = 1;
        region.rects[0] = *rect;
    }
    return gdi32_push_paint_region(hdc, &region, token);
}

BOOL gdi32_push_paint_region(HDC hdc, const GDI_REGION *region, uint64_t *token)
{
    static uint64_t next_token;
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !region || !token)
        return FALSE;
    GDI_PAINT_CLIP *clip = kmalloc(sizeof(*clip));
    if (!clip) return FALSE;
    gdi_memset(clip, 0, sizeof(*clip));
    clip->previous = dc->paint_clip;
    gdi32_region_data_assign(&clip->region, region);
    if (clip->previous && !gdi32_region_data_combine(&clip->region, region,
                            &clip->previous->region, GDI_RGN_AND)) {
        gdi32_region_data_clear(&clip->region);
        kfree(clip);
        return FALSE;
    }
    gdi32_region_data_bounds(&clip->region, &clip->rect);
    clip->token = __atomic_add_fetch(&next_token, 1, __ATOMIC_RELAXED);
    dc->paint_clip = clip;
    *token = clip->token;
    return TRUE;
}

void gdi32_pop_paint_clip(HDC hdc, uint64_t token, BOOL restore_previous)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !dc->paint_clip || dc->paint_clip->token != token) return;
    do {
        GDI_PAINT_CLIP *clip = dc->paint_clip;
        dc->paint_clip = clip->previous;
        gdi32_region_data_clear(&clip->region);
        kfree(clip);
    } while (!restore_previous && dc->paint_clip);
}

void gdi32_release_window_dc(HANDLE window)
{
    if (!window) return;
    for (int i = 0; i < MAX_GDI_DCS; i++) {
        GDI_DC *dc = &gdi_dcs[i];
        if (!dc->in_use || !dc->pooled_window ||
            dc->owner_window != (ULONG_PTR)window) continue;
        gdi_release_dc(dc);
    }
}

/* ── Pixel format (passthrough stubs — DirectDraw uses these) ── */

int WINAPI ChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR *ppfd)
{
    serial_puts("[GDI32-PFD] ChoosePixelFormat hdc=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hdc, 8);
    serial_puts(" pfd=0x");
    serial_puthex((uint64_t)(ULONG_PTR)ppfd, 8);
    if (ppfd) {
        serial_puts(" size=");
        serial_putdec(ppfd->nSize);
        serial_puts(" flags=0x");
        serial_puthex(ppfd->dwFlags, 8);
        serial_puts(" rgba/depth/stencil=");
        serial_putdec(ppfd->cColorBits);
        serial_puts("/");
        serial_putdec(ppfd->cAlphaBits);
        serial_puts("/");
        serial_putdec(ppfd->cDepthBits);
        serial_puts("/");
        serial_putdec(ppfd->cStencilBits);
    }
    serial_puts(" -> 1\n");
    return 1; /* format #1 */
}

BOOL WINAPI SetPixelFormat(HDC hdc, int format, const PIXELFORMATDESCRIPTOR *ppfd)
{
    serial_puts("[GDI32-PFD] SetPixelFormat hdc=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hdc, 8);
    serial_puts(" format=");
    serial_putdec((uint64_t)(uint32_t)format);
    serial_puts(" pfd=0x");
    serial_puthex((uint64_t)(ULONG_PTR)ppfd, 8);
    serial_puts(" -> TRUE\n");
    return TRUE;
}

int WINAPI GetPixelFormat(HDC hdc)
{
    (void)hdc;
    return 1;
}

int WINAPI DescribePixelFormat(HDC hdc, int iPixelFormat, DWORD nBytes,
                                LPPIXELFORMATDESCRIPTOR ppfd)
{
    serial_puts("[GDI32-PFD] DescribePixelFormat hdc=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hdc, 8);
    serial_puts(" format=");
    serial_putdec((uint64_t)(uint32_t)iPixelFormat);
    serial_puts(" bytes=");
    serial_putdec(nBytes);
    serial_puts(" out=0x");
    serial_puthex((uint64_t)(ULONG_PTR)ppfd, 8);
    serial_puts(" -> 1\n");
    if (ppfd && nBytes >= sizeof(PIXELFORMATDESCRIPTOR)) {
        BYTE *p = (BYTE *)ppfd;
        for (SIZE_T i = 0; i < sizeof(PIXELFORMATDESCRIPTOR); i++) p[i] = 0;
        ppfd->nSize       = sizeof(PIXELFORMATDESCRIPTOR);
        ppfd->nVersion    = 1;
        ppfd->dwFlags     = PFD_DRAW_TO_WINDOW;
        if (opengl32_has_render_backend()) {
            /* Mesa supplies this format through our WGL driver. GENERIC_FORMAT
             * denotes the GDI OpenGL implementation, not every CPU renderer.
             * WGL_ACCELERATION_ARB still reports software/hardware separately. */
            ppfd->dwFlags |= PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        } else
            ppfd->dwFlags |= PFD_SUPPORT_GDI;
        ppfd->iPixelType  = PFD_TYPE_RGBA;
        ppfd->cColorBits  = 32;
        ppfd->cRedBits    = 8;
        ppfd->cGreenBits  = 8;
        ppfd->cBlueBits   = 8;
        ppfd->cAlphaBits  = 8;
        ppfd->cDepthBits  = 24;
        ppfd->cStencilBits = 8;
        ppfd->iLayerType  = PFD_MAIN_PLANE;
    }
    return 1; /* 1 pixel format available */
}

BOOL WINAPI SwapBuffers(HDC hdc)
{
    return opengl32_swap_buffers(hdc);
}

/* ── Bitmap creation ─────────────────────────────────────────── */

HBITMAP WINAPI CreateCompatibleBitmap(HDC hdc, int cx, int cy)
{
    serial_puts("[GDI32] CreateCompatibleBitmap hdc=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hdc, 8);
    serial_puts(" size=");
    serial_putdec((uint64_t)(uint32_t)cx);
    serial_puts("x");
    serial_putdec((uint64_t)(uint32_t)cy);
    serial_puts(" slots=");
    serial_putdec((uint64_t)bitmap_slots_used());
    serial_puts("\n");

    if (cx <= 0 || cy <= 0) {
        serial_puts("[GDI32] CreateCompatibleBitmap failed: invalid dimensions\n");
        return NULL;
    }

    int idx = alloc_bmp();
    if (idx < 0) {
        serial_puts("[GDI32] CreateCompatibleBitmap failed: bitmap table full\n");
        return NULL;
    }

    int bpp = 32;
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc) bpp = dc->bpp;

    int pitch = cx * (bpp / 8);
    void *pixels = kmalloc((uint64_t)pitch * cy);
    if (!pixels) {
        gdi_bmps[idx].in_use = 0;
        serial_puts("[GDI32] CreateCompatibleBitmap failed: allocation bytes=0x");
        serial_puthex((uint64_t)(uint32_t)pitch * (uint64_t)(uint32_t)cy, 16);
        serial_puts("\n");
        return NULL;
    }
    gdi_memset(pixels, 0, (SIZE_T)pitch * cy);

    gdi_bmps[idx].pixels = pixels;
    gdi_bmps[idx].width  = cx;
    gdi_bmps[idx].height = cy;
    gdi_bmps[idx].bpp    = bpp;
    gdi_bmps[idx].pitch  = pitch;
    if (dc && dc->format) gdi_bmps[idx].format = *dc->format;

    HBITMAP bitmap = (HBITMAP)(ULONG_PTR)(BMP_TAG | (unsigned)idx);
    serial_puts("[GDI32] CreateCompatibleBitmap -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)bitmap, 8);
    serial_puts("\n");
    return bitmap;
}

static BOOL gdi_native_color_masks(int bpp, DWORD *red, DWORD *green,
                                   DWORD *blue)
{
    if (!red || !green || !blue) return FALSE;

    if (bpp == 16) {
        *red = 0x0000F800u;
        *green = 0x000007E0u;
        *blue = 0x0000001Fu;
        return TRUE;
    }
    if (bpp == 32) {
        *red = 0x00FF0000u;
        *green = 0x0000FF00u;
        *blue = 0x000000FFu;
        return TRUE;
    }
    return FALSE;
}

static void gdi_format_masks(int bpp, const GDI_PIXEL_FORMAT *format,
                              DWORD *red, DWORD *green, DWORD *blue)
{
    if (format && format->red) {
        *red = format->red;
        *green = format->green;
        *blue = format->blue;
    } else {
        *red = *green = *blue = 0;
        gdi_native_color_masks(bpp, red, green, blue);
    }
}

static BOOL gdi_formats_equal(const GDI_DC *a, const GDI_DC *b)
{
    if (a->bpp != b->bpp) return FALSE;
    if (a->bpp == 16 || a->bpp == 32) {
        DWORD ar, ag, ab, br, bg, bb;
        gdi_format_masks(a->bpp, a->format, &ar, &ag, &ab);
        gdi_format_masks(b->bpp, b->format, &br, &bg, &bb);
        return ar == br && ag == bg && ab == bb;
    }
    if (a->bpp <= 8) {
        UINT ac = a->format ? a->format->color_count : 0;
        UINT bc = b->format ? b->format->color_count : 0;
        if (ac != bc) return FALSE;
        for (UINT i = 0; i < ac; i++)
            if (a->format->colors[i] != b->format->colors[i]) return FALSE;
    }
    return TRUE;
}

int WINAPI GetDIBits(HDC hdc, HBITMAP bitmap, UINT start_scan,
                     UINT scan_lines, PVOID bits, PVOID bitmap_info,
                     UINT usage)
{
    (void)hdc;
    GDI_BITMAP *bmp = bmp_from_handle(bitmap);
    return gdi_get_dibits(bmp, start_scan, scan_lines, bits, bitmap_info, usage);
}

HBITMAP WINAPI CreateBitmap(int nWidth, int nHeight, UINT nPlanes,
                             UINT nBitCount, PVOID lpBits)
{
    if (nWidth <= 0 || nHeight <= 0 || nPlanes != 1 ||
        (nBitCount != 1 && nBitCount != 8 && nBitCount != 16 &&
         nBitCount != 24 && nBitCount != 32)) {
        SetLastError(87);
        return NULL;
    }
    /* CreateBitmap input rows are WORD-aligned, unlike DIB DWORD rows. */
    uint64_t row_bytes = (((uint64_t)(UINT)nWidth * nBitCount + 15) / 16) * 2;
    if (row_bytes > 0x7FFFFFFFu || row_bytes * (UINT)nHeight > (SIZE_T)-1) {
        SetLastError(8);
        return NULL;
    }

    int idx = alloc_bmp();
    if (idx < 0) return NULL;

    int bpp = (int)nBitCount;
    int pitch = (int)row_bytes;
    void *pixels = kmalloc((uint64_t)pitch * nHeight);
    if (!pixels) {
        gdi_bmps[idx].in_use = 0;
        return NULL;
    }

    if (lpBits) {
        gdi_memcpy(pixels, lpBits, (SIZE_T)pitch * nHeight);
    } else {
        gdi_memset(pixels, 0, (SIZE_T)pitch * nHeight);
    }

    gdi_bmps[idx].pixels = pixels;
    gdi_bmps[idx].width  = nWidth;
    gdi_bmps[idx].height = nHeight;
    gdi_bmps[idx].bpp    = bpp;
    gdi_bmps[idx].pitch  = pitch;

    return (HBITMAP)(ULONG_PTR)(BMP_TAG | (unsigned)idx);
}

HBITMAP gdi32_clone_bitmap(HBITMAP bitmap)
{
    GDI_BITMAP *source = bmp_from_handle(bitmap);
    if (!source || !source->pixels || source->pitch <= 0 ||
        source->height <= 0)
        return NULL;

    uint64_t size = (uint64_t)(uint32_t)source->pitch *
                    (uint64_t)(uint32_t)source->height;
    if (!size || size > (uint64_t)(SIZE_T)-1)
        return NULL;

    int idx = alloc_bmp();
    if (idx < 0) return NULL;
    void *pixels = kmalloc(size);
    if (!pixels) {
        gdi_bmps[idx].in_use = 0;
        return NULL;
    }

    gdi_memcpy(pixels, source->pixels, (SIZE_T)size);
    gdi_bmps[idx].pixels = pixels;
    gdi_bmps[idx].width = source->width;
    gdi_bmps[idx].height = source->height;
    gdi_bmps[idx].bpp = source->bpp;
    gdi_bmps[idx].pitch = source->pitch;
    gdi_bmps[idx].bottomup = source->bottomup;
    gdi_bmps[idx].format = source->format;
    return (HBITMAP)(ULONG_PTR)(BMP_TAG | (unsigned)idx);
}

static BOOL gdi_read_bitmap_pixel(const GDI_BITMAP *bitmap, int x, int y,
                                  uint32_t *color)
{
    if (!bitmap || !bitmap->pixels || !color || x < 0 || y < 0 ||
        x >= bitmap->width || y >= bitmap->height)
        return FALSE;

    int stored_y = bitmap->bottomup ? bitmap->height - 1 - y : y;
    const BYTE *row = (const BYTE *)bitmap->pixels +
                      (SIZE_T)stored_y * bitmap->pitch;
    if (bitmap->bpp == 32 && (!bitmap->format.red ||
        (bitmap->format.red == 0xFF0000 && bitmap->format.green == 0xFF00 &&
         bitmap->format.blue == 0xFF))) {
        *color = ((const uint32_t *)row)[x];
    } else {
        return gdi_read_pixel(row, x, bitmap->bpp, &bitmap->format, color);
    }
    return TRUE;
}

BOOL gdi32_draw_icon_bitmap(HDC hdc, HBITMAP color_handle,
                            HBITMAP mask_handle, int x, int y,
                            int width, int height)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    GDI_BITMAP *color = bmp_from_handle(color_handle);
    GDI_BITMAP *mask = bmp_from_handle(mask_handle);
    GDI_BITMAP *source = color ? color : mask;
    if (!dc || !dc->surface || !source)
        return FALSE;

    int source_height = color ? source->height : source->height / 2;
    if (width <= 0) width = source->width;
    if (height <= 0) height = source_height;
    if (width <= 0 || height <= 0)
        return FALSE;

    BOOL color_has_alpha = FALSE;
    if (color && color->bpp == 32) {
        for (int sy = 0; sy < color->height && !color_has_alpha; sy++) {
            for (int sx = 0; sx < color->width; sx++) {
                uint32_t sample;
                if (gdi_read_bitmap_pixel(color, sx, sy, &sample) &&
                    (sample & 0xFF000000u)) {
                    color_has_alpha = TRUE;
                    break;
                }
            }
        }
    }

    BOOL drew = FALSE;
    for (int dy = 0; dy < height; dy++) {
        int sy = (int)((uint64_t)(uint32_t)dy *
                       (uint32_t)source_height / (uint32_t)height);
        for (int dx = 0; dx < width; dx++) {
            int sx = (int)((uint64_t)(uint32_t)dx *
                           (uint32_t)source->width / (uint32_t)width);
            uint32_t source_color;
            if (!gdi_read_bitmap_pixel(source, sx, sy, &source_color))
                continue;

            if (!color) {
                uint32_t xor_color, dest;
                if (!gdi_read_bitmap_pixel(mask, sx, sy + source_height,
                                            &xor_color) ||
                    !gdi_read_dc_pixel(dc, x + dx, y + dy, &dest))
                    continue;
                uint32_t result = ((dest & source_color) ^ xor_color) & 0xFFFFFFu;
                drew |= gdi_write_dc_pixel(dc, x + dx, y + dy,
                                            0xFF000000u | result);
                continue;
            }

            BYTE alpha = color ? (BYTE)(source_color >> 24) : 0xFF;
            if (color && !color_has_alpha) {
                uint32_t mask_color = 0;
                if (mask && gdi_read_bitmap_pixel(mask, sx, sy, &mask_color) &&
                    (mask_color & 0x00FFFFFFu) == 0x00FFFFFFu)
                    continue;
                alpha = 0xFF;
            } else if (color && alpha == 0) {
                continue;
            }

            uint32_t output = 0xFF000000u | (source_color & 0x00FFFFFFu);
            if (alpha != 0xFF) {
                uint32_t dest;
                if (gdi_read_dc_pixel(dc, x + dx, y + dy, &dest)) {
                    DWORD inv = 255u - alpha;
                    DWORD r = ((((source_color >> 16) & 0xFFu) * alpha) +
                               (((dest >> 16) & 0xFFu) * inv) + 127u) / 255u;
                    DWORD g = ((((source_color >> 8) & 0xFFu) * alpha) +
                               (((dest >> 8) & 0xFFu) * inv) + 127u) / 255u;
                    DWORD b = (((source_color & 0xFFu) * alpha) +
                               ((dest & 0xFFu) * inv) + 127u) / 255u;
                    output = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
            drew |= gdi_write_dc_pixel(dc, x + dx, y + dy, output);
        }
    }
    if (drew) mark_dc_dirty(dc);
    return drew;
}

static HBITMAP gdi_create_dibitmap_impl(HDC hdc, PVOID pbmih, DWORD flInit,
                                         PVOID pjBits, PVOID pbmi,
                                         UINT iUsage);

HBITMAP WINAPI CreateDIBitmap(HDC hdc, PVOID pbmih, DWORD flInit,
                              PVOID pjBits, PVOID pbmi, UINT iUsage)
{
    return gdi_create_dibitmap_impl(hdc, pbmih, flInit, pjBits, pbmi,
                                    iUsage);
}

/* ── Object selection / deletion ─────────────────────────────── */

HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ h)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return h; /* unknown DC — passthrough */

    if (h == STOCK_BITMAP) {
        HGDIOBJ prev = dc->prev_bitmap;
        dc->surface = NULL;
        dc->format = NULL;
        dc->bottomup = 0;
        dc->prev_bitmap = STOCK_BITMAP;
        return prev;
    }

    /* Selecting a bitmap into the DC */
    if (IS_BMP_HANDLE(h)) {
        GDI_BITMAP *bmp = bmp_from_handle((HBITMAP)h);
        if (bmp) {
            HGDIOBJ prev = dc->prev_bitmap;
            dc->surface     = bmp->pixels;
            dc->width       = bmp->width;
            dc->height      = bmp->height;
            dc->bpp         = bmp->bpp;
            dc->pitch       = bmp->pitch;
            dc->bottomup    = bmp->bottomup;
            dc->format      = &bmp->format;
            dc->prev_bitmap = h;
            return prev ? prev : h;
        }
    }

    if (font_from_handle(h, NULL, TRUE) || IS_STOCK_FONT_HANDLE(h)) {
        HGDIOBJ prev = dc->current_font;
        dc->current_font = h;
        return prev ? prev : STOCK_SYSTEM_FONT;
    }
    if (IS_FONT_HANDLE(h)) return NULL;

    /* Selection needs references, not large brush/pen pattern snapshots. */
    if (brush_acquire(h, dc, NULL)) {
        HGDIOBJ previous = dc->current_brush;
        dc->current_brush = h;
        brush_release(previous);
        return previous;
    }
    if (((ULONG_PTR)h & TAG_MASK) == BRUSH_TAG) return NULL;

    if (gdi_pen_acquire(h, dc, NULL)) {
        HGDIOBJ previous = dc->current_pen;
        dc->current_pen = h;
        gdi_pen_release(previous);
        return previous;
    }
    if (((ULONG_PTR)h & TAG_MASK) == PEN_TAG) return NULL;

    /* For brushes and pens, keep the existing passthrough behavior. */
    return h;
}

HGDIOBJ WINAPI GetCurrentObject(HDC hdc, UINT type)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return NULL;
    if (type == OBJ_BITMAP)
        return dc->prev_bitmap ? dc->prev_bitmap : STOCK_BITMAP;
    if (type == OBJ_FONT)
        return dc->current_font ? dc->current_font : STOCK_SYSTEM_FONT;
    if (type == OBJ_BRUSH) return dc->current_brush;
    if (type == 1 || type == 11) return dc->current_pen; /* OBJ_(EXT)PEN */
    if (type == OBJ_PAL) return dc->current_palette;
    return NULL;
}

static BOOL gdi_dc_object_selected(HGDIOBJ object)
{
    for (int i = 0; i < MAX_GDI_DCS; i++) {
        const GDI_DC *dc = &gdi_dcs[i];
        if (!dc->in_use) continue;
        if (dc->prev_bitmap == object || dc->current_font == object ||
            dc->current_palette == object) return TRUE;
        for (const GDI_SAVED_DC *saved = dc->saved; saved; saved = saved->previous)
            if (saved->dc.prev_bitmap == object ||
                saved->dc.current_font == object ||
                saved->dc.current_palette == object) return TRUE;
    }
    return FALSE;
}

static int WINAPI SaveDC(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || (dc->saved && dc->saved->level == 0x7FFFFFFF)) return 0;
    GDI_SAVED_DC *saved = kmalloc(sizeof(*saved));
    if (!saved) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    saved->dc = *dc;
    GDI_BRUSH brush;
    if (!brush_acquire(saved->dc.current_brush, dc, &brush)) {
        kfree(saved);
        return 0;
    }
    GDI_PEN pen;
    if (!gdi_pen_acquire(saved->dc.current_pen, dc, &pen)) {
        brush_release(saved->dc.current_brush);
        kfree(saved);
        return 0;
    }
    gdi32_region_data_retain(&saved->dc.clip_region);
    saved->previous = dc->saved;
    saved->level = dc->saved ? dc->saved->level + 1 : 1;
    dc->saved = saved;
    return saved->level;
}

static BOOL WINAPI RestoreDC(HDC hdc, int level)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !dc->saved || !level) return FALSE;
    int64_t target_level = level < 0 ? (int64_t)dc->saved->level + level + 1 : level;
    if (target_level <= 0 || target_level > dc->saved->level) return FALSE;
    GDI_SAVED_DC *target = dc->saved;
    while (target && target->level != target_level) target = target->previous;
    if (!target) return FALSE;

    /* Rebind a memory bitmap by its pinned handle. Surface addresses, window
     * geometry and nested BeginPaint ownership are never restored from a
     * stale snapshot: a window may have been resized since SaveDC. */
    if (!dc->pooled_window && !dc->is_screen && target->dc.prev_bitmap)
        SelectObject(hdc, target->dc.prev_bitmap);
    brush_release(dc->current_brush);
    gdi_pen_release(dc->current_pen);
#define RESTORE_ATTRIBUTE(field) dc->field = target->dc.field
    RESTORE_ATTRIBUTE(text_color);
    RESTORE_ATTRIBUTE(bk_color);
    RESTORE_ATTRIBUTE(brush_color);
    RESTORE_ATTRIBUTE(pen_color);
    RESTORE_ATTRIBUTE(poly_fill_mode);
    RESTORE_ATTRIBUTE(rop2);
    RESTORE_ATTRIBUTE(arc_direction);
    RESTORE_ATTRIBUTE(current_pen);
    RESTORE_ATTRIBUTE(bk_mode);
    RESTORE_ATTRIBUTE(text_align);
    RESTORE_ATTRIBUTE(current_x);
    RESTORE_ATTRIBUTE(current_y);
    RESTORE_ATTRIBUTE(map_mode);
    RESTORE_ATTRIBUTE(stretch_mode);
    RESTORE_ATTRIBUTE(viewport_x);
    RESTORE_ATTRIBUTE(viewport_y);
    RESTORE_ATTRIBUTE(window_x);
    RESTORE_ATTRIBUTE(window_y);
    RESTORE_ATTRIBUTE(window_cx);
    RESTORE_ATTRIBUTE(window_cy);
    RESTORE_ATTRIBUTE(viewport_cx);
    RESTORE_ATTRIBUTE(viewport_cy);
    RESTORE_ATTRIBUTE(brush_x);
    RESTORE_ATTRIBUTE(brush_y);
    RESTORE_ATTRIBUTE(graphics_mode);
    RESTORE_ATTRIBUTE(current_font);
    RESTORE_ATTRIBUTE(current_brush);
    RESTORE_ATTRIBUTE(current_palette);
    RESTORE_ATTRIBUTE(palette_background);
    RESTORE_ATTRIBUTE(clip_active);
#undef RESTORE_ATTRIBUTE
    gdi_memcpy(dc->world_transform, target->dc.world_transform,
               sizeof(dc->world_transform));
    gdi32_region_data_assign(&dc->clip_region, &target->dc.clip_region);
    /* Transfer the target's brush reference to the live DC; discard every
     * state above it, including the restored state itself. */
    while (dc->saved) {
        GDI_SAVED_DC *saved = dc->saved;
        BOOL done = saved == target;
        dc->saved = saved->previous;
        if (!done) brush_release(saved->dc.current_brush);
        if (!done) gdi_pen_release(saved->dc.current_pen);
        gdi32_region_data_clear(&saved->dc.clip_region);
        kfree(saved);
        if (done) break;
    }
    return TRUE;
}

static HPALETTE WINAPI CreatePalette(const GDI_LOGPALETTE *request)
{
    if (!request || request->version != 0x300 || !request->count) return NULL;
    SIZE_T bytes = (SIZE_T)request->count * sizeof(GDI_PALETTE_ENTRY);
    GDI_PALETTE *palette = kmalloc(sizeof(*palette) + bytes);
    if (!palette) return NULL;
    palette->count = request->count;
    palette->owner_pid = GetCurrentProcessId();
    gdi_memcpy(palette->entries, request->entries, bytes);
    uint64_t flags = brushes_lock();
    if (next_palette_id > 0xFFFFFFu) {
        brushes_unlock(flags);
        kfree(palette);
        return NULL;
    }
    palette->handle = (HPALETTE)(ULONG_PTR)(PALETTE_TAG | next_palette_id++);
    palette->next = gdi_palettes;
    gdi_palettes = palette;
    brushes_unlock(flags);
    return palette->handle;
}

static UINT WINAPI GetPaletteEntries(HPALETTE handle, UINT start, UINT count,
                                       GDI_PALETTE_ENTRY *entries)
{
    uint64_t flags = brushes_lock();
    GDI_PALETTE *palette = palette_find_locked(handle);
    UINT available = palette ? palette->count :
        handle == STOCK_DEFAULT_PALETTE ? 20 : 0;
    if (!entries) count = available;
    else if (start >= available) count = 0;
    else {
        if (count > available - start) count = available - start;
        if (palette) gdi_memcpy(entries, palette->entries + start,
                                 (SIZE_T)count * sizeof(*entries));
        else for (UINT i = 0; i < count; i++) {
            DWORD color = gdi_default_palette[start + i];
            entries[i] = (GDI_PALETTE_ENTRY){(BYTE)color, (BYTE)(color >> 8),
                                            (BYTE)(color >> 16), 0};
        }
    }
    brushes_unlock(flags);
    return count;
}

static UINT WINAPI SetPaletteEntries(HPALETTE handle, UINT start, UINT count,
                                       const GDI_PALETTE_ENTRY *entries)
{
    if (!entries) return 0;
    uint64_t flags = brushes_lock();
    GDI_PALETTE *palette = palette_find_locked(handle);
    if (!palette || start >= palette->count) count = 0;
    else {
        if (count > palette->count - start) count = palette->count - start;
        gdi_memcpy(palette->entries + start, entries,
                    (SIZE_T)count * sizeof(*entries));
    }
    brushes_unlock(flags);
    return count;
}

static HPALETTE WINAPI SelectPalette(HDC hdc, HPALETTE handle, BOOL background)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return NULL;
    uint64_t flags = brushes_lock();
    BOOL valid = handle == STOCK_DEFAULT_PALETTE || palette_find_locked(handle);
    HPALETTE previous = NULL;
    if (valid) {
        previous = dc->current_palette;
        dc->current_palette = handle;
        dc->palette_background = background;
    }
    brushes_unlock(flags);
    return previous;
}

static UINT WINAPI RealizePalette(HDC hdc)
{
    /* The display is true color (RASTERCAPS excludes RC_PALETTE), so no
     * physical system-palette entries change. Logical PALETTEINDEX colors
     * still resolve through the selected palette when pixels are written. */
    return dc_from_handle(hdc) ? 0 : 0xFFFFFFFFu;
}

static BOOL WINAPI UpdateColors(HDC hdc)
{
    /* No physical palette is exposed by this true-color display. */
    SetLastError(dc_from_handle(hdc) ? 50 : 6);
    return FALSE;
}

static int WINAPI gdi_printer_operation(HDC hdc)
{
    /* Every DC currently exposed by GDI is a display or memory DC. None is
     * a spooler/printer DC, so a print job cannot be started or advanced. */
    (void)hdc;
    SetLastError(6); /* ERROR_INVALID_HANDLE; SP_ERROR */
    return -1;
}

static int WINAPI StartDocA(HDC hdc, PCVOID document)
{
    (void)document;
    return gdi_printer_operation(hdc);
}

static BOOL gdi_resolve_color(const GDI_DC *dc, DWORD color, DWORD *rgb)
{
    if ((color & 0xFF000000u) == 0x01000000u) { /* PALETTEINDEX */
        GDI_PALETTE_ENTRY entry;
        if (!GetPaletteEntries(dc->current_palette, color & 0xFFFFu, 1, &entry))
            return FALSE;
        if (entry.flags & 2) { /* PC_EXPLICIT: low word is a system index. */
            UINT index = entry.red | ((UINT)entry.green << 8);
            if (index >= 20) return FALSE;
            *rgb = gdi_default_palette[index];
        } else *rgb = entry.red | ((DWORD)entry.green << 8) | ((DWORD)entry.blue << 16);
    } else *rgb = color & 0xFFFFFFu; /* RGB / PALETTERGB on a true-color device */
    return TRUE;
}


static void region_normalize_rect(GDI_RECT *rect)
{
    if (rect->left > rect->right) {
        LONG value = rect->left;
        rect->left = rect->right;
        rect->right = value;
    }
    if (rect->top > rect->bottom) {
        LONG value = rect->top;
        rect->top = rect->bottom;
        rect->bottom = value;
    }
}

static BOOL region_rect_empty(const GDI_RECT *rect)
{
    return rect->left >= rect->right || rect->top >= rect->bottom;
}

static BOOL region_rect_contains(const GDI_RECT *outer,
                                 const GDI_RECT *inner)
{
    return outer->left <= inner->left && outer->top <= inner->top &&
           outer->right >= inner->right && outer->bottom >= inner->bottom;
}

static BOOL region_intersect_rects(const GDI_RECT *first,
                                   const GDI_RECT *second,
                                   GDI_RECT *result)
{
    result->left = first->left > second->left ? first->left : second->left;
    result->top = first->top > second->top ? first->top : second->top;
    result->right = first->right < second->right
        ? first->right : second->right;
    result->bottom = first->bottom < second->bottom
        ? first->bottom : second->bottom;
    return !region_rect_empty(result);
}

static void region_remove_rect(GDI_REGION *region, UINT index)
{
    if (index >= region->count) return;
    GDI_RECT *rects = GDI_REGION_RECTS(region);
    for (UINT i = index + 1; i < region->count; i++)
        rects[i - 1] = rects[i];
    region->count--;
}

static BOOL region_append_rect(GDI_REGION *region, GDI_RECT rect)
{
    region->polygon_type = 0;
    region_normalize_rect(&rect);
    if (region_rect_empty(&rect)) return TRUE;
    if (!region_reserve(region, region->count)) return FALSE;

    for (UINT i = 0; i < region->count; ) {
        GDI_RECT *current = &GDI_REGION_RECTS(region)[i];
        if (region_rect_contains(current, &rect)) return TRUE;
        if (region_rect_contains(&rect, current)) {
            region_remove_rect(region, i);
            continue;
        }

        if (current->top == rect.top && current->bottom == rect.bottom &&
            current->left <= rect.right && rect.left <= current->right) {
            if (current->left < rect.left) rect.left = current->left;
            if (current->right > rect.right) rect.right = current->right;
            region_remove_rect(region, i);
            continue;
        }
        if (current->left == rect.left && current->right == rect.right &&
            current->top <= rect.bottom && rect.top <= current->bottom) {
            if (current->top < rect.top) rect.top = current->top;
            if (current->bottom > rect.bottom) rect.bottom = current->bottom;
            region_remove_rect(region, i);
            continue;
        }
        i++;
    }

    if (region->count == 0xFFFFFFFFu ||
        !region_reserve(region, region->count + 1)) return FALSE;
    GDI_REGION_RECTS(region)[region->count++] = rect;
    return TRUE;
}

static BOOL region_subtract_rect(const GDI_RECT *source,
                                 const GDI_RECT *cut,
                                 GDI_REGION *result)
{
    GDI_RECT overlap;
    if (!region_intersect_rects(source, cut, &overlap))
        return region_append_rect(result, *source);

    GDI_RECT piece = {
        source->left, source->top, source->right, overlap.top
    };
    if (!region_append_rect(result, piece)) return FALSE;
    piece = (GDI_RECT){
        source->left, overlap.bottom, source->right, source->bottom
    };
    if (!region_append_rect(result, piece)) return FALSE;
    piece = (GDI_RECT){
        source->left, overlap.top, overlap.left, overlap.bottom
    };
    if (!region_append_rect(result, piece)) return FALSE;
    piece = (GDI_RECT){
        overlap.right, overlap.top, source->right, overlap.bottom
    };
    return region_append_rect(result, piece);
}

static BOOL region_difference(const GDI_REGION *first,
                              const GDI_REGION *second,
                              GDI_REGION *result)
{
    result->count = 0;
    for (UINT i = 0; i < first->count; i++) {
        GDI_REGION_SCOPE(pieces);
        if (!region_append_rect(&pieces, GDI_REGION_RECTS(first)[i])) return FALSE;

        for (UINT j = 0; j < second->count && pieces.count; j++) {
            GDI_RECT overlap;
            if (!region_intersect_rects(&GDI_REGION_RECTS(first)[i],
                                       &GDI_REGION_RECTS(second)[j], &overlap))
                continue;
            GDI_REGION_SCOPE(next);
            for (UINT k = 0; k < pieces.count; k++) {
                if (!region_subtract_rect(&GDI_REGION_RECTS(&pieces)[k],
                                          &GDI_REGION_RECTS(second)[j], &next))
                    return FALSE;
            }
            gdi32_region_data_move(&pieces, &next);
        }
        for (UINT j = 0; j < pieces.count; j++) {
            if (!region_append_rect(result, GDI_REGION_RECTS(&pieces)[j])) return FALSE;
        }
    }
    return TRUE;
}

static BOOL region_intersection(const GDI_REGION *first,
                                const GDI_REGION *second,
                                GDI_REGION *result)
{
    result->count = 0;
    for (UINT i = 0; i < first->count; i++) {
        for (UINT j = 0; j < second->count; j++) {
            GDI_RECT overlap;
            if (region_intersect_rects(&GDI_REGION_RECTS(first)[i], &GDI_REGION_RECTS(second)[j],
                                       &overlap) &&
                !region_append_rect(result, overlap))
                return FALSE;
        }
    }
    return TRUE;
}

static BOOL region_union(const GDI_REGION *first,
                         const GDI_REGION *second,
                         GDI_REGION *result)
{
    result->count = 0;
    GDI_RECT bounds;
    if (first->count == 1 &&
        gdi32_region_data_bounds(second, &bounds) &&
        region_rect_contains(&GDI_REGION_RECTS(first)[0], &bounds))
        return region_append_rect(result, GDI_REGION_RECTS(first)[0]);
    if (second->count == 1 &&
        gdi32_region_data_bounds(first, &bounds) &&
        region_rect_contains(&GDI_REGION_RECTS(second)[0], &bounds))
        return region_append_rect(result, GDI_REGION_RECTS(second)[0]);
    for (UINT i = 0; i < first->count; i++) {
        if (!region_append_rect(result, GDI_REGION_RECTS(first)[i])) return FALSE;
    }

    GDI_REGION_SCOPE(unique);
    if (!region_difference(second, first, &unique)) return FALSE;
    for (UINT i = 0; i < unique.count; i++) {
        if (!region_append_rect(result, GDI_REGION_RECTS(&unique)[i])) return FALSE;
    }
    return TRUE;
}

static BOOL region_combine(const GDI_REGION *first,
                           const GDI_REGION *second,
                           int mode, GDI_REGION *result)
{
    gdi32_region_data_clear(result);
    result->in_use = 1;
    switch (mode) {
    case GDI_RGN_AND:
        return region_intersection(first, second, result);
    case GDI_RGN_OR:
        return region_union(first, second, result);
    case GDI_RGN_XOR: {
        GDI_REGION_SCOPE(left);
        GDI_REGION_SCOPE(right);
        if (!region_difference(first, second, &left) ||
            !region_difference(second, first, &right))
            return FALSE;
        return region_union(&left, &right, result);
    }
    case GDI_RGN_DIFF:
        return region_difference(first, second, result);
    case GDI_RGN_COPY:
        gdi32_region_data_assign(result, first);
        result->in_use = 1;
        return TRUE;
    default:
        return FALSE;
    }
}

static int region_type(const GDI_REGION *region)
{
    if (region->polygon_type) return region->polygon_type;
    if (!region->count) return GDI_NULLREGION;
    return region->count == 1 ? GDI_SIMPLEREGION : GDI_COMPLEXREGION;
}

BOOL gdi32_region_data_read(HGDIOBJ handle, GDI_REGION *data)
{
    if (!data) return FALSE;
    regions_lock();
    GDI_REGION *region = region_from_handle(handle);
    if (region) gdi32_region_data_assign(data, region);
    regions_unlock();
    if (!region) SetLastError(6);
    return region != NULL;
}

int gdi32_region_data_write(HGDIOBJ handle, const GDI_REGION *data)
{
    if (!data) return GDI_RGN_ERROR;
    regions_lock();
    GDI_REGION *region = region_from_handle(handle);
    if (region) {
        gdi32_region_data_assign(region, data);
        region->in_use = 1;
    }
    regions_unlock();
    if (!region) {
        SetLastError(6);
        return GDI_RGN_ERROR;
    }
    return region_type(data);
}

int gdi32_region_data_combine(GDI_REGION *destination,
                              const GDI_REGION *first,
                              const GDI_REGION *second, int mode)
{
    if (!destination || !first || !second ||
        mode < GDI_RGN_AND || mode > GDI_RGN_COPY) {
        SetLastError(87);
        return GDI_RGN_ERROR;
    }
    GDI_REGION_SCOPE(result);
    if (!region_combine(first, second, mode, &result)) {
        SetLastError(8);
        return GDI_RGN_ERROR;
    }
    gdi32_region_data_move(destination, &result);
    return region_type(destination);
}

int gdi32_region_data_bounds(const GDI_REGION *data, GDI_RECT *bounds)
{
    if (!data || !bounds)
        return GDI_RGN_ERROR;
    if (data->polygon_type) {
        *bounds = data->polygon_bounds;
        return data->polygon_type;
    }
    *bounds = (GDI_RECT){0,0,0,0};
    if (!data->count) return GDI_NULLREGION;
    *bounds = GDI_REGION_RECTS(data)[0];
    for (UINT i = 1; i < data->count; i++) {
        const GDI_RECT *part = &GDI_REGION_RECTS(data)[i];
        if (part->left < bounds->left) bounds->left = part->left;
        if (part->top < bounds->top) bounds->top = part->top;
        if (part->right > bounds->right) bounds->right = part->right;
        if (part->bottom > bounds->bottom) bounds->bottom = part->bottom;
    }
    return region_type(data);
}

HGDIOBJ WINAPI CreateRectRgn(int left, int top, int right, int bottom)
{
    regions_lock();
    int index = alloc_region();
    if (index < 0) {
        regions_unlock();
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }
    GDI_RECT rect = { left, top, right, bottom };
    if (!region_append_rect(&gdi_regions[index], rect)) {
        gdi32_region_data_clear(&gdi_regions[index]);
        gdi_region_owners[index] = 0;
        regions_unlock();
        SetLastError(8);
        return NULL;
    }
    HGDIOBJ handle = (HGDIOBJ)(ULONG_PTR)(REGION_TAG | (unsigned)index);
    regions_unlock();
    return handle;
}

HGDIOBJ WINAPI CreateRectRgnIndirect(const GDI_RECT *rect)
{
    if (!rect) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }
    return CreateRectRgn(rect->left, rect->top, rect->right, rect->bottom);
}

typedef struct {
    LONG x, top, bottom, dx, dy;
    int winding;
} GDI_POLYGON_EDGE;

typedef struct { LONG x; int winding; } GDI_POLYGON_CROSSING;

/* Native GDI converts integer vertices to signed 28-bit device coordinates.
 * Keep the conversion explicit: neither left-shifting a negative LONG nor
 * overflowing a signed subtraction is defined C. */
static LONG polygon_coordinate(LONG coordinate)
{
    ULONG bits = (ULONG)coordinate;
    return (LONG)(bits & 0x07FFFFFFu) - (LONG)(bits & 0x08000000u);
}

static void polygon_sift_crossings(GDI_POLYGON_CROSSING *crossings,
                                    UINT root, UINT count)
{
    while (root < count / 2) {
        UINT child = root * 2 + 1;
        if (child + 1 < count && crossings[child].x < crossings[child + 1].x)
            child++;
        if (crossings[root].x >= crossings[child].x) break;
        GDI_POLYGON_CROSSING swap = crossings[root];
        crossings[root] = crossings[child];
        crossings[child] = swap;
        root = child;
    }
}

static void polygon_sort_crossings(GDI_POLYGON_CROSSING *crossings, UINT count)
{
    for (UINT i = count / 2; i > 0; i--)
        polygon_sift_crossings(crossings, i - 1, count);
    for (UINT i = count; i > 1; i--) {
        GDI_POLYGON_CROSSING swap = crossings[0];
        crossings[0] = crossings[i - 1];
        crossings[i - 1] = swap;
        polygon_sift_crossings(crossings, 0, i - 1);
    }
}

/* Bands arrive in increasing y order and contain disjoint sorted spans.
 * Coalesce identical adjacent bands in O(number of spans), without scanning
 * every earlier rectangle for every row of a diagonal polygon. */
static BOOL polygon_append_band(GDI_REGION *region, const GDI_RECT *spans,
                                  UINT count, UINT *previous, UINT *previous_count)
{
    if (!count) { *previous_count = 0; return TRUE; }
    GDI_RECT *rects = GDI_REGION_RECTS(region);
    BOOL same = *previous_count == count;
    for (UINT i = 0; same && i < count; i++)
        same = rects[*previous + i].left == spans[i].left &&
               rects[*previous + i].right == spans[i].right &&
               rects[*previous + i].bottom == spans[i].top;
    if (same) {
        for (UINT i = 0; i < count; i++) rects[*previous + i].bottom = spans[i].bottom;
        return TRUE;
    }
    if (count > 0xFFFFFFFFu - region->count ||
        !region_reserve(region, region->count + count)) return FALSE;
    *previous = region->count;
    *previous_count = count;
    gdi_memcpy(GDI_REGION_RECTS(region) + region->count, spans,
                   (SIZE_T)count * sizeof(*spans));
    region->count += count;
    return TRUE;
}

static BOOL polygon_is_collinear(const GDI_POINT *points, int count)
{
    GDI_POINT origin = {polygon_coordinate(points[0].x), polygon_coordinate(points[0].y)};
    LONG dx = 0, dy = 0;
    for (int i = 1; i < count; i++) {
        LONG x = polygon_coordinate(points[i].x) - origin.x;
        LONG y = polygon_coordinate(points[i].y) - origin.y;
        if (!dx && !dy) { dx = x; dy = y; }
        else if ((int64_t)x * dy != (int64_t)y * dx) return FALSE;
    }
    return TRUE;
}

static HGDIOBJ WINAPI CreatePolyPolygonRgn(const GDI_POINT *points,
                                            const int *counts, int polygons, int mode)
{
    if (!points || !counts || !polygons || (mode != 1 && mode != 2)) return NULL;
    if (polygons < 0) { SetLastError(87); return NULL; }
    UINT vertices = 0;
    for (int i = 0; i < polygons; i++) {
        if (counts[i] < 2) { SetLastError(87); return NULL; }
        if ((UINT)counts[i] > 0x7FFFFFFFu - vertices) { SetLastError(8); return NULL; }
        vertices += (UINT)counts[i];
    }
    SIZE_T bytes;
    if (__builtin_mul_overflow((SIZE_T)vertices,
            sizeof(GDI_POLYGON_EDGE) + sizeof(GDI_POLYGON_CROSSING) + sizeof(GDI_RECT), &bytes)) {
        SetLastError(8);
        return NULL;
    }
    GDI_POLYGON_EDGE *edges = kmalloc(bytes);
    if (!edges) { SetLastError(8); return NULL; }
    GDI_POLYGON_CROSSING *crossings = (GDI_POLYGON_CROSSING *)(edges + vertices);
    GDI_RECT *spans = (GDI_RECT *)(crossings + vertices);
    UINT edge_count = 0, offset = 0;
    LONG first_y = 0, last_y = 0;
    for (int polygon = 0; polygon < polygons; polygon++) {
        int count = counts[polygon];
        if (polygons != 1 || !polygon_is_collinear(points + offset, count)) {
            for (int i = 0; i < count; i++) {
                GDI_POINT a = points[offset + i];
                GDI_POINT b = points[offset + (i + 1 == count ? 0 : i + 1)];
                a.x = polygon_coordinate(a.x); a.y = polygon_coordinate(a.y);
                b.x = polygon_coordinate(b.x); b.y = polygon_coordinate(b.y);
                if (a.y == b.y) continue;
                int winding = a.y < b.y ? 1 : -1;
                if (a.y > b.y) { GDI_POINT swap = a; a = b; b = swap; }
                if (!edge_count || a.y < first_y) first_y = a.y;
                if (!edge_count || b.y > last_y) last_y = b.y;
                edges[edge_count++] = (GDI_POLYGON_EDGE){a.x, a.y, b.y,
                                                      b.x - a.x, b.y - a.y, winding};
            }
        }
        offset += (UINT)count;
    }
    GDI_REGION_SCOPE(result);
    UINT previous = 0, previous_count = 0;
    UINT empty_bands = 0;
    LONG first_empty_y = 0;
    BOOL monotone = polygons == 1;
    BOOL ok = TRUE;
    for (LONG y = first_y; edge_count && y < last_y; ) {
        UINT crossing_count = 0;
        LONG next_y = last_y;
        for (UINT i = 0; i < edge_count; i++) {
            const GDI_POLYGON_EDGE *edge = &edges[i];
            if (y < edge->top) {
                if (edge->top < next_y) next_y = edge->top;
                continue;
            }
            if (y >= edge->bottom) continue;
            if (edge->bottom < next_y) next_y = edge->bottom;
            int64_t numerator = (int64_t)edge->x * edge->dy +
                                (int64_t)(y - edge->top) * edge->dx;
            LONG x = (LONG)(numerator >= 0 ? (numerator + edge->dy - 1) / edge->dy
                                          : numerator / edge->dy);
            crossings[crossing_count++] = (GDI_POLYGON_CROSSING){x, edge->winding};
            /* Jump to the first row whose rounded crossing changes. Vertical
             * and almost vertical edges can cover millions of rows in one band. */
            int64_t rows = next_y - y;
            if (edge->dx > 0) rows = ((int64_t)x * edge->dy - numerator) / edge->dx + 1;
            if (edge->dx < 0) rows = (numerator - ((int64_t)x - 1) * edge->dy +
                                      (-(int64_t)edge->dx) - 1) / (-(int64_t)edge->dx);
            if (rows < (int64_t)next_y - y) next_y = y + (LONG)rows;
        }
        if (crossing_count > 2) monotone = FALSE;
        /* A gap between disconnected contours contributes to native region
         * metadata, but has no pixels and must not enter the rectangle list.
         * Coincident edges that cancel are not such a gap. */
        if (!crossing_count) {
            if (!empty_bands) first_empty_y = y;
            if (empty_bands < 2) empty_bands++;
        }
        polygon_sort_crossings(crossings, crossing_count);
        UINT span_count = 0;
        int winding = 0;
        LONG start = 0;
        for (UINT i = 0; i < crossing_count; ) {
            LONG x = crossings[i].x;
            BOOL was_inside = mode == 1 ? (winding & 1) != 0 : winding != 0;
            do { winding += crossings[i++].winding; }
            while (i < crossing_count && crossings[i].x == x);
            BOOL inside = mode == 1 ? (winding & 1) != 0 : winding != 0;
            if (!was_inside && inside) start = x;
            if (was_inside && !inside && start < x)
                spans[span_count++] = (GDI_RECT){start, y, x, next_y};
        }
        if (!polygon_append_band(&result, spans, span_count, &previous, &previous_count)) {
            ok = FALSE;
            break;
        }
        y = next_y;
    }
    kfree(edges);
    if (!ok) { SetLastError(8); return NULL; }
    if (edge_count) {
        /* Native GDI keeps the final edge boundary for the general scan
         * path. A single y-monotone polygon instead has tight pixel bounds.
         * This metadata is independent of band coalescing and row skipping;
         * neither must change a region's queried bounds. */
        GDI_RECT bounds;
        int type = gdi32_region_data_bounds(&result, &bounds);
        if (!result.count) { bounds.top = last_y; type = GDI_SIMPLEREGION; }
        if (empty_bands) {
            if (first_empty_y < bounds.top) bounds.top = first_empty_y;
            if (result.count || empty_bands > 1) type = GDI_COMPLEXREGION;
        }
        if (!monotone || !result.count) bounds.bottom = last_y;
        result.polygon_bounds = bounds;
        result.polygon_type = type;
    }
    regions_lock();
    int index = alloc_region();
    if (index >= 0) {
        gdi32_region_data_move(&gdi_regions[index], &result);
        gdi_regions[index].in_use = 1;
    }
    regions_unlock();
    if (index < 0) { SetLastError(8); return NULL; }
    return (HGDIOBJ)(ULONG_PTR)(REGION_TAG | (unsigned)index);
}

static HGDIOBJ WINAPI CreatePolygonRgn(const GDI_POINT *points, int count, int mode)
{
    if (count < 0) return NULL;
    /* Unlike a NULL vertex pointer with a positive count, zero/one vertices
     * are rejected with ERROR_INVALID_PARAMETER by native GDI. */
    if (count < 2 && (mode == 1 || mode == 2)) { SetLastError(87); return NULL; }
    return CreatePolyPolygonRgn(points, &count, 1, mode);
}

BOOL WINAPI SetRectRgn(HGDIOBJ rgn, int left, int top, int right, int bottom)
{
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    if (!region) {
        regions_unlock();
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    gdi32_region_data_clear(region);
    region->in_use = 1;
    GDI_RECT rect = { left, top, right, bottom };
    BOOL result = region_append_rect(region, rect);
    regions_unlock();
    return result;
}

int WINAPI CombineRgn(HGDIOBJ dest_handle, HGDIOBJ first_handle,
                      HGDIOBJ second_handle, int mode)
{
    regions_lock();
    GDI_REGION *dest = region_from_handle(dest_handle);
    GDI_REGION *first = region_from_handle(first_handle);
    GDI_REGION *second = mode == GDI_RGN_COPY
        ? first : region_from_handle(second_handle);
    if (!dest || !first || !second || mode < GDI_RGN_AND ||
        mode > GDI_RGN_COPY) {
        regions_unlock();
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return GDI_RGN_ERROR;
    }

    GDI_REGION_SCOPE(result);
    if (!region_combine(first, second, mode, &result)) {
        regions_unlock();
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return GDI_RGN_ERROR;
    }
    gdi32_region_data_move(dest, &result);
    int type = region_type(dest);
    regions_unlock();
    return type;
}

BOOL WINAPI EqualRgn(HGDIOBJ first_handle, HGDIOBJ second_handle)
{
    regions_lock();
    GDI_REGION *first = region_from_handle(first_handle);
    GDI_REGION *second = region_from_handle(second_handle);
    GDI_REGION_SCOPE(difference);
    if (!first || !second ||
        !region_combine(first, second, GDI_RGN_XOR, &difference)) {
        regions_unlock();
        return FALSE;
    }
    BOOL equal = difference.count == 0;
    regions_unlock();
    return equal;
}

BOOL WINAPI PtInRegion(HGDIOBJ rgn, int x, int y)
{
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    if (!region) {
        regions_unlock();
        return FALSE;
    }
    for (UINT i = 0; i < region->count; i++) {
        const GDI_RECT *rect = &GDI_REGION_RECTS(region)[i];
        if (x >= rect->left && x < rect->right &&
            y >= rect->top && y < rect->bottom) {
            regions_unlock();
            return TRUE;
        }
    }
    regions_unlock();
    return FALSE;
}

BOOL WINAPI RectInRegion(HGDIOBJ rgn, const GDI_RECT *input)
{
    if (!input) return FALSE;
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    if (!region) {
        regions_unlock();
        return FALSE;
    }
    GDI_RECT rect = *input;
    region_normalize_rect(&rect);
    for (UINT i = 0; i < region->count; i++) {
        GDI_RECT overlap;
        if (region_intersect_rects(&GDI_REGION_RECTS(region)[i], &rect, &overlap)) {
            regions_unlock();
            return TRUE;
        }
    }
    regions_unlock();
    return FALSE;
}

/* RGNDATA requires ordered, disjoint rectangles. Internal region snapshots
 * may have different decompositions, so serialize their exact union into
 * horizontal bands without changing the shared source or its bounds. */
static BOOL region_pair_less(GDI_POINT a, GDI_POINT b)
{
    return a.x < b.x || (a.x == b.x && a.y < b.y);
}

static void region_pair_sift(GDI_POINT *pairs, UINT root, UINT count)
{
    while (count > 1 && root <= (count - 2) / 2) {
        UINT child = root * 2 + 1;
        if (child + 1 < count && region_pair_less(pairs[child], pairs[child + 1])) child++;
        if (!region_pair_less(pairs[root], pairs[child])) break;
        GDI_POINT value = pairs[root]; pairs[root] = pairs[child]; pairs[child] = value;
        root = child;
    }
}

static void region_pair_sort(GDI_POINT *pairs, UINT count)
{
    for (UINT i = count / 2; i; i--) region_pair_sift(pairs, i - 1, count);
    for (UINT i = count; i > 1; i--) {
        GDI_POINT value = pairs[0]; pairs[0] = pairs[i - 1]; pairs[i - 1] = value;
        region_pair_sift(pairs, 0, i - 1);
    }
}

static BOOL region_canonicalize(const GDI_REGION *source, GDI_REGION *result)
{
    if (!source->count) return TRUE;
    if (source->count > UINT32_MAX / 3) return FALSE;
    GDI_POINT *edges = kmalloc((SIZE_T)source->count * 3 * sizeof(*edges));
    if (!edges) return FALSE;
    GDI_POINT *spans = edges + source->count * 2;
    const GDI_RECT *rects = GDI_REGION_RECTS(source);
    UINT edge_count = 0;
    for (UINT i = 0; i < source->count; i++) {
        if (region_rect_empty(&rects[i])) continue;
        edges[edge_count++] = (GDI_POINT){rects[i].top, 0};
        edges[edge_count++] = (GDI_POINT){rects[i].bottom, 0};
    }
    region_pair_sort(edges, edge_count);
    UINT unique = 0;
    for (UINT i = 0; i < edge_count; i++)
        if (!unique || edges[i].x != edges[unique - 1].x) edges[unique++] = edges[i];
    UINT previous = 0, previous_count = 0;
    BOOL success = TRUE;
    for (UINT band = 1; band < unique; band++) {
        LONG top = edges[band - 1].x, bottom = edges[band].x;
        UINT count = 0;
        for (UINT i = 0; i < source->count; i++) {
            if (rects[i].top <= top && rects[i].bottom >= bottom && rects[i].left < rects[i].right)
                spans[count++] = (GDI_POINT){rects[i].left, rects[i].right};
        }
        region_pair_sort(spans, count);
        UINT merged = 0;
        for (UINT i = 0; i < count; i++) {
            if (merged && spans[i].x <= spans[merged - 1].y) {
                if (spans[i].y > spans[merged - 1].y) spans[merged - 1].y = spans[i].y;
            } else spans[merged++] = spans[i];
        }
        BOOL same = merged && merged == previous_count;
        for (UINT i = 0; same && i < merged; i++) {
            GDI_RECT *old = &GDI_REGION_RECTS(result)[previous + i];
            same = old->left == spans[i].x && old->right == spans[i].y && old->bottom == top;
        }
        if (same) {
            for (UINT i = 0; i < merged; i++) GDI_REGION_RECTS(result)[previous + i].bottom = bottom;
            continue;
        }
        previous = result->count; previous_count = merged;
        if (merged > (UINT32_MAX - 32) / sizeof(GDI_RECT) - result->count ||
            !region_reserve(result, result->count + merged)) { success = FALSE; break; }
        for (UINT i = 0; i < merged; i++)
            GDI_REGION_RECTS(result)[result->count++] = (GDI_RECT){spans[i].x, top, spans[i].y, bottom};
    }
    kfree(edges);
    return success;
}

DWORD WINAPI GetRegionData(HGDIOBJ rgn, DWORD bytes, PVOID data)
{
    GDI_REGION_SCOPE(source);
    GDI_REGION_SCOPE(canonical);
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    if (region) gdi32_region_data_assign(&source, region);
    regions_unlock();
    /* Windows' PE32 wrapper preserves LastError for invalid handle/length;
     * PE64 sets the documented error. The buffer stays untouched in both. */
    if (!region) { if (!g_compat32_mode) SetLastError(6); return 0; }
    if (!region_canonicalize(&source, &canonical)) { SetLastError(8); return 0; }
    struct { DWORD size, type, count, rectangle_bytes; GDI_RECT bounds; } header =
        {32, 1, canonical.count, canonical.count * sizeof(GDI_RECT), {0,0,0,0}};
    _Static_assert(sizeof(header) == 32, "RGNDATAHEADER ABI");
    DWORD required = 32 + header.rectangle_bytes;
    if (!data) return required;
    if (bytes < required) { if (!g_compat32_mode) SetLastError(87); return 0; }
#ifdef __KERNEL_X86__
    if (!win32_user_range_writable(data, required, g_compat32_mode)) {
        SetLastError(87); return 0;
    }
#endif
    /* A cancelled polygon may retain an empty GetRgnBox cache at its final
     * edge, while the serialized empty region has the canonical zero box. */
    if (canonical.count) gdi32_region_data_bounds(&source, &header.bounds);
    gdi_memcpy(data, &header, sizeof(header));
    gdi_memcpy((BYTE *)data + sizeof(header), GDI_REGION_RECTS(&canonical), header.rectangle_bytes);
    return required;
}

int WINAPI GetRgnBox(HGDIOBJ rgn, GDI_RECT *rect)
{
    if (!rect) return GDI_RGN_ERROR;
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    if (!region) {
        regions_unlock();
        return GDI_RGN_ERROR;
    }
    int type = gdi32_region_data_bounds(region, rect);
    regions_unlock();
    return type;
}

int WINAPI OffsetRgn(HGDIOBJ rgn, int x, int y)
{
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    if (!region) {
        regions_unlock();
        return GDI_RGN_ERROR;
    }
    if (!region_reserve(region, region->count)) {
        regions_unlock();
        SetLastError(8);
        return GDI_RGN_ERROR;
    }
    for (UINT i = 0; i < region->count; i++) {
        GDI_RECT *rect = &GDI_REGION_RECTS(region)[i];
        rect->left += x;
        rect->right += x;
        rect->top += y;
        rect->bottom += y;
    }
    if (region->count && region->polygon_type) {
        region->polygon_bounds.left += x;
        region->polygon_bounds.right += x;
        region->polygon_bounds.top += y;
        region->polygon_bounds.bottom += y;
    }
    int type = region_type(region);
    regions_unlock();
    return type;
}

int WINAPI SelectClipRgn(HDC hdc, HGDIOBJ rgn)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return GDI_RGN_ERROR;
    if (!rgn) {
        dc->clip_active = FALSE;
        gdi32_region_data_clear(&dc->clip_region);
        return GDI_SIMPLEREGION;
    }
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    int type = region ? region_type(region) : GDI_RGN_ERROR;
    if (region) {
        gdi32_region_data_assign(&dc->clip_region, region);
        dc->clip_active = TRUE;
    }
    regions_unlock();
    return type;
}

int gdi32_region_selftest(void)
{
    int checks = 0;
    int failures = 0;
#define REGION_CHECK(condition) do { \
    checks++; \
    if (!(condition)) failures++; \
} while (0)

    HGDIOBJ first = CreateRectRgn(0, 0, 10, 10);
    HGDIOBJ second = CreateRectRgn(5, 5, 15, 15);
    HGDIOBJ result = CreateRectRgn(0, 0, 0, 0);
    REGION_CHECK(first != NULL && second != NULL && result != NULL);
    REGION_CHECK(PtInRegion(first, 0, 0));
    REGION_CHECK(PtInRegion(first, 9, 9));
    REGION_CHECK(!PtInRegion(first, 10, 9));
    REGION_CHECK(!PtInRegion(first, 9, 10));

    GDI_RECT probe = { 9, 9, 12, 12 };
    REGION_CHECK(RectInRegion(first, &probe));
    probe = (GDI_RECT){ 10, 0, 12, 2 };
    REGION_CHECK(!RectInRegion(first, &probe));

    REGION_CHECK(CombineRgn(result, first, second, GDI_RGN_AND) ==
                 GDI_SIMPLEREGION);
    REGION_CHECK(PtInRegion(result, 5, 5));
    REGION_CHECK(!PtInRegion(result, 4, 5));
    REGION_CHECK(!PtInRegion(result, 10, 5));

    REGION_CHECK(CombineRgn(result, first, second, GDI_RGN_OR) ==
                 GDI_COMPLEXREGION);
    REGION_CHECK(PtInRegion(result, 1, 1));
    REGION_CHECK(PtInRegion(result, 14, 14));
    REGION_CHECK(!PtInRegion(result, 14, 1));
    GDI_RECT bounds;
    REGION_CHECK(GetRgnBox(result, &bounds) == GDI_COMPLEXREGION);
    REGION_CHECK(bounds.left == 0 && bounds.top == 0 &&
                 bounds.right == 15 && bounds.bottom == 15);

    REGION_CHECK(CombineRgn(result, first, second, GDI_RGN_DIFF) ==
                 GDI_COMPLEXREGION);
    REGION_CHECK(PtInRegion(result, 1, 1));
    REGION_CHECK(PtInRegion(result, 9, 1));
    REGION_CHECK(!PtInRegion(result, 5, 5));

    REGION_CHECK(CombineRgn(result, first, second, GDI_RGN_XOR) ==
                 GDI_COMPLEXREGION);
    REGION_CHECK(PtInRegion(result, 1, 1));
    REGION_CHECK(PtInRegion(result, 14, 14));
    REGION_CHECK(!PtInRegion(result, 7, 7));

    REGION_CHECK(CombineRgn(result, first, NULL, GDI_RGN_COPY) ==
                 GDI_SIMPLEREGION);
    REGION_CHECK(EqualRgn(result, first));
    REGION_CHECK(OffsetRgn(result, 2, -3) == GDI_SIMPLEREGION);
    REGION_CHECK(PtInRegion(result, 2, -3));
    REGION_CHECK(!PtInRegion(result, 1, -3));
    REGION_CHECK(CombineRgn(result, result, second, GDI_RGN_AND) ==
                 GDI_SIMPLEREGION);
    REGION_CHECK(PtInRegion(result, 5, 5));

    HGDIOBJ empty = CreateRectRgn(4, 4, 4, 9);
    REGION_CHECK(empty != NULL);
    REGION_CHECK(GetRgnBox(empty, &bounds) == GDI_NULLREGION);
    REGION_CHECK(bounds.left == 0 && bounds.top == 0 &&
                 bounds.right == 0 && bounds.bottom == 0);

    REGION_CHECK(DeleteObject(first));
    REGION_CHECK(!PtInRegion(first, 1, 1));
    REGION_CHECK(DeleteObject(second));
    REGION_CHECK(DeleteObject(result));
    REGION_CHECK(DeleteObject(empty));

    serial_puts("[GDI-REGION-TEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
#undef REGION_CHECK
    return failures;
}

UINT WINAPI SetTextAlign(HDC hdc, UINT align)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return 0xFFFFFFFFu;
    UINT prev = dc->text_align;
    dc->text_align = align;
    return prev;
}

UINT WINAPI GetTextAlign(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    return dc ? dc->text_align : 0xFFFFFFFFu;
}

BOOL WINAPI DeleteObject(HGDIOBJ ho)
{
    if (((ULONG_PTR)ho & TAG_MASK) == PEN_TAG) {
        uint64_t flags = brushes_lock();
        GDI_PEN **link = &gdi_pens;
        while (*link && (*link)->handle != ho) link = &(*link)->next;
        GDI_PEN *pen = *link;
        if (!pen || pen->owner_pid != GetCurrentProcessId() || pen->references) {
            brushes_unlock(flags);
            return FALSE;
        }
        *link = pen->next;
        brushes_unlock(flags);
        kfree(pen->pattern.pixels);
        kfree(pen);
        return TRUE;
    }
    if (((ULONG_PTR)ho & TAG_MASK) == PALETTE_TAG) {
        uint64_t flags = brushes_lock();
        GDI_PALETTE **link = &gdi_palettes;
        while (*link && (*link)->handle != ho) link = &(*link)->next;
        GDI_PALETTE *palette = *link;
        if (!palette || palette->owner_pid != GetCurrentProcessId() ||
            gdi_dc_object_selected(ho)) {
            brushes_unlock(flags);
            return FALSE;
        }
        *link = palette->next;
        brushes_unlock(flags);
        kfree(palette);
        return TRUE;
    }
    if (((ULONG_PTR)ho & TAG_MASK) == BRUSH_TAG) {
        uint64_t flags = brushes_lock();
        GDI_BRUSH **link = &gdi_brushes[(ULONG_PTR)ho % BRUSH_BUCKETS];
        while (*link && (*link)->handle != ho) link = &(*link)->next;
        GDI_BRUSH *brush = *link;
        if (!brush || brush->references ||
            brush->owner_pid != GetCurrentProcessId()) {
            brushes_unlock(flags);
            return FALSE;
        }
        *link = brush->next;
        brushes_unlock(flags);
        kfree(brush->pattern.pixels);
        kfree(brush);
        return TRUE;
    }
    if (IS_REGION_HANDLE(ho)) {
        regions_lock();
        GDI_REGION *region = region_from_handle(ho);
        if (!region) {
            regions_unlock();
            SetLastError(6); /* ERROR_INVALID_HANDLE */
            return FALSE;
        }
        gdi32_region_data_clear(region);
        gdi_region_owners[REGION_INDEX(ho)] = 0;
        regions_unlock();
        return TRUE;
    }
    /* If it's a bitmap handle, free the pixels with the matching allocator. */
    if (IS_BMP_HANDLE(ho)) {
        if (ho == STOCK_BITMAP) return TRUE;
        if (gdi_dc_object_selected(ho)) return FALSE;
        GDI_BITMAP *bmp = bmp_from_handle((HBITMAP)ho);
        if (bmp) {
            if (bmp->pixels) {
                if (bmp->mapping_base)
                    UnmapViewOfFile(bmp->mapping_base);
                else if (bmp->virtual_alloc)
                    VirtualFree(bmp->pixels, 0, MEM_RELEASE);
                else
                    kfree(bmp->pixels);
                bmp->pixels = NULL;
            }
            bmp->mapping_base = NULL;
            bmp->in_use = 0;
        }
        return TRUE;
    }
    if (IS_FONT_HANDLE(ho)) {
        if (gdi_dc_object_selected(ho)) return FALSE;
        DWORD owner = GetCurrentProcessId();
        uint64_t flags = brushes_lock();
        GDI_FONT **link = &gdi_fonts[(ULONG_PTR)ho % FONT_BUCKETS];
        while (*link && (*link)->handle != ho) link = &(*link)->next;
        GDI_FONT *font = *link;
        if (!font || font->owner_pid != owner) {
            brushes_unlock(flags);
            return FALSE;
        }
        *link = font->next;
        brushes_unlock(flags);
        kfree(font);
        return TRUE;
    }
    /* Other objects (pens and brushes) — just succeed */
    return TRUE;
}

typedef struct {
    LONG bmType;
    LONG bmWidth;
    LONG bmHeight;
    LONG bmWidthBytes;
    USHORT bmPlanes;
    USHORT bmBitsPixel;
    uint32_t bmBits;
} GDI_BITMAP_INFO32;

typedef struct {
    LONG bmType;
    LONG bmWidth;
    LONG bmHeight;
    LONG bmWidthBytes;
    USHORT bmPlanes;
    USHORT bmBitsPixel;
    PVOID bmBits;
} GDI_BITMAP_INFO64;

_Static_assert(sizeof(GDI_BITMAP_INFO32) == 24,
               "32-bit BITMAP layout changed");
_Static_assert(sizeof(GDI_BITMAP_INFO64) == 32,
               "64-bit BITMAP layout changed");

typedef struct {
    GDI_BITMAP_INFO32 bitmap;
    GDI_BITMAP_HEADER header;
    DWORD masks[3];
    uint32_t section, offset;
} GDI_DIBSECTION32;

typedef struct {
    GDI_BITMAP_INFO64 bitmap;
    GDI_BITMAP_HEADER header;
    DWORD masks[3];
    HANDLE section;
    DWORD offset;
} GDI_DIBSECTION64;

_Static_assert(sizeof(GDI_DIBSECTION32) == 84, "32-bit DIBSECTION layout changed");
_Static_assert(sizeof(GDI_DIBSECTION64) == 104, "64-bit DIBSECTION layout changed");

static int gdi_get_object(HGDIOBJ h, int c, PVOID pv, BOOL wide)
{
    GDI_PEN pen;
    if (gdi_pen_acquire(h, NULL, &pen)) {
        gdi_pen_release(h);
        /* Stock drawing pens report zero logical width (one device pixel).
         * NULL_PEN keeps the special one-unit LOGPEN representation. */
        if (((ULONG_PTR)h & TAG_MASK) == 0xAA000000u && pen.style != 5)
            pen.width = 0;
        int prefix = g_compat32_mode ? 24 : 28;
        int required = pen.extended ? prefix + (int)pen.style_count * 4 : 16;
        if (!pv) return required;
        if (!c && g_compat32_mode) SetLastError(998); /* ERROR_NOACCESS */
        if (c < required) return 0;
        /* NULL_PEN also accepts exactly sizeof(EXTLOGPEN). Other legacy
         * pens retain LOGPEN, irrespective of the larger buffer size.
         * WOW64 leaves the unused trailing entry untouched here. */
        BOOL extended = pen.extended ||
                        (pen.style == 5 && c == (g_compat32_mode ? 28 : 32));
        int written = extended && !pen.extended ? c : required;
        if (!win32_user_range_writable(pv, (SIZE_T)written, g_compat32_mode)) {
            SetLastError(998);
            return 0;
        }
        DWORD *out = pv;
        if (extended) {
            out[0] = pen.style; out[1] = pen.width;
            out[2] = pen.brush_style; out[3] = pen.color;
            out[4] = (DWORD)pen.hatch;
            if (!g_compat32_mode) out[5] = (DWORD)(pen.hatch >> 32);
            out[prefix / 4 - 1] = pen.style_count;
            for (UINT i = 0; i < pen.style_count; i++)
                out[prefix / 4 + i] = pen.styles[i];
            if (!pen.extended && !g_compat32_mode) out[7] = 0;
            return written;
        }
        out[0] = pen.style; out[1] = pen.width; out[2] = 0; out[3] = pen.color;
        return 16;
    }
    UINT entries = GetPaletteEntries(h, 0, 0, NULL);
    if (entries) {
        if (!pv) return sizeof(WORD);
        if (c < (int)sizeof(WORD)) return 0;
        *(WORD *)pv = (WORD)entries;
        return sizeof(WORD);
    }
    GDI_BRUSH brush;
    if (brush_acquire(h, NULL, &brush)) {
        int required = g_compat32_mode ? 12 : 16; /* LOGBRUSH */
        if (pv && c >= required) {
            gdi_memset(pv, 0, required);
            ((DWORD *)pv)[0] = brush.style;
            ((DWORD *)pv)[1] = brush.color;
        }
        brush_release(h);
        return !pv || c >= required ? required : 0;
    }
    GDI_LOGFONTW stock_font;
    const GDI_LOGFONTW *logfont = font_from_handle(h, &stock_font, FALSE)
        ? &stock_font : NULL;
    GDI_BITMAP *bmp = bmp_from_handle((HBITMAP)h);
    int width, height, pitch, bpp, dib_section;
    int required = g_compat32_mode ? (int)sizeof(GDI_BITMAP_INFO32)
                                   : (int)sizeof(GDI_BITMAP_INFO64);

    if (!logfont && IS_STOCK_FONT_HANDLE(h)) {
        gdi_stock_font_logfont(&stock_font);
        logfont = &stock_font;
    }

    if (logfont) {
        required = wide ? (int)sizeof(GDI_LOGFONTW)
                        : (int)sizeof(GDI_LOGFONTA);
        if (!pv) return required;
        if (c < required) return 0;

        if (wide) {
            gdi_memcpy(pv, logfont, sizeof(*logfont));
        } else {
            GDI_LOGFONTA *out = (GDI_LOGFONTA *)pv;
            gdi_memset(out, 0, sizeof(*out));
            gdi_memcpy(out, logfont, 28);
            for (int i = 0; i < 31; i++) {
                WCHAR ch = logfont->lfFaceName[i];
                out->lfFaceName[i] = ch < 0x80 ? (char)ch : '?';
                if (!ch) break;
            }
        }
        return required;
    }

    if (bmp) {
        width = bmp->width;
        height = bmp->height;
        pitch = bmp->pitch;
        bpp = bmp->bpp;
        dib_section = bmp->dib_section;
    } else if (h == STOCK_BITMAP) {
        width = 1;
        height = 1;
        pitch = 2;
        bpp = 1;
        dib_section = 0;
    } else {
        if (h) {
            serial_puts("[GDI32] GetObject invalid handle=0x");
            serial_puthex((uint64_t)(ULONG_PTR)h, 8);
            serial_puts("\n");
        }
        return 0;
    }

    serial_puts("[GDI32] GetObject bitmap=0x");
    serial_puthex((uint64_t)(ULONG_PTR)h, 8);
    serial_puts(" bytes=");
    serial_putdec((uint64_t)(uint32_t)c);
    serial_puts(" need=");
    serial_putdec((uint64_t)(uint32_t)required);
    serial_puts("\n");

    if (!pv) return required;
    if (c < required) return 0;

    int extended_size = g_compat32_mode ? (int)sizeof(GDI_DIBSECTION32)
                                       : (int)sizeof(GDI_DIBSECTION64);
    BOOL extended = dib_section && c >= extended_size;
    if (extended) {
        required = extended_size;
        gdi_memset(pv, 0, required);
        GDI_BITMAP_HEADER *header;
        DWORD *masks;
        if (g_compat32_mode) {
            GDI_DIBSECTION32 *out = pv;
            header = &out->header;
            masks = out->masks;
            out->section = (uint32_t)(ULONG_PTR)bmp->section;
            out->offset = bmp->section_offset;
        } else {
            GDI_DIBSECTION64 *out = pv;
            header = &out->header;
            masks = out->masks;
            out->section = bmp->section;
            out->offset = bmp->section_offset;
        }
        *header = bmp->dib_header;
        if (header->compression == GDI_BI_BITFIELDS) {
            masks[0] = bmp->format.red;
            masks[1] = bmp->format.green;
            masks[2] = bmp->format.blue;
        }
    }

    if (g_compat32_mode) {
        GDI_BITMAP_INFO32 *out = (GDI_BITMAP_INFO32 *)pv;
        gdi_memset(out, 0, sizeof(*out));
        out->bmWidth = width;
        out->bmHeight = height;
        out->bmWidthBytes = pitch;
        out->bmPlanes = 1;
        out->bmBitsPixel = (USHORT)bpp;
        out->bmBits = (bmp && dib_section)
            ? (uint32_t)(ULONG_PTR)bmp->pixels : 0;
    } else {
        GDI_BITMAP_INFO64 *out = (GDI_BITMAP_INFO64 *)pv;
        gdi_memset(out, 0, sizeof(*out));
        out->bmWidth = width;
        out->bmHeight = height;
        out->bmWidthBytes = pitch;
        out->bmPlanes = 1;
        out->bmBitsPixel = (USHORT)bpp;
        out->bmBits = (bmp && dib_section) ? bmp->pixels : NULL;
    }
    return required;
}

int WINAPI GetObjectA(HGDIOBJ h, int c, PVOID pv)
{
    return gdi_get_object(h, c, pv, FALSE);
}

static int WINAPI GetObjectW_k32(HGDIOBJ h, int c, PVOID pv)
{
    return gdi_get_object(h, c, pv, TRUE);
}

static DWORD WINAPI GetObjectType_k32(HGDIOBJ h)
{
    GDI_PEN pen;
    if (gdi_pen_acquire(h, NULL, &pen)) {
        gdi_pen_release(h);
        return pen.extended ? 11 : 1; /* OBJ_EXTPEN / OBJ_PEN */
    }
    if (GetPaletteEntries(h, 0, 0, NULL)) return OBJ_PAL;
    GDI_BRUSH brush;
    if (brush_acquire(h, NULL, &brush)) {
        brush_release(h);
        return OBJ_BRUSH;
    }
    if (bmp_from_handle((HBITMAP)h) || h == STOCK_BITMAP) return OBJ_BITMAP;
    if (font_from_handle(h, NULL, FALSE)) return 6; /* OBJ_FONT */
    if (dc_from_handle((HDC)h)) return 3; /* OBJ_DC */
    if (((ULONG_PTR)h & TAG_MASK) == PEN_TAG) return 0;
    if (((ULONG_PTR)h & TAG_MASK) == 0xAA000000u) return 6; /* OBJ_FONT */
    if (IS_REGION_HANDLE(h)) {
        regions_lock();
        BOOL valid = region_from_handle(h) != NULL;
        regions_unlock();
        if (valid) return 8; /* OBJ_REGION */
    }
    return 0;
}

/* ── GDI object creation (brushes, pens, stock objects) ──────── */

static HBRUSH_GDI gdi_create_brush(UINT style, DWORD color,
                                    const GDI_BITMAP *pattern)
{
    GDI_BRUSH *brush = kmalloc(sizeof(*brush));
    if (!brush) return NULL;
    gdi_memset(brush, 0, sizeof(*brush));
    brush->style = style;
    brush->color = color & 0xFFFFFFu;
    brush->owner_pid = GetCurrentProcessId();
    if (pattern) {
        SIZE_T bytes = (SIZE_T)pattern->pitch * pattern->height;
        brush->pattern = *pattern;
        brush->pattern.pixels = kmalloc(bytes);
        if (!brush->pattern.pixels) {
            kfree(brush);
            return NULL;
        }
        gdi_memcpy(brush->pattern.pixels, pattern->pixels, bytes);
    }
    uint64_t flags = brushes_lock();
    do {
        ULONG id = next_brush_id++ & 0x00FFFFFFu;
        brush->handle = (HGDIOBJ)(ULONG_PTR)(BRUSH_TAG | id);
    } while (brush_find_locked(brush->handle));
    UINT bucket = (ULONG_PTR)brush->handle % BRUSH_BUCKETS;
    brush->next = gdi_brushes[bucket];
    gdi_brushes[bucket] = brush;
    brushes_unlock(flags);
    return (HBRUSH_GDI)brush->handle;
}

HBRUSH_GDI WINAPI CreateSolidBrush(DWORD color)
{
    return gdi_create_brush(0, color, NULL);
}

HBRUSH_GDI WINAPI CreatePatternBrush(HBITMAP hBitmap)
{
    GDI_BITMAP *bitmap = bmp_from_handle(hBitmap);
    if (!bitmap || !bitmap->pixels || bitmap->pitch <= 0 ||
        bitmap->height <= 0 || bitmap->width <= 0) return NULL;
    return gdi_create_brush(3, 0, bitmap); /* BS_PATTERN */
}

static HPEN gdi_create_pen(const GDI_PEN *request)
{
    GDI_PEN *pen = kmalloc(sizeof(*pen));
    if (!pen) { SetLastError(8); return NULL; }
    *pen = *request;
    pen->owner_pid = GetCurrentProcessId();
    if (request->pattern.pixels) {
        SIZE_T bytes = (SIZE_T)request->pattern.pitch * request->pattern.height;
        pen->pattern.pixels = kmalloc(bytes);
        if (!pen->pattern.pixels) { kfree(pen); SetLastError(8); return NULL; }
        gdi_memcpy(pen->pattern.pixels, request->pattern.pixels, bytes);
    }
    uint64_t flags = brushes_lock();
    do {
        pen->handle = (HGDIOBJ)(ULONG_PTR)(PEN_TAG | (next_pen_id++ & 0xFFFFFFu));
    } while (gdi_pen_find_locked(pen->handle));
    pen->next = gdi_pens;
    gdi_pens = pen;
    brushes_unlock(flags);
    return (HPEN)pen->handle;
}

HPEN WINAPI CreatePen(int iStyle, int cWidth, DWORD color)
{
    int64_t width = cWidth < 0 ? -(int64_t)cWidth : cWidth;
    if (iStyle < 0 || iStyle > 6 || width > 0x7FFFFFFF) {
        SetLastError(87);
        return NULL;
    }
    if (iStyle == 5) return (HPEN)(ULONG_PTR)0xAA000008u;
    GDI_PEN pen = {0};
    pen.style = iStyle; pen.width = (DWORD)width; pen.color = color;
    return gdi_create_pen(&pen);
}

static HPEN WINAPI ExtCreatePen(DWORD style, DWORD width, PCVOID brush,
                                  DWORD count, const DWORD *styles)
{
    DWORD line_style = style & 15;
    BOOL geometric = (style & 0xF0000u) == 0x10000u;
    int64_t signed_width = (LONG)width;
    if (signed_width < 0) signed_width = -signed_width;
    if ((style & ~0x1330Fu) || (style & 0xF0000u) > 0x10000u ||
        (style & 0xF00u) > 0x200u || (style & 0xF000u) > 0x2000u ||
        line_style > 8 || signed_width > 0x7FFFFFFF ||
        (!geometric && line_style == 6) || (geometric && line_style == 8)) {
        SetLastError(87);
        return NULL;
    }
    if (!brush || !win32_user_range_readable(brush,
            g_compat32_mode ? 12 : 16, g_compat32_mode)) {
        SetLastError(87);
        return NULL;
    }
    GDI_PEN pen = {0};
    const BYTE *input = brush;
    gdi_memcpy(&pen.brush_style, input, 4);
    gdi_memcpy(&pen.color, input + 4, 4);
    gdi_memcpy(&pen.hatch, input + 8, g_compat32_mode ? 4 : 8);
    if (line_style == 5) return (HPEN)(ULONG_PTR)0xAA000008u;
    if ((!geometric && (signed_width != 1 || pen.brush_style != 0)) ||
        (geometric && !signed_width && line_style >= 1 && line_style <= 4) ||
        (line_style != 7 && (count || styles)) ||
        (line_style == 7 && (!count || count > 16 || !styles)) ||
        pen.brush_style > 3) {
        SetLastError(87);
        return NULL;
    }
    if (pen.brush_style == 1) return (HPEN)(ULONG_PTR)0xAA000008u;
    if (line_style == 7) {
        if (!win32_user_range_readable(styles, count * sizeof(*styles), g_compat32_mode)) {
            SetLastError(87);
            return NULL;
        }
        gdi_memcpy(pen.styles, styles, count * sizeof(*styles));
        uint64_t total = 0;
        for (UINT i = 0; i < count; i++) {
            if (pen.styles[i] > 0x7FFFFFFFu || (!geometric && !pen.styles[i])) {
                SetLastError(87);
                return NULL;
            }
            total += pen.styles[i];
        }
        if (!total) { SetLastError(87); return NULL; }
        pen.style_count = count;
    }
    if (pen.brush_style == 3) {
        GDI_BITMAP *bitmap = bmp_from_handle((HBITMAP)pen.hatch);
        if (!bitmap || bitmap->dib_section || !bitmap->pixels || bitmap->pitch <= 0 ||
            bitmap->height <= 0 || bitmap->width <= 0) {
            SetLastError(87);
            return NULL;
        }
        pen.pattern = *bitmap;
    }
    pen.extended = TRUE; pen.style = style; pen.width = (DWORD)signed_width;
    return gdi_create_pen(&pen);
}

HGDIOBJ WINAPI GetStockObject(int i)
{
    return (HGDIOBJ)(ULONG_PTR)(0xAA000000 + i);
}

/* ── BitBlt / PatBlt ─────────────────────────────────────────── */

/* ROP codes */
#define ROP_SRCCOPY    0x00CC0020
#define ROP_BLACKNESS  0x00000042
#define ROP_WHITENESS  0x00FF0062
#define ROP_PATCOPY    0x00F00021

static uint32_t gdi_colorref_to_argb(DWORD color);

static BOOL gdi_dc_point_visible(const GDI_DC *dc, int x, int y)
{
    if (dc->visible_region) {
        BOOL visible = FALSE;
        for (UINT i = 0; i < dc->visible_region->count; i++) {
            const GDI_RECT *rect = &GDI_REGION_RECTS(dc->visible_region)[i];
            if (x >= rect->left && x < rect->right &&
                y >= rect->top && y < rect->bottom) { visible = TRUE; break; }
        }
        if (!visible) return FALSE;
    }
    if (dc->paint_clip) {
        const GDI_RECT *rect = &dc->paint_clip->rect;
        if (x < rect->left || x >= rect->right ||
            y < rect->top || y >= rect->bottom) return FALSE;
        if (dc->paint_clip->region.count > 1) {
            BOOL visible = FALSE;
            for (UINT i = 0; i < dc->paint_clip->region.count; i++) {
                rect = &GDI_REGION_RECTS(&dc->paint_clip->region)[i];
                if (x >= rect->left && x < rect->right &&
                    y >= rect->top && y < rect->bottom) { visible = TRUE; break; }
            }
            if (!visible) return FALSE;
        }
    }
    if (!dc->clip_active) return TRUE;
    for (UINT i = 0; i < dc->clip_region.count; i++) {
        const GDI_RECT *rect = &GDI_REGION_RECTS(&dc->clip_region)[i];
        if (x >= rect->left && x < rect->right &&
            y >= rect->top && y < rect->bottom) return TRUE;
    }
    return FALSE;
}

static int gdi_pattern_coordinate(int coordinate, int origin, int size)
{
    int result = (int)(((int64_t)coordinate - origin) % size);
    return result < 0 ? result + size : result;
}

BOOL gdi32_fill_rect(HDC hdc, const GDI_RECT *rect, HGDIOBJ handle)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    GDI_BRUSH brush;
    if (!dc || !dc->surface || !rect || !brush_acquire(handle, dc, &brush))
        return FALSE;

    int64_t left = (int64_t)rect->left + dc->viewport_x;
    int64_t top = (int64_t)rect->top + dc->viewport_y;
    int64_t right = (int64_t)rect->right + dc->viewport_x;
    int64_t bottom = (int64_t)rect->bottom + dc->viewport_y;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > dc->width) right = dc->width;
    if (bottom > dc->height) bottom = dc->height;
    uint32_t solid = gdi_colorref_to_argb(brush.color);
    BOOL wrote = FALSE;
    if (brush.style != 1) {
        for (int64_t y = top; y < bottom; y++) {
            for (int64_t x = left; x < right; x++) {
                if (!gdi_dc_point_visible(dc, (int)x, (int)y)) continue;
                uint32_t color = solid;
                if (brush.style == 3) {
                    int px = gdi_pattern_coordinate((int)x, dc->brush_x,
                                                    brush.pattern.width);
                    int py = gdi_pattern_coordinate((int)y, dc->brush_y,
                                                    brush.pattern.height);
                    if (!gdi_read_bitmap_pixel(&brush.pattern, px, py, &color))
                        continue;
                    if (brush.pattern.bpp == 1)
                        color = gdi_colorref_to_argb((color & 0xFFFFFFu)
                            ? dc->bk_color : dc->text_color);
                }
                wrote |= gdi_write_dc_pixel(dc, (int)x, (int)y, color);
            }
        }
    }
    brush_release(handle);
    if (wrote) mark_dc_dirty(dc);
    return TRUE;
}

BOOL gdi32_draw_focus_rect(HDC hdc, const GDI_RECT *rect)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    if (!dc || !dc->surface || !rect || dc->map_mode != 1) return FALSE;
    int64_t left = (int64_t)rect->left + dc->viewport_x;
    int64_t top = (int64_t)rect->top + dc->viewport_y;
    int64_t right = (int64_t)rect->right + dc->viewport_x;
    int64_t bottom = (int64_t)rect->bottom + dc->viewport_y;
    int64_t x0 = left < 0 ? 0 : left, x1 = right > dc->width ? dc->width : right;
    int64_t y0 = top < 0 ? 0 : top, y1 = bottom > dc->height ? dc->height : bottom;
    BOOL wrote = FALSE;
    for (int64_t y = y0; y < y1; y++) {
        for (int64_t x = x0; x < x1; x++) {
            if ((x != left && x != right - 1 && y != top && y != bottom - 1) ||
                ((x + y) & 1) || !gdi_dc_point_visible(dc, (int)x, (int)y))
                continue;
            uint32_t color;
            if (gdi_read_dc_pixel(dc, (int)x, (int)y, &color))
                wrote |= gdi_write_dc_pixel(dc, (int)x, (int)y,
                                             color ^ 0x00FFFFFFu);
        }
    }
    if (wrote) mark_dc_dirty(dc);
    return TRUE;
}

BOOL WINAPI BitBlt(HDC hdcDest, int x, int y, int cx, int cy,
                   HDC hdcSrc, int x1, int y1, DWORD rop)
{
    GDI_DRAW_SCOPE(destination_draw);
    GDI_DRAW_SCOPE(source_draw);
    GDI_DC *dst = gdi_acquire_draw_dc(hdcDest, &destination_draw);
    GDI_DC *trace_src = hdcSrc == hdcDest ? dst :
        gdi_acquire_draw_dc(hdcSrc, &source_draw);
    if (gdi_blit_trace_count++ < 64) {
        serial_puts("[GDI-BLIT] dst=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hdcDest, 8);
        serial_puts(dst && dst->surface ? " ready" : " invalid");
        serial_puts(" src=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hdcSrc, 8);
        serial_puts(trace_src && trace_src->surface ? " ready" : " invalid");
        serial_puts(" size=");
        serial_putdec((uint64_t)(uint32_t)cx);
        serial_puts("x");
        serial_putdec((uint64_t)(uint32_t)cy);
        serial_puts(" rop=0x");
        serial_puthex(rop, 8);
        serial_puts("\n");
    }
    if (!dst || !dst->surface) return FALSE;

    if (rop == ROP_BLACKNESS || rop == ROP_WHITENESS || rop == ROP_PATCOPY) {
        int64_t right = (int64_t)x + cx, bottom = (int64_t)y + cy;
        if (right < (-2147483647LL - 1) || right > 2147483647LL ||
            bottom < (-2147483647LL - 1) || bottom > 2147483647LL)
            return FALSE;
        GDI_RECT rect = { x, y, (LONG)right, (LONG)bottom };
        HGDIOBJ brush = rop == ROP_PATCOPY ? dst->current_brush :
            GetStockObject(rop == ROP_BLACKNESS ? 4 : 0);
        return gdi32_fill_rect(hdcDest, &rect, brush);
    }

    /* SRCCOPY: requires valid source */
    if (rop == ROP_SRCCOPY) {
        GDI_DC *src = trace_src;
        if (!src || !src->surface) return FALSE;

        /* Keep clipping arithmetic wide until both surfaces bound the copy. */
        int64_t sx = x1, sy = y1;
        int64_t dx = x, dy = y;
        int64_t w = cx, h = cy;

        /* Clip to source bounds */
        if (sx < 0) { w += sx; dx -= sx; sx = 0; }
        if (sy < 0) { h += sy; dy -= sy; sy = 0; }
        if (sx + w > src->width)  w = src->width - sx;
        if (sy + h > src->height) h = src->height - sy;

        /* Clip to dest bounds */
        if (dx < 0) { w += dx; sx -= dx; dx = 0; }
        if (dy < 0) { h += dy; sy -= dy; dy = 0; }
        if (dx + w > dst->width)  w = dst->width - dx;
        if (dy + h > dst->height) h = dst->height - dy;

        if (w <= 0 || h <= 0) return TRUE;

        int src_bytes = src->bpp / 8;
        BOOL clipped = dst->clip_active || dst->paint_clip || dst->visible_region;
        BOOL simple_clip = (!dst->clip_active || dst->clip_region.count <= 1) &&
            (!dst->paint_clip || dst->paint_clip->region.count <= 1) &&
            (!dst->visible_region || dst->visible_region->count <= 1);
        BOOL identical_format = simple_clip && gdi_formats_equal(src, dst) &&
                                src_bytes > 0 && src_bytes <= 4;
        BOOL opaque_alpha = FALSE;
        if (identical_format && clipped) {
            /* Match the pixel path's coverage semantics. Native RGB32 reads
             * become opaque; 24-bit RGB has no padding bits. Keep palettes,
             * other masks and packed formats on their conversion path. */
            if (src->bpp == 32) {
                DWORD red, green, blue;
                gdi_format_masks(src->bpp, src->format, &red, &green, &blue);
                identical_format = red == 0xFF0000 && green == 0xFF00 &&
                                   blue == 0xFF;
                opaque_alpha = identical_format;
            } else {
                identical_format = src->bpp == 24;
            }
            /* Opposite row layouts over the same storage require the generic
             * path; directional row copies assume identical layouts. */
            if (src->surface == dst->surface &&
                (src->pitch != dst->pitch || src->height != dst->height ||
                 src->bottomup != dst->bottomup))
                identical_format = FALSE;
        }

        if (identical_format && clipped) {
            /* BeginPaint and a simple application region intersect once,
             * rather than testing every pixel of the original blit. */
            const GDI_RECT *clips[3] = {
                dst->paint_clip ? &dst->paint_clip->rect : NULL,
                dst->clip_active && dst->clip_region.count ? &GDI_REGION_RECTS(&dst->clip_region)[0] : NULL,
                dst->visible_region && dst->visible_region->count ?
                    &GDI_REGION_RECTS(dst->visible_region)[0] : NULL
            };
            if ((dst->clip_active && !dst->clip_region.count) ||
                (dst->visible_region && !dst->visible_region->count)) return TRUE;
            for (unsigned i = 0; i < 3; i++) {
                const GDI_RECT *clip = clips[i];
                if (!clip) continue;
                if (dx < clip->left) {
                    int64_t skip = clip->left - dx;
                    sx += skip; w -= skip; dx = clip->left;
                }
                if (dy < clip->top) {
                    int64_t skip = clip->top - dy;
                    sy += skip; h -= skip; dy = clip->top;
                }
                if (dx + w > clip->right) w = clip->right - dx;
                if (dy + h > clip->bottom) h = clip->bottom - dy;
                if (w <= 0 || h <= 0) return TRUE;
            }
        }

        if (identical_format) {
            BYTE *src_base = (BYTE *)src->surface;
            BYTE *dst_base = (BYTE *)dst->surface;
            SIZE_T row_bytes = (SIZE_T)w * src_bytes;
            int first = 0;
            int last = h;
            int step = 1;
            if (src->surface == dst->surface && dy > sy) {
                first = h - 1;
                last = -1;
                step = -1;
            }
            for (int row = first; row != last; row += step) {
                int src_y = src->bottomup ? src->height - 1 - (sy + row)
                                          : sy + row;
                int dst_y = dst->bottomup ? dst->height - 1 - (dy + row)
                                          : dy + row;
                BYTE *sp = src_base + (SIZE_T)src_y * src->pitch + sx * src_bytes;
                BYTE *dp = dst_base + (SIZE_T)dst_y * dst->pitch + dx * src_bytes;
                gdi_memmove(dp, sp, row_bytes);
                if (opaque_alpha)
                    for (int col = 0; col < w; col++) dp[(SIZE_T)col * 4 + 3] = 255;
            }
        } else {
            for (int row = 0; row < h; row++) {
                for (int col = 0; col < w; col++) {
                    uint32_t color;
                    if (!gdi_read_dc_pixel(src, sx + col, sy + row, &color) ||
                        !gdi_write_dc_pixel(dst, dx + col, dy + row, color))
                        return FALSE;
                }
            }
        }
        mark_dc_dirty(dst);
        return TRUE;
    }

    /* Unknown ROP — succeed silently */
    return TRUE;
}

BOOL WINAPI PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop)
{
    /* PatBlt is BitBlt with no source */
    return BitBlt(hdc, x, y, w, h, NULL, 0, 0, rop);
}

/* ── Line drawing ────────────────────────────────────────────── */

static BOOL gdi_polygon_draw(HDC hdc, const GDI_POINT *points,
                             const int *counts, int polygons, int operation);

BOOL WINAPI MoveToEx(HDC hdc, int x, int y, PVOID lppt)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) { SetLastError(6); return FALSE; }
    if (lppt) {
        ((LONG *)lppt)[0] = dc->current_x;
        ((LONG *)lppt)[1] = dc->current_y;
    }
    dc->current_x = x;
    dc->current_y = y;
    return TRUE;
}

BOOL WINAPI GetCurrentPositionEx(HDC hdc, PVOID point)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !point) { SetLastError(!dc ? 6 : 87); return FALSE; }
    ((LONG *)point)[0] = dc->current_x;
    ((LONG *)point)[1] = dc->current_y;
    return TRUE;
}

BOOL WINAPI LineTo(HDC hdc, int x, int y)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) { SetLastError(6); return FALSE; }
    GDI_POINT points[2] = {{dc->current_x, dc->current_y}, {x, y}};
    int count = 2;
    return gdi_polygon_draw(hdc, points, &count, 1, 2);
}

/* ── Text ────────────────────────────────────────────────────── */

DWORD WINAPI SetTextColor(HDC hdc, DWORD color)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc) {
        DWORD prev = dc->text_color;
        dc->text_color = color;
        return prev;
    }
    return 0;
}

DWORD WINAPI GetTextColor(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    return dc ? dc->text_color : 0xFFFFFFFFu;
}

DWORD WINAPI SetBkColor(HDC hdc, DWORD color)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc) {
        DWORD prev = dc->bk_color;
        dc->bk_color = color;
        return prev;
    }
    return 0;
}

DWORD WINAPI GetBkColor(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    return dc ? dc->bk_color : 0xFFFFFFFFu;
}

int WINAPI SetBkMode(HDC hdc, int mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc) {
        int prev = dc->bk_mode;
        dc->bk_mode = mode;
        return prev;
    }
    return 0;
}

static uint32_t gdi_colorref_to_argb(DWORD color)
{
    return 0xFF000000u | ((color & 0x000000FFu) << 16) |
           (color & 0x0000FF00u) | ((color & 0x00FF0000u) >> 16);
}

static BOOL gdi_text_pixel(GDI_DC *dc, int64_t x, int64_t y,
                            uint32_t color, const GDI_RECT *clip)
{
    if (x < 0 || y < 0 || x >= dc->width || y >= dc->height) return FALSE;
    if (clip && (x - dc->viewport_x < clip->left ||
                 x - dc->viewport_x >= clip->right ||
                 y - dc->viewport_y < clip->top ||
                 y - dc->viewport_y >= clip->bottom)) return FALSE;
    return gdi_write_dc_pixel(dc, (int)x, (int)y, color);
}

static BOOL gdi_text_fill(GDI_DC *dc, int64_t left, int64_t top,
                           int64_t right, int64_t bottom, uint32_t color,
                           const GDI_RECT *clip)
{
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > dc->width) right = dc->width;
    if (bottom > dc->height) bottom = dc->height;
    BOOL wrote = FALSE;
    for (int64_t y = top; y < bottom; y++)
        for (int64_t x = left; x < right; x++)
            wrote |= gdi_text_pixel(dc, x, y, color, clip);
    return wrote;
}

static BOOL gdi_draw_bitmap_glyph(GDI_DC *dc, UINT codepoint,
                                    int64_t x, int64_t y, uint32_t foreground,
                                    const GDI_RECT *clip)
{
    if (codepoint < 32 || codepoint > 126) codepoint = '?';
    const uint8_t *glyph = gui_font8x16[codepoint - 32];
    BOOL visible = x < dc->width && x + 8 > 0 &&
                   y < dc->height && y + 16 > 0;
    if (clip && (x - dc->viewport_x >= clip->right ||
                 x + 8 - dc->viewport_x <= clip->left ||
                 y - dc->viewport_y >= clip->bottom ||
                 y + 16 - dc->viewport_y <= clip->top)) visible = FALSE;
    BOOL wrote = FALSE;
    for (int row = 0; visible && row < 16; row++)
        for (int column = 0; column < 8; column++)
            if (glyph[row] & (0x80u >> column))
                wrote |= gdi_text_pixel(dc, x + column, y + row,
                                        foreground, clip);
    return wrote;
}

BOOL gdi32_rasterize_glyph(HDC hdc, WCHAR character, GDI_FONT_BITMAP *bitmap)
{
    GDI_DC *source = dc_from_handle(hdc);
    if (!source || !bitmap) {
        SetLastError(!source ? 6 : 87);
        return FALSE;
    }
    /* GDI maps every selected font to this bitmap face today. Render through
     * the same primitive as TextOut, without changing the caller's DC state,
     * selected bitmap, colors, clipping, mapping or current position. WGL
     * requires an empty list for glyphs absent from this bitmap face; unlike
     * ordinary GDI text, this bridge must not substitute a question mark. */
    gdi_memset(bitmap, 0, sizeof(*bitmap));
    if (character < 32 || character > 126) return TRUE;
    bitmap->width = 8;
    bitmap->height = 16;
    bitmap->origin_y = 4; /* Descent below the GDI baseline. */
    bitmap->advance_x = 8;
    GDI_DC dc;
    gdi_memset(&dc, 0, sizeof(dc));
    dc.surface = bitmap->bits;
    dc.width = 8;
    dc.height = 16;
    dc.pitch = 4;
    dc.bpp = 1;
    dc.bottomup = 1;
    dc.current_font = source->current_font;
    gdi_draw_bitmap_glyph(&dc, character, 0, 0, 0xFFFFFFFFu, NULL);
    return TRUE;
}

static BOOL gdi_ext_text_out(HDC hdc, int x, int y, UINT options,
                              const GDI_RECT *rect, PCWSTR text, UINT count,
                              const int *spacing)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    if (!dc || (count && !text)) {
        SetLastError(!dc ? 6 : 87);
        return FALSE;
    }
    int64_t advance_x = 0, advance_y = 0;
    for (UINT i = 0; i < count; i++) {
        advance_x += spacing ? spacing[i * ((options & 0x2000) ? 2 : 1)] : 8;
        if (spacing && (options & 0x2000)) advance_y -= spacing[2*i+1];
    }
    int64_t origin_x = (dc->text_align & 1) ? dc->current_x : x;
    int64_t origin_y = (dc->text_align & 1) ? dc->current_y : y;
    UINT horizontal = dc->text_align & 6;
    if (horizontal == 6) origin_x -= advance_x / 2;
    else if (horizontal == 2) origin_x -= advance_x;
    int64_t next_x = horizontal == 6 ? dc->current_x :
                     (horizontal == 2 ? origin_x : origin_x + advance_x);
    int64_t next_y = (int64_t)dc->current_y + advance_y;
    if ((dc->text_align & 1) &&
        (next_x < -2147483648LL || next_x > 2147483647LL ||
         next_y < -2147483648LL || next_y > 2147483647LL)) {
        SetLastError(87);
        return FALSE;
    }
    if ((dc->text_align & 24) == 24) origin_y -= 12;
    else if (dc->text_align & 8) origin_y -= 16;
    origin_x += dc->viewport_x;
    origin_y += dc->viewport_y;

    uint32_t foreground = gdi_colorref_to_argb(dc->text_color);
    uint32_t background = gdi_colorref_to_argb(dc->bk_color);
    const GDI_RECT *clip = (options & 4) ? rect : NULL; /* ETO_CLIPPED */
    BOOL wrote = FALSE;
    if ((options & 2) && rect) /* ETO_OPAQUE also works with no string. */
        wrote |= gdi_text_fill(dc,
            (int64_t)rect->left + dc->viewport_x,
            (int64_t)rect->top + dc->viewport_y,
            (int64_t)rect->right + dc->viewport_x,
            (int64_t)rect->bottom + dc->viewport_y, background, NULL);
    if (count && dc->bk_mode == 2)
        wrote |= gdi_text_fill(dc, origin_x, origin_y,
            origin_x + advance_x, origin_y + 16, background, clip);

    int64_t glyph_x = origin_x, glyph_y = origin_y;
    for (UINT index = 0; index < count; index++) {
        wrote |= gdi_draw_bitmap_glyph(dc, text[index], glyph_x, glyph_y,
                                        foreground, clip);
        glyph_x += spacing ? spacing[index * ((options & 0x2000) ? 2 : 1)] : 8;
        if (spacing && (options & 0x2000)) glyph_y -= spacing[2*index+1];
    }
    if ((dc->text_align & 1) && count) {
        gdi_draw_commit_position(&draw, dc, (int)next_x, (int)next_y);
    }
    if (wrote) mark_dc_dirty(dc);
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI TextOutW(HDC hdc, int x, int y, PCWSTR lpString, int c)
{
    if (c < 0) { SetLastError(87); return FALSE; }
    return gdi_ext_text_out(hdc, x, y, 0, NULL, lpString, (UINT)c, NULL);
}

static BOOL WINAPI TextOutA_k32(HDC hdc, int x, int y, PCSTR text, int count)
{
    if (count < 0) { SetLastError(87); return FALSE; }
    return ExtTextOutA(hdc, x, y, 0, NULL, text, (UINT)count, NULL);
}

BOOL WINAPI ExtTextOutA(HDC hdc, int x, int y, UINT options,
                        PVOID lprect, PCSTR lpString, UINT c, PVOID lpDx)
{
    if (!dc_from_handle(hdc) || c > 0x7FFFFFFFu || (c && !lpString)) {
        SetLastError(!dc_from_handle(hdc) ? 6 : 87);
        return FALSE;
    }
    /* The ANSI entry point explicitly does not draw glyph-index input. */
    if (options & 0x10) return TRUE;
    if (!c) return gdi_ext_text_out(hdc, x, y, options, lprect, NULL, 0, NULL);
    int length = MultiByteToWideChar(0, 0, lpString, (int)c, NULL, 0);
    if (length <= 0) return FALSE;
    WCHAR *wide = kmalloc((SIZE_T)length * sizeof(WCHAR));
    if (!wide) { SetLastError(8); return FALSE; }
    MultiByteToWideChar(0, 0, lpString, (int)c, wide, length);
    BOOL result = gdi_ext_text_out(hdc, x, y, options, lprect, wide,
                                    (UINT)length, lpDx);
    kfree(wide);
    return result;
}

BOOL WINAPI ExtTextOutW(HDC hdc, int x, int y, UINT options,
                         const GDI_RECT *rect, PCWSTR text, UINT count,
                         const int *spacing)
{
    if (count > 0x7FFFFFFFu) { SetLastError(87); return FALSE; }
    return gdi_ext_text_out(hdc, x, y, options, rect, text, count, spacing);
}

static int WINAPI SetMapMode_k32(HDC hdc, int mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || mode < 1 || mode > 8) return 0;
    int previous = dc->map_mode;
    dc->map_mode = mode;
    if (mode == 1) {
        dc->window_cx = dc->window_cy = 1;
        dc->viewport_cx = dc->viewport_cy = 1;
    } else if (mode != 8 && (mode != 7 || previous != 7)) {
        int x = GetDeviceCaps(hdc, 4), y = GetDeviceCaps(hdc, 6);
        int scale = mode == 3 ? 100 : 10;
        if (mode == 4 || mode == 5 || mode == 6) {
            scale = mode == 4 ? 1000 : mode == 5 ? 10000 : 14400;
            dc->window_cx = (int)(((int64_t)x * scale + 127) / 254);
            dc->window_cy = (int)(((int64_t)y * scale + 127) / 254);
        } else {
            dc->window_cx = x * scale;
            dc->window_cy = y * scale;
        }
        dc->viewport_cx = GetDeviceCaps(hdc, HORZRES);
        dc->viewport_cy = -GetDeviceCaps(hdc, VERTRES);
    }
    return previous;
}

static int WINAPI GetMapMode_k32(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    return dc ? dc->map_mode : 0;
}

static BOOL WINAPI SetWindowOrgEx(HDC hdc, int x, int y, GDI_POINT *previous)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return FALSE;
    if (previous) *previous = (GDI_POINT){dc->window_x, dc->window_y};
    dc->window_x = x;
    dc->window_y = y;
    return TRUE;
}

static void gdi_adjust_isotropic(GDI_DC *dc)
{
    if (dc->map_mode != 7) return;
    double sx = (double)dc->viewport_cx / dc->window_cx;
    double sy = (double)dc->viewport_cy / dc->window_cy;
    if (sx < 0) sx = -sx;
    if (sy < 0) sy = -sy;
    /* The virtual display has square pixels. Keep signs for axis reflection
     * and shrink one viewport extent to give both axes the same scale. */
    if (sx > sy) {
        double value = dc->window_cx * sy;
        if (value < 0) value = -value;
        int64_t extent = (int64_t)(value + 0.5);
        if (!extent) extent = 1;
        int64_t limit = dc->viewport_cx < 0 ? 2147483648LL : 2147483647LL;
        if (extent > limit) extent = limit;
        dc->viewport_cx = (int)(dc->viewport_cx < 0 ? -extent : extent);
    } else {
        double value = dc->window_cy * sx;
        if (value < 0) value = -value;
        int64_t extent = (int64_t)(value + 0.5);
        if (!extent) extent = 1;
        int64_t limit = dc->viewport_cy < 0 ? 2147483648LL : 2147483647LL;
        if (extent > limit) extent = limit;
        dc->viewport_cy = (int)(dc->viewport_cy < 0 ? -extent : extent);
    }
}

static BOOL WINAPI SetWindowExtEx(HDC hdc, int x, int y, GDI_POINT *previous)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return FALSE;
    if (previous) *previous = (GDI_POINT){dc->window_cx, dc->window_cy};
    if (dc->map_mode < 7) return TRUE;
    if (!x || !y) return FALSE;
    dc->window_cx = x;
    dc->window_cy = y;
    gdi_adjust_isotropic(dc);
    return TRUE;
}

static BOOL WINAPI SetViewportExtEx(HDC hdc, int x, int y, GDI_POINT *previous)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return FALSE;
    if (previous) *previous = (GDI_POINT){dc->viewport_cx, dc->viewport_cy};
    if (dc->map_mode < 7) return TRUE;
    if (!x || !y) return TRUE; /* Windows ignores a zero viewport extent. */
    dc->viewport_cx = x;
    dc->viewport_cy = y;
    gdi_adjust_isotropic(dc);
    return TRUE;
}

static BOOL WINAPI GetWindowOrgEx(HDC hdc, GDI_POINT *point)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !point) return FALSE;
    *point = (GDI_POINT){dc->window_x, dc->window_y};
    return TRUE;
}

static BOOL WINAPI GetViewportOrgEx(HDC hdc, GDI_POINT *point)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !point) return FALSE;
    *point = (GDI_POINT){dc->viewport_x, dc->viewport_y};
    return TRUE;
}

static BOOL WINAPI GetWindowExtEx(HDC hdc, GDI_POINT *point)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !point) return FALSE;
    *point = (GDI_POINT){dc->window_cx, dc->window_cy};
    return TRUE;
}

static BOOL WINAPI GetViewportExtEx(HDC hdc, GDI_POINT *point)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !point) return FALSE;
    *point = (GDI_POINT){dc->viewport_cx, dc->viewport_cy};
    return TRUE;
}

static BOOL gdi_transform_point(const GDI_DC *dc, GDI_POINT input,
                                   GDI_POINT *output, BOOL inverse)
{
    double x = input.x, y = input.y;
    if (!dc->window_cx || !dc->window_cy || !dc->viewport_cx || !dc->viewport_cy)
        return FALSE;
    const float *m = dc->world_transform;
    if (inverse) {
        if (input.x < -67108864 || input.x > 67108863 ||
            input.y < -67108864 || input.y > 67108863) return FALSE;
        x = (x - dc->viewport_x) * dc->window_cx / dc->viewport_cx + dc->window_x;
        y = (y - dc->viewport_y) * dc->window_cy / dc->viewport_cy + dc->window_y;
        if (dc->graphics_mode == 2) {
            double determinant = (double)m[0] * m[3] - (double)m[1] * m[2];
            if (!determinant) return FALSE;
            double tx = x - m[4], ty = y - m[5];
            x = (tx * m[3] - ty * m[2]) / determinant;
            y = (ty * m[0] - tx * m[1]) / determinant;
        }
    } else {
        if (dc->graphics_mode == 2) {
            double tx = x * m[0] + y * m[2] + m[4];
            y = x * m[1] + y * m[3] + m[5];
            x = tx;
        }
        x = (x - dc->window_x) * dc->viewport_cx / dc->window_cx + dc->viewport_x;
        y = (y - dc->window_y) * dc->viewport_cy / dc->window_cy + dc->viewport_y;
    }
    /* Check before converting, including NaN/infinity and the documented
     * signed 27-bit device-coordinate limit. */
    double min = inverse ? -2147483648.0 : -67108864.0;
    double max = inverse ? 2147483647.0 : 67108863.0;
    if (!(x >= min && x <= max && y >= min && y <= max)) return FALSE;
    /* GDI rounds half-way values toward positive infinity, including
     * negative coordinates (for example, -4.5 becomes -4). */
    x += 0.5; y += 0.5;
    output->x = (LONG)x;
    output->y = (LONG)y;
    if (x < output->x) output->x--;
    if (y < output->y) output->y--;
    return TRUE;
}

static BOOL gdi_convert_points(HDC hdc, GDI_POINT *points, int count, BOOL inverse)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || count < 0 || (count && !points)) return FALSE;
    /* Validate the whole array before changing any caller-owned point. */
    for (int i = 0; i < count; i++) {
        GDI_POINT converted;
        if (!gdi_transform_point(dc, points[i], &converted, inverse)) return FALSE;
    }
    for (int i = 0; i < count; i++)
        gdi_transform_point(dc, points[i], &points[i], inverse);
    return TRUE;
}

static BOOL WINAPI LPtoDP(HDC hdc, GDI_POINT *points, int count)
{
    return gdi_convert_points(hdc, points, count, FALSE);
}

static BOOL WINAPI DPtoLP(HDC hdc, GDI_POINT *points, int count)
{
    return gdi_convert_points(hdc, points, count, TRUE);
}

static BOOL WINAPI GetDCOrgEx(HDC hdc, GDI_POINT *point)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !point) return FALSE;
    *point = (GDI_POINT){0, 0};
    if (!dc->pooled_window || dc->is_screen) return TRUE;
    HANDLE window = (HANDLE)dc->owner_window;
    if (!dc->whole_window)
        return ClientToScreen && ClientToScreen(window, point);
    GDI_RECT rect;
    if (!GetWindowRect || !GetWindowRect(window, &rect)) return FALSE;
    point->x = rect.left;
    point->y = rect.top;
    return TRUE;
}

int WINAPI GetRandomRgn(HDC hdc, HGDIOBJ rgn, int kind)
{
    GDI_DC *dc = dc_from_handle(hdc);
    regions_lock();
    BOOL valid = region_from_handle(rgn) != NULL;
    regions_unlock();
    if (!dc || !valid) { if (!g_compat32_mode) SetLastError(6); return -1; }
    if (kind != 4) return 0; /* Public SYSRGN contract. */
    DWORD saved_error = GetLastError();
    GDI_REGION_SCOPE(visible);
    GDI_REGION_SCOPE(clipped);
    if (dc->visible_region) {
        gdi32_region_data_assign(&visible, dc->visible_region);
        if (!gdi_refresh_visible_region(dc, &visible)) return -1;
    } else if (dc->pooled_window && dc->owner_window && GetDCEx && ReleaseDC) {
        /* Legacy GetDC/BeginPaint contexts have no cached visible region.
         * Ask USER32 to apply the window/class styles through its existing
         * common-DC path, copy the result and release that temporary borrow. */
        HDC borrow = GetDCEx((HANDLE)dc->owner_window, NULL, 0x10002u | (dc->whole_window ? 1u : 0u));
        GDI_DC *common = dc_from_handle(borrow);
        BOOL copied = common && common->visible_region;
        if (copied) gdi32_region_data_assign(&visible, common->visible_region);
        if (borrow) ReleaseDC((HANDLE)dc->owner_window, borrow);
        if (!copied) return -1;
    } else {
        /* An untouched memory DC has the stock 1x1 bitmap, even though its
         * device capabilities retain the associated display's dimensions. */
        BOOL stock = !dc->is_screen && !dc->pooled_window && dc->prev_bitmap == STOCK_BITMAP;
        if (!region_append_rect(&visible, (GDI_RECT){0,0,stock ? 1 : dc->width,stock ? 1 : dc->height})) {
            SetLastError(8); return -1;
        }
    }
    if (dc->paint_clip) {
        if (!gdi32_region_data_combine(&clipped, &visible, &dc->paint_clip->region, GDI_RGN_AND)) return -1;
        gdi32_region_data_move(&visible, &clipped);
    }
    GDI_POINT origin;
    if (!GetDCOrgEx(hdc, &origin) || !region_reserve(&visible, visible.count)) return -1;
    for (UINT i = 0; i < visible.count; i++) {
        GDI_RECT *rect = &GDI_REGION_RECTS(&visible)[i];
        int64_t left = (int64_t)rect->left + origin.x, right = (int64_t)rect->right + origin.x;
        int64_t top = (int64_t)rect->top + origin.y, bottom = (int64_t)rect->bottom + origin.y;
        if (left < -2147483648LL || right > 2147483647LL || top < -2147483648LL || bottom > 2147483647LL) {
            SetLastError(87); return -1;
        }
        *rect = (GDI_RECT){(LONG)left,(LONG)top,(LONG)right,(LONG)bottom};
    }
    visible.polygon_type = 0;
    if (!gdi32_region_data_write(rgn, &visible)) return -1;
    SetLastError(saved_error);
    return 1;
}

static BOOL WINAPI SetViewportOrgEx_k32(HDC hdc, int x, int y,
                                         GDI_POINT *previous)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (previous) {
        previous->x = dc->viewport_x;
        previous->y = dc->viewport_y;
    }
    dc->viewport_x = x;
    dc->viewport_y = y;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetBrushOrgEx_k32(HDC hdc, int x, int y,
                                      GDI_POINT *previous)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (previous) {
        previous->x = dc->brush_x;
        previous->y = dc->brush_y;
    }
    dc->brush_x = x;
    dc->brush_y = y;
    SetLastError(0);
    return TRUE;
}

static int WINAPI SetStretchBltMode_k32(HDC hdc, int mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || mode < 1 || mode > 4) return 0;
    int previous = dc->stretch_mode;
    dc->stretch_mode = mode;
    return previous;
}

static DWORD WINAPI SetDCBrushColor_k32(HDC hdc, DWORD color)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return 0xFFFFFFFFu; /* CLR_INVALID */
    DWORD previous = dc->brush_color;
    dc->brush_color = color;
    return previous;
}

static BOOL WINAPI GdiFlush_k32(void)
{
    return TRUE;
}

static BOOL WINAPI Polyline_k32(HDC hdc, const GDI_POINT *points, int count)
{
    return gdi_polygon_draw(hdc, points, &count, 1, 0);
}

static int WINAPI GetPolyFillMode(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) { SetLastError(6); return 0; }
    return dc->poly_fill_mode;
}

static int WINAPI SetPolyFillMode(HDC hdc, int mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) { SetLastError(6); return 0; }
    int previous = dc->poly_fill_mode;
    dc->poly_fill_mode = mode;
    return previous;
}

static int WINAPI GetROP2(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) { SetLastError(6); return 0; }
    return dc->rop2;
}

static int WINAPI SetROP2(HDC hdc, int mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) { SetLastError(6); return 0; }
    int previous = dc->rop2;
    dc->rop2 = mode & 255;
    return previous;
}

static int WINAPI GetArcDirection(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return 0;
    return dc->arc_direction;
}

static int WINAPI SetArcDirection(HDC hdc, int direction)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return 0;
    if (direction != 1 && direction != 2) {
        if (!g_compat32_mode) SetLastError(87);
        return 0;
    }
    int previous = dc->arc_direction;
    dc->arc_direction = direction;
    return previous;
}

static DWORD WINAPI SetDCPenColor(HDC hdc, DWORD color)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) { SetLastError(6); return 0xFFFFFFFFu; }
    DWORD previous = dc->pen_color;
    dc->pen_color = color;
    return previous;
}

static DWORD WINAPI GetDCPenColor(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) { SetLastError(6); return 0xFFFFFFFFu; }
    return dc->pen_color;
}

typedef struct {
    USHORT *pixels;
    int left, top, width, height;
    int rop2;
} GDI_SHAPE_MASK;

static DWORD gdi_apply_rop2(DWORD source, DWORD destination, int mode)
{
    UINT table = (UINT)(mode - 1);
    DWORD result = 0;
    if (table & 1) result |= ~source & ~destination;
    if (table & 2) result |= ~source & destination;
    if (table & 4) result |= source & ~destination;
    if (table & 8) result |= source & destination;
    return 0xFF000000u | (result & 0xFFFFFFu);
}

/* Each pixel stores fill coverage, stroke presence and two four-bit maps
 * (source, destination) -> result. Keep background and pen visits separate:
 * an opaque cosmetic stroke paints its gaps before its foreground, including
 * where later gaps intersect earlier foreground segments. */
static void gdi_mask_visit(GDI_SHAPE_MASK *mask, int x, int y,
                            BOOL background, BOOL union_coverage)
{
    x -= mask->left; y -= mask->top;
    if (x < 0 || y < 0 || x >= mask->width || y >= mask->height) return;
    USHORT *coverage = &mask->pixels[(SIZE_T)y * mask->width + x];
    if (union_coverage && (*coverage & 2)) return;
    UINT previous = (*coverage & 2) ? *coverage >> 2 : 0xAA;
    UINT shift = background ? 0 : 4;
    UINT table = (UINT)(mask->rop2 - 1), composed = 0;
    for (UINT source = 0; source < 2; source++) {
        for (UINT d = 0; d < 2; d++) {
            UINT input = (previous >> (shift + 2 * source + d)) & 1;
            composed |= ((table >> (2 * source + input)) & 1) << (2 * source + d);
        }
    }
    previous = (previous & ~(15u << shift)) | (composed << shift);
    *coverage = (*coverage & 1) | 2 | (USHORT)(previous << 2);
}

static DWORD gdi_pen_rop_composite(USHORT coverage, DWORD pen,
                                    DWORD background, DWORD destination)
{
    DWORD result = 0;
    UINT table = coverage >> 2;
    for (UINT pair = 0; pair < 4; pair++) {
        DWORD selected = (pair & 2 ? pen : ~pen) &
                         (pair & 1 ? background : ~background);
        for (UINT d = 0; d < 2; d++) {
            UINT after_background = (table >> (2 * (pair & 1) + d)) & 1;
            UINT after_pen = (table >> (4 + 2 * (pair >> 1) + after_background)) & 1;
            if (after_pen) result |= selected & (d ? destination : ~destination);
        }
    }
    return 0xFF000000u | (result & 0xFFFFFFu);
}

static BOOL gdi_pen_style_at(const GDI_PEN *pen, double distance,
                               double *remaining)
{
    DWORD style = pen->style & 15;
    if (!style || style == 6 || (!pen->extended && pen->width > 1 && style < 5)) {
        *remaining = 1.0e30;
        return TRUE;
    }
    if (style == 5) { *remaining = 1.0e30; return FALSE; }
    static const DWORD dash[] = {3, 1}, cosmetic_dash[] = {6, 2};
    static const DWORD dot[] = {1, 1}, dashdot[] = {3, 1, 1, 1};
    static const DWORD cosmetic_dashdot[] = {3, 2, 1, 2};
    static const DWORD dashdotdot[] = {3, 1, 1, 1, 1, 1};
    const DWORD *pattern = dot;
    UINT count = 2;
    BOOL geometric = (pen->style & 0x10000) != 0;
    double unit = geometric ? (pen->width ? pen->width : 1) : 3;
    if (style == 1) pattern = geometric ? dash : cosmetic_dash;
    if (style == 3) { pattern = geometric ? dashdot : cosmetic_dashdot; count = 4; }
    if (style == 4) { pattern = dashdotdot; count = 6; }
    if (style == 7) { pattern = pen->styles; count = pen->style_count; unit = 1; }
    if (style == 8) unit = 1;
    double period = 0;
    for (UINT i = 0; i < count; i++) period += pattern[i] * unit;
    if (count & 1) period *= 2;
    if (!period) { *remaining = 1.0e30; return FALSE; }
    distance -= (uint64_t)(distance / period) * period;
    for (UINT i = 0; i < count * 2; i++) {
        double length = pattern[i % count] * unit;
        if (distance < length) { *remaining = length - distance; return !(i & 1); }
        distance -= length;
    }
    *remaining = 1;
    return TRUE;
}

typedef struct { double x, y; } GDI_STROKE_POINT;

static double gdi_stroke_length(double x, double y)
{
    double result, square = x * x + y * y;
    __asm__ ("sqrtsd %1, %0" : "=x"(result) : "x"(square));
    return result;
}

static GDI_STROKE_POINT gdi_stroke_device_point(const GDI_DC *dc,
                                                GDI_STROKE_POINT point)
{
    if (dc->graphics_mode == 2) {
        const float *m = dc->world_transform;
        double x = point.x * m[0] + point.y * m[2] + m[4];
        point.y = point.x * m[1] + point.y * m[3] + m[5];
        point.x = x;
    }
    point.x = (point.x - dc->window_x) * dc->viewport_cx / dc->window_cx + dc->viewport_x;
    point.y = (point.y - dc->window_y) * dc->viewport_cy / dc->window_cy + dc->viewport_y;
    return point;
}

/* Convex pieces of the widened path are unioned before applying the ROP.
 * Intersect scan rows with their original edges, so clipping does not alter
 * the slope or create artificial end caps at the edge of the destination. */
static void gdi_mask_polygon(GDI_SHAPE_MASK *mask, const GDI_DC *dc,
                               const GDI_STROKE_POINT *vertices, UINT count)
{
    GDI_STROKE_POINT points[64];
    if (count < 3 || count > 64) return;
    double top = 0, bottom = 0;
    for (UINT i = 0; i < count; i++) {
        points[i] = gdi_stroke_device_point(dc, vertices[i]);
        /* Path geometry uses 28.4 device coordinates. Keep remote values
         * in floating point until clipping, avoiding integer overflow. */
        double *coordinates[] = {&points[i].x, &points[i].y};
        for (UINT axis = 0; axis < 2; axis++) {
            double value = *coordinates[axis] * 16 + 0.5;
            if (value > -1.0e15 && value < 1.0e15) {
                int64_t fixed = (int64_t)value;
                if (fixed > value) fixed--;
                *coordinates[axis] = (double)fixed / 16;
            }
        }
        if (!i || points[i].y < top) top = points[i].y;
        if (!i || points[i].y > bottom) bottom = points[i].y;
    }
    if (top < mask->top) top = mask->top;
    if (bottom > (double)mask->top + mask->height) bottom = (double)mask->top + mask->height;
    if (!(top < bottom)) return;
    int first = (int)top;
    if (first < top) first++;
    for (int y = first; y < bottom; y++) {
        double left = 0, right = 0;
        BOOL found = FALSE;
        for (UINT i = 0; i < count; i++) {
            GDI_STROKE_POINT a = points[i], b = points[(i + 1) % count];
            if (a.y > b.y) { GDI_STROKE_POINT swap = a; a = b; b = swap; }
            if (y < a.y || y >= b.y) continue;
            double x = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (!found || x < left) left = x;
            if (!found || x > right) right = x;
            found = TRUE;
        }
        if (left < mask->left) left = mask->left;
        if (right > (double)mask->left + mask->width) right = (double)mask->left + mask->width;
        if (!found || !(left < right)) continue;
        int start = (int)left;
        if (start < left) start++;
        for (int x = start; x < right; x++) gdi_mask_visit(mask, x, y, FALSE, TRUE);
    }
}

static void gdi_mask_disc(GDI_SHAPE_MASK *mask, const GDI_DC *dc,
                            GDI_STROKE_POINT center, double radius)
{
    GDI_STROKE_POINT origin = gdi_stroke_device_point(dc, center);
    GDI_STROKE_POINT horizontal = gdi_stroke_device_point(dc,
        (GDI_STROKE_POINT){center.x + radius, center.y});
    GDI_STROKE_POINT vertical = gdi_stroke_device_point(dc,
        (GDI_STROKE_POINT){center.x, center.y + radius});
    double extent = gdi_stroke_length(horizontal.x - origin.x, horizontal.y - origin.y);
    double other = gdi_stroke_length(vertical.x - origin.x, vertical.y - origin.y);
    if (other > extent) extent = other;
    UINT count = 4;
    double cosine = 0, sine = 1;
    while (count < 64) {
        double half_cosine = (1 + cosine) * 0.5;
        __asm__ ("sqrtsd %1, %0" : "=x"(half_cosine) : "x"(half_cosine));
        if (extent * (1 - half_cosine) <= 0.5) break;
        sine = (1 - cosine) * 0.5;
        __asm__ ("sqrtsd %1, %0" : "=x"(sine) : "x"(sine));
        cosine = half_cosine;
        count *= 2;
    }
    GDI_STROKE_POINT points[64];
    double x = radius, y = 0;
    for (UINT i = 0; i < count; i++) {
        points[i] = (GDI_STROKE_POINT){center.x + x, center.y + y};
        double next = x * cosine - y * sine;
        y = x * sine + y * cosine;
        x = next;
    }
    gdi_mask_polygon(mask, dc, points, count);
}

static void gdi_stroke_segment(GDI_SHAPE_MASK *mask, const GDI_DC *dc,
                                 GDI_STROKE_POINT a, GDI_STROKE_POINT b,
                                 double radius, UINT start_cap, UINT end_cap)
{
    double dx = b.x - a.x, dy = b.y - a.y;
    double length = gdi_stroke_length(dx, dy);
    if (!length) {
        if (!start_cap || !end_cap) gdi_mask_disc(mask, dc, a, radius);
        return;
    }
    double ux = dx / length, uy = dy / length;
    double nx = -uy * radius, ny = ux * radius;
    if (radius == 0.5 && dc->graphics_mode != 2 &&
        dc->window_cx == dc->viewport_cx && dc->window_cy == dc->viewport_cy) {
        /* A one-pixel geometric nib flattens to a diamond. Its support
         * vector follows the major axis, unlike an ideal circular nib. */
        double ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        nx = ax >= ay ? 0 : dy < 0 ? radius : -radius;
        ny = ax >= ay ? dx < 0 ? -radius : radius : 0;
    }
    GDI_STROKE_POINT first = a, last = b;
    if (start_cap == 1) { a.x -= ux * radius; a.y -= uy * radius; }
    if (end_cap == 1) { b.x += ux * radius; b.y += uy * radius; }
    GDI_STROKE_POINT corners[] = {
        {a.x + nx, a.y + ny},
        {b.x + nx, b.y + ny},
        {b.x - nx, b.y - ny},
        {a.x - nx, a.y - ny}
    };
    gdi_mask_polygon(mask, dc, corners, 4);
    if (!start_cap) gdi_mask_disc(mask, dc, first, radius);
    if (!end_cap) gdi_mask_disc(mask, dc, last, radius);
}

static void gdi_stroke_join(GDI_SHAPE_MASK *mask, const GDI_DC *dc,
                              GDI_STROKE_POINT a, GDI_STROKE_POINT vertex,
                              GDI_STROKE_POINT b, double radius, UINT join)
{
    double ax = vertex.x - a.x, ay = vertex.y - a.y;
    double bx = b.x - vertex.x, by = b.y - vertex.y;
    double al = gdi_stroke_length(ax, ay), bl = gdi_stroke_length(bx, by);
    if (!al || !bl) return;
    ax /= al; ay /= al; bx /= bl; by /= bl;
    double cross = ax * by - ay * bx;
    if (!cross) return;
    if (!join) { gdi_mask_disc(mask, dc, vertex, radius); return; }
    double side = cross > 0 ? -radius : radius;
    GDI_STROKE_POINT outline[4] = {
        {vertex.x - ay * side, vertex.y + ax * side}, vertex,
        {vertex.x - by * side, vertex.y + bx * side}
    };
    UINT count = 3;
    if (join == 2) {
        double t = ((outline[2].x - outline[0].x) * by -
                    (outline[2].y - outline[0].y) * bx) / cross;
        GDI_STROKE_POINT miter = {outline[0].x + t * ax, outline[0].y + t * ay};
        if (gdi_stroke_length(miter.x - vertex.x, miter.y - vertex.y) <= 10 * radius) {
            /* Retain the wedge between the two segments as well as the
             * miter tip. Replacing the vertex leaves a triangular hole. */
            outline[3] = vertex;
            outline[1] = miter; /* Default GDI miter limit. */
            count = 4;
        }
    }
    gdi_mask_polygon(mask, dc, outline, count);
}

static BOOL gdi_geometric_stroke_points(GDI_SHAPE_MASK *mask, const GDI_DC *dc,
                                   const GDI_STROKE_POINT *points, UINT count,
                                   BOOL closed, const GDI_PEN *pen)
{
    double radius = (pen->width ? pen->width : 1) * 0.5;
    UINT cap = (pen->style >> 8) & 15, join = (pen->style >> 12) & 15;
    GDI_STROKE_POINT origin = gdi_stroke_device_point(dc, (GDI_STROKE_POINT){0, 0});
    GDI_STROKE_POINT horizontal = gdi_stroke_device_point(dc, (GDI_STROKE_POINT){radius, 0});
    GDI_STROKE_POINT vertical = gdi_stroke_device_point(dc, (GDI_STROKE_POINT){0, radius});
    double hx = horizontal.x - origin.x, hy = horizontal.y - origin.y;
    double vx = vertical.x - origin.x, vy = vertical.y - origin.y;
    if (hx < 0) hx = -hx;
    if (hy < 0) hy = -hy;
    if (vx < 0) vx = -vx;
    if (vy < 0) vy = -vy;
    double padding[] = {hx + vx + 1, hy + vy + 1};
    double phase = 0;
    UINT budget = 1048576;
    for (UINT i = 0; i < count - 1 + (UINT)closed; i++) {
        GDI_STROKE_POINT a = {points[i].x, points[i].y};
        GDI_STROKE_POINT b = {points[(i + 1) % count].x, points[(i + 1) % count].y};
        double dx = b.x - a.x, dy = b.y - a.y, length = gdi_stroke_length(dx, dy);
        if (!length) continue;
        GDI_STROKE_POINT first = gdi_stroke_device_point(dc, a);
        GDI_STROKE_POINT last = gdi_stroke_device_point(dc, b);
        double begin = 0, end = 1;
        for (UINT axis = 0; axis < 2; axis++) {
            double start = axis ? first.y : first.x;
            double delta = (axis ? last.y : last.x) - start;
            double low = (axis ? mask->top : mask->left) - padding[axis];
            double high = (axis ? mask->top + mask->height : mask->left + mask->width) + padding[axis];
            if (!delta) {
                if (start < low || start > high) end = -1;
                continue;
            }
            double lo = (low - start) / delta, hi = (high - start) / delta;
            if (lo > hi) { double swap = lo; lo = hi; hi = swap; }
            if (lo > begin) begin = lo;
            if (hi < end) end = hi;
        }
        double at = begin * length, stop = end * length;
        while (at < stop) {
            if (!budget--) { SetLastError(8); return FALSE; }
            double remaining;
            BOOL foreground = gdi_pen_style_at(pen, phase + at, &remaining);
            if (remaining > stop - at) remaining = stop - at;
            if (!(remaining > 0) || at + remaining == at) { SetLastError(87); return FALSE; }
            if (foreground) {
                UINT start_cap = cap, end_cap = cap;
                double unused;
                if (at <= 0 && (i || closed) &&
                    gdi_pen_style_at(pen, phase > 0 ? phase - 0.000001 : 0, &unused)) start_cap = 2;
                if (at + remaining >= length && (i + 2 < count || closed) &&
                    gdi_pen_style_at(pen, phase + length + 0.000001, &unused)) end_cap = 2;
                gdi_stroke_segment(mask, dc,
                    (GDI_STROKE_POINT){a.x + dx * at / length, a.y + dy * at / length},
                    (GDI_STROKE_POINT){a.x + dx * (at + remaining) / length,
                                       a.y + dy * (at + remaining) / length},
                    radius, start_cap, end_cap);
            }
            at += remaining;
        }
        phase += length;
        if (i + 2 < count || closed) {
            double unused;
            if (gdi_pen_style_at(pen, phase - 0.000001, &unused) &&
                gdi_pen_style_at(pen, phase + 0.000001, &unused)) {
                GDI_STROKE_POINT next = points[(i + 2) % count];
                gdi_stroke_join(mask, dc, a, b, (GDI_STROKE_POINT){next.x, next.y}, radius, join);
            }
        }
    }
    return TRUE;
}

static BOOL gdi_geometric_stroke(GDI_SHAPE_MASK *mask, const GDI_DC *dc,
                                   const GDI_POINT *points, UINT count,
                                   BOOL closed, const GDI_PEN *pen)
{
    GDI_STROKE_POINT *path = kmalloc((SIZE_T)count * sizeof(*path));
    if (!path) { SetLastError(8); return FALSE; }
    for (UINT i = 0; i < count; i++)
        path[i] = (GDI_STROKE_POINT){points[i].x, points[i].y};
    BOOL result = gdi_geometric_stroke_points(mask, dc, path, count, closed, pen);
    kfree(path);
    return result;
}

static BOOL gdi_pen_pattern(const GDI_PEN *pen, const GDI_DC *dc, int x, int y,
                              DWORD *color, BOOL *visible)
{
    *visible = TRUE;
    if (pen->brush_style == 2) {
        int px = gdi_pattern_coordinate(x, dc->brush_x, 8);
        int py = gdi_pattern_coordinate(y, dc->brush_y, 8);
        BOOL ink = pen->hatch == 0 ? py == 3 : pen->hatch == 1 ? px == 4 :
                   pen->hatch == 2 ? px == py : pen->hatch == 3 ? px + py == 7 :
                   pen->hatch == 4 ? px == 4 || py == 3 : px == py || px + py == 7;
        if (!ink) {
            *visible = dc->bk_mode == 2;
            *color = gdi_colorref_to_argb(dc->bk_color);
        }
    } else if (pen->brush_style == 3) {
        int px = gdi_pattern_coordinate(x, dc->brush_x, pen->pattern.width);
        int py = gdi_pattern_coordinate(y, dc->brush_y, pen->pattern.height);
        if (!gdi_read_bitmap_pixel(&pen->pattern, px, py, color)) return FALSE;
        if (pen->pattern.bpp == 1)
            *color = gdi_colorref_to_argb((*color & 0xFFFFFFu) ? dc->bk_color : dc->text_color);
    }
    return TRUE;
}

static BOOL gdi_shape_composite(GDI_SHAPE_MASK *mask, GDI_DC *dc,
                                   const GDI_PEN *pen, const GDI_BRUSH *brush)
{
    GDI_RECT bounds = {mask->left, mask->top,
        mask->left + mask->width, mask->top + mask->height};
    DWORD pen_rgb = 0, brush_rgb = 0;
    BOOL ok = gdi_resolve_color(dc, pen->color, &pen_rgb) &&
         gdi_resolve_color(dc, brush->color, &brush_rgb);
    BOOL wrote = FALSE;
    for (int y = bounds.top; ok && y < bounds.bottom; y++) {
        for (int x = bounds.left; x < bounds.right; x++) {
            USHORT coverage = mask->pixels[(SIZE_T)(y - bounds.top) * mask->width + x - bounds.left];
            if (!coverage || !gdi_dc_point_visible(dc, x, y)) continue;
            DWORD color = gdi_colorref_to_argb(brush_rgb);
            if ((coverage & 1) && brush->style == 3) {
                int px = gdi_pattern_coordinate(x, dc->brush_x, brush->pattern.width);
                int py = gdi_pattern_coordinate(y, dc->brush_y, brush->pattern.height);
                if (!gdi_read_bitmap_pixel(&brush->pattern, px, py, &color)) { ok = FALSE; break; }
                if (brush->pattern.bpp == 1) color = gdi_colorref_to_argb((color & 0xFFFFFFu) ? dc->bk_color : dc->text_color);
            }
            DWORD destination;
            if (!gdi_read_dc_pixel(dc, x, y, &destination)) { ok = FALSE; break; }
            if (coverage & 1) destination = gdi_apply_rop2(color, destination, dc->rop2);
            if (coverage & 2) {
                color = gdi_colorref_to_argb(pen_rgb);
                BOOL visible;
                if (!gdi_pen_pattern(pen, dc, x, y, &color, &visible)) { ok = FALSE; break; }
                if (visible) destination = gdi_pen_rop_composite(coverage, color,
                        gdi_colorref_to_argb(dc->bk_color), destination);
            }
            if (!gdi_write_dc_pixel(dc, x, y, destination)) { ok = FALSE; break; }
            wrote = TRUE;
        }
    }
    if (wrote) mark_dc_dirty(dc);
    return ok;
}

/* Iterate only visible major-axis coordinates. Half-open segments avoid
 * repeating the endpoint at each contour vertex. Ties go toward the smaller
 * device coordinate, independently of segment direction. */
static void gdi_pen_line(GDI_SHAPE_MASK *mask, GDI_POINT a, GDI_POINT b,
                           const GDI_PEN *pen, const GDI_DC *dc,
                           uint64_t *phase)
{
    BOOL geometric = (pen->style & 0x10000) != 0;
    if (geometric) {
        int64_t x = (int64_t)b.x - a.x, y = (int64_t)b.y - a.y;
        if (x < 0) x = -x;
        if (y < 0) y = -y;
        if ((x >= y && a.x > b.x) || (x < y && a.y > b.y)) {
            GDI_POINT swap = a; a = b; b = swap;
        }
    }
    int64_t dx = (int64_t)b.x - a.x, dy = (int64_t)b.y - a.y;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    BOOL horizontal = dx >= dy;
    int64_t major = horizontal ? dx : dy, minor = horizontal ? dy : dx;
    if (!major) return;
    int64_t origin = horizontal ? a.x : a.y;
    int sign = horizontal ? sx : sy;
    int64_t minimum = horizontal ? mask->left : mask->top;
    int64_t maximum = minimum + (horizontal ? mask->width : mask->height) - 1;
    int64_t first = sign > 0 ? minimum - origin : origin - maximum;
    int64_t last = sign > 0 ? maximum - origin : origin - minimum;
    if (first < 0) first = 0;
    if (last >= major) last = major - 1;
    for (int64_t step = first; step <= last; step++) {
        uint64_t product = (uint64_t)step * (uint64_t)minor;
        int minor_sign = horizontal ? sy : sx;
        int64_t secondary = (int64_t)(product / (uint64_t)major);
        uint64_t remainder = product % (uint64_t)major;
        if (remainder * 2 > (uint64_t)major ||
            (remainder * 2 == (uint64_t)major &&
             (geometric ? minor_sign > 0 : minor_sign < 0))) secondary++;
        int64_t x = (int64_t)a.x + sx * (horizontal ? step : secondary);
        int64_t y = (int64_t)a.y + sy * (horizontal ? secondary : step);
        double remaining;
        BOOL foreground = gdi_pen_style_at(pen, (double)(*phase + step), &remaining);
        if (foreground || (!pen->extended && dc->bk_mode == 2))
            gdi_mask_visit(mask, (int)x, (int)y, !foreground, geometric);
    }
    *phase += (uint64_t)major;
}

/* operation: 0 Polyline, 1 (Poly)Polygon, 2 LineTo with local points.
 * Collect coverage first, then apply the fill followed by every pen visit.
 * No destination changes on validation errors. */
static BOOL gdi_polygon_draw(HDC hdc, const GDI_POINT *points,
                             const int *counts, int polygons, int operation)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    if (!dc) { SetLastError(6); return FALSE; }
    if (!points || !counts || !polygons) return FALSE;
    if (polygons < 0) { SetLastError(87); return FALSE; }
    int *sizes = kmalloc((SIZE_T)polygons * sizeof(*sizes));
    if (!sizes) { SetLastError(8); return FALSE; }
    gdi_memcpy(sizes, counts, (SIZE_T)polygons * sizeof(*sizes));
    UINT vertices = 0;
    BOOL ok = TRUE;
    for (int i = 0; i < polygons; i++) {
        if (sizes[i] < 2) { SetLastError(87); ok = FALSE; break; }
        if ((UINT)sizes[i] > 0x7FFFFFFFu - vertices) { SetLastError(8); ok = FALSE; break; }
        vertices += (UINT)sizes[i];
    }
    if (!ok) { kfree(sizes); return FALSE; }
    SIZE_T point_bytes = (SIZE_T)vertices * sizeof(GDI_POINT);
    if (operation != 2 && !win32_user_range_readable(points, point_bytes, g_compat32_mode)) {
        kfree(sizes); return FALSE;
    }
    GDI_POINT *mapped = kmalloc(point_bytes);
    GDI_POINT *logical = kmalloc(point_bytes);
    GDI_POLYGON_CROSSING *crossings = kmalloc((SIZE_T)vertices * sizeof(*crossings));
    if (!mapped || !logical || !crossings) {
        kfree(sizes); kfree(mapped); kfree(logical); kfree(crossings);
        SetLastError(8); return FALSE;
    }
    gdi_memcpy(logical, points, point_bytes);
    GDI_RECT bounds = {0, 0, 0, 0};
    for (UINT i = 0; i < vertices; i++) {
        if (!gdi_transform_point(dc, logical[i], &mapped[i], FALSE)) {
            ok = FALSE; SetLastError(87); break;
        }
        if (!i || mapped[i].x < bounds.left) bounds.left = mapped[i].x;
        if (!i || mapped[i].y < bounds.top) bounds.top = mapped[i].y;
        if (!i || mapped[i].x >= bounds.right) bounds.right = mapped[i].x + 1;
        if (!i || mapped[i].y >= bounds.bottom) bounds.bottom = mapped[i].y + 1;
    }
    GDI_PEN pen;
    GDI_BRUSH brush;
    BOOL pen_held = ok && gdi_pen_acquire(dc->current_pen, dc, &pen);
    BOOL brush_held = pen_held && brush_acquire(dc->current_brush, dc, &brush);
    ok = ok && pen_held && brush_held;
    BOOL geometric = ok && (pen.width > 1 ||
        ((pen.style & 0x10000) && pen.width &&
         ((pen.style & 15) || dc->graphics_mode == 2 ||
          dc->window_cx != dc->viewport_cx || dc->window_cy != dc->viewport_cy)));
    if (ok && pen.brush_style == 2 && pen.hatch > 5) { SetLastError(120); ok = FALSE; }
    if (ok && (dc->rop2 < 1 || dc->rop2 > 16)) { SetLastError(87); ok = FALSE; }
    if (geometric) {
        GDI_STROKE_POINT origin = gdi_stroke_device_point(dc, (GDI_STROKE_POINT){0, 0});
        GDI_STROKE_POINT x = gdi_stroke_device_point(dc, (GDI_STROKE_POINT){pen.width, 0});
        GDI_STROKE_POINT y = gdi_stroke_device_point(dc, (GDI_STROKE_POINT){0, pen.width});
        x.x -= origin.x; x.y -= origin.y; y.x -= origin.x; y.y -= origin.y;
        if (x.x < 0) x.x = -x.x;
        if (x.y < 0) x.y = -x.y;
        if (y.x < 0) y.x = -y.x;
        if (y.y < 0) y.y = -y.y;
        double px = 5 * (x.x + y.x) + 2, py = 5 * (x.y + y.y) + 2;
        double left = bounds.left - px, right = bounds.right + px;
        double top = bounds.top - py, bottom = bounds.bottom + py;
        bounds.left = left < 0 ? 0 : left > dc->width ? dc->width : (int)left;
        bounds.top = top < 0 ? 0 : top > dc->height ? dc->height : (int)top;
        bounds.right = right < 0 ? 0 : right >= dc->width ? dc->width : (int)right + 1;
        bounds.bottom = bottom < 0 ? 0 : bottom >= dc->height ? dc->height : (int)bottom + 1;
    }
    if (bounds.left < 0) bounds.left = 0;
    if (bounds.top < 0) bounds.top = 0;
    if (bounds.right > dc->width) bounds.right = dc->width;
    if (bounds.bottom > dc->height) bounds.bottom = dc->height;
    GDI_SHAPE_MASK mask = {NULL, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top, dc->rop2};
    SIZE_T area = 0;
    if (ok && dc->surface && mask.width > 0 && mask.height > 0) {
        if (__builtin_mul_overflow((SIZE_T)mask.width, (SIZE_T)mask.height, &area) ||
            area > (SIZE_T)-1 / sizeof(*mask.pixels) ||
            !(mask.pixels = kmalloc(area * sizeof(*mask.pixels)))) { SetLastError(8); ok = FALSE; }
    }
    if (ok && area) {
        gdi_memset(mask.pixels, 0, area * sizeof(*mask.pixels));
        UINT offset = 0;
        for (int polygon = 0; ok && (pen.style & 15) != 5 && polygon < polygons; polygon++) {
            int count = sizes[polygon];
            if (geometric) {
                ok = gdi_geometric_stroke(&mask, dc, logical + offset,
                                           (UINT)count, operation == 1, &pen);
                offset += (UINT)count;
                continue;
            }
            uint64_t phase = 0;
            for (int i = 0; i < count - 1; i++)
                gdi_pen_line(&mask, mapped[offset + i], mapped[offset + i + 1],
                             &pen, dc, &phase);
            if (operation == 1) gdi_pen_line(&mask, mapped[offset + count - 1],
                                           mapped[offset], &pen, dc, &phase);
            if ((pen.style & 0x10000) && !(pen.style & 15)) {
                if (operation == 1 || !(pen.style & 0xF000)) {
                    for (int i = operation == 1 ? 0 : 1;
                         i < (operation == 1 ? count : count - 1); i++)
                        gdi_mask_visit(&mask, mapped[offset + i].x,
                                       mapped[offset + i].y, FALSE, TRUE);
                }
                if (operation != 1 && (!(pen.style & 0xF00) ||
                    (pen.width && (pen.style & 0xF00) == 0x100))) {
                    gdi_mask_visit(&mask, mapped[offset].x, mapped[offset].y, FALSE, TRUE);
                    gdi_mask_visit(&mask, mapped[offset + count - 1].x,
                                   mapped[offset + count - 1].y, FALSE, TRUE);
                }
            }
            offset += (UINT)count;
        }
        for (int y = bounds.top; ok && operation == 1 && brush.style != 1 && y < bounds.bottom; y++) {
            UINT crossing_count = 0;
            offset = 0;
            for (int polygon = 0; polygon < polygons; polygon++) {
                int count = sizes[polygon];
                for (int i = 0; i < count; i++) {
                    GDI_POINT a = mapped[offset + i], b = mapped[offset + (i + 1 == count ? 0 : i + 1)];
                    int winding = a.y < b.y ? 1 : -1;
                    if (a.y > b.y) { GDI_POINT swap = a; a = b; b = swap; }
                    if (y < a.y || y >= b.y) continue;
                    int64_t dy = (int64_t)b.y - a.y;
                    int64_t numerator = (int64_t)a.x * dy + (int64_t)(y - a.y) * (b.x - a.x);
                    LONG x = (LONG)(numerator >= 0 ? (numerator + dy - 1) / dy : numerator / dy);
                    crossings[crossing_count++] = (GDI_POLYGON_CROSSING){x, winding};
                }
                offset += (UINT)count;
            }
            polygon_sort_crossings(crossings, crossing_count);
            int winding = 0, start = 0;
            for (UINT i = 0; i < crossing_count; ) {
                int x = crossings[i].x;
                BOOL before = dc->poly_fill_mode == 1 ? (winding & 1) != 0 : winding != 0;
                do { winding += crossings[i++].winding; } while (i < crossing_count && crossings[i].x == x);
                BOOL after = dc->poly_fill_mode == 1 ? (winding & 1) != 0 : winding != 0;
                if (!before && after) start = x;
                if (before && !after) {
                    int end = x < bounds.right ? x : bounds.right;
                    if (start < bounds.left) start = bounds.left;
                    for (int column = start; column < end; column++)
                        mask.pixels[(SIZE_T)(y - bounds.top) * mask.width + column - bounds.left] |= 1;
                }
            }
        }
        ok = ok && gdi_shape_composite(&mask, dc, &pen, &brush);
    }
    if (ok && operation == 2) gdi_draw_commit_position(&draw, dc, logical[vertices - 1].x, logical[vertices - 1].y);
    if (brush_held) brush_release(dc->current_brush);
    if (pen_held) gdi_pen_release(dc->current_pen);
    kfree(mask.pixels); kfree(crossings); kfree(mapped); kfree(logical); kfree(sizes);
    return ok;
}

static BOOL WINAPI Polygon(HDC hdc, const GDI_POINT *points, int count)
{
    if (count < 0) return FALSE;
    return gdi_polygon_draw(hdc, points, &count, 1, 1);
}

static BOOL WINAPI PolyPolygon(HDC hdc, const GDI_POINT *points, const int *counts, int polygons)
{
    if (polygons > 0 && counts && !win32_user_range_readable(counts,
            (SIZE_T)polygons * sizeof(*counts), g_compat32_mode)) return FALSE;
    return gdi_polygon_draw(hdc, points, counts, polygons, 1);
}

typedef struct {
    GDI_STROKE_POINT *points;
    UINT count, capacity;
    double left, top, right, bottom;
} GDI_ARC_PATH;

static GDI_STROKE_POINT gdi_arc_point(GDI_STROKE_POINT point)
{
    double *coordinates[] = {&point.x, &point.y};
    for (UINT axis = 0; axis < 2; axis++) {
        double value = *coordinates[axis] * 16 + 0.5;
        if (value > -1.0e15 && value < 1.0e15) {
            int64_t fixed = (int64_t)value;
            if (fixed > value) fixed--;
            *coordinates[axis] = (double)fixed / 16;
        }
    }
    return point;
}

static BOOL gdi_arc_append(GDI_ARC_PATH *path, GDI_STROKE_POINT point)
{
    point = gdi_arc_point(point);
    if (path->count && path->points[path->count - 1].x == point.x &&
        path->points[path->count - 1].y == point.y) return TRUE;
    if (path->count == path->capacity) {
        UINT capacity = path->capacity ? path->capacity * 2 : 16;
        if (capacity > 16384) { SetLastError(8); return FALSE; }
        GDI_STROKE_POINT *points = kmalloc((SIZE_T)capacity * sizeof(*points));
        if (!points) { SetLastError(8); return FALSE; }
        if (path->count) gdi_memcpy(points, path->points, (SIZE_T)path->count * sizeof(*points));
        kfree(path->points);
        path->points = points;
        path->capacity = capacity;
    }
    path->points[path->count++] = point;
    return TRUE;
}

static BOOL gdi_arc_flatten(GDI_ARC_PATH *path, const GDI_STROKE_POINT p[4], UINT depth)
{
    BOOL left = TRUE, right = TRUE, above = TRUE, below = TRUE;
    for (UINT i = 0; i < 4; i++) {
        left = left && p[i].x < path->left;
        right = right && p[i].x > path->right;
        above = above && p[i].y < path->top;
        below = below && p[i].y > path->bottom;
    }
    /* A cubic stays inside its control hull. Outside a common clipping
     * half-plane its chord cannot affect either the fill or the stroke. */
    if (left || right || above || below) return gdi_arc_append(path, p[3]);
    GDI_STROKE_POINT q[3], r[2], middle;
    for (UINT i = 0; i < 3; i++)
        q[i] = (GDI_STROKE_POINT){(p[i].x + p[i + 1].x) * 0.5,
                                (p[i].y + p[i + 1].y) * 0.5};
    for (UINT i = 0; i < 2; i++)
        r[i] = (GDI_STROKE_POINT){(q[i].x + q[i + 1].x) * 0.5,
                                (q[i].y + q[i + 1].y) * 0.5};
    middle = (GDI_STROKE_POINT){(r[0].x + r[1].x) * 0.5, (r[0].y + r[1].y) * 0.5};
    /* Bound the entire cubic's deviation from its chord, not only the
     * midpoint. The second derivative is six times a linear interpolation
     * of these differences; its chord error is at most 6/8 of the maximum.
     * A half-pixel tolerance therefore permits second differences <= 2/3. */
    BOOL flat = TRUE;
    for (UINT i = 0; i < 2; i++) {
        double dx = p[i].x - 2 * p[i + 1].x + p[i + 2].x;
        double dy = p[i].y - 2 * p[i + 1].y + p[i + 2].y;
        if (dx < -2.0 / 3 || dx > 2.0 / 3 ||
            dy < -2.0 / 3 || dy > 2.0 / 3) flat = FALSE;
    }
    if (flat)
        return gdi_arc_append(path, p[3]);
    if (depth == 24) { SetLastError(8); return FALSE; }
    GDI_STROKE_POINT first[] = {p[0], q[0], r[0], middle};
    GDI_STROKE_POINT last[] = {middle, r[1], q[2], p[3]};
    return gdi_arc_flatten(path, first, depth + 1) &&
           gdi_arc_flatten(path, last, depth + 1);
}

/* Monotonic within each quadrant; only ordering, not an angle in radians,
 * is needed to split the ellipse into cubic pieces of at most 90 degrees. */
static double gdi_arc_angle(GDI_STROKE_POINT point)
{
    double x = point.x, y = -point.y;
    if (y >= 0) return x >= 0 ? y / (x + y) : 1 - x / (y - x);
    return x <= 0 ? 2 + y / (x + y) : 3 + x / (x - y);
}

static BOOL gdi_arc_points(GDI_ARC_PATH *path, const GDI_DC *dc,
                              GDI_RECT box, GDI_POINT start, GDI_POINT end, BOOL pie)
{
    GDI_STROKE_POINT lower = {box.left, box.top}, upper = {box.right, box.bottom};
    GDI_STROKE_POINT radial[] = {{start.x, start.y}, {end.x, end.y}};
    BOOL device = dc->graphics_mode != 2;
    if (device) {
        lower = gdi_stroke_device_point(dc, lower);
        upper = gdi_stroke_device_point(dc, upper);
        for (UINT i = 0; i < 2; i++) radial[i] = gdi_stroke_device_point(dc, radial[i]);
    }
    if (lower.x > upper.x) { double swap = lower.x; lower.x = upper.x; upper.x = swap; }
    if (lower.y > upper.y) { double swap = lower.y; lower.y = upper.y; upper.y = swap; }
    double original_rx = (upper.x - lower.x) * 0.5;
    double original_ry = (upper.y - lower.y) * 0.5;
    if (!original_rx || !original_ry) return TRUE;
    GDI_STROKE_POINT center = {(lower.x + upper.x) * 0.5, (lower.y + upper.y) * 0.5};
    for (UINT i = 0; i < 2; i++) {
        radial[i].x = (radial[i].x - center.x) / original_rx;
        radial[i].y = (radial[i].y - center.y) / original_ry;
        double length = gdi_stroke_length(radial[i].x, radial[i].y);
        if (length) { radial[i].x /= length; radial[i].y /= length; }
        else radial[i] = (GDI_STROKE_POINT){1, 0};
    }
    double rx = original_rx, ry = original_ry;
    if (device) {
        rx -= 0.5; ry -= 0.5; center.x -= 0.5; center.y -= 0.5;
        if (rx < 0) rx = 0;
        if (ry < 0) ry = 0;
    }
    BOOL reverse = dc->arc_direction == 2;
    if (reverse) { GDI_STROKE_POINT swap = radial[0]; radial[0] = radial[1]; radial[1] = swap; }
    double position = gdi_arc_angle(radial[0]), finish = gdi_arc_angle(radial[1]);
    if (finish <= position) finish += 4;
    GDI_STROKE_POINT current = radial[0];
    GDI_STROKE_POINT initial = {center.x + rx * current.x, center.y + ry * current.y};
    if (!device) initial = gdi_stroke_device_point(dc, initial);
    if (!gdi_arc_append(path, initial)) return FALSE;
    while (position < finish) {
        double stop = (int)position + 1;
        if (stop > finish) stop = finish;
        static const GDI_STROKE_POINT axes[] = {{1,0},{0,-1},{-1,0},{0,1}};
        GDI_STROKE_POINT next = stop == finish ? radial[1] : axes[(UINT)stop & 3];
        double cosine = current.x * next.x + current.y * next.y;
        double sine = current.y * next.x - current.x * next.y;
        if (cosine > 1) cosine = 1;
        if (cosine < 0) cosine = 0;
        double root;
        double square = 2 * (1 + cosine);
        __asm__ ("sqrtsd %1, %0" : "=x"(root) : "x"(square));
        double tangent = (4.0 / 3.0) * sine / (1 + cosine + root);
        GDI_STROKE_POINT controls[] = {
            {center.x + rx * current.x, center.y + ry * current.y},
            {center.x + rx * (current.x + tangent * current.y),
             center.y + ry * (current.y - tangent * current.x)},
            {center.x + rx * (next.x - tangent * next.y),
             center.y + ry * (next.y + tangent * next.x)},
            {center.x + rx * next.x, center.y + ry * next.y}
        };
        for (UINT i = 0; i < 4; i++)
            controls[i] = gdi_arc_point(device ? controls[i] : gdi_stroke_device_point(dc, controls[i]));
        if (!gdi_arc_flatten(path, controls, 0)) return FALSE;
        position = stop; current = next;
    }
    if (reverse) for (UINT i = 0; i < path->count / 2; i++) {
        GDI_STROKE_POINT swap = path->points[i];
        path->points[i] = path->points[path->count - i - 1];
        path->points[path->count - i - 1] = swap;
    }
    return !pie || gdi_arc_append(path, device ? center : gdi_stroke_device_point(dc, center));
}

/* Fractional cosmetic segments follow the diamond exit rule: a pixel is
 * visited when the segment leaves its half-pixel diamond before the endpoint.
 * Clip the major-axis walk first; subpixel path vertices remain unrounded. */
static void gdi_path_line(GDI_SHAPE_MASK *mask, GDI_STROKE_POINT a,
                             GDI_STROKE_POINT b, const GDI_PEN *pen,
                             const GDI_DC *dc, double *phase)
{
    double dx = b.x - a.x, dy = b.y - a.y;
    double ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    BOOL horizontal = ax >= ay, geometric = (pen->style & 0x10000) != 0;
    double distance = geometric ? gdi_stroke_length(dx, dy) : horizontal ? ax : ay;
    if (!distance) return;
    a.x -= 1.0e-9; a.y -= 1.0e-10;
    double start = horizontal ? a.x : a.y, delta = horizontal ? dx : dy;
    double low = start, high = start + delta;
    if (low > high) { double swap = low; low = high; high = swap; }
    low -= 0.5; high += 0.5;
    int minimum = horizontal ? mask->left : mask->top;
    int maximum = minimum + (horizontal ? mask->width : mask->height) - 1;
    if (low < minimum) low = minimum;
    if (high > maximum) high = maximum;
    if (low <= high) {
        int first = (int)low;
        if (first < low) first++;
        for (int offset = 0; offset <= (int)high - first; offset++) {
            int major = delta > 0 ? first + offset : (int)high - offset;
            double at = (major - start) / delta;
            double projected = (horizontal ? a.y : a.x) + at * (horizontal ? dy : dx);
            int minor_min = horizontal ? mask->top : mask->left;
            int minor_max = minor_min + (horizontal ? mask->height : mask->width);
            if (projected < (double)minor_min - 2 || projected > (double)minor_max + 2) continue;
            int middle = (int)projected;
            if (middle > projected) middle--;
            for (int minor = middle - 1; minor <= middle + 2; minor++) {
                if (minor < minor_min || minor >= minor_max) continue;
                int x = horizontal ? major : minor, y = horizontal ? minor : major;
                double enter = -1.0e30, leave = 1.0e30;
                double origins[] = {a.x + a.y, a.x - a.y};
                double slopes[] = {dx + dy, dx - dy};
                double centers[] = {(double)x + y, (double)x - y};
                for (UINT axis = 0; axis < 2; axis++) {
                    if (!slopes[axis]) {
                        double difference = origins[axis] - centers[axis];
                        if (difference < -0.5 || difference > 0.5) leave = -1;
                    } else {
                        double lo = (centers[axis] - 0.5 - origins[axis]) / slopes[axis];
                        double hi = (centers[axis] + 0.5 - origins[axis]) / slopes[axis];
                        if (lo > hi) { double swap = lo; lo = hi; hi = swap; }
                        if (lo > enter) enter = lo;
                        if (hi < leave) leave = hi;
                    }
                }
                if (!(enter < leave && leave >= 0 && leave < 1)) continue;
                double remaining, along = at < 0 ? 0 : at > 1 ? 1 : at;
                BOOL foreground = gdi_pen_style_at(pen, *phase + along * distance, &remaining);
                if (foreground || (!pen->extended && dc->bk_mode == 2))
                    gdi_mask_visit(mask, x, y, !foreground, geometric);
            }
        }
    }
    *phase += distance;
}

static BOOL gdi_arc_fill(GDI_SHAPE_MASK *mask, const GDI_ARC_PATH *path, BOOL outlined)
{
    GDI_POLYGON_CROSSING *crossings = kmalloc((SIZE_T)path->count * sizeof(*crossings));
    if (!crossings) { SetLastError(8); return FALSE; }
    /* Brush-only sectors sample pixel centers. The cosmetic outline path
     * uses integer scan coordinates before composing the boundary pen. */
    double offset = outlined ? 0 : 0.5;
    for (int y = mask->top; y < mask->top + mask->height; y++) {
        UINT count = 0;
        double scan = y + offset;
        for (UINT i = 0; i < path->count; i++) {
            GDI_STROKE_POINT a = path->points[i], b = path->points[(i + 1) % path->count];
            if (a.y > b.y) { GDI_STROKE_POINT swap = a; a = b; b = swap; }
            if (scan < a.y || scan >= b.y) continue;
            double x = a.x + (scan - a.y) * (b.x - a.x) / (b.y - a.y) - offset;
            if (x < mask->left) x = mask->left;
            if (x > (double)mask->left + mask->width) x = (double)mask->left + mask->width;
            int column = (int)x;
            if (column < x) column++;
            crossings[count++] = (GDI_POLYGON_CROSSING){column, 1};
        }
        polygon_sort_crossings(crossings, count);
        for (UINT i = 0; i + 1 < count; i += 2)
            for (int x = crossings[i].x; x < crossings[i + 1].x; x++)
                mask->pixels[(SIZE_T)(y - mask->top) * mask->width + x - mask->left] |= 1;
    }
    kfree(crossings);
    return TRUE;
}

static BOOL gdi_arc_unmap(const GDI_DC *dc, GDI_STROKE_POINT *point)
{
    double x = (point->x - dc->viewport_x) * dc->window_cx / dc->viewport_cx + dc->window_x;
    double y = (point->y - dc->viewport_y) * dc->window_cy / dc->viewport_cy + dc->window_y;
    if (dc->graphics_mode == 2) {
        const float *m = dc->world_transform;
        double determinant = (double)m[0] * m[3] - (double)m[1] * m[2];
        if (!determinant) { SetLastError(87); return FALSE; }
        x -= m[4]; y -= m[5];
        *point = (GDI_STROKE_POINT){(x * m[3] - y * m[2]) / determinant,
                                  (y * m[0] - x * m[1]) / determinant};
    } else *point = (GDI_STROKE_POINT){x, y};
    return TRUE;
}

static BOOL gdi_arc_draw(HDC hdc, GDI_RECT box, GDI_POINT start, GDI_POINT end, BOOL pie)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    if (!dc) { SetLastError(6); return FALSE; }
    GDI_PEN pen;
    GDI_BRUSH brush;
    BOOL pen_held = gdi_pen_acquire(dc->current_pen, dc, &pen);
    BOOL brush_held = pen_held && brush_acquire(dc->current_brush, dc, &brush);
    BOOL ok = pen_held && brush_held;
    if (ok && (dc->rop2 < 1 || dc->rop2 > 16)) { SetLastError(87); ok = FALSE; }
    if (ok && pen.brush_style == 2 && pen.hatch > 5) { SetLastError(120); ok = FALSE; }
    GDI_ARC_PATH path = {0};
    GDI_SHAPE_MASK mask = {0};
    BOOL wide = ok && (pen.width > 1 || ((pen.style & 0x10000) && pen.width));
    double px = 2, py = 2;
    if (wide) {
        GDI_STROKE_POINT zero = gdi_stroke_device_point(dc, (GDI_STROKE_POINT){0,0});
        GDI_STROKE_POINT x = gdi_stroke_device_point(dc, (GDI_STROKE_POINT){pen.width,0});
        GDI_STROKE_POINT y = gdi_stroke_device_point(dc, (GDI_STROKE_POINT){0,pen.width});
        x.x -= zero.x; x.y -= zero.y; y.x -= zero.x; y.y -= zero.y;
        px += 5 * ((x.x < 0 ? -x.x : x.x) + (y.x < 0 ? -y.x : y.x));
        py += 5 * ((x.y < 0 ? -x.y : x.y) + (y.y < 0 ? -y.y : y.y));
    }
    path.left = -px; path.top = -py; path.right = dc->width + px; path.bottom = dc->height + py;
    if (ok && dc->surface && dc->width > 0 && dc->height > 0)
        ok = gdi_arc_points(&path, dc, box, start, end, pie);
    if (ok && path.count > 1) {
        double left = path.points[0].x, right = left, top = path.points[0].y, bottom = top;
        for (UINT i = 1; i < path.count; i++) {
            GDI_STROKE_POINT point = path.points[i];
            if (point.x < left) left = point.x;
            if (point.x > right) right = point.x;
            if (point.y < top) top = point.y;
            if (point.y > bottom) bottom = point.y;
        }
        left -= px; right += px; top -= py; bottom += py;
        int l = left < 0 ? 0 : left > dc->width ? dc->width : (int)left;
        int t = top < 0 ? 0 : top > dc->height ? dc->height : (int)top;
        int r = right < 0 ? 0 : right >= dc->width ? dc->width : (int)right + 1;
        int b = bottom < 0 ? 0 : bottom >= dc->height ? dc->height : (int)bottom + 1;
        mask = (GDI_SHAPE_MASK){NULL,l,t,r-l,b-t,dc->rop2};
        if (mask.width > 0 && mask.height > 0) {
            SIZE_T area;
            if (__builtin_mul_overflow((SIZE_T)mask.width, (SIZE_T)mask.height, &area) ||
                area > (SIZE_T)-1 / sizeof(*mask.pixels) ||
                !(mask.pixels = kmalloc(area * sizeof(*mask.pixels)))) { SetLastError(8); ok = FALSE; }
            if (ok) {
                gdi_memset(mask.pixels, 0, area * sizeof(*mask.pixels));
                if (pie && brush.style != 1)
                    ok = gdi_arc_fill(&mask, &path, (pen.style & 15) != 5);
                if (ok && (pen.style & 15) != 5) {
                    if (wide) {
                        for (UINT i = 0; ok && i < path.count; i++) ok = gdi_arc_unmap(dc, &path.points[i]);
                        if (ok) ok = gdi_geometric_stroke_points(&mask, dc, path.points, path.count, pie, &pen);
                    } else {
                        double phase = 0;
                        for (UINT i = 0; i < path.count - 1 + (UINT)pie; i++)
                            gdi_path_line(&mask, path.points[i], path.points[(i + 1) % path.count], &pen, dc, &phase);
                    }
                }
                if (ok) ok = gdi_shape_composite(&mask, dc, &pen, &brush);
            }
        }
    }
    kfree(mask.pixels); kfree(path.points);
    if (brush_held) brush_release(dc->current_brush);
    if (pen_held) gdi_pen_release(dc->current_pen);
    return ok;
}

static BOOL WINAPI Arc(HDC hdc, int left, int top, int right, int bottom,
                          int start_x, int start_y, int end_x, int end_y)
{
    return gdi_arc_draw(hdc, (GDI_RECT){left,top,right,bottom},
                        (GDI_POINT){start_x,start_y}, (GDI_POINT){end_x,end_y}, FALSE);
}

static BOOL WINAPI Pie(HDC hdc, int left, int top, int right, int bottom,
                          int start_x, int start_y, int end_x, int end_y)
{
    return gdi_arc_draw(hdc, (GDI_RECT){left,top,right,bottom},
                        (GDI_POINT){start_x,start_y}, (GDI_POINT){end_x,end_y}, TRUE);
}


typedef struct {
    const BYTE *bits;
    const BYTE *colors;
    int width;
    int height;
    int top_down;
    int bpp;
    int pitch;
    int core_header;
    DWORD compression;
    DWORD color_count;
    DWORD red_mask;
    DWORD green_mask;
    DWORD blue_mask;
} GDI_DIB_VIEW;

static UINT gdi_dib_trace_count;
static UINT gdi_frame_probe_count;

static USHORT gdi_read_u16(const BYTE *p)
{
    return (USHORT)((USHORT)p[0] | ((USHORT)p[1] << 8));
}

static DWORD gdi_read_u32(const BYTE *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) |
           ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static BOOL gdi_abs_i32(int value, int *absolute)
{
    if (!absolute || value == (-2147483647 - 1)) return FALSE;
    *absolute = value < 0 ? -value : value;
    return TRUE;
}

static BOOL gdi_valid_color_masks(int bpp, DWORD red, DWORD green, DWORD blue)
{
    DWORD masks[3] = { red, green, blue };
    if ((red & green) || (red & blue) || (green & blue)) return FALSE;
    for (UINT i = 0; i < 3; i++) {
        DWORD mask = masks[i];
        if (!mask || (bpp == 16 && (mask >> 16))) return FALSE;
        mask >>= __builtin_ctz(mask);
        if (mask & (mask + 1u)) return FALSE;
    }
    return TRUE;
}

static BOOL gdi_parse_dib(PCVOID bitmap_info, PCVOID bits, UINT usage,
                          GDI_DIB_VIEW *view)
{
    if (!bitmap_info || !view || usage != GDI_DIB_RGB_COLORS)
        return FALSE;

    const BYTE *info = (const BYTE *)bitmap_info;
    DWORD header_size = gdi_read_u32(info);
    gdi_memset(view, 0, sizeof(*view));
    view->bits = (const BYTE *)bits;

    int raw_height;
    if (header_size == 12) {
        view->core_header = 1;
        view->width = (int)gdi_read_u16(info + 4);
        raw_height = (int)gdi_read_u16(info + 6);
        if (gdi_read_u16(info + 8) != 1) return FALSE;
        view->bpp = (int)gdi_read_u16(info + 10);
        view->compression = GDI_BI_RGB;
        view->colors = info + 12;
    } else if (header_size >= 40) {
        view->width = (int)(LONG)gdi_read_u32(info + 4);
        raw_height = (int)(LONG)gdi_read_u32(info + 8);
        if (gdi_read_u16(info + 12) != 1) return FALSE;
        view->bpp = (int)gdi_read_u16(info + 14);
        view->compression = gdi_read_u32(info + 16);
        view->color_count = gdi_read_u32(info + 32);
        view->colors = info + header_size;

        if (view->compression == GDI_BI_BITFIELDS ||
            view->compression == GDI_BI_ALPHABITFIELDS) {
            const BYTE *masks;
            if (header_size >= 52) {
                masks = info + 40;
            } else if (header_size == 40) {
                masks = info + 40;
                view->colors += view->compression == GDI_BI_ALPHABITFIELDS
                              ? 16 : 12;
            } else {
                return FALSE;
            }
            view->red_mask = gdi_read_u32(masks);
            view->green_mask = gdi_read_u32(masks + 4);
            view->blue_mask = gdi_read_u32(masks + 8);
            if (!gdi_valid_color_masks(view->bpp, view->red_mask,
                                        view->green_mask, view->blue_mask))
                return FALSE;
        } else if (view->compression != GDI_BI_RGB) {
            return FALSE;
        }
    } else {
        return FALSE;
    }

    if (view->width <= 0 || !raw_height ||
        !gdi_abs_i32(raw_height, &view->height))
        return FALSE;
    view->top_down = raw_height < 0;

    if (view->bpp != 1 && view->bpp != 4 && view->bpp != 8 &&
        view->bpp != 16 && view->bpp != 24 && view->bpp != 32)
        return FALSE;
    if ((view->compression == GDI_BI_BITFIELDS ||
         view->compression == GDI_BI_ALPHABITFIELDS) &&
        view->bpp != 16 && view->bpp != 32)
        return FALSE;

    uint64_t row_bits = (uint64_t)(uint32_t)view->width *
                        (uint64_t)(uint32_t)view->bpp;
    uint64_t pitch = ((row_bits + 31) / 32) * 4;
    if (!pitch || pitch > 0x7FFFFFFFULL) return FALSE;
    view->pitch = (int)pitch;

    if (view->bpp <= 8) {
        DWORD maximum = 1u << view->bpp;
        if (!view->color_count || view->color_count > maximum)
            view->color_count = maximum;
    }
    if (view->compression == GDI_BI_RGB && view->bpp == 16) {
        view->red_mask = 0x7C00;
        view->green_mask = 0x03E0;
        view->blue_mask = 0x001F;
    } else if (view->compression == GDI_BI_RGB && view->bpp == 32) {
        view->red_mask = 0xFF0000;
        view->green_mask = 0x00FF00;
        view->blue_mask = 0x0000FF;
    }
    return TRUE;
}

static BYTE gdi_expand_mask(DWORD pixel, DWORD mask)
{
    if (!mask) return 0;
    unsigned shift = (unsigned)__builtin_ctz(mask);
    DWORD maximum = mask >> shift;
    DWORD component = (pixel & mask) >> shift;
    unsigned bits = 32u - (unsigned)__builtin_clz(maximum);
    if (bits >= 8) return (BYTE)(component >> (bits - 8));
    DWORD expanded = component;
    unsigned filled = bits;
    while (filled < 8) {
        expanded = (expanded << bits) | component;
        filled += bits;
    }
    return (BYTE)(expanded >> (filled - 8));
}

static DWORD gdi_compress_mask(BYTE value, DWORD mask)
{
    if (!mask) return 0;
    unsigned shift = (unsigned)__builtin_ctz(mask);
    unsigned bits = 32u - (unsigned)__builtin_clz(mask >> shift);
    DWORD component = bits <= 8 ? (DWORD)value >> (8 - bits)
                               : (DWORD)value << (bits - 8);
    return (component << shift) & mask;
}

static uint32_t gdi_palette_pixel(const GDI_DIB_VIEW *view, DWORD index)
{
    if (!view->colors || index >= view->color_count) return 0xFF000000u;
    const BYTE *entry = view->colors + index * (view->core_header ? 3 : 4);
    return 0xFF000000u | ((uint32_t)entry[2] << 16) |
           ((uint32_t)entry[1] << 8) | entry[0];
}

static void gdi_format_from_dib(const GDI_DIB_VIEW *view, GDI_PIXEL_FORMAT *format)
{
    gdi_memset(format, 0, sizeof(*format));
    format->red = view->red_mask;
    format->green = view->green_mask;
    format->blue = view->blue_mask;
    if (view->bpp <= 8) {
        format->color_count = view->color_count;
        for (UINT i = 0; i < format->color_count; i++)
            format->colors[i] = gdi_palette_pixel(view, i);
    }
}

static BOOL gdi_read_pixel(const BYTE *row, int x, int bpp,
                            const GDI_PIXEL_FORMAT *format, uint32_t *color)
{
    if (bpp <= 8) {
        UINT index;
        if (bpp == 1) index = (row[x >> 3] >> (7 - (x & 7))) & 1;
        else if (bpp == 4) index = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15;
        else if (bpp == 8) index = row[x];
        else return FALSE;
        if (format && format->color_count) {
            *color = index < format->color_count ? format->colors[index] : 0xFF000000u;
        } else {
            DWORD gray = index * 255 / ((1u << bpp) - 1);
            *color = 0xFF000000u | (gray << 16) | (gray << 8) | gray;
        }
        return TRUE;
    }
    if (bpp == 24) {
        const BYTE *p = row + (SIZE_T)x * 3;
        *color = 0xFF000000u | ((DWORD)p[2] << 16) | ((DWORD)p[1] << 8) | p[0];
        return TRUE;
    }
    if (bpp != 16 && bpp != 32) return FALSE;
    DWORD red, green, blue;
    gdi_format_masks(bpp, format, &red, &green, &blue);
    DWORD value = bpp == 16 ? gdi_read_u16(row + (SIZE_T)x * 2)
                           : gdi_read_u32(row + (SIZE_T)x * 4);
    if (red == 0xFF0000 && green == 0xFF00 && blue == 0xFF) {
        *color = 0xFF000000u | (value & 0xFFFFFFu);
        return TRUE;
    }
    if (bpp == 16 && blue == 0x1F &&
        ((red == 0x7C00 && green == 0x3E0) ||
         (red == 0xF800 && green == 0x7E0))) {
        DWORD r = (value >> (red == 0x7C00 ? 10 : 11)) & 31;
        DWORD g = (value & green) >> 5, b = value & 31;
        r = (r << 3) | (r >> 2);
        g = green == 0x3E0 ? (g << 3) | (g >> 2) : (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        *color = 0xFF000000u | (r << 16) | (g << 8) | b;
        return TRUE;
    }
    *color = 0xFF000000u | ((DWORD)gdi_expand_mask(value, red) << 16) |
             ((DWORD)gdi_expand_mask(value, green) << 8) | gdi_expand_mask(value, blue);
    return TRUE;
}

static BOOL gdi_read_dib_pixel(const GDI_DIB_VIEW *view, int x, int y,
                               uint32_t *color)
{
    if (!view || !view->bits || !color || x < 0 || y < 0 ||
        x >= view->width || y >= view->height)
        return FALSE;

    int stored_y = view->top_down ? y : view->height - 1 - y;
    const BYTE *row = view->bits + (SIZE_T)stored_y * view->pitch;
    DWORD value;
    switch (view->bpp) {
    case 1:
        value = (row[x >> 3] >> (7 - (x & 7))) & 1u;
        *color = gdi_palette_pixel(view, value);
        return TRUE;
    case 4:
        value = row[x >> 1];
        value = (x & 1) ? (value & 0x0Fu) : (value >> 4);
        *color = gdi_palette_pixel(view, value);
        return TRUE;
    case 8:
        *color = gdi_palette_pixel(view, row[x]);
        return TRUE;
    case 16:
        value = gdi_read_u16(row + (SIZE_T)x * 2);
        *color = 0xFF000000u |
                 ((uint32_t)gdi_expand_mask(value, view->red_mask) << 16) |
                 ((uint32_t)gdi_expand_mask(value, view->green_mask) << 8) |
                 gdi_expand_mask(value, view->blue_mask);
        return TRUE;
    case 24: {
        const BYTE *pixel = row + (SIZE_T)x * 3;
        *color = 0xFF000000u | ((uint32_t)pixel[2] << 16) |
                 ((uint32_t)pixel[1] << 8) | pixel[0];
        return TRUE;
    }
    case 32:
        value = gdi_read_u32(row + (SIZE_T)x * 4);
        if (view->compression == GDI_BI_RGB) {
            *color = 0xFF000000u | (value & 0x00FFFFFFu);
        } else {
            *color = 0xFF000000u |
                     ((uint32_t)gdi_expand_mask(value, view->red_mask) << 16) |
                     ((uint32_t)gdi_expand_mask(value, view->green_mask) << 8) |
                     gdi_expand_mask(value, view->blue_mask);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

BOOL gdi32_decode_icon_dib(const BYTE *data, DWORD bytes, int width, int height,
                            BOOL monochrome, GDI_ICON_IMAGE *image)
{
    if (!data || !image || bytes < 12 || width < 0 || height < 0)
        return FALSE;
    gdi_memset(image, 0, sizeof(*image));
    DWORD header = gdi_read_u32(data);
    if (header != 12 && header != 40 && header != 52 && header != 56 &&
        header != 108 && header != 124)
        return FALSE;
    if (bytes < header) return FALSE;
    DWORD extra_masks = 0;
    if (header == 40) {
        DWORD compression = gdi_read_u32(data + 16);
        if (compression == GDI_BI_BITFIELDS) extra_masks = 12;
        else if (compression == GDI_BI_ALPHABITFIELDS) extra_masks = 16;
    }
    if (bytes < header + extra_masks) return FALSE;
    GDI_DIB_VIEW view;
    if (!gdi_parse_dib(data, NULL, GDI_DIB_RGB_COLORS, &view) ||
        view.width > 8192 || view.height > 16384 || (view.height & 1))
        return FALSE;
    view.height /= 2;
    if (!view.height) return FALSE;
    SIZE_T palette_bytes = view.bpp <= 8
        ? (SIZE_T)view.color_count * (view.core_header ? 3 : 4) : 0;
    SIZE_T offset = header + extra_masks + palette_bytes;
    SIZE_T xor_bytes = (SIZE_T)view.pitch * view.height;
    SIZE_T mask_pitch = ((SIZE_T)view.width + 31) / 32 * 4;
    SIZE_T and_bytes = mask_pitch * view.height;
    if (offset > bytes || xor_bytes > bytes - offset ||
        and_bytes > bytes - offset - xor_bytes)
        return FALSE;
    view.bits = data + offset;
    const BYTE *and_bits = view.bits + xor_bytes;
    if (!width) width = view.width;
    if (!height) height = view.height;
    if (width > 8192 || height > 8192) return FALSE;
    BOOL mono = monochrome || view.bpp == 1;
    SIZE_T output_pitch = ((SIZE_T)width + 15) / 16 * 2;
    SIZE_T mask_bytes = output_pitch * height * (mono ? 2 : 1);
    BYTE *mask_pixels = kmalloc(mask_bytes);
    uint32_t *color_pixels = mono ? NULL
        : kmalloc((SIZE_T)width * height * sizeof(uint32_t));
    if (!mask_pixels || (!mono && !color_pixels)) {
        if (mask_pixels) kfree(mask_pixels);
        if (color_pixels) kfree(color_pixels);
        SetLastError(8);
        return FALSE;
    }
    gdi_memset(mask_pixels, 0, mask_bytes);
    for (int y = 0; y < height; y++) {
        int sy = (int)((uint64_t)(UINT)y * (UINT)view.height / (UINT)height);
        int stored_y = view.top_down ? sy : view.height - 1 - sy;
        for (int x = 0; x < width; x++) {
            int sx = (int)((uint64_t)(UINT)x * (UINT)view.width / (UINT)width);
            BYTE mask = (and_bits[(SIZE_T)stored_y * mask_pitch + (sx >> 3)]
                         >> (7 - (sx & 7))) & 1;
            mask_pixels[(SIZE_T)y * output_pitch + (x >> 3)] |=
                mask << (7 - (x & 7));
            uint32_t pixel = 0;
            gdi_read_dib_pixel(&view, sx, sy, &pixel);
            if (mono) {
                DWORD gray = ((pixel >> 16) & 255) * 77 +
                             ((pixel >> 8) & 255) * 150 + (pixel & 255) * 29;
                if (gray >= 128 * 256)
                    mask_pixels[(SIZE_T)(y + height) * output_pitch + (x >> 3)] |=
                        1u << (7 - (x & 7));
            } else {
                DWORD alpha = 0;
                if (view.bpp == 32 && view.compression == GDI_BI_RGB)
                    alpha = gdi_read_u32(view.bits + (SIZE_T)stored_y * view.pitch +
                                         (SIZE_T)sx * 4) & 0xFF000000u;
                color_pixels[(SIZE_T)y * width + x] = alpha | (pixel & 0xFFFFFFu);
            }
        }
    }
    image->mask = CreateBitmap(width, height * (mono ? 2 : 1), 1, 1, mask_pixels);
    if (!mono) image->color = CreateBitmap(width, height, 1, 32, color_pixels);
    kfree(mask_pixels);
    if (color_pixels) kfree(color_pixels);
    if (!image->mask || (!mono && !image->color)) {
        if (image->mask) DeleteObject(image->mask);
        if (image->color) DeleteObject(image->color);
        gdi_memset(image, 0, sizeof(*image));
        SetLastError(8);
        return FALSE;
    }
    image->width = width;
    image->height = height;
    image->source_width = view.width;
    image->source_height = view.height;
    return TRUE;
}

static HBITMAP gdi_create_dibitmap_impl(HDC hdc, PVOID pbmih, DWORD flInit,
                                         PVOID pjBits, PVOID pbmi,
                                         UINT iUsage)
{
    if (!pbmih || (flInit & ~GDI_CBM_INIT) ||
        iUsage != GDI_DIB_RGB_COLORS) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }

    const BYTE *header = (const BYTE *)pbmih;
    DWORD header_size = gdi_read_u32(header);
    if (header_size < 40 || gdi_read_u16(header + 12) != 1) {
        SetLastError(87);
        return NULL;
    }

    int width = (int)(LONG)gdi_read_u32(header + 4);
    int raw_height = (int)(LONG)gdi_read_u32(header + 8);
    int height = 0;
    int header_bpp = (int)gdi_read_u16(header + 14);
    DWORD compression = gdi_read_u32(header + 16);
    if (width <= 0 || width > 8192 ||
        !gdi_abs_i32(raw_height, &height) || height <= 0 || height > 8192 ||
        (header_bpp != 1 && header_bpp != 4 && header_bpp != 8 &&
         header_bpp != 16 && header_bpp != 24 && header_bpp != 32) ||
        (compression != GDI_BI_RGB && compression != GDI_BI_BITFIELDS &&
         compression != GDI_BI_ALPHABITFIELDS) ||
        ((compression == GDI_BI_BITFIELDS ||
          compression == GDI_BI_ALPHABITFIELDS) &&
         header_bpp != 16 && header_bpp != 32)) {
        SetLastError(87);
        return NULL;
    }

    BOOL initialize = (flInit & GDI_CBM_INIT) != 0;
    GDI_DIB_VIEW source;
    if (initialize &&
        (!pjBits || !pbmi ||
         !gdi_parse_dib(pbmi, pjBits, iUsage, &source) ||
         source.width != width || source.height != height)) {
        SetLastError(87);
        return NULL;
    }

    int output_bpp;
    if (initialize) {
        /* Keep monochrome masks compact. Indexed palettes and custom
         * bitfields are converted to the device-neutral 32-bit form so the
         * bitmap remains self-contained after BITMAPINFO goes out of scope. */
        output_bpp = source.bpp == 1 ? 1 : 32;
    } else {
        GDI_DC *dc = dc_from_handle(hdc);
        if (!dc) {
            SetLastError(6); /* ERROR_INVALID_HANDLE */
            return NULL;
        }
        output_bpp = dc->bpp;
        if (output_bpp != 1 && output_bpp != 8 && output_bpp != 16 &&
            output_bpp != 24 && output_bpp != 32)
            output_bpp = 32;
    }

    uint64_t row_bits = (uint64_t)(uint32_t)width *
                        (uint64_t)(uint32_t)output_bpp;
    uint64_t pitch64 = ((row_bits + 31U) / 32U) * 4U;
    uint64_t bytes64 = pitch64 * (uint64_t)(uint32_t)height;
    if (!pitch64 || pitch64 > 0x7FFFFFFFULL || !bytes64 ||
        bytes64 > (uint64_t)(SIZE_T)-1) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }

    int idx = alloc_bmp();
    if (idx < 0) {
        SetLastError(8);
        return NULL;
    }
    BYTE *pixels = (BYTE *)kmalloc(bytes64);
    if (!pixels) {
        gdi_bmps[idx].in_use = 0;
        SetLastError(8);
        return NULL;
    }
    gdi_memset(pixels, 0, (SIZE_T)bytes64);

    int bottomup = 0;
    if (initialize && output_bpp == 1) {
        if ((uint64_t)(uint32_t)source.pitch != pitch64) {
            kfree(pixels);
            gdi_bmps[idx].in_use = 0;
            SetLastError(87);
            return NULL;
        }
        gdi_memcpy(pixels, source.bits, (SIZE_T)bytes64);
        bottomup = source.top_down ? 0 : 1;
    } else if (initialize) {
        for (int y = 0; y < height; y++) {
            uint32_t *row = (uint32_t *)(pixels + (SIZE_T)y * pitch64);
            for (int x = 0; x < width; x++) {
                if (!gdi_read_dib_pixel(&source, x, y, &row[x])) {
                    kfree(pixels);
                    gdi_bmps[idx].in_use = 0;
                    SetLastError(87);
                    return NULL;
                }
            }
        }
    }

    GDI_BITMAP *bitmap = &gdi_bmps[idx];
    bitmap->pixels = pixels;
    bitmap->width = width;
    bitmap->height = height;
    bitmap->bpp = output_bpp;
    bitmap->pitch = (int)pitch64;
    bitmap->bottomup = bottomup;

    HBITMAP handle = (HBITMAP)(ULONG_PTR)(BMP_TAG | (uint32_t)idx);
    serial_puts("[GDI32] CreateDIBitmap ");
    serial_putdec((uint32_t)width);
    serial_puts("x");
    serial_putdec((uint32_t)height);
    serial_puts("x");
    serial_putdec((uint32_t)output_bpp);
    serial_puts(initialize ? " initialized\n" : " empty\n");
    return handle;
}

static void gdi_trace_dib_frame(const GDI_DIB_VIEW *view)
{
    if (!view || gdi_frame_probe_count >= 4 ||
        view->width < 256 || view->height < 256)
        return;

    uint64_t hash = 1469598103934665603ULL;
    uint64_t nonblack = 0;
    uint32_t first = 0, center = 0, last = 0;
    for (int y = 0; y < view->height; y++) {
        for (int x = 0; x < view->width; x++) {
            uint32_t color = 0;
            if (!gdi_read_dib_pixel(view, x, y, &color))
                continue;
            uint32_t rgb = color & 0x00FFFFFFu;
            if (rgb) nonblack++;
            hash ^= rgb;
            hash *= 1099511628211ULL;
            if (!x && !y) first = color;
            if (x == view->width / 2 && y == view->height / 2)
                center = color;
            if (x == view->width - 1 && y == view->height - 1)
                last = color;
        }
    }

    serial_puts("[GDI-FRAME] index=");
    serial_putdec(gdi_frame_probe_count++);
    serial_puts(" nonblack=");
    serial_putdec(nonblack);
    serial_puts(" hash=0x");
    serial_puthex(hash, 16);
    serial_puts(" first=0x");
    serial_puthex(first, 8);
    serial_puts(" center=0x");
    serial_puthex(center, 8);
    serial_puts(" last=0x");
    serial_puthex(last, 8);
    serial_puts("\n");
}

static BOOL gdi_read_dc_pixel(const GDI_DC *dc, int x, int y,
                              uint32_t *color)
{
    if (!dc || !dc->surface || !color || x < 0 || y < 0 ||
        x >= dc->width || y >= dc->height)
        return FALSE;

    int stored_y = dc->bottomup ? dc->height - 1 - y : y;
    const BYTE *pixel = (const BYTE *)dc->surface +
                        (SIZE_T)stored_y * dc->pitch;
    return gdi_read_pixel(pixel, x, dc->bpp, dc->format, color);
}

BOOL gdi32_copy_layered_pixels(HDC hdc, int x, int y, int width, int height,
                                uint32_t *pixels, BOOL per_pixel)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    if (!dc || !dc->surface) { SetLastError(6); return FALSE; }
    if (!pixels || width <= 0 || height <= 0 || (per_pixel && dc->bpp != 32)) {
        SetLastError(87);
        return FALSE;
    }
    BOOL native = dc->bpp == 24;
    if (dc->bpp == 32) {
        DWORD red, green, blue;
        gdi_format_masks(dc->bpp, dc->format, &red, &green, &blue);
        native = red == 0xff0000 && green == 0xff00 && blue == 0xff;
    }
    ULONG_PTR src_base = (ULONG_PTR)dc->surface, dst_base = (ULONG_PTR)pixels;
    SIZE_T src_size = (SIZE_T)(UINT)dc->pitch * (UINT)dc->height;
    SIZE_T dst_size = (SIZE_T)width * height * sizeof(*pixels);
    BOOL overlap = src_base <= dst_base ? dst_base - src_base < src_size
                                        : src_base - dst_base < dst_size;
    if (native && !overlap) {
        int64_t source_x = (int64_t)x + dc->viewport_x;
        int64_t source_y = (int64_t)y + dc->viewport_y;
        int64_t left = -source_x, right = (int64_t)dc->width - source_x;
        if (left < 0) left = 0;
        if (left > width) left = width;
        if (right < left) right = left;
        if (right > width) right = width;
        DWORD outside = per_pixel ? 0 : 0xff000000u;
        int bytes = dc->bpp / 8;
        for (int row = 0; row < height; row++) {
            DWORD *destination = pixels + (SIZE_T)row * width;
            int64_t sy = source_y + row;
            int first = (int)left, last = (int)right;
            if (sy < 0 || sy >= dc->height) first = last = 0;
            for (int col = 0; col < first; col++) destination[col] = outside;
            for (int col = last; col < width; col++) destination[col] = outside;
            if (first == last) continue;
            int stored = dc->bottomup ? dc->height - 1 - (int)sy : (int)sy;
            const BYTE *source = (const BYTE *)dc->surface +
                (SIZE_T)stored * dc->pitch + (SIZE_T)(source_x + first) * bytes;
            if (bytes == 4) {
                gdi_memcpy(destination + first, source, (SIZE_T)(last - first) * 4);
                if (!per_pixel)
                    for (int col = first; col < last; col++) destination[col] |= 0xff000000u;
            } else {
                for (int col = first; col < last; col++, source += 3)
                    destination[col] = 0xff000000u | ((DWORD)source[2] << 16) |
                                       ((DWORD)source[1] << 8) | source[0];
            }
        }
        return TRUE;
    }
    /* A bitmap selected into a memory DC belongs to its caller. Copy it now;
     * the window must not retain pointers after SelectObject/DeleteObject. */
    for (int row = 0; row < height; row++) {
        int64_t sy = (int64_t)y + row + dc->viewport_y;
        for (int col = 0; col < width; col++) {
            int64_t sx = (int64_t)x + col + dc->viewport_x;
            DWORD color = per_pixel ? 0 : 0xff000000u;
            if (sx >= 0 && sx < dc->width && sy >= 0 && sy < dc->height) {
                gdi_read_dc_pixel(dc, (int)sx, (int)sy, &color);
                if (per_pixel) {
                    int stored = dc->bottomup ? dc->height - 1 - (int)sy : (int)sy;
                    const BYTE *source = (const BYTE *)dc->surface +
                        (SIZE_T)stored * dc->pitch + (SIZE_T)sx * 4;
                    color = (color & 0xffffffu) | ((DWORD)source[3] << 24);
                }
            }
            pixels[(SIZE_T)row * width + col] = color;
        }
    }
    return TRUE;
}

static BOOL gdi_write_dc_pixel(GDI_DC *dc, int x, int y, uint32_t color)
{
    if (!dc || !dc->surface || x < 0 || y < 0 ||
        x >= dc->width || y >= dc->height)
        return FALSE;
    if (!gdi_dc_point_visible(dc, x, y)) return TRUE;
    int stored_y = dc->bottomup ? dc->height - 1 - y : y;
    BYTE *pixel = (BYTE *)dc->surface + (SIZE_T)stored_y * dc->pitch;
    if (dc->bpp == 16 || dc->bpp == 32) {
        DWORD red, green, blue;
        gdi_format_masks(dc->bpp, dc->format, &red, &green, &blue);
        DWORD value;
        if (dc->bpp == 32 && red == 0xFF0000 && green == 0xFF00 && blue == 0xFF)
            value = color; /* Preserve coverage/alpha on native 32-bit surfaces. */
        else
            value = gdi_compress_mask((BYTE)(color >> 16), red) |
                    gdi_compress_mask((BYTE)(color >> 8), green) |
                    gdi_compress_mask((BYTE)color, blue);
        if (dc->bpp == 16) ((USHORT *)pixel)[x] = (USHORT)value;
        else ((DWORD *)pixel)[x] = value;
    } else if (dc->bpp == 24) {
        pixel += (SIZE_T)x * 3;
        pixel[0] = (BYTE)color;
        pixel[1] = (BYTE)(color >> 8);
        pixel[2] = (BYTE)(color >> 16);
    } else if (dc->bpp == 1 || dc->bpp == 4 || dc->bpp == 8) {
        UINT index = 0;
        if (dc->format && dc->format->color_count) {
            UINT best = ~0u;
            for (UINT i = 0; i < dc->format->color_count; i++) {
                DWORD p = dc->format->colors[i];
                int r = (int)((p >> 16) & 255) - (int)((color >> 16) & 255);
                int g = (int)((p >> 8) & 255) - (int)((color >> 8) & 255);
                int b = (int)(p & 255) - (int)(color & 255);
                UINT distance = (UINT)(r*r + g*g + b*b);
                if (distance < best) { best = distance; index = i; }
            }
        } else {
            UINT gray = (((color >> 16) & 255) + ((color >> 8) & 255) + (color & 255)) / 3;
            index = gray >> (8 - dc->bpp);
        }
        if (dc->bpp == 8) pixel[x] = (BYTE)index;
        else {
            UINT shift = dc->bpp == 1 ? 7 - (x & 7) : (x & 1) ? 0 : 4;
            UINT at = dc->bpp == 1 ? (UINT)x >> 3 : (UINT)x >> 1;
            UINT mask = ((1u << dc->bpp) - 1) << shift;
            pixel[at] = (BYTE)((pixel[at] & ~mask) | (index << shift));
        }
    } else {
        return FALSE;
    }
    return TRUE;
}

static int gdi_get_dibits(GDI_BITMAP *bmp, UINT start, UINT count,
                           PVOID bits, PVOID info, UINT usage)
{
    if (!bmp || !info || usage != GDI_DIB_RGB_COLORS) return 0;
    GDI_BITMAP_HEADER *header = info;
    if (header->size < 40) return 0;
    BOOL query = !bits && !header->bpp;
    if (query) {
        header->bpp = (USHORT)bmp->bpp;
        header->compression = GDI_BI_RGB;
        header->height = bmp->height;
        header->width = bmp->width;
        header->planes = 1;
        header->image_size = (DWORD)((((uint64_t)(UINT)bmp->width * bmp->bpp + 31) / 32) *
                                     4 * (UINT)bmp->height);
        header->colors_used = header->colors_important = 0;
        return 1; /* Header-only query; the caller need not supply a color table. */
    }
    int height;
    if (!gdi_abs_i32(header->height, &height)) return 0;
    if (!height) height = bmp->height;
    if (height != bmp->height) return 0;
    header->height = header->height < 0 ? -height : height;
    header->width = bmp->width;
    header->planes = 1;
    if (header->compression == GDI_BI_BITFIELDS) {
        DWORD *masks = (DWORD *)((BYTE *)info + 40);
        /* A query with BI_BITFIELDS asks for the device/bitmap's masks. */
        if (!bits || !(masks[0] | masks[1] | masks[2])) {
            if (header->bpp != 16 && header->bpp != 32) return 0;
            gdi_format_masks(header->bpp,
                              header->bpp == bmp->bpp ? &bmp->format : NULL,
                              &masks[0], &masks[1], &masks[2]);
        }
    }
    if (header->bpp <= 8) {
        if (header->bpp != bmp->bpp) return 0;
        UINT colors = 1u << header->bpp;
        DWORD *table = (DWORD *)((BYTE *)info + header->size);
        for (UINT i = 0; i < colors; i++) {
            DWORD gray = i * 255 / (colors - 1);
            table[i] = i < bmp->format.color_count ? bmp->format.colors[i] & 0xFFFFFFu
                       : (gray << 16) | (gray << 8) | gray;
        }
        header->colors_used = colors;
    } else {
        header->colors_used = 0;
    }
    GDI_DIB_VIEW view;
    if (!gdi_parse_dib(info, bits, usage, &view)) return 0;
    uint64_t size = (uint64_t)(UINT)view.pitch * (UINT)view.height;
    if (size > 0xFFFFFFFFu) return 0;
    header->image_size = (DWORD)size;
    if (!bits) return query ? 1 : height;
    if (start >= (UINT)bmp->height) return 0;
    if (count > (UINT)bmp->height - start) count = (UINT)bmp->height - start;
    GDI_PIXEL_FORMAT format;
    gdi_format_from_dib(&view, &format);
    GDI_DC output = {0};
    output.surface = bits;
    output.width = view.width;
    output.height = (int)count;
    output.pitch = view.pitch;
    output.bpp = view.bpp;
    output.format = &format;
    for (UINT row = 0; row < count; row++) {
        int y = view.top_down ? (int)(start + row) : bmp->height - 1 - (int)(start + row);
        for (int x = 0; x < view.width; x++) {
            uint32_t color;
            if (!gdi_read_bitmap_pixel(bmp, x, y, &color) ||
                !gdi_write_dc_pixel(&output, x, (int)row, color)) return 0;
        }
    }
    return (int)count;
}

void gdi32_clear_window_coverage(HDC hdc)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    if (!dc || !dc->pooled_window || !dc->owner_window || dc->bpp != 32)
        return;
    /* Retained transparent controls erase coverage, not the pixels below
     * them. Preserve the application's clip and BeginPaint update region. */
    for (int y = 0; y < dc->height; y++)
        for (int x = 0; x < dc->width; x++)
            gdi_write_dc_pixel(dc, x, y, 0);
}

/* Return -1 for formats/regions that require the general pixel path. */
static int gdi_copy_unscaled_dib(GDI_DC *dc, const GDI_DIB_VIEW *view,
                                 int64_t x, int64_t y, int width, int height,
                                 int64_t source_x, int64_t source_y)
{
    if ((view->bpp != 24 && view->bpp != 32) ||
        (dc->bpp != 24 && dc->bpp != 32) ||
        (dc->clip_active && dc->clip_region.count > 1) ||
        (dc->paint_clip && dc->paint_clip->region.count > 1) ||
        (dc->visible_region && dc->visible_region->count > 1))
        return -1;
    if (view->bpp == 32 && (view->red_mask != 0xFF0000 ||
        view->green_mask != 0xFF00 || view->blue_mask != 0xFF))
        return -1;
    if (dc->bpp == 32) {
        DWORD red, green, blue;
        gdi_format_masks(dc->bpp, dc->format, &red, &green, &blue);
        if (red != 0xFF0000 || green != 0xFF00 || blue != 0xFF) return -1;
    }

    /* The old pixel path defines aliased DIB behavior. Do not replace it with
     * row copies when any part of the described storage overlaps. */
    ULONG_PTR src_base = (ULONG_PTR)view->bits, dst_base = (ULONG_PTR)dc->surface;
    uint64_t src_size = (uint64_t)(UINT)view->pitch * (UINT)view->height;
    uint64_t dst_size = (uint64_t)(UINT)dc->pitch * (UINT)dc->height;
    if (src_base <= dst_base ? dst_base - src_base < src_size
                             : src_base - dst_base < dst_size)
        return -1;

    int64_t left = x < 0 ? 0 : x, top = y < 0 ? 0 : y;
    int64_t right = x + width, bottom = y + height;
    if (right > dc->width) right = dc->width;
    if (bottom > dc->height) bottom = dc->height;
    if (left < x - source_x) left = x - source_x;
    if (top < y - source_y) top = y - source_y;
    if (right > x + view->width - source_x) right = x + view->width - source_x;
    if (bottom > y + view->height - source_y) bottom = y + view->height - source_y;
    if (left >= right || top >= bottom) return 0;

    /* Preserve the generic path's scanline count, including rows accepted by
     * the DC but excluded by its application/paint clip. */
    int rows = (int)(bottom - top);
    const GDI_RECT *clips[3] = {
        dc->paint_clip ? &dc->paint_clip->rect : NULL,
        dc->clip_active && dc->clip_region.count ? &GDI_REGION_RECTS(&dc->clip_region)[0] : NULL,
        dc->visible_region && dc->visible_region->count ?
            &GDI_REGION_RECTS(dc->visible_region)[0] : NULL
    };
    if ((dc->clip_active && !dc->clip_region.count) ||
        (dc->visible_region && !dc->visible_region->count)) right = left;
    for (unsigned i = 0; i < 3; i++) {
        const GDI_RECT *clip = clips[i];
        if (!clip) continue;
        if (left < clip->left) left = clip->left;
        if (top < clip->top) top = clip->top;
        if (right > clip->right) right = clip->right;
        if (bottom > clip->bottom) bottom = clip->bottom;
    }

    int src_bytes = view->bpp / 8, dst_bytes = dc->bpp / 8;
    if (left < right && top < bottom) {
        SIZE_T columns = (SIZE_T)(right - left);
        for (int64_t dy = top; dy < bottom; dy++) {
            int sy = (int)(source_y + dy - y);
            int stored_src_y = view->top_down ? sy : view->height - 1 - sy;
            int stored_dst_y = dc->bottomup ? dc->height - 1 - (int)dy : (int)dy;
            const BYTE *source = view->bits + (SIZE_T)stored_src_y * view->pitch +
                                 (SIZE_T)(source_x + left - x) * src_bytes;
            BYTE *destination = (BYTE *)dc->surface + (SIZE_T)stored_dst_y * dc->pitch +
                                 (SIZE_T)left * dst_bytes;
            if (src_bytes == dst_bytes) {
                gdi_memcpy(destination, source, columns * src_bytes);
                if (dst_bytes == 4)
                    for (SIZE_T col = 0; col < columns; col++) destination[col * 4 + 3] = 255;
            } else {
                /* RGB24 <-> RGB32 changes stride, not channel order. */
                for (SIZE_T col = 0; col < columns; col++) {
                    destination[0] = source[0];
                    destination[1] = source[1];
                    destination[2] = source[2];
                    if (dst_bytes == 4) destination[3] = 255;
                    source += src_bytes;
                    destination += dst_bytes;
                }
            }
        }
    }
    mark_dc_dirty(dc);
    return rows;
}

static int gdi_stretch_dib(GDI_DC *dc, int64_t x, int64_t y, int width, int height,
                           int64_t source_x, int64_t source_y, int source_width,
                           int source_height, const GDI_DIB_VIEW *view,
                           DWORD rop)
{
    int dest_width, dest_height, src_width, src_height;
    if (!dc || !dc->surface || !view || !view->bits || rop != ROP_SRCCOPY ||
        !gdi_abs_i32(width, &dest_width) ||
        !gdi_abs_i32(height, &dest_height) ||
        !gdi_abs_i32(source_width, &src_width) ||
        !gdi_abs_i32(source_height, &src_height) ||
        !dest_width || !dest_height || !src_width || !src_height)
        return 0;

    if (width > 0 && height > 0 && width == source_width && height == source_height) {
        int rows = gdi_copy_unscaled_dib(dc, view, x, y, width, height, source_x, source_y);
        if (rows >= 0) return rows;
    }

    int64_t left = width > 0 ? x : x - dest_width;
    int64_t top = height > 0 ? y : y - dest_height;
    int64_t clip_left = left < 0 ? 0 : left;
    int64_t clip_top = top < 0 ? 0 : top;
    int64_t clip_right = left + dest_width;
    int64_t clip_bottom = top + dest_height;
    if (clip_right > dc->width) clip_right = dc->width;
    if (clip_bottom > dc->height) clip_bottom = dc->height;
    if (clip_left >= clip_right || clip_top >= clip_bottom) return 0;

    int rows_written = 0;
    for (int64_t dy = clip_top; dy < clip_bottom; dy++) {
        int64_t dest_v = dy - top;
        if (height < 0) dest_v = dest_height - 1 - dest_v;
        int src_v = (int)(((uint64_t)(uint32_t)dest_v *
                           (uint64_t)(uint32_t)src_height) /
                          (uint64_t)(uint32_t)dest_height);
        int64_t sy = source_height > 0 ? source_y + src_v
                                       : source_y - src_v - 1;
        if (sy < 0 || sy >= view->height) continue;
        BOOL wrote_row = FALSE;
        for (int64_t dx = clip_left; dx < clip_right; dx++) {
            int64_t dest_u = dx - left;
            if (width < 0) dest_u = dest_width - 1 - dest_u;
            int src_u = (int)(((uint64_t)(uint32_t)dest_u *
                               (uint64_t)(uint32_t)src_width) /
                              (uint64_t)(uint32_t)dest_width);
            int64_t sx = source_width > 0 ? source_x + src_u
                                          : source_x - src_u - 1;
            if (sx < 0 || sx >= view->width) continue;
            uint32_t color;
            if (gdi_read_dib_pixel(view, (int)sx, (int)sy, &color) &&
                gdi_write_dc_pixel(dc, (int)dx, (int)dy, color))
                wrote_row = TRUE;
        }
        if (wrote_row) rows_written++;
    }
    if (rows_written) mark_dc_dirty(dc);
    return rows_written;
}

static BOOL WINAPI StretchBlt_k32(HDC destination, int x, int y,
                                   int width, int height, HDC source,
                                   int source_x, int source_y,
                                   int source_width, int source_height,
                                   DWORD rop)
{
    if (width == source_width && height == source_height)
        return BitBlt(destination, x, y, width, height, source,
                      source_x, source_y, rop);
    GDI_DRAW_SCOPE(destination_draw);
    GDI_DRAW_SCOPE(source_draw);
    GDI_DC *dst = gdi_acquire_draw_dc(destination, &destination_draw);
    GDI_DC *src = source == destination ? dst : gdi_acquire_draw_dc(source, &source_draw);
    int dw, dh, sw, sh;
    if (!src || !dst || !src->surface || !dst->surface || rop != ROP_SRCCOPY ||
        !gdi_abs_i32(width, &dw) || !gdi_abs_i32(height, &dh) ||
        !gdi_abs_i32(source_width, &sw) || !gdi_abs_i32(source_height, &sh) ||
        !dw || !dh || !sw || !sh) return FALSE;

    int64_t left = (int64_t)x + dst->viewport_x - (width < 0 ? dw : 0);
    int64_t top = (int64_t)y + dst->viewport_y - (height < 0 ? dh : 0);
    int64_t right = left + dw, bottom = top + dh;
    int64_t clip_left = left < 0 ? 0 : left, clip_top = top < 0 ? 0 : top;
    if (right > dst->width) right = dst->width;
    if (bottom > dst->height) bottom = dst->height;
    if (right <= clip_left || bottom <= clip_top) return TRUE;

    /* A source snapshot keeps overlapping scaled copies independent of write order. */
    uint32_t *saved = NULL;
    if (src->surface == dst->surface) {
        uint64_t size = (uint64_t)(UINT)src->width * (UINT)src->height * 4;
        saved = kmalloc(size);
        if (!saved) return FALSE;
        for (int yy = 0; yy < src->height; yy++)
            for (int xx = 0; xx < src->width; xx++)
                gdi_read_dc_pixel(src, xx, yy, &saved[(SIZE_T)yy * src->width + xx]);
    }
    for (int64_t dy = clip_top; dy < bottom; dy++) {
        int64_t v = dy - top;
        if (height < 0) v = dh - 1 - v;
        int64_t sy = v * sh / dh;
        sy = (int64_t)source_y + src->viewport_y + (source_height > 0 ? sy : -sy - 1);
        if (sy < 0 || sy >= src->height) continue;
        for (int64_t dx = clip_left; dx < right; dx++) {
            int64_t u = dx - left;
            if (width < 0) u = dw - 1 - u;
            int64_t sx = u * sw / dw;
            sx = (int64_t)source_x + src->viewport_x + (source_width > 0 ? sx : -sx - 1);
            if (sx < 0 || sx >= src->width) continue;
            uint32_t color;
            if (saved) color = saved[(SIZE_T)sy * src->width + sx];
            else if (!gdi_read_dc_pixel(src, (int)sx, (int)sy, &color)) continue;
            gdi_write_dc_pixel(dst, (int)dx, (int)dy, color);
        }
    }
    if (saved) kfree(saved);
    mark_dc_dirty(dst);
    return TRUE;
}

static int WINAPI StretchDIBits_k32(HDC hdc, int x, int y, int width,
                                     int height, int source_x, int source_y,
                                     int source_width, int source_height,
                                     PCVOID bits, PCVOID bitmap_info,
                                     UINT usage, DWORD rop)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    GDI_DIB_VIEW view;
    if (!dc || !bits || !gdi_parse_dib(bitmap_info, bits, usage, &view)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    gdi_trace_dib_frame(&view);
    if (gdi_dib_trace_count++ < 64) {
        serial_puts("[GDI-DIB] StretchDIBits dst=");
        serial_putdec((uint64_t)(uint32_t)width);
        serial_puts("x");
        serial_putdec((uint64_t)(uint32_t)height);
        serial_puts(" src=");
        serial_putdec((uint64_t)(uint32_t)source_width);
        serial_puts("x");
        serial_putdec((uint64_t)(uint32_t)source_height);
        serial_puts(" dib=");
        serial_putdec((uint64_t)(uint32_t)view.width);
        serial_puts("x");
        serial_putdec((uint64_t)(uint32_t)view.height);
        serial_puts("x");
        serial_putdec((uint64_t)(uint32_t)view.bpp);
        serial_puts(view.top_down ? " top-down\n" : " bottom-up\n");
    }
    int rows = gdi_stretch_dib(dc, (int64_t)x + dc->viewport_x,
                               (int64_t)y + dc->viewport_y, width, height,
                               source_x, source_y, source_width,
                               source_height, &view, rop);
    SetLastError(rows ? 0 : 87);
    return rows;
}

static int WINAPI SetDIBitsToDevice_k32(HDC hdc, int x, int y, DWORD width,
                                        DWORD height, int source_x,
                                        int source_y, UINT start_scan,
                                        UINT scan_lines, PCVOID bits,
                                        PCVOID bitmap_info, UINT usage)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    GDI_DIB_VIEW view;
    if (!dc || !bits || !width || !height || !scan_lines ||
        width > 0x7FFFFFFFu || height > 0x7FFFFFFFu ||
        !gdi_parse_dib(bitmap_info, bits, usage, &view) ||
        start_scan >= (UINT)view.height) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    UINT available = (UINT)view.height - start_scan;
    UINT rows = scan_lines < available ? scan_lines : available;
    if (rows > height) rows = height;
    if (gdi_dib_trace_count++ < 64) {
        serial_puts("[GDI-DIB] SetDIBitsToDevice size=");
        serial_putdec(width);
        serial_puts("x");
        serial_putdec(height);
        serial_puts(" start=");
        serial_putdec(start_scan);
        serial_puts(" lines=");
        serial_putdec(rows);
        serial_puts(" bpp=");
        serial_putdec((uint64_t)(uint32_t)view.bpp);
        serial_puts("\n");
    }

    int copied = gdi_stretch_dib(dc, (int64_t)x + dc->viewport_x,
                                 (int64_t)y + dc->viewport_y, (int)width, (int)rows,
                                 source_x, (int64_t)source_y + start_scan,
                                 (int)width, (int)rows, &view, ROP_SRCCOPY);
    SetLastError(copied ? 0 : 87);
    return copied;
}

int gdi32_dib_selftest(void)
{
    int failures = 0;
    int checks = 0;
#define DIB_CHECK(condition) do { checks++; if (!(condition)) failures++; } while (0)

    int index = alloc_dc();
    if (index < 0) return 1;
    GDI_DC *dc = &gdi_dcs[index];
    uint32_t destination[16];
    HDC handle = (HDC)(ULONG_PTR)(DC_TAG | (unsigned)index);
    dc->surface = destination;
    dc->width = 4;
    dc->height = 4;
    dc->bpp = 32;
    dc->pitch = 4 * 4;
    dc->bottomup = 0;

    BYTE info[64];
    gdi_memset(info, 0, sizeof(info));
    info[0] = 40;
    info[4] = 2;                         /* biWidth = 2 */
    info[8] = 0xFE; info[9] = 0xFF;      /* biHeight = -2 */
    info[10] = 0xFF; info[11] = 0xFF;
    info[12] = 1;                        /* biPlanes = 1 */
    info[14] = 32;                       /* biBitCount = 32 */
    uint32_t top_down[4] = {
        0x00FF0000, 0x0000FF00,
        0x000000FF, 0x00FFFFFF,
    };

    HBITMAP initialized = CreateDIBitmap(
        handle, info, GDI_CBM_INIT, top_down, info, GDI_DIB_RGB_COLORS);
    GDI_BITMAP *initialized_bitmap = bmp_from_handle(initialized);
    DIB_CHECK(initialized_bitmap != NULL);
    if (initialized_bitmap) {
        DIB_CHECK(initialized_bitmap->width == 2 &&
                  initialized_bitmap->height == 2 &&
                  initialized_bitmap->bpp == 32 &&
                  !initialized_bitmap->bottomup);
        uint32_t *pixels = (uint32_t *)initialized_bitmap->pixels;
        DIB_CHECK(pixels[0] == 0xFFFF0000u &&
                  pixels[1] == 0xFF00FF00u &&
                  pixels[2] == 0xFF0000FFu &&
                  pixels[3] == 0xFFFFFFFFu);
        DIB_CHECK(DeleteObject((HGDIOBJ)initialized));
        DIB_CHECK(!bmp_from_handle(initialized));
    }

    HBITMAP empty = CreateDIBitmap(handle, info, 0, NULL, NULL,
                                    GDI_DIB_RGB_COLORS);
    GDI_BITMAP *empty_bitmap = bmp_from_handle(empty);
    DIB_CHECK(empty_bitmap != NULL);
    if (empty_bitmap) {
        uint32_t *pixels = (uint32_t *)empty_bitmap->pixels;
        DIB_CHECK(empty_bitmap->bpp == 32 && pixels[0] == 0 &&
                  pixels[3] == 0);
        DIB_CHECK(DeleteObject((HGDIOBJ)empty));
    }
    DIB_CHECK(!CreateDIBitmap(handle, info, GDI_CBM_INIT, NULL, info,
                              GDI_DIB_RGB_COLORS));

    gdi_memset(destination, 0, sizeof(destination));
    int rows = StretchDIBits_k32(handle, 1, 1, 2, 2, 0, 0, 2, 2,
                                  top_down, info, GDI_DIB_RGB_COLORS,
                                  ROP_SRCCOPY);
    DIB_CHECK(rows == 2);
    DIB_CHECK(destination[5] == 0xFFFF0000u);
    DIB_CHECK(destination[6] == 0xFF00FF00u);
    DIB_CHECK(destination[9] == 0xFF0000FFu);
    DIB_CHECK(destination[10] == 0xFFFFFFFFu);

    info[8] = 2; info[9] = 0;            /* bottom-up biHeight = 2 */
    info[10] = 0; info[11] = 0;
    uint32_t bottom_up[4] = {
        0x000000FF, 0x00FFFFFF,
        0x00FF0000, 0x0000FF00,
    };
    gdi_memset(destination, 0, sizeof(destination));
    rows = StretchDIBits_k32(handle, 0, 0, 2, 2, 0, 0, 2, 2,
                             bottom_up, info, GDI_DIB_RGB_COLORS,
                             ROP_SRCCOPY);
    DIB_CHECK(rows == 2);
    DIB_CHECK(destination[0] == 0xFFFF0000u);
    DIB_CHECK(destination[1] == 0xFF00FF00u);
    DIB_CHECK(destination[4] == 0xFF0000FFu);
    DIB_CHECK(destination[5] == 0xFFFFFFFFu);

    info[8] = 0xFE; info[9] = 0xFF;
    info[10] = 0xFF; info[11] = 0xFF;
    gdi_memset(destination, 0, sizeof(destination));
    rows = StretchDIBits_k32(handle, 0, 0, 4, 4, 0, 0, 2, 2,
                             top_down, info, GDI_DIB_RGB_COLORS,
                             ROP_SRCCOPY);
    DIB_CHECK(rows == 4);
    DIB_CHECK(destination[0] == 0xFFFF0000u &&
              destination[5] == 0xFFFF0000u);
    DIB_CHECK(destination[3] == 0xFF00FF00u);
    DIB_CHECK(destination[12] == 0xFF0000FFu);
    DIB_CHECK(destination[15] == 0xFFFFFFFFu);

    info[8] = 0xFF; info[9] = 0xFF;      /* top-down biHeight = -1 */
    info[14] = 24;
    BYTE rgb24[8] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0, 0 };
    gdi_memset(destination, 0, sizeof(destination));
    rows = StretchDIBits_k32(handle, 0, 0, 2, 1, 0, 0, 2, 1,
                             rgb24, info, GDI_DIB_RGB_COLORS,
                             ROP_SRCCOPY);
    DIB_CHECK(rows == 1);
    DIB_CHECK(destination[0] == 0xFFFF0000u);
    DIB_CHECK(destination[1] == 0xFF00FF00u);

    info[8] = 0xFE; info[14] = 32;
    gdi_memset(destination, 0, sizeof(destination));
    rows = SetDIBitsToDevice_k32(handle, 1, 0, 2, 2, 0, 0, 0, 2,
                                 top_down, info, GDI_DIB_RGB_COLORS);
    DIB_CHECK(rows == 2);
    DIB_CHECK(destination[1] == 0xFFFF0000u);
    DIB_CHECK(destination[2] == 0xFF00FF00u);
    DIB_CHECK(destination[5] == 0xFF0000FFu);
    DIB_CHECK(destination[6] == 0xFFFFFFFFu);

    dc->in_use = 0;
    dc->surface = NULL;
    serial_puts("[GDI-DIB-TEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
#undef DIB_CHECK
    return failures;
}

typedef struct {
    LONG x;
    LONG y;
    USHORT red;
    USHORT green;
    USHORT blue;
    USHORT alpha;
} GDI_TRIVERTEX;

typedef struct {
    ULONG upper_left;
    ULONG lower_right;
} GDI_GRADIENT_RECT;

#define GRADIENT_FILL_RECT_H 0u
#define GRADIENT_FILL_RECT_V 1u

static uint32_t *gdi_dc_row32(GDI_DC *dc, int y)
{
    if (!dc || !dc->surface || dc->bpp != 32 ||
        y < 0 || y >= dc->height)
        return NULL;
    if (dc->bottomup)
        y = dc->height - 1 - y;
    return (uint32_t *)((BYTE *)dc->surface + (SIZE_T)y * dc->pitch);
}

static USHORT gradient_channel(USHORT first, USHORT second,
                               int position, int span)
{
    if (span <= 0) return first;
    return (USHORT)(((uint64_t)first * (uint64_t)(span - position) +
                     (uint64_t)second * (uint64_t)position) /
                    (uint64_t)span);
}

static BOOL WINAPI GradientFill_k32(HDC hdc, const GDI_TRIVERTEX *vertices,
                                     ULONG vertex_count, PCVOID mesh,
                                     ULONG mesh_count, ULONG mode)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    const GDI_GRADIENT_RECT *rects = (const GDI_GRADIENT_RECT *)mesh;
    if (!dc || !dc->surface || dc->bpp != 32 || !vertices || !rects ||
        (mode != GRADIENT_FILL_RECT_H && mode != GRADIENT_FILL_RECT_V))
        return FALSE;

    for (ULONG item = 0; item < mesh_count; item++) {
        if (rects[item].upper_left >= vertex_count ||
            rects[item].lower_right >= vertex_count)
            return FALSE;

        const GDI_TRIVERTEX *first = &vertices[rects[item].upper_left];
        const GDI_TRIVERTEX *second = &vertices[rects[item].lower_right];
        if ((mode == GRADIENT_FILL_RECT_H && first->x > second->x) ||
            (mode == GRADIENT_FILL_RECT_V && first->y > second->y)) {
            const GDI_TRIVERTEX *swap = first;
            first = second;
            second = swap;
        }

        int x0 = first->x < second->x ? first->x : second->x;
        int y0 = first->y < second->y ? first->y : second->y;
        int x1 = first->x > second->x ? first->x : second->x;
        int y1 = first->y > second->y ? first->y : second->y;
        x0 += dc->viewport_x;
        x1 += dc->viewport_x;
        y0 += dc->viewport_y;
        y1 += dc->viewport_y;
        if (x0 == x1 || y0 == y1) continue;

        int clip_x0 = x0 < 0 ? 0 : x0;
        int clip_y0 = y0 < 0 ? 0 : y0;
        int clip_x1 = x1 > dc->width ? dc->width : x1;
        int clip_y1 = y1 > dc->height ? dc->height : y1;
        int span = mode == GRADIENT_FILL_RECT_H ? x1 - x0 : y1 - y0;

        for (int y = clip_y0; y < clip_y1; y++) {
            uint32_t *row = gdi_dc_row32(dc, y);
            if (!row) continue;
            for (int x = clip_x0; x < clip_x1; x++) {
                if (!gdi_dc_point_visible(dc, x, y)) continue;
                int position = mode == GRADIENT_FILL_RECT_H ? x - x0 : y - y0;
                USHORT red = gradient_channel(first->red, second->red,
                                              position, span);
                USHORT green = gradient_channel(first->green, second->green,
                                                position, span);
                USHORT blue = gradient_channel(first->blue, second->blue,
                                               position, span);
                USHORT alpha = gradient_channel(first->alpha, second->alpha,
                                                position, span);
                row[x] = ((uint32_t)(alpha >> 8) << 24) |
                         ((uint32_t)(red >> 8) << 16) |
                         ((uint32_t)(green >> 8) << 8) |
                         (uint32_t)(blue >> 8);
            }
        }
    }
    mark_dc_dirty(dc);
    return TRUE;
}

static BOOL WINAPI TransparentBlt_k32(HDC destination, int x, int y,
                                       int width, int height, HDC source,
                                       int source_x, int source_y,
                                       int source_width, int source_height,
                                       UINT transparent)
{
    GDI_DRAW_SCOPE(destination_draw);
    GDI_DRAW_SCOPE(source_draw);
    GDI_DC *dst = gdi_acquire_draw_dc(destination, &destination_draw);
    GDI_DC *src = source == destination ? dst : gdi_acquire_draw_dc(source, &source_draw);
    if (!dst || !src || dst->bpp != 32 || src->bpp != 32 ||
        width <= 0 || height <= 0 || source_width <= 0 || source_height <= 0)
        return FALSE;

    x += dst->viewport_x;
    y += dst->viewport_y;
    source_x += src->viewport_x;
    source_y += src->viewport_y;
    for (int dy = 0; dy < height; dy++) {
        int dst_y = y + dy;
        int src_y = source_y + (int)((int64_t)dy * source_height / height);
        uint32_t *dst_row = gdi_dc_row32(dst, dst_y);
        uint32_t *src_row = gdi_dc_row32(src, src_y);
        if (!dst_row || !src_row) continue;
        for (int dx = 0; dx < width; dx++) {
            int dst_x = x + dx;
            int src_x = source_x + (int)((int64_t)dx * source_width / width);
            if (dst_x < 0 || dst_x >= dst->width ||
                src_x < 0 || src_x >= src->width)
                continue;
            uint32_t pixel = src_row[src_x];
            if (gdi_dc_point_visible(dst, dst_x, dst_y) &&
                (pixel & 0x00FFFFFFu) != (transparent & 0x00FFFFFFu))
                dst_row[dst_x] = pixel;
        }
    }
    mark_dc_dirty(dst);
    return TRUE;
}

static BOOL WINAPI AlphaBlend_k32(HDC destination, int x, int y,
                                   int width, int height, HDC source,
                                   int source_x, int source_y,
                                   int source_width, int source_height,
                                   DWORD blend_function)
{
    GDI_DRAW_SCOPE(destination_draw);
    GDI_DRAW_SCOPE(source_draw);
    GDI_DC *dst = gdi_acquire_draw_dc(destination, &destination_draw);
    GDI_DC *src = source == destination ? dst : gdi_acquire_draw_dc(source, &source_draw);
    if (!dst || !src || dst->bpp != 32 || src->bpp != 32 ||
        width <= 0 || height <= 0 || source_width <= 0 || source_height <= 0)
        return FALSE;

    UINT constant_alpha = (blend_function >> 16) & 0xFFu;
    BOOL use_source_alpha = ((blend_function >> 24) & 0x01u) != 0;
    x += dst->viewport_x;
    y += dst->viewport_y;
    source_x += src->viewport_x;
    source_y += src->viewport_y;
    for (int dy = 0; dy < height; dy++) {
        int dst_y = y + dy;
        int src_y = source_y + (int)((int64_t)dy * source_height / height);
        uint32_t *dst_row = gdi_dc_row32(dst, dst_y);
        uint32_t *src_row = gdi_dc_row32(src, src_y);
        if (!dst_row || !src_row) continue;
        for (int dx = 0; dx < width; dx++) {
            int dst_x = x + dx;
            int src_x = source_x + (int)((int64_t)dx * source_width / width);
            if (dst_x < 0 || dst_x >= dst->width ||
                src_x < 0 || src_x >= src->width)
                continue;

            if (!gdi_dc_point_visible(dst, dst_x, dst_y)) continue;
            uint32_t source_pixel = src_row[src_x];
            uint32_t destination_pixel = dst_row[dst_x];
            UINT alpha = constant_alpha;
            if (use_source_alpha)
                alpha = alpha * ((source_pixel >> 24) & 0xFFu) / 255u;
            UINT inverse = 255u - alpha;
            UINT blue = ((source_pixel & 0xFFu) * alpha +
                         (destination_pixel & 0xFFu) * inverse + 127u) / 255u;
            UINT green = (((source_pixel >> 8) & 0xFFu) * alpha +
                          ((destination_pixel >> 8) & 0xFFu) * inverse + 127u) /
                         255u;
            UINT red = (((source_pixel >> 16) & 0xFFu) * alpha +
                        ((destination_pixel >> 16) & 0xFFu) * inverse + 127u) /
                       255u;
            dst_row[dst_x] = 0xFF000000u | (red << 16) | (green << 8) | blue;
        }
    }
    mark_dc_dirty(dst);
    return TRUE;
}

static HANDLE WINAPI AddFontMemResourceEx_k32(PVOID data, DWORD size,
                                               PVOID reserved,
                                               DWORD *font_count)
{
    return gdi_font_resource_add_memory(data, size, reserved, font_count);
}

static BOOL WINAPI RemoveFontMemResourceEx_k32(HANDLE handle)
{
    return gdi_font_resource_remove_memory(handle);
}

static int WINAPI AddFontResourceA_k32(PCSTR path)
{
    return gdi_font_resource_add_path(path, 0, NULL);
}

static int WINAPI AddFontResourceExA_k32(PCSTR path, DWORD flags,
                                          PVOID reserved)
{
    return gdi_font_resource_add_path(path, flags, reserved);
}

static BOOL WINAPI RemoveFontResourceA_k32(PCSTR path)
{
    return gdi_font_resource_remove_path(path, 0, NULL);
}

static BOOL WINAPI RemoveFontResourceExA_k32(PCSTR path, DWORD flags,
                                              PVOID reserved)
{
    return gdi_font_resource_remove_path(path, flags, reserved);
}

static int WINAPI AddFontResourceW_k32(PCWSTR path)
{
    char utf8[260];
    if (dwrite_utf16_path_to_utf8(path, utf8, sizeof(utf8)) < 0) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    return gdi_font_resource_add_path(utf8, 0, NULL);
}

static BOOL WINAPI RemoveFontResourceW_k32(PCWSTR path)
{
    char utf8[260];
    if (dwrite_utf16_path_to_utf8(path, utf8, sizeof(utf8)) < 0) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    return gdi_font_resource_remove_path(utf8, 0, NULL);
}

static int WINAPI AddFontResourceExW_k32(PCWSTR path, DWORD flags,
                                          PVOID reserved)
{
    char utf8[260];
    if (dwrite_utf16_path_to_utf8(path, utf8, sizeof(utf8)) < 0) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    return gdi_font_resource_add_path(utf8, flags, reserved);
}

static BOOL WINAPI RemoveFontResourceExW_k32(PCWSTR path, DWORD flags,
                                              PVOID reserved)
{
    char utf8[260];
    if (dwrite_utf16_path_to_utf8(path, utf8, sizeof(utf8)) < 0) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    return gdi_font_resource_remove_path(utf8, flags, reserved);
}

static int WINAPI SetGraphicsMode_k32(HDC hdc, int mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || (mode != 1 && mode != 2)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    int previous = dc->graphics_mode;
    dc->graphics_mode = mode;
    SetLastError(0);
    return previous;
}

static int WINAPI GetGraphicsMode_k32(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }
    return dc->graphics_mode;
}

static void world_transform_identity(float transform[6])
{
    transform[0] = 1.0f;
    transform[1] = 0.0f;
    transform[2] = 0.0f;
    transform[3] = 1.0f;
    transform[4] = 0.0f;
    transform[5] = 0.0f;
}

static void world_transform_multiply(float result[6], const float left[6],
                                     const float right[6])
{
    float composed[6];
    composed[0] = left[0] * right[0] + left[1] * right[2];
    composed[1] = left[0] * right[1] + left[1] * right[3];
    composed[2] = left[2] * right[0] + left[3] * right[2];
    composed[3] = left[2] * right[1] + left[3] * right[3];
    composed[4] = left[4] * right[0] + left[5] * right[2] + right[4];
    composed[5] = left[4] * right[1] + left[5] * right[3] + right[5];
    for (int i = 0; i < 6; i++) result[i] = composed[i];
}

static BOOL WINAPI SetWorldTransform_k32(HDC hdc, const float *transform)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !transform || dc->graphics_mode != 2) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    for (int i = 0; i < 6; i++) dc->world_transform[i] = transform[i];
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI GetWorldTransform_k32(HDC hdc, float *transform)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !transform) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    for (int i = 0; i < 6; i++) transform[i] = dc->world_transform[i];
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI ModifyWorldTransform_k32(HDC hdc, const float *transform,
                                             DWORD mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || dc->graphics_mode != 2 ||
        (mode != 1 && (!transform || mode < 2 || mode > 4))) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    if (mode == 1) { /* MWT_IDENTITY */
        world_transform_identity(dc->world_transform);
    } else if (mode == 2) { /* MWT_LEFTMULTIPLY */
        world_transform_multiply(dc->world_transform, transform,
                                 dc->world_transform);
    } else if (mode == 3) { /* MWT_RIGHTMULTIPLY */
        world_transform_multiply(dc->world_transform, dc->world_transform,
                                 transform);
    } else { /* MWT_SET */
        for (int i = 0; i < 6; i++) dc->world_transform[i] = transform[i];
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI GetTextMetricsA_stub(HDC hdc, PVOID metrics)
{
    (void)hdc;
    if (!metrics) return FALSE;

    gdi_memset(metrics, 0, 56); /* sizeof(TEXTMETRICA) */
    LONG *m = (LONG *)metrics;
    m[0] = 16;  /* height */
    m[1] = 12;  /* ascent */
    m[2] = 4;   /* descent */
    m[5] = 8;   /* average width */
    m[6] = 8;   /* maximum width */
    m[7] = 400; /* normal weight */
    return TRUE;
}

BOOL WINAPI GetTextMetricsW(HDC hdc, PVOID metrics)
{
    LONG *m;
    WCHAR *chars;

    if (!dc_from_handle(hdc) || !metrics) return FALSE;

    gdi_memset(metrics, 0, 60); /* sizeof(TEXTMETRICW) */
    m = (LONG *)metrics;
    m[0] = 16;  /* height */
    m[1] = 12;  /* ascent */
    m[2] = 4;   /* descent */
    m[5] = 8;   /* average width */
    m[6] = 8;   /* maximum width */
    m[7] = 400; /* normal weight */
    chars = (WCHAR *)((BYTE *)metrics + 44);
    chars[0] = L' ';
    chars[1] = 0x00FF;
    chars[2] = L'?';
    chars[3] = L' ';
    return TRUE;
}

typedef int (WINAPI *FONTENUMPROCA)(PVOID, PVOID, DWORD, LONG_PTR);
typedef int (WINAPI *FONTENUMPROCW)(PVOID, PVOID, DWORD, LONG_PTR);

typedef struct {
    GDI_LOGFONTW logfont;
    WCHAR full_name[64];
    WCHAR style[32];
    WCHAR script[32];
} GDI_ENUMLOGFONTEXW;

typedef struct {
    LONG height;
    LONG ascent;
    LONG descent;
    LONG internal_leading;
    LONG external_leading;
    LONG average_width;
    LONG maximum_width;
    LONG weight;
    LONG overhang;
    LONG digitized_aspect_x;
    LONG digitized_aspect_y;
    WCHAR first_char;
    WCHAR last_char;
    WCHAR default_char;
    WCHAR break_char;
    BYTE italic;
    BYTE underlined;
    BYTE struck_out;
    BYTE pitch_and_family;
    BYTE charset;
    BYTE padding[3];
    DWORD flags;
    UINT size_em;
    UINT cell_height;
    UINT average_width_em;
    DWORD unicode_subsets[4];
    DWORD codepage_subsets[2];
} GDI_NEWTEXTMETRICEXW;

_Static_assert(sizeof(GDI_ENUMLOGFONTEXW) == 348,
               "ENUMLOGFONTEXW layout changed");
_Static_assert(sizeof(GDI_NEWTEXTMETRICEXW) == 100,
               "NEWTEXTMETRICEXW layout changed");

static void gdi_copy_wide(WCHAR *destination, UINT capacity,
                          const WCHAR *source)
{
    UINT index = 0;
    if (!capacity) return;
    while (source && source[index] && index + 1 < capacity) {
        destination[index] = source[index];
        index++;
    }
    destination[index] = 0;
}

static void gdi_ascii_to_wide(WCHAR *destination, UINT capacity,
                              const char *source)
{
    UINT index = 0;
    if (!capacity) return;
    while (source && source[index] && index + 1 < capacity) {
        destination[index] = (WCHAR)(BYTE)source[index];
        index++;
    }
    destination[index] = 0;
}

static int WINAPI EnumFontFamiliesExA_stub(HDC hdc, PVOID logfont,
                                            PVOID callback, LONG_PTR param,
                                            DWORD flags)
{
    (void)hdc; (void)flags;
    if (!callback) return 0;
    if (g_compat32_mode) {
        uint32_t args[4] = {
            (uint32_t)(ULONG_PTR)logfont, (uint32_t)(ULONG_PTR)logfont,
            4, (uint32_t)param
        };
        return (int)compat32_callback_args((uint32_t)(ULONG_PTR)callback, 4, args);
    }
    return ((FONTENUMPROCA)callback)(logfont, logfont, 4, param);
}

static int WINAPI EnumFontFamiliesExW_k32(HDC hdc, PVOID requested_font,
                                           PVOID callback, LONG_PTR param,
                                           DWORD flags)
{
    if (!dc_from_handle(hdc) || !requested_font || !callback) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    (void)flags;

    BYTE *wire = (BYTE *)VirtualAlloc(NULL, 0x1000,
                                      MEM_RESERVE | MEM_COMMIT,
                                      PAGE_READWRITE);
    if (!wire || (g_compat32_mode &&
                  (ULONG_PTR)wire + 0x1000 > UINT32_MAX)) {
        if (wire) VirtualFree(wire, 0, MEM_RELEASE);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }

    GDI_ENUMLOGFONTEXW *font = (GDI_ENUMLOGFONTEXW *)wire;
    GDI_NEWTEXTMETRICEXW *metrics =
        (GDI_NEWTEXTMETRICEXW *)(wire + 512);
    const GDI_LOGFONTW *request = (const GDI_LOGFONTW *)requested_font;
    gdi_memset(font, 0, sizeof(*font));
    gdi_memset(metrics, 0, sizeof(*metrics));
    gdi_stock_font_logfont(&font->logfont);
    if (request->lfFaceName[0])
        gdi_copy_wide(font->logfont.lfFaceName, 32, request->lfFaceName);
    gdi_copy_wide(font->full_name, 64, font->logfont.lfFaceName);
    gdi_ascii_to_wide(font->style, 32, "Regular");
    gdi_ascii_to_wide(font->script, 32, "Western");

    metrics->height = 16;
    metrics->ascent = 12;
    metrics->descent = 4;
    metrics->average_width = 8;
    metrics->maximum_width = 8;
    metrics->weight = font->logfont.lfWeight;
    metrics->digitized_aspect_x = 96;
    metrics->digitized_aspect_y = 96;
    metrics->first_char = L' ';
    metrics->last_char = 0x00FF;
    metrics->default_char = L'?';
    metrics->break_char = L' ';
    metrics->italic = font->logfont.lfItalic;
    metrics->underlined = font->logfont.lfUnderline;
    metrics->struck_out = font->logfont.lfStrikeOut;
    metrics->pitch_and_family = font->logfont.lfPitchAndFamily;
    metrics->charset = font->logfont.lfCharSet;
    metrics->flags = 0x40; /* NTM_REGULAR */
    metrics->size_em = 2048;
    metrics->cell_height = 2048;
    metrics->average_width_em = 1024;
    metrics->unicode_subsets[0] = 1;  /* Basic Latin */
    metrics->codepage_subsets[0] = 1; /* Latin-1 */

    int result;
    if (g_compat32_mode) {
        uint32_t args[4] = {
            (uint32_t)(ULONG_PTR)font,
            (uint32_t)(ULONG_PTR)metrics,
            4, /* TRUETYPE_FONTTYPE */
            (uint32_t)param
        };
        result = (int)compat32_callback_args(
            (uint32_t)(ULONG_PTR)callback, 4, args);
    } else {
        result = ((FONTENUMPROCW)callback)(font, metrics, 4, param);
    }
    VirtualFree(wire, 0, MEM_RELEASE);
    SetLastError(0);
    return result;
}

static int WINAPI EnumFontFamiliesW(HDC hdc, PCWSTR family, PVOID callback,
                                      LONG_PTR param)
{
    GDI_LOGFONTW request;
    gdi_memset(&request, 0, sizeof(request));
    request.lfCharSet = 1; /* DEFAULT_CHARSET */
    if (family) gdi_copy_wide(request.lfFaceName, 32, family);
    return EnumFontFamiliesExW_k32(hdc, &request, callback, param, 0);
}

static BOOL WINAPI GetCharABCWidthsW_stub(HDC hdc, UINT first, UINT last,
                                           PVOID widths)
{
    (void)hdc;
    if (!widths || last < first || last - first > 0x10FFFF) return FALSE;
    LONG *abc = (LONG *)widths;
    UINT count = last - first + 1;
    for (UINT i = 0; i < count; i++, abc += 3) {
        abc[0] = 0;
        abc[1] = 8;
        abc[2] = 0;
    }
    return TRUE;
}

static BOOL WINAPI GetCharWidthW_k32(HDC hdc, UINT first, UINT last,
                                      int *widths)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !widths || last < first || last - first > 0x10FFFFu) {
        SetLastError(!dc ? 6 : 87);
        return FALSE;
    }

    int width = 8;
    GDI_LOGFONTW font;
    if (font_from_handle(dc->current_font, &font, FALSE) && font.lfWidth) {
        width = font.lfWidth;
        if (width < 0) width = -width;
    }
    UINT count = last - first + 1;
    for (UINT index = 0; index < count; index++) widths[index] = width;
    SetLastError(0);
    return TRUE;
}

static DWORD WINAPI GetFontData_k32(HDC hdc, DWORD table, DWORD offset,
                                     PVOID buffer, DWORD size)
{
    (void)table;
    (void)offset;
    (void)buffer;
    (void)size;
    if (!dc_from_handle(hdc)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0xFFFFFFFFu; /* GDI_ERROR */
    }

    /* GDI fonts currently expose metrics but do not retain an sfnt backing
     * stream. Report the documented failure value so callers can fall back to
     * Java/DirectWrite font files instead of treating a missing export as a
     * delay-load exception. */
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return 0xFFFFFFFFu; /* GDI_ERROR */
}

BOOL WINAPI GetTextExtentPoint32A(HDC hdc, PCSTR lpString, int c, PVOID lpSize)
{
    if (!dc_from_handle(hdc) || !lpSize || c < 0 || (c && !lpString))
        return FALSE;
    int length = c ? MultiByteToWideChar(0, 0, lpString, c, NULL, 0) : 0;
    if (c && length <= 0) return FALSE;
    if (length > 0x7FFFFFFF / 8) return FALSE;
    ((LONG *)lpSize)[0] = length * 8;
    ((LONG *)lpSize)[1] = length ? 16 : 0;
    return TRUE;
}

BOOL WINAPI GetTextExtentPoint32W(HDC hdc, PCWSTR lpString, int c, PVOID lpSize)
{
    if (!dc_from_handle(hdc) || !lpSize || c < 0 || c > 0x7FFFFFFF / 8 ||
        (c && !lpString)) return FALSE;
    ((LONG *)lpSize)[0] = c * 8;
    ((LONG *)lpSize)[1] = c ? 16 : 0;
    return TRUE;
}

/* ── Additional GDI32 imports ─────────────────────────────────── */

static int WINAPI GetTextFaceW_k32(HDC hdc, int count, PWSTR name)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }

    GDI_LOGFONTW stock_font;
    const GDI_LOGFONTW *logfont = NULL;
    if (font_from_handle(dc->current_font, &stock_font, FALSE) &&
        stock_font.lfFaceName[0]) {
        logfont = &stock_font;
    } else {
        gdi_stock_font_logfont(&stock_font);
        logfont = &stock_font;
    }

    int length = 0;
    while (length < 31 && logfont->lfFaceName[length]) length++;
    if (!name)
        return length + 1; /* Query includes the terminating NUL. */
    if (count <= 0)
        return 0;

    int copied = length;
    if (copied >= count) copied = count - 1;
    for (int i = 0; i < copied; i++)
        name[i] = logfont->lfFaceName[i];
    name[copied] = 0;
    return copied;
}

static PVOID gdi_create_font(const GDI_LOGFONTW *logfont)
{
    GDI_FONT *font = (GDI_FONT *)kmalloc(sizeof(*font));
    if (!font) return NULL;
    font->owner_pid = GetCurrentProcessId();
    font->logfont = *logfont;
    /* Allocation and the caller-memory read happen outside the IRQ-safe lock.
     * IDs survive deletion; wrapping must not alias any still-live handle. */
    uint64_t flags = brushes_lock();
    BOOL available = FALSE;
    for (ULONG attempt = 0; attempt < 0x01000000u; attempt++) {
        ULONG id = next_font_id++ & 0x00FFFFFFu;
        if (!id) continue;
        font->handle = (HGDIOBJ)(ULONG_PTR)(FONT_TAG | id);
        if (!gdi_font_find_locked(font->handle)) {
            available = TRUE;
            break;
        }
    }
    if (!available) {
        brushes_unlock(flags);
        kfree(font);
        return NULL;
    }
    UINT bucket = (ULONG_PTR)font->handle % FONT_BUCKETS;
    font->next = gdi_fonts[bucket];
    gdi_fonts[bucket] = font;
    HGDIOBJ handle = font->handle;
    brushes_unlock(flags);
    return handle;
}

static PVOID WINAPI CreateFontA_stub(int h, int w, int esc, int orient, int weight,
                                      DWORD italic, DWORD underline, DWORD strikeout,
                                      DWORD charset, DWORD outprec, DWORD clipprec,
                                      DWORD quality, DWORD pitch, const char *face)
{
    GDI_LOGFONTW lf;
    gdi_memset(&lf, 0, sizeof(lf));
    lf.lfHeight = h;
    lf.lfWidth = w;
    lf.lfEscapement = esc;
    lf.lfOrientation = orient;
    lf.lfWeight = weight;
    lf.lfItalic = (BYTE)italic;
    lf.lfUnderline = (BYTE)underline;
    lf.lfStrikeOut = (BYTE)strikeout;
    lf.lfCharSet = (BYTE)charset;
    lf.lfOutPrecision = (BYTE)outprec;
    lf.lfClipPrecision = (BYTE)clipprec;
    lf.lfQuality = (BYTE)quality;
    lf.lfPitchAndFamily = (BYTE)pitch;
    if (face) {
        for (int i = 0; i < 31 && face[i]; i++)
            lf.lfFaceName[i] = (WCHAR)(BYTE)face[i];
    }
    return gdi_create_font(&lf);
}

static PVOID WINAPI CreateFontW_stub(int h, int w, int esc, int orient, int weight,
                                      DWORD italic, DWORD underline, DWORD strikeout,
                                      DWORD charset, DWORD outprec, DWORD clipprec,
                                      DWORD quality, DWORD pitch, const WCHAR *face)
{
    GDI_LOGFONTW lf;
    gdi_memset(&lf, 0, sizeof(lf));
    lf.lfHeight = h;
    lf.lfWidth = w;
    lf.lfEscapement = esc;
    lf.lfOrientation = orient;
    lf.lfWeight = weight;
    lf.lfItalic = (BYTE)italic;
    lf.lfUnderline = (BYTE)underline;
    lf.lfStrikeOut = (BYTE)strikeout;
    lf.lfCharSet = (BYTE)charset;
    lf.lfOutPrecision = (BYTE)outprec;
    lf.lfClipPrecision = (BYTE)clipprec;
    lf.lfQuality = (BYTE)quality;
    lf.lfPitchAndFamily = (BYTE)pitch;
    if (face) {
        for (int i = 0; i < 31 && face[i]; i++)
            lf.lfFaceName[i] = face[i];
    }
    return gdi_create_font(&lf);
}

static PVOID WINAPI CreateFontIndirectW_stub(const void *logfont)
{
    if (!logfont) return NULL;
    return gdi_create_font((const GDI_LOGFONTW *)logfont);
}

static PVOID WINAPI CreateFontIndirectA_impl(const GDI_LOGFONTA *request)
{
    if (!request) { SetLastError(87); return NULL; }
    GDI_LOGFONTW font;
    gdi_memset(&font, 0, sizeof(font));
    gdi_memcpy(&font, request, 28); /* Identical scalar prefix in A/W. */
    int length = 0;
    while (length < 31 && request->lfFaceName[length]) length++;
    if (length && !MultiByteToWideChar(0, 0, request->lfFaceName, length,
                                      font.lfFaceName, 31))
        return NULL;
    return gdi_create_font(&font);
}

/* Bitmap GDI currently has neither a TrueType outline stream nor complex
 * character shaping. Typed failure contracts let D3DX detect that capability
 * without a missing-import exception or fabricated glyph/outline data. */
static DWORD WINAPI GetGlyphOutline_impl(HDC hdc, UINT character, UINT format,
                                          PVOID metrics, DWORD bytes,
                                          PVOID buffer, const void *transform)
{
    (void)character; (void)format; (void)metrics; (void)bytes;
    (void)buffer; (void)transform;
    SetLastError(dc_from_handle(hdc) ? 50 : 6);
    return 0xFFFFFFFFU;
}

static UINT WINAPI GetOutlineTextMetricsA_impl(HDC hdc, UINT bytes, PVOID metrics)
{
    (void)bytes; (void)metrics;
    SetLastError(dc_from_handle(hdc) ? 50 : 6);
    return 0;
}

static DWORD WINAPI GetCharacterPlacement_impl(HDC hdc, const void *text,
                                                int count, int extent,
                                                PVOID results, DWORD flags)
{
    (void)text; (void)count; (void)extent; (void)results; (void)flags;
    SetLastError(dc_from_handle(hdc) ? 50 : 6);
    return 0;
}

static DWORD WINAPI GetFontLanguageInfo_impl(HDC hdc)
{
    if (!dc_from_handle(hdc)) { SetLastError(6); return 0x8000U; } /* GCP_ERROR */
    return 0; /* The current bitmap font has no kerning, ligatures or shaping. */
}

typedef struct {
    UINT charset, codepage;
    DWORD unicode_ranges[4], codepage_ranges[2];
} GDI_CHARSETINFO;
_Static_assert(sizeof(GDI_CHARSETINFO) == 32, "CHARSETINFO wire layout");

static BOOL WINAPI TranslateCharsetInfo_impl(const DWORD *source,
                                              GDI_CHARSETINFO *output,
                                              DWORD flags)
{
    /* Ordered by font-signature bit so SRCFONTSIG chooses its lowest known
     * bit. This lookup does not claim font coverage or install a code page. */
    static const struct { UINT charset, codepage, bit; } mappings[] = {
        {0,1252,0}, {238,1250,1}, {204,1251,2}, {161,1253,3},
        {162,1254,4}, {177,1255,5}, {178,1256,6}, {186,1257,7},
        {163,1258,8}, {222,874,16}, {128,932,17}, {134,936,18},
        {129,949,19}, {136,950,20}, {130,1361,21}, {2,42,31}
    };
    if (!output || (flags != 1 && flags != 2 && flags != 3) ||
        (flags == 3 && !source)) {
        SetLastError(87); return FALSE;
    }
    UINT value = (UINT)(ULONG_PTR)source;
    for (unsigned i = 0; i < sizeof(mappings)/sizeof(mappings[0]); i++) {
        BOOL match = flags == 1 ? value == mappings[i].charset :
                     flags == 2 ? value == mappings[i].codepage :
                                  (source[0] & (1U << mappings[i].bit)) != 0;
        if (!match) continue;
        GDI_CHARSETINFO result = {0};
        result.charset = mappings[i].charset;
        result.codepage = mappings[i].codepage;
        result.codepage_ranges[0] = 1U << mappings[i].bit;
        gdi_memcpy(output, &result, sizeof(result));
        return TRUE;
    }
    SetLastError(87);
    return FALSE;
}

/* Real CreateDIBSection. UT99's SoftDrv windowed path recreates its render DIB
 * on every SetRes (incl. color-depth changes) and then check()s the returned
 * bits pointer (UWindowsViewport::ResizeViewport, WinDrv line 2348) — a NULL
 * here is a FATAL engine assert ("Critical Error" → exit). NT semantics: parse
 * the 32-bit BITMAPINFOHEADER, allocate the pixel buffer, return a real HBITMAP
 * and write the bits pointer through ppvBits (a 4-byte slot in the 32-bit
 * caller — write it as uint32, not a 64-bit store). */
static PVOID WINAPI CreateDIBSection_impl(HDC hdc, PVOID pbmi, UINT usage,
                                           void **ppvBits, PVOID hSection, DWORD offset)
{
    (void)hdc;
    serial_puts("[GDI32] CreateDIBSection request pbmi=0x");
    serial_puthex((uint64_t)(ULONG_PTR)pbmi, 16);
    serial_puts(" section=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hSection, 16);
    serial_puts(" slots=");
    serial_putdec((uint64_t)bitmap_slots_used());
    serial_puts("\n");
    if (ppvBits) {
        if (g_compat32_mode) *(uint32_t *)ppvBits = 0;
        else *ppvBits = NULL;
    }
    GDI_DIB_VIEW view;
    if (!ppvBits || !gdi_parse_dib(pbmi, NULL, usage, &view) ||
        view.width > 8192 || view.height > 8192) {
        SetLastError(87);
        serial_puts("[GDI32] CreateDIBSection failed: invalid format\n");
        return NULL;
    }
    int w = view.width, h = view.height, bpp = view.bpp;

    int idx = alloc_bmp();
    if (idx < 0) {
        serial_puts("[GDI32] CreateDIBSection failed: bitmap table full\n");
        return NULL;
    }

    uint32_t pitch = (uint32_t)view.pitch;
    uint64_t size  = (uint64_t)pitch * (uint64_t)h;
    void *pixels = NULL;
    void *mapping_base = NULL;

    if (hSection) {
        /* CreateDIBSection may expose an existing file mapping as its pixel
         * storage. Chromium uses this path to let its renderer write a frame
         * that the browser process later BitBlts. Keep the view base separate
         * because the DIB offset only needs DWORD alignment, while the NT
         * section mapper requires a page-aligned offset. */
        if (offset & 3u) {
            gdi_bmps[idx].in_use = 0;
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
            serial_puts("[GDI32] CreateDIBSection failed: unaligned offset\n");
            return NULL;
        }
        DWORD aligned_offset = offset & ~0xFFFu;
        SIZE_T delta = (SIZE_T)(offset - aligned_offset);
        if (size > (uint64_t)(SIZE_T)-1 - delta) {
            gdi_bmps[idx].in_use = 0;
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        mapping_base = MapViewOfFile((HANDLE)hSection, 0x0002u,
                                     0, aligned_offset,
                                     (SIZE_T)size + delta);
        if (mapping_base)
            pixels = (BYTE *)mapping_base + delta;
    } else {
        pixels = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT,
                              PAGE_READWRITE);
    }
    if (!pixels) {
        gdi_bmps[idx].in_use = 0;
        serial_puts("[GDI32] CreateDIBSection failed: allocation bytes=0x");
        serial_puthex(size, 16);
        serial_puts("\n");
        return NULL;
    }
    if (!mapping_base)
        gdi_memset(pixels, 0, size);

    gdi_bmps[idx].pixels = pixels;
    gdi_bmps[idx].width  = w;
    gdi_bmps[idx].height = h;
    gdi_bmps[idx].bpp    = bpp;
    gdi_bmps[idx].pitch  = (int)pitch;
    gdi_bmps[idx].bottomup    = !view.top_down;
    gdi_bmps[idx].virtual_alloc = mapping_base ? 0 : 1;
    gdi_bmps[idx].mapping_base  = mapping_base;
    GDI_BITMAP *bitmap = &gdi_bmps[idx];
    gdi_format_from_dib(&view, &bitmap->format);
    bitmap->dib_section = 1;
    bitmap->section = hSection;
    bitmap->section_offset = hSection ? offset : 0;
    bitmap->dib_header = (GDI_BITMAP_HEADER){
        .size = 40, .width = w, .height = h, .planes = 1, .bpp = (USHORT)bpp,
        .compression = bpp == 16 ? GDI_BI_BITFIELDS : view.compression,
        .image_size = (DWORD)size,
    };

    if (ppvBits) {
        if (g_compat32_mode)
            *(uint32_t *)ppvBits = (uint32_t)(uintptr_t)pixels;
        else
            *ppvBits = pixels;
    }

    serial_puts("[GDI32] CreateDIBSection ");
    serial_putdec((uint64_t)w); serial_puts("x"); serial_putdec((uint64_t)h);
    serial_puts("x"); serial_putdec(bpp);
    serial_puts(" compression="); serial_putdec(view.compression);
    serial_puts(" masks="); serial_puthex(view.red_mask, 8);
    serial_puts("/"); serial_puthex(view.green_mask, 8);
    serial_puts("/"); serial_puthex(view.blue_mask, 8);
    serial_puts(" bits=0x"); serial_puthex((uint64_t)(uintptr_t)pixels, 8);
    if (mapping_base) {
        serial_puts(" shared-base=0x");
        serial_puthex((uint64_t)(uintptr_t)mapping_base, 16);
    }
    serial_puts("\n");
    return (PVOID)(ULONG_PTR)(BMP_TAG | (uint32_t)idx);
}

static DWORD WINAPI GetPixel_k32(HDC hdc, int x, int y)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    GDI_POINT point;
    DWORD argb;
    if (!dc || !gdi_transform_point(dc, (GDI_POINT){x, y}, &point, FALSE) ||
        !gdi_dc_point_visible(dc, point.x, point.y) ||
        !gdi_read_dc_pixel(dc, point.x, point.y, &argb)) return 0xFFFFFFFFu;
    return ((argb & 0xFF) << 16) | (argb & 0xFF00) | ((argb >> 16) & 0xFF);
}

static DWORD WINAPI SetPixel(HDC hdc, int x, int y, DWORD color)
{
    GDI_DRAW_SCOPE(draw);
    GDI_DC *dc = gdi_acquire_draw_dc(hdc, &draw);
    GDI_POINT point;
    DWORD rgb, actual;
    if (!dc || !gdi_transform_point(dc, (GDI_POINT){x, y}, &point, FALSE) ||
        !gdi_dc_point_visible(dc, point.x, point.y) ||
        !gdi_resolve_color(dc, color, &rgb) ||
        !gdi_write_dc_pixel(dc, point.x, point.y, gdi_colorref_to_argb(rgb)) ||
        !gdi_read_dc_pixel(dc, point.x, point.y, &actual)) return 0xFFFFFFFFu;
    mark_dc_dirty(dc);
    return ((actual & 0xFF) << 16) | (actual & 0xFF00) | ((actual >> 16) & 0xFF);
}

typedef struct { PVOID *lpVtbl; } DWRITE_OBJECT_STUB;
typedef struct _DWRITE_COLLECTION_STUB DWRITE_COLLECTION_STUB;
typedef struct _DWRITE_FAMILY_STUB DWRITE_FAMILY_STUB;
typedef struct {
    PVOID *lpVtbl;
    DWRITE_FAMILY_STUB *family;
} DWRITE_FONT_STUB;
#define DWRITE_GSUB_CACHE_CAPACITY 64
typedef struct {
    USHORT glyph;
    USHORT source_glyph;
} DWRITE_GSUB_CACHE_ENTRY;
struct _DWRITE_FAMILY_STUB {
    PVOID *lpVtbl;
    DWRITE_COLLECTION_STUB *collection;
    DWRITE_FONT_STUB font;
};
struct _DWRITE_COLLECTION_STUB {
    PVOID *lpVtbl;
    volatile ULONG refs;
    UINT family_count;
    UINT font_file_count;
    BOOL heap_owned;
    PVOID *font_files;
    DWRITE_FAMILY_STUB family;
};
typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    PVOID loader;
    UINT key_size;
    volatile ULONG gsub_cache_lock;
    UINT gsub_cache_count;
    DWRITE_GSUB_CACHE_ENTRY gsub_cache[DWRITE_GSUB_CACHE_CAPACITY];
    BYTE key[];
} DWRITE_FONT_FILE_STUB;
typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    vfs_node_t node;
} DWRITE_LOCAL_FONT_STREAM;
typedef struct {
    PVOID *lpVtbl;
} DWRITE_LOCAL_FONT_LOADER;
typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    UINT face_type;
    UINT face_index;
    UINT simulations;
    UINT font_file_count;
    PVOID font_files[];
} DWRITE_FONT_FACE_STUB;
typedef struct {
    PVOID stream;
    PVOID fragment_context;
} DWRITE_FONT_TABLE_CONTEXT;
typedef struct {
    PVOID *lpVtbl;
    const WCHAR *value;
} DWRITE_STRINGS_STUB;

typedef struct {
    USHORT design_units_per_em;
    USHORT ascent;
    USHORT descent;
    int16_t line_gap;
    USHORT cap_height;
    USHORT x_height;
    int16_t underline_position;
    USHORT underline_thickness;
    int16_t strikethrough_position;
    USHORT strikethrough_thickness;
} DWRITE_FONT_METRICS_STUB;

typedef struct {
    int32_t left_side_bearing;
    UINT advance_width;
    int32_t right_side_bearing;
    int32_t top_side_bearing;
    UINT advance_height;
    int32_t bottom_side_bearing;
    int32_t vertical_origin_y;
} DWRITE_GLYPH_METRICS_STUB;

typedef struct {
    float advance_offset;
    float ascender_offset;
} DWRITE_GLYPH_OFFSET_STUB;

typedef struct {
    PVOID font_face;
    float font_em_size;
    UINT glyph_count;
    const USHORT *glyph_indices;
    const float *glyph_advances;
    const DWRITE_GLYPH_OFFSET_STUB *glyph_offsets;
    BOOL sideways;
    UINT bidi_level;
} DWRITE_GLYPH_RUN_STUB;

typedef struct {
    float m11, m12, m21, m22, dx, dy;
} DWRITE_MATRIX_STUB;

typedef struct {
    LONG left, top, right, bottom;
} DWRITE_RECT_STUB;

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    DWRITE_RECT_STUB bounds;
    UINT glyph_count;
    USHORT raster_chars[];
} DWRITE_GLYPH_ANALYSIS_STUB;

#define DWRITE_E_NOINTERFACE         ((LONG)0x80004002)
#define DWRITE_E_POINTER             ((LONG)0x80004003)
#define DWRITE_E_NOTIMPL             ((LONG)0x80004001)
#define DWRITE_E_INVALIDARG          ((LONG)0x80070057)
#define DWRITE_E_OUTOFMEMORY         ((LONG)0x8007000E)
#define DWRITE_E_INSUFFICIENT_BUFFER ((LONG)0x8007007A)
#define DWRITE_E_FILEFORMAT          ((LONG)0x88985000)
#define DWRITE_E_NOFONT              ((LONG)0x88985002)
#define DWRITE_E_FILENOTFOUND        ((LONG)0x88985003)
#define DWRITE_E_FILEACCESS          ((LONG)0x88985004)
#define DWRITE_E_ALREADYREGISTERED   ((LONG)0x88985006)

#define DWRITE_FONT_FILE_TYPE_UNKNOWN             0
#define DWRITE_FONT_FILE_TYPE_CFF                 1
#define DWRITE_FONT_FILE_TYPE_TRUETYPE            2
#define DWRITE_FONT_FILE_TYPE_OPENTYPE_COLLECTION 3
#define DWRITE_FONT_FACE_TYPE_CFF                 0
#define DWRITE_FONT_FACE_TYPE_TRUETYPE            1
#define DWRITE_FONT_FACE_TYPE_OPENTYPE_COLLECTION 2
#define DWRITE_FONT_FACE_TYPE_UNKNOWN             6
#define DWRITE_OPENTYPE_TAG_CMAP                  0x70616D63U
#define DWRITE_OPENTYPE_TAG_GSUB                  0x42555347U
#define DWRITE_OPENTYPE_TAG_MAXP                  0x7078616DU

static const GUID dwrite_iid_iunknown = {
    0x00000000, 0x0000, 0x0000,
    { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
};
static const GUID dwrite_iid_factory = {
    0xB859EE5A, 0xD838, 0x4B5B,
    { 0xA2, 0xE8, 0x1A, 0xDC, 0x7D, 0x93, 0xDB, 0x48 }
};
static const GUID dwrite_iid_factory1 = {
    0x30572F99, 0xDAC6, 0x41DB,
    { 0xA1, 0x6E, 0x04, 0x86, 0x30, 0x7E, 0x60, 0x6A }
};
static const GUID dwrite_iid_factory2 = {
    0x0439FC60, 0xCA44, 0x4994,
    { 0x8D, 0xEE, 0x3A, 0x9A, 0xF7, 0xB7, 0x32, 0xEC }
};
static const GUID dwrite_iid_factory3 = {
    0x9A1B41C3, 0xD3BB, 0x466A,
    { 0x87, 0xFC, 0xFE, 0x67, 0x55, 0x6A, 0x3B, 0x65 }
};
static const GUID dwrite_iid_collection = {
    0xA84CEE02, 0x3EEA, 0x4EEE,
    { 0xA8, 0x27, 0x87, 0xC1, 0xA0, 0x2A, 0x0F, 0xCC }
};
static const GUID dwrite_iid_family = {
    0xDA20D8EF, 0x812A, 0x4C43,
    { 0x98, 0x02, 0x62, 0xEC, 0x4A, 0xBD, 0x7A, 0xDD }
};
static const GUID dwrite_iid_font_list = {
    0x1A0D8438, 0x1D97, 0x4EC1,
    { 0xAE, 0xF9, 0xA2, 0xFB, 0x86, 0xED, 0x6A, 0xCB }
};
static const GUID dwrite_iid_font = {
    0xACD16696, 0x8C14, 0x4F5D,
    { 0x87, 0x7E, 0xFE, 0x3F, 0xC1, 0xD3, 0x27, 0x37 }
};
static const GUID dwrite_iid_face = {
    0x5F49804D, 0x7024, 0x4D43,
    { 0xBF, 0xA9, 0xD2, 0x59, 0x84, 0xF5, 0x38, 0x49 }
};
static const GUID dwrite_iid_font_file = {
    0x739D886A, 0xCEF5, 0x47DC,
    { 0x87, 0x69, 0x1A, 0x8B, 0x41, 0xBE, 0xBB, 0xB0 }
};
static const GUID dwrite_iid_font_file_stream = {
    0x6D4865FE, 0x0AB8, 0x4D91,
    { 0x8F, 0x62, 0x5D, 0xD6, 0xBE, 0x34, 0xA3, 0xE0 }
};
static const GUID dwrite_iid_font_file_loader = {
    0x727CAD4E, 0xD6AF, 0x4C9E,
    { 0x8A, 0x08, 0xD6, 0x95, 0xB1, 0x1C, 0xAA, 0x49 }
};
static const GUID dwrite_iid_local_font_file_loader = {
    0xB2D9F3EC, 0xC9FE, 0x4A11,
    { 0xA2, 0xEC, 0xD8, 0x62, 0x08, 0xF7, 0xC0, 0xA2 }
};
static const GUID dwrite_iid_strings = {
    0x08256209, 0x099A, 0x4B34,
    { 0xB8, 0x6D, 0xC2, 0x2B, 0x11, 0x0E, 0x77, 0x71 }
};
static const GUID dwrite_iid_glyph_analysis = {
    0x7D97DBF7, 0xE085, 0x42D4,
    { 0x81, 0xE3, 0x6A, 0x88, 0x3B, 0xDE, 0xD1, 0x18 }
};
static const GUID dwrite_iid_font_fallback = {
    0xEFA008F9, 0xF7A1, 0x48BF,
    { 0xB0, 0x5C, 0xF2, 0x24, 0x71, 0x3C, 0xC0, 0xFF }
};

static int dwrite_guid_equal(LPCGUID left, LPCGUID right)
{
    const BYTE *a = (const BYTE *)left;
    const BYTE *b = (const BYTE *)right;
    if (!a || !b) return 0;
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static LONG WINAPI dwrite_factory_QueryInterface(PVOID self, LPCGUID iid,
                                                  PVOID *object)
{
    if (!object) return (LONG)0x80004003; /* E_POINTER */
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_factory) &&
        !dwrite_guid_equal(iid, &dwrite_iid_factory1) &&
        !dwrite_guid_equal(iid, &dwrite_iid_factory2) &&
        !dwrite_guid_equal(iid, &dwrite_iid_factory3))
        return (LONG)0x80004002; /* E_NOINTERFACE */
    *object = self;
    return 0;
}

static ULONG WINAPI dwrite_factory_AddRef(PVOID self)
{
    (void)self;
    return 2;
}

static ULONG WINAPI dwrite_factory_Release(PVOID self)
{
    (void)self;
    return 1;
}

static void dwrite_trace(const char *line);

static LONG WINAPI dwrite_factory_notimpl(PVOID self)
{
    (void)self;
    PVOID caller = __builtin_return_address(0);
    dwrite_trace("[DWRITE] unsupported factory method\n");
    dll_debug_log_address(caller);
    return DWRITE_E_NOTIMPL;
}

/* IDWriteFactory3 extends the 24-entry base interface. Keep every inherited
 * slot callable so unsupported extension methods fail with E_NOTIMPL. */
static PVOID dwrite_factory_vtbl[40];
static PVOID dwrite_collection_vtbl[7];
static PVOID dwrite_family_vtbl[9];
static PVOID dwrite_font_vtbl[14];
static PVOID dwrite_face_vtbl[18];
static PVOID dwrite_font_file_vtbl[6];
static PVOID dwrite_local_font_stream_vtbl[7];
static PVOID dwrite_local_font_loader_vtbl[7];
static PVOID dwrite_strings_vtbl[9];
static PVOID dwrite_analysis_vtbl[6];
static PVOID dwrite_fallback_vtbl[4];

static LONG WINAPI dwrite_factory_CreateFontFace(
    PVOID self, UINT requested_face_type, UINT file_count,
    PVOID const *font_files, UINT face_index, UINT simulations,
    PVOID *font_face);
static LONG WINAPI dwrite_face_TryGetFontTable(
    PVOID self, UINT table_tag, const void **table_data, UINT *table_size,
    PVOID *context, BOOL *exists);
static void WINAPI dwrite_face_ReleaseFontTable(PVOID self, PVOID context);

static DWRITE_OBJECT_STUB dwrite_factory = { dwrite_factory_vtbl };
static DWRITE_COLLECTION_STUB dwrite_collection = {
    dwrite_collection_vtbl, 1, 0, 0, FALSE, NULL,
    { dwrite_family_vtbl, &dwrite_collection,
      { dwrite_font_vtbl, NULL } }
};
typedef struct _DWRITE_SYSTEM_COLLECTION_CACHE {
    struct _DWRITE_SYSTEM_COLLECTION_CACHE *next;
    DWORD owner_pid;
    uint64_t generation;
    DWRITE_COLLECTION_STUB *collection;
} DWRITE_SYSTEM_COLLECTION_CACHE;
static DWRITE_SYSTEM_COLLECTION_CACHE *dwrite_system_collections;
static volatile ULONG dwrite_system_collection_lock;
static LONG dwrite_acquire_system_font_collection(
    BOOL check_updates, DWRITE_COLLECTION_STUB **collection_out);
static DWRITE_LOCAL_FONT_LOADER dwrite_local_font_loader = {
    dwrite_local_font_loader_vtbl
};
static DWRITE_OBJECT_STUB dwrite_fallback = { dwrite_fallback_vtbl };

typedef ULONG (WINAPI *DWRITE_ADDREF_FN)(PVOID);
typedef ULONG (WINAPI *DWRITE_RELEASE_FN)(PVOID);

static ULONG dwrite_object_addref(PVOID object)
{
    if (!object || !*(PVOID **)object || !(*(PVOID **)object)[1])
        return 0;
    return ((DWRITE_ADDREF_FN)(*(PVOID **)object)[1])(object);
}

static ULONG dwrite_object_release(PVOID object)
{
    if (!object || !*(PVOID **)object || !(*(PVOID **)object)[2])
        return 0;
    return ((DWRITE_RELEASE_FN)(*(PVOID **)object)[2])(object);
}

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
} DWRITE_NUMBER_SUBSTITUTION;

static const GUID dwrite_iid_number_substitution = {
    0x14885CC9, 0xBAB0, 0x4F90,
    {0xB6, 0xED, 0x5C, 0x36, 0x6A, 0x2C, 0xD0, 0x3D}
};

static ULONG WINAPI dwrite_number_AddRef(PVOID self)
{
    return __atomic_add_fetch(&((DWRITE_NUMBER_SUBSTITUTION *)self)->refs,
                               1, __ATOMIC_RELAXED);
}

static ULONG WINAPI dwrite_number_Release(PVOID self)
{
    ULONG refs = __atomic_sub_fetch(
        &((DWRITE_NUMBER_SUBSTITUTION *)self)->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) kfree(self);
    return refs;
}

static LONG WINAPI dwrite_number_QueryInterface(PVOID self, LPCGUID iid,
                                                PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_number_substitution))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    dwrite_number_AddRef(self);
    return 0;
}

static PVOID dwrite_number_vtbl[] = {
    (PVOID)dwrite_number_QueryInterface,
    (PVOID)dwrite_number_AddRef,
    (PVOID)dwrite_number_Release
};

static LONG WINAPI dwrite_factory_CreateNumberSubstitution(
    PVOID self, UINT method, PCWSTR locale, BOOL ignore_override,
    PVOID *object)
{
    (void)self;
    (void)locale;
    (void)ignore_override;
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (method > 4) return DWRITE_E_INVALIDARG;
    /* NONE preserves nominal U+0030..0039, independent of locale/overrides.
     * Other modes need NLS digit data and contextual analysis, not success
     * from a placeholder that silently renders the wrong number shapes. */
    if (method != 2) return DWRITE_E_NOTIMPL;
    DWRITE_NUMBER_SUBSTITUTION *number = kmalloc(sizeof(*number));
    if (!number) return DWRITE_E_OUTOFMEMORY;
    number->lpVtbl = dwrite_number_vtbl;
    number->refs = 1;
    *object = number;
    dwrite_trace("[DWRITE] factory.CreateNumberSubstitution NONE\n");
    return 0;
}

static LONG WINAPI dwrite_local_font_loader_QueryInterface(
    PVOID self, LPCGUID iid, PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_font_file_loader) &&
        !dwrite_guid_equal(iid, &dwrite_iid_local_font_file_loader))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    dwrite_factory_AddRef(self);
    if (dwrite_guid_equal(iid, &dwrite_iid_local_font_file_loader))
        dwrite_trace("[DWRITE] local loader QueryInterface\n");
    return 0;
}

static LONG WINAPI dwrite_local_font_stream_QueryInterface(
    PVOID self, LPCGUID iid, PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_font_file_stream))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    (void)__atomic_add_fetch(&((DWRITE_LOCAL_FONT_STREAM *)self)->refs, 1,
                             __ATOMIC_RELAXED);
    return 0;
}

static ULONG WINAPI dwrite_local_font_stream_AddRef(PVOID self)
{
    DWRITE_LOCAL_FONT_STREAM *stream = (DWRITE_LOCAL_FONT_STREAM *)self;
    if (!stream) return 0;
    return __atomic_add_fetch(&stream->refs, 1, __ATOMIC_RELAXED);
}

static ULONG WINAPI dwrite_local_font_stream_Release(PVOID self)
{
    DWRITE_LOCAL_FONT_STREAM *stream = (DWRITE_LOCAL_FONT_STREAM *)self;
    if (!stream) return 0;
    ULONG refs = __atomic_sub_fetch(&stream->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) kfree(stream);
    return refs;
}

static LONG WINAPI dwrite_local_font_stream_ReadFileFragment(
    PVOID self, const void **fragment_start, uint64_t file_offset,
    uint64_t fragment_size, PVOID *fragment_context)
{
    DWRITE_LOCAL_FONT_STREAM *stream = (DWRITE_LOCAL_FONT_STREAM *)self;
    if (!stream || !fragment_start || !fragment_context)
        return DWRITE_E_POINTER;
    *fragment_start = NULL;
    *fragment_context = NULL;
    if (file_offset > stream->node.size ||
        fragment_size > stream->node.size - file_offset ||
        fragment_size > 0x7FFFFFFFU)
        return DWRITE_E_FILEACCESS;

    BYTE *fragment = (BYTE *)kmalloc(fragment_size ? fragment_size : 1);
    if (!fragment) return DWRITE_E_OUTOFMEMORY;
    if (fragment_size) {
        int read = vfs_read(&stream->node, file_offset, fragment,
                            fragment_size);
        if (read < 0 || (uint64_t)read != fragment_size) {
            kfree(fragment);
            return DWRITE_E_FILEACCESS;
        }
    }
    *fragment_start = fragment;
    *fragment_context = fragment;
    return 0;
}

static void WINAPI dwrite_local_font_stream_ReleaseFileFragment(
    PVOID self, PVOID fragment_context)
{
    (void)self;
    kfree(fragment_context);
}

static LONG WINAPI dwrite_local_font_stream_GetFileSize(PVOID self,
                                                         uint64_t *size)
{
    DWRITE_LOCAL_FONT_STREAM *stream = (DWRITE_LOCAL_FONT_STREAM *)self;
    if (!stream || !size) return DWRITE_E_POINTER;
    *size = stream->node.size;
    return 0;
}

static LONG WINAPI dwrite_local_font_stream_GetLastWriteTime(
    PVOID self, uint64_t *last_write_time)
{
    if (!self || !last_write_time) return DWRITE_E_POINTER;
    *last_write_time = 0;
    return 0;
}

static LONG WINAPI dwrite_local_font_loader_CreateStreamFromKey(
    PVOID self, const void *font_file_reference_key, UINT key_size,
    PVOID *font_file_stream)
{
    (void)self;
    if (!font_file_stream) return DWRITE_E_POINTER;
    *font_file_stream = NULL;
    if (!font_file_reference_key || key_size < 2 || key_size > 4096)
        return DWRITE_E_INVALIDARG;

    const char *key = (const char *)font_file_reference_key;
    if (key[key_size - 1] != '\0') return DWRITE_E_INVALIDARG;
    for (UINT i = 0; i + 1 < key_size; i++)
        if (!key[i]) return DWRITE_E_INVALIDARG;

    vfs_node_t node;
    if (!vfs_find(key, VFS_MODE_WIN32, &node))
        return DWRITE_E_FILENOTFOUND;

    DWRITE_LOCAL_FONT_STREAM *stream =
        (DWRITE_LOCAL_FONT_STREAM *)kmalloc(sizeof(*stream));
    if (!stream) return DWRITE_E_OUTOFMEMORY;
    stream->lpVtbl = dwrite_local_font_stream_vtbl;
    stream->refs = 1;
    stream->node = node;
    *font_file_stream = stream;
    return 0;
}

static LONG dwrite_local_font_key_to_utf16(
    const void *font_file_reference_key, UINT key_size, WCHAR *path,
    UINT capacity, UINT *path_length)
{
    if (!font_file_reference_key || !path_length || key_size < 2 ||
        key_size > 4096)
        return DWRITE_E_INVALIDARG;

    const BYTE *key = (const BYTE *)font_file_reference_key;
    if (key[key_size - 1] != 0) return DWRITE_E_INVALIDARG;

    UINT input = 0;
    UINT output = 0;
    while (input + 1 < key_size) {
        UINT codepoint;
        BYTE first = key[input++];
        if (!first) return DWRITE_E_INVALIDARG;

        if (first < 0x80) {
            codepoint = first;
        } else {
            UINT continuation_count;
            if (first >= 0xC2 && first <= 0xDF) {
                continuation_count = 1;
                codepoint = first & 0x1FU;
            } else if (first >= 0xE0 && first <= 0xEF) {
                continuation_count = 2;
                codepoint = first & 0x0FU;
            } else if (first >= 0xF0 && first <= 0xF4) {
                continuation_count = 3;
                codepoint = first & 0x07U;
            } else {
                return DWRITE_E_INVALIDARG;
            }
            if (input + continuation_count > key_size - 1)
                return DWRITE_E_INVALIDARG;
            for (UINT i = 0; i < continuation_count; i++) {
                BYTE continuation = key[input++];
                if ((continuation & 0xC0U) != 0x80U)
                    return DWRITE_E_INVALIDARG;
                codepoint = (codepoint << 6) | (continuation & 0x3FU);
            }
            if ((continuation_count == 2 && codepoint < 0x800U) ||
                (continuation_count == 3 && codepoint < 0x10000U) ||
                (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
                codepoint > 0x10FFFFU)
                return DWRITE_E_INVALIDARG;
        }

        UINT units = codepoint < 0x10000U ? 1 : 2;
        if (path && output + units >= capacity)
            return DWRITE_E_INSUFFICIENT_BUFFER;
        if (path) {
            if (units == 1) {
                path[output] = (WCHAR)codepoint;
            } else {
                codepoint -= 0x10000U;
                path[output] = (WCHAR)(0xD800U + (codepoint >> 10));
                path[output + 1] =
                    (WCHAR)(0xDC00U + (codepoint & 0x3FFU));
            }
        }
        output += units;
    }

    if (path) {
        if (output >= capacity) return DWRITE_E_INSUFFICIENT_BUFFER;
        path[output] = 0;
    }
    *path_length = output;
    return 0;
}

static LONG WINAPI dwrite_local_font_loader_GetFilePathLengthFromKey(
    PVOID self, const void *font_file_reference_key, UINT key_size,
    UINT *path_length)
{
    (void)self;
    LONG status = dwrite_local_font_key_to_utf16(
        font_file_reference_key, key_size, NULL, 0, path_length);
    if (status >= 0)
        dwrite_trace("[DWRITE] local loader.GetFilePathLengthFromKey\n");
    return status;
}

static LONG WINAPI dwrite_local_font_loader_GetFilePathFromKey(
    PVOID self, const void *font_file_reference_key, UINT key_size,
    WCHAR *path, UINT path_capacity)
{
    (void)self;
    if (!path) return DWRITE_E_POINTER;
    UINT path_length = 0;
    LONG status = dwrite_local_font_key_to_utf16(
        font_file_reference_key, key_size, path, path_capacity, &path_length);
    if (status >= 0)
        dwrite_trace("[DWRITE] local loader.GetFilePathFromKey\n");
    return status;
}

static LONG WINAPI dwrite_local_font_loader_GetLastWriteTimeFromKey(
    PVOID self, const void *font_file_reference_key, UINT key_size,
    uint64_t *last_write_time)
{
    (void)self;
    if (!last_write_time) return DWRITE_E_POINTER;
    UINT path_length = 0;
    LONG status = dwrite_local_font_key_to_utf16(
        font_file_reference_key, key_size, NULL, 0, &path_length);
    if (status < 0) return status;
    *last_write_time = 0;
    return 0;
}

static LONG dwrite_utf16_path_to_utf8(PCWSTR input, char *output, UINT capacity)
{
    if (!input || !output || !capacity) return DWRITE_E_INVALIDARG;
    UINT written = 0;
    for (UINT i = 0; input[i]; i++) {
        UINT codepoint = input[i];
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            UINT low = input[++i];
            if (low < 0xDC00 || low > 0xDFFF)
                return DWRITE_E_INVALIDARG;
            codepoint = 0x10000U + ((codepoint - 0xD800U) << 10) +
                        (low - 0xDC00U);
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            return DWRITE_E_INVALIDARG;
        }

        UINT needed = codepoint < 0x80 ? 1 : codepoint < 0x800 ? 2 :
                      codepoint < 0x10000 ? 3 : 4;
        if (written + needed >= capacity) return DWRITE_E_INVALIDARG;
        if (needed == 1) {
            output[written++] = (char)codepoint;
        } else if (needed == 2) {
            output[written++] = (char)(0xC0U | (codepoint >> 6));
            output[written++] = (char)(0x80U | (codepoint & 0x3FU));
        } else if (needed == 3) {
            output[written++] = (char)(0xE0U | (codepoint >> 12));
            output[written++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
            output[written++] = (char)(0x80U | (codepoint & 0x3FU));
        } else {
            output[written++] = (char)(0xF0U | (codepoint >> 18));
            output[written++] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
            output[written++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
            output[written++] = (char)(0x80U | (codepoint & 0x3FU));
        }
    }
    output[written] = '\0';
    return 0;
}

static LONG dwrite_create_local_font_file(const char *path, PVOID *font_file)
{
    if (!font_file) return DWRITE_E_POINTER;
    *font_file = NULL;
    if (!path || !*path) return DWRITE_E_INVALIDARG;

    UINT key_size = 1;
    while (path[key_size - 1]) {
        if (key_size >= 4096) return DWRITE_E_INVALIDARG;
        key_size++;
    }
    vfs_node_t node;
    if (!vfs_find(path, VFS_MODE_WIN32, &node))
        return DWRITE_E_FILENOTFOUND;

    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)kmalloc(
        sizeof(*file) + key_size);
    if (!file) return DWRITE_E_OUTOFMEMORY;
    file->lpVtbl = dwrite_font_file_vtbl;
    file->refs = 1;
    file->loader = &dwrite_local_font_loader;
    dwrite_factory_AddRef(file->loader);
    file->key_size = key_size;
    file->gsub_cache_lock = 0;
    file->gsub_cache_count = 0;
    gdi_memcpy(file->key, path, key_size);
    *font_file = file;
    return 0;
}

#define DWRITE_MAX_REGISTERED_LOADERS 64
static PVOID dwrite_collection_loaders[DWRITE_MAX_REGISTERED_LOADERS];
static PVOID dwrite_file_loaders[DWRITE_MAX_REGISTERED_LOADERS];
static volatile ULONG dwrite_loader_lock;

static void dwrite_loaders_lock(void)
{
    while (__atomic_exchange_n(&dwrite_loader_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void dwrite_loaders_unlock(void)
{
    __atomic_store_n(&dwrite_loader_lock, 0, __ATOMIC_RELEASE);
}

static LONG dwrite_register_loader(PVOID slots[], PVOID loader)
{
    if (!loader || !*(PVOID **)loader || !dwrite_object_addref(loader))
        return DWRITE_E_INVALIDARG;

    int empty = -1;
    dwrite_loaders_lock();
    for (int i = 0; i < DWRITE_MAX_REGISTERED_LOADERS; i++) {
        if (slots[i] == loader) {
            dwrite_loaders_unlock();
            dwrite_object_release(loader);
            return DWRITE_E_ALREADYREGISTERED;
        }
        if (!slots[i] && empty < 0)
            empty = i;
    }
    if (empty >= 0)
        slots[empty] = loader;
    dwrite_loaders_unlock();

    if (empty < 0) {
        dwrite_object_release(loader);
        return DWRITE_E_OUTOFMEMORY;
    }
    return 0;
}

static LONG dwrite_unregister_loader(PVOID slots[], PVOID loader)
{
    if (!loader)
        return DWRITE_E_INVALIDARG;

    int found = -1;
    dwrite_loaders_lock();
    for (int i = 0; i < DWRITE_MAX_REGISTERED_LOADERS; i++) {
        if (slots[i] == loader) {
            slots[i] = NULL;
            found = i;
            break;
        }
    }
    dwrite_loaders_unlock();

    if (found < 0)
        return DWRITE_E_INVALIDARG;
    dwrite_object_release(loader);
    return 0;
}

static PVOID dwrite_acquire_registered_loader(PVOID slots[], PVOID loader)
{
    PVOID result = NULL;
    dwrite_loaders_lock();
    for (int i = 0; i < DWRITE_MAX_REGISTERED_LOADERS; i++) {
        if (slots[i] == loader) {
            if (dwrite_object_addref(loader))
                result = loader;
            break;
        }
    }
    dwrite_loaders_unlock();
    return result;
}

static ULONG WINAPI dwrite_collection_AddRef(PVOID self)
{
    DWRITE_COLLECTION_STUB *collection = (DWRITE_COLLECTION_STUB *)self;
    if (!collection || collection == &dwrite_collection)
        return 2;
    return __atomic_add_fetch(&collection->refs, 1, __ATOMIC_RELAXED);
}

static ULONG WINAPI dwrite_collection_Release(PVOID self)
{
    DWRITE_COLLECTION_STUB *collection = (DWRITE_COLLECTION_STUB *)self;
    if (!collection || collection == &dwrite_collection ||
        !collection->heap_owned)
        return 1;

    ULONG refs = __atomic_sub_fetch(&collection->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) {
        for (UINT i = 0; i < collection->font_file_count; i++)
            dwrite_object_release(collection->font_files[i]);
        kfree(collection->font_files);
        kfree(collection);
    }
    return refs;
}

static ULONG WINAPI dwrite_family_AddRef(PVOID self)
{
    DWRITE_FAMILY_STUB *family = (DWRITE_FAMILY_STUB *)self;
    return family && family->collection
        ? dwrite_collection_AddRef(family->collection) : 0;
}

static ULONG WINAPI dwrite_family_Release(PVOID self)
{
    DWRITE_FAMILY_STUB *family = (DWRITE_FAMILY_STUB *)self;
    return family && family->collection
        ? dwrite_collection_Release(family->collection) : 0;
}

static ULONG WINAPI dwrite_font_AddRef(PVOID self)
{
    DWRITE_FONT_STUB *font = (DWRITE_FONT_STUB *)self;
    return font && font->family ? dwrite_family_AddRef(font->family) : 0;
}

static ULONG WINAPI dwrite_font_Release(PVOID self)
{
    DWRITE_FONT_STUB *font = (DWRITE_FONT_STUB *)self;
    return font && font->family ? dwrite_family_Release(font->family) : 0;
}

static LONG WINAPI dwrite_font_file_QueryInterface(PVOID self, LPCGUID iid,
                                                     PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_font_file))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    dwrite_object_addref(self);
    return 0;
}

static ULONG WINAPI dwrite_font_file_AddRef(PVOID self)
{
    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)self;
    if (!file) return 0;
    return __atomic_add_fetch(&file->refs, 1, __ATOMIC_RELAXED);
}

static ULONG WINAPI dwrite_font_file_Release(PVOID self)
{
    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)self;
    if (!file) return 0;
    ULONG refs = __atomic_sub_fetch(&file->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) {
        dwrite_object_release(file->loader);
        kfree(file);
    }
    return refs;
}

static LONG WINAPI dwrite_font_file_GetReferenceKey(PVOID self,
                                                     const void **key,
                                                     UINT *key_size)
{
    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)self;
    if (!file || !key || !key_size) return DWRITE_E_POINTER;
    *key = file->key;
    *key_size = file->key_size;
    return 0;
}

static LONG WINAPI dwrite_font_file_GetLoader(PVOID self, PVOID *loader)
{
    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)self;
    if (!file || !loader) return DWRITE_E_POINTER;
    *loader = file->loader;
    dwrite_object_addref(file->loader);
    return 0;
}

static UINT dwrite_read_be32(const BYTE *value)
{
    return ((UINT)value[0] << 24) | ((UINT)value[1] << 16) |
           ((UINT)value[2] << 8) | (UINT)value[3];
}

static USHORT dwrite_read_be16(const BYTE *value)
{
    return (USHORT)(((USHORT)value[0] << 8) | (USHORT)value[1]);
}

static USHORT dwrite_cmap_lookup_format4(const BYTE *table, UINT size,
                                          UINT codepoint)
{
    if (!table || size < 16 || codepoint > 0xFFFFU)
        return 0;

    UINT length = dwrite_read_be16(table + 2);
    UINT seg_count = dwrite_read_be16(table + 6) / 2U;
    if (length < 16 || length > size || !seg_count)
        return 0;

    uint64_t end_offset = 14;
    uint64_t start_offset = end_offset + (uint64_t)seg_count * 2U + 2U;
    uint64_t delta_offset = start_offset + (uint64_t)seg_count * 2U;
    uint64_t range_offset = delta_offset + (uint64_t)seg_count * 2U;
    if (range_offset + (uint64_t)seg_count * 2U > length)
        return 0;

    for (UINT i = 0; i < seg_count; i++) {
        UINT end = dwrite_read_be16(table + end_offset + i * 2U);
        if (codepoint > end)
            continue;
        UINT start = dwrite_read_be16(table + start_offset + i * 2U);
        if (codepoint < start)
            return 0;

        int16_t delta = (int16_t)dwrite_read_be16(
            table + delta_offset + i * 2U);
        UINT range = dwrite_read_be16(table + range_offset + i * 2U);
        if (!range)
            return (USHORT)((codepoint + delta) & 0xFFFFU);

        uint64_t glyph_offset = range_offset + (uint64_t)i * 2U + range +
            (uint64_t)(codepoint - start) * 2U;
        if (glyph_offset + 2U > length)
            return 0;
        USHORT glyph = dwrite_read_be16(table + glyph_offset);
        return glyph ? (USHORT)((glyph + delta) & 0xFFFFU) : 0;
    }
    return 0;
}

static USHORT dwrite_cmap_lookup_format12(const BYTE *table, UINT size,
                                           UINT codepoint)
{
    if (!table || size < 16)
        return 0;
    uint64_t length = dwrite_read_be32(table + 4);
    uint64_t group_count = dwrite_read_be32(table + 12);
    if (length < 16 || length > size || group_count > (length - 16) / 12U)
        return 0;

    uint64_t low = 0;
    uint64_t high = group_count;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2U;
        const BYTE *group = table + 16U + middle * 12U;
        UINT start = dwrite_read_be32(group);
        UINT end = dwrite_read_be32(group + 4);
        if (codepoint < start) {
            high = middle;
        } else if (codepoint > end) {
            low = middle + 1U;
        } else {
            uint64_t glyph = (uint64_t)dwrite_read_be32(group + 8) +
                codepoint - start;
            return glyph <= 0xFFFFU ? (USHORT)glyph : 0;
        }
    }
    return 0;
}

static USHORT dwrite_cmap_lookup(const BYTE *table, UINT size,
                                  UINT codepoint)
{
    if (!table || size < 4)
        return 0;
    UINT record_count = dwrite_read_be16(table + 2);
    if (record_count > (size - 4U) / 8U)
        return 0;

    for (UINT pass = 0; pass < 2; pass++) {
        UINT wanted_format = pass == 0 ? 12U : 4U;
        for (UINT i = 0; i < record_count; i++) {
            const BYTE *record = table + 4U + i * 8U;
            UINT platform = dwrite_read_be16(record);
            UINT encoding = dwrite_read_be16(record + 2);
            if (platform != 0 &&
                !(platform == 3 && (encoding == 1 || encoding == 10)))
                continue;
            UINT offset = dwrite_read_be32(record + 4);
            if (offset > size - 2U)
                continue;
            const BYTE *subtable = table + offset;
            UINT format = dwrite_read_be16(subtable);
            if (format != wanted_format)
                continue;
            USHORT glyph = format == 12
                ? dwrite_cmap_lookup_format12(subtable, size - offset,
                                              codepoint)
                : dwrite_cmap_lookup_format4(subtable, size - offset,
                                             codepoint);
            if (glyph)
                return glyph;
        }
    }
    return 0;
}

static USHORT dwrite_cmap_glyph_to_ascii(const BYTE *table, UINT size,
                                          USHORT glyph)
{
    for (UINT codepoint = 32; codepoint <= 126; codepoint++)
        if (dwrite_cmap_lookup(table, size, codepoint) == glyph)
            return (USHORT)codepoint;
    if (glyph && dwrite_cmap_lookup(table, size, 0x00A0U) == glyph)
        return (USHORT)' ';
    return 0;
}

static BOOL dwrite_coverage_index(const BYTE *coverage, UINT size,
                                  USHORT glyph, UINT *index)
{
    if (!coverage || !index || size < 4) return FALSE;
    UINT format = dwrite_read_be16(coverage);
    UINT count = dwrite_read_be16(coverage + 2);
    if (format == 1) {
        if (count > (size - 4U) / 2U) return FALSE;
        UINT low = 0;
        UINT high = count;
        while (low < high) {
            UINT middle = low + (high - low) / 2U;
            USHORT candidate = dwrite_read_be16(
                coverage + 4U + middle * 2U);
            if (glyph < candidate) high = middle;
            else if (glyph > candidate) low = middle + 1U;
            else {
                *index = middle;
                return TRUE;
            }
        }
        return FALSE;
    }
    if (format != 2 || count > (size - 4U) / 6U) return FALSE;
    for (UINT i = 0; i < count; i++) {
        const BYTE *range = coverage + 4U + i * 6U;
        UINT start = dwrite_read_be16(range);
        UINT end = dwrite_read_be16(range + 2);
        UINT first_index = dwrite_read_be16(range + 4);
        if (start > end) return FALSE;
        if (glyph < start) return FALSE;
        if (glyph <= end) {
            *index = first_index + (UINT)glyph - start;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL dwrite_coverage_glyph(const BYTE *coverage, UINT size,
                                  UINT index, USHORT *glyph)
{
    if (!coverage || !glyph || size < 4) return FALSE;
    UINT format = dwrite_read_be16(coverage);
    UINT count = dwrite_read_be16(coverage + 2);
    if (format == 1) {
        if (count > (size - 4U) / 2U || index >= count) return FALSE;
        *glyph = dwrite_read_be16(coverage + 4U + index * 2U);
        return TRUE;
    }
    if (format != 2 || count > (size - 4U) / 6U) return FALSE;
    for (UINT i = 0; i < count; i++) {
        const BYTE *range = coverage + 4U + i * 6U;
        UINT start = dwrite_read_be16(range);
        UINT end = dwrite_read_be16(range + 2);
        UINT first_index = dwrite_read_be16(range + 4);
        if (start > end) return FALSE;
        UINT range_count = end - start + 1U;
        if (index >= first_index && index - first_index < range_count) {
            *glyph = (USHORT)(start + index - first_index);
            return TRUE;
        }
    }
    return FALSE;
}

static USHORT dwrite_gsub_reverse_single_subtable(const BYTE *subtable,
                                                   UINT size,
                                                   USHORT glyph)
{
    if (!subtable || size < 6) return 0;
    UINT format = dwrite_read_be16(subtable);
    UINT coverage_offset = dwrite_read_be16(subtable + 2);
    if (coverage_offset > size - 4U) return 0;
    const BYTE *coverage = subtable + coverage_offset;
    UINT coverage_size = size - coverage_offset;

    if (format == 1) {
        int16_t delta = (int16_t)dwrite_read_be16(subtable + 4);
        USHORT source = (USHORT)((int32_t)glyph - delta);
        UINT index;
        return dwrite_coverage_index(coverage, coverage_size, source, &index)
            ? source : 0;
    }
    if (format != 2) return 0;
    UINT count = dwrite_read_be16(subtable + 4);
    if (count > (size - 6U) / 2U) return 0;
    for (UINT i = 0; i < count; i++) {
        if (dwrite_read_be16(subtable + 6U + i * 2U) != glyph)
            continue;
        USHORT source = 0;
        if (dwrite_coverage_glyph(coverage, coverage_size, i, &source))
            return source;
    }
    return 0;
}

static USHORT dwrite_gsub_reverse_single(const BYTE *table, UINT size,
                                         USHORT glyph)
{
    if (!table || size < 12 || dwrite_read_be16(table) != 1) return 0;
    UINT lookup_list_offset = dwrite_read_be16(table + 8);
    if (lookup_list_offset > size - 2U) return 0;
    const BYTE *lookup_list = table + lookup_list_offset;
    UINT lookup_list_size = size - lookup_list_offset;
    UINT lookup_count = dwrite_read_be16(lookup_list);
    if (lookup_count > (lookup_list_size - 2U) / 2U) return 0;

    for (UINT i = 0; i < lookup_count; i++) {
        UINT lookup_offset = dwrite_read_be16(
            lookup_list + 2U + i * 2U);
        if (lookup_offset > lookup_list_size - 6U) continue;
        const BYTE *lookup = lookup_list + lookup_offset;
        UINT lookup_size = lookup_list_size - lookup_offset;
        UINT lookup_type = dwrite_read_be16(lookup);
        UINT subtable_count = dwrite_read_be16(lookup + 4);
        if (subtable_count > (lookup_size - 6U) / 2U) continue;

        for (UINT j = 0; j < subtable_count; j++) {
            UINT subtable_offset = dwrite_read_be16(
                lookup + 6U + j * 2U);
            if (subtable_offset > lookup_size - 6U) continue;
            const BYTE *subtable = lookup + subtable_offset;
            UINT subtable_size = lookup_size - subtable_offset;
            USHORT source = 0;
            if (lookup_type == 1) {
                source = dwrite_gsub_reverse_single_subtable(
                    subtable, subtable_size, glyph);
            } else if (lookup_type == 7 && subtable_size >= 8 &&
                       dwrite_read_be16(subtable) == 1 &&
                       dwrite_read_be16(subtable + 2) == 1) {
                UINT extension_offset = dwrite_read_be32(subtable + 4);
                if (extension_offset <= subtable_size - 6U)
                    source = dwrite_gsub_reverse_single_subtable(
                        subtable + extension_offset,
                        subtable_size - extension_offset, glyph);
            }
            if (source) return source;
        }
    }
    return 0;
}

static LONG WINAPI dwrite_font_file_Analyze(PVOID self, BOOL *supported,
                                             UINT *file_type,
                                             UINT *face_type,
                                             UINT *face_count)
{
    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)self;
    if (!file || !supported || !file_type || !face_type || !face_count)
        return DWRITE_E_POINTER;
    *supported = FALSE;
    *file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    *face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    *face_count = 0;

    PVOID *loader_vtbl = file->loader ? *(PVOID **)file->loader : NULL;
    if (!loader_vtbl || !loader_vtbl[3])
        return DWRITE_E_INVALIDARG;
    typedef LONG (WINAPI *CREATE_STREAM_FN)(PVOID, const void *, UINT,
                                            PVOID *);
    PVOID stream = NULL;
    LONG status = ((CREATE_STREAM_FN)loader_vtbl[3])(
        file->loader, file->key, file->key_size, &stream);
    if (status < 0 || !stream)
        return status < 0 ? status : DWRITE_E_FILEFORMAT;

    PVOID *stream_vtbl = *(PVOID **)stream;
    if (!stream_vtbl || !stream_vtbl[3] || !stream_vtbl[4] ||
        !stream_vtbl[5]) {
        dwrite_object_release(stream);
        return DWRITE_E_FILEFORMAT;
    }

    typedef LONG (WINAPI *GET_SIZE_FN)(PVOID, uint64_t *);
    typedef LONG (WINAPI *READ_FRAGMENT_FN)(PVOID, const void **, uint64_t,
                                            uint64_t, PVOID *);
    typedef void (WINAPI *RELEASE_FRAGMENT_FN)(PVOID, PVOID);
    uint64_t size = 0;
    status = ((GET_SIZE_FN)stream_vtbl[5])(stream, &size);
    if (status < 0 || size < 4) {
        dwrite_object_release(stream);
        return status < 0 ? status : DWRITE_E_FILEFORMAT;
    }

    uint64_t header_size = size < 12 ? size : 12;
    const BYTE *header = NULL;
    PVOID fragment_context = NULL;
    status = ((READ_FRAGMENT_FN)stream_vtbl[3])(
        stream, (const void **)&header, 0, header_size, &fragment_context);
    if (status >= 0 && header) {
        UINT tag = dwrite_read_be32(header);
        if (tag == 0x00010000U || tag == 0x74727565U ||
            tag == 0x74797031U) {
            *supported = TRUE;
            *file_type = DWRITE_FONT_FILE_TYPE_TRUETYPE;
            *face_type = DWRITE_FONT_FACE_TYPE_TRUETYPE;
            *face_count = 1;
        } else if (tag == 0x4F54544FU) { /* OTTO */
            *supported = TRUE;
            *file_type = DWRITE_FONT_FILE_TYPE_CFF;
            *face_type = DWRITE_FONT_FACE_TYPE_CFF;
            *face_count = 1;
        } else if (tag == 0x74746366U && header_size >= 12) { /* ttcf */
            UINT count = dwrite_read_be32(header + 8);
            if (count && count <= 4096) {
                *supported = TRUE;
                *file_type = DWRITE_FONT_FILE_TYPE_OPENTYPE_COLLECTION;
                *face_type = DWRITE_FONT_FACE_TYPE_OPENTYPE_COLLECTION;
                *face_count = count;
            }
        }
        ((RELEASE_FRAGMENT_FN)stream_vtbl[4])(stream, fragment_context);
    }
    dwrite_object_release(stream);
    return status < 0 ? status : 0;
}

static BOOL gdi_font_validate_file(const char *path, DWORD *face_count,
                                   DWORD *error)
{
    if (face_count) *face_count = 0;
    if (error) *error = 11; /* ERROR_BAD_FORMAT */
    if (!path || !face_count) {
        if (error) *error = 87; /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    PVOID file = NULL;
    LONG status = dwrite_create_local_font_file(path, &file);
    if (status < 0 || !file) {
        if (error) {
            *error = status == DWRITE_E_FILENOTFOUND ? 2 :
                     status == DWRITE_E_OUTOFMEMORY ? 8 : 11;
        }
        return FALSE;
    }

    BOOL supported = FALSE;
    UINT file_type = 0;
    UINT face_type = 0;
    UINT faces = 0;
    status = dwrite_font_file_Analyze(file, &supported, &file_type,
                                      &face_type, &faces);
    dwrite_object_release(file);
    if (status < 0 || !supported || !faces) {
        if (error && status == DWRITE_E_OUTOFMEMORY) *error = 8;
        return FALSE;
    }
    *face_count = faces;
    if (error) *error = 0;
    return TRUE;
}

static BOOL gdi_font_validate_memory(const void *data, DWORD size,
                                     DWORD *face_count)
{
    if (face_count) *face_count = 0;
    if (!data || !face_count || size < 4) return FALSE;
    const BYTE *header = (const BYTE *)data;
    UINT tag = dwrite_read_be32(header);
    if (tag == 0x00010000U || tag == 0x74727565U ||
        tag == 0x74797031U || tag == 0x4F54544FU) {
        if (size < 12) return FALSE;
        *face_count = 1;
        return TRUE;
    }
    if (tag != 0x74746366U || size < 12) return FALSE;
    UINT faces = dwrite_read_be32(header + 8);
    if (!faces || faces > 4096 ||
        (uint64_t)faces * sizeof(UINT) > size - 12U)
        return FALSE;
    *face_count = faces;
    return TRUE;
}

static LONG WINAPI dwrite_fallback_QueryInterface(PVOID self, LPCGUID iid,
                                                   PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_font_fallback))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    return 0;
}

static LONG WINAPI dwrite_fallback_MapCharacters(
    PVOID self, PVOID analysis_source, UINT text_position, UINT text_length,
    PVOID base_collection, PCWSTR base_family_name, UINT base_weight,
    UINT base_style, UINT base_stretch, UINT *mapped_length,
    PVOID *mapped_font, float *scale)
{
    (void)self;
    (void)analysis_source;
    (void)text_position;
    (void)base_family_name;
    (void)base_weight;
    (void)base_style;
    (void)base_stretch;
    if (!mapped_length || !mapped_font || !scale) return DWRITE_E_POINTER;
    *mapped_length = text_length;
    DWRITE_COLLECTION_STUB *collection =
        (DWRITE_COLLECTION_STUB *)base_collection;
    BOOL release_collection = FALSE;
    if (!collection && dwrite_acquire_system_font_collection(
            FALSE, &collection) >= 0)
        release_collection = TRUE;
    *mapped_font = text_length && collection && collection->family_count
        ? &collection->family.font : NULL;
    if (*mapped_font) dwrite_font_AddRef(*mapped_font);
    if (release_collection) dwrite_collection_Release(collection);
    *scale = 1.0f;
    dwrite_trace("[DWRITE] fallback.MapCharacters\n");
    return 0;
}

static const WCHAR dwrite_family_name[] = {
    'S', 'e', 'g', 'o', 'e', ' ', 'U', 'I', 0
};
static const WCHAR dwrite_face_name[] = {
    'R', 'e', 'g', 'u', 'l', 'a', 'r', 0
};
static const WCHAR dwrite_locale_name[] = {
    'e', 'n', '-', 'U', 'S', 0
};
static DWRITE_STRINGS_STUB dwrite_family_names = {
    dwrite_strings_vtbl, dwrite_family_name
};
static DWRITE_STRINGS_STUB dwrite_face_names = {
    dwrite_strings_vtbl, dwrite_face_name
};

static UINT dwrite_trace_count;
static UINT dwrite_alpha_trace_count;
static UINT dwrite_glyph_trace_count;

static void dwrite_trace(const char *line)
{
#if defined(OK_QUIET) && OK_QUIET
    (void)line;
#else
    if (dwrite_trace_count++ < 320)
        serial_puts(line);
#endif
}

static LONG dwrite_floor_to_long(float value)
{
    LONG whole = (LONG)value;
    return value < (float)whole ? whole - 1 : whole;
}

static LONG dwrite_ceil_to_long(float value)
{
    LONG whole = (LONG)value;
    return value > (float)whole ? whole + 1 : whole;
}

static void dwrite_transform_point(const DWRITE_MATRIX_STUB *matrix,
                                   float x, float y, float *out_x,
                                   float *out_y)
{
    if (!matrix) {
        *out_x = x;
        *out_y = y;
        return;
    }
    *out_x = x * matrix->m11 + y * matrix->m21 + matrix->dx;
    *out_y = x * matrix->m12 + y * matrix->m22 + matrix->dy;
}

static void dwrite_gsub_cache_lock(DWRITE_FONT_FILE_STUB *file)
{
    while (__atomic_exchange_n(&file->gsub_cache_lock, 1,
                               __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void dwrite_gsub_cache_unlock(DWRITE_FONT_FILE_STUB *file)
{
    __atomic_store_n(&file->gsub_cache_lock, 0, __ATOMIC_RELEASE);
}

static BOOL dwrite_gsub_cache_lookup(DWRITE_FONT_FILE_STUB *file,
                                     USHORT glyph, USHORT *source)
{
    if (!file || !source) return FALSE;
    BOOL found = FALSE;
    dwrite_gsub_cache_lock(file);
    UINT count = file->gsub_cache_count;
    if (count > DWRITE_GSUB_CACHE_CAPACITY)
        count = DWRITE_GSUB_CACHE_CAPACITY;
    for (UINT i = 0; i < count; i++) {
        if (file->gsub_cache[i].glyph != glyph) continue;
        *source = file->gsub_cache[i].source_glyph;
        found = TRUE;
        break;
    }
    dwrite_gsub_cache_unlock(file);
    return found;
}

static void dwrite_gsub_cache_insert(DWRITE_FONT_FILE_STUB *file,
                                     USHORT glyph, USHORT source)
{
    if (!file) return;
    dwrite_gsub_cache_lock(file);
    UINT count = file->gsub_cache_count;
    if (count > DWRITE_GSUB_CACHE_CAPACITY)
        count = DWRITE_GSUB_CACHE_CAPACITY;
    for (UINT i = 0; i < count; i++) {
        if (file->gsub_cache[i].glyph != glyph) continue;
        file->gsub_cache[i].source_glyph = source;
        dwrite_gsub_cache_unlock(file);
        return;
    }
    UINT slot;
    if (count < DWRITE_GSUB_CACHE_CAPACITY) {
        slot = count;
        file->gsub_cache_count = count + 1U;
    } else {
        slot = (UINT)glyph % DWRITE_GSUB_CACHE_CAPACITY;
    }
    file->gsub_cache[slot].glyph = glyph;
    file->gsub_cache[slot].source_glyph = source;
    dwrite_gsub_cache_unlock(file);
}

static USHORT dwrite_reverse_substituted_glyph(DWRITE_FONT_FACE_STUB *face,
                                                USHORT glyph)
{
    if (!face || !face->font_file_count || !face->font_files[0]) return 0;
    DWRITE_FONT_FILE_STUB *file =
        (DWRITE_FONT_FILE_STUB *)face->font_files[0];
    if (file->lpVtbl != dwrite_font_file_vtbl) return 0;

    USHORT source = 0;
    if (dwrite_gsub_cache_lookup(file, glyph, &source)) return source;

    const void *gsub = NULL;
    UINT gsub_size = 0;
    PVOID context = NULL;
    BOOL exists = FALSE;
    LONG status = dwrite_face_TryGetFontTable(
        face, DWRITE_OPENTYPE_TAG_GSUB, &gsub, &gsub_size, &context,
        &exists);
    if (status >= 0 && exists && gsub)
        source = dwrite_gsub_reverse_single(
            (const BYTE *)gsub, gsub_size, glyph);
    if (context) dwrite_face_ReleaseFontTable(face, context);
    dwrite_gsub_cache_insert(file, glyph, source);
    return source;
}

static LONG WINAPI dwrite_analysis_QueryInterface(PVOID self, LPCGUID iid,
                                                    PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_glyph_analysis))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    (void)__atomic_add_fetch(&((DWRITE_GLYPH_ANALYSIS_STUB *)self)->refs, 1,
                             __ATOMIC_RELAXED);
    return 0;
}

static ULONG WINAPI dwrite_analysis_AddRef(PVOID self)
{
    if (!self) return 0;
    return __atomic_add_fetch(&((DWRITE_GLYPH_ANALYSIS_STUB *)self)->refs, 1,
                              __ATOMIC_RELAXED);
}

static ULONG WINAPI dwrite_analysis_Release(PVOID self)
{
    DWRITE_GLYPH_ANALYSIS_STUB *analysis =
        (DWRITE_GLYPH_ANALYSIS_STUB *)self;
    if (!analysis) return 0;
    ULONG refs = __atomic_sub_fetch(&analysis->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) kfree(analysis);
    return refs;
}

static LONG WINAPI dwrite_analysis_GetAlphaTextureBounds(
    PVOID self, UINT texture_type, DWRITE_RECT_STUB *bounds)
{
    DWRITE_GLYPH_ANALYSIS_STUB *analysis =
        (DWRITE_GLYPH_ANALYSIS_STUB *)self;
    if (!analysis || !bounds) return DWRITE_E_POINTER;
    if (texture_type > 1) return DWRITE_E_INVALIDARG;
    *bounds = analysis->bounds;
    if (dwrite_alpha_trace_count++ < 64) {
        serial_puts("[DWRITE-ALPHA] bounds type=");
        serial_putdec(texture_type);
        serial_puts(" rect=");
        serial_putdec((uint64_t)(int64_t)bounds->left);
        serial_puts(",");
        serial_putdec((uint64_t)(int64_t)bounds->top);
        serial_puts("-");
        serial_putdec((uint64_t)(int64_t)bounds->right);
        serial_puts(",");
        serial_putdec((uint64_t)(int64_t)bounds->bottom);
        serial_puts(" glyphs=");
        serial_putdec(analysis->glyph_count);
        serial_puts("\n");
    }
    dwrite_trace("[DWRITE] analysis.GetAlphaTextureBounds\n");
    return 0;
}

static LONG WINAPI dwrite_analysis_CreateAlphaTexture(
    PVOID self, UINT texture_type, const DWRITE_RECT_STUB *bounds,
    BYTE *alpha_values, UINT buffer_size)
{
    DWRITE_GLYPH_ANALYSIS_STUB *analysis =
        (DWRITE_GLYPH_ANALYSIS_STUB *)self;
    if (!analysis || !bounds || !alpha_values) return DWRITE_E_POINTER;
    if (texture_type > 1 || bounds->right < bounds->left ||
        bounds->bottom < bounds->top)
        return DWRITE_E_INVALIDARG;

    uint64_t width = (uint64_t)(bounds->right - bounds->left);
    uint64_t height = (uint64_t)(bounds->bottom - bounds->top);
    uint64_t channels = texture_type == 1 ? 3 : 1;
    uint64_t required = width * height * channels;
    if (width > 4096 || height > 4096 || required > buffer_size)
        return DWRITE_E_INSUFFICIENT_BUFFER;

    if (dwrite_alpha_trace_count++ < 64) {
        serial_puts("[DWRITE-ALPHA] create type=");
        serial_putdec(texture_type);
        serial_puts(" size=");
        serial_putdec(width);
        serial_puts("x");
        serial_putdec(height);
        serial_puts(" bytes=");
        serial_putdec(required);
        serial_puts("\n");
    }

    gdi_memset(alpha_values, 0, (SIZE_T)required);
    if (!width || !height || !analysis->glyph_count) return 0;

    LONG run_width = analysis->bounds.right - analysis->bounds.left;
    LONG run_height = analysis->bounds.bottom - analysis->bounds.top;
    if (run_width < 1 || run_height < 1) return 0;
    for (uint64_t y = 0; y < height; y++) {
        LONG global_y = bounds->top + (LONG)y;
        if (global_y < analysis->bounds.top ||
            global_y >= analysis->bounds.bottom)
            continue;
        UINT glyph_y = (UINT)(((uint64_t)(global_y - analysis->bounds.top) *
                               16) / (uint64_t)run_height);
        if (glyph_y > 15) glyph_y = 15;
        for (uint64_t x = 0; x < width; x++) {
            LONG global_x = bounds->left + (LONG)x;
            if (global_x < analysis->bounds.left ||
                global_x >= analysis->bounds.right)
                continue;

            uint64_t relative_x =
                (uint64_t)(global_x - analysis->bounds.left);
            UINT glyph_slot = (UINT)((relative_x * analysis->glyph_count) /
                                     (uint64_t)run_width);
            if (glyph_slot >= analysis->glyph_count)
                glyph_slot = analysis->glyph_count - 1;
            LONG cell_left = analysis->bounds.left +
                (LONG)(((uint64_t)run_width * glyph_slot) /
                       analysis->glyph_count);
            LONG cell_right = analysis->bounds.left +
                (LONG)(((uint64_t)run_width * (glyph_slot + 1)) /
                       analysis->glyph_count);
            LONG cell_width = cell_right - cell_left;
            if (cell_width < 1)
                continue;
            UINT glyph_x = (UINT)(((uint64_t)(global_x - cell_left) * 8) /
                                  (uint64_t)cell_width);
            if (glyph_x > 7) glyph_x = 7;

            USHORT glyph_index = analysis->raster_chars[glyph_slot];
            const uint8_t *glyph =
                (glyph_index >= 32 && glyph_index <= 126)
                    ? gui_font8x16[glyph_index - 32]
                    : gui_font8x16['?' - 32];
            if (!(glyph[glyph_y] & (0x80u >> glyph_x)))
                continue;

            uint64_t pixel = (y * width + x) * channels;
            for (uint64_t channel = 0; channel < channels; channel++)
                alpha_values[pixel + channel] = 255;
        }
    }
    dwrite_trace("[DWRITE] analysis.CreateAlphaTexture\n");
    return 0;
}

static LONG WINAPI dwrite_analysis_GetAlphaBlendParams(
    PVOID self, PVOID rendering_params, float *gamma, float *contrast,
    float *cleartype_level)
{
    (void)self;
    (void)rendering_params;
    if (!gamma || !contrast || !cleartype_level) return DWRITE_E_POINTER;
    *gamma = 2.2f;
    *contrast = 1.0f;
    *cleartype_level = 1.0f;
    return 0;
}

static LONG WINAPI dwrite_factory_CreateGlyphRunAnalysis(
    PVOID self, const DWRITE_GLYPH_RUN_STUB *run, float pixels_per_dip,
    const DWRITE_MATRIX_STUB *transform, UINT rendering_mode,
    UINT measuring_mode, float baseline_x, float baseline_y, PVOID *result)
{
    (void)self;
    (void)rendering_mode;
    (void)measuring_mode;
    if (!run || !result) return DWRITE_E_POINTER;
    *result = NULL;
    if (!(run->font_em_size > 0.0f) || !(pixels_per_dip > 0.0f) ||
        run->glyph_count > 65536)
        return DWRITE_E_INVALIDARG;

    float advance = 0.0f;
    for (UINT i = 0; i < run->glyph_count; i++) {
        float value = run->glyph_advances
            ? run->glyph_advances[i] : run->font_em_size * 0.5f;
        if (!((value > 0.01f || value < -0.01f) &&
              value > -4096.0f && value < 4096.0f))
            value = run->font_em_size * 0.5f;
        if (value < 0.0f) value = -value;
        advance += value;
    }
    if (advance > 4096.0f) advance = 4096.0f;

    float x0 = baseline_x * pixels_per_dip;
    float x1 = (baseline_x + advance) * pixels_per_dip;
    float y0 = (baseline_y - run->font_em_size * 0.8f) * pixels_per_dip;
    float y1 = (baseline_y + run->font_em_size * 0.2f) * pixels_per_dip;
    float px[4], py[4];
    dwrite_transform_point(transform, x0, y0, &px[0], &py[0]);
    dwrite_transform_point(transform, x1, y0, &px[1], &py[1]);
    dwrite_transform_point(transform, x0, y1, &px[2], &py[2]);
    dwrite_transform_point(transform, x1, y1, &px[3], &py[3]);

    float min_x = px[0], max_x = px[0], min_y = py[0], max_y = py[0];
    for (int i = 1; i < 4; i++) {
        if (px[i] < min_x) min_x = px[i];
        if (px[i] > max_x) max_x = px[i];
        if (py[i] < min_y) min_y = py[i];
        if (py[i] > max_y) max_y = py[i];
    }
    if (!(min_x > -1048576.0f && max_x < 1048576.0f &&
          min_y > -1048576.0f && max_y < 1048576.0f))
        return DWRITE_E_INVALIDARG;

    const BYTE *cmap = NULL;
    UINT cmap_size = 0;
    PVOID cmap_context = NULL;
    BOOL cmap_exists = FALSE;
    DWRITE_FONT_FACE_STUB *face =
        (DWRITE_FONT_FACE_STUB *)run->font_face;
    if (face && face->lpVtbl == dwrite_face_vtbl) {
        LONG cmap_status = dwrite_face_TryGetFontTable(
            face, DWRITE_OPENTYPE_TAG_CMAP, (const void **)&cmap,
            &cmap_size, &cmap_context, &cmap_exists);
        if (cmap_status < 0 || !cmap_exists) {
            cmap = NULL;
            cmap_size = 0;
            cmap_context = NULL;
        }
    }

    SIZE_T analysis_size = sizeof(DWRITE_GLYPH_ANALYSIS_STUB) +
        (SIZE_T)run->glyph_count * sizeof(USHORT);
    DWRITE_GLYPH_ANALYSIS_STUB *analysis =
        (DWRITE_GLYPH_ANALYSIS_STUB *)kmalloc(analysis_size);
    if (!analysis) {
        if (cmap_context)
            dwrite_face_ReleaseFontTable(face, cmap_context);
        return (LONG)0x8007000E; /* E_OUTOFMEMORY */
    }
    analysis->lpVtbl = dwrite_analysis_vtbl;
    analysis->refs = 1;
    analysis->bounds.left = dwrite_floor_to_long(min_x);
    analysis->bounds.top = dwrite_floor_to_long(min_y);
    analysis->bounds.right = dwrite_ceil_to_long(max_x);
    analysis->bounds.bottom = dwrite_ceil_to_long(max_y);
    if (analysis->bounds.right - analysis->bounds.left > 4096)
        analysis->bounds.right = analysis->bounds.left + 4096;
    if (analysis->bounds.bottom - analysis->bounds.top > 4096)
        analysis->bounds.bottom = analysis->bounds.top + 4096;
    analysis->glyph_count = run->glyph_count;
    for (UINT i = 0; i < run->glyph_count; i++) {
        USHORT glyph = run->glyph_indices
            ? run->glyph_indices[i] : (USHORT)'?';
        USHORT raster_char = cmap
            ? dwrite_cmap_glyph_to_ascii(cmap, cmap_size, glyph) : 0;
        USHORT source_glyph = glyph;
        for (UINT depth = 0; !raster_char && cmap && depth < 4; depth++) {
            USHORT source = dwrite_reverse_substituted_glyph(
                face, source_glyph);
            if (!source || source == source_glyph) break;
            source_glyph = source;
            raster_char = dwrite_cmap_glyph_to_ascii(
                cmap, cmap_size, source_glyph);
        }
        if (!raster_char && glyph >= 32 && glyph <= 126)
            raster_char = glyph;
        if (!raster_char && dwrite_glyph_trace_count++ < 32) {
            serial_puts("[DWRITE-UNMAPPED] id=");
            serial_putdec(glyph);
            serial_puts(cmap ? " cmap=1\n" : " cmap=0\n");
        }
        analysis->raster_chars[i] = raster_char ? raster_char : (USHORT)'?';
    }
    if (cmap_context)
        dwrite_face_ReleaseFontTable(face, cmap_context);
    *result = analysis;
    dwrite_trace("[DWRITE] factory.CreateGlyphRunAnalysis\n");
    return 0;
}

static LONG dwrite_static_QueryInterface(PVOID self, LPCGUID iid,
                                         LPCGUID primary, LPCGUID secondary,
                                         PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, primary) &&
        (!secondary || !dwrite_guid_equal(iid, secondary)))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    return 0;
}

static LONG WINAPI dwrite_collection_QueryInterface(PVOID self, LPCGUID iid,
                                                     PVOID *object)
{
    LONG status = dwrite_static_QueryInterface(
        self, iid, &dwrite_iid_collection, NULL, object);
    if (status >= 0)
        dwrite_collection_AddRef(self);
    return status;
}

static UINT WINAPI dwrite_collection_GetFontFamilyCount(PVOID self)
{
    DWRITE_COLLECTION_STUB *collection = (DWRITE_COLLECTION_STUB *)self;
    dwrite_trace("[DWRITE] collection.GetFontFamilyCount\n");
    return collection ? collection->family_count : 0;
}

static LONG WINAPI dwrite_collection_GetFontFamily(PVOID self, UINT index,
                                                    PVOID *family)
{
    DWRITE_COLLECTION_STUB *collection = (DWRITE_COLLECTION_STUB *)self;
    dwrite_trace("[DWRITE] collection.GetFontFamily\n");
    if (!family) return DWRITE_E_POINTER;
    *family = NULL;
    if (!collection || index >= collection->family_count)
        return DWRITE_E_INVALIDARG;
    *family = &collection->family;
    dwrite_family_AddRef(*family);
    return 0;
}

static LONG WINAPI dwrite_collection_FindFamilyName(PVOID self,
                                                     PCWSTR name,
                                                     UINT *index,
                                                     BOOL *exists)
{
    DWRITE_COLLECTION_STUB *collection = (DWRITE_COLLECTION_STUB *)self;
    dwrite_trace("[DWRITE] collection.FindFamilyName -> family 0\n");
    if (!name || !index || !exists) return DWRITE_E_POINTER;
    *index = 0;
    *exists = collection && collection->family_count != 0;
    return 0;
}

static LONG WINAPI dwrite_collection_GetFontFromFontFace(PVOID self,
                                                         PVOID face,
                                                         PVOID *font)
{
    DWRITE_COLLECTION_STUB *collection = (DWRITE_COLLECTION_STUB *)self;
    DWRITE_FONT_FACE_STUB *font_face = (DWRITE_FONT_FACE_STUB *)face;
    dwrite_trace("[DWRITE] collection.GetFontFromFontFace\n");
    if (!font) return DWRITE_E_POINTER;
    *font = NULL;
    if (!collection || !font_face || font_face->lpVtbl != dwrite_face_vtbl ||
        font_face->font_file_count != collection->font_file_count)
        return DWRITE_E_INVALIDARG;
    for (UINT i = 0; i < collection->font_file_count; i++)
        if (font_face->font_files[i] != collection->font_files[i])
            return DWRITE_E_INVALIDARG;
    *font = &collection->family.font;
    dwrite_font_AddRef(*font);
    return 0;
}

static LONG WINAPI dwrite_family_QueryInterface(PVOID self, LPCGUID iid,
                                                 PVOID *object)
{
    LONG status = dwrite_static_QueryInterface(
        self, iid, &dwrite_iid_family, &dwrite_iid_font_list, object);
    if (status >= 0) dwrite_family_AddRef(self);
    return status;
}

static LONG WINAPI dwrite_family_GetFontCollection(PVOID self,
                                                    PVOID *collection)
{
    DWRITE_FAMILY_STUB *family = (DWRITE_FAMILY_STUB *)self;
    if (!collection) return DWRITE_E_POINTER;
    *collection = family ? family->collection : NULL;
    if (!*collection) return DWRITE_E_INVALIDARG;
    dwrite_collection_AddRef(*collection);
    return 0;
}

static UINT WINAPI dwrite_family_GetFontCount(PVOID self)
{
    DWRITE_FAMILY_STUB *family = (DWRITE_FAMILY_STUB *)self;
    UINT count = family && family->collection &&
                 family->collection->font_file_count ? 1 : 0;
    dwrite_trace(count ? "[DWRITE] family.GetFontCount -> 1\n" :
                         "[DWRITE] family.GetFontCount -> 0\n");
    return count;
}

static LONG WINAPI dwrite_family_GetFont(PVOID self, UINT index, PVOID *font)
{
    DWRITE_FAMILY_STUB *family = (DWRITE_FAMILY_STUB *)self;
    dwrite_trace("[DWRITE] family.GetFont\n");
    if (!font) return DWRITE_E_POINTER;
    *font = NULL;
    if (!family || !family->collection ||
        !family->collection->font_file_count || index != 0)
        return DWRITE_E_INVALIDARG;
    *font = &family->font;
    dwrite_font_AddRef(*font);
    return 0;
}

static LONG WINAPI dwrite_family_GetFamilyNames(PVOID self, PVOID *names)
{
    (void)self;
    dwrite_trace("[DWRITE] family.GetFamilyNames\n");
    if (!names) return DWRITE_E_POINTER;
    *names = &dwrite_family_names;
    return 0;
}

static LONG WINAPI dwrite_family_GetFirstMatchingFont(PVOID self, UINT weight,
                                                       UINT stretch, UINT style,
                                                       PVOID *font)
{
    DWRITE_FAMILY_STUB *family = (DWRITE_FAMILY_STUB *)self;
    (void)weight;
    (void)stretch;
    (void)style;
    dwrite_trace("[DWRITE] family.GetFirstMatchingFont\n");
    if (!font) return DWRITE_E_POINTER;
    *font = NULL;
    if (!family || !family->collection ||
        !family->collection->font_file_count)
        return DWRITE_E_NOFONT;
    *font = &family->font;
    dwrite_font_AddRef(*font);
    return 0;
}

static LONG WINAPI dwrite_family_GetMatchingFonts(PVOID self, UINT weight,
                                                   UINT stretch, UINT style,
                                                   PVOID *fonts)
{
    (void)weight;
    (void)stretch;
    (void)style;
    if (!fonts) return DWRITE_E_POINTER;
    *fonts = self;
    dwrite_family_AddRef(self);
    return 0;
}

static LONG WINAPI dwrite_font_QueryInterface(PVOID self, LPCGUID iid,
                                               PVOID *object)
{
    LONG status = dwrite_static_QueryInterface(
        self, iid, &dwrite_iid_font, NULL, object);
    if (status >= 0) dwrite_font_AddRef(self);
    return status;
}

static LONG WINAPI dwrite_font_GetFontFamily(PVOID self, PVOID *family)
{
    DWRITE_FONT_STUB *font = (DWRITE_FONT_STUB *)self;
    if (!family) return DWRITE_E_POINTER;
    *family = font ? font->family : NULL;
    if (!*family) return DWRITE_E_INVALIDARG;
    dwrite_family_AddRef(*family);
    return 0;
}

static UINT WINAPI dwrite_font_GetWeight(PVOID self)
{
    (void)self;
    return 400;
}

static UINT WINAPI dwrite_font_GetStretch(PVOID self)
{
    (void)self;
    return 5;
}

static UINT WINAPI dwrite_font_GetStyle(PVOID self)
{
    (void)self;
    return 0;
}

static BOOL WINAPI dwrite_font_IsSymbolFont(PVOID self)
{
    (void)self;
    return FALSE;
}

static LONG WINAPI dwrite_font_GetFaceNames(PVOID self, PVOID *names)
{
    (void)self;
    if (!names) return DWRITE_E_POINTER;
    *names = &dwrite_face_names;
    return 0;
}

static LONG WINAPI dwrite_font_GetInformationalStrings(PVOID self, UINT id,
                                                        PVOID *strings,
                                                        BOOL *exists)
{
    (void)self;
    (void)id;
    if (!strings || !exists) return DWRITE_E_POINTER;
    *strings = NULL;
    *exists = FALSE;
    return 0;
}

static UINT WINAPI dwrite_font_GetSimulations(PVOID self)
{
    (void)self;
    return 0;
}

static void dwrite_fill_font_metrics(DWRITE_FONT_METRICS_STUB *metrics)
{
    if (!metrics) return;
    metrics->design_units_per_em = 2048;
    metrics->ascent = 1900;
    metrics->descent = 500;
    metrics->line_gap = 0;
    metrics->cap_height = 1462;
    metrics->x_height = 1024;
    metrics->underline_position = -200;
    metrics->underline_thickness = 100;
    metrics->strikethrough_position = 600;
    metrics->strikethrough_thickness = 100;
}

static void WINAPI dwrite_font_GetMetrics(PVOID self,
                                          DWRITE_FONT_METRICS_STUB *metrics)
{
    (void)self;
    dwrite_fill_font_metrics(metrics);
}

static LONG WINAPI dwrite_font_HasCharacter(PVOID self, UINT value,
                                             BOOL *exists)
{
    (void)self;
    (void)value;
    if (!exists) return DWRITE_E_POINTER;
    *exists = TRUE;
    return 0;
}

static LONG WINAPI dwrite_font_CreateFontFace(PVOID self, PVOID *face)
{
    DWRITE_FONT_STUB *font = (DWRITE_FONT_STUB *)self;
    dwrite_trace("[DWRITE] font.CreateFontFace\n");
    if (!face) return DWRITE_E_POINTER;
    *face = NULL;
    if (!font || !font->family || !font->family->collection)
        return DWRITE_E_INVALIDARG;
    DWRITE_COLLECTION_STUB *collection = font->family->collection;
    if (!collection->font_file_count) return DWRITE_E_NOFONT;
    return dwrite_factory_CreateFontFace(
        &dwrite_factory, DWRITE_FONT_FACE_TYPE_UNKNOWN,
        collection->font_file_count, (PVOID const *)collection->font_files,
        0, 0, face);
}

static LONG dwrite_open_font_stream(PVOID font_file, PVOID *stream)
{
    if (!stream) return DWRITE_E_POINTER;
    *stream = NULL;
    if (!font_file || !*(PVOID **)font_file)
        return DWRITE_E_INVALIDARG;
    PVOID *file_vtbl = *(PVOID **)font_file;
    if (!file_vtbl[3] || !file_vtbl[4]) return DWRITE_E_INVALIDARG;

    typedef LONG (WINAPI *GET_KEY_FN)(PVOID, const void **, UINT *);
    typedef LONG (WINAPI *GET_LOADER_FN)(PVOID, PVOID *);
    typedef LONG (WINAPI *CREATE_STREAM_FN)(PVOID, const void *, UINT,
                                             PVOID *);
    const void *key = NULL;
    UINT key_size = 0;
    LONG status = ((GET_KEY_FN)file_vtbl[3])(
        font_file, &key, &key_size);
    if (status < 0 || !key || !key_size)
        return status < 0 ? status : DWRITE_E_FILEFORMAT;

    PVOID loader = NULL;
    status = ((GET_LOADER_FN)file_vtbl[4])(font_file, &loader);
    if (status < 0 || !loader || !*(PVOID **)loader)
        return status < 0 ? status : DWRITE_E_FILEFORMAT;
    PVOID *loader_vtbl = *(PVOID **)loader;
    if (!loader_vtbl[3]) {
        dwrite_object_release(loader);
        return DWRITE_E_FILEFORMAT;
    }
    status = ((CREATE_STREAM_FN)loader_vtbl[3])(
        loader, key, key_size, stream);
    dwrite_object_release(loader);
    if (status < 0 || !*stream)
        return status < 0 ? status : DWRITE_E_FILEFORMAT;
    return 0;
}

static ULONG WINAPI dwrite_face_AddRef(PVOID self)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    if (!face) return 0;
    return __atomic_add_fetch(&face->refs, 1, __ATOMIC_RELAXED);
}

static ULONG WINAPI dwrite_face_Release(PVOID self)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    if (!face) return 0;
    ULONG refs = __atomic_sub_fetch(&face->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) {
        for (UINT i = 0; i < face->font_file_count; i++)
            dwrite_object_release(face->font_files[i]);
        kfree(face);
    }
    return refs;
}

static LONG WINAPI dwrite_face_QueryInterface(PVOID self, LPCGUID iid,
                                               PVOID *object)
{
    LONG status = dwrite_static_QueryInterface(
        self, iid, &dwrite_iid_face, NULL, object);
    if (status >= 0) dwrite_face_AddRef(self);
    return status;
}

static UINT WINAPI dwrite_face_GetType(PVOID self)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    return face ? face->face_type : DWRITE_FONT_FACE_TYPE_UNKNOWN;
}

static LONG WINAPI dwrite_face_GetFiles(PVOID self, UINT *file_count,
                                        PVOID *files)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    if (!file_count) return DWRITE_E_POINTER;
    if (!face || face->lpVtbl != dwrite_face_vtbl)
        return DWRITE_E_INVALIDARG;
    UINT required = face->font_file_count;
    if (!files) {
        *file_count = required;
        dwrite_trace("[DWRITE] face.GetFiles query\n");
        return 0;
    }
    UINT capacity = *file_count;
    *file_count = required;
    if (capacity < required) return DWRITE_E_INSUFFICIENT_BUFFER;
    for (UINT i = 0; i < required; i++) {
        files[i] = face->font_files[i];
        dwrite_object_addref(files[i]);
    }
    dwrite_trace("[DWRITE] face.GetFiles returned files\n");
    return 0;
}

static UINT WINAPI dwrite_face_GetIndex(PVOID self)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    return face ? face->face_index : 0;
}

static UINT WINAPI dwrite_face_GetSimulations(PVOID self)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    return face ? face->simulations : 0;
}

static BOOL WINAPI dwrite_face_IsSymbolFont(PVOID self)
{
    (void)self;
    return FALSE;
}

static void WINAPI dwrite_face_GetMetrics(PVOID self,
                                          DWRITE_FONT_METRICS_STUB *metrics)
{
    (void)self;
    dwrite_fill_font_metrics(metrics);
}

static USHORT WINAPI dwrite_face_GetGlyphCount(PVOID self)
{
    const void *maxp = NULL;
    UINT maxp_size = 0;
    PVOID context = NULL;
    BOOL exists = FALSE;
    USHORT glyph_count = 256;

    LONG status = dwrite_face_TryGetFontTable(
        self, DWRITE_OPENTYPE_TAG_MAXP, &maxp, &maxp_size, &context,
        &exists);
    if (status >= 0 && exists && maxp && maxp_size >= 6)
        glyph_count = dwrite_read_be16((const BYTE *)maxp + 4);
    if (context)
        dwrite_face_ReleaseFontTable(self, context);
    return glyph_count;
}

static void dwrite_fill_glyph_metrics(DWRITE_GLYPH_METRICS_STUB *metrics,
                                      UINT count)
{
    if (!metrics) return;
    for (UINT i = 0; i < count; i++) {
        metrics[i].left_side_bearing = 0;
        metrics[i].advance_width = 1024;
        metrics[i].right_side_bearing = 0;
        metrics[i].top_side_bearing = 0;
        metrics[i].advance_height = 2048;
        metrics[i].bottom_side_bearing = 0;
        metrics[i].vertical_origin_y = 1900;
    }
}

static LONG WINAPI dwrite_face_GetDesignGlyphMetrics(
    PVOID self, const USHORT *glyph_indices, UINT glyph_count,
    DWRITE_GLYPH_METRICS_STUB *metrics, BOOL sideways)
{
    (void)self;
    (void)glyph_indices;
    (void)sideways;
    if (!metrics) return DWRITE_E_POINTER;
    dwrite_fill_glyph_metrics(metrics, glyph_count);
    return 0;
}

static LONG WINAPI dwrite_face_GetGlyphIndices(PVOID self,
                                                const UINT *codepoints,
                                                UINT count,
                                                USHORT *glyph_indices)
{
    if (!codepoints || !glyph_indices) return DWRITE_E_POINTER;

    const void *cmap = NULL;
    UINT cmap_size = 0;
    PVOID context = NULL;
    BOOL exists = FALSE;
    LONG status = dwrite_face_TryGetFontTable(
        self, DWRITE_OPENTYPE_TAG_CMAP, &cmap, &cmap_size, &context,
        &exists);
    if (status < 0)
        return status;

    for (UINT i = 0; i < count; i++)
        glyph_indices[i] = exists && cmap
            ? dwrite_cmap_lookup((const BYTE *)cmap, cmap_size,
                                 codepoints[i])
            : (codepoints[i] && codepoints[i] < 256
                ? (USHORT)codepoints[i] : 0);
    if (context)
        dwrite_face_ReleaseFontTable(self, context);
    return 0;
}

static LONG WINAPI dwrite_face_TryGetFontTable(PVOID self, UINT table_tag,
                                               const void **table_data,
                                               UINT *table_size,
                                               PVOID *context, BOOL *exists)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    if (!table_data || !table_size || !context || !exists)
        return DWRITE_E_POINTER;
    *table_data = NULL;
    *table_size = 0;
    *context = NULL;
    *exists = FALSE;
    if (!face || !face->font_file_count) return DWRITE_E_INVALIDARG;

    PVOID stream = NULL;
    LONG status = dwrite_open_font_stream(face->font_files[0], &stream);
    if (status < 0) return status;
    PVOID *stream_vtbl = *(PVOID **)stream;
    if (!stream_vtbl || !stream_vtbl[3] || !stream_vtbl[4]) {
        dwrite_object_release(stream);
        return DWRITE_E_FILEFORMAT;
    }
    typedef LONG (WINAPI *READ_FRAGMENT_FN)(PVOID, const void **, uint64_t,
                                             uint64_t, PVOID *);
    typedef void (WINAPI *RELEASE_FRAGMENT_FN)(PVOID, PVOID);
    READ_FRAGMENT_FN read_fragment = (READ_FRAGMENT_FN)stream_vtbl[3];
    RELEASE_FRAGMENT_FN release_fragment =
        (RELEASE_FRAGMENT_FN)stream_vtbl[4];

    const BYTE *header = NULL;
    PVOID header_context = NULL;
    PVOID offset_context = NULL;
    PVOID directory_context = NULL;
    PVOID fragment_context = NULL;
    status = read_fragment(stream, (const void **)&header, 0, 12,
                           &header_context);
    if (status < 0 || !header) goto table_error;
    uint64_t face_offset = 0;
    if (dwrite_read_be32(header) == 0x74746366U) { /* ttcf */
        UINT count = dwrite_read_be32(header + 8);
        release_fragment(stream, header_context);
        header = NULL;
        header_context = NULL;
        if (face->face_index >= count) {
            status = DWRITE_E_FILEFORMAT;
            goto table_error;
        }
        const BYTE *offset_data = NULL;
        status = read_fragment(stream, (const void **)&offset_data,
                               12ULL + (uint64_t)face->face_index * 4ULL,
                               4, &offset_context);
        if (status < 0 || !offset_data) goto table_error;
        face_offset = dwrite_read_be32(offset_data);
        release_fragment(stream, offset_context);
        offset_context = NULL;
        status = read_fragment(stream, (const void **)&header, face_offset,
                               12, &header_context);
        if (status < 0 || !header) goto table_error;
    }

    UINT table_count = ((UINT)header[4] << 8) | header[5];
    release_fragment(stream, header_context);
    header = NULL;
    header_context = NULL;
    if (!table_count || table_count > 4096) {
        status = DWRITE_E_FILEFORMAT;
        goto table_error;
    }

    const BYTE *directory = NULL;
    uint64_t directory_size = (uint64_t)table_count * 16ULL;
    status = read_fragment(stream, (const void **)&directory,
                           face_offset + 12ULL, directory_size,
                           &directory_context);
    if (status < 0 || !directory) goto table_error;

    UINT file_tag = ((table_tag & 0x000000FFU) << 24) |
                    ((table_tag & 0x0000FF00U) << 8) |
                    ((table_tag & 0x00FF0000U) >> 8) |
                    ((table_tag & 0xFF000000U) >> 24);
    uint64_t found_offset = 0;
    UINT found_size = 0;
    for (UINT i = 0; i < table_count; i++) {
        const BYTE *record = directory + (uint64_t)i * 16ULL;
        if (dwrite_read_be32(record) == file_tag) {
            found_offset = dwrite_read_be32(record + 8);
            found_size = dwrite_read_be32(record + 12);
            break;
        }
    }
    release_fragment(stream, directory_context);
    directory_context = NULL;
    if (!found_size) {
        dwrite_object_release(stream);
        return 0;
    }

    const void *font_table = NULL;
    status = read_fragment(stream, &font_table, found_offset, found_size,
                           &fragment_context);
    if (status < 0 || !font_table) goto table_error;
    DWRITE_FONT_TABLE_CONTEXT *table_context =
        (DWRITE_FONT_TABLE_CONTEXT *)kmalloc(sizeof(*table_context));
    if (!table_context) {
        release_fragment(stream, fragment_context);
        fragment_context = NULL;
        status = DWRITE_E_OUTOFMEMORY;
        goto table_error;
    }
    table_context->stream = stream;
    table_context->fragment_context = fragment_context;
    *table_data = font_table;
    *table_size = found_size;
    *context = table_context;
    *exists = TRUE;
    return 0;

table_error:
    if (header_context) release_fragment(stream, header_context);
    if (offset_context) release_fragment(stream, offset_context);
    if (directory_context) release_fragment(stream, directory_context);
    if (fragment_context) release_fragment(stream, fragment_context);
    dwrite_object_release(stream);
    return status < 0 ? status : DWRITE_E_FILEFORMAT;
}

static void WINAPI dwrite_face_ReleaseFontTable(PVOID self, PVOID context)
{
    (void)self;
    DWRITE_FONT_TABLE_CONTEXT *table_context =
        (DWRITE_FONT_TABLE_CONTEXT *)context;
    if (!table_context || !table_context->stream) return;
    PVOID *stream_vtbl = *(PVOID **)table_context->stream;
    if (stream_vtbl && stream_vtbl[4]) {
        typedef void (WINAPI *RELEASE_FRAGMENT_FN)(PVOID, PVOID);
        ((RELEASE_FRAGMENT_FN)stream_vtbl[4])(
            table_context->stream, table_context->fragment_context);
    }
    dwrite_object_release(table_context->stream);
    kfree(table_context);
}

static LONG WINAPI dwrite_face_GetGlyphRunOutline(PVOID self, float em_size,
                                                  const USHORT *indices,
                                                  const float *advances,
                                                  const PVOID offsets,
                                                  UINT count, BOOL sideways,
                                                  BOOL rtl, PVOID sink)
{
    (void)self;
    (void)em_size;
    (void)indices;
    (void)advances;
    (void)offsets;
    (void)count;
    (void)sideways;
    (void)rtl;
    if (!sink) return DWRITE_E_POINTER;
    dwrite_trace("[DWRITE] face.GetGlyphRunOutline -> empty geometry\n");
    return 0;
}

static LONG WINAPI dwrite_face_GetRecommendedRenderingMode(
    PVOID self, float em_size, float pixels_per_dip, UINT measuring_mode,
    PVOID params, UINT *rendering_mode)
{
    (void)self;
    (void)em_size;
    (void)pixels_per_dip;
    (void)measuring_mode;
    (void)params;
    if (!rendering_mode) return DWRITE_E_POINTER;
    *rendering_mode = 4;
    return 0;
}

static LONG WINAPI dwrite_face_GetGdiCompatibleMetrics(
    PVOID self, float em_size, float pixels_per_dip, const PVOID transform,
    DWRITE_FONT_METRICS_STUB *metrics)
{
    (void)self;
    (void)em_size;
    (void)pixels_per_dip;
    (void)transform;
    if (!metrics) return DWRITE_E_POINTER;
    dwrite_fill_font_metrics(metrics);
    return 0;
}

static LONG WINAPI dwrite_face_GetGdiCompatibleGlyphMetrics(
    PVOID self, float em_size, float pixels_per_dip, const PVOID transform,
    BOOL use_gdi_natural, const USHORT *indices, UINT count,
    DWRITE_GLYPH_METRICS_STUB *metrics, BOOL sideways)
{
    (void)self;
    (void)em_size;
    (void)pixels_per_dip;
    (void)transform;
    (void)use_gdi_natural;
    (void)indices;
    (void)sideways;
    if (!metrics) return DWRITE_E_POINTER;
    dwrite_fill_glyph_metrics(metrics, count);
    return 0;
}

static LONG WINAPI dwrite_strings_QueryInterface(PVOID self, LPCGUID iid,
                                                  PVOID *object)
{
    return dwrite_static_QueryInterface(self, iid, &dwrite_iid_strings,
                                        NULL, object);
}

static UINT dwrite_wstrlen(PCWSTR value)
{
    UINT len = 0;
    if (value)
        while (value[len]) len++;
    return len;
}

static LONG dwrite_copy_wstring(PCWSTR source, PWSTR dest, UINT size)
{
    UINT len = dwrite_wstrlen(source);
    if (!dest) return DWRITE_E_POINTER;
    if (size <= len) return DWRITE_E_INSUFFICIENT_BUFFER;
    for (UINT i = 0; i <= len; i++) dest[i] = source[i];
    return 0;
}

static UINT WINAPI dwrite_strings_GetCount(PVOID self)
{
    (void)self;
    return 1;
}

static LONG WINAPI dwrite_strings_FindLocaleName(PVOID self, PCWSTR locale,
                                                  UINT *index, BOOL *exists)
{
    (void)self;
    (void)locale;
    if (!index || !exists) return DWRITE_E_POINTER;
    *index = 0;
    *exists = TRUE;
    return 0;
}

static LONG WINAPI dwrite_strings_GetLocaleNameLength(PVOID self, UINT index,
                                                       UINT *length)
{
    (void)self;
    if (!length) return DWRITE_E_POINTER;
    if (index != 0) return DWRITE_E_INVALIDARG;
    *length = dwrite_wstrlen(dwrite_locale_name);
    return 0;
}

static LONG WINAPI dwrite_strings_GetLocaleName(PVOID self, UINT index,
                                                 PWSTR locale, UINT size)
{
    (void)self;
    if (index != 0) return DWRITE_E_INVALIDARG;
    return dwrite_copy_wstring(dwrite_locale_name, locale, size);
}

static LONG WINAPI dwrite_strings_GetStringLength(PVOID self, UINT index,
                                                   UINT *length)
{
    DWRITE_STRINGS_STUB *strings = (DWRITE_STRINGS_STUB *)self;
    if (!length) return DWRITE_E_POINTER;
    if (index != 0) return DWRITE_E_INVALIDARG;
    *length = dwrite_wstrlen(strings->value);
    return 0;
}

static LONG WINAPI dwrite_strings_GetString(PVOID self, UINT index,
                                             PWSTR value, UINT size)
{
    DWRITE_STRINGS_STUB *strings = (DWRITE_STRINGS_STUB *)self;
    if (index != 0) return DWRITE_E_INVALIDARG;
    return dwrite_copy_wstring(strings->value, value, size);
}

static LONG WINAPI dwrite_factory_RegisterFontCollectionLoader(PVOID self,
                                                                 PVOID loader)
{
    (void)self;
    LONG status = dwrite_register_loader(dwrite_collection_loaders, loader);
    if (status >= 0)
        dwrite_trace("[DWRITE] registered font collection loader\n");
    return status;
}

static LONG WINAPI dwrite_factory_UnregisterFontCollectionLoader(PVOID self,
                                                                   PVOID loader)
{
    (void)self;
    LONG status = dwrite_unregister_loader(dwrite_collection_loaders, loader);
    if (status >= 0)
        dwrite_trace("[DWRITE] unregistered font collection loader\n");
    return status;
}

static LONG WINAPI dwrite_factory_RegisterFontFileLoader(PVOID self,
                                                           PVOID loader)
{
    (void)self;
    LONG status = dwrite_register_loader(dwrite_file_loaders, loader);
    if (status >= 0)
        dwrite_trace("[DWRITE] registered font file loader\n");
    return status;
}

static LONG WINAPI dwrite_factory_UnregisterFontFileLoader(PVOID self,
                                                             PVOID loader)
{
    (void)self;
    LONG status = dwrite_unregister_loader(dwrite_file_loaders, loader);
    if (status >= 0)
        dwrite_trace("[DWRITE] unregistered font file loader\n");
    return status;
}

static LONG WINAPI dwrite_factory_CreateFontFileReference(
    PVOID self, PCWSTR file_path, const PVOID last_write_time,
    PVOID *font_file)
{
    (void)self;
    (void)last_write_time;
    if (!font_file) return DWRITE_E_POINTER;
    *font_file = NULL;
    if (!file_path) return DWRITE_E_INVALIDARG;

    char path[4096];
    LONG status = dwrite_utf16_path_to_utf8(file_path, path, sizeof(path));
    if (status < 0) return status;
    status = dwrite_create_local_font_file(path, font_file);
    if (status >= 0)
        dwrite_trace("[DWRITE] factory.CreateFontFileReference\n");
    return status;
}

static LONG WINAPI dwrite_factory_CreateCustomFontFileReference(
    PVOID self, const void *key, UINT key_size, PVOID loader, PVOID *font_file)
{
    (void)self;
    if (!font_file) return DWRITE_E_POINTER;
    *font_file = NULL;
    if (!key || !key_size || key_size > 1024 * 1024 || !loader)
        return DWRITE_E_INVALIDARG;

    PVOID retained_loader = dwrite_acquire_registered_loader(
        dwrite_file_loaders, loader);
    if (!retained_loader)
        return DWRITE_E_INVALIDARG;

    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)kmalloc(
        sizeof(*file) + key_size);
    if (!file) {
        dwrite_object_release(retained_loader);
        return DWRITE_E_OUTOFMEMORY;
    }
    file->lpVtbl = dwrite_font_file_vtbl;
    file->refs = 1;
    file->loader = retained_loader;
    file->key_size = key_size;
    file->gsub_cache_lock = 0;
    file->gsub_cache_count = 0;
    gdi_memcpy(file->key, key, key_size);
    *font_file = file;
    return 0;
}

static void dwrite_release_font_files(PVOID *files, UINT count)
{
    if (!files) return;
    for (UINT i = 0; i < count; i++)
        dwrite_object_release(files[i]);
    kfree(files);
}

static LONG WINAPI dwrite_factory_CreateCustomFontCollection(
    PVOID self, PVOID loader, const void *key, UINT key_size,
    PVOID *font_collection)
{
    if (!font_collection) return DWRITE_E_POINTER;
    *font_collection = NULL;
    if (!loader || (key_size && !key) || key_size > 1024 * 1024)
        return DWRITE_E_INVALIDARG;

    PVOID retained_loader = dwrite_acquire_registered_loader(
        dwrite_collection_loaders, loader);
    if (!retained_loader)
        return DWRITE_E_INVALIDARG;

    PVOID *loader_vtbl = *(PVOID **)retained_loader;
    if (!loader_vtbl || !loader_vtbl[3]) {
        dwrite_object_release(retained_loader);
        return DWRITE_E_INVALIDARG;
    }

    typedef LONG (WINAPI *CREATE_ENUMERATOR_FN)(PVOID, PVOID, const void *,
                                                 UINT, PVOID *);
    PVOID enumerator = NULL;
    LONG status = ((CREATE_ENUMERATOR_FN)loader_vtbl[3])(
        retained_loader, self, key, key_size, &enumerator);
    dwrite_object_release(retained_loader);
    if (status < 0 || !enumerator)
        return status < 0 ? status : DWRITE_E_NOFONT;

    PVOID *enum_vtbl = *(PVOID **)enumerator;
    if (!enum_vtbl || !enum_vtbl[3] || !enum_vtbl[4]) {
        dwrite_object_release(enumerator);
        return DWRITE_E_INVALIDARG;
    }

    typedef LONG (WINAPI *MOVE_NEXT_FN)(PVOID, BOOL *);
    typedef LONG (WINAPI *GET_CURRENT_FILE_FN)(PVOID, PVOID *);
    PVOID *files = NULL;
    UINT file_count = 0;
    UINT capacity = 0;

    for (;;) {
        BOOL has_file = FALSE;
        status = ((MOVE_NEXT_FN)enum_vtbl[3])(enumerator, &has_file);
        if (status < 0 || !has_file)
            break;
        if (file_count >= 4096) {
            status = DWRITE_E_OUTOFMEMORY;
            break;
        }

        PVOID file = NULL;
        status = ((GET_CURRENT_FILE_FN)enum_vtbl[4])(enumerator, &file);
        if (status < 0 || !file) {
            if (status >= 0) status = DWRITE_E_FILEFORMAT;
            break;
        }

        PVOID *file_vtbl = *(PVOID **)file;
        if (!file_vtbl || !file_vtbl[5]) {
            dwrite_object_release(file);
            status = DWRITE_E_FILEFORMAT;
            break;
        }
        typedef LONG (WINAPI *ANALYZE_FILE_FN)(PVOID, BOOL *, UINT *,
                                                UINT *, UINT *);
        BOOL supported = FALSE;
        UINT file_type = 0, face_type = 0, face_count = 0;
        status = ((ANALYZE_FILE_FN)file_vtbl[5])(
            file, &supported, &file_type, &face_type, &face_count);
        if (status < 0) {
            dwrite_object_release(file);
            break;
        }
        if (!supported || !face_count) {
            dwrite_object_release(file);
            continue;
        }

        if (file_count == capacity) {
            UINT next_capacity = capacity ? capacity * 2 : 4;
            PVOID *grown = (PVOID *)krealloc(
                files, (uint64_t)next_capacity * sizeof(*files));
            if (!grown) {
                dwrite_object_release(file);
                status = DWRITE_E_OUTOFMEMORY;
                break;
            }
            files = grown;
            capacity = next_capacity;
        }
        files[file_count++] = file;
    }
    dwrite_object_release(enumerator);

    if (status < 0) {
        dwrite_release_font_files(files, file_count);
        return status;
    }
    if (!file_count) {
        kfree(files);
        return DWRITE_E_NOFONT;
    }

    DWRITE_COLLECTION_STUB *collection =
        (DWRITE_COLLECTION_STUB *)kmalloc(sizeof(*collection));
    if (!collection) {
        dwrite_release_font_files(files, file_count);
        return DWRITE_E_OUTOFMEMORY;
    }
    collection->lpVtbl = dwrite_collection_vtbl;
    collection->refs = 1;
    collection->family_count = 1;
    collection->font_file_count = file_count;
    collection->heap_owned = TRUE;
    collection->font_files = files;
    collection->family.lpVtbl = dwrite_family_vtbl;
    collection->family.collection = collection;
    collection->family.font.lpVtbl = dwrite_font_vtbl;
    collection->family.font.family = &collection->family;
    *font_collection = collection;
    dwrite_trace("[DWRITE] factory.CreateCustomFontCollection\n");
    return 0;
}

static LONG WINAPI dwrite_factory_CreateFontFace(
    PVOID self, UINT requested_face_type, UINT file_count,
    PVOID const *font_files, UINT face_index, UINT simulations,
    PVOID *font_face)
{
    (void)self;
    if (!font_face) return DWRITE_E_POINTER;
    *font_face = NULL;
    if (!file_count || !font_files || (simulations & ~3U))
        return DWRITE_E_INVALIDARG;

    UINT available_faces = 0;
    UINT resolved_face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    for (UINT i = 0; i < file_count; i++) {
        if (!font_files[i] || !*(PVOID **)font_files[i] ||
            !(*(PVOID **)font_files[i])[5])
            return DWRITE_E_INVALIDARG;
        typedef LONG (WINAPI *ANALYZE_FILE_FN)(PVOID, BOOL *, UINT *,
                                                UINT *, UINT *);
        BOOL supported = FALSE;
        UINT file_type = 0, face_type = 0, faces = 0;
        LONG status = ((ANALYZE_FILE_FN)(*(PVOID **)font_files[i])[5])(
            font_files[i], &supported, &file_type, &face_type, &faces);
        if (status < 0) return status;
        if (!supported || !faces) return DWRITE_E_FILEFORMAT;
        if (resolved_face_type == DWRITE_FONT_FACE_TYPE_UNKNOWN)
            resolved_face_type = face_type;
        if (requested_face_type != DWRITE_FONT_FACE_TYPE_UNKNOWN &&
            requested_face_type != face_type)
            return DWRITE_E_INVALIDARG;
        if (available_faces > UINT32_MAX - faces)
            return DWRITE_E_INVALIDARG;
        available_faces += faces;
    }
    if (face_index >= available_faces)
        return DWRITE_E_INVALIDARG;

    if (file_count > (UINT32_MAX - sizeof(DWRITE_FONT_FACE_STUB)) /
                     sizeof(PVOID))
        return DWRITE_E_OUTOFMEMORY;
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)kmalloc(
        sizeof(*face) + (uint64_t)file_count * sizeof(PVOID));
    if (!face) return DWRITE_E_OUTOFMEMORY;
    face->lpVtbl = dwrite_face_vtbl;
    face->refs = 1;
    face->face_type = requested_face_type == DWRITE_FONT_FACE_TYPE_UNKNOWN
        ? resolved_face_type : requested_face_type;
    face->face_index = face_index;
    face->simulations = simulations;
    face->font_file_count = file_count;
    for (UINT i = 0; i < file_count; i++) {
        face->font_files[i] = font_files[i];
        dwrite_object_addref(face->font_files[i]);
    }
    *font_face = face;
    dwrite_trace("[DWRITE] factory.CreateFontFace\n");
    return 0;
}

static BOOL dwrite_system_font_missing_reported;

static void dwrite_system_collections_lock(void)
{
    while (__atomic_exchange_n(&dwrite_system_collection_lock, 1,
                               __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void dwrite_system_collections_unlock(void)
{
    __atomic_store_n(&dwrite_system_collection_lock, 0, __ATOMIC_RELEASE);
}

static BOOL dwrite_try_system_font(const char *path, PVOID *file_out)
{
    *file_out = NULL;
    PVOID file = NULL;
    if (dwrite_create_local_font_file(path, &file) < 0) return FALSE;
    BOOL supported = FALSE;
    UINT file_type = 0, face_type = 0, face_count = 0;
    LONG status = dwrite_font_file_Analyze(
        file, &supported, &file_type, &face_type, &face_count);
    if (status < 0 || !supported || !face_count) {
        dwrite_object_release(file);
        return FALSE;
    }
    *file_out = file;
    return TRUE;
}

static LONG dwrite_build_system_font_collection(
    DWORD process_id, DWRITE_COLLECTION_STUB **collection_out,
    uint64_t *generation_out)
{
    if (!collection_out || !generation_out) return DWRITE_E_POINTER;
    *collection_out = NULL;

    char **registered_paths = NULL;
    UINT registered_count = 0;
    if (!gdi_font_resource_snapshot_paths(
            process_id, &registered_paths, &registered_count,
            generation_out))
        return DWRITE_E_OUTOFMEMORY;

    static const char *const candidates[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\System\\Windows\\Fonts\\segoeui.ttf",
        NULL
    };
    PVOID file = NULL;
    const char *selected_path = NULL;
    for (UINT i = 0; candidates[i]; i++) {
        if (dwrite_try_system_font(candidates[i], &file)) {
            selected_path = candidates[i];
            break;
        }
    }
    for (UINT i = 0; !file && i < registered_count; i++) {
        if (dwrite_try_system_font(registered_paths[i], &file)) {
            selected_path = registered_paths[i];
            break;
        }
    }

    DWRITE_COLLECTION_STUB *collection =
        (DWRITE_COLLECTION_STUB *)kmalloc(sizeof(*collection));
    PVOID *files = file ? (PVOID *)kmalloc(sizeof(*files)) : NULL;
    if (!collection || (file && !files)) {
        dwrite_object_release(file);
        kfree(files);
        kfree(collection);
        gdi_font_resource_free_path_snapshot(
            registered_paths, registered_count);
        return DWRITE_E_OUTOFMEMORY;
    }
    if (file) files[0] = file;
    collection->lpVtbl = dwrite_collection_vtbl;
    collection->refs = 1; /* cache ownership */
    collection->family_count = file ? 1 : 0;
    collection->font_file_count = file ? 1 : 0;
    collection->heap_owned = TRUE;
    collection->font_files = files;
    collection->family.lpVtbl = dwrite_family_vtbl;
    collection->family.collection = collection;
    collection->family.font.lpVtbl = dwrite_font_vtbl;
    collection->family.font.family = &collection->family;

    if (selected_path) {
        serial_puts("[DWRITE] system font: ");
        serial_puts(selected_path);
        serial_puts("\n");
    } else if (!dwrite_system_font_missing_reported) {
        dwrite_system_font_missing_reported = TRUE;
        serial_puts("[DWRITE] no usable system font file found\n");
    }

    gdi_font_resource_free_path_snapshot(registered_paths, registered_count);
    *collection_out = collection;
    return 0;
}

static LONG dwrite_acquire_system_font_collection(
    BOOL check_updates, DWRITE_COLLECTION_STUB **collection_out)
{
    if (!collection_out) return DWRITE_E_POINTER;
    *collection_out = NULL;
    DWORD process_id = GetCurrentProcessId();
    if (!process_id) process_id = 1;
    uint64_t generation = gdi_font_resource_current_generation();

    dwrite_system_collections_lock();
    for (DWRITE_SYSTEM_COLLECTION_CACHE *entry = dwrite_system_collections;
         entry; entry = entry->next) {
        if (entry->owner_pid == process_id &&
            (!check_updates || entry->generation == generation)) {
            *collection_out = entry->collection;
            dwrite_collection_AddRef(*collection_out);
            dwrite_system_collections_unlock();
            return 0;
        }
    }
    dwrite_system_collections_unlock();

    DWRITE_COLLECTION_STUB *built = NULL;
    LONG status = dwrite_build_system_font_collection(
        process_id, &built, &generation);
    if (status < 0) return status;
    DWRITE_SYSTEM_COLLECTION_CACHE *published =
        (DWRITE_SYSTEM_COLLECTION_CACHE *)kmalloc(sizeof(*published));
    if (!published) {
        dwrite_collection_Release(built);
        return DWRITE_E_OUTOFMEMORY;
    }
    published->owner_pid = process_id;
    published->generation = generation;
    published->collection = built;
    published->next = NULL;

    DWRITE_SYSTEM_COLLECTION_CACHE *stale = NULL;
    dwrite_system_collections_lock();
    DWRITE_SYSTEM_COLLECTION_CACHE **link = &dwrite_system_collections;
    while (*link) {
        DWRITE_SYSTEM_COLLECTION_CACHE *entry = *link;
        if (entry->owner_pid != process_id) {
            link = &entry->next;
            continue;
        }
        if ((!check_updates || entry->generation == generation) &&
            !stale) {
            *collection_out = entry->collection;
            dwrite_collection_AddRef(*collection_out);
            dwrite_system_collections_unlock();
            dwrite_collection_Release(built);
            kfree(published);
            return 0;
        }
        *link = entry->next;
        entry->next = stale;
        stale = entry;
    }
    published->next = dwrite_system_collections;
    dwrite_system_collections = published;
    *collection_out = built;
    dwrite_collection_AddRef(built); /* caller ownership */
    dwrite_system_collections_unlock();

    while (stale) {
        DWRITE_SYSTEM_COLLECTION_CACHE *next = stale->next;
        dwrite_collection_Release(stale->collection);
        kfree(stale);
        stale = next;
    }
    return 0;
}

static void dwrite_release_system_collection_cache(DWORD process_id)
{
    DWRITE_SYSTEM_COLLECTION_CACHE *retired = NULL;
    dwrite_system_collections_lock();
    DWRITE_SYSTEM_COLLECTION_CACHE **link = &dwrite_system_collections;
    while (*link) {
        DWRITE_SYSTEM_COLLECTION_CACHE *entry = *link;
        if (entry->owner_pid != process_id) {
            link = &entry->next;
            continue;
        }
        *link = entry->next;
        entry->next = retired;
        retired = entry;
    }
    dwrite_system_collections_unlock();
    while (retired) {
        DWRITE_SYSTEM_COLLECTION_CACHE *next = retired->next;
        dwrite_collection_Release(retired->collection);
        kfree(retired);
        retired = next;
    }
}

static LONG WINAPI dwrite_factory_GetSystemFontCollection(PVOID self,
                                                          PVOID *collection,
                                                          BOOL check_updates)
{
    (void)self;
    if (!collection) return DWRITE_E_POINTER;
    *collection = NULL;
    LONG status = dwrite_acquire_system_font_collection(
        check_updates, (DWRITE_COLLECTION_STUB **)collection);
    if (status < 0) return status;
    dwrite_trace("[DWRITE] factory.GetSystemFontCollection\n");
    return 0;
}

static LONG WINAPI dwrite_factory_GetSystemFontFallback(PVOID self,
                                                         PVOID *fallback)
{
    (void)self;
    if (!fallback) return DWRITE_E_POINTER;
    *fallback = &dwrite_fallback;
    dwrite_trace("[DWRITE] factory.GetSystemFontFallback\n");
    return 0;
}

static LONG WINAPI DWriteCreateFactory_stub(UINT factory_type, LPCGUID iid,
                                             PVOID *factory)
{
    (void)factory_type;
    if (!iid || !factory) return DWRITE_E_INVALIDARG;
    (void)gdi32_shim_init();
    if (!dwrite_guid_equal(iid, &dwrite_iid_factory) &&
        !dwrite_guid_equal(iid, &dwrite_iid_iunknown)) {
        *factory = NULL;
        return DWRITE_E_NOINTERFACE;
    }
    *factory = &dwrite_factory;
    dwrite_trace("[DWRITE] DWriteCreateFactory\n");
    return 0;
}

/* ── Export table ──────────────────────────────────────────────── */

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
} DWRITE_TEST_UNKNOWN;

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    UINT reads;
    BYTE data[12];
} DWRITE_TEST_STREAM;

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    UINT create_calls;
    DWRITE_TEST_STREAM *stream;
} DWRITE_TEST_FILE_LOADER;

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    UINT position;
    PVOID factory;
    DWRITE_TEST_FILE_LOADER *file_loader;
} DWRITE_TEST_ENUMERATOR;

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    UINT create_calls;
    DWRITE_TEST_ENUMERATOR *enumerator;
} DWRITE_TEST_COLLECTION_LOADER;

static LONG WINAPI dwrite_test_QueryInterface(PVOID self, LPCGUID iid,
                                               PVOID *object)
{
    (void)iid;
    if (!object) return DWRITE_E_POINTER;
    *object = self;
    dwrite_object_addref(self);
    return 0;
}

static ULONG WINAPI dwrite_test_AddRef(PVOID self)
{
    DWRITE_TEST_UNKNOWN *object = (DWRITE_TEST_UNKNOWN *)self;
    return object
        ? __atomic_add_fetch(&object->refs, 1, __ATOMIC_RELAXED) : 0;
}

static ULONG WINAPI dwrite_test_Release(PVOID self)
{
    DWRITE_TEST_UNKNOWN *object = (DWRITE_TEST_UNKNOWN *)self;
    return object
        ? __atomic_sub_fetch(&object->refs, 1, __ATOMIC_RELAXED) : 0;
}

static LONG WINAPI dwrite_test_stream_ReadFileFragment(
    PVOID self, const void **fragment, uint64_t offset, uint64_t size,
    PVOID *context)
{
    DWRITE_TEST_STREAM *stream = (DWRITE_TEST_STREAM *)self;
    if (!stream || !fragment || !context || offset > sizeof(stream->data) ||
        size > sizeof(stream->data) - offset)
        return DWRITE_E_FILEFORMAT;
    *fragment = stream->data + offset;
    *context = NULL;
    stream->reads++;
    return 0;
}

static void WINAPI dwrite_test_stream_ReleaseFileFragment(PVOID self,
                                                           PVOID context)
{
    (void)self;
    (void)context;
}

static LONG WINAPI dwrite_test_stream_GetFileSize(PVOID self, uint64_t *size)
{
    if (!self || !size) return DWRITE_E_POINTER;
    *size = sizeof(((DWRITE_TEST_STREAM *)self)->data);
    return 0;
}

static LONG WINAPI dwrite_test_stream_GetLastWriteTime(PVOID self,
                                                        uint64_t *time)
{
    (void)self;
    if (!time) return DWRITE_E_POINTER;
    *time = 0;
    return 0;
}

static LONG WINAPI dwrite_test_file_loader_CreateStream(
    PVOID self, const void *key, UINT key_size, PVOID *stream)
{
    DWRITE_TEST_FILE_LOADER *loader = (DWRITE_TEST_FILE_LOADER *)self;
    if (!loader || !key || !key_size || !stream) return DWRITE_E_INVALIDARG;
    loader->create_calls++;
    *stream = loader->stream;
    dwrite_object_addref(*stream);
    return 0;
}

static LONG WINAPI dwrite_test_enumerator_MoveNext(PVOID self,
                                                    BOOL *has_file)
{
    DWRITE_TEST_ENUMERATOR *enumerator = (DWRITE_TEST_ENUMERATOR *)self;
    if (!enumerator || !has_file) return DWRITE_E_POINTER;
    *has_file = enumerator->position++ == 0;
    return 0;
}

static LONG WINAPI dwrite_test_enumerator_GetCurrentFile(PVOID self,
                                                          PVOID *font_file)
{
    DWRITE_TEST_ENUMERATOR *enumerator = (DWRITE_TEST_ENUMERATOR *)self;
    static const UINT file_key = 0x4F534954;
    if (!enumerator || !font_file || enumerator->position != 1)
        return DWRITE_E_INVALIDARG;
    PVOID *factory_vtbl = *(PVOID **)enumerator->factory;
    typedef LONG (WINAPI *CREATE_FILE_FN)(PVOID, const void *, UINT, PVOID,
                                          PVOID *);
    return ((CREATE_FILE_FN)factory_vtbl[8])(
        enumerator->factory, &file_key, sizeof(file_key),
        enumerator->file_loader, font_file);
}

static LONG WINAPI dwrite_test_collection_loader_CreateEnumerator(
    PVOID self, PVOID factory, const void *key, UINT key_size,
    PVOID *enumerator_out)
{
    DWRITE_TEST_COLLECTION_LOADER *loader =
        (DWRITE_TEST_COLLECTION_LOADER *)self;
    if (!loader || !factory || (key_size && !key) || !enumerator_out)
        return DWRITE_E_INVALIDARG;
    loader->create_calls++;
    loader->enumerator->position = 0;
    loader->enumerator->factory = factory;
    *enumerator_out = loader->enumerator;
    dwrite_object_addref(*enumerator_out);
    return 0;
}

static PVOID dwrite_test_stream_vtbl[] = {
    (PVOID)dwrite_test_QueryInterface,
    (PVOID)dwrite_test_AddRef,
    (PVOID)dwrite_test_Release,
    (PVOID)dwrite_test_stream_ReadFileFragment,
    (PVOID)dwrite_test_stream_ReleaseFileFragment,
    (PVOID)dwrite_test_stream_GetFileSize,
    (PVOID)dwrite_test_stream_GetLastWriteTime
};
static PVOID dwrite_test_file_loader_vtbl[] = {
    (PVOID)dwrite_test_QueryInterface,
    (PVOID)dwrite_test_AddRef,
    (PVOID)dwrite_test_Release,
    (PVOID)dwrite_test_file_loader_CreateStream
};
static PVOID dwrite_test_enumerator_vtbl[] = {
    (PVOID)dwrite_test_QueryInterface,
    (PVOID)dwrite_test_AddRef,
    (PVOID)dwrite_test_Release,
    (PVOID)dwrite_test_enumerator_MoveNext,
    (PVOID)dwrite_test_enumerator_GetCurrentFile
};
static PVOID dwrite_test_collection_loader_vtbl[] = {
    (PVOID)dwrite_test_QueryInterface,
    (PVOID)dwrite_test_AddRef,
    (PVOID)dwrite_test_Release,
    (PVOID)dwrite_test_collection_loader_CreateEnumerator
};

static void dwrite_test_check(int condition, const char *name,
                              int *checks, int *failures)
{
    (*checks)++;
    if (condition) return;
    (*failures)++;
    serial_puts("[DWRITE-TEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

int gdi32_dwrite_selftest(void)
{
    int checks = 0;
    int failures = 0;
    DWRITE_TEST_STREAM stream = {
        dwrite_test_stream_vtbl, 1, 0,
        { 0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0 }
    };
    DWRITE_TEST_FILE_LOADER file_loader = {
        dwrite_test_file_loader_vtbl, 1, 0, &stream
    };
    DWRITE_TEST_ENUMERATOR enumerator = {
        dwrite_test_enumerator_vtbl, 1, 0, NULL, &file_loader
    };
    DWRITE_TEST_COLLECTION_LOADER collection_loader = {
        dwrite_test_collection_loader_vtbl, 1, 0, &enumerator
    };
    PVOID *factory_vtbl;
    PVOID collection = NULL;
    static const BYTE cmap_format4[] = {
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0c,
        0x00, 0x04, 0x00, 0x20, 0x00, 0x00, 0x00, 0x04,
        0x00, 0x04, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x41, 0xff, 0xff,
        0x00, 0x00,
        0x00, 0x41, 0xff, 0xff,
        0xff, 0xc6, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00
    };
    static const BYTE gsub_extension_single[] = {
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a,
        0x00, 0x01, 0x00, 0x04,
        0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08,
        0x00, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x64,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x07
    };

    serial_puts("[DWRITE-TEST] starting custom-font pipeline test\n");
    gdi32_shim_init();
    factory_vtbl = dwrite_factory_vtbl;

    DWORD memory_faces = 0;
    HANDLE memory_font = AddFontMemResourceEx_k32(
        stream.data, sizeof(stream.data), NULL, &memory_faces);
    dwrite_test_check(memory_font != NULL && memory_faces == 1,
                      "memory font registration validates and reports faces",
                      &checks, &failures);
    dwrite_test_check(RemoveFontMemResourceEx_k32(memory_font),
                      "memory font handle unregisters its resource",
                      &checks, &failures);
    dwrite_test_check(!RemoveFontMemResourceEx_k32(memory_font) &&
                      GetLastError() == 6,
                      "stale memory font handles are rejected",
                      &checks, &failures);

    BYTE invalid_font[12] = {0};
    memory_faces = 9;
    memory_font = AddFontMemResourceEx_k32(
        invalid_font, sizeof(invalid_font), NULL, &memory_faces);
    dwrite_test_check(!memory_font && memory_faces == 0 &&
                      GetLastError() == 11,
                      "invalid memory fonts fail without publishing a handle",
                      &checks, &failures);
    dwrite_test_check(
        AddFontResourceA_k32("C:\\__ositok_missing_font__.ttf") == 0 &&
            GetLastError() == 2,
        "missing path fonts report file-not-found instead of fake success",
        &checks, &failures);

    DWRITE_COLLECTION_STUB *system_first = NULL;
    DWRITE_COLLECTION_STUB *system_second = NULL;
    LONG system_status = dwrite_acquire_system_font_collection(
        FALSE, &system_first);
    dwrite_test_check(system_status == 0 && system_first != NULL,
                      "system collection publishes an immutable snapshot",
                      &checks, &failures);
    system_status = dwrite_acquire_system_font_collection(
        TRUE, &system_second);
    dwrite_test_check(system_status == 0 && system_second == system_first,
                      "unchanged system collection is cached per process",
                      &checks, &failures);
    dwrite_object_release(system_second);
    dwrite_object_release(system_first);
    DWORD system_owner = GetCurrentProcessId();
    dwrite_release_system_collection_cache(system_owner ? system_owner : 1);

    dwrite_test_check(
        dwrite_cmap_lookup(cmap_format4, sizeof(cmap_format4), 'A') == 7,
        "format 4 cmap maps a codepoint to its glyph",
        &checks, &failures);
    dwrite_test_check(
        dwrite_cmap_lookup(cmap_format4, sizeof(cmap_format4), 'B') == 0,
        "format 4 cmap reports a missing codepoint",
        &checks, &failures);
    dwrite_test_check(
        dwrite_cmap_glyph_to_ascii(
            cmap_format4, sizeof(cmap_format4), 7) == 'A',
        "cmap reverse lookup recovers the raster character",
        &checks, &failures);
    dwrite_test_check(
        dwrite_gsub_reverse_single(
            gsub_extension_single, sizeof(gsub_extension_single), 100) == 7,
        "GSUB extension reverses a single substitution",
        &checks, &failures);
    dwrite_test_check(
        dwrite_gsub_reverse_single(
            gsub_extension_single, sizeof(gsub_extension_single), 101) == 0,
        "GSUB reverse lookup reports an unmapped glyph",
        &checks, &failures);

    typedef LONG (WINAPI *LOADER_FN)(PVOID, PVOID);
    typedef LONG (WINAPI *CREATE_COLLECTION_FN)(PVOID, PVOID, const void *,
                                                 UINT, PVOID *);
    LONG status = ((LOADER_FN)factory_vtbl[5])(
        &dwrite_factory, &collection_loader);
    dwrite_test_check(status == 0 && collection_loader.refs == 2,
                      "register collection loader retains one reference",
                      &checks, &failures);
    status = ((LOADER_FN)factory_vtbl[5])(
        &dwrite_factory, &collection_loader);
    dwrite_test_check(status == DWRITE_E_ALREADYREGISTERED &&
                      collection_loader.refs == 2,
                      "duplicate collection registration is rejected",
                      &checks, &failures);

    status = ((LOADER_FN)factory_vtbl[13])(&dwrite_factory, &file_loader);
    dwrite_test_check(status == 0 && file_loader.refs == 2,
                      "register file loader retains one reference",
                      &checks, &failures);
    status = ((LOADER_FN)factory_vtbl[13])(&dwrite_factory, &file_loader);
    dwrite_test_check(status == DWRITE_E_ALREADYREGISTERED &&
                      file_loader.refs == 2,
                      "duplicate file registration is rejected",
                      &checks, &failures);

    status = ((CREATE_COLLECTION_FN)factory_vtbl[4])(
        &dwrite_factory, &collection_loader, NULL, 0, &collection);
    dwrite_test_check(status == 0 && collection != NULL,
                      "zero-length custom collection key is accepted",
                      &checks, &failures);
    dwrite_test_check(collection_loader.create_calls == 1 &&
                      file_loader.create_calls == 1 && stream.reads == 1,
                      "collection enumerates and analyzes the font stream",
                      &checks, &failures);
    dwrite_test_check(enumerator.refs == 1 && stream.refs == 1,
                      "temporary enumerator and stream references are released",
                      &checks, &failures);
    dwrite_test_check(file_loader.refs == 3,
                      "font file retains its registered loader",
                      &checks, &failures);

    if (collection) {
        DWRITE_COLLECTION_STUB *created =
            (DWRITE_COLLECTION_STUB *)collection;
        typedef UINT (WINAPI *GET_COUNT_FN)(PVOID);
        UINT families = ((GET_COUNT_FN)created->lpVtbl[3])(created);
        dwrite_test_check(families == 1 && created->font_file_count == 1,
                          "collection publishes one validated family",
                          &checks, &failures);

        PVOID family = NULL;
        typedef LONG (WINAPI *GET_FAMILY_FN)(PVOID, UINT, PVOID *);
        status = ((GET_FAMILY_FN)created->lpVtbl[4])(
            created, 0, &family);
        dwrite_test_check(status == 0 && family == &created->family,
                          "family belongs to the custom collection",
                          &checks, &failures);

        PVOID owning_collection = NULL;
        typedef LONG (WINAPI *GET_COLLECTION_FN)(PVOID, PVOID *);
        if (family)
            status = ((GET_COLLECTION_FN)(*(PVOID **)family)[3])(
                family, &owning_collection);
        else
            status = DWRITE_E_INVALIDARG;
        dwrite_test_check(status == 0 && owning_collection == collection,
                          "family returns its owning collection",
                          &checks, &failures);
        dwrite_object_release(owning_collection);

        PVOID face = NULL;
        typedef LONG (WINAPI *CREATE_FACE_FN)(PVOID, UINT, UINT,
                                               PVOID const *, UINT, UINT,
                                               PVOID *);
        status = ((CREATE_FACE_FN)factory_vtbl[9])(
            &dwrite_factory, DWRITE_FONT_FACE_TYPE_TRUETYPE,
            created->font_file_count, (PVOID const *)created->font_files,
            0, 0, &face);
        dwrite_test_check(status == 0 && face &&
                          ((DWRITE_FONT_FACE_STUB *)face)->lpVtbl ==
                              dwrite_face_vtbl,
                          "font face is created from validated files",
                          &checks, &failures);

        UINT face_file_count = 0;
        typedef LONG (WINAPI *GET_FILES_FN)(PVOID, UINT *, PVOID *);
        if (face)
            status = ((GET_FILES_FN)(*(PVOID **)face)[4])(
                face, &face_file_count, NULL);
        else
            status = DWRITE_E_INVALIDARG;
        dwrite_test_check(status == 0 && face_file_count == 1,
                          "font face reports its retained files",
                          &checks, &failures);

        PVOID returned_file = NULL;
        UINT returned_count = face_file_count;
        if (face)
            status = ((GET_FILES_FN)(*(PVOID **)face)[4])(
                face, &returned_count, &returned_file);
        dwrite_test_check(status == 0 && returned_count == 1 &&
                          returned_file == created->font_files[0],
                          "font face returns and retains its source file",
                          &checks, &failures);
        dwrite_object_release(returned_file);

        UINT short_count = 0;
        PVOID short_file = NULL;
        if (face)
            status = ((GET_FILES_FN)(*(PVOID **)face)[4])(
                face, &short_count, &short_file);
        dwrite_test_check(status == DWRITE_E_INSUFFICIENT_BUFFER &&
                          short_count == 1 && !short_file,
                          "font face rejects an undersized file array",
                          &checks, &failures);

        PVOID font = NULL;
        typedef LONG (WINAPI *GET_FONT_FN)(PVOID, UINT, PVOID *);
        if (family)
            status = ((GET_FONT_FN)(*(PVOID **)family)[5])(
                family, 0, &font);
        dwrite_test_check(status == 0 && font == &created->family.font,
                          "family returns its collection-owned font",
                          &checks, &failures);
        PVOID font_face = NULL;
        if (font)
            status = ((LONG (WINAPI *)(PVOID, PVOID *))
                (*(PVOID **)font)[13])(font, &font_face);
        dwrite_test_check(status == 0 && font_face != NULL,
                          "font creates a face from its collection files",
                          &checks, &failures);
        dwrite_object_release(font_face);
        dwrite_object_release(font);

        dwrite_object_release(face);
        dwrite_object_release(family);

        dwrite_object_release(collection);
        collection = NULL;
    }
    dwrite_test_check(file_loader.refs == 2,
                      "collection release drops retained font files",
                      &checks, &failures);

    status = ((LOADER_FN)factory_vtbl[14])(&dwrite_factory, &file_loader);
    dwrite_test_check(status == 0 && file_loader.refs == 1,
                      "file loader unregister releases factory reference",
                      &checks, &failures);
    status = ((LOADER_FN)factory_vtbl[14])(&dwrite_factory, &file_loader);
    dwrite_test_check(status == DWRITE_E_INVALIDARG,
                      "unregistered file loader cannot be removed twice",
                      &checks, &failures);
    status = ((LOADER_FN)factory_vtbl[6])(
        &dwrite_factory, &collection_loader);
    dwrite_test_check(status == 0 && collection_loader.refs == 1,
                      "collection loader unregister releases factory reference",
                      &checks, &failures);
    status = ((CREATE_COLLECTION_FN)factory_vtbl[4])(
        &dwrite_factory, &collection_loader, NULL, 0, &collection);
    dwrite_test_check(status == DWRITE_E_INVALIDARG && !collection,
                      "unregistered loader cannot create collections",
                      &checks, &failures);

    serial_puts("[DWRITE-TEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static UINT WINAPI SetDIBColorTable(HDC hdc, UINT start, UINT count,
                                    const DWORD *colors)
{
    GDI_DC *dc = dc_from_handle(hdc);
    GDI_BITMAP *bmp = dc ? bmp_from_handle(dc->prev_bitmap) : NULL;
    if (!bmp || !bmp->dib_section || bmp->bpp > 8 || !colors ||
        start >= bmp->format.color_count)
        return 0;
    UINT available = bmp->format.color_count - start;
    if (count > available) count = available;
    for (UINT i = 0; i < count; i++)
        bmp->format.colors[start + i] = colors[i] & 0x00FFFFFFU;
    return count;
}

static NTSTATUS WINAPI D3DKMTCreateDCFromMemory(PVOID description)
{
    /* No WDDM memory-DC provider yet. Wine can fall back to CreateDIBSection;
     * do not expose a fabricated kernel graphics handle. */
    (void)description;
    return (NTSTATUS)0xC0000002; /* STATUS_NOT_IMPLEMENTED */
}

static NTSTATUS WINAPI D3DKMTDestroyDCFromMemory(const void *description)
{
    (void)description;
    return (NTSTATUS)0xC0000002;
}

static const SHIM_EXPORT gdi32_exports[] = {
    { "D3DKMTEnumAdapters", (PVOID)D3DKMTEnumAdapters, 1, CC_STDCALL },
    { "D3DKMTEnumAdapters2", (PVOID)D3DKMTEnumAdapters2, 1, CC_STDCALL },
    { "D3DKMTOpenAdapterFromLuid", (PVOID)D3DKMTOpenAdapterFromLuid, 1, CC_STDCALL },
    { "D3DKMTOpenAdapterFromDeviceName", (PVOID)D3DKMTOpenAdapterFromDeviceName, 1, CC_STDCALL },
    { "D3DKMTOpenAdapterFromGdiDisplayName", (PVOID)D3DKMTOpenAdapterFromGdiDisplayName, 1, CC_STDCALL },
    { "D3DKMTOpenAdapterFromHdc", (PVOID)D3DKMTOpenAdapterFromHdc, 1, CC_STDCALL },
    { "D3DKMTCloseAdapter", (PVOID)D3DKMTCloseAdapter, 1, CC_STDCALL },
    { "D3DKMTQueryAdapterInfo", (PVOID)D3DKMTQueryAdapterInfo, 1, CC_STDCALL },
    { "D3DKMTGetDisplayModeList", (PVOID)D3DKMTGetDisplayModeList, 1, CC_STDCALL },
    { "D3DKMTGetScanLine", (PVOID)D3DKMTGetScanLine, 1, CC_STDCALL },
    { "D3DKMTCreateDevice", (PVOID)D3DKMTCreateDevice, 1, CC_STDCALL },
    { "D3DKMTDestroyDevice", (PVOID)D3DKMTDestroyDevice, 1, CC_STDCALL },
    { "D3DKMTCreateAllocation", (PVOID)D3DKMTCreateAllocation, 1, CC_STDCALL },
    { "D3DKMTLock", (PVOID)D3DKMTLock, 1, CC_STDCALL },
    { "D3DKMTLock2", (PVOID)D3DKMTLock2, 1, CC_STDCALL },
    { "D3DKMTUnlock2", (PVOID)D3DKMTUnlock2, 1, CC_STDCALL },
    { "D3DKMTCreateContext", (PVOID)D3DKMTCreateContext, 1, CC_STDCALL },
    { "D3DKMTRender", (PVOID)D3DKMTRender, 1, CC_STDCALL },
    { "D3DKMTCreateSynchronizationObject", (PVOID)D3DKMTCreateSynchronizationObject, 1, CC_STDCALL },
    { "D3DKMTCreateSynchronizationObject2", (PVOID)D3DKMTCreateSynchronizationObject2, 1, CC_STDCALL },
    { "D3DKMTCreatePagingQueue", (PVOID)D3DKMTCreatePagingQueue, 1, CC_STDCALL },
    { "D3DKMTMakeResident", (PVOID)D3DKMTMakeResident, 1, CC_STDCALL },
    { "D3DKMTEvict", (PVOID)D3DKMTEvict, 1, CC_STDCALL },
    { "D3DKMTOfferAllocations", (PVOID)D3DKMTOfferAllocations, 1, CC_STDCALL },
    { "D3DKMTReclaimAllocations", (PVOID)D3DKMTReclaimAllocations, 1, CC_STDCALL },
    { "D3DKMTReclaimAllocations2", (PVOID)D3DKMTReclaimAllocations2, 1, CC_STDCALL },
    { "D3DKMTDestroyPagingQueue", (PVOID)D3DKMTDestroyPagingQueue, 1, CC_STDCALL },
    { "D3DKMTDestroySynchronizationObject", (PVOID)D3DKMTDestroySynchronizationObject, 1, CC_STDCALL },
    { "D3DKMTWaitForSynchronizationObject", (PVOID)D3DKMTWaitForSynchronizationObject, 1, CC_STDCALL },
    { "D3DKMTWaitForSynchronizationObject2", (PVOID)D3DKMTWaitForSynchronizationObject2, 1, CC_STDCALL },
    { "D3DKMTSignalSynchronizationObject", (PVOID)D3DKMTSignalSynchronizationObject, 1, CC_STDCALL },
    { "D3DKMTSignalSynchronizationObject2", (PVOID)D3DKMTSignalSynchronizationObject2, 1, CC_STDCALL },
    { "D3DKMTSignalSynchronizationObjectFromCpu", (PVOID)D3DKMTSignalSynchronizationObjectFromCpu, 1, CC_STDCALL },
    { "D3DKMTWaitForSynchronizationObjectFromCpu", (PVOID)D3DKMTWaitForSynchronizationObjectFromCpu, 1, CC_STDCALL },
    { "D3DKMTWaitForSynchronizationObjectFromGpu", (PVOID)D3DKMTWaitForSynchronizationObjectFromGpu, 1, CC_STDCALL },
    { "D3DKMTSignalSynchronizationObjectFromGpu", (PVOID)D3DKMTSignalSynchronizationObjectFromGpu, 1, CC_STDCALL },
    { "D3DKMTSignalSynchronizationObjectFromGpu2", (PVOID)D3DKMTSignalSynchronizationObjectFromGpu2, 1, CC_STDCALL },
    { "D3DKMTDestroyContext", (PVOID)D3DKMTDestroyContext, 1, CC_STDCALL },
    { "D3DKMTUnlock", (PVOID)D3DKMTUnlock, 1, CC_STDCALL },
    { "D3DKMTCreateAllocation2", (PVOID)D3DKMTCreateAllocation2, 1, CC_STDCALL },
    { "D3DKMTDestroyAllocation", (PVOID)D3DKMTDestroyAllocation, 1, CC_STDCALL },
    { "D3DKMTDestroyAllocation2", (PVOID)D3DKMTDestroyAllocation2, 1, CC_STDCALL },
    { "D3DKMTQueryAllocationResidency", (PVOID)D3DKMTQueryAllocationResidency, 1, CC_STDCALL },
    { "D3DKMTSetAllocationPriority", (PVOID)D3DKMTSetAllocationPriority, 1, CC_STDCALL },
    { "D3DKMTGetContextSchedulingPriority", (PVOID)D3DKMTGetContextSchedulingPriority, 1, CC_STDCALL },
    { "D3DKMTSetContextSchedulingPriority", (PVOID)D3DKMTSetContextSchedulingPriority, 1, CC_STDCALL },
    { "D3DKMTGetDeviceState", (PVOID)D3DKMTGetDeviceState, 1, CC_STDCALL },
    { "D3DKMTMarkDeviceAsError", (PVOID)D3DKMTMarkDeviceAsError, 1, CC_STDCALL },
    { "SetDIBColorTable", (PVOID)SetDIBColorTable, 4, CC_STDCALL },
    { "D3DKMTCreateDCFromMemory", (PVOID)D3DKMTCreateDCFromMemory, 1, CC_STDCALL },
    { "D3DKMTDestroyDCFromMemory", (PVOID)D3DKMTDestroyDCFromMemory, 1, CC_STDCALL },
    { "GetDeviceCaps",       (PVOID)GetDeviceCaps, 2, CC_STDCALL },
    { "ExtEscape",           (PVOID)ExtEscape_k32, 6, CC_STDCALL },
    { "GetDeviceGammaRamp",  (PVOID)GetDeviceGammaRamp, 2, CC_STDCALL },
    { "SetDeviceGammaRamp",  (PVOID)SetDeviceGammaRamp, 2, CC_STDCALL },
    { "CreateDCA",           (PVOID)CreateDCA, 4, CC_STDCALL },
    { "CreateDCW",           (PVOID)CreateDCW_k32, 4, CC_STDCALL },
    { "CreateICW",           (PVOID)CreateDCW_k32, 4, CC_STDCALL },
    { "GetICMProfileW",      (PVOID)GetICMProfileW_stub, 3, CC_STDCALL },
    { "DeleteDC",            (PVOID)DeleteDC, 1, CC_STDCALL },
    { "ChoosePixelFormat",   (PVOID)ChoosePixelFormat, 2, CC_STDCALL },
    { "SetPixelFormat",      (PVOID)SetPixelFormat, 3, CC_STDCALL },
    { "GetPixelFormat",      (PVOID)GetPixelFormat, 1, CC_STDCALL },
    { "DescribePixelFormat", (PVOID)DescribePixelFormat, 4, CC_STDCALL },
    { "SwapBuffers",         (PVOID)SwapBuffers, 1, CC_STDCALL },
    { "SelectObject",        (PVOID)SelectObject, 2, CC_STDCALL },
    { "GetCurrentObject",    (PVOID)GetCurrentObject, 2, CC_STDCALL },
    { "SaveDC",              (PVOID)SaveDC, 1, CC_STDCALL },
    { "RestoreDC",           (PVOID)RestoreDC, 2, CC_STDCALL },
    { "CreatePalette",       (PVOID)CreatePalette, 1, CC_STDCALL },
    { "SelectPalette",       (PVOID)SelectPalette, 3, CC_STDCALL },
    { "GetPaletteEntries",   (PVOID)GetPaletteEntries, 4, CC_STDCALL },
    { "SetPaletteEntries",   (PVOID)SetPaletteEntries, 4, CC_STDCALL },
    { "RealizePalette",      (PVOID)RealizePalette, 1, CC_STDCALL },
    { "UpdateColors",        (PVOID)UpdateColors, 1, CC_STDCALL },
    { "StartDocA",           (PVOID)StartDocA, 2, CC_STDCALL },
    { "StartPage",           (PVOID)gdi_printer_operation, 1, CC_STDCALL },
    { "EndPage",             (PVOID)gdi_printer_operation, 1, CC_STDCALL },
    { "EndDoc",              (PVOID)gdi_printer_operation, 1, CC_STDCALL },
    { "GdiFlush",            (PVOID)GdiFlush_k32, 0, CC_STDCALL },
    { "AddFontMemResourceEx", (PVOID)AddFontMemResourceEx_k32, 4, CC_STDCALL },
    { "RemoveFontMemResourceEx", (PVOID)RemoveFontMemResourceEx_k32, 1, CC_STDCALL },
    { "AddFontResourceA",    (PVOID)AddFontResourceA_k32, 1, CC_STDCALL },
    { "AddFontResourceW",    (PVOID)AddFontResourceW_k32, 1, CC_STDCALL },
    { "AddFontResourceExA",  (PVOID)AddFontResourceExA_k32, 3, CC_STDCALL },
    { "RemoveFontResourceA", (PVOID)RemoveFontResourceA_k32, 1, CC_STDCALL },
    { "RemoveFontResourceW", (PVOID)RemoveFontResourceW_k32, 1, CC_STDCALL },
    { "RemoveFontResourceExA", (PVOID)RemoveFontResourceExA_k32, 3, CC_STDCALL },
    { "AddFontResourceExW",  (PVOID)AddFontResourceExW_k32, 3, CC_STDCALL },
    { "RemoveFontResourceExW", (PVOID)RemoveFontResourceExW_k32, 3, CC_STDCALL },
    { "CreateRectRgn",       (PVOID)CreateRectRgn, 4, CC_STDCALL },
    { "CreateRectRgnIndirect", (PVOID)CreateRectRgnIndirect, 1, CC_STDCALL },
    { "CreatePolygonRgn",    (PVOID)CreatePolygonRgn, 3, CC_STDCALL },
    { "CreatePolyPolygonRgn", (PVOID)CreatePolyPolygonRgn, 4, CC_STDCALL },
    { "SetRectRgn",          (PVOID)SetRectRgn, 5, CC_STDCALL },
    { "CombineRgn",          (PVOID)CombineRgn, 4, CC_STDCALL },
    { "EqualRgn",            (PVOID)EqualRgn, 2, CC_STDCALL },
    { "PtInRegion",          (PVOID)PtInRegion, 3, CC_STDCALL },
    { "RectInRegion",        (PVOID)RectInRegion, 2, CC_STDCALL },
    { "GetRgnBox",           (PVOID)GetRgnBox, 2, CC_STDCALL },
    { "GetRegionData",       (PVOID)GetRegionData, 3, CC_STDCALL },
    { "GetRandomRgn",        (PVOID)GetRandomRgn, 3, CC_STDCALL },
    { "OffsetRgn",           (PVOID)OffsetRgn, 3, CC_STDCALL },
    { "SelectClipRgn",       (PVOID)SelectClipRgn, 2, CC_STDCALL },
    { "SetViewportOrgEx",    (PVOID)SetViewportOrgEx_k32, 4, CC_STDCALL },
    { "SetWindowOrgEx",      (PVOID)SetWindowOrgEx, 4, CC_STDCALL },
    { "SetWindowExtEx",      (PVOID)SetWindowExtEx, 4, CC_STDCALL },
    { "SetViewportExtEx",    (PVOID)SetViewportExtEx, 4, CC_STDCALL },
    { "GetWindowOrgEx",      (PVOID)GetWindowOrgEx, 2, CC_STDCALL },
    { "GetViewportOrgEx",    (PVOID)GetViewportOrgEx, 2, CC_STDCALL },
    { "GetWindowExtEx",      (PVOID)GetWindowExtEx, 2, CC_STDCALL },
    { "GetViewportExtEx",    (PVOID)GetViewportExtEx, 2, CC_STDCALL },
    { "LPtoDP",              (PVOID)LPtoDP, 3, CC_STDCALL },
    { "DPtoLP",              (PVOID)DPtoLP, 3, CC_STDCALL },
    { "GetDCOrgEx",          (PVOID)GetDCOrgEx, 2, CC_STDCALL },
    { "SetBrushOrgEx",       (PVOID)SetBrushOrgEx_k32, 4, CC_STDCALL },
    { "SetDCBrushColor",     (PVOID)SetDCBrushColor_k32, 2, CC_STDCALL },
    { "SetTextAlign",        (PVOID)SetTextAlign, 2, CC_STDCALL },
    { "GetTextAlign",        (PVOID)GetTextAlign, 1, CC_STDCALL },
    { "DeleteObject",        (PVOID)DeleteObject, 1, CC_STDCALL },
    { "GetObjectA",          (PVOID)GetObjectA, 3, CC_STDCALL },
    { "GetObjectType",       (PVOID)GetObjectType_k32, 1, CC_STDCALL },
    /* GDI object creation */
    { "CreateCompatibleDC",       (PVOID)CreateCompatibleDC, 1, CC_STDCALL },
    { "CreateCompatibleBitmap",   (PVOID)CreateCompatibleBitmap, 3, CC_STDCALL },
    { "GetDIBits",                (PVOID)GetDIBits, 7, CC_STDCALL },
    { "CreateSolidBrush",         (PVOID)CreateSolidBrush, 1, CC_STDCALL },
    { "CreatePatternBrush",       (PVOID)CreatePatternBrush, 1, CC_STDCALL },
    { "CreatePen",                (PVOID)CreatePen, 3, CC_STDCALL },
    { "ExtCreatePen",             (PVOID)ExtCreatePen, 5, CC_STDCALL },
    { "CreateBitmap",             (PVOID)CreateBitmap, 5, CC_STDCALL },
    { "CreateDIBitmap",           (PVOID)CreateDIBitmap, 6, CC_STDCALL },
    { "GetStockObject",           (PVOID)GetStockObject, 1, CC_STDCALL },
    /* Drawing */
    { "BitBlt",                   (PVOID)BitBlt, 9, CC_STDCALL },
    { "StretchBlt",               (PVOID)StretchBlt_k32, 11, CC_STDCALL },
    { "StretchDIBits",            (PVOID)StretchDIBits_k32, 13, CC_STDCALL },
    { "SetDIBitsToDevice",        (PVOID)SetDIBitsToDevice_k32, 12, CC_STDCALL },
    { "GradientFill",             (PVOID)GradientFill_k32, 6, CC_STDCALL },
    { "TransparentBlt",           (PVOID)TransparentBlt_k32, 11, CC_STDCALL },
    { "AlphaBlend",               (PVOID)AlphaBlend_k32, 11, CC_STDCALL },
    { "PatBlt",                   (PVOID)PatBlt, 6, CC_STDCALL },
    { "MoveToEx",                 (PVOID)MoveToEx, 4, CC_STDCALL },
    { "GetCurrentPositionEx",     (PVOID)GetCurrentPositionEx, 2, CC_STDCALL },
    { "LineTo",                   (PVOID)LineTo, 3, CC_STDCALL },
    { "Polyline",                 (PVOID)Polyline_k32, 3, CC_STDCALL },
    { "Polygon",                  (PVOID)Polygon, 3, CC_STDCALL },
    { "Arc",                      (PVOID)Arc, 9, CC_STDCALL },
    { "Pie",                      (PVOID)Pie, 9, CC_STDCALL },
    { "GetArcDirection",          (PVOID)GetArcDirection, 1, CC_STDCALL },
    { "SetArcDirection",          (PVOID)SetArcDirection, 2, CC_STDCALL },
    { "PolyPolygon",              (PVOID)PolyPolygon, 4, CC_STDCALL },
    { "GetPolyFillMode",           (PVOID)GetPolyFillMode, 1, CC_STDCALL },
    { "SetPolyFillMode",           (PVOID)SetPolyFillMode, 2, CC_STDCALL },
    { "GetROP2",                   (PVOID)GetROP2, 1, CC_STDCALL },
    { "SetROP2",                   (PVOID)SetROP2, 2, CC_STDCALL },
    { "GetDCPenColor",             (PVOID)GetDCPenColor, 1, CC_STDCALL },
    { "SetDCPenColor",             (PVOID)SetDCPenColor, 2, CC_STDCALL },
    /* Text */
    { "SetTextColor",             (PVOID)SetTextColor, 2, CC_STDCALL },
    { "GetTextColor",             (PVOID)GetTextColor, 1, CC_STDCALL },
    { "SetBkColor",               (PVOID)SetBkColor, 2, CC_STDCALL },
    { "GetBkColor",               (PVOID)GetBkColor, 1, CC_STDCALL },
    { "SetBkMode",                (PVOID)SetBkMode, 2, CC_STDCALL },
    { "TextOutW",                 (PVOID)TextOutW, 5, CC_STDCALL },
    { "TextOutA",                 (PVOID)TextOutA_k32, 5, CC_STDCALL },
    { "ExtTextOutA",              (PVOID)ExtTextOutA, 8, CC_STDCALL },
    { "ExtTextOutW",              (PVOID)ExtTextOutW, 8, CC_STDCALL },
    { "GetTextExtentPoint32A",    (PVOID)GetTextExtentPoint32A, 4, CC_STDCALL },
    { "GetTextExtentPoint32W",    (PVOID)GetTextExtentPoint32W, 4, CC_STDCALL },
    { "GetTextFaceW",             (PVOID)GetTextFaceW_k32, 3, CC_STDCALL },
    { "SetMapMode",               (PVOID)SetMapMode_k32, 2, CC_STDCALL },
    { "GetMapMode",               (PVOID)GetMapMode_k32, 1, CC_STDCALL },
    { "SetStretchBltMode",        (PVOID)SetStretchBltMode_k32, 2, CC_STDCALL },
    { "SetGraphicsMode",          (PVOID)SetGraphicsMode_k32, 2, CC_STDCALL },
    { "GetGraphicsMode",          (PVOID)GetGraphicsMode_k32, 1, CC_STDCALL },
    { "SetWorldTransform",        (PVOID)SetWorldTransform_k32, 2, CC_STDCALL },
    { "GetWorldTransform",        (PVOID)GetWorldTransform_k32, 2, CC_STDCALL },
    { "ModifyWorldTransform",     (PVOID)ModifyWorldTransform_k32, 3, CC_STDCALL },
    { "GetTextMetricsA",          (PVOID)GetTextMetricsA_stub, 2, CC_STDCALL },
    { "GetTextMetricsW",          (PVOID)GetTextMetricsW, 2, CC_STDCALL },
    { "EnumFontFamiliesExA",      (PVOID)EnumFontFamiliesExA_stub, 5, CC_STDCALL },
    { "EnumFontFamiliesExW",      (PVOID)EnumFontFamiliesExW_k32, 5, CC_STDCALL },
    { "EnumFontFamiliesW",        (PVOID)EnumFontFamiliesW, 4, CC_STDCALL },
    { "GetCharABCWidthsW",        (PVOID)GetCharABCWidthsW_stub, 4, CC_STDCALL },
    { "GetCharWidthW",            (PVOID)GetCharWidthW_k32, 4, CC_STDCALL },
    { "GetFontData",              (PVOID)GetFontData_k32, 5, CC_STDCALL },
    { "GetObjectW",               (PVOID)GetObjectW_k32, 3, CC_STDCALL },
    /* Additional stubs */
    { "CreateFontA",              (PVOID)CreateFontA_stub, 14, CC_STDCALL },
    { "CreateFontW",              (PVOID)CreateFontW_stub, 14, CC_STDCALL },
    { "CreateFontIndirectW",      (PVOID)CreateFontIndirectW_stub, 1, CC_STDCALL },
    { "CreateFontIndirectA",      (PVOID)CreateFontIndirectA_impl, 1, CC_STDCALL },
    { "GetGlyphOutlineA",         (PVOID)GetGlyphOutline_impl, 7, CC_STDCALL },
    { "GetGlyphOutlineW",         (PVOID)GetGlyphOutline_impl, 7, CC_STDCALL },
    { "GetOutlineTextMetricsA",   (PVOID)GetOutlineTextMetricsA_impl, 3, CC_STDCALL },
    { "GetCharacterPlacementA",   (PVOID)GetCharacterPlacement_impl, 6, CC_STDCALL },
    { "GetCharacterPlacementW",   (PVOID)GetCharacterPlacement_impl, 6, CC_STDCALL },
    { "GetFontLanguageInfo",      (PVOID)GetFontLanguageInfo_impl, 1, CC_STDCALL },
    { "TranslateCharsetInfo",     (PVOID)TranslateCharsetInfo_impl, 3, CC_STDCALL },
    { "CreateDIBSection",         (PVOID)CreateDIBSection_impl, 6, CC_STDCALL },
    { "GetPixel",                 (PVOID)GetPixel_k32, 3, CC_STDCALL },
    { "SetPixel",                 (PVOID)SetPixel, 4, CC_STDCALL },
    { "DWriteCreateFactory",      (PVOID)DWriteCreateFactory_stub, 3, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *gdi32_abi_table(int *count)
{
    *count = (int)(sizeof(gdi32_exports) / sizeof(gdi32_exports[0]));
    return (const WIN32_EXPORT *)gdi32_exports;
}

static int gdi_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID gdi32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;
    for (int i = 0; gdi32_exports[i].name; i++) {
        if (gdi_strcmp(func_name, gdi32_exports[i].name) == 0)
            return gdi32_exports[i].func;
    }
    return NULL;
}

PVOID gdi32_shim_init(void)
{
    dwrite_collection.family.collection = &dwrite_collection;
    dwrite_collection.family.font.family = &dwrite_collection.family;
    for (int i = 3; i < 40; i++)
        dwrite_factory_vtbl[i] = (PVOID)dwrite_factory_notimpl;
    dwrite_factory_vtbl[0] = (PVOID)dwrite_factory_QueryInterface;
    dwrite_factory_vtbl[1] = (PVOID)dwrite_factory_AddRef;
    dwrite_factory_vtbl[2] = (PVOID)dwrite_factory_Release;
    dwrite_factory_vtbl[3] = (PVOID)dwrite_factory_GetSystemFontCollection;
    dwrite_factory_vtbl[4] = (PVOID)dwrite_factory_CreateCustomFontCollection;
    dwrite_factory_vtbl[5] = (PVOID)dwrite_factory_RegisterFontCollectionLoader;
    dwrite_factory_vtbl[6] = (PVOID)dwrite_factory_UnregisterFontCollectionLoader;
    dwrite_factory_vtbl[7] = (PVOID)dwrite_factory_CreateFontFileReference;
    dwrite_factory_vtbl[8] = (PVOID)dwrite_factory_CreateCustomFontFileReference;
    dwrite_factory_vtbl[9] = (PVOID)dwrite_factory_CreateFontFace;
    dwrite_factory_vtbl[13] = (PVOID)dwrite_factory_RegisterFontFileLoader;
    dwrite_factory_vtbl[14] = (PVOID)dwrite_factory_UnregisterFontFileLoader;
    dwrite_factory_vtbl[22] = (PVOID)dwrite_factory_CreateNumberSubstitution;
    dwrite_factory_vtbl[23] = (PVOID)dwrite_factory_CreateGlyphRunAnalysis;
    dwrite_factory_vtbl[26] = (PVOID)dwrite_factory_GetSystemFontFallback;
    for (int i = 3; i < 7; i++)
        dwrite_collection_vtbl[i] = (PVOID)dwrite_factory_notimpl;
    dwrite_collection_vtbl[0] = (PVOID)dwrite_collection_QueryInterface;
    dwrite_collection_vtbl[1] = (PVOID)dwrite_collection_AddRef;
    dwrite_collection_vtbl[2] = (PVOID)dwrite_collection_Release;
    dwrite_collection_vtbl[3] = (PVOID)dwrite_collection_GetFontFamilyCount;
    dwrite_collection_vtbl[4] = (PVOID)dwrite_collection_GetFontFamily;
    dwrite_collection_vtbl[5] = (PVOID)dwrite_collection_FindFamilyName;
    dwrite_collection_vtbl[6] = (PVOID)dwrite_collection_GetFontFromFontFace;

    dwrite_family_vtbl[0] = (PVOID)dwrite_family_QueryInterface;
    dwrite_family_vtbl[1] = (PVOID)dwrite_family_AddRef;
    dwrite_family_vtbl[2] = (PVOID)dwrite_family_Release;
    dwrite_family_vtbl[3] = (PVOID)dwrite_family_GetFontCollection;
    dwrite_family_vtbl[4] = (PVOID)dwrite_family_GetFontCount;
    dwrite_family_vtbl[5] = (PVOID)dwrite_family_GetFont;
    dwrite_family_vtbl[6] = (PVOID)dwrite_family_GetFamilyNames;
    dwrite_family_vtbl[7] = (PVOID)dwrite_family_GetFirstMatchingFont;
    dwrite_family_vtbl[8] = (PVOID)dwrite_family_GetMatchingFonts;

    dwrite_font_vtbl[0] = (PVOID)dwrite_font_QueryInterface;
    dwrite_font_vtbl[1] = (PVOID)dwrite_font_AddRef;
    dwrite_font_vtbl[2] = (PVOID)dwrite_font_Release;
    dwrite_font_vtbl[3] = (PVOID)dwrite_font_GetFontFamily;
    dwrite_font_vtbl[4] = (PVOID)dwrite_font_GetWeight;
    dwrite_font_vtbl[5] = (PVOID)dwrite_font_GetStretch;
    dwrite_font_vtbl[6] = (PVOID)dwrite_font_GetStyle;
    dwrite_font_vtbl[7] = (PVOID)dwrite_font_IsSymbolFont;
    dwrite_font_vtbl[8] = (PVOID)dwrite_font_GetFaceNames;
    dwrite_font_vtbl[9] = (PVOID)dwrite_font_GetInformationalStrings;
    dwrite_font_vtbl[10] = (PVOID)dwrite_font_GetSimulations;
    dwrite_font_vtbl[11] = (PVOID)dwrite_font_GetMetrics;
    dwrite_font_vtbl[12] = (PVOID)dwrite_font_HasCharacter;
    dwrite_font_vtbl[13] = (PVOID)dwrite_font_CreateFontFace;

    dwrite_face_vtbl[0] = (PVOID)dwrite_face_QueryInterface;
    dwrite_face_vtbl[1] = (PVOID)dwrite_face_AddRef;
    dwrite_face_vtbl[2] = (PVOID)dwrite_face_Release;
    dwrite_face_vtbl[3] = (PVOID)dwrite_face_GetType;
    dwrite_face_vtbl[4] = (PVOID)dwrite_face_GetFiles;
    dwrite_face_vtbl[5] = (PVOID)dwrite_face_GetIndex;
    dwrite_face_vtbl[6] = (PVOID)dwrite_face_GetSimulations;
    dwrite_face_vtbl[7] = (PVOID)dwrite_face_IsSymbolFont;
    dwrite_face_vtbl[8] = (PVOID)dwrite_face_GetMetrics;
    dwrite_face_vtbl[9] = (PVOID)dwrite_face_GetGlyphCount;
    dwrite_face_vtbl[10] = (PVOID)dwrite_face_GetDesignGlyphMetrics;
    dwrite_face_vtbl[11] = (PVOID)dwrite_face_GetGlyphIndices;
    dwrite_face_vtbl[12] = (PVOID)dwrite_face_TryGetFontTable;
    dwrite_face_vtbl[13] = (PVOID)dwrite_face_ReleaseFontTable;
    dwrite_face_vtbl[14] = (PVOID)dwrite_face_GetGlyphRunOutline;
    dwrite_face_vtbl[15] = (PVOID)dwrite_face_GetRecommendedRenderingMode;
    dwrite_face_vtbl[16] = (PVOID)dwrite_face_GetGdiCompatibleMetrics;
    dwrite_face_vtbl[17] = (PVOID)dwrite_face_GetGdiCompatibleGlyphMetrics;

    dwrite_font_file_vtbl[0] = (PVOID)dwrite_font_file_QueryInterface;
    dwrite_font_file_vtbl[1] = (PVOID)dwrite_font_file_AddRef;
    dwrite_font_file_vtbl[2] = (PVOID)dwrite_font_file_Release;
    dwrite_font_file_vtbl[3] = (PVOID)dwrite_font_file_GetReferenceKey;
    dwrite_font_file_vtbl[4] = (PVOID)dwrite_font_file_GetLoader;
    dwrite_font_file_vtbl[5] = (PVOID)dwrite_font_file_Analyze;

    dwrite_local_font_stream_vtbl[0] =
        (PVOID)dwrite_local_font_stream_QueryInterface;
    dwrite_local_font_stream_vtbl[1] =
        (PVOID)dwrite_local_font_stream_AddRef;
    dwrite_local_font_stream_vtbl[2] =
        (PVOID)dwrite_local_font_stream_Release;
    dwrite_local_font_stream_vtbl[3] =
        (PVOID)dwrite_local_font_stream_ReadFileFragment;
    dwrite_local_font_stream_vtbl[4] =
        (PVOID)dwrite_local_font_stream_ReleaseFileFragment;
    dwrite_local_font_stream_vtbl[5] =
        (PVOID)dwrite_local_font_stream_GetFileSize;
    dwrite_local_font_stream_vtbl[6] =
        (PVOID)dwrite_local_font_stream_GetLastWriteTime;

    dwrite_local_font_loader_vtbl[0] =
        (PVOID)dwrite_local_font_loader_QueryInterface;
    dwrite_local_font_loader_vtbl[1] = (PVOID)dwrite_factory_AddRef;
    dwrite_local_font_loader_vtbl[2] = (PVOID)dwrite_factory_Release;
    dwrite_local_font_loader_vtbl[3] =
        (PVOID)dwrite_local_font_loader_CreateStreamFromKey;
    dwrite_local_font_loader_vtbl[4] =
        (PVOID)dwrite_local_font_loader_GetFilePathLengthFromKey;
    dwrite_local_font_loader_vtbl[5] =
        (PVOID)dwrite_local_font_loader_GetFilePathFromKey;
    dwrite_local_font_loader_vtbl[6] =
        (PVOID)dwrite_local_font_loader_GetLastWriteTimeFromKey;

    dwrite_strings_vtbl[0] = (PVOID)dwrite_strings_QueryInterface;
    dwrite_strings_vtbl[1] = (PVOID)dwrite_factory_AddRef;
    dwrite_strings_vtbl[2] = (PVOID)dwrite_factory_Release;
    dwrite_strings_vtbl[3] = (PVOID)dwrite_strings_GetCount;
    dwrite_strings_vtbl[4] = (PVOID)dwrite_strings_FindLocaleName;
    dwrite_strings_vtbl[5] = (PVOID)dwrite_strings_GetLocaleNameLength;
    dwrite_strings_vtbl[6] = (PVOID)dwrite_strings_GetLocaleName;
    dwrite_strings_vtbl[7] = (PVOID)dwrite_strings_GetStringLength;
    dwrite_strings_vtbl[8] = (PVOID)dwrite_strings_GetString;

    dwrite_analysis_vtbl[0] = (PVOID)dwrite_analysis_QueryInterface;
    dwrite_analysis_vtbl[1] = (PVOID)dwrite_analysis_AddRef;
    dwrite_analysis_vtbl[2] = (PVOID)dwrite_analysis_Release;
    dwrite_analysis_vtbl[3] = (PVOID)dwrite_analysis_GetAlphaTextureBounds;
    dwrite_analysis_vtbl[4] = (PVOID)dwrite_analysis_CreateAlphaTexture;
    dwrite_analysis_vtbl[5] = (PVOID)dwrite_analysis_GetAlphaBlendParams;

    dwrite_fallback_vtbl[0] = (PVOID)dwrite_fallback_QueryInterface;
    dwrite_fallback_vtbl[1] = (PVOID)dwrite_factory_AddRef;
    dwrite_fallback_vtbl[2] = (PVOID)dwrite_factory_Release;
    dwrite_fallback_vtbl[3] = (PVOID)dwrite_fallback_MapCharacters;

    return (PVOID)gdi32_exports;
}
