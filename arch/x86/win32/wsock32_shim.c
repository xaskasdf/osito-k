/*
 * OsitoK Windows Compatibility Layer — wsock32/ws2_32 Shim
 *
 * Bridges Win32 Winsock calls to OsitoK's kernel TCP/IP stack (net.c).
 * Supports socket/connect/send/recv/closesocket for TCP clients,
 * gethostbyname via kernel DNS, and select (always-ready stub).
 * bind/listen/accept remain stubs (server sockets less common in games).
 */

#include "wsock32_shim.h"
#include "win32_abi.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

/* ── Kernel network API (from kernel/net.h) ──────────────── */

extern int  net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                            uint16_t src_port);
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern int  net_tcp_recv(int conn, void *buf, uint32_t buf_size);
extern int  net_tcp_recv_timeout(int conn, void *buf, uint32_t buf_size,
                                 uint32_t timeout_ticks);
extern void net_tcp_close(int conn);
extern int  net_tcp_state(int conn);
extern int  net_dns_resolve(const char *hostname, uint8_t ip_out[4]);
extern int  net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
                         uint16_t src_port, const void *data, uint32_t len);
extern void net_poll(void);

/* TCP states from net.h */
#define KERN_TCP_CLOSED      0
#define KERN_TCP_ESTABLISHED 2

/* ── Additional Winsock error codes ──────────────────────── */
#define WSAEWOULDBLOCK  10035
#define WSAEMFILE       10024
#define WSAENOTSOCK     10038
#define WSAEFAULT       10014
#define WSAENOTCONN     10057
#define WSAECONNREFUSED 10061
#define WSAECONNRESET   10054
#define WSAHOST_NOT_FOUND 11001

/* ── Winsock state ───────────────────────────────────────── */

static int wsa_last_error = WSANOTINITIALISED;
static int wsa_initialized = 0;

/* ── Socket table ────────────────────────────────────────── */

#define MAX_WIN32_SOCKETS 32
#define BASE_SOCKET       0x100   /* offset so SOCKET values don't collide with stdin/stdout */

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

typedef struct {
    int     in_use;
    int     af;
    int     type;       /* SOCK_STREAM or SOCK_DGRAM */
    int     protocol;
    int     kern_conn_id;   /* kernel TCP connection index, or -1 */
    int     connected;
    uint8_t remote_ip[4];
    uint16_t remote_port;
    uint16_t local_port;
} win32_socket_t;

static win32_socket_t win32_sockets[MAX_WIN32_SOCKETS];
static uint16_t next_ephemeral_port = 49152;

static void ws_memset(void *p, int v, SIZE_T n)
{
    BYTE *b = (BYTE *)p;
    while (n--) *b++ = (BYTE)v;
}

static void ws_memcpy(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--) *d++ = *s++;
}

static int ws_strlen(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n;
}

static int ws_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Convert SOCKET handle to table index, or -1 if invalid */
static int sock_to_index(SOCKET s)
{
    int idx = (int)(s - BASE_SOCKET);
    if (idx < 0 || idx >= MAX_WIN32_SOCKETS) return -1;
    if (!win32_sockets[idx].in_use) return -1;
    return idx;
}

/* Allocate next ephemeral port */
static uint16_t alloc_ephemeral_port(void)
{
    uint16_t p = next_ephemeral_port++;
    if (next_ephemeral_port > 65500) next_ephemeral_port = 49152;
    return p;
}

/* ── WSA functions ───────────────────────────────────────── */

int WINAPI WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData)
{
    (void)wVersionRequested;
    serial_puts("[WSOCK] WSAStartup — bridging to kernel TCP/IP\n");
    if (lpWSAData) {
        BYTE *p = (BYTE *)lpWSAData;
        for (SIZE_T i = 0; i < sizeof(WSADATA); i++) p[i] = 0;
        lpWSAData->wVersion     = wVersionRequested;
        lpWSAData->wHighVersion = wVersionRequested;
    }
    /* Initialize socket table */
    ws_memset(win32_sockets, 0, sizeof(win32_sockets));
    for (int i = 0; i < MAX_WIN32_SOCKETS; i++)
        win32_sockets[i].kern_conn_id = -1;

    wsa_initialized = 1;
    wsa_last_error = 0;
    return 0; /* success */
}

