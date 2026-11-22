#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-blit-buffer-layout.sh VULKAN_INCLUDE OUTPUT_DIR\nChecks production buffer/texture blit validation with simulated Vulkan; no GPU work.\n'
    exit 0
fi
[ "$#" -eq 2 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$2"
logs=$(cd "$2" && pwd)
clang -arch x86_64 -fobjc-arc -O1 -g -Wall -Wextra -Werror -I "$1" \
    -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    "$source_dir/test-blit-buffer-layout.m" -o "$logs/test-blit-buffer-layout" \
    > "$logs/build.log" 2>&1
"$logs/test-blit-buffer-layout" > "$logs/result.log" 2>&1
grep '^BLIT_BUFFER_LAYOUT' "$logs/result.log"
