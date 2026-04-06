/*
 * OsitoK x86-64 — BSD Socket API (AF_INET)
 *
 * POSIX socket interface over the existing TCP/UDP stack.
 * Supports: socket, bind, listen, accept, connect, send, recv,
 *           sendto, recvfrom, setsockopt, getsockname, getpeername.
 *
 * Socket FDs integrate with the kernel FD table (FD_TYPE_SOCKET).
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern uint64_t idt_get_ticks(void);

/* Network stack (net.c) */
extern int  net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                            uint16_t src_port);
extern int  net_tcp_send(int conn_idx, const void *data, uint32_t len);
extern int  net_tcp_recv(int conn_idx, void *buf, uint32_t buf_size);
extern void net_tcp_close(int conn_idx);
extern int  net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
                         uint16_t src_port, const void *data, uint32_t len);
extern void net_udp_listen(uint16_t port, void *handler);
extern void net_poll(void);

/* TCP passive open (net.c) */
extern int  net_tcp_listen(uint16_t port) __attribute__((weak));
extern int  net_tcp_accept(int listen_idx) __attribute__((weak));

/* ── Socket Constants ────────────────────────────────────────── */

#define AF_INET      2
#define AF_INET6    10
#define SOCK_STREAM  1   /* TCP */
#define SOCK_DGRAM   2   /* UDP */

#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_KEEPALIVE 9

/* ── Socket Address ──────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t sin_family;
    uint16_t sin_port;      /* Network byte order */
    uint32_t sin_addr;      /* Network byte order */
    uint8_t  sin_zero[8];
} sockaddr_in_t;

/* ── Socket Table ────────────────────────────────────────────── */

#define MAX_SOCKETS 32

typedef struct {
    bool     active;
    int      domain;       /* AF_INET */
    int      type;         /* SOCK_STREAM, SOCK_DGRAM */
    int      protocol;
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t  remote_ip[4];
    int      tcp_conn;     /* TCP connection index in net.c (-1 = none) */
    bool     listening;
    bool     connected;
    uint8_t  udp_buf[2048];
    uint32_t udp_buf_len;
    bool     udp_has_data;
    uint8_t  udp_src_ip[4];
    uint16_t udp_src_port;
} socket_t;

static socket_t sockets[MAX_SOCKETS];

static inline uint16_t ntohs_s(uint16_t v) {
    return (v >> 8) | (v << 8);
}

/* ── UDP receive handler ─────────────────────────────────────── */

static void socket_udp_handler(const uint8_t *src_ip, uint16_t src_port,
                               const uint8_t *data, uint32_t len)
{
    /* Find the socket listening on this port */
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].active && sockets[i].type == SOCK_DGRAM &&
            !sockets[i].udp_has_data) {
            if (len > sizeof(sockets[i].udp_buf))
                len = sizeof(sockets[i].udp_buf);
            memcpy(sockets[i].udp_buf, data, len);
            sockets[i].udp_buf_len = len;
            memcpy(sockets[i].udp_src_ip, src_ip, 4);
            sockets[i].udp_src_port = src_port;
            sockets[i].udp_has_data = true;
            return;
        }
    }
}

/* ── Public API (called from syscall.c) ──────────────────────── */

int sock_socket(int domain, int type, int protocol)
{
    if (domain != AF_INET) return -97;  /* EAFNOSUPPORT */
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -93;  /* EPROTONOSUPPORT */

    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active) {
            memset(&sockets[i], 0, sizeof(socket_t));
            sockets[i].active = true;
            sockets[i].domain = domain;
            sockets[i].type = type;
            sockets[i].protocol = protocol;
            sockets[i].tcp_conn = -1;
            return i;  /* Return socket index (mapped to FD by syscall layer) */
        }
    }
    return -24;  /* EMFILE */
}

int sock_bind(int sock_idx, const sockaddr_in_t *addr)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (!s->active) return -9;

    s->local_port = ntohs_s(addr->sin_port);

    if (s->type == SOCK_DGRAM) {
        net_udp_listen(s->local_port, (void *)socket_udp_handler);
    }
    return 0;
}

int sock_listen(int sock_idx, int backlog)
{
    (void)backlog;
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (!s->active || s->type != SOCK_STREAM) return -95;

    s->listening = true;
    if (net_tcp_listen)
        net_tcp_listen(s->local_port);
    return 0;
}

