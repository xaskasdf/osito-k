/* W4.8 link-time stubs for symbols deliberately deferred.
 *
 * AUDIT POLICY (Wave 3, Apr 28):
 *   Every function in this file is either:
 *     A) A REAL implementation backed by OsitoK syscalls / kernel ABI, OR
 *     B) A documented intentional no-op with a comment block explaining
 *        WHY it is safe AND naming the upstream Mesa file that would
 *        provide the real implementation if/when needed.
 *
 *   No bare `return 0/NULL/-1;` without justification.  If you find one,
 *   either fix it or raise an alarm — silent stubs are how we lost a day
 *   of debugging on u_thread_create returning -1.
 */

#include <stdint.h>
#include <stddef.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t  u8;

/* W4.8++ BLAKE3 SIMD stubs removed — real impls now provided by
 * mesa/src/util/blake3/blake3_{sse2,sse41,avx2,avx512}_x86-64_unix.S
 * (compiled via BLAKE3_ASM_OBJS into libmesa_util.a).
 */

/* The generated Mesa glcpp lexer/parser is linked from libmesa_compiler.
 * User-provided GLSL must never fall back to placeholder parser symbols. */

/* W4.8++ — TGSI exec stubs removed (real impl from tgsi_exec.c in libmesa_gallium.a). */
/* W4.8++ — u_vbuf stubs removed (real impl from u_vbuf.c in libmesa_gallium.a). */

/* ============================================================
 * (2) translate_sse2 — INTENTIONAL NO-OP
 * ============================================================
 * Upstream:  mesa/src/gallium/auxiliary/translate/translate_sse.c
 *            (x86 rtasm JIT for vertex-element software fetch).
 *
 * Path on smoke test:  NEVER REACHED.
 *   - rtasm JIT only kicks in for software vertex transform path.
 *   - Zink uses real GPU vertex pipeline via Vulkan; gallium's translate
 *     module falls back to its generic C path (translate_generic.c) when
 *     translate_sse2_create returns NULL.
 *
 * Vendoring requires translate_sse.c + the rtasm subdirectory + dynamic
 * code-generation buffer mgmt (mmap PROT_EXEC) — large surface for
 * zero benefit on a Zink-only build.
 */
void *translate_sse2_create(const void *key) {
    (void)key;
    return (void *)0;  /* translate_generic_create takes over in caller */
}

/* ============================================================
 * (3) util_barrier — REAL IMPLEMENTATION
 * ============================================================
 * Upstream:  mesa/src/util/u_thread.c:179-234 (the !HAVE_PTHREAD branch).
 *
 * Implements pthread_barrier semantics: N threads call wait, the last one
 * to arrive releases all and resets the barrier.  Backed by our futex
 * mutex + condvar primitives in mesa_libc_stubs.c.
 *
 * The util_barrier struct from u_thread.h is:
 *   struct {
 *     unsigned count;
 *     unsigned waiters;
 *     uint64_t sequence;
 *     mtx_t mutex;       (= int = 4 bytes futex word)
 *     cnd_t condvar;     (= int = 4 bytes futex seq)
 *   } util_barrier;
 *
 * We mirror the layout here so we can manipulate fields by offset without
 * a header dependency on u_thread.h (avoids include circular hell).
 */
struct _osito_util_barrier {
    unsigned count;
    unsigned waiters;
    uint64_t sequence;
    int      mutex;
    int      condvar;
};

extern int mtx_init(int *m, int type);
extern int mtx_destroy(int *m);
extern int mtx_lock(int *m);
extern int mtx_unlock(int *m);
extern int cnd_init(int *c);
extern int cnd_destroy(int *c);
extern int cnd_wait(int *c, int *m);
extern int cnd_broadcast(int *c);

int util_barrier_init(void *barrier, unsigned count) {
    struct _osito_util_barrier *b = (struct _osito_util_barrier *)barrier;
    b->count    = count;
    b->waiters  = 0;
    b->sequence = 0;
    mtx_init(&b->mutex, 0);
    cnd_init(&b->condvar);
    return 0;
}

int util_barrier_destroy(void *barrier) {
    struct _osito_util_barrier *b = (struct _osito_util_barrier *)barrier;
    mtx_destroy(&b->mutex);
    cnd_destroy(&b->condvar);
    return 0;
}

int util_barrier_wait(void *barrier) {
    struct _osito_util_barrier *b = (struct _osito_util_barrier *)barrier;
    mtx_lock(&b->mutex);
    b->waiters++;
    if (b->waiters < b->count) {
        uint64_t my_seq = b->sequence;
        do {
            cnd_wait(&b->condvar, &b->mutex);
        } while (my_seq == b->sequence);
    } else {
        /* Last arriver: reset and release. */
        b->waiters = 0;
        b->sequence++;
        cnd_broadcast(&b->condvar);
    }
    mtx_unlock(&b->mutex);
    return 1;  /* true — Mesa's barrier_wait returns bool/non-zero on success */
}

/* ============================================================
 * (4) u_cnd_monotonic — REAL IMPLEMENTATION
 * ============================================================
 * Upstream:  mesa/src/util/cnd_monotonic.{c,h}
 *
 * Same primitive as cnd_t (already real in mesa_libc_stubs.c), but uses
 * CLOCK_MONOTONIC for timed waits.  On OsitoK there's no separate monotonic
 * clock — gettimeofday is the only time source — so monotonic ≡ realtime.
 *
 * Struct from cnd_monotonic.h (non-Windows):
 *   struct u_cnd_monotonic { pthread_cond_t cond; }  (= int = 4 bytes)
 */
