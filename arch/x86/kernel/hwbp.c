/*
 * arch/x86/kernel/hwbp.c — hardware breakpoint/watchpoint manager
 *
 * Handlers read DR6 at entry to identify which DR register triggered,
 * then print the watchpoint info + triggering RIP. Integrates with the
 * existing #DB handler in idt.c.
 */

#include "../include/hwbp.h"
#include "hwbp_trace.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern void serial_puthex(uint64_t v, int d);

hwbp_t hwbps[4];
/* Preserve hwbp_t's public layout, also used by the shell and WASM stubs. */
static hwbp_trace_t execution_traces[4];

/* DR register access */
static void dr_set_addr(int slot, uint64_t v)
{
    switch (slot) {
    case 0: __asm__ volatile("mov %0, %%dr0" :: "r"(v)); break;
    case 1: __asm__ volatile("mov %0, %%dr1" :: "r"(v)); break;
    case 2: __asm__ volatile("mov %0, %%dr2" :: "r"(v)); break;
    case 3: __asm__ volatile("mov %0, %%dr3" :: "r"(v)); break;
    }
}

static uint64_t dr_get_addr(int slot)
{
    uint64_t v = 0;
    switch (slot) {
    case 0: __asm__ volatile("mov %%dr0, %0" : "=r"(v)); break;
    case 1: __asm__ volatile("mov %%dr1, %0" : "=r"(v)); break;
    case 2: __asm__ volatile("mov %%dr2, %0" : "=r"(v)); break;
    case 3: __asm__ volatile("mov %%dr3, %0" : "=r"(v)); break;
    }
    return v;
}

static uint64_t dr7_read(void)
{
    uint64_t v;
    __asm__ volatile("mov %%dr7, %0" : "=r"(v));
    return v;
}

static void dr7_write(uint64_t v)
{
    __asm__ volatile("mov %0, %%dr7" :: "r"(v));
}

static void dr6_clear(void)
{
    __asm__ volatile("mov %0, %%dr6" :: "r"(0ULL));
}

int hwbp_set(int slot, uint64_t addr, hwbp_cond_t cond, hwbp_len_t len,
             const char *name)
{
    if (slot < 0 || slot > 3) return -1;

    execution_traces[slot] = (hwbp_trace_t){0};
    hwbps[slot].addr = addr;
    hwbps[slot].cond = cond;
    hwbps[slot].len = len;
    hwbps[slot].active = true;
    hwbps[slot].hit_count = 0;
    int i = 0;
    if (name) {
        while (i < 31 && name[i]) { hwbps[slot].name[i] = name[i]; i++; }
    }
    hwbps[slot].name[i] = 0;

    dr_set_addr(slot, addr);

    uint64_t dr7 = dr7_read();
    /* Clear local enable, cond, len for this slot */
    uint32_t ctrl_shift = 16 + slot * 4;
    dr7 &= ~(3ULL << (slot * 2));            /* local+global enable bits */
    dr7 &= ~(0xFULL << ctrl_shift);          /* cond+len bits */
    /* Set local enable (bit slot*2), cond, len */
    dr7 |= (1ULL << (slot * 2));             /* local enable */
    dr7 |= ((uint64_t)cond << ctrl_shift);
    dr7 |= ((uint64_t)len  << (ctrl_shift + 2));
    dr7_write(dr7);

    return 0;
}

int hwbp_clear(int slot)
{
    if (slot < 0 || slot > 3) return -1;
    uint64_t dr7 = dr7_read();
    dr7 &= ~(3ULL << (slot * 2));
    dr7_write(dr7);
    hwbps[slot].active = false;
    execution_traces[slot].active = false;
    return 0;
}

void hwbp_clear_all(void)
{
    for (int i = 0; i < 4; i++) hwbp_clear(i);
}

/* Only called after the instruction fingerprint and current CR3 match.
 * Inspect one declared return slot, never a stack walk. Both the slot and
 * destination must be present; the destination must be executable. Win32
 * currently runs at CPL0 with supervisor mappings in the low canonical half. */
