/*
 * ncm.c -- Ethernet over the USB NCM function: framing, queues, the link
 *
 * dwc3_acm.c moves the USB transfers; this packs and unpacks their NTB16
 * transfer blocks (CDC NCM 1.0, section 3) and keeps the frames between
 * them and the network stack (kernel/net.c), whose NIC it is. Frames go
 * through two single-producer queues: the RX one filled under the console
 * lock (acm_poll), the TX one by the stack (held by its user).
 *
 * `net up` re-enumerates the gadget as ACM + NCM (usb_set_ncm). When the
 * host selects the data interface, the net task asks for an address by
 * DHCP: Windows' Internet Connection Sharing answers, which routes the
 * phone to the internet. With nobody answering it takes a link-local
 * 169.254.x.y, as Windows does after its own DHCP gives up, so the two
 * reach each other anyway. The task then polls the stack every 10 ms, so
 * the phone answers ARP and ping, unless a command holds it.
 *
 * Polling the gadget is what moves frames, so the NIC's receive polls it
 * when its queue is empty and its send after queueing: any user polling
 * the stack (ping's loop, DHCP) drives the USB side too.
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "../../kernel/net.h"
#include "usb.h"

#define NTH16_SIG       0x484D434E          /* "NCMH" */
#define NDP16_SIG       0x304D434E          /* "NCM0": no CRC */
#define FRAME_MAX       1536
#define RXQ             32
#define TXQ             32
#define TX_PER_NTB      8

static struct frame {
    uint16_t len;
    uint8_t  data[FRAME_MAX];
} rxq[RXQ], txq[TXQ];
static volatile uint32_t rx_head, rx_tail, tx_head, tx_tail;
static uint16_t tx_seq;
static volatile int link_up, link_changed;
static uint32_t rx_frames, tx_frames, rx_drops, tx_drops, bad_ntbs;
static int net_started, task_started, dhcp_ok;

/* The phone's own MAC; the host's is NCM_HOST_MAC_STR (iMACAddress) */
static const uint8_t dev_mac[6] = { 0x02, 0x4F, 0x4B, 0x00, 0x00, 0x02 };

static inline void barrier(void) { __asm__ volatile("dmb ish" : : : "memory"); }

