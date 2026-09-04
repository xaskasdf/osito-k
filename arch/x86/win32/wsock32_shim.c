/*
 * OsitoK Windows Compatibility Layer — wsock32/ws2_32 Shim
 *
 * Bridges Win32 Winsock calls to OsitoK's kernel TCP/IP stack (net.c).
 * Implements client/server sockets, overlapped I/O, event notification,
 * name resolution, protocol discovery, and select() over the kernel stack.
 * Unsupported operations fail explicitly with a Winsock error.
 */

#include "wsock32_shim.h"
#include "kernel32_shim.h"
#include "win32_abi.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern DWORD win32_current_process_id(void);
extern int sched_spawn(const char *name, void (*entry)(void));
extern int sched_sleep_ticks(uint64_t ticks);
extern void sched_yield(void);
extern void random_get_bytes(void *buffer, uint32_t length);
extern NTSTATUS ntsync_set_event_for_process(HANDLE event, ULONG owner_pid,
                                              LONG *previous_state);

/* ── Kernel network API (from kernel/net.h) ──────────────── */

extern int  net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                            uint16_t src_port);
extern int  net_tcp_connect_begin(const uint8_t dst_ip[4], uint16_t dst_port,
                                  uint16_t src_port);
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern int  net_tcp_recv(int conn, void *buf, uint32_t buf_size);
extern int  net_tcp_peek(int conn, void *buf, uint32_t buf_size);
extern int  net_tcp_recv_timeout(int conn, void *buf, uint32_t buf_size,
                                 uint32_t timeout_ticks);
extern void net_tcp_shutdown(int conn);
extern int  net_tcp_close_timeout(int conn, uint32_t timeout_ticks);
extern void net_tcp_abort(int conn);
extern int  net_tcp_state(int conn);
extern int  net_tcp_rx_available(int conn);
extern int  net_tcp_send_ready(int conn);
extern int  net_tcp_set_keepalive(int conn, int enabled,
                                  uint32_t idle_ticks,
                                  uint32_t interval_ticks);
extern int  net_tcp_error(int conn);
extern void net_tcp_release(int conn);
extern int  net_tcp_listen(uint16_t port);
extern void net_tcp_set_listen_backlog(int listener, int backlog);
extern int  net_tcp_accept(int listener, uint32_t timeout_ticks);
extern int  net_tcp_accept_ready(int listener);
extern void net_tcp_stop_listen(int listener);
extern int  net_tcp_get_peer(int conn, uint8_t ip_out[4], uint16_t *port_out);
extern int  net_dns_resolve(const char *hostname, uint8_t ip_out[4]);
extern int  net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
                         uint16_t src_port, const void *data, uint32_t len);
extern int  kernel_sock_socket(int domain, int type, int protocol)
    __asm__("sock_socket");
extern int  kernel_sock_bind(int socket, PCVOID address)
    __asm__("sock_bind");
extern int  kernel_sock_connect(int socket, PCVOID address)
    __asm__("sock_connect");
extern int  kernel_sock_sendto(int socket, PCVOID data, uint32_t length,
                               int flags, PCVOID address)
    __asm__("sock_sendto");
extern int  kernel_sock_recvfrom(int socket, PVOID data, uint32_t length,
                                 int flags, PVOID address,
                                 uint32_t *address_length)
    __asm__("sock_recvfrom");
extern int  kernel_sock_close(int socket) __asm__("sock_close");
extern int  kernel_sock_pending_bytes(int socket)
    __asm__("sock_pending_bytes");
extern int  kernel_sock_has_pending_data(int socket)
    __asm__("sock_has_pending_data");
extern int  kernel_sock_set_recv_buffer(int socket, uint32_t size)
    __asm__("sock_set_recv_buffer");
extern void net_poll(void);
extern uint8_t *net_get_ip_ptr(void);
extern uint8_t *net_get_netmask_ptr(void);
extern uint8_t *net_get_gateway_ptr(void);
extern void net_get_mac(uint8_t mac_out[6]);

/* TCP states from net.h */
#define KERN_TCP_CLOSED      0
#define KERN_TCP_SYN_SENT    1
#define KERN_TCP_ESTABLISHED 2
#define KERN_TCP_CLOSE_WAIT  5
#define KERN_TCP_LAST_ACK    6
#define KERN_TCP_TIME_WAIT   7
#define KERN_TCP_ERROR_NONE      0
#define KERN_TCP_ERROR_RESET     1
#define KERN_TCP_ERROR_TIMED_OUT 2

/* ── Additional Winsock error codes ──────────────────────── */
#define WSAEWOULDBLOCK  10035
#define WSAEMSGSIZE      10040
#define WSAEMFILE       10024
#define WSAENOTSOCK     10038
#define WSAEFAULT       10014
#define WSAENOTCONN     10057
#define WSAESHUTDOWN    10058
#define WSAECONNREFUSED 10061
#define WSAECONNRESET   10054
#define WSAEADDRINUSE   10048
#define WSAEADDRNOTAVAIL 10049
#define WSAEACCES        10013
#define WSAENOBUFS      10055
#define WSAEPROTONOSUPPORT 10043
#define WSAESOCKTNOSUPPORT 10044
#define WSAEOPNOTSUPP    10045
#define WSAEAFNOSUPPORT  10047
#define WSAEINVAL        10022
#define WSAEINPROGRESS   10036
#define WSAEALREADY      10037
#define WSAENOPROTOOPT   10042
#define WSAEISCONN       10056
#define WSAENAMETOOLONG  10063
#define WSASYSNOTREADY   10091
#define WSAENOMORE       10102
#define WSASERVICE_NOT_FOUND 10108
#define WSA_E_NO_MORE    10110
#define WSATYPE_NOT_FOUND 10109
#define WSAHOST_NOT_FOUND 11001
#define WSA_IO_PENDING       997
#define SIO_KEEPALIVE_VALS 0x98000004U

#define NS_ALL            0
#define NS_DNS           12
#define NS_NLA           15

#define LUP_RETURN_NAME         0x0010U
#define LUP_RETURN_TYPE         0x0020U
#define LUP_RETURN_COMMENT      0x0080U
#define LUP_RETURN_BLOB         0x0200U
#define LUP_RETURN_ALL          0x0FF0U
#define LUP_FLUSHPREVIOUS       0x1000U

#define NI_NOFQDN       0x01
#define NI_NUMERICHOST  0x02
#define NI_NAMEREQD     0x04
#define NI_NUMERICSERV  0x08
#define NI_DGRAM        0x10

#define SOL_SOCKET       0xFFFF
#define SO_DEBUG         0x0001
#define SO_ACCEPTCONN    0x0002
#define SO_REUSEADDR     0x0004
#define SO_KEEPALIVE     0x0008
#define SO_DONTROUTE     0x0010
#define SO_BROADCAST     0x0020
#define SO_LINGER        0x0080
#define SO_OOBINLINE     0x0100
#define SO_EXCLUSIVEADDRUSE ((int)~SO_REUSEADDR)
#define SO_DONTLINGER    ((int)~SO_LINGER)
#define SO_SNDBUF        0x1001
#define SO_RCVBUF        0x1002
#define SO_SNDLOWAT      0x1003
#define SO_RCVLOWAT      0x1004
#define SO_SNDTIMEO      0x1005
#define SO_RCVTIMEO      0x1006
#define SO_ERROR         0x1007
#define SO_TYPE          0x1008
#define SO_MAX_MSG_SIZE  0x2003
#define SO_RANDOMIZE_PORT 0x3005
#define SO_UPDATE_ACCEPT_CONTEXT  0x700B
#define SO_CONNECT_TIME  0x700C
#define SO_UPDATE_CONNECT_CONTEXT 0x7010
#define TCP_NODELAY      0x0001

#define WSOCK_FIONBIO    0x8004667EU
#define WSOCK_FIONREAD   0x4004667FU
#define WSOCK_SIOCATMARK 0x40047307U
#define WSOCK_MSG_OOB     0x0001
#define WSOCK_MSG_PEEK    0x0002
#define WSOCK_MSG_WAITALL 0x0008
#define WSOCK_MSG_PARTIAL 0x8000
#define WSOCK_TCP_SEND_CAPACITY 16384
#define WSOCK_TCP_RECV_CAPACITY 131072

#define WSOCK_FD_READ       0x01
#define WSOCK_FD_WRITE      0x02
#define WSOCK_FD_OOB        0x04
#define WSOCK_FD_ACCEPT     0x08
#define WSOCK_FD_CONNECT    0x10
#define WSOCK_FD_CLOSE      0x20
#define WSOCK_FD_MAX_EVENTS 10
#define WSOCK_WAIT_TIMEOUT  0x00000102U
#define WSOCK_WAIT_FAILED   0xFFFFFFFFU
#define WSOCK_INFINITE      0xFFFFFFFFU

/* ── Winsock state ───────────────────────────────────────── */

/* ── Socket table ────────────────────────────────────────── */

#define MAX_WIN32_SOCKETS 256
#define BASE_SOCKET       0x100   /* offset so SOCKET values don't collide with stdin/stdout */

#define AF_INET     2
#define AF_INET6   23
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define IPPROTO_IPV6 41
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPV6_V6ONLY 27

typedef struct {
    int     in_use;
    int     af;
    int     type;       /* SOCK_STREAM or SOCK_DGRAM */
    int     protocol;
    int     kern_conn_id;   /* kernel TCP connection index, or -1 */
    int     kern_listener_id; /* kernel TCP listener index, or -1 */
    int     kern_socket_id;  /* kernel BSD endpoint for datagrams, or -1 */
    int     connected;
    int     connecting;
    int     listening;
    int     nonblocking;
    uint8_t remote_ip[4];
    uint8_t local_ip[4];
    uint16_t remote_port;
    uint16_t local_port;
    HANDLE  event_handle;
    LONG    event_mask;
    LONG    event_pending;
    LONG    event_ready_mask;
    int     pending_error;
    int     send_buffer_size;
    int     recv_buffer_size;
    int     send_timeout_ms;
    int     recv_timeout_ms;
    int     send_low_water;
    int     recv_low_water;
    DWORD   connected_at_ms;
    DWORD   generation;
    DWORD   option_flags;
    DWORD   keepalive_time_ms;
    DWORD   keepalive_interval_ms;
    USHORT  linger_seconds;
    BYTE    linger_enabled;
    BYTE    no_delay;
    BYTE    accept_context_updated;
    BYTE    connect_context_updated;
    BYTE    shutdown_receive;
    BYTE    shutdown_send;
} win32_socket_t;

#define WSOCK_OPT_DEBUG          0x0001U
#define WSOCK_OPT_REUSEADDR      0x0002U
#define WSOCK_OPT_KEEPALIVE      0x0004U
#define WSOCK_OPT_DONTROUTE      0x0008U
#define WSOCK_OPT_BROADCAST      0x0010U
#define WSOCK_OPT_OOBINLINE      0x0020U
#define WSOCK_OPT_EXCLUSIVEADDR  0x0040U
#define WSOCK_OPT_IPV6ONLY       0x0080U
#define WSOCK_OPT_RANDOMIZE_PORT 0x0100U

typedef struct {
    LONG network_events;
    int  error_codes[WSOCK_FD_MAX_EVENTS];
} WSOCK_NETWORK_EVENTS;

#define MAX_WSOCK_PROCESS_STATES 256

typedef struct {
    BOOL used;
    DWORD pid;
    int startup_refs;
    int last_error;
    win32_socket_t sockets[MAX_WIN32_SOCKETS];
} wsock_process_state_t;

typedef struct {
    int32_t chain_length;
    DWORD chain_entries[7];
} wsock_protocol_chain_t;

typedef struct {
    DWORD service_flags1;
    DWORD service_flags2;
    DWORD service_flags3;
    DWORD service_flags4;
    DWORD provider_flags;
    BYTE provider_id[16];
    DWORD catalog_entry_id;
    wsock_protocol_chain_t protocol_chain;
    int32_t version;
    int32_t address_family;
    int32_t max_sockaddr;
    int32_t min_sockaddr;
    int32_t socket_type;
    int32_t protocol;
    int32_t protocol_max_offset;
    int32_t network_byte_order;
    int32_t security_scheme;
    DWORD message_size;
    DWORD provider_reserved;
} wsock_protocol_info_header_t;

typedef struct {
    wsock_protocol_info_header_t header;
    WCHAR protocol_name[256];
} wsock_protocol_info_w_t;

_Static_assert(sizeof(wsock_protocol_chain_t) == 32,
               "WSAPROTOCOLCHAIN layout");
_Static_assert(sizeof(wsock_protocol_info_header_t) == 116,
               "WSAPROTOCOL_INFO header layout");
_Static_assert(sizeof(wsock_protocol_info_w_t) == 628,
               "WSAPROTOCOL_INFOW layout");

#define MAX_WSOCK_DUPLICATIONS 128

typedef struct {
    volatile LONG state; /* 0=free, 1=ready, 2=being populated/consumed */
    DWORD token;
    DWORD source_pid;
    DWORD target_pid;
    win32_socket_t socket;
} wsock_duplication_t;

static wsock_duplication_t g_wsock_duplications[MAX_WSOCK_DUPLICATIONS];
static volatile DWORD g_wsock_duplication_token;

static wsock_process_state_t g_wsock_process_states[MAX_WSOCK_PROCESS_STATES];
static volatile LONG g_wsock_process_states_lock;
static volatile LONG g_wsock_socket_lifecycle_lock;
static volatile LONG g_wsock_event_worker_started;
static volatile DWORD g_wsock_socket_generation;
static volatile DWORD g_ephemeral_sequence;

static int wsock_apply_keepalive(win32_socket_t *ws);
static void wsock_inherit_listener_options(win32_socket_t *accepted,
                                           const win32_socket_t *listener);
static void wsock_rearm_socket_event(win32_socket_t *ws, LONG events);
static void wsock_refresh_socket_event_for_process(win32_socket_t *ws,
                                                    DWORD owner_pid);

#define MAX_PENDING_ACCEPTEX 256
#define MAX_PENDING_CONNECTEX 256
#define MAX_PENDING_WSARECV 256
#define MAX_WSARECV_BUFFERS 64
#define WSOCK_CONNECT_TIMEOUT_MS 60000ULL

typedef struct {
    volatile LONG state; /* 0=free, 1=pending, 2=servicing */
    DWORD owner_pid;
    DWORD owner_tid;
    BOOL compat32;
    SOCKET listen_socket;
    SOCKET accept_socket;
    DWORD listen_generation;
    DWORD accept_generation;
    PVOID output;
    DWORD receive_length;
    DWORD local_length;
    DWORD remote_length;
    PVOID overlapped;
    int conn_id;
} wsock_pending_accept_t;

static wsock_pending_accept_t g_pending_accepts[MAX_PENDING_ACCEPTEX];

typedef struct {
    volatile LONG state; /* 0=free, 1=pending, 2=servicing */
    DWORD owner_pid;
    DWORD owner_tid;
    BOOL compat32;
    SOCKET socket;
    DWORD socket_generation;
    PVOID send_buffer;
    DWORD send_length;
    PVOID overlapped;
    int conn_id;
    ULONGLONG deadline_ms;
} wsock_pending_connect_t;

static wsock_pending_connect_t g_pending_connects[MAX_PENDING_CONNECTEX];

typedef struct {
    PVOID data;
    uint32_t length;
} wsock_captured_buffer_t;

typedef struct {
    volatile LONG state; /* 0=free, 1=pending, 2=servicing, 3=reserved */
    ULONGLONG sequence;
    DWORD owner_pid;
    DWORD owner_tid;
    BOOL compat32;
    SOCKET socket;
    DWORD socket_generation;
    BOOL datagram;
    DWORD count;
    wsock_captured_buffer_t buffers[MAX_WSARECV_BUFFERS];
    DWORD input_flags;
    PVOID from;
    int *fromlen;
    PVOID overlapped;
} wsock_pending_recv_t;

static wsock_pending_recv_t g_pending_recvs[MAX_PENDING_WSARECV];
static volatile ULONGLONG g_pending_recv_next_sequence;
static volatile LONG g_pending_recv_service_lock;

typedef struct {
    uint32_t len;
    uint32_t buf;
} WSABUF32;

typedef struct {
    uint32_t len;
    uint32_t padding;
    PVOID    buf;
} WSABUF64;

typedef struct {
    uint32_t name;
    int32_t namelen;
    uint32_t buffers;
    DWORD buffer_count;
    WSABUF32 control;
    DWORD flags;
} WSAMSG32;

typedef struct {
    PVOID name;
    int32_t namelen;
    uint32_t padding0;
    PVOID buffers;
    DWORD buffer_count;
    DWORD padding1;
    WSABUF64 control;
    DWORD flags;
    DWORD padding2;
} WSAMSG64;

typedef struct {
    PVOID name;
    int *namelen;
    PCVOID buffers;
    DWORD buffer_count;
    PVOID control;
    DWORD *control_length;
    DWORD *flags;
} wsock_message_view_t;

typedef struct {
    uint32_t count;
    uint32_t sockets[64];
} FDSET32;

typedef struct {
    uint32_t count;
    uint32_t padding;
    uint64_t sockets[64];
} FDSET64;

typedef struct {
    int32_t  ai_flags;
    int32_t  ai_family;
    int32_t  ai_socktype;
    int32_t  ai_protocol;
    uint32_t ai_addrlen;
    uint32_t ai_canonname;
    uint32_t ai_addr;
    uint32_t ai_next;
} ADDRINFO32;

typedef struct {
    int32_t ai_flags;
    int32_t ai_family;
    int32_t ai_socktype;
    int32_t ai_protocol;
    SIZE_T  ai_addrlen;
    PCSTR   ai_canonname;
    PVOID   ai_addr;
    PVOID   ai_next;
} ADDRINFO64;

typedef struct {
    DWORD dwSize;
    uint32_t lpszServiceInstanceName;
    uint32_t lpServiceClassId;
    uint32_t lpVersion;
    uint32_t lpszComment;
    DWORD dwNameSpace;
    uint32_t lpNSProviderId;
    uint32_t lpszContext;
    DWORD dwNumberOfProtocols;
    uint32_t lpafpProtocols;
    uint32_t lpszQueryString;
    DWORD dwNumberOfCsAddrs;
    uint32_t lpcsaBuffer;
    DWORD dwOutputFlags;
    uint32_t lpBlob;
} WSAQUERYSETW32;

typedef struct {
    DWORD dwSize;
    DWORD padding0;
    uint64_t lpszServiceInstanceName;
    uint64_t lpServiceClassId;
    uint64_t lpVersion;
    uint64_t lpszComment;
    DWORD dwNameSpace;
    DWORD padding1;
    uint64_t lpNSProviderId;
    uint64_t lpszContext;
    DWORD dwNumberOfProtocols;
    DWORD padding2;
    uint64_t lpafpProtocols;
    uint64_t lpszQueryString;
    DWORD dwNumberOfCsAddrs;
    DWORD padding3;
    uint64_t lpcsaBuffer;
    DWORD dwOutputFlags;
    DWORD padding4;
    uint64_t lpBlob;
} WSAQUERYSETW64;

typedef struct {
    GUID provider_id;
    DWORD name_space;
    BOOL active;
    DWORD version;
    uint32_t identifier;
} WSOCK_NAMESPACE_INFO32;

typedef struct {
    GUID provider_id;
    DWORD name_space;
    BOOL active;
    DWORD version;
    DWORD padding;
    uint64_t identifier;
} WSOCK_NAMESPACE_INFO64;

typedef struct {
    DWORD size;
    uint32_t data;
} WSOCK_BLOB32;

typedef struct {
    DWORD size;
    DWORD padding;
    uint64_t data;
} WSOCK_BLOB64;

typedef struct {
    DWORD type;
    DWORD size;
    DWORD next_offset;
    DWORD connectivity_type;
    DWORD internet;
} WSOCK_NLA_BLOB;

typedef struct {
    volatile LONG state;
    DWORD token;
    DWORD owner_pid;
    DWORD begin_flags;
    BOOL compat32;
    BOOL returned;
} wsock_lookup_t;

#define MAX_WSOCK_LOOKUPS 32
static wsock_lookup_t g_wsock_lookups[MAX_WSOCK_LOOKUPS];
static volatile DWORD g_wsock_lookup_token;

static const GUID g_wsock_provider_id = {
    0x9E0D33B7U, 0x8A58U, 0x4D4BU,
    { 0xA1, 0x6B, 0x4F, 0x53, 0x49, 0x54, 0x4F, 0x4B }
};
static const GUID g_wsock_dns_namespace_id = {
    0x9E0D33B8U, 0x8A58U, 0x4D4BU,
    { 0xA1, 0x6B, 0x4F, 0x53, 0x49, 0x54, 0x4F, 0x4B }
};
static const GUID g_wsock_nla_namespace_id = {
    0x6642243AU, 0x3BA8U, 0x4AA6U,
    { 0xBA, 0xA5, 0x2E, 0x0B, 0xD7, 0x1F, 0xDD, 0x83 }
};
static const GUID g_wsock_nla_service_class = {
    0x0037E515U, 0xB5C9U, 0x4A43U,
    { 0xBA, 0xDA, 0x8B, 0x48, 0xA8, 0x7A, 0xD2, 0x39 }
};
static const GUID g_wsock_wsa_recv_msg_id = {
    0xF689D7C8U, 0x6F1FU, 0x436BU,
    { 0x8A, 0x53, 0xE5, 0x4F, 0xE3, 0x51, 0xC3, 0x22 }
};
static const GUID g_wsock_wsa_send_msg_id = {
    0xA441E712U, 0x754FU, 0x43CAU,
    { 0x84, 0xA7, 0x0D, 0xEE, 0x44, 0xCF, 0x60, 0x6D }
};

_Static_assert(sizeof(WSABUF32) == 8, "PE32 WSABUF layout");
_Static_assert(sizeof(WSABUF64) == 16, "PE64 WSABUF layout");
_Static_assert(sizeof(WSAMSG32) == 28, "PE32 WSAMSG layout");
_Static_assert(sizeof(WSAMSG64) == 56, "PE64 WSAMSG layout");
_Static_assert(sizeof(FDSET32) == 260, "PE32 fd_set layout");
_Static_assert(sizeof(FDSET64) == 520, "PE64 fd_set layout");
_Static_assert(sizeof(ADDRINFO32) == 32, "PE32 addrinfo layout");
_Static_assert(sizeof(ADDRINFO64) == 48, "PE64 addrinfo layout");
_Static_assert(sizeof(WSAQUERYSETW32) == 60, "PE32 WSAQUERYSETW layout");
_Static_assert(sizeof(WSAQUERYSETW64) == 120, "PE64 WSAQUERYSETW layout");
_Static_assert(sizeof(WSOCK_NAMESPACE_INFO32) == 32,
               "PE32 WSANAMESPACE_INFOW layout");
_Static_assert(sizeof(WSOCK_NAMESPACE_INFO64) == 40,
               "PE64 WSANAMESPACE_INFOW layout");
_Static_assert(sizeof(WSOCK_BLOB32) == 8, "PE32 BLOB layout");
_Static_assert(sizeof(WSOCK_BLOB64) == 16, "PE64 BLOB layout");
_Static_assert(sizeof(WSOCK_NLA_BLOB) == 20, "NLA_BLOB connectivity layout");

static uint32_t wsabuf_len_for_mode(PCVOID buffers, DWORD index,
                                    BOOL compat32)
{
    return compat32 ? ((const WSABUF32 *)buffers)[index].len
                    : ((const WSABUF64 *)buffers)[index].len;
}

static PVOID wsabuf_data_for_mode(PCVOID buffers, DWORD index, BOOL compat32)
{
    return compat32
        ? (PVOID)(uintptr_t)((const WSABUF32 *)buffers)[index].buf
        : ((const WSABUF64 *)buffers)[index].buf;
}

static void wsabuf_set_len_for_mode(PVOID buffers, DWORD index,
                                    BOOL compat32, uint32_t length)
{
    if (compat32)
        ((WSABUF32 *)buffers)[index].len = length;
    else
        ((WSABUF64 *)buffers)[index].len = length;
}

static int wsock_message_view(PVOID message, BOOL compat32,
                              wsock_message_view_t *view)
{
    if (!message || !view) return WSAEFAULT;
    if (compat32) {
        WSAMSG32 *value = (WSAMSG32 *)message;
        view->name = (PVOID)(uintptr_t)value->name;
        view->namelen = (int *)&value->namelen;
        view->buffers = (PCVOID)(uintptr_t)value->buffers;
        view->buffer_count = value->buffer_count;
        view->control = (PVOID)(uintptr_t)value->control.buf;
        view->control_length = &value->control.len;
        view->flags = &value->flags;
    } else {
        WSAMSG64 *value = (WSAMSG64 *)message;
        view->name = value->name;
        view->namelen = (int *)&value->namelen;
        view->buffers = value->buffers;
        view->buffer_count = value->buffer_count;
        view->control = value->control.buf;
        view->control_length = &value->control.len;
        view->flags = &value->flags;
    }
    return 0;
}

static uint32_t wsabuf_len(PCVOID buffers, DWORD index)
{
    return wsabuf_len_for_mode(buffers, index, g_compat32_mode);
}

static PVOID wsabuf_data(PCVOID buffers, DWORD index)
{
    return wsabuf_data_for_mode(buffers, index, g_compat32_mode);
}

static uint32_t fdset_count(PCVOID set)
{
    return g_compat32_mode ? ((const FDSET32 *)set)->count
                           : ((const FDSET64 *)set)->count;
}

static SOCKET fdset_socket(PCVOID set, uint32_t index)
{
    return g_compat32_mode
        ? (SOCKET)((const FDSET32 *)set)->sockets[index]
        : (SOCKET)((const FDSET64 *)set)->sockets[index];
}

static void fdset_store(PVOID set, uint32_t index, SOCKET socket)
{
    if (g_compat32_mode)
        ((FDSET32 *)set)->sockets[index] = (uint32_t)socket;
    else
        ((FDSET64 *)set)->sockets[index] = (uint64_t)socket;
}

static void fdset_set_count(PVOID set, uint32_t count)
{
    if (g_compat32_mode)
        ((FDSET32 *)set)->count = count;
    else
        ((FDSET64 *)set)->count = count;
}

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

#if !defined(OK_QUIET) || !OK_QUIET
static volatile LONG g_loopback_trace_budget = 128;
static volatile LONG g_loopback_control_trace_budget = 256;
#endif

static BOOL wsock_trace_loopback_socket(const win32_socket_t *socket)
{
    if (!socket || socket->type != SOCK_STREAM)
        return FALSE;
    return socket->remote_ip[0] == 127 || socket->local_ip[0] == 127 ||
           socket->local_port == 49155 || socket->remote_port == 49155 ||
           socket->local_port == 49191 || socket->remote_port == 49191;
}

static BOOL wsock_trace_loopback_control(const win32_socket_t *socket)
{
#if defined(OK_QUIET) && OK_QUIET
    (void)socket;
    return FALSE;
#else
    if (!wsock_trace_loopback_socket(socket))
        return FALSE;
    return __atomic_fetch_sub(&g_loopback_control_trace_budget, 1,
                              __ATOMIC_RELAXED) > 0;
#endif
}

static void wsock_trace_loopback_payload(const char *direction,
                                         const win32_socket_t *socket,
                                         const void *data, DWORD length)
{
#if defined(OK_QUIET) && OK_QUIET
    (void)direction;
    (void)socket;
    (void)data;
    (void)length;
#else
    static const char digits[] = "0123456789ABCDEF";
    char hex[192 * 2 + 1];
    char ascii[192 + 1];
    DWORD shown;

    if (!data || !length || !wsock_trace_loopback_socket(socket))
        return;
    if (__atomic_fetch_sub(&g_loopback_trace_budget, 1, __ATOMIC_RELAXED) <= 0)
        return;

    shown = length > 192 ? 192 : length;
    for (DWORD i = 0; i < shown; i++) {
        BYTE value = ((const BYTE *)data)[i];
        hex[i * 2] = digits[value >> 4];
        hex[i * 2 + 1] = digits[value & 15];
        ascii[i] = value >= 32 && value < 127 ? (char)value : '.';
    }
    hex[shown * 2] = 0;
    ascii[shown] = 0;

    serial_puts("[WSOCK-DATA] ");
    serial_puts(direction);
    serial_puts(" pid=");
    serial_putdec(win32_current_process_id());
    serial_puts(" local=");
    serial_putdec(socket->local_port);
    serial_puts(" remote=");
    serial_putdec(socket->remote_port);
    serial_puts(" len=");
    serial_putdec(length);
    serial_puts(" hex=");
    serial_puts(hex);
    serial_puts(" ascii='");
    serial_puts(ascii);
    serial_puts("'\n");
#endif
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

static int ws_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return (int)ca - (int)cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void wsock_process_states_acquire(void)
{
    while (__sync_lock_test_and_set(&g_wsock_process_states_lock, 1))
        __asm__ volatile ("pause");
}

static void wsock_process_states_release(void)
{
    __sync_lock_release(&g_wsock_process_states_lock);
}

static void wsock_socket_lifecycle_acquire(void)
{
    while (__sync_lock_test_and_set(&g_wsock_socket_lifecycle_lock, 1))
        __asm__ volatile ("pause");
}

static void wsock_socket_lifecycle_release(void)
{
    __sync_lock_release(&g_wsock_socket_lifecycle_lock);
}

static wsock_process_state_t *wsock_state_current(void)
{
    DWORD pid = win32_current_process_id();

    for (int i = 0; i < MAX_WSOCK_PROCESS_STATES; i++) {
        wsock_process_state_t *state = &g_wsock_process_states[i];
        if (__atomic_load_n(&state->used, __ATOMIC_ACQUIRE) &&
            state->pid == pid)
            return state;
    }

    wsock_process_states_acquire();
    wsock_process_state_t *free_state = NULL;
    for (int i = 0; i < MAX_WSOCK_PROCESS_STATES; i++) {
        wsock_process_state_t *state = &g_wsock_process_states[i];
        if (state->used && state->pid == pid) {
            wsock_process_states_release();
            return state;
        }
        if (!state->used && !free_state)
            free_state = state;
    }

    if (free_state) {
        ws_memset(free_state, 0, sizeof(*free_state));
        free_state->pid = pid;
        free_state->last_error = WSANOTINITIALISED;
        for (int i = 0; i < MAX_WIN32_SOCKETS; i++) {
            free_state->sockets[i].kern_conn_id = -1;
            free_state->sockets[i].kern_listener_id = -1;
            free_state->sockets[i].kern_socket_id = -1;
        }
        __atomic_store_n(&free_state->used, TRUE, __ATOMIC_RELEASE);
    }
    wsock_process_states_release();

    if (!free_state) {
        /* The kernel currently permits at most 256 processes. */
        serial_puts("[WSOCK] process state table exhausted\n");
        return &g_wsock_process_states[0];
    }
    return free_state;
}

static wsock_process_state_t *wsock_state_for_pid(DWORD pid)
{
    for (int i = 0; i < MAX_WSOCK_PROCESS_STATES; i++) {
        wsock_process_state_t *state = &g_wsock_process_states[i];
        if (__atomic_load_n(&state->used, __ATOMIC_ACQUIRE) &&
            state->pid == pid)
            return state;
    }
    return NULL;
}

static BOOL wsock_same_backend(const win32_socket_t *left,
                               const win32_socket_t *right)
{
    return left && right && left->generation &&
           left->generation == right->generation;
}

/* Caller holds g_wsock_process_states_lock. */
static BOOL wsock_backend_has_reference(const win32_socket_t *socket)
{
    for (int i = 0; i < MAX_WSOCK_PROCESS_STATES; i++) {
        wsock_process_state_t *state = &g_wsock_process_states[i];
        if (!state->used) continue;
        for (int j = 0; j < MAX_WIN32_SOCKETS; j++) {
            win32_socket_t *candidate = &state->sockets[j];
            if (candidate->in_use && wsock_same_backend(socket, candidate))
                return TRUE;
        }
    }
    for (int i = 0; i < MAX_WSOCK_DUPLICATIONS; i++) {
        wsock_duplication_t *duplicate = &g_wsock_duplications[i];
        if (duplicate->state && duplicate->socket.in_use &&
            wsock_same_backend(socket, &duplicate->socket))
            return TRUE;
    }
    return FALSE;
}

static void wsock_clear_handle_associations(win32_socket_t *socket)
{
    socket->event_handle = NULL;
    socket->event_mask = 0;
    socket->event_pending = 0;
    socket->event_ready_mask = 0;
}

static void wsock_copy_shared_state(win32_socket_t *destination,
                                    const win32_socket_t *source)
{
    HANDLE event_handle = destination->event_handle;
    LONG event_mask = destination->event_mask;
    LONG event_pending = destination->event_pending;
    LONG event_ready_mask = destination->event_ready_mask;
    int in_use = destination->in_use;

    *destination = *source;
    destination->in_use = in_use;
    destination->event_handle = event_handle;
    destination->event_mask = event_mask;
    destination->event_pending = event_pending;
    destination->event_ready_mask = event_ready_mask;
}

static void wsock_propagate_shared_state(win32_socket_t *source)
{
    wsock_process_states_acquire();
    for (int i = 0; i < MAX_WSOCK_PROCESS_STATES; i++) {
        wsock_process_state_t *state = &g_wsock_process_states[i];
        if (!state->used) continue;
        for (int j = 0; j < MAX_WIN32_SOCKETS; j++) {
            win32_socket_t *candidate = &state->sockets[j];
            if (candidate != source && candidate->in_use &&
                wsock_same_backend(source, candidate))
                wsock_copy_shared_state(candidate, source);
        }
    }
    for (int i = 0; i < MAX_WSOCK_DUPLICATIONS; i++) {
        wsock_duplication_t *duplicate = &g_wsock_duplications[i];
        if (duplicate->state && duplicate->socket.in_use &&
            wsock_same_backend(source, &duplicate->socket)) {
            duplicate->socket = *source;
            wsock_clear_handle_associations(&duplicate->socket);
        }
    }
    wsock_process_states_release();
}

static void wsock_fill_protocol_info(const win32_socket_t *socket,
                                     DWORD token,
                                     wsock_protocol_info_w_t *info)
{
    static const char tcp_name[] = "OsitoK TCP/IPv4";
    static const char udp_name[] = "OsitoK UDP/IPv4";
    const char *protocol_name = socket->type == SOCK_STREAM
                              ? tcp_name : udp_name;

    ws_memset(info, 0, sizeof(*info));
    info->header.service_flags1 = socket->type == SOCK_STREAM
        ? 0x00000026U /* delivery, order, graceful close */
        : 0x00000209U; /* connectionless, message-oriented, broadcast */
    info->header.provider_flags = 0x00000008U; /* protocol=0 may select it */
    ws_memcpy(info->header.provider_id, &g_wsock_provider_id,
              sizeof(g_wsock_provider_id));
    info->header.catalog_entry_id = socket->type == SOCK_STREAM ? 1 : 2;
    info->header.protocol_chain.chain_length = 1;
    info->header.protocol_chain.chain_entries[0] =
        info->header.catalog_entry_id;
    info->header.version = 2;
    info->header.address_family = socket->af;
    info->header.max_sockaddr = socket->af == AF_INET6 ? 28 : 16;
    info->header.min_sockaddr = info->header.max_sockaddr;
    info->header.socket_type = socket->type;
    info->header.protocol = socket->protocol;
    info->header.network_byte_order = 0;
    info->header.message_size = socket->type == SOCK_DGRAM ? 65507U : 0;
    info->header.provider_reserved = token;
    for (int i = 0; protocol_name[i] && i < 255; i++)
        info->protocol_name[i] = (WCHAR)(BYTE)protocol_name[i];
}

static DWORD wsock_next_duplication_token(void)
{
    DWORD sequence = __atomic_add_fetch(&g_wsock_duplication_token, 1,
                                        __ATOMIC_RELAXED) & 0x00FFFFFFU;
    if (!sequence)
        sequence = __atomic_add_fetch(&g_wsock_duplication_token, 1,
                                      __ATOMIC_RELAXED) & 0x00FFFFFFU;
    return 0x4F000000U | sequence;
}

static int wsock_raw_index(SOCKET socket)
{
    int index = (int)(socket - BASE_SOCKET);
    return index >= 0 && index < MAX_WIN32_SOCKETS ? index : -1;
}

static void wsock_set_overlapped_status(PVOID overlapped, BOOL compat32,
                                         NTSTATUS status, DWORD bytes)
{
    if (!overlapped) return;
    if (compat32) {
        volatile uint32_t *values = (volatile uint32_t *)overlapped;
        values[0] = (uint32_t)status;
        values[1] = bytes;
    } else {
        volatile ULONG_PTR *values = (volatile ULONG_PTR *)overlapped;
        values[0] = (ULONG_PTR)status;
        values[1] = bytes;
    }
}

static void wsock_store_sockaddr(uint8_t *sa, const uint8_t ip[4],
                                  uint16_t port)
{
    ws_memset(sa, 0, 16);
    sa[0] = AF_INET;
    sa[2] = (uint8_t)(port >> 8);
    sa[3] = (uint8_t)port;
    ws_memcpy(sa + 4, ip, 4);
}

static int wsock_sockaddr_length(const win32_socket_t *socket)
{
    return socket->af == AF_INET6 ? 28 : 16;
}

static void wsock_store_peer_sockaddr(const win32_socket_t *socket,
                                      uint8_t *destination,
                                      const uint8_t ip[4], uint16_t port)
{
    if (socket->af != AF_INET6) {
        wsock_store_sockaddr(destination, ip, port);
        return;
    }

    ws_memset(destination, 0, 28);
    destination[0] = AF_INET6;
    destination[2] = (uint8_t)(port >> 8);
    destination[3] = (uint8_t)port;
    if (ip[0] || ip[1] || ip[2] || ip[3]) {
        destination[18] = 0xFF;
        destination[19] = 0xFF;
        ws_memcpy(destination + 20, ip, 4);
    }
}

static int wsock_capture_recv_buffers(
    PCVOID buffers, DWORD count, BOOL compat32,
    wsock_captured_buffer_t captured[MAX_WSARECV_BUFFERS],
    DWORD *total_capacity)
{
    uint64_t capacity = 0;

    for (DWORD i = 0; i < count; i++) {
        captured[i].length = wsabuf_len_for_mode(buffers, i, compat32);
        captured[i].data = wsabuf_data_for_mode(buffers, i, compat32);
        if (captured[i].length && !captured[i].data)
            return WSAEFAULT;
        capacity += captured[i].length;
        if (capacity > 0xFFFFFFFFULL)
            return WSAEMSGSIZE;
    }
    *total_capacity = (DWORD)capacity;
    return 0;
}

static int wsock_recv_datagram_captured_now(
    win32_socket_t *socket, const wsock_captured_buffer_t *buffers,
    DWORD count, DWORD total_capacity, DWORD input_flags, PVOID from,
    int *fromlen, DWORD *bytes_received, DWORD *output_flags)
{
    int pending = kernel_sock_has_pending_data(socket->kern_socket_id);
    if (pending <= 0)
        return pending < 0 ? WSAENOTSOCK : WSAEWOULDBLOCK;

    int datagram_length = kernel_sock_pending_bytes(socket->kern_socket_id);
    if (datagram_length < 0)
        return WSAENOTSOCK;
    if (datagram_length > 65507)
        return WSAENETDOWN;

    BYTE empty_datagram;
    BYTE *datagram = &empty_datagram;
    if (datagram_length) {
        datagram = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
                                     (SIZE_T)datagram_length);
        if (!datagram)
            return WSAENOBUFS;
    }

    uint8_t endpoint[16];
    uint32_t endpoint_length = sizeof(endpoint);
    int received = kernel_sock_recvfrom(
        socket->kern_socket_id, datagram, (uint32_t)datagram_length,
        (int)(input_flags & WSOCK_MSG_PEEK), endpoint, &endpoint_length);
    if (received < 0) {
        if (datagram_length) HeapFree(GetProcessHeap(), 0, datagram);
        return received == -11 ? WSAEWOULDBLOCK : WSAENETDOWN;
    }

    DWORD copied = (DWORD)received;
    if (copied > total_capacity) copied = total_capacity;
    DWORD source_offset = 0;
    for (DWORD i = 0; i < count && source_offset < copied; i++) {
        DWORD amount = buffers[i].length;
        if (amount > copied - source_offset)
            amount = copied - source_offset;
        if (amount) {
            ws_memcpy(buffers[i].data, datagram + source_offset, amount);
            source_offset += amount;
        }
    }

    if (from) {
        uint16_t source_port = (uint16_t)((uint16_t)endpoint[2] << 8) |
                               endpoint[3];
        wsock_store_peer_sockaddr(socket, (uint8_t *)from, endpoint + 4,
                                  source_port);
        *fromlen = wsock_sockaddr_length(socket);
    }
    if (datagram_length) HeapFree(GetProcessHeap(), 0, datagram);

    BOOL truncated = (DWORD)received > total_capacity;
    if (bytes_received) *bytes_received = copied;
    if (output_flags) *output_flags = truncated ? WSOCK_MSG_PARTIAL : 0;
    wsock_rearm_socket_event(socket, WSOCK_FD_READ);
    return truncated ? WSAEMSGSIZE : 0;
}

