.DEFAULT_GOAL := all
LLVM_PREFIX ?= /opt/homebrew/opt/llvm/bin
ifeq ($(origin CC),default)
CC := clang
endif
ifneq ($(wildcard $(LLVM_PREFIX)/clang),)
CC := $(LLVM_PREFIX)/clang
endif
BUILD_MODE ?= debug
DEBUG_SANITIZERS ?= address,leak,undefined
ifeq ($(BUILD_MODE),debug)
OPT := -O1
SANITIZERS := $(DEBUG_SANITIZERS)
MODE_FLAGS := -g3 -fno-omit-frame-pointer -fno-sanitize-recover=undefined
else ifeq ($(BUILD_MODE),tsan)
OPT := -O1
SANITIZERS := thread
MODE_FLAGS := -g3 -fno-omit-frame-pointer
else ifeq ($(BUILD_MODE),release)
OPT := -O2
SANITIZERS :=
MODE_FLAGS := -DNDEBUG
else
$(error BUILD_MODE must be debug, tsan, or release)
endif
ifneq ($(SANITIZERS),)
MODE_FLAGS += -fsanitize=$(SANITIZERS)
endif
CPPFLAGS += -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?=
OBJDIR := build/$(BUILD_MODE)/obj
BIN := bin/$(BUILD_MODE)/bpp
SRCS := src/main.c src/compiler.c src/lexer.c src/parser.c src/ir.c src/emit_c.c src/emit_arm64.c
OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d) $(OBJDIR)/src/runtime.d $(OBJDIR)/src/value.d $(OBJDIR)/src/codegen_runtime.d
.PHONY: all debug tsan release check check-arm64 clean
RUNTIME_OBJS := $(OBJDIR)/src/runtime.o $(OBJDIR)/src/value.o $(OBJDIR)/src/codegen_runtime.o
all: $(BIN)
debug:
	$(MAKE) BUILD_MODE=debug all
tsan:
	$(MAKE) BUILD_MODE=tsan all
release:
	$(MAKE) BUILD_MODE=release all
$(OBJDIR)/src/main.o: CPPFLAGS += '-DBPP_PROJECT_DIR="$(CURDIR)"' '-DBPP_HOST_CC="$(CC)"' '-DBPP_SANITIZERS="$(SANITIZERS)"' '-DBPP_OPTIMIZATION="$(OPT)"' '-DBPP_RUNTIME_OBJECT="$(CURDIR)/$(OBJDIR)/src/runtime.o"' '-DBPP_VALUE_OBJECT="$(CURDIR)/$(OBJDIR)/src/value.o"' '-DBPP_ARM_RUNTIME_OBJECT="$(CURDIR)/$(OBJDIR)/src/codegen_runtime.o"'
$(OBJDIR)/%.o: %.c Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) -MMD -MP -c "$<" -o "$@"
$(BIN): $(OBJS) $(RUNTIME_OBJS)
	mkdir -p "$(@D)"
	$(CC) $(LDFLAGS) $(MODE_FLAGS) -o "$@" $(OBJS) $(LDLIBS)
check-arm64: $(BIN)
	BPP_TEST_BACKEND=arm64 sh tests/run_tests.sh ./$(BIN)
check: $(BIN) $(OBJDIR)/test_ir $(OBJDIR)/test_codegen_runtime
	./$(OBJDIR)/test_ir
	./$(OBJDIR)/test_codegen_runtime
	sh tests/run_tests.sh ./$(BIN)
$(OBJDIR)/test_codegen_runtime: tests/test_codegen_runtime.c $(RUNTIME_OBJS) include/codegen_abi.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) -UNDEBUG tests/test_codegen_runtime.c $(RUNTIME_OBJS) -o "$@"
$(OBJDIR)/test_ir: tests/test_ir.c src/ir.c include/ir.h include/parser.h include/lexer.h Makefile
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(MODE_FLAGS) -UNDEBUG src/ir.c tests/test_ir.c -o "$@"
clean:
	rm -rf build bin/debug bin/tsan bin/release
-include $(DEPS)
