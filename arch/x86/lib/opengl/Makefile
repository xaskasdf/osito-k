# Build Mesa static libraries — OsitoK SDK archives produced from vendored
# Mesa 25.0.0 source under mesa/.
#
# Invoked from arch/x86/Makefile (or directly via `make` in this dir).
# Mirrors the structure of arch/x86/lib/vulkan/Makefile.
#
# Source layout:
#   mesa/                -- vendored Mesa 25.0.0 (do NOT edit)
#   osito_compat/        -- our shims (force-included via -include)
#   build/lib/opengl/    -- where .o + .a land (under $(OSITOK_ROOT)/arch/x86/build)
#
# Targets:
#   libmesa_util.a      -- W4.0: portable util/ subset (hash, ralloc, etc.)
#   libmesa_gallium.a   -- W4.1: gallium auxiliary layer (pipe helpers,
#                          cso_cache, tgsi, translate, pipebuffer, etc.)
#
# W4.1 exclusions (from gallium/auxiliary/):
#   - gallivm/       LLVM JIT (not vendored)
#   - hud/           curses dependency (not vendored)
#   - pipe-loader/   dlopen ICD loader (not vendored)
#   - rtasm/         x86 runtime asm JIT (not vendored)
#   - vl/            video decode (not vendored)
#   - draw/          entangled with gallivm + tgsi_exec; deferred to W4.2+
#   - nir/           builds on compiler/nir — deferred to W4.3
#   - postprocess/   needs draw_* helpers
#   - target-helpers/ driver-loader dispatch, not needed for Zink
#   - driver_ddebug/ / driver_noop/ / driver_trace/ wrapper drivers
#   - renderonly/    DRM-KMS split-gpu helper (needs fcntl.h / ioctl / drmXxx)
#   - tessellator/   .cpp; C++ surface handled in W4.2
#   - util/u_blitter.c, u_gen_mipmap.c, u_simple_shaders.c, u_tests.c,
#     u_threaded_context.c, u_vbuf.c — pull pipe_context / tgsi_ureg
#     machinery that requires larger subset
#   - tgsi/tgsi_exec.c  -- soft fp16/double runtime, heavy dependencies
#   - indices/u_primconvert.c -- calls pipe_context hooks deferred to driver wave
#   - pipebuffer/pb_*_debug.c / pb_bufmgr_slab.c  -- assorted debug helpers

OPENGL_ROOT := $(CURDIR)
OSITOK_ROOT ?= $(OPENGL_ROOT)/../../../..
MESA_ROOT   := $(OPENGL_ROOT)/mesa
BUILD_DIR   := $(OSITOK_ROOT)/arch/x86/build/lib/opengl
OUT          := $(BUILD_DIR)/libmesa_util.a
OUT_GALLIUM  := $(BUILD_DIR)/libmesa_gallium.a
OUT_COMPILER := $(BUILD_DIR)/libmesa_compiler.a

CC          ?= gcc
CXX         ?= g++
AR          ?= ar
GCC_INC     := $(shell $(CC) -print-file-name=include)

INCLUDES := \
  -I$(OPENGL_ROOT)/include \
  -I$(OPENGL_ROOT)/osito_compat \
  -I$(OSITOK_ROOT)/arch/x86/libc/qjs_headers \
  -I$(OSITOK_ROOT)/arch/x86/libc \
  -I$(OSITOK_ROOT)/arch/x86/include \
  -I$(MESA_ROOT)/include \
  -I$(MESA_ROOT)/src \
  -I$(MESA_ROOT)/src/util \
  -I$(MESA_ROOT)/src/c11 \
  -I$(MESA_ROOT)/src/gallium/include \
  -I$(MESA_ROOT)/src/gallium/auxiliary

# W4.2 — extended include set for compiler/{glsl,nir,spirv}/
INCLUDES_COMPILER := \
  $(INCLUDES) \
  -I$(MESA_ROOT)/src/compiler \
  -I$(MESA_ROOT)/src/compiler/glsl \
  -I$(MESA_ROOT)/src/compiler/nir \
  -I$(MESA_ROOT)/src/compiler/spirv \
  -I$(OSITOK_ROOT)/arch/x86/lib/vulkan/include

