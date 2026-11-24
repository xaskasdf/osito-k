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
extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);

/* Network stack (net.c) */
extern int  net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                            uint16_t src_port);
extern int  net_tcp_send(int conn_idx, const void *data, uint32_t len);
extern int  net_tcp_recv(int conn_idx, void *buf, uint32_t buf_size);
extern void net_tcp_close(int conn_idx);
extern int  net_tcp_set_keepalive(int conn_idx, int enabled,
                                  uint32_t idle_ticks,
                                  uint32_t interval_ticks);
extern int  net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
                         uint16_t src_port, const void *data, uint32_t len);
extern int  net_udp_send_broadcast_self(uint16_t dst_port, uint16_t src_port,
                                        const void *data, uint32_t len);
extern int  net_udp_listen(uint16_t port, void *handler);
extern void net_poll(void);
extern void net_get_mac(uint8_t mac_out[6]);
extern uint8_t *net_get_ip_ptr(void);
extern uint16_t net_udp_dispatch_port(void);

/* TCP passive open (net.c) */
extern int  net_tcp_listen(uint16_t port) __attribute__((weak));
extern int  net_tcp_accept(int listen_idx, uint32_t timeout_ticks) __attribute__((weak));

/* ── Socket Constants ────────────────────────────────────────── */

#define AF_INET      2
#define AF_INET6    10
#define SOCK_STREAM  1   /* TCP */
#define SOCK_DGRAM   2   /* UDP */

#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9

/* ── Socket Address ──────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t sin_family;
    uint16_t sin_port;      /* Network byte order */
    uint32_t sin_addr;      /* Network byte order */
    uint8_t  sin_zero[8];
} sockaddr_in_t;

/* ── Socket Table ────────────────────────────────────────────── */

#define MAX_SOCKETS 256
#define UDP_MAX_DATAGRAM 65507U
#define UDP_QUEUE_LIMIT  131072U
#define UDP_QUEUE_PACKET_LIMIT 256U
#define TCP_SEND_BUFFER_SIZE 16384U
#define TCP_RECV_BUFFER_SIZE 131072U
#define TCP_KEEPALIVE_IDLE_TICKS 720000U
#define TCP_KEEPALIVE_INTERVAL_TICKS 7500U

typedef struct udp_packet {
    struct udp_packet *next;
    uint32_t length;
    uint8_t src_ip[4];
    uint16_t src_port;
    uint8_t data[];
} udp_packet_t;

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
    bool     reuse_addr;
    bool     keepalive;
    udp_packet_t *udp_head;
    udp_packet_t *udp_tail;
    uint32_t udp_queued_bytes;
    uint32_t udp_queued_packets;
    uint32_t udp_queue_limit;
} socket_t;

static socket_t sockets[MAX_SOCKETS];
static uint16_t next_ephemeral_port = 49152;

static inline uint16_t ntohs_s(uint16_t v) {
    return (v >> 8) | (v << 8);
}

static bool socket_port_conflicts(const socket_t *socket, uint16_t port)
{
    for (int i = 0; i < MAX_SOCKETS; i++) {
        const socket_t *candidate = &sockets[i];
        if (!candidate->active || candidate == socket ||
            candidate->type != socket->type ||
            candidate->local_port != port)
            continue;

        /* SO_REUSEADDR does not permit concurrent TCP listeners. */
        if (socket->type == SOCK_STREAM || !socket->reuse_addr ||
            !candidate->reuse_addr)
            return true;
    }
    return false;
}

static int socket_allocate_ephemeral(const socket_t *socket,
                                     uint16_t *port)
{
    for (uint32_t attempt = 0; attempt < 16384U; attempt++) {
        uint16_t candidate = next_ephemeral_port++;
        if (next_ephemeral_port < 49152)
            next_ephemeral_port = 49152;
        if (!socket_port_conflicts(socket, candidate)) {
            *port = candidate;
            return 0;
        }
    }
    return -98; /* EADDRINUSE */
}

static bool socket_udp_enqueue(socket_t *socket, const uint8_t *src_ip,
                               uint16_t src_port, const uint8_t *data,
                               uint32_t len)
{
    if (!socket || len > UDP_MAX_DATAGRAM ||
        socket->udp_queued_packets >= UDP_QUEUE_PACKET_LIMIT ||
        socket->udp_queued_bytes > socket->udp_queue_limit ||
        len > socket->udp_queue_limit - socket->udp_queued_bytes)
        return false;

    udp_packet_t *packet = (udp_packet_t *)kmalloc(sizeof(*packet) + len);
    if (!packet) return false;
    packet->next = 0;
    packet->length = len;
    memcpy(packet->src_ip, src_ip, 4);
    packet->src_port = src_port;
    if (len) memcpy(packet->data, data, len);

    if (socket->udp_tail)
        socket->udp_tail->next = packet;
    else
        socket->udp_head = packet;
    socket->udp_tail = packet;
    socket->udp_queued_bytes += len;
    socket->udp_queued_packets++;
    return true;
}

