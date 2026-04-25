# arch/x86/lib/dxvk/Makefile -- W5.0/W5.1 build infrastructure for DXVK on OsitoK.
#
# Produces three static archives:
#   $(BUILD_DIR)/libdxvk_util.a   -- DXVK src/util/  (hash maps, log, etc.)
#   $(BUILD_DIR)/libdxvk_spirv.a  -- DXVK src/spirv/ (SPIR-V module builder)
#   $(BUILD_DIR)/libdxvk_dxbc.a   -- DXVK src/dxbc/  (D3D11 bytecode -> SPIR-V)
#
# Both compiled C++17 with -fno-exceptions -fno-rtti, force-including the
# osito_compat/dxvk_compat.h shim so the upstream sources compile unchanged.
#
# Invoked by:
#   docker run --rm -v $HOME/osito-k:/osito-k gcc:12 bash -c \
#     'cd /osito-k/arch/x86/lib/dxvk && DXVK_ROOT=$$(pwd) OSITOK_ROOT=/osito-k make'
#
# Mirrors the layout of arch/x86/lib/opengl/Makefile but builds C++ via g++.

DXVK_ROOT := $(CURDIR)
OSITOK_ROOT ?= $(DXVK_ROOT)/../../../..
DXVK_SRC  := $(DXVK_ROOT)/dxvk/src
BUILD_DIR := $(OSITOK_ROOT)/arch/x86/build/lib/dxvk
UTIL_OUT  := $(BUILD_DIR)/libdxvk_util.a
SPIRV_OUT := $(BUILD_DIR)/libdxvk_spirv.a
DXBC_OUT  := $(BUILD_DIR)/libdxvk_dxbc.a

# C++ compiler.  Prefer the OsitoK cross-compiler if present; else fall
# back to the host g++ (which inside Docker is gcc:12's g++).
CROSS_CXX := $(OSITOK_ROOT)/../ok-ported/toolchain/cross-bin/x86_64-pc-ositok-musl-c++
ifeq ($(wildcard $(CROSS_CXX)),)
CXX ?= g++
else
CXX ?= $(CROSS_CXX)
endif
AR  ?= ar

GCC_INC     := $(shell $(CXX) -print-file-name=include)
GCC_CXX_INC := $(shell $(CXX) -print-file-name=include) # placeholder

INCLUDES := \
  -I$(DXVK_ROOT)/com_stub \
  -I$(DXVK_ROOT)/include \
  -I$(DXVK_SRC) \
  -I$(DXVK_SRC)/util \
  -I$(DXVK_SRC)/util/com \
  -I$(DXVK_SRC)/util/log \
  -I$(DXVK_SRC)/util/sha1 \
  -I$(DXVK_SRC)/util/sync \
  -I$(DXVK_SRC)/util/config \
  -I$(DXVK_SRC)/util/rc \
  -I$(DXVK_SRC)/spirv \
  -I$(DXVK_SRC)/vulkan \
  -I$(DXVK_SRC)/dxvk \
  -I$(DXVK_SRC)/dxbc \
  -I$(OSITOK_ROOT)/arch/x86/lib/vulkan/include

# C++17, freestanding, no exceptions, no RTTI.  Hosted libstdc++ is
# pulled via -isystem (gcc:12 ships it under /usr/include/c++/12/).
CXX_FLAGS := \
  -std=gnu++17 \
  -ffreestanding -fno-stack-protector -mno-red-zone \
  -fno-exceptions -fno-rtti \
  -fno-pic -fvisibility=hidden \
  -O2 \
  -Wall -Wextra \
  -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers \
  -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable \
  -Wno-non-virtual-dtor -Wno-deprecated-declarations \
  -DDXVK_NO_EXCEPTIONS=1 -DUSE_X86_64 -D__OSITO_K__ \
  -include $(DXVK_ROOT)/osito_compat/dxvk_compat.h \
  $(INCLUDES) \
  -isystem $(GCC_INC)

# ----------------------------------------------------------------------
# UTIL_SRCS — DXVK src/util/ subset that we currently compile.  Files
# left out of this list are deferred (mostly because they require
# Windows GDI / NTDDI / event handle APIs we have not stubbed yet).
# ----------------------------------------------------------------------
UTIL_SRCS := \
  dxvk/src/util/util_env.cpp \
  dxvk/src/util/util_flush.cpp \
  dxvk/src/util/util_luid.cpp \
  dxvk/src/util/util_matrix.cpp \
  dxvk/src/util/util_sleep.cpp \
  dxvk/src/util/util_string.cpp \
  dxvk/src/util/com/com_private_data.cpp \
  dxvk/src/util/config/config.cpp \
  dxvk/src/util/log/log.cpp \
  dxvk/src/util/log/log_debug.cpp \
  dxvk/src/util/sha1/sha1_util.cpp \
  dxvk/src/util/sync/sync_recursive.cpp \
  dxvk/src/util/thread.cpp

