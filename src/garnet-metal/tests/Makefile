# Host unit and contract regressions for GarnetMetal (runs offline on Linux/macOS; no GPU).
CC := clang
BUILD ?= ../build-host-metal-tests
SRC := ..
KEXT_SRC := ../../garnet/src
REPO_ROOT := ../../..
CFLAGS := -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -I$(SRC) -I$(KEXT_SRC)
LDFLAGS := -lm -ldl -lpthread

NOARG_TESTS := \
  test-argument-capacity \
  test-argument-limit-snapshot \
  test-argument-public-sampler \
  test-argument-scan \
  test-argument-summary-owners \
  test-argument-summary-plan \
  test-argument-writer-epochs \
  test-async-compute \
  test-bar-policy \
  test-binary-archive \
  test-buffer-ranges \
  test-buffer-registry \
  test-cblog \
  test-compute-icb \
  test-counters-amp \
  test-crc \
  test-devcaps \
  test-dispflip \
  test-display-commits \
  test-drawcache \
  test-dynlib-vft \
  test-gather-caps \
  test-heap-backing \
  test-heapalloc \
  test-hotswap \
  test-impcache \
  test-io-residency \
  test-mesh-runtime \
  test-nocopy-protection \
  test-nocopy-range \
  test-nocopy-ro-policy \
  test-null-remap \
  test-null-shape \
  test-pitch \
  test-plane \
  test-pool \
  test-query-caps \
  test-queue-pipe \
  test-rt-accel \
  test-rt-runtime \
  test-sparse-metal \
  test-swizzle-placement \
  test-t1 \
  test-texture-transfer \
  test-vram-surf

SRCDIR_TESTS := \
  test-fencewait \
  test-gate \
  test-gpl \
  test-vkpool

ALL_C_BINS := $(addprefix $(BUILD)/,$(NOARG_TESTS) $(SRCDIR_TESTS))

all: host py

host: $(ALL_C_BINS)
	@set -e; root="$(abspath $(REPO_ROOT))"; src="$(abspath $(SRC))"; \
	for t in $(NOARG_TESTS); do (cd "$$root" && "$(abspath $(BUILD))/$$t"); done; \
	for t in $(SRCDIR_TESTS); do (cd "$$src" && "$(abspath $(BUILD))/$$t" "$$src"); done

py:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest test-validation-runner.py
	PYTHONDONTWRITEBYTECODE=1 python3 test-reflection-trim.py
	PYTHONDONTWRITEBYTECODE=1 python3 test-nocopy-completion-production.py
	PYTHONDONTWRITEBYTECODE=1 python3 test-present-analysis.py
	bash test-windowserver-shader-recovery.sh

$(BUILD):
	mkdir -p $@

$(BUILD)/%: %.c | $(BUILD)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all host py clean
