/*
 * OsitoK x86-64 — HTTP Client
 *
 * X-NET5: Minimal HTTPS client over DNS + TCP + TLS.
 * Supports GET/POST, chunked transfer-encoding, streaming body.
 * No redirects, no cookies, no compression.
 */

#include "http.h"

/* ── External dependencies ───────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

extern int  net_dns_resolve(const char *hostname, uint8_t ip_out[4]);
extern int  net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                            uint16_t src_port);
extern void net_tcp_close(int conn);
extern int  net_tcp_state(int conn);
extern uint64_t idt_get_ticks(void);

#define TCP_STATE_ESTABLISHED 2

/* ── Helpers ─────────────────────────────────────────────────── */

static void *hmemcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void hmemset(void *dst, int c, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)c;
}

static int hstrcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Append string to buffer, return new position */
static int buf_puts(char *buf, int pos, int max, const char *s)
{
    while (*s && pos < max - 1)
        buf[pos++] = *s++;
    return pos;
}

/* Append decimal number as string */
static int buf_putdec(char *buf, int pos, int max, uint32_t val)
{
    char tmp[12];
    int len = 0;
    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0) {
            tmp[len++] = '0' + (val % 10);
            val /= 10;
        }
    }
    for (int i = len - 1; i >= 0 && pos < max - 1; i--)
        buf[pos++] = tmp[i];
    return pos;
}

/* Parse hex digit, -1 on invalid */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Ephemeral port allocator */
static uint16_t next_port = 49400;

/* Size accessors for opaque allocation from shell */
uint32_t http_session_size(void) { return (uint32_t)sizeof(http_session_t); }
uint32_t http_response_size(void) { return (uint32_t)sizeof(http_response_t); }

/* Leftover body data from header parsing (set by http_request,
 * consumed by http_read_body). Only one request at a time. */
static uint8_t leftover_buf[4096];
static int     leftover_len;

/* ── TLS dispatch helpers (1.2/1.3 unified) ──────────────────── */

extern int tls13_send(const void *data, uint32_t len);
extern int tls13_recv(void *buf, uint32_t cap);

static inline int http_tls_send(http_session_t *s, const void *data, uint32_t len)
{
    return s->use_tls13 ? tls13_send(data, len) : tls_send(&s->tls, data, len);
}

/* 1.2 tls_recv takes a deadline-ish timeout (ticks); 1.3 tls_recv has its
 * own internal deadline. The 1.3 wrapper ignores the timeout arg for now —
 * tls13_recv blocks against the live socket with its own watchdog. */
static inline int http_tls_recv(http_session_t *s, void *buf, uint32_t cap,
                                uint32_t timeout_ticks)
{
    if (s->use_tls13) return tls13_recv(buf, cap);
    return tls_recv(&s->tls, buf, cap, timeout_ticks);
}

static inline void http_tls_close(http_session_t *s)
{
    /* TLS 1.3 has no explicit close API in this build — just drop the
     * TCP; the peer detects close via FIN. TLS 1.2 needs tls_close to
     * emit close_notify before TCP teardown. */
    if (!s->use_tls13) tls_close(&s->tls);
}

/* ── HTTP Session Management ─────────────────────────────────── */

