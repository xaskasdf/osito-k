/*
 * OsitoK x86-64 — Claude API Client
 *
 * X-CL1: Talks to api.anthropic.com/v1/messages over HTTPS.
 * Builds JSON request, parses SSE streaming response.
 * No external JSON library — minimal hand-rolled builder/parser.
 */

#include "claude.h"
#include "http.h"

/* ── External dependencies ───────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern uint32_t http_session_size(void);
extern uint32_t http_response_size(void);

/* ── Helpers ─────────────────────────────────────────────────── */

static uint32_t cstrlen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void *cmemcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void cmemset(void *dst, int c, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)c;
}

/* ── API Key Storage ─────────────────────────────────────────── */

static char api_key[256];
static bool api_key_set = false;

void claude_set_api_key(const char *key)
{
    uint32_t len = cstrlen(key);
    if (len >= sizeof(api_key)) len = sizeof(api_key) - 1;
    cmemcpy(api_key, key, len);
    api_key[len] = '\0';
    api_key_set = true;
}

const char *claude_get_api_key(void)
{
    return api_key_set ? api_key : (const char *)0;
}

/* ── JSON Builder ────────────────────────────────────────────── */

/* Append string to buffer, return new position */
static int jp(char *buf, int pos, int max, const char *s)
{
    while (*s && pos < max - 1)
        buf[pos++] = *s++;
    return pos;
}

/* Append JSON-escaped string (escapes " and \ and control chars) */
static int jp_str(char *buf, int pos, int max, const char *s)
{
    pos = jp(buf, pos, max, "\"");
    while (*s && pos < max - 2) {
        char c = *s++;
        if (c == '"') {
            buf[pos++] = '\\'; if (pos < max - 1) buf[pos++] = '"';
        } else if (c == '\\') {
            buf[pos++] = '\\'; if (pos < max - 1) buf[pos++] = '\\';
        } else if (c == '\n') {
            buf[pos++] = '\\'; if (pos < max - 1) buf[pos++] = 'n';
        } else if (c == '\r') {
            buf[pos++] = '\\'; if (pos < max - 1) buf[pos++] = 'r';
        } else if (c == '\t') {
            buf[pos++] = '\\'; if (pos < max - 1) buf[pos++] = 't';
        } else {
            buf[pos++] = c;
        }
    }
    pos = jp(buf, pos, max, "\"");
    return pos;
}

/* Append decimal number */
static int jp_int(char *buf, int pos, int max, int val)
{
    char tmp[12];
    int len = 0;
    if (val == 0) {
        tmp[len++] = '0';
    } else {
        int v = val < 0 ? -val : val;
        while (v > 0) { tmp[len++] = '0' + (v % 10); v /= 10; }
        if (val < 0) tmp[len++] = '-';
    }
    for (int i = len - 1; i >= 0 && pos < max - 1; i--)
        buf[pos++] = tmp[i];
    return pos;
}

/* Build Claude API request JSON */
static int build_request_json(char *buf, int buf_size,
                               const claude_msg_t *messages, int msg_count,
                               const char *model, int max_tokens, bool stream)
{
    int p = 0, mx = buf_size;

    p = jp(buf, p, mx, "{\"model\":");
    p = jp_str(buf, p, mx, model ? model : CLAUDE_DEFAULT_MODEL);

    p = jp(buf, p, mx, ",\"max_tokens\":");
    p = jp_int(buf, p, mx, max_tokens > 0 ? max_tokens : 1024);

    if (stream)
        p = jp(buf, p, mx, ",\"stream\":true");

    p = jp(buf, p, mx, ",\"messages\":[");
    for (int i = 0; i < msg_count; i++) {
        if (i > 0) p = jp(buf, p, mx, ",");
        p = jp(buf, p, mx, "{\"role\":");
        p = jp_str(buf, p, mx, messages[i].role);
        p = jp(buf, p, mx, ",\"content\":");
        p = jp_str(buf, p, mx, messages[i].content);
        p = jp(buf, p, mx, "}");
    }
    p = jp(buf, p, mx, "]}");

    if (p < mx) buf[p] = '\0';
    return p;
}

/* ── JSON Parser (minimal) ───────────────────────────────────── */

/* Find a JSON string value by key in a flat JSON object.
 * Returns pointer to the value string (inside the source),
 * and sets *len to the string length (excluding quotes).
 * Does NOT handle nested objects/arrays as values.
 * Returns NULL if key not found. */