# C sources (sha1.c is the SHA-1 reference impl in plain C).
UTIL_C_SRCS := \
  dxvk/src/util/sha1/sha1.c

# Files deferred (need additional shims or have unconditional throw/try
# blocks incompatible with -fno-exceptions; tracked so we can lift them
# wave by wave):
#
#   dxvk/src/util/util_fps_limiter.cpp  -- catches std::invalid_argument
#                                          from std::stod when DXVK_FRAME_RATE
#                                          env var fails to parse.  Trivial
#                                          to patch (W5.1 hand-edit).
#   dxvk/src/util/util_gdi.cpp          -- needs HDC + GDI bitmap copy
#   dxvk/src/util/util_shared_res.cpp   -- needs winioctl IOCTL_*
#   dxvk/src/util/sha1/sha1.c           -- header-only path covered
#   dxvk/src/util/com/com_guid.cpp      -- includes d3d11_interfaces.h /
#                                          dxgi_interfaces.h / dxvk_hash.h
#                                          which arrive in W5.1+.  Deferred.

# ----------------------------------------------------------------------
# SPIRV_SRCS — DXVK src/spirv/.  All four upstream .cpp / header units
# build cleanly with the shim:
#
#   spirv_code_buffer.cpp  -- SPIR-V word stream emitter / encoder
#   spirv_compression.cpp  -- DXVK in-memory cache compression
#   spirv_module.cpp       -- the high-level builder DXBC + DXIL drive
#
# (spirv_instruction.h is header-only; the upstream meson.build does not
# list it as a source — same here.)
#
# Together they pack into libdxvk_spirv.a at ~140 KB / 3 objects, which
# exceeds the W5.0 acceptance bar (≥3 spirv objects).
# ----------------------------------------------------------------------
SPIRV_SRCS := \
  dxvk/src/spirv/spirv_code_buffer.cpp \
  dxvk/src/spirv/spirv_compression.cpp \
  dxvk/src/spirv/spirv_module.cpp

# ----------------------------------------------------------------------
# DXBC_SRCS — DXVK src/dxbc/. The D3D11 bytecode -> SPIR-V translator.
# 13 upstream .cpp files per meson.build. All self-contained shader
# translation logic (no OS interaction).
#
# dxbc_options.cpp is deferred: it #includes "../d3d11/d3d11_options.h"
# which would drag the entire D3D11 frontend into W5.1 scope. It is a
# ~20-line class that DXVK instantiates from the D3D11 layer; DXBC
# compilation itself doesn't call it.
# ----------------------------------------------------------------------
DXBC_SRCS := \
  dxvk/src/dxbc/dxbc_analysis.cpp \
  dxvk/src/dxbc/dxbc_chunk_isgn.cpp \
  dxvk/src/dxbc/dxbc_chunk_shex.cpp \
  dxvk/src/dxbc/dxbc_common.cpp \
  dxvk/src/dxbc/dxbc_compiler.cpp \
  dxvk/src/dxbc/dxbc_decoder.cpp \
  dxvk/src/dxbc/dxbc_defs.cpp \
  dxvk/src/dxbc/dxbc_header.cpp \
  dxvk/src/dxbc/dxbc_module.cpp \
  dxvk/src/dxbc/dxbc_names.cpp \
  dxvk/src/dxbc/dxbc_reader.cpp \
  dxvk/src/dxbc/dxbc_util.cpp

UTIL_OBJS    := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(UTIL_SRCS))
UTIL_C_OBJS  := $(patsubst %.c,$(BUILD_DIR)/%.o,$(UTIL_C_SRCS))
SPIRV_OBJS   := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SPIRV_SRCS))
DXBC_OBJS    := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(DXBC_SRCS))

.PHONY: all clean util spirv dxbc
all: util spirv dxbc

util:  $(UTIL_OUT)
spirv: $(SPIRV_OUT)
dxbc:  $(DXBC_OUT)

$(UTIL_OUT): $(UTIL_OBJS) $(UTIL_C_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(UTIL_OBJS) $(UTIL_C_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

$(SPIRV_OUT): $(SPIRV_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(SPIRV_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

$(DXBC_OUT): $(DXBC_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(DXBC_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

# Pattern rule: build/lib/dxvk/foo/bar.o : foo/bar.cpp
$(BUILD_DIR)/%.o: $(DXVK_ROOT)/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX   $*"
	@$(CXX) $(CXX_FLAGS) -c -o $@ $<

# Plain C path (sha1.c). Same warning posture, different std.
CC ?= gcc
$(BUILD_DIR)/%.o: $(DXVK_ROOT)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    $*"
	@$(CC) -std=gnu11 -ffreestanding -fno-stack-protector -mno-red-zone \
	       -fno-pic -O2 -Wall \
	       -D__OSITO_K__ \
	       $(INCLUDES) -isystem $(GCC_INC) \
	       -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)
	@echo "  CLEAN $(BUILD_DIR)"
