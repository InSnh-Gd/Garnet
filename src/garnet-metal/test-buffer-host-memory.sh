#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-buffer-host-memory.sh VULKAN_INCLUDE OUTPUT_DIR\nChecks production Metal host-memory initialization with simulated Vulkan and real CPU VA.\n'
    exit 0
fi
[ "$#" -eq 2 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
clang -arch x86_64 -fobjc-arc -O1 -g -Wall -Wextra -Werror -I "$1" -I "$source_dir" \
    -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    "$source_dir/test-buffer-host-memory.m" -o "$out/test-buffer-host-memory"
"$out/test-buffer-host-memory" > "$out/result.log" 2>&1
grep '^BUFFER_HOST_MEMORY_' "$out/result.log"
"$out/test-buffer-host-memory" --help > "$out/help.log" 2>&1
if "$out/test-buffer-host-memory" invalid > "$out/invalid.log" 2>&1; then exit 1
else [ "$?" -eq 2 ]; fi
