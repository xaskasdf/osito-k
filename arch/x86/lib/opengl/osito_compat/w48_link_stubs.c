/* W4.8 link-time stubs for symbols deliberately deferred. */

#include <stdint.h>
#include <stddef.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t  u8;

/* BLAKE3 SIMD: dispatcher falls back to portable when these are not chosen. */
void blake3_compress_in_place_sse2(u32 cv[8], const u8 block[64], u8 block_len, u64 counter, u8 flags) { (void)cv;(void)block;(void)block_len;(void)counter;(void)flags; }
void blake3_compress_xof_sse2(const u32 cv[8], const u8 block[64], u8 block_len, u64 counter, u8 flags, u8 out[64]) { (void)cv;(void)block;(void)block_len;(void)counter;(void)flags;(void)out; }
void blake3_hash_many_sse2(const u8 *const *inputs, size_t num_inputs, size_t blocks, const u32 key[8], u64 counter, int incr, u8 flags, u8 fs, u8 fe, u8 *out) { (void)inputs;(void)num_inputs;(void)blocks;(void)key;(void)counter;(void)incr;(void)flags;(void)fs;(void)fe;(void)out; }
void blake3_compress_in_place_sse41(u32 cv[8], const u8 block[64], u8 block_len, u64 counter, u8 flags) { (void)cv;(void)block;(void)block_len;(void)counter;(void)flags; }
void blake3_compress_xof_sse41(const u32 cv[8], const u8 block[64], u8 block_len, u64 counter, u8 flags, u8 out[64]) { (void)cv;(void)block;(void)block_len;(void)counter;(void)flags;(void)out; }
void blake3_hash_many_sse41(const u8 *const *inputs, size_t num_inputs, size_t blocks, const u32 key[8], u64 counter, int incr, u8 flags, u8 fs, u8 fe, u8 *out) { (void)inputs;(void)num_inputs;(void)blocks;(void)key;(void)counter;(void)incr;(void)flags;(void)fs;(void)fe;(void)out; }
void blake3_hash_many_avx2(const u8 *const *inputs, size_t num_inputs, size_t blocks, const u32 key[8], u64 counter, int incr, u8 flags, u8 fs, u8 fe, u8 *out) { (void)inputs;(void)num_inputs;(void)blocks;(void)key;(void)counter;(void)incr;(void)flags;(void)fs;(void)fe;(void)out; }
void blake3_compress_in_place_avx512(u32 cv[8], const u8 block[64], u8 block_len, u64 counter, u8 flags) { (void)cv;(void)block;(void)block_len;(void)counter;(void)flags; }
void blake3_compress_xof_avx512(const u32 cv[8], const u8 block[64], u8 block_len, u64 counter, u8 flags, u8 out[64]) { (void)cv;(void)block;(void)block_len;(void)counter;(void)flags;(void)out; }
void blake3_hash_many_avx512(const u8 *const *inputs, size_t num_inputs, size_t blocks, const u32 key[8], u64 counter, int incr, u8 flags, u8 fs, u8 fe, u8 *out) { (void)inputs;(void)num_inputs;(void)blocks;(void)key;(void)counter;(void)incr;(void)flags;(void)fs;(void)fe;(void)out; }

/* glcpp preprocessor stubs */
void *glcpp_parser_create(void *api, void *extensions, void *state) { (void)api;(void)extensions;(void)state; return (void *)0; }
void glcpp_lex_set_source_string(void *parser, const char *src) { (void)parser;(void)src; }
int  glcpp_parser_parse(void *parser) { (void)parser; return 0; }
void glcpp_parser_resolve_implicit_version(void *parser) { (void)parser; }
void glcpp_parser_destroy(void *parser) { (void)parser; }

/* TGSI software interpreter */
void *tgsi_exec_machine_create(int processor) { (void)processor; return (void *)0; }
void  tgsi_exec_machine_destroy(void *m) { (void)m; }
void  tgsi_exec_machine_bind_shader(void *mach, const void *tokens, void *sampler, void *image, void *buffer) { (void)mach;(void)tokens;(void)sampler;(void)image;(void)buffer; }
unsigned tgsi_exec_machine_run(void *mach, int start_pc) { (void)mach;(void)start_pc; return 0; }
void tgsi_exec_set_constant_buffers(void *mach, unsigned num_buffers, const void *bufs) { (void)mach;(void)num_buffers;(void)bufs; }

/* translate_sse2 (x86 rtasm JIT) */
void *translate_sse2_create(const void *key) { (void)key; return (void *)0; }