static void wsock_fill_acceptex_addresses(const wsock_pending_accept_t *pending,
                                           const win32_socket_t *listener,
                                           const win32_socket_t *accepted)
{
    if (!pending->output) return;
    uint8_t *base = (uint8_t *)pending->output;
    if (pending->local_length >= 32)
        wsock_store_sockaddr(base + pending->receive_length + 16,
                             listener->local_ip, listener->local_port);
    if (pending->remote_length >= 32)
        wsock_store_sockaddr(base + pending->receive_length +
                             pending->local_length + 16,
                             accepted->remote_ip, accepted->remote_port);
}

static void wsock_service_pending_accepts(DWORD owner_pid)
{
    for (int i = 0; i < MAX_PENDING_ACCEPTEX; i++) {
        wsock_pending_accept_t *pending = &g_pending_accepts[i];
        if (__atomic_load_n(&pending->state, __ATOMIC_ACQUIRE) != 1 ||
            pending->owner_pid != owner_pid)
            continue;
        LONG expected = 1;
        if (!__atomic_compare_exchange_n(&pending->state, &expected, 2, FALSE,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;

        wsock_process_state_t *owner = wsock_state_for_pid(pending->owner_pid);
        int listen_idx = wsock_raw_index(pending->listen_socket);
        int accept_idx = wsock_raw_index(pending->accept_socket);
        if (!owner || listen_idx < 0 || accept_idx < 0 ||
            !owner->sockets[listen_idx].in_use ||
            !owner->sockets[accept_idx].in_use ||
            owner->sockets[listen_idx].generation !=
                pending->listen_generation ||
            owner->sockets[accept_idx].generation !=
                pending->accept_generation) {
            if (owner) {
                k32_iocp_complete_handle_status_for_owner(
                    (HANDLE)(ULONG_PTR)pending->listen_socket, 0,
                    pending->overlapped, pending->owner_pid,
                    pending->compat32, STATUS_CANCELLED);
            }
            __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
            continue;
        }

        win32_socket_t *listener = &owner->sockets[listen_idx];
        win32_socket_t *accepted = &owner->sockets[accept_idx];
        if (pending->conn_id < 0) {
            int conn = net_tcp_accept(listener->kern_listener_id, 0);
            if (conn < 0) {
                __atomic_store_n(&pending->state, 1, __ATOMIC_RELEASE);
                continue;
            }
            pending->conn_id = conn;
            accepted->kern_conn_id = conn;
            accepted->connected = 1;
            accepted->connected_at_ms = GetTickCount();
            accepted->pending_error = 0;
            accepted->listening = 0;
            wsock_inherit_listener_options(accepted, listener);
            accepted->local_port = listener->local_port;
            ws_memcpy(accepted->local_ip, listener->local_ip, 4);
            if (net_tcp_get_peer(conn, accepted->remote_ip,
                                 &accepted->remote_port) < 0) {
                accepted->remote_ip[0] = 127;
                accepted->remote_ip[1] = 0;
                accepted->remote_ip[2] = 0;
                accepted->remote_ip[3] = 1;
                accepted->remote_port = 0;
            }
            wsock_apply_keepalive(accepted);
        }

        DWORD received = 0;
        if (pending->receive_length) {
            int available = net_tcp_rx_available(pending->conn_id);
            if (available <= 0) {
                __atomic_store_n(&pending->state, 1, __ATOMIC_RELEASE);
                continue;
            }
            DWORD amount = (DWORD)available;
            if (amount > pending->receive_length)
                amount = pending->receive_length;
            int result = net_tcp_recv(pending->conn_id, pending->output, amount);
            if (result > 0) received = (DWORD)result;
        }

        wsock_fill_acceptex_addresses(pending, listener, accepted);
        wsock_set_overlapped_status(pending->overlapped, pending->compat32,
                                    STATUS_SUCCESS, received);
        k32_iocp_complete_handle_for_owner(
            (HANDLE)(ULONG_PTR)pending->listen_socket, received,
            pending->overlapped, pending->owner_pid, pending->compat32);
        serial_puts("[WSOCK] AcceptEx complete listen=");
        serial_puthex((uint32_t)pending->listen_socket, 4);
        serial_puts(" accept=");
        serial_puthex((uint32_t)pending->accept_socket, 4);
        serial_puts(" conn=");
        serial_puthex((uint32_t)pending->conn_id, 2);
        serial_puts(" bytes=");
        serial_puthex(received, 8);
        serial_puts("\n");
        __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
    }
}

static BOOL wsock_tcp_eof(int state)
{
    return state == KERN_TCP_CLOSED || state == KERN_TCP_CLOSE_WAIT ||
           state == KERN_TCP_LAST_ACK || state == KERN_TCP_TIME_WAIT;
}

static int wsock_connect_error(int conn_id, BOOL timed_out)
{
    if (timed_out)
        return WSAETIMEDOUT;

    int kernel_error = net_tcp_error(conn_id);
    if (kernel_error == KERN_TCP_ERROR_TIMED_OUT)
        return WSAETIMEDOUT;
    if (kernel_error == KERN_TCP_ERROR_RESET)
        return WSAECONNREFUSED;
    return WSAECONNREFUSED;
}

static int wsock_tcp_send_error(const win32_socket_t *socket)
{
    if (!socket || socket->kern_conn_id < 0)
        return WSAENOTCONN;

    int kernel_error = net_tcp_error(socket->kern_conn_id);
    if (kernel_error == KERN_TCP_ERROR_RESET)
        return WSAECONNRESET;
    if (kernel_error == KERN_TCP_ERROR_TIMED_OUT)
        return WSAETIMEDOUT;

    int state = net_tcp_state(socket->kern_conn_id);
    if (state == KERN_TCP_CLOSED || state == KERN_TCP_LAST_ACK ||
        state == KERN_TCP_TIME_WAIT)
        return WSAENOTCONN;

    return WSAENETDOWN;
}

static NTSTATUS wsock_connect_status(int error)
{
    if (error == WSAETIMEDOUT)
        return STATUS_IO_TIMEOUT;
    if (error == WSAECONNRESET)
        return STATUS_CONNECTION_RESET;
    return STATUS_CONNECTION_REFUSED;
}

static int wsock_status_error(NTSTATUS status)
{
    if (status == STATUS_CANCELLED)
        return 995; /* WSA_OPERATION_ABORTED */
    if (status == STATUS_IO_TIMEOUT)
        return WSAETIMEDOUT;
    if (status == STATUS_CONNECTION_RESET)
        return WSAECONNRESET;
    if (status == STATUS_CONNECTION_REFUSED)
        return WSAECONNREFUSED;
    if (status == STATUS_BUFFER_OVERFLOW)
        return WSAEMSGSIZE;
    if (status == STATUS_INSUFFICIENT_RESOURCES)
        return WSAENOBUFS;
    if (status == STATUS_INVALID_HANDLE)
        return WSAENOTSOCK;
    return WSAEINVAL;
}

/* owner_filter==0 services every process from the global network worker. */
static void wsock_service_pending_connects(DWORD owner_filter)
{
    ULONGLONG now = GetTickCount64();

    for (int i = 0; i < MAX_PENDING_CONNECTEX; i++) {
        wsock_pending_connect_t *pending = &g_pending_connects[i];
        if (__atomic_load_n(&pending->state, __ATOMIC_ACQUIRE) != 1 ||
            (owner_filter && pending->owner_pid != owner_filter))
            continue;

        LONG expected = 1;
        if (!__atomic_compare_exchange_n(&pending->state, &expected, 2,
                                         FALSE, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            continue;

        wsock_process_state_t *owner = wsock_state_for_pid(pending->owner_pid);
        int index = wsock_raw_index(pending->socket);
        if (!owner || index < 0 || !owner->sockets[index].in_use ||
            owner->sockets[index].generation != pending->socket_generation) {
            if (pending->conn_id >= 0)
                net_tcp_abort(pending->conn_id);
            if (owner) {
                k32_iocp_complete_handle_status_for_owner(
                    (HANDLE)(ULONG_PTR)pending->socket, 0,
                    pending->overlapped, pending->owner_pid,
                    pending->compat32, STATUS_CANCELLED);
            }
            __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
            continue;
        }

        win32_socket_t *socket = &owner->sockets[index];
        int state = net_tcp_state(pending->conn_id);
        BOOL timed_out = state == KERN_TCP_SYN_SENT &&
                         now >= pending->deadline_ms;
        if (state == KERN_TCP_SYN_SENT && !timed_out) {
            __atomic_store_n(&pending->state, 1, __ATOMIC_RELEASE);
            continue;
        }

        DWORD transferred = 0;
        int error = 0;
        if (state == KERN_TCP_ESTABLISHED || state == KERN_TCP_CLOSE_WAIT) {
            socket->connected = 1;
            socket->connecting = 0;
            socket->connected_at_ms = GetTickCount();
            socket->pending_error = 0;
            wsock_apply_keepalive(socket);
            if (pending->send_buffer && pending->send_length) {
                int sent = net_tcp_send(pending->conn_id,
                                        pending->send_buffer,
                                        pending->send_length);
                if (sent < 0)
                    error = WSAECONNRESET;
                else
                    transferred = (DWORD)sent;
            }
        } else {
            if (timed_out)
                net_tcp_abort(pending->conn_id);
            error = wsock_connect_error(pending->conn_id, timed_out);
            socket->connected = 0;
            socket->connecting = 0;
            socket->pending_error = error;
        }

        wsock_propagate_shared_state(socket);
        wsock_refresh_socket_event_for_process(socket, pending->owner_pid);

        serial_puts("[WSOCK] ConnectEx ");
        serial_puts(error ? "failed" : "complete");
        serial_puts(" socket=");
        serial_puthex((uint64_t)pending->socket, 8);
        serial_puts(" conn=");
        serial_puthex((uint32_t)pending->conn_id, 2);
        serial_puts(" bytes=");
        serial_puthex(transferred, 8);
        if (error) {
            serial_puts(" error=");
            serial_putdec((uint64_t)error);
        }
        serial_puts("\n");

        if (error) {
            k32_iocp_complete_handle_status_for_owner(
                (HANDLE)(ULONG_PTR)pending->socket, transferred,
                pending->overlapped, pending->owner_pid, pending->compat32,
                wsock_connect_status(error));
        } else {
            k32_iocp_complete_handle_for_owner(
                (HANDLE)(ULONG_PTR)pending->socket, transferred,
                pending->overlapped, pending->owner_pid, pending->compat32);
        }
        __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
    }
}

static void wsock_wake_ready_pending_accepts(void)
{
    for (int i = 0; i < MAX_PENDING_ACCEPTEX; i++) {
        wsock_pending_accept_t *pending = &g_pending_accepts[i];
        if (__atomic_load_n(&pending->state, __ATOMIC_ACQUIRE) != 1)
            continue;

        wsock_process_state_t *owner = wsock_state_for_pid(pending->owner_pid);
        int index = wsock_raw_index(pending->listen_socket);
        if (!owner || index < 0 || !owner->sockets[index].in_use ||
            owner->sockets[index].generation != pending->listen_generation)
            continue;

        int listener = owner->sockets[index].kern_listener_id;
        if (listener >= 0 && net_tcp_accept_ready(listener) > 0) {
            k32_iocp_wake_handle_for_owner(
                (HANDLE)(ULONG_PTR)pending->listen_socket,
                pending->owner_pid);
        }
    }
}

static void wsock_wake_ready_pending_recvs(void)
{
    for (int i = 0; i < MAX_PENDING_WSARECV; i++) {
        wsock_pending_recv_t *pending = &g_pending_recvs[i];
        if (__atomic_load_n(&pending->state, __ATOMIC_ACQUIRE) != 1)
            continue;

        wsock_process_state_t *owner = wsock_state_for_pid(pending->owner_pid);
        int index = wsock_raw_index(pending->socket);
        if (!owner || index < 0 || !owner->sockets[index].in_use)
            continue;

        win32_socket_t *socket = &owner->sockets[index];
        if (pending->socket_generation != socket->generation)
            continue;
        if (pending->datagram) {
            if (socket->kern_socket_id >= 0 &&
                kernel_sock_has_pending_data(socket->kern_socket_id) > 0) {
                k32_iocp_wake_handle_for_owner(
                    (HANDLE)(ULONG_PTR)pending->socket, pending->owner_pid);
            }
            continue;
        }
        if (socket->kern_conn_id < 0)
            continue;
        int available = net_tcp_rx_available(socket->kern_conn_id);
        int state = net_tcp_state(socket->kern_conn_id);
        if (available > 0 || wsock_tcp_eof(state)) {
            k32_iocp_wake_handle_for_owner(
                (HANDLE)(ULONG_PTR)pending->socket, pending->owner_pid);
        }
    }
}

static void wsock_wake_ready_pending_io(void)
{
    wsock_wake_ready_pending_accepts();
    wsock_wake_ready_pending_recvs();
}

static DWORD wsock_recv_buffers_now(win32_socket_t *socket, PCVOID buffers,
                                    DWORD count, BOOL compat32)
{
    DWORD total = 0;
    for (DWORD i = 0; i < count; i++) {
        uint32_t length = wsabuf_len_for_mode(buffers, i, compat32);
        PVOID data = wsabuf_data_for_mode(buffers, i, compat32);
        if (!length) continue;

        int available = net_tcp_rx_available(socket->kern_conn_id);
        if (available <= 0) break;
        if (length > (uint32_t)available) length = (uint32_t)available;
        int received = net_tcp_recv(socket->kern_conn_id, data, length);
        if (received <= 0) break;
        wsock_trace_loopback_payload("RX", socket, data, (DWORD)received);
        total += (DWORD)received;
        if ((uint32_t)received < length) break;
    }
    return total;
}

static DWORD wsock_recv_captured_now(
    win32_socket_t *socket, const wsock_captured_buffer_t *buffers, DWORD count)
{
    DWORD total = 0;
    for (DWORD i = 0; i < count; i++) {
        uint32_t length = buffers[i].length;
        if (!length) continue;

        int available = net_tcp_rx_available(socket->kern_conn_id);
        if (available <= 0) break;
        if (length > (uint32_t)available) length = (uint32_t)available;
        int received = net_tcp_recv(socket->kern_conn_id, buffers[i].data,
                                    length);
        if (received <= 0) break;
        wsock_trace_loopback_payload("RX", socket, buffers[i].data,
                                     (DWORD)received);
        total += (DWORD)received;
        if ((uint32_t)received < length) break;
    }
    return total;
}

static NTSTATUS wsock_status_from_error(int error)
{
    switch (error) {
    case 0: return STATUS_SUCCESS;
    case WSAEMSGSIZE: return STATUS_BUFFER_OVERFLOW;
    case WSAENOBUFS: return STATUS_INSUFFICIENT_RESOURCES;
    case WSAENOTSOCK: return STATUS_INVALID_HANDLE;
    default: return STATUS_UNSUCCESSFUL;
    }
}

static BOOL wsock_pending_recv_has_earlier(
    const wsock_pending_recv_t *candidate)
{
    for (int i = 0; i < MAX_PENDING_WSARECV; i++) {
        const wsock_pending_recv_t *other = &g_pending_recvs[i];
        if (other == candidate ||
            __atomic_load_n(&other->state, __ATOMIC_ACQUIRE) != 1)
            continue;
        if (other->owner_pid == candidate->owner_pid &&
            other->socket == candidate->socket &&
            other->socket_generation == candidate->socket_generation &&
            other->sequence < candidate->sequence)
            return TRUE;
    }
    return FALSE;
}

static BOOL wsock_socket_has_pending_recv(DWORD owner_pid, SOCKET socket,
                                          DWORD socket_generation)
{
    for (int i = 0; i < MAX_PENDING_WSARECV; i++) {
        const wsock_pending_recv_t *pending = &g_pending_recvs[i];
        LONG state = __atomic_load_n(&pending->state, __ATOMIC_ACQUIRE);
        if (state != 1 && state != 2)
            continue;
        if (pending->owner_pid == owner_pid && pending->socket == socket &&
            pending->socket_generation == socket_generation)
            return TRUE;
    }
    return FALSE;
}

static void wsock_service_pending_recvs(DWORD owner_pid)
{
    LONG unlocked = 0;
    if (!__atomic_compare_exchange_n(&g_pending_recv_service_lock, &unlocked,
                                     1, FALSE, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;

    for (;;) {
        BOOL completed_any = FALSE;
        for (int i = 0; i < MAX_PENDING_WSARECV; i++) {
            wsock_pending_recv_t *pending = &g_pending_recvs[i];
            if (__atomic_load_n(&pending->state, __ATOMIC_ACQUIRE) != 1 ||
                pending->owner_pid != owner_pid ||
                wsock_pending_recv_has_earlier(pending))
                continue;
            LONG expected = 1;
            if (!__atomic_compare_exchange_n(&pending->state, &expected, 2,
                                             FALSE, __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE))
                continue;

            wsock_process_state_t *owner =
                wsock_state_for_pid(pending->owner_pid);
            int index = wsock_raw_index(pending->socket);
            if (!owner) {
                __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
                completed_any = TRUE;
                break;
            }
            if (index < 0 || !owner->sockets[index].in_use ||
                owner->sockets[index].generation !=
                    pending->socket_generation) {
                k32_iocp_complete_handle_status_for_owner(
                    (HANDLE)(ULONG_PTR)pending->socket, 0,
                    pending->overlapped, pending->owner_pid,
                    pending->compat32, STATUS_CANCELLED);
                __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
                completed_any = TRUE;
                break;
            }

            win32_socket_t *socket = &owner->sockets[index];
            if (pending->datagram) {
                DWORD received = 0;
                DWORD output_flags = 0;
                DWORD capacity = 0;
                for (DWORD j = 0; j < pending->count; j++)
                    capacity += pending->buffers[j].length;
                int error = wsock_recv_datagram_captured_now(
                    socket, pending->buffers, pending->count, capacity,
                    pending->input_flags, pending->from, pending->fromlen,
                    &received, &output_flags);
                if (error == WSAEWOULDBLOCK) {
                    __atomic_store_n(&pending->state, 1, __ATOMIC_RELEASE);
                    continue;
                }

                NTSTATUS status = wsock_status_from_error(error);
                k32_iocp_complete_handle_status_for_owner(
                    (HANDLE)(ULONG_PTR)pending->socket, received,
                    pending->overlapped, pending->owner_pid,
                    pending->compat32, status);
                serial_puts(
                    "[WSOCK] WSARecvFrom async complete socket=");
                serial_puthex((uint32_t)pending->socket, 4);
                serial_puts(" owner=");
                serial_puthex(pending->owner_pid, 8);
                serial_puts(" bytes=");
                serial_puthex(received, 8);
                serial_puts(" status=");
                serial_puthex((uint32_t)status, 8);
                serial_puts("\n");
                __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
                completed_any = TRUE;
                break;
            }

            DWORD received = 0;
            if (socket->kern_conn_id >= 0) {
                received = wsock_recv_captured_now(
                    socket, pending->buffers, pending->count);
            }

            int state = socket->kern_conn_id >= 0
                      ? net_tcp_state(socket->kern_conn_id) : KERN_TCP_CLOSED;
            if (!received && !wsock_tcp_eof(state)) {
                __atomic_store_n(&pending->state, 1, __ATOMIC_RELEASE);
                continue;
            }

            k32_iocp_complete_handle_for_owner(
                (HANDLE)(ULONG_PTR)pending->socket, received,
                pending->overlapped, pending->owner_pid, pending->compat32);
            serial_puts("[WSOCK] WSARecv async complete socket=");
            serial_puthex((uint32_t)pending->socket, 4);
            serial_puts(" owner=");
            serial_puthex(pending->owner_pid, 8);
            serial_puts(" bytes=");
            serial_puthex(received, 8);
            serial_puts("\n");
            __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
            completed_any = TRUE;
            break;
        }
        if (!completed_any)
            break;
    }

    __atomic_store_n(&g_pending_recv_service_lock, 0, __ATOMIC_RELEASE);
}

void wsock_service_pending_io(void)
{
    DWORD owner_pid = win32_current_process_id();
    net_poll();
    wsock_service_pending_connects(owner_pid);
    wsock_wake_ready_pending_io();
    wsock_service_pending_accepts(owner_pid);
    wsock_service_pending_recvs(owner_pid);
}

BOOL wsock_cancel_io(SOCKET socket, PVOID overlapped,
                     BOOL current_thread_only)
{
    DWORD owner_pid = win32_current_process_id();
    DWORD owner_tid = GetCurrentThreadId();
    BOOL cancelled = FALSE;

    for (int i = 0; i < MAX_PENDING_CONNECTEX; i++) {
        wsock_pending_connect_t *pending = &g_pending_connects[i];
        if (__atomic_load_n(&pending->state, __ATOMIC_ACQUIRE) != 1 ||
            pending->owner_pid != owner_pid || pending->socket != socket ||
            (overlapped && pending->overlapped != overlapped) ||
            (current_thread_only && pending->owner_tid != owner_tid))
            continue;
        LONG expected = 1;
        if (!__atomic_compare_exchange_n(&pending->state, &expected, 2,
                                         FALSE, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            continue;

        if (pending->conn_id >= 0)
            net_tcp_abort(pending->conn_id);
        wsock_process_state_t *owner = wsock_state_for_pid(owner_pid);
        int index = wsock_raw_index(socket);
        if (owner && index >= 0 &&
            owner->sockets[index].generation == pending->socket_generation) {
            win32_socket_t *target = &owner->sockets[index];
            target->kern_conn_id = -1;
            target->connected = 0;
            target->connecting = 0;
            target->pending_error = 0;
            wsock_propagate_shared_state(target);
        }
        k32_iocp_complete_handle_status_for_owner(
            (HANDLE)(ULONG_PTR)pending->socket, 0, pending->overlapped,
            pending->owner_pid, pending->compat32, STATUS_CANCELLED);
        __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
        cancelled = TRUE;
    }

    for (int i = 0; i < MAX_PENDING_WSARECV; i++) {
        wsock_pending_recv_t *pending = &g_pending_recvs[i];
        if (__atomic_load_n(&pending->state, __ATOMIC_ACQUIRE) != 1 ||
            pending->owner_pid != owner_pid || pending->socket != socket ||
            (overlapped && pending->overlapped != overlapped) ||
            (current_thread_only && pending->owner_tid != owner_tid))
            continue;
        LONG expected = 1;
        if (!__atomic_compare_exchange_n(&pending->state, &expected, 2, FALSE,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            continue;

        k32_iocp_complete_handle_status_for_owner(
            (HANDLE)(ULONG_PTR)pending->socket, 0, pending->overlapped,
            pending->owner_pid, pending->compat32, STATUS_CANCELLED);
        __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
        cancelled = TRUE;
    }

    for (int i = 0; i < MAX_PENDING_ACCEPTEX; i++) {
        wsock_pending_accept_t *pending = &g_pending_accepts[i];
        if (__atomic_load_n(&pending->state, __ATOMIC_ACQUIRE) != 1 ||
            pending->owner_pid != owner_pid ||
            (pending->listen_socket != socket &&
             pending->accept_socket != socket) ||
            (overlapped && pending->overlapped != overlapped) ||
            (current_thread_only && pending->owner_tid != owner_tid))
            continue;
        LONG expected = 1;
        if (!__atomic_compare_exchange_n(&pending->state, &expected, 2, FALSE,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            continue;

        if (pending->conn_id >= 0) {
            wsock_process_state_t *owner = wsock_state_for_pid(owner_pid);
            int accept_index = wsock_raw_index(pending->accept_socket);
            net_tcp_abort(pending->conn_id);
            if (owner && accept_index >= 0 &&
                owner->sockets[accept_index].generation ==
                    pending->accept_generation) {
                owner->sockets[accept_index].kern_conn_id = -1;
                owner->sockets[accept_index].connected = 0;
                owner->sockets[accept_index].remote_port = 0;
                ws_memset(owner->sockets[accept_index].remote_ip, 0, 4);
            }
        }
        k32_iocp_complete_handle_status_for_owner(
            (HANDLE)(ULONG_PTR)pending->listen_socket, 0,
            pending->overlapped, pending->owner_pid, pending->compat32,
            STATUS_CANCELLED);
        __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
        cancelled = TRUE;
    }
    return cancelled;
}

static void wsock_release_backend_on_process_exit(
    const win32_socket_t *socket)
{
    if (socket->kern_conn_id >= 0)
        net_tcp_abort(socket->kern_conn_id);
    if (socket->kern_listener_id >= 0)
        net_tcp_stop_listen(socket->kern_listener_id);
    if (socket->kern_socket_id >= 0)
        kernel_sock_close(socket->kern_socket_id);
}

static void wsock_discard_process_async_state(DWORD process_id)
{
    for (int i = 0; i < MAX_PENDING_CONNECTEX; i++) {
        wsock_pending_connect_t *pending = &g_pending_connects[i];
        if (__atomic_load_n(&pending->state, __ATOMIC_ACQUIRE) &&
            pending->owner_pid == process_id) {
            if (pending->conn_id >= 0)
                net_tcp_abort(pending->conn_id);
            __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
        }
    }
    for (int i = 0; i < MAX_PENDING_WSARECV; i++) {
        wsock_pending_recv_t *pending = &g_pending_recvs[i];
        if (__atomic_load_n(&pending->state, __ATOMIC_ACQUIRE) &&
            pending->owner_pid == process_id)
            __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
    }
    for (int i = 0; i < MAX_PENDING_ACCEPTEX; i++) {
        wsock_pending_accept_t *pending = &g_pending_accepts[i];
        if (__atomic_load_n(&pending->state, __ATOMIC_ACQUIRE) &&
            pending->owner_pid == process_id)
            __atomic_store_n(&pending->state, 0, __ATOMIC_RELEASE);
    }
    for (int i = 0; i < MAX_WSOCK_LOOKUPS; i++) {
        wsock_lookup_t *lookup = &g_wsock_lookups[i];
        if (__atomic_load_n(&lookup->state, __ATOMIC_ACQUIRE) &&
            lookup->owner_pid == process_id)
            __atomic_store_n(&lookup->state, 0, __ATOMIC_RELEASE);
    }
}

DWORD wsock_release_process(DWORD process_id)
{
    if (!process_id)
        return 0;

    DWORD released = 0;
    DWORD discarded_duplicates = 0;
    wsock_socket_lifecycle_acquire();
    wsock_discard_process_async_state(process_id);

    /* An unconsumed WSADuplicateSocket token owns a backend reference. Drop
     * tokens addressed to the exiting process before its socket handles so
     * the final-reference test can reclaim the transport exactly once. */
    for (;;) {
        win32_socket_t closing;
        BOOL found = FALSE;
        BOOL last_reference = FALSE;

        wsock_process_states_acquire();
        for (int i = 0; i < MAX_WSOCK_DUPLICATIONS; i++) {
            wsock_duplication_t *duplicate = &g_wsock_duplications[i];
            if (duplicate->state != 1 ||
                duplicate->target_pid != process_id)
                continue;
            closing = duplicate->socket;
            ws_memset(duplicate, 0, sizeof(*duplicate));
            last_reference = !wsock_backend_has_reference(&closing);
            found = TRUE;
            discarded_duplicates++;
            break;
        }
        wsock_process_states_release();

        if (!found)
            break;
        if (last_reference)
            wsock_release_backend_on_process_exit(&closing);
    }

    for (;;) {
        win32_socket_t closing;
        BOOL found = FALSE;
        BOOL last_reference = FALSE;

        wsock_process_states_acquire();
        wsock_process_state_t *state = NULL;
        for (int i = 0; i < MAX_WSOCK_PROCESS_STATES; i++) {
            if (g_wsock_process_states[i].used &&
                g_wsock_process_states[i].pid == process_id) {
                state = &g_wsock_process_states[i];
                break;
            }
        }
        if (!state) {
            wsock_process_states_release();
            break;
        }

        for (int i = 0; i < MAX_WIN32_SOCKETS; i++) {
            if (!state->sockets[i].in_use)
                continue;
            closing = state->sockets[i];
            state->sockets[i].in_use = 0;
            last_reference = !wsock_backend_has_reference(&closing);
            found = TRUE;
            released++;
            break;
        }
        if (!found)
            ws_memset(state, 0, sizeof(*state));
        wsock_process_states_release();

        if (!found)
            break;
        if (last_reference)
            wsock_release_backend_on_process_exit(&closing);
    }

    wsock_socket_lifecycle_release();

    if (released || discarded_duplicates) {
        serial_puts("[WSOCK] release pid=");
        serial_putdec(process_id);
        serial_puts(" handles=");
        serial_putdec(released);
        serial_puts(" duplicate_tokens=");
        serial_putdec(discarded_duplicates);
        serial_puts("\n");
    }
    return released;
}

extern TEB *win64_current_teb(void);
extern TEB32 *compat32_current_teb(void);

static int *wsock_last_error_slot(void)
{
    if (g_compat32_mode) {
        TEB32 *teb = compat32_current_teb();
        if (teb) {
            return (int *)((BYTE *)teb +
                           __builtin_offsetof(TEB32, LastErrorValue));
        }
    } else {
        TEB *teb = win64_current_teb();
        if (teb) return (int *)&teb->LastErrorValue;
    }

    return &wsock_state_current()->last_error;
}

#define win32_sockets   (wsock_state_current()->sockets)
#define wsa_initialized (wsock_state_current()->startup_refs > 0)
#define wsa_last_error  (*wsock_last_error_slot())

ULONG WINAPI wsock_inet_addr(PCSTR cp);
SOCKET WINAPI wsock_socket(int af, int type, int protocol);
int WINAPI wsock_recv(SOCKET s, PSTR buf, int len, int flags);
int WINAPI wsock_sendto(SOCKET s, PCSTR buf, int len, int flags,
                        PVOID to, int tolen);
int WINAPI wsock_recvfrom(SOCKET s, PSTR buf, int len, int flags,
                           PVOID from, int *fromlen);
static int wsock_bind_implicit(win32_socket_t *socket);

/* Convert SOCKET handle to table index, or -1 if invalid */
static int sock_to_index(SOCKET s)
{
    int idx = (int)(s - BASE_SOCKET);
    if (idx < 0 || idx >= MAX_WIN32_SOCKETS) return -1;
    if (!win32_sockets[idx].in_use) return -1;
    return idx;
}

static LONG wsock_socket_ready_events(win32_socket_t *ws)
{
    LONG ready = 0;

    if (ws->type == SOCK_DGRAM) {
        ready |= WSOCK_FD_WRITE;
        if (ws->connected) ready |= WSOCK_FD_CONNECT;
        if (ws->kern_socket_id >= 0 &&
            kernel_sock_has_pending_data(ws->kern_socket_id) > 0)
            ready |= WSOCK_FD_READ;
        return ready;
    }
    if (ws->listening && ws->kern_listener_id >= 0) {
        if (net_tcp_accept_ready(ws->kern_listener_id))
            ready |= WSOCK_FD_ACCEPT | WSOCK_FD_READ;
        return ready;
    }
    if (ws->kern_conn_id < 0)
        return ready;

    int state = net_tcp_state(ws->kern_conn_id);
    if (state == KERN_TCP_ESTABLISHED) {
        if (ws->connecting) {
            ws->connecting = 0;
            ws->connected = 1;
            ws->connected_at_ms = GetTickCount();
            ws->pending_error = 0;
            wsock_apply_keepalive(ws);
            wsock_propagate_shared_state(ws);
        }
        ready |= WSOCK_FD_CONNECT;
        if (net_tcp_send_ready(ws->kern_conn_id))
            ready |= WSOCK_FD_WRITE;
        if (net_tcp_rx_available(ws->kern_conn_id) >= ws->recv_low_water)
            ready |= WSOCK_FD_READ;
    } else if (state == KERN_TCP_CLOSED) {
        if (!ws->pending_error) {
            int kernel_error = net_tcp_error(ws->kern_conn_id);
            if (kernel_error == KERN_TCP_ERROR_RESET)
                ws->pending_error = WSAECONNRESET;
            else if (kernel_error == KERN_TCP_ERROR_TIMED_OUT)
                ws->pending_error = WSAETIMEDOUT;
        }
        if (ws->connecting)
            ws->connecting = 0;
        ready |= ws->connected ? WSOCK_FD_CLOSE : WSOCK_FD_CONNECT;
    }
    return ready;
}

static void wsock_refresh_socket_event_for_process(win32_socket_t *ws,
                                                    DWORD owner_pid)
{
    HANDLE event = ws->event_handle;
    LONG mask = __atomic_load_n(&ws->event_mask, __ATOMIC_ACQUIRE);
    if (!event || !mask)
        return;

    LONG ready = wsock_socket_ready_events(ws);
    LONG previous = __atomic_exchange_n(&ws->event_ready_mask, ready,
                                         __ATOMIC_ACQ_REL);
    LONG raised = ready & ~previous & mask;
    if (raised)
        __atomic_fetch_or(&ws->event_pending, raised, __ATOMIC_ACQ_REL);
    LONG pending = __atomic_load_n(&ws->event_pending, __ATOMIC_ACQUIRE);
    if (pending & mask) {
        NTSTATUS status = ntsync_set_event_for_process(event, owner_pid, NULL);
        if (raised && wsock_trace_loopback_control(ws)) {
            serial_puts("[WSOCK-EVENT] raise pid=");
            serial_putdec(owner_pid);
            serial_puts(" event=");
            serial_puthex((uint64_t)(ULONG_PTR)event, 8);
            serial_puts(" ready=");
            serial_puthex((uint32_t)ready, 8);
            serial_puts(" raised=");
            serial_puthex((uint32_t)raised, 8);
            serial_puts(" pending=");
            serial_puthex((uint32_t)pending, 8);
            serial_puts(" status=");
            serial_puthex((uint32_t)status, 8);
            serial_puts("\n");
        }
    }
}

static void wsock_refresh_socket_event(win32_socket_t *ws)
{
    wsock_refresh_socket_event_for_process(ws, win32_current_process_id());
}

static void wsock_rearm_socket_event(win32_socket_t *ws, LONG events)
{
    __atomic_fetch_and(&ws->event_ready_mask, ~events, __ATOMIC_ACQ_REL);
    wsock_refresh_socket_event(ws);
}

static void wsock_signal_socket_error(win32_socket_t *ws, int error,
                                      LONG event)
{
    ws->pending_error = error;
    if (__atomic_load_n(&ws->event_mask, __ATOMIC_ACQUIRE) & event) {
        __atomic_fetch_or(&ws->event_pending, event, __ATOMIC_ACQ_REL);
        if (ws->event_handle)
            ntsync_set_event_for_process(ws->event_handle,
                                         win32_current_process_id(), NULL);
    }
    wsock_propagate_shared_state(ws);
}

static void wsock_refresh_network_events(void)
{
    wsock_service_pending_io();
    for (int i = 0; i < MAX_WIN32_SOCKETS; i++) {
        if (win32_sockets[i].in_use)
            wsock_refresh_socket_event(&win32_sockets[i]);
    }
}

static void wsock_event_worker(void)
{
    for (;;) {
        /* Event-select users may have no thread executing a Winsock call while
         * they wait. Drive the polled NIC here before deriving FD_* state so
         * network events are observable without API-side polling. */
        net_poll();
        wsock_service_pending_connects(0);
        wsock_wake_ready_pending_io();
        for (int state_index = 0;
             state_index < MAX_WSOCK_PROCESS_STATES; state_index++) {
            wsock_process_state_t *state =
                &g_wsock_process_states[state_index];
            if (!__atomic_load_n(&state->used, __ATOMIC_ACQUIRE) ||
                __atomic_load_n(&state->startup_refs, __ATOMIC_ACQUIRE) <= 0)
                continue;

            for (int socket_index = 0;
                 socket_index < MAX_WIN32_SOCKETS; socket_index++) {
                win32_socket_t *socket = &state->sockets[socket_index];
                if (__atomic_load_n(&socket->in_use, __ATOMIC_ACQUIRE))
                    wsock_refresh_socket_event_for_process(socket, state->pid);
            }
        }

        if (sched_sleep_ticks(1) < 0)
            sched_yield();
    }
}

static void wsock_start_event_worker(void)
{
    if (__sync_lock_test_and_set(&g_wsock_event_worker_started, 1))
        return;
    if (sched_spawn("wsock-events", wsock_event_worker) < 0)
        __sync_lock_release(&g_wsock_event_worker_started);
}

/* Windows' dynamic client range starts at 49152. */
#define WSOCK_EPHEMERAL_FIRST 49152U
#define WSOCK_EPHEMERAL_LAST  65500U
#define WSOCK_EPHEMERAL_COUNT \
    (WSOCK_EPHEMERAL_LAST - WSOCK_EPHEMERAL_FIRST + 1U)

static uint16_t alloc_ephemeral_port(const win32_socket_t *socket)
{
    DWORD value;
    if (socket->option_flags & WSOCK_OPT_RANDOMIZE_PORT)
        random_get_bytes(&value, sizeof(value));
    else
        value = __sync_fetch_and_add(&g_ephemeral_sequence, 1);
    return (uint16_t)(WSOCK_EPHEMERAL_FIRST +
                      value % WSOCK_EPHEMERAL_COUNT);
}

static void wsock_initialize_socket(win32_socket_t *ws, int af, int type,
                                    int protocol)
{
    DWORD generation;

    ws_memset(ws, 0, sizeof(*ws));
    ws->in_use = 1;
    ws->af = af;
    ws->type = type;
    ws->protocol = protocol;
    ws->kern_conn_id = -1;
    ws->kern_listener_id = -1;
    ws->kern_socket_id = -1;
    ws->local_port = 0;
    ws->send_buffer_size = WSOCK_TCP_SEND_CAPACITY;
    ws->recv_buffer_size = WSOCK_TCP_RECV_CAPACITY;
    ws->send_low_water = 1;
    ws->recv_low_water = 1;
    ws->keepalive_time_ms = 7200000;
    ws->keepalive_interval_ms = 1000;

    /* net_tcp_send() emits immediately; Nagle is not present in this stack. */
    ws->no_delay = type == SOCK_STREAM;

    generation = __atomic_add_fetch(&g_wsock_socket_generation, 1,
                                    __ATOMIC_RELAXED);
    if (!generation)
        generation = __atomic_add_fetch(&g_wsock_socket_generation, 1,
                                        __ATOMIC_RELAXED);
    ws->generation = generation;
}

static uint32_t wsock_milliseconds_to_ticks(DWORD milliseconds)
{
    uint64_t ticks = ((uint64_t)milliseconds + 9U) / 10U;
    if (!ticks) ticks = 1;
    if (ticks > 0xFFFFFFFFULL) ticks = 0xFFFFFFFFULL;
    return (uint32_t)ticks;
}

static int wsock_apply_keepalive(win32_socket_t *ws)
{
    if (ws->type != SOCK_STREAM) return -1;
    if (ws->kern_conn_id < 0) return 0;
    int enabled = (ws->option_flags & WSOCK_OPT_KEEPALIVE) != 0;
    return net_tcp_set_keepalive(
        ws->kern_conn_id, enabled,
        wsock_milliseconds_to_ticks(ws->keepalive_time_ms),
        wsock_milliseconds_to_ticks(ws->keepalive_interval_ms));
}

static void wsock_inherit_listener_options(win32_socket_t *accepted,
                                           const win32_socket_t *listener)
{
    DWORD inherited = WSOCK_OPT_DEBUG | WSOCK_OPT_REUSEADDR |
                      WSOCK_OPT_KEEPALIVE | WSOCK_OPT_DONTROUTE |
                      WSOCK_OPT_BROADCAST | WSOCK_OPT_OOBINLINE;
    accepted->option_flags = (accepted->option_flags & ~inherited) |
                             (listener->option_flags & inherited);
    accepted->send_buffer_size = listener->send_buffer_size;
    accepted->recv_buffer_size = listener->recv_buffer_size;
    accepted->send_timeout_ms = listener->send_timeout_ms;
    accepted->recv_timeout_ms = listener->recv_timeout_ms;
    accepted->send_low_water = listener->send_low_water;
    accepted->recv_low_water = listener->recv_low_water;
    accepted->keepalive_time_ms = listener->keepalive_time_ms;
    accepted->keepalive_interval_ms = listener->keepalive_interval_ms;
    accepted->no_delay = listener->no_delay;
}

/* ── WSA functions ───────────────────────────────────────── */

typedef struct {
    WORD     wVersion;
    WORD     wHighVersion;
    char     szDescription[257];
    char     szSystemStatus[129];
    USHORT   iMaxSockets;
    USHORT   iMaxUdpDg;
    uint32_t lpVendorInfo;
} WSADATA32;

_Static_assert(sizeof(WSADATA32) == 400, "Win32 WSADATA ABI");
_Static_assert(sizeof(WSADATA) == 408, "Win64 WSADATA ABI");

int WINAPI WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData)
{
    wsock_process_state_t *state = wsock_state_current();
    serial_puts("[WSOCK] WSAStartup — bridging to kernel TCP/IP\n");
    if (!lpWSAData) {
        wsa_last_error = WSAEFAULT;
        return WSAEFAULT;
    }

    if (g_compat32_mode) {
        WSADATA32 *data32 = (WSADATA32 *)lpWSAData;
        ws_memset(data32, 0, sizeof(*data32));
        data32->wVersion = wVersionRequested;
        data32->wHighVersion = wVersionRequested;
    } else {
        ws_memset(lpWSAData, 0, sizeof(*lpWSAData));
        lpWSAData->wVersion = wVersionRequested;
        lpWSAData->wHighVersion = wVersionRequested;
    }

    wsock_socket_lifecycle_acquire();
    if (state->startup_refs == 0) {
        ws_memset(state->sockets, 0, sizeof(state->sockets));
        for (int i = 0; i < MAX_WIN32_SOCKETS; i++) {
            state->sockets[i].kern_conn_id = -1;
            state->sockets[i].kern_listener_id = -1;
            state->sockets[i].kern_socket_id = -1;
        }
    }
    state->startup_refs++;
    wsa_last_error = 0;
    wsock_socket_lifecycle_release();
    wsock_start_event_worker();

    serial_puts("[WSOCK] pid=");
    serial_putdec(state->pid);
    serial_puts(" refs=");
    serial_putdec((uint64_t)state->startup_refs);
    serial_puts("\n");
    return 0; /* success */
}

int WINAPI WSACleanup(void)
{
    wsock_process_state_t *state = wsock_state_current();
    wsock_socket_lifecycle_acquire();
    if (state->startup_refs <= 0) {
        wsa_last_error = WSANOTINITIALISED;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }

    state->startup_refs--;
    if (state->startup_refs > 0) {
        wsa_last_error = 0;
        wsock_socket_lifecycle_release();
        return 0;
    }

    /* Close all open sockets */
    for (int i = 0; i < MAX_WIN32_SOCKETS; i++) {
        if (!state->sockets[i].in_use) continue;
        wsock_cancel_io((SOCKET)(BASE_SOCKET + i), NULL, FALSE);
        win32_socket_t closing = state->sockets[i];
        wsock_process_states_acquire();
        state->sockets[i].in_use = 0;
        BOOL last_reference = !wsock_backend_has_reference(&closing);
        wsock_process_states_release();
        if (last_reference && closing.kern_conn_id >= 0)
            net_tcp_shutdown(closing.kern_conn_id);
        if (last_reference && closing.kern_listener_id >= 0)
            net_tcp_stop_listen(closing.kern_listener_id);
        if (last_reference && closing.kern_socket_id >= 0)
            kernel_sock_close(closing.kern_socket_id);
        k32_iocp_forget_file((HANDLE)(ULONG_PTR)(BASE_SOCKET + i));
        state->sockets[i].kern_conn_id = -1;
        state->sockets[i].kern_listener_id = -1;
        state->sockets[i].kern_socket_id = -1;
    }
    wsa_last_error = 0;
    wsock_socket_lifecycle_release();
    return 0;
}

int WINAPI WSAGetLastError(void)
{
    return wsa_last_error;
}

void WINAPI WSASetLastError(int error)
{
    wsa_last_error = error;
}

static BOOL wsock_guid_equal(const GUID *left, const GUID *right)
{
    if (!left || !right) return FALSE;
    const BYTE *a = (const BYTE *)left;
    const BYTE *b = (const BYTE *)right;
    for (SIZE_T i = 0; i < sizeof(GUID); i++)
        if (a[i] != b[i]) return FALSE;
    return TRUE;
}

static DWORD wsock_wide_bytes(const char *text)
{
    return (DWORD)(ws_strlen(text) + 1) * (DWORD)sizeof(WCHAR);
}

static void wsock_ascii_to_wide(WCHAR *destination, const char *source)
{
    while (*source) *destination++ = (WCHAR)(BYTE)*source++;
    *destination = 0;
}

static DWORD wsock_align_offset(DWORD offset, DWORD alignment)
{
    return (offset + alignment - 1U) & ~(alignment - 1U);
}

static BOOL wsock_protocol_requested(const int *protocols, int protocol)
{
    if (!protocols) return TRUE;
    for (int i = 0; i < 64 && protocols[i]; i++)
        if (protocols[i] == protocol) return TRUE;
    return FALSE;
}

static int wsock_enum_protocols_impl(const int *protocols,
                                     wsock_protocol_info_w_t *buffer,
                                     DWORD *buffer_length, int *error,
                                     BOOL require_startup)
{
    if (require_startup && !wsa_initialized) {
        if (error) *error = WSANOTINITIALISED;
        return SOCKET_ERROR;
    }
    if (!buffer_length) {
        if (error) *error = WSAEFAULT;
        return SOCKET_ERROR;
    }

    int count = 0;
    if (wsock_protocol_requested(protocols, IPPROTO_TCP)) count++;
    if (wsock_protocol_requested(protocols, IPPROTO_UDP)) count++;
    DWORD required = (DWORD)count * (DWORD)sizeof(wsock_protocol_info_w_t);
    if (required && (!buffer || *buffer_length < required)) {
        *buffer_length = required;
        if (error) *error = WSAENOBUFS;
        return SOCKET_ERROR;
    }

    int output = 0;
    win32_socket_t description;
    ws_memset(&description, 0, sizeof(description));
    description.af = AF_INET;
    if (wsock_protocol_requested(protocols, IPPROTO_TCP)) {
        description.type = SOCK_STREAM;
        description.protocol = IPPROTO_TCP;
        wsock_fill_protocol_info(&description, 0, &buffer[output++]);
    }
    if (wsock_protocol_requested(protocols, IPPROTO_UDP)) {
        description.type = SOCK_DGRAM;
        description.protocol = IPPROTO_UDP;
        wsock_fill_protocol_info(&description, 0, &buffer[output++]);
    }
    *buffer_length = required;
    if (error) *error = 0;
    return output;
}

static int WINAPI wsock_WSAEnumProtocolsW(const int *protocols, PVOID buffer,
                                           DWORD *buffer_length)
{
    int error = 0;
    int result = wsock_enum_protocols_impl(
        protocols, (wsock_protocol_info_w_t *)buffer, buffer_length, &error,
        TRUE);
    wsa_last_error = error;
    return result;
}

static int WINAPI wsock_WSCEnumProtocols(const int *protocols, PVOID buffer,
                                          DWORD *buffer_length, int *error)
{
    if (!error) return SOCKET_ERROR;
    return wsock_enum_protocols_impl(
        protocols, (wsock_protocol_info_w_t *)buffer, buffer_length, error,
        FALSE);
}

static int WINAPI wsock_WSCGetProviderPath(const GUID *provider,
                                            WCHAR *path, int *path_length,
                                            int *error)
{
    static const char provider_path[] =
        "C:\\Windows\\System32\\ws2_32.dll";
    if (!error) return SOCKET_ERROR;
    if (!provider || !path_length) {
        *error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    if (!wsock_guid_equal(provider, &g_wsock_provider_id)) {
        *error = WSAEINVAL;
        return SOCKET_ERROR;
    }
    int required = ws_strlen(provider_path) + 1;
    if (!path || *path_length < required) {
        *path_length = required;
        *error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    wsock_ascii_to_wide(path, provider_path);
    *path_length = required;
    *error = 0;
    return 0;
}

static int WINAPI wsock_WSAEnumNameSpaceProvidersW(DWORD *buffer_length,
                                                    PVOID buffer)
{
    static const char dns_name[] = "OsitoK DNS";
    static const char nla_name[] = "Network Location Awareness";
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return SOCKET_ERROR;
    }
    if (!buffer_length) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }

    BOOL compat32 = g_compat32_mode;
    DWORD header_size = compat32 ? sizeof(WSOCK_NAMESPACE_INFO32)
                                 : sizeof(WSOCK_NAMESPACE_INFO64);
    DWORD required = header_size * 2U + wsock_wide_bytes(dns_name) +
                     wsock_wide_bytes(nla_name);
    if (!buffer || *buffer_length < required) {
        *buffer_length = required;
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }

    ws_memset(buffer, 0, required);
    BYTE *strings = (BYTE *)buffer + header_size * 2U;
    WCHAR *dns_identifier = (WCHAR *)strings;
    WCHAR *nla_identifier = (WCHAR *)(strings + wsock_wide_bytes(dns_name));
    wsock_ascii_to_wide(dns_identifier, dns_name);
    wsock_ascii_to_wide(nla_identifier, nla_name);

    if (compat32) {
        WSOCK_NAMESPACE_INFO32 *info = (WSOCK_NAMESPACE_INFO32 *)buffer;
        info[0].provider_id = g_wsock_dns_namespace_id;
        info[0].name_space = NS_DNS;
        info[0].active = TRUE;
        info[0].version = 2;
        info[0].identifier = (uint32_t)(uintptr_t)dns_identifier;
        info[1].provider_id = g_wsock_nla_namespace_id;
        info[1].name_space = NS_NLA;
        info[1].active = TRUE;
        info[1].version = 1;
        info[1].identifier = (uint32_t)(uintptr_t)nla_identifier;
    } else {
        WSOCK_NAMESPACE_INFO64 *info = (WSOCK_NAMESPACE_INFO64 *)buffer;
        info[0].provider_id = g_wsock_dns_namespace_id;
        info[0].name_space = NS_DNS;
        info[0].active = TRUE;
        info[0].version = 2;
        info[0].identifier = (uint64_t)(uintptr_t)dns_identifier;
        info[1].provider_id = g_wsock_nla_namespace_id;
        info[1].name_space = NS_NLA;
        info[1].active = TRUE;
        info[1].version = 1;
        info[1].identifier = (uint64_t)(uintptr_t)nla_identifier;
    }

    *buffer_length = required;
    wsa_last_error = 0;
    return 2;
}

static DWORD wsock_query_size(BOOL compat32)
{
    return compat32 ? sizeof(WSAQUERYSETW32) : sizeof(WSAQUERYSETW64);
}

static DWORD wsock_query_namespace(PCVOID query, BOOL compat32)
{
    return compat32 ? ((const WSAQUERYSETW32 *)query)->dwNameSpace
                    : ((const WSAQUERYSETW64 *)query)->dwNameSpace;
}

static DWORD wsock_query_declared_size(PCVOID query, BOOL compat32)
{
    return compat32 ? ((const WSAQUERYSETW32 *)query)->dwSize
                    : ((const WSAQUERYSETW64 *)query)->dwSize;
}

static const GUID *wsock_query_service_class(PCVOID query, BOOL compat32)
{
    uintptr_t address = compat32
        ? (uintptr_t)((const WSAQUERYSETW32 *)query)->lpServiceClassId
        : (uintptr_t)((const WSAQUERYSETW64 *)query)->lpServiceClassId;
    return (const GUID *)address;
}

static wsock_lookup_t *wsock_lookup_find(HANDLE handle)
{
    DWORD token = (DWORD)(ULONG_PTR)handle;
    DWORD owner = win32_current_process_id();
    for (int i = 0; i < MAX_WSOCK_LOOKUPS; i++) {
        wsock_lookup_t *lookup = &g_wsock_lookups[i];
        if (__atomic_load_n(&lookup->state, __ATOMIC_ACQUIRE) == 1 &&
            lookup->token == token && lookup->owner_pid == owner)
            return lookup;
    }
    return NULL;
}

static int WINAPI wsock_WSALookupServiceBeginW(PVOID restrictions,
                                                DWORD flags, PVOID lookup)
{
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return SOCKET_ERROR;
    }
    if (!restrictions || !lookup) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    BOOL compat32 = g_compat32_mode;
    if (wsock_query_declared_size(restrictions, compat32) <
        wsock_query_size(compat32)) {
        wsa_last_error = WSAEINVAL;
        return SOCKET_ERROR;
    }
    DWORD name_space = wsock_query_namespace(restrictions, compat32);
    const GUID *service_class =
        wsock_query_service_class(restrictions, compat32);
    if ((name_space != NS_ALL && name_space != NS_NLA) ||
        (service_class &&
         !wsock_guid_equal(service_class, &g_wsock_nla_service_class)) ||
        (name_space == NS_ALL && !service_class)) {
        wsa_last_error = WSASERVICE_NOT_FOUND;
        return SOCKET_ERROR;
    }

    wsock_lookup_t *slot = NULL;
    for (int i = 0; i < MAX_WSOCK_LOOKUPS; i++) {
        LONG expected = 0;
        if (__atomic_compare_exchange_n(&g_wsock_lookups[i].state,
                                        &expected, 2, FALSE,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            slot = &g_wsock_lookups[i];
            break;
        }
    }
    if (!slot) {
        wsa_last_error = WSAENOBUFS;
        return SOCKET_ERROR;
    }

    DWORD token = __atomic_add_fetch(&g_wsock_lookup_token, 1,
                                     __ATOMIC_RELAXED) & 0xFFFFU;
    if (!token)
        token = __atomic_add_fetch(&g_wsock_lookup_token, 1,
                                   __ATOMIC_RELAXED) & 0xFFFFU;
    token |= 0x4E4C0000U;
    slot->token = token;
    slot->owner_pid = win32_current_process_id();
    slot->begin_flags = flags;
    slot->compat32 = compat32;
    slot->returned = FALSE;
    __atomic_store_n(&slot->state, 1, __ATOMIC_RELEASE);

    if (compat32)
        *(uint32_t *)lookup = token;
    else
        *(uint64_t *)lookup = token;
    wsa_last_error = 0;
    return 0;
}

static int WINAPI wsock_WSALookupServiceNextW(HANDLE handle, DWORD flags,
                                               DWORD *buffer_length,
                                               PVOID results)
{
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return SOCKET_ERROR;
    }
    if (!buffer_length) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    wsock_lookup_t *lookup = wsock_lookup_find(handle);
    if (!lookup) {
        wsa_last_error = 6; /* WSA_INVALID_HANDLE */
        return SOCKET_ERROR;
    }
    if ((flags & LUP_FLUSHPREVIOUS) || lookup->returned) {
        lookup->returned = TRUE;
        wsa_last_error = WSA_E_NO_MORE;
        return SOCKET_ERROR;
    }

    DWORD requested = flags & lookup->begin_flags & LUP_RETURN_ALL;
    BOOL compat32 = lookup->compat32;
    DWORD pointer_align = compat32 ? 4U : 8U;
    DWORD required = wsock_query_size(compat32);
    static const char network_name[] = "OsitoK Network";
    static const char comment[] = "Active IPv4 network";
    if (requested & LUP_RETURN_NAME)
        required += wsock_wide_bytes(network_name);
    if (requested & LUP_RETURN_TYPE) {
        required = wsock_align_offset(required, 4);
        required += sizeof(GUID);
    }
    if (requested & LUP_RETURN_COMMENT)
        required += wsock_wide_bytes(comment);
    if (requested & LUP_RETURN_BLOB) {
        required = wsock_align_offset(required, pointer_align);
        required += compat32 ? sizeof(WSOCK_BLOB32) : sizeof(WSOCK_BLOB64);
        required = wsock_align_offset(required, 4);
        required += sizeof(WSOCK_NLA_BLOB);
    }

    if (!results || *buffer_length < required) {
        *buffer_length = required;
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }

    ws_memset(results, 0, required);
    DWORD offset = wsock_query_size(compat32);
    uintptr_t name_pointer = 0;
    uintptr_t class_pointer = 0;
    uintptr_t comment_pointer = 0;
    uintptr_t blob_pointer = 0;
    if (requested & LUP_RETURN_NAME) {
        name_pointer = (uintptr_t)((BYTE *)results + offset);
        wsock_ascii_to_wide((WCHAR *)name_pointer, network_name);
        offset += wsock_wide_bytes(network_name);
    }
    if (requested & LUP_RETURN_TYPE) {
        offset = wsock_align_offset(offset, 4);
        class_pointer = (uintptr_t)((BYTE *)results + offset);
        ws_memcpy((PVOID)class_pointer, &g_wsock_nla_service_class,
                  sizeof(GUID));
        offset += sizeof(GUID);
    }
    if (requested & LUP_RETURN_COMMENT) {
        comment_pointer = (uintptr_t)((BYTE *)results + offset);
        wsock_ascii_to_wide((WCHAR *)comment_pointer, comment);
        offset += wsock_wide_bytes(comment);
    }
    if (requested & LUP_RETURN_BLOB) {
        offset = wsock_align_offset(offset, pointer_align);
        blob_pointer = (uintptr_t)((BYTE *)results + offset);
        offset += compat32 ? sizeof(WSOCK_BLOB32) : sizeof(WSOCK_BLOB64);
        offset = wsock_align_offset(offset, 4);
        WSOCK_NLA_BLOB *nla =
            (WSOCK_NLA_BLOB *)((BYTE *)results + offset);
        uint8_t *ip = net_get_ip_ptr();
        BOOL online = ip && (ip[0] || ip[1] || ip[2] || ip[3]);
        nla->type = 3; /* NLA_CONNECTIVITY */
        nla->size = sizeof(*nla);
        nla->next_offset = 0;
        nla->connectivity_type = 2; /* NLA_NETWORK_UNMANAGED */
        nla->internet = online ? 2 : 1; /* YES or NO */
        if (compat32) {
            WSOCK_BLOB32 *blob = (WSOCK_BLOB32 *)blob_pointer;
            blob->size = sizeof(*nla);
            blob->data = (uint32_t)(uintptr_t)nla;
        } else {
            WSOCK_BLOB64 *blob = (WSOCK_BLOB64 *)blob_pointer;
            blob->size = sizeof(*nla);
            blob->data = (uint64_t)(uintptr_t)nla;
        }
    }

    if (compat32) {
        WSAQUERYSETW32 *query = (WSAQUERYSETW32 *)results;
        query->dwSize = sizeof(*query);
        query->lpszServiceInstanceName = (uint32_t)name_pointer;
        query->lpServiceClassId = (uint32_t)class_pointer;
        query->lpszComment = (uint32_t)comment_pointer;
        query->dwNameSpace = NS_NLA;
        query->lpNSProviderId = 0;
        query->dwOutputFlags = 0;
        query->lpBlob = (uint32_t)blob_pointer;
    } else {
        WSAQUERYSETW64 *query = (WSAQUERYSETW64 *)results;
        query->dwSize = sizeof(*query);
        query->lpszServiceInstanceName = (uint64_t)name_pointer;
        query->lpServiceClassId = (uint64_t)class_pointer;
        query->lpszComment = (uint64_t)comment_pointer;
        query->dwNameSpace = NS_NLA;
        query->lpNSProviderId = 0;
        query->dwOutputFlags = 0;
        query->lpBlob = (uint64_t)blob_pointer;
    }

    lookup->returned = TRUE;
    *buffer_length = required;
    wsa_last_error = 0;
    return 0;
}

static int WINAPI wsock_WSALookupServiceEnd(HANDLE handle)
{
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return SOCKET_ERROR;
    }
    wsock_lookup_t *lookup = wsock_lookup_find(handle);
    if (!lookup) {
        wsa_last_error = 6; /* WSA_INVALID_HANDLE */
        return SOCKET_ERROR;
    }
    __atomic_store_n(&lookup->state, 0, __ATOMIC_RELEASE);
    wsa_last_error = 0;
    return 0;
}

static int WINAPI wsock_WSASetServiceW(PVOID registration, int operation,
                                        DWORD flags)
{
    (void)flags;
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return SOCKET_ERROR;
    }
    if (!registration) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    if (operation < 0 || operation > 2) {
        wsa_last_error = WSAEINVAL;
        return SOCKET_ERROR;
    }
    wsa_last_error = operation == 0 ? WSAEOPNOTSUPP
                                    : WSASERVICE_NOT_FOUND;
    return SOCKET_ERROR;
}

static HANDLE WINAPI wsock_WSACreateEvent(void)
{
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return NULL;
    }
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    wsa_last_error = event ? 0 : (int)GetLastError();
    return event;
}

static BOOL WINAPI wsock_WSACloseEvent(HANDLE event)
{
    BOOL ok = CloseHandle(event);
    wsa_last_error = ok ? 0 : (int)GetLastError();
    return ok;
}

static BOOL WINAPI wsock_WSASetEvent(HANDLE event)
{
    BOOL ok = SetEvent(event);
    wsa_last_error = ok ? 0 : (int)GetLastError();
    return ok;
}

static BOOL WINAPI wsock_WSAResetEvent(HANDLE event)
{
    BOOL ok = ResetEvent(event);
    wsa_last_error = ok ? 0 : (int)GetLastError();
    return ok;
}

static int WINAPI wsock_WSAEventSelect(SOCKET socket, HANDLE event,
                                        LONG network_events)
{
    int idx = sock_to_index(socket);
    if (idx < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }
    if (network_events && !event) {
        wsa_last_error = WSAEINVAL;
        return SOCKET_ERROR;
    }

    win32_socket_t *ws = &win32_sockets[idx];
    if (network_events)
        ws->nonblocking = 1;
    ws->event_handle = network_events ? event : NULL;
    __atomic_store_n(&ws->event_mask, network_events, __ATOMIC_RELEASE);
    __atomic_store_n(&ws->event_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ws->event_ready_mask, 0, __ATOMIC_RELEASE);
    if (event)
        ResetEvent(event);
    wsock_refresh_socket_event(ws);
    if (wsock_trace_loopback_control(ws)) {
        serial_puts("[WSOCK-EVENT] select pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" socket=");
        serial_puthex((uint32_t)socket, 4);
        serial_puts(" event=");
        serial_puthex((uint64_t)(ULONG_PTR)event, 8);
        serial_puts(" mask=");
        serial_puthex((uint32_t)network_events, 8);
        serial_puts(" pending=");
        serial_puthex((uint32_t)__atomic_load_n(&ws->event_pending,
                                                __ATOMIC_ACQUIRE), 8);
        serial_puts(" ready=");
        serial_puthex((uint32_t)__atomic_load_n(&ws->event_ready_mask,
                                                __ATOMIC_ACQUIRE), 8);
        serial_puts("\n");
    }
    wsock_propagate_shared_state(ws);
    wsa_last_error = 0;
    return 0;
}

static int WINAPI wsock_WSAEnumNetworkEvents(SOCKET socket, HANDLE event,
                                              WSOCK_NETWORK_EVENTS *events)
{
    int idx = sock_to_index(socket);
    if (idx < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }
    if (!events) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }

    wsock_refresh_network_events();
    win32_socket_t *ws = &win32_sockets[idx];
    LONG mask = __atomic_load_n(&ws->event_mask, __ATOMIC_ACQUIRE);
    events->network_events =
        __atomic_load_n(&ws->event_pending, __ATOMIC_ACQUIRE) & mask;
    for (int i = 0; i < WSOCK_FD_MAX_EVENTS; i++)
        events->error_codes[i] = 0;
    if (events->network_events & WSOCK_FD_CONNECT)
        events->error_codes[4] = ws->pending_error;
    if (events->network_events & WSOCK_FD_CLOSE)
        events->error_codes[5] = ws->pending_error;
    if (events->network_events & (WSOCK_FD_CONNECT | WSOCK_FD_CLOSE))
        ws->pending_error = 0;
    __atomic_fetch_and(&ws->event_pending, ~events->network_events,
                       __ATOMIC_ACQ_REL);
    if (event)
        ResetEvent(event);
    if (wsock_trace_loopback_control(ws)) {
        serial_puts("[WSOCK-EVENT] enum pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" socket=");
        serial_puthex((uint32_t)socket, 4);
        serial_puts(" event=");
        serial_puthex((uint64_t)(ULONG_PTR)event, 8);
        serial_puts(" events=");
        serial_puthex((uint32_t)events->network_events, 8);
        serial_puts(" pending=");
        serial_puthex((uint32_t)__atomic_load_n(&ws->event_pending,
                                                __ATOMIC_ACQUIRE), 8);
        serial_puts(" ready=");
        serial_puthex((uint32_t)__atomic_load_n(&ws->event_ready_mask,
                                                __ATOMIC_ACQUIRE), 8);
        serial_puts("\n");
    }
    wsa_last_error = 0;
    return 0;
}

static DWORD WINAPI wsock_WSAWaitForMultipleEvents(DWORD count,
                                                     const HANDLE *events,
                                                     BOOL wait_all,
                                                     DWORD timeout,
                                                     BOOL alertable)
{
    (void)alertable;
    ULONGLONG start = GetTickCount64();
    for (;;) {
        wsock_refresh_network_events();
        DWORD result = WaitForMultipleObjects(count, events, wait_all, 0);
        if (result != WSOCK_WAIT_TIMEOUT || timeout == 0)
            return result;
        if (timeout != WSOCK_INFINITE && GetTickCount64() - start >= timeout)
            return WSOCK_WAIT_TIMEOUT;
        Sleep(1);
    }
}

int WINAPI wsock_ioctlsocket(SOCKET s, LONG cmd, ULONG *argp)
{
    int idx = sock_to_index(s);
    if (idx < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }
    if (!argp) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }

    win32_socket_t *ws = &win32_sockets[idx];
    switch ((ULONG)cmd) {
    case WSOCK_FIONBIO:
        ws->nonblocking = *argp != 0;
        break;
    case WSOCK_FIONREAD:
        if (ws->type == SOCK_STREAM && ws->kern_conn_id >= 0) {
            int available = net_tcp_rx_available(ws->kern_conn_id);
            *argp = available > 0 ? (ULONG)available : 0;
        } else if (ws->type == SOCK_DGRAM && ws->kern_socket_id >= 0) {
            int available = kernel_sock_pending_bytes(ws->kern_socket_id);
            *argp = available > 0 ? (ULONG)available : 0;
        } else {
            *argp = 0;
        }
        break;
    case WSOCK_SIOCATMARK:
        /* Urgent data is unsupported, so the stream is always at its mark. */
        *argp = 1;
        break;
    default:
        wsa_last_error = WSAEOPNOTSUPP;
        return SOCKET_ERROR;
    }
    if ((ULONG)cmd == WSOCK_FIONBIO)
        wsock_propagate_shared_state(ws);
    wsa_last_error = 0;
    return 0;
}

int WINAPI wsock_fd_is_set(SOCKET s, const void *set)
{
    if (!set) return 0;
    uint32_t count = fdset_count(set);
    if (count > 64) count = 64;
    for (uint32_t i = 0; i < count; i++)
        if (fdset_socket(set, i) == s) return 1;
    return 0;
}

static BOOL WINAPI wsock_connect_ex(SOCKET s, PVOID name, int namelen,
                                     PVOID send_buf, DWORD send_len,
                                     DWORD *bytes_sent, PVOID overlapped);
static BOOL WINAPI wsock_disconnect_ex(SOCKET s, PVOID overlapped,
                                        DWORD flags, DWORD reserved);
static BOOL WINAPI wsock_AcceptEx(SOCKET listen_socket, SOCKET accept_socket,
                                  PVOID output, DWORD receive_length,
                                  DWORD local_length, DWORD remote_length,
                                  DWORD *bytes_received, PVOID overlapped);
static void WINAPI wsock_GetAcceptExSockaddrs(
    PVOID output, DWORD receive_length, DWORD local_length,
    DWORD remote_length, PVOID *local_addr, int *local_addrlen,
    PVOID *remote_addr, int *remote_addrlen);
static int WINAPI wsock_WSASendMsg(SOCKET s, PVOID message, DWORD flags,
                                    DWORD *bytes_sent, PVOID overlapped,
                                    PVOID completion);
static int WINAPI wsock_WSARecvMsg(SOCKET s, PVOID message,
                                    DWORD *bytes_received, PVOID overlapped,
                                    PVOID completion);

int WINAPI WSAIoctl(SOCKET s, DWORD code, PVOID in_buf, DWORD in_len,
                    PVOID out_buf, DWORD out_len, DWORD *bytes_returned,
                    PVOID overlapped, PVOID completion_routine)
{
    static uint32_t connect_thunk;
    static uint32_t disconnect_thunk;
    static uint32_t accept_thunk;
    static uint32_t accept_addrs_thunk;
    static uint32_t recv_msg_thunk;
    static uint32_t send_msg_thunk;
    uint32_t *thunk;
    uint64_t target;
    const char *name;
    uint8_t argc;

    (void)overlapped;
    (void)completion_routine;
    if (bytes_returned) *bytes_returned = 0;
    int socket_index = sock_to_index(s);
    if (socket_index < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }
    if (code == SIO_KEEPALIVE_VALS) {
        if (!in_buf || in_len < 3 * sizeof(uint32_t)) {
            wsa_last_error = WSAEFAULT;
            return SOCKET_ERROR;
        }
        win32_socket_t *ws = &win32_sockets[socket_index];
        if (ws->type != SOCK_STREAM) {
            wsa_last_error = WSAENOPROTOOPT;
            return SOCKET_ERROR;
        }
        const uint32_t *values = (const uint32_t *)in_buf;
        if (values[0] && (!values[1] || !values[2])) {
            wsa_last_error = WSAEINVAL;
            return SOCKET_ERROR;
        }

        DWORD old_flags = ws->option_flags;
        DWORD old_time = ws->keepalive_time_ms;
        DWORD old_interval = ws->keepalive_interval_ms;
        if (values[0]) {
            ws->option_flags |= WSOCK_OPT_KEEPALIVE;
            ws->keepalive_time_ms = values[1];
            ws->keepalive_interval_ms = values[2];
        } else {
            ws->option_flags &= ~WSOCK_OPT_KEEPALIVE;
        }
        if (wsock_apply_keepalive(ws) < 0) {
            ws->option_flags = old_flags;
            ws->keepalive_time_ms = old_time;
            ws->keepalive_interval_ms = old_interval;
            wsa_last_error = WSAENETDOWN;
            return SOCKET_ERROR;
        }
        wsock_propagate_shared_state(ws);
        wsa_last_error = 0;
        return 0;
    }
    if (code != 0xC8000006) {
        wsa_last_error = WSAEOPNOTSUPP;
        return SOCKET_ERROR;
    }
    DWORD pointer_size = g_compat32_mode ? 4 : 8;
    if (!in_buf || in_len < 16 || !out_buf || out_len < pointer_size) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }

    const uint32_t *guid = (const uint32_t *)in_buf;
    if (guid[0] == 0x25A207B9 && guid[1] == 0x4660DDF3 &&
        guid[2] == 0xE576E98E && guid[3] == 0x3E06748C) {
        thunk = &connect_thunk;
        target = (uint64_t)(uintptr_t)wsock_connect_ex;
        name = "ConnectEx";
        argc = 7;
    } else if (guid[0] == 0x7FDA2E11 && guid[1] == 0x436F8630 &&
               guid[2] == 0x36F531A0 && guid[3] == 0x57C1EEA6) {
        thunk = &disconnect_thunk;
        target = (uint64_t)(uintptr_t)wsock_disconnect_ex;
        name = "DisconnectEx";
        argc = 4;
    } else if (guid[0] == 0xB5367DF1 && guid[1] == 0x11CFCBAC &&
               guid[2] == 0x8000CA95 && guid[3] == 0x92A1485F) {
        thunk = &accept_thunk;
        target = (uint64_t)(uintptr_t)wsock_AcceptEx;
        name = "AcceptEx";
        argc = 8;
    } else if (guid[0] == 0xB5367DF2 && guid[1] == 0x11CFCBAC &&
               guid[2] == 0x8000CA95 && guid[3] == 0x92A1485F) {
        thunk = &accept_addrs_thunk;
        target = (uint64_t)(uintptr_t)wsock_GetAcceptExSockaddrs;
        name = "GetAcceptExSockaddrs";
        argc = 8;
    } else if (wsock_guid_equal((const GUID *)in_buf,
                                &g_wsock_wsa_recv_msg_id)) {
        thunk = &recv_msg_thunk;
        target = (uint64_t)(uintptr_t)wsock_WSARecvMsg;
        name = "WSARecvMsg";
        argc = 5;
    } else if (wsock_guid_equal((const GUID *)in_buf,
                                &g_wsock_wsa_send_msg_id)) {
        thunk = &send_msg_thunk;
        target = (uint64_t)(uintptr_t)wsock_WSASendMsg;
        name = "WSASendMsg";
        argc = 6;
    } else {
        wsa_last_error = WSAEOPNOTSUPP;
        return SOCKET_ERROR;
    }

    if (g_compat32_mode) {
        if (!*thunk)
            *thunk = compat32_make_thunk_ex(target, name, argc, CC_STDCALL);
        if (!*thunk) {
            wsa_last_error = WSAEOPNOTSUPP;
            return SOCKET_ERROR;
        }
        *(uint32_t *)out_buf = *thunk;
    } else {
        *(uint64_t *)out_buf = target;
    }

    if (bytes_returned) *bytes_returned = pointer_size;
    wsa_last_error = 0;
    return 0;
}

static int WINAPI wsock_WSADuplicateSocketW(SOCKET socket, DWORD target_pid,
                                             wsock_protocol_info_w_t *info)
{
    wsock_process_state_t *state = wsock_state_current();
    if (!target_pid || !info) {
        wsa_last_error = !info ? WSAEFAULT : WSAEINVAL;
        return SOCKET_ERROR;
    }

    wsock_socket_lifecycle_acquire();
    int index = wsock_raw_index(socket);
    if (index < 0 || !state->sockets[index].in_use) {
        wsa_last_error = WSAENOTSOCK;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }

    wsock_process_states_acquire();
    wsock_duplication_t *slot = NULL;
    for (int i = 0; i < MAX_WSOCK_DUPLICATIONS; i++) {
        if (!g_wsock_duplications[i].state) {
            slot = &g_wsock_duplications[i];
            slot->state = 2;
            break;
        }
    }
    if (!slot) {
        wsock_process_states_release();
        wsa_last_error = WSAENOBUFS;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }

    ws_memset(&slot->socket, 0, sizeof(slot->socket));
    DWORD token = wsock_next_duplication_token();
    slot->token = token;
    slot->source_pid = win32_current_process_id();
    slot->target_pid = target_pid;
    slot->socket = state->sockets[index];
    wsock_clear_handle_associations(&slot->socket);
    slot->state = 1;
    wsock_fill_protocol_info(&state->sockets[index], token, info);
    wsock_process_states_release();
    wsa_last_error = 0;
    wsock_socket_lifecycle_release();

    serial_puts("[WSOCK] duplicate socket=");
    serial_puthex((uint32_t)socket, 4);
    serial_puts(" target=");
    serial_putdec(target_pid);
    serial_puts(" token=");
    serial_puthex(token, 8);
    serial_puts("\n");
    return 0;
}

static SOCKET wsock_socket_from_duplicate(
    const wsock_protocol_info_header_t *protocol_info)
{
    DWORD target_pid = win32_current_process_id();
    DWORD token = protocol_info->provider_reserved;
    wsock_process_state_t *state = wsock_state_current();
    SOCKET result = INVALID_SOCKET;

    wsock_socket_lifecycle_acquire();
    wsock_process_states_acquire();
    wsock_duplication_t *slot = NULL;
    for (int i = 0; i < MAX_WSOCK_DUPLICATIONS; i++) {
        wsock_duplication_t *candidate = &g_wsock_duplications[i];
        if (candidate->state == 1 && candidate->token == token &&
            candidate->target_pid == target_pid) {
            candidate->state = 2;
            slot = candidate;
            break;
        }
    }
    if (!slot) {
        wsock_process_states_release();
        wsa_last_error = WSAEINVAL;
        wsock_socket_lifecycle_release();
        return INVALID_SOCKET;
    }

    int index = -1;
    if (state->used && state->pid == target_pid) {
        for (int i = 0; i < MAX_WIN32_SOCKETS; i++) {
            if (!state->sockets[i].in_use) {
                index = i;
                break;
            }
        }
    }
    if (index < 0) {
        slot->state = 1;
        wsock_process_states_release();
        wsa_last_error = state->startup_refs > 0
                       ? WSAEMFILE : WSANOTINITIALISED;
        wsock_socket_lifecycle_release();
        return INVALID_SOCKET;
    }

    state->sockets[index] = slot->socket;
    state->sockets[index].in_use = 1;
    wsock_clear_handle_associations(&state->sockets[index]);
    ws_memset(slot, 0, sizeof(*slot));
    result = (SOCKET)(BASE_SOCKET + index);
    wsock_process_states_release();
    wsa_last_error = 0;
    wsock_socket_lifecycle_release();

    serial_puts("[WSOCK] consume duplicate target=");
    serial_putdec(target_pid);
    serial_puts(" socket=");
    serial_puthex((uint32_t)result, 4);
    serial_puts(" token=");
    serial_puthex(token, 8);
    serial_puts("\n");
    return result;
}

static SOCKET WINAPI wsock_WSASocketA(int af, int type, int protocol,
                                      PVOID protocol_info, DWORD group,
                                      DWORD flags)
{
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return INVALID_SOCKET;
    }
    if (group != 0 && group != 1) {
        wsa_last_error = WSAEINVAL;
        return INVALID_SOCKET;
    }
    if (flags & ~0x00000081U) { /* OVERLAPPED | NO_HANDLE_INHERIT */
        wsa_last_error = WSAEINVAL;
        return INVALID_SOCKET;
    }

    const wsock_protocol_info_header_t *info =
        (const wsock_protocol_info_header_t *)protocol_info;
    if (info && info->provider_reserved)
        return wsock_socket_from_duplicate(info);
    if (info) {
        if (af == -1) af = info->address_family;
        if (type == -1) type = info->socket_type;
        if (protocol == -1) protocol = info->protocol;
    } else if (af == -1 || type == -1 || protocol == -1) {
        wsa_last_error = WSAEINVAL;
        return INVALID_SOCKET;
    }
    return wsock_socket(af, type, protocol);
}

static int wsock_service_port(PCSTR service, uint16_t *port)
{
    if (!service) { *port = 0; return 0; }
    if (ws_strcmp(service, "http") == 0) { *port = 80; return 0; }
    if (ws_strcmp(service, "https") == 0) { *port = 443; return 0; }

    uint32_t value = 0;
    if (!*service) return -1;
    while (*service) {
        if (*service < '0' || *service > '9') return -1;
        value = value * 10 + (uint32_t)(*service++ - '0');
        if (value > 65535) return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

static volatile LONG g_getaddrinfo_trace_budget = 96;

#define WSOCK_ADDRINFO_MAGIC 0x4F5349544F414949ULL

typedef struct {
    uint64_t magic;
    uint32_t compat32;
    uint32_t reserved;
} wsock_addrinfo_header_t;

_Static_assert(sizeof(wsock_addrinfo_header_t) == 16,
               "addrinfo allocation header layout");

static int wsock_parse_ipv4_literal(PCSTR text, uint8_t ip[4])
{
    if (!text || !*text) return 0;
    const char *cursor = text;
    for (int part = 0; part < 4; part++) {
        if (*cursor < '0' || *cursor > '9') return 0;
        uint32_t value = 0;
        do {
            value = value * 10 + (uint32_t)(*cursor++ - '0');
            if (value > 255) return 0;
        } while (*cursor >= '0' && *cursor <= '9');
        ip[part] = (uint8_t)value;
        if (part < 3) {
            if (*cursor++ != '.') return 0;
        } else if (*cursor) {
            return 0;
        }
    }
    return 1;
}

static BOOL wsock_host_name_equal(PCSTR name, PCSTR expected)
{
    if (!name || !expected) return FALSE;
    while (*name && *expected) {
        unsigned char left = (unsigned char)*name++;
        unsigned char right = (unsigned char)*expected++;
        if (left >= 'A' && left <= 'Z') left = (unsigned char)(left + 32);
        if (right >= 'A' && right <= 'Z') right = (unsigned char)(right + 32);
        if (left != right) return FALSE;
    }
    if (*expected) return FALSE;
    return !*name || (*name == '.' && !name[1]);
}

static BOOL wsock_resolve_local_ipv4(PCSTR name, uint8_t ip[4])
{
    if (wsock_host_name_equal(name, "localhost")) {
        ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1;
        return TRUE;
    }
    if (wsock_host_name_equal(name, "osito")) {
        ws_memcpy(ip, net_get_ip_ptr(), 4);
        return TRUE;
    }
    return FALSE;
}

static PVOID wsock_addrinfo_alloc(BOOL compat32, int32_t flags,
                                  int32_t socktype, int32_t protocol,
                                  uint16_t port, const uint8_t ip[4],
                                  PCSTR canonical_name)
{
    SIZE_T info_size = compat32 ? sizeof(ADDRINFO32) : sizeof(ADDRINFO64);
    SIZE_T canonical_size = canonical_name
                          ? (SIZE_T)ws_strlen(canonical_name) + 1 : 0;
    SIZE_T total = sizeof(wsock_addrinfo_header_t) + info_size + 16 +
                   canonical_size;
    wsock_addrinfo_header_t *header =
        (wsock_addrinfo_header_t *)HeapAlloc(GetProcessHeap(), 0x8, total);
    if (!header) return NULL;

    header->magic = WSOCK_ADDRINFO_MAGIC;
    header->compat32 = compat32 ? 1U : 0U;
    BYTE *info = (BYTE *)(header + 1);
    BYTE *address = info + info_size;
    char *canonical = canonical_size ? (char *)(address + 16) : NULL;

    address[0] = AF_INET;
    address[2] = (uint8_t)(port >> 8);
    address[3] = (uint8_t)port;
    ws_memcpy(address + 4, ip, 4);
    if (canonical)
        ws_memcpy(canonical, canonical_name, canonical_size);

    if (compat32) {
        ADDRINFO32 *result = (ADDRINFO32 *)info;
        result->ai_flags = flags;
        result->ai_family = AF_INET;
        result->ai_socktype = socktype;
        result->ai_protocol = protocol;
        result->ai_addrlen = 16;
        result->ai_canonname = canonical
            ? (uint32_t)(uintptr_t)canonical : 0;
        result->ai_addr = (uint32_t)(uintptr_t)address;
    } else {
        ADDRINFO64 *result = (ADDRINFO64 *)info;
        result->ai_flags = flags;
        result->ai_family = AF_INET;
        result->ai_socktype = socktype;
        result->ai_protocol = protocol;
        result->ai_addrlen = 16;
        result->ai_canonname = canonical;
        result->ai_addr = address;
    }
    return info;
}

static void wsock_addrinfo_set_next(PVOID info, BOOL compat32, PVOID next)
{
    if (compat32)
        ((ADDRINFO32 *)info)->ai_next = (uint32_t)(uintptr_t)next;
    else
        ((ADDRINFO64 *)info)->ai_next = next;
}

static void WINAPI wsock_freeaddrinfo(PVOID info)
{
    for (int count = 0; info && count < 64; count++) {
        wsock_addrinfo_header_t *header =
            (wsock_addrinfo_header_t *)((BYTE *)info - sizeof(*header));
        if (header->magic != WSOCK_ADDRINFO_MAGIC) return;
        BOOL compat32 = header->compat32 != 0;
        PVOID next = compat32
            ? (PVOID)(uintptr_t)((ADDRINFO32 *)info)->ai_next
            : ((ADDRINFO64 *)info)->ai_next;
        header->magic = 0;
        HeapFree(GetProcessHeap(), 0, header);
        info = next;
    }
}

static int WINAPI wsock_getaddrinfo(PCSTR node, PCSTR service,
                                    PCVOID hints_ptr, PVOID result_ptr)
{
    uint8_t ip[4];
    uint16_t port;
    int32_t flags = 0, family = 0, socktype = 0, protocol = 0;
    BOOL compat32 = g_compat32_mode;

    if (!result_ptr || (!node && !service) || (node && !*node)) {
        wsa_last_error = WSAEINVAL;
        return WSAEINVAL;
    }
    if (compat32) {
        const ADDRINFO32 *hints = (const ADDRINFO32 *)hints_ptr;
        *(uint32_t *)result_ptr = 0;
        if (hints) {
            flags = hints->ai_flags;
            family = hints->ai_family;
            socktype = hints->ai_socktype;
            protocol = hints->ai_protocol;
        }
    } else {
        const ADDRINFO64 *hints = (const ADDRINFO64 *)hints_ptr;
        *(PVOID *)result_ptr = NULL;
        if (hints) {
            flags = hints->ai_flags;
            family = hints->ai_family;
            socktype = hints->ai_socktype;
            protocol = hints->ai_protocol;
        }
    }
    BOOL trace = __atomic_fetch_sub(&g_getaddrinfo_trace_budget, 1,
                                    __ATOMIC_RELAXED) > 0;
    if (trace) {
        serial_puts("[WSOCK-GAI] pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" mode=");
        serial_putdec(compat32 ? 32 : 64);
        serial_puts(" family=");
        serial_putdec((uint64_t)(uint32_t)family);
        serial_puts(" type=");
        serial_putdec((uint64_t)(uint32_t)socktype);
        serial_puts(" node='");
        serial_puts(node ? node : "(null)");
        serial_puts("' service='");
        serial_puts(service ? service : "");
        serial_puts("'\n");
    }
    if (family != 0 && family != AF_INET) {
        if (trace)
            serial_puts("[WSOCK-GAI-FAIL] unsupported address family\n");
        wsa_last_error = WSAEAFNOSUPPORT;
        return WSAEAFNOSUPPORT;
    }
    if (wsock_service_port(service, &port) < 0) {
        if (trace)
            serial_puts("[WSOCK-GAI-FAIL] unsupported service\n");
        wsa_last_error = WSATYPE_NOT_FOUND;
        return WSATYPE_NOT_FOUND;
    }

    if (!node) {
        if (flags & 1) { /* AI_PASSIVE */
            ip[0] = 0; ip[1] = 0; ip[2] = 0; ip[3] = 0;
        } else {
            ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1;
        }
    } else if (!wsock_parse_ipv4_literal(node, ip)) {
        if (flags & 4) { /* AI_NUMERICHOST */
            if (trace)
                serial_puts("[WSOCK-GAI-FAIL] non-numeric host\n");
            wsa_last_error = WSAHOST_NOT_FOUND;
            return WSAHOST_NOT_FOUND;
        }
        if (!wsock_resolve_local_ipv4(node, ip) &&
            net_dns_resolve(node, ip) < 0) {
            if (trace)
                serial_puts("[WSOCK-GAI-FAIL] DNS resolution failed\n");
            wsa_last_error = WSAHOST_NOT_FOUND;
            return WSAHOST_NOT_FOUND;
        }
    }

    int32_t result_types[2];
    int32_t result_protocols[2];
    int result_count = 0;
    if (!socktype) {
        if (!protocol || protocol == IPPROTO_TCP) {
            result_types[result_count] = SOCK_STREAM;
            result_protocols[result_count++] = IPPROTO_TCP;
        }
        if (!protocol || protocol == IPPROTO_UDP) {
            result_types[result_count] = SOCK_DGRAM;
            result_protocols[result_count++] = IPPROTO_UDP;
        }
        if (!result_count) {
            wsa_last_error = WSAEPROTONOSUPPORT;
            return WSAEPROTONOSUPPORT;
        }
    } else if (socktype == SOCK_STREAM) {
        if (protocol && protocol != IPPROTO_TCP) {
            wsa_last_error = WSAEPROTONOSUPPORT;
            return WSAEPROTONOSUPPORT;
        }
        result_types[0] = SOCK_STREAM;
        result_protocols[0] = IPPROTO_TCP;
        result_count = 1;
    } else if (socktype == SOCK_DGRAM) {
        if (protocol && protocol != IPPROTO_UDP) {
            wsa_last_error = WSAEPROTONOSUPPORT;
            return WSAEPROTONOSUPPORT;
        }
        result_types[0] = SOCK_DGRAM;
        result_protocols[0] = IPPROTO_UDP;
        result_count = 1;
    } else {
        if (trace)
            serial_puts("[WSOCK-GAI-FAIL] unsupported socket type\n");
        wsa_last_error = WSAESOCKTNOSUPPORT;
        return WSAESOCKTNOSUPPORT;
    }

    PVOID first = NULL;
    PVOID last = NULL;
    for (int i = 0; i < result_count; i++) {
        PCSTR canonical = i == 0 && node && (flags & 2) ? node : NULL;
        PVOID current = wsock_addrinfo_alloc(
            compat32, flags, result_types[i], result_protocols[i], port, ip,
            canonical);
        if (!current) {
            wsock_freeaddrinfo(first);
            wsa_last_error = 8; /* WSA_NOT_ENOUGH_MEMORY */
            return 8;
        }
        if (!first) first = current;
        if (last) wsock_addrinfo_set_next(last, compat32, current);
        last = current;
    }
    if (compat32)
        *(uint32_t *)result_ptr = (uint32_t)(uintptr_t)first;
    else
        *(PVOID *)result_ptr = first;

    wsa_last_error = 0;
    serial_puts("[WSOCK] getaddrinfo: ");
    serial_puts(node ? node : "(null)");
    serial_puts("\n");
    return 0;
}

static int WINAPI wsock_WSASendTo(SOCKET s, PCVOID buffers,
                                  DWORD count, DWORD *bytes_sent, DWORD flags,
                                  PVOID to, int tolen, PVOID overlapped,
                                  PVOID completion)
{
    if (bytes_sent) *bytes_sent = 0;
    if (!buffers || count == 0 || count > 64) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    if (!overlapped && !bytes_sent) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    if (completion) {
        wsa_last_error = WSAEOPNOTSUPP;
        return SOCKET_ERROR;
    }
    for (DWORD i = 0; i < count; i++) {
        if (wsabuf_len(buffers, i) && !wsabuf_data(buffers, i)) {
            wsa_last_error = WSAEFAULT;
            return SOCKET_ERROR;
        }
    }

    int socket_index = sock_to_index(s);
    if (socket_index < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }

    if (win32_sockets[socket_index].type == SOCK_DGRAM) {
        uint64_t datagram_length = 0;
        for (DWORD i = 0; i < count; i++) {
            uint32_t length = wsabuf_len(buffers, i);
            if (length && !wsabuf_data(buffers, i)) {
                wsa_last_error = WSAEFAULT;
                return SOCKET_ERROR;
            }
            datagram_length += length;
            if (datagram_length > 65507U) {
                wsa_last_error = WSAEMSGSIZE;
                return SOCKET_ERROR;
            }
        }

        BYTE *datagram = NULL;
        if (datagram_length) {
            datagram = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
                                         (SIZE_T)datagram_length);
            if (!datagram) {
                wsa_last_error = WSAENOBUFS;
                return SOCKET_ERROR;
            }
        }
        DWORD offset = 0;
        for (DWORD i = 0; i < count; i++) {
            uint32_t length = wsabuf_len(buffers, i);
            if (!length) continue;
            ws_memcpy(datagram + offset, wsabuf_data(buffers, i), length);
            offset += length;
        }
        int sent = wsock_sendto(s, (PCSTR)datagram, (int)datagram_length,
                                (int)flags, to, tolen);
        if (datagram) HeapFree(GetProcessHeap(), 0, datagram);
        if (sent == SOCKET_ERROR) return SOCKET_ERROR;
        if (bytes_sent) *bytes_sent = (DWORD)sent;
        if (overlapped)
            k32_iocp_complete_handle((HANDLE)(ULONG_PTR)s, (DWORD)sent,
                                     overlapped);
        wsa_last_error = 0;
        return 0;
    }

    DWORD total = 0;
    for (DWORD i = 0; i < count; i++) {
        uint32_t len = wsabuf_len(buffers, i);
        PVOID data = wsabuf_data(buffers, i);
        if (!len) continue;
#if !defined(OK_QUIET) || !OK_QUIET
        serial_puts("[WSOCK] WSASend len=");
        serial_puthex(len, 8);
        serial_puts(" buf=");
        serial_puthex((uint64_t)(uintptr_t)data, 16);
        serial_puts("\n");
#endif
        int trace_index = sock_to_index(s);
        if (trace_index >= 0)
            wsock_trace_loopback_payload("TX", &win32_sockets[trace_index],
                                         data, len);
        int sent = wsock_sendto(s, (PCSTR)data, (int)len, (int)flags,
                                to, tolen);
        if (sent == SOCKET_ERROR) {
            if (!total) return SOCKET_ERROR;
            break;
        }
        total += (DWORD)sent;
        if ((DWORD)sent < len) break;
    }
    if (bytes_sent) *bytes_sent = total;
    if (overlapped) {
#if !defined(OK_QUIET) || !OK_QUIET
        serial_puts("[WSOCK] WSASend complete bytes=");
        serial_puthex(total, 8);
        serial_puts(" overlapped=");
        serial_puthex((uint64_t)(uintptr_t)overlapped, 16);
        serial_puts("\n");
#endif
        k32_iocp_complete_handle((HANDLE)(ULONG_PTR)s, total, overlapped);
    }
    wsa_last_error = 0;
    return 0;
}

static int WINAPI wsock_WSASend(SOCKET s, PCVOID buffers,
                                DWORD count, DWORD *bytes_sent, DWORD flags,
                                PVOID overlapped, PVOID completion)
{
    return wsock_WSASendTo(s, buffers, count, bytes_sent, flags, NULL, 0,
                           overlapped, completion);
}

static int WINAPI wsock_WSARecvFrom(SOCKET s, PCVOID buffers,
                                    DWORD count, DWORD *bytes_received,
                                    DWORD *flags, PVOID from, int *fromlen,
                                    PVOID overlapped, PVOID completion)
{
    if (!buffers || count == 0 || count > MAX_WSARECV_BUFFERS) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    if (!flags || (!overlapped && !bytes_received)) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    if (completion) {
        wsa_last_error = WSAEOPNOTSUPP;
        return SOCKET_ERROR;
    }

    int socket_index = sock_to_index(s);
    if (socket_index < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }

    BOOL compat32 = g_compat32_mode;
    wsock_captured_buffer_t captured[MAX_WSARECV_BUFFERS];
    DWORD capacity = 0;
    int capture_error = wsock_capture_recv_buffers(
        buffers, count, compat32, captured, &capacity);
    if (capture_error) {
        wsa_last_error = capture_error;
        return SOCKET_ERROR;
    }

    win32_socket_t *socket = &win32_sockets[socket_index];
    DWORD owner_pid = win32_current_process_id();
    if (socket->type == SOCK_DGRAM) {
        if (*flags & ~WSOCK_MSG_PEEK) {
            wsa_last_error = WSAEOPNOTSUPP;
            return SOCKET_ERROR;
        }
        if (socket->shutdown_receive) {
            wsa_last_error = WSAESHUTDOWN;
            return SOCKET_ERROR;
        }
        if (from) {
            int required = wsock_sockaddr_length(socket);
            if (!fromlen) {
                wsa_last_error = WSAEFAULT;
                return SOCKET_ERROR;
            }
            if (*fromlen < required) {
                *fromlen = required;
                wsa_last_error = WSAEFAULT;
                return SOCKET_ERROR;
            }
        }
        if (wsock_bind_implicit(socket) < 0) {
            wsa_last_error = WSAENOBUFS;
            return SOCKET_ERROR;
        }

        net_poll();
        BOOL queue_behind_pending = FALSE;
        if (overlapped) {
            wsock_service_pending_recvs(owner_pid);
            queue_behind_pending = wsock_socket_has_pending_recv(
                owner_pid, s, socket->generation);
        }
        int pending = kernel_sock_has_pending_data(socket->kern_socket_id);
        if (pending > 0 && !queue_behind_pending) {
            DWORD received = 0;
            DWORD output_flags = 0;
            int error = wsock_recv_datagram_captured_now(
                socket, captured, count, capacity, *flags, from, fromlen,
                &received, &output_flags);
            if (bytes_received) *bytes_received = received;
            *flags = output_flags;
            if (overlapped) {
                NTSTATUS status = wsock_status_from_error(error);
                wsock_set_overlapped_status(overlapped, compat32, status,
                                            received);
                if (!error)
                    k32_iocp_complete_handle((HANDLE)(ULONG_PTR)s, received,
                                             overlapped);
            }
            if (error) {
                wsa_last_error = error;
                return SOCKET_ERROR;
            }
            wsa_last_error = 0;
            return 0;
        }
        if (pending < 0) {
            wsa_last_error = WSAENOTSOCK;
            return SOCKET_ERROR;
        }

        if (!overlapped) {
            ULONGLONG deadline = socket->recv_timeout_ms
                ? GetTickCount64() + (DWORD)socket->recv_timeout_ms : 0;
            while (kernel_sock_has_pending_data(socket->kern_socket_id) == 0) {
                if (socket->nonblocking) {
                    wsa_last_error = WSAEWOULDBLOCK;
                    return SOCKET_ERROR;
                }
                if (deadline && GetTickCount64() >= deadline) {
                    wsa_last_error = WSAETIMEDOUT;
                    return SOCKET_ERROR;
                }
                if (sched_sleep_ticks(1) < 0) sched_yield();
            }

            DWORD received = 0;
            DWORD output_flags = 0;
            int error = wsock_recv_datagram_captured_now(
                socket, captured, count, capacity, *flags, from, fromlen,
                &received, &output_flags);
            *bytes_received = received;
            *flags = output_flags;
            if (error) {
                wsa_last_error = error;
                return SOCKET_ERROR;
            }
            wsa_last_error = 0;
            return 0;
        }
    } else if (overlapped) {
        if (!socket->connected || socket->kern_conn_id < 0) {
            wsa_last_error = WSAENOTCONN;
            return SOCKET_ERROR;
        }
        if (*flags) {
            wsa_last_error = WSAEOPNOTSUPP;
            return SOCKET_ERROR;
        }

        BOOL has_capacity = capacity != 0;

        net_poll();
        wsock_service_pending_recvs(owner_pid);
        BOOL queue_behind_pending = wsock_socket_has_pending_recv(
            owner_pid, s, socket->generation);
        DWORD total = has_capacity && !queue_behind_pending
                     ? wsock_recv_buffers_now(socket, buffers, count, compat32)
                     : 0;
        int tcp_state = net_tcp_state(socket->kern_conn_id);
        if (!has_capacity ||
            (!queue_behind_pending &&
             (total || wsock_tcp_eof(tcp_state)))) {
            if (bytes_received) *bytes_received = total;
            if (flags) *flags = 0;
            wsock_set_overlapped_status(overlapped, compat32,
                                        STATUS_SUCCESS, total);
            k32_iocp_complete_handle((HANDLE)(ULONG_PTR)s, total, overlapped);
            wsa_last_error = 0;
            return 0;
        }

        wsock_pending_recv_t *slot = NULL;
        for (int i = 0; i < MAX_PENDING_WSARECV; i++) {
            LONG expected = 0;
            if (__atomic_compare_exchange_n(&g_pending_recvs[i].state,
                                            &expected, 3, FALSE,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                slot = &g_pending_recvs[i];
                break;
            }
        }
        if (!slot) {
            wsa_last_error = WSAENOBUFS;
            return SOCKET_ERROR;
        }

        slot->owner_pid = win32_current_process_id();
        slot->owner_tid = GetCurrentThreadId();
        slot->compat32 = compat32;
        slot->socket = s;
        slot->socket_generation = socket->generation;
        slot->datagram = FALSE;
        slot->count = count;
        for (DWORD i = 0; i < count; i++) {
            slot->buffers[i] = captured[i];
        }
        slot->input_flags = 0;
        slot->from = NULL;
        slot->fromlen = NULL;
        slot->overlapped = overlapped;
        slot->sequence = __atomic_add_fetch(
            &g_pending_recv_next_sequence, 1, __ATOMIC_RELAXED);
        wsock_set_overlapped_status(overlapped, compat32, STATUS_PENDING, 0);
        __atomic_store_n(&slot->state, 1, __ATOMIC_RELEASE);

        serial_puts("[WSOCK] WSARecv pending socket=");
        serial_puthex((uint32_t)s, 4);
        serial_puts(" owner=");
        serial_puthex(slot->owner_pid, 8);
        serial_puts(" buffers=");
        serial_puthex(count, 4);
        serial_puts(" overlapped=");
        serial_puthex((uint64_t)(uintptr_t)overlapped, 16);
        serial_puts("\n");

        wsa_last_error = 997; /* WSA_IO_PENDING */
        return SOCKET_ERROR;
    }

    if (socket->type == SOCK_DGRAM) {
        wsock_pending_recv_t *slot = NULL;
        for (int i = 0; i < MAX_PENDING_WSARECV; i++) {
            LONG expected = 0;
            if (__atomic_compare_exchange_n(&g_pending_recvs[i].state,
                                            &expected, 3, FALSE,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                slot = &g_pending_recvs[i];
                break;
            }
        }
        if (!slot) {
            wsa_last_error = WSAENOBUFS;
            return SOCKET_ERROR;
        }

        slot->owner_pid = win32_current_process_id();
        slot->owner_tid = GetCurrentThreadId();
        slot->compat32 = compat32;
        slot->socket = s;
        slot->socket_generation = socket->generation;
        slot->datagram = TRUE;
        slot->count = count;
        for (DWORD i = 0; i < count; i++) slot->buffers[i] = captured[i];
        slot->input_flags = *flags;
        slot->from = from;
        slot->fromlen = fromlen;
        slot->overlapped = overlapped;
        slot->sequence = __atomic_add_fetch(
            &g_pending_recv_next_sequence, 1, __ATOMIC_RELAXED);
        wsock_set_overlapped_status(overlapped, compat32, STATUS_PENDING, 0);
        __atomic_store_n(&slot->state, 1, __ATOMIC_RELEASE);
        wsa_last_error = 997; /* WSA_IO_PENDING */
        return SOCKET_ERROR;
    }

    if (!capacity) {
        *bytes_received = 0;
        *flags = 0;
        wsa_last_error = 0;
        return 0;
    }
    if (capacity > 0x7FFFFFFFU) {
        wsa_last_error = WSAEMSGSIZE;
        return SOCKET_ERROR;
    }
    BYTE *stream_data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, capacity);
    if (!stream_data) {
        wsa_last_error = WSAENOBUFS;
        return SOCKET_ERROR;
    }
    int received = wsock_recv(s, (PSTR)stream_data, (int)capacity,
                              (int)*flags);
    if (received == SOCKET_ERROR) {
        HeapFree(GetProcessHeap(), 0, stream_data);
        return SOCKET_ERROR;
    }
    DWORD total = (DWORD)received;
    DWORD offset = 0;
    for (DWORD i = 0; i < count && offset < total; i++) {
        DWORD amount = captured[i].length;
        if (amount > total - offset) amount = total - offset;
        if (amount) {
            ws_memcpy(captured[i].data, stream_data + offset, amount);
            offset += amount;
        }
    }
    HeapFree(GetProcessHeap(), 0, stream_data);
    if (bytes_received) *bytes_received = total;
    *flags = 0;
    wsa_last_error = 0;
    return 0;
}

static int WINAPI wsock_WSARecv(SOCKET s, PCVOID buffers,
                                DWORD count, DWORD *bytes_received,
                                DWORD *flags, PVOID overlapped,
                                PVOID completion)
{
    return wsock_WSARecvFrom(s, buffers, count, bytes_received, flags,
                             NULL, NULL, overlapped, completion);
}

static int WINAPI wsock_WSASendMsg(SOCKET s, PVOID message, DWORD flags,
                                    DWORD *bytes_sent, PVOID overlapped,
                                    PVOID completion)
{
    wsock_message_view_t view;
    int error = wsock_message_view(message, g_compat32_mode, &view);
    if (error) {
        wsa_last_error = error;
        return SOCKET_ERROR;
    }
    if (*view.control_length) {
        wsa_last_error = view.control ? WSAEOPNOTSUPP : WSAEFAULT;
        return SOCKET_ERROR;
    }
    return wsock_WSASendTo(s, view.buffers, view.buffer_count, bytes_sent,
                           flags, view.name, *view.namelen, overlapped,
                           completion);
}

static int WINAPI wsock_WSARecvMsg(SOCKET s, PVOID message,
                                    DWORD *bytes_received, PVOID overlapped,
                                    PVOID completion)
{
    wsock_message_view_t view;
    int error = wsock_message_view(message, g_compat32_mode, &view);
    if (error) {
        wsa_last_error = error;
        return SOCKET_ERROR;
    }
    if (*view.control_length && !view.control) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    *view.control_length = 0;
    return wsock_WSARecvFrom(s, view.buffers, view.buffer_count,
                             bytes_received, view.flags, view.name,
                             view.name ? view.namelen : NULL, overlapped,
                             completion);
}

static int wsock_overlapped_status(PVOID overlapped, NTSTATUS *status,
                                   DWORD *bytes, HANDLE *event)
{
    if (!overlapped)
        return 0;
    if (g_compat32_mode) {
        volatile uint32_t *values = (volatile uint32_t *)overlapped;
        *status = (NTSTATUS)values[0];
        *bytes = values[1];
        *event = (HANDLE)(ULONG_PTR)values[4];
    } else {
        volatile ULONG_PTR *values = (volatile ULONG_PTR *)overlapped;
        *status = (NTSTATUS)values[0];
        *bytes = (DWORD)values[1];
        *event = (HANDLE)values[3];
    }
    return 1;
}

static BOOL WINAPI wsock_WSAGetOverlappedResult(SOCKET socket,
                                                 PVOID overlapped,
                                                 DWORD *bytes_transferred,
                                                 BOOL wait,
                                                 DWORD *flags)
{
    if (sock_to_index(socket) < 0) {
        wsa_last_error = WSAENOTSOCK;
        return FALSE;
    }
    if (!bytes_transferred) {
        wsa_last_error = WSAEFAULT;
        return FALSE;
    }

    NTSTATUS status;
    DWORD bytes;
    HANDLE event;
    if (!wsock_overlapped_status(overlapped, &status, &bytes, &event)) {
        wsa_last_error = WSAEFAULT;
        return FALSE;
    }
    if (status == STATUS_PENDING && wait) {
        if (!event ||
            WaitForSingleObject(event, WSOCK_INFINITE) == WSOCK_WAIT_FAILED) {
            wsa_last_error = WSAEINVAL;
            return FALSE;
        }
        wsock_overlapped_status(overlapped, &status, &bytes, &event);
    }
    if (status == STATUS_PENDING) {
        wsa_last_error = 996; /* WSA_IO_INCOMPLETE */
        return FALSE;
    }
    *bytes_transferred = bytes;
    if (flags)
        *flags = status == STATUS_BUFFER_OVERFLOW ? WSOCK_MSG_PARTIAL : 0;
    if (!NT_SUCCESS(status)) {
        wsa_last_error = wsock_status_error(status);
        return FALSE;
    }

    wsa_last_error = 0;
    return TRUE;
}

/* ── socket() ────────────────────────────────────────────── */

SOCKET WINAPI wsock_socket(int af, int type, int protocol)
{
    wsock_process_state_t *state = wsock_state_current();
    wsock_socket_lifecycle_acquire();
    if (state->startup_refs <= 0) {
        wsa_last_error = WSANOTINITIALISED;
        wsock_socket_lifecycle_release();
        return INVALID_SOCKET;
    }

    if (af != AF_INET && af != AF_INET6) {
        wsa_last_error = WSAEAFNOSUPPORT;
        wsock_socket_lifecycle_release();
        return INVALID_SOCKET;
    }
    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        wsa_last_error = WSAESOCKTNOSUPPORT;
        wsock_socket_lifecycle_release();
        return INVALID_SOCKET;
    }
    if (!protocol)
        protocol = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    if ((type == SOCK_STREAM && protocol != IPPROTO_TCP) ||
        (type == SOCK_DGRAM && protocol != IPPROTO_UDP)) {
        wsa_last_error = WSAEPROTONOSUPPORT;
        wsock_socket_lifecycle_release();
        return INVALID_SOCKET;
    }

    /* Keep allocation and close cleanup mutually exclusive.  A SOCKET value
     * is visible to other threads as soon as this function returns, so its
     * slot must be fully initialized before another allocator can reuse it. */
    int idx = -1;
    for (int i = 0; i < MAX_WIN32_SOCKETS; i++) {
        if (!state->sockets[i].in_use) { idx = i; break; }
    }
    if (idx < 0) {
        wsock_socket_lifecycle_release();
        serial_puts("[WSOCK] socket: no free slots\n");
        wsa_last_error = WSAEMFILE;
        return INVALID_SOCKET;
    }

    win32_socket_t *ws = &state->sockets[idx];
    wsock_initialize_socket(ws, af, type, protocol);
    if (type == SOCK_DGRAM) {
        ws->kern_socket_id = kernel_sock_socket(AF_INET, SOCK_DGRAM,
                                                 IPPROTO_UDP);
        if (ws->kern_socket_id < 0) {
            ws->in_use = 0;
            ws->kern_socket_id = -1;
            wsock_socket_lifecycle_release();
            wsa_last_error = WSAENOBUFS;
            return INVALID_SOCKET;
        }
    }
    wsock_socket_lifecycle_release();

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

static int wsock_parse_ipv4_endpoint(const uint8_t *sa, int namelen,
                                     uint8_t ip[4], uint16_t *port)
{
    if (!sa || !ip || !port || namelen < 4)
        return WSAEFAULT;

    uint16_t family = (uint16_t)sa[0] | ((uint16_t)sa[1] << 8);
    *port = (uint16_t)((uint16_t)sa[2] << 8) | sa[3];
    if (family == AF_INET) {
        if (namelen < 8) return WSAEFAULT;
        ws_memcpy(ip, sa + 4, 4);
        return 0;
    }
    if (family != AF_INET6)
        return WSAEAFNOSUPPORT;
    if (namelen < 24)
        return WSAEFAULT;

    const uint8_t *addr = sa + 8;
    BOOL first_ten_zero = TRUE;
    BOOL first_twelve_zero = TRUE;
    BOOL first_fifteen_zero = TRUE;
    for (int i = 0; i < 15; i++) {
        if (addr[i] != 0) first_fifteen_zero = FALSE;
        if (i < 12 && addr[i] != 0) first_twelve_zero = FALSE;
        if (i < 10 && addr[i] != 0) first_ten_zero = FALSE;
    }

    if (first_fifteen_zero && addr[15] == 1) {
        ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1;
        return 0;
    }
    if ((first_ten_zero && addr[10] == 0xFF && addr[11] == 0xFF) ||
        first_twelve_zero) {
        ws_memcpy(ip, addr + 12, 4);
        return 0;
    }
    return WSAEAFNOSUPPORT;
}

static int wsock_prepare_connect(SOCKET s, PVOID name, int namelen,
                                 win32_socket_t **socket_out,
                                 uint8_t ip[4], uint16_t *port_out)
{
    int idx = sock_to_index(s);
    if (idx < 0) return WSAENOTSOCK;

    win32_socket_t *ws = &win32_sockets[idx];
    if (!name) return WSAEFAULT;
    if (ws->connected) return WSAEISCONN;
    if (ws->connecting) return WSAEALREADY;
    if (ws->listening) return WSAEOPNOTSUPP;

    uint8_t *sa = (uint8_t *)name;
    uint16_t port_be;
    int parse_error = wsock_parse_ipv4_endpoint(sa, namelen, ip, &port_be);
    if (parse_error) return parse_error;
    if (ws->af == AF_INET6 &&
        (ws->option_flags & WSOCK_OPT_IPV6ONLY))
        return WSAEAFNOSUPPORT;

    ws_memcpy(ws->remote_ip, ip, 4);
    ws->remote_port  = port_be;

    if (wsock_bind_implicit(ws) < 0)
        return WSAENOBUFS;
    if (!ws->local_ip[0] && !ws->local_ip[1] &&
        !ws->local_ip[2] && !ws->local_ip[3]) {
        if (ip[0] == 127) {
            ws->local_ip[0] = 127;
            ws->local_ip[1] = 0;
            ws->local_ip[2] = 0;
            ws->local_ip[3] = 1;
        } else {
            ws_memcpy(ws->local_ip, net_get_ip_ptr(), 4);
        }
    }

    serial_puts("[WSOCK] connect to ");
    serial_puthex(ip[0], 2); serial_puts(".");
    serial_puthex(ip[1], 2); serial_puts(".");
    serial_puthex(ip[2], 2); serial_puts(".");
    serial_puthex(ip[3], 2); serial_puts(":");
    serial_puthex(port_be, 4); serial_puts("\n");

    *socket_out = ws;
    *port_out = port_be;
    return 0;
}

int WINAPI wsock_connect(SOCKET s, PVOID name, int namelen)
{
    win32_socket_t *ws;
    uint16_t port_be;
    uint8_t ip[4];
    int prepare_error = wsock_prepare_connect(s, name, namelen, &ws, ip,
                                              &port_be);
    if (prepare_error) {
        wsa_last_error = prepare_error;
        return SOCKET_ERROR;
    }

    if (ws->type == SOCK_STREAM) {
        int conn = ws->nonblocking
                 ? net_tcp_connect_begin(ip, port_be, ws->local_port)
                 : net_tcp_connect(ip, port_be, ws->local_port);
        if (conn < 0) {
            serial_puts("[WSOCK] connect: kernel TCP connect failed\n");
            ws->connecting = 0;
            wsock_signal_socket_error(ws, WSAECONNREFUSED,
                                      WSOCK_FD_CONNECT);
            wsa_last_error = WSAECONNREFUSED;
            return SOCKET_ERROR;
        }
        ws->kern_conn_id = conn;
        if (ws->nonblocking &&
            net_tcp_state(conn) == KERN_TCP_SYN_SENT) {
            ws->connected = 0;
            ws->connecting = 1;
            ws->pending_error = 0;
            wsock_propagate_shared_state(ws);
            wsock_refresh_socket_event(ws);
            serial_puts("[WSOCK] connect: pending, kern_conn=");
            serial_puthex(conn, 2); serial_puts("\n");
            wsa_last_error = WSAEWOULDBLOCK;
            return SOCKET_ERROR;
        }
        ws->connected = 1;
        ws->connecting = 0;
        ws->connected_at_ms = GetTickCount();
        ws->pending_error = 0;
        wsock_apply_keepalive(ws);
        wsock_wake_ready_pending_accepts();
        wsock_service_pending_accepts(win32_current_process_id());
        wsock_refresh_socket_event(ws);
        serial_puts("[WSOCK] connect: OK, kern_conn=");
        serial_puthex(conn, 2); serial_puts("\n");
    } else {
        uint8_t endpoint[16];
        wsock_store_sockaddr(endpoint, ip, port_be);
        if (ws->kern_socket_id < 0 ||
            kernel_sock_connect(ws->kern_socket_id, endpoint) < 0) {
            wsa_last_error = WSAENETDOWN;
            return SOCKET_ERROR;
        }
        ws->connected = 1;
        ws->connecting = 0;
        ws->connected_at_ms = GetTickCount();
        ws->pending_error = 0;
        wsock_refresh_socket_event(ws);
    }

    wsock_propagate_shared_state(ws);
    wsa_last_error = 0;
    return 0;
}

/* ── send() ──────────────────────────────────────────────── */

int WINAPI wsock_send(SOCKET s, PCSTR buf, int len, int flags)
{
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }

    win32_socket_t *ws = &win32_sockets[idx];
    if (!ws->connected) { wsa_last_error = WSAENOTCONN; return SOCKET_ERROR; }
    if (ws->shutdown_send) { wsa_last_error = WSAESHUTDOWN; return SOCKET_ERROR; }
    if (len < 0) { wsa_last_error = WSAEINVAL; return SOCKET_ERROR; }
    if (len && !buf) { wsa_last_error = WSAEFAULT; return SOCKET_ERROR; }
    if (flags) { wsa_last_error = WSAEOPNOTSUPP; return SOCKET_ERROR; }
    if (!len) { wsa_last_error = 0; return 0; }

#if !defined(OK_QUIET) || !OK_QUIET
    serial_puts("[WSOCK] send len=");
    serial_puthex((uint32_t)len, 8);
    serial_puts("\n");
#endif

    if (ws->type == SOCK_STREAM) {
        if (ws->kern_conn_id < 0) { wsa_last_error = WSAENOTCONN; return SOCKET_ERROR; }
        ULONGLONG deadline = ws->send_timeout_ms
                           ? GetTickCount64() + (DWORD)ws->send_timeout_ms : 0;
        int sent = net_tcp_send(ws->kern_conn_id, buf, (uint32_t)len);
        while (sent == 0 && len > 0 && !ws->nonblocking &&
               net_tcp_state(ws->kern_conn_id) == KERN_TCP_ESTABLISHED) {
            if (deadline && GetTickCount64() >= deadline)
                break;
            Sleep(1);
            sent = net_tcp_send(ws->kern_conn_id, buf, (uint32_t)len);
        }
        if (sent < 0) {
            int error = wsock_tcp_send_error(ws);
            serial_puts("[WSOCK-SEND-FAIL] socket=");
            serial_puthex((uint32_t)s, 8);
            serial_puts(" conn=");
            serial_putdec((uint32_t)ws->kern_conn_id);
            serial_puts(" state=");
            serial_putdec((uint32_t)net_tcp_state(ws->kern_conn_id));
            serial_puts(" kernel_error=");
            serial_putdec((uint32_t)net_tcp_error(ws->kern_conn_id));
            serial_puts(" wsa_error=");
            serial_putdec((uint32_t)error);
            serial_puts(" len=");
            serial_putdec((uint32_t)len);
            serial_puts("\n");
            wsock_signal_socket_error(ws, error, WSOCK_FD_CLOSE);
            wsa_last_error = error;
            return SOCKET_ERROR;
        }
        if (sent == 0 && len > 0) {
            wsock_refresh_socket_event(ws);
            int error = deadline && GetTickCount64() >= deadline
                      ? WSAETIMEDOUT : WSAEWOULDBLOCK;
            if (error == WSAETIMEDOUT)
                ws->pending_error = error;
            wsa_last_error = error;
            return SOCKET_ERROR;
        }
        wsock_wake_ready_pending_recvs();
        DWORD owner_pid = win32_current_process_id();
        wsock_service_pending_accepts(owner_pid);
        wsock_service_pending_recvs(owner_pid);
        wsock_refresh_socket_event(ws);
        wsa_last_error = 0;
        return sent;
    } else {
        return wsock_sendto(s, buf, len, flags, NULL, 0);
    }
}

/* ── recv() ──────────────────────────────────────────────── */

static int wsock_wait_for_receive(win32_socket_t *ws, int minimum)
{
    ULONGLONG deadline = ws->recv_timeout_ms
                       ? GetTickCount64() + (DWORD)ws->recv_timeout_ms : 0;

    for (;;) {
        net_poll();
        int available = net_tcp_rx_available(ws->kern_conn_id);
        if (available >= minimum || (available > 0 && ws->nonblocking))
            return available;

        int state = net_tcp_state(ws->kern_conn_id);
        if (wsock_tcp_eof(state))
            return available > 0 ? available : -1;
        if (ws->nonblocking)
            return 0;
        if (deadline && GetTickCount64() >= deadline)
            return available;
        if (sched_sleep_ticks(1) < 0)
            sched_yield();
    }
}

static int wsock_recv_waitall(win32_socket_t *ws, PSTR buffer, int length,
                              int *receive_error)
{
    ULONGLONG deadline = ws->recv_timeout_ms
                       ? GetTickCount64() + (DWORD)ws->recv_timeout_ms : 0;
    int total = 0;
    *receive_error = 0;

    while (total < length) {
        net_poll();
        int available = net_tcp_rx_available(ws->kern_conn_id);
        if (available > 0) {
            int amount = length - total;
            if (amount > available) amount = available;
            int received = net_tcp_recv(ws->kern_conn_id, buffer + total,
                                        (uint32_t)amount);
            if (received < 0) {
                if (!total) *receive_error = WSAECONNRESET;
                break;
            }
            if (!received) {
                if (!total) *receive_error = WSAEWOULDBLOCK;
                break;
            }
            total += received;
            continue;
        }

        if (wsock_tcp_eof(net_tcp_state(ws->kern_conn_id)))
            break;
        if (ws->nonblocking) {
            if (!total) *receive_error = WSAEWOULDBLOCK;
            break;
        }
        if (deadline && GetTickCount64() >= deadline) {
            if (!total) *receive_error = WSAETIMEDOUT;
            break;
        }
        if (sched_sleep_ticks(1) < 0) sched_yield();
    }
    return total;
}

int WINAPI wsock_recv(SOCKET s, PSTR buf, int len, int flags)
{
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }

    win32_socket_t *ws = &win32_sockets[idx];
    if (ws->type == SOCK_DGRAM)
        return wsock_recvfrom(s, buf, len, flags, NULL, NULL);
    if (!ws->connected || ws->kern_conn_id < 0) {
        wsa_last_error = WSAENOTCONN;
        return SOCKET_ERROR;
    }
    if (ws->shutdown_receive) {
        wsa_last_error = WSAESHUTDOWN;
        return SOCKET_ERROR;
    }
    if (len < 0) { wsa_last_error = WSAEINVAL; return SOCKET_ERROR; }
    if (len && !buf) { wsa_last_error = WSAEFAULT; return SOCKET_ERROR; }
    if ((flags & WSOCK_MSG_OOB) ||
        (flags & ~(WSOCK_MSG_PEEK | WSOCK_MSG_WAITALL | WSOCK_MSG_OOB))) {
        wsa_last_error = WSAEOPNOTSUPP;
        return SOCKET_ERROR;
    }
    if (!len) { wsa_last_error = 0; return 0; }

    if ((flags & (WSOCK_MSG_WAITALL | WSOCK_MSG_PEEK)) ==
            (WSOCK_MSG_WAITALL | WSOCK_MSG_PEEK) &&
        len > WSOCK_TCP_RECV_CAPACITY) {
        wsa_last_error = WSAEMSGSIZE;
        return SOCKET_ERROR;
    }
    if ((flags & WSOCK_MSG_WAITALL) && !(flags & WSOCK_MSG_PEEK)) {
        int receive_error = 0;
        int received = wsock_recv_waitall(ws, buf, len, &receive_error);
        wsock_rearm_socket_event(ws, WSOCK_FD_READ);
        if (receive_error) {
            if (receive_error == WSAECONNRESET)
                wsock_signal_socket_error(ws, receive_error,
                                          WSOCK_FD_CLOSE);
            if (receive_error == WSAETIMEDOUT)
                ws->pending_error = receive_error;
            wsa_last_error = receive_error;
            return SOCKET_ERROR;
        }
        if (received)
            wsock_trace_loopback_payload("RX", ws, buf, (DWORD)received);
        wsa_last_error = 0;
        return received;
    }

    int minimum = flags & WSOCK_MSG_WAITALL ? len : ws->recv_low_water;
    if (minimum > len) minimum = len;
    if (minimum < 1) minimum = 1;
    int available = wsock_wait_for_receive(ws, minimum);
    int rcvd = 0;
    if (available > 0) {
        rcvd = flags & WSOCK_MSG_PEEK
             ? net_tcp_peek(ws->kern_conn_id, buf, (uint32_t)len)
             : net_tcp_recv(ws->kern_conn_id, buf, (uint32_t)len);
    } else if (available < 0) {
        wsock_rearm_socket_event(ws, WSOCK_FD_READ);
        wsa_last_error = 0;
        return 0;
    }
    if (wsock_trace_loopback_control(ws)) {
        serial_puts("[WSOCK-RECV] pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" socket=");
        serial_puthex((uint32_t)s, 4);
        serial_puts(" conn=");
        serial_putdec((uint64_t)ws->kern_conn_id);
        serial_puts(" len=");
        serial_puthex((uint32_t)len, 8);
        serial_puts(" flags=");
        serial_puthex((uint32_t)flags, 8);
        serial_puts(" available=");
        serial_puthex((uint32_t)available, 8);
        serial_puts(" result=");
        serial_puthex((uint32_t)rcvd, 8);
        serial_puts("\n");
    }
    if (rcvd < 0) {
        /* Connection closed */
        wsock_rearm_socket_event(ws, WSOCK_FD_READ);
        wsock_signal_socket_error(ws, WSAECONNRESET, WSOCK_FD_CLOSE);
        wsa_last_error = WSAECONNRESET;
        return SOCKET_ERROR;
    }
    if (rcvd == 0) {
        wsock_rearm_socket_event(ws, WSOCK_FD_READ);
        int error = ws->nonblocking ? WSAEWOULDBLOCK : WSAETIMEDOUT;
        if (error == WSAETIMEDOUT)
            ws->pending_error = error;
        wsa_last_error = error;
        return SOCKET_ERROR;
    }

    wsock_trace_loopback_payload("RX", ws, buf, (DWORD)rcvd);
    wsock_rearm_socket_event(ws, WSOCK_FD_READ);
    wsa_last_error = 0;
    return rcvd;
}

/* ── closesocket() ───────────────────────────────────────── */

int WINAPI closesocket(SOCKET s)
{
    wsock_process_state_t *state = wsock_state_current();
    wsock_socket_lifecycle_acquire();
    int idx = wsock_raw_index(s);
    if (idx < 0 || !state->sockets[idx].in_use) {
        wsa_last_error = WSAENOTSOCK;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }

    win32_socket_t *ws = &state->sockets[idx];
    win32_socket_t closing = *ws;
    BOOL last_reference;
    wsock_process_states_acquire();
    ws->in_use = 0;
    last_reference = !wsock_backend_has_reference(&closing);
    if (last_reference && closing.kern_conn_id >= 0 &&
        closing.linger_enabled && closing.linger_seconds &&
        closing.nonblocking) {
        ws->in_use = 1;
        wsock_process_states_release();
        wsa_last_error = WSAEWOULDBLOCK;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }
    wsock_process_states_release();

    wsock_cancel_io(s, NULL, FALSE);
    closing = *ws;

    if (last_reference && closing.kern_conn_id >= 0) {
        if (closing.linger_enabled) {
            if (closing.linger_seconds)
                (void)net_tcp_close_timeout(
                    closing.kern_conn_id,
                    (uint32_t)closing.linger_seconds * 100U);
            else
                net_tcp_abort(closing.kern_conn_id);
        } else {
            net_tcp_shutdown(closing.kern_conn_id);
        }
        wsock_wake_ready_pending_recvs();
        wsock_service_pending_recvs(win32_current_process_id());
        net_tcp_release(closing.kern_conn_id);
    }
    if (last_reference && closing.kern_listener_id >= 0)
        net_tcp_stop_listen(closing.kern_listener_id);
    if (last_reference && closing.kern_socket_id >= 0)
        kernel_sock_close(closing.kern_socket_id);

    ws->kern_conn_id = -1;
    ws->kern_listener_id = -1;
    ws->kern_socket_id = -1;
    ws->connected = 0;
    ws->connecting = 0;
    ws->listening = 0;
    k32_iocp_forget_file((HANDLE)(ULONG_PTR)s);

    wsa_last_error = 0;
    wsock_socket_lifecycle_release();

    serial_puts("[WSOCK] closesocket ");
    serial_puthex(BASE_SOCKET + idx, 4); serial_puts("\n");
    return 0;
}

/* ── select() ────────────────────────────────────────────── */

enum {
    WSOCK_SELECT_READ,
    WSOCK_SELECT_WRITE,
    WSOCK_SELECT_EXCEPT
};

typedef struct {
    LONG seconds;
    LONG microseconds;
} WSOCK_TIMEVAL;

typedef struct {
    SOCKET socket;
    DWORD generation;
} wsock_select_entry_t;

typedef struct {
    uint32_t count;
    wsock_select_entry_t entries[64];
} wsock_select_set_t;

_Static_assert(sizeof(WSOCK_TIMEVAL) == 8, "Winsock timeval layout");

static int wsock_capture_fdset(PCVOID set, wsock_select_set_t *captured)
{
    captured->count = 0;
    if (!set) return 0;

    uint32_t count = fdset_count(set);
    if (count > 64) return WSAEINVAL;

    for (uint32_t i = 0; i < count; i++) {
        SOCKET s = fdset_socket(set, i);
        int idx = sock_to_index(s);
        if (idx < 0) return WSAENOTSOCK;
        captured->entries[i].socket = s;
        captured->entries[i].generation = win32_sockets[idx].generation;
    }
    captured->count = count;
    return 0;
}

static int wsock_evaluate_fdset(const wsock_select_set_t *captured,
                                int kind, wsock_select_set_t *ready)
{
    ready->count = 0;
    for (uint32_t i = 0; i < captured->count; i++) {
        const wsock_select_entry_t *entry = &captured->entries[i];
        int idx = sock_to_index(entry->socket);
        if (idx < 0 || win32_sockets[idx].generation != entry->generation)
            return WSAENOTSOCK;

        win32_socket_t *socket = &win32_sockets[idx];
        LONG events = wsock_socket_ready_events(socket);
        BOOL is_ready = FALSE;
        if (kind == WSOCK_SELECT_READ) {
            is_ready = (events & (WSOCK_FD_READ | WSOCK_FD_ACCEPT |
                                  WSOCK_FD_CLOSE)) != 0;
        } else if (kind == WSOCK_SELECT_WRITE) {
            is_ready = !socket->shutdown_send &&
                       (events & WSOCK_FD_WRITE) != 0;
        } else {
            is_ready = socket->pending_error != 0 ||
                       (events & WSOCK_FD_OOB) != 0;
        }
        if (is_ready)
            ready->entries[ready->count++] = *entry;
    }
    return 0;
}

static void wsock_store_fdset(PVOID set, const wsock_select_set_t *ready)
{
    if (!set) return;
    for (uint32_t i = 0; i < ready->count; i++)
        fdset_store(set, i, ready->entries[i].socket);
    fdset_set_count(set, ready->count);
}

int WINAPI wsock_select(int nfds, PVOID readfds, PVOID writefds,
                       PVOID exceptfds, PVOID timeout)
{
    static uint32_t trace_count;
    wsock_select_set_t requested[3], ready[3];
    ULONGLONG timeout_ms = 0;
    ULONGLONG started = GetTickCount64();
    BOOL finite_timeout = timeout != NULL;
    (void)nfds;

    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return SOCKET_ERROR;
    }
    if (!readfds && !writefds && !exceptfds) {
        wsa_last_error = WSAEINVAL;
        return SOCKET_ERROR;
    }
    if (finite_timeout) {
        const WSOCK_TIMEVAL *value = (const WSOCK_TIMEVAL *)timeout;
        if (value->seconds < 0 || value->microseconds < 0 ||
            value->microseconds >= 1000000) {
            wsa_last_error = WSAEINVAL;
            return SOCKET_ERROR;
        }
        timeout_ms = (ULONGLONG)(ULONG)value->seconds * 1000ULL +
                     ((ULONGLONG)(ULONG)value->microseconds + 999ULL) /
                         1000ULL;
    }

    int error = wsock_capture_fdset(readfds, &requested[0]);
    if (!error) error = wsock_capture_fdset(writefds, &requested[1]);
    if (!error) error = wsock_capture_fdset(exceptfds, &requested[2]);
    if (error) {
        wsa_last_error = error;
        return SOCKET_ERROR;
    }

    int count;
    for (;;) {
        net_poll();
        wsock_service_pending_connects(win32_current_process_id());
        error = wsock_evaluate_fdset(&requested[0], WSOCK_SELECT_READ,
                                     &ready[0]);
        if (!error)
            error = wsock_evaluate_fdset(&requested[1], WSOCK_SELECT_WRITE,
                                         &ready[1]);
        if (!error)
            error = wsock_evaluate_fdset(&requested[2], WSOCK_SELECT_EXCEPT,
                                         &ready[2]);
        if (error) {
            wsa_last_error = error;
            return SOCKET_ERROR;
        }

        count = (int)(ready[0].count + ready[1].count + ready[2].count);
        if (count || (finite_timeout &&
                      GetTickCount64() - started >= timeout_ms))
            break;
        Sleep(1);
    }

    wsock_store_fdset(readfds, &ready[0]);
    wsock_store_fdset(writefds, &ready[1]);
    wsock_store_fdset(exceptfds, &ready[2]);

    if (trace_count++ < 32) {
        serial_puts("[WSOCK] select mode=");
        serial_puts(g_compat32_mode ? "32" : "64");
        serial_puts(" ready=");
        serial_puthex((uint32_t)count, 2);
        serial_puts("\n");
    }

    wsa_last_error = 0;
    return count;
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
static uint64_t hostent_addr_list64[2];
static uint64_t hostent_aliases64[1];
static char     hostent_name[256];

static uint8_t  hostent_buf[16];
static uint8_t  hostent_buf64[32];

static PVOID wsock_make_hostent(PCSTR name, const uint8_t ip[4])
{
    int name_length = ws_strlen(name);
    if (name_length > 255) name_length = 255;
    ws_memcpy(hostent_name, name, (SIZE_T)name_length);
    hostent_name[name_length] = 0;
    ws_memcpy(hostent_ip, ip, sizeof(hostent_ip));

    wsa_last_error = 0;
    if (g_compat32_mode) {
        hostent_addr_list[0] = (uint32_t)(uintptr_t)hostent_ip;
        hostent_addr_list[1] = 0;
        hostent_aliases[0] = 0;
        uint32_t *host = (uint32_t *)hostent_buf;
        host[0] = (uint32_t)(uintptr_t)hostent_name;
        host[1] = (uint32_t)(uintptr_t)hostent_aliases;
        hostent_buf[8] = AF_INET;
        hostent_buf[9] = 0;
        hostent_buf[10] = 4;
        hostent_buf[11] = 0;
        host[3] = (uint32_t)(uintptr_t)hostent_addr_list;
        return (PVOID)hostent_buf;
    }

    ws_memset(hostent_buf64, 0, sizeof(hostent_buf64));
    hostent_addr_list64[0] = (uint64_t)(uintptr_t)hostent_ip;
    hostent_addr_list64[1] = 0;
    hostent_aliases64[0] = 0;
    uint64_t *host = (uint64_t *)hostent_buf64;
    host[0] = (uint64_t)(uintptr_t)hostent_name;
    host[1] = (uint64_t)(uintptr_t)hostent_aliases64;
    hostent_buf64[16] = AF_INET;
    hostent_buf64[17] = 0;
    hostent_buf64[18] = 4;
    hostent_buf64[19] = 0;
    host[3] = (uint64_t)(uintptr_t)hostent_addr_list64;
    return (PVOID)hostent_buf64;
}

PVOID WINAPI wsock_gethostbyname(PCSTR name)
{
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return NULL;
    }
    if (!name) { wsa_last_error = WSAEFAULT; return NULL; }

    serial_puts("[WSOCK] gethostbyname(\"");
    serial_puts(name);
    serial_puts("\")\n");

    uint8_t ip[4];
    int ret = 0;
    if (!wsock_parse_ipv4_literal(name, ip) &&
        !wsock_resolve_local_ipv4(name, ip)) {
        ret = net_dns_resolve(name, ip);
    }
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

    return wsock_make_hostent(name, ip);
}

static PVOID WINAPI wsock_gethostbyaddr(PCVOID address, int length, int type)
{
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return NULL;
    }
    if (!address || length != 4) {
        wsa_last_error = WSAEFAULT;
        return NULL;
    }
    if (type != AF_INET) {
        wsa_last_error = WSAEAFNOSUPPORT;
        return NULL;
    }

    const uint8_t *ip = (const uint8_t *)address;
    if (ip[0] == 127)
        return wsock_make_hostent("localhost", ip);

    const uint8_t *local = net_get_ip_ptr();
    if ((local[0] || local[1] || local[2] || local[3]) &&
        ip[0] == local[0] && ip[1] == local[1] &&
        ip[2] == local[2] && ip[3] == local[3])
        return wsock_make_hostent("osito", ip);

    /* The kernel resolver currently supports A records but not PTR queries.
     * Do not manufacture a reverse-DNS name for a non-local address. */
    wsa_last_error = WSAHOST_NOT_FOUND;
    return NULL;
}

/* The Winsock protocol database APIs return storage owned by Winsock. Like
 * Windows, this buffer is reused by the next protocol lookup on the process. */
typedef struct {
    const char *name;
    int16_t number;
} WSOCK_PROTOCOL_ENTRY;

static const WSOCK_PROTOCOL_ENTRY wsock_protocol_db[] = {
    { "ip",        0 },
    { "icmp",      1 },
    { "igmp",      2 },
    { "tcp",       IPPROTO_TCP },
    { "udp",       IPPROTO_UDP },
    { "ipv6",      IPPROTO_IPV6 },
    { "ipv6-icmp", 58 },
    { NULL,          0 }
};

static uint32_t protoent_aliases32[1];
static uint32_t protoent_buf32[3];
static uint64_t protoent_aliases64[1];
static uint8_t protoent_buf64[24] __attribute__((aligned(8)));

static PVOID wsock_make_protoent(const WSOCK_PROTOCOL_ENTRY *entry)
{
    wsa_last_error = 0;
    if (g_compat32_mode) {
        protoent_aliases32[0] = 0;
        protoent_buf32[0] = (uint32_t)(uintptr_t)entry->name;
        protoent_buf32[1] = (uint32_t)(uintptr_t)protoent_aliases32;
        protoent_buf32[2] = (uint16_t)entry->number;
        return (PVOID)protoent_buf32;
    }

    ws_memset(protoent_buf64, 0, sizeof(protoent_buf64));
    protoent_aliases64[0] = 0;
    ((uint64_t *)protoent_buf64)[0] = (uint64_t)(uintptr_t)entry->name;
    ((uint64_t *)protoent_buf64)[1] =
        (uint64_t)(uintptr_t)protoent_aliases64;
    *(int16_t *)(protoent_buf64 + 16) = entry->number;
    return (PVOID)protoent_buf64;
}

static PVOID WINAPI wsock_getprotobyname(PCSTR name)
{
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return NULL;
    }
    if (!name) {
        wsa_last_error = WSAEFAULT;
        return NULL;
    }

    for (int i = 0; wsock_protocol_db[i].name; i++) {
        if (ws_stricmp(name, wsock_protocol_db[i].name) == 0)
            return wsock_make_protoent(&wsock_protocol_db[i]);
    }
    wsa_last_error = 11004; /* WSANO_DATA */
    return NULL;
}

static PVOID WINAPI wsock_getprotobynumber(int number)
{
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return NULL;
    }
    for (int i = 0; wsock_protocol_db[i].name; i++) {
        if (number == wsock_protocol_db[i].number)
            return wsock_make_protoent(&wsock_protocol_db[i]);
    }
    wsa_last_error = 11004; /* WSANO_DATA */
    return NULL;
}

static int WINAPI wsock_gethostname(PSTR name, int name_length)
{
    static const char hostname[] = "osito";
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return SOCKET_ERROR;
    }
    int required = (int)sizeof(hostname);
    if (!name || name_length < required) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    ws_memcpy(name, hostname, sizeof(hostname));
    wsa_last_error = 0;
    return 0;
}

/* ── inet_addr() ─────────────────────────────────────────── */

ULONG WINAPI wsock_inet_addr(PCSTR cp)
{
    if (!cp) return 0xFFFFFFFF; /* INADDR_NONE */

    uint32_t parts[4];
    int part_count = 0;
    const char *p = cp;

    for (;;) {
        uint32_t value = 0;
        uint32_t base = 10;
        int digits = 0;
        if (part_count == 4) return 0xFFFFFFFF;

        if (*p == '0') {
            base = 8;
            p++;
            digits = 1;
            if (*p == 'x' || *p == 'X') {
                base = 16;
                p++;
                digits = 0;
            }
        }

        for (;;) {
            uint32_t digit;
            if (*p >= '0' && *p <= '9')
                digit = (uint32_t)(*p - '0');
            else if (*p >= 'a' && *p <= 'f')
                digit = (uint32_t)(*p - 'a') + 10U;
            else if (*p >= 'A' && *p <= 'F')
                digit = (uint32_t)(*p - 'A') + 10U;
            else
                break;
            if (digit >= base ||
                value > (0xFFFFFFFFU - digit) / base)
                return 0xFFFFFFFF;
            value = value * base + digit;
            p++;
            digits++;
        }
        if (!digits) return 0xFFFFFFFF;
        parts[part_count++] = value;
        if (*p != '.') break;
        p++;
    }

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p) return 0xFFFFFFFF;

    uint32_t address;
    switch (part_count) {
    case 1:
        address = parts[0];
        break;
    case 2:
        if (parts[0] > 0xFFU || parts[1] > 0xFFFFFFU)
            return 0xFFFFFFFF;
        address = (parts[0] << 24) | parts[1];
        break;
    case 3:
        if (parts[0] > 0xFFU || parts[1] > 0xFFU ||
            parts[2] > 0xFFFFU)
            return 0xFFFFFFFF;
        address = (parts[0] << 24) | (parts[1] << 16) | parts[2];
        break;
    case 4:
        if (parts[0] > 0xFFU || parts[1] > 0xFFU ||
            parts[2] > 0xFFU || parts[3] > 0xFFU)
            return 0xFFFFFFFF;
        address = (parts[0] << 24) | (parts[1] << 16) |
                  (parts[2] << 8) | parts[3];
        break;
    default:
        return 0xFFFFFFFF;
    }

    return ((address >> 24) & 0x000000FFU) |
           ((address >> 8)  & 0x0000FF00U) |
           ((address << 8)  & 0x00FF0000U) |
           ((address << 24) & 0xFF000000U);
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

static int wsock_append_decimal(char *buffer, int position, uint32_t value)
{
    char reversed[10];
    int count = 0;
    do {
        reversed[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value);
    while (count) buffer[position++] = reversed[--count];
    return position;
}

static int wsock_format_ipv4(const uint8_t address[4], char output[16])
{
    int position = 0;
    for (int i = 0; i < 4; i++) {
        if (i) output[position++] = '.';
        position = wsock_append_decimal(output, position, address[i]);
    }
    output[position] = 0;
    return position;
}

static int wsock_format_ipv6(const uint8_t address[16], char output[64])
{
    static const char hex[] = "0123456789abcdef";
    BOOL mapped = TRUE;
    for (int i = 0; i < 10; i++)
        if (address[i]) mapped = FALSE;
    mapped = mapped && address[10] == 0xFF && address[11] == 0xFF;
    if (mapped) {
        output[0] = ':'; output[1] = ':';
        output[2] = 'f'; output[3] = 'f'; output[4] = 'f'; output[5] = 'f';
        output[6] = ':';
        int length = wsock_format_ipv4(address + 12, output + 7);
        return length + 7;
    }

    uint16_t words[8];
    int best_start = -1, best_length = 0;
    for (int i = 0; i < 8; i++)
        words[i] = (uint16_t)((uint16_t)address[i * 2] << 8) |
                   address[i * 2 + 1];
    for (int i = 0; i < 8;) {
        if (words[i]) { i++; continue; }
        int start = i;
        while (i < 8 && !words[i]) i++;
        int length = i - start;
        if (length >= 2 && length > best_length) {
            best_start = start;
            best_length = length;
        }
    }

    int position = 0;
    for (int i = 0; i < 8;) {
        if (i == best_start) {
            output[position++] = ':';
            output[position++] = ':';
            i += best_length;
            continue;
        }
        if (position && output[position - 1] != ':')
            output[position++] = ':';
        uint16_t value = words[i++];
        int shift = 12;
        while (shift && ((value >> shift) & 0xFU) == 0) shift -= 4;
        for (; shift >= 0; shift -= 4)
            output[position++] = hex[(value >> shift) & 0xFU];
    }
    output[position] = 0;
    return position;
}

static PCSTR WINAPI wsock_inet_ntop(int family, PCVOID source,
                                     PSTR destination, SIZE_T size)
{
    if (!source || !destination) {
        wsa_last_error = WSAEFAULT;
        return NULL;
    }
    char formatted[64];
    int length;
    if (family == AF_INET)
        length = wsock_format_ipv4((const uint8_t *)source, formatted);
    else if (family == AF_INET6)
        length = wsock_format_ipv6((const uint8_t *)source, formatted);
    else {
        wsa_last_error = WSAEAFNOSUPPORT;
        return NULL;
    }
    if (size <= (SIZE_T)length) {
        wsa_last_error = 87; /* ERROR_INVALID_PARAMETER */
        return NULL;
    }
    ws_memcpy(destination, formatted, (SIZE_T)length + 1U);
    wsa_last_error = 0;
    return destination;
}

static BOOL wsock_ipv4_equal(const uint8_t left[4], const uint8_t right[4])
{
    for (int i = 0; i < 4; i++)
        if (left[i] != right[i]) return FALSE;
    return TRUE;
}

static int wsock_copy_ansi_result(PSTR destination, DWORD capacity,
                                  const char *source)
{
    DWORD required = (DWORD)ws_strlen(source) + 1U;
    if (!destination || capacity < required) return WSAEFAULT;
    ws_memcpy(destination, source, required);
    return 0;
}

static const char *wsock_service_name(uint16_t port, int flags)
{
    switch (port) {
    case 22:  return "ssh";
    case 53:  return "domain";
    case 67:  return (flags & NI_DGRAM) ? "bootps" : NULL;
    case 68:  return (flags & NI_DGRAM) ? "bootpc" : NULL;
    case 80:  return "http";
    case 123: return (flags & NI_DGRAM) ? "ntp" : NULL;
    case 443: return "https";
    default:  return NULL;
    }
}

static int WINAPI wsock_getnameinfo(PCVOID socket_address, int address_length,
                                     PSTR host, DWORD host_length,
                                     PSTR service, DWORD service_length,
                                     int flags)
{
    const int valid_flags = NI_NOFQDN | NI_NUMERICHOST | NI_NAMEREQD |
                            NI_NUMERICSERV | NI_DGRAM;
    if (!wsa_initialized) {
        wsa_last_error = WSANOTINITIALISED;
        return WSANOTINITIALISED;
    }
    if (!socket_address) {
        wsa_last_error = WSAEFAULT;
        return WSAEFAULT;
    }
    if ((!host && !service) || (host && !host_length) ||
        (service && !service_length) || (flags & ~valid_flags) ||
        ((flags & NI_NUMERICHOST) && (flags & NI_NAMEREQD))) {
        int error = !host && !service ? WSAHOST_NOT_FOUND : WSAEINVAL;
        wsa_last_error = error;
        return error;
    }

    const uint8_t *sa = (const uint8_t *)socket_address;
    uint16_t family = (uint16_t)sa[0] | (uint16_t)((uint16_t)sa[1] << 8);
    const uint8_t *address;
    int inet_family;
    if (family == AF_INET) {
        if (address_length < 16) {
            wsa_last_error = WSAEFAULT;
            return WSAEFAULT;
        }
        address = sa + 4;
        inet_family = AF_INET;
    } else if (family == AF_INET6) {
        if (address_length < 28) {
            wsa_last_error = WSAEFAULT;
            return WSAEFAULT;
        }
        address = sa + 8;
        inet_family = AF_INET6;
    } else {
        wsa_last_error = WSAEAFNOSUPPORT;
        return WSAEAFNOSUPPORT;
    }

    char numeric_host[64];
    if (!wsock_inet_ntop(inet_family, address, numeric_host,
                         sizeof(numeric_host)))
        return wsa_last_error;

    if (host) {
        const char *host_result = numeric_host;
        BOOL local_name = FALSE;
        if (!(flags & NI_NUMERICHOST)) {
            if (family == AF_INET) {
                static const uint8_t loopback[4] = { 127, 0, 0, 1 };
                if (wsock_ipv4_equal(address, loopback)) {
                    host_result = "localhost";
                    local_name = TRUE;
                } else if (wsock_ipv4_equal(address, net_get_ip_ptr())) {
                    host_result = "osito";
                    local_name = TRUE;
                }
            } else {
                BOOL loopback = TRUE;
                for (int i = 0; i < 15; i++)
                    if (address[i]) loopback = FALSE;
                if (loopback && address[15] == 1) {
                    host_result = "localhost";
                    local_name = TRUE;
                }
            }
            if (!local_name && (flags & NI_NAMEREQD)) {
                wsa_last_error = WSAHOST_NOT_FOUND;
                return WSAHOST_NOT_FOUND;
            }
        }
        int error = wsock_copy_ansi_result(host, host_length, host_result);
        if (error) {
            wsa_last_error = error;
            return error;
        }
    }

    if (service) {
        uint16_t port = (uint16_t)((uint16_t)sa[2] << 8) | sa[3];
        const char *service_result = NULL;
        char numeric_service[6];
        if (!(flags & NI_NUMERICSERV))
            service_result = wsock_service_name(port, flags);
        if (!service_result) {
            int position = wsock_append_decimal(numeric_service, 0, port);
            numeric_service[position] = 0;
            service_result = numeric_service;
        }
        int error = wsock_copy_ansi_result(service, service_length,
                                           service_result);
        if (error) {
            wsa_last_error = error;
            return error;
        }
    }

    wsa_last_error = 0;
    return 0;
}

static BOOL wsock_port_conflicts(const win32_socket_t *socket, uint16_t port)
{
    BOOL conflict = FALSE;
    wsock_process_states_acquire();
    for (int i = 0; i < MAX_WSOCK_PROCESS_STATES && !conflict; i++) {
        wsock_process_state_t *state = &g_wsock_process_states[i];
        if (!state->used) continue;
        for (int j = 0; j < MAX_WIN32_SOCKETS; j++) {
            win32_socket_t *candidate = &state->sockets[j];
            if (!candidate->in_use || candidate == socket ||
                wsock_same_backend(candidate, socket) ||
                candidate->type != socket->type ||
                candidate->local_port != port)
                continue;
            BOOL both_reuse =
                (candidate->option_flags & WSOCK_OPT_REUSEADDR) &&
                (socket->option_flags & WSOCK_OPT_REUSEADDR);
            BOOL exclusive =
                (candidate->option_flags & WSOCK_OPT_EXCLUSIVEADDR) ||
                (socket->option_flags & WSOCK_OPT_EXCLUSIVEADDR);
            if (exclusive || !both_reuse) {
                conflict = TRUE;
                break;
            }
        }
    }
    wsock_process_states_release();
    return conflict;
}

static int wsock_allocate_ephemeral(win32_socket_t *socket, uint16_t *port)
{
    for (DWORD attempt = 0; attempt < WSOCK_EPHEMERAL_COUNT; attempt++) {
        uint16_t candidate = alloc_ephemeral_port(socket);
        if (!wsock_port_conflicts(socket, candidate)) {
            *port = candidate;
            return 0;
        }
    }
    return -1;
}

static int wsock_bind_implicit_locked(win32_socket_t *socket)
{
    if (socket->local_port) return 0;
    uint16_t port;
    if (wsock_allocate_ephemeral(socket, &port) < 0)
        return -1;
    socket->local_port = port;
    if (socket->type == SOCK_DGRAM) {
        uint8_t endpoint[16];
        uint8_t any[4] = { 0, 0, 0, 0 };
        wsock_store_sockaddr(endpoint, any, port);
        if (socket->kern_socket_id < 0 ||
            kernel_sock_bind(socket->kern_socket_id, endpoint) < 0) {
            socket->local_port = 0;
            return -1;
        }
    }
    return 0;
}

static int wsock_bind_implicit(win32_socket_t *socket)
{
    wsock_socket_lifecycle_acquire();
    int result = wsock_bind_implicit_locked(socket);
    wsock_socket_lifecycle_release();
    return result;
}

int WINAPI wsock_bind(SOCKET s, PVOID name, int namelen)
{
    wsock_process_state_t *state = wsock_state_current();
    wsock_socket_lifecycle_acquire();
    int idx = wsock_raw_index(s);
    if (idx < 0 || !state->sockets[idx].in_use) {
        wsa_last_error = WSAENOTSOCK;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }

    win32_socket_t *socket = &state->sockets[idx];
    if (socket->local_port) {
        wsa_last_error = WSAEINVAL;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }
    uint8_t ip[4];
    uint16_t port;
    int parse_error = wsock_parse_ipv4_endpoint(
        (const uint8_t *)name, namelen, ip, &port);
    if (parse_error) {
        wsa_last_error = parse_error;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }
    if (!port && wsock_allocate_ephemeral(socket, &port) < 0) {
        wsa_last_error = WSAEADDRINUSE;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }
    if (port && wsock_port_conflicts(socket, port)) {
        wsa_last_error = WSAEADDRINUSE;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }

    if (socket->type == SOCK_DGRAM) {
        uint8_t endpoint[16];
        wsock_store_sockaddr(endpoint, ip, port);
        if (socket->kern_socket_id < 0 ||
            kernel_sock_bind(socket->kern_socket_id, endpoint) < 0) {
            wsa_last_error = WSAEADDRINUSE;
            wsock_socket_lifecycle_release();
            return SOCKET_ERROR;
        }
    }
    socket->local_port = port;
    ws_memcpy(socket->local_ip, ip, 4);
    wsock_propagate_shared_state(socket);
    wsa_last_error = 0;
    wsock_socket_lifecycle_release();

    serial_puts("[WSOCK] bind port=");
    serial_puthex(port, 4);
    serial_puts("\n");
    return 0;
}

int WINAPI wsock_listen(SOCKET s, int backlog)
{
    wsock_process_state_t *state = wsock_state_current();
    wsock_socket_lifecycle_acquire();
    int idx = wsock_raw_index(s);
    if (idx < 0 || !state->sockets[idx].in_use) {
        wsa_last_error = WSAENOTSOCK;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }
    win32_socket_t *ws = &state->sockets[idx];
    if (ws->type != SOCK_STREAM) {
        wsa_last_error = WSAEOPNOTSUPP;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }
    if (wsock_bind_implicit_locked(ws) < 0) {
        wsa_last_error = WSAEADDRINUSE;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }
    if (ws->kern_listener_id < 0)
        ws->kern_listener_id = net_tcp_listen(ws->local_port);
    if (ws->kern_listener_id < 0) {
        wsa_last_error = WSAEADDRINUSE;
        wsock_socket_lifecycle_release();
        return SOCKET_ERROR;
    }
    net_tcp_set_listen_backlog(ws->kern_listener_id, backlog);
    ws->listening = 1;
    uint16_t port = ws->local_port;
    int listener_id = ws->kern_listener_id;
    wsock_propagate_shared_state(ws);
    wsa_last_error = 0;
    wsock_socket_lifecycle_release();

    serial_puts("[WSOCK] listen port=");
    serial_puthex(port, 4);
    serial_puts(" listener=");
    serial_puthex((uint32_t)listener_id, 2);
    serial_puts("\n");
    return 0;
}

SOCKET WINAPI wsock_accept(SOCKET s, PVOID addr, int *addrlen)
{
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return INVALID_SOCKET; }
    win32_socket_t *listener = &win32_sockets[idx];
    if (!listener->listening || listener->kern_listener_id < 0) {
        wsa_last_error = WSAEINVAL;
        return INVALID_SOCKET;
    }

    uint32_t timeout = listener->nonblocking ? 0 : WSOCK_INFINITE;
    int conn = net_tcp_accept(listener->kern_listener_id, timeout);
    if (conn < 0) {
        wsa_last_error = WSAEWOULDBLOCK;
        return INVALID_SOCKET;
    }

    SOCKET accepted = wsock_socket(listener->af, listener->type,
                                   listener->protocol);
    int accepted_idx = sock_to_index(accepted);
    if (accepted_idx < 0) {
        net_tcp_shutdown(conn);
        wsa_last_error = WSAENOBUFS;
        return INVALID_SOCKET;
    }
    win32_socket_t *peer = &win32_sockets[accepted_idx];
    peer->kern_conn_id = conn;
    peer->connected = 1;
    peer->connected_at_ms = GetTickCount();
    peer->pending_error = 0;
    wsock_inherit_listener_options(peer, listener);
    peer->local_port = listener->local_port;
    ws_memcpy(peer->local_ip, listener->local_ip, 4);
    if (net_tcp_get_peer(conn, peer->remote_ip, &peer->remote_port) < 0) {
        peer->remote_ip[0] = 127;
        peer->remote_ip[1] = 0;
        peer->remote_ip[2] = 0;
        peer->remote_ip[3] = 1;
        peer->remote_port = 0;
    }
    wsock_apply_keepalive(peer);

    if (addr && addrlen && *addrlen >= 16) {
        uint8_t *sa = (uint8_t *)addr;
        ws_memset(sa, 0, 16);
        sa[0] = AF_INET;
        sa[2] = (uint8_t)(peer->remote_port >> 8);
        sa[3] = (uint8_t)peer->remote_port;
        ws_memcpy(sa + 4, peer->remote_ip, 4);
        *addrlen = 16;
    }
    serial_puts("[WSOCK] accept socket=");
    serial_puthex((uint32_t)accepted, 4);
    serial_puts(" conn=");
    serial_puthex((uint32_t)conn, 2);
    serial_puts("\n");
    wsa_last_error = 0;
    return accepted;
}

int WINAPI wsock_getpeername(SOCKET s, PVOID name, int *namelen)
{
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }

    win32_socket_t *ws = &win32_sockets[idx];
    if (!ws->connected) { wsa_last_error = WSAENOTCONN; return SOCKET_ERROR; }

    if (!name || !namelen) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    int required = wsock_sockaddr_length(ws);
    if (*namelen < required) {
        *namelen = required;
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    wsock_store_peer_sockaddr(ws, (uint8_t *)name, ws->remote_ip,
                              ws->remote_port);
    *namelen = required;
    wsa_last_error = 0;
    return 0;
}

int WINAPI wsock_getsockname(SOCKET s, PVOID name, int *namelen)
{
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }

    win32_socket_t *ws = &win32_sockets[idx];
    if (!name || !namelen) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    int required = wsock_sockaddr_length(ws);
    if (*namelen < required) {
        *namelen = required;
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    wsock_store_peer_sockaddr(ws, (uint8_t *)name, ws->local_ip,
                              ws->local_port);
    *namelen = required;
    wsa_last_error = 0;
    return 0;
}

typedef struct {
    USHORT onoff;
    USHORT linger_seconds;
} wsock_linger_t;

static int wsock_copy_option_out(PSTR optval, int *optlen,
                                 const void *value, int value_length)
{
    if (!optval || !optlen) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    if (*optlen < value_length) {
        *optlen = value_length;
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    ws_memcpy(optval, value, (SIZE_T)value_length);
    *optlen = value_length;
    wsa_last_error = 0;
    return 0;
}

static int wsock_copy_int_option(PSTR optval, int *optlen, int value)
{
    return wsock_copy_option_out(optval, optlen, &value, sizeof(value));
}

static int wsock_read_int_option(PCSTR optval, int optlen, int *value)
{
    if (!optval || !value || optlen < (int)sizeof(*value)) {
        wsa_last_error = WSAEFAULT;
        return SOCKET_ERROR;
    }
    ws_memcpy(value, optval, sizeof(*value));
    return 0;
}

static int wsock_setsockopt_error(SOCKET socket, int level, int optname,
                                  PCSTR optval, int optlen, int error)
{
    static volatile uint32_t failure_count;
    uint32_t trace_index = __sync_fetch_and_add(&failure_count, 1);

    wsa_last_error = error;
    if (trace_index < 256) {
        int value = 0;
        if (optval && optlen >= (int)sizeof(value))
            ws_memcpy(&value, optval, sizeof(value));
        serial_puts("[WSOCK-OPT-FAIL] pid=");
        serial_putdec(wsock_state_current()->pid);
        serial_puts(" socket=");
        serial_puthex((uint32_t)socket, 4);
        serial_puts(" level=");
        serial_putdec((uint64_t)(uint32_t)level);
        serial_puts(" option=");
        serial_puthex((uint32_t)optname, 8);
        serial_puts(" len=");
        serial_putdec((uint64_t)(uint32_t)optlen);
        serial_puts(" value=");
        serial_puthex((uint32_t)value, 8);
        serial_puts(" error=");
        serial_putdec((uint64_t)(uint32_t)error);
        serial_puts("\n");
    }
    return SOCKET_ERROR;
}

static int wsock_option_enabled(const win32_socket_t *ws, DWORD flag)
{
    return (ws->option_flags & flag) != 0;
}

static int wsock_finish_socket_update(win32_socket_t *ws)
{
    wsock_propagate_shared_state(ws);
    wsa_last_error = 0;
    return 0;
}

int WINAPI wsock_getsockopt(SOCKET s, int level, int optname, PSTR optval, int *optlen)
{
    int idx = sock_to_index(s);
    if (idx < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }

    win32_socket_t *ws = &win32_sockets[idx];
    if (level == IPPROTO_TCP) {
        if (optname != TCP_NODELAY || ws->type != SOCK_STREAM) {
            wsa_last_error = WSAENOPROTOOPT;
            return SOCKET_ERROR;
        }
        return wsock_copy_int_option(optval, optlen, ws->no_delay != 0);
    }
    if (level == IPPROTO_IPV6) {
        if (optname != IPV6_V6ONLY || ws->af != AF_INET6) {
            wsa_last_error = WSAENOPROTOOPT;
            return SOCKET_ERROR;
        }
        return wsock_copy_int_option(
            optval, optlen,
            wsock_option_enabled(ws, WSOCK_OPT_IPV6ONLY));
    }
    if (level != SOL_SOCKET) {
        wsa_last_error = WSAENOPROTOOPT;
        return SOCKET_ERROR;
    }

    int value;
    switch (optname) {
    case SO_DEBUG:
        value = wsock_option_enabled(ws, WSOCK_OPT_DEBUG);
        break;
    case SO_ACCEPTCONN:
        value = ws->listening != 0;
        break;
    case SO_REUSEADDR:
        value = wsock_option_enabled(ws, WSOCK_OPT_REUSEADDR);
        break;
    case SO_KEEPALIVE:
        value = wsock_option_enabled(ws, WSOCK_OPT_KEEPALIVE);
        break;
    case SO_DONTROUTE:
        value = wsock_option_enabled(ws, WSOCK_OPT_DONTROUTE);
        break;
    case SO_BROADCAST:
        value = wsock_option_enabled(ws, WSOCK_OPT_BROADCAST);
        break;
    case SO_OOBINLINE:
        value = wsock_option_enabled(ws, WSOCK_OPT_OOBINLINE);
        break;
    case SO_EXCLUSIVEADDRUSE:
        value = wsock_option_enabled(ws, WSOCK_OPT_EXCLUSIVEADDR);
        break;
    case SO_DONTLINGER:
        value = !ws->linger_enabled;
        break;
    case SO_SNDBUF:
        value = ws->send_buffer_size;
        break;
    case SO_RCVBUF:
        value = ws->recv_buffer_size;
        break;
    case SO_SNDLOWAT:
        value = ws->send_low_water;
        break;
    case SO_RCVLOWAT:
        value = ws->recv_low_water;
        break;
    case SO_SNDTIMEO:
        value = ws->send_timeout_ms;
        break;
    case SO_RCVTIMEO:
        value = ws->recv_timeout_ms;
        break;
    case SO_ERROR:
        (void)wsock_socket_ready_events(ws);
        value = ws->pending_error;
        ws->pending_error = 0;
        break;
    case SO_TYPE:
        value = ws->type;
        break;
    case SO_MAX_MSG_SIZE:
        value = ws->type == SOCK_DGRAM ? 65507 : 0;
        break;
    case SO_CONNECT_TIME:
        value = ws->connected
              ? (int)((GetTickCount() - ws->connected_at_ms) / 1000U)
              : -1;
        break;
    case SO_RANDOMIZE_PORT:
        value = wsock_option_enabled(ws, WSOCK_OPT_RANDOMIZE_PORT);
        break;
    case SO_LINGER: {
        wsock_linger_t linger;
        linger.onoff = ws->linger_enabled;
        linger.linger_seconds = ws->linger_seconds;
        return wsock_copy_option_out(optval, optlen, &linger,
                                     sizeof(linger));
    }
    default:
        wsa_last_error = WSAENOPROTOOPT;
        return SOCKET_ERROR;
    }
    return wsock_copy_int_option(optval, optlen, value);
}

int WINAPI wsock_setsockopt(SOCKET s, int level, int optname,
                           PCSTR optval, int optlen)
{
    int idx = sock_to_index(s);
    if (idx < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }

    win32_socket_t *ws = &win32_sockets[idx];
    int value;
    if (level == IPPROTO_TCP) {
        if (optname != TCP_NODELAY || ws->type != SOCK_STREAM) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAENOPROTOOPT);
        }
        if (wsock_read_int_option(optval, optlen, &value) == SOCKET_ERROR)
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, wsa_last_error);
        if (!value) {
            /* The kernel currently always transmits without Nagle. */
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEOPNOTSUPP);
        }
        ws->no_delay = 1;
        return wsock_finish_socket_update(ws);
    }
    if (level == IPPROTO_IPV6) {
        if (optname != IPV6_V6ONLY || ws->af != AF_INET6) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAENOPROTOOPT);
        }
        if (wsock_read_int_option(optval, optlen, &value) == SOCKET_ERROR)
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, wsa_last_error);
        if (value)
            ws->option_flags |= WSOCK_OPT_IPV6ONLY;
        else
            ws->option_flags &= ~WSOCK_OPT_IPV6ONLY;
        return wsock_finish_socket_update(ws);
    }
    if (level != SOL_SOCKET) {
        return wsock_setsockopt_error(s, level, optname, optval, optlen,
                                      WSAENOPROTOOPT);
    }

    if (optname == SO_UPDATE_CONNECT_CONTEXT) {
        if (optval || optlen != 0) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEINVAL);
        }
        if (!ws->connected) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAENOTCONN);
        }
        ws->connect_context_updated = 1;
        return wsock_finish_socket_update(ws);
    }
    if (optname == SO_UPDATE_ACCEPT_CONTEXT) {
        int socket_size = g_compat32_mode ? 4 : (int)sizeof(SOCKET);
        if (!optval || optlen < socket_size || !ws->connected) {
            int error = !optval || optlen < socket_size
                      ? WSAEFAULT : WSAENOTCONN;
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, error);
        }
        SOCKET listener_socket = g_compat32_mode
                               ? (SOCKET)*(const uint32_t *)optval
                               : *(const SOCKET *)optval;
        int listener_idx = sock_to_index(listener_socket);
        if (listener_idx < 0 || !win32_sockets[listener_idx].listening) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEINVAL);
        }
        win32_socket_t *listener = &win32_sockets[listener_idx];
        ws->local_port = listener->local_port;
        ws_memcpy(ws->local_ip, listener->local_ip, sizeof(ws->local_ip));
        ws->accept_context_updated = 1;
        return wsock_finish_socket_update(ws);
    }
    if (optname == SO_LINGER) {
        if (!optval || optlen < (int)sizeof(wsock_linger_t)) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEFAULT);
        }
        wsock_linger_t linger;
        ws_memcpy(&linger, optval, sizeof(linger));
        ws->linger_enabled = linger.onoff != 0;
        ws->linger_seconds = linger.linger_seconds;
        return wsock_finish_socket_update(ws);
    }
    if (optname == SO_RANDOMIZE_PORT) {
        if (wsock_read_int_option(optval, optlen, &value) == SOCKET_ERROR)
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, wsa_last_error);
        if (ws->local_port) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEINVAL);
        }
        if (value)
            ws->option_flags |= WSOCK_OPT_RANDOMIZE_PORT;
        else
            ws->option_flags &= ~WSOCK_OPT_RANDOMIZE_PORT;
        return wsock_finish_socket_update(ws);
    }
    if (wsock_read_int_option(optval, optlen, &value) == SOCKET_ERROR)
        return wsock_setsockopt_error(s, level, optname, optval, optlen,
                                      wsa_last_error);

    DWORD flag = 0;
    switch (optname) {
    case SO_DEBUG:
        if (value) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEOPNOTSUPP);
        }
        flag = WSOCK_OPT_DEBUG;
        break;
    case SO_REUSEADDR:
        if (value && wsock_option_enabled(ws, WSOCK_OPT_EXCLUSIVEADDR)) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEINVAL);
        }
        flag = WSOCK_OPT_REUSEADDR;
        break;
    case SO_KEEPALIVE:
        if (ws->type != SOCK_STREAM) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAENOPROTOOPT);
        }
        flag = WSOCK_OPT_KEEPALIVE;
        break;
    case SO_DONTROUTE:
        if (value) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEOPNOTSUPP);
        }
        flag = WSOCK_OPT_DONTROUTE;
        break;
    case SO_BROADCAST:
        if (ws->type != SOCK_DGRAM) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAENOPROTOOPT);
        }
        flag = WSOCK_OPT_BROADCAST;
        break;
    case SO_OOBINLINE:
        if (value) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEOPNOTSUPP);
        }
        flag = WSOCK_OPT_OOBINLINE;
        break;
    case SO_EXCLUSIVEADDRUSE:
        if (value && wsock_option_enabled(ws, WSOCK_OPT_REUSEADDR)) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEINVAL);
        }
        flag = WSOCK_OPT_EXCLUSIVEADDR;
        break;
    case SO_DONTLINGER:
        ws->linger_enabled = value ? 0 : 1;
        return wsock_finish_socket_update(ws);
    case SO_SNDBUF:
        if (value < 0) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEINVAL);
        }
        /* Winsock permits zero to request unbuffered sends.  Osito's
         * WSASend path already completes only after handing the payload to
         * the transport, so retaining zero provides that contract. */
        ws->send_buffer_size = value > WSOCK_TCP_SEND_CAPACITY
                             ? WSOCK_TCP_SEND_CAPACITY : value;
        return wsock_finish_socket_update(ws);
    case SO_RCVBUF:
        if (value <= 0) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEINVAL);
        }
        value = value > WSOCK_TCP_RECV_CAPACITY
              ? WSOCK_TCP_RECV_CAPACITY : value;
        if (ws->type == SOCK_DGRAM &&
            (ws->kern_socket_id < 0 ||
             kernel_sock_set_recv_buffer(ws->kern_socket_id,
                                         (uint32_t)value) < 0)) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAENOBUFS);
        }
        ws->recv_buffer_size = value;
        return wsock_finish_socket_update(ws);
    case SO_SNDLOWAT:
        if (value != 1) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEOPNOTSUPP);
        }
        ws->send_low_water = 1;
        return wsock_finish_socket_update(ws);
    case SO_RCVLOWAT:
        if (value <= 0 || value > ws->recv_buffer_size) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEINVAL);
        }
        ws->recv_low_water = value;
        return wsock_finish_socket_update(ws);
    case SO_SNDTIMEO:
        if (value < 0) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEINVAL);
        }
        ws->send_timeout_ms = value;
        return wsock_finish_socket_update(ws);
    case SO_RCVTIMEO:
        if (value < 0) {
            return wsock_setsockopt_error(s, level, optname, optval,
                                          optlen, WSAEINVAL);
        }
        ws->recv_timeout_ms = value;
        return wsock_finish_socket_update(ws);
    default:
        return wsock_setsockopt_error(s, level, optname, optval, optlen,
                                      WSAENOPROTOOPT);
    }

    DWORD old_flags = ws->option_flags;
    if (value)
        ws->option_flags |= flag;
    else
        ws->option_flags &= ~flag;
    if (flag == WSOCK_OPT_KEEPALIVE && wsock_apply_keepalive(ws) < 0) {
        ws->option_flags = old_flags;
        return wsock_setsockopt_error(s, level, optname, optval, optlen,
                                      WSAENETDOWN);
    }
    return wsock_finish_socket_update(ws);
}

