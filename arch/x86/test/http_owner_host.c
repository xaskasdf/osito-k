#include "../kernel/http.c"
extern int printf(const char *, ...);
static unsigned checks, failures, fail, tcp_calls, closed, completions;
static int completion;
static void (*dns_done)(int, void *), (*tcp_done)(int, void *);
static void *dns_arg, *tcp_arg;
static http_session_t first, second;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
int net_dns_resolve(const char *host, uint8_t ip[4])
{ (void)host; memset(ip, 1, 4); return fail & 1 ? -1 : 0; }
int net_tcp_connect(const uint8_t ip[4], uint16_t port, uint16_t source)
{
    (void)ip; (void)port; (void)source;
    tcp_calls++;
    return ((tcp_calls == 1 && (fail & 2)) || (tcp_calls == 2 && (fail & 8))) ? -1 : 7;
}
void net_tcp_close(int fd) { CHECK(fd == 7); closed++; }
int tls13_connect(int fd, const char *host)
{ (void)fd; (void)host; return fail & 4 ? -1 : 0; }
int tls_connect(tls_conn_t *tls, int fd, const char *host)
{ (void)tls; (void)fd; (void)host; return fail & 16 ? -1 : 0; }
void tls_close(tls_conn_t *tls) { (void)tls; }
void tls13_close(void) {}
int net_dns_resolve_async(const char *host, uint8_t *ip, void (*done)(int, void *), void *arg)
{
    (void)host; memset(ip, 1, 4);
    if (fail & 32) return -1;
    dns_done = done; dns_arg = arg; return 0;
}
int net_tcp_connect_async(const uint8_t *ip, uint16_t port, uint16_t source,
                          void (*done)(int, void *), void *arg)
{
    (void)ip; (void)port; (void)source;
    if (fail & 64) return -1;
    tcp_done = done; tcp_arg = arg; return 0;
}
static void completed(int result, void *arg)
{ CHECK(arg == &first); completion = result; completions++; }
static void reset(unsigned faults)
{
    CHECK(!http_owner);
    for (unsigned i = 0; i < HTTP_ASYNC_SLOTS; i++) CHECK(!http_async_pool[i].session);
    fail = faults; tcp_calls = closed = completions = 0; completion = 1;
    dns_done = tcp_done = NULL; dns_arg = tcp_arg = NULL;
    memset(&first, 0, sizeof(first)); memset(&second, 0xa5, sizeof(second));
}
static void available(void)
{
    fail = 0;
    CHECK(!http_open(&second, "second.test")); CHECK(http_owner == &second);
    http_close(&second); CHECK(!http_owner);
    unsigned count = closed; http_close(&second); CHECK(closed == count);
}
int main(void)
{
    static const unsigned faults[] = {1, 2, 4 | 8, 4 | 16};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        reset(faults[i]); CHECK(http_open(&first, "first.test") < 0);
        CHECK(!http_owner); available();
    }
    reset(0); CHECK(!http_open(&first, "first.test"));
    CHECK(http_open(&second, "second.test") < 0 && tcp_calls == 1);
    CHECK(http_open_async(&second, "second.test", completed, &first) < 0);
    http_close(&second); CHECK(!closed && http_owner == &first);
    http_close(&first); CHECK(closed == 1); available();
    reset(4); CHECK(!http_open(&first, "first.test") && !first.use_tls13);
    CHECK(tcp_calls == 2 && closed == 1); http_close(&first); available();
    reset(32); CHECK(http_open_async(&first, "first.test", completed, &first) < 0);
    CHECK(!completions); available();
    for (unsigned stage = 0; stage < 5; stage++) {
        reset(stage == 1 ? 64 : stage == 3 ? 16 : 0);
        CHECK(!http_open_async(&first, "first.test", completed, &first));
        CHECK(http_open(&second, "second.test") < 0 && !tcp_calls);
        CHECK(dns_done != NULL); dns_done(stage == 0 ? -1 : 0, dns_arg);
        if (stage >= 2) { CHECK(tcp_done != NULL); tcp_done(stage == 2 ? -1 : 7, tcp_arg); }
        CHECK(completions == 1 && completion == (stage == 4 ? 0 : -1));
        if (stage == 4) {
            CHECK(http_owner == &first && first.connected);
            CHECK(http_open(&second, "second.test") < 0); http_close(&first);
        }
        available();
    }
    printf("HTTP session ownership: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
