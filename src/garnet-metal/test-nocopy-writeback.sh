#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-nocopy-writeback.sh VULKAN_INCLUDE OUTPUT_DIR\nRuns real render/write-back/segment methods with protected desktop-sized memory and simulated Vulkan; no GPU work.\n'
    exit 0
fi
[ "$#" -eq 2 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$2"
output=$(cd "$2" && pwd)
clang -arch x86_64 -fobjc-arc -O1 -g -Wall -Wextra -Werror -I "$1" \
    -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    "$source_dir/test-nocopy-writeback.m" -o "$output/test-nocopy-writeback" \
    > "$output/build.log" 2>&1
failures=0
cases=0
ulimit -c 0
for mode in readonly-render readonly-finish readonly-segment readonly-host-action writable-segment linear-segment trace-encode-drop trace-pre-submit-drop icon-tight icon-padded icon-partial-clear icon-full-clear a8-abandoned-upload a8-abandoned-sample menu-abandoned-command menu-init-retry menu-shared-scratch blit-full-buffer blit-full-texture blit-partial blit-incomplete-source blit-earlier-read scanout-offset-full scanout-offset-partial scanout-offset-overflow; do
    cases=$((cases + 1))
    if "$output/test-nocopy-writeback" "$mode" > "$output/$mode.log" 2>&1; then
        printf 'NOCOPY_WRITEBACK_CASE_PASS %s\n' "$mode"
    else
        result=$?
        failures=$((failures + 1))
        printf 'NOCOPY_WRITEBACK_CASE_FAIL %s exit=%s\n' "$mode" "$result"
        cat "$output/$mode.log"
    fi
done
printf 'NOCOPY_WRITEBACK_SUITE cases=%s failures=%s gpu_execution=0\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
