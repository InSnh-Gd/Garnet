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
  test-argscan-incremental \
  test-argument-capacity \
  test-argument-limit-snapshot \
  test-argument-public-sampler \
  test-argument-scan \
  test-argument-summary-owners \
  test-argument-summary-plan \
  test-argument-writer-epochs \
  test-binary-archive \
  test-compute-icb \
  test-counters-amp \
  test-dispflip \
  test-dynlib-vft \
  test-gather-caps \
  test-heap-alloc \
  test-host-display \
  test-host-memory \
  test-host-runtime \
  test-impcache \
  test-io-residency \
  test-mesh-runtime \
  test-null-remap \
  test-null-shape \
  test-rt-accel \
  test-rt-curves \
  test-rt-motion \
  test-rt-runtime \
  test-sparse-metal \
  test-swizzle-placement

SRCDIR_TESTS := \
  test-fencewait \
  test-gate \
  test-gpl \
  test-pipeline-fallback \
  test-vkpool \
  test-xlate-fallback

ASSEMBLED_TESTS := \
  test-cienv \
  test-intfmt \
  test-ioalias \
  test-texdesc

ALL_C_BINS := $(addprefix $(BUILD)/,$(NOARG_TESTS) $(SRCDIR_TESTS) $(ASSEMBLED_TESTS))

all: host assembled py

host: $(addprefix $(BUILD)/,$(NOARG_TESTS) $(SRCDIR_TESTS))
	@set -e; root="$(abspath $(REPO_ROOT))"; src="$(abspath $(SRC))"; \
	for t in $(NOARG_TESTS); do (cd "$$root" && "$(abspath $(BUILD))/$$t"); done; \
	for t in $(SRCDIR_TESTS); do (cd "$$src" && "$(abspath $(BUILD))/$$t" "$$src"); done

assembled: $(addprefix $(BUILD)/,$(ASSEMBLED_TESTS))
	@set -e; asm="$(abspath $(BUILD))/assembled"; src="$(abspath $(SRC))"; \
	mkdir -p "$$asm"; \
	PYTHONDONTWRITEBYTECODE=1 python3 "$$src/assemble-device.py" "$$src/GarnetDevice.m" --include-dir "$$src" --output "$$asm/GarnetDevice.m"; \
	cp "$$src/garnet_intfmt.h" "$$src/Info.plist" "$$asm/"; \
	for t in $(ASSEMBLED_TESTS); do "$(abspath $(BUILD))/$$t" "$$asm"; done

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

.PHONY: all host assembled py clean
