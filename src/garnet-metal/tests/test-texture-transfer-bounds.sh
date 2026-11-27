#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-texture-transfer-bounds.sh VULKAN_INCLUDE OUTPUT_DIR\nChecks production texture CPU/backing validation with guarded CPU memory and simulated Vulkan; no GPU work.\n'
    exit 0
fi
[ "$#" -eq 2 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$2"
logs=$(cd "$2" && pwd)
clang -arch x86_64 -fobjc-arc -O1 -g -Wall -Wextra -Werror -I "$1" \
    -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    "$source_dir/test-texture-transfer-bounds.m" -o "$logs/test-texture-transfer-bounds" \
    > "$logs/build.log" 2>&1
failures=0
cases=0
ulimit -c 0
for mode in buffer-offset-wrap buffer-row-wrap buffer-width-wrap buffer-vkoffset-wrap \
    buffer-backend-limit plan-zero plan-truncated-width plan-truncated-levels \
    plan-format-width plan-format-height host-replace-row-wrap host-get-row-wrap \
    staging-replace-row-wrap staging-get-row-wrap staging-replace-image-wrap staging-get-image-wrap \
    buffer-final-row-control plan-format-control host-transfer-control host-single-row-control; do
    cases=$((cases + 1))
    if "$logs/test-texture-transfer-bounds" "$mode" > "$logs/$mode.log" 2>&1; then
        printf 'TEXTURE_TRANSFER_BOUNDS_CASE_PASS %s\n' "$mode"
    else
        result=$?
        failures=$((failures + 1))
        printf 'TEXTURE_TRANSFER_BOUNDS_CASE_FAIL %s exit=%s\n' "$mode" "$result"
    fi
done
printf 'TEXTURE_TRANSFER_BOUNDS_SUITE cases=%s failures=%s gpu_execution=0\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