int WINAPI WSACleanup(void)
{
    /* Close all open sockets */
    for (int i = 0; i < MAX_WIN32_SOCKETS; i++) {
        if (win32_sockets[i].in_use && win32_sockets[i].kern_conn_id >= 0) {
            net_tcp_close(win32_sockets[i].kern_conn_id);
        }
        win32_sockets[i].in_use = 0;
    }
    wsa_initialized = 0;
    return 0;
}

int WINAPI WSAGetLastError(void) { return wsa_last_error; }

/* ── socket() ────────────────────────────────────────────── */

SOCKET WINAPI wsock_socket(int af, int type, int protocol)
{
    if (!wsa_initialized) { wsa_last_error = WSANOTINITIALISED; return INVALID_SOCKET; }

    /* Find free slot */
    int idx = -1;
    for (int i = 0; i < MAX_WIN32_SOCKETS; i++) {
        if (!win32_sockets[i].in_use) { idx = i; break; }
    }
    if (idx < 0) {
        serial_puts("[WSOCK] socket: no free slots\n");
        wsa_last_error = WSAEMFILE;
        return INVALID_SOCKET;
    }

    win32_socket_t *ws = &win32_sockets[idx];
    ws->in_use       = 1;
    ws->af           = af;
    ws->type         = type;
    ws->protocol     = protocol;
    ws->kern_conn_id = -1;
    ws->connected    = 0;
    ws->local_port   = alloc_ephemeral_port();

    serial_puts("[WSOCK] socket(");
    serial_puthex(af, 2); serial_puts(",");
    serial_puthex(type, 2); serial_puts(") = ");
    serial_puthex(BASE_SOCKET + idx, 4); serial_puts("\n");

    wsa_last_error = 0;
    return (SOCKET)(BASE_SOCKET + idx);
}

/* ── connect() ───────────────────────────────────────────── */

/* Winsock sockaddr_in layout (16 bytes):
 *   +0: uint16_t sin_family (AF_INET=2)
 *   +2: uint16_t sin_port (big-endian)
 *   +4: uint32_t sin_addr (big-endian)
 *   +8: char sin_zero[8]
 */

int WINAPI wsock_connect(SOCKET s, PVOID name, int namelen)
{
    (void)namelen;
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }

    win32_socket_t *ws = &win32_sockets[idx];
    if (!name) { wsa_last_error = WSAEFAULT; return SOCKET_ERROR; }

    /* Parse sockaddr_in */
    uint8_t *sa = (uint8_t *)name;
    uint16_t port_be = (uint16_t)(sa[2] << 8) | sa[3];
    uint8_t ip[4] = { sa[4], sa[5], sa[6], sa[7] };

    ws->remote_ip[0] = ip[0]; ws->remote_ip[1] = ip[1];
    ws->remote_ip[2] = ip[2]; ws->remote_ip[3] = ip[3];
    ws->remote_port  = port_be;

    serial_puts("[WSOCK] connect to ");
    serial_puthex(ip[0], 2); serial_puts(".");
    serial_puthex(ip[1], 2); serial_puts(".");
    serial_puthex(ip[2], 2); serial_puts(".");
    serial_puthex(ip[3], 2); serial_puts(":");
    serial_puthex(port_be, 4); serial_puts("\n");

    if (ws->type == SOCK_STREAM) {
        /* TCP connect via kernel */
        int conn = net_tcp_connect(ip, port_be, ws->local_port);
        if (conn < 0) {
            serial_puts("[WSOCK] connect: kernel TCP connect failed\n");
            wsa_last_error = WSAECONNREFUSED;
            return SOCKET_ERROR;
        }
        ws->kern_conn_id = conn;
        ws->connected = 1;
        serial_puts("[WSOCK] connect: OK, kern_conn=");
        serial_puthex(conn, 2); serial_puts("\n");
    } else {
        /* UDP: "connected" just saves the remote address */
        ws->connected = 1;
    }

    wsa_last_error = 0;
    return 0;
}

