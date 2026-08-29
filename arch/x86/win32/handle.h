/*
 * OsitoK Windows Compatibility Layer — Handle Table
 *
 * NT-style handle table. Each process has its own table.
 * Handles are indices * 4 (matching Windows convention where
 * handles are always multiples of 4).
 *
 * Object types supported:
 *   OBJ_TYPE_FILE     — open file (backed by OsitoFS or console)
 *   OBJ_TYPE_PROCESS  — process
 *   OBJ_TYPE_THREAD   — thread
 *   OBJ_TYPE_EVENT    — synchronization event
 *   OBJ_TYPE_SECTION  — memory-mapped section (PE image)
 */

#ifndef HANDLE_H
#define HANDLE_H

#include "nttypes.h"

/* ── Object types ───────────────────────────────────────────── */

typedef enum _OBJECT_TYPE_ID {
    OBJ_TYPE_NONE     = 0,
    OBJ_TYPE_FILE     = 1,
    OBJ_TYPE_PROCESS  = 2,
    OBJ_TYPE_THREAD   = 3,
    OBJ_TYPE_EVENT    = 4,
    OBJ_TYPE_SECTION  = 5,
    OBJ_TYPE_MUTANT   = 6,
    OBJ_TYPE_SEMAPHORE = 7,
    OBJ_TYPE_SNAPSHOT = 8,
    OBJ_TYPE_JOB      = 9,
    OBJ_TYPE_TOKEN    = 10,
    OBJ_TYPE_POWER_REQUEST = 11,
} OBJECT_TYPE_ID;

/* ── Handle table entry ─────────────────────────────────────── */

typedef struct _HANDLE_ENTRY {
    OBJECT_TYPE_ID  type;
    ULONG           access;     /* granted ACCESS_MASK */
    PVOID           object;     /* pointer to type-specific struct */
    ULONG           refs;       /* owners sharing this numeric handle */
    ULONG           owner_pid;  /* sole owner, or 0 when shared */
    LONG            owner_head; /* internal per-process owner list */
} HANDLE_ENTRY;

/* ── Handle table ───────────────────────────────────────────── */

#define MAX_HANDLES     4096
#define HANDLE_TO_INDEX(h)  ((ULONG)(ULONG_PTR)(h) >> 2)
#define INDEX_TO_HANDLE(i)  ((HANDLE)(ULONG_PTR)((i) << 2))

typedef struct _HANDLE_TABLE {
    HANDLE_ENTRY entries[MAX_HANDLES];
    ULONG        count;     /* number of allocated handles */
} HANDLE_TABLE, *PHANDLE_TABLE;

/* ── File object ────────────────────────────────────────────── */

#define FILE_OBJ_CONSOLE_IN     0x01
#define FILE_OBJ_CONSOLE_OUT    0x02
#define FILE_OBJ_CONSOLE_ERR    0x04
#define FILE_OBJ_DISK_FILE      0x08
#define FILE_OBJ_PIPE_READ      0x10
#define FILE_OBJ_PIPE_WRITE     0x20
#define FILE_OBJ_PIPE_SIDE_A    0x40
#define FILE_OBJ_PIPE_SIDE_B    0x80
#define FILE_OBJ_DIRECTORY      0x100
#define FILE_OBJ_SERIAL         0x200

typedef struct _FILE_OBJECT {
    ULONG       flags;          /* FILE_OBJ_* */
    PVOID       osfs_file;      /* osfs2_file_t* for disk files */
    LONGLONG    position;       /* current byte offset */
    LONGLONG    size;           /* file size (cached) */
    WCHAR       name[260];      /* file name (MAX_PATH) */
} FILE_OBJECT, *PFILE_OBJECT;

typedef struct _HANDLE_OBJECT_SNAPSHOT {
    OBJECT_TYPE_ID type;
    ACCESS_MASK    access;
    ULONG          handle_count;
    ULONG          pointer_count;
    ULONG          file_flags;
    ULONG          name_length;
    WCHAR          name[260];
} HANDLE_OBJECT_SNAPSHOT;

/* ── API ────────────────────────────────────────────────────── */

