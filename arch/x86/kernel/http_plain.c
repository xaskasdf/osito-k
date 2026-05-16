/*
 * http_plain.c — plain HTTP/1.1 client for AIA + OCSP.
 *
 * Smaller and simpler than http.c (which is HTTPS-only).  Used
 * exclusively by the AIA-chasing path and OCSP queries, which
 * RFC 5280 §4.2.2.1 and RFC 6960 specify MUST use plain HTTP —
 * fetching cert material over HTTPS would create a chicken-and-egg
 * problem.
 *
 * Subset of HTTP/1.1 we implement:
 *   - GET and POST with Content-Length bodies (no chunked TX).
 *   - Response: status line, headers, Content-Length body or
 *     read-until-close.
 *   - No redirects, cookies, compression, keep-alive, or proxy.
 *
 * The output buffer holds ONLY the body — headers are parsed and
 * discarded.  Caller supplies max body size.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

extern int  net_dns_resolve(const char *hostname, uint8_t ip_out[4]);
extern int  net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                            uint16_t src_port);
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern int  net_tcp_recv_timeout(int conn, void *buf, uint32_t size,
                                  uint32_t timeout_ticks);
extern void net_tcp_close(int conn);
extern int  net_tcp_state(int conn);

#define TCP_STATE_ESTABLISHED 2

/* Parse a URL of the form `http://host[:port]/path`.  Fills `host`,
 * `port`, `path`.  Returns 0 on success. */
static int parse_http_url(const char *url, char *host, uint32_t host_cap,
                          uint16_t *port_out, char *path, uint32_t path_cap)
{
    /* Match the literal "http://" prefix. */
    static const char prefix[] = "http://";
    for (uint32_t i = 0; i < 7; i++)
        if (url[i] != prefix[i]) return -1;
    const char *p = url + 7;

    /* host[:port] up to '/' or end. */
    uint32_t hi = 0;
    while (*p && *p != '/' && *p != ':' && hi + 1 < host_cap) host[hi++] = *p++;
    host[hi] = 0;
    uint16_t port = 80;
    if (*p == ':') {
        p++;
        port = 0;
        while (*p >= '0' && *p <= '9') { port = (uint16_t)(port * 10 + (*p - '0')); p++; }
    }
    *port_out = port;

    /* Path defaults to "/" if empty. */
    if (*p == 0) {
        if (path_cap >= 2) { path[0] = '/'; path[1] = 0; }
        return 0;
    }
    uint32_t pi = 0;
    while (*p && pi + 1 < path_cap) path[pi++] = *p++;
    path[pi] = 0;
    return 0;
}

/* Helper: copy a constant string into a buffer at offset, advancing. */
static uint32_t put_str(uint8_t *buf, uint32_t off, uint32_t cap, const char *s)
{
    for (uint32_t i = 0; s[i] && off + 1 < cap; i++) buf[off++] = (uint8_t)s[i];
    return off;
}
static uint32_t put_u32(uint8_t *buf, uint32_t off, uint32_t cap, uint32_t v)
{
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; i--)
        if (off + 1 < cap) buf[off++] = (uint8_t)tmp[i];
    return off;
}

/* Read response, drop headers, body to out.  Returns body length on
 * success, -1 on failure. */
static int read_response(int conn, uint8_t *out, uint32_t cap)
{
    static uint8_t buf[4096];
    uint32_t buf_len = 0;
    uint32_t body_off = 0;
    int header_done = 0;
    uint32_t content_length = 0;
    int      have_cl = 0;

    /* Read until \r\n\r\n found in the buffer. */
    int idle = 0;
    while (!header_done && buf_len < sizeof buf - 1) {
        int n = net_tcp_recv_timeout(conn, buf + buf_len,
                                      (uint32_t)(sizeof buf - 1 - buf_len), 200);
        if (n < 0) {
            if (net_tcp_state(conn) == TCP_STATE_ESTABLISHED && ++idle < 12)
                continue;
            return -1;
        }
        if (n == 0) { if (++idle >= 12) return -1; continue; }
        idle = 0;
        buf_len += (uint32_t)n;
        /* Look for \r\n\r\n. */
        for (uint32_t i = 3; i < buf_len; i++) {
            if (buf[i - 3] == '\r' && buf[i - 2] == '\n' &&
                buf[i - 1] == '\r' && buf[i - 0] == '\n') {
                header_done = 1;
                body_off = i + 1;
                /* NOTE: do NOT null-terminate buf[i+1] here — that
                 * byte is the first byte of the body.  We treat the
                 * headers as a length-bounded byte range below
                 * instead of a C string. */
                break;
            }
        }
    }
    if (!header_done) return -1;

    /* Parse status code: "HTTP/1.x SSS ..." */
    uint32_t hp = 0;
    while (hp < buf_len && buf[hp] != ' ') hp++;
    while (hp < buf_len && buf[hp] == ' ') hp++;
    int status = 0;
    while (hp < buf_len && buf[hp] >= '0' && buf[hp] <= '9') {
        status = status * 10 + (buf[hp] - '0'); hp++;
    }
    if (status < 200 || status >= 300) {
        serial_puts("[HTTP-P] non-2xx status ");
        serial_putdec((uint64_t)status); serial_puts("\n");
        return -1;
    }

    /* Find Content-Length header (case-insensitive). */
    for (uint32_t i = 0; i + 16 < body_off; i++) {
        if ((buf[i] == 'C' || buf[i] == 'c') &&
            (buf[i+1] == 'o' || buf[i+1] == 'O')) {
            /* Match "Content-Length:". */
            static const char clh[] = "Content-Length:";
            int ok = 1;
            for (int k = 0; k < 15; k++) {
                char a = (char)buf[i + k]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                char b = clh[k];           if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { ok = 0; break; }
            }
            if (!ok) continue;
            uint32_t v = i + 15;
            while (v < body_off && (buf[v] == ' ' || buf[v] == '\t')) v++;
            while (v < body_off && buf[v] >= '0' && buf[v] <= '9') {
                content_length = content_length * 10 + (buf[v] - '0'); v++;
            }
            have_cl = 1;
            break;
        }
    }
    (void)have_cl;

    /* Copy the body bytes already in `buf`. */
    uint32_t already = buf_len - body_off;
    uint32_t take = already < cap ? already : cap;
    for (uint32_t i = 0; i < take; i++) out[i] = buf[body_off + i];
    uint32_t got = take;

    /* Read the rest from the socket. */
    uint32_t want_total = have_cl ? content_length : cap;
    if (want_total > cap) want_total = cap;
    idle = 0;
    while (got < want_total) {
        int n = net_tcp_recv_timeout(conn, out + got, want_total - got, 200);
        if (n < 0) {
            if (net_tcp_state(conn) == TCP_STATE_ESTABLISHED && ++idle < 12)
                continue;
            break;
        }
        if (n == 0) { if (++idle >= 12) break; continue; }
        idle = 0;
        got += (uint32_t)n;
    }
    return (int)got;
}

