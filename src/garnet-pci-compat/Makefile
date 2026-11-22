# Makefile — cross-compile GarnetPCIBarCompat.kext for x86_64 from any host.
# Targets macOS 11+ ABI (including macOS 15 / 26).
#   make          build build/GarnetPCIBarCompat.kext
#   make test     build and run host unit tests
#   make clean

.DEFAULT_GOAL := all
PRODUCT   := GarnetPCIBarCompat
BUNDLE_ID := org.garnet.pcibarcompat
ARCH      := x86_64
DEPLOY    := 11.0
HOST_OS   := $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
SDK       := $(shell xcrun --sdk macosx --show-sdk-path)
NM        ?= nm -arch $(ARCH)
CXXFILT   ?= c++filt
SIGN      ?= 1
HOST_CXX  ?= clang++
HOST_SAN  ?= -fsanitize=address,undefined
else
SDK       = $(MKSDK)
NM        ?= llvm-nm
CXXFILT   ?= $(shell command -v llvm-cxxfilt || command -v c++filt || echo llvm-cxxfilt) -_
SIGN      ?= 0
HOST_CXX  ?= clang++
HOST_SAN  ?= -fsanitize=address,undefined
endif
MKSDK     ?= ../../build-toolchain/sdk
KEXT_LD   ?=

BUILD := build
KEXT  := $(BUILD)/$(PRODUCT).kext
EXEC  := $(KEXT)/Contents/MacOS/$(PRODUCT)

ifeq ($(origin CXX),default)
CXX := clang++
endif
ifeq ($(origin CC),default)
CC  := clang
endif
CXX ?= clang++
CC  ?= clang

CXX_SRCS := src/GarnetPCIBarCompat.cpp
C_SRCS   := src/kmod_info.c
OBJS     := $(patsubst %.cpp,$(BUILD)/%.o,$(CXX_SRCS)) $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))

COMMON := -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) \
  -I$(MKSDK)/Headers -Isrc -mmacosx-version-min=$(DEPLOY) \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
  -D_FORTIFY_SOURCE=0 -nostdinc -fno-builtin -fno-common -fno-stack-protector \
  -mkernel -fapple-kext -Wall -Os -g
CXXFLAGS := $(COMMON) -std=c++17 -fno-exceptions -fno-rtti -fcheck-new -fno-c++-static-destructors
CFLAGS   := $(filter-out -fapple-kext,$(COMMON))
LDFLAGS  := -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) \
  -nostdlib -Xlinker -kext -L$(MKSDK)/Library/x86_64 \
  $(if $(wildcard $(MKSDK)/Library/x86_64/libkmodc++.a),-lkmodc++) -lkmod -Wl,-no_deduplicate \
  $(if $(KEXT_LD),--ld-path=$(KEXT_LD))

all: $(KEXT)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d)

$(KEXT): $(OBJS) Info.plist
	@mkdir -p $(KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OBJS) -o $(EXEC)
	@# Verify zero undefined internal symbols that would silently prevent kernel loading.
	@$(NM) -u $(EXEC) > $(BUILD)/undefined-symbols.txt
	@bad=$$($(CXXFILT) < $(BUILD)/undefined-symbols.txt | grep -E 'Garnet|garnet::|garnet_pci|RDNA4|rdna4|pcibarcompat|Lilu' || true); \
	if [ -n "$$bad" ]; then \
	  echo "LINK ERROR: these symbols are OURS or forbidden but undefined — the kext will not load:"; \
	  echo "$$bad" | sed 's/^/    /'; \
	  rm -f $(EXEC); exit 1; \
	fi
	cp Info.plist $(KEXT)/Contents/Info.plist
	find $(KEXT) \( -name '.DS_Store' -o -name '._*' \) -delete
ifeq ($(SIGN),1)
	-xattr -d com.apple.FinderInfo $(KEXT) 2>/dev/null
	xattr -cr $(KEXT)
	codesign --force --sign - $(KEXT) || { xattr -d com.apple.FinderInfo $(KEXT) 2>/dev/null; xattr -cr $(KEXT); codesign --force --sign - $(KEXT); }
	@test -f $(KEXT)/Contents/_CodeSignature/CodeResources || { echo 'codesign produced no CodeResources'; exit 1; }
else ifeq ($(SIGN),0)
	@echo "Unsigned kext: for OpenCore injection; macOS kmutil loading requires separate validation."
else
	$(error SIGN must be 0 or 1)
endif
	@echo "Built $(KEXT) for $(ARCH) (min macOS $(DEPLOY))"

test: tests/pci_bar_compat_test.cpp src/pci_bar_compat_pure.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror $(HOST_SAN) \
	  -Isrc tests/pci_bar_compat_test.cpp -o $(BUILD)/pci_bar_compat_test
	$(BUILD)/pci_bar_compat_test

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
