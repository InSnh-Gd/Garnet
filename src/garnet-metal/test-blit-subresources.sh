#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: sudo bash test-blit-subresources.sh BUNDLE OUTPUT_DIR [--prepare VULKAN_INCLUDE]\nTests whole and ranged texture copies across mipmaps, arrays, cubes and 3D images. --prepare audits production RADV with mock BOs without submission.\n'
    exit 0
fi
[ "$#" -eq 2 ] || { [ "$#" -eq 4 ] && [ "$3" = --prepare ]; } || exit 2
[ "$(uname -s)" = Darwin ] && [ "$(id -u)" -eq 0 ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
bundle=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
if [ "${3:-}" = --prepare ]; then
    clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -I "$4" -framework Foundation \
        -DRDNA4_FEATURE_AUDIT -dynamiclib -install_name @rpath/librdna4featureaudit.dylib \
        "$source_dir/test-system-pipelines.m" -o "$out/librdna4featureaudit.dylib"
    clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
        -DRDNA4_BLIT_AUDIT -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
        "$source_dir/test-blit-subresources.m" -o "$out/test-blit-subresources"
else
    clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
        "$source_dir/test-blit-subresources.m" -o "$out/test-blit-subresources"
fi
result=0
for mode in whole-array whole-upsize whole-downsize range-array whole-3d range-3d whole-cube range-cube alias-array sample-cube render-cube \
            bad-slice bad-level bad-count bad-size bad-format bad-overlap bad-origin; do
    if [ "${3:-}" = --prepare ]; then
        "$out/test-blit-subresources" "$bundle" "$mode" --prepare > "$out/$mode.txt" 2>&1 || result=1
    else
        if [[ "$mode" != bad-* ]]; then
            "$out/test-blit-subresources" --reference "$mode" > "$out/reference-$mode.txt" 2>&1 || result=1
            grep -E '^BLIT_SUBRESOURCES_(EXECUTION_PASS|FAIL)' "$out/reference-$mode.txt" || result=1
        fi
        "$out/test-blit-subresources" "$bundle" "$mode" > "$out/$mode.txt" 2>&1 || result=1
    fi
    grep -E '^BLIT_SUBRESOURCES_(EXECUTION_PASS|AUDIT_PASS|REJECTION_PASS|FAIL)' "$out/$mode.txt" || result=1
done
exit "$result"
