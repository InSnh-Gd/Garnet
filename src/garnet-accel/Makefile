# GarnetAccel aux kext (org.garnet.accel). Same recipe as src/garnet/Makefile
# (MacKernelSDK, macOS 11 ABI, -mkernel -fapple-kext, -Xlinker -kext).
#   make        kext + gates A/B/C + host twin (each FAILS the make)
#   make test   host tests
# Linux cross: pass MKSDK + KEXT_LD, BUILD + GARNET_LAYOUT_OUT outside the repo,
# and GARNET_KC_WORK at the matching KC dir. Tool/SDK choice travels as GARNET_NM/GARNET_SDK.
PRODUCT := GarnetAccel
ARCH    := x86_64
DEPLOY  := 11.0
HOST_OS := $(shell uname -s)
MKSDK   ?= $(if $(GARNET_MKSDK),$(GARNET_MKSDK),../../build-toolchain/sdk)
ifeq ($(HOST_OS),Darwin)
SDK     := $(shell xcrun --sdk macosx --show-sdk-path)
NM      := nm
CXXFILT := c++filt
SIGN    ?= 1
else
# Kernel-only builds need MacKernelSDK, not the macOS userspace SDK.
SDK     = $(MKSDK)
NM      := llvm-nm
CXXFILT := $(shell command -v llvm-cxxfilt || command -v c++filt || echo llvm-cxxfilt) -_
SIGN    ?= 0
endif
KEXT_LD ?=
LAYOUT  := ../../tools/ioaccel-layout
GARNET_LAYOUT_OUT ?= $(abspath $(LAYOUT)/layout-out)
export GARNET_LAYOUT_OUT
export GARNET_NM := $(NM)
export GARNET_SDK := $(abspath $(SDK))
export GARNET_MKSDK := $(abspath $(MKSDK))
BUILD   ?= build
KEXT    := $(BUILD)/$(PRODUCT).kext
EXEC    := $(KEXT)/Contents/MacOS/$(PRODUCT)
GEN     := $(BUILD)/gen
COMMON  := -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) -I$(MKSDK)/Headers -Isrc -I$(GEN) -I$(GARNET_LAYOUT_OUT) \
  -mmacosx-version-min=$(DEPLOY) -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
  -D_FORTIFY_SOURCE=0 -nostdinc -fno-builtin -fno-common -fno-stack-protector -mkernel -fapple-kext -Wall -Os -g
CXXFLAGS := $(COMMON) -std=c++17 -fno-exceptions -fno-rtti -fcheck-new -fno-c++-static-destructors \
  -include src/GarnetBootArgs.h   # rdna4-* boot-arg fallback (logs when a legacy token is used)
LDFLAGS := -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) \
  -nostdlib -Xlinker -kext -L$(MKSDK)/Library/x86_64 \
  $(if $(wildcard $(MKSDK)/Library/x86_64/libkmodc++.a),-lkmodc++) -lkmod -Wl,-no_deduplicate \
  $(if $(KEXT_LD),--ld-path=$(KEXT_LD))
GENHDR  := $(GARNET_LAYOUT_OUT)/IOAccelFamily2_decl.h

all: $(KEXT) gates

# the generated declarations must be fresh: gen_header.py output == the committed header (gate C's input)
$(GEN)/garnet_gate.inc: gates/gen_gate.py gates/common.py gates/leaves.py $(GARNET_LAYOUT_OUT)/model.json $(GARNET_LAYOUT_OUT)/forwarded.json
	@mkdir -p $(GEN)
	python3 gates/gen_gate.py $@

$(GEN)/garnet_tramp.inc: gates/gen_tramp.py gates/gen_gate.py gates/common.py gates/leaves.py $(GARNET_LAYOUT_OUT)/slots.json $(GARNET_LAYOUT_OUT)/forwarded.json $(LAYOUT)/gen_header.py
	@mkdir -p $(GEN)
	python3 gates/gen_tramp.py $@

$(BUILD)/accel.o: src/GarnetAccel.cpp src/garnet_accel_display_impl.inc src/garnetaccel_pure.h src/GarnetMetalOps.h src/GarnetDisplayOps.h $(GENHDR) $(GEN)/garnet_gate.inc $(GEN)/garnet_tramp.inc
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c src/GarnetAccel.cpp -o $@
$(BUILD)/kmod_info.o: src/kmod_info.c
	@mkdir -p $(BUILD)
	$(CC) $(filter-out -fapple-kext,$(COMMON)) -c src/kmod_info.c -o $@

$(KEXT): $(BUILD)/accel.o $(BUILD)/kmod_info.o Info.plist
	@mkdir -p $(KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(BUILD)/accel.o $(BUILD)/kmod_info.o -o $(EXEC)
	@# our symbols must all be defined (a virtual declared and never defined links here and then silently fails in the kernel linker)
	@bad=$$($(NM) -u $(EXEC) | $(CXXFILT) | grep -E 'Garnet|garnet' || true); \
	 if [ -n "$$bad" ]; then echo "LINK ERROR: our symbols undefined: $$bad"; rm -f $(EXEC); exit 1; fi
	cp Info.plist $(KEXT)/Contents/Info.plist
ifeq ($(SIGN),1)
	-xattr -d com.apple.FinderInfo $(KEXT) 2>/dev/null
	xattr -cr $(KEXT)
	codesign --force --sign - $(KEXT) || { xattr -cr $(KEXT); codesign --force --sign - $(KEXT); }
else ifeq ($(SIGN),0)
	@echo "Unsigned kext: for OpenCore injection; macOS kmutil loading requires separate validation."
else
	$(error SIGN must be 0 or 1)
endif
	@echo "Built $(KEXT)"

# gates A and B on the LINKED kext, gate C = the static_asserts already compiled into the object (sizes, slot arithmetic) + the header freshness check
gates: $(KEXT)
	python3 gates/gate_link.py $(EXEC) $(BUILD)/accel.o
	python3 gates/layout_gate_host.py $(EXEC)

test:
	$(MAKE) -C tests all

clean:
	rm -rf $(BUILD)
.PHONY: all gates test clean
