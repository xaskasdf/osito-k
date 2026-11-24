/*
 * llm.c -- local LLM inference: `model`, `chat` and `llm`
 *
 * The inference stack is x86's, ported: fs/gguf.c parses the GGUF,
 * tokenizer.c (BPE or SentencePiece), inference.c (llama-style models and
 * brandon-tiny, with the chat template chosen from the vocabulary) and
 * tensor.c, whose SIMD paths are NEON here (tensor_neon.c), or SDOT on an
 * 8-bit copy of the input for quantized weights (tensor_sdot.c). This file
 * loads a model from OsitoFS into the high memory region, wires the
 * tokenizer, and has the shell commands plus what the x86 code expects of
 * its kernel: SMP work submission and NVMe layer streaming are stubbed, so
 * everything runs on one CPU.
 *
 * FP/SIMD registers aren't saved by the scheduler. The shell task is
 * pinned (to the boot CPU) and nothing else there uses them; `model` and
 * `chat` move it to the X1 (CPU 7) for speed while they run, and back.
 *
 * Matvecs are split by rows over the X1 and three workers pinned to the
 * A78s (CPUs 4-6; inference.c's matvec calls llm_par_run). Each worker is
 * the only FP user on its CPU. While `model` or `chat` runs they spin
 * waiting for work, since a matvec takes a fraction of a millisecond and
 * a sleep a tick; otherwise they sleep. The A55s are left alone: an even
 * split would wait on them.
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "../include/dispatch.h"
#include "../fs/gguf.h"
#include "../../../include/common/ositofs2_format.h"
#include "../include/fs.h"
#include "../fs/ositofs3.h"
#include "inference.h"
#include "tokenizer.h"
#include "tensor.h"
#include "tensor_sdot.h"

#define LLM_CPU         7               /* the X1 */
#define MAX_SEQ         512
#define MAX_TOKENS      200
#define READ_CHUNK      (1U << 20)

extern int tensor_simd;
extern tokenizer_t g_tokenizer;
extern const char *llama_state_arch(void *s);
extern int gguf_load_from_mem(gguf_model_t *model, void *data, uint64_t size);
extern int llama_chat(llama_state_t *state, const char *text, uint32_t max_tokens,
                      void (*on_token)(const char *text, void *ctx), void *ctx);
extern void matvec_f16_avx2(float *out, const void *weight, const float *input,
                            uint32_t rows, uint32_t cols);

/* --- The matvec workers */

#define WORKERS 3
static const int worker_cpu[WORKERS] = { 4, 5, 6 };    /* the A78s */

static struct worker {
    volatile uint32_t posted, done;     /* job numbers */
    void (*volatile fn)(void *ctx, int i);
    void *volatile ctx;
    volatile int part;
} workers[WORKERS];
static volatile int spinning;           /* model / chat running */
static int workers_started;
int llm_parts = 1;                      /* inference.c splits matvecs this many ways */
int llm_use_workers = 1;                /* `llm smp off`: the X1 alone */

static inline void barrier(void) { __asm__ volatile("dmb ish" : : : "memory"); }

static void worker_task(void *arg)
{
    struct worker *w = (struct worker *)arg;
    uint32_t seen = w->posted;
    for (;;) {
        if (w->posted == seen) {
            if (spinning)
                __asm__ volatile("yield");
            else
                task_delay_ms(20);
            continue;
        }
        barrier();
        seen = w->posted;
        w->fn(w->ctx, w->part);
        barrier();
        w->done = seen;
    }
}

/* part(ctx, i) for i < parts: the first parts - 1 on the workers, the
 * last here; returns when all are done */
void llm_par_run(int parts, void (*part)(void *ctx, int i), void *ctx)
{
    int n = parts - 1 < WORKERS ? parts - 1 : WORKERS;
    for (int i = 0; i < n; i++) {
        workers[i].fn = part;
        workers[i].ctx = ctx;
        workers[i].part = i;
        barrier();
        workers[i].posted = workers[i].posted + 1;
    }
    for (int i = n; i < parts; i++)
        part(ctx, i);
    for (int i = 0; i < n; i++)
        while (workers[i].done != workers[i].posted)
            __asm__ volatile("yield");
    barrier();
}

static void workers_on(void)
{
    /* task_create_on takes any CPU number, and a task pinned to one that
     * isn't online never runs (QEMU virt has 1-4): only with all eight */
    if (!workers_started) {
        workers_started = smp_get_cpu_count() > LLM_CPU ? 1 : -1;
        for (int i = 0; i < WORKERS && workers_started > 0; i++)
            if (task_create_on("llm", worker_task, &workers[i], 1, worker_cpu[i]) < 0)
                workers_started = -1;
    }
    if (workers_started > 0 && llm_use_workers) {
        spinning = 1;
        llm_parts = WORKERS + 1;
    }
}

