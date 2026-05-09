# brandon-tiny integration guide (for another Claude)

You are landing brandon-tiny-10m support in a kernel/runtime that already
runs Llama-style inference. This file is the recipe — follow it in order,
the bugs surface in this order in practice.

## Files to read first (canonical sources)

| Path | What it has |
|---|---|
| `~/osito-a-models/docs/brandon-arch-spec.md` | KV pairs, tensor shapes, forward algorithm spec |
| `~/osito-a/arch/x86/fs/gguf.c` (lines ~520-850) | Reference C parser for `brandon.*` keys + SPM tokenizer extraction |
| `~/osito-a/arch/x86/kernel/inference.c` (lines ~790-1023) | Reference `brandon_forward_one` + register prefill |
| `~/osito-a/arch/x86/kernel/tokenizer.c` (lines ~120-600) | SPM encode/decode (`spm_encode`, `spm_decode_bytes`) |
| `~/osito-a-models/scripts/convert-brandon-tiny.py` | How the GGUF is produced (tells you the on-disk layout) |
| `~/osito-a-models/brandon-tiny/scripts/test_generation.py` | Reference Python sampling recipe |
| `~/osito-k/arch/x86/{fs/gguf.c,kernel/{inference,tokenizer}.c}` | Working port; commits `9fc9b1d`, `98284db`, `81c34f4`, `b4b2f0e` |
| `~/osito-k/arch/x86/kernel/tensor_avx2.c` `matvec_f16_avx2` | Native F16C path |

The model file: `~/osito-a-models/build/brandon-tiny-10m-f16.gguf` (21 MB).

## Step 1 — extend the GGUF parser

Brandon uses `general.architecture = "brandon"` and arch-prefixed keys.
Llama-only parsers silently skip them and report `layers=0 dim=0`.

Add these key handlers (all `uint32` unless noted):

```
brandon.embedding_length              # hidden dim (256)
brandon.block_count                   # *unique* transformer blocks (12)
brandon.feed_forward_length           # SwiGLU intermediate (720)
brandon.attention.head_count          # total Q heads (8)
brandon.attention.head_count_kv       # KV heads — GQA (2)
brandon.attention.layer_norm_rms_epsilon   (float32)
brandon.context_length                # max_seq_len (512)
brandon.rope.freq_base                (float32, 10000.0)
brandon.rope.dimension_count          # head_dim (32)

# Brandon-specific
brandon.compute_layer_count           # logical layers (24); ≥ block_count
brandon.layer_map                     # int32 array[compute_layer_count]
brandon.use_dwa                       # bool
brandon.use_value_residual            # bool
brandon.n_registers                   # uint32 (4 in the 10m model)
brandon.n_loops                       # uint32 (1 = no looping)
brandon.weight_tying                  # bool — output reuses token_embd
```

**Verify**: after parse, `architecture = "brandon"`, `block_count = 12`,
`compute_layer_count = 24`, `layer_map.length = 24`,
`use_dwa && use_value_residual && n_registers == 4`.

## Step 2 — fan out the layer table via layer_map

Brandon stores 12 unique block weight sets but the forward pass walks
24 logical layers. Don't iterate unique blocks directly; alias them:

```c
// Resolve once per unique block
for (b = 0; b < block_count; b++)            // 12
    blocks[b] = lookup_tensors("blk." + b + ".*");

// Fan out via layer_map
for (l = 0; l < compute_layer_count; l++)    // 24
    weights.layers[l] = blocks[layer_map[l]];
```

Two extra global tensors must also resolve:

- `register.weight` — `[n_registers, dim]` F16, only when `n_registers > 0`
- `dwa.weight` — `[n_layers, n_layers+1]` F32, only when `use_dwa`
- `output.weight` — **omitted** when `weight_tying = true`. Fall back to
  `token_embd.weight` as the LM head.

## Step 3 — THE bug to avoid: F16 matvec must exist

This is the highest-leverage trap. **Brandon ships every layer weight
as F16** (norms + dwa stay F32). If your matvec dispatcher looks like

```c
case Q4_0:  …
case Q8_0:  …
case F32:   …
default:    memset(out, 0, rows * sizeof(float));
            break;
```

…then your transformer runs on **zero weights**. The model produces
flat-distribution Wikipedia-vocab tokens, no NaN, no crash, no signal
that anything is wrong. Easy to misread as "this 10M model is just bad."

**Fix** — explicit F16 case (osito-k commit `98284db`):

```c
case GGML_TYPE_F16: {
    const uint16_t *w = tensor->data;
    for (r = 0; r < rows; r++) {
        float sum = 0;
        const uint16_t *row = w + (uint64_t)r * cols;
        for (c = 0; c < cols; c++)
            sum += f16_to_f32(row[c]) * input[c];
        out[r] = sum;
    }
    break;
}
```

Native x86: use `_mm256_cvtph_ps` (F16C) for an 8-lane vectorized path.
WASM: pre-dequant F16 → F32 once at load time (osito-k commit `526c50e`)
so the F32 path takes over. Either is fine.

**Treat `default: memset(out, 0)` as a smell** — make it
`panic("unsupported tensor type")` so silent corruption surfaces as a
hard failure.

