/*
 * OsitoK Windows Compatibility Layer - comctl32.dll Shim
 */

#include "comctl32_shim.h"
#include "compat32.h"
#include "kernel32_shim.h"
#include "user32_shim.h"
#include "win32_abi.h"
#include "../kernel/smp.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t value, int digits);
extern void serial_putdec(uint64_t value);
extern void *kcalloc(uint64_t count, uint64_t size);
extern uint32_t sched_capacity_get(void);
extern int sched_current_get(void);

#define GWLP_WNDPROC              (-4)
#define MAX_SUBCLASS_WINDOWS       512
#define MAX_SUBCLASS_ENTRIES       2048

typedef struct {
    const char *name;
    DWORD groups;
    DWORD style;
    int cb_wnd_extra32;
    int cb_wnd_extra64;
    ULONG_PTR background;
} COMMON_CONTROL_CLASS;

/* Class metadata matches the v5 common-controls classes exposed by current
 * 32-bit and 64-bit Windows.  The control state stored in cbWndExtra follows
 * the caller ABI, while the class WNDPROC remains native inside the shim. */
static const COMMON_CONTROL_CLASS common_control_classes[] = {
    { "SysListView32",       ICC_LISTVIEW_CLASSES,   0x00004008, 4,  8,  6 },
    { "SysHeader32",         ICC_LISTVIEW_CLASSES,   0x00004008, 4,  8, 16 },
    { "SysTreeView32",       ICC_TREEVIEW_CLASSES,   0x00004008, 4,  8,  0 },
    { "ToolbarWindow32",     ICC_BAR_CLASSES,        0x00004008, 4,  8, 16 },
    { "msctls_statusbar32",  ICC_BAR_CLASSES,        0x00004009, 4,  8, 16 },
    { "msctls_trackbar32",   ICC_BAR_CLASSES,        0x00004000, 4,  8, 16 },
    { "tooltips_class32",    ICC_BAR_CLASSES,        0x00004808, 4,  8,  0 },
    { "SysTabControl32",     ICC_TAB_CLASSES,        0x0000400B, 4,  8, 16 },
    { "msctls_updown32",     ICC_UPDOWN_CLASS,       0x00004003, 4,  8, 16 },
    { "msctls_progress32",   ICC_PROGRESS_CLASS,     0x00004003, 4,  8, 16 },
    { "msctls_hotkey32",     ICC_HOTKEY_CLASS,       0x00004000, 24, 48,  0 },
    { "SysAnimate32",        ICC_ANIMATE_CLASS,      0x00004008, 4,  8, 16 },
    { "SysDateTimePick32",   ICC_DATE_CLASSES,       0x00004000, 4,  8,  6 },
    { "SysMonthCal32",       ICC_DATE_CLASSES,       0x00004000, 4,  8,  6 },
    { "ComboBoxEx32",        ICC_USEREX_CLASSES,     0x00004000, 4,  8,  6 },
    { "ReBarWindow32",       ICC_COOL_CLASSES,       0x00004008, 4,  8, 16 },
    { "SysIPAddress32",      ICC_INTERNET_CLASSES,   0x0000400B, 4,  4,  6 },
    { "SysPager",            ICC_PAGESCROLLER_CLASS, 0x00004000, 4,  8, 16 },
    { "NativeFontCtl",       ICC_NATIVEFNTCTL_CLASS, 0x00004000, 4,  8, 16 },
    { "SysLink",             ICC_LINK_CLASS,         0x00004008, 4,  8,  6 },
};

typedef LRESULT (WINAPI *SUBCLASS_PROC)(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam, ULONG_PTR subclass_id,
    ULONG_PTR reference_data);

typedef struct {
    BOOL used;
    DWORD owner_pid;
    HWND window;
    ULONG_PTR original_proc;
    ULONG_PTR wrapper_proc;
    uint32_t generation;
} SUBCLASS_WINDOW;

typedef struct {
    BOOL used;
    int window_slot;
    uint32_t window_generation;
    ULONG_PTR proc;
    ULONG_PTR subclass_id;
    ULONG_PTR reference_data;
    uint64_t sequence;
} SUBCLASS_ENTRY;

typedef struct subclass_dispatch_frame {
    struct subclass_dispatch_frame *previous;
    DWORD owner_pid;
    HWND window;
    ULONG_PTR original_proc;
    uint64_t next_sequence;
} SUBCLASS_DISPATCH_FRAME;

