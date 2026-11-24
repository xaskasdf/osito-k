/* Shared native Windows / Osito-K contract checks. No installer is executed. */
#define RW(s) ((const WCHAR *)u##s)
#define RECORD_CHECK(c) record_check((c), __LINE__)
#include "msi_records_cases.h"
static int record_equal(const WCHAR *a, const WCHAR *b)
{ while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void record_contract_checks(void)
{
    DWORD counts[] = {0, 1, 3, 65535, 65536, 0xffffffffU};
    for (unsigned i = 0; i < sizeof(counts)/sizeof(*counts); i++) {
        DWORD h = MsiCreateRecord(counts[i]);
        RECORD_CHECK((h != 0) == (counts[i] <= 65535));
        RECORD_CHECK(MsiRecordGetFieldCount(h) == (h ? counts[i] : 0xffffffffU));
        if (h) RECORD_CHECK(MsiCloseHandle(h) == 0);
    }
    DWORD h = MsiCreateRecord(3); RECORD_CHECK(h != 0); if (!h) return;
    DWORD fields[] = {0, 1, 3, 4, 65535, 0xffffffffU};
    for (unsigned i = 0; i < sizeof(fields)/sizeof(*fields); i++) {
        DWORD field = fields[i], length = 16; WCHAR buf[16]; buf[0] = 'Z';
        RECORD_CHECK(MsiRecordSetStringW(h, field, RW("12")) == (field <= 3 ? 0U : 87U));
        RECORD_CHECK(MsiRecordGetInteger(h, field) == (field <= 3 ? 12 : (LONG)0x80000000U));
        RECORD_CHECK(MsiRecordGetStringW(h, field, buf, &length) == 0);
        RECORD_CHECK(length == (field <= 3 ? 2U : 0U));
        RECORD_CHECK(record_equal(buf, field <= 3 ? RW("12") : RW("")));
    }
    for (unsigned i = 0; i < sizeof(record_integers)/sizeof(*record_integers); i++) {
        RECORD_CHECK(MsiRecordSetStringW(h, 1, record_integers[i].text) == 0);
        RECORD_CHECK(MsiRecordGetInteger(h, 1) == record_integers[i].expected);
    }
    for (unsigned i = 0; i < sizeof(record_buffers)/sizeof(*record_buffers); i++) {
        const RECORD_BUFFER_CASE *test = &record_buffers[i];
        WCHAR buf[32]; for (unsigned j = 0; j < 32; j++) buf[j] = 'Z';
        RECORD_CHECK(MsiRecordSetStringW(h, 1, test->text) == 0);
        DWORD size = test->capacity;
        RECORD_CHECK(MsiRecordGetStringW(h, 1, test->null_buffer ? NULL : buf, &size) == test->result);
        RECORD_CHECK(size == test->length);
        for (unsigned j = 0; j < 32; j++) RECORD_CHECK(buf[j] == test->storage[j]);
    }
    for (unsigned i = 0; i < sizeof(record_formats)/sizeof(*record_formats); i++) {
        const RECORD_FORMAT_CASE *test = &record_formats[i];
        RECORD_CHECK(MsiRecordSetStringW(h, 1, test->value) == 0);
        RECORD_CHECK(MsiRecordSetStringW(h, 2, NULL) == 0);
        RECORD_CHECK(MsiRecordSetInteger(h, 3, 42) == 0);
        RECORD_CHECK(MsiRecordSetStringW(h, 0, test->format) == 0);
        WCHAR buf[1024]; buf[0] = 'Z'; DWORD size = 1024;
        RECORD_CHECK(MsiFormatRecordW(0, h, buf, &size) == test->result);
        RECORD_CHECK(size == test->length);
        RECORD_CHECK(record_equal(buf, test->text));
    }
    LONG numbers[] = {(LONG)0x80000000U, -1, 0, 42, 2147483647};
    const WCHAR *texts[] = {RW(""), RW("-1"), RW("0"), RW("42"), RW("2147483647")};
    for (unsigned i = 0; i < 5; i++) {
        DWORD length = 32; WCHAR buf[32];
        RECORD_CHECK(MsiRecordSetInteger(h, 1, numbers[i]) == 0);
        RECORD_CHECK(MsiRecordGetInteger(h, 1) == numbers[i]);
        RECORD_CHECK(MsiRecordGetStringW(h, 1, buf, &length) == 0 && record_equal(buf, texts[i]));
        RECORD_CHECK(MsiRecordIsNull(h, 1) == (i == 0));
        RECORD_CHECK(MsiRecordDataSize(h, 1) == (i ? 4U : 0U));
    }
    RECORD_CHECK(MsiRecordSetStringW(h, 1, RW("Alpha")) == 0);
    RECORD_CHECK(MsiRecordDataSize(h, 1) == 5);
    RECORD_CHECK(MsiRecordIsNull(h, 4) == 1 && MsiRecordDataSize(h, 4) == 0);
    RECORD_CHECK(MsiCloseHandle(h) == 0);
    RECORD_CHECK(MsiCloseHandle(h) == 6);
    RECORD_CHECK(MsiRecordGetFieldCount(h) == 0xffffffffU);
    RECORD_CHECK(MsiRecordGetInteger(h, 1) == (LONG)0x80000000U);
    RECORD_CHECK(MsiRecordIsNull(h, 1) == 0 && MsiRecordDataSize(h, 1) == 0);
    WCHAR buf[32]; buf[0] = 'Z'; DWORD length = 32;
    RECORD_CHECK(MsiRecordGetStringW(h, 1, buf, &length) == 6 && length == 32 && buf[0] == 'Z');
    DWORD fresh = MsiCreateRecord(1); RECORD_CHECK(fresh != 0 && fresh != h);
    RECORD_CHECK(MsiRecordSetStringW(h, 1, RW("stale")) == 6);
    RECORD_CHECK(MsiRecordIsNull(fresh, 1) == 1); RECORD_CHECK(MsiCloseHandle(fresh) == 0);
}