int http_open(http_session_t *s, const char *hostname)
{
    hmemset(s, 0, sizeof(*s));

    /* DNS resolve */
    uint8_t ip[4];
    serial_puts("[HTTP] Resolving ");
    serial_puts(hostname);
    serial_puts("...\n");

    if (net_dns_resolve(hostname, ip) < 0) {
        serial_puts("[HTTP] DNS failed\n");
        return -1;
    }

    serial_puts("[HTTP] -> ");
    serial_putdec(ip[0]); serial_puts(".");
    serial_putdec(ip[1]); serial_puts(".");
    serial_putdec(ip[2]); serial_puts(".");
    serial_putdec(ip[3]); serial_puts("\n");

    /* TCP connect */
    s->tcp_conn = net_tcp_connect(ip, 443, next_port++);
    if (s->tcp_conn < 0) {
        serial_puts("[HTTP] TCP connect failed\n");
        return -1;
    }

    /* TLS handshake — prefer 1.3 (one-RTT handshake + modern ciphers).
     * Fall back to 1.2 if the server can't do 1.3. tls13 uses a global
     * state struct, so we hold an outer single-conn lock in http_open
     * by virtue of it being called serially per session. */
    extern int  tls13_connect(int tcp_conn, const char *hostname);
    if (tls13_connect(s->tcp_conn, hostname) == 0) {
        s->use_tls13 = true;
        serial_puts("[HTTP] HTTPS ready (TLS 1.3)\n");
        s->connected = true;
        return 0;
    }

    /* 1.3 failed — close and reopen TCP, then try 1.2. The TLS 1.3 path
     * may have left the connection mid-handshake; safest is a fresh TCP. */
    serial_puts("[HTTP] TLS 1.3 failed, falling back to 1.2\n");
    net_tcp_close(s->tcp_conn);
    s->tcp_conn = net_tcp_connect(ip, 443, next_port++);
    if (s->tcp_conn < 0) {
        serial_puts("[HTTP] TCP reconnect failed\n");
        return -1;
    }
    if (tls_connect(&s->tls, s->tcp_conn, hostname) < 0) {
        serial_puts("[HTTP] TLS 1.2 handshake also failed\n");
        net_tcp_close(s->tcp_conn);
        return -1;
    }
    s->use_tls13 = false;
    serial_puts("[HTTP] HTTPS ready (TLS 1.2)\n");
    s->connected = true;
    return 0;
}

void http_close(http_session_t *s)
{
    if (!s->connected) return;
    http_tls_close(s);
    net_tcp_close(s->tcp_conn);
    s->connected = false;
}

/* ── HTTP Request ────────────────────────────────────────────── */