static SUBCLASS_WINDOW subclass_windows[MAX_SUBCLASS_WINDOWS];
static SUBCLASS_ENTRY subclass_entries[MAX_SUBCLASS_ENTRIES];
static spinlock_t subclass_lock = SPINLOCK_INIT;
static uint64_t subclass_sequence;
static uint32_t subclass_wrapper_thunk;

static SUBCLASS_DISPATCH_FRAME **subclass_frames;
static uint32_t subclass_frame_capacity;
static volatile uint32_t subclass_frame_table_lock;
static uint32_t subclass_trace_count;

static BOOL subclass_trace_begin(const char *operation)
{
    if (subclass_trace_count++ >= 96)
        return FALSE;
    serial_puts("[COMCTL32-SUBCLASS] ");
    serial_puts(operation);
    return TRUE;
}

static uint64_t subclass_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&subclass_lock);
    return flags;
}

static void subclass_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&subclass_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static int subclass_frame_table_ensure(void)
{
    if (__atomic_load_n(&subclass_frames, __ATOMIC_ACQUIRE))
        return 1;

    while (__sync_lock_test_and_set(&subclass_frame_table_lock, 1))
        __asm__ volatile ("pause" ::: "memory");
    if (!subclass_frames) {
        uint32_t capacity = sched_capacity_get();
        SUBCLASS_DISPATCH_FRAME **frames =
            (SUBCLASS_DISPATCH_FRAME **)kcalloc(capacity, sizeof(*frames));
        if (!capacity || !frames) {
            __sync_lock_release(&subclass_frame_table_lock);
            return 0;
        }
        subclass_frame_capacity = capacity;
        __atomic_store_n(&subclass_frames, frames, __ATOMIC_RELEASE);
    }
    __sync_lock_release(&subclass_frame_table_lock);
    return 1;
}

static int subclass_frame_owner(void)
{
    if (!subclass_frame_table_ensure())
        return -1;
    int owner = sched_current_get();
    return owner >= 0 && (uint32_t)owner < subclass_frame_capacity
         ? owner : -1;
}

static int subclass_find_window_locked(DWORD owner_pid, HWND window)
{
    for (int i = 0; i < MAX_SUBCLASS_WINDOWS; i++)
        if (subclass_windows[i].used &&
            subclass_windows[i].owner_pid == owner_pid &&
            subclass_windows[i].window == window)
            return i;
    return -1;
}

static int subclass_find_entry_locked(int window_slot, uint32_t generation,
                                      ULONG_PTR proc, ULONG_PTR subclass_id)
{
    for (int i = 0; i < MAX_SUBCLASS_ENTRIES; i++)
        if (subclass_entries[i].used &&
            subclass_entries[i].window_slot == window_slot &&
            subclass_entries[i].window_generation == generation &&
            subclass_entries[i].proc == proc &&
            subclass_entries[i].subclass_id == subclass_id)
            return i;
    return -1;
}

static ULONG_PTR subclass_wrapper_address(void);

static LRESULT subclass_call_proc(ULONG_PTR proc, HWND window, UINT message,
                                  WPARAM wparam, LPARAM lparam,
                                  ULONG_PTR subclass_id,
                                  ULONG_PTR reference_data)
{
    if (!proc)
        return DefWindowProcW(window, message, wparam, lparam);

    if (g_compat32_mode) {
        uint32_t args[6] = {
            (uint32_t)(ULONG_PTR)window,
            message,
            (uint32_t)wparam,
            (uint32_t)lparam,
            (uint32_t)subclass_id,
            (uint32_t)reference_data,
        };
        uint32_t stack_top = compat32_current_user_stack_top();
        return stack_top
             ? (LRESULT)compat32_callback_args_on_stack(
                   (uint32_t)proc, 6, args, stack_top)
             : (LRESULT)compat32_callback_args((uint32_t)proc, 6, args);
    }

    return ((SUBCLASS_PROC)proc)(window, message, wparam, lparam,
                                 subclass_id, reference_data);
}

