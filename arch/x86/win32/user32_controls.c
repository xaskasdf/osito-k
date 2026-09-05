/* Native BUTTON/STATIC procedures, shared by PE32 and PE64 callers. */
#include "user32_controls.h"
#include "gdi32_shim.h"
#include "kernel32_shim.h"
#include "compat32.h"

extern void *kcalloc(uint64_t count, uint64_t size);
extern void kfree(void *pointer);

#define WM_GETDLGCODE 0x0087
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_PRINTCLIENT 0x0318
#define WM_CANCELMODE 0x001F
#define BM_GETCHECK 0x00F0
#define BM_SETCHECK 0x00F1
#define BM_GETSTATE 0x00F2
#define BM_SETSTATE 0x00F3
#define BM_SETSTYLE 0x00F4
#define BM_GETIMAGE 0x00F6
#define BM_SETIMAGE 0x00F7
#define STM_SETICON 0x0170
#define STM_GETICON 0x0171
#define STM_SETIMAGE 0x0172
#define STM_GETIMAGE 0x0173
#define BS_TYPEMASK 15u
#define BS_NOTIFY 0x4000u
#define BS_PUSHLIKE 0x1000u

struct U32_CONTROL {
    UINT atom, references;
    BOOL dead, pressed, mouse_armed, key_armed;
    UINT check;
    HFONT font;
    HANDLE image;
    UINT image_type;
};

U32_CONTROL *user32_control_create(UINT atom)
{
    U32_CONTROL *control = kcalloc(1, sizeof(*control));
    if (control) { control->atom = atom; control->references = 1; }
    return control;
}

void user32_control_release(U32_CONTROL *control)
{
    if (!control) return;
    control->dead = TRUE;
    if (--control->references == 0) kfree(control);
}

static DWORD control_style(HWND window)
{
    return (DWORD)GetWindowLongPtrA(window, -16);
}

static void control_redraw(U32_CONTROL *control, HWND window)
{
    if (!control->dead) {
        InvalidateRect(window, NULL, TRUE);
        UpdateWindow(window);
    }
}

static void control_notify(U32_CONTROL *control, HWND window, UINT code)
{
    if (control->dead) return;
    HWND parent = GetParent(window);
    if (parent) SendMessageA(parent, WM_COMMAND,
        ((UINT)GetDlgCtrlID(window) & 0xFFFFu) | ((WPARAM)code << 16),
        (LPARAM)(ULONG_PTR)window);
}

static void control_fill(HDC dc, RECT rect, int color)
{
    if (rect.right > rect.left && rect.bottom > rect.top)
        FillRect(dc, &rect, (HBRUSH)(ULONG_PTR)(color+1));
}

static void control_frame(HDC dc, RECT rect, int first, int second)
{
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    control_fill(dc, (RECT){rect.left, rect.top, rect.right, rect.top+1}, first);
    control_fill(dc, (RECT){rect.left, rect.top, rect.left+1, rect.bottom}, first);
    control_fill(dc, (RECT){rect.left, rect.bottom-1, rect.right, rect.bottom}, second);
    control_fill(dc, (RECT){rect.right-1, rect.top, rect.right, rect.bottom}, second);
}

static BOOL button_radio(UINT type) { return type == 4 || type == 9; }
static BOOL button_check(UINT type)
{
    return type == 2 || type == 3 || type == 5 || type == 6 || button_radio(type);
}

static UINT button_max_check(UINT type)
{
    return type == 5 || type == 6 ? 2 : button_check(type) ? 1 : 0;
}

static void button_uncheck_group(U32_CONTROL *control, HWND window)
{
    HWND first = GetWindow(window, 0), start = first;
    for (HWND item = first; item; item = GetWindow(item, 2)) {
        if (control_style(item) & 0x20000) start = item; /* WS_GROUP */
        if (item == window) break;
    }
    for (HWND item = start; item && !control->dead;) {
        HWND next = GetWindow(item, 2);
        if (item != start && (control_style(item) & 0x20000)) break;
        if (item != window && (SendMessageA(item, WM_GETDLGCODE, 0, 0) & 0x40))
            SendMessageA(item, BM_SETCHECK, 0, 0);
        item = next;
    }
}

static void button_click(U32_CONTROL *control, HWND window)
{
    DWORD style = control_style(window);
    UINT type = style & BS_TYPEMASK;
    if (control->dead || (style & WS_DISABLED) || type == 7) return;
    if (type == 3) control->check = !control->check;
    else if (type == 6) control->check = (control->check+1) % 3;
    else if (type == 9) {
        SendMessageA(window, BM_SETCHECK, 1, 0);
        button_uncheck_group(control, window);
    }
    control_redraw(control, window);
    control_notify(control, window, BN_CLICKED);
}