int u_cnd_monotonic_init(void *cnd) {
    return cnd_init((int *)cnd);
}
void u_cnd_monotonic_destroy(void *cnd) {
    cnd_destroy((int *)cnd);
}
int u_cnd_monotonic_broadcast(void *cnd) {
    return cnd_broadcast((int *)cnd);
}
extern int cnd_signal(int *c);
int u_cnd_monotonic_signal(void *cnd) {
    return cnd_signal((int *)cnd);
}
int u_cnd_monotonic_wait(void *cnd, void *mtx) {
    return cnd_wait((int *)cnd, (int *)mtx);
}
int u_cnd_monotonic_timedwait(void *cnd, void *mtx, const void *ts) {
    /* Kernel sys_futex doesn't honour the timeout arg yet (same situation
     * as cnd_timedwait in mesa_libc_stubs.c).  Treat as untimed wait —
     * Mesa's only timed-wait callers are debug profiling paths in the
     * util_queue worker scheduler; over-waiting blocks shutdown but does
     * not corrupt state.  Ref: mesa/src/util/cnd_monotonic.c upstream
     * impl uses pthread_cond_clockwait(CLOCK_MONOTONIC). */
    (void)ts;
    return cnd_wait((int *)cnd, (int *)mtx);
}

/* u_thread — DROP `u_thread_create` here. The real Mesa impl in
 * mesa/src/util/u_thread.c wraps thrd_create (which we now provide for
 * real via SYS_CLONE in mesa_libc_stubs.c). With --allow-multiple-definition
 * the linker takes the FIRST archive object; this stub had been silently
 * shadowing the real one, making every util_queue_init fail at thread
 * spawn (-1 return from the stub). */

/* ============================================================
 * (5) u_thread_setname — REAL (best-effort)
 * ============================================================
 * Upstream: mesa/src/util/u_thread.c:u_thread_setname uses
 *           pthread_setname_np → prctl(PR_SET_NAME) on Linux.
 *
 * OsitoK kernel exposes prctl(15 == PR_SET_NAME, name) via syscall 157.
 * If the kernel branch isn't wired we just silently ignore.  Setting the
 * thread name is purely a debug aid (visible in `top` / serial logs).
 */
extern long __syscall2(long n, long a, long b);
extern long __syscall5(long n, long a, long b, long c, long d, long e);
void u_thread_setname(const char *name) {
    if (!name) return;
    /* prctl(PR_SET_NAME=15, name) — syscall 157 on Linux/OsitoK ABI.
     * Returns -ENOSYS on kernels without prctl; we ignore the error. */
    (void)__syscall5(157, 15, (long)(uintptr_t)name, 0, 0, 0);
}

/* ============================================================
 * (6) util_thread_get_time_nano — INTENTIONAL NO-OP
 * ============================================================
 * Upstream: mesa/src/util/u_thread.c — RUSAGE_THREAD getrusage on Linux
 *           (per-thread CPU time, kernel + user).
 *
 * OsitoK doesn't expose per-thread getrusage.  Returning 0 means Mesa's
 * profiling (util_queue stats, MESA_PROFILE) can't track per-worker CPU.
 * Smoke test path NEVER USES THIS — only enabled by MESA_PROFILE=1 env
 * var (always NULL on OsitoK; getenv returns NULL).
 */
long util_thread_get_time_nano(int thread_id) {
    (void)thread_id;
    return 0;  /* no per-thread CPU accounting available */
}

/* ============================================================
 * (7) util_get_current_cpu — REAL IMPLEMENTATION via CPUID
 * ============================================================
 * Upstream: mesa/src/util/u_cpu_detect.c uses sched_getcpu() on Linux.
 *
 * We read the local APIC ID from CPUID leaf 1 EBX[31:24] — fast (no
 * syscall) and gives us a unique-per-logical-CPU integer.  Mesa uses
 * this to NUMA-pin worker threads; on a single-socket bare-metal box
 * the APIC ID is also the logical CPU index for ID < 256.
 *
 * For >256-CPU boxes we'd want CPUID leaf 0xB (extended topology) which
 * yields a 32-bit APIC ID in EDX.  Single-CPU boxes will see ID = 0,
 * which is correct.
 */
int util_get_current_cpu(void) {
    unsigned int eax, ebx, ecx, edx;
    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1), "c"(0)
    );
    /* Initial APIC ID is in EBX[31:24] (CPUID.1) */
    return (int)((ebx >> 24) & 0xFF);
}

