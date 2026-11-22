#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash agdc-live-build.sh [--object|--service] KERNEL_SDK PREPARED_PROBE_DIR\nBuilds a VM-only probe. --object checks an isolated constructor/init/release. --service checks the shared AGDC builder and vendor callback on a real VM framebuffer; it does not execute native_flow or a native GPU. It does not install or load it.\n'
    exit 0
fi
object_test=0
service_test=0
if [ "${1:-}" = --object ]; then object_test=1; shift; fi
if [ "${1:-}" = --service ]; then service_test=1; shift; fi
[ "$((object_test + service_test))" -le 1 ] || exit 2
[ "$#" -eq 2 ] && [ "$(uname -s)" = Darwin ] || exit 2
sdk=$(xcrun --sdk macosx --show-sdk-path)
kernel=$(cd "$1" && pwd)
cd "$2"
common=(-arch x86_64 -target x86_64-apple-macos11.0 -isysroot "$sdk" -I "$kernel/Headers" -I .
    -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -D_FORTIFY_SOURCE=0 -nostdinc
    -fno-builtin -fno-common -fno-stack-protector -mkernel -Wall -Wextra -Werror -Os -g)
if [ "$object_test" -eq 1 ]; then common+=(-DRDNA4_AGDC_OBJECT_TEST); fi
if [ "$service_test" -eq 1 ]; then common+=(-DRDNA4_AGDC_SERVICE_TEST); fi
clang++ "${common[@]}" -fapple-kext -std=c++17 -fno-exceptions -fno-rtti -fcheck-new -fno-c++-static-destructors -c agdc-live.cpp -o live.o
clang "${common[@]}" -c kmod_info.c -o kmod_info.o
bundle=RDNA4AGDCLive.kext
mkdir -p "$bundle/Contents/MacOS"
clang++ -arch x86_64 -target x86_64-apple-macos11.0 -isysroot "$sdk" -nostdlib -Xlinker -kext \
    -L "$kernel/Library/x86_64" live.o kmod_info.o -lkmodc++ -lkmod -o "$bundle/Contents/MacOS/RDNA4AGDCLive"
cp Info.plist "$bundle/Contents/Info.plist"
if [ "$service_test" -eq 1 ]; then
    /usr/libexec/PlistBuddy -c 'Set :IOKitPersonalities:RDNA4AGDCLive:IOProviderClass IONDRVFramebuffer' "$bundle/Contents/Info.plist"
    /usr/libexec/PlistBuddy -c 'Delete :IOKitPersonalities:RDNA4AGDCLive:IOResourceMatch' "$bundle/Contents/Info.plist"
    /usr/libexec/PlistBuddy -c 'Add :IOKitPersonalities:RDNA4AGDCLive:IOMatchCategory string RDNA4AGDCServiceTest' "$bundle/Contents/Info.plist"
fi
codesign --force --sign - "$bundle"
codesign --verify --strict -v "$bundle"
