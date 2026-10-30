MESA := arch/x86/lib/opengl/mesa
INTEL := arch/x86/lib/intel_d3d9/mesa-intel/src/intel
PLATFORM ?= host
ifneq ($(filter $(PLATFORM),win32 win64),)
ifeq ($(PLATFORM),win32)
OUT ?= arch/x86/build/native-intel-compiler-pe32
MINGW_ROOT ?= /usr/i686-w64-mingw32
MINGW_TARGET := i686-w64-windows-gnu
MINGW_HAVE_STRNDUP ?= 0
else
OUT ?= arch/x86/build/native-intel-compiler-pe64
MINGW_ROOT ?= C:/msys64/mingw64
MINGW_TARGET := x86_64-w64-windows-gnu
MINGW_HAVE_STRNDUP ?= 1
endif
# Optional extracted cross-toolchain, without a system-wide installation.
MINGW_TOOLCHAIN_FLAGS ?=
MINGW_CXX_FLAGS ?=
CC := clang --target=$(MINGW_TARGET) --sysroot=$(MINGW_ROOT) $(MINGW_TOOLCHAIN_FLAGS)
CXX := clang++ --target=$(MINGW_TARGET) --sysroot=$(MINGW_ROOT) $(MINGW_TOOLCHAIN_FLAGS) $(MINGW_CXX_FLAGS)
AR := llvm-ar
LLVM_LINK ?= llvm-link
OPT ?= opt
EXE := .exe
PLATFORM_FLAGS := -flto -mno-ms-bitfields -D_WIN32_WINNT=0x0602 \
 -DINTEL_D3D9_HAVE_STRNDUP=$(MINGW_HAVE_STRNDUP) -include arch/x86/lib/intel_d3d9/win32_port.h \
 -Iarch/x86/lib/intel_d3d9/win32_include
else ifeq ($(PLATFORM),host)
OUT ?= arch/x86/build/native-intel-compiler-host
PLATFORM_FLAGS := -DHAVE_PTHREAD -DHAVE_ENDIAN_H -D_GNU_SOURCE -DHAVE_SECURE_GETENV \
 -DHAVE_POSIX_MEMALIGN -DHAVE_STRTOD_L -DHAVE_STRTOLL_L
else
$(error Unsupported PLATFORM=$(PLATFORM); choose host, win32 or win64)
endif
PYTHON ?= python3
.DEFAULT_GOAL := probe
CFLAGS := -MMD -MP -std=gnu11 -O2 -ffunction-sections -fdata-sections -fno-strict-aliasing \
 $(PLATFORM_FLAGS) -DHAVE_STRUCT_TIMESPEC \
 -DHAVE_FUNC_ATTRIBUTE_PACKED -DHAVE_FUNC_ATTRIBUTE_PURE -DHAVE_FUNC_ATTRIBUTE_CONST \
 -DHAVE_FUNC_ATTRIBUTE_MALLOC -DHAVE_FUNC_ATTRIBUTE_FORMAT -DHAVE_FUNC_ATTRIBUTE_NORETURN \
 -DHAVE_FUNC_ATTRIBUTE_UNUSED -DHAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT \
 -DHAVE_TIMESPEC_GET -DHAVE_STRTOF \
 -I$(INTEL)/../../include -I$(MESA)/include -I$(MESA)/src -I$(MESA)/src/compiler -I$(MESA)/src/compiler/nir \
 -I$(INTEL) -I$(INTEL)/.. -I$(INTEL)/../../include -I$(INTEL)/compiler \
 -I$(OUT)/generated -I$(OUT)/generated/intel -I$(OUT)/generated/intel/dev \
 -Iarch/x86/lib/vulkan/include -I$(MESA)/src/util
