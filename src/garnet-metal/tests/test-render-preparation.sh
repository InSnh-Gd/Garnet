#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: sudo bash test-render-preparation.sh VULKAN_INCLUDE RADV_BUNDLE OUTPUT_DIR\nChecks real system render pipeline compilation and command encoding with mock BOs. No GPU execution or pixel validation.\n'
    exit 0
fi
[ "$#" -eq 3 ] || { printf 'Expected three arguments; use --help.\n' >&2; exit 2; }
[ "$(uname -s)" = Darwin ] && [ "$(id -u)" -eq 0 ] || { printf 'Run as root inside macOS.\n' >&2; exit 2; }
include=$(cd "$1" && pwd)
bundle=$(cd "$2" && pwd)
mkdir -p "$3"
out=$(cd "$3" && pwd)
source_dir=$(cd "$(dirname "$0")" && pwd)
clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -I "$include" -framework Foundation \
    -DRDNA4_FEATURE_AUDIT -dynamiclib -install_name @rpath/librdna4featureaudit.dylib \
    "$source_dir/test-system-pipelines.m" -o "$out/librdna4featureaudit.dylib"
clang -arch x86_64 -fobjc-arc -O1 \
    -framework Metal -framework Foundation -framework IOKit -framework CoreGraphics -framework ImageIO -framework IOSurface \
    -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
    "$source_dir/test-render-preparation.m" -o "$out/test-render-preparation"
"$out/test-render-preparation" --help
clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -DRDNA4_VIEW_AUDIT -framework Foundation -framework Metal -framework IOSurface \
    -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
    "$source_dir/test-texture-view.m" -o "$out/test-texture-view"
if "$out/test-render-preparation" "$bundle" invalid; then
    printf 'Invalid render command was accepted.\n' >&2
    exit 1
else
    [ "$?" -eq 2 ] || exit 1
fi
failures=0
for command in sysdraw skydraw fbsover fbcopy fbfetch depth msaa; do
    log="$out/$command.txt"
    if "$out/test-render-preparation" "$bundle" "$command" > "$log" 2>&1; then
        grep '^RENDER_PREPARATION_PASS' "$log"
    else
        cat "$log" >&2
        failures=$((failures + 1))
    fi
done
if "$out/test-texture-view" "$bundle" > "$out/texture-view.txt" 2>&1; then
    grep -E '^TEXTURE_(MIP_|HOST_|LAYERED_)?VIEW_AUDIT_PASS' "$out/texture-view.txt"
else
    cat "$out/texture-view.txt" >&2
    failures=$((failures + 1))
fi
printf 'RENDER_PREPARATION_SUITE cases=8 failures=%s gpu_execution=untested\n' "$failures"
[ "$failures" -eq 0 ]