/* ── send() ──────────────────────────────────────────────── */

int WINAPI wsock_send(SOCKET s, PCSTR buf, int len, int flags)
{
    (void)flags;
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }

    win32_socket_t *ws = &win32_sockets[idx];
    if (!ws->connected) { wsa_last_error = WSAENOTCONN; return SOCKET_ERROR; }

    if (ws->type == SOCK_STREAM) {
        if (ws->kern_conn_id < 0) { wsa_last_error = WSAENOTCONN; return SOCKET_ERROR; }
        int sent = net_tcp_send(ws->kern_conn_id, buf, (uint32_t)len);
        if (sent < 0) {
            wsa_last_error = WSAENETDOWN;
            return SOCKET_ERROR;
        }
        wsa_last_error = 0;
        return sent;
    } else {
        /* UDP sendto via connected address */
        int ret = net_udp_send(ws->remote_ip, ws->remote_port,
                               ws->local_port, buf, (uint32_t)len);
        if (ret < 0) {
            wsa_last_error = WSAENETDOWN;
            return SOCKET_ERROR;
        }
        wsa_last_error = 0;
        return len; /* UDP: all or nothing */
    }
}

/* ── recv() ──────────────────────────────────────────────── */

int WINAPI wsock_recv(SOCKET s, PSTR buf, int len, int flags)
{
    (void)flags;
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }

    win32_socket_t *ws = &win32_sockets[idx];
    if (!ws->connected || ws->kern_conn_id < 0) {
        wsa_last_error = WSAENOTCONN;
        return SOCKET_ERROR;
    }

    /* Poll the network stack to process incoming packets */
    net_poll();

    int rcvd = net_tcp_recv(ws->kern_conn_id, buf, (uint32_t)len);
    if (rcvd < 0) {
        /* Connection closed */
        wsa_last_error = WSAECONNRESET;
        return SOCKET_ERROR;
    }
    if (rcvd == 0) {
        /* No data available — try with a short timeout */
        rcvd = net_tcp_recv_timeout(ws->kern_conn_id, buf, (uint32_t)len, 500);
        if (rcvd < 0) {
            wsa_last_error = WSAECONNRESET;
            return SOCKET_ERROR;
        }
        if (rcvd == 0) {
            /* Check if connection is still alive */
            int state = net_tcp_state(ws->kern_conn_id);
            if (state == KERN_TCP_CLOSED) {
                /* Graceful close: return 0 */
                return 0;
            }
            /* Still connected but no data — block would be needed.
             * Return WSAEWOULDBLOCK so caller retries. */
            wsa_last_error = WSAEWOULDBLOCK;
            return SOCKET_ERROR;
        }
    }

    wsa_last_error = 0;
    return rcvd;
}

/* ── closesocket() ───────────────────────────────────────── */

int WINAPI closesocket(SOCKET s)
{
    int idx = sock_to_index(s);
    if (idx < 0) return 0; /* silently ignore invalid */

    win32_socket_t *ws = &win32_sockets[idx];
    if (ws->kern_conn_id >= 0) {
        net_tcp_close(ws->kern_conn_id);
    }
    ws->in_use = 0;
    ws->kern_conn_id = -1;
    ws->connected = 0;

    serial_puts("[WSOCK] closesocket ");
    serial_puthex(BASE_SOCKET + idx, 4); serial_puts("\n");
    return 0;
}

/* ── select() ────────────────────────────────────────────── */

/* Simplified select: poll kernel, then report all sockets as ready.
 * This is sufficient for most game networking patterns. */
int WINAPI wsock_select(int nfds, PVOID readfds, PVOID writefds,
                       PVOID exceptfds, PVOID timeout)
{
    (void)nfds; (void)exceptfds; (void)timeout;

    /* Poll the kernel network stack */
    net_poll();

    /* Count ready sockets. fd_set layout for Win32:
     *   uint32_t fd_count;
     *   SOCKET   fd_array[FD_SETSIZE];
     * We return the count from readfds if provided, else writefds. */
    int count = 0;
    if (readfds) {
        uint32_t *fds = (uint32_t *)readfds;
        uint32_t n = fds[0];
        if (n > 0) count += (int)n; /* report all as ready */
    }
    if (writefds) {
        uint32_t *fds = (uint32_t *)writefds;
        uint32_t n = fds[0];
        if (n > 0) count += (int)n;
    }

    wsa_last_error = 0;
    return count > 0 ? count : 1; /* at least 1 ready */
}

