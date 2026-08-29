/*
 * OsitoK Windows Compatibility Layer — Handle Table Implementation
 */

#include "handle.h"
#include "ntsyscall.h"
#include "../kernel/smp.h"

extern DWORD win32_current_process_id(void);

/* ── Helpers ────────────────────────────────────────────────── */

static spinlock_t handle_table_lock = SPINLOCK_INIT;

#define MAX_HANDLE_OWNER_RECORDS (MAX_HANDLES * 2)
#define HANDLE_OWNER_NONE (-1)

typedef struct {
    ULONG pid;
    ULONG refs;
    LONG next;
    BOOL used;
} HANDLE_OWNER_RECORD;

static HANDLE_OWNER_RECORD handle_owners[MAX_HANDLE_OWNER_RECORDS];
static ULONG handle_owner_cursor;

static inline uint64_t handle_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&handle_table_lock);
    return flags;
}

static inline void handle_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&handle_table_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static inline void *nt_memset(void *s, int c, SIZE_T n)
{
    BYTE *p = (BYTE *)s;
    while (n--) *p++ = (BYTE)c;
    return s;
}

/* ── Init ───────────────────────────────────────────────────── */

void handle_table_init(PHANDLE_TABLE table)
{
    uint64_t flags = handle_lock_irqsave();
    for (ULONG i = 1; i < MAX_HANDLES; i++) {
        HANDLE_ENTRY *entry = &table->entries[i];
        if (entry->type == OBJ_TYPE_NONE)
            continue;
        LONG owner_index = entry->owner_head;
        while (owner_index != HANDLE_OWNER_NONE) {
            HANDLE_OWNER_RECORD *owner = &handle_owners[owner_index];
            LONG next = owner->next;
            nt_memset(owner, 0, sizeof(*owner));
            owner_index = next;
        }
    }
    nt_memset(table, 0, sizeof(HANDLE_TABLE));
    table->count = 0;
    handle_unlock_irqrestore(flags);
}

/* ── Allocate ───────────────────────────────────────────────── */

static HANDLE_OWNER_RECORD *owner_find_locked(HANDLE_ENTRY *entry, ULONG pid)
{
    for (LONG i = entry->owner_head; i != HANDLE_OWNER_NONE;
         i = handle_owners[i].next) {
        HANDLE_OWNER_RECORD *owner = &handle_owners[i];
        if (owner->used && owner->pid == pid)
            return owner;
    }
    return NULL;
}

static BOOL owner_add_locked(HANDLE_ENTRY *entry, ULONG pid)
{
    HANDLE_OWNER_RECORD *owner = owner_find_locked(entry, pid);
    if (owner) {
        if (owner->refs == 0xFFFFFFFFU)
            return FALSE;
        owner->refs++;
        return TRUE;
    }

    for (ULONG n = 0; n < MAX_HANDLE_OWNER_RECORDS; n++) {
        ULONG i = (handle_owner_cursor + n) % MAX_HANDLE_OWNER_RECORDS;
        owner = &handle_owners[i];
        if (owner->used)
            continue;
        owner->used = TRUE;
        owner->pid = pid;
        owner->refs = 1;
        owner->next = entry->owner_head;
        entry->owner_head = (LONG)i;
        handle_owner_cursor = (i + 1) % MAX_HANDLE_OWNER_RECORDS;
        return TRUE;
    }
    return FALSE;
}

static BOOL owner_remove_locked(HANDLE_ENTRY *entry, ULONG pid)
{
    LONG previous = HANDLE_OWNER_NONE;
    for (LONG i = entry->owner_head; i != HANDLE_OWNER_NONE;
         i = handle_owners[i].next) {
        HANDLE_OWNER_RECORD *owner = &handle_owners[i];
        if (!owner->used || owner->pid != pid) {
            previous = i;
            continue;
        }
        if (--owner->refs)
            return TRUE;
        if (previous == HANDLE_OWNER_NONE)
            entry->owner_head = owner->next;
        else
            handle_owners[previous].next = owner->next;
        nt_memset(owner, 0, sizeof(*owner));
        handle_owner_cursor = (ULONG)i;
        return TRUE;
    }
    return FALSE;
}

static ULONG owner_sole_pid_locked(HANDLE_ENTRY *entry)
{
    ULONG sole_pid = 0;
    for (LONG i = entry->owner_head; i != HANDLE_OWNER_NONE;
         i = handle_owners[i].next) {
        HANDLE_OWNER_RECORD *owner = &handle_owners[i];
        if (!owner->used || !owner->refs)
            continue;
        if (sole_pid && sole_pid != owner->pid)
            return 0;
        sole_pid = owner->pid;
    }
    return sole_pid;
}