static void socket_udp_clear(socket_t *socket)
{
    udp_packet_t *packet = socket->udp_head;
    while (packet) {
        udp_packet_t *next = packet->next;
        kfree(packet);
        packet = next;
    }
    socket->udp_head = 0;
    socket->udp_tail = 0;
    socket->udp_queued_bytes = 0;
    socket->udp_queued_packets = 0;
}

/* ── UDP receive handler ─────────────────────────────────────── */

static void socket_udp_handler(const uint8_t *src_ip, uint16_t src_port,
                               const uint8_t *data, uint32_t len)
{
    uint16_t dst_port = net_udp_dispatch_port();
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].active && sockets[i].type == SOCK_DGRAM &&
            sockets[i].local_port == dst_port) {
            if (sockets[i].connected &&
                (sockets[i].remote_port != src_port ||
                 memcmp(sockets[i].remote_ip, src_ip, 4) != 0))
                continue;
            (void)socket_udp_enqueue(&sockets[i], src_ip, src_port,
                                     data, len);
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
            sockets[i].udp_queue_limit = UDP_QUEUE_LIMIT;
            if (type == SOCK_DGRAM &&
                net_udp_listen(0, (void *)socket_udp_handler) < 0) {
                memset(&sockets[i], 0, sizeof(socket_t));
                sockets[i].tcp_conn = -1;
                return -105; /* ENOBUFS */
            }
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
    if (!addr) return -14;
    if (addr->sin_family != AF_INET) return -97;
    if (s->local_port) return -22;

    uint16_t port = ntohs_s(addr->sin_port);
    int result = 0;
    if (!port)
        result = socket_allocate_ephemeral(s, &port);
    else if (socket_port_conflicts(s, port))
        result = -98;
    if (result < 0) return result;
    s->local_port = port;

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
        int conn = net_tcp_accept(s->tcp_conn >= 0 ? s->tcp_conn : 0, 0);
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

    if (s->local_port == 0) {
        int result = socket_allocate_ephemeral(s, &s->local_port);
        if (result < 0) return result;
    }

    if (s->type == SOCK_STREAM) {
        if (s->local_port == 0) s->local_port = 30000 + (uint16_t)(idt_get_ticks() & 0x7FFF);
        int conn = net_tcp_connect(ip, port, s->local_port);
        if (conn < 0) return -111;  /* ECONNREFUSED */
        s->tcp_conn = conn;
        if (s->keepalive &&
            net_tcp_set_keepalive(conn, 1, TCP_KEEPALIVE_IDLE_TICKS,
                                  TCP_KEEPALIVE_INTERVAL_TICKS) < 0) {
            net_tcp_close(conn);
            s->tcp_conn = -1;
            return -5;
        }
    }
    s->connected = true;
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
    if (s->type == SOCK_DGRAM)
        return net_udp_send(s->remote_ip, s->remote_port, s->local_port, buf, len);
    return -95;
}

int sock_recvfrom(int sock_idx, void *buf, uint32_t len,
                  int flags, sockaddr_in_t *src, uint32_t *addrlen);

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
    if (s->type == SOCK_DGRAM)
        return sock_recvfrom(sock_idx, buf, len, flags, 0, 0);
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

    if (s->local_port == 0) {
        int result = socket_allocate_ephemeral(s, &s->local_port);
        if (result < 0) return result;
    }

    uint8_t *local_ip = net_get_ip_ptr();
    bool loopback = ip[0] == 127;
    bool local = memcmp(ip, local_ip, 4) == 0;
    bool broadcast = ip[0] == 255 && ip[1] == 255 &&
                     ip[2] == 255 && ip[3] == 255;
    if (broadcast)
        return net_udp_send_broadcast_self(port, s->local_port, buf, len);
    if (loopback || local) {
        bool delivered = false;
        for (int i = 0; i < MAX_SOCKETS; i++) {
            socket_t *dst = &sockets[i];
            if (!dst->active || dst->type != SOCK_DGRAM || dst->local_port != port)
                continue;
            if (dst->connected &&
                (dst->remote_port != s->local_port ||
                 memcmp(dst->remote_ip, loopback ? ip : local_ip, 4) != 0))
                continue;
            (void)socket_udp_enqueue(dst, loopback ? ip : local_ip,
                                     s->local_port, buf, len);
            delivered = true;
        }
        (void)delivered;
        return (int)len;
    }
    return net_udp_send(ip, port, s->local_port, buf, len);
}

int sock_recvfrom(int sock_idx, void *buf, uint32_t len,
                  int flags, sockaddr_in_t *src, uint32_t *addrlen)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (!s->active || s->type != SOCK_DGRAM) return -95;

    net_poll();
    udp_packet_t *packet = s->udp_head;
    if (!packet) return -11;  /* EAGAIN */

    uint32_t copy = packet->length < len ? packet->length : len;
    if (copy) memcpy(buf, packet->data, copy);

    if (src) {
        src->sin_family = AF_INET;
        src->sin_port = ntohs_s(packet->src_port);
        memcpy(&src->sin_addr, packet->src_ip, 4);
    }
    if (addrlen) *addrlen = sizeof(sockaddr_in_t);

    if (!(flags & 0x02)) { /* MSG_PEEK */
        s->udp_head = packet->next;
        if (!s->udp_head) s->udp_tail = 0;
        s->udp_queued_bytes -= packet->length;
        s->udp_queued_packets--;
        kfree(packet);
    }
    return (int)copy;
}

