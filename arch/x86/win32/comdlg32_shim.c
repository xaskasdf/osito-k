/*
 * OsitoK Windows Compatibility Layer — comdlg32.dll Shim
 * Filesystem-backed file/folder dialogs; remaining common dialogs are stubs.
 */

#include "comdlg32_shim.h"
#include "win32_abi.h"

extern void serial_puts(const char *s);

/* Shared filesystem-backed Open/Save and folder dialogs. */
#include "kernel32_shim.h"
#include "user32_shim.h"
#include "compat32.h"
#include "shell32_shim.h"
#include "ole32_shim.h"

#define CD_SETTEXT 0x000c
#define CD_PATH 260
#define CD_TEXT 4096
#define CD_LIST 0x460
#define CD_NAME 0x480
#define CD_LOCATION 0x481
#define CD_FILTER 0x470
#define CD_GO 0x5100
#define CD_UP 0x5101
#define CD_STATUS 0x440
#define CDM_GETSPEC (0x400+100)
#define CDM_GETFILEPATH (0x400+101)
#define CDM_GETFOLDERPATH (0x400+102)
#define CDM_SETCONTROLTEXT (0x400+104)

extern int WINAPI CompareStringOrdinal(PCWSTR, int, PCWSTR, int, BOOL);

/* Public descriptor layouts differ after every pointer on PE32. */
typedef struct {
    DWORD size, owner, instance, filter, custom, max_custom, index, file, max_file;
    DWORD file_title, max_title, initial, title, flags;
    WORD offset, extension;
    DWORD defext, data, hook, template_name, reserved, reserved_flags, flags_ex;
} CD_OPEN32;
typedef struct {
    DWORD size; ULONG_PTR owner, instance, filter, custom; DWORD max_custom, index;
    ULONG_PTR file; DWORD max_file; ULONG_PTR file_title; DWORD max_title;
    ULONG_PTR initial, title; DWORD flags; WORD offset, extension;
    ULONG_PTR defext, data, hook, template_name, reserved; DWORD reserved_flags, flags_ex;
} CD_OPEN64;
_Static_assert(sizeof(CD_OPEN32)==88 && sizeof(CD_OPEN64)==152,"OPENFILENAME layouts");
#define CD_OFFSET(compat,member) ((compat) ? __builtin_offsetof(CD_OPEN32,member) : __builtin_offsetof(CD_OPEN64,member))

typedef struct { DWORD pid, tid, error; } CD_ERROR;
static CD_ERROR cd_errors[1024];
static volatile DWORD cd_error_lock;

typedef struct {
    BOOL compat, wide, save, folder, typed;
    DWORD flags, error, filter_index, filter_count, max_file, max_title;
    PVOID descriptor, file, file_title;
    HWND owner, window, hook_window, list, name, location, filters, status;
    ULONG_PTR hook;
    LPARAM data;
    WCHAR cwd[CD_PATH], root[CD_PATH], title[256], extension[CD_PATH];
    WCHAR initial[CD_TEXT], filter[CD_TEXT], selected[CD_PATH];
    DWORD filter_offsets[128];
} CD_STATE;

static ULONG_PTR cd_read(PCVOID data, DWORD offset, UINT size)
{
    ULONG_PTR value = 0;
    memcpy(&value, (const BYTE *)data + offset, size);
    return value;
}

static BOOL cd_write(PVOID data, DWORD offset, ULONG_PTR value, UINT size, BOOL compat)
{
    if (!win32_user_range_writable((BYTE *)data + offset, size, compat)) return FALSE;
    memcpy((BYTE *)data + offset, &value, size);
    return TRUE;
}

static DWORD cd_error(BOOL store, DWORD value)
{
    DWORD pid = GetCurrentProcessId(), tid = GetCurrentThreadId(), result = 0;
    int empty = -1;
    while (__sync_lock_test_and_set(&cd_error_lock, 1)) __asm__ volatile("pause");
    for (UINT i = 0; i < 1024; i++) {
        if (cd_errors[i].pid == pid && cd_errors[i].tid == tid) {
            if (store) cd_errors[i].error = value;
            result = cd_errors[i].error;
            __sync_lock_release(&cd_error_lock);
            return result;
        }
        if (!cd_errors[i].pid && empty < 0) empty = (int)i;
    }
    if (store && empty >= 0) {
        cd_errors[empty] = (CD_ERROR){pid,tid,value}; result = value;
    } else if (store) result = 9; /* CDERR_MEMALLOCFAILURE */
    __sync_lock_release(&cd_error_lock);
    return result;
}

void comdlg32_release_process(DWORD pid)
{
    while (__sync_lock_test_and_set(&cd_error_lock, 1)) __asm__ volatile("pause");
    for (UINT i = 0; i < 1024; i++) if (cd_errors[i].pid == pid) cd_errors[i] = (CD_ERROR){0};
    __sync_lock_release(&cd_error_lock);
}

static UINT cd_wlen(PCWSTR text)
{
    UINT length = 0; if (text) while (text[length]) length++; return length;
}

static BOOL cd_copy(PWSTR output, PCWSTR input, UINT capacity)
{
    UINT length = cd_wlen(input);
    if (length >= capacity) return FALSE;
    memcpy(output, input, (SIZE_T)(length + 1) * sizeof(WCHAR)); return TRUE;
}

