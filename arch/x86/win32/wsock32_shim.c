/*
 * OsitoK Windows Compatibility Layer — wsock32/ws2_32 Shim
 * All networking calls fail gracefully. UT99 runs in offline mode.
 */

#include "wsock32_shim.h"

extern void serial_puts(const char *s);

static int wsa_last_error = WSANOTINITIALISED;

int WINAPI WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData)
{
    (void)wVersionRequested;
    serial_puts("[WSOCK] WSAStartup (stub — offline mode)\n");
    if (lpWSAData) {
        BYTE *p = (BYTE *)lpWSAData;
        for (SIZE_T i = 0; i < sizeof(WSADATA); i++) p[i] = 0;
        lpWSAData->wVersion     = wVersionRequested;
        lpWSAData->wHighVersion = wVersionRequested;
    }
    wsa_last_error = 0;
    return 0; /* success — pretend init worked */
}

int WINAPI WSACleanup(void) { return 0; }
int WINAPI WSAGetLastError(void) { return wsa_last_error; }

SOCKET WINAPI sock_socket(int af, int type, int protocol)
{
    (void)af; (void)type; (void)protocol;
    wsa_last_error = WSAENETDOWN;
    return INVALID_SOCKET;
}

int WINAPI closesocket(SOCKET s) { (void)s; return 0; }

int WINAPI sock_connect(SOCKET s, PVOID name, int namelen)
{
    (void)s; (void)name; (void)namelen;
    wsa_last_error = WSAENETDOWN;
    return SOCKET_ERROR;
}

int WINAPI sock_send(SOCKET s, PCSTR buf, int len, int flags)
{
    (void)s; (void)buf; (void)len; (void)flags;
    wsa_last_error = WSAENETDOWN;
    return SOCKET_ERROR;
}

int WINAPI sock_recv(SOCKET s, PSTR buf, int len, int flags)
{
    (void)s; (void)buf; (void)len; (void)flags;
    wsa_last_error = WSAENETDOWN;
    return SOCKET_ERROR;
}

int WINAPI sock_bind(SOCKET s, PVOID name, int namelen)
{
    (void)s; (void)name; (void)namelen;
    wsa_last_error = WSAENETDOWN;
    return SOCKET_ERROR;
}

int WINAPI sock_listen(SOCKET s, int backlog)
{
    (void)s; (void)backlog;
    wsa_last_error = WSAENETDOWN;
    return SOCKET_ERROR;
}

SOCKET WINAPI sock_accept(SOCKET s, PVOID addr, int *addrlen)
{
    (void)s; (void)addr; (void)addrlen;
    wsa_last_error = WSAENETDOWN;
    return INVALID_SOCKET;
}

int WINAPI sock_getpeername(SOCKET s, PVOID name, int *namelen)
{
    (void)s; (void)name; (void)namelen;
    wsa_last_error = WSAENETDOWN;
    return SOCKET_ERROR;
}

int WINAPI sock_getsockname(SOCKET s, PVOID name, int *namelen)
{
    (void)s; (void)name; (void)namelen;
    wsa_last_error = WSAENETDOWN;
    return SOCKET_ERROR;
}

int WINAPI sock_getsockopt(SOCKET s, int level, int optname, PSTR optval, int *optlen)
{
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
    wsa_last_error = WSAENETDOWN;
    return SOCKET_ERROR;
}

ULONG WINAPI sock_inet_addr(PCSTR cp)
{
    (void)cp;
    return 0xFFFFFFFF; /* INADDR_NONE */
}

PSTR WINAPI sock_inet_ntoa(ULONG in)
{
    (void)in;
    return (PSTR)"0.0.0.0";
}

int WINAPI sock_recvfrom(SOCKET s, PSTR buf, int len, int flags,
                         PVOID from, int *fromlen)
{
    (void)s; (void)buf; (void)len; (void)flags; (void)from; (void)fromlen;
    wsa_last_error = WSAENETDOWN;
    return SOCKET_ERROR;
}

int WINAPI sock_select(int nfds, PVOID readfds, PVOID writefds,
                       PVOID exceptfds, PVOID timeout)
{
    (void)nfds; (void)readfds; (void)writefds; (void)exceptfds; (void)timeout;
    wsa_last_error = WSAENETDOWN;
    return SOCKET_ERROR;
}

int WINAPI sock_sendto(SOCKET s, PCSTR buf, int len, int flags,
                       PVOID to, int tolen)
{
    (void)s; (void)buf; (void)len; (void)flags; (void)to; (void)tolen;
    wsa_last_error = WSAENETDOWN;
    return SOCKET_ERROR;
}