/* ── gethostbyname() ─────────────────────────────────────── */

/*
 * 32-bit hostent layout for PE32 callers:
 *   +0: uint32_t h_name       (ptr to hostname string)
 *   +4: uint32_t h_aliases    (ptr to alias list — NULL-terminated)
 *   +8: int16_t  h_addrtype   (AF_INET=2)
 *  +10: int16_t  h_length     (4 for IPv4)
 *  +12: uint32_t h_addr_list  (ptr to array of ptrs to addresses)
 * Total: 16 bytes
 */
static uint8_t  hostent_ip[4];
static uint32_t hostent_addr_list[2];   /* [0]=ptr to ip, [1]=0 */
static uint32_t hostent_aliases[1];     /* [0]=0 (NULL terminator) */
static char     hostent_name[256];

/* The actual struct returned to 32-bit callers */
static uint8_t  hostent_buf[16];

PVOID WINAPI wsock_gethostbyname(PCSTR name)
{
    if (!name) { wsa_last_error = WSAEFAULT; return NULL; }

    serial_puts("[WSOCK] gethostbyname(\"");
    serial_puts(name);
    serial_puts("\")\n");

    uint8_t ip[4];
    int ret = net_dns_resolve(name, ip);
    if (ret < 0) {
        serial_puts("[WSOCK] gethostbyname: DNS resolve failed\n");
        wsa_last_error = WSAHOST_NOT_FOUND;
        return NULL;
    }

    serial_puts("[WSOCK] gethostbyname: resolved to ");
    serial_puthex(ip[0], 2); serial_puts(".");
    serial_puthex(ip[1], 2); serial_puts(".");
    serial_puthex(ip[2], 2); serial_puts(".");
    serial_puthex(ip[3], 2); serial_puts("\n");

    /* Copy hostname */
    int nlen = ws_strlen(name);
    if (nlen > 255) nlen = 255;
    ws_memcpy(hostent_name, name, (SIZE_T)nlen);
    hostent_name[nlen] = '\0';

    /* Fill IP address */
    hostent_ip[0] = ip[0]; hostent_ip[1] = ip[1];
    hostent_ip[2] = ip[2]; hostent_ip[3] = ip[3];

    /* Build pointer arrays (32-bit addresses) */
    hostent_addr_list[0] = (uint32_t)(uintptr_t)hostent_ip;
    hostent_addr_list[1] = 0;
    hostent_aliases[0] = 0;

    /* Build 32-bit hostent struct */
    uint32_t *he = (uint32_t *)hostent_buf;
    he[0] = (uint32_t)(uintptr_t)hostent_name;       /* h_name */
    he[1] = (uint32_t)(uintptr_t)hostent_aliases;     /* h_aliases */
    /* h_addrtype (int16) + h_length (int16) packed into 4 bytes */
    hostent_buf[8]  = AF_INET & 0xFF;                  /* h_addrtype low */
    hostent_buf[9]  = 0;                                /* h_addrtype high */
    hostent_buf[10] = 4;                                /* h_length low */
    hostent_buf[11] = 0;                                /* h_length high */
    he[3] = (uint32_t)(uintptr_t)hostent_addr_list;   /* h_addr_list */

    wsa_last_error = 0;
    return (PVOID)hostent_buf;
}

/* ── inet_addr() ─────────────────────────────────────────── */

ULONG WINAPI wsock_inet_addr(PCSTR cp)
{
    if (!cp) return 0xFFFFFFFF; /* INADDR_NONE */

    uint32_t parts[4] = {0,0,0,0};
    int pi = 0;
    const char *p = cp;

    while (*p && pi < 4) {
        if (*p >= '0' && *p <= '9') {
            parts[pi] = parts[pi] * 10 + (uint32_t)(*p - '0');
        } else if (*p == '.') {
            pi++;
        } else {
            return 0xFFFFFFFF;
        }
        p++;
    }

    /* Return in network byte order (big-endian as uint32_t stored little-endian) */
    return (parts[0]) | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}