static BOOL cd_decode(PCVOID input, BOOL wide, PWSTR output, UINT capacity, BOOL compat)
{
    if (!capacity) return FALSE;
    output[0] = 0;
    if (!input) return TRUE;
    UINT count = 0, step = wide ? 2 : 1;
    while (count < CD_TEXT) {
        if (!win32_user_range_readable((const BYTE *)input + (SIZE_T)count * step, step, compat)) return FALSE;
        if (!cd_read(input, count * step, step)) break;
        count++;
    }
    if (count == CD_TEXT) return FALSE;
    if (wide) return cd_copy(output, input, capacity);
    return MultiByteToWideChar(0, 0, input, (int)count + 1, output, (int)capacity) != 0;
}

static UINT cd_encode(PCWSTR text, UINT length, PVOID output, UINT capacity, BOOL wide, BOOL compat)
{
    UINT required = wide ? length : (UINT)WideCharToMultiByte(0,0,text,(int)length,NULL,0,NULL,NULL);
    if (!output) return required;
    if (!required || required > capacity ||
        !win32_user_range_writable(output, (SIZE_T)required * (wide ? 2 : 1), compat)) return 0;
    if (wide) memcpy(output, text, (SIZE_T)required * sizeof(WCHAR));
    else if (!WideCharToMultiByte(0,0,text,(int)length,output,(int)required,NULL,NULL)) return 0;
    return required;
}

static UINT cd_file_part(PCWSTR path)
{
    UINT first = 0;
    for (UINT i = 0; path[i]; i++) if (path[i] == '\\' || path[i] == '/' || path[i] == ':') first = i + 1;
    return first;
}

static void cd_parent(PWSTR path)
{
    UINT length = cd_wlen(path);
    while (length > 3 && (path[length-1] == '\\' || path[length-1] == '/')) path[--length] = 0;
    UINT first = cd_file_part(path);
    if (first > 3) first--;
    if (first) path[first] = 0;
}

static BOOL cd_join(CD_STATE *state, PCWSTR name, PWSTR path)
{
    WCHAR combined[CD_PATH]; UINT count = 0, length = cd_wlen(name);
    if (!length) return cd_copy(path, state->cwd, CD_PATH);
    BOOL absolute = name[0] == '\\' || name[0] == '/' || (length > 1 && name[1] == ':');
    if (!absolute) {
        count = cd_wlen(state->cwd);
        if (count + length + 2 > CD_PATH) return FALSE;
        memcpy(combined, state->cwd, count * sizeof(WCHAR));
        if (count && combined[count-1] != '\\' && combined[count-1] != '/') combined[count++] = '\\';
    } else if (length >= CD_PATH) return FALSE;
    memcpy(combined + count, name, (length + 1) * sizeof(WCHAR));
    DWORD result = GetFullPathNameW(combined, CD_PATH, path, NULL);
    if (!result || result >= CD_PATH) return FALSE;
    if (state->folder && state->root[0]) {
        UINT root = cd_wlen(state->root);
        if (cd_wlen(path) < root || CompareStringOrdinal(path, root, state->root, root, TRUE) != 2 ||
            (path[root] && path[root] != '\\' && state->root[root-1] != '\\')) return FALSE;
    }
    return TRUE;
}

static BOOL cd_match(PCWSTR pattern, PCWSTR name)
{
    if (pattern[0] == '*' && pattern[1] == '.' && pattern[2] == '*' && !pattern[3]) return TRUE;
    PCWSTR star = NULL, restart = NULL;
    while (*name) {
        if (*pattern == '*') { star = ++pattern; restart = name; }
        else if (*pattern == '?' || (*pattern && CompareStringOrdinal(pattern,1,name,1,TRUE) == 2)) { pattern++; name++; }
        else if (star) { pattern = star; name = ++restart; }
        else return FALSE;
    }
    while (*pattern == '*') pattern++;
    return !*pattern;
}

static BOOL cd_filter(CD_STATE *state, PCWSTR name)
{
    if (!state->filter_count) return TRUE;
    UINT index = state->filter_index ? state->filter_index - 1 : 0;
    if (index >= state->filter_count) index = 0;
    PCWSTR pattern = state->filter + state->filter_offsets[2 * index + 1];
    while (*pattern) {
        WCHAR part[CD_PATH]; UINT count = 0;
        while (*pattern && *pattern != ';') { if (count < CD_PATH-1) part[count++] = *pattern; pattern++; }
        part[count] = 0;
        if (cd_match(part,name)) return TRUE;
        if (*pattern == ';') pattern++;
    }
    return FALSE;
}

static LONG_PTR cd_hook(CD_STATE *state, HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (!state->hook) return 0;
    if (state->compat) {
        DWORD args[] = {(DWORD)(ULONG_PTR)window,message,(DWORD)wp,(DWORD)lp};
        DWORD stack = compat32_current_user_stack_top();
        return (LONG)(stack ? compat32_callback_args_on_stack((DWORD)state->hook,4,args,stack) :
                              compat32_callback_args((DWORD)state->hook,4,args));
    }
    typedef LONG_PTR(WINAPI *CALLBACK)(HWND,UINT,WPARAM,LPARAM);
    return ((CALLBACK)state->hook)(window,message,wp,lp);
}