static BOOL control_inside(HWND window, LPARAM point)
{
    RECT rect;
    int x = (short)(point & 0xFFFF), y = (short)((point >> 16) & 0xFFFF);
    return GetClientRect(window, &rect) && x >= 0 && y >= 0 &&
           x < rect.right && y < rect.bottom;
}

static void control_owner_draw(U32_CONTROL *control, HWND window,
                                HDC dc, RECT rect)
{
    typedef struct {
        UINT type, id, item, action, state;
        HWND window; HDC dc; RECT rect; ULONG_PTR data;
    } DRAW_ITEM;
    typedef struct {
        UINT type, id, item, action, state, window, dc;
        RECT rect; UINT data;
    } DRAW_ITEM32;
    _Static_assert(sizeof(DRAW_ITEM) == 64, "DRAWITEMSTRUCT64 ABI");
    _Static_assert(sizeof(DRAW_ITEM32) == 48, "DRAWITEMSTRUCT32 ABI");
    HWND parent = GetParent(window);
    if (!parent || control->dead) return;
    UINT state = (control->pressed ? 1 : 0) | (control->check ? 8 : 0) |
        ((control_style(window) & WS_DISABLED) ? 4 : 0) |
        (GetFocus() == window ? 16 : 0);
    UINT id = (UINT)GetDlgCtrlID(window), type = control->atom == 0x80 ? 4 : 5;
    HANDLE heap = GetProcessHeap();
    /* Application callbacks must receive both the right layout and a pointer
     * accessible to PE32, not a truncated native kernel stack address. */
    PVOID payload = HeapAlloc(heap, 0, g_compat32_mode ? 48 : 64);
    if (!payload) return;
    if (g_compat32_mode) *(DRAW_ITEM32 *)payload = (DRAW_ITEM32){
        type, id, 0, 1, state, (UINT)(ULONG_PTR)window,
        (UINT)(ULONG_PTR)dc, rect, 0};
    else *(DRAW_ITEM *)payload = (DRAW_ITEM){type, id, 0, 1, state, window, dc, rect, 0};
    SendMessageA(parent, 0x002B, id, (LPARAM)(ULONG_PTR)payload);
    HeapFree(heap, 0, payload);
}

static void control_image(U32_CONTROL *control, HDC dc, RECT rect, BOOL centered)
{
    if (!control->image) return;
    if (control->image_type == 1) {
        int width = GetSystemMetrics(SM_CXICON), height = GetSystemMetrics(SM_CYICON);
        DrawIconEx(dc, centered ? (rect.right-width)/2 : 0,
            centered ? (rect.bottom-height)/2 : 0,
            control->image, width, height, 0, NULL, 3);
    } else {
        BYTE object[32];
        if (!GetObjectA(control->image, sizeof(object), object)) return;
        LONG *fields = (LONG *)object;
        int width = fields[1], height = fields[2];
        HDC source = CreateCompatibleDC(dc);
        if (!source) return;
        HGDIOBJ old = SelectObject(source, control->image);
        BitBlt(dc, centered ? (rect.right-width)/2 : 0,
            centered ? (rect.bottom-height)/2 : 0,
            width, height, source, 0, 0, 0x00CC0020);
        SelectObject(source, old);
        DeleteDC(source);
    }
}

static void button_marker(HDC dc, RECT marker, BOOL radio, UINT checked,
                            BOOL pressed, BOOL disabled)
{
    int ink = disabled || checked == 2 ? 17 : 18;
    if (radio) {
        int cx = (marker.left+marker.right)/2, cy = (marker.top+marker.bottom)/2;
        for (int y = -6; y <= 6; y++) for (int x = -6; x <= 6; x++) {
            int square = x*x+y*y;
            if (square > 40) continue;
            int color = square >= 25 ? (x+y < 0 ? 16 : 20) :
                checked && square <= 6 ? ink : pressed ? 15 : 5;
            control_fill(dc, (RECT){cx+x,cy+y,cx+x+1,cy+y+1}, color);
        }
    } else {
        control_fill(dc, marker, pressed ? 15 : 5);
        control_frame(dc, marker, 16, 20);
        if (checked) {
            int x = marker.left+3, y = marker.top+5;
            for (int i = 0; i < 3; i++)
                control_fill(dc, (RECT){x+i,y+i,x+i+2,y+i+2}, ink);
            for (int i = 0; i < 6; i++)
                control_fill(dc, (RECT){x+2+i,y+2-i,x+4+i,y+4-i}, ink);
        }
    }
}

