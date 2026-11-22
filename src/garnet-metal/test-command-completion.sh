#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-command-completion.sh VULKAN_INCLUDE BUNDLE OUTPUT_DIR\nTests real Metal completion and lifetime with simulated Vulkan submission/fences in a macOS VM.\n'
    exit 0
fi
[ "$#" -eq 3 ] || { printf 'Expected three arguments; use --help.\n' >&2; exit 2; }
[ "$(uname -s)" = Darwin ] || { printf 'Requires a macOS VM.\n' >&2; exit 2; }
include=$(cd "$1" && pwd)
bundle=$(cd "$2" && pwd)
mkdir -p "$3"
out=$(cd "$3" && pwd)
source_dir=$(cd "$(dirname "$0")" && pwd)
common=(-arch x86_64 -fobjc-arc -Wall -Wextra -Werror -I "$include" -framework Foundation)
clang "${common[@]}" -DRDNA4_FEATURE_AUDIT -dynamiclib \
    -install_name @rpath/librdna4featureaudit.dylib "$source_dir/test-system-pipelines.m" \
    -o "$out/librdna4featureaudit.dylib"
clang "${common[@]}" -framework Metal -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
    "$source_dir/test-command-completion.m" -o "$out/test-command-completion"
result=0
modes=(reject submit-error fence-error complete wait-submitted wait-frame-submitted surface-schedule lost timeout early-timeout early-hung wait-error)
for mode in "${modes[@]}"; do
    "$out/test-command-completion" "$bundle" "$mode" > "$out/$mode.txt" 2>&1 || result=1
    grep -E '^(COMPLETION_|FAULT_GUARD_|FAIL)' "$out/$mode.txt" || { cat "$out/$mode.txt" >&2; result=1; }
done
cp "$out/test-command-completion" "$out/WindowServer"
for mode in wait-submitted wait-frame-submitted; do
    "$out/WindowServer" "$bundle" "$mode" > "$out/WindowServer-$mode.txt" 2>&1 || result=1
    grep -E '^(COMPLETION_|FAIL)' "$out/WindowServer-$mode.txt" || { cat "$out/WindowServer-$mode.txt" >&2; result=1; }
done
for operation in replace get; do
    for mode in reject submit-error fence-error complete lost timeout wait-error; do
        "$out/test-command-completion" "$bundle" "$mode" "$operation" > "$out/$operation-$mode.txt" 2>&1 || result=1
        grep -E '^(ONESHOT_|FAIL)' "$out/$operation-$mode.txt" || { cat "$out/$operation-$mode.txt" >&2; result=1; }
    done
done
exit "$result"
