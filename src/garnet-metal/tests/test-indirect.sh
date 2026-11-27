#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: sudo bash test-indirect.sh BUNDLE OUTPUT_DIR [--prepare VULKAN_INCLUDE]\nTests indirect Metal draws with private GPU argument buffers; --prepare audits real RADV calls with mock BOs, without submission.\n'
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
        -DRDNA4_INDIRECT_AUDIT -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
        "$source_dir/test-indirect.m" -o "$out/test-indirect"
else
    clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
        "$source_dir/test-indirect.m" -o "$out/test-indirect"
fi
result=0
for mode in draw indexed multi zero bad-offset bad-size bad-index; do
    if [ "${3:-}" = --prepare ]; then
        "$out/test-indirect" "$bundle" "$mode" --prepare > "$out/$mode.txt" 2>&1 || result=1
    else
        if [ "$mode" = draw ] || [ "$mode" = indexed ] || [ "$mode" = multi ] || [ "$mode" = zero ]; then
            "$out/test-indirect" --reference "$mode" > "$out/reference-$mode.txt" 2>&1 || result=1
            grep -E '^INDIRECT_(EXECUTION_PASS|FAIL)' "$out/reference-$mode.txt" || result=1
        fi
        "$out/test-indirect" "$bundle" "$mode" > "$out/$mode.txt" 2>&1 || result=1
    fi
    grep -E '^INDIRECT_(EXECUTION_PASS|AUDIT_PASS|PREPARE_PASS|REJECTION_PASS|FAIL)' "$out/$mode.txt" || result=1
done
exit "$result"
