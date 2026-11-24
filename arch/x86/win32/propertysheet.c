/* Legacy property sheets and wizards. Public descriptors retain the guest ABI. */
#include "propertysheet.h"
#include "kernel32_shim.h"
#include "compat32.h"
#include "win32_abi.h"

#define PS_PAGE_LIMIT 256
#define PS_SHEET_LIMIT 32
#define PS_SHEET_PAGES 99
#define PS_HANDLE_BASE 0x79000000U
#define PS_WIZARD_FLAGS (0x20U | 0x2000U | 0x01000000U | 0x00400000U)
#define PS_ID_BACK 0x3023
#define PS_ID_NEXT 0x3024
#define PS_ID_FINISH 0x3025

typedef struct {
    volatile int used;
    DWORD owner, generation, flags;
    BOOL compat, attached;
    HANDLE handle;
    BYTE *descriptor;
    PVOID copied_text[4];
    HINSTANCE instance;
    ULONG_PTR resource, proc, callback;
    DWORD *ref_parent;
    HWND window;
} PS_PAGE;

typedef struct {
    volatile int used;
    DWORD owner, flags;
    BOOL compat, modal, done, disposing, released, changed;
    HWND window, owner_window;
    PS_PAGE *pages[PS_SHEET_PAGES];
    int count, current;
    DWORD buttons;
    LONG_PTR result;
    ULONG_PTR callback;
    HWND back, next, finish, cancel, apply, title, subtitle;
    int width, height;
} PS_SHEET;

static PS_PAGE ps_pages[PS_PAGE_LIMIT];
static PS_SHEET ps_sheets[PS_SHEET_LIMIT];
static volatile DWORD ps_generation;

static ULONG_PTR ps_read_pointer(PCVOID data, DWORD offset, BOOL compat)
{
    ULONG_PTR value = 0;
    memcpy(&value, (const BYTE *)data + offset, compat ? 4 : 8);
    return value;
}

static LONG_PTR ps_callback(BOOL compat, ULONG_PTR proc, HWND window,
                            UINT message, LPARAM param)
{
    if (!proc) return 0;
    if (compat) {
        uint32_t args[3] = {(uint32_t)(ULONG_PTR)window, message, (uint32_t)param};
        uint32_t stack = compat32_current_user_stack_top();
        uint32_t result = stack ? compat32_callback_args_on_stack(
            (uint32_t)proc, 3, args, stack) :
            compat32_callback_args((uint32_t)proc, 3, args);
        return (LONG)(uint32_t)result;
    }
    typedef LONG_PTR (WINAPI *CALLBACK)(HWND, UINT, LPARAM);
    return ((CALLBACK)proc)(window, message, param);
}

static PSTR ps_copy_text(PCSTR text)
{
    if ((ULONG_PTR)text <= 0xFFFFU) return (PSTR)text;
    SIZE_T size = 0;
    while (size < 4096 && text[size]) size++;
    if (size == 4096) return NULL;
    PSTR copy = LocalAlloc(0, size + 1);
    if (copy) memcpy(copy, text, size + 1);
    return copy;
}

static PS_PAGE *ps_page_find(HANDLE handle)
{
    DWORD pid = GetCurrentProcessId();
    for (int i = 0; i < PS_PAGE_LIMIT; i++)
        if (ps_pages[i].used == 1 && ps_pages[i].owner == pid &&
            ps_pages[i].handle == handle) return &ps_pages[i];
    return NULL;
}

static PS_SHEET *ps_sheet_find(HWND window)
{
    DWORD pid = GetCurrentProcessId();
    for (int i = 0; i < PS_SHEET_LIMIT; i++)
        if (ps_sheets[i].used == 1 && ps_sheets[i].owner == pid &&
            ps_sheets[i].window == window) return &ps_sheets[i];
    return NULL;
}

