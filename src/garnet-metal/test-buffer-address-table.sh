#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-buffer-address-table.sh VULKAN_INCLUDE OUTPUT_DIR\nChecks production address-table metadata with simulated Vulkan.\n'
    exit 0
fi
[ "$#" -eq 2 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
clang -arch x86_64 -fobjc-arc -O1 -Wall -Wextra -Werror -I "$1" -I "$source_dir" \
    -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    "$source_dir/test-buffer-address-table.m" -o "$out/test-buffer-address-table"
"$out/test-buffer-address-table" > "$out/result.log" 2>&1
grep '^BUFFER_ADDRESS_TABLE_' "$out/result.log"
