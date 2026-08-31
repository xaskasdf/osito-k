/*
 * OsitoK Windows Compatibility Layer — shell32.dll Shim Implementation
 *
 * Stub implementation — ShellExecute logs the request and returns
 * success (handle > 32) without launching any process.
 */

#include "shell32_shim.h"
#include "ole32_shim.h"
#include "advapi32_shim.h"
#include "win32_abi.h"
#include "../kernel/smp.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t value);
extern void serial_puthex(uint64_t value, int digits);
extern PVOID WINAPI LocalAlloc(UINT uFlags, SIZE_T dwBytes);
extern void WINAPI SetLastError(DWORD error);
extern DWORD win32_current_process_id(void);
extern BOOL WINAPI IsWindow(HWND window);
extern HANDLE WINAPI CopyIcon(HANDLE icon);
extern HANDLE WINAPI LoadIconW(HINSTANCE instance, PCWSTR name);
extern BOOL user32_release_icon(HANDLE icon);
extern DWORD WINAPI GetFileAttributesW(PCWSTR path);
extern DWORD WINAPI GetFileAttributesA(PCSTR path);
extern BOOL WINAPI CreateDirectoryA(PCSTR path, PVOID security_attributes);
extern DWORD WINAPI GetLastError(void);
extern int WINAPI GetSystemMetrics(int index);

#define NIM_ADD         0x00000000U
#define NIM_MODIFY      0x00000001U
#define NIM_DELETE      0x00000002U
#define NIM_SETFOCUS    0x00000003U
#define NIM_SETVERSION  0x00000004U

#define NIF_MESSAGE     0x00000001U
#define NIF_ICON        0x00000002U
#define NIF_TIP         0x00000004U
#define NIF_STATE       0x00000008U
#define NIF_INFO        0x00000010U
#define NIF_GUID        0x00000020U

#define SHGFI_ICON               0x00000100U
#define SHGFI_DISPLAYNAME        0x00000200U
#define SHGFI_TYPENAME           0x00000400U
#define SHGFI_ATTRIBUTES         0x00000800U
#define SHGFI_ICONLOCATION       0x00001000U
#define SHGFI_USEFILEATTRIBUTES  0x00000010U

#define ABM_GETSTATE       0x00000004U
#define ABM_GETTASKBARPOS  0x00000005U
#define ABM_GETAUTOHIDEBAR 0x00000007U

#define SSF_SHOWALLOBJECTS       0x00000001U
#define SSF_SHOWEXTENSIONS       0x00000002U
#define SSF_SHOWCOMPCOLOR        0x00000008U
#define SSF_SHOWSYSFILES         0x00000020U
#define SSF_DOUBLECLICKINWEBVIEW 0x00000080U
#define SSF_SHOWATTRIBCOL        0x00000100U
#define SSF_DESKTOPHTML          0x00000200U
#define SSF_WIN95CLASSIC         0x00000400U
#define SSF_DONTPRETTYPATH       0x00000800U
#define SSF_MAPNETDRVBUTTON      0x00001000U
#define SSF_SHOWINFOTIP          0x00002000U
#define SSF_HIDEICONS            0x00004000U
#define SSF_NOCONFIRMRECYCLE     0x00008000U
#define SSF_AUTOCHECKSELECT      0x00800000U
#define SSF_ICONSONLY            0x01000000U

#define SHELL_PATH_PIDL_MAGIC 0x4F534954U

#define SHELL_NOTIFY_SLOTS 128

typedef struct {
    DWORD cb_size;
    HWND hwnd;
    UINT id;
    UINT flags;
    UINT callback_message;
    HANDLE icon;
    DWORD state;
    DWORD state_mask;
    UINT version;
    DWORD info_flags;
    BOOL has_guid;
    GUID guid;
    WCHAR tip[128];
    WCHAR info[256];
    WCHAR info_title[64];
} SHELL_NOTIFY_DATA;

typedef struct {
    BOOL used;
    DWORD owner_pid;
    HWND hwnd;
    UINT id;
    UINT flags;
    UINT callback_message;
    HANDLE icon;
    DWORD state;
    UINT version;
    DWORD info_flags;
    BOOL has_guid;
    GUID guid;
    WCHAR tip[128];
    WCHAR info[256];
    WCHAR info_title[64];
} SHELL_NOTIFY_ENTRY;

static SHELL_NOTIFY_ENTRY shell_notify_entries[SHELL_NOTIFY_SLOTS];
static spinlock_t shell_notify_lock = SPINLOCK_INIT;

static inline uint64_t shell_notify_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&shell_notify_lock);
    return flags;
}

static inline void shell_notify_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&shell_notify_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static void shell_zero(void *memory, SIZE_T bytes)
{
    BYTE *out = (BYTE *)memory;
    for (SIZE_T i = 0; i < bytes; i++) out[i] = 0;
}

static void shell_copy(void *destination, const void *source, SIZE_T bytes)
{
    BYTE *out = (BYTE *)destination;
    const BYTE *in = (const BYTE *)source;
    for (SIZE_T i = 0; i < bytes; i++) out[i] = in[i];
}

static BOOL shell_range_valid(DWORD size, SIZE_T offset, SIZE_T bytes)
{
    return offset <= size && bytes <= (SIZE_T)size - offset;
}

static DWORD shell_read_u32(const BYTE *data, SIZE_T offset)
{
    return (DWORD)data[offset] |
           ((DWORD)data[offset + 1] << 8) |
           ((DWORD)data[offset + 2] << 16) |
           ((DWORD)data[offset + 3] << 24);
}

static ULONG_PTR shell_read_pointer(const BYTE *data, SIZE_T offset)
{
    ULONG_PTR value = shell_read_u32(data, offset);
    if (!g_compat32_mode)
        value |= (ULONG_PTR)shell_read_u32(data, offset + 4) << 32;
    return value;
}

static void shell_write_u32(BYTE *data, SIZE_T offset, DWORD value)
{
    data[offset] = (BYTE)value;
    data[offset + 1] = (BYTE)(value >> 8);
    data[offset + 2] = (BYTE)(value >> 16);
    data[offset + 3] = (BYTE)(value >> 24);
}

