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
#   - util/u_simple_shaders.c, u_tests.c, u_threaded_context.c,
#     u_vbuf.c — pull pipe_context / tgsi_ureg machinery that requires
#     larger subset
#   (W4.4 lifted u_blitter.c + u_gen_mipmap.c into the GALLIUM list —
#    they compile clean against the existing pipe-helper headers; the
#    runtime calls into u_simple_shaders.c are linker-undefined symbols
#    that the consumer (zink) will reach only on actual GL paths.)
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
OUT_ZINK     := $(BUILD_DIR)/libmesa_zink.a

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

# W4.3 — extended include set for gallium/drivers/zink/.
# Zink's includes pull from compiler/, vulkan/util/, and gallium/drivers/zink/
# itself (its own .h files). Plus our renderdoc_app.h vendor and the
# osito_compat shims for vk_struct_type_cast.h (generated upstream).
# Important: $(MESA_ROOT)/include comes FIRST so its vulkan_core.h
# (Mesa 25.0.0, VK_HEADER_VERSION 305) wins over the venus ICD's older
# 303 header in $(OSITOK_ROOT)/arch/x86/lib/vulkan/include — Mesa's
# generated dispatch table references VK_HEADER_VERSION 305 symbols.
INCLUDES_ZINK := \
  -I$(MESA_ROOT)/include \
  $(INCLUDES_COMPILER) \
  -I$(MESA_ROOT)/src/gallium/drivers/zink \
  -I$(MESA_ROOT)/src/gallium/auxiliary/nir \
  -I$(MESA_ROOT)/src/vulkan/util

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

# W4.3 — CFLAGS for gallium/drivers/zink/. Same as CFLAGS_COMPILER but
# with the zink include path and -DZINK_USE_DXIL=0 / -DZINK_DEBUG=0 to
# silence any feature-flag-bound code paths we don't need.
CFLAGS_ZINK := \
  -std=gnu11 -ffreestanding -nostdinc \
  -fno-stack-protector -mno-red-zone \
  -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-missing-field-initializers -Wno-pointer-arith \
  -Wno-sign-compare -Wno-misleading-indentation \
  -Wno-unused-variable -Wno-unused-but-set-variable \
  -Wno-implicit-fallthrough -Wno-switch -Wno-empty-body \
  -O2 -fno-pic -fvisibility=hidden \
  -D__OSITO_K__ -DZINK -DGALLIUM_HAVE_VK=1 \
  -DHAVE_FUNC_ATTRIBUTE_PACKED \
  -DHAVE_FUNC_ATTRIBUTE_FLATTEN \
  -DHAVE_FUNC_ATTRIBUTE_FORMAT \
  -DHAVE_FUNC_ATTRIBUTE_UNUSED \
  -DHAVE_FUNC_ATTRIBUTE_NORETURN \
  -DHAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT \
  -include $(OPENGL_ROOT)/osito_compat/mesa_compat.h \
  $(INCLUDES_ZINK) \
  -isystem $(GCC_INC)

