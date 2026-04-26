/* W4.8 link-time stubs for symbols deliberately deferred. */

#include <stdint.h>
#include <stddef.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t  u8;

/* W4.8++ BLAKE3 SIMD stubs removed — real impls now provided by
 * mesa/src/util/blake3/blake3_{sse2,sse41,avx2,avx512}_x86-64_unix.S
 * (compiled via BLAKE3_ASM_OBJS into libmesa_util.a).
 */

/* glcpp preprocessor stubs */
void *glcpp_parser_create(void *api, void *extensions, void *state) { (void)api;(void)extensions;(void)state; return (void *)0; }
void glcpp_lex_set_source_string(void *parser, const char *src) { (void)parser;(void)src; }
int  glcpp_parser_parse(void *parser) { (void)parser; return 0; }
void glcpp_parser_resolve_implicit_version(void *parser) { (void)parser; }
void glcpp_parser_destroy(void *parser) { (void)parser; }

/* W4.8++ — TGSI exec stubs removed (real impl from tgsi_exec.c in libmesa_gallium.a). */
/* W4.8++ — u_vbuf stubs removed (real impl from u_vbuf.c in libmesa_gallium.a). */

/* translate_sse2 (x86 rtasm JIT) — keep stubbed; rtasm JIT not vendored. */
void *translate_sse2_create(const void *key) { (void)key; return (void *)0; }

/* util_barrier */
int  util_barrier_init(void *barrier, unsigned count) { (void)barrier;(void)count; return 0; }
int  util_barrier_destroy(void *barrier) { (void)barrier; return 0; }
int  util_barrier_wait(void *barrier) { (void)barrier; return 0; }

/* u_cnd_monotonic */
int  u_cnd_monotonic_init(void *cnd) { (void)cnd; return 0; }
void u_cnd_monotonic_destroy(void *cnd) { (void)cnd; }
int  u_cnd_monotonic_broadcast(void *cnd) { (void)cnd; return 0; }
int  u_cnd_monotonic_wait(void *cnd, void *mtx) { (void)cnd;(void)mtx; return 0; }
int  u_cnd_monotonic_timedwait(void *cnd, void *mtx, const void *ts) { (void)cnd;(void)mtx;(void)ts; return 0; }

/* u_thread */
int u_thread_create(void *thread, void *(*routine)(void *), void *arg) { (void)thread;(void)routine;(void)arg; return -1; }
void u_thread_setname(const char *name) { (void)name; }
long util_thread_get_time_nano(int thread_id) { (void)thread_id; return 0; }

/* current cpu / affinity */
int util_get_current_cpu(void) { return 0; }
int util_set_thread_affinity(unsigned long thread, const void *mask, void *old_mask, unsigned mask_size) { (void)thread;(void)mask;(void)old_mask;(void)mask_size; return 0; }

/* trace driver wrapper. trace_dumping_*_locked() now real (tr_dump.c).
 * trace_context_create_threaded stays stubbed: tr_context.c needs tr_util.h
 * which is not vendored.
 */
void *trace_context_create_threaded(void *screen, void *pipe, void **replace_pipe, void *replace_data, unsigned flags) { (void)screen;(void)replace_pipe;(void)replace_data;(void)flags; return pipe; }

/* W4.10++ — nir_lower_aaline_fs / nir_lower_aapoint_fs stubs removed.
 * Real impls now provided by mesa/src/gallium/auxiliary/nir/nir_draw_helpers.c
 * (compiled into libmesa_gallium.a). */

/* ARB program parser */
int _mesa_parse_arb_program(void *ctx, unsigned target, const unsigned char *str, unsigned len, void *prog) { (void)ctx;(void)target;(void)str;(void)len;(void)prog; return 0; }

/* DRI option parser */
void driParseConfigFiles(void *cache, const void *info, int screen_no, const char *driver_name, const char *kernel_driver_name, const char *app_name, const char *app_version, const char *engine_name, const char *engine_version) { (void)cache;(void)info;(void)screen_no;(void)driver_name;(void)kernel_driver_name;(void)app_name;(void)app_version;(void)engine_name;(void)engine_version; }
unsigned char driQueryOptionb(const void *cache, const char *name) { (void)cache;(void)name; return 0; }

/* vk_format helpers — vk_format.c is NOT vendored under mesa/src/vulkan/util/.
 * Improved fallbacks: aspects returns COLOR_BIT (safe default) instead of 0
 * which could trigger assert(aspects != 0) on the depth/stencil branch.
 * Format conversions still return UNDEFINED (0) — real impls require the
 * generated 600-line vk_format_map[] table. */
#define VK_IMAGE_ASPECT_COLOR_BIT_OK 0x00000001
unsigned vk_format_aspects(unsigned format)
{
    /* Known depth/stencil VkFormat enum values (Vulkan spec) */
    switch (format) {
    case 124: return 0x00000002;                           /* D16_UNORM    -> DEPTH */
    case 125: return 0x00000002;                           /* X8_D24_UNORM */
    case 126: return 0x00000002;                           /* D32_SFLOAT   */
    case 127: return 0x00000004;                           /* S8_UINT      -> STENCIL */
    case 128: return 0x00000002 | 0x00000004;              /* D16_S8_UINT  */
    case 129: return 0x00000002 | 0x00000004;              /* D24_S8_UINT  */
    case 130: return 0x00000002 | 0x00000004;              /* D32_S8_UINT  */
    default:  return VK_IMAGE_ASPECT_COLOR_BIT_OK;
    }
}
unsigned vk_format_to_pipe_format(unsigned vk_format) { (void)vk_format; return 0; }
unsigned vk_format_from_pipe_format(unsigned pipe_format) { (void)pipe_format; return 0; }

/* ASTC decoder LUT */
void _mesa_init_astc_decoder_luts(void *luts) { (void)luts; }
const void *_mesa_get_astc_decoder_partition_table(unsigned block_w, unsigned block_h, unsigned partition_count) { (void)block_w;(void)block_h;(void)partition_count; return (void *)0; }

/* W4.10++ — os_file POSIX helpers stubs removed. Real impls now in
 * arch/x86/libc/crtgl.c (real syscall wrappers around fcntl, open, getpid,
 * getrandom). */

/* ZINK driver core — W4.10: 42 stubs lifted, real impls now in
 * libmesa_zink.a from zink_compiler.c + zink_context.c.
 * Remaining: zink_reset_ds3_states (lives in zink_state.c ds3 path,
 * not exported by current build) + 2 transitive deps from real zink TUs.
 */
void zink_reset_ds3_states(void *ctx) { (void)ctx; }

/* W4.10 — transitive deps pulled in by real zink_context.c + zink_compiler.c */
void *trace_get_possibly_threaded_context(void *pipe) { return pipe; }
void *vk_spec_info_to_nir_spirv(const void *vk_spec_info, void *count_out) { (void)vk_spec_info;(void)count_out; return (void *)0; }

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