static void shell_write_pointer(BYTE *data, SIZE_T offset, ULONG_PTR value)
{
    shell_write_u32(data, offset, (DWORD)value);
    if (!g_compat32_mode)
        shell_write_u32(data, offset + 4, (DWORD)(value >> 32));
}

static void shell_read_text(const BYTE *data, DWORD size, SIZE_T offset,
                            BOOL wide, WCHAR *output, UINT capacity)
{
    if (!output || !capacity) return;
    output[0] = 0;
    if (offset >= size) return;
    SIZE_T unit = wide ? sizeof(WCHAR) : sizeof(BYTE);
    SIZE_T available = ((SIZE_T)size - offset) / unit;
    if (available >= capacity) available = capacity - 1;
    for (SIZE_T i = 0; i < available; i++) {
        WCHAR value = wide
            ? (WCHAR)((WORD)data[offset + i * 2] |
                      ((WORD)data[offset + i * 2 + 1] << 8))
            : (WCHAR)data[offset + i];
        output[i] = value;
        if (!value) return;
        output[i + 1] = 0;
    }
}

static void shell_copy_wide(WCHAR *destination, const WCHAR *source,
                            UINT capacity)
{
    if (!capacity) return;
    UINT i = 0;
    if (source) {
        while (i + 1 < capacity && source[i]) {
            destination[i] = source[i];
            i++;
        }
    }
    destination[i] = 0;
}

static BOOL shell_guid_equal(const GUID *left, const GUID *right)
{
    const BYTE *a = (const BYTE *)left;
    const BYTE *b = (const BYTE *)right;
    for (UINT i = 0; i < sizeof(GUID); i++)
        if (a[i] != b[i]) return FALSE;
    return TRUE;
}

static BOOL shell_notify_parse(PVOID input, BOOL wide,
                               SHELL_NOTIFY_DATA *output)
{
    if (!input || !output) return FALSE;
    const BYTE *data = (const BYTE *)input;
    shell_zero(output, sizeof(*output));
    output->cb_size = shell_read_u32(data, 0);

    SIZE_T hwnd_offset = g_compat32_mode ? 4 : 8;
    SIZE_T id_offset = g_compat32_mode ? 8 : 16;
    SIZE_T flags_offset = g_compat32_mode ? 12 : 20;
    SIZE_T callback_offset = g_compat32_mode ? 16 : 24;
    SIZE_T icon_offset = g_compat32_mode ? 20 : 32;
    SIZE_T tip_offset = g_compat32_mode ? 24 : 40;
    SIZE_T pointer_size = g_compat32_mode ? 4 : 8;
    if (!shell_range_valid(output->cb_size, flags_offset, sizeof(DWORD)))
        return FALSE;

    if (shell_range_valid(output->cb_size, hwnd_offset, pointer_size))
        output->hwnd = (HWND)shell_read_pointer(data, hwnd_offset);
    if (shell_range_valid(output->cb_size, id_offset, sizeof(DWORD)))
        output->id = shell_read_u32(data, id_offset);
    output->flags = shell_read_u32(data, flags_offset);
    if (shell_range_valid(output->cb_size, callback_offset, sizeof(DWORD)))
        output->callback_message = shell_read_u32(data, callback_offset);
    if (shell_range_valid(output->cb_size, icon_offset, pointer_size))
        output->icon = (HANDLE)shell_read_pointer(data, icon_offset);

    SIZE_T text_unit = wide ? sizeof(WCHAR) : sizeof(BYTE);
    SIZE_T state_offset = tip_offset + 128 * text_unit;
    SIZE_T state_mask_offset = state_offset + sizeof(DWORD);
    SIZE_T info_offset = state_mask_offset + sizeof(DWORD);
    SIZE_T version_offset = info_offset + 256 * text_unit;
    SIZE_T title_offset = version_offset + sizeof(DWORD);
    SIZE_T info_flags_offset = title_offset + 64 * text_unit;
    SIZE_T guid_offset = info_flags_offset + sizeof(DWORD);

    shell_read_text(data, output->cb_size, tip_offset, wide,
                    output->tip, 128);
    if (shell_range_valid(output->cb_size, state_offset, sizeof(DWORD)))
        output->state = shell_read_u32(data, state_offset);
    if (shell_range_valid(output->cb_size, state_mask_offset, sizeof(DWORD)))
        output->state_mask = shell_read_u32(data, state_mask_offset);
    shell_read_text(data, output->cb_size, info_offset, wide,
                    output->info, 256);
    if (shell_range_valid(output->cb_size, version_offset, sizeof(DWORD)))
        output->version = shell_read_u32(data, version_offset);
    shell_read_text(data, output->cb_size, title_offset, wide,
                    output->info_title, 64);
    if (shell_range_valid(output->cb_size, info_flags_offset, sizeof(DWORD)))
        output->info_flags = shell_read_u32(data, info_flags_offset);
    if ((output->flags & NIF_GUID) &&
        shell_range_valid(output->cb_size, guid_offset, sizeof(GUID))) {
        shell_copy(&output->guid, data + guid_offset, sizeof(GUID));
        output->has_guid = TRUE;
    }
    return TRUE;
}

static SHELL_NOTIFY_ENTRY *shell_notify_find_locked(
    DWORD owner_pid, const SHELL_NOTIFY_DATA *data)
{
    for (UINT i = 0; i < SHELL_NOTIFY_SLOTS; i++) {
        SHELL_NOTIFY_ENTRY *entry = &shell_notify_entries[i];
        if (!entry->used || entry->owner_pid != owner_pid) continue;
        if (data->has_guid) {
            if (entry->has_guid && shell_guid_equal(&entry->guid, &data->guid))
                return entry;
        } else if (!entry->has_guid && entry->hwnd == data->hwnd &&
                   entry->id == data->id) {
            return entry;
        }
    }
    return NULL;
}

static SHELL_NOTIFY_ENTRY *shell_notify_alloc_locked(void)
{
    for (UINT i = 0; i < SHELL_NOTIFY_SLOTS; i++)
        if (!shell_notify_entries[i].used) return &shell_notify_entries[i];
    return NULL;
}

