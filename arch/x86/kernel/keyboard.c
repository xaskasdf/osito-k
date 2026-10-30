/*
 * OsitoK x86-64 — PS/2 Keyboard Driver
 *
 * X-OS8: Scancode set 1 → ASCII translation, ring buffer, IRQ 1.
 * Uses the 8042 PS/2 controller (standard in QEMU q35).
 * IRQ 1 uses PIC vector 0x71; the shared receive path also drains AUX.
 */

#include "../include/types.h"
#include "../include/input_events.h"
#include "../drivers/ps2.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);

/* xHCI USB polling (weak: works without xHCI driver) */
extern void xhci_poll(void) __attribute__((weak));
extern void win32_post_keyboard_scan(uint16_t scan, int key_up)
    __attribute__((weak));

/* Compositor state (weak: absent if compositor not compiled) */
extern bool compositor_is_running(void) __attribute__((weak));

/* Avoid burning a runnable scheduler slot while the shell waits for input.
 * The weak declaration preserves the early-boot polling fallback. */
extern int sched_sleep_ticks(uint64_t ticks) __attribute__((weak));
extern bool sched_is_enabled(void) __attribute__((weak));
extern bool terminal_control_key(unsigned char key) __attribute__((weak));

/* ── Ring buffer ─────────────────────────────────────────────── */

#define KB_BUF_SIZE  256

static char     kb_buf[KB_BUF_SIZE];
static volatile uint32_t kb_head;   /* Write position */
static volatile uint32_t kb_tail;   /* Read position */

/* ── Modifier state ──────────────────────────────────────────── */

static bool kb_shift;
static bool kb_ctrl;
static bool kb_alt;
static bool kb_caps;

/* ── Scancode Set 1 → ASCII tables ───────────────────────────── */

static const char sc_normal[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,' ',0,  0,0,0,0,0,0,0,0,0,0,  /* F1-F10 */
    0,  0,  /* Num/Scroll Lock */
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
    0,0,0,  0,0,  /* F11, F12 */
};

static const char sc_shifted[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0,' ',0,  0,0,0,0,0,0,0,0,0,0,
    0,  0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
    0,0,0,  0,0,
};

/* Scancodes for modifier keys */
#define SC_LSHIFT_PRESS   0x2A
#define SC_RSHIFT_PRESS   0x36
#define SC_LSHIFT_RELEASE 0xAA
#define SC_RSHIFT_RELEASE 0xB6
#define SC_CTRL_PRESS     0x1D
#define SC_CTRL_RELEASE   0x9D
#define SC_ALT_PRESS      0x38
#define SC_ALT_RELEASE    0xB8
#define SC_CAPS_PRESS     0x3A
#define SC_UP             0x48
#define SC_DOWN            0x50
#define SC_LEFT            0x4B
#define SC_RIGHT           0x4D

/* ── Push character into ring buffer ─────────────────────────── */

void kb_push(char c)
{
    if (terminal_control_key && terminal_control_key((unsigned char)c)) return;
    uint32_t next = (kb_head + 1) % KB_BUF_SIZE;
    if (next != kb_tail) {  /* Not full */
        kb_buf[kb_head] = c;
        /* MFENCE: (1) "memory" clobber = compiler barrier (ensures buf write is
         * not reordered past head advance by GCC optimizer); (2) MFENCE opcode =
         * hardware full fence, translated by QEMU TCG to ARM DMB ISH, which
         * gives cross-vCPU store visibility on Apple Silicon MTTCG host. */
        __asm__ volatile ("mfence" ::: "memory");
        kb_head = next;
    }
}

/* ── Extended scancode state ──────────────────────────────────── */

static bool kb_extended;   /* true after 0xE0 prefix byte */

/* Push VT100 escape sequence into ring buffer: ESC [ <suffix> */
void kb_push_esc(const char *seq)
{
    kb_push(27);   /* ESC */
    kb_push('[');
    while (*seq) kb_push(*seq++);
}

/* ── Core scancode processing (shared by IRQ and inject) ─────── */