static void workers_off(void)
{
    llm_parts = 1;
    spinning = 0;
}

/* What the x86 inference expects of its kernel. ap_worker_count = 0 keeps
 * x86's own SMP paths off (the split above replaces them); the submit
 * functions are never reached then, and NVMe streaming
 * (llama_init_streaming) reports no device. */
int ap_worker_count;
int smp_submit(int cpu, void (*fn)(void *, void *), void *arg, void *res)
{
    (void)cpu; (void)fn; (void)arg; (void)res;
    return -1;
}
int smp_submit_any(void (*fn)(void *, void *), void *arg, void *res)
{
    (void)fn; (void)arg; (void)res;
    return -1;
}
void smp_wait(int id) { (void)id; }
int nvme_read(uint64_t lba, uint32_t count, void *buf)
{
    (void)lba; (void)count; (void)buf;
    return -1;
}
int nvme_read_async(uint64_t lba, uint32_t count, uint64_t phys)
{
    (void)lba; (void)count; (void)phys;
    return -1;
}
int nvme_wait_cq(uint16_t cid) { (void)cid; return -1; }
uint32_t nvme_get_lba_size(void) { return 512; }
/* x86's per-phase profiler, and the NVMe-direct tensor map's file offset */
void perf_phase_enter(int slot, const char *name) { (void)slot; (void)name; }
void perf_phase_exit(int slot) { (void)slot; }
uint64_t osfs2_file_byte_offset(osfs2_file_t *file) { (void)file; return 0; }

static gguf_model_t model;
static gguf_tokenizer_t gtok;
static llama_state_t llama;
static int loaded;
static void *model_data;
static uint64_t model_pages;
static char model_file[64];
static uint64_t last_tokens, last_ms;

static uint64_t now_ms(void)
{
    return timer_ms();
}

/* Run fn on the X1 with the workers spinning, then put the calling task
 * back where it was */
static void on_big_core(void (*fn)(void *), void *arg)
{
    int self = task_self();
    int was = task_pin(self, LLM_CPU);
    workers_on();
    fn(arg);
    workers_off();
    if (was >= -1)
        task_pin(self, was);
}

static void unload(void)
{
    if (!loaded && !model_data)
        return;
    if (loaded)
        llama_free(&llama);
    tok_free(&g_tokenizer);
    if (model_data)
        mem_free_pages(model_data, model_pages);
    model_data = (void *)0;
    loaded = 0;
    model_file[0] = 0;
}

static void load(void *arg)
{
    const char *name = (const char *)arg;
    void *f = fs_find(name);
    if (!f) {
        serial_puts("model: no file ");
        serial_puts(name);
        serial_puts(" (put it with rog5-put.ps1)\n");
        return;
    }
    unload();
    uint64_t size = osfs3_file_size(f);
    model_pages = (size + 4095) / 4096;
    model_data = mem_alloc_aligned_high(size, 2U << 20);
    if (!model_data) {
        serial_puts("model: no memory for it\n");
        return;
    }

    uint64_t t0 = now_ms();
    for (uint64_t off = 0; off < size; off += READ_CHUNK) {
        uint64_t n = size - off < READ_CHUNK ? size - off : READ_CHUNK;
        if (osfs3_read_file(f, off, (uint8_t *)model_data + off, n) < 0) {
            serial_puts("model: read failed\n");
            unload();
            return;
        }
    }
    uint64_t t1 = now_ms();
    serial_puts("model: read ");
    serial_putdec(size >> 20);
    serial_puts(" MB in ");
    serial_putdec(t1 - t0);
    serial_puts(" ms\n");

    if (gguf_load_from_mem(&model, model_data, size) < 0) {
        serial_puts("model: not a GGUF it can parse\n");
        unload();
        return;
    }
    /* The vocabulary's own algorithm: "llama" with scores is SentencePiece,
     * "gpt2" (or none) is BPE with merges */
    if (gguf_load_tokenizer(&model, &gtok) == 0) {
        if (gtok.tok_model[0] == 'l' && gtok.scores)
            tok_init_spm(&g_tokenizer, (const char **)gtok.tokens, gtok.token_lens,
                         gtok.scores, gtok.token_types, gtok.n_tokens,
                         gtok.bos_id, gtok.eos_id, gtok.unk_id, gtok.pad_id);
        else
            tok_init(&g_tokenizer, (const char **)gtok.tokens, gtok.token_lens, gtok.n_tokens,
                     (const char **)gtok.merges, gtok.merge_lens, gtok.n_merges,
                     gtok.bos_id, gtok.eos_id);
    }
    dispatch_init();
    if (llama_init(&llama, &model, MAX_SEQ) < 0) {
        serial_puts("model: llama_init failed\n");
        unload();
        return;
    }
    loaded = 1;
    int i = 0;
    while (name[i] && i < (int)sizeof(model_file) - 1) {
        model_file[i] = name[i];
        i++;
    }
    model_file[i] = 0;
    serial_puts("model: ");
    serial_puts(model_file);
    serial_puts(" ready in ");
    serial_putdec(now_ms() - t0);
    serial_puts(" ms (");
    serial_puts(disp.path_name);
    serial_puts(", on CPU ");
    serial_putdec(LLM_CPU);
    serial_puts("); chat <text>\n");
}