static LRESULT subclass_call_original(ULONG_PTR proc, HWND window,
                                      UINT message, WPARAM wparam,
                                      LPARAM lparam)
{
    if (!proc)
        return DefWindowProcW(window, message, wparam, lparam);

    if (g_compat32_mode) {
        uint32_t args[4] = {
            (uint32_t)(ULONG_PTR)window,
            message,
            (uint32_t)wparam,
            (uint32_t)lparam,
        };
        uint32_t stack_top = compat32_current_user_stack_top();
        return stack_top
             ? (LRESULT)compat32_callback_args_on_stack(
                   (uint32_t)proc, 4, args, stack_top)
             : (LRESULT)compat32_callback_args((uint32_t)proc, 4, args);
    }

    return ((WNDPROC)proc)(window, message, wparam, lparam);
}

static LRESULT subclass_dispatch_next(SUBCLASS_DISPATCH_FRAME *frame,
                                      UINT message, WPARAM wparam,
                                      LPARAM lparam)
{
    ULONG_PTR proc = 0;
    ULONG_PTR subclass_id = 0;
    ULONG_PTR reference_data = 0;
    uint64_t selected_sequence = 0;

    uint64_t flags = subclass_lock_irqsave();
    int window_slot = subclass_find_window_locked(frame->owner_pid,
                                                   frame->window);
    if (window_slot >= 0) {
        uint32_t generation = subclass_windows[window_slot].generation;
        for (int i = 0; i < MAX_SUBCLASS_ENTRIES; i++) {
            SUBCLASS_ENTRY *entry = &subclass_entries[i];
            if (!entry->used || entry->window_slot != window_slot ||
                entry->window_generation != generation ||
                entry->sequence >= frame->next_sequence ||
                entry->sequence <= selected_sequence)
                continue;
            proc = entry->proc;
            subclass_id = entry->subclass_id;
            reference_data = entry->reference_data;
            selected_sequence = entry->sequence;
        }
    }
    if (selected_sequence)
        frame->next_sequence = selected_sequence;
    subclass_unlock_irqrestore(flags);

    if (selected_sequence)
    {
        if (subclass_trace_begin("dispatch hwnd=0x")) {
            serial_puthex((uint64_t)(ULONG_PTR)frame->window, 8);
            serial_puts(" msg=0x");
            serial_puthex(message, 4);
            serial_puts(" proc=0x");
            serial_puthex(proc, 8);
            serial_puts(" id=");
            serial_putdec(subclass_id);
            serial_puts(" seq=");
            serial_putdec(selected_sequence);
            serial_puts("\n");
        }
        return subclass_call_proc(proc, frame->window, message, wparam,
                                  lparam, subclass_id, reference_data);
    }
    if (subclass_trace_begin("original hwnd=0x")) {
        serial_puthex((uint64_t)(ULONG_PTR)frame->window, 8);
        serial_puts(" msg=0x");
        serial_puthex(message, 4);
        serial_puts(" proc=0x");
        serial_puthex(frame->original_proc, 8);
        serial_puts("\n");
    }
    return subclass_call_original(frame->original_proc, frame->window,
                                  message, wparam, lparam);
}

static void subclass_forget_window(DWORD owner_pid, HWND window,
                                   BOOL restore_proc)
{
    ULONG_PTR original_proc = 0;
    ULONG_PTR wrapper_proc = 0;
    uint64_t flags = subclass_lock_irqsave();
    int slot = subclass_find_window_locked(owner_pid, window);
    if (slot >= 0) {
        uint32_t generation = subclass_windows[slot].generation;
        original_proc = subclass_windows[slot].original_proc;
        wrapper_proc = subclass_windows[slot].wrapper_proc;
        for (int i = 0; i < MAX_SUBCLASS_ENTRIES; i++)
            if (subclass_entries[i].used &&
                subclass_entries[i].window_slot == slot &&
                subclass_entries[i].window_generation == generation)
                subclass_entries[i].used = FALSE;
        subclass_windows[slot].used = FALSE;
    }
    subclass_unlock_irqrestore(flags);

    if (restore_proc && wrapper_proc && IsWindow(window) &&
        (ULONG_PTR)GetWindowLongPtrW(window, GWLP_WNDPROC) == wrapper_proc)
        SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)original_proc);
}

