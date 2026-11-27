#!/bin/bash
set -euo pipefail

if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-heap-gpu.sh OUTPUT_DIR\n'
    printf 'Builds the real-GPU automatic-heap test.\n'
    printf 'Run: OUTPUT_DIR/test-heap-gpu BUNDLE  (candidate semantics)\n'
    printf '     OUTPUT_DIR/test-heap-gpu --reference  (system-device public semantics)\n'
    exit 0
fi

[ "$#" -eq 1 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$1"
out=$(cd "$1" && pwd)
binary="$out/test-heap-gpu"

shasum -a 256 "$source_dir/test-heap-gpu.m" > "$out/fixture.sha256"
clang -arch x86_64 -fobjc-arc -fblocks -O1 -g -Wall -Wextra -Werror \
    -framework Foundation -framework Metal \
    "$source_dir/test-heap-gpu.m" -o "$binary" > "$out/build.log" 2>&1

"$binary" --help > "$out/help.log" 2>&1
grep -F 'Usage: test-heap-gpu BUNDLE|--reference' "$out/help.log"
if "$binary" invalid extra >/dev/null 2>&1; then
    exit 1
else
    [ "$?" -eq 2 ]
fi

printf 'BUILD_PASS %s\n' "$binary"
