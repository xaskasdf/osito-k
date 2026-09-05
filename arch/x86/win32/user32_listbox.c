/* Native LISTBOX storage, selection, and drawing. */
#include "user32_listbox.h"
#include "gdi32_shim.h"
#include "kernel32_shim.h"
#include "compat32.h"

extern void *kcalloc(uint64_t count, uint64_t size);
extern void kfree(void *pointer);
extern int WINAPI CompareStringOrdinal(PCWSTR, int, PCWSTR, int, BOOL);

#define LBS_NOTIFY 0x0001u
#define LBS_SORT 0x0002u
#define LBS_MULTIPLESEL 0x0008u
#define LBS_OWNERDRAWFIXED 0x0010u
#define LBS_OWNERDRAWVARIABLE 0x0020u
#define LBS_HASSTRINGS 0x0040u
#define LBS_USETABSTOPS 0x0080u
#define LBS_MULTICOLUMN 0x0200u
#define LBS_WANTKEYBOARDINPUT 0x0400u
#define LBS_EXTENDEDSEL 0x0800u
#define LBS_NODATA 0x2000u
#define LBS_NOSEL 0x4000u
#define LB_ERR (-1)
#define LB_ERRSPACE (-2)

enum {
    LB_ADDSTRING=0x180, LB_INSERTSTRING, LB_DELETESTRING,
    LB_SELITEMRANGEEX, LB_RESETCONTENT, LB_SETSEL, LB_SETCURSEL,
    LB_GETSEL, LB_GETCURSEL, LB_GETTEXT, LB_GETTEXTLEN, LB_GETCOUNT,
    LB_SELECTSTRING, LB_DIR, LB_GETTOPINDEX, LB_FINDSTRING,
    LB_GETSELCOUNT, LB_GETSELITEMS, LB_SETTABSTOPS, LB_GETHORIZONTALEXTENT,
    LB_SETHORIZONTALEXTENT, LB_SETCOLUMNWIDTH, LB_ADDFILE, LB_SETTOPINDEX,
    LB_GETITEMRECT, LB_GETITEMDATA, LB_SETITEMDATA, LB_SELITEMRANGE,
    LB_SETANCHORINDEX, LB_GETANCHORINDEX, LB_SETCARETINDEX, LB_GETCARETINDEX,
    LB_SETITEMHEIGHT, LB_GETITEMHEIGHT, LB_FINDSTRINGEXACT, LB_SETLOCALE,
    LB_GETLOCALE, LB_SETCOUNT, LB_INITSTORAGE, LB_ITEMFROMPOINT=0x1A9
};

typedef struct {
    PWSTR text;
    int length, height;
    ULONG_PTR data;
    BOOL selected;
} LIST_ITEM;

struct U32_LISTBOX {
    DWORD style, locale, search_time;
    UINT references;
    BOOL dead, closing, redraw, mouse_down, mouse_changed, drag_select;
    uint64_t mutation;
    LIST_ITEM *items;
    int count, capacity, selected, caret, anchor, top, height;
    int horizontal_extent, horizontal_pos, column_width, wheel;
    int *tabs;
    UINT tab_count;
    HFONT font;
    PWSTR search;
    int search_length;
};

static BOOL list_owner(const U32_LISTBOX *list)
{
    return (list->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) != 0;
}

BOOL user32_listbox_has_strings(const U32_LISTBOX *list)
{
    return list && !list->dead && (!list_owner(list) ||
        (list->style & LBS_HASSTRINGS)) && !(list->style & LBS_NODATA);
}

