/*
 * OsitoK Windows Compatibility Layer — Handle Table Implementation
 */

#include "handle.h"

/* ── Helpers ────────────────────────────────────────────────── */

static inline void *nt_memset(void *s, int c, SIZE_T n)
{
    BYTE *p = (BYTE *)s;
    while (n--) *p++ = (BYTE)c;
    return s;
}

/* ── Init ───────────────────────────────────────────────────── */

void handle_table_init(PHANDLE_TABLE table)
{
    nt_memset(table, 0, sizeof(HANDLE_TABLE));
    table->count = 0;
}

/* ── Allocate ───────────────────────────────────────────────── */

NTSTATUS handle_alloc(PHANDLE_TABLE table,
                      OBJECT_TYPE_ID type,
                      ACCESS_MASK access,
                      PVOID object,
                      PHANDLE out_handle)
{
    if (!table || !out_handle || type == OBJ_TYPE_NONE)
        return STATUS_INVALID_PARAMETER;

    /* Find first free slot (slot 0 is reserved — handles start at 4) */
    for (ULONG i = 1; i < MAX_HANDLES; i++) {
        if (table->entries[i].type == OBJ_TYPE_NONE) {
            table->entries[i].type   = type;
            table->entries[i].access = access;
            table->entries[i].object = object;
            table->count++;
            *out_handle = INDEX_TO_HANDLE(i);
            return STATUS_SUCCESS;
        }
    }

    return STATUS_INSUFFICIENT_RESOURCES;
}

/* ── Lookup (with type check) ───────────────────────────────── */

NTSTATUS handle_lookup(PHANDLE_TABLE table,
                       HANDLE handle,
                       OBJECT_TYPE_ID expected_type,
                       PVOID *out_object)
{
    if (!table || !out_object)
        return STATUS_INVALID_PARAMETER;

    ULONG idx = HANDLE_TO_INDEX(handle);
    if (idx == 0 || idx >= MAX_HANDLES)
        return STATUS_INVALID_HANDLE;

    HANDLE_ENTRY *entry = &table->entries[idx];
    if (entry->type == OBJ_TYPE_NONE)
        return STATUS_INVALID_HANDLE;

    if (expected_type != OBJ_TYPE_NONE && entry->type != expected_type)
        return STATUS_OBJECT_TYPE_MISMATCH;

    *out_object = entry->object;
    return STATUS_SUCCESS;
}

/* ── Get entry (no type check) ──────────────────────────────── */

HANDLE_ENTRY *handle_get_entry(PHANDLE_TABLE table, HANDLE handle)
{
    if (!table) return NULL;

    ULONG idx = HANDLE_TO_INDEX(handle);
    if (idx == 0 || idx >= MAX_HANDLES)
        return NULL;

    HANDLE_ENTRY *entry = &table->entries[idx];
    if (entry->type == OBJ_TYPE_NONE)
        return NULL;

    return entry;
}

/* ── Close ──────────────────────────────────────────────────── */

NTSTATUS handle_close(PHANDLE_TABLE table, HANDLE handle)
{
    if (!table)
        return STATUS_INVALID_PARAMETER;

    ULONG idx = HANDLE_TO_INDEX(handle);
    if (idx == 0 || idx >= MAX_HANDLES)
        return STATUS_INVALID_HANDLE;

    HANDLE_ENTRY *entry = &table->entries[idx];
    if (entry->type == OBJ_TYPE_NONE)
        return STATUS_INVALID_HANDLE;

    entry->type   = OBJ_TYPE_NONE;
    entry->access = 0;
    entry->object = NULL;
    table->count--;

    return STATUS_SUCCESS;
}

/* ── Duplicate ──────────────────────────────────────────────── */

NTSTATUS handle_duplicate(PHANDLE_TABLE src_table,
                          HANDLE src_handle,
                          PHANDLE_TABLE dst_table,
                          PHANDLE dst_handle,
                          ACCESS_MASK desired_access,
                          BOOL inherit_handle,
                          ULONG options)
{
    (void)inherit_handle;

    if (!src_table || !dst_table || !dst_handle)
        return STATUS_INVALID_PARAMETER;

    HANDLE_ENTRY *src_entry = handle_get_entry(src_table, src_handle);
    if (!src_entry)
        return STATUS_INVALID_HANDLE;

    ACCESS_MASK access = (options & DUPLICATE_SAME_ACCESS)
                         ? src_entry->access : desired_access;

    NTSTATUS status = handle_alloc(dst_table, src_entry->type,
                                   access, src_entry->object, dst_handle);
    if (!NT_SUCCESS(status))
        return status;

    if (options & DUPLICATE_CLOSE_SOURCE)
        handle_close(src_table, src_handle);

    return STATUS_SUCCESS;
}
