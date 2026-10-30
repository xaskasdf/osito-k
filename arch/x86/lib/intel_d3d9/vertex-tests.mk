include arch/x86/lib/intel_d3d9/compiler.mk
.PHONY: vertex-export vertex-sanitize
vertex-export: $(OUT)/vertex-export$(EXE)
.PHONY: vertex-api
vertex-api: $(OUT)/vertex-api-test$(EXE)
$(OUT)/vertex-api.o: arch/x86/test/intel_d3d9_vertex_api.c $(VERTEX_FIXTURES)/vertex-fixtures.h
	@$(CC) $(CFLAGS) -I$(VERTEX_FIXTURES) -c $< -o $@
$(OUT)/vertex-export.o: arch/x86/test/intel_d3d9_vertex_export.c
	@$(CC) $(CFLAGS) $(if $(filter host,$(PLATFORM)),-DVERTEX_DISASSEMBLE) -c $< -o $@
ifeq ($(PLATFORM),host)
.PHONY: vertex-fold
vertex-fold: $(OUT)/vertex-fold
$(OUT)/vertex-fold.o: arch/x86/test/intel_d3d9_vertex_fold.c
	@$(CC) $(CFLAGS) $(SANITIZE_FLAGS) -c $< -o $@
$(OUT)/vertex-fold: $(OUT)/vertex-fold.o $(OUT)/vertex-san.o $(OUT)/shader-san.o $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(CXX) $(SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^ -lm -pthread
$(OUT)/vertex-export: $(OUT)/vertex-export.o $(OUT)/api.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(CXX) -Wl,--gc-sections -o $@ $^ -lm -pthread
$(OUT)/vertex-api-test: $(OUT)/vertex-api.o $(OUT)/api.o $(OUT)/frontend.o $(OUT)/vertex.o $(OUT)/shader.o $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(CXX) -Wl,--gc-sections -o $@ $^ -lm -pthread
$(OUT)/vertex-export-san.o: arch/x86/test/intel_d3d9_vertex_export.c
	@$(CC) $(CFLAGS) $(SANITIZE_FLAGS) -DVERTEX_DISASSEMBLE -c $< -o $@
$(OUT)/api-san.o: arch/x86/lib/intel_d3d9/compiler_api.c arch/x86/lib/intel_d3d9/compiler_api.h
	@$(CC) $(CFLAGS) $(SANITIZE_FLAGS) -c $< -o $@
vertex-sanitize: $(OUT)/vertex-export-sanitized
$(OUT)/vertex-export-sanitized: $(OUT)/vertex-export-san.o $(OUT)/api-san.o $(OUT)/frontend-san.o $(OUT)/vertex-san.o $(OUT)/shader-san.o $(OUT)/libintel-native.a $(OUT)/libnir-native.a
	@$(CXX) $(SANITIZE_FLAGS) -Wl,--gc-sections -o $@ $^ -lm -pthread
else
$(OUT)/vertex-api-test.exe: $(OUT)/vertex-api.o $(OUT)/compiler-api-native.o
	@$(CXX) -O2 -flto -fuse-ld=lld -static -Wl,--gc-sections,--no-insert-timestamp -o $@ $^ -lm -lpsapi -lwinpthread -lsynchronization
$(OUT)/vertex-export.exe: $(OUT)/vertex-export.o $(OUT)/compiler-api-native.o
	@$(CXX) -O2 -flto -fuse-ld=lld -static -Wl,--gc-sections,--no-insert-timestamp -o $@ $^ -lm -lpsapi -lwinpthread -lsynchronization
endif
-include $(OUT)/vertex.d $(OUT)/vertex-san.d $(OUT)/vertex-export.d $(OUT)/vertex-export-san.d $(OUT)/api-san.d
