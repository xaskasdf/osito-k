# Build libmesa_util.a — OsitoK SDK static archive for Mesa's portable
# util/ subset, produced from vendored Mesa 25.0.0 source under mesa/.
#
# Invoked from arch/x86/Makefile (or directly via `make` in this dir).
# Mirrors the structure of arch/x86/lib/vulkan/Makefile.
#
# Source layout:
#   mesa/                -- vendored Mesa 25.0.0 (do NOT edit)
#   osito_compat/        -- our shims (force-included via -include)
#   build/lib/opengl/    -- where .o + .a land (under $(OSITOK_ROOT)/arch/x86/build)

OPENGL_ROOT := $(CURDIR)
OSITOK_ROOT ?= $(OPENGL_ROOT)/../../../..
MESA_ROOT   := $(OPENGL_ROOT)/mesa
BUILD_DIR   := $(OSITOK_ROOT)/arch/x86/build/lib/opengl
OUT         := $(BUILD_DIR)/libmesa_util.a

CC          ?= gcc
AR          ?= ar
GCC_INC     := $(shell $(CC) -print-file-name=include)

INCLUDES := \
  -I$(OPENGL_ROOT)/include \
  -I$(MESA_ROOT)/include \
  -I$(MESA_ROOT)/src \
  -I$(MESA_ROOT)/src/util \
  -I$(MESA_ROOT)/src/c11 \
  -I$(OSITOK_ROOT)/arch/x86/libc \
  -I$(OSITOK_ROOT)/arch/x86/include

CFLAGS := \
  -std=gnu11 -ffreestanding -nostdinc \
  -fno-stack-protector -mno-red-zone \
  -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-missing-field-initializers -Wno-pointer-arith \
  -Wno-sign-compare \
  -O2 -fno-pic -fvisibility=hidden \
  -DHAVE_PTHREAD=0 -D__OSITO_K__ \
  -include $(OPENGL_ROOT)/osito_compat/mesa_compat.h \
  $(INCLUDES) \
  -isystem $(GCC_INC)

# T3 — list of util/ files we currently know how to compile.
# Update this list as more files are validated.
UTIL_SRCS := \
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
  mesa/src/util/u_qsort.c \
  mesa/src/util/u_vector.c \
  mesa/src/util/u_worklist.c \
  mesa/src/util/vma.c

UTIL_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(UTIL_SRCS))

.PHONY: all clean
all: $(OUT)

$(OUT): $(UTIL_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(UTIL_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

# Pattern rule: build/lib/opengl/foo/bar.o : foo/bar.c
$(BUILD_DIR)/%.o: $(OPENGL_ROOT)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    $*"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)
	@echo "  CLEAN $(BUILD_DIR)"