/* u_vbuf */
void *u_vbuf_create(void *pipe, const void *caps) { (void)pipe;(void)caps; return (void *)0; }
void  u_vbuf_destroy(void *mgr) { (void)mgr; }
void  u_vbuf_get_caps(void *screen, void *caps, unsigned flags) { (void)screen;(void)caps;(void)flags; }
void  u_vbuf_set_vertex_elements(void *mgr, const void *state) { (void)mgr;(void)state; }
void  u_vbuf_unset_vertex_elements(void *mgr) { (void)mgr; }
void  u_vbuf_set_vertex_buffers(void *mgr, unsigned count, const void *buffers) { (void)mgr;(void)count;(void)buffers; }
void  u_vbuf_save_vertex_elements(void *mgr) { (void)mgr; }
void  u_vbuf_restore_vertex_elements(void *mgr) { (void)mgr; }
void  u_vbuf_set_flatshade_first(void *mgr, int first) { (void)mgr;(void)first; }
void  u_vbuf_draw_vbo(void *mgr, const void *info, unsigned drawid_offset, const void *indirect, const void *draws, unsigned num_draws) { (void)mgr;(void)info;(void)drawid_offset;(void)indirect;(void)draws;(void)num_draws; }

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

/* trace driver wrapper */
void *trace_context_create_threaded(void *screen, void *pipe, void **replace_pipe, void *replace_data, unsigned flags) { (void)screen;(void)replace_pipe;(void)replace_data;(void)flags; return pipe; }
int  trace_dumping_enabled_locked(void) { return 0; }
void trace_dumping_start_locked(void) {}
void trace_dumping_stop_locked(void) {}

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

/* Additional zink internals exposed by zink_synchronization.cpp / draw.cpp (deferred C++ TUs) */
void *zink_get_cmdbuf(void *ctx, void *src, void *dst) { (void)ctx;(void)src;(void)dst; return (void *)0; }
void *zink_get_gfx_pipeline_eq_func(void *screen, void *prog) { (void)screen;(void)prog; return (void *)0; }
void  zink_init_screen_pipeline_libs(void *screen) { (void)screen; }
void  zink_resource_buffer_transfer_dst_barrier(void *ctx, void *res, unsigned offset, unsigned size) { (void)ctx;(void)res;(void)offset;(void)size; }
void  zink_resource_image_barrier_init(void *ctx, void *res, unsigned new_layout, unsigned new_access, unsigned new_pipeline) { (void)ctx;(void)res;(void)new_layout;(void)new_access;(void)new_pipeline; }
void  zink_resource_image_barrier2_init(void *ctx, void *res, unsigned new_layout, unsigned new_access, unsigned new_pipeline) { (void)ctx;(void)res;(void)new_layout;(void)new_access;(void)new_pipeline; }
void  zink_synchronization_init(void *screen) { (void)screen; }