/* GET url and place response body in `out`.  Returns body length or -1. */
int http_plain_get(const char *url, uint8_t *out, uint32_t out_cap)
{
    char host[128];
    char path[512];
    uint16_t port;
    if (parse_http_url(url, host, sizeof host, &port, path, sizeof path) < 0) {
        serial_puts("[HTTP-P] bad URL\n"); return -1;
    }
    uint8_t ip[4];
    if (net_dns_resolve(host, ip) < 0) {
        serial_puts("[HTTP-P] DNS fail: "); serial_puts(host); serial_puts("\n");
        return -1;
    }
    int conn = net_tcp_connect(ip, port, 50100 + (port & 0xFF));
    if (conn < 0) { serial_puts("[HTTP-P] TCP connect fail\n"); return -1; }

    /* Build request. */
    static uint8_t req[1024];
    uint32_t off = 0;
    off = put_str(req, off, sizeof req, "GET ");
    off = put_str(req, off, sizeof req, path);
    off = put_str(req, off, sizeof req, " HTTP/1.1\r\nHost: ");
    off = put_str(req, off, sizeof req, host);
    off = put_str(req, off, sizeof req, "\r\nConnection: close\r\n\r\n");
    if (net_tcp_send(conn, req, off) < 0) {
        net_tcp_close(conn); return -1;
    }
    int body_len = read_response(conn, out, out_cap);
    net_tcp_close(conn);
    return body_len;
}

/* POST `body` with Content-Type to `url`; response body in `out`. */
int http_plain_post(const char *url, const char *content_type,
                    const uint8_t *body, uint32_t body_len,
                    uint8_t *out, uint32_t out_cap)
{
    char host[128];
    char path[512];
    uint16_t port;
    if (parse_http_url(url, host, sizeof host, &port, path, sizeof path) < 0) {
        serial_puts("[HTTP-P] bad URL\n"); return -1;
    }
    uint8_t ip[4];
    if (net_dns_resolve(host, ip) < 0) {
        serial_puts("[HTTP-P] DNS fail: "); serial_puts(host); serial_puts("\n");
        return -1;
    }
    int conn = net_tcp_connect(ip, port, 50200 + (port & 0xFF));
    if (conn < 0) { serial_puts("[HTTP-P] TCP connect fail\n"); return -1; }

    static uint8_t req[2048];
    uint32_t off = 0;
    off = put_str(req, off, sizeof req, "POST ");
    off = put_str(req, off, sizeof req, path);
    off = put_str(req, off, sizeof req, " HTTP/1.1\r\nHost: ");
    off = put_str(req, off, sizeof req, host);
    off = put_str(req, off, sizeof req, "\r\nContent-Type: ");
    off = put_str(req, off, sizeof req, content_type);
    off = put_str(req, off, sizeof req, "\r\nContent-Length: ");
    off = put_u32(req, off, sizeof req, body_len);
    off = put_str(req, off, sizeof req, "\r\nConnection: close\r\n\r\n");
    if (net_tcp_send(conn, req, off) < 0) {
        net_tcp_close(conn); return -1;
    }
    if (body_len > 0)
        if (net_tcp_send(conn, body, body_len) < 0) {
            net_tcp_close(conn); return -1;
        }
    int body_resp = read_response(conn, out, out_cap);
    net_tcp_close(conn);
    return body_resp;
}