int WINAPI wsock_shutdown(SOCKET s, int how)
{
    int idx = sock_to_index(s);
    if (idx < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }
    if (how < 0 || how > 2) {
        wsa_last_error = WSAEINVAL;
        return SOCKET_ERROR;
    }

    win32_socket_t *ws = &win32_sockets[idx];
    if (!ws->connected) {
        wsa_last_error = WSAENOTCONN;
        return SOCKET_ERROR;
    }
    if (how == 0 || how == 2)
        ws->shutdown_receive = 1;
    if (how == 1 || how == 2) {
        ws->shutdown_send = 1;
        if (ws->kern_conn_id >= 0)
            net_tcp_shutdown(ws->kern_conn_id);
    }
    wsock_propagate_shared_state(ws);
    wsa_last_error = 0;
    return 0;
}

static int WINAPI wsock_WSASendDisconnect(SOCKET s, PCVOID disconnect_data)
{
    if (disconnect_data) {
        uint32_t length = wsabuf_len_for_mode(disconnect_data, 0,
                                              g_compat32_mode);
        const char *data = (const char *)wsabuf_data_for_mode(
            disconnect_data, 0, g_compat32_mode);
        if (length > 0x7fffffffU) {
            wsa_last_error = WSAEMSGSIZE;
            return SOCKET_ERROR;
        }
        if (length && !data) {
            wsa_last_error = WSAEFAULT;
            return SOCKET_ERROR;
        }

        uint32_t sent_total = 0;
        while (sent_total < length) {
            int sent = wsock_send(s, data + sent_total,
                                  (int)(length - sent_total), 0);
            if (sent == SOCKET_ERROR)
                return SOCKET_ERROR;
            if (sent == 0) {
                wsa_last_error = WSAEWOULDBLOCK;
                return SOCKET_ERROR;
            }
            sent_total += (uint32_t)sent;
        }
    }

    return wsock_shutdown(s, 1); /* SD_SEND */
}

