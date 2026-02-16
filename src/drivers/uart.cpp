/*
 * OsitoK - UART0 driver
 *
 * TX: polled (write to FIFO, wait if full)
 * RX: interrupt-driven with 64-byte ring buffer
 *
 * The UART interrupt (INUM 5) is dispatched by os_exception_handler()
 * in timer_tick.c, which calls uart_isr_handler() defined here.
 */

#include "drivers/uart.h"
#include "kernel/task.h"
#include "kernel/sem.h"

extern "C" {

/* ====== RX ring buffer ====== */
static volatile uint8_t rx_buf[UART_RX_BUF_SIZE];
static volatile uint8_t rx_head = 0;  /* Write index (ISR writes here) */
static volatile uint8_t rx_tail = 0;  /* Read index (user reads from here) */

/* ====== UART RX interrupt handler ====== */

/*
 * uart_isr_handler - UART0 interrupt handler
 *
 * Called from os_exception_handler() when INUM_UART is pending.
 * Reads all available bytes from the FIFO into the ring buffer.
 *
 * Runs in exception context (IRAM required).
 */
void IRAM_ATTR uart_isr_handler(void)
{
    uint32_t status = UART0_INT_ST;

    /* RX FIFO full or timeout */
    if (status & (UART_RXFIFO_FULL_INT | UART_RXFIFO_TOUT_INT)) {
        /* Read all available bytes from FIFO */
        while (UART0_STATUS & UART_RXFIFO_CNT_MASK) {
            uint8_t byte = UART0_FIFO & 0xFF;

            /* Compute next write position */
            uint8_t next_head = (rx_head + 1) % UART_RX_BUF_SIZE;

            /* Drop byte if buffer full */
            if (next_head != rx_tail) {
                rx_buf[rx_head] = byte;
                rx_head = next_head;
            }
        }
    }

    /* Clear all UART interrupts */
    UART0_INT_CLR = 0xFFFFFFFF;
}

/* ====== UART output mutex ====== */

static mutex_t uart_mtx;

void uart_lock(void)
{
    mutex_lock(&uart_mtx);
}

void uart_unlock(void)
{
    mutex_unlock(&uart_mtx);
}

/* ====== Public API ====== */

void uart_init(void)
{
    /* UART0 hardware is already configured by nosdk_init (baud, 8N1, pins).
     * Here we set up the UART for RX interrupts.
     * The actual interrupt routing is handled by our custom vector table
     * and os_exception_handler() — no ROM ets_isr_attach needed. */

    /* Initialize output mutex */
    mutex_init(&uart_mtx);

    /* Clear pending interrupts */
    UART0_INT_CLR = 0xFFFFFFFF;

    /* Configure CONF1:
     *   - RX FIFO full threshold = 1 byte (bits 0-6)
     *   - RX timeout enable (bit 31)
     *   - RX timeout threshold = 10 (bits 24-30)
     */
    UART0_CONF1 = (1 << 0)          /* RXFIFO full threshold = 1 */
                | UART_RX_TOUT_EN    /* Enable RX timeout */
                | (10 << 24);        /* Timeout threshold = 10 bit-times */

    /* Enable RX interrupts at the UART peripheral level */
    UART0_INT_ENA = UART_RXFIFO_FULL_INT | UART_RXFIFO_TOUT_INT;

    /* INUM_UART is enabled in INTENABLE by timer_init() */

    uart_puts("uart: initialized (115200 8N1, RX interrupts enabled)\n");
}

void uart_putc(char c)
{
    /* Feed HW WDT to prevent reset during long output */
    REG32(0x60000914) = 0x73;

    /* Wait until TX FIFO has space (FIFO depth = 128 bytes) */
    while (((UART0_STATUS >> UART_TXFIFO_CNT_SHIFT) & UART_TXFIFO_CNT_MASK) >= 126)
        ;

    UART0_FIFO = (uint32_t)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

void uart_put_dec(uint32_t val)
{
    char buf[12];
    int i = 0;

    if (val == 0) {
        uart_putc('0');
        return;
    }

    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }

    /* Print in reverse */
    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

void uart_put_hex(uint32_t val)
{
    static const char hex[] = "0123456789abcdef";

    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(hex[(val >> i) & 0xF]);
    }
}

int uart_getc(void)
{
    if (rx_head == rx_tail) {
        return -1; /* Buffer empty */
    }

    uint32_t ps = irq_save();
    uint8_t byte = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % UART_RX_BUF_SIZE;
    irq_restore(ps);

    return byte;
}

bool uart_rx_available(void)
{
    return rx_head != rx_tail;
}

} /* extern "C" */