/* GLSL C++ frontend deferral (uniform_query.cpp, shader_query.cpp, ir_function.cpp etc.) */
void _mesa_BindAttribLocation(unsigned program, unsigned index, const char *name) { (void)program;(void)index;(void)name; }
void _mesa_BindAttribLocation_no_error(unsigned program, unsigned index, const char *name) { (void)program;(void)index;(void)name; }
void _mesa_BindFragDataLocation(unsigned program, unsigned colorNumber, const char *name) { (void)program;(void)colorNumber;(void)name; }
void _mesa_BindFragDataLocationIndexed(unsigned program, unsigned colorNumber, unsigned index, const char *name) { (void)program;(void)colorNumber;(void)index;(void)name; }
void _mesa_BindFragDataLocationIndexed_no_error(unsigned program, unsigned colorNumber, unsigned index, const char *name) { (void)program;(void)colorNumber;(void)index;(void)name; }
void _mesa_BindFragDataLocation_no_error(unsigned program, unsigned colorNumber, const char *name) { (void)program;(void)colorNumber;(void)name; }
void _mesa_GetActiveAttrib(unsigned p, unsigned i, int sz, int *l, int *s, unsigned *t, char *n) { (void)p;(void)i;(void)sz;(void)l;(void)s;(void)t;(void)n; }
void _mesa_GetActiveUniform(unsigned p, unsigned i, int sz, int *l, int *s, unsigned *t, char *n) { (void)p;(void)i;(void)sz;(void)l;(void)s;(void)t;(void)n; }
void _mesa_GetActiveUniform_impl(void *ctx, unsigned p, unsigned i, int sz, int *l, int *s, unsigned *t, char *n, int x) { (void)ctx;(void)p;(void)i;(void)sz;(void)l;(void)s;(void)t;(void)n;(void)x; }
void _mesa_GetActiveUniformsiv(unsigned p, int c, const unsigned *i, unsigned pname, int *params) { (void)p;(void)c;(void)i;(void)pname;(void)params; }
int  _mesa_GetAttribLocation(unsigned program, const char *name) { (void)program;(void)name; return -1; }
int  _mesa_GetFragDataIndex(unsigned program, const char *name) { (void)program;(void)name; return -1; }
int  _mesa_GetFragDataLocation(unsigned program, const char *name) { (void)program;(void)name; return -1; }
unsigned _mesa_count_active_attribs(void *prog) { (void)prog; return 0; }
void *_mesa_create_program_resource_hash(void) { return (void *)0; }
int  _mesa_ensure_and_associate_uniform_storage(void *ctx, void *sh, void *prog, unsigned num) { (void)ctx;(void)sh;(void)prog;(void)num; return 0; }
void _mesa_flush_vertices_for_uniforms(void *ctx, const void *uni) { (void)ctx;(void)uni; }
void _mesa_get_program_interfaceiv(void *ctx, void *shProg, unsigned program_interface, unsigned pname, int *params) { (void)ctx;(void)shProg;(void)program_interface;(void)pname;(void)params; }
void _mesa_get_program_resource_name(void *ctx, void *shProg, unsigned pi, unsigned idx, int bufSize, int *length, char *name, int caller) { (void)ctx;(void)shProg;(void)pi;(void)idx;(void)bufSize;(void)length;(void)name;(void)caller; }
void _mesa_get_program_resourceiv(void *ctx, void *shProg, unsigned pi, unsigned idx, int propCount, const unsigned *props, int bufSize, int *length, int *params) { (void)ctx;(void)shProg;(void)pi;(void)idx;(void)propCount;(void)props;(void)bufSize;(void)length;(void)params; }
void _mesa_get_uniform(void *ctx, unsigned program, int location, int bufSize, unsigned returnType, void *paramsOut) { (void)ctx;(void)program;(void)location;(void)bufSize;(void)returnType;(void)paramsOut; }
void _mesa_glsl_builtin_functions_decref(void) {}
void _mesa_glsl_builtin_functions_init_or_ref(void) {}
int  _mesa_glsl_can_implicitly_convert(const void *from, const void *to, void *state) { (void)from;(void)to;(void)state; return 0; }
void _mesa_glsl_compile_shader(void *ctx, void *shader, int dump_ast, int dump_hir, int force_recompile) { (void)ctx;(void)shader;(void)dump_ast;(void)dump_hir;(void)force_recompile; }
const void *_mesa_glsl_get_builtin_uniform_desc(const char *name) { (void)name; return (void *)0; }
unsigned _mesa_longest_attribute_name_length(void *prog) { (void)prog; return 0; }
unsigned _mesa_program_resource_array_size(void *res) { (void)res; return 0; }
int  _mesa_program_resource_find_index(void *shProg, unsigned pi, unsigned idx) { (void)shProg;(void)pi;(void)idx; return -1; }
void *_mesa_program_resource_find_name(void *shProg, unsigned pi, const char *name, unsigned *idx) { (void)shProg;(void)pi;(void)name;(void)idx; return (void *)0; }
void _mesa_program_resource_hash_destroy(void *prog) { (void)prog; }
unsigned _mesa_program_resource_index(void *shProg, void *res) { (void)shProg;(void)res; return 0; }
int  _mesa_program_resource_location(void *shProg, unsigned pi, const char *name) { (void)shProg;(void)pi;(void)name; return -1; }
int  _mesa_program_resource_location_index(void *shProg, unsigned pi, const char *name) { (void)shProg;(void)pi;(void)name; return -1; }
const char *_mesa_program_resource_name(void *res) { (void)res; return ""; }
unsigned _mesa_program_resource_name_length(void *res) { (void)res; return 0; }
unsigned _mesa_program_resource_prop(void *shProg, void *res, unsigned idx, unsigned prop, int *val, int caller) { (void)shProg;(void)res;(void)idx;(void)prop;(void)val;(void)caller; return 0; }
void _mesa_propagate_uniforms_to_driver_storage(void *uni_storage, unsigned array_idx, unsigned count) { (void)uni_storage;(void)array_idx;(void)count; }
int  _mesa_sampler_uniforms_are_valid(const void *prog, char *err, unsigned errlen) { (void)prog;(void)err;(void)errlen; return 1; }
int  _mesa_sampler_uniforms_pipeline_are_valid(void *pip) { (void)pip; return 1; }
void _mesa_uniform(int location, int count, const void *values, void *ctx, void *prog, unsigned glsl_type) { (void)location;(void)count;(void)values;(void)ctx;(void)prog;(void)glsl_type; }
void _mesa_uniform_handle(int location, int count, const void *values, void *ctx, void *prog, unsigned glsl_type) { (void)location;(void)count;(void)values;(void)ctx;(void)prog;(void)glsl_type; }
void _mesa_uniform_matrix(int cols, int rows, int location, int count, unsigned char transpose, const void *values, void *ctx, void *prog, unsigned matrix_type) { (void)cols;(void)rows;(void)location;(void)count;(void)transpose;(void)values;(void)ctx;(void)prog;(void)matrix_type; }
void _mesa_unpack_astc_2d_ldr(uint8_t *dst_row, unsigned dst_stride, const uint8_t *src_row, unsigned src_stride, unsigned width, unsigned height, unsigned format) { (void)dst_row;(void)dst_stride;(void)src_row;(void)src_stride;(void)width;(void)height;(void)format; }
int  _mesa_validate_pipeline_io(void *pip) { (void)pip; return 1; }