/* ── inet_ntoa() ─────────────────────────────────────────── */

static char inet_ntoa_buf[16];

static void itoa_simple(uint32_t v, char *buf, int *pos)
{
    if (v >= 100) { buf[(*pos)++] = '0' + (char)(v / 100); v %= 100; }
    if (v >= 10)  { buf[(*pos)++] = '0' + (char)(v / 10);  v %= 10; }
    buf[(*pos)++] = '0' + (char)v;
}

PSTR WINAPI wsock_inet_ntoa(ULONG in)
{
    int pos = 0;
    itoa_simple((in >>  0) & 0xFF, inet_ntoa_buf, &pos);
    inet_ntoa_buf[pos++] = '.';
    itoa_simple((in >>  8) & 0xFF, inet_ntoa_buf, &pos);
    inet_ntoa_buf[pos++] = '.';
    itoa_simple((in >> 16) & 0xFF, inet_ntoa_buf, &pos);
    inet_ntoa_buf[pos++] = '.';
    itoa_simple((in >> 24) & 0xFF, inet_ntoa_buf, &pos);
    inet_ntoa_buf[pos] = '\0';
    return inet_ntoa_buf;
}

/* ── Stubs (server sockets, not commonly needed for games) ── */

int WINAPI wsock_bind(SOCKET s, PVOID name, int namelen)
{
    (void)namelen;
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }

    /* Parse local port from sockaddr_in */
    if (name) {
        uint8_t *sa = (uint8_t *)name;
        uint16_t port_be = (uint16_t)(sa[2] << 8) | sa[3];
        win32_sockets[idx].local_port = port_be;
        serial_puts("[WSOCK] bind port=");
        serial_puthex(port_be, 4); serial_puts(" (stored)\n");
    }

    wsa_last_error = 0;
    return 0; /* pretend success */
}

int WINAPI wsock_listen(SOCKET s, int backlog)
{
    (void)s; (void)backlog;
    serial_puts("[WSOCK] listen (stub)\n");
    wsa_last_error = 0;
    return 0;
}

SOCKET WINAPI wsock_accept(SOCKET s, PVOID addr, int *addrlen)
{
    (void)s; (void)addr; (void)addrlen;
    serial_puts("[WSOCK] accept (stub — would block forever)\n");
    wsa_last_error = WSAEWOULDBLOCK;
    return INVALID_SOCKET;
}

int WINAPI wsock_getpeername(SOCKET s, PVOID name, int *namelen)
{
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }

    win32_socket_t *ws = &win32_sockets[idx];
    if (!ws->connected) { wsa_last_error = WSAENOTCONN; return SOCKET_ERROR; }

    if (name && namelen && *namelen >= 16) {
        uint8_t *sa = (uint8_t *)name;
        ws_memset(sa, 0, 16);
        sa[0] = (uint8_t)(AF_INET & 0xFF); sa[1] = (uint8_t)(AF_INET >> 8);
        sa[2] = (uint8_t)(ws->remote_port >> 8); sa[3] = (uint8_t)(ws->remote_port & 0xFF);
        sa[4] = ws->remote_ip[0]; sa[5] = ws->remote_ip[1];
        sa[6] = ws->remote_ip[2]; sa[7] = ws->remote_ip[3];
        *namelen = 16;
    }
    wsa_last_error = 0;
    return 0;
}

int WINAPI wsock_getsockname(SOCKET s, PVOID name, int *namelen)
{
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }

    if (name && namelen && *namelen >= 16) {
        uint8_t *sa = (uint8_t *)name;
        ws_memset(sa, 0, 16);
        sa[0] = (uint8_t)(AF_INET & 0xFF); sa[1] = (uint8_t)(AF_INET >> 8);
        uint16_t lp = win32_sockets[idx].local_port;
        sa[2] = (uint8_t)(lp >> 8); sa[3] = (uint8_t)(lp & 0xFF);
        /* Local IP: 0.0.0.0 (any) */
        *namelen = 16;
    }
    wsa_last_error = 0;
    return 0;
}

