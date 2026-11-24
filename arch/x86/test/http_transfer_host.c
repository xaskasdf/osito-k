/* Fragmented transport tests: exercise the real HTTP parser/body reader. */
#include "../kernel/http.c"
extern int printf(const char *, ...);
static unsigned checks, failures, transport, fragment, send_fragment;
static unsigned wire_size, wire_pos, sent_size, body_size, calls_tls, calls_plain;
static unsigned connects, closes, abort_body, timeout_eof;
static uint16_t last_port;
static const char *wire;
static char sent[4096], body[128];
#define CHECK(x) do { checks++; if (!(x)) { printf("HTTP FAIL line=%d mode=%u fragment=%u: %s\n", __LINE__, transport, fragment, #x); failures++; } } while (0)
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
static int http_transfer_recv(void *out, uint32_t capacity)
{
    if (wire_pos == wire_size) return timeout_eof ? 0 : -1;
    unsigned amount = wire_size - wire_pos;
    if (amount > capacity) amount = capacity;
    if (amount > fragment) amount = fragment;
    memcpy(out, wire + wire_pos, amount); wire_pos += amount;
    return (int)amount;
}
static int http_transfer_send(const void *data, uint32_t length)
{
    unsigned amount = length;
    if (!transport && amount > send_fragment) amount = send_fragment;
    CHECK(amount <= sizeof(sent) - sent_size);
    if (amount > sizeof(sent) - sent_size) return -1;
    memcpy(sent + sent_size, data, amount); sent_size += amount;
    return (int)amount;
}
int net_tcp_send(int fd, const void *data, uint32_t length)
{ CHECK(fd == 7); calls_plain++; return http_transfer_send(data, length); }
int net_tcp_recv_timeout(int fd, void *out, uint32_t capacity, uint32_t timeout)
{ (void)timeout; CHECK(fd == 7); calls_plain++; return http_transfer_recv(out, capacity); }
int net_tcp_state(int fd)
{ CHECK(fd == 7); return wire_pos < wire_size || timeout_eof ? TCP_STATE_ESTABLISHED : 0; }
int net_dns_resolve(const char *host, uint8_t ip[4])
{ CHECK(!strcmp(host,"fixture.test")); memset(ip, 1, 4); return 0; }
int net_tcp_connect(const uint8_t ip[4], uint16_t port, uint16_t source)
{ (void)ip; (void)source; last_port = port; connects++; return 7; }
void net_tcp_close(int fd) { CHECK(fd == 7); closes++; }
int tls13_connect(int fd, const char *host)
{ (void)host; CHECK(fd == 7); calls_tls++; return transport == 1 ? 0 : -1; }
int tls_connect(tls_conn_t *s, int fd, const char *host)
{ (void)s; (void)host; CHECK(fd == 7); calls_tls++; return 0; }
void tls_close(tls_conn_t *s) { (void)s; calls_tls++; }
void tls13_close(void) { calls_tls++; }
int tls13_send(const void *data, uint32_t length)
{ calls_tls++; return http_transfer_send(data, length); }
int tls13_recv(void *out, uint32_t capacity)
{ calls_tls++; return http_transfer_recv(out, capacity); }
int tls_send(tls_conn_t *s, const void *data, uint32_t length)
{ (void)s; calls_tls++; return http_transfer_send(data, length); }
int tls_recv(tls_conn_t *s, void *out, uint32_t capacity, uint32_t timeout)
{ (void)s; (void)timeout; calls_tls++; return http_transfer_recv(out, capacity); }
int net_dns_resolve_async(const char *host, uint8_t *ip, void (*done)(int, void *), void *arg)
{ (void)host; (void)ip; (void)done; (void)arg; return -1; }
int net_tcp_connect_async(const uint8_t *ip, uint16_t port, uint16_t source,
                          void (*done)(int, void *), void *arg)
{ (void)ip; (void)port; (void)source; (void)done; (void)arg; return -1; }
static int http_transfer_body(const void *data, uint32_t length, void *context)
{
    CHECK(context == body);
    if (abort_body) return -1;
    CHECK(length <= sizeof(body) - body_size);
    if (length > sizeof(body) - body_size) return -1;
    memcpy(body + body_size, data, length); body_size += length; return 0;
}
static void http_transfer_case(const char *response, const char *expected, bool header_failure)
{
    static http_session_t session;
    http_response_t parsed;
    CHECK(!http_owner);
    wire = response; wire_size = (unsigned)strlen(wire);
    wire_pos = sent_size = body_size = calls_tls = calls_plain = connects = closes = 0;
    CHECK(http_open_port(&session, "fixture.test", 18765, transport != 0) == 0);
    CHECK(session.connected && session.plaintext == (transport == 0));
    CHECK(last_port == 18765 && connects == (transport == 2 ? 2U : 1U));
    int rc = http_request(&session, "GET", "/runtime.cab", "fixture.test:18765", 0, 0, 0, &parsed);
    CHECK(sent_size >= 4 && !memcmp(sent + sent_size - 4, "\r\n\r\n", 4));
    if (header_failure) CHECK(rc < 0);
    else {
        CHECK(rc == 0 && parsed.status_code == 200);
        if (!rc) {
            rc = http_read_body(&session, &parsed, http_transfer_body, body);
            if (!expected) CHECK(rc < 0);
            else CHECK(rc == (int)strlen(expected) && body_size == strlen(expected) &&
                       !memcmp(body, expected, body_size));
        }
    }
    http_close(&session);
    CHECK(!http_owner && !session.connected);
    CHECK(transport ? calls_tls && !calls_plain : !calls_tls && calls_plain);
    CHECK(closes == connects);
}
int main(void)
{
    uint8_t ip[4] = {9,9,9,9};
    CHECK(http_numeric_host("10.0.2.2",ip) && ip[0]==10 && ip[1]==0 && ip[2]==2 && ip[3]==2);
    CHECK(http_numeric_host("255.255.255.255",ip) && ip[3]==255);
    CHECK(!http_numeric_host("256.0.2.2",ip));
    CHECK(!http_numeric_host("1.2.3",ip));
    CHECK(!http_numeric_host("1.2.3.4.5",ip));
    CHECK(!http_numeric_host("1.2.3.4x",ip));
    CHECK(!http_numeric_host("fixture.test",ip));
    static http_session_t numeric;
    CHECK(!http_open_port(&numeric,"10.0.2.2",18766,false));
    CHECK(http_owner==&numeric && numeric.plaintext);
    http_abort(&numeric);
    CHECK(!http_owner && !numeric.connected && numeric.tcp_conn==-1);
    CHECK(!http_open_port(&numeric,"10.0.2.2",18766,false));
    numeric.connected=false; /* simulate forced teardown during TLS handshake */
    unsigned before=closes;
    http_abort(&numeric);
    CHECK(closes==before+1 && !http_owner);
    static const struct {const char *wire, *body; bool header_failure;} cases[] = {
        {"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc", "abc", false},
        {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", "", false},
        {"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nabc", "abc", false},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4;ext=yes\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", "Wikipedia", false},
        {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc", 0, false},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nabc", 0, false},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n", 0, false},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc!!0\r\n\r\n", 0, false},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nXYZ\r\n", 0, false},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n80000000\r\n", 0, false},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\n", 0, false},
        {"HTTP/1.1 200 OK\r\nContent-Length: 4294967299\r\n\r\nabc", 0, true},
        {"HTTP/1.1 200 OK\r\nContent-Length: 3junk\r\n\r\nabc", 0, true},
        {"HTTP/1.1 200 OK\r\nContent-Length: -3\r\n\r\nabc", 0, true},
        {"HTTP/1.1 200 OK\r\nContent-Length: 3 \t\r\n\r\nabc", "abc", false},
    };
    const unsigned fragments[] = {1,2,7,4096};
    for (transport=0; transport<3; transport++) {
        for (unsigned f=0; f<4; f++) {
            fragment=fragments[f];send_fragment=fragment;
            for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++)
                http_transfer_case(cases[i].wire,cases[i].body,cases[i].header_failure);
            abort_body=1;http_transfer_case(cases[0].wire,0,false);abort_body=0;
            timeout_eof=1;http_transfer_case(cases[2].wire,0,false);timeout_eof=0;
        }
    }
    printf("HTTP transfers: %u checks, %u failures\n",checks,failures);
    return failures != 0;
}
