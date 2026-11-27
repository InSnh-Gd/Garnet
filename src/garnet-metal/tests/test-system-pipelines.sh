#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-system-pipelines.sh [--source-library] VULKAN_INCLUDE BUNDLE AIR_CORPUS OUTPUT_DIR\nRuns shipped compute pipeline creation and shader-feature checks in a macOS VM. No GPU submissions.\n--source-library also tests runtime source compilation, reflection and encoding; requires root.\n'
    exit 0
fi
source_test=0
if [ "${1:-}" = --source-library ]; then
    source_test=1
    shift
    [ "$(id -u)" -eq 0 ] || { printf 'The source-library test requires root.\n' >&2; exit 2; }
fi
[ "$#" -eq 4 ] || { printf 'Expected four arguments; use --help.\n' >&2; exit 2; }
[ "$(uname -s)" = Darwin ] || { printf 'Requires a macOS VM.\n' >&2; exit 2; }
include=$(cd "$1" && pwd)
bundle=$(cd "$2" && pwd)
corpus=$(cd "$3" && pwd)
mkdir -p "$4"
out=$(cd "$4" && pwd)
source_dir=$(cd "$(dirname "$0")" && pwd)
common=(-arch x86_64 -fobjc-arc -Wall -Wextra -Werror -I "$include" -framework Foundation)
clang "${common[@]}" -DRDNA4_FEATURE_AUDIT -dynamiclib \
    -install_name @rpath/librdna4featureaudit.dylib "$source_dir/test-system-pipelines.m" \
    -o "$out/librdna4featureaudit.dylib"
clang "${common[@]}" -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
    "$source_dir/test-system-pipelines.m" -o "$out/test-system-pipelines"
"$out/test-system-pipelines" "$bundle" "$corpus"
if [ "$source_test" -eq 1 ]; then
    clang "${common[@]}" -framework Metal -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
        "$source_dir/test-library-source.m" -o "$out/test-library-source"
    "$out/test-library-source" "$bundle"
    clang "${common[@]}" -framework Metal -L "$out" -lrdna4featureaudit -Wl,-rpath,@executable_path \
        "$source_dir/test-pipeline-reflection.m" -o "$out/test-pipeline-reflection"
    "$out/test-pipeline-reflection" --reference
    "$out/test-pipeline-reflection" "$bundle"
fi
