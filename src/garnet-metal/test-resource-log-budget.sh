#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-resource-log-budget.sh VULKAN_INCLUDE OUTPUT_DIR\nChecks production resource creation log volume; simulated Vulkan, no GPU work.\n'
    exit 0
fi
[ "$#" -eq 2 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$2"
logs=$(cd "$2" && pwd)
clang -arch x86_64 -fobjc-arc -O1 -Wall -Wextra -Werror -I "$1" \
    -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    "$source_dir/test-resource-log-budget.m" -o "$logs/test-resource-log-budget" \
    > "$logs/build.log" 2>&1
"$logs/test-resource-log-budget" > "$logs/result.log" 2> "$logs/stderr.log"
normal=$(grep -Ec 'newBufferWithLength:256 options:|RDNA4Buffer .*: 256 bytes' "$logs/stderr.log" || true)
errors=$(grep -c 'newBuffer: refused length' "$logs/stderr.log" || true)
cat "$logs/result.log"
maximum=${RDNA4_LOG_BUDGET_MAX:-32}
printf 'RESOURCE_LOG_VOLUME normal=%s max=%s errors=%s expected_errors=32\n' "$normal" "$maximum" "$errors"
[ "$normal" -gt 0 ] && [ "$normal" -le "$maximum" ] && [ "$errors" -eq 32 ]
