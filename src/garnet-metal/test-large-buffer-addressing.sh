#!/bin/bash
set -euo pipefail

if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-large-buffer-addressing.sh OUTPUT_DIR\n'
    printf 'Builds the live 64-bit buffer-addressing test. This script never runs GPU work.\n'
    printf 'Run: OUTPUT_DIR/test-large-buffer-addressing BUNDLE|--reference\n'
    exit 0
fi

if [ "$#" -ne 1 ] || [ -z "$1" ]; then
    printf 'Expected OUTPUT_DIR\n' >&2
    exit 2
fi
if [ "$(uname -s)" != Darwin ]; then
    printf 'Run on macOS.\n' >&2
    exit 2
fi

source_dir=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1"
out=$(cd -- "$1" && pwd)
binary="$out/test-large-buffer-addressing"

shasum -a 256 "$source_dir/test-large-buffer-addressing.m" \
    "$source_dir/test-large-buffer-addressing.sh" > "$out/fixture.sha256"

clang -arch x86_64 -fobjc-arc -fblocks -O1 -g -Wall -Wextra -Werror \
    -framework Foundation -framework Metal \
    "$source_dir/test-large-buffer-addressing.m" -o "$binary" 2>&1 | tee "$out/build.log"

printf 'BUILD_PASS %s\n' "$binary" | tee -a "$out/build.log"
