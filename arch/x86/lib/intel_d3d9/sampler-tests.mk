# Instrument the Osito compiler boundary and frontend. Upstream compiler
# archives keep their normal build; this is not an all-Mesa sanitizer claim.
include arch/x86/lib/intel_d3d9/compiler.mk
.PHONY: sampler-sanitize
sampler-sanitize: $(OUT)/sampler-api-sanitized
$(OUT)/sampler-api-san.o: arch/x86/test/intel_d3d9_sampler_api.c $(SAMPLER_FIXTURES)/sampler-fixtures.h
	@$(CC) $(CFLAGS) $(SANITIZE_FLAGS) -I$(SAMPLER_FIXTURES) -c $< -o $@
$(OUT)/api-san.o: arch/x86/lib/intel_d3d9/compiler_api.c arch/x86/lib/intel_d3d9/compiler_api.h
	@$(CC) $(CFLAGS) $(SANITIZE_FLAGS) -c $< -o $@
$(OUT)/sampler-api-sanitized: $(OUT)/sampler-api-san.o $(OUT)/api-san.o $(OUT)/frontend-san.o $(OUT)/vertex-san.o $(OUT)/shader-san.o $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(CXX) $(SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $(OUT)/sampler-api-san.o $(OUT)/api-san.o $(OUT)/frontend-san.o $(OUT)/vertex-san.o $(OUT)/shader-san.o \
	 -Wl,--start-group $(OUT)/libintel-native.a $(OUT)/libnir-native.a -Wl,--end-group -lm -pthread
-include $(OUT)/sampler-api-san.d $(OUT)/api-san.d
