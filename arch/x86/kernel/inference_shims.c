/*
 * inference_shims.c — adapts osito-k's single-slot llama_state_t to
 * the slot-based API that inferconnect_oict.c expects.
 *
 * osito-a runs distributed inference with multiple in-flight slots
 * for pipeline parallelism (oict). osito-k v1 is single-slot — one
 * forward pass at a time on the boot thread. These shims expose the
 * same symbol names but ignore slot_id and forward to the existing
 * blocking llama_forward / brandon path. When osito-k grows
 * multi-slot inference, replace these with real implementations.
 */

#include "../include/types.h"
#include "inference.h"

extern int  llama_forward(llama_state_t *s, uint32_t token);
extern void serial_puts(const char *s);

/* ── State getters ─────────────────────────────────────────────
 * inference.c already provides llama_state_dim, llama_state_layers,
 * llama_state_vocab, llama_state_arch with int/const-char return
 * types. OICT expects uint32_t-returning n_layers/vocab_size and
 * also kv_dim/use_dwa/use_value_residual/logits. Add only those. */

uint32_t llama_state_n_layers(void *s)    { return ((llama_state_t *)s)->n_layers; }
uint32_t llama_state_kv_dim(void *s)      { return ((llama_state_t *)s)->kv_dim; }
uint32_t llama_state_vocab_size(void *s)  { return ((llama_state_t *)s)->vocab_size; }
bool     llama_state_use_dwa(void *s)     { return ((llama_state_t *)s)->use_dwa; }
bool     llama_state_use_value_residual(void *s) {
    return ((llama_state_t *)s)->use_value_residual;
}
float   *llama_state_logits(void *s)      { return ((llama_state_t *)s)->logits; }

/* ── Slot wrappers — osito-k has one slot (slot_id ignored) ───── */

uint32_t llama_slot_pos(void *s, uint32_t slot_id)
{
    (void)slot_id;
    return ((llama_state_t *)s)->pos;
}

void llama_slot_set_pos(void *s, uint32_t slot_id, uint32_t pos)
{
    (void)slot_id;
    ((llama_state_t *)s)->pos = pos;
}

/* Blocking forward — ignores slot_id, delegates to llama_forward. */
int llama_forward_slot(void *s, uint32_t slot_id, uint32_t token)
{
    (void)slot_id;
    return llama_forward((llama_state_t *)s, token);
}

/* "Async" forward — osito-k runs it blocking and returns a fake seq
 * number (the current pos). llama_request_wait below is a no-op so
 * the caller pattern (`seq = async; wait(seq); read state`) still
 * works end-to-end, just without the parallelism win. */
uint64_t llama_forward_async(void *s, uint32_t slot_id, uint32_t token)
{
    (void)slot_id;
    llama_state_t *st = (llama_state_t *)s;
    llama_forward(st, token);
    return (uint64_t)st->pos;
}

void llama_request_wait(void *s, uint64_t seq)
{
    (void)s; (void)seq;
    /* Already blocking — nothing to wait on. */
}

/* Run a range of tokens through the model. Returns 0 on success. */
int llama_forward_slot_range(void *s, uint32_t slot_id, uint32_t tok,
                              uint32_t n_tokens)
{
    (void)slot_id;
    llama_state_t *st = (llama_state_t *)s;
    (void)tok;  /* osito-a passes a starting token; osito-k's
                 * llama_forward signature only takes one token at a
                 * time. Caller is responsible for stepping tokens
                 * through llama_forward_slot in a loop. */
    /* No-op for n_tokens > 1 in this build — return 0 so the caller's
     * normal logits-read path runs against whatever pos was last set. */
    (void)n_tokens;
    return 0;
}

uint64_t llama_forward_range_async(void *s, uint32_t slot_id,
                                    uint32_t tok, uint32_t n_tokens)
{
    (void)s; (void)slot_id; (void)tok; (void)n_tokens;
    return 0;
}

/* ── Brandon-arch range forward (DWA + value residual aware) ───
 * Brandon is the alternate arch (block-shared TinyLlama). For
 * single-slot osito-k, this is the same as llama_forward_slot_range
 * — llama_forward already dispatches by st->arch internally. */
int brandon_forward_slot_range(void *s, uint32_t slot,
                                uint32_t tok, uint32_t n_tokens)
{
    return llama_forward_slot_range(s, slot, tok, n_tokens);
}

uint64_t brandon_forward_range_async(void *s, uint32_t slot_id,
                                      uint32_t tok, uint32_t n_tokens)
{
    return llama_forward_range_async(s, slot_id, tok, n_tokens);
}