static LRESULT WINAPI subclass_window_proc(HWND window, UINT message,
                                           WPARAM wparam, LPARAM lparam)
{
    DWORD owner_pid = GetCurrentProcessId();
    ULONG_PTR original_proc = 0;
    uint64_t flags = subclass_lock_irqsave();
    int slot = subclass_find_window_locked(owner_pid, window);
    if (slot >= 0)
        original_proc = subclass_windows[slot].original_proc;
    subclass_unlock_irqrestore(flags);

    if (slot < 0)
        return DefWindowProcW(window, message, wparam, lparam);

    SUBCLASS_DISPATCH_FRAME frame = {
        .previous = NULL,
        .owner_pid = owner_pid,
        .window = window,
        .original_proc = original_proc,
        .next_sequence = UINT64_MAX,
    };
    int owner = subclass_frame_owner();
    if (owner >= 0) {
        frame.previous = subclass_frames[owner];
        subclass_frames[owner] = &frame;
    }

    LRESULT result = subclass_dispatch_next(&frame, message, wparam, lparam);

    if (owner >= 0 && subclass_frames[owner] == &frame)
        subclass_frames[owner] = frame.previous;
    if (message == WM_NCDESTROY)
        subclass_forget_window(owner_pid, window, FALSE);
    return result;
}

static ULONG_PTR subclass_wrapper_address(void)
{
    if (!g_compat32_mode)
        return (ULONG_PTR)subclass_window_proc;
    if (!subclass_wrapper_thunk)
        subclass_wrapper_thunk = compat32_make_thunk(
            (uint64_t)(ULONG_PTR)subclass_window_proc,
            "Comctl32SubclassWndProc", 4);
    return subclass_wrapper_thunk;
}

BOOL WINAPI shim_SetWindowSubclass(HWND window, PVOID subclass_proc,
                                   ULONG_PTR subclass_id,
                                   ULONG_PTR reference_data)
{
    if (!window || !subclass_proc || !IsWindow(window)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD owner_pid = GetCurrentProcessId();
    ULONG_PTR wrapper_proc = subclass_wrapper_address();
    if (!wrapper_proc) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    BOOL new_window = FALSE;
    int window_slot = -1;
    int entry_slot = -1;
    uint64_t flags = subclass_lock_irqsave();
    window_slot = subclass_find_window_locked(owner_pid, window);
    if (window_slot < 0) {
        for (int i = 0; i < MAX_SUBCLASS_WINDOWS; i++)
            if (!subclass_windows[i].used) {
                window_slot = i;
                break;
            }
        if (window_slot >= 0) {
            SUBCLASS_WINDOW *record = &subclass_windows[window_slot];
            record->used = TRUE;
            record->owner_pid = owner_pid;
            record->window = window;
            record->original_proc =
                (ULONG_PTR)GetWindowLongPtrW(window, GWLP_WNDPROC);
            record->wrapper_proc = wrapper_proc;
            if (++record->generation == 0)
                record->generation = 1;
            new_window = TRUE;
        }
    }

    if (window_slot >= 0) {
        SUBCLASS_WINDOW *record = &subclass_windows[window_slot];
        entry_slot = subclass_find_entry_locked(window_slot,
                                                 record->generation,
                                                 (ULONG_PTR)subclass_proc,
                                                 subclass_id);
        if (entry_slot >= 0) {
            subclass_entries[entry_slot].reference_data = reference_data;
        } else {
            for (int i = 0; i < MAX_SUBCLASS_ENTRIES; i++)
                if (!subclass_entries[i].used) {
                    entry_slot = i;
                    break;
                }
            if (entry_slot >= 0) {
                SUBCLASS_ENTRY *entry = &subclass_entries[entry_slot];
                entry->used = TRUE;
                entry->window_slot = window_slot;
                entry->window_generation = record->generation;
                entry->proc = (ULONG_PTR)subclass_proc;
                entry->subclass_id = subclass_id;
                entry->reference_data = reference_data;
                entry->sequence = ++subclass_sequence;
            }
        }
    }
    subclass_unlock_irqrestore(flags);

    if (window_slot < 0 || entry_slot < 0) {
        if (new_window)
            subclass_forget_window(owner_pid, window, FALSE);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    if (new_window)
        SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)wrapper_proc);
    if (subclass_trace_begin("set hwnd=0x")) {
        serial_puthex((uint64_t)(ULONG_PTR)window, 8);
        serial_puts(" proc=0x");
        serial_puthex((ULONG_PTR)subclass_proc, 8);
        serial_puts(" id=");
        serial_putdec(subclass_id);
        serial_puts(" ref=0x");
        serial_puthex(reference_data, 8);
        serial_puts(new_window ? " install=1\n" : " install=0\n");
    }
    return TRUE;
}