static int WINAPI wsock_WSARecvDisconnect(SOCKET s, PVOID disconnect_data)
{
    if (disconnect_data) {
        uint32_t capacity = wsabuf_len_for_mode(disconnect_data, 0,
                                                g_compat32_mode);
        PVOID data = wsabuf_data_for_mode(disconnect_data, 0,
                                          g_compat32_mode);
        if (capacity && !data) {
            wsa_last_error = WSAEFAULT;
            return SOCKET_ERROR;
        }
    }

    int result = wsock_shutdown(s, 0); /* SD_RECEIVE */
    if (result == 0 && disconnect_data)
        wsabuf_set_len_for_mode(disconnect_data, 0, g_compat32_mode, 0);
    return result;
}

int WINAPI wsock_recvfrom(SOCKET s, PSTR buf, int len, int flags,
                          PVOID from, int *fromlen)
{
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    win32_socket_t *ws = &win32_sockets[idx];
    if (ws->type != SOCK_DGRAM || ws->kern_socket_id < 0) {
        if (ws->type == SOCK_STREAM)
            return wsock_recv(s, buf, len, flags);
        wsa_last_error = WSAEOPNOTSUPP;
        return SOCKET_ERROR;
    }
    if (ws->shutdown_receive) {
        wsa_last_error = WSAESHUTDOWN;
        return SOCKET_ERROR;
    }
    if (len < 0) { wsa_last_error = WSAEINVAL; return SOCKET_ERROR; }
    if (len && !buf) { wsa_last_error = WSAEFAULT; return SOCKET_ERROR; }
    if (flags & ~(WSOCK_MSG_PEEK)) {
        wsa_last_error = WSAEOPNOTSUPP;
        return SOCKET_ERROR;
    }
    if (from) {
        int required = wsock_sockaddr_length(ws);
        if (!fromlen) {
            wsa_last_error = WSAEFAULT;
            return SOCKET_ERROR;
        }
        if (*fromlen < required) {
            *fromlen = required;
            wsa_last_error = WSAEFAULT;
            return SOCKET_ERROR;
        }
    }
    if (wsock_bind_implicit(ws) < 0) {
        wsa_last_error = WSAENOBUFS;
        return SOCKET_ERROR;
    }

    ULONGLONG deadline = ws->recv_timeout_ms
                       ? GetTickCount64() + (DWORD)ws->recv_timeout_ms : 0;
    for (;;) {
        int pending = kernel_sock_has_pending_data(ws->kern_socket_id);
        if (pending > 0) break;
        if (pending < 0) {
            wsa_last_error = WSAENOTSOCK;
            return SOCKET_ERROR;
        }
        if (ws->nonblocking) {
            wsa_last_error = WSAEWOULDBLOCK;
            return SOCKET_ERROR;
        }
        if (deadline && GetTickCount64() >= deadline) {
            wsa_last_error = WSAETIMEDOUT;
            return SOCKET_ERROR;
        }
        if (sched_sleep_ticks(1) < 0) sched_yield();
    }

    wsock_captured_buffer_t captured = { buf, (uint32_t)len };
    DWORD received = 0;
    DWORD output_flags = 0;
    int error = wsock_recv_datagram_captured_now(
        ws, &captured, 1, (DWORD)len, (DWORD)flags, from, fromlen,
        &received, &output_flags);
    (void)output_flags;
    if (error) {
        wsa_last_error = error;
        return SOCKET_ERROR;
    }
    wsa_last_error = 0;
    return (int)received;
}