static BOOL cd_notify(CD_STATE *state, int code)
{
    if (!state->hook || !(state->flags & 0x80000)) return FALSE;
    BYTE *notice = LocalAlloc(0x40, state->compat ? 20 : 40);
    if (!notice) { state->error = 9; return TRUE; }
    UINT step = state->compat ? 4 : 8, header = state->compat ? 12 : 24;
    cd_write(notice,0,(ULONG_PTR)state->window,step,state->compat);
    cd_write(notice,2*step,(DWORD)code,4,state->compat);
    cd_write(notice,header,(ULONG_PTR)state->descriptor,step,state->compat);
    cd_write(notice,header+step,(ULONG_PTR)state->file,step,state->compat);
    SetWindowLongPtrA(state->hook_window,0,0);
    LONG_PTR result = cd_hook(state,state->hook_window,0x4e,0,(LPARAM)notice);
    BOOL veto = result && GetWindowLongPtrA(state->hook_window,0);
    LocalFree(notice); return veto;
}

static HWND cd_control(CD_STATE *state, PCSTR type, PCSTR text, UINT id,
                          DWORD style, int x, int y, int width, int height)
{
    return CreateWindowExA(0,type,text,WS_CHILD|WS_VISIBLE|style,x,y,width,height,
        state->window,(HMENU)(ULONG_PTR)id,NULL,NULL);
}

/* Native controls can call back through the PE32 bridge too. Keep message
 * buffers in the owning guest heap instead of truncating a kernel stack. */
static LRESULT cd_message(CD_STATE *state, HWND window, UINT message,
                          WPARAM wp, PVOID buffer, UINT bytes, BOOL output)
{
    if (!state->compat || (ULONG_PTR)buffer <= UINT32_MAX)
        return SendMessageW(window,message,wp,(LPARAM)buffer);
    PVOID guest = LocalAlloc(output ? 0x40 : 0,bytes);
    if (!guest) {
        if (output) memset(buffer,0,bytes);
        state->error = 9;
        return -1;
    }
    if (!output) memcpy(guest,buffer,bytes);
    LRESULT result = SendMessageW(window,message,wp,(LPARAM)guest);
    if (output) memcpy(buffer,guest,bytes);
    LocalFree(guest);
    return result;
}

static BOOL cd_refresh(CD_STATE *state)
{
    SendMessageW(state->location,CD_SETTEXT,0,(LPARAM)state->cwd);
    SendMessageW(state->list,0x184,0,0); /* LB_RESETCONTENT */
    WCHAR pattern[CD_PATH]; static const WCHAR any[] = {'*',0};
    if (!cd_join(state,any,pattern)) return FALSE;
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(pattern,&entry);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == 2 || GetLastError() == 18;
    BOOL ok = TRUE;
    do {
        if (entry.cFileName[0] == '.' && (!entry.cFileName[1] ||
            (entry.cFileName[1] == '.' && !entry.cFileName[2]))) continue;
        BOOL directory = (entry.dwFileAttributes & 0x10) != 0;
        if (!directory && ((state->folder && !(state->flags & 0x4000)) ||
            (!state->folder && !cd_filter(state,entry.cFileName)))) continue;
        if ((entry.dwFileAttributes & 6) && !(state->flags & 0x10000000)) continue;
        WCHAR label[CD_PATH];
        if (!cd_copy(label,entry.cFileName,CD_PATH)) { ok = FALSE; break; }
        UINT length = cd_wlen(label);
        if (directory && length + 1 < CD_PATH) { label[length++] = '\\'; label[length] = 0; }
        LRESULT index = cd_message(state,state->list,0x180,0,label,(length+1)*sizeof(WCHAR),FALSE);
        if (index < 0) { ok = FALSE; break; }
        SendMessageW(state->list,0x19a,(WPARAM)index,(LPARAM)entry.dwFileAttributes);
    } while (FindNextFileW(search,&entry));
    FindClose(search);
    if (!ok) state->error = 9;
    return ok;
}

static BOOL cd_accept(CD_STATE *state);
static ULONG_PTR WINAPI cd_dialog_proc(HWND window, UINT message, WPARAM wp, LPARAM lp);

static ULONG_PTR WINAPI cd_hook_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    CD_STATE *state = GetPropA(window,"Osito.FileDialog");
    if (message == WM_INITDIALOG) {
        state = (CD_STATE *)lp;
        if (!SetPropA(window,"Osito.FileDialog",state)) return FALSE;
        state->hook_window = window;
        return cd_hook(state,window,message,wp,(LPARAM)state->descriptor);
    }
    if (!state) return FALSE;
    LONG_PTR result = cd_hook(state,window,message,wp,lp);
    if (message == 0x82) RemovePropA(window,"Osito.FileDialog");
    return result;
}