/* GLSL deserialize / link helpers (serialize.cpp + linker_util.cpp deferral) */
void deserialize_glsl_program(void *blob, void *ctx, void *prog) { (void)blob;(void)ctx;(void)prog; }
void serialize_glsl_program(void *blob, void *ctx, void *prog) { (void)blob;(void)ctx;(void)prog; }
const char *interpolation_string(unsigned interp) { (void)interp; return ""; }
void link_util_add_program_resource(void *prog, void *resource_set, unsigned type, const void *data, unsigned char stages) { (void)prog;(void)resource_set;(void)type;(void)data;(void)stages; }
int  link_util_calculate_subroutine_compat(void *prog) { (void)prog; return 1; }
int  link_util_check_subroutine_resources(void *prog) { (void)prog; return 1; }
int  link_util_check_uniform_resources(void *ctx, void *prog) { (void)ctx;(void)prog; return 1; }
int  link_util_find_empty_block(void *prog, void *var) { (void)prog;(void)var; return -1; }
void link_util_mark_array_elements_referenced(const void *list, unsigned list_size, unsigned dim, unsigned mask) { (void)list;(void)list_size;(void)dim;(void)mask; }
int  link_util_parse_program_resource_name(const char *name, int n_len, int *array_index) { (void)name;(void)n_len;(void)array_index; return 0; }
int  link_util_should_add_buffer_variable(void *prog, void *block, int top_level_array_size, int top_level_array_stride, int row_major, int matrix_stride, unsigned packing) { (void)prog;(void)block;(void)top_level_array_size;(void)top_level_array_stride;(void)row_major;(void)matrix_stride;(void)packing; return 0; }
void link_util_update_empty_uniform_locations(void *prog) { (void)prog; }
void linker_error(void *prog, const char *fmt, ...) { (void)prog;(void)fmt; }
void linker_warning(void *prog, const char *fmt, ...) { (void)prog;(void)fmt; }
char *resource_name_updated(char *name) { return name; }

/* state_tracker (st_*) — st_glsl_to_nir.cpp + st_atom_array.cpp deferral */
void *st_create_gallium_vertex_state(void *st, const void *info, unsigned num_attrs) { (void)st;(void)info;(void)num_attrs; return (void *)0; }
void  st_finalize_nir(void *st, void *prog, void *shader_program, void *nir, int finalize_by_driver, int is_before_variants, int is_draw_shader) { (void)st;(void)prog;(void)shader_program;(void)nir;(void)finalize_by_driver;(void)is_before_variants;(void)is_draw_shader; }
void  st_init_update_array(void *st) { (void)st; }
int   st_link_shader(void *ctx, void *shader_program) { (void)ctx;(void)shader_program; return 1; }
int   st_nir_lower_samplers(void *screen, void *nir, void *shp, void *prog) { (void)screen;(void)nir;(void)shp;(void)prog; return 0; }
int   st_nir_lower_uniforms(void *st, void *nir) { (void)st;(void)nir; return 0; }
int   st_nir_lower_wpos_ytransform(void *nir, void *prog, void *screen) { (void)nir;(void)prog;(void)screen; return 0; }
void  st_setup_arrays(void *st, const void *vp, const void *ve_inputs, void *ve_state, void *vbuffers, unsigned *num_vbuffers, int *has_user_vertex_buffers) { (void)st;(void)vp;(void)ve_inputs;(void)ve_state;(void)vbuffers;(void)num_vbuffers;(void)has_user_vertex_buffers; }
void  st_setup_current_user(void *st, const void *vp, const void *vs, const void *ve_inputs, void *ve_state, void *vbuffers, unsigned *num_vbuffers) { (void)st;(void)vp;(void)vs;(void)ve_inputs;(void)ve_state;(void)vbuffers;(void)num_vbuffers; }
void  st_update_array(void *st) { (void)st; }

/* string_to_uint_map_*  — C++ container shimmed as a no-op map. */
void *string_to_uint_map_ctor(void) { return (void *)0; }
void  string_to_uint_map_dtor(void *m) { (void)m; }
int   string_to_uint_map_get(void *m, unsigned *value, const char *key) { (void)m;(void)key; if (value) *value = 0; return 0; }
void  string_to_uint_map_put(void *m, unsigned value, const char *key) { (void)m;(void)value;(void)key; }