static uint64_t chat_tokens;

static void on_token(const char *text, void *ctx)
{
    (void)ctx;
    chat_tokens++;
    serial_puts(text);
}

static void chat(void *arg)
{
    chat_tokens = 0;
    uint64_t t0 = now_ms();
    int r = llama_chat(&llama, (const char *)arg, MAX_TOKENS, on_token, (void *)0);
    last_ms = now_ms() - t0;
    last_tokens = chat_tokens;
    serial_puts(r < 0 ? "\n[error]\n" : "\n");
}

/* Shell `model`: the loaded model; `model <file>` loads one from OsitoFS */
void cmd_model(int argc, char **argv)
{
    if (argc >= 2) {
        on_big_core(load, argv[1]);
        return;
    }
    if (!loaded) {
        serial_puts("model: none loaded (model <file.gguf>)\n");
        return;
    }
    serial_puts("model: ");
    serial_puts(model_file);
    serial_puts(", ");
    serial_puts(llama_state_arch(&llama));
    serial_puts(", ");
    serial_putdec(llama.n_layers);
    serial_puts(" layers, dim ");
    serial_putdec(llama.dim);
    serial_puts(", vocab ");
    serial_putdec(llama.vocab_size);
    serial_puts(", ");
    serial_putdec(model_pages * 4096 >> 20);
    serial_puts(" MB\n");
}

/* Shell `chat <text>` */
void cmd_chat(int argc, char **argv)
{
    if (argc < 2) {
        serial_puts("usage: chat <text>\n");
        return;
    }
    if (!loaded) {
        serial_puts("chat: no model (model <file.gguf>)\n");
        return;
    }
    static char prompt[1024];
    int n = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1 && n < (int)sizeof(prompt) - 1)
            prompt[n++] = ' ';
        for (const char *w = argv[i]; *w && n < (int)sizeof(prompt) - 1; w++)
            prompt[n++] = *w;
    }
    prompt[n] = 0;
    on_big_core(chat, prompt);  /* llama_chat prints prefill and ms/token */
}

/* --- `llm selftest`: NEON against the scalar code, and the math */

static uint64_t rng = 0x2545F4914F6CDD1DULL;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)rng;
}

static float rndf(void)                 /* [-1, 1) */
{
    return (float)(int32_t)rnd() / 2147483648.0f;
}

static float absf(float x) { return x < 0 ? -x : x; }

/* The largest difference relative to the largest magnitude */
static float rel_diff(const float *a, const float *b, uint32_t n)
{
    float d = 0, m = 1e-30f;
    for (uint32_t i = 0; i < n; i++) {
        if (absf(a[i] - b[i]) > d)
            d = absf(a[i] - b[i]);
        if (absf(b[i]) > m)
            m = absf(b[i]);
    }
    return d / m;
}

static void put_e6(float x)             /* x in millionths */
{
    int64_t v = (int64_t)(x * 1e6f + (x < 0 ? -0.5f : 0.5f));
    if (v < 0) {
        serial_puts("-");
        v = -v;
    }
    serial_putdec((uint64_t)v);
    serial_puts("e-6");
}

static int check(const char *what, float err, float limit)
{
    serial_puts("  ");
    serial_puts(what);
    serial_puts(": ");
    put_e6(err);
    serial_puts(err <= limit ? " ok\n" : " FAIL\n");
    return err <= limit;
}

#define ST_ROWS 64
#define ST_COLS 512

/* The worker split, on a q4_0 matvec: each part takes a slice of rows */
static struct {
    float *out;
    const uint8_t *w;
    const float *x;
    int parts;
} st_split;

