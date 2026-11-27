#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: sudo bash test-indirect-compute.sh BUNDLE OUTPUT_DIR [--prepare VULKAN_INCLUDE]\nTests indirect Metal compute with private and GPU-generated arguments; --prepare audits real RADV calls with mock BOs, without submission.\n'
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
        -DRDNA4_INDIRECT_COMPUTE_AUDIT -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
        "$source_dir/test-indirect-compute.m" -o "$out/test-indirect-compute"
else
    clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
        "$source_dir/test-indirect-compute.m" -o "$out/test-indirect-compute"
fi
result=0
for mode in grid generated zero bad-offset bad-size bad-local; do
    if [ "${3:-}" = --prepare ]; then
        "$out/test-indirect-compute" "$bundle" "$mode" --prepare > "$out/$mode.txt" 2>&1 || result=1
        grep -E '^INDIRECT_COMPUTE_(AUDIT_PASS|FAIL)' "$out/$mode.txt" || result=1
    else
        if [ "$mode" = grid ] || [ "$mode" = generated ] || [ "$mode" = zero ]; then
            "$out/test-indirect-compute" --reference "$mode" > "$out/reference-$mode.txt" 2>&1 || result=1
            grep -E '^INDIRECT_COMPUTE_(EXECUTION_PASS|FAIL)' "$out/reference-$mode.txt" || result=1
        fi
        "$out/test-indirect-compute" "$bundle" "$mode" > "$out/$mode.txt" 2>&1 || result=1
        grep -E '^INDIRECT_COMPUTE_(EXECUTION_PASS|REJECTION_PASS|FAIL)' "$out/$mode.txt" || result=1
    fi
done
exit "$result"