int http_request(http_session_t *s, const char *method, const char *path,
                 const char *hostname,
                 const char *const *req_headers,
                 const void *body, uint32_t body_len,
                 http_response_t *resp)
{
    if (!s->connected) return -1;
    hmemset(resp, 0, sizeof(*resp));
    leftover_len = 0;

    /* Build request */
    char req[2048];
    int p = 0, mx = (int)sizeof(req);

    p = buf_puts(req, p, mx, method);
    p = buf_puts(req, p, mx, " ");
    p = buf_puts(req, p, mx, path);
    p = buf_puts(req, p, mx, " HTTP/1.1\r\nHost: ");
    p = buf_puts(req, p, mx, hostname);
    p = buf_puts(req, p, mx, "\r\n");

    if (req_headers) {
        for (int i = 0; req_headers[i]; i++) {
            p = buf_puts(req, p, mx, req_headers[i]);
            p = buf_puts(req, p, mx, "\r\n");
        }
    }

    if (body && body_len > 0) {
        p = buf_puts(req, p, mx, "Content-Length: ");
        p = buf_putdec(req, p, mx, body_len);
        p = buf_puts(req, p, mx, "\r\n");
    }

    p = buf_puts(req, p, mx, "\r\n");

    /* Send headers */
    if (http_tls_send(s, req, (uint32_t)p) < 0) return -1;

    /* Send body */
    if (body && body_len > 0) {
        if (http_tls_send(s, body, body_len) < 0) return -1;
    }

    serial_puts("[HTTP] Sent ");
    serial_puts(method);
    serial_puts(" ");
    serial_puts(path);
    serial_puts("\n");

    /* ── Read response headers ── */

    char hdr[4096];
    int hdr_len = 0;
    int hdr_end = -1;

    while (hdr_len < (int)sizeof(hdr) - 1) {
        int n = http_tls_recv(s, hdr + hdr_len,
                              (uint32_t)(sizeof(hdr) - 1 - (uint32_t)hdr_len), 5000);
        if (n <= 0) {
            serial_puts("[HTTP] Timeout reading headers\n");
            return -1;
        }
        hdr_len += n;
        hdr[hdr_len] = '\0';

        for (int i = 0; i <= hdr_len - 4; i++) {
            if (hdr[i] == '\r' && hdr[i+1] == '\n' &&
                hdr[i+2] == '\r' && hdr[i+3] == '\n') {
                hdr_end = i + 4;
                break;
            }
        }
        if (hdr_end >= 0) break;
    }

    if (hdr_end < 0) {
        serial_puts("[HTTP] Headers too large\n");
        return -1;
    }

    /* Parse status line: "HTTP/1.x NNN ...\r\n" */
    int i = 0;
    while (i < hdr_end && hdr[i] != ' ') i++;
    i++;
    int code = 0;
    while (i < hdr_end && hdr[i] >= '0' && hdr[i] <= '9')
        code = code * 10 + (hdr[i++] - '0');
    resp->status_code = code;

    while (i < hdr_end && hdr[i] != '\n') i++;
    i++;

    /* Parse response headers */
    while (i < hdr_end - 2 && resp->header_count < HTTP_MAX_HEADERS) {
        int ns = i;
        while (i < hdr_end && hdr[i] != ':' && hdr[i] != '\r') i++;
        if (hdr[i] != ':') break;
        int nlen = i - ns;
        i++;
        while (i < hdr_end && hdr[i] == ' ') i++;

        int vs = i;
        while (i < hdr_end && hdr[i] != '\r') i++;
        int vlen = i - vs;

        if (i < hdr_end && hdr[i] == '\r') i++;
        if (i < hdr_end && hdr[i] == '\n') i++;

        http_header_t *h = &resp->headers[resp->header_count];
        int cn = nlen < HTTP_MAX_HDR_NAME - 1 ? nlen : HTTP_MAX_HDR_NAME - 1;
        hmemcpy(h->name, hdr + ns, (uint32_t)cn);
        h->name[cn] = '\0';
        int cv = vlen < HTTP_MAX_HDR_VALUE - 1 ? vlen : HTTP_MAX_HDR_VALUE - 1;
        hmemcpy(h->value, hdr + vs, (uint32_t)cv);
        h->value[cv] = '\0';
        resp->header_count++;
    }

    /* Detect chunked / content-length */
    const char *te = http_get_header(resp, "Transfer-Encoding");
    if (te && hstrcasecmp(te, "chunked") == 0)
        resp->chunked = true;

    const char *cl = http_get_header(resp, "Content-Length");
    if (cl) {
        uint32_t v = 0;
        for (int j = 0; cl[j] >= '0' && cl[j] <= '9'; j++)
            v = v * 10 + (uint32_t)(cl[j] - '0');
        resp->content_length = v;
    }

    serial_puts("[HTTP] ");
    serial_putdec((uint64_t)resp->status_code);
    if (resp->chunked) serial_puts(" chunked");
    serial_puts("\n");

    /* Stash leftover body data */
    int left = hdr_len - hdr_end;
    if (left > 0 && left <= (int)sizeof(leftover_buf)) {
        hmemcpy(leftover_buf, hdr + hdr_end, (uint32_t)left);
        leftover_len = left;
    }

    return 0;
}

/* ── Header Lookup ───────────────────────────────────────────── */

const char *http_get_header(const http_response_t *resp, const char *name)
{
    for (int i = 0; i < resp->header_count; i++) {
        if (hstrcasecmp(resp->headers[i].name, name) == 0)
            return resp->headers[i].value;
    }
    return NULL;
}

/* ── Body Reading ────────────────────────────────────────────── */

