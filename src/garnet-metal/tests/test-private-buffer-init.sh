#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-private-buffer-init.sh VULKAN_INCLUDE OUTPUT_DIR\nChecks production large Private initialization with simulated Vulkan.\n'
    exit 0
fi
[ "$#" -eq 2 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
clang -arch x86_64 -fobjc-arc -O1 -g -Wall -Wextra -Werror -I "$1" -I "$source_dir" \
    -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    "$source_dir/test-private-buffer-init.m" -o "$out/test-private-buffer-init"
"$out/test-private-buffer-init" > "$out/result.log" 2>&1
grep '^PRIVATE_INIT_' "$out/result.log"
"$out/test-private-buffer-init" --help > "$out/help.log" 2>&1
if "$out/test-private-buffer-init" invalid > "$out/invalid.log" 2>&1; then exit 1
else [ "$?" -eq 2 ]; fi
