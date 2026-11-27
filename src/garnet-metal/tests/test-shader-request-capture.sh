#!/bin/bash
set -euo pipefail
[ "$#" -eq 1 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$1"
output=$(cd "$1" && pwd)
{ echo '#define GARNET_INTERNAL static'; awk '/^GARNET_INTERNAL int rdna4_dump_write\(/ {printing=1} printing {print} printing && /^}/ {exit}' \
    "$source_dir/garnet_shaders.m"; } > "$output/dump-write.inc"
clang -std=gnu11 -fobjc-arc -Wall -Wextra -Werror -I "$output" -I "$source_dir" \
    -framework Foundation "$source_dir/test-shader-request-capture.m" -o "$output/test-shader-request-capture"
"$output/test-shader-request-capture" "$output"
