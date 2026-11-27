#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-fence-execution.sh OUTPUT_DIR\n'
    printf 'Builds only; run test-fence-execution BUNDLE a or BUNDLE b on the VM GPU.\n'
    exit 0
fi
[ "$#" -eq 1 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$1"
out=$(cd "$1" && pwd)
clang -arch x86_64 -fobjc-arc -O1 -Wall -Wextra -Werror \
    -framework Foundation -framework Metal -framework IOSurface \
    "$source_dir/test-fence-execution.m" -o "$out/test-fence-execution"
