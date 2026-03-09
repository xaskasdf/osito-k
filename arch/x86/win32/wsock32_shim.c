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
    { "WSAGetLastError", (PVOID)WSAGetLastError },
    { "socket",         (PVOID)sock_socket },
    { "closesocket",    (PVOID)closesocket },
    { "connect",        (PVOID)sock_connect },
    { "send",           (PVOID)sock_send },
    { "recv",           (PVOID)sock_recv },
    { "bind",           (PVOID)sock_bind },
    { "listen",         (PVOID)sock_listen },
    { "ntohl",          (PVOID)ntohl },
    { "htonl",          (PVOID)htonl },
    { "ntohs",          (PVOID)ntohs },
    { "htons",          (PVOID)htons },
    { NULL, NULL }
};

static int ws_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID wsock32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;
    for (int i = 0; wsock_exports[i].name; i++) {
        if (ws_strcmp(func_name, wsock_exports[i].name) == 0)
            return wsock_exports[i].func;
    }
    return NULL;
}

PVOID wsock32_shim_init(void) { return (PVOID)wsock_exports; }
