#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-heap-execution.sh VULKAN_INCLUDE OUTPUT_DIR\n'
    printf 'Runs production RDNA4Heap execution with simulated Vulkan, no GPU execution.\n'
    exit 0
fi
[ "$#" -eq 2 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
clang -arch x86_64 -fobjc-arc -O1 -Wall -Wextra -Werror -I "$1" -I "$source_dir" \
    -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    "$source_dir/test-heap-execution.m" -o "$out/test-heap-execution"
"$out/test-heap-execution" > "$out/result.log" 2>&1
grep '^HEAP_EXECUTION_' "$out/result.log"