static ULONG_PTR WINAPI cd_dialog_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    CD_STATE *state = GetPropA(window,"Osito.FileDialog");
    if (message == WM_INITDIALOG) {
        state = (CD_STATE *)lp; state->window = window;
        if (!SetPropA(window,"Osito.FileDialog",state)) { state->error = 9; EndDialog(window,0); return TRUE; }
        SendMessageW(window,CD_SETTEXT,0,(LPARAM)state->title);
        cd_control(state,"STATIC","Folder:",0x441,0,12,14,56,20);
        state->location = cd_control(state,"EDIT","",CD_LOCATION,WS_BORDER|WS_TABSTOP|0x80,72,10,400,24);
        cd_control(state,"BUTTON","Go",CD_GO,WS_TABSTOP,482,10,62,25);
        cd_control(state,"BUTTON","Up",CD_UP,WS_TABSTOP,552,10,62,25);
        state->list = cd_control(state,"LISTBOX","",CD_LIST,WS_BORDER|WS_VSCROLL|WS_TABSTOP|1|2|0x40|
            ((!state->folder && (state->flags & 0x200) && !state->save) ? 0x800 : 0),12,44,602,272);
        cd_control(state,"STATIC",state->folder?"Name:":"File name:",0x442,0,12,330,80,20);
        state->name = cd_control(state,"EDIT","",CD_NAME,WS_BORDER|WS_TABSTOP|0x80,96,325,518,25);
        state->filters = cd_control(state,"COMBOBOX","",CD_FILTER,WS_TABSTOP|WS_VSCROLL|3,96,359,360,100);
        cd_control(state,"STATIC","File type:",0x443,0,12,363,80,20);
        cd_control(state,"BUTTON",state->folder?"Select":state->save?"Save":"Open",IDOK,WS_TABSTOP|1,450,401,78,28);
        cd_control(state,"BUTTON","Cancel",IDCANCEL,WS_TABSTOP,536,401,78,28);
        state->status = cd_control(state,"STATIC","",CD_STATUS,0,12,402,325,25);
        if (!state->folder && !(state->flags & 4)) {
            HWND readonly = cd_control(state,"BUTTON","Read only",0x410,WS_TABSTOP|3,12,397,130,25);
            SendMessageA(readonly,0xf1,state->flags&1,0);
        }
        if (!state->location || !state->list || !state->name || !state->filters ||
            !GetDlgItem(window,IDOK) || !GetDlgItem(window,IDCANCEL)) {
            state->error = 7; EndDialog(window,0); return TRUE;
        }
        for (UINT i = 0; i < state->filter_count; i++)
            SendMessageW(state->filters,0x143,0,(LPARAM)(state->filter + state->filter_offsets[2*i]));
        if (!state->filter_count) {
            static const WCHAR all[] = {'A','l','l',' ','f','i','l','e','s',' ','(','*','.','*',')',0};
            cd_message(state,state->filters,0x143,0,(PVOID)all,sizeof(all),FALSE);
        }
        SendMessageA(state->filters,0x14e,state->filter_index ? state->filter_index-1 : 0,0);
        if (state->folder) {
            ShowWindow(state->filters,SW_HIDE); ShowWindow(GetDlgItem(window,0x443),SW_HIDE);
        }
        SendMessageW(state->name,CD_SETTEXT,0,(LPARAM)state->initial);
        state->typed = TRUE;
        if (!cd_refresh(state)) { if (!state->error) state->error = 2; EndDialog(window,0); return TRUE; }
        if (state->folder) cd_hook(state,window,1,0,state->data); /* BFFM_INITIALIZED */
        else if (state->hook && (state->flags & 0x80000)) {
            static const struct __attribute__((packed)) {
                DWORD style,ex_style; WORD count; short x,y,cx,cy; WORD menu,class_name,title;
            } child = {WS_CHILD,0,0,0,0,1,1,0,0,0};
            DLGPROC proc = cd_hook_proc;
            if (state->compat) proc = (DLGPROC)(ULONG_PTR)compat32_make_thunk_ex(
                (ULONG_PTR)cd_hook_proc,"COMDLG32!HookDialogProc",4,CC_STDCALL);
            state->hook_window = proc ? CreateDialogIndirectParamW(NULL,&child,window,proc,(LPARAM)state) : NULL;
            if (!state->hook_window) { state->error = 7; EndDialog(window,0); return TRUE; }
            cd_notify(state,-601); /* CDN_INITDONE */
        } else if (state->hook) {
            state->hook_window = window;
            cd_hook(state,window,WM_INITDIALOG,0,(LPARAM)state->descriptor);
        }
        SetFocus(state->name); return FALSE;
    }
    if (!state) return FALSE;
    if (!state->folder && state->hook && !(state->flags & 0x80000) &&
        cd_hook(state,window,message,wp,lp)) return TRUE;
    if (message == WM_CLOSE) { EndDialog(window,0); return TRUE; }
    if (message == 0x82) { RemovePropA(window,"Osito.FileDialog"); return FALSE; }
    if (state->folder && (message == 0x466 || message == 0x467)) {
        WCHAR path[CD_PATH],full[CD_PATH];
        BOOL valid = wp ? cd_decode((PCVOID)lp,message==0x467,path,CD_PATH,state->compat) : shell32_pidl_path((PCVOID)lp,path);
        if (valid && cd_join(state,path,full)) {
            DWORD attributes = GetFileAttributesW(full);
            if (attributes != (DWORD)-1 && (attributes & 0x10)) {
                cd_copy(state->cwd,full,CD_PATH); cd_refresh(state);
                SendMessageA(state->name,CD_SETTEXT,0,0);
            }
        }
        return TRUE;
    }
    if (state->folder && message == 0x465) { EnableWindow(GetDlgItem(window,IDOK),lp!=0); return TRUE; }
    if (state->folder && (message == 0x464 || message == 0x468)) {
        WCHAR text[CD_PATH];
        if (cd_decode((PCVOID)lp,message==0x468,text,CD_PATH,state->compat))
            cd_message(state,state->status,CD_SETTEXT,0,text,(cd_wlen(text)+1)*sizeof(WCHAR),FALSE);
        return TRUE;
    }
    if (message == CDM_SETCONTROLTEXT) {
        WCHAR text[CD_TEXT];
        if (cd_decode((PCVOID)lp,state->wide,text,CD_TEXT,state->compat))
            cd_message(state,GetDlgItem(window,(int)wp),CD_SETTEXT,0,text,(cd_wlen(text)+1)*sizeof(WCHAR),FALSE);
        return TRUE;
    }
    if (message == CDM_GETFOLDERPATH || message == CDM_GETSPEC || message == CDM_GETFILEPATH) {
        WCHAR name[CD_PATH],path[CD_PATH]; PCWSTR text = state->cwd;
        if (message != CDM_GETFOLDERPATH) {
            cd_message(state,state->name,0xd,CD_PATH,name,sizeof(name),TRUE); text = name;
            if (message == CDM_GETFILEPATH) { if (!cd_join(state,name,path)) return -1; text = path; }
        }
        UINT required = cd_encode(text,cd_wlen(text)+1,NULL,0,state->wide,state->compat);
        if (lp && wp >= required) cd_encode(text,cd_wlen(text)+1,(PVOID)lp,(UINT)wp,state->wide,state->compat);
        SetWindowLongPtrA(window,0,required); return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    UINT id = (UINT)wp & 0xffff, notification = ((UINT)wp >> 16) & 0xffff;
    if (id == IDCANCEL) { EndDialog(window,0); return TRUE; }
    if (id == CD_NAME && notification == 0x300) { state->typed = TRUE; return TRUE; }
    if (id == CD_FILTER && notification == 1) {
        LRESULT selected = SendMessageA(state->filters,0x147,0,0);
        if (selected >= 0) state->filter_index = (DWORD)selected + 1;
        cd_refresh(state); return TRUE;
    }
    if (id == CD_GO || id == CD_UP) {
        WCHAR name[CD_PATH],path[CD_PATH];
        if (id == CD_UP) { cd_copy(name,state->cwd,CD_PATH); cd_parent(name); }
        else cd_message(state,state->location,0xd,CD_PATH,name,sizeof(name),TRUE);
        if (cd_join(state,name,path)) {
            DWORD attributes = GetFileAttributesW(path);
            if (attributes != (DWORD)-1 && (attributes & 0x10)) { cd_copy(state->cwd,path,CD_PATH); cd_refresh(state); return TRUE; }
        }
        MessageBoxA(window,"This folder could not be opened.","Folder",0x10); return TRUE;
    }
    if (id == CD_LIST && (notification == 1 || notification == 2)) {
        LRESULT index = SendMessageW(state->list,0x188,0,0);
        if (index >= 0) {
            WCHAR name[CD_PATH];
            if (SendMessageW(state->list,0x18a,index,0) < CD_PATH &&
                cd_message(state,state->list,0x189,index,name,sizeof(name),TRUE) >= 0) {
                UINT length = cd_wlen(name); if (length && name[length-1] == '\\') name[length-1] = 0;
                cd_message(state,state->name,CD_SETTEXT,0,name,(cd_wlen(name)+1)*sizeof(WCHAR),FALSE); state->typed = FALSE;
                if (state->folder && state->hook) {
                    WCHAR path[CD_PATH];
                    if (cd_join(state,name,path)) {
                        PVOID pidl = shell32_make_path_pidl(path);
                        if (pidl) { cd_hook(state,window,2,(WPARAM)pidl,state->data); shim_CoTaskMemFree(pidl); }
                    }
                }
            }
        }
        if (notification != 2) return TRUE;
        id = IDOK;
    }
    if (id == IDOK) {
        if (cd_accept(state)) EndDialog(window,1);
        else if (state->error) EndDialog(window,0);
        return TRUE;
    }
    return FALSE;
}

