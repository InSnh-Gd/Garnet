#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-core-image.sh VULKAN_INCLUDE BUNDLE OUTPUT_DIR\nTests Core Image compilation and encoding through the native bundle in a macOS VM. Requires root. No GPU submissions or pixel validation.\n'
    exit 0
fi
[ "$#" -eq 3 ] || { printf 'Expected three arguments; use --help.\n' >&2; exit 2; }
[ "$(uname -s)" = Darwin ] || { printf 'Requires a macOS VM.\n' >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || { printf 'Requires root for the generic native-device test.\n' >&2; exit 2; }
include=$(cd "$1" && pwd)
bundle=$(cd "$2" && pwd)
mkdir -p "$3"
out=$(cd "$3" && pwd)
source_dir=$(cd "$(dirname "$0")" && pwd)
common=(-arch x86_64 -fobjc-arc -Wall -Wextra -Werror -I "$include" -framework Foundation)
clang "${common[@]}" -DRDNA4_FEATURE_AUDIT -dynamiclib \
    -install_name @rpath/librdna4featureaudit.dylib "$source_dir/test-system-pipelines.m" \
    -o "$out/librdna4featureaudit.dylib"
clang "${common[@]}" -framework Metal -framework CoreImage -framework CoreGraphics \
    -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
    "$source_dir/test-core-image.m" -o "$out/test-core-image"
"$out/test-core-image" --help
if "$out/test-core-image" "$bundle" invalid early; then
    printf 'Invalid filter was accepted.\n' >&2
    exit 1
else
    [ "$?" -eq 2 ] || exit 1
fi
result=0
for filter in builtin cikl blur transform composite; do
    for load in early late; do
        log="$out/$filter-$load.txt"
        if "$out/test-core-image" "$bundle" "$filter" "$load" > "$log" 2>&1; then
            grep -E '^CORE_IMAGE_(PREPARE_PASS|ENCODING_PASS)' "$log"
        else
            cat "$log" >&2
            result=1
        fi
    done
done
exit "$result"