BOOL WINAPI shim_GetWindowSubclass(HWND window, PVOID subclass_proc,
                                   ULONG_PTR subclass_id,
                                   ULONG_PTR *reference_data)
{
    if (!window || !subclass_proc) {
        SetLastError(87);
        return FALSE;
    }

    BOOL found = FALSE;
    uint64_t flags = subclass_lock_irqsave();
    int window_slot = subclass_find_window_locked(GetCurrentProcessId(),
                                                   window);
    if (window_slot >= 0) {
        SUBCLASS_WINDOW *record = &subclass_windows[window_slot];
        int entry_slot = subclass_find_entry_locked(
            window_slot, record->generation, (ULONG_PTR)subclass_proc,
            subclass_id);
        if (entry_slot >= 0) {
            if (reference_data)
                *reference_data = subclass_entries[entry_slot].reference_data;
            found = TRUE;
        }
    }
    subclass_unlock_irqrestore(flags);
    return found;
}

BOOL WINAPI shim_RemoveWindowSubclass(HWND window, PVOID subclass_proc,
                                      ULONG_PTR subclass_id)
{
    DWORD owner_pid = GetCurrentProcessId();
    BOOL found = FALSE;
    BOOL empty = FALSE;
    uint64_t flags = subclass_lock_irqsave();
    int window_slot = subclass_find_window_locked(owner_pid, window);
    if (window_slot >= 0) {
        SUBCLASS_WINDOW *record = &subclass_windows[window_slot];
        int entry_slot = subclass_find_entry_locked(
            window_slot, record->generation, (ULONG_PTR)subclass_proc,
            subclass_id);
        if (entry_slot >= 0) {
            subclass_entries[entry_slot].used = FALSE;
            found = TRUE;
            empty = TRUE;
            for (int i = 0; i < MAX_SUBCLASS_ENTRIES; i++)
                if (subclass_entries[i].used &&
                    subclass_entries[i].window_slot == window_slot &&
                    subclass_entries[i].window_generation ==
                        record->generation) {
                    empty = FALSE;
                    break;
                }
        }
    }
    subclass_unlock_irqrestore(flags);

    if (found && empty)
        subclass_forget_window(owner_pid, window, TRUE);
    if (subclass_trace_begin("remove hwnd=0x")) {
        serial_puthex((uint64_t)(ULONG_PTR)window, 8);
        serial_puts(" proc=0x");
        serial_puthex((ULONG_PTR)subclass_proc, 8);
        serial_puts(" id=");
        serial_putdec(subclass_id);
        serial_puts(found ? " found=1\n" : " found=0\n");
    }
    return found;
}

LRESULT WINAPI shim_DefSubclassProc(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam)
{
    int owner = subclass_frame_owner();
    SUBCLASS_DISPATCH_FRAME *frame =
        owner >= 0 ? subclass_frames[owner] : NULL;
    if (!frame || frame->owner_pid != GetCurrentProcessId() ||
        frame->window != window)
        return DefWindowProcW(window, message, wparam, lparam);
    if (subclass_trace_begin("defnext hwnd=0x")) {
        serial_puthex((uint64_t)(ULONG_PTR)window, 8);
        serial_puts(" msg=0x");
        serial_puthex(message, 4);
        serial_puts(" next_seq=");
        serial_putdec(frame->next_sequence);
        serial_puts("\n");
    }
    return subclass_dispatch_next(frame, message, wparam, lparam);
}

void comctl32_release_window(DWORD owner_pid, HWND window)
{
    subclass_forget_window(owner_pid, window, FALSE);
}

void comctl32_release_process(DWORD owner_pid)
{
    uint64_t flags = subclass_lock_irqsave();
    for (int i = 0; i < MAX_SUBCLASS_WINDOWS; i++) {
        if (!subclass_windows[i].used ||
            subclass_windows[i].owner_pid != owner_pid)
            continue;
        uint32_t generation = subclass_windows[i].generation;
        for (int j = 0; j < MAX_SUBCLASS_ENTRIES; j++)
            if (subclass_entries[j].used &&
                subclass_entries[j].window_slot == i &&
                subclass_entries[j].window_generation == generation)
                subclass_entries[j].used = FALSE;
        subclass_windows[i].used = FALSE;
    }
    subclass_unlock_irqrestore(flags);
}

