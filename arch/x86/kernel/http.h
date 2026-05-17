/*
 * OsitoK x86-64 — HTTP Client
 *
 * Minimal HTTPS client over TLS 1.2.
 * Supports GET and POST with chunked transfer-encoding.
 * Designed for Claude API (api.anthropic.com).
 */

#ifndef OSITOK_HTTP_H
#define OSITOK_HTTP_H

#include "../include/types.h"
#include "tls.h"

/* ── HTTP Response ──────────────────────────────────────────── */

#define HTTP_MAX_HEADERS    16
#define HTTP_MAX_HDR_NAME   32
#define HTTP_MAX_HDR_VALUE  256

typedef struct {
    char name[HTTP_MAX_HDR_NAME];
    char value[HTTP_MAX_HDR_VALUE];
} http_header_t;

typedef struct {
    int           status_code;      /* e.g. 200, 404, 500 */
    http_header_t headers[HTTP_MAX_HEADERS];
    int           header_count;
    bool          chunked;          /* Transfer-Encoding: chunked */
    uint32_t      content_length;   /* Content-Length (0 if chunked) */
} http_response_t;

/* Streaming callback: called with each chunk of body data.
 * Return 0 to continue, -1 to abort. */
typedef int (*http_body_cb)(const void *data, uint32_t len, void *ctx);

/* ── HTTP Session ───────────────────────────────────────────── */

typedef struct {
    tls_conn_t  tls;
    int         tcp_conn;
    bool        connected;
    bool        use_tls13;     /* set by http_open: true if TLS 1.3 succeeded */
} http_session_t;

/* ── HTTP API ───────────────────────────────────────────────── */

/* Open HTTPS session to host:443.
 * Performs DNS + TCP + TLS handshake.
 * Returns 0 on success, -1 on failure. */
int http_open(http_session_t *s, const char *hostname);

/* Send HTTP request and receive response headers.
 * method: "GET" or "POST"
 * path: e.g. "/v1/messages"
 * req_headers: array of "Name: Value" strings, NULL-terminated
 * body/body_len: request body (NULL/0 for GET)
 * resp: filled with status code + parsed headers
 * Returns 0 on success, -1 on failure. */
int http_request(http_session_t *s, const char *method, const char *path,
                 const char *hostname,
                 const char *const *req_headers,
                 const void *body, uint32_t body_len,
                 http_response_t *resp);

/* Read response body with streaming callback.
 * Handles chunked transfer-encoding automatically.
 * Returns total bytes read, or -1 on error. */
int http_read_body(http_session_t *s, const http_response_t *resp,
                   http_body_cb callback, void *ctx);

/* Read response body into buffer (convenience wrapper).
 * Returns bytes read, or -1 on error. */
int http_read_body_full(http_session_t *s, const http_response_t *resp,
                        void *buf, uint32_t buf_size);

/* Close HTTPS session. */
void http_close(http_session_t *s);

/* Get a response header value by name (case-insensitive).
 * Returns pointer to value string, or NULL if not found. */
const char *http_get_header(const http_response_t *resp, const char *name);

/* ── Async HTTP API ─────────────────────────────────────────── */

typedef void (*http_open_cb_t)(int result, void *ctx);

/* Async version of http_open: DNS → TCP → TLS as callback chain.
 * Calls cb(0, ctx) on success, cb(-1, ctx) on failure.
 * hostname must remain valid until callback fires.
 * Returns 0 if initiated, -1 if resources unavailable. */
int http_open_async(http_session_t *s, const char *hostname,
                    http_open_cb_t cb, void *ctx);

#endif /* OSITOK_HTTP_H */
