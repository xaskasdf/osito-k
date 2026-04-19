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