static NTSTATUS handle_alloc_locked(PHANDLE_TABLE table,
                                    OBJECT_TYPE_ID type,
                                    ACCESS_MASK access,
                                    PVOID object,
                                    ULONG owner_pid,
                                    PHANDLE out_handle)
{
    /* Find first free slot (slot 0 is reserved — handles start at 4) */
    for (ULONG i = 1; i < MAX_HANDLES; i++) {
        if (table->entries[i].type == OBJ_TYPE_NONE) {
            HANDLE_ENTRY *entry = &table->entries[i];
            entry->owner_head = HANDLE_OWNER_NONE;
            if (!owner_add_locked(entry, owner_pid))
                return STATUS_INSUFFICIENT_RESOURCES;
            entry->type   = type;
            entry->access = access;
            entry->object = object;
            entry->refs   = 1;
            entry->owner_pid = owner_pid;
            table->count++;
            *out_handle = INDEX_TO_HANDLE(i);
            return STATUS_SUCCESS;
        }
    }

    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS handle_alloc(PHANDLE_TABLE table,
                      OBJECT_TYPE_ID type,
                      ACCESS_MASK access,
                      PVOID object,
                      PHANDLE out_handle)
{
    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    return handle_alloc_for_process(table, type, access, object, owner_pid,
                                    out_handle);
}

NTSTATUS handle_alloc_for_process(PHANDLE_TABLE table,
                                  OBJECT_TYPE_ID type,
                                  ACCESS_MASK access,
                                  PVOID object,
                                  ULONG owner_pid,
                                  PHANDLE out_handle)
{
    if (!table || !out_handle || type == OBJ_TYPE_NONE || !owner_pid)
        return STATUS_INVALID_PARAMETER;

    uint64_t flags = handle_lock_irqsave();
    NTSTATUS status = handle_alloc_locked(table, type, access, object,
                                          owner_pid, out_handle);
    handle_unlock_irqrestore(flags);
    return status;
}

NTSTATUS handle_retain(PHANDLE_TABLE table, HANDLE handle)
{
    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    return handle_retain_for_process(table, handle, owner_pid);
}

NTSTATUS handle_retain_for_process(PHANDLE_TABLE table, HANDLE handle,
                                   ULONG owner_pid)
{
    if (!table || !owner_pid)
        return STATUS_INVALID_PARAMETER;

    uint64_t flags = handle_lock_irqsave();
    ULONG idx = HANDLE_TO_INDEX(handle);
    NTSTATUS status = STATUS_INVALID_HANDLE;
    if (idx != 0 && idx < MAX_HANDLES) {
        HANDLE_ENTRY *entry = &table->entries[idx];
        if (entry->type != OBJ_TYPE_NONE && entry->refs != 0 &&
            entry->refs != 0xFFFFFFFFU &&
            owner_add_locked(entry, owner_pid)) {
            entry->refs++;
            entry->owner_pid = owner_sole_pid_locked(entry);
            status = STATUS_SUCCESS;
        }
    }
    handle_unlock_irqrestore(flags);
    return status;
}

NTSTATUS handle_release_for_process(PHANDLE_TABLE table, HANDLE handle,
                                    ULONG owner_pid)
{
    return handle_close_entry_for_process(table, handle, owner_pid, NULL);
}

/* ── Lookup (with type check) ───────────────────────────────── */

NTSTATUS handle_lookup(PHANDLE_TABLE table,
                       HANDLE handle,
                       OBJECT_TYPE_ID expected_type,
                       PVOID *out_object)
{
    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    return handle_lookup_for_process(table, handle, owner_pid, expected_type,
                                     out_object);
}

NTSTATUS handle_lookup_for_process(PHANDLE_TABLE table,
                                   HANDLE handle,
                                   ULONG owner_pid,
                                   OBJECT_TYPE_ID expected_type,
                                   PVOID *out_object)
{
    if (!table || !out_object || !owner_pid)
        return STATUS_INVALID_PARAMETER;

    uint64_t flags = handle_lock_irqsave();
    ULONG idx = HANDLE_TO_INDEX(handle);
    NTSTATUS status = STATUS_SUCCESS;
    if (idx == 0 || idx >= MAX_HANDLES) {
        status = STATUS_INVALID_HANDLE;
    } else {
        HANDLE_ENTRY *entry = &table->entries[idx];
        if (entry->type == OBJ_TYPE_NONE)
            status = STATUS_INVALID_HANDLE;
        else if (idx > 3 && !owner_find_locked(entry, owner_pid))
            status = STATUS_INVALID_HANDLE;
        else if (expected_type != OBJ_TYPE_NONE && entry->type != expected_type)
            status = STATUS_OBJECT_TYPE_MISMATCH;
        else
            *out_object = entry->object;
    }
    handle_unlock_irqrestore(flags);
    return status;
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

BOOL handle_query_state(PHANDLE_TABLE table, HANDLE handle, ULONG owner_pid,
                        OBJECT_TYPE_ID *out_type, ULONG *out_refs,
                        ULONG *out_owner_refs, ULONG *out_sole_owner)
{
    if (!table)
        return FALSE;

    uint64_t flags = handle_lock_irqsave();
    ULONG idx = HANDLE_TO_INDEX(handle);
    BOOL valid = idx != 0 && idx < MAX_HANDLES &&
                 table->entries[idx].type != OBJ_TYPE_NONE;
    if (valid) {
        HANDLE_ENTRY *entry = &table->entries[idx];
        HANDLE_OWNER_RECORD *owner = owner_pid
                                   ? owner_find_locked(entry, owner_pid)
                                   : NULL;
        if (out_type) *out_type = entry->type;
        if (out_refs) *out_refs = entry->refs;
        if (out_owner_refs) *out_owner_refs = owner ? owner->refs : 0;
        if (out_sole_owner)
            *out_sole_owner = owner_sole_pid_locked(entry);
    } else {
        if (out_type) *out_type = OBJ_TYPE_NONE;
        if (out_refs) *out_refs = 0;
        if (out_owner_refs) *out_owner_refs = 0;
        if (out_sole_owner) *out_sole_owner = 0;
    }
    handle_unlock_irqrestore(flags);
    return valid;
}

NTSTATUS handle_snapshot_for_process(PHANDLE_TABLE table, HANDLE handle,
                                     ULONG owner_pid,
                                     HANDLE_OBJECT_SNAPSHOT *snapshot)
{
    if (!table || !owner_pid || !snapshot)
        return STATUS_INVALID_PARAMETER;

    uint64_t flags = handle_lock_irqsave();
    ULONG idx = HANDLE_TO_INDEX(handle);
    NTSTATUS status = STATUS_INVALID_HANDLE;
    if (idx != 0 && idx < MAX_HANDLES) {
        HANDLE_ENTRY *entry = &table->entries[idx];
        if (entry->type != OBJ_TYPE_NONE && entry->refs &&
            (idx <= 3 || owner_find_locked(entry, owner_pid))) {
            nt_memset(snapshot, 0, sizeof(*snapshot));
            snapshot->type = entry->type;
            snapshot->access = entry->access;

            if (entry->object) {
                for (ULONG i = 1; i < MAX_HANDLES; i++) {
                    HANDLE_ENTRY *other = &table->entries[i];
                    if (other->type != entry->type ||
                        other->object != entry->object || !other->refs)
                        continue;
                    snapshot->handle_count++;
                    snapshot->pointer_count += other->refs;
                }
            } else {
                snapshot->handle_count = 1;
                snapshot->pointer_count = entry->refs;
            }

            if (entry->type == OBJ_TYPE_FILE && entry->object) {
                FILE_OBJECT *file = (FILE_OBJECT *)entry->object;
                snapshot->file_flags = file->flags;
                while (snapshot->name_length < 259 &&
                       file->name[snapshot->name_length]) {
                    snapshot->name[snapshot->name_length] =
                        file->name[snapshot->name_length];
                    snapshot->name_length++;
                }
                snapshot->name[snapshot->name_length] = 0;
            }
            status = STATUS_SUCCESS;
        }
    }
    handle_unlock_irqrestore(flags);
    return status;
}

BOOL handle_object_referenced(PHANDLE_TABLE table,
                              OBJECT_TYPE_ID type,
                              PVOID object)
{
    if (!table || !object)
        return FALSE;

    uint64_t flags = handle_lock_irqsave();
    BOOL referenced = FALSE;
    for (ULONG i = 1; i < MAX_HANDLES; i++) {
        HANDLE_ENTRY *entry = &table->entries[i];
        if (entry->type == type && entry->object == object) {
            referenced = TRUE;
            break;
        }
    }
    handle_unlock_irqrestore(flags);
    return referenced;
}

HANDLE handle_take_owned(PHANDLE_TABLE table, ULONG owner_pid)
{
    if (!table || !owner_pid)
        return NULL;

    uint64_t flags = handle_lock_irqsave();
    HANDLE handle = NULL;
    for (ULONG i = 1; i < MAX_HANDLES; i++) {
        HANDLE_ENTRY *entry = &table->entries[i];
        if (entry->type == OBJ_TYPE_NONE ||
            !owner_find_locked(entry, owner_pid))
            continue;
        handle = INDEX_TO_HANDLE(i);
        break;
    }
    handle_unlock_irqrestore(flags);
    return handle;
}

/* ── Close ──────────────────────────────────────────────────── */

static NTSTATUS handle_close_locked(PHANDLE_TABLE table, HANDLE handle,
                                    ULONG closing_pid,
                                    HANDLE_ENTRY *closed_entry)
{
    ULONG idx = HANDLE_TO_INDEX(handle);
    if (idx == 0 || idx >= MAX_HANDLES)
        return STATUS_INVALID_HANDLE;

    HANDLE_ENTRY *entry = &table->entries[idx];
    if (entry->type == OBJ_TYPE_NONE)
        return STATUS_INVALID_HANDLE;

    if (!owner_remove_locked(entry, closing_pid))
        return STATUS_INVALID_HANDLE;

    if (closed_entry)
        *closed_entry = *entry;

    if (entry->refs > 1) {
        entry->refs--;
        entry->owner_pid = owner_sole_pid_locked(entry);
        return STATUS_SUCCESS;
    }

    entry->type   = OBJ_TYPE_NONE;
    entry->access = 0;
    entry->object = NULL;
    entry->refs   = 0;
    entry->owner_pid = 0;
    table->count--;

    return STATUS_SUCCESS;
}

NTSTATUS handle_close_entry(PHANDLE_TABLE table, HANDLE handle,
                            HANDLE_ENTRY *closed_entry)
{
    if (!table)
        return STATUS_INVALID_PARAMETER;

    ULONG closing_pid = win32_current_process_id();
    if (!closing_pid) closing_pid = 1;
    return handle_close_entry_for_process(table, handle, closing_pid,
                                          closed_entry);
}

NTSTATUS handle_close_entry_for_process(PHANDLE_TABLE table, HANDLE handle,
                                        ULONG owner_pid,
                                        HANDLE_ENTRY *closed_entry)
{
    if (!table || !owner_pid)
        return STATUS_INVALID_PARAMETER;

    uint64_t flags = handle_lock_irqsave();
    NTSTATUS status = handle_close_locked(table, handle, owner_pid,
                                          closed_entry);
    handle_unlock_irqrestore(flags);
    return status;
}

NTSTATUS handle_close(PHANDLE_TABLE table, HANDLE handle)
{
    return handle_close_entry(table, handle, NULL);
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
    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    return handle_duplicate_for_process(src_table, src_handle, owner_pid,
                                        dst_table, dst_handle, owner_pid,
                                        desired_access, inherit_handle,
                                        options);
}

NTSTATUS handle_duplicate_for_process(PHANDLE_TABLE src_table,
                                      HANDLE src_handle,
                                      ULONG source_pid,
                                      PHANDLE_TABLE dst_table,
                                      PHANDLE dst_handle,
                                      ULONG target_pid,
                                      ACCESS_MASK desired_access,
                                      BOOL inherit_handle,
                                      ULONG options)
{
    (void)inherit_handle;

    if (!src_table || !dst_table || !dst_handle || !source_pid || !target_pid)
        return STATUS_INVALID_PARAMETER;

    uint64_t flags = handle_lock_irqsave();
    ULONG src_idx = HANDLE_TO_INDEX(src_handle);
    NTSTATUS status = STATUS_INVALID_HANDLE;
    if (src_idx == 0 || src_idx >= MAX_HANDLES)
        goto out;

    HANDLE_ENTRY *src_entry = &src_table->entries[src_idx];
    if (src_entry->type == OBJ_TYPE_NONE ||
        (src_idx > 3 && !owner_find_locked(src_entry, source_pid)))
        goto out;

    ACCESS_MASK access;
    if (options & DUPLICATE_SAME_ACCESS) {
        access = src_entry->access;
    } else {
        /* Windows can grant rights beyond the source handle when the
         * object's DACL permits them.  Chromium uses an empty DACL to make
         * read-only shared-memory handles non-upgradable. */
        BOOL can_escalate = src_entry->type == OBJ_TYPE_SECTION &&
            nt_section_allows_access_escalation(src_entry->object,
                                                desired_access);
        if (!(src_entry->access & GENERIC_ALL) &&
            (desired_access & ~src_entry->access) != 0 && !can_escalate) {
            status = STATUS_ACCESS_DENIED;
            goto out;
        }
        access = desired_access;
    }
    status = handle_alloc_locked(dst_table, src_entry->type, access,
                                 src_entry->object, target_pid, dst_handle);
    if (NT_SUCCESS(status) && (options & DUPLICATE_CLOSE_SOURCE))
        handle_close_locked(src_table, src_handle, source_pid, NULL);

out:
    handle_unlock_irqrestore(flags);
    return status;
}