static LRESULT WINAPI common_control_wndproc(HWND window, DWORD message,
                                              WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

static BOOL register_common_control_classes(DWORD groups)
{
    for (SIZE_T i = 0;
         i < sizeof(common_control_classes) / sizeof(common_control_classes[0]);
         i++) {
        const COMMON_CONTROL_CLASS *control = &common_control_classes[i];
        if (!(groups & control->groups))
            continue;
        int cb_wnd_extra = g_compat32_mode ? control->cb_wnd_extra32
                                           : control->cb_wnd_extra64;
        if (!user32_register_library_class(
                control->name, control->style, 0, cb_wnd_extra,
                (HBRUSH)control->background, common_control_wndproc)) {
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return FALSE;
        }
    }
    return TRUE;
}

void WINAPI shim_InitCommonControls(void)
{
    serial_puts("[COMCTL32] InitCommonControls flags=0x000000FF\n");
    (void)register_common_control_classes(ICC_WIN95_CLASSES);
}

BOOL WINAPI shim_InitCommonControlsEx(const INITCOMMONCONTROLSEX *icc)
{
    if (!icc || icc->dwSize != sizeof(*icc))
        return FALSE;

    serial_puts("[COMCTL32] InitCommonControlsEx flags=0x");
    serial_puthex(icc->dwICC, 8);
    serial_puts("\n");
    return register_common_control_classes(icc->dwICC);
}

PVOID WINAPI shim_CreateStatusWindowA(LONG style, const char *text,
                                       PVOID hwnd, UINT id)
{
    (void)style; (void)text; (void)hwnd; (void)id;
    serial_puts("[COMCTL32] CreateStatusWindowA (stub - NULL)\n");
    return NULL;
}

PVOID WINAPI shim_ImageList_Create(int cx, int cy, UINT flags,
                                    int initial, int grow)
{
    (void)cx; (void)cy; (void)flags; (void)initial; (void)grow;
    serial_puts("[COMCTL32] ImageList_Create (stub - NULL)\n");
    return NULL;
}

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; }
    SHIM_EXPORT;

static const SHIM_EXPORT comctl32_exports[] = {
    { "InitCommonControls",   (PVOID)shim_InitCommonControls,   0, CC_STDCALL },
    { "InitCommonControlsEx", (PVOID)shim_InitCommonControlsEx, 1, CC_STDCALL },
    { "CreateStatusWindowA",  (PVOID)shim_CreateStatusWindowA,  4, CC_STDCALL },
    { "ImageList_Create",     (PVOID)shim_ImageList_Create,     5, CC_STDCALL },
    { "SetWindowSubclass",    (PVOID)shim_SetWindowSubclass,    4, CC_STDCALL },
    { "GetWindowSubclass",    (PVOID)shim_GetWindowSubclass,    4, CC_STDCALL },
    { "RemoveWindowSubclass", (PVOID)shim_RemoveWindowSubclass, 3, CC_STDCALL },
    { "DefSubclassProc",      (PVOID)shim_DefSubclassProc,      4, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *comctl32_abi_table(int *count)
{
    *count = (int)(sizeof(comctl32_exports) / sizeof(comctl32_exports[0]));
    return (const WIN32_EXPORT *)comctl32_exports;
}

static int cc_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID comctl32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) {
        switch (ordinal) {
        case 17:  return (PVOID)shim_InitCommonControls;
        case 82:  return (PVOID)shim_InitCommonControlsEx;
        case 410: return (PVOID)shim_SetWindowSubclass;
        case 411: return (PVOID)shim_GetWindowSubclass;
        case 412: return (PVOID)shim_RemoveWindowSubclass;
        case 413: return (PVOID)shim_DefSubclassProc;
        default:  return NULL;
        }
    }
    for (int i = 0; comctl32_exports[i].name; i++)
        if (cc_strcmp(func_name, comctl32_exports[i].name) == 0)
            return comctl32_exports[i].func;
    return NULL;
}

PVOID comctl32_shim_init(void)
{
    serial_puts("[COMCTL32] comctl32.dll shim initialized\n");
    return (PVOID)comctl32_exports;
}