static const char *json_find_str(const char *json, const char *key,
                                  int *len)
{
    /* Search for "key":"value" */
    uint32_t klen = cstrlen(key);
    const char *p = json;

    while (*p) {
        /* Find quote-delimited key */
        if (*p == '"') {
            p++;
            /* Compare key */
            bool match = true;
            for (uint32_t i = 0; i < klen; i++) {
                if (p[i] != key[i]) { match = false; break; }
            }
            if (match && p[klen] == '"') {
                p += klen + 1;  /* skip key + closing quote */
                /* Skip whitespace and colon */
                while (*p == ' ' || *p == ':') p++;
                if (*p == '"') {
                    p++;  /* opening quote of value */
                    const char *start = p;
                    /* Find closing quote (handle escapes) */
                    while (*p && !(*p == '"' && *(p-1) != '\\')) p++;
                    *len = (int)(p - start);
                    return start;
                }
            }
        }
        p++;
    }
    return NULL;
}

/* ── SSE Stream Parser ───────────────────────────────────────── */

/* Parse SSE events from the streaming response body.
 * SSE format:
 *   event: <type>\n
 *   data: <json>\n
 *   \n
 *
 * We care about content_block_delta events with text_delta.
 * data: {"type":"content_block_delta","index":0,
 *        "delta":{"type":"text_delta","text":"Hello"}}
 */

typedef struct {
    claude_stream_cb callback;
    void            *ctx;
    int              total_len;
    bool             error;
} sse_ctx_t;

static int sse_body_cb(const void *data, uint32_t len, void *ctx)
{
    sse_ctx_t *sc = (sse_ctx_t *)ctx;
    const char *buf = (const char *)data;

    /* Process line by line. SSE lines end with \n.
     * We accumulate into a static line buffer. */
    static char line[CLAUDE_MAX_RESPONSE];
    static int line_pos = 0;

    for (uint32_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n') {
            line[line_pos] = '\0';

            /* Process completed line */
            if (line_pos >= 6 && line[0] == 'd' && line[1] == 'a' &&
                line[2] == 't' && line[3] == 'a' && line[4] == ':' &&
                line[5] == ' ') {
                /* "data: {...}" */
                const char *json = line + 6;

                /* Check event type */
                int type_len = 0;
                const char *type = json_find_str(json, "type", &type_len);

                if (type && type_len == 20 &&
                    type[0] == 'c' && type[8] == 'b' &&
                    type[14] == 'd' && type[19] == 'a') {
                    /* "content_block_delta" — extract the text from
                     * delta.text (skip the "type":"text_delta" match) */
                    int text_len = 0;
                    const char *text = json_find_str(json, "text", &text_len);
                    /* First hit is "text_delta" — skip it */
                    if (text && text_len > 4 && text[4] == '_') {
                        text = json_find_str(text + text_len + 1, "text", &text_len);
                    }
                    /* Second hit might be delta.type="text_delta" again — skip */
                    if (text && text_len > 4 && text[4] == '_') {
                        text = json_find_str(text + text_len + 1, "text", &text_len);
                    }
                    if (text && text_len > 0) {
                        /* Unescape the text in place to a temp buffer */
                        char decoded[2048];
                        int dlen = 0;
                        for (int j = 0; j < text_len && dlen < (int)sizeof(decoded) - 1; j++) {
                            if (text[j] == '\\' && j + 1 < text_len) {
                                j++;
                                switch (text[j]) {
                                    case 'n': decoded[dlen++] = '\n'; break;
                                    case 'r': decoded[dlen++] = '\r'; break;
                                    case 't': decoded[dlen++] = '\t'; break;
                                    case '"': decoded[dlen++] = '"'; break;
                                    case '\\': decoded[dlen++] = '\\'; break;
                                    default: decoded[dlen++] = text[j]; break;
                                }
                            } else {
                                decoded[dlen++] = text[j];
                            }
                        }

                        sc->total_len += dlen;
                        if (sc->callback) {
                            if (sc->callback(decoded, (uint32_t)dlen, sc->ctx) < 0) {
                                sc->error = true;
                                return -1;
                            }
                        }
                    }
                }
            }

            line_pos = 0;
        } else if (c != '\r' && line_pos < (int)sizeof(line) - 1) {
            line[line_pos++] = c;
        }
    }

    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