# W4.4 — CXXFLAGS_ZINK for gallium/drivers/zink/{zink_draw,zink_synchronization}.cpp.
# Mirrors CFLAGS_ZINK but for C++17 + hosted libstdc++ (W4.2 pattern, used
# also by DXVK W5.0). Mesa's zink C++ TUs need <atomic>, template
# instantiations, and pull libstdc++ headers transparently via -isystem.
# Same -Wno-* set as CFLAGS_ZINK plus C++ specific quiets.
CXXFLAGS_ZINK := \
  -std=gnu++17 -fno-exceptions -fno-rtti \
  -ffreestanding -fno-stack-protector -mno-red-zone \
  -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-missing-field-initializers -Wno-pointer-arith \
  -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable \
  -Wno-non-virtual-dtor -Wno-deprecated-declarations \
  -Wno-class-memaccess -Wno-stringop-truncation \
  -Wno-implicit-fallthrough -Wno-switch -Wno-empty-body \
  -Wno-cast-function-type \
  -O2 -fno-pic -fvisibility=hidden \
  -D__OSITO_K__ -DZINK -DGALLIUM_HAVE_VK=1 \
  -DHAVE_FUNC_ATTRIBUTE_PACKED \
  -DHAVE_FUNC_ATTRIBUTE_FLATTEN \
  -DHAVE_FUNC_ATTRIBUTE_FORMAT \
  -DHAVE_FUNC_ATTRIBUTE_UNUSED \
  -DHAVE_FUNC_ATTRIBUTE_NORETURN \
  -DHAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT \
  -include $(OPENGL_ROOT)/osito_compat/mesa_compat.h \
  -I$(OPENGL_ROOT)/include \
  -I$(OPENGL_ROOT)/osito_compat \
  -I$(MESA_ROOT)/include \
  -I$(MESA_ROOT)/src \
  -I$(MESA_ROOT)/src/util \
  -I$(MESA_ROOT)/src/gallium/include \
  -I$(MESA_ROOT)/src/gallium/auxiliary \
  -I$(MESA_ROOT)/src/gallium/drivers/zink \
  -I$(MESA_ROOT)/src/gallium/auxiliary/nir \
  -I$(MESA_ROOT)/src/vulkan/util \
  -I$(MESA_ROOT)/src/compiler \
  -I$(MESA_ROOT)/src/compiler/nir \
  -I$(MESA_ROOT)/src/compiler/spirv \
  -I$(OSITOK_ROOT)/arch/x86/lib/vulkan/include \
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
  mesa/src/gallium/auxiliary/util/u_blitter.c \
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
  mesa/src/gallium/auxiliary/util/u_gen_mipmap.c \
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
  mesa/src/gallium/auxiliary/util/u_upload_mgr.c \
  mesa/src/gallium/auxiliary/nir/nir_to_spirv/nir_to_spirv.c \
  mesa/src/gallium/auxiliary/nir/nir_to_spirv/spirv_builder.c

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

# ---------------------------------------------------------------------------
# W4.3 — libmesa_zink.a
# ---------------------------------------------------------------------------
#
# Source list explicitly lists each .c we accept.  We start with the
# simplest leaves (zink_format, zink_lower_cubemap_to_array,
# zink_format_test) and grow the list as more headers / shims land.
#
# Excluded from the initial build (W4.3 - to be revisited in W4.4):
#   zink_compiler.c   — pulls in the GLSL→NIR pipeline + nir_to_spirv
#   zink_context.c    — large pipe_context implementation, ~37 includes
#   zink_screen.c     — driver entrypoint, depends on full Vulkan ICD
#   zink_draw.cpp     — c++17 driver hot path
#   zink_synchronization.cpp — c++17 sync primitives
#   (zink_program.c IS built but has a RUNTIME dep on util_blitter_*
#    which lives in u_blitter.c — that file is excluded from libmesa_gallium.a
#    at W4.1 deferral. zink_create_context will fail to link or hit an
#    undefined symbol at runtime in W4.4 unless u_blitter.c is added.
#    Tracked in W4.3 quality review.)
#
# Anything we couldn't get past header parse stays out and we document it.

ZINK_C_SRCS := \
  mesa/src/gallium/drivers/zink/zink_format.c \
  mesa/src/gallium/drivers/zink/zink_format_test.c \
  mesa/src/gallium/drivers/zink/zink_lower_cubemap_to_array.c \
  mesa/src/gallium/drivers/zink/zink_device_info.c \
  mesa/src/gallium/drivers/zink/zink_instance.c \
  mesa/src/gallium/drivers/zink/zink_nir_algebraic.c \
  mesa/src/gallium/drivers/zink/zink_bo.c \
  mesa/src/gallium/drivers/zink/zink_kopper.c \
  mesa/src/gallium/drivers/zink/zink_descriptors.c \
  mesa/src/gallium/drivers/zink/zink_framebuffer.c \
  mesa/src/gallium/drivers/zink/zink_query.c \
  mesa/src/gallium/drivers/zink/zink_fence.c \
  mesa/src/gallium/drivers/zink/zink_surface.c \
  mesa/src/gallium/drivers/zink/zink_batch.c \
  mesa/src/gallium/drivers/zink/zink_pipeline.c \
  mesa/src/gallium/drivers/zink/zink_state.c \
  mesa/src/gallium/drivers/zink/zink_render_pass.c \
  mesa/src/gallium/drivers/zink/zink_blit.c \
  mesa/src/gallium/drivers/zink/zink_clear.c \
  mesa/src/gallium/drivers/zink/zink_resource.c \
  mesa/src/gallium/drivers/zink/zink_program.c

