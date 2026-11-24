/* USER32 text layout over the selected GDI font and ordinary DC clipping. */
#include "user32_shim.h"
#include "gdi32_shim.h"
#include "kernel32_shim.h"

extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);

#define DT_CENTER          0x000001u
#define DT_RIGHT           0x000002u
#define DT_VCENTER         0x000004u
#define DT_BOTTOM          0x000008u
#define DT_WORDBREAK       0x000010u
#define DT_SINGLELINE      0x000020u
#define DT_EXPANDTABS      0x000040u
#define DT_TABSTOP         0x000080u
#define DT_NOCLIP          0x000100u
#define DT_EXTERNALLEADING 0x000200u
#define DT_CALCRECT        0x000400u
#define DT_NOPREFIX        0x000800u
#define DT_EDITCONTROL     0x002000u
#define DT_PATH_ELLIPSIS   0x004000u
#define DT_END_ELLIPSIS    0x008000u
#define DT_MODIFYSTRING    0x010000u
#define DT_WORD_ELLIPSIS   0x040000u
#define DT_HIDEPREFIX      0x100000u
#define DT_PREFIXONLY      0x200000u

typedef struct {
    UINT size;
    int tab_length, left_margin, right_margin;
    UINT length_drawn;
} U32_DRAWTEXTPARAMS;

typedef struct {
    WCHAR character;
    BOOL underline;
    int advance;
    int source_end;
} U32_TEXT_CHAR;

_Static_assert(sizeof(U32_DRAWTEXTPARAMS) == 20, "DRAWTEXTPARAMS ABI");
_Static_assert(sizeof(GDI_TEXTMETRICW) == 60, "TEXTMETRICW ABI");

static BOOL text_coordinate(int64_t value)
{
    return value >= -2147483648LL && value <= 2147483647LL;
}

static int text_length(PCWSTR text, int count)
{
    if (count < -1 || (!text && count != 0)) return -1;
    if (count != -1) return count;
    int length = 0;
    while (text[length]) {
        if (length == 0x7FFFFFFE) return -1;
        length++;
    }
    return length;
}

static int text_prepare(HDC dc, PCWSTR text, int count, UINT flags,
                          U32_TEXT_CHAR *chars)
{
    int used = 0;
    BOOL underline = FALSE;
    for (int i = 0; i < count; i++) {
        WCHAR ch = text[i];
        if (!ch) break;
        if (!(flags & DT_NOPREFIX) && ch == '&') {
            if (i + 1 < count && text[i+1] == '&') i++;
            else { underline = TRUE; continue; }
        }
        if (ch == '\r' || ch == '\n') {
            if (flags & DT_SINGLELINE) continue;
            if (ch == '\r' && i + 1 < count && text[i+1] == '\n') i++;
            ch = '\n';
        }
        if (ch == '\t' && !(flags & DT_EXPANDTABS)) continue;
        LONG size[2] = {0, 0};
        if (ch != '\n' && ch != '\t' &&
            !GetTextExtentPoint32W(dc, &ch, 1, size)) return -1;
        chars[used++] = (U32_TEXT_CHAR){ch, underline, size[0], i+1};
        underline = FALSE;
    }
    return used;
}

static BOOL text_space(WCHAR ch)
{
    return ch == ' ' || ch == '\t';
}

/* Return a whole word even when it is wider than the formatting rectangle. */
static int text_line(U32_TEXT_CHAR *chars, int count, int start, UINT flags,
                       int64_t available, int tab_width, int *next,
                       BOOL *newline, int64_t *width)
{
    int end = start, word_break = -1;
    int64_t before_break = 0, used = 0;
    while (end < count && chars[end].character != '\n') {
        WCHAR ch = chars[end].character;
        if (text_space(ch) && (end == start ||
                              !text_space(chars[end-1].character))) {
            word_break = end;
            before_break = used;
        }
        if (ch == '\t') chars[end].advance = tab_width > 0
            ? tab_width - (int)(used % tab_width) : 0;
        int64_t extended = used + chars[end].advance;
        if ((flags & DT_WORDBREAK) && !(flags & DT_SINGLELINE) &&
            extended > available && word_break > start) {
            end = word_break + 1;
            used = before_break + chars[word_break].advance;
            break;
        }
        used = extended;
        end++;
    }
    int consumed = end;
    if (end < count && text_space(chars[end].character) &&
        (flags & DT_WORDBREAK) && !(flags & DT_SINGLELINE))
        while (consumed < count && text_space(chars[consumed].character)) consumed++;
    *newline = consumed < count && chars[consumed].character == '\n';
    if (*newline) consumed++;
    *next = consumed;
    *width = used;
    return end;
}

