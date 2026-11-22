#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: sudo bash test-pipeline-oom.sh VULKAN_INCLUDE BUNDLE RADV OUTPUT_DIR\nTests native visible-VRAM exhaustion, pipeline refusal and recovery without GPU submission.\n'
    exit 0
fi
[ "$#" -eq 4 ] || { printf 'Expected four arguments; use --help.\n' >&2; exit 2; }
[ "$(uname -s)" = Darwin ] && [ "$(id -u)" -eq 0 ] || { printf 'Requires root on native macOS.\n' >&2; exit 2; }
include=$(cd "$1" && pwd)
bundle=$(cd "$2" && pwd)
radv=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
mkdir -p "$4"
out=$(cd "$4" && pwd)
source_dir=$(cd "$(dirname "$0")" && pwd)
cp "$radv" "$out/libvulkan_radeon.dylib"
clang -arch x86_64 -fobjc-arc -O1 -g -Wno-deprecated-declarations -Wall -Wextra -Werror \
    -I "$source_dir" -I "$include" -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    "$source_dir/test-pipeline-oom.m" -o "$out/test-pipeline-oom"
shasum -a 256 "$out/libvulkan_radeon.dylib" "$out/test-pipeline-oom"
for mode in cached nocache; do
    cache=$(mktemp -d "$out/$mode.XXXXXX")
    if [ "$mode" = nocache ]; then
        RADV_DEBUG=nocache "$out/test-pipeline-oom" "$bundle" "$cache" > "$out/$mode.log" 2>&1
    else
        "$out/test-pipeline-oom" "$bundle" "$cache" > "$out/$mode.log" 2>&1
    fi
    grep -E '^(PRESSURE|PASS|FAIL)' "$out/$mode.log"
done
