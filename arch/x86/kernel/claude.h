/*
 * OsitoK x86-64 — Claude API Client
 *
 * X-CL1: HTTP client for Anthropic Messages API.
 * Supports streaming (SSE) and non-streaming responses.
 */

#ifndef OSITOK_CLAUDE_H
#define OSITOK_CLAUDE_H

#include "../include/types.h"

/* ── Configuration ──────────────────────────────────────────── */

#define CLAUDE_MAX_MESSAGES     16
#define CLAUDE_MAX_MSG_LEN      2048
#define CLAUDE_MAX_RESPONSE     8192
#define CLAUDE_API_HOST         "api.anthropic.com"
#define CLAUDE_API_PATH         "/v1/messages"
#define CLAUDE_API_VERSION      "2023-06-01"
#define CLAUDE_DEFAULT_MODEL    "claude-sonnet-4-20250514"

/* ── Message Types ──────────────────────────────────────────── */

typedef struct {
    const char *role;       /* "user" or "assistant" */
    const char *content;    /* Text content */
} claude_msg_t;

/* Streaming callback: called with each text chunk as it arrives.
 * Return 0 to continue, -1 to abort. */
typedef int (*claude_stream_cb)(const char *text, uint32_t len, void *ctx);

/* ── API ────────────────────────────────────────────────────── */

/* Set API key (stored in static buffer, persists across calls).
 * Must be called before claude_chat(). */
void claude_set_api_key(const char *key);

/* Get current API key (NULL if not set). */
const char *claude_get_api_key(void);

/* Send a message and stream the response.
 * messages: array of messages (conversation history)
 * msg_count: number of messages
 * model: model name (NULL for default)
 * max_tokens: max response tokens (0 for default 1024)
 * callback: called with each text chunk (NULL = collect into buffer)
 * ctx: passed to callback
 * Returns: total response text length, or -1 on error. */
int claude_chat(const claude_msg_t *messages, int msg_count,
                const char *model, int max_tokens,
                claude_stream_cb callback, void *ctx);

/* Convenience: single-turn chat, response written to buf.
 * Returns bytes written to buf, or -1 on error. */
int claude_ask(const char *prompt, char *response_buf, uint32_t buf_size);

/* ── Multi-Turn Session (X-CL2) ───────────────────────────────── */

#define CLAUDE_SESSION_MAX_TURNS  8   /* Max user+assistant turn pairs */

typedef struct {
    char     user[CLAUDE_SESSION_MAX_TURNS][CLAUDE_MAX_MSG_LEN];
    char     assistant[CLAUDE_SESSION_MAX_TURNS][CLAUDE_MAX_MSG_LEN];
    int      turn_count;     /* Number of completed turns */
    char     pending_response[CLAUDE_MAX_RESPONSE]; /* Current response accumulator */
    uint32_t response_pos;
} claude_session_t;

/* Allocate and initialize a session (kmalloc). Caller must free. */
claude_session_t *claude_session_new(void);

/* Free a session */
void claude_session_free(claude_session_t *s);

/* Send a user message within a session (multi-turn).
 * Appends user message to history, streams response via callback,
 * saves assistant response to history for next turn.
 * Returns total response length, or -1 on error. */
int claude_session_send(claude_session_t *s, const char *user_msg,
                         claude_stream_cb callback, void *ctx);

/* Clear session history (restart conversation) */
void claude_session_clear(claude_session_t *s);

#endif /* OSITOK_CLAUDE_H */