static int text_ellipsis(U32_TEXT_CHAR *chars, int count, UINT flags,
                           int dot_width, int64_t available, int64_t *width,
                           int *kept_prefix, int *kept_suffix)
{
    *kept_prefix = *kept_suffix = count;
    if (*width <= available) return count;
    int suffix = count, prefix = count;
    int64_t suffix_width = 0, prefix_width = *width;
    if (flags & DT_PATH_ELLIPSIS) {
        for (int i = count - 1; i >= 0; i--) {
            if (chars[i].character == '\\') { suffix = i; break; }
        }
        if (suffix == 0 || suffix == count) {
            if (!(flags & (DT_END_ELLIPSIS | DT_WORD_ELLIPSIS))) return count;
            suffix = count;
        }
        for (int i = suffix; i < count; i++) suffix_width += chars[i].advance;
        prefix = suffix;
        prefix_width -= suffix_width;
    }
    while (prefix > 0 && prefix_width + suffix_width + 3*dot_width > available)
        prefix_width -= chars[--prefix].advance;
    if (flags & DT_WORD_ELLIPSIS) {
        while (prefix > 0 && !text_space(chars[prefix-1].character))
            prefix_width -= chars[--prefix].advance;
    }
    *kept_prefix = prefix;
    *kept_suffix = suffix;
    int tail_count = count - suffix;
    if (prefix + 3 > suffix) {
        for (int i = tail_count - 1; i >= 0; i--) chars[prefix+3+i] = chars[suffix+i];
    } else {
        for (int i = 0; i < tail_count; i++) chars[prefix+3+i] = chars[suffix+i];
    }
    for (int i = 0; i < 3; i++)
        chars[prefix+i] = (U32_TEXT_CHAR){'.', FALSE, dot_width, 0};
    *width = prefix_width + suffix_width + 3*dot_width;
    return prefix + 3 + tail_count;
}

static BOOL text_draw_line(HDC dc, int x, int y, U32_TEXT_CHAR *chars,
                             int count, WCHAR *glyphs, int *advances,
                             const RECT *clip, UINT flags, int ascent)
{
    for (int i = 0; i < count; i++) {
        glyphs[i] = chars[i].character == '\t' ? ' ' : chars[i].character;
        advances[i] = chars[i].advance;
    }
    if (!(flags & DT_PREFIXONLY)) {
        int offset = 0;
        int64_t position = x;
        while (offset < count) {
            int chunk = count - offset;
            if (chunk > 8192) chunk = 8192; /* ExtTextOut's documented limit. */
            if (!text_coordinate(position) ||
                !ExtTextOutW(dc, (int)position, y, clip ? 4 : 0,
                    (const GDI_RECT *)clip, glyphs+offset, (UINT)chunk,
                    advances+offset)) return FALSE;
            for (int i = 0; i < chunk; i++) position += advances[offset+i];
            offset += chunk;
        }
    }
    if (!(flags & DT_HIDEPREFIX)) {
        int64_t position = x;
        for (int i = 0; i < count; i++) {
            int64_t right = position + chars[i].advance;
            int64_t top = (int64_t)y + ascent + 1;
            if (chars[i].underline && text_coordinate(position) &&
                text_coordinate(right) && text_coordinate(top + 1)) {
                GDI_RECT rule = {(LONG)position, (LONG)top, (LONG)right, (LONG)top+1};
                if (clip) {
                    if (rule.left < clip->left) rule.left = clip->left;
                    if (rule.right > clip->right) rule.right = clip->right;
                    if (rule.top < clip->top) rule.top = clip->top;
                    if (rule.bottom > clip->bottom) rule.bottom = clip->bottom;
                }
                DWORD previous = SetBkColor(dc, GetTextColor(dc));
                BOOL result = ExtTextOutW(dc, 0, 0, 2, &rule, NULL, 0, NULL);
                SetBkColor(dc, previous);
                if (!result) return FALSE;
            }
            position = right;
        }
    }
    return TRUE;
}