/* ============================================================
 * (8) util_set_thread_affinity — INTENTIONAL NO-OP
 * ============================================================
 * Upstream: mesa/src/util/u_thread.c calls pthread_setaffinity_np →
 *           sched_setaffinity (syscall 203) on Linux.
 *
 * OsitoK's SMP scheduler (arch/x86/kernel/smp.c, sched_rt.c) does NOT
 * currently expose a sys_sched_setaffinity hook — threads run wherever
 * the AP scheduler picks them up.  Wiring would need:
 *   1. add SYS_SCHED_SETAFFINITY=203 branch in arch/x86/kernel/syscall.c
 *   2. add per-process->per-thread cpu_affinity_mask field
 *   3. honour mask in sched_pick_next_task()
 *
 * Mesa Zink's only caller is util_queue worker pinning; without affinity
 * threads still run, just not pinned to a specific core.  Smoke test
 * path NEVER USES THIS — only enabled by MESA_QUEUE_AFFINITY env var.
 *
 * Returns 0 ("success") instead of -1 so Mesa doesn't log a spurious
 * error.  No allocations to leak.
 */
int util_set_thread_affinity(unsigned long thread, const void *mask,
                             void *old_mask, unsigned mask_size) {
    (void)thread;(void)mask;(void)old_mask;(void)mask_size;
    return 0;  /* report success — kernel will still schedule the thread */
}

/* ============================================================
 * (9) trace_context_create_threaded — INTENTIONAL passthrough
 * ============================================================
 * Upstream:  mesa/src/gallium/auxiliary/driver_trace/tr_context.c:2530
 *
 * Real impl wraps a pipe_context with trace recording.  Activated by
 * GALLIUM_TRACE env var (always NULL on OsitoK).  Returning the input
 * pipe unmodified means "no tracing" — exactly what we want.  The
 * caller in zink_context.c:5626 is robust to this passthrough.
 */
void *trace_context_create_threaded(void *screen, void *pipe, void **replace_pipe,
                                    void *replace_data, unsigned flags) {
    (void)screen;(void)replace_pipe;(void)replace_data;(void)flags;
    return pipe;  /* no-op trace wrapper: identity passthrough */
}

/* W4.10++ — nir_lower_aaline_fs / nir_lower_aapoint_fs stubs removed.
 * Real impls now provided by mesa/src/gallium/auxiliary/nir/nir_draw_helpers.c
 * (compiled into libmesa_gallium.a). */

/* ============================================================
 * (10) _mesa_parse_arb_program — INTENTIONAL NO-OP
 * ============================================================
 * Upstream:  mesa/src/mesa/program/arbprogparse.c
 *
 * Parses ARB_vertex_program / ARB_fragment_program assembly text.
 * Zink does not use ARB programs — they're a legacy classic-Mesa path.
 * Smoke test NEVER REACHES this (no glProgramStringARB calls).
 *
 * Returns 0 ("not parsed") so caller's _mesa_error() path triggers and
 * the application gets GL_INVALID_OPERATION for any ARB-program API.
 */
int _mesa_parse_arb_program(void *ctx, unsigned target, const unsigned char *str,
                            unsigned len, void *prog) {
    (void)ctx;(void)target;(void)str;(void)len;(void)prog;
    return 0;  /* parse failed: GL spec error path engages in caller */
}

/* ============================================================
 * (11) driParseConfigFiles / driQueryOptionb — INTENTIONAL defaults
 * ============================================================
 * Upstream:  mesa/src/util/xmlconfig.c
 *
 * On Linux these parse /etc/drirc + ~/.drirc XML config files for
 * per-application Mesa overrides (e.g. "force AA on for Quake").
 * OsitoK has no /etc filesystem to scan; defaults are acceptable.
 *
 * driQueryOptionb returns 0 = false for every option, which is the
 * Mesa default for ALL boolean drirc options anyway (the in-tree
 * options.h definitions all default to false).
 */
void driParseConfigFiles(void *cache, const void *info, int screen_no,
                         const char *driver_name, const char *kernel_driver_name,
                         const char *app_name, const char *app_version,
                         const char *engine_name, const char *engine_version) {
    (void)cache;(void)info;(void)screen_no;(void)driver_name;
    (void)kernel_driver_name;(void)app_name;(void)app_version;
    (void)engine_name;(void)engine_version;
    /* No config file system; cache stays empty. driQueryOption* returns defaults. */
}
unsigned char driQueryOptionb(const void *cache, const char *name) {
    (void)cache;(void)name;
    return 0;
}

int driQueryOptioni(const void *cache, const char *name) {
    (void)cache;(void)name;
    return 0;
}

float driQueryOptionf(const void *cache, const char *name) {
    (void)cache;(void)name;
    return 0.0f;
}

char *driQueryOptionstr(const void *cache, const char *name) {
    static char empty[] = "";
    (void)cache;(void)name;
    return empty;
}

#if 0 /* Superseded by Mesa 25's complete src/vulkan/util/vk_format.c. */
/* ============================================================
 * (12) vk_format helpers — REAL TABLE-DRIVEN IMPL
 * ============================================================
 * Upstream:  mesa/src/vulkan/util/vk_format.c (NOT vendored — generated
 *            from gen_vk_format.py at Mesa build time, not in our tree).
 *
 * The hello-gl-clear smoke test path:
 *   zink_create_screen → choose_pdev → zink_get_format_props
 *     → vk_format_to_pipe_format(VK_FORMAT_B8G8R8A8_UNORM) etc.
 *   When this returned 0 (PIPE_FORMAT_NONE), zink interpreted it as
 *   "format not supported" → fell back to ever-narrower formats →
 *   eventually choose_pdev failed because no swap-chain format matched.
 *
 * Real impl:  hand-mapped table covering every format Zink uses on the
 * Vulkan smoke path (color attachments, swap-chain formats, depth/stencil,
 * common texture formats).  Format codes from vulkan_core.h; PIPE_FORMAT_*
 * from u_formats.h.
 *
 * This is ~80 entries — not 600 — because we only need formats Vulkan
 * actually exposes via vkGetPhysicalDeviceFormatProperties() AND that
 * Zink wants to expose via gallium.  The remaining ~520 formats in the
 * full vk_format_map are video / planar YCbCr / extensions Zink doesn't
 * touch on a smoke path (default: PIPE_FORMAT_NONE = unsupported).
 */