static void st_part(void *ctx, int i)
{
    (void)ctx;
    uint32_t r0 = ST_ROWS * (uint32_t)i / (uint32_t)st_split.parts;
    uint32_t r1 = ST_ROWS * (uint32_t)(i + 1) / (uint32_t)st_split.parts;
    matvec_q4_0(st_split.out + r0, st_split.w + (uint64_t)r0 * (ST_COLS / 32 * 18),
                st_split.x, r1 - r0, ST_COLS);
}

static void selftest(void *arg)
{
    (void)arg;
    static uint8_t q4[ST_ROWS * ST_COLS / 32 * 18];
    static uint8_t q41[ST_ROWS * ST_COLS / 32 * 20];
    static uint8_t q80[ST_ROWS * ST_COLS / 32 * 34];
    static int8_t xq[ST_COLS];
    static float xd[ST_COLS / 32], xs[ST_COLS / 32];
    static __fp16 f16[ST_ROWS * ST_COLS];
    static float x[ST_COLS], a[ST_COLS], b[ST_COLS], w[ST_COLS];
    static float ref[ST_ROWS > ST_COLS ? ST_ROWS : ST_COLS], got[ST_ROWS > ST_COLS ? ST_ROWS : ST_COLS];
    int ok = 1, saved = tensor_simd;

    for (uint32_t i = 0; i < sizeof(q4); i++)
        q4[i] = (uint8_t)rnd();
    for (uint32_t blk = 0; blk < sizeof(q4) / 18; blk++) {
        __fp16 s = (__fp16)(0.01f + 0.05f * (absf(rndf())));
        memcpy(q4 + blk * 18, &s, 2);
    }
    for (uint32_t i = 0; i < sizeof(q41); i++)
        q41[i] = (uint8_t)rnd();
    for (uint32_t blk = 0; blk < sizeof(q41) / 20; blk++) {
        __fp16 dm[2] = { (__fp16)(0.01f + 0.05f * absf(rndf())), (__fp16)(-0.4f * absf(rndf())) };
        memcpy(q41 + blk * 20, dm, 4);
    }
    for (uint32_t i = 0; i < sizeof(q80); i++)
        q80[i] = (uint8_t)rnd();
    for (uint32_t blk = 0; blk < sizeof(q80) / 34; blk++) {
        __fp16 s = (__fp16)(0.0005f + 0.002f * absf(rndf()));
        memcpy(q80 + blk * 34, &s, 2);
    }
    for (uint32_t i = 0; i < ST_ROWS * ST_COLS; i++)
        f16[i] = (__fp16)rndf();
    for (uint32_t i = 0; i < ST_COLS; i++) {
        x[i] = rndf();
        a[i] = rndf();
        b[i] = rndf();
        w[i] = 0.5f + rndf();
    }
    serial_puts("llm selftest (NEON vs scalar, largest relative difference):\n");

    tensor_simd = 0;
    matvec_q4_0(ref, q4, x, ST_ROWS, ST_COLS);
    tensor_simd = 1;
    matvec_q4_0(got, q4, x, ST_ROWS, ST_COLS);
    ok &= check("matvec q4_0", rel_diff(got, ref, ST_ROWS), 1e-5f);

    matvec_q4_0(ref, q4, x, ST_ROWS, ST_COLS);
    st_split.out = got;
    st_split.w = q4;
    st_split.x = x;
    st_split.parts = llm_parts;
    memset(got, 0, sizeof(got));
    llm_par_run(llm_parts, st_part, (void *)0);
    serial_puts("  split over ");
    serial_putdec((uint64_t)llm_parts);
    serial_puts(llm_parts > 1 ? " CPUs (X1 + A78s)" : " CPU");
    ok &= check("", rel_diff(got, ref, ST_ROWS), 0);

    tensor_simd = 0;
    matvec_q4_1(ref, q41, x, ST_ROWS, ST_COLS);
    tensor_simd = 1;
    matvec_q4_1(got, q41, x, ST_ROWS, ST_COLS);
    ok &= check("matvec q4_1", rel_diff(got, ref, ST_ROWS), 1e-5f);

    tensor_simd = 0;
    matvec_q8_0(ref, q80, x, ST_ROWS, ST_COLS);
    tensor_simd = 1;
    matvec_q8_0(got, q80, x, ST_ROWS, ST_COLS);
    ok &= check("matvec q8_0", rel_diff(got, ref, ST_ROWS), 1e-5f);

    /* SDOT rounds the input to 8 bits: a fraction of a percent */
    if (tensor_sdot_detect()) {
        q8_input_t xq8 = { xd, xs, xq };
        q8_quantize(&xq8, x, ST_COLS);
        matvec_q8_0_sdot(got, q80, &xq8, ST_ROWS, ST_COLS);
        ok &= check("sdot q8_0 ", rel_diff(got, ref, ST_ROWS), 2e-2f);
        tensor_simd = 0;
        matvec_q4_0(ref, q4, x, ST_ROWS, ST_COLS);
        matvec_q4_0_sdot(got, q4, &xq8, ST_ROWS, ST_COLS);
        ok &= check("sdot q4_0 ", rel_diff(got, ref, ST_ROWS), 2e-2f);
        matvec_q4_1(ref, q41, x, ST_ROWS, ST_COLS);
        matvec_q4_1_sdot(got, q41, &xq8, ST_ROWS, ST_COLS);
        ok &= check("sdot q4_1 ", rel_diff(got, ref, ST_ROWS), 2e-2f);
        tensor_simd = 1;
    } else {
        serial_puts("  sdot: not on this CPU\n");
    }

    for (uint32_t r = 0; r < ST_ROWS; r++) {
        float s = 0;
        for (uint32_t c = 0; c < ST_COLS; c++)
            s += (float)f16[r * ST_COLS + c] * x[c];
        ref[r] = s;
    }
    matvec_f16_avx2(got, f16, x, ST_ROWS, ST_COLS);
    ok &= check("matvec f16 ", rel_diff(got, ref, ST_ROWS), 1e-5f);

    tensor_simd = 0;
    rmsnorm(ref, x, w, ST_COLS);
    tensor_simd = 1;
    rmsnorm(got, x, w, ST_COLS);
    ok &= check("rmsnorm    ", rel_diff(got, ref, ST_COLS), 1e-6f);

    tensor_simd = 0;
    vec_add(ref, a, b, ST_COLS);
    tensor_simd = 1;
    vec_add(got, a, b, ST_COLS);
    ok &= check("vec_add    ", rel_diff(got, ref, ST_COLS), 0);

    tensor_simd = 0;
    vec_mul(ref, a, b, ST_COLS);
    tensor_simd = 1;
    vec_mul(got, a, b, ST_COLS);
    ok &= check("vec_mul    ", rel_diff(got, ref, ST_COLS), 0);

    /* The math, against known values */
    float m[][2] = {
        { expf_bare(1.0f),            2.718281828f },
        { expf_bare(-10.0f),          4.539993e-5f },
        { sinf_bare(1.0f),            0.841470985f },
        { cosf_bare(1000.0f),         0.562379076f },
        { sinf_bare(-300.5f),         0.887961975f },
        { powf_bare(10000.0f, -0.5f), 0.01f },
        { sqrtf_bare(2.0f),           1.414213562f },
    };
    float worst = 0;
    for (uint32_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        float e = absf(m[i][0] - m[i][1]) / absf(m[i][1]);
        if (e > worst)
            worst = e;
    }
    ok &= check("exp/sin/cos/pow/sqrt", worst, 1e-6f);

    tensor_simd = saved;
    serial_puts(ok ? "llm selftest: all ok\n" : "llm selftest: FAILED\n");
}

