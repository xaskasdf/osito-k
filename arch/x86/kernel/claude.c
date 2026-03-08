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

/* OsitoFS (X-CL3 tool use) */
extern void *osfs2_find(const char *name);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern void *osfs2_create(const char *name, uint64_t size);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
extern int   osfs2_delete(const char *name);
extern uint32_t osfs2_file_count(void);
extern void    *osfs2_file_at(uint32_t index);
extern uint64_t osfs2_file_size(void *file);

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

/* Tools JSON definition (X-CL3) */
static const char *tools_json_def =
    ",\"tools\":["
    "{\"name\":\"file_read\",\"description\":\"Read a file from the OsitoK filesystem.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
    "\"description\":\"File name\"}},\"required\":[\"path\"]}},"
    "{\"name\":\"file_write\",\"description\":\"Write content to a file (creates or overwrites).\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
    "\"description\":\"File name\"},\"content\":{\"type\":\"string\","
    "\"description\":\"Content to write\"}},\"required\":[\"path\",\"content\"]}},"
    "{\"name\":\"file_list\",\"description\":\"List all files on the OsitoK filesystem.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{}}}"
    "]";

/* Build Claude API request JSON.
 * If with_tools, appends tools definitions.
 * If content starts with '[', it's treated as raw JSON (for tool_use/tool_result). */
static int build_request_json(char *buf, int buf_size,
                               const claude_msg_t *messages, int msg_count,
                               const char *model, int max_tokens, bool stream,
                               bool with_tools)
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
        if (messages[i].content[0] == '[')
            p = jp(buf, p, mx, messages[i].content);  /* Raw JSON array */
        else
            p = jp_str(buf, p, mx, messages[i].content);
        p = jp(buf, p, mx, "}");
    }
    p = jp(buf, p, mx, "]");

    if (with_tools)
        p = jp(buf, p, mx, tools_json_def);

    p = jp(buf, p, mx, "}");

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
    claude_stream_cb     callback;
    void                *ctx;
    int                  total_len;
    bool                 error;
    claude_tool_state_t *tool_state;  /* Non-NULL when tools enabled (X-CL3) */
} sse_ctx_t;

/* JSON-unescape src[0..src_len) into dst, return decoded length */
static int json_unescape(const char *src, int src_len, char *dst, int dst_max)
{
    int dlen = 0;
    for (int j = 0; j < src_len && dlen < dst_max - 1; j++) {
        if (src[j] == '\\' && j + 1 < src_len) {
            j++;
            switch (src[j]) {
                case 'n':  dst[dlen++] = '\n'; break;
                case 'r':  dst[dlen++] = '\r'; break;
                case 't':  dst[dlen++] = '\t'; break;
                case '"':  dst[dlen++] = '"';  break;
                case '\\': dst[dlen++] = '\\'; break;
                default:   dst[dlen++] = src[j]; break;
            }
        } else {
            dst[dlen++] = src[j];
        }
    }
    return dlen;
}