# W4.4 — C++17 zink TUs. Built with hosted libstdc++ (CXXFLAGS_ZINK).
ZINK_CXX_SRCS := \
  mesa/src/gallium/drivers/zink/zink_draw.cpp \
  mesa/src/gallium/drivers/zink/zink_synchronization.cpp

# W4.4 — OsitoK-specific zink loader bridge + screen entrypoint.
# These live under osito_compat/ and become part of libmesa_zink.a so
# any zink-using ELF gets them transparently from one archive.
ZINK_LOADER_SRCS := \
  osito_compat/zink_vk_loader.c

ZINK_C_OBJS      := $(patsubst %.c,$(BUILD_DIR)/%.o,$(ZINK_C_SRCS))
ZINK_CXX_OBJS    := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(ZINK_CXX_SRCS))
ZINK_LOADER_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(ZINK_LOADER_SRCS))
ZINK_OBJS        := $(ZINK_C_OBJS) $(ZINK_CXX_OBJS) $(ZINK_LOADER_OBJS)

.PHONY: all clean util gallium compiler zink
all: $(OUT) $(OUT_GALLIUM) $(OUT_COMPILER) $(OUT_ZINK)
util: $(OUT)
gallium: $(OUT_GALLIUM)
compiler: $(OUT_COMPILER)
zink: $(OUT_ZINK)

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

$(OUT_ZINK): $(ZINK_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(ZINK_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

# W4.4 — bridge code for libmesa_zink.a uses CFLAGS_ZINK so the v305
# vulkan_core.h is in scope (matches the rest of the zink build).  Must
# come BEFORE the generic osito_compat/% rule.
$(BUILD_DIR)/osito_compat/zink_%.o: $(OPENGL_ROOT)/osito_compat/zink_%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    osito_compat/zink_$*"
	@$(CC) $(CFLAGS_ZINK) -c -o $@ $<

# Default pattern rule for util/gallium (build/lib/opengl/foo/bar.o : foo/bar.c)
$(BUILD_DIR)/osito_compat/%.o: $(OPENGL_ROOT)/osito_compat/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    $*"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/mesa/src/util/%.o: $(OPENGL_ROOT)/mesa/src/util/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    $*"
	@$(CC) $(CFLAGS) -c -o $@ $<

# nir_to_spirv requires the FULL zink include set (it includes
# zink_compiler.h transitively, which pulls vk_util.h, vk_dispatch_table.h,
# kopper_interface.h, etc.). Reuse CFLAGS_ZINK so we get matching headers
# AND vulkan_core.h v305 (avoids 303-vs-305 drift).
$(BUILD_DIR)/mesa/src/gallium/auxiliary/nir/nir_to_spirv/%.o: \
        $(OPENGL_ROOT)/mesa/src/gallium/auxiliary/nir/nir_to_spirv/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    nir_to_spirv/$*"
	@$(CC) $(CFLAGS_ZINK) -c -o $@ $<

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

# W4.3 pattern rule for gallium/drivers/zink/*.c -> CFLAGS_ZINK.
# Must come BEFORE the generic gallium rule to win precedence.
$(BUILD_DIR)/mesa/src/gallium/drivers/zink/%.o: $(MESA_ROOT)/src/gallium/drivers/zink/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    zink/$*"
	@$(CC) $(CFLAGS_ZINK) -c -o $@ $<

# W4.4 pattern rule for gallium/drivers/zink/*.cpp -> CXXFLAGS_ZINK.
$(BUILD_DIR)/mesa/src/gallium/drivers/zink/%.o: $(MESA_ROOT)/src/gallium/drivers/zink/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX   zink/$*"
	@$(CXX) $(CXXFLAGS_ZINK) -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)
	@echo "  CLEAN $(BUILD_DIR)"