static void ps_page_release(PS_PAGE *page, BOOL callbacks)
{
    if (!page || page->used != 1) return;
    page->used = -1;
    if (callbacks) {
        if (page->window && IsWindow(page->window)) DestroyWindow(page->window);
        if (page->callback)
            ps_callback(page->compat, page->callback, NULL, 1,
                         (LPARAM)page->descriptor); /* PSPCB_RELEASE */
        if (page->ref_parent) __atomic_sub_fetch(page->ref_parent, 1, __ATOMIC_SEQ_CST);
        for (int i = 0; i < 4; i++)
            if ((ULONG_PTR)page->copied_text[i] > 0xFFFFU)
                LocalFree(page->copied_text[i]);
        LocalFree(page->descriptor);
    }
    /* On process teardown the address-space owner reclaims guest allocations;
     * invoking its callbacks or freeing through another process heap is unsafe. */
    memset(page, 0, sizeof(*page));
}

HANDLE WINAPI shim_CreatePropertySheetPageA(PCVOID descriptor)
{
    BOOL compat = g_compat32_mode != 0;
    if (!descriptor || !win32_user_range_readable(descriptor, 8, compat)) {
        SetLastError(87);
        return NULL;
    }
    DWORD size, flags;
    memcpy(&size, descriptor, 4);
    memcpy(&flags, (const BYTE *)descriptor + 4, 4);
    DWORD step = compat ? 4 : 8;
    if (size < 8 + 8 * step || size > 4096 ||
        !win32_user_range_readable(descriptor, size, compat)) {
        SetLastError(87);
        return NULL;
    }
    PS_PAGE *page = NULL;
    for (int i = 0; i < PS_PAGE_LIMIT; i++) {
        if (__sync_bool_compare_and_swap(&ps_pages[i].used, 0, -1)) {
            page = &ps_pages[i];
            break;
        }
    }
    if (!page) { SetLastError(8); return NULL; }
    page->owner = GetCurrentProcessId();
    page->compat = compat;
    page->flags = flags;
    page->descriptor = LocalAlloc(0, size);
    if (!page->descriptor) { memset(page, 0, sizeof(*page)); return NULL; }
    memcpy(page->descriptor, descriptor, size);
    DWORD fields[4] = {8 + step, 8 + 3 * step, 8 + 8 * step, 8 + 9 * step};
    DWORD masks[4] = {0, 8, 0x1000, 0x2000};
    for (int i = 0; i < 4; i++) {
        if (fields[i] + step > size || (i == 0 && (flags & 1)) ||
            (i && !(flags & masks[i]))) continue;
        PCSTR text = (PCSTR)ps_read_pointer(descriptor, fields[i], compat);
        PSTR copy = ps_copy_text(text);
        if (text && !copy) {
            for (int j = 0; j < i; j++)
                if ((ULONG_PTR)page->copied_text[j] > 0xFFFFU) LocalFree(page->copied_text[j]);
            LocalFree(page->descriptor);
            memset(page, 0, sizeof(*page));
            SetLastError(8);
            return NULL;
        }
        page->copied_text[i] = copy;
        ULONG_PTR address = (ULONG_PTR)copy;
        memcpy(page->descriptor + fields[i], &address, step);
    }
    page->instance = (HINSTANCE)ps_read_pointer(descriptor, 8, compat);
    page->resource = ps_read_pointer(page->descriptor, 8 + step, compat);
    page->proc = ps_read_pointer(descriptor, 8 + 4 * step, compat);
    page->callback = flags & 0x80 ? ps_read_pointer(descriptor, 8 + 6 * step, compat) : 0;
    page->ref_parent = flags & 0x40 ? (DWORD *)ps_read_pointer(descriptor, 8 + 7 * step, compat) : NULL;
    page->generation = __atomic_add_fetch(&ps_generation, 1, __ATOMIC_SEQ_CST);
    page->handle = (HANDLE)(ULONG_PTR)(PS_HANDLE_BASE |
        ((page->generation & 0xFFFFU) << 8) | (DWORD)(page - ps_pages));
    if (page->ref_parent) __atomic_add_fetch(page->ref_parent, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&page->used, 1, __ATOMIC_RELEASE);
    return page->handle;
}

BOOL WINAPI shim_DestroyPropertySheetPage(HANDLE handle)
{
    PS_PAGE *page = ps_page_find(handle);
    if (!page || page->attached) { SetLastError(6); return FALSE; }
    ps_page_release(page, TRUE);
    return TRUE;
}