static void kb_process_scancode(uint8_t sc, bool emit)
{
    /* Extended scancode prefix — set flag and wait for next byte */
    if (sc == 0xE0) {
        kb_extended = true;
        return;
    }

    /* Handle extended key releases (0xE0 + 0x80|scancode) */
    if (kb_extended && (sc & 0x80)) {
        /* Extended key release — handle Ctrl release via 0xE0 prefix */
        if (sc == SC_CTRL_RELEASE) kb_ctrl = false;
        kb_extended = false;
        return;
    }

    /* Handle extended key presses */
    if (kb_extended) {
        kb_extended = false;
        /* Extended modifier keys */
        if (sc == SC_CTRL_PRESS) { kb_ctrl = true; return; }

        if (!emit) return;

        switch (sc) {
        case 0x48: kb_push_esc("A");  return;  /* Up */
        case 0x50: kb_push_esc("B");  return;  /* Down */
        case 0x4D: kb_push_esc("C");  return;  /* Right */
        case 0x4B: kb_push_esc("D");  return;  /* Left */
        case 0x47: kb_push_esc("H");  return;  /* Home */
        case 0x4F: kb_push_esc("F");  return;  /* End */
        case 0x52: kb_push_esc("2~"); return;  /* Insert */
        case 0x53: kb_push_esc("3~"); return;  /* Delete */
        case 0x49: kb_push_esc("5~"); return;  /* Page Up */
        case 0x51: kb_push_esc("6~"); return;  /* Page Down */
        }
        return; /* Unknown extended key — ignore */
    }

    /* Handle modifier keys */
    switch (sc) {
    case SC_LSHIFT_PRESS:
    case SC_RSHIFT_PRESS:
        kb_shift = true;
        break;
    case SC_LSHIFT_RELEASE:
    case SC_RSHIFT_RELEASE:
        kb_shift = false;
        break;
    case SC_CTRL_PRESS:
        kb_ctrl = true;
        break;
    case SC_CTRL_RELEASE:
        kb_ctrl = false;
        break;
    case SC_ALT_PRESS:
        kb_alt = true;
        break;
    case SC_ALT_RELEASE:
        kb_alt = false;
        break;
    case SC_CAPS_PRESS:
        kb_caps = !kb_caps;
        break;
    }

    /* Ignore key releases for the ASCII buffer (bit 7 set) */
    if (!emit || (sc & 0x80)) return;

    /* Translate scancode to ASCII */
    char c;
    if (kb_shift)
        c = sc_shifted[sc & 0x7F];
    else
        c = sc_normal[sc & 0x7F];

    /* Apply caps lock to letters */
    if (kb_caps && c >= 'a' && c <= 'z')
        c = c - 'a' + 'A';
    else if (kb_caps && c >= 'A' && c <= 'Z')
        c = c - 'A' + 'a';

    /* Ctrl+letter: ASCII 1-26 */
    if (kb_ctrl && c >= 'a' && c <= 'z') {
        kb_push(c - 'a' + 1);
        return;
    }
    if (kb_ctrl && c >= 'A' && c <= 'Z') {
        kb_push(c - 'A' + 1);
        return;
    }

    if (c) kb_push(c);
}

/* Set-1 is a transport encoding, not the HID usage namespace consumed by
 * the compositor and native applications. Keep prefix state independent of
 * the console injector, which may run on another input path. */
static bool kb_hw_extended;
static uint8_t kb_hw_pause;

static const uint8_t set1_hid[128] = {
    [0x01]=0x29, [0x02]=0x1E, [0x03]=0x1F, [0x04]=0x20, [0x05]=0x21,
    [0x06]=0x22, [0x07]=0x23, [0x08]=0x24, [0x09]=0x25, [0x0A]=0x26,
    [0x0B]=0x27, [0x0C]=0x2D, [0x0D]=0x2E, [0x0E]=0x2A, [0x0F]=0x2B,
    [0x10]=0x14, [0x11]=0x1A, [0x12]=0x08, [0x13]=0x15, [0x14]=0x17,
    [0x15]=0x1C, [0x16]=0x18, [0x17]=0x0C, [0x18]=0x12, [0x19]=0x13,
    [0x1A]=0x2F, [0x1B]=0x30, [0x1C]=0x28, [0x1D]=0xE0, [0x1E]=0x04,
    [0x1F]=0x16, [0x20]=0x07, [0x21]=0x09, [0x22]=0x0A, [0x23]=0x0B,
    [0x24]=0x0D, [0x25]=0x0E, [0x26]=0x0F, [0x27]=0x33, [0x28]=0x34,
    [0x29]=0x35, [0x2A]=0xE1, [0x2B]=0x31, [0x2C]=0x1D, [0x2D]=0x1B,
    [0x2E]=0x06, [0x2F]=0x19, [0x30]=0x05, [0x31]=0x11, [0x32]=0x10,
    [0x33]=0x36, [0x34]=0x37, [0x35]=0x38, [0x36]=0xE5, [0x37]=0x55,
    [0x38]=0xE2, [0x39]=0x2C, [0x3A]=0x39, [0x3B]=0x3A, [0x3C]=0x3B,
    [0x3D]=0x3C, [0x3E]=0x3D, [0x3F]=0x3E, [0x40]=0x3F, [0x41]=0x40,
    [0x42]=0x41, [0x43]=0x42, [0x44]=0x43, [0x45]=0x53, [0x46]=0x47,
    [0x47]=0x5F, [0x48]=0x60, [0x49]=0x61, [0x4A]=0x56, [0x4B]=0x5C,
    [0x4C]=0x5D, [0x4D]=0x5E, [0x4E]=0x57, [0x4F]=0x59, [0x50]=0x5A,
    [0x51]=0x5B, [0x52]=0x62, [0x53]=0x63, [0x54]=0x46, [0x56]=0x64,
    [0x57]=0x44, [0x58]=0x45,
};

