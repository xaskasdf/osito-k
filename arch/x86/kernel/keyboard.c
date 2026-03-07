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
extern void serial_puthex(uint64_t val, int digits);

/* ── PS/2 ports ──────────────────────────────────────────────── */

#define KB_DATA_PORT    0x60
#define KB_STATUS_PORT  0x64
#define KB_CMD_PORT     0x64

#define KB_STATUS_OBF   0x01  /* Output buffer full (data ready) */

/* ── Ring buffer ─────────────────────────────────────────────── */

#define KB_BUF_SIZE  64

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

static void kb_push(char c)
{
    uint32_t next = (kb_head + 1) % KB_BUF_SIZE;
    if (next != kb_tail) {  /* Not full */
        kb_buf[kb_head] = c;
        kb_head = next;
    }
}

/* ── IRQ 1 handler (called from IDT vector 33) ──────────────── */

void keyboard_irq(void)
{
    uint8_t sc = inb(KB_DATA_PORT);

    /* Handle modifier keys */
    switch (sc) {
    case SC_LSHIFT_PRESS:
    case SC_RSHIFT_PRESS:
        kb_shift = true;
        return;
    case SC_LSHIFT_RELEASE:
    case SC_RSHIFT_RELEASE:
        kb_shift = false;
        return;
    case SC_CTRL_PRESS:
        kb_ctrl = true;
        return;
    case SC_CTRL_RELEASE:
        kb_ctrl = false;
        return;
    case SC_ALT_PRESS:
        kb_alt = true;
        return;
    case SC_ALT_RELEASE:
        kb_alt = false;
        return;
    case SC_CAPS_PRESS:
        kb_caps = !kb_caps;
        return;
    }

    /* Ignore key releases (bit 7 set) */
    if (sc & 0x80) return;

    /* Ignore extended scancodes (0xE0 prefix) for now */
    if (sc == 0xE0) return;

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

    /* Ctrl+C → ASCII 3 (ETX) */
    if (kb_ctrl && (c == 'c' || c == 'C')) {
        kb_push(3);
        return;
    }

    /* Ctrl+D → ASCII 4 (EOT) */
    if (kb_ctrl && (c == 'd' || c == 'D')) {
        kb_push(4);
        return;
    }

    if (c) kb_push(c);
}

/* ── Public API ──────────────────────────────────────────────── */

/* Read one character (blocking) */
char kb_getchar(void)
{
    while (kb_head == kb_tail)
        __asm__ volatile ("hlt");  /* Wait for IRQ */

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