int WINAPI wsock_sendto(SOCKET s, PCSTR buf, int len, int flags,
                       PVOID to, int tolen)
{
    int idx = sock_to_index(s);
    if (idx < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }

    win32_socket_t *ws = &win32_sockets[idx];
    if (ws->type == SOCK_STREAM)
        return wsock_send(s, buf, len, flags);
    if (ws->type != SOCK_DGRAM || ws->kern_socket_id < 0) {
        wsa_last_error = WSAEOPNOTSUPP;
        return SOCKET_ERROR;
    }
    if (ws->shutdown_send) {
        wsa_last_error = WSAESHUTDOWN;
        return SOCKET_ERROR;
    }
    if (len < 0) { wsa_last_error = WSAEINVAL; return SOCKET_ERROR; }
    if (len && !buf) { wsa_last_error = WSAEFAULT; return SOCKET_ERROR; }
    if (len > 65507) { wsa_last_error = WSAEMSGSIZE; return SOCKET_ERROR; }
    if (flags) { wsa_last_error = WSAEOPNOTSUPP; return SOCKET_ERROR; }

    uint8_t dst_ip[4];
    uint16_t dst_port;

    if (to) {
        int parse_error = wsock_parse_ipv4_endpoint(
            (const uint8_t *)to, tolen, dst_ip, &dst_port);
        if (parse_error) {
            wsa_last_error = parse_error;
            return SOCKET_ERROR;
        }
    } else if (ws->connected) {
        ws_memcpy(dst_ip, ws->remote_ip, 4);
        dst_port = ws->remote_port;
    } else {
        wsa_last_error = WSAENOTCONN;
        return SOCKET_ERROR;
    }
    if (!dst_port) {
        wsa_last_error = WSAEADDRNOTAVAIL;
        return SOCKET_ERROR;
    }
    BOOL broadcast = dst_ip[0] == 255 && dst_ip[1] == 255 &&
                     dst_ip[2] == 255 && dst_ip[3] == 255;
    if (broadcast && !(ws->option_flags & WSOCK_OPT_BROADCAST)) {
        wsa_last_error = WSAEACCES;
        return SOCKET_ERROR;
    }
    if (wsock_bind_implicit(ws) < 0) {
        wsa_last_error = WSAENOBUFS;
        return SOCKET_ERROR;
    }

    uint8_t endpoint[16];
    wsock_store_sockaddr(endpoint, dst_ip, dst_port);
    int ret = kernel_sock_sendto(ws->kern_socket_id, buf, (uint32_t)len,
                                 flags, endpoint);
    if (ret < 0) {
        wsa_last_error = WSAENETDOWN;
        return SOCKET_ERROR;
    }
    wsock_propagate_shared_state(ws);
    wsa_last_error = 0;
    return ret;
}

