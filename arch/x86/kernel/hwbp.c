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

static struct {
    uint64_t seq;
    uint32_t thisp;
    uint32_t ret;
    uint32_t key;
    uint32_t action;
    uint32_t aux;
} hwbp_last_cie;

static struct {
    uint64_t seq;
    uint32_t thisp;
    uint32_t ret;
    uint32_t vp;
    uint32_t key;
    uint32_t action;
    uint32_t aux;
} hwbp_last_eie;

static struct {
    uint64_t seq;
    uint32_t thisp;
    uint32_t ret;
    uint32_t key;
    uint32_t action;
    uint32_t aux;
    uint32_t state;
} hwbp_last_uiproc;

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

static void hwbp_put_ascii_preview(uint32_t va)
{
    if (va < 0x10000 || (uint64_t)va >= 0x80000000ULL) {
        serial_puts("<bad>");
        return;
    }
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)va;
    serial_puts("\"");
    char tmp[64];
    int n = 0;
    for (int i = 0; i < 63; i++) {
        uint8_t c = p[i];
        if (c == 0) break;
        tmp[n++] = (c < 0x20 || c >= 0x7F) ? '?' : (char)c;
    }
    tmp[n] = 0;
    serial_puts(tmp);
    serial_puts("\"");
}

static int hwbp_looks_wstr(uint32_t va)
{
    if (va < 0x10000 || (uint64_t)va >= 0x80000000ULL) return 0;
    volatile uint16_t *w = (volatile uint16_t *)(uintptr_t)va;
    uint16_t c0 = w[0];
    uint16_t c1 = w[1];
    if (c0 < 0x20 || c0 >= 0x7F) return 0;
    return c1 == 0 || (c1 >= 0x20 && c1 < 0x7F);
}

static int hwbp_wstr_prefix(uint32_t va, const char *prefix)
{
    if (!hwbp_looks_wstr(va)) return 0;
    volatile uint16_t *w = (volatile uint16_t *)(uintptr_t)va;
    for (int i = 0; prefix[i]; i++) {
        if ((uint8_t)w[i] != (uint8_t)prefix[i]) return 0;
    }
    return 1;
}