# W4.2 — C++ uses HOSTED gcc:12 libstdc++ (no -nostdinc, no -nostdinc++).
# We DROP -Imesa/src/c11 because it shadows glibc's <time.h> with mesa's
# polyfill (which expects time_t to be pre-declared). All Mesa users
# of c11/* already use the explicit "c11/threads.h" path.
INCLUDES_COMPILER_CXX := \
  -I$(OPENGL_ROOT)/include \
  -I$(OPENGL_ROOT)/osito_compat \
  -I$(MESA_ROOT)/include \
  -I$(MESA_ROOT)/src \
  -I$(MESA_ROOT)/src/util \
  -I$(MESA_ROOT)/src/gallium/include \
  -I$(MESA_ROOT)/src/gallium/auxiliary \
  -I$(MESA_ROOT)/src/compiler \
  -I$(MESA_ROOT)/src/compiler/glsl \
  -I$(MESA_ROOT)/src/compiler/nir \
  -I$(MESA_ROOT)/src/compiler/spirv \
  -I$(OSITOK_ROOT)/arch/x86/lib/vulkan/include

CFLAGS := \
  -std=gnu11 -ffreestanding -nostdinc \
  -fno-stack-protector -mno-red-zone \
  -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-missing-field-initializers -Wno-pointer-arith \
  -Wno-sign-compare \
  -O2 -fno-pic -fvisibility=hidden \
  -D__OSITO_K__ \
  -DHAVE_FUNC_ATTRIBUTE_PACKED \
  -DHAVE_FUNC_ATTRIBUTE_FLATTEN \
  -DHAVE_FUNC_ATTRIBUTE_FORMAT \
  -DHAVE_FUNC_ATTRIBUTE_UNUSED \
  -DHAVE_FUNC_ATTRIBUTE_NORETURN \
  -DHAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT \
  -include $(OPENGL_ROOT)/osito_compat/mesa_compat.h \
  $(INCLUDES) \
  -isystem $(GCC_INC)

# W4.2 — CFLAGS for compiler/nir/. Same as base CFLAGS but with the
# compiler/* includes, plus -Wno-misleading-indentation for the
# 32 MB nir_opt_algebraic.c (Mesa upstream does the same — see
# nir/meson.build comment).
CFLAGS_COMPILER := \
  -std=gnu11 -ffreestanding -nostdinc \
  -fno-stack-protector -mno-red-zone \
  -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-missing-field-initializers -Wno-pointer-arith \
  -Wno-sign-compare -Wno-misleading-indentation \
  -O2 -fno-pic -fvisibility=hidden \
  -D__OSITO_K__ \
  -DHAVE_FUNC_ATTRIBUTE_PACKED \
  -DHAVE_FUNC_ATTRIBUTE_FLATTEN \
  -DHAVE_FUNC_ATTRIBUTE_FORMAT \
  -DHAVE_FUNC_ATTRIBUTE_UNUSED \
  -DHAVE_FUNC_ATTRIBUTE_NORETURN \
  -DHAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT \
  -include $(OPENGL_ROOT)/osito_compat/mesa_compat.h \
  $(INCLUDES_COMPILER) \
  -isystem $(GCC_INC)

# W4.2 — CXXFLAGS for compiler/glsl/.  Hosted libstdc++ (no -nostdinc).
# Same flag pattern as DXVK's W5.0 build:
#   -std=gnu++17 -fno-exceptions -fno-rtti
# We DON'T set -nostdinc because the GLSL frontend uses <vector>,
# <string>, <unordered_map> from libstdc++. The -isystem GCC_INC line
# pulls in /usr/local/include/c++/12/ for libstdc++ headers and
# /usr/include/ glibc headers transparently.
CXXFLAGS_COMPILER := \
  -std=gnu++17 -fno-exceptions -fno-rtti \
  -ffreestanding -fno-stack-protector -mno-red-zone \
  -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-missing-field-initializers -Wno-pointer-arith \
  -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable \
  -Wno-non-virtual-dtor -Wno-deprecated-declarations \
  -Wno-class-memaccess -Wno-stringop-truncation \
  -O2 -fno-pic -fvisibility=hidden \
  -D__OSITO_K__ \
  -DHAVE_FUNC_ATTRIBUTE_PACKED \
  -DHAVE_FUNC_ATTRIBUTE_FLATTEN \
  -DHAVE_FUNC_ATTRIBUTE_FORMAT \
  -DHAVE_FUNC_ATTRIBUTE_UNUSED \
  -DHAVE_FUNC_ATTRIBUTE_NORETURN \
  -DHAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT \
  -include $(OPENGL_ROOT)/osito_compat/mesa_compat.h \
  $(INCLUDES_COMPILER_CXX) \
  -isystem $(GCC_INC)

