.DEFAULT_GOAL := all

BPP_ROOT ?= ../..
CC ?= cc
AR ?= ar
RANLIB ?= ranlib
BUILD_DIR ?= build
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_S),Darwin)
DEFAULT_TARGET := macos-arm64
else ifeq ($(UNAME_M),x86_64)
DEFAULT_TARGET := linux-x86_64
else ifeq ($(UNAME_M),amd64)
DEFAULT_TARGET := linux-x86_64
else ifeq ($(UNAME_M),aarch64)
DEFAULT_TARGET := linux-arm64
else ifeq ($(UNAME_M),arm64)
DEFAULT_TARGET := linux-arm64
else
DEFAULT_TARGET := unsupported
endif
TARGET ?= $(DEFAULT_TARGET)

ifeq ($(TARGET),unsupported)
$(error set TARGET to macos-arm64, linux-x86_64, or linux-arm64)
endif

CPPFLAGS := -I$(BPP_ROOT)/include -Ivendor/include -Ivendor/lib -Ivendor/src -Ivendor/src/core
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE

XF_SRCS := vendor/src/ast.c vendor/src/interp.c vendor/src/lexer.c vendor/src/parser.c vendor/src/simd.c \
           vendor/src/symTable.c vendor/src/value.c vendor/src/vm.c vendor/src/gc.c vendor/src/mt.c \
           vendor/src/core.c vendor/src/core/helpers.c vendor/src/core/math.c vendor/src/core/str.c \
           vendor/src/core/os.c vendor/src/core/generics.c vendor/src/core/regex.c vendor/src/core/format.c \
           vendor/src/core/ds.c vendor/src/core/edit.c vendor/src/core/process.c vendor/src/core/img.c \
           vendor/lib/driver.c vendor/lib/api.c
SRCS := src/bpp_xf_bridge.c src/interp_lifecycle.c $(XF_SRCS)
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
ARCHIVE := libxf.bpp.a
MANIFEST := libxf.bppabi
MANIFEST_TOOL := $(BUILD_DIR)/write_manifest

.PHONY: all clean test
all: $(ARCHIVE) $(MANIFEST)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(ARCHIVE): $(OBJS)
	$(AR) rcs "$@" $(OBJS)
	$(RANLIB) "$@"

$(MANIFEST_TOOL): src/write_manifest.c
	@mkdir -p "$(@D)"
	$(CC) $(CFLAGS) "$<" -o "$@"

$(MANIFEST): $(ARCHIVE) $(MANIFEST_TOOL) Makefile
	$(MANIFEST_TOOL) "$(TARGET)" "$(ARCHIVE)" "$@"

test: all
	@printf '%s\n' 'import library "xf";' 'xf_run_string("BEGIN { print(6 * 7); }");' 'return xf_version();' > "$(BUILD_DIR)/smoke.bpp"
	$(BPP_ROOT)/bin/release/bpp -L . --print-result -o "$(BUILD_DIR)/smoke" "$(BUILD_DIR)/smoke.bpp"
	"$(BUILD_DIR)/smoke" | grep -Fx '42'

clean:
	rm -rf "$(BUILD_DIR)" "$(ARCHIVE)" "$(MANIFEST)"