static int sse_body_cb(const void *data, uint32_t len, void *ctx)
{
    sse_ctx_t *sc = (sse_ctx_t *)ctx;
    const char *buf = (const char *)data;

    static char line[CLAUDE_MAX_RESPONSE];
    static int line_pos = 0;

    for (uint32_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n') {
            line[line_pos] = '\0';

            if (line_pos >= 6 && line[0] == 'd' && line[1] == 'a' &&
                line[2] == 't' && line[3] == 'a' && line[4] == ':' &&
                line[5] == ' ') {
                const char *json = line + 6;

                int type_len = 0;
                const char *type = json_find_str(json, "type", &type_len);

                /* ── content_block_delta (type_len=19, pos14='d') ── */
                if (type && type_len == 19 &&
                    type[0] == 'c' && type[8] == 'b' &&
                    type[14] == 'd' && type[18] == 'a') {
                    /* Find delta type (second "type" field) */
                    int dt_len = 0;
                    const char *dt = json_find_str(type + type_len, "type", &dt_len);

                    if (dt && dt_len == 10 && dt[0] == 't' && dt[5] == 'd') {
                        /* "text_delta" — extract text */
                        int text_len = 0;
                        const char *text = json_find_str(dt + dt_len, "text", &text_len);
                        if (text && text_len > 0) {
                            char decoded[2048];
                            int dlen = json_unescape(text, text_len,
                                                      decoded, (int)sizeof(decoded));
                            sc->total_len += dlen;
                            if (sc->callback) {
                                if (sc->callback(decoded, (uint32_t)dlen, sc->ctx) < 0) {
                                    sc->error = true;
                                    return -1;
                                }
                            }
                        }
                    } else if (dt && dt_len == 16 && dt[0] == 'i' && dt[6] == 'j') {
                        /* "input_json_delta" — accumulate tool input (X-CL3) */
                        if (sc->tool_state && sc->tool_state->cur_tool >= 0) {
                            int pj_len = 0;
                            const char *pj = json_find_str(json, "partial_json", &pj_len);
                            if (pj && pj_len > 0) {
                                claude_tool_use_t *tu =
                                    &sc->tool_state->uses[sc->tool_state->cur_tool];
                                int dlen = json_unescape(pj, pj_len,
                                    tu->input_json + tu->input_len,
                                    CLAUDE_MAX_TOOL_INPUT - (int)tu->input_len);
                                tu->input_len += (uint32_t)dlen;
                            }
                        }
                    }
                }

                /* ── content_block_start (type_len=19, pos14='s') ── */
                else if (type && type_len == 19 &&
                         type[0] == 'c' && type[8] == 'b' &&
                         type[14] == 's' && type[18] == 't') {
                    if (sc->tool_state) {
                        /* Check inner type for "tool_use" */
                        int t2_len = 0;
                        const char *t2 = json_find_str(type + type_len, "type", &t2_len);
                        if (t2 && t2_len == 8 && t2[0] == 't' && t2[5] == 'u') {
                            /* "tool_use" — extract id and name */
                            int idx = sc->tool_state->count;
                            if (idx < CLAUDE_MAX_TOOL_USES) {
                                int id_len = 0, nm_len = 0;
                                const char *id = json_find_str(json, "id", &id_len);
                                const char *nm = json_find_str(json, "name", &nm_len);
                                if (id && id_len < CLAUDE_MAX_TOOL_ID) {
                                    cmemcpy(sc->tool_state->uses[idx].id, id, id_len);
                                    sc->tool_state->uses[idx].id[id_len] = '\0';
                                }
                                if (nm && nm_len < CLAUDE_MAX_TOOL_NAME) {
                                    cmemcpy(sc->tool_state->uses[idx].name, nm, nm_len);
                                    sc->tool_state->uses[idx].name[nm_len] = '\0';
                                }
                                sc->tool_state->uses[idx].input_len = 0;
                                sc->tool_state->cur_tool = idx;
                                sc->tool_state->count++;
                            }
                        }
                    }
                }

                /* ── content_block_stop (type_len=18) ── */
                else if (type && type_len == 18 &&
                         type[0] == 'c' && type[14] == 's' && type[17] == 'p') {
                    if (sc->tool_state)
                        sc->tool_state->cur_tool = -1;
                }

                /* ── message_delta (type_len=13) — stop_reason ── */
                else if (type && type_len == 13 &&
                         type[0] == 'm' && type[7] == '_') {
                    if (sc->tool_state) {
                        int sr_len = 0;
                        const char *sr = json_find_str(json, "stop_reason", &sr_len);
                        if (sr && sr_len == 8 && sr[0] == 't' && sr[5] == 'u') {
                            sc->tool_state->stop_for_tools = true;
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

/* Internal chat with optional tools + tool state */
static int claude_chat_ex(const claude_msg_t *messages, int msg_count,
                           const char *model, int max_tokens,
                           claude_stream_cb callback, void *ctx,
                           bool with_tools, claude_tool_state_t *tool_state)
{
    if (!api_key_set) {
        serial_puts("[CLAUDE] No API key set\n");
        return -1;
    }

    /* Build request JSON — scale buffer with message count */
    uint32_t json_buf_size = 4096 + (uint32_t)msg_count * 2048;
    if (with_tools) json_buf_size += 1024;  /* Tools definition */
    char *json_buf = (char *)kmalloc(json_buf_size);
    if (!json_buf) return -1;

    int json_len = build_request_json(json_buf, (int)json_buf_size,
                                       messages, msg_count,
                                       model, max_tokens, true,
                                       with_tools);

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
    sse_ctx_t sc = {
        .callback = callback,
        .ctx = ctx,
        .total_len = 0,
        .error = false,
        .tool_state = tool_state,
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

/* ── Public API (wraps internal) ─────────────────────────────── */

int claude_chat(const claude_msg_t *messages, int msg_count,
                const char *model, int max_tokens,
                claude_stream_cb callback, void *ctx)
{
    return claude_chat_ex(messages, msg_count, model, max_tokens,
                           callback, ctx, false, NULL);
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

/* ══════════════════════════════════════════════════════════════
 *  X-CL2: Multi-Turn Session
 * ══════════════════════════════════════════════════════════════ */

claude_session_t *claude_session_new(void)
{
    claude_session_t *s = (claude_session_t *)kmalloc(sizeof(claude_session_t));
    if (s) cmemset(s, 0, sizeof(*s));
    return s;
}

void claude_session_free(claude_session_t *s)
{
    if (s) kfree(s);
}

void claude_session_clear(claude_session_t *s)
{
    if (s) {
        s->turn_count = 0;
        s->response_pos = 0;
    }
}

/* Callback that captures response into session + forwards to user cb */
typedef struct {
    claude_session_t *session;
    claude_stream_cb  user_cb;
    void             *user_ctx;
} session_stream_ctx_t;

static int session_stream_cb(const char *text, uint32_t len, void *ctx)
{
    session_stream_ctx_t *sc = (session_stream_ctx_t *)ctx;
    claude_session_t *s = sc->session;

    /* Capture into pending_response */
    for (uint32_t i = 0; i < len && s->response_pos < CLAUDE_MAX_RESPONSE - 1; i++)
        s->pending_response[s->response_pos++] = text[i];

    /* Forward to user callback */
    if (sc->user_cb)
        return sc->user_cb(text, len, sc->user_ctx);
    return 0;
}

int claude_session_send(claude_session_t *s, const char *user_msg,
                         claude_stream_cb callback, void *ctx)
{
    if (!s) return -1;

    /* If history is full, shift out oldest turn to make room */
    if (s->turn_count >= CLAUDE_SESSION_MAX_TURNS) {
        for (int i = 0; i < CLAUDE_SESSION_MAX_TURNS - 1; i++) {
            cmemcpy(s->user[i], s->user[i + 1], CLAUDE_MAX_MSG_LEN);
            cmemcpy(s->assistant[i], s->assistant[i + 1], CLAUDE_MAX_MSG_LEN);
        }
        s->turn_count = CLAUDE_SESSION_MAX_TURNS - 1;
    }

    /* Save user message */
    int cur = s->turn_count;
    uint32_t ulen = cstrlen(user_msg);
    if (ulen >= CLAUDE_MAX_MSG_LEN) ulen = CLAUDE_MAX_MSG_LEN - 1;
    cmemcpy(s->user[cur], user_msg, ulen);
    s->user[cur][ulen] = '\0';

    /* Build message array from history: user0, assistant0, user1, ... userN */
    claude_msg_t messages[CLAUDE_MAX_MESSAGES];
    int msg_count = 0;

    for (int i = 0; i <= cur && msg_count < CLAUDE_MAX_MESSAGES - 1; i++) {
        messages[msg_count].role = "user";
        messages[msg_count].content = s->user[i];
        msg_count++;

        if (i < cur) {
            messages[msg_count].role = "assistant";
            messages[msg_count].content = s->assistant[i];
            msg_count++;
        }
    }

    /* Reset response accumulator */
    s->response_pos = 0;

    /* Stream request with capture callback */
    session_stream_ctx_t sc = {
        .session  = s,
        .user_cb  = callback,
        .user_ctx = ctx,
    };

    int r = claude_chat(messages, msg_count, NULL, 2048,
                         session_stream_cb, &sc);

    /* Save response to history */
    s->pending_response[s->response_pos] = '\0';
    cmemcpy(s->assistant[cur], s->pending_response,
             s->response_pos < CLAUDE_MAX_MSG_LEN ? s->response_pos + 1
                                                    : CLAUDE_MAX_MSG_LEN);
    s->assistant[cur][CLAUDE_MAX_MSG_LEN - 1] = '\0';
    s->turn_count = cur + 1;

    return r;
}

/* ══════════════════════════════════════════════════════════════
 *  X-CL3: Tool Use — File Read/Write/List
 * ══════════════════════════════════════════════════════════════ */

/* Extract JSON string value by key from tool input (unescaped) */
static int tool_get_param(const char *input_json, const char *key,
                           char *out, int out_max)
{
    int vlen = 0;
    const char *v = json_find_str(input_json, key, &vlen);
    if (!v || vlen <= 0) return -1;
    int dlen = json_unescape(v, vlen, out, out_max);
    out[dlen] = '\0';
    return dlen;
}

/* Tool: file_read — read file from OsitoFS */
static int tool_file_read(const char *input_json, char *result, int max_len)
{
    char path[128];
    if (tool_get_param(input_json, "path", path, sizeof(path)) < 0) {
        int p = 0;
        p = jp(result, p, max_len, "Error: missing 'path' parameter");
        return p;
    }

    void *file = osfs2_find(path);
    if (!file) {
        int p = 0;
        p = jp(result, p, max_len, "Error: file not found: ");
        p = jp(result, p, max_len, path);
        return p;
    }

    uint64_t fsize = osfs2_file_size(file);
    uint32_t read_size = (uint32_t)(fsize < (uint64_t)(max_len - 1) ? fsize : (uint64_t)(max_len - 1));

    if (osfs2_read(file, 0, result, read_size) < 0) {
        int p = 0;
        p = jp(result, p, max_len, "Error: read failed for: ");
        p = jp(result, p, max_len, path);
        return p;
    }

    result[read_size] = '\0';
    serial_puts("[TOOL] file_read: ");
    serial_puts(path);
    serial_puts(" (");
    serial_putdec(read_size);
    serial_puts(" bytes)\n");
    return (int)read_size;
}

/* Tool: file_write — write/create file on OsitoFS */
static int tool_file_write(const char *input_json, char *result, int max_len)
{
    char path[128];
    if (tool_get_param(input_json, "path", path, sizeof(path)) < 0) {
        int p = 0;
        p = jp(result, p, max_len, "Error: missing 'path' parameter");
        return p;
    }

    /* Get content — may be large, allocate temp buffer */
    char *content = (char *)kmalloc(CLAUDE_MAX_TOOL_RESULT);
    if (!content) {
        int p = 0;
        p = jp(result, p, max_len, "Error: out of memory");
        return p;
    }

    int clen = tool_get_param(input_json, "content", content, CLAUDE_MAX_TOOL_RESULT);
    if (clen < 0) {
        kfree(content);
        int p = 0;
        p = jp(result, p, max_len, "Error: missing 'content' parameter");
        return p;
    }

    /* Find or create file */
    void *file = osfs2_find(path);
    if (!file) {
        file = osfs2_create(path, (uint64_t)clen);
        if (!file) {
            kfree(content);
            int p = 0;
            p = jp(result, p, max_len, "Error: failed to create file: ");
            p = jp(result, p, max_len, path);
            return p;
        }
    }

    if (osfs2_write(file, 0, content, (uint64_t)clen) < 0) {
        kfree(content);
        int p = 0;
        p = jp(result, p, max_len, "Error: write failed for: ");
        p = jp(result, p, max_len, path);
        return p;
    }

    kfree(content);

    int p = 0;
    p = jp(result, p, max_len, "Wrote ");
    p = jp_int(result, p, max_len, clen);
    p = jp(result, p, max_len, " bytes to ");
    p = jp(result, p, max_len, path);

    serial_puts("[TOOL] file_write: ");
    serial_puts(path);
    serial_puts(" (");
    serial_putdec((uint64_t)clen);
    serial_puts(" bytes)\n");
    return p;
}

/* Tool: file_list — list all files on OsitoFS */
static int tool_file_list(char *result, int max_len)
{
    uint32_t count = osfs2_file_count();
    int p = 0;
    p = jp(result, p, max_len, "Files on OsitoFS (");
    p = jp_int(result, p, max_len, (int)count);
    p = jp(result, p, max_len, " files):\n");

    for (uint32_t i = 0; i < count && p < max_len - 64; i++) {
        void *file = osfs2_file_at(i);
        if (!file) break;
        /* File name is at offset 0 of osfs2_file_t */
        const char *name = (const char *)file;
        uint64_t size = osfs2_file_size(file);
        p = jp(result, p, max_len, "  ");
        p = jp(result, p, max_len, name);
        p = jp(result, p, max_len, "  (");
        if (size >= 1024 * 1024) {
            p = jp_int(result, p, max_len, (int)(size / (1024 * 1024)));
            p = jp(result, p, max_len, " MB)\n");
        } else if (size >= 1024) {
            p = jp_int(result, p, max_len, (int)(size / 1024));
            p = jp(result, p, max_len, " KB)\n");
        } else {
            p = jp_int(result, p, max_len, (int)size);
            p = jp(result, p, max_len, " B)\n");
        }
    }

    serial_puts("[TOOL] file_list: ");
    serial_putdec(count);
    serial_puts(" files\n");
    return p;
}

/* Execute a single tool and return result length */
static int tool_execute(const claude_tool_use_t *tu, char *result, int max_len)
{
    result[0] = '\0';

    if (tu->name[0] == 'f' && tu->name[5] == 'r')  /* file_read */
        return tool_file_read(tu->input_json, result, max_len);
    if (tu->name[0] == 'f' && tu->name[5] == 'w')  /* file_write */
        return tool_file_write(tu->input_json, result, max_len);
    if (tu->name[0] == 'f' && tu->name[5] == 'l')  /* file_list */
        return tool_file_list(result, max_len);

    int p = 0;
    p = jp(result, p, max_len, "Error: unknown tool: ");
    p = jp(result, p, max_len, tu->name);
    return p;
}

/* Build assistant content JSON: [{"type":"text","text":"..."},{"type":"tool_use",...}] */
static int build_assistant_content(const char *text, uint32_t text_len,
                                    const claude_tool_state_t *ts,
                                    char *buf, int buf_size)
{
    int p = 0, mx = buf_size;
    p = jp(buf, p, mx, "[");

    /* Text block (if any) */
    if (text_len > 0) {
        p = jp(buf, p, mx, "{\"type\":\"text\",\"text\":");
        /* Need a null-terminated copy for jp_str */
        char *tcopy = (char *)kmalloc(text_len + 1);
        if (tcopy) {
            cmemcpy(tcopy, text, text_len);
            tcopy[text_len] = '\0';
            p = jp_str(buf, p, mx, tcopy);
            kfree(tcopy);
        } else {
            p = jp_str(buf, p, mx, "...");
        }
        p = jp(buf, p, mx, "}");
    }

    /* Tool use blocks */
    for (int i = 0; i < ts->count; i++) {
        if (text_len > 0 || i > 0) p = jp(buf, p, mx, ",");
        p = jp(buf, p, mx, "{\"type\":\"tool_use\",\"id\":");
        p = jp_str(buf, p, mx, ts->uses[i].id);
        p = jp(buf, p, mx, ",\"name\":");
        p = jp_str(buf, p, mx, ts->uses[i].name);
        p = jp(buf, p, mx, ",\"input\":");
        /* Input is already valid JSON */
        if (ts->uses[i].input_len > 0) {
            p = jp(buf, p, mx, ts->uses[i].input_json);
        } else {
            p = jp(buf, p, mx, "{}");
        }
        p = jp(buf, p, mx, "}");
    }

    p = jp(buf, p, mx, "]");
    if (p < mx) buf[p] = '\0';
    return p;
}

/* Build tool_result content JSON: [{"type":"tool_result","tool_use_id":"...","content":"..."},...] */
static int build_tool_result_content(const claude_tool_state_t *ts,
                                      char **results, int *result_lens,
                                      char *buf, int buf_size)
{
    int p = 0, mx = buf_size;
    p = jp(buf, p, mx, "[");

    for (int i = 0; i < ts->count; i++) {
        if (i > 0) p = jp(buf, p, mx, ",");
        p = jp(buf, p, mx, "{\"type\":\"tool_result\",\"tool_use_id\":");
        p = jp_str(buf, p, mx, ts->uses[i].id);
        p = jp(buf, p, mx, ",\"content\":");
        /* Truncate result for safety */
        char *r = results[i];
        r[result_lens[i]] = '\0';
        p = jp_str(buf, p, mx, r);
        p = jp(buf, p, mx, "}");
    }

    p = jp(buf, p, mx, "]");
    if (p < mx) buf[p] = '\0';
    return p;
}

/* Session send with tool support (X-CL3) */
int claude_session_send_with_tools(claude_session_t *s, const char *user_msg,
                                    claude_stream_cb callback, void *ctx)
{
    if (!s) return -1;

    /* Sliding window (same as claude_session_send) */
    if (s->turn_count >= CLAUDE_SESSION_MAX_TURNS) {
        for (int i = 0; i < CLAUDE_SESSION_MAX_TURNS - 1; i++) {
            cmemcpy(s->user[i], s->user[i + 1], CLAUDE_MAX_MSG_LEN);
            cmemcpy(s->assistant[i], s->assistant[i + 1], CLAUDE_MAX_MSG_LEN);
        }
        s->turn_count = CLAUDE_SESSION_MAX_TURNS - 1;
    }

    /* Save user message */
    int cur = s->turn_count;
    uint32_t ulen = cstrlen(user_msg);
    if (ulen >= CLAUDE_MAX_MSG_LEN) ulen = CLAUDE_MAX_MSG_LEN - 1;
    cmemcpy(s->user[cur], user_msg, ulen);
    s->user[cur][ulen] = '\0';

    /* Build initial message array from history */
    claude_msg_t messages[CLAUDE_MAX_MESSAGES];
    int msg_count = 0;

    for (int i = 0; i <= cur && msg_count < CLAUDE_MAX_MESSAGES - 4; i++) {
        messages[msg_count].role = "user";
        messages[msg_count].content = s->user[i];
        msg_count++;

        if (i < cur) {
            messages[msg_count].role = "assistant";
            messages[msg_count].content = s->assistant[i];
            msg_count++;
        }
    }

    s->response_pos = 0;

    session_stream_ctx_t sc = {
        .session  = s,
        .user_cb  = callback,
        .user_ctx = ctx,
    };

    /* Tool state */
    claude_tool_state_t ts;
    cmemset(&ts, 0, sizeof(ts));
    ts.cur_tool = -1;

    int r = claude_chat_ex(messages, msg_count, NULL, 2048,
                            session_stream_cb, &sc, true, &ts);

    /* Tool loop — max 5 iterations */
    int base_msg_count = msg_count;  /* Save for rebuilding */
    char *asst_content = NULL;
    char *tr_content = NULL;

    for (int iter = 0; iter < 5 && ts.stop_for_tools && ts.count > 0; iter++) {
        serial_puts("[CLAUDE] Tool use round ");
        serial_putdec((uint64_t)(iter + 1));
        serial_puts(": ");
        serial_putdec((uint64_t)ts.count);
        serial_puts(" tool(s)\n");

        /* Show tool names to user */
        for (int t = 0; t < ts.count; t++) {
            if (callback) {
                callback("\n[tool: ", 8, ctx);
                callback(ts.uses[t].name, cstrlen(ts.uses[t].name), ctx);
                callback("]\n", 2, ctx);
            }
        }

        /* Execute tools */
        char *results[CLAUDE_MAX_TOOL_USES];
        int result_lens[CLAUDE_MAX_TOOL_USES];
        bool exec_ok = true;

        for (int t = 0; t < ts.count; t++) {
            results[t] = (char *)kmalloc(CLAUDE_MAX_TOOL_RESULT);
            if (!results[t]) { exec_ok = false; break; }
            ts.uses[t].input_json[ts.uses[t].input_len] = '\0';
            result_lens[t] = tool_execute(&ts.uses[t], results[t],
                                           CLAUDE_MAX_TOOL_RESULT - 1);
        }

        if (!exec_ok) {
            for (int t = 0; t < ts.count; t++)
                if (results[t]) kfree(results[t]);
            break;
        }

        /* Build assistant content (text + tool_use blocks) */
        if (asst_content) kfree(asst_content);
        asst_content = (char *)kmalloc(16384);
        if (!asst_content) {
            for (int t = 0; t < ts.count; t++) kfree(results[t]);
            break;
        }
        s->pending_response[s->response_pos] = '\0';
        build_assistant_content(s->pending_response, s->response_pos,
                                 &ts, asst_content, 16384);

        /* Build tool_result content */
        if (tr_content) kfree(tr_content);
        tr_content = (char *)kmalloc(16384);
        if (!tr_content) {
            for (int t = 0; t < ts.count; t++) kfree(results[t]);
            break;
        }
        build_tool_result_content(&ts, results, result_lens, tr_content, 16384);

        for (int t = 0; t < ts.count; t++) kfree(results[t]);

        /* Rebuild messages: history + assistant (tool_use) + user (tool_result) */
        msg_count = base_msg_count;
        if (msg_count < CLAUDE_MAX_MESSAGES - 2) {
            messages[msg_count].role = "assistant";
            messages[msg_count].content = asst_content;
            msg_count++;
            messages[msg_count].role = "user";
            messages[msg_count].content = tr_content;
            msg_count++;
        }

        /* Reset for next round */
        cmemset(&ts, 0, sizeof(ts));
        ts.cur_tool = -1;
        s->response_pos = 0;

        if (callback) callback("\nClaude: ", 9, ctx);

        r = claude_chat_ex(messages, msg_count, NULL, 2048,
                            session_stream_cb, &sc, true, &ts);
    }

    if (asst_content) kfree(asst_content);
    if (tr_content) kfree(tr_content);

    /* Save final response to history */
    s->pending_response[s->response_pos] = '\0';
    cmemcpy(s->assistant[cur], s->pending_response,
             s->response_pos < CLAUDE_MAX_MSG_LEN ? s->response_pos + 1
                                                    : CLAUDE_MAX_MSG_LEN);
    s->assistant[cur][CLAUDE_MAX_MSG_LEN - 1] = '\0';
    s->turn_count = cur + 1;

    return r;
}