int http_read_body(http_session_t *s, const http_response_t *resp,
                   http_body_cb callback, void *ctx)
{
    if (!s->connected) return -1;

    int total = 0;

    /* tls_recv conflates two return cases under -1: hard fail
     * (connection closed, decrypt error, alert) and pure timeout
     * (no record within the 500-tick budget — peer is mid-response,
     * the next TLS record is just slow to arrive).  Treating both as
     * end-of-stream truncates large bodies whenever CF inserts a >5 s
     * pause between records (their bge-large `/embed` reply does this
     * routinely under load: ~7 KB land fast, the rest dribbles in).
     *
     * tls_recv's internal call chain (tls_recv_record → tls_read_exact
     * → net_tcp_recv_timeout) reports the same -1 for "deadline
     * elapsed" as for "TCP closed."  Rather than threading a new
     * return code through three layers, distinguish at this layer:
     * after a -1, peek at the TCP connection state.  If the kernel
     * still has it ESTABLISHED, the peer hasn't closed — retry up to
     * MAX_IDLE_RETRIES times.  If it's any post-ESTABLISHED state,
     * the peer really closed and we should stop. */
    const int MAX_IDLE_RETRIES = 12;   /* ~60 s at 500 ticks each */
    int idle = 0;
    if (resp->chunked) {
        /* ── Chunked transfer-encoding ── */
        /* Format: <hex-size>\r\n<data>\r\n ... 0\r\n\r\n */
        uint8_t buf[4096];
        int bp = 0, bl = 0;  /* buffer position, buffer length */

        /* Seed buffer with leftover */
        if (leftover_len > 0 && leftover_len <= (int)sizeof(buf)) {
            hmemcpy(buf, leftover_buf, (uint32_t)leftover_len);
            bl = leftover_len;
            leftover_len = 0;
        }

        for (;;) {
            /* Read chunk size line */
            char line[32];
            int lp = 0;
            bool got_line = false;

            while (!got_line) {
                if (bp >= bl) {
                    int n = http_tls_recv(s, buf, sizeof(buf), 500);
                    if (n < 0) {
                        if (net_tcp_state(s->tcp_conn) == TCP_STATE_ESTABLISHED
                            && ++idle < MAX_IDLE_RETRIES) continue;
                        goto done;
                    }
                    if (n == 0) {
                        if (++idle >= MAX_IDLE_RETRIES) goto done;
                        continue;
                    }
                    idle = 0;
                    bp = 0;
                    bl = n;
                }
                char c = (char)buf[bp++];
                if (c == '\n') {
                    line[lp] = '\0';
                    got_line = true;
                } else if (lp < (int)sizeof(line) - 1 && c != '\r') {
                    line[lp++] = c;
                }
            }

            /* Parse hex chunk size */
            uint32_t chunk_size = 0;
            for (int i = 0; line[i]; i++) {
                int d = hex_digit(line[i]);
                if (d < 0) break;
                chunk_size = (chunk_size << 4) | (uint32_t)d;
            }

            if (chunk_size == 0) break;  /* final chunk */

            /* Read chunk data */
            uint32_t remaining = chunk_size;
            while (remaining > 0) {
                if (bp >= bl) {
                    int n = http_tls_recv(s, buf, sizeof(buf), 500);
                    if (n < 0) {
                        if (net_tcp_state(s->tcp_conn) == TCP_STATE_ESTABLISHED
                            && ++idle < MAX_IDLE_RETRIES) continue;
                        goto done;
                    }
                    if (n == 0) {
                        if (++idle >= MAX_IDLE_RETRIES) goto done;
                        continue;
                    }
                    idle = 0;
                    bp = 0;
                    bl = n;
                }
                uint32_t avail = (uint32_t)(bl - bp);
                uint32_t take = remaining < avail ? remaining : avail;

                if (callback) {
                    if (callback(buf + bp, take, ctx) < 0)
                        return -1;
                }
                total += (int)take;
                bp += (int)take;
                remaining -= take;
            }

            /* Consume trailing \r\n */
            for (int skip = 0; skip < 2; ) {
                if (bp >= bl) {
                    int n = http_tls_recv(s, buf, sizeof(buf), 500);
                    if (n < 0) {
                        if (net_tcp_state(s->tcp_conn) == TCP_STATE_ESTABLISHED
                            && ++idle < MAX_IDLE_RETRIES) continue;
                        goto done;
                    }
                    if (n == 0) {
                        if (++idle >= MAX_IDLE_RETRIES) goto done;
                        continue;
                    }
                    idle = 0;
                    bp = 0;
                    bl = n;
                }
                bp++;
                skip++;
            }
        }
    } else {
        /* ── Content-Length or read until close ── */
        uint32_t remaining = resp->content_length;
        bool has_cl = (remaining > 0);
        uint8_t buf[4096];

        /* Deliver leftover first */
        if (leftover_len > 0) {
            uint32_t give = (uint32_t)leftover_len;
            if (has_cl && give > remaining) give = remaining;
            if (callback) {
                if (callback(leftover_buf, give, ctx) < 0)
                    return -1;
            }
            total += (int)give;
            if (has_cl) remaining -= give;
            leftover_len = 0;
        }

        while (!has_cl || remaining > 0) {
            uint32_t want = sizeof(buf);
            if (has_cl && remaining < want) want = remaining;

            int n = http_tls_recv(s, buf, want, 500);
            if (n < 0) {
                /* tls_recv -1: could be a real close or just a tls_recv
                 * deadline (the underlying tls_read_exact treats both
                 * the same).  If the TCP connection is still alive and
                 * we still have content-length remaining, keep trying. */
                if (net_tcp_state(s->tcp_conn) == TCP_STATE_ESTABLISHED
                    && ++idle < MAX_IDLE_RETRIES) continue;
                break;
            }
            if (n == 0) {
                /* Timeout with connection still alive.  If we have a
                 * known content-length and haven't reached it, keep
                 * trying — the server may be mid-response.  For read-
                 * until-close (no CL), a long idle stretch is the
                 * normal terminator, so still cap retries. */
                if (++idle >= MAX_IDLE_RETRIES) break;
                continue;
            }
            idle = 0;

            if (callback) {
                if (callback(buf, (uint32_t)n, ctx) < 0)
                    return -1;
            }
            total += n;
            if (has_cl) remaining -= (uint32_t)n;
        }
    }

done:
    return total;
}