int WINAPI wsock_getsockopt(SOCKET s, int level, int optname, PSTR optval, int *optlen)
{
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
    wsa_last_error = 0;
    return 0; /* pretend success */
}

int WINAPI wsock_setsockopt(SOCKET s, int level, int optname,
                           PCSTR optval, int optlen)
{
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0; /* pretend it worked */
}

int WINAPI wsock_shutdown(SOCKET s, int how)
{
    (void)how;
    int idx = sock_to_index(s);
    if (idx >= 0 && win32_sockets[idx].kern_conn_id >= 0) {
        net_tcp_close(win32_sockets[idx].kern_conn_id);
        win32_sockets[idx].kern_conn_id = -1;
        win32_sockets[idx].connected = 0;
    }
    return 0;
}

int WINAPI wsock_recvfrom(SOCKET s, PSTR buf, int len, int flags,
                         PVOID from, int *fromlen)
{
    /* For connected UDP sockets, just do recv */
    (void)from; (void)fromlen;
    return wsock_recv(s, buf, len, flags);
}

int WINAPI wsock_sendto(SOCKET s, PCSTR buf, int len, int flags,
                       PVOID to, int tolen)
{
    (void)flags; (void)tolen;
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }

    win32_socket_t *ws = &win32_sockets[idx];

    /* If 'to' is provided, parse destination from sockaddr_in */
    uint8_t dst_ip[4];
    uint16_t dst_port;

    if (to) {
        uint8_t *sa = (uint8_t *)to;
        dst_port = (uint16_t)(sa[2] << 8) | sa[3];
        dst_ip[0] = sa[4]; dst_ip[1] = sa[5];
        dst_ip[2] = sa[6]; dst_ip[3] = sa[7];
    } else if (ws->connected) {
        ws_memcpy(dst_ip, ws->remote_ip, 4);
        dst_port = ws->remote_port;
    } else {
        wsa_last_error = WSAENOTCONN;
        return SOCKET_ERROR;
    }

    if (ws->type == SOCK_STREAM && ws->kern_conn_id >= 0) {
        return wsock_send(s, buf, len, flags);
    }

    /* UDP send */
    int ret = net_udp_send(dst_ip, dst_port, ws->local_port, buf, (uint32_t)len);
    if (ret < 0) {
        wsa_last_error = WSAENETDOWN;
        return SOCKET_ERROR;
    }
    wsa_last_error = 0;
    return len;
}