void        handle_table_init(PHANDLE_TABLE table);

NTSTATUS    handle_alloc(PHANDLE_TABLE table,
                         OBJECT_TYPE_ID type,
                         ACCESS_MASK access,
                         PVOID object,
                         PHANDLE out_handle);

NTSTATUS    handle_alloc_for_process(PHANDLE_TABLE table,
                                     OBJECT_TYPE_ID type,
                                     ACCESS_MASK access,
                                     PVOID object,
                                     ULONG owner_pid,
                                     PHANDLE out_handle);

NTSTATUS    handle_lookup(PHANDLE_TABLE table,
                          HANDLE handle,
                          OBJECT_TYPE_ID expected_type,
                          PVOID *out_object);

NTSTATUS    handle_lookup_for_process(PHANDLE_TABLE table,
                                      HANDLE handle,
                                      ULONG owner_pid,
                                      OBJECT_TYPE_ID expected_type,
                                      PVOID *out_object);

/* Retain the same numeric handle for an inherited child owner. */
NTSTATUS    handle_retain(PHANDLE_TABLE table,
                          HANDLE handle);

NTSTATUS    handle_retain_for_process(PHANDLE_TABLE table,
                                      HANDLE handle,
                                      ULONG owner_pid);

NTSTATUS    handle_release_for_process(PHANDLE_TABLE table,
                                       HANDLE handle,
                                       ULONG owner_pid);

NTSTATUS    handle_close(PHANDLE_TABLE table,
                         HANDLE handle);

/* Atomically release a handle owner and return the affected entry. */
NTSTATUS    handle_close_entry(PHANDLE_TABLE table,
                               HANDLE handle,
                               HANDLE_ENTRY *closed_entry);

NTSTATUS    handle_close_entry_for_process(PHANDLE_TABLE table,
                                           HANDLE handle,
                                           ULONG owner_pid,
                                           HANDLE_ENTRY *closed_entry);

/* Lookup without type check (returns entry directly) */
HANDLE_ENTRY *handle_get_entry(PHANDLE_TABLE table, HANDLE handle);

/* Snapshot handle ownership while holding the table lock. Intended for
 * bounded diagnostics around lifetime bugs. */
BOOL        handle_query_state(PHANDLE_TABLE table,
                               HANDLE handle,
                               ULONG owner_pid,
                               OBJECT_TYPE_ID *out_type,
                               ULONG *out_refs,
                               ULONG *out_owner_refs,
                               ULONG *out_sole_owner);

/* Copy queryable object metadata while the handle table lock guarantees the
 * object's lifetime. The returned snapshot contains no live object pointer. */
NTSTATUS    handle_snapshot_for_process(PHANDLE_TABLE table,
                                        HANDLE handle,
                                        ULONG owner_pid,
                                        HANDLE_OBJECT_SNAPSHOT *snapshot);

BOOL        handle_object_referenced(PHANDLE_TABLE table,
                                     OBJECT_TYPE_ID type,
                                     PVOID object);

/* Remove one handle from an exiting process's ownership set. */
HANDLE      handle_take_owned(PHANDLE_TABLE table,
                              ULONG owner_pid);

/* Duplicate a handle within the same or across tables */
NTSTATUS    handle_duplicate(PHANDLE_TABLE src_table,
                             HANDLE src_handle,
                             PHANDLE_TABLE dst_table,
                             PHANDLE dst_handle,
                             ACCESS_MASK desired_access,
                             BOOL inherit_handle,
                             ULONG options);

NTSTATUS    handle_duplicate_for_process(PHANDLE_TABLE src_table,
                                         HANDLE src_handle,
                                         ULONG source_pid,
                                         PHANDLE_TABLE dst_table,
                                         PHANDLE dst_handle,
                                         ULONG target_pid,
                                         ACCESS_MASK desired_access,
                                         BOOL inherit_handle,
                                         ULONG options);

/* Options for handle_duplicate */
#define DUPLICATE_CLOSE_SOURCE  0x00000001
#define DUPLICATE_SAME_ACCESS   0x00000002

#endif /* HANDLE_H */