static void ps_buttons(PS_SHEET *sheet)
{
    BOOL finish = (sheet->buttons & 12) != 0;
    ShowWindow(sheet->next, finish ? SW_HIDE : SW_SHOW);
    ShowWindow(sheet->finish, finish ? SW_SHOW : SW_HIDE);
    EnableWindow(sheet->back, (sheet->buttons & 1) != 0);
    EnableWindow(sheet->next, (sheet->buttons & 2) != 0);
    EnableWindow(sheet->finish, (sheet->buttons & 4) != 0 && !(sheet->buttons & 8));
    EnableWindow(sheet->apply, sheet->changed);
}

static void ps_header_text(PS_SHEET *sheet, PS_PAGE *page)
{
    PCSTR title = page->copied_text[2], subtitle = page->copied_text[3];
    /* A system-class WNDPROC can still cross the PE32 callback bridge. An
     * absent header clears with NULL, never a high kernel string truncated to
     * a guest address. Nonempty titles already live in the owning guest heap. */
    SetWindowTextA(sheet->title, (ULONG_PTR)title > 0xFFFFU ? title : NULL);
    SetWindowTextA(sheet->subtitle, (ULONG_PTR)subtitle > 0xFFFFU ? subtitle : NULL);
    BOOL show = !(page->flags & 0x800) && (sheet->flags & (0x01000000U | 0x2000U));
    ShowWindow(sheet->title, show ? SW_SHOW : SW_HIDE);
    ShowWindow(sheet->subtitle, show ? SW_SHOW : SW_HIDE);
}