static void shell_notify_apply(SHELL_NOTIFY_ENTRY *entry,
                               const SHELL_NOTIFY_DATA *data,
                               HANDLE icon_copy, BOOL adding)
{
    if (adding) {
        entry->hwnd = data->hwnd;
        entry->id = data->id;
        entry->has_guid = data->has_guid;
        if (data->has_guid) entry->guid = data->guid;
    }
    entry->flags |= data->flags;
    if (data->flags & NIF_MESSAGE)
        entry->callback_message = data->callback_message;
    if (data->flags & NIF_ICON)
        entry->icon = icon_copy;
    if (data->flags & NIF_TIP)
        shell_copy_wide(entry->tip, data->tip, 128);
    if (data->flags & NIF_STATE)
        entry->state = (entry->state & ~data->state_mask) |
                       (data->state & data->state_mask);
    if (data->flags & NIF_INFO) {
        shell_copy_wide(entry->info, data->info, 256);
        shell_copy_wide(entry->info_title, data->info_title, 64);
        entry->info_flags = data->info_flags;
    }
}

static void shell_notify_trace(BOOL wide, DWORD message,
                               const SHELL_NOTIFY_DATA *data, BOOL result)
{
    serial_puts("[SHELL32] Shell_NotifyIcon");
    serial_puts(wide ? "W" : "A");
    serial_puts(" msg=");
    serial_putdec(message);
    serial_puts(" pid=");
    serial_putdec(win32_current_process_id());
    serial_puts(" hwnd=0x");
    serial_puthex((uint64_t)(ULONG_PTR)data->hwnd,
                  g_compat32_mode ? 8 : 16);
    serial_puts(" id=");
    serial_putdec(data->id);
    serial_puts(result ? " ok\n" : " failed\n");
}

static BOOL shell_notify_icon(DWORD message, PVOID input, BOOL wide)
{
    SHELL_NOTIFY_DATA data;
    if (!shell_notify_parse(input, wide, &data)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD owner_pid = win32_current_process_id();
    if (message == NIM_ADD && (!data.hwnd || !IsWindow(data.hwnd))) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        shell_notify_trace(wide, message, &data, FALSE);
        return FALSE;
    }

    HANDLE icon_copy = NULL;
    if ((message == NIM_ADD || message == NIM_MODIFY) &&
        (data.flags & NIF_ICON) && data.icon) {
        icon_copy = CopyIcon(data.icon);
        if (!icon_copy) {
            shell_notify_trace(wide, message, &data, FALSE);
            return FALSE;
        }
    }

    HANDLE old_icon = NULL;
    BOOL result = FALSE;
    DWORD error = 0;
    uint64_t irq_flags = shell_notify_lock_irqsave();
    SHELL_NOTIFY_ENTRY *entry = shell_notify_find_locked(owner_pid, &data);

    if (message == NIM_ADD) {
        if (entry) {
            error = 183; /* ERROR_ALREADY_EXISTS */
        } else if (!(entry = shell_notify_alloc_locked())) {
            error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        } else {
            shell_zero(entry, sizeof(*entry));
            entry->used = TRUE;
            entry->owner_pid = owner_pid;
            shell_notify_apply(entry, &data, icon_copy, TRUE);
            icon_copy = NULL;
            result = TRUE;
        }
    } else if (message == NIM_MODIFY) {
        if (!entry) {
            error = 1168; /* ERROR_NOT_FOUND */
        } else {
            if (data.flags & NIF_ICON) old_icon = entry->icon;
            shell_notify_apply(entry, &data, icon_copy, FALSE);
            if (data.flags & NIF_ICON) icon_copy = NULL;
            result = TRUE;
        }
    } else if (message == NIM_DELETE) {
        if (!entry) {
            error = 1168;
        } else {
            old_icon = entry->icon;
            shell_zero(entry, sizeof(*entry));
            result = TRUE;
        }
    } else if (message == NIM_SETVERSION) {
        if (!entry) {
            error = 1168;
        } else {
            entry->version = data.version;
            result = TRUE;
        }
    } else if (message == NIM_SETFOCUS) {
        if (!entry) error = 1168;
        else result = TRUE;
    } else {
        error = 87;
    }
    shell_notify_unlock_irqrestore(irq_flags);

    if (old_icon) user32_release_icon(old_icon);
    if (icon_copy) user32_release_icon(icon_copy);
    if (!result) SetLastError(error);
    shell_notify_trace(wide, message, &data, result);
    return result;
}

/* ── API Implementations ───────────────────────────────────── */

/* True if path ends in ".exe" (case-insensitive) — UT99 relaunches itself this
 * way to apply a video/color-depth change. */
static int ends_in_exe(PCSTR p)
{
    if (!p) return 0;
    int n = 0; while (p[n]) n++;
    if (n < 4) return 0;
    const char *e = p + n - 4;
    return (e[0] == '.' &&
            (e[1] == 'e' || e[1] == 'E') &&
            (e[2] == 'x' || e[2] == 'X') &&
            (e[3] == 'e' || e[3] == 'E'));
}

HINSTANCE WINAPI ShellExecuteA(HWND hwnd, PCSTR lpOperation, PCSTR lpFile,
                                PCSTR lpParameters, PCSTR lpDirectory, int nShowCmd)
{
    (void)hwnd; (void)lpParameters; (void)lpDirectory; (void)nShowCmd;
    serial_puts("[SHELL32] ShellExecuteA: ");
    if (lpOperation) { serial_puts(lpOperation); serial_puts(" "); }
    if (lpFile) serial_puts(lpFile);
    serial_puts("\n");
    /* Launching an .exe = the game relaunching itself → request a re-exec so the
     * current process's ExitProcess restarts the EXE instead of exiting dead. */
    if (ends_in_exe(lpFile)) {
        extern int g_win32_relaunch;
        g_win32_relaunch = 1;
        serial_puts("[SHELL32] .exe launch → RE-EXEC requested\n");
    }
    return (HINSTANCE)(ULONG_PTR)32; /* >32 = success */
}

