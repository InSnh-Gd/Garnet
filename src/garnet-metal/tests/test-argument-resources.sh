#!/usr/bin/env bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-argument-resources.sh OUTPUT_DIR BUNDLE|--reference [--encode-only]\nDefault executes a four-quadrant texture sampling kernel. Captures AIR, MSL, layout, encoded bytes and exact GPU results.\n'
    exit 0
fi
[ "$#" -ge 2 ] && [ "$#" -le 3 ] || exit 2
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1"
out=$(cd -- "$1" && pwd)
args=("$2" "$out")
if [ "$#" -eq 3 ]; then
    [ "$3" = --encode-only ] || exit 2
    args+=(--encode-only)
fi
shasum -a 256 "$test_dir/test-argument-resources.m" > "$out/fixture.sha256"
clang -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
    "$test_dir/test-argument-resources.m" -o "$out/test-argument-resources" 2>&1 | tee "$out/build.log"
printf 'ARGUMENT_RESOURCES_BUILD_PASS %s\n' "$out/test-argument-resources" | tee -a "$out/build.log"
"$out/test-argument-resources" "${args[@]}" 2>&1 | tee "$out/result.log"