static BOOL WINAPI wsock_connect_ex(SOCKET s, PVOID name, int namelen,
                                     PVOID send_buf, DWORD send_len,
                                     DWORD *bytes_sent, PVOID overlapped)
{
    if (bytes_sent) *bytes_sent = 0;
    if (!overlapped || (send_len && !send_buf)) {
        wsa_last_error = WSAEFAULT;
        return FALSE;
    }

    int index = sock_to_index(s);
    if (index < 0) {
        wsa_last_error = WSAENOTSOCK;
        return FALSE;
    }
    win32_socket_t *ws = &win32_sockets[index];
    if (ws->type != SOCK_STREAM) {
        wsa_last_error = WSAEOPNOTSUPP;
        return FALSE;
    }
    /* ConnectEx requires an explicitly bound socket, including bind(..., 0). */
    if (!ws->local_port) {
        wsa_last_error = WSAEINVAL;
        return FALSE;
    }

    wsock_pending_connect_t *slot = NULL;
    for (int i = 0; i < MAX_PENDING_CONNECTEX; i++) {
        LONG expected = 0;
        if (__atomic_compare_exchange_n(&g_pending_connects[i].state,
                                        &expected, 2, FALSE,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            slot = &g_pending_connects[i];
            break;
        }
    }
    if (!slot) {
        wsa_last_error = WSAENOBUFS;
        return FALSE;
    }

    uint8_t ip[4];
    uint16_t port;
    int prepare_error = wsock_prepare_connect(s, name, namelen, &ws, ip,
                                               &port);
    if (prepare_error) {
        __atomic_store_n(&slot->state, 0, __ATOMIC_RELEASE);
        wsa_last_error = prepare_error;
        return FALSE;
    }

    int conn = net_tcp_connect_begin(ip, port, ws->local_port);
    if (conn < 0) {
        __atomic_store_n(&slot->state, 0, __ATOMIC_RELEASE);
        wsock_signal_socket_error(ws, WSAECONNREFUSED, WSOCK_FD_CONNECT);
        wsa_last_error = WSAECONNREFUSED;
        return FALSE;
    }

    ws->kern_conn_id = conn;
    ws->connected = 0;
    ws->connecting = 1;
    ws->pending_error = 0;
    wsock_propagate_shared_state(ws);

    slot->owner_pid = win32_current_process_id();
    slot->owner_tid = GetCurrentThreadId();
    slot->compat32 = g_compat32_mode;
    slot->socket = s;
    slot->socket_generation = ws->generation;
    slot->send_buffer = send_buf;
    slot->send_length = send_len;
    slot->overlapped = overlapped;
    slot->conn_id = conn;
    slot->deadline_ms = GetTickCount64() + WSOCK_CONNECT_TIMEOUT_MS;
    wsock_set_overlapped_status(overlapped, slot->compat32,
                                STATUS_PENDING, 0);
    __atomic_store_n(&slot->state, 1, __ATOMIC_RELEASE);

    serial_puts("[WSOCK] ConnectEx pending socket=");
    serial_puthex((uint64_t)s, 8);
    serial_puts(" conn=");
    serial_puthex((uint32_t)conn, 2);
    serial_puts(" overlapped=");
    serial_puthex((uint64_t)(uintptr_t)overlapped, 16);
    serial_puts("\n");

    wsock_service_pending_connects(slot->owner_pid);
    if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) == 0) {
        NTSTATUS status;
        DWORD transferred;
        HANDLE event;
        if (!wsock_overlapped_status(overlapped, &status, &transferred,
                                     &event)) {
            wsa_last_error = WSAEFAULT;
            return FALSE;
        }
        if (!NT_SUCCESS(status)) {
            wsa_last_error = wsock_status_error(status);
            return FALSE;
        }
        if (bytes_sent) *bytes_sent = transferred;
        wsa_last_error = 0;
        return TRUE;
    }

    wsa_last_error = WSA_IO_PENDING;
    return FALSE;
}

static BOOL WINAPI wsock_disconnect_ex(SOCKET s, PVOID overlapped,
                                        DWORD flags, DWORD reserved)
{
    (void)overlapped;
    (void)flags;
    (void)reserved;
    int idx = sock_to_index(s);
    if (idx < 0) {
        wsa_last_error = WSAENOTSOCK;
        return FALSE;
    }
    win32_socket_t *ws = &win32_sockets[idx];
    if (ws->kern_conn_id >= 0) net_tcp_shutdown(ws->kern_conn_id);
    ws->kern_conn_id = -1;
    ws->connected = 0;
    ws->connecting = 0;
    wsa_last_error = 0;
    return TRUE;
}

/* Byte order conversion — these are pure functions, no network needed */
ULONG  WINAPI ntohl(ULONG n)  { return ((n>>24)&0xFF)|((n>>8)&0xFF00)|((n<<8)&0xFF0000)|((n<<24)&0xFF000000); }
ULONG  WINAPI htonl(ULONG h)  { return ntohl(h); }
USHORT WINAPI ntohs(USHORT n) { return (USHORT)(((n>>8)&0xFF)|((n<<8)&0xFF00)); }
USHORT WINAPI htons(USHORT h) { return ntohs(h); }