static void hwbp_put_tchar_preview(uint32_t va)
{
    if (hwbp_looks_wstr(va)) {
        volatile uint16_t *w = (volatile uint16_t *)(uintptr_t)va;
        serial_puts("L\"");
        char tmp[96];
        int n = 0;
        for (int i = 0; i < 95; i++) {
            uint16_t c = w[i];
            if (c == 0) break;
            tmp[n++] = (c < 0x20 || c >= 0x7F) ? '?' : (char)c;
        }
        tmp[n] = 0;
        serial_puts(tmp);
        serial_puts("\"");
        return;
    }
    hwbp_put_ascii_preview(va);
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

        {
            const char *nm = hwbps[i].name;
            int is_cieret = (nm[0] == 'c' && nm[1] == 'i' && nm[2] == 'e' &&
                             nm[3] == 'r');
            if (is_cieret) {
                serial_puts("[CIE-RET] seq=");
                serial_putdec(hwbp_last_cie.seq);
                serial_puts(" this=0x");
                serial_puthex(hwbp_last_cie.thisp, 8);
                serial_puts(" ret=0x");
                serial_puthex((uint32_t)frame->rip, 8);
                serial_puts(" eax=0x");
                serial_puthex((uint32_t)frame->rax, 8);
                serial_puts(" key=0x");
                serial_puthex(hwbp_last_cie.key, 4);
                serial_puts(" action=");
                serial_putdec(hwbp_last_cie.action);
                serial_puts(" aux=0x");
                serial_puthex(hwbp_last_cie.aux, 8);
                serial_puts("\n");
                hwbp_clear(i);
                handled = true;
                continue;
            }
            int is_eieret = (nm[0] == 'e' && nm[1] == 'i' && nm[2] == 'e' &&
                             nm[3] == 'r');
            if (is_eieret) {
                serial_puts("[EIE-RET] seq=");
                serial_putdec(hwbp_last_eie.seq);
                serial_puts(" this=0x");
                serial_puthex(hwbp_last_eie.thisp, 8);
                serial_puts(" ret=0x");
                serial_puthex((uint32_t)frame->rip, 8);
                serial_puts(" eax=0x");
                serial_puthex((uint32_t)frame->rax, 8);
                serial_puts(" vp=0x");
                serial_puthex(hwbp_last_eie.vp, 8);
                serial_puts(" key=0x");
                serial_puthex(hwbp_last_eie.key, 4);
                serial_puts(" action=");
                serial_putdec(hwbp_last_eie.action);
                serial_puts(" aux=0x");
                serial_puthex(hwbp_last_eie.aux, 8);
                serial_puts("\n");
                hwbp_clear(i);
                handled = true;
                continue;
            }
            int is_uipret = (nm[0] == 'u' && nm[1] == 'i' && nm[2] == 'p' &&
                             nm[3] == 'r');
            if (is_uipret) {
                serial_puts("[UIP-RET] seq=");
                serial_putdec(hwbp_last_uiproc.seq);
                serial_puts(" this=0x");
                serial_puthex(hwbp_last_uiproc.thisp, 8);
                serial_puts(" ret=0x");
                serial_puthex((uint32_t)frame->rip, 8);
                serial_puts(" eax=0x");
                serial_puthex((uint32_t)frame->rax, 8);
                serial_puts(" key=0x");
                serial_puthex(hwbp_last_uiproc.key, 4);
                serial_puts(" action=");
                serial_putdec(hwbp_last_uiproc.action);
                serial_puts(" aux=0x");
                serial_puthex(hwbp_last_uiproc.aux, 8);
                serial_puts(" state=0x");
                serial_puthex(hwbp_last_uiproc.state, 2);
                serial_puts("\n");
                hwbp_clear(i);
                handled = true;
                continue;
            }
        }

        /* UT99/WinDrv input probe: CauseInputEvent(this, key, action, delta).
         * This one is hot enough that the generic HWBP string-oriented dump is
         * just noise; print the thiscall arguments as a compact input trace. */
        {
            const char *nm = hwbps[i].name;
            int is_cie = (nm[0] == 'c' && nm[1] == 'i' && nm[2] == 'e') ||
                         ((uint32_t)hwbps[i].addr == 0x11106560U);
            if (is_cie) {
                uint32_t esp32 = (uint32_t)frame->rsp;
                uint32_t ret = 0, key = 0, action = 0, aux = 0;
                if (esp32 >= 0x10000 && esp32 < 0x7FFFF000U) {
                    volatile uint32_t *st = (volatile uint32_t *)(uintptr_t)esp32;
                    ret    = st[0];
                    key    = st[1];
                    action = st[2];
                    aux    = st[3];
                }
                int axis = (action == 4);
                int log_this = !axis || hwbps[i].hit_count <= 12;
                if (log_this) {
                    serial_puts("[CIE] hit=");
                    serial_putdec(hwbps[i].hit_count);
                    serial_puts(" this=0x");
                    serial_puthex((uint64_t)(uint32_t)frame->rcx, 8);
                    serial_puts(" ret=0x");
                    serial_puthex(ret, 8);
                    serial_puts(" key=0x");
                    serial_puthex(key, 4);
                    serial_puts(" action=");
                    serial_putdec(action);
                    serial_puts(" aux=0x");
                    serial_puthex(aux, 8);
                    serial_puts("\n");
                }
                hwbp_last_cie.seq = hwbps[i].hit_count;
                hwbp_last_cie.thisp = (uint32_t)frame->rcx;
                hwbp_last_cie.ret = ret;
                hwbp_last_cie.key = key;
                hwbp_last_cie.action = action;
                hwbp_last_cie.aux = aux;
                if (!axis && ret >= 0x10000000U && ret < 0x80000000U)
                    hwbp_set(2, ret, HWBP_EXECUTE, HWBP_LEN_1, "cieret");
                handled = true;
                continue;
            }
        }

        /* UT99/UInput::Process(this, outdev, key, action, delta). This is
         * where held-key state is resolved into bound commands. */
        {
            const char *nm = hwbps[i].name;
            int is_uiproc = (nm[0] == 'u' && nm[1] == 'i' && nm[2] == 'p') ||
                            ((uint32_t)hwbps[i].addr == 0x10393E50U);
            if (is_uiproc) {
                uint32_t esp32 = (uint32_t)frame->rsp;
                uint32_t ret = 0, outdev = 0, key = 0, action = 0, aux = 0;
                uint32_t entry = 0, cmd = 0, len = 0, max = 0, state = 0;
                uint32_t thisp = (uint32_t)frame->rcx;
                if (esp32 >= 0x10000 && esp32 < 0x7FFFF000U) {
                    volatile uint32_t *st = (volatile uint32_t *)(uintptr_t)esp32;
                    ret    = st[0];
                    outdev = st[1];
                    key    = st[2];
                    action = st[3];
                    aux    = st[4];
                }
                if (thisp >= 0x10000 && thisp < 0x7FFFF000U && key < 0x100) {
                    entry = thisp + 0x2AC + key * 12;
                    cmd = *(volatile uint32_t *)(uintptr_t)entry;
                    len = *(volatile uint32_t *)(uintptr_t)(entry + 4);
                    max = *(volatile uint32_t *)(uintptr_t)(entry + 8);
                    state = *(volatile uint8_t *)(uintptr_t)(thisp + 0xEB0 + key);
                }
                int interesting = (key == 0x25 || key == 0x26 || key == 0x27 ||
                                   key == 0x28 || key == 0xE4 || key == 0xE5);
                int log_this = interesting || hwbps[i].hit_count <= 24;
                if (log_this) {
                    serial_puts("[UIP] hit=");
                    serial_putdec(hwbps[i].hit_count);
                    serial_puts(" this=0x");
                    serial_puthex(thisp, 8);
                    serial_puts(" ret=0x");
                    serial_puthex(ret, 8);
                    serial_puts(" out=0x");
                    serial_puthex(outdev, 8);
                    serial_puts(" key=0x");
                    serial_puthex(key, 4);
                    serial_puts(" action=");
                    serial_putdec(action);
                    serial_puts(" aux=0x");
                    serial_puthex(aux, 8);
                    serial_puts(" state=0x");
                    serial_puthex(state, 2);
                    serial_puts(" cmd=0x");
                    serial_puthex(cmd, 8);
                    serial_puts(" len=");
                    serial_putdec(len);
                    serial_puts(" max=");
                    serial_putdec(max);
                    serial_puts(" text=");
                    hwbp_put_tchar_preview(cmd);
                    serial_puts("\n");
                }
                hwbp_last_uiproc.seq = hwbps[i].hit_count;
                hwbp_last_uiproc.thisp = thisp;
                hwbp_last_uiproc.ret = ret;
                hwbp_last_uiproc.key = key;
                hwbp_last_uiproc.action = action;
                hwbp_last_uiproc.aux = aux;
                hwbp_last_uiproc.state = state;
                if (log_this && ret >= 0x10000000U && ret < 0x80000000U)
                    hwbp_set(2, ret, HWBP_EXECUTE, HWBP_LEN_1, "uipret");
                handled = true;
                continue;
            }
        }

        /* UT99/UInput::Exec(this=UInput+0x28, cmd, outdev). UInput::Process
         * calls this with bound commands such as MoveForward. Alias expansion
         * should recursively re-enter here with Axis aBaseY... */
        {
            const char *nm = hwbps[i].name;
            int is_uiexec = (nm[0] == 'u' && nm[1] == 'i' && nm[2] == 'e' &&
                             nm[3] == 'x') ||
                            ((uint32_t)hwbps[i].addr == 0x10393800U);
            if (is_uiexec) {
                uint32_t esp32 = (uint32_t)frame->rsp;
                uint32_t ret = 0, cmd = 0, outdev = 0;
                uint32_t sub = (uint32_t)frame->rcx;
                uint32_t owner = (sub >= 0x28) ? sub - 0x28 : 0;
                uint32_t vp = 0, action = 0, scale = 0;
                if (esp32 >= 0x10000 && esp32 < 0x7FFFF000U) {
                    volatile uint32_t *st = (volatile uint32_t *)(uintptr_t)esp32;
                    ret    = st[0];
                    cmd    = st[1];
                    outdev = st[2];
                }
                if (sub >= 0x10000 && sub < 0x7FFFF000U) {
                    vp     = *(volatile uint32_t *)(uintptr_t)(sub + 0xE78);
                    action = *(volatile uint32_t *)(uintptr_t)(sub + 0xE80);
                    scale  = *(volatile uint32_t *)(uintptr_t)(sub + 0xE84);
                }
                int interesting =
                    hwbp_wstr_prefix(cmd, "Move") ||
                    hwbp_wstr_prefix(cmd, "Axis") ||
                    hwbp_wstr_prefix(cmd, "Strafe") ||
                    hwbp_wstr_prefix(cmd, "Turn") ||
                    hwbp_wstr_prefix(cmd, "Button") ||
                    hwbp_wstr_prefix(cmd, "Fire") ||
                    hwbp_wstr_prefix(cmd, "AltFire");
                if (interesting || hwbps[i].hit_count <= 32) {
                    serial_puts("[UIEXEC] hit=");
                    serial_putdec(hwbps[i].hit_count);
                    serial_puts(" sub=0x");
                    serial_puthex(sub, 8);
                    serial_puts(" owner=0x");
                    serial_puthex(owner, 8);
                    serial_puts(" ret=0x");
                    serial_puthex(ret, 8);
                    serial_puts(" out=0x");
                    serial_puthex(outdev, 8);
                    serial_puts(" vp=0x");
                    serial_puthex(vp, 8);
                    serial_puts(" action=");
                    serial_putdec(action);
                    serial_puts(" scale=0x");
                    serial_puthex(scale, 8);
                    serial_puts(" cmd=0x");
                    serial_puthex(cmd, 8);
                    serial_puts(" text=");
                    hwbp_put_tchar_preview(cmd);
                    serial_puts("\n");
                }
                handled = true;
                continue;
            }
        }

        /* UT99/UInput::Exec Axis success path. At 0x10393B05, EDI still holds
         * the resolved destination float, after the action==2/4 store paths. */
        {
            const char *nm = hwbps[i].name;
            int is_uiax = (nm[0] == 'u' && nm[1] == 'i' && nm[2] == 'a' &&
                           nm[3] == 'x') ||
                          ((uint32_t)hwbps[i].addr == 0x10393B05U);
            if (is_uiax) {
                uint32_t ebp32 = (uint32_t)frame->rbp;
                uint32_t cmd = 0, outdev = 0;
                uint32_t sub = (uint32_t)frame->rsi;
                uint32_t owner = (sub >= 0x28) ? sub - 0x28 : 0;
                uint32_t target = (uint32_t)frame->rdi;
                uint32_t value = 0, vp = 0, actor = 0, action = 0, scale = 0;
                if (ebp32 >= 0x10000 && ebp32 < 0x7FFFF000U) {
                    volatile uint32_t *bp = (volatile uint32_t *)(uintptr_t)ebp32;
                    cmd = bp[2];
                    outdev = bp[3];
                }
                if (target >= 0x10000 && target < 0x7FFFF000U)
                    value = *(volatile uint32_t *)(uintptr_t)target;
                if (sub >= 0x10000 && sub < 0x7FFFF000U) {
                    vp     = *(volatile uint32_t *)(uintptr_t)(sub + 0xE78);
                    action = *(volatile uint32_t *)(uintptr_t)(sub + 0xE80);
                    scale  = *(volatile uint32_t *)(uintptr_t)(sub + 0xE84);
                    if (vp >= 0x10000 && vp < 0x7FFFF000U)
                        actor = *(volatile uint32_t *)(uintptr_t)(vp + 0x30);
                }
                if (hwbps[i].hit_count <= 120) {
                    serial_puts("[UIAX] hit=");
                    serial_putdec(hwbps[i].hit_count);
                    serial_puts(" sub=0x");
                    serial_puthex(sub, 8);
                    serial_puts(" owner=0x");
                    serial_puthex(owner, 8);
                    serial_puts(" target=0x");
                    serial_puthex(target, 8);
                    serial_puts(" value=0x");
                    serial_puthex(value, 8);
                    serial_puts(" out=0x");
                    serial_puthex(outdev, 8);
                    serial_puts(" vp=0x");
                    serial_puthex(vp, 8);
                    serial_puts(" actor=0x");
                    serial_puthex(actor, 8);
                    serial_puts(" action=");
                    serial_putdec(action);
                    serial_puts(" scale=0x");
                    serial_puthex(scale, 8);
                    serial_puts(" cmd=0x");
                    serial_puthex(cmd, 8);
                    serial_puts(" text=");
                    hwbp_put_tchar_preview(cmd);
                    serial_puts("\n");
                }
                handled = true;
                continue;
            }
        }

        /* UT99/UInput::DirectAxis(this, key, speed, delta). If this fires,
         * binding execution reached the axis write path. */
        {
            const char *nm = hwbps[i].name;
            int is_uaxis = (nm[0] == 'u' && nm[1] == 'a' && nm[2] == 'x') ||
                           ((uint32_t)hwbps[i].addr == 0x10393F90U);
            if (is_uaxis) {
                uint32_t esp32 = (uint32_t)frame->rsp;
                uint32_t ret = 0, key = 0, speed = 0, delta = 0;
                uint32_t thisp = (uint32_t)frame->rcx;
                uint32_t vp = 0, actor = 0;
                if (esp32 >= 0x10000 && esp32 < 0x7FFFF000U) {
                    volatile uint32_t *st = (volatile uint32_t *)(uintptr_t)esp32;
                    ret = st[0];
                    key = st[1];
                    speed = st[2];
                    delta = st[3];
                }
                if (thisp >= 0x10000 && thisp < 0x7FFFF000U) {
                    vp = *(volatile uint32_t *)(uintptr_t)(thisp + 0xEA0);
                    if (vp >= 0x10000 && vp < 0x7FFFF000U)
                        actor = *(volatile uint32_t *)(uintptr_t)(vp + 0x30);
                }
                if (hwbps[i].hit_count <= 80) {
                    serial_puts("[UAXIS] hit=");
                    serial_putdec(hwbps[i].hit_count);
                    serial_puts(" this=0x");
                    serial_puthex(thisp, 8);
                    serial_puts(" ret=0x");
                    serial_puthex(ret, 8);
                    serial_puts(" key=0x");
                    serial_puthex(key, 4);
                    serial_puts(" speed=0x");
                    serial_puthex(speed, 8);
                    serial_puts(" delta=0x");
                    serial_puthex(delta, 8);
                    serial_puts(" vp=0x");
                    serial_puthex(vp, 8);
                    serial_puts(" actor=0x");
                    serial_puthex(actor, 8);
                    serial_puts("\n");
                }
                handled = true;
                continue;
            }
        }

        /* UT99/Engine input probe: UEngine::InputEvent(this, viewport,
         * key, action, delta).  WinDrv reaches it via the engine vtable
         * entry used by CauseInputEvent; hook the real implementation so
         * we catch direct virtual calls as well as export-thunk calls. */
        {
            const char *nm = hwbps[i].name;
            int is_eie = (nm[0] == 'e' && nm[1] == 'i' && nm[2] == 'e') ||
                         ((uint32_t)hwbps[i].addr == 0x103839C0U);
            if (is_eie) {
                uint32_t esp32 = (uint32_t)frame->rsp;
                uint32_t ret = 0, vp = 0, key = 0, action = 0, aux = 0;
                uint32_t actor = 0, vp34 = 0, p4a8 = 0, p228 = 0, p24 = 0;
                if (esp32 >= 0x10000 && esp32 < 0x7FFFF000U) {
                    volatile uint32_t *st = (volatile uint32_t *)(uintptr_t)esp32;
                    ret    = st[0];
                    vp     = st[1];
                    key    = st[2];
                    action = st[3];
                    aux    = st[4];
                }
                if (vp >= 0x10000 && vp < 0x7FFFF000U) {
                    actor = *(volatile uint32_t *)(uintptr_t)(vp + 0x30);
                    vp34  = *(volatile uint32_t *)(uintptr_t)(vp + 0x34);
                    if (actor >= 0x10000 && actor < 0x7FFFF000U) {
                        p4a8 = *(volatile uint32_t *)(uintptr_t)(actor + 0x4A8);
                        if (p4a8 >= 0x10000 && p4a8 < 0x7FFFF000U) {
                            p228 = *(volatile uint32_t *)(uintptr_t)(p4a8 + 0x228);
                            if (p228 >= 0x10000 && p228 < 0x7FFFF000U)
                                p24 = *(volatile uint32_t *)(uintptr_t)(p228 + 0x24);
                        }
                    }
                }
                int axis = (action == 4);
                int log_this = !axis || hwbps[i].hit_count <= 12;
                if (log_this) {
                    serial_puts("[EIE] hit=");
                    serial_putdec(hwbps[i].hit_count);
                    serial_puts(" this=0x");
                    serial_puthex((uint64_t)(uint32_t)frame->rcx, 8);
                    serial_puts(" ret=0x");
                    serial_puthex(ret, 8);
                    serial_puts(" vp=0x");
                    serial_puthex(vp, 8);
                    serial_puts(" key=0x");
                    serial_puthex(key, 4);
                    serial_puts(" action=");
                    serial_putdec(action);
                    serial_puts(" aux=0x");
                    serial_puthex(aux, 8);
                    serial_puts(" actor=0x");
                    serial_puthex(actor, 8);
                    serial_puts(" vp34=0x");
                    serial_puthex(vp34, 8);
                    serial_puts(" p4a8=0x");
                    serial_puthex(p4a8, 8);
                    serial_puts(" p228=0x");
                    serial_puthex(p228, 8);
                    serial_puts(" p24=0x");
                    serial_puthex(p24, 8);
                    serial_puts("\n");
                }
                hwbp_last_eie.seq = hwbps[i].hit_count;
                hwbp_last_eie.thisp = (uint32_t)frame->rcx;
                hwbp_last_eie.ret = ret;
                hwbp_last_eie.vp = vp;
                hwbp_last_eie.key = key;
                hwbp_last_eie.action = action;
                hwbp_last_eie.aux = aux;
                if (log_this && ret >= 0x10000000U && ret < 0x80000000U)
                    hwbp_set(2, ret, HWBP_EXECUTE, HWBP_LEN_1, "eieret");
                handled = true;
                continue;
            }
        }

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
         * full saved value; truncate to 32 bits to read user memory.
         *
         * Suspicious-content filter: print the full detail block on
         * the first 4 fires (warm-up sanity), and after that only when
         * the buffer at [esp+0] decodes to a suspicious wide string —
         * a 1- or 2-char numeric, an empty string, or one starting
         * with a literal '.' (the " .GameEngine" cascade pattern).
         * Everything else is plain banner/info chatter we don't care
         * about for this hunt. */
        /* Compact mode: print buffer contents on EVERY fire as a
         * one-liner. Cheap, ordered, and lets us correlate the
         * sprintf output history against the throw cascade lines. */
        {
            uint32_t esp32 = (uint32_t)frame->rsp;
            volatile uint32_t *st = (volatile uint32_t *)(uintptr_t)esp32;
            uint32_t buf_ptr = (uint32_t)st[0];
            uint32_t fmt_ptr = (uint32_t)st[2];
            /* EDI on EVERY hit (not just first-4 detail dump) — for the
             * GameEngine-Browse EDI-clobber bisect inside StaticConstructObject. */
            serial_puts("  edi=0x");
            serial_puthex((uint64_t)(uint32_t)frame->rdi, 8);
            serial_puts(" esp=0x");
            serial_puthex((uint64_t)(uint32_t)frame->rsp, 8);
            serial_puts(" ebp=0x");
            serial_puthex((uint64_t)(uint32_t)frame->rbp, 8);
            serial_puts(" buf=L\"");
            if (buf_ptr >= 0x10000 && (uint64_t)buf_ptr < 0x80000000ULL) {
                volatile uint16_t *w = (volatile uint16_t *)(uintptr_t)buf_ptr;
                char tmp[64]; int n = 0;
                for (int k = 0; k < 63; k++) {
                    uint16_t c = w[k];
                    if (c == 0) break;
                    tmp[n++] = (c < 0x20 || c >= 0x7F) ? '?' : (char)c;
                }
                tmp[n] = 0; serial_puts(tmp);
            }
            serial_puts("\" fmt=L\"");
            if (fmt_ptr >= 0x10000 && (uint64_t)fmt_ptr < 0x80000000ULL) {
                volatile uint16_t *w = (volatile uint16_t *)(uintptr_t)fmt_ptr;
                char tmp[64]; int n = 0;
                for (int k = 0; k < 63; k++) {
                    uint16_t c = w[k];
                    if (c == 0) break;
                    tmp[n++] = (c < 0x20 || c >= 0x7F) ? '?' : (char)c;
                }
                tmp[n] = 0; serial_puts(tmp);
            }
            serial_puts("\"");
            /* Render the stack args that are TCHAR* names. Different Core.dll
             * functions put the name at different arg slots:
             *   StaticFindObject(Class, Outer, Name, Exact) → st[3]=Name
             *   CreatePackage(Outer, Name)                  → st[2]=Name
             * Try st[1..3] and print each that decodes to a plausible wide
             * string, labeled argN. Catches the package name in CreatePackage
             * and the lookup name in StaticFindObject (the Mac None/None0
             * frontier). */
            for (int ai = 1; ai <= 3; ai++) {
                uint32_t name_ptr = (uint32_t)st[ai];
                if (name_ptr < 0x10000 || (uint64_t)name_ptr >= 0x80000000ULL)
                    continue;
                volatile uint16_t *w = (volatile uint16_t *)(uintptr_t)name_ptr;
                uint16_t c0 = w[0];
                if (c0 < 0x20 || c0 >= 0x7F) continue;   /* not a printable wstr */
                /* Require the 2nd unit to also be ASCII-ish or NUL to avoid
                 * mis-rendering pointers as 1-char strings. */
                uint16_t c1 = w[1];
                if (c1 != 0 && (c1 < 0x20 || c1 >= 0x7F)) continue;
                serial_puts(" arg"); serial_putdec((uint64_t)ai);
                serial_puts("=L\"");
                char tmp[64]; int n = 0;
                for (int k = 0; k < 63; k++) {
                    uint16_t c = w[k];
                    if (c == 0) break;
                    tmp[n++] = (c < 0x20 || c >= 0x7F) ? '?' : (char)c;
                }
                tmp[n] = 0; serial_puts(tmp);
                serial_puts("\"");
            }
            /* At the StaticConstructObject ClassConstructor call (Core.dll
             * 0x1015D40C), EDI=the UClass. Dump the ctor field region
             * [EDI+0x4D8..0x4EC] + the class Name (FName idx at EDI+0x20) so
             * we can see why [EDI+0x4E4] (ClassConstructor) is garbage (0x09)
             * for the WindowsClient class vs valid for working classes. */
            if (dr_get_addr(i) == 0x1015D40CULL) {
                uint32_t cls = (uint32_t)frame->rdi;
                if (cls >= 0x01000000 && cls < 0x60000000) {
                    serial_puts(" CLASS@0x"); serial_puthex(cls, 8);
                    serial_puts(" Name=");
                    serial_putdec((uint64_t)*(volatile uint32_t *)(uintptr_t)(cls + 0x20));
                    serial_puts(" ctorfld[+4D8..4EC]=");
                    for (uint32_t off = 0x4D8; off <= 0x4EC; off += 4) {
                        serial_puts("0x");
                        serial_puthex((uint64_t)*(volatile uint32_t *)(uintptr_t)(cls + off), 8);
                        serial_puts(" ");
                    }
                }
            }
            serial_puts("\n");
        }

        /* Slot 3 (ProcessRegistrants Phase 2 ConditionalRegister probe) logs
         * compact ECX+ESI+UObject-dump for hits 197..205 — captures the
         * entries closest to the throw point. */
        if (i == 3 && hwbps[i].hit_count >= 197 && hwbps[i].hit_count <= 205) {
            uint32_t ecx32 = (uint32_t)frame->rcx;
            uint32_t esi32 = (uint32_t)frame->rsi;
            serial_puts("  [slot3-late] ecx=0x");
            serial_puthex(ecx32, 8);
            serial_puts(" esi=");
            serial_putdec((uint64_t)esi32);
            /* Dump first 64 bytes of the UClass at ECX, then try to render
             * each pointer-shaped value as a string from .rdata. */
            serial_puts(" bytes=");
            volatile uint32_t *u = (volatile uint32_t *)(uintptr_t)ecx32;
            for (int k = 0; k < 16; k++) {
                serial_puts("0x");
                serial_puthex((uint64_t)u[k], 8);
                serial_puts(" ");
            }
            serial_puts("\n");
            /* For each pointer-looking field, try to render as ASCII string. */
            for (int k = 0; k < 16; k++) {
                uint32_t v = u[k];
                if (v < 0x10000000 || v >= 0x12000000) continue;
                volatile char *s = (volatile char *)(uintptr_t)v;
                /* Check for plausible ASCII string: first chars printable. */
                int ok = 1;
                for (int c = 0; c < 4; c++) {
                    char ch = s[c];
                    if (ch < 0x20 || ch >= 0x7F) { ok = 0; break; }
                }
                if (!ok) continue;
                serial_puts("    field+0x");
                serial_puthex((uint64_t)(k * 4), 2);
                serial_puts(" -> \"");
                char tmp[40]; int n = 0;
                for (int c = 0; c < 39 && s[c]; c++) {
                    char ch = s[c];
                    tmp[n++] = (ch < 0x20 || ch >= 0x7F) ? '?' : ch;
                }
                tmp[n] = 0;
                serial_puts(tmp);
                serial_puts("\"\n");
            }
        }
        if (hwbps[i].hit_count <= 4) {
            uint32_t esp32 = (uint32_t)frame->rsp;
            uint32_t ecx32 = (uint32_t)frame->rcx;
            uint32_t edx32 = (uint32_t)frame->rdx;
            uint32_t esi32 = (uint32_t)frame->rsi;
            uint32_t edi32 = (uint32_t)frame->rdi;
            uint32_t ebx32 = (uint32_t)frame->rbx;
            uint32_t eax32 = (uint32_t)frame->rax;
            serial_puts("  esp=0x"); serial_puthex(esp32, 8);
            serial_puts(" eax=0x"); serial_puthex(eax32, 8);
            serial_puts(" ecx=0x"); serial_puthex(ecx32, 8);
            serial_puts(" edx=0x"); serial_puthex(edx32, 8);
            serial_puts(" ebx=0x"); serial_puthex(ebx32, 8);
            serial_puts(" esi=0x"); serial_puthex(esi32, 8);
            serial_puts(" edi=0x"); serial_puthex(edi32, 8);
            serial_puts("\n");
            /* Try to render ESI (rep movs source) and EDI as wide
             * strings — common rep-movsb hot path produces a wstring
             * the caller is about to use. */
            hwbp_try_wstr("ESI",  esi32);
            hwbp_try_wstr("EDI",  edi32);

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
        /* FMW-specific: dump EVERY fire (not just first 4) but
         * filter to MISMATCH cases only — fires where the
         * assertion would have failed if not patched. Pool->Next->Prev
         * is at [ECX+0x1c] (per disasm: mov edx, [ecx+0x1c]; cmp edx,
         * [ebp-0x18]). We check the same condition here. Only log
         * when EDX != [ebp-0x18] — meaning the assertion would have
         * failed if not patched. */
        {
            const char *nm = hwbps[i].name;
            int is_fmw = (nm[0]=='F' && nm[1]=='M' && nm[2]=='W' && nm[3]=='-');
            if (is_fmw) {
                uint32_t ebp32 = (uint32_t)frame->rbp;
                uint32_t edx32 = (uint32_t)frame->rdx;
                uint32_t pool_cursor_va = ebp32 - 0x18;
                int cursor_ok = (pool_cursor_va >= 0x10000 &&
                                 (uint64_t)pool_cursor_va < 0x80000000ULL);
                uint32_t cursor_val = cursor_ok ?
                    *(volatile uint32_t *)(uintptr_t)pool_cursor_va : 0;
                /* Site-specific assertion check. Strings decoded from
                 * UT.exe .data:
                 *   site 0 (0x109032A8, line 367): "Pool->PrevLink==PoolPtr"
                 *     → fails when EDX != cursor_val
                 *   site 1 (0x10903303, line 370): "Free->Blocks>0"
                 *     → fails when EDX (Pool->[+0x4]) is 0 (ja = pass when
                 *       unsigned greater than 0; fail when == 0)
                 *   other FMW-* sites default to site-0 filter. */
                int site_idx = -1;
                int is_write_probe = 0;
                if (nm[4]=='s' && nm[5]=='i' && nm[6]=='t' && nm[7]=='e') {
                    if (nm[8]=='0') site_idx = 0;
                    else if (nm[8]=='1') site_idx = 1;
                } else {
                    /* FMW-Table*, FMW-pool* — WRITE probes, log every hit */
                    is_write_probe = 1;
                }
                int mismatch;
                if (is_write_probe) {
                    /* For WRITE probes, "mismatch" means "log every hit" —
                     * we want full timeline of mutations, not assert-fail
                     * filtering. */
                    mismatch = 1;
                } else if (site_idx == 1) {
                    /* Free->Blocks > 0 — assertion fails when EDX is 0 */
                    mismatch = (edx32 == 0);
                } else {
                    /* site 0 (and default): cursor mismatch */
                    mismatch = (edx32 != cursor_val);
                }
                /* FMW-INLINE-REPAIR — when site 0 detects PrevLink
                 * mismatch, repair the pool's PrevLink AT THIS POINT
                 * so the engine's walk sees consistent state. The
                 * je→jmp patch in compat32.c handles the je outcome
                 * (always takes the "pass" path), but downstream code
                 * may still access pool->PrevLink and expect it valid.
                 *
                 * cursor_addr = [ebp-0x18] (the cursor variable's value)
                 * ecx = pool (the pool whose PrevLink we just checked)
                 * Write pool->PrevLink = cursor_addr.
                 *
                 * Only do this for site 0 (the PrevLink check). Other
                 * mismatch types (site 1 Free->Blocks) don't have a
                 * simple repair. */
                if (mismatch && site_idx == 0) {
                    uint32_t pool_ptr = (uint32_t)frame->rcx;
                    if (pool_ptr >= 0x40000000 && pool_ptr < 0x80000000ULL &&
                        cursor_val >= 0x10000000) {
                        *(volatile uint32_t *)(uintptr_t)(pool_ptr + 0x1c) = cursor_val;
                        static uint32_t inline_repairs = 0;
                        inline_repairs++;
                        if (inline_repairs <= 20 || (inline_repairs % 50 == 0)) {
                            serial_puts("[FMW-INLINE-REPAIR] pool@0x");
                            serial_puthex((uint64_t)pool_ptr, 8);
                            serial_puts(" PrevLink: 0x");
                            serial_puthex((uint64_t)edx32, 8);
                            serial_puts(" → 0x");
                            serial_puthex((uint64_t)cursor_val, 8);
                            serial_puts(" (cum=");
                            serial_putdec((uint64_t)inline_repairs);
                            serial_puts(")\n");
                        }
                    }
                }
                if (mismatch) {
                    serial_puts("[FMW-MISMATCH] slot=");
                    serial_putdec((uint64_t)i);
                    serial_puts(" hit=");
                    serial_putdec((uint64_t)hwbps[i].hit_count);
                    serial_puts(" EDX=0x"); serial_puthex(edx32, 8);
                    serial_puts(" cursor=*[ebp-0x18]=0x"); serial_puthex(cursor_val, 8);
                    serial_puts(" ebp=0x"); serial_puthex(ebp32, 8);
                    serial_puts(" ecx=0x"); serial_puthex((uint64_t)frame->rcx, 8);
                    serial_puts("\n");
                    /* Dump the pool struct that triggered the mismatch
                     * AND walk the full Table->FirstPool list to see
                     * the broader corruption pattern. */
                    uint32_t pool_ptr = (uint32_t)frame->rcx;
                    if (pool_ptr >= 0x10000 && (uint64_t)pool_ptr < 0x80000000ULL) {
                        serial_puts("  [FMW-MISMATCH] pool@0x");
                        serial_puthex(pool_ptr, 8);
                        serial_puts(":");
                        for (int off = 0; off <= 0x1c; off += 4) {
                            uint32_t v = *(volatile uint32_t *)(uintptr_t)(pool_ptr + (uint32_t)off);
                            serial_puts(" +"); serial_puthex((uint64_t)off, 2);
                            serial_puts("=0x"); serial_puthex(v, 8);
                        }
                        serial_puts("\n");
                    }
                    /* Walk Table->FirstPool list. cursor is &Table->FirstPool
                     * (we read it above as cursor_val). Walk = *cursor →
                     * pool->Next → pool->Next->Next → ... NULL.
                     * Dump first 8 nodes with their Next/PrevLink. */
                    if (cursor_val >= 0x10000 && (uint64_t)cursor_val < 0x80000000ULL) {
                        uint32_t head = *(volatile uint32_t *)(uintptr_t)cursor_val;
                        serial_puts("  [FMW-MISMATCH] walking Table->FirstPool chain (head=*cursor):\n");
                        for (int n = 0; n < 8; n++) {
                            if (head == 0) { serial_puts("    [end]\n"); break; }
                            if (head < 0x10000 || (uint64_t)head >= 0x80000000ULL) {
                                serial_puts("    [bad ptr 0x"); serial_puthex(head, 8); serial_puts("]\n"); break;
                            }
                            uint32_t nxt = *(volatile uint32_t *)(uintptr_t)(head + 0x18);
                            uint32_t prv = *(volatile uint32_t *)(uintptr_t)(head + 0x1c);
                            serial_puts("    pool@0x"); serial_puthex(head, 8);
                            serial_puts(" Next=0x"); serial_puthex(nxt, 8);
                            serial_puts(" PrevLink=0x"); serial_puthex(prv, 8);
                            serial_puts("\n");
                            head = nxt;
                        }
                    }
                }
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