static BOOL ps_page_create_window(PS_SHEET *sheet, PS_PAGE *page)
{
    if (page->window && IsWindow(page->window)) return TRUE;
    if (page->callback && !ps_callback(page->compat, page->callback, NULL, 2,
                                       (LPARAM)page->descriptor)) return FALSE;
    if (page->flags & 1)
        page->window = CreateDialogIndirectParamA(page->instance,
            (PCVOID)page->resource, sheet->window, (DLGPROC)page->proc,
            (LPARAM)page->descriptor);
    else
        page->window = CreateDialogParamA(page->instance,
            (PCSTR)page->resource, sheet->window, (DLGPROC)page->proc,
            (LPARAM)page->descriptor);
    if (!page->window) return FALSE;
    ShowWindow(page->window, SW_HIDE);
    LONG_PTR style = GetWindowLongPtrA(page->window, -16);
    SetWindowLongPtrA(page->window, -16, (style | WS_CHILD | 0x400) &
        ~(WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_BORDER));
    SetParent(page->window, sheet->window);
    int top = (sheet->flags & (0x01000000U | 0x2000U)) && !(page->flags & 0x800) ? 62 : 12;
    SetWindowPos(page->window, NULL, 12, top, sheet->width - 24,
        sheet->height - top - 54, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return TRUE;
}

static BOOL ps_page_template_size(PS_PAGE *page, int *width, int *height)
{
    PCVOID data = (PCVOID)page->resource;
    DWORD size = 26;
    if (!(page->flags & 1)) {
        WCHAR name[256];
        PCWSTR id = (PCWSTR)page->resource;
        if (page->resource > 0xFFFFU) {
            if (!MultiByteToWideChar(0, 0, (PCSTR)page->resource, -1,
                                      name, 256)) return FALSE;
            id = name;
        }
        if (!kernel32_resource_data_w(page->instance, id,
                (PCWSTR)(ULONG_PTR)5, &data, &size)) return FALSE;
    }
    if (size < 18 || !win32_user_range_readable(data, 18, page->compat))
        return FALSE;
    WORD prefix[2];
    memcpy(prefix, data, sizeof(prefix));
    BOOL extended = prefix[1] == 0xFFFFU;
    DWORD offset = extended ? 22 : 14;
    if ((extended && prefix[0] != 1) || size < offset + 4 ||
        !win32_user_range_readable(data, offset + 4, page->compat)) return FALSE;
    int16_t dimensions[2];
    memcpy(dimensions, (const BYTE *)data + offset, sizeof(dimensions));
    if (dimensions[0] <= 0 || dimensions[1] <= 0) return FALSE;
    /* USER32 currently maps dialog units with an 8x16 base font. Keep the
     * sheet and its pages on that same scale, including extended templates. */
    *width = dimensions[0] * 2;
    *height = dimensions[1] * 2;
    return TRUE;
}

static LRESULT ps_notify(PS_SHEET *sheet, int index, int code, LPARAM param)
{
    if (index < 0 || index >= sheet->count) return 0;
    PS_PAGE *page = sheet->pages[index];
    if (!page->window || !IsWindow(page->window)) return 0;
    BYTE *notice = LocalAlloc(0x40, sheet->compat ? 16 : 32);
    if (!notice) return -1;
    ULONG_PTR from = (ULONG_PTR)sheet->window;
    memcpy(notice, &from, sheet->compat ? 4 : 8);
    memcpy(notice + (sheet->compat ? 8 : 16), &code, 4);
    memcpy(notice + (sheet->compat ? 12 : 24), &param, sheet->compat ? 4 : 8);
    LRESULT result = SendMessageA(page->window, 0x004E /* WM_NOTIFY */, 0, (LPARAM)notice);
    LocalFree(notice);
    return sheet->compat ? (LONG)(uint32_t)result : result;
}

static int ps_page_index(PS_SHEET *sheet, ULONG_PTR value, BOOL handle)
{
    for (int i = 0; i < sheet->count; i++) {
        PS_PAGE *page = sheet->pages[i];
        if ((handle ? (ULONG_PTR)page->handle : page->resource) == value) return i;
    }
    return -1;
}

static BOOL ps_select(PS_SHEET *sheet, int index, int direction, BOOL validate)
{
    if (sheet->done || index < 0 || index >= sheet->count) return FALSE;
    int old = sheet->current;
    if (validate && old >= 0 && ps_notify(sheet, old, -201, 0)) return FALSE;
    for (int tries = 0; tries < sheet->count; tries++) {
        if (index < 0 || index >= sheet->count || sheet->done) return FALSE;
        PS_PAGE *page = sheet->pages[index];
        if (!ps_page_create_window(sheet, page)) return FALSE;
        LRESULT result = ps_notify(sheet, index, -200, 0);
        if (!result) {
            if (old >= 0 && old != index) ShowWindow(sheet->pages[old]->window, SW_HIDE);
            sheet->current = index;
            ps_header_text(sheet, page);
            ShowWindow(page->window, SW_SHOW);
            return TRUE;
        }
        index = result == -1 ? index + direction : ps_page_index(sheet, (ULONG_PTR)result, FALSE);
    }
    return FALSE;
}

static void ps_navigate(PS_SHEET *sheet, int direction)
{
    if (sheet->done || sheet->current < 0) return;
    LRESULT result = ps_notify(sheet, sheet->current, direction < 0 ? -206 : -207, 0);
    if (result == -1) return;
    int next = result ? ps_page_index(sheet, (ULONG_PTR)result, FALSE) : sheet->current + direction;
    (void)ps_select(sheet, next, direction, TRUE);
}

static BOOL ps_apply(PS_SHEET *sheet, BOOL closing)
{
    if (sheet->current >= 0 && ps_notify(sheet, sheet->current, -201, 0)) return FALSE;
    for (int i = 0; i < sheet->count; i++) {
        LRESULT error = ps_notify(sheet, i, -202, closing);
        if (error) {
            if (error == 1) ps_select(sheet, i, 1, FALSE);
            return FALSE;
        }
    }
    sheet->changed = FALSE;
    ps_buttons(sheet);
    return TRUE;
}

static BOOL ps_finish(PS_SHEET *sheet, BOOL cancel)
{
    if (sheet->done) return TRUE;
    if (cancel) {
        if (ps_notify(sheet, sheet->current, -209, 0)) return FALSE;
        for (int i = 0; i < sheet->count; i++) ps_notify(sheet, i, -203, 0);
    } else if (sheet->flags & PS_WIZARD_FLAGS) {
        if (ps_notify(sheet, sheet->current, -208, 0)) return FALSE;
    } else if (!ps_apply(sheet, TRUE)) return FALSE;
    sheet->result = cancel ? 0 : (sheet->result ? sheet->result : 1);
    sheet->done = TRUE;
    sheet->current = -1;
    ShowWindow(sheet->window, SW_HIDE);
    return TRUE;
}

static void ps_sheet_dispose(PS_SHEET *sheet, BOOL callbacks)
{
    if (!sheet || sheet->used != 1 || sheet->disposing) return;
    sheet->disposing = TRUE;
    if (callbacks && sheet->window && IsWindow(sheet->window)) DestroyWindow(sheet->window);
    sheet->window = NULL;
    sheet->done = TRUE;
    if (!sheet->released) {
        sheet->released = TRUE;
        for (int i = 0; i < sheet->count; i++) ps_page_release(sheet->pages[i], callbacks);
    }
    sheet->disposing = FALSE;
    if (!sheet->modal || !callbacks) memset(sheet, 0, sizeof(*sheet));
}

void propertysheet_release_window(DWORD owner_pid, HWND window)
{
    for (int i = 0; i < PS_PAGE_LIMIT; i++)
        if (ps_pages[i].used == 1 && ps_pages[i].owner == owner_pid &&
            ps_pages[i].window == window) ps_pages[i].window = NULL;
    for (int i = 0; i < PS_SHEET_LIMIT; i++) {
        PS_SHEET *sheet = &ps_sheets[i];
        if (sheet->used == 1 && sheet->owner == owner_pid && sheet->window == window) {
            sheet->window = NULL;
            ps_sheet_dispose(sheet, TRUE);
        }
    }
}

void propertysheet_release_process(DWORD owner_pid)
{
    for (int i = 0; i < PS_SHEET_LIMIT; i++)
        if (ps_sheets[i].used == 1 && ps_sheets[i].owner == owner_pid)
            ps_sheet_dispose(&ps_sheets[i], FALSE);
    for (int i = 0; i < PS_PAGE_LIMIT; i++)
        if (ps_pages[i].used == 1 && ps_pages[i].owner == owner_pid)
            ps_page_release(&ps_pages[i], FALSE);
}

static LRESULT WINAPI ps_sheet_proc(HWND window, DWORD message,
                                     WPARAM wparam, LPARAM lparam)
{
    PS_SHEET *sheet = ps_sheet_find(window);
    if (!sheet) return DefWindowProcA(window, message, wparam, lparam);
    switch (message) {
    case WM_CLOSE: ps_finish(sheet, TRUE); return 0;
    case WM_COMMAND: {
        UINT id = (UINT)(wparam & 0xFFFFU);
        if (id == PS_ID_BACK) ps_navigate(sheet, -1);
        else if (id == PS_ID_NEXT) ps_navigate(sheet, 1);
        else if (id == PS_ID_FINISH || id == 1) ps_finish(sheet, FALSE);
        else if (id == 2) ps_finish(sheet, TRUE);
        return 0;
    }
    case 0x0465: { /* PSM_SETCURSEL */
        int index = lparam ? ps_page_index(sheet, (ULONG_PTR)lparam, TRUE) : (int)wparam;
        return ps_select(sheet, index, index < sheet->current ? -1 : 1, TRUE);
    }
    case 0x0466: { /* PSM_REMOVEPAGE */
        int index = lparam ? ps_page_index(sheet, (ULONG_PTR)lparam, TRUE) : (int)wparam;
        if (index < 0 || index >= sheet->count) return 0;
        PS_PAGE *page = sheet->pages[index];
        BOOL current = index == sheet->current;
        if (current) sheet->current = -1;
        else if (index < sheet->current) sheet->current--;
        for (int i = index + 1; i < sheet->count; i++) sheet->pages[i - 1] = sheet->pages[i];
        sheet->count--;
        ps_page_release(page, TRUE);
        if (current && sheet->count) ps_select(sheet, index < sheet->count ? index : index - 1, 1, FALSE);
        return 0;
    }
    case 0x0467: /* PSM_ADDPAGE */
    case 0x0477: { /* PSM_INSERTPAGE */
        PS_PAGE *page = ps_page_find((HANDLE)(ULONG_PTR)lparam);
        if (!page || page->attached || sheet->count >= PS_SHEET_PAGES) return FALSE;
        int index = message == 0x0467 ? sheet->count : (int)wparam;
        if (index < 0 || index > sheet->count) {
            index = ps_page_index(sheet, (ULONG_PTR)wparam, TRUE);
            if (index < 0) return FALSE;
        }
        for (int i = sheet->count; i > index; i--) sheet->pages[i] = sheet->pages[i - 1];
        page->attached = TRUE;
        sheet->pages[index] = page;
        sheet->count++;
        if (sheet->current >= index) sheet->current++;
        return TRUE;
    }
    case 0x0469: sheet->result = 2; return 0; /* PSM_RESTARTWINDOWS */
    case 0x046A: sheet->result = 3; return 0; /* PSM_REBOOTSYSTEM */
    case 0x046B: { /* PSM_CANCELTOCLOSE */
        EnableWindow(sheet->cancel, FALSE);
        HWND okay = GetDlgItem(window, 1);
        /* System-class dispatch may use a PE32 procedure thunk. Keep every
         * nonempty message payload in the calling process's address space. */
        if (okay) {
            PSTR text = ps_copy_text("Close");
            if (text) {
                SetWindowTextA(okay, text);
                LocalFree(text);
            }
        }
        return 0;
    }
    case 0x046C: /* PSM_QUERYSIBLINGS */
        for (int i = 0; i < sheet->count; i++) {
            if (!sheet->pages[i]->window) continue;
            LRESULT result = SendMessageA(sheet->pages[i]->window, message, wparam, lparam);
            if (result) return result;
        }
        return 0;
    case 0x046F: return SetWindowTextA(window, (PCSTR)lparam);
    case 0x0470: sheet->buttons = (DWORD)lparam; ps_buttons(sheet); return 0;
    case 0x0471: /* PSM_PRESSBUTTON */
        if (wparam == 0) ps_navigate(sheet, -1);
        else if (wparam == 1) ps_navigate(sheet, 1);
        else if (wparam == 2 || wparam == 3) ps_finish(sheet, FALSE);
        else if (wparam == 5) ps_finish(sheet, TRUE);
        else if (wparam == 6) ps_notify(sheet, sheet->current, -205, 0);
        return 0;
    case 0x0472: return ps_select(sheet, ps_page_index(sheet, (ULONG_PTR)lparam, FALSE), 1, TRUE);
    case 0x0473: /* PSM_SETFINISHTEXTA */
        SetWindowTextA(sheet->finish, (PCSTR)lparam);
        sheet->buttons = 4;
        ps_buttons(sheet);
        return 0;
    case 0x0475: /* PSM_ISDIALOGMESSAGE */
        return sheet->current >= 0 ? IsDialogMessageA(sheet->pages[sheet->current]->window, (LPMSG)lparam) : FALSE;
    case 0x0476: return sheet->current >= 0 && !sheet->done ? (LRESULT)sheet->pages[sheet->current]->window : 0;
    case 0x0478: return SendMessageW(window, 0x000C, 0, lparam); /* PSM_SETTITLEW */
    case 0x0479:
        SendMessageW(sheet->finish, 0x000C, 0, lparam);
        sheet->buttons = 4;
        ps_buttons(sheet);
        return 0;
    case 0x047D: /* PSM_SETHEADERTITLEA */
    case 0x047F: { /* PSM_SETHEADERSUBTITLEA */
        if (wparam >= (WPARAM)sheet->count) return 0;
        PS_PAGE *page = sheet->pages[wparam];
        int slot = message == 0x047D ? 2 : 3;
        PSTR copy = ps_copy_text((PCSTR)lparam);
        if (lparam && !copy) return 0;
        if ((ULONG_PTR)page->copied_text[slot] > 0xFFFFU) LocalFree(page->copied_text[slot]);
        page->copied_text[slot] = copy;
        if ((int)wparam == sheet->current) ps_header_text(sheet, page);
        return 0;
    }
    case 0x0482: return wparam < (WPARAM)sheet->count ? (LRESULT)sheet->pages[wparam]->window : 0;
    case 0x0483: return ps_page_index(sheet, (ULONG_PTR)lparam, TRUE);
    case 0x0484: return wparam < (WPARAM)sheet->count ? (LRESULT)sheet->pages[wparam]->handle : 0;
    case 0x0485: return ps_page_index(sheet, (ULONG_PTR)lparam, FALSE);
    case 0x0486: return wparam < (WPARAM)sheet->count ? (LRESULT)sheet->pages[wparam]->resource : 0;
    case 0x0487: return sheet->result;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

LONG_PTR WINAPI shim_PropertySheetA(PCVOID descriptor)
{
    BOOL compat = g_compat32_mode != 0;
    DWORD size, flags;
    if (!descriptor || !win32_user_range_readable(descriptor, 8, compat)) {
        SetLastError(87); return -1;
    }
    memcpy(&size, descriptor, 4);
    memcpy(&flags, (const BYTE *)descriptor + 4, 4);
    if (size < (compat ? 40U : 72U) || size > 4096 ||
        !win32_user_range_readable(descriptor, size, compat)) {
        SetLastError(87); return -1;
    }
    /* Start with the legacy wizard family used by redistributable installers.
     * Tabbed property dialogs and Aero-specific navigation are not provided. */
    if (!(flags & PS_WIZARD_FLAGS) || (flags & 0x4000)) {
        SetLastError(120); return -1;
    }
    DWORD count;
    memcpy(&count, (const BYTE *)descriptor + (compat ? 24 : 40), 4);
    ULONG_PTR list = ps_read_pointer(descriptor, compat ? 32 : 56, compat);
    if (!count || count > PS_SHEET_PAGES || !list) { SetLastError(87); return -1; }
    PS_SHEET *sheet = NULL;
    for (int i = 0; i < PS_SHEET_LIMIT; i++)
        if (__sync_bool_compare_and_swap(&ps_sheets[i].used, 0, -1)) { sheet = &ps_sheets[i]; break; }
    if (!sheet) { SetLastError(8); return -1; }
    sheet->owner = GetCurrentProcessId();
    sheet->compat = compat;
    sheet->flags = flags;
    sheet->modal = !(flags & 0x400);
    sheet->current = -1;
    sheet->width = 424 + 24;
    sheet->height = 228 + 12 + 54;
    sheet->buttons = 2;
    sheet->owner_window = (HWND)ps_read_pointer(descriptor, 8, compat);
    sheet->callback = flags & 0x100 ? ps_read_pointer(descriptor, compat ? 36 : 64, compat) : 0;
    __atomic_store_n(&sheet->used, 1, __ATOMIC_RELEASE);
    const BYTE *cursor = (const BYTE *)list;
    for (DWORD i = 0; i < count; i++) {
        HANDLE handle;
        if (flags & 8) {
            handle = shim_CreatePropertySheetPageA(cursor);
            if (handle) { DWORD stride; memcpy(&stride, cursor, 4); cursor += stride; }
        } else {
            if (!win32_user_range_readable(cursor, compat ? 4 : 8, compat)) goto failed;
            handle = (HANDLE)ps_read_pointer(cursor, 0, compat);
            cursor += compat ? 4 : 8;
        }
        PS_PAGE *page = ps_page_find(handle);
        if (!page || page->attached || page->compat != compat) goto failed;
        page->attached = TRUE;
        sheet->pages[sheet->count++] = page;
        int page_width, page_height;
        if (!ps_page_template_size(page, &page_width, &page_height)) goto failed;
        int top = (flags & (0x01000000U | 0x2000U)) && !(page->flags & 0x800) ? 62 : 12;
        if (page_width + 24 > sheet->width) sheet->width = page_width + 24;
        if (page_height + top + 54 > sheet->height)
            sheet->height = page_height + top + 54;
    }
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD ex_style = 0x00000001U; /* WS_EX_DLGMODALFRAME */
    if (sheet->callback) {
        WORD *dialog = LocalAlloc(0x40, 24);
        if (!dialog) goto failed;
        memcpy(dialog, &style, 4);
        memcpy(dialog + 2, &ex_style, 4);
        dialog[7] = (WORD)(sheet->width / 2);
        dialog[8] = (WORD)(sheet->height / 2);
        ps_callback(compat, sheet->callback, NULL, 2, (LPARAM)dialog);
        memcpy(&style, dialog, 4);
        memcpy(&ex_style, dialog + 2, 4);
        sheet->width = dialog[7] * 2;
        sheet->height = dialog[8] * 2;
        LocalFree(dialog);
        if (sheet->width <= 0 || sheet->height <= 0) goto failed;
    }
    if (!user32_register_library_class("OsitoPropertySheet", 8, 0, 0,
                                        (HBRUSH)16, ps_sheet_proc)) goto failed;
    PCSTR caption = (PCSTR)ps_read_pointer(descriptor, compat ? 20 : 32, compat);
    if ((ULONG_PTR)caption <= 0xFFFFU) caption = "";
    RECT outer = {0, 0, sheet->width, sheet->height};
    if (!AdjustWindowRectEx(&outer, style, FALSE, ex_style)) goto failed;
    sheet->window = CreateWindowExA(ex_style, "OsitoPropertySheet", caption,
        style, 80, 60, outer.right - outer.left, outer.bottom - outer.top,
        sheet->owner_window, NULL, NULL, NULL);
    if (!sheet->window) goto failed;
    RECT client;
    if (GetClientRect(sheet->window, &client)) {
        sheet->width = client.right;
        sheet->height = client.bottom;
    }
    int button_y = sheet->height - 36, right = sheet->width - 12;
    sheet->back = CreateWindowExA(0, "BUTTON", "< Back", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        right - 270, button_y, 84, 24, sheet->window, (HMENU)PS_ID_BACK, NULL, NULL);
    sheet->next = CreateWindowExA(0, "BUTTON", "Next >", WS_CHILD | WS_VISIBLE | WS_TABSTOP | 1,
        right - 182, button_y, 84, 24, sheet->window, (HMENU)PS_ID_NEXT, NULL, NULL);
    sheet->finish = CreateWindowExA(0, "BUTTON", "Finish", WS_CHILD | WS_TABSTOP | 1,
        right - 182, button_y, 84, 24, sheet->window, (HMENU)PS_ID_FINISH, NULL, NULL);
    sheet->cancel = CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        right - 94, button_y, 84, 24, sheet->window, (HMENU)2, NULL, NULL);
    sheet->title = CreateWindowExA(0, "STATIC", "", WS_CHILD, 12, 8, sheet->width - 24, 20, sheet->window, NULL, NULL, NULL);
    sheet->subtitle = CreateWindowExA(0, "STATIC", "", WS_CHILD, 12, 30, sheet->width - 24, 24, sheet->window, NULL, NULL, NULL);
    if (!sheet->cancel || !sheet->finish || !sheet->back || !sheet->next) goto failed;
    ps_buttons(sheet);
    for (int i = 0; i < sheet->count; i++)
        if ((sheet->pages[i]->flags & 0x400) && !ps_page_create_window(sheet, sheet->pages[i])) goto failed;
    ULONG_PTR start = ps_read_pointer(descriptor, compat ? 28 : 48, compat);
    int index = (flags & 0x40) ? ps_page_index(sheet, start, FALSE) : (int)start;
    if (index < 0 || index >= sheet->count) index = 0;
    if (!ps_select(sheet, index, 1, FALSE)) goto failed;
    if (sheet->callback) ps_callback(compat, sheet->callback, sheet->window, 1, 0);
    if (!sheet->done) ShowWindow(sheet->window, SW_SHOW);
    if (!sheet->modal) return (LONG_PTR)sheet->window;
    BOOL disabled = sheet->owner_window && IsWindowEnabled(sheet->owner_window);
    if (disabled) EnableWindow(sheet->owner_window, FALSE);
    BOOL quit = FALSE;
    int quit_code = 0;
    while (!sheet->done && sheet->window && IsWindow(sheet->window)) {
        MSG msg64;
        uint32_t msg32[7];
        LPMSG msg = compat ? (LPMSG)(void *)msg32 : &msg64;
        BOOL status = GetMessageA(msg, NULL, 0, 0);
        if (status <= 0) {
            sheet->result = -1;
            quit = status == 0;
            quit_code = compat ? (int)msg32[2] : (int)msg64.wParam;
            break;
        }
        if (!ps_sheet_proc(sheet->window, 0x0475, 0, (LPARAM)msg)) {
            TranslateMessage(msg);
            DispatchMessageA(msg);
        }
    }
    LONG_PTR result = sheet->result;
    HWND owner = sheet->owner_window;
    ps_sheet_dispose(sheet, TRUE);
    memset(sheet, 0, sizeof(*sheet));
    if (disabled && IsWindow(owner)) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    if (quit) PostQuitMessage(quit_code);
    return result;
failed:
    ps_sheet_dispose(sheet, TRUE);
    memset(sheet, 0, sizeof(*sheet));
    return -1;
}
