#include "shell_fileop.h"
#include "kernel32_shim.h"
#include "compat32.h"

#define FILEOP_PATH_MAX 260
#define FILEOP_LIST_MAX 256

typedef struct {
    char pattern[FILEOP_PATH_MAX], from[FILEOP_PATH_MAX], to[FILEOP_PATH_MAX];
    WIN32_FIND_DATAA found;
} FILEOP_WALK;

static int shell_file_operation_join(char result[FILEOP_PATH_MAX],
                                      PCSTR directory, PCSTR leaf)
{
    SIZE_T a = strlen(directory), b = strlen(leaf);
    BOOL slash = a && directory[a - 1] != '\\' && directory[a - 1] != '/';
    if (a + b + (slash ? 1 : 0) >= FILEOP_PATH_MAX) return 0x7C;
    memcpy(result, directory, a);
    if (slash) result[a++] = '\\';
    memcpy(result + a, leaf, b + 1);
    return 0;
}

static int shell_file_operation_list(PCSTR list, PCSTR *entries, int *count)
{
    *count = 0;
    if (!list) return 87;
    SIZE_T total = 0;
    while (*list) {
        if (*count >= FILEOP_LIST_MAX) return 87;
        SIZE_T length = 0;
        while (length < FILEOP_PATH_MAX && list[length]) length++;
        if (length == FILEOP_PATH_MAX) return 0x7C;
        total += length + 1;
        if (total > 65535) return 87;
        entries[(*count)++] = list;
        list += length + 1;
    }
    return 0;
}

static int shell_file_operation_one(PCSTR from, PCSTR to, DWORD operation,
                                     WORD flags, unsigned depth)
{
    if (depth >= 32) return 0x79; /* DE_PATHTOODEEP */
    DWORD attributes = GetFileAttributesA(from);
    if (attributes == 0xFFFFFFFFU) return 2;
    if (operation != 3) {
        SIZE_T i = 0;
        while (from[i] && to[i]) {
            char a = from[i], b = to[i];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
            i++;
        }
        if (!from[i] && !to[i]) return 0x71; /* DE_SAMEFILE */
        if ((attributes & 16) && !from[i] && (to[i] == '\\' || to[i] == '/'))
            return 0x76; /* DE_DESTSUBTREE */
    }
    if (operation == 4 || (operation == 1 && GetFileAttributesA(to) == 0xFFFFFFFFU))
        return MoveFileExA_k32(from, to, 0) ? 0 : (int)GetLastError();
    if (!(attributes & 16)) {
        if (operation == 3) {
            if (attributes & 1) SetFileAttributesA(from, attributes & ~1U);
            return DeleteFileA(from) ? 0 : (int)GetLastError();
        }
        DWORD target = GetFileAttributesA(to);
        if (target != 0xFFFFFFFFU && !(flags & 0x10)) return 0x75; /* confirmation needed */
        if (operation == 1)
            return MoveFileExA_k32(from, to, 1 | 2 | 8) ? 0 : (int)GetLastError();
        return CopyFileA(from, to, FALSE) ? 0 : (int)GetLastError();
    }
    if (operation != 3 && !CreateDirectoryA(to, NULL) && GetLastError() != 183)
        return (int)GetLastError();
    FILEOP_WALK *walk = LocalAlloc(0x40, sizeof(*walk));
    if (!walk) return 8;
    int error = shell_file_operation_join(walk->pattern, from, "*");
    HANDLE find = INVALID_HANDLE_VALUE;
    if (!error) {
        find = FindFirstFileA(walk->pattern, &walk->found);
        if (find == INVALID_HANDLE_VALUE && GetLastError() != 2 && GetLastError() != 18)
            error = (int)GetLastError();
    }
    if (find != INVALID_HANDLE_VALUE) {
        do {
            PCSTR name = walk->found.cFileName;
            if ((name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) ||
                ((flags & 0x1000) && (walk->found.dwFileAttributes & 16))) continue;
            error = shell_file_operation_join(walk->from, from, name);
            if (!error && operation != 3)
                error = shell_file_operation_join(walk->to, to, name);
            if (!error) error = shell_file_operation_one(walk->from, walk->to,
                                                          operation, flags, depth + 1);
            if (error) break;
        } while (FindNextFileA(find, &walk->found));
        FindClose(find);
    }
    LocalFree(walk);
    if (!error && operation != 2 && !RemoveDirectoryA(from)) error = (int)GetLastError();
    return error;
}

