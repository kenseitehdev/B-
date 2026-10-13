.DEFAULT_GOAL := all
LLVM_PREFIX ?= /opt/homebrew/opt/llvm/bin
UNAME_S := $(shell uname -s)
ifeq ($(origin CC),default)
CC := clang
ifneq ($(wildcard $(LLVM_PREFIX)/clang),)
CC := $(LLVM_PREFIX)/clang
endif
endif
PYTHON ?= python3
BUILD_MODE ?= debug
ifeq ($(UNAME_S),Darwin)
DEBUG_SANITIZERS ?= address,undefined
else
DEBUG_SANITIZERS ?= address,leak,undefined
endif
MODE_LINK_FLAGS :=
ifeq ($(BUILD_MODE),debug)
OPT := -O1
SANITIZERS := $(DEBUG_SANITIZERS)
MODE_FLAGS := -g3 -fno-omit-frame-pointer -fno-sanitize-recover=undefined
else ifeq ($(BUILD_MODE),tsan)
OPT := -O1
SANITIZERS := thread
MODE_FLAGS := -g3 -fno-omit-frame-pointer
else ifeq ($(BUILD_MODE),msan)
ifeq ($(UNAME_S),Linux)
else
$(error BUILD_MODE=msan is supported only on Linux with Clang and compiler-rt)
endif
CC_IS_CLANG := $(shell "$(CC)" --version 2>/dev/null | head -1 | grep -ic clang)
ifeq ($(CC_IS_CLANG),0)
$(error BUILD_MODE=msan requires a Clang compiler; use CC=clang)
endif
OPT := -O1
SANITIZERS := memory
MODE_FLAGS := -g3 -fno-omit-frame-pointer -fno-optimize-sibling-calls -fPIE
MODE_LINK_FLAGS := -pie
else ifeq ($(BUILD_MODE),release)
OPT := -O2
SANITIZERS :=
MODE_FLAGS := -DNDEBUG
else
$(error BUILD_MODE must be debug, tsan, msan, or release)
endif
ifneq ($(SANITIZERS),)
MODE_FLAGS += -fsanitize=$(SANITIZERS)
endif
# UCRT64's runtime lacks Clang's nonrecovering UBSAN handler exports.
# Trap instrumentation checks all enabled undefined operations without them.
ifeq ($(OS),Windows_NT)
ifneq ($(BUILD_MODE),release)
MODE_FLAGS += -gcodeview
endif
ifneq ($(findstring undefined,$(SANITIZERS)),)
MODE_FLAGS += -fsanitize-trap=undefined
endif
endif
CPPFLAGS += -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?=
OBJDIR := build/$(BUILD_MODE)/obj
ifeq ($(OS),Windows_NT)
EXE := .exe
PROJECT_DIR := $(shell cygpath -m "$(CURDIR)")
else
EXE :=
PROJECT_DIR := $(CURDIR)
endif
BIN := bin/$(BUILD_MODE)/bpp$(EXE)
SRCS := src/main.c src/compiler.c src/lexer.c src/parser.c src/ir.c src/ir_effects.c src/isel.c src/emit_arm64.c src/regalloc.c src/registers.c src/target.c src/range.c src/optimize.c src/emit_x64.c src/raw_native.c src/locations.c src/value_facts.c src/strict_runtime.c
OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d) $(OBJDIR)/src/runtime.d $(OBJDIR)/src/value.d $(OBJDIR)/src/codegen_runtime.d
.PHONY: all debug tsan msan release check check-native check-arm64 check-msabi check-targets check-static-abi check-xf-static-lib check-runtime-boundary check-strict-native-runtime check-differential check-stress check-asan-hook check-native-sanitizer check-native-checks check-sanitizer-availability check-verification check-fuzz check-reproducible check-msan check-raw-core check-debug-metadata benchmark benchmark-smoke test targets clean
RUNTIME_OBJS := $(OBJDIR)/src/runtime.o $(OBJDIR)/src/value.o $(OBJDIR)/src/codegen_runtime.o $(OBJDIR)/src/bpp_abi.o $(OBJDIR)/src/truth.o
DEPS += $(RUNTIME_OBJS:.o=.d)
NATIVE_SANITIZER_OBJ := $(OBJDIR)/src/native_sanitizer.o
DEPS += $(NATIVE_SANITIZER_OBJ:.o=.d)
ifeq ($(BUILD_MODE),msan)
# Assembly cannot populate Clang's parameter/return TLS shadow. The portable
# byte-definedness map checks native values; retain MSAN for C memory accesses.
$(NATIVE_SANITIZER_OBJ) $(RUNTIME_OBJS): CFLAGS += -fno-sanitize-memory-param-retval
endif
TEST_SANITIZER_ENV := BPP_TEST_SANITIZED=$(if $(strip $(SANITIZERS)),1,0)
DEBUGGER_TEST_ENV :=
ifneq ($(BUILD_MODE),debug)
DEBUGGER_TEST_ENV := BPP_SKIP_DEBUGGER_TESTS=1
endif
all: $(BIN)
debug:
	$(MAKE) BUILD_MODE=debug all