static int draw_text_wide(HDC dc, PWSTR text, int count, RECT *rect,
                            UINT flags, U32_DRAWTEXTPARAMS *params)
{
    GDI_TEXTMETRICW metrics;
    int length = text_length(text, count);
    if (!rect || length < 0 || (params && params->size != sizeof(*params)) ||
        !GetTextMetricsW(dc, &metrics)) {
        SetLastError(87);
        return 0;
    }
    if (params) params->length_drawn = 0;
    int line_height = metrics.height;
    if (flags & DT_EXTERNALLEADING) line_height += metrics.external_leading;
    if (line_height <= 0) return 0;
    if (!length) {
        if (flags & DT_CALCRECT) {
            int64_t bottom = (int64_t)rect->top + ((flags & DT_SINGLELINE) ? line_height : 0);
            if (!text_coordinate(bottom)) { SetLastError(87); return 0; }
            rect->right = rect->left;
            rect->bottom = (LONG)bottom;
        }
        return (flags & DT_SINGLELINE) ? line_height : 1;
    }
    uint64_t capacity = (uint64_t)(UINT)length + 4;
    BOOL modifying = !(flags & DT_CALCRECT) && (flags & DT_MODIFYSTRING) &&
        (flags & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS));
    U32_TEXT_CHAR *chars = kmalloc(capacity * sizeof(*chars) * 2 +
        capacity * (sizeof(WCHAR) * (modifying ? 2 : 1) + sizeof(int)));
    if (!chars) { SetLastError(8); return 0; }
    U32_TEXT_CHAR *line = chars + capacity;
    int *advances = (int *)(line + capacity);
    WCHAR *glyphs = (WCHAR *)(advances + capacity);
    WCHAR *modified = glyphs + capacity;
    UINT modified_used = 0, copy_from = 0;
    int used = text_prepare(dc, text, length, flags, chars);
    int result = 0;
    if (used < 0) goto done;
    int left = params ? params->left_margin : 0;
    int right = params ? params->right_margin : 0;
    int64_t available = (int64_t)rect->right - rect->left - left - right;
    int64_t tab = (int64_t)metrics.average_width *
        ((flags & DT_TABSTOP) && params ? params->tab_length : 8);
    if (tab < 0 || tab > 2147483647) { SetLastError(87); goto done; }
    LONG dots[2];
    WCHAR dot = '.';
    if (!GetTextExtentPoint32W(dc, &dot, 1, dots)) goto done;
    int64_t y = rect->top, widest = 0;
    if ((flags & DT_SINGLELINE) && !(flags & DT_CALCRECT)) {
        if (flags & DT_VCENTER) y += ((int64_t)rect->bottom - rect->top - line_height) / 2;
        else if (flags & DT_BOTTOM) y = (int64_t)rect->bottom - line_height;
    }
    int start = 0;
    BOOL more;
    do {
        if (!(flags & (DT_CALCRECT | DT_NOCLIP | DT_SINGLELINE)) &&
            (y >= rect->bottom || ((flags & DT_EDITCONTROL) &&
                                   y + line_height > rect->bottom))) break;
        int next;
        BOOL newline;
        int64_t width;
        int end = text_line(chars, used, start, flags, available, (int)tab,
                               &next, &newline, &width);
        int line_count = end - start;
        for (int i = 0; i < line_count; i++) line[i] = chars[start+i];
        UINT ellipsis_flags = next == used && !newline ? flags : flags & ~DT_END_ELLIPSIS;
        if (!(flags & DT_CALCRECT) &&
            (ellipsis_flags & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS)) &&
            width > available) {
            int prefix, suffix;
            line_count = text_ellipsis(line, line_count, ellipsis_flags, dots[0],
                                         available, &width, &prefix, &suffix);
            if (modifying && prefix < suffix) {
                UINT from = start + prefix ? (UINT)chars[start+prefix-1].source_end : 0;
                UINT to = start + suffix == used ? (UINT)length :
                    (start + suffix ? (UINT)chars[start+suffix-1].source_end : 0);
                /* Replace source spans, not rendered glyphs: keep mnemonic
                 * markers, line endings, and untouched later lines intact. */
                if (from < copy_from || to < from ||
                    (uint64_t)modified_used + from - copy_from + 3 >= capacity)
                    goto done;
                while (copy_from < from) modified[modified_used++] = text[copy_from++];
                for (int i = 0; i < 3; i++) modified[modified_used++] = '.';
                copy_from = to;
            }
        }
        if (width > widest) widest = width;
        int64_t x = (int64_t)rect->left + left;
        if (flags & DT_CENTER) x += (available - width) / 2;
        else if (flags & DT_RIGHT) x += available - width;
        if (!text_coordinate(x) || !text_coordinate(y + line_height)) {
            SetLastError(87);
            goto done;
        }
        if (!(flags & DT_CALCRECT) &&
            !text_draw_line(dc, (int)x, (int)y, line, line_count, glyphs, advances,
                (flags & DT_NOCLIP) ? NULL : rect, flags, metrics.ascent)) goto done;
        y += line_height;
        if (params) params->length_drawn = next < used
            ? (UINT)(next ? chars[next-1].source_end : 0) : (UINT)length;
        more = next < used || newline;
        if (next == start && !newline) more = FALSE;
        start = next;
    } while (more);
    if (!text_coordinate(y - rect->top)) { SetLastError(87); goto done; }
    if (modified_used) {
        if ((uint64_t)modified_used + (UINT)length - copy_from >= capacity) goto done;
        while (copy_from < (UINT)length) modified[modified_used++] = text[copy_from++];
        modified[modified_used] = 0;
        for (UINT i = 0; i <= modified_used; i++) text[i] = modified[i];
    }
    if (flags & DT_CALCRECT) {
        if (!text_coordinate((int64_t)rect->left + widest + left + right)) goto done;
        rect->right = (LONG)((int64_t)rect->left + widest + left + right);
        rect->bottom = (LONG)y;
    }
    result = (int)(y - rect->top);
