/* Exercise the production GOP handoff with firmware protocol mocks. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../boot/boot_efi.c"

#define MOCK_MODES 32

EFI_BOOT_SERVICES *BS;
EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES services;
static EFI_SYSTEM_TABLE system_table;
static EFI_GRAPHICS_OUTPUT_PROTOCOL graphics;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE active;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION active_info;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION modes[MOCK_MODES];
static EFI_STATUS query_status[MOCK_MODES], set_status[MOCK_MODES];
static UINTN query_size[MOCK_MODES];
static BOOLEAN query_null[MOCK_MODES];
static EFI_STATUS console_status, locate_status;
static BOOLEAN no_protocol, set_invalid;
static unsigned handle_calls, locate_calls, query_calls, set_calls;
static unsigned allocations, frees;
static UINT32 last_set;

UINTN Print(const CHAR16 *fmt, ...)
{
    (void)fmt;
    return 0;
}

static EFI_STATUS EFIAPI mock_handle(EFI_HANDLE handle, EFI_GUID *guid, void **out)
{
    EFI_GUID expected = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    assert(handle == ST->ConsoleOutHandle);
    assert(memcmp(guid, &expected, sizeof(expected)) == 0);
    handle_calls++;
    *out = !EFI_ERROR(console_status) && !no_protocol ? &graphics : NULL;
    return console_status;
}

static EFI_STATUS EFIAPI mock_locate(EFI_GUID *guid, void *registration, void **out)
{
    EFI_GUID expected = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    assert(memcmp(guid, &expected, sizeof(expected)) == 0);
    assert(!registration);
    locate_calls++;
    *out = !EFI_ERROR(locate_status) && !no_protocol ? &graphics : NULL;
    return locate_status;
}

static EFI_STATUS EFIAPI mock_free(void *buffer)
{
    assert(buffer && frees < allocations);
    free(buffer);
    frees++;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_query(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 mode,
                                    UINTN *size,
                                    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info)
{
    assert(gop == &graphics && mode < active.MaxMode && mode < MOCK_MODES);
    query_calls++;
    *size = query_size[mode];
    *info = NULL;
    if (!query_null[mode]) {
        *info = malloc(sizeof(**info));
        assert(*info);
        **info = modes[mode];
        allocations++;
    }
    return query_status[mode];
}

static void activate(UINT32 mode)
{
    active.Mode = mode;
    active_info = modes[mode];
    active.Info = &active_info;
    active.SizeOfInfo = sizeof(active_info);
    active.FrameBufferBase = 0xe0000000;
    active.FrameBufferSize = (UINTN)active_info.PixelsPerScanLine *
                            active_info.VerticalResolution * sizeof(UINT32);
}

static EFI_STATUS EFIAPI mock_set(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 mode)
{
    assert(gop == &graphics && mode < active.MaxMode && mode < MOCK_MODES);
    set_calls++;
    last_set = mode;
    if (!EFI_ERROR(set_status[mode])) {
        activate(mode);
        if (set_invalid)
            active.FrameBufferSize = 1;
    }
    return set_status[mode];
}

static void reset(UINT32 count, UINT32 current)
{
    assert(allocations == frees);
    memset(&services, 0, sizeof(services));
    memset(&system_table, 0, sizeof(system_table));
    memset(&graphics, 0, sizeof(graphics));
    memset(&active, 0, sizeof(active));
    memset(modes, 0, sizeof(modes));
    BS = &services;
    ST = &system_table;
    BS->HandleProtocol = mock_handle;
    BS->LocateProtocol = mock_locate;
    BS->FreePool = mock_free;
    ST->ConsoleOutHandle = &system_table;
    graphics.QueryMode = mock_query;
    graphics.SetMode = mock_set;
    graphics.Mode = &active;
    active.MaxMode = count;
    for (UINT32 i = 0; i < MOCK_MODES; i++) {
        modes[i].HorizontalResolution = 640 + i * 16;
        modes[i].VerticalResolution = 480 + i * 8;
        modes[i].PixelsPerScanLine = modes[i].HorizontalResolution;
        modes[i].PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
        query_size[i] = sizeof(modes[i]);
        query_status[i] = set_status[i] = EFI_SUCCESS;
        query_null[i] = FALSE;
    }
    console_status = locate_status = EFI_SUCCESS;
    no_protocol = set_invalid = FALSE;
    handle_calls = locate_calls = query_calls = set_calls = 0;
    allocations = frees = 0;
    last_set = ~(UINT32)0;
    activate(current);
}

static void check_handoff(void)
{
    assert(allocations == frees);
    assert(gop_selected_mode < gop_mode_count);
    const boot_display_mode_t *mode = &gop_modes[gop_selected_mode];
    assert(gop_fb_base == active.FrameBufferBase);
    assert(gop_fb_width == active_info.HorizontalResolution);
    assert(gop_fb_height == active_info.VerticalResolution);
    assert(gop_fb_pitch == active_info.PixelsPerScanLine);
    assert(mode->width == gop_fb_width && mode->height == gop_fb_height);
    assert(mode->pitch == gop_fb_pitch && mode->pixel_format == active_info.PixelFormat);
}

static void test_keep_firmware_mode(void)
{
    static const UINT32 resolutions[][2] = {{1920, 1080}, {3840, 2160}, {800, 600}};
    for (unsigned i = 0; i < sizeof(resolutions) / sizeof(resolutions[0]); i++) {
        reset(6, 0);
        modes[0].HorizontalResolution = resolutions[i][0];
        modes[0].VerticalResolution = resolutions[i][1];
        modes[0].PixelsPerScanLine = resolutions[i][0] + 64;
        activate(0);
        assert(init_gop() == EFI_SUCCESS);
        assert(!set_calls && !locate_calls && handle_calls == 1);
        assert(query_calls == 5 && gop_mode_count == 6);
        check_handoff();
    }
    puts("PASS preserve active firmware resolution and padded stride");
}

static void test_compact_table(void)
{
    reset(8, 7);
    modes[0].PixelFormat = PixelBltOnly;
    modes[1].PixelFormat = PixelBitMask;
    modes[2].PixelsPerScanLine = 1;
    query_size[3] = sizeof(modes[3]) - 1;
    query_status[4] = EFI_DEVICE_ERROR;
    query_null[5] = TRUE;
    assert(init_gop() == EFI_SUCCESS);
    assert(!set_calls && gop_mode_count == 2 && gop_selected_mode == 0);
    assert(gop_modes[1].width == modes[6].HorizontalResolution);
    check_handoff();

    reset(MOCK_MODES, MOCK_MODES - 1);
    assert(init_gop() == EFI_SUCCESS);
    assert(!set_calls && gop_mode_count == BOOT_MAX_DISPLAY_MODES);
    assert(query_calls == MOCK_MODES - 1);
    check_handoff();
    puts("PASS compact mode index and active mode retained in full table");
}

static void test_console_fallback(void)
{
    reset(1, 0);
    console_status = EFI_UNSUPPORTED;
    assert(init_gop() == EFI_SUCCESS);
    assert(handle_calls == 1 && locate_calls == 1);
    check_handoff();
    reset(1, 0);
    ST->ConsoleOutHandle = NULL;
    assert(init_gop() == EFI_SUCCESS);
    assert(!handle_calls && locate_calls == 1);
    check_handoff();
    reset(1, 0);
    console_status = locate_status = EFI_NOT_FOUND;
    assert(init_gop() == EFI_NOT_FOUND);
    assert(!gop_fb_base && !gop_mode_count && !query_calls && !set_calls);
    reset(1, 0);
    no_protocol = TRUE;
    assert(init_gop() == EFI_NOT_FOUND);
    assert(!gop_fb_base && !gop_mode_count);
    puts("PASS console preference, discovery fallback and no protocol");
}

static void test_mode_validation(void)
{
    reset(1, 0);
    assert(gop_framebuffer_usable(&graphics));
    assert(!gop_framebuffer_usable(NULL));
    assert(!gop_mode_usable(NULL, sizeof(active_info)));
    assert(!gop_mode_usable(&active_info, sizeof(active_info) - 1));
    active_info.PixelFormat = PixelRedGreenBlueReserved8BitPerColor;
    assert(gop_framebuffer_usable(&graphics));
    active_info.PixelFormat = (EFI_GRAPHICS_PIXEL_FORMAT)-1;
    assert(!gop_framebuffer_usable(&graphics));
    activate(0);
    active_info.HorizontalResolution = 0;
    assert(!gop_framebuffer_usable(&graphics));
    activate(0);
    active_info.VerticalResolution = 0;
    assert(!gop_framebuffer_usable(&graphics));
    activate(0);
    active_info.PixelsPerScanLine--;
    assert(!gop_framebuffer_usable(&graphics));
    activate(0);
    active_info.HorizontalResolution = active_info.PixelsPerScanLine = ~(UINT32)0;
    active_info.VerticalResolution = ~(UINT32)0;
    assert(!gop_framebuffer_usable(&graphics));
    activate(0);
    active.FrameBufferSize--;
    assert(!gop_framebuffer_usable(&graphics));
    activate(0);
    active.FrameBufferBase = 0;
    assert(!gop_framebuffer_usable(&graphics));
    active.FrameBufferBase = ~(UINT64)0 - 1;
    assert(!gop_framebuffer_usable(&graphics));
    activate(0);
    active.Mode = active.MaxMode;
    assert(!gop_framebuffer_usable(&graphics));
    activate(0);
    active.Info = NULL;
    assert(!gop_framebuffer_usable(&graphics));
    graphics.Mode = NULL;
    assert(init_gop() == EFI_UNSUPPORTED);
    puts("PASS framebuffer extent, format, overflow and metadata validation");
}

static void test_fallback_modes(void)
{
    reset(4, 0);
    modes[0].PixelFormat = PixelBltOnly;
    activate(0);
    set_status[1] = EFI_DEVICE_ERROR;
    assert(init_gop() == EFI_SUCCESS);
    assert(set_calls == 2 && last_set == 2 && active.Mode == 2);
    check_handoff();
    reset(1, 0);
    active.FrameBufferBase = 0;
    assert(init_gop() == EFI_SUCCESS);
    assert(set_calls == 1 && last_set == 0);
    check_handoff();
    reset(1, 0);
    active.FrameBufferSize = 1;
    set_status[0] = EFI_DEVICE_ERROR;
    assert(init_gop() == EFI_DEVICE_ERROR);
    assert(set_calls == 1 && !gop_fb_base && !gop_mode_count);
    reset(2, 0);
    active.FrameBufferSize = 1;
    set_invalid = TRUE;
    assert(init_gop() == EFI_DEVICE_ERROR);
    assert(set_calls == 2 && !gop_fb_base && !gop_mode_count);
    reset(1, 0);
    modes[0].PixelFormat = PixelBltOnly;
    activate(0);
    assert(init_gop() == EFI_UNSUPPORTED);
    assert(!set_calls && !gop_mode_count && !gop_fb_base);
    assert(allocations == frees);
    puts("PASS recovery only when needed, SetMode errors and invalid success");
}

int main(void)
{
    test_keep_firmware_mode();
    test_compact_table();
    test_console_fallback();
    test_mode_validation();
    test_fallback_modes();
    puts("PASS GOP boot handoff contract");
    return 0;
}