static BOOL cd_accept(CD_STATE *state)
{
    WCHAR name[CD_PATH], path[CD_PATH], chosen[CD_TEXT]; UINT used = 0, first = 0, extension = 0;
    cd_message(state,state->name,0xd,CD_PATH,name,sizeof(name),TRUE);
    if (!name[0] && !state->folder) return FALSE;
    if (!cd_join(state,name,path)) { MessageBoxA(state->window,"This path is not valid.","File name",0x10); return FALSE; }
    DWORD attributes = GetFileAttributesW(path);
    if (attributes != (DWORD)-1 && (attributes & 0x10)) {
        if (state->folder) return cd_copy(state->selected,path,CD_PATH);
        cd_copy(state->cwd,path,CD_PATH); cd_refresh(state);
        SendMessageA(state->name,CD_SETTEXT,0,0); return FALSE;
    }
    if (state->folder) {
        if (attributes != (DWORD)-1 && (state->flags & 0x4000) && !(state->flags & 1))
            return cd_copy(state->selected,path,CD_PATH);
        MessageBoxA(state->window,"Choose an existing folder.","Folder",0x10); return FALSE;
    }
    LRESULT selection_count = !state->typed && !state->save && (state->flags & 0x200)
        ? SendMessageW(state->list,0x190,0,0) : 0;
    BOOL multiple = selection_count > 1;
    if (multiple) {
        if (!(state->flags & 0x80000)) { state->error = 2; return FALSE; }
        int *indices = LocalAlloc(0,(SIZE_T)selection_count * sizeof(int));
        if (!indices) { state->error = 9; return FALSE; }
        LRESULT count = SendMessageW(state->list,0x191,selection_count,(LPARAM)indices);
        cd_copy(chosen,state->cwd,CD_TEXT); used = cd_wlen(chosen)+1; first = used;
        for (LRESULT i = 0; i < count; i++) {
            if ((DWORD)SendMessageW(state->list,0x199,indices[i],0) & 0x10) {
                LocalFree(indices); MessageBoxA(state->window,"Choose files from one folder.","Files",0x10); return FALSE;
            }
            LRESULT length = SendMessageW(state->list,0x18a,indices[i],0);
            if (length < 0 || (UINT)length + used + 2 > CD_TEXT) {
                LocalFree(indices); state->error = 0x3003; return FALSE;
            }
            cd_message(state,state->list,0x189,indices[i],chosen+used,((UINT)length+1)*sizeof(WCHAR),TRUE);
            used += (UINT)length+1;
        }
        LocalFree(indices); chosen[used++] = 0;
    } else {
        first = cd_file_part(path); UINT length = cd_wlen(path);
        for (UINT i = first; i < length; i++) if (path[i] == '.') extension = i+1;
        if (!extension && state->extension[0] && !(state->flags & 0x100)) {
            UINT extra = cd_wlen(state->extension);
            if (length + extra + 2 > CD_PATH) { state->error = 0x3002; return FALSE; }
            path[length++] = '.'; extension = length;
            memcpy(path+length,state->extension,(extra+1)*sizeof(WCHAR));
            attributes = GetFileAttributesW(path);
        }
        BOOL exists = attributes != (DWORD)-1;
        WCHAR parent[CD_PATH]; cd_copy(parent,path,CD_PATH); cd_parent(parent);
        DWORD parent_attributes = GetFileAttributesW(parent);
        if (!(state->flags & 0x100) &&
            (((state->flags & (0x800|0x1000)) && (parent_attributes == (DWORD)-1 || !(parent_attributes & 0x10))) ||
             ((state->flags & 0x1000) && !exists))) {
            MessageBoxA(state->window,"The selected file or folder does not exist.","File name",0x10); return FALSE;
        }
        if (state->save && exists && (state->flags & 2) &&
            MessageBoxA(state->window,"The file already exists. Replace it?","Save",4|0x20) != 6) return FALSE;
        if (!exists && (state->flags & 0x2000) &&
            MessageBoxA(state->window,"The file does not exist. Use this name to create it?","Open",4|0x20) != 6) return FALSE;
        if ((state->flags & 0x8000) && exists && (attributes & 1)) {
            MessageBoxA(state->window,"The selected file is read only.","File name",0x10); return FALSE;
        }
        cd_copy(chosen,path,CD_TEXT); used = cd_wlen(chosen)+1;
    }
    UINT required = cd_encode(chosen,used,NULL,0,state->wide,state->compat);
    if (!required) { state->error = 2; return FALSE; }
    BOOL fits = required <= state->max_file;
    if (fits && !cd_encode(chosen,used,state->file,state->max_file,state->wide,state->compat)) {
        state->error = 2; return FALSE;
    }
    if (!fits) {
        cd_write(state->file,0,required > 65535 ? 65535 : required,2,state->compat);
        state->error = 0x3003;
    }
    UINT offset = state->wide ? first : cd_encode(chosen,first,NULL,0,FALSE,state->compat);
    UINT ext = !extension ? 0 : state->wide ? extension : cd_encode(chosen,extension,NULL,0,FALSE,state->compat);
    DWORD flags = state->flags;
    if (state->extension[0] && !multiple && extension &&
        CompareStringOrdinal(chosen+extension,-1,state->extension,-1,TRUE) != 2) flags |= 0x400;
    else flags &= ~0x400u;
    if (!(state->flags & 4)) {
        flags &= ~1u;
        if (SendMessageA(GetDlgItem(state->window,0x410),0xf0,0,0) == 1) flags |= 1;
    }
    if (fits) {
        if (!cd_write(state->descriptor,CD_OFFSET(state->compat,offset),offset,2,state->compat) ||
            !cd_write(state->descriptor,CD_OFFSET(state->compat,extension),multiple?0:ext,2,state->compat) ||
            !cd_write(state->descriptor,CD_OFFSET(state->compat,flags),flags,4,state->compat) ||
            !cd_write(state->descriptor,CD_OFFSET(state->compat,index),state->filter_index,4,state->compat)) {
            state->error = 2; return FALSE;
        }
        if (!multiple && state->file_title && state->max_title) {
            WCHAR title[CD_PATH];
            cd_copy(title,chosen+first,CD_PATH);
            UINT length = cd_wlen(title);
            while (length && cd_encode(title,length+1,NULL,0,state->wide,state->compat) > state->max_title)
                title[--length] = 0;
            if (!cd_encode(title,length+1,state->file_title,state->max_title,state->wide,state->compat)) {
                state->error = 2; return FALSE;
            }
        }
    }
    BOOL veto = cd_notify(state,-606); /* CDN_FILEOK */
    if (veto) { if (state->error != 9) state->error = 0; return FALSE; }
    if (state->error) return FALSE;
    if (!(state->flags & 8)) SetCurrentDirectoryW(state->cwd);
    return TRUE;
}

