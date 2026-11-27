#!/bin/bash
set -euo pipefail

if [ "${1:-}" = --help ]; then
    printf 'Usage: sudo bash test-icb.sh VULKAN_INCLUDE RADV_DYLIB OUTPUT_DIR\n'
    printf 'Builds the production Metal bundle and compares direct draws with CPU-recorded render ICBs in the macOS VM.\n'
    exit 0
fi
[ "$#" -eq 3 ] || { printf 'Expected VULKAN_INCLUDE RADV_DYLIB OUTPUT_DIR; use --help.\n' >&2; exit 2; }
[ "$(uname -s)" = Darwin ] || { printf 'Requires a macOS VM.\n' >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || { printf 'Run as root inside the macOS VM.\n' >&2; exit 2; }

source_dir=$(cd "$(dirname "$0")" && pwd)
include=$(cd "$1" && pwd)
radv=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
mkdir -p "$3"
out=$(cd "$3" && pwd)
bundle="$out/GarnetMetal.bundle"
mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources"
cp "$source_dir/Info.plist" "$bundle/Contents/Info.plist"
cp "$radv" "$bundle/Contents/Resources/libvulkan_radeon.dylib"

common=(-arch x86_64 -fobjc-arc -O1 -Wall -Wextra -Werror -I "$include"
        -framework Foundation -framework Metal)
clang "${common[@]}" -framework IOKit -framework IOSurface -bundle \
    "$source_dir/GarnetDevice.m" -o "$bundle/Contents/MacOS/GarnetMetal" \
    > "$out/build-bundle.log" 2>&1
clang "${common[@]}" "$source_dir/test-icb.m" -o "$out/test-icb" \
    > "$out/build-test.log" 2>&1

mkdir -p "$out/cache"
result=0
env RDNA4M_TEST_USERCACHE="$out/cache" \
    "$out/test-icb" "$bundle" > "$out/run.log" 2>&1 || result=$?
grep '^ICB_' "$out/run.log" || cat "$out/run.log" >&2
printf 'ICB_NOTE VM RADV-over-MoltenVK execution; physical GPU behaviour is not claimed\n'
exit "$result"