static bool hwbp_trace_read_caller(const hwbp_trace_t *trace, uint64_t rsp,
                                  uint64_t *caller)
{
    uint64_t slot, flags;
    if (!hwbp_trace_caller_slot(trace, rsp, &slot) || !caller ||
        paging_query_mapping_in_cr3(trace->cr3, slot, &flags, NULL) ||
        !(flags & 1))
        return false;
    const volatile uint8_t *bytes = (const volatile uint8_t *)slot;
    uint64_t value = 0;
    for (unsigned i = 0; i < trace->caller_bits / 8; i++)
        value |= (uint64_t)bytes[i] << (i * 8);
    if (value < 4096 || value > 0x00007FFFFFFFFFFFULL ||
        paging_query_mapping_in_cr3(trace->cr3, value, &flags, NULL) ||
        !(flags & 1) || (flags & (1ULL << 63)))
        return false;
    *caller = value;
    return true;
}

bool hwbp_dispatch(x86_interrupt_frame_t *frame)
{
    uint64_t dr6;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));

    bool handled = false;
    for (int slot = 0; slot < 4; slot++) {
        if (!(dr6 & (1ULL << slot)) || !hwbps[slot].active)
            continue;

        hwbp_t *breakpoint = &hwbps[slot];
        hwbp_trace_t *trace = &execution_traces[slot];
        if (trace->limit) {
            uint64_t cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            hwbp_trace_result_t result = hwbp_trace_accept(
                trace, frame->rip, cr3,
                (const volatile uint8_t *)frame->rip);
            /* An out-of-scope hit still needs RF, or the same instruction
             * would immediately fault again before it can execute. */
            handled = true;
            if (result == HWBP_TRACE_IGNORE) continue;
            if (result == HWBP_TRACE_MISMATCH) {
                hwbp_clear(slot);
                serial_puts("[HWBP-TRACE] disabled signature mismatch slot=");
                serial_putdec((uint64_t)slot);
                serial_puts("\n");
                continue;
            }

            extern int32_t proc_current_pid(void);
            breakpoint->hit_count = trace->hits;
            serial_puts("[HWBP-TRACE] slot=");
            serial_putdec((uint64_t)slot);
            serial_puts(" pid=");
            serial_putdec((uint32_t)proc_current_pid());
            serial_puts(" cr3=");
            serial_puthex(cr3, 16);
            serial_puts(" rip=");
            serial_puthex(frame->rip, 16);
            serial_puts(trace->reg == HWBP_TRACE_EDI32
                ? " edi32=" : " eax32=");
            serial_puthex((uint32_t)(trace->reg == HWBP_TRACE_EDI32
                ? frame->rdi : frame->rax), 8);
            serial_puts(" hits=");
            serial_putdec(trace->hits);
            if (trace->caller_bits) {
                uint64_t caller;
                serial_puts(" caller=");
                if (hwbp_trace_read_caller(trace, frame->rsp, &caller))
                    serial_puthex(caller, 16);
                else
                    serial_puts("unavailable");
            }
            serial_puts("\n");
            if (result == HWBP_TRACE_LIMIT) {
                hwbp_clear(slot);
                serial_puts("[HWBP-TRACE] disabled hit limit slot=");
                serial_putdec((uint64_t)slot);
                serial_puts("\n");
            }
            continue;
        }
        breakpoint->hit_count++;
        serial_puts("[HWBP] slot=");
        serial_putdec((uint64_t)slot);
        if (breakpoint->name[0]) {
            serial_puts(" name=");
            serial_puts(breakpoint->name);
        }
        serial_puts(" addr=0x");
        serial_puthex(dr_get_addr(slot), 16);
        serial_puts(" rip=0x");
        serial_puthex(frame->rip, 16);
        serial_puts(" hits=");
        serial_putdec(breakpoint->hit_count);
        serial_puts("\n");
        handled = true;
    }

    dr6_clear();
    if (handled)
        frame->rflags |= (1ULL << 16);
    return handled;
}