HINSTANCE WINAPI ShellExecuteW(HWND hwnd, PCWSTR lpOperation, PCWSTR lpFile,
                                PCWSTR lpParameters, PCWSTR lpDirectory, int nShowCmd)
{
    (void)hwnd; (void)lpOperation; (void)lpFile;
    (void)lpParameters; (void)lpDirectory; (void)nShowCmd;
    serial_puts("[SHELL32] ShellExecuteW: stub\n");
    return (HINSTANCE)(ULONG_PTR)32; /* >32 = success */
}

BOOL WINAPI Shell_NotifyIconA(DWORD dwMessage, PVOID lpData)
{
    return shell_notify_icon(dwMessage, lpData, FALSE);
}

BOOL WINAPI Shell_NotifyIconW(DWORD dwMessage, PVOID lpData)
{
    return shell_notify_icon(dwMessage, lpData, TRUE);
}

static void WINAPI DragAcceptFiles_k32(HWND hwnd, BOOL accept)
{
    (void)hwnd;
    (void)accept;
    serial_puts("[SHELL32] DragAcceptFiles\n");
}

static UINT WINAPI DragQueryFileW_k32(HANDLE drop, UINT file,
                                       PWSTR path, UINT path_chars)
{
    (void)drop;
    (void)file;
    if (path && path_chars) path[0] = 0;
    return 0;
}

static UINT WINAPI DragQueryFileA_k32(HANDLE drop, UINT file,
                                       PSTR path, UINT path_chars)
{
    (void)drop;
    (void)file;
    if (path && path_chars) path[0] = 0;
    return 0;
}

static void WINAPI DragFinish_k32(HANDLE drop)
{
    (void)drop;
}

static void WINAPI SHGetSettings_k32(PVOID settings, DWORD mask)
{
    if (!settings) return;

    /* SHELLFLAGSTATE is one DWORD of bitfields.  The SSF mask values do not
     * match those bit positions, so translate each enabled default instead
     * of copying the mask.  Osito's Explorer defaults hide protected/hidden
     * files and extensions while retaining color and information tips. */
    DWORD state = 0;
    if (mask & SSF_SHOWCOMPCOLOR) state |= 1U << 4;
    if (mask & SSF_SHOWINFOTIP) state |= 1U << 11;

    /* Unset defaults intentionally remain zero in the output structure. */
    *(DWORD *)settings = state;
}

static BOOL WINAPI IsUserAnAdmin_k32(void)
{
    return TRUE;
}

static void shell_write_wide_text(BYTE *output, DWORD output_size,
                                  SIZE_T offset, const WCHAR *text,
                                  UINT capacity)
{
    if (!output || !shell_range_valid(output_size, offset,
                                      (SIZE_T)capacity * sizeof(WCHAR)))
        return;
    UINT i = 0;
    if (text) {
        while (i + 1 < capacity && text[i]) {
            output[offset + i * 2] = (BYTE)text[i];
            output[offset + i * 2 + 1] = (BYTE)(text[i] >> 8);
            i++;
        }
    }
    output[offset + i * 2] = 0;
    output[offset + i * 2 + 1] = 0;
}

static void shell_write_ascii_text(BYTE *output, DWORD output_size,
                                   SIZE_T offset, const char *text,
                                   UINT capacity)
{
    if (!output || !shell_range_valid(output_size, offset,
                                      (SIZE_T)capacity * sizeof(WCHAR)))
        return;
    UINT i = 0;
    if (text) {
        while (i + 1 < capacity && text[i]) {
            output[offset + i * 2] = (BYTE)text[i];
            output[offset + i * 2 + 1] = 0;
            i++;
        }
    }
    output[offset + i * 2] = 0;
    output[offset + i * 2 + 1] = 0;
}

