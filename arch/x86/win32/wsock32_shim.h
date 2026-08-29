/*
 * OsitoK Windows Compatibility Layer — wsock32.dll / ws2_32.dll Shim
 * Winsock calls are bridged to the kernel network and synchronization layers.
 */

#ifndef WSOCK32_SHIM_H
#define WSOCK32_SHIM_H

#include "nttypes.h"

/* Winsock error */
#define WSANOTINITIALISED   10093
#define WSAENETDOWN         10050
#define WSAEFAULT           10014
#define WSAEMFILE           10024
#define WSAENOTSOCK         10038
#define WSAENOTCONN         10057
#define WSAECONNREFUSED     10061
#define WSAECONNRESET       10054
#define WSAEWOULDBLOCK      10035
#define WSAETIMEDOUT        10060
#define SOCKET_ERROR        (-1)
#define INVALID_SOCKET      (~(ULONG_PTR)0)

/* Socket types */
typedef ULONG_PTR SOCKET;

typedef struct WSAData {
    WORD    wVersion;
    WORD    wHighVersion;
    char    szDescription[257];
    char    szSystemStatus[129];
    USHORT  iMaxSockets;
    USHORT  iMaxUdpDg;
    PSTR    lpVendorInfo;
} WSADATA, *LPWSADATA;

int    WINAPI WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData);
int    WINAPI WSACleanup(void);
int    WINAPI WSAGetLastError(void);
SOCKET WINAPI sock_socket(int af, int type, int protocol);
SOCKET WINAPI sock_accept(SOCKET s, PVOID addr, int *addrlen);
int    WINAPI closesocket(SOCKET s);
int    WINAPI sock_connect(SOCKET s, PVOID name, int namelen);
int    WINAPI sock_send(SOCKET s, PCSTR buf, int len, int flags);
int    WINAPI sock_recv(SOCKET s, PSTR buf, int len, int flags);
int    WINAPI sock_recvfrom(SOCKET s, PSTR buf, int len, int flags, PVOID from, int *fromlen);
int    WINAPI sock_sendto(SOCKET s, PCSTR buf, int len, int flags, PVOID to, int tolen);
int    WINAPI sock_bind(SOCKET s, PVOID name, int namelen);
int    WINAPI sock_listen(SOCKET s, int backlog);
int    WINAPI sock_select(int nfds, PVOID readfds, PVOID writefds, PVOID exceptfds, PVOID timeout);
int    WINAPI sock_getpeername(SOCKET s, PVOID name, int *namelen);
int    WINAPI sock_getsockname(SOCKET s, PVOID name, int *namelen);
int    WINAPI sock_getsockopt(SOCKET s, int level, int optname, PSTR optval, int *optlen);
int    WINAPI sock_setsockopt(SOCKET s, int level, int optname, PCSTR optval, int optlen);
int    WINAPI sock_shutdown(SOCKET s, int how);
PVOID  WINAPI sock_gethostbyname(PCSTR name);
ULONG  WINAPI sock_inet_addr(PCSTR cp);
PSTR   WINAPI sock_inet_ntoa(ULONG in);
ULONG  WINAPI ntohl(ULONG netlong);
ULONG  WINAPI htonl(ULONG hostlong);
USHORT WINAPI ntohs(USHORT netshort);
USHORT WINAPI htons(USHORT hostshort);

PVOID wsock32_shim_init(void);
PVOID wsock32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
PVOID ws2_32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
void  wsock_service_pending_io(void);
BOOL  wsock_cancel_io(SOCKET socket, PVOID overlapped,
                      BOOL current_thread_only);
DWORD wsock_release_process(DWORD process_id);
int   wsock_selftest(void);

#endif /* WSOCK32_SHIM_H */