**Verify**: at greedy (temp 0) the chat must produce *something*
(not 128 empty tokens). Empty greedy output = matvec returning zeros
= argmax landing on `<pad>` = id 0 every step.

## Step 4 — three brandon-specific bits in the forward pass

Beyond the standard Llama RMSNorm → Q/K/V → RoPE → SDPA → SwiGLU
sequence, brandon adds three modifications.

### 4a. Layer dispatch via layer_map

If you aliased `weights.layers[L]` at load (step 2), the forward loop
is unchanged: `for L in 0..n_layers: run(weights.layers[L])`. Don't
iterate unique blocks.

### 4b. Value Residual Learning (per-token, NOT slot lifetime)

```c
v_first_captured = false;     // RESET at the start of EACH forward call
for (l = 0; l < n_layers; l++) {
    rmsnorm(xb, x, attn_norm);
    matvec(Q, attn_q, xb, dim, dim);
    matvec(K, attn_k, xb, kv_dim, dim);
    matvec(V, attn_v, xb, kv_dim, dim);

    if (use_value_residual) {
        if (l == 0) {
            if (!v_first_captured) {
                memcpy(v_first, V, kv_dim * sizeof(float));
                v_first_captured = true;
            }
            // Layer 0 itself uses raw V. Do NOT add v_first here.
        } else {
            for (i = 0; i < kv_dim; i++)
                V[i] += v_first[i];     // element-wise add, NO learned alpha
        }
    }
    rope(Q); rope(K);                   // RoPE on Q/K only — V stays unrotated
    // …attention + output projection + residual
}
```

**Historical bug to avoid**: persisting `v_first` across forwards
(slot lifetime instead of per-token) collapses the model into a single
repeating token. The `v_first_captured = false` reset at function
entry is load-bearing.

### 4c. DenseFormer DWA mixing (post-FFN, per-layer)

```c
if (use_dwa) memcpy(dwa_buf, x, dim * sizeof(float));   // before the loop

for (l = 0; l < n_layers; l++) {
    // …attention + FFN, x is now the post-FFN hidden state with residual
    if (use_dwa) {
        memcpy(dwa_buf + (l + 1) * dim, x, dim * sizeof(float));

        const float *w_row = dwa_weights + l * (n_layers + 1);
        memset(x, 0, dim * sizeof(float));
        for (j = 0; j <= l + 1; j++) {
            float w = w_row[j];
            if (w == 0.0f) continue;
            const float *src = dwa_buf + j * dim;
            for (i = 0; i < dim; i++) x[i] += w * src[i];
        }
    }
}
```

`dwa.weight` PyTorch shape `[n_layers, n_layers+1]`. GGUF reverses the
shape labels but bytes stay C-row-major, so `dwa[L][j] = dwa[L*25+j]`
is correct.

`dwa_buf` is per-token only. Reset (or just overwrite `dwa_buf[0]`) at
every forward call.

### 4d. Register tokens — lazy prefill before user content

The `n_registers` learnable embeddings occupy KV positions
`0..n_registers-1` before any user token. Prefill on the first chat
forward:

```c
if (n_registers > 0 && !registers_prefilled) {
    for (r = 0; r < n_registers; r++) {
        // Decode register[r] (typically F16) into x[]
        if (register_dtype == F32)
            memcpy(x, &register_weight[r * dim], dim * 4);
        else
            for (i = 0; i < dim; i++)
                x[i] = f16_to_f32(register_weight[r * dim + i]);
        forward_one(/*pos=*/pos, /*produce_logits=*/false);
        pos++;
    }
    registers_prefilled = true;
}
// ...then forward the user token at pos = n_registers
```

**Reset `registers_prefilled = false` at the start of EVERY chat call**
(not just the first one). The KV cache for slots 0..3 must hold *this*
conversation's register activations, not the previous chat's.

## Step 5 — tokenizer: SentencePiece, not byte-level BPE

Brandon's GGUF reports `tokenizer.ggml.model = "llama"` (= SPM). It
ships:

- `tokenizer.ggml.tokens` — 8192 piece strings, with `▁` (U+2581)
  marking word boundaries
- `tokenizer.ggml.scores` — float32 array length 8192
- `tokenizer.ggml.token_type` — int32 array, codes 1..6
- `tokenizer.ggml.bos_token_id` (2), `eos_token_id` (3),
  `padding_token_id` (0), `unknown_token_id`
- ChatML specials live as regular vocab entries: `<|im_start|>` at id 4,
  `<|im_end|>` at id 5

If your tokenizer only does byte-level BPE, "hello" tokenizes to ~24
tokens of garbage and output looks vocab-shaped but incoherent.

**Fix**: add an SPM path. Encoding is "longest-match against the piece
list with leading `▁` substituting for spaces." Decoding is "emit the
bytes of each piece, replacing `▁` with a single space." Reference:
`~/osito-a/arch/x86/kernel/tokenizer.c` `spm_encode` /
`spm_decode_bytes`.

Dispatch by `tokenizer.ggml.model`: "llama" + scores → SPM path; "gpt2"
or unset → BPE.

