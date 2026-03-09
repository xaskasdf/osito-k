/*
 * OsitoK Windows Compatibility Layer — wsock32.dll / ws2_32.dll Shim
 * Stub — all calls return errors (offline mode). UT99 works offline.
 */

#ifndef WSOCK32_SHIM_H
#define WSOCK32_SHIM_H

#include "nttypes.h"

/* Winsock error */
#define WSANOTINITIALISED   10093
#define WSAENETDOWN         10050
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

int   WINAPI WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData);
int   WINAPI WSACleanup(void);
int   WINAPI WSAGetLastError(void);
SOCKET WINAPI sock_socket(int af, int type, int protocol);
int   WINAPI closesocket(SOCKET s);
int   WINAPI sock_connect(SOCKET s, PVOID name, int namelen);
int   WINAPI sock_send(SOCKET s, PCSTR buf, int len, int flags);
int   WINAPI sock_recv(SOCKET s, PSTR buf, int len, int flags);
int   WINAPI sock_bind(SOCKET s, PVOID name, int namelen);
int   WINAPI sock_listen(SOCKET s, int backlog);
ULONG WINAPI ntohl(ULONG netlong);
ULONG WINAPI htonl(ULONG hostlong);
USHORT WINAPI ntohs(USHORT netshort);
USHORT WINAPI htons(USHORT hostshort);

PVOID wsock32_shim_init(void);
PVOID wsock32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* WSOCK32_SHIM_H */