static const uint8_t set1_e0_hid[128] = {
    [0x1C]=0x58, [0x1D]=0xE4, [0x35]=0x54, [0x37]=0x46, [0x38]=0xE6,
    [0x46]=0x48, [0x47]=0x4A, [0x48]=0x52, [0x49]=0x4B, [0x4B]=0x50,
    [0x4D]=0x4F, [0x4F]=0x4D, [0x50]=0x51, [0x51]=0x4E, [0x52]=0x49,
    [0x53]=0x4C, [0x5B]=0xE3, [0x5C]=0xE7, [0x5D]=0x65,
};

static uint8_t kb_decode_hw(uint8_t sc, bool *extended, bool *pulse)
{
    static const uint8_t pause[] = {0xE1,0x1D,0x45,0xE1,0x9D,0xC5};
    *extended = false;
    *pulse = false;
    if (kb_hw_pause) {
        if (sc == pause[kb_hw_pause]) {
            if (++kb_hw_pause < sizeof(pause)) return 0;
            kb_hw_pause = 0;
            *pulse = true; /* Pause has no separate break sequence. */
            return 0x48;
        }
        kb_hw_pause = 0; /* A truncated sequence must not eat the next key. */
    }
    if (sc == 0xE1) {
        kb_hw_extended = false;
        kb_hw_pause = 1;
        return 0;
    }
    if (sc == 0xE0) {
        kb_hw_extended = true;
        return 0;
    }
    *extended = kb_hw_extended;
    kb_hw_extended = false;
    /* E0 fake Shift bytes around PrintScreen/navigation are deliberately
     * unmapped. ACK/RESEND/error bytes have no entry in either table. */
    return (*extended ? set1_e0_hid : set1_hid)[sc & 0x7F];
}

/* Physical keyboard bytes, after the i8042 source/status check. */

void kb_handle_hw_byte(uint8_t sc)
{
    bool captured = input_keyboard_post_set1(&sc, 1);
    bool extended, pulse;
    uint8_t usage = kb_decode_hw(sc, &extended, &pulse);
    if (!usage) return;

    bool emit = !captured &&
        !(compositor_is_running && compositor_is_running());
    if (usage != 0x48) {
        if (extended) kb_process_scancode(0xE0, emit);
        kb_process_scancode(sc, emit);
    }
    if (captured) return; /* DOS retains the untouched set-1 byte stream. */
    input_post_key(usage, pulse || !(sc & 0x80), extended);
    if (pulse) input_post_key(usage, false, false);
    /* Submit decoded transitions atomically; a second keyboard must not
     * inherit this transport's E0/E1 prefix state. DOS owns captured bytes. */
    if (win32_post_keyboard_scan) {
        uint16_t scan = pulse ? 0xE145 : (extended ? 0xE000 : 0) | (sc & 0x7F);
        win32_post_keyboard_scan(scan, pulse ? 0 : (sc & 0x80) != 0);
        if (pulse) win32_post_keyboard_scan(scan, 1);
    }
}

void keyboard_irq(void)
{
    ps2_irq();
}

/* ── Inject scancode from compositor (no I/O port read) ──────── */

void kb_inject_scancode(uint8_t sc)
{
    kb_process_scancode(sc, true);
}

/* ── Public API ──────────────────────────────────────────────── */