/* Byte order conversion — these are pure functions, no network needed */
ULONG  WINAPI ntohl(ULONG n)  { return ((n>>24)&0xFF)|((n>>8)&0xFF00)|((n<<8)&0xFF0000)|((n<<24)&0xFF000000); }
ULONG  WINAPI htonl(ULONG h)  { return ntohl(h); }
USHORT WINAPI ntohs(USHORT n) { return (USHORT)(((n>>8)&0xFF)|((n<<8)&0xFF00)); }
USHORT WINAPI htons(USHORT h) { return ntohs(h); }

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT wsock_exports[] = {
    { "WSAStartup",     (PVOID)WSAStartup,         2, CC_STDCALL },
    { "WSACleanup",     (PVOID)WSACleanup,         0, CC_STDCALL },
    { "WSAGetLastError",(PVOID)WSAGetLastError,    0, CC_STDCALL },
    { "socket",         (PVOID)wsock_socket,       3, CC_STDCALL },
    { "accept",         (PVOID)wsock_accept,       3, CC_STDCALL },
    { "closesocket",    (PVOID)closesocket,        1, CC_STDCALL },
    { "connect",        (PVOID)wsock_connect,      3, CC_STDCALL },
    { "send",           (PVOID)wsock_send,         4, CC_STDCALL },
    { "recv",           (PVOID)wsock_recv,         4, CC_STDCALL },
    { "recvfrom",       (PVOID)wsock_recvfrom,     6, CC_STDCALL },
    { "sendto",         (PVOID)wsock_sendto,       6, CC_STDCALL },
    { "bind",           (PVOID)wsock_bind,         3, CC_STDCALL },
    { "listen",         (PVOID)wsock_listen,       2, CC_STDCALL },
    { "select",         (PVOID)wsock_select,       5, CC_STDCALL },
    { "getpeername",    (PVOID)wsock_getpeername,  3, CC_STDCALL },
    { "getsockname",    (PVOID)wsock_getsockname,  3, CC_STDCALL },
    { "getsockopt",     (PVOID)wsock_getsockopt,   5, CC_STDCALL },
    { "setsockopt",     (PVOID)wsock_setsockopt,   5, CC_STDCALL },
    { "shutdown",       (PVOID)wsock_shutdown,     2, CC_STDCALL },
    { "gethostbyname",  (PVOID)wsock_gethostbyname,1, CC_STDCALL },
    { "inet_addr",      (PVOID)wsock_inet_addr,    1, CC_STDCALL },
    { "inet_ntoa",      (PVOID)wsock_inet_ntoa,    1, CC_STDCALL },
    { "ntohl",          (PVOID)ntohl,              1, CC_STDCALL },
    { "htonl",          (PVOID)htonl,              1, CC_STDCALL },
    { "ntohs",          (PVOID)ntohs,              1, CC_STDCALL },
    { "htons",          (PVOID)htons,              1, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

/* ── Ordinal table (WSOCK32 exports by ordinal, not name) ──── */

typedef struct { USHORT ordinal; PVOID func; } SHIM_ORDINAL;

static const SHIM_ORDINAL wsock_ordinals[] = {
    {   1, (PVOID)wsock_accept       },  /* accept       */
    {   2, (PVOID)wsock_bind         },  /* bind         */
    {   3, (PVOID)closesocket       },  /* closesocket  */
    {   4, (PVOID)wsock_connect      },  /* connect      */
    {   9, (PVOID)wsock_getpeername  },  /* getpeername  */
    {  10, (PVOID)wsock_getsockname  },  /* getsockname  */
    {  11, (PVOID)wsock_getsockopt   },  /* getsockopt   */
    {  12, (PVOID)htonl             },  /* htonl        */
    {  13, (PVOID)htons             },  /* htons        */
    {  14, (PVOID)wsock_inet_addr    },  /* inet_addr    */
    {  15, (PVOID)wsock_inet_ntoa    },  /* inet_ntoa    */
    {  16, (PVOID)wsock_listen       },  /* listen       */
    {  18, (PVOID)wsock_recv         },  /* recv         */
    {  19, (PVOID)wsock_recvfrom     },  /* recvfrom     */
    {  20, (PVOID)wsock_select       },  /* select       */
    {  21, (PVOID)wsock_send         },  /* send         */
    {  22, (PVOID)wsock_sendto       },  /* sendto       */
    {  23, (PVOID)wsock_setsockopt   },  /* setsockopt   */
    {  24, (PVOID)wsock_shutdown     },  /* shutdown      */
    {  25, (PVOID)wsock_socket       },  /* socket       */
    {  52, (PVOID)wsock_gethostbyname},  /* gethostbyname */
    { 111, (PVOID)WSAStartup        },  /* WSAStartup   */
    { 116, (PVOID)WSACleanup        },  /* WSACleanup   */
    {   0, NULL }
};

PVOID wsock32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) {
        for (int i = 0; wsock_ordinals[i].func; i++) {
            if (wsock_ordinals[i].ordinal == ordinal)
                return wsock_ordinals[i].func;
        }
        return NULL;
    }
    for (int i = 0; wsock_exports[i].name; i++) {
        if (ws_strcmp(func_name, wsock_exports[i].name) == 0)
            return wsock_exports[i].func;
    }
    return NULL;
}

PVOID wsock32_shim_init(void) { return (PVOID)wsock_exports; }

const WIN32_EXPORT *wsock32_abi_table(int *count) {
    *count = (int)(sizeof(wsock_exports)/sizeof(wsock_exports[0]));
    return (const WIN32_EXPORT *)wsock_exports;
}
