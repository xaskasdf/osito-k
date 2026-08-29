/*
 * agent_simple.c — single-worker agent task queue for osito-k.
 *
 * Implements the agent_submit_slot / agent_slot_wait /
 * agent_read_response_slot API that inferconnect_rpc.c calls when
 * a remote peer delegates a task. Unlike osito-a's full agent.c
 * (which adds a UCB1 bandit + tool-use system prompt + trajectory
 * persistence), this is a straight prompt → llama_forward loop
 * with greedy argmax sampling and no agentic loop — but it IS
 * real: the worker kthread processes submissions end to end and
 * publishes the generated text to the response buffer.
 *
 * When osito-k grows a richer agent loop, drop this file and
 * port the full agent.c from osito-a.
 */

#include "../include/types.h"
#include "inference.h"
#include "tokenizer.h"

#define AGENT_N_SLOTS      4
#define AGENT_PROMPT_CAP   2048
#define AGENT_RESPONSE_CAP 2048

typedef struct {
    volatile uint32_t pending;     /* 1 = work to do */
    volatile uint32_t done;        /* 1 = response ready */
    volatile uint64_t task_id;     /* monotonic */
    int       failed;
    uint32_t  max_tokens;
    uint32_t  prompt_len;
    uint32_t  response_len;
    char      prompt[AGENT_PROMPT_CAP];
    char      response[AGENT_RESPONSE_CAP];
} agent_slot_t;

static agent_slot_t g_slots[AGENT_N_SLOTS];
static volatile uint64_t g_next_task_id = 1;
static volatile bool g_worker_started = false;
static volatile bool g_agent_ready = false;

extern llama_state_t *prompt_llama;
extern char g_tokenizer[];
extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern int  kthread_create(const char *name, void (*func)(void *), void *data);
extern void sched_yield(void);
extern int sched_sleep_ticks(uint64_t ticks);
extern int  llama_forward(llama_state_t *state, uint32_t token);
extern bool tok_is_ready(const tokenizer_t *tok);

/* Llama 3 chat-template wrap of the bare user prompt. Mirrors what
 * main.c::prompt_handler does so remote tasks land in the same
 * format as local prompts. */
static int wrap_chat_template(const char *user, uint32_t user_len,
                              uint32_t *tokens, uint32_t cap)
{
    uint32_t n = 0;
    if (cap < 16) return -1;
    tokens[n++] = 128000;  /* <|begin_of_text|>             */
    tokens[n++] = 128006;  /* <|start_header_id|>           */
    int r = tok_encode((tokenizer_t *)g_tokenizer, "user", 4, tokens + n, cap - n);
    if (r > 0) n += (uint32_t)r;
    tokens[n++] = 128007;  /* <|end_header_id|>             */
    r = tok_encode((tokenizer_t *)g_tokenizer, "\n\n", 2, tokens + n, cap - n);
    if (r > 0) n += (uint32_t)r;
    r = tok_encode((tokenizer_t *)g_tokenizer, user, user_len,
                   tokens + n, cap - n);
    if (r > 0) n += (uint32_t)r;
    tokens[n++] = 128009;  /* <|eot_id|>                    */
    tokens[n++] = 128006;  /* <|start_header_id|>           */
    r = tok_encode((tokenizer_t *)g_tokenizer, "assistant", 9,
                   tokens + n, cap - n);
    if (r > 0) n += (uint32_t)r;
    tokens[n++] = 128007;  /* <|end_header_id|>             */
    r = tok_encode((tokenizer_t *)g_tokenizer, "\n\n", 2, tokens + n, cap - n);
    if (r > 0) n += (uint32_t)r;
    return (int)n;
}

static uint32_t sample_argmax(const float *logits, uint32_t vocab_size)
{
    uint32_t best = 0;
    float    best_v = logits[0];
    for (uint32_t v = 1; v < vocab_size; v++) {
        if (logits[v] > best_v) { best_v = logits[v]; best = v; }
    }
    return best;
}