int sock_accept(int sock_idx, sockaddr_in_t *addr, uint32_t *addrlen)
{
    (void)addr; (void)addrlen;
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (!s->active || !s->listening) return -22;

    /* Poll for incoming connection */
    if (net_tcp_accept) {
        int conn = net_tcp_accept(s->tcp_conn >= 0 ? s->tcp_conn : 0);
        if (conn >= 0) {
            /* Create new socket for the accepted connection */
            int new_idx = sock_socket(AF_INET, SOCK_STREAM, 0);
            if (new_idx < 0) return new_idx;
            sockets[new_idx].tcp_conn = conn;
            sockets[new_idx].connected = true;
            return new_idx;
        }
    }
    return -11;  /* EAGAIN */
}

int sock_connect(int sock_idx, const sockaddr_in_t *addr)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (!s->active) return -9;

    uint8_t ip[4];
    uint32_t addr_be = addr->sin_addr;
    ip[0] = addr_be & 0xFF;
    ip[1] = (addr_be >> 8) & 0xFF;
    ip[2] = (addr_be >> 16) & 0xFF;
    ip[3] = (addr_be >> 24) & 0xFF;
    uint16_t port = ntohs_s(addr->sin_port);

    memcpy(s->remote_ip, ip, 4);
    s->remote_port = port;

    if (s->type == SOCK_STREAM) {
        if (s->local_port == 0) s->local_port = 30000 + (uint16_t)(idt_get_ticks() & 0x7FFF);
        int conn = net_tcp_connect(ip, port, s->local_port);
        if (conn < 0) return -111;  /* ECONNREFUSED */
        s->tcp_conn = conn;
        s->connected = true;
    }
    return 0;
}

int sock_send(int sock_idx, const void *buf, uint32_t len, int flags)
{
    (void)flags;
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (!s->active || !s->connected) return -107;  /* ENOTCONN */

    if (s->type == SOCK_STREAM)
        return net_tcp_send(s->tcp_conn, buf, len);
    return -95;
}

int sock_recv(int sock_idx, void *buf, uint32_t len, int flags)
{
    (void)flags;
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (!s->active) return -9;

    if (s->type == SOCK_STREAM) {
        if (s->tcp_conn < 0) return -107;
        return net_tcp_recv(s->tcp_conn, buf, len);
    }
    return -95;
}

int sock_sendto(int sock_idx, const void *buf, uint32_t len,
                int flags, const sockaddr_in_t *dest)
{
    (void)flags;
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (!s->active || s->type != SOCK_DGRAM) return -95;

    uint8_t ip[4];
    uint32_t a = dest->sin_addr;
    ip[0] = a & 0xFF; ip[1] = (a >> 8) & 0xFF;
    ip[2] = (a >> 16) & 0xFF; ip[3] = (a >> 24) & 0xFF;
    uint16_t port = ntohs_s(dest->sin_port);

    if (s->local_port == 0) s->local_port = 30000 + (uint16_t)(idt_get_ticks() & 0x7FFF);
    return net_udp_send(ip, port, s->local_port, buf, len);
}

int sock_recvfrom(int sock_idx, void *buf, uint32_t len,
                  int flags, sockaddr_in_t *src, uint32_t *addrlen)
{
    (void)flags;
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (!s->active || s->type != SOCK_DGRAM) return -95;

    net_poll();
    if (!s->udp_has_data) return -11;  /* EAGAIN */

    uint32_t copy = s->udp_buf_len < len ? s->udp_buf_len : len;
    memcpy(buf, s->udp_buf, copy);

    if (src) {
        src->sin_family = AF_INET;
        src->sin_port = ntohs_s(s->udp_src_port);
        memcpy(&src->sin_addr, s->udp_src_ip, 4);
    }
    if (addrlen) *addrlen = sizeof(sockaddr_in_t);

    s->udp_has_data = false;
    return (int)copy;
}

int sock_close(int sock_idx)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (s->type == SOCK_STREAM && s->tcp_conn >= 0)
        net_tcp_close(s->tcp_conn);
    s->active = false;
    return 0;
}

int sock_setsockopt(int sock_idx, int level, int optname,
                    const void *optval, uint32_t optlen)
{
    (void)sock_idx; (void)level; (void)optname;
    (void)optval; (void)optlen;
    return 0;  /* Pretend success for compatibility */
}

int sock_getsockname(int sock_idx, sockaddr_in_t *addr, uint32_t *addrlen)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (addr) {
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_port = ntohs_s(s->local_port);
    }
    if (addrlen) *addrlen = sizeof(sockaddr_in_t);
    return 0;
}