int WINAPI sock_setsockopt(SOCKET s, int level, int optname,
                           PCSTR optval, int optlen)
{
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0; /* pretend it worked */
}

int WINAPI sock_shutdown(SOCKET s, int how)
{
    (void)s; (void)how;
    return 0;
}

PVOID WINAPI sock_gethostbyname(PCSTR name)
{
    (void)name;
    return NULL;
}

/* Byte order conversion — these are pure functions, no network needed */
ULONG  WINAPI ntohl(ULONG n)  { return ((n>>24)&0xFF)|((n>>8)&0xFF00)|((n<<8)&0xFF0000)|((n<<24)&0xFF000000); }
ULONG  WINAPI htonl(ULONG h)  { return ntohl(h); }
USHORT WINAPI ntohs(USHORT n) { return (USHORT)(((n>>8)&0xFF)|((n<<8)&0xFF00)); }
USHORT WINAPI htons(USHORT h) { return ntohs(h); }

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; } SHIM_EXPORT;

static const SHIM_EXPORT wsock_exports[] = {
    { "WSAStartup",     (PVOID)WSAStartup },
    { "WSACleanup",     (PVOID)WSACleanup },
    { "WSAGetLastError",(PVOID)WSAGetLastError },
    { "socket",         (PVOID)sock_socket },
    { "accept",         (PVOID)sock_accept },
    { "closesocket",    (PVOID)closesocket },
    { "connect",        (PVOID)sock_connect },
    { "send",           (PVOID)sock_send },
    { "recv",           (PVOID)sock_recv },
    { "recvfrom",       (PVOID)sock_recvfrom },
    { "sendto",         (PVOID)sock_sendto },
    { "bind",           (PVOID)sock_bind },
    { "listen",         (PVOID)sock_listen },
    { "select",         (PVOID)sock_select },
    { "getpeername",    (PVOID)sock_getpeername },
    { "getsockname",    (PVOID)sock_getsockname },
    { "getsockopt",     (PVOID)sock_getsockopt },
    { "setsockopt",     (PVOID)sock_setsockopt },
    { "shutdown",       (PVOID)sock_shutdown },
    { "gethostbyname",  (PVOID)sock_gethostbyname },
    { "inet_addr",      (PVOID)sock_inet_addr },
    { "inet_ntoa",      (PVOID)sock_inet_ntoa },
    { "ntohl",          (PVOID)ntohl },
    { "htonl",          (PVOID)htonl },
    { "ntohs",          (PVOID)ntohs },
    { "htons",          (PVOID)htons },
    { NULL, NULL }
};

/* ── Ordinal table (WSOCK32 exports by ordinal, not name) ──── */

typedef struct { USHORT ordinal; PVOID func; } SHIM_ORDINAL;

static const SHIM_ORDINAL wsock_ordinals[] = {
    {   1, (PVOID)sock_accept       },  /* accept       */
    {   2, (PVOID)sock_bind         },  /* bind         */
    {   3, (PVOID)closesocket       },  /* closesocket  */
    {   4, (PVOID)sock_connect      },  /* connect      */
    {   9, (PVOID)sock_getpeername  },  /* getpeername  */
    {  10, (PVOID)sock_getsockname  },  /* getsockname  */
    {  11, (PVOID)sock_getsockopt   },  /* getsockopt   */
    {  12, (PVOID)htonl             },  /* htonl        */
    {  13, (PVOID)htons             },  /* htons        */
    {  14, (PVOID)sock_inet_addr    },  /* inet_addr    */
    {  15, (PVOID)sock_inet_ntoa    },  /* inet_ntoa    */
    {  16, (PVOID)sock_listen       },  /* listen       */
    {  18, (PVOID)sock_recv         },  /* recv         */
    {  19, (PVOID)sock_recvfrom     },  /* recvfrom     */
    {  20, (PVOID)sock_select       },  /* select       */
    {  21, (PVOID)sock_send         },  /* send         */
    {  22, (PVOID)sock_sendto       },  /* sendto       */
    {  23, (PVOID)sock_setsockopt   },  /* setsockopt   */
    {  24, (PVOID)sock_shutdown     },  /* shutdown     */
    {  25, (PVOID)sock_socket       },  /* socket       */
    {  52, (PVOID)sock_gethostbyname},  /* gethostbyname */
    { 111, (PVOID)WSAStartup        },  /* WSAStartup   */
    { 116, (PVOID)WSACleanup        },  /* WSACleanup   */
    {   0, NULL }
};

static int ws_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

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