/* PIPE_FORMAT_* values mirror mesa/src/util/format/u_formats.h enum order.
 * We hard-code the small subset Zink actually maps.  Source-of-truth grep:
 *   grep -n 'PIPE_FORMAT_' mesa/src/util/format/u_formats.h
 */
#define PIPE_FORMAT_NONE                 0
#define PIPE_FORMAT_B8G8R8A8_UNORM       1
#define PIPE_FORMAT_B8G8R8X8_UNORM       2
#define PIPE_FORMAT_A8R8G8B8_UNORM       3
#define PIPE_FORMAT_X8R8G8B8_UNORM       4
#define PIPE_FORMAT_A8B8G8R8_UNORM       5
#define PIPE_FORMAT_X8B8G8R8_UNORM       6
#define PIPE_FORMAT_R8_UNORM             83
#define PIPE_FORMAT_R8G8_UNORM           84
#define PIPE_FORMAT_R8G8B8_UNORM         85
#define PIPE_FORMAT_R8G8B8A8_UNORM       86
#define PIPE_FORMAT_R8G8B8X8_UNORM       87
#define PIPE_FORMAT_R8_SNORM             88
#define PIPE_FORMAT_R8G8_SNORM           89
#define PIPE_FORMAT_R8G8B8_SNORM         90
#define PIPE_FORMAT_R8G8B8A8_SNORM       91
#define PIPE_FORMAT_R8_USCALED           92
#define PIPE_FORMAT_R8G8_USCALED         93
#define PIPE_FORMAT_R8G8B8_USCALED       94
#define PIPE_FORMAT_R8G8B8A8_USCALED     95
#define PIPE_FORMAT_R8_SSCALED           96
#define PIPE_FORMAT_R8G8_SSCALED         97
#define PIPE_FORMAT_R8G8B8_SSCALED       98
#define PIPE_FORMAT_R8G8B8A8_SSCALED     99
#define PIPE_FORMAT_R8_UINT              100
#define PIPE_FORMAT_R8G8_UINT            101
#define PIPE_FORMAT_R8G8B8_UINT          102
#define PIPE_FORMAT_R8G8B8A8_UINT        103
#define PIPE_FORMAT_R8_SINT              104
#define PIPE_FORMAT_R8G8_SINT            105
#define PIPE_FORMAT_R8G8B8_SINT          106
#define PIPE_FORMAT_R8G8B8A8_SINT        107
#define PIPE_FORMAT_R8G8B8A8_SRGB        108
#define PIPE_FORMAT_B8G8R8A8_SRGB        109
#define PIPE_FORMAT_R16_UNORM            110
#define PIPE_FORMAT_R16G16_UNORM         111
#define PIPE_FORMAT_R16G16B16A16_UNORM   112
#define PIPE_FORMAT_R16_FLOAT            113
#define PIPE_FORMAT_R16G16_FLOAT         114
#define PIPE_FORMAT_R16G16B16A16_FLOAT   115
#define PIPE_FORMAT_R32_FLOAT            116
#define PIPE_FORMAT_R32G32_FLOAT         117
#define PIPE_FORMAT_R32G32B32_FLOAT      118
#define PIPE_FORMAT_R32G32B32A32_FLOAT   119
#define PIPE_FORMAT_R32_UINT             120
#define PIPE_FORMAT_R32G32_UINT          121
#define PIPE_FORMAT_R32G32B32_UINT       122
#define PIPE_FORMAT_R32G32B32A32_UINT    123
#define PIPE_FORMAT_R32_SINT             124
#define PIPE_FORMAT_R32G32_SINT          125
#define PIPE_FORMAT_R32G32B32_SINT       126
#define PIPE_FORMAT_R32G32B32A32_SINT    127
#define PIPE_FORMAT_Z16_UNORM            140
#define PIPE_FORMAT_Z32_FLOAT            141
#define PIPE_FORMAT_S8_UINT              142
#define PIPE_FORMAT_X8Z24_UNORM          143
#define PIPE_FORMAT_Z24X8_UNORM          144
#define PIPE_FORMAT_S8_UINT_Z24_UNORM    145
#define PIPE_FORMAT_Z24_UNORM_S8_UINT    146
#define PIPE_FORMAT_Z32_FLOAT_S8X24_UINT 147
#define PIPE_FORMAT_R10G10B10A2_UNORM    160
#define PIPE_FORMAT_B10G10R10A2_UNORM    161
#define PIPE_FORMAT_R5G6B5_UNORM         170
#define PIPE_FORMAT_B5G6R5_UNORM         171
#define PIPE_FORMAT_R5G5B5A1_UNORM       172
#define PIPE_FORMAT_B5G5R5A1_UNORM       173
#define PIPE_FORMAT_R4G4B4A4_UNORM       174
#define PIPE_FORMAT_B4G4R4A4_UNORM       175

