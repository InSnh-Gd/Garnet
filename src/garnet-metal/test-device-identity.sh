#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-device-identity.sh VULKAN_INCLUDE OUTPUT_DIR\nRuns the production Metal identity regression with simulated registry and base initialization.\n'
    exit 0
fi
[ "$#" -eq 2 ] && [ "$(uname -s)" = Darwin ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
clang -arch x86_64 -fobjc-arc -O1 -g -Wall -Wextra -Werror -I "$1" -I "$source_dir" \
    -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    "$source_dir/test-device-identity.m" -o "$out/test-device-identity"
"$out/test-device-identity" > "$out/result.log" 2>&1
grep '^DEVICE_\(IDENTITY\|MEMORY\)_' "$out/result.log"
"$out/test-device-identity" --help > "$out/help.log" 2>&1
if "$out/test-device-identity" invalid > "$out/invalid.log" 2>&1; then exit 1
else [ "$?" -eq 2 ]; fi