tsan:
	$(MAKE) BUILD_MODE=tsan all
msan:
	$(MAKE) BUILD_MODE=msan all
release:
	$(MAKE) BUILD_MODE=release all
$(OBJDIR)/src/main.o: CPPFLAGS += '-DBPP_PROJECT_DIR="$(PROJECT_DIR)"' '-DBPP_HOST_CC="$(CC)"' '-DBPP_SANITIZERS="$(SANITIZERS)"' '-DBPP_OPTIMIZATION="$(OPT)"' '-DBPP_RUNTIME_OBJECT="$(PROJECT_DIR)/$(OBJDIR)/src/runtime.o"' '-DBPP_VALUE_OBJECT="$(PROJECT_DIR)/$(OBJDIR)/src/value.o"' '-DBPP_NATIVE_RUNTIME_OBJECT="$(PROJECT_DIR)/$(OBJDIR)/src/codegen_runtime.o"' '-DBPP_NATIVE_SANITIZER_OBJECT="$(PROJECT_DIR)/$(NATIVE_SANITIZER_OBJ)"' '-DBPP_TRUTH_OBJECT="$(PROJECT_DIR)/$(OBJDIR)/src/truth.o"' '-DBPP_STATIC_ABI_OBJECT="$(PROJECT_DIR)/$(OBJDIR)/src/bpp_abi.o"'
$(OBJDIR)/%.o: %.c Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) -MMD -MP -c "$<" -o "$@"
$(BIN): $(OBJS) $(RUNTIME_OBJS) $(NATIVE_SANITIZER_OBJ)
	mkdir -p "$(@D)"
	$(CC) $(LDFLAGS) $(MODE_FLAGS) $(MODE_LINK_FLAGS) -o "$@" $(OBJS) $(LDLIBS)
check-native: $(BIN)
	$(TEST_SANITIZER_ENV) BPP_TEST_BACKEND=native sh tests/run_tests.sh ./$(BIN)
check-arm64: $(BIN)
	BPP_TEST_BACKEND=arm64 sh tests/run_tests.sh ./$(BIN)
check-msabi: $(BIN)
	BPP_MSABI_CC="$(CC)" sh tests/check_msabi.sh ./$(BIN)
check-static-abi: $(BIN) $(RUNTIME_OBJS)
	BPP_ABI_CC="$(CC)" BPP_ABI_FLAGS="$(MODE_FLAGS) $(MODE_LINK_FLAGS)" AR="$(AR)" sh tests/check_static_abi.sh ./$(BIN) $(OBJDIR)/src/runtime.o $(OBJDIR)/src/value.o $(OBJDIR)/src/codegen_runtime.o $(OBJDIR)/src/bpp_abi.o $(OBJDIR)/src/truth.o
check-xf-static-lib: $(BIN)
	$(MAKE) -C Libraries/xf BPP_ROOT="$(CURDIR)" test
check-runtime-boundary: $(BIN) tests/check_native_runtime_boundary.sh tests/native_runtime_boundary.bpp
	sh tests/check_native_runtime_boundary.sh ./$(BIN)
check-native-checks: $(BIN) $(OBJDIR)/test_raw_sanitizer$(EXE)
	BPP_TEST_SANITIZED=1 BPP_NATIVE_CHECKS=1 sh tests/check_native_sanitizer.sh ./$(BIN) ./$(OBJDIR)/test_raw_sanitizer$(EXE)
check-sanitizer-availability:
	sh tests/check_sanitizer_availability.sh "$(CC)"
check-native-sanitizer: $(BIN) $(OBJDIR)/test_raw_sanitizer$(EXE)
	$(TEST_SANITIZER_ENV) sh tests/check_native_sanitizer.sh ./$(BIN) ./$(OBJDIR)/test_raw_sanitizer$(EXE)
$(OBJDIR)/test_raw_sanitizer$(EXE): tests/test_raw_sanitizer.c $(NATIVE_SANITIZER_OBJ) include/native_sanitizer.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) -UNDEBUG tests/test_raw_sanitizer.c $(NATIVE_SANITIZER_OBJ) -o "$@"
check-raw-core: $(BIN) tests/check_raw_core.sh tests/raw_core.bpp
	sh tests/check_raw_core.sh ./$(BIN)