/* VkFormat enum values from include/vulkan/vulkan_core.h */
#define VK_FORMAT_UNDEFINED                  0
#define VK_FORMAT_R5G6B5_UNORM_PACK16        4
#define VK_FORMAT_B5G6R5_UNORM_PACK16        5
#define VK_FORMAT_R5G5B5A1_UNORM_PACK16      6
#define VK_FORMAT_B5G5R5A1_UNORM_PACK16      7
#define VK_FORMAT_R4G4B4A4_UNORM_PACK16      2
#define VK_FORMAT_B4G4R4A4_UNORM_PACK16      3
#define VK_FORMAT_R8_UNORM                   9
#define VK_FORMAT_R8_SNORM                   10
#define VK_FORMAT_R8_USCALED                 11
#define VK_FORMAT_R8_SSCALED                 12
#define VK_FORMAT_R8_UINT                    13
#define VK_FORMAT_R8_SINT                    14
#define VK_FORMAT_R8G8_UNORM                 16
#define VK_FORMAT_R8G8_SNORM                 17
#define VK_FORMAT_R8G8_USCALED               18
#define VK_FORMAT_R8G8_SSCALED               19
#define VK_FORMAT_R8G8_UINT                  20
#define VK_FORMAT_R8G8_SINT                  21
#define VK_FORMAT_R8G8B8_UNORM               23
#define VK_FORMAT_R8G8B8_SNORM               24
#define VK_FORMAT_R8G8B8_USCALED             25
#define VK_FORMAT_R8G8B8_SSCALED             26
#define VK_FORMAT_R8G8B8_UINT                27
#define VK_FORMAT_R8G8B8_SINT                28
#define VK_FORMAT_R8G8B8A8_UNORM             37
#define VK_FORMAT_R8G8B8A8_SNORM             38
#define VK_FORMAT_R8G8B8A8_USCALED           39
#define VK_FORMAT_R8G8B8A8_SSCALED           40
#define VK_FORMAT_R8G8B8A8_UINT              41
#define VK_FORMAT_R8G8B8A8_SINT              42
#define VK_FORMAT_R8G8B8A8_SRGB              43
#define VK_FORMAT_B8G8R8A8_UNORM             44
#define VK_FORMAT_B8G8R8A8_SRGB              50
#define VK_FORMAT_A2R10G10B10_UNORM_PACK32   58
#define VK_FORMAT_A2B10G10R10_UNORM_PACK32   64
#define VK_FORMAT_R16_UNORM                  70
#define VK_FORMAT_R16_SNORM                  71
#define VK_FORMAT_R16_SFLOAT                 76
#define VK_FORMAT_R16G16_UNORM               77
#define VK_FORMAT_R16G16_SFLOAT              83
#define VK_FORMAT_R16G16B16A16_UNORM         91
#define VK_FORMAT_R16G16B16A16_SFLOAT        97
#define VK_FORMAT_R32_UINT                   98
#define VK_FORMAT_R32_SINT                   99
#define VK_FORMAT_R32_SFLOAT                 100
#define VK_FORMAT_R32G32_UINT                101
#define VK_FORMAT_R32G32_SINT                102
#define VK_FORMAT_R32G32_SFLOAT              103
#define VK_FORMAT_R32G32B32_UINT             104
#define VK_FORMAT_R32G32B32_SINT             105
#define VK_FORMAT_R32G32B32_SFLOAT           106
#define VK_FORMAT_R32G32B32A32_UINT          107
#define VK_FORMAT_R32G32B32A32_SINT          108
#define VK_FORMAT_R32G32B32A32_SFLOAT        109
#define VK_FORMAT_D16_UNORM                  124
#define VK_FORMAT_X8_D24_UNORM_PACK32        125
#define VK_FORMAT_D32_SFLOAT                 126
#define VK_FORMAT_S8_UINT                    127
#define VK_FORMAT_D16_UNORM_S8_UINT          128
#define VK_FORMAT_D24_UNORM_S8_UINT          129
#define VK_FORMAT_D32_SFLOAT_S8_UINT         130

#define VK_IMAGE_ASPECT_COLOR_BIT_OK 0x00000001
#define VK_IMAGE_ASPECT_DEPTH_BIT_OK 0x00000002
#define VK_IMAGE_ASPECT_STENCIL_BIT_OK 0x00000004

unsigned vk_format_aspects(unsigned format)
{
    switch (format) {
    case VK_FORMAT_D16_UNORM:           return VK_IMAGE_ASPECT_DEPTH_BIT_OK;
    case VK_FORMAT_X8_D24_UNORM_PACK32: return VK_IMAGE_ASPECT_DEPTH_BIT_OK;
    case VK_FORMAT_D32_SFLOAT:          return VK_IMAGE_ASPECT_DEPTH_BIT_OK;
    case VK_FORMAT_S8_UINT:             return VK_IMAGE_ASPECT_STENCIL_BIT_OK;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT_OK | VK_IMAGE_ASPECT_STENCIL_BIT_OK;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT_OK;
    }
}

