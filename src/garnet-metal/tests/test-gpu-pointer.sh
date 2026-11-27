#!/usr/bin/env bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-gpu-pointer.sh OUTPUT_DIR BUNDLE|--reference [--pipeline-only]\nDefault executes GPU writes. Use --pipeline-only for compilation without dispatch.\n'
    exit 0
fi
[ "$#" -ge 2 ] && [ "$#" -le 3 ] || exit 2
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1"
out=$(cd -- "$1" && pwd)
shift
shasum -a 256 "$test_dir/test-gpu-pointer.m" > "$out/fixture.sha256"
clang -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
    "$test_dir/test-gpu-pointer.m" -o "$out/test-gpu-pointer" 2>&1 | tee "$out/build.log"
printf 'GPU_POINTER_BUILD_PASS %s\n' "$out/test-gpu-pointer" | tee -a "$out/build.log"
GPU_POINTER_SOURCE="$out/gpu-pointer.metal" "$out/test-gpu-pointer" "$@" 2>&1 | tee "$out/result.log"
