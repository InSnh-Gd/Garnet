#!/bin/bash
set -euo pipefail
[[ $# == 3 && $(uname -s) == Darwin ]] || { echo 'usage: test-native-buffer-route.sh VULKAN_INCLUDE RADV_DYLIB OUTPUT_DIR' >&2; exit 2; }
source_dir=$(cd "$(dirname "$0")" && pwd)
include=$(cd "$1" && pwd)
mkdir -p "$3"
out=$(cd "$3" && pwd)
bundle="$out/GarnetMetal.bundle"
mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources"
cp "$source_dir/Info.plist" "$bundle/Contents/Info.plist"
cp "$2" "$bundle/Contents/Resources/libvulkan_radeon.dylib"
common=(-arch x86_64 -fobjc-arc -O1 -Wall -Wextra -Werror -I "$include" -framework Foundation -framework Metal)
clang "${common[@]}" -framework IOKit -framework IOSurface -bundle "$source_dir/GarnetDevice.m" -o "$out/GarnetMetal-production"
clang "${common[@]}" -DRDNA4_NATIVE_ROUTE_GLUE -framework IOKit -framework IOSurface -bundle \
    "$source_dir/test-native-buffer-route.m" -o "$bundle/Contents/MacOS/GarnetMetal"
clang "${common[@]}" -DRDNA4_FEATURE_AUDIT -dynamiclib -install_name @rpath/librdna4featureaudit.dylib \
    "$source_dir/test-system-pipelines.m" -o "$out/librdna4featureaudit.dylib"
clang "${common[@]}" -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
    "$source_dir/test-native-buffer-route.m" -o "$out/test-native-buffer-route"
clang "${common[@]}" -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
    "$source_dir/test-command-completion.m" -o "$out/test-command-completion"
result=0
for mode in complete order concurrent missing no-idle compute render texture host byte-fill unsupported partial timeout wait-timeout wait-error submit-error zero-seq unsupported-seq idle-error; do
    "$out/test-native-buffer-route" "$bundle" "$mode" > "$out/$mode.log" 2>&1 || result=1
    grep '^NATIVE_ROUTE_' "$out/$mode.log" || { tail -10 "$out/$mode.log"; result=1; }
done
production="$out/production/GarnetMetal.bundle"
mkdir -p "$production/Contents/MacOS" "$production/Contents/Resources"
cp "$source_dir/Info.plist" "$production/Contents/Info.plist"
cp "$out/GarnetMetal-production" "$production/Contents/MacOS/GarnetMetal"
cp "$2" "$production/Contents/Resources/libvulkan_radeon.dylib"
"$out/test-command-completion" "$production" complete > "$out/production-real-exports.log" 2>&1 || result=1
grep '^COMPLETION_' "$out/production-real-exports.log" || result=1
if [[ -n ${RDNA4_LEGACY_RADV:-} ]]; then
    nm -gU "$RDNA4_LEGACY_RADV" > "$out/legacy-symbols.txt"
    if grep -q 'radv_darwin_buffer_' "$out/legacy-symbols.txt"; then
        echo 'legacy library unexpectedly exports native buffer ABI' >&2; exit 1
    fi
    cp "$RDNA4_LEGACY_RADV" "$production/Contents/Resources/libvulkan_radeon.dylib"
    "$out/test-command-completion" "$production" complete > "$out/production-legacy.log" 2>&1 || result=1
    grep '^COMPLETION_' "$out/production-legacy.log" || result=1
fi
shasum -a 256 "$source_dir/GarnetDevice.m" "$source_dir"/rdna4_*_impl.inc "$source_dir/rdna4_native_blit.h" "$source_dir/test-native-buffer-route.m" \
    "$source_dir/test-native-buffer-route.sh" "$out/GarnetMetal-production" "$bundle/Contents/MacOS/GarnetMetal" \
    "$out/test-native-buffer-route" "$out/test-command-completion" "$out/librdna4featureaudit.dylib" \
    "$bundle/Contents/Resources/libvulkan_radeon.dylib" "$production/Contents/Resources/libvulkan_radeon.dylib" > "$out/SHA256SUMS"
exit "$result"