unsigned vk_format_to_pipe_format(unsigned vk_format)
{
    switch (vk_format) {
    /* 8-bit single */
    case VK_FORMAT_R8_UNORM:           return PIPE_FORMAT_R8_UNORM;
    case VK_FORMAT_R8_SNORM:           return PIPE_FORMAT_R8_SNORM;
    case VK_FORMAT_R8_USCALED:         return PIPE_FORMAT_R8_USCALED;
    case VK_FORMAT_R8_SSCALED:         return PIPE_FORMAT_R8_SSCALED;
    case VK_FORMAT_R8_UINT:            return PIPE_FORMAT_R8_UINT;
    case VK_FORMAT_R8_SINT:            return PIPE_FORMAT_R8_SINT;
    /* 8-bit dual */
    case VK_FORMAT_R8G8_UNORM:         return PIPE_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8_SNORM:         return PIPE_FORMAT_R8G8_SNORM;
    case VK_FORMAT_R8G8_USCALED:       return PIPE_FORMAT_R8G8_USCALED;
    case VK_FORMAT_R8G8_SSCALED:       return PIPE_FORMAT_R8G8_SSCALED;
    case VK_FORMAT_R8G8_UINT:          return PIPE_FORMAT_R8G8_UINT;
    case VK_FORMAT_R8G8_SINT:          return PIPE_FORMAT_R8G8_SINT;
    /* 8-bit triple */
    case VK_FORMAT_R8G8B8_UNORM:       return PIPE_FORMAT_R8G8B8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM:       return PIPE_FORMAT_R8G8B8_SNORM;
    case VK_FORMAT_R8G8B8_USCALED:     return PIPE_FORMAT_R8G8B8_USCALED;
    case VK_FORMAT_R8G8B8_SSCALED:     return PIPE_FORMAT_R8G8B8_SSCALED;
    case VK_FORMAT_R8G8B8_UINT:        return PIPE_FORMAT_R8G8B8_UINT;
    case VK_FORMAT_R8G8B8_SINT:        return PIPE_FORMAT_R8G8B8_SINT;
    /* 8-bit RGBA */
    case VK_FORMAT_R8G8B8A8_UNORM:     return PIPE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SNORM:     return PIPE_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8A8_USCALED:   return PIPE_FORMAT_R8G8B8A8_USCALED;
    case VK_FORMAT_R8G8B8A8_SSCALED:   return PIPE_FORMAT_R8G8B8A8_SSCALED;
    case VK_FORMAT_R8G8B8A8_UINT:      return PIPE_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8A8_SINT:      return PIPE_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R8G8B8A8_SRGB:      return PIPE_FORMAT_R8G8B8A8_SRGB;
    /* 8-bit BGRA — swap-chain format we want for compositor BGRA matches */
    case VK_FORMAT_B8G8R8A8_UNORM:     return PIPE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:      return PIPE_FORMAT_B8G8R8A8_SRGB;
    /* 10/10/10/2 */
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return PIPE_FORMAT_B10G10R10A2_UNORM;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return PIPE_FORMAT_R10G10B10A2_UNORM;
    /* 16-bit float / unorm */
    case VK_FORMAT_R16_UNORM:          return PIPE_FORMAT_R16_UNORM;
    case VK_FORMAT_R16_SFLOAT:         return PIPE_FORMAT_R16_FLOAT;
    case VK_FORMAT_R16G16_UNORM:       return PIPE_FORMAT_R16G16_UNORM;
    case VK_FORMAT_R16G16_SFLOAT:      return PIPE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R16G16B16A16_UNORM: return PIPE_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT:return PIPE_FORMAT_R16G16B16A16_FLOAT;
    /* 32-bit float / int */
    case VK_FORMAT_R32_UINT:           return PIPE_FORMAT_R32_UINT;
    case VK_FORMAT_R32_SINT:           return PIPE_FORMAT_R32_SINT;
    case VK_FORMAT_R32_SFLOAT:         return PIPE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R32G32_UINT:        return PIPE_FORMAT_R32G32_UINT;
    case VK_FORMAT_R32G32_SINT:        return PIPE_FORMAT_R32G32_SINT;
    case VK_FORMAT_R32G32_SFLOAT:      return PIPE_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R32G32B32_UINT:     return PIPE_FORMAT_R32G32B32_UINT;
    case VK_FORMAT_R32G32B32_SINT:     return PIPE_FORMAT_R32G32B32_SINT;
    case VK_FORMAT_R32G32B32_SFLOAT:   return PIPE_FORMAT_R32G32B32_FLOAT;
    case VK_FORMAT_R32G32B32A32_UINT:  return PIPE_FORMAT_R32G32B32A32_UINT;
    case VK_FORMAT_R32G32B32A32_SINT:  return PIPE_FORMAT_R32G32B32A32_SINT;
    case VK_FORMAT_R32G32B32A32_SFLOAT:return PIPE_FORMAT_R32G32B32A32_FLOAT;
    /* depth/stencil */
    case VK_FORMAT_D16_UNORM:          return PIPE_FORMAT_Z16_UNORM;
    case VK_FORMAT_X8_D24_UNORM_PACK32:return PIPE_FORMAT_Z24X8_UNORM;
    case VK_FORMAT_D32_SFLOAT:         return PIPE_FORMAT_Z32_FLOAT;
    case VK_FORMAT_S8_UINT:            return PIPE_FORMAT_S8_UINT;
    case VK_FORMAT_D24_UNORM_S8_UINT:  return PIPE_FORMAT_Z24_UNORM_S8_UINT;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return PIPE_FORMAT_Z32_FLOAT_S8X24_UINT;
    /* packed RGB */
    case VK_FORMAT_R5G6B5_UNORM_PACK16:return PIPE_FORMAT_B5G6R5_UNORM;
    case VK_FORMAT_B5G6R5_UNORM_PACK16:return PIPE_FORMAT_R5G6B5_UNORM;
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16: return PIPE_FORMAT_B5G5R5A1_UNORM;
    case VK_FORMAT_B5G5R5A1_UNORM_PACK16: return PIPE_FORMAT_R5G5B5A1_UNORM;
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return PIPE_FORMAT_B4G4R4A4_UNORM;
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16: return PIPE_FORMAT_R4G4B4A4_UNORM;
    default:
        /* Unknown format → "unsupported".  Zink interprets PIPE_FORMAT_NONE
         * correctly here (skips the format).  If a smoke-test failure
         * traces back to PIPE_FORMAT_NONE for an unmapped VkFormat, add
         * the case above. */
        return PIPE_FORMAT_NONE;
    }
}