static bool hwbp_text_equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int hwbp_hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hwbp_parse_number(const char *text, unsigned base, uint64_t max,
                              uint64_t *value)
{
    uint64_t result = 0;
    if (base == 16 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) text += 2;
    if (!*text) return false;
    for (; *text; text++) {
        int digit = hwbp_hex_digit(*text);
        if (digit < 0 || (unsigned)digit >= base ||
            (uint64_t)digit > max || result > (max - digit) / base)
            return false;
        result = result * base + (unsigned)digit;
    }
    *value = result;
    return true;
}

static bool hwbp_parse_signature(const char *text, uint8_t *signature)
{
    for (unsigned i = 0; i < HWBP_TRACE_SIGNATURE_BYTES; i++) {
        /* Test each terminator before reading the next character. */
        if (!text[0] || !text[1]) return false;
        int high = hwbp_hex_digit(text[0]);
        int low = hwbp_hex_digit(text[1]);
        if (high < 0 || low < 0) return false;
        signature[i] = (uint8_t)((high << 4) | low);
        text += 2;
    }
    return !*text;
}

static bool hwbp_parse_caller(const char *text, unsigned *bits, unsigned *offset)
{
    if (!text || !bits || !offset) return false;
    const char *prefixes[] = {"caller32:", "caller64:"};
    for (unsigned p = 0; p < 2; p++) {
        unsigned i = 0;
        while (i < 9 && text[i] == prefixes[p][i]) i++;
        if (i != 9) continue;
        uint64_t parsed;
        unsigned width = p ? 64 : 32;
        if (!hwbp_parse_number(text + 9, 10, 512, &parsed) ||
            !hwbp_trace_caller_config(width, (unsigned)parsed))
            return false;
        *bits = width;
        *offset = (unsigned)parsed;
        return true;
    }
    return false;
}

static int hwbp_trace_arm(int slot, uint32_t target_pid, uint64_t addr,
                          hwbp_trace_reg_t reg, uint32_t limit,
                          const uint8_t *signature, unsigned caller_bits,
                          unsigned caller_offset)
{
    extern uint32_t smp_current_cpu(void);
    extern void *proc_find(uint32_t pid);
    extern uint32_t proc_state_of(void *process);
    extern uint64_t proc_get_cr3_pid(int pid);
    extern uint64_t paging_get_kernel_cr3(void);
    if (slot < 0 || slot >= 4 || !target_pid || smp_current_cpu() != 0 ||
        !hwbp_trace_caller_config(caller_bits, caller_offset))
        return -1;

    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    int result = -1;
    void *target = proc_find(target_pid);
    hwbp_trace_t trace;
    if (!target || proc_state_of(target) == 4) {
        result = -3;
    } else if (hwbps[slot].active) {
        result = -2;
    } else {
        uint64_t cr3 = proc_get_cr3_pid((int)target_pid);
        if (!cr3) cr3 = paging_get_kernel_cr3();
        if (hwbp_trace_prepare(&trace, addr, cr3 & ~4095ULL,
                               reg, limit, signature)) {
            trace.caller_bits = (uint8_t)caller_bits;
            trace.caller_offset = (uint16_t)caller_offset;
            /* Configure the policy before enabling interrupts; regular
             * watch/unwatch operations invalidate this private policy. */
            result = hwbp_set(slot, addr, HWBP_EXECUTE, HWBP_LEN_1, "scalar32");
            if (!result) execution_traces[slot] = trace;
        }
    }
    if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
    return result;
}

/* The SSH shell calls this directly while a console PE is running. Only
 * bounded, fingerprinted execution probes are admitted remotely. */