check-debug-metadata: $(BIN) tests/check_debug_metadata.sh
	$(DEBUGGER_TEST_ENV) sh tests/check_debug_metadata.sh ./$(BIN)
check-strict-native-runtime: $(BIN) tests/check_strict_native_runtime.sh tests/strict_native_runtime.bpp
	BPP_STRICT_CC="$(CC)" sh tests/check_strict_native_runtime.sh ./$(BIN)
check-targets: $(BIN)
	sh tests/check_targets.sh ./$(BIN)
targets: $(BIN)
	./$(BIN) --list-targets
check-differential: $(BIN) $(OBJDIR)/generate_case$(EXE)
	$(PYTHON) tests/check_verification.py differential ./$(BIN) --generator ./$(OBJDIR)/generate_case$(EXE)
check-stress: $(BIN) $(OBJDIR)/generate_case$(EXE)
	sh tests/check_stress.sh ./$(BIN) ./$(OBJDIR)/generate_case$(EXE)
benchmark: $(BIN) $(OBJDIR)/benchmark_clock$(EXE)
	@sh tests/benchmark.sh ./$(BIN) ./$(OBJDIR)/benchmark_clock$(EXE)
benchmark-smoke: $(BIN) $(OBJDIR)/benchmark_clock$(EXE)
	@BPP_BENCHMARK_SAMPLES=1 BPP_BENCHMARK_RUNS=1 sh tests/benchmark.sh ./$(BIN) ./$(OBJDIR)/benchmark_clock$(EXE) > $(OBJDIR)/benchmark-smoke.csv
	@head -n 1 $(OBJDIR)/benchmark-smoke.csv | grep -Fx 'schema,case,selection,target,host_os,host_arch,cpu,compiler_identity,executable_bytes,text_bytes,assembly_source_bytes,sample_count,runs_per_sample,median_wall_ms,mad_wall_ms,median_cycles,mad_cycles,cycle_source'
	@awk -F, 'NR == 7 && $$2 == "calls" && $$3 == "generic" && $$9 > 0 && $$11 > 0 { ok=1 } END { exit !ok }' $(OBJDIR)/benchmark-smoke.csv
check-asan-hook: $(OBJDIR)/test_native_sanitizer$(EXE)
	sh tests/check_asan_hook.sh ./$(OBJDIR)/test_native_sanitizer$(EXE)
check-msan: $(OBJDIR)/test_msan$(EXE)
	sh tests/check_msan.sh ./$(OBJDIR)/test_msan$(EXE)
test: check check-native check-strict-native-runtime check-native-sanitizer check-gates check-truth check-native-truth
ifeq ($(BUILD_MODE),msan)
test: check-msan
endif
check: $(BIN) $(OBJDIR)/test_ir$(EXE) $(OBJDIR)/test_codegen_runtime$(EXE) $(OBJDIR)/test_static_abi$(EXE) $(OBJDIR)/test_regalloc$(EXE) $(OBJDIR)/test_optimize$(EXE) $(OBJDIR)/test_value_facts$(EXE) $(OBJDIR)/test_ir_effects$(EXE) $(OBJDIR)/test_isel$(EXE) $(OBJDIR)/test_ranges$(EXE)
	./$(OBJDIR)/test_ir$(EXE)
	./$(OBJDIR)/test_codegen_runtime$(EXE)
	./$(OBJDIR)/test_static_abi$(EXE)
	./$(OBJDIR)/test_regalloc$(EXE)
	./$(OBJDIR)/test_optimize$(EXE)
	./$(OBJDIR)/test_value_facts$(EXE)
	./$(OBJDIR)/test_ir_effects$(EXE)
	./$(OBJDIR)/test_isel$(EXE)
	./$(OBJDIR)/test_ranges$(EXE)
	sh tests/check_native_runtime_boundary.sh ./$(BIN)
	$(DEBUGGER_TEST_ENV) sh tests/check_debug_metadata.sh ./$(BIN)
	$(TEST_SANITIZER_ENV) sh tests/run_tests.sh ./$(BIN)
