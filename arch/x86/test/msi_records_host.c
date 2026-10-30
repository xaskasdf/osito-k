#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#define OSITOK_X86_TYPES_H
#define __KERNEL_X86__ 1
#include MSI_SHIM_HOST_SOURCE
static unsigned allocations, failures, checks;
static int fail_after = -1;
static _Thread_local DWORD owner = 1;
static _Thread_local int compat;
static const void *deny_read, *deny_write;
static DWORD live;
static void record_check(int condition, unsigned line)
{
    __atomic_add_fetch(&checks, 1, __ATOMIC_RELAXED);
    if (!condition) { __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED); printf("MSIRECORD FAIL line=%u\n", line); }
}
void *kmalloc(uint64_t size)
{
    if (fail_after >= 0 && fail_after-- == 0) return NULL;
    void *result = malloc(size);
    if (result) __atomic_add_fetch(&allocations, 1, __ATOMIC_RELAXED);
    return result;
}
void kfree(void *pointer)
{ if (pointer) { __atomic_sub_fetch(&allocations, 1, __ATOMIC_RELAXED); free(pointer); } }
DWORD win32_current_process_id(void) { return owner; }
/* The record-only host fixture never creates discovery cursors. */
void msi_discovery_release_process(DWORD process) { (void)process; }
int *proc_win32_compat32_mode_slot(void) { return &compat; }
int win32_user_range_readable(const void *p, SIZE_T size, BOOL mode)
{ (void)size; (void)mode; return (uintptr_t)p >= 4096 && p != deny_read; }
int win32_user_range_writable(void *p, SIZE_T size, BOOL mode)
{ (void)size; (void)mode; return (uintptr_t)p >= 4096 && p != deny_write; }
#include "msi_records_checks.h"
static void *record_worker(void *argument)
{
    uintptr_t which = (uintptr_t)argument;
    for (unsigned i = 0; i < 10000; i++) {
        DWORD handle = __atomic_load_n(&live, __ATOMIC_ACQUIRE);
        if (which == 0) {
            DWORD next = MsiCreateRecord(2); RECORD_CHECK(next != 0);
            DWORD previous = __atomic_exchange_n(&live, next, __ATOMIC_ACQ_REL);
            if (previous) RECORD_CHECK(MsiCloseHandle(previous) == 0);
        } else if (which == 1) {
            DWORD result = MsiRecordSetStringW(handle, 1, i & 1 ? RW("Alpha") : RW("Beta"));
            RECORD_CHECK(result == 0 || result == 6);
        } else {
            WCHAR buf[32]; DWORD length = 32;
            DWORD result = MsiRecordGetStringW(handle, 1, buf, &length);
            RECORD_CHECK(result == 6 || (result == 0 && (record_equal(buf, RW("")) ||
                record_equal(buf, RW("Alpha")) || record_equal(buf, RW("Beta")))));
        }
    }
    return NULL;
}
int main(void)
{
    record_contract_checks(); RECORD_CHECK(!allocations);
    DWORD a = MsiCreateRecord(3); RECORD_CHECK(a != 0);
    owner = 2; RECORD_CHECK(MsiCloseHandle(a) == 6 && MsiRecordGetFieldCount(a) == 0xffffffffU);
    DWORD b = MsiCreateRecord(1); RECORD_CHECK(b != 0);
    msi_release_process(1); RECORD_CHECK(MsiRecordGetFieldCount(b) == 1);
    owner = 1; RECORD_CHECK(MsiRecordGetFieldCount(a) == 0xffffffffU);
    msi_release_process(2); RECORD_CHECK(!allocations);
    a = MsiCreateRecord(3); RECORD_CHECK(a != 0);
    RECORD_CHECK(MsiRecordSetStringW(a, 1, RW("old")) == 0);
    unsigned baseline = allocations;
    fail_after = 0; RECORD_CHECK(MsiCreateRecord(1) == 0); fail_after = -1;
    fail_after = 0; RECORD_CHECK(MsiRecordSetStringW(a, 1, RW("new")) == 8); fail_after = -1;
    WCHAR buf[64]; DWORD capacity = 64;
    RECORD_CHECK(MsiRecordGetStringW(a, 1, buf, &capacity) == 0 && record_equal(buf, RW("old")));
    RECORD_CHECK(allocations == baseline);
    RECORD_CHECK(MsiRecordSetStringW(a, 0, RW("x{[1]} [3] [[1]]")) == 0);
    baseline = allocations;
    for (int i = 0; i < 32; i++) {
        fail_after = i; capacity = 64; buf[0] = 'Z';
        DWORD result = MsiFormatRecordW(0, a, buf, &capacity); fail_after = -1;
        RECORD_CHECK(result == 0 || result == 8);
        if (result) RECORD_CHECK(capacity == 64 && buf[0] == 'Z');
        RECORD_CHECK(allocations == baseline);
    }
    RECORD_CHECK(MsiRecordSetStringW(a, 0, RW("{{[1]}}")) == 0);
    capacity = 64; buf[0] = 'Z';
    RECORD_CHECK(MsiFormatRecordW(0, a, buf, &capacity) == 120 && capacity == 64 && buf[0] == 'Z');
    RECORD_CHECK(MsiRecordSetStringW(a, 1, (void *)1) == 87);
    RECORD_CHECK(MsiRecordGetStringW(a, 1, buf, NULL) == 87);
    deny_write = buf; capacity = 64;
    RECORD_CHECK(MsiRecordGetStringW(a, 1, buf, &capacity) == 87 && capacity == 64); deny_write = NULL;
    deny_read = &capacity;
    RECORD_CHECK(MsiRecordGetStringW(a, 1, buf, &capacity) == 87); deny_read = NULL;
    RECORD_CHECK(MsiCloseHandle(a) == 0 && !allocations);
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++) RECORD_CHECK(!pthread_create(&threads[i], NULL, record_worker, (void *)i));
    for (unsigned i = 0; i < 4; i++) RECORD_CHECK(!pthread_join(threads[i], NULL));
    RECORD_CHECK(MsiCloseHandle(live) == 0 && !allocations);
    for (unsigned i = 0; i < MSI_RECORD_HANDLES; i++) RECORD_CHECK(MsiCreateRecord(0) != 0);
    RECORD_CHECK(MsiCreateRecord(0) == 0);
    msi_release_process(1); RECORD_CHECK(!allocations);
    printf("MSIRECORD HOST checks=%u failures=%u live_allocations=%u\n", checks, failures, allocations);
    return failures ? 1 : 0;
}