static void control_paint(U32_CONTROL *control, HWND window, HDC dc)
{
    RECT rect;
    if (!dc || control->dead || !GetClientRect(window, &rect)) return;
    DWORD style = control_style(window);
    UINT type = style & (control->atom == 0x80 ? 15 : 31);
    if ((control->atom == 0x80 && type == 11) ||
        (control->atom == 0x82 && type == 13)) {
        control_owner_draw(control, window, dc, rect);
        return;
    }
    PWSTR caption = user32_copy_window_text(window);
    if (!caption) return;
    DWORD text_color = GetTextColor(dc), background_color = GetBkColor(dc);
    UINT alignment = SetTextAlign(dc, 0);
    int background_mode = SetBkMode(dc, 1);
    HGDIOBJ font = SelectObject(dc, control->font ? control->font : GetStockObject(17));
    SetTextColor(dc, GetSysColor(18));
    SetBkColor(dc, GetSysColor(15));
    HWND parent = GetParent(window);
    UINT message = control->atom == 0x80 ? 0x0135 : 0x0138;
    BOOL narrow = g_compat32_mode;
    LRESULT answer = parent ? SendMessageA(parent, message,
        (WPARAM)(ULONG_PTR)dc, (LPARAM)(ULONG_PTR)window) : 0;
    HBRUSH brush = (HBRUSH)(narrow ? (ULONG_PTR)(UINT)answer : (ULONG_PTR)answer);
    if (control->dead) goto restore;
    if (brush) FillRect(dc, &rect, brush);
    else control_fill(dc, rect, 15);
    BOOL disabled = (style & WS_DISABLED) != 0;
    if (disabled && !(control->atom == 0x82 && type == 11))
        SetTextColor(dc, GetSysColor(17));
    if (control->atom == 0x82) {
        if (type >= 4 && type <= 9) {
            int color = type % 3 == 1 ? 6 : type % 3 == 2 ? 1 : 5;
            if (type < 7) control_fill(dc, rect, color);
            else control_frame(dc, rect, color, color);
        } else if (type == 16 || type == 17 || type == 18) {
            if (type == 16) rect.bottom = rect.top+2;
            if (type == 17) rect.right = rect.left+2;
            control_frame(dc, rect, 16, 20);
        } else if (type == 3 || type == 14) {
            control_image(control, dc, rect, (style & 0x200) != 0);
        } else {
            UINT flags = type == 11 ? 0x20 : type == 12 ? 0x40 : 0x50;
            if (type == 1) flags |= 1;
            if (type == 2) flags |= 2;
            if (style & 0x80) flags |= 0x800;
            if (style & 0x200) flags = (flags & ~0x10u) | 0x24;
            if (style & 0xC000) {
                flags = (flags & ~0x10u) | 0x20;
                flags |= (style & 0xC000) == 0x4000 ? 0x8000 :
                         (style & 0xC000) == 0x8000 ? 0x4000 : 0x40000;
            }
            if (style & 0x20000) {
                control_frame(dc, rect, 16, 20);
                rect.left++; rect.top++; rect.right--; rect.bottom--;
            }
            DrawTextW(dc, caption, -1, &rect, flags);
        }
    } else if (type == 7) {
        RECT frame = rect;
        frame.top += 7;
        control_frame(dc, frame, 16, 20);
        RECT label = {rect.left+8, rect.top, rect.right-8, rect.bottom};
        RECT measured = label;
        DrawTextW(dc, caption, -1, &measured, 0x420);
        control_fill(dc, measured, 15);
        DrawTextW(dc, caption, -1, &label, 0x20);
    } else {
        RECT label = rect;
        BOOL boxed = type <= 1 || (style & BS_PUSHLIKE);
        if (boxed) {
            control_fill(dc, rect, 15);
            if (type == 1) {
                control_frame(dc, label, 6, 6);
                label.left++; label.top++; label.right--; label.bottom--;
            }
            BOOL down = control->pressed || ((style & BS_PUSHLIKE) && control->check);
            control_frame(dc, label, down ? 16 : 20, down ? 20 : 16);
            label.left += 4; label.right -= 4; label.top += 3; label.bottom -= 3;
            if (down) { label.left++; label.right++; label.top++; label.bottom++; }
        } else if (button_check(type)) {
            RECT marker = {rect.left, (rect.bottom-13)/2, rect.left+13, (rect.bottom+13)/2};
            if (style & 0x20) { marker.left = rect.right-13; marker.right = rect.right; }
            button_marker(dc, marker, button_radio(type), control->check, control->pressed, disabled);
            if (style & 0x20) label.right -= 17;
            else label.left += 17;
        }
        UINT flags = (style & 0x2000) ? 0x10 : 0x24;
        flags |= (style & 0x300) == 0x100 ? 0 :
                 (style & 0x300) == 0x200 ? 2 : boxed ? 1 : 0;
        if (style & (0x40 | 0x80)) control_image(control, dc, rect, TRUE);
        else DrawTextW(dc, caption, -1, &label, flags);
        if (GetFocus() == window) {
            if (!boxed) {
                RECT measured = label;
                DrawTextW(dc, caption, -1, &measured, flags | 0x400);
                if (measured.right < label.right) label.right = measured.right+1;
            }
            DrawFocusRect(dc, &label);
        }
    }
restore:
    SelectObject(dc, font);
    SetTextAlign(dc, alignment);
    SetBkMode(dc, background_mode);
    SetTextColor(dc, text_color);
    SetBkColor(dc, background_color);
    kfree(caption);
}