## Step 6 — sampling recipe (non-negotiable for the 10m model)

The reference `test_generation.py` uses:

```
temperature=0.7
top_k=50
top_p=0.9
repetition_penalty=1.2
no_repeat_ngram_size=3
```

The 10m model **collapses into degenerate attractors** (the famous
"United States" loop, see `~/osito-a/arch/x86/agent/agent.c:112`)
without both rep_penalty AND no_repeat_ngram_size:

- `rep_penalty` alone reduces but doesn't eliminate the loops.
  "United States United States" becomes "Pacific Pacific Pacific."
- `no_repeat_ngram_size=3` is a **hard structural ban** on closing any
  3-gram that already appeared in the window. Walk the linear history;
  if the last (n-1)=2 tokens match positions `[i, i+1]` for any prior
  `i`, force `logits[history[i+2]] = -INF`.

With both active, output becomes fluent multi-clause English (still
hallucinating because 10m params, but every phrase is grammatical).

Reference impl: osito-k commit `81c34f4` `apply_no_repeat_ngram` +
`apply_penalties`.

## Step 7 — chat template (depends on use case)

The `chat.py` reference uses **raw text** (no template, no BOS):
just `tokenizer.encode(prompt)`. The `test_generation.py` AND
`rag-brandon.py` use ChatML:

```
<|im_start|>user\n{prompt}<|im_end|>\n<|im_start|>assistant\n
```

For RAG add a system turn:

```
<|im_start|>system\n
Answer the user's question using the context below. If the context
does not contain the answer, say you don't know.

Context:
{retrieved_chunks}<|im_end|>\n
<|im_start|>user\n{query}<|im_end|>\n
<|im_start|>assistant\n
```

Detect ChatML token ids by scanning the vocab for the literal strings
`<|im_start|>` / `<|im_end|>` — don't assume positional ids 4/5, the
converter writes them as regular vocab entries and the position can
shift if the tokenizer is rebuilt.

## Step 8 — calibrate against the reference

After steps 1-7, brandon-tiny-10m on `chat Who was Albert Einstein?`
should produce roughly:

```
was a man who had been over 14 years old and died in 189 BC when he
is born in Mesopotamia between the 19th and 20th century University
of China decided to take his life back home in the United States…
```

Fluent English, factual hallucinations (10m params), no degenerate
loops. Matches the `rag-experiment.md` reference output style.

If you see *less* than this — check matvec F16, then v_first reset,
then the sampling recipe.

## Step 9 — self-check before declaring done

- [ ] GGUF loads with `architecture="brandon"`,
      `n_unique_blocks=block_count=12`,
      `n_layers=compute_layer_count=24`,
      `register.weight` and `dwa.weight` resolved
- [ ] First forward through layer 0 produces non-zero K/V (matvec F16
      works)
- [ ] `<|im_start|>` and `<|im_end|>` resolve via vocab string search
- [ ] At greedy (temp 0) the chat produces non-empty output (= argmax
      doesn't land on `<pad>` = id 0 every step)
- [ ] At temp 0.7 + penalty 1.2 + ngram 3, `chat hello` produces
      grammatical multi-clause English
- [ ] No "the the the" or "United States" loops in 128-token outputs
- [ ] Subsequent chat calls re-run register prefill (look for
      4 distinct K/V writes at positions 0..3 every chat command)

## Step 10 — production deployment notes

- F16 GGUF (~21 MB) is what `convert-brandon-tiny.py` ships. No Q4
  variant exists yet.
- For WASM / no-SIMD: pre-dequant F16 → F32 at load (~40 MB extra,
  ~3x speedup since the F32 inner loop auto-vectorizes). osito-k
  `gguf_dequant_f16_to_f32` in commit `526c50e`.
- KV cache: 24 logical layers × max_seq × kv_dim float = ~6 MB for
  max_seq=512.
- Auxiliary state: `dwa_buf = (n_layers+1)*dim*4 = 25 KB`,
  `v_first = kv_dim*4 = 256 B`. Total ~30 KB.
- Throughput targets:
  - WASM with `-msimd128` and pre-dequant: ~60-80 ms/token
  - Native x86 with F16C/AVX2: ~10-20 ms/token

## Final sanity: which order do bugs surface

If you're debugging mid-port, here's the failure mode at each step
to help you localize:

| Symptom | Most likely cause |
|---|---|
| `layers=0 dim=0` after GGUF load | Step 1: missing `brandon.*` parser |
| `tensor blk.13.attn_q.weight not found` | Step 2: looking up >n_unique_blocks |
| Output empty under greedy, vocab-shaped under temp | Step 3: matvec F16 case missing → zero weights |
| Repeating single token "on on on" | Step 4b: v_first persists across forwards |
| Output coherent but unrelated to prompt | Step 5: BPE tokenizer mangling SPM input |
| Output fluent but loops "Pacific Pacific" | Step 6: missing no_repeat_ngram_size |
| First chat OK, second chat outputs from prior context | Step 4d: registers_prefilled not reset |
| Output coherent but 10x slower than expected | Step 10: scalar F16 matvec, need pre-dequant or F16C |
