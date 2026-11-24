/*
 * OsitoK x86-64 — Kernel Symbol Table (/proc/kallsyms)
 *
 * Maps kernel addresses to function names for readable stack traces.
 * Symbols registered at boot from kmod_register_symbol() and
 * manually from key kernel functions.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);

#define KALLSYMS_MAX 256

typedef struct {
    uint64_t    addr;
    const char *name;
    char        type;  /* 'T'=text, 'D'=data, 't'=static text */
} kallsym_entry_t;

static kallsym_entry_t kallsyms[KALLSYMS_MAX];
static int kallsym_count;
static bool kallsyms_sorted;

/* ── Registration ────────────────────────────────────────────── */

void kallsyms_add(const char *name, uint64_t addr, char type)
{
    if (kallsym_count >= KALLSYMS_MAX) return;
    kallsyms[kallsym_count].name = name;
    kallsyms[kallsym_count].addr = addr;
    kallsyms[kallsym_count].type = type;
    kallsym_count++;
    kallsyms_sorted = false;
}

/* Sort by address (insertion sort — only done once) */
static void kallsyms_sort(void)
{
    if (kallsyms_sorted) return;
    for (int i = 1; i < kallsym_count; i++) {
        kallsym_entry_t tmp = kallsyms[i];
        int j = i - 1;
        while (j >= 0 && kallsyms[j].addr > tmp.addr) {
            kallsyms[j + 1] = kallsyms[j];
            j--;
        }
        kallsyms[j + 1] = tmp;
    }
    kallsyms_sorted = true;
}

/* ── Lookup ──────────────────────────────────────────────────── */

/* Find the function containing an address (binary search).
 * Returns the symbol with the highest address <= target. */
const char *kallsyms_lookup(uint64_t addr, uint64_t *offset_out)
{
    if (kallsym_count == 0) return NULL;
    kallsyms_sort();

    /* Binary search for largest addr <= target */
    int lo = 0, hi = kallsym_count - 1;
    int best = -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kallsyms[mid].addr <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (best < 0) return NULL;
    if (offset_out) *offset_out = addr - kallsyms[best].addr;
    return kallsyms[best].name;
}

/* ── Symbolicate a stack trace ───────────────────────────────── */

void kallsyms_print_addr(uint64_t addr)
{
    uint64_t offset;
    const char *name = kallsyms_lookup(addr, &offset);
    serial_puts("0x");
    serial_puthex(addr, 16);
    if (name) {
        serial_puts(" <");
        serial_puts(name);
        if (offset > 0) {
            serial_puts("+0x");
            serial_puthex(offset, 4);
        }
        serial_puts(">");
    }
}

/* ── /proc/kallsyms format ───────────────────────────────────── */

int kallsyms_read(char *buf, int max_len)
{
    kallsyms_sort();
    int p = 0;
    for (int i = 0; i < kallsym_count && p < max_len - 40; i++) {
        /* Format: "ffffffff81000000 T function_name\n" */
        uint64_t a = kallsyms[i].addr;
        for (int b = 60; b >= 0; b -= 4) {
            char hex = "0123456789abcdef"[(a >> b) & 0xF];
            if (p < max_len - 1) buf[p++] = hex;
        }
        if (p < max_len - 1) buf[p++] = ' ';
        if (p < max_len - 1) buf[p++] = kallsyms[i].type;
        if (p < max_len - 1) buf[p++] = ' ';
        const char *n = kallsyms[i].name;
        while (*n && p < max_len - 1) buf[p++] = *n++;
        if (p < max_len - 1) buf[p++] = '\n';
    }
    buf[p] = '\0';
    return p;
}

int kallsyms_count(void) { return kallsym_count; }
