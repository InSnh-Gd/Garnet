#!/usr/bin/env bash
set -euo pipefail
if [[ ${1:-} == --help ]]; then
    printf 'Usage: bash test-argument-nested.sh OUTPUT_DIR BUNDLE|--reference [--encode-only]\nChecks nested argument-buffer encoding and GPU pointer traversal.\n'
    exit 0
fi
[[ $# -ge 2 && $# -le 3 && $(uname -s) == Darwin ]] || exit 2
[[ $# -eq 2 || $3 == --encode-only ]] || exit 2
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1"
out=$(cd -- "$1" && pwd)
shasum -a 256 "$test_dir/test-argument-nested.m" > "$out/fixture.sha256"
clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal \
    "$test_dir/test-argument-nested.m" -o "$out/test-argument-nested" > "$out/build.log" 2>&1
args=("$2" "$out")
[[ $# -eq 3 ]] && args+=(--encode-only)
"$out/test-argument-nested" "${args[@]}" 2>&1 | tee "$out/result.log"