int sock_close(int sock_idx)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (!s->active) return -9;
    if (s->type == SOCK_STREAM && s->tcp_conn >= 0)
        net_tcp_close(s->tcp_conn);
    socket_udp_clear(s);
    memset(s, 0, sizeof(*s));
    s->tcp_conn = -1;
    return 0;
}

int sock_setsockopt(int sock_idx, int level, int optname,
                    const void *optval, uint32_t optlen)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS || !sockets[sock_idx].active)
        return -9;
    if (level != SOL_SOCKET) return -92; /* ENOPROTOOPT */
    if (!optval || optlen < sizeof(int)) return -22;

    int value;
    memcpy(&value, optval, sizeof(value));
    socket_t *socket = &sockets[sock_idx];
    switch (optname) {
    case SO_REUSEADDR:
        socket->reuse_addr = value != 0;
        return 0;
    case SO_KEEPALIVE: {
        if (socket->type != SOCK_STREAM) return -92;
        bool enabled = value != 0;
        if (socket->tcp_conn >= 0 &&
            net_tcp_set_keepalive(socket->tcp_conn, enabled,
                                  TCP_KEEPALIVE_IDLE_TICKS,
                                  TCP_KEEPALIVE_INTERVAL_TICKS) < 0)
            return -5;
        socket->keepalive = enabled;
        return 0;
    }
    case SO_RCVBUF:
        if (socket->type != SOCK_DGRAM) return -92;
        if (value <= 0 || (uint32_t)value > UDP_QUEUE_LIMIT) return -22;
        socket->udp_queue_limit = (uint32_t)value;
        return 0;
    case SO_SNDBUF:
        return -92;
    default:
        return -92;
    }
}

int sock_getsockname(int sock_idx, sockaddr_in_t *addr, uint32_t *addrlen)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (addr) {
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_port = ntohs_s(s->local_port);
        memcpy(&addr->sin_addr, net_get_ip_ptr(), 4);
    }
    if (addrlen) *addrlen = sizeof(sockaddr_in_t);
    return 0;
}

int sock_getpeername(int sock_idx, sockaddr_in_t *addr, uint32_t *addrlen)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return -9;
    socket_t *s = &sockets[sock_idx];
    if (!s->active || !s->connected) return -107;
    if (addr) {
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_port = ntohs_s(s->remote_port);
        memcpy(&addr->sin_addr, s->remote_ip, 4);
    }
    if (addrlen) *addrlen = sizeof(sockaddr_in_t);
    return 0;
}

int sock_getsockopt(int sock_idx, int level, int optname,
                    void *optval, uint32_t *optlen)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS || !sockets[sock_idx].active)
        return -9;
    if (!optval || !optlen || *optlen < sizeof(int)) return -22;
    if (level != SOL_SOCKET) return -92;

    socket_t *socket = &sockets[sock_idx];
    int value;
    switch (optname) {
    case SO_REUSEADDR:
        value = socket->reuse_addr;
        break;
    case SO_KEEPALIVE:
        if (socket->type != SOCK_STREAM) return -92;
        value = socket->keepalive;
        break;
    case SO_TYPE:
        value = socket->type;
        break;
    case SO_ERROR:
        value = 0;
        break;
    case SO_SNDBUF:
        value = socket->type == SOCK_STREAM
              ? (int)TCP_SEND_BUFFER_SIZE : (int)UDP_MAX_DATAGRAM;
        break;
    case SO_RCVBUF:
        value = socket->type == SOCK_DGRAM
              ? (int)socket->udp_queue_limit : (int)TCP_RECV_BUFFER_SIZE;
        break;
    default:
        return -92;
    }
    memcpy(optval, &value, sizeof(value));
    *optlen = sizeof(value);
    return 0;
}

int sock_pending_bytes(int sock_idx)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS || !sockets[sock_idx].active)
        return -9;
    net_poll();
    udp_packet_t *packet = sockets[sock_idx].udp_head;
    return packet ? (int)packet->length : 0;
}

int sock_has_pending_data(int sock_idx)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS || !sockets[sock_idx].active)
        return -9;
    net_poll();
    return sockets[sock_idx].udp_head != 0;
}

int sock_set_recv_buffer(int sock_idx, uint32_t size)
{
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS || !sockets[sock_idx].active)
        return -9;
    if (sockets[sock_idx].type != SOCK_DGRAM)
        return -92;
    if (!size || size > UDP_QUEUE_LIMIT)
        return -22;
    sockets[sock_idx].udp_queue_limit = size;
    return 0;
}
