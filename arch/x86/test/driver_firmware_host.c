#include "../kernel/driver_firmware.c"
extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void free(void *);
static unsigned failures, checks, allocations, requests, published;
static bool local_file, corrupt, online, bad_status, too_large, transport_error, persist_error;
static uint8_t ip[4] = {192,168,1,20};
static uint64_t clock_ns, epoch = 1;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)
void *kmalloc(size_t n) { void *p = malloc(n); if (p) allocations++; return p; }
void kfree(void *p) { if (p) { allocations--; free(p); } }
bool net_link_ready(void) { return online; }
uint8_t *net_get_ip_ptr(void) { return ip; }
uint64_t net_link_epoch(void) { return epoch; }
uint64_t idt_get_monotonic_ns(void) { return clock_ns; }
int driver_file_read(const char *p, void *data, unsigned cap, unsigned *n, bool private_file)
{ (void)p; (void)private_file; CHECK(cap == 10); if (!local_file) return 1; memcpy(data, corrupt ? "bad" : "abc", 3); *n = 3; return 0; }
int driver_file_publish(const char *path, const void *data, unsigned n, bool replace)
{ CHECK(!strcmp(path, "test-fw") && n == 3 && !memcmp(data, "abc", 3) && !replace); published++; return persist_error ? -1 : 0; }
int http_open(http_session_t *s, const char *host) { (void)s; CHECK(!strcmp(host, "example.test")); return transport_error ? -1 : 0; }
void http_close(http_session_t *s) { (void)s; }
int http_request(http_session_t *s, const char *method, const char *path, const char *host,
                 const char *const *headers, const void *body, uint32_t size, http_response_t *r)
{
    (void)s; (void)host; (void)headers;
    CHECK(!strcmp(method, "GET") && !strcmp(path, "/pinned") && !body && !size);
    requests++; memset(r, 0, sizeof(*r)); r->status_code = bad_status ? 302 : 200;
    r->content_length = too_large ? 100 : 3; return 0;
}
int http_read_body(http_session_t *s, const http_response_t *r, http_body_cb cb, void *arg)
{ (void)s; (void)r; return cb(corrupt ? "bad" : "abc", 3, arg) ? -1 : 3; }
int main(void)
{
    const driver_firmware_t fw = {"test-fw", "example.test", "/pinned",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 10};
    void *data; unsigned n; const char *error;
    CHECK(driver_firmware_get(&fw, &data, &n, &error) == 1 && !data && !allocations && !requests);
    local_file = true; CHECK(!driver_firmware_get(&fw, &data, &n, &error) && n == 3 && !error && !requests); kfree(data);
    corrupt = true; CHECK(driver_firmware_get(&fw, &data, &n, &error) < 0 && !allocations && !published);
    local_file = false; corrupt = false; online = true;
    bad_status = true; CHECK(driver_firmware_get(&fw, &data, &n, &error) < 0 && !published); bad_status = false;
    too_large = true; CHECK(driver_firmware_get(&fw, &data, &n, &error) < 0 && !published); too_large = false;
    corrupt = true; CHECK(driver_firmware_get(&fw, &data, &n, &error) < 0 && !published); corrupt = false;
    transport_error = true; CHECK(driver_firmware_get(&fw, &data, &n, &error) < 0 && !published); transport_error = false;
    persist_error = true; CHECK(driver_firmware_get(&fw, &data, &n, &error) < 0 && !data); persist_error = false;
    CHECK(!driver_firmware_get(&fw, &data, &n, &error) && n == 3 && !memcmp(data, "abc", 3)); kfree(data);
    uint8_t buffer[8]; download_t d = {buffer, 0, sizeof(buffer), 10, epoch};
    CHECK(download_body("oversized", 9, &d) < 0);
    clock_ns = 11; CHECK(download_body("x", 1, &d) < 0); clock_ns = 0;
    epoch++; CHECK(download_body("x", 1, &d) < 0);
    CHECK(!allocations);
    printf("driver-firmware: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