int claude_chat(const claude_msg_t *messages, int msg_count,
                const char *model, int max_tokens,
                claude_stream_cb callback, void *ctx)
{
    if (!api_key_set) {
        serial_puts("[CLAUDE] No API key set\n");
        return -1;
    }

    /* Build request JSON */
    char *json_buf = (char *)kmalloc(4096);
    if (!json_buf) return -1;

    int json_len = build_request_json(json_buf, 4096,
                                       messages, msg_count,
                                       model, max_tokens, true);

    serial_puts("[CLAUDE] Request: ");
    serial_putdec((uint64_t)json_len);
    serial_puts(" bytes JSON\n");

    /* Allocate HTTP session + response */
    void *session = kmalloc(http_session_size());
    void *resp = kmalloc(http_response_size());
    if (!session || !resp) {
        if (session) kfree(session);
        if (resp) kfree(resp);
        kfree(json_buf);
        return -1;
    }

    /* Open HTTPS connection */
    if (http_open(session, CLAUDE_API_HOST) < 0) {
        kfree(resp);
        kfree(session);
        kfree(json_buf);
        return -1;
    }

    /* Build headers */
    char auth_hdr[300];
    int ap = 0;
    ap = jp(auth_hdr, ap, (int)sizeof(auth_hdr), "x-api-key: ");
    ap = jp(auth_hdr, ap, (int)sizeof(auth_hdr), api_key);
    auth_hdr[ap] = '\0';

    const char *headers[] = {
        "Content-Type: application/json",
        auth_hdr,
        "anthropic-version: " CLAUDE_API_VERSION,
        "Connection: close",
        NULL
    };

    /* Send request */
    if (http_request(session, "POST", CLAUDE_API_PATH, CLAUDE_API_HOST,
                     headers, json_buf, (uint32_t)json_len, resp) < 0) {
        serial_puts("[CLAUDE] Request failed\n");
        http_close(session);
        kfree(resp);
        kfree(session);
        kfree(json_buf);
        return -1;
    }

    kfree(json_buf);  /* No longer needed */

    /* Check HTTP status */
    /* resp is opaque from shell, but we know http_response_t layout */
    int status = *(int *)resp;  /* status_code is first field */
    if (status != 200) {
        serial_puts("[CLAUDE] HTTP ");
        serial_putdec((uint64_t)status);
        serial_puts("\n");

        /* Read error body for diagnostics */
        char err_buf[512];
        int err_n = http_read_body_full(session, resp, err_buf, sizeof(err_buf) - 1);
        if (err_n > 0) {
            err_buf[err_n] = '\0';
            serial_puts("[CLAUDE] Error: ");
            /* Show first 200 chars */
            for (int i = 0; i < err_n && i < 200; i++) {
                char s[2] = {err_buf[i], 0};
                serial_puts(s);
            }
            serial_puts("\n");
        }
        http_close(session);
        kfree(resp);
        kfree(session);
        return -1;
    }

    /* Parse streaming response */
    /* Reset SSE line buffer */
    sse_ctx_t sc = {
        .callback = callback,
        .ctx = ctx,
        .total_len = 0,
        .error = false,
    };

    /* Reset static line buffer in sse_body_cb */
    /* (it's function-static, resets when line_pos = 0 at newline) */

    http_read_body(session, resp, sse_body_cb, &sc);

    serial_puts("\n[CLAUDE] Done (");
    serial_putdec((uint64_t)sc.total_len);
    serial_puts(" chars)\n");

    http_close(session);
    kfree(resp);
    kfree(session);

    return sc.error ? -1 : sc.total_len;
}

/* ── Convenience: single-turn ────────────────────────────────── */

typedef struct {
    char     *buf;
    uint32_t  size;
    uint32_t  pos;
} ask_ctx_t;

static int ask_cb(const char *text, uint32_t len, void *ctx)
{
    ask_ctx_t *a = (ask_ctx_t *)ctx;
    for (uint32_t i = 0; i < len && a->pos < a->size - 1; i++)
        a->buf[a->pos++] = text[i];
    return 0;
}

int claude_ask(const char *prompt, char *response_buf, uint32_t buf_size)
{
    claude_msg_t msg = { .role = "user", .content = prompt };
    ask_ctx_t ctx = { .buf = response_buf, .size = buf_size, .pos = 0 };

    int r = claude_chat(&msg, 1, NULL, 1024, ask_cb, &ctx);
    if (r < 0) return -1;

    if (ctx.pos < ctx.size)
        ctx.buf[ctx.pos] = '\0';

    return (int)ctx.pos;
}
