#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-fences.sh VULKAN_INCLUDE RADV_DYLIB OUTPUT_DIR\n'
    printf 'Tests production MTLFence object boundaries and encoded-position scheduling.\n'
    printf 'Vulkan submission/fence behavior is simulated; this is not a physical-GPU PASS.\n'
    exit 0
fi
[ "$#" -eq 3 ] || { printf 'Expected three arguments; use --help.\n' >&2; exit 2; }
[ "$(uname -s)" = Darwin ] || { printf 'Requires a macOS VM.\n' >&2; exit 2; }
source_dir=$(cd "$(dirname "$0")" && pwd)
include=$(cd "$1" && pwd)
radv=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
mkdir -p "$3"
out=$(cd "$3" && pwd)
bundle="$out/GarnetMetal.bundle"
mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources"
cp "$source_dir/Info.plist" "$bundle/Contents/Info.plist"
cp "$radv" "$bundle/Contents/Resources/libvulkan_radeon.dylib"

common=(-arch x86_64 -fobjc-arc -O1 -Wall -Wextra -Werror -I "$include" -framework Foundation -framework Metal)
clang "${common[@]}" -framework IOKit -framework IOSurface -bundle "$source_dir/GarnetDevice.m" \
    -o "$bundle/Contents/MacOS/GarnetMetal"
clang "${common[@]}" -DRDNA4_FEATURE_AUDIT -dynamiclib \
    -install_name @rpath/librdna4featureaudit.dylib "$source_dir/test-system-pipelines.m" \
    -o "$out/librdna4featureaudit.dylib"
clang "${common[@]}" -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
    "$source_dir/test-fences.m" -o "$out/test-fences"

result=0
for mode in success failure lost fence-timeout; do
    "$out/test-fences" "$bundle" "$mode" > "$out/$mode.txt" 2>&1 || result=1
    grep '^FENCE_' "$out/$mode.txt" || { cat "$out/$mode.txt" >&2; result=1; }
done
printf 'FENCE_SEGMENT_NOTE simulated Vulkan; physical GPU fence visibility is not claimed\n'
exit "$result"
