# arch/x86/lib/dxvk/Makefile -- W5.0/W5.1/W5.2 build infrastructure for DXVK on OsitoK.
#
# Produces six static archives:
#   $(BUILD_DIR)/libdxvk_util.a   -- DXVK src/util/  (hash maps, log, etc.)
#   $(BUILD_DIR)/libdxvk_spirv.a  -- DXVK src/spirv/ (SPIR-V module builder)
#   $(BUILD_DIR)/libdxvk_dxbc.a   -- DXVK src/dxbc/  (D3D11 bytecode -> SPIR-V)
#   $(BUILD_DIR)/libdxvk_core.a   -- DXVK src/dxvk/  (Vulkan abstraction layer) [W5.2]
#   $(BUILD_DIR)/libdxvk_dxgi.a   -- DXGI frontend
#   $(BUILD_DIR)/libdxvk_d3d11.a  -- D3D10/D3D11 frontend
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
DXVK_INC  := $(DXVK_ROOT)/dxvk/include
GENERATED_DIR := $(DXVK_ROOT)/include/generated
BUILD_DIR := $(OSITOK_ROOT)/arch/x86/build/lib/dxvk
UTIL_OUT  := $(BUILD_DIR)/libdxvk_util.a
SPIRV_OUT := $(BUILD_DIR)/libdxvk_spirv.a
DXBC_OUT  := $(BUILD_DIR)/libdxvk_dxbc.a
CORE_OUT  := $(BUILD_DIR)/libdxvk_core.a
D3D11_OUT := $(BUILD_DIR)/libdxvk_d3d11.a
DXGI_OUT  := $(BUILD_DIR)/libdxvk_dxgi.a