done:
    kfree(chars);
    return result;
}

int WINAPI DrawTextExW(HDC dc, PWSTR text, int count, PVOID rect,
                         UINT flags, PVOID params)
{
    return draw_text_wide(dc, text, count, rect, flags, params);
}

int WINAPI DrawTextW(HDC dc, PCWSTR text, int count, PVOID rect, UINT flags)
{
    U32_DRAWTEXTPARAMS params = {sizeof(params), (int)((flags >> 8) & 255), 0, 0, 0};
    return draw_text_wide(dc, (PWSTR)text, count, rect, flags, &params);
}

int WINAPI DrawTextExA(HDC dc, PSTR text, int count, PVOID rect,
                         UINT flags, PVOID parameters)
{
    U32_DRAWTEXTPARAMS *params = parameters;
    if (count < -1 || (!text && count != 0) ||
        (params && params->size != sizeof(*params))) { SetLastError(87); return 0; }
    if (!count || (count == -1 && !*text))
        return draw_text_wide(dc, NULL, 0, rect, flags, params);
    int length = MultiByteToWideChar(0, 0, text, count, NULL, 0);
    if (length <= 0) return 0;
    WCHAR *wide = kmalloc(((uint64_t)(UINT)length + 4) * sizeof(WCHAR));
    if (!wide) { SetLastError(8); return 0; }
    MultiByteToWideChar(0, 0, text, count, wide, length);
    if (count == -1) length--;
    wide[length] = 0;
    U32_DRAWTEXTPARAMS local = params ? *params :
        (U32_DRAWTEXTPARAMS){sizeof(local), 8, 0, 0, 0};
    int result = draw_text_wide(dc, wide, length, rect, flags, &local);
    if (params) params->length_drawn = local.length_drawn
        ? (UINT)WideCharToMultiByte(0, 0, wide, (int)local.length_drawn, NULL, 0, NULL, NULL) : 0;
    if (result && (flags & DT_MODIFYSTRING) &&
        (flags & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS))) {
        int bytes = WideCharToMultiByte(0, 0, wide, -1, NULL, 0, NULL, NULL);
        if (bytes > 0) WideCharToMultiByte(0, 0, wide, -1, text, bytes, NULL, NULL);
    }
    kfree(wide);
    return result;
}

int WINAPI DrawTextA(HDC dc, PCSTR text, int count, PVOID rect, UINT flags)
{
    U32_DRAWTEXTPARAMS params = {sizeof(params), (int)((flags >> 8) & 255), 0, 0, 0};
    return DrawTextExA(dc, (PSTR)text, count, rect, flags, &params);
}
