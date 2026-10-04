# SPDX-License-Identifier: MIT

NDK ?= $(ANDROID_NDK_HOME)
API ?= 35
BUILD_DIR ?= build
HOST_CC ?= cc

TOOLCHAIN_DIR := $(firstword $(wildcard $(NDK)/toolchains/llvm/prebuilt/*))
ANDROID_CC := $(TOOLCHAIN_DIR)/bin/aarch64-linux-android$(API)-clang

CPPFLAGS ?= -Iinclude
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic

STREAM := $(BUILD_DIR)/adreno_perf_stream
SWEEP := $(BUILD_DIR)/adreno_perf_sweep
TARGETS := $(STREAM) $(SWEEP)
TABLE := include/a8xx_perf_table.inc
TABLE_CHECK := $(BUILD_DIR)/a8xx_perf_table.generated.inc

.PHONY: all check check-ndk clean deploy table table-check

all: $(TARGETS)

check-ndk:
	@test -n "$(NDK)" || { echo "Set ANDROID_NDK_HOME (or pass NDK=/path/to/android-ndk)." >&2; exit 2; }
	@test -x "$(ANDROID_CC)" || { echo "Android compiler not found: $(ANDROID_CC)" >&2; exit 2; }

$(BUILD_DIR):
	mkdir -p "$@"

$(STREAM): src/adreno_perf_stream.c $(TABLE) | $(BUILD_DIR) check-ndk
	"$(ANDROID_CC)" $(CPPFLAGS) $(CFLAGS) -o "$@" "$<"

$(SWEEP): src/adreno_perf_sweep.c $(TABLE) | $(BUILD_DIR) check-ndk
	"$(ANDROID_CC)" $(CPPFLAGS) $(CFLAGS) -o "$@" "$<"

table:
	python3 tools/generate_a8xx_perf_table.py data/a8xx_perfcntrs.xml > "$(TABLE).tmp"
	mv "$(TABLE).tmp" "$(TABLE)"

table-check: | $(BUILD_DIR)
	python3 tools/generate_a8xx_perf_table.py data/a8xx_perfcntrs.xml > "$(TABLE_CHECK)"
	cmp "$(TABLE_CHECK)" "$(TABLE)"

check: table-check
	"$(HOST_CC)" -std=c11 -Wall -Wextra -Wpedantic -Iinclude -fsyntax-only \
		src/adreno_perf_stream.c src/adreno_perf_sweep.c
	bash -n scripts/deploy.sh scripts/pull_latest_sweep.sh

deploy: all
	scripts/deploy.sh

clean:
	rm -f "$(STREAM)" "$(SWEEP)" "$(TABLE_CHECK)" "$(TABLE).tmp"
