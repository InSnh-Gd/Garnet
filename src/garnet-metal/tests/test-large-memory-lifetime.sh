#!/bin/bash
set -euo pipefail

if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-large-memory-lifetime.sh VULKAN_INCLUDE OUTPUT_DIR\n'
    printf 'Runs production Metal resource lifetime code with simulated Vulkan boundaries.\n'
    printf 'No real large backing, GPU execution, or kernel page/handle release is tested.\n'
    exit 0
fi

[ "$#" -eq 2 ] && [ -n "$1" ] && [ -n "$2" ] || {
    printf 'Expected VULKAN_INCLUDE OUTPUT_DIR; use --help.\n' >&2
    exit 2
}
[ "$(uname -s)" = Darwin ] || {
    printf 'Requires macOS.\n' >&2
    exit 2
}

include=$(cd "$1" && pwd)
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
binary="$out/test-large-memory-lifetime"

clang -arch x86_64 -fobjc-arc -O1 -g -Wall -Wextra -Werror \
    -I "$include" -I "$source_dir" \
    -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    "$source_dir/test-large-memory-lifetime.m" -o "$binary" \
    > "$out/build.log" 2>&1

"$binary" --help > "$out/help.log" 2>&1
grep '^Usage: test-large-memory-lifetime' "$out/help.log"

expect_usage_exit() {
    local status=0
    "$binary" "$@" >/dev/null 2>&1 || status=$?
    [ "$status" -eq 2 ] || {
        printf 'Expected exit 2 for invalid arguments, got %s\n' "$status" >&2
        exit 1
    }
}

expect_usage_exit
expect_usage_exit --help extra
expect_usage_exit invalid-mode
expect_usage_exit all extra

"$binary" all > "$out/result.log" 2>&1
grep '^LARGE_MEMORY_LIFETIME_' "$out/result.log"