CXXFLAGS := $(filter-out -std=gnu11,$(CFLAGS)) -std=gnu++17 -fno-exceptions -fno-rtti
NIR := $(wildcard $(MESA)/src/compiler/nir/*.c)
UTIL_NAMES := ralloc hash_table set bitscan blob half_float softfloat double \
 fast_idiv_by_const u_math u_debug u_printf u_worklist u_vector u_dynarray \
 register_allocate dag rb_tree mesa-sha1 memstream os_misc os_time log \
 simple_mtx u_call_once u_cpu_detect sparse_array strtod mesa-blake3 u_process sha1/sha1 format/u_format_table
UTIL := $(addprefix $(MESA)/src/util/,$(addsuffix .c,$(UTIL_NAMES)))
TOP := $(MESA)/src/compiler/glsl_types.c $(MESA)/src/compiler/builtin_types.c $(MESA)/src/compiler/shader_enums.c
ifneq ($(filter $(PLATFORM),win32 win64),)
UTIL += $(MESA)/src/util/u_thread.c $(MESA)/src/util/futex.c
TOP += $(MESA)/src/c11/impl/threads_win32.c $(MESA)/src/c11/impl/time.c
PLATFORM_OBJ := $(OUT)/port.o $(OUT)/mesa/util/u_qsort.o
endif
COMMON_OBJ := $(patsubst $(MESA)/src/%.c,$(OUT)/mesa/%.o,$(NIR) $(UTIL) $(TOP))
.PHONY: common
common: $(OUT)/libnir-native.a
$(OUT)/libnir-native.a: $(COMMON_OBJ)
	@rm -f "$@"
	@$(AR) rcs $@ $^
$(OUT)/mesa/%.o: $(MESA)/src/%.c
	@mkdir -p $(dir $@)
	@echo "  CC $<"
	@$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/mesa/%.o: $(MESA)/src/%.cpp
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/intel-sources.mk: arch/x86/scripts/generate-native-intel.py $(INTEL)/compiler/meson.build $(INTEL)/../../source.json
	@$(PYTHON) arch/x86/scripts/generate-native-intel.py $(OUT)

include $(OUT)/intel-sources.mk
INTEL_OBJ += $(OUT)/intel/dev/i915/intel_device_info.o
.PHONY: intel
intel: $(OUT)/libintel-native.a
$(OUT)/libintel-native.a: $(INTEL_OBJ)
	@rm -f "$@"
	@$(AR) rcs $@ $^
$(OUT)/intel/%.o: $(INTEL)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC $<"
	@$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/intel/%.o: $(INTEL)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@
$(OUT)/generated/%.o: $(OUT)/generated/%.c
	@$(CC) $(CFLAGS) -c $< -o $@

.PHONY: probe
probe: $(OUT)/native-compiler$(EXE)
$(OUT)/frontend.o: arch/x86/lib/intel_d3d9/nir_frontend.c arch/x86/lib/intel_d3d9/nir_frontend.h
	@$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/vertex.o: arch/x86/lib/intel_d3d9/nir_vertex.c arch/x86/lib/intel_d3d9/nir_frontend.h
	@$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/shader.o: arch/x86/lib/intel_d3d9/shader.c arch/x86/lib/intel_d3d9/shader.h
	@$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/probe.o: arch/x86/test/intel_d3d9_compiler_host.c
	@$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/port.o: arch/x86/lib/intel_d3d9/win32_port.c
	@$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/api.o: arch/x86/lib/intel_d3d9/compiler_api.c arch/x86/lib/intel_d3d9/compiler_api.h
	@$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/api-probe.o: arch/x86/test/intel_d3d9_api_host.c $(OUT)/generated/compiler-api-fixtures.h
	@$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/export.o: arch/x86/test/intel_d3d9_export_host.c arch/x86/lib/intel_d3d9/compiler_api.h
	@$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/generated/compiler-api-fixtures.h: arch/x86/scripts/generate-compiler-api-tests.py
	@$(PYTHON) $< $(OUT)/generated
.PHONY: api-test umd export
api-test: $(OUT)/compiler-api-test$(EXE)
export: $(OUT)/compiler-export$(EXE)
ifneq ($(filter $(PLATFORM),win32 win64),)
# COFF checks undefined symbols before dead-code elimination. Merge the compiler
# IR, internalize its implementation, and remove unreachable OS/render helpers
# before the PE link. There is deliberately no ioctl implementation to link.
$(OUT)/compiler-linked.bc: $(OUT)/probe.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o $(PLATFORM_OBJ) $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(LLVM_LINK) $^ -o $@
$(OUT)/compiler-pruned.bc: $(OUT)/compiler-linked.bc
	@$(OPT) -passes=internalize,globaldce -internalize-public-api-list=main $< -o $@
$(OUT)/native-compiler.exe: $(OUT)/compiler-pruned.bc
	@$(CXX) -O2 -flto -fuse-ld=lld -static -Wl,--gc-sections -o $@ $< -lm -lpsapi -lwinpthread -lsynchronization
$(OUT)/api-linked.bc: $(OUT)/api.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o $(PLATFORM_OBJ) $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(LLVM_LINK) $^ -o $@
$(OUT)/api-pruned.bc: $(OUT)/api-linked.bc
	@$(OPT) -passes=internalize,globaldce -internalize-public-api-list=intel_d3d9_compiler_create,intel_d3d9_compiler_destroy,intel_d3d9_compile_pixel,intel_d3d9_compile_vertex,intel_d3d9_program_free $< -o $@
$(OUT)/compiler-api-native.o: $(OUT)/api-pruned.bc
	@$(CC) -O2 -c $< -o $@
$(OUT)/compiler-api-test.exe: $(OUT)/api-probe.o $(OUT)/compiler-api-native.o
	@$(CXX) -O2 -flto -fuse-ld=lld -static -Wl,--gc-sections,--no-insert-timestamp -o $@ $^ -lm -lpsapi -lwinpthread -lsynchronization
$(OUT)/compiler-export.exe: $(OUT)/export.o $(OUT)/compiler-api-native.o
	@$(CXX) -O2 -flto -fuse-ld=lld -static -Wl,--gc-sections,--no-insert-timestamp -o $@ $^ -lm -lpsapi -lwinpthread -lsynchronization
MSVC_TARGET := $(if $(filter win32,$(PLATFORM)),i686,x86_64)-pc-windows-msvc
$(OUT)/umd.obj: arch/x86/lib/intel_d3d9/umd.c arch/x86/lib/intel_d3d9/compiler_api.h arch/x86/drivers/intel_gpu_umd_abi.h arch/x86/drivers/intel_gen9_texture.h
	@test -n "$(SDK_INCLUDE)" -a -n "$(MSVC_INCLUDE)" -a -n "$(WDK_INCLUDE)"
	@clang --target=$(MSVC_TARGET) -O2 -ffreestanding -fno-builtin -fno-stack-protector -D_DLL -D_MT \
	 -Wall -Wextra -Werror -Wno-ignored-attributes -Wno-pragma-pack -Wno-nonportable-include-path \
	 -isystem "$(MSVC_INCLUDE)" -isystem "$(SDK_INCLUDE)/um" -isystem "$(SDK_INCLUDE)/shared" \
	 -isystem "$(SDK_INCLUDE)/ucrt" -isystem "$(WDK_INCLUDE)" -c $< -o $@
umd: $(OUT)/osito-intel9.dll
$(OUT)/umd-texture.obj: arch/x86/drivers/intel_gen9_texture.c arch/x86/drivers/intel_gen9_texture.h
	@$(CC) -O2 -ffreestanding -fno-builtin -fno-stack-protector \
	 -Wall -Wextra -Werror -c $< -o $@
$(OUT)/umd.def:
	@printf 'EXPORTS\nOpenAdapter=$(if $(filter win32,$(PLATFORM)),OpenAdapter@4,OpenAdapter)\n' > $@
$(OUT)/osito-intel9.dll: $(OUT)/umd.obj $(OUT)/umd-texture.obj $(OUT)/compiler-api-native.o $(OUT)/umd.def
	@$(CXX) -shared -fuse-ld=lld -static -Wl,--gc-sections,--kill-at,--no-insert-timestamp -o $@ $^ -lm -lpsapi -lwinpthread -lsynchronization
else
$(OUT)/native-compiler: $(OUT)/probe.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(CXX) -Wl,--gc-sections -o $@ $(OUT)/probe.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o \
	 -Wl,--start-group $(OUT)/libintel-native.a $(OUT)/libnir-native.a -Wl,--end-group -lm -pthread
$(OUT)/compiler-api-test: $(OUT)/api-probe.o $(OUT)/api.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(CXX) -Wl,--gc-sections -o $@ $(OUT)/api-probe.o $(OUT)/api.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o \
	 -Wl,--start-group $(OUT)/libintel-native.a $(OUT)/libnir-native.a -Wl,--end-group -lm -pthread
$(OUT)/compiler-export: $(OUT)/export.o $(OUT)/api.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(CXX) -Wl,--gc-sections -o $@ $(OUT)/export.o $(OUT)/api.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o \
	 -Wl,--start-group $(OUT)/libintel-native.a $(OUT)/libnir-native.a -Wl,--end-group -lm -pthread
endif

# Instrument our decoder/frontend/probe. Upstream archives retain their normal
# build; this checks our boundary and allocation lifetime, not all Mesa internals.
SANITIZE_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
.PHONY: sanitize
ifeq ($(PLATFORM),host)
sanitize: $(OUT)/native-compiler-sanitized
else
sanitize:
	@echo 'Sanitizer qualification currently requires PLATFORM=host'; exit 1
endif
$(OUT)/frontend-san.o: arch/x86/lib/intel_d3d9/nir_frontend.c arch/x86/lib/intel_d3d9/compiler.mk
	@$(CC) $(CFLAGS) $(SANITIZE_FLAGS) -c $< -o $@
$(OUT)/vertex-san.o: arch/x86/lib/intel_d3d9/nir_vertex.c arch/x86/lib/intel_d3d9/compiler.mk
	@$(CC) $(CFLAGS) $(SANITIZE_FLAGS) -c $< -o $@
$(OUT)/shader-san.o: arch/x86/lib/intel_d3d9/shader.c arch/x86/lib/intel_d3d9/compiler.mk
	@$(CC) $(CFLAGS) $(SANITIZE_FLAGS) -c $< -o $@
$(OUT)/probe-san.o: arch/x86/test/intel_d3d9_compiler_host.c arch/x86/lib/intel_d3d9/compiler.mk
	@$(CC) $(CFLAGS) $(SANITIZE_FLAGS) -c $< -o $@
$(OUT)/native-compiler-sanitized: $(OUT)/probe-san.o $(OUT)/frontend-san.o $(OUT)/vertex-san.o $(OUT)/shader-san.o $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(CXX) $(SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $(OUT)/probe-san.o $(OUT)/frontend-san.o $(OUT)/vertex-san.o $(OUT)/shader-san.o \
	 -Wl,--start-group $(OUT)/libintel-native.a $(OUT)/libnir-native.a -Wl,--end-group -lm -pthread

$(COMMON_OBJ) $(INTEL_OBJ) $(PLATFORM_OBJ) $(OUT)/probe.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o $(OUT)/api.o $(OUT)/api-probe.o: arch/x86/lib/intel_d3d9/compiler.mk
-include $(COMMON_OBJ:.o=.d) $(INTEL_OBJ:.o=.d) $(OUT)/probe.d $(OUT)/frontend.d $(OUT)/shader.d
-include $(PLATFORM_OBJ:.o=.d)
-include $(OUT)/api.d $(OUT)/api-probe.d
-include $(OUT)/vertex.d $(OUT)/vertex-san.d
-include $(OUT)/export.d
-include $(OUT)/probe-san.d $(OUT)/frontend-san.d $(OUT)/shader-san.d

.PHONY: sampler-export
sampler-export: $(OUT)/sampler-export$(EXE)
$(OUT)/sampler-export.o: arch/x86/test/intel_d3d9_sampler_export.c
	@$(CC) $(CFLAGS) $(if $(filter host,$(PLATFORM)),-DSAMPLER_DISASSEMBLE) -c $< -o $@
ifeq ($(PLATFORM),host)
$(OUT)/sampler-export: $(OUT)/sampler-export.o $(OUT)/api.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(CXX) -Wl,--gc-sections -o $@ $(OUT)/sampler-export.o $(OUT)/api.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o \
	 -Wl,--start-group $(OUT)/libintel-native.a $(OUT)/libnir-native.a -Wl,--end-group -lm -pthread
else
$(OUT)/sampler-export.exe: $(OUT)/sampler-export.o $(OUT)/compiler-api-native.o
	@$(CXX) -O2 -flto -fuse-ld=lld -static -Wl,--gc-sections,--no-insert-timestamp -o $@ $^ -lm -lpsapi -lwinpthread -lsynchronization
endif
-include $(OUT)/sampler-export.d

.PHONY: sampler-test
sampler-test: $(OUT)/sampler-api-test$(EXE)
$(OUT)/sampler-api.o: arch/x86/test/intel_d3d9_sampler_api.c $(SAMPLER_FIXTURES)/sampler-fixtures.h
	@$(CC) $(CFLAGS) -I$(SAMPLER_FIXTURES) -c $< -o $@
ifeq ($(PLATFORM),host)
$(OUT)/sampler-api-test: $(OUT)/sampler-api.o $(OUT)/api.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(CXX) -Wl,--gc-sections -o $@ $(OUT)/sampler-api.o $(OUT)/api.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o \
	 -Wl,--start-group $(OUT)/libintel-native.a $(OUT)/libnir-native.a -Wl,--end-group -lm -pthread
else
$(OUT)/sampler-api-test.exe: $(OUT)/sampler-api.o $(OUT)/compiler-api-native.o
	@$(CXX) -O2 -flto -fuse-ld=lld -static -Wl,--gc-sections,--no-insert-timestamp -o $@ $^ -lm -lpsapi -lwinpthread -lsynchronization
endif
-include $(OUT)/sampler-api.d