static BOOL cd_run(CD_STATE *state)
{
    static const struct __attribute__((packed)) {
        DWORD style,ex_style; WORD count; short x,y,cx,cy; WORD menu,class_name,title;
    } dialog = {WS_CAPTION|WS_SYSMENU|0x80|0x800,0x1,0,0,0,320,220,0,0,0};
    DLGPROC proc = cd_dialog_proc;
    if (state->compat) proc = (DLGPROC)(ULONG_PTR)compat32_make_thunk_ex(
        (ULONG_PTR)cd_dialog_proc,"COMDLG32!FileDialogProc",4,CC_STDCALL);
    LONG_PTR result = proc ? DialogBoxIndirectParamW(NULL,&dialog,state->owner,proc,(LPARAM)state) : -1;
    if (result == -1 && !state->error) state->error = 2;
    cd_error(TRUE,state->error);
    return result == 1 && !state->error;
}

static BOOL cd_file(PVOID descriptor, BOOL wide, BOOL save)
{
    BOOL compat = g_compat32_mode != 0; UINT step = compat?4:8;
    cd_error(TRUE,0);
    if (!descriptor || !win32_user_range_readable(descriptor,4,compat)) { cd_error(TRUE,2); return FALSE; }
    DWORD size = (DWORD)cd_read(descriptor,0,4);
    if (size != (compat?76u:136u) && size != (compat?88u:152u)) { cd_error(TRUE,1); return FALSE; }
    if (!win32_user_range_readable(descriptor,size,compat) || !win32_user_range_writable(descriptor,size,compat)) {
        cd_error(TRUE,2); return FALSE;
    }
    CD_STATE *state = LocalAlloc(0x40,sizeof(*state));
    if (!state) { cd_error(TRUE,9); return FALSE; }
    state->compat = compat; state->wide = wide; state->save = save; state->descriptor = descriptor;
    state->owner = (HWND)cd_read(descriptor,CD_OFFSET(compat,owner),step);
    state->flags = (DWORD)cd_read(descriptor,CD_OFFSET(compat,flags),4);
    state->file = (PVOID)cd_read(descriptor,CD_OFFSET(compat,file),step);
    state->max_file = (DWORD)cd_read(descriptor,CD_OFFSET(compat,max_file),4);
    state->file_title = (PVOID)cd_read(descriptor,CD_OFFSET(compat,file_title),step);
    state->max_title = (DWORD)cd_read(descriptor,CD_OFFSET(compat,max_title),4);
    state->filter_index = (DWORD)cd_read(descriptor,CD_OFFSET(compat,index),4);
    if (state->flags & 0x20) state->hook = cd_read(descriptor,CD_OFFSET(compat,hook),step);
    if ((state->flags & 0x20) && !state->hook) state->error = 11;
    if ((state->flags & 0xc0) || /* Custom dialog templates need a separate layout contract. */
        ((state->flags & 0x200) && !(state->flags & 0x80000)) ||
        cd_read(descriptor,CD_OFFSET(compat,custom),step)) state->error = 2;
    if (!state->file || !state->max_file ||
        !cd_decode(state->file,wide,state->initial,CD_TEXT,compat) ||
        !GetCurrentDirectoryW(CD_PATH,state->cwd)) state->error = 2;
    if (state->hook && !win32_user_range_executable((PCVOID)state->hook,1,compat)) state->error = 11;
    PCVOID initial = (PCVOID)cd_read(descriptor,CD_OFFSET(compat,initial),step);
    WCHAR initial_directory[CD_PATH], path[CD_PATH];
    if (!cd_decode(initial,wide,initial_directory,CD_PATH,compat)) state->error = 2;
    if (initial_directory[0] && cd_join(state,initial_directory,path)) {
        DWORD attrs = GetFileAttributesW(path);
        if (attrs != (DWORD)-1 && (attrs & 0x10)) cd_copy(state->cwd,path,CD_PATH);
    }
    UINT first = cd_file_part(state->initial);
    if (first && cd_join(state,state->initial,path)) {
        cd_parent(path); cd_copy(state->cwd,path,CD_PATH);
        UINT count = cd_wlen(state->initial+first)+1;
        for (UINT i = 0; i < count; i++) state->initial[i] = state->initial[first+i];
    }
    PCVOID title = (PCVOID)cd_read(descriptor,CD_OFFSET(compat,title),step);
    if (!cd_decode(title,wide,state->title,256,compat)) state->error = 2;
    if (!state->title[0]) {
        PCSTR text = save ? "Save As" : "Open";
        for (UINT i = 0; text[i]; i++) state->title[i] = (BYTE)text[i];
    }
    if (!cd_decode((PCVOID)cd_read(descriptor,CD_OFFSET(compat,defext),step),wide,state->extension,CD_PATH,compat)) state->error = 2;
    PCVOID filter = (PCVOID)cd_read(descriptor,CD_OFFSET(compat,filter),step);
    UINT stored = 0;
    while (filter && state->filter_count < 64) {
        UINT pair = state->filter_count * 2;
        if (!cd_decode(filter,wide,state->filter+stored,CD_TEXT-stored,compat)) { state->error = 2; break; }
        UINT length = cd_wlen(state->filter+stored);
        if (!length) break;
        state->filter_offsets[pair] = stored; stored += length+1;
        UINT raw = 0; while (cd_read(filter,raw*(wide?2:1),wide?2:1)) raw++;
        filter = (const BYTE *)filter + (raw+1)*(wide?2:1);
        if (stored >= CD_TEXT || !cd_decode(filter,wide,state->filter+stored,CD_TEXT-stored,compat)) { state->error = 2; break; }
        length = cd_wlen(state->filter+stored);
        if (!length) { state->error = 2; break; }
        state->filter_offsets[pair+1] = stored; stored += length+1;
        raw = 0; while (cd_read(filter,raw*(wide?2:1),wide?2:1)) raw++;
        filter = (const BYTE *)filter + (raw+1)*(wide?2:1);
        state->filter_count++;
        if (stored >= CD_TEXT) { state->error = 2; break; }
    }
    if (!state->filter_index || state->filter_index > state->filter_count) state->filter_index = 1;
    BOOL result = !state->error && cd_run(state);
    cd_error(TRUE,state->error);
    LocalFree(state);
    return result;
}

