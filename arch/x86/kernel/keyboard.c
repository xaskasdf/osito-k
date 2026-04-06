/*
 * OsitoK x86-64 — PS/2 Keyboard Driver
 *
 * X-OS8: Scancode set 1 → ASCII translation, ring buffer, IRQ 1.
 * Uses the 8042 PS/2 controller (standard in QEMU q35).
 * IRQ 1 → IDT vector 33 (32 + IRQ number).
 */

#include "../include/types.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);

/* xHCI USB polling (weak: works without xHCI driver) */
extern void xhci_poll(void) __attribute__((weak));

/* Compositor state (weak: absent if compositor not compiled) */
extern bool compositor_is_running(void) __attribute__((weak));

/* ── PS/2 ports ──────────────────────────────────────────────── */

#define KB_DATA_PORT    0x60
#define KB_STATUS_PORT  0x64
#define KB_CMD_PORT     0x64

#define KB_STATUS_OBF   0x01  /* Output buffer full (data ready) */

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

static void kb_process_scancode(uint8_t sc)
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
    if (sc & 0x80) return;

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

/* ── Keyboard capture flag ────────────────────────────────────
 * When a graphical process (Q2, game) has focus, keyboard events
 * go only to the input_events queue (SYS_GET_INPUT_EVENT), not to
 * kb_buf. This prevents shell keystrokes from leaking into games.
 * Set via kbd_set_captured(true) when process creates SHM surface,
 * cleared via kbd_set_captured(false) on process exit. */

static volatile bool g_keyboard_captured = false;

void kbd_set_captured(bool captured) { g_keyboard_captured = captured; }
bool kbd_is_captured(void)           { return g_keyboard_captured; }

/* ── IRQ 1 handler (called from IDT vector 33) ──────────────── */

void keyboard_irq(void)
{
    uint8_t sc = inb(KB_DATA_PORT);
    /* Post to input event system for games / compositor.
     * Skip 0xE0 prefix byte (extended scancode marker). */
    if (sc != 0xE0) {
        extern void input_post_key(uint8_t scancode, bool pressed, bool extended);
        input_post_key(sc & 0x7F, !(sc & 0x80), false);
    }
    /* Process PS/2 scancode → ASCII → kb_buf for the shell.
     * Skip when compositor is running (compositor handles HID→kb_push routing
     * from the input_events ring to avoid double input with PS/2+USB).
     * Also skip when a graphical process has captured the keyboard. */
    if (!g_keyboard_captured &&
        !(compositor_is_running && compositor_is_running()))
        kb_process_scancode(sc);
}

/* ── Inject scancode from compositor (no I/O port read) ──────── */

void kb_inject_scancode(uint8_t sc)
{
    kb_process_scancode(sc);
}

/* ── Public API ──────────────────────────────────────────────── */

/* Read one character (blocking) */
char kb_getchar(void)
{
    while (kb_head == kb_tail) {
        /* Only poll USB directly when compositor isn't running.
         * Concurrent xhci_poll() between compositor and shell threads
         * corrupts USB endpoint state → kb_push() never fires.
         * When compositor runs, it calls xhci_poll() every frame and
         * pushes chars via kb_push(); shell just waits for HLT to wake. */
        if (!(compositor_is_running && compositor_is_running())) {
            if (xhci_poll) xhci_poll();
        }
        if (kb_head != kb_tail) break;
        __asm__ volatile ("hlt");  /* Wait for IRQ or next timer tick */
    }

    char c = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return c;
}

/* Read one character (non-blocking, returns 0 if empty) */
char kb_trygetchar(void)
{
    if (kb_head == kb_tail) return 0;
    char c = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return c;
}

/* Check if input is available */
bool kb_has_input(void)
{
    return kb_head != kb_tail;
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

    /* Flush any pending data */
    while (inb(KB_STATUS_PORT) & KB_STATUS_OBF)
        inb(KB_DATA_PORT);

    /* Enable keyboard IRQ (unmask IRQ 1 in PIC) */
    uint8_t mask = inb(0x21);
    mask &= ~(1 << 1);  /* Clear bit 1 = unmask IRQ 1 */
    outb(0x21, mask);

    serial_puts("[KB] PS/2 keyboard ready (IRQ 1 → vector 0x71)\n");
}
