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

/* aaline / aapoint NIR fallbacks */
int nir_lower_aaline_fs(void *shader, int *vars, void *stipple_tex, void *stipple_sampler) { (void)shader;(void)vars;(void)stipple_tex;(void)stipple_sampler; return 0; }
int nir_lower_aapoint_fs(void *shader, int *vars, int coord_replace) { (void)shader;(void)vars;(void)coord_replace; return 0; }

/* ARB program parser */
int _mesa_parse_arb_program(void *ctx, unsigned target, const unsigned char *str, unsigned len, void *prog) { (void)ctx;(void)target;(void)str;(void)len;(void)prog; return 0; }

/* DRI option parser */
void driParseConfigFiles(void *cache, const void *info, int screen_no, const char *driver_name, const char *kernel_driver_name, const char *app_name, const char *app_version, const char *engine_name, const char *engine_version) { (void)cache;(void)info;(void)screen_no;(void)driver_name;(void)kernel_driver_name;(void)app_name;(void)app_version;(void)engine_name;(void)engine_version; }
unsigned char driQueryOptionb(const void *cache, const char *name) { (void)cache;(void)name; return 0; }

/* vk_format helpers (vk_format.c excluded) */
unsigned vk_format_aspects(unsigned format) { (void)format; return 0; }
unsigned vk_format_to_pipe_format(unsigned vk_format) { (void)vk_format; return 0; }
unsigned vk_format_from_pipe_format(unsigned pipe_format) { (void)pipe_format; return 0; }

/* ASTC decoder LUT */
void _mesa_init_astc_decoder_luts(void *luts) { (void)luts; }
const void *_mesa_get_astc_decoder_partition_table(unsigned block_w, unsigned block_h, unsigned partition_count) { (void)block_w;(void)block_h;(void)partition_count; return (void *)0; }

/* os_file Linux helpers */
int os_dupfd_cloexec(int fd) { (void)fd; return -1; }
int os_file_create_unique(const char *prefix, int filemode) { (void)prefix;(void)filemode; return -1; }
int os_same_file_description(int fd1, int fd2) { (void)fd1;(void)fd2; return -1; }

/* ZINK driver core (deferred since W4.3) */
void *zink_context_create(void *pscreen, void *priv, unsigned flags) { (void)pscreen;(void)priv;(void)flags; return (void *)0; }
void zink_screen_init_compiler(void *screen) { (void)screen; }
const void *zink_get_compiler_options(void *pscreen, unsigned ir, unsigned shader) { (void)pscreen;(void)ir;(void)shader; return (void *)0; }
void zink_compiler_assign_io(void *screen, void *producer, void *consumer) { (void)screen;(void)producer;(void)consumer; }
void *zink_shader_create(void *screen, void *nir) { (void)screen;(void)nir; return (void *)0; }
void  zink_shader_init(void *screen, void *zs) { (void)screen;(void)zs; }
void  zink_shader_free(void *screen, void *shader) { (void)screen;(void)shader; }
void  zink_gfx_shader_free(void *screen, void *shader) { (void)screen;(void)shader; }
void *zink_shader_compile(void *screen, int sep, void *base_nir, void *zs, unsigned int *non_fs_keybox, void *key) { (void)screen;(void)sep;(void)base_nir;(void)zs;(void)non_fs_keybox;(void)key; return (void *)0; }
void *zink_shader_compile_separate(void *screen, void *zs) { (void)screen;(void)zs; return (void *)0; }
void  zink_shader_finalize(void *pscreen, void *nirptr) { (void)pscreen;(void)nirptr; }
int   zink_shader_has_cubes(void *nir) { (void)nir; return 0; }
void *zink_shader_tcs_create(void *screen, void *vs, unsigned vertices_per_patch, void *key) { (void)screen;(void)vs;(void)vertices_per_patch;(void)key; return (void *)0; }
void  zink_shader_tcs_init(void *screen, void *zs, void *nir, unsigned vertices_per_patch) { (void)screen;(void)zs;(void)nir;(void)vertices_per_patch; }
void *zink_shader_tcs_compile(void *screen, void *zs, unsigned patch_vertices, int can_shobj) { (void)screen;(void)zs;(void)patch_vertices;(void)can_shobj; return (void *)0; }
void  zink_shader_serialize_blob(void *nir, void *blob) { (void)nir;(void)blob; }
void *zink_shader_deserialize(void *screen, void *zs) { (void)screen;(void)zs; return (void *)0; }
void *zink_shader_blob_deserialize(void *screen, void *blob) { (void)screen;(void)blob; return (void *)0; }
void *zink_tgsi_to_nir(void *screen, const void *tokens) { (void)screen;(void)tokens; return (void *)0; }
void  zink_lower_system_values_to_inlined_uniforms(void *nir) { (void)nir; }
void *zink_create_quads_emulation_gs(const void *options, const void *vs_nir, int flatshade_first, int alpha_test) { (void)options;(void)vs_nir;(void)flatshade_first;(void)alpha_test; return (void *)0; }
void zink_batch_no_rp(void *ctx) { (void)ctx; }
void zink_batch_rp(void *ctx) { (void)ctx; }
int  zink_check_batch_completion(void *ctx, uint64_t id) { (void)ctx;(void)id; return 1; }
void zink_cmd_debug_marker_begin(void *ctx, void *cmdbuf, const char *fmt) { (void)ctx;(void)cmdbuf;(void)fmt; }
void zink_cmd_debug_marker_end(void *ctx, void *cmdbuf) { (void)ctx;(void)cmdbuf; }
void zink_copy_buffer(void *ctx, void *dst, void *src, unsigned dst_offset, unsigned src_offset, unsigned size) { (void)ctx;(void)dst;(void)src;(void)dst_offset;(void)src_offset;(void)size; }
void zink_copy_image_buffer(void *ctx, void *dst, void *src, unsigned dst_level, unsigned dstx, unsigned dsty, unsigned dstz, unsigned src_level, const void *src_box, unsigned map_flags) { (void)ctx;(void)dst;(void)src;(void)dst_level;(void)dstx;(void)dsty;(void)dstz;(void)src_level;(void)src_box;(void)map_flags; }
int  zink_fence_wait(void *pctx) { (void)pctx; return 1; }
void zink_wait_on_batch(void *ctx, uint64_t batch_id) { (void)ctx;(void)batch_id; }
void zink_flush_memory_barrier(void *ctx, int is_compute) { (void)ctx;(void)is_compute; }
void zink_init_vk_sample_locations(void *ctx, void *vk_loc) { (void)ctx;(void)vk_loc; }
void zink_rebind_all_buffers(void *ctx) { (void)ctx; }
void zink_rebind_all_images(void *ctx) { (void)ctx; }
int  zink_resource_rebind(void *ctx, void *res) { (void)ctx;(void)res; return 0; }
void zink_reset_ds3_states(void *ctx) { (void)ctx; }
void zink_set_null_fs(void *ctx) { (void)ctx; }
void *zink_tc_context_unwrap(void *ctx) { return ctx; }
void zink_update_barriers(void *ctx, int compute, void *index, void *indirect, void *indirect_draw_count) { (void)ctx;(void)compute;(void)index;(void)indirect;(void)indirect_draw_count; }
void zink_update_descriptor_refs(void *ctx, int compute) { (void)ctx;(void)compute; }
void zink_update_fbfetch(void *ctx) { (void)ctx; }
void zink_update_rendering_info(void *ctx) { (void)ctx; }
void zink_update_shadow_samplerviews(void *ctx, unsigned mask) { (void)ctx;(void)mask; }

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