/* BROWSEINFO has no length member. Read each public field at its ABI offset;
 * the selected item is an owned, filesystem PIDL compatible with CoTaskMem. */
PVOID comdlg32_select_folder(PVOID descriptor, BOOL wide)
{
    BOOL compat = g_compat32_mode != 0;
    UINT step = compat ? 4 : 8, size = compat ? 32 : 64;
    if (!descriptor || !win32_user_range_readable(descriptor,size,compat)) {
        SetLastError(87); return NULL;
    }
    CD_STATE *state = LocalAlloc(0x40,sizeof(*state));
    if (!state) { SetLastError(8); return NULL; }
    state->folder = TRUE; state->wide = wide; state->compat = compat;
    state->descriptor = descriptor;
    state->owner = (HWND)cd_read(descriptor,0,step);
    PCVOID root = (PCVOID)cd_read(descriptor,step,step);
    PVOID display = (PVOID)cd_read(descriptor,2*step,step);
    PCVOID title = (PCVOID)cd_read(descriptor,3*step,step);
    state->flags = (DWORD)cd_read(descriptor,4*step,4);
    state->hook = cd_read(descriptor,compat?20:40,step);
    state->data = (LPARAM)cd_read(descriptor,compat?24:48,step);
    DWORD error = 0;
    if ((state->flags & (0x1000|0x2000)) != 0) error = 50; /* Virtual computer/printer namespaces. */
    else if (!cd_decode(title,wide,state->title,256,compat) ||
        (state->hook && !win32_user_range_executable((PCVOID)state->hook,1,compat)) ||
        (root && !shell32_pidl_path(root,state->root))) error = 87;
    if (!state->title[0]) {
        static const WCHAR title_default[] = {'S','e','l','e','c','t',' ','f','o','l','d','e','r',0};
        cd_copy(state->title,title_default,256);
    }
    if (state->root[0]) cd_copy(state->cwd,state->root,CD_PATH);
    else if (!GetCurrentDirectoryW(CD_PATH,state->cwd)) error = 3;
    PVOID result = NULL;
    if (!error && cd_run(state)) {
        PCWSTR name = state->selected + cd_file_part(state->selected);
        if (!*name) name = state->selected;
        if ((display && !cd_encode(name,cd_wlen(name)+1,display,260,wide,compat)) ||
            !cd_write(descriptor,compat?28:56,0,4,compat)) error = 87;
        else {
            result = shell32_make_path_pidl(state->selected);
            if (!result) error = 8;
        }
    }
    if (!error && state->error) error = 8;
    LocalFree(state);
    SetLastError(error);
    return result;
}


