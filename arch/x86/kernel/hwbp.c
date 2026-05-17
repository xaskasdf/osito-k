/*
 * arch/x86/kernel/hwbp.c — hardware breakpoint/watchpoint manager
 *
 * Handlers read DR6 at entry to identify which DR register triggered,
 * then print the watchpoint info + triggering RIP. Integrates with the
 * existing #DB handler in idt.c.
 */

#include "../include/hwbp.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern void serial_puthex(uint64_t v, int d);

hwbp_t hwbps[4];

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
    return 0;
}

void hwbp_clear_all(void)
{
    for (int i = 0; i < 4; i++) hwbp_clear(i);
}

/* Forward-declared frame type to avoid pulling in idt.h */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

/* Render the four bytes at `va` as a wide-string when they look like
 * one ((low byte printable ASCII, high byte 0). Otherwise no-op. */
static void hwbp_try_wstr(const char *label, uint32_t va)
{
    if (va < 0x10000 || (uint64_t)va >= 0x80000000ULL) return;
    volatile uint16_t *w = (volatile uint16_t *)(uintptr_t)va;
    uint16_t w0 = *w;
    uint8_t lo = (uint8_t)(w0 & 0xFF), hi = (uint8_t)(w0 >> 8);
    if (lo < 0x20 || lo >= 0x7F || hi != 0) return;
    serial_puts("    "); serial_puts(label); serial_puts("=L\"");
    char tmp[96]; int n = 0;
    for (int i = 0; i < 95; i++) {
        uint16_t c = w[i];
        if (c == 0) break;
        tmp[n++] = (c < 0x20 || c >= 0x7F) ? '?' : (char)c;
    }
    tmp[n] = 0;
    serial_puts(tmp);
    serial_puts("\"\n");
}

bool hwbp_dispatch(struct interrupt_frame *frame)
{
    uint64_t dr6;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));

    bool handled = false;
    for (int i = 0; i < 4; i++) {
        if (!(dr6 & (1ULL << i))) continue;
        if (!hwbps[i].active) continue;

        hwbps[i].hit_count++;
        serial_puts("[HWBP] slot ");
        serial_putdec(i);
        if (hwbps[i].name[0]) {
            serial_puts(" (");
            serial_puts(hwbps[i].name);
            serial_puts(")");
        }
        serial_puts(" hit @ addr=0x");
        serial_puthex(dr_get_addr(i), 12);
        serial_puts(" from RIP=0x");
        serial_puthex(frame->rip, 12);
        serial_puts(" hits=");
        serial_putdec(hwbps[i].hit_count);
        serial_puts("\n");

        /* Detail dump: a HWBP_EXECUTE fires BEFORE the instruction runs,
         * so the caller's stack still holds the soon-to-be-called
         * arguments. Print ECX/EDX, then 8 dwords at [RSP], then try a
         * wide-string render on each pointer-shaped value. PE32 in
         * compat mode has 32-bit ESP — interrupt_frame->rsp holds the
         * full saved value; truncate to 32 bits to read user memory. */
        if (hwbps[i].hit_count <= 8) {
            uint32_t esp32 = (uint32_t)frame->rsp;
            uint32_t ecx32 = (uint32_t)frame->rcx;
            uint32_t edx32 = (uint32_t)frame->rdx;
            serial_puts("  esp=0x"); serial_puthex(esp32, 8);
            serial_puts(" ecx=0x"); serial_puthex(ecx32, 8);
            serial_puts(" edx=0x"); serial_puthex(edx32, 8);
            serial_puts("\n");

            volatile uint32_t *st = (volatile uint32_t *)(uintptr_t)esp32;
            uint32_t args[8] = {0};
            for (int k = 0; k < 8; k++) {
                /* Coarse readability — must be in user/compat32 range. */
                uint32_t a = esp32 + (uint32_t)(k * 4);
                if (a < 0x10000 || (uint64_t)a >= 0x80000000ULL) break;
                args[k] = st[k];
            }
            serial_puts("  stack:");
            for (int k = 0; k < 8; k++) {
                serial_puts(" ["); serial_putdec((uint64_t)k); serial_puts("]=");
                serial_puthex(args[k], 8);
            }
            serial_puts("\n");

            hwbp_try_wstr("ECX",  ecx32);
            hwbp_try_wstr("EDX",  edx32);
            for (int k = 0; k < 4; k++) {
                char lbl[8] = { 'a', 'r', 'g', (char)('0' + k), 0, 0, 0, 0 };
                hwbp_try_wstr(lbl, args[k]);
            }
        }

        handled = true;
    }

    /* Clear DR6 status bits and set RF (Resume Flag) in frame's RFLAGS so
     * we don't re-trigger on this instruction after iretq. */
    dr6_clear();
    if (handled) frame->rflags |= (1ULL << 16);
    return handled;
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
