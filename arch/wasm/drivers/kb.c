/*
 * OsitoK WASM — Keyboard driver
 *
 * Replaces arch/x86/kernel/keyboard.c (PS/2 + IRQ 1).
 * Input comes from a JS keydown event ring buffer.
 * kb_getchar() blocks via emscripten_sleep() until a key arrives.
 */

#include <stdint.h>
#include <stdbool.h>
#include <emscripten.h>

/* ── Ring buffer (filled from JS) ────────────────────────────── */

#define KB_BUF_SIZE 256

static char     kb_buf[KB_BUF_SIZE];
static volatile uint32_t kb_head;   /* JS writes here */
static volatile uint32_t kb_tail;   /* C reads here */

/* Called from JS: wasm_kb_push(charCode) */
EMSCRIPTEN_KEEPALIVE
void wasm_kb_push(int c)
{
    uint32_t next = (kb_head + 1) % KB_BUF_SIZE;
    if (next != kb_tail) {
        kb_buf[kb_head] = (char)c;
        kb_head = next;
    }
}

/* ── Public keyboard API ─────────────────────────────────────── */

void kb_init(void)
{
    /* JS keydown listener is set up in shell.html */
}

bool kb_has_input(void)
{
    return kb_head != kb_tail;
}

char kb_trygetchar(void)
{
    if (!kb_has_input()) return 0;
    char c = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return c;
}

char kb_getchar(void)
{
    while (!kb_has_input())
        emscripten_sleep(10);   /* yield to JS event loop — requires ASYNCIFY */
    return kb_trygetchar();
}

/* Modifier states — always false in WASM */
bool kb_ctrl_held(void)  { return false; }
bool kb_shift_held(void) { return false; }
