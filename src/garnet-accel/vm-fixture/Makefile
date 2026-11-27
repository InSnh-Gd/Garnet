# VM fixture only; never copied by the driver package installer.
SDK := $(shell xcrun --sdk macosx --show-sdk-path)
MKSDK ?= $(GARNET_MKSDK)
GARNET_LAYOUT_OUT ?= $(abspath ../../../tools/ioaccel-layout/layout-out)
OUT := build
KEXT := $(OUT)/GarnetVMFixture.kext
COMMON := -arch x86_64 -target x86_64-apple-macos11.0 -isysroot $(SDK) -I$(MKSDK)/Headers -I$(GARNET_LAYOUT_OUT) -I../src \
    -I$(MKSDK)/../src/garnet-accel/src \
    -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -D_FORTIFY_SOURCE=0 -nostdinc \
    -fno-builtin -fno-common -fno-stack-protector -mkernel -Wall -Wextra -Werror -Os -g

all: $(KEXT) $(OUT)/fixture-control $(OUT)/test-device-port $(OUT)/test-user-client

$(OUT):
	mkdir -p $@

$(OUT)/fixture.o: Fixture.cpp $(GARNET_LAYOUT_OUT)/IOAccelFamily2_decl.h | $(OUT)
	clang++ $(COMMON) -fapple-kext -std=c++17 -fno-exceptions -fno-rtti -fcheck-new -fno-c++-static-destructors -c $< -o $@

$(OUT)/kmod_info.o: kmod_info.c | $(OUT)
	clang $(COMMON) -c $< -o $@

$(KEXT): $(OUT)/fixture.o $(OUT)/kmod_info.o Info.plist
	mkdir -p $@/Contents/MacOS
	clang++ -arch x86_64 -target x86_64-apple-macos11.0 -isysroot $(SDK) -nostdlib -Xlinker -kext \
	    -L$(MKSDK)/Library/x86_64 $(OUT)/fixture.o $(OUT)/kmod_info.o -lkmodc++ -lkmod -o $@/Contents/MacOS/GarnetVMFixture
	cp Info.plist $@/Contents/Info.plist
	codesign --force --sign - $@
	codesign --verify --strict -v $@

$(OUT)/fixture-control: control.c | $(OUT)
	clang -arch x86_64 -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation $< -o $@

$(OUT)/test-device-port: test-device-port.m | $(OUT)
	clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal -framework IOKit $< -o $@

$(OUT)/test-user-client: test-user-client.c | $(OUT)
	clang -arch x86_64 -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation $< -o $@

.PHONY: all