static void process_slot(agent_slot_t *sl)
{
    sl->failed = 0;
    sl->response_len = 0;

    if (!prompt_llama) { sl->failed = 1; return; }
    if (!tok_is_ready((tokenizer_t *)g_tokenizer)) { sl->failed = 2; return; }

    static uint32_t tokens[1024];
    int n = wrap_chat_template(sl->prompt, sl->prompt_len, tokens, 1024);
    if (n <= 0) { sl->failed = 3; return; }

    /* Prefill — reset to fresh context per task. */
    prompt_llama->pos = 0;
    for (int i = 0; i < n; i++) llama_forward(prompt_llama, tokens[i]);

    /* Decode loop — greedy argmax with EOT stop. */
    uint32_t generated[512];
    uint32_t ng = 0;
    uint32_t cap_tok = sl->max_tokens > 256 ? 256 : sl->max_tokens;
    for (uint32_t step = 0; step < cap_tok && ng < 512; step++) {
        uint32_t tok = sample_argmax(prompt_llama->logits,
                                     prompt_llama->vocab_size);
        if (tok == 128009 || tok == 128001) break;  /* <|eot_id|> or <|end_of_text|> */
        generated[ng++] = tok;
        llama_forward(prompt_llama, tok);
    }

    /* Detokenize in one shot. */
    int dec = tok_decode((tokenizer_t *)g_tokenizer, generated, ng,
                         sl->response, AGENT_RESPONSE_CAP - 1);
    if (dec > 0) {
        sl->response_len = (uint32_t)dec;
        sl->response[sl->response_len] = '\0';
    }
}

static void agent_worker(void *_)
{
    (void)_;
    for (;;) {
        for (uint32_t i = 0; i < AGENT_N_SLOTS; i++) {
            agent_slot_t *sl = &g_slots[i];
            if (!sl->pending) continue;
            sl->pending = 0;
            process_slot(sl);
            __sync_synchronize();
            sl->done = 1;
        }
        /* No slot event exists yet, so poll at the scheduler tick rate. */
        (void)sched_sleep_ticks(1);
    }
}

/* Init-ready gate (osito-a parity). Without this, an RPC-arrived
 * agent task that races boot can land before the worker is alive
 * and the slot's task_id collide with a later submission. */
bool agent_is_initialized(void)
{
    return g_agent_ready;
}

void agent_init(void)
{
    if (g_worker_started) return;
    g_worker_started = true;
    kthread_create("agent-worker", agent_worker, NULL);
    __sync_synchronize();
    g_agent_ready = true;
    serial_puts("[AGENT] init complete (4 slots, single worker, greedy sampler)\n");
}

/* ── Public API ──────────────────────────────────────────────── */

int64_t agent_submit_slot(uint32_t slot, const char *prompt,
                          uint32_t max_tokens, float temp)
{
    (void)temp;  /* Greedy sampler in v1 — bandit/temperature ignored. */
    if (slot >= AGENT_N_SLOTS) return -1;
    /* Refuse before init complete — osito-a's `-5` convention. The
     * caller (ic_handle_agent_task) falls through to its echo path. */
    if (!g_agent_ready) return -5;

    agent_slot_t *sl = &g_slots[slot];
    if (sl->pending) return -1;  /* slot busy */

    uint32_t plen = 0;
    while (prompt[plen] && plen < AGENT_PROMPT_CAP - 1) plen++;
    if (plen == 0) return -1;
    for (uint32_t i = 0; i < plen; i++) sl->prompt[i] = prompt[i];
    sl->prompt[plen] = '\0';
    sl->prompt_len = plen;
    sl->max_tokens = max_tokens ? max_tokens : 64;
    sl->response_len = 0;
    sl->done = 0;
    sl->failed = 0;

    uint64_t tid = __sync_add_and_fetch(&g_next_task_id, 1);
    sl->task_id = tid;
    __sync_synchronize();
    sl->pending = 1;
    return (int64_t)tid;
}

extern uint64_t idt_get_ticks(void);

int agent_slot_wait(uint32_t slot, uint64_t task_id, uint32_t timeout_ticks)
{
    if (slot >= AGENT_N_SLOTS) return -1;
    agent_slot_t *sl = &g_slots[slot];

    uint64_t deadline = idt_get_ticks() + timeout_ticks;
    while (idt_get_ticks() < deadline) {
        if (sl->task_id == task_id && sl->done) {
            return sl->failed ? -1 : 0;
        }
        sched_yield();
    }
    return -2;  /* timeout */
}

uint32_t agent_read_response_slot(uint32_t slot, char *out, uint32_t cap,
                                  uint32_t since_offset)
{
    if (slot >= AGENT_N_SLOTS || cap == 0) return 0;
    agent_slot_t *sl = &g_slots[slot];
    if (!sl->done) return 0;
    if (since_offset >= sl->response_len) return 0;
    uint32_t want = sl->response_len - since_offset;
    if (want >= cap) want = cap - 1;
    for (uint32_t i = 0; i < want; i++)
        out[i] = sl->response[since_offset + i];
    out[want] = '\0';
    return want;
}
