#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: sudo bash test-cold-pipelines.sh VM_TEST_BUNDLE OUTPUT_DIR [CASE [NATIVE_TRANSLATE]]\nRuns isolated cold-cache first-result, timeout-retry and concurrent compilation tests through MoltenVK in macOS. Daemon cases also require the shipping native-translate executable. No native GPU transport coverage.\n'
    exit 0
fi
[ "$#" -ge 2 ] && [ "$#" -le 4 ] || exit 2
[ "$(uname -s)" = Darwin ] && [ "$(id -u)" -eq 0 ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
base=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
bundle="$out/GarnetMetal-cold-test.bundle"
[ ! -e "$bundle" ] || { printf 'Use a fresh output directory.\n' >&2; exit 2; }
ditto "$base" "$bundle"
resources="$bundle/Contents/Resources"
cp "$resources/libgarnetxlate.dylib" "$out/libgarnetxlate-real.dylib"
clang -arch x86_64 -std=c11 -Wall -Wextra -Werror -dynamiclib \
    -install_name @rpath/libgarnetxlate.dylib "$source_dir/test-cold-translator.c" \
    -o "$resources/libgarnetxlate.dylib"
codesign --force --sign - "$resources/libgarnetxlate.dylib"
codesign --force --sign - "$bundle"
clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
    -L "$resources" -lgarnetxlate -Wl,-rpath,"$resources" \
    "$source_dir/test-cold-pipelines.m" -o "$out/test-cold-pipelines"
cases=(sync-compute sync-render async-function async-function-reflection async-descriptor async-descriptor-reflection async-render async-render-reflection refuse-compute refuse-render refuse-async-function refuse-async-function-reflection refuse-async-descriptor refuse-async-descriptor-reflection refuse-async-render refuse-async-render-reflection timeout-compute timeout-render concurrent-compute)
if [ "$#" -ge 3 ]; then cases=("$3"); fi
failures=0
for mode in "${cases[@]}"; do
    command=("$out/test-cold-pipelines" "$bundle" "$out/libgarnetxlate-real.dylib" "$mode")
    if [ "$#" -eq 4 ]; then command+=("$4"); fi
    if "${command[@]}" > "$out/$mode.txt" 2>&1; then
        printf 'COLD_CASE_PASS %s\n' "$mode"
    else
        rc=$?
        printf 'COLD_CASE_FAIL %s exit=%s log=%s\n' "$mode" "$rc" "$out/$mode.txt"
        failures=$((failures + 1))
    fi
done
printf 'COLD_SUITE cases=%s failures=%s native_hardware_transport=untested\n' "${#cases[@]}" "$failures"
[ "$failures" -eq 0 ]