int WINAPI shell_SHFileOperationA(PVOID operation_data)
{
    BOOL compat = g_compat32_mode != 0;
    SIZE_T size = compat ? 30 : 56;
    if (!operation_data || !win32_user_range_writable(operation_data, size, compat)) return 87;
    BYTE *data = operation_data;
    DWORD operation;
    WORD flags;
    ULONG_PTR from_list = 0, to_list = 0;
    memcpy(&operation, data + (compat ? 4 : 8), 4);
    memcpy(&from_list, data + (compat ? 8 : 16), compat ? 4 : 8);
    memcpy(&to_list, data + (compat ? 12 : 24), compat ? 4 : 8);
    memcpy(&flags, data + (compat ? 16 : 32), 2);
    DWORD aborted = FALSE;
    memcpy(data + (compat ? 18 : 36), &aborted, 4);
    int error = 0;
    /* Recycle/undo and collision-name mappings need their own persistent
     * state. Reject them instead of silently changing the requested action. */
    if (flags & (0x40 | 8 | 0x20)) error = 120;
    if (operation < 1 || operation > 4 || !from_list || (operation != 3 && !to_list)) error = 87;
    if (operation == 3 && !(flags & 0x10)) error = 0x75;
    typedef struct {
        PCSTR sources[FILEOP_LIST_MAX], targets[FILEOP_LIST_MAX];
        char from[FILEOP_PATH_MAX], to[FILEOP_PATH_MAX], destination[FILEOP_PATH_MAX];
        FILEOP_WALK walk;
    } WORK;
    WORK *work = error ? NULL : LocalAlloc(0x40, sizeof(*work));
    if (!error && !work) error = 8;
    int sources = 0, targets = 0;
    if (!error) error = shell_file_operation_list((PCSTR)from_list, work->sources, &sources);
    if (!error && operation != 3) error = shell_file_operation_list((PCSTR)to_list, work->targets, &targets);
    if (!error && sources && operation != 3 &&
        ((!targets) || ((flags & 1) ? targets != sources : targets != 1))) error = 0x72;
    if (!error && operation == 4 && sources > 1) error = 0x72;
    for (int i = 0; !error && i < sources; i++) {
        DWORD length = GetFullPathNameA(work->sources[i], FILEOP_PATH_MAX, work->from, NULL);
        if (!length || length >= FILEOP_PATH_MAX) { error = 0x7C; break; }
        BOOL wildcard = FALSE;
        PCSTR leaf = work->from;
        for (PCSTR p = work->from; *p; p++) {
            if (*p == '\\' || *p == '/') {
                if (wildcard) { error = 87; break; }
                leaf = p + 1;
            } else if (*p == '*' || *p == '?') wildcard = TRUE;
        }
        if (error) break;
        if (wildcard && operation == 4) { error = 0x72; break; }
        BOOL into_directory = FALSE;
        if (operation != 3) {
            length = GetFullPathNameA(work->targets[(flags & 1) ? i : 0], FILEOP_PATH_MAX, work->to, NULL);
            if (!length || length >= FILEOP_PATH_MAX) { error = 0x7C; break; }
            DWORD target_attributes = GetFileAttributesA(work->to);
            into_directory = operation != 4 &&
                ((target_attributes != 0xFFFFFFFFU && (target_attributes & 16)) ||
                 (!(flags & 1) && (sources > 1 || wildcard)) || work->to[length - 1] == '\\');
            /* FOF_NOCONFIRMMKDIR permits preparing missing parent directories. */
            if (flags & 0x200) {
                for (DWORD offset = 3; offset < length; offset++) {
                    if (work->to[offset] != '\\' && work->to[offset] != '/') continue;
                    char saved = work->to[offset];work->to[offset] = 0;
                    if (!CreateDirectoryA(work->to, NULL) && GetLastError() != 183) error = (int)GetLastError();
                    work->to[offset] = saved;
                    if (error) break;
                }
            }
            if (!error && into_directory && target_attributes == 0xFFFFFFFFU &&
                !CreateDirectoryA(work->to, NULL)) error = (int)GetLastError();
            if (error) break;
            if (!wildcard && into_directory) error = shell_file_operation_join(work->destination, work->to, leaf);
            else memcpy(work->destination, work->to, length + 1);
        }
        if (!wildcard) {
            if (!error) error = shell_file_operation_one(work->from, work->destination, operation, flags, 0);
            continue;
        }
        FILEOP_WALK *walk = &work->walk;
        HANDLE find = FindFirstFileA(work->from, &walk->found);
        if (find == INVALID_HANDLE_VALUE) { error = 2; break; }
        SIZE_T prefix = (SIZE_T)(leaf - work->from);
        do {
            PCSTR name = walk->found.cFileName;
            if ((name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) ||
                ((flags & 0x80) && (walk->found.dwFileAttributes & 16))) continue;
            SIZE_T name_length = strlen(name);
            if (prefix + name_length >= FILEOP_PATH_MAX) { error = 0x7C; break; }
            memcpy(walk->from, work->from, prefix);
            memcpy(walk->from + prefix, name, name_length + 1);
            if (operation != 3) error = shell_file_operation_join(walk->to, work->to, name);
            if (!error) error = shell_file_operation_one(walk->from, walk->to, operation, flags, 0);
            if (error) break;
        } while (FindNextFileA(find, &walk->found));
        FindClose(find);
    }
    if (work) LocalFree(work);
    if (error) {
        aborted = TRUE;
        memcpy(data + (compat ? 18 : 36), &aborted, 4);
    }
    return error;
}
