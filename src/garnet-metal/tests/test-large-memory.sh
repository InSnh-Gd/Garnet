#!/bin/bash
set -euo pipefail

if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-large-memory.sh OUTPUT_DIR\nBuilds test-large-memory for explicit VM/live invocation. This script never runs a GPU test.\n'
    exit 0
fi

[ "$#" -eq 1 ] && [ -n "$1" ] || { printf 'Expected OUTPUT_DIR\n' >&2; exit 2; }
[ "$(uname -s)" = Darwin ] || { printf 'Run on macOS.\n' >&2; exit 2; }

source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$1"
out=$(cd "$1" && pwd)
binary="$out/test-large-memory"

clang -arch x86_64 -fobjc-arc -O1 -g -Wall -Wextra -Werror \
    -framework Foundation -framework Metal \
    "$source_dir/test-large-memory.m" -o "$binary"

"$binary" --help

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
expect_usage_exit /nonexistent/GarnetMetal.bundle invalid-mode
expect_usage_exit /nonexistent/GarnetMetal.bundle small 0
expect_usage_exit /nonexistent/GarnetMetal.bundle small 17
expect_usage_exit /nonexistent/GarnetMetal.bundle small malformed
expect_usage_exit /nonexistent/GarnetMetal.bundle small 1 extra

printf 'test-large-memory binary: %s\n' "$binary"