static WCHAR shell_lower_wide(WCHAR value)
{
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static BOOL shell_wide_suffix(PCWSTR text, const char *suffix)
{
    if (!text || !suffix) return FALSE;
    SIZE_T text_length = 0;
    SIZE_T suffix_length = 0;
    while (text[text_length]) text_length++;
    while (suffix[suffix_length]) suffix_length++;
    if (text_length < suffix_length) return FALSE;
    text += text_length - suffix_length;
    for (SIZE_T i = 0; i < suffix_length; i++)
        if (shell_lower_wide(text[i]) != (WCHAR)suffix[i]) return FALSE;
    return TRUE;
}

static ULONG_PTR WINAPI SHGetFileInfoW_k32(PCWSTR path, DWORD attributes,
                                            PVOID file_info,
                                            UINT file_info_size, UINT flags)
{
    if (!path) {
        SetLastError(87);
        return 0;
    }

    DWORD actual_attributes = attributes;
    if (!(flags & SHGFI_USEFILEATTRIBUTES)) {
        actual_attributes = GetFileAttributesW(path);
        if (actual_attributes == 0xFFFFFFFFU) return 0;
    }

    SIZE_T pointer_size = g_compat32_mode ? 4 : 8;
    SIZE_T icon_offset = 0;
    SIZE_T index_offset = pointer_size;
    SIZE_T attributes_offset = index_offset + sizeof(LONG);
    SIZE_T display_offset = attributes_offset + sizeof(DWORD);
    SIZE_T type_offset = display_offset + 260 * sizeof(WCHAR);
    SIZE_T expected_size = type_offset + 80 * sizeof(WCHAR);
    BYTE *output = (BYTE *)file_info;

    if (file_info && file_info_size) {
        SIZE_T clear_size = file_info_size;
        if (clear_size > expected_size) clear_size = expected_size;
        shell_zero(output, clear_size);
    }

    if ((flags & SHGFI_ATTRIBUTES) && output &&
        shell_range_valid(file_info_size, attributes_offset, sizeof(DWORD)))
        shell_write_u32(output, attributes_offset, actual_attributes);

    if ((flags & (SHGFI_DISPLAYNAME | SHGFI_ICONLOCATION)) && output) {
        PCWSTR display = path;
        if (!(flags & SHGFI_ICONLOCATION)) {
            for (PCWSTR cursor = path; *cursor; cursor++)
                if (*cursor == '\\' || *cursor == '/') display = cursor + 1;
        }
        shell_write_wide_text(output, file_info_size, display_offset,
                              display, 260);
    }

    if ((flags & SHGFI_TYPENAME) && output) {
        const char *type_name = "File";
        if (actual_attributes & FILE_ATTRIBUTE_DIRECTORY)
            type_name = "File folder";
        else if (shell_wide_suffix(path, ".exe"))
            type_name = "Application";
        else if (shell_wide_suffix(path, ".dll"))
            type_name = "Dynamic Link Library";
        shell_write_ascii_text(output, file_info_size, type_offset,
                               type_name, 80);
    }

    if (output && shell_range_valid(file_info_size, index_offset,
                                    sizeof(LONG)))
        shell_write_u32(output, index_offset, 0);

    if (flags & SHGFI_ICON) {
        HANDLE icon = CopyIcon(LoadIconW(NULL, NULL));
        if (!icon) return 0;
        if (!output || !shell_range_valid(file_info_size, icon_offset,
                                          pointer_size)) {
            user32_release_icon(icon);
            SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
            return 0;
        }
        shell_write_pointer(output, icon_offset, (ULONG_PTR)icon);
    }
    return 1;
}

static ULONG_PTR WINAPI SHAppBarMessage_k32(DWORD message, PVOID appbar_data)
{
    if (message == ABM_GETSTATE) return 0;
    if (message == ABM_GETAUTOHIDEBAR) return 0;
    if (!appbar_data) {
        SetLastError(87);
        return 0;
    }

    BYTE *data = (BYTE *)appbar_data;
    DWORD size = shell_read_u32(data, 0);
    SIZE_T edge_offset = g_compat32_mode ? 12 : 20;
    SIZE_T rect_offset = g_compat32_mode ? 16 : 24;
    if (message == ABM_GETTASKBARPOS) {
        if (!shell_range_valid(size, rect_offset, 4 * sizeof(LONG))) {
            SetLastError(87);
            return 0;
        }
        int width = GetSystemMetrics(0);  /* SM_CXSCREEN */
        int height = GetSystemMetrics(1); /* SM_CYSCREEN */
        int top = height > 32 ? height - 32 : 0;
        shell_write_u32(data, edge_offset, 3); /* ABE_BOTTOM */
        shell_write_u32(data, rect_offset, 0);
        shell_write_u32(data, rect_offset + 4, (DWORD)top);
        shell_write_u32(data, rect_offset + 8, (DWORD)width);
        shell_write_u32(data, rect_offset + 12, (DWORD)height);
    }
    return 1;
}

static HRESULT WINAPI SHGetKnownFolderPath_k32(LPCGUID folder, DWORD flags,
                                                HANDLE token, PVOID out_path)
{
    static const GUID local_app_data = {
        0xF1B32785, 0x6FBA, 0x4FCF,
        { 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91 }
    };
    static const char path[] = "C:\\Users\\osito\\AppData\\Local";
    (void)flags;
    (void)token;

    if (!folder || !out_path) return (HRESULT)0x80070057;
    const BYTE *actual = (const BYTE *)folder;
    const BYTE *expected = (const BYTE *)&local_app_data;
    for (int i = 0; i < 16; i++)
        if (actual[i] != expected[i]) return (HRESULT)0x80070002;

    PWSTR result = (PWSTR)shim_CoTaskMemAlloc(sizeof(path) * sizeof(WCHAR));
    if (!result) return (HRESULT)0x8007000E;
    for (SIZE_T i = 0; i < sizeof(path); i++) result[i] = (BYTE)path[i];

    if (g_compat32_mode)
        *(uint32_t *)out_path = (uint32_t)(ULONG_PTR)result;
    else
        *(PWSTR *)out_path = result;
    return 0;
}

static int WINAPI SHCreateDirectoryExA_k32(HWND hwnd, PCSTR path,
                                            PVOID security_attributes);
static DWORD shell_ensure_special_folder_w(HWND hwnd, PCWSTR path);

static HRESULT WINAPI SHGetFolderPathW_k32(HWND hwnd, int csidl, HANDLE token,
                                            DWORD flags, PWSTR out_path)
{
    (void)hwnd; (void)token; (void)flags;
    if (!out_path) return (HRESULT)0x80070057;

    const char *path;
    switch (csidl & 0xFF) {
        case 0x00:
        case 0x10: path = "C:\\Users\\osito\\Desktop"; break;
        case 0x05: path = "C:\\Users\\osito\\Documents"; break;
        case 0x1A: path = "C:\\Users\\osito\\AppData\\Roaming"; break;
        case 0x1C: path = "C:\\Users\\osito\\AppData\\Local"; break;
        case 0x14: path = "C:\\Windows\\Fonts"; break;
        case 0x20: path = "C:\\Users\\osito\\AppData\\Local\\Microsoft\\Windows\\INetCache"; break;
        case 0x23: path = "C:\\ProgramData"; break;
        case 0x24:
        case 0x25: path = "C:\\System"; break;
        case 0x26:
        case 0x2A: path = "C:\\System\\Program Files"; break;
        case 0x2B:
        case 0x2C: path = "C:\\System\\Program Files\\Common Files"; break;
        case 0x28: path = "C:\\Users\\osito"; break;
        default:
            serial_puts("[SHELL32] SHGetFolderPathW unsupported CSIDL=0x");
            serial_puthex((uint32_t)csidl, 8);
            serial_puts("\n");
            out_path[0] = 0;
            return (HRESULT)0x80070002;
    }

    int i = 0;
    while (path[i] && i < 259) {
        out_path[i] = (BYTE)path[i];
        i++;
    }
    out_path[i] = 0;

    if (csidl & 0x8000) { /* CSIDL_FLAG_CREATE */
        DWORD error = shell_ensure_special_folder_w(hwnd, out_path);
        if (error) {
            out_path[0] = 0;
            return (HRESULT)(0x80070000U | (error & 0xFFFFU));
        }
    }
    return 0;
}

static HRESULT WINAPI SHGetFolderPathA_k32(HWND hwnd, int csidl, HANDLE token,
                                            DWORD flags, PSTR out_path)
{
    if (!out_path) return (HRESULT)0x80070057;
    WCHAR wide_path[260];
    HRESULT status = SHGetFolderPathW_k32(hwnd, csidl, token, flags,
                                           wide_path);
    if (status != 0) {
        out_path[0] = 0;
        return status;
    }
    int i = 0;
    while (wide_path[i] && i < 259) {
        out_path[i] = wide_path[i] <= 0xFF ? (char)wide_path[i] : '?';
        i++;
    }
    out_path[i] = 0;
    return 0;
}

typedef struct __attribute__((packed)) {
    WORD cb;
    DWORD magic;
    WCHAR path[260];
    WORD terminator;
} SHELL_PATH_PIDL;

static HRESULT WINAPI SHGetSpecialFolderLocation_k32(HWND hwnd, int csidl,
                                                      PVOID out_pidl)
{
    if (!out_pidl) return (HRESULT)0x80070057;
    if (g_compat32_mode)
        *(uint32_t *)out_pidl = 0;
    else
        *(PVOID *)out_pidl = NULL;

    WCHAR path[260];
    HRESULT status = SHGetFolderPathW_k32(hwnd, csidl, NULL, 0, path);
    if (status != 0) return status;

    SHELL_PATH_PIDL *pidl =
        (SHELL_PATH_PIDL *)shim_CoTaskMemAlloc(sizeof(*pidl));
    if (!pidl) return (HRESULT)0x8007000E;
    shell_zero(pidl, sizeof(*pidl));
    pidl->cb = (WORD)(sizeof(*pidl) - sizeof(pidl->terminator));
    pidl->magic = SHELL_PATH_PIDL_MAGIC;
    shell_copy_wide(pidl->path, path, 260);

    if (g_compat32_mode)
        *(uint32_t *)out_pidl = (uint32_t)(ULONG_PTR)pidl;
    else
        *(PVOID *)out_pidl = pidl;
    return 0;
}

static BOOL WINAPI SHGetPathFromIDListW_k32(PCVOID raw_pidl, PWSTR out_path)
{
    if (!raw_pidl || !out_path) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    const SHELL_PATH_PIDL *pidl = (const SHELL_PATH_PIDL *)raw_pidl;
    if (pidl->cb != sizeof(*pidl) - sizeof(pidl->terminator) ||
        pidl->magic != SHELL_PATH_PIDL_MAGIC || pidl->terminator != 0) {
        out_path[0] = 0;
        SetLastError(87);
        return FALSE;
    }
    shell_copy_wide(out_path, pidl->path, 260);
    SetLastError(0);
    return TRUE;
}

static int WINAPI SHCreateDirectoryExA_k32(HWND hwnd, PCSTR path,
                                            PVOID security_attributes)
{
    (void)hwnd;
    if (!path || !*path) return 87; /* ERROR_INVALID_PARAMETER */

    char partial[260];
    DWORD length = 0;
    while (path[length]) {
        if (length >= sizeof(partial) - 1)
            return 206; /* ERROR_FILENAME_EXCED_RANGE */
        partial[length] = path[length] == '/' ? '\\' : path[length];
        length++;
    }
    while (length > 0 && partial[length - 1] == '\\') length--;
    partial[length] = 0;
    if (!length) return 87;

    DWORD start = (length >= 2 && partial[1] == ':') ? 3 : 0;
    for (DWORD i = start; i <= length; i++) {
        if (i != length && partial[i] != '\\') continue;
        char saved = partial[i];
        partial[i] = 0;
        if (partial[0]) {
            DWORD attributes = GetFileAttributesA(partial);
            if (attributes == 0xFFFFFFFFU) {
                if (!CreateDirectoryA(partial, security_attributes)) {
                    DWORD error = GetLastError();
                    partial[i] = saved;
                    if (error != 183) return (int)error;
                }
            } else if (!(attributes & 0x10U)) {
                partial[i] = saved;
                return 80; /* ERROR_FILE_EXISTS */
            } else if (i == length) {
                partial[i] = saved;
                return 183; /* ERROR_ALREADY_EXISTS */
            }
        }
        partial[i] = saved;
    }
    return 0;
}

static DWORD shell_ensure_special_folder_w(HWND hwnd, PCWSTR path)
{
    char narrow_path[260];
    DWORD i = 0;
    if (!path || !*path) return 87; /* ERROR_INVALID_PARAMETER */
    while (path[i]) {
        if (i >= sizeof(narrow_path) - 1 || path[i] > 0x7F)
            return 206; /* ERROR_FILENAME_EXCED_RANGE */
        narrow_path[i] = (char)path[i];
        i++;
    }
    narrow_path[i] = 0;

    int result = SHCreateDirectoryExA_k32(hwnd, narrow_path, NULL);
    return result == 0 || result == 183 ? 0 : (DWORD)result;
}

static BOOL WINAPI SHGetSpecialFolderPathW_k32(HWND hwnd, PWSTR out_path,
                                                int csidl, BOOL create)
{
    if (!out_path) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    int requested_csidl = csidl | (create ? 0x8000 : 0);
    HRESULT status = SHGetFolderPathW_k32(hwnd, requested_csidl, NULL, 0,
                                           out_path);
    if (status != 0) {
        SetLastError((DWORD)status & 0xFFFFU);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SHGetSpecialFolderPathA_k32(HWND hwnd, PSTR out_path,
                                                int csidl, BOOL create)
{
    if (!out_path) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    int requested_csidl = csidl | (create ? 0x8000 : 0);
    HRESULT status = SHGetFolderPathA_k32(hwnd, requested_csidl, NULL, 0,
                                           out_path);
    if (status != 0) {
        SetLastError((DWORD)status & 0xFFFFU);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

PVOID WINAPI CommandLineToArgvW(PCWSTR cmd, int *argc_out)
{
    if (!cmd || !argc_out) return NULL;

    SIZE_T len = 0;
    while (cmd[len]) len++;

    SIZE_T pointer_size = g_compat32_mode ? sizeof(uint32_t) : sizeof(ULONG_PTR);
    SIZE_T pointer_bytes = (len + 2) * pointer_size;
    BYTE *block = (BYTE *)LocalAlloc(0x40, pointer_bytes + (len + 2) * sizeof(WCHAR));
    if (!block) return NULL;

    WCHAR *out = (WCHAR *)(block + pointer_bytes);
    const WCHAR *p = cmd;
    int argc = 0;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (g_compat32_mode)
            ((uint32_t *)block)[argc] = (uint32_t)(ULONG_PTR)out;
        else
            ((ULONG_PTR *)block)[argc] = (ULONG_PTR)out;
        argc++;
        int quoted = 0;
        while (*p) {
            if (*p == '\\') {
                int slashes = 0;
                while (*p == '\\') { slashes++; p++; }
                if (*p == '"') {
                    for (int i = 0; i < slashes / 2; i++) *out++ = '\\';
                    if (slashes & 1) *out++ = *p++;
                    else { quoted = !quoted; p++; }
                } else {
                    while (slashes--) *out++ = '\\';
                }
            } else if (*p == '"') {
                quoted = !quoted;
                p++;
            } else if (!quoted && (*p == ' ' || *p == '\t')) {
                break;
            } else {
                *out++ = *p++;
            }
        }
        *out++ = 0;
    }

    if (g_compat32_mode)
        ((uint32_t *)block)[argc] = 0;
    else
        ((ULONG_PTR *)block)[argc] = 0;
    *argc_out = argc;
    serial_puts("[SHELL32] CommandLineToArgvW argc=");
    serial_putdec((uint64_t)argc);
    serial_puts(" argv=0x");
    serial_puthex((uint32_t)(ULONG_PTR)block, 8);
    serial_puts(" argv[0]=0x");
    serial_puthex(g_compat32_mode ? ((uint32_t *)block)[0]
                                  : ((ULONG_PTR *)block)[0],
                  g_compat32_mode ? 8 : 16);
    serial_puts("\n");
    return block;
}

/* ── Export table ──────────────────────────────────────────── */

static WCHAR shlwapi_lower(WCHAR c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static PWSTR WINAPI shlwapi_PathFindExtensionW(PCWSTR path)
{
    if (!path) return NULL;
    PCWSTR end = path;
    PCWSTR extension = NULL;
    for (; *end; end++) {
        if (*end == '\\' || *end == '/') extension = NULL;
        else if (*end == '.') extension = end;
    }
    return (PWSTR)(extension ? extension : end);
}

static PWSTR WINAPI shlwapi_PathFindFileNameW(PCWSTR path)
{
    if (!path) return NULL;
    PCWSTR name = path;
    for (PCWSTR p = path; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    return (PWSTR)name;
}

static void WINAPI shlwapi_PathRemoveExtensionW(PWSTR path)
{
    PWSTR extension = shlwapi_PathFindExtensionW(path);
    if (extension && *extension == '.') *extension = 0;
}

static BOOL shlwapi_match_one(PCWSTR file, PCWSTR spec, PCWSTR spec_end)
{
    PCWSTR star = NULL;
    PCWSTR retry = NULL;
    while (*file) {
        if (spec < spec_end &&
            (*spec == '?' || shlwapi_lower(*spec) == shlwapi_lower(*file))) {
            spec++;
            file++;
        } else if (spec < spec_end && *spec == '*') {
            star = ++spec;
            retry = file;
        } else if (star) {
            spec = star;
            file = ++retry;
        } else {
            return FALSE;
        }
    }
    while (spec < spec_end && *spec == '*') spec++;
    return spec == spec_end;
}

static BOOL WINAPI shlwapi_PathMatchSpecW(PCWSTR file, PCWSTR specs)
{
    if (!file || !specs) return FALSE;
    while (*specs) {
        PCWSTR end = specs;
        while (*end && *end != ';') end++;
        if (shlwapi_match_one(file, specs, end)) return TRUE;
        specs = *end ? end + 1 : end;
    }
    return FALSE;
}

static HRESULT WINAPI shlwapi_AssocQueryStringW(
    DWORD flags, DWORD query, PCWSTR assoc, PCWSTR extra,
    PWSTR output, DWORD *output_chars)
{
    (void)flags; (void)query; (void)assoc; (void)extra;
    if (output && output_chars && *output_chars) output[0] = 0;
    if (output_chars) *output_chars = 0;
    return (HRESULT)0x80070483;
}

typedef struct {
    const GUID *iid;
    int offset;
} SHLWAPI_QITAB;

static int shlwapi_guid_equal(const GUID *left, const GUID *right)
{
    if (!left || !right) return 0;
    const BYTE *a = (const BYTE *)left;
    const BYTE *b = (const BYTE *)right;
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static HRESULT WINAPI shlwapi_QISearch(PVOID object,
                                        const SHLWAPI_QITAB *table,
                                        REFIID iid, PVOID *result)
{
    static const GUID iid_iunknown = {
        0x00000000, 0x0000, 0x0000,
        { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
    };
    if (!result) return (HRESULT)0x80070057;
    *result = NULL;
    if (!object || !table || !iid || !table[0].iid)
        return (HRESULT)0x80004002;

    const SHLWAPI_QITAB *entry = table;
    if (!shlwapi_guid_equal(iid, &iid_iunknown)) {
        while (entry->iid && !shlwapi_guid_equal(iid, entry->iid)) entry++;
        if (!entry->iid) return (HRESULT)0x80004002;
    }

    PVOID iface = (BYTE *)object + entry->offset;
    PVOID *vtbl = *(PVOID **)iface;
    if (!vtbl || !vtbl[1]) return (HRESULT)0x80004002;
    ((ULONG (WINAPI *)(PVOID))vtbl[1])(iface);
    *result = iface;
    return 0;
}

static PVOID WINAPI shlwapi_SHCreateMemStream(const BYTE *data, UINT size)
{
    (void)data; (void)size;
    return NULL;
}

static BOOL WINAPI shlwapi_IsOS(DWORD os)
{
    (void)os;
    return FALSE;
}

static LONG WINAPI shlwapi_SHDeleteKeyA(HKEY key, PCSTR subkey)
{
    return RegDeleteTreeA(key, subkey);
}

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT shell32_exports[] = {
    { "ShellExecuteA",      (PVOID)ShellExecuteA,     6, CC_STDCALL },
    { "ShellExecuteW",      (PVOID)ShellExecuteW,     6, CC_STDCALL },
    { "CommandLineToArgvW", (PVOID)CommandLineToArgvW,2, CC_STDCALL },
    { "Shell_NotifyIconA",  (PVOID)Shell_NotifyIconA, 2, CC_STDCALL },
    { "Shell_NotifyIconW",  (PVOID)Shell_NotifyIconW, 2, CC_STDCALL },
    { "DragAcceptFiles",    (PVOID)DragAcceptFiles_k32, 2, CC_STDCALL },
    { "DragQueryFileA",     (PVOID)DragQueryFileA_k32, 4, CC_STDCALL },
    { "DragQueryFileW",     (PVOID)DragQueryFileW_k32, 4, CC_STDCALL },
    { "DragFinish",         (PVOID)DragFinish_k32,     1, CC_STDCALL },
    { "SHGetFileInfoW",     (PVOID)SHGetFileInfoW_k32, 5, CC_STDCALL },
    { "SHAppBarMessage",    (PVOID)SHAppBarMessage_k32, 2, CC_STDCALL },
    { "IsUserAnAdmin",      (PVOID)IsUserAnAdmin_k32, 0, CC_STDCALL },
    { "SHCreateDirectoryExA", (PVOID)SHCreateDirectoryExA_k32, 3, CC_STDCALL },
    { "SHGetFolderPathA",   (PVOID)SHGetFolderPathA_k32, 5, CC_STDCALL },
    { "SHGetFolderPathW",   (PVOID)SHGetFolderPathW_k32, 5, CC_STDCALL },
    { "SHGetSettings",      (PVOID)SHGetSettings_k32,    2, CC_STDCALL },
    { "SHGetSpecialFolderPathA", (PVOID)SHGetSpecialFolderPathA_k32, 4, CC_STDCALL },
    { "SHGetSpecialFolderPathW", (PVOID)SHGetSpecialFolderPathW_k32, 4, CC_STDCALL },
    { "SHGetKnownFolderPath", (PVOID)SHGetKnownFolderPath_k32, 4, CC_STDCALL },
    { "SHGetSpecialFolderLocation", (PVOID)SHGetSpecialFolderLocation_k32, 3, CC_STDCALL },
    { "SHGetPathFromIDListW",       (PVOID)SHGetPathFromIDListW_k32, 2, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

static const SHIM_EXPORT shlwapi_exports[] = {
    { "AssocQueryStringW",    (PVOID)shlwapi_AssocQueryStringW,    6, CC_STDCALL },
    { "PathFindExtensionW",   (PVOID)shlwapi_PathFindExtensionW,   1, CC_STDCALL },
    { "PathFindFileNameW",    (PVOID)shlwapi_PathFindFileNameW,    1, CC_STDCALL },
    { "PathMatchSpecW",       (PVOID)shlwapi_PathMatchSpecW,       2, CC_STDCALL },
    { "PathRemoveExtensionW", (PVOID)shlwapi_PathRemoveExtensionW, 1, CC_STDCALL },
    { "SHCreateMemStream",    (PVOID)shlwapi_SHCreateMemStream,    2, CC_STDCALL },
    { "QISearch",             (PVOID)shlwapi_QISearch,             4, CC_STDCALL },
    { "IsOS",                 (PVOID)shlwapi_IsOS,                 1, CC_STDCALL },
    { "SHDeleteKeyA",         (PVOID)shlwapi_SHDeleteKeyA,         2, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *shell32_abi_table(int *count) {
    *count = (int)(sizeof(shell32_exports)/sizeof(shell32_exports[0]));
    return (const WIN32_EXPORT *)shell32_exports;
}

const WIN32_EXPORT *shlwapi_abi_table(int *count) {
    *count = (int)(sizeof(shlwapi_exports)/sizeof(shlwapi_exports[0]));
    return (const WIN32_EXPORT *)shlwapi_exports;
}

static int shell_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID shell32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal)
        return ordinal == 680 ? (PVOID)IsUserAnAdmin_k32 : NULL;
    for (int i = 0; shell32_exports[i].name; i++) {
        if (shell_strcmp(func_name, shell32_exports[i].name) == 0)
            return shell32_exports[i].func;
    }
    return NULL;
}

PVOID shlwapi_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) {
        if (ordinal == 12)  return (PVOID)shlwapi_SHCreateMemStream;
        if (ordinal == 219) return (PVOID)shlwapi_QISearch;
        if (ordinal == 437) return (PVOID)shlwapi_IsOS;
        return NULL;
    }
    for (int i = 0; shlwapi_exports[i].name; i++) {
        if (shell_strcmp(func_name, shlwapi_exports[i].name) == 0)
            return shlwapi_exports[i].func;
    }
    return NULL;
}

void shell32_release_process(DWORD process_id)
{
    for (UINT i = 0; i < SHELL_NOTIFY_SLOTS; i++) {
        HANDLE icon = NULL;
        uint64_t flags = shell_notify_lock_irqsave();
        SHELL_NOTIFY_ENTRY *entry = &shell_notify_entries[i];
        if (entry->used && entry->owner_pid == process_id) {
            icon = entry->icon;
            shell_zero(entry, sizeof(*entry));
        }
        shell_notify_unlock_irqrestore(flags);
        if (icon) user32_release_icon(icon);
    }
}

static void shell32_release_all(void)
{
    for (UINT i = 0; i < SHELL_NOTIFY_SLOTS; i++) {
        HANDLE icon = NULL;
        uint64_t flags = shell_notify_lock_irqsave();
        SHELL_NOTIFY_ENTRY *entry = &shell_notify_entries[i];
        if (entry->used) {
            icon = entry->icon;
            shell_zero(entry, sizeof(*entry));
        }
        shell_notify_unlock_irqrestore(flags);
        if (icon) user32_release_icon(icon);
    }
}

PVOID shell32_shim_init(void)
{
    shell32_release_all();
    return (PVOID)shell32_exports;
}
