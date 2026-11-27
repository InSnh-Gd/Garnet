#!/usr/bin/env bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-argument-encoder.sh OUTPUT_DIR BUNDLE|--reference [--encode-only]\nDefault dispatches GPU writes; --encode-only tests source compilation and argument encoding. Captures AIR, MSL, runtime layout and buffer bytes.\n'
    exit 0
fi
[ "$#" -ge 2 ] && [ "$#" -le 3 ] || exit 2
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1"
out=$(cd -- "$1" && pwd)
target=$2
args=("$target" "$out")
if [ "$#" -eq 3 ]; then
    [ "$3" = --encode-only ] || exit 2
    args+=(--encode-only)
fi
shasum -a 256 "$test_dir/test-argument-encoder.m" > "$out/fixture.sha256"
clang -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
    "$test_dir/test-argument-encoder.m" -o "$out/test-argument-encoder" 2>&1 | tee "$out/build.log"
printf 'ARGUMENT_ENCODER_BUILD_PASS %s\n' "$out/test-argument-encoder" | tee -a "$out/build.log"
"$out/test-argument-encoder" "${args[@]}" 2>&1 | tee "$out/result.log"