static void wsock_test_expect(BOOL condition, const char *name,
                              int *checks, int *failures)
{
    (*checks)++;
    if (condition) return;
    (*failures)++;
    serial_puts("[WSOCKTEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

static BOOL wsock_test_bytes_equal(const BYTE *left, const BYTE *right,
                                   SIZE_T length)
{
    for (SIZE_T i = 0; i < length; i++)
        if (left[i] != right[i]) return FALSE;
    return TRUE;
}

typedef struct {
    ULONG_PTR completion_key;
    PVOID overlapped;
    ULONG_PTR internal;
    DWORD bytes;
    DWORD padding;
} wsock_test_overlapped_entry_t;

static volatile HANDLE g_wsock_test_iocp_port;
static volatile PVOID g_wsock_test_iocp_overlapped;

static void wsock_test_iocp_poster(void)
{
    if (sched_sleep_ticks(2) < 0)
        sched_yield();
    (void)PostQueuedCompletionStatus(
        (HANDLE)g_wsock_test_iocp_port, 0x1357U, 0x2468U,
        (PVOID)g_wsock_test_iocp_overlapped);
}

int wsock_selftest(void)
{
    static const uint8_t loopback[4] = { 127, 0, 0, 1 };
    static const BYTE first_payload[6] = { 'a', 'b', 'c', 'd', 'e', 'f' };
    static const BYTE truncated_payload[8] = {
        't', 'r', 'u', 'n', 'c', 'a', 't', 'e'
    };
    static const BYTE message_payload[7] = {
        'm', 'e', 's', 's', 'a', 'g', 'e'
    };
    WSADATA startup_data;
    ADDRINFO64 addrinfo_hints;
    PVOID addrinfo_result = NULL;
    wsock_protocol_info_w_t protocols[2];
    wsock_protocol_info_w_t duplicate_info;
    SOCKET server = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET duplicate = INVALID_SOCKET;
    SOCKET tcp_listener = INVALID_SOCKET;
    SOCKET tcp_client = INVALID_SOCKET;
    SOCKET tcp_peer = INVALID_SOCKET;
    SOCKET closed_listener = INVALID_SOCKET;
    SOCKET closed_client = INVALID_SOCKET;
    SOCKET closed_peer = INVALID_SOCKET;
    SOCKET capacity_sockets[96];
    int capacity_count = 0;
    HANDLE completion_port = NULL;
    uint8_t server_address[16];
    uint8_t tcp_address[16];
    uint8_t source_address[28];
    int server_address_length = sizeof(server_address);
    int tcp_address_length = sizeof(tcp_address);
    int source_address_length = sizeof(source_address);
    DWORD protocol_bytes = 0;
    DWORD bytes = 0;
    DWORD flags = 0;
    ULONG available = 0;
    int option = 0;
    int option_length = sizeof(option);
    FDSET64 set;
    WSOCK_TIMEVAL zero_timeout = { 0, 0 };
    WSOCK_TIMEVAL short_timeout = { 0, 2000 };
    BYTE receive_a[3];
    BYTE receive_b[4];
    WSABUF64 receive_buffers[2];
    WSABUF64 one_buffer;
    WSABUF64 empty_buffer;
    WSABUF64 message_send_buffers[2];
    WSABUF64 message_receive_buffers[2];
    WSAMSG64 send_message;
    WSAMSG64 receive_message;
    BYTE message_receive_a[3];
    BYTE message_receive_b[4];
    uint8_t message_source[28];
    uint64_t extension64 = 0;
    uint32_t extension32 = 0;
    DWORD extension_bytes = 0;
    WSAMSG32 layout32;
    wsock_message_view_t layout_view;
    ULONG_PTR overlapped[4];
    BYTE pending_data[4];
    WSABUF64 pending_buffer;
    ULONG_PTR fifo_hole_overlapped[4];
    ULONG_PTR fifo_oldest_overlapped[4];
    ULONG_PTR fifo_newest_overlapped[4];
    BYTE fifo_receive_data[3];
    WSABUF64 fifo_hole_buffer;
    WSABUF64 fifo_oldest_buffer;
    WSABUF64 fifo_newest_buffer;
    DWORD completion_bytes = 0;
    ULONG_PTR completion_key = 0;
    PVOID completion_overlapped = NULL;
    DWORD result_flags = 0;
    int saved_mode = g_compat32_mode;
    int checks = 0;
    int failures = 0;
    BOOL startup_done = FALSE;

    serial_puts("[WSOCKTEST] starting Winsock contract test\n");
    if (saved_mode) {
        serial_puts("[WSOCKTEST] FAIL: cannot run during a PE32 callback\n");
        return 1;
    }

    ws_memset(&startup_data, 0, sizeof(startup_data));
    int result = WSAStartup(0x0202, &startup_data);
    wsock_test_expect(result == 0, "WSAStartup", &checks, &failures);
    if (result != 0) goto cleanup;
    startup_done = TRUE;
    wsock_test_expect(startup_data.wVersion == 0x0202,
                      "WSAStartup negotiated version", &checks, &failures);

    for (int i = 0; i < (int)(sizeof(capacity_sockets) /
                              sizeof(capacity_sockets[0])); i++) {
        capacity_sockets[i] = wsock_socket(AF_INET, SOCK_STREAM,
                                           IPPROTO_TCP);
        if (capacity_sockets[i] == INVALID_SOCKET)
            break;
        capacity_count++;
    }
    wsock_test_expect(capacity_count ==
                          (int)(sizeof(capacity_sockets) /
                                sizeof(capacity_sockets[0])),
                      "more than FD_SETSIZE socket handles", &checks,
                      &failures);
    while (capacity_count > 0)
        closesocket(capacity_sockets[--capacity_count]);

    result = wsock_WSAEnumProtocolsW(NULL, NULL, &protocol_bytes);
    wsock_test_expect(result == SOCKET_ERROR &&
                      WSAGetLastError() == WSAENOBUFS,
                      "WSAEnumProtocolsW sizing error", &checks, &failures);
    wsock_test_expect(protocol_bytes == sizeof(protocols),
                      "WSAEnumProtocolsW sizing", &checks, &failures);
    ws_memset(protocols, 0, sizeof(protocols));
    result = wsock_WSAEnumProtocolsW(NULL, protocols, &protocol_bytes);
    wsock_test_expect(result == 2, "WSAEnumProtocolsW entries",
                      &checks, &failures);
    wsock_test_expect(protocols[0].header.socket_type == SOCK_STREAM &&
                      protocols[0].header.protocol == IPPROTO_TCP &&
                      protocols[1].header.socket_type == SOCK_DGRAM &&
                      protocols[1].header.protocol == IPPROTO_UDP,
                      "protocol catalog contents", &checks, &failures);

    {
        PVOID proto = wsock_getprotobyname("TCP");
        wsock_test_expect(proto &&
                          *(int16_t *)((BYTE *)proto + 16) == IPPROTO_TCP,
                          "getprotobyname TCP", &checks, &failures);
        proto = wsock_getprotobynumber(IPPROTO_UDP);
        wsock_test_expect(proto &&
                          *(int16_t *)((BYTE *)proto + 16) == IPPROTO_UDP,
                          "getprotobynumber UDP", &checks, &failures);
    }

    wsock_test_expect(wsock_inet_addr("127.0.0.1") == 0x0100007FU,
                      "inet_addr dotted decimal", &checks, &failures);
    wsock_test_expect(wsock_inet_addr("127.1") == 0x0100007FU,
                      "inet_addr two-part form", &checks, &failures);
    wsock_test_expect(wsock_inet_addr("0x7f.1") == 0x0100007FU,
                      "inet_addr hexadecimal form", &checks, &failures);
    wsock_test_expect(wsock_inet_addr("0300.0250.01.01") == 0x0101A8C0U,
                      "inet_addr octal form", &checks, &failures);
    wsock_test_expect(wsock_inet_addr("256.1.1.1") == 0xFFFFFFFFU,
                      "inet_addr range rejection", &checks, &failures);

    ws_memset(&addrinfo_hints, 0, sizeof(addrinfo_hints));
    addrinfo_hints.ai_family = AF_INET;
    addrinfo_hints.ai_socktype = SOCK_STREAM;
    addrinfo_hints.ai_protocol = IPPROTO_TCP;
    result = wsock_getaddrinfo("localhost", "49153", &addrinfo_hints,
                               &addrinfo_result);
    {
        ADDRINFO64 *resolved = (ADDRINFO64 *)addrinfo_result;
        const BYTE *address = resolved ? (const BYTE *)resolved->ai_addr : NULL;
        wsock_test_expect(result == 0 && resolved &&
                          resolved->ai_family == AF_INET &&
                          resolved->ai_socktype == SOCK_STREAM &&
                          resolved->ai_protocol == IPPROTO_TCP &&
                          resolved->ai_addrlen == 16 && !resolved->ai_next &&
                          address && address[0] == AF_INET &&
                          address[2] == 0xC0 && address[3] == 0x01 &&
                          wsock_test_bytes_equal(address + 4, loopback, 4),
                          "getaddrinfo localhost loopback", &checks,
                          &failures);
    }
    if (addrinfo_result) wsock_freeaddrinfo(addrinfo_result);

    addrinfo_result = NULL;
    result = wsock_getaddrinfo("LOCALHOST.", "443", &addrinfo_hints,
                               &addrinfo_result);
    {
        ADDRINFO64 *resolved = (ADDRINFO64 *)addrinfo_result;
        const BYTE *address = resolved ? (const BYTE *)resolved->ai_addr : NULL;
        wsock_test_expect(result == 0 && address &&
                          address[2] == 0x01 && address[3] == 0xBB &&
                          wsock_test_bytes_equal(address + 4, loopback, 4),
                          "getaddrinfo localhost case and trailing dot",
                          &checks, &failures);
    }
    if (addrinfo_result) wsock_freeaddrinfo(addrinfo_result);

    addrinfo_result = NULL;
    addrinfo_hints.ai_flags = 4; /* AI_NUMERICHOST */
    result = wsock_getaddrinfo("localhost", NULL, &addrinfo_hints,
                               &addrinfo_result);
    wsock_test_expect(result == WSAHOST_NOT_FOUND && !addrinfo_result &&
                      WSAGetLastError() == WSAHOST_NOT_FOUND,
                      "AI_NUMERICHOST rejects localhost", &checks,
                      &failures);

    tcp_listener = wsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    tcp_client = wsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    wsock_test_expect(tcp_listener != INVALID_SOCKET &&
                      tcp_client != INVALID_SOCKET,
                      "TCP loopback socket creation", &checks, &failures);
    if (tcp_listener != INVALID_SOCKET && tcp_client != INVALID_SOCKET) {
        wsock_store_sockaddr(tcp_address, loopback, 0);
        result = wsock_bind(tcp_listener, tcp_address,
                            sizeof(tcp_address));
        wsock_test_expect(result == 0, "TCP loopback bind", &checks,
                          &failures);
        if (result == 0) {
            result = wsock_getsockname(tcp_listener, tcp_address,
                                       &tcp_address_length);
            wsock_test_expect(result == 0 && tcp_address_length == 16 &&
                              (tcp_address[2] || tcp_address[3]),
                              "TCP loopback endpoint", &checks, &failures);
        }
        if (result == 0) {
            result = wsock_listen(tcp_listener, 1);
            wsock_test_expect(result == 0, "TCP loopback listen", &checks,
                              &failures);
        }
        if (result == 0) {
            result = wsock_connect(tcp_client, tcp_address,
                                   sizeof(tcp_address));
            wsock_test_expect(result == 0, "TCP loopback connect", &checks,
                              &failures);
        }
        if (result == 0) {
            tcp_peer = wsock_accept(tcp_listener, NULL, NULL);
            wsock_test_expect(tcp_peer != INVALID_SOCKET,
                              "TCP loopback accept", &checks, &failures);
        }
        if (tcp_peer != INVALID_SOCKET) {
            uint32_t keepalive_values[3] = { 1, 10, 10 };
            DWORD keepalive_bytes = 0;
            result = WSAIoctl(tcp_client, SIO_KEEPALIVE_VALS,
                              keepalive_values, sizeof(keepalive_values),
                              NULL, 0, &keepalive_bytes, NULL, NULL);
            wsock_test_expect(result == 0, "TCP keepalive configuration",
                              &checks, &failures);
            if (result == 0) {
                static const BYTE keepalive_payload = 'k';
                BYTE keepalive_receive = 0;
                if (sched_sleep_ticks(20) < 0)
                    for (int i = 0; i < 20; i++) sched_yield();
                result = wsock_send(tcp_client,
                                    (PCSTR)&keepalive_payload, 1, 0);
                wsock_test_expect(result == 1,
                                  "TCP survives loopback keepalive probes",
                                  &checks, &failures);
                if (result == 1) {
                    result = wsock_recv(tcp_peer,
                                        (PSTR)&keepalive_receive, 1, 0);
                    wsock_test_expect(result == 1 &&
                                      keepalive_receive == keepalive_payload,
                                      "TCP data after keepalive probes",
                                      &checks, &failures);
                }
            }

            int peer_index = sock_to_index(tcp_peer);
            wsock_test_expect(peer_index >= 0 &&
                              win32_sockets[peer_index].kern_conn_id >= 0,
                              "TCP reset peer state", &checks, &failures);
            if (peer_index >= 0 &&
                win32_sockets[peer_index].kern_conn_id >= 0) {
                static const BYTE reset_payload = 'r';
                net_tcp_abort(win32_sockets[peer_index].kern_conn_id);
                result = wsock_send(tcp_client,
                                    (PCSTR)&reset_payload, 1, 0);
                wsock_test_expect(result == SOCKET_ERROR &&
                                  WSAGetLastError() == WSAECONNRESET,
                                  "TCP reset maps to WSAECONNRESET",
                                  &checks, &failures);
            }
        }
    }
    if (tcp_peer != INVALID_SOCKET) {
        closesocket(tcp_peer);
        tcp_peer = INVALID_SOCKET;
    }
    if (tcp_client != INVALID_SOCKET) {
        closesocket(tcp_client);
        tcp_client = INVALID_SOCKET;
    }
    if (tcp_listener != INVALID_SOCKET) {
        closesocket(tcp_listener);
        tcp_listener = INVALID_SOCKET;
    }

    closed_listener = wsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    closed_client = wsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    wsock_test_expect(closed_listener != INVALID_SOCKET &&
                      closed_client != INVALID_SOCKET,
                      "TCP pre-accept close socket creation", &checks,
                      &failures);
    if (closed_listener != INVALID_SOCKET &&
        closed_client != INVALID_SOCKET) {
        tcp_address_length = sizeof(tcp_address);
        wsock_store_sockaddr(tcp_address, loopback, 0);
        result = wsock_bind(closed_listener, tcp_address,
                            sizeof(tcp_address));
        if (result == 0)
            result = wsock_getsockname(closed_listener, tcp_address,
                                       &tcp_address_length);
        if (result == 0)
            result = wsock_listen(closed_listener, 4);
        if (result == 0)
            result = wsock_connect(closed_client, tcp_address,
                                   sizeof(tcp_address));
        wsock_test_expect(result == 0, "TCP pre-accept close setup",
                          &checks, &failures);
        if (result == 0) {
            static const BYTE closed_payload = 'c';
            BYTE closed_receive = 0;
            result = wsock_send(closed_client,
                                (PCSTR)&closed_payload, 1, 0);
            wsock_test_expect(result == 1,
                              "TCP data queued before peer close",
                              &checks, &failures);
            closesocket(closed_client);
            closed_client = INVALID_SOCKET;
            for (int i = 0; i < 20; i++) {
                net_poll();
                sched_yield();
            }
            ULONG nonblocking = 1;
            wsock_ioctlsocket(closed_listener, WSOCK_FIONBIO,
                              &nonblocking);
            closed_peer = wsock_accept(closed_listener, NULL, NULL);
            wsock_test_expect(closed_peer != INVALID_SOCKET,
                              "TCP accepts peer closed after request",
                              &checks, &failures);
            if (closed_peer != INVALID_SOCKET) {
                result = wsock_recv(closed_peer,
                                    (PSTR)&closed_receive, 1, 0);
                wsock_test_expect(result == 1 &&
                                  closed_receive == closed_payload,
                                  "TCP reads data after pre-accept close",
                                  &checks, &failures);
            }
        }
    }
    if (closed_peer != INVALID_SOCKET) {
        closesocket(closed_peer);
        closed_peer = INVALID_SOCKET;
    }
    if (closed_client != INVALID_SOCKET) {
        closesocket(closed_client);
        closed_client = INVALID_SOCKET;
    }
    if (closed_listener != INVALID_SOCKET) {
        closesocket(closed_listener);
        closed_listener = INVALID_SOCKET;
    }

    server = wsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    client = wsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    wsock_test_expect(server != INVALID_SOCKET && client != INVALID_SOCKET,
                      "UDP socket creation", &checks, &failures);
    if (server == INVALID_SOCKET || client == INVALID_SOCKET) goto cleanup;

    option = 1;
    result = wsock_setsockopt(server, SOL_SOCKET, SO_RANDOMIZE_PORT,
                              (PCSTR)&option, sizeof(option));
    wsock_test_expect(result == 0, "SO_RANDOMIZE_PORT set",
                      &checks, &failures);
    option = 0;
    option_length = sizeof(option);
    result = wsock_getsockopt(server, SOL_SOCKET, SO_RANDOMIZE_PORT,
                              (PSTR)&option, &option_length);
    wsock_test_expect(result == 0 && option == 1,
                      "SO_RANDOMIZE_PORT get", &checks, &failures);

    wsock_store_sockaddr(server_address, loopback, 0);
    result = wsock_bind(server, server_address, sizeof(server_address));
    wsock_test_expect(result == 0, "UDP ephemeral bind", &checks, &failures);
    result = wsock_getsockname(server, server_address,
                               &server_address_length);
    wsock_test_expect(result == 0 && server_address_length == 16 &&
                      (server_address[2] || server_address[3]),
                      "getsockname bound endpoint", &checks, &failures);
    option = 0;
    result = wsock_setsockopt(server, SOL_SOCKET, SO_RANDOMIZE_PORT,
                              (PCSTR)&option, sizeof(option));
    wsock_test_expect(result == SOCKET_ERROR &&
                      WSAGetLastError() == WSAEINVAL,
                      "SO_RANDOMIZE_PORT rejects bound socket",
                      &checks, &failures);

    option = 4096;
    result = wsock_setsockopt(server, SOL_SOCKET, SO_RCVBUF,
                              (PCSTR)&option, sizeof(option));
    wsock_test_expect(result == 0, "SO_RCVBUF set", &checks, &failures);
    option = 0;
    option_length = sizeof(option);
    result = wsock_getsockopt(server, SOL_SOCKET, SO_RCVBUF,
                              (PSTR)&option, &option_length);
    wsock_test_expect(result == 0 && option == 4096,
                      "SO_RCVBUF get", &checks, &failures);
    option = 0;
    result = wsock_setsockopt(client, SOL_SOCKET, SO_SNDBUF,
                              (PCSTR)&option, sizeof(option));
    wsock_test_expect(result == 0, "SO_SNDBUF unbuffered set",
                      &checks, &failures);
    option = -1;
    option_length = sizeof(option);
    result = wsock_getsockopt(client, SOL_SOCKET, SO_SNDBUF,
                              (PSTR)&option, &option_length);
    wsock_test_expect(result == 0 && option == 0,
                      "SO_SNDBUF unbuffered get", &checks, &failures);
    option = 1;
    result = wsock_setsockopt(server, SOL_SOCKET, SO_DEBUG,
                              (PCSTR)&option, sizeof(option));
    wsock_test_expect(result == SOCKET_ERROR &&
                      WSAGetLastError() == WSAEOPNOTSUPP,
                      "unsupported option reports error", &checks,
                      &failures);

    ws_memset(&set, 0, sizeof(set));
    set.count = 1;
    set.sockets[0] = client;
    result = wsock_select(0, NULL, &set, NULL, &zero_timeout);
    wsock_test_expect(result == 1 && set.count == 1 &&
                      set.sockets[0] == client,
                      "select writable socket", &checks, &failures);
    ws_memset(&set, 0, sizeof(set));
    set.count = 1;
    set.sockets[0] = server;
    result = wsock_select(0, &set, NULL, NULL, &short_timeout);
    wsock_test_expect(result == 0 && set.count == 0,
                      "select finite timeout", &checks, &failures);
    ws_memset(&set, 0, sizeof(set));
    set.count = 1;
    set.sockets[0] = 0xFFFFFFFFU;
    result = wsock_select(0, &set, NULL, NULL, &zero_timeout);
    wsock_test_expect(result == SOCKET_ERROR &&
                      WSAGetLastError() == WSAENOTSOCK,
                      "select invalid socket", &checks, &failures);

    result = wsock_sendto(client, (PCSTR)first_payload,
                          sizeof(first_payload), 0, server_address,
                          sizeof(server_address));
    wsock_test_expect(result == (int)sizeof(first_payload),
                      "UDP loopback send", &checks, &failures);
    result = wsock_ioctlsocket(server, WSOCK_FIONREAD, &available);
    wsock_test_expect(result == 0 && available == sizeof(first_payload),
                      "FIONREAD reports first datagram", &checks, &failures);

    ws_memset(receive_a, 0, sizeof(receive_a));
    ws_memset(receive_b, 0, sizeof(receive_b));
    receive_buffers[0].len = 2;
    receive_buffers[0].padding = 0;
    receive_buffers[0].buf = receive_a;
    receive_buffers[1].len = 4;
    receive_buffers[1].padding = 0;
    receive_buffers[1].buf = receive_b;
    bytes = 0;
    flags = WSOCK_MSG_PEEK;
    source_address_length = sizeof(source_address);
    result = wsock_WSARecvFrom(server, receive_buffers, 2, &bytes, &flags,
                               source_address, &source_address_length,
                               NULL, NULL);
    wsock_test_expect(result == 0 && bytes == sizeof(first_payload) &&
                      flags == 0 &&
                      wsock_test_bytes_equal(receive_a, first_payload, 2) &&
                      wsock_test_bytes_equal(receive_b, first_payload + 2, 4),
                      "WSARecvFrom scatter and peek", &checks, &failures);
    wsock_test_expect(source_address_length == 16 &&
                      source_address[0] == AF_INET &&
                      (source_address[2] || source_address[3]),
                      "WSARecvFrom source endpoint", &checks, &failures);
    available = 0;
    wsock_ioctlsocket(server, WSOCK_FIONREAD, &available);
    wsock_test_expect(available == sizeof(first_payload),
                      "MSG_PEEK preserves datagram", &checks, &failures);

    ws_memset(receive_a, 0, sizeof(receive_a));
    ws_memset(receive_b, 0, sizeof(receive_b));
    bytes = 0;
    flags = 0;
    result = wsock_WSARecvFrom(server, receive_buffers, 2, &bytes, &flags,
                               NULL, NULL, NULL, NULL);
    wsock_test_expect(result == 0 && bytes == sizeof(first_payload) &&
                      wsock_test_bytes_equal(receive_a, first_payload, 2) &&
                      wsock_test_bytes_equal(receive_b, first_payload + 2, 4),
                      "WSARecvFrom consumes one datagram", &checks,
                      &failures);

    ws_memset(&duplicate_info, 0, sizeof(duplicate_info));
    result = wsock_WSADuplicateSocketW(client, win32_current_process_id(),
                                        &duplicate_info);
    wsock_test_expect(result == 0, "WSADuplicateSocketW", &checks,
                      &failures);
    if (result == 0) {
        duplicate = wsock_WSASocketA(-1, -1, -1, &duplicate_info, 0, 1);
        wsock_test_expect(duplicate != INVALID_SOCKET,
                          "WSASocket consumes duplicate token", &checks,
                          &failures);
        if (duplicate != INVALID_SOCKET) {
            int client_index = sock_to_index(client);
            int duplicate_index = sock_to_index(duplicate);
            wsock_test_expect(client_index >= 0 && duplicate_index >= 0 &&
                              wsock_same_backend(&win32_sockets[client_index],
                                                 &win32_sockets[duplicate_index]),
                              "duplicated socket shares backend", &checks,
                              &failures);
            closesocket(duplicate);
            duplicate = INVALID_SOCKET;
        }
    }

    result = wsock_sendto(client, (PCSTR)truncated_payload,
                          sizeof(truncated_payload), 0, server_address,
                          sizeof(server_address));
    wsock_test_expect(result == (int)sizeof(truncated_payload),
                      "original socket survives duplicate close", &checks,
                      &failures);
    ws_memset(receive_a, 0, sizeof(receive_a));
    one_buffer.len = sizeof(receive_a);
    one_buffer.padding = 0;
    one_buffer.buf = receive_a;
    bytes = 0;
    flags = 0;
    result = wsock_WSARecvFrom(server, &one_buffer, 1, &bytes, &flags,
                               NULL, NULL, NULL, NULL);
    wsock_test_expect(result == SOCKET_ERROR &&
                      WSAGetLastError() == WSAEMSGSIZE &&
                      bytes == sizeof(receive_a) &&
                      flags == WSOCK_MSG_PARTIAL &&
                      wsock_test_bytes_equal(receive_a,
                                             truncated_payload,
                                             sizeof(receive_a)),
                      "UDP truncation contract", &checks, &failures);

    result = wsock_sendto(client, NULL, 0, 0, server_address,
                          sizeof(server_address));
    wsock_test_expect(result == 0, "zero-length UDP send", &checks,
                      &failures);
    ws_memset(&set, 0, sizeof(set));
    set.count = 1;
    set.sockets[0] = server;
    result = wsock_select(0, &set, NULL, NULL, &zero_timeout);
    wsock_test_expect(result == 1 && set.count == 1,
                      "zero-length datagram is readable", &checks,
                      &failures);
    empty_buffer.len = 0;
    empty_buffer.padding = 0;
    empty_buffer.buf = NULL;
    bytes = 99;
    flags = 0;
    result = wsock_WSARecvFrom(server, &empty_buffer, 1, &bytes, &flags,
                               NULL, NULL, NULL, NULL);
    wsock_test_expect(result == 0 && bytes == 0,
                      "zero-length datagram receive", &checks, &failures);

    message_send_buffers[0].len = 3;
    message_send_buffers[0].padding = 0;
    message_send_buffers[0].buf = (PVOID)message_payload;
    message_send_buffers[1].len = 4;
    message_send_buffers[1].padding = 0;
    message_send_buffers[1].buf = (PVOID)(message_payload + 3);
    ws_memset(&send_message, 0, sizeof(send_message));
    send_message.name = server_address;
    send_message.namelen = sizeof(server_address);
    send_message.buffers = message_send_buffers;
    send_message.buffer_count = 2;
    bytes = 0;
    result = wsock_WSASendMsg(client, &send_message, 0, &bytes, NULL, NULL);
    wsock_test_expect(result == 0 && bytes == sizeof(message_payload),
                      "WSASendMsg scatter", &checks, &failures);

    ws_memset(message_receive_a, 0, sizeof(message_receive_a));
    ws_memset(message_receive_b, 0, sizeof(message_receive_b));
    message_receive_buffers[0].len = sizeof(message_receive_a);
    message_receive_buffers[0].padding = 0;
    message_receive_buffers[0].buf = message_receive_a;
    message_receive_buffers[1].len = sizeof(message_receive_b);
    message_receive_buffers[1].padding = 0;
    message_receive_buffers[1].buf = message_receive_b;
    ws_memset(&receive_message, 0, sizeof(receive_message));
    receive_message.name = message_source;
    receive_message.namelen = sizeof(message_source);
    receive_message.buffers = message_receive_buffers;
    receive_message.buffer_count = 2;
    bytes = 0;
    result = wsock_WSARecvMsg(server, &receive_message, &bytes, NULL, NULL);
    wsock_test_expect(result == 0 && bytes == sizeof(message_payload) &&
                      receive_message.control.len == 0 &&
                      wsock_test_bytes_equal(message_receive_a,
                                             message_payload, 3) &&
                      wsock_test_bytes_equal(message_receive_b,
                                             message_payload + 3, 4),
                      "WSARecvMsg scatter", &checks, &failures);

    result = WSAIoctl(server, 0xC8000006U,
                      (PVOID)&g_wsock_wsa_recv_msg_id,
                      sizeof(g_wsock_wsa_recv_msg_id), &extension64,
                      sizeof(extension64), &extension_bytes, NULL, NULL);
    wsock_test_expect(result == 0 && extension_bytes == 8 &&
                      extension64 == (uint64_t)(uintptr_t)wsock_WSARecvMsg,
                      "WSAID_WSARECVMSG PE64 extension", &checks,
                      &failures);
    extension64 = 0;
    extension_bytes = 0;
    result = WSAIoctl(server, 0xC8000006U,
                      (PVOID)&g_wsock_wsa_send_msg_id,
                      sizeof(g_wsock_wsa_send_msg_id), &extension64,
                      sizeof(extension64), &extension_bytes, NULL, NULL);
    wsock_test_expect(result == 0 && extension_bytes == 8 &&
                      extension64 == (uint64_t)(uintptr_t)wsock_WSASendMsg,
                      "WSAID_WSASENDMSG PE64 extension", &checks,
                      &failures);

    ws_memset(&layout32, 0, sizeof(layout32));
    layout32.name = 0x11112222U;
    layout32.namelen = 16;
    layout32.buffers = 0x33334444U;
    layout32.buffer_count = 2;
    layout32.control.buf = 0x55556666U;
    layout32.control.len = 7;
    layout32.flags = WSOCK_MSG_PEEK;
    result = wsock_message_view(&layout32, TRUE, &layout_view);
    wsock_test_expect(result == 0 &&
                      (uintptr_t)layout_view.name == 0x11112222U &&
                      (uintptr_t)layout_view.buffers == 0x33334444U &&
                      (uintptr_t)layout_view.control == 0x55556666U &&
                      layout_view.buffer_count == 2 &&
                      *layout_view.control_length == 7 &&
                      *layout_view.flags == WSOCK_MSG_PEEK,
                      "PE32 WSAMSG layout", &checks, &failures);

    if (!compat32_is_initialized())
        compat32_init();
    g_compat32_mode = 1;
    extension_bytes = 0;
    result = WSAIoctl(server, 0xC8000006U,
                      (PVOID)&g_wsock_wsa_recv_msg_id,
                      sizeof(g_wsock_wsa_recv_msg_id), &extension32,
                      sizeof(extension32), &extension_bytes, NULL, NULL);
    g_compat32_mode = 0;
    wsock_test_expect(result == 0 && extension_bytes == 4 && extension32,
                      "WSAID_WSARECVMSG PE32 thunk", &checks, &failures);

    completion_port = CreateIoCompletionPort(
        (HANDLE)(ULONG_PTR)server, NULL, 0xA55AU, 1);
    wsock_test_expect(completion_port != NULL,
                      "socket IOCP association", &checks, &failures);
    if (completion_port) {
        wsock_test_expect(
            SetFileCompletionNotificationModes(
                (HANDLE)(ULONG_PTR)server, 1),
            "socket skip-on-success mode", &checks, &failures);
        ws_memset(overlapped, 0, sizeof(overlapped));
        ws_memset(pending_data, 0, sizeof(pending_data));
        ws_memset(source_address, 0, sizeof(source_address));
        pending_buffer.len = sizeof(pending_data);
        pending_buffer.padding = 0;
        pending_buffer.buf = pending_data;
        bytes = 0xA5A5A5A5U;
        flags = 0;
        source_address_length = sizeof(source_address);
        result = wsock_WSARecvFrom(server, &pending_buffer, 1, &bytes,
                                   &flags, source_address,
                                   &source_address_length, overlapped, NULL);
        wsock_test_expect(result == SOCKET_ERROR &&
                          WSAGetLastError() == 997 &&
                          (NTSTATUS)overlapped[0] == STATUS_PENDING &&
                          bytes == 0xA5A5A5A5U && flags == 0 &&
                          source_address_length == sizeof(source_address),
                          "overlapped UDP receive pending", &checks,
                          &failures);

        result = wsock_sendto(client, (PCSTR)truncated_payload,
                              sizeof(truncated_payload), 0, server_address,
                              sizeof(server_address));
        wsock_test_expect(result == (int)sizeof(truncated_payload),
                          "overlapped UDP test send", &checks, &failures);
        completion_bytes = 0;
        completion_key = 0;
        completion_overlapped = NULL;
        BOOL dequeued = GetQueuedCompletionStatus(
            completion_port, &completion_bytes, &completion_key,
            &completion_overlapped, 100);
        wsock_test_expect(!dequeued &&
                          completion_bytes == sizeof(pending_data) &&
                          completion_key == 0xA55AU &&
                          completion_overlapped == overlapped,
                          "IOCP reports overlapped UDP truncation", &checks,
                          &failures);
        result_flags = 0;
        completion_bytes = 0;
        BOOL completed = wsock_WSAGetOverlappedResult(
            server, overlapped, &completion_bytes, FALSE, &result_flags);
        wsock_test_expect(!completed && WSAGetLastError() == WSAEMSGSIZE &&
                          completion_bytes == sizeof(pending_data) &&
                          result_flags == WSOCK_MSG_PARTIAL &&
                          bytes == 0xA5A5A5A5U && flags == 0 &&
                          source_address_length == 16 &&
                          wsock_test_bytes_equal(
                              pending_data, truncated_payload,
                              sizeof(pending_data)),
                          "delayed WSARecvFrom preserves call outputs",
                          &checks, &failures);

        ws_memset(overlapped, 0, sizeof(overlapped));
        bytes = 0x5A5A5A5AU;
        flags = 0;
        result = wsock_WSARecvFrom(server, &pending_buffer, 1, &bytes,
                                   &flags, NULL, NULL, overlapped, NULL);
        wsock_test_expect(result == SOCKET_ERROR &&
                          WSAGetLastError() == 997 &&
                          (NTSTATUS)overlapped[0] == STATUS_PENDING &&
                          bytes == 0x5A5A5A5AU,
                          "second overlapped UDP receive pending", &checks,
                          &failures);
        BOOL cancelled = wsock_cancel_io(server, overlapped, FALSE);
        wsock_test_expect(cancelled &&
                          (NTSTATUS)overlapped[0] == STATUS_CANCELLED &&
                          bytes == 0x5A5A5A5AU && flags == 0,
                          "CancelIoEx socket status", &checks, &failures);
        dequeued = GetQueuedCompletionStatus(
            completion_port, &completion_bytes, &completion_key,
            &completion_overlapped, 0);
        wsock_test_expect(!dequeued && completion_bytes == 0 &&
                          completion_key == 0xA55AU &&
                          completion_overlapped == overlapped,
                          "IOCP preserves cancelled completion", &checks,
                          &failures);
        result_flags = 0;
        completion_bytes = 99;
        completed = wsock_WSAGetOverlappedResult(
            server, overlapped, &completion_bytes, FALSE, &result_flags);
        wsock_test_expect(!completed && WSAGetLastError() == 995 &&
                          completion_bytes == 0 && result_flags == 0,
                          "WSAGetOverlappedResult cancellation", &checks,
                          &failures);

        ws_memset(fifo_hole_overlapped, 0,
                  sizeof(fifo_hole_overlapped));
        ws_memset(fifo_oldest_overlapped, 0,
                  sizeof(fifo_oldest_overlapped));
        ws_memset(fifo_newest_overlapped, 0,
                  sizeof(fifo_newest_overlapped));
        ws_memset(fifo_receive_data, 0, sizeof(fifo_receive_data));
        fifo_hole_buffer.len = 1;
        fifo_hole_buffer.padding = 0;
        fifo_hole_buffer.buf = &fifo_receive_data[0];
        fifo_oldest_buffer.len = 1;
        fifo_oldest_buffer.padding = 0;
        fifo_oldest_buffer.buf = &fifo_receive_data[1];
        fifo_newest_buffer.len = 1;
        fifo_newest_buffer.padding = 0;
        fifo_newest_buffer.buf = &fifo_receive_data[2];

        flags = 0;
        result = wsock_WSARecvFrom(
            server, &fifo_hole_buffer, 1, &bytes, &flags, NULL, NULL,
            fifo_hole_overlapped, NULL);
        BOOL fifo_hole_pending = result == SOCKET_ERROR &&
                                 WSAGetLastError() == 997;
        wsock_test_expect(fifo_hole_pending,
                          "FIFO test reserves first receive slot", &checks,
                          &failures);

        flags = 0;
        result = wsock_WSARecvFrom(
            server, &fifo_oldest_buffer, 1, &bytes, &flags, NULL, NULL,
            fifo_oldest_overlapped, NULL);
        BOOL fifo_oldest_pending = result == SOCKET_ERROR &&
                                   WSAGetLastError() == 997;
        wsock_test_expect(fifo_oldest_pending,
                          "FIFO test queues oldest active receive", &checks,
                          &failures);

        BOOL fifo_hole_cancelled = fifo_hole_pending &&
            wsock_cancel_io(server, fifo_hole_overlapped, FALSE);
        wsock_test_expect(fifo_hole_cancelled,
                          "FIFO test opens a lower receive slot", &checks,
                          &failures);
        if (fifo_hole_cancelled) {
            completion_bytes = 0;
            completion_key = 0;
            completion_overlapped = NULL;
            (void)GetQueuedCompletionStatus(
                completion_port, &completion_bytes, &completion_key,
                &completion_overlapped, 0);
            wsock_test_expect(completion_overlapped == fifo_hole_overlapped,
                              "FIFO test drains cancelled receive", &checks,
                              &failures);
        }

        flags = 0;
        result = wsock_WSARecvFrom(
            server, &fifo_newest_buffer, 1, &bytes, &flags, NULL, NULL,
            fifo_newest_overlapped, NULL);
        BOOL fifo_newest_pending = result == SOCKET_ERROR &&
                                   WSAGetLastError() == 997;
        wsock_test_expect(fifo_newest_pending,
                          "FIFO test reuses the lower receive slot", &checks,
                          &failures);

        if (fifo_oldest_pending && fifo_newest_pending) {
            static const BYTE fifo_payload[2] = { '1', '2' };
            result = wsock_sendto(client, (PCSTR)&fifo_payload[0], 1, 0,
                                  server_address, sizeof(server_address));
            wsock_test_expect(result == 1, "FIFO test sends first datagram",
                              &checks, &failures);
            completion_bytes = 0;
            completion_key = 0;
            completion_overlapped = NULL;
            BOOL fifo_dequeued = GetQueuedCompletionStatus(
                completion_port, &completion_bytes, &completion_key,
                &completion_overlapped, 100);
            wsock_test_expect(
                fifo_dequeued && completion_bytes == 1 &&
                completion_overlapped == fifo_oldest_overlapped &&
                fifo_receive_data[1] == fifo_payload[0],
                "overlapped receive FIFO survives slot reuse", &checks,
                &failures);

            result = wsock_sendto(client, (PCSTR)&fifo_payload[1], 1, 0,
                                  server_address, sizeof(server_address));
            wsock_test_expect(result == 1, "FIFO test sends second datagram",
                              &checks, &failures);
            completion_bytes = 0;
            completion_key = 0;
            completion_overlapped = NULL;
            fifo_dequeued = GetQueuedCompletionStatus(
                completion_port, &completion_bytes, &completion_key,
                &completion_overlapped, 100);
            wsock_test_expect(
                fifo_dequeued && completion_bytes == 1 &&
                completion_overlapped == fifo_newest_overlapped &&
                fifo_receive_data[2] == fifo_payload[1],
                "overlapped receive FIFO advances after completion", &checks,
                &failures);
        }

        if ((NTSTATUS)fifo_oldest_overlapped[0] == STATUS_PENDING) {
            (void)wsock_cancel_io(server, fifo_oldest_overlapped, FALSE);
            (void)GetQueuedCompletionStatus(
                completion_port, &completion_bytes, &completion_key,
                &completion_overlapped, 0);
        }
        if ((NTSTATUS)fifo_newest_overlapped[0] == STATUS_PENDING) {
            (void)wsock_cancel_io(server, fifo_newest_overlapped, FALSE);
            (void)GetQueuedCompletionStatus(
                completion_port, &completion_bytes, &completion_key,
                &completion_overlapped, 0);
        }

        wsock_test_overlapped_entry_t entry;
        ULONG removed = 0;
        ULONG_PTR posted_overlapped[4] = { 0 };
        ws_memset(&entry, 0, sizeof(entry));
        g_wsock_test_iocp_port = completion_port;
        g_wsock_test_iocp_overlapped = posted_overlapped;
        int poster_pid = sched_spawn("iocp-test-post", wsock_test_iocp_poster);
        BOOL dequeued_ex = poster_pid >= 0 && GetQueuedCompletionStatusEx(
            completion_port, &entry, 1, &removed, 500, FALSE);
        wsock_test_expect(dequeued_ex && removed == 1 &&
                          entry.completion_key == 0x2468U &&
                          entry.overlapped == posted_overlapped &&
                          entry.internal == STATUS_SUCCESS &&
                          entry.bytes == 0x1357U,
                          "blocking GQCSEx preserves completion packet",
                          &checks, &failures);
        g_wsock_test_iocp_port = NULL;
        g_wsock_test_iocp_overlapped = NULL;
    }

cleanup:
    g_compat32_mode = 0;
    if (duplicate != INVALID_SOCKET) closesocket(duplicate);
    if (tcp_peer != INVALID_SOCKET) closesocket(tcp_peer);
    if (tcp_client != INVALID_SOCKET) closesocket(tcp_client);
    if (tcp_listener != INVALID_SOCKET) closesocket(tcp_listener);
    if (closed_peer != INVALID_SOCKET) closesocket(closed_peer);
    if (closed_client != INVALID_SOCKET) closesocket(closed_client);
    if (closed_listener != INVALID_SOCKET) closesocket(closed_listener);
    if (server != INVALID_SOCKET) closesocket(server);
    if (client != INVALID_SOCKET) closesocket(client);
    if (completion_port) CloseHandle(completion_port);
    if (startup_done) WSACleanup();
    g_compat32_mode = saved_mode;

    serial_puts("[WSOCKTEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

/* ponytail: inbound AcceptEx stays pending until the TCP listener is real. */
static BOOL WINAPI wsock_AcceptEx(SOCKET listen_socket, SOCKET accept_socket,
                                  PVOID output, DWORD receive_length,
                                  DWORD local_length, DWORD remote_length,
                                  DWORD *bytes_received, PVOID overlapped)
{
    int listen_idx = sock_to_index(listen_socket);
    int accept_idx = sock_to_index(accept_socket);
    if (listen_idx < 0 || accept_idx < 0) {
        wsa_last_error = WSAENOTSOCK;
        return FALSE;
    }
    if (!overlapped || !output) {
        wsa_last_error = WSAEFAULT;
        return FALSE;
    }
    win32_socket_t *listener = &win32_sockets[listen_idx];
    win32_socket_t *accepted = &win32_sockets[accept_idx];
    if (!listener->listening || listener->kern_listener_id < 0 ||
        accepted->connected) {
        wsa_last_error = WSAEINVAL;
        return FALSE;
    }

    wsock_pending_accept_t *slot = NULL;
    for (int i = 0; i < MAX_PENDING_ACCEPTEX; i++) {
        LONG expected = 0;
        if (__atomic_compare_exchange_n(&g_pending_accepts[i].state,
                                        &expected, 2, FALSE,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            slot = &g_pending_accepts[i];
            break;
        }
    }
    if (!slot) {
        wsa_last_error = WSAENOBUFS;
        return FALSE;
    }

    slot->owner_pid = win32_current_process_id();
    slot->owner_tid = GetCurrentThreadId();
    slot->compat32 = g_compat32_mode;
    slot->listen_socket = listen_socket;
    slot->accept_socket = accept_socket;
    slot->listen_generation = listener->generation;
    slot->accept_generation = accepted->generation;
    slot->output = output;
    slot->receive_length = receive_length;
    slot->local_length = local_length;
    slot->remote_length = remote_length;
    slot->overlapped = overlapped;
    slot->conn_id = -1;
    wsock_set_overlapped_status(overlapped, slot->compat32,
                                STATUS_PENDING, 0);
    __atomic_store_n(&slot->state, 1, __ATOMIC_RELEASE);

    serial_puts("[WSOCK] AcceptEx pending listen=");
    serial_puthex((uint32_t)listen_socket, 4);
    serial_puts(" accept=");
    serial_puthex((uint32_t)accept_socket, 4);
    serial_puts(" recv=");
    serial_puthex(receive_length, 8);
    serial_puts("\n");
    wsock_service_pending_accepts(slot->owner_pid);
    if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) == 0) {
        NTSTATUS status;
        DWORD completed_bytes;
        HANDLE event;
        if (!wsock_overlapped_status(overlapped, &status, &completed_bytes,
                                     &event)) {
            wsa_last_error = WSAEFAULT;
            return FALSE;
        }
        if (!NT_SUCCESS(status)) {
            wsa_last_error = status == STATUS_CANCELLED
                           ? 995 : WSAECONNRESET;
            return FALSE;
        }
        if (bytes_received)
            *bytes_received = completed_bytes;
        wsa_last_error = 0;
        return TRUE;
    }
    wsa_last_error = 997; /* WSA_IO_PENDING */
    return FALSE;
}

static void WINAPI wsock_GetAcceptExSockaddrs(
    PVOID output, DWORD receive_length, DWORD local_length,
    DWORD remote_length, PVOID *local_addr, int *local_addrlen,
    PVOID *remote_addr, int *remote_addrlen)
{
    uint8_t *base = (uint8_t *)output;
    if (local_addr)
        *local_addr = base && local_length >= 32
                    ? base + receive_length + 16 : NULL;
    if (local_addrlen)
        *local_addrlen = base && local_length >= 32 ? 16 : 0;
    if (remote_addr)
        *remote_addr = base && remote_length >= 32
                     ? base + receive_length + local_length + 16 : NULL;
    if (remote_addrlen)
        *remote_addrlen = base && remote_length >= 32 ? 16 : 0;
}

typedef struct {
    uint32_t length, flags;
    uint64_t next, sockaddr;
    int32_t sockaddr_length;
    uint32_t pad;
    uint32_t prefix_origin, suffix_origin, dad_state;
    uint32_t valid_lifetime, preferred_lifetime, lease_lifetime;
    uint8_t on_link_prefix_length;
    uint8_t tail[7];
} iphlp_unicast64_t;

typedef struct {
    uint32_t length, flags;
    uint64_t next, sockaddr;
    int32_t sockaddr_length;
    uint32_t pad, prefix_length;
    uint32_t tail;
} iphlp_prefix64_t;

typedef struct {
    uint32_t length, if_index;
    uint64_t next, adapter_name, first_unicast;
    uint64_t first_anycast, first_multicast, first_dns_server;
    uint64_t dns_suffix, description, friendly_name;
    uint8_t physical_address[8];
    uint32_t physical_address_length, flags, mtu, if_type, oper_status;
    uint32_t ipv6_if_index, zone_indices[16];
    uint64_t first_prefix;
    uint8_t tail[264];
} iphlp_adapter64_t;

typedef struct {
    iphlp_adapter64_t adapter;
    iphlp_unicast64_t unicast;
    iphlp_prefix64_t prefix;
    uint8_t unicast_sockaddr[16];
    uint8_t prefix_sockaddr[16];
    char adapter_name[8];
    WCHAR friendly_name[8];
    WCHAR dns_suffix[1];
} iphlp_payload64_t;

typedef struct {
    union {
        uint64_t alignment;
        struct { uint32_t length, flags; } fields;
    } header;
    uint32_t next, sockaddr;
    int32_t sockaddr_length;
    uint32_t prefix_origin, suffix_origin, dad_state;
    uint32_t valid_lifetime, preferred_lifetime, lease_lifetime;
    uint8_t on_link_prefix_length;
    uint8_t tail[3];
} iphlp_unicast32_t;

typedef struct {
    union {
        uint64_t alignment;
        struct { uint32_t length, flags; } fields;
    } header;
    uint32_t next, sockaddr;
    int32_t sockaddr_length;
    uint32_t prefix_length;
} iphlp_prefix32_t;

typedef struct {
    union {
        uint64_t alignment;
        struct { uint32_t length, if_index; } fields;
    } header;
    uint32_t next, adapter_name, first_unicast;
    uint32_t first_anycast, first_multicast, first_dns_server;
    uint32_t dns_suffix, description, friendly_name;
    uint8_t physical_address[8];
    uint32_t physical_address_length, flags, mtu, if_type, oper_status;
    uint32_t ipv6_if_index, zone_indices[16];
    uint32_t first_prefix;
    uint8_t tail[232];
} iphlp_adapter32_t;

typedef struct {
    iphlp_adapter32_t adapter;
    iphlp_unicast32_t unicast;
    iphlp_prefix32_t prefix;
    uint8_t unicast_sockaddr[16];
    uint8_t prefix_sockaddr[16];
    char adapter_name[8];
    WCHAR friendly_name[8];
    WCHAR dns_suffix[1];
} iphlp_payload32_t;

_Static_assert(sizeof(iphlp_adapter64_t) == 448, "IP_ADAPTER_ADDRESSES64 layout");
_Static_assert(sizeof(iphlp_unicast64_t) == 64, "IP_ADAPTER_UNICAST_ADDRESS64 layout");
_Static_assert(__builtin_offsetof(iphlp_adapter64_t, friendly_name) == 72,
               "IP_ADAPTER_ADDRESSES64 FriendlyName offset");
_Static_assert(__builtin_offsetof(iphlp_adapter64_t, first_prefix) == 176,
               "IP_ADAPTER_ADDRESSES64 FirstPrefix offset");
_Static_assert(sizeof(iphlp_adapter32_t) == 376, "IP_ADAPTER_ADDRESSES32 layout");
_Static_assert(sizeof(iphlp_unicast32_t) == 48,
               "IP_ADAPTER_UNICAST_ADDRESS32 layout");
_Static_assert(sizeof(iphlp_prefix32_t) == 24, "IP_ADAPTER_PREFIX32 layout");
_Static_assert(sizeof(iphlp_payload32_t) == 512, "GetAdaptersAddresses32 payload");
_Static_assert(__builtin_offsetof(iphlp_adapter32_t, friendly_name) == 40,
               "IP_ADAPTER_ADDRESSES32 FriendlyName offset");
_Static_assert(__builtin_offsetof(iphlp_adapter32_t, first_prefix) == 140,
               "IP_ADAPTER_ADDRESSES32 FirstPrefix offset");

#define IPHLP_ERROR_INVALID_PARAMETER     87U
#define IPHLP_ERROR_BUFFER_OVERFLOW       111U
#define IPHLP_ERROR_INSUFFICIENT_BUFFER  122U
#define IPHLP_ERROR_NOT_SUPPORTED         50U
#define IPHLP_ERROR_NO_DATA              232U

#define IPHLP_GAA_SKIP_UNICAST        0x0001U
#define IPHLP_GAA_INCLUDE_PREFIX      0x0010U
#define IPHLP_GAA_SKIP_FRIENDLY_NAME  0x0020U

#define IPHLP_TCP_TABLE_BASIC_LISTENER          0U
#define IPHLP_TCP_TABLE_BASIC_CONNECTIONS       1U
#define IPHLP_TCP_TABLE_BASIC_ALL               2U
#define IPHLP_TCP_TABLE_OWNER_PID_LISTENER      3U
#define IPHLP_TCP_TABLE_OWNER_PID_CONNECTIONS   4U
#define IPHLP_TCP_TABLE_OWNER_PID_ALL           5U
#define IPHLP_TCP_TABLE_HEADROOM_ROWS           16U

typedef struct {
    DWORD state;
    DWORD local_address;
    DWORD local_port;
    DWORD remote_address;
    DWORD remote_port;
    DWORD owning_pid;
} iphlp_tcp_row_owner_pid_t;

_Static_assert(sizeof(iphlp_tcp_row_owner_pid_t) == 24,
               "MIB_TCPROW_OWNER_PID layout");

static DWORD iphlp_ipv4_address(const uint8_t address[4])
{
    return (DWORD)address[0] | ((DWORD)address[1] << 8) |
           ((DWORD)address[2] << 16) | ((DWORD)address[3] << 24);
}

#define IPHLP_MAX_INTERFACE_NAME_LEN 256U
#define IPHLP_MAXLEN_PHYSADDR          8U
#define IPHLP_MAXLEN_IFDESCR         256U

typedef struct {
    WCHAR wsz_name[IPHLP_MAX_INTERFACE_NAME_LEN];
    DWORD index;
    DWORD type;
    DWORD mtu;
    DWORD speed;
    DWORD physical_address_length;
    BYTE  physical_address[IPHLP_MAXLEN_PHYSADDR];
    DWORD admin_status;
    DWORD oper_status;
    DWORD last_change;
    DWORD in_octets;
    DWORD in_ucast_packets;
    DWORD in_nucast_packets;
    DWORD in_discards;
    DWORD in_errors;
    DWORD in_unknown_protocols;
    DWORD out_octets;
    DWORD out_ucast_packets;
    DWORD out_nucast_packets;
    DWORD out_discards;
    DWORD out_errors;
    DWORD out_queue_length;
    DWORD description_length;
    BYTE  description[IPHLP_MAXLEN_IFDESCR];
} iphlp_if_row_t;

typedef struct {
    DWORD address;
    DWORD index;
    DWORD mask;
    DWORD broadcast_address;
    DWORD reassembly_size;
    USHORT unused;
    USHORT type;
} iphlp_ipaddr_row_t;

typedef struct {
    uint32_t next;
    char ip_address[16];
    char ip_mask[16];
    DWORD context;
} iphlp_ip_addr_string32_t;

typedef struct {
    uint32_t next;
    DWORD combo_index;
    char adapter_name[260];
    char description[132];
    DWORD address_length;
    BYTE address[IPHLP_MAXLEN_PHYSADDR];
    DWORD index;
    DWORD type;
    DWORD dhcp_enabled;
    uint32_t current_ip_address;
    iphlp_ip_addr_string32_t ip_address_list;
    iphlp_ip_addr_string32_t gateway_list;
    iphlp_ip_addr_string32_t dhcp_server;
    BOOL have_wins;
    iphlp_ip_addr_string32_t primary_wins_server;
    iphlp_ip_addr_string32_t secondary_wins_server;
    DWORD lease_obtained;
    DWORD lease_expires;
} iphlp_adapter_info32_t;

_Static_assert(sizeof(iphlp_if_row_t) == 860, "MIB_IFROW layout");
_Static_assert(sizeof(iphlp_ipaddr_row_t) == 24, "MIB_IPADDRROW layout");
_Static_assert(sizeof(iphlp_ip_addr_string32_t) == 40,
               "IP_ADDR_STRING32 layout");
_Static_assert(sizeof(iphlp_adapter_info32_t) == 640,
               "IP_ADAPTER_INFO32 layout");

static void iphlp_copy_ascii(char *destination, DWORD capacity,
                             const char *source)
{
    DWORD i = 0;
    if (!destination || !capacity) return;
    while (source && source[i] && i + 1U < capacity) {
        destination[i] = source[i];
        i++;
    }
    destination[i] = 0;
}

static void iphlp_copy_wide_ascii(WCHAR *destination, DWORD capacity,
                                  const char *source)
{
    DWORD i = 0;
    if (!destination || !capacity) return;
    while (source && source[i] && i + 1U < capacity) {
        destination[i] = (WCHAR)(BYTE)source[i];
        i++;
    }
    destination[i] = 0;
}

static DWORD iphlp_copy_description(BYTE *destination, DWORD capacity,
                                    const char *source)
{
    DWORD length = 0;
    while (source && source[length] && length < capacity)
        length++;
    if (length)
        ws_memcpy(destination, source, length);
    return length;
}

static DWORD WINAPI iphlp_GetIfTable(PVOID table, DWORD *size, BOOL order)
{
    (void)order;
    const DWORD row_count = 2;
    const DWORD required = sizeof(DWORD) +
                           row_count * sizeof(iphlp_if_row_t);
    if (!size)
        return IPHLP_ERROR_INVALID_PARAMETER;
    if (!table || *size < required) {
        *size = required;
        return IPHLP_ERROR_INSUFFICIENT_BUFFER;
    }

    ws_memset(table, 0, required);
    *(DWORD *)table = row_count;
    iphlp_if_row_t *rows =
        (iphlp_if_row_t *)((BYTE *)table + sizeof(DWORD));

    iphlp_if_row_t *ethernet = &rows[0];
    iphlp_copy_wide_ascii(ethernet->wsz_name,
                          IPHLP_MAX_INTERFACE_NAME_LEN,
                          "\\DEVICE\\TCPIP_OSITO0");
    ethernet->index = 1;
    ethernet->type = 6;             /* IF_TYPE_ETHERNET_CSMACD */
    ethernet->mtu = 1500;
    ethernet->speed = 1000000000U;
    ethernet->physical_address_length = 6;
    net_get_mac(ethernet->physical_address);
    ethernet->admin_status = 1;     /* MIB_IF_ADMIN_STATUS_UP */
    ethernet->oper_status = 5;      /* MIB_IF_OPER_STATUS_OPERATIONAL */
    ethernet->description_length = iphlp_copy_description(
        ethernet->description, IPHLP_MAXLEN_IFDESCR, "OsitoK Ethernet");

    iphlp_if_row_t *loopback = &rows[1];
    iphlp_copy_wide_ascii(loopback->wsz_name,
                          IPHLP_MAX_INTERFACE_NAME_LEN,
                          "\\DEVICE\\TCPIP_LOOPBACK");
    loopback->index = 2;
    loopback->type = 24;            /* IF_TYPE_SOFTWARE_LOOPBACK */
    loopback->mtu = 65536;
    loopback->speed = 10000000U;
    loopback->admin_status = 1;
    loopback->oper_status = 5;
    loopback->description_length = iphlp_copy_description(
        loopback->description, IPHLP_MAXLEN_IFDESCR, "OsitoK Loopback");

    *size = required;
    return 0;
}

static DWORD WINAPI iphlp_GetIpAddrTable(PVOID table, DWORD *size, BOOL order)
{
    (void)order;
    const DWORD row_count = 2;
    const DWORD required = sizeof(DWORD) +
                           row_count * sizeof(iphlp_ipaddr_row_t);
    if (!size)
        return IPHLP_ERROR_INVALID_PARAMETER;
    if (!table || *size < required) {
        *size = required;
        return IPHLP_ERROR_INSUFFICIENT_BUFFER;
    }

    ws_memset(table, 0, required);
    *(DWORD *)table = row_count;
    iphlp_ipaddr_row_t *rows =
        (iphlp_ipaddr_row_t *)((BYTE *)table + sizeof(DWORD));

    uint8_t *ip = net_get_ip_ptr();
    uint8_t mask[4] = {255, 255, 255, 0};
    uint8_t broadcast[4] = {ip[0], ip[1], ip[2], 255};
    rows[0].address = iphlp_ipv4_address(ip);
    rows[0].index = 1;
    rows[0].mask = iphlp_ipv4_address(mask);
    rows[0].broadcast_address = iphlp_ipv4_address(broadcast);
    rows[0].reassembly_size = 65535;
    rows[0].type = 0x0005;          /* PRIMARY | DYNAMIC */

    static const uint8_t loopback_ip[4] = {127, 0, 0, 1};
    static const uint8_t loopback_mask[4] = {255, 0, 0, 0};
    static const uint8_t loopback_broadcast[4] = {127, 255, 255, 255};
    rows[1].address = iphlp_ipv4_address(loopback_ip);
    rows[1].index = 2;
    rows[1].mask = iphlp_ipv4_address(loopback_mask);
    rows[1].broadcast_address = iphlp_ipv4_address(loopback_broadcast);
    rows[1].reassembly_size = 65535;
    rows[1].type = 0x0001;          /* PRIMARY */

    *size = required;
    return 0;
}

static DWORD WINAPI iphlp_GetFriendlyIfIndex(DWORD index)
{
    return index == 1U || index == 2U ? index : 0U;
}

static DWORD WINAPI iphlp_GetAdaptersInfo(PVOID info, DWORD *size)
{
    const DWORD required = sizeof(iphlp_adapter_info32_t);
    if (!size)
        return IPHLP_ERROR_INVALID_PARAMETER;

    /* IP_ADAPTER_INFO contains native pointers. The PE32 layout is the one
     * used by the bundled Java runtime; a PE64 caller should use
     * GetAdaptersAddresses until its pointer-sized layout is implemented. */
    if (!g_compat32_mode)
        return IPHLP_ERROR_NOT_SUPPORTED;
    if (!info || *size < required) {
        *size = required;
        return IPHLP_ERROR_INSUFFICIENT_BUFFER;
    }

    iphlp_adapter_info32_t *adapter = (iphlp_adapter_info32_t *)info;
    ws_memset(adapter, 0, sizeof(*adapter));
    iphlp_copy_ascii(adapter->adapter_name, sizeof(adapter->adapter_name),
                     "{OSITOK-ETH0}");
    iphlp_copy_ascii(adapter->description, sizeof(adapter->description),
                     "OsitoK Ethernet");
    adapter->address_length = 6;
    net_get_mac(adapter->address);
    adapter->index = 1;
    adapter->type = 6;             /* MIB_IF_TYPE_ETHERNET */
    adapter->dhcp_enabled = TRUE;

    char address[16];
    char mask[16];
    char gateway[16];
    (void)wsock_format_ipv4(net_get_ip_ptr(), address);
    (void)wsock_format_ipv4(net_get_netmask_ptr(), mask);
    (void)wsock_format_ipv4(net_get_gateway_ptr(), gateway);
    iphlp_copy_ascii(adapter->ip_address_list.ip_address,
                     sizeof(adapter->ip_address_list.ip_address), address);
    iphlp_copy_ascii(adapter->ip_address_list.ip_mask,
                     sizeof(adapter->ip_address_list.ip_mask), mask);
    iphlp_copy_ascii(adapter->gateway_list.ip_address,
                     sizeof(adapter->gateway_list.ip_address), gateway);
    iphlp_copy_ascii(adapter->gateway_list.ip_mask,
                     sizeof(adapter->gateway_list.ip_mask), "0.0.0.0");
    iphlp_copy_ascii(adapter->dhcp_server.ip_address,
                     sizeof(adapter->dhcp_server.ip_address), gateway);
    iphlp_copy_ascii(adapter->dhcp_server.ip_mask,
                     sizeof(adapter->dhcp_server.ip_mask), "255.255.255.255");

    *size = required;
    return 0;
}

static DWORD iphlp_network_port(uint16_t port)
{
    return (DWORD)(((port & 0xFFU) << 8) | (port >> 8));
}

static DWORD iphlp_mib_tcp_state(const win32_socket_t *socket)
{
    if (socket->listening)
        return 2; /* MIB_TCP_STATE_LISTEN */

    switch (net_tcp_state(socket->kern_conn_id)) {
    case KERN_TCP_CLOSED:      return 1;  /* CLOSED */
    case 1:                    return 3;  /* SYN_SENT */
    case KERN_TCP_ESTABLISHED: return 5;  /* ESTAB */
    case 3:                    return 6;  /* FIN_WAIT1 */
    case 4:                    return 7;  /* FIN_WAIT2 */
    case KERN_TCP_CLOSE_WAIT:  return 8;  /* CLOSE_WAIT */
    case KERN_TCP_LAST_ACK:    return 10; /* LAST_ACK */
    case KERN_TCP_TIME_WAIT:   return 11; /* TIME_WAIT */
    case 8:                    return 2;  /* LISTEN */
    case 9:                    return 4;  /* SYN_RCVD */
    default:                   return 1;
    }
}

static BOOL iphlp_tcp_socket_matches(const win32_socket_t *socket,
                                     ULONG table_class)
{
    if (!socket->in_use || socket->type != SOCK_STREAM)
        return FALSE;

    ULONG mode = table_class % 3U;
    if (socket->listening)
        return mode != IPHLP_TCP_TABLE_BASIC_CONNECTIONS;
    if (!socket->connected || socket->kern_conn_id < 0)
        return FALSE;
    return mode != IPHLP_TCP_TABLE_BASIC_LISTENER;
}

static int iphlp_tcp_row_compare(const BYTE *left, const BYTE *right)
{
    const DWORD *a = (const DWORD *)left;
    const DWORD *b = (const DWORD *)right;
    for (int field = 1; field <= 4; field++) {
        if (a[field] < b[field]) return -1;
        if (a[field] > b[field]) return 1;
    }
    return 0;
}

static DWORD WINAPI iphlp_GetExtendedTcpTable(PVOID table, DWORD *size,
                                                BOOL order, ULONG family,
                                                ULONG table_class,
                                                ULONG reserved)
{
    if (!size || reserved)
        return IPHLP_ERROR_INVALID_PARAMETER;
    if (family != AF_INET)
        return IPHLP_ERROR_NOT_SUPPORTED;
    if (table_class > IPHLP_TCP_TABLE_OWNER_PID_ALL)
        return IPHLP_ERROR_INVALID_PARAMETER;

    DWORD row_size = table_class >= IPHLP_TCP_TABLE_OWNER_PID_LISTENER
                   ? sizeof(iphlp_tcp_row_owner_pid_t)
                   : sizeof(iphlp_tcp_row_owner_pid_t) - sizeof(DWORD);
    DWORD count = 0;

    wsock_process_states_acquire();
    for (int process = 0; process < MAX_WSOCK_PROCESS_STATES; process++) {
        wsock_process_state_t *state = &g_wsock_process_states[process];
        if (!state->used) continue;
        for (int socket = 0; socket < MAX_WIN32_SOCKETS; socket++)
            if (iphlp_tcp_socket_matches(&state->sockets[socket], table_class))
                count++;
    }

    DWORD required = sizeof(DWORD) + count * row_size;
    if (!table || *size < required) {
        DWORD advertised = required;
        DWORD headroom = IPHLP_TCP_TABLE_HEADROOM_ROWS * row_size;
        if (advertised <= 0xFFFFFFFFU - headroom)
            advertised += headroom;
        wsock_process_states_release();
        serial_puts("[IPHLP] GetExtendedTcpTable resize class=");
        serial_putdec(table_class);
        serial_puts(" entries=");
        serial_putdec(count);
        serial_puts(" capacity=");
        serial_putdec(*size);
        serial_puts(" advertised=");
        serial_putdec(advertised);
        serial_puts("\n");
        *size = advertised;
        return IPHLP_ERROR_INSUFFICIENT_BUFFER;
    }

    ws_memset(table, 0, required);
    *(DWORD *)table = count;
    BYTE *cursor = (BYTE *)table + sizeof(DWORD);
    uint8_t *host_ip = net_get_ip_ptr();
    for (int process = 0; process < MAX_WSOCK_PROCESS_STATES; process++) {
        wsock_process_state_t *state = &g_wsock_process_states[process];
        if (!state->used) continue;
        for (int socket_index = 0; socket_index < MAX_WIN32_SOCKETS;
             socket_index++) {
            win32_socket_t *socket = &state->sockets[socket_index];
            if (!iphlp_tcp_socket_matches(socket, table_class)) continue;

            iphlp_tcp_row_owner_pid_t *row =
                (iphlp_tcp_row_owner_pid_t *)cursor;
            const uint8_t *local_ip = socket->local_ip;
            BOOL local_unspecified = local_ip[0] == 0 && local_ip[1] == 0 &&
                                     local_ip[2] == 0 && local_ip[3] == 0;
            if (local_unspecified && !socket->listening) {
                if (socket->remote_ip[0] == 127) {
                    static const uint8_t loopback[4] = {127, 0, 0, 1};
                    local_ip = loopback;
                } else {
                    local_ip = host_ip;
                }
            }

            row->state = iphlp_mib_tcp_state(socket);
            row->local_address = iphlp_ipv4_address(local_ip);
            row->local_port = iphlp_network_port(socket->local_port);
            if (!socket->listening) {
                row->remote_address = iphlp_ipv4_address(socket->remote_ip);
                row->remote_port = iphlp_network_port(socket->remote_port);
            }
            if (row_size == sizeof(*row))
                row->owning_pid = state->pid;
            cursor += row_size;
        }
    }
    wsock_process_states_release();

    if (order && count > 1) {
        BYTE temporary[sizeof(iphlp_tcp_row_owner_pid_t)];
        BYTE *rows = (BYTE *)table + sizeof(DWORD);
        for (DWORD i = 1; i < count; i++) {
            BYTE *current = rows + i * row_size;
            ws_memcpy(temporary, current, row_size);
            DWORD j = i;
            while (j && iphlp_tcp_row_compare(
                       rows + (j - 1U) * row_size, temporary) > 0) {
                ws_memcpy(rows + j * row_size,
                          rows + (j - 1U) * row_size, row_size);
                j--;
            }
            ws_memcpy(rows + j * row_size, temporary, row_size);
        }
    }

    *size = required;
    serial_puts("[IPHLP] GetExtendedTcpTable class=");
    serial_putdec(table_class);
    serial_puts(" entries=");
    serial_putdec(count);
    serial_puts("\n");
    return 0;
}

static uint8_t iphlp_prefix_length(const uint8_t mask[4])
{
    uint8_t length = 0;
    for (int octet = 0; octet < 4; octet++) {
        uint8_t bit = 0x80;
        while (bit && (mask[octet] & bit)) {
            length++;
            bit >>= 1;
        }
        if (bit) break;
    }
    return length;
}

static ULONG WINAPI iphlp_GetAdaptersAddresses(ULONG family, ULONG flags,
                                                PVOID reserved, PVOID addresses,
                                                ULONG *size)
{
    if (!size || reserved)
        return IPHLP_ERROR_INVALID_PARAMETER;
    if (family != 0 && family != 2) {
        *size = 0;
        return IPHLP_ERROR_NO_DATA;
    }

    ULONG required = g_compat32_mode ? sizeof(iphlp_payload32_t)
                                     : sizeof(iphlp_payload64_t);
    if (!addresses || *size < required) {
        *size = required;
        return IPHLP_ERROR_BUFFER_OVERFLOW;
    }

    uint8_t *ip = net_get_ip_ptr();
    uint8_t *mask = net_get_netmask_ptr();
    uint8_t prefix_length = iphlp_prefix_length(mask);
    BOOL have_unicast = !(flags & IPHLP_GAA_SKIP_UNICAST) &&
                        (ip[0] || ip[1] || ip[2] || ip[3]);
    BOOL have_prefix = have_unicast &&
                       (flags & IPHLP_GAA_INCLUDE_PREFIX);

    if (g_compat32_mode) {
        iphlp_payload32_t *p = (iphlp_payload32_t *)addresses;
        ws_memset(p, 0, sizeof(*p));

        p->adapter.header.fields.length = sizeof(p->adapter);
        p->adapter.header.fields.if_index = 1;
        p->adapter.adapter_name = (uint32_t)(uintptr_t)p->adapter_name;
        p->adapter.first_unicast = have_unicast
            ? (uint32_t)(uintptr_t)&p->unicast : 0;
        p->adapter.dns_suffix = (uint32_t)(uintptr_t)p->dns_suffix;
        p->adapter.description = (uint32_t)(uintptr_t)p->friendly_name;
        if (!(flags & IPHLP_GAA_SKIP_FRIENDLY_NAME))
            p->adapter.friendly_name = (uint32_t)(uintptr_t)p->friendly_name;
        p->adapter.physical_address_length = 6;
        net_get_mac(p->adapter.physical_address);
        p->adapter.flags = 0x84; /* DHCPv4 + IPv4 enabled */
        p->adapter.mtu = 1500;
        p->adapter.if_type = 6; /* IF_TYPE_ETHERNET_CSMACD */
        p->adapter.oper_status = 1; /* IfOperStatusUp */
        p->adapter.first_prefix = have_prefix
            ? (uint32_t)(uintptr_t)&p->prefix : 0;
        iphlp_copy_ascii(p->adapter_name, sizeof(p->adapter_name), "osito0");
        iphlp_copy_wide_ascii(p->friendly_name,
                              sizeof(p->friendly_name) / sizeof(WCHAR),
                              "osito0");

        p->unicast.header.fields.length = sizeof(p->unicast);
        p->unicast.header.fields.flags = 1; /* DNS eligible */
        p->unicast.sockaddr = (uint32_t)(uintptr_t)p->unicast_sockaddr;
        p->unicast.sockaddr_length = sizeof(p->unicast_sockaddr);
        p->unicast.prefix_origin = 3; /* IpPrefixOriginDhcp */
        p->unicast.suffix_origin = 3; /* IpSuffixOriginDhcp */
        p->unicast.dad_state = 4; /* IpDadStatePreferred */
        p->unicast.valid_lifetime = 0xFFFFFFFFU;
        p->unicast.preferred_lifetime = 0xFFFFFFFFU;
        p->unicast.lease_lifetime = 0xFFFFFFFFU;
        p->unicast.on_link_prefix_length = prefix_length;
        p->unicast_sockaddr[0] = 2; /* AF_INET */
        ws_memcpy(p->unicast_sockaddr + 4, ip, 4);

        p->prefix.header.fields.length = sizeof(p->prefix);
        p->prefix.sockaddr = (uint32_t)(uintptr_t)p->prefix_sockaddr;
        p->prefix.sockaddr_length = sizeof(p->prefix_sockaddr);
        p->prefix.prefix_length = prefix_length;
        p->prefix_sockaddr[0] = 2;
        for (int octet = 0; octet < 4; octet++)
            p->prefix_sockaddr[4 + octet] = ip[octet] & mask[octet];
    } else {
        iphlp_payload64_t *p = (iphlp_payload64_t *)addresses;
        ws_memset(p, 0, sizeof(*p));

        p->adapter.length = sizeof(p->adapter);
        p->adapter.if_index = 1;
        p->adapter.adapter_name = (uint64_t)(uintptr_t)p->adapter_name;
        p->adapter.first_unicast = have_unicast
            ? (uint64_t)(uintptr_t)&p->unicast : 0;
        p->adapter.dns_suffix = (uint64_t)(uintptr_t)p->dns_suffix;
        p->adapter.description = (uint64_t)(uintptr_t)p->friendly_name;
        if (!(flags & IPHLP_GAA_SKIP_FRIENDLY_NAME))
            p->adapter.friendly_name = (uint64_t)(uintptr_t)p->friendly_name;
        p->adapter.physical_address_length = 6;
        net_get_mac(p->adapter.physical_address);
        p->adapter.flags = 0x84; /* DHCPv4 + IPv4 enabled */
        p->adapter.mtu = 1500;
        p->adapter.if_type = 6; /* IF_TYPE_ETHERNET_CSMACD */
        p->adapter.oper_status = 1; /* IfOperStatusUp */
        p->adapter.first_prefix = have_prefix
            ? (uint64_t)(uintptr_t)&p->prefix : 0;
        iphlp_copy_ascii(p->adapter_name, sizeof(p->adapter_name), "osito0");
        iphlp_copy_wide_ascii(p->friendly_name,
                              sizeof(p->friendly_name) / sizeof(WCHAR),
                              "osito0");

        p->unicast.length = sizeof(p->unicast);
        p->unicast.flags = 1; /* DNS eligible */
        p->unicast.sockaddr = (uint64_t)(uintptr_t)p->unicast_sockaddr;
        p->unicast.sockaddr_length = sizeof(p->unicast_sockaddr);
        p->unicast.prefix_origin = 3; /* IpPrefixOriginDhcp */
        p->unicast.suffix_origin = 3; /* IpSuffixOriginDhcp */
        p->unicast.dad_state = 4; /* IpDadStatePreferred */
        p->unicast.valid_lifetime = 0xFFFFFFFFU;
        p->unicast.preferred_lifetime = 0xFFFFFFFFU;
        p->unicast.lease_lifetime = 0xFFFFFFFFU;
        p->unicast.on_link_prefix_length = prefix_length;
        p->unicast_sockaddr[0] = 2; /* AF_INET */
        ws_memcpy(p->unicast_sockaddr + 4, ip, 4);

        p->prefix.length = sizeof(p->prefix);
        p->prefix.sockaddr = (uint64_t)(uintptr_t)p->prefix_sockaddr;
        p->prefix.sockaddr_length = sizeof(p->prefix_sockaddr);
        p->prefix.prefix_length = prefix_length;
        p->prefix_sockaddr[0] = 2;
        for (int octet = 0; octet < 4; octet++)
            p->prefix_sockaddr[4 + octet] = ip[octet] & mask[octet];
    }

    *size = required;
    serial_puts(g_compat32_mode
        ? "[IPHLP] GetAdaptersAddresses PE32: 1 IPv4 adapter\n"
        : "[IPHLP] GetAdaptersAddresses PE64: 1 IPv4 adapter\n");
    return 0;
}

static DWORD WINAPI iphlp_NotifyAddrChange(PVOID handle_out, PVOID overlapped)
{
    if (!handle_out || !overlapped)
        return 87; /* ERROR_INVALID_PARAMETER */
    if (g_compat32_mode)
        *(uint32_t *)handle_out = 0;
    else
        *(uint64_t *)handle_out = 0;
    /* ponytail: add a pending wait only when DHCP can publish address changes. */
    return 50; /* ERROR_NOT_SUPPORTED */
}

static BOOL WINAPI wininet_InternetCrackUrlW(PCWSTR url, DWORD length,
                                              DWORD flags, PVOID components)
{
    (void)url; (void)length; (void)flags; (void)components;
    return FALSE;
}

typedef struct {
    BOOL auto_detect;
    PWSTR auto_config_url;
    PWSTR proxy;
    PWSTR proxy_bypass;
} WINHTTP_CURRENT_USER_IE_PROXY_CONFIG_U32;

static BOOL WINAPI winhttp_GetIEProxyConfigForCurrentUser(
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG_U32 *config)
{
    if (!config) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    ws_memset(config, 0, g_compat32_mode ? 16 : sizeof(*config));
    return TRUE; /* Valid direct-connection configuration. */
}

static const SHIM_EXPORT wsock_exports[] = {
    { "AcceptEx",             (PVOID)wsock_AcceptEx,             8, CC_STDCALL },
    { "GetAcceptExSockaddrs", (PVOID)wsock_GetAcceptExSockaddrs, 8, CC_STDCALL },
    { "GetExtendedTcpTable",  (PVOID)iphlp_GetExtendedTcpTable,  6, CC_STDCALL },
    { "GetIfTable",           (PVOID)iphlp_GetIfTable,           3, CC_STDCALL },
    { "GetIpAddrTable",       (PVOID)iphlp_GetIpAddrTable,       3, CC_STDCALL },
    { "GetFriendlyIfIndex",   (PVOID)iphlp_GetFriendlyIfIndex,   1, CC_STDCALL },
    { "GetAdaptersInfo",      (PVOID)iphlp_GetAdaptersInfo,      2, CC_STDCALL },
    { "GetAdaptersAddresses", (PVOID)iphlp_GetAdaptersAddresses, 5, CC_STDCALL },
    { "NotifyAddrChange",     (PVOID)iphlp_NotifyAddrChange,     2, CC_STDCALL },
    { "InternetCrackUrlW",    (PVOID)wininet_InternetCrackUrlW,  4, CC_STDCALL },
    { "WinHttpGetIEProxyConfigForCurrentUser",
      (PVOID)winhttp_GetIEProxyConfigForCurrentUser, 1, CC_STDCALL },
    { "WSAStartup",     (PVOID)WSAStartup,         2, CC_STDCALL },
    { "WSACleanup",     (PVOID)WSACleanup,         0, CC_STDCALL },
    { "WSAGetLastError",(PVOID)WSAGetLastError,    0, CC_STDCALL },
    { "WSASetLastError",(PVOID)WSASetLastError,    1, CC_STDCALL },
    { "WSACreateEvent", (PVOID)wsock_WSACreateEvent, 0, CC_STDCALL },
    { "WSACloseEvent",  (PVOID)wsock_WSACloseEvent,  1, CC_STDCALL },
    { "WSASetEvent",    (PVOID)wsock_WSASetEvent,    1, CC_STDCALL },
    { "WSAResetEvent",  (PVOID)wsock_WSAResetEvent,  1, CC_STDCALL },
    { "WSAEventSelect", (PVOID)wsock_WSAEventSelect, 3, CC_STDCALL },
    { "WSAEnumNetworkEvents", (PVOID)wsock_WSAEnumNetworkEvents,
                                                       3, CC_STDCALL },
    { "WSAWaitForMultipleEvents", (PVOID)wsock_WSAWaitForMultipleEvents,
                                                       5, CC_STDCALL },
    { "WSAEnumProtocolsW", (PVOID)wsock_WSAEnumProtocolsW,
                                                       3, CC_STDCALL },
    { "WSCEnumProtocols", (PVOID)wsock_WSCEnumProtocols,
                                                       4, CC_STDCALL },
    { "WSCGetProviderPath", (PVOID)wsock_WSCGetProviderPath,
                                                       4, CC_STDCALL },
    { "WSAEnumNameSpaceProvidersW",
      (PVOID)wsock_WSAEnumNameSpaceProvidersW,         2, CC_STDCALL },
    { "WSALookupServiceBeginW", (PVOID)wsock_WSALookupServiceBeginW,
                                                       3, CC_STDCALL },
    { "WSALookupServiceNextW", (PVOID)wsock_WSALookupServiceNextW,
                                                       4, CC_STDCALL },
    { "WSALookupServiceEnd", (PVOID)wsock_WSALookupServiceEnd,
                                                       1, CC_STDCALL },
    { "WSASetServiceW", (PVOID)wsock_WSASetServiceW, 3, CC_STDCALL },
    { "WSAIoctl",       (PVOID)WSAIoctl,           9, CC_STDCALL },
    { "WSADuplicateSocketW", (PVOID)wsock_WSADuplicateSocketW,
                                                       3, CC_STDCALL },
    { "WSASocketA",     (PVOID)wsock_WSASocketA,   6, CC_STDCALL },
    { "WSASocketW",     (PVOID)wsock_WSASocketA,   6, CC_STDCALL },
    { "WSASend",        (PVOID)wsock_WSASend,      7, CC_STDCALL },
    { "WSASendDisconnect", (PVOID)wsock_WSASendDisconnect,
                                                       2, CC_STDCALL },
    { "WSASendTo",      (PVOID)wsock_WSASendTo,    9, CC_STDCALL },
    { "WSASendMsg",     (PVOID)wsock_WSASendMsg,   6, CC_STDCALL },
    { "WSARecv",        (PVOID)wsock_WSARecv,      7, CC_STDCALL },
    { "WSARecvDisconnect", (PVOID)wsock_WSARecvDisconnect,
                                                       2, CC_STDCALL },
    { "WSARecvFrom",    (PVOID)wsock_WSARecvFrom,  9, CC_STDCALL },
    { "WSARecvMsg",     (PVOID)wsock_WSARecvMsg,   5, CC_STDCALL },
    { "WSAGetOverlappedResult", (PVOID)wsock_WSAGetOverlappedResult,
                                                       5, CC_STDCALL },
    { "getaddrinfo",    (PVOID)wsock_getaddrinfo,  4, CC_STDCALL },
    { "freeaddrinfo",   (PVOID)wsock_freeaddrinfo, 1, CC_STDCALL },
    { "gethostname",    (PVOID)wsock_gethostname,  2, CC_STDCALL },
    { "getnameinfo",    (PVOID)wsock_getnameinfo,  7, CC_STDCALL },
    { "GetNameInfoA",   (PVOID)wsock_getnameinfo,  7, CC_STDCALL },
    { "inet_ntop",      (PVOID)wsock_inet_ntop,    4, CC_STDCALL },
    { "InetNtopA",      (PVOID)wsock_inet_ntop,    4, CC_STDCALL },
    { "ioctlsocket",    (PVOID)wsock_ioctlsocket,  3, CC_STDCALL },
    { "__WSAFDIsSet",   (PVOID)wsock_fd_is_set,    2, CC_STDCALL },
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
    { "gethostbyaddr",  (PVOID)wsock_gethostbyaddr,3, CC_STDCALL },
    { "gethostbyname",  (PVOID)wsock_gethostbyname,1, CC_STDCALL },
    { "getprotobyname", (PVOID)wsock_getprotobyname, 1, CC_STDCALL },
    { "getprotobynumber", (PVOID)wsock_getprotobynumber,
                                                      1, CC_STDCALL },
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
    {   3, (PVOID)closesocket        },  /* closesocket  */
    {   4, (PVOID)wsock_connect      },  /* connect      */
    {   5, (PVOID)wsock_getpeername  },  /* getpeername  */
    {   6, (PVOID)wsock_getsockname  },  /* getsockname  */
    {   7, (PVOID)wsock_getsockopt   },  /* getsockopt   */
    {   8, (PVOID)htonl              },  /* htonl        */
    {   9, (PVOID)htons              },  /* htons        */
    {  10, (PVOID)wsock_inet_addr    },  /* inet_addr    */
    {  11, (PVOID)wsock_inet_ntoa    },  /* inet_ntoa    */
    {  12, (PVOID)wsock_ioctlsocket  },  /* ioctlsocket  */
    {  13, (PVOID)wsock_listen       },  /* listen       */
    {  14, (PVOID)ntohl              },  /* ntohl        */
    {  15, (PVOID)ntohs              },  /* ntohs        */
    {  16, (PVOID)wsock_recv         },  /* recv         */
    {  17, (PVOID)wsock_recvfrom     },  /* recvfrom     */
    {  18, (PVOID)wsock_select       },  /* select       */
    {  19, (PVOID)wsock_send         },  /* send         */
    {  20, (PVOID)wsock_sendto       },  /* sendto       */
    {  21, (PVOID)wsock_setsockopt   },  /* setsockopt   */
    {  22, (PVOID)wsock_shutdown     },  /* shutdown     */
    {  23, (PVOID)wsock_socket       },  /* socket       */
    {  51, (PVOID)wsock_gethostbyaddr},  /* gethostbyaddr */
    {  52, (PVOID)wsock_gethostbyname},  /* gethostbyname */
    {  53, (PVOID)wsock_getprotobyname}, /* getprotobyname */
    {  54, (PVOID)wsock_getprotobynumber}, /* getprotobynumber */
    {  57, (PVOID)wsock_gethostname   },  /* gethostname */
    { 111, (PVOID)WSAGetLastError     },  /* WSAGetLastError */
    { 115, (PVOID)WSAStartup          },  /* WSAStartup   */
    { 116, (PVOID)WSACleanup          },  /* WSACleanup   */
    { 151, (PVOID)wsock_fd_is_set     },  /* __WSAFDIsSet */
    {1141, (PVOID)wsock_AcceptEx     },  /* AcceptEx     */
    {1142, (PVOID)wsock_GetAcceptExSockaddrs}, /* GetAcceptExSockaddrs */
    {   0, NULL }
};

static const SHIM_ORDINAL ws2_ordinals[] = {
    {   1, (PVOID)wsock_accept        },
    {   2, (PVOID)wsock_bind          },
    {   3, (PVOID)closesocket         },
    {   4, (PVOID)wsock_connect       },
    {   5, (PVOID)wsock_getpeername   },
    {   6, (PVOID)wsock_getsockname   },
    {   7, (PVOID)wsock_getsockopt    },
    {   8, (PVOID)htonl               },
    {   9, (PVOID)htons               },
    {  10, (PVOID)wsock_ioctlsocket   },
    {  11, (PVOID)wsock_inet_addr     },
    {  12, (PVOID)wsock_inet_ntoa     },
    {  13, (PVOID)wsock_listen        },
    {  14, (PVOID)ntohl               },
    {  15, (PVOID)ntohs               },
    {  16, (PVOID)wsock_recv          },
    {  17, (PVOID)wsock_recvfrom      },
    {  18, (PVOID)wsock_select        },
    {  19, (PVOID)wsock_send          },
    {  20, (PVOID)wsock_sendto        },
    {  21, (PVOID)wsock_setsockopt    },
    {  22, (PVOID)wsock_shutdown      },
    {  23, (PVOID)wsock_socket        },
    {  51, (PVOID)wsock_gethostbyaddr },
    {  52, (PVOID)wsock_gethostbyname },
    {  53, (PVOID)wsock_getprotobyname},
    {  54, (PVOID)wsock_getprotobynumber},
    {  57, (PVOID)wsock_gethostname   },
    { 111, (PVOID)WSAGetLastError     },
    { 112, (PVOID)WSASetLastError     },
    { 115, (PVOID)WSAStartup          },
    { 116, (PVOID)WSACleanup          },
    { 151, (PVOID)wsock_fd_is_set     },
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

PVOID ws2_32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) {
        for (int i = 0; ws2_ordinals[i].func; i++) {
            if (ws2_ordinals[i].ordinal == ordinal)
                return ws2_ordinals[i].func;
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