static uint32_t le16(const volatile uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const volatile uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static void put16(volatile uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(volatile uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* ── From dwc3_acm.c, under the console lock ─────────────────── */

void ncm_rx_ntb(const volatile uint8_t *b, uint32_t n)
{
    if (n < 12 || le32(b) != NTH16_SIG || le16(b + 4) != 12) {
        bad_ntbs++;
        return;
    }
    uint32_t block = le16(b + 8) < n ? le16(b + 8) : n;
    uint32_t ndp = le16(b + 10);
    for (int chain = 0; ndp && ndp + 8 <= block && chain < 8; chain++) {
        if (le32(b + ndp) != NDP16_SIG) {
            bad_ntbs++;
            return;
        }
        uint32_t end = ndp + le16(b + ndp + 4);
        for (uint32_t e = ndp + 8; e + 4 <= end && e + 4 <= block; e += 4) {
            uint32_t at = le16(b + e), len = le16(b + e + 2);
            if (!at || !len)
                break;
            if (at + len > block || len > FRAME_MAX) {
                bad_ntbs++;
                continue;
            }
            uint32_t next = (rx_head + 1) % RXQ;
            if (next == rx_tail) {
                rx_drops++;
                continue;
            }
            struct frame *f = &rxq[rx_head];
            for (uint32_t i = 0; i < len; i++)
                f->data[i] = b[at + i];
            f->len = (uint16_t)len;
            barrier();
            rx_head = next;
            rx_frames++;
        }
        ndp = le16(b + ndp + 6);
    }
}

/* Up to TX_PER_NTB queued frames as one NTB16: header, one NDP, then the
 * datagrams 4-byte aligned. Never a multiple of the packet size, so the
 * transfer ends with a short packet. */
uint32_t ncm_tx_build(volatile uint8_t *b, uint32_t max)
{
    uint32_t n = 0, data = 0;
    for (uint32_t t = tx_tail; t != tx_head && n < TX_PER_NTB; t = (t + 1) % TXQ) {
        uint32_t need = (txq[t].len + 3u) & ~3u;
        if (12 + 8 + 4 * (n + 2) + data + need + 1 > max)
            break;
        data += need;
        n++;
    }
    if (!n)
        return 0;
    barrier();
    uint32_t ndp_len = 8 + 4 * (n + 1);
    uint32_t off = 12 + ndp_len;
    put32(b, NTH16_SIG);
    put16(b + 4, 12);
    put16(b + 6, tx_seq++);
    put16(b + 10, 12);
    put32(b + 12, NDP16_SIG);
    put16(b + 16, ndp_len);
    put16(b + 18, 0);
    for (uint32_t i = 0; i < n; i++) {
        struct frame *f = &txq[tx_tail];
        put16(b + 20 + 4 * i, off);
        put16(b + 22 + 4 * i, f->len);
        for (uint32_t j = 0; j < f->len; j++)
            b[off + j] = f->data[j];
        off = (off + f->len + 3u) & ~3u;
        tx_tail = (tx_tail + 1) % TXQ;
        tx_frames++;
    }
    put32(b + 20 + 4 * n, 0);
    if (off % 512 == 0)
        b[off++] = 0;
    put16(b + 8, off);
    return off;
}

void ncm_link(int up)
{
    if (up) {
        rx_head = rx_tail = tx_head = tx_tail = 0;
    }
    link_up = up;
    link_changed = 1;
}

/* ── The stack's NIC ─────────────────────────────────────────── */

static void service(void)
{
    uint64_t flags = console_lock();
    acm_poll();
    console_unlock(flags);
}

static void nic_get_mac(uint8_t mac[6])
{
    memcpy(mac, dev_mac, 6);
}

static int nic_send(const void *frame, uint32_t len)
{
    if (!link_up || len > FRAME_MAX)
        return -1;
    uint32_t next = (tx_head + 1) % TXQ;
    if (next == tx_tail)
        service();
    if (next == tx_tail) {
        tx_drops++;
        return -1;
    }
    struct frame *f = &txq[tx_head];
    memcpy(f->data, frame, len);
    f->len = (uint16_t)len;
    barrier();
    tx_head = next;
    service();
    return 0;
}

static int nic_recv(void *buf, uint32_t *len)
{
    if (rx_tail == rx_head)
        service();
    if (rx_tail == rx_head)
        return -1;
    barrier();
    struct frame *f = &rxq[rx_tail];
    memcpy(buf, f->data, f->len);
    *len = f->len;
    barrier();
    rx_tail = (rx_tail + 1) % RXQ;
    return 0;
}

static const net_nic_t ncm_nic = { nic_get_mac, nic_send, nic_recv };

/* ── Addressing and the poll task ────────────────────────────── */

extern int dhcp_run(void);

static void put_ip(const uint8_t ip[4])
{
    for (int i = 0; i < 4; i++) {
        serial_putdec(ip[i]);
        if (i < 3)
            serial_putc('.');
    }
}

/* DHCP, else a link-local address (no conflict probing) */
static void configure(void)
{
    net_hold();
    dhcp_ok = dhcp_run() == 0;
    if (!dhcp_ok) {
        uint8_t ip[4] = { 169, 254, 0x4F, (uint8_t)(2 + timer_ms() % 250) };
        static const uint8_t mask[4] = { 255, 255, 0, 0 };
        net_set_ip(ip, mask);
    }
    uint8_t ip[4], mask[4], gw[4];
    net_get_ip(ip, mask, gw);
    net_release();
    serial_puts("[NET ] usb0: ");
    put_ip(ip);
    serial_puts(dhcp_ok ? " (DHCP)\n" : " (link-local: no DHCP server; "
                "share the PC's connection to it for the internet)\n");
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (link_changed) {
            link_changed = 0;
            if (link_up)
                configure();
        }
        if (link_up)
            net_trypoll();
        task_delay_ms(10);
    }
}

static void net_up(void)
{
    if (!net_started) {
        static const uint8_t none[4] = { 0, 0, 0, 0 };
        net_set_nic(&ncm_nic);
        net_init(none);
        net_started = 1;
    }
    if (!task_started)
        task_started = task_create_on("net", net_task, (void *)0, 1, TASK_ANY_CPU) >= 0;
    serial_puts("net: re-enumerating as serial + network; the console comes back in a moment\n");
    usb_set_ncm(1);
}

/* Shell `net`: the link and the address; `net up` / `net down` */
void platform_net_cmd(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "up") == 0) {
        net_up();
        return;
    }
    if (argc == 2 && strcmp(argv[1], "down") == 0) {
        usb_set_ncm(0);
        serial_puts("net: serial port only\n");
        return;
    }
    if (argc == 3 && strcmp(argv[1], "dns") == 0) {
        uint8_t ip[4] = { 0 };
        const char *p = argv[2];
        for (int i = 0; i < 4 && *p; i++) {
            uint32_t v = 0;
            while (*p >= '0' && *p <= '9')
                v = v * 10 + (uint32_t)(*p++ - '0');
            ip[i] = (uint8_t)v;
            if (*p == '.')
                p++;
        }
        net_dns_set_server(ip);
    } else if (argc != 1) {
        serial_puts("usage: net [up | down | dns <ip>]\n");
        return;
    }
    static const char *const states[] = {
        "off (net up)", "waiting for the host", "configured, data interface off", "up",
    };
    serial_puts("usb0: ");
    serial_puts(states[usb_ncm_state()]);
    if (net_started) {
        uint8_t ip[4], mask[4], gw[4];
        net_get_ip(ip, mask, gw);
        serial_puts(", ");
        put_ip(ip);
        serial_puts(" mask ");
        put_ip(mask);
        if (dhcp_ok) {
            serial_puts(" gateway ");
            put_ip(gw);
        }
        uint8_t dns[4];
        net_dns_get_server(dns);
        serial_puts(" dns ");
        put_ip(dns);
    }
    serial_puts("\n  rx ");
    serial_putdec(rx_frames);
    serial_puts(" frames (");
    serial_putdec(rx_drops);
    serial_puts(" dropped, ");
    serial_putdec(bad_ntbs);
    serial_puts(" bad NTBs), tx ");
    serial_putdec(tx_frames);
    serial_puts(" (");
    serial_putdec(tx_drops);
    serial_puts(" dropped)\n");
}