static void control_render(U32_CONTROL *control, HWND window, HDC dc)
{
    /* STATIC bitmap printing uses the supplied DC's clip. A window DC is
     * already bounded to its client; BUTTON also clips owner-draw callbacks. */
    if (control->atom != 0x80) { control_paint(control, window, dc); return; }
    RECT client;
    uint64_t token;
    if (!GetClientRect(window, &client) ||
        !gdi32_push_paint_clip(dc, (const GDI_RECT *)&client, &token)) return;
    control_paint(control, window, dc);
    gdi32_pop_paint_clip(dc, token, TRUE);
}

static BOOL control_message(U32_CONTROL *control, HWND window, UINT message,
                             WPARAM wp, LPARAM lp, LRESULT *result)
{
    DWORD style = control_style(window);
    UINT type = style & BS_TYPEMASK;
    *result = 0;
    switch (message) {
    case WM_GETFONT: *result = (LRESULT)(ULONG_PTR)control->font; return TRUE;
    case WM_SETFONT:
        control->font = (HFONT)(ULONG_PTR)wp;
        if (lp) control_redraw(control, window);
        return TRUE;
    case WM_ERASEBKGND: *result = 1; return TRUE;
    case WM_PAINT: {
        BYTE paint[72];
        HDC dc = wp ? (HDC)(ULONG_PTR)wp : BeginPaint(window, paint);
        if (dc) control_render(control, window, dc);
        if (dc && !wp) EndPaint(window, paint);
        return TRUE;
    }
    case WM_PRINTCLIENT:
        control_render(control, window, (HDC)(ULONG_PTR)wp);
        return TRUE;
    case WM_GETDLGCODE:
        *result = control->atom == 0x82 || type == 7 ? 0x100 :
            0x2000 | (type == 1 ? 0x10 : type == 0 ? 0x20 : button_radio(type) ? 0x40 : 0);
        return TRUE;
    case WM_ENABLE:
        control->pressed = control->mouse_armed = control->key_armed = FALSE;
        if (GetCapture() == window) ReleaseCapture();
        control_redraw(control, window);
        return TRUE;
    case WM_SIZE:
        InvalidateRect(window, NULL, TRUE);
        return TRUE;
    }
    if (control->atom == 0x82) {
        if (message == STM_GETICON || message == STM_GETIMAGE) {
            UINT image_type = message == STM_GETICON ? 1 : (UINT)wp;
            *result = image_type == control->image_type ? (LRESULT)(ULONG_PTR)control->image : 0;
            return TRUE;
        }
        if (message == STM_SETIMAGE || message == STM_SETICON) {
            UINT image_type = message == STM_SETICON ? 1 : (UINT)wp;
            UINT static_type = style & 31;
            if ((static_type == 14 && image_type == 0) || (static_type == 3 && image_type == 1)) {
                *result = (LRESULT)(ULONG_PTR)control->image;
                control->image = (HANDLE)(ULONG_PTR)(message == STM_SETICON ? wp : lp);
                control->image_type = image_type;
                if (image_type == 0 && !(style & (0x200 | 0x40)) && control->image) {
                    BYTE object[32];
                    if (GetObjectA(control->image, sizeof(object), object))
                        SetWindowPos(window, NULL, 0, 0, ((LONG *)object)[1], ((LONG *)object)[2], 0x16);
                }
                control_redraw(control, window);
            }
            return TRUE;
        }
        if (message == WM_NCHITTEST) { *result = style & 0x100 ? HTCLIENT : -1; return TRUE; }
        if ((message == WM_LBUTTONDOWN || message == 0x0203) && (style & 0x100)) {
            control_notify(control, window, message == WM_LBUTTONDOWN ? 0 : 1);
            return TRUE;
        }
        return FALSE;
    }
    switch (message) {
    case BM_GETCHECK: *result = control->check; return TRUE;
    case BM_SETCHECK: {
        UINT maximum = button_max_check(type);
        control->check = wp > maximum ? maximum : (UINT)wp;
        if (button_radio(type)) {
            DWORD next = control->check ? style | WS_TABSTOP : style & ~WS_TABSTOP;
            if (next != style) SetWindowLongPtrA(window, -16, next);
        }
        control_redraw(control, window);
        return TRUE;
    }
    case BM_GETSTATE:
        *result = control->check | (control->pressed ? 4 : 0) | (GetFocus() == window ? 8 : 0);
        return TRUE;
    case BM_SETSTATE:
        control->pressed = wp != 0;
        control_redraw(control, window);
        return TRUE;
    case BM_SETSTYLE:
        SetWindowLongPtrA(window, -16, (style & ~BS_TYPEMASK) | (wp & BS_TYPEMASK));
        if (lp) control_redraw(control, window);
        return TRUE;
    case BM_GETIMAGE:
        *result = wp == control->image_type ? (LRESULT)(ULONG_PTR)control->image : 0;
        return TRUE;
    case BM_SETIMAGE:
        if (wp <= 1) {
            *result = (LRESULT)(ULONG_PTR)control->image;
            control->image = (HANDLE)(ULONG_PTR)lp;
            control->image_type = (UINT)wp;
            control_redraw(control, window);
        }
        return TRUE;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        if (message == WM_KILLFOCUS) control->key_armed = control->pressed = FALSE;
        control_redraw(control, window);
        if (style & BS_NOTIFY) control_notify(control, window, message == WM_SETFOCUS ? 6 : 7);
        return TRUE;
    case WM_CANCELMODE: case WM_CAPTURECHANGED:
        control->mouse_armed = control->key_armed = control->pressed = FALSE;
        if (message == WM_CANCELMODE && GetCapture() == window) ReleaseCapture();
        control_redraw(control, window);
        return TRUE;
    }
    if (style & WS_DISABLED) return FALSE;
    switch (message) {
    case BM_CLICK:
        button_click(control, window);
        return TRUE;
    case WM_LBUTTONDOWN:
        if (type == 7) return TRUE;
        SetFocus(window);
        if (control->dead) return TRUE;
        SetCapture(window);
        control->mouse_armed = control->pressed = TRUE;
        control_redraw(control, window);
        return TRUE;
    case WM_MOUSEMOVE:
        if (control->mouse_armed && GetCapture() == window) {
            BOOL pressed = control_inside(window, lp);
            if (pressed != control->pressed) {
                control->pressed = pressed;
                control_redraw(control, window);
            }
        }
        return TRUE;
    case WM_LBUTTONUP: {
        BOOL click = control->mouse_armed && control->pressed &&
            GetCapture() == window && control_inside(window, lp);
        control->mouse_armed = control->pressed = FALSE;
        if (GetCapture() == window) ReleaseCapture();
        control_redraw(control, window);
        if (click) button_click(control, window);
        return TRUE;
    }
    case WM_KEYDOWN:
        if (wp == VK_SPACE && type != 7) {
            control->key_armed = control->pressed = TRUE;
            control_redraw(control, window);
        }
        return TRUE;
    case WM_KEYUP:
        if (wp == VK_SPACE && control->key_armed) {
            control->key_armed = control->pressed = FALSE;
            button_click(control, window);
        }
        return TRUE;
    }
    return FALSE;
}

BOOL user32_control_message(U32_CONTROL *control, HWND window, UINT message,
                            WPARAM wp, LPARAM lp, LRESULT *result)
{
    if (!control || control->dead) return FALSE;
    control->references++;
    BOOL handled = control_message(control, window, message, wp, lp, result);
    if (--control->references == 0) kfree(control);
    return handled;
}