# T3 — list of util/ files we currently know how to compile.
# Update this list as more files are validated.
UTIL_SRCS := \
  osito_compat/mesa_libc_stubs.c \
  mesa/src/util/bitscan.c \
  mesa/src/util/blob.c \
  mesa/src/util/crc32.c \
  mesa/src/util/dag.c \
  mesa/src/util/double.c \
  mesa/src/util/fast_idiv_by_const.c \
  mesa/src/util/half_float.c \
  mesa/src/util/hash_table.c \
  mesa/src/util/log.c \
  mesa/src/util/memstream.c \
  mesa/src/util/mesa-sha1.c \
  mesa/src/util/os_misc.c \
  mesa/src/util/os_time.c \
  mesa/src/util/parson.c \
  mesa/src/util/ralloc.c \
  mesa/src/util/rb_tree.c \
  mesa/src/util/register_allocate.c \
  mesa/src/util/rgtc.c \
  mesa/src/util/set.c \
  mesa/src/util/sha1/sha1.c \
  mesa/src/util/simple_mtx.c \
  mesa/src/util/slab.c \
  mesa/src/util/softfloat.c \
  mesa/src/util/sparse_array.c \
  mesa/src/util/string_buffer.c \
  mesa/src/util/strtod.c \
  mesa/src/util/u_atomic.c \
  mesa/src/util/u_call_once.c \
  mesa/src/util/u_dynarray.c \
  mesa/src/util/u_hash_table.c \
  mesa/src/util/u_idalloc.c \
  mesa/src/util/u_math.c \
  mesa/src/util/u_printf.c \
  mesa/src/util/u_vector.c \
  mesa/src/util/u_worklist.c \
  mesa/src/util/vma.c

UTIL_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(UTIL_SRCS))

# ---------------------------------------------------------------------------
# W4.1 — libmesa_gallium.a
# ---------------------------------------------------------------------------
# src/util/format/ stays in util/ layout (format table generated by
# u_format_table.py — pre-generated into the tree so we don't need python
# at build time).
GALLIUM_SRCS := \
  mesa/src/util/format_srgb.c \
  mesa/src/util/format/u_format.c \
  mesa/src/util/format/u_format_bptc.c \
  mesa/src/util/format/u_format_etc.c \
  mesa/src/util/format/u_format_fxt1.c \
  mesa/src/util/format/u_format_latc.c \
  mesa/src/util/format/u_format_other.c \
  mesa/src/util/format/u_format_rgtc.c \
  mesa/src/util/format/u_format_s3tc.c \
  mesa/src/util/format/u_format_table.c \
  mesa/src/util/format/u_format_yuv.c \
  mesa/src/util/format/u_format_zs.c \
  mesa/src/gallium/auxiliary/cso_cache/cso_cache.c \
  mesa/src/gallium/auxiliary/cso_cache/cso_hash.c \
  mesa/src/gallium/auxiliary/indices/u_indices_gen.c \
  mesa/src/gallium/auxiliary/indices/u_unfilled_gen.c \
  mesa/src/gallium/auxiliary/pipebuffer/pb_buffer_fenced.c \
  mesa/src/gallium/auxiliary/pipebuffer/pb_cache.c \
  mesa/src/gallium/auxiliary/pipebuffer/pb_slab.c \
  mesa/src/gallium/auxiliary/pipebuffer/pb_validate.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_aa_point.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_build.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_dump.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_dynamic_indexing.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_from_mesa.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_info.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_iterate.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_lowering.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_parse.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_point_sprite.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_sanity.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_scan.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_strings.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_text.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_transform.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_ureg.c \
  mesa/src/gallium/auxiliary/tgsi/tgsi_util.c \
  mesa/src/gallium/auxiliary/translate/translate.c \
  mesa/src/gallium/auxiliary/translate/translate_cache.c \
  mesa/src/gallium/auxiliary/translate/translate_generic.c \
  mesa/src/gallium/auxiliary/util/u_async_debug.c \
  mesa/src/gallium/auxiliary/util/u_bitmask.c \
  mesa/src/gallium/auxiliary/util/u_cache.c \
  mesa/src/gallium/auxiliary/util/u_debug_describe.c \
  mesa/src/gallium/auxiliary/util/u_debug_flush.c \
  mesa/src/gallium/auxiliary/util/u_debug_image.c \
  mesa/src/gallium/auxiliary/util/u_debug_refcnt.c \
  mesa/src/gallium/auxiliary/util/u_draw.c \
  mesa/src/gallium/auxiliary/util/u_draw_quad.c \
  mesa/src/gallium/auxiliary/util/u_driconf.c \
  mesa/src/gallium/auxiliary/util/u_dump_defines.c \
  mesa/src/gallium/auxiliary/util/u_dump_state.c \
  mesa/src/gallium/auxiliary/util/u_framebuffer.c \
  mesa/src/gallium/auxiliary/util/u_handle_table.c \
  mesa/src/gallium/auxiliary/util/u_helpers.c \
  mesa/src/gallium/auxiliary/util/u_index_modify.c \
  mesa/src/gallium/auxiliary/util/u_log.c \
  mesa/src/gallium/auxiliary/util/u_prim.c \
  mesa/src/gallium/auxiliary/util/u_prim_restart.c \
  mesa/src/gallium/auxiliary/util/u_pstipple.c \
  mesa/src/gallium/auxiliary/util/u_resource.c \
  mesa/src/gallium/auxiliary/util/u_sample_positions.c \
  mesa/src/gallium/auxiliary/util/u_sampler.c \
  mesa/src/gallium/auxiliary/util/u_screen.c \
  mesa/src/gallium/auxiliary/util/u_split_draw.c \
  mesa/src/gallium/auxiliary/util/u_suballoc.c \
  mesa/src/gallium/auxiliary/util/u_surface.c \
  mesa/src/gallium/auxiliary/util/u_texture.c \
  mesa/src/gallium/auxiliary/util/u_tile.c \
  mesa/src/gallium/auxiliary/util/u_transfer.c \
  mesa/src/gallium/auxiliary/util/u_transfer_helper.c \
  mesa/src/gallium/auxiliary/util/u_upload_mgr.c