/* ── Common Dialogs ────────────────────────────────────────── */

BOOL WINAPI shim_GetOpenFileNameA(PVOID ofn)
{
    return cd_file(ofn,FALSE,FALSE);
}

BOOL WINAPI shim_GetOpenFileNameW(PVOID ofn)
{
    return cd_file(ofn,TRUE,FALSE);
}

BOOL WINAPI shim_GetSaveFileNameA(PVOID ofn)
{
    return cd_file(ofn,FALSE,TRUE);
}

BOOL WINAPI shim_GetSaveFileNameW(PVOID ofn)
{
    return cd_file(ofn,TRUE,TRUE);
}

BOOL WINAPI shim_ChooseColorA(PVOID cc)
{
    (void)cc;
    serial_puts("[COMDLG32] ChooseColorA (stub — cancelled)\n");
    return FALSE;
}

BOOL WINAPI shim_ChooseFontA(PVOID cf)
{
    (void)cf;
    serial_puts("[COMDLG32] ChooseFontA (stub — cancelled)\n");
    return FALSE;
}

DWORD WINAPI shim_CommDlgExtendedError(void)
{
    DWORD error = cd_error(FALSE,0);
    SetLastError(0);
    return error;
}

BOOL WINAPI shim_PrintDlgA(PVOID pd)
{
    (void)pd;
    serial_puts("[COMDLG32] PrintDlgA (stub — cancelled)\n");
    return FALSE;
}

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT comdlg32_exports[] = {
    { "GetOpenFileNameA",    (PVOID)shim_GetOpenFileNameA,     1, CC_STDCALL },
    { "GetOpenFileNameW",    (PVOID)shim_GetOpenFileNameW,     1, CC_STDCALL },
    { "GetSaveFileNameW",    (PVOID)shim_GetSaveFileNameW,     1, CC_STDCALL },
    { "GetSaveFileNameA",    (PVOID)shim_GetSaveFileNameA,     1, CC_STDCALL },
    { "ChooseColorA",        (PVOID)shim_ChooseColorA,         1, CC_STDCALL },
    { "ChooseFontA",         (PVOID)shim_ChooseFontA,          1, CC_STDCALL },
    { "CommDlgExtendedError",(PVOID)shim_CommDlgExtendedError, 0, CC_STDCALL },
    { "PrintDlgA",           (PVOID)shim_PrintDlgA,            1, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *comdlg32_abi_table(int *count) {
    *count = (int)(sizeof(comdlg32_exports)/sizeof(comdlg32_exports[0]));
    return (const WIN32_EXPORT *)comdlg32_exports;
}

static int cd_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID comdlg32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal) return NULL;
    for (int i = 0; comdlg32_exports[i].name; i++) {
        if (cd_strcmp(func_name, comdlg32_exports[i].name) == 0)
            return comdlg32_exports[i].func;
    }
    return NULL;
}

PVOID comdlg32_shim_init(void)
{
    serial_puts("[COMDLG32] comdlg32.dll shim initialized\n");
    return (PVOID)comdlg32_exports;
}