/* ── Convenience: read body into buffer ──────────────────────── */

typedef struct {
    uint8_t  *buf;
    uint32_t  size;
    uint32_t  pos;
} buf_ctx_t;

static int buf_body_cb(const void *data, uint32_t len, void *ctx_)
{
    buf_ctx_t *b = (buf_ctx_t *)ctx_;
    uint32_t copy = len;
    if (b->pos + copy > b->size)
        copy = b->size - b->pos;
    if (copy > 0) {
        hmemcpy(b->buf + b->pos, data, copy);
        b->pos += copy;
    }
    return 0;
}

int http_read_body_full(http_session_t *s, const http_response_t *resp,
                        void *buf, uint32_t buf_size)
{
    buf_ctx_t ctx = { .buf = (uint8_t *)buf, .size = buf_size, .pos = 0 };
    int r = http_read_body(s, resp, buf_body_cb, &ctx);
    if (r < 0) return -1;
    return (int)ctx.pos;
}

/* ══════════════════════════════════════════════════════════════
 *  Async HTTP open — DNS → TCP → TLS callback chain
 * ══════════════════════════════════════════════════════════════ */

extern int net_dns_resolve_async(const char *, uint8_t *, void (*)(int, void*), void*);
extern int net_tcp_connect_async(const uint8_t *, uint16_t, uint16_t,
                                 void (*)(int, void*), void*);

/* Private context threaded through the callback chain */
typedef struct {
    http_session_t *session;
    const char     *hostname;
    http_open_cb_t  done_cb;
    void           *done_ctx;
    uint8_t         resolved_ip[4];
    uint16_t        src_port;
} http_async_ctx_t;