int hwbp_command(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && hwbp_text_equal(argv[1], "status"))) {
        hwbp_list();
        for (int slot = 0; slot < 4; slot++) {
            hwbp_trace_t *trace = &execution_traces[slot];
            if (!trace->limit) continue;
            serial_puts("[HWBP-TRACE] status slot=");
            serial_putdec((uint64_t)slot);
            serial_puts(" active=");
            serial_putdec(trace->active);
            serial_puts(" cr3=");
            serial_puthex(trace->cr3, 16);
            serial_puts(" hits=");
            serial_putdec(trace->hits);
            serial_puts(" limit=");
            serial_putdec(trace->limit);
            serial_puts(" result=");
            serial_putdec(trace->last_result);
            if (trace->caller_bits) {
                serial_puts(" caller-bits=");
                serial_putdec(trace->caller_bits);
                serial_puts(" caller-offset=");
                serial_putdec(trace->caller_offset);
            }
            serial_puts("\n");
        }
        return 0;
    }

    uint64_t slot, pid, addr, limit;
    if (argc == 3 && hwbp_text_equal(argv[1], "clear") &&
        hwbp_parse_number(argv[2], 10, 3, &slot)) {
        extern uint32_t smp_current_cpu(void);
        if (smp_current_cpu() != 0) return -1;
        hwbp_clear((int)slot);
        serial_puts("[HWBP-TRACE] cleared slot=");
        serial_putdec(slot);
        serial_puts("\n");
        return 0;
    }
    uint8_t signature[HWBP_TRACE_SIGNATURE_BYTES];
    hwbp_trace_reg_t reg;
    if ((argc == 8 || argc == 9) && hwbp_text_equal(argv[1], "trace") &&
        hwbp_parse_number(argv[2], 10, 3, &slot) &&
        hwbp_parse_number(argv[3], 10, 0x7FFFFFFF, &pid) && pid &&
        hwbp_parse_number(argv[4], 16, 0x00007FFFFFFFFFFFULL, &addr) &&
        hwbp_parse_number(argv[6], 10, HWBP_TRACE_MAX_HITS, &limit) && limit &&
        hwbp_parse_signature(argv[7], signature)) {
        if (hwbp_text_equal(argv[5], "eax32")) reg = HWBP_TRACE_EAX32;
        else if (hwbp_text_equal(argv[5], "edi32")) reg = HWBP_TRACE_EDI32;
        else goto usage;
        unsigned caller_bits = 0, caller_offset = 0;
        if (argc == 9 && !hwbp_parse_caller(argv[8], &caller_bits, &caller_offset))
            goto usage;
        int result = hwbp_trace_arm((int)slot, (uint32_t)pid, addr, reg,
                                     (uint32_t)limit, signature,
                                     caller_bits, caller_offset);
        if (result) {
            serial_puts(result == -2 ? "[HWBP-TRACE] slot busy\n" :
                result == -3 ? "[HWBP-TRACE] target not alive\n" :
                "[HWBP-TRACE] invalid address or configuration\n");
            return -1;
        }
        serial_puts("[HWBP-TRACE] armed slot=");
        serial_putdec(slot);
        serial_puts(" target=");
        serial_putdec(pid);
        serial_puts("\n");
        return 0;
    }
usage:
    serial_puts("Usage: hwbp [status]; hwbp clear <slot>\n"
                "       hwbp trace <slot> <kernel-pid> <addr-hex> "
                "<eax32|edi32> <limit:1..32> <16-byte-signature-hex> "
                "[caller32:<offset>|caller64:<offset>]\n");
    return -1;
}

void hwbp_list(void)
{
    serial_puts("[HWBP] slots:\n");
    for (int i = 0; i < 4; i++) {
        serial_puts("  [");
        serial_putdec(i);
        serial_puts("] ");
        if (!hwbps[i].active) {
            serial_puts("(free)\n");
            continue;
        }
        static const char *cond_names[] = { "exec", "write", "io", "rw" };
        serial_puts(cond_names[hwbps[i].cond & 3]);
        serial_puts(" ");
        static const uint8_t len_bytes[] = { 1, 2, 8, 4 };
        serial_putdec(len_bytes[hwbps[i].len & 3]);
        serial_puts("B @ 0x");
        serial_puthex(hwbps[i].addr, 12);
        if (hwbps[i].name[0]) { serial_puts(" \""); serial_puts(hwbps[i].name); serial_puts("\""); }
        serial_puts(" hits=");
        serial_putdec(hwbps[i].hit_count);
        serial_puts("\n");
    }
}