GALLIUM_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(GALLIUM_SRCS))

# ---------------------------------------------------------------------------
# W4.2 — libmesa_compiler.a
# ---------------------------------------------------------------------------
#
# Sources:
#   * compiler top-level (3): glsl_types.c, builtin_types.c, shader_enums.c
#     (compiled as C — both use C99 designated initialisers in declaration
#      order that gcc rejects under C++ rules)
#   * compiler/nir (209): all .c files INCLUDING the four pre-generated
#     codegen files (nir_opt_algebraic.c — 32 MB, nir_intrinsics.c,
#     nir_constant_expressions.c, nir_opcodes.c).
#     -Wno-misleading-indentation drops the gcc note that flags the giant
#     generated nir_opt_algebraic.c indentation; Mesa upstream does the
#     same in nir/meson.build.
#   * compiler/spirv (15): spirv_to_nir.c + vtn_*.c + spirv_info.c +
#     vtn_gather_types.c. spirv2nir.c (standalone driver) is excluded
#     because it pulls <fcntl.h> for stdin reading.
#   * compiler/glsl (17 of 55 .cpp files): only the AST/IR helpers
#     that don't transitively #include "main/macros.h", "main/mtypes.h",
#     "program/symbol_table.h", or "main/consts_exts.h". Those headers
#     belong to mesa/src/mesa/main/ — Mesa's GL state-tracker, which is
#     a separate ~MB-scale chunk we'll vendor in a later wave (or skip
#     entirely if Zink ends up not needing the legacy GL→GLSL path).
#
# W4.2 exclusions (from compiler/glsl/), tracked here so a later wave
# can lift them when mesa/main/ ships:
#   ast_array_index.cpp ast_function.cpp ast_to_hir.cpp ast_type.cpp
#   builtin_functions.cpp builtin_types.cpp builtin_variables.cpp
#   glsl_lexer.cpp glsl_parser.cpp glsl_parser_extras.cpp
#   glsl_symbol_table.cpp ir.cpp ir_function.cpp
#   ir_function_detect_recursion.cpp ir_print_visitor.cpp ir_validate.cpp
#   gl_nir_*.cpp (12 files — link/lower/etc, all need main/mtypes.h)
#   glsl_to_nir.cpp (needs float64_glsl.h xxd-embedded resource)
#   linker_util.cpp lower_distance.cpp lower_int64.cpp lower_precision.cpp
#   lower_*.cpp (the ones still needing main/macros.h)
#   opt_algebraic.cpp opt_*.cpp (the ones needing main/macros.h)
#   serialize.cpp shader_cache.cpp standalone.cpp
#   standalone_scaffolding.cpp main.cpp
#
# The 17 included GLSL .cpp files give us enough surface to do basic
# IR walking + cloning + visitor patterns; full GLSL→NIR is a W4.3+
# task once mesa/main/ + xxd float64_glsl.h are vendored.