GLSLANG := glslangValidator
SHADER_SRCS := \
  $(wildcard $(DXVK_SRC)/dxvk/shaders/*.comp) \
  $(wildcard $(DXVK_SRC)/dxvk/shaders/*.frag) \
  $(wildcard $(DXVK_SRC)/dxvk/shaders/*.geom) \
  $(wildcard $(DXVK_SRC)/dxvk/shaders/*.vert) \
  $(wildcard $(DXVK_SRC)/dxvk/hud/shaders/*.frag) \
  $(wildcard $(DXVK_SRC)/dxvk/hud/shaders/*.vert) \
  $(wildcard $(DXVK_SRC)/d3d11/shaders/*.frag) \
  $(wildcard $(DXVK_SRC)/d3d11/shaders/*.vert)
SHADER_HEADERS := $(foreach src,$(SHADER_SRCS),\
  $(GENERATED_DIR)/$(basename $(notdir $(src))).h)

define make_shader_rule
$(GENERATED_DIR)/$(basename $(notdir $(1))).h: $(1)
	@mkdir -p $$(dir $$@)
	@echo "  GLSL  $$(notdir $$<)"
	@$$(GLSLANG) --quiet --target-env vulkan1.3 \
		--vn $$(basename $$(notdir $$<)) $$< -o $$@
endef

$(foreach src,$(SHADER_SRCS),$(eval $(call make_shader_rule,$(src))))

OSITOK_TOOLCHAIN_ROOT ?= /root/x-tools
OSITOK_TARGET ?= x86_64-linux-musl
OSITOK_BIN := $(OSITOK_TOOLCHAIN_ROOT)/bin

CXX := $(OSITOK_BIN)/$(OSITOK_TARGET)-g++
CC  := $(OSITOK_BIN)/$(OSITOK_TARGET)-gcc
AR  := $(OSITOK_BIN)/$(OSITOK_TARGET)-ar

INCLUDES := \
  -I$(DXVK_ROOT)/com_stub \
  -I$(DXVK_ROOT)/include \
  -I$(DXVK_ROOT)/include/generated \
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

# D3D11 / DXGI use a different windows.h shim path. The DXVK native
# windows shim (vendored under dxvk/include/native/windows/) supplies
# the GUID/HRESULT/COM macros the WIDL-generated MinGW headers need,
# while the MinGW d3d11/dxgi headers themselves live under
# dxvk/include/mingw/. The com_stub/ headers used by the rest of DXVK
# stay out of the search path so the two GUID definitions don't collide.
D3D11_INCLUDES := \
  -I$(DXVK_INC)/native/windows \
  -I$(DXVK_INC)/mingw \
  -I$(DXVK_ROOT)/include \
  -I$(DXVK_ROOT)/include/generated \
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
  -I$(OSITOK_ROOT)/arch/x86/lib/vulkan/include \
  -I$(DXVK_ROOT)/com_stub_d3d11

# C++17 with the hosted musl libstdc++ used by the GTA toolchain.
CXX_FLAGS := \
  -std=gnu++17 \
  -fno-stack-protector -mno-red-zone \
  -fno-exceptions -fno-rtti \
  -fno-pic -fno-pie -fvisibility=hidden \
  -O2 \
  -Wall -Wextra \
  -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers \
  -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable \
  -Wno-non-virtual-dtor -Wno-deprecated-declarations \
  -D_GNU_SOURCE -DDXVK_NO_EXCEPTIONS=1 -DUSE_X86_64 -D__OSITO_K__ \
  -include $(DXVK_ROOT)/osito_compat/dxvk_compat.h \
  $(INCLUDES)

# D3D11/DXGI flags: same toolchain, different INCLUDES (native windows
# shim instead of com_stub/), and skip the dxvk_compat.h force-include
# (the native windows shim provides the same Win32 typedefs and would
# otherwise clash on GUID/IUnknown).
D3D11_CXX_FLAGS := \
  -std=gnu++17 \
  -fno-stack-protector -mno-red-zone \
  -fno-exceptions \
  -fno-pic -fno-pie -fvisibility=hidden \
  -O2 \
  -Wall -Wextra \
  -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers \
  -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable \
  -Wno-non-virtual-dtor -Wno-deprecated-declarations -Wno-attributes \
  -Wno-class-memaccess -Wno-pragmas -Wno-narrowing \
  -D_GNU_SOURCE -DDXVK_NO_EXCEPTIONS=1 -DUSE_X86_64 -D__OSITO_K__ \
  -include $(DXVK_ROOT)/osito_compat/dxvk_d3d11_compat.h \
  $(D3D11_INCLUDES)

# ----------------------------------------------------------------------
# UTIL_SRCS — DXVK src/util/ subset that we currently compile.  Files
# left out of this list are deferred (mostly because they require
# Windows GDI / NTDDI / event handle APIs we have not stubbed yet).
# ----------------------------------------------------------------------
UTIL_SRCS := \
  dxvk/src/util/util_env.cpp \
  dxvk/src/util/util_error_ositok.cpp \
  dxvk/src/util/util_fps_limiter.cpp \
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
  dxvk/src/util/thread.cpp \
  osito_compat/dxvk_compositor_bridge.cpp \
  osito_compat/dxvk_logger_ositok.cpp

# com_guid.cpp pulls d3d11_interfaces.h + dxgi_interfaces.h transitively,
# which reach the native windows shim — same dependency surface as
# d3d11/dxgi proper. We compile it with D3D11_CXX_FLAGS for that reason
# and stash it in libdxvk_util.a alongside the other com bits via a
# dedicated rule below.
UTIL_D3D11_SRCS := \
  dxvk/src/util/com/com_guid.cpp \
  dxvk/src/util/util_gdi.cpp \
  dxvk/src/util/util_shared_res.cpp

# C sources (sha1.c is the SHA-1 reference impl in plain C).
UTIL_C_SRCS := \
  dxvk/src/util/sha1/sha1.c

# Files deferred (need additional shims or have unconditional throw/try
# blocks incompatible with -fno-exceptions; tracked so we can lift them
# wave by wave):
#
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

# dxbc_options.cpp pulls in the real d3d11_options.h (W5.3) which now
# transitively reaches dxgi1_6.h, so it needs the D3D11_CXX_FLAGS path.
DXBC_D3D11_SRCS := \
  dxvk/src/dxbc/dxbc_options.cpp

# ----------------------------------------------------------------------
# CORE_SRCS — DXVK src/dxvk/. The Vulkan abstraction layer.
# 48 upstream .cpp files. Excluded:
#   dxvk_openvr.cpp -- needs OpenVR headers (VR support, not used)
#   dxvk_openxr.cpp -- needs OpenXR headers (AR/VR, not used)
#   dxvk_platform_exts.cpp -- pulls openvr.h + openxr.h transitively
# ----------------------------------------------------------------------
CORE_SRCS := \
  dxvk/src/dxvk/dxvk_adapter.cpp \
  dxvk/src/dxvk/dxvk_barrier.cpp \
  dxvk/src/dxvk/dxvk_buffer.cpp \
  dxvk/src/dxvk/dxvk_cmdlist.cpp \
  dxvk/src/dxvk/dxvk_compute.cpp \
  dxvk/src/dxvk/dxvk_context.cpp \
  dxvk/src/dxvk/dxvk_cs.cpp \
  dxvk/src/dxvk/dxvk_data.cpp \
  dxvk/src/dxvk/dxvk_descriptor.cpp \
  dxvk/src/dxvk/dxvk_device.cpp \
  dxvk/src/dxvk/dxvk_device_filter.cpp \
  dxvk/src/dxvk/dxvk_extensions.cpp \
  dxvk/src/dxvk/dxvk_fence.cpp \
  dxvk/src/dxvk/dxvk_format.cpp \
  dxvk/src/dxvk/dxvk_framebuffer.cpp \
  dxvk/src/dxvk/dxvk_gpu_event.cpp \
  dxvk/src/dxvk/dxvk_gpu_query.cpp \
  dxvk/src/dxvk/dxvk_graphics.cpp \
  dxvk/src/dxvk/dxvk_image.cpp \
  dxvk/src/dxvk/dxvk_instance.cpp \
  dxvk/src/dxvk/dxvk_lifetime.cpp \
  dxvk/src/dxvk/dxvk_memory.cpp \
  dxvk/src/dxvk/dxvk_meta_blit.cpp \
  dxvk/src/dxvk/dxvk_meta_clear.cpp \
  dxvk/src/dxvk/dxvk_meta_copy.cpp \
  dxvk/src/dxvk/dxvk_meta_mipgen.cpp \
  dxvk/src/dxvk/dxvk_meta_pack.cpp \
  dxvk/src/dxvk/dxvk_meta_resolve.cpp \
  dxvk/src/dxvk/dxvk_options.cpp \
  dxvk/src/dxvk/dxvk_pipelayout.cpp \
  dxvk/src/dxvk/dxvk_pipemanager.cpp \
  dxvk/src/dxvk/dxvk_presenter.cpp \
  dxvk/src/dxvk/dxvk_queue.cpp \
  dxvk/src/dxvk/dxvk_resource.cpp \
  dxvk/src/dxvk/dxvk_sampler.cpp \
  dxvk/src/dxvk/dxvk_shader.cpp \
  dxvk/src/dxvk/dxvk_shader_key.cpp \
  dxvk/src/dxvk/dxvk_signal.cpp \
  dxvk/src/dxvk/dxvk_sparse.cpp \
  dxvk/src/dxvk/dxvk_staging.cpp \
  dxvk/src/dxvk/dxvk_state_cache.cpp \
  dxvk/src/dxvk/dxvk_stats.cpp \
  dxvk/src/dxvk/dxvk_swapchain_blitter.cpp \
  dxvk/src/dxvk/dxvk_unbound.cpp \
  dxvk/src/dxvk/dxvk_util.cpp \
  dxvk/src/dxvk/hud/dxvk_hud.cpp \
  dxvk/src/dxvk/hud/dxvk_hud_font.cpp \
  dxvk/src/dxvk/hud/dxvk_hud_item.cpp \
  dxvk/src/dxvk/hud/dxvk_hud_renderer.cpp \
  dxvk/src/dxvk/dxvk_platform_exts.cpp \
  dxvk/src/vulkan/vulkan_loader.cpp \
  dxvk/src/vulkan/vulkan_names.cpp \
  dxvk/src/wsi/wsi_platform_ositok.cpp \
  dxvk/src/wsi/wsi_edid_ositok.cpp

# ----------------------------------------------------------------------
# D3D11_SRCS / DXGI_SRCS — the COM-style D3D11 frontend (W5.3).
#
# 37 d3d11 .cpp + 10 dxgi .cpp upstream. We exclude:
#   d3d11_main.cpp     -- replaced by d3d11_main_ositok.cpp wrapper (T5)
#   d3d11_gdi.cpp      -- pulls Win32 GDI (HDC, BITMAPINFO, GetDC).
#   d3d11_on_12.cpp    -- D3D11on12 interop, drags in D3D12 surface.
#   d3d11_cuda.cpp     -- NVX CUDA interop, needs cudaGL.h.
#   d3d11_video.cpp    -- DXVA video decode interfaces, very Windows-y.
#   dxgi_main.cpp      -- replaced by dxgi_main_ositok.cpp wrapper (T5)
#   dxgi_monitor.cpp   -- needs ChangeDisplaySettingsExW etc.
# Files that build are everything else. We keep the subset that lets
# D3D11CreateDevice + DXGIFactory creation work.
# ----------------------------------------------------------------------
D3D11_SRCS := \
  dxvk/src/d3d11/d3d11_annotation.cpp \
  dxvk/src/d3d11/d3d11_blend.cpp \
  dxvk/src/d3d11/d3d11_buffer.cpp \
  dxvk/src/d3d11/d3d11_class_linkage.cpp \
  dxvk/src/d3d11/d3d11_cmdlist.cpp \
  dxvk/src/d3d11/d3d11_context.cpp \
  dxvk/src/d3d11/d3d11_context_def.cpp \
  dxvk/src/d3d11/d3d11_context_ext.cpp \
  dxvk/src/d3d11/d3d11_context_imm.cpp \
  dxvk/src/d3d11/d3d11_cuda.cpp \
  dxvk/src/d3d11/d3d11_depth_stencil.cpp \
  dxvk/src/d3d11/d3d11_device.cpp \
  dxvk/src/d3d11/d3d11_enums.cpp \
  dxvk/src/d3d11/d3d11_features.cpp \
  dxvk/src/d3d11/d3d11_fence.cpp \
  dxvk/src/d3d11/d3d11_gdi.cpp \
  dxvk/src/d3d11/d3d11_initializer.cpp \
  dxvk/src/d3d11/d3d11_input_layout.cpp \
  dxvk/src/d3d11/d3d11_interop.cpp \
  dxvk/src/d3d11/d3d11_on_12.cpp \
  dxvk/src/d3d11/d3d11_options.cpp \
  dxvk/src/d3d11/d3d11_query.cpp \
  dxvk/src/d3d11/d3d11_rasterizer.cpp \
  dxvk/src/d3d11/d3d11_resource.cpp \
  dxvk/src/d3d11/d3d11_sampler.cpp \
  dxvk/src/d3d11/d3d11_shader.cpp \
  dxvk/src/d3d11/d3d11_state.cpp \
  dxvk/src/d3d11/d3d11_state_object.cpp \
  dxvk/src/d3d11/d3d11_swapchain.cpp \
  dxvk/src/d3d11/d3d11_texture.cpp \
  dxvk/src/d3d11/d3d11_util.cpp \
  dxvk/src/d3d11/d3d11_video.cpp \
  dxvk/src/d3d11/d3d11_view_dsv.cpp \
  dxvk/src/d3d11/d3d11_view_rtv.cpp \
  dxvk/src/d3d11/d3d11_view_srv.cpp \
  dxvk/src/d3d11/d3d11_view_uav.cpp \
  dxvk/src/d3d11/d3d11_main_ositok.cpp

# D3D10 compat layer — the D3D11Device instantiates a D3D10Device wrapper, so
# these are pulled by the D3D11 frontend. Compiled with D3D11_CXX_FLAGS and
# stashed in libdxvk_d3d11.a.
D3D10_SRCS := \
  dxvk/src/d3d10/d3d10_blend.cpp \
  dxvk/src/d3d10/d3d10_buffer.cpp \
  dxvk/src/d3d10/d3d10_core.cpp \
  dxvk/src/d3d10/d3d10_depth_stencil.cpp \
  dxvk/src/d3d10/d3d10_device.cpp \
  dxvk/src/d3d10/d3d10_input_layout.cpp \
  dxvk/src/d3d10/d3d10_multithread.cpp \
  dxvk/src/d3d10/d3d10_query.cpp \
  dxvk/src/d3d10/d3d10_rasterizer.cpp \
  dxvk/src/d3d10/d3d10_sampler.cpp \
  dxvk/src/d3d10/d3d10_texture.cpp \
  dxvk/src/d3d10/d3d10_util.cpp \
  dxvk/src/d3d10/d3d10_view_dsv.cpp \
  dxvk/src/d3d10/d3d10_view_rtv.cpp \
  dxvk/src/d3d10/d3d10_view_srv.cpp

DXGI_SRCS := \
  dxvk/src/dxgi/dxgi_adapter.cpp \
  dxvk/src/dxgi/dxgi_enums.cpp \
  dxvk/src/dxgi/dxgi_factory.cpp \
  dxvk/src/dxgi/dxgi_format.cpp \
  dxvk/src/dxgi/dxgi_monitor.cpp \
  dxvk/src/dxgi/dxgi_options.cpp \
  dxvk/src/dxgi/dxgi_output.cpp \
  dxvk/src/dxgi/dxgi_surface.cpp \
  dxvk/src/dxgi/dxgi_swapchain.cpp \
  dxvk/src/dxgi/dxgi_main_ositok.cpp

UTIL_OBJS    := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(UTIL_SRCS))
UTIL_D3D11_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(UTIL_D3D11_SRCS))
UTIL_C_OBJS  := $(patsubst %.c,$(BUILD_DIR)/%.o,$(UTIL_C_SRCS))
SPIRV_OBJS   := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SPIRV_SRCS))
DXBC_OBJS    := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(DXBC_SRCS))
DXBC_D3D11_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(DXBC_D3D11_SRCS))
CORE_OBJS    := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CORE_SRCS))
D3D11_OBJS   := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(D3D11_SRCS))
D3D10_OBJS   := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(D3D10_SRCS))
DXGI_OBJS    := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(DXGI_SRCS))

$(CORE_OBJS) $(D3D11_OBJS): $(SHADER_HEADERS)

.PHONY: all clean shaders util spirv dxbc core d3d11 dxgi
all: util spirv dxbc core dxgi d3d11
shaders: $(SHADER_HEADERS)

util:  $(UTIL_OUT)
spirv: $(SPIRV_OUT)
dxbc:  $(DXBC_OUT)
core:  $(CORE_OUT)
dxgi:  $(DXGI_OUT)
d3d11: $(D3D11_OUT)

$(UTIL_OUT): $(UTIL_OBJS) $(UTIL_D3D11_OBJS) $(UTIL_C_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(UTIL_OBJS) $(UTIL_D3D11_OBJS) $(UTIL_C_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

$(SPIRV_OUT): $(SPIRV_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(SPIRV_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

$(DXBC_OUT): $(DXBC_OBJS) $(DXBC_D3D11_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(DXBC_OBJS) $(DXBC_D3D11_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

$(CORE_OUT): $(CORE_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(CORE_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

$(DXGI_OUT): $(DXGI_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(DXGI_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

$(D3D11_OUT): $(D3D11_OBJS) $(D3D10_OBJS) | $(BUILD_DIR)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(D3D11_OBJS) $(D3D10_OBJS)
	@echo "  -> $$($(AR) t $@ | wc -l) objects in $@"

# D3D10 compat layer pattern rule (same flags as d3d11).
$(BUILD_DIR)/dxvk/src/d3d10/%.o: $(DXVK_ROOT)/dxvk/src/d3d10/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX[d3d10]   $*"
	@$(CXX) $(D3D11_CXX_FLAGS) -c -o $@ $<

# D3D11/DXGI pattern rule: separate flags (D3D11_CXX_FLAGS).
# Use a stem match restricted to dxvk/src/d3d11/* and dxvk/src/dxgi/*
# so we do not accidentally compile other TUs with the wrong shim.
$(BUILD_DIR)/dxvk/src/d3d11/%.o: $(DXVK_ROOT)/dxvk/src/d3d11/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX[d3d11]   $*"
	@$(CXX) $(D3D11_CXX_FLAGS) -c -o $@ $<

$(BUILD_DIR)/dxvk/src/dxgi/%.o: $(DXVK_ROOT)/dxvk/src/dxgi/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX[dxgi]    $*"
	@$(CXX) $(D3D11_CXX_FLAGS) -c -o $@ $<

# WSI types cross the DXGI boundary and must use the same Win32 ABI typedefs.
$(BUILD_DIR)/dxvk/src/wsi/wsi_platform_ositok.o: $(DXVK_ROOT)/dxvk/src/wsi/wsi_platform_ositok.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX[dxgi]    wsi/wsi_platform_ositok"
	@$(CXX) $(D3D11_CXX_FLAGS) -c -o $@ $<

# com_guid.cpp ships with libdxvk_util.a but is compiled like d3d11/dxgi.
$(BUILD_DIR)/dxvk/src/util/com/com_guid.o: $(DXVK_ROOT)/dxvk/src/util/com/com_guid.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX[d3d11]   util/com/com_guid"
	@$(CXX) $(D3D11_CXX_FLAGS) -c -o $@ $<

$(BUILD_DIR)/dxvk/src/util/util_gdi.o: $(DXVK_ROOT)/dxvk/src/util/util_gdi.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX[d3d11]   util/util_gdi"
	@$(CXX) $(D3D11_CXX_FLAGS) -c -o $@ $<

$(BUILD_DIR)/dxvk/src/util/util_shared_res.o: $(DXVK_ROOT)/dxvk/src/util/util_shared_res.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX[d3d11]   util/util_shared_res"
	@$(CXX) $(D3D11_CXX_FLAGS) -c -o $@ $<

# dxbc_options.cpp likewise needs the d3d11 include path.
$(BUILD_DIR)/dxvk/src/dxbc/dxbc_options.o: $(DXVK_ROOT)/dxvk/src/dxbc/dxbc_options.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX[d3d11]   dxbc/dxbc_options"
	@$(CXX) $(D3D11_CXX_FLAGS) -c -o $@ $<

# util_string.cpp / com_private_data.cpp expose WCHAR- and GUID-typed symbols
# (str::fromws, ComPrivateData::set/getData) consumed by the d3d11/dxgi
# frontend. They MUST see the same WCHAR (wchar_t) and GUID (struct GUID)
# definitions as that frontend, so compile them with D3D11_CXX_FLAGS — building
# them with the plain CXX_FLAGS yielded `_GUID`/`unsigned short` mangling that
# did not match the d3d11 callers (4 undefined references at final link).
$(BUILD_DIR)/dxvk/src/util/util_string.o: $(DXVK_ROOT)/dxvk/src/util/util_string.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX[d3d11]   util/util_string"
	@$(CXX) $(D3D11_CXX_FLAGS) -c -o $@ $<
$(BUILD_DIR)/dxvk/src/util/com/com_private_data.o: $(DXVK_ROOT)/dxvk/src/util/com/com_private_data.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX[d3d11]   util/com/com_private_data"
	@$(CXX) $(D3D11_CXX_FLAGS) -c -o $@ $<

# Pattern rule: build/lib/dxvk/foo/bar.o : foo/bar.cpp
$(BUILD_DIR)/%.o: $(DXVK_ROOT)/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CXX   $*"
	@$(CXX) $(CXX_FLAGS) -c -o $@ $<

# Plain C path (sha1.c). Same warning posture, different std.
$(BUILD_DIR)/%.o: $(DXVK_ROOT)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC    $*"
	@$(CC) -std=gnu11 -ffreestanding -fno-stack-protector -mno-red-zone \
	       -fPIC -O2 -Wall \
	       -D__OSITO_K__ \
	       $(INCLUDES) \
	       -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)
	@echo "  CLEAN $(BUILD_DIR)"