static BOOL list_multiple(const U32_LISTBOX *list)
{
    return (list->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0;
}

static BOOL list_valid(const U32_LISTBOX *list, int index)
{
    return index >= 0 && index < list->count;
}

U32_LISTBOX *user32_listbox_create(DWORD style)
{
    U32_LISTBOX *list = kcalloc(1, sizeof(*list));
    if (!list) return NULL;
    if (style & LBS_MULTICOLUMN) style &= ~LBS_OWNERDRAWVARIABLE;
    if (style & LBS_OWNERDRAWFIXED) style &= ~LBS_OWNERDRAWVARIABLE;
    list->style = style;
    list->references = 1;
    list->selected = list->anchor = -1;
    list->height = 16;
    list->column_width = 150;
    list->locale = 0x0409;
    list->redraw = TRUE;
    return list;
}

static void list_unref(U32_LISTBOX *list)
{
    if (--list->references) return;
    for (int i = 0; i < list->count; i++) kfree(list->items[i].text);
    kfree(list->items);
    kfree(list->tabs);
    kfree(list->search);
    kfree(list);
}

void user32_listbox_release(U32_LISTBOX *list)
{
    if (!list) return;
    list->dead = TRUE;
    list_unref(list);
}

static void list_redraw(U32_LISTBOX *list, HWND window)
{
    if (list->dead || list->closing || !list->redraw) return;
    InvalidateRect(window, NULL, TRUE);
    UpdateWindow(window);
}

static void list_notify(U32_LISTBOX *list, HWND window, UINT code)
{
    if (list->dead || list->closing || !(list->style & LBS_NOTIFY)) return;
    HWND parent = GetParent(window);
    if (parent) SendMessageA(parent, WM_COMMAND,
        ((UINT)GetDlgCtrlID(window) & 0xFFFFu) | ((WPARAM)(code & 0xFFFFu) << 16),
        (LPARAM)(ULONG_PTR)window);
}

static PWSTR list_copy_text(PCVOID source, BOOL wide, int *length)
{
    int count = 1;
    if (source) {
        if (wide) {
            while (((PCWSTR)source)[count-1]) {
                if (count == 0x7FFFFFFF) return NULL;
                count++;
            }
        } else count = MultiByteToWideChar(0, 0, source, -1, NULL, 0);
    }
    if (count <= 0) return NULL;
    PWSTR text = kcalloc((UINT)count, sizeof(WCHAR));
    if (!text) return NULL;
    if (source && wide)
        for (int i = 0; i < count; i++) text[i] = ((PCWSTR)source)[i];
    else if (source && !MultiByteToWideChar(0, 0, source, -1, text, count)) {
        kfree(text);
        return NULL;
    }
    *length = count-1;
    return text;
}

static BOOL list_reserve(U32_LISTBOX *list, int wanted)
{
    if (wanted < 0) return FALSE;
    if (wanted <= list->capacity) return TRUE;
    int capacity = list->capacity ? list->capacity : 16;
    while (capacity < wanted) {
        if (capacity > 0x3FFFFFFF) { capacity = wanted; break; }
        capacity *= 2;
    }
    LIST_ITEM *items = kcalloc((UINT)capacity, sizeof(*items));
    if (!items) return FALSE;
    for (int i = 0; i < list->count; i++) items[i] = list->items[i];
    kfree(list->items);
    list->items = items;
    list->capacity = capacity;
    return TRUE;
}

static PVOID list_payload(SIZE_T bytes)
{
    /* Callbacks execute in the application's ABI, including low PE32 pointers. */
    return HeapAlloc(GetProcessHeap(), 8, bytes);
}

static int list_measure(U32_LISTBOX *list, HWND window, int index, ULONG_PTR data)
{
    typedef struct { UINT type, id, item, width, height; ULONG_PTR data; } MEASURE;
    typedef struct { UINT type, id, item, width, height, data; } MEASURE32;
    _Static_assert(sizeof(MEASURE) == 32 && sizeof(MEASURE32) == 24, "MEASUREITEM ABI");
    BOOL narrow = g_compat32_mode;
    PVOID payload = list_payload(narrow ? sizeof(MEASURE32) : sizeof(MEASURE));
    if (!payload) return list->height;
    UINT id = (UINT)GetDlgCtrlID(window);
    if (narrow) *(MEASURE32 *)payload = (MEASURE32){2,id,(UINT)index,0,(UINT)list->height,(UINT)data};
    else *(MEASURE *)payload = (MEASURE){2,id,(UINT)index,0,(UINT)list->height,data};
    SendMessageA(GetParent(window), 0x002C, id, (LPARAM)(ULONG_PTR)payload);
    UINT height = narrow ? ((MEASURE32 *)payload)->height : ((MEASURE *)payload)->height;
    HeapFree(GetProcessHeap(), 0, payload);
    return height && height <= 255 ? (int)height : list->height;
}

static BOOL list_compare_owner(U32_LISTBOX *list, HWND window, int index,
                                ULONG_PTR data, int *answer)
{
    typedef struct {
        UINT type, id; HWND window; UINT first; ULONG_PTR first_data;
        UINT second; ULONG_PTR second_data; DWORD locale;
    } COMPARE;
    typedef struct { UINT type,id,window,first,first_data,second,second_data,locale; } COMPARE32;
    _Static_assert(sizeof(COMPARE) == 56 && sizeof(COMPARE32) == 32, "COMPAREITEM ABI");
    BOOL narrow = g_compat32_mode;
    PVOID payload = list_payload(narrow ? sizeof(COMPARE32) : sizeof(COMPARE));
    if (!payload) return FALSE;
    UINT id = (UINT)GetDlgCtrlID(window);
    ULONG_PTR existing = list->items[index].data;
    if (narrow) *(COMPARE32 *)payload = (COMPARE32){2,id,(UINT)(ULONG_PTR)window,
        (UINT)index,(UINT)existing,(UINT)-1,(UINT)data,list->locale};
    else *(COMPARE *)payload = (COMPARE){2,id,window,(UINT)index,existing,(UINT)-1,data,list->locale};
    uint64_t mutation = list->mutation;
    *answer = (int)SendMessageA(GetParent(window), 0x0039, id, (LPARAM)(ULONG_PTR)payload);
    HeapFree(GetProcessHeap(), 0, payload);
    return !list->dead && !list->closing && mutation == list->mutation;
}

static void list_delete_notice(U32_LISTBOX *list, HWND window, int index, ULONG_PTR data)
{
    typedef struct { UINT type,id,item; HWND window; ULONG_PTR data; } DELETE_ITEM;
    typedef struct { UINT type,id,item,window,data; } DELETE32;
    _Static_assert(sizeof(DELETE_ITEM) == 32 && sizeof(DELETE32) == 20, "DELETEITEM ABI");
    if (list->dead || !list_owner(list) || (list->style & LBS_NODATA)) return;
    BOOL narrow = g_compat32_mode;
    PVOID payload = list_payload(narrow ? sizeof(DELETE32) : sizeof(DELETE_ITEM));
    if (!payload) return;
    UINT id = (UINT)GetDlgCtrlID(window);
    if (narrow) *(DELETE32 *)payload = (DELETE32){2,id,(UINT)index,(UINT)(ULONG_PTR)window,(UINT)data};
    else *(DELETE_ITEM *)payload = (DELETE_ITEM){2,id,(UINT)index,window,data};
    SendMessageA(GetParent(window), 0x002D, id, (LPARAM)(ULONG_PTR)payload);
    HeapFree(GetProcessHeap(), 0, payload);
}

static int list_height(const U32_LISTBOX *list, int index)
{
    return (list->style & LBS_OWNERDRAWVARIABLE) && list_valid(list, index)
        ? list->items[index].height : list->height;
}

static int list_rows(const U32_LISTBOX *list, HWND window)
{
    RECT client = {0};
    GetClientRect(window, &client);
    int rows = client.bottom / list->height;
    return rows > 0 ? rows : 1;
}

static int list_last_top(const U32_LISTBOX *list, HWND window)
{
    if (!list->count) return 0;
    RECT client = {0};
    GetClientRect(window, &client);
    if (list->style & LBS_MULTICOLUMN) {
        int rows = list_rows(list, window);
        int columns = client.right / list->column_width;
        if (columns < 1) columns = 1;
        int last = ((list->count-1)/rows+1-columns)*rows;
        return last > 0 ? last : 0;
    }
    if (!(list->style & LBS_OWNERDRAWVARIABLE)) {
        int last = list->count-list_rows(list, window);
        return last > 0 ? last : 0;
    }
    int index = list->count-1, height = list_height(list, index);
    while (index > 0 && height + list_height(list, index-1) <= client.bottom)
        height += list_height(list, --index);
    return index;
}

static void list_clamp_top(U32_LISTBOX *list, HWND window)
{
    int last = list_last_top(list, window);
    if (list->top > last) list->top = last;
    if (list->top < 0) list->top = 0;
    if (list->style & LBS_MULTICOLUMN) list->top -= list->top % list_rows(list, window);
}

static LONG list_coordinate(int64_t value)
{
    return value > 0x7FFFFFFF ? 0x7FFFFFFF : value < (-0x7FFFFFFF-1LL) ?
        (-0x7FFFFFFF-1) : (LONG)value;
}

static RECT list_item_rect(const U32_LISTBOX *list, HWND window, int index)
{
    RECT rect = {0};
    GetClientRect(window, &rect);
    if (list->style & LBS_MULTICOLUMN) {
        int rows = list_rows(list, window);
        rect.left = list_coordinate((int64_t)(index/rows-list->top/rows)*list->column_width);
        rect.right = list_coordinate((int64_t)rect.left+list->column_width);
        rect.top = (index%rows)*list->height;
    } else {
        int64_t y = 0;
        if (list->style & LBS_OWNERDRAWVARIABLE) {
            for (int i = list->top; i < index; i++) y += list_height(list, i);
            for (int i = index; i < list->top; i++) y -= list_height(list, i);
        } else y = (int64_t)(index-list->top)*list->height;
        rect.top = list_coordinate(y);
    }
    rect.bottom = list_coordinate((int64_t)rect.top+list_height(list, index));
    return rect;
}

static void list_ensure_visible(U32_LISTBOX *list, HWND window, int index)
{
    if (!list_valid(list, index)) return;
    RECT client = {0};
    GetClientRect(window, &client);
    if (index < list->top) list->top = index;
    RECT item = list_item_rect(list, window, index);
    if (list->style & LBS_MULTICOLUMN) {
        if (item.right > client.right) {
            int columns = client.right/list->column_width;
            if (columns < 1) columns = 1;
            int rows = list_rows(list, window);
            list->top = (index/rows-columns+1)*rows;
        }
    } else if (item.bottom > client.bottom) {
        if (!(list->style & LBS_OWNERDRAWVARIABLE))
            list->top = index-list_rows(list, window)+1;
        else {
            list->top = index;
            int height = list_height(list, index);
            while (list->top > 0 && height + list_height(list, list->top-1) <= client.bottom)
                height += list_height(list, --list->top);
        }
    }
    list_clamp_top(list, window);
}

static int list_insert(U32_LISTBOX *list, HWND window, int index,
                       LPARAM value, BOOL wide, BOOL sort)
{
    if (list->closing || (list->style & LBS_NODATA)) return LB_ERR;
    if (index == -1 || index > list->count) index = list->count;
    if (index < 0) return LB_ERR;
    LIST_ITEM item = {.height = list->height};
    if (user32_listbox_has_strings(list)) {
        item.text = list_copy_text((PCVOID)value, wide, &item.length);
        if (!item.text) goto no_space;
    } else item.data = g_compat32_mode ? (ULONG_PTR)(UINT)value : (ULONG_PTR)value;
    if (sort && (list->style & LBS_SORT)) {
        int low = 0, high = list->count;
        while (low < high) {
            int middle = low+(high-low)/2, order;
            if (item.text) order = CompareStringOrdinal(list->items[middle].text,
                list->items[middle].length, item.text, item.length, TRUE)-2;
            else if (!list_compare_owner(list, window, middle, item.data, &order)) {
                kfree(item.text);
                return LB_ERR;
            }
            if (order <= 0) low = middle+1;
            else high = middle;
        }
        index = low;
    }
    if (list->count == 0x7FFFFFFF || !list_reserve(list, list->count+1)) goto no_space;
    for (int i = list->count; i > index; i--) list->items[i] = list->items[i-1];
    list->items[index] = item;
    if (list->selected >= index) list->selected++;
    if (list->anchor >= index) list->anchor++;
    if (list->count && list->caret >= index) list->caret++;
    list->count++;
    uint64_t mutation = ++list->mutation;
    if (list->style & LBS_OWNERDRAWVARIABLE) {
        int height = list_measure(list, window, index, item.data);
        if (list->dead || list->closing || mutation != list->mutation) return LB_ERR;
        list->items[index].height = height;
    }
    list_redraw(list, window);
    return index;
no_space:
    kfree(item.text);
    list_notify(list, window, (UINT)-2);
    return LB_ERRSPACE;
}

static void list_reset(U32_LISTBOX *list, HWND window)
{
    /* Detach before notifying: deletion callbacks may reset, add, or destroy. */
    LIST_ITEM *items = list->items;
    int count = list->count;
    list->items = NULL;
    list->count = list->capacity = list->top = list->caret = 0;
    list->selected = list->anchor = -1;
    list->mutation++;
    for (int i = 0; i < count; i++) {
        list_delete_notice(list, window, i, items[i].data);
        kfree(items[i].text);
    }
    kfree(items);
    list_redraw(list, window);
}

static int list_delete(U32_LISTBOX *list, HWND window, int index)
{
    if (!list_valid(list, index)) return LB_ERR;
    LIST_ITEM item = list->items[index];
    for (int i = index; i+1 < list->count; i++) list->items[i] = list->items[i+1];
    list->count--;
    if (list->selected == index) list->selected = -1;
    else if (list->selected > index) list->selected--;
    if (list->anchor >= index) list->anchor--;
    if (list->caret > index || list->caret == list->count) list->caret--;
    if (list->caret < 0) list->caret = 0;
    list_clamp_top(list, window);
    list->mutation++;
    list_delete_notice(list, window, index, item.data);
    kfree(item.text);
    list_redraw(list, window);
    return list->count;
}

static int list_find(U32_LISTBOX *list, int start, LPARAM value, BOOL wide, BOOL exact)
{
    if (!list->count) return LB_ERR;
    if (start < -1 || start >= list->count) start = -1;
    int length = 0, found = LB_ERR;
    PWSTR text = user32_listbox_has_strings(list) ?
        list_copy_text((PCVOID)value, wide, &length) : NULL;
    if (user32_listbox_has_strings(list) && !text) return LB_ERRSPACE;
    int index = start;
    for (int n = 0; n < list->count; n++) {
        if (++index == list->count) index = 0;
        LIST_ITEM *item = &list->items[index];
        if (text ? (item->length >= length && (!exact || item->length == length) &&
            CompareStringOrdinal(item->text, length, text, length, TRUE) == 2) :
            item->data == (g_compat32_mode ? (ULONG_PTR)(UINT)value : (ULONG_PTR)value)) {
            found = index;
            break;
        }
    }
    kfree(text);
    return found;
}

static void list_draw_item(U32_LISTBOX *list, HWND window, HDC dc, int index,
                            RECT rect, BOOL selected)
{
    typedef struct {
        UINT type,id,item,action,state; HWND window; HDC dc; RECT rect; ULONG_PTR data;
    } DRAW;
    typedef struct { UINT type,id,item,action,state,window,dc; RECT rect; UINT data; } DRAW32;
    _Static_assert(sizeof(DRAW) == 64 && sizeof(DRAW32) == 48, "DRAWITEM ABI");
    BOOL narrow = g_compat32_mode;
    PVOID payload = list_payload(narrow ? sizeof(DRAW32) : sizeof(DRAW));
    if (!payload) return;
    UINT id = (UINT)GetDlgCtrlID(window);
    UINT state = (selected ? 1 : 0) | (GetFocus() == window && list->caret == index ? 16 : 0) |
        (((DWORD)GetWindowLongPtrA(window, -16) & WS_DISABLED) ? 4 : 0);
    ULONG_PTR data = list_valid(list, index) ? list->items[index].data : 0;
    if (narrow) *(DRAW32 *)payload = (DRAW32){2,id,(UINT)index,1,state,
        (UINT)(ULONG_PTR)window,(UINT)(ULONG_PTR)dc,rect,(UINT)data};
    else *(DRAW *)payload = (DRAW){2,id,(UINT)index,1,state,window,dc,rect,data};
    SendMessageA(GetParent(window), 0x002B, id, (LPARAM)(ULONG_PTR)payload);
    HeapFree(GetProcessHeap(), 0, payload);
}

static void list_text(U32_LISTBOX *list, HDC dc, RECT rect, const LIST_ITEM *item)
{
    int x = rect.left+2-list->horizontal_pos;
    if (!(list->style & LBS_USETABSTOPS)) {
        ExtTextOutW(dc, x, rect.top, 4, (const GDI_RECT *)&rect,
            item->text, (UINT)item->length, NULL);
        return;
    }
    int begin = 0;
    for (int i = 0; i <= item->length; i++) {
        if (i < item->length && item->text[i] != '\t') continue;
        POINT extent = {0};
        GetTextExtentPoint32W(dc, item->text+begin, i-begin, &extent);
        ExtTextOutW(dc, x, rect.top, 4, (const GDI_RECT *)&rect,
            item->text+begin, (UINT)(i-begin), NULL);
        x = list_coordinate((int64_t)x+extent.x);
        int relative = x-rect.left+list->horizontal_pos;
        int stop = 0;
        for (UINT tab = 0; tab < list->tab_count; tab++) {
            int pixels = list_coordinate((int64_t)list->tabs[tab]*2);
            if (pixels > relative) { stop = pixels; break; }
        }
        if (!stop) {
            int interval = list->tab_count == 1 ? list->tabs[0]*2 : 64;
            if (interval < 1) interval = 64;
            stop = list_coordinate(((int64_t)relative/interval+1)*interval);
        }
        x = list_coordinate((int64_t)rect.left+stop-list->horizontal_pos);
        begin = i+1;
    }
}

static void list_paint(U32_LISTBOX *list, HWND window, HDC dc)
{
    RECT client;
    uint64_t token;
    if (!dc || list->dead || !GetClientRect(window, &client) ||
        !gdi32_push_paint_clip(dc, (const GDI_RECT *)&client, &token)) return;
    DWORD text = GetTextColor(dc), background = GetBkColor(dc);
    UINT alignment = SetTextAlign(dc, 0);
    int mode = SetBkMode(dc, 1);
    HGDIOBJ font = SelectObject(dc, list->font ? list->font : GetStockObject(17));
    SetTextColor(dc, GetSysColor(8));
    SetBkColor(dc, GetSysColor(5));
    BOOL narrow = g_compat32_mode;
    LRESULT answer = SendMessageA(GetParent(window), 0x0134,
        (WPARAM)(ULONG_PTR)dc, (LPARAM)(ULONG_PTR)window);
    if (list->dead) goto restore;
    HBRUSH brush = (HBRUSH)(narrow ? (ULONG_PTR)(UINT)answer : (ULONG_PTR)answer);
    if (!brush) brush = (HBRUSH)(ULONG_PTR)6;
    FillRect(dc, &client, brush);
    DWORD normal_text = GetTextColor(dc), normal_background = GetBkColor(dc);
    BOOL disabled = ((DWORD)GetWindowLongPtrA(window, -16) & WS_DISABLED) != 0;
    uint64_t mutation = list->mutation;
    for (int i = list->top; i < list->count && !list->dead; i++) {
        RECT rect = list_item_rect(list, window, i);
        if (list->style & LBS_MULTICOLUMN) {
            if (rect.left >= client.right) break;
        } else if (rect.top >= client.bottom) break;
        BOOL selected = !(list->style & LBS_NOSEL) &&
            (list_multiple(list) ? list->items[i].selected : list->selected == i);
        if (list_owner(list)) list_draw_item(list, window, dc, i, rect, selected);
        else {
            if (selected) FillRect(dc, &rect, (HBRUSH)(ULONG_PTR)14);
            SetTextColor(dc, GetSysColor(disabled ? 17 : selected ? 14 : 8));
            if (!disabled && !selected) SetTextColor(dc, normal_text);
            SetBkColor(dc, selected ? GetSysColor(13) : normal_background);
            list_text(list, dc, rect, &list->items[i]);
            if (GetFocus() == window && list->caret == i && !(list->style & LBS_NOSEL))
                DrawFocusRect(dc, &rect);
        }
        if (mutation != list->mutation) {
            if (!list->dead) InvalidateRect(window, NULL, TRUE);
            break;
        }
    }
restore:
    SelectObject(dc, font);
    SetTextColor(dc, text); SetBkColor(dc, background);
    SetTextAlign(dc, alignment); SetBkMode(dc, mode);
    gdi32_pop_paint_clip(dc, token, TRUE);
}

static int list_point(U32_LISTBOX *list, HWND window, int x, int y, BOOL *outside)
{
    RECT client = {0};
    GetClientRect(window, &client);
    *outside = x < 0 || y < 0 || x >= client.right || y >= client.bottom;
    if (!list->count) return -1;
    int64_t index = list->top;
    if (list->style & LBS_MULTICOLUMN) {
        int rows = list_rows(list, window);
        index += (int64_t)(x > 0 ? x/list->column_width : 0)*rows;
        int row = y > 0 ? y/list->height : 0;
        index += row < rows ? row : rows-1;
    } else if (list->style & LBS_OWNERDRAWVARIABLE) {
        int64_t bottom = list_height(list, (int)index);
        while (index+1 < list->count && y >= bottom)
            bottom += list_height(list, (int)++index);
    } else index += y > 0 ? y/list->height : 0;
    if (index >= list->count) index = list->count-1;
    return (int)index;
}

static BOOL list_range(U32_LISTBOX *list, int first, int last, BOOL selected)
{
    BOOL changed = FALSE;
    if (first > last) { int swap = first; first = last; last = swap; }
    if (first < 0) first = 0;
    if (last >= list->count) last = list->count-1;
    for (int i = first; i <= last; i++) {
        changed |= list->items[i].selected != selected;
        list->items[i].selected = selected;
    }
    return changed;
}

static BOOL list_user_select(U32_LISTBOX *list, HWND window, int index,
                              BOOL shift, BOOL control, BOOL toggle)
{
    if (!list_valid(list, index) || (list->style & LBS_NOSEL)) return FALSE;
    BOOL changed = FALSE;
    if (!list_multiple(list)) {
        changed = list->selected != index;
        list->selected = index;
    } else if ((list->style & LBS_EXTENDEDSEL) && shift) {
        if (list->anchor < 0) list->anchor = list->caret;
        if (!control) changed |= list_range(list, 0, list->count-1, FALSE);
        changed |= list_range(list, list->anchor, index, TRUE);
    } else if ((list->style & LBS_MULTIPLESEL) || control) {
        if (toggle) {
            list->items[index].selected = !list->items[index].selected;
            changed = TRUE;
        }
        list->anchor = index;
    } else {
        for (int i = 0; i < list->count; i++) {
            BOOL selected = i == index;
            changed |= list->items[i].selected != selected;
            list->items[i].selected = selected;
        }
        list->anchor = index;
    }
    list->caret = index;
    list_ensure_visible(list, window, index);
    list_redraw(list, window);
    return changed;
}

static void list_key(U32_LISTBOX *list, HWND window, UINT key)
{
    if (!list->count || (list->style & LBS_NOSEL)) return;
    if (list->style & LBS_WANTKEYBOARDINPUT) {
        int handled = (int)SendMessageA(GetParent(window), 0x002E,
            (key & 0xFFFFu) | ((WPARAM)(UINT)list->caret << 16), (LPARAM)(ULONG_PTR)window);
        if (list->dead || handled == -2) return;
        if (handled >= 0) {
            if (list_user_select(list, window, handled, FALSE, FALSE, FALSE))
                list_notify(list, window, 1);
            return;
        }
    }
    int next = list->caret, page = list_rows(list, window);
    BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    BOOL control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (key == VK_HOME) next = 0;
    else if (key == VK_END) next = list->count-1;
    else if (key == VK_UP) next--;
    else if (key == VK_DOWN) next++;
    else if (key == VK_PRIOR) next -= page;
    else if (key == VK_NEXT) next = list_coordinate((int64_t)next+page);
    else if (key == VK_LEFT) next -= (list->style & LBS_MULTICOLUMN) ? page : 1;
    else if (key == VK_RIGHT) next = list_coordinate((int64_t)next+
        ((list->style & LBS_MULTICOLUMN) ? page : 1));
    else if (key != VK_SPACE) return;
    if (next < 0) next = 0;
    if (next >= list->count) next = list->count-1;
    if (list_multiple(list) && key != VK_SPACE &&
        ((list->style & LBS_MULTIPLESEL) || (control && !shift))) {
        list->caret = next;
        list_ensure_visible(list, window, next);
        list_redraw(list, window);
    } else if (list_user_select(list, window, next, shift, control, key == VK_SPACE))
        list_notify(list, window, 1);
}

static void list_character(U32_LISTBOX *list, HWND window, WCHAR character)
{
    if (character < 32 || !user32_listbox_has_strings(list) || (list->style & LBS_NOSEL)) return;
    if (list->style & LBS_WANTKEYBOARDINPUT) {
        int handled = (int)SendMessageA(GetParent(window), 0x002F,
            character | ((WPARAM)(UINT)list->caret << 16), (LPARAM)(ULONG_PTR)window);
        if (list->dead || handled == -2) return;
        if (handled >= 0) {
            if (list_user_select(list, window, handled, FALSE, FALSE, FALSE))
                list_notify(list, window, 1);
            return;
        }
    }
    DWORD now = GetTickCount();
    int length = (DWORD)(now-list->search_time) <= 1000 ? list->search_length : 0;
    if (length >= 0x7FFFFFFD) return;
    PWSTR search = kcalloc((UINT)length+2, sizeof(WCHAR));
    if (!search) { list_notify(list, window, (UINT)-2); return; }
    for (int i = 0; i < length; i++) search[i] = list->search[i];
    search[length] = character;
    int found = list_find(list, length ? list->caret-1 : list->caret,
        (LPARAM)(ULONG_PTR)search, TRUE, FALSE);
    if (found < 0 && length) {
        search[0] = character; search[1] = 0; length = 0;
        found = list_find(list, list->caret, (LPARAM)(ULONG_PTR)search, TRUE, FALSE);
    }
    kfree(list->search);
    list->search = search; list->search_length = length+1; list->search_time = now;
    if (found >= 0 && list_user_select(list, window, found, FALSE, FALSE, FALSE))
        list_notify(list, window, 1);
}

static BOOL list_message(U32_LISTBOX *list, HWND window, DWORD message,
                          WPARAM wp, LPARAM lp, BOOL wide, LRESULT *result)
{
    int index = (int)(UINT)wp;
    *result = 0;
    if (list->closing && message >= LB_ADDSTRING && message <= LB_ITEMFROMPOINT) {
        *result = LB_ERR;
        return TRUE;
    }
    switch (message) {
    case WM_CREATE:
        if (list->style & LBS_OWNERDRAWFIXED)
            list->height = list_measure(list, window, 0, 0);
        return TRUE;
    case WM_DESTROY:
        list->closing = TRUE;
        list_reset(list, window);
        return TRUE;
    case 0x0031: *result = (LRESULT)(ULONG_PTR)list->font; return TRUE;
    case 0x0030: {
        list->font = (HFONT)(ULONG_PTR)wp;
        if (!list_owner(list)) {
            HDC dc = GetDC(window);
            HGDIOBJ previous = SelectObject(dc, list->font ? list->font : GetStockObject(17));
            BYTE metrics[64] = {0};
            if (GetTextMetricsW(dc, metrics) && *(LONG *)metrics > 0 && *(LONG *)metrics <= 255)
                list->height = *(LONG *)metrics;
            SelectObject(dc, previous); ReleaseDC(window, dc);
            list_clamp_top(list, window);
        }
        if (lp) list_redraw(list, window);
        return TRUE;
    }
    case 0x0087: *result = 0x81; return TRUE; /* DLGC_WANTARROWS | DLGC_WANTCHARS */
    case 0x000B:
        list->redraw = wp != 0;
        if (wp) list_redraw(list, window);
        return TRUE;
    case WM_PAINT: {
        BYTE paint[72];
        HDC dc = wp ? (HDC)(ULONG_PTR)wp : BeginPaint(window, paint);
        if (list->redraw) list_paint(list, window, dc);
        if (dc && !wp) EndPaint(window, paint);
        return TRUE;
    }
    case 0x0318: list_paint(list, window, (HDC)(ULONG_PTR)wp); return TRUE;
    case WM_ERASEBKGND: *result = 1; return TRUE;
    case WM_SIZE:
        list_clamp_top(list, window); list_redraw(list, window); return TRUE;
    case WM_ENABLE:
        if (!wp && GetCapture() == window) ReleaseCapture();
        list_redraw(list, window); return TRUE;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        list_redraw(list, window);
        list_notify(list, window, message == WM_SETFOCUS ? 4 : 5);
        return TRUE;
    case LB_INITSTORAGE:
        *result = index < 0 || !list_reserve(list, index) ? LB_ERRSPACE : list->capacity;
        return TRUE;
    case LB_ADDSTRING: case LB_INSERTSTRING:
        *result = list_insert(list, window, message == LB_ADDSTRING ? -1 : index,
            lp, wide, message == LB_ADDSTRING); return TRUE;
    case LB_DELETESTRING: *result = list_delete(list, window, index); return TRUE;
    case LB_RESETCONTENT: list_reset(list, window); return TRUE;
    case LB_GETCOUNT: *result = list->count; return TRUE;
    case LB_GETTEXT: case LB_GETTEXTLEN: {
        if (!list_valid(list, index)) { *result = LB_ERR; return TRUE; }
        LIST_ITEM *item = &list->items[index];
        if (!user32_listbox_has_strings(list)) {
            *result = g_compat32_mode ? 4 : sizeof(ULONG_PTR);
            if (message == LB_GETTEXT && lp) {
                if (g_compat32_mode) *(UINT *)(ULONG_PTR)lp = (UINT)item->data;
                else *(ULONG_PTR *)(ULONG_PTR)lp = item->data;
            }
        } else if (wide) {
            *result = item->length;
            if (message == LB_GETTEXT && lp)
                for (int i = 0; i <= item->length; i++) ((PWSTR)(ULONG_PTR)lp)[i] = item->text[i];
        } else {
            int bytes = WideCharToMultiByte(0, 0, item->text, -1, NULL, 0, NULL, NULL);
            *result = bytes > 0 ? bytes-1 : LB_ERR;
            if (message == LB_GETTEXT && lp && bytes > 0)
                WideCharToMultiByte(0, 0, item->text, -1, (PSTR)(ULONG_PTR)lp, bytes, NULL, NULL);
        }
        return TRUE;
    }
    case LB_GETITEMDATA:
        *result = list_valid(list, index) ? (LRESULT)list->items[index].data : LB_ERR;
        return TRUE;
    case LB_SETITEMDATA:
        if (!list_valid(list, index) || (list->style & LBS_NODATA)) *result = LB_ERR;
        else { list->items[index].data = g_compat32_mode ? (ULONG_PTR)(UINT)lp : (ULONG_PTR)lp; list->mutation++; }
        return TRUE;
    case LB_GETCURSEL:
        *result = list_multiple(list) ? list->caret : list->selected; return TRUE;
    case LB_SETCURSEL:
        if (list_multiple(list)) { *result = LB_ERR; return TRUE; }
        list->selected = list_valid(list, index) ? index : -1;
        *result = list->selected;
        if (list->selected >= 0) {
            list->caret = index; list_ensure_visible(list, window, index);
        }
        list_redraw(list, window); return TRUE;
    case LB_GETSEL:
        *result = !list_valid(list, index) ? LB_ERR : list_multiple(list) ?
            list->items[index].selected : list->selected == index;
        return TRUE;
    case LB_SETSEL: {
        int item = (int)(UINT)lp;
        if (!list_multiple(list) || (item != -1 && !list_valid(list, item))) *result = LB_ERR;
        else {
            if (item == -1) list_range(list, 0, list->count-1, wp != 0);
            else { list->items[item].selected = wp != 0; list_ensure_visible(list, window, item); }
            list_redraw(list, window);
        }
        return TRUE;
    }
    case LB_GETSELCOUNT: case LB_GETSELITEMS: {
        if (!list_multiple(list) || (message == LB_GETSELITEMS && (index < 0 || (index && !lp)))) {
            *result = LB_ERR; return TRUE;
        }
        int count = 0;
        for (int i = 0; i < list->count; i++) if (list->items[i].selected) {
            if (message == LB_GETSELITEMS) {
                if (count >= index) break;
                ((int *)(ULONG_PTR)lp)[count] = i;
            }
            count++;
        }
        *result = count; return TRUE;
    }
    case LB_SELITEMRANGE: case LB_SELITEMRANGEEX: {
        int first = message == LB_SELITEMRANGE ? (UINT)lp & 0xFFFFu : index;
        int last = message == LB_SELITEMRANGE ? ((UINT)lp >> 16) : (int)(UINT)lp;
        if (!list_multiple(list)) *result = LB_ERR;
        else {
            list_range(list, first, last, message == LB_SELITEMRANGE ? wp != 0 : first <= last);
            list_redraw(list, window);
        }
        return TRUE;
    }
    case LB_GETCARETINDEX: *result = list->caret; return TRUE;
    case LB_GETANCHORINDEX: *result = list->anchor; return TRUE;
    case LB_SETCARETINDEX: case LB_SETANCHORINDEX:
        if (!list_valid(list, index)) *result = LB_ERR;
        else {
            if (message == LB_SETANCHORINDEX) list->anchor = index;
            else { list->caret = index; list_ensure_visible(list, window, index); list_redraw(list, window); }
        }
        return TRUE;
    case LB_FINDSTRING: case LB_FINDSTRINGEXACT: case LB_SELECTSTRING:
        *result = list_find(list, index, lp, wide, message == LB_FINDSTRINGEXACT);
        if (message == LB_SELECTSTRING && *result >= 0) {
            if (list_multiple(list)) list->items[*result].selected = TRUE;
            else list->selected = (int)*result;
            list->caret = (int)*result;
            list_ensure_visible(list, window, (int)*result); list_redraw(list, window);
        }
        return TRUE;
    case LB_GETTOPINDEX: *result = list->top; return TRUE;
    case LB_SETTOPINDEX:
        if (index >= 0 && index < list->count) {
            list->top = index; list_clamp_top(list, window); list_redraw(list, window);
        }
        return TRUE;
    case LB_GETITEMRECT:
        if (!lp || !list_valid(list, index)) *result = LB_ERR;
        else *(RECT *)(ULONG_PTR)lp = list_item_rect(list, window, index);
        return TRUE;
    case LB_ITEMFROMPOINT: {
        BOOL outside;
        int item = list_point(list, window, (short)(UINT)lp, (short)((UINT)lp >> 16), &outside);
        *result = ((UINT)item & 0xFFFFu) | (outside ? 0x10000 : 0); return TRUE;
    }
    case LB_SETITEMHEIGHT:
        if ((UINT)lp > 255 || !lp || ((list->style & LBS_OWNERDRAWVARIABLE) && !list_valid(list, index)))
            *result = LB_ERR;
        else {
            if (list->style & LBS_OWNERDRAWVARIABLE) list->items[index].height = (UINT)lp;
            else list->height = (UINT)lp;
            list_clamp_top(list, window); list_redraw(list, window);
        }
        return TRUE;
    case LB_GETITEMHEIGHT:
        *result = (list->style & LBS_OWNERDRAWVARIABLE) && !list_valid(list, index) ?
            LB_ERR : list_height(list, index); return TRUE;
    case LB_GETHORIZONTALEXTENT: *result = list->horizontal_extent; return TRUE;
    case LB_SETHORIZONTALEXTENT:
        list->horizontal_extent = index > 0 ? index : 0;
        if (list->horizontal_pos > list->horizontal_extent) list->horizontal_pos = list->horizontal_extent;
        list_redraw(list, window); return TRUE;
    case LB_SETCOLUMNWIDTH:
        if (index > 0) { list->column_width = index; list_clamp_top(list, window); list_redraw(list, window); }
        return TRUE;
    case LB_SETTABSTOPS: {
        if (!(list->style & LBS_USETABSTOPS)) return TRUE;
        if (index < 0 || (index && !lp)) return TRUE;
        int *tabs = index ? kcalloc((UINT)index, sizeof(int)) : NULL;
        if (index && !tabs) return TRUE;
        for (int i = 0; i < index; i++) {
            int value = ((int *)(ULONG_PTR)lp)[i];
            if (value <= 0 || value > 0x3FFFFFFF || (i && value <= tabs[i-1])) { kfree(tabs); return TRUE; }
            tabs[i] = value;
        }
        kfree(list->tabs); list->tabs = tabs; list->tab_count = index;
        *result = TRUE; return TRUE;
    }
    case LB_GETLOCALE: *result = list->locale; return TRUE;
    case LB_SETLOCALE:
        *result = list->locale; list->locale = (DWORD)wp; return TRUE;
    case LB_SETCOUNT:
        if (!(list->style & LBS_NODATA) || (list->style & LBS_HASSTRINGS) || index < 0) *result = LB_ERR;
        else if (!list_reserve(list, index)) *result = LB_ERRSPACE;
        else {
            for (int i = list->count; i < index; i++) list->items[i] = (LIST_ITEM){0};
            list->count = index;
            if (list->selected >= index) list->selected = -1;
            if (list->caret >= index) list->caret = index > 0 ? index-1 : 0;
            list->mutation++; list_clamp_top(list, window); list_redraw(list, window);
        }
        return TRUE;
    case LB_DIR: case LB_ADDFILE:
        SetLastError(120); *result = LB_ERR; return TRUE;
    case WM_CAPTURECHANGED: case 0x001F:
        list->mouse_down = list->mouse_changed = FALSE;
        if (message == 0x001F && GetCapture() == window) ReleaseCapture();
        return TRUE;
    }
    if ((DWORD)GetWindowLongPtrA(window, -16) & WS_DISABLED) return FALSE;
    switch (message) {
    case WM_KEYDOWN: list_key(list, window, (UINT)wp); return TRUE;
    case WM_CHAR: {
        WCHAR character = (WCHAR)wp;
        if (!wide) { char byte = (char)wp; MultiByteToWideChar(0, 0, &byte, 1, &character, 1); }
        list_character(list, window, character); return TRUE;
    }
    case WM_LBUTTONDOWN: case 0x0203: {
        SetFocus(window);
        if (list->dead) return TRUE;
        BOOL outside;
        int item = list_point(list, window, (short)(UINT)lp, (short)((UINT)lp >> 16), &outside);
        if (outside || item < 0 || (list->style & LBS_NOSEL)) return TRUE;
        SetCapture(window);
        if (list->dead) return TRUE;
        list->mouse_down = TRUE;
        list->mouse_changed = list_user_select(list, window, item, (wp & 4) != 0, (wp & 8) != 0, TRUE);
        if (!list->dead && list_valid(list, item)) list->drag_select = list->items[item].selected;
        if (message == 0x0203) list_notify(list, window, 2);
        return TRUE;
    }
    case WM_MOUSEMOVE:
        if (list->mouse_down && GetCapture() == window) {
            BOOL outside;
            int item = list_point(list, window, (short)(UINT)lp, (short)((UINT)lp >> 16), &outside);
            if (!outside && list_valid(list, item) && item != list->caret) {
                if (list->style & LBS_MULTIPLESEL) {
                    list->mouse_changed |= list->items[item].selected != list->drag_select;
                    list->items[item].selected = list->drag_select; list->caret = item;
                    list_redraw(list, window);
                } else list->mouse_changed |= list_user_select(list, window, item,
                    (list->style & LBS_EXTENDEDSEL) != 0, (wp & 8) != 0, FALSE);
            }
        }
        return TRUE;
    case WM_LBUTTONUP: {
        BOOL changed = list->mouse_down && list->mouse_changed;
        list->mouse_down = list->mouse_changed = FALSE;
        if (GetCapture() == window) ReleaseCapture();
        if (changed) list_notify(list, window, 1);
        return TRUE;
    }
    case 0x0115: case 0x0114: {
        int step = message == 0x0114 && (list->style & LBS_MULTICOLUMN) ? list_rows(list, window) : 1;
        int page = list_rows(list, window);
        BOOL horizontal = message == 0x0114 && !(list->style & LBS_MULTICOLUMN);
        int position = horizontal ? list->horizontal_pos : list->top;
        int last = horizontal ? list->horizontal_extent : list_last_top(list, window);
        switch ((UINT)wp & 0xFFFFu) {
        case 0: position -= step; break;
        case 1: position = list_coordinate((int64_t)position+step); break;
        case 2: position -= page; break;
        case 3: position = list_coordinate((int64_t)position+page); break;
        case 4: case 5: position = ((UINT)wp >> 16); break;
        case 6: position = 0; break;
        case 7: position = last; break;
        }
        if (position < 0) position = 0;
        if (position > last) position = last;
        if (horizontal) list->horizontal_pos = position;
        else list->top = position;
        list_redraw(list, window); return TRUE;
    }
    case 0x020A: {
        UINT lines = 3;
        SystemParametersInfoW(0x0068, 0, &lines, 0);
        list->wheel += (short)((UINT)wp >> 16);
        int steps = list->wheel / 120;
        list->wheel %= 120;
        int amount = lines == (UINT)-1 ? list_rows(list, window) :
            lines > 0x7FFFFFFF ? 0x7FFFFFFF : (int)lines;
        list->top = list_coordinate((int64_t)list->top-(int64_t)steps*amount);
        list_clamp_top(list, window); list_redraw(list, window); return TRUE;
    }
    }
    return FALSE;
}

BOOL user32_listbox_message(U32_LISTBOX *list, HWND window, DWORD message,
                            WPARAM wp, LPARAM lp, BOOL wide, LRESULT *result)
{
    if (!list || list->dead) return FALSE;
    list->references++;
    BOOL handled = list_message(list, window, message, wp, lp, wide, result);
    list_unref(list);
    return handled;
}