# C sources for libmesa_compiler.a (compiled as C: top-level + nir + spirv).
# Wildcards are evaluated at make-time; one-shot generation.
COMPILER_NIR_SRCS    := $(wildcard $(MESA_ROOT)/src/compiler/nir/*.c)
COMPILER_SPIRV_SRCS  := $(filter-out $(MESA_ROOT)/src/compiler/spirv/spirv2nir.c, \
                                     $(wildcard $(MESA_ROOT)/src/compiler/spirv/*.c))
COMPILER_TOP_SRCS    := \
  $(MESA_ROOT)/src/compiler/glsl_types.c \
  $(MESA_ROOT)/src/compiler/builtin_types.c \
  $(MESA_ROOT)/src/compiler/shader_enums.c

COMPILER_C_SRCS_ABS  := $(COMPILER_TOP_SRCS) $(COMPILER_NIR_SRCS) $(COMPILER_SPIRV_SRCS)
# strip $(OPENGL_ROOT)/ prefix to get relative paths for the build rule
COMPILER_C_SRCS      := $(patsubst $(OPENGL_ROOT)/%,%,$(COMPILER_C_SRCS_ABS))

# C++ sources for libmesa_compiler.a — minimal GLSL frontend pieces.
COMPILER_CXX_SRCS := \
  mesa/src/compiler/glsl/ir_basic_block.cpp \
  mesa/src/compiler/glsl/ir_clone.cpp \
  mesa/src/compiler/glsl/ir_constant_expression.cpp \
  mesa/src/compiler/glsl/ir_expression_flattening.cpp \
  mesa/src/compiler/glsl/ir_hierarchical_visitor.cpp \
  mesa/src/compiler/glsl/ir_hv_accept.cpp \
  mesa/src/compiler/glsl/ir_rvalue_visitor.cpp \
  mesa/src/compiler/glsl/ir_variable_refcount.cpp \
  mesa/src/compiler/glsl/lower_builtins.cpp \
  mesa/src/compiler/glsl/lower_jumps.cpp \
  mesa/src/compiler/glsl/lower_mat_op_to_vec.cpp \
  mesa/src/compiler/glsl/lower_packing_builtins.cpp \
  mesa/src/compiler/glsl/opt_dead_code.cpp \
  mesa/src/compiler/glsl/opt_flatten_nested_if_blocks.cpp \
  mesa/src/compiler/glsl/opt_function_inlining.cpp \
  mesa/src/compiler/glsl/opt_if_simplification.cpp \
  mesa/src/compiler/glsl/opt_tree_grafting.cpp

COMPILER_C_OBJS   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(COMPILER_C_SRCS))
COMPILER_CXX_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(COMPILER_CXX_SRCS))
COMPILER_OBJS     := $(COMPILER_C_OBJS) $(COMPILER_CXX_OBJS)

.PHONY: all clean util gallium compiler
all: $(OUT) $(OUT_GALLIUM) $(OUT_COMPILER)
util: $(OUT)
gallium: $(OUT_GALLIUM)
compiler: $(OUT_COMPILER)

$(OUT): $(UTIL_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(UTIL_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

$(OUT_GALLIUM): $(GALLIUM_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(GALLIUM_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

$(OUT_COMPILER): $(COMPILER_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(COMPILER_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

# Default pattern rule for util/gallium (build/lib/opengl/foo/bar.o : foo/bar.c)
$(BUILD_DIR)/osito_compat/%.o: $(OPENGL_ROOT)/osito_compat/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    $*"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/mesa/src/util/%.o: $(OPENGL_ROOT)/mesa/src/util/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    $*"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/mesa/src/gallium/%.o: $(OPENGL_ROOT)/mesa/src/gallium/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    $*"
	@$(CC) $(CFLAGS) -c -o $@ $<

# W4.2 pattern rules for compiler/{nir,spirv,top}/ — use CFLAGS_COMPILER.
$(BUILD_DIR)/mesa/src/compiler/nir/%.o: $(MESA_ROOT)/src/compiler/nir/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    nir/$*"
	@$(CC) $(CFLAGS_COMPILER) -c -o $@ $<

$(BUILD_DIR)/mesa/src/compiler/spirv/%.o: $(MESA_ROOT)/src/compiler/spirv/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    spirv/$*"
	@$(CC) $(CFLAGS_COMPILER) -c -o $@ $<

$(BUILD_DIR)/mesa/src/compiler/%.o: $(MESA_ROOT)/src/compiler/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    compiler/$*"
	@$(CC) $(CFLAGS_COMPILER) -c -o $@ $<

# W4.2 pattern rule for compiler/glsl/*.cpp -> CXX_FLAGS_COMPILER.
$(BUILD_DIR)/mesa/src/compiler/glsl/%.o: $(MESA_ROOT)/src/compiler/glsl/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX   glsl/$*"
	@$(CXX) $(CXXFLAGS_COMPILER) -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)
	@echo "  CLEAN $(BUILD_DIR)"