unsigned vk_format_from_pipe_format(unsigned pipe_format)
{
    switch (pipe_format) {
    case PIPE_FORMAT_R8_UNORM:           return VK_FORMAT_R8_UNORM;
    case PIPE_FORMAT_R8_SNORM:           return VK_FORMAT_R8_SNORM;
    case PIPE_FORMAT_R8_UINT:            return VK_FORMAT_R8_UINT;
    case PIPE_FORMAT_R8_SINT:            return VK_FORMAT_R8_SINT;
    case PIPE_FORMAT_R8G8_UNORM:         return VK_FORMAT_R8G8_UNORM;
    case PIPE_FORMAT_R8G8_SNORM:         return VK_FORMAT_R8G8_SNORM;
    case PIPE_FORMAT_R8G8_UINT:          return VK_FORMAT_R8G8_UINT;
    case PIPE_FORMAT_R8G8_SINT:          return VK_FORMAT_R8G8_SINT;
    case PIPE_FORMAT_R8G8B8_UNORM:       return VK_FORMAT_R8G8B8_UNORM;
    case PIPE_FORMAT_R8G8B8A8_UNORM:     return VK_FORMAT_R8G8B8A8_UNORM;
    case PIPE_FORMAT_R8G8B8A8_SNORM:     return VK_FORMAT_R8G8B8A8_SNORM;
    case PIPE_FORMAT_R8G8B8A8_UINT:      return VK_FORMAT_R8G8B8A8_UINT;
    case PIPE_FORMAT_R8G8B8A8_SINT:      return VK_FORMAT_R8G8B8A8_SINT;
    case PIPE_FORMAT_R8G8B8A8_SRGB:      return VK_FORMAT_R8G8B8A8_SRGB;
    case PIPE_FORMAT_B8G8R8A8_UNORM:     return VK_FORMAT_B8G8R8A8_UNORM;
    case PIPE_FORMAT_B8G8R8A8_SRGB:      return VK_FORMAT_B8G8R8A8_SRGB;
    case PIPE_FORMAT_R10G10B10A2_UNORM:  return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case PIPE_FORMAT_B10G10R10A2_UNORM:  return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    case PIPE_FORMAT_R16_UNORM:          return VK_FORMAT_R16_UNORM;
    case PIPE_FORMAT_R16_FLOAT:          return VK_FORMAT_R16_SFLOAT;
    case PIPE_FORMAT_R16G16_UNORM:       return VK_FORMAT_R16G16_UNORM;
    case PIPE_FORMAT_R16G16_FLOAT:       return VK_FORMAT_R16G16_SFLOAT;
    case PIPE_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case PIPE_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case PIPE_FORMAT_R32_UINT:           return VK_FORMAT_R32_UINT;
    case PIPE_FORMAT_R32_SINT:           return VK_FORMAT_R32_SINT;
    case PIPE_FORMAT_R32_FLOAT:          return VK_FORMAT_R32_SFLOAT;
    case PIPE_FORMAT_R32G32_UINT:        return VK_FORMAT_R32G32_UINT;
    case PIPE_FORMAT_R32G32_SINT:        return VK_FORMAT_R32G32_SINT;
    case PIPE_FORMAT_R32G32_FLOAT:       return VK_FORMAT_R32G32_SFLOAT;
    case PIPE_FORMAT_R32G32B32A32_UINT:  return VK_FORMAT_R32G32B32A32_UINT;
    case PIPE_FORMAT_R32G32B32A32_SINT:  return VK_FORMAT_R32G32B32A32_SINT;
    case PIPE_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case PIPE_FORMAT_Z16_UNORM:          return VK_FORMAT_D16_UNORM;
    case PIPE_FORMAT_Z24X8_UNORM:        return VK_FORMAT_X8_D24_UNORM_PACK32;
    case PIPE_FORMAT_Z32_FLOAT:          return VK_FORMAT_D32_SFLOAT;
    case PIPE_FORMAT_S8_UINT:            return VK_FORMAT_S8_UINT;
    case PIPE_FORMAT_Z24_UNORM_S8_UINT:  return VK_FORMAT_D24_UNORM_S8_UINT;
    case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

#endif

/* W4.10++ — ASTC LUT stubs REMOVED: real impls now provided by
 * texcompress_astc_luts.o + texcompress_astc_luts_wrap.o (Granite
 * library, lifted with noop std::mutex shim in vendored header
 * since cross sysroot libstdc++ was built without _GLIBCXX_HAS_GTHREADS). */

/* W4.10++ — os_file POSIX helpers stubs removed. Real impls now in
 * arch/x86/libc/crtgl.c (real syscall wrappers around fcntl, open, getpid,
 * getrandom). */

/* ============================================================
 * (13) zink_reset_ds3_states — INTENTIONAL NO-OP
 * ============================================================
 * Upstream: mesa/src/gallium/drivers/zink/zink_state.c — invalidates the
 *           "dynamic state 3" cache when EXT_extended_dynamic_state3 is
 *           used.  Lives in a sub-path of zink_state.c not currently
 *           compiled into our libmesa_zink.a (the EXT_DS3 extension
 *           pipeline isn't wired up in our zink_compiler.c port).
 *
 * Path on smoke test: NEVER REACHED — zink_create_context only calls
 * this if screen->info.have_EXT_extended_dynamic_state3 is true; our
 * loader_dispatch.c reports the extension as unavailable.  See
 * arch/x86/lib/vulkan/loader_dispatch.c:vkEnumerateDeviceExtensionProperties.
 *
 * If we ever wire EXT_DS3, vendor zink_state.c into the build; the real
 * impl is ~200 LOC of memset+ctx->dirty bit manipulation.
 */
void zink_reset_ds3_states(void *ctx) {
    (void)ctx;
    /* Extension EXT_DS3 not advertised → caller never invokes us. */
}

/* ============================================================
 * (14) trace_get_possibly_threaded_context — INTENTIONAL passthrough
 * ============================================================
 * Upstream: mesa/src/gallium/auxiliary/driver_trace/tr_context.c:2608
 *
 * Real impl: if pipe->priv is a trace_context wrapper, peel it off and
 * return the inner pipe; otherwise return the input unchanged.  Since
 * trace_context_create_threaded above is a passthrough (no wrapping),
 * the input pipe NEVER has a trace wrapper to peel off — returning it
 * unchanged is correct.
 */
void *trace_get_possibly_threaded_context(void *pipe) {
    return pipe;  /* identity: no trace wrapper to unwrap */
}

/* ============================================================
 * (15) vk_spec_info_to_nir_spirv — NIR helper (smoke-path safe NULL)
 * ============================================================
 * Upstream: mesa/src/vulkan/util/vk_util.h:347 (inlined header helper).
 *           The real impl walks VkSpecializationInfo entries and emits
 *           a struct nir_spirv_specialization[count_out] array used by
 *           spirv_to_nir for SPIR-V specialization constants.
 *
 * Path on smoke test:
 *   zink_compiler.c:3401 calls this during shader compile.  Smoke test
 *   never compiles a user shader (no glShaderSource).  Zink's internal
 *   blit/clear paths use pre-compiled SPIR-V with NO spec consts.
 *
 * Returning NULL with *count_out=0 means "no spec constants" — exactly
 * the right answer when caller has no VkSpecializationInfo to convert.
 *
 * If we add SPIR-V shader compile, real impl is ~80 LOC: walk
 * spec_info->mapEntryCount, for each entry index into spec_info->pData
 * by .offset .size, copy into spec_entries[i] = { .id, .data, .defined_on_module=false }.
 * The nir_spirv_specialization struct lives in mesa/src/compiler/spirv/spirv.h.
 */
unsigned *vk_spec_info_to_nir_spirv(const void *vk_spec_info, void *count_out) {
    (void)vk_spec_info;
    if (count_out) *(unsigned *)count_out = 0;
    return (unsigned *)0;  /* "no spec consts" — safe for smoke path */
}

/* W4.9 — sections below removed: now provided by REAL C++ TUs compiled via
 * CXX_CROSS (ositok cross g++ + libstdc++ from sysroot).
 *
 * Removed (81 stubs total):
 *   - GLSL C++ frontend (uniform_query.cpp, shader_query.cpp etc.)
 *     _mesa_BindAttribLocation, _mesa_GetActiveUniform*, _mesa_uniform*,
 *     _mesa_program_resource_*, _mesa_glsl_*, _mesa_unpack_astc_2d_ldr, ...
 *   - GLSL deserialize/link (serialize.cpp + linker_util.cpp)
 *     serialize_glsl_program, deserialize_glsl_program, link_util_*,
 *     linker_error, linker_warning, interpolation_string, resource_name_updated
 *   - state_tracker (st_glsl_to_nir.cpp + st_atom_array.cpp)
 *     st_finalize_nir, st_link_shader, st_nir_lower_*, st_setup_*, st_update_array
 *   - string_to_uint_map_* (string_to_uint_map.cpp now in libmesa_compiler.a)
 *   - zink C++ TUs (zink_synchronization.cpp + zink_draw.cpp)
 *     zink_synchronization_init, zink_resource_image_barrier*_init,
 *     zink_get_cmdbuf, zink_get_gfx_pipeline_eq_func, zink_init_screen_pipeline_libs,
 *     zink_resource_buffer_transfer_dst_barrier
 */
