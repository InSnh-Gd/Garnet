#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-command-event-execution.sh OUTPUT_DIR\nBuilds only; run test-command-event-execution --reference or BUNDLE explicitly.\n'
    exit 0
fi
[ "$#" -eq 1 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$1"
clang -arch x86_64 -fobjc-arc -O1 -Wall -Wextra -Werror -framework Foundation -framework Metal -framework IOSurface \
    "$source_dir/test-command-event-execution.m" -o "$1/test-command-event-execution"
