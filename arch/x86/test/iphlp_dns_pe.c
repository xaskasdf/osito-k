/* Public IP Helper layout/ABI fixture, also runnable on native Windows. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef unsigned char BYTE;
#ifdef _WIN64
typedef unsigned long long UINT_PTR;
#else
typedef unsigned long UINT_PTR;
#endif
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
API char *CALL GetCommandLineA(void);
API void CALL ExitProcess(DWORD);

typedef union {
    unsigned long long alignment;
    struct { DWORD length, reserved; } fields;
} HEADER;
typedef struct DNS_ADDRESS {
    HEADER header;
    struct DNS_ADDRESS *next;
    struct { BYTE *address; int length; } socket;
} DNS_ADDRESS;
typedef struct ADAPTER {
    HEADER header;
    struct ADAPTER *next;
    char *name;
    void *unicast, *anycast, *multicast;
    DNS_ADDRESS *dns;
    WCHAR *suffix, *description, *friendly_name;
    BYTE physical_address[8];
    DWORD physical_length, flags, mtu, type, status;
} ADAPTER;
typedef DWORD (CALL *query_fn)(DWORD, DWORD, void *, void *, DWORD *);

#ifdef _WIN64
_Static_assert(sizeof(DNS_ADDRESS) == 32, "DNS64 size");
_Static_assert(__builtin_offsetof(ADAPTER, dns) == 48, "DNS64 adapter offset");
#else
_Static_assert(sizeof(DNS_ADDRESS) == 24, "DNS32 size");
_Static_assert(__builtin_offsetof(ADAPTER, dns) == 28, "DNS32 adapter offset");
#endif

static union { unsigned long long alignment; BYTE bytes[65552]; } buffer;
static DWORD failures;
static void report(const char *s)
{
    DWORD count = 0, written;
    while (s[count]) count++;
    WriteFile(GetStdHandle((DWORD)-11), s, count, &written, 0);
}
static void check(int value, const char *name)
{
    if (!value) { failures++; report("IPHLP-DNS FAIL "); report(name); report("\n"); }
}
static int inside(const void *p, DWORD length, DWORD capacity)
{
    UINT_PTR offset = (UINT_PTR)p - (UINT_PTR)buffer.bytes;
    return offset <= capacity && length <= capacity - offset;
}
static void fill(void)
{
    for (unsigned i = 0; i < sizeof(buffer.bytes); i++) buffer.bytes[i] = 0xA5;
}
static int unchanged(unsigned begin, unsigned end)
{
    for (unsigned i = begin; i < end; i++)
        if (buffer.bytes[i] != 0xA5) return 0;
    return 1;
}
static int expected_ip(BYTE ip[4])
{
    const char *s = GetCommandLineA();
    if (*s == '"') {
        s++;
        while (*s && *s != '"') s++;
        if (*s) s++;
    } else {
        while (*s && *s != ' ') s++;
    }
    while (*s == ' ') s++;
    if (!*s) return 0;
    for (unsigned n = 0; n < 4; n++) {
        unsigned value = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            value = value * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || value > 255) return -1;
        }
        if (!digits || (n < 3 && *s++ != '.')) return -1;
        ip[n] = (BYTE)value;
    }
    return *s ? -1 : 1;
}

void mainCRTStartup(void)
{
    void *module = LoadLibraryA("iphlpapi.dll");
    query_fn query = GetProcAddress(module, "GetAdaptersAddresses");
    if (!query) { report("IPHLP-DNS MISSING EXPORT\n"); ExitProcess(2); }
    BYTE expected[4] = {0};
    int supplied = expected_ip(expected);
    if (supplied < 0) { report("IPHLP-DNS BAD ARGUMENT\n"); ExitProcess(2); }
    int want_dns = !supplied || (expected[0] | expected[1] | expected[2] | expected[3]);
    /* 0x27 is Chromium's DNS-only query, independent of unicast enumeration. */
    static const DWORD options[] = {0, 0x27, 8, 9, 0x10, 0x38};
    for (unsigned family = 0; family <= 2; family += 2) {
        for (unsigned test = 0; test < sizeof(options) / sizeof(*options); test++) {
            DWORD flags = options[test];
            struct { DWORD value, canary; } size = {0, 0x12345678};
            DWORD rc = query(family, flags, 0, 0, &size.value);
            check(rc == 111 && size.value >= sizeof(ADAPTER), "size probe");
            check(size.canary == 0x12345678, "ULONG width");
            if (rc != 111 || size.value > 65536 || size.value < sizeof(ADAPTER)) continue;
            DWORD capacity = size.value;
            fill();
            size.value = 0;
            rc = query(family, flags, 0, buffer.bytes, &size.value);
            check(rc == 111 && unchanged(0, sizeof(buffer.bytes)), "short buffer write");
            size.value = capacity;
            rc = query(family, flags, 0, buffer.bytes, &size.value);
            check(rc == 0, "query");
            check(unchanged(capacity, sizeof(buffer.bytes)), "buffer overrun");
            check(size.canary == 0x12345678, "ULONG overrun");
            if (rc) continue;
            ADAPTER *adapter = (ADAPTER *)buffer.bytes;
            unsigned adapters = 0, servers = 0, matches = 0;
            for (; adapter && adapters < 256; adapter = adapter->next, adapters++) {
                if (!inside(adapter, sizeof(*adapter), capacity)) {
                    check(0, "adapter pointer"); break;
                }
                if (flags & 1) check(!adapter->unicast, "skip unicast");
                if (flags & 8) check(!adapter->dns, "skip DNS");
                DNS_ADDRESS *dns = adapter->dns;
                unsigned nodes = 0;
                for (; dns && nodes < 256; dns = dns->next, nodes++) {
                    if (!inside(dns, sizeof(*dns), capacity)) {
                        check(0, "DNS pointer"); break;
                    }
                    check(dns->header.fields.length == sizeof(*dns), "DNS node size");
                    check(dns->header.fields.reserved == 0, "DNS reserved");
                    if (dns->socket.length < 2 ||
                        !inside(dns->socket.address, (DWORD)dns->socket.length, capacity)) {
                        check(0, "sockaddr pointer"); break;
                    }
                    BYTE *address = dns->socket.address;
                    unsigned af = address[0] | ((unsigned)address[1] << 8);
                    check((af == 2 && dns->socket.length == 16) ||
                          (af == 23 && dns->socket.length == 28), "sockaddr layout");
                    if (adapter->status == 1 && adapter->type != 24) {
                        servers++;
                        if (af == 2 && dns->socket.length == 16 &&
                            address[4] == expected[0] && address[5] == expected[1] &&
                            address[6] == expected[2] && address[7] == expected[3]) matches++;
                    }
                }
                check(!dns, "DNS list terminates");
            }
            check(!adapter, "adapter list terminates");
            if (!(flags & 8)) {
                check(want_dns ? servers > 0 : servers == 0, "configured DNS list");
                if (supplied && want_dns) check(matches > 0, "configured DNS value");
            }
        }
    }
    report(failures ? "IPHLP-DNS FAIL\n" : "IPHLP-DNS PASS\n");
    ExitProcess(failures ? 1 : 0);
}