/* We use a small static pool (2 slots) since HTTP opens are rare */
#define HTTP_ASYNC_SLOTS 2
static http_async_ctx_t http_async_pool[HTTP_ASYNC_SLOTS];

static http_async_ctx_t *http_async_alloc(void)
{
    for (int i = 0; i < HTTP_ASYNC_SLOTS; i++)
        if (!http_async_pool[i].session) return &http_async_pool[i];
    return NULL;
}

/* Phase 3: TLS handshake done (still synchronous — TLS is stateful) */
static void http_async_tls_done(http_async_ctx_t *ac, int result)
{
    http_session_t *s = ac->session;
    http_open_cb_t cb = ac->done_cb;
    void *ctx = ac->done_ctx;

    ac->session = NULL;  /* release slot */

    if (result < 0) {
        serial_puts("[HTTP-ASYNC] TLS failed\n");
        net_tcp_close(s->tcp_conn);
        if (cb) cb(-1, ctx);
        return;
    }

    serial_puts("[HTTP-ASYNC] HTTPS ready\n");
    s->connected = true;
    if (cb) cb(0, ctx);
}

/* Phase 2: TCP connected → start TLS handshake */
static void http_async_tcp_done(int result, void *arg)
{
    http_async_ctx_t *ac = (http_async_ctx_t *)arg;
    http_session_t *s = ac->session;

    if (result < 0) {
        serial_puts("[HTTP-ASYNC] TCP connect failed\n");
        http_open_cb_t cb = ac->done_cb;
        void *ctx = ac->done_ctx;
        ac->session = NULL;
        if (cb) cb(-1, ctx);
        return;
    }

    s->tcp_conn = result;
    serial_puts("[HTTP-ASYNC] TCP connected, starting TLS...\n");

    /* TLS handshake is still synchronous (complex state machine).
     * This blocks briefly but DNS+TCP were async. */
    int tls_result = tls_connect(&s->tls, s->tcp_conn, ac->hostname);
    http_async_tls_done(ac, tls_result);
}

/* Phase 1: DNS resolved → start TCP connect */
static void http_async_dns_done(int result, void *arg)
{
    http_async_ctx_t *ac = (http_async_ctx_t *)arg;

    if (result < 0) {
        serial_puts("[HTTP-ASYNC] DNS failed\n");
        http_open_cb_t cb = ac->done_cb;
        void *ctx = ac->done_ctx;
        ac->session = NULL;
        if (cb) cb(-1, ctx);
        return;
    }

    serial_puts("[HTTP-ASYNC] DNS resolved: ");
    serial_putdec(ac->resolved_ip[0]); serial_puts(".");
    serial_putdec(ac->resolved_ip[1]); serial_puts(".");
    serial_putdec(ac->resolved_ip[2]); serial_puts(".");
    serial_putdec(ac->resolved_ip[3]); serial_puts("\n");

    /* Start async TCP connect */
    if (net_tcp_connect_async(ac->resolved_ip, 443, ac->src_port,
                              http_async_tcp_done, ac) < 0) {
        serial_puts("[HTTP-ASYNC] TCP connect queue failed\n");
        http_open_cb_t cb = ac->done_cb;
        void *ctx = ac->done_ctx;
        ac->session = NULL;
        if (cb) cb(-1, ctx);
    }
}

int http_open_async(http_session_t *s, const char *hostname,
                    http_open_cb_t cb, void *ctx)
{
    hmemset(s, 0, sizeof(*s));

    http_async_ctx_t *ac = http_async_alloc();
    if (!ac) return -1;

    ac->session  = s;
    ac->hostname = hostname;
    ac->done_cb  = cb;
    ac->done_ctx = ctx;
    ac->src_port = next_port++;

    serial_puts("[HTTP-ASYNC] Resolving ");
    serial_puts(hostname);
    serial_puts("...\n");

    if (net_dns_resolve_async(hostname, ac->resolved_ip,
                              http_async_dns_done, ac) < 0) {
        ac->session = NULL;
        return -1;
    }

    return 0;
}