$(OBJDIR)/test_regalloc$(EXE): tests/test_regalloc.c src/regalloc.c src/registers.c include/regalloc.h include/target.h include/ir.h Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) -UNDEBUG tests/test_regalloc.c src/regalloc.c src/registers.c -o "$@"
$(OBJDIR)/test_codegen_runtime$(EXE): tests/test_codegen_runtime.c $(RUNTIME_OBJS) include/codegen_abi.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) -UNDEBUG tests/test_codegen_runtime.c $(RUNTIME_OBJS) -o "$@"
$(OBJDIR)/test_static_abi$(EXE): tests/test_static_abi.c $(RUNTIME_OBJS) include/bpp_abi.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) -UNDEBUG tests/test_static_abi.c $(RUNTIME_OBJS) -o "$@"
$(OBJDIR)/test_ir$(EXE): tests/test_ir.c src/ir.c include/ir.h include/parser.h include/lexer.h Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) -UNDEBUG src/ir.c tests/test_ir.c -o "$@"
$(OBJDIR)/test_optimize$(EXE): tests/test_optimize.c src/ir.c src/ir_effects.c src/value_facts.c src/optimize.c include/ir.h include/ir_effects.h include/value_facts.h include/target.h Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) -UNDEBUG tests/test_optimize.c src/ir.c src/ir_effects.c src/value_facts.c src/optimize.c -o "$@"
$(OBJDIR)/test_value_facts$(EXE): tests/test_value_facts.c src/value_facts.c include/value_facts.h include/ir.h Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) -UNDEBUG tests/test_value_facts.c src/value_facts.c -o "$@"
$(OBJDIR)/test_ir_effects$(EXE): tests/test_ir_effects.c src/ir_effects.c include/ir_effects.h include/ir.h Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) -UNDEBUG tests/test_ir_effects.c src/ir_effects.c -o "$@"
$(OBJDIR)/test_isel$(EXE): tests/test_isel.c src/ir.c src/ir_effects.c src/isel.c src/target.c src/range.c src/emit_arm64.c src/emit_x64.c src/raw_native.c src/regalloc.c src/registers.c src/value_facts.c src/locations.c src/strict_runtime.c include/isel.h include/target.h include/ir.h Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) -UNDEBUG tests/test_isel.c src/ir.c src/ir_effects.c src/isel.c src/target.c src/range.c src/emit_arm64.c src/emit_x64.c src/raw_native.c src/regalloc.c src/registers.c src/value_facts.c src/locations.c src/strict_runtime.c -o "$@"
$(OBJDIR)/test_ranges$(EXE): tests/test_ranges.c src/ir.c src/ir_effects.c src/target.c src/range.c src/isel.c src/emit_arm64.c src/emit_x64.c src/raw_native.c src/regalloc.c src/registers.c src/value_facts.c src/locations.c src/strict_runtime.c include/target.h include/ir.h Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) -UNDEBUG tests/test_ranges.c src/ir.c src/ir_effects.c src/target.c src/range.c src/isel.c src/emit_arm64.c src/emit_x64.c src/raw_native.c src/regalloc.c src/registers.c src/value_facts.c src/locations.c src/strict_runtime.c -o "$@"
$(OBJDIR)/generate_case$(EXE): tests/generate_case.c Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) tests/generate_case.c -o "$@"
$(OBJDIR)/benchmark_clock$(EXE): tests/benchmark_clock.c Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) tests/benchmark_clock.c -o "$@"
$(OBJDIR)/test_native_sanitizer$(EXE): tests/test_native_sanitizer.c $(RUNTIME_OBJS) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) tests/test_native_sanitizer.c $(RUNTIME_OBJS) -o "$@"
$(OBJDIR)/test_msan$(EXE): tests/test_msan.c Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) tests/test_msan.c -o "$@"
clean:
	rm -rf build bin/debug bin/tsan bin/release
-include $(DEPS)

check-verification: $(BIN)
	$(PYTHON) tests/check_verification.py stress ./$(BIN)
check-reproducible: $(BIN)
	$(PYTHON) tests/check_verification.py reproducible ./$(BIN)
check-fuzz: $(BIN) $(OBJDIR)/fuzz_compiler$(EXE) $(OBJDIR)/generate_case$(EXE)
	$(PYTHON) tests/check_verification.py fuzz ./$(BIN) --harness ./$(OBJDIR)/fuzz_compiler$(EXE) --generator ./$(OBJDIR)/generate_case$(EXE)
$(OBJDIR)/fuzz_compiler$(EXE): tests/fuzz_compiler.c $(filter-out $(OBJDIR)/src/main.o,$(OBJS)) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) tests/fuzz_compiler.c $(filter-out $(OBJDIR)/src/main.o,$(OBJS)) -o "$@"

.PHONY: check-gates
check-gates: $(BIN)
	$(PYTHON) tests/check_gates.py ./$(BIN)

.PHONY: check-truth
check-truth: $(OBJDIR)/test_truth$(EXE)
	./$(OBJDIR)/test_truth$(EXE)
$(OBJDIR)/test_truth$(EXE): tests/test_truth.c src/truth.c include/truth.h
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) $(MODE_LINK_FLAGS) -UNDEBUG tests/test_truth.c src/truth.c -o "$@"

.PHONY: check-native-truth
check-native-truth: $(BIN)
	$(PYTHON) tests/check_native_truth.py ./$(BIN)
