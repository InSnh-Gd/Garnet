#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-icb-transactions.sh OUTPUT_DIR\n'
    printf 'Runs the real CPU ICB reset/copy transaction implementation with a manual action stub.\n'
    printf 'No GPU submission is made; this does not claim executeCommandsInBuffer completion.\n'
    exit 0
fi
[ "$#" -eq 1 ] || { printf 'Expected OUTPUT_DIR; use --help.\n' >&2; exit 2; }
[ "$(uname -s)" = Darwin ] || { printf 'Requires macOS.\n' >&2; exit 2; }
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$1"
out=$(cd "$1" && pwd)
clang -arch x86_64 -fobjc-arc -O1 -Wall -Wextra -Werror \
    -I "$source_dir" -framework Foundation -framework Metal \
    "$source_dir/test-icb-transactions.m" \
    -o "$out/test-icb-transactions"
"$out/test-icb-transactions" > "$out/run.log" 2>&1 || {
    cat "$out/run.log" >&2
    exit 1
}
grep '^ICB_TRANSACTION_' "$out/run.log"
printf 'ICB_TRANSACTION_NOTE manual action stub; no GPU submission\n'