/* Shell `llm`: the compute path; `llm simd on|off`, `llm sdot on|off`,
 * `llm smp on|off`, `llm selftest` */
void cmd_llm(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "selftest") == 0) {
        on_big_core(selftest, (void *)0);
        return;
    }
    if (argc == 3 && strcmp(argv[1], "simd") == 0) {
        tensor_simd = strcmp(argv[2], "off") != 0;
    } else if (argc == 3 && strcmp(argv[1], "sdot") == 0) {
        tensor_sdot_wanted = strcmp(argv[2], "off") != 0;
    } else if (argc == 3 && strcmp(argv[1], "smp") == 0) {
        llm_use_workers = strcmp(argv[2], "off") != 0;
    } else if (argc != 1) {
        serial_puts("usage: llm [simd on|off | sdot on|off | smp on|off | selftest]\n");
        return;
    }
    dispatch_init();
    serial_puts("llm: ");
    serial_puts(disp.path_name);
    if (tensor_simd && tensor_sdot_wanted && !tensor_sdot)
        serial_puts(" (no SDOT on this CPU)");
    if (smp_get_cpu_count() <= LLM_CPU)
        serial_puts(", one CPU (no X1 here)");
    else
        serial_puts(llm_use_workers ? ", the X1 + the A78s (CPUs 7, 4-6)" : ", the X1 alone");
    serial_puts(loaded ? ", model " : ", no model");
    serial_puts(loaded ? model_file : "");
    if (last_tokens && last_ms) {
        serial_puts(", last chat ");
        serial_putdec(last_tokens);
        serial_puts(" tokens in ");
        serial_putdec(last_ms);
        serial_puts(" ms with the prefill");
    }
    serial_puts("\n");
}
