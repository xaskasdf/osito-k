/* Exercise the real bounded-probe policy and parser without executing DR ops. */
#include "../kernel/hwbp.c"

extern int printf(const char *, ...);
static unsigned checks, failures;
#define CHECK(test) do { checks++; if (!(test)) { \
    printf("FAIL line %d: %s\n", __LINE__, #test); failures++; \
} } while (0)

static uint64_t mapped_stack, mapped_caller;
static uint64_t stack_permissions, caller_permissions;
static bool stack_present, caller_present;

int paging_query_mapping_in_cr3(uint64_t cr3, uint64_t virt,
                                uint64_t *flags, uint64_t *page_size)
{
    if (cr3 != 0x12345000) return -1;
    if (stack_present && virt >= mapped_stack && virt < mapped_stack + 32)
        *flags = stack_permissions;
    else if (caller_present && virt == mapped_caller)
        *flags = caller_permissions;
    else
        return -1;
    if (page_size) *page_size = 4096;
    return 0;
}

int main(void)
{
    uint8_t code[16], signature[16];
    for (unsigned i = 0; i < 16; i++) code[i] = signature[i] = (uint8_t)i;
    hwbp_trace_t trace = {0};
    uint64_t addr = 0x140002000ULL, cr3 = 0x12345000;
    CHECK(hwbp_trace_prepare(&trace, addr, cr3, HWBP_TRACE_EDI32, 2, signature));
    /* A wrong address space must not dereference even a poisoned code pointer. */
    const volatile uint8_t *poison = (const volatile uint8_t *)1;
    CHECK(hwbp_trace_accept(&trace, addr, cr3 + 4096, poison) == HWBP_TRACE_IGNORE);
    CHECK(hwbp_trace_accept(&trace, addr + 1, cr3, poison) == HWBP_TRACE_IGNORE);
    CHECK(!trace.hits && trace.active);
    CHECK(hwbp_trace_accept(&trace, addr, cr3 | 7, code) == HWBP_TRACE_HIT);
    CHECK(trace.hits == 1 && trace.active);
    CHECK(hwbp_trace_accept(&trace, addr, cr3, code) == HWBP_TRACE_LIMIT);
    CHECK(trace.hits == 2 && !trace.active);
    CHECK(hwbp_trace_accept(&trace, addr, cr3, poison) == HWBP_TRACE_IGNORE);
    CHECK(trace.hits == 2 && trace.last_result == HWBP_TRACE_LIMIT);

    CHECK(hwbp_trace_prepare(&trace, addr, cr3, HWBP_TRACE_EAX32, 32, signature));
    code[15] ^= 1;
    CHECK(hwbp_trace_accept(&trace, addr, cr3, code) == HWBP_TRACE_MISMATCH);
    CHECK(!trace.hits && !trace.active);
    CHECK(hwbp_trace_accept(&trace, addr, cr3, poison) == HWBP_TRACE_IGNORE);
    code[15] ^= 1;
    CHECK(hwbp_trace_prepare(&trace, addr, cr3, HWBP_TRACE_EAX32, 1, signature));
    CHECK(hwbp_trace_accept(&trace, addr, cr3, NULL) == HWBP_TRACE_MISMATCH);
    CHECK(!trace.active && !trace.hits);

    CHECK(!hwbp_trace_prepare(&trace, 0, cr3, HWBP_TRACE_EAX32, 1, signature));
    CHECK(!hwbp_trace_prepare(&trace, 0xFFFFFFFF80000000ULL, cr3,
                              HWBP_TRACE_EAX32, 1, signature));
    CHECK(!hwbp_trace_prepare(&trace, 0x800000000000ULL, cr3,
                              HWBP_TRACE_EAX32, 1, signature));
    CHECK(hwbp_trace_prepare(&trace, addr + 4080, cr3,
                             HWBP_TRACE_EAX32, 1, signature));
    CHECK(!hwbp_trace_prepare(&trace, addr + 4081, cr3,
                              HWBP_TRACE_EAX32, 1, signature));
    CHECK(!hwbp_trace_prepare(&trace, addr, 0, HWBP_TRACE_EAX32, 1, signature));
    CHECK(!hwbp_trace_prepare(&trace, addr, cr3 + 1, HWBP_TRACE_EAX32, 1, signature));
    CHECK(!hwbp_trace_prepare(&trace, addr, cr3, HWBP_TRACE_EAX32, 0, signature));
    CHECK(!hwbp_trace_prepare(&trace, addr, cr3, HWBP_TRACE_EAX32, 33, signature));
    CHECK(!hwbp_trace_prepare(&trace, addr, cr3, (hwbp_trace_reg_t)2, 1, signature));

    uint64_t value;
    CHECK(hwbp_parse_number("18446744073709551615", 10, UINT64_MAX, &value));
    CHECK(value == UINT64_MAX);
    CHECK(!hwbp_parse_number("18446744073709551616", 10, UINT64_MAX, &value));
    CHECK(hwbp_parse_number("0x138e6bf95", 16, UINT64_MAX, &value));
    CHECK(value == 0x138e6bf95ULL);
    const char *bad[] = {"", "0x", "-1", "+1", " 1", "1 ", "1z", "1;reboot"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!hwbp_parse_number(bad[i], 16, UINT64_MAX, &value));
    CHECK(!hwbp_parse_number("4", 10, 3, &value));
    CHECK(!hwbp_parse_number("33", 10, 32, &value));
    CHECK(!hwbp_parse_number("ff", 10, 32, &value));
    CHECK(hwbp_parse_signature("000102030405060708090a0b0c0d0e0f", signature));
    for (unsigned i = 0; i < 16; i++) CHECK(signature[i] == code[i]);
    const char *bad_signatures[] = {"", "0", "00",
        "000102030405060708090a0b0c0d0e0",
        "000102030405060708090a0b0c0d0e0f0",
        "000102030405060708090a0b0c0d0e0z"};
    for (unsigned i = 0; i < sizeof(bad_signatures) / sizeof(bad_signatures[0]); i++)
        CHECK(!hwbp_parse_signature(bad_signatures[i], signature));

    unsigned bits, offset;
    CHECK(hwbp_parse_caller("caller64:424", &bits, &offset));
    CHECK(bits == 64 && offset == 424);
    CHECK(hwbp_parse_caller("caller32:4", &bits, &offset));
    CHECK(bits == 32 && offset == 4);
    CHECK(hwbp_parse_caller("caller64:512", &bits, &offset));
    CHECK(hwbp_parse_caller("caller32:0", &bits, &offset));
    const char *bad_callers[] = {"", "caller64:", "caller64:1", "caller32:3",
        "caller64:513", "caller16:0", "caller64:-8", "caller64:+8",
        "caller64:18446744073709551616", "caller64:8 ", "caller64:8extra"};
    for (unsigned i = 0; i < sizeof(bad_callers) / sizeof(bad_callers[0]); i++)
        CHECK(!hwbp_parse_caller(bad_callers[i], &bits, &offset));
    CHECK(!hwbp_parse_caller(NULL, &bits, &offset));
    CHECK(hwbp_trace_caller_config(0, 0));
    CHECK(!hwbp_trace_caller_config(0, 8));
    CHECK(!hwbp_trace_caller_config(16, 0));

    uint8_t stack[32] __attribute__((aligned(16))) = {0};
    mapped_stack = (uint64_t)(uintptr_t)stack;
    for (unsigned width = 4; width <= 8; width += 4) {
        trace = (hwbp_trace_t){.cr3 = cr3, .caller_bits = width * 8,
                               .caller_offset = width};
        mapped_caller = width == 4 ? 0x401234 : 0x140003100ULL;
        for (unsigned i = 0; i < width; i++)
            stack[width + i] = (uint8_t)(mapped_caller >> (i * 8));
        stack_present = caller_present = true;
        stack_permissions = 5 | (1ULL << 63);
        caller_permissions = 5;
        uint64_t caller = 0;
        CHECK(hwbp_trace_read_caller(&trace, mapped_stack, &caller));
        CHECK(caller == mapped_caller);
        stack_present = false;
        CHECK(!hwbp_trace_read_caller(&trace, mapped_stack, &caller));
        CHECK(!hwbp_trace_read_caller(&trace, 4096, &caller));
        stack_present = true;
        /* Win32 guest pages are supervisor mappings while it runs at CPL0. */
        stack_permissions = 3;
        CHECK(hwbp_trace_read_caller(&trace, mapped_stack, &caller));
        CHECK(caller == mapped_caller);
        stack_permissions = 0;
        CHECK(!hwbp_trace_read_caller(&trace, mapped_stack, &caller));
        stack_permissions = 5;
        caller_present = false;
        CHECK(!hwbp_trace_read_caller(&trace, mapped_stack, &caller));
        caller_present = true;
        caller_permissions = 5 | (1ULL << 63);
        CHECK(!hwbp_trace_read_caller(&trace, mapped_stack, &caller));
        caller_permissions = 3;
        CHECK(hwbp_trace_read_caller(&trace, mapped_stack, &caller));
        CHECK(caller == mapped_caller);
        caller_permissions = 0;
        CHECK(!hwbp_trace_read_caller(&trace, mapped_stack, &caller));
        caller_permissions = 5;
        stack[width] = 0;
        for (unsigned i = 1; i < width; i++) stack[width + i] = 0;
        CHECK(!hwbp_trace_read_caller(&trace, mapped_stack, &caller));
        uint64_t slot;
        CHECK(!hwbp_trace_caller_slot(&trace, 0, &slot));
        CHECK(!hwbp_trace_caller_slot(&trace, UINT64_MAX, &slot));
        CHECK(!hwbp_trace_caller_slot(&trace, 0x800000000000ULL - width, &slot));
        CHECK(!hwbp_trace_caller_slot(&trace, 4097, &slot));
        CHECK(!hwbp_trace_caller_slot(&trace, mapped_stack, NULL));
    }
    trace.caller_bits = 0;
    CHECK(!hwbp_trace_caller_slot(&trace, mapped_stack, &value));
    printf("HWBP trace policy/parser: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