/* Only prompt readers opt into cancellation; normal input remains blocking. */
int kb_getchar_interruptible(bool (*cancel)(void *), void *context)
{
    bool restore_interrupts = false;
    if (kb_head == kb_tail && sched_is_enabled && sched_is_enabled()) {
        uint64_t flags;
        __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
        if (!(flags & (1ULL << 9))) {
            /* Interrupt-gate callers such as syscall and native DPMI enter
             * with IF clear. Let the scheduler and input producers run while
             * this blocking operation waits, then restore the caller state. */
            restore_interrupts = true;
            __asm__ volatile ("sti" ::: "memory");
        }
    }

    while (kb_head == kb_tail) {
        /* Only poll USB directly when compositor isn't running.
         * Concurrent xhci_poll() between compositor and shell threads
         * corrupts USB endpoint state → kb_push() never fires.
         * When compositor runs, it calls xhci_poll() every frame and
         * pushes chars via kb_push(); shell just waits to be woken. */
        if (!(compositor_is_running && compositor_is_running())) {
            ps2_poll();
            if (xhci_poll) xhci_poll();
        }
        if (kb_head != kb_tail) break;
        /* Serial console input (headless / CI / debugging): a byte on COM1 RX
         * is treated as a typed key. term_readline already maps \r and 0x7F. */
        {
            extern int serial_getc(void);
            int sc = serial_getc();
            if (sc >= 0) {
                if (terminal_control_key && terminal_control_key((unsigned char)sc))
                    continue;
                if (restore_interrupts)
                    __asm__ volatile ("cli" ::: "memory");
                return (unsigned char)sc;
            }
        }
        if (cancel && cancel(context)) {
            if (restore_interrupts)
                __asm__ volatile ("cli" ::: "memory");
            return -1;
        }
        /* COM1 RX interrupts are disabled, so wake periodically to poll. A
         * single 10 ms tick keeps console input responsive without issuing
         * hundreds of thousands of KVM port-I/O exits per second. */
        if (sched_sleep_ticks && sched_sleep_ticks(1) == 0)
            continue;
        /* Bare-metal safe: PAUSE instead of HLT.
         * HLT depends on the APIC timer or xHCI MSI/INTx waking us up.
         * Same pattern as commit 2832f23 (sched yield via INT $0x20):
         * on real HW with masked/misrouted IRQs, HLT never wakes and
         * xhci_poll() never gets re-entered → kernel "hangs" at prompt.
         * PAUSE is just a CPU hint — the loop keeps polling xhci_poll()
         * and PS/2 IRQs (still on a vector) can also fill kb_buf. */
        __asm__ volatile ("pause");
    }

    char c = kb_buf[kb_tail];
    kb_buf[kb_tail] = 0;
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    if (restore_interrupts)
        __asm__ volatile ("cli" ::: "memory");
    return (unsigned char)c;
}

char kb_getchar(void)
{
    return (char)kb_getchar_interruptible(NULL, NULL);
}

/* Read one character (non-blocking, returns 0 if empty) */
char kb_trygetchar(void)
{
    if (kb_head == kb_tail) return 0;
    char c = kb_buf[kb_tail];
    kb_buf[kb_tail] = 0;
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return c;
}

/* Check if input is available */
bool kb_has_input(void)
{
    return kb_head != kb_tail;
}

/* IBM BIOS INT 16h shift-state layout. The driver tracks Shift as a combined
 * state, so expose it as left Shift rather than claiming both keys are down. */
uint8_t kb_get_bios_shift_flags(void)
{
    uint8_t flags = 0;
    if (kb_shift) flags |= 0x02;
    if (kb_ctrl)  flags |= 0x04;
    if (kb_alt)   flags |= 0x08;
    if (kb_caps)  flags |= 0x40;
    return flags;
}

/* Flush pending keyboard input — call before exec'ing a new process
 * so stale shell keystrokes don't leak into the new program's stdin. */
void kbd_flush(void)
{
    kb_tail = kb_head;
}

/* Diagnostic getters (safe to call from any thread context) */

/* ── Initialize keyboard ────────────────────────────────────── */

void kb_init(void)
{
    serial_puts("[KB] Initializing PS/2 keyboard...\n");

    kb_head = 0;
    kb_tail = 0;
    kb_shift = false;
    kb_ctrl = false;
    kb_alt = false;
    kb_caps = false;

    kb_extended = false;
    kb_hw_extended = false;
    kb_hw_pause = 0;
    ps2_init();

    serial_puts("[KB] PS/2 keyboard ready (IRQ 1 → vector 0x71)\n");
}
